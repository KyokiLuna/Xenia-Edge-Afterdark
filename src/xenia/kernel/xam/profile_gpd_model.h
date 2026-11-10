/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_PROFILE_GPD_MODEL_H_
#define XENIA_KERNEL_XAM_PROFILE_GPD_MODEL_H_
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include "xenia/kernel/xam/profile_gpd_publication.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_info_title.h"
namespace xe::filesystem {
class ProfileIntentOwner;
}
namespace xe::kernel::xam {
std::error_code ValidateProfileGpdRecords(std::span<const uint8_t>,
                                          bool dashboard);
class ProfileGpdModelAuthority;
class ProfileGpdTransaction;
struct ProfileGpdReadModels {
  explicit ProfileGpdReadModels(std::span<const uint8_t> dashboard_bytes)
      : dashboard(std::vector<uint8_t>(dashboard_bytes.begin(),
                                       dashboard_bytes.end())) {}
  GpdInfoProfile dashboard;
  std::map<uint32_t, GpdInfoTitle> titles;
  std::optional<UserSetting> Setting(uint32_t title, uint32_t id,
                                     std::error_code&) const;
};
enum class ProfileGpdMutationKind {
  kUnlock,
  kSetSetting,
  kSetSettings,
  kAdjustSetting,
  kTouchTitle,
  kRemoveTitle,
  kRefreshTitleData,
  kFillMissingIcons,
  kRefreshProfileData,
  kRefreshSummary,
  kReload,
  kFlush
};
// A captured SPA projection owns every string/image/achievement. It never
// borrows SpaInfo, its buffers or a currently executing title module.
struct ProfileGpdTitleData {
  uint32_t title_id = 0, achievement_count = 0, total_gamerscore = 0;
  bool include_in_profile = false;
  std::u16string title_name;
  std::vector<AchievementDetails> achievements;
  std::map<uint32_t, std::vector<uint8_t>> images;
  std::vector<uint8_t> title_icon;
  // Requires a genuine owned SPA, not a no-op shared_ptr alias to spa_info().
  // The current XamState raw getter is not an ownership/admission API.
  static std::shared_ptr<const ProfileGpdTitleData> Capture(
      std::shared_ptr<const SpaInfo>, XLanguage);
};
struct ProfileGpdMutation {
  ProfileGpdMutationKind kind = ProfileGpdMutationKind::kFlush;
  uint32_t title_id = 0, record_id = 0;
  // Non-unlock commands require their actual immutable ingress request ID.
  // Unlock retries are keyed by (sign-in generation, title, achievement).
  uint64_t request_id = 0, guest_time = 0;
  uint64_t foreground_generation = 0, domain_id = 0;
  int32_t delta = 0;
  std::optional<UserSetting> setting;
  std::vector<UserSetting> settings;
  std::shared_ptr<const ProfileGpdTitleData> title_data;
};
enum class ProfileGpdFileAction { kUnchanged, kReplace, kCreate, kDelete };
struct ProfileGpdPlannedFile {
  GpdSnapshotLoad before;
  ProfileGpdFileAction action = ProfileGpdFileAction::kUnchanged;
  std::vector<uint8_t> intended;
};
struct ProfileGpdModelPlan {
  std::map<uint32_t, ProfileGpdPlannedFile> files;
  bool already_unlocked = false;
  // Derived unlock-plan shape only. Real load tokens and the typed journal
  // authorize writes; other mutators use explicit Create/Delete, never an
  // empty write or a legacy publication fallback.
  bool two_existing_replacements = false;
};
struct ProfileGpdMutationOrigin {
  uint64_t xuid = 0;
  uint32_t user_slot = 0;
  ProfileGpdModelStamp stamp;
  std::shared_ptr<ProfileStorageLease> storage;
  // Local boot/owner lifetime identity, never serialized as restart identity.
  std::weak_ptr<const ProfileGpdModelAuthority> owner;
};
struct ProfileGpdReadCapture {
  ProfileGpdMutationOrigin origin;
  std::shared_ptr<const ProfileGpdReadModels> models;
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      tokens;
};
// A pending command is independent of UserProfile and survives its destruction.
// The authority owns it until a later real journal/cache commit or explicit
// recovery; no final bool-based completion/notification API is invented here.
class ProfileGpdPendingMutation final {
 public:
  const auto& origin() const { return origin_; }
  const auto& command() const { return command_; }
  bool Matches(const ProfileGpdMutation &) const;
  std::shared_ptr<const ProfileGpdModelPlan> BuildPlan(std::error_code&);

 private:
  friend class ProfileGpdModelAuthority;
  ProfileGpdPendingMutation(
      ProfileGpdMutationOrigin, ProfileGpdMutation,
      std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>>);
  const ProfileGpdMutationOrigin origin_;
  const ProfileGpdMutation command_;
  const std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>> baselines_;
  std::mutex mutex_;
  std::shared_ptr<const ProfileGpdModelPlan> plan_;
};
// This is the actual proposed per-profile metadata owner. ProfileManager mints
// its sign-in generation and retains old owners with pending work on sign-out.
// It has no raw ProfileManager/UserProfile/SPA references and performs no IO.
class ProfileGpdModelAuthority final
    : public std::enable_shared_from_this<ProfileGpdModelAuthority> {
 public:
  class PreparedCommit;
  class CommitGuard;
  static std::shared_ptr<ProfileGpdModelAuthority> Create(
      uint64_t xuid, uint32_t slot, uint64_t sign_in_generation,
      std::shared_ptr<ProfileStorageLease>, std::error_code&);
  uint64_t xuid() const { return xuid_; }
  uint64_t AllocateRequestId(std::error_code&);
  const auto& storage() const { return storage_; }
  std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>> CaptureLoads(
      std::error_code&) const;
  std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>> Dependencies(
      const std::shared_ptr<ProfileGpdPendingMutation>&,
      std::error_code&) const;
  uint32_t user_slot() const { return slot_; }
  uint64_t sign_in_generation() const { return sign_in_generation_; }
  void Retire();
  // Only genuine Load results qualify; absent is retained as absent and can be
  // consumed only by the explicit TouchTitle creation policy, never as repair.
  std::error_code InstallLoaded(std::map<uint32_t, GpdSnapshotLoad>);
  std::unique_ptr<PreparedCommit> PrepareLoaded(
      std::map<uint32_t, GpdSnapshotLoad>, std::error_code&);
  // Returns SAME pending object for an exact retry before any IsUnlocked test.
  // A different operation cannot mutate the model while an intent is pending.
  std::shared_ptr<ProfileGpdPendingMutation> Begin(
      ProfileGpdMutation, bool& retry, std::error_code&,
      std::optional<ProfileGpdModelStamp> required_stamp = {});
  std::optional<ProfileGpdMutationOrigin> CaptureOrigin(std::error_code&) const;
  std::shared_ptr<const ProfileGpdPendingMutation> Pending(
      std::error_code&) const;
  bool IsCurrent(const ProfileGpdMutationOrigin&) const;
  std::shared_ptr<const ProfileGpdReadModels> Read(std::error_code&) const;
  std::optional<ProfileGpdReadCapture> CaptureRead(std::error_code&) const;
  std::shared_ptr<ProfileGpdTransaction> AcquireTransaction(
      std::shared_ptr<const filesystem::ProfileIntentOwner>,
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner>,
      std::error_code&);
  std::error_code ReleaseRetiredPublication();
  // Parse/allocate detached immutable models before acquiring a storage batch
  // or namespace/model locks. No caller-filled durable-completion flag exists.
  std::unique_ptr<PreparedCommit> PrepareCommit(
      std::shared_ptr<ProfileGpdPendingMutation>,
      std::map<uint32_t, GpdSnapshotLoad> current, std::error_code&);
  std::unique_ptr<CommitGuard> TryCommit(
      std::unique_ptr<PreparedCommit>,
      std::shared_ptr<const filesystem::HostStorageSnapshotBatch>,
      std::error_code&);

 private:
  friend class ProfileGpdTransaction;
  void DiscardUnpublished(const std::shared_ptr<ProfileGpdPendingMutation>&,
                          const ProfileGpdTransaction*);
  ProfileGpdModelAuthority(uint64_t, uint32_t, uint64_t,
                           std::shared_ptr<ProfileStorageLease>);
  const uint64_t xuid_, sign_in_generation_;
  const uint32_t slot_;
  const std::shared_ptr<ProfileStorageLease> storage_;
  mutable std::mutex mutex_;
  std::atomic<bool> retired_{false};
  uint64_t version_ = 0, next_request_id_ = 1;
  std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>> baselines_;
  std::shared_ptr<const ProfileGpdReadModels> models_;
  std::shared_ptr<ProfileGpdPendingMutation> pending_;
  std::shared_ptr<ProfileGpdTransaction> pending_transaction_;
};
class ProfileGpdModelAuthority::PreparedCommit final {
 public:
  const auto& tokens() const { return checked; }

 private:
  friend class ProfileGpdModelAuthority;
  friend class CommitGuard;
  std::shared_ptr<ProfileGpdPendingMutation> pending;
  uint64_t expected_version = 0;
  std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>> baselines;
  std::shared_ptr<const ProfileGpdReadModels> models;
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      checked;
};
class ProfileGpdModelAuthority::CommitGuard final {
 public:
  CommitGuard(const CommitGuard&) = delete;
  // Returns false on logical retirement before commit; performs no mutation
  // in that case. A retirement racing a successful commit still invalidates
  // notification origin. Caller keeps the namespace cache guard until done.
  bool Apply() noexcept;

 private:
  friend class ProfileGpdModelAuthority;
  CommitGuard() = default;
  std::shared_ptr<ProfileGpdModelAuthority> owner_;
  std::unique_ptr<PreparedCommit> prepared_;
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch_;
  std::unique_lock<std::mutex> lock_;
  bool applied_ = false;
};
}  // namespace xe::kernel::xam
#endif
