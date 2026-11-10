/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe::kernel::xam {

pointer_result_t XMemSet_entry(lpvoid_t destination, dword_t value,
                               dword_t count) {
  if (count) {
    std::memset(destination.as<void*>(), static_cast<uint8_t>(value.value()),
                count.value());
  }
  return destination.guest_address();
}
DECLARE_XAM_EXPORT1(XMemSet, kMemory, kImplemented);

pointer_result_t XMemCpy_entry(lpvoid_t destination, lpvoid_t source,
                               dword_t count) {
  if (count) {
    std::memcpy(destination.as<void*>(), source.as<const void*>(),
                count.value());
  }
  return destination.guest_address();
}
DECLARE_XAM_EXPORT1(XMemCpy, kMemory, kImplemented);

pointer_result_t XMemCpyStreaming_entry(lpvoid_t destination, lpvoid_t source,
                                        dword_t count) {
  // The console selects cache-aware bulk copy paths, including a write-combine
  // destination path. Their observable byte-copy contract is the same; neither
  // copy export supplies memmove's overlap guarantee.
  return XMemCpy_entry(destination, source, count);
}
DECLARE_XAM_EXPORT1(XMemCpyStreaming, kMemory, kImplemented);

}  // namespace xe::kernel::xam

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Memory);
