/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_VERTEX_ACCESS_ANALYSIS_H_
#define XENIA_GPU_NATIVE_VERTEX_ACCESS_ANALYSIS_H_

#include <cstdint>
#include <span>
#include <vector>

#include "xenia/gpu/native_draw_read_inventory.h"

namespace xe::gpu {

// A CPU-only proof of vertex input ranges under an explicit backend contract:
// the supported implicit auto-index draw supplies initial r0.x = integer ID in
// [0, N-1], including host fan/quad expansion that only reuses those IDs.
// This proves neither shader semantics nor the ownership / stability of guest
// memory, and grants no GPU execution permission.
class NativeVertexAccessAnalysis {
 public:
  enum class Error {
    kNone,
    kInvalidDraw,
    kMissingVgtState,
    kUnsupportedVgtState,
    kInvalidShader,
    kMissingVertexFetch,
    kUnsupportedIndexProvenance,
    kUnsupportedFetch,
    kOutOfBounds,
    kInvalidMemoryRange,
    kAnalysisMismatch,
  };
  struct Access {
    uint32_t instruction_dword_offset = UINT32_MAX;
    uint32_t slot = 0;
    uint32_t stride_words = 0;
    int32_t offset_words = 0;
    // Relative to the binding, including any gaps. A range is not a lease.
    uint64_t first_word = 0;
    uint64_t exclusive_end_word = 0;
    uint32_t declared_words = 0;
    uint32_t physical_address = 0;
    bool mini = false;
  };
  struct Result {
    Error error = Error::kNone;
    uint32_t error_dword_offset = UINT32_MAX;
    uint32_t error_slot = UINT32_MAX;
    NativeShaderReadAnalysis::Error shader_error =
        NativeShaderReadAnalysis::Error::kNone;
    NativeDrawReadInventory::Error read_error =
        NativeDrawReadInventory::Error::kNone;
    bool vertex_input_bounds_verified = false;
    bool execution_ready = false;
    bool ownership_verified = false;
    bool uses_auto_index_backend_contract = false;
    bool copy_rectangle = false;
    uint32_t vertex_count = 0;
    std::vector<Access> accesses;
  };

  // The caller owns an immutable copy of the exact selected vertex load's
  // bytes. Stage and metadata are parsed again here, not supplied by a caller.
  // The bytecode has no intrinsic stage tag; matching it to draw.vertex_shader
  // remains the caller's source-provenance obligation. Copy mode instead uses
  // the backend's explicit fetch0 / 24-byte rectangle and needs no VS bytes.
  static Result Analyze(const NativeDrawReadInventory::Draw& draw,
                        std::span<const uint8_t> owned_selected_vertex_ucode);
  static const char* ErrorName(Error error);
};

}  // namespace xe::gpu
#endif  // XENIA_GPU_NATIVE_VERTEX_ACCESS_ANALYSIS_H_
