/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#ifndef XENIA_BASE_MANAGED_DEVICE_IDENTITY_H_
#define XENIA_BASE_MANAGED_DEVICE_IDENTITY_H_
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include "xenia/base/host_storage.h"
namespace xe::filesystem {
struct ManagedDeviceFormatReceipt;
class ManagedDeviceIdentityTestAccess;
// Opaque source-owned persistent identity. UUID and epoch are restart identity;
// st_dev, mount IDs, owned root and registry generation remain live admission
// pins. No factory creates a directory, adopts unknown metadata, or rebinds a
// copied marker to a different configured location/directory file-ID.
class ManagedDeviceIdentity final {
 public:
  using Uuid = std::array<uint8_t, 16>;
  static constexpr auto kMarker = ".xenia-managed-source-v1";
  // Open an existing actual writable source before registry/guest admission.
  // A bounded pending-format record may return its retained owner with
  // operation_in_progress solely for explicit Format recovery; CheckCurrent
  // and normal registry/profile admission must reject it. Other invalid/foreign
  // or corrupt records are preserved, never replaced with a new identity.
  static std::shared_ptr<const ManagedDeviceIdentity> Open(
      std::shared_ptr<const HostStorageRoot> exact_managed_source,
      std::string configured_source_id, uint32_t configured_device_id,
      std::error_code&);
  static std::shared_ptr<const ManagedDeviceIdentity> OpenExisting(
      std::shared_ptr<const HostStorageRoot>, std::string, uint32_t,
      std::error_code&);
  // False with clear error means absent. No identity or guest permission is
  // returned; the same native marker decoder/lease/currentness checks apply.
  static bool CheckExisting(std::shared_ptr<const HostStorageRoot>, std::string,
                            uint32_t, std::error_code&);
  ~ManagedDeviceIdentity();
  const std::shared_ptr<const HostStorageRoot>& source() const {
    return source_;
  }
  const Uuid& uuid() const { return uuid_; }
  uint64_t content_epoch() const { return content_epoch_; }
  const std::string& source_id() const { return source_id_; }
  uint32_t device_id() const { return device_id_; }
  uint64_t directory_file_id() const { return directory_file_id_; }
  const std::filesystem::path& configured_location() const { return location_; }
  bool pending_format() const { return pending_format_; }
  // Cached cancellation only, safe under final model/GCRC guards. It does not
  // perform path/filesystem observation or claim independent live currentness.
  bool retiring() const { return retired_.load(std::memory_order_acquire); }
  // Logical removal cancels new/final admission without waiting for I/O or
  // releasing a marker lease still retained by an admitted operation.
  void Retire() const { retired_.store(true, std::memory_order_release); }
  const std::atomic<bool>* cancellation() const { return &retired_; }
  // Observing/blocking validation; invoke outside GCRC/model/registry locks.
  std::error_code CheckCurrent() const;
  // Explicit user-authorized managed format only. All profile/source owners and
  // actual common file leases must first drain. The marker lease itself is
  // permitted, preserved, and never makes its own format permanently busy.
  // Durable pending epoch precedes every user-data deletion; incomplete format
  // blocks ordinary Open/CheckCurrent until explicit recovery completes it.
  // Old proof cancellation is published before the first irreversible change.
  static ManagedDeviceFormatReceipt Format(
      std::shared_ptr<const ManagedDeviceIdentity> exact_owner);

 private:
  friend class ManagedDeviceIdentityTestAccess;
  friend class ManagedVolumeAuthorityTestAccess;
  friend class ManagedVolumeAuthority;
  friend class ManagedVolumeFormatHandoff;
  // Private producer capability. Only the managed-volume factory may validate
  // binding/root provenance and enter this coupled on-disk handoff.
  struct Handoff {
    std::shared_ptr<const ManagedDeviceIdentity> identity;
    std::shared_ptr<const HostStorageIntentLease> source_lease;
    std::shared_ptr<const HostStorageRoot> recovery_source;
    std::shared_ptr<const HostStorageGuestProjection> projection;
    Uuid nonce{};
    std::array<uint8_t, 32> binding{};
    uint64_t old_epoch = 0, phase = 0;
    bool prepared_durable = false, creator_ready = false;
    std::error_code error;
  };
  static std::unique_ptr<Handoff> PrepareHandoff(
      std::shared_ptr<const ManagedDeviceIdentity>,
      std::array<uint8_t, 32> binding,
      const std::atomic<bool>* cancellation = nullptr,
      const std::atomic<bool>* external_cancellation = nullptr);
  static std::unique_ptr<Handoff> InspectHandoff(
      std::shared_ptr<const ManagedDeviceIdentity>,
      std::array<uint8_t, 32> binding);
  static std::error_code ReopenHandoffForResume(Handoff&);
  static ManagedDeviceFormatReceipt ExecuteHandoff(Handoff&);
  static std::error_code AdvanceHandoff(Handoff&, bool committed);
  static std::shared_ptr<const ManagedDeviceIdentity> OpenInternal(
      std::shared_ptr<const HostStorageRoot>, std::string, uint32_t,
      std::error_code&, bool create, bool admit_guest);
  static ManagedDeviceFormatReceipt FormatInternal(
      std::shared_ptr<const ManagedDeviceIdentity>, std::error_code);
  struct State;
  ManagedDeviceIdentity(std::shared_ptr<const HostStorageRoot>, std::string,
                        uint32_t, Uuid, uint64_t, uint64_t,
                        std::filesystem::path, bool, std::shared_ptr<State>);
  const std::shared_ptr<const HostStorageRoot> source_;
  const std::string source_id_;
  const uint32_t device_id_;
  const Uuid uuid_;
  const uint64_t content_epoch_, directory_file_id_;
  const std::filesystem::path location_;
  const bool pending_format_;
  const std::shared_ptr<State> state_;
  mutable std::atomic<bool> retired_{false};
};
struct ManagedDeviceFormatReceipt {
  std::error_code error;
  // Published means this call actually appended the pending record. Recovery
  // instead reports observed=true; error never erases known actual effects.
  bool epoch_published = false, epoch_observed = false, epoch_durable = false;
  bool data_started = false, complete = false;
  uint64_t content_epoch = 0, removed_entries = 0;
  // Present only after actual durable completion/reopen. Never caller-filled.
  std::shared_ptr<const ManagedDeviceIdentity> current;
};
}  // namespace xe::filesystem
#endif
