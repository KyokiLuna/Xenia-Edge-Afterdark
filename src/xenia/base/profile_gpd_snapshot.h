/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_PROFILE_GPD_SNAPSHOT_H_
#define XENIA_BASE_PROFILE_GPD_SNAPSHOT_H_
#include "xenia/base/snapshot_publisher.h"
namespace xe::filesystem {
inline constexpr uint64_t kMaximumProfileGpdSnapshotBytes = 64ull << 20;
// Detached source/format observations only. A real retained storage owner and
// snapshot token still authorize every actual observation and mutation.
struct ProfileGpdSnapshotIdentity {
  uint64_t xuid = 0;
  uint64_t source_generation = 0;
  uint32_t title_id = 0;
};
struct ProfileGpdSnapshotLoad {
  std::error_code error;
  ProfileGpdSnapshotIdentity identity;
  std::shared_ptr<const HostStorageSnapshotToken> token;
  bool missing() const { return !error && token && !token->exists(); }
  bool readable() const { return !error && token && token->exists(); }
};
struct ProfileGpdSnapshotWrite {
  bool serialized = false;
  ProfileGpdSnapshotIdentity identity;
  SnapshotPublicationReceipt storage;
};
// Deletion is an explicit typed operation, never serialized/empty GPD data.
struct ProfileGpdSnapshotDelete {
  ProfileGpdSnapshotIdentity identity;
  SnapshotPublicationReceipt storage;
};
std::error_code ValidateProfileGpdSnapshot(std::span<const uint8_t>);
// The strict format-neutral byte store is shared with pre-admission redo.
// Native model/manager lifetimes and logical publication are separate duties.
class ProfileGpdSnapshotStore final {
 public:
  static constexpr uint64_t kMaximumBytes = kMaximumProfileGpdSnapshotBytes;
  static ProfileGpdSnapshotLoad Load(ProfileGpdSnapshotIdentity,
      std::shared_ptr<const HostStorageRoot> exact_profile_view);
  static ProfileGpdSnapshotWrite Publish(const SnapshotRecoveryOwner&,
      const ProfileGpdSnapshotLoad&, std::span<const uint8_t>,
      const std::atomic<bool>* canceled = nullptr);
  static ProfileGpdSnapshotDelete Delete(const SnapshotRecoveryOwner&,
      const ProfileGpdSnapshotLoad&, const std::atomic<bool>* canceled = nullptr);
};
}  // namespace xe::filesystem
#endif
