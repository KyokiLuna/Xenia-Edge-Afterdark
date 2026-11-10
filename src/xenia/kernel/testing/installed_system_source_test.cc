/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include "xenia/vfs/installed_system_source.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
namespace {
using Source = vfs::InstalledSystemSource;
struct ImageFixture {
  std::filesystem::path root;
  ImageFixture() {
    std::random_device rng;
    for (unsigned i = 0; i < 16; ++i) {
      auto next = std::filesystem::temp_directory_path() /
                  ("xenia_installed_image_" + std::to_string(rng()) + "_" +
                   std::to_string(rng()));
      if (std::filesystem::create_directory(next)) {
        root = std::filesystem::canonical(next);
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
  }
  ~ImageFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  void Install(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path / "flash");
    std::filesystem::create_directories(path / "sysext" / "Content");
    std::filesystem::create_directories(path / "sysaux");
    for (auto name : {"flash/dash.xex", "sysext/Content/probe"}) {
      std::ofstream stream(path / name, std::ios::binary);
      stream.write(bytes.data(), bytes.size());
      REQUIRE(stream.good());
    }
  }
};
ContentSource Definition(const Source::Snapshot& image) {
  return {"installed-system",
          1,
          DeviceType::HDD,
          ContentSourceKind::kInstalledSystem,
          image.path() / "sysext" / "Content",
          "\\Device\\Harddisk0\\SystemExtPartition\\Content",
          u"System Content",
          true,
          0,
          "\\Device\\Harddisk0\\SystemExtPartition"};
}
void Bind(ContentSourceRegistry& registry,
          const std::shared_ptr<const Source::Snapshot>& image) {
  std::error_code error;
  auto content = image->ContentRoot(error);
  REQUIRE((!error || error == std::errc::no_such_file_or_directory));
  REQUIRE(registry.RegisterReadOnly(Definition(*image), content,
                                    image->generation()));
}
std::string Read(const std::shared_ptr<filesystem::HostStorageRoot>& root,
                 const std::filesystem::path& path) {
  REQUIRE(root);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(root->ReadFile(path, 64, bytes));
  return {bytes.begin(), bytes.end()};
}
void Mount(vfs::VirtualFileSystem& fs, std::string_view name,
           std::shared_ptr<filesystem::HostStorageRoot> root,
           bool requested_read_only = true) {
  const auto path = root->path();
  auto device = std::make_unique<vfs::HostPathDevice>(
      name, path, requested_read_only, false, "FATX", std::move(root));
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
}
struct OpenFile {
  vfs::File* file = nullptr;
  ~OpenFile() {
    if (file) {
      file->Destroy();
    }
  }
};
}  // namespace

TEST_CASE("Local installed source can be installed after a missing setup",
          "[content][storage][installed-system]") {
  ImageFixture fixture;
  const auto local = fixture.root / "local-system";
  std::error_code error;
  const auto source = Source::Create(local, {}, error);
  REQUIRE(source);
  REQUIRE_FALSE(error);
  const auto missing = source->snapshot();
  REQUIRE(missing->valid());
  REQUIRE_FALSE(missing->available());
  REQUIRE_FALSE(std::filesystem::exists(local));
  ContentSourceRegistry registry;
  Bind(registry, missing);
  REQUIRE_FALSE(registry.Source("installed-system")->available);
  fixture.Install(local, "first");
  REQUIRE_FALSE(registry.Source("installed-system")->available);
  const auto ready = source->PrepareRefresh(error, true);
  REQUIRE(ready);
  REQUIRE(ready->generation() == missing->generation() + 1);
  REQUIRE(source->Commit(ready, error));
  Bind(registry, ready);
  REQUIRE(registry.Source("installed-system")->available);
  REQUIRE(registry.Source("installed-system")->generation ==
          ready->generation());
  REQUIRE_FALSE(registry.WritableSource(1));
  REQUIRE_FALSE(registry.Resolve("installed-system", "probe", true));
  REQUIRE(ready->DashboardPath(error) == local / "flash" / "dash.xex");
  REQUIRE(Read(registry.Source("installed-system")->storage, "probe") ==
          "first");
}

TEST_CASE(
    "Installed partitions package mounts and catalog retain read-only rights",
    "[content][storage][installed-system]") {
  ImageFixture fixture;
  const auto local = fixture.root / "system";
  fixture.Install(local, "image");
  std::error_code error;
  const auto source = Source::Create(local, {}, error);
  REQUIRE(source);
  const auto image = source->snapshot();
  const auto flash = image->PartitionRoot(Source::Partition::kFlash, error);
  const auto package = image->DirectoryRoot(local / "flash", error);
  const auto auxiliary =
      image->PartitionRoot(Source::Partition::kSystemAux, error);
  REQUIRE(flash);
  REQUIRE(package);
  REQUIRE(auxiliary);
  REQUIRE(flash->read_only());
  REQUIRE(package->read_only());
  REQUIRE(auxiliary->Create("new-file", false));
  REQUIRE(flash->Remove("dash.xex", false));
  REQUIRE(flash->Rename("dash.xex", "moved", false));
  vfs::VirtualFileSystem fs;
  Mount(fs, "\\Device\\Flash", flash);
  // Simulate allow_game_relative_writes=true: the owned cap still wins.
  Mount(fs, "\\Device\\Package_0", package, false);
  OpenFile file;
  vfs::FileAction action;
  REQUIRE(fs.OpenFile(nullptr, "\\Device\\Package_0\\dash.xex",
                      vfs::FileDisposition::kOpen,
                      filesystem::FileAccess::kGenericRead, false, true,
                      &file.file, &action) == X_STATUS_SUCCESS);
  std::array<uint8_t, 5> bytes{};
  size_t transferred = 0;
  REQUIRE(file.file->ReadSync(bytes, 0, &transferred) == X_STATUS_SUCCESS);
  REQUIRE(transferred == bytes.size());
  REQUIRE(std::string(bytes.begin(), bytes.end()) == "image");
  REQUIRE_FALSE(fs.UnregisterDevice("\\Device\\Package_0"));
  OpenFile denied;
  REQUIRE(fs.OpenFile(nullptr, "\\Device\\Package_0\\dash.xex",
                      vfs::FileDisposition::kOverwrite,
                      filesystem::FileAccess::kGenericWrite, false, true,
                      &denied.file, &action) == X_STATUS_ACCESS_DENIED);
  REQUIRE_FALSE(fs.DeletePath("\\Device\\Flash\\dash.xex"));
  REQUIRE_FALSE(fs.CreatePath("\\Device\\Flash\\new", 0));
  REQUIRE(fs.RetireDevice("\\Device\\Package_0"));
  REQUIRE(file.file->ReadSync(bytes, 0, &transferred) == X_STATUS(0xC000009D));
  REQUIRE(Read(flash, "dash.xex") == "image");
}

TEST_CASE("Explicit shared image never redirects the local installer",
          "[content][storage][installed-system]") {
  ImageFixture fixture;
  const auto shared = fixture.root / "shared";
  const auto local = fixture.root / "local";
  fixture.Install(shared, "shared");
  std::error_code error;
  const auto source = Source::Create(local, shared, error);
  REQUIRE(source);
  const auto image = source->snapshot();
  REQUIRE(image->shared());
  REQUIRE_FALSE(std::filesystem::exists(local));
  fixture.Install(local, "local");
  REQUIRE(source->PrepareRefresh(error, true) == image);
  REQUIRE(source->Commit(image, error));
  REQUIRE(image->DashboardPath(error) == shared / "flash" / "dash.xex");
  REQUIRE(Read(image->DirectoryRoot(shared / "flash", error), "dash.xex") ==
          "shared");
  const auto installer =
      filesystem::HostStorageRoot::Open(local, false, error, 0);
  REQUIRE(installer);
  REQUIRE_FALSE(installer->Create("installer-owned", false));
  REQUIRE(std::filesystem::exists(local / "installer-owned"));
  REQUIRE_FALSE(std::filesystem::exists(shared / "installer-owned"));
  REQUIRE_FALSE(image->DirectoryRoot(local / "flash", error));
  REQUIRE(error == std::errc::permission_denied);
}

TEST_CASE("Invalid shared sources fail without creating or falling back",
          "[content][storage][installed-system]") {
  ImageFixture fixture;
  const auto local = fixture.root / "local";
  fixture.Install(local, "local");
  std::error_code error;
  REQUIRE_FALSE(Source::Create(local, fixture.root / "missing", error));
  REQUIRE(error == std::errc::no_such_file_or_directory);
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "missing"));
  REQUIRE_FALSE(Source::Create(local, local, error));
  REQUIRE(error == std::errc::invalid_argument);
  REQUIRE_FALSE(Source::Create(local, fixture.root, error));
  REQUIRE(error == std::errc::invalid_argument);
  std::filesystem::create_directory(fixture.root / "not-an-image");
  REQUIRE_FALSE(Source::Create(local, fixture.root / "not-an-image", error));
  REQUIRE(error == std::errc::no_such_file_or_directory);
  std::filesystem::create_directory_symlink(local, fixture.root / "alias");
  REQUIRE_FALSE(Source::Create(fixture.root / "other-local",
                               fixture.root / "alias", error));
  REQUIRE(error == std::errc::permission_denied);
  const auto source = Source::Create(local, {}, error);
  REQUIRE(source);
  REQUIRE(
      Read(source->snapshot()->PartitionRoot(Source::Partition::kFlash, error),
           "dash.xex") == "local");
}

TEST_CASE(
    "Installed local generations replace only at an explicit safe boundary",
    "[content][storage][installed-system]") {
  ImageFixture fixture;
  const auto local = fixture.root / "system";
  fixture.Install(local, "old");
  std::error_code error;
  const auto source = Source::Create(local, {}, error);
  REQUIRE(source);
  const auto old = source->snapshot();
  ContentSourceRegistry registry;
  Bind(registry, old);
  const auto old_content = registry.Source("installed-system")->storage;
  std::filesystem::rename(local, fixture.root / "old-system");
  fixture.Install(local, "new");
  REQUIRE_FALSE(old->valid());
  REQUIRE_FALSE(registry.Source("installed-system")->available);
  REQUIRE_FALSE(registry.Resolve("installed-system", "probe", false));
  const auto next = source->PrepareRefresh(error);
  REQUIRE(next);
  REQUIRE(next->generation() == old->generation() + 1);
  REQUIRE(source->Commit(next, error));
  Bind(registry, next);
  REQUIRE(Read(registry.Source("installed-system")->storage, "probe") == "new");
  std::vector<uint8_t> bytes;
  REQUIRE(old_content->ReadFile("probe", 64, bytes));
  REQUIRE_FALSE(
      registry.RegisterReadOnly(Definition(*old), {}, old->generation()));
  REQUIRE_FALSE(source->Commit(old, error));
  const auto alien = Source::Create(fixture.root / "alien", {}, error);
  REQUIRE(alien);
  REQUIRE_FALSE(source->Commit(alien->snapshot(), error));
}

TEST_CASE("A replaced explicit shared image is never silently adopted",
          "[content][storage][installed-system]") {
  ImageFixture fixture;
  const auto shared = fixture.root / "shared";
  fixture.Install(shared, "first");
  std::error_code error;
  const auto source = Source::Create(fixture.root / "local", shared, error);
  REQUIRE(source);
  const auto image = source->snapshot();
  ContentSourceRegistry registry;
  Bind(registry, image);
  std::filesystem::rename(shared, fixture.root / "retained-shared");
  fixture.Install(shared, "replacement");
  REQUIRE_FALSE(image->valid());
  REQUIRE_FALSE(source->PrepareRefresh(error, true));
  REQUIRE(error == std::errc::no_such_device);
  REQUIRE_FALSE(registry.Source("installed-system")->available);
  REQUIRE_FALSE(image->DirectoryRoot(shared / "flash", error));
  REQUIRE(source->snapshot() == image);
}
TEST_CASE("Host aliases to installed executables cannot widen package rights",
          "[content][storage][installed-system]") {
  ImageFixture fixture;
  const auto shared = fixture.root / "shared";
  fixture.Install(shared, "source");
  std::error_code error;
  const auto source = Source::Create(fixture.root / "local", shared, error);
  REQUIRE(source);
  const auto image = source->snapshot();
  const auto alias = fixture.root / "outside-alias";
  std::filesystem::create_directory_symlink(shared / "flash", alias);
  // These are the actual production classification and owned-root methods,
  // followed by a real Package_0 mount with relative writes requested.
  REQUIRE(image->Contains(alias / "dash.xex"));
  const auto directory = image->DirectoryRoot(alias, error);
  REQUIRE(directory);
  REQUIRE(directory->path() == shared / "flash");
  vfs::VirtualFileSystem fs;
  Mount(fs, "\\Device\\Package_0", directory, false);
  OpenFile denied;
  vfs::FileAction action;
  REQUIRE(fs.OpenFile(nullptr, "\\Device\\Package_0\\dash.xex",
                      vfs::FileDisposition::kOverwrite,
                      filesystem::FileAccess::kGenericWrite, false, true,
                      &denied.file, &action) == X_STATUS_ACCESS_DENIED);
  REQUIRE(Read(directory, "dash.xex") == "source");
  const auto loose_alias = fixture.root / "loose.xex";
  std::filesystem::create_symlink(shared / "flash" / "dash.xex", loose_alias);
  REQUIRE(image->Contains(loose_alias));
  // A file-only alias whose containing directory is outside fails closed;
  // it is not sent to the ordinary writable title-root fallback.
  REQUIRE_FALSE(image->DirectoryRoot(loose_alias.parent_path(), error));
  REQUIRE(error == std::errc::permission_denied);
}

TEST_CASE(
    "System installation excludes pending recursive and detached launches",
    "[content][storage][installed-system]") {
  vfs::InstalledSystemOperationGate gate;
  auto preparing = gate.TryLaunch();
  REQUIRE(preparing);
  REQUIRE_FALSE(gate.TryInstall());
  auto nested = gate.TryLaunch();
  REQUIRE(nested);
  preparing.reset();
  REQUIRE_FALSE(gate.TryInstall());
  nested.reset();
  auto installation = gate.TryInstall();
  REQUIRE(installation);
  REQUIRE(gate.installing());
  REQUIRE_FALSE(gate.TryInstall());
  REQUIRE_FALSE(gate.TryLaunch());
  // Moving a reservation transfers release authority, never releases early.
  auto moved = std::move(installation);
  installation.reset();
  REQUIRE(gate.installing());
  REQUIRE_FALSE(gate.TryLaunch());
  moved.reset();
  REQUIRE_FALSE(gate.installing());
  auto background = gate.TryLaunch();
  REQUIRE(background);
  std::promise<void> entered, release;
  auto released = release.get_future();
  std::thread worker([token = std::move(background), &entered,
                      released = std::move(released)]() mutable {
    (void)token;
    entered.set_value();
    released.wait();
  });
  entered.get_future().wait();
  const bool blocked = !gate.TryInstall();
  release.set_value();
  worker.join();
  REQUIRE(blocked);
  REQUIRE(gate.TryInstall());
}

TEST_CASE(
    "System operation reservations retain state past their initiating owner",
    "[content][storage][installed-system]") {
  auto gate = std::make_unique<vfs::InstalledSystemOperationGate>();
  auto installation = gate->TryInstall();
  REQUIRE(installation);
  gate.reset();
  // The State outlives the front end; cancellation/destruction has no raw
  // Emulator pointer or mutex borrowed from the initiating stack frame.
  installation.reset();
  SUCCEED("reservation rundown completed after gate destruction");
}
}  // namespace xe::kernel::xam::test
#endif
