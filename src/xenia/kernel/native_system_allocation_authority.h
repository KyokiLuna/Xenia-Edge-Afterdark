/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_ALLOCATION_AUTHORITY_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_ALLOCATION_AUTHORITY_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "xenia/base/physical_range_leases.h"
#include "xenia/kernel/guest_runtime.h"
#include "xenia/kernel/xobject.h"

namespace xe::kernel {
class KernelState;
class NativeFrameOwnerSession;
class NativeImportDomain;
class NativeSystemUiService;
class UserModule;
class XThread;
class NativeSystemFrameDelegation;
namespace test {
class NativeSystemAllocationTestAccess;
}

// Retained through the actual allocator call. Denial never becomes legacy
// unowned allocation. This grants allocation attribution, never GPU execution.
struct NativeSystemPhysicalAllocationDecision {
  enum class Kind { kLegacy, kDenied, kOwned };
  Kind kind = Kind::kLegacy;
  memory::PhysicalAllocationOwner owner;
  std::shared_ptr<void> operation;
};

class NativeSystemInvocationState;
class NativeSystemInvocationScope {
 public:
  NativeSystemInvocationScope() = default;
  NativeSystemInvocationScope(NativeSystemInvocationScope&&) noexcept;
  NativeSystemInvocationScope& operator=(
      NativeSystemInvocationScope&&) noexcept;
  ~NativeSystemInvocationScope();
  NativeSystemInvocationScope(const NativeSystemInvocationScope&) = delete;
  NativeSystemInvocationScope& operator=(const NativeSystemInvocationScope&) =
      delete;
  explicit operator bool() const;
  // Call on the same healthy, actual XThread after the native invocation and
  // CPU native work for this invocation completed. A frozen/submitted Source
  // may outlive it; the same real Task reenters before explicit retirement.
  // false, abandonment
  // or thread exit closes the cohort before detaching its thread admission.
  bool Complete(bool succeeded);

 private:
  friend class NativeSystemAllocationAuthority;
  friend class NativeSystemFrameDelegation;
  explicit NativeSystemInvocationScope(
      std::shared_ptr<NativeSystemInvocationState> state);
  void Abandon() noexcept;
  std::shared_ptr<NativeSystemInvocationState> state_;
};

// One Kernel/runtime/root generation, reserved before heap/CRT. Kernel owns
// the tombstone, runtime and root code lease. No HUD/VK module is pinned here.
// The historical owner.session_id field identifies this allocation cohort;
// it is not a native Task pointer, worker ID, foreground generation or Source.
class NativeSystemAllocationAuthority
    : public std::enable_shared_from_this<NativeSystemAllocationAuthority> {
 public:
  using Owner = memory::PhysicalAllocationOwner;
  using BootstrapScope = NativeSystemInvocationScope;
  static std::shared_ptr<NativeSystemAllocationAuthority> Reserve(
      KernelState* kernel, const object_ref<UserModule>& root,
      const std::shared_ptr<GuestRuntime>& runtime);
  ~NativeSystemAllocationAuthority();
  const Owner& owner() const;
  uint64_t root_load_generation() const;
  KernelState* kernel() const;
  object_ref<UserModule> root_module() const;
  uint32_t bootstrap_thread_id() const;
  // Only the active immutable GuestRuntime plan entry may enter pre-domain
  // bootstrap. SYSTEM alone, arbitrary metadata or a copied Function cannot.
  BootstrapScope EnterBootstrap(XThread& thread,
                                const GuestRuntime::Function& function);
  // Once only, after heap Ready; exact registered native root/load identity.
  // Does not initialize native Task/TLS, or advertise service readiness.
  bool BindDomain(const std::shared_ptr<NativeImportDomain>& domain);
  bool IsAccepting() const;
  bool MatchesDomain(const std::shared_ptr<NativeImportDomain>& domain) const;
  // Must be invoked from the verified native render/record boundary, with
  // the original guest LR. Uses installed public 1B6, which adds a reference.
  // The caller cannot supply a Task pointer or manufacture its TLS fields.
  std::shared_ptr<NativeSystemFrameDelegation> DelegateCurrentRenderTask(
      XThread& thread, uint32_t return_address);
  void CloseAdmission() noexcept;
  // Same actual producer-stop proof contract as Session. Each bound graph
  // independently requires its same-backend stop proof if GPU was attempted.
  // No PPC release occurs here; native Task references are reclaimed with the
  // stopped Kernel memory, never replayed on a stopped XThread.
  bool RetireAfterKernelStop(const std::function<bool(const Owner&)>& proof);

 private:
  friend class KernelState;
  friend class NativeSystemFrameDelegation;
  friend class NativeSystemInvocationScope;
  friend class NativeSystemInvocationState;
  friend class NativeFrameOwnerSession;
  friend class test::NativeSystemAllocationTestAccess;
  struct State;
  explicit NativeSystemAllocationAuthority(std::unique_ptr<State> state);
  NativeSystemPhysicalAllocationDecision DecidePhysicalAllocation(
      XThread& thread, uint32_t return_address);
  bool ValidateCurrent(XThread& thread, uint32_t return_address,
                       bool require_domain, bool require_system = true) const;
  uint64_t ReserveSourceSerial();
  std::shared_ptr<void> AcquireFrameWriter();
  std::unique_ptr<State> state_;
};

// Explicit permission for the real native render Task to use allocations
// tagged earlier by its cohort. Existing Memory owner/pin lookup is reused:
// no allocation is transferred, retagged or adopted from an unowned block.
class NativeSystemFrameDelegation
    : public std::enable_shared_from_this<NativeSystemFrameDelegation> {
 public:
  using InvocationScope = NativeSystemInvocationScope;
  ~NativeSystemFrameDelegation();
  const memory::PhysicalAllocationOwner& owner() const;
  const std::shared_ptr<NativeFrameOwnerSession>& session() const;
  InvocationScope EnterCurrentTask(XThread& thread, uint32_t return_address);
  // Only explicit healthy-context native CloseHandle; false retains the
  // control record. Requires no active invocation, update, Source or GPU use.
  bool CloseOnCurrentTask(XThread& thread, uint32_t return_address);
  bool MatchesCurrentThread(const KernelState* kernel, const XThread* thread,
                            uint32_t thread_id, uint32_t process_type) const;
  // Does not call PPC; used for binding metadata / retained plane liveness.
  bool IsAccepting() const;
  std::shared_ptr<NativeSystemAllocationAuthority> authority() const;

 private:
  friend class NativeSystemAllocationAuthority;
  friend class NativeSystemInvocationScope;
  friend class NativeSystemInvocationState;
  friend class NativeFrameOwnerSession;
  struct State;
  explicit NativeSystemFrameDelegation(std::unique_ptr<State> state);
  uint64_t ReserveSourceSerial();
  std::shared_ptr<void> AcquireFrameWriter();
  std::unique_ptr<State> state_;
};

// Internal XThread bridge; none of these manufacture a Task/TLS or Source.
std::shared_ptr<NativeFrameOwnerSession> NativeSystemInvocationSession(
    const std::shared_ptr<NativeSystemInvocationState>&);
bool NativeSystemInvocationIsBootstrap(
    const std::shared_ptr<NativeSystemInvocationState>&,
    const NativeSystemAllocationAuthority*, const XThread*);
void RevokeNativeSystemInvocationState(
    const std::shared_ptr<NativeSystemInvocationState>&) noexcept;

}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_SYSTEM_ALLOCATION_AUTHORITY_H_
