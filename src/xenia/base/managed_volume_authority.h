/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_MANAGED_VOLUME_AUTHORITY_H_
#define XENIA_BASE_MANAGED_VOLUME_AUTHORITY_H_
#include <optional>
#include "xenia/base/managed_device_identity.h"
#include "xenia/base/profile_intent_owner.h"
#include "xenia/base/snapshot_publisher.h"
namespace xe::filesystem {
class ManagedVolumeFormatHandoff;
class ContentMutationOwner;
// Retained process storage inputs, independent of KernelState or settings
// payloads. Capture groups existing authorities; it creates no journal engine.
// Per-device same-FS initialization remains ManagedVolumeAuthority::Open.
class ManagedStorageContext final {
 public:
  static std::shared_ptr<const ManagedStorageContext> Capture(
      std::shared_ptr<HostStorageRoot> data_parent,
      std::shared_ptr<const SnapshotRecoveryOwner> legacy_recovery,
      std::shared_ptr<HostStorageRoot> state_parent_override, std::error_code&);
  const auto& data_parent() const { return data_parent_; }
  const auto& legacy_recovery() const { return legacy_recovery_; }
  const auto& state_parent_override() const { return state_parent_override_; }
  std::error_code CheckCurrent() const;

 private:
  ManagedStorageContext() = default;
  std::shared_ptr<HostStorageRoot> data_parent_, state_parent_override_;
  std::shared_ptr<const SnapshotRecoveryOwner> legacy_recovery_;
};
// One host initialization result for one exact managed Content generation.
// Failure objects retain created/uncertain private authorities and exclusions.
// Neither this bundle nor Content's path changes the managed UUID/XUID/layout.
class ManagedVolumeAuthority final {
 public:
  struct Options {
    std::shared_ptr<HostStorageRoot> content;
    std::string source_id;
    uint32_t device_id = 0;
    // Explicit legacy primary policy only. Never infer Parent(Content).
    std::shared_ptr<HostStorageRoot> legacy_private_parent;
    std::shared_ptr<HostStorageRoot> legacy_public_root;
    std::shared_ptr<const SnapshotRecoveryOwner> legacy_recovery;
    bool preserve_legacy_layout = false;
    // Explicit known pre-V1 producer locations. Only fully existing validated
    // owners may be reused; this is not permission to create/migrate journals.
    struct ExistingLegacy {
      std::shared_ptr<HostStorageRoot> private_parent, public_root;
      std::shared_ptr<const SnapshotRecoveryOwner> recovery;
    };
    std::optional<ExistingLegacy> existing_legacy;
    // Optional explicit existing writable same-FS parent for the typed state
    // and public-shell directories. The parent is not adopted as private.
    std::shared_ptr<HostStorageRoot> state_parent_override;
  };
  static std::shared_ptr<const ManagedVolumeAuthority> Open(Options);
  // Capture trusted existing capabilities only; caller must close published
  // guest generations and drop all old MVA owners before actual execution.
  std::unique_ptr<ManagedVolumeFormatHandoff> CaptureFormatHandoff() const;
  static std::unique_ptr<ManagedVolumeFormatHandoff> InspectFormatRecovery(
      Options);
  // Optional cancellation belongs to this synchronous caller and is never
  // retained. It is sampled at actual final Prepared admission after drain.
  static ManagedDeviceFormatReceipt ExecuteFormat(
      ManagedVolumeFormatHandoff&, const std::atomic<bool>* cancellation = nullptr);
  static std::shared_ptr<const ManagedVolumeAuthority> FromCompletedFormat(
      ManagedVolumeFormatHandoff&);
  static std::shared_ptr<const ManagedVolumeAuthority> RestoreUnchangedFormat(
      ManagedVolumeFormatHandoff&);
  static std::shared_ptr<const ManagedVolumeAuthority> ResumeFormat(
      ManagedVolumeFormatHandoff&);
  const auto& error() const { return error_; }
  const auto& content() const { return content_; }
  const auto& public_root() const { return public_root_; }
  const auto& identity() const { return identity_; }
  const auto& intents() const { return intents_; }
  const auto& recovery() const { return recovery_; }
  const auto& mutations() const { return mutations_; }
  bool profile_writable() const;
  bool format_eligible() const;

 private:
  friend class ManagedVolumeAuthorityTestAccess;
  ManagedVolumeAuthority() = default;
  static std::unique_ptr<ManagedDeviceIdentity::Handoff> PrepareFormatHandoff(
      ManagedVolumeFormatHandoff&, const std::atomic<bool>* external);
  static std::array<uint8_t, 32> FormatBindingDigest(
      const ManagedVolumeAuthority&, std::error_code&);
  static std::array<uint8_t, 32> FormatTicketDigest(
      const ManagedVolumeFormatHandoff&, std::error_code&);
  static std::shared_ptr<ManagedVolumeAuthority> BuildFormatVolume(
      ManagedVolumeFormatHandoff&, bool completed);
  static std::shared_ptr<const ManagedVolumeAuthority> OpenInternal(Options,
                                                                   bool inspect);
  // Declared first, destroyed last: retain initialization exclusion through
  // partial/error owner teardown and transfer the same lease into intents.
  std::shared_ptr<const HostStorageIntentLease> source_lease_;
  std::shared_ptr<HostStorageRoot> content_, private_parent_, public_root_,
      public_authority_, state_parent_, binding_root_;
  std::shared_ptr<const ManagedDeviceIdentity> identity_;
  std::shared_ptr<const ProfileIntentOwner> intents_;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery_;
  std::shared_ptr<ContentMutationOwner> mutations_;
  std::shared_ptr<const HostStorageGuestProjection> projection_;
  std::error_code error_;
};
// Opaque owned handoff, not an MVA/ProfileIntentOwner retainer or a caller
// readiness flag. The optional new partial owner exists only after actual
// format effects and retains uncertainty until explicit recovery.
class ManagedVolumeFormatHandoff final {
 public:
  ~ManagedVolumeFormatHandoff() = default;
  ManagedVolumeFormatHandoff(const ManagedVolumeFormatHandoff&) = delete;
  ManagedVolumeFormatHandoff& operator=(const ManagedVolumeFormatHandoff&) = delete;
  const auto& error() const { return error_; }
  const auto& source_id() const { return source_id_; }
  // Retiring/original identity pin only, never the private new-owner creator.
  const auto& captured_identity() const { return identity_; }
  uint64_t phase() const { return operation_ ? operation_->phase : 0; }
  void Cancel() { canceled_.store(true, std::memory_order_release); }
  // Pure logical closure only, before any owner destruction/IO. Registry's
  // separate Content guest view must be closed under its own transition.
  void ClosePublishedPublicView() {
    if (old_public_view_) { old_public_view_->Retire(); }
  }

 private:
  friend class ManagedVolumeAuthority;
  friend class ManagedVolumeAuthorityTestAccess;
  ManagedVolumeFormatHandoff() = default;
  std::shared_ptr<HostStorageRoot> content_, private_parent_, public_authority_,
      state_parent_, binding_root_, old_public_view_;
  std::shared_ptr<const ManagedDeviceIdentity> identity_;
  std::string source_id_;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery_;
  std::shared_ptr<const HostStorageGuestProjection> projection_;
  std::weak_ptr<const ProfileIntentOwner> previous_intents_;
  std::weak_ptr<ContentMutationOwner> previous_mutations_;
  std::unique_ptr<ManagedDeviceIdentity::Handoff> operation_;
  std::shared_ptr<const ManagedVolumeAuthority> partial_;
  std::array<uint8_t, 32> binding_{};
  std::atomic<bool> canceled_{false};
  std::error_code error_;
};
}  // namespace xe::filesystem
#endif
