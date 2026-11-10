/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_TESTING_SNAPSHOT_FIXTURE_H_
#define XENIA_BASE_TESTING_SNAPSHOT_FIXTURE_H_
#include <array>
#include <filesystem>
#include <random>
#include <string>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/snapshot_publisher.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_LINUX
#include <sys/stat.h>
#include <unistd.h>
namespace xe::filesystem::test {
struct SnapshotScratch {
  std::filesystem::path path;
  bool owned = false;
  SnapshotScratch() {
    std::random_device random;
    for (unsigned i = 0; i != 16; ++i) {
      path = std::filesystem::temp_directory_path() /
          ("xenia-snapshot-" + std::to_string(random()) + "-" +
           std::to_string(random()));
      if (std::filesystem::create_directory(path)) { owned = true; break; }
    }
    REQUIRE(owned);
  }
  ~SnapshotScratch() {
    if (owned) { std::error_code error; std::filesystem::remove_all(path, error); }
  }
  std::shared_ptr<HostStorageRoot> Root() {
    std::error_code error;
    // Genuine filesystem authority with the applicable shared FS policy.
    // Qualification pins its project FS via test-suite bootstrap; a separate
    // small external volume does not acquire a fabricated 20GiB reserve.
    auto root = HostStorageRoot::Open(path, false, error);
    REQUIRE_FALSE(error);
    REQUIRE(root);
    return root;
  }
};
struct SnapshotFixture {
  SnapshotScratch scratch;
  std::shared_ptr<HostStorageRoot> parent;
  std::unique_ptr<SnapshotRecoveryOwner> owner;
  std::shared_ptr<HostStorageRoot> profile;
  SnapshotFixture() {
    parent = scratch.Root();
    owner = SnapshotRecoveryOwner::Open(parent);
    REQUIRE_FALSE(owner->error());
    REQUIRE(owner->root());
    REQUIRE_FALSE(parent->Create("profile", true));
    std::error_code error;
    auto raw = parent->Subroot("profile", false, error);
    REQUIRE(raw);
    profile = raw->GuestView(error);
    REQUIRE_FALSE(error);
    REQUIRE(profile);
  }
  std::shared_ptr<const HostStorageSnapshotToken> Token(
      const std::filesystem::path& leaf, uint64_t maximum = 4096) {
    std::error_code error;
    auto token = profile->CaptureSnapshot(leaf, maximum, error);
    REQUIRE_FALSE(error);
    REQUIRE(token);
    return token;
  }
  std::vector<uint8_t> Bytes(const std::filesystem::path& leaf) {
    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(profile->ReadFile(leaf, 4096, bytes));
    return bytes;
  }
};
}  // namespace xe::filesystem::test
#endif
#endif
