/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace xe::kernel {

// Admission/rundown policy only. This neither executes PPC nor proves that all
// native control entry paths are intercepted. No public route is enabled.
// Kernel must retain this object until its actual stopped teardown boundary.
class NativeNotificationPreferenceCoordinator {
 private:
  struct State;
  struct Invocation;

 public:
  // Preserve the actual native DWORDs, including noncanonical nonzero values.
  // Order is C4/C8/CC/D0: show, context5, sound, IPTV.
  using Options = std::array<uint32_t, 4>;
  static constexpr uint32_t kOptionMask = 0x4230;
  static Options OptionsFromRetailFlags(uint32_t);
  static uint32_t DisabledOptionBits(const Options&);
  static bool SameEnabledOptions(const Options&, const Options&);

  enum class Phase {
    kFallback,
    kQuiescingFallback,
    kNativeMayRun,
    kNativeObserved,
    kUncertain,
    kStopping
  };
  enum class NativeCall { kGet, kSetThenReadback };
  // Values must come from the existing shared console authority, not a second
  // preferences store. A successful read is not a durability acknowledgement.
  struct PersistenceObservation {
    std::optional<uint32_t> retail_flags;
    std::error_code read_error;
    bool update_observed = false;
    std::error_code update_error;
    bool published = false;
    bool durable = false;
  };
  struct Projection {
    Options native_options;
    PersistenceObservation persistence;
    bool observed_flags_match = false;
  };
  struct Snapshot {
    Phase phase;
    size_t fallback_operations;
    bool native_operation;
    bool native_entered;
    bool uncertain;
    // Last fully returned/read-verified native outputs only. This is a cached
    // observation, not proof against unseen internal native setters.
    std::optional<Projection> last_native;
    std::optional<PersistenceObservation> latest_persistence;
  };

  class FallbackOperation {
   public:
    ~FallbackOperation();
    FallbackOperation(const FallbackOperation&) = delete;
    FallbackOperation& operator=(const FallbackOperation&) = delete;

   private:
    friend class NativeNotificationPreferenceCoordinator;
    explicit FallbackOperation(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
  };

  class NativeOperation {
   public:
    ~NativeOperation();
    NativeOperation(const NativeOperation&) = delete;
    NativeOperation& operator=(const NativeOperation&) = delete;
    // Immediately before actual guest entry, outside every coordinator lock.
    // Stop/cancellation before this point denies without side effects.
    bool MarkEntering();
    bool CancelBeforeEntry();
    // Adapter must have a normal native return and verified ALL four writes
    // from genuine GetOptions (also after SetOptions). Residual r3 is ignored.
    // Store outcome is observed separately; a failed native void RMW cannot be
    // replaced with a fabricated success or a private-cache rollback.
    bool CompleteReadback(Options, PersistenceObservation);
    // Native invocation DID return normally, but owner/output observation was
    // unavailable (e.g. lazy allocation failed). Allow a later genuine getter,
    // never fallback or automatic replay of a possibly applied setter.
    bool CompleteWithoutReadback(PersistenceObservation);

   private:
    friend class NativeNotificationPreferenceCoordinator;
    NativeOperation(std::shared_ptr<State>, std::shared_ptr<Invocation>);
    std::shared_ptr<State> state_;
    std::shared_ptr<Invocation> invocation_;
  };

  // The existing ConsoleSettingsStore/XConfig owner. No IO callback is invoked
  // here; callers execute their real shared-authority operation with a permit.
  explicit NativeNotificationPreferenceCoordinator(
      std::shared_ptr<void> console_owner);
  bool OwnsConsoleAuthority(const std::shared_ptr<void>&) const;
  std::unique_ptr<FallbackOperation> TryAcquireFallback();
  // Must precede FIRST native root PPC, not merely the first observed getter.
  bool BeginNativeTransition(std::shared_ptr<void> cohort_owner);
  bool FallbackDrained() const;
  // Invoke immediately before first native heap/bootstrap execution, only once
  // all previously admitted fallback IO completed. Irreversible from here.
  bool MarkNativeMayRun();
  bool CancelTransitionBeforeNative();
  // Owner is the ACTUAL service/domain/code/current-caller/argument lifetime
  // reservation, independently validated by Root's future native adapter.
  // This class cannot manufacture that reservation or provide a host PPC turn.
  std::unique_ptr<NativeOperation> TryAcquireNative(
      NativeCall, std::shared_ptr<void> actual_invocation_owner);
  // Host-only sink AFTER the real RetailFlags RMW, attributed to the validated
  // native SetOptions caller. Internal native setters are not redirected or
  // serialized. Invalidate the old projection until a genuine getter returns.
  // False means the caller observed native activity outside the handover;
  // its owning service must fail closed rather than reopen fallback.
  bool ObserveNativeSetterPersistence(PersistenceObservation);
  std::optional<Projection> ObservedProjection() const;
  Snapshot snapshot() const;
  void BeginStop();
  // Metadata check only; external host must also join the real caller/worker.
  // Uncertain guest execution retains all owners until actual Kernel teardown.
  bool SafelyDrained() const;

 private:
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
