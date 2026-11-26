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
X_STATUS File::SetAllocationSize(uint64_t length) {
  if(length>std::numeric_limits<size_t>::max())return X_STATUS(0xC0000095);
  Information actual{};const auto status=QueryInformation(&actual);
  if(XFAILED(status))return status;
  return length<actual.end_of_file ? SetLength(size_t(length)) : X_STATUS_SUCCESS;
}
X_STATUS File::QueryInformation(Information* output) const {
  if (!output) return X_STATUS_INVALID_PARAMETER;
  auto lock = global_critical_region::AcquireDirect();
  if (!entry_ || !device_owner_ || device_owner_.get() != entry_->device()) {
    return X_STATUS_INVALID_HANDLE;
  }
  // Namespace withdrawal alone does not retire an already-open byte view.
  // Each mutable/managed backend decides actual source/view availability.
  Information result{};
  result.create_timestamp = entry_->create_timestamp();
  result.access_timestamp = entry_->access_timestamp();
  result.write_timestamp = result.change_timestamp = entry_->write_timestamp();
  result.allocation_size = entry_->allocation_size();
  result.end_of_file = entry_->size();
  result.attributes = entry_->attributes();
  *output = result;
  return X_STATUS_SUCCESS;
}
File::File(uint32_t file_access, Entry* entry)
    : device_owner_(entry ? entry->device()->weak_from_this().lock() : nullptr),
      file_access_(file_access),
      entry_(entry) {}
}  // namespace xe::vfs
