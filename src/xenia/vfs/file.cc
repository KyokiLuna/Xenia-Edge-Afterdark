/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/vfs/file.h"
#include <limits>

#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"
namespace xe::vfs {
X_STATUS RoundFileAllocationSize(uint64_t native_bytes, uint64_t guest_unit,
                                uint64_t* output) {
  if (!output || !guest_unit) {
    return X_STATUS_INVALID_PARAMETER;
  }
  constexpr uint64_t maximum = uint64_t(std::numeric_limits<int64_t>::max());
  if (native_bytes > maximum || guest_unit > maximum) {
    return X_STATUS(0xC0000095);  // STATUS_INTEGER_OVERFLOW
  }
  const uint64_t remainder = native_bytes % guest_unit;
  const uint64_t padding = remainder ? guest_unit - remainder : 0;
  if (native_bytes > maximum - padding) {
    return X_STATUS(0xC0000095);
  }
  *output = native_bytes + padding;
  return X_STATUS_SUCCESS;
}
File::File(uint32_t file_access, Entry* entry)
    : device_owner_(entry ? entry->device()->weak_from_this().lock() : nullptr),
      file_access_(file_access),
      entry_(entry) {}
}  // namespace xe::vfs
