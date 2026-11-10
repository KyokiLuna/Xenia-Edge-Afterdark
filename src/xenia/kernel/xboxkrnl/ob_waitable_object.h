/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_OB_WAITABLE_OBJECT_H_
#define XENIA_KERNEL_XBOXKRNL_OB_WAITABLE_OBJECT_H_

#include <cstdint>

namespace xe {
class Memory;
}

namespace xe::kernel::xboxkrnl {

enum class WaitableObjectResolutionKind {
  kNullObject,
  kUnreadableHeader,
  kUnreadableType,
  kOffsetOverflow,
  kUnreadableTarget,
  kOffset,
  kAbsolute,
  kKernelDefault,
};

struct WaitableObjectResolution {
  WaitableObjectResolutionKind kind = WaitableObjectResolutionKind::kNullObject;
  uint32_t body = 0;
  uint32_t object_type = 0;
  uint32_t default_object = 0;
  uint32_t waitable = 0;
};

// Resolves a borrowed Object Manager body, not a raw dispatcher header or an
// object handle. The caller retains the owning object throughout this call and
// any use of the returned borrowed pointer. No guest bytes or references
// change. The exact low-address HLE default object is supplied by
// KernelGuestGlobals, never inferred from arbitrary mapped pointers. See the
// implementation for the explicitly qualified retail high-pointer convention.
WaitableObjectResolution ResolveWaitableObject(
    Memory& memory, uint32_t body, uint32_t exact_kernel_default_object);

}  // namespace xe::kernel::xboxkrnl

#endif  // XENIA_KERNEL_XBOXKRNL_OB_WAITABLE_OBJECT_H_
