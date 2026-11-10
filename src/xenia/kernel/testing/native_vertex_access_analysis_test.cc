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
#include "xenia/gpu/native_vertex_access_analysis.h"
#include "xenia/gpu/ucode.h"

namespace xe::gpu::test {
namespace {
using Access = NativeVertexAccessAnalysis;
using Inventory = NativeDrawReadInventory;
using Instruction = std::array<uint32_t, 3>;

// Generic public ISA fields only, no captured/supplied shader bytes.
Instruction Fetch(uint32_t slot = 95, uint32_t stride = 4, int32_t offset = 0,
                  bool mini = false) {
  return {1u << 12 | 1u << 19 | (slot / 3) << 20 | (slot % 3) << 25,
          0x688u | uint32_t(xenos::VertexFormat::k_32_32_FLOAT) << 16 |
              uint32_t(mini) << 30,
          stride | (uint32_t(offset) & 0x7FFFFF) << 8};
}

std::vector<uint8_t> Shader(std::initializer_list<Instruction> instructions,
                            uint32_t sequence) {
  REQUIRE(instructions.size() <= 6);
  std::vector<uint32_t> words{
      1u | uint32_t(instructions.size()) << 12 | sequence << 16,
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12 |
          uint32_t(ucode::AddressingMode::kAbsolute) << 11,
      0};
  for (const auto& instruction : instructions) {
    words.insert(words.end(), instruction.begin(), instruction.end());
  }
  std::vector<uint8_t> bytes;
  for (uint32_t word : words) {
    bytes.push_back(uint8_t(word >> 24));
    bytes.push_back(uint8_t(word >> 16));
    bytes.push_back(uint8_t(word >> 8));
    bytes.push_back(uint8_t(word));
  }
  return bytes;
}

Inventory::Draw Draw(uint32_t count = 4, uint32_t declared_words = 16,
                     uint32_t slot = 95) {
  Inventory::Draw draw{};
  draw.packet_dword_offset = 17;
  draw.opcode = xenos::PM4_DRAW_INDX_2;
  draw.initiator = count << 16 |
                   uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                   uint32_t(xenos::PrimitiveType::kQuadList);
  draw.vertex_shader = 0;
  draw.state_known.set();
  draw.state_words = {
      uint32_t(xenos::EdramMode::kColorDepth), 0, 0, 0xFFFFFF, 0, 0, 0, 0};
  draw.fetch_known.set(slot * 2);
  draw.fetch_known.set(slot * 2 + 1);
  draw.fetch_words[slot * 2] = 0x00100003;
  draw.fetch_words[slot * 2 + 1] = declared_words << 2 | 2;
  return draw;
}

TEST_CASE("Vertex access proof checks full and cached mini ranges",
          "[graphics][native_vertex_access]") {
  const auto bytes = Shader({Fetch(), Fetch(0, 4, 2, true)}, 5);
  const auto result = Access::Analyze(Draw(), bytes);
  REQUIRE(result.error == Access::Error::kNone);
  REQUIRE(result.vertex_input_bounds_verified);
  REQUIRE(result.accesses.size() == 2);
  CHECK(result.vertex_count == 4);
  CHECK(result.uses_auto_index_backend_contract);
  CHECK_FALSE(result.copy_rectangle);
  CHECK(result.accesses[0].slot == 95);
  CHECK(result.accesses[0].first_word == 0);
  CHECK(result.accesses[0].exclusive_end_word == 14);
  CHECK(result.accesses[1].slot == 95);
  CHECK(result.accesses[1].mini);
  CHECK(result.accesses[1].first_word == 2);
  CHECK(result.accesses[1].exclusive_end_word == 16);
  CHECK(result.accesses[1].declared_words == 16);
  CHECK(result.accesses[1].physical_address == 0x00100000);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Two color rectangles bound all six guest vertex fetches",
          "[graphics][native_vertex_access]") {
  auto draw = Draw(6, 42);
  draw.initiator = 6u << 16 | uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                   uint32_t(xenos::PrimitiveType::kRectangleList);
  draw.state_words[3] = 5;
  auto mini = Fetch(0, 7, 6, true);
  mini[1] = (mini[1] & ~(63u << 16)) |
            (uint32_t(xenos::VertexFormat::k_8_8_8_8) << 16);
  const auto bytes = Shader({Fetch(95, 7), mini}, 5);
  const auto result = Access::Analyze(draw, bytes);
  REQUIRE(result.error == Access::Error::kNone);
  REQUIRE(result.vertex_input_bounds_verified);
  REQUIRE(result.accesses.size() == 2);
  CHECK(result.vertex_count == 6);
  CHECK(result.uses_auto_index_backend_contract);
  CHECK_FALSE(result.copy_rectangle);
  CHECK(result.accesses[0].exclusive_end_word == 37);
  CHECK(result.accesses[1].first_word == 6);
  CHECK(result.accesses[1].exclusive_end_word == 42);
  CHECK(result.accesses[1].declared_words == 42);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Color rectangle expansion cannot relax guest binding or VGT bounds",
          "[graphics][native_vertex_access]") {
  auto draw = Draw(6, 42);
  draw.initiator = 6u << 16 | uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                   uint32_t(xenos::PrimitiveType::kRectangleList);
  draw.state_words[3] = 5;
  auto mini = Fetch(0, 7, 6, true);
  mini[1] = (mini[1] & ~(63u << 16)) |
            (uint32_t(xenos::VertexFormat::k_8_8_8_8) << 16);
  auto expected = Access::Error::kOutOfBounds;
  SECTION("the last packed component is outside a 41-word declaration") {
    draw.fetch_words[191] = 41u << 2 | 2;
  }
  SECTION("MAX4 does not admit guest vertex5") {
    draw.state_words[3] = 4;
    expected = Access::Error::kUnsupportedVgtState;
  }
  SECTION("a third rectangle needs more guest input") {
    draw.initiator += 3u << 16;
    draw.state_words[3] = 8;
  }
  const auto result = Access::Analyze(draw, Shader({Fetch(95, 7), mini}, 5));
  CHECK(result.error == expected);
  CHECK_FALSE(result.vertex_input_bounds_verified);
  CHECK(result.accesses.empty());
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Cached mini address survives changes to the original index",
          "[graphics][native_vertex_access]") {
  auto full = Fetch();
  full[0] &= ~(63u << 12);  // The full fetch overwrites r0, after reading r0.x.
  auto mini = Fetch(0, 4, 2, true);
  mini[0] |= 63u << 5;  // Ignored mini source is not a new index provenance.
  SECTION("a mini reuses its full fetch address") {
    const auto result = Access::Analyze(Draw(), Shader({full, mini}, 5));
    REQUIRE(result.vertex_input_bounds_verified);
    REQUIRE(result.accesses.size() == 2);
    CHECK(result.accesses[1].slot == 95);
  }
  SECTION("intervening ALU does not create a new full address") {
    const auto result =
        Access::Analyze(Draw(), Shader({full, Instruction{}, mini}, 17));
    CHECK(result.error == Access::Error::kNone);
    CHECK(result.vertex_input_bounds_verified);
  }
}

TEST_CASE("A later full fetch cannot reuse a clobbered initial index",
          "[graphics][native_vertex_access]") {
  auto first = Fetch();
  Instruction middle = Fetch();
  uint32_t sequence = 5;
  SECTION("fetch writes r0.x") { first[0] &= ~(63u << 12); }
  SECTION("relative fetch destination may alias r0") { first[0] |= 1u << 18; }
  SECTION("even an apparently harmless ALU needs unsupported dataflow") {
    first = Instruction{};
    sequence = 4;
  }
  const auto result =
      Access::Analyze(Draw(), Shader({first, middle}, sequence));
  CHECK(result.error == Access::Error::kUnsupportedIndexProvenance);
  CHECK_FALSE(result.vertex_input_bounds_verified);
  CHECK(result.accesses.empty());
}

TEST_CASE("Fetches that preserve r0.x can share the initial index proof",
          "[graphics][native_vertex_access]") {
  auto first = Fetch();
  first[0] &= ~(63u << 12);
  first[1] = (first[1] & ~7u) | 7u;  // x destination remains unchanged.
  const auto result = Access::Analyze(Draw(), Shader({first, Fetch()}, 5));
  CHECK(result.error == Access::Error::kNone);
  CHECK(result.vertex_input_bounds_verified);
}

TEST_CASE("Vertex access ranges fail atomically on undersized binding",
          "[graphics][native_vertex_access]") {
  const auto result =
      Access::Analyze(Draw(4, 15), Shader({Fetch(), Fetch(0, 4, 2, true)}, 5));
  CHECK(result.error == Access::Error::kOutOfBounds);
  CHECK(result.error_slot == 95);
  CHECK(result.accesses.empty());
  CHECK_FALSE(result.vertex_input_bounds_verified);
}

TEST_CASE("Index and signed offset assumptions are explicit",
          "[graphics][native_vertex_access]") {
  auto fetch = Fetch();
  auto expected = Access::Error::kUnsupportedIndexProvenance;
  SECTION("different source register") { fetch[0] |= 1u << 5; }
  SECTION("different source component") { fetch[0] |= 1u << 30; }
  SECTION("relative source") { fetch[0] |= 1u << 11; }
  SECTION("rounded index") { fetch[1] |= 1u << 15; }
  SECTION("nonzero full offset is outside the first proof subset") {
    fetch[2] |= 1u << 8;
    expected = Access::Error::kUnsupportedFetch;
  }
  const auto result = Access::Analyze(Draw(), Shader({fetch}, 1));
  CHECK(result.error == expected);
  CHECK_FALSE(result.vertex_input_bounds_verified);
  CHECK(result.accesses.empty());
}

TEST_CASE("Mini offsets are bounded with signed arithmetic",
          "[graphics][native_vertex_access]") {
  auto mini = Fetch(0, 4, -1, true);
  SECTION("negative start never wraps into the binding") {}
  SECTION("positive end exceeds the binding") { mini = Fetch(0, 4, 3, true); }
  SECTION("largest signed mini offset stays bounded") {
    mini = Fetch(0, 4, 0x3FFFFF, true);
  }
  const auto result = Access::Analyze(Draw(), Shader({Fetch(), mini}, 5));
  CHECK(result.error == Access::Error::kOutOfBounds);
  CHECK(result.accesses.empty());
}

TEST_CASE("VGT state must be locally known and compatible with zero-based IDs",
          "[graphics][native_vertex_access]") {
  auto draw = Draw();
  auto expected = Access::Error::kUnsupportedVgtState;
  SECTION("missing max") {
    draw.state_known.reset(3);
    expected = Access::Error::kMissingVgtState;
  }
  SECTION("missing min") {
    draw.state_known.reset(4);
    expected = Access::Error::kMissingVgtState;
  }
  SECTION("missing index offset") {
    draw.state_known.reset(5);
    expected = Access::Error::kMissingVgtState;
  }
  SECTION("nonzero min") { draw.state_words[4] = 1; }
  SECTION("nonzero offset") { draw.state_words[5] = 1; }
  SECTION("maximum smaller than last vertex") { draw.state_words[3] = 2; }
  SECTION("reserved high bits are not a maximum") {
    draw.state_words[3] = 0xFF000000;
  }
  const auto result = Access::Analyze(draw, Shader({Fetch()}, 1));
  CHECK(result.error == expected);
  CHECK_FALSE(result.vertex_input_bounds_verified);
}

TEST_CASE("Vertex proof preserves draw and binding rejection rules",
          "[graphics][native_vertex_access]") {
  auto draw = Draw();
  SECTION("zero vertex count") { draw.initiator &= 0xFFFF; }
  SECTION("incomplete quad") {
    draw.initiator = draw.initiator & 0xFFFF | 5u << 16;
  }
  SECTION("DMA indices are outside the implicit contract") {
    draw.initiator &= ~(3u << 6);
  }
  SECTION("nonzero VIZ") { draw.state_words[1] = 1; }
  SECTION("different output path") { draw.state_words[2] = 1; }
  SECTION("missing binding half") { draw.fetch_known.reset(191); }
  SECTION("physical range overflow") { draw.fetch_words[190] = 0xFFFFFFF3; }
  SECTION("missing selected vertex load") { draw.vertex_shader.reset(); }
  const auto result = Access::Analyze(draw, Shader({Fetch()}, 1));
  CHECK(result.error == Access::Error::kInvalidDraw);
  CHECK(result.read_error != Inventory::Error::kNone);
  CHECK_FALSE(result.vertex_input_bounds_verified);
  CHECK(result.accesses.empty());
}

TEST_CASE("Untrusted shader bytes are reparsed instead of accepting metadata",
          "[graphics][native_vertex_access]") {
  auto bytes = Shader({Fetch()}, 1);
  auto expected = Access::Error::kInvalidShader;
  SECTION("truncated code") { bytes.pop_back(); }
  SECTION("pixel texture instruction cannot be analyzed as a vertex load") {
    bytes[15] |= 1;  // Opcode bit in first fetch's BE low byte.
  }
  SECTION("mini without cached full address") {
    bytes = Shader({Fetch(0, 4, 2, true)}, 1);
  }
  SECTION("ALU-only shader is outside this input proof subset") {
    bytes = Shader({Instruction{}}, 0);
    expected = Access::Error::kMissingVertexFetch;
  }
  const auto result = Access::Analyze(Draw(), bytes);
  CHECK(result.error == expected);
  CHECK_FALSE(result.vertex_input_bounds_verified);
  CHECK(result.accesses.empty());
}

TEST_CASE("Copy range proof uses fetch0 rectangle without shader or VGT",
          "[graphics][native_vertex_access]") {
  auto draw = Draw(3, 6, 0);
  draw.state_known.reset(3);
  draw.state_known.reset(4);
  draw.state_known.reset(5);
  draw.state_words[0] = uint32_t(xenos::EdramMode::kCopy);
  draw.initiator = 3u << 16 | uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                   uint32_t(xenos::PrimitiveType::kRectangleList);
  draw.vertex_shader.reset();
  SECTION("exact backend rectangle") {
    const auto result = Access::Analyze(draw, {});
    REQUIRE(result.vertex_input_bounds_verified);
    REQUIRE(result.accesses.size() == 1);
    CHECK(result.copy_rectangle);
    CHECK_FALSE(result.uses_auto_index_backend_contract);
    CHECK(result.accesses[0].slot == 0);
    CHECK(result.accesses[0].exclusive_end_word == 6);
    CHECK_FALSE(result.execution_ready);
    CHECK_FALSE(result.ownership_verified);
  }
  SECTION("larger declaration is not silently accepted") {
    draw.fetch_words[1] = 7u << 2;
    const auto result = Access::Analyze(draw, {});
    CHECK(result.error == Access::Error::kInvalidDraw);
    CHECK_FALSE(result.vertex_input_bounds_verified);
  }
}

}  // namespace
}  // namespace xe::gpu::test
