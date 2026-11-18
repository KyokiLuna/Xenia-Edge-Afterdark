/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_TASK_CONTINUATION_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_TASK_CONTINUATION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/guest_runtime.h"

namespace xe::cpu { struct GuestCallResult; }
namespace xe::kernel {
class KernelState;
class NativeImportDomain;
class NativeSystemUiStartupCatalog;
class XThread;
class NativeMediaService;
class NativeProfileIdentityOwner;
class NativeSystemUiService;
namespace test {
class NativeSystemTaskContinuationTestAccess;
class NativeProfileIdentityTaskTestAccess;
}

// One startup and one explicit postboot diagnostic on real native XAM Tasks.
// This is not a general UI scheduler or a manufactured Task/TLS/app chain. Native Task
// Schedule/Wait/Close and the worker callback ABI remain authoritative.
class NativeSystemTaskContinuation {
 public:
  using Work = std::function<GuestRuntime::ExecutionResult(XThread&)>;
  // Installs the exact catalog's private callback through the CPU's verified
  // native Schedule-registration witness, before native workers are started.
  static std::shared_ptr<NativeSystemTaskContinuation> Create(
      std::shared_ptr<const NativeSystemUiStartupCatalog> catalog,
      std::shared_ptr<NativeImportDomain> domain);
  ~NativeSystemTaskContinuation();
  GuestRuntime::ExecutionResult InvokeAndWait(XThread& bootstrap, Work work);
  // One explicit postboot diagnostic only, serialized after successful startup.
  // Uses this same genuine native Schedule/Wait/Close callback owner.
  struct DiagnosticResult {
    GuestRuntime::ExecutionResult execution;
    // Actual scheduled handle, published only after real normal Wait+Close.
    // Work ran only after GetCurrentTask matched this exact handle.
    uint32_t completed_native_task = 0;
  };
  DiagnosticResult InvokePostBootDiagnosticAndWait(XThread& bootstrap, Work work);
  void BeginStop();
  // External host coordinator only. Must precede scheduler reclamation and
  // provider destruction; false means retention/fail-stop, not safe abandon.
  bool Drain();
  // These close/drain only this transport. The owning service must also join
  // its whole bootstrap host thread before the guest scheduler is reclaimed.
  static void BeginStopForKernel(KernelState*);
  static bool DrainForKernel(KernelState*);

 private:
  friend class test::NativeSystemTaskContinuationTestAccess;
  friend class test::NativeProfileIdentityTaskTestAccess;
  friend class NativeSystemUiService;
  // Exact Service-owned pre-CPU retention, never a native completion/drain.
  bool RetainedForActualKernelStop() const noexcept;
  bool RetireAfterActualKernelStop() noexcept;
  friend class NativeMediaService;
  friend class NativeProfileIdentityOwner;
  // Definitions live in the respective consumer TU. Only its actual retained
  // owner/serial can supply work; there is no public general-service callback.
  GuestRuntime::ExecutionResult InvokeOwnedMediaAndWait(
      NativeMediaService&, uint64_t serial, Work);
  GuestRuntime::ExecutionResult InvokeOwnedProfileIdentityAndWait(
      NativeProfileIdentityOwner&, uint64_t serial, Work);
  // Only finite admission/retention metadata, always under State::mutex in
  // production. Tests exercise this actual policy without Kernel/PPC mocks.
  struct Admission {
    enum class Role { kStartup, kPostBootDiagnostic, kOwnedMedia, kOwnedProfileIdentity };
    static bool IsOwned(Role role) {
      return role==Role::kOwnedMedia || role==Role::kOwnedProfileIdentity;
    }
    bool Claim(Role role) {
      if (closed || (stopping && !IsOwned(role)) || uncertain || active || owner) return false;
      if (role == Role::kStartup) {
        if (startup_claimed) return false;
        startup_claimed = true;
      } else if(role==Role::kPostBootDiagnostic) {
        if (!startup_succeeded || postboot_claimed) return false;
        postboot_claimed = true;
      } else if(!startup_succeeded) return false;
      current = role;
      active = true;
      return true;
    }
    bool BindOwner(uint32_t value, std::shared_ptr<void> lifetime) {
      if (!active || closed || (stopping && !IsOwned(current)) || uncertain || owner || !value || !lifetime)
        return false;
      ticket = value;
      owner = std::move(lifetime);
      return true;
    }
    bool MatchesTicket(uint32_t value) const {
      return active && owner && value && value == ticket;
    }
    // Exact existing Invocation owner only. The shared callback address is
    // transport, not identity source; typed identity keeps its own observer.
    bool MatchesSharedScheduleOwner(const std::shared_ptr<void>& actual_owner,
        std::span<const uint32_t> args,uint32_t callback,uint32_t output) const {
      return current!=Role::kOwnedProfileIdentity && !closed && !uncertain &&
          (!stopping || IsOwned(current)) && actual_owner &&
          owner.get()==actual_owner.get() && args.size()==4 && callback && output &&
          args[0]==callback && MatchesTicket(args[1]) && !args[2] && args[3]==output;
    }
    void Finish(bool healthy, bool succeeded) {
      if (!active) return;
      uncertain |= !healthy;
      if (current == Role::kStartup)
        startup_succeeded = !uncertain && succeeded;
      active = false;
      if (!uncertain) {
        owner.reset();
        ticket = 0;
      }
    }
    void Poison() { uncertain = true; }
    bool stopping = false, uncertain = false, active = false, closed = false;
    bool startup_claimed = false, startup_succeeded = false;
    bool postboot_claimed = false;
    Role current = Role::kStartup;
    uint32_t ticket = 0;
    std::shared_ptr<void> owner;
  };
  // Reads only this existing bound Invocation/Admission/registry, before
  // native Schedule and outside the identity owner's mutex/GCRC.
  bool MatchesSharedSchedule(XThread&,std::span<const uint32_t>,
      const std::shared_ptr<const NativeSystemUiStartupCatalog>&,
      const std::shared_ptr<NativeImportDomain>&) const;
  // Existing actual Invocation reference/execution only. Does not consult
  // allocation acceptance or grant source/readiness/new native work.
  bool MatchesTaskReleaseCaller(XThread&,uint32_t native_task) const;
  GuestRuntime::ExecutionResult InvokeAndWaitImpl(
      XThread&, Work, Admission::Role, uint32_t* completed_native_task = nullptr,
      std::function<bool()> owner_current = {},
      std::function<bool(XThread&,std::span<const uint32_t>)> before_schedule = {},
      std::function<void(const cpu::GuestCallResult&)> after_schedule = {},
      std::function<bool()> stop_retention_current = {},
      std::function<void()> identity_host_maintenance = {});
  struct State;
  explicit NativeSystemTaskContinuation(std::shared_ptr<State>);
  static void Enter(cpu::ppc::PPCContext*, KernelState*);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_SYSTEM_TASK_CONTINUATION_H_
