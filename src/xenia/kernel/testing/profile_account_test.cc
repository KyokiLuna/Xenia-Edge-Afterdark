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
}  // namespace xe::kernel::xam::test
