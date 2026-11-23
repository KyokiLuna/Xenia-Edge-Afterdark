/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
namespace xe::kernel::xam {
std::error_code ValidateGpdSnapshot(std::span<const uint8_t> bytes) {
  return filesystem::ValidateProfileGpdSnapshot(bytes);
}
GpdSnapshotLoad GpdSnapshotStore::Load(GpdSnapshotIdentity identity,
    std::shared_ptr<const filesystem::HostStorageRoot> view) {
  return filesystem::ProfileGpdSnapshotStore::Load(identity, std::move(view));
}
GpdSnapshotWrite GpdSnapshotStore::Publish(
    const filesystem::SnapshotRecoveryOwner& owner,
    const GpdSnapshotLoad& loaded, std::span<const uint8_t> serialized,
    const std::atomic<bool>* canceled) {
  return filesystem::ProfileGpdSnapshotStore::Publish(owner, loaded, serialized,
                                                      canceled);
}
GpdSnapshotWrite GpdSnapshotStore::Publish(
    const filesystem::SnapshotRecoveryOwner& owner,
    const GpdSnapshotLoad& loaded, const GpdInfo& model,
    const std::atomic<bool>* canceled) {
  // Load failure takes precedence and must never authorize an empty model.
  if (loaded.error) {
    GpdSnapshotWrite result;
    result.identity = loaded.identity;
    result.storage.expected = loaded.token;
    result.storage.error = loaded.error;
    return result;
  }
  std::vector<uint8_t> serialized;
  const auto error = model.SerializeChecked(serialized, kMaximumBytes);
  if (error) {
    GpdSnapshotWrite result;
    result.identity = loaded.identity;
    result.storage.expected = loaded.token;
    result.storage.error = error;
    return result;
  }
  return Publish(owner, loaded, serialized, canceled);
}
GpdSnapshotDelete GpdSnapshotStore::Delete(
    const filesystem::SnapshotRecoveryOwner& owner,
    const GpdSnapshotLoad& loaded, const std::atomic<bool>* canceled) {
  return filesystem::ProfileGpdSnapshotStore::Delete(owner, loaded, canceled);
}
}  // namespace xe::kernel::xam
