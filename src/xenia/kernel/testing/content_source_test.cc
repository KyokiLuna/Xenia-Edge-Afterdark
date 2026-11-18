/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include "xenia/kernel/xam/content_source.h"
#include <array>
#include <iterator>
#include <limits>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/host_storage.h"
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/media_reference_store.h"
#include "xenia/base/media_selection.h"
#include "xenia/base/testing/cache_storage_fixture.h"
#include "xenia/kernel/native_content_storage_source.h"
#include "xenia/vfs/devices/cache_metadata_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/media_reference_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>

#include "third_party/catch/include/catch.hpp"

namespace xe::kernel::xam::test {
// This friendship can pause only timing. Every observation and registration
// still performs the genuine native probe/common graph admission afterward.
class SelectedMediaTimingAccess {
 public:
  static void Set(ContentSourceRegistry& registry,
      std::function<void(const std::string&)> sampled={},
      std::function<void(const std::string&)> prepared={}) {
    registry.SetSelectedMediaTimingForTest(std::move(sampled),std::move(prepared));
  }
};
namespace {

struct Scratch {
  std::filesystem::path root;
  Scratch() {
    std::random_device rng;
    for (unsigned i = 0; i < 16; ++i) {
      auto candidate = std::filesystem::temp_directory_path() /
                       ("xenia_content_sources_" + std::to_string(rng()) + "_" +
                        std::to_string(rng()));
      if (std::filesystem::create_directory(candidate)) {
        root = std::move(candidate);
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
  }
  ~Scratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  ContentSource Managed(std::string id, uint32_t device) {
    auto path = root / id;
    std::filesystem::create_directory(path);
    return {std::move(id),
            device,
            DeviceType::HDD,
            ContentSourceKind::kManaged,
            path,
            "",
            u"Storage",
            false,
            30};
  }
};

}  // namespace

TEST_CASE("Content sources separate devices and read-only system overlays",
          "[content][storage]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  auto hdd = scratch.Managed("hdd", 1);
  auto usb = scratch.Managed("usb", 3);
  auto system = scratch.Managed("system", 1);
  system.kind = ContentSourceKind::kInstalledSystem;
  system.read_only = true;
  REQUIRE(registry.Register(hdd));
  REQUIRE(registry.Register(usb));
  REQUIRE(registry.Register(system));
  REQUIRE(registry.DeviceIds() == std::vector<uint32_t>{1, 3});
  REQUIRE(registry.Sources(1).size() == 2);
  REQUIRE(registry.Sources(3).size() == 1);
  REQUIRE(registry.WritableSource(1)->source.id == "hdd");
  REQUIRE(registry.WritableSource(3)->source.id == "usb");
  REQUIRE(registry.Source("system")->free_bytes == 0);
  REQUIRE(registry.Resolve("system", "package/file", false));
  REQUIRE_FALSE(registry.Resolve("system", "package/file", true));
  auto duplicate = scratch.Managed("second-hdd", 1);
  REQUIRE_FALSE(registry.Register(duplicate));
  REQUIRE(registry.Register(hdd));
  hdd.read_only = true;
  REQUIRE_FALSE(registry.Register(hdd));
}

TEST_CASE("A guest device cannot mix physical device types",
          "[content][storage]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  auto hdd = scratch.Managed("hdd", 1);
  hdd.guest_volume_root = "\\Device\\Harddisk0\\Partition1";
  REQUIRE(registry.Register(hdd));
  auto system = scratch.Managed("system", 1);
  system.kind = ContentSourceKind::kInstalledSystem;
  system.read_only = true;
  system.device_type = DeviceType::USBMASS;
  REQUIRE_FALSE(registry.Register(system));
  system.device_type = DeviceType::HDD;
  REQUIRE(registry.Register(system));
  REQUIRE(registry.Register(hdd));
  hdd.guest_volume_root = "\\OtherVolume";
  REQUIRE_FALSE(registry.Register(hdd));
}

TEST_CASE("FATX resolution finds every actual host component",
          "[content][storage]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  auto source = scratch.Managed("hdd", 1);
  REQUIRE(registry.Register(source));
  const auto package = source.root / "a000000000000001/fffe07df/00abcdef/Save";
  REQUIRE(std::filesystem::create_directories(package));
  const auto existing =
      registry.Resolve("hdd", "A000000000000001/FFFE07DF/00ABCDEF/save", false);
  REQUIRE(existing);
  REQUIRE(std::filesystem::equivalent(*existing, package));
  const auto future = registry.Resolve(
      "hdd", "A000000000000001/FFFE07DF/00ABCDEF/SAVE/new", true);
  REQUIRE(future);
  REQUIRE(std::filesystem::equivalent(future->parent_path(), package));
  REQUIRE(future->filename() == "new");

  const auto other = package.parent_path() / "sAvE";
  // Case-insensitive host filesystems cannot create distinct aliases.
  if (std::filesystem::create_directory(other)) {
    REQUIRE(registry.Resolve("hdd", "a000000000000001/fffe07df/00abcdef/Save",
                             false) == package);
    REQUIRE_FALSE(registry.Resolve(
        "hdd", "A000000000000001/FFFE07DF/00ABCDEF/SAVE", false));
  }
}

TEST_CASE("Device space uses available bytes and survives removal",
          "[content][storage]") {
  Scratch scratch;
  uint64_t available = 80;
  ContentSourceRegistry registry([&](const auto&) {
    return std::optional(std::filesystem::space_info{500, 100, available});
  });
  auto source = scratch.Managed("hdd", 1);
  REQUIRE(registry.Register(source));
  REQUIRE(registry.Source("hdd")->capacity_bytes == 500);
  REQUIRE(registry.Source("hdd")->free_bytes == 50);
  available = 20;
  REQUIRE(registry.Source("hdd")->free_bytes == 0);
  std::filesystem::remove(source.root);
  REQUIRE_FALSE(registry.Source("hdd")->available);
  REQUIRE_FALSE(registry.WritableSource(1));
  REQUIRE_FALSE(registry.Resolve("hdd", "save", true));
  std::filesystem::create_directory(source.root);
  available = 90;
  REQUIRE(registry.WritableSource(1)->source.id == "hdd");
  REQUIRE(registry.WritableSource(1)->free_bytes == 60);
}

TEST_CASE("Content source resolution refuses traversal and symlink escapes",
          "[content][storage]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  auto source = scratch.Managed("hdd", 1);
  REQUIRE(registry.Register(source));
  auto outside = scratch.Managed("outside", 3).root;
  std::filesystem::create_directory(source.root / "inside");
  REQUIRE(registry.Resolve("hdd", "inside/new/save", true) ==
          source.root / "inside/new/save");
  REQUIRE_FALSE(registry.Resolve("hdd", "../outside/save", true));
  REQUIRE_FALSE(registry.Resolve("hdd", outside / "save", true));
  REQUIRE_FALSE(registry.Resolve("hdd", ".", true));
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside, source.root / "escape",
                                            ec);
  if (!ec) {
    REQUIRE_FALSE(registry.Resolve("hdd", "escape/new/save", true));
    REQUIRE_FALSE(registry.Resolve("hdd", "escape/file", false));
  }
  std::filesystem::create_directory_symlink(source.root / "inside",
                                            source.root / "alias", ec);
  if (!ec) {
    REQUIRE(registry.Resolve("hdd", "alias/new/save", true) ==
            source.root / "inside/new/save");
  }
  ec.clear();
  std::filesystem::create_directory_symlink(outside / "not-created",
                                            source.root / "dangling", ec);
  if (!ec) {
    REQUIRE_FALSE(registry.Resolve("hdd", "dangling/save", true));
    REQUIRE_FALSE(registry.Resolve("hdd", "dangling/save", false));
    REQUIRE_FALSE(std::filesystem::exists(outside / "not-created"));
    std::filesystem::create_directory_symlink("dangling", source.root / "chain",
                                              ec);
    if (!ec) {
      REQUIRE_FALSE(registry.Resolve("hdd", "chain/save", true));
    }
  }
}

TEST_CASE("Captured source roots outlive their registry",
          "[content][storage]") {
  Scratch scratch;
  auto source = scratch.Managed("owned", 1);
  std::ofstream(source.root / "probe", std::ios::binary) << "owned";
  std::shared_ptr<filesystem::HostStorageRoot> retained;
  {
    ContentSourceRegistry registry;
    REQUIRE(registry.Register(source));
    const auto captured = registry.Source(source.id);
    REQUIRE(captured);
    REQUIRE(captured->available);
    REQUIRE(captured->generation != 0);
    retained = captured->storage;
  }
  REQUIRE(retained);
  std::vector<uint8_t> bytes;
  CHECK_FALSE(retained->ReadFile("probe", 16, bytes));
  CHECK(bytes == std::vector<uint8_t>{'o', 'w', 'n', 'e', 'd'});
}

TEST_CASE("Removal and replacement retire old source generations",
          "[content][storage]") {
  Scratch scratch;
  auto source = scratch.Managed("removable", 1);
  std::ofstream(source.root / "probe", std::ios::binary) << "old";
  ContentSourceRegistry registry;
  REQUIRE(registry.Register(source));
  const auto first = registry.Source(source.id);
  REQUIRE(first);
  REQUIRE(first->available);
  REQUIRE(first->storage);
  std::filesystem::rename(source.root, scratch.root / "removed");
  std::vector<uint8_t> expected{'o', 'l', 'd'};
  SECTION("observed removal and reinsertion") {
    REQUIRE_FALSE(registry.Source(source.id)->available);
    REQUIRE_FALSE(first->storage->Observe().available);
    std::filesystem::rename(scratch.root / "removed", source.root);
  }
  SECTION("replacement without an observed gap") {
    expected = {'n', 'e', 'w'};
    std::filesystem::create_directory(source.root);
    std::ofstream(source.root / "probe", std::ios::binary) << "new";
  }
  const auto next = registry.Source(source.id);
  REQUIRE(next);
  REQUIRE(next->available);
  REQUIRE(next->storage);
  CHECK(next->generation > first->generation);
  CHECK(next->storage != first->storage);
  CHECK_FALSE(first->storage->Observe().available);
  std::vector<uint8_t> bytes;
  CHECK(first->storage->ReadFile("probe", 16, bytes));
  CHECK_FALSE(next->storage->ReadFile("probe", 16, bytes));
  CHECK(bytes == expected);
}

namespace {
struct StorageSnapshots {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<NativeContentStorageSnapshot> values;
  void Add(NativeContentStorageSnapshot snapshot) {
    std::lock_guard lock(mutex);
    values.push_back(std::move(snapshot));
    changed.notify_all();
  }
  template <typename Predicate>
  bool Wait(Predicate predicate) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(2), [&] {
      return std::ranges::any_of(values, predicate);
    });
  }
  size_t Count() {
    std::lock_guard lock(mutex);
    return values.size();
  }
};
ContentSource NativeVolume(Scratch& scratch, std::string id, uint32_t device) {
  auto source = scratch.Managed(std::move(id), device);
  source.guest_volume_root = device == 1
                                 ? "\\Device\\Harddisk0\\Partition1"
                                 : "\\Device\\Mass" + std::to_string(device);
  if (device != 1) {
    source.device_type = DeviceType::USBMASS;
  }
  return source;
}
const NativeContentStorageDevice* FindVolume(
    const NativeContentStorageSnapshot& snapshot, std::string_view identity) {
  const auto found = std::ranges::find_if(
      snapshot.devices,
      [&](const auto& value) { return value.identity == identity; });
  return found == snapshot.devices.end() ? nullptr : &*found;
}
}  // namespace

TEST_CASE("Native storage projection merges overlays and excludes host media",
          "[content][storage][notifications]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  const auto hdd = NativeVolume(scratch, "hdd", 1);
  REQUIRE(registry.Register(hdd));
  auto provider = registry.storage_notifications();
  StorageSnapshots events;
  auto connection = provider->Connect(
      [&](auto snapshot) { events.Add(std::move(snapshot)); });
  REQUIRE(connection);
  REQUIRE(connection->initial.revision != 0);
  REQUIRE(connection->initial.devices.size() == 1);
  const auto first = connection->initial;
  REQUIRE(first.devices[0].identity == "hdd");
  REQUIRE(first.devices[0].generation != 0);
  REQUIRE(first.devices[0].guest_volume_root == hdd.guest_volume_root);
  REQUIRE(first.devices[0].available);

  auto installed = scratch.Managed("system", 1);
  installed.kind = ContentSourceKind::kInstalledSystem;
  installed.read_only = true;
  installed.guest_volume_root = "\\Device\\Harddisk0\\SystemExtPartition";
  REQUIRE(registry.Register(installed));
  auto media = NativeVolume(scratch, "media", 4);
  media.kind = ContentSourceKind::kHostMedia;
  media.read_only = true;
  REQUIRE(registry.Register(media));
  auto duplicate = provider->Connect([](auto) {});
  REQUIRE(duplicate);
  CHECK(duplicate->initial.revision == first.revision);
  CHECK(duplicate->initial.devices == first.devices);
  CHECK(events.Count() == 0);
  duplicate->subscription->BeginStop();
  CHECK(duplicate->subscription->Drain());
  connection->subscription->BeginStop();
  CHECK(connection->subscription->Drain());
}

TEST_CASE(
    "Storage connect cannot lose a concurrent managed volume registration",
    "[content][storage][notifications]") {
  for (unsigned iteration = 0; iteration < 8; ++iteration) {
    Scratch scratch;
    ContentSourceRegistry registry;
    REQUIRE(registry.Register(NativeVolume(scratch, "hdd", 1)));
    auto usb = NativeVolume(scratch, "usb", 3);
    StorageSnapshots events;
    auto provider = registry.storage_notifications();
    std::barrier start(2);
    std::optional<NativeContentStorageSource::Connection> connection;
    std::thread connector([&] {
      start.arrive_and_wait();
      connection = provider->Connect(
          [&](auto snapshot) { events.Add(std::move(snapshot)); });
    });
    start.arrive_and_wait();
    const bool registered = registry.Register(usb);
    connector.join();
    REQUIRE(registered);
    REQUIRE(connection);
    const bool in_initial = FindVolume(connection->initial, "usb") != nullptr;
    CHECK((in_initial || events.Wait([](const auto& value) {
      const auto* usb = FindVolume(value, "usb");
      return usb && usb->available && usb->generation != 0;
    })));
    CHECK(connection->initial.revision != 0);
    connection->subscription->BeginStop();
    CHECK(connection->subscription->Drain());
  }
}

TEST_CASE(
    "Connected storage monitor observes removal and a fresh root generation",
    "[content][storage][notifications]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  auto source = NativeVolume(scratch, "hdd", 1);
  std::ofstream(source.root / "probe", std::ios::binary) << "keep";
  REQUIRE(registry.Register(source));
  const auto captured = registry.Source("hdd");
  REQUIRE(captured);
  REQUIRE(captured->storage);
  StorageSnapshots events;
  auto connection = registry.storage_notifications()->Connect(
      [&](auto snapshot) { events.Add(std::move(snapshot)); });
  REQUIRE(connection);
  const auto revision = connection->initial.revision;
  const auto generation = captured->generation;
  std::filesystem::rename(source.root, scratch.root / "removed");
  CHECK(events.Wait([&](const auto& value) {
    const auto* volume = FindVolume(value, "hdd");
    return value.revision > revision && volume && !volume->available;
  }));
  CHECK_FALSE(captured->storage->Observe().available);
  std::filesystem::rename(scratch.root / "removed", source.root);
  CHECK(events.Wait([&](const auto& value) {
    const auto* volume = FindVolume(value, "hdd");
    return value.revision > revision && volume && volume->available &&
           volume->generation > generation;
  }));
  CHECK_FALSE(captured->storage->Observe().available);
  const auto current = registry.Source("hdd");
  REQUIRE(current);
  REQUIRE(current->storage);
  std::vector<uint8_t> bytes;
  CHECK_FALSE(current->storage->ReadFile("probe", 16, bytes));
  CHECK(bytes == std::vector<uint8_t>{'k', 'e', 'e', 'p'});
  connection->subscription->BeginStop();
  CHECK(connection->subscription->Drain());
}

TEST_CASE(
    "Storage subscription rundown waits admitted work and closes new calls",
    "[content][storage][notifications]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  REQUIRE(registry.Register(NativeVolume(scratch, "hdd", 1)));
  auto usb = NativeVolume(scratch, "usb", 3);
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  std::atomic<unsigned> calls{0};
  std::atomic<bool> reentered{false};
  auto connection = registry.storage_notifications()->Connect([&](auto) {
    ++calls;
    reentered.store(registry.DeviceIds().size() == 2 &&
                    registry.Source("hdd").has_value());
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait_for(lock, std::chrono::seconds(2), [&] { return release; });
  });
  REQUIRE(connection);
  std::thread producer([&] { registry.Register(usb); });
  bool observed;
  {
    std::unique_lock lock(mutex);
    observed = changed.wait_for(lock, std::chrono::seconds(1),
                                [&] { return entered; });
  }
  connection->subscription->BeginStop();
  auto drain = std::async(std::launch::async,
                          [&] { return connection->subscription->Drain(); });
  const bool waited = drain.wait_for(std::chrono::milliseconds(30)) ==
                      std::future_status::timeout;
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  producer.join();
  CHECK(observed);
  CHECK(waited);
  CHECK(reentered.load());
  CHECK(drain.get());
  const auto count = calls.load();
  REQUIRE(registry.Register(NativeVolume(scratch, "later", 5)));
  CHECK(calls.load() == count);
}

TEST_CASE("A storage callback cannot drain itself",
          "[content][storage][notifications]") {
  Scratch scratch;
  ContentSourceRegistry registry;
  REQUIRE(registry.Register(NativeVolume(scratch, "hdd", 1)));
  std::shared_ptr<NativeContentStorageSource::Subscription> subscription;
  std::atomic<bool> called{false}, self_result{true};
  auto connection = registry.storage_notifications()->Connect([&](auto) {
    self_result.store(subscription->Drain());
    called.store(true);
  });
  REQUIRE(connection);
  subscription = connection->subscription;
  REQUIRE(registry.Register(NativeVolume(scratch, "usb", 3)));
  CHECK(called.load());
  CHECK_FALSE(self_result.load());
  CHECK(subscription->Drain());
}

TEST_CASE(
    "Disconnected subscriptions leave the monitor dormant and provider closes "
    "with registry",
    "[content][storage][notifications]") {
  Scratch scratch;
  std::atomic<unsigned> samples{0};
  auto registry =
      std::make_unique<ContentSourceRegistry>([&](const auto& path) {
        ++samples;
        std::error_code error;
        const auto info = std::filesystem::space(path, error);
        return error ? std::nullopt : std::optional(info);
      });
  REQUIRE(registry->Register(NativeVolume(scratch, "hdd", 1)));
  auto provider = registry->storage_notifications();
  auto connection = provider->Connect([](auto) {});
  REQUIRE(connection);
  connection->subscription->BeginStop();
  REQUIRE(connection->subscription->Drain());
  // Permit a refresh already launched at stop time to leave its owned scope.
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  const auto stopped = samples.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  CHECK(samples.load() == stopped);
  registry.reset();
  CHECK_FALSE(provider->Connect([](auto) {}));
  CHECK(connection->subscription->Drain());
  ContentSourceRegistry restarted;
  REQUIRE(restarted.Register(NativeVolume(scratch, "hdd", 1)));
  auto reopened = restarted.storage_notifications()->Connect([](auto) {});
  REQUIRE(reopened);
  REQUIRE(reopened->initial.devices.size() == 1);
  CHECK(reopened->initial.devices[0].identity == "hdd");
  CHECK(reopened->initial.devices[0].available);
  CHECK(reopened->initial.devices[0].generation != 0);
  reopened->subscription->BeginStop();
  CHECK(reopened->subscription->Drain());
}

TEST_CASE("Geometry callbacks are sampled outside registry locks",
          "[content][storage][notifications]") {
  Scratch scratch;
  ContentSourceRegistry* current = nullptr;
  bool called = false;
  ContentSourceRegistry registry([&](const auto&) {
    called = true;
    // This needs the catalog lock and must be safe from the external seam.
    (void)current->DeviceIds();
    return std::optional(std::filesystem::space_info{500, 400, 300});
  });
  current = &registry;
  REQUIRE(registry.Register(NativeVolume(scratch, "hdd", 1)));
  CHECK(called);
}

TEST_CASE("Registry destruction from a sink retains admitted producer state",
          "[content][storage][notifications]") {
  Scratch scratch;
  auto registry = std::make_unique<ContentSourceRegistry>();
  REQUIRE(registry->Register(NativeVolume(scratch, "hdd", 1)));
  auto provider = registry->storage_notifications();
  auto usb = NativeVolume(scratch, "usb", 3);
  bool called = false;
  auto connection = provider->Connect([&](auto) {
    registry.reset();
    called = true;
  });
  REQUIRE(connection);
  // Register's admitted publish owns state; its wrapper may leave during the
  // callback without a self-join or a subsequent access to that wrapper.
  CHECK(registry->Register(usb));
  CHECK(called);
  CHECK_FALSE(registry);
  CHECK(connection->subscription->Drain());
  CHECK_FALSE(provider->Connect([](auto) {}));
}


#if !XE_PLATFORM_WIN32
namespace {
ContentSource SelectedSource(std::string id,uint32_t device) {
  return {std::move(id),device,DeviceType::USBMASS,ContentSourceKind::kSelectedMedia,
      {},"",u"Selected media",true,0,"\\Device\\Mass"+std::to_string(device)};
}
}
TEST_CASE("Selected file volume reads only retained selected files and revokes native owners",
          "[content][storage][selected_media]") {
  Scratch scratch;std::ofstream(scratch.root/"song")<<"abcde";
  std::ofstream(scratch.root/"sibling")<<"private sibling";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);
  REQUIRE(root);auto selected=filesystem::MediaSelection::CaptureFiles({{"Music/song.mp3",root,"song"}},error);
  REQUIRE(selected);REQUIRE_FALSE(error);REQUIRE(root->open_file_count()>0);
  REQUIRE_FALSE(selected->CaptureFolderRoot(error));
  const auto escaped_rows=selected->entries();REQUIRE(escaped_rows.size()==1);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;const auto source=SelectedSource("music",7);
  REQUIRE(registry.RegisterSelectedMedia(source,selected,1,fs));
  const auto state=registry.Source("music");REQUIRE(state);REQUIRE(state->available);
  REQUIRE_FALSE(state->storage);REQUIRE(state->source.root.empty());
  REQUIRE_FALSE(state->profile_writable);REQUIRE_FALSE(state->format_eligible);
  REQUIRE_FALSE(state->managed_format_proof);REQUIRE(state->free_bytes==0);
  REQUIRE_FALSE(registry.Resolve("music","Music/song.mp3",false));
  bool matched=false;REQUIRE_FALSE(registry.CaptureSourceDirectory(scratch.root,true,matched,error));
  REQUIRE(matched);REQUIRE(error==std::errc::permission_denied);
  auto captured=registry.CaptureSelectedMedia(*state);REQUIRE(captured==selected);
  const auto actual_device=fs.AcquireDevice(source.guest_volume_root);
  REQUIRE(actual_device);REQUIRE(actual_device->storage_volume()==registry.CaptureStorageVolume(*state));
  REQUIRE_FALSE(fs.ResolvePath(source.guest_volume_root+"\\sibling"));
  REQUIRE_FALSE(fs.ResolvePath(source.guest_volume_root+"\\..\\sibling"));
  vfs::File* file=nullptr;vfs::FileAction action;
  REQUIRE(fs.OpenFile(nullptr,source.guest_volume_root+"\\music\\SONG.mp3",vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead,false,true,&file,&action)==X_STATUS_SUCCESS);
  REQUIRE(file);std::array<uint8_t,3> bytes{};size_t count=99;
  REQUIRE(file->ReadSync(bytes,1,&count)==X_STATUS_SUCCESS);REQUIRE(count==3);
  REQUIRE(bytes==std::array<uint8_t,3>{'b','c','d'});
  auto observer=root->OpenFile("song",filesystem::FileAccess::kGenericRead,error);REQUIRE(observer);
  const auto actual_allocation=observer->QueryAllocatedBytes(error);REQUIRE(actual_allocation);
  uint64_t reported=UINT64_MAX;REQUIRE(file->QueryAllocationSize(&reported)==X_STATUS_SUCCESS);
  REQUIRE(reported==((*actual_allocation+511)/512)*512);observer.reset();
  REQUIRE(file->WriteSync(bytes,0,&count)==X_STATUS_ACCESS_DENIED);REQUIRE(count==0);
  vfs::File* writer=nullptr;
  REQUIRE(fs.OpenFile(nullptr,source.guest_volume_root+"\\Music\\song.mp3",vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericWrite,false,true,&writer,&action)==X_STATUS_ACCESS_DENIED);
  REQUIRE_FALSE(writer);
  REQUIRE(registry.WithdrawSelectedMedia("music",1));
  REQUIRE_FALSE(registry.CaptureSelectedMedia(*state));
  REQUIRE(file->ReadSync(bytes,0,&count)==X_STATUS(0xC000009D));REQUIRE(count==0);
  REQUIRE(root->open_file_count()==0);REQUIRE(escaped_rows[0].bytes==5);
  file->Destroy();fs.Clear();
  REQUIRE(std::filesystem::exists(scratch.root/"song"));
  REQUIRE(std::filesystem::file_size(scratch.root/"sibling")==15);
}
TEST_CASE("Selected sources publish actual readonly USB generations without package authority",
          "[content][storage][selected_media][notifications]") {
  Scratch scratch;std::ofstream(scratch.root/"image")<<"old";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto first=filesystem::MediaSelection::CaptureFiles({{"image.jpg",root,"image"}},error);REQUIRE(first);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("pictures",8);
  REQUIRE_FALSE(registry.Register(source));
  auto provider=registry.storage_notifications();StorageSnapshots events;
  auto connected=provider->Connect([&](auto snapshot){events.Add(std::move(snapshot));});REQUIRE(connected);
  REQUIRE(registry.RegisterSelectedMedia(source,first,1,fs));
  const auto a=registry.Source("pictures");REQUIRE(a);REQUIRE(a->available);
  auto snapshot=provider->Connect([](auto){});REQUIRE(snapshot);
  const auto* usb=FindVolume(snapshot->initial,"pictures");REQUIRE(usb);
  REQUIRE(usb->type==DeviceType::USBMASS);REQUIRE(usb->guest_volume_root==source.guest_volume_root);
  REQUIRE_FALSE(usb->managed_format_proof);REQUIRE(usb->generation==1);
  auto forged=*a;forged.source.device_id=99;REQUIRE_FALSE(registry.CaptureSelectedMedia(forged));
  // External replacement never becomes the old selection's new data.
  std::filesystem::rename(scratch.root/"image",scratch.root/"original");
  std::ofstream(scratch.root/"image")<<"new data";
  REQUIRE_FALSE(registry.Source("pictures")->available);REQUIRE_FALSE(first->current_cached());
  REQUIRE_FALSE(a->storage_volume->available_cached());
  REQUIRE_FALSE(registry.RegisterSelectedMedia(source,first,1,fs));
  auto next=filesystem::MediaSelection::CaptureFiles({{"image.jpg",root,"image"}},error);REQUIRE(next);
  REQUIRE(registry.RegisterSelectedMedia(source,next,2,fs));
  REQUIRE_FALSE(registry.WithdrawSelectedMedia("pictures",1));
  const auto b=registry.Source("pictures");REQUIRE(b);REQUIRE(b->available);REQUIRE(b->generation==2);
  std::array<uint8_t,3> bytes{};REQUIRE(registry.CaptureSelectedMedia(*b)->Read(0,0,bytes).bytes==3);
  REQUIRE(bytes==std::array<uint8_t,3>{'n','e','w'});
  REQUIRE_FALSE(registry.CaptureSelectedMedia(*a));
  REQUIRE_FALSE(registry.WritableSource(8));
  ContentSourceRegistry catalog(ContentSourceRegistry::Access::kReadOnlyCatalog);
  REQUIRE_FALSE(catalog.RegisterSelectedMedia(source,next,2,fs));
  connected->subscription->BeginStop();REQUIRE(connected->subscription->Drain());
  snapshot->subscription->BeginStop();REQUIRE(snapshot->subscription->Drain());
}

TEST_CASE("Controlled media withdrawal retains only its already published attachment until exact uses drain",
          "[content][storage][selected_media][notifications]") {
  Scratch scratch;std::ofstream(scratch.root/"song")<<"data";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto selected=filesystem::MediaSelection::CaptureFiles({{"song.mp3",root,"song"}},error);REQUIRE(selected);
  auto registry=std::make_unique<ContentSourceRegistry>();vfs::VirtualFileSystem fs;auto source=SelectedSource("controlled",6);
  REQUIRE(registry->RegisterSelectedMedia(source,selected,1,fs));
  const auto before=registry->Source("controlled");REQUIRE(before);REQUIRE(before->available);
  auto use=registry->CaptureSelectedMediaUse(*before);REQUIRE(use);REQUIRE(use->current());
  auto notifications=registry->storage_notifications();auto connection=notifications->Connect([](auto){});REQUIRE(connection);
  const auto attached=connection->initial;REQUIRE(FindVolume(attached,"controlled"));
  REQUIRE(registry->WithdrawSelectedMedia("controlled",1));REQUIRE_FALSE(use->current());
  REQUIRE_FALSE(registry->Source("controlled")->available);
  REQUIRE_FALSE(registry->CaptureSelectedMediaUse(*before));
  REQUIRE(root->open_file_count()>0);
  auto pending=notifications->Connect([](auto){});REQUIRE(pending);
  REQUIRE(pending->initial.devices==attached.devices);
  // This releases the REAL source count. It is not a fake FA Close receipt;
  // native owner tests/runtime must separately prove they release after Close.
  use.reset();REQUIRE_FALSE(registry->Source("controlled")->available);
  REQUIRE(root->open_file_count()==0);
  auto removed=notifications->Connect([](auto){});REQUIRE(removed);
  REQUIRE(FindVolume(removed->initial,"controlled"));
  REQUIRE_FALSE(FindVolume(removed->initial,"controlled")->available);
  auto replacement=filesystem::MediaSelection::CaptureFiles({{"song.mp3",root,"song"}},error);REQUIRE(replacement);
  REQUIRE(registry->RegisterSelectedMedia(source,replacement,2,fs));
  auto latest=registry->Source("controlled");REQUIRE(latest);auto escaped=registry->CaptureSelectedMediaUse(*latest);REQUIRE(escaped);
  REQUIRE_FALSE(registry->WithdrawSelectedMedia("controlled",1));REQUIRE(escaped->current());
  connection->subscription->BeginStop();REQUIRE(connection->subscription->Drain());
  pending->subscription->BeginStop();REQUIRE(pending->subscription->Drain());
  removed->subscription->BeginStop();REQUIRE(removed->subscription->Drain());
  registry.reset();REQUIRE_FALSE(escaped->current());REQUIRE(root->open_file_count()==0);
  REQUIRE(replacement->entries().size()==1);REQUIRE_FALSE(replacement->current_cached());
  REQUIRE(std::filesystem::exists(scratch.root/"song"));
}
TEST_CASE("Selected media validates exact namespaces cancellation and physical private exclusions",
          "[content][storage][selected_media]") {
  Scratch scratch;std::ofstream(scratch.root/"plain")<<"data";
  std::filesystem::create_directory(scratch.root/"vault");
  std::filesystem::permissions(scratch.root/"vault",std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
  std::ofstream(scratch.root/"vault/secret")<<"secret";
  std::filesystem::permissions(scratch.root/"vault/secret",
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto vault=filesystem::HostStorageRoot::Open(scratch.root/"vault",false,error,0);REQUIRE(vault);
  auto protected_root=vault->ProtectFlat(error);REQUIRE(protected_root);
  REQUIRE_FALSE(filesystem::MediaSelection::CaptureFiles({{"secret",root,"vault/secret"}},error));
  REQUIRE(error);
  REQUIRE_FALSE(filesystem::MediaSelection::CaptureFiles({{"../escape",root,"plain"}},error));
  REQUIRE_FALSE(filesystem::MediaSelection::CaptureFiles({{"one",root,"plain"},{"ONE",root,"plain"}},error));
  REQUIRE_FALSE(filesystem::MediaSelection::CaptureFiles({{"one",root,"plain"},{"one/two",root,"plain"}},error));
  auto selected=filesystem::MediaSelection::CaptureFiles({{"plain",root,"plain"}},error);REQUIRE(selected);
  std::atomic<bool> canceled{true};std::array<uint8_t,4> bytes{9,9,9,9};
  const auto denied=selected->Read(0,0,bytes,&canceled);
  REQUIRE(denied.error==std::errc::operation_canceled);REQUIRE(denied.bytes==0);
  REQUIRE(bytes==std::array<uint8_t,4>{9,9,9,9});
  // Folder selection cannot hide a protected descendant through a raw root.
  REQUIRE_FALSE(filesystem::MediaSelection::CaptureFolder(root,error));
  selected->Retire();REQUIRE(root->open_file_count()==0);
  REQUIRE(std::filesystem::file_size(scratch.root/"plain")==4);
}


TEST_CASE("Selected source publication preserves foreign mounts and genuine busy files",
          "[content][storage][selected_media]") {
  Scratch scratch;std::ofstream(scratch.root/"song")<<"data";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto selection=filesystem::MediaSelection::CaptureFiles({{"song.mp3",root,"song"}},error);REQUIRE(selection);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("music",5);
  SECTION("unrelated registered device cannot be displaced") {
    auto foreign=std::make_unique<vfs::HostPathDevice>(source.guest_volume_root,scratch.root,true,false,"FATX",root);
    REQUIRE(foreign->Initialize());REQUIRE(fs.RegisterDevice(std::move(foreign)));
    auto exact=fs.AcquireDevice(source.guest_volume_root);REQUIRE(exact);
    REQUIRE_FALSE(registry.RegisterSelectedMedia(source,selection,1,fs));
    REQUIRE(fs.AcquireDevice(source.guest_volume_root)==exact);REQUIRE_FALSE(registry.Source("music"));
  }
  SECTION("real open file blocks replacement without closing old source") {
    REQUIRE(registry.RegisterSelectedMedia(source,selection,1,fs));
    const auto old=registry.Source("music");REQUIRE(old);auto old_device=fs.AcquireDevice(source.guest_volume_root);REQUIRE(old_device);
    vfs::File* file=nullptr;vfs::FileAction action;
    REQUIRE(fs.OpenFile(nullptr,source.guest_volume_root+"\\song.mp3",vfs::FileDisposition::kOpen,
        vfs::FileAccess::kGenericRead,false,true,&file,&action)==X_STATUS_SUCCESS);REQUIRE(file);
    auto next=filesystem::MediaSelection::CaptureFiles({{"song.mp3",root,"song"}},error);REQUIRE(next);
    REQUIRE_FALSE(registry.RegisterSelectedMedia(source,next,2,fs));
    REQUIRE(fs.AcquireDevice(source.guest_volume_root)==old_device);
    REQUIRE(registry.CaptureSelectedMedia(*old)==selection);
    std::array<uint8_t,4> bytes{};size_t count=0;
    REQUIRE(file->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==4);
    file->Destroy();REQUIRE(registry.RegisterSelectedMedia(source,next,2,fs));
    REQUIRE(fs.AcquireDevice(source.guest_volume_root)!=old_device);
    REQUIRE_FALSE(registry.CaptureSelectedMedia(*old));
    REQUIRE_FALSE(fs.UnregisterDevice(source.guest_volume_root,old_device.get()));
    REQUIRE(registry.Source("music")->generation==2);
  }
  REQUIRE(std::filesystem::file_size(scratch.root/"song")==4);
}
TEST_CASE("Unavailable selected intent publishes no device and admits only a later genuine mount",
          "[content][storage][selected_media][persistence]") {
  Scratch scratch;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;
  auto source=SelectedSource("missing",4);REQUIRE(registry.RegisterUnavailableSelectedMedia(source));
  const auto absent=registry.Source("missing");REQUIRE(absent);REQUIRE_FALSE(absent->available);
  REQUIRE(absent->source.id=="missing");REQUIRE(absent->generation==0);
  REQUIRE_FALSE(absent->storage);REQUIRE_FALSE(absent->selected_media);REQUIRE_FALSE(absent->storage_volume);
  REQUIRE_FALSE(fs.AcquireDevice(source.guest_volume_root));
  auto provider=registry.storage_notifications();auto no_attach=provider->Connect([](auto){});REQUIRE(no_attach);
  REQUIRE(FindVolume(no_attach->initial,"missing"));REQUIRE_FALSE(FindVolume(no_attach->initial,"missing")->available);
  std::ofstream(scratch.root/"source")<<"restored";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto real=filesystem::MediaSelection::CaptureFiles({{"source",root,"source"}},error);REQUIRE(real);
  REQUIRE(registry.RegisterSelectedMedia(source,real,1,fs));
  const auto present=registry.Source("missing");REQUIRE(present);REQUIRE(present->available);
  REQUIRE(present->source.id==absent->source.id);REQUIRE(present->generation==1);
  REQUIRE(fs.AcquireDevice(source.guest_volume_root));
  REQUIRE_FALSE(registry.RegisterUnavailableSelectedMedia(source));
  no_attach->subscription->BeginStop();REQUIRE(no_attach->subscription->Drain());
}
TEST_CASE("Media reference persistence restarts exact identities and preserves missing intent and originals",
          "[content][storage][selected_media][persistence]") {
  Scratch scratch;std::filesystem::create_directory(scratch.root/"input");
  std::filesystem::create_directory(scratch.root/"metadata");
  std::filesystem::permissions(scratch.root/"metadata",std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
  std::ofstream(scratch.root/"input/song")<<"original";
  std::error_code error;auto parent=filesystem::HostStorageRoot::Open(scratch.root,false,error,0);REQUIRE(parent);
  auto recovery=filesystem::SnapshotRecoveryOwner::Open(parent);REQUIRE(recovery);REQUIRE_FALSE(recovery->error());
  auto metadata_raw=filesystem::HostStorageRoot::Open(scratch.root/"metadata",false,error,0);REQUIRE(metadata_raw);
  auto metadata=metadata_raw->ProtectFlat(error);REQUIRE(metadata);
  auto input=filesystem::HostStorageRoot::Open(scratch.root/"input",true,error,0);REQUIRE(input);
  auto selected=filesystem::MediaSelection::CaptureFiles({{"Music/song.mp3",input,"song"}},error);REQUIRE(selected);
  {
    const auto saved=filesystem::MediaReferenceStore::Save(*metadata,*recovery,"music",7,"Music",R"(\Device\Mass7)",*selected);
    REQUIRE_FALSE(saved.error);REQUIRE(saved.published);REQUIRE(saved.durable);
  }
  selected->Retire();selected.reset();
  // Reload uses only persisted intent and actual native identities; no prior
  // in-memory selection/model, temporary signin or package-shaped header.
  auto loaded=filesystem::MediaReferenceStore::Load(*metadata,"music");
  REQUIRE_FALSE(loaded.error);REQUIRE(loaded.reference);REQUIRE(loaded.selection);
  REQUIRE(loaded.reference->id=="music");REQUIRE(loaded.reference->device_id==7);
  std::array<uint8_t,3> bytes{};REQUIRE(loaded.selection->Read(0,0,bytes).bytes==3);
  REQUIRE(bytes==std::array<uint8_t,3>{'o','r','i'});
  loaded.selection->Retire();loaded.selection.reset();
  std::filesystem::rename(scratch.root/"input/song",scratch.root/"input/preserved-original");
  auto missing=filesystem::MediaReferenceStore::Load(*metadata,"music");
  REQUIRE(missing.error);REQUIRE(missing.reference);REQUIRE(missing.reference->id=="music");REQUIRE_FALSE(missing.selection);
  std::ofstream(scratch.root/"input/song")<<"replacement";
  auto replaced=filesystem::MediaReferenceStore::Load(*metadata,"music");
  REQUIRE(replaced.error==std::errc::no_such_device);REQUIRE(replaced.reference);REQUIRE_FALSE(replaced.selection);
  // Explicit reference removal deletes ONLY the private intent and keeps its
  // actual protected recovery copy. Neither original nor replacement is data
  // owned by that deletion operation.
  const auto removed=filesystem::MediaReferenceStore::Remove(*metadata,*recovery,"music");
  REQUIRE_FALSE(removed.error);REQUIRE(removed.published);REQUIRE(removed.durable);REQUIRE(removed.recovery_durable);
  REQUIRE_FALSE(filesystem::MediaReferenceStore::Load(*metadata,"music").reference);
  REQUIRE(std::filesystem::file_size(scratch.root/"input/preserved-original")==8);
  REQUIRE(std::filesystem::file_size(scratch.root/"input/song")==11);
}
TEST_CASE("Media reference CAS preserves future bytes cancellation malformed records and real space refusal",
          "[content][storage][selected_media][persistence]") {
  Scratch scratch;for(const auto name:{"input","metadata","full"})std::filesystem::create_directory(scratch.root/name);
  for(const auto name:{"metadata","full"})std::filesystem::permissions(scratch.root/name,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
  std::ofstream(scratch.root/"input/image")<<"image bytes";
  std::error_code error;auto parent=filesystem::HostStorageRoot::Open(scratch.root,false,error,0);REQUIRE(parent);
  auto recovery=filesystem::SnapshotRecoveryOwner::Open(parent);REQUIRE(recovery);REQUIRE_FALSE(recovery->error());
  auto raw=filesystem::HostStorageRoot::Open(scratch.root/"metadata",false,error,0);REQUIRE(raw);
  auto metadata=raw->ProtectFlat(error);REQUIRE(metadata);
  auto input=filesystem::HostStorageRoot::Open(scratch.root/"input",true,error,0);REQUIRE(input);
  auto selection=filesystem::MediaSelection::CaptureFiles({{"image.jpg",input,"image"}},error);REQUIRE(selection);
  const auto first=filesystem::MediaReferenceStore::Save(*metadata,*recovery,"pictures",8,"Pictures",R"(\Device\Mass8)",*selection);
  REQUIRE_FALSE(first.error);REQUIRE(first.durable);
  const auto leaf=filesystem::MediaReferenceStore::Leaf("pictures");
  auto snapshot=metadata->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(snapshot);
  std::vector<uint8_t> future(snapshot->bytes().begin(),snapshot->bytes().end()-32);
  const std::array<uint8_t,12> opaque{0,0,0,99,0,0,0,4,0xDE,0xAD,0xBE,0xEF};
  future.insert(future.end(),opaque.begin(),opaque.end());std::array<uint8_t,32> digest{};
  mbedtls_sha256(future.data(),future.size(),digest.data(),0);future.insert(future.end(),digest.begin(),digest.end());
  const auto denied=filesystem::SnapshotPublisher::Publish(*recovery,snapshot,future);
  REQUIRE(denied.error==std::errc::permission_denied);REQUIRE_FALSE(denied.published);
  const auto unchanged=metadata->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);
  REQUIRE(unchanged);REQUIRE(std::ranges::equal(unchanged->bytes(),snapshot->bytes()));
  { // A future external producer writes this isolated fixture's opaque TLV.
    std::ofstream writer(scratch.root/"metadata"/leaf,std::ios::binary|std::ios::trunc);
    REQUIRE(writer);writer.write(reinterpret_cast<const char*>(future.data()),future.size());
    writer.close();REQUIRE(writer);
  }
  const auto updated=filesystem::MediaReferenceStore::Save(*metadata,*recovery,"pictures",8,"Renamed",R"(\Device\Mass8)",*selection);
  REQUIRE_FALSE(updated.error);REQUIRE(updated.durable);
  const auto loaded=filesystem::MediaReferenceStore::Load(*metadata,"pictures");REQUIRE_FALSE(loaded.error);REQUIRE(loaded.reference);
  REQUIRE(std::ranges::equal(loaded.reference->unknown,opaque));REQUIRE(loaded.reference->name=="Renamed");
  auto before=metadata->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(before);
  std::atomic<bool> canceled{true};const auto cancel=filesystem::MediaReferenceStore::Save(*metadata,*recovery,"pictures",8,"Canceled",R"(\Device\Mass8)",*selection,&canceled);
  REQUIRE(cancel.error==std::errc::operation_canceled);REQUIRE_FALSE(cancel.published);
  auto after=metadata->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(after);
  REQUIRE(std::ranges::equal(before->bytes(),after->bytes()));
  REQUIRE(filesystem::MediaReferenceStore::Load(*input,"pictures").error==std::errc::permission_denied);
  std::vector<uint8_t> corrupt(after->bytes().begin(),after->bytes().end());corrupt[0]^=0xFF;
  const auto denied_delete=filesystem::SnapshotPublisher::Delete(*recovery,after);
  REQUIRE(denied_delete.error);REQUIRE_FALSE(denied_delete.published);
  { // Malformed external input must be preserved, never replaced by Save.
    std::ofstream writer(scratch.root/"metadata"/leaf,std::ios::binary|std::ios::trunc);
    REQUIRE(writer);writer.write(reinterpret_cast<const char*>(corrupt.data()),corrupt.size());
    writer.close();REQUIRE(writer);
  }
  const auto refused=filesystem::MediaReferenceStore::Save(*metadata,*recovery,"pictures",8,"No empty replacement",R"(\Device\Mass8)",*selection);
  REQUIRE(refused.error==std::errc::invalid_argument);REQUIRE_FALSE(refused.published);
  auto preserved=metadata->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(preserved);
  REQUIRE(std::ranges::equal(preserved->bytes(),corrupt));
  const auto geometry=std::filesystem::space(scratch.root);
  REQUIRE(geometry.capacity<UINT64_MAX-filesystem::kHostStorageReserve);
  auto no_space_raw=filesystem::HostStorageRoot::Open(scratch.root/"full",false,error,geometry.capacity+filesystem::kHostStorageReserve);REQUIRE(no_space_raw);
  auto no_space=no_space_raw->ProtectFlat(error);REQUIRE(no_space);
  const auto full=filesystem::MediaReferenceStore::Save(*no_space,*recovery,"pictures",8,"Pictures",R"(\Device\Mass8)",*selection);
  REQUIRE(full.error==std::errc::no_space_on_device);REQUIRE_FALSE(full.published);
  REQUIRE(std::filesystem::file_size(scratch.root/"input/image")==11);
}
TEST_CASE("Media reference writes never grant publication to other private roles or raw paths",
          "[content][storage][selected_media][persistence]") {
  Scratch scratch;
  for(const auto name:{"input","flat","intents"}) {
    std::filesystem::create_directory(scratch.root/name);
    std::filesystem::permissions(scratch.root/name,std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }
  std::ofstream(scratch.root/"input/file")<<"source";
  std::error_code error;
  auto parent=filesystem::HostStorageRoot::Open(scratch.root,false,error,0);REQUIRE(parent);
  auto recovery=filesystem::SnapshotRecoveryOwner::Open(parent);REQUIRE(recovery);REQUIRE_FALSE(recovery->error());
  auto flat_raw=filesystem::HostStorageRoot::Open(scratch.root/"flat",false,error,0);REQUIRE(flat_raw);
  auto flat=flat_raw->ProtectFlat(error);REQUIRE(flat);
  auto intent_raw=filesystem::HostStorageRoot::Open(scratch.root/"intents",false,error,0);REQUIRE(intent_raw);
  auto intents=intent_raw->ProtectProfileIntents(error);REQUIRE(intents);
  auto input=filesystem::HostStorageRoot::Open(scratch.root/"input",true,error,0);REQUIRE(input);
  auto selected=filesystem::MediaSelection::CaptureFiles({{"file",input,"file"}},error);REQUIRE(selected);
  const auto leaf=filesystem::MediaReferenceStore::Leaf("scope");
  for(const filesystem::HostStorageRoot* target:{flat_raw.get(),intents.get(),recovery->root().get()}) {
    const auto refused=filesystem::MediaReferenceStore::Save(*target,*recovery,"scope",7,"Scope",R"(\Device\Mass7)",*selected);
    REQUIRE(refused.error);REQUIRE_FALSE(refused.published);REQUIRE_FALSE(refused.stage_created);
    REQUIRE_FALSE(std::filesystem::exists(target->path()/leaf));
  }
  const auto saved=filesystem::MediaReferenceStore::Save(*flat,*recovery,"scope",7,"Scope",R"(\Device\Mass7)",*selected);
  REQUIRE_FALSE(saved.error);REQUIRE(saved.published);REQUIRE(saved.durable);
  const auto before=flat->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(before);
  const std::array<uint8_t,1> arbitrary{0};
  const auto refused=filesystem::SnapshotPublisher::Publish(*recovery,before,arbitrary);
  REQUIRE(refused.error==std::errc::permission_denied);REQUIRE_FALSE(refused.stage_created);
  const auto denied_delete=filesystem::SnapshotPublisher::Delete(*recovery,before);
  REQUIRE(denied_delete.error);REQUIRE_FALSE(denied_delete.published);
  const auto after=flat->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(after);
  REQUIRE(std::ranges::equal(before->bytes(),after->bytes()));
  // The public protector API reuses the actual registered policy through an
  // empty retained Subroot. It must still support the same private intent.
  auto reused=flat_raw->ProtectFlat(error);REQUIRE(reused);REQUIRE_FALSE(error);
  REQUIRE(reused.get()!=flat.get());
  const auto updated=filesystem::MediaReferenceStore::Save(*reused,*recovery,"scope",7,"Reopened",R"(\Device\Mass7)",*selected);
  REQUIRE_FALSE(updated.error);REQUIRE(updated.durable);REQUIRE(updated.recovery_durable);
  const auto loaded=filesystem::MediaReferenceStore::Load(*flat,"scope");
  REQUIRE_FALSE(loaded.error);REQUIRE(loaded.reference);REQUIRE(loaded.reference->name=="Reopened");
  const auto removed=filesystem::MediaReferenceStore::Remove(*reused,*recovery,"scope");
  REQUIRE_FALSE(removed.error);REQUIRE(removed.durable);REQUIRE(removed.recovery_durable);
  REQUIRE_FALSE(std::filesystem::exists(flat->path()/leaf));
  REQUIRE(std::filesystem::file_size(scratch.root/"input/file")==6);
}

TEST_CASE("Selected folder keeps its genuine readonly subtree and never grants package paths",
          "[content][storage][selected_media]") {
  Scratch scratch;std::filesystem::create_directory(scratch.root/"selected");
  std::ofstream(scratch.root/"selected/photo.jpg")<<"picture";std::ofstream(scratch.root/"outside")<<"outside";
  std::error_code error;auto native=filesystem::HostStorageRoot::Open(scratch.root/"selected",false,error,0);REQUIRE(native);
  auto selection=filesystem::MediaSelection::CaptureFolder(native,error);REQUIRE(selection);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("folder",9);
  REQUIRE(registry.RegisterSelectedMedia(source,selection,1,fs));
  auto state=registry.Source("folder");REQUIRE(state);REQUIRE(state->storage);REQUIRE(state->storage->read_only());
  REQUIRE_FALSE(registry.Resolve(*state,"photo.jpg",false));
  auto view=selection->CaptureFolderRoot(error);REQUIRE(view);REQUIRE(view->read_only());
  // Host policy handles defer denial until mutation. The guest VFS separately
  // rejects requested write access before opening a selected-media entry.
  auto write=view->OpenFile("photo.jpg",filesystem::FileAccess::kGenericWrite,error);
  REQUIRE(write);REQUIRE_FALSE(error);
  const std::array<uint8_t,3> replacement{'b','a','d'};
  const auto refused=write->WriteResult(0,replacement);
  REQUIRE(refused.error==std::errc::read_only_file_system);REQUIRE(refused.bytes==0);
  write.reset();
  std::ifstream original(scratch.root/"selected/photo.jpg",std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(original)),{});
  REQUIRE(unchanged=="picture");
  const auto actual_device=fs.AcquireDevice(source.guest_volume_root);
  REQUIRE(actual_device);REQUIRE(actual_device->is_read_only());
  REQUIRE(actual_device->storage_volume()==registry.CaptureStorageVolume(*state));
  REQUIRE(fs.ResolvePath(source.guest_volume_root+"\\photo.jpg"));
  REQUIRE_FALSE(fs.ResolvePath(source.guest_volume_root+"\\..\\outside"));
  REQUIRE(registry.WithdrawSelectedMedia("folder",1));
  REQUIRE_FALSE(view->Observe().available);REQUIRE(std::filesystem::exists(scratch.root/"selected/photo.jpg"));
}

TEST_CASE("Selected replacement refusal preserves the exact active mount and metadata",
          "[content][storage][selected_media][replacement]") {
  Scratch scratch;std::ofstream(scratch.root/"old")<<"old";
  std::ofstream(scratch.root/"new")<<"new";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto old=filesystem::MediaSelection::CaptureFiles({{"entry",root,"old"}},error);REQUIRE(old);
  auto next=filesystem::MediaSelection::CaptureFiles({{"entry",root,"new"}},error);REQUIRE(next);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("active",3);
  REQUIRE(registry.RegisterSelectedMedia(source,old,1,fs));
  const auto before=registry.Source("active");REQUIRE(before);REQUIRE(before->available);
  const auto device=fs.AcquireDevice(source.guest_volume_root);REQUIRE(device);
  SECTION("a new selection bound to another actual source refuses before changing the old mount") {
    REQUIRE(registry.RegisterSelectedMedia(SelectedSource("other",4),next,2,fs));
    REQUIRE_FALSE(registry.RegisterSelectedMedia(source,next,2,fs));
  }
  SECTION("final common namespace conflict rolls back prepared source metadata") {
    bool inserted=false;
    SelectedMediaTimingAccess::Set(registry,{},[&](const auto&) {
      inserted=fs.RegisterSymbolicLink(source.guest_volume_root,R"(\Device\Unrelated)");
    });
    REQUIRE_FALSE(registry.RegisterSelectedMedia(source,next,2,fs));
    SelectedMediaTimingAccess::Set(registry);
    REQUIRE(inserted);REQUIRE(fs.UnregisterSymbolicLink(source.guest_volume_root));
  }
  REQUIRE(fs.AcquireDevice(source.guest_volume_root)==device);
  REQUIRE_FALSE(device->namespace_retiring());REQUIRE(before->storage_volume->available_cached());
  const auto after=registry.Source("active");REQUIRE(after);REQUIRE(after->available);
  REQUIRE(after->generation==1);REQUIRE(after->storage_volume==before->storage_volume);
  REQUIRE(registry.CaptureSelectedMedia(*after)==old);
  std::array<uint8_t,3> bytes{};const auto read=old->Read(0,0,bytes);
  REQUIRE_FALSE(read.error);REQUIRE(read.bytes==3);REQUIRE(bytes==std::array<uint8_t,3>{'o','l','d'});
}

TEST_CASE("Selected replacement preserves the single graph while an actual child mount exists",
          "[content][storage][selected_media][replacement]") {
  Scratch scratch;std::ofstream(scratch.root/"file")<<"data";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto old=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(old);
  auto next=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(next);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("tree",3);
  REQUIRE(registry.RegisterSelectedMedia(source,old,1,fs));
  auto parent=fs.AcquireDevice(source.guest_volume_root);REQUIRE(parent);
  auto view=root->Subroot({},true,error);REQUIRE(view);
  std::unique_ptr<vfs::Device> child=std::make_unique<vfs::HostPathDevice>(
      R"(\Device\MediaChild)",view->path(),true,false,"FATX",view);
  REQUIRE(child->Initialize());auto prepared=fs.PrepareDeviceRegistration(child,error,parent);REQUIRE(prepared);
  std::shared_ptr<vfs::Device> registered;REQUIRE(fs.RegisterPreparedDeviceExclusive(*prepared,registered)==X_STATUS_SUCCESS);
  REQUIRE(registered->mount_parent()==parent);
  REQUIRE_FALSE(registry.RegisterSelectedMedia(source,next,2,fs));
  REQUIRE(fs.AcquireDevice(source.guest_volume_root)==parent);REQUIRE_FALSE(parent->namespace_retiring());
  REQUIRE(registry.Source("tree")->available);REQUIRE(old->current_cached());
  REQUIRE(fs.UnregisterDevice(registered->mount_path(),registered.get()));
  auto pending_view=root->Subroot({},true,error);REQUIRE(pending_view);
  std::unique_ptr<vfs::Device> pending_child=std::make_unique<vfs::HostPathDevice>(
      R"(\Device\PendingMediaChild)",pending_view->path(),true,false,"FATX",pending_view);
  REQUIRE(pending_child->Initialize());
  auto pending=fs.PrepareDeviceRegistration(pending_child,error,parent);REQUIRE(pending);
  REQUIRE(registry.RegisterSelectedMedia(source,next,2,fs));
  std::shared_ptr<vfs::Device> refused;
  REQUIRE(fs.RegisterPreparedDeviceExclusive(*pending,refused)==X_STATUS(0xC000009D));
  REQUIRE_FALSE(refused);REQUIRE_FALSE(fs.AcquireDevice(R"(\Device\PendingMediaChild)"));
  const auto replacement=fs.AcquireDevice(source.guest_volume_root);REQUIRE(replacement);REQUIRE(replacement!=parent);
  REQUIRE_FALSE(fs.UnregisterDevice(parent->mount_path(),parent.get()));
  REQUIRE(fs.AcquireDevice(source.guest_volume_root)==replacement);REQUIRE(registry.Source("tree")->generation==2);
}

TEST_CASE("Concurrent selected sources cannot publish one guest device identity twice",
          "[content][storage][selected_media][concurrency]") {
  Scratch scratch;std::ofstream(scratch.root/"a")<<"a";std::ofstream(scratch.root/"b")<<"b";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto a=filesystem::MediaSelection::CaptureFiles({{"a",root,"a"}},error);REQUIRE(a);
  auto b=filesystem::MediaSelection::CaptureFiles({{"b",root,"b"}},error);REQUIRE(b);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;
  auto first=SelectedSource("first",7),second=SelectedSource("second",7);
  second.guest_volume_root=R"(\Device\Mass8)";
  std::promise<void> reached_a,reached_b,release;
  auto wait_a=reached_a.get_future(),wait_b=reached_b.get_future();auto released=release.get_future().share();
  SelectedMediaTimingAccess::Set(registry,{},[&](const std::string& id) {
    (id=="first"?reached_a:reached_b).set_value();released.wait();
  });
  auto task_a=std::async(std::launch::async,[&]{return registry.RegisterSelectedMedia(first,a,1,fs);});
  // Finish each real preparation before starting the next: shared-filesystem
  // admission is nonwaiting and may legitimately refuse concurrent prepare.
  // Both prepared transactions still wait above for the SAME release, so the
  // final identity publication remains concurrent. Release/join before REQUIRE.
  const bool captured_a=wait_a.wait_for(std::chrono::seconds(5))==std::future_status::ready;
  auto task_b=std::async(std::launch::async,[&]{return registry.RegisterSelectedMedia(second,b,1,fs);});
  const bool captured_b=wait_b.wait_for(std::chrono::seconds(5))==std::future_status::ready;
  release.set_value();const bool result_a=task_a.get(),result_b=task_b.get();SelectedMediaTimingAccess::Set(registry);
  REQUIRE(captured_a);REQUIRE(captured_b);REQUIRE(result_a!=result_b);
  REQUIRE(registry.DeviceIds()==std::vector<uint32_t>{7});
  const auto rows=registry.Sources(7);REQUIRE(rows.size()==1);REQUIRE(rows[0].available);
  REQUIRE(rows[0].source.id==(result_a?"first":"second"));
  REQUIRE(bool(fs.AcquireDevice(first.guest_volume_root))==result_a);
  REQUIRE(bool(fs.AcquireDevice(second.guest_volume_root))==result_b);
  // The pre-existing general source entry point cannot bypass the selected
  // physical-identity rule after either concurrent publication wins.
  auto managed=scratch.Managed("late-managed",7);managed.device_type=DeviceType::USBMASS;
  REQUIRE_FALSE(registry.Register(managed));REQUIRE(registry.Sources(7).size()==1);
}

TEST_CASE("A stale selected-media probe cannot revoke a newer actual source generation",
          "[content][storage][selected_media][concurrency]") {
  Scratch scratch;std::ofstream(scratch.root/"file")<<"payload";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto old=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(old);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("generation",5);
  REQUIRE(registry.RegisterSelectedMedia(source,old,1,fs));
  std::promise<void> captured,release;auto arrived=captured.get_future();auto released=release.get_future().share();
  std::atomic<bool> once{false};
  SelectedMediaTimingAccess::Set(registry,[&](const auto&) {
    if(!once.exchange(true)){captured.set_value();released.wait();}
  });
  auto delayed=std::async(std::launch::async,[&]{return registry.Source("generation");});
  const bool reached=arrived.wait_for(std::chrono::seconds(5))==std::future_status::ready;
  // Even on a setup refusal, always release the real observing thread before
  // a Catch assertion can unwind this fixture.
  bool withdrew=false,registered=false;std::shared_ptr<filesystem::MediaSelection> next;
  if(reached) {
    withdrew=registry.WithdrawSelectedMedia("generation",1);
    next=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);
    if(next)registered=registry.RegisterSelectedMedia(source,next,2,fs);
  }
  release.set_value();const auto refused=delayed.get();SelectedMediaTimingAccess::Set(registry);
  REQUIRE(reached);REQUIRE(withdrew);REQUIRE(next);REQUIRE(registered);
  REQUIRE(refused);REQUIRE_FALSE(refused->available);REQUIRE(refused->observation_error==std::errc::operation_canceled);
  REQUIRE_FALSE(registry.CaptureSelectedMedia(*refused));REQUIRE_FALSE(registry.CaptureStorageVolume(*refused));
  const auto current=registry.Source("generation");REQUIRE(current);REQUIRE(current->available);REQUIRE(current->generation==2);
  REQUIRE(current->storage_volume->available_cached());REQUIRE(registry.CaptureSelectedMedia(*current)==next);
  std::array<uint8_t,7> bytes{};REQUIRE(next->Read(0,0,bytes).bytes==7);
  REQUIRE(bytes==std::array<uint8_t,7>{'p','a','y','l','o','a','d'});
  auto connection=registry.storage_notifications()->Connect([](auto){});REQUIRE(connection);
  const auto* projected=FindVolume(connection->initial,"generation");REQUIRE(projected);
  REQUIRE(projected->available);REQUIRE(projected->generation==2);
  connection->subscription->BeginStop();REQUIRE(connection->subscription->Drain());
}

TEST_CASE("A refused GCRC media observation keeps real files and the cached attachment alive",
          "[content][storage][selected_media][lifetime]") {
  Scratch scratch;std::ofstream(scratch.root/"file")<<"data";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto selection=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(selection);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("context",4);
  REQUIRE(registry.RegisterSelectedMedia(source,selection,1,fs));
  const auto before=registry.Source("context");REQUIRE(before);REQUIRE(before->available);
  const auto files=root->open_file_count();REQUIRE(files>0);
  {
    auto held=global_critical_region::AcquireDirect();
    const auto physical=selection->Observe();REQUIRE_FALSE(physical.available);
    REQUIRE(physical.error==std::errc::operation_would_block);REQUIRE(selection->current_cached());
    const auto refused=registry.Source("context");REQUIRE(refused);REQUIRE_FALSE(refused->available);
    REQUIRE(refused->observation_error==std::errc::operation_would_block);
    REQUIRE_FALSE(refused->storage);REQUIRE_FALSE(refused->storage_volume);
    REQUIRE_FALSE(registry.CaptureSelectedMedia(*refused));REQUIRE(root->open_file_count()==files);
    REQUIRE(before->storage_volume->available_cached());
  }
  const auto after=registry.Source("context");REQUIRE(after);REQUIRE(after->available);
  REQUIRE(after->generation==before->generation);REQUIRE(after->storage_volume==before->storage_volume);
  REQUIRE(registry.CaptureSelectedMedia(*after)==selection);
  std::array<uint8_t,4> bytes{};REQUIRE(selection->Read(0,0,bytes).bytes==4);
  REQUIRE(bytes==std::array<uint8_t,4>{'d','a','t','a'});
}

TEST_CASE("Selected media authority is issued by one actual registry lifetime",
          "[content][storage][selected_media][lifetime]") {
  Scratch scratch;std::ofstream(scratch.root/"file")<<"data";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto first=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(first);
  auto independent=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(independent);
  auto registry_a=std::make_unique<ContentSourceRegistry>();ContentSourceRegistry registry_b;
  vfs::VirtualFileSystem fs_a,fs_b;auto source=SelectedSource("same",6);
  REQUIRE(registry_a->RegisterSelectedMedia(source,first,1,fs_a));
  REQUIRE(registry_a->RegisterSelectedMedia(source,first,1,fs_a));
  REQUIRE_FALSE(registry_b.RegisterSelectedMedia(source,first,1,fs_b));
  REQUIRE_FALSE(fs_b.AcquireDevice(source.guest_volume_root));REQUIRE_FALSE(registry_b.Source("same"));
  REQUIRE(registry_a->Source("same")->available);REQUIRE(first->current_cached());
  REQUIRE(registry_b.RegisterSelectedMedia(source,independent,1,fs_b));
  const auto other_device=fs_b.AcquireDevice(source.guest_volume_root);REQUIRE(other_device);
  registry_a.reset();REQUIRE_FALSE(first->current_cached());
  const auto remaining=registry_b.Source("same");REQUIRE(remaining);REQUIRE(remaining->available);
  REQUIRE(registry_b.CaptureSelectedMedia(*remaining)==independent);
  REQUIRE(fs_b.AcquireDevice(source.guest_volume_root)==other_device);
  std::array<uint8_t,4> bytes{};REQUIRE(independent->Read(0,0,bytes).bytes==4);
  REQUIRE(bytes==std::array<uint8_t,4>{'d','a','t','a'});
}

TEST_CASE("Unparseable reference names never publish or replace valid media intent",
          "[content][storage][selected_media][persistence]") {
  Scratch scratch;std::filesystem::create_directory(scratch.root/"input");
  std::filesystem::create_directory(scratch.root/"metadata");
  std::filesystem::permissions(scratch.root/"metadata",std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
  std::ofstream(scratch.root/"input/file")<<"data";
  std::error_code error;auto parent=filesystem::HostStorageRoot::Open(scratch.root,false,error,0);REQUIRE(parent);
  auto recovery=filesystem::SnapshotRecoveryOwner::Open(parent);REQUIRE(recovery);REQUIRE_FALSE(recovery->error());
  auto raw=filesystem::HostStorageRoot::Open(scratch.root/"metadata",false,error,0);REQUIRE(raw);
  auto metadata=raw->ProtectFlat(error);REQUIRE(metadata);
  auto input=filesystem::HostStorageRoot::Open(scratch.root/"input",true,error,0);REQUIRE(input);
  auto selection=filesystem::MediaSelection::CaptureFiles({{"file",input,"file"}},error);REQUIRE(selection);
  const auto saved=filesystem::MediaReferenceStore::Save(*metadata,*recovery,"valid",7,"Valid",R"(\Device\Mass7)",*selection);
  REQUIRE_FALSE(saved.error);REQUIRE(saved.durable);
  const auto leaf=filesystem::MediaReferenceStore::Leaf("valid");
  const auto before=metadata->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(before);
  for(const auto& id:{std::string("valid"),std::string("fresh")}) {
    const auto refused=filesystem::MediaReferenceStore::Save(*metadata,*recovery,id,7,std::string("bad\0name",8),R"(\Device\Mass7)",*selection);
    REQUIRE(refused.error==std::errc::invalid_argument);REQUIRE_FALSE(refused.published);REQUIRE_FALSE(refused.durable);
  }
  const auto after=metadata->CaptureSnapshot(leaf,filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);REQUIRE(after);
  REQUIRE(std::ranges::equal(before->bytes(),after->bytes()));
  const auto valid=filesystem::MediaReferenceStore::Load(*metadata,"valid");REQUIRE_FALSE(valid.error);
  REQUIRE(valid.reference);REQUIRE(valid.reference->name=="Valid");
  const auto absent=metadata->CaptureSnapshot(filesystem::MediaReferenceStore::Leaf("fresh"),filesystem::MediaReferenceStore::kMaximumWireBytes,error,true);
  REQUIRE(absent);REQUIRE_FALSE(absent->exists());REQUIRE(std::filesystem::file_size(scratch.root/"input/file")==4);
}


TEST_CASE("Selected media guest opens preserve exact readonly intent without changing legacy devices",
          "[content][storage][selected_media][readonly]") {
  const bool folder=GENERATE(false,true);
  Scratch scratch;std::filesystem::create_directory(scratch.root/"selected");
  std::filesystem::create_directory(scratch.root/"legacy");
  std::ofstream(scratch.root/"selected/asset")<<"payload";
  std::ofstream(scratch.root/"legacy/asset")<<"legacy";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root/"selected",true,error,0);REQUIRE(root);
  auto selection=folder?filesystem::MediaSelection::CaptureFolder(root,error):
      filesystem::MediaSelection::CaptureFiles({{"asset",root,"asset"}},error);REQUIRE(selection);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("strict",2);
  REQUIRE(registry.RegisterSelectedMedia(source,selection,1,fs));
  const auto device=fs.AcquireDevice(source.guest_volume_root);REQUIRE(device);
  REQUIRE(device->requires_strict_read_only_open());
  const auto path=source.guest_volume_root+"\\asset";
  vfs::File* file=nullptr;vfs::FileAction action;
  REQUIRE(fs.OpenFile(nullptr,path,vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,
      false,true,&file,&action)==X_STATUS_SUCCESS);REQUIRE(file);
  std::array<uint8_t,7> bytes{};size_t count=0;
  REQUIRE(file->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==7);
  REQUIRE(bytes==std::array<uint8_t,7>{'p','a','y','l','o','a','d'});file->Destroy();
  for(const auto access:{vfs::FileAccess::kGenericWrite,vfs::FileAccess::kFileWriteAttributes,
      0x10u,0x10000u,0x40000u,vfs::FileAccess::kGenericAll}) {
    file=nullptr;
    REQUIRE(fs.OpenFile(nullptr,path,vfs::FileDisposition::kOpen,access,
        false,true,&file,&action)==X_STATUS_ACCESS_DENIED);REQUIRE_FALSE(file);
  }
  for(const auto disposition:{vfs::FileDisposition::kOverwrite,vfs::FileDisposition::kOverwriteIf,
      vfs::FileDisposition::kCreate,vfs::FileDisposition::kSuperscede}) {
    file=nullptr;
    REQUIRE(fs.OpenFile(nullptr,path,disposition,vfs::FileAccess::kGenericRead,
        false,true,&file,&action)==X_STATUS_ACCESS_DENIED);REQUIRE_FALSE(file);
  }
  file=nullptr;
  REQUIRE(fs.OpenFile(nullptr,path,vfs::FileDisposition::kOpenIf,vfs::FileAccess::kGenericRead,
      false,true,&file,&action)==X_STATUS_SUCCESS);REQUIRE(file);file->Destroy();file=nullptr;
  REQUIRE(fs.OpenFile(nullptr,source.guest_volume_root+"\\new",vfs::FileDisposition::kOpenIf,
      vfs::FileAccess::kGenericRead,false,true,&file,&action)==X_STATUS_ACCESS_DENIED);REQUIRE_FALSE(file);
  REQUIRE_FALSE(std::filesystem::exists(scratch.root/"selected/new"));
  std::ifstream original(scratch.root/"selected/asset",std::ios::binary);
  const std::string original_bytes{std::istreambuf_iterator<char>(original),std::istreambuf_iterator<char>()};
  REQUIRE(original_bytes=="payload");
  // Existing readonly compatibility behavior is intentionally unchanged for
  // an ordinary host/disc-style device which has no selected-media role.
  auto legacy=std::make_unique<vfs::HostPathDevice>(R"(\Device\LegacyReadOnly)",
      scratch.root/"legacy",true);REQUIRE(legacy->Initialize());
  REQUIRE_FALSE(legacy->requires_strict_read_only_open());REQUIRE(fs.RegisterDevice(std::move(legacy)));
  file=nullptr;
  REQUIRE(fs.OpenFile(nullptr,R"(\Device\LegacyReadOnly\asset)",vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericWrite,false,true,&file,&action)==X_STATUS_SUCCESS);REQUIRE(file);
  file->Destroy();REQUIRE(std::filesystem::file_size(scratch.root/"legacy/asset")==6);
}


TEST_CASE("Selected file size changes invalidate one captured generation without freezing same-size bytes",
          "[content][storage][selected_media][currentness]") {
  const int change=GENERATE(-1,0,1);
  Scratch scratch;std::ofstream(scratch.root/"file")<<"abcdef";
  std::error_code error;auto root=filesystem::HostStorageRoot::Open(scratch.root,true,error,0);REQUIRE(root);
  auto observer=root->OpenFile("file",filesystem::FileAccess::kGenericRead,error);REQUIRE(observer);
  const auto initial=observer->QueryInfo(error);REQUIRE(initial);REQUIRE(initial->size==6);
  auto selection=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(selection);
  ContentSourceRegistry registry;vfs::VirtualFileSystem fs;auto source=SelectedSource("size",5);
  REQUIRE(registry.RegisterSelectedMedia(source,selection,1,fs));
  const auto before=registry.Source("size");REQUIRE(before);REQUIRE(before->available);
  const uint64_t expected=change<0?2:change>0?10:6;
  if(change)std::filesystem::resize_file(scratch.root/"file",expected);
  else {std::fstream edit(scratch.root/"file",std::ios::in|std::ios::out|std::ios::binary);REQUIRE(edit.good());edit<<"UVWXYZ";edit.flush();REQUIRE(edit.good());}
  const auto actual=observer->QueryInfo(error);REQUIRE(actual);REQUIRE_FALSE(error);
  REQUIRE(actual->filesystem_id==initial->filesystem_id);REQUIRE(actual->file_id==initial->file_id);
  REQUIRE(actual->size==expected);
  std::array<uint8_t,3> bytes{0x71,0x72,0x73};const auto read=selection->Read(0,0,bytes);
  if(!change) {
    REQUIRE_FALSE(read.error);REQUIRE(read.bytes==3);REQUIRE(bytes==std::array<uint8_t,3>{'U','V','W'});
    const auto current=registry.Source("size");REQUIRE(current);REQUIRE(current->available);REQUIRE(current->generation==1);
    REQUIRE(current->storage_volume==before->storage_volume);return;
  }
  REQUIRE(read.error==std::errc::no_such_device);REQUIRE(read.bytes==0);
  REQUIRE(bytes==std::array<uint8_t,3>{0x71,0x72,0x73});
  const auto unavailable=registry.Source("size");REQUIRE(unavailable);REQUIRE_FALSE(unavailable->available);
  REQUIRE_FALSE(selection->current_cached());REQUIRE_FALSE(before->storage_volume->available_cached());
  REQUIRE_FALSE(registry.CaptureSelectedMedia(*before));REQUIRE(selection->entries()[0].bytes==6);
  auto recaptured=filesystem::MediaSelection::CaptureFiles({{"file",root,"file"}},error);REQUIRE(recaptured);
  REQUIRE(recaptured->entries()[0].bytes==expected);
  REQUIRE(registry.RegisterSelectedMedia(source,recaptured,2,fs));
  const auto current=registry.Source("size");REQUIRE(current);REQUIRE(current->available);REQUIRE(current->generation==2);
  const auto entry=fs.ResolvePath(source.guest_volume_root+"\\file");REQUIRE(entry);REQUIRE(entry->size()==expected);
  bytes.fill(0x75);const auto fresh=recaptured->Read(0,0,bytes);REQUIRE_FALSE(fresh.error);
  REQUIRE(fresh.bytes==std::min<uint64_t>(expected,bytes.size()));
  REQUIRE(bytes[0]=='a');REQUIRE(bytes[1]=='b');
}

#endif


#if XE_PLATFORM_LINUX
namespace {
using CacheOwner=filesystem::CacheStorageOwner;
using CacheRole=CacheOwner::Role;
using CacheDisk=filesystem::test::CacheFixture;
ContentSource CacheSource(CacheOwner& owner,CacheRole role,std::error_code& error){
  const auto identity=filesystem::CacheStorageOwnerTestAccess::Identity(owner,role);REQUIRE(identity);
  const auto root=owner.CaptureRoot(role,error);REQUIRE(root);REQUIRE_FALSE(error);
  constexpr std::array<const char*,3> mounts{R"(\CACHE)",R"(\CACHE0)",R"(\CACHE1)"};
  ContentSource source;source.id=identity->source_id();source.root=root->path();source.device_id=1;
  source.device_type=DeviceType::HDD;source.kind=ContentSourceKind::kCache;source.read_only=false;
  source.reserved_bytes=CacheOwner::kMinimumFreeBytes;source.name=u"Title Cache";
  source.guest_volume_root=mounts[size_t(role)];source.guest_root=source.guest_volume_root;return source;
}
std::array<ContentSource,3> RegisterCacheRoles(ContentSourceRegistry& registry,vfs::VirtualFileSystem& fs,
    const std::shared_ptr<CacheOwner>& owner,uint64_t generation=1){
  std::array<ContentSource,3> sources;std::error_code error;
  for(size_t i=0;i<3;++i){sources[i]=CacheSource(*owner,CacheRole(i),error);
    REQUIRE(registry.RegisterCache(sources[i],owner,CacheRole(i),generation+i,fs));}
  return sources;
}
vfs::File* OpenCache(vfs::VirtualFileSystem& fs,std::string_view path,uint32_t access,
    vfs::FileDisposition disposition=vfs::FileDisposition::kOpen){
  vfs::File* file=nullptr;vfs::FileAction action;
  REQUIRE(fs.OpenFile(nullptr,path,disposition,access,false,true,&file,&action)==X_STATUS_SUCCESS);
  REQUIRE(file);return file;
}
}
TEST_CASE("Cache roles use actual prepared storage but never become content devices", "[cache_owner][content][role]") {
  CacheDisk disk;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto owner=disk.Open(0xCAFE0001,error);REQUIRE(owner);REQUIRE_FALSE(error);
  const auto sources=RegisterCacheRoles(registry,fs,owner);
  REQUIRE(registry.DeviceIds().empty());REQUIRE(registry.Sources(1).empty());REQUIRE_FALSE(registry.WritableSource(1));
  auto connection=registry.storage_notifications()->Connect([](auto){});REQUIRE(connection);REQUIRE(connection->initial.devices.empty());
  for(size_t i=0;i<3;++i){
    const auto actual=registry.Source(sources[i].id);REQUIRE(actual);REQUIRE(actual->available);
    REQUIRE_FALSE(actual->profile_writable);REQUIRE_FALSE(actual->format_eligible);
    REQUIRE_FALSE(actual->managed_identity);REQUIRE_FALSE(actual->volume);REQUIRE_FALSE(actual->managed_format_proof);
    const auto volume=registry.CaptureStorageVolume(*actual);REQUIRE(volume);REQUIRE(volume->type()==DeviceType::HDD);
    REQUIRE(volume->source_id()==sources[i].id);REQUIRE(volume->generation()==1+i);
    REQUIRE_FALSE(registry.Resolve(*actual,"save",true));REQUIRE_FALSE(registry.Resolve(sources[i].id,"save",false));
    bool matched=false;REQUIRE_FALSE(registry.CaptureSourceDirectory(sources[i].root,false,matched,error));REQUIRE_FALSE(matched);
  }
  const std::array<uint8_t,5> bytes{1,0,0xFF,4,9};
  auto* file=OpenCache(fs,R"(cache0:\real-save)",vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,vfs::FileDisposition::kCreate);
  size_t transferred=0;REQUIRE(file->WriteSync(bytes,0,&transferred)==X_STATUS_SUCCESS);REQUIRE(transferred==bytes.size());
  REQUIRE(file->Flush()==X_STATUS_SUCCESS);std::array<uint8_t,5> output{};
  REQUIRE(file->ReadSync(output,0,&transferred)==X_STATUS_SUCCESS);REQUIRE(transferred==output.size());REQUIRE(output==bytes);
  file->Destroy();REQUIRE(CacheDisk::NativeBytes(disk.Source(0xCAFE0001,1)/"real-save")==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  REQUIRE_FALSE(disk.root->Create("actual-content",true));
  ContentSource hdd{"hdd",1,DeviceType::HDD,ContentSourceKind::kManaged,disk.root->path()/"actual-content","",u"HDD",false,0};
  hdd.guest_volume_root=R"(\Device\Harddisk0\Partition1)";REQUIRE(registry.Register(hdd));
  REQUIRE(registry.DeviceIds()==std::vector<uint32_t>{1});REQUIRE(registry.Sources(1).size()==1);
  REQUIRE(registry.WritableSource(1)->source.id=="hdd");disk.Originals();
  connection->subscription->BeginStop();REQUIRE(connection->subscription->Drain());
}
TEST_CASE("Cache logical metadata writes only its actual table window", "[cache_owner][vfs][range]") {
  CacheDisk disk;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto owner=disk.Open(0xCAFE0001,error);REQUIRE(owner);REQUIRE_FALSE(error);
  const auto sources=RegisterCacheRoles(registry,fs,owner);
  const auto legacy=registry.Source(sources[0].id);REQUIRE(legacy);
  auto metadata=vfs::CacheMetadataDevice::Create(owner,registry.CaptureStorageVolume(*legacy),error);
  REQUIRE(metadata);REQUIRE_FALSE(error);std::shared_ptr<vfs::Device> registered;
  REQUIRE(fs.RegisterDeviceExclusive(std::move(metadata),registered)==X_STATUS_SUCCESS);
  auto* file=OpenCache(fs,R"(\Device\Harddisk0\Partition0)",vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite);
  const std::array<uint8_t,8> bytes{7,0,9,2,4,0,0xBB,0xFF};size_t count=99;
  REQUIRE(file->WriteSync(bytes,0x880,&count)==X_STATUS_SUCCESS);REQUIRE(count==bytes.size());REQUIRE(file->Flush()==X_STATUS_SUCCESS);
  std::array<uint8_t,8> output{};REQUIRE(file->ReadSync(output,0x880,&count)==X_STATUS_SUCCESS);REQUIRE(output==bytes);REQUIRE(count==bytes.size());
  for(const auto offset:{size_t(0),size_t(0x7FF),size_t(0xBFF),std::numeric_limits<size_t>::max()}){
    output.fill(0xCD);count=99;REQUIRE(file->ReadSync(output,offset,&count)==X_STATUS_NOT_SUPPORTED);REQUIRE(count==0);
    REQUIRE(output==std::array<uint8_t,8>{0xCD,0xCD,0xCD,0xCD,0xCD,0xCD,0xCD,0xCD});
    count=99;REQUIRE(file->WriteSync(bytes,offset,&count)==X_STATUS_NOT_SUPPORTED);REQUIRE(count==0);
  }
  auto busy=owner->CaptureFormat(CacheRole::kCache0,8);REQUIRE(busy);REQUIRE(busy->receipt().error==std::errc::device_or_resource_busy);
  REQUIRE_FALSE(busy->receipt().prepared_durable);busy.reset();file->Destroy();
  auto* raw=OpenCache(fs,R"(\Device\Harddisk0\Cache1)",vfs::FileAccess::kGenericWrite);
  // No native utility initializer/full-header protocol is invented. A real
  // FATX-looking packet cannot become discarded success or geometry mutation.
  std::array<uint8_t,0x1000> header{};header[0]='X';header[1]='T';header[2]='A';header[3]='F';header[11]=8;
  count=99;REQUIRE(raw->WriteSync(header,0,&count)==X_STATUS_NOT_SUPPORTED);REQUIRE(count==0);raw->Destroy();
  auto table=owner->table(error);REQUIRE_FALSE(error);REQUIRE(table.sectors_per_cluster[1]==0);
  REQUIRE(std::equal(bytes.begin(),bytes.end(),table.bytes.begin()+0x80));disk.Originals();
}
TEST_CASE("Cache committed geometry publishes only a fresh exact folder device", "[cache_owner][vfs][format]") {
  CacheDisk disk;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto owner=disk.Open(0xCAFE0001,error);REQUIRE(owner);REQUIRE_FALSE(error);const auto sources=RegisterCacheRoles(registry,fs,owner);
  const auto old_device=fs.AcquireDevice(R"(\CACHE0)");REQUIRE(old_device);
  const auto old_unit=old_device->sectors_per_allocation_unit();REQUIRE(old_unit!=0);
  REQUIRE(registry.RegisterCache(sources[1],owner,CacheRole::kCache0,2,fs));
  REQUIRE_FALSE(registry.RegisterCache(sources[1],owner,CacheRole::kCache0,4,fs));
  REQUIRE(fs.AcquireDevice(R"(\CACHE0)")==old_device);REQUIRE_FALSE(old_device->namespace_retiring());
  REQUIRE(registry.Source(sources[1].id)->available);
  REQUIRE(registry.Source(sources[1].id)->generation==2);
  auto* still_current=OpenCache(fs,R"(cache0:\same-root-refusal)",vfs::FileAccess::kGenericWrite,vfs::FileDisposition::kCreate);
  const std::array<uint8_t,3> preserved{4,8,15};size_t written=0;
  REQUIRE(still_current->WriteSync(preserved,0,&written)==X_STATUS_SUCCESS);REQUIRE(written==preserved.size());
  REQUIRE(still_current->Flush()==X_STATUS_SUCCESS);still_current->Destroy();
  REQUIRE(CacheDisk::NativeBytes(disk.Source(0xCAFE0001,1)/"same-root-refusal")==std::vector<uint8_t>(preserved.begin(),preserved.end()));
  auto handoff=owner->CaptureFormat(CacheRole::kCache0,8);REQUIRE(handoff);REQUIRE_FALSE(handoff->receipt().error);
  const auto complete=CacheOwner::ExecuteFormat(*handoff);REQUIRE_FALSE(complete.error);REQUIRE(complete.complete);
  REQUIRE(old_device->sectors_per_allocation_unit()==old_unit);
  REQUIRE(registry.RegisterCache(sources[1],owner,CacheRole::kCache0,4,fs));
  const auto fresh=fs.AcquireDevice(R"(\CACHE0)");REQUIRE(fresh);REQUIRE(fresh!=old_device);
  REQUIRE(fresh->sectors_per_allocation_unit()==8);REQUIRE(fresh->bytes_per_sector()==0x200);
  REQUIRE(fresh->storage_volume()->generation()==4);
  REQUIRE(old_device->sectors_per_allocation_unit()==old_unit);disk.Originals();
}

TEST_CASE("Cache registration rejects foreign aliases issuers and NUL metadata without publication", "[cache_owner][vfs][conflict]") {
  CacheDisk disk;ContentSourceRegistry registry,foreign_registry;vfs::VirtualFileSystem fs,foreign_fs;
  std::error_code error;auto owner=disk.Open(0xCAFE0001,error);REQUIRE(owner);REQUIRE_FALSE(error);
  auto source=CacheSource(*owner,CacheRole::kCache0,error);
  auto malformed=source;malformed.id.push_back('\0');malformed.id+="foreign";
  REQUIRE_FALSE(registry.RegisterCache(malformed,owner,CacheRole::kCache0,1,fs));
  REQUIRE_FALSE(fs.AcquireDevice(source.guest_volume_root));REQUIRE_FALSE(fs.IsSymbolicLinkRegistered("cache0:"));
  malformed=source;malformed.name.push_back(0);malformed.name+=u"hidden";
  REQUIRE_FALSE(registry.RegisterCache(malformed,owner,CacheRole::kCache0,1,fs));
  auto foreign=std::make_unique<vfs::NullDevice>(source.guest_volume_root,std::initializer_list<std::string>{});REQUIRE(foreign->Initialize());
  std::shared_ptr<vfs::Device> foreign_device;REQUIRE(fs.RegisterDeviceExclusive(std::move(foreign),foreign_device)==X_STATUS_SUCCESS);
  REQUIRE_FALSE(registry.RegisterCache(source,owner,CacheRole::kCache0,1,fs));REQUIRE(fs.AcquireDevice(source.guest_volume_root)==foreign_device);
  REQUIRE_FALSE(registry.Source(source.id));REQUIRE_FALSE(fs.IsSymbolicLinkRegistered("cache0:"));
  REQUIRE(fs.UnregisterDevice(source.guest_volume_root,foreign_device.get()));foreign_device.reset();
  REQUIRE(registry.RegisterCache(source,owner,CacheRole::kCache0,1,fs));
  REQUIRE_FALSE(foreign_registry.RegisterCache(source,owner,CacheRole::kCache0,1,foreign_fs));
  REQUIRE_FALSE(foreign_registry.Source(source.id));REQUIRE_FALSE(foreign_fs.AcquireDevice(source.guest_volume_root));
  // A separate foreign alias also remains exact, even with an owned folder.
  auto second=CacheSource(*owner,CacheRole::kCache1,error);
  REQUIRE(fs.RegisterSymbolicLink("cache1:",R"(\UnrelatedVolume)"));
  REQUIRE_FALSE(registry.RegisterCache(second,owner,CacheRole::kCache1,2,fs));
  std::string target;REQUIRE(fs.FindSymbolicLink("cache1:",target));REQUIRE(target==R"(\UnrelatedVolume)");
  REQUIRE_FALSE(registry.Source(second.id));REQUIRE_FALSE(fs.AcquireDevice(second.guest_volume_root));disk.Originals();
}
TEST_CASE("Cache withdrawal preserves old file identity and publishes a different product", "[cache_owner][vfs][lifetime]") {
  CacheDisk disk;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto first=disk.Open(0xCAFE0001,error);REQUIRE(first);REQUIRE_FALSE(error);const auto old=RegisterCacheRoles(registry,fs,first);
  auto* retained=OpenCache(fs,R"(cache0:\old-data)",vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericRead,vfs::FileDisposition::kCreate);
  const std::array<uint8_t,4> data{1,3,5,7};size_t bytes=0;REQUIRE(retained->WriteSync(data,0,&bytes)==X_STATUS_SUCCESS);REQUIRE(retained->Flush()==X_STATUS_SUCCESS);
  const auto old_device=retained->retained_device();const auto old_path=disk.Source(0xCAFE0001,1)/"old-data";
  for(size_t i=0;i<3;++i)REQUIRE(registry.WithdrawCache(old[i].id,1+i,fs));
  first->Retire();REQUIRE(first->CloseNativeAfterIoDrained());
  auto second=disk.Open(0xCAFE0002,error);REQUIRE(second);REQUIRE_FALSE(error);const auto next=RegisterCacheRoles(registry,fs,second,4);
  REQUIRE(fs.AcquireDevice(R"(\CACHE0)")!=old_device);
  std::array<uint8_t,4> unchanged{0xCC,0xCC,0xCC,0xCC};bytes=99;
  REQUIRE(retained->ReadSync(unchanged,0,&bytes)!=X_STATUS_SUCCESS);REQUIRE(bytes==0);
  REQUIRE(unchanged==std::array<uint8_t,4>{0xCC,0xCC,0xCC,0xCC});retained->Destroy();
  REQUIRE(CacheDisk::NativeBytes(old_path)==std::vector<uint8_t>(data.begin(),data.end()));
  REQUIRE_FALSE(std::filesystem::exists(disk.Source(0xCAFE0002,1)/"old-data"));
  REQUIRE_FALSE(registry.WithdrawCache(old[1].id,2,fs));
  REQUIRE(registry.Source(next[1].id)->available);std::string target;REQUIRE(fs.FindSymbolicLink("cache0:",target));REQUIRE(target==R"(\CACHE0)");disk.Originals();
}
TEST_CASE("Cache registry reports actual removed source and never adopts its replacement", "[cache_owner][content][removal]") {
  CacheDisk disk;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto owner=disk.Open(0xCAFE0001,error);REQUIRE(owner);REQUIRE_FALSE(error);const auto sources=RegisterCacheRoles(registry,fs,owner);
  const auto before=registry.Source(sources[1].id);REQUIRE(before);REQUIRE(before->available);
  const auto descriptor=registry.CaptureStorageVolume(*before);REQUIRE(descriptor);const auto generation=before->generation;
  const auto original=disk.Source(0xCAFE0001,1);std::filesystem::rename(original,original.parent_path()/"removed-product");
  REQUIRE(std::filesystem::create_directory(original));
  const auto unavailable=registry.Source(sources[1].id);REQUIRE(unavailable);REQUIRE_FALSE(unavailable->available);
  REQUIRE(unavailable->generation==generation);REQUIRE_FALSE(descriptor->available_cached());
  REQUIRE_FALSE(registry.CaptureStorageVolume(*before));REQUIRE_FALSE(registry.WritableSource(1));
  REQUIRE_FALSE(registry.Resolve(sources[1].id,"new-save",true));
  REQUIRE_FALSE(std::filesystem::exists(original/filesystem::ManagedDeviceIdentity::kMarker));disk.Originals();
}

TEST_CASE("Three cache roles cannot block genuine pending HDD recovery or bypass exact source IDs", "[cache_owner][content][managed_format][recovery]") {
  const bool exact_id_conflict=GENERATE(false,true);
  CacheDisk disk;Scratch hdd;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto cache=disk.Open(0xCAFE0001,error);REQUIRE(cache);REQUIRE_FALSE(error);
  const auto caches=RegisterCacheRoles(registry,fs,cache);
  const auto source=hdd.Managed("hdd",1);
  auto parent=filesystem::HostStorageRoot::Open(hdd.root,false,error);REQUIRE(parent);REQUIRE_FALSE(error);
  const auto options=[&]{
    filesystem::ManagedVolumeAuthority::Options value;
    value.content=filesystem::HostStorageRoot::Open(source.root,false,error);REQUIRE(value.content);REQUIRE_FALSE(error);
    value.source_id=source.id;value.device_id=source.device_id;value.state_parent_override=parent;return value;
  };
  auto volume=filesystem::ManagedVolumeAuthority::Open(options());REQUIRE(volume);REQUIRE_FALSE(volume->error());
  REQUIRE(volume->format_eligible());const auto uuid=volume->identity()->uuid();const auto epoch=volume->identity()->content_epoch();
  const std::array<uint8_t,4> old_data{8,6,7,5};
  REQUIRE_FALSE(volume->content()->WriteNewFile("old-save",old_data).error);
  auto original=volume->CaptureFormatHandoff();REQUIRE(original);REQUIRE_FALSE(original->error());
  original->ClosePublishedPublicView();volume.reset();
  const auto reset=filesystem::ManagedVolumeAuthority::ExecuteFormat(*original);
  REQUIRE_FALSE(reset.error);REQUIRE(reset.complete);REQUIRE(original->phase()==1);
  REQUIRE_FALSE(std::filesystem::exists(source.root/"old-save"));original.reset();
  {std::ofstream after(source.root/"after-reset",std::ios::binary);after<<"preserve actual new epoch";REQUIRE(after.good());}
  auto recovery=filesystem::ManagedVolumeAuthority::InspectFormatRecovery(options());
  REQUIRE(recovery);REQUIRE_FALSE(recovery->error());REQUIRE(recovery->phase()==1);
  REQUIRE(recovery->captured_identity()->uuid()==uuid);
  if(exact_id_conflict){
    REQUIRE_FALSE(parent->Create("readonly-overlay",true));
    auto overlay=parent->Subroot("readonly-overlay",true,error);REQUIRE(overlay);REQUIRE_FALSE(error);
    ContentSource collision{source.id,1,DeviceType::HDD,ContentSourceKind::kInstalledSystem,
        overlay->path(),R"(\InstalledCollision)",u"Actual readonly overlay",true,0};
    REQUIRE(registry.RegisterReadOnly(collision,overlay,1));
  }
  auto transition=registry.BeginManagedRecovery(source,*recovery,error);
  if(exact_id_conflict){
    REQUIRE_FALSE(transition);REQUIRE(error==std::errc::state_not_recoverable);
    const auto held=registry.Source(source.id);REQUIRE(held);REQUIRE(held->available);
    REQUIRE(held->source.kind==ContentSourceKind::kInstalledSystem);
    const auto resumed=filesystem::ManagedVolumeAuthority::ResumeFormat(*recovery);
    REQUIRE(resumed);REQUIRE_FALSE(resumed->error());
  }else{
    REQUIRE(transition);REQUIRE_FALSE(error);transition->PublishWithdrawal();
    const auto pending=registry.Source(source.id);REQUIRE(pending);REQUIRE_FALSE(pending->available);
    const auto resumed=filesystem::ManagedVolumeAuthority::ResumeFormat(*recovery);
    REQUIRE(resumed);REQUIRE_FALSE(resumed->error());REQUIRE(resumed->identity()->uuid()==uuid);
    REQUIRE(resumed->identity()->content_epoch()==epoch+1);REQUIRE_FALSE(transition->Publish(resumed));
    const auto current=registry.Source(source.id);REQUIRE(current);REQUIRE(current->available);
    REQUIRE(current->managed_identity==resumed->identity());REQUIRE(current->profile_writable);
    REQUIRE(registry.DeviceIds()==std::vector<uint32_t>{1});REQUIRE(registry.Sources(1).size()==1);
    REQUIRE(registry.WritableSource(1)->source.id==source.id);
  }
  REQUIRE(CacheDisk::NativeBytes(source.root/"after-reset")==std::vector<uint8_t>{
      'p','r','e','s','e','r','v','e',' ','a','c','t','u','a','l',' ','n','e','w',' ','e','p','o','c','h'});
  for(const auto& row:caches){const auto held=registry.Source(row.id);REQUIRE(held);REQUIRE(held->available);}
  disk.Originals();
}
TEST_CASE("Cache withdrawal preserves its closing record until detached namespace absence is proven", "[cache_owner][vfs][lifetime][alias]") {
  const bool expire_device=GENERATE(false,true);
  CacheDisk disk;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto owner=disk.Open(0xCAFE0001,error);REQUIRE(owner);REQUIRE_FALSE(error);const auto sources=RegisterCacheRoles(registry,fs,owner);
  auto device=fs.AcquireDevice(R"(\CACHE0)");REQUIRE(device);const auto weak=std::weak_ptr<vfs::Device>(device);
  REQUIRE(fs.UnregisterDevice(device->mount_path(),device.get()));
  if(expire_device)device.reset();REQUIRE(weak.expired()==expire_device);
  std::string target;REQUIRE(fs.FindSymbolicLink("cache0:",target));REQUIRE(target==R"(\CACHE0)");
  REQUIRE_FALSE(registry.WithdrawCache(sources[1].id,2,fs));
  const auto closing=registry.Source(sources[1].id);REQUIRE(closing);REQUIRE_FALSE(closing->available);
  REQUIRE(fs.FindSymbolicLink("cache0:",target));REQUIRE(target==R"(\CACHE0)");
  // The external namespace owner which detached the device now removes its
  // leftover alias. Withdrawal itself never adopts a name as delete authority.
  REQUIRE(fs.UnregisterSymbolicLink("cache0:"));
  REQUIRE(registry.WithdrawCache(sources[1].id,2,fs));REQUIRE_FALSE(registry.Source(sources[1].id));
  for(const auto i:{size_t(0),size_t(2)})REQUIRE(registry.WithdrawCache(sources[i].id,1+i,fs));
  device.reset();owner->Retire();REQUIRE(owner->CloseNativeAfterIoDrained());
  auto next=disk.Open(0xCAFE0002,error);REQUIRE(next);REQUIRE_FALSE(error);
  const auto replacement=RegisterCacheRoles(registry,fs,next,4);
  REQUIRE(registry.Source(replacement[1].id)->available);REQUIRE(fs.FindSymbolicLink("cache0:",target));
  REQUIRE(target==R"(\CACHE0)");disk.Originals();
}
TEST_CASE("Stale and refused cache samples cannot overwrite a freshly committed actual source", "[cache_owner][content][concurrency]") {
  CacheDisk disk;ContentSourceRegistry registry;vfs::VirtualFileSystem fs;std::error_code error;
  auto owner=disk.Open(0xCAFE0001,error);REQUIRE(owner);REQUIRE_FALSE(error);const auto sources=RegisterCacheRoles(registry,fs,owner);
  std::promise<void> captured,release;auto arrived=captured.get_future();auto released=release.get_future().share();
  std::atomic<bool> once{false};
  SelectedMediaTimingAccess::Set(registry,[&](const std::string& id){
    if(id==sources[1].id && !once.exchange(true)){captured.set_value();released.wait();}
  });
  auto delayed=std::async(std::launch::async,[&]{return registry.Source(sources[1].id);});
  bool release_sent=false;
  struct ReleaseSample {std::promise<void>& signal;bool& sent;~ReleaseSample(){if(!sent)signal.set_value();}} release_on_unwind{release,release_sent};
  const bool reached=arrived.wait_for(std::chrono::seconds(5))==std::future_status::ready;
  std::unique_ptr<filesystem::CacheFormatHandoff> format;
  CacheOwner::Receipt completed;bool registered=false;std::optional<ContentSourceState> fresh;
  if(reached){
    format=owner->CaptureFormat(CacheRole::kCache0,8);
    if(format && !format->receipt().error)completed=CacheOwner::ExecuteFormat(*format);
    if(completed.complete)registered=registry.RegisterCache(sources[1],owner,CacheRole::kCache0,4,fs);
    if(registered)fresh=registry.Source(sources[1].id);
  }
  release.set_value();release_sent=true;const auto refused=delayed.get();SelectedMediaTimingAccess::Set(registry);
  REQUIRE(reached);REQUIRE(format);REQUIRE_FALSE(format->receipt().error);REQUIRE(completed.complete);REQUIRE(registered);
  REQUIRE(fresh);REQUIRE(fresh->available);REQUIRE(fresh->generation==4);REQUIRE(refused);REQUIRE_FALSE(refused->available);
  REQUIRE(refused->observation_error==std::errc::operation_canceled);
  // No fresh observation may repair the cache before these assertions: the
  // delayed old probe must have left the actual published G+1 row unchanged.
  REQUIRE(registry.CaptureStorageVolume(*fresh)==fresh->storage_volume);
  REQUIRE(fresh->storage_volume->available_cached());REQUIRE_FALSE(fresh->storage->retiring());
  {auto namespace_lock=global_critical_region::AcquireDirect();
    const auto denied=registry.Source(sources[1].id);REQUIRE(denied);REQUIRE_FALSE(denied->available);
    REQUIRE(denied->observation_error==std::errc::operation_would_block);}
  REQUIRE(registry.CaptureStorageVolume(*fresh)==fresh->storage_volume);
  auto* file=OpenCache(fs,R"(cache0:\after-real-format)",vfs::FileAccess::kGenericWrite,vfs::FileDisposition::kCreate);
  const std::array<uint8_t,2> bytes{0xA5,0x5A};size_t written=0;
  REQUIRE(file->WriteSync(bytes,0,&written)==X_STATUS_SUCCESS);REQUIRE(written==bytes.size());
  REQUIRE(file->Flush()==X_STATUS_SUCCESS);file->Destroy();disk.Originals();
}
#endif

}  // namespace xe::kernel::xam::test

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
TEST_CASE(
    "Media metadata owner inventories exact references and preserves corrupt "
    "intent",
    "[content][storage][selected_media][app_reference]") {
  Scratch scratch;
  std::filesystem::create_directory(scratch.root / "input");
  std::ofstream(scratch.root / "input/song") << "original";
  std::error_code error;
  auto parent = filesystem::HostStorageRoot::Open(scratch.root, false, error);
  REQUIRE(parent);
  auto recovery = filesystem::SnapshotRecoveryOwner::Open(parent);
  REQUIRE(recovery);
  REQUIRE_FALSE(recovery->error());
  auto metadata = filesystem::MediaReferenceStore::OpenMetadata(parent);
  REQUIRE(metadata);
  REQUIRE_FALSE(metadata->error());
  REQUIRE(metadata->root());
  auto input =
      filesystem::HostStorageRoot::Open(scratch.root / "input", true, error);
  REQUIRE(input);
  auto selection = filesystem::MediaSelection::CaptureFiles(
      {{"song", input, "song"}}, error);
  REQUIRE(selection);
  const auto saved = filesystem::MediaReferenceStore::Save(
      *metadata->root(), *recovery, "retained", 7, "Music", R"(\Device\Mass7)",
      *selection);
  REQUIRE_FALSE(saved.error);
  REQUIRE(saved.published);
  REQUIRE(saved.durable);
  auto rows = filesystem::MediaReferenceStore::List(*metadata->root(), error);
  REQUIRE_FALSE(error);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].id == "retained");
  CHECK(rows[0].members[0].root == input->path());
  {
    auto global = global_critical_region::AcquireDirect();
    CHECK(filesystem::MediaReferenceStore::List(*metadata->root(), error)
              .empty());
    CHECK(error);
  }
  // Uncooperating corruption of this fixture's own bytes is preserved, never
  // interpreted as an empty catalog or repaired by an ordinary Save.
  const auto leaf = filesystem::MediaReferenceStore::Leaf("retained");
  std::ofstream(metadata->root()->path() / leaf,
                std::ios::binary | std::ios::trunc)
      << "bad";
  CHECK(
      filesystem::MediaReferenceStore::List(*metadata->root(), error).empty());
  CHECK(error);
  const auto refused = filesystem::MediaReferenceStore::Save(
      *metadata->root(), *recovery, "retained", 7, "Music", R"(\Device\Mass7)",
      *selection);
  CHECK(refused.error);
  CHECK_FALSE(refused.published);
  CHECK(std::filesystem::file_size(metadata->root()->path() / leaf) == 3);
  CHECK(std::filesystem::file_size(scratch.root / "input/song") == 8);
}
TEST_CASE(
    "Removed media source waits actual uses before releasing its exact "
    "namespace",
    "[content][storage][selected_media][app_reference]") {
  Scratch scratch;
  std::ofstream(scratch.root / "song") << "data";
  std::ofstream(scratch.root / "other") << "unrelated";
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(scratch.root, true, error);
  REQUIRE(root);
  auto selection =
      filesystem::MediaSelection::CaptureFiles({{"song", root, "song"}}, error);
  REQUIRE(selection);
  ContentSourceRegistry registry;
  vfs::VirtualFileSystem fs;
  std::shared_ptr<const SelectedMediaPublication> publication;
  REQUIRE(registry.RegisterSelectedMedia(SelectedSource("first", 7), selection,
                                         1, fs, &publication));
  REQUIRE(publication);
  const auto current = registry.Source("first");
  REQUIRE(current);
  auto use = registry.CaptureSelectedMediaUse(*current);
  REQUIRE(use);
  vfs::File* raw = nullptr;
  vfs::FileAction action{};
  REQUIRE(fs.OpenFile(nullptr, R"(\Device\Mass7\song)",
                      vfs::FileDisposition::kOpen,
                      vfs::FileAccess::kFileReadData, false, true, &raw,
                      &action) == X_STATUS_SUCCESS);
  const auto destroy = [](vfs::File* file) {
    if (file) {
      file->Destroy();
    }
  };
  std::unique_ptr<vfs::File, decltype(destroy)> file(raw, destroy);
  REQUIRE(file);
  const auto actual_device = fs.AcquireDevice(R"(\Device\Mass7)");
  REQUIRE(actual_device);
  CHECK(actual_device->has_open_files());
  // A real receipt from another publication cannot target this generation.
  auto unrelated = filesystem::MediaSelection::CaptureFiles(
      {{"other", root, "other"}}, error);
  std::shared_ptr<const SelectedMediaPublication> foreign;
  REQUIRE(registry.RegisterSelectedMedia(SelectedSource("foreign", 8),
                                         unrelated, 1, fs, &foreign));
  const auto retained_files = root->open_file_count();
  REQUIRE(retained_files >= 2);
  CHECK_FALSE(registry.WithdrawSelectedMedia("first", 1, foreign));
  CHECK(registry.Source("first")->available);
  REQUIRE(registry.WithdrawSelectedMedia("first", 1, publication));
  CHECK_FALSE(registry.ForgetSelectedMedia("first", 1, fs, publication));
  CHECK(fs.AcquireDevice(R"(\Device\Mass7)"));
  CHECK_FALSE(registry.ForgetSelectedMedia("first", 2, fs));
  use.reset();
  // No SelectedMediaUse survives, but an actual ordinary File still does.
  CHECK_FALSE(registry.ForgetSelectedMedia("first", 1, fs, publication));
  CHECK(fs.AcquireDevice(R"(\Device\Mass7)") == actual_device);
  CHECK(actual_device->has_open_files());
  CHECK(root->open_file_count() == retained_files);
  CHECK_FALSE(registry.ForgetSelectedMedia("first", 1, fs, foreign));
  file.reset();
  CHECK_FALSE(actual_device->has_open_files());
  REQUIRE(registry.ForgetSelectedMedia("first", 1, fs, publication));
  CHECK(root->open_file_count() < retained_files);
  CHECK_FALSE(registry.Source("first"));
  CHECK_FALSE(fs.AcquireDevice(R"(\Device\Mass7)"));
  auto next =
      filesystem::MediaSelection::CaptureFiles({{"song", root, "song"}}, error);
  REQUIRE(next);
  REQUIRE(
      registry.RegisterSelectedMedia(SelectedSource("second", 7), next, 1, fs));
  CHECK(registry.Source("second")->available);
}
}  // namespace xe::kernel::xam::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
TEST_CASE(
    "Selected media registration returns its own receipt before reentrant "
    "replacement",
    "[content][storage][selected_media][app_reference]") {
  Scratch scratch;
  std::ofstream(scratch.root / "first") << "first";
  std::ofstream(scratch.root / "second") << "second";
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(scratch.root, true, error);
  REQUIRE(root);
  auto first = filesystem::MediaSelection::CaptureFiles(
      {{"song", root, "first"}}, error);
  auto second = filesystem::MediaSelection::CaptureFiles(
      {{"song", root, "second"}}, error);
  REQUIRE(first);
  REQUIRE(second);
  ContentSourceRegistry registry;
  vfs::VirtualFileSystem fs;
  const auto source = SelectedSource("receipt", 7);
  std::atomic<bool> attempted{false}, replaced{false};
  auto connection = registry.storage_notifications()->Connect(
      [&](const NativeContentStorageSnapshot& snapshot) {
        if (std::any_of(snapshot.devices.begin(), snapshot.devices.end(),
                        [&](const auto& row) {
                          return row.identity == source.id &&
                                 row.generation == 1 && row.available;
                        }) &&
            !attempted.exchange(true)) {
          // A genuine independent producer replaces the source from the real
          // postcommit notification cut before the initial producer returns.
          replaced.store(registry.RegisterSelectedMedia(source, second, 2, fs));
        }
      });
  REQUIRE(connection);
  std::shared_ptr<const SelectedMediaPublication> issued;
  REQUIRE(registry.RegisterSelectedMedia(source, first, 1, fs, &issued));
  connection->subscription->BeginStop();
  REQUIRE(connection->subscription->Drain());
  REQUIRE(attempted.load());
  REQUIRE(replaced.load());
  REQUIRE(issued);
  const auto current = registry.Source(source.id);
  REQUIRE(current);
  CHECK(current->available);
  CHECK(current->generation == 2);
  CHECK(current->selected_media == second);
  CHECK_FALSE(registry.OwnsSelectedMedia(*current, issued));
  CHECK_FALSE(registry.WithdrawSelectedMedia(source.id, 2, issued));
  // Asking for a fresh receipt cannot adopt an intervening compatible source.
  auto third = filesystem::MediaSelection::CaptureFiles(
      {{"song", root, "first"}}, error);
  REQUIRE(third);
  std::shared_ptr<const SelectedMediaPublication> refused;
  CHECK_FALSE(registry.RegisterSelectedMedia(source, third, 3, fs, &refused));
  CHECK_FALSE(refused);
  CHECK(registry.Source(source.id)->selected_media == second);
  CHECK(registry.Source(source.id)->available);
}
}  // namespace xe::kernel::xam::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
TEST_CASE(
    "Delayed media observers cannot recreate forgotten or replace live "
    "publication metadata",
    "[content][storage][selected_media][app_reference][concurrency]") {
  const auto observer =
      GENERATE(0, 1, 2);  // Source, Sources, real State::Refresh.
  const auto replacement =
      GENERATE(0, 1, 2);  // Absent, active ABA, unavailable.
  Scratch scratch;
  std::ofstream(scratch.root / "song") << "payload";
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(scratch.root, true, error);
  REQUIRE(root);
  auto first =
      filesystem::MediaSelection::CaptureFiles({{"song", root, "song"}}, error);
  REQUIRE(first);
  ContentSourceRegistry registry;
  vfs::VirtualFileSystem fs;
  const auto source = SelectedSource("delayed", 7);
  std::shared_ptr<const SelectedMediaPublication> original;
  REQUIRE(registry.RegisterSelectedMedia(source, first, 1, fs, &original));
  REQUIRE(original);
  struct SampleGate {
    const std::string source_id;
    std::promise<void> captured, release;
    std::future<void> arrived;
    std::shared_future<void> released;
    std::atomic<bool> once{false}, timed_out{false}, sent{false};
    explicit SampleGate(std::string id)
        : source_id(std::move(id)),
          arrived(captured.get_future()),
          released(release.get_future().share()) {}
    void Release() noexcept {
      if (!sent.exchange(true)) {
        try {
          release.set_value();
        } catch (...) {
          // The observer remains bounded even if fixture release itself fails.
          timed_out.store(true);
        }
      }
    }
  };
  auto gate = std::make_shared<SampleGate>(source.id);
  // A background sampler may already hold a copied callable after seam clear
  // and sink Drain. Every copy owns all gate state until its real return.
  SelectedMediaTimingAccess::Set(registry, [gate](const std::string& id) {
    if (id == gate->source_id && !gate->once.exchange(true)) {
      gate->captured.set_value();
      gate->timed_out.store(gate->released.wait_for(std::chrono::seconds(5)) !=
                            std::future_status::ready);
    }
  });
  struct Observation {
    std::optional<ContentSourceState> one;
    std::vector<ContentSourceState> many;
    std::optional<NativeContentStorageSource::Connection> connection;
  };
  // Declare the future before its release guard, so exception unwinding always
  // releases the real observing thread before future destruction can join it.
  std::future<Observation> delayed;
  struct ReleaseSample {
    std::shared_ptr<SampleGate> gate;
    ~ReleaseSample() { gate->Release(); }
  } release_on_unwind{gate};
  delayed = std::async(std::launch::async, [&, gate, observer,
                                            device_id = source.device_id] {
    Observation result;
    if (observer == 0) {
      result.one = registry.Source(gate->source_id);
    } else if (observer == 1) {
      result.many = registry.Sources(device_id);
    } else {
      // Connect really calls State::Refresh before creating its subscription;
      // no private refresh invocation or fabricated observation is used.
      result.connection =
          registry.storage_notifications()->Connect([](auto) {});
    }
    return result;
  });
  const bool reached = gate->arrived.wait_for(std::chrono::seconds(5)) ==
                       std::future_status::ready;
  bool withdrew = false, forgot = false, registered = false;
  std::shared_ptr<filesystem::MediaSelection> next;
  std::shared_ptr<const SelectedMediaPublication> current_receipt;
  std::optional<ContentSourceState> fresh;
  if (reached) {
    withdrew = registry.WithdrawSelectedMedia(source.id, 1, original);
    if (withdrew) {
      forgot = registry.ForgetSelectedMedia(source.id, 1, fs, original);
    }
    if (forgot && replacement == 1) {
      next = filesystem::MediaSelection::CaptureFiles({{"song", root, "song"}},
                                                      error);
      if (next) {
        registered = registry.RegisterSelectedMedia(source, next, 1, fs,
                                                    &current_receipt);
      }
    } else if (forgot && replacement == 2) {
      registered =
          registry.RegisterUnavailableSelectedMedia(source, &current_receipt);
    }
    if (registered) {
      fresh = registry.Source(source.id);
    }
  }
  gate->Release();
  auto observed = delayed.get();
  SelectedMediaTimingAccess::Set(registry);
  bool drained = true;
  if (observed.connection) {
    observed.connection->subscription->BeginStop();
    drained = observed.connection->subscription->Drain();
  }
  REQUIRE(reached);
  REQUIRE_FALSE(gate->timed_out.load());
  REQUIRE(withdrew);
  REQUIRE(forgot);
  REQUIRE(drained);
  if (observer == 2) {
    REQUIRE(observed.connection);
  }
  if (replacement == 0) {
    if (observer == 0) {
      CHECK_FALSE(observed.one);
    }
    if (observer == 1) {
      CHECK(observed.many.empty());
    }
    if (observer == 2) {
      CHECK_FALSE(FindVolume(observed.connection->initial, source.id));
    }
    CHECK_FALSE(registry.Source(source.id));
  } else {
    REQUIRE(registered);
    REQUIRE(current_receipt);
    REQUIRE(fresh);
    // Check exact cached ownership before any fresh IO could repair a row.
    CHECK(registry.OwnsSelectedMedia(*fresh, current_receipt));
    CHECK(registry.SelectedMediaAvailable(source.id, current_receipt) ==
          (replacement == 1));
    if (observer != 2) {
      const ContentSourceState* stale = nullptr;
      if (observer == 0) {
        REQUIRE(observed.one);
        stale = &*observed.one;
      } else {
        REQUIRE(observed.many.size() == 1);
        stale = &observed.many.front();
      }
      CHECK_FALSE(stale->available);
      CHECK(stale->observation_error == std::errc::operation_canceled);
      CHECK_FALSE(registry.CaptureSelectedMedia(*stale));
    } else {
      const auto* projected =
          FindVolume(observed.connection->initial, source.id);
      REQUIRE(projected);
      CHECK(projected->available == (replacement == 1));
      CHECK(projected->generation == (replacement == 1 ? 1 : 0));
    }
    if (replacement == 1) {
      CHECK(registry.CaptureSelectedMedia(*fresh) == next);
      REQUIRE(registry.WithdrawSelectedMedia(source.id, 1, current_receipt));
    }
    REQUIRE(registry.ForgetSelectedMedia(source.id, replacement == 1 ? 1 : 0,
                                         fs, current_receipt));
  }
  // Absent-source observations must not leave an ownerless map entry; every
  // path can really register the same ID/generation again after actual forget.
  auto restored =
      filesystem::MediaSelection::CaptureFiles({{"song", root, "song"}}, error);
  REQUIRE(restored);
  std::shared_ptr<const SelectedMediaPublication> restored_receipt;
  REQUIRE(registry.RegisterSelectedMedia(source, restored, 1, fs,
                                         &restored_receipt));
  CHECK(registry.SelectedMediaAvailable(source.id, restored_receipt));
  CHECK(std::filesystem::file_size(scratch.root / "song") == 7);
}
}  // namespace xe::kernel::xam::test
#endif
