/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_BASE_HOST_STORAGE_H_
#define XENIA_BASE_HOST_STORAGE_H_
#include <atomic>
#include "xenia/base/content_package_tree.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>
#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"
namespace xe::filesystem {
namespace content_package_mutation_wire { struct Record; }
inline constexpr uint64_t kHostStorageReserve = 20ull << 30;
struct HostStorageSnapshot {
  bool available = false;
  bool read_only = true;
  uint64_t capacity_bytes = 0, available_bytes = 0, reserve_bytes = 0;
  uint64_t usable_bytes = 0;
  std::error_code error;
};
// One live OS space authority and admission lock per host filesystem. This is
// not a usage ledger. The optional query is an isolated fault-test seam.
class HostStorageGuestProjection;
class ManagedVolumeAuthority;
class ContentMutationOwner;
struct GuestHostBacking {
  std::filesystem::path path;
  bool subtree = false;
  std::optional<FileHandleInfo> opened_file = std::nullopt;
  std::shared_ptr<const HostStorageGuestProjection> projection = {};
  bool requires_projection = false;
};
class HostStorageGuestMount {
 public:
  ~HostStorageGuestMount();

 private:
  friend class HostStorageProtection;
  friend class HostStorageRoot;
  struct State;
  explicit HostStorageGuestMount(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
// Minted only by the managed-device initializer. This is a retained native
// source/exclusion capability, never a caller-provided list of ignored names.
class HostStorageGuestProjection final {
 public:
  ~HostStorageGuestProjection();
 private:
  friend class HostStorageRoot;
  friend class HostStorageProtection;
  friend class ManagedVolumeAuthority;
  friend class ContentMutationOwner;
  friend class HostStorageManagedMetadata;
  struct State;
  explicit HostStorageGuestProjection(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
// Read-only namespace coordinates. Every directory identity used below owns a
// genuine nofollow native pin, including ancestors; inode reuse cannot turn an
// expired directory into authority. Mount IDs are intentionally irrelevant to
// physical conflict DENIAL across bind aliases. This grants no IO permission.
// Capture before model/GCRC/namespace locks. Comparisons use metadata only.
class HostStorageDirectoryIdentity final {
 public:
  static std::shared_ptr<const HostStorageDirectoryIdentity> Capture(
      const std::filesystem::path&, std::error_code&);
  ~HostStorageDirectoryIdentity();
  const std::filesystem::path& path() const;
  bool MatchesNativeDirectory(uint64_t filesystem, uint64_t directory) const;
  // Positive routing/permission requires actual rebased entry identity.
  bool SameEntry(const std::filesystem::path& entry,
      const HostStorageDirectoryIdentity& other,
      const std::filesystem::path& other_entry,
      bool ascii_folded_leaf = false) const;
  bool OverlapsKnown(const std::filesystem::path& entry,
      const HostStorageDirectoryIdentity& other,
      const std::filesystem::path& other_entry, bool structural,
      bool ascii_folded_leaf = false) const;
  // Conservative denial also closes unresolved same-FS bind-boundary ancestry.
  bool Overlaps(const std::filesystem::path& entry,
      const HostStorageDirectoryIdentity& other,
      const std::filesystem::path& other_entry, bool structural,
      bool ascii_folded_leaf = false) const;
  // Rebase an observed entry onto this real directory, including physical
  // aliases. Empty means this directory itself; nullopt means outside it.
  std::optional<std::filesystem::path> RelativeEntry(
      const HostStorageDirectoryIdentity& other,
      const std::filesystem::path& other_entry) const;
 private:
  struct State;
  explicit HostStorageDirectoryIdentity(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
class HostStorageSpace : public std::enable_shared_from_this<HostStorageSpace> {
 public:
  using Query = std::function<std::filesystem::space_info(
      const std::filesystem::path&, std::error_code&)>;
  class MutationGuard {
   public:
    ~MutationGuard();
    MutationGuard(const MutationGuard&) = delete;
    MutationGuard& operator=(const MutationGuard&) = delete;
    std::error_code Admit(uint64_t allocation_bound);
    // Metadata-only check; the actual guard must stay on its owning thread.
    bool IsOwnedByCurrentThread() const noexcept;
    std::shared_ptr<const HostStorageFileLease> TrackOpen(
        const std::filesystem::path&, const FileHandleInfo&,
        bool exclusive = false, bool namespace_scope = false,
        std::shared_ptr<const HostStorageFileLease> parent_permit = {},
        bool content_io = false, bool ascii_folded_leaf = false,
        std::shared_ptr<const HostStorageDirectoryIdentity> namespace_origin = {},
        std::filesystem::path namespace_entry = {},
        std::shared_ptr<const HostStorageDirectoryIdentity> namespace_tree = {});
    bool Conflicts(
        const std::filesystem::path&, bool subtree, uint64_t filesystem,
        uint64_t file,
        const std::shared_ptr<const HostStorageFileLease>& permitted = {},
        bool allow_existing_content_io = false,
        std::shared_ptr<const HostStorageDirectoryIdentity> query_identity = {},
        std::filesystem::path query_relative = {});
    bool Reserved(const std::filesystem::path&, const FileHandleInfo&,
        const std::shared_ptr<const HostStorageFileLease>& permitted = {},
        bool allow_clean_content_overwrite = false);
    void Moved(const std::shared_ptr<const HostStorageFileLease>&,
               const std::filesystem::path&);

   private:
    friend class HostStorageSpace;
    struct State;
    explicit MutationGuard(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
  };
  static std::shared_ptr<HostStorageSpace> ForPath(const std::filesystem::path&,
                                                   uint64_t reserve,
                                                   std::error_code&,
                                                   Query isolated_query = {});
  HostStorageSnapshot Observe(const std::filesystem::path&) const;
  bool NativeAuthorityFor(uint64_t filesystem_id) const;
  // Cached physical-coordinate denial only. No filesystem observation or
  // mutation-owner lock. Unknown native origin cannot bypass an active claim.
  bool ContentNamespaceReserved(const std::filesystem::path&, bool structural,
      const std::shared_ptr<const HostStorageFileLease>& permitted,
      const std::shared_ptr<const HostStorageDirectoryIdentity>& root_identity,
      const std::filesystem::path& relative) const;
  std::unique_ptr<MutationGuard> Begin(const std::filesystem::path&,
      std::error_code&, std::shared_ptr<const HostStorageDirectoryIdentity> = {});
  // Admission cannot wait on another process while a namespace caller owns
  // GCRC. Busy is explicit; no guest-memory/provider callbacks run here.
  std::unique_ptr<MutationGuard> TryBegin(const std::filesystem::path&,
      std::error_code&, std::shared_ptr<const HostStorageDirectoryIdentity> = {});
  ~HostStorageSpace();

 private:
  struct State;
  explicit HostStorageSpace(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
class PolicyFileHandle;
class HostStorageSnapshotToken;
class HostStorageSnapshotStage;
class HostStorageSnapshotBatch;
class HostStorageSnapshotTestAccess;
class HostStorageManagedMetadata;
class ManagedDeviceIdentity;
class ProfileIntentOwner;
class HostStorageIntentLease;
struct HostStorageRootIdentity {
  uint64_t filesystem = 0, directory = 0, mount = 0;
  bool operator==(const HostStorageRootIdentity&) const = default;
};
// A matched current file can be durable without this process having published
// it. This observation intentionally contains no publication token/status.
struct HostStorageDurableObservation {
  std::error_code error;
  bool matched = false, durable = false;
};
class HostStorageRoot : public std::enable_shared_from_this<HostStorageRoot> {
 public:
  static constexpr size_t kMaximumSnapshotBatchTokens = 4096;
  static constexpr uint64_t kMaximumSnapshotBatchBytes = 256ull << 20;
  struct IntentWriteResult {
    std::error_code error;
    size_t bytes = 0;
    bool created = false, durable = false;
  };
  struct ReplaceResult {
    std::error_code error;
    bool published = false;
    bool backup_created = false;
    bool durable = false;
    std::shared_ptr<const HostStorageSnapshotToken> current;
  };
  static std::shared_ptr<HostStorageRoot> Open(
      const std::filesystem::path&, bool read_only, std::error_code&,
      uint64_t reserve = 0,
      std::shared_ptr<HostStorageSpace> space = {});
  ~HostStorageRoot();
  const std::filesystem::path& path() const;
  const std::shared_ptr<const HostStorageDirectoryIdentity>&
  directory_identity() const;  // retained metadata only
  bool read_only() const;
  bool native_filesystem_authority() const;
  void Retire();
  // Cached logical cancellation only. Includes owned parent generations; no
  // filesystem observation, metadata lock or guest callback.
  bool retiring() const;
  size_t open_file_count() const;
  HostStorageSnapshot Observe() const;
  // Flat console authority only: max64 regular owner-private files, no
  // directories/symlinks/special entries. Unknown content is preserved and
  // rejected, never repaired/removed. Owned view keeps protection alive.
  std::shared_ptr<HostStorageRoot> ProtectFlat(std::error_code&) const;
  // Separate flat recovery policy: owner-private regular snapshot copies,
  // max8192 entries; never relaxes the console max64 policy. Native authority
  // proof is required; overlap needs a genuine retained exclusion capability.
  std::shared_ptr<HostStorageRoot> ProtectRecovery(std::error_code&) const;
  // Distinct immutable paired-intent/done grammar and owner purpose. This
  // neither expands console max64 nor snapshot recovery grammar.
  std::shared_ptr<HostStorageRoot> ProtectProfileIntents(std::error_code&) const;
  bool is_profile_intent_private() const;
  bool is_recovery_private() const;
  bool is_guest_private() const;
  static bool HasProtectedGuestStorage();
  std::shared_ptr<HostStorageRoot> GuestView(std::error_code&) const;
  GuestHostBacking GuestBacking() const;
  bool has_guest_projection() const;
  bool ContentNamespaceReady(const std::filesystem::path& relative,
                             bool structural = false) const;
  // Also used by catalog namespace resolution before exposing a host path.
  // Structural operations reject ancestors of an exclusion before mutation.
  std::error_code CheckGuestAccess(const std::filesystem::path& relative,
                                  bool structural = false) const;
  bool format_eligible() const;
  // Host launch classification opens only an identity probe, then returns a
  // Subroot of THIS retained authority. A matched denial forbids path fallback.
  std::shared_ptr<HostStorageRoot> CaptureGuestDirectory(
      const std::filesystem::path&, bool force_readonly, bool& matched,
      std::error_code&) const;

  // nullopt declares unknown host provenance; it is denied with active
  // protection and prevents later private admission while that mount lives.
  // Empty declares a genuinely host-free guest device. No guest name resolver.
  static std::shared_ptr<const HostStorageGuestMount> AdmitGuestMount(
      std::optional<std::vector<GuestHostBacking>>, std::error_code&);
  // Rights can only narrow. Missing children are never created implicitly.
  std::shared_ptr<HostStorageRoot> Subroot(const std::filesystem::path&,
                                           bool force_readonly,
                                           std::error_code&) const;
  // A new local view also depends on a logical source generation. Physical
  // admission remains owned by this root's actual destination filesystem.
  std::shared_ptr<HostStorageRoot> WithDependency(
      std::shared_ptr<const HostStorageRoot>, std::error_code&) const;
  std::unique_ptr<FileHandle> OpenFile(const std::filesystem::path& relative,
                                       uint32_t access, std::error_code&,
                                       bool create_exclusive = false) const;
  // Writes a newly created staged/journal file only; never truncates a target.
  FileIoResult WriteNewFile(const std::filesystem::path&,
                            std::span<const uint8_t>,
                            bool durable = true,
                            const std::atomic<bool>* canceled = nullptr) const;
  // Reserve exactly one regular file/initiating handle before logical FATX
  // overwrite. Later opens/maps and mutations through aliases fail busy until
  // this owned handle trims, flushes and closes. Old clusters remain reusable.
  std::unique_ptr<FileHandle> ReserveOverwrite(const std::filesystem::path&,
                                               std::error_code&) const;
  std::error_code Create(const std::filesystem::path&, bool directory) const;
  std::error_code Remove(const std::filesystem::path&, bool recursive) const;
  // Reset one existing package leaf to an empty directory under the same
  // admission and lease guard. An existing directory keeps its identity and
  // the optional exact flat child name; a regular file becomes a directory.
  // All affected entries are preflighted before removal. This is not a durable
  // package transaction: a later native unlink/mkdir failure may be partial.
  std::error_code ResetDirectory(
      const std::filesystem::path&,
      const std::filesystem::path& preserved_leaf = {}) const;
  std::error_code Rename(
      const std::filesystem::path& from, const std::filesystem::path& to,
      bool replace_existing,
      std::shared_ptr<const HostStorageFileLease> permitted = {}) const;
  // Publish a staged regular file within one parent. Replacing an existing
  // target requires a new backup name; its original inode is retained before
  // the atomic replacement. A flush error
  // after rename has published=true and must never be treated as no mutation.
  ReplaceResult AtomicReplaceFile(
      const std::filesystem::path& staged, const std::filesystem::path& target,
      const std::filesystem::path& backup = {}) const;
  // Protected profile backups use an owned vault outside every guest mount.
  // Existing backup names fail unchanged; target/vault must share one actual
  // filesystem and its single admission owner.
  ReplaceResult AtomicReplaceFile(
      const std::filesystem::path& staged, const std::filesystem::path& target,
      std::shared_ptr<const HostStorageRoot> backup_root,
      const std::filesystem::path& backup) const;
  // Complete immutable load tokens, not a success-shaped empty fallback.
  // Missing returns a valid absent token; any unreadable/short/oversize payload
  // returns error and no token. All CAS operations share Mutation/Conflicts.
  // ASCII-folded final leaves resolve one exact spelling or reject ambiguity;
  // CAS rechecks that logical slot, including an originally absent target.
  std::shared_ptr<const HostStorageSnapshotToken> CaptureSnapshot(
      const std::filesystem::path&, uint64_t maximum, std::error_code&,
      bool ascii_folded_leaf = false) const;
  // The leaf is created exclusively and reserved against other opens/maps.
  // created can be true on later failure: preserve that uncertain file.
  std::unique_ptr<HostStorageSnapshotStage> CreateSnapshotStage(
      const std::filesystem::path&, bool& created, std::error_code&) const;
  ReplaceResult CompareAndPublishSnapshot(
      const HostStorageSnapshotToken& expected, HostStorageSnapshotStage& stage,
      std::span<const uint8_t> intended,
      std::shared_ptr<const HostStorageSnapshotToken> recovery = {}) const;
  // Delete exactly one existing flat snapshot leaf only after revalidating its
  // complete raw bytes/identity AND its separate durable protected backup under
  // the same Mutation/Conflicts authority. Publication means actual unlink,
  // not an empty write. A successful unlink mints an absent publication token.
  ReplaceResult CompareAndDeleteSnapshot(
      const HostStorageSnapshotToken& expected,
      std::shared_ptr<const HostStorageSnapshotToken> recovery) const;
  // After published=true/durable=false, reconcile exact new bytes/identity or
  // an actually deleted slot's absence before retrying fsync. Ordinary read
  // tokens (including missing files) never convey publication authority.
  ReplaceResult RetrySnapshotDurability(
      std::shared_ptr<const HostStorageSnapshotToken>) const;
  // Revalidate an immutable observed token and sync its real file and parent.
  // This is observation, not publication or authority to retry a replacement.
  // No metadata/namespace lock may be held by the caller.
  HostStorageDurableObservation ObserveSnapshotDurability(
      const HostStorageSnapshotToken&) const;
  // Actual target-namespace reservation, revalidated against this immutable
  // flat token before return. All later IO must use the returned owned view.
  // It exempts only its real permit; foreign file/tree aliases still conflict.
  std::shared_ptr<HostStorageRoot> ReserveSnapshotFile(
      std::shared_ptr<const HostStorageSnapshotToken>, std::error_code&) const;
  bool IsSnapshotReservationOf(const HostStorageRoot&) const;
  // Metadata-only release after the view was retired and its actual consumers
  // drained. Retiring the view permanently prevents old callers resuming IO.
  std::error_code ReleaseRetiredSnapshotReservation() const;
  // Validate all exact current snapshots under ONE actual filesystem mutation
  // owner. The returned guard keeps that owner until destruction. It mints no
  // publication/retry authority. Acquire outside GCRC/model locks; consumers
  // may only TRY their metadata locks while retaining it, never wait for them.
  std::unique_ptr<HostStorageSnapshotBatch> ValidateSnapshotBatch(
      std::vector<std::shared_ptr<const HostStorageSnapshotToken>>,
      std::error_code&) const;
  // A protected immutable intent may be created only while this genuine batch
  // still owns the SAME native filesystem admission. No callbacks or nested
  // Mutation occur. Errors after exclusive creation preserve the partial file.
  IntentWriteResult
  WriteNewIntentWithSnapshotGuard(const HostStorageSnapshotBatch &,
                                  const std::filesystem::path &,
                                  std::span<const uint8_t>) const;
  std::error_code FlushDirectory(
      const std::filesystem::path& relative = {}) const;
  // Flush one existing directory subtree under the common Mutation authority.
  // Inventory is bounded to 4096 entries (including the selected directory),
  // depth 64, and retained native descriptors; OS limits/errors are preserved.
  // Every single-link regular file is synced before child-first directories.
  // Symlinks, special/multi-link files, mount crossings, protected overlaps and
  // exclusive overwrite reservations fail. Existing ordinary handles may stay
  // open, but their cooperating writes are excluded for the entire operation.
  // No data/namespace writes, capacity admission or guest/provider callbacks.
  // The caller must not hold GCRC. Unrelated host writers are outside this
  // authority. A later error can follow partial successful syncs.
  std::error_code FlushTree(
      const std::filesystem::path& relative = {}) const;
  std::error_code ReadFile(const std::filesystem::path&, uint64_t maximum,
                           std::vector<uint8_t>& output) const;
  std::unique_ptr<MappedMemory> MapRead(const std::filesystem::path&,
                                        size_t offset, size_t length,
                                        std::error_code&) const;

 private:
  friend class PolicyFileHandle;
  friend class HostStorageProtection;
  friend class ProfileIntentOwner;
  friend class SnapshotRecoveryOwner;
  friend class ContentMutationOwner;
  friend class HostStorageSnapshotTestAccess;
  friend class HostStorageManagedMetadata;
  friend class ManagedDeviceIdentity;
  friend class ManagedVolumeAuthority;
  friend class ManagedStorageContext;
  std::shared_ptr<HostStorageRoot> MountedFilesystemRoot(std::error_code&) const;
  std::shared_ptr<HostStorageRoot> IndependentNamespaceView(std::error_code&) const;
  std::error_code PrivateDirectory() const;
  void SetFormatEligibility(bool allow);
  std::error_code CheckStateContainer(bool public_shell) const;
  std::shared_ptr<const HostStorageGuestProjection> Projection() const;
  bool PublicProjectionScope() const;
  std::shared_ptr<const HostStorageGuestProjection> InstallProjection(
      std::vector<std::shared_ptr<const HostStorageRoot>> private_roots,
      std::vector<std::shared_ptr<const HostStorageRoot>> namespace_roots,
      bool allow_format, std::error_code&);
  std::error_code PrivateExcludedFrom(const HostStorageRoot&) const;
  std::error_code AttachPublicProjection(const HostStorageRoot& source);
  enum class ManagedMetadataOpen { kCreate, kExisting, kProbeExisting };
  std::shared_ptr<HostStorageManagedMetadata> OpenManagedMetadata(
      const std::filesystem::path&, std::error_code&,
      ManagedMetadataOpen = ManagedMetadataOpen::kCreate) const;
  // Native FD inventory only; matching names confer no access permission.
  uint32_t ManagedVolumeEntries(std::string_view key, std::error_code&) const;
  bool IntentLeaseMatches(const HostStorageIntentLease&) const;
  // Validated MVA handoffs may bind a new unpublished namespace to the exact
  // retained public scope. Sealed exclusions/scopes are never reset or moved.
  std::error_code AttachRetainedPublicProjection(
      const HostStorageRoot& source,
      const HostStorageGuestProjection& retained);
  std::shared_ptr<const HostStorageIntentLease> RebindFormatLease(
      const HostStorageIntentLease&, std::error_code&) const;
  ReplaceResult CompareAndDeleteSnapshotInternal(
      const HostStorageSnapshotToken& expected,
      std::shared_ptr<const HostStorageSnapshotToken> recovery,
      std::error_code post_flush_error) const;
  HostStorageRootIdentity IntentRootIdentity(std::error_code&) const;
  std::error_code IntentDisjointFrom(const HostStorageRoot&) const;
  std::shared_ptr<const HostStorageIntentLease> AcquireIntentLease(
      std::error_code&, bool profile_target = false) const;
  std::vector<std::filesystem::path> ListIntentFiles(std::error_code&) const;
  HostStorageDurableObservation ObserveIntentSnapshot(
      const HostStorageSnapshotToken&) const;
  std::unique_ptr<HostStorageContentTree> CaptureContentTree(
      const std::filesystem::path&, std::error_code&) const;
  // Private content-owner phases. Records are admitted by the actual managed
  // owner/journal, never by guest paths or caller-filled wire structures.
  std::error_code QuarantineContentPackage(
      const content_package_mutation_wire::Record&,
      const HostStorageRoot& operation) const;
  std::error_code RetireContentPackage(
      const content_package_mutation_wire::Record&,
      const HostStorageRoot& operation) const;
  std::error_code QuarantineContentTruncate(
      const content_package_mutation_wire::Record&,
      const HostStorageRoot& operation) const;
  std::error_code RetireContentTruncate(
      const content_package_mutation_wire::Record&,
      const HostStorageRoot& operation) const;
  std::error_code ContentTruncateInternal(
      const content_package_mutation_wire::Record&,
      const HostStorageRoot& operation, bool retire) const;
  std::error_code CheckContentPathCase(const std::filesystem::path&) const;
  std::shared_ptr<HostStorageRoot> ReserveContentNamespace(
      const std::filesystem::path&, const std::optional<FileHandleInfo>&,
      bool subtree, std::error_code&, bool ascii_folded_leaf = false) const;
  std::shared_ptr<HostStorageRoot> ReserveContentIo(
      const std::filesystem::path&, std::error_code&) const;
  void ReleaseContentNamespace() const;
  std::vector<ContentPackageInitialEntry> CaptureInitialContentTree(
      const std::filesystem::path&, std::error_code&) const;
  HostStorageDurableObservation ObserveInitialContentDirectory(
      const std::filesystem::path&, uint64_t,
      std::span<const ContentPackageInitialEntry>) const;
  std::error_code PublishInitialContentDirectory(
      const HostStorageRoot&, uint64_t, const std::filesystem::path&, bool&,
      std::span<const ContentPackageInitialEntry>) const;
  std::error_code PublishContentDirectoryImpl(
      const HostStorageRoot&, uint64_t, const std::filesystem::path&, bool&,
      std::span<const ContentPackageInitialEntry>) const;
  HostStorageDurableObservation ObserveContentDirectory(
      const std::filesystem::path&, uint64_t directory) const;
  std::error_code PublishContentDirectory(
      const HostStorageRoot& staged, uint64_t expected_directory,
      const std::filesystem::path& target, bool& published) const;
  std::unique_ptr<HostStorageSnapshotStage> RecoverContentSnapshotStage(
      const std::filesystem::path&, uint64_t file_id,
      std::span<const uint8_t>, std::error_code&) const;
  std::shared_ptr<HostStorageRoot> ProtectContentMutations(std::error_code&) const;
  bool is_content_mutation_private() const;
  std::vector<std::filesystem::path> ListContentMutationFiles(std::error_code&) const;
  std::vector<std::pair<std::filesystem::path, uint64_t>> ListContentStageDirectories(
      std::error_code&) const;
  const std::shared_ptr<const HostStorageFileLease>& ContentPermit() const;
  struct SnapshotReplacement;
  struct State;
  explicit HostStorageRoot(std::unique_ptr<State>);
  ReplaceResult ReplaceFile(const std::filesystem::path& staged,
                            const std::filesystem::path& target,
                            const HostStorageRoot& backup_root,
                            const std::filesystem::path& backup,
                            bool reuse_original,
                            const SnapshotReplacement* checked = nullptr) const;
  std::unique_ptr<FileHandle> OpenFileInternal(
      const std::filesystem::path&, uint32_t, std::error_code&,
      bool create_exclusive, bool overwrite,
      bool nonwaiting_admission = false, bool stage = false,
      bool* created = nullptr) const;
  std::error_code Validate() const;
  void RetainHandle() const;
  void ReleaseHandle() const;
  std::unique_ptr<HostStorageSpace::MutationGuard> Mutation(
      std::error_code&) const;
  std::unique_ptr<State> state_;
};
// Tokens retain the exact source view. They cannot be constructed from an
// alias/path guess, and expose only immutable raw bytes and stable identity.
class HostStorageSnapshotToken final {
 public:
  bool exists() const { return exists_; }
  bool ascii_folded_leaf() const { return ascii_folded_leaf_; }
  std::span<const uint8_t> bytes() const { return bytes_; }
  const FileHandleInfo& info() const { return info_; }
  const std::filesystem::path& relative() const { return relative_; }
  const std::shared_ptr<const HostStorageRoot>& root() const { return root_; }
 private:
  friend class HostStorageRoot;
  HostStorageSnapshotToken(std::shared_ptr<const HostStorageRoot> root,
      std::filesystem::path relative, bool exists, FileHandleInfo info,
      std::vector<uint8_t> bytes, bool ascii_folded_leaf = false,
      bool publication_observed = false)
      : root_(std::move(root)), relative_(std::move(relative)), exists_(exists),
        info_(info), bytes_(std::move(bytes)),
        ascii_folded_leaf_(ascii_folded_leaf),
        publication_observed_(publication_observed) {}
  std::shared_ptr<const HostStorageRoot> root_;
  std::filesystem::path relative_;
  bool exists_;
  FileHandleInfo info_;
  std::vector<uint8_t> bytes_;
  bool ascii_folded_leaf_;
  // Opaque authority minted only for a successful CAS publication receipt.
  // Ordinary load tokens can never assert that this publisher changed a file.
  bool publication_observed_;
};
// Source-owned marker file only. No raw descriptor or mutation authority is
// exposed outside the opaque managed identity producer. The retained exclusive
// name/inode lease protects the marker without excluding its whole guest tree.
class HostStorageManagedMetadata final {
 public:
  ~HostStorageManagedMetadata();

 private:
  friend class HostStorageRoot;
  friend class ManagedDeviceIdentity;
  struct WriteResult {
    std::error_code error;
    size_t bytes = 0;
    bool durable = false;
  };
  struct FormatResult {
    std::error_code error;
    bool epoch_published = false, epoch_observed = false, epoch_durable = false;
    bool data_started = false, complete = false;
    uint64_t removed_entries = 0;
    std::shared_ptr<const HostStorageRoot> fresh_source;
    std::shared_ptr<const HostStorageGuestProjection> fresh_projection;
    std::shared_ptr<const HostStorageIntentLease> fresh_source_lease;
  };
  HostStorageManagedMetadata(std::shared_ptr<const HostStorageRoot>,
                             std::filesystem::path, int, FileHandleInfo,
                             std::shared_ptr<const HostStorageFileLease>, bool,
                             std::vector<uint8_t>,
                             std::shared_ptr<HostStorageRoot>);
  std::error_code Check() const;
  WriteResult Initialize(std::span<const uint8_t>);
  WriteResult Append(std::span<const uint8_t>);
  std::error_code Synchronize() const;
  std::shared_ptr<const HostStorageIntentLease> ReserveFormatSource(
      std::error_code&) const;
  FormatResult Format(std::span<const uint8_t> pending_record,
                      std::span<const uint8_t> complete_record,
                      bool already_pending, std::atomic<bool>& old_cancellation,
                      std::error_code after_epoch_error,
                      std::shared_ptr<const HostStorageIntentLease> = {});
  void Close();
  const std::shared_ptr<const HostStorageRoot> root_;
  const std::filesystem::path leaf_;
  int fd_ = -1;
  FileHandleInfo info_;
  std::shared_ptr<const HostStorageFileLease> lease_;
  const bool created_;
  std::vector<uint8_t> bytes_;
  std::shared_ptr<HostStorageRoot> guest_view_;
};
// Move-only operation owner. Destruction closes its lease, never deletes a
// staged/uncertain file. Caller records the leaf in its recovery receipt.
class HostStorageSnapshotStage final {
 public:
  FileIoResult WriteResult(size_t, std::span<const uint8_t>);
  // Verification happens again under the common CAS mutation guard. No raw
  // native handle escapes and writes are refused after the first CAS attempt.
  std::error_code FlushResult();
  std::optional<FileHandleInfo> QueryInfo(std::error_code&);
  const std::filesystem::path& relative() const { return relative_; }
 private:
  friend class HostStorageRoot;
  HostStorageSnapshotStage(std::shared_ptr<const HostStorageRoot> root,
      std::filesystem::path relative, std::unique_ptr<FileHandle> file)
      : root_(std::move(root)), relative_(std::move(relative)),
        file_(std::move(file)) {}
  std::shared_ptr<const HostStorageRoot> root_;
  std::filesystem::path relative_;
  std::unique_ptr<FileHandle> file_;
  std::mutex mutex_;
  bool sealed_ = false;
};
// A genuine, bounded currentness proof, not a caller-filled success receipt.
// Accessors perform no I/O. Destruction releases the actual admission/flock
// owner; consumers must release model/cache/GCRC locks before the last guard.
class HostStorageSnapshotBatch final {
 public:
  ~HostStorageSnapshotBatch() = default;
  HostStorageSnapshotBatch(const HostStorageSnapshotBatch&) = delete;
  HostStorageSnapshotBatch& operator=(const HostStorageSnapshotBatch&) = delete;
  const auto& root() const { return root_; }
  const auto& tokens() const { return tokens_; }
  // Same-index native fstat metadata obtained under this exact guard. Absent
  // tokens have no file metadata. Consumers perform no path-based refresh.
  const auto& metadata() const { return metadata_; }
 private:
  friend class HostStorageRoot;
  HostStorageSnapshotBatch(std::shared_ptr<const HostStorageRoot> root,
      std::vector<std::shared_ptr<const HostStorageSnapshotToken>> tokens,
      std::vector<std::optional<FileInfo>> metadata,
      std::unique_ptr<HostStorageSpace::MutationGuard> guard)
      : root_(std::move(root)), tokens_(std::move(tokens)),
        metadata_(std::move(metadata)), guard_(std::move(guard)) {}
  const std::shared_ptr<const HostStorageRoot> root_;
  const std::vector<std::shared_ptr<const HostStorageSnapshotToken>> tokens_;
  const std::vector<std::optional<FileInfo>> metadata_;
  const std::unique_ptr<HostStorageSpace::MutationGuard> guard_;
};
}  // namespace xe::filesystem
#endif
