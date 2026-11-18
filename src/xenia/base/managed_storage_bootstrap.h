/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_MANAGED_STORAGE_BOOTSTRAP_H_
#define XENIA_BASE_MANAGED_STORAGE_BOOTSTRAP_H_

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "xenia/base/managed_volume_authority.h"

namespace xe::filesystem {
class ContentMutationOwner;
class ProfileIntentOwner;
class HostStorageBootstrapDirectory;
class CacheStorageOwner;

// Immutable source-marker provenance. It is appended as a checked typed
// frame, never by editing existing source bytes/UUID/epoch. A missing binding
// is an error for this source, including after a process restart.
struct ManagedBootstrapReference {
  uint64_t epoch = 0, binding_directory = 0;
  std::array<uint8_t, 16> nonce{};
  std::array<uint8_t, 32> pending_digest{}, prefix_digest{};
  bool operator==(const ManagedBootstrapReference&) const = default;
};

struct ManagedBootstrapSourceDescription {
  std::array<uint8_t, 16> uuid{};
  uint64_t epoch = 0, directory = 0, marker_file = 0;
  uint32_t device = 0;
  std::string source, location;
  std::array<uint8_t, 32> last_hash{}, prefix_digest{};
  std::optional<ManagedBootstrapReference> binding;
  bool format_pending = false;
};

class ManagedStorageBootstrap;

// The owner factories cannot manufacture this from a bool, source ID or
// guest record. It is consumed once by the actual MVA initializer or owned
// format producer, bound to the same retained source lease and parent.
class ManagedBootstrapCreationPermit final {
 private:
  friend std::error_code ManagedDeviceIdentity::CheckBootstrapCurrent(
      const ManagedBootstrapCreationPermit&) const;
  friend class ManagedStorageBootstrap;
  friend class ManagedVolumeAuthority;
  friend class ProfileIntentOwner;
  friend class ContentMutationOwner;
  enum class Purpose { kProfile, kContentMutation, kFormatProfile };
  ManagedBootstrapCreationPermit(Purpose,
      std::shared_ptr<const ManagedDeviceIdentity>,
      std::shared_ptr<const HostStorageRoot>,
      std::shared_ptr<const HostStorageIntentLease>,
      std::weak_ptr<ManagedStorageBootstrap> = {},
      std::shared_ptr<HostStorageRoot> component = {});
  std::error_code Consume(Purpose, const ManagedDeviceIdentity&,
      const HostStorageRoot&, const HostStorageIntentLease&);
  std::error_code Check(const ManagedDeviceIdentity&) const;
  std::shared_ptr<HostStorageRoot> ComponentRoot(
      const std::shared_ptr<HostStorageRoot>& named, std::error_code&) const;
  const Purpose purpose_;
  const std::shared_ptr<const ManagedDeviceIdentity> identity_;
  const std::shared_ptr<const HostStorageRoot> parent_;
  const std::shared_ptr<const HostStorageIntentLease> source_lease_;
  const std::weak_ptr<ManagedStorageBootstrap> bootstrap_;
  const std::shared_ptr<HostStorageRoot> component_;
  std::atomic<bool> consumed_{false};
};

// One-use release of the exact metadata-only bootstrap projection. Native
// GuestView cannot infer readiness from a path, bool or committed source tag:
// ordinary journal recovery must have succeeded on the same retained owners.
class ManagedBootstrapGuestAdmissionPermit final {
 private:
  friend class ManagedStorageBootstrap;
  friend class CacheStorageOwner;
  friend class HostStorageRoot;
  ManagedBootstrapGuestAdmissionPermit(std::weak_ptr<ManagedStorageBootstrap>,
      std::shared_ptr<const HostStorageRoot>,
      std::shared_ptr<const HostStorageIntentLease>,
      std::shared_ptr<const ManagedDeviceIdentity>,
      std::shared_ptr<const HostStorageGuestProjection>,
      std::shared_ptr<const ContentMutationOwner>);
  ManagedBootstrapGuestAdmissionPermit(std::shared_ptr<CacheStorageOwner>,
      std::shared_ptr<const HostStorageRoot>,
      std::shared_ptr<const HostStorageIntentLease>,
      std::shared_ptr<const ManagedDeviceIdentity>,
      std::shared_ptr<const HostStorageGuestProjection>);
  bool FinalCurrent() const noexcept;
  std::error_code Consume(const HostStorageRoot&,const HostStorageGuestProjection&);
  const std::weak_ptr<ManagedStorageBootstrap> bootstrap_;
  const std::shared_ptr<const HostStorageRoot> source_;
  const std::shared_ptr<const HostStorageIntentLease> lease_;
  const std::shared_ptr<const ManagedDeviceIdentity> identity_;
  const std::shared_ptr<const HostStorageGuestProjection> projection_;
  const std::shared_ptr<const ContentMutationOwner> mutation_;
  const std::shared_ptr<CacheStorageOwner> cache_;
  std::atomic<bool> consumed_{false};
};

// Synchronous one-use admission after the real owned format has durably
// committed and its exact new-epoch owners have actually recovered. The
// lexical Handoff stays owned by FromCompletedFormat throughout Consume.
class ManagedFormatGuestAdmissionPermit final {
 private:
  friend class ManagedVolumeAuthority;
  friend class CacheStorageOwner;
  friend class HostStorageRoot;
  ManagedFormatGuestAdmissionPermit(const ManagedDeviceIdentity::Handoff&,
      std::shared_ptr<const ContentMutationOwner>);
  ManagedFormatGuestAdmissionPermit(const ManagedDeviceIdentity::Handoff&,
      std::shared_ptr<CacheStorageOwner>);
  std::error_code Consume(const HostStorageRoot&, const HostStorageGuestProjection&);
  bool FinalCurrent() const;  // cached/atomic only, no IO or owner locks
  const ManagedDeviceIdentity::Handoff& handoff_;
  const std::shared_ptr<const ManagedDeviceIdentity> identity_;
  const std::shared_ptr<const HostStorageRoot> source_, observation_source_;
  const std::shared_ptr<const HostStorageIntentLease> lease_;
  const std::shared_ptr<const HostStorageGuestProjection> projection_, observation_projection_;
  const std::shared_ptr<const ContentMutationOwner> mutation_;
  const std::shared_ptr<CacheStorageOwner> cache_;
  std::atomic<bool> consumed_{false};
};

// Initialization state belongs to the existing MVA binding/managed source.
// This object is not a content journal or a second device registry. It owns
// actual unpublished/error native capsules until typed admission succeeds.
class ManagedStorageBootstrap final : public std::enable_shared_from_this<ManagedStorageBootstrap> {
 public:
  ~ManagedStorageBootstrap();
 private:
  friend class ManagedVolumeAuthority;
  friend class ManagedDeviceIdentity;
  friend class ManagedBootstrapCreationPermit;
  friend class ManagedBootstrapGuestAdmissionPermit;
  friend class ProfileIntentOwner;
  friend class ContentMutationOwner;
  friend class ManagedVolumeAuthorityTestAccess;
  friend class ManagedDeviceIdentityTestAccess;
  struct Impl;
  explicit ManagedStorageBootstrap(std::unique_ptr<Impl>);
  static unsigned Boundaries(const ManagedVolumeAuthority&);
  static bool Open(ManagedVolumeAuthority&,
      const ManagedVolumeAuthority::Options&,
      const std::shared_ptr<HostStorageRoot>& content_anchor, bool inspect, unsigned stop = 0);
  // Before a normal factory creates/admits anything, a matching pending
  // binding blocks it even when the old legacy source has not yet been tagged.
  static std::error_code BeforeNormalOpen(const HostStorageRoot&,
      std::string_view source, uint32_t device);
  // Complete metadata decode precedes this gate, and this gate precedes
  // GuestView. A required reference can never fall back to unbound legacy.
  static std::error_code CheckNormal(const HostStorageRoot&,
      const ManagedBootstrapSourceDescription&);
  static std::error_code ValidateBindingContainer(
      const std::shared_ptr<HostStorageRoot>&);
  static std::error_code ValidateOwnerParent(
      const ManagedDeviceIdentity&, const HostStorageRoot&);
  std::error_code CheckGuestAdmission(const ManagedBootstrapGuestAdmissionPermit&) const;
  static std::error_code CheckComponentCurrent(const HostStorageRoot&);
  static std::error_code CheckPermitBindings(const ManagedDeviceIdentity&,
      const HostStorageRoot& expected_parent, const HostStorageRoot& actual_parent,
      const HostStorageIntentLease&);
  bool AllowsPending(const HostStorageRoot&,
      const ManagedBootstrapSourceDescription&) const;
  std::error_code CheckPending(const ManagedDeviceIdentity&) const;
  std::unique_ptr<ManagedBootstrapCreationPermit> Permit(
      ManagedBootstrapCreationPermit::Purpose,
      const std::shared_ptr<const HostStorageRoot>&);
  std::unique_ptr<Impl> impl_;
};

}  // namespace xe::filesystem
#endif
