/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_BASE_CONTENT_MUTATION_OWNER_H_
#define XENIA_BASE_CONTENT_MUTATION_OWNER_H_
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include "xenia/base/content_mutation_wire.h"
#include "xenia/base/content_package_mutation_wire.h"
#include "xenia/base/managed_device_identity.h"
#include "xenia/base/snapshot_publisher.h"
namespace xe::filesystem {
class ManagedVolumeAuthority;
class ManagedStorageBootstrap;
class ManagedBootstrapCreationPermit;
// One exact managed Content epoch. Factory admission and restart recovery run
// before catalog/guest publication. This is a separate journal purpose from
// profile intents, format phases and single-file backup storage.
class ContentMutationOwner final : public std::enable_shared_from_this<ContentMutationOwner> {
 public:
  using Kind = content_mutation_wire::Kind;
  struct Receipt {
    std::error_code error;
    std::string nonce;
    bool prepared_durable = false, metadata_published = false;
    bool directory_published = false, observed = false, durable = false;
    bool complete = false, stream_admitted_durable = false;
    SnapshotPublicationReceipt metadata;
    bool aborted_before_publication = false;
    std::vector<std::filesystem::path> preserved_artifacts;
  };
  const std::error_code& error() const { return error_; }
  // Cached only: safe for registry/catalog admission. Not an observing proof.
  bool ready() const {
    return !error_ && !blocked_.load(std::memory_order_acquire) && identity_ &&
           !identity_->retiring();
  }
  bool idle() const {
    return ready() && active_.load(std::memory_order_acquire) == 0;
  }
  struct PackageReceipt {
    std::error_code error;
    std::string nonce;
    bool prepared_durable = false, quarantined = false;
    bool header_removed = false, retirement_durable = false;
    bool space_retired = false, observed = false, complete = false;
  };
  // Consumes the genuine retained-tree proof. No guest path/ID or ordinary
  // absent token can authorize retirement; all member leases must be drained.
  PackageReceipt Delete(std::unique_ptr<HostStorageContentTree>,
      std::shared_ptr<const HostStorageSnapshotToken> exact_header,
      const std::atomic<bool>* canceled = nullptr);
  // Directory projection only: keeps the exact package inode, complete raw
  // Header and actual metadata-thumbnail inode. Packed containers need a real
  // writer/format adapter and are refused before any stage or public change.
  PackageReceipt Truncate(std::unique_ptr<HostStorageContentTree>,
      std::shared_ptr<const HostStorageSnapshotToken> exact_header,
      const std::atomic<bool>* canceled = nullptr);

  bool Visible(const std::filesystem::path& package) const;
  std::unique_ptr<HostStorageContentTree> CapturePackage(
      const std::filesystem::path&, std::error_code&,
      std::shared_ptr<const HostStorageRoot> admission_root = {}) const;
  std::shared_ptr<const HostStorageSnapshotToken> CaptureMetadata(
      Kind, const std::filesystem::path& package,
      const std::filesystem::path& exact_target, std::error_code&) const;
  // expected is a real complete load from this exact epoch. It is rechecked
  // under the common namespace reservation before creating any journal/stage.
  // The caller merges format fields into its complete bytes; tails/padding
  // are never synthesized or discarded by this format-neutral owner.
  Receipt WriteMetadata(Kind, const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> expected,
      std::span<const uint8_t> intended,
      const std::atomic<bool>* canceled = nullptr);
  // Creates the header plus ONE initially empty package directory. A genuine
  // orphan Header may already exist: its exact supplied before snapshot and
  // full raw bytes remain under the existing publisher/backup contract. Header and
  // directory can be physically published at different crash points, but are
  // hidden/reserved until durable Done. Restart reconciles exact identities.
  Receipt Create(const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> expected_header,
      std::span<const uint8_t> header,
      const std::atomic<bool>* canceled = nullptr);
  // Publish a complete bounded initial tree and its Header through this SAME
  // journal/namespace engine. No public empty directory is filled afterward.
  // Members are detached bytes, never authority; target/header must be absent.
  Receipt CreateInitial(const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> expected_header,
      std::span<const uint8_t> header,
      std::span<const ContentPackageInitialMember> members,
      const std::atomic<bool>* canceled = nullptr);
  // Bounded native-host reads into the same owned journal/staging engine.
  // The source is borrowed only for this synchronous call and is never queued.
  Receipt CreateStream(const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> expected_header,
      std::span<const uint8_t> header, ContentPackageStreamSource& source,
      const std::atomic<bool>* canceled = nullptr,
      std::atomic<uint64_t>* progress = nullptr);
  // Reconcile authenticated publication duties and preparation-only aborts.
  // Partial control bytes/private setup are preserved, never adopted. Foreign
  // identities and irreconcilable third states still deny availability. A Done
  // or abort marks its recorded duty, never a fresh UI success receipt.
  std::error_code Recover();
  // Owned metadata only, never retained storage authority. Includes opaque
  // private control temps and preservation destinations from actual recovery.
  std::vector<std::filesystem::path> PreservedArtifacts() const;
  class FlushReservation final {
   public:
    ~FlushReservation();
    FlushReservation(const FlushReservation&) = delete;
    FlushReservation& operator=(const FlushReservation&) = delete;
   private:
    friend class ContentMutationOwner;
    FlushReservation(std::shared_ptr<ContentMutationOwner> owner,
        std::shared_ptr<HostStorageRoot> view, std::filesystem::path package)
        : owner_(std::move(owner)), view_(std::move(view)), package_(std::move(package)) {}
    std::shared_ptr<ContentMutationOwner> owner_;
    std::shared_ptr<HostStorageRoot> view_;
    std::filesystem::path package_;
  };
  std::unique_ptr<FlushReservation> BeginFlush(
      const std::filesystem::path& package, std::error_code&);
  // Caller first drains actual queued package I/O. This performs real common
  // file + directory barriers and the separate metadata parent barrier.
  std::error_code Flush(FlushReservation&,
                        const std::filesystem::path& exact_header);
 private:
  friend class ManagedVolumeAuthority;
  friend class ManagedStorageBootstrap;
  friend class ContentMutationOwnerTestAccess;
  ContentMutationOwner() = default;
  static std::shared_ptr<ContentMutationOwner> OpenInternal(
      std::shared_ptr<HostStorageRoot> content,
      std::shared_ptr<const ManagedDeviceIdentity>,
      std::shared_ptr<HostStorageRoot> private_parent,
      std::shared_ptr<const SnapshotRecoveryOwner>,
      std::shared_ptr<const HostStorageIntentLease> source_lease,
      bool allow_initialize,
      ManagedBootstrapCreationPermit* creation = nullptr);
  // Detached existing XCMOWN1 bytes plus actual native root IDs; no authority.
  static std::vector<uint8_t> BootstrapMarker(
      const ManagedDeviceIdentity&, const HostStorageRoot& root,
      const HostStorageRoot& records, const HostStorageRoot& staging,
      std::error_code&);
  Receipt Mutate(Kind, const std::filesystem::path&,
      std::shared_ptr<const HostStorageSnapshotToken>, std::span<const uint8_t>,
      const std::atomic<bool>*, std::error_code post_stage_parent_error = {},
      std::span<const ContentPackageInitialMember> initial = {},
      unsigned stop_boundary = 0,
      // Private deny-only fault ceiling, applied after the genuine native
      // capacity observation. Never increases capacity or grants authority.
      std::optional<uint64_t> initial_space_ceiling = {});
  Receipt StreamInternal(const std::filesystem::path&,
      std::shared_ptr<const HostStorageSnapshotToken>, std::span<const uint8_t>,
      ContentPackageStreamSource&, const std::atomic<bool>*, std::atomic<uint64_t>*,
      uint64_t stop_after_bytes = UINT64_MAX,
      std::optional<uint64_t> space_ceiling = {},
      std::atomic<bool>* cancel_after_authentication = nullptr,
      unsigned preparation_boundary = 0);
  std::error_code RollbackStream(const content_mutation_wire::Record&,
      std::span<const uint8_t>, const std::shared_ptr<HostStorageRoot>&,
      const std::shared_ptr<HostStorageRoot>&);
  enum class PackageBoundary { kNone, kQuarantined, kHeaderCommitted };
  PackageReceipt DeleteInternal(std::unique_ptr<HostStorageContentTree>,
      std::shared_ptr<const HostStorageSnapshotToken>, const std::atomic<bool>*,
      bool stop_after_prepared, bool stop_after_retire = false,
      content_package_mutation_wire::Kind kind = content_package_mutation_wire::Kind::kDelete,
      PackageBoundary boundary = PackageBoundary::kNone);
  PackageReceipt AdvancePackage(const content_package_mutation_wire::Record&,
      std::span<const uint8_t>, std::shared_ptr<HostStorageRoot> package_reserved,
      std::shared_ptr<HostStorageRoot> header_reserved, bool recovering,
      bool stop_after_retire = false, PackageBoundary boundary = PackageBoundary::kNone);
  std::error_code RecoverPackageIntent(
      const content_package_mutation_wire::Record&, std::span<const uint8_t>,
      const std::set<std::filesystem::path>& present,
      std::map<std::filesystem::path, uint64_t>& known_directories);
  std::error_code AppendRecord(const std::filesystem::path&, std::span<const uint8_t>);
  std::error_code BeginPreparation(content_mutation_wire::Record&,
      const HostStorageSnapshotToken&, std::span<const uint8_t>, std::vector<uint8_t>&);
  std::error_code AbortPreparation(const content_mutation_wire::Preparation&,
      std::span<const uint8_t>, const std::shared_ptr<HostStorageRoot>&,
      const std::shared_ptr<HostStorageRoot>&, unsigned stop_boundary = 0);
  std::error_code PreserveArtifact(content_mutation_wire::Preservation,
      const std::shared_ptr<HostStorageRoot>&, unsigned stop_boundary = 0);
  std::error_code ObservePreparationBackup(const content_mutation_wire::Preparation&);
  std::error_code CheckPreparationBefore(const content_mutation_wire::Preparation&,
                                       const HostStorageSnapshotToken&) const;
  std::error_code RecoverPreparations(const std::vector<std::filesystem::path>&,
      const std::set<std::filesystem::path>&,
      std::map<std::filesystem::path, uint64_t>&);
  std::error_code RecoverLocked();
  Receipt Advance(const content_mutation_wire::Record&,
                  std::span<const uint8_t> encoded,
                  std::shared_ptr<HostStorageRoot> reserved,
                  std::shared_ptr<HostStorageRoot> package_reserved,
                  std::unique_ptr<PreparedSnapshotPublication>, bool recovery,
                  unsigned stop_boundary = 0);
  std::error_code CheckCurrent() const;
  std::error_code EnsureParents(const std::filesystem::path&) const;
  content_mutation_wire::Identity wire_identity_;
  std::shared_ptr<const HostStorageIntentLease> source_lease_;
  std::shared_ptr<HostStorageRoot> content_, parent_, records_, staging_;
  std::shared_ptr<const ManagedDeviceIdentity> identity_;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery_;
  // Serializes actual host operations, never held by native/GCRC/model locks
  // and never across guest/UI callbacks. StreamSource is synchronous native
  // host I/O, not an emulated provider dispatch. Cached state uses atomics only.
  mutable std::mutex operation_mutex_;
  std::atomic<uint64_t> active_{0};
  std::atomic<bool> blocked_{true};
  // Preserve pending reservations on error; explicit Recover reuses/replaces
  // them outside catalog locks. No owner/operation strong-reference cycle.
  struct PendingReservation {
    std::string nonce;
    std::shared_ptr<HostStorageRoot> view;
  };
  std::map<std::filesystem::path, PendingReservation> pending_reservations_;
  // Production bound reserves canonical/temporary/abort phases together
  // (create12, metadata4, retirement6). Tests may lower this
  // private budget to exercise the real filesystem path without huge trees.
  size_t maximum_records_ = 8192;
  // Deny-only source-test interruption after real private writes/rename.
  std::string record_stop_suffix_;
  unsigned record_stop_boundary_ = 0;
  std::optional<uint64_t> preparation_space_ceiling_;
  std::error_code error_;
  std::vector<std::filesystem::path> preserved_artifacts_;
};
}  // namespace xe::filesystem
#endif
