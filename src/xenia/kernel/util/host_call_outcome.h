/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_UTIL_HOST_CALL_OUTCOME_H_
#define XENIA_KERNEL_UTIL_HOST_CALL_OUTCOME_H_
#include <exception>
#include <utility>
#include "xenia/kernel/util/guest_control_flow.h"
#include "xenia/xbox.h"
#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif
namespace xe::kernel {
// Host invocation, not the operation's own status/bytes/durability receipt.
// Queued unstarted work at actual scheduler stop has no resumed guest caller.
enum class HostCallOutcome { kUnstarted, kReturned, kExecutionFailed, kAdmissionFailed };
template<class Fn> [[nodiscard]] HostCallOutcome InvokeHostCall(Fn&& fn) {
  try { std::forward<Fn>(fn)(); return HostCallOutcome::kReturned; }
#if !XE_PLATFORM_WIN32
  catch(const FiberReentryException&) { throw; }
#endif
#if defined(__GLIBCXX__)
  catch(abi::__forced_unwind&) { throw; }
#endif
  catch(...) { return HostCallOutcome::kExecutionFailed; }
}
inline X_STATUS HostCallFailureStatus(HostCallOutcome outcome) {
  if (outcome == HostCallOutcome::kUnstarted) { return X_STATUS_CANCELLED; }
  if (outcome == HostCallOutcome::kAdmissionFailed) {
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }
  return X_STATUS_UNSUCCESSFUL;
}
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_UTIL_HOST_CALL_OUTCOME_H_
