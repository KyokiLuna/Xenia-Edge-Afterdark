#pragma once
#include <cstdint>
#include <span>
#include "xenia/kernel/native_system_ui_public_calls.h"
namespace xe::kernel {
class KernelState;
struct NativeNotificationResult {
  enum class Disposition {
    kUnavailableBeforeNative,
    kInvalidArguments,
    kNativeReturned,
    kNativeSuppressed,
    kNativeRejected,
    kExecutionUncertain
  };
  Disposition disposition = Disposition::kUnavailableBeforeNative;
  uint32_t value = 0;
  bool may_use_fallback() const {
    return disposition == Disposition::kUnavailableBeforeNative;
  }
  bool inputs_must_remain_until_kernel_stop() const {
    return disposition == Disposition::kExecutionUncertain;
  }
};
// Typed original-caller gateway. It creates no native worker, changes no TLS,
// allocates no modal lease, and owns no independent popup or sound engine.
class NativeNotificationService {
 public:
  // Native clones caller text/image/optional payload before a normal queue
  // return. Caller-owned guest storage must survive this synchronous call;
  // uncertain execution retains it until actual Kernel stop, never replayed.
  static NativeNotificationResult TryInvokeOnOriginalCaller(
      KernelState*, NativeSystemUiPublicCall,
      std::span<const uint64_t>) noexcept;
  static bool ArgumentsValid(NativeSystemUiPublicCall,
                             std::span<const uint64_t>) noexcept;
  // Native queue result0 includes coalescence,1 includes deliberate
  // suppression; neither authorizes a second popup/cue. Void controls ignore
  // residualr3.
  static NativeNotificationResult InterpretReturned(
      NativeSystemUiPublicCall, uint32_t actual_native_value) noexcept;
};
}  // namespace xe::kernel
