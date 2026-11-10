/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include "xenia/kernel/xam/content_manager.h"
#include "xenia/base/content_mutation_owner.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/vfs/file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_content_storage_source.h"
#include "xenia/kernel/native_managed_format_frontdoor.h"
#include "xenia/base/byte_order.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace xe::kernel::test {
class NativeManagedFormatFrontdoorTestAccess {
 public:
  using Lineage = NativeManagedFormatFrontdoor::Lineage;
  using Frame = NativeManagedFormatFrontdoor::Frame;
  using Raw = NativeManagedFormatFrontdoor::Raw;
  using Snapshot = NativeManagedFormatFrontdoor::Snapshot;
  using Geometry = NativeManagedFormatFrontdoor::Geometry;
};
}
namespace xe::kernel::xam {
// Exercise the production retained-selection producer without manufacturing a
// KernelState or mount success. These fixtures test real registry/FD admission
// and the public mutators at the actual pre-mount ownership boundary.
class ContentPackageSelectionTestAccess final {
 public:
  using Selection = ContentManager::PackageSelection;
  static std::unique_ptr<Selection> Capture(ContentManager& owner, uint64_t xuid,
      const XCONTENT_AGGREGATE_DATA& data, X_RESULT& result) {
    return owner.CapturePackageSelection(xuid, data, result);
  }
  static std::unique_ptr<Selection> Create(ContentManager& owner, uint64_t xuid,
      const XCONTENT_AGGREGATE_DATA& data, X_RESULT& result) {
    auto selected = std::make_unique<Selection>();
    result = owner.PublishContentHeader(xuid, data, true, selected.get());
    if (result) { return {}; }
  return selected;
  }
  static std::unique_ptr<Selection> LibraryAlias(ContentManager& owner,
      const std::filesystem::path& path, X_RESULT& result) {
    auto selected = std::make_unique<Selection>();
    result = owner.SelectPackageAt(*selected, path, true, true, {});
    if (result) { return {}; } return selected;
  }
  static bool Current(const ContentManager& owner, const Selection& selected) {
    return owner.ValidatePackageSelection(selected);
  }
};
}  // namespace xe::kernel::xam

namespace xe::kernel::xam::test {
namespace {

struct CatalogScratch {
  std::filesystem::path root;
  CatalogScratch() {
    std::random_device rng;
    for (unsigned i = 0; i < 16; ++i) {
      auto candidate = std::filesystem::temp_directory_path() /
                       ("xenia_catalog_" + std::to_string(rng()) + "_" +
                        std::to_string(rng()));
      if (std::filesystem::create_directory(candidate)) {
        root = std::move(candidate);
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    std::filesystem::create_directory(root / "hdd");
    std::filesystem::create_directory(root / "system");
  }
  ~CatalogScratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

// The same native parent/recovery factory used by production, without an
// emulated title, KernelState, manual readiness flag or isolated SpaceQuery.
struct ManagedCatalogStorage {
  std::shared_ptr<filesystem::HostStorageRoot> data_parent;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery;
  std::shared_ptr<const filesystem::ManagedStorageContext> context;
  explicit ManagedCatalogStorage(const CatalogScratch& scratch) {
    std::error_code error;
    data_parent =
        filesystem::HostStorageRoot::Open(scratch.root, false, error, 0);
    REQUIRE(data_parent);
    REQUIRE_FALSE(error);
    REQUIRE(data_parent->native_filesystem_authority());
    recovery = filesystem::SnapshotRecoveryOwner::Open(data_parent);
    REQUIRE(recovery);
    REQUIRE_FALSE(recovery->error());
    // The fixture explicitly chooses its owned parent for nondefault Content,
    // so it never writes a private container into the host mounted root.
    context = filesystem::ManagedStorageContext::Capture(data_parent, recovery,
                                                         data_parent, error);
    REQUIRE(context);
    REQUIRE_FALSE(error);
  }
  void RequireWritable(ContentManager& manager, const std::string& id = "hdd") {
    REQUIRE(manager.managed_sources_ready());
    const auto source = manager.sources().Source(id);
    REQUIRE(source);
    REQUIRE(source->available);
    REQUIRE_FALSE(source->source.read_only);
    REQUIRE(source->storage);
    REQUIRE(source->managed_identity);
    REQUIRE_FALSE(source->managed_identity->CheckCurrent());
    const auto volume = manager.Volume(id);
    REQUIRE(volume);
    REQUIRE(volume->profile_writable());
    REQUIRE(volume->identity() == source->managed_identity);
    REQUIRE(manager.ProfileIntents(id) == volume->intents());
  }
};

XCONTENT_AGGREGATE_DATA ProfileRecord() {
  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.xuid = 1;
  data.title_id = 0xFFFE07D1;
  data.content_type = XContentType::kProfile;
  data.set_file_name("profile");
  data.set_display_name(u"Profile");
  return data;
}

void WriteLegacyHeader(const std::filesystem::path& path,
                       const XCONTENT_AGGREGATE_DATA& data) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(&data), sizeof(data));
  REQUIRE(file.good());
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("Installed system packages share discovery without user copies",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto type = static_cast<XContentType>(0x8000);
  const auto package = scratch.root /
                       "system/0000000000000000/FFFE07DF/00008000/"
                       "FFFE07DF00000002";
  REQUIRE(std::filesystem::create_directories(package));
  std::ofstream(package / "Asset.toc") << "fixture";
  // Listing exact known IDs requires no emulated title or guest allocation.
  ContentManager content(ContentManager::ReadOnlyCatalogTag{},
                         scratch.root / "hdd");
  REQUIRE(content.sources().Register(
      {"installed", 1, DeviceType::HDD, ContentSourceKind::kInstalledSystem,
       scratch.root / "system", "", u"System", true, 0}));
  const auto listed = content.ListContent(1, 0, 0xFFFE07DF, type);
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].device_id == 1);
  REQUIRE(listed[0].title_id == 0xFFFE07DF);
  REQUIRE(listed[0].file_name() == "FFFE07DF00000002");
  REQUIRE(content.ListContentTitleIds(1, 0) ==
          std::vector<uint32_t>{0xFFFE07DF});
  REQUIRE(content.ListContentTypes(1, 0, 0xFFFE07DF) ==
          std::vector<XContentType>{type});
  REQUIRE(content.ListContentXuids() == std::vector<uint64_t>{0});
  REQUIRE(content.ListContent(77, 0, 0xFFFE07DF, type).empty());
  REQUIRE(std::filesystem::is_empty(scratch.root / "hdd"));
}

TEST_CASE("Legacy sidecars cannot replace content identity with zero bytes",
          "[content][catalog]") {
  CatalogScratch scratch;
  auto package =
      scratch.root / "hdd/0000000000000001/FFFE07D1/00010000/profile";
  auto headers =
      scratch.root / "hdd/0000000000000001/FFFE07D1/Headers/00010000";
  REQUIRE(std::filesystem::create_directories(package));
  REQUIRE(std::filesystem::create_directories(headers));
  ContentManager content(ContentManager::ReadOnlyCatalogTag{},
                         scratch.root / "hdd");
  XCONTENT_AGGREGATE_DATA metadata{};
  auto write_header = [&] {
    std::ofstream file(headers / "profile.header", std::ios::binary);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    REQUIRE(file.good());
  };
  write_header();
  XCONTENT_AGGREGATE_DATA read{};
  REQUIRE(XFAILED(content.ReadContentHeaderFile(package, 1, 0xFFFE07D1,
                                                XContentType::kProfile, read)));
  // Invalid metadata does not suppress the actual folder or reassign its user.
  auto listed = content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile);
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].xuid == 1);
  REQUIRE(listed[0].title_id == 0xFFFE07D1);
  metadata.content_type = XContentType::kProfile;
  metadata.title_id = 0xFFFE07D1;
  metadata.set_file_name("profile");
  metadata.set_display_name(u"Existing name");
  write_header();
  REQUIRE(XSUCCEEDED(content.ReadContentHeaderFile(
      package, 1, 0xFFFE07D1, XContentType::kProfile, read)));
  REQUIRE(read.xuid == 1);  // Legacy omitted owner, location is authoritative.
  REQUIRE(read.display_name() == u"Existing name");
  metadata.xuid = 2;
  write_header();
  REQUIRE(XFAILED(content.ReadContentHeaderFile(package, 1, 0xFFFE07D1,
                                                XContentType::kProfile, read)));
  metadata.xuid = 1;
  metadata.set_file_name("other-profile");
  write_header();
  REQUIRE(XFAILED(content.ReadContentHeaderFile(package, 1, 0xFFFE07D1,
                                                XContentType::kProfile, read)));
  listed = content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile);
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].file_name() == "profile");
  REQUIRE(listed[0].display_name() == u"profile");
  metadata.set_file_name("PROFILE");
  write_header();
  REQUIRE(XSUCCEEDED(content.ReadContentHeaderFile(
      package, 1, 0xFFFE07D1, XContentType::kProfile, read)));
  REQUIRE(read.file_name() == "profile");
}

TEST_CASE("Merged discovery and opening fall back to the same valid source",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto relative = "0000000000000001/FFFE07D1/00010000/profile";
  const auto managed = scratch.root / "hdd" / relative;
  const auto installed = scratch.root / "system" / relative;
  REQUIRE(std::filesystem::create_directories(managed.parent_path()));
  std::ofstream(managed) << "corrupt package file";
  REQUIRE(std::filesystem::create_directories(installed));
  std::ofstream(installed / "Account") << "system fixture";
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  REQUIRE(content.sources().Register(
      {"installed", 1, DeviceType::HDD, ContentSourceKind::kInstalledSystem,
       scratch.root / "system", "\\SystemContent", u"System", true, 0}));
  const auto listed =
      content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile);
  REQUIRE(listed.size() == 1);
  const auto data = ProfileRecord();
  REQUIRE(content.ContentExists(1, data));
  std::string guest;
  REQUIRE(content.ResolveGuestContentPath(1, data, false, guest) ==
          X_ERROR_SUCCESS);
  REQUIRE(guest ==
          "\\SystemContent\\0000000000000001\\FFFE07D1\\00010000\\profile");
  REQUIRE(content.ResolveGuestContentPath(1, data, true, guest) ==
          X_ERROR_ACCESS_DENIED);
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.WriteContentHeaderFile(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.SetContentThumbnail(1, data, {1, 2, 3}) ==
          X_ERROR_ACCESS_DENIED);
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(ReadText(managed) == "corrupt package file");
  REQUIRE(ReadText(installed / "Account") == "system fixture");
}

TEST_CASE("Header names never redirect physical content identity",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto package =
      scratch.root / "hdd/0000000000000000/FFFE07D1/00000001/profile";
  const auto headers =
      scratch.root / "hdd/0000000000000000/FFFE07D1/Headers/00000001";
  REQUIRE(std::filesystem::create_directories(package));
  REQUIRE(std::filesystem::create_directories(headers));
  ContentManager content(ContentManager::ReadOnlyCatalogTag{},
                         scratch.root / "hdd");
  XCONTENT_AGGREGATE_DATA read{};
  SECTION("Legacy shared marker is normalized against the actual leaf") {
    auto data = ProfileRecord();
    data.content_type = XContentType::kSavedGame;
    data.xuid = 0;
    data.file_name_raw[0] =
        char(uint8_t(data.file_name_raw[0]) | kSharedContentFileNameBit);
    WriteLegacyHeader(headers / "profile.header", data);
    REQUIRE(XSUCCEEDED(content.ReadContentHeaderFile(
        package, 0, 0xFFFE07D1, XContentType::kSavedGame, read)));
    REQUIRE(read.file_name() == "profile");
    REQUIRE(read.xuid == 0);
  }
  SECTION("Container header strings are bounded and appended names ignored") {
    auto header = std::make_unique<vfs::XContentContainerHeader>();
    header->content_header.magic = vfs::XContentPackageType::kPirs;
    auto& metadata = header->content_metadata;
    metadata.metadata_version = 2;
    metadata.content_type = XContentType::kSavedGame;
    metadata.execution_info.title_id = 0xFFFE07D1;
    for (auto& name : metadata.display_name_raw.uint) {
      std::fill(std::begin(name), std::end(name), uint16_t('x'));
    }
    for (auto& name : metadata.display_name_ex_raw.uint) {
      std::fill(std::begin(name), std::end(name), uint16_t('y'));
    }
    {
      std::ofstream file(headers / "profile.header", std::ios::binary);
      file.write(reinterpret_cast<const char*>(header.get()), sizeof(*header));
      file << "different-package";
      REQUIRE(file.good());
    }
    REQUIRE(XSUCCEEDED(content.ReadContentHeaderFile(
        package, 0, 0xFFFE07D1, XContentType::kSavedGame, read)));
    REQUIRE(read.file_name() == "profile");
    REQUIRE(read.display_name() == std::u16string(127, u'x'));
    auto data = ProfileRecord();
    data.content_type = XContentType::kSavedGame;
    data.xuid = 0;
    std::vector<uint8_t> bytes;
    REQUIRE(content.GetContentMetadata(0, data, &bytes) == X_ERROR_SUCCESS);
    REQUIRE(bytes.size() == sizeof(vfs::XContentMetadata));
    std::memcpy(&metadata, bytes.data(), bytes.size());
    REQUIRE(metadata.display_name_raw.uint[0][127] == 0);
    REQUIRE(metadata.display_name_ex_raw.uint[0][127] == 0);
  }
}

TEST_CASE("Selected source provenance survives overlapping host roots",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto overlay = scratch.root / "hdd/installed-system";
  const auto package = overlay / "0000000000000001/FFFE07D1/00010000/profile";
  REQUIRE(std::filesystem::create_directories(package));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  REQUIRE(content.sources().Register(
      {"installed", 1, DeviceType::HDD, ContentSourceKind::kInstalledSystem,
       overlay, "\\SystemContent", u"System", true, 0}));
  const auto data = ProfileRecord();
  std::string guest;
  REQUIRE(content.ResolveGuestContentPath(1, data, false, guest) ==
          X_ERROR_SUCCESS);
  REQUIRE(guest ==
          "\\SystemContent\\0000000000000001\\FFFE07D1\\00010000\\profile");
  REQUIRE(content.ResolveGuestContentPath(1, data, true, guest) ==
          X_ERROR_ACCESS_DENIED);
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(std::filesystem::is_directory(package));
}

TEST_CASE("Catalog discovery and writes retain actual FATX component casing",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto type = static_cast<XContentType>(0x00ABCDEF);
  const auto package =
      scratch.root / "hdd/a000000000000001/fffe07df/00abcdef/MySave";
  const auto headers =
      scratch.root / "hdd/a000000000000001/fffe07df/headers/00abcdef";
  REQUIRE(std::filesystem::create_directories(package));
  REQUIRE(std::filesystem::create_directories(headers));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.xuid = 0xA000000000000001;
  data.title_id = 0xFFFE07DF;
  data.content_type = type;
  data.set_file_name("mysave");
  data.set_display_name(u"Kept name");
  REQUIRE(content.ListContentXuids() ==
          std::vector<uint64_t>{0xA000000000000001});
  REQUIRE(content.ListContentTitleIds(1, data.xuid) ==
          std::vector<uint32_t>{0xFFFE07DF});
  REQUIRE(content.ListContentTypes(1, data.xuid, 0xFFFE07DF) ==
          std::vector<XContentType>{type});
  REQUIRE(content.ContentExists(data.xuid, data));
  REQUIRE(content.WriteContentHeaderFile(data.xuid, data) == X_ERROR_SUCCESS);
  REQUIRE(std::filesystem::is_regular_file(headers / "MySave.header"));
  const auto listed = content.ListContent(1, data.xuid, 0xFFFE07DF, type);
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].file_name() == "MySave");
  REQUIRE(listed[0].display_name() == u"Kept name");
  REQUIRE(content.SetContentThumbnail(data.xuid, data, {1, 2, 3}) ==
          X_ERROR_SUCCESS);
  std::vector<uint8_t> thumbnail;
  REQUIRE(content.GetContentThumbnail(data.xuid, data, &thumbnail) ==
          X_ERROR_SUCCESS);
  REQUIRE(thumbnail == std::vector<uint8_t>{1, 2, 3});
  const auto source = content.sources().Source("hdd");
  REQUIRE(source);
  vfs::HostPathDevice device("\\Catalog", source->storage->path(), false, false,
                             "FATX", source->storage);
  REQUIRE(device.Initialize());
  const auto* root = device.ResolvePath("");
  REQUIRE(root);
  REQUIRE(root->children().size() == 1);
  REQUIRE(root->children()[0]->name() == "a000000000000001");
  REQUIRE(std::filesystem::is_regular_file(
      scratch.root / "hdd" / filesystem::ManagedDeviceIdentity::kMarker));
  REQUIRE_FALSE(std::filesystem::exists(scratch.root / "hdd/Content"));
}

TEST_CASE("Unavailable storage is never recreated by a content operation",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto usb = scratch.root / "usb";
  REQUIRE(std::filesystem::create_directory(usb));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  REQUIRE(content.RegisterManagedSource({"usb",
                                         3,
                                         DeviceType::USBMASS,
                                         ContentSourceKind::kManaged,
                                         usb,
                                         "\\USBContent",
                                         u"USB",
                                         false,
                                         0,
                                         "\\USB",
                                         {},
                                         scratch.root}));
  storage.RequireWritable(content, "usb");
  auto data = ProfileRecord();
  data.device_id = 3;
  const auto removed_usb = scratch.root / "removed-usb";
  std::filesystem::rename(usb, removed_usb);
  REQUIRE_FALSE(std::filesystem::exists(usb));
  REQUIRE(std::filesystem::is_regular_file(
      removed_usb / filesystem::ManagedDeviceIdentity::kMarker));
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(content.WriteContentHeaderFile(1, data) ==
          X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(content.SetContentThumbnail(1, data, {}) ==
          X_ERROR_DEVICE_NOT_CONNECTED);
  std::string guest;
  REQUIRE(content.ResolveGuestContentPath(1, data, true, guest) ==
          X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE_FALSE(content.ContentExists(1, data));
  REQUIRE_FALSE(std::filesystem::exists(usb));
  data.device_id = 77;
  REQUIRE(content.ResolveGuestContentPath(1, data, false, guest) ==
          X_ERROR_DEVICE_NOT_CONNECTED);
  data.device_id = static_cast<uint32_t>(DummyDeviceId::ODD);
  REQUIRE(content.ResolveGuestContentPath(1, data, true, guest) ==
          X_ERROR_ACCESS_DENIED);
}

TEST_CASE("Readonly-only devices deny writes without reporting disconnection",
          "[content][catalog]") {
  CatalogScratch scratch;
  ContentManager content(ContentManager::ReadOnlyCatalogTag{},
                         scratch.root / "hdd");
  REQUIRE(content.sources().Register(
      {"readonly-device", 7, DeviceType::HDD,
       ContentSourceKind::kInstalledSystem, scratch.root / "system",
       "\\ReadonlyContent", u"Readonly", true, 0}));
  auto data = ProfileRecord();
  data.device_id = 7;
  REQUIRE_FALSE(content.ContentExists(1, data));
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.WriteContentHeaderFile(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.SetContentThumbnail(1, data, {}) == X_ERROR_ACCESS_DENIED);
  std::string guest;
  REQUIRE(content.ResolveGuestContentPath(1, data, true, guest) ==
          X_ERROR_ACCESS_DENIED);
  REQUIRE(content.ResolveGuestContentPath(1, data, false, guest) ==
          X_ERROR_PATH_NOT_FOUND);
  REQUIRE(std::filesystem::remove(scratch.root / "system"));
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("A readonly overlay does not reconnect its missing writable device",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto managed = scratch.root / "usb";
  REQUIRE(std::filesystem::create_directory(managed));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  REQUIRE(content.RegisterManagedSource({"usb",
                                         3,
                                         DeviceType::USBMASS,
                                         ContentSourceKind::kManaged,
                                         managed,
                                         "\\USBContent",
                                         u"USB",
                                         false,
                                         0,
                                         "\\USB",
                                         {},
                                         scratch.root}));
  storage.RequireWritable(content, "usb");
  REQUIRE(content.sources().Register(
      {"usb-overlay", 3, DeviceType::USBMASS,
       ContentSourceKind::kInstalledSystem, scratch.root / "system",
       "\\USBSystemContent", u"USB system", true, 0}));
  auto data = ProfileRecord();
  data.device_id = 3;
  const auto removed_usb = scratch.root / "removed-usb";
  std::filesystem::rename(managed, removed_usb);
  REQUIRE_FALSE(std::filesystem::exists(managed));
  REQUIRE(std::filesystem::is_regular_file(
      removed_usb / filesystem::ManagedDeviceIdentity::kMarker));
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE_FALSE(std::filesystem::exists(managed));
}

TEST_CASE("Catalog operations refuse a package symlink outside its source",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto parent = scratch.root / "hdd/0000000000000001/FFFE07D1/00010000";
  const auto outside = scratch.root / "outside";
  REQUIRE(std::filesystem::create_directories(parent));
  REQUIRE(std::filesystem::create_directory(outside));
  std::ofstream(outside / "Account") << "preserved";
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside, parent / "profile", ec);
  if (ec) {
    WARN("Host does not permit symlink fixtures");
    return;
  }
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  std::filesystem::create_directory_symlink(
      outside, scratch.root / "hdd/A000000000000001", ec);
  REQUIRE_FALSE(ec);
  REQUIRE(content.ListContentXuids() == std::vector<uint64_t>{1});
  REQUIRE(content.ListContentTitleIds(1, 0xA000000000000001).empty());
  const auto data = ProfileRecord();
  REQUIRE(
      content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile).empty());
  REQUIRE_FALSE(content.ContentExists(1, data));
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.WriteContentHeaderFile(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.SetContentThumbnail(1, data, {4, 5, 6}) ==
          X_ERROR_ACCESS_DENIED);
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_ACCESS_DENIED);
  std::string guest;
  REQUIRE(content.ResolveGuestContentPath(1, data, false, guest) ==
          X_ERROR_PATH_NOT_FOUND);
  REQUIRE(content.ResolveGuestContentPath(1, data, true, guest) ==
          X_ERROR_ACCESS_DENIED);
  REQUIRE(ReadText(outside / "Account") == "preserved");
  REQUIRE(std::filesystem::is_symlink(parent / "profile"));
}

TEST_CASE("Auxiliary content paths obey the package source boundary",
          "[content][catalog]") {
  CatalogScratch scratch;
  const auto title = scratch.root / "hdd/0000000000000001/FFFE07D1";
  const auto package = title / "00010000/profile";
  const auto outside = scratch.root / "outside";
  REQUIRE(std::filesystem::create_directories(package));
  REQUIRE(std::filesystem::create_directory(outside));
  auto data = ProfileRecord();
  data.set_display_name(u"Outside metadata");
  std::error_code ec;
  SECTION("Headers directory") {
    REQUIRE(std::filesystem::create_directory(outside / "00010000"));
    WriteLegacyHeader(outside / "00010000/profile.header", data);
    std::filesystem::create_directory_symlink(outside, title / "Headers", ec);
  }
  SECTION("Header leaf") {
    REQUIRE(std::filesystem::create_directories(title / "Headers/00010000"));
    WriteLegacyHeader(outside / "profile.header", data);
    std::filesystem::create_symlink(outside / "profile.header",
                                    title / "Headers/00010000/profile.header",
                                    ec);
  }
  if (ec) {
    WARN("Host does not permit symlink fixtures");
    return;
  }
  std::ofstream(outside / "thumbnail") << "preserved thumbnail";
  std::filesystem::create_symlink(outside / "thumbnail",
                                  package / vfs::kPackageThumbnailFileName, ec);
  if (ec) {
    WARN("Host does not permit symlink fixtures");
    return;
  }
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  XCONTENT_AGGREGATE_DATA read{};
  REQUIRE(XFAILED(content.ReadContentHeaderFile(package, 1, 0xFFFE07D1,
                                                XContentType::kProfile, read)));
  const auto listed =
      content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile);
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].file_name() == "profile");
  REQUIRE(listed[0].display_name() == u"profile");
  REQUIRE(content.WriteContentHeaderFile(1, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content.SetContentThumbnail(1, data, {7, 8, 9}) ==
          X_ERROR_ACCESS_DENIED);
  std::vector<uint8_t> bytes;
  REQUIRE(content.GetContentThumbnail(1, data, &bytes) ==
          X_ERROR_FILE_NOT_FOUND);
  REQUIRE(content.GetContentMetadata(1, data, &bytes) ==
          X_ERROR_FILE_NOT_FOUND);
  REQUIRE(bytes.empty());
  REQUIRE(ReadText(outside / "thumbnail") == "preserved thumbnail");
}

TEST_CASE("Offline catalogs cannot acquire managed storage authority",
          "[content][catalog]") {
  CatalogScratch scratch;
  ContentManager content(ContentManager::ReadOnlyCatalogTag{},
                         scratch.root / "hdd");
  REQUIRE(content.sources().read_only_catalog());
  REQUIRE_FALSE(content.managed_sources_ready());
  REQUIRE_FALSE(content.sources().WritableSource(1));
  REQUIRE_FALSE(content.ProfileIntents("hdd"));
  REQUIRE_FALSE(content.Volume("hdd"));
  REQUIRE(content.GetContentTotalSpace() == 0);
  REQUIRE(content.GetContentFreeSpace() == 0);
  REQUIRE_FALSE(content.RegisterManagedSource(
      {"forbidden", 3, DeviceType::USBMASS, ContentSourceKind::kManaged,
       scratch.root / "hdd", "\\USBContent", u"USB", false, 0}));
  // A real, independently initialized source proves that RegisterManaged is
  // refused by the readonly policy, rather than by an absent/invalid identity.
  CatalogScratch writable;
  ManagedCatalogStorage storage(writable);
  ContentManager managed(storage.context, writable.root / "hdd");
  storage.RequireWritable(managed);
  const auto actual = managed.sources().Source("hdd");
  REQUIRE(actual);
  ContentSourceRegistry references(
      ContentSourceRegistry::Access::kReadOnlyCatalog);
  REQUIRE_FALSE(references.RegisterManaged(
      actual->source, actual->managed_identity, actual->volume));
  auto readonly_managed = actual->source;
  readonly_managed.read_only = true;
  REQUIRE_FALSE(references.Register(readonly_managed));
  REQUIRE_FALSE(references.Register(actual->source));
  REQUIRE(references.DeviceIds().empty());
  const auto notifications = references.storage_notifications();
  REQUIRE(notifications);
  auto connection = notifications->Connect([](NativeContentStorageSnapshot) {});
  REQUIRE(connection);
  REQUIRE(connection->initial.devices.empty());
  connection->subscription->BeginStop();
  REQUIRE(connection->subscription->Drain());
  REQUIRE(std::filesystem::is_empty(scratch.root / "hdd"));
  REQUIRE_FALSE(std::filesystem::exists(
      scratch.root / filesystem::SnapshotRecoveryOwner::kDirectory));
  // Invalid explicit managed context is not converted into legacy readiness.
  ContentManager unavailable(
      std::shared_ptr<const filesystem::ManagedStorageContext>{},
      scratch.root / "hdd");
  REQUIRE_FALSE(unavailable.managed_sources_ready());
  REQUIRE(unavailable.sources().DeviceIds().empty());
  REQUIRE(std::filesystem::is_empty(scratch.root / "hdd"));
}

TEST_CASE("Owned storage writes do not imply a guest mount context",
          "[content][catalog]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  auto data = ProfileRecord();
  data.set_file_name("new-profile");
  const auto package =
      scratch.root / "hdd/0000000000000001/FFFE07D1/00010000/new-profile";
  // Guest mounting must fail before its create step touches the host source.
  REQUIRE(content.CreateContent("probe", 1, data) == X_ERROR_NOT_SUPPORTED);
  REQUIRE_FALSE(std::filesystem::exists(package));
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_SUCCESS);
  REQUIRE(std::filesystem::is_directory(package));
  REQUIRE(content.ContentExists(1, data));
  uint32_t license = 0;
  REQUIRE(content.OpenContent("probe", 1, data, license) ==
          X_ERROR_NOT_SUPPORTED);
  REQUIRE_FALSE(content.ResolvePackage("probe", 1, data));
  REQUIRE(content.CloseContent("probe") == X_ERROR_NOT_SUPPORTED);
  REQUIRE(content.ListLibraryContent().empty());
  REQUIRE(content.ResolveGameUserContentPath(1).empty());
  auto current = data;
  current.title_id = kCurrentlyRunningTitleId;
  REQUIRE(
      content
          .ListContent(1, 1, kCurrentlyRunningTitleId, XContentType::kProfile)
          .empty());
  REQUIRE(content.ListContentTypes(1, 1, kCurrentlyRunningTitleId).empty());
  REQUIRE_FALSE(content.ContentExists(1, current));
  REQUIRE(content.WriteContentHeaderFile(1, current) == X_ERROR_NOT_SUPPORTED);
  REQUIRE(content.CreatePackage(1, current) == X_ERROR_NOT_SUPPORTED);
  current.device_id = static_cast<uint32_t>(DummyDeviceId::ODD);
  REQUIRE_FALSE(content.ContentExists(1, current));
  REQUIRE(content
              .ListContentODD(static_cast<uint32_t>(DummyDeviceId::ODD), 0,
                              0xFFFE07D1, XContentType::kProfile)
              .empty());
  storage.RequireWritable(content);
}

TEST_CASE("Maximum-length content names are bounded by the ABI field",
          "[content][catalog]") {
  XCONTENT_AGGREGATE_DATA data;
  std::memset(&data, 'a', sizeof(data));
  REQUIRE(data.file_name() == std::string(42, 'a'));
}

TEST_CASE("Catalog destructive operations share actual file and mapping leases",
          "[content][catalog][content_mutation]") {
  CatalogScratch scratch;
  const std::filesystem::path relative =
      "0000000000000001/FFFE07D1/00010000/PrOfIlE";
  const auto package = scratch.root / "hdd" / relative;
  const auto other = package.parent_path() / "other";
  REQUIRE(std::filesystem::create_directories(package / "nested"));
  REQUIRE(std::filesystem::create_directory(other));
  std::ofstream(package / "nested/save") << "save";
  std::ofstream(package / vfs::kPackageThumbnailFileName) << "picture";
  std::ofstream(other / "Account") << "other package";
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto source = content.sources().Source("hdd");
  REQUIRE(source);
  auto data = ProfileRecord();
  data.set_file_name("PROFILE");
  std::error_code error;
  auto directory = source->storage->Subroot(relative, false, error);
  REQUIRE(directory);
  auto held = source->storage->OpenFile(relative / "nested/save",
                                        filesystem::FileAccess::kFileReadData,
                                        error);
  REQUIRE(held);
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_SHARING_VIOLATION);
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_SHARING_VIOLATION);
  REQUIRE(ReadText(package / "nested/save") == "save");
  held.reset();
  auto mapped = source->storage->MapRead(relative / "nested/save", 0, 0, error);
  REQUIRE(mapped);
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_SHARING_VIOLATION);
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_SHARING_VIOLATION);
  mapped.reset();
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_SUCCESS);
  std::vector<uint8_t> retained_picture;
  REQUIRE_FALSE(directory->ReadFile(vfs::kPackageThumbnailFileName, 16,
                                     retained_picture));
  REQUIRE(std::string(retained_picture.begin(), retained_picture.end()) ==
          "picture");
  REQUIRE_FALSE(std::filesystem::exists(package / "nested"));
  REQUIRE(ReadText(package / vfs::kPackageThumbnailFileName) == "picture");
  REQUIRE(ReadText(other / "Account") == "other package");
  REQUIRE_FALSE(std::filesystem::exists(package.parent_path() / "PROFILE"));
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_SUCCESS);
  REQUIRE_FALSE(std::filesystem::exists(package));
  REQUIRE(ReadText(other / "Account") == "other package");
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_FILE_NOT_FOUND);
}

TEST_CASE("Catalog file truncation retains managed source ownership",
          "[content][catalog][content_mutation]") {
  CatalogScratch scratch;
  const std::filesystem::path relative =
      "0000000000000001/FFFE07D1/00010000/profile";
  const auto package = scratch.root / "hdd" / relative;
  REQUIRE(std::filesystem::create_directories(package.parent_path()));
  std::ofstream(package) << "existing package bytes";
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto source = content.sources().Source("hdd");
  REQUIRE(source);
  const auto data = ProfileRecord();
  std::error_code error;
  auto held = source->storage->OpenFile(relative,
                                        filesystem::FileAccess::kFileReadData,
                                        error);
  REQUIRE(held);
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_SHARING_VIOLATION);
  REQUIRE(ReadText(package) == "existing package bytes");
  held.reset();
  // A regular container needs its actual mounted-namespace writer. Replacing
  // unknown packed bytes with a success-shaped empty host directory loses
  // metadata and is not native TruncateExisting semantics.
  const auto names_before = std::filesystem::exists(package.parent_path().parent_path() / "Headers");
  const auto package_size = std::filesystem::file_size(package);
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_NOT_SUPPORTED);
  REQUIRE(std::filesystem::is_regular_file(package));
  REQUIRE(std::filesystem::file_size(package) == package_size);
  REQUIRE(ReadText(package) == "existing package bytes");
  REQUIRE(std::filesystem::exists(package.parent_path().parent_path() / "Headers") == names_before);
  REQUIRE_FALSE(source->managed_identity->CheckCurrent());
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_SUCCESS);
  REQUIRE_FALSE(source->managed_identity->CheckCurrent());
}

TEST_CASE("Protected managed marker aliases deny catalog mutation before data",
          "[content][catalog][content_mutation]") {
  CatalogScratch scratch;
  const auto package =
      scratch.root / "hdd/0000000000000001/FFFE07D1/00010000/profile";
  REQUIRE(std::filesystem::create_directories(package));
  std::ofstream(package / "Account") << "keep this profile";
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto marker = scratch.root / "hdd" /
                      filesystem::ManagedDeviceIdentity::kMarker;
  const auto original = ReadText(marker);
  REQUIRE_FALSE(original.empty());
  std::error_code error;
  std::filesystem::create_hard_link(marker, package / "marker-alias", error);
  REQUIRE_FALSE(error);
  const auto data = ProfileRecord();
  REQUIRE(content.TruncateContent(1, data) != X_ERROR_SUCCESS);
  REQUIRE(content.DeleteContent(1, data) != X_ERROR_SUCCESS);
  REQUIRE(ReadText(package / "Account") == "keep this profile");
  REQUIRE(ReadText(marker) == original);
  REQUIRE(std::filesystem::exists(package / "marker-alias"));
}


TEST_CASE("Managed registry guest retirement preserves internal format authority",
          "[content][catalog][managed_format]") {
  CatalogScratch scratch;
  std::ofstream(scratch.root / "hdd/internal-authority-sentinel")
      << "retained internal authority";
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto first = content.sources().Source("hdd");
  REQUIRE(first);
  REQUIRE(first->storage != first->managed_identity->source());
  REQUIRE(first->storage->path() == first->managed_identity->source()->path());
  const auto marker = scratch.root / "hdd" /
                      filesystem::ManagedDeviceIdentity::kMarker;
  const auto marker_before = ReadText(marker);
  const auto volume = content.Volume("hdd");
  REQUIRE(content.sources().RegisterManaged(first->source,
                                            first->managed_identity, volume));
  const auto repeated = content.sources().Source("hdd");
  REQUIRE(repeated);
  REQUIRE(repeated->storage == first->storage);
  REQUIRE(repeated->generation == first->generation);
  std::error_code error;
  auto descendant = first->storage->Subroot({}, false, error);
  REQUIRE(descendant);
  REQUIRE_FALSE(error);
  first->storage->Retire();
  REQUIRE(descendant->retiring());
  REQUIRE(first->storage->retiring());
  REQUIRE_FALSE(first->managed_identity->source()->retiring());
  REQUIRE_FALSE(first->managed_identity->CheckCurrent());
  std::vector<uint8_t> sentinel;
  REQUIRE_FALSE(first->managed_identity->source()->ReadFile(
      "internal-authority-sentinel", 64, sentinel));
  REQUIRE(std::string(sentinel.begin(), sentinel.end()) ==
          "retained internal authority");
  REQUIRE_FALSE(content.sources().RegisterManaged(
      first->source, first->managed_identity, volume));
  const auto removed = content.sources().Source("hdd");
  REQUIRE(removed);
  REQUIRE_FALSE(removed->available);
  REQUIRE_FALSE(removed->storage);
  REQUIRE_FALSE(removed->managed_identity->CheckCurrent());
  REQUIRE(ReadText(marker) == marker_before);
}

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
TEST_CASE("Managed format withdraws before pending operation capture",
          "[content][catalog][managed_format][notifications]") {
  CatalogScratch scratch;
  const auto usb = scratch.root / "usb";
  REQUIRE(std::filesystem::create_directory(usb));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  REQUIRE(content.RegisterManagedSource(
      {"usb", 3, DeviceType::USBMASS, ContentSourceKind::kManaged, usb,
       "\\USBContent", u"USB", false, 0, "\\USB", {}, scratch.root}));
  storage.RequireWritable(content, "usb");
  std::ofstream(scratch.root / "hdd/save.bin") << "withdrawal preserves data";

  std::mutex mutex;
  std::condition_variable changed;
  bool done = false, started = false, in_factory = false;
  bool saw_withdrawal = false, pending_was_busy = false, callback_failed = false;
  std::error_code begin_error, after_error;
  std::unique_ptr<ContentManager::ManagedFormat> operation, recaptured;
  const auto notifications = content.sources().storage_notifications();
  auto connection = notifications->Connect([&](NativeContentStorageSnapshot snapshot) {
    const auto unavailable = [&](const char* id) {
      return std::any_of(snapshot.devices.begin(), snapshot.devices.end(),
                         [&](const auto& device) {
                           return device.identity == id && !device.available;
                         });
    };
    // Begin from the real monitor's USB-removal callback. While it is inside
    // this sink it cannot race a second polling refresh ahead of the nested
    // HDD withdrawal. No sleep or scheduler timing establishes the result.
    if (in_factory && unavailable("hdd")) {
      std::error_code error;
      auto premature = content.CapturePendingManagedFormat("hdd", error);
      saw_withdrawal = true;
      pending_was_busy = !premature &&
          error == std::errc::device_or_resource_busy;
      return;
    }
    if (started || !unavailable("usb")) { return; }
    started = true;
    try {
      in_factory = true;
      operation = content.BeginIdleManagedFormat("hdd", begin_error);
      in_factory = false;
      recaptured = content.CapturePendingManagedFormat("hdd", after_error);
    } catch (...) {
      in_factory = false;
      callback_failed = true;
    }
    {
      std::lock_guard lock(mutex);
      done = true;
    }
    changed.notify_all();
  });
  REQUIRE(connection);
  std::filesystem::rename(usb, scratch.root / "removed-usb");
  bool observed;
  {
    std::unique_lock lock(mutex);
    observed = changed.wait_for(lock, std::chrono::seconds(5), [&] { return done; });
  }
  // Drain before assertions or any captured callback state is destroyed.
  connection->subscription->BeginStop();
  const bool drained = connection->subscription->Drain();
  REQUIRE(drained);
  REQUIRE(observed);
  REQUIRE_FALSE(callback_failed);
  REQUIRE(saw_withdrawal);
  REQUIRE(pending_was_busy);
  REQUIRE(operation);
  REQUIRE_FALSE(begin_error);
  REQUIRE(recaptured);
  REQUIRE_FALSE(after_error);
  recaptured->Cancel();
  const auto canceled = recaptured->Execute();
  REQUIRE(canceled.error == std::errc::operation_canceled);
  REQUIRE_FALSE(canceled.epoch_published);
  REQUIRE_FALSE(canceled.data_started);
  REQUIRE(ReadText(scratch.root / "hdd/save.bin") == "withdrawal preserves data");
  const auto restored = content.sources().Source("hdd");
  REQUIRE(restored);
  REQUIRE(restored->available);
}

TEST_CASE("Managed format returns real external-owner BUSY with fresh unchanged generation",
          "[content][catalog][managed_format]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  std::ofstream(scratch.root / "hdd/save.bin") << "must remain";
  const auto marker = scratch.root / "hdd" / filesystem::ManagedDeviceIdentity::kMarker;
  const auto marker_before = ReadText(marker);
  auto first = content.sources().Source("hdd");
  REQUIRE(first);
  auto external = first->volume;
  const auto old_view = first->storage;
  const auto old_identity = first->managed_identity;
  const auto generation = first->generation;
  first.reset();
  std::error_code error;
  auto format = content.BeginIdleManagedFormat("hdd", error);
  REQUIRE(format);
  REQUIRE_FALSE(error);
  REQUIRE(old_view->retiring());
  REQUIRE_FALSE(old_identity->CheckCurrent());
  REQUIRE_FALSE(content.Volume("hdd"));
  auto withdrawn = content.sources().Source("hdd");
  REQUIRE(withdrawn);
  REQUIRE_FALSE(withdrawn->available);
  REQUIRE_FALSE(withdrawn->volume);
  const auto busy = format->Execute();
  REQUIRE(busy.error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(busy.epoch_published);
  REQUIRE_FALSE(busy.data_started);
  REQUIRE_FALSE(busy.current);
  auto restored = content.sources().Source("hdd");
  REQUIRE(restored);
  REQUIRE(restored->available);
  REQUIRE(restored->generation > generation);
  REQUIRE(restored->storage != old_view);
  REQUIRE_FALSE(restored->storage->retiring());
  REQUIRE(restored->managed_identity == old_identity);
  REQUIRE(restored->volume == content.Volume("hdd"));
  REQUIRE(old_view->retiring());
  REQUIRE(ReadText(marker) == marker_before);
  REQUIRE(ReadText(scratch.root / "hdd/save.bin") == "must remain");
  REQUIRE_FALSE(content.CapturePendingManagedFormat("hdd", error));
}
TEST_CASE("Managed format cancellation restores data without reviving old views",
          "[content][catalog][managed_format]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  std::ofstream(scratch.root / "hdd/save.bin") << "cancel preserves";
  const auto marker = scratch.root / "hdd" / filesystem::ManagedDeviceIdentity::kMarker;
  const auto marker_before = ReadText(marker);
  auto first = content.sources().Source("hdd");
  REQUIRE(first);
  const auto old_view = first->storage;
  const auto identity = first->managed_identity;
  const auto generation = first->generation;
  first.reset();
  std::error_code error;
  auto format = content.BeginIdleManagedFormat("hdd", error);
  REQUIRE(format);
  REQUIRE_FALSE(error);
  // Releasing a frontend handle does not lose the owned pending handoff.
  format.reset();
  REQUIRE_FALSE(content.BeginIdleManagedFormat("hdd", error));
  format = content.CapturePendingManagedFormat("hdd", error);
  REQUIRE(format);
  REQUIRE_FALSE(error);
  format->Cancel();
  const auto canceled = format->Execute();
  REQUIRE(canceled.error == std::errc::operation_canceled);
  REQUIRE_FALSE(canceled.epoch_published);
  REQUIRE_FALSE(canceled.data_started);
  auto restored = content.sources().Source("hdd");
  REQUIRE(restored);
  REQUIRE(restored->available);
  REQUIRE(restored->generation > generation);
  REQUIRE(restored->managed_identity == identity);
  REQUIRE(restored->storage != old_view);
  REQUIRE_FALSE(restored->storage->retiring());
  REQUIRE(old_view->retiring());
  REQUIRE_FALSE(identity->CheckCurrent());
  REQUIRE(ReadText(marker) == marker_before);
  REQUIRE(ReadText(scratch.root / "hdd/save.bin") == "cancel preserves");
}
TEST_CASE("Managed idle format publishes one coherent fresh owner and generation",
          "[content][catalog][managed_format]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  std::ofstream(scratch.root / "hdd/save.bin") << "disposable";
  auto first = content.sources().Source("hdd");
  REQUIRE(first);
  const auto old_view = first->storage;
  const auto old_identity = first->managed_identity;
  const auto old_epoch = old_identity->content_epoch();
  const auto uuid = old_identity->uuid();
  const auto generation = first->generation;
  first.reset();
  std::error_code error;
  auto format = content.BeginIdleManagedFormat("hdd", error);
  REQUIRE(format);
  REQUIRE_FALSE(error);
  const auto receipt = format->Execute();
  REQUIRE_FALSE(receipt.error);
  REQUIRE(receipt.complete);
  REQUIRE(receipt.current);
  auto next = content.sources().Source("hdd");
  REQUIRE(next);
  REQUIRE(next->available);
  REQUIRE(next->profile_writable);
  REQUIRE(next->format_eligible);
  REQUIRE(next->generation > generation);
  REQUIRE(next->storage != old_view);
  REQUIRE(next->managed_identity == receipt.current);
  REQUIRE(next->managed_identity->uuid() == uuid);
  REQUIRE(next->managed_identity->content_epoch() == old_epoch + 1);
  REQUIRE(next->volume == content.Volume("hdd"));
  REQUIRE(next->volume->intents()->managed_identity() == receipt.current);
  REQUIRE(next->volume->intents()->source() == next->volume->content());
  REQUIRE(old_view->retiring());
  REQUIRE(old_identity->retiring());
  REQUIRE_FALSE(receipt.current->CheckCurrent());
  REQUIRE_FALSE(std::filesystem::exists(scratch.root / "hdd/save.bin"));
  REQUIRE_FALSE(content.CapturePendingManagedFormat("hdd", error));
}
TEST_CASE("Managed format respects actual open file lease before any epoch change",
          "[content][catalog][managed_format]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  std::ofstream(scratch.root / "hdd/save.bin") << "leased bytes";
  const auto marker = scratch.root / "hdd" / filesystem::ManagedDeviceIdentity::kMarker;
  const auto marker_before = ReadText(marker);
  auto first = content.sources().Source("hdd");
  REQUIRE(first);
  std::error_code error;
  auto file = first->storage->OpenFile("save.bin", filesystem::FileAccess::kFileReadData,
                                       error);
  REQUIRE(file);
  REQUIRE_FALSE(error);
  const auto old_epoch = first->managed_identity->content_epoch();
  first.reset();
  auto format = content.BeginIdleManagedFormat("hdd", error);
  REQUIRE(format);
  REQUIRE_FALSE(error);
  const auto refused = format->Execute();
  REQUIRE(refused.error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(refused.epoch_published);
  REQUIRE_FALSE(refused.data_started);
  REQUIRE(ReadText(marker) == marker_before);
  REQUIRE(ReadText(scratch.root / "hdd/save.bin") == "leased bytes");
  auto restored = content.sources().Source("hdd");
  REQUIRE(restored);
  REQUIRE(restored->available);
  REQUIRE(restored->managed_identity->content_epoch() == old_epoch);
  restored.reset();
  file.reset();
  format.reset();
  auto retry = content.BeginIdleManagedFormat("hdd", error);
  REQUIRE(retry);
  REQUIRE_FALSE(error);
  const auto completed = retry->Execute();
  REQUIRE_FALSE(completed.error);
  REQUIRE(completed.complete);
  REQUIRE(completed.current->content_epoch() == old_epoch + 1);
}
TEST_CASE("Managed manager restart requires explicit actual-phase recovery",
          "[content][catalog][managed_format][recovery]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  std::unique_ptr<filesystem::ManagedVolumeFormatHandoff> ticket;
  filesystem::ManagedDeviceIdentity::Uuid uuid{};
  uint64_t epoch = 0;
  {
    ContentManager content(storage.context, scratch.root / "hdd");
    storage.RequireWritable(content);
    auto owner = content.Volume("hdd");
    REQUIRE(owner);
    uuid = owner->identity()->uuid();
    epoch = owner->identity()->content_epoch();
    std::ofstream(scratch.root / "hdd/save.bin") << "before actual reset";
    ticket = owner->CaptureFormatHandoff();
    REQUIRE_FALSE(ticket->error());
    ticket->ClosePublishedPublicView();
  }  // Actual manager, registry and MVA references drain.
  const auto reset = filesystem::ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE_FALSE(reset.error);
  REQUIRE(reset.complete);
  REQUIRE_FALSE(reset.current);
  REQUIRE(ticket->phase() == 1);
  ticket.reset();  // Native metadata and description close as on process loss.
  std::ofstream(scratch.root / "hdd/after-reset.bin") << "must not reset twice";
  const auto marker = scratch.root / "hdd" / filesystem::ManagedDeviceIdentity::kMarker;
  const auto pending_marker = ReadText(marker);
  ContentManager restarted(storage.context, scratch.root / "hdd");
  REQUIRE_FALSE(restarted.managed_sources_ready());
  REQUIRE_FALSE(restarted.sources().Source("hdd"));
  REQUIRE(ReadText(marker) == pending_marker);
  REQUIRE(ReadText(scratch.root / "hdd/after-reset.bin") == "must not reset twice");
  // Actual externally retained failed-bootstrap owner prevents independent EX
  // drainage. Releasing it permits explicit reinspection of the same record.
  auto failed_bootstrap = restarted.Volume("hdd");
  REQUIRE(failed_bootstrap);
  REQUIRE(failed_bootstrap->error() == std::errc::operation_in_progress);
  std::error_code error;
  auto recovery = restarted.InspectManagedFormatRecovery("hdd", error);
  REQUIRE(recovery);
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(restarted.managed_sources_ready());
  REQUIRE(ReadText(marker) == pending_marker);
  failed_bootstrap.reset();
  REQUIRE_FALSE(recovery->Resume());
  REQUIRE(restarted.managed_sources_ready());
  auto next = restarted.sources().Source("hdd");
  REQUIRE(next);
  REQUIRE(next->available);
  REQUIRE(next->generation == 1);
  REQUIRE(next->profile_writable);
  REQUIRE(next->managed_identity->uuid() == uuid);
  REQUIRE(next->managed_identity->content_epoch() == epoch + 1);
  REQUIRE(next->volume == restarted.Volume("hdd"));
  REQUIRE(next->volume->intents()->managed_identity() == next->managed_identity);
  REQUIRE(ReadText(scratch.root / "hdd/after-reset.bin") == "must not reset twice");
  REQUIRE_FALSE(std::filesystem::exists(scratch.root / "hdd/save.bin"));
  const auto completed = recovery->Execute();
  REQUIRE_FALSE(completed.error);
  REQUIRE(completed.complete);
  REQUIRE(completed.epoch_observed);
  REQUIRE(completed.current == next->managed_identity);
}

TEST_CASE("Managed source proofs stay distinct and stable in public snapshots",
          "[content][catalog][managed_format][source_proof]") {
  CatalogScratch scratch;
  const auto usb = scratch.root / "usb";
  REQUIRE(std::filesystem::create_directory(usb));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  REQUIRE(content.RegisterManagedSource(
      {"usb", 3, DeviceType::USBMASS, ContentSourceKind::kManaged, usb,
       "\\USBContent", u"USB", false, 0, "\\USB", {}, scratch.root}));
  storage.RequireWritable(content, "usb");
  const auto hdd = content.sources().Source("hdd");
  const auto removable = content.sources().Source("usb");
  REQUIRE(hdd);
  REQUIRE(removable);
  REQUIRE(hdd->format_eligible);
  REQUIRE(removable->format_eligible);
  const auto hdd_proof = hdd->managed_format_proof;
  const auto usb_proof = removable->managed_format_proof;
  REQUIRE(hdd_proof);
  REQUIRE(usb_proof);
  REQUIRE(hdd_proof != usb_proof);
  REQUIRE(hdd_proof->source_id() == "hdd");
  REQUIRE(usb_proof->source_id() == "usb");
  REQUIRE(hdd_proof->generation() == hdd->generation);
  REQUIRE(usb_proof->generation() == removable->generation);
  for (unsigned refresh = 0; refresh != 3; ++refresh) {
    const auto hdd_again = content.sources().Source("hdd");
    const auto usb_again = content.sources().Source("usb");
    REQUIRE(hdd_again);
    REQUIRE(usb_again);
    REQUIRE(hdd_again->managed_format_proof == hdd_proof);
    REQUIRE(usb_again->managed_format_proof == usb_proof);
    REQUIRE(hdd_again->generation == hdd_proof->generation());
    REQUIRE(usb_again->generation == usb_proof->generation());
    REQUIRE(hdd_again->managed_identity == hdd->managed_identity);
    REQUIRE(usb_again->managed_identity == removable->managed_identity);
  }
  REQUIRE(hdd_proof->current());
  REQUIRE(usb_proof->current());
  auto connection = content.sources().storage_notifications()->Connect(
      [](NativeContentStorageSnapshot) {});
  REQUIRE(connection);
  connection->subscription->BeginStop();
  REQUIRE(connection->subscription->Drain());
  const auto& devices = connection->initial.devices;
  const auto published_hdd = std::find_if(devices.begin(), devices.end(),
      [](const auto& device) { return device.identity == "hdd"; });
  const auto published_usb = std::find_if(devices.begin(), devices.end(),
      [](const auto& device) { return device.identity == "usb"; });
  REQUIRE(published_hdd != devices.end());
  REQUIRE(published_usb != devices.end());
  REQUIRE(published_hdd->available);
  REQUIRE(published_usb->available);
  REQUIRE(published_hdd->managed_format_proof == hdd_proof);
  REQUIRE(published_usb->managed_format_proof == usb_proof);
  REQUIRE(published_hdd->generation == hdd_proof->generation());
  REQUIRE(published_usb->generation == usb_proof->generation());
}

TEST_CASE("Readonly erroneous and unowned sources have no managed format proof",
          "[content][catalog][managed_format][source_proof]") {
  CatalogScratch scratch;
  const auto host = scratch.root / "host";
  REQUIRE(std::filesystem::create_directory(host));
  ContentSourceRegistry references(ContentSourceRegistry::Access::kReadOnlyCatalog);
  std::error_code error;
  auto installed_root = filesystem::HostStorageRoot::Open(
      scratch.root / "system", true, error);
  REQUIRE(installed_root);
  REQUIRE_FALSE(error);
  REQUIRE(references.RegisterReadOnly(
      {"installed", 7, DeviceType::HDD, ContentSourceKind::kInstalledSystem,
       scratch.root / "system", "\\SystemContent", u"Installed", true, 0},
      installed_root, 1));
  REQUIRE(references.Register(
      {"host", 8, DeviceType::ODD, ContentSourceKind::kHostMedia,
       host, "\\HostContent", u"Host", true, 0}));
  for (const char* id : {"installed", "host"}) {
    const auto source = references.Source(id);
    REQUIRE(source);
    REQUIRE(source->available);
    REQUIRE(source->source.read_only);
    REQUIRE_FALSE(source->format_eligible);
    REQUIRE_FALSE(source->managed_format_proof);
  }
  ContentSourceRegistry absent;
  const auto missing_path = scratch.root / "missing";
  REQUIRE(absent.Register(
      {"missing", 9, DeviceType::USBMASS, ContentSourceKind::kManaged,
       missing_path, "\\MissingContent", u"Missing", false, 0, "\\Missing"}));
  const auto missing = absent.Source("missing");
  REQUIRE(missing);
  REQUIRE_FALSE(missing->available);
  REQUIRE_FALSE(missing->storage);
  REQUIRE_FALSE(missing->format_eligible);
  REQUIRE_FALSE(missing->managed_format_proof);
  REQUIRE_FALSE(std::filesystem::exists(missing_path));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto actual = content.sources().Source("hdd");
  REQUIRE(actual);
  REQUIRE(actual->managed_format_proof);
  // A real current managed identity alone does not confer the coupled MVA's
  // format capability. No readiness flag or identity is manufactured here.
  ContentSourceRegistry identity_only;
  REQUIRE(identity_only.RegisterManaged(actual->source, actual->managed_identity));
  const auto unowned = identity_only.Source("hdd");
  REQUIRE(unowned);
  REQUIRE(unowned->available);
  REQUIRE(unowned->managed_identity == actual->managed_identity);
  REQUIRE_FALSE(unowned->volume);
  REQUIRE_FALSE(unowned->format_eligible);
  REQUIRE_FALSE(unowned->managed_format_proof);
  REQUIRE_FALSE(content.BeginIdleManagedFormat(
      std::shared_ptr<const ManagedFormatSourceProof>{}, error));
  REQUIRE(error);
  REQUIRE(actual->managed_format_proof->current());
  REQUIRE_FALSE(actual->storage->retiring());
}

TEST_CASE("Foreign and retired proofs refuse before retiring another source",
          "[content][catalog][managed_format][source_proof]") {
  CatalogScratch scratch;
  const auto usb = scratch.root / "usb";
  REQUIRE(std::filesystem::create_directory(usb));
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  REQUIRE(content.RegisterManagedSource(
      {"usb", 3, DeviceType::USBMASS, ContentSourceKind::kManaged, usb,
       "\\USBContent", u"USB", false, 0, "\\USB", {}, scratch.root}));
  storage.RequireWritable(content, "usb");
  const auto hdd = content.sources().Source("hdd");
  const auto removable = content.sources().Source("usb");
  REQUIRE(hdd);
  REQUIRE(removable);
  const auto hdd_proof = hdd->managed_format_proof;
  const auto usb_proof = removable->managed_format_proof;
  REQUIRE(hdd_proof);
  REQUIRE(usb_proof);
  REQUIRE(hdd_proof->current());
  CatalogScratch foreign_scratch;
  ManagedCatalogStorage foreign_storage(foreign_scratch);
  ContentManager foreign(foreign_storage.context, foreign_scratch.root / "hdd");
  foreign_storage.RequireWritable(foreign);
  const auto foreign_before = foreign.sources().Source("hdd");
  REQUIRE(foreign_before);
  REQUIRE(foreign_before->managed_format_proof);
  const auto foreign_marker = foreign_scratch.root / "hdd" /
                              filesystem::ManagedDeviceIdentity::kMarker;
  const auto foreign_marker_before = ReadText(foreign_marker);
  std::ofstream(foreign_scratch.root / "hdd/fresh-sentinel") << "foreign stays fresh";
  std::error_code error;
  // The source ID matches. Its actual registry/identity/volume do not.
  REQUIRE_FALSE(foreign.BeginIdleManagedFormat(hdd_proof, error));
  REQUIRE(error);
  const auto foreign_after = foreign.sources().Source("hdd");
  REQUIRE(foreign_after);
  REQUIRE(foreign_after->generation == foreign_before->generation);
  REQUIRE(foreign_after->storage == foreign_before->storage);
  REQUIRE(foreign_after->managed_format_proof == foreign_before->managed_format_proof);
  REQUIRE_FALSE(foreign_before->storage->retiring());
  REQUIRE(foreign_after->managed_format_proof->current());
  REQUIRE(ReadText(foreign_marker) == foreign_marker_before);
  REQUIRE(ReadText(foreign_scratch.root / "hdd/fresh-sentinel") == "foreign stays fresh");
  REQUIRE(hdd_proof->current());
  const auto usb_marker = usb / filesystem::ManagedDeviceIdentity::kMarker;
  const auto usb_marker_before = ReadText(usb_marker);
  std::ofstream(usb / "fresh-sentinel") << "USB stays fresh";
  hdd->storage->Retire();
  REQUIRE_FALSE(hdd_proof->current());
  REQUIRE(usb_proof->current());
  REQUIRE_FALSE(content.BeginIdleManagedFormat(hdd_proof, error));
  REQUIRE(error);
  const auto usb_after = content.sources().Source("usb");
  REQUIRE(usb_after);
  REQUIRE(usb_after->available);
  REQUIRE(usb_after->generation == removable->generation);
  REQUIRE(usb_after->storage == removable->storage);
  REQUIRE(usb_after->volume == removable->volume);
  REQUIRE(usb_after->managed_format_proof == usb_proof);
  REQUIRE_FALSE(removable->storage->retiring());
  REQUIRE_FALSE(removable->managed_identity->retiring());
  REQUIRE(usb_proof->current());
  REQUIRE(ReadText(usb_marker) == usb_marker_before);
  REQUIRE(ReadText(usb / "fresh-sentinel") == "USB stays fresh");
  REQUIRE_FALSE(content.CapturePendingManagedFormat("hdd", error));
  REQUIRE_FALSE(content.CapturePendingManagedFormat("usb", error));
}

TEST_CASE("Held proofs expire with their registry and do not retain volume owners",
          "[content][catalog][managed_format][source_proof]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  std::shared_ptr<const ManagedFormatSourceProof> manager_proof, registry_proof;
  std::weak_ptr<const filesystem::ManagedVolumeAuthority> owner;
  std::weak_ptr<const filesystem::ManagedDeviceIdentity> identity;
  {
    ContentManager content(storage.context, scratch.root / "hdd");
    storage.RequireWritable(content);
    const auto source = content.sources().Source("hdd");
    REQUIRE(source);
    manager_proof = source->managed_format_proof;
    REQUIRE(manager_proof);
    REQUIRE(manager_proof->current());
    owner = source->volume;
    identity = source->managed_identity;
    {
      ContentSourceRegistry registry;
      REQUIRE(registry.RegisterManaged(
          source->source, source->managed_identity, source->volume));
      const auto registered = registry.Source("hdd");
      REQUIRE(registered);
      registry_proof = registered->managed_format_proof;
      REQUIRE(registry_proof);
      REQUIRE(registry_proof != manager_proof);
      REQUIRE(registry_proof->current());
    }
    REQUIRE_FALSE(registry_proof->current());
    REQUIRE(manager_proof->current());
    REQUIRE_FALSE(owner.expired());
    REQUIRE_FALSE(identity.expired());
  }
  REQUIRE_FALSE(manager_proof->current());
  REQUIRE_FALSE(registry_proof->current());
  // If a held proof retained registry State or its MVA, these public weak
  // owner observations would still be live after both actual registries die.
  REQUIRE(owner.expired());
  REQUIRE(identity.expired());
}

TEST_CASE("An exact held source proof formats without retaining the old owner",
          "[content][catalog][managed_format][source_proof]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  std::ofstream(scratch.root / "hdd/old-save") << "old generation";
  auto source = content.sources().Source("hdd");
  REQUIRE(source);
  const auto proof = source->managed_format_proof;
  REQUIRE(proof);
  REQUIRE(proof->current());
  const auto old_view = source->storage;
  const auto old_identity = source->managed_identity;
  const auto old_epoch = old_identity->content_epoch();
  const auto old_generation = source->generation;
  std::weak_ptr<const filesystem::ManagedVolumeAuthority> old_owner = source->volume;
  // Keep the opaque proof, not SourceState's strong MVA. No polling producer
  // is started here: cached current() may truthfully report BUSY during refresh.
  source.reset();
  std::error_code error;
  auto operation = content.BeginIdleManagedFormat(proof, error);
  REQUIRE(operation);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(proof->current());
  REQUIRE(old_view->retiring());
  REQUIRE(old_owner.expired());
  const auto completed = operation->Execute();
  REQUIRE_FALSE(completed.error);
  REQUIRE(completed.complete);
  REQUIRE(completed.current);
  REQUIRE(completed.current->content_epoch() == old_epoch + 1);
  REQUIRE_FALSE(std::filesystem::exists(scratch.root / "hdd/old-save"));
  REQUIRE_FALSE(proof->current());
  const auto fresh = content.sources().Source("hdd");
  REQUIRE(fresh);
  REQUIRE(fresh->available);
  REQUIRE(fresh->managed_format_proof);
  REQUIRE(fresh->managed_format_proof != proof);
  REQUIRE(fresh->generation > old_generation);
  REQUIRE(fresh->managed_format_proof->generation() == fresh->generation);
  REQUIRE(fresh->managed_identity == completed.current);
  REQUIRE(fresh->managed_format_proof->current());
  REQUIRE_FALSE(fresh->storage->retiring());
  const auto marker = scratch.root / "hdd" / filesystem::ManagedDeviceIdentity::kMarker;
  const auto marker_before = ReadText(marker);
  std::ofstream(scratch.root / "hdd/fresh-sentinel") << "new generation stays fresh";
  REQUIRE_FALSE(content.BeginIdleManagedFormat(proof, error));
  REQUIRE(error);
  const auto unchanged = content.sources().Source("hdd");
  REQUIRE(unchanged);
  REQUIRE(unchanged->generation == fresh->generation);
  REQUIRE(unchanged->storage == fresh->storage);
  REQUIRE(unchanged->managed_identity == completed.current);
  REQUIRE(unchanged->managed_format_proof == fresh->managed_format_proof);
  REQUIRE(unchanged->managed_format_proof->current());
  REQUIRE_FALSE(unchanged->storage->retiring());
  REQUIRE(ReadText(marker) == marker_before);
  REQUIRE(ReadText(scratch.root / "hdd/fresh-sentinel") == "new generation stays fresh");
  REQUIRE_FALSE(content.CapturePendingManagedFormat("hdd", error));
}

namespace {
using Dash = xe::kernel::test::NativeManagedFormatFrontdoorTestAccess;
// These exercise the actual production lifetime protocol with real registry
// capabilities. The fabricated frame metadata does NOT assert native code,
// user input, a guest Task, format scene or frontend execution occurred.
struct DashLineageFixture {
  Dash::Lineage lineage;
  Dash::Frame frame{1, 2, 0x8000, 0x100000, 0x922FA5E0, 3};
  Dash::Raw raw{};
  explicit DashLineageFixture(uint32_t id = 1) {
    xe::store_and_swap<uint32_t>(raw.data(), id);
    xe::store_and_swap<uint32_t>(raw.data()+4, 1u);
  }
  void Producer(std::shared_ptr<const ManagedFormatSourceProof> proof,
                uint32_t object = 0x200000) {
    const auto retained = proof;
    auto producer = frame;
    REQUIRE(lineage.Enter(producer, 0x92264450, {}, {}));
    auto delivered = frame; delivered.sp -= 0xF0; delivered.lr = 0x922645C4;
    lineage.Deliver(delivered, 0x1234, delivered.sp + 0x60, raw, std::move(proof), 1);
    auto ctor = delivered; ctor.lr = 0x92264570;
    const bool entered = lineage.Enter(ctor, 0x92264280,
        {object, delivered.sp + 0x60, 0, 0, 0}, {raw});
    if (entered) {
      auto refresh = ctor; refresh.sp -= 0xC0; refresh.lr = 0x92264264;
      REQUIRE(RefreshAt(refresh, object, raw, raw, raw, retained));
      lineage.Leave(ctor, 0x92264280, object, {raw});
    }
    lineage.Leave(producer, 0x92264450, 0, {});
  }
  bool RefreshAt(Dash::Frame refresh, uint32_t object,
      const Dash::Raw& before, const Dash::Raw& produced, const Dash::Raw& returned,
      std::shared_ptr<const ManagedFormatSourceProof> proof, Dash::Geometry geometry = {}) {
    if (!lineage.Enter(refresh, 0x92263BF8, {object}, {before})) return false;
    auto body = refresh; body.sp -= 0x90; body.lr = 0x92263C2C;
    const bool received = lineage.Localized(body, object + 0x100, produced,
                                            std::move(proof), 1, geometry);
    lineage.Leave(refresh, 0x92263BF8, 0, {returned});
    return received;
  }
  bool Select(uint32_t object = 0x200000, uint32_t clone = 0x210000) {
    const bool selected = lineage.Enter(frame, 0x922F9B08, {0x300000, object}, {raw});
    REQUIRE(selected);
    const bool copied = lineage.Enter(frame, 0x922F73B8, {clone, object}, {raw});
    if (copied) lineage.Leave(frame, 0x922F73B8, clone, {raw});
    lineage.Leave(frame, 0x922F9B08, 0, {raw, 0, 0, 0, clone});
    return copied;
  }
  bool Scene(uint32_t object = 0x400000, uint32_t hxui = 0x500000) {
    auto f = frame;
    const bool entered = lineage.Enter(f, 0x922F7930, {hxui, 0xA000},
                                       {raw, 0, 0, 0x300000, 0x210000});
    if (entered) lineage.Leave(f, 0x922F7930, 0, {{}, object, hxui});
    return entered;
  }
  bool Transfer(uint32_t progress = 0x600000) {
    auto loader = frame; loader.lr = 0x923083BC;
    const bool entered = lineage.Enter(loader, 0x922F58D0,
        {0x500000, 0x9202E0CC, 0, 0, 0}, {});
    if (!entered) return false;
    auto factory = frame; factory.sp -= 0x200;
    REQUIRE(lineage.Enter(factory, 0x922F78B0, {0x700000, 0xB000}, {}));
    lineage.Leave(factory, 0x922F78B0, 0, {{}, progress, 0x700000});
    // Real loader's returned HXUI must be that exact nested factory result.
    lineage.Leave(loader, 0x922F58D0, 0, {{}, 0, 0x700000});
    return bool(lineage.Progress(progress));
  }
};
}
TEST_CASE("Dash confirmation keeps its original proof after numeric device reuse",
          "[content][catalog][managed_format][dash_lineage]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  auto old = content.sources().Source("hdd");
  REQUIRE(old); const auto proof = old->managed_format_proof;
  REQUIRE(proof); old.reset();
  DashLineageFixture frontend;
  frontend.Producer(proof);
  REQUIRE(frontend.Select());
  REQUIRE(frontend.Scene());
  std::error_code error;
  auto format = content.BeginIdleManagedFormat(proof, error);
  REQUIRE(format); REQUIRE_FALSE(error);
  REQUIRE(format->Execute().complete);
  auto fresh = content.sources().Source("hdd");
  REQUIRE(fresh); const auto replacement = fresh->managed_format_proof;
  REQUIRE(replacement); REQUIRE(replacement != proof); fresh.reset();
  std::ofstream(scratch.root / "hdd/keep-new-generation") << "new-save";
  const auto before = ReadText(scratch.root / "hdd/keep-new-generation");
  // Current list and selected wrapper have changed, but the open FormatHD
  // scene retains the original proof instead of consulting global+304 again.
  frontend.Producer(replacement);
  REQUIRE(frontend.Select());
  REQUIRE(frontend.Transfer());
  const auto selected = frontend.lineage.Progress(0x600000);
  REQUIRE(selected); REQUIRE(selected->proof == proof);
  REQUIRE_FALSE(content.BeginIdleManagedFormat(selected->proof, error));
  REQUIRE(error); REQUIRE(replacement->current());
  REQUIRE(ReadText(scratch.root / "hdd/keep-new-generation") == before);
  REQUIRE_FALSE(frontend.Transfer(0x610000));
}
TEST_CASE("Dash copied records cannot substitute for an owned enumeration delivery",
          "[content][catalog][managed_format][dash_lineage]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto source = content.sources().Source("hdd");
  REQUIRE(source); REQUIRE(source->managed_format_proof);
  DashLineageFixture forged;
  forged.Producer({});
  REQUIRE_FALSE(forged.Select());
  REQUIRE_FALSE(forged.Scene());
  REQUIRE_FALSE(forged.Transfer());
  DashLineageFixture actual;
  actual.Producer(source->managed_format_proof);
  REQUIRE(actual.Select()); REQUIRE(actual.Scene()); REQUIRE(actual.Transfer());
  auto row = actual.lineage.Progress(0x600000);
  REQUIRE(row); REQUIRE(row->proof == source->managed_format_proof);
  // Exact observed delete/scene destruction revokes this creation serial.
  actual.lineage.Enter(actual.frame, 0x922F6398, {0x600000}, {});
  REQUIRE_FALSE(actual.lineage.Progress(0x600000));
  // Reusing its numeric address or copying bytes does not recreate the proof.
  actual.lineage.Leave(actual.frame, 0x922F78B0, 0, {{}, 0x600000, 0x700000});
  REQUIRE_FALSE(actual.lineage.Progress(0x600000));
  DashLineageFixture overlapping;
  overlapping.Producer(source->managed_format_proof);
  REQUIRE(overlapping.lineage.Enter(overlapping.frame, 0x922F73B8,
                                    {0x210000, 0x200000}, {overlapping.raw}));
  overlapping.lineage.Enter(overlapping.frame, 0x9218D9E8, {0x210000}, {});
  overlapping.lineage.Leave(overlapping.frame, 0x922F73B8, 0x210000,
                            {overlapping.raw});
  REQUIRE_FALSE(overlapping.Select(0x210000, 0x220000));
}
TEST_CASE("Abandoned Dash factory reentry cannot pair with a later identical return",
          "[content][catalog][managed_format][dash_lineage]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto source = content.sources().Source("hdd"); REQUIRE(source);
  DashLineageFixture destroyed;
  destroyed.Producer(source->managed_format_proof);
  REQUIRE(destroyed.Select()); REQUIRE(destroyed.Scene());
  auto loader = destroyed.frame; loader.lr = 0x923083BC;
  REQUIRE(destroyed.lineage.Enter(loader, 0x922F58D0,
      {0x500000, 0x9202E0CC, 0, 0, 0}, {}));
  auto factory = destroyed.frame; factory.sp -= 0x200;
  REQUIRE(destroyed.lineage.Enter(factory, 0x922F78B0, {0x700000, 0xB000}, {}));
  destroyed.lineage.Leave(factory, 0x922F78B0, 0, {{}, 0x600000, 0x700000});
  // The factory has returned, but the loader has not published ownership.
  // Destroying that actual Progress must prevent the later loader return
  // from recreating proof for the now dead/recyclable object address.
  destroyed.lineage.Enter(destroyed.frame, 0x922F6398, {0x600000}, {});
  destroyed.lineage.Leave(loader, 0x922F58D0, 0, {{}, 0, 0x700000});
  REQUIRE_FALSE(destroyed.lineage.Progress(0x600000));
  DashLineageFixture frontend;
  frontend.Producer(source->managed_format_proof); REQUIRE(frontend.Select());
  const Dash::Snapshot selected{frontend.raw, 0, 0, 0x300000, 0x210000};
  REQUIRE(frontend.lineage.Enter(frontend.frame, 0x922F7930, {0x500000, 0xA000}, selected));
  // Same actual identity/SP/LR after an abandoned guest call is ambiguous,
  // not a unique invocation token. The production protocol closes admission.
  REQUIRE_FALSE(frontend.lineage.Enter(frontend.frame, 0x922F7930, {0x500000, 0xA000}, selected));
  frontend.lineage.Leave(frontend.frame, 0x922F7930, 0, {{}, 0x400000, 0x500000});
  REQUIRE_FALSE(frontend.Transfer());
  REQUIRE_FALSE(frontend.lineage.Progress(0x600000));
  REQUIRE(source->managed_format_proof->current());
}

TEST_CASE("Dash localized refresh accepts only proven metadata of its original source",
          "[content][catalog][managed_format][localized_receipt]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto source = content.sources().Source("hdd");
  REQUIRE(source); REQUIRE(source->managed_format_proof);
  const auto proof = source->managed_format_proof;
  DashLineageFixture frontend;
  frontend.Producer(proof);
  auto produced = frontend.raw;
  xe::store_and_swap<uint64_t>(produced.data()+8, 10000);
  xe::store_and_swap<uint64_t>(produced.data()+0x10, 7500);
  constexpr std::u16string_view localized = u"Hard Drive";
  for (size_t i = 0; i != localized.size(); ++i)
    xe::store_and_swap<uint16_t>(produced.data()+0x18+i*2, localized[i]);
  auto refresh = frontend.frame; refresh.lr = 0x92264598;
  // Real opaque registry proof, explicit display-byte model. The separate
  // registered639 ABI case checks actual serializer/localized output.
  REQUIRE(frontend.RefreshAt(refresh, 0x200000, frontend.raw, produced, produced, proof));
  frontend.raw = produced;
  REQUIRE(frontend.Select()); REQUIRE(frontend.Scene()); REQUIRE(frontend.Transfer());
  const auto selected = frontend.lineage.Progress(0x600000);
  REQUIRE(selected); REQUIRE(selected->proof == proof); REQUIRE(selected->raw == produced);
  REQUIRE(proof->current());

  DashLineageFixture cached;
  cached.Producer(proof);
  auto expected = produced;
  // Exact nativecached-geometry effects: total1000, used250 => free750.
  xe::store_and_swap<uint64_t>(expected.data()+8, 1000);
  xe::store_and_swap<uint64_t>(expected.data()+0x10, 750);
  REQUIRE(cached.RefreshAt(refresh, 0x200000, cached.raw, produced, expected, proof,
                          {true, 1000, 250}));
  cached.raw = expected;
  REQUIRE(cached.Select()); REQUIRE(cached.Scene()); REQUIRE(cached.Transfer());
  REQUIRE(cached.lineage.Progress(0x600000)->raw == expected);

  DashLineageFixture saturated;
  saturated.Producer(proof);
  auto no_free = produced;
  xe::store_and_swap<uint64_t>(no_free.data()+8, 1000);
  xe::store_and_swap<uint64_t>(no_free.data()+0x10, 0);
  REQUIRE(saturated.RefreshAt(refresh, 0x200000, saturated.raw, produced, no_free,
                             proof, {true, 1000, 1500}));
  saturated.raw = no_free;
  REQUIRE(saturated.Select()); REQUIRE(saturated.Scene()); REQUIRE(saturated.Transfer());
  REQUIRE(saturated.lineage.Progress(0x600000)->raw == no_free);

  DashLineageFixture tampered;
  tampered.Producer(proof);
  auto wrong = produced; wrong[0x4F] ^= 1;
  REQUIRE(tampered.RefreshAt(refresh, 0x200000, tampered.raw, produced, wrong, proof));
  tampered.raw = wrong;
  REQUIRE_FALSE(tampered.Select()); REQUIRE_FALSE(tampered.Scene());
  REQUIRE_FALSE(tampered.Transfer());
}
TEST_CASE("Same-ID localized replacement cannot upgrade an old Dash menu",
          "[content][catalog][managed_format][localized_receipt]") {
  CatalogScratch scratch;
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  auto source = content.sources().Source("hdd"); REQUIRE(source);
  const auto old = source->managed_format_proof; REQUIRE(old); source.reset();
  DashLineageFixture frontend;
  frontend.Producer(old); REQUIRE(frontend.Select()); REQUIRE(frontend.Scene());
  std::error_code error;
  auto format = content.BeginIdleManagedFormat(old, error);
  REQUIRE(format); REQUIRE_FALSE(error); REQUIRE(format->Execute().complete);
  auto fresh = content.sources().Source("hdd"); REQUIRE(fresh);
  const auto replacement = fresh->managed_format_proof;
  REQUIRE(replacement); REQUIRE(replacement != old); fresh.reset();
  std::ofstream(scratch.root / "hdd/do-not-reset-again") << "fresh-generation";
  auto observed = frontend.raw;  // Deliberately SAME bytes and numeric ID.
  auto refresh = frontend.frame; refresh.lr = 0x92264598;
  REQUIRE_FALSE(frontend.RefreshAt(refresh, 0x200000, frontend.raw, observed,
                                  observed, replacement));
  REQUIRE_FALSE(frontend.Select());
  REQUIRE_FALSE(frontend.Transfer());  // Old preconfirmation menu was revoked.
  REQUIRE_FALSE(frontend.lineage.Progress(0x600000));
  REQUIRE_FALSE(old->current()); REQUIRE(replacement->current());
  REQUIRE(ReadText(scratch.root / "hdd/do-not-reset-again") == "fresh-generation");
  REQUIRE_FALSE(content.BeginIdleManagedFormat(old, error));
  REQUIRE(error); REQUIRE(replacement->current());
  REQUIRE(ReadText(scratch.root / "hdd/do-not-reset-again") == "fresh-generation");
}
#endif


TEST_CASE("Managed header publication preserves license padding and unknown tail",
          "[content][catalog][mutation]") {
  CatalogScratch scratch;
  const auto package = scratch.root / "hdd/0000000000000001/FFFE07D1/00010000/profile";
  const auto headers = scratch.root / "hdd/0000000000000001/FFFE07D1/headers/00010000";
  REQUIRE(std::filesystem::create_directories(package));
  REQUIRE(std::filesystem::create_directories(headers));
  auto data = ProfileRecord(); data.set_display_name(u"Old name");
  std::vector<uint8_t> original(sizeof(data) + 12, 0xA7);
  std::memcpy(original.data(), &data, sizeof(data));
  // ABI-defined padding/alignment and appended legacy license/tail are not
  // display/name fields. Preserve all of them, including embedded zero bytes.
  for (size_t i : {size_t(0x132), size_t(0x133), size_t(0x134), size_t(0x135),
                   size_t(0x136), size_t(0x137), size_t(0x144), size_t(0x145),
                   size_t(0x146), size_t(0x147)}) { original[i] = uint8_t(i); }
  const std::array<uint8_t,12> tail{0x78,0x56,0x34,0x12,0,0x91,0,0x72,0xFF,0,4,5};
  std::copy(tail.begin(), tail.end(), original.begin() + sizeof(data));
  const auto path = headers / "profile.header";
  { std::ofstream file(path, std::ios::binary);
    REQUIRE(file.good()); file.write(reinterpret_cast<const char*>(original.data()), original.size()); }
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  data.set_display_name(u"Committed name");
  REQUIRE(content.WriteContentHeaderFile(1, data) == X_ERROR_SUCCESS);
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> actual(std::istreambuf_iterator<char>(file), {});
  REQUIRE(actual.size() == original.size());
  REQUIRE(std::equal(tail.begin(), tail.end(), actual.begin() + sizeof(data)));
  for (size_t i : {size_t(0x132), size_t(0x133), size_t(0x134), size_t(0x135),
                   size_t(0x136), size_t(0x137), size_t(0x144), size_t(0x145),
                   size_t(0x146), size_t(0x147)}) { REQUIRE(actual[i] == original[i]); }
  XCONTENT_AGGREGATE_DATA read{};
  REQUIRE(XSUCCEEDED(content.ReadContentHeaderFile(package, 1, 0xFFFE07D1,
      XContentType::kProfile, read)));
  REQUIRE(read.display_name() == u"Committed name");
  REQUIRE(read.file_name() == "profile");
  REQUIRE(read.xuid == 1); REQUIRE(read.title_id == 0xFFFE07D1);
}

TEST_CASE("Managed package create returns only with durable discoverable metadata",
          "[content][catalog][mutation]") {
  CatalogScratch scratch; ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  auto data = ProfileRecord(); data.set_display_name(u"New package");
  REQUIRE_FALSE(content.ContentExists(1, data));
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_SUCCESS);
  REQUIRE(content.ContentExists(1, data));
  const auto listed = content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile);
  REQUIRE(listed.size() == 1); REQUIRE(listed[0].display_name() == u"New package");
  REQUIRE(listed[0].file_name() == "profile");
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_ALREADY_EXISTS);
  REQUIRE(content.SetContentThumbnail(1, data, {1,0,2,0xFF}) == X_ERROR_SUCCESS);
  REQUIRE(content.SetContentThumbnail(1, data, {}) == X_ERROR_SUCCESS);
  std::vector<uint8_t> thumbnail{99};
  REQUIRE(content.GetContentThumbnail(1, data, &thumbnail) == X_ERROR_SUCCESS);
  REQUIRE(thumbnail.empty());
  SECTION("Public directory truncation preserves complete metadata and the empty thumbnail") {
    const std::filesystem::path relative = "0000000000000001/FFFE07D1/00010000/profile";
    const auto package = scratch.root / "hdd" / relative;
    const auto header = scratch.root / "hdd/0000000000000001/FFFE07D1/Headers/00010000/profile.header";
    const auto original_header = ReadText(header);
    std::error_code error;
    auto actual = content.Volume("hdd")->content()->Subroot(relative, false, error);
    REQUIRE(actual); REQUIRE_FALSE(error);
    const std::array<uint8_t,4> user_data{4, 0, 2, 255};
    REQUIRE_FALSE(actual->WriteNewFile("Account", user_data).error);
    actual.reset();
    REQUIRE(content.TruncateContent(1, data) == X_ERROR_SUCCESS);
    REQUIRE(std::filesystem::is_directory(package));
    REQUIRE_FALSE(std::filesystem::exists(package / "Account"));
    REQUIRE(std::filesystem::is_regular_file(package / vfs::kPackageThumbnailFileName));
    REQUIRE(std::filesystem::file_size(package / vfs::kPackageThumbnailFileName) == 0);
    REQUIRE(ReadText(header) == original_header);
    REQUIRE(content.ContentExists(1, data));
    const auto after = content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile);
    REQUIRE(after.size() == 1); REQUIRE(after[0].display_name() == u"New package");
  }
}

TEST_CASE("Managed metadata refuses malformed old headers without replacing bytes",
          "[content][catalog][mutation]") {
  CatalogScratch scratch;
  const auto package = scratch.root / "hdd/0000000000000001/FFFE07D1/00010000/profile";
  const auto headers = scratch.root / "hdd/0000000000000001/FFFE07D1/Headers/00010000";
  REQUIRE(std::filesystem::create_directories(package));
  REQUIRE(std::filesystem::create_directories(headers));
  const auto path = headers / "profile.header";
  const std::string original("short\0header", 12);
  { std::ofstream file(path, std::ios::binary); REQUIRE(file.good()); file.write(original.data(), original.size()); }
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  const auto data = ProfileRecord();
  REQUIRE(content.WriteContentHeaderFile(1, data) != X_ERROR_SUCCESS);
  std::ifstream file(path, std::ios::binary);
  REQUIRE(std::string(std::istreambuf_iterator<char>(file), {}) == original);
  REQUIRE(content.Volume("hdd")->mutations()->ready());
}


TEST_CASE("Managed shared and marketplace headers retain their physical XUID zero",
          "[content][catalog][mutation]") {
  CatalogScratch scratch; ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  for (bool marketplace : {false, true}) {
    auto data = ProfileRecord();
    data.content_type = marketplace ? XContentType::kMarketplaceContent : XContentType::kSavedGame;
    data.set_file_name(marketplace ? "dlc" : "shared");
    data.set_display_name(u"Common owner");
    if (!marketplace) { data.file_name_raw[0] = char(uint8_t(data.file_name_raw[0]) | kSharedContentFileNameBit); }
    REQUIRE(content.CreatePackage(1, data) == X_ERROR_SUCCESS);
    const auto leaf = marketplace ? "dlc" : "shared";
    const auto type = marketplace ? "00000002" : "00000001";
    const auto package = scratch.root / "hdd/0000000000000000/FFFE07D1" / type / leaf;
    XCONTENT_AGGREGATE_DATA read{};
    REQUIRE(XSUCCEEDED(content.ReadContentHeaderFile(package, 0, 0xFFFE07D1, data.content_type, read)));
    REQUIRE(read.xuid == 0); REQUIRE(read.file_name() == leaf);
    REQUIRE(content.ContentExists(1, data));
    // Preserve a genuine valid orphan header and recreate its missing package.
    REQUIRE(std::filesystem::remove(package));
    REQUIRE(content.CreatePackage(1, data) == X_ERROR_SUCCESS);
    REQUIRE(XSUCCEEDED(content.ReadContentHeaderFile(package, 0, 0xFFFE07D1, data.content_type, read)));
    REQUIRE(read.xuid == 0); REQUIRE(read.display_name() == u"Common owner");
  }
}

TEST_CASE("Retained publisher selection never follows a changed fallback package",
          "[content][catalog][selection]") {
  CatalogScratch scratch;
  const auto a = scratch.root / "hdd/0000000000000001/12340001/00000003/publisher";
  const auto b = scratch.root / "hdd/0000000000000001/12340002/00000003/publisher";
  const auto ah = scratch.root / "hdd/0000000000000001/12340001/Headers/00000003/publisher.header";
  const auto bh = scratch.root / "hdd/0000000000000001/12340002/Headers/00000003/publisher.header";
  auto first = ProfileRecord(); first.title_id = 0x12340001;
  first.content_type = XContentType::kPublisher; first.set_file_name("publisher");
  first.set_display_name(u"Original selected publisher");
  auto second = first; second.title_id = 0x12340002;
  second.set_display_name(u"Different fallback publisher");
  REQUIRE(std::filesystem::create_directories(a));
  REQUIRE(std::filesystem::create_directories(b));
  REQUIRE(std::filesystem::create_directories(ah.parent_path()));
  REQUIRE(std::filesystem::create_directories(bh.parent_path()));
  WriteLegacyHeader(ah, first); WriteLegacyHeader(bh, second);
  std::ofstream(a / "save") << "first data";
  std::ofstream(b / "save") << "second data";
  ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd"); storage.RequireWritable(content);
  X_RESULT result = X_ERROR_SUCCESS;
  auto selected = ContentPackageSelectionTestAccess::Capture(content, 1, first, result);
  REQUIRE(selected); REQUIRE(result == X_ERROR_SUCCESS);
  REQUIRE(selected->path == a); REQUIRE(selected->source);
  REQUIRE(ContentPackageSelectionTestAccess::Current(content, *selected));
  // The production pre-mount claim must protect the physical package even
  // though it has NOT entered open_packages_ yet. Both public mutators test it.
  const auto header_before = ReadText(ah);
  REQUIRE(content.DeleteContent(1, first) == X_ERROR_SHARING_VIOLATION);
  REQUIRE(content.TruncateContent(1, first) == X_ERROR_SHARING_VIOLATION);
  REQUIRE(ReadText(a / "save") == "first data"); REQUIRE(ReadText(ah) == header_before);
  // Emulate a noncooperating host selector change. Fresh lookup really chooses
  // B, but the already captured A becomes invalid rather than mounting B with
  // A's retained metadata/license flags or silently upgrading its source.
  std::filesystem::rename(a, scratch.root / "retained-first");
  REQUIRE_FALSE(ContentPackageSelectionTestAccess::Current(content, *selected));
  auto fresh = ContentPackageSelectionTestAccess::Capture(content, 1, first, result);
  REQUIRE(fresh); REQUIRE(result == X_ERROR_SUCCESS); REQUIRE(fresh->path == b);
  REQUIRE(ContentPackageSelectionTestAccess::Current(content, *fresh));
  REQUIRE(selected->path == a); REQUIRE(ReadText(b / "save") == "second data");
  REQUIRE(ReadText(ah) == header_before); REQUIRE(ReadText(scratch.root / "retained-first/save") == "first data");
}
TEST_CASE("Created package selection owns its exact source through pre-mount publication",
          "[content][catalog][selection]") {
  CatalogScratch scratch; ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd"); storage.RequireWritable(content);
  auto data = ProfileRecord(); data.set_display_name(u"Exact newly created package");
  X_RESULT result = X_ERROR_SUCCESS;
  auto selected = ContentPackageSelectionTestAccess::Create(content, 1, data, result);
  REQUIRE(selected); REQUIRE(result == X_ERROR_SUCCESS); REQUIRE(selected->source);
  const auto path = scratch.root / "hdd/0000000000000001/FFFE07D1/00010000/profile";
  REQUIRE(selected->path == path); REQUIRE(selected->directory);
  REQUIRE(selected->directory->path() == path);
  REQUIRE(ContentPackageSelectionTestAccess::Current(content, *selected));
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_SHARING_VIOLATION);
  REQUIRE(content.TruncateContent(1, data) == X_ERROR_SHARING_VIOLATION);
  SECTION("Releasing actual selection admits the existing mutation again") {
    selected.reset(); REQUIRE(content.TruncateContent(1, data) == X_ERROR_SUCCESS);
    REQUIRE(content.ContentExists(1, data));
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  SECTION("Library aliases claim the same canonical physical package") {
    selected.reset(); std::error_code error;
    const auto alias = scratch.root / "library-path-alias";
    std::filesystem::create_directory_symlink(path, alias, error);
    REQUIRE_FALSE(error);
    selected = ContentPackageSelectionTestAccess::LibraryAlias(content, alias, result);
    REQUIRE(selected); REQUIRE(result == X_ERROR_SUCCESS);
    REQUIRE(selected->path == path); REQUIRE(selected->library); REQUIRE(selected->read_only);
    REQUIRE(ContentPackageSelectionTestAccess::Current(content, *selected));
    REQUIRE(content.DeleteContent(1, data) == X_ERROR_SHARING_VIOLATION);
    REQUIRE(content.TruncateContent(1, data) == X_ERROR_SHARING_VIOLATION);
    REQUIRE(std::filesystem::exists(path));
  }
#endif
  SECTION("Retirement cancels the captured guest generation without changing the internal owner") {
    const auto identity = selected->source->managed_identity;
    selected->source->storage->Retire();
    REQUIRE_FALSE(ContentPackageSelectionTestAccess::Current(content, *selected));
    REQUIRE_FALSE(identity->retiring()); REQUIRE_FALSE(identity->CheckCurrent());
    REQUIRE(std::filesystem::exists(path));
  }
}
TEST_CASE("Managed content delete commits retained data Header and catalog absence",
          "[content][catalog][mutation]") {
  CatalogScratch scratch; ManagedCatalogStorage storage(scratch);
  ContentManager content(storage.context, scratch.root / "hdd");
  storage.RequireWritable(content);
  auto data = ProfileRecord(); data.set_display_name(u"Retire exact package");
  REQUIRE(content.CreatePackage(1, data) == X_ERROR_SUCCESS);
  const auto volume = content.Volume("hdd");
  const std::filesystem::path relative = "0000000000000001/FFFE07D1/00010000/profile";
  const auto package = scratch.root / "hdd" / relative;
  const auto header = scratch.root / "hdd/0000000000000001/FFFE07D1/Headers/00010000/profile.header";
  std::error_code error;
  auto actual = volume->content()->Subroot(relative, false, error);
  REQUIRE(actual); REQUIRE_FALSE(error);
  const std::array<uint8_t,5> bytes{8, 0, 6, 5, 4};
  REQUIRE_FALSE(actual->WriteNewFile("Account", bytes).error);
  const auto raw_header = ReadText(header);
  REQUIRE(content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile).size() == 1);
  SECTION("A real open member prevents deletion until the actual handle closes") {
    auto open = actual->OpenFile("Account", filesystem::FileAccess::kFileReadData, error);
    REQUIRE(open); REQUIRE_FALSE(error);
    REQUIRE(content.DeleteContent(1, data) == X_ERROR_SHARING_VIOLATION);
    REQUIRE(ReadText(header) == raw_header);
    REQUIRE(ReadText(package / "Account") == std::string(bytes.begin(), bytes.end()));
    REQUIRE(content.ContentExists(1, data));
    open.reset();
  }
  SECTION("Readonly discovery cannot acquire the managed mutation owner") {
    CatalogScratch read_only;
    const auto readonly_package = read_only.root / "hdd" / relative;
    const auto readonly_header = read_only.root /
        "hdd/0000000000000001/FFFE07D1/Headers/00010000/profile.header";
    REQUIRE(std::filesystem::create_directories(readonly_package));
    REQUIRE(std::filesystem::create_directories(readonly_header.parent_path()));
    WriteLegacyHeader(readonly_header, data);
    std::ofstream(readonly_package / "Account") << "real reference payload";
    ContentManager reference(ContentManager::ReadOnlyCatalogTag{}, read_only.root / "hdd");
    REQUIRE(reference.ContentExists(1, data));
    const auto readonly_raw = ReadText(readonly_header);
    REQUIRE(reference.DeleteContent(1, data) == X_ERROR_ACCESS_DENIED);
    REQUIRE(ReadText(readonly_header) == readonly_raw);
    REQUIRE(ReadText(readonly_package / "Account") == "real reference payload");
    REQUIRE(ReadText(header) == raw_header);
    REQUIRE(std::filesystem::exists(package / "Account"));
  }
  actual.reset();
  REQUIRE(content.DeleteContent(1, data) == X_ERROR_SUCCESS);
  REQUIRE_FALSE(std::filesystem::exists(package));
  REQUIRE_FALSE(std::filesystem::exists(header));
  REQUIRE_FALSE(content.ContentExists(1, data));
  REQUIRE(content.ListContent(1, 1, 0xFFFE07D1, XContentType::kProfile).empty());
  REQUIRE(volume->mutations()->ready());
}
TEST_CASE("Expected source resolution refuses recycled IDs without changing retained bytes",
          "[content][catalog][source_identity]") {
  CatalogScratch scratch;
  std::ofstream(scratch.root / "system/probe") << "original retained source";
  std::error_code error;
  auto first_root = filesystem::HostStorageRoot::Open(scratch.root / "system", true, error, 0);
  REQUIRE(first_root); REQUIRE_FALSE(error);
  ContentSourceRegistry registry;
  const ContentSource definition{"fixed", 7, DeviceType::HDD,
      ContentSourceKind::kInstalledSystem, scratch.root / "system", "\\SystemContent",
      u"Actual installed image", true, 0};
  REQUIRE(registry.RegisterReadOnly(definition, first_root, 1));
  const auto old = registry.Source("fixed");
  REQUIRE(old); REQUIRE(old->available);
  REQUIRE(registry.Resolve(*old, "probe", false) == scratch.root / "system/probe");
  REQUIRE_FALSE(registry.Resolve(*old, "probe", true));
  REQUIRE(std::filesystem::create_directory(scratch.root / "next-system"));
  std::ofstream(scratch.root / "next-system/probe") << "new generation same configured ID";
  std::filesystem::rename(scratch.root / "system", scratch.root / "previous-system");
  std::filesystem::rename(scratch.root / "next-system", scratch.root / "system");
  auto next_root = filesystem::HostStorageRoot::Open(scratch.root / "system", true, error, 0);
  REQUIRE(next_root); REQUIRE_FALSE(error);
  REQUIRE(registry.RegisterReadOnly(definition, next_root, 2));
  const auto current = registry.Source("fixed");
  REQUIRE(current); REQUIRE(current->available);
  REQUIRE(current->generation == 2); REQUIRE(old->storage != current->storage);
  REQUIRE_FALSE(registry.Resolve(*old, "probe", false));
  REQUIRE_FALSE(registry.Resolve(*old, "probe", true));
  REQUIRE(registry.Resolve(*current, "probe", false) == scratch.root / "system/probe");
  REQUIRE(ReadText(scratch.root / "previous-system/probe") == "original retained source");
  REQUIRE(ReadText(scratch.root / "system/probe") == "new generation same configured ID");
}
}  // namespace xe::kernel::xam::test


namespace xe::kernel::xam::test {
namespace {
struct ContentRestartRuntime {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;
  CatalogScratch scratch;
  std::unique_ptr<Emulator> emulator;
  ContentRestartRuntime() {
    REQUIRE(std::filesystem::create_directory(scratch.root / "content"));
    REQUIRE(std::filesystem::create_directory(scratch.root / "cache"));
    REQUIRE(KernelState::shared() == nullptr);
    emulator = std::make_unique<Emulator>("", scratch.root,
        scratch.root / "content", scratch.root / "cache");
    Setup();
  }
  void Setup() {
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
    REQUIRE(emulator->kernel_state()->content_manager()->managed_sources_ready());
  }
  ~ContentRestartRuntime() { emulator.reset(); }
  ContentManager& content() { return *emulator->kernel_state()->content_manager(); }
  XCONTENT_AGGREGATE_DATA Save(std::string_view leaf) {
    XCONTENT_AGGREGATE_DATA data{};
    data.device_id = 1; data.title_id = 0x12345678;
    data.content_type = XContentType::kSavedGame;
    data.set_file_name(std::string(leaf)); data.set_display_name(u"Teardown save");
    return data;
  }
};
struct RestartFile {
  vfs::File* file = nullptr;
  ~RestartFile() { if (file) { file->Destroy(); } }
  void Reset() { if (file) { file->Destroy(); file = nullptr; } }
};
void WriteRestartPayload(ContentRestartRuntime& runtime, std::string_view root,
                         std::span<const uint8_t> payload) {
  RestartFile opened; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,
      std::string(root) + ":\\marker.bin", vfs::FileDisposition::kCreate,
      vfs::FileAccess::kGenericWrite, false, true, &opened.file, &action) == X_STATUS_SUCCESS);
  REQUIRE(opened.file); size_t written = 0;
  REQUIRE(opened.file->WriteSync(payload, 0, &written) == X_STATUS_SUCCESS);
  REQUIRE(written == payload.size()); REQUIRE(opened.file->Flush() == X_STATUS_SUCCESS);
}
void ReadRestartPayload(ContentRestartRuntime& runtime, std::string_view root,
                        std::span<const uint8_t> expected) {
  RestartFile opened; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,
      std::string(root) + ":\\marker.bin", vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead, false, true, &opened.file, &action) == X_STATUS_SUCCESS);
  REQUIRE(opened.file); std::vector<uint8_t> bytes(expected.size()); size_t count = 0;
  REQUIRE(opened.file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == expected.size()); REQUIRE(std::equal(bytes.begin(), bytes.end(), expected.begin()));
}
}
TEST_CASE("Unclosed managed content packages release old HDD authority across emulator restart",
          "[kernel][content][storage][restart][lifetime]") {
  ContentRestartRuntime runtime;
  const auto first = runtime.Save("first"); const auto second = runtime.Save("second");
  const std::array<uint8_t, 5> payload{1, 0, 0x7F, 0x80, 0xFF};
  SECTION("A genuinely retained old authority remains busy until its release") {
    REQUIRE(runtime.content().CreateContent("First", 1, first) == X_ERROR_SUCCESS);
    REQUIRE(runtime.content().CreateContent("Second", 1, second) == X_ERROR_SUCCESS);
    WriteRestartPayload(runtime, "First", payload); WriteRestartPayload(runtime, "Second", payload);
    auto retained = runtime.content().Volume("hdd"); REQUIRE(retained);
    const std::weak_ptr<const filesystem::ManagedVolumeAuthority> old_volume = retained;
    runtime.emulator->Shutdown(); REQUIRE_FALSE(old_volume.expired());
    // This refusal is real source-flock exclusion, not a simulated capacity or
    // a boolean readiness test. The fix must never ignore an intentional owner.
    REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_UNSUCCESSFUL);
    REQUIRE_FALSE(runtime.content().managed_sources_ready());
    { const auto refused = runtime.content().Volume("hdd");
      REQUIRE(refused); REQUIRE(refused->error() == std::errc::device_or_resource_busy); }
    runtime.emulator->Shutdown();
    retained.reset(); REQUIRE(old_volume.expired());
    runtime.Setup(); uint32_t license = 0;
    REQUIRE(runtime.content().OpenContent("Reload", 1, first, license) == X_ERROR_SUCCESS);
    ReadRestartPayload(runtime, "Reload", payload);
  }
  SECTION("Unclosed package ownership drains on each same-Emulator restart") {
  for (unsigned iteration = 0; iteration != 3; ++iteration) {
    if (!iteration) {
      REQUIRE(runtime.content().CreateContent("First", 1, first) == X_ERROR_SUCCESS);
      REQUIRE(runtime.content().CreateContent("Second", 1, second) == X_ERROR_SUCCESS);
      WriteRestartPayload(runtime, "First", payload); WriteRestartPayload(runtime, "Second", payload);
    } else {
      uint32_t license = 0;
      REQUIRE(runtime.content().OpenContent("First", 1, first, license) == X_ERROR_SUCCESS);
      REQUIRE(runtime.content().OpenContent("Second", 1, second, license) == X_ERROR_SUCCESS);
      ReadRestartPayload(runtime, "First", payload); ReadRestartPayload(runtime, "Second", payload);
    }
    const std::weak_ptr<const filesystem::ManagedVolumeAuthority> old_volume = runtime.content().Volume("hdd");
    REQUIRE_FALSE(old_volume.expired());
    // Deliberately omit XamContentClose / ContentManager::CloseContent, as a
    // title can switch executables with roots still open. Same Emulator keeps
    // its intentional console/recovery owners through Shutdown/Setup.
    runtime.emulator->Shutdown(); REQUIRE(KernelState::shared() == nullptr);
    REQUIRE(old_volume.expired());
    runtime.Setup();
    REQUIRE(runtime.content().ContentExists(1, first));
    REQUIRE(runtime.content().ContentExists(1, second));
  }
  }
}
TEST_CASE("A caller held package File keeps its Entry alive through quiescent content teardown",
          "[kernel][content][storage][restart][lifetime]") {
  ContentRestartRuntime runtime; const auto save = runtime.Save("held");
  REQUIRE(runtime.content().CreateContent("Held", 1, save) == X_ERROR_SUCCESS);
  const std::array<uint8_t, 3> payload{7, 0, 9}; WriteRestartPayload(runtime, "Held", payload);
  RestartFile opened; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr, "Held:\\marker.bin",
      vfs::FileDisposition::kOpen, vfs::FileAccess::kGenericRead,
      false, true, &opened.file, &action) == X_STATUS_SUCCESS);
  REQUIRE(opened.file); REQUIRE(opened.file->entry());
  const auto absolute_path = opened.file->entry()->absolute_path();
  const auto old_device = opened.file->entry()->device()->weak_from_this();
  REQUIRE_FALSE(old_device.expired());
  runtime.emulator->Shutdown(); REQUIRE(KernelState::shared() == nullptr);
  // The exact native File owns the Device/tree independently of the package
  // map. Teardown cannot free its borrowed Entry or substitute another root.
  REQUIRE_FALSE(old_device.expired());
  REQUIRE(opened.file->entry()->absolute_path() == absolute_path);
  REQUIRE(opened.file->entry()->device() == old_device.lock().get());
  opened.Reset(); REQUIRE(old_device.expired());
  runtime.Setup(); REQUIRE(runtime.content().managed_sources_ready());
  uint32_t license = 0;
  REQUIRE(runtime.content().OpenContent("Reload", 1, save, license) == X_ERROR_SUCCESS);
  ReadRestartPayload(runtime, "Reload", payload);
}
}  // namespace xe::kernel::xam::test
