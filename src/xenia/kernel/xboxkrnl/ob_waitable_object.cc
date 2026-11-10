/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/ob_waitable_object.h"

#include <cstring>
#include <limits>

#include "xenia/kernel/kernel.h"
#include "xenia/kernel/xobject.h"
#include "xenia/memory.h"

namespace xe::kernel::xboxkrnl {

WaitableObjectResolution ResolveWaitableObject(
    Memory& memory, uint32_t body, uint32_t exact_kernel_default_object) {
  WaitableObjectResolution result;
  result.body = body;
  if (!body) {
    return result;
  }
  result.kind = WaitableObjectResolutionKind::kUnreadableHeader;
  if (body < sizeof(X_OBJECT_HEADER) ||
      memory.QueryRangeAccess(body - sizeof(X_OBJECT_HEADER),
                              sizeof(X_OBJECT_HEADER)) ==
          xe::memory::PageAccess::kNoAccess) {
    return result;
  }
  X_OBJECT_HEADER header;
  std::memcpy(&header, memory.TranslateVirtual(body - sizeof(X_OBJECT_HEADER)),
              sizeof(header));
  result.object_type = header.object_type_ptr;
  result.kind = WaitableObjectResolutionKind::kUnreadableType;
  if (!result.object_type ||
      memory.QueryRangeAccess(result.object_type, sizeof(X_OBJECT_TYPE)) ==
          xe::memory::PageAccess::kNoAccess) {
    return result;
  }
  X_OBJECT_TYPE type;
  std::memcpy(&type, memory.TranslateVirtual(result.object_type), sizeof(type));
  result.default_object = type.unknown_size_or_object_;

  uint32_t target;
  auto kind = WaitableObjectResolutionKind::kOffset;
  if (exact_kernel_default_object &&
      result.default_object == exact_kernel_default_object) {
    // HLE globals live in the low guest virtual heap, unlike retail kernel
    // globals. Recognize only this actual Kernel-owned identity as absolute.
    target = result.default_object;
    kind = WaitableObjectResolutionKind::kKernelDefault;
  } else if (result.default_object & 0x80000000u) {
    // Compatibility inference: retail kernel default objects use high guest
    // addresses; nonnegative metadata is an embedded dispatcher offset. The
    // supplied 17559 listener directly proves offset +0xC, not every possible
    // absolute/sentinel encoding. Unsupported targets still fail readability.
    target = result.default_object;
    kind = WaitableObjectResolutionKind::kAbsolute;
  } else {
    if (result.default_object > std::numeric_limits<uint32_t>::max() - body) {
      result.kind = WaitableObjectResolutionKind::kOffsetOverflow;
      return result;
    }
    target = body + result.default_object;
  }
  result.kind = WaitableObjectResolutionKind::kUnreadableTarget;
  if (!target || memory.QueryRangeAccess(target, sizeof(X_DISPATCH_HEADER)) ==
                     xe::memory::PageAccess::kNoAccess) {
    return result;
  }
  result.kind = kind;
  result.waitable = target;
  return result;
}

}  // namespace xe::kernel::xboxkrnl
