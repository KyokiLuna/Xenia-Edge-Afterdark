/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/vfs/module_resource.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/xex_metadata.h"
namespace xe::vfs::test {
namespace {
void BE(std::vector<uint8_t>& b, size_t at, uint32_t value, size_t width = 4) {
  for (size_t i = 0; i < width; ++i) {
    b[at + width - i - 1] = uint8_t(value >> (8 * i));
  }
}
std::vector<uint8_t> XexResource() {
  const std::string name = "cue.bin";
  std::vector<uint8_t> package(0x16 + 9 + name.size() + 3);
  BE(package, 0, 0x5855495A);
  BE(package, 4, 3);
  BE(package, 0x10, 9 + name.size());
  BE(package, 0x14, 1, 2);
  BE(package, 0x16, 3);
  package[0x1E] = uint8_t(name.size());
  std::copy(name.begin(), name.end(), package.begin() + 0x1F);
  package[package.size() - 3] = 7;
  package[package.size() - 2] = 8;
  package.back() = 9;
  std::vector<uint8_t> xex(0x400 + 0x200);
  BE(xex, 0, 0x58455832);
  BE(xex, 8, 0x400);
  BE(xex, 16, 0x100);
  BE(xex, 20, 2);
  BE(xex, 24, XEX_HEADER_FILE_FORMAT_INFO);
  BE(xex, 28, 0x40);
  BE(xex, 32, XEX_HEADER_RESOURCE_INFO);
  BE(xex, 36, 0x80);
  BE(xex, 0x40, 8);
  BE(xex, 0x80, 20);
  std::memcpy(xex.data() + 0x84, "skin", 4);
  BE(xex, 0x8C, 0x82000100);
  BE(xex, 0x90, package.size());
  BE(xex, 0x104, 0x200);
  BE(xex, 0x210, 0x82000000);
  xex[0x400] = 'M';
  xex[0x401] = 'Z';
  xex[0x43C] = 0x80;
  xex[0x480] = 'P';
  xex[0x481] = 'E';
  std::copy(package.begin(), package.end(), xex.begin() + 0x500);
  return xex;
}
struct Directory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xenia-resource-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { REQUIRE(std::filesystem::create_directory(path)); }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
}  // namespace
TEST_CASE(
    "Offline module resources reuse bounded decode and distinguish absent "
    "members",
    "[resource][xex]") {
  auto bytes = XexResource();
  auto extracted = ExtractXexResource(bytes, "skin");
  REQUIRE(extracted.status == XexResourceStatus::kLoaded);
  CHECK_FALSE(extracted.bytes.empty());
  CHECK(ExtractXexResource(bytes, "other").status ==
        XexResourceStatus::kNotFound);
  BE(bytes, 0x8C, 0x81FFFFFF);
  CHECK(ExtractXexResource(bytes, "skin").status ==
        XexResourceStatus::kInvalid);
}
TEST_CASE(
    "Installed module resource VFS snapshot is read-only and failure leaves "
    "output unchanged",
    "[resource][vfs]") {
  Directory directory;
  const auto bytes = XexResource();
  const auto file = directory.path / "module.xex";
  {
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(output.good());
  }
  VirtualFileSystem fs;
  auto device = std::make_unique<HostPathDevice>("\\Device\\Resource",
                                                 directory.path, true);
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  const std::vector<uint8_t> expected{7, 8, 9};
  std::vector<uint8_t> output{0xA5};
  REQUIRE(ReadModuleResource(fs, "\\Device\\Resource\\module.xex", "skin",
                             "cue.bin", output) == X_STATUS_SUCCESS);
  CHECK(output == expected);
  CHECK(ReadModuleResource(fs, "\\Device\\Resource\\module.xex", "skin",
                           "missing", output) == X_STATUS_NOT_FOUND);
  CHECK(output == expected);
  CHECK(ReadModuleResource(fs, "\\Device\\Resource\\module.xex", "skin",
                           "cue.bin", output, 2) == X_STATUS_BUFFER_TOO_SMALL);
  CHECK(output == expected);
  std::vector<uint8_t> after(bytes.size());
  std::ifstream input(file, std::ios::binary);
  REQUIRE(static_cast<bool>(
      input.read(reinterpret_cast<char*>(after.data()), after.size())));
  CHECK(after == bytes);
  fs.Clear();
}
}  // namespace xe::vfs::test
