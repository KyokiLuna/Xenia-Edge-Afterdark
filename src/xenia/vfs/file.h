/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_FILE_H_
#define XENIA_VFS_FILE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

#include "xenia/base/filesystem.h"
#include "xenia/xbox.h"

namespace xe::filesystem {
class HostStorageFileLease;
}

namespace xe {
namespace vfs {

class Entry;
class Device;

// Pure checked guest-unit conversion; this grants no filesystem/allocation
// authority and is never a substitute for an actual backend observation.
X_STATUS RoundFileAllocationSize(uint64_t native_bytes, uint64_t guest_unit,
                                uint64_t* output);

class File {
 public:
  struct Information {
    uint64_t create_timestamp = 0, access_timestamp = 0, write_timestamp = 0;
    uint64_t change_timestamp = 0, allocation_size = 0, end_of_file = 0;
    uint32_t attributes = 0;
  };
  struct DeviceControlLayout {
    uint32_t input_bytes = 0;
    uint32_t output_bytes = 0;
  };
  File(uint32_t file_access, Entry* entry);
  // Captured while the Entry is live at actual open. A detached container's
  // complete device/tree survives until this file's derived destructor ends.
  const std::shared_ptr<Device>& retained_device() const {
    return device_owner_;
  }
  virtual ~File() = default;

  virtual void Destroy() = 0;
  virtual std::shared_ptr<const filesystem::HostStorageFileLease>
  storage_lease() const {
    return {};
  }

  virtual X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                            size_t* out_bytes_read) = 0;
  virtual X_STATUS WriteSync(std::span<const uint8_t> buffer,
                             size_t byte_offset, size_t* out_bytes_written) = 0;

  // TODO: Parameters
  virtual X_STATUS ReadAsync(std::span<uint8_t> buffer, size_t byte_offset,
                             size_t* out_bytes_read) {
    return X_STATUS_NOT_IMPLEMENTED;
  }

  // TODO: Parameters
  virtual X_STATUS WriteAsync(std::span<uint8_t> buffer, size_t byte_offset,
                              size_t* out_bytes_written) {
    return X_STATUS_NOT_IMPLEMENTED;
  }

  // Query this retained byte view. Immutable backend metadata comes from its
  // retained Entry; mutable or managed views override this to observe their
  // actual backing/session and return genuine unavailable/error status.
  // The caller serializes with other operations on this file. No output is
  // changed on failure, and success is not a flush/completion notification.
  virtual X_STATUS QueryInformation(Information* output) const;

  // Actual retained-file allocation in guest allocation-unit geometry.
  // Only an explicit backend implementation may succeed; no cached Entry
  // allocation/EOF fallback. Output is unchanged on every failure.
  virtual X_STATUS QueryAllocationSize(uint64_t* output) const {
    return X_STATUS_NOT_SUPPORTED;
  }
  virtual X_STATUS SetLength(size_t length) { return X_STATUS_NOT_IMPLEMENTED; }
  // Unsupported backends must never imply that host data reached storage.
  virtual X_STATUS Flush() { return X_STATUS_NOT_SUPPORTED; }
  // A retained file advertises a fixed, bounded control packet before the
  // kernel accepts it. The current folder-volume controls have no input and
  // write exactly 8 or 16 bytes; output capacity may be larger.
  virtual std::optional<DeviceControlLayout> DescribeDeviceControl(
      uint32_t code) const {
    return {};
  }
  X_STATUS CheckDeviceControlAccess(uint32_t code) const {
    // CTL_CODE's access bits describe the handle rights required by the
    // control, independently of its packet layout.
    const uint32_t requested = (code >> 14) & 3;
    if ((requested & 1) &&
        !(file_access_ & (filesystem::FileAccess::kFileReadData |
                          filesystem::FileAccess::kGenericRead |
                          filesystem::FileAccess::kGenericAll))) {
      return X_STATUS_ACCESS_DENIED;
    }
    if ((requested & 2) &&
        !(file_access_ & (filesystem::FileAccess::kFileWriteData |
                          filesystem::FileAccess::kGenericWrite |
                          filesystem::FileAccess::kGenericAll))) {
      return X_STATUS_ACCESS_DENIED;
    }
    return X_STATUS_SUCCESS;
  }
  virtual X_STATUS DeviceControl(uint32_t code, std::span<const uint8_t> input,
                                 std::span<uint8_t> output,
                                 uint32_t* transferred) {
    *transferred = 0;
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  // Sets the FILETIMEs that aren't 0.
  virtual X_STATUS SetTimes(uint64_t create_timestamp,
                            uint64_t access_timestamp,
                            uint64_t write_timestamp) {
    return X_STATUS_UNSUCCESSFUL;
  }
  virtual X_STATUS Rename(const std::filesystem::path file_path) {
    return X_STATUS_NOT_IMPLEMENTED;
  }

  // xe::filesystem::FileAccess
  uint32_t file_access() const { return file_access_; }
  const Entry* entry() const { return entry_; }
  Entry* entry() { return entry_; }

 protected:
  // xe::filesystem::FileAccess
  std::shared_ptr<Device> device_owner_;
  uint32_t file_access_ = 0;
  Entry* entry_ = nullptr;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_FILE_H_
