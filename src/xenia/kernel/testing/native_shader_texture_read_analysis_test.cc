/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <initializer_list>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/native_shader_read_analysis.h"
#include "xenia/gpu/ucode.h"

namespace xe::gpu::test {
namespace {
using Analysis = NativeShaderReadAnalysis;
using Instruction = std::array<uint32_t, 3>;

// Synthetic instructions constructed from the common ucode.h bit fields, not
// supplied shader bytes, addresses, hashes, console goldens or GPU execution.
Instruction Texture(uint32_t slot, uint32_t coordinate = 0) {
  return {uint32_t(ucode::FetchOpcode::kTextureFetch) | coordinate << 5 |
              1u << 19 | slot << 20 | 4u << 26,
          0x688 | 3u << 12 | 3u << 14 | 3u << 16 | 7u << 18 | 3u << 24 |
              3u << 26 | 1u << 28,
          uint32_t(xenos::FetchOpDimension::k2D) << 14};
}

std::vector<uint8_t> ShaderBytes(
    std::initializer_list<Instruction> instructions, uint32_t sequence) {
  REQUIRE(instructions.size() <= 6);
  // Two CFs packed into 96 bits: exece(group1,count), then nop.
  std::vector<uint32_t> words{
      1u | uint32_t(instructions.size()) << 12 | sequence << 16,
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12 |
          uint32_t(ucode::AddressingMode::kAbsolute) << 11,
      0};
  for (const auto& instruction : instructions) {
    words.insert(words.end(), instruction.begin(), instruction.end());
  }
  std::vector<uint8_t> bytes;
  for (const auto word : words) {
    bytes.push_back(uint8_t(word >> 24));
    bytes.push_back(uint8_t(word >> 16));
    bytes.push_back(uint8_t(word >> 8));
    bytes.push_back(uint8_t(word));
  }
  return bytes;
}

void CheckNoPermission(const Analysis::Result& result) {
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.access_bounds_verified);
}

void CheckFailed(const Analysis::Result& result, Analysis::Error error) {
  CHECK(result.error == error);
  CHECK_FALSE(result.metadata_analyzed);
  CHECK_FALSE(result);
  CHECK(result.texture_fetch_bitmap == 0);
  CHECK(result.texture_fetches.empty());
  CHECK((result.vertex_fetch_bitmap == std::array<uint32_t, 3>{}));
  CHECK(result.vertex_fetches.empty());
  CheckNoPermission(result);
}

TEST_CASE("Pixel texture metadata retains slot31 and the exact instruction ABI",
          "[graphics][native_read_inventory][native_texture]") {
  auto instruction = Texture(31, 63);
  instruction[0] |= 62u << 12;
  instruction[0] = (instruction[0] & ~(63u << 26)) | 0x1Bu << 26;
  // Signed seven-bit bias is in sixteenths; signed XY offsets in halves.
  instruction[2] |=
      (uint32_t(-7) & 127) << 2 | (uint32_t(-3) & 31) << 16 | 5u << 21;
  auto bytes = ShaderBytes({instruction}, 1);
  const auto original = bytes;
  const auto result = Analysis::Analyze(xenos::ShaderType::kPixel, bytes);
  REQUIRE(result);
  CHECK(result.error == Analysis::Error::kNone);
  CHECK(result.stage == xenos::ShaderType::kPixel);
  CHECK(result.texture_fetch_bitmap == 0x80000000u);
  REQUIRE(result.texture_fetches.size() == 1);
  const auto& fetch = result.texture_fetches.front();
  CHECK(fetch.instruction_dword_offset == 3);
  CHECK(fetch.slot == 31);
  CHECK(fetch.dimension == uint32_t(xenos::FetchOpDimension::k2D));
  CHECK(fetch.normalized_coordinates);
  CHECK(fetch.coordinate_register == 63);
  CHECK(fetch.coordinate_swizzle == 0x1B);
  CHECK_FALSE(fetch.coordinate_relative);
  CHECK(fetch.dest_register == 62);
  CHECK(fetch.dest_swizzle == 0x688);
  CHECK_FALSE(fetch.dest_relative);
  CHECK(fetch.fetch_valid_only);
  CHECK(fetch.mag_filter == 3);
  CHECK(fetch.min_filter == 3);
  CHECK(fetch.mip_filter == 3);
  CHECK(fetch.aniso_filter == 7);
  CHECK(fetch.arbitrary_filter == 0);
  CHECK(fetch.vol_mag_filter == 3);
  CHECK(fetch.vol_min_filter == 3);
  CHECK(fetch.sample_location == 0);
  CHECK(fetch.use_computed_lod);
  CHECK_FALSE(fetch.use_register_lod);
  CHECK_FALSE(fetch.use_register_gradients);
  CHECK(fetch.lod_bias == -0.4375f);
  CHECK(fetch.offset_x == -1.5f);
  CHECK(fetch.offset_y == 2.5f);
  CHECK(fetch.offset_z == 0.0f);
  CHECK(fetch.instruction_words == instruction);
  CHECK(result.vertex_fetches.empty());
  CHECK(bytes == original);
  CheckNoPermission(result);
}

TEST_CASE(
    "Repeated texture slots retain both coordinate reads and one bitmap bit",
    "[graphics][native_read_inventory][native_texture]") {
  auto first = Texture(0, 0);
  auto second = Texture(0, 1);
  first[1] = (first[1] & ~4095u) | 0xFDF;
  second[1] = (second[1] & ~4095u) | 0xFFB;
  const auto result =
      Analysis::Analyze(xenos::ShaderType::kPixel,
                        ShaderBytes({first, second, Instruction{}}, 5));
  REQUIRE(result);
  REQUIRE(result.texture_fetches.size() == 2);
  CHECK(result.texture_fetch_bitmap == 1);
  CHECK(result.texture_fetches[0].instruction_dword_offset == 3);
  CHECK(result.texture_fetches[1].instruction_dword_offset == 6);
  CHECK(result.texture_fetches[0].coordinate_register == 0);
  CHECK(result.texture_fetches[1].coordinate_register == 1);
  CHECK(result.texture_fetches[0].dest_swizzle == 0xFDF);
  CHECK(result.texture_fetches[1].dest_swizzle == 0xFFB);
  CheckNoPermission(result);
}

TEST_CASE(
    "Texture filter overrides distinguish base-map mip from invalid "
    "magnification",
    "[graphics][native_read_inventory][native_texture]") {
  auto instruction = Texture(5);
  instruction[1] &= ~((3u << 12) | (3u << 14) | (3u << 16) | (7u << 18));
  instruction[1] |= 1u << 12 | 2u << 16;
  const auto result = Analysis::Analyze(xenos::ShaderType::kPixel,
                                        ShaderBytes({instruction}, 1));
  REQUIRE(result);
  REQUIRE(result.texture_fetches.size() == 1);
  CHECK(result.texture_fetch_bitmap == 1u << 5);
  CHECK(result.texture_fetches[0].mag_filter == 1);
  CHECK(result.texture_fetches[0].min_filter == 0);
  CHECK(result.texture_fetches[0].mip_filter == 2);
  CHECK(result.texture_fetches[0].aniso_filter == 0);
  CheckNoPermission(result);
}

TEST_CASE("Texture preflight refuses unsupported forms before legacy analysis",
          "[graphics][native_read_inventory][native_texture]") {
  auto instruction = Texture(0);
  auto stage = xenos::ShaderType::kPixel;
  auto expected = Analysis::Error::kUnsupportedTexture;
  SECTION("vertex texture reads need a different stage contract") {
    stage = xenos::ShaderType::kVertex;
  }
  SECTION("unknown opcode cannot reach legacy switch") {
    instruction[0] = (instruction[0] & ~31u) | 31;
  }
  SECTION("stateful LOD instruction") {
    instruction[0] =
        (instruction[0] & ~31u) | uint32_t(ucode::FetchOpcode::kSetTextureLod);
  }
  SECTION("different known texture operation") {
    instruction[0] = (instruction[0] & ~31u) |
                     uint32_t(ucode::FetchOpcode::kGetTextureWeights);
  }
  SECTION("1D requires another sampler layout") {
    instruction[2] &= ~(3u << 14);
  }
  SECTION("cube requires another sampler layout") {
    instruction[2] |= 3u << 14;
  }
  SECTION("unnormalized coordinates") { instruction[0] |= 1u << 25; }
  SECTION("relative coordinate register") { instruction[0] |= 1u << 11; }
  SECTION("relative destination register") { instruction[0] |= 1u << 18; }
  SECTION("invalid textures can be fetched") { instruction[0] &= ~(1u << 19); }
  SECTION("instruction predication") { instruction[1] |= 1u << 31; }
  SECTION("unused predicate condition is not guessed") {
    instruction[2] |= 1u << 31;
  }
  SECTION("reserved destination swizzle") {
    instruction[1] = (instruction[1] & ~7u) | 6;
    expected = Analysis::Error::kInvalidFetch;
  }
  SECTION("base-map filter is invalid for magnification") {
    instruction[1] = (instruction[1] & ~(3u << 12)) | 2u << 12;
  }
  SECTION("base-map filter is invalid for minification") {
    instruction[1] = (instruction[1] & ~(3u << 14)) | 2u << 14;
  }
  SECTION("undefined anisotropic encoding") {
    instruction[1] = (instruction[1] & ~(7u << 18)) | 6u << 18;
  }
  SECTION("anisotropic override is outside this subset") {
    instruction[1] = (instruction[1] & ~(7u << 18)) | 3u << 18;
  }
  SECTION("arbitrary filtering is not silently discarded") {
    instruction[1] |= 1u << 21;
  }
  SECTION("volume filter override is outside 2D subset") {
    instruction[1] &= ~(3u << 24);
  }
  SECTION("unknown word1 bit") { instruction[1] |= 1u << 30; }
  SECTION("reserved word2 field") { instruction[2] |= 1u << 9; }
  SECTION("center-sampling variant is outside this subset") {
    instruction[2] |= 1u << 1;
  }
  SECTION("computed LOD disabled") { instruction[1] &= ~(1u << 28); }
  SECTION("register LOD state is not tracked") { instruction[1] |= 1u << 29; }
  SECTION("register gradients are not tracked") { instruction[2] |= 1; }
  SECTION("Z offset outside 2D subset") { instruction[2] |= 1u << 26; }
  CheckFailed(Analysis::Analyze(stage, ShaderBytes({instruction}, 1)),
              expected);
}

TEST_CASE(
    "Texture limits and later invalid instructions discard all candidates",
    "[graphics][native_read_inventory][native_texture]") {
  const auto two_fetches = ShaderBytes({Texture(1), Texture(31)}, 5);
  Analysis::Limits limits;
  SECTION("texture budget cannot be zero") {
    limits.max_texture_fetches = 0;
    CheckFailed(
        Analysis::Analyze(xenos::ShaderType::kPixel, two_fetches, limits),
        Analysis::Error::kInvalidLimits);
  }
  SECTION("texture hard bound cannot be raised") {
    ++limits.max_texture_fetches;
    CheckFailed(
        Analysis::Analyze(xenos::ShaderType::kPixel, two_fetches, limits),
        Analysis::Error::kInvalidLimits);
  }
  SECTION("second valid fetch exceeds configured budget") {
    limits.max_texture_fetches = 1;
    CheckFailed(
        Analysis::Analyze(xenos::ShaderType::kPixel, two_fetches, limits),
        Analysis::Error::kLimitExceeded);
  }
  SECTION("later bad ALU cannot leave partial texture reads") {
    CheckFailed(
        Analysis::Analyze(
            xenos::ShaderType::kPixel,
            ShaderBytes({Texture(31), Instruction{0, 0, 31u << 24}}, 1)),
        Analysis::Error::kInvalidAlu);
  }
  SECTION("later memexport cannot leave partial texture reads") {
    const Instruction memexport{
        uint32_t(ucode::ExportRegister::kExportData0) | 1u << 15 | 1u << 16, 0,
        0};
    CheckFailed(Analysis::Analyze(xenos::ShaderType::kPixel,
                                  ShaderBytes({Texture(31), memexport}, 1)),
                Analysis::Error::kUnsupportedMemexport);
  }
}
}  // namespace
}  // namespace xe::gpu::test
