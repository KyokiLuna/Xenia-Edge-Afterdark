/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_VFS_MANAGED_FILE_SERVICE_H_
#define XENIA_VFS_MANAGED_FILE_SERVICE_H_
#include <atomic>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/base/host_storage.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/file_open_context.h"
#include "xenia/vfs/entry.h"
namespace xe::vfs {
// Pure cached namespace values derived by the existing managed package owner.
// This owns NO native descriptor, Device, source/model/capability or callback;
// reading/updating it never grants File access or touches the filesystem.
class ManagedFileNamespaceView final {
 public:
  explicit ManagedFileNamespaceView(std::shared_ptr<const File::Information> initial)
      : current_(std::move(initial)) {}
  std::shared_ptr<const File::Information> Snapshot() const noexcept {
    return current_.load(std::memory_order_acquire);
  }
  // The actual producer prepares the plain value before its namespace commit.
  // Existing Source/CM transaction ownership, not this cache, grants publication.
  void PublishPrepared(std::shared_ptr<const File::Information> value) noexcept {
    current_.store(std::move(value),std::memory_order_release);
  }
 private:
  std::atomic<std::shared_ptr<const File::Information>> current_;
};
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
  enum class NamespaceRole { kProtectedLeaf, kRawFolderProjection };
  // The explicit CM-issued raw folder projection manages the actual package
  // node, not its independently opened member leaves. The real member Device
  // still uses every ordinary/Account/GPD/PNG per-open policy. Defaults retain
  // existing descendant protection; tombstones retain the published role.
  virtual NamespaceRole namespace_role() const noexcept {
    return NamespaceRole::kProtectedLeaf;
  }
  // Optional source/catalog-derived file projection over the SAME physical
  // managed leaf, prepared outside namespace ownership. The producer checks
  // the exact actual Device/volume generation. Default ordinary services do
  // not change namespace shape. OpenWithContext still owns every permission.
  virtual std::shared_ptr<const ManagedFileNamespaceView> NamespaceView(
      const Device&) const { return {}; }

  virtual X_STATUS Open(std::shared_ptr<Device> requested_device,
      std::string requested_path, FileDisposition, uint32_t access,
      bool directory, File**, FileAction*) = 0;
  // Called through the SAME exact-leaf policy before any disposition effect,
  // after VFS has checked all actual source constraints and dropped namespace
  // ownership. Existing policies do not silently accept another policy's
  // native request capability. No worker TLS/path lookup reconstructs it.
  virtual X_STATUS OpenWithContext(std::shared_ptr<Device> requested_device,
      std::string requested_path, FileDisposition disposition, uint32_t access,
      bool directory, File** file, FileAction* action,
      const FileOpenContext& context) {
    if (context.managed_authority) {
      if (file) { *file = nullptr; }
      if (action) { *action = FileAction::kDoesNotExist; }
      return X_STATUS_ACCESS_DENIED;
    }
    return Open(std::move(requested_device), std::move(requested_path),
                disposition, access, directory, file, action);
  }
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
    ManagedFileService::NamespaceRole role =
        ManagedFileService::NamespaceRole::kProtectedLeaf;
  };
  // Tombstones retain genuine directory/ancestor pins, never a mounted device,
  // Account file handle or producer authority. This prevents inode reuse from
  // invalidating physical-alias protection after the live service retires.
  // Rows remain bounded (4096); native FD exhaustion is an explicit refusal.
  std::vector<Row> services_;
};
}  // namespace xe::vfs
#endif
