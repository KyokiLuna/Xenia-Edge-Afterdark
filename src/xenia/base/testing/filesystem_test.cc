/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/platform.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

using xe::filesystem::FileInfo;

namespace {

// A new directory of our own under the system temp directory, removed with
// the test. The name is random so concurrent runs do not share or delete each
// other's directories.
class ScratchDir {
 public:
  ScratchDir() {
    std::error_code ec;
    auto temp = std::filesystem::temp_directory_path(ec);
    std::random_device rng;
    for (int attempt = 0; attempt < 16; ++attempt) {
      auto candidate =
          temp / ("xenia_filesystem_test_" + std::to_string(rng()) + "_" +
                  std::to_string(rng()));
      // create_directory returns false if the directory already existed.
      if (std::filesystem::create_directory(candidate, ec)) {
        path_ = std::move(candidate);
        return;
      }
    }
  }
  ~ScratchDir() {
    if (path_.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

const FileInfo* Find(const std::vector<FileInfo>& infos,
                     const std::string& name) {
  for (const auto& info : infos) {
    if (info.name == name) {
      return &info;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("List Files", "[filesystem]") {
  ScratchDir scratch;
  REQUIRE(std::filesystem::exists(scratch.path()));

  std::error_code ec;
  std::filesystem::create_directory(scratch.path() / "a_directory", ec);
  REQUIRE(!ec);
  {
    std::ofstream file(scratch.path() / "a_file");
    file << "1234";
  }

  SECTION("a directory and a file are told apart") {
    auto infos = xe::filesystem::ListFiles(scratch.path());
    const auto* dir = Find(infos, "a_directory");
    const auto* file = Find(infos, "a_file");
    REQUIRE(dir);
    REQUIRE(file);
    REQUIRE(dir->type == FileInfo::Type::kDirectory);
    REQUIRE(dir->total_size == 0);
    REQUIRE(file->type == FileInfo::Type::kFile);
    REQUIRE(file->total_size == 4);
  }

#if !XE_PLATFORM_WIN32
  SECTION("a symlinked directory is a directory") {
    // dirent::d_type reports DT_LNK here, so classifying from it made a
    // symlinked directory look like a file.
    std::filesystem::create_directory_symlink(
        scratch.path() / "a_directory", scratch.path() / "linked_directory",
        ec);
    REQUIRE(!ec);
    auto infos = xe::filesystem::ListFiles(scratch.path());
    const auto* link = Find(infos, "linked_directory");
    REQUIRE(link);
    REQUIRE(link->type == FileInfo::Type::kDirectory);
  }

  SECTION("a dangling symlink reports the link, not a garbage size") {
    // stat fails on it, and its return used to be discarded, leaving the
    // struct indeterminate and feeding the size and the timestamps from it.
    // The lstat fallback describes the link itself, whose size is the length
    // of the path it holds.
    const auto target = scratch.path() / "not_here";
    std::filesystem::create_symlink(target, scratch.path() / "dangling", ec);
    REQUIRE(!ec);
    auto infos = xe::filesystem::ListFiles(scratch.path());
    const auto* dangling = Find(infos, "dangling");
    REQUIRE(dangling);
    REQUIRE(dangling->type == FileInfo::Type::kFile);
    REQUIRE(dangling->total_size == target.string().size());
  }
#endif  // !XE_PLATFORM_WIN32
}

}  // namespace xe::base::test

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
#include <sys/stat.h>
#include <unistd.h>
namespace xe::base::test {
namespace {
uint64_t ObservedNativeAllocation(const filesystem::FileHandle& file) {
  struct stat value{};
  REQUIRE(::fstat(int(file.native_handle()), &value) == 0);
  REQUIRE(S_ISREG(value.st_mode));
  REQUIRE(value.st_blocks >= 0);
  REQUIRE(uint64_t(value.st_blocks) <= UINT64_MAX / 512);
  return uint64_t(value.st_blocks) * 512;
}
}  // namespace
TEST_CASE("Native allocation observes real sparse extents independently of EOF",
          "[filesystem][allocation][retained]") {
  ScratchDir scratch;
  const auto path = scratch.path() / "allocation";
  {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.good());
  }
  auto file = filesystem::FileHandle::OpenExisting(
      path, filesystem::FileAccess::kGenericAll);
  REQUIRE(file);
  std::error_code error;
  auto allocated = file->QueryAllocatedBytes(error);
  REQUIRE_FALSE(error);
  REQUIRE(allocated);
  REQUIRE(*allocated == ObservedNativeAllocation(*file));
  constexpr size_t size = 8 * 1024 * 1024;
  REQUIRE_FALSE(file->ResizeResult(size));
  allocated = file->QueryAllocatedBytes(error);
  REQUIRE_FALSE(error);
  REQUIRE(allocated);
  REQUIRE(*allocated == ObservedNativeAllocation(*file));
  REQUIRE(*allocated == 0);  // Sparse zero is not an unsupported result.
  auto info = file->QueryInfo(error);
  REQUIRE(info);
  REQUIRE_FALSE(error);
  REQUIRE(info->size == size);
  const std::array<uint8_t, 3> bytes{0xA5, 0, 0x69};
  const auto written = file->WriteAllResult(size - bytes.size(), bytes);
  REQUIRE_FALSE(written.error);
  REQUIRE(written.bytes == bytes.size());
  REQUIRE_FALSE(file->FlushResult());
  allocated = file->QueryAllocatedBytes(error);
  REQUIRE_FALSE(error);
  REQUIRE(allocated);
  REQUIRE(*allocated == ObservedNativeAllocation(*file));
  REQUIRE(*allocated < size);
  std::array<uint8_t, 3> read{};
  const auto result = file->ReadResult(size - bytes.size(), read);
  REQUIRE_FALSE(result.error);
  REQUIRE(result.bytes == bytes.size());
  REQUIRE(read == bytes);
  REQUIRE_FALSE(file->ResizeResult(0));
  REQUIRE_FALSE(file->FlushResult());
  allocated = file->QueryAllocatedBytes(error);
  REQUIRE_FALSE(error);
  REQUIRE(allocated);
  REQUIRE(*allocated == ObservedNativeAllocation(*file));
  REQUIRE(*allocated == 0);
}
TEST_CASE("Native allocation remains on an open inode after rename and unlink",
          "[filesystem][allocation][retained]") {
  ScratchDir scratch;
  const auto path = scratch.path() / "name";
  const auto held = scratch.path() / "held";
  {
    std::ofstream file(path, std::ios::binary);
    file << "native";
    REQUIRE(file.good());
  }
  auto file = filesystem::FileHandle::OpenExisting(
      path, filesystem::FileAccess::kGenericRead);
  REQUIRE(file);
  struct stat before{};
  REQUIRE(::fstat(int(file->native_handle()), &before) == 0);
  std::filesystem::rename(path, held);
  {
    std::ofstream replacement(path, std::ios::binary);
    replacement << std::string(16384, 'z');
    REQUIRE(replacement.good());
  }
  struct stat named{};
  REQUIRE(::stat(path.c_str(), &named) == 0);
  REQUIRE(named.st_ino != before.st_ino);
  std::error_code error;
  auto allocated = file->QueryAllocatedBytes(error);
  REQUIRE_FALSE(error);
  REQUIRE(allocated);
  REQUIRE(*allocated == ObservedNativeAllocation(*file));
  auto info = file->QueryInfo(error);
  REQUIRE(info);
  REQUIRE_FALSE(error);
  REQUIRE(info->file_id == uint64_t(before.st_ino));
  REQUIRE(info->size == 6);
  REQUIRE(std::filesystem::remove(held));
  allocated = file->QueryAllocatedBytes(error);
  REQUIRE_FALSE(error);
  REQUIRE(allocated);
  REQUIRE(*allocated == ObservedNativeAllocation(*file));
  info = file->QueryInfo(error);
  REQUIRE(info);
  REQUIRE_FALSE(error);
  REQUIRE(info->links == 0);
  auto directory = filesystem::FileHandle::OpenExisting(
      scratch.path(), filesystem::FileAccess::kGenericRead);
  REQUIRE(directory);
  REQUIRE_FALSE(directory->QueryAllocatedBytes(error));
  REQUIRE(error == std::errc::operation_not_supported);
}
}  // namespace xe::base::test
#endif
