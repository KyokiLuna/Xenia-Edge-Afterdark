/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/native_shader_diagnostic_scan.h"

#include "xenia/gpu/xenos.h"

namespace xe::gpu {

NativeShaderDiagnosticScan::Result NativeShaderDiagnosticScan::Scan(
    std::span<const uint8_t> bytes) {
  return Scan(bytes, Limits{});
}

NativeShaderDiagnosticScan::Result NativeShaderDiagnosticScan::Scan(
    std::span<const uint8_t> bytes, const Limits& limits) {
  Result result;
  const auto fail = [&](Error error, uint32_t offset) {
    result.error = error;
    result.error_dword_offset = offset;
    result.candidates.clear();
    return result;
  };
  constexpr Limits maximum;
  if (!limits.max_dwords || limits.max_dwords > maximum.max_dwords ||
      !limits.max_packets || limits.max_packets > maximum.max_packets ||
      !limits.max_external_reads ||
      limits.max_external_reads > maximum.max_external_reads ||
      !limits.max_shader_dwords ||
      limits.max_shader_dwords > maximum.max_shader_dwords) {
    return fail(Error::kInvalidLimits, 0);
  }
  if (bytes.empty() || bytes.size() % 4) {
    return fail(Error::kInvalidByteCount, 0);
  }
  const size_t word_count = bytes.size() / 4;
  if (word_count > limits.max_dwords) {
    return fail(Error::kLimitExceeded, 0);
  }
  const auto word = [&](uint32_t index) {
    const size_t offset = size_t(index) * 4;
    return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
           uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
  };
  for (uint32_t offset = 0; offset < word_count;) {
    if (result.packet_count >= limits.max_packets) {
      return fail(Error::kLimitExceeded, offset);
    }
    const uint32_t header = word(offset);
    const uint32_t type = header >> 30;
    const uint32_t packet_size = type == 2   ? 1
                                 : type == 1 ? 3
                                             : ((header >> 16) & 0x3FFF) + 2;
    if (packet_size > word_count - offset) {
      return fail(Error::kTruncatedPacket, offset);
    }
    if (type == 2 && header != 0x80000000) {
      return fail(Error::kInvalidPadding, offset);
    }
    if (type == 3) {
      const uint32_t opcode = (header >> 8) & 0x7F;
      if (opcode == xenos::PM4_IM_LOAD ||
          opcode == xenos::PM4_IM_LOAD_IMMEDIATE) {
        // Match the strict profile's load ABI without inheriting its draw
        // acceptance: no predicated/header-low flags, start offset, unknown
        // stage, zero/oversized shader or extra/missing payload DWORDs.
        const uint32_t count = packet_size - 1;
        if ((header & 0xFF) || count < 2) {
          return fail(Error::kInvalidShaderLoad, offset);
        }
        const bool is_inline = opcode == xenos::PM4_IM_LOAD_IMMEDIATE;
        const uint32_t address_type = word(offset + 1);
        const uint32_t stage = is_inline ? address_type : address_type & 3;
        const uint32_t start_size = word(offset + 2);
        const uint32_t dwords = start_size & 0xFFFF;
        if (stage > 1 || (start_size >> 16) || !dwords ||
            dwords > limits.max_shader_dwords ||
            count != (is_inline ? 2 + dwords : 2)) {
          return fail(Error::kInvalidShaderLoad, offset);
        }
        if (!is_inline) {
          const uint32_t raw_address = address_type & ~3u;
          // This operand is already in the GPU's address form. Preserve it;
          // never apply a CPU virtual-heap alias conversion or a 4 KiB bias.
          const uint32_t physical_address = xenos::CpuToGpu(raw_address);
          const uint32_t byte_count = dwords * 4;
          constexpr uint32_t kPhysicalSize = 0x20000000;
          if (physical_address >= kPhysicalSize ||
              byte_count > kPhysicalSize - physical_address) {
            return fail(Error::kInvalidMemoryRange, offset);
          }
          if (result.candidates.size() >= limits.max_external_reads) {
            return fail(Error::kLimitExceeded, offset);
          }
          result.candidates.push_back({stage ? Stage::kPixel : Stage::kVertex,
                                       offset, result.shader_load_count,
                                       raw_address, physical_address,
                                       byte_count});
        }
        ++result.shader_load_count;
      } else {
        ++result.uninterpreted_type3_packets;
        result.indirect_buffer_packets +=
            opcode == xenos::PM4_INDIRECT_BUFFER ||
            opcode == xenos::PM4_INDIRECT_BUFFER_PFD;
        // Do not inspect payload words as packets, chase IB pointers, run
        // control flow, or claim these declarations execute. Only framing is
        // known for these packets, including otherwise unsupported draws.
      }
    }
    ++result.packet_count;
    offset += packet_size;
  }
  result.complete_primary_scan = true;
  return result;
}

const char* NativeShaderDiagnosticScan::ErrorName(Error error) {
  switch (error) {
    case Error::kNone:
      return "none";
    case Error::kInvalidLimits:
      return "invalid_limits";
    case Error::kInvalidByteCount:
      return "invalid_byte_count";
    case Error::kLimitExceeded:
      return "limit_exceeded";
    case Error::kTruncatedPacket:
      return "truncated_packet";
    case Error::kInvalidPadding:
      return "invalid_padding";
    case Error::kInvalidShaderLoad:
      return "invalid_shader_load";
    case Error::kInvalidMemoryRange:
      return "invalid_memory_range";
  }
  return "unknown";
}

}  // namespace xe::gpu
