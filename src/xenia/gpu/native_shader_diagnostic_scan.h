/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_NATIVE_SHADER_DIAGNOSTIC_SCAN_H_
#define XENIA_GPU_NATIVE_SHADER_DIAGNOSTIC_SCAN_H_

#include <cstdint>
#include <span>
#include <vector>

namespace xe::gpu {

// A linear diagnostic inventory, independent of NativeCommandStream acceptance.
// Validates complete primary packet framing and the known IM_LOAD operand ABI
// across otherwise uninterpreted packets. It never reads guest memory, follows
// indirect buffers/control flow, validates shaders, or returns an executable
// stream. Even a complete scan is an incomplete read inventory: the candidates
// may be unreachable or conditional, and other packets may declare more reads.
class NativeShaderDiagnosticScan {
 public:
  enum class Error {
    kNone,
    kInvalidLimits,
    kInvalidByteCount,
    kLimitExceeded,
    kTruncatedPacket,
    kInvalidPadding,
    kInvalidShaderLoad,
    kInvalidMemoryRange,
  };
  enum class Stage { kVertex, kPixel };
  // Same hard primary, packet, external-read and shader-size bounds as the
  // strict native profile. Callers can tighten these, never raise them.
  struct Limits {
    uint32_t max_dwords = 0x10000;
    uint32_t max_packets = 4096;
    uint32_t max_external_reads = 512;
    uint32_t max_shader_dwords = 4096;
  };
  struct Candidate {
    Stage stage;
    uint32_t packet_dword_offset;
    // Counts inline and external loads, so a candidate retains its position
    // among the actual primary shader-load declarations.
    uint32_t shader_load_index;
    uint32_t raw_address;
    uint32_t physical_address;
    uint32_t byte_count;
  };
  struct Result {
    Error error = Error::kNone;
    uint32_t error_dword_offset = 0;
    bool complete_primary_scan = false;
    uint32_t packet_count = 0;
    uint32_t shader_load_count = 0;
    uint32_t uninterpreted_type3_packets = 0;
    uint32_t indirect_buffer_packets = 0;
    // Empty on any error, including a malformed tail after otherwise valid
    // loads. No partial scanner result is eligible for capture.
    std::vector<Candidate> candidates;
  };

  static Result Scan(std::span<const uint8_t> big_endian_primary);
  static Result Scan(std::span<const uint8_t> big_endian_primary,
                     const Limits& limits);
  static const char* ErrorName(Error error);
};

}  // namespace xe::gpu

#endif  // XENIA_GPU_NATIVE_SHADER_DIAGNOSTIC_SCAN_H_
