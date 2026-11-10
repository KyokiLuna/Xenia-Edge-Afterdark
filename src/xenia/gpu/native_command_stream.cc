/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/native_command_stream.h"

#include <bitset>
#include <optional>
#include <utility>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/xenos.h"

namespace xe::gpu {
namespace {

constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint32_t kFetchFirst = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
constexpr uint32_t kFetchWords = 192;

bool IsStateRegister(uint32_t index) {
  // Current WriteRegister only treats scratch, coherency and gamma writes as
  // host side effects. Keep this profile inside render/constant state and the
  // explicitly checked coherency triplet. Query/copy draw modes are checked
  // separately. New backend register side effects require revisiting this.
  return (index >= 0x2000 && index <= 0x23FF) ||
         (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
          index <= XE_GPU_REG_SHADER_CONSTANT_511_W) ||
         (index >= kFetchFirst && index < kFetchFirst + kFetchWords) ||
         (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
          index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) ||
         (index >= XE_GPU_REG_COHER_SIZE_HOST &&
          index <= XE_GPU_REG_COHER_STATUS_HOST);
}

bool PhysicalExtentFits(uint32_t address, uint32_t size) {
  return size && address < kPhysicalSize && size <= kPhysicalSize - address;
}

}  // namespace

NativeCommandStream::Result NativeCommandStream::Analyze(
    std::span<const uint8_t> bytes) {
  return Analyze(bytes, Limits{});
}

NativeCommandStream::Result NativeCommandStream::Analyze(
    std::span<const uint8_t> bytes, const Limits& limits) {
  std::shared_ptr<NativeCommandStream> stream;
  const auto fail = [&](Error error, uint32_t offset,
                        uint32_t missing_register = UINT32_MAX) -> Result {
    Result result{error, offset, {}};
    result.missing_state_register = missing_register;
    if (stream) {
      for (const auto& read : stream->dependencies_) {
        if (read.kind == MemoryDependency::Kind::kShaderCode) {
          result.prefix_shader_reads.push_back(read);
        }
      }
    }
    return result;
  };
  constexpr Limits maximum;
  if (!limits.max_dwords || limits.max_dwords > maximum.max_dwords ||
      !limits.max_packets || !limits.max_draws || !limits.max_dependencies ||
      limits.max_packets > maximum.max_packets ||
      limits.max_draws > maximum.max_draws ||
      limits.max_dependencies > maximum.max_dependencies ||
      !limits.max_shader_dwords ||
      limits.max_shader_dwords > maximum.max_shader_dwords ||
      !limits.max_vertex_bytes ||
      limits.max_vertex_bytes > maximum.max_vertex_bytes) {
    return fail(Error::kInvalidLimits, 0);
  }
  if (bytes.empty() || bytes.size() % 4) {
    return fail(Error::kInvalidByteCount, 0);
  }
  if (bytes.size() / 4 > limits.max_dwords) {
    return fail(Error::kLimitExceeded, 0);
  }
  stream = std::shared_ptr<NativeCommandStream>(new NativeCommandStream);
  auto& words = stream->words_;
  words.reserve(bytes.size() / 4);
  for (size_t offset = 0; offset < bytes.size(); offset += 4) {
    words.push_back(uint32_t(bytes[offset]) << 24 |
                    uint32_t(bytes[offset + 1]) << 16 |
                    uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3]);
  }
  std::array<uint32_t, RegisterFile::kRegisterCount> registers{};
  std::bitset<RegisterFile::kRegisterCount> known;
  std::optional<uint32_t> vertex_shader;
  std::optional<uint32_t> pixel_shader;
  const auto dependency = [&](MemoryDependency::Kind kind, uint32_t offset,
                              uint32_t raw_address, uint32_t size,
                              uint32_t binding) -> Error {
    const uint32_t physical_address = xenos::CpuToGpu(raw_address);
    if (!PhysicalExtentFits(physical_address, size)) {
      return Error::kInvalidMemoryRange;
    }
    if (stream->dependencies_.size() >= limits.max_dependencies) {
      return Error::kLimitExceeded;
    }
    stream->dependencies_.push_back(
        {kind, offset, raw_address, physical_address, size, binding});
    return Error::kNone;
  };

  for (uint32_t offset = 0; offset < words.size();) {
    if (stream->packets_.size() >= limits.max_packets) {
      return fail(Error::kLimitExceeded, offset);
    }
    const uint32_t header = words[offset];
    const uint32_t type = header >> 30;
    const uint32_t size = type == 2   ? 1
                          : type == 1 ? 3
                                      : ((header >> 16) & 0x3FFF) + 2;
    if (size > words.size() - offset) {
      return fail(Error::kTruncatedPacket, offset);
    }
    auto kind = PacketKind::kNop;
    if (type == 0) {
      // This profile has sequential writes only. A repeated special register
      // write needs its own semantics, rather than accepting the packet size.
      if (header & 0x8000) {
        return fail(Error::kUnsupportedPacket, offset);
      }
      const uint32_t first = header & 0x7FFF;
      for (uint32_t i = 0; i < size - 1; ++i) {
        const uint32_t index = first + i;
        if (index >= RegisterFile::kRegisterCount || !IsStateRegister(index)) {
          return fail(Error::kUnsupportedRegister, offset);
        }
        registers[index] = words[offset + 1 + i];
        known.set(index);
        stream->register_writes_.push_back({offset, index, registers[index]});
      }
      kind = PacketKind::kRegisterWrite;
    } else if (type == 2) {
      if (header != 0x80000000) {
        return fail(Error::kUnsupportedPacket, offset);
      }
    } else if (type != 3 || (header & 0xFF)) {
      // Reject type1, predication and unknown low header flags. The native
      // profile must not inherit a title predicate or a control-flow path.
      return fail(Error::kUnsupportedPacket, offset);
    } else {
      const uint32_t opcode = (header >> 8) & 0x7F;
      const uint32_t count = size - 1;
      switch (opcode) {
        case xenos::PM4_WAIT_REG_MEM: {
          if (count != 5 || words[offset + 1] != 3 ||
              words[offset + 2] != XE_GPU_REG_COHER_STATUS_HOST ||
              words[offset + 3] != 0 || words[offset + 4] != 0x80000000 ||
              words[offset + 5] >= 0x100 ||
              !known[XE_GPU_REG_COHER_STATUS_HOST] ||
              !known[XE_GPU_REG_COHER_SIZE_HOST] ||
              !known[XE_GPU_REG_COHER_BASE_HOST]) {
            return fail(Error::kUnsupportedWait, offset);
          }
          const uint32_t status = registers[XE_GPU_REG_COHER_STATUS_HOST];
          const uint32_t base = registers[XE_GPU_REG_COHER_BASE_HOST];
          const uint32_t range = registers[XE_GPU_REG_COHER_SIZE_HOST];
          if ((status & ~UINT32_C(0x0701FFFF)) || base >= kPhysicalSize ||
              range > kPhysicalSize - base) {
            return fail(Error::kUnsupportedWait, offset);
          }
          stream->coherency_waits_.push_back({offset, status, base, range});
          // MakeCoherent clears the status after the locally declared action.
          // This is analysis of that transition, not CPU acknowledgement of
          // GPU work and not an implementation of the eventual checked wait.
          registers[XE_GPU_REG_COHER_STATUS_HOST] = 0;
          kind = PacketKind::kCoherencyWait;
        } break;
        case xenos::PM4_IM_LOAD:
        case xenos::PM4_IM_LOAD_IMMEDIATE: {
          if (count < 2) {
            return fail(Error::kInvalidShader, offset);
          }
          const bool is_inline = opcode == xenos::PM4_IM_LOAD_IMMEDIATE;
          const uint32_t address_type = words[offset + 1];
          const uint32_t stage = is_inline ? address_type : address_type & 3;
          const uint32_t start_size = words[offset + 2];
          const uint32_t dwords = start_size & 0xFFFF;
          if (stage > 1 || (start_size >> 16) || !dwords ||
              dwords > limits.max_shader_dwords ||
              count != (is_inline ? 2 + dwords : 2)) {
            return fail(Error::kInvalidShader, offset);
          }
          const uint32_t index = uint32_t(stream->shaders_.size());
          if (!is_inline) {
            const auto error =
                dependency(MemoryDependency::Kind::kShaderCode, offset,
                           address_type & ~3u, dwords * 4, index);
            if (error != Error::kNone) {
              return fail(error, offset);
            }
          }
          stream->shaders_.push_back(
              {stage ? ShaderStage::kPixel : ShaderStage::kVertex, offset,
               dwords, is_inline, is_inline ? offset + 3 : 0});
          (stage ? pixel_shader : vertex_shader) = index;
          kind =
              is_inline ? PacketKind::kInlineShader : PacketKind::kShaderLoad;
        } break;
        case xenos::PM4_INVALIDATE_STATE: {
          constexpr uint32_t kShaders =
              xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER |
              xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER;
          if (count != 1 || !words[offset + 1] ||
              (words[offset + 1] & ~kShaders)) {
            return fail(Error::kUnsupportedPacket, offset);
          }
          // The current executor treats this as a no-op. Requiring a local
          // subsequent shader load is deliberately stricter than inheriting
          // previously selected state whose validity was just invalidated.
          if (words[offset + 1] & xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER) {
            vertex_shader.reset();
          }
          if (words[offset + 1] & xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER) {
            pixel_shader.reset();
          }
          kind = PacketKind::kShaderInvalidation;
        } break;
        case xenos::PM4_DRAW_INDX: {
          if (count != 2 || words[offset + 1] != 0) {
            return fail(Error::kInvalidDraw, offset);
          }
          const uint32_t initiator = words[offset + 2];
          const uint32_t vertices = initiator >> 16;
          // Only implicit, end-of-packet, nonindexed triangle lists. No VIZ
          // token, DMA/index memory, immediate indices or tessellation mode.
          const uint32_t expected_low =
              uint32_t(xenos::PrimitiveType::kTriangleList) |
              (uint32_t(xenos::SourceSelect::kAutoIndex) << 6);
          if ((initiator & 0xFFFF) != expected_low || !vertices ||
              vertices % 3) {
            return fail(Error::kInvalidDraw, offset);
          }
          if (!vertex_shader || !pixel_shader) {
            return fail(Error::kMissingLocalState, offset);
          }
          if (!known[XE_GPU_REG_RB_MODECONTROL]) {
            return fail(Error::kMissingLocalState, offset,
                        XE_GPU_REG_RB_MODECONTROL);
          }
          if (!known[XE_GPU_REG_PA_SC_VIZ_QUERY]) {
            return fail(Error::kMissingLocalState, offset,
                        XE_GPU_REG_PA_SC_VIZ_QUERY);
          }
          if (registers[XE_GPU_REG_RB_MODECONTROL] !=
                  uint32_t(xenos::EdramMode::kColorDepth) ||
              registers[XE_GPU_REG_PA_SC_VIZ_QUERY]) {
            return fail(Error::kInvalidDraw, offset);
          }
          if (stream->draws_.size() >= limits.max_draws) {
            return fail(Error::kLimitExceeded, offset);
          }
          Draw draw{offset,         initiator,     vertices,
                    *vertex_shader, *pixel_shader, {}};
          for (uint32_t i = 0; i < kFetchWords; ++i) {
            if (!known[kFetchFirst + i]) {
              return fail(Error::kMissingLocalState, offset, kFetchFirst + i);
            }
            draw.fetch_constants[i] = registers[kFetchFirst + i];
          }
          for (uint32_t i = 0; i < kFetchWords / 2; ++i) {
            const uint32_t word0 = draw.fetch_constants[i * 2];
            const uint32_t word1 = draw.fetch_constants[i * 2 + 1];
            const uint32_t fetch_type = word0 & 3;
            if (fetch_type == uint32_t(xenos::FetchConstantType::kTexture)) {
              return fail(Error::kUnsupportedTexture, offset);
            }
            if (fetch_type != uint32_t(xenos::FetchConstantType::kVertex)) {
              continue;
            }
            const uint32_t size_bytes = ((word1 >> 2) & 0xFFFFFF) * 4;
            if (!size_bytes) {
              continue;
            }
            if (size_bytes > limits.max_vertex_bytes) {
              return fail(Error::kLimitExceeded, offset);
            }
            const auto error =
                dependency(MemoryDependency::Kind::kDeclaredVertexBuffer,
                           offset, word0 & ~3u, size_bytes, i);
            if (error != Error::kNone) {
              return fail(error, offset);
            }
          }
          stream->draws_.push_back(std::move(draw));
          kind = PacketKind::kDraw;
        } break;
        default:
          return fail(Error::kUnsupportedPacket, offset);
      }
    }
    stream->packets_.push_back({kind, offset, size});
    offset += size;
  }
  if (stream->draws_.empty()) {
    return fail(Error::kNoDraw, uint32_t(words.size()));
  }
  return {Error::kNone, 0, std::move(stream)};
}

}  // namespace xe::gpu
