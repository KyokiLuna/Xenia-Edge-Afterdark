/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_BASE_CONTENT_PACKAGE_TREE_H_
#define XENIA_BASE_CONTENT_PACKAGE_TREE_H_
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include "xenia/base/filesystem.h"
namespace xe::filesystem {
class HostStorageRoot;
class ContentMutationOwner;
class ManagedDeviceIdentity;
class HostStorageIntentLease;
// Stable source-local identities for a bounded package tree. Persistent wire
// records deliberately omit volatile st_dev/mount numbers; the real retained
// root/managed UUID/epoch still supplies live filesystem admission.
struct ContentPackageTreeEntry {
  std::filesystem::path relative;
  uint64_t file = 0, bytes = 0;
  int64_t write_seconds = 0;
  uint32_t write_nanoseconds = 0, mode = 0;
  bool directory = false;
  bool operator==(const ContentPackageTreeEntry&) const = default;
};
// Detached initial bytes, not storage authority. Explicit parents precede
// children; the implicit package root is not a caller-provided member.
struct ContentPackageInitialMember {
  std::filesystem::path relative;
  std::vector<uint8_t> bytes;
  bool directory = false;
};
// Authenticated prepared-tree data. Native IDs/stamps come from actual staged
// files; digests bind complete regular bytes, directories have a zero digest.
struct ContentPackageInitialEntry {
  ContentPackageTreeEntry entry;
  std::array<uint8_t, 32> digest{};
  bool operator==(const ContentPackageInitialEntry&) const = default;
};
// Minted only by the native-FD/common-lease producer. Guest record paths or
// caller-filled entry arrays cannot construct a live mutation authority.
// Destroy outside GCRC/metadata locks: it releases actual native descriptors.
class HostStorageContentTree final {
 public:
  ~HostStorageContentTree();
  HostStorageContentTree(const HostStorageContentTree&) = delete;
  HostStorageContentTree& operator=(const HostStorageContentTree&) = delete;
  const std::vector<ContentPackageTreeEntry>& entries() const;
  const FileHandleInfo& top_info() const;
  const std::filesystem::path& relative() const;
 private:
  friend class HostStorageRoot;
  friend class ContentMutationOwner;
  struct State;
  explicit HostStorageContentTree(std::unique_ptr<State>);
  void BindIdentity(std::shared_ptr<const ManagedDeviceIdentity>,
                    std::shared_ptr<const HostStorageIntentLease>,
                    std::shared_ptr<const HostStorageRoot> admission_root);
  const std::shared_ptr<const ManagedDeviceIdentity>& identity() const;
  bool admission_retired() const;
  std::unique_ptr<State> state_;
};
}  // namespace xe::filesystem
#endif
