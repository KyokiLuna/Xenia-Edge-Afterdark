/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <algorithm>
#include <initializer_list>
#include <type_traits>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/native_command_stream.h"
#include "xenia/gpu/native_frame_command_plan.h"

namespace xe::gpu::test {
namespace {
using Plan = NativeFrameCommandPlan;
static_assert(!std::is_default_constructible_v<Plan>);
static_assert(std::is_const_v<decltype(Plan::Result::plan)::element_type>);

struct Primary {
  std::vector<uint32_t> words;
  void Register(uint32_t index, std::initializer_list<uint32_t> values) {
    REQUIRE(values.size() != 0);
    words.push_back(uint32_t(values.size() - 1) << 16 | index);
    words.insert(words.end(), values);
  }
  uint32_t Packet(uint32_t opcode, const std::vector<uint32_t>& payload) {
    REQUIRE_FALSE(payload.empty());
    const auto offset = uint32_t(words.size());
    words.push_back(0xC0000000 | uint32_t(payload.size() - 1) << 16 |
                    opcode << 8);
    words.insert(words.end(), payload.begin(), payload.end());
    return offset;
  }
  uint32_t Shader(uint32_t stage = 0, bool in = true) {
    // Synthetic public ISA fields only. Shader semantics are NOT part of the
    // packet-profile proof, even when the words have valid instruction shape.
    const std::vector<uint32_t> code{0x1001, 0x2800, 0, 0, 0, 0};
    std::vector<uint32_t> payload{in ? stage : 0x00100000u | stage,
                                  uint32_t(code.size())};
    if (in) {
      payload.insert(payload.end(), code.begin(), code.end());
    }
    return Packet(in ? xenos::PM4_IM_LOAD_IMMEDIATE : xenos::PM4_IM_LOAD,
                  payload);
  }
  void Prepare(bool pixel = true) {
    Register(XE_GPU_REG_SQ_VS_CONST, {0x000FF000});
    Shader(0);
    if (pixel) {
      Register(XE_GPU_REG_SQ_PS_CONST, {0x000FF100});
      Shader(1);
    }
  }
  uint32_t Draw(
      xenos::EdramMode mode = xenos::EdramMode::kColorDepth,
      xenos::PrimitiveType primitive = xenos::PrimitiveType::kTriangleList,
      uint32_t count = 3, bool token = false) {
    Register(XE_GPU_REG_RB_MODECONTROL, {uint32_t(mode)});
    Register(XE_GPU_REG_PA_SC_VIZ_QUERY, {0});
    Register(XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, {0});
    const uint32_t initiator = count << 16 |
                               uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                               uint32_t(primitive);
    return Packet(token ? xenos::PM4_DRAW_INDX : xenos::PM4_DRAW_INDX_2,
                  token ? std::vector<uint32_t>{0, initiator}
                        : std::vector<uint32_t>{initiator});
  }
  uint32_t Flush() {
    return Packet(xenos::PM4_EVENT_WRITE, {xenos::CACHE_FLUSH});
  }
  void Coherency() {
    Register(XE_GPU_REG_COHER_SIZE_HOST, {0x1000, 0x2000, 0x03000000});
  }
  uint32_t Wait() {
    return Packet(xenos::PM4_WAIT_REG_MEM,
                  {3, XE_GPU_REG_COHER_STATUS_HOST, 0, 0x80000000, 8});
  }
  uint32_t OverlayEnable(uint32_t enable = 1) {
    const auto at = uint32_t(words.size());
    Register(XE_GPU_REG_D1OVL_UPDATE, {0x10000});
    Register(XE_GPU_REG_D1OVL_ALPHA_OUTSIDE_OVL_WINDOW, {0, 255, 0});
    Register(XE_GPU_REG_D1OVL_ENABLE, {enable});
    Register(XE_GPU_REG_D1OVL_UPDATE, {0});
    return at;
  }
  uint32_t OverlayWindow() {
    const auto at = uint32_t(words.size());
    Register(XE_GPU_REG_D1OVL_UPDATE, {0x10000});
    Register(XE_GPU_REG_D1OVL_CONTROL1, {0});
    Register(XE_GPU_REG_D1OVL_SURFACE_ADDRESS, {0x1FA4E000});
    Register(XE_GPU_REG_D1OVL_PITCH, {736});
    Register(XE_GPU_REG_D1OVL_SURFACE_OFFSET_X, {256, 576});
    Register(XE_GPU_REG_D1OVL_START, {0x01000240, 0x03D9029A});
    Register(XE_GPU_REG_D1OVL_UPDATE, {0});
    return at;
  }
  void CopyFlush() {
    Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
    Flush();
  }
  std::vector<uint8_t> Bytes() const {
    std::vector<uint8_t> bytes;
    for (uint32_t word : words) {
      bytes.push_back(uint8_t(word >> 24));
      bytes.push_back(uint8_t(word >> 16));
      bytes.push_back(uint8_t(word >> 8));
      bytes.push_back(uint8_t(word));
    }
    return bytes;
  }
};

void Rejected(const Primary& primary, Plan::Error error) {
  const auto result = Plan::Analyze(primary.Bytes());
  CHECK(result.error == error);
  CHECK_FALSE(result);
  CHECK_FALSE(result.plan);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.shader_semantics_verified);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Native frame profile owns all supported draw-family packets",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Coherency();
  p.Wait();
  p.Draw(xenos::EdramMode::kDepthOnly, xenos::PrimitiveType::kRectangleList, 3);
  p.Draw(xenos::EdramMode::kColorDepth, xenos::PrimitiveType::kTriangleList, 6,
         true);
  p.words.push_back(
      0x80000000);  // Canonical Type2 padding stays a whole packet.
  p.Draw(xenos::EdramMode::kColorDepth, xenos::PrimitiveType::kTriangleFan, 4,
         true);
  p.Draw(xenos::EdramMode::kColorDepth, xenos::PrimitiveType::kQuadList, 8,
         true);
  p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
  p.Flush();
  p.Register(XE_GPU_REG_COHER_DEST_BASE_1, {0});
  p.Register(XE_GPU_REG_RB_COPY_CONTROL, {0});
  p.Register(XE_GPU_REG_RB_COPY_DEST_INFO, {0});
  p.Register(XE_GPU_REG_WAIT_UNTIL, {0x20000});
  p.Register(XE_GPU_REG_TC_CNTL_STATUS, {1});
  p.Register(XE_GPU_REG_VC_CNTL, {1});
  const auto strict = NativeCommandStream::Analyze(p.Bytes());
  REQUIRE_FALSE(strict);
  auto bytes = p.Bytes();
  const auto result = Plan::Analyze(bytes);
  REQUIRE(result);
  REQUIRE(result.plan);
  CHECK(result.plan->inventory().draws.size() == 5);
  CHECK(result.plan->inventory().shaders.size() == 2);
  CHECK(result.plan->has_final_copy());
  CHECK(result.plan->has_cache_flush());
  CHECK(result.plan->inventory().state_invalidations == 1);
  // Retain the diagnostic invalidation of a recognized terminal event; never
  // repair unknown source bits or promote this into execution permission.
  CHECK(result.plan->inventory().draws[3].fetch_known.none());
  CHECK(result.plan->inventory().draws[3].constants.float_known.none());
  const auto padding = std::find_if(
      result.plan->packets().begin(), result.plan->packets().end(),
      [](const Plan::Packet& packet) {
        return packet.kind == Plan::PacketKind::kNop && packet.dword_count == 1;
      });
  CHECK(padding != result.plan->packets().end());
  const auto first_word = result.plan->words().front();
  bytes.assign(bytes.size(), 0);
  CHECK(result.plan->words().front() == first_word);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.shader_semantics_verified);
  CHECK_FALSE(result.ownership_verified);
  CHECK(NativeCommandStream::Analyze(p.Bytes()).error == strict.error);
}

TEST_CASE("Color rectangle groups share the ordinary implicit draw contract",
          "[graphics][native_frame_command_plan]") {
  for (uint32_t count : {3u, 6u, 9u}) {
    Primary p;
    p.Prepare();
    p.Draw(xenos::EdramMode::kColorDepth, xenos::PrimitiveType::kRectangleList,
           count);
    const auto result = Plan::Analyze(p.Bytes());
    REQUIRE(result);
    REQUIRE(result.plan->inventory().draws.size() == 1);
    CHECK(result.plan->inventory().draws[0].error ==
          NativeDrawReadInventory::Error::kNone);
    CHECK_FALSE(result.execution_ready);
    CHECK_FALSE(result.shader_semantics_verified);
    CHECK_FALSE(result.ownership_verified);
  }
}

TEST_CASE("Color rectangles reject incomplete or nonimplicit draw contracts",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  const auto at = p.Draw(xenos::EdramMode::kColorDepth,
                         xenos::PrimitiveType::kRectangleList, 6);
  SECTION("no vertices") { p.words[at + 1] &= 0xFFFF; }
  SECTION("one vertex") { p.words[at + 1] -= 5u << 16; }
  SECTION("two vertices") { p.words[at + 1] -= 4u << 16; }
  SECTION("four vertices") { p.words[at + 1] -= 2u << 16; }
  SECTION("five vertices") { p.words[at + 1] -= 1u << 16; }
  SECTION("seven vertices") { p.words[at + 1] += 1u << 16; }
  SECTION("nonzero VIZ query state") { p.words[at - 3] = 1; }
  SECTION("different output path") { p.words[at - 1] = 1; }
  SECTION("DMA indices") { p.words[at + 1] &= ~(3u << 6); }
  SECTION("explicit major mode") { p.words[at + 1] |= 1u << 8; }
  SECTION("index format bit") { p.words[at + 1] |= 1u << 11; }
  SECTION("not end of primitive") { p.words[at + 1] |= 1u << 12; }
  SECTION("reserved initiator bit") { p.words[at + 1] |= 1u << 10; }
  const auto result = Plan::Analyze(p.Bytes());
  CHECK(result.error == Plan::Error::kInvalidDraw);
  CHECK(result.error_dword_offset == at);
  CHECK_FALSE(result.plan);
  CHECK_FALSE(result.execution_ready);
}

TEST_CASE("Color rectangle token packets still reject a nonzero VIZ token",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  const auto at = p.Draw(xenos::EdramMode::kColorDepth,
                         xenos::PrimitiveType::kRectangleList, 6, true);
  p.words[at + 1] = 1;
  const auto result = Plan::Analyze(p.Bytes());
  CHECK(result.error == Plan::Error::kInvalidDraw);
  CHECK(result.error_dword_offset == at);
  CHECK_FALSE(result.plan);
}

TEST_CASE("Color rectangle support does not widen copy or depth profiles",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  auto mode = xenos::EdramMode::kDepthOnly;
  SECTION("depth-only is still one rectangle") {}
  SECTION("copy is still the exact backend rectangle") {
    mode = xenos::EdramMode::kCopy;
  }
  p.Draw(mode, xenos::PrimitiveType::kRectangleList, 6);
  Rejected(p, Plan::Error::kInvalidDraw);
}

TEST_CASE("Native color rectangles reach the terminal overlay request program",
          "[graphics][native_frame_command_plan][native_overlay]") {
  // Public packet/ISA fields model the native draw-family sequence, without
  // embedding captured shader or command bytes or claiming resource ownership.
  Primary p;
  p.Prepare();
  const auto color = p.Draw(xenos::EdramMode::kColorDepth,
                            xenos::PrimitiveType::kRectangleList, 6);
  p.Draw(xenos::EdramMode::kDepthOnly, xenos::PrimitiveType::kRectangleList, 3);
  const auto copy =
      p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
  p.Flush();
  p.Register(XE_GPU_REG_COHER_DEST_BASE_1, {0});
  p.Register(XE_GPU_REG_RB_COPY_CONTROL, {0});
  p.Register(XE_GPU_REG_RB_COPY_DEST_INFO, {0});
  p.Register(XE_GPU_REG_PA_SC_WINDOW_OFFSET, {0, 0, 0x03D9029A});
  p.OverlayEnable(1);
  p.OverlayWindow();
  p.OverlayEnable(0);
  p.Register(XE_GPU_REG_WAIT_UNTIL, {0x20000});
  p.Register(XE_GPU_REG_TC_CNTL_STATUS, {1});
  p.Register(XE_GPU_REG_VC_CNTL, {1});
  const auto result = Plan::Analyze(p.Bytes());
  REQUIRE(result);
  REQUIRE(result.plan->inventory().draws.size() == 3);
  CHECK(result.plan->inventory().draws[0].packet_dword_offset == color);
  CHECK(result.plan->inventory().draws[0].initiator == 0x00060088);
  for (const auto& draw : result.plan->inventory().draws) {
    CHECK(draw.error == NativeDrawReadInventory::Error::kNone);
  }
  REQUIRE(result.plan->final_copy_packet_dword_offset());
  CHECK(*result.plan->final_copy_packet_dword_offset() == copy);
  REQUIRE(result.plan->native_overlay_program());
  const auto requested = result.plan->native_overlay_program()->Apply({});
  REQUIRE(requested.valid());
  CHECK(requested.Has(NativeOverlayState::kGeometryKnown));
  CHECK(requested.enable == 0);
  CHECK(requested.surface_address == 0x1FA4E000);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.shader_semantics_verified);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Frame packet acceptance does not establish shader constant closure",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  const auto vertex_load = uint32_t(p.words.size());
  p.Shader(0, false);
  p.Draw();
  const auto result = Plan::Analyze(p.Bytes());
  REQUIRE(result);
  REQUIRE(result.plan->inventory().shaders.size() == 3);
  const auto& shader = result.plan->inventory().shaders.back();
  CHECK_FALSE(shader.is_inline);
  CHECK(shader.packet_dword_offset == vertex_load);
  CHECK(shader.raw_address == 0x00100000);
  CHECK(shader.physical_address == 0x00100000);
  CHECK(result.plan->inventory().draws[0].vertex_shader == 2);
  CHECK(result.plan->inventory().draws[0].constants.float_known.none());
  CHECK_FALSE(result.shader_semantics_verified);
  CHECK_FALSE(result.execution_ready);
}

TEST_CASE("Frame coherency records declared state without inventing completion",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Coherency();
  p.Wait();
  p.Wait();
  p.Draw();
  const auto result = Plan::Analyze(p.Bytes());
  REQUIRE(result);
  REQUIRE(result.plan->coherency_waits().size() == 2);
  CHECK(result.plan->coherency_waits()[0].status == 0x03000000);
  CHECK(result.plan->coherency_waits()[0].base == 0x2000);
  CHECK(result.plan->coherency_waits()[0].size == 0x1000);
  CHECK(result.plan->coherency_waits()[1].status == 0);
  CHECK_FALSE(result.execution_ready);
}

TEST_CASE("Frame wait profile rejects memory polls and undeclared coherency",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  bool declared = true;
  SECTION("missing local triplet") { declared = false; }
  if (declared) {
    p.Coherency();
  }
  const auto at = p.Wait();
  SECTION("memory poll") { p.words[at + 1] |= 0x10; }
  SECTION("different register") { p.words[at + 2] = XE_GPU_REG_SCRATCH_REG0; }
  SECTION("different mask") { p.words[at + 4] = 0xFFFFFFFF; }
  SECTION("unbounded poll delay") { p.words[at + 5] = 0x100; }
  p.Draw();
  Rejected(p, Plan::Error::kUnsupportedWait);
}

TEST_CASE("A coherency extent must fit even when each register value fits",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Register(XE_GPU_REG_COHER_SIZE_HOST, {0x20000000, 0x1000, 0x10000});
  p.Wait();
  p.Draw();
  Rejected(p, Plan::Error::kUnsupportedWait);
}

TEST_CASE("Frame shader loads reject stage size start and physical overflow",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  const auto at = p.Shader();
  auto error = Plan::Error::kInvalidShader;
  SECTION("unknown stage") { p.words[at + 1] = 2; }
  SECTION("nonzero instruction start") { p.words[at + 2] |= 1u << 16; }
  SECTION("zero size") { p.words[at + 2] = 0; }
  SECTION("extra inline payload") { --p.words[at + 2]; }
  SECTION("external code crosses physical end") {
    p.words.resize(at);
    p.Packet(xenos::PM4_IM_LOAD, {0x1FFFFFFC, 6});
    error = Plan::Error::kInvalidMemoryRange;
  }
  p.Draw();
  Rejected(p, error);
}

TEST_CASE("Shader invalidation requires a fresh local load",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Packet(xenos::PM4_INVALIDATE_STATE,
           {xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER});
  SECTION("previous binding cannot be inherited") {
    p.Draw();
    Rejected(p, Plan::Error::kMissingLocalState);
  }
  SECTION("fresh load repairs shader identity only") {
    p.Shader();
    p.Draw();
    const auto result = Plan::Analyze(p.Bytes());
    REQUIRE(result);
    CHECK(result.plan->inventory().draws[0].vertex_shader == 2);
    CHECK(result.plan->inventory().draws[0].constants.float_known.none());
  }
}

TEST_CASE("Frame profile rejects control flow and memory-signaling packets",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  SECTION("indirect buffer") {
    p.Packet(xenos::PM4_INDIRECT_BUFFER, {0x100000, 3});
  }
  SECTION("memory write") { p.Packet(xenos::PM4_MEM_WRITE, {0x100000, 1}); }
  SECTION("shader event with address") {
    p.Packet(xenos::PM4_EVENT_WRITE_SHD, {0, 0x100000, 1});
  }
  SECTION("unknown opcode") { p.Packet(0x7F, {0}); }
  SECTION("predicated NOP") {
    const auto at = p.Packet(xenos::PM4_NOP, {0});
    p.words[at] |= 1;
  }
  SECTION("reserved Type3 bit15") {
    const auto at = p.Packet(xenos::PM4_NOP, {0});
    p.words[at] |= 0x8000;
  }
  SECTION("Type1 is not a nested packet list") {
    p.words.insert(p.words.end(), {0x40000000, 0xC0004600, 6});
  }
  Rejected(p, Plan::Error::kUnsupportedPacket);
}

TEST_CASE("Only the no-address cache-flush event belongs to the frame profile",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  SECTION("different event") {
    p.Packet(xenos::PM4_EVENT_WRITE, {xenos::VS_DONE_TS});
  }
  SECTION("address payload") {
    p.Packet(xenos::PM4_EVENT_WRITE, {xenos::CACHE_FLUSH, 0x100000});
  }
  SECTION("a second event is not an epilogue") {
    p.Flush();
    p.Flush();
  }
  SECTION("no draw after terminal event") {
    p.Flush();
    p.Packet(xenos::PM4_DRAW_INDX_2, {0x30084});
  }
  Rejected(p, Plan::Error::kUnsupportedPacket);
}

TEST_CASE("Frame register allowlist excludes unrelated host side effects",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  auto index = uint32_t(XE_GPU_REG_SCRATCH_REG0);
  uint32_t value = 0;
  SECTION("scratch") {}
  SECTION("gamma") { index = XE_GPU_REG_DC_LUT_RW_INDEX; }
  SECTION("arbitrary low MMIO") { index = 1; }
  SECTION("event initiator requires its typed event") {
    index = XE_GPU_REG_VGT_EVENT_INITIATOR;
    value = 6;
  }
  SECTION("unsupported wait control bits") {
    index = XE_GPU_REG_WAIT_UNTIL;
    value = 1;
  }
  SECTION("unsupported cache control bits") {
    index = XE_GPU_REG_TC_CNTL_STATUS;
    value = 2;
  }
  SECTION("unsupported coherency flags") {
    index = XE_GPU_REG_COHER_STATUS_HOST;
    value = 0x80000000;
  }
  p.Register(index, {value});
  p.Draw();
  const auto result = Plan::Analyze(p.Bytes());
  CHECK(result.error == Plan::Error::kUnsupportedRegister);
  CHECK(result.error_register == index);
  CHECK_FALSE(result.plan);
}

TEST_CASE("Repeated register and invalid padding headers are not normalized",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  SECTION("one-register mode") {
    p.words.insert(p.words.end(),
                   {0x8000u | XE_GPU_REG_SHADER_CONSTANT_000_X, 0});
  }
  SECTION("Type2 reserved payload bits") { p.words.push_back(0x80000001); }
  Rejected(p, Plan::Error::kUnsupportedPacket);
}

TEST_CASE("Mode-aware shader selectors are local rather than reset defaults",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  SECTION("depth-only needs no pixel stage") {
    p.Prepare(false);
    p.Draw(xenos::EdramMode::kDepthOnly, xenos::PrimitiveType::kRectangleList,
           3);
    CHECK(Plan::Analyze(p.Bytes()));
  }
  SECTION("copy needs neither stage nor selector") {
    p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
    CHECK(Plan::Analyze(p.Bytes()));
  }
  SECTION("a color draw cannot inherit a pixel shader") {
    p.Prepare(false);
    p.Draw();
    Rejected(p, Plan::Error::kMissingLocalState);
  }
  SECTION("missing selector") {
    p.Shader(0);
    p.Shader(1);
    p.Draw();
    const auto result = Plan::Analyze(p.Bytes());
    CHECK(result.error == Plan::Error::kMissingLocalState);
    CHECK(result.error_register == XE_GPU_REG_SQ_VS_CONST);
    CHECK_FALSE(result.plan);
  }
  SECTION("selector zero is unsupported despite legacy title default") {
    p.Prepare();
    p.Register(XE_GPU_REG_SQ_PS_CONST, {0});
    p.Draw();
    Rejected(p, Plan::Error::kUnsupportedSelector);
  }
}

TEST_CASE("Draw mode and output path require explicit supported values",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  SECTION("missing output path is not a disabled reset value") {
    p.Register(XE_GPU_REG_RB_MODECONTROL,
               {uint32_t(xenos::EdramMode::kColorDepth)});
    p.Register(XE_GPU_REG_PA_SC_VIZ_QUERY, {0});
    p.Packet(xenos::PM4_DRAW_INDX_2, {0x30084});
    const auto result = Plan::Analyze(p.Bytes());
    CHECK(result.error == Plan::Error::kMissingLocalState);
    CHECK(result.error_register == XE_GPU_REG_VGT_OUTPUT_PATH_CNTL);
    CHECK_FALSE(result.plan);
  }
  SECTION("active output path cannot masquerade as ordinary geometry") {
    const auto draw = p.Draw();
    p.words.resize(draw);
    p.Register(XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, {1});
    p.Packet(xenos::PM4_DRAW_INDX_2, {0x30084});
    Rejected(p, Plan::Error::kInvalidDraw);
  }
  SECTION("unsupported EDRAM mode") {
    p.Draw(xenos::EdramMode::kNoOperation);
    Rejected(p, Plan::Error::kInvalidDraw);
  }
  SECTION("unknown high mode bits are not silently masked") {
    const auto draw = p.Draw();
    p.words.resize(draw);
    p.Register(XE_GPU_REG_RB_MODECONTROL,
               {uint32_t(xenos::EdramMode::kColorDepth) | 0x100});
    p.Packet(xenos::PM4_DRAW_INDX_2, {0x30084});
    Rejected(p, Plan::Error::kInvalidDraw);
  }
}

TEST_CASE("Sequential register ranges cannot cross an allowed family",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  const auto offset = uint32_t(p.words.size());
  p.Register(0x23FF, {0, 0});
  p.Draw();
  const auto result = Plan::Analyze(p.Bytes());
  CHECK(result.error == Plan::Error::kUnsupportedRegister);
  CHECK(result.error_dword_offset == offset);
  CHECK(result.error_register == 0x2400);
  CHECK_FALSE(result.plan);
}

TEST_CASE(
    "Frame geometry rejects incomplete quads indices VIZ and post-copy draws",
    "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  const auto at =
      p.Draw(xenos::EdramMode::kColorDepth, xenos::PrimitiveType::kQuadList, 4);
  SECTION("incomplete quad") { p.words[at + 1] += 1u << 16; }
  SECTION("DMA indices") { p.words[at + 1] &= ~(3u << 6); }
  SECTION("explicit major mode") { p.words[at + 1] |= 1u << 8; }
  SECTION("non-EOP") { p.words[at + 1] |= 1u << 12; }
  SECTION("VIZ state") {
    p.words.resize(at);
    p.Register(XE_GPU_REG_PA_SC_VIZ_QUERY, {1});
    p.Packet(xenos::PM4_DRAW_INDX_2, {0x4008D});
  }
  SECTION("post-copy draw") {
    p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
    p.Draw();
  }
  Rejected(p, Plan::Error::kInvalidDraw);
}

TEST_CASE("Post-flush register writes are restricted to the small epilogue",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  p.Flush();
  p.Register(XE_GPU_REG_RB_SURFACE_INFO, {640});
  Rejected(p, Plan::Error::kUnsupportedRegister);
}

TEST_CASE("Terminal native window restoration preserves real ordered state",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
  p.Flush();
  p.Register(XE_GPU_REG_COHER_DEST_BASE_1, {0});
  uint32_t offset = 0, tl = 0, br = 0x03D40354;
  SECTION("native record tail values") {}
  SECTION("signed offsets and offset-disabled empty scissor are valid state") {
    offset = (uint32_t(-7) & 0x7FFF) | (3u << 16);
    tl = 0x80000000 | 5 | (9u << 16);
    br = 0;
  }
  const uint32_t restore = uint32_t(p.words.size());
  p.Register(XE_GPU_REG_PA_SC_WINDOW_OFFSET, {offset, tl, br});
  // Actual native epilogue command literals, with a public-ISA test prelude.
  p.Register(XE_GPU_REG_RB_COPY_CONTROL, {0});
  p.Register(XE_GPU_REG_RB_COPY_DEST_INFO, {0});
  p.Register(XE_GPU_REG_WAIT_UNTIL, {0x20000});
  p.Register(XE_GPU_REG_TC_CNTL_STATUS, {1});
  p.Register(XE_GPU_REG_VC_CNTL, {1});
  const auto result = Plan::Analyze(p.Bytes());
  REQUIRE(result);
  CHECK(result.plan->has_final_copy());
  CHECK(result.plan->has_cache_flush());
  REQUIRE(result.plan->inventory().draws.size() == 2);
  const auto packet = std::find_if(
      result.plan->packets().begin(), result.plan->packets().end(),
      [&](const auto& item) { return item.dword_offset == restore; });
  REQUIRE(packet != result.plan->packets().end());
  CHECK(packet->kind == Plan::PacketKind::kRegisterWrite);
  CHECK(packet->dword_count == 4);
  std::vector<Plan::RegisterWrite> restored;
  for (const auto& write : result.plan->register_writes()) {
    if (write.packet_dword_offset == restore) {
      restored.push_back(write);
    }
  }
  REQUIRE(restored.size() == 3);
  CHECK(restored[0].index == XE_GPU_REG_PA_SC_WINDOW_OFFSET);
  CHECK(restored[0].value == offset);
  CHECK(restored[1].index == XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
  CHECK(restored[1].value == tl);
  CHECK(restored[2].index == XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
  CHECK(restored[2].value == br);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.shader_semantics_verified);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Terminal window state requires the complete valid context group",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  bool copy = true;
  SECTION("cache flush without a final copy does not admit restoration") {
    copy = false;
  }
  if (copy) {
    p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
  }
  p.Flush();
  const uint32_t at = uint32_t(p.words.size());
  p.Register(XE_GPU_REG_PA_SC_WINDOW_OFFSET, {0, 0, 0x01E00280});
  SECTION("partial group") { p.words[at] -= 1u << 16; }
  SECTION("extra register cannot piggyback") {
    p.words[at] += 1u << 16;
    p.words.push_back(0);
  }
  SECTION("different group start") { ++p.words[at]; }
  SECTION("offset X reserved bit") { p.words[at + 1] |= 1u << 15; }
  SECTION("offset Y reserved bit") { p.words[at + 1] |= 1u << 31; }
  SECTION("top-left X reserved bit") { p.words[at + 2] |= 1u << 14; }
  SECTION("top-left Y reserved bit") { p.words[at + 2] |= 1u << 30; }
  SECTION("bottom-right X reserved bit") { p.words[at + 3] |= 1u << 14; }
  SECTION("bottom-right Y reserved bit") { p.words[at + 3] |= 1u << 31; }
  // A failure in even the last word returns no partly accepted plan/prefix.
  Rejected(p, Plan::Error::kUnsupportedRegister);
}

TEST_CASE("Window restoration does not reopen terminal frame execution",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
  p.Flush();
  p.Register(XE_GPU_REG_PA_SC_WINDOW_OFFSET, {0, 0, 0x03D40354});
  auto error = Plan::Error::kUnsupportedPacket;
  SECTION("future draw") { p.Packet(xenos::PM4_DRAW_INDX_2, {0x30084}); }
  SECTION("future shader load") { p.Shader(); }
  SECTION("future coherency wait") { p.Wait(); }
  SECTION("second cache-flush event") { p.Flush(); }
  SECTION("addressed event") {
    p.Packet(xenos::PM4_EVENT_WRITE_CFL, {0, 0x100000});
  }
  SECTION("indirect buffer") {
    p.Packet(xenos::PM4_INDIRECT_BUFFER, {0x100000, 4});
  }
  SECTION("memory write") { p.Packet(xenos::PM4_MEM_WRITE, {0x100000, 1}); }
  SECTION("scratch MMIO") {
    p.Register(XE_GPU_REG_SCRATCH_REG0, {0});
    error = Plan::Error::kUnsupportedRegister;
  }
  SECTION("event initiator register") {
    p.Register(XE_GPU_REG_VGT_EVENT_INITIATOR, {6});
    error = Plan::Error::kUnsupportedRegister;
  }
  SECTION("unrelated render state is not part of this context group") {
    p.Register(XE_GPU_REG_RB_SURFACE_INFO, {640});
    error = Plan::Error::kUnsupportedRegister;
  }
  SECTION("cache-control value guards remain exact") {
    p.Register(XE_GPU_REG_TC_CNTL_STATUS, {2});
    error = Plan::Error::kUnsupportedRegister;
  }
  Rejected(p, error);
}

TEST_CASE("Later framing failure never returns an accepted frame prefix",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  p.words.push_back(0xC0012700);
  Rejected(p, Plan::Error::kTruncatedPacket);
}

TEST_CASE("Native frame bounds may be tightened but not raised",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  p.Draw();
  Plan::Limits limits;
  auto expected = Plan::Error::kInvalidLimits;
  SECTION("raise words") { ++limits.max_dwords; }
  SECTION("raise packets") { ++limits.max_packets; }
  SECTION("raise draws") { ++limits.max_draws; }
  SECTION("raise shader loads") { ++limits.max_shader_loads; }
  SECTION("raise shader size") { ++limits.max_shader_dwords; }
  SECTION("zero limit") { limits.max_packets = 0; }
  SECTION("smaller packet budget") {
    limits.max_packets = 1;
    expected = Plan::Error::kLimitExceeded;
  }
  SECTION("smaller word budget") {
    limits.max_dwords = uint32_t(p.words.size() - 1);
    expected = Plan::Error::kLimitExceeded;
  }
  SECTION("smaller draw budget") {
    limits.max_draws = 1;
    p.Draw();
    expected = Plan::Error::kLimitExceeded;
  }
  SECTION("smaller shader load budget") {
    limits.max_shader_loads = 1;
    expected = Plan::Error::kLimitExceeded;
  }
  SECTION("smaller shader word budget") {
    limits.max_shader_dwords = 5;
    expected = Plan::Error::kInvalidShader;
  }
  CHECK(Plan::Analyze(p.Bytes(), limits).error == expected);
}

TEST_CASE("Frame profile requires at least one complete draw",
          "[graphics][native_frame_command_plan]") {
  Primary p;
  p.Prepare();
  Rejected(p, Plan::Error::kNoDraw);
  CHECK(Plan::Analyze({}).error == Plan::Error::kInvalidByteCount);
  auto bytes = p.Bytes();
  bytes.pop_back();
  CHECK(Plan::Analyze(bytes).error == Plan::Error::kInvalidByteCount);
}

TEST_CASE(
    "Native overlay presentation permits clipped windows with checked ends",
    "[graphics][native_overlay]") {
  NativeOverlayPresentation p{-1920, -1080, -2000, -1100, 1920, 1080, 729, 90};
  REQUIRE(p.valid());
  SECTION("entirely outside is an animation state") {
    p.x = 10000;
    p.y = 10000;
    CHECK(p.valid());
  }
  SECTION("all normalized opacity values are consumer metadata") {
    for (uint32_t opacity = 0; opacity < 256; ++opacity) {
      p.opacity = uint8_t(opacity);
      CHECK(p.valid());
    }
  }
  SECTION("zero extent is invalid") {
    p.width = 0;
    CHECK_FALSE(p.valid());
  }
  SECTION("dimension limit is not a hardware geometry claim") {
    p.canvas_width = 16385;
    CHECK_FALSE(p.valid());
  }
  SECTION("window end cannot overflow int32") {
    p.x = INT32_MAX;
    CHECK_FALSE(p.valid());
  }
  SECTION("canvas end cannot overflow int32") {
    p.canvas_y = INT32_MAX;
    CHECK_FALSE(p.valid());
  }
}

TEST_CASE("Native overlay keeps raw requests outside the render register bank",
          "[graphics][native_frame_command_plan][native_overlay]") {
  static_assert(!std::is_default_constructible_v<NativeOverlayProgram>);
  Primary p;
  p.Prepare();
  const auto copy =
      p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
  p.Flush();
  const auto enable = p.OverlayEnable(1);
  const auto placement = p.OverlayWindow();
  const auto disable = p.OverlayEnable(0);
  p.Register(XE_GPU_REG_WAIT_UNTIL, {0x20000});
  p.Register(XE_GPU_REG_TC_CNTL_STATUS, {1});
  p.Register(XE_GPU_REG_VC_CNTL, {1});
  auto bytes = p.Bytes();
  const auto result = Plan::Analyze(bytes);
  REQUIRE(result);
  REQUIRE(result.plan->final_copy_packet_dword_offset());
  CHECK(*result.plan->final_copy_packet_dword_offset() == copy);
  const auto& overlay = result.plan->native_overlay_program();
  REQUIRE(overlay);
  REQUIRE(overlay->transactions().size() == 3);
  CHECK(overlay->transactions()[0].lock_packet_dword_offset == enable);
  CHECK(overlay->transactions()[1].lock_packet_dword_offset == placement);
  CHECK(overlay->transactions()[2].lock_packet_dword_offset == disable);
  const auto final = overlay->Apply({});
  REQUIRE(final.valid());
  CHECK(final.known_mask == NativeOverlayState::kAllKnown);
  CHECK(final.enable == 0);  // Requested disable, never scanout evidence.
  CHECK(final.surface_address == 0x1FA4E000);
  CHECK(final.pitch == 736);
  CHECK(final.start == 0x01000240);
  CHECK(final.end == 0x03D9029A);
  for (const auto& write : result.plan->register_writes()) {
    CHECK(write.index != XE_GPU_REG_D1OVL_UPDATE);
    CHECK(write.index != XE_GPU_REG_D1OVL_SURFACE_ADDRESS);
    CHECK(write.index != XE_GPU_REG_D1OVL_ENABLE);
  }
  NativeOverlayState running;
  for (const auto& packet : result.plan->packets()) {
    if (packet.kind != Plan::PacketKind::kNativeOverlayControl) {
      continue;
    }
    const auto before = running;
    REQUIRE(overlay->ApplyAtPacket(packet.dword_offset, running));
    const auto unlock = std::find_if(
        overlay->transactions().begin(), overlay->transactions().end(),
        [&](const auto& t) {
          return t.unlock_packet_dword_offset == packet.dword_offset;
        });
    if (unlock == overlay->transactions().end()) {
      CHECK(running == before);
    }
  }
  CHECK(running == final);
  CHECK_FALSE(overlay->ApplyAtPacket(copy, running));
  bytes.assign(bytes.size(), 0);
  CHECK(overlay->Apply({}) == final);
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE(
    "Enable-only overlay requests preserve unknown geometry and prior state",
    "[graphics][native_frame_command_plan][native_overlay]") {
  Primary p;
  p.Prepare();
  p.CopyFlush();
  p.OverlayEnable();
  const auto result = Plan::Analyze(p.Bytes());
  REQUIRE(result);
  REQUIRE(result.plan->native_overlay_program());
  const auto& program = *result.plan->native_overlay_program();
  auto state = program.Apply({});
  REQUIRE(state.valid());
  CHECK(state.known_mask ==
        (NativeOverlayState::kEnable | NativeOverlayState::kAlphaKnown));
  CHECK_FALSE(state.Has(NativeOverlayState::kGeometryKnown));
  NativeOverlayState prior;
  prior.Set(NativeOverlayState::Field::kSurfaceAddress, 0x00100000);
  prior.Set(NativeOverlayState::Field::kPitch, 640);
  state = program.Apply(prior);
  CHECK(state.surface_address == prior.surface_address);
  CHECK(state.pitch == prior.pitch);
  CHECK_FALSE(state.Has(NativeOverlayState::kStart));
  // A partial raw state is not a resource, output-layout or GPU permission.
  CHECK_FALSE(result.execution_ready);
}

TEST_CASE("Modal packet plans acquire no overlay program",
          "[graphics][native_frame_command_plan][native_overlay]") {
  Primary p;
  p.Prepare();
  p.Draw();
  const auto result = Plan::Analyze(p.Bytes());
  REQUIRE(result);
  CHECK_FALSE(result.plan->native_overlay_program());
  CHECK_FALSE(result.plan->final_copy_packet_dword_offset());
}

TEST_CASE(
    "Native overlay transactions require a genuine terminal copy and flush",
    "[graphics][native_frame_command_plan][native_overlay]") {
  Primary p;
  p.Prepare();
  p.Draw();
  SECTION("before final copy") { p.OverlayEnable(); }
  SECTION("flush alone does not prove resolve") {
    p.Flush();
    p.OverlayEnable();
  }
  SECTION("copy without flush") {
    p.Draw(xenos::EdramMode::kCopy, xenos::PrimitiveType::kRectangleList, 3);
    p.OverlayEnable();
  }
  Rejected(p, Plan::Error::kInvalidOverlayControl);
}

TEST_CASE(
    "Overlay enable groups reject missing nested or malformed transactions",
    "[graphics][native_frame_command_plan][native_overlay]") {
  Primary p;
  p.Prepare();
  p.CopyFlush();
  const auto at = p.OverlayEnable();
  SECTION("unlock without lock") { p.words[at + 1] = 0; }
  SECTION("missing unlock") { p.words.resize(p.words.size() - 2); }
  SECTION("nested lock") { p.words[at + 2] = XE_GPU_REG_D1OVL_UPDATE; }
  SECTION("wrong alpha count") { p.words[at + 2] -= 1u << 16; }
  SECTION("extra alpha write cannot piggyback") { p.words[at + 2] += 1u << 16; }
  SECTION("outside alpha unknown") { p.words[at + 3] = 1; }
  SECTION("inside alpha fade is not recovered") { p.words[at + 4] = 127; }
  SECTION("alpha mode unknown") { p.words[at + 5] = 1; }
  SECTION("nonboolean enable") { p.words[at + 7] = 2; }
  SECTION("unrelated MMIO in locked group") {
    p.words[at + 6] = XE_GPU_REG_SCRATCH_REG0;
  }
  SECTION("lock has reserved bits") { p.words[at + 1] |= 1; }
  SECTION("unlock is another lock") { p.words[at + 9] = 0x10000; }
  Rejected(p, Plan::Error::kInvalidOverlayControl);
}

TEST_CASE(
    "Overlay placement rejects unsupported format address and register bits",
    "[graphics][native_frame_command_plan][native_overlay]") {
  Primary p;
  p.Prepare();
  p.CopyFlush();
  const auto at = p.OverlayWindow();
  SECTION("unknown control format") { p.words[at + 3] = 0x100; }
  SECTION("surface is not page aligned") { p.words[at + 5] |= 1; }
  SECTION("surface physical overflow") { p.words[at + 5] = 0x20000000; }
  SECTION("zero surface does not grant absent-output ownership") {
    p.words[at + 5] = 0;
  }
  SECTION("pitch not32 aligned") { p.words[at + 7] = 735; }
  SECTION("zero pitch") { p.words[at + 7] = 0; }
  SECTION("X origin is not256 aligned") { p.words[at + 9] |= 1; }
  SECTION("Y origin is not2 aligned") { p.words[at + 10] |= 1; }
  SECTION("window start reserved bits") { p.words[at + 12] |= 1u << 12; }
  SECTION("window end reserved bits") { p.words[at + 13] |= 1u << 28; }
  SECTION("reordered surface and pitch packets") {
    std::swap(p.words[at + 4], p.words[at + 6]);
  }
  SECTION("partial paired offsets") { p.words[at + 8] -= 1u << 16; }
  SECTION("unrelated register cannot be appended inside transaction") {
    p.words[at + 14] = XE_GPU_REG_SCRATCH_REG0;
  }
  Rejected(p, Plan::Error::kInvalidOverlayControl);
}

TEST_CASE(
    "Native overlay cannot reopen shader draw or arbitrary MMIO admission",
    "[graphics][native_frame_command_plan][native_overlay]") {
  Primary p;
  p.Prepare();
  p.CopyFlush();
  p.OverlayEnable();
  auto expected = Plan::Error::kUnsupportedPacket;
  SECTION("draw after transaction") {
    p.Packet(xenos::PM4_DRAW_INDX_2, {0x30084});
  }
  SECTION("shader after transaction") { p.Shader(); }
  SECTION("memory write after transaction") {
    p.Packet(xenos::PM4_MEM_WRITE, {0x100000, 1});
  }
  SECTION("unlocked display write is not general MMIO permission") {
    p.Register(XE_GPU_REG_D1OVL_ENABLE, {1});
    expected = Plan::Error::kUnsupportedRegister;
  }
  Rejected(p, expected);
}

TEST_CASE("Overlay transaction count shares strict whole-plan limits",
          "[graphics][native_frame_command_plan][native_overlay]") {
  Primary p;
  p.Prepare();
  p.CopyFlush();
  for (uint32_t i = 0; i < NativeOverlayProgram::kMaxTransactions; ++i) {
    p.OverlayEnable(i & 1);
  }
  SECTION("bounded complete program") {
    const auto result = Plan::Analyze(p.Bytes());
    REQUIRE(result);
    REQUIRE(result.plan->native_overlay_program());
    CHECK(result.plan->native_overlay_program()->transactions().size() == 64);
  }
  SECTION("extra transaction returns no accepted prefix") {
    p.OverlayEnable();
    Rejected(p, Plan::Error::kInvalidOverlayControl);
  }
  SECTION("each packet is charged against plan budget") {
    Plan::Limits limits;
    limits.max_packets = 12;
    const auto result = Plan::Analyze(p.Bytes(), limits);
    CHECK(result.error == Plan::Error::kLimitExceeded);
    CHECK_FALSE(result.plan);
  }
}

// The observed native no-draw packet shape uses public PM4/register values.
// Parsing is not native Task, allocation, GPU or presentation authority.
Primary ControlPrimary() {
  Primary p;
  p.words = {0x00022080, 0,          0,          0x03D40354, 0x00000A31,
             0x03000000, 0x00010A2F, 0x20000000, 0,          0xC0043C00,
             3,          0xA31,      0,          0x80000000, 8,
             0x00022080, 0,          0,          0x03D40354, 0x0000186B,
             0x10000,    0x000218C1, 0,          255,        0,
             0x00001860, 0,          0x0000186B, 0};
  return p;
}
TEST_CASE("Complete native disable has a typed no-image control plan",
          "[graphics][native_frame_command_plan][control]") {
  const auto p = ControlPrimary();
  const auto result = Plan::Analyze(p.Bytes());
  REQUIRE(result);
  CHECK(result.plan->kind() == Plan::Kind::kOverlayControl);
  CHECK(result.plan->packets().size() == 9);
  CHECK(result.plan->inventory().draws.empty());
  CHECK(result.plan->inventory().shaders.empty());
  CHECK_FALSE(result.plan->has_final_copy());
  CHECK_FALSE(result.plan->has_cache_flush());
  REQUIRE(result.plan->coherency_waits().size() == 1);
  const auto& wait = result.plan->coherency_waits().front();
  CHECK(wait.packet_dword_offset == 9);
  CHECK(wait.status == 0x03000000);
  CHECK(wait.base == 0);
  CHECK(wait.size == 0x20000000);
  REQUIRE(result.plan->native_overlay_program());
  const auto requested = result.plan->native_overlay_program()->Apply({});
  CHECK(requested.valid());
  CHECK(requested.Has(NativeOverlayState::kEnable |
                      NativeOverlayState::kAlphaKnown));
  CHECK(requested.enable == 0);
  CHECK_FALSE(requested.Has(NativeOverlayState::kGeometryKnown));
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.ownership_verified);
}
TEST_CASE("Control records reject malformed waits locks and invented surfaces",
          "[graphics][native_frame_command_plan][control]") {
  auto p = ControlPrimary();
  SECTION("enable is never a no-image output") { p.words[26] = 1; }
  SECTION("incomplete unlock") { p.words.pop_back(); }
  SECTION("wrong wait mask") { p.words[13] = 0xFFFFFFFF; }
  SECTION("coherency range overflow") { p.words[8] = 1; }
  SECTION("missing required coherency state") { p.words[6] = 0x00010A2D; }
  SECTION("surface address is not a harmless control write") {
    p.words[25] = 0x1862;
    p.words[26] = 0x100000;
  }
  SECTION("unrelated render state is not a control frame") {
    p.words[0] = 0x00022000;
  }
  SECTION("no wait") {
    p.words.erase(p.words.begin() + 9, p.words.begin() + 15);
  }
  SECTION("post-unlock prefix must not escape") {
    p.words.push_back(0x80000000);
  }
  const auto result = Plan::Analyze(p.Bytes());
  CHECK_FALSE(result);
  CHECK_FALSE(result.plan);
}
TEST_CASE("Draw shader and memory effects never become no-image controls",
          "[graphics][native_frame_command_plan][control]") {
  auto p = ControlPrimary();
  SECTION("draw") { p.Packet(xenos::PM4_DRAW_INDX_2, {0x30088}); }
  SECTION("shader") { p.Shader(); }
  SECTION("event write") {
    p.Packet(xenos::PM4_EVENT_WRITE, {xenos::CACHE_FLUSH});
  }
  SECTION("memory write packet") { p.Packet(0x3D, {0x100000, 1}); }
  const auto result = Plan::Analyze(p.Bytes());
  CHECK_FALSE(result);
  CHECK_FALSE(result.plan);
}
}  // namespace
}  // namespace xe::gpu::test
