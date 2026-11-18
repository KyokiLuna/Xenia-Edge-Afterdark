/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_BASE_NATIVE_UI_BACKEND_ROUTE_H_
#define XENIA_BASE_NATIVE_UI_BACKEND_ROUTE_H_
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace xe {
// CPU-only arbitration before a native UI-creating queue or PPC attempt.
// Neither this metadata nor a host lease authorizes native/GPU execution.
class NativeUiBackendRoute final {
 private:
  struct State;

 public:
  struct Cohort {
    uint64_t domain = 0, root_load_generation = 0, backend_epoch = 0;
    uint64_t foreground_generation = 0, title_load_generation = 0;
    uint32_t root_hmodule = 0, title_hmodule = 0;
    bool operator==(const Cohort&) const = default;
    bool valid() const;
  };
  // An opaque CPU epoch owned by one actual Kernel. It carries no native
  // root, module, readiness or execution authority. A replacement Kernel owns
  // a different epoch even when the guest title ID and paths are identical.
  struct HostOnlyEpoch final {};
  enum class Input { kUnknown, kProvenNoInitialSwap, kReady };
  enum class Decision { kDenied, kNative, kHost };

  // A CPU admission receipt only: the actual prepared native route still
  // supplies all native readiness/cohort/execution proof. Held across the
  // creating call, then transferred to an actual native visibility observation
  // or the already-selected host fallback lease, never guessed from a timer.
  class NativeCreationLease final {
   public:
    ~NativeCreationLease();
    void MarkExecutionAttempt() noexcept;
    void Finish(bool queued) noexcept;
   private:
    friend class NativeUiBackendRoute;
    NativeCreationLease(std::shared_ptr<State>, uint64_t revision);
    std::shared_ptr<State> state_;
    const uint64_t visibility_revision_;
    bool finished_ = true;
    bool execution_attempted_ = false;
  };

  // This object and callbacks capture no Kernel/provider strong holders.
  // A weak notification callback enters the existing Transport operation and
  // is covered by the same Entry rundown before touching Kernel state.
  // Queued UI factories carry copied values and frontend pointers only, and
  // acquire Entry BEFORE dereferencing any such pointer. Stop cancels a pure
  // CPU completion waiter; it does not wait for an inert queued factory.
  class HostLease final : public std::enable_shared_from_this<HostLease> {
   public:
    class Entry final {
     public:
      ~Entry();
      Entry(const Entry&) = delete;
      Entry& operator=(const Entry&) = delete;

     private:
      friend class HostLease;
      friend class NativeUiBackendRoute;
      Entry(std::shared_ptr<State>, std::shared_ptr<HostLease>);
      std::shared_ptr<State> state_;
      std::shared_ptr<HostLease> lease_;
      const Entry* previous_ = nullptr;
      static thread_local const Entry* current_;
    };
    ~HostLease();
    const Cohort& cohort() const { return cohort_; }
    uint64_t serial() const { return serial_; }
    // Same-thread/LIFO scope only; never capture in a queued callback or move
    // across threads. The original host caller may retain Entry across a short
    // runtime-capture callback return while publishing its owned UI operation.
    std::unique_ptr<Entry> TryEnter();
    bool cancelled() const;
    // One operation's pure CPU waiter. May be called immediately if stop won.
    void SetCancellation(std::function<void()>);
    void Cancel() noexcept;
    // End only an explicitly host-only operation. Late old-dialog callbacks
    // become inert; the delayed SYS_UI notification does not retain admission.
    void FinishHostOnly() noexcept;
    // Actual modal/result completion for either selected host lane. Existing
    // visibility=false publication must happen before this call. Retire only
    // this exact host slot, make delayed old references inert, then finish the
    // transferred Kernel ingress outside this route's mutex. Never changes a
    // native cohort, native seal/readiness or actual native visibility.
    void FinishHostOperation() noexcept;
    void RetainHostIngress(std::shared_ptr<NativeCreationLease>);
    // Existing physical Xenia overlay notification, not native SYSUI9.
    void SetVisible(bool) noexcept;

   private:
    friend class NativeUiBackendRoute;
    HostLease(std::shared_ptr<State>, Cohort, uint64_t);
    std::shared_ptr<State> state_;
    Cohort cohort_;
    uint64_t serial_ = 0;
    bool cancelled_ = false;
    std::function<void()> cancel_;
    std::shared_ptr<NativeCreationLease> host_ingress_;
  };
  struct Admission {
    Decision decision = Decision::kDenied;
    std::shared_ptr<HostLease> host;
  };
  explicit NativeUiBackendRoute(std::function<void(bool)> visibility = {});
  Admission Admit(const Cohort&, Input, bool allow_host);
  Admission AdmitHostOnly(const std::shared_ptr<const HostOnlyEpoch>&,
                          uint64_t title_generation);
  std::shared_ptr<NativeCreationLease> ReserveNativeCreation(
      const std::shared_ptr<const HostOnlyEpoch>&);
  // Only the actual Kernel native SYS_UI observer supplies this publication.
  // Generic host BroadcastNotification must never call it.
  void ObserveNativeUiVisibility(bool visible) noexcept;
  // Retain at input-call ENTRY through actual PPC and filtering. A modal that
  // closes during that call must not leak its final cached transition.
  std::shared_ptr<HostLease> CurrentHost(const Cohort&) const;
  void BeginStop() noexcept;
  bool Drain(std::chrono::steady_clock::time_point deadline) noexcept;

 private:
  std::shared_ptr<State> state_;
};
}  // namespace xe
#endif
