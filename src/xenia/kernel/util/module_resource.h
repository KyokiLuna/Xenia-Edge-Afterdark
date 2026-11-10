/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_UTIL_MODULE_RESOURCE_H_
#define XENIA_KERNEL_UTIL_MODULE_RESOURCE_H_
#include <string_view>
#include <vector>
#include "xenia/kernel/user_module.h"
namespace xe::kernel::util {
// Copies the actual loaded module resource under its execution lifetime lease.
// No guest pointer is returned; no disk extraction/cache is created. Complete
// output stays unchanged on failure. Caller cannot hold GCRC across providers.
X_STATUS CopyModuleResource(KernelState&, const object_ref<UserModule>&,
                            std::string_view section, std::vector<uint8_t>&,
                            uint32_t maximum_bytes = 16u * 1024u * 1024u);
}  // namespace xe::kernel::util
#endif
