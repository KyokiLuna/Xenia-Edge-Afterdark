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
#include "xenia/emulator.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/base/console_storage_volume.h"
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
#include "xenia/kernel/xam/selected_media_source_proof.h"
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
  // Independent metadata owner tag prevents receipt ABA across a driver
  // recreation with the same Kernel address/domain/cookie values.
  const std::shared_ptr<const uint8_t> content_issuer =
      std::make_shared<const uint8_t>(0);
  // Paths are driver-owned Kernel allocations, never native-private pointers.
  // Retain even a removed device's path until the entire Kernel has stopped.
  std::map<std::string, uint32_t> guest_paths;
  uint32_t callback = 0;
  uint64_t registration = 1, wake_epoch = 0, announced_epoch = 0;
  uint64_t delivered = 0, empty_drains = 0;
  size_t batch_cost = 0, active_calls = 0, pending_callbacks = 0, config_uses = 0;
  bool stopping = false, failed = false, drain_proved = false;
  bool retired = false, retiring = false;
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
  std::shared_ptr<const NativeContentStorageDriver> owner;
  bool config_use = false;
  ~Call() {
    std::lock_guard lock(owner->state_->mutex);
    if (config_use) --owner->state_->config_uses;
    else --owner->state_->active_calls;
    owner->state_->drained.notify_all();
  }
};
NativeContentStorageDriver::NativeContentStorageDriver(Config config)
    : kernel_identity_(config.kernel), generation_(config.generation),
      config_(std::move(config)), state_(std::make_unique<State>()) {}
std::shared_ptr<NativeContentStorageDriver> NativeContentStorageDriver::Create(
    Config config) {
  const auto domain = config.dispatcher ? config.dispatcher->domain() : nullptr;
  if (!config.kernel || !config.generation || !config.dispatcher ||
      !config.source || !config.authorize_current_caller ||
      !config.validate_callback || !config.validate_context ||
      !config.protocol_failed ||
      !domain || domain->id() != config.generation ||
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
  const auto config_use = owner ? owner->AcquireConfigUse() : nullptr;
  if (!config_use || !owner->IsCurrent()) {
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
    owner->state_->drain_proved = false;
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
  std::shared_ptr<NativeContentStorageDriver> owner;
  {std::lock_guard lock(registry_mutex);
    const auto found = registry.find(kernel);
    if (found != registry.end()) owner = found->second.lock();}
  if (!owner) return {};
  std::lock_guard lock(owner->state_->mutex);
  return owner->state_->retired || owner->state_->retiring ||
                 owner->kernel_identity_ != kernel ? nullptr : owner;
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
  const auto found = registry.find(kernel_identity_);
  const auto self = weak_from_this();
  if (found != registry.end() && !found->second.owner_before(self) &&
      !self.owner_before(found->second)) {
    registry.erase(found);
  }
}
bool NativeContentStorageDriver::IsCurrent() const {
  const auto use = AcquireConfigUse();
  if (!use) return false;
  try {
    const auto domain = config_.dispatcher ? config_.dispatcher->domain() : nullptr;
    return domain && domain->active() &&
           domain->id() == generation_ &&
           config_.dispatcher->IsAccepting() && config_.validate_context();
  } catch (...) {
    return false;
  }
}
bool NativeContentStorageDriver::AuthorizeCurrentCaller() const {
  const auto use = AcquireConfigUse();
  if (!use) return false;
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
  if (state_->stopping || state_->retiring || state_->retired ||
      state_->active_calls == SIZE_MAX) {
    return {};
  }
  auto call = std::make_shared<Call>();
  call->owner = shared_from_this();
  ++state_->active_calls;
  return call;
}
std::shared_ptr<NativeContentStorageDriver::Call>
NativeContentStorageDriver::AcquireConfigUse(bool stopping_allowed) const noexcept try {
  std::lock_guard lock(state_->mutex);
  if (state_->retiring || state_->retired ||
      (!stopping_allowed && state_->stopping) || state_->config_uses == SIZE_MAX)
    return {};
  const auto owner = shared_from_this();
  auto use = std::make_shared<Call>();
  use->owner = owner;
  use->config_use = true;
  ++state_->config_uses;
  return use;
} catch (...) {return {};}
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
  const auto use = AcquireConfigUse();
  if (!use) return false;
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
uint32_t NativeContentStorageDriver::GetOrCreateGuestPathLocked(
    const std::string& path) {
  auto found = state_->guest_paths.find(path);
  if (found == state_->guest_paths.end()) {
    if (state_->guest_paths.size() == 1024) return 0;
    found = state_->guest_paths.emplace(path,0).first;
  }
  if (!found->second) {
    const uint32_t address = config_.kernel->memory()->SystemHeapAlloc(
        uint32_t(path.size()+1));
    if (!address) return 0;
    found->second = address;
    std::memcpy(config_.kernel->memory()->TranslateVirtual(address),
                path.c_str(),path.size()+1);
  }
  return found->second;
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
      const uint32_t path_address = GetOrCreateGuestPathLocked(path);
      if (!path_address) {
        lock.unlock();
        memory_lock.unlock();
        ProtocolFailed();
        return X_STATUS_NO_MEMORY;
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
            path_address, delivery->change().transition, false});
      }
      NativeContentStorageNotification output{};
      output.volume_path = path_address;
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
std::shared_ptr<const NativeOpticalStorageProof>
NativeContentStorageDriver::CaptureOpticalSource(std::string_view native_root) const {
  const auto use=AcquireConfigUse();if(!use || !config_.kernel)return {};
  std::weak_ptr<vfs::Device> weak;
  const vfs::Device* identity=nullptr;
  std::shared_ptr<const filesystem::ConsoleStorageVolume> volume;
  uint64_t generation=0;
  {
    auto global=global_critical_region::AcquireDirect();
    // Borrow from the registered VFS row only while this same namespace lock
    // owns it. Copy weak/control-block and volume METADATA, never a temporary
    // strong Device whose final FD/resource release could land on this native
    // dispatcher stack after a concurrent mount withdrawal.
    const auto& title_mount=config_.kernel->title_mount_path_;
    auto* device=title_mount.empty()?nullptr:config_.kernel->file_system()->GetDevice(title_mount);
    if(!device || device->namespace_retiring() ||
        !xe::utf8::equal_case(device->mount_path(),native_root) ||
        !xe::utf8::equal_case(title_mount,native_root))return {};
    volume=device->storage_volume();
    if(!volume || !volume->available_cached() ||
        volume->origin()!=filesystem::ConsoleStorageVolume::Origin::kOpticalMedia ||
        volume->type()!=filesystem::ConsoleStorageType::ODD)return {};
    const auto execution=config_.kernel->emulator()->title_execution_state().snapshot();
    if(!execution.generation ||
        (execution.phase!=TitleExecutionState::Phase::kPreparing && execution.phase!=TitleExecutionState::Phase::kRunning))return {};
    generation=execution.generation;weak=device->weak_from_this();identity=device;
  }
  return std::shared_ptr<const NativeOpticalStorageProof>(new NativeOpticalStorageProof(
      std::move(volume),std::move(weak),identity,std::string(native_root),generation));
}
bool NativeContentStorageDriver::OpticalSourceCurrent(const NativeOpticalStorageProof& proof) const {
  const auto use=AcquireConfigUse();if(!use || !config_.kernel)return false;
  auto global=global_critical_region::AcquireDirect();
  return OpticalSourceCurrentLocked(proof);
}
bool NativeContentStorageDriver::OpticalSourceCurrentLocked(const NativeOpticalStorageProof& proof) const {
  if(!global_critical_region::is_held_by_current_thread() ||
      !config_.kernel || config_.kernel!=kernel_identity_)return false;
  const auto& title_mount=config_.kernel->title_mount_path_;
  const auto* device=title_mount.empty()?nullptr:config_.kernel->file_system()->GetDevice(title_mount);
  if(!device || device!=proof.identity_ || device->namespace_retiring() ||
      !xe::utf8::equal_case(title_mount,proof.root_))return false;
  const auto weak=device->weak_from_this();
  const auto execution=config_.kernel->emulator()->title_execution_state().snapshot();
  return !proof.device_.owner_before(weak) && !weak.owner_before(proof.device_) &&
      device->storage_volume()==proof.volume_ && proof.volume_ && proof.volume_->available_cached() &&
      execution.generation==proof.title_generation_ &&
      (execution.phase==TitleExecutionState::Phase::kPreparing || execution.phase==TitleExecutionState::Phase::kRunning);
}
void NativeContentStorageDriver::OnProcessingEntry(
    const cpu::ppc::PPCContext& context) {
  const auto pointer = Pointer(context.r[3]);
  const auto domain = config_.dispatcher ? config_.dispatcher->domain() : nullptr;
  const auto frame = pointer && domain ? ActualFrame(*config_.kernel,
      *domain, context, *pointer) : std::nullopt;
  if (!frame) { StopFormatBindings(); return; }
  const auto packet = ReadProcessingPacket(*config_.kernel->memory(), *pointer);
  if (!packet) { StopFormatBindings(); return; }
  std::shared_ptr<const NativeOpticalStorageProof> optical;
  if(frame->lr==0x8167EA50 && uint64_t(frame->sp)+0x50==frame->packet &&
      packet->path_pointer==0x815F39F4 && packet->type==4 && packet->transition==1 &&
      packet->path=="\\Device\\CdRom0")optical=CaptureOpticalSource(packet->path);
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
  if(optical && state_->processing_cookie!=UINT64_MAX) {
    auto slot=std::find_if(state_->processing_deliveries.begin(),state_->processing_deliveries.end(),
        [](const auto& value){return !value;});
    if(slot!=state_->processing_deliveries.end()) {
      cookie=++state_->processing_cookie;
      NativeContentStorageDevice observed;
      observed.identity="title-optical";observed.generation=optical->title_generation();
      observed.type=xam::DeviceType::ODD;observed.guest_volume_root=packet->path;observed.available=true;
      observed.source_volume=optical->volume();observed.optical_source_proof=std::move(optical);
      slot->emplace(State::ProcessingDelivery{std::move(observed),cookie,frame->thread,
          frame->pcr,frame->sp,packet->path_pointer,NativeContentStorageTransition::kInserted,false});
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
  const auto domain = config_.dispatcher ? config_.dispatcher->domain() : nullptr;
  const auto frame = domain ? ActualFrame(*config_.kernel, *domain,
                                         context, saved->packet) : std::nullopt;
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
  const auto& volume=owned.device.source_volume;
  const auto& optical=owned.device.optical_source_proof;
  const bool optical_valid=optical && receipt->call.type==4 &&
      owned.device.type==xam::DeviceType::ODD && volume && volume==optical->volume() &&
      volume->origin()==filesystem::ConsoleStorageVolume::Origin::kOpticalMedia &&
      volume->available_cached() && owned.device.generation==optical->title_generation() &&
      owned.device.guest_volume_root==optical->guest_root();
  const bool registered_valid=!optical && BindingType(receipt->call.type) &&
      state_->queue.IsDesired(owned.device) &&
      (owned.device.managed_format_proof || owned.device.selected_media_proof || volume) &&
      (!volume || (volume->available_cached() &&
          volume->origin()==filesystem::ConsoleStorageVolume::Origin::kRegisteredSource &&
          volume->source_id()==owned.device.identity && volume->generation()==owned.device.generation &&
          volume->type()==owned.device.type)) &&
      (!owned.device.selected_media_proof || owned.device.selected_media_proof->SourceCurrentCached());
  if(!receipt->succeeded || receipt->call.transition!=1 || (!optical_valid && !registered_valid))return;
  // Optical is an authentic native-produced row, not a queue notification or
  // format capability. Capturing/consuming it later also requires the same
  // live existing VFS Device/title generation, outside this metadata mutex.
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
std::optional<NativeContentStorageDriver::ContentProcessingReceipt>
NativeContentStorageDriver::CaptureContentProcessing(uint32_t native_id) const noexcept try {
  const auto use=AcquireConfigUse();
  if(!use || !native_id || !IsCurrent())return {};
  std::optional<ContentProcessingReceipt> result;
  {
    std::lock_guard lock(state_->mutex);
    if(state_->stopping || state_->binding_failed)return {};
    for(const auto& binding:state_->bindings) {
      if(!binding || binding->native_id!=native_id || !binding->cookie ||
          !binding->device.source_volume || !binding->device.source_volume->available_cached() ||
          (!binding->device.optical_source_proof && !state_->queue.IsDesired(binding->device)))continue;
      if(result)return {};
      result=ContentProcessingReceipt{};result->issuer_=state_->content_issuer;
      result->driver_generation_=generation_;result->cookie_=binding->cookie;
      result->native_id_=binding->native_id;result->device_=binding->device;
    }
  }
  if(result && result->device_.optical_source_proof &&
      !OpticalSourceCurrent(*result->device_.optical_source_proof))return {};
  return result;
} catch(...) {return {};}
std::optional<NativeContentStorageDriver::ContentProcessingReceipt>
NativeContentStorageDriver::CaptureContentProcessing(
    const std::shared_ptr<const filesystem::ConsoleStorageVolume>& volume) const noexcept try {
  const auto use=AcquireConfigUse();
  if(!use || !volume || !volume->available_cached() || !IsCurrent())return {};
  std::optional<ContentProcessingReceipt> result;
  {
    std::lock_guard lock(state_->mutex);
    if(state_->stopping || state_->binding_failed)return {};
    for(const auto& binding:state_->bindings) {
      if(!binding || !binding->native_id || !binding->cookie ||
          binding->device.source_volume!=volume ||
          binding->device.source_volume.owner_before(volume) || volume.owner_before(binding->device.source_volume) ||
          (!binding->device.optical_source_proof && !state_->queue.IsDesired(binding->device)))continue;
      if(result)return {};
      result=ContentProcessingReceipt{};result->issuer_=state_->content_issuer;
      result->driver_generation_=generation_;result->cookie_=binding->cookie;
      result->native_id_=binding->native_id;result->device_=binding->device;
    }
  }
  if(result && result->device_.optical_source_proof &&
      !OpticalSourceCurrent(*result->device_.optical_source_proof))return {};
  return result;
} catch(...) {return {};}
std::vector<NativeContentStorageDriver::ContentProcessingReceipt>
NativeContentStorageDriver::CaptureContentProcessing() const {
  const auto use=AcquireConfigUse();
  if(!use || !IsCurrent())return {};
  std::vector<ContentProcessingReceipt> result;
  {
    std::lock_guard lock(state_->mutex);
    if(state_->stopping || state_->binding_failed)return {};
    result.reserve(state_->bindings.size());
    for(const auto& binding:state_->bindings) {
      if(!binding || !binding->native_id || !binding->cookie ||
          !binding->device.source_volume || !binding->device.source_volume->available_cached() ||
          (!binding->device.optical_source_proof && !state_->queue.IsDesired(binding->device)))continue;
      ContentProcessingReceipt row;
      row.issuer_=state_->content_issuer;row.driver_generation_=generation_;
      row.cookie_=binding->cookie;row.native_id_=binding->native_id;row.device_=binding->device;
      result.push_back(std::move(row));
    }
  }
  // No VFS/current-title check or potential final Config owner drop occurs
  // under the Driver metadata lock. Holding a row owns no backing/FD/Task.
  std::erase_if(result,[&](const auto& row){return row.optical_source() &&
      !OpticalSourceCurrent(*row.optical_source());});
  return result;
}
bool NativeContentStorageDriver::ContentProcessingCurrent(
    const ContentProcessingReceipt& receipt) const noexcept try {
  const auto use = AcquireConfigUse();
  if (!use || !IsCurrent() || !receipt.native_id_ || !receipt.cookie_ ||
      receipt.driver_generation_ != generation_ || !receipt.device_.source_volume ||
      !receipt.device_.source_volume->available_cached()) return false;
  if(receipt.device_.optical_source_proof &&
      !OpticalSourceCurrent(*receipt.device_.optical_source_proof))return false;
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || state_->binding_failed ||
      receipt.issuer_ != state_->content_issuer ||
      receipt.issuer_.owner_before(state_->content_issuer) ||
      state_->content_issuer.owner_before(receipt.issuer_)) return false;
  return std::any_of(state_->bindings.begin(), state_->bindings.end(),
      [&](const auto& binding) {
        return binding && binding->native_id == receipt.native_id_ &&
            binding->cookie == receipt.cookie_ && binding->device == receipt.device_ &&
            (binding->device.optical_source_proof || state_->queue.IsDesired(binding->device));
      });
} catch (...) { return false; }
std::optional<NativeContentStorageDriver::ContentProcessingAdmission>
NativeContentStorageDriver::TryAdmitContentProcessing(
    const ContentProcessingReceipt& receipt, std::error_code& error) const noexcept try {
  error.clear();
  if (!global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  // Check the actual current optical mount/control block/title generation
  // after provider waits, BEFORE taking Driver metadata. GCRC keeps the VFS
  // identity stable through the bounded store; no stale weak backing is locked.
  if(receipt.device_.optical_source_proof &&
      !OpticalSourceCurrentLocked(*receipt.device_.optical_source_proof)) {
    error=std::make_error_code(std::errc::no_such_device);return {};
  }
  ContentProcessingAdmission guard(state_->mutex);
  if (!guard.lock_.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  if (state_->stopping || state_->retiring || state_->retired || state_->binding_failed ||
      !receipt.native_id_ || !receipt.cookie_ || receipt.driver_generation_ != generation_ ||
      receipt.issuer_ != state_->content_issuer ||
      receipt.issuer_.owner_before(state_->content_issuer) ||
      state_->content_issuer.owner_before(receipt.issuer_) ||
      !receipt.device_.source_volume || !receipt.device_.source_volume->available_cached() ||
      std::none_of(state_->bindings.begin(), state_->bindings.end(),
        [&](const auto& binding) {
          return binding && binding->native_id == receipt.native_id_ &&
              binding->cookie == receipt.cookie_ && binding->device == receipt.device_ &&
              (binding->device.optical_source_proof || state_->queue.IsDesired(binding->device));
        })) {
    error = std::make_error_code(std::errc::no_such_device); return {};
  }
  return std::optional<ContentProcessingAdmission>(std::move(guard));
} catch (...) { error = std::make_error_code(std::errc::io_error); return {}; }
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
std::optional<NativeContentStorageDriver::SelectedMediaProcessingReceipt>
NativeContentStorageDriver::CaptureSelectedMediaProcessing(
    std::string_view actual_native_path) const noexcept try {
  if(!IsCurrent() || !AuthorizeCurrentCaller())return {};
  std::lock_guard lock(state_->mutex);
  if(state_->stopping || state_->binding_failed)return {};
  std::optional<SelectedMediaProcessingReceipt> result;
  for(const auto& binding:state_->bindings) {
    if(!binding || !binding->cookie || !binding->native_id ||
        binding->device.guest_volume_root!=actual_native_path ||
        binding->device.type!=xam::DeviceType::USBMASS ||
        !binding->device.selected_media_proof ||
        !binding->device.selected_media_proof->SourceCurrentCached() ||
        !state_->queue.IsDesired(binding->device))continue;
    const auto& proof=binding->device.selected_media_proof;
    if(proof->source_id()!=binding->device.identity ||
        proof->generation()!=binding->device.generation || result)return {};
    result=SelectedMediaProcessingReceipt{this,generation_,binding->cookie,
        binding->native_id,proof};
  }
  return result;
} catch(...) {return {};}
bool NativeContentStorageDriver::SelectedMediaProcessingCurrent(
    const SelectedMediaProcessingReceipt& receipt) const noexcept try {
  if(receipt.issuer!=this || receipt.driver_generation!=generation_ ||
      !receipt.cookie || !receipt.native_id || !receipt.source ||
      !receipt.source->SourceCurrentCached() || !IsCurrent())return false;
  std::lock_guard lock(state_->mutex);
  return !state_->stopping && !state_->binding_failed &&
      std::any_of(state_->bindings.begin(),state_->bindings.end(),[&](const auto& binding){
        return binding && binding->native_id==receipt.native_id &&
            binding->cookie==receipt.cookie && binding->device.selected_media_proof==receipt.source &&
            state_->queue.IsDesired(binding->device);
      });
} catch(...) {return false;}

void NativeContentStorageDriver::ProtocolFailed() noexcept {
  const auto use = AcquireConfigUse();
  if (!use) return;
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
  // No allocation is needed to close ingress. This counted tail is declared
  // before Subscription, so every held source callback owner is gone before
  // stopped retirement can observe zero Config users.
  struct Tail {
    State* state;bool counted=false;
    ~Tail(){if(counted){std::lock_guard lock(state->mutex);
      --state->config_uses;state->drained.notify_all();}}
  } tail{state_.get()};
  std::shared_ptr<NativeContentStorageSource::Subscription> subscription;
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    if (state_->retired || state_->retiring) return;
    state_->CloseBindings();
    state_->callback = 0;
    state_->drained.notify_all();
    if (state_->config_uses == SIZE_MAX) return;
    ++state_->config_uses;tail.counted=true;
    subscription = state_->subscription;
  }
  if (subscription) subscription->BeginStop();
}
bool NativeContentStorageDriver::Drain() noexcept {
  const auto use = AcquireConfigUse(true);
  if (!use) {std::lock_guard lock(state_->mutex);return state_->retired;}
  BeginStop();
  if (config_.dispatcher && config_.dispatcher->IsCurrentWorker()) {
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
  if (!state_->drained.wait_for(lock, std::chrono::seconds(5), [&] {
    return !state_->active_calls && !state_->pending_callbacks;
  })) return false;
  if (state_->subscription != subscription) return false;
  state_->drain_proved = true;
  return true;
}
bool NativeContentStorageDriver::RetireAfterActualKernelStop(
    KernelState* expected_kernel) noexcept try {
  if (XThread::IsInThread() || global_critical_region::is_held_by_current_thread())
    return false;
  // Exact live owner through every resource move/free/destructor tail. The
  // private caller is Service after its factual whole CPU/IO/GPU stop proof.
  const auto owner = shared_from_this();
  {
    std::lock_guard lock(state_->mutex);
    if (!expected_kernel || expected_kernel != kernel_identity_) return false;
    if (state_->retired) return true;
    if (config_.kernel != expected_kernel || !state_->stopping ||
        !state_->drain_proved || state_->active_calls ||
        state_->pending_callbacks || state_->config_uses || state_->retiring)
      return false;
    // The zero-use check and refusal of new short Config users are one gate.
    state_->retiring = true;
  }
  struct ReopenRetirement {
    State* state;
    ~ReopenRetirement() {std::lock_guard lock(state->mutex);
      if (!state->retired) state->retiring = false;}
  } reopen{state_.get()};
  for (const auto& thread : expected_kernel->object_table()->GetObjectsByType<XThread>(
           XObject::Type::Thread))
    if (thread->is_running()) return false;
  Config config;
  std::shared_ptr<NativeContentStorageSource::Subscription> subscription;
  std::map<std::string,uint32_t> paths;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->retiring || state_->retired || !state_->stopping ||
        !state_->drain_proved || state_->active_calls ||
        state_->pending_callbacks || state_->config_uses ||
        config_.kernel != expected_kernel) return false;
    config = std::move(config_);config_ = {};
    subscription = std::move(state_->subscription);
    paths = std::move(state_->guest_paths);
    state_->guest_paths.clear();
    state_->retired = true;
  }
  {
    std::lock_guard lock(registry_mutex);
    const auto found = registry.find(kernel_identity_);
    if (found != registry.end() && found->second.lock().get() == this)
      registry.erase(found);
  }
  // These are ONLY allocations made by this driver's existing host path
  // producer. No native private pointer, native Close/free, device action or
  // producer callback is fabricated. Memory and providers are still alive.
  for (const auto& [path,address] : paths)
    if (address) expected_kernel->memory()->SystemHeapFree(address);
  // Config/source/subscription/code closure destruction is outside all locks.
  return true;
} catch (...) {return false;}
namespace test {
class NativeContentStorageDriverTestAccess {
 public:
  static std::shared_ptr<NativeContentStorageDriver> HostOnlyOwner(
      KernelState*,std::shared_ptr<void>);
  static uint32_t AllocateHostPath(
      const std::shared_ptr<NativeContentStorageDriver>&,const std::string&);
  static std::shared_ptr<void> HoldConfigUse(
      const std::shared_ptr<NativeContentStorageDriver>&);
  static bool Retire(const std::shared_ptr<NativeContentStorageDriver>&,KernelState*);
  static std::shared_ptr<const NativeOpticalStorageProof> CaptureOptical(
      const std::shared_ptr<NativeContentStorageDriver>&,std::string_view);
  static bool OpticalCurrent(const std::shared_ptr<NativeContentStorageDriver>&,
      const NativeOpticalStorageProof&);
};
}  // namespace test
std::shared_ptr<NativeContentStorageDriver>
test::NativeContentStorageDriverTestAccess::HostOnlyOwner(
    KernelState* kernel,std::shared_ptr<void> lifetime) {
  // Deliberately UNINSTALLED partial host owner. No dispatcher/domain/source
  // exists and no native Create, caller or device permission is granted.
  NativeContentStorageDriver::Config config;
  config.kernel=kernel;
  config.validate_context=[lifetime=std::move(lifetime)]{return false;};
  return std::shared_ptr<NativeContentStorageDriver>(
      new NativeContentStorageDriver(std::move(config)));
}
uint32_t test::NativeContentStorageDriverTestAccess::AllocateHostPath(
    const std::shared_ptr<NativeContentStorageDriver>& owner,const std::string& path) {
  const auto use=owner->AcquireConfigUse();
  if(!use)return 0;
  auto memory_guard=global_critical_region::AcquireDirect();
  std::lock_guard lock(owner->state_->mutex);
  return owner->GetOrCreateGuestPathLocked(path);
}
std::shared_ptr<void> test::NativeContentStorageDriverTestAccess::HoldConfigUse(
    const std::shared_ptr<NativeContentStorageDriver>& owner) {
  return owner->AcquireConfigUse();
}
bool test::NativeContentStorageDriverTestAccess::Retire(
    const std::shared_ptr<NativeContentStorageDriver>& owner,KernelState* kernel) {
  return owner->RetireAfterActualKernelStop(kernel);
}
std::shared_ptr<const NativeOpticalStorageProof>
test::NativeContentStorageDriverTestAccess::CaptureOptical(
    const std::shared_ptr<NativeContentStorageDriver>& owner,std::string_view root) {
  return owner->CaptureOpticalSource(root);
}
bool test::NativeContentStorageDriverTestAccess::OpticalCurrent(
    const std::shared_ptr<NativeContentStorageDriver>& owner,const NativeOpticalStorageProof& proof) {
  return owner->OpticalSourceCurrent(proof);
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
