/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/native_command_stream.h"
#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/native_shader_read_analysis.h"
#include "xenia/gpu/ucode.h"

namespace xe::gpu::test {
namespace {
using Analysis = NativeShaderReadAnalysis;
using Inventory = NativeDrawReadInventory;
using Instruction = std::array<uint32_t, 3>;

// Synthetic ISA bitfields from ucode.h only. No supplied shader code, hashes,
// addresses, GPU objects, emulator or guest memory are used in these tests.
Instruction Fetch(uint32_t slot, bool mini = false, int32_t offset = 0) {
  return {(uint32_t(1) << 19) | ((slot / 3) << 20) | ((slot % 3) << 25),
          0x688 | (uint32_t(xenos::VertexFormat::k_32_32_FLOAT) << 16) |
              (uint32_t(mini) << 30),
          2 | ((uint32_t(offset) & 0x7FFFFF) << 8)};
}

std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> result;
  for (const auto word : words) {
    result.push_back(uint8_t(word >> 24));
    result.push_back(uint8_t(word >> 16));
    result.push_back(uint8_t(word >> 8));
    result.push_back(uint8_t(word));
  }
  return result;
}

std::vector<uint32_t> ShaderWords(
    std::initializer_list<Instruction> instructions, uint32_t sequence) {
  REQUIRE(instructions.size() <= 6);
  // CF0 exece at group1, CF1 nop. CFs pack two48-bit instructions into96bits.
  std::vector<uint32_t> result{
      1 | (uint32_t(instructions.size()) << 12) | (sequence << 16),
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12 |
          uint32_t(ucode::AddressingMode::kAbsolute) << 11,
      0};
  for (const auto& instruction : instructions) {
    result.insert(result.end(), instruction.begin(), instruction.end());
  }
  return result;
}

struct Primary {
  std::vector<uint32_t> words;
  void Packet(uint32_t opcode, const std::vector<uint32_t>& payload) {
    REQUIRE_FALSE(payload.empty());
    words.push_back(0xC0000000 | uint32_t(payload.size() - 1) << 16 |
                    opcode << 8);
    words.insert(words.end(), payload.begin(), payload.end());
  }
  void Register(uint32_t index, std::initializer_list<uint32_t> values) {
    words.push_back(uint32_t(values.size() - 1) << 16 | index);
    words.insert(words.end(), values);
  }
  void InlineShader(const std::vector<uint32_t>& shader,
                    xenos::ShaderType stage = xenos::ShaderType::kVertex) {
    std::vector<uint32_t> payload{uint32_t(stage), uint32_t(shader.size())};
    payload.insert(payload.end(), shader.begin(), shader.end());
    Packet(xenos::PM4_IM_LOAD_IMMEDIATE, payload);
  }
  void Draw(xenos::EdramMode mode, xenos::PrimitiveType primitive,
            uint32_t vertices, bool with_token = false) {
    Register(XE_GPU_REG_RB_MODECONTROL, {uint32_t(mode)});
    Register(XE_GPU_REG_PA_SC_VIZ_QUERY, {0});
    Register(XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, {0});
    const uint32_t initiator = vertices << 16 |
                               uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                               uint32_t(primitive);
    Packet(with_token ? xenos::PM4_DRAW_INDX : xenos::PM4_DRAW_INDX_2,
           with_token ? std::vector<uint32_t>{0, initiator}
                      : std::vector<uint32_t>{initiator});
  }
  Inventory::ScanResult Scan() const { return Inventory::Scan(Bytes(words)); }
};

TEST_CASE("Bounded shader metadata finds slot95 and inherited mini fetch",
          "[graphics][native_read_inventory]") {
  const auto input = Bytes(ShaderWords({Fetch(95), Fetch(0, true, 1)}, 5));
  auto result = Analysis::Analyze(xenos::ShaderType::kVertex, input);
  REQUIRE(result);
  CHECK(result.error == Analysis::Error::kNone);
  CHECK((result.vertex_fetch_bitmap ==
         std::array<uint32_t, 3>{0, 0, 0x80000000}));
  REQUIRE(result.vertex_fetches.size() == 2);
  CHECK(result.vertex_fetches[0].slot == 95);
  CHECK(result.vertex_fetches[1].slot == 95);
  CHECK(result.vertex_fetches[1].mini);
  CHECK(result.vertex_fetches[1].offset_words == 1);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.access_bounds_verified);
}

TEST_CASE("Shader preflight rejects malformed or unsupported instructions",
          "[graphics][native_read_inventory]") {
  auto words = ShaderWords({Fetch(95)}, 1);
  Analysis::Error expected = Analysis::Error::kInvalidFetch;
  SECTION("reserved selection reaches slot96") { words[3] |= 3u << 25; }
  SECTION("mini lacks preceding full") { words[4] |= 1u << 30; }
  SECTION("zero stride cannot enter asserted legacy binding path") {
    words[5] &= ~255u;
  }
  SECTION("invalid format") { words[4] &= ~(63u << 16); }
  SECTION("invalid destination swizzle6") { words[4] |= 6; }
  SECTION("predicated fetch is outside straightline subset") {
    words[4] |= 1u << 31;
  }
  SECTION("texture fetch never enters legacy texture parser") {
    words[3] |= 1;
    expected = Analysis::Error::kUnsupportedTexture;
  }
  SECTION("exec payload leaves owned code") {
    words[0] = (words[0] & ~0xFFFu) | 2;
    expected = Analysis::Error::kInvalidExec;
  }
  SECTION("exec points into CF table") {
    words[0] &= ~0xFFFu;
    expected = Analysis::Error::kInvalidExec;
  }
  SECTION("conditional execution is not guessed") {
    words[1] = uint32_t(ucode::ControlFlowOpcode::kCondExecEnd) << 12;
    expected = Analysis::Error::kUnsupportedControlFlow;
  }
  SECTION("memory alloc is rejected before analysis") {
    words[1] = uint32_t(ucode::ControlFlowOpcode::kAlloc) << 12 |
               uint32_t(ucode::AllocType::kMemory) << 9;
    expected = Analysis::Error::kUnsupportedMemexport;
  }
  const auto result =
      Analysis::Analyze(xenos::ShaderType::kVertex, Bytes(words));
  CHECK(result.error == expected);
  CHECK_FALSE(result);
  CHECK(result.vertex_fetches.empty());
  CHECK((result.vertex_fetch_bitmap == std::array<uint32_t, 3>{}));
}

TEST_CASE(
    "ALU preflight rejects unknown operations and exports before legacy "
    "parsing",
    "[graphics][native_read_inventory]") {
  Instruction op{};
  auto expected = Analysis::Error::kInvalidAlu;
  SECTION("unknown vector opcode") { op[2] = 31u << 24; }
  SECTION("unknown scalar opcode") { op[0] = 63u << 26; }
  SECTION("predicated ALU") { op[1] = 1u << 28; }
  SECTION("memory export data") {
    op[0] = uint32_t(ucode::ExportRegister::kExportData0) | 1u << 15 | 1u << 16;
    expected = Analysis::Error::kUnsupportedMemexport;
  }
  SECTION("unsupported vertex export") { op[0] = 30 | 1u << 15 | 1u << 16; }
  const auto result = Analysis::Analyze(xenos::ShaderType::kVertex,
                                        Bytes(ShaderWords({op}, 0)));
  CHECK(result.error == expected);
  CHECK_FALSE(result.metadata_analyzed);
  CHECK(result.vertex_fetches.empty());
}

TEST_CASE("A later shader error discards earlier used-fetch candidates",
          "[graphics][native_read_inventory]") {
  const auto words = ShaderWords({Fetch(95), Instruction{0, 0, 31u << 24}}, 1);
  const auto result =
      Analysis::Analyze(xenos::ShaderType::kVertex, Bytes(words));
  CHECK(result.error == Analysis::Error::kInvalidAlu);
  CHECK(result.vertex_fetches.empty());
  CHECK((result.vertex_fetch_bitmap == std::array<uint32_t, 3>{}));
}

TEST_CASE(
    "Pixel ALU metadata is separate from vertex and has no fabricated fetch",
    "[graphics][native_read_inventory]") {
  const auto bytes = Bytes(ShaderWords({Instruction{}}, 0));
  const auto result = Analysis::Analyze(xenos::ShaderType::kPixel, bytes);
  REQUIRE(result);
  CHECK((result.vertex_fetch_bitmap == std::array<uint32_t, 3>{}));
  CHECK_FALSE(result.execution_ready);
  const auto illegal = Analysis::Analyze(xenos::ShaderType::kPixel,
                                         Bytes(ShaderWords({Fetch(0)}, 1)));
  CHECK(illegal.error == Analysis::Error::kInvalidFetch);
}

TEST_CASE("Shader diagnostic limits and dump policy are fail closed",
          "[graphics][native_read_inventory]") {
  const auto bytes = Bytes(ShaderWords({Fetch(0)}, 1));
  SECTION("hard limits cannot grow") {
    Analysis::Limits limits;
    ++limits.max_dwords;
    CHECK(Analysis::Analyze(xenos::ShaderType::kVertex, bytes, limits).error ==
          Analysis::Error::kInvalidLimits);
  }
  SECTION("exec budget") {
    auto longer = Bytes(ShaderWords({Fetch(0), Fetch(0)}, 5));
    Analysis::Limits limits;
    limits.max_exec_instructions = 1;
    CHECK(Analysis::Analyze(xenos::ShaderType::kVertex, longer, limits).error ==
          Analysis::Error::kLimitExceeded);
  }
  SECTION("complete96bit groups are required") {
    CHECK(Analysis::Analyze(xenos::ShaderType::kVertex,
                            std::span(bytes).first(bytes.size() - 4))
              .error == Analysis::Error::kInvalidByteCount);
  }
  SECTION("global dump setting is never mutated or used") {
    struct Restore {
      std::filesystem::path previous = cvars::dump_shaders;
      ~Restore() { cvars::dump_shaders = previous; }
    } restore;
    cvars::dump_shaders = std::filesystem::path("diagnostic-must-not-write");
    CHECK(Analysis::Analyze(xenos::ShaderType::kVertex, bytes).error ==
          Analysis::Error::kShaderDumpEnabled);
    CHECK(cvars::dump_shaders ==
          std::filesystem::path("diagnostic-must-not-write"));
  }
}

TEST_CASE("Draw diagnostics require only actual used fetch pairs",
          "[graphics][native_read_inventory]") {
  const auto shader = ShaderWords({Fetch(95)}, 1);
  const auto analysis =
      Analysis::Analyze(xenos::ShaderType::kVertex, Bytes(shader));
  REQUIRE(analysis);
  Primary primary;
  primary.InlineShader(shader);
  constexpr uint32_t fetch_first = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
  primary.Register(fetch_first + 190, {0x00100003, 16u << 2});
  primary.Draw(xenos::EdramMode::kColorDepth,
               xenos::PrimitiveType::kTriangleFan, 4, true);
  const auto raw = Bytes(primary.words);
  const auto strict_before = NativeCommandStream::Analyze(raw);
  REQUIRE_FALSE(strict_before);
  auto scan = primary.Scan();
  REQUIRE(scan.complete_primary_scan);
  REQUIRE(scan.draws.size() == 1);
  const auto& draw = scan.draws.front();
  CHECK(draw.fetch_known.count() == 2);
  auto reads = Inventory::VertexReads(draw, &analysis);
  REQUIRE(reads.error == Inventory::Error::kNone);
  REQUIRE(reads.reads.size() == 1);
  CHECK(reads.reads[0].slot == 95);
  CHECK(reads.reads[0].physical_address == 0x00100000);
  CHECK(reads.reads[0].byte_count == 64);
  CHECK_FALSE(reads.execution_ready);
  CHECK_FALSE(reads.access_bounds_verified);
  auto missing = draw;
  missing.fetch_known.reset(191);
  reads = Inventory::VertexReads(missing, &analysis);
  CHECK(reads.error == Inventory::Error::kMissingState);
  CHECK(reads.error_slot == 95);
  CHECK(reads.reads.empty());
  const auto strict_after = NativeCommandStream::Analyze(raw);
  CHECK(strict_after.error == strict_before.error);
  CHECK_FALSE(strict_after.stream);
  const auto pixel = Analysis::Analyze(xenos::ShaderType::kPixel,
                                       Bytes(ShaderWords({Instruction{}}, 0)));
  REQUIRE(pixel);
  CHECK(Inventory::VertexReads(draw, &pixel).error ==
        Inventory::Error::kUnsupportedShader);
}

TEST_CASE("Copy diagnostics require backend fetch0 not last shader binding",
          "[graphics][native_read_inventory]") {
  Primary primary;
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                   {0x00200003, 6u << 2 | 2u});
  primary.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList,
               3);
  auto scan = primary.Scan();
  REQUIRE(scan.complete_primary_scan);
  REQUIRE(scan.draws.size() == 1);
  const auto result = Inventory::VertexReads(scan.draws[0], nullptr);
  REQUIRE(result.error == Inventory::Error::kNone);
  REQUIRE(result.reads.size() == 1);
  CHECK(result.reads[0].slot == 0);
  CHECK(result.reads[0].resolve_rectangle);
  CHECK(result.reads[0].byte_count == 24);
  CHECK(result.reads[0].endian == 2);
  auto wrong = scan.draws[0];
  wrong.fetch_words[1] = 7u << 2;
  CHECK(Inventory::VertexReads(wrong, nullptr).error ==
        Inventory::Error::kInvalidVertexFetch);
}

TEST_CASE(
    "Used vertex extents reject invalid type, physical overflow and budgets",
    "[graphics][native_read_inventory]") {
  const auto shader = ShaderWords({Fetch(0)}, 1);
  const auto metadata =
      Analysis::Analyze(xenos::ShaderType::kVertex, Bytes(shader));
  REQUIRE(metadata);
  Primary primary;
  primary.InlineShader(shader);
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                   {0x00200003, 6u << 2});
  primary.Draw(xenos::EdramMode::kColorDepth,
               xenos::PrimitiveType::kTriangleList, 3);
  auto scan = primary.Scan();
  REQUIRE(scan.draws.size() == 1);
  auto draw = scan.draws[0];
  auto error = Inventory::Error::kInvalidVertexFetch;
  SECTION("invalid vertex type is never allowed by global bypass setting") {
    draw.fetch_words[0] = 0x00200001;
  }
  SECTION("zero length") { draw.fetch_words[1] = 0; }
  SECTION("capture bound cannot be raised") {
    draw.fetch_words[1] = (0x100001u << 2);
  }
  SECTION("physical end overflow") {
    draw.fetch_words[0] = 0xFFFFFFFC | 3;
    error = Inventory::Error::kInvalidMemoryRange;
  }
  const auto result = Inventory::VertexReads(draw, &metadata);
  CHECK(result.error == error);
  CHECK(result.reads.empty());
}

TEST_CASE("Unknown state or malformed tails cannot fabricate geometry closure",
          "[graphics][native_read_inventory]") {
  Primary primary;
  primary.InlineShader(ShaderWords({Fetch(0)}, 1));
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                   {0x00200003, 6u << 2});
  primary.Draw(xenos::EdramMode::kDepthOnly,
               xenos::PrimitiveType::kRectangleList, 3);
  SECTION("IB does not recurse and poisons inherited state") {
    primary.Packet(xenos::PM4_INDIRECT_BUFFER, {0x00100000, 3});
    primary.Draw(xenos::EdramMode::kDepthOnly,
                 xenos::PrimitiveType::kRectangleList, 3);
    const auto result = primary.Scan();
    REQUIRE(result.complete_primary_scan);
    REQUIRE(result.draws.size() == 2);
    CHECK(result.state_invalidations == 1);
    CHECK_FALSE(result.draws[1].vertex_shader);
    CHECK(result.draws[1].fetch_known.none());
  }
  SECTION("later framing error discards every candidate") {
    primary.words.push_back(0xC0012700);
    const auto result = primary.Scan();
    CHECK_FALSE(result.complete_primary_scan);
    CHECK(result.shaders.empty());
    CHECK(result.draws.empty());
  }
}

TEST_CASE("Auto-index quad inventory keeps the original four input vertices",
          "[graphics][native_read_inventory]") {
  auto full = Fetch(95);
  full[2] = 4;  // Four words per vertex, XY then mini-fetched UV.
  auto mini = Fetch(0, true, 2);
  mini[2] = (mini[2] & ~255u) | 4;
  const auto shader = ShaderWords({full, mini}, 5);
  const auto metadata =
      Analysis::Analyze(xenos::ShaderType::kVertex, Bytes(shader));
  REQUIRE(metadata);
  REQUIRE(metadata.vertex_fetches.size() == 2);
  CHECK(metadata.vertex_fetches[1].slot == 95);
  CHECK(metadata.vertex_fetches[1].offset_words == 2);
  Primary primary;
  primary.InlineShader(shader);
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190,
                   {0x00100003, (32u * 4) << 2});
  primary.Draw(xenos::EdramMode::kColorDepth, xenos::PrimitiveType::kQuadList,
               32);
  const auto scan = primary.Scan();
  REQUIRE(scan.draws.size() == 1);
  const auto reads = Inventory::VertexReads(scan.draws[0], &metadata);
  REQUIRE(reads.error == Inventory::Error::kNone);
  REQUIRE(reads.reads.size() == 1);
  CHECK(reads.reads[0].byte_count == 512);
  CHECK_FALSE(reads.execution_ready);
  CHECK_FALSE(reads.access_bounds_verified);
  // Host conversion emits (a,b,c),(a,c,d) per four guest vertices. The
  // inventory neither invents another guest input nor approves execution.
  CHECK_FALSE(NativeCommandStream::Analyze(Bytes(primary.words)));
  for (const uint32_t count : {0u, 1u, 2u, 3u, 5u, 65535u}) {
    auto bad = scan.draws[0];
    bad.initiator = (bad.initiator & 0xFFFF) | (count << 16);
    CHECK(Inventory::VertexReads(bad, &metadata).error ==
          Inventory::Error::kUnsupportedGeometry);
  }
  for (const uint32_t flag : {1u << 6, 1u << 8, 1u << 11}) {
    auto bad = scan.draws[0];
    bad.initiator ^= flag;
    CHECK(Inventory::VertexReads(bad, &metadata).error ==
          Inventory::Error::kUnsupportedGeometry);
  }
  auto queried = scan.draws[0];
  queried.state_words[1] = 1;
  CHECK(Inventory::VertexReads(queried, &metadata).error ==
        Inventory::Error::kUnsupportedGeometry);
  queried = scan.draws[0];
  queried.state_words[2] = 1;
  CHECK(Inventory::VertexReads(queried, &metadata).error ==
        Inventory::Error::kUnsupportedGeometry);
}

// Generic normalized 2D TFETCH encoding from public ucode bitfields. It uses
// inherited sampler filters, not any supplied shader's instruction bytes.
Instruction Sample(uint32_t slot) {
  return {1u | 1u << 19 | slot << 20,
          0x688u | 3u << 12 | 3u << 14 | 3u << 16 | 7u << 18 | 3u << 24 |
              3u << 26 | 1u << 28,
          uint32_t(xenos::FetchOpDimension::k2D) << 14};
}

xenos::xe_gpu_texture_fetch_t LinearR8(uint32_t width = 128,
                                       uint32_t height = 128) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.clamp_x = fetch.clamp_y = xenos::ClampMode::kClampToEdge;
  fetch.pitch = 256 >> 5;
  fetch.format = xenos::TextureFormat::k_8;
  fetch.base_address = 0x00100000 >> 12;
  fetch.size_2d.width = width - 1;
  fetch.size_2d.height = height - 1;
  fetch.swizzle = 0x688;
  fetch.mag_filter = fetch.min_filter = fetch.mip_filter =
      xenos::TextureFilter::kLinear;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  return fetch;
}

void SetTexture(Inventory::Draw& draw, uint32_t slot,
                const xenos::xe_gpu_texture_fetch_t& fetch) {
  std::memcpy(draw.fetch_words.data() + slot * 6, &fetch, sizeof(fetch));
  for (uint32_t i = 0; i < 6; ++i) {
    draw.fetch_known.set(slot * 6 + i);
  }
}

Inventory::Draw TextureDraw(const std::vector<uint32_t>& shader) {
  Primary primary;
  primary.InlineShader(shader, xenos::ShaderType::kPixel);
  primary.Draw(xenos::EdramMode::kColorDepth, xenos::PrimitiveType::kQuadList,
               4);
  auto scan = primary.Scan();
  REQUIRE(scan.complete_primary_scan);
  REQUIRE(scan.draws.size() == 1);
  REQUIRE(scan.draws[0].pixel_shader);
  return scan.draws[0];
}

TEST_CASE("Texture snapshots distinguish texels from row and storage padding",
          "[graphics][native_read_inventory]") {
  const auto shader = ShaderWords({Sample(0), Sample(0)}, 5);
  const auto metadata =
      Analysis::Analyze(xenos::ShaderType::kPixel, Bytes(shader));
  REQUIRE(metadata);
  REQUIRE(metadata.texture_fetches.size() == 2);
  auto draw = TextureDraw(shader);
  auto fetch = LinearR8();
  uint32_t extent = 32640, logical = 16384, rows = 32768, storage = 32768;
  SECTION("non-square row padding is not trailing readable data") {}
  SECTION("full-width rows") {
    fetch = LinearR8(256, 256);
    extent = logical = rows = storage = 65536;
  }
  SECTION("a single row does not read the rest of its storage allocation") {
    fetch = LinearR8(128, 1);
    extent = logical = 128;
    rows = 256;
    storage = 8192;
  }
  SetTexture(draw, 0, fetch);
  const auto result = Inventory::TextureReads(draw, &metadata);
  REQUIRE(result.error == Inventory::Error::kNone);
  REQUIRE(result.reads.size() == 1);
  const auto& read = result.reads[0];
  CHECK(read.slot == 0);
  CHECK(read.byte_count == extent);
  CHECK(read.logical_texel_bytes == logical);
  CHECK(read.row_pitch_bytes == 256);
  CHECK(read.padded_span_bytes == rows);
  CHECK(read.storage_stride_bytes == storage);
  CHECK(read.physical_address == 0x00100000);
  CHECK(read.width == fetch.size_2d.width + 1);
  CHECK(read.height == fetch.size_2d.height + 1);
  CHECK(read.format == uint32_t(xenos::TextureFormat::k_8));
  CHECK(read.endian == uint32_t(xenos::Endian::kNone));
  CHECK(read.fetch_words[3] == fetch.dword_3);
  REQUIRE(read.instructions.size() == 2);
  CHECK(read.instructions[0].instruction_words == Sample(0));
  CHECK(read.instructions[1].instruction_words == Sample(0));
  CHECK(draw.fetch_known.count() == 6);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.access_bounds_verified);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Texture inventory retains explicit versus inherited sampler state",
          "[graphics][native_read_inventory]") {
  auto instruction = Sample(0);
  instruction[1] &= ~((3u << 12) | (3u << 14));
  instruction[1] |= 1u << 12 | 1u << 14;  // Explicit linear min/mag.
  const auto shader = ShaderWords({instruction}, 1);
  const auto metadata =
      Analysis::Analyze(xenos::ShaderType::kPixel, Bytes(shader));
  REQUIRE(metadata);
  auto draw = TextureDraw(shader);
  auto fetch = LinearR8();
  fetch.mag_filter = fetch.min_filter = xenos::TextureFilter::kPoint;
  fetch.mip_filter = xenos::TextureFilter::kBaseMap;
  SetTexture(draw, 0, fetch);
  const auto result = Inventory::TextureReads(draw, &metadata);
  REQUIRE(result.error == Inventory::Error::kNone);
  REQUIRE(result.reads.size() == 1);
  CHECK(result.reads[0].fetch_words[3] == fetch.dword_3);
  CHECK(result.reads[0].instructions[0].instruction_words == instruction);
  const auto inherited_shader = ShaderWords({Sample(0)}, 1);
  const auto inherited_metadata =
      Analysis::Analyze(xenos::ShaderType::kPixel, Bytes(inherited_shader));
  REQUIRE(inherited_metadata);
  CHECK(Inventory::TextureReads(draw, &inherited_metadata).error ==
        Inventory::Error::kUnsupportedTexture);
}

TEST_CASE("Texture inventory fails on missing state and unsupported layout",
          "[graphics][native_read_inventory]") {
  const auto shader = ShaderWords({Sample(0)}, 1);
  const auto metadata =
      Analysis::Analyze(xenos::ShaderType::kPixel, Bytes(shader));
  REQUIRE(metadata);
  auto draw = TextureDraw(shader);
  auto fetch = LinearR8();
  auto expected = Inventory::Error::kUnsupportedTexture;
  bool missing = false;
  SECTION("one missing descriptor word cannot be filled from defaults") {
    expected = Inventory::Error::kMissingState;
    missing = true;
  }
  SECTION("volumes need another layout contract") {
    fetch.dimension = xenos::DataDimension::k3D;
  }
  SECTION("16-bit format needs another decoder") {
    fetch.format = xenos::TextureFormat::k_16;
  }
  SECTION("stacked arrays need another span contract") { fetch.stacked = 1; }
  SECTION("effective mip chain is not base only") {
    fetch.mip_address = 1;
    fetch.mip_max_level = 1;
  }
  SECTION("mip-only effective resource is not an owned base") {
    fetch.base_address = 0;
    fetch.mip_address = 1;
    fetch.mip_min_level = fetch.mip_max_level = 1;
    expected = Inventory::Error::kInvalidTextureFetch;
  }
  SECTION(
      "packed storage with effective extra mip resources is outside the "
      "profile") {
    fetch.packed_mips = 1;
    fetch.mip_address = 1;
    fetch.mip_max_level = 1;
  }
  SECTION("repeat changes edge behavior") {
    fetch.clamp_x = xenos::ClampMode::kRepeat;
  }
  SECTION("signed texels are not unsigned normalized") {
    fetch.sign_x = xenos::TextureSign::kSigned;
  }
  SECTION("integer numeric mode is outside the profile") {
    fetch.num_format = 1;
  }
  SECTION("exponent adjustment needs another numeric profile") {
    fetch.exp_adjust = 1;
  }
  SECTION("anisotropy needs another sampling contract") {
    fetch.aniso_filter = xenos::AnisoFilter::kMax_2_1;
  }
  SECTION("border storage needs another memory profile") {
    fetch.border_size = 1;
  }
  SECTION("pitch smaller than width") {
    fetch.size_2d.width = 256;
    expected = Inventory::Error::kInvalidTextureLayout;
  }
  SECTION("pitch is not linear-row aligned") {
    fetch.pitch = 4;
    expected = Inventory::Error::kInvalidTextureLayout;
  }
  SECTION("zero address is not an allocation") {
    fetch.base_address = 0;
    expected = Inventory::Error::kInvalidTextureFetch;
  }
  SECTION("a masked base page of zero is absent in the common cache") {
    fetch.base_address = 0xA0000;
    expected = Inventory::Error::kInvalidTextureFetch;
  }
  SECTION("physical end is bounded") {
    fetch.base_address = 0x1FFFF;
    expected = Inventory::Error::kInvalidMemoryRange;
  }
  SECTION("per-span capture budget is fixed") {
    fetch.size_2d.width = fetch.size_2d.height = 8191;
    fetch.pitch = 8192 >> 5;
    expected = Inventory::Error::kLimitExceeded;
  }
  SECTION("invalid fetch type") {
    fetch.type = xenos::FetchConstantType::kVertex;
    expected = Inventory::Error::kInvalidTextureFetch;
  }
  SetTexture(draw, 0, fetch);
  if (missing) {
    draw.fetch_known.reset(5);
  }
  const auto result = Inventory::TextureReads(draw, &metadata);
  CHECK(result.error == expected);
  CHECK(result.error_slot == 0);
  CHECK(result.reads.empty());
}

TEST_CASE("Texture GPU addresses use the common page mask without CPU bias",
          "[graphics][native_read_inventory]") {
  const auto shader = ShaderWords({Sample(0)}, 1);
  const auto metadata =
      Analysis::Analyze(xenos::ShaderType::kPixel, Bytes(shader));
  REQUIRE(metadata);
  auto draw = TextureDraw(shader);
  auto fetch = LinearR8();
  fetch.base_address = 0xE0010;
  SetTexture(draw, 0, fetch);
  const auto result = Inventory::TextureReads(draw, &metadata);
  REQUIRE(result.error == Inventory::Error::kNone);
  REQUIRE(result.reads.size() == 1);
  CHECK(result.reads[0].raw_address == 0xE0010000);
  CHECK(result.reads[0].physical_address == 0x00010000);
}

TEST_CASE("Texture candidates are atomic and depth or copy ignores its PS",
          "[graphics][native_read_inventory]") {
  const auto shader = ShaderWords({Sample(0), Sample(31)}, 5);
  const auto metadata =
      Analysis::Analyze(xenos::ShaderType::kPixel, Bytes(shader));
  REQUIRE(metadata);
  auto draw = TextureDraw(shader);
  SetTexture(draw, 0, LinearR8());
  SetTexture(draw, 31, LinearR8(256, 256));
  const auto good = Inventory::TextureReads(draw, &metadata);
  REQUIRE(good.error == Inventory::Error::kNone);
  REQUIRE(good.reads.size() == 2);
  CHECK(good.reads[1].slot == 31);
  auto bad = draw;
  bad.fetch_known.reset(191);
  const auto failed = Inventory::TextureReads(bad, &metadata);
  CHECK(failed.error == Inventory::Error::kMissingState);
  CHECK(failed.error_slot == 31);
  CHECK(failed.reads.empty());
  bad = draw;
  bad.pixel_shader.reset();
  CHECK(Inventory::TextureReads(bad, &metadata).error ==
        Inventory::Error::kUnsupportedShader);
  CHECK(Inventory::TextureReads(draw, nullptr).error ==
        Inventory::Error::kUnsupportedShader);
  auto wrong_stage = metadata;
  wrong_stage.stage = xenos::ShaderType::kVertex;
  CHECK(Inventory::TextureReads(draw, &wrong_stage).error ==
        Inventory::Error::kUnsupportedShader);
  for (const auto mode :
       {xenos::EdramMode::kDepthOnly, xenos::EdramMode::kCopy}) {
    Primary primary;
    primary.Draw(mode, xenos::PrimitiveType::kRectangleList, 3);
    const auto scan = primary.Scan();
    REQUIRE(scan.draws.size() == 1);
    const auto result = Inventory::TextureReads(scan.draws[0], nullptr);
    CHECK(result.error == Inventory::Error::kNone);
    CHECK(result.reads.empty());
  }
}
}  // namespace
}  // namespace xe::gpu::test
