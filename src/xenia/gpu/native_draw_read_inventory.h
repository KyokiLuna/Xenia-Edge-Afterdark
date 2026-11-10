/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_DRAW_READ_INVENTORY_H_
#define XENIA_GPU_NATIVE_DRAW_READ_INVENTORY_H_

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xenia/gpu/native_shader_read_analysis.h"
#include "xenia/gpu/native_system_title_input.h"
#include "xenia/gpu/registers.h"

namespace xe::gpu {

// Linear diagnostic state, separate from NativeCommandStream acceptance. No
// recursive IBs, inherited register values, guest pointers or executable
// stream. Unknown state-changing packets discard known state rather than
// guessing it.
class NativeDrawReadInventory {
 public:
  enum class Error {
    kNone,
    kInvalidPrimary,
    kLimitExceeded,
    kInvalidRegisterRange,
    kMissingState,
    kUnsupportedGeometry,
    kUnsupportedShader,
    kInvalidVertexFetch,
    kInvalidTextureFetch,
    kUnsupportedTexture,
    kInvalidTextureLayout,
    kInvalidMemoryRange,
    kInvalidConstantMetadata,
    kUnsupportedConstantAddressing,
    kUnsupportedConstantSelector,
  };
  struct ShaderLoad {
    xenos::ShaderType stage;
    uint32_t packet_dword_offset;
    uint32_t dword_count;
    bool is_inline;
    uint32_t inline_dword_offset;
    uint32_t raw_address;
    uint32_t physical_address;
  };
  inline static constexpr std::array<uint32_t, 8> kStateRegisters = {
      XE_GPU_REG_RB_MODECONTROL,       XE_GPU_REG_PA_SC_VIZ_QUERY,
      XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, XE_GPU_REG_VGT_MAX_VTX_INDX,
      XE_GPU_REG_VGT_MIN_VTX_INDX,     XE_GPU_REG_VGT_INDX_OFFSET,
      XE_GPU_REG_SQ_PROGRAM_CNTL,      XE_GPU_REG_SQ_CONTEXT_MISC};
  struct ConstantState {
    // Both256-vector stage banks, retained only from explicit guest writes.
    // Zero-initialized storage is never a declaration without a known bit.
    std::array<uint32_t, 2048> float_words{};
    std::bitset<2048> float_known;
    std::array<uint32_t, 8> bool_words{};
    std::bitset<8> bool_known;
    std::array<uint32_t, 32> loop_words{};
    std::bitset<32> loop_known;
    // Vertex and pixel SQ_*_CONST selectors, in that order.
    std::array<uint32_t, 2> selectors{};
    std::bitset<2> selectors_known;
  };
  struct Draw {
    uint32_t packet_dword_offset;
    uint32_t opcode;
    uint32_t initiator;
    uint32_t viz_token;
    std::optional<uint32_t> vertex_shader;
    std::optional<uint32_t> pixel_shader;
    std::array<uint32_t, 192> fetch_words{};
    std::bitset<192> fetch_known;
    std::array<uint32_t, kStateRegisters.size()> state_words{};
    std::bitset<kStateRegisters.size()> state_known;
    Error error = Error::kNone;
    ConstantState constants{};
    std::shared_ptr<const NativeSystemTitleInput> title_input;
  };
  struct ScanResult {
    Error error = Error::kNone;
    uint32_t error_dword_offset = 0;
    bool complete_primary_scan = false;
    bool execution_ready = false;
    uint32_t state_invalidations = 0;
    std::vector<ShaderLoad> shaders;
    std::vector<Draw> draws;
  };
  struct VertexRead {
    uint32_t slot;
    uint32_t raw_address;
    uint32_t physical_address;
    uint32_t byte_count;
    uint32_t endian;
    bool resolve_rectangle;
  };
  struct ReadResult {
    Error error = Error::kNone;
    uint32_t error_slot = UINT32_MAX;
    bool execution_ready = false;
    bool access_bounds_verified = false;
    std::vector<VertexRead> reads;
  };
  struct TextureRead {
    uint32_t slot;
    // Logical host-endian descriptor and exact preflighted shader instructions.
    // Retain sampler overrides instead of replacing them with guessed defaults.
    std::array<uint32_t, 6> fetch_words;
    std::vector<NativeShaderReadAnalysis::TextureFetch> instructions;
    bool from_title_input = false;
    uint32_t raw_address;
    uint32_t physical_address;
    // Common-layout loader extent rounded to 16 bytes, including row gaps and
    // the final load vector. Neither this nor padding proves an allocation.
    uint32_t byte_count;
    uint64_t logical_texel_bytes;
    uint32_t row_pitch_bytes;
    uint32_t padded_span_bytes;
    uint32_t storage_stride_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t endian;
  };
  struct TextureReadResult {
    Error error = Error::kNone;
    uint32_t error_slot = UINT32_MAX;
    bool execution_ready = false;
    bool access_bounds_verified = false;
    bool ownership_verified = false;
    std::vector<TextureRead> reads;
  };
  struct FloatValue {
    xenos::ShaderType stage;
    uint32_t index;
    std::array<uint32_t, 4> words{};
    // One bit per raw word. Unknown words must be represented as unknown,
    // rather than treating their zero-initialized storage as a source value.
    uint32_t known_mask = 0;
  };
  struct BoolValue {
    xenos::ShaderType stage;
    uint32_t word_index;
    uint32_t used_mask;
    uint32_t value = 0;
    bool known = false;
  };
  struct LoopValue {
    xenos::ShaderType stage;
    uint32_t index;
    uint32_t value = 0;
    bool known = false;
  };
  struct ConstantReadResult {
    Error error = Error::kNone;
    uint32_t error_register = UINT32_MAX;
    xenos::ShaderType error_stage = xenos::ShaderType::kVertex;
    uint32_t error_constant_index = UINT32_MAX;
    bool used_constant_values_known = false;
    bool execution_ready = false;
    bool shader_semantics_verified = false;
    std::vector<FloatValue> floats;
    std::vector<BoolValue> bools;
    std::vector<LoopValue> loops;
  };

  static ScanResult Scan(std::span<const uint8_t> big_endian_primary);
  static ScanResult Scan(
      std::span<const uint8_t> big_endian_primary,
      std::shared_ptr<const NativeSystemTitleInput> title_input);
  // Copy mode uses the backend's explicit fetch0 / 24-byte rectangle contract
  // regardless of active shaders. Other modes need the exact selected VS's
  // successfully preflighted metadata. Full declared spans are conservative
  // snapshots, NOT proof of actual ALU-generated index bounds or ownership.
  static ReadResult VertexReads(const Draw& draw,
                                const NativeShaderReadAnalysis::Result* vertex);
  // Potential reads of the exact selected, successfully preflighted PS.
  // Supports native-owned unsigned-normalized 2D R8/RGBA8888 base
  // format families, preserving original fetch/endian/swizzle; clamp X/Y edge,
  // effective linear min/mag, no anisotropy, linear/tiled/packed base storage
  // with no extra effective mip resources. DepthOnly and Copy perform no PS
  // texture reads. Always diagnostic, never a backend
  // texture binding, access/ownership proof or execution approval.
  static TextureReadResult TextureReads(
      const Draw& draw, const NativeShaderReadAnalysis::Result* pixel);
  // Exact selected, successfully preflighted metadata only. Known explicit
  // fixed-bank selectors are required; no reset defaults or live RF values
  // are inherited. Copy ignores both shaders, DepthOnly ignores the PS.
  // Missing constant words retain their known masks for diagnostic output.
  static ConstantReadResult ConstantReads(
      const Draw& draw, const NativeShaderReadAnalysis::Result* vertex,
      const NativeShaderReadAnalysis::Result* pixel);
  static const char* ErrorName(Error error);
};

}  // namespace xe::gpu
#endif  // XENIA_GPU_NATIVE_DRAW_READ_INVENTORY_H_
