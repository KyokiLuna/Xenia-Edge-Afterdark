/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include <cstring>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/user_profile.h"

namespace xe::kernel::xam::test {
namespace {
class SettingsDatabase : public GpdInfo {
 public:
  SettingsDatabase() : GpdInfo(kDashboardID) {}
  void Insert(uint32_t id, const std::vector<uint8_t>& data) {
    Entry e(id, uint16_t(GpdSection::kSetting), uint32_t(data.size()));
    e.data = data;
    UpsertEntry(&e);
  }
};
std::vector<uint8_t> Stored(uint32_t id, X_USER_DATA_TYPE type,
                            uint32_t declared, size_t actual) {
  X_XDBF_GPD_SETTING_HEADER header{};
  header.setting_id = id;
  header.setting_type = type;
  header.base_data.binary.size = declared;
  header.base_data.binary.ptr = 0xFFFFFFFF;  // Persisted pointers are opaque.
  std::vector<uint8_t> data(sizeof(header) + actual, 0x42);
  std::memcpy(data.data(), &header, sizeof(header));
  return data;
}
}  // namespace
TEST_CASE("Profile setting snapshots own stored bytes without modifying GPDs",
          "[profile][persistence]") {
  constexpr uint32_t avatar = 0x63E80044;
  SettingsDatabase database;
  std::optional<UserSetting> result;
  REQUIRE(UserSetting::ReadFromGpd(database, avatar, result) == X_ERROR_SUCCESS);
  CHECK_FALSE(result);  // Missing appearance is not a fabricated preset.
  database.Insert(avatar, Stored(avatar, X_USER_DATA_TYPE::BINARY, 1000, 1000));
  const auto before = database.Serialize();
  REQUIRE(UserSetting::ReadFromGpd(database, avatar, result) == X_ERROR_SUCCESS);
  REQUIRE(result);
  CHECK(result->get_setting_source() == X_USER_PROFILE_SETTING_SOURCE::TITLE);
  CHECK(result->get_extended_data().size() == 1000);
  CHECK(database.Serialize() == before);
  database.Insert(avatar, {});  // Invalidate source after taking the snapshot.
  CHECK(result->get_extended_data().front() == 0x42);
  CHECK(result->get_extended_data().back() == 0x42);
}
TEST_CASE("Malformed stored profile settings cannot become empty defaults",
          "[profile][persistence]") {
  constexpr uint32_t avatar = 0x63E80044, text = 0x402C0011;
  SettingsDatabase database;
  uint32_t id = avatar;
  std::vector<uint8_t> data;
  SECTION("truncated setting header") { data.resize(4); }
  SECTION("truncated extended data") {
    data = Stored(avatar, X_USER_DATA_TYPE::BINARY, 1000, 999);
  }
  SECTION("wrong stored type") {
    data = Stored(avatar, X_USER_DATA_TYPE::INT32, 0, 0);
  }
  SECTION("wrong stored ID") {
    data = Stored(avatar + 1, X_USER_DATA_TYPE::BINARY, 1000, 1000);
  }
  SECTION("over-key payload") {
    data = Stored(avatar, X_USER_DATA_TYPE::BINARY, 1001, 1001);
  }
  SECTION("odd UTF16 length") {
    id = text; data = Stored(text, X_USER_DATA_TYPE::WSTRING, 3, 3);
  }
  database.Insert(id, data);
  const auto before = database.Serialize();
  std::optional<UserSetting> result;
  CHECK(UserSetting::ReadFromGpd(database, id, result) == X_ERROR_FILE_CORRUPT);
  CHECK_FALSE(result);
  CHECK(database.Serialize() == before);
}
}  // namespace xe::kernel::xam::test
