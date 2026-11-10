/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/xam/system_font.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_crypt.h"

namespace xe::kernel::xam {
namespace {
uint32_t Word(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}
bool Fits(uint64_t at, uint64_t size, size_t extent) {
  return at <= extent && size <= extent - at;
}
struct Table {
  uint32_t tag, offset, size;
};
constexpr uint32_t kXttf = 0x78747466, kXchk = 0x7863686B, kXglf = 0x78676C66;
}  // namespace

bool ValidateSystemFont(std::span<const uint8_t> bytes,
                        SystemFontInfo& output) {
  constexpr size_t kHeader = 0x118, kMaximumTables = 16 * 1024 * 1024;
  if (bytes.size() < kHeader || bytes.size() > kMaximumSystemFontBytes ||
      std::memcmp(bytes.data(), "xttf", 4)) {
    return false;
  }
  const uint32_t signed_size = Word(bytes.data() + 0x104);
  const uint32_t packed = Word(bytes.data() + 0x10C);
  const uint32_t unpacked = Word(bytes.data() + 0x110);
  if (Word(bytes.data() + 0x108) != bytes.size() ||
      Word(bytes.data() + 0x114) != 0x10000 || signed_size < 0x14 ||
      !Fits(0x104, signed_size, bytes.size()) ||
      !Fits(kHeader, packed, size_t(0x104) + signed_size) || !packed ||
      unpacked < 12 || unpacked > kMaximumTables ||
      !xboxkrnl::XeKeysVerifyXttFontSignature(
          bytes.subspan(4, 256), bytes.subspan(0x104, signed_size))) {
    return false;
  }
  std::vector<uint8_t> sfnt(unpacked);
  size_t actual = sfnt.size();
  // The packed extent includes authenticated alignment padding in real XTT.
  if (zng_uncompress(sfnt.data(), &actual, bytes.data() + kHeader, packed) !=
          Z_OK ||
      actual != unpacked || Word(sfnt.data()) != 0x10000) {
    return false;
  }
  const size_t count = size_t(sfnt[4]) << 8 | sfnt[5];
  if (!count || count > 256 || !Fits(12, count * 16, sfnt.size())) {
    return false;
  }
  std::vector<Table> tables;
  std::optional<Table> descriptor, hashes, glyphs;
  for (size_t i = 0; i != count; ++i) {
    const auto* record = sfnt.data() + 12 + i * 16;
    const Table t{Word(record), Word(record + 8), Word(record + 12)};
    if (std::any_of(tables.begin(), tables.end(),
                    [&](const Table& old) { return old.tag == t.tag; })) {
      return false;
    }
    tables.push_back(t);
    // xglf addresses bytes in the physical font, not the inflated sfnt buffer.
    if (t.tag == kXglf) {
      glyphs = t;
    } else if (!Fits(t.offset, t.size, sfnt.size())) {
      return false;
    }
    if (t.tag == kXttf) {
      descriptor = t;
    }
    if (t.tag == kXchk) {
      hashes = t;
    }
  }
  if (!descriptor || descriptor->size != 264 || !hashes || !glyphs ||
      Word(sfnt.data() + descriptor->offset) != 0x10000 ||
      glyphs->offset != uint64_t(0x104) + signed_size ||
      (glyphs->offset & 4095) || !glyphs->size || (glyphs->size & 4095) ||
      !Fits(glyphs->offset, glyphs->size, bytes.size()) ||
      uint64_t(glyphs->offset) + glyphs->size != bytes.size() ||
      hashes->size != uint64_t(glyphs->size / 4096) * 20) {
    return false;
  }
  for (uint32_t i = 0; i != glyphs->size / 4096; ++i) {
    std::array<uint8_t, 20> digest;
    if (mbedtls_sha1(bytes.data() + glyphs->offset + size_t(i) * 4096, 4096,
                     digest.data()) ||
        !std::equal(digest.begin(), digest.end(),
                    sfnt.data() + hashes->offset + size_t(i) * 20)) {
      return false;
    }
  }
  SystemFontInfo value;
  std::copy_n(bytes.data() + 4, 256, value.signature.begin());
  std::copy_n(sfnt.data() + descriptor->offset + 8, 256,
              value.base_signature.begin());
  value.is_base =
      std::none_of(value.base_signature.begin(), value.base_signature.end(),
                   [](uint8_t b) { return b; });
  value.signed_bytes = signed_size;
  value.glyph_pages = glyphs->size / 4096;
  output = value;
  return true;
}

std::optional<std::string> SystemFontRecoveryBackupName(
    std::span<const uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > kMaximumSystemFontBytes) {
    return std::nullopt;
  }
  std::array<uint8_t, 32> digest;
  if (mbedtls_sha256(bytes.data(), bytes.size(), digest.data(), 0)) {
    return std::nullopt;
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string name;
  name.reserve(68);
  for (uint8_t byte : digest) {
    name.push_back(hex[byte >> 4]);
    name.push_back(hex[byte & 15]);
  }
  return name + ".xtt";
}

bool IsGeneratedSystemFontSubstitute(std::span<const uint8_t> bytes) {
  if (bytes.size() != 1257472 || std::memcmp(bytes.data(), "xttf", 4)) {
    return false;
  }
  constexpr std::array<uint8_t, 32> expected{
      0x7C, 0x78, 0x10, 0x9D, 0xA5, 0x0E, 0x71, 0x43, 0xB0, 0x80, 0x4E,
      0xB6, 0x71, 0xD5, 0x34, 0x7B, 0x8E, 0xD4, 0x59, 0xBF, 0xEF, 0xFD,
      0xD8, 0x54, 0x9E, 0x75, 0xE9, 0xA4, 0x51, 0xE2, 0x55, 0x02};
  std::array<uint8_t, 32> digest;
  return !mbedtls_sha256(bytes.data() + 0x104, bytes.size() - 0x104,
                         digest.data(), 0) &&
         digest == expected;
}
}  // namespace xe::kernel::xam
