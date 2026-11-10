/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xboxkrnl/xecrypt_signed_hash.h"
#include "xenia/kernel/xam/system_font.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/testing/signed_hash_vectors.h"
#include "xenia/kernel/xboxkrnl/xecrypt_rc4.h"
namespace xe::kernel::xboxkrnl::test {
namespace {
std::vector<uint8_t> Hex(std::string_view text) {
  std::vector<uint8_t> bytes;
  auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < text.size(); i += 2) {
    bytes.push_back(uint8_t((digit(text[i]) << 4) | digit(text[i + 1])));
  }
  return bytes;
}
TEST_CASE(
    "RotSumSha independent answers retain each span tail and truncation "
    "boundary",
    "[crypt][signed_hash]") {
  struct Vector {
    std::string_view first, second, digest;
  };
  const Vector vectors[] = {
      {"", "", "c1f48a59c2629f48b955483785512db79040b3c1"},
      {"616263", "", "9d274d6e00e4580bd200f1af781bd5c1efb7651d"},
      {"000102030405060708090a0b0c0d0e0f10", "",
       "ad48b464ce8fab68075fedcf065322babcea8593"},
      {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
       "808080808080808080", "7036c4b749aeb612edeb15d0e1a5a1d0cdf20052"},
      {"41", "42424242424242", "672462ebaa63234dc2949293053242142176c4ba"},
      {"4142424242424242", "", "f69d1a8d46c653fadb3cdfb03d31b357102e0574"},
      {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e",
       "000102030405060708", "a80589d24a65a39d1327a556b41ae3e819c699f7"},
  };
  for (const auto& vector : vectors) {
    auto first = Hex(vector.first), second = Hex(vector.second),
         expected = Hex(vector.digest);
    const auto before_first = first, before_second = second;
    std::array<uint8_t, 20> result;
    REQUIRE(XeCryptComputeRotSumSha(first, second, result));
    CHECK(std::equal(result.begin(), result.end(), expected.begin()));
    CHECK(first == before_first);
    CHECK(second == before_second);
  }
}
TEST_CASE("Xbox BeSig validates real salted format for both public exponents",
          "[crypt][signed_hash]") {
  const auto hash = Hex(kBeSigHash), salt = Hex(kBeSigSalt);
  for (auto pair : {std::pair{kBeSigKey3, kBeSigValid3},
                    std::pair{kBeSigKey65537, kBeSigValid65537}}) {
    const auto key = Hex(pair.first), signature = Hex(pair.second);
    const auto key_before = key, sig_before = signature;
    REQUIRE(XeCryptVerifyBeSignature(signature, hash, salt, key));
    CHECK(key == key_before);
    CHECK(signature == sig_before);
    for (size_t i = 0; i < hash.size(); ++i) {
      auto bad = hash;
      bad[i] ^= 1;
      CHECK_FALSE(XeCryptVerifyBeSignature(signature, bad, salt, key));
    }
    for (size_t i = 0; i < salt.size(); ++i) {
      auto bad = salt;
      bad[i] ^= 1;
      CHECK_FALSE(XeCryptVerifyBeSignature(signature, hash, bad, key));
    }
    for (size_t i : {size_t(0), size_t(127), size_t(255)}) {
      auto bad = signature;
      bad[i] ^= 1;
      CHECK_FALSE(XeCryptVerifyBeSignature(bad, hash, salt, key));
    }
  }
}
TEST_CASE(
    "Xbox BeSig checks signed malformed padding independently of RSA validity",
    "[crypt][signed_hash]") {
  const auto key = Hex(kBeSigKey3), hash = Hex(kBeSigHash),
             salt = Hex(kBeSigSalt);
  for (auto vector : {kBeSigBadFirstPadding, kBeSigBadPadding, kBeSigBadMarker,
                      kBeSigBadSalt, kBeSigBadEnd, kBeSigBadEncodedHash}) {
    CHECK_FALSE(XeCryptVerifyBeSignature(Hex(vector), hash, salt, key));
  }
  const auto alias_hash = Hex(kBeSigAliasHash);
  REQUIRE(
      XeCryptVerifyBeSignature(Hex(kBeSigAliasValid), alias_hash, salt, key));
  // Same RSA residue, distinct noncanonical integer; reducing modulo N would
  // incorrectly accept this additive-N alias of the valid signature.
  CHECK_FALSE(
      XeCryptVerifyBeSignature(Hex(kBeSigAliasInvalid), alias_hash, salt, key));
}
TEST_CASE("Xbox BeSig rejects malformed key shape and exact operand extents",
          "[crypt][signed_hash]") {
  const auto key = Hex(kBeSigKey3), sig = Hex(kBeSigValid3),
             hash = Hex(kBeSigHash), salt = Hex(kBeSigSalt);
  for (auto field : {0u, 1u, 31u, 33u, UINT32_MAX}) {
    auto bad = key;
    bad[0] = uint8_t(field >> 24);
    bad[1] = uint8_t(field >> 16);
    bad[2] = uint8_t(field >> 8);
    bad[3] = uint8_t(field);
    CHECK_FALSE(XeCryptVerifyBeSignature(sig, hash, salt, bad));
  }
  for (auto exponent : {0u, 1u, 2u, 7u, UINT32_MAX}) {
    auto bad = key;
    for (size_t i = 0; i < 4; ++i) {
      bad[4 + i] = uint8_t(exponent >> (24 - i * 8));
    }
    CHECK_FALSE(XeCryptVerifyBeSignature(sig, hash, salt, bad));
  }
  for (size_t length : {size_t(0), size_t(16), size_t(271)}) {
    CHECK_FALSE(XeCryptVerifyBeSignature(
        sig, hash, salt, std::span<const uint8_t>(key).first(length)));
  }
  CHECK_FALSE(XeCryptVerifyBeSignature(std::span<const uint8_t>(sig).first(255),
                                       hash, salt, key));
  CHECK_FALSE(XeCryptVerifyBeSignature(
      sig, std::span<const uint8_t>(hash).first(19), salt, key));
  CHECK_FALSE(XeCryptVerifyBeSignature(
      sig, hash, std::span<const uint8_t>(salt).first(9), key));
  auto oversized = key;
  oversized.push_back(0);
  CHECK_FALSE(XeCryptVerifyBeSignature(sig, hash, salt, oversized));
  auto even = key;
  even[23] &= 0xFE;
  CHECK_FALSE(XeCryptVerifyBeSignature(sig, hash, salt, even));
  auto empty = key;
  std::fill(empty.begin() + 16, empty.end(), 0);
  CHECK_FALSE(XeCryptVerifyBeSignature(sig, hash, salt, empty));
  auto modulus_signature = std::vector<uint8_t>(key.begin() + 16, key.end());
  CHECK_FALSE(XeCryptVerifyBeSignature(modulus_signature, hash, salt, key));
}
TEST_CASE("Promoted RC4 core retains known output and split-call state",
          "[crypt][signed_hash][rc4]") {
  const std::array<uint8_t, 3> key{'K', 'e', 'y'};
  const std::array<uint8_t, 9> expected{0xBB, 0xF3, 0x16, 0xE8, 0xD9,
                                        0x40, 0xAF, 0x0A, 0xD3};
  std::array<uint8_t, 9> data{'P', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'};
  XECRYPT_RC4_STATE state;
  REQUIRE(XeCryptRc4Key(state, key));
  XeCryptRc4Ecb(state, std::span<uint8_t>(data).first(4));
  XeCryptRc4Ecb(state, std::span<uint8_t>(data).subspan(4));
  CHECK(data == expected);
  REQUIRE(XeCryptRc4Key(state, key));
  XeCryptRc4Ecb(state, data);
  const std::array<uint8_t, 9> plain{'P', 'l', 'a', 'i', 'n',
                                     't', 'e', 'x', 't'};
  CHECK(data == plain);
  CHECK_FALSE(XeCryptRc4Key(state, {}));
  CHECK(state.i == 0);
  CHECK(state.j == 0);
  for (size_t i = 0; i < 256; ++i) {
    CHECK(state.S[i] == i);
  }
}
}  // namespace
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

void FontHeaderWord(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (size_t i = 0; i != 4; ++i) {
    bytes[at + i] = uint8_t(value >> (24 - i * 8));
  }
}
}  // namespace
TEST_CASE("System font validator rejects bounded malformed containers atomically",
          "[crypt][system_font]") {
  for (const size_t extent : {size_t(0), size_t(1), size_t(3), size_t(4),
                              size_t(0x103), size_t(0x117)}) {
    RejectFontWithoutChanges(std::vector<uint8_t>(extent));
  }
  // A plausible header with no authentic signature must never become a font.
  std::vector<uint8_t> header(4096);
  std::copy_n("xttf", 4, header.begin());
  FontHeaderWord(header, 0x104, 4096 - 0x104);
  FontHeaderWord(header, 0x108, uint32_t(header.size()));
  FontHeaderWord(header, 0x10C, 1);
  FontHeaderWord(header, 0x110, 12);
  FontHeaderWord(header, 0x114, 0x10000);
  RejectFontWithoutChanges(header);
  auto bad_magic = header;
  bad_magic[0] = 'X';
  RejectFontWithoutChanges(bad_magic);
  for (const auto [at, word] : {
           std::pair{size_t(0x104), uint32_t(0)},
           std::pair{size_t(0x104), UINT32_MAX},
           std::pair{size_t(0x108), uint32_t(header.size() + 1)},
           std::pair{size_t(0x10C), UINT32_MAX},
           std::pair{size_t(0x110), UINT32_MAX},
           std::pair{size_t(0x114), uint32_t(0)}}) {
    auto malformed = header;
    FontHeaderWord(malformed, at, word);
    RejectFontWithoutChanges(malformed);
  }
  std::vector<uint8_t> oversized(xam::kMaximumSystemFontBytes + 1);
  std::copy_n("xttf", 4, oversized.begin());
  RejectFontWithoutChanges(oversized);
}
TEST_CASE("Generated font recognition rejects unrelated containers without granting validity",
          "[crypt][system_font]") {
  CHECK_FALSE(xam::IsGeneratedSystemFontSubstitute({}));
  std::vector<uint8_t> bytes(1257472);
  CHECK_FALSE(xam::IsGeneratedSystemFontSubstitute(bytes));
  std::copy_n("xttf", 4, bytes.begin());
  CHECK_FALSE(xam::IsGeneratedSystemFontSubstitute(bytes));
  RejectFontWithoutChanges(bytes);
}
}  // namespace xe::kernel::xboxkrnl::test
