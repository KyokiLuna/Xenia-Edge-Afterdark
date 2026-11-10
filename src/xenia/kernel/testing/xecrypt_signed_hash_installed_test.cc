/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xecrypt_signed_hash.h"
#include "xenia/kernel/xam/system_font.h"
namespace xe::kernel::xboxkrnl {
dword_result_t XeKeysGetKey_entry(word_t, lpvoid_t, lpdword_t);
}
namespace xe::kernel::xboxkrnl::test {
TEST_CASE(
    "Installed font RotSumSha and salted BeSig authenticate real preserved "
    "bytes",
    "[.installed-signed-hash][crypt][signed_hash]") {
  const char* source = std::getenv("XENIA_TEST_SYSTEM_XTT");
  REQUIRE(source);
  REQUIRE(std::filesystem::file_size(source) == 1392640);
  std::ifstream file(source, std::ios::binary);
  std::array<uint8_t, 0x118> header;
  REQUIRE(static_cast<bool>(
      file.read(reinterpret_cast<char*>(header.data()), header.size())));
  const uint32_t size = xe::load_and_swap<uint32_t>(header.data() + 0x104);
  REQUIRE(size == 61180);
  std::vector<uint8_t> input(size);
  file.seekg(0x104);
  REQUIRE(static_cast<bool>(
      file.read(reinterpret_cast<char*>(input.data()), input.size())));
  std::array<uint8_t, 32> input_sha;
  REQUIRE(mbedtls_sha256(input.data(), input.size(), input_sha.data(), 0) == 0);
  const std::array<uint8_t, 32> expected_input{
      0x7c, 0xe3, 0x7a, 0x29, 0xd7, 0x7e, 0x0b, 0x4c, 0xe9, 0x17, 0x60,
      0xa1, 0x6b, 0xde, 0x9c, 0xd9, 0x1a, 0xd8, 0xdf, 0x29, 0xe6, 0x3a,
      0x9a, 0x42, 0x53, 0xa4, 0x51, 0xe6, 0xad, 0xeb, 0xe7, 0xea};
  REQUIRE(input_sha == expected_input);
  std::array<uint8_t, 20> hash;
  REQUIRE(XeCryptComputeRotSumSha(input, {}, hash));
  const std::array<uint8_t, 20> expected{
      0x4a, 0x85, 0x8c, 0x94, 0x75, 0xad, 0x43, 0x4c, 0x67, 0x13,
      0xf3, 0xef, 0x08, 0x32, 0xfa, 0xcb, 0x6f, 0xc8, 0x6a, 0xe1};
  REQUIRE(hash == expected);
  std::array<uint8_t, 272> key;
  uint32_t key_size = xe::byte_swap<uint32_t>(272);
  REQUIRE(uint32_t(XeKeysGetKey_entry(uint16_t(0x39), key.data(), &key_size)) ==
          X_STATUS_SUCCESS);
  REQUIRE(xe::byte_swap<uint32_t>(key_size) == 272);
  const std::array<uint8_t, 10> salt{'X', 'B', 'O', 'X', '3',
                                     '6', '0', 'X', 'T', 'T'};
  const auto signature = std::span<const uint8_t>(header).subspan(4, 256);
  REQUIRE(XeCryptVerifyBeSignature(signature, hash, salt, key));
  auto bad_hash = hash;
  bad_hash[0] ^= 1;
  CHECK_FALSE(XeCryptVerifyBeSignature(signature, bad_hash, salt, key));
  auto bad_salt = salt;
  bad_salt[0] ^= 1;
  CHECK_FALSE(XeCryptVerifyBeSignature(signature, hash, bad_salt, key));
  std::array<uint8_t, 256> bad_signature;
  std::copy(signature.begin(), signature.end(), bad_signature.begin());
  bad_signature[0] ^= 1;
  CHECK_FALSE(XeCryptVerifyBeSignature(bad_signature, hash, salt, key));
}
}  // namespace xe::kernel::xboxkrnl::test

namespace xe::kernel::xboxkrnl::test {
namespace {
xam::SystemFontInfo FontOutputCanary() {
  xam::SystemFontInfo info;
  info.signature.fill(0xA5);
  info.base_signature.fill(0x5A);
  info.signed_bytes = 0x11223344;
  info.glyph_pages = 0x55667788;
  info.is_base = true;
  return info;
}
bool SameFontInfo(const xam::SystemFontInfo& a,
                  const xam::SystemFontInfo& b) {
  return a.signature == b.signature && a.base_signature == b.base_signature &&
         a.signed_bytes == b.signed_bytes && a.glyph_pages == b.glyph_pages &&
         a.is_base == b.is_base;
}
void RejectFontWithoutChanges(const std::vector<uint8_t>& bytes) {
  const auto before = bytes;
  const auto canary = FontOutputCanary();
  auto output = canary;
  CHECK_FALSE(xam::ValidateSystemFont(bytes, output));
  CHECK(SameFontInfo(output, canary));
  CHECK(bytes == before);
}

std::vector<uint8_t> ReadOwnedFontFixture(const char* variable) {
  const char* value = std::getenv(variable);
  REQUIRE(value);
  REQUIRE(*value);
  std::error_code error;
  const auto size = std::filesystem::file_size(value, error);
  REQUIRE_FALSE(error);
  REQUIRE(size >= 0x118);
  REQUIRE(size <= xam::kMaximumSystemFontBytes);
  std::ifstream file(value, std::ios::binary);
  REQUIRE(file.is_open());
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  REQUIRE(file.gcount() == std::streamsize(bytes.size()));
  return bytes;
}
xam::SystemFontInfo AuthenticateOwnedFont(const std::vector<uint8_t>& bytes) {
  xam::SystemFontInfo info;
  REQUIRE(xam::ValidateSystemFont(bytes, info));
  CHECK(std::equal(bytes.begin() + 4, bytes.begin() + 0x104,
                   info.signature.begin()));
  CHECK(info.signed_bytes == xe::load_and_swap<uint32_t>(bytes.data() + 0x104));
  REQUIRE(info.glyph_pages > 0);
  REQUIRE(uint64_t(0x104) + info.signed_bytes +
              uint64_t(info.glyph_pages) * 4096 == bytes.size());
  return info;
}
void RejectOwnedFontCorruptions(const std::vector<uint8_t>& bytes,
                                const xam::SystemFontInfo& info) {
  const size_t glyphs = size_t(0x104) + info.signed_bytes;
  for (const size_t at : {size_t(4 + 127), size_t(0x118), glyphs,
                          glyphs + size_t(info.glyph_pages - 1) * 4096 + 4095}) {
    REQUIRE(at < bytes.size());
    auto damaged = bytes;
    damaged[at] ^= 0x80;
    // Signature, authenticated compressed metadata, and both boundary glyph
    // pages are separate trust boundaries; no test signs replacement bytes.
    RejectFontWithoutChanges(damaged);
  }
}
}  // namespace
TEST_CASE("Installed signed system font validates every preserved glyph page",
          "[.installed-system-font][crypt][system_font]") {
  const auto bytes = ReadOwnedFontFixture("XENIA_TEST_SYSTEM_XTT");
  const auto info = AuthenticateOwnedFont(bytes);
  CHECK_FALSE(xam::IsGeneratedSystemFontSubstitute(bytes));
  RejectOwnedFontCorruptions(bytes, info);
}
TEST_CASE("Owned JK font patch binds only to its authenticated base",
          "[.owned-jk-system-font][crypt][system_font]") {
  // Explicit opt-in fixtures: absent environment fails this hidden case rather
  // than silently skipping it or turning synthetic bytes into a positive base.
  const auto base = ReadOwnedFontFixture("XENIA_TEST_SYSTEM_JK_XTT");
  const auto patch = ReadOwnedFontFixture("XENIA_TEST_SYSTEM_JK_XTTP");
  const auto foreign = ReadOwnedFontFixture("XENIA_TEST_SYSTEM_XTT");
  const auto base_info = AuthenticateOwnedFont(base);
  const auto patch_info = AuthenticateOwnedFont(patch);
  const auto foreign_info = AuthenticateOwnedFont(foreign);
  REQUIRE(base_info.is_base);
  REQUIRE_FALSE(patch_info.is_base);
  REQUIRE(base_info.signature != foreign_info.signature);
  CHECK(patch_info.base_signature == base_info.signature);
  CHECK(patch_info.base_signature != foreign_info.signature);
  CHECK_FALSE(xam::IsGeneratedSystemFontSubstitute(base));
  CHECK_FALSE(xam::IsGeneratedSystemFontSubstitute(patch));
  RejectOwnedFontCorruptions(base, base_info);
  RejectOwnedFontCorruptions(patch, patch_info);
}
}  // namespace xe::kernel::xboxkrnl::test
