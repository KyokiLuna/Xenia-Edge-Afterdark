/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_CONTENT_MANAGER_H_
#define XENIA_KERNEL_XAM_CONTENT_MANAGER_H_

#include <memory>
#include <atomic>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string_key.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/title_catalog.h"
#include "xenia/vfs/entry.h"
#include "xenia/xbox.h"
#include <map>
#include <mutex>
#include "xenia/base/profile_intent_owner.h"
#include "xenia/base/managed_volume_authority.h"

#include "xenia/base/content_package_tree.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
namespace vfs {
class XContentReadBacking;
class Device;
class XContentContainerDevice;
}  // namespace vfs
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

// If set in XCONTENT_AGGREGATE_DATA, will be substituted with the running
// titles ID
// TODO: check if actual x360 kernel/xam has a value similar to this
constexpr uint32_t kCurrentlyRunningTitleId = 0xFFFFFFFF;

// This bit of a content file name's first byte marks content shared under XUID
// 0, stored under the name without it.
constexpr uint8_t kSharedContentFileNameBit = 0x80;

struct XCONTENT_DATA {
  be<uint32_t> device_id;         // 0x0 sz:0x4
  be<XContentType> content_type;  // 0x4 sz:0x4
  union {
    // this should be be<uint16_t>, but that stops copy constructor being
    // generated...
    uint16_t uint[128];
    char16_t chars[128];
  } display_name_raw;  // 0x8 sz:0x100

  char file_name_raw[42];  // 0x108 sz:0x2A

  // Some games use this padding field as a null-terminator, as eg.
  // DLC packages usually fill the entire file_name_raw array
  // Not every game sets it to 0 though, so make sure any file_name_raw reads
  // only go up to 42 chars!
  uint8_t padding[2];  // 0x132 sz: 0x2

  bool operator==(const XCONTENT_DATA& other) const {
    // Package is located via device_id/content_type/file_name, so only need to
    // compare those
    return device_id == other.device_id && content_type == other.content_type &&
           file_name() == other.file_name();
  }

  std::u16string display_name() const {
    return load_and_swap<std::u16string>(display_name_raw.uint);
  }

  std::string file_name() const {
    std::string value;
    value.assign(file_name_raw, strnlen(file_name_raw, countof(file_name_raw)));
    return value;
  }

  void set_display_name(const std::u16string_view value) {
    // Some games (e.g. 584108A9) require multiple null-terminators for it to
    // read the string properly, blanking the array should take care of that

    std::fill_n(display_name_raw.chars, countof(display_name_raw.chars), 0);
    string_util::copy_and_swap_truncating(display_name_raw.chars, value,
                                          countof(display_name_raw.chars));
  }

  void set_file_name(const std::string_view value) {
    std::fill_n(file_name_raw, countof(file_name_raw), 0);
    string_util::copy_maybe_truncating<string_util::Safety::IKnowWhatIAmDoing>(
        file_name_raw, value, xe::countof(file_name_raw));

    // Some games rely on padding field acting as a null-terminator...
    padding[0] = padding[1] = 0;
  }
};
static_assert_size(XCONTENT_DATA, 0x134);

struct XCONTENT_AGGREGATE_DATA : XCONTENT_DATA {
  be<uint64_t> xuid;  // some titles store XUID here?
  be<uint32_t> title_id;

  XCONTENT_AGGREGATE_DATA() = default;
  XCONTENT_AGGREGATE_DATA(const XCONTENT_DATA& other) {
    device_id = other.device_id;
    content_type = other.content_type;
    set_display_name(other.display_name());
    set_file_name(other.file_name());
    padding[0] = padding[1] = 0;
    xuid = 0;
    title_id = kCurrentlyRunningTitleId;
  }

  bool operator==(const XCONTENT_AGGREGATE_DATA& other) const {
    // Package is located via device_id/title_id/content_type/file_name, so only
    // need to compare those
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type &&
           file_name() == other.file_name();
  }
};
static_assert_size(XCONTENT_AGGREGATE_DATA, 0x148);

struct XCONTENT_DATA_MEDIA {
  be<uint8_t> series_id[0x10];  // 0x0 sz:0x10
  be<uint8_t> seasonid[0x10];   // 0x10 sz:0x10
  be<uint16_t> season_number;   // 0x20 sz:0x2
  be<uint16_t> episode_number;  // 0x22 sz:0x2
};
static_assert_size(XCONTENT_DATA_MEDIA, 0x24);

struct XCONTENT_DATA_AVATAR_ASSET {
  be<uint32_t> subcategory;           // 0x0 sz:0x4
  be<int32_t> colorizable;            // 0x4 sz:0x4
  be<uint8_t> asset_id[0x10];         // 0x8 sz:0x10
  be<uint8_t> skeleton_version_mask;  // 0x18 sz:0x1
};
static_assert_size(XCONTENT_DATA_AVATAR_ASSET, 0x1C);

enum XCONTENT_INTERNAL_FLAGS : uint32_t {
  Partial = 0x00000001,
  Corrupt = 0x00000002,
  KinectEnabled = 0x00000004,
  DeepLinkSupported = 0x00000008,
  DisableNetworkStorage = 0x00000010,
  MoveOnlyTransfer = 0x00000020,
  DeviceTransfer = 0x00000040,
  ProfileTransfer = 0x00000080,
};

struct XCONTENT_DATA_INTERNAL : XCONTENT_DATA {  // 0x0 sz:0x134
  be<uint32_t> category;                         // 0x134 sz:0x4
  be<uint64_t> xuid;                             // 0x138 sz:0x8
  be<uint32_t> title_id;                         // 0x140 sz:0x4
  be<uint32_t> license_mask;                     // 0x144 sz:0x4

  // added in V4532 0x148
  be<uint64_t> content_size;  // 0x148 sz:0x8

  // added in NXE
  be<uint64_t> creation_time;  // 0x150 sz:0x8 FILETIME
  char16_t title_name[0x40];   // 0x158 sz:0x80

  // compiler does not like
  // union {
  // XCONTENT_DATA_AVATAR_ASSET avatar_content_data;  // 0x1D8 sz:0x1C
  // XCONTENT_DATA_MEDIA media_content_data;          // 0x1D8 sz:0x24
  //};
  be<uint8_t> padding[0x24];                  // 0x1D8 sz:0x1C
  be<XCONTENT_INTERNAL_FLAGS> xcontent_flag;  // 0x1FC sz:0x04

  XCONTENT_DATA_INTERNAL() = default;
  XCONTENT_DATA_INTERNAL(const XCONTENT_DATA& other) {
    device_id = other.device_id;
    content_type = other.content_type;
    set_display_name(other.display_name());
    set_file_name(other.file_name());
    padding[0] = padding[1] = 0;
    xuid = 0;
    title_id = kCurrentlyRunningTitleId;
  }

  bool operator==(const XCONTENT_DATA_INTERNAL& other) const {
    // Package is located via device_id/title_id/content_type/file_name, so only
    // need to compare those
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type &&
           file_name() == other.file_name();
  }
};
static_assert_size(XCONTENT_DATA_INTERNAL, 0x200);

// FILETIMEs and the size in bytes.
struct ContentAttributes {
  uint64_t creation_time;
  uint64_t last_access_time;
  uint64_t last_write_time;
  uint64_t size;
};

class ContentPackage {
 public:
  // root_path is a directory inside the package to mount at, empty for the
  // package root.
  ContentPackage(KernelState* kernel_state, const std::string_view root_name,
                 const XCONTENT_AGGREGATE_DATA& data,
                 const std::filesystem::path& package_path,
                 const std::string_view root_path = "", bool read_only = false,
                 std::shared_ptr<filesystem::HostStorageRoot> storage = {});
  // A package stored on another device, such as the disc.
  ContentPackage(KernelState* kernel_state, const std::string_view root_name,
                 const XCONTENT_AGGREGATE_DATA& data,
                 vfs::Entry* package_entry);
  ~ContentPackage();

  void LoadPackageLicenseMask(const std::filesystem::path header_path);

  const XCONTENT_AGGREGATE_DATA& GetPackageContentData() const {
    return content_data_;
  }

  const uint32_t GetPackageLicense() const { return license_; }

  bool is_mounted() const { return mounted_; }

  // Ends in a separator, so it prefixes only this package's entries.
  const std::string& device_path() const { return device_path_; }

  const std::filesystem::path& package_path() const { return package_path_; }

 private:
  ContentPackage(KernelState* kernel_state, const std::string_view root_name,
                 const XCONTENT_AGGREGATE_DATA& data,
                 const std::string_view root_path);

  friend class ContentManager;
  ContentPackage(KernelState*, std::string_view, const XCONTENT_AGGREGATE_DATA&,
      const std::filesystem::path&, std::string_view,
      std::unique_ptr<vfs::XContentReadBacking>);
  void MountHostPath(const std::filesystem::path& package_path, bool read_only,
                     std::shared_ptr<filesystem::HostStorageRoot> storage = {});
  void MountContainer(std::unique_ptr<vfs::XContentContainerDevice> device,
                      const std::string_view name);

  // Outlives storage release/unmount and owns only cached admission metadata.
  // Its state is independent of the manager wrapper (no raw-owner callback).
  std::shared_ptr<void> package_admission_;
  std::unique_ptr<vfs::XContentReadBacking> validation_backing_;
  KernelState* kernel_state_;
  std::string root_name_;
  std::string device_path_;
  std::filesystem::path package_path_;
  XCONTENT_AGGREGATE_DATA content_data_;
  uint32_t license_;
  bool mounted_ = false, link_owned_ = false;
  std::string root_path_, published_link_target_;
  std::weak_ptr<vfs::Device> registered_device_;
  void PublishMount(std::unique_ptr<vfs::Device>);
  std::optional<ContentSourceState> source_;
};

class ContentManager {
 public:
  bool RegisterManagedSource(ContentSource);
  bool RegisterRequiredManagedSource(ContentSource);
  bool managed_sources_ready() const;
  std::shared_ptr<const filesystem::ProfileIntentOwner> ProfileIntents(
      const std::string& source_id) const;
  std::shared_ptr<const filesystem::ManagedVolumeAuthority> Volume(
      const std::string& source_id) const;
  // Owned synchronous backend operation. No guest Task, overlap, scheduler or
  // native FE raw formatter is manufactured here. Dropped handles remain
  // recoverable from this manager's pending map until explicit completion.
  class ManagedFormat final {
   public:
    ~ManagedFormat();
    ManagedFormat(const ManagedFormat&) = delete;
    ManagedFormat& operator=(const ManagedFormat&) = delete;
    filesystem::ManagedDeviceFormatReceipt Execute();
    std::error_code Resume();
    void Cancel();
   private:
    friend class ContentManager;
    struct Impl;
    struct Owners;
    ManagedFormat(std::shared_ptr<Impl>, std::shared_ptr<Owners>);
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<Owners> owners_;
  };
  // Requires the runtime's actual ProfileManager to reserve an idle namespace.
  // A storage-only manager has no guest/profile runtime by construction.
  std::unique_ptr<ManagedFormat> BeginIdleManagedFormat(
      const std::string& source_id, std::error_code&);
  // Exact expected registry generation, retained through final admission.
  std::unique_ptr<ManagedFormat> BeginIdleManagedFormat(
      std::shared_ptr<const ManagedFormatSourceProof>, std::error_code&);
  std::unique_ptr<ManagedFormat> CapturePendingManagedFormat(
      const std::string& source_id, std::error_code&);
  // Explicit restart recovery of an already registered producer configuration;
  // does not create roots, recover silently or publish an unfinished owner.
  std::unique_ptr<ManagedFormat> InspectManagedFormatRecovery(
      const std::string& source_id, std::error_code&);
  struct ReadOnlyCatalogTag {};
  // Exact-ID physical discovery only. No managed marker, private owner or
  // native physical-volume publication is created by this mode.
  ContentManager(ReadOnlyCatalogTag, const std::filesystem::path& root_path);
  // Storage authority and optional guest execution context are independent.
  // A null/invalid storage context fails required managed-source admission.
  ContentManager(
      std::shared_ptr<const filesystem::ManagedStorageContext> storage_context,
      const std::filesystem::path& root_path,
      KernelState* guest_runtime = nullptr);
  ~ContentManager();

  ContentSourceRegistry& sources() { return sources_; }
  const ContentSourceRegistry& sources() const { return sources_; }

  std::vector<XCONTENT_AGGREGATE_DATA> ListContent(
      const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
      const XContentType content_type) const;

  // The users with content stored on the hard drive, the titles with content
  // stored on the device for a user and the content types a title has there,
  // for listing content across titles.
  std::vector<uint64_t> ListContentXuids(uint32_t device_id = 1) const;
  std::vector<uint32_t> ListContentTitleIds(const uint32_t device_id,
                                            const uint64_t xuid) const;
  std::vector<XContentType> ListContentTypes(const uint32_t device_id,
                                             const uint64_t xuid,
                                             const uint32_t title_id) const;
  // A record for each release in the game library, which the dashboard lists
  // as its games.
  std::vector<XCONTENT_AGGREGATE_DATA> ListLibraryContent() const;
  // The game library path of the release a ListLibraryContent record names.
  std::optional<std::filesystem::path> LibraryContentPath(
      const XCONTENT_AGGREGATE_DATA& data) const;
  // Matches exact physical variants already resolved by the UI snapshot.
  // Neither config state nor source package bytes are modified here.
  std::optional<TitleCatalogVariant>
  LibraryContentVariant(uint64_t xuid, const XCONTENT_AGGREGATE_DATA &data);
  bool LibraryContainsPhysicalContent(uint64_t xuid,
                                      const XCONTENT_AGGREGATE_DATA &data);
  X_RESULT ResolveGuestContentPath(uint64_t xuid,
                                    const XCONTENT_AGGREGATE_DATA& data,
                                    bool for_write, std::string& path);

  std::vector<XCONTENT_AGGREGATE_DATA> ListContentODD(
      const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
      const XContentType content_type) const;

  std::unique_ptr<ContentPackage> ResolvePackage(
      const std::string_view root_name, const uint64_t xuid,
      const XCONTENT_AGGREGATE_DATA& data, const uint32_t disc_number = -1);

  // What any request for the content fails with because of its file name.
  X_RESULT CheckPackageName(const uint64_t xuid,
                            const XCONTENT_AGGREGATE_DATA& data);
  bool ContentExists(const uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data);
  X_RESULT FlushContent(std::string_view root_name);
  X_RESULT WriteContentHeaderFile(const uint64_t xuid,
                                  XCONTENT_AGGREGATE_DATA data);
  X_RESULT ReadContentHeaderFile(const std::filesystem::path& package_path,
                                 const uint64_t xuid, const uint32_t title_id,
                                 XContentType content_type,
                                 XCONTENT_AGGREGATE_DATA& data) const;
  // Makes the package without mounting it.
  X_RESULT CreatePackage(const uint64_t xuid,
                         const XCONTENT_AGGREGATE_DATA& data);
  // Complete initial tree and Header share the same durable creation owner.
  // No member is exposed through an empty public package before completion.
  X_RESULT CreateInitialPackage(
      uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
      std::span<const filesystem::ContentPackageInitialMember> members);
  X_RESULT CreateContent(const std::string_view root_name, const uint64_t xuid,
                         const XCONTENT_AGGREGATE_DATA& data);
  // Empties the package but for its thumbnail.
  X_RESULT TruncateContent(const uint64_t xuid,
                           const XCONTENT_AGGREGATE_DATA& data);
  X_RESULT OpenContent(const std::string_view root_name, const uint64_t xuid,
                       const XCONTENT_AGGREGATE_DATA& data,
                       uint32_t& content_license,
                       const uint32_t disc_number = -1);
  // Mounts the package stored in a file under root_name, on the host or inside
  // another device such as the disc.
  X_RESULT OpenContentFile(const std::string_view root_name,
                           vfs::Entry* package_entry);
  X_RESULT CloseContent(const std::string_view root_name);
  // Closes the packages mounted from files on |device| before it goes away.
  void ClosePackagesMountedFrom(const vfs::Device* device);
  X_RESULT GetContentThumbnail(const uint64_t xuid,
                               const XCONTENT_AGGREGATE_DATA& data,
                               std::vector<uint8_t>* buffer);
  // The package header's metadata, an XContentMetadata.
  X_RESULT GetContentMetadata(const uint64_t xuid,
                              const XCONTENT_AGGREGATE_DATA& data,
                              std::vector<uint8_t>* buffer);
  // The package's file times and size. A game's size includes its data files.
  X_RESULT GetContentAttributes(const uint64_t xuid,
                                const XCONTENT_AGGREGATE_DATA& data,
                                ContentAttributes* attributes);
  X_RESULT SetContentThumbnail(const uint64_t xuid,
                               const XCONTENT_AGGREGATE_DATA& data,
                               std::vector<uint8_t> buffer);
  X_RESULT DeleteContent(const uint64_t xuid,
                         const XCONTENT_AGGREGATE_DATA& data);
  std::filesystem::path ResolveGameUserContentPath(const uint64_t xuid);
  bool IsContentOpen(const std::filesystem::path& package_path) const;
  void CloseOpenedFilesFromContent(const ContentPackage& package);

  uint64_t GetContentTotalSpace() const;
  uint64_t GetContentFreeSpace() const;

 private:
  std::unique_ptr<ManagedFormat> BeginIdleManagedFormatExpected(
      const std::string&, std::shared_ptr<const ManagedFormatSourceProof>,
      std::error_code&);
  struct PackageMutationTarget {
    // Retain the exact volume, projection and source generation, not a new
    // writable source chosen after resolving an existing package elsewhere.
    ContentSourceState source;
    std::filesystem::path relative;
  };
  X_RESULT MutateContentPackage(uint64_t, const XCONTENT_AGGREGATE_DATA&, bool truncate);
  X_RESULT ResolvePackageMutation(uint64_t xuid,
                                   const XCONTENT_AGGREGATE_DATA&,
                                   PackageMutationTarget&, bool retain_source = false);
  // Cached exact physical-path claims close the mount-publication race with
  // package retirement. Only resolved retained source paths enter this map;
  // no filesystem observation/provider callback occurs under GCRC.
  friend class ContentPackageSelectionTestAccess;
  struct PackageAdmission;
  struct PackageSelection {
    // Release the cached claim LAST, after all retained storage/FDs are gone.
    std::shared_ptr<PackageAdmission> claim;
    std::filesystem::path path;
    bool library = false, read_only = false;
    std::optional<ContentSourceState> source;
    std::shared_ptr<filesystem::HostStorageRoot> directory;
    std::unique_ptr<filesystem::FileHandle> file;
    std::optional<filesystem::FileHandleInfo> file_info;
  };
  std::unique_ptr<PackageSelection> CapturePackageSelection(
      uint64_t, const XCONTENT_AGGREGATE_DATA&, X_RESULT&);
  X_RESULT SelectPackageAt(PackageSelection&, std::filesystem::path, bool library,
      bool read_only, std::optional<ContentSourceState>);
  bool ValidatePackageSelection(const PackageSelection&) const;
  std::unique_ptr<ContentPackage> MountPackageSelection(
      std::string_view, const XCONTENT_AGGREGATE_DATA&, PackageSelection&, uint32_t);
  struct PendingPackageAdmission { size_t readers = 0; bool mutation = false; };
  struct PackageAdmissionState {
    std::mutex mutex;
    std::map<std::filesystem::path, PendingPackageAdmission> pending;
    bool closed = false;
  };
  std::shared_ptr<PackageAdmissionState> package_admissions_ =
      std::make_shared<PackageAdmissionState>();
  std::shared_ptr<PackageAdmission> ClaimPackagePath(
      const std::filesystem::path&, bool mutation);
  struct RootAdmission;
  std::unique_ptr<RootAdmission> ClaimRootName(std::string_view);
  std::unordered_set<string_key_insensitive> pending_package_roots_;
  struct ContentFlush;
  // Entries with abandoned guest waiters remain bounded event/status records
  // until manager teardown. Completed entries retain no filesystem owner.
  std::mutex content_flush_mutex_;
  std::map<uint64_t, std::shared_ptr<ContentFlush>> content_flushes_;
  uint64_t next_content_flush_ = 1;
  X_RESULT PublishContentHeader(uint64_t, XCONTENT_AGGREGATE_DATA, bool create,
                               PackageSelection* created_selection = nullptr,
                               std::span<const filesystem::ContentPackageInitialMember>
                                   initial_members = {});
  // Resolve an auxiliary file through the same live source as its package.
  // Both paths are checked, including symlinks in Headers or thumbnail names.
  std::optional<std::filesystem::path> ResolveSourcePath(
      const std::filesystem::path& package_path,
      const std::filesystem::path& target_path, bool for_write,
      std::string_view source_id = {}) const;
  bool ReadPhysicalContentData(const std::filesystem::path& package_path,
                               uint64_t xuid, uint32_t title_id,
                               XContentType content_type,
                               XCONTENT_AGGREGATE_DATA& data,
                               std::string_view source_id) const;
  X_RESULT ReadContentHeaderFileFromSource(
      const std::filesystem::path& package_path, uint64_t xuid,
      uint32_t title_id, XContentType content_type,
      XCONTENT_AGGREGATE_DATA& data, std::string_view source_id) const;
  struct PackageSourcePath {
    std::string source_id;
    std::filesystem::path path;
  };
  std::vector<PackageSourcePath> ReadPackageRoots(
      uint32_t device_id, uint64_t xuid, uint32_t title_id,
      XContentType content_type) const;
  // X_ERROR_INVALID_NAME for a file name the file system refuses and
  // X_ERROR_PATH_NOT_FOUND for one that names no package.
  X_RESULT ResolvePackagePath(const uint64_t xuid,
                              const XCONTENT_AGGREGATE_DATA& data,
                              std::filesystem::path& path,
                              std::string* source_id = nullptr,
                              ContentSourceState* retained = nullptr);
  X_RESULT ResolveWritePath(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                            std::filesystem::path& path,
                            std::string* source_id = nullptr,
                            ContentSourceState* retained = nullptr);
  // Where a package is read from, which is the game library's release for a
  // record of ListLibraryContent.
  X_RESULT ResolveReadPath(const uint64_t xuid,
                           const XCONTENT_AGGREGATE_DATA& data,
                           std::filesystem::path& path, bool* library = nullptr,
                           bool* read_only = nullptr,
                           std::string* source_id = nullptr,
                           ContentSourceState* retained = nullptr,
                           PackageSelection* selecting = nullptr);
  std::string ResolveDiscPackageRoot(const uint64_t xuid, uint32_t title_id,
                                     const XContentType content_type) const;
  vfs::Entry* ResolveDiscPackage(const XCONTENT_AGGREGATE_DATA& data) const;
  // The folders named by 8 hex digits in the user's content folder on the
  // device, or in its folder for |title_id|, as title IDs and content types
  // are.
  std::vector<uint32_t> ListHexFolders(
      const uint32_t device_id, const uint64_t xuid,
      const std::optional<uint32_t> title_id) const;
  // Closes |package| and the packages mounted from files in it.
  void ClosePackage(ContentPackage* package);

  std::unordered_set<uint32_t> FindPublisherTitleIds(
      const uint64_t xuid,
      uint32_t base_title_id = kCurrentlyRunningTitleId,
      uint32_t device_id = 1) const;

  bool UpdateSpaData(std::string_view guest_path);

  bool ResolveTitleId(uint32_t& title_id) const;
  KernelState* kernel_state_ = nullptr;
  std::shared_ptr<const filesystem::ManagedStorageContext> storage_context_;
  std::filesystem::path root_path_;
  // Shared storage authority, independent of the ContentManager/Kernel wrapper.
  // Pending owned operations retain uncertainty but never borrow guest runtime.
  std::shared_ptr<ManagedFormat::Owners> managed_owners_;
  ContentSourceRegistry sources_;

  // TODO(benvanik): remove use of global lock, it's bad here!
  xe::global_critical_region global_critical_region_;
  std::unordered_map<string_key_insensitive, ContentPackage*> open_packages_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_CONTENT_MANAGER_H_
