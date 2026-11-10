/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_content_storage_driver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <type_traits>

#include "xenia/base/mutex.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_content_storage_protocol.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_callback_dispatcher.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {
constexpr X_STATUS kDeviceNotConnected = 0xC000009D;
std::mutex registry_mutex;
std::map<KernelState*, std::weak_ptr<NativeContentStorageDriver>> registry;

bool Writable(Memory& memory, uint32_t address, uint32_t length) {
  const uint64_t end = uint64_t(address) + length;
  if (!address || !length || end > 0x100000000ull) {
    return false;
  }
  for (uint64_t cursor = address; cursor < end;) {
    auto* heap = memory.LookupHeap(uint32_t(cursor));
    HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(cursor), &info, 1) ||
        !(info.state & kMemoryAllocationCommit) ||
        !IsWritableProtect(info.protect)) {
      return false;
    }
    const uint64_t next =
        (cursor & ~(uint64_t(heap->page_size()) - 1)) + heap->page_size();
    if (next <= cursor) {
      return false;
    }
    cursor = next;
  }
  return true;
}
std::optional<uint32_t> Pointer(uint64_t value) {
  if (value >> 32 && value >> 32 != UINT32_MAX) { return {}; }
  return uint32_t(value) ? std::optional(uint32_t(value)) : std::nullopt;
}
std::optional<NativeStorageProcessingFrame> ActualFrame(
    KernelState& kernel, const NativeImportDomain& domain,
    const cpu::ppc::PPCContext& context, uint32_t packet) {
  if (!XThread::IsInThread()) { return {}; }
  auto* thread = XThread::GetCurrentThread();
  const auto sp = Pointer(context.r[1]), pcr = Pointer(context.r[13]);
  auto root = kernel.GetUserModuleForAddress(
      NativeContentStorageDriver::kProcessingFunction);
  if (!sp || !pcr || thread->kernel_state() != &kernel ||
      *pcr != thread->pcr_ptr() || context.kernel_state != &kernel ||
      context.processor != kernel.processor() ||
      context.thread_state != thread->thread_state() ||
      thread->thread_state()->context() != &context ||
      thread->native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM ||
      !root || !root->xex_module() || root->hmodule_ptr() != domain.root().hmodule ||
      root->load_generation() != domain.root().generation ||
      kernel.GetUserModuleForAddress(uint32_t(context.lr) - 4).get() != root.get() ||
      kernel.ResolveNativeImportCaller(uint32_t(context.lr)).domain.get() != &domain) {
    return {};
  }
  // Verify the exact canonical helpers whose metadata enables inlining. These
  // checks apply only to format bindings, never general startup admission.
  constexpr std::array<uint32_t, 7> save_words{
      0xFB61FFD0, 0xFB81FFD8, 0xFBA1FFE0, 0xFBC1FFE8,
      0xFBE1FFF0, 0x9181FFF8, 0x4E800020};
  constexpr std::array<uint32_t, 8> restore_words{
      0xEB61FFD0, 0xEB81FFD8, 0xEBA1FFE0, 0xEBC1FFE8,
      0xEBE1FFF0, 0x8181FFF8, 0x7D8803A6, 0x4E800020};
  const auto helper = [&](uint32_t address, const auto& words) {
    std::array<uint8_t, sizeof(words)> bytes;
    if (kernel.GetUserModuleForAddress(address).get() != root.get() ||
        !internal::ReadNativeWaitSnapshot(*kernel.memory(), address, bytes)) {
      return false;
    }
    for (size_t i = 0; i != words.size(); ++i) {
      if (xe::load_and_swap<uint32_t>(bytes.data() + i * 4) != words[i] ||
          root->xex_module()->TranslateCode(address + uint32_t(i) * 4) !=
              kernel.memory()->TranslateVirtual<const uint8_t*>(address + uint32_t(i) * 4)) {
        return false;
      }
    }
    return true;
  };
  if (!helper(0x8172D3C4, save_words) || !helper(0x8172D414, restore_words)) {
    return {};
  }
  // Both supported backends inline the recognized canonical restore27 and
  // reach E918's own JIT epilog only for its real restored LR. No helper alias.
  // Helper declarations carry the metadata even when early precompilation
  // is disabled and the backend inlines them without a compiled entry.
  // Read only the authenticated owned module; never Resolve/compile here.
  const auto* symbol = root->xex_module()->LookupSymbol(0x8172D414, false);
  if (!symbol || symbol->type() != cpu::Symbol::Type::kFunction ||
      symbol->module() != root->xex_module() ||
      symbol->address() != 0x8172D414) { return {}; }
  const auto* restore = static_cast<const cpu::Function*>(symbol);
  if (!restore->IsSaverest() || !restore->IsRestore() ||
      restore->SaverestType() != cpu::SaveRestoreType::GPR ||
      restore->SaverestIndex() != 27 ||
      restore->behavior() != cpu::Function::Behavior::kEpilogReturn) { return {}; }
  NativeStorageProcessingFrame value{domain.id(), domain.root().generation,
      thread->thread_id(), *pcr, *sp, uint32_t(context.lr), packet};
  return value.valid() ? std::optional(value) : std::nullopt;
}
struct ProcessingPacket {
  uint32_t path_pointer = 0, transition = 0, type = 0, native_id = 0;
  std::string path;
};
std::optional<ProcessingPacket> ReadProcessingPacket(Memory& memory, uint32_t at) {
  std::array<uint8_t, 16> bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, at, bytes)) { return {}; }
  const auto word = [&](size_t offset) {
    return xe::load_and_swap<uint32_t>(bytes.data() + offset);
  };
  ProcessingPacket packet{word(0), word(4), word(8), word(12), {}};
  if (!packet.path_pointer || (packet.transition != 1 && packet.transition != 2)) {
    return {};
  }
  for (uint32_t i = 0; i <= 62; ++i) {
    std::array<uint8_t, 1> character;
    if (uint64_t(packet.path_pointer) + i >= 0x100000000ull ||
        !internal::ReadNativeWaitSnapshot(memory, packet.path_pointer + i, character)) {
      return {};
    }
    if (!character[0]) { return packet.path.empty() ? std::nullopt : std::optional(packet); }
    if (character[0] < 0x20 || character[0] >= 0x7F) { return {}; }
    packet.path.push_back(char(character[0]));
  }
  return {};
}
bool BindingType(uint32_t type) { return type == 1 || type == 16; }
}  // namespace

struct NativeContentStorageDriver::State {
  mutable std::mutex mutex;
  std::condition_variable drained;
  std::shared_ptr<NativeContentStorageSource::Subscription> subscription;
  NativeContentStorageQueue queue;
  // Paths are driver-owned Kernel allocations, never native-private pointers.
  // Retain even a removed device's path until the entire Kernel has stopped.
  std::map<std::string, uint32_t> guest_paths;
  uint32_t callback = 0;
  uint64_t registration = 1, wake_epoch = 0, announced_epoch = 0;
  uint64_t delivered = 0, empty_drains = 0;
  size_t batch_cost = 0, active_calls = 0, pending_callbacks = 0;
  bool stopping = false, failed = false;
  struct ProcessingDelivery {
    NativeContentStorageDevice device;
    uint64_t cookie = 0;
    uint32_t thread = 0, pcr = 0, worker_sp = 0, path_pointer = 0;
    NativeContentStorageTransition transition;
    bool bootstrap = false;
  };
  static_assert(std::is_nothrow_move_assignable_v<ProcessingDelivery>);
  static_assert(std::is_nothrow_move_constructible_v<ProcessingDelivery>);
  struct Binding {
    NativeContentStorageDevice device;
    uint32_t native_id = 0;
    uint64_t cookie = 0;
  };
  // These contain only metadata/weak common proofs, never an old MVA lease.
  std::array<std::optional<ProcessingDelivery>, 64> processing_deliveries;
  std::array<std::optional<Binding>, 20> bindings;
  NativeStorageProcessingProtocol processing;
  std::optional<NativeContentStorageDevice> bootstrap;
  bool bootstrap_claimed = false, binding_failed = false;
  uint64_t processing_cookie = 0;
  void CloseBindings() {
    binding_failed = true;
    processing.Stop();
    bootstrap.reset();
    for (auto& delivery : processing_deliveries) { delivery.reset(); }
    for (auto& binding : bindings) { binding.reset(); }
  }
};
struct NativeContentStorageDriver::Call {
  std::shared_ptr<NativeContentStorageDriver> owner;
  ~Call() {
    std::lock_guard lock(owner->state_->mutex);
    --owner->state_->active_calls;
    owner->state_->drained.notify_all();
  }
};
NativeContentStorageDriver::NativeContentStorageDriver(Config config)
    : config_(std::move(config)), state_(std::make_unique<State>()) {}
std::shared_ptr<NativeContentStorageDriver> NativeContentStorageDriver::Create(
    Config config) {
  if (!config.kernel || !config.generation || !config.dispatcher ||
      !config.source || !config.authorize_current_caller ||
      !config.validate_callback || !config.validate_context ||
      !config.protocol_failed ||
      config.dispatcher->domain()->id() != config.generation ||
      !config.dispatcher->IsAccepting()) {
    return {};
  }
  auto owner = std::shared_ptr<NativeContentStorageDriver>(
      new NativeContentStorageDriver(std::move(config)));
  return owner->IsCurrent() ? owner : nullptr;
}
std::unique_ptr<NativeContentStorageDriver::Installation>
NativeContentStorageDriver::Install(
    const std::shared_ptr<NativeContentStorageDriver>& owner) {
  if (!owner || !owner->IsCurrent()) {
    return {};
  }
  {
    std::lock_guard lock(registry_mutex);
    if (registry[owner->config_.kernel].lock()) {
      return {};
    }
  }
  // The resident owner precedes subscription/publication. Failed or partial
  // startup retains all producer/guest-path lifetimes until Kernel teardown.
  owner->config_.kernel->RetainNativeResourceUntilTeardown(owner);
  const std::weak_ptr<NativeContentStorageDriver> weak = owner;
  std::optional<NativeContentStorageSource::Connection> connection;
  try {
    connection = owner->config_.source->Connect(
        [weak](NativeContentStorageSnapshot snapshot) {
          if (const auto current = weak.lock()) {
            current->Publish(std::move(snapshot));
          }
        });
  } catch (...) {
    owner->ProtocolFailed();
    return {};
  }
  if (!connection || !connection->subscription) {
    owner->ProtocolFailed();
    return {};
  }
  {
    std::lock_guard lock(owner->state_->mutex);
    owner->state_->subscription = std::move(connection->subscription);
  }
  owner->Publish(std::move(connection->initial));
  if (!owner->IsCurrent() || !owner->snapshot().accepting) {
    owner->BeginStop();
    return {};
  }
  auto installation = std::unique_ptr<Installation>(new Installation(owner));
  {
    std::lock_guard lock(registry_mutex);
    auto& slot = registry[owner->config_.kernel];
    if (slot.lock()) {
      owner->BeginStop();
      return {};
    }
    slot = owner;
  }
  installation->armed_ = true;
  return installation;
}
NativeContentStorageDriver::Installation::~Installation() {
  if (armed_) {
    owner_->BeginStop();
  }
}
std::shared_ptr<NativeContentStorageDriver> NativeContentStorageDriver::Lookup(
    KernelState* kernel) {
  std::lock_guard lock(registry_mutex);
  const auto found = registry.find(kernel);
  return found == registry.end() ? nullptr : found->second.lock();
}
NativeContentStorageDriver::~NativeContentStorageDriver() {
  BeginStop();
  // Kernel retains this resource until its producers/CPUs have stopped, before
  // Memory disappears. No PPC or source callback is executed by this release.
  for (const auto& [path, address] : state_->guest_paths) {
    if (address) {
      config_.kernel->memory()->SystemHeapFree(address);
    }
  }
  std::lock_guard lock(registry_mutex);
  const auto found = registry.find(config_.kernel);
  if (found != registry.end() && found->second.expired()) {
    registry.erase(found);
  }
}
bool NativeContentStorageDriver::IsCurrent() const {
  try {
    return config_.dispatcher->domain()->active() &&
           config_.dispatcher->domain()->id() == config_.generation &&
           config_.dispatcher->IsAccepting() && config_.validate_context();
  } catch (...) {
    return false;
  }
}
bool NativeContentStorageDriver::AuthorizeCurrentCaller() const {
  try {
    return IsCurrent() && XThread::IsInThread() &&
           XThread::GetCurrentThread()->kernel_state() == config_.kernel &&
           XThread::GetCurrentThread()
                   ->guest_object<X_KTHREAD>()
                   ->process_type == X_PROCTYPE_SYSTEM &&
           config_.authorize_current_caller();
  } catch (...) {
    return false;
  }
}
std::shared_ptr<NativeContentStorageDriver::Call>
NativeContentStorageDriver::AcquireCall() {
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || state_->active_calls == SIZE_MAX) {
    return {};
  }
  auto call = std::make_shared<Call>();
  call->owner = shared_from_this();
  ++state_->active_calls;
  return call;
}
bool NativeContentStorageDriver::SetCallback(uint32_t address) {
  const auto call = AcquireCall();
  if (!call || !AuthorizeCurrentCaller()) {
    return false;
  }
  try {
    if (address &&
        (address != kNativeCallback || !config_.validate_callback(address))) {
      return false;
    }
  } catch (...) {
    return false;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->registration == UINT64_MAX) {
      return false;
    }
    if (state_->callback != address) {
      state_->callback = address;
      ++state_->registration;
      state_->announced_epoch = 0;
    }
  }
  if (address && !QueueWake()) {
    ProtocolFailed();
    return false;
  }
  return true;
}
void NativeContentStorageDriver::Publish(
    NativeContentStorageSnapshot snapshot) noexcept {
  bool failed = false, wake = false;
  try {
    {
      std::lock_guard lock(state_->mutex);
      if (state_->stopping) {
        return;
      }
      std::optional<NativeContentStorageDevice> bootstrap;
      if (!state_->queue.initialized()) {
        for (const auto& device : snapshot.devices) {
          if (device.available && device.managed_format_proof &&
              NativeContentStorageQueue::IsBuiltinHdd(device)) {
            if (bootstrap) { state_->binding_failed = true; break; }
            bootstrap = device;
          }
        }
      }
      const auto result = state_->queue.Apply(std::move(snapshot));
      if (result == NativeContentStorageQueue::ApplyResult::kStale) {
        return;
      }
      if (result == NativeContentStorageQueue::ApplyResult::kInvalid ||
          state_->wake_epoch == UINT64_MAX) {
        failed = true;
      } else {
        if (bootstrap) { state_->bootstrap = std::move(bootstrap); }
        for (auto& binding : state_->bindings) {
          if (binding && !state_->queue.IsDesired(binding->device)) { binding.reset(); }
        }
        ++state_->wake_epoch;
        wake = state_->queue.Peek() != nullptr;
      }
    }
    if (wake && !QueueWake()) {
      failed = true;
    }
  } catch (...) {
    failed = true;
  }
  if (failed) {
    ProtocolFailed();
  }
}
bool NativeContentStorageDriver::QueueWake() noexcept {
  uint64_t registration = 0, announced = 0;
  try {
    {
      std::lock_guard lock(state_->mutex);
      if (state_->stopping) {
        return false;
      }
      if (!state_->callback || !state_->queue.Peek() ||
          state_->pending_callbacks ||
          state_->announced_epoch == state_->wake_epoch) {
        return true;
      }
      registration = state_->registration;
      announced = state_->wake_epoch;
      state_->announced_epoch = announced;
      ++state_->pending_callbacks;
    }
    const std::weak_ptr<NativeContentStorageDriver> weak = shared_from_this();
    auto reserved = std::make_shared<std::atomic<bool>>(false);
    const bool queued = config_.dispatcher->Enqueue(
        kNativeCallback, {},
        [weak, reserved](bool executed, uint32_t) {
          const auto owner = weak.lock();
          if (!owner) {
            return;
          }
          bool failed = false, again = false;
          {
            std::lock_guard lock(owner->state_->mutex);
            --owner->state_->pending_callbacks;
            failed = !executed && reserved->load() && !owner->state_->stopping;
            // A newer registration may have arrived while this old queued
            // callback was being suppressed. It still needs its own wake.
            again =
                !owner->state_->stopping &&
                owner->state_->callback == kNativeCallback &&
                owner->state_->wake_epoch != owner->state_->announced_epoch &&
                owner->state_->queue.Peek();
            owner->state_->drained.notify_all();
          }
          if (failed || (again && !owner->QueueWake())) {
            owner->ProtocolFailed();
          }
        },
        [weak, registration, reserved]() -> std::shared_ptr<void> {
          const auto owner = weak.lock();
          if (!owner) {
            return {};
          }
          try {
            if (!owner->IsCurrent() ||
                !owner->config_.validate_callback(kNativeCallback)) {
              owner->ProtocolFailed();
              return {};
            }
          } catch (...) {
            owner->ProtocolFailed();
            return {};
          }
          const auto call = owner->AcquireCall();
          if (!call) {
            return {};
          }
          {
            std::lock_guard lock(owner->state_->mutex);
            if (owner->state_->stopping ||
                owner->state_->registration != registration ||
                owner->state_->callback != kNativeCallback ||
                !owner->state_->queue.Peek()) {
              return {};
            }
          }
          reserved->store(true);
          return call;
        });
    if (queued) {
      return true;
    }
  } catch (...) {
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->pending_callbacks) {
      --state_->pending_callbacks;
    }
    state_->drained.notify_all();
  }
  return false;
}
X_STATUS NativeContentStorageDriver::GetNotification(uint32_t guest_output) {
  const auto call = AcquireCall();
  if (!call || !AuthorizeCurrentCaller()) {
    return kDeviceNotConnected;
  }
  bool resignal = false;
  try {
    auto memory_lock = global_critical_region::AcquireDirect();
    auto& memory = *config_.kernel->memory();
    if (!Writable(memory, guest_output,
                  sizeof(NativeContentStorageNotification))) {
      memory_lock.unlock();
      // This admitted fixed native worker supplies its own24-byte stack
      // record. A bad mapping is a protocol failure, never an empty drain.
      ProtocolFailed();
      return X_STATUS_INVALID_PARAMETER;
    }
    std::unique_lock lock(state_->mutex);
    if (state_->stopping) {
      return kDeviceNotConnected;
    }
    const auto* next = state_->queue.Peek();
    if (!next) {
      if (state_->empty_drains == UINT64_MAX) {
        lock.unlock();
        memory_lock.unlock();
        ProtocolFailed();
        return kDeviceNotConnected;
      }
      state_->batch_cost = 0;
      ++state_->empty_drains;
      return kNoMoreNotifications;
    }
    const size_t cost =
        next->transition == NativeContentStorageTransition::kInserted
            ? NativeContentStorageQueue::NativeVolumeCost(next->device)
            : 0;
    if (cost >
        NativeContentStorageQueue::kMaximumNativeVolumes - state_->batch_cost) {
      state_->batch_cost = 0;
      if (state_->wake_epoch == UINT64_MAX) {
        lock.unlock();
        memory_lock.unlock();
        ProtocolFailed();
        return kDeviceNotConnected;
      }
      ++state_->wake_epoch;
      resignal = true;
    } else {
      auto delivery = state_->queue.PrepareDelivery();
      if (!delivery) {
        lock.unlock();
        memory_lock.unlock();
        ProtocolFailed();
        return kDeviceNotConnected;
      }
      const auto& path = delivery->change().device.guest_volume_root;
      auto found = state_->guest_paths.find(path);
      if (found == state_->guest_paths.end()) {
        if (state_->guest_paths.size() == 1024) {
          lock.unlock();
          memory_lock.unlock();
          ProtocolFailed();
          return X_STATUS_NO_MEMORY;
        }
        found = state_->guest_paths.emplace(path, 0).first;
      }
      if (!found->second) {
        const uint32_t address =
            memory.SystemHeapAlloc(uint32_t(path.size() + 1));
        if (!address) {
          lock.unlock();
          memory_lock.unlock();
          ProtocolFailed();
          return X_STATUS_NO_MEMORY;
        }
        found->second = address;
        std::memcpy(memory.TranslateVirtual(address), path.c_str(),
                    path.size() + 1);
      }
      std::optional<State::ProcessingDelivery> processing;
      auto* current_thread = XThread::GetCurrentThread();
      const auto& context = *current_thread->thread_state()->context();
      const auto worker_sp = Pointer(context.r[1]);
      const auto worker_pcr = Pointer(context.r[13]);
      if (!state_->binding_failed && worker_sp && worker_pcr &&
          *worker_pcr == current_thread->pcr_ptr() &&
          (uint32_t(context.lr) == 0x8167EB94 || uint32_t(context.lr) == 0x8167ED44) &&
          uint64_t(*worker_sp) + 0x70 == guest_output &&
          state_->processing_cookie != UINT64_MAX) {
        processing.emplace(State::ProcessingDelivery{
            delivery->change().device, state_->processing_cookie + 1,
            current_thread->thread_id(), *worker_pcr, *worker_sp,
            found->second, delivery->change().transition, false});
      }
      NativeContentStorageNotification output{};
      output.volume_path = found->second;
      output.transition = uint32_t(delivery->change().transition);
      output.device_type = uint32_t(delivery->change().device.type);
      if (state_->delivered == UINT64_MAX) {
        lock.unlock();
        memory_lock.unlock();
        ProtocolFailed();
        return kDeviceNotConnected;
      }
      std::memcpy(memory.TranslateVirtual(guest_output), &output,
                  sizeof(output));
      // Same owner lock as preparation: this commit cannot become stale, and
      // performs no allocation after the complete caller output was stored.
      if (!state_->queue.CommitDelivery(std::move(*delivery))) {
        lock.unlock();
        memory_lock.unlock();
        ProtocolFailed();
        return kDeviceNotConnected;
      }
      if (processing) {
        auto slot = std::find_if(state_->processing_deliveries.begin(),
            state_->processing_deliveries.end(), [&](const auto& value) {
              return value && value->thread == processing->thread;
            });
        if (slot == state_->processing_deliveries.end()) {
          slot = std::find_if(state_->processing_deliveries.begin(),
              state_->processing_deliveries.end(), [](const auto& value) { return !value; });
        }
        if (slot != state_->processing_deliveries.end()) {
          state_->processing_cookie = processing->cookie;
          *slot = std::move(processing);  // no allocation after guest output
        } else {
          state_->binding_failed = true;
        }
      }
      state_->batch_cost += cost;
      ++state_->delivered;
      return X_STATUS_SUCCESS;
    }
  } catch (...) {
    ProtocolFailed();
    return X_STATUS_NO_MEMORY;
  }
  if (resignal && !QueueWake()) {
    ProtocolFailed();
    return kDeviceNotConnected;
  }
  return kNoMoreNotifications;
}
// These callbacks are attached only to the authenticated storage mutation
// function. Short Call scopes never span PPC execution, so an abandoned frame
// cannot strand driver rundown or a native-domain operation.
void NativeContentStorageDriver::ObserveProcessingEntry(
    void* raw, uint64_t function) noexcept {
  std::shared_ptr<NativeContentStorageDriver> owner;
  try {
    if (!raw || function != kProcessingFunction || !XThread::IsInThread()) { return; }
    owner = Lookup(XThread::GetCurrentThread()->kernel_state());
    const auto call = owner ? owner->AcquireCall() : nullptr;
    if (call) {
      if (owner->AuthorizeCurrentCaller()) {
        owner->OnProcessingEntry(*static_cast<const cpu::ppc::PPCContext*>(raw));
      } else {
        // An unadmitted call can still execute guest table-writing code. It
        // may revoke format metadata, never create or inherit its authority.
        owner->StopFormatBindings();
      }
    }
  } catch (...) {
    if (owner) { owner->StopFormatBindings(); }
  }
}
void NativeContentStorageDriver::ObserveProcessingReturn(
    void* raw, uint64_t function) noexcept {
  std::shared_ptr<NativeContentStorageDriver> owner;
  try {
    if (!raw || function != kProcessingFunction || !XThread::IsInThread()) { return; }
    owner = Lookup(XThread::GetCurrentThread()->kernel_state());
    const auto call = owner ? owner->AcquireCall() : nullptr;
    if (call) {
      if (owner->AuthorizeCurrentCaller()) {
        owner->OnProcessingReturn(*static_cast<const cpu::ppc::PPCContext*>(raw));
      } else {
        owner->StopFormatBindings();
      }
    }
  } catch (...) {
    if (owner) { owner->StopFormatBindings(); }
  }
}
void NativeContentStorageDriver::StopFormatBindings() noexcept {
  try {
    std::lock_guard lock(state_->mutex);
    state_->CloseBindings();
  } catch (...) {
  }
}
void NativeContentStorageDriver::OnProcessingEntry(
    const cpu::ppc::PPCContext& context) {
  const auto pointer = Pointer(context.r[3]);
  const auto frame = pointer ? ActualFrame(*config_.kernel,
      *config_.dispatcher->domain(), context, *pointer) : std::nullopt;
  if (!frame) { StopFormatBindings(); return; }
  const auto packet = ReadProcessingPacket(*config_.kernel->memory(), *pointer);
  if (!packet) { StopFormatBindings(); return; }
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || state_->binding_failed) { return; }
  // E918 removes matching old rows before any insertion. Revoke first even
  // when this call is not joined to an owned host change or later fails.
  for (auto& binding : state_->bindings) {
    if (binding && NativeStorageProcessingProtocol::PathsOverlap(
          binding->device.guest_volume_root, packet->path)) { binding.reset(); }
  }
  if (frame->lr == 0x8167EE2C && state_->bootstrap_claimed) {
    // Reinitialization cleared the whole native table before this call.
    state_->CloseBindings();
    return;
  }
  uint64_t cookie = 0;
  if (frame->lr == 0x8167EC40 &&
      uint64_t(frame->sp) + 0x50 == frame->packet) {
    for (const auto& delivery : state_->processing_deliveries) {
      if (delivery && !delivery->bootstrap && delivery->thread == frame->thread &&
          delivery->pcr == frame->pcr && delivery->worker_sp == frame->sp &&
          delivery->path_pointer == packet->path_pointer &&
          uint32_t(delivery->transition) == packet->transition &&
          uint32_t(delivery->device.type) == packet->type &&
          delivery->device.guest_volume_root == packet->path && BindingType(packet->type)) {
        cookie = delivery->cookie;
        break;
      }
    }
  } else if (frame->lr == 0x8167EE2C &&
             uint64_t(frame->sp) + 0x60 == frame->packet &&
             packet->path_pointer == 0x815F3A24 && packet->transition == 1 &&
             packet->type == 1 && !state_->bootstrap_claimed && state_->bootstrap &&
             state_->bootstrap->guest_volume_root == packet->path &&
             state_->queue.IsDesired(*state_->bootstrap)) {
    // This baseline was deliberately NOT delivered by210. Only the genuine
    // initializer's actual mutation can turn it into a processed receipt.
    state_->bootstrap_claimed = true;
    auto slot = std::find_if(state_->processing_deliveries.begin(),
        state_->processing_deliveries.end(), [](const auto& value) { return !value; });
    if (slot != state_->processing_deliveries.end() &&
        state_->processing_cookie != UINT64_MAX) {
      cookie = ++state_->processing_cookie;
      slot->emplace(State::ProcessingDelivery{*state_->bootstrap, cookie,
          frame->thread, frame->pcr, frame->sp, packet->path_pointer,
          NativeContentStorageTransition::kInserted, true});
    }
  }
  // MU/Sapphire's paired/partially successful packets and other native callers
  // may invalidate bindings but cannot mint a managed format capability here.
  state_->processing.Begin({*frame, packet->path_pointer, packet->transition,
                            packet->type, packet->path, cookie});
}
void NativeContentStorageDriver::OnProcessingReturn(
    const cpu::ppc::PPCContext& context) {
  std::optional<NativeStorageProcessingFrame> saved;
  {
    std::lock_guard lock(state_->mutex);
    saved = state_->processing.Pending(XThread::GetCurrentThread()->thread_id());
  }
  if (!saved) { return; }
  const auto frame = ActualFrame(*config_.kernel, *config_.dispatcher->domain(),
                                 context, saved->packet);
  if (!frame) { StopFormatBindings(); return; }
  const auto packet = ReadProcessingPacket(*config_.kernel->memory(), saved->packet);
  if (!packet) { StopFormatBindings(); return; }
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || state_->binding_failed) { return; }
  auto receipt = state_->processing.Complete(*frame, uint32_t(context.r[3]),
      packet->path_pointer, packet->transition, packet->type,
      packet->native_id, packet->path);
  if (!receipt) { state_->CloseBindings(); return; }
  if (!receipt->call.delivery_cookie) { return; }  // Unjoined native mutation.
  auto delivery = std::find_if(state_->processing_deliveries.begin(),
      state_->processing_deliveries.end(), [&](const auto& value) {
        return value && value->cookie == receipt->call.delivery_cookie &&
            value->thread == frame->thread && value->pcr == frame->pcr &&
            value->worker_sp == frame->sp;
      });
  if (delivery == state_->processing_deliveries.end()) { return; }
  auto owned = std::move(**delivery);
  delivery->reset();
  if (!receipt->succeeded || receipt->call.transition != 1 ||
      !owned.device.managed_format_proof || !BindingType(receipt->call.type) ||
      !state_->queue.IsDesired(owned.device)) { return; }
  auto same_id = std::find_if(state_->bindings.begin(), state_->bindings.end(),
      [&](const auto& value) { return value && value->native_id == receipt->native_id; });
  if (same_id != state_->bindings.end()) {
    // An old unrelated mapping survived a native-ID reuse: do not pick either.
    same_id->reset();
    state_->binding_failed = true;
    return;
  }
  auto free = std::find_if(state_->bindings.begin(), state_->bindings.end(),
      [](const auto& value) { return !value; });
  if (free == state_->bindings.end()) { state_->binding_failed = true; return; }
  free->emplace(State::Binding{std::move(owned.device), receipt->native_id,
                               receipt->call.delivery_cookie});
}
std::shared_ptr<const xam::ManagedFormatSourceProof>
NativeContentStorageDriver::CaptureFormatSource(uint32_t native_id,
                                                std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  const auto call = AcquireCall();
  if (!call || !native_id || !AuthorizeCurrentCaller()) {
    error = std::make_error_code(std::errc::operation_not_permitted); return {};
  }
  // Filesystem/currentness work belongs to the host-operation boundary, after
  // native storage has left its table CS and critical region. The current
  // thread owns both relevant lifetimes; no native lock is acquired here.
  {
    auto critical = global_critical_region::AcquireDirect();
    auto* thread = XThread::GetCurrentThread();
    std::array<uint8_t, 28> storage_cs;
    if (!thread || thread->kernel_state() != config_.kernel ||
        thread->guest_object<X_KTHREAD>()->apc_disable_count ||
        !internal::ReadNativeWaitSnapshot(*config_.kernel->memory(),
                                         0x81A8132C, storage_cs) ||
        xe::load_and_swap<uint32_t>(storage_cs.data() + 0x18) ==
            thread->guest_object()) {
      error = std::make_error_code(std::errc::operation_would_block);
      return {};
    }
  }
  std::shared_ptr<const xam::ManagedFormatSourceProof> proof;
  std::optional<NativeContentStorageDevice> processed;
  uint64_t cookie = 0;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->binding_failed) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    for (const auto& binding : state_->bindings) {
      if (binding && binding->native_id == native_id &&
          state_->queue.IsDesired(binding->device)) {
        proof = binding->device.managed_format_proof;
        processed = binding->device;
        cookie = binding->cookie;
        break;
      }
    }
  }
  // current() may transiently own the common State while inspecting cached
  // metadata. It is deliberately outside driver/GCRC/profile/model locks.
  if (!proof || !processed || !proof->current() || !AuthorizeCurrentCaller()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  // Join to the actual retained common registry, not arbitrary provider fields
  // or a borrowed proof relabelled with another path. This observing IO and all
  // eventual strong MVA/root release are outside the native-driver locks.
  const auto source = config_.kernel->content_manager()->sources().Source(proof->source_id());
  if (!source || !source->available || !source->format_eligible || !source->volume ||
      source->managed_format_proof != proof || source->generation != proof->generation() ||
      processed->identity != source->source.id ||
      processed->generation != source->generation ||
      processed->type != source->source.device_type ||
      processed->guest_volume_root != source->source.guest_volume_root) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->binding_failed ||
        std::none_of(state_->bindings.begin(), state_->bindings.end(),
          [&](const auto& binding) {
            return binding && binding->native_id == native_id &&
                binding->cookie == cookie &&
                binding->device.managed_format_proof == proof &&
                state_->queue.IsDesired(binding->device);
          })) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
  }
  // The later format reservation checks this EXACT proof again at the common
  // retirement boundary. Never resolve a native number or path a second time.
  return proof;
}
void NativeContentStorageDriver::ProtocolFailed() noexcept {
  bool first = false;
  {
    std::lock_guard lock(state_->mutex);
    first = !state_->failed;
    state_->failed = true;
  }
  BeginStop();
  if (first) {
    try {
      config_.protocol_failed();
    } catch (...) {
    }
  }
}
void NativeContentStorageDriver::BeginStop() noexcept {
  std::shared_ptr<NativeContentStorageSource::Subscription> subscription;
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->CloseBindings();
    state_->callback = 0;
    subscription = state_->subscription;
    state_->drained.notify_all();
  }
  if (subscription) {
    subscription->BeginStop();
  }
}
bool NativeContentStorageDriver::Drain() noexcept {
  BeginStop();
  if (config_.dispatcher->IsCurrentWorker()) {
    return false;
  }
  std::shared_ptr<NativeContentStorageSource::Subscription> subscription;
  {
    std::lock_guard lock(state_->mutex);
    subscription = state_->subscription;
  }
  if (subscription && !subscription->Drain()) {
    return false;
  }
  std::unique_lock lock(state_->mutex);
  return state_->drained.wait_for(lock, std::chrono::seconds(5), [&] {
    return !state_->active_calls && !state_->pending_callbacks;
  });
}
NativeContentStorageDriver::Snapshot NativeContentStorageDriver::snapshot()
    const {
  std::lock_guard lock(state_->mutex);
  return {!state_->stopping && !state_->failed && state_->queue.initialized(),
          state_->failed,
          state_->callback == kNativeCallback,
          state_->queue.revision(),
          state_->delivered,
          state_->empty_drains,
          state_->queue.size(),
          state_->pending_callbacks,
          state_->active_calls,
          size_t(std::count_if(state_->bindings.begin(), state_->bindings.end(),
              [](const auto& binding) { return bool(binding); })),
          state_->binding_failed};
}
}  // namespace xe::kernel
