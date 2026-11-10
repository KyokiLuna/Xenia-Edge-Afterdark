/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"
#include "xenia/emulator.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
DECLARE_string(xefu_path);
#if XE_PLATFORM_LINUX
namespace xe::kernel::test {
TEST_CASE("Graphics replay rejects guest CPU and persistent storage setup",
          "[console_owner][graphics_replay]") {
  SECTION("guest execution cannot opt into console-free replay") {
    Emulator replay("", "", "", "", Emulator::SetupPurpose::kGraphicsReplay);
    REQUIRE(replay.Setup(nullptr, nullptr, true, {}, {}, {}) ==
            X_STATUS_INVALID_PARAMETER);
    REQUIRE_FALSE(replay.memory());
    REQUIRE_FALSE(replay.kernel_state());
    REQUIRE_FALSE(replay.file_system());
  }
  SECTION("an explicit root cannot be silently ignored") {
    Emulator replay("", "not-a-managed-console", "", "",
                    Emulator::SetupPurpose::kGraphicsReplay);
    REQUIRE(replay.Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_INVALID_PARAMETER);
    REQUIRE_FALSE(replay.memory());
    REQUIRE_FALSE(replay.kernel_state());
    REQUIRE_FALSE(replay.file_system());
  }
}
TEST_CASE("Graphics replay rejects title launch before filesystem mutation",
          "[console_owner][graphics_replay]") {
  Emulator replay("", "", "", "", Emulator::SetupPurpose::kGraphicsReplay);
  REQUIRE(replay.LaunchPath("no-guest-title.xex") ==
          X_STATUS_INVALID_PARAMETER);
  REQUIRE_FALSE(replay.memory());
  REQUIRE_FALSE(replay.kernel_state());
  REQUIRE_FALSE(replay.file_system());
}
TEST_CASE("All direct replay launch APIs reject before touching guest state",
          "[console_owner][graphics_replay]") {
  Emulator replay("", "", "", "", Emulator::SetupPurpose::kGraphicsReplay);
  const std::array<std::function<X_STATUS()>, 7> launchers{
      [&] { return replay.LaunchXexFile("missing.xex"); },
      [&] { return replay.LaunchDiscImage("missing.iso"); },
      [&] { return replay.LaunchXboxOriginal("missing.xbe", "default.xbe"); },
      [&] { return replay.LaunchDiscArchive("missing.zar"); },
      [&] { return replay.LaunchStfsContainer("missing.stfs"); },
      [&] { return replay.LaunchDiscPackage("missing", "package"); },
      [&] { return replay.LaunchDefaultModule("missing.xex"); }};
  for (const auto &launch : launchers) {
    REQUIRE(launch() == X_STATUS_INVALID_PARAMETER);
    REQUIRE_FALSE(replay.memory());
    REQUIRE_FALSE(replay.kernel_state());
    REQUIRE_FALSE(replay.file_system());
  }
}
TEST_CASE("Replay Xbox launch never creates a XeFu directory in the CWD",
          "[console_owner][graphics_replay]") {
  struct ScratchCwd {
    std::filesystem::path original = std::filesystem::current_path();
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("xenia-replay-cwd-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string xefu = cvars::xefu_path;
    ScratchCwd() {
      REQUIRE(std::filesystem::create_directory(path));
      std::filesystem::current_path(path);
      cvars::xefu_path.clear();
    }
    ~ScratchCwd() {
      cvars::xefu_path = xefu;
      std::error_code error;
      std::filesystem::current_path(original, error);
      if (!error) {
        std::filesystem::remove_all(path, error);
      }
    }
  } scratch;
  REQUIRE(std::filesystem::is_empty(scratch.path));
  Emulator replay("", "", "", "", Emulator::SetupPurpose::kGraphicsReplay);
  REQUIRE(replay.LaunchXboxOriginal("missing.xbe", "default.xbe") ==
          X_STATUS_INVALID_PARAMETER);
  REQUIRE(std::filesystem::is_empty(scratch.path));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "xefu"));
}
} // namespace xe::kernel::test
#endif
