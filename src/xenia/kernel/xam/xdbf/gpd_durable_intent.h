/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_XDBF_GPD_DURABLE_INTENT_H_
#define XENIA_KERNEL_XAM_XDBF_GPD_DURABLE_INTENT_H_
#include "xenia/base/profile_intent_owner.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
#include <array>
#include <span>
#include <string>
namespace xe::kernel::xam {
enum class GpdIntentAction : uint64_t {
  kReplace = 1,
  kCreate = 2,
  kDelete = 3
};
struct GpdManagedSourceIdentity {
  filesystem::ManagedDeviceIdentity::Uuid uuid{};
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
  GpdSnapshotLoad before;
  std::vector<uint8_t> intended;
};
struct GpdIntentSummary {
  std::error_code error;
  std::filesystem::path journal;
  GpdIntentBinding binding;
  // Historical valid done record; does not assert current target bytes or
  // which process published them. Invalid/foreign/unknown records block load.
  bool done_record_valid = false;
};
struct GpdIntentFileReceipt {
  enum class State { kUnobserved, kBefore, kIntended, kConflict };
  State state = State::kUnobserved;
  std::error_code error;
  filesystem::SnapshotPublicationReceipt publication;
  filesystem::HostStorageRoot::ReplaceResult retry;
  filesystem::HostStorageDurableObservation observation;
};
struct GpdIntentAttempt {
  std::error_code error;
  std::filesystem::path journal, done;
  bool intent_durable = false, done_durable = false;
  bool historically_complete = false;
  bool all_durable_sequentially_observed = false;
  std::vector<GpdIntentFileReceipt> files;
};
struct GpdDurableIntentRecord;
// Explicit storage-only owner. Caller MUST gate every profile model/guest
// mutator and retain its actual ProfileStorageLease for Prepare/Advance.
// Startup Scan/Recover must precede profile model loading/default fallback.
// No manager/device callback, user notification or cross-file atomicity claim.
// Version1 envelope holds1..16 typed actions, aggregate raw payload<=256MiB.
// Replace, explicit observed-absent Create, and existing validated Delete are
// distinct typed actions. Delete retains raw before bytes in the durable intent
// and a protected recovery copy; empty intended bytes never authorize a write.
// Restart authority is the genuine managed UUID+format epoch and source/profile
// directory file-IDs. Runtime filesystem/mount IDs remain live admission pins.
class GpdDurableIntentStore final {
 public:
  ~GpdDurableIntentStore();
  static std::unique_ptr<GpdDurableIntentStore>
  Prepare(std::shared_ptr<const filesystem::ProfileIntentOwner>,
          std::shared_ptr<const filesystem::SnapshotRecoveryOwner>,
          GpdIntentBinding, std::vector<GpdIntentMutation>, std::error_code &,
          std::filesystem::path &attempted_journal,
          std::span<
              const std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
              model_dependencies = {});
  static std::vector<GpdIntentSummary> Scan(
      const filesystem::ProfileIntentOwner&, std::error_code&);
  // Explicit stable binding supplied by real device/profile authority, never
  // resolved from an intent path/alias. Runtime source generation may differ
  // after restart; the journal retains its original generation as provenance.
  static std::unique_ptr<GpdDurableIntentStore> Recover(
      std::shared_ptr<const filesystem::ProfileIntentOwner>,
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner>,
      const std::filesystem::path& journal, GpdIntentBinding current_binding,
      std::shared_ptr<const filesystem::HostStorageRoot> exact_profile,
      std::error_code&);
  GpdIntentAttempt Advance();
  const std::filesystem::path& journal() const { return journal_; }

 private:
  GpdDurableIntentStore() = default;
  static std::vector<GpdIntentSummary> ScanLocked(
      const filesystem::ProfileIntentOwner&, std::error_code&);
  std::shared_ptr<const filesystem::ProfileIntentOwner> owner_;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_;
  std::shared_ptr<const filesystem::HostStorageRoot> view_;
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> journal_token_;
  std::filesystem::path journal_;
  std::unique_ptr<GpdDurableIntentRecord> record_;
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      published_;
  std::shared_ptr<const filesystem::HostStorageIntentLease> target_lease_;
  bool recovered_ = false;
};
}  // namespace xe::kernel::xam
#endif
