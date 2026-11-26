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
#include <span>
#include <system_error>
#include <vector>
#include "xenia/base/filesystem.h"
namespace xe::filesystem {
class HostStorageRoot;
class HostStorageSnapshotBatch;
class HostStorageSnapshotToken;
class SnapshotPublicationSet;
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
struct ContentPackageStreamMember {
  std::filesystem::path relative;
  uint64_t size = 0;
  bool directory = false;
};
// Synchronous native-host I/O only. The producer retains the originally opened
// package and its native fragment handles; methods must never reselect a
// pathname or invoke guest/UI callbacks. Metadata is copied before staging.
// This supplies bytes, never destination mutation or publication authority.
class ContentPackageStreamSource {
 public:
  virtual ~ContentPackageStreamSource() = default;
  virtual std::span<const ContentPackageStreamMember> members() const = 0;
  virtual FileIoResult Read(size_t member, uint64_t offset,
                           std::span<uint8_t> output) = 0;
  virtual std::error_code CheckCurrent() const = 0;
};
// Authenticated prepared-tree data. Native IDs/stamps come from actual staged
// files; digests bind complete regular bytes, directories have a zero digest.
struct ContentPackageInitialEntry {
  ContentPackageTreeEntry entry;
  std::array<uint8_t, 32> digest{};
  bool operator==(const ContentPackageInitialEntry&) const = default;
};
// Metadata-only native layout of one retained package directory. This opens
// no payload bytes and grants no File read/write, namespace reservation or
// delete/truncate authority. The physical pins prevent inode reuse; consumers
// still revalidate the exact layout and their independent source/open policy
// before using it. Existing compatible owned File views may coexist.
class HostStorageContentLayout final {
 public:
  ~HostStorageContentLayout();
  HostStorageContentLayout(const HostStorageContentLayout&) = delete;
  HostStorageContentLayout& operator=(const HostStorageContentLayout&) = delete;
  const std::vector<ContentPackageTreeEntry>& entries() const;
  const FileHandleInfo& top_info() const;
  const std::filesystem::path& relative() const;
  struct Statistics {
    uint64_t file_bytes = 0;
    // Each actual retained regular member owns this many virtual 4KiB bytes.
    // This is backing geometry, never source permission or raw read authority.
    uint64_t data_extent = 0;
    // Actual native st_blocks attribution for those SAME retained members.
    uint64_t allocated_bytes = 0;
    bool operator==(const Statistics&) const = default;
  };
  const Statistics& statistics() const;
  // Observing native metadata I/O, only in a real owned host-I/O scope and
  // outside GCRC. This is not a cached liveness check or immutable byte lease.
  std::error_code CheckCurrent() const;
 private:
  friend class HostStorageRoot;
  struct State;
  explicit HostStorageContentLayout(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
// Immutable complete flat-member bytes issued only by the SAME native
// profile publication admission. This retains its real scoped source until
// released. It grants no native IO-drain, account/GPD semantic quiescence,
// cold-format, timestamp, Header-signature or descriptor-ACK authority.
// Last destruction releases real scoped reservations/native pins: release it
// outside GCRC/model/cache locks after the genuine native consumers are done.
class HostStorageContentSnapshot final : public ContentPackageStreamSource {
public:
  ~HostStorageContentSnapshot() override;
  HostStorageContentSnapshot(const HostStorageContentSnapshot &) = delete;
  std::span<const ContentPackageStreamMember> members() const override;
  FileIoResult Read(size_t member, uint64_t offset,
                    std::span<uint8_t> output) override;
  // Joins exact bytes and complete membership under ONE genuine snapshot
  // batch guard. Does no guest callbacks and must run outside GCRC.
  std::error_code CheckCurrent() const override;
  // Its generic batch FileInfo creation field retains the existing POSIX
  // ctime convention; it is NOT native FAT birth metadata and must never feed
  // FromFileInfo/native profile creation. Actual native FAT words stay
  // required.
  std::unique_ptr<HostStorageSnapshotBatch> Validate(std::error_code &) const;
  // Final SAME-source full-set proof after this snapshot's own transaction
  // published selected members. Only exact tokens from its publication set's
  // actual durable receipts may replace baseline dependencies. Unchanged
  // members remain byte/identity checked and unexpected names still refuse.
  // Does not publish, import a native map, sign a Header or complete guest IO.
  std::unique_ptr<HostStorageSnapshotBatch> ValidateAfterOwnPublications(
      std::span<const std::shared_ptr<const HostStorageSnapshotToken>>,
      std::error_code&) const;
  // Metadata census only. POSIX write time here is not native FAT birth time;
  // the STFS producer's exact-index creation metadata must come separately.
  const HostStorageContentLayout &layout() const;

private:
  friend class SnapshotPublicationSet;
  struct State;
  explicit HostStorageContentSnapshot(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
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
