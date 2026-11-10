/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_XCONFIG_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_XCONFIG_H_
#include <cstdint>
#include "xenia/xbox.h"
namespace xe {
class Memory;
}
namespace xe::kernel {
class XConfig;
}
namespace xe::kernel::xboxkrnl {
// Shared actual guest-buffer adapters; independent of KernelState lifetime so
// bounds/byte-order/status tests need no fabricated guest Kernel or thread.
X_STATUS ReadXConfigGuest(XConfig&, Memory&, uint16_t category,
                          uint16_t setting, uint32_t buffer, uint16_t capacity,
                          uint32_t required_size);
X_STATUS WriteXConfigGuest(XConfig&, Memory&, uint16_t category,
                           uint16_t setting, uint32_t buffer, uint32_t size);
}  // namespace xe::kernel::xboxkrnl
#endif
