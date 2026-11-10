/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/xex_image.h"

#include <algorithm>
#include <array>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha1.h"

namespace xe::test {
TEST_CASE("Shared XEX codec refuses overlapping buffers before modification",
          "[xex-image]") {
  for (auto compression :
       {XexImageCompression::kNone, XexImageCompression::kBasic,
        XexImageCompression::kNormal}) {
    for (auto encryption :
         {XexImageEncryption::kNone, XexImageEncryption::kNormal}) {
      std::array<uint8_t, 64> storage;
      for (size_t i = 0; i < storage.size(); ++i) {
        storage[i] = uint8_t(i);
      }
      const auto original = storage;
      XexImageOptions options;
      options.compression = compression;
      options.encryption = encryption;
      const std::array<XexImageBasicBlock, 1> blocks{{{16, 0}}};
      options.basic_blocks = blocks;
      options.normal_window_size = 1u << 15;
      const auto bytes = std::span<uint8_t>(storage);
      // Equal starts and partial overlap in both directions all fail intact.
      for (const auto offsets :
           {std::array<size_t, 2>{8, 8}, std::array<size_t, 2>{8, 0},
            std::array<size_t, 2>{8, 16}}) {
        REQUIRE(DecodeXexImage(bytes.subspan(offsets[0], 16),
                               bytes.subspan(offsets[1], 24),
                               options) == XexImageResult::kInvalidFormat);
        REQUIRE(storage == original);
      }
    }
  }
  std::array<uint8_t, 8> adjacent{1, 2, 3, 4, 0xCD, 0xCD, 0xCD, 0xCD};
  auto bytes = std::span<uint8_t>(adjacent);
  REQUIRE(DecodeXexImage(bytes.first(4), bytes.last(4), {}) ==
          XexImageResult::kSuccess);
  REQUIRE((adjacent == std::array<uint8_t, 8>{1, 2, 3, 4, 1, 2, 3, 4}));
  REQUIRE(DecodeXexImage(bytes.first(0), bytes, {}) ==
          XexImageResult::kSuccess);
  REQUIRE(std::all_of(adjacent.begin(), adjacent.end(),
                      [](uint8_t byte) { return byte == 0; }));
}

TEST_CASE("Shared XEX image codec preserves plain and sparse images",
          "[xex-image]") {
  const std::array<uint8_t, 4> source{1, 2, 3, 4};
  std::array<uint8_t, 8> destination;
  destination.fill(0xCD);
  XexImageOptions options;
  REQUIRE(DecodeXexImage(source, destination, options) ==
          XexImageResult::kSuccess);
  REQUIRE((destination == std::array<uint8_t, 8>{1, 2, 3, 4, 0, 0, 0, 0}));

  std::array<XexImageBasicBlock, 2> blocks{{{2, 3}, {2, 1}}};
  options.compression = XexImageCompression::kBasic;
  options.basic_blocks = blocks;
  REQUIRE(DecodeXexImage(source, destination, options) ==
          XexImageResult::kSuccess);
  REQUIRE((destination == std::array<uint8_t, 8>{1, 2, 0, 0, 0, 3, 4, 0}));

  blocks[1].zero_size = 2;
  REQUIRE(DecodeXexImage(source, destination, options) ==
          XexImageResult::kOutOfBounds);
  blocks[1] = {3, 0};
  REQUIRE(DecodeXexImage(source, destination, options) ==
          XexImageResult::kOutOfBounds);
}

TEST_CASE("Shared XEX AES decodes a fixed known ciphertext", "[xex-image]") {
  // AES-128 zero key, zero plaintext and zero CBC IV.
  const std::array<uint8_t, 16> cipher{0x66, 0xE9, 0x4B, 0xD4, 0xEF, 0x8A,
                                       0x2C, 0x3B, 0x88, 0x4C, 0xFA, 0x59,
                                       0xCA, 0x34, 0x2B, 0x2E};
  std::array<uint8_t, 16> output;
  output.fill(0xCD);
  XexImageOptions options;
  options.encryption = XexImageEncryption::kNormal;
  REQUIRE(DecodeXexImage(cipher, output, options) == XexImageResult::kSuccess);
  REQUIRE(std::all_of(output.begin(), output.end(),
                      [](uint8_t b) { return b == 0; }));
  std::array<uint8_t, 16> master{};
  REQUIRE(DecryptXexImageSessionKey(cipher, master, output));
  REQUIRE(output == master);

  SECTION("Partial ciphertext and short destinations are rejected") {
    REQUIRE(DecodeXexImage(std::span<const uint8_t>(cipher).first(15), output,
                           options) == XexImageResult::kInvalidFormat);
    REQUIRE(DecodeXexImage(cipher, std::span<uint8_t>(output).first(15),
                           options) == XexImageResult::kOutOfBounds);
    const std::array<XexImageBasicBlock, 1> partial{{{15, 1}}};
    options.compression = XexImageCompression::kBasic;
    options.basic_blocks = partial;
    REQUIRE(DecodeXexImage(cipher, output, options) ==
            XexImageResult::kInvalidFormat);
  }

  SECTION("CBC state continues between sparse basic blocks") {
    std::vector<uint8_t> chained(cipher.begin(), cipher.end());
    const uint8_t second[] = {0xF7, 0x95, 0xBD, 0x4A, 0x52, 0xE2, 0x9E, 0xD7,
                              0x13, 0xD3, 0x13, 0xFA, 0x20, 0xE9, 0x8D, 0xBC};
    chained.insert(chained.end(), std::begin(second), std::end(second));
    const std::array<XexImageBasicBlock, 2> blocks{{{16, 8}, {16, 8}}};
    options.compression = XexImageCompression::kBasic;
    options.basic_blocks = blocks;
    std::array<uint8_t, 50> guarded;
    guarded.fill(0xCD);
    REQUIRE(DecodeXexImage(chained, std::span<uint8_t>(guarded).subspan(1, 48),
                           options) == XexImageResult::kSuccess);
    REQUIRE(guarded.front() == 0xCD);
    REQUIRE(guarded.back() == 0xCD);
    REQUIRE(std::all_of(guarded.begin() + 1, guarded.end() - 1,
                        [](uint8_t value) { return value == 0; }));
  }
}

TEST_CASE("Shared XEX normal compression verifies blocks and decodes LZX",
          "[xex-image]") {
  // A standard LZX uncompressed block for sixteen literal bytes. Bit words
  // are little-endian; the block type/length are MSB-first inside each word.
  std::vector<uint8_t> lzx{0x00, 0x30, 0x00, 0x01, 1, 0, 0, 0,
                           1,    0,    0,    0,    1, 0, 0, 0};
  for (uint8_t i = 0; i < 16; ++i) {
    lzx.push_back(i);
  }
  std::vector<uint8_t> source(24, 0);  // Last block's next-size/hash are zero.
  source.push_back(0);
  source.push_back(uint8_t(lzx.size()));
  source.insert(source.end(), lzx.begin(), lzx.end());
  source.push_back(0);
  source.push_back(0);
  XexImageOptions options;
  options.compression = XexImageCompression::kNormal;
  options.normal_window_size = 1 << 15;
  options.normal_first_block_size = uint32_t(source.size());
  REQUIRE(mbedtls_sha1(source.data(), source.size(),
                       options.normal_first_block_hash.data()) == 0);
  std::array<uint8_t, 16> output;
  output.fill(0xCD);

  SECTION("A failed attempt does not prevent a subsequent correct retry") {
    options.normal_first_block_hash[0] ^= 1;
    REQUIRE(DecodeXexImage(source, output, options) ==
            XexImageResult::kBlockHashMismatch);
    options.normal_first_block_hash[0] ^= 1;
    REQUIRE(DecodeXexImage(source, output, options) ==
            XexImageResult::kSuccess);
    for (size_t i = 0; i < output.size(); ++i) {
      REQUIRE(output[i] == i);
    }
  }
  SECTION("Truncated and overflowing chunk chains are rejected") {
    auto truncated = source;
    truncated.pop_back();
    REQUIRE(DecodeXexImage(truncated, output, options) ==
            XexImageResult::kOutOfBounds);
    source[24] = source[25] = 0xFF;
    REQUIRE(mbedtls_sha1(source.data(), source.size(),
                         options.normal_first_block_hash.data()) == 0);
    REQUIRE(DecodeXexImage(source, output, options) ==
            XexImageResult::kOutOfBounds);
  }
  SECTION("Only regular LZX window sizes reach the decoder") {
    for (uint32_t window : {0u, 1u << 14, 1u << 22, 1u << 31}) {
      options.normal_window_size = window;
      REQUIRE(DecodeXexImage(source, output, options) ==
              XexImageResult::kInvalidFormat);
    }
    options.normal_window_size = 1u << 21;
    REQUIRE(DecodeXexImage(source, output, options) ==
            XexImageResult::kSuccess);
  }
}
}  // namespace xe::test
