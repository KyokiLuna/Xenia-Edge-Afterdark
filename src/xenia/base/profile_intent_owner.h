/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_PROFILE_INTENT_OWNER_H_
#define XENIA_BASE_PROFILE_INTENT_OWNER_H_
#include <mutex>
#include "xenia/base/host_storage.h"
#include "xenia/base/managed_device_identity.h"
namespace xe::kernel::xam {
class GpdDurableIntentStore;
}
namespace xe::filesystem {
// Explicit Emulator bootstrap BEFORE any guest mount. Distinct from console
// configuration and the existing single-file snapshot recovery authority.
class ProfileIntentOwner final {
 public:
  static constexpr auto kDirectoryPrefix = ".xenia-profile-intents-v3-";
  // external_private_parent is the one actual early managed private authority
  // supplied by Emulator, outside every guest backing. registered_source is
  // the actual retained managed device root, not a guessed alias/journal path.
  // The deterministic child and marker bind managed UUID+content epoch.
  // Filesystem/mount IDs remain separate live admission pins.
  static std::unique_ptr<ProfileIntentOwner> Open(
      std::shared_ptr<const HostStorageRoot> external_private_parent,
      std::shared_ptr<const ManagedDeviceIdentity> registered_source);
  static std::unique_ptr<ProfileIntentOwner> OpenExisting(
      std::shared_ptr<const HostStorageRoot>,
      std::shared_ptr<const ManagedDeviceIdentity>);
  const std::error_code& error() const { return error_; }
  const auto& root() const { return root_; }
  const auto& source() const { return source_; }
  const auto& private_parent() const { return private_parent_; }
  const auto& source_identity() const {
    return source_identity_;
  }  // Live admission only.
  const auto& managed_identity() const { return managed_identity_; }

 private:
  friend class xe::kernel::xam::GpdDurableIntentStore;
  friend class ManagedVolumeAuthority;
  static std::unique_ptr<ProfileIntentOwner> OpenInternal(
      std::shared_ptr<const HostStorageRoot>,
      std::shared_ptr<const ManagedDeviceIdentity>, bool create,
      std::shared_ptr<const HostStorageIntentLease> source_lease = {});
  // Called only after a complete durable journal has been decoded/verified.
  // Existing intended bytes OR explicitly journaled intended absence can be
  // observed durably. This never claims publication or mints a retry token.
  HostStorageDurableObservation Observe(
      const HostStorageSnapshotToken& token) const;
  HostStorageRootIdentity Identity(const HostStorageRoot& root,
                                   std::error_code&) const;
  ProfileIntentOwner() = default;
  std::vector<std::filesystem::path> List(std::error_code&) const;
  std::shared_ptr<const HostStorageIntentLease> AcquireTarget(
      const HostStorageRoot&, std::error_code&) const;
  std::shared_ptr<const HostStorageRoot> private_parent_, source_;
  std::shared_ptr<HostStorageRoot> root_;
  // Retain a real independent directory-description flock until the last
  // store/operation releases the owner. Never borrow a device or manager.
  std::shared_ptr<const HostStorageIntentLease> lease_, source_lease_;
  std::shared_ptr<const ManagedDeviceIdentity> managed_identity_;
  HostStorageRootIdentity source_identity_;
  mutable std::mutex mutex_;
  std::error_code error_;
};
}  // namespace xe::filesystem
#endif
