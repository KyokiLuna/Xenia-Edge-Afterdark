/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_MANAGED_FORMAT_IO_H_
#define XENIA_KERNEL_NATIVE_MANAGED_FORMAT_IO_H_
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/guest_runtime.h"
namespace xe::kernel {
class GuestRuntimeCallExecutor;
class KernelState;
class NativeImportDomain;
class NativeManagedFormatSelection;
class UserModule;
class XThread;
enum class NativeSystemUiPublicCall : uint16_t;
namespace xam {
class ContentManager;
}
namespace test {
class NativeManagedFormatIoTestAccess;
}
// One exact managed request family beneath the original native XMsg envelope.
// It owns host work, never a replacement Task, event, APC or overlap object.
class NativeManagedFormatIo final
    : public std::enable_shared_from_this<NativeManagedFormatIo> {
 public:
  struct Config {
    KernelState* kernel = nullptr;
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeImportDomain> domain;
    // Actual manager retained by the service until this owner has drained.
    xam::ContentManager* content = nullptr;
    // cancel=true permits cancellation of this already-admitted selection;
    // it must not require its Progress object to remain a fresh Start choice.
    std::function<bool(XThread&, const NativeManagedFormatSelection&,
                       bool cancel)>
        authorize_original_caller;
    // Current actual SYSTEM root/domain/context only, cached metadata/RAM.
    // Never filesystem observation, registry refresh or an executing guest
    // call.
    std::function<bool(cpu::ppc::PPCContext&, UserModule&)>
        authorize_native_context;
  };
  class Installation final {
   public:
    ~Installation();
    Installation(const Installation&) = delete;
    Installation& operator=(const Installation&) = delete;

   private:
    friend class NativeManagedFormatIo;
    explicit Installation(std::shared_ptr<NativeManagedFormatIo> value)
        : owner_(std::move(value)) {}
    std::shared_ptr<NativeManagedFormatIo> owner_;
  };
  static std::shared_ptr<NativeManagedFormatIo> Create(Config);
  static std::unique_ptr<Installation> Install(
      std::shared_ptr<NativeManagedFormatIo>);
  static std::shared_ptr<NativeManagedFormatIo> Lookup(KernelState*);
  ~NativeManagedFormatIo();
  GuestRuntime::ExecutionResult StartOriginal(
      XThread&, std::shared_ptr<const NativeManagedFormatSelection>,
      std::span<const uint64_t>);
  std::optional<GuestRuntime::ExecutionResult> CancelOriginal(
      XThread&, std::span<const uint64_t>);
  // Called only at the real per-function CallNative entry/normal-return seams.
  // The supplied module/address are checked against the retained actual root.
  static void ObserveOwnedPacketEntry(cpu::ppc::PPCContext&, UserModule&,
                                      uint32_t) noexcept;
  static void ObserveOwnedPacketReturn(cpu::ppc::PPCContext&, UserModule&,
                                       uint32_t) noexcept;
  static void ObserveCancelEntry(cpu::ppc::PPCContext&, UserModule&,
                                 uint32_t) noexcept;
  static void ObserveCompletionEntry(cpu::ppc::PPCContext&, UserModule&,
                                     uint32_t) noexcept;
  static void ObserveCompletionReturn(cpu::ppc::PPCContext&, UserModule&,
                                      uint32_t) noexcept;
  // A value replaces ONLY authenticated owned managed calls at 81684578.
  // nullopt leaves an unrelated native call untouched, never authorizes it.
  static std::optional<uint32_t> TryHandleFormat(cpu::ppc::PPCContext&,
                                                 UserModule&,
                                                 uint32_t) noexcept;
  void BeginStop() noexcept;
  // External host coordinator only, while the guest scheduler is still alive.
  // False retains the installation/requests: it is not safe abandonment.
  bool Drain() noexcept;

 private:
  friend class GuestRuntimeCallExecutor;
  friend class test::NativeManagedFormatIoTestAccess;
  // Actual production pairing state. Entry identities are not unique serials:
  // a second eligible entry poisons the pair instead of completing stale work.
  struct Pair {
    bool entered = false, bound = false, poisoned = false;
    uint32_t sp = 0, lr = 0, output = 0, packet = 0;
    bool Enter(uint32_t stack, uint32_t caller, uint32_t out) {
      if (entered || bound || !stack || !caller || !out) {
        poisoned = true;
        return false;
      }
      entered = true;
      sp = stack;
      lr = caller;
      output = out;
      return true;
    }
    bool Bind(uint32_t stack, uint32_t caller, uint32_t value) {
      if (poisoned || !entered || bound || stack != sp || caller != lr ||
          !value) {
        poisoned = true;
        return false;
      }
      bound = true;
      packet = value;
      return true;
    }
  };
  static bool CurrentScopeMatches(GuestRuntimeCallExecutor*, XThread&,
                                  NativeSystemUiPublicCall,
                                  std::span<const uint64_t>);
  struct State;
  explicit NativeManagedFormatIo(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif
