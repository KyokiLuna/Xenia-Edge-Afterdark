/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_SNAPSHOT_PUBLISHER_H_
#define XENIA_BASE_SNAPSHOT_PUBLISHER_H_
#include <atomic>
#include <memory>
#include <span>
#include "xenia/base/host_storage.h"
namespace xe::filesystem {
// Open at explicit Emulator bootstrap, before any guest-backed mount. No
// global owner, console-settings payload, profile alias discovery or adoption.
class SnapshotRecoveryOwner final {
 public:
  static constexpr auto kDirectory = ".xenia-recovery-v1";
  static constexpr auto kMarker = "owner.v1";
  static std::unique_ptr<SnapshotRecoveryOwner> Open(
      std::shared_ptr<const HostStorageRoot> managed_parent);
  static std::unique_ptr<SnapshotRecoveryOwner> OpenExisting(
      std::shared_ptr<const HostStorageRoot> managed_parent);
  const std::error_code& error() const { return error_; }
  const std::shared_ptr<HostStorageRoot>& root() const { return root_; }
  bool created() const { return created_; }
  // Native filesystem compatibility, before creating a stage/journal. Zero
  // reserve is allowed; isolated query authorities are never private owners.
  std::error_code CheckTarget(const HostStorageRoot&) const;
 private:
  static std::unique_ptr<SnapshotRecoveryOwner> OpenInternal(
      std::shared_ptr<const HostStorageRoot>, bool create);
  SnapshotRecoveryOwner() = default;
  std::shared_ptr<HostStorageRoot> root_;
  std::error_code error_;
  bool created_ = false;
};
enum class SnapshotPublicationPhase {
  kAdmission,
  kStageCreate,
  kStageWrite,
  kStageFlush,
  kRecoveryCreate,
  kRecoveryWrite,
  kRecoveryFlush,
  kRecoveryVerify,
  kCompareAndPublish,
  kComplete,
  kCompareAndDelete
};
enum class SnapshotMutationKind { kWrite, kDelete };
struct SnapshotPublicationReceipt {
  std::error_code error;
  SnapshotPublicationPhase phase = SnapshotPublicationPhase::kAdmission;
  std::filesystem::path stage, recovery;
  bool stage_created = false, recovery_created = false;
  size_t staged_count = 0, recovery_count = 0;
  bool recovery_durable = false, published = false, durable = false;
  std::shared_ptr<const HostStorageSnapshotToken> expected, current;
  SnapshotMutationKind kind = SnapshotMutationKind::kWrite;
};
class ContentMutationOwner;
// Owns an already written/flushed immutable stage and, when necessary, its
// exact durable old-byte backup. A journal owner may persist these real native
// identities before publishing; no guest handle or raw FD escapes.
class PreparedSnapshotPublication final {
 public:
  const SnapshotPublicationReceipt& receipt() const { return receipt_; }
  const FileHandleInfo& stage_info() const { return stage_info_; }
  std::span<const uint8_t> intended() const { return intended_; }
 private:
  friend class SnapshotPublisher;
  friend class ContentMutationOwner;
  SnapshotPublicationReceipt receipt_;
  FileHandleInfo stage_info_{};
  std::vector<uint8_t> intended_;
  std::unique_ptr<HostStorageSnapshotStage> stage_;
  std::shared_ptr<const HostStorageSnapshotToken> recovery_;
  bool consumed_ = false;
};
class SnapshotPublisher final {
 public:
  static std::unique_ptr<PreparedSnapshotPublication> Prepare(
      const SnapshotRecoveryOwner&,
      std::shared_ptr<const HostStorageSnapshotToken> expected,
      std::span<const uint8_t> intended,
      const std::atomic<bool>* canceled = nullptr);
  static SnapshotPublicationReceipt Commit(
      PreparedSnapshotPublication&, const std::atomic<bool>* canceled = nullptr);
  // Format-neutral single-file operation. Existing bytes are never truncated;
  // the exact immutable old snapshot is copied to a distinct protected root.
  // Every created/uncertain file is retained and identified in the receipt.
  // Cancellation cannot roll back publication. No multi-file atomicity claim.
  static SnapshotPublicationReceipt Publish(
      const SnapshotRecoveryOwner&,
      std::shared_ptr<const HostStorageSnapshotToken> expected,
      std::span<const uint8_t> intended,
      const std::atomic<bool>* canceled = nullptr);
  // Explicit deletion of an observed existing flat file. Its raw bytes are
  // preserved in a durable protected recovery copy before common CAS unlink.
  // No stage/empty write and no synthetic success for an already-missing file.
  static SnapshotPublicationReceipt Delete(
      const SnapshotRecoveryOwner&,
      std::shared_ptr<const HostStorageSnapshotToken> expected,
      const std::atomic<bool>* canceled = nullptr);
 private:
  friend class ContentMutationOwner;
  static std::unique_ptr<PreparedSnapshotPublication> PrepareInternal(
      const SnapshotRecoveryOwner&,
      std::shared_ptr<const HostStorageSnapshotToken>, std::span<const uint8_t>,
      const std::atomic<bool>*, std::string_view exact_nonce,
      // Test fault may report uncertainty only AFTER the real parent barrier.
      std::error_code post_stage_parent_error = {});
};
}  // namespace xe::filesystem
#endif
