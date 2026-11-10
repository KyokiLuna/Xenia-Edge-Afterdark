/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/xui_resource.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
namespace xe::resource::test {
namespace {
void BE(std::vector<uint8_t>& b, size_t at, uint32_t value, size_t width = 4) {
  for (size_t i = 0; i < width; ++i) {
    b[at + width - i - 1] = uint8_t(value >> (8 * i));
  }
}
std::vector<uint8_t> Package(uint32_t version) {
  const std::string name = "NotifyPopup.xma";
  const size_t width = version == 1 ? 2 : 1;
  const size_t table = 9 + name.size() * width;
  std::vector<uint8_t> bytes(0x16 + table + 3);
  BE(bytes, 0, 0x5855495A);
  BE(bytes, 4, version);
  BE(bytes, 0x10, table);
  BE(bytes, 0x14, 1, 2);
  BE(bytes, 0x16, 3);
  BE(bytes, 0x1A, 0);
  bytes[0x1E] = uint8_t(name.size());
  for (size_t i = 0; i < name.size(); ++i) {
    bytes[0x1F + i * width + width - 1] = name[i];
  }
  bytes[0x16 + table] = 1;
  bytes[0x17 + table] = 2;
  bytes[0x18 + table] = 3;
  return bytes;
}
}  // namespace
TEST_CASE("Shared XUI packages retain actual v1/v3 names and data bounds",
          "[xui][resource]") {
  for (uint32_t version : {1u, 3u}) {
    auto bytes = Package(version);
    auto found = LookupXuiPackageFile(bytes, "notifypopup.XMA");
    REQUIRE(found.status == XuiPackageStatus::kFound);
    REQUIRE(found.bytes.size() == 3);
    CHECK(found.bytes[1] == 2);
    CHECK(LookupXuiPackageFile(bytes, "other.xma").status ==
          XuiPackageStatus::kNotFound);
    BE(bytes, 0x1A, UINT32_MAX);
    CHECK(LookupXuiPackageFile(bytes, "NotifyPopup.xma").status ==
          XuiPackageStatus::kInvalid);
  }
}
TEST_CASE(
    "Shared XUI parsing rejects malformed encoding and table/data overlap",
    "[xui][resource]") {
  auto bytes = Package(3);
  bytes[0x1F] = 0xFF;
  CHECK_FALSE(FindXuiPackageFile(bytes, "NotifyPopup.xma"));
  bytes = Package(1);
  bytes[0x1F] = 0xD8;
  bytes[0x20] = 0x00;
  CHECK_FALSE(FindXuiPackageFile(bytes, "NotifyPopup.xma"));
  bytes = Package(3);
  BE(bytes, 0x10, 1);
  CHECK(LookupXuiPackageFile(bytes, "NotifyPopup.xma").status ==
        XuiPackageStatus::kInvalid);
  bytes = Package(3);
  BE(bytes, 4, 5);
  CHECK(LookupXuiPackageFile(bytes, "NotifyPopup.xma").status ==
        XuiPackageStatus::kUnsupported);
}
TEST_CASE(
    "Shared XUIS tables return owned strings and preserve empty locale rows",
    "[xui][resource]") {
  std::vector<uint8_t> bytes(12 + 1 + 3);
  BE(bytes, 0, 0x58554953);
  bytes[4] = 2;
  bytes[5] = 2;
  BE(bytes, 10, 2, 2);
  bytes[12] = 0;
  bytes[13] = 'A';
  bytes[14] = 'B';
  bytes[15] = 0;
  uint8_t flags = 0;
  auto strings = ParseXuiStringTable(bytes, &flags);
  REQUIRE(strings);
  REQUIRE(strings->size() == 2);
  CHECK((*strings)[0].empty());
  CHECK((*strings)[1] == u"AB");
  CHECK(flags == 2);
  bytes[13] = 'C';
  CHECK((*strings)[1] == u"AB");
  bytes.pop_back();
  CHECK_FALSE(ParseXuiStringTable(bytes, &flags));
  CHECK_FALSE(ParseXuiStringTable(bytes, nullptr));
}
}  // namespace xe::resource::test
