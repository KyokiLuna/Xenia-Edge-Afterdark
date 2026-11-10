/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_devices/svod_container_file.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_entry.h"
#include "xenia/vfs/host_storage_errors.h"

namespace xe {
namespace vfs {

SvodContainerFile::SvodContainerFile(uint32_t file_access,
                                     SvodContainerEntry* entry)
    : XContentContainerFile(file_access, entry), entry_(entry) {}

SvodContainerFile::~SvodContainerFile() = default;

void SvodContainerFile::Destroy() { delete this; }

X_STATUS SvodContainerFile::Read(std::span<uint8_t> buffer, size_t offset,
                                 size_t record_file, size_t* transferred) {
  auto& file = entry_->files()->at(record_file);
  const auto result = file->ReadResult(offset, buffer);
  *transferred = result.bytes;
  return HostStorageError(result.error);
}

}  // namespace vfs
}  // namespace xe
