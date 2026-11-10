/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_ui_notifications.h"
#include "xenia/base/native_system_ui_trace.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {
constexpr uint32_t kBroadcast = 0x816A93C8;
constexpr uint32_t kBroadcastBytes = 276;
constexpr uint32_t kFinalUnlockReturn = 0x816A94D0;
constexpr uint32_t kNativeListenerLock = 0x81A82204;
constexpr std::array<uint8_t, 32> kBroadcastSha256 = {
    0x4C, 0x14, 0x76, 0x77, 0xA4, 0x9B, 0x31, 0x06, 0x69, 0xBA, 0x49,
    0xF3, 0xF2, 0xA5, 0x43, 0x22, 0x71, 0xA0, 0x07, 0x80, 0x67, 0xA5,
    0x6E, 0xC9, 0x29, 0x18, 0xBB, 0xA7, 0x91, 0xB7, 0x80, 0x41};
std::mutex installations_mutex;
std::map<KernelState*, std::weak_ptr<NativeSystemUiNotifications>>
    installations;

bool BodyMatches(KernelState& kernel, UserModule& module) {
  if (module.GetProcAddressByOrdinal(0x28E) != kBroadcast ||
      kernel.memory()->QueryRangeAccess(kBroadcast, kBroadcastBytes) ==
          memory::PageAccess::kNoAccess) {
    return false;
  }
  for (uint32_t offset = 0; offset < kBroadcastBytes; offset += 4) {
    const uint32_t address = kBroadcast + offset;
    xex2_section_type section;
    if (kernel.GetUserModuleForAddress(address).get() != &module ||
        !module.xex_module()->GetPageSectionType(address, &section) ||
        section != XEX_SECTION_CODE ||
        module.xex_module()->TranslateCode(address) !=
            kernel.memory()->TranslateVirtual<const uint8_t*>(address)) {
      return false;
    }
  }
  std::array<uint8_t, 32> digest;
  mbedtls_sha256(kernel.memory()->TranslateVirtual<const uint8_t*>(kBroadcast),
                 kBroadcastBytes, digest.data(), 0);
  return digest == kBroadcastSha256;
}
[[noreturn]] void FailStop() noexcept {
  try {
    XELOGE(
        "Native SYS_UI observer: admitted shim frames not drained before "
        "Kernel teardown; retaining resources until process exit");
    xe::FlushLog();
  } catch (...) {
  }
  std::quick_exit(EXIT_FAILURE);
}
}  // namespace

struct NativeSystemUiNotifications::State {
  static constexpr size_t kCapacity = 64;
  struct Entry {
    NativeSystemUiVisibilityEvent event;
    std::shared_ptr<void> lifetime;
    bool ready = false, released = false;
  };
  Config config;
  KernelState* kernel = nullptr;
  std::shared_ptr<KernelState::UserModuleExecutionLease> module_lease;
  // Test peer may supply only host observers; no fake Kernel/native object.
  std::function<void(const NativeSystemUiVisibilityEvent&)> deliver;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::array<std::optional<Entry>, kCapacity> entries;
  size_t head = 0, pending = 0, active_preflight = 0;
  size_t unlock_revocations = 0, active_revocation = 0;
  size_t active_completions = 0;
  bool revocation_published = false;
  uint64_t next_sequence = 1;
  Snapshot status;
  bool installed = false, delivering = false, stopping = false, retired = false;
  // These counters alone may be touched under the actual native spinlock.
  // Never format/log/allocate or acquire another lock on that path.
  std::array<std::atomic<uint64_t>, 16> trace_counts{};
  xe::NativeSystemUiTraceLimiter summary_trace;
  uint64_t trace_domain_id = 0, trace_root_generation = 0;
  uint32_t trace_root_hmodule = 0;
  void TraceCount(size_t reason) noexcept {
    if (!cvars::native_system_ui_trace) {
      return;
    }
    if constexpr (!std::atomic<uint64_t>::is_always_lock_free) {
      return;
    }
    auto value = trace_counts[reason].load(std::memory_order_relaxed);
    while (value != UINT64_MAX &&
           !trace_counts[reason].compare_exchange_weak(
               value, value + 1, std::memory_order_relaxed)) {
    }
  }
  void TraceSummary(uint32_t stage) noexcept {
    if (!cvars::native_system_ui_trace) {
      return;
    }
    summary_trace.Emit(
        "sysui9", "producer_summary",
        {{"stage", stage},
         {"observe", trace_counts[0].load()},
         {"not_accepting", trace_counts[1].load()},
         {"wrong_context", trace_counts[2].load()},
         {"wrong_root_domain", trace_counts[3].load()},
         {"no_lifetime", trace_counts[4].load()},
         {"bad_stack", trace_counts[5].load()},
         {"body_memory_lock_mismatch", trace_counts[6].load()},
         {"other_notification", trace_counts[7].load()},
         {"bad_boolean", trace_counts[8].load()},
         {"reserve_closed", trace_counts[9].load()},
         {"reserve_capacity", trace_counts[10].load()},
         {"reserved", trace_counts[11].load()},
         {"delivery_attempt", trace_counts[12].load()},
         {"delivery_returned", trace_counts[13].load()},
         {"delivery_threw", trace_counts[14].load()},
         {"unlock_complete", trace_counts[15].load()}},
        trace_domain_id
            ? std::initializer_list<xe::NativeSystemUiTraceLimiter::
                                        Field>{{"domain_id", trace_domain_id},
                                               {"root_hmodule",
                                                trace_root_hmodule},
                                               {"root_load_generation",
                                                trace_root_generation}}
            : std::initializer_list<xe::NativeSystemUiTraceLimiter::Field>{});
  }
};

NativeSystemUiNotifications::NativeSystemUiNotifications(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemUiNotifications::~NativeSystemUiNotifications() = default;

std::shared_ptr<NativeSystemUiNotifications>
NativeSystemUiNotifications::CreateQueueForTest(
    std::function<void(const NativeSystemUiVisibilityEvent&)> deliver,
    std::function<void()> revoked) {
  auto state = std::make_shared<State>();
  state->installed = true;
  state->deliver = std::move(deliver);
  state->config.host_revoked = std::move(revoked);
  return std::shared_ptr<NativeSystemUiNotifications>(
      new NativeSystemUiNotifications(std::move(state)));
}

std::shared_ptr<NativeSystemUiNotifications>
NativeSystemUiNotifications::Create(Config config) {
  if (!config.catalog || !config.domain || !config.host_observer ||
      !config.host_revoked || !config.domain->active() ||
      !config.catalog->allocation()->MatchesDomain(config.domain) ||
      !config.catalog->allocation()->IsAccepting()) {
    return {};
  }
  auto calls = config.catalog->calls();
  auto root = calls->module();
  auto* kernel = calls->kernel();
  if (!kernel || config.domain->root() !=
                     NativeImportDomain::Member{root->hmodule_ptr(),
                                                calls->load_generation()}) {
    return {};
  }
  auto lease = kernel->AcquireUserModuleExecutionLease(root);
  auto operation = config.domain->Acquire();
  if (!lease || !operation) {
    return {};
  }
  {
    auto global = global_critical_region::Acquire();
    if (!root->native_import_ready() ||
        root->load_generation() != calls->load_generation() ||
        !BodyMatches(*kernel, *root)) {
      return {};
    }
  }
  auto state = std::make_shared<State>();
  state->config = std::move(config);
  state->trace_domain_id = state->config.domain->id();
  state->trace_root_hmodule = state->config.domain->root().hmodule;
  state->trace_root_generation = state->config.domain->root().generation;
  state->kernel = kernel;
  state->module_lease = std::move(lease);
  state->status.accepting = true;
  // Native listeners already received this exact broadcast. Forward only to
  // emulator TITLE listener objects, then observe visibility once on the host.
  state->deliver = [kernel, observer = state->config.host_observer](
                       const NativeSystemUiVisibilityEvent& event) {
    const auto* emulator = kernel->emulator();
    const auto foreground = emulator->title_execution_state().snapshot();
    if (event.foreground_generation() && emulator->OwnsTitleExecution(kernel) &&
        foreground.generation == event.foreground_generation() &&
        foreground.phase != TitleExecutionState::Phase::kUnloaded) {
      kernel->BroadcastNativeSystemUiNotification(event.visible());
    }
    observer(event);
  };
  auto result = std::shared_ptr<NativeSystemUiNotifications>(
      new NativeSystemUiNotifications(state));
  struct KernelRetirement {
    explicit KernelRetirement(
        std::shared_ptr<NativeSystemUiNotifications> value)
        : owner(std::move(value)) {}
    std::shared_ptr<NativeSystemUiNotifications> owner;
    ~KernelRetirement() { owner->RetireAfterKernelStop(); }
  };
  // Retain before publishing discovery; dropping a partially started service
  // never removes the ordered stop/drain target while native workers can run.
  kernel->RetainNativeResourceUntilTeardown(
      std::make_shared<KernelRetirement>(result));
  {
    std::lock_guard lock(installations_mutex);
    if (!installations[kernel].expired()) {
      result->BeginStop();
      return {};
    }
    installations[kernel] = result;
    std::lock_guard state_lock(state->mutex);
    state->installed = true;
  }
  return result;
}

std::shared_ptr<NativeSystemUiNotifications>
NativeSystemUiNotifications::Lookup(KernelState* kernel) {
  std::lock_guard lock(installations_mutex);
  const auto found = installations.find(kernel);
  return found == installations.end() ? nullptr : found->second.lock();
}
NativeSystemUiNotifications::Snapshot NativeSystemUiNotifications::snapshot()
    const noexcept {
  std::lock_guard lock(state_->mutex);
  auto result = state_->status;
  result.pending = state_->pending;
  result.accepting = state_->installed && !state_->stopping &&
                     !state_->retired && !result.poisoned;
  return result;
}
bool NativeSystemUiNotifications::IsAccepting() const noexcept {
  std::shared_ptr<NativeImportDomain> domain;
  std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->installed || state_->stopping || state_->retired ||
        state_->status.poisoned) {
      return false;
    }
    domain = state_->config.domain;
    catalog = state_->config.catalog;
  }
  return domain && catalog && domain->active() &&
         catalog->allocation()->IsAccepting();
}
std::optional<bool> NativeSystemUiNotifications::CurrentVisibility(
    KernelState* kernel) noexcept {
  const auto observer = Lookup(kernel);
  if (!observer) {
    return std::nullopt;
  }
  const auto current = observer->snapshot();
  const auto* emulator = kernel->emulator();
  const auto foreground = emulator->title_execution_state().snapshot();
  return current.accepting && current.known && current.foreground_generation &&
                 emulator->OwnsTitleExecution(kernel) &&
                 current.foreground_generation == foreground.generation &&
                 foreground.phase != TitleExecutionState::Phase::kUnloaded
             ? std::optional(current.visible)
             : std::nullopt;
}

NativeSystemUiNotifications::UnlockObservation::UnlockObservation(
    std::shared_ptr<NativeSystemUiNotifications> owner, uint64_t sequence)
    : owner_(std::move(owner)), sequence_(sequence) {}
NativeSystemUiNotifications::UnlockObservation::UnlockObservation(
    UnlockObservation&& other) noexcept
    : owner_(std::move(other.owner_)),
      sequence_(std::exchange(other.sequence_, 0)) {}
NativeSystemUiNotifications::UnlockObservation&
NativeSystemUiNotifications::UnlockObservation::operator=(
    UnlockObservation&& other) noexcept {
  if (this != &other) {
    if (owner_) {
      owner_->Finish(sequence_, false);
    }
    owner_ = std::move(other.owner_);
    sequence_ = std::exchange(other.sequence_, 0);
  }
  return *this;
}
NativeSystemUiNotifications::UnlockObservation::~UnlockObservation() {
  if (owner_) {
    owner_->Finish(sequence_, false);
  }
}
void NativeSystemUiNotifications::UnlockObservation::Complete() noexcept {
  if (owner_) {
    struct CompletionFrame {
      std::shared_ptr<NativeSystemUiNotifications> owner;
      std::shared_ptr<State> state;
      ~CompletionFrame() {
        // Drop the actual complete-call holder before its admission count.
        owner.reset();
        std::lock_guard lock(state->mutex);
        --state->active_completions;
        state->changed.notify_all();
      }
    } frame{std::move(owner_), {}};
    frame.state = frame.owner->state_;
    {
      std::lock_guard lock(frame.state->mutex);
      if (frame.state->active_completions == SIZE_MAX) {
        FailStop();
      }
      ++frame.state->active_completions;
    }
    frame.state->TraceCount(15);
    static xe::NativeSystemUiTraceLimiter trace;
    if (frame.state->installed) {
      trace.Emit(
          "sysui9", "native_unlock_complete",
          {{"revocation_token", sequence_ == 0}},
          sequence_
              ? std::initializer_list<xe::NativeSystemUiTraceLimiter::
                                          Field>{{"native_notify_sequence",
                                                  sequence_}}
              : std::initializer_list<xe::NativeSystemUiTraceLimiter::Field>{});
    }
    frame.owner->PublishRevocation();
    frame.owner->Finish(std::exchange(sequence_, 0), true);
    frame.owner->PublishRevocation();
  }
}

NativeSystemUiNotifications::UnlockObservation
NativeSystemUiNotifications::BeforeNativeListenerUnlock(
    KernelState* kernel, const cpu::ppc::PPCContext* context) noexcept {
  // High-frequency generic kernel path: do not even look up the service for
  // unrelated spinlocks/callers. Exact root/hash/domain checks follow below.
  if (!context || uint32_t(context->lr) != kFinalUnlockReturn ||
      uint32_t(context->r[3]) != kNativeListenerLock) {
    return {};
  }
  try {
    auto observer = Lookup(kernel);
    return observer ? observer->Observe(context) : UnlockObservation{};
  } catch (...) {
    if (auto observer = Lookup(kernel)) {
      return observer->RevokeAfterUnlock();
    }
    return {};
  }
}

NativeSystemUiNotifications::UnlockObservation
NativeSystemUiNotifications::Observe(const cpu::ppc::PPCContext* context) {
  state_->TraceCount(0);
  if (!XThread::IsInThread() || !IsAccepting()) {
    state_->TraceCount(1);
    return {};
  }
  struct Preflight {
    std::shared_ptr<State> state;
    ~Preflight() {
      if (state) {
        std::lock_guard lock(state->mutex);
        --state->active_preflight;
        state->changed.notify_all();
      }
    }
  } preflight;
  Config config;
  KernelState* kernel;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->retired || state_->status.poisoned ||
        state_->active_preflight == SIZE_MAX) {
      return {};
    }
    config = state_->config;
    kernel = state_->kernel;
    ++state_->active_preflight;
    preflight.state = state_;
  }
  auto* thread = XThread::GetCurrentThread();
  if (thread->kernel_state() != kernel ||
      thread->thread_state()->context() != context ||
      thread->native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM) {
    state_->TraceCount(2);
    return {};
  }
  const auto caller = kernel->ResolveNativeImportCaller(uint32_t(context->lr));
  const auto root = config.catalog->calls()->module();
  if (!caller.scoped || caller.domain != config.domain ||
      caller.module.get() != root.get() ||
      root->load_generation() != config.catalog->calls()->load_generation()) {
    state_->TraceCount(3);
    return {};
  }
  struct Lifetime {
    NativeImportDomain::Operation domain;
    std::shared_ptr<KernelState::UserModuleExecutionLease> module;
  };
  auto lifetime = std::make_shared<Lifetime>();
  lifetime->domain = config.domain->Acquire();
  lifetime->module = kernel->AcquireUserModuleExecutionLease(root);
  if (!lifetime->domain || !lifetime->module) {
    state_->TraceCount(4);
    return {};
  }
  const uint32_t stack = uint32_t(context->r[1]);
  if ((stack & 15) || stack < thread->stack_limit() ||
      uint64_t(stack) + 0x80 > thread->stack_base()) {
    state_->TraceCount(5);
    return RevokeAfterUnlock();
  }
  uint32_t notification;
  {
    auto global = global_critical_region::Acquire();
    auto* memory = kernel->memory();
    if (!BodyMatches(*kernel, *root) ||
        memory->QueryRangeAccess(stack, 0x80) ==
            memory::PageAccess::kNoAccess ||
        memory->QueryRangeAccess(kNativeListenerLock, 4) ==
            memory::PageAccess::kNoAccess ||
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
            kNativeListenerLock)) != uint32_t(context->r[13])) {
      state_->TraceCount(6);
      return RevokeAfterUnlock();
    }
    notification =
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(stack + 0x50));
  }
  // SYS_UI9 uses exactly Boolean data. Other native notifications continue
  // through their original native listener registry without a host mirror.
  const uint32_t data = uint32_t(context->r[29]);
  if (notification != kXNotificationSystemUI) {
    state_->TraceCount(7);
    return {};
  }
  if (data > 1) {
    state_->TraceCount(8);
    return RevokeAfterUnlock();
  }
  NativeSystemUiVisibilityEvent event;
  event.owner_ = config.catalog->allocation()->owner();
  event.domain_id_ = config.domain->id();
  event.root_generation_ = root->load_generation();
  const auto* emulator = kernel->emulator();
  const auto foreground = emulator->title_execution_state().snapshot();
  event.foreground_generation_ =
      emulator->OwnsTitleExecution(kernel) &&
              foreground.phase != TitleExecutionState::Phase::kUnloaded
          ? foreground.generation
          : 0;
  event.visible_ = bool(data);
  return Reserve(std::move(event), std::move(lifetime));
}

NativeSystemUiNotifications::UnlockObservation
NativeSystemUiNotifications::Reserve(NativeSystemUiVisibilityEvent event,
                                     std::shared_ptr<void> lifetime) {
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || state_->retired || state_->status.poisoned) {
    state_->TraceCount(9);
    return {};
  }
  if (state_->pending == State::kCapacity ||
      state_->next_sequence == UINT64_MAX) {
    state_->TraceCount(10);
    state_->status.poisoned = true;
    state_->stopping = true;
    ++state_->unlock_revocations;
    return UnlockObservation(shared_from_this(), 0);
  }
  const uint64_t sequence = state_->next_sequence++;
  event.sequence_ = sequence;
  const size_t tail = (state_->head + state_->pending) % State::kCapacity;
  state_->entries[tail].emplace(
      State::Entry{std::move(event), std::move(lifetime), false, false});
  ++state_->pending;
  state_->TraceCount(11);
  return UnlockObservation(shared_from_this(), sequence);
}

NativeSystemUiNotifications::UnlockObservation
NativeSystemUiNotifications::RevokeAfterUnlock() {
  std::lock_guard lock(state_->mutex);
  state_->status.poisoned = true;
  state_->stopping = true;
  if (state_->unlock_revocations == SIZE_MAX) {
    FailStop();
  }
  ++state_->unlock_revocations;
  return UnlockObservation(shared_from_this(), 0);
}
void NativeSystemUiNotifications::PublishRevocation() noexcept {
  const std::function<void()>* revoked = nullptr;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->installed || (!state_->stopping && !state_->status.poisoned) ||
        state_->revocation_published) {
      return;
    }
    state_->revocation_published = true;
    // The active count excludes Retire from moving Config; no allocating
    // std::function copy is needed on this post-release/teardown path.
    revoked = &state_->config.host_revoked;
    ++state_->active_revocation;
  }
  try {
    if (*revoked) {
      (*revoked)();
    }
  } catch (...) {
    std::lock_guard lock(state_->mutex);
    state_->status.poisoned = true;
  }
  std::lock_guard lock(state_->mutex);
  --state_->active_revocation;
  state_->changed.notify_all();
}

void NativeSystemUiNotifications::Finish(uint64_t sequence,
                                         bool released) noexcept {
  {
    std::lock_guard lock(state_->mutex);
    if (!sequence) {
      if (state_->unlock_revocations) {
        --state_->unlock_revocations;
      }
      if (!released) {
        state_->status.poisoned = true;
        state_->stopping = true;
      }
      state_->changed.notify_all();
      return;
    }
    bool found = false;
    for (auto& entry : state_->entries) {
      if (entry && entry->event.sequence() == sequence) {
        if (entry->ready) {
          return;
        }
        entry->ready = true;
        entry->released = released;
        found = true;
        break;
      }
    }
    if (!found) {
      return;
    }
    if (!released) {
      state_->status.poisoned = true;
      state_->stopping = true;
    }
    if (state_->delivering) {
      return;
    }
    state_->delivering = true;
  }
  // One drainer preserves reservation/native-lock order even if a later host
  // release shim returns first. No guest spinlock or observer mutex is held
  // while broadcasting to HLE listeners or invoking the trusted host sink.
  for (;;) {
    std::optional<State::Entry> entry;
    bool deliver = false;
    {
      std::lock_guard lock(state_->mutex);
      auto& head = state_->entries[state_->head];
      if (!head || !head->ready) {
        state_->delivering = false;
        state_->changed.notify_all();
        return;
      }
      entry = std::move(head);
      head.reset();
      deliver = !state_->status.poisoned && entry->released;
      if (deliver) {
        // Publish the native state before HLE delivery, so a listener created
        // as the broadcast unlocks cannot receive a stale startup seed.
        state_->status.known = true;
        state_->status.visible = entry->event.visible();
        state_->status.last_sequence = entry->event.sequence();
        state_->status.foreground_generation =
            entry->event.foreground_generation();
      }
    }
    bool succeeded = false;
    if (deliver) {
      state_->TraceCount(12);
      static xe::NativeSystemUiTraceLimiter trace;
      if (state_->installed) {
        trace.Emit(
            "sysui9", "native_delivery",
            {{"native_notify_sequence", entry->event.sequence()},
             {"domain_id", entry->event.domain_id()},
             {"root_load_generation", entry->event.root_load_generation()},
             {"kernel_epoch", entry->event.owner().kernel_epoch},
             {"runtime_module_generation",
              entry->event.owner().module_generation},
             {"foreground_generation", entry->event.foreground_generation()},
             {"visible", entry->event.visible()}});
      }
      try {
        state_->deliver(entry->event);
        succeeded = true;
        state_->TraceCount(13);
      } catch (...) {
        state_->TraceCount(14);
        // A partial multi-listener delivery is not replayed. Close authority;
        // the service must stop this cohort before another public UI request.
      }
    }
    entry.reset();  // Module/domain references drop before Drain can succeed.
    {
      std::lock_guard lock(state_->mutex);
      if (deliver && !succeeded) {
        state_->status.poisoned = true;
        state_->stopping = true;
      }
      state_->head = (state_->head + 1) % State::kCapacity;
      --state_->pending;
      state_->changed.notify_all();
    }
  }
}

void NativeSystemUiNotifications::BeginStop() noexcept {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->changed.notify_all();
  }
  PublishRevocation();
  state_->TraceSummary(1);
}
bool NativeSystemUiNotifications::Drain() noexcept {
  if (XThread::IsInThread()) {
    return false;
  }
  std::unique_lock lock(state_->mutex);
  if (!state_->stopping) {
    return false;
  }
  const bool drained =
      state_->changed.wait_for(lock, std::chrono::seconds(5), [&] {
        return !state_->pending && !state_->delivering &&
               !state_->active_preflight && !state_->unlock_revocations &&
               !state_->active_revocation && !state_->active_completions;
      });
  return drained && !state_->status.poisoned;
}
void NativeSystemUiNotifications::RetireAfterKernelStop() noexcept {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    if (state_->pending || state_->delivering || state_->active_preflight ||
        state_->unlock_revocations || state_->active_revocation ||
        state_->active_completions ||
        (state_->installed && !state_->revocation_published)) {
      FailStop();
    }
    state_->retired = true;
  }
  state_->TraceSummary(2);
  Config config;
  std::shared_ptr<KernelState::UserModuleExecutionLease> lease;
  std::function<void(const NativeSystemUiVisibilityEvent&)> deliver;
  {
    std::lock_guard lock(state_->mutex);
    config = std::move(state_->config);
    lease = std::move(state_->module_lease);
    deliver = std::move(state_->deliver);
  }
  std::lock_guard lock(installations_mutex);
  const auto found = installations.find(state_->kernel);
  if (found != installations.end() && found->second.lock().get() == this) {
    installations.erase(found);
  }
}
}  // namespace xe::kernel
