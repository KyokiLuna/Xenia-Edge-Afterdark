/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/platform.h"

#if !XE_PLATFORM_ANDROID
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <wx/init.h>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/system_font.h"
#include "xenia/ui/system_files_installer_wx.h"

namespace xe::ui::test {
namespace {
std::filesystem::path OwnedInstallerFixture(const char* variable) {
  const char* value = std::getenv(variable);
  REQUIRE(value);
  REQUIRE(*value);
  const auto path = std::filesystem::path(value);
  REQUIRE(std::filesystem::is_regular_file(path));
  return path;
}
std::vector<uint8_t> ReadInstallerBytes(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  REQUIRE_FALSE(error);
  REQUIRE(size <= kernel::xam::kMaximumSystemFontBytes);
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  REQUIRE(file.gcount() == std::streamsize(bytes.size()));
  return bytes;
}
void WriteInstallerBytes(const std::filesystem::path& path,
                          const std::vector<uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file.write(reinterpret_cast<const char*>(bytes.data()),
             std::streamsize(bytes.size()));
  file.close();
  REQUIRE(file.good());
}
class InstallerDestination {
 public:
  InstallerDestination() {
    std::random_device random;
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
          ("xenia-system-font-installer-" + std::to_string(random()) + "-" +
           std::to_string(random()));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        root = candidate;
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
    system = root / "system";
  }
  ~InstallerDestination() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  std::filesystem::path root, system;
};
std::string RawBackupName(const std::vector<uint8_t>& bytes) {
  const auto name = kernel::xam::SystemFontRecoveryBackupName(bytes);
  REQUIRE(name);
  return *name;
}
using InstallerTree = std::map<std::string, std::vector<uint8_t>>;
InstallerTree SnapshotInstallerTree(const std::filesystem::path& root) {
  InstallerTree tree;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    REQUIRE_FALSE(entry.is_symlink());
    const auto relative = entry.path().lexically_relative(root).generic_string();
    if (entry.is_directory()) {
      tree.emplace("directory:" + relative, std::vector<uint8_t>{});
    } else {
      REQUIRE(entry.is_regular_file());
      tree.emplace("file:" + relative, ReadInstallerBytes(entry.path()));
    }
  }
  return tree;
}
}  // namespace

TEST_CASE("Owned system update installation preserves or repairs the exact base font",
          "[.owned-system-font-install][system_font][installer]") {
  // The UI test executable has no wxIMPLEMENT_APP factory. The bundled
  // wxInitialize implementation supplies its actual dummy console app; no
  // dialog, window, GUI app replacement or native guest execution is needed.
  wxInitializer wx;
  REQUIRE(wx.IsOk());
  const auto update = OwnedInstallerFixture("XENIA_TEST_SYSTEM_UPDATE_ZIP");
  const auto base_path = OwnedInstallerFixture("XENIA_TEST_SYSTEM_JK_XTT");
  const auto base = ReadInstallerBytes(base_path);
  kernel::xam::SystemFontInfo base_info;
  REQUIRE(kernel::xam::ValidateSystemFont(base, base_info));
  REQUIRE(base_info.is_base);
  InstallerDestination owned;
  const auto accept = [](size_t, size_t) { return true; };

  SECTION("A patch-only update cannot publish a missing base") {
    CHECK(InstallSystemFiles(update, owned.system, accept) ==
          SystemFilesResult::kMissingBaseFont);
    CHECK_FALSE(std::filesystem::exists(owned.system / "flash"));
    CHECK_FALSE(std::filesystem::exists(owned.system / "staging"));
    CHECK_FALSE(std::filesystem::exists(owned.system / "font-backups"));
  }
  SECTION("Explicit retail repair retains the exact generated raw backup") {
    const auto generated_path =
        OwnedInstallerFixture("XENIA_TEST_SYSTEM_GENERATED_XTT");
    const auto generated = ReadInstallerBytes(generated_path);
    REQUIRE(kernel::xam::IsGeneratedSystemFontSubstitute(generated));
    kernel::xam::SystemFontInfo invalid;
    REQUIRE_FALSE(kernel::xam::ValidateSystemFont(generated, invalid));
    WriteInstallerBytes(owned.system / "flash" / "xenonjklatin.xtt", generated);
    SystemFilesInstallOptions options;
    options.base_font = base_path;
    REQUIRE(InstallSystemFiles(update, owned.system, accept, options) ==
            SystemFilesResult::kSuccess);
    CHECK(ReadInstallerBytes(owned.system / "flash" / "xenonjklatin.xtt") == base);
    const auto backup = owned.system / "font-backups" / RawBackupName(generated);
    CHECK(ReadInstallerBytes(backup) == generated);
    CHECK_FALSE(std::filesystem::exists(owned.system / "staging"));
    CHECK_FALSE(std::filesystem::exists(GetUnrestoredSystemFilesPath(owned.system)));
    CHECK(ReadInstallerBytes(generated_path) == generated);
  }
  SECTION("An installed authenticated matching base needs no selected option") {
    WriteInstallerBytes(owned.system / "flash" / "xenonjklatin.xtt", base);
    REQUIRE(InstallSystemFiles(update, owned.system, accept) ==
            SystemFilesResult::kSuccess);
    CHECK(ReadInstallerBytes(owned.system / "flash" / "xenonjklatin.xtt") == base);
    CHECK_FALSE(std::filesystem::exists(owned.system / "font-backups"));
    CHECK_FALSE(std::filesystem::exists(owned.system / "staging"));
  }
  SECTION("Cancellation after preparation leaves all installed bytes and names") {
    WriteInstallerBytes(owned.system / "flash" / "xenonjklatin.xtt", base);
    WriteInstallerBytes(owned.system / "flash" / "old.bin", {1, 2, 3});
    WriteInstallerBytes(owned.system / "sysext" / "old.bin", {4, 5, 6});
    WriteInstallerBytes(owned.system / "sysaux" / "old.bin", {7, 8, 9});
    const auto before = SnapshotInstallerTree(owned.system);
    bool canceled_after_preparation = false;
    const auto cancel_before_publication = [&](size_t done, size_t total) {
      // Unzip reports [0,total), then the real final admission reports (1,1).
      // Cancel at that exact synchronous boundary, without sleep or timeout.
      if (done == total) {
        canceled_after_preparation = true;
        return false;
      }
      return true;
    };
    CHECK(InstallSystemFiles(update, owned.system, cancel_before_publication) ==
          SystemFilesResult::kCancelled);
    CHECK(canceled_after_preparation);
    CHECK(SnapshotInstallerTree(owned.system) == before);
  }
  CHECK(ReadInstallerBytes(base_path) == base);
}
}  // namespace xe::ui::test
#endif  // !XE_PLATFORM_ANDROID
