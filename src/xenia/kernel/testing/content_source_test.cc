/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include "xenia/kernel/xam/content_source.h"
#include "xenia/base/host_storage.h"
#include "xenia/kernel/native_content_storage_source.h"

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

}  // namespace xe::kernel::xam::test
