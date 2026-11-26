/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_PROFILE_GPD_TRANSACTION_H_
#define XENIA_KERNEL_XAM_PROFILE_GPD_TRANSACTION_H_
#include <chrono>
#include <functional>
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/xdbf/gpd_durable_intent.h"
#include "xenia/vfs/installed_system_source.h"
namespace xe::kernel::xam {
// Synchronous borrowed caller policy, never an ownership/read/IO permit. The
// real preparation frame and its existing deadline outlive every nested use.
// Call Retry only AFTER all model/cache/batch/namespace guards are destroyed.
struct ProfilePreparationProgress final {
  std::chrono::steady_clock::time_point deadline;
  const std::function<bool()>& wait;
  const std::atomic<bool>* admission_open=nullptr; // borrowed negative-only stop flag
  bool Retry(const std::error_code&) const;
};
// Live-epoch event only. No persisted UI protocol: replayed historical awards
// restore data and NEVER manufacture a new event/counter increment.
class DurableAchievementEvent final {
 public:
  const auto& origin() const { return origin_; }
  const auto& command() const { return pending_->command(); }
  bool TryConsume() const { return !consumed_.exchange(true); }
  bool Current() const;

 private:
  friend class ProfileGpdTransaction;
  DurableAchievementEvent(std::shared_ptr<const ProfileGpdPendingMutation>,
                          ProfileGpdMutationOrigin);
  const std::shared_ptr<const ProfileGpdPendingMutation> pending_;
  const ProfileGpdMutationOrigin origin_;
  mutable std::atomic<bool> consumed_{false};
};
struct ProfileGpdTransactionAttempt {
  enum class State {
    kPending,
    kDurablePendingCommit,
    kCommitted,
    kAlreadyCommitted,
    kAlreadyUnlocked,
    kRetired,
    kPreparationUncertain
  };
  State state = State::kPending;
  std::error_code error;
  GpdIntentAttempt storage;
  std::filesystem::path attempted_journal;
  std::shared_ptr<const DurableAchievementEvent> event;
};
// Captured at ingress under the manager's short ownership guard. Safe to keep
// across deferred IO; no manager, UserProfile or caller-slot lookup later.
struct ProfileGpdWriter {
  std::shared_ptr<ProfileGpdModelAuthority> model;
  std::shared_ptr<const filesystem::ProfileIntentOwner> owner;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery;
  std::shared_ptr<filesystem::ContentMutationOwner> metadata;
  // Observing IO stays outside metadata/GCRC. A pending exact retry retains
  // its original clone/stamp; no reload can normalize partially published data.
  std::optional<ProfileGpdMutationOrigin>
  CaptureCurrentOrigin(const ProfileGpdMutation &, std::error_code &) const;
  std::error_code Reload(const ProfilePreparationProgress* progress=nullptr) const;
  // Selected-model preparation only, before native roster publication. Only
  // demonstrably LE stock keys are repaired through the normal durable writer.
  // Valid/unknown/custom data is preserved; read-only observers never coerce it.
  std::error_code RepairLegacyPictureKeys(
      std::shared_ptr<const vfs::InstalledSystemSource::Snapshot>,
      const ProfilePreparationProgress* progress=nullptr) const;
  ProfileGpdTransactionAttempt Submit(
      ProfileGpdMutation,
      std::optional<ProfileGpdModelStamp> required_stamp = {}) const;
  ProfileGpdTransactionAttempt ExecutePending(
      std::shared_ptr<ProfileGpdPendingMutation>) const;
};
struct CapturedAchievementAward {
  ProfileGpdWriter writer;
  ProfileGpdMutation command;
  ProfileGpdMutationOrigin origin;
  ProfileGpdTransactionAttempt Execute() const {
    return writer.Submit(command, origin.stamp);
  }
};
// One real pending host mutation. It owns no ProfileManager/UserProfile/SPA
// pointer and invokes no UI or guest callback. All IO runs outside GCRC. The
// model owns this object before the first journal attempt, so retries reuse the
// SAME cloned intent even if one member published or fsync outcome is
// uncertain.
class ProfileGpdTransaction final {
 public:
  static std::shared_ptr<ProfileGpdTransaction> Create(
      std::shared_ptr<ProfileGpdModelAuthority>,
      std::shared_ptr<ProfileGpdPendingMutation>,
      std::shared_ptr<const filesystem::ProfileIntentOwner>,
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner>,
      std::error_code&, std::shared_ptr<filesystem::ContentMutationOwner> metadata = {});
  ProfileGpdTransactionAttempt Attempt();
  // Only after real retirement and outside all namespace/model locks. A busy
  // in-flight operation retains its store/target lease until later retry.
  std::error_code ReleaseForRecovery();
  // Call BEFORE profile model/default loading. Reconciles actual before,
  // intended or conflict using the journal's explicit authority. No UI event.
  static std::error_code ReplayBeforeLoad(
      std::shared_ptr<ProfileStorageLease>,
      std::shared_ptr<const filesystem::ProfileIntentOwner>,
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner>,
      std::shared_ptr<filesystem::ContentMutationOwner> metadata = {});

 private:
  friend class ProfileGpdModelAuthority;
  friend class ::xe::kernel::NativeProfileSavePublication;
  static std::shared_ptr<ProfileGpdTransaction> CreateImpl(
      std::shared_ptr<ProfileGpdModelAuthority>,
      std::shared_ptr<ProfileGpdPendingMutation>,
      std::shared_ptr<const filesystem::ProfileIntentOwner>,
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner>, std::error_code&,
      std::shared_ptr<filesystem::ContentMutationOwner>,
      std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication>);
  ProfileGpdTransactionAttempt AttemptNative(
      std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication>);
  // Same retained native transaction only. A definitely unstarted failure
  // cannot be turned into a later write after original caller failure/cancel.
  ProfileGpdTransactionAttempt AttemptNativeRecovery(
      std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication>);
  ProfileGpdTransactionAttempt AttemptImpl(
      std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication>,
      bool attempted_recovery_only = false);
  std::error_code CancelNativeUnpublished(
      const std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication>&);
  ProfileGpdTransaction() = default;
  static GpdIntentBinding Binding(const ProfileStorageLease&,
                                  const filesystem::ProfileIntentOwner&);
  std::mutex mutex_;
  std::weak_ptr<ProfileGpdModelAuthority> model_;
  std::shared_ptr<ProfileGpdPendingMutation> pending_;
  std::shared_ptr<const filesystem::ProfileIntentOwner> owner_;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_;
  std::shared_ptr<filesystem::ContentMutationOwner> metadata_;
  std::shared_ptr<filesystem::ProfileArtifactAdmission> artifact_admission_;
  std::unique_ptr<GpdDurableIntentStore> store_;
  std::filesystem::path attempted_journal_;
  bool preparation_uncertain_ = false, committed_ = false;
};
}  // namespace xe::kernel::xam
#endif
