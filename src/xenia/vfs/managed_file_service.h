/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_VFS_MANAGED_FILE_SERVICE_H_
#define XENIA_VFS_MANAGED_FILE_SERVICE_H_
#include <filesystem>
#include <memory>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/base/host_storage.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/entry.h"
namespace xe::vfs {
// Host-owned, exact managed leaves only. Matching selects an existing policy;
// neither a filename nor a guessed Account layout creates storage authority.
// Open runs after releasing namespace ownership, before any disposition effect.
class ManagedFileService {
 public:
  virtual ~ManagedFileService() = default;
  virtual const std::filesystem::path& backing_path() const = 0;
  virtual std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const = 0;  // prepared real native pin, metadata only
  virtual bool retiring() const = 0;  // metadata/atomics only
  virtual X_STATUS Open(std::shared_ptr<Device> requested_device,
      std::string requested_path, FileDisposition, uint32_t access,
      bool directory, File**, FileAction*) = 0;
};
class ManagedFileRegistry final {
 public:
  // A retired policy remains a tombstone against raw open/delete/rename/map.
  // Only a verified new host mount may replace it; old handles retain old owners.
  bool Register(std::shared_ptr<ManagedFileService>);
  std::shared_ptr<ManagedFileService> Find(const std::filesystem::path&,
      std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> = {}) const;
  bool Protects(const std::filesystem::path&, bool subtree,
      std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> = {}) const;
 private:
  mutable xe::global_critical_region namespace_;
  struct Row {
    std::filesystem::path path;
    std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent;
    std::weak_ptr<ManagedFileService> service;
  };
  // Tombstones retain genuine directory/ancestor pins, never a mounted device,
  // Account file handle or producer authority. This prevents inode reuse from
  // invalidating physical-alias protection after the live service retires.
  // Rows remain bounded (4096); native FD exhaustion is an explicit refusal.
  std::vector<Row> services_;
};
}  // namespace xe::vfs
#endif
