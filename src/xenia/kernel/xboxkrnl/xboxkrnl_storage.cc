/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_content_storage_driver.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

namespace xe::kernel::xboxkrnl {

// Supplied17559 native initializer8167ED90 passes exactly one callback address
// at8167EE34. Its return is unused; registration is admitted only against the
// owned driver's checked root/callback profile. No absent driver means success.
dword_result_t DrvSetContentStorageCallback_entry(dword_t callback) {
  auto driver = NativeContentStorageDriver::Lookup(kernel_state());
  return driver && driver->SetCallback(callback) ? X_STATUS_SUCCESS
                                                 : X_STATUS_NOT_SUPPORTED;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetContentStorageCallback, kFileSystem, kSketchy);

// Both genuine worker calls (8167EB90/8167ED40) pass one output pointer. The
// worker consumes the24-byte prefix and a borrowed ANSI volume-root string;
// signed-negative ends that drain. Failed output validation never pops a row.
dword_result_t DrvGetContentStorageNotification_entry(dword_t output) {
  auto driver = NativeContentStorageDriver::Lookup(kernel_state());
  return driver ? driver->GetNotification(output) : X_STATUS(0xC000009D);
}
DECLARE_XBOXKRNL_EXPORT1(DrvGetContentStorageNotification, kFileSystem,
                         kSketchy);

}  // namespace xe::kernel::xboxkrnl

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Storage);
