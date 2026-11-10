/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <memory>
#include <string>
#include "third_party/catch/include/catch.hpp"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe::vfs::test {
TEST_CASE("Namespace translation selects longest whole-component alias",
          "[vfs][namespace]") {
  VirtualFileSystem fs;
  for (const std::string name : {"Outer", "Inner"}) {
    auto device = std::make_unique<NullDevice>(
        "\\Device\\" + name, std::initializer_list<std::string>{"\\file"});
    REQUIRE(device->Initialize());
    REQUIRE(fs.RegisterDevice(std::move(device)));
  }
  REQUIRE(fs.RegisterSymbolicLink("GAME:", "\\Device\\Outer"));
  REQUIRE(fs.RegisterSymbolicLink("game:\\nested", "\\Device\\Inner"));
  std::string output = "unchanged", target = "unchanged";
  REQUIRE(fs.TranslateSymbolicPath("gAmE:\\NeStEd\\file", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Inner\\file");
  REQUIRE(fs.FindSymbolicLink("GAME:\\nested\\file", target));
  CHECK(target == "\\Device\\Inner");
  auto* entry = fs.ResolvePath("GAME:\\nested\\file");
  REQUIRE(entry);
  CHECK(entry->device() == fs.GetDevice("\\Device\\Inner"));
  REQUIRE(fs.TranslateSymbolicPath("game:\\nestedLong\\file", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Outer\\nestedLong\\file");
  REQUIRE(fs.TranslateSymbolicPath("game:file", output) == X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Outer\\file");
  CHECK(fs.ResolvePath("game:file") ==
        fs.GetDevice("\\Device\\Outer")->ResolvePath("\\file"));
}
TEST_CASE("Namespace aliases share DOS prefixes and preserve direct NT names",
          "[vfs][namespace]") {
  VirtualFileSystem fs;
  std::string output = "old", target = "old";
  REQUIRE(fs.RegisterSymbolicLink("\\System??\\X:", "\\Device\\Disk"));
  CHECK_FALSE(fs.RegisterSymbolicLink("\\??\\x:", "\\Device\\Other"));
  REQUIRE(fs.TranslateSymbolicPath("\\DosDevices\\x:\\a\\..\\b", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Disk\\b");
  REQUIRE(fs.RegisterSymbolicLink("\\Alias", "\\Device\\Short"));
  CHECK_FALSE(fs.FindSymbolicLink("\\AliasLong\\file", target));
  CHECK(target == "old");
  REQUIRE(fs.TranslateSymbolicPath("\\Device\\NotMounted\\future", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\NotMounted\\future");
  CHECK(fs.ResolvePath(output) == nullptr);
  REQUIRE(fs.UnregisterSymbolicLink("\\??\\X:"));
  CHECK_FALSE(fs.IsSymbolicLinkRegistered("x:"));
}
TEST_CASE("Namespace loops growth and limits never publish a partial result",
          "[vfs][namespace]") {
  VirtualFileSystem fs;
  std::string output = "sentinel";
  REQUIRE(fs.RegisterSymbolicLink("A:", "b:"));
  REQUIRE(fs.RegisterSymbolicLink("B:", "a:"));
  CHECK(XFAILED(fs.TranslateSymbolicPath("a:\\file", output)));
  CHECK(output == "sentinel");
  REQUIRE(fs.RegisterSymbolicLink("grow:", "grow:\\more"));
  CHECK(XFAILED(fs.TranslateSymbolicPath("grow:\\file", output)));
  CHECK(output == "sentinel");
  REQUIRE(fs.RegisterSymbolicLink(
      "long:", std::string(VirtualFileSystem::kMaximumTranslatedPath, 'q')));
  CHECK(fs.TranslateSymbolicPath("long:\\x", output) ==
        X_STATUS_BUFFER_TOO_SMALL);
  CHECK(output == "sentinel");
  CHECK(XFAILED(fs.TranslateSymbolicPath(std::string(0x10000, 'q'), output)));
  CHECK(output == "sentinel");
  CHECK(XFAILED(fs.TranslateSymbolicPath(std::string("a\0b", 3), output)));
  CHECK(output == "sentinel");
  for (uint32_t i = 0; i < 33; ++i) {
    REQUIRE(fs.RegisterSymbolicLink("d" + std::to_string(i) + ":",
                                    "d" + std::to_string(i + 1) + ":"));
  }
  CHECK(XFAILED(fs.TranslateSymbolicPath("d0:\\file", output)));
  CHECK(output == "sentinel");
  fs.Clear();
  REQUIRE(fs.TranslateSymbolicPath("\\Device\\Disk", output) ==
          X_STATUS_SUCCESS);
}
TEST_CASE(
    "Malformed namespace bytes are rejected before checked UTF8 iteration",
    "[vfs][namespace][encoding]") {
  VirtualFileSystem fs;
  REQUIRE(fs.RegisterSymbolicLink("data:", "\\Device\\Disk"));
  for (const auto bad : {std::string("\xFF", 1), std::string("\xE2\x82", 2),
                         std::string("\xC0\xAF", 2)}) {
    std::string output = "sentinel", target = "sentinel";
    CHECK(fs.TranslateSymbolicPath(bad, output) ==
          X_STATUS_OBJECT_NAME_INVALID);
    CHECK(output == "sentinel");
    CHECK_FALSE(fs.RegisterSymbolicLink(bad, "\\Device\\Disk"));
    CHECK_FALSE(fs.RegisterSymbolicLink("bad:", bad));
    CHECK_FALSE(fs.FindSymbolicLink(bad, target));
    CHECK(target == "sentinel");
    CHECK_FALSE(fs.UnregisterSymbolicLink(bad));
    CHECK_FALSE(fs.IsSymbolicLinkRegistered(bad));
    CHECK(fs.ResolvePath(bad) == nullptr);
  }
}
}  // namespace xe::vfs::test
