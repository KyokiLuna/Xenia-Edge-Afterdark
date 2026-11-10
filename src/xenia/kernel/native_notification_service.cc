#include "xenia/kernel/native_notification_service.h"
#include "xenia/kernel/native_system_ui_service.h"
namespace xe::kernel {
namespace {
using Call = NativeSystemUiPublicCall;
using Disposition = NativeNotificationResult::Disposition;
bool Queue(Call call) {
  return call == Call::kNotifyQueue || call == Call::kNotifyQueueEx;
}
}  // namespace
bool NativeNotificationService::ArgumentsValid(
    Call call, std::span<const uint64_t> arguments) noexcept {
  const auto* profile = NativeSystemUiPublicProfile(call);
  if (!profile || profile->family != NativeSystemPublicFamily::kNotifications ||
      !NativeSystemUiPublicArgumentsValid(call, arguments)) {
    return false;
  }
  if (!Queue(call)) {
    if (call == Call::kNotifyGetOptions) {
      for (const auto pointer : arguments) {
        if (!pointer || (pointer & 3)) {
          return false;
        }
      }
    }
    return true;
  }
  if (arguments[1] > 3 && arguments[1] != 0xFF) {
    return false;
  }
  // The original public qword reaches native unchanged; the real wrapper stores
  // its lowDWORD in FE21013. It is not placement and highbits are not pointers.
  if (uint32_t(arguments[2]) > 3 || !arguments[3] || (arguments[3] & 1)) {
    return false;
  }
  if (call == Call::kNotifyQueueEx) {
    if (arguments[5] & 1) {
      return false;  // Optional UTF16 image pointer.
    }
    if (arguments[6] & 1) {
      const uint32_t bytes = uint32_t(arguments[7] >> 32);
      if (!uint32_t(arguments[7]) || !bytes || bytes > 128) {
        return false;
      }
    }
  }
  return true;
}
NativeNotificationResult NativeNotificationService::InterpretReturned(
    Call call, uint32_t value) noexcept {
  if (Queue(call)) {
    if (int32_t(value) < 0) {
      return {Disposition::kNativeRejected, value};
    }
    return {value == 1 ? Disposition::kNativeSuppressed
                       : Disposition::kNativeReturned,
            value};
  }
  if (call == Call::kNotifyDelay && value) {
    return {Disposition::kNativeRejected, value};
  }
  if (call == Call::kNotifyPosition || call == Call::kNotifySetOptions ||
      call == Call::kNotifyGetOptions) {
    return {Disposition::kNativeReturned, 0};
  }
  return {Disposition::kNativeReturned, value};
}
NativeNotificationResult NativeNotificationService::TryInvokeOnOriginalCaller(
    KernelState* kernel, Call call,
    std::span<const uint64_t> arguments) noexcept {
  if (!ArgumentsValid(call, arguments)) {
    return {Disposition::kInvalidArguments, 0x80070057};
  }
  const auto* profile = NativeSystemUiPublicProfile(call);
  // Cataloguing verified calls is not runtime/source/cue/prefs admission. This
  // family stays closed until Root integrates the actual prerequisite owners.
  if (!kernel || !NativeSystemPublicFamilyRouteEnabled(profile->family)) {
    return {};
  }
  const auto result =
      NativeSystemUiService::TryInvokePublicRegistersOnOriginalCaller(
          kernel, call, arguments);
  using NativeDisposition = NativeSystemUiPublicCallResult::Disposition;
  switch (result.disposition) {
    case NativeDisposition::kInactive:
      return {};
    case NativeDisposition::kReturned:
      return InterpretReturned(call, result.value);
    case NativeDisposition::kExecutionFailed:
      return {Disposition::kExecutionUncertain, result.value};
    case NativeDisposition::kDenied:
      return {Disposition::kNativeRejected, result.value};
    case NativeDisposition::kHostFallback:
      // Nonmodal notifications never acquire or propagate a modal HostLease.
      // A future mistaken creating-UI classification fails closed here.
      return {Disposition::kNativeRejected, 0x80004005};
  }
  return {Disposition::kNativeRejected, 0x80004005};
}
}  // namespace xe::kernel
