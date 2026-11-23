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
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/base/cache_storage_owner.h"

namespace xe::filesystem {
class HostStorageRoot;
class MediaSelection;
struct HostStorageSnapshot;
class ManagedDeviceIdentity;
class ManagedVolumeAuthority;
class ManagedVolumeFormatHandoff;
}  // namespace xe::filesystem
namespace xe::vfs { class VirtualFileSystem; }
namespace xe::kernel {
class NativeContentStorageSource;
class NativeMediaService;
}

namespace xe::kernel::xam {
class ContentManager;
class ManagedFormatSourceProof;
class SelectedMediaSourceProof;
namespace test { class SelectedMediaTimingAccess; class SelectedMediaNativeAdmissionAccess; }

// A source is a physical location, not a title/profile identity. More than one
// source can contribute to a guest device: installed system content is a
// read-only source on the HDD, never a second writable copy of those packages.
enum class ContentSourceKind { kManaged, kInstalledSystem, kHostMedia, kSelectedMedia, kCache };

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
  // Opaque ordinary-media authority. File selections expose no native parent.
  std::shared_ptr<filesystem::MediaSelection> selected_media;
  bool profile_writable = false;
  bool format_eligible = false;
  std::shared_ptr<const ManagedFormatSourceProof> managed_format_proof;
  std::shared_ptr<const filesystem::ConsoleStorageVolume> storage_volume;
  // A refused fresh selected-media observation grants no backing. It does not
  // overwrite the last physical projection or retire that actual generation.
  std::error_code observation_error;
};

// Immutable cached namespace/selector facts, with NO HostStorageRoot, MVA,
// selection or Device ownership. This is not file/read/write/format admission.
// Exact metadata identity is still required at each actual common IO boundary.
struct ContentSourceDescription {
  ContentSource source;
  uint64_t generation=0,capacity_bytes=0,free_bytes=0;
  std::shared_ptr<const filesystem::ConsoleStorageVolume> identity;
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

// Identity of one actual registry publication, including metadata-only intent.
// It owns no registry, selection, Device or native resource and grants no read
// authority. Numeric generation reuse cannot manufacture this identity.
class SelectedMediaPublication final {
 public:
  ~SelectedMediaPublication() = default;
  SelectedMediaPublication(const SelectedMediaPublication&) = delete;
  SelectedMediaPublication& operator=(const SelectedMediaPublication&) = delete;

 private:
  friend class ContentSourceRegistry;
  SelectedMediaPublication() = default;
};

class SelectedMediaUse final {
 public:
  ~SelectedMediaUse();
  SelectedMediaUse(const SelectedMediaUse&)=delete;
  SelectedMediaUse& operator=(const SelectedMediaUse&)=delete;
  // Pure atomic cached validation; no registry/native ownership acquisition.
  // False closes ingress, not permission to skip native
  // Close/rundown. Destruction drops only an atomic generation use count.
  bool current() const noexcept;
 private:
  friend class ContentSourceRegistry;
  struct Impl;
  explicit SelectedMediaUse(std::unique_ptr<Impl>);
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
  // Explicit host selection only. No package enumeration/path resolution or
  // managed/profile/format authority is granted by this registration.
  bool RegisterSelectedMedia(
      ContentSource, std::shared_ptr<filesystem::MediaSelection>,
      uint64_t generation, vfs::VirtualFileSystem&,
      std::shared_ptr<const SelectedMediaPublication>* publication = nullptr);
  // A requested receipt is issued by THIS actual transaction before callbacks,
  // not by a later Source lookup. Existing idempotent publications are refused
  // when a fresh owner receipt is requested.
  // Saved intent with genuinely unavailable input. Publishes metadata only;
  // never opens a path, creates a Device or claims a native attachment.
  bool RegisterUnavailableSelectedMedia(
      ContentSource,
      std::shared_ptr<const SelectedMediaPublication>* publication = nullptr);
  // Cached metadata check against the exact sampled source and publication.
  // A foreign newer generation or same-number ABA publication always refuses.
  bool OwnsSelectedMedia(
      const ContentSourceState&,
      const std::shared_ptr<const SelectedMediaPublication>&) const;
  // Cached post-notification availability of THIS issued publication. No IO,
  // source re-observation, ownership substitution or native attachment proof.
  bool SelectedMediaAvailable(
      const std::string& id,
      const std::shared_ptr<const SelectedMediaPublication>&) const;
  bool WithdrawSelectedMedia(
      const std::string& id, uint64_t expected_generation,
      const std::shared_ptr<const SelectedMediaPublication>& expected = {});
  // Host reference removal, after exact withdrawal. Existing cursor/file uses
  // must really drain before its exact device and source record are forgotten.
  bool ForgetSelectedMedia(
      const std::string& id, uint64_t expected_generation,
      vfs::VirtualFileSystem&,
      const std::shared_ptr<const SelectedMediaPublication>& expected = {});
  // Non-content physical cache role: excluded from selectors, package paths,
  // profiles and user HDD format. Only the exact typed product owner can bind
  // its current role root and namespace through the existing registry/VFS.
  bool RegisterCache(ContentSource,std::shared_ptr<filesystem::CacheStorageOwner>,
      filesystem::CacheStorageOwner::Role,uint64_t generation,vfs::VirtualFileSystem&);
  // Cancels the exact observed generation before closing its actual VFS graph.
  // Busy real Files remain bound to that retired device for truthful retry.
  bool WithdrawCache(const std::string& id,uint64_t expected_generation,vfs::VirtualFileSystem&);

  std::shared_ptr<filesystem::MediaSelection> CaptureSelectedMedia(
      const ContentSourceState&) const;
  std::shared_ptr<SelectedMediaUse> CaptureSelectedMediaUse(
      const ContentSourceState&) const;
  // Pending controlled retirement is finalized by the existing source refresh
  // producer after the last exact use drains. No owner callback in a destructor.

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
  // Exact captured cached generation only; no observation or numeric-ID lookup.
  std::shared_ptr<const filesystem::ConsoleStorageVolume> CaptureStorageVolume(
      const ContentSourceState&) const;
  // Bounded namespace publication only, after GCRC and any driver binding
  // admission. The caller retains this Registry/manager for the whole scope.
  // No observing IO, callbacks, guest execution or last-owner release while
  // held. This joins the exact existing registry generation to an effect;
  // cached volume availability alone is not such a lease.
  class SourceGenerationAdmission final {
   public:
    ~SourceGenerationAdmission()=default;
    SourceGenerationAdmission(SourceGenerationAdmission&&) noexcept=default;
    SourceGenerationAdmission& operator=(SourceGenerationAdmission&&) noexcept=default;
   private:
    friend class ContentSourceRegistry;
    explicit SourceGenerationAdmission(std::mutex& mutex)
        :lock_(mutex,std::try_to_lock){}
    std::unique_lock<std::mutex> lock_;
  };
  std::optional<SourceGenerationAdmission> TryAdmitSourceGeneration(
      const ContentSourceState&,std::error_code&) const noexcept;
  // The metadata form joins guest-ID/namespace publication without retaining
  // an MVA or native backing. It grants no read/write/format capability.
  std::optional<SourceGenerationAdmission> TryAdmitSourceGeneration(
      const ContentSourceDescription&,std::error_code&) const noexcept;
  std::optional<SourceGenerationAdmission> TryAdmitSourceGenerations(
      std::span<const ContentSourceDescription>,std::error_code&) const noexcept;
  // Exact already-observed generation only, with nonwaiting metadata
  // admission. No path observation/refresh, weak native owner, callback or IO.
  // Caller must use ordinary retained-root admission for actual operations.
  std::optional<ContentSourceState> CaptureSourceCached(
      const std::shared_ptr<const filesystem::ConsoleStorageVolume>&,
      std::error_code&) const;
  // Same exact cached validation as CaptureSourceCached, returning metadata
  // only. Suitable for an ephemeral native-ID translation or visible selector:
  // its lifetime can never retain a source flock or guest-access backing.
  std::optional<ContentSourceDescription> DescribeSourceCached(
      const std::shared_ptr<const filesystem::ConsoleStorageVolume>&,
      std::error_code&) const;
  // Cached selector/ID projection only. No Sources() observation, source/root
  // owner, callback or native resource may escape these metadata methods.
  std::vector<ContentSourceDescription> DescribeSourcesCached(
      uint32_t device_id, std::error_code&) const;
  bool IsDescriptionCurrentCached(const ContentSourceDescription&) const noexcept;

  std::shared_ptr<filesystem::HostStorageRoot> CaptureManagedDirectory(
      const std::filesystem::path&, bool force_readonly, bool& matched,
      std::error_code&,
      std::shared_ptr<const filesystem::ConsoleStorageVolume>* volume = nullptr) const;

  // Exact managed OR registered readonly source directory. Source policy and
  // root are captured together; unknown writable/catalog roots gain no access.
  std::shared_ptr<filesystem::HostStorageRoot> CaptureSourceDirectory(
      const std::filesystem::path&,bool force_readonly,bool& matched,std::error_code&,
      std::shared_ptr<const filesystem::ConsoleStorageVolume>* volume = nullptr) const;

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
  friend class xe::kernel::NativeMediaService;
  // Nonwaiting admission only for the exact issued selected proof. No source
  // refresh, filesystem observation or file/root capability is returned.
  // Actual native query initialization/open must still succeed truthfully.
  std::shared_ptr<SelectedMediaUse> CaptureSelectedMediaUse(
      const std::shared_ptr<const SelectedMediaSourceProof>&,std::error_code&) const;
  friend class test::SelectedMediaTimingAccess;
  friend class test::SelectedMediaNativeAdmissionAccess;
  // Timing only, outside all locks, after actual capture/preparation. No probe
  // result, file identity, source or authority can be substituted by this seam.
  void SetSelectedMediaTimingForTest(std::function<void(const std::string&)> sampled,
      std::function<void(const std::string&)> prepared);
  static filesystem::HostStorageSnapshot ProbeSelected(
      const std::shared_ptr<filesystem::MediaSelection>&,bool draining);
  std::shared_ptr<filesystem::HostStorageRoot> CaptureDirectory(
      const std::filesystem::path&,bool,bool&,std::error_code&,bool managed_only,
      std::shared_ptr<const filesystem::ConsoleStorageVolume>*) const;
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
