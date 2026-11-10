/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_XDBF_GPD_SNAPSHOT_H_
#define XENIA_KERNEL_XAM_XDBF_GPD_SNAPSHOT_H_
#include "xenia/base/snapshot_publisher.h"
namespace xe::kernel::xam {
class GpdInfo;
// Supplied by a real ProfileManager storage operation; not inferred from a
// User_ alias. Caller MUST retain its actual ProfileStorageLease until this
// synchronous operation returns and postcommit refresh is scheduled. Retaining
// view alone is not permission to dismantle its VFS device during the call.
struct GpdSnapshotIdentity {
  uint64_t xuid = 0;
  uint64_t source_generation = 0;
  uint32_t title_id = 0;
};
struct GpdSnapshotLoad {
  std::error_code error;
  GpdSnapshotIdentity identity;
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> token;
  bool missing() const { return !error && token && !token->exists(); }
  bool readable() const { return !error && token && token->exists(); }
};
struct GpdSnapshotWrite {
  bool serialized = false;
  GpdSnapshotIdentity identity;
  filesystem::SnapshotPublicationReceipt storage;
};
// Deletion is an explicit typed operation, never serialized/empty GPD data.
struct GpdSnapshotDelete {
  GpdSnapshotIdentity identity;
  filesystem::SnapshotPublicationReceipt storage;
};
// Strict wire-bound validation BEFORE constructing GpdInfo or authorizing a
// serialized snapshot. Unknown used records and raw bytes are accepted intact;
// no default model is synthesized for missing/unreadable/malformed payloads.
std::error_code ValidateGpdSnapshot(std::span<const uint8_t>);
class GpdSnapshotStore final {
 public:
  static constexpr uint64_t kMaximumBytes = 64ull << 20;
  static GpdSnapshotLoad Load(GpdSnapshotIdentity,
      std::shared_ptr<const filesystem::HostStorageRoot> exact_profile_view);
  // Only already serialized bytes are accepted. This does not make the legacy
  // GpdInfo::Serialize safe for invalid models or wire it into UserProfile.
  static GpdSnapshotWrite Publish(const filesystem::SnapshotRecoveryOwner&,
      const GpdSnapshotLoad&, std::span<const uint8_t> serialized,
      const std::atomic<bool>* canceled = nullptr);
  static GpdSnapshotWrite Publish(const filesystem::SnapshotRecoveryOwner&,
      const GpdSnapshotLoad&, const GpdInfo&,
      const std::atomic<bool>* canceled = nullptr);
  static GpdSnapshotDelete Delete(const filesystem::SnapshotRecoveryOwner&,
                                  const GpdSnapshotLoad&,
                                  const std::atomic<bool>* canceled = nullptr);
};
}  // namespace xe::kernel::xam
#endif
