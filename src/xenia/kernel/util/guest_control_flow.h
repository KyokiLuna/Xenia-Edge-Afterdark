/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_UTIL_GUEST_CONTROL_FLOW_H_
#define XENIA_KERNEL_UTIL_GUEST_CONTROL_FLOW_H_
#include <cstdint>
#include "xenia/base/platform.h"
namespace xe::kernel {
#if !XE_PLATFORM_WIN32
// Original XThread::Reenter control-flow sentinel. Keep its type/namespace
// while allowing host callback boundaries to preserve it explicitly.
struct FiberReentryException { uint32_t address; };
#endif
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_UTIL_GUEST_CONTROL_FLOW_H_
