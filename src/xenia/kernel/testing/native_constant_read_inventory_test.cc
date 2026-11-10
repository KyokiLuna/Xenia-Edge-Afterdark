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
#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/ucode.h"

namespace xe::gpu::test {
namespace {
using Analysis = NativeShaderReadAnalysis;
using Inventory = NativeDrawReadInventory;
using Instruction = std::array<uint32_t, 3>;

// Public ALU fields only. max r0.x,c#,r0 with retain_prev (no scalar source).
Instruction Constant(uint32_t index, bool dynamic = false) {
  return {1u << 16 | uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26,
          uint32_t(dynamic) << 31,
          index << 16 | uint32_t(ucode::AluVectorOpcode::kMax) << 24 |
              1u << 30 | 1u << 29};
}

Instruction NoConstant() {
  auto op = Constant(0);
  op[2] |= 1u << 31;  // First vector source is also r0.
  return op;
}

std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> bytes;
  for (const uint32_t word : words) {
    bytes.push_back(uint8_t(word >> 24));
    bytes.push_back(uint8_t(word >> 16));
    bytes.push_back(uint8_t(word >> 8));
    bytes.push_back(uint8_t(word));
  }
  return bytes;
}

std::vector<uint32_t> Shader(std::initializer_list<Instruction> instructions) {
  REQUIRE(instructions.size() <= 6);
  std::vector<uint32_t> words{
      1u | uint32_t(instructions.size()) << 12,
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12 |
          uint32_t(ucode::AddressingMode::kAbsolute) << 11,
      0};
  for (const auto& op : instructions) {
    words.insert(words.end(), op.begin(), op.end());
  }
  return words;
}

Analysis::Result Metadata(xenos::ShaderType stage = xenos::ShaderType::kVertex,
                          bool use_constant = true) {
  const auto result = Analysis::Analyze(
      stage, Bytes(Shader({use_constant ? Constant(0) : NoConstant()})));
  REQUIRE(result);
  return result;
}

struct Primary {
  std::vector<uint32_t> words;
  void Register(uint32_t index, std::initializer_list<uint32_t> values,
                bool one_register = false) {
    REQUIRE(values.size() != 0);
    words.push_back(uint32_t(values.size() - 1) << 16 | index |
                    (uint32_t(one_register) << 15));
    words.insert(words.end(), values);
  }
  void Packet(uint32_t opcode, std::initializer_list<uint32_t> payload) {
    REQUIRE_FALSE(payload.size() == 0);
    words.push_back(0xC0000000 | uint32_t(payload.size() - 1) << 16 |
                    opcode << 8);
    words.insert(words.end(), payload);
  }
  void Loads(bool pixel = true, uint32_t vertex_constant = 0,
             uint32_t pixel_constant = UINT32_MAX) {
    for (uint32_t stage = 0; stage < (pixel ? 2u : 1u); ++stage) {
      const uint32_t index = stage == 0 ? vertex_constant : pixel_constant;
      const auto shader =
          Shader({index == UINT32_MAX ? NoConstant() : Constant(index)});
      words.push_back(0xC0000000 | uint32_t(shader.size() + 1) << 16 |
                      uint32_t(xenos::PM4_IM_LOAD_IMMEDIATE) << 8);
      words.push_back(stage);
      words.push_back(uint32_t(shader.size()));
      words.insert(words.end(), shader.begin(), shader.end());
    }
  }
  void Selectors(bool pixel = true) {
    Register(XE_GPU_REG_SQ_VS_CONST, {0x000FF000});
    if (pixel) {
      Register(XE_GPU_REG_SQ_PS_CONST, {0x000FF100});
    }
  }
  void Draw(xenos::EdramMode mode = xenos::EdramMode::kColorDepth) {
    Register(XE_GPU_REG_RB_MODECONTROL, {uint32_t(mode)});
    Register(XE_GPU_REG_PA_SC_VIZ_QUERY, {0});
    Register(XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, {0});
    const auto primitive = mode == xenos::EdramMode::kColorDepth
                               ? xenos::PrimitiveType::kTriangleList
                               : xenos::PrimitiveType::kRectangleList;
    Packet(xenos::PM4_DRAW_INDX_2,
           {3u << 16 | uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
            uint32_t(primitive)});
  }
  Inventory::ScanResult Scan() const { return Inventory::Scan(Bytes(words)); }
};

Inventory::Draw State(bool use_constant = true) {
  Primary primary;
  primary.Loads(true, use_constant ? 0 : UINT32_MAX);
  primary.Selectors();
  primary.Draw();
  auto scan = primary.Scan();
  REQUIRE(scan.complete_primary_scan);
  REQUIRE(scan.draws.size() == 1);
  return scan.draws.front();
}

TEST_CASE("Preflight exposes the legacy float constant map without new CF",
          "[graphics][native_constant_inventory]") {
  const auto result = Analysis::Analyze(
      xenos::ShaderType::kVertex, Bytes(Shader({Constant(0), Constant(255)})));
  REQUIRE(result);
  CHECK(result.constant_use.float_count == 2);
  CHECK((result.constant_use.float_bitmap ==
         std::array<uint64_t, 4>{1, 0, 0, UINT64_C(1) << 63}));
  CHECK_FALSE(result.constant_use.float_dynamic_addressing);
  CHECK((result.constant_use.bool_bitmap == std::array<uint32_t, 8>{}));
  CHECK(result.constant_use.loop_bitmap == 0);
  auto words = Shader({Constant(0)});
  words[1] = uint32_t(ucode::ControlFlowOpcode::kCondExecEnd) << 12;
  const auto rejected =
      Analysis::Analyze(xenos::ShaderType::kVertex, Bytes(words));
  CHECK(rejected.error == Analysis::Error::kUnsupportedControlFlow);
  CHECK(rejected.constant_use.float_count == 0);
  CHECK((rejected.constant_use.float_bitmap == std::array<uint64_t, 4>{}));
  CHECK((rejected.constant_use.bool_bitmap == std::array<uint32_t, 8>{}));
  CHECK(rejected.constant_use.loop_bitmap == 0);
  CHECK_FALSE(rejected.constant_use.float_dynamic_addressing);
}

TEST_CASE("Explicit stage banks preserve raw float words and known masks",
          "[graphics][native_constant_inventory]") {
  const auto vertex = Analysis::Analyze(xenos::ShaderType::kVertex,
                                        Bytes(Shader({Constant(255)})));
  const auto pixel = Analysis::Analyze(xenos::ShaderType::kPixel,
                                       Bytes(Shader({Constant(7)})));
  REQUIRE(vertex);
  REQUIRE(pixel);
  Primary primary;
  primary.Loads(true, 255, 7);
  primary.Selectors();
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_000_X + 255 * 4,
                   {0xBF000000, 0x3F800000, 0x40000000, 0x40400000});
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_256_X + 7 * 4, {0, 1, 2, 3});
  primary.Draw();
  const auto scan = primary.Scan();
  REQUIRE(scan.draws.size() == 1);
  const auto result = Inventory::ConstantReads(scan.draws[0], &vertex, &pixel);
  REQUIRE(result.used_constant_values_known);
  REQUIRE(result.floats.size() == 2);
  CHECK(result.floats[0].stage == xenos::ShaderType::kVertex);
  CHECK(result.floats[0].index == 255);
  CHECK(result.floats[0].words[0] == 0xBF000000);
  CHECK(result.floats[0].known_mask == 15);
  CHECK(result.floats[1].stage == xenos::ShaderType::kPixel);
  CHECK(result.floats[1].index == 7);
  CHECK((result.floats[1].words == std::array<uint32_t, 4>{0, 1, 2, 3}));
  CHECK(result.floats[1].known_mask == 15);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.shader_semantics_verified);
}

TEST_CASE("One missing vector component remains unknown with exact register",
          "[graphics][native_constant_inventory]") {
  auto draw = State();
  const auto vertex = Metadata();
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  draw.constants.float_words[3] = 0xDEADBEEF;  // Stale storage is not a value.
  draw.constants.float_known.set(0);
  draw.constants.float_known.set(1);
  draw.constants.float_known.set(2);
  const auto result = Inventory::ConstantReads(draw, &vertex, &pixel);
  CHECK(result.error == Inventory::Error::kMissingState);
  CHECK(result.error_register == XE_GPU_REG_SHADER_CONSTANT_000_X + 3);
  CHECK(result.error_stage == xenos::ShaderType::kVertex);
  CHECK(result.error_constant_index == 0);
  REQUIRE(result.floats.size() == 1);
  CHECK(result.floats[0].known_mask == 7);
  CHECK(result.floats[0].words[3] == 0);
  CHECK_FALSE(result.used_constant_values_known);
}

TEST_CASE("Unused constant words are not demanded or emitted",
          "[graphics][native_constant_inventory]") {
  const auto vertex = Metadata(xenos::ShaderType::kVertex, false);
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  const auto result = Inventory::ConstantReads(State(false), &vertex, &pixel);
  CHECK(result.error == Inventory::Error::kNone);
  CHECK(result.used_constant_values_known);
  CHECK(result.floats.empty());
  CHECK(result.bools.empty());
  CHECK(result.loops.empty());
}

TEST_CASE(
    "Constant selectors must be explicit and in the supported fixed banks",
    "[graphics][native_constant_inventory]") {
  auto draw = State();
  const auto vertex = Metadata();
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  auto expected = Inventory::Error::kUnsupportedConstantSelector;
  SECTION("selector zero is not promoted from legacy title defaults") {
    draw.constants.selectors[0] = 0;
  }
  SECTION("different pixel bank is unsupported") {
    draw.constants.selectors[1] = 0x000FF000;
  }
  SECTION("missing selector is not initialized by storage") {
    draw.constants.selectors_known.reset(0);
    expected = Inventory::Error::kMissingState;
  }
  const auto result = Inventory::ConstantReads(draw, &vertex, &pixel);
  CHECK(result.error == expected);
  CHECK_FALSE(result.used_constant_values_known);
  CHECK(result.floats.empty());
}

TEST_CASE("Dynamic float addressing is reported and never truncated",
          "[graphics][native_constant_inventory]") {
  const auto dynamic = Analysis::Analyze(xenos::ShaderType::kVertex,
                                         Bytes(Shader({Constant(3, true)})));
  REQUIRE(dynamic);
  CHECK(dynamic.constant_use.float_dynamic_addressing);
  CHECK(dynamic.constant_use.float_count == 256);
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  const auto result = Inventory::ConstantReads(State(), &dynamic, &pixel);
  CHECK(result.error == Inventory::Error::kUnsupportedConstantAddressing);
  CHECK_FALSE(result.used_constant_values_known);
  CHECK(result.floats.empty());
}

TEST_CASE("Packed bool values preserve the used bit and known source word",
          "[graphics][native_constant_inventory]") {
  auto vertex = Metadata(xenos::ShaderType::kVertex, false);
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  // Consumer-only synthetic maps: this does not widen the shader preflight
  // to allow conditional CF or claim a console shader emitted these maps.
  vertex.constant_use.bool_bitmap[7] = 0x80000000;
  auto draw = State(false);
  SECTION("explicit word includes both used and unused bits") {
    draw.constants.bool_words[7] = 0x80000001;
    draw.constants.bool_known.set(7);
    const auto result = Inventory::ConstantReads(draw, &vertex, &pixel);
    REQUIRE(result.used_constant_values_known);
    REQUIRE(result.bools.size() == 1);
    CHECK(result.bools[0].word_index == 7);
    CHECK(result.bools[0].used_mask == 0x80000000);
    CHECK(result.bools[0].value == 0x80000001);
    CHECK(result.bools[0].known);
  }
  SECTION("missing word reports bool255 rather than treating it false") {
    draw.constants.bool_words[7] = 0xFFFFFFFF;
    const auto result = Inventory::ConstantReads(draw, &vertex, &pixel);
    CHECK(result.error_register == XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 7);
    CHECK(result.error_constant_index == 255);
    REQUIRE(result.bools.size() == 1);
    CHECK_FALSE(result.bools[0].known);
    CHECK(result.bools[0].value == 0);
    CHECK_FALSE(result.used_constant_values_known);
  }
}

TEST_CASE("Loop values distinguish an explicit zero from missing state",
          "[graphics][native_constant_inventory]") {
  auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  const auto vertex = Metadata(xenos::ShaderType::kVertex, false);
  pixel.constant_use.loop_bitmap = 0x80000000;
  auto draw = State(false);
  bool known = false;
  SECTION("explicit zero") {
    draw.constants.loop_known.set(31);
    known = true;
  }
  SECTION("missing loop31") {}
  const auto result = Inventory::ConstantReads(draw, &vertex, &pixel);
  REQUIRE(result.loops.size() == 1);
  CHECK(result.loops[0].known == known);
  CHECK(result.loops[0].value == 0);
  CHECK(result.used_constant_values_known == known);
  if (!known) {
    CHECK(result.error_register == XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + 31);
    CHECK(result.error_stage == xenos::ShaderType::kPixel);
    CHECK(result.error_constant_index == 31);
  }
}

TEST_CASE(
    "Unknown packets invalidate all constant provenance without erasing "
    "evidence",
    "[graphics][native_constant_inventory]") {
  Primary primary;
  primary.Loads();
  primary.Selectors();
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_000_X, {1, 2, 3, 4});
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031, {0xFFFFFFFF});
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_LOOP_00, {0x12345678});
  primary.Draw();
  primary.Packet(xenos::PM4_INDIRECT_BUFFER, {0x00100000, 3});
  primary.Loads();
  primary.Selectors();
  primary.Draw();
  const auto scan = primary.Scan();
  REQUIRE(scan.draws.size() == 2);
  CHECK(scan.draws[0].constants.float_known.count() == 4);
  CHECK(scan.draws[0].constants.bool_known.count() == 1);
  CHECK(scan.draws[0].constants.loop_known.count() == 1);
  CHECK(scan.draws[1].constants.float_known.none());
  CHECK(scan.draws[1].constants.bool_known.none());
  CHECK(scan.draws[1].constants.loop_known.none());
  const auto vertex = Metadata();
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  const auto result = Inventory::ConstantReads(scan.draws[1], &vertex, &pixel);
  CHECK_FALSE(result.used_constant_values_known);
  REQUIRE(result.floats.size() == 1);
  CHECK(result.floats[0].known_mask == 0);
  CHECK((result.floats[0].words == std::array<uint32_t, 4>{}));
}

TEST_CASE("Draw snapshots retain their own constant values across later writes",
          "[graphics][native_constant_inventory]") {
  Primary primary;
  primary.Loads();
  primary.Selectors();
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_000_X, {1, 2, 3, 4});
  primary.Draw();
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_000_X, {5, 6, 7, 8});
  primary.Draw();
  const auto scan = primary.Scan();
  REQUIRE(scan.draws.size() == 2);
  const auto vertex = Metadata();
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  const auto first = Inventory::ConstantReads(scan.draws[0], &vertex, &pixel);
  const auto second = Inventory::ConstantReads(scan.draws[1], &vertex, &pixel);
  REQUIRE(first.used_constant_values_known);
  REQUIRE(second.used_constant_values_known);
  CHECK(first.floats[0].words[0] == 1);
  CHECK(second.floats[0].words[0] == 5);
}

TEST_CASE("One-register packets do not invent adjacent known constant words",
          "[graphics][native_constant_inventory]") {
  Primary primary;
  primary.Loads();
  primary.Selectors();
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_000_X, {1, 2, 3, 4}, true);
  primary.Draw();
  const auto scan = primary.Scan();
  REQUIRE(scan.draws.size() == 1);
  const auto vertex = Metadata();
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  const auto result = Inventory::ConstantReads(scan.draws[0], &vertex, &pixel);
  REQUIRE(result.floats.size() == 1);
  CHECK(result.floats[0].known_mask == 1);
  CHECK(result.floats[0].words[0] == 4);
  CHECK(result.error_register == XE_GPU_REG_SHADER_CONSTANT_000_X + 1);
}

TEST_CASE("Type1 payload values cannot manufacture nested constant packets",
          "[graphics][native_constant_inventory]") {
  Primary primary;
  primary.Loads();
  primary.Selectors();
  // Two low-register writes. Their values resemble a constant packet but are
  // not headers; Type1 cannot address the high shader-constant registers.
  primary.words.insert(primary.words.end(),
                       {0x40000000, 0x00034000, 0x12345678});
  primary.Draw();
  const auto scan = primary.Scan();
  REQUIRE(scan.draws.size() == 1);
  CHECK(scan.draws[0].constants.float_known.none());
  const auto vertex = Metadata();
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  const auto result = Inventory::ConstantReads(scan.draws[0], &vertex, &pixel);
  CHECK(result.error_register == XE_GPU_REG_SHADER_CONSTANT_000_X);
  CHECK_FALSE(result.used_constant_values_known);
}

TEST_CASE("Constant snapshots retain the existing whole-primary draw limit",
          "[graphics][native_constant_inventory]") {
  Primary primary;
  primary.Loads();
  primary.Selectors();
  for (uint32_t i = 0; i < 257; ++i) {
    primary.Draw();
  }
  const auto scan = primary.Scan();
  CHECK(scan.error == Inventory::Error::kLimitExceeded);
  CHECK_FALSE(scan.complete_primary_scan);
  CHECK(scan.draws.empty());
  CHECK(scan.shaders.empty());
}

TEST_CASE("Constant inventory follows depth and copy mode stage boundaries",
          "[graphics][native_constant_inventory]") {
  Primary primary;
  const auto vertex = Metadata(xenos::ShaderType::kVertex, false);
  SECTION("depth-only ignores absent pixel selector and metadata") {
    primary.Loads(false, UINT32_MAX);
    primary.Selectors(false);
    primary.Draw(xenos::EdramMode::kDepthOnly);
    const auto scan = primary.Scan();
    REQUIRE(scan.draws.size() == 1);
    const auto result =
        Inventory::ConstantReads(scan.draws[0], &vertex, nullptr);
    CHECK(result.used_constant_values_known);
  }
  SECTION("copy ignores shaders and all constant state") {
    primary.Draw(xenos::EdramMode::kCopy);
    const auto scan = primary.Scan();
    REQUIRE(scan.draws.size() == 1);
    const auto result =
        Inventory::ConstantReads(scan.draws[0], nullptr, nullptr);
    CHECK(result.used_constant_values_known);
    CHECK(result.floats.empty());
    CHECK_FALSE(result.execution_ready);
  }
  SECTION("color mode requires pixel metadata") {
    primary.Loads(false, UINT32_MAX);
    primary.Selectors(false);
    primary.Draw();
    const auto scan = primary.Scan();
    REQUIRE(scan.draws.size() == 1);
    CHECK(Inventory::ConstantReads(scan.draws[0], &vertex, nullptr).error ==
          Inventory::Error::kUnsupportedShader);
  }
}

TEST_CASE("Mismatched metadata and later framing errors cannot pass closure",
          "[graphics][native_constant_inventory]") {
  auto vertex = Metadata();
  const auto pixel = Metadata(xenos::ShaderType::kPixel, false);
  auto expected = Inventory::Error::kInvalidConstantMetadata;
  SECTION("bitmap count mismatch") { ++vertex.constant_use.float_count; }
  SECTION("wrong source stage") {
    vertex.stage = xenos::ShaderType::kPixel;
    expected = Inventory::Error::kUnsupportedShader;
  }
  const auto result = Inventory::ConstantReads(State(), &vertex, &pixel);
  CHECK(result.error == expected);
  CHECK(result.floats.empty());
  CHECK_FALSE(result.used_constant_values_known);
  Primary primary;
  primary.Draw();
  primary.words.push_back(0xC0012700);
  const auto scan = primary.Scan();
  CHECK_FALSE(scan.complete_primary_scan);
  CHECK(scan.draws.empty());
}

}  // namespace
}  // namespace xe::gpu::test
