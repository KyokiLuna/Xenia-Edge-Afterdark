/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/vfs/file.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"
namespace xe::vfs {
File::File(uint32_t file_access, Entry* entry)
    : device_owner_(entry ? entry->device()->weak_from_this().lock() : nullptr),
      file_access_(file_access),
      entry_(entry) {}
}  // namespace xe::vfs
