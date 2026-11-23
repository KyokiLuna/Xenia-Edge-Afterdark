/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include <algorithm>
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

TEST_CASE("Host profile strings use native UTF16BE across GPD and guest-origin roundtrips",
          "[kernel][profile][persistence][abi][wire_string]") {
  const std::u16string host = u"A\u00E9\u6C34\U0001F3AE";
  const std::vector<uint8_t> expected = {
      0,0x41, 0,0xE9, 0x6C,0x34, 0xD8,0x3C, 0xDF,0xAE, 0,0};
  UserSetting value(UserSettingId::XPROFILE_GAMERCARD_MOTTO, host);
  CHECK(std::vector<uint8_t>(value.get_extended_data().begin(),
                             value.get_extended_data().end()) == expected);
  auto terminated = host; terminated.push_back(0);
  CHECK(std::get<std::u16string>(value.get_host_data()) == terminated);
  const auto wire = value.Serialize();
  REQUIRE(wire.size() == sizeof(X_XDBF_GPD_SETTING_HEADER) + expected.size());
  X_XDBF_GPD_SETTING_HEADER header;
  std::memcpy(&header, wire.data(), sizeof(header));
  CHECK(header.setting_id == uint32_t(UserSettingId::XPROFILE_GAMERCARD_MOTTO));
  CHECK(header.setting_type == X_USER_DATA_TYPE::WSTRING);
  CHECK(header.base_data.unicode.size == expected.size());
  CHECK(std::equal(expected.begin(), expected.end(), wire.begin() + sizeof(header)));
  // Guest and GPD constructors already own wire bytes, so they must not be
  // swapped again. Deliberately provide an unaligned source span.
  std::vector<uint8_t> unaligned(1, 0xA7);
  unaligned.insert(unaligned.end(), expected.begin(), expected.end());
  header.base_data.unicode.ptr = 0x87654321;
  UserSetting from_gpd(&header, std::span(unaligned).subspan(1));
  CHECK(std::get<std::u16string>(from_gpd.get_host_data()) == terminated);
  CHECK(std::vector<uint8_t>(from_gpd.get_extended_data().begin(),
                             from_gpd.get_extended_data().end()) == expected);
  SettingsDatabase db;
  db.Insert(uint32_t(UserSettingId::XPROFILE_GAMERCARD_MOTTO), wire);
  const auto before = db.Serialize();
  std::optional<UserSetting> read;
  REQUIRE(UserSetting::ReadFromGpd(db,
      uint32_t(UserSettingId::XPROFILE_GAMERCARD_MOTTO), read) == X_ERROR_SUCCESS);
  REQUIRE(read);
  CHECK(std::get<std::u16string>(read->get_host_data()) == terminated);
  CHECK(db.Serialize() == before);
  // This caller formerly pre-swapped to compensate for the host constructor.
  const auto key = UserSetting::GetDefaultSetting(
      uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY));
  REQUIRE(key);
  const std::vector<uint8_t> key_wire = {
      0,'F',0,'F',0,'F',0,'E',0,'0',0,'7',0,'D',0,'1',
      0,'0',0,'0',0,'0',0,'2',0,'0',0,'0',0,'0',0,'2',
      0,'0',0,'0',0,'0',0,'1',0,'0',0,'0',0,'0',0,'2',0,0};
  CHECK(std::vector<uint8_t>(key->get_extended_data().begin(),
                             key->get_extended_data().end()) == key_wire);
  UserSetting limit(UserSettingId::XPROFILE_GAMERCARD_USER_BIO,
                     std::u16string(1001, u'A'));
  const auto limited = limit.get_extended_data();
  REQUIRE(limited.size() == kMaxUserDataSize);
  CHECK(limited[0] == 0); CHECK(limited[1] == 'A');
  CHECK(limited[limited.size()-4] == 0); CHECK(limited[limited.size()-3] == 'A');
  CHECK(limited[limited.size()-2] == 0); CHECK(limited.back() == 0);
  std::vector<uint8_t> opaque(1000, 0x81); opaque[0x380] = 0xE0;
  UserSetting appearance(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1, opaque);
  CHECK(std::vector<uint8_t>(appearance.get_extended_data().begin(),
                             appearance.get_extended_data().end()) == opaque);
}
}  // namespace xe::kernel::xam::test
