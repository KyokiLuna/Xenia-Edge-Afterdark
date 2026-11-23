/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_SNAPSHOT_PUBLICATION_SET_H_
#define XENIA_BASE_SNAPSHOT_PUBLICATION_SET_H_
#include <memory>
#include <span>
#include <vector>
#include "xenia/base/snapshot_publisher.h"
namespace xe::filesystem {
// One actual bounded set of flat snapshot reservations. It adds only the
// observed members to an already-owned view, preserving the existing parent
// permit and its depth limit. No guest path or caller-filled file identity can
// construct a reservation or a currentness receipt.
//
// The complete profile publication owner must retain this set through native
// IO drain and logical model/cache commit or retired-source recovery. This
// class alone is neither a multi-file journal nor a native completion/barrier.
class SnapshotPublicationSet final {
 public:
  static std::shared_ptr<SnapshotPublicationSet> Reserve(
      std::shared_ptr<const HostStorageRoot> base,
      std::vector<std::shared_ptr<const HostStorageSnapshotToken>> before,
      std::error_code&);
  ~SnapshotPublicationSet();
  SnapshotPublicationSet(const SnapshotPublicationSet&) = delete;
  const std::shared_ptr<const HostStorageRoot>& base() const;
  // Tokens retain the original published view. Only this owned set can use
  // its actual reservation chain to observe/mutate fenced members.
  std::shared_ptr<const HostStorageSnapshotToken> Capture(
      const std::filesystem::path&, uint64_t maximum, std::error_code&,
      bool ascii_folded_leaf = false) const;
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
  struct State;
  explicit SnapshotPublicationSet(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::filesystem
#endif
