/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_PROFILE_PUBLICATION_INTENT_H_
#define XENIA_BASE_PROFILE_PUBLICATION_INTENT_H_
#include "xenia/base/profile_intent_owner.h"
#include "xenia/base/profile_gpd_snapshot.h"
#include <array>
#include <span>
#include <string>
#include "xenia/base/content_mutation_owner.h"
#include "xenia/base/snapshot_publication_set.h"
namespace xe::filesystem {
namespace detail {
std::error_code AdmitProfileIntentCompletion(size_t occupied, size_t unfinished);
}
enum class GpdIntentAction : uint64_t {
  kReplace = 1,
  kCreate = 2,
  kDelete = 3
};
struct GpdManagedSourceIdentity {
  ManagedDeviceIdentity::Uuid uuid{};
  uint64_t content_epoch = 0, directory = 0;
  bool operator==(const GpdManagedSourceIdentity&) const = default;
};
struct GpdIntentBinding {
  std::string source_id;
  uint32_t device_id = 0;
  uint64_t xuid = 0, source_generation = 0;
  std::filesystem::path profile_relative;
  GpdManagedSourceIdentity managed_source;
};
struct GpdIntentMutation {
  GpdIntentAction action = GpdIntentAction::kReplace;
  ProfileGpdSnapshotLoad before;
  std::vector<uint8_t> intended;
};
struct GpdIntentSummary {
  std::error_code error;
  std::filesystem::path journal;
  GpdIntentBinding binding;
  // Historical valid done record; does not assert current target bytes or
  // which process published them. Invalid/foreign/unknown records block load.
  bool done_record_valid = false;
  uint32_t format_version = 3;
};
struct GpdIntentFileReceipt {
  enum class State { kUnobserved, kBefore, kIntended, kConflict };
  State state = State::kUnobserved;
  std::error_code error;
  SnapshotPublicationReceipt publication;
  HostStorageRoot::ReplaceResult retry;
  HostStorageDurableObservation observation;
};
struct GpdIntentAttempt {
  std::error_code error;
  std::filesystem::path journal, done;
  bool intent_durable = false, done_durable = false;
  bool historically_complete = false;
  bool all_durable_sequentially_observed = false;
  std::vector<GpdIntentFileReceipt> files;
};
enum class ProfileArtifactKind : uint64_t {
  kGpd = 1, kPublic64 = 2, kPublic32 = 3, kPersonal64 = 4,
  kPersonal32 = 5, kAvatar64 = 6, kAvatar32 = 7, kThumbnail = 8
};
enum class ProfileArtifactAction : uint64_t {
  kReplace = 1, kCreate = 2, kDelete = 3, kObserve = 4
};
struct ProfileArtifactExpected {
  ProfileArtifactKind kind = ProfileArtifactKind::kGpd;
  uint32_t title_id = 0;
  std::shared_ptr<const HostStorageSnapshotToken> token;
};
struct ProfileArtifactMutation {
  ProfileArtifactKind kind = ProfileArtifactKind::kGpd;
  uint32_t title_id = 0;
  ProfileArtifactAction action = ProfileArtifactAction::kObserve;
  std::vector<uint8_t> intended;
};
// One issued actual source/profile/member admission, prepared before native
// staging. Values alone do not reconstruct its reservations or CM participant.
class ProfileArtifactAdmission final {
 public:
  ~ProfileArtifactAdmission();
  const std::vector<ProfileArtifactExpected>& expected() const;
  // Composition capability issued by actual admission, not a caller-filled
  // before/done flag. Retain through exact model/VFS publication or recovery.
  const std::shared_ptr<SnapshotPublicationSet>& members() const;
 private:
  friend class ProfilePublicationIntentStore;
  struct Data;
  explicit ProfileArtifactAdmission(std::shared_ptr<Data>);
  std::shared_ptr<Data> data_;
};
struct ProfilePublicationIntentRecord;
// Explicit storage-only owner. Caller MUST gate every profile model/guest
// mutator and retain its actual ProfileStorageLease for Prepare/Advance.
// Startup Scan/Recover must precede profile model loading/default fallback.
// No manager/device callback, native completion or model/cache publication.
// Legacy03 remains GPD-only. Explicit04 adds the closed fixed-PNG/thumbnail
// family and same-guard Done; neither format is physical cross-file atomicity.
// Both envelopes hold1..16 typed actions, aggregate raw payload<=256MiB.
// Replace, explicit observed-absent Create, and existing validated Delete are
// distinct typed actions. Delete retains raw before bytes in the durable intent
// and a protected recovery copy; empty intended bytes never authorize a write.
// Restart authority is the genuine managed UUID+format epoch and source/profile
// directory file-IDs. Runtime filesystem/mount IDs remain live admission pins.
class ProfilePublicationIntentStore final {
 public:
  ~ProfilePublicationIntentStore();
  static std::unique_ptr<ProfilePublicationIntentStore>
  Prepare(std::shared_ptr<const ProfileIntentOwner>,
          std::shared_ptr<const SnapshotRecoveryOwner>,
          GpdIntentBinding, std::vector<GpdIntentMutation>, std::error_code &,
          std::filesystem::path &attempted_journal,
          std::span<
              const std::shared_ptr<const HostStorageSnapshotToken>>
              model_dependencies = {});
  static std::shared_ptr<ProfileArtifactAdmission> AcquireArtifacts(
      std::shared_ptr<const ProfileIntentOwner>,
      std::shared_ptr<const SnapshotRecoveryOwner>,
      std::shared_ptr<ContentMutationOwner>, GpdIntentBinding,
      std::vector<ProfileArtifactExpected> main_members, bool include_thumbnail,
      std::error_code&);
  static std::unique_ptr<ProfilePublicationIntentStore> PrepareArtifacts(
      std::shared_ptr<ProfileArtifactAdmission>,
      std::vector<ProfileArtifactMutation>, std::error_code&,
      std::filesystem::path& attempted_journal,
      std::span<const std::shared_ptr<const HostStorageSnapshotToken>> dependencies = {});
  // Pure canonical profile-name derivation, never storage authority.
  static std::filesystem::path ProfileRelative(uint64_t xuid);
  // Called with actual owners before MVA guest admission. Canonical04 replays
  // without a profile slot/default model. Legacy03 retains lease-driven replay;
  // unknown/foreign records of either version fail without adoption.
  static std::error_code ReplayBeforeGuestAdmission(
      std::shared_ptr<const ProfileIntentOwner>,
      std::shared_ptr<const SnapshotRecoveryOwner>,
      std::shared_ptr<ContentMutationOwner>);
  static std::vector<GpdIntentSummary> Scan(
      const ProfileIntentOwner&, std::error_code&);
  // Explicit stable binding supplied by real device/profile authority, never
  // resolved from an intent path/alias. Runtime source generation may differ
  // after restart; the journal retains its original generation as provenance.
  static std::unique_ptr<ProfilePublicationIntentStore> Recover(
      std::shared_ptr<const ProfileIntentOwner>,
      std::shared_ptr<const SnapshotRecoveryOwner>,
      const std::filesystem::path& journal, GpdIntentBinding current_binding,
      std::shared_ptr<const HostStorageRoot> exact_profile,
      std::error_code&, std::shared_ptr<ContentMutationOwner> metadata_owner = {});
  GpdIntentAttempt Advance();
  const std::filesystem::path& journal() const { return journal_; }

 private:
  ProfilePublicationIntentStore() = default;
  struct ArtifactState;
  static std::shared_ptr<ProfileArtifactAdmission> AcquireArtifactsInternal(
      std::shared_ptr<const ProfileIntentOwner>,
      std::shared_ptr<const SnapshotRecoveryOwner>,
      std::shared_ptr<ContentMutationOwner>, GpdIntentBinding,
      std::vector<ProfileArtifactExpected>, bool, std::error_code&,
      const std::filesystem::path& recovering_journal = {});
  static std::unique_ptr<ProfilePublicationIntentStore> RecoverArtifacts(
      std::shared_ptr<const ProfileIntentOwner>,
      std::shared_ptr<const SnapshotRecoveryOwner>, std::shared_ptr<ContentMutationOwner>,
      const std::filesystem::path&, GpdIntentBinding,
      std::shared_ptr<const HostStorageRoot>, std::error_code&);
  GpdIntentAttempt AdvanceArtifacts();
  std::unique_ptr<ArtifactState> artifacts_;

  static std::vector<GpdIntentSummary> ScanLocked(
      const ProfileIntentOwner&, std::error_code&);
  std::shared_ptr<const ProfileIntentOwner> owner_;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery_;
  std::shared_ptr<const HostStorageRoot> view_;
  std::shared_ptr<const HostStorageSnapshotToken> journal_token_;
  std::filesystem::path journal_;
  std::unique_ptr<ProfilePublicationIntentRecord> record_;
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>>
      published_;
  std::shared_ptr<const HostStorageIntentLease> target_lease_;
  bool recovered_ = false;
};
}  // namespace xe::filesystem
#endif
