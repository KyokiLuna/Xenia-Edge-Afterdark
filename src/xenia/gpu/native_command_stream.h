/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_NATIVE_COMMAND_STREAM_H_
#define XENIA_GPU_NATIVE_COMMAND_STREAM_H_

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace xe::gpu {

// Bounded front-end for an unpublished, untextured native draw recording.
// Acceptance means only that this supported PM4 profile is well formed and
// its declared read dependencies have been inventoried. It is NEVER permission
// to execute: shader semantics/actual used bindings, owned memory access and
// lifetime, isolated GPU context, render targets and backend draw success must
// still be validated. No guest memory is read and no GPU work is submitted.
class NativeCommandStream {
 public:
  enum class Error {
    kNone,
    kInvalidLimits,
    kInvalidByteCount,
    kLimitExceeded,
    kTruncatedPacket,
    kUnsupportedPacket,
    kUnsupportedRegister,
    kUnsupportedWait,
    kInvalidShader,
    kInvalidDraw,
    kMissingLocalState,
    kUnsupportedTexture,
    kInvalidMemoryRange,
    kNoDraw,
  };
  enum class PacketKind {
    kRegisterWrite,
    kNop,
    kCoherencyWait,
    kShaderLoad,
    kInlineShader,
    kShaderInvalidation,
    kDraw,
  };
  enum class ShaderStage { kVertex, kPixel };
  // Callers may tighten these hard profile bounds, not raise them.
  struct Limits {
    uint32_t max_dwords = 0x10000;
    uint32_t max_packets = 4096;
    uint32_t max_draws = 256;
    uint32_t max_dependencies = 512;
    uint32_t max_shader_dwords = 4096;
    uint32_t max_vertex_bytes = 0x1000000;
  };
  struct Packet {
    PacketKind kind;
    uint32_t dword_offset;
    uint32_t dword_count;
  };
  struct RegisterWrite {
    uint32_t packet_dword_offset;
    uint32_t index;
    uint32_t value;
  };
  struct MemoryDependency {
    enum class Kind { kShaderCode, kDeclaredVertexBuffer };
    Kind kind;
    uint32_t packet_dword_offset;
    // Preserve the GPU operand's address form alongside the physical extent
    // used by the existing PM4/vertex paths. This is not virtual-heap alias
    // translation, and does not prove committed memory or ownership.
    uint32_t raw_address;
    uint32_t physical_address;
    uint32_t byte_count;
    // ShaderLoad index for code, vertex fetch index [0,95] for vertex data.
    uint32_t binding_index;
  };
  struct ShaderLoad {
    ShaderStage stage;
    uint32_t packet_dword_offset;
    uint32_t dword_count;
    // Inline words belong to words(); an external shader instead has a
    // MemoryDependency. Neither form has had its instructions analyzed.
    bool is_inline;
    uint32_t inline_dword_offset;
  };
  struct CoherencyWait {
    uint32_t packet_dword_offset;
    uint32_t status;
    uint32_t base;
    uint32_t size;
    // This is a cache/coherency action, not a memory read dependency. The
    // current CP may await earlier memexport work before clearing status;
    // future execution must supply a checked, bounded coherency boundary.
  };
  struct Draw {
    uint32_t packet_dword_offset;
    uint32_t initiator;
    uint32_t vertex_count;
    uint32_t vertex_shader;
    uint32_t pixel_shader;
    // Draw-time snapshot, including disabled/zero-size slots. Dependencies
    // conservatively include each nonempty declared vertex span, not proof
    // that every span is used or that these are all shader memory accesses.
    std::array<uint32_t, 192> fetch_constants;
  };
  struct Result {
    Error error;
    uint32_t error_dword_offset;
    std::shared_ptr<const NativeCommandStream> stream;
    // Rejection still permits diagnostic copies of independently validated
    // external shader reads in the prefix. This is an incomplete inventory;
    // the rejected stream remains null and must never authorize execution.
    std::vector<MemoryDependency> prefix_shader_reads{};
    uint32_t missing_state_register = UINT32_MAX;
    explicit operator bool() const { return error == Error::kNone; }
  };

  static Result Analyze(std::span<const uint8_t> big_endian_bytes);
  static Result Analyze(std::span<const uint8_t> big_endian_bytes,
                        const Limits& limits);

  // All storage is owned; no input span or guest pointer is retained. Words
  // are logical host-endian PM4 DWORDs, not an executable physical buffer.
  const std::vector<uint32_t>& words() const { return words_; }
  const std::vector<Packet>& packets() const { return packets_; }
  const std::vector<RegisterWrite>& register_writes() const {
    return register_writes_;
  }
  const std::vector<MemoryDependency>& dependencies() const {
    return dependencies_;
  }
  const std::vector<ShaderLoad>& shaders() const { return shaders_; }
  const std::vector<CoherencyWait>& coherency_waits() const {
    return coherency_waits_;
  }
  const std::vector<Draw>& draws() const { return draws_; }

 private:
  NativeCommandStream() = default;
  std::vector<uint32_t> words_;
  std::vector<Packet> packets_;
  std::vector<RegisterWrite> register_writes_;
  std::vector<MemoryDependency> dependencies_;
  std::vector<ShaderLoad> shaders_;
  std::vector<CoherencyWait> coherency_waits_;
  std::vector<Draw> draws_;
};

}  // namespace xe::gpu

#endif  // XENIA_GPU_NATIVE_COMMAND_STREAM_H_
