/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_SHADER_READ_ANALYSIS_H_
#define XENIA_GPU_NATIVE_SHADER_READ_ANALYSIS_H_

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "xenia/gpu/xenos.h"

namespace xe::gpu {

// CPU-only diagnostic metadata. This deliberately supports a bounded,
// straight-line vertex subset and an acyclic forward-branch pixel subset
// before entering Shader::AnalyzeUcode, whose legacy interface is not a
// bytecode validator. All lexical reads remain conservative potential reads,
// including unreachable blocks. No guest memory/backend is supplied.
// Success inventories potential vertex/texture reads, never execution
// permission, index/access bounds, ownership, translation validity or complete
// draw closure.
class NativeShaderReadAnalysis {
 public:
  enum class Error {
    kNone,
    kInvalidLimits,
    kInvalidByteCount,
    kLimitExceeded,
    kInvalidStage,
    kShaderDumpEnabled,
    kUnsupportedControlFlow,
    kInvalidExec,
    kMissingEnd,
    kInvalidFetch,
    kUnsupportedTexture,
    kUnsupportedMemexport,
    kInvalidAlu,
    kAnalysisMismatch,
  };
  struct Limits {
    uint32_t max_dwords = 4096;
    uint32_t max_exec_instructions = 4096;
    uint32_t max_vertex_fetches = 512;
    uint32_t max_texture_fetches = 512;
  };
  struct VertexFetch {
    uint32_t instruction_dword_offset;
    uint32_t slot;
    uint32_t stride_words;
    int32_t offset_words;
    uint32_t format;
    uint32_t needed_word_mask;
    uint32_t index_register;
    uint32_t index_component;
    bool index_relative;
    bool index_rounded;
    bool mini;
  };
  struct TextureFetch {
    uint32_t instruction_dword_offset;
    uint32_t slot;
    uint32_t dimension;
    bool normalized_coordinates;
    uint32_t coordinate_register;
    uint32_t coordinate_swizzle;  // Three absolute 2-bit components.
    bool coordinate_relative;
    uint32_t dest_register;
    uint32_t dest_swizzle;
    bool dest_relative;
    bool fetch_valid_only;
    uint32_t mag_filter, min_filter, mip_filter, aniso_filter;
    uint32_t arbitrary_filter, vol_mag_filter, vol_min_filter;
    uint32_t sample_location;
    bool use_computed_lod, use_register_lod, use_register_gradients;
    float lod_bias, offset_x, offset_y, offset_z;
    std::array<uint32_t, 3> instruction_words;
  };
  struct ConstantUse {
    std::array<uint64_t, 4> float_bitmap{};
    uint32_t float_count = 0;
    bool float_dynamic_addressing = false;
    std::array<uint32_t, 8> bool_bitmap{};
    uint32_t loop_bitmap = 0;
  };
  struct Result {
    Error error = Error::kNone;
    uint32_t error_dword_offset = 0;
    bool metadata_analyzed = false;
    bool execution_ready = false;
    bool access_bounds_verified = false;
    // Meaningful with metadata_analyzed, not an independently trusted stage.
    xenos::ShaderType stage = xenos::ShaderType::kVertex;
    std::array<uint32_t, 3> vertex_fetch_bitmap{};
    std::vector<VertexFetch> vertex_fetches;
    uint32_t texture_fetch_bitmap = 0;
    std::vector<TextureFetch> texture_fetches;
    ConstantUse constant_use{};
    bool writes_depth = false;
    bool kills_pixels = false;
    explicit operator bool() const { return metadata_analyzed; }
  };

  static Result Analyze(xenos::ShaderType stage,
                        std::span<const uint8_t> big_endian_ucode);
  static Result Analyze(xenos::ShaderType stage,
                        std::span<const uint8_t> big_endian_ucode,
                        const Limits& limits);
  static const char* ErrorName(Error error);
};

}  // namespace xe::gpu
#endif  // XENIA_GPU_NATIVE_SHADER_READ_ANALYSIS_H_
