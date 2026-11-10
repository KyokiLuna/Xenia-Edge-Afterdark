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
#include <initializer_list>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/native_shader_read_analysis.h"
#include "xenia/gpu/native_vertex_access_analysis.h"
#include "xenia/gpu/ucode.h"

namespace xe::gpu::test {
namespace {
using Analysis = NativeShaderReadAnalysis;
using Inventory = NativeDrawReadInventory;
using Instruction = std::array<uint32_t, 3>;
using Cf = std::array<uint32_t, 2>;
using Op = ucode::ControlFlowOpcode;

// Public ucode.h fields only. No supplied shader bytes, addresses, hashes,
// Kernel/Task objects, GPU execution or sampled-branch golden results.
Cf Exec(Op op, uint32_t group, uint32_t sequence = 0) {
  return {group | 1u << 12 | sequence << 16, uint32_t(op) << 12 | 1u << 11};
}

Cf Jump(uint32_t target = 4, bool unconditional = false,
        bool predicated = false) {
  return {target | uint32_t(unconditional) << 13 | uint32_t(predicated) << 14,
          uint32_t(Op::kCondJmp) << 12 | 128u << 2};
}

Instruction Constant(uint32_t index) {
  // Export MAX(cN,cN) to color0; scalar retain-prev has no writes.
  return {
      1u << 15 | 15u << 16 |
          uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26,
      0,
      index << 16 | index << 8 | uint32_t(ucode::AluVectorOpcode::kMax) << 24};
}

Instruction Sample(uint32_t slot) {
  return {uint32_t(ucode::FetchOpcode::kTextureFetch) | 1u << 19 | slot << 20,
          0x688u | 3u << 12 | 3u << 14 | 3u << 16 | 7u << 18 | 3u << 24 |
              3u << 26 | 1u << 28,
          uint32_t(xenos::FetchOpDimension::k2D) << 14};
}

struct Program {
  std::array<Cf, 6> cf{Jump(), Exec(Op::kExec, 3),    Exec(Op::kExecEnd, 4),
                       Cf{},   Exec(Op::kExecEnd, 5), Cf{}};
  std::array<Instruction, 3> body{Constant(1), Constant(2), Constant(7)};
  std::vector<uint32_t> Words() const {
    std::vector<uint32_t> words;
    for (uint32_t i = 0; i < cf.size(); i += 2) {
      const auto& a = cf[i];
      const auto& b = cf[i + 1];
      words.insert(words.end(), {a[0], (a[1] & 0xFFFF) | b[0] << 16,
                                 b[0] >> 16 | b[1] << 16});
    }
    for (const auto& i : body) {
      words.insert(words.end(), i.begin(), i.end());
    }
    return words;
  }
};

std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> bytes;
  for (uint32_t w : words) {
    bytes.insert(bytes.end(), {uint8_t(w >> 24), uint8_t(w >> 16),
                               uint8_t(w >> 8), uint8_t(w)});
  }
  return bytes;
}

Analysis::Result Analyze(const Program& p,
                         xenos::ShaderType stage = xenos::ShaderType::kPixel) {
  return Analysis::Analyze(stage, Bytes(p.Words()));
}

void Failed(const Analysis::Result& r, Analysis::Error error) {
  CHECK(r.error == error);
  CHECK_FALSE(r.metadata_analyzed);
  CHECK_FALSE(r.execution_ready);
  CHECK_FALSE(r.access_bounds_verified);
  CHECK(r.texture_fetches.empty());
  CHECK(r.texture_fetch_bitmap == 0);
  CHECK(r.vertex_fetches.empty());
  CHECK((r.vertex_fetch_bitmap == std::array<uint32_t, 3>{}));
  CHECK(r.constant_use.float_count == 0);
  CHECK((r.constant_use.bool_bitmap == std::array<uint32_t, 8>{}));
}

struct DrawFixture {
  std::vector<uint32_t> words;
  Analysis::Result vertex;
  void Register(uint32_t index, std::initializer_list<uint32_t> values) {
    words.push_back(uint32_t(values.size() - 1) << 16 | index);
    words.insert(words.end(), values.begin(), values.end());
  }
  void Shader(xenos::ShaderType stage, const std::vector<uint32_t>& code) {
    words.insert(words.end(), {0xC0000000 | uint32_t(code.size() + 1) << 16 |
                                   uint32_t(xenos::PM4_IM_LOAD_IMMEDIATE) << 8,
                               uint32_t(stage), uint32_t(code.size())});
    words.insert(words.end(), code.begin(), code.end());
  }
  Inventory::Draw Make(const Program& pixel, bool bool_known = true,
                       bool bool_value = false, bool all_floats = true) {
    const auto exec = Exec(Op::kExecEnd, 1);
    const Instruction no_constants{
        uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26, 0,
        (1u << 29) | (1u << 30) | (1u << 31)};
    std::vector<uint32_t> code{exec[0], exec[1], 0};
    code.insert(code.end(), no_constants.begin(), no_constants.end());
    vertex = Analysis::Analyze(xenos::ShaderType::kVertex, Bytes(code));
    REQUIRE(vertex);
    Shader(xenos::ShaderType::kVertex, code);
    Shader(xenos::ShaderType::kPixel, pixel.Words());
    Register(XE_GPU_REG_SQ_VS_CONST, {0x000FF000, 0x000FF100});
    for (uint32_t i : {1u, 2u, 7u}) {
      if (i != 7 || all_floats) {
        Register(XE_GPU_REG_SHADER_CONSTANT_000_X + 1024 + i * 4,
                 {0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000});
      }
    }
    if (bool_known) {
      Register(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 4,
               {uint32_t(bool_value)});
    }
    Register(XE_GPU_REG_RB_MODECONTROL,
             {uint32_t(xenos::EdramMode::kColorDepth)});
    Register(XE_GPU_REG_PA_SC_VIZ_QUERY, {0});
    Register(XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, {0});
    words.insert(words.end(),
                 {0xC0000000 | uint32_t(xenos::PM4_DRAW_INDX_2) << 8,
                  3u << 16 | uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                      uint32_t(xenos::PrimitiveType::kTriangleList)});
    const auto scan = Inventory::Scan(Bytes(words));
    REQUIRE(scan.complete_primary_scan);
    REQUIRE(scan.draws.size() == 1);
    return scan.draws.front();
  }
};

void SetTexture(Inventory::Draw& draw, uint32_t slot) {
  xenos::xe_gpu_texture_fetch_t f{};
  f.type = xenos::FetchConstantType::kTexture;
  f.clamp_x = f.clamp_y = xenos::ClampMode::kClampToEdge;
  f.pitch = 256 >> 5;
  f.format = xenos::TextureFormat::k_8;
  f.base_address = 0x100000 >> 12;
  f.size_2d.width = f.size_2d.height = 127;
  f.swizzle = 0x688;
  f.mag_filter = f.min_filter = f.mip_filter = xenos::TextureFilter::kLinear;
  f.dimension = xenos::DataDimension::k2DOrStacked;
  std::memcpy(draw.fetch_words.data() + slot * 6, &f, sizeof(f));
  for (uint32_t i = 0; i < 6; ++i) {
    draw.fetch_known.set(slot * 6 + i);
  }
}

TEST_CASE("Forward pixel alternatives retain both terminal constant paths",
          "[graphics][native_pixel_flow]") {
  Program p;
  bool boolean = true;
  uint32_t additional_bool_mask = 0;
  SECTION("bool conditional forward branch") {}
  SECTION("unconditional branch keeps lexical read union") {
    p.cf[0] = Jump(4, true);
    boolean = false;
  }
  SECTION("predicate branch has no invented bool constant") {
    p.cf[0] = Jump(4, false, true);
    boolean = false;
  }
  SECTION("unreachable branch keeps its bool read") {
    p.cf[0] = Jump(4, true);
    boolean = false;
    p.cf[3] = Jump(4);
    p.cf[3][1] = uint32_t(Op::kCondJmp) << 12 | 255u << 2;
    additional_bool_mask = 0x80000000;
  }
  const auto r = Analyze(p);
  REQUIRE(r);
  CHECK(r.error == Analysis::Error::kNone);
  CHECK(r.constant_use.float_count == 3);
  CHECK(r.constant_use.float_bitmap[0] == ((1u << 1) | (1u << 2) | (1u << 7)));
  CHECK(r.constant_use.bool_bitmap[4] == uint32_t(boolean));
  CHECK(r.constant_use.bool_bitmap[7] == additional_bool_mask);
  CHECK(r.constant_use.loop_bitmap == 0);
  CHECK_FALSE(r.execution_ready);
  CHECK_FALSE(r.access_bounds_verified);
}

TEST_CASE("Pixel forward targets and all-path termination are closed",
          "[graphics][native_pixel_flow]") {
  Program p;
  auto error = Analysis::Error::kUnsupportedControlFlow;
  SECTION("self target") { p.cf[0] = Jump(0); }
  SECTION("backward target in unreachable block") { p.cf[3] = Jump(1); }
  SECTION("target leaves CF table") { p.cf[0] = Jump(6); }
  SECTION("target is not masked") { p.cf[0] = Jump(8191); }
  SECTION("reserved jump low bits") { p.cf[0][0] |= 1u << 15; }
  SECTION("reserved jump high bit") { p.cf[0][1] |= 1; }
  SECTION("loop is not a forward branch") {
    p.cf[0][1] = uint32_t(Op::kLoopStart) << 12;
  }
  SECTION("call needs another contract") {
    p.cf[0][1] = uint32_t(Op::kCondCall) << 12;
  }
  SECTION("return needs another contract") {
    p.cf[0][1] = uint32_t(Op::kReturn) << 12;
  }
  SECTION("conditional exec is separate") {
    p.cf[0][1] = uint32_t(Op::kCondExecEnd) << 12;
  }
  SECTION("one alternative escapes without END") {
    p.cf[2] = Jump(5, true);
    error = Analysis::Error::kMissingEnd;
  }
  SECTION("last reachable exec is not terminal") {
    p.cf[4] = Exec(Op::kExec, 5);
    error = Analysis::Error::kMissingEnd;
  }
  SECTION("overlapping exec payloads") {
    p.cf[4] = Exec(Op::kExecEnd, 3);
    error = Analysis::Error::kInvalidExec;
  }
  SECTION("payload overlaps CF table") {
    p.cf[4] = Exec(Op::kExecEnd, 2);
    error = Analysis::Error::kInvalidExec;
  }
  Failed(Analyze(p), error);
}

TEST_CASE("Pixel read union needs descriptors even for unselected blocks",
          "[graphics][native_pixel_flow]") {
  Program p;
  p.cf[0] = Jump(4, true);
  p.cf[1] = Exec(Op::kExec, 3, 1);
  p.cf[4] = Exec(Op::kExecEnd, 5, 1);
  p.body[0] = Sample(2);
  p.body[2] = Sample(7);
  const auto pixel = Analyze(p);
  REQUIRE(pixel);
  REQUIRE(pixel.texture_fetches.size() == 2);
  CHECK(pixel.texture_fetch_bitmap == ((1u << 2) | (1u << 7)));
  DrawFixture f;
  auto draw = f.Make(p);
  CHECK(Inventory::TextureReads(draw, &pixel).error ==
        Inventory::Error::kMissingState);
  SetTexture(draw, 2);
  const auto one = Inventory::TextureReads(draw, &pixel);
  CHECK(one.error == Inventory::Error::kMissingState);
  CHECK(one.error_slot == 7);
  SetTexture(draw, 7);
  const auto both = Inventory::TextureReads(draw, &pixel);
  REQUIRE(both.error == Inventory::Error::kNone);
  CHECK(both.reads.size() == 2);
  CHECK_FALSE(both.execution_ready);
  CHECK_FALSE(both.access_bounds_verified);
  CHECK_FALSE(both.ownership_verified);
}

TEST_CASE("Pixel branch constants need actual bool and both float paths",
          "[graphics][native_pixel_flow]") {
  const Program p;
  const auto pixel = Analyze(p);
  REQUIRE(pixel);
  bool known = true, value = false, floats = true;
  auto error = Inventory::Error::kNone;
  SECTION("known false closes both paths") {}
  SECTION("known true closes both paths") { value = true; }
  SECTION("unknown bool is not false") {
    known = false;
    error = Inventory::Error::kMissingState;
  }
  SECTION("other terminal float cannot be pruned") {
    floats = false;
    error = Inventory::Error::kMissingState;
  }
  DrawFixture f;
  const auto draw = f.Make(p, known, value, floats);
  const auto r = Inventory::ConstantReads(draw, &f.vertex, &pixel);
  CHECK(r.error == error);
  CHECK(r.used_constant_values_known == (error == Inventory::Error::kNone));
  if (error == Inventory::Error::kNone) {
    REQUIRE(r.bools.size() == 1);
    CHECK(r.bools[0].word_index == 4);
    CHECK(r.bools[0].used_mask == 1);
    CHECK(r.bools[0].known);
    CHECK(r.bools[0].value == uint32_t(value));
    CHECK(r.floats.size() == 3);
  }
}

TEST_CASE("Vertex bounds cannot inherit pixel branch admission",
          "[graphics][native_pixel_flow]") {
  const Program p;
  Failed(Analyze(p, xenos::ShaderType::kVertex),
         Analysis::Error::kUnsupportedControlFlow);
  DrawFixture f;
  const auto draw = f.Make(p);
  const auto r = NativeVertexAccessAnalysis::Analyze(draw, Bytes(p.Words()));
  CHECK_FALSE(r.vertex_input_bounds_verified);
  CHECK(r.error == NativeVertexAccessAnalysis::Error::kInvalidShader);
  CHECK(r.accesses.empty());
}

TEST_CASE("Later pixel failure discards every read candidate",
          "[graphics][native_pixel_flow]") {
  Program p;
  p.cf[1] = Exec(Op::kExec, 3, 1);
  p.body[0] = Sample(2);
  p.body[2][2] = 31u << 24;
  auto error = Analysis::Error::kInvalidAlu;
  SECTION("later invalid ALU") {}
  SECTION("unreachable instructions remain checked") {
    p.cf[0] = Jump(4, true);
    p.body[0][1] |= 1u << 31;
    error = Analysis::Error::kUnsupportedTexture;
  }
  SECTION("memory export cannot hide behind branch") {
    p.body[2][0] =
        uint32_t(ucode::ExportRegister::kExportData0) | 1u << 15 | 1u << 16;
    p.body[2][2] = 0;
    error = Analysis::Error::kUnsupportedMemexport;
  }
  Failed(Analyze(p), error);
  Analysis::Limits limits;
  limits.max_exec_instructions = 2;
  const Program valid;
  Failed(Analysis::Analyze(xenos::ShaderType::kPixel, Bytes(valid.Words()),
                           limits),
         Analysis::Error::kLimitExceeded);
}
}  // namespace
}  // namespace xe::gpu::test
