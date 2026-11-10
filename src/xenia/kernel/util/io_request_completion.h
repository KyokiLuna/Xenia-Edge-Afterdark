/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_UTIL_IO_REQUEST_COMPLETION_H_
#define XENIA_KERNEL_UTIL_IO_REQUEST_COMPLETION_H_
#include <cstdint>
#include "xenia/xbox.h"

namespace xe::kernel::util {
enum class InlineIoFailurePolicy {
  // Existing read/write policy is deliberately unchanged in this slice.
  kRejectWithoutCompletion,
  // Flush/control prevalidation has already accepted the request. A real
  // backend error still publishes final status/count and signals completion.
  kCompleteAdmitted,
};

// Shared by the actual SubmitRequest completion path. Publication happens
// before APC/port/file/event notification; a failed IOSB store is reported
// through remaining completion channels without fabricating an IOSB write.
// Tests use actual Memory/backend failures here but do not mint a Kernel,
// guest thread or claim valid-Kernel event/APC/port execution from this seam.
template <typename Transfer, typename Publish, typename Notify>
X_STATUS CompleteIoRequest(bool posted, bool cancelled,
                           InlineIoFailurePolicy policy, Transfer transfer,
                           Publish publish, Notify notify) {
  uint32_t bytes = 0;
  X_STATUS status = cancelled ? X_STATUS_CANCELLED : transfer(&bytes);
  if (!posted && XFAILED(status) &&
      policy == InlineIoFailurePolicy::kRejectWithoutCompletion) {
    return status;
  }
  if (XFAILED(status) && policy == InlineIoFailurePolicy::kCompleteAdmitted) {
    bytes = 0;
  }
  const auto published = publish(status, bytes);
  if (XFAILED(published)) {
    status = published;
    bytes = 0;
  }
  notify(status, bytes);
  return status;
}
}  // namespace xe::kernel::util
#endif
