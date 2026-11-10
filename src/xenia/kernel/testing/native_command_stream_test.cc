/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/native_command_stream.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/xenos.h"

namespace xe::gpu::test {
namespace {

using Stream = NativeCommandStream;
using Error = Stream::Error;

// Synthetic words only. These do not contain a guest program or prove that
// either placeholder shader is valid. Shader instruction analysis is a later
// mandatory gate, even when this PM4/dependency inventory succeeds.
struct Fixture {
  std::vector<uint32_t> words;
  uint32_t wait_offset;
  uint32_t pixel_offset;
  uint32_t vertex_offset;
  uint32_t fetch_offset;
  uint32_t draw_offset;

  void Registers(uint32_t first, std::initializer_list<uint32_t> values) {
    REQUIRE(values.size());
    words.push_back(uint32_t(values.size() - 1) << 16 | first);
    words.insert(words.end(), values);
  }
  uint32_t Packet(uint32_t opcode, std::initializer_list<uint32_t> operands) {
    REQUIRE(operands.size());
    const uint32_t offset = uint32_t(words.size());
    words.push_back(0xC0000000 | uint32_t(operands.size() - 1) << 16 |
                    opcode << 8);
    words.insert(words.end(), operands);
    return offset;
  }
  Fixture() {
    Registers(XE_GPU_REG_COHER_SIZE_HOST, {0x20000000, 0, 0x03000000});
    wait_offset = Packet(xenos::PM4_WAIT_REG_MEM,
                         {3, XE_GPU_REG_COHER_STATUS_HOST, 0, 0x80000000, 8});
    pixel_offset = Packet(xenos::PM4_IM_LOAD, {0x00100001, 9});
    Packet(xenos::PM4_INVALIDATE_STATE,
           {xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER});
    vertex_offset = Packet(xenos::PM4_IM_LOAD_IMMEDIATE, {0, 3, 0, 0, 0});
    Registers(XE_GPU_REG_RB_MODECONTROL,
              {uint32_t(xenos::EdramMode::kColorDepth)});
    Registers(XE_GPU_REG_PA_SC_VIZ_QUERY, {0});
    fetch_offset = uint32_t(words.size());
    words.push_back(191u << 16 | XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    words.resize(words.size() + 192, 0);
    // One empty declared vertex slot and one nonempty final slot.
    words[fetch_offset + 1 + 4] = 3;
    words[fetch_offset + 1 + 190] = 0xC0200003;
    words[fetch_offset + 1 + 191] = 12u << 2 | 2;
    words.push_back(0x80000000);
    draw_offset = Packet(xenos::PM4_DRAW_INDX, {0, 6u << 16 | 0x84});
  }
  std::vector<uint8_t> Bytes() const {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (const auto word : words) {
      bytes.push_back(uint8_t(word >> 24));
      bytes.push_back(uint8_t(word >> 16));
      bytes.push_back(uint8_t(word >> 8));
      bytes.push_back(uint8_t(word));
    }
    return bytes;
  }
  Stream::Result Analyze() const { return Stream::Analyze(Bytes()); }
};

TEST_CASE("Native PM4 inventory owns unaligned input and typed read extents",
          "[graphics][native_command]") {
  const Fixture fixture;
  auto bytes = fixture.Bytes();
  bytes.insert(bytes.begin(), 0xAB);
  const auto result =
      Stream::Analyze(std::span<const uint8_t>(bytes).subspan(1));
  REQUIRE(result);
  REQUIRE(result.stream);
  const auto& stream = *result.stream;
  CHECK(stream.words() == fixture.words);
  std::fill(bytes.begin(), bytes.end(), 0);
  CHECK(stream.words() == fixture.words);
  REQUIRE(stream.dependencies().size() == 2);
  const auto& pixel = stream.dependencies()[0];
  CHECK(pixel.kind == Stream::MemoryDependency::Kind::kShaderCode);
  CHECK(pixel.raw_address == 0x00100000);
  CHECK(pixel.physical_address == 0x00100000);
  CHECK(pixel.byte_count == 36);
  CHECK(pixel.packet_dword_offset == fixture.pixel_offset);
  const auto& vertices = stream.dependencies()[1];
  CHECK(vertices.kind == Stream::MemoryDependency::Kind::kDeclaredVertexBuffer);
  CHECK(vertices.raw_address == 0xC0200000);
  CHECK(vertices.physical_address == 0x00200000);
  CHECK(vertices.byte_count == 48);
  CHECK(vertices.binding_index == 95);
  REQUIRE(stream.shaders().size() == 2);
  CHECK_FALSE(stream.shaders()[0].is_inline);
  CHECK(stream.shaders()[0].stage == Stream::ShaderStage::kPixel);
  CHECK(stream.shaders()[1].is_inline);
  CHECK(stream.shaders()[1].inline_dword_offset == fixture.vertex_offset + 3);
  REQUIRE(stream.draws().size() == 1);
  CHECK(stream.draws()[0].vertex_count == 6);
  CHECK(stream.draws()[0].vertex_shader == 1);
  CHECK(stream.draws()[0].pixel_shader == 0);
  CHECK(stream.draws()[0].fetch_constants[190] == 0xC0200003);
}

TEST_CASE("Native coherency waits require exact local register semantics",
          "[graphics][native_command]") {
  Fixture fixture;
  SECTION("range action is inventoried without a read of all physical RAM") {
    const auto result = fixture.Analyze();
    REQUIRE(result);
    REQUIRE(result.stream->coherency_waits().size() == 1);
    const auto& wait = result.stream->coherency_waits()[0];
    CHECK(wait.status == 0x03000000);
    CHECK(wait.base == 0);
    CHECK(wait.size == 0x20000000);
    CHECK(result.stream->dependencies().size() == 2);
  }
  SECTION("memory polling cannot enter the supported profile") {
    fixture.words[fixture.wait_offset + 1] |= 0x10;
    CHECK(fixture.Analyze().error == Error::kUnsupportedWait);
  }
  SECTION("arbitrary register polling is refused") {
    fixture.words[fixture.wait_offset + 2] = XE_GPU_REG_RB_MODECONTROL;
    CHECK(fixture.Analyze().error == Error::kUnsupportedWait);
  }
  SECTION("unknown comparison and reference are refused") {
    fixture.words[fixture.wait_offset + 1] = 7;
    CHECK(fixture.Analyze().error == Error::kUnsupportedWait);
    fixture.words[fixture.wait_offset + 1] = 3;
    fixture.words[fixture.wait_offset + 3] = 1;
    CHECK(fixture.Analyze().error == Error::kUnsupportedWait);
  }
  SECTION("coherency range cannot wrap the physical address space") {
    fixture.words[2] = 4;
    CHECK(fixture.Analyze().error == Error::kUnsupportedWait);
  }
  SECTION("inherited coherency state is insufficient") {
    fixture.words.erase(fixture.words.begin(),
                        fixture.words.begin() + fixture.wait_offset);
    CHECK(fixture.Analyze().error == Error::kUnsupportedWait);
  }
  SECTION("unsupported long wait policy is refused") {
    fixture.words[fixture.wait_offset + 5] = 0x100;
    CHECK(fixture.Analyze().error == Error::kUnsupportedWait);
  }
}

TEST_CASE("Native shader loads validate operands and exact inline extent",
          "[graphics][native_command]") {
  Fixture fixture;
  SECTION("unknown shader type") {
    fixture.words[fixture.pixel_offset + 1] |= 2;
    CHECK(fixture.Analyze().error == Error::kInvalidShader);
  }
  SECTION("nonzero instruction start") {
    fixture.words[fixture.pixel_offset + 2] |= 1u << 16;
    CHECK(fixture.Analyze().error == Error::kInvalidShader);
  }
  SECTION("empty code") {
    fixture.words[fixture.pixel_offset + 2] = 0;
    CHECK(fixture.Analyze().error == Error::kInvalidShader);
  }
  SECTION("external code extent cannot cross physical end") {
    fixture.words[fixture.pixel_offset + 1] = 0x1FFFFFFD;
    CHECK(fixture.Analyze().error == Error::kInvalidMemoryRange);
  }
  SECTION("inline extent must match the packet rather than spill into next") {
    fixture.words[fixture.vertex_offset + 2] = 4;
    CHECK(fixture.Analyze().error == Error::kInvalidShader);
  }
  SECTION("trailing inline operands must not become new packet headers") {
    fixture.words[fixture.vertex_offset + 2] = 2;
    CHECK(fixture.Analyze().error == Error::kInvalidShader);
  }
  SECTION("post-load invalidation requires another local shader load") {
    fixture.words.resize(fixture.draw_offset);
    fixture.Packet(xenos::PM4_INVALIDATE_STATE,
                   {xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER});
    fixture.Packet(xenos::PM4_DRAW_INDX, {0, 6u << 16 | 0x84});
    CHECK(fixture.Analyze().error == Error::kMissingLocalState);
  }
}

TEST_CASE("Native draws reject control and index-buffer dependencies",
          "[graphics][native_command]") {
  Fixture fixture;
  SECTION("DMA indices") {
    fixture.words[fixture.draw_offset + 2] &= ~0xC0;
    CHECK(fixture.Analyze().error == Error::kInvalidDraw);
  }
  SECTION("VIZ condition") {
    fixture.words[fixture.draw_offset + 1] = 0x100;
    CHECK(fixture.Analyze().error == Error::kInvalidDraw);
  }
  SECTION("incomplete triangle") {
    fixture.words[fixture.draw_offset + 2] = 2u << 16 | 0x84;
    CHECK(fixture.Analyze().error == Error::kInvalidDraw);
  }
  SECTION("explicit major mode") {
    fixture.words[fixture.draw_offset + 2] |= 1u << 8;
    CHECK(fixture.Analyze().error == Error::kInvalidDraw);
  }
  SECTION("resolve mode is not ordinary rendering") {
    fixture.words.resize(fixture.draw_offset);
    fixture.Registers(XE_GPU_REG_RB_MODECONTROL,
                      {uint32_t(xenos::EdramMode::kCopy)});
    fixture.Packet(xenos::PM4_DRAW_INDX, {0, 6u << 16 | 0x84});
    CHECK(fixture.Analyze().error == Error::kInvalidDraw);
  }
  SECTION("register-based VIZ cannot inherit a predicate") {
    fixture.words.resize(fixture.draw_offset);
    fixture.Registers(XE_GPU_REG_PA_SC_VIZ_QUERY, {1});
    fixture.Packet(xenos::PM4_DRAW_INDX, {0, 6u << 16 | 0x84});
    CHECK(fixture.Analyze().error == Error::kInvalidDraw);
  }
}

TEST_CASE("Native draw dependencies use each draw's complete fetch snapshot",
          "[graphics][native_command]") {
  Fixture fixture;
  SECTION("second draw sees changed declaration without altering first") {
    fixture.Registers(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190,
                      {0x00300003, 8u << 2});
    fixture.Packet(xenos::PM4_DRAW_INDX, {0, 3u << 16 | 0x84});
    const auto result = fixture.Analyze();
    REQUIRE(result);
    REQUIRE(result.stream->draws().size() == 2);
    REQUIRE(result.stream->dependencies().size() == 3);
    CHECK(result.stream->draws()[0].fetch_constants[190] == 0xC0200003);
    CHECK(result.stream->draws()[1].fetch_constants[190] == 0x00300003);
    CHECK(result.stream->dependencies()[2].physical_address == 0x00300000);
    CHECK(result.stream->dependencies()[2].byte_count == 32);
  }
  SECTION("texture descriptor needs a separately implemented layout gate") {
    fixture.words[fixture.fetch_offset + 1] = 2;
    CHECK(fixture.Analyze().error == Error::kUnsupportedTexture);
  }
  SECTION("last pair cannot come from prior title state") {
    fixture.words[fixture.fetch_offset] -= 2u << 16;
    fixture.words.erase(fixture.words.begin() + fixture.fetch_offset + 191,
                        fixture.words.begin() + fixture.fetch_offset + 193);
    CHECK(fixture.Analyze().error == Error::kMissingLocalState);
  }
  SECTION("exact physical end is permitted, crossing it is not") {
    fixture.words[fixture.fetch_offset + 191] = 0x1FFFFFD3;
    REQUIRE(fixture.Analyze());
    fixture.words[fixture.fetch_offset + 191] += 4;
    CHECK(fixture.Analyze().error == Error::kInvalidMemoryRange);
  }
}

TEST_CASE("Native PM4 refuses side effects without publishing a partial plan",
          "[graphics][native_command]") {
  for (const auto opcode : {xenos::PM4_INTERRUPT, xenos::PM4_MEM_WRITE,
                            xenos::PM4_REG_TO_MEM, xenos::PM4_INDIRECT_BUFFER,
                            xenos::PM4_XE_SWAP, xenos::PM4_EVENT_WRITE_ZPD}) {
    Fixture fixture;
    fixture.Packet(opcode, {0, 0});
    const auto result = fixture.Analyze();
    CHECK(result.error == Error::kUnsupportedPacket);
    CHECK_FALSE(result.stream);
  }
  for (const auto reg : {XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_DC_LUT_RW_INDEX}) {
    Fixture fixture;
    fixture.Registers(reg, {0});
    CHECK(fixture.Analyze().error == Error::kUnsupportedRegister);
  }
  SECTION("predicated draw") {
    Fixture fixture;
    fixture.words[fixture.draw_offset] |= 1;
    CHECK(fixture.Analyze().error == Error::kUnsupportedPacket);
  }
  SECTION("repeated register form") {
    Fixture fixture;
    fixture.words[0] |= 0x8000;
    CHECK(fixture.Analyze().error == Error::kUnsupportedPacket);
  }
}

TEST_CASE("Native PM4 limits and truncation fail before exposing owned plans",
          "[graphics][native_command]") {
  Fixture fixture;
  SECTION("unaligned length and empty input") {
    auto bytes = fixture.Bytes();
    bytes.pop_back();
    CHECK(Stream::Analyze(bytes).error == Error::kInvalidByteCount);
    CHECK(Stream::Analyze({}).error == Error::kInvalidByteCount);
  }
  SECTION("packet extends past input") {
    fixture.words.pop_back();
    CHECK(fixture.Analyze().error == Error::kTruncatedPacket);
  }
  SECTION("profile cannot be widened beyond hard limits") {
    Stream::Limits limits;
    ++limits.max_dwords;
    CHECK(Stream::Analyze(fixture.Bytes(), limits).error ==
          Error::kInvalidLimits);
  }
  SECTION("caller can tighten total size") {
    Stream::Limits limits;
    limits.max_dwords = 8;
    CHECK(Stream::Analyze(fixture.Bytes(), limits).error ==
          Error::kLimitExceeded);
  }
  SECTION("dependency cap includes external code and vertex reads") {
    Stream::Limits limits;
    limits.max_dependencies = 1;
    CHECK(Stream::Analyze(fixture.Bytes(), limits).error ==
          Error::kLimitExceeded);
  }
  SECTION("valid state without a draw is not the supported recording profile") {
    fixture.words.resize(fixture.draw_offset);
    CHECK(fixture.Analyze().error == Error::kNoDraw);
  }
}

TEST_CASE(
    "Rejected native fetch gaps retain only validated shader read candidates",
    "[graphics][native_command]") {
  Fixture fixture;
  const auto fetch = fixture.fetch_offset;
  std::vector<uint32_t> original(fixture.words.begin() + fetch + 1,
                                 fixture.words.begin() + fetch + 193);
  fixture.words[fetch] = 89u << 16 | 0x4800;
  fixture.words[fetch + 91] = 95u << 16 | 0x4860;
  std::copy(original.begin() + 96, original.end(),
            fixture.words.begin() + fetch + 92);
  std::fill(fixture.words.begin() + fetch + 188,
            fixture.words.begin() + fetch + 193, 0x80000000);
  const auto result = fixture.Analyze();
  CHECK_FALSE(result);
  CHECK_FALSE(result.stream);
  CHECK(result.error == Stream::Error::kMissingLocalState);
  CHECK(result.missing_state_register == 0x485A);
  REQUIRE(result.prefix_shader_reads.size() == 1);
  const auto& shader = result.prefix_shader_reads[0];
  CHECK(shader.kind == Stream::MemoryDependency::Kind::kShaderCode);
  CHECK(shader.byte_count == 36);
  CHECK(shader.packet_dword_offset == fixture.pixel_offset);
  CHECK(shader.physical_address == 0x00100000);
  // This is a copy of validated prefix data, never a partial executable stream.
  std::fill(fixture.words.begin(), fixture.words.end(), 0);
  CHECK(shader.byte_count == 36);
}

}  // namespace
}  // namespace xe::gpu::test
