/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HOST_PATH_FILE_H_
#define XENIA_VFS_DEVICES_HOST_PATH_FILE_H_

#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

class HostPathEntry;
class Device;

class HostPathFile : public File {
 public:
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<xe::filesystem::FileHandle> file_handle);
  ~HostPathFile() override;

  void Destroy() override;
  // Pure admission query; never completes, closes or repairs the session.
  bool HasMutatingClose() const;
  // Nonmutating query of the retained open handle, never Entry's cached EOF.
  // The caller supplies the usual blocking-I/O scheduling boundary.
  std::optional<filesystem::FileHandleInfo> QueryBackingInfo(
      std::error_code& error) const;
  // An Entry may hold a pending overwrite before any File has opened it.
  static bool EntryHasMutatingClose(const Entry*);
  std::shared_ptr<const filesystem::HostStorageFileLease> storage_lease()
      const override {
    return file_handle_->storage_lease();
  }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override;
  X_STATUS Flush() override;
  std::optional<DeviceControlLayout> DescribeDeviceControl(
      uint32_t code) const override;
  X_STATUS DeviceControl(uint32_t code, std::span<const uint8_t> input,
                         std::span<uint8_t> output,
                         uint32_t* transferred) override;
  X_STATUS SetTimes(uint64_t create_timestamp, uint64_t access_timestamp,
                    uint64_t write_timestamp) override;

 private:
  // Tests may wrap the real owned backend at its typed I/O boundary, keeping
  // the actual reservation/root/handle while injecting host failures.
  friend class HostPathFileTestAccess;
  // Registered mount/tree stays alive through final Entry::ReleaseFile.
  std::shared_ptr<Device> device_owner_;
  std::unique_ptr<xe::filesystem::FileHandle> file_handle_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HOST_PATH_FILE_H_
