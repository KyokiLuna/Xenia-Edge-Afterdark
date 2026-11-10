/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_NULL_DEVICE_H_
#define XENIA_VFS_DEVICES_NULL_DEVICE_H_

#include <atomic>
#include <string>

#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

class NullEntry;

class NullDevice : public Device {
 public:
  NullDevice(const std::string& mount_path,
             const std::initializer_list<std::string>& null_paths);
  ~NullDevice() override;

  bool Initialize() override;
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings()
      const override {
    return std::vector<filesystem::GuestHostBacking>{};
  }
  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return false; }
  bool has_open_files() const override;
  void Retire() override { retired_.store(true); }
  bool available() const { return !retired_.load(); }
  filesystem::HostStorageSnapshot storage_space() const override {
    filesystem::HostStorageSnapshot result;
    result.error =
        std::make_error_code(available() ? std::errc::operation_not_supported
                                         : std::errc::no_such_device);
    return result;
  }

  bool supports_concurrent_io() const override { return true; }

  // Raw partitions, served by the disk driver.
  bool queues_all_requests() const override { return true; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 40; }

  uint32_t total_allocation_units() const override { return 0; }
  uint32_t available_allocation_units() const override { return 0; }

  // A sector size here does not advertise working raw storage or capacity.
  uint32_t sectors_per_allocation_unit() const override { return 0x80; }

  // STFC requires <= 0x1000
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  std::string name_;
  std::unique_ptr<Entry> root_entry_;
  std::vector<std::string> null_paths_;
  std::atomic<bool> retired_{false};
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_NULL_DEVICE_H_
