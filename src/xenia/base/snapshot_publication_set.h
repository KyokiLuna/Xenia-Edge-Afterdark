/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_SNAPSHOT_PUBLICATION_SET_H_
#define XENIA_BASE_SNAPSHOT_PUBLICATION_SET_H_
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "xenia/base/snapshot_publisher.h"
namespace xe::kernel::xam { class FreshProfileConEpoch; }
namespace xe::filesystem {
// One actual bounded set of flat snapshot reservations. It adds only the
// observed members to an already-owned view, preserving the existing parent
// permit and its depth limit. No guest path or caller-filled file identity can
// construct a reservation or a currentness receipt.
//
// The complete profile publication owner must retain this set through native
// IO drain and logical model/cache commit or retired-source recovery. This
// class alone is neither a multi-file journal nor a native completion/barrier.
class SnapshotPublicationSet final
    : public std::enable_shared_from_this<SnapshotPublicationSet> {
public:
  static std::shared_ptr<SnapshotPublicationSet> Reserve(
      std::shared_ptr<const HostStorageRoot> base,
      std::vector<std::shared_ptr<const HostStorageSnapshotToken>> before,
      std::error_code&);
  // A genuine completed member checkpoint can continue with the SAME reserved
  // namespace, without resetting this immutable operation or reacquiring an
  // unrelated writer. Preparation joins complete membership and exact durable
  // own receipts. It performs no publication or native completion itself.
  class PreparedContinuation final {
   public:
    ~PreparedContinuation();
    PreparedContinuation(const PreparedContinuation&) = delete;
    const std::shared_ptr<SnapshotPublicationSet>& successor() const;
    const std::vector<std::shared_ptr<const HostStorageSnapshotToken>>& before() const;
    // No allocation, IO or new admission. Caller first prepares its actual
    // cache/model/native ownership transition under this same retained batch.
    // A repeated Apply refuses; abandoning preparation leaves this set active.
    bool Apply() noexcept;
    // Drop the original set lock and native guard BEFORE releasing retained
    // record/byte/admission owners. Destruction also performs this release.
    void ReleaseLocksAfterApply() noexcept;
   private:
    friend class SnapshotPublicationSet;
    struct Data;
    explicit PreparedContinuation(std::unique_ptr<Data>);
    std::unique_ptr<Data> data_;
  };
  std::unique_ptr<PreparedContinuation> PrepareContinuation(
      const HostStorageContentSnapshot& complete_before,
      std::span<const std::shared_ptr<const HostStorageSnapshotToken>> own_current,
      std::error_code&) const;
  ~SnapshotPublicationSet();
  SnapshotPublicationSet(const SnapshotPublicationSet&) = delete;
  const std::shared_ptr<const HostStorageRoot>& base() const;
  // Tokens retain the original published view. Only this owned set can use
  // its actual reservation chain to observe/mutate fenced members.
  std::shared_ptr<const HostStorageSnapshotToken> Capture(
      const std::filesystem::path&, uint64_t maximum, std::error_code&,
      bool ascii_folded_leaf = false) const;
  // Read-only metadata of this exact owned/projected tuple through its real
  // existing reservation scope. The temporary descriptor closes before return;
  // callers still join their final tokens with Validate(). No write/promotion
  // or native semantic authority is created by this filesystem observation.
  std::optional<RetainedFileMetadata> ObserveRetainedMetadata(
      std::shared_ptr<const HostStorageSnapshotToken>, std::error_code&) const;
  SnapshotPublicationReceipt Publish(
      const SnapshotRecoveryOwner&,
      std::shared_ptr<const HostStorageSnapshotToken> expected,
      std::span<const uint8_t>, const std::atomic<bool>* canceled = nullptr);
  SnapshotPublicationReceipt Delete(
      const SnapshotRecoveryOwner&,
      std::shared_ptr<const HostStorageSnapshotToken> expected,
      const std::atomic<bool>* canceled = nullptr);
  HostStorageRoot::ReplaceResult RetryDurability(
      std::shared_ptr<const HostStorageSnapshotToken>);
  HostStorageDurableObservation ObserveDurability(
      std::shared_ptr<const HostStorageSnapshotToken>) const;
  // The returned genuine batch owns the SAME native mutation guard while
  // identifying the original view/tokens required by model and VFS commit.
  // Its private keepalive releases reservation owners AFTER that guard.
  std::unique_ptr<HostStorageSnapshotBatch> Validate(
      std::vector<std::shared_ptr<const HostStorageSnapshotToken>>,
      std::error_code&) const;
 private:
  friend class ContentMutationOwner;
  friend class xe::kernel::xam::FreshProfileConEpoch;
  friend class HostStorageContentSnapshot;
  std::unique_ptr<HostStorageContentSnapshot>
  CaptureContentSnapshotForCM(const HostStorageRoot &,
                              const std::filesystem::path &,
                              std::error_code &) const;
  std::unique_ptr<HostStorageSnapshotBatch>
  ValidateContentSnapshot(const HostStorageContentSnapshot &,
                          std::error_code &) const;
  std::unique_ptr<HostStorageSnapshotBatch>
  ValidateContentSnapshotAfterOwnPublications(const HostStorageContentSnapshot&,
      std::span<const std::shared_ptr<const HostStorageSnapshotToken>>,
      std::error_code&) const;
  friend class ProfilePublicationIntentStore;
  static bool RootDescendsFrom(std::shared_ptr<const HostStorageRoot>,
                               const std::shared_ptr<const HostStorageRoot>&);
  // Only the genuine metadata owner may join its issued thumbnail authority.
  // This does not grant the ordinary caller a second namespace permission.
  std::shared_ptr<const HostStorageSnapshotToken> AttachThumbnail(
      std::shared_ptr<const HostStorageRoot> exact_source,
      std::shared_ptr<HostStorageRoot> reserved,
      std::shared_ptr<const HostStorageSnapshotToken> actual_before,
      std::shared_ptr<const void> admission_owner, std::error_code&);
  std::unique_ptr<HostStorageContentLayout> CaptureContentLayoutForCM(
      const HostStorageRoot& exact_source, const std::filesystem::path& exact_package,
      std::error_code&) const;
  struct State;
  explicit SnapshotPublicationSet(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::filesystem
#endif
