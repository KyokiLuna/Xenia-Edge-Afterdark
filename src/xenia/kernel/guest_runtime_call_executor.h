/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_GUEST_RUNTIME_CALL_EXECUTOR_H_
#define XENIA_KERNEL_GUEST_RUNTIME_CALL_EXECUTOR_H_
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/xobject.h"
namespace xe::kernel {
class KernelState;
class NativeSystemAllocationAuthority;
class NativeManagedFormatIo;
class UserModule;
class XThread;
enum class NativeSystemUiPublicCall : uint16_t;
// Exact loaded generation, bounded plan loader and checked synchronous call
// executor. Loading/bridges do not initialize a runtime or invent Task context.
class GuestRuntimeCallExecutor
    : public std::enable_shared_from_this<GuestRuntimeCallExecutor> {
 public:
  static std::shared_ptr<GuestRuntimeCallExecutor> Load(
      KernelState*, const object_ref<UserModule>&, const std::filesystem::path&,
      std::string* error);
  ~GuestRuntimeCallExecutor();
  const ValidatedGuestRuntimePlan& validated() const;
  KernelState* kernel() const;
  object_ref<UserModule> module() const;
  std::shared_ptr<GuestRuntime> heap_runtime() const;
  uint64_t load_generation() const;
  // Explicit allowlisted bridge set, quiescent binding before native workers.
  bool InstallTrustedBridges(std::string* error);
  GuestRuntime::InitStatus InitializeHeap(
      XThread&, const std::shared_ptr<NativeSystemAllocationAuthority>& = {});
  GuestRuntime::ExecutionResult ExecuteInitializationFunction(
      XThread&, const GuestRuntime::Function&);
  // Exact validated deferred prototype/arity/contract only. Initialization
  // calls stay SYSTEM; public deferred UI/Input calls stay on original actual
  // TITLE/SYSTEM caller, without process/TLS/current-Task substitution.
  GuestRuntime::ExecutionResult ExecuteDeferredSystemFunction(
      XThread&, std::string_view identity, std::span<const uint32_t> arguments);
  GuestRuntime::ExecutionResult ExecuteDeferredOnOriginalCaller(
      XThread&, std::string_view identity, std::span<const uint32_t> arguments);
  // Only the fixed public profile may select wide register positions. A caller
  // cannot supply a width mask or silently widen guest pointers/DWORDs.
  GuestRuntime::ExecutionResult ExecuteDeferredPublicRegistersOnOriginalCaller(
      XThread&, NativeSystemUiPublicCall, std::span<const uint64_t> arguments);

 private:
  friend class NativeSystemUiStartupRuntime;
  friend class NativeManagedFormatIo;
  // Private exact managed ingress only. Unlike the generic public method,
  // this requires the retained IO owner's real per-XThread execution scope.
  GuestRuntime::ExecutionResult ExecuteManagedFormatRegistersOnOriginalCaller(
      XThread&, NativeSystemUiPublicCall, std::span<const uint64_t>);
  // Frontend transaction only, after its actual native Task reference check.
  GuestRuntime::ExecutionResult ExecuteDeferredNativeTaskFunction(
      XThread&, std::string_view, std::span<const uint32_t>);
  struct State;
  explicit GuestRuntimeCallExecutor(std::unique_ptr<State>);
  GuestRuntime::ExecutionResult Execute(XThread&, const GuestRuntime::Function&,
                                        std::span<const uint32_t>,
                                        bool system_only);
  GuestRuntime::ExecutionResult ExecuteRegisters(XThread&,
                                                 const GuestRuntime::Function&,
                                                 std::span<const uint64_t>,
                                                 uint8_t uint64_register_mask,
                                                 bool system_only);
  std::unique_ptr<State> state_;
};
}  // namespace xe::kernel
#endif
