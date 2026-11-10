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
#include <climits>
#include <cstring>
#include <vector>

#include "third_party/mbedtls/include/mbedtls/aes.h"
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "xenia/base/lzx.h"

namespace xe {
namespace {
constexpr uint8_t kKeys[4][16] = {
    {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB,
     0x08, 0x96, 0xBF, 0x91},
    {},
    {0xA2, 0x6C, 0x10, 0xF7, 0x1F, 0xD9, 0x35, 0xE9, 0x8B, 0x99, 0x92, 0x2C,
     0xE9, 0x32, 0x15, 0x72},
    {0xA8, 0xB0, 0x05, 0x12, 0xED, 0xE3, 0x63, 0x8D, 0xC6, 0x58, 0xB3, 0x10,
     0x1F, 0x9F, 0x50, 0xD1}};

uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}

class Aes {
 public:
  explicit Aes(const std::array<uint8_t, 16>& key) {
    mbedtls_aes_init(&context_);
    valid_ = mbedtls_aes_setkey_dec(&context_, key.data(), 128) == 0;
  }
  ~Aes() { mbedtls_aes_free(&context_); }
  bool Decrypt(std::span<const uint8_t> source,
               std::span<uint8_t> destination) {
    if (source.size() % 16 || destination.size() < source.size()) {
      return false;
    }
    const size_t size = source.size();
    return valid_ && (!size || mbedtls_aes_crypt_cbc(
                                   &context_, MBEDTLS_AES_DECRYPT, size, iv_,
                                   source.data(), destination.data()) == 0);
  }

 private:
  mbedtls_aes_context context_;
  uint8_t iv_[16]{};
  bool valid_ = false;
};
}  // namespace

const uint8_t* XexImageMasterKey(bool xex1, bool devkit) {
  return kKeys[(xex1 ? 2 : 0) + (devkit ? 1 : 0)];
}

bool DecryptXexImageSessionKey(std::span<const uint8_t, 16> encrypted,
                               std::span<const uint8_t, 16> master,
                               std::span<uint8_t, 16> session) {
  std::array<uint8_t, 16> key;
  std::copy(master.begin(), master.end(), key.begin());
  return Aes(key).Decrypt(encrypted, session);
}

XexImageResult DecodeXexImage(std::span<const uint8_t> source,
                              std::span<uint8_t> destination,
                              const XexImageOptions& options) {
  if (!source.empty() && !destination.empty()) {
    const auto input = reinterpret_cast<uintptr_t>(source.data());
    const auto output = reinterpret_cast<uintptr_t>(destination.data());
    // Subtraction avoids wrapping an address when comparing span endpoints.
    const bool overlaps = input <= output ? output - input < source.size()
                                          : input - output < destination.size();
    if (overlaps) {
      return XexImageResult::kInvalidFormat;
    }
  }
  if (options.encryption != XexImageEncryption::kNone &&
      options.encryption != XexImageEncryption::kNormal) {
    return XexImageResult::kInvalidFormat;
  }
  if (options.encryption == XexImageEncryption::kNormal && source.size() % 16) {
    return XexImageResult::kInvalidFormat;
  }
  std::fill(destination.begin(), destination.end(), 0);
  Aes aes(options.session_key);
  auto copy = [&](std::span<const uint8_t> from, std::span<uint8_t> to) {
    if (options.encryption == XexImageEncryption::kNormal) {
      return aes.Decrypt(from, to);
    }
    std::copy(from.begin(), from.end(), to.begin());
    return true;
  };

  switch (options.compression) {
    case XexImageCompression::kNone:
      if (source.size() > destination.size()) {
        return XexImageResult::kOutOfBounds;
      }
      return copy(source, destination) ? XexImageResult::kSuccess
                                       : XexImageResult::kInvalidKey;

    case XexImageCompression::kBasic: {
      size_t in = 0, out = 0;
      for (const auto& block : options.basic_blocks) {
        if (options.encryption == XexImageEncryption::kNormal &&
            block.data_size % 16) {
          return XexImageResult::kInvalidFormat;
        }
        if (block.data_size > source.size() - in ||
            block.data_size > destination.size() - out ||
            block.zero_size > destination.size() - out - block.data_size) {
          return XexImageResult::kOutOfBounds;
        }
        if (!copy(source.subspan(in, block.data_size),
                  destination.subspan(out, block.data_size))) {
          return XexImageResult::kInvalidKey;
        }
        in += block.data_size;
        out += size_t(block.data_size) + block.zero_size;
      }
      return XexImageResult::kSuccess;
    }

    case XexImageCompression::kNormal: {
      // These are also the limits of the shared mspack memory adapter.
      if (source.size() >= INT_MAX || destination.size() >= INT_MAX ||
          options.normal_window_size < (1u << 15) ||
          options.normal_window_size > (1u << 21) ||
          (options.normal_window_size & (options.normal_window_size - 1))) {
        return XexImageResult::kInvalidFormat;
      }
      std::vector<uint8_t> decrypted;
      auto input = source;
      if (options.encryption == XexImageEncryption::kNormal) {
        decrypted.resize(source.size());
        if (!aes.Decrypt(source, decrypted)) {
          return XexImageResult::kInvalidKey;
        }
        input = decrypted;
      }
      std::vector<uint8_t> compressed;
      compressed.reserve(source.size());
      size_t offset = 0;
      uint32_t block_size = options.normal_first_block_size;
      auto expected_hash = options.normal_first_block_hash;
      while (block_size) {
        if (block_size < 26 || block_size > input.size() - offset) {
          return XexImageResult::kOutOfBounds;
        }
        auto block = input.subspan(offset, block_size);
        uint8_t hash[20];
        if (mbedtls_sha1(block.data(), block.size(), hash) ||
            !std::equal(expected_hash.begin(), expected_hash.end(), hash)) {
          return XexImageResult::kBlockHashMismatch;
        }
        const uint32_t next_size = ReadBE32(block.data());
        std::copy_n(block.data() + 4, 20, expected_hash.begin());
        size_t chunk_offset = 24;
        bool terminated = false;
        while (chunk_offset + 2 <= block.size()) {
          const uint32_t chunk_size =
              uint32_t(block[chunk_offset]) << 8 | block[chunk_offset + 1];
          chunk_offset += 2;
          if (!chunk_size) {
            terminated = true;
            break;
          }
          if (chunk_size > block.size() - chunk_offset ||
              chunk_size > source.size() - compressed.size()) {
            return XexImageResult::kOutOfBounds;
          }
          compressed.insert(compressed.end(), block.begin() + chunk_offset,
                            block.begin() + chunk_offset + chunk_size);
          chunk_offset += chunk_size;
        }
        if (!terminated) {
          return XexImageResult::kOutOfBounds;
        }
        offset += block_size;
        block_size = next_size;
      }
      return lzx_decompress(compressed.data(), compressed.size(),
                            destination.data(), destination.size(),
                            options.normal_window_size, nullptr, 0) == 0
                 ? XexImageResult::kSuccess
                 : XexImageResult::kDecompressionFailed;
    }
  }
  return XexImageResult::kInvalidFormat;
}
}  // namespace xe
