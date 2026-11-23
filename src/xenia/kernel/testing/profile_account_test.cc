/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include <array>
#include <cstring>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/profile_manager.h"

namespace xe::kernel::xam::test {
TEST_CASE("Account authentication bounds preserve output on damaged input",
          "[profile][persistence]") {
  X_XAMACCOUNTINFO account{};
  string_util::copy_and_swap_truncating(account.gamertag, u"Local User",
                                        xe::countof(account.gamertag));
  account.network_id = 0x87654321;  // Unknown fields must survive unchanged.
  std::array<uint8_t, sizeof(X_XAMACCOUNTINFO) + 0x18> encrypted{};
  for (bool devkit : {false, true}) {
    ProfileManager::EncryptAccountFile(&account, encrypted.data(), devkit);
    X_XAMACCOUNTINFO output{};
    REQUIRE(ProfileManager::DecryptAccountFile(encrypted, &output, devkit));
    CHECK(std::memcmp(&output, &account, sizeof(account)) == 0);
    for (size_t length : {size_t(0), sizeof(account), encrypted.size() - 1}) {
      output.network_id = 0x12345678;
      const auto before = output;
      CHECK_FALSE(ProfileManager::DecryptAccountFile(
          std::span(encrypted).first(length), &output, devkit));
      CHECK(std::memcmp(&output, &before, sizeof(output)) == 0);
    }
    const auto before = output;
    encrypted.back() ^= 0x80;
    CHECK_FALSE(ProfileManager::DecryptAccountFile(encrypted, &output, devkit));
    CHECK(std::memcmp(&output, &before, sizeof(output)) == 0);
  }
  CHECK_FALSE(ProfileManager::DecryptAccountFile(encrypted, nullptr));
}
TEST_CASE("Account gamer tag decoding stops at its fixed buffer", "[profile]") {
  X_XAMACCOUNTINFO account{};
  for (auto& ch : account.gamertag) ch = xe::byte_swap(char16_t(u'A'));
  account.xuid_online = 0x4141414141414141ULL;
  CHECK(account.GetGamertagString() == std::string(16, 'A'));
}
TEST_CASE("Host Account field edits clear the complete native name and preserve all unrelated bytes",
          "[profile][account_edit][abi][preservation]") {
  X_XAMACCOUNTINFO before;
  std::memset(&before, 0xa5, sizeof(before));
  string_util::copy_and_swap_truncating(before.gamertag, u"ABCDEFGHIJKLMNO", 16);
  before.ToggleLiveFlag(true);
  before.xuid_online = 0;  // A rename must never provision a new online identity.
  ProfileAccountFieldEdits rename;
  rename.gamertag = "A";
  X_XAMACCOUNTINFO after{};
  REQUIRE_FALSE(rename.Apply(before, after));
  const std::array<uint8_t, 32> expected_name{0, 0x41};
  CHECK(std::memcmp(after.gamertag, expected_name.data(), expected_name.size()) == 0);
  auto untouched = after;
  std::memcpy(untouched.gamertag, before.gamertag, sizeof(before.gamertag));
  CHECK(std::memcmp(&untouched, &before, sizeof(before)) == 0);
  CHECK(after.GetGamertagString() == "A");
  CHECK(after.IsLiveEnabled());
  CHECK(after.GetOnlineXUID() == 0);
  const auto form = ProfileAccountFieldValues::From(before);
  const auto none = ProfileAccountFieldEdits::Between(form, form);
  CHECK(none.empty());
  REQUIRE_FALSE(none.Apply(before, after));
  CHECK(std::memcmp(&after, &before, sizeof(before)) == 0);
  auto desired = form;
  desired.gamertag = "B";
  const auto delta = ProfileAccountFieldEdits::Between(form, desired);
  REQUIRE(delta.gamertag); CHECK(*delta.gamertag == "B");
  CHECK_FALSE(delta.country); CHECK_FALSE(delta.language);
}
TEST_CASE("Host Account edit validation refuses truncation or invalid fields without changing output",
          "[profile][account_edit][validation][failure]") {
  X_XAMACCOUNTINFO before;
  std::memset(&before, 0x5a, sizeof(before));
  for (const auto& invalid : std::vector<std::string>{
           "", "ABCDEFGHIJKLMNOP", "1A", " A", "A ", "A  B", "A!",
           std::string("A\0B", 3), "\xC3\xA9"}) {
    ProfileAccountFieldEdits edits;
    edits.gamertag = invalid;
    auto output = before;
    CHECK(edits.Apply(before, output) == std::errc::invalid_argument);
    CHECK(std::memcmp(&output, &before, sizeof(before)) == 0);
    CHECK(edits.Apply(output, output) == std::errc::invalid_argument);
    CHECK(std::memcmp(&output, &before, sizeof(before)) == 0);
  }
  for (uint32_t invalid : {0x100u, UINT32_MAX}) {
    ProfileAccountFieldEdits edits;
    edits.country = static_cast<XOnlineCountry>(invalid);
    auto output = before;
    CHECK(edits.Apply(before, output) == std::errc::invalid_argument);
    CHECK(std::memcmp(&output, &before, sizeof(before)) == 0);
  }
  ProfileAccountFieldEdits bad_language;
  bad_language.language = static_cast<XLanguage>(0x20);
  auto output = before;
  CHECK(bad_language.Apply(before, output) == std::errc::invalid_argument);
  CHECK(std::memcmp(&output, &before, sizeof(before)) == 0);
  ProfileAccountFieldEdits longest;
  longest.gamertag = "ABCDEFGHIJKLMNO";
  REQUIRE_FALSE(longest.Apply(before, output));
  CHECK(output.GetGamertagString() == "ABCDEFGHIJKLMNO");
  const auto* bytes = reinterpret_cast<const uint8_t*>(output.gamertag);
  CHECK(bytes[28] == 0); CHECK(bytes[29] == 'O');
  CHECK(bytes[30] == 0); CHECK(bytes[31] == 0);
}
}  // namespace xe::kernel::xam::test
