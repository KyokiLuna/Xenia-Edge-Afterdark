/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_notification_preference_coordinator.h"

#include <limits>
#include <mutex>
#include <utility>

namespace xe::kernel {
struct NativeNotificationPreferenceCoordinator::Invocation {
  NativeCall call;
  std::shared_ptr<void> owner;
  bool entered = false;
};
struct NativeNotificationPreferenceCoordinator::State {
  explicit State(std::shared_ptr<void> owner) : console(std::move(owner)) {}
  std::mutex mutex;
  std::shared_ptr<void> console, cohort;
  Phase phase = Phase::kFallback;
  bool uncertain = false;
  size_t fallback = 0;
  std::shared_ptr<Invocation> active;
  // Preallocated active record is moved here on uncertain return/abandonment;
  // no allocation or release of argument/caller ownership follows uncertainty.
  std::shared_ptr<Invocation> quarantine;
  std::optional<Projection> last_native;
  std::optional<PersistenceObservation> latest_persistence;
};

NativeNotificationPreferenceCoordinator::Options
NativeNotificationPreferenceCoordinator::OptionsFromRetailFlags(
    uint32_t flags) {
  return {uint32_t(!(flags & 0x10)), uint32_t(!(flags & 0x200)),
          uint32_t(!(flags & 0x20)), uint32_t(!(flags & 0x4000))};
}
uint32_t NativeNotificationPreferenceCoordinator::DisabledOptionBits(
    const Options& options) {
  return (options[0] ? 0 : 0x10) | (options[1] ? 0 : 0x200) |
         (options[2] ? 0 : 0x20) | (options[3] ? 0 : 0x4000);
}
bool NativeNotificationPreferenceCoordinator::SameEnabledOptions(
    const Options& a, const Options& b) {
  for (size_t i = 0; i < a.size(); ++i) {
    if (bool(a[i]) != bool(b[i])) {
      return false;
    }
  }
  return true;
}
NativeNotificationPreferenceCoordinator::
    NativeNotificationPreferenceCoordinator(std::shared_ptr<void> console_owner)
    : state_(std::make_shared<State>(std::move(console_owner))) {}
bool NativeNotificationPreferenceCoordinator::OwnsConsoleAuthority(
    const std::shared_ptr<void>& owner) const {
  std::lock_guard lock(state_->mutex);
  return owner && state_->console == owner;
}
NativeNotificationPreferenceCoordinator::FallbackOperation::FallbackOperation(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeNotificationPreferenceCoordinator::FallbackOperation::
    ~FallbackOperation() {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  --state_->fallback;
}
std::unique_ptr<NativeNotificationPreferenceCoordinator::FallbackOperation>
NativeNotificationPreferenceCoordinator::TryAcquireFallback() {
  // Allocate before admission so allocation failure cannot leak an IO count.
  auto operation =
      std::unique_ptr<FallbackOperation>(new FallbackOperation({}));
  std::lock_guard lock(state_->mutex);
  if (!state_->console || state_->phase != Phase::kFallback ||
      state_->fallback == std::numeric_limits<size_t>::max()) {
    return {};
  }
  operation->state_ = state_;
  ++state_->fallback;
  return operation;
}
bool NativeNotificationPreferenceCoordinator::BeginNativeTransition(
    std::shared_ptr<void> cohort) {
  std::lock_guard lock(state_->mutex);
  if (!cohort || !state_->console || state_->phase != Phase::kFallback) {
    return false;
  }
  state_->cohort = std::move(cohort);
  state_->phase = Phase::kQuiescingFallback;
  return true;
}
bool NativeNotificationPreferenceCoordinator::FallbackDrained() const {
  std::lock_guard lock(state_->mutex);
  return state_->phase == Phase::kQuiescingFallback && !state_->fallback;
}
bool NativeNotificationPreferenceCoordinator::MarkNativeMayRun() {
  std::lock_guard lock(state_->mutex);
  if (state_->phase != Phase::kQuiescingFallback || state_->fallback) {
    return false;
  }
  state_->phase = Phase::kNativeMayRun;
  return true;
}
bool NativeNotificationPreferenceCoordinator::CancelTransitionBeforeNative() {
  std::shared_ptr<void> release;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->phase != Phase::kQuiescingFallback || state_->fallback) {
      return false;
    }
    release = std::move(state_->cohort);
    state_->phase = Phase::kFallback;
  }
  return true;
}
NativeNotificationPreferenceCoordinator::NativeOperation::NativeOperation(
    std::shared_ptr<State> state, std::shared_ptr<Invocation> invocation)
    : state_(std::move(state)), invocation_(std::move(invocation)) {}
NativeNotificationPreferenceCoordinator::NativeOperation::~NativeOperation() {
  std::shared_ptr<Invocation> release;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->active != invocation_) {
      return;
    }
    if (invocation_->entered) {
      state_->quarantine = std::move(state_->active);
      state_->uncertain = true;
      if (state_->phase != Phase::kStopping) {
        state_->phase = Phase::kUncertain;
      }
    } else {
      release = std::move(state_->active);
    }
  }
}
std::unique_ptr<NativeNotificationPreferenceCoordinator::NativeOperation>
NativeNotificationPreferenceCoordinator::TryAcquireNative(
    NativeCall call, std::shared_ptr<void> owner) {
  if (call != NativeCall::kGet && call != NativeCall::kSetThenReadback) {
    return {};
  }
  auto invocation =
      std::make_shared<Invocation>(Invocation{call, std::move(owner)});
  auto operation =
      std::unique_ptr<NativeOperation>(new NativeOperation(state_, invocation));
  std::lock_guard lock(state_->mutex);
  if (!invocation->owner || state_->active || state_->quarantine ||
      (state_->phase != Phase::kNativeMayRun &&
       state_->phase != Phase::kNativeObserved) ||
      (state_->phase == Phase::kNativeMayRun && call != NativeCall::kGet)) {
    return {};
  }
  state_->active = std::move(invocation);
  return operation;
}
bool NativeNotificationPreferenceCoordinator::NativeOperation::MarkEntering() {
  std::lock_guard lock(state_->mutex);
  if (state_->active != invocation_ || invocation_->entered ||
      (state_->phase != Phase::kNativeMayRun &&
       state_->phase != Phase::kNativeObserved)) {
    return false;
  }
  invocation_->entered = true;
  return true;
}
bool NativeNotificationPreferenceCoordinator::NativeOperation::
    CancelBeforeEntry() {
  std::shared_ptr<Invocation> release;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->active != invocation_ || invocation_->entered) {
      return false;
    }
    release = std::move(state_->active);
  }
  return true;
}
bool NativeNotificationPreferenceCoordinator::NativeOperation::CompleteReadback(
    Options options, PersistenceObservation persistence) {
  std::shared_ptr<Invocation> release;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->active != invocation_ || !invocation_->entered) {
      return false;
    }
    const bool matches =
        !persistence.read_error && persistence.retail_flags &&
        SameEnabledOptions(options,
                           OptionsFromRetailFlags(*persistence.retail_flags));
    state_->latest_persistence = persistence;
    state_->last_native = Projection{options, std::move(persistence), matches};
    if (state_->phase != Phase::kStopping) {
      state_->phase = Phase::kNativeObserved;
    }
    release = std::move(state_->active);
  }
  return true;
}
bool NativeNotificationPreferenceCoordinator::NativeOperation::
    CompleteWithoutReadback(PersistenceObservation persistence) {
  std::shared_ptr<Invocation> release;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->active != invocation_ || !invocation_->entered) {
      return false;
    }
    state_->latest_persistence = std::move(persistence);
    if (state_->phase != Phase::kStopping) {
      state_->phase = Phase::kNativeMayRun;
    }
    release = std::move(state_->active);
  }
  return true;
}
bool NativeNotificationPreferenceCoordinator::ObserveNativeSetterPersistence(
    PersistenceObservation persistence) {
  std::lock_guard lock(state_->mutex);
  // Retain the actual persistence outcome even when an unexpected native
  // caller forces the authority transition into its uncertain state.
  state_->latest_persistence = std::move(persistence);
  if (state_->phase == Phase::kFallback ||
      state_->phase == Phase::kQuiescingFallback) {
    state_->uncertain = true;
    state_->phase = Phase::kUncertain;
    return false;
  }
  if (state_->phase == Phase::kNativeObserved) {
    state_->phase = Phase::kNativeMayRun;
  }
  return true;
}
std::optional<NativeNotificationPreferenceCoordinator::Projection>
NativeNotificationPreferenceCoordinator::ObservedProjection() const {
  std::lock_guard lock(state_->mutex);
  if (state_->phase != Phase::kNativeObserved || state_->quarantine ||
      (state_->active && state_->active->entered &&
       state_->active->call == NativeCall::kSetThenReadback)) {
    return {};
  }
  return state_->last_native;
}
NativeNotificationPreferenceCoordinator::Snapshot
NativeNotificationPreferenceCoordinator::snapshot() const {
  std::lock_guard lock(state_->mutex);
  return {state_->phase,
          state_->fallback,
          bool(state_->active),
          state_->active && state_->active->entered,
          state_->uncertain,
          state_->last_native,
          state_->latest_persistence};
}
void NativeNotificationPreferenceCoordinator::BeginStop() {
  std::lock_guard lock(state_->mutex);
  state_->phase = Phase::kStopping;
}
bool NativeNotificationPreferenceCoordinator::SafelyDrained() const {
  std::lock_guard lock(state_->mutex);
  return state_->phase == Phase::kStopping && !state_->fallback &&
         !state_->active && !state_->quarantine && !state_->uncertain;
}
}  // namespace xe::kernel
