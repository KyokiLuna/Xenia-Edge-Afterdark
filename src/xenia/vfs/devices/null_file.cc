/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/null_file.h"

#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/null_entry.h"

namespace xe {
namespace vfs {

NullFile::NullFile(uint32_t file_access, NullEntry* entry)
    : File(file_access, entry),
      device_owner_(entry->device()->weak_from_this().lock()) {
  entry->RetainFile();
}

NullFile::~NullFile() { entry_->ReleaseFile(); }

void NullFile::Destroy() { delete this; }

X_STATUS NullFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                            size_t* out_bytes_read) {
  *out_bytes_read = 0;
  if (!static_cast<NullDevice*>(entry_->device())->available()) {
    return X_STATUS(0xC000009D);
  }
  if (!(file_access_ & FileAccess::kFileReadData)) {
    return X_STATUS_ACCESS_DENIED;
  }

  return buffer.empty() ? X_STATUS_SUCCESS : X_STATUS_NOT_SUPPORTED;
}

X_STATUS NullFile::WriteSync(std::span<const uint8_t> buffer,
                             size_t byte_offset, size_t* out_bytes_written) {
  *out_bytes_written = 0;
  if (!static_cast<NullDevice*>(entry_->device())->available()) {
    return X_STATUS(0xC000009D);
  }
  if (!(file_access_ &
        (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  // These raw endpoints have no sector backend. In particular a FATX format
  // header must not change geometry after an acknowledged-but-discarded write.
  return buffer.empty() ? X_STATUS_SUCCESS : X_STATUS_NOT_SUPPORTED;
}

X_STATUS NullFile::SetLength(size_t length) {
  if (!static_cast<NullDevice*>(entry_->device())->available()) {
    return X_STATUS(0xC000009D);
  }
  if (!(file_access_ & FileAccess::kFileWriteData)) {
    return X_STATUS_ACCESS_DENIED;
  }

  return X_STATUS_NOT_SUPPORTED;
}

}  // namespace vfs
}  // namespace xe
