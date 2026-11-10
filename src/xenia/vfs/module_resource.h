/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_VFS_MODULE_RESOURCE_H_
#define XENIA_VFS_MODULE_RESOURCE_H_
#include <string_view>
#include <vector>
#include "xenia/xbox.h"
namespace xe::vfs {
class VirtualFileSystem;
// Read-only bounded VFS open and owned snapshot. Optional member uses the
// shared XUIZ parser. No extraction, cache, guest module execution, or writes.
X_STATUS ReadModuleResource(VirtualFileSystem&, std::string_view guest_module,
                            std::string_view section, std::string_view member,
                            std::vector<uint8_t>& output,
                            uint32_t maximum_resource_bytes = 16u * 1024u *
                                                              1024u);
}  // namespace xe::vfs
#endif
