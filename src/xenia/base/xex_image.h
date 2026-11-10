/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_BASE_XEX_IMAGE_H_
#define XENIA_BASE_XEX_IMAGE_H_

#include <array>
#include <cstdint>
#include <span>

namespace xe {

enum class XexImageCompression : uint16_t { kNone, kBasic, kNormal };
enum class XexImageEncryption : uint16_t { kNone, kNormal };
enum class XexImageResult {
  kSuccess,
  kInvalidFormat,
  kOutOfBounds,
  kInvalidKey,
  kBlockHashMismatch,
  kDecompressionFailed,
};

struct XexImageBasicBlock {
  uint32_t data_size;
  uint32_t zero_size;
};

struct XexImageOptions {
  XexImageCompression compression = XexImageCompression::kNone;
  XexImageEncryption encryption = XexImageEncryption::kNone;
  std::array<uint8_t, 16> session_key{};
  std::span<const XexImageBasicBlock> basic_blocks;
  uint32_t normal_window_size = 0;
  uint32_t normal_first_block_size = 0;
  std::array<uint8_t, 20> normal_first_block_hash{};
};

// The same retail/devkit candidates used by the runtime loader. No key files,
// configuration, guest memory, modules or threads are consulted.
const uint8_t* XexImageMasterKey(bool xex1, bool devkit);
bool DecryptXexImageSessionKey(std::span<const uint8_t, 16> encrypted,
                               std::span<const uint8_t, 16> master,
                               std::span<uint8_t, 16> session);

// Decode the headerless payload into caller-owned storage. The caller chooses
// allocation/ownership and validates the resulting executable/resource layout.
// Input and output must not overlap; an overlap returns kInvalidFormat before
// modifying either span. Empty spans and adjacent non-overlapping spans are OK.
// Failure never publishes a partially decoded image as valid.
XexImageResult DecodeXexImage(std::span<const uint8_t> source,
                              std::span<uint8_t> destination,
                              const XexImageOptions& options);

}  // namespace xe
#endif  // XENIA_BASE_XEX_IMAGE_H_
