/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <utility>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_ducker_policy.h"
#include "xenia/apu/nop_audio_driver.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/guest_io_buffer.h"

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

DEFINE_uint32(apu_max_queued_frames, 8,
              "Allows changing max buffered audio frames to reduce audio "
              "delay. Lowering this value might cause performance issues. "
              "Value range: [4-64]",
              "APU");
UPDATE_from_uint32(apu_max_queued_frames, 2024, 8, 31, 20, 64);

namespace xe {
namespace apu {

namespace {
std::atomic<uint64_t> next_audio_owner_epoch{1};
static_assert(AudioDuckerPolicy::kCategoryCount == 32);
}

struct AudioSystem::ClientRegistration {
  ClientRegistration(AudioClientLifecycle::Identity identity,
                     std::shared_ptr<void> lease, uint32_t callback,
                     uint32_t argument, uint32_t wrapper)
      : lifecycle(identity, std::move(lease)),
        callback(callback),
        callback_arg(argument),
        wrapped_callback_arg(wrapper) {}
  AudioClientLifecycle lifecycle;
  AudioCategoryDelivery category_delivery;
  const uint32_t callback;
  const uint32_t callback_arg;
  const uint32_t wrapped_callback_arg;
  std::shared_ptr<const NativeAudioClientKey> observation_key;
  std::shared_ptr<NativeAudioObservation> observation;
  uint64_t frame_sequence = 0;
  AudioDriver* driver = nullptr;  // clients_mutex_ protects the driver.
  std::atomic<uint32_t> frames_submitted{0};
  std::atomic<uint32_t> frames_processed{0};
  std::atomic<uint32_t> frames_dropped{0};
};

class AudioSystem::ClientOperation {
 public:
  explicit ClientOperation(AudioSystem* owner) : owner_(owner) {
    std::lock_guard lock(owner_->clients_mutex_);
    if (!owner_->shutting_down_ && owner_->client_operations_ != UINT32_MAX) {
      ++owner_->client_operations_;
      active_ = true;
    }
  }
  ~ClientOperation() { Complete(); }
  // Actual host work completes this explicitly: a parked guest fiber may be
  // discarded without destroying its local shared_ptr during scheduler stop.
  void Complete() {
    if (active_.exchange(false, std::memory_order_acq_rel)) {
      std::lock_guard lock(owner_->clients_mutex_);
      if (!--owner_->client_operations_) {
        owner_->client_operations_drained_.notify_all();
      }
    }
  }
  bool active() const { return active_.load(std::memory_order_acquire); }

 private:
  AudioSystem* const owner_;
  std::atomic<bool> active_{false};
};

std::shared_ptr<AudioSystem::ClientOperation>
AudioSystem::BeginClientOperation() {
  auto operation = std::make_shared<ClientOperation>(this);
  return operation->active() ? operation : nullptr;
}

AudioSystem::AudioSystem(cpu::Processor* processor)
    : memory_(processor->memory()),
      processor_(processor),
      worker_running_(false),
      owner_epoch_(next_audio_owner_epoch.fetch_add(1)),
      ducker_policy_(std::make_shared<AudioDuckerPolicy>(
          AudioDuckerPolicy::SteadyNowNs, owner_epoch_)) {
  queued_frames_ = std::clamp(cvars::apu_max_queued_frames,
                              static_cast<uint32_t>(kMinimumQueuedFrames),
                              static_cast<uint32_t>(kMaximumQueuedFrames));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = xe::threading::Semaphore::Create(0, queued_frames_);
  }
  pending_work_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(pending_work_event_);

  xma_decoder_ = std::make_unique<xe::apu::XmaDecoder>(processor_);

  resume_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  ducker_policy_->Stop();
  StopIndependentAudio();
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(kernel::KernelState* kernel_state) {
  kernel_state_ = kernel_state;
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state->GetSystemProcess()));
  // As we run audio callbacks the debugger must be able to suspend us.
  worker_thread_->set_can_debugger_suspend(true);
  worker_thread_->set_name("Audio Worker");
  result = worker_thread_->Create();
  if (XFAILED(result)) {
    worker_running_ = false;
    worker_thread_.reset();
    return result;
  }
  // SYSTEM XAudio allocates its mixer buffers for reserved logical CPU4.
  // Select it before Setup returns and render clients can be admitted. Keep
  // default creation first so other threads' fallback CPU sequence is
  // unchanged. XHostThread affinity updates the guest PCR without pinning a
  // host OS CPU.
  worker_thread_->SetAffinity(0x10);
  XELOGI(
      "APU: SYSTEM callback worker guest_cpu={} process_type={} owner_epoch={}",
      worker_thread_->active_cpu(),
      uint32_t(worker_thread_->guest_object<kernel::X_KTHREAD>()->process_type),
      owner_epoch_);
  // Set high priority for this thread for better pacing.
  worker_thread_->SetPriority(24);
  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  // The host mixer releases a client's semaphore on its own coarse cadence,
  // but Xenos audio subsystem operates at 5.333ms interval (see
  // xaudio2_audio_driver.cc) Interval scales inversely with guest_time_scalar.
  // We therefore pace pumps to each client's next_pump_us deadline and use
  // the semaphore only as back-pressure: a frame is submitted only if
  // a host output slot is free, otherwise it is dropped (the host queue
  // is full, so it is already well buffered).
  while (worker_running_) {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    size_t client_index = kMaximumClientCount;
    std::shared_ptr<ClientRegistration> registration;
    uint64_t earliest_pump_us = std::numeric_limits<uint64_t>::max();
    {
      std::lock_guard<std::mutex> clients_lock(clients_mutex_);

      for (size_t i = 0; i < kMaximumClientCount; ++i) {
        if (!clients_[i].registration ||
            clients_[i].registration->lifecycle.phase() !=
                AudioClientLifecycle::Phase::kActive ||
            clients_[i].next_pump_us >= earliest_pump_us) {
          continue;
        }
        earliest_pump_us = clients_[i].next_pump_us;
        client_index = i;
        registration = clients_[i].registration;
      }

      if (client_index != kMaximumClientCount) {
        const double scalar = xe::Clock::guest_time_scalar();
        const uint64_t min_us =
            scalar > 0.0 ? static_cast<uint64_t>(kAudioPumpInterval / scalar)
                         : kAudioPumpInterval;
        // Counted from the deadline, so a late pump is made up and the host
        // queue absorbs the stall, until it falls further behind than the
        // queue holds.
        uint64_t next_pump_us = earliest_pump_us + min_us;
        if (now > earliest_pump_us + min_us * queued_frames_) {
          next_pump_us = now + min_us;
        }
        clients_[client_index].next_pump_us = next_pump_us;
      }
    }

    // No clients yet: park until one registers or we're told to stop.
    if (client_index == kMaximumClientCount) {
      xe::threading::Wait(pending_work_event_.get(), true);
      if (paused_) {
        pause_fence_.Signal();
        xe::threading::Wait(resume_event_.get(), false);
      }
      continue;
    }

    // Pace to kAudioIntervalSlack ahead of the deadline.
    const uint64_t wake_target_us = earliest_pump_us > kAudioIntervalSlack
                                        ? earliest_pump_us - kAudioIntervalSlack
                                        : 0;
    if (wake_target_us > now) {
      const std::chrono::milliseconds timeout((wake_target_us - now) / 1000);
      auto result =
          xe::threading::Wait(pending_work_event_.get(), true, timeout);
      if (result == xe::threading::WaitResult::kSuccess) {
        if (paused_) {
          pause_fence_.Signal();
          xe::threading::Wait(resume_event_.get(), false);
        }
        continue;
      }

      const uint64_t now_precise = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      if (wake_target_us > now_precise) {
        xe::threading::NanoSleepPrecise((wake_target_us - now_precise) * 1000);
      }
    }

    // Claim this exact generation, never a replacement found after pacing.
    {
      std::lock_guard<std::mutex> clients_lock(clients_mutex_);
      if (clients_[client_index].registration != registration ||
          registration->lifecycle.phase() !=
              AudioClientLifecycle::Phase::kActive ||
          xe::threading::Wait(client_semaphores_[client_index].get(), false,
                              std::chrono::milliseconds(0)) !=
              xe::threading::WaitResult::kSuccess ||
          !registration->lifecycle.BeginCallback()) {
        continue;
      }
    }

    bool executed = false;
    {
      AudioClientLifecycle::CallbackScope completion(registration->lifecycle);
      SCOPE_profile_cpu_i("apu", "xe::apu::AudioSystem->client_callback");
      try {
        // Preserve the existing SYSTEM callback ABI explicitly. Registration
        // process metadata is not permission to invent TITLE callback routing.
        if (worker_thread_->guest_object<kernel::X_KTHREAD>()->process_type ==
            registration->lifecycle.identity().process_type) {
          auto* state = worker_thread_->thread_state();
          state->context()->r[3] = registration->wrapped_callback_arg;
          executed = processor_->Execute(state, registration->callback);
        }
      } catch (const std::exception&) {
        XELOGE("APU: callback execution interrupted");
      }
      // Unknown exceptions and POSIX thread-exit forced unwind propagate.
      completion.SetExecuted(executed);
    }
    if (!executed) {
      std::shared_ptr<NativeAudioObservation> observation;
      {
        std::lock_guard lock(clients_mutex_);
        observation = registration->observation;
      }
      if (observation) { observation->Fail(); }
      XELOGE("APU: retaining failed callback generation {} until audio stop",
             registration->lifecycle.identity().generation);
    } else if (registration->lifecycle.phase() ==
               AudioClientLifecycle::Phase::kRetiring) {
      CleanupClient(client_index, registration);
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (int i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.registration) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::StopWorker() {
  worker_running_ = false;
  pending_work_event_->Set();
  // A paused worker waits for the resume.
  resume_event_->Set();
  if (worker_thread_) {
    worker_thread_->Wait(0, 0, 0, nullptr);
    worker_thread_.reset();
  }
  {
    std::lock_guard lock(clients_mutex_);
    for (const auto& slot : clients_) {
      if (slot.registration) {
        slot.registration->lifecycle.CallbackWorkerStopped();
      }
    }
  }
  pause_fence_.Signal();
}

void AudioSystem::Shutdown() {
  {
    std::lock_guard lock(clients_mutex_);
    shutting_down_ = true;
  }
  StopWorker();
  {
    // Emulator::Shutdown destroys AudioSystem before GuestScheduler. Wait for
    // preparation tokens and offloaded retirement/cleanup closures first.
    std::unique_lock lock(clients_mutex_);
    client_operations_drained_.wait(lock,
                                    [this] { return !client_operations_; });
  }
  ducker_policy_->Stop();
  StopIndependentAudio();
  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    std::shared_ptr<ClientRegistration> registration;
    {
      std::lock_guard lock(clients_mutex_);
      registration = clients_[i].registration;
    }
    if (registration) {
      registration->lifecycle.Retire(true);
      CleanupClient(i, registration);
    }
  }
  restored_clients_.clear();
}

X_STATUS AudioSystem::PrepareClient(uint32_t callback, uint32_t callback_arg,
                                    PreparedClient* prepared) {
  return PrepareClientInternal(callback, callback_arg, {}, 0, prepared);
}

X_STATUS AudioSystem::PrepareClientInternal(
    uint32_t callback, uint32_t callback_arg,
    std::optional<size_t> restored_index, uint32_t restored_wrapper,
    PreparedClient* prepared) {
  if (!kernel_state_ || !callback || (callback & 3) || !prepared ||
      prepared->registration_ ||
      (restored_index && *restored_index >= kMaximumClientCount)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto operation = BeginClientOperation();
  if (!operation) {
    return X_STATUS_UNSUCCESSFUL;
  }
  auto module = kernel_state_->GetUserModuleForAddress(callback);
  std::shared_ptr<void> module_lease;
  uint32_t hmodule = 0;
  auto owner_kind = AudioClientLifecycle::Identity::OwnerKind::kUnclassified;
  if (module) {
    module_lease = kernel_state_->AcquireUserModuleExecutionLease(module);
    if (!module_lease) {
      return X_STATUS_ACCESS_DENIED;
    }
    hmodule = module->hmodule_ptr();
    owner_kind = AudioClientLifecycle::Identity::OwnerKind::kModule;
  } else {
    // Preserve dynamic/unclassified callback compatibility. This is not a
    // fabricated module/data pin or proof that arbitrary guest code is safe.
    XELOGI(
        "APU: callback has no classified module owner; legacy lifetime "
        "applies");
  }
  uint32_t wrapper = restored_wrapper;
  if (!wrapper) {
    wrapper = memory()->SystemHeapAlloc(4);
    if (!wrapper) {
      return X_STATUS_NO_MEMORY;
    }
    xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(wrapper),
                                 callback_arg);
  } else if ((wrapper & 3) ||
             memory()->QueryRangeAccess(wrapper, 4) !=
                 xe::memory::PageAccess::kReadWrite ||
             xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(wrapper)) !=
                 callback_arg) {
    return X_STATUS_INVALID_PARAMETER;
  }
  size_t index;
  std::shared_ptr<ClientRegistration> registration;
  try {
    std::lock_guard lock(clients_mutex_);
    const int free_index =
        restored_index ? int(*restored_index) : FindFreeClient();
    if (free_index < 0 || clients_[free_index].registration || shutting_down_ ||
        !next_client_generation_ || !owner_epoch_) {
      index = kMaximumClientCount;
    } else {
      index = size_t(free_index);
      registration = std::make_shared<ClientRegistration>(
          AudioClientLifecycle::Identity{
              owner_epoch_, next_client_generation_++,
              kernel::X_PROCTYPE_SYSTEM, hmodule, owner_kind},
          std::move(module_lease), callback, callback_arg, wrapper);
      registration->observation_key = std::shared_ptr<const NativeAudioClientKey>(
          new NativeAudioClientKey(registration->lifecycle.identity(), index,
                                   callback, callback_arg, wrapper));
      clients_[index].registration = registration;
      clients_[index].next_pump_us = 0;
    }
  } catch (...) {
    if (!restored_wrapper) {
      memory()->SystemHeapFree(wrapper);
    }
    return X_STATUS_NO_MEMORY;
  }
  if (!registration) {
    if (!restored_wrapper) {
      memory()->SystemHeapFree(wrapper);
    }
    return X_STATUS_UNSUCCESSFUL;
  }
  AudioDriver* driver = nullptr;
  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  try {
    result =
        CreateClientDriver(index, client_semaphores_[index].get(), &driver);
  } catch (...) {
    XELOGE("APU: driver preparation interrupted");
  }
  bool can_publish;
  {
    std::lock_guard lock(clients_mutex_);
    registration->driver = driver;
    can_publish = !shutting_down_ && XSUCCEEDED(result) && driver;
  }
  const bool prepared_ok = registration->lifecycle.CompletePreparation();
  if (!can_publish || !prepared_ok) {
    registration->lifecycle.Retire();
    registration->lifecycle.WaitForQuiescence();
    CleanupClient(index, registration);
    return XFAILED(result) ? result : X_STATUS_UNSUCCESSFUL;
  }
  prepared->owner_ = this;
  prepared->index_ = index;
  prepared->registration_ = std::move(registration);
  prepared->operation_ = std::move(operation);
  return X_STATUS_SUCCESS;
}

X_STATUS AudioSystem::ActivateClient(PreparedClient& prepared) {
  if (prepared.owner_ != this || !prepared.registration_ ||
      !prepared.operation_ || prepared.index_ >= kMaximumClientCount) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto operation = prepared.operation_;
  {
    std::lock_guard lock(clients_mutex_);
    const auto& registration = prepared.registration_;
    if (shutting_down_ ||
        clients_[prepared.index_].registration != registration ||
        registration->lifecycle.identity().owner_epoch != owner_epoch_ ||
        registration->lifecycle.phase() !=
            AudioClientLifecycle::Phase::kReady ||
        !client_semaphores_[prepared.index_]->Release(queued_frames_,
                                                      nullptr) ||
        !registration->lifecycle.Activate()) {
      return X_STATUS_UNSUCCESSFUL;
    }
    clients_[prepared.index_].next_pump_us = 0;
  }
  // Destroying the operation takes clients_mutex_: do it after unlocking.
  pending_work_event_->Set();
  prepared = PreparedClient{};
  operation->Complete();
  return X_STATUS_SUCCESS;
}

std::shared_ptr<const NativeAudioClientKey> AudioSystem::DescribePreparedClient(
    const PreparedClient& prepared) {
  if (prepared.owner_ != this || !prepared.registration_ ||
      !prepared.operation_ || prepared.index_ >= kMaximumClientCount) { return {}; }
  std::lock_guard lock(clients_mutex_);
  const auto& registration = prepared.registration_;
  const auto& identity = registration->lifecycle.identity();
  if (shutting_down_ || clients_[prepared.index_].registration != registration ||
      identity.owner_epoch != owner_epoch_ || !identity.owner_hmodule ||
      identity.owner_kind != AudioClientLifecycle::Identity::OwnerKind::kModule ||
      registration->lifecycle.phase() != AudioClientLifecycle::Phase::kReady) {
    return {};
  }
  return registration->observation_key;
}

bool AudioSystem::AttachNativeObservation(
    const std::shared_ptr<const NativeAudioClientKey>& key,
    const std::shared_ptr<NativeAudioObservation>& observation) {
  if (!key || !observation || key->index() >= kMaximumClientCount) { return false; }
  auto operation = BeginClientOperation();
  if (!operation) { return false; }
  std::lock_guard lock(clients_mutex_);
  const auto& registration = clients_[key->index()].registration;
  if (shutting_down_ || !registration || registration->observation_key != key ||
      !registration->driver || registration->observation ||
      key->identity().owner_epoch != owner_epoch_ ||
      registration->lifecycle.phase() != AudioClientLifecycle::Phase::kActive ||
      !observation->Bind(*key, registration->frame_sequence,
                         registration->driver->NativeObservationBackend())) {
    return false;
  }
  registration->observation = observation;
  return true;
}

bool AudioSystem::DetachNativeObservation(
    const std::shared_ptr<const NativeAudioClientKey>& key,
    const std::shared_ptr<NativeAudioObservation>& observation) {
  if (!key || !observation || key->index() >= kMaximumClientCount) { return false; }
  auto operation = BeginClientOperation();
  if (!operation) { return false; }
  std::shared_ptr<NativeAudioObservation> detached;
  {
    std::lock_guard lock(clients_mutex_);
    const auto& registration = clients_[key->index()].registration;
    if (!registration || registration->observation_key != key ||
        registration->observation != observation) { return false; }
    detached = std::exchange(registration->observation, {});
  }
  detached->Close();
  return true;
}

void AudioSystem::CancelPreparedClient(PreparedClient& prepared) {
  if (prepared.owner_ != this || !prepared.registration_) {
    return;
  }
  auto operation = prepared.operation_;
  auto registration = prepared.registration_;
  registration->lifecycle.Retire();
  if (registration->lifecycle.WaitForQuiescence()) {
    CleanupClient(prepared.index_, registration);
  }
  prepared = PreparedClient{};
  operation->Complete();
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg,
                                     size_t* out_index) {
  PreparedClient prepared;
  auto result = PrepareClient(callback, callback_arg, &prepared);
  if (XFAILED(result)) {
    return result;
  }
  if (out_index) {
    *out_index = prepared.index();
  }
  result = ActivateClient(prepared);
  if (XFAILED(result)) {
    CancelPreparedClient(prepared);
  }
  return result;
}

void AudioSystem::SubmitFrame(
    size_t index, float* samples,
    const std::shared_ptr<const NativeAudioClientKey>& native_origin) {
  SCOPE_profile_cpu_f("apu");

  auto operation = BeginClientOperation();
  if (!operation) {
    return;
  }
  std::lock_guard<std::mutex> clients_lock(clients_mutex_);
  if (index >= kMaximumClientCount) {
    return;
  }
  const auto& registration = clients_[index].registration;
  if (!registration || !registration->driver || !samples) {
    return;
  }
  const auto phase = registration->lifecycle.phase();
  if (phase != AudioClientLifecycle::Phase::kActive &&
      !(phase == AudioClientLifecycle::Phase::kRetiring &&
        registration->lifecycle.IsCurrentCallback())) {
    return;
  }
  ++registration->frames_submitted;
  ++registration->frames_processed;
  if (registration->frame_sequence != UINT64_MAX) {
    ++registration->frame_sequence;
  } else if (registration->observation) {
    registration->observation->FailIfOpen();
  }
  auto observation = registration->observation;
  if (observation && observation->Peek().closed) { observation.reset(); }
  if (observation && (native_origin != registration->observation_key ||
                      !registration->lifecycle.IsCurrentCallback())) {
    // An AU slot can be submitted by unrelated callers. Preserve ordinary
    // playback, but never label those bytes as the authenticated native pump.
    observation->FailIfOpen();
    observation.reset();
  }
  const auto& identity = registration->lifecycle.identity();
  registration->driver->SubmitFrameObserved(
      samples, {identity.owner_epoch, identity.generation,
                registration->frame_sequence}, observation);
}

X_STATUS AudioSystem::ReadCategoryVolume(uint32_t category, uint32_t output) {
  if (category >= AudioDuckerPolicy::kCategoryCount) {
    return X_E_INVALIDARG;
  }
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  std::shared_ptr<ClientOperation> operation;
  try {
    operation = BeginClientOperation();
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  }
  if (!operation) {
    return X_STATUS(0xC00000A3);
  }
  const auto snapshot = ducker_policy_->Read();
  std::array<uint8_t, 4> bytes;
  xe::store_and_swap<float>(bytes.data(), snapshot->categories[category]);
  return kernel::util::WriteGuestIoBuffer(*memory_, output, bytes);
}

X_STATUS AudioSystem::ReadCategoryChangeMask(size_t index, uint32_t output) {
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  std::shared_ptr<ClientOperation> operation;
  try {
    operation = BeginClientOperation();
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  }
  if (!operation) {
    return X_STATUS(0xC00000A3);
  }
  std::shared_ptr<ClientRegistration> registration;
  {
    std::lock_guard lock(clients_mutex_);
    if (index >= kMaximumClientCount || !clients_[index].registration ||
        clients_[index].registration->lifecycle.phase() !=
            AudioClientLifecycle::Phase::kActive) {
      return X_STATUS_INVALID_HANDLE;
    }
    registration = clients_[index].registration;
  }
  const auto pending =
      registration->category_delivery.Stage(*ducker_policy_->Read());
  std::array<uint8_t, 4> bytes;
  xe::store_and_swap<uint32_t>(bytes.data(), pending.mask);
  const auto status = kernel::util::WriteGuestIoBuffer(*memory_, output, bytes);
  if (XSUCCEEDED(status)) {
    registration->category_delivery.Acknowledge(pending);
  }
  return status;
}

bool AudioSystem::GetClientPerformance(size_t index,
                                       ClientPerformance* out_perf) {
  auto operation = BeginClientOperation();
  if (!operation) {
    return false;
  }
  if (index >= kMaximumClientCount || !out_perf) {
    return false;
  }

  std::lock_guard lock(clients_mutex_);
  const auto& registration = clients_[index].registration;
  if (!registration ||
      registration->lifecycle.phase() != AudioClientLifecycle::Phase::kActive) {
    return false;
  }

  out_perf->frames_submitted = registration->frames_submitted.load();
  out_perf->frames_processed = registration->frames_processed.load();
  out_perf->frames_dropped = registration->frames_dropped.load();
  return true;
}

X_STATUS AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto operation = BeginClientOperation();
  if (!operation || index >= kMaximumClientCount) {
    return X_STATUS_INVALID_PARAMETER;
  }
  std::shared_ptr<ClientRegistration> registration;
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    registration = clients_[index].registration;
  }
  if (!registration) {
    return X_STATUS_INVALID_PARAMETER;
  }
  registration->lifecycle.Retire();
  if (registration->lifecycle.IsCurrentCallback()) {
    // Worker owns this exact snapshot and cleans it after Execute unwinds.
    operation->Complete();
    return X_STATUS_SUCCESS;
  }
  struct Outcome {
    std::atomic<bool> cleaned{false};
    threading::Fence done;
  };
  struct HostCompletion {
    HostCompletion(std::shared_ptr<ClientOperation> operation,
                   std::shared_ptr<Outcome> outcome)
        : operation(std::move(operation)), outcome(std::move(outcome)) {}
    HostCompletion(const HostCompletion&) = delete;
    HostCompletion& operator=(const HostCompletion&) = delete;
    std::shared_ptr<ClientOperation> operation;
    std::shared_ptr<Outcome> outcome;
    ~HostCompletion() {
      outcome->done.Signal();
      operation->Complete();
    }
  };
  auto outcome = std::make_shared<Outcome>();
  auto completion = std::make_shared<HostCompletion>(operation, outcome);
  std::function<void()> work = [this, index, registration, outcome,
                                completion = std::move(completion)] {
    (void)
        completion;  // Own completion even when the queued function is dropped.
    if (registration->lifecycle.WaitForQuiescence()) {
      outcome->cleaned.store(CleanupClient(index, registration),
                             std::memory_order_release);
    }
  };
  // RunBlockingIo retains a reference to a guest-stack std::function. Use an
  // owned PostHostCall instead: shutdown may drop queued jobs and reclaim
  // parked fibers without unwinding their locals. The job's completion ticket
  // closes the counter on execution OR cancellation, independently of them.
  if (kernel::GuestScheduler::CurrentThreadOffloadsBlockingCalls()) {
    kernel_state_->guest_scheduler()->PostHostCall(
        std::move(work),
        kernel::GuestScheduler::BlockingCallClass::kConcurrent);
  } else {
    work();
  }
  work = {};
  kernel::GuestScheduler::WaitOnFence(outcome->done);
  return outcome->cleaned.load(std::memory_order_acquire)
             ? X_STATUS_SUCCESS
             : X_STATUS_UNSUCCESSFUL;
}

bool AudioSystem::CleanupClient(
    size_t index, const std::shared_ptr<ClientRegistration>& registration) {
  if (index >= kMaximumClientCount ||
      registration->lifecycle.identity().owner_epoch != owner_epoch_) {
    return false;
  }
  if (!registration->lifecycle.ClaimCleanup()) {
    return registration->lifecycle.WaitForCleanup();
  }
  AudioDriver* driver_to_destroy;
  std::shared_ptr<NativeAudioObservation> observation;
  {
    std::lock_guard lock(clients_mutex_);
    if (clients_[index].registration != registration) {
      return false;
    }
    driver_to_destroy = std::exchange(registration->driver, nullptr);
    observation = std::exchange(registration->observation, {});
  }
  // Keep the slot occupied until both the guest callback and host driver have
  // stopped, and all old semaphore signals have been drained.
  DestroyDriver(driver_to_destroy);
  if (observation) { observation->Retire(); }
  memory()->SystemHeapFree(registration->wrapped_callback_arg);
  auto client_semaphore = client_semaphores_[index].get();
  xe::threading::WaitResult wait_result;
  do {
    wait_result = xe::threading::Wait(client_semaphore, false,
                                      std::chrono::milliseconds(0));
  } while (wait_result == xe::threading::WaitResult::kSuccess);
  assert_true(wait_result == xe::threading::WaitResult::kTimeout);
  {
    std::lock_guard lock(clients_mutex_);
    clients_[index].registration.reset();
    clients_[index].next_pump_us = 0;
  }
  registration->lifecycle.CompleteCleanup();
  return true;
}

bool AudioSystem::Save(ByteStream* stream) {
  std::shared_ptr<ClientOperation> operation;
  try {
    operation = BeginClientOperation();
  } catch (const std::bad_alloc&) {
    return false;
  }
  if (!operation) {
    return false;
  }
  if (!paused_ || !ducker_policy_->Read()->paused) {
    return false;
  }
  std::array<uint8_t, AudioDuckerPolicy::kSavedBytes> policy;
  if (!AudioDuckerPolicy::Encode(ducker_policy_->Save(), policy)) {
    return false;
  }
  std::vector<std::pair<uint32_t, std::shared_ptr<ClientRegistration>>>
      snapshot;
  try {
    snapshot.reserve(kMaximumClientCount);
  } catch (const std::bad_alloc&) {
    return false;
  }
  {
    std::lock_guard lock(clients_mutex_);
    if (shutting_down_ || client_operations_ != 1 ||
        !restored_clients_.empty()) {
      return false;
    }
    for (uint32_t i = 0; i < kMaximumClientCount; ++i) {
      if (auto registration = clients_[i].registration) {
        if (registration->lifecycle.phase() !=
            AudioClientLifecycle::Phase::kActive) {
          return false;
        }
        snapshot.emplace_back(i, std::move(registration));
      }
    }
  }
  constexpr size_t kClientBytes = 16 + 32 * sizeof(uint64_t);
  const size_t bytes = 12 + policy.size() + snapshot.size() * kClientBytes;
  if (!stream || stream->offset() > stream->data_length() ||
      bytes > stream->data_length() - stream->offset()) {
    return false;
  }
  stream->Write(make_fourcc("XAU2"));
  stream->Write(uint32_t(snapshot.size()));
  stream->Write(uint32_t(policy.size()));
  stream->Write(policy.data(), policy.size());
  for (const auto& [index, registration] : snapshot) {
    stream->Write(index);
    stream->Write(registration->callback);
    stream->Write(registration->callback_arg);
    stream->Write(registration->wrapped_callback_arg);
    for (auto version : registration->category_delivery.Save()) {
      stream->Write(version);
    }
  }
  return true;
}

bool AudioSystem::Restore(ByteStream* stream) {
  std::shared_ptr<ClientOperation> operation;
  try {
    operation = BeginClientOperation();
  } catch (const std::bad_alloc&) {
    return false;
  }
  if (!operation) {
    return false;
  }
  if (!paused_ || !stream || stream->offset() > stream->data_length() ||
      stream->data_length() - stream->offset() < 8) {
    return false;
  }
  const size_t start = stream->offset();
  const auto signature = stream->Read<uint32_t>();
  const bool legacy = signature == kAudioSaveSignature;
  if (!legacy && signature != make_fourcc("XAU2")) {
    return false;
  }
  const auto num_clients = stream->Read<uint32_t>();
  if (num_clients > kMaximumClientCount) {
    return false;
  }
  const size_t bytes = legacy ? 8 + num_clients * 16
                              : 12 + AudioDuckerPolicy::kSavedBytes +
                                    num_clients * (16 + 32 * 8);
  if (bytes > stream->data_length() - start) {
    return false;
  }
  AudioDuckerPolicy::SavedState policy;
  if (!legacy) {
    if (stream->Read<uint32_t>() != AudioDuckerPolicy::kSavedBytes) {
      return false;
    }
    std::array<uint8_t, AudioDuckerPolicy::kSavedBytes> encoded;
    stream->Read(encoded.data(), encoded.size());
    const auto parsed = AudioDuckerPolicy::Decode(encoded);
    if (!parsed) {
      return false;
    }
    policy = *parsed;
  }
  std::vector<RestoredClient> staged;
  try {
    staged.reserve(num_clients);
  } catch (const std::bad_alloc&) {
    return false;
  }
  uint32_t seen = 0;
  for (uint32_t i = 0; i < num_clients; ++i) {
    RestoredClient client{stream->Read<uint32_t>(), stream->Read<uint32_t>(),
                          stream->Read<uint32_t>(), stream->Read<uint32_t>()};
    if (client.index >= kMaximumClientCount || !client.callback ||
        (client.callback & 3) || !client.wrapped_callback_arg ||
        (client.wrapped_callback_arg & 3) || (seen & (1u << client.index))) {
      return false;
    }
    if (!legacy) {
      for (size_t j = 0; j < client.category_seen.size(); ++j) {
        client.category_seen[j] = stream->Read<uint64_t>();
        if (client.category_seen[j] > policy.state.category_versions[j]) {
          return false;
        }
      }
    }
    seen |= 1u << client.index;
    staged.push_back(client);
  }
  {
    std::lock_guard lock(clients_mutex_);
    if (client_operations_ != 1 || shutting_down_) {
      return false;
    }
    for (const auto& slot : clients_) {
      if (slot.registration && slot.registration->lifecycle.phase() ==
                                   AudioClientLifecycle::Phase::kFailed) {
        return false;
      }
    }
  }
  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    std::shared_ptr<ClientRegistration> registration;
    {
      std::lock_guard lock(clients_mutex_);
      registration = clients_[i].registration;
    }
    if (registration) {
      registration->lifecycle.Retire();
      if (!registration->lifecycle.WaitForQuiescence() ||
          !CleanupClient(i, registration)) {
        return false;
      }
    }
  }
  if (ducker_policy_->Restore(policy) != AudioDuckerPolicy::Status::kSuccess) {
    return false;
  }
  if (legacy) {
    XELOGW(
        "APU: legacy XAUD has no audio policy; restored explicit local "
        "defaults, without ducking continuity");
  }
  // Kernel/Memory restore precedes Resume's fresh module leases/activation.
  restored_clients_ = std::move(staged);
  return true;
}

void AudioSystem::Pause() {
  auto operation = BeginClientOperation();
  if (!operation) {
    return;
  }
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  pending_work_event_->Set();
  pause_fence_.Wait();

  if (ducker_policy_->SetPaused(true) != AudioDuckerPolicy::Status::kSuccess) {
    xe::FatalError("APU: audio policy clock could not pause");
  }
  xma_decoder_->Pause();
  PauseIndependentAudio(true);
}

void AudioSystem::Resume() {
  auto operation = BeginClientOperation();
  if (!operation) {
    return;
  }
  if (!paused_) {
    return;
  }
  const auto restored = std::move(restored_clients_);
  restored_clients_.clear();
  for (const auto& client : restored) {
    PreparedClient prepared;
    auto status = PrepareClientInternal(client.callback, client.callback_arg,
                                        client.index,
                                        client.wrapped_callback_arg, &prepared);
    if (XSUCCEEDED(status)) {
      prepared.registration_->category_delivery.Restore(client.category_seen);
      status = ActivateClient(prepared);
      if (XFAILED(status)) {
        CancelPreparedClient(prepared);
      }
    }
    if (XFAILED(status)) {
      // Resume's legacy void ABI cannot report a successful restored audio
      // generation. Keep it inactive and state this failure explicitly.
      XELOGE("APU: restored client {} stayed inactive, status={:08X}",
             client.index, status);
    }
  }
  if (ducker_policy_->SetPaused(false) != AudioDuckerPolicy::Status::kSuccess) {
    xe::FatalError("APU: audio policy clock could not resume");
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
  PauseIndependentAudio(false);
}

IndependentAudioPreparation AudioSystem::PrepareIndependentAudio(PcmAudio pcm) {
  IndependentAudioPreparation result;
  if (global_critical_region::is_held_by_current_thread() ||
      !IndependentAudioSession::ValidPcm(pcm)) {
    result.status = IndependentAudioPrepareStatus::kInvalidPcm;
    return result;
  }
  try {
    auto operation = BeginClientOperation();
    if (!operation) {
      return result;
    }
    {
      std::lock_guard lock(clients_mutex_);
      std::erase_if(independent_audio_,
                    [](const auto& weak) { return weak.expired(); });
      if (shutting_down_ ||
          independent_audio_.size() + independent_preparing_ >= 16) {
        return result;
      }
      ++independent_preparing_;
    }
    struct Reservation {
      AudioSystem* owner;
      ~Reservation() {
        std::lock_guard lock(owner->clients_mutex_);
        --owner->independent_preparing_;
      }
    } reservation{this};
    auto semaphore = threading::Semaphore::Create(kMaximumQueuedFrames,
                                                  kMaximumQueuedFrames);
    if (!semaphore) {
      return result;
    }
    const uint32_t channels = pcm.channels == 1 ? 2 : pcm.channels;
    std::unique_ptr<AudioDriver> driver(
        CreateDriver(semaphore.get(), pcm.frequency, channels, false));
    result = IndependentAudioSession::Prepare(
        std::move(pcm), std::move(driver), std::move(semaphore), is_paused());
    if (!result.session) {
      return result;
    }
    {
      std::lock_guard lock(clients_mutex_);
      std::erase_if(independent_audio_,
                    [](const auto& weak) { return weak.expired(); });
      if (!shutting_down_ && independent_audio_.size() < 16) {
        independent_audio_.push_back(result.session);
        // Sample pause atomically with publication against pause's snapshot.
        result.session->SetPaused(paused_.load());
        return result;
      }
    }
    result.session->BeginStop();
    result.session->Drain();
    result.session.reset();
    result.status = IndependentAudioPrepareStatus::kUnavailable;
    return result;
  } catch (const std::bad_alloc&) {
    result.session.reset();
    result.status = IndependentAudioPrepareStatus::kNoMemory;
    return result;
  } catch (const std::exception&) {
    result.session.reset();
    result.status = IndependentAudioPrepareStatus::kBackendFailed;
    return result;
  }
}
void AudioSystem::StopIndependentAudio() {
  std::vector<std::weak_ptr<IndependentAudioSession>> sessions;
  {
    std::lock_guard lock(clients_mutex_);
    sessions.swap(independent_audio_);
  }
  for (auto& weak : sessions) {
    if (auto session = weak.lock()) {
      session->BeginStop();
    }
  }
  for (auto& weak : sessions) {
    if (auto session = weak.lock()) {
      if (!session->Drain()) {
        std::terminate();
      }
    }
  }
}
void AudioSystem::PauseIndependentAudio(bool paused) {
  std::array<std::shared_ptr<IndependentAudioSession>, 16> sessions;
  size_t count = 0;
  {
    std::lock_guard lock(clients_mutex_);
    for (auto& weak : independent_audio_) {
      if (auto session = weak.lock()) {
        sessions[count++] = std::move(session);
      }
    }
  }
  for (size_t i = 0; i < count; ++i) {
    sessions[i]->SetPaused(paused);
  }
}

X_STATUS AudioSystem::CreateClientDriver(size_t index,
                                         xe::threading::Semaphore* semaphore,
                                         AudioDriver** out_driver) {
  *out_driver = nullptr;
  X_STATUS result = CreateDriver(index, semaphore, out_driver);
  if (result == X_STATUS_SUCCESS && *out_driver) {
    return result;
  }

  if (*out_driver) {
    DestroyDriver(*out_driver);
    *out_driver = nullptr;
  }
  // The audio worker paces registered clients; release their slots immediately.
  *out_driver = new nop::NopAudioDriver(semaphore);
  XELOGW("APU: {} failed to create a driver for client {}; continuing silently",
         name(), index);
  return X_STATUS_SUCCESS;
}

void AudioSystem::DestroyDriver(AudioDriver* driver) {
  if (driver) {
    driver->Shutdown();
    delete driver;
  }
}

}  // namespace apu
}  // namespace xe
