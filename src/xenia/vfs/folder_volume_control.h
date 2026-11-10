/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_VFS_FOLDER_VOLUME_CONTROL_H_
#define XENIA_VFS_FOLDER_VOLUME_CONTROL_H_

#include <cstdint>
#include <optional>
#include <span>

#include "xenia/base/host_storage.h"
#include "xenia/base/memory.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/host_storage_errors.h"

namespace xe::vfs {
inline constexpr uint32_t kFolderVolumeDriveGeometry = 0x70000;
inline constexpr uint32_t kFolderVolumePartitionInfo = 0x74004;

inline std::optional<File::DeviceControlLayout> DescribeFolderVolumeControl(
    uint32_t code) {
  switch (code) {
    case kFolderVolumeDriveGeometry:
      return File::DeviceControlLayout{0, 8};
    case kFolderVolumePartitionInfo:
      return File::DeviceControlLayout{0, 16};
    default:
      return {};
  }
}

// One whole virtual folder volume, not an inferred physical partition layout.
// Its capacity is the same live declared view as FileFsSizeInformation.
inline X_STATUS EncodeFolderVolumeControl(
    uint32_t code, const filesystem::HostStorageSnapshot& snapshot,
    uint32_t sector_bytes, std::span<const uint8_t> input,
    std::span<uint8_t> output, uint32_t* transferred) {
  *transferred = 0;
  const auto layout = DescribeFolderVolumeControl(code);
  if (!layout) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  if (!input.empty()) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (output.size() < layout->output_bytes) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  if (snapshot.error) {
    return HostStorageError(snapshot.error);
  }
  if (!snapshot.available) {
    return X_STATUS(0xC000009D);  // STATUS_DEVICE_NOT_CONNECTED.
  }
  if (!sector_bytes) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  const uint64_t sectors = snapshot.capacity_bytes / sector_bytes;
  if (code == kFolderVolumeDriveGeometry) {
    if (sectors > UINT32_MAX) {
      return X_STATUS(0xC0000095);  // STATUS_INTEGER_OVERFLOW.
    }
    xe::store_and_swap<uint32_t>(output.data(), uint32_t(sectors));
    xe::store_and_swap<uint32_t>(output.data() + 4, sector_bytes);
  } else {
    // StartingOffset is zero only for this explicitly declared whole virtual
    // folder volume. This does not describe a retail physical HDD partition.
    xe::store_and_swap<uint64_t>(output.data(), 0);
    xe::store_and_swap<uint64_t>(output.data() + 8, sectors * sector_bytes);
  }
  *transferred = layout->output_bytes;
  return X_STATUS_SUCCESS;
}
}  // namespace xe::vfs
#endif  // XENIA_VFS_FOLDER_VOLUME_CONTROL_H_
