/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#include "xenia/kernel/console_data_owner.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace xe::kernel::test {
#if XE_PLATFORM_LINUX
namespace {
using filesystem::HostStorageRoot;
constexpr ConsoleSettingKey kRetail{3, 0xC};
struct ConsoleScratch {
  std::filesystem::path path;
  ConsoleScratch() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-console-owner-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~ConsoleScratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path Private() const {
    return path / ConsoleDataOwner::kDirectory;
  }
  std::shared_ptr<HostStorageRoot> Root() {
    std::error_code error;
    auto root = HostStorageRoot::Open(path, false, error, 0);
    REQUIRE(root);
    REQUIRE_FALSE(error);
    return root;
  }
};
std::vector<uint8_t> Read(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  file.write(bytes.data(), bytes.size());
  file.close();
  REQUIRE_FALSE(file.fail());
}
} // namespace
TEST_CASE("Console owner persists settings through VFS clear and reopen",
          "[console_owner][persistence]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(owner->error());
  REQUIRE(owner->private_root());
  REQUIRE(owner->settings());
  const auto update = owner->settings()->WriteU32(kRetail, 0x80123456);
  REQUIRE_FALSE(update.error);
  REQUIRE(update.published);
  REQUIRE(update.durable);
  const auto before = Read(scratch.Private() / "xconfig.bin");
  REQUIRE(std::filesystem::create_directory(scratch.path / "hdd"));
  vfs::VirtualFileSystem vfs;
  for (int title = 0; title < 2; ++title) {
    auto device = std::make_unique<vfs::HostPathDevice>(
        "hdd:", scratch.path / "hdd", false);
    REQUIRE(device->Initialize());
    REQUIRE(vfs.RegisterDevice(std::move(device)));
    vfs.Clear();
    uint32_t flags = 0;
    REQUIRE_FALSE(owner->settings()->ReadU32(kRetail, 0, flags));
    REQUIRE(flags == 0x80123456);
    std::error_code error;
    REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
        std::vector<filesystem::GuestHostBacking>{{scratch.path, true}},
        error));
    REQUIRE(error == std::errc::permission_denied);
  }
  owner.reset();
  owner = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(owner->error());
  uint32_t flags = 0;
  REQUIRE_FALSE(owner->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0x80123456);
  REQUIRE(Read(scratch.Private() / "xconfig.bin") == before);
}
TEST_CASE("Unknown or interrupted console bootstrap is preserved and rejected",
          "[console_owner][recovery]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  REQUIRE(std::filesystem::create_directory(scratch.Private()));
  SECTION("empty preexisting directory is not adopted") {}
  SECTION("foreign bytes are not replaced") {
    Write(scratch.Private() / ConsoleDataOwner::kMarker, "unknown owner");
    Write(scratch.Private() / "xconfig.bin", "preserve me");
  }
  const bool had_marker =
      std::filesystem::exists(scratch.Private() / ConsoleDataOwner::kMarker);
  const auto marker = had_marker
                          ? Read(scratch.Private() / ConsoleDataOwner::kMarker)
                          : std::vector<uint8_t>{};
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error());
  REQUIRE_FALSE(owner->settings());
  REQUIRE_FALSE(owner->private_root());
  REQUIRE(std::filesystem::exists(scratch.Private() /
                                  ConsoleDataOwner::kMarker) == had_marker);
  if (had_marker) {
    REQUIRE(Read(scratch.Private() / ConsoleDataOwner::kMarker) == marker);
    REQUIRE((Read(scratch.Private() / "xconfig.bin") ==
             std::vector<uint8_t>{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                                  'm', 'e'}));
  }
  REQUIRE_FALSE(std::filesystem::exists(scratch.Private() / "xconfig.lock"));
}
TEST_CASE("Console owner rejects linked directory without altering target",
          "[console_owner][confinement]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  REQUIRE(std::filesystem::create_directory(scratch.path / "other"));
  Write(scratch.path / "other" / "keep", "host original");
  std::filesystem::create_directory_symlink(scratch.path / "other",
                                            scratch.Private());
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error());
  REQUIRE_FALSE(owner->settings());
  REQUIRE(std::filesystem::is_symlink(scratch.Private()));
  REQUIRE(
      std::distance(std::filesystem::directory_iterator(scratch.path / "other"),
                    std::filesystem::directory_iterator()) == 1);
  REQUIRE((Read(scratch.path / "other" / "keep") ==
           std::vector<uint8_t>{'h', 'o', 's', 't', ' ', 'o', 'r', 'i', 'g',
                                'i', 'n', 'a', 'l'}));
}
TEST_CASE("Corrupt console store remains protected without empty replacement",
          "[console_owner][recovery]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  {
    auto owner = ConsoleDataOwner::Open(root);
    REQUIRE_FALSE(owner->error());
    REQUIRE_FALSE(owner->settings()->WriteU32(kRetail, 7).error);
  }
  Write(scratch.Private() / "xconfig.bin", "damaged database");
  const auto before = Read(scratch.Private() / "xconfig.bin");
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error() == std::errc::bad_message);
  REQUIRE_FALSE(owner->settings());
  REQUIRE(owner->private_root());
  std::error_code error;
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<filesystem::GuestHostBacking>{{scratch.path, true}}, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE(Read(scratch.Private() / "xconfig.bin") == before);
}
TEST_CASE("Console owner cannot follow a replaced directory generation",
          "[console_owner][lifetime]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(owner->error());
  REQUIRE_FALSE(owner->settings()->WriteU32(kRetail, 42).error);
  const auto bytes = Read(scratch.Private() / "xconfig.bin");
  std::filesystem::rename(scratch.Private(), scratch.path / "retained");
  REQUIRE(std::filesystem::create_directory(scratch.Private()));
  uint32_t flags = 0xDEADBEEF;
  REQUIRE(owner->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0xDEADBEEF);
  auto next = ConsoleDataOwner::Open(root);
  REQUIRE(next->error());
  REQUIRE_FALSE(next->settings());
  REQUIRE(std::filesystem::is_empty(scratch.Private()));
  REQUIRE(Read(scratch.path / "retained" / "xconfig.bin") == bytes);
}
TEST_CASE("Console bootstrap obeys space admission and preserves parent errors",
          "[console_owner][admission]") {
  ConsoleScratch scratch;
  std::error_code error;
  auto space = filesystem::HostStorageSpace::ForPath(
      scratch.path, filesystem::kHostStorageReserve, error,
      [](const auto &, std::error_code &ec) {
        ec.clear();
        return std::filesystem::space_info{40ull << 30, 20ull << 30,
                                           20ull << 30};
      });
  REQUIRE(space);
  auto root = HostStorageRoot::Open(scratch.path, false, error,
                                    filesystem::kHostStorageReserve, space);
  REQUIRE(root);
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error() == std::errc::no_space_on_device);
  REQUIRE_FALSE(owner->settings());
  REQUIRE_FALSE(std::filesystem::exists(scratch.Private()));
  const auto original = std::make_error_code(std::errc::read_only_file_system);
  owner = ConsoleDataOwner::Open({}, original);
  REQUIRE(owner->error() == original);
  REQUIRE_FALSE(owner->private_root());
}
TEST_CASE("Two console owners use the same durable authority",
          "[console_owner][persistence]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  auto first = ConsoleDataOwner::Open(root);
  auto second = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(first->error());
  REQUIRE_FALSE(second->error());
  REQUIRE_FALSE(first->settings()->WriteU32(kRetail, 0xABCDEF01).error);
  uint32_t flags = 0;
  REQUIRE_FALSE(second->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0xABCDEF01);
  const auto update = second->settings()->ModifyU32(kRetail, 0, ~0xFFu, 0x57);
  REQUIRE_FALSE(update.error);
  REQUIRE(update.durable);
  REQUIRE_FALSE(first->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0xABCDEF57);
}
#else
TEST_CASE("Console owner records unsupported protection without creating data",
          "[console_owner]") {
  auto owner = ConsoleDataOwner::Open({});
  REQUIRE(owner->error() == std::errc::operation_not_supported);
  REQUIRE_FALSE(owner->private_root());
  REQUIRE_FALSE(owner->settings());
  WARN("UNVERIFIED: console authority mount protection on this platform");
}
#endif
} // namespace xe::kernel::test
