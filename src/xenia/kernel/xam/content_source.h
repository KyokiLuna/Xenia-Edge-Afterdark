/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#ifndef XENIA_KERNEL_XAM_CONTENT_SOURCE_H_
#define XENIA_KERNEL_XAM_CONTENT_SOURCE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>
#include "xenia/kernel/xam/xam_content_device.h"

namespace xe::filesystem {
class HostStorageRoot;
class ManagedDeviceIdentity;
class ManagedVolumeAuthority;
class ManagedVolumeFormatHandoff;
}  // namespace xe::filesystem
namespace xe::kernel {
class NativeContentStorageSource;
}

namespace xe::kernel::xam {
class ContentManager;
class ManagedFormatSourceProof;

// A source is a physical location, not a title/profile identity. More than one
// source can contribute to a guest device: installed system content is a
// read-only source on the HDD, never a second writable copy of those packages.
enum class ContentSourceKind { kManaged, kInstalledSystem, kHostMedia };

struct ContentSource {
  std::string id;
  uint32_t device_id = 0;
  DeviceType device_type = DeviceType::Invalid;
  ContentSourceKind kind = ContentSourceKind::kManaged;
  std::filesystem::path root;
  std::string guest_root;
  std::u16string name;
  bool read_only = true;
  uint64_t reserved_bytes = 0;
  // The device/partition root is an explicit mount, distinct from Content.
  // It must never be inferred by stripping a host or guest path component.
  std::string guest_volume_root = {};
  // Explicit producer-selected public shell, never inferred Parent(Content).
  std::filesystem::path host_volume_root = {};
  // Optional explicit existing same-FS parent for a typed private container.
  std::filesystem::path private_state_parent = {};
};

struct ContentSourceState {
  ContentSource source;
  bool available = false;
  uint64_t capacity_bytes = 0;
  // Unprivileged usable space, with this source's reserve already removed.
  uint64_t free_bytes = 0;
  // Owned root generation; no consumer borrows the registry or another mount.
  // A removed/replaced root retires this generation before a new one is opened.
  std::shared_ptr<filesystem::HostStorageRoot> storage;
  uint64_t generation = 0;
  std::shared_ptr<const filesystem::ManagedDeviceIdentity> managed_identity;
  std::shared_ptr<const filesystem::ManagedVolumeAuthority> volume;
  // Separate operations: actual filewrites do not imply paired-GPD/format.
  bool profile_writable = false;
  bool format_eligible = false;
  std::shared_ptr<const ManagedFormatSourceProof> managed_format_proof;
};

// Opaque cached registry capability for one actual managed generation. It is
// not a native device-ID binding or permission to substitute another source.
// Holding it never retains an MVA/intent source flock or the registry State.
class ManagedFormatSourceProof final {
 public:
  ~ManagedFormatSourceProof();
  ManagedFormatSourceProof(const ManagedFormatSourceProof&) = delete;
  ManagedFormatSourceProof& operator=(const ManagedFormatSourceProof&) = delete;
  const std::string& source_id() const;
  uint64_t generation() const;
  // Cached nonblocking validation only; false includes busy/closed/retired.
  // Invoke outside driver/GCRC/model/metadata locks: a transient weak State
  // rundown reference is released here. No filesystem observation/callbacks.
  bool current() const noexcept;
 private:
  friend class ContentSourceRegistry;
  friend class ContentManager;
  struct Impl;
  explicit ManagedFormatSourceProof(std::unique_ptr<Impl>);
  bool MatchesIdentity(
      const std::shared_ptr<const filesystem::ManagedDeviceIdentity>&) const;
  bool MatchesVolume(
      const std::shared_ptr<const filesystem::ManagedVolumeAuthority>&) const;
  std::unique_ptr<Impl> impl_;
};

class ContentSourceRegistry {
 public:
  using SpaceQuery = std::function<std::optional<std::filesystem::space_info>(
      const std::filesystem::path&)>;

  enum class Access { kNormal, kReadOnlyCatalog };
  explicit ContentSourceRegistry(SpaceQuery query = {});
  explicit ContentSourceRegistry(Access access, SpaceQuery query = {});
  bool read_only_catalog() const;

  ~ContentSourceRegistry();

  ContentSourceRegistry(const ContentSourceRegistry&) = delete;
  ContentSourceRegistry& operator=(const ContentSourceRegistry&) = delete;

  // Registration neither creates nor modifies the host directory. Existing
  // source IDs may only be refreshed with the same identity and access policy.
  bool Register(ContentSource source);
  // Producer has already created/validated its marker outside registry locks.
  // Registration itself still never creates a directory or identity.
  bool RegisterManaged(
      ContentSource, std::shared_ptr<const filesystem::ManagedDeviceIdentity>,
      std::shared_ptr<const filesystem::ManagedVolumeAuthority> = {});
  // Fixed installed-image authority. Null means an observed missing source,
  // never permission to reopen it by path. Only a newer host source generation
  // may replace it; existing files retain the retired older view.
  bool RegisterReadOnly(ContentSource source,
                        std::shared_ptr<filesystem::HostStorageRoot> authority,
                        uint64_t generation);
  class ManagedTransition final {
   public:
    ~ManagedTransition();
    ManagedTransition(const ManagedTransition&) = delete;
    ManagedTransition& operator=(const ManagedTransition&) = delete;
    // Call only after profile/namespace/manager locks and old owners have been
    // released. This dispatches the actual unavailable snapshot to observers.
    void PublishWithdrawal();
    std::error_code Publish(
        std::shared_ptr<const filesystem::ManagedVolumeAuthority>);
   private:
    friend class ContentSourceRegistry;
    struct Impl;
    explicit ManagedTransition(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
  };
  // Pure admission closure under short locks; no callbacks or observing IO.
  // Detached MVA refs are returned to be destroyed AFTER all caller locks.
  // The token retains identity/capabilities, never an old MVA or intent owner.
  std::unique_ptr<ManagedTransition> BeginManagedTransition(
      const std::string& id, uint64_t expected_generation,
      const std::shared_ptr<const filesystem::ManagedVolumeAuthority>& expected,
      std::vector<std::shared_ptr<const filesystem::ManagedVolumeAuthority>>&
          detached_owners,
      std::error_code&,
      std::shared_ptr<const ManagedFormatSourceProof> expected_proof = {});

  // Explicit startup recovery only: no guest generation was ever published.
  // Requires the actual opaque inspected handoff, never a filename/ready flag.
  std::unique_ptr<ManagedTransition> BeginManagedRecovery(
      ContentSource, const filesystem::ManagedVolumeFormatHandoff&,
      std::error_code&);

  using ManagedRecoveryAdmission = std::function<std::unique_ptr<ManagedTransition>(
      const filesystem::ManagedVolumeFormatHandoff&, std::error_code&)>;
  ManagedRecoveryAdmission CaptureManagedRecoveryAdmission(ContentSource) const;

  std::vector<uint32_t> DeviceIds() const;
  std::vector<ContentSourceState> Sources(uint32_t device_id) const;
  std::optional<ContentSourceState> Source(const std::string& id) const;
  std::optional<ContentSourceState> WritableSource(uint32_t device_id) const;
  std::shared_ptr<filesystem::HostStorageRoot> CaptureManagedDirectory(
      const std::filesystem::path&, bool force_readonly, bool& matched,
      std::error_code&) const;

  // An owned observer of managed logical volumes. Its subscription and initial
  // snapshot are atomic; callbacks never borrow this registry or its locks.
  std::shared_ptr<NativeContentStorageSource> storage_notifications() const;

  // Resolve a case-insensitive relative guest path below this exact source.
  // Refuse ambiguous case-only aliases and symlinks
  // escaping the source, including an escape through an existing parent of a
  // new destination. No filesystem mutation is performed here.
  std::optional<std::filesystem::path> Resolve(
      const std::string& source_id, const std::filesystem::path& relative,
      bool for_write) const;
  // Resolve only against the exact captured root/generation. Neither a reused
  // string ID nor a caller-filled state can replace the registered capability.
  // Actual filesystem work occurs outside the registry mutex.
  std::optional<std::filesystem::path> Resolve(
      const ContentSourceState& expected, const std::filesystem::path& relative,
      bool for_write) const;

 private:
  bool RegisterImpl(ContentSource, std::shared_ptr<filesystem::HostStorageRoot>,
                    uint64_t, bool fixed,
      std::shared_ptr<const filesystem::ManagedDeviceIdentity> = {},
      std::shared_ptr<const filesystem::ManagedVolumeAuthority> = {});
  struct State;
  static std::shared_ptr<const ManagedFormatSourceProof> MintManagedFormatProof(
      const std::shared_ptr<State>&, const std::string&, uint64_t,
      std::weak_ptr<const filesystem::ManagedDeviceIdentity>,
      std::weak_ptr<const filesystem::ManagedVolumeAuthority>);
  class Notifications;
  static std::unique_ptr<ManagedTransition> BeginManagedRecoveryOwned(
      std::shared_ptr<State>, std::shared_ptr<Notifications>, ContentSource,
      const filesystem::ManagedVolumeFormatHandoff&, std::error_code&);
  std::shared_ptr<State> state_;
  std::shared_ptr<Notifications> notifications_;
};

}  // namespace xe::kernel::xam

#endif  // XENIA_KERNEL_XAM_CONTENT_SOURCE_H_
