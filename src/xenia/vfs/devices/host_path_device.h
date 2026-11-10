/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_
#define XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_

#include <filesystem>
#include <string>
#include <vector>
#include "xenia/vfs/device.h"
#include "xenia/base/host_storage.h"

namespace xe {
namespace vfs {

// Xenia keeps a package thumbnail as a file inside the package directory.
// The console holds it in the STFS header, so the guest never sees it.
inline constexpr char kPackageThumbnailFileName[] = "__thumbnail.png";

class HostPathEntry;

class HostPathDevice : public Device {
 public:
  class PreparedSnapshotCache;
  class SnapshotCacheCommit;
  // Flat managed leaves only. Allocation/capacity preparation precedes storage
  // currentness admission; no file is created or opened through these APIs.
  std::shared_ptr<PreparedSnapshotCache> PrepareSnapshotCache(
      std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>,
      std::error_code&);
  // Holds GCRC only after a nonwaiting acquisition. No filesystem/provider IO.
  // The genuine common batch remains owned through cache/model publication.
  std::unique_ptr<SnapshotCacheCommit> TryCommitSnapshotCache(
      std::shared_ptr<PreparedSnapshotCache>,
      std::shared_ptr<const filesystem::HostStorageSnapshotBatch>,
      std::error_code&);
  // Only a caller registering a whole virtual folder volume may grant disk
  // geometry controls. Content/package/system roots are not volumes by name.
  enum class ControlRole { kNone, kFolderVolume };
  // |name| is the file system the directory stands in for.
  HostPathDevice(
      const std::string_view mount_path, const std::filesystem::path& host_path,
      bool read_only, bool collapse_content_packages = false,
      const std::string_view name = "FATX",
      std::shared_ptr<filesystem::HostStorageRoot> storage = {},
      std::shared_ptr<const filesystem::HostStorageRoot> geometry = {},
      ControlRole control_role = ControlRole::kNone);
  ~HostPathDevice() override;

  bool Initialize() override;
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings()
      const override {
    return std::vector<filesystem::GuestHostBacking>{
        storage_ ? storage_->GuestBacking()
                 : filesystem::GuestHostBacking{host_path_, true}};
  }
  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return read_only_; }
  bool has_open_files() const override;
  void Retire() override;
  // Pre-registration only, with the actual child capability from the current
  // owned view. No observing IO, path adoption or arbitrary permit injection.
  bool InstallSnapshotReservation(std::shared_ptr<filesystem::HostStorageRoot>);
  filesystem::HostStorageSnapshot storage_space() const override;
  const std::shared_ptr<filesystem::HostStorageRoot>& storage() const {
    return storage_;
  }
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  ManagedDirectoryIdentity() const override {
    return storage_ ? storage_->directory_identity() : nullptr;
  }
  ControlRole control_role() const { return control_role_; }

  bool supports_concurrent_io() const override { return true; }

  bool queues_all_requests() const override { return name_ == "STFS"; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override;
  uint32_t available_allocation_units() const override;
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 protected:
  void OnManagedFileRundown() override;
  friend class HostPathEntry;
  std::filesystem::path host_path() const { return host_path_; }

 private:
  void ApplySnapshotCache(SnapshotCacheCommit&) noexcept;
  void PopulateEntry(HostPathEntry* parent_entry);
  // Canonical paths on the recursion stack, so a directory symlink cycle ends.
  void PopulateEntry(HostPathEntry* parent_entry,
                     std::vector<std::filesystem::path>& ancestors);

  std::shared_ptr<filesystem::HostStorageRoot> storage_;
  std::shared_ptr<const filesystem::HostStorageRoot> geometry_;
  std::string name_;
  std::filesystem::path host_path_;
  std::unique_ptr<Entry> root_entry_;
  bool read_only_;
  bool collapse_content_packages_;
  const ControlRole control_role_;
  bool snapshot_reservation_ = false;
};

class HostPathDevice::PreparedSnapshotCache final {
 public:
  ~PreparedSnapshotCache();

 private:
  friend class HostPathDevice;
  friend class SnapshotCacheCommit;
  struct Row {
    std::shared_ptr<const filesystem::HostStorageSnapshotToken> token;
    std::unique_ptr<HostPathEntry> new_leaf;
  };
  std::shared_ptr<HostPathDevice> device;
  std::vector<Row> rows;
  bool consumed = false;
};
class HostPathDevice::SnapshotCacheCommit final {
 public:
  ~SnapshotCacheCommit();
  SnapshotCacheCommit(const SnapshotCacheCommit&) = delete;
  // All checks/allocation happened before this point. Preserves existing
  // Entry identity, removes only absent leaves, adopts only prepared children.
  void Apply() noexcept;

 private:
  friend class HostPathDevice;
  SnapshotCacheCommit() = default;
  std::shared_ptr<PreparedSnapshotCache> prepared_;
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch_;
  std::vector<HostPathEntry*> existing_;
  xe::global_unique_lock_type namespace_lock_;
  bool applied_ = false;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_
