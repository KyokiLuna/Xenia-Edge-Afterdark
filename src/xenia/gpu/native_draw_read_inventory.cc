/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_draw_read_inventory.h"

#include <bit>
#include <cstring>
#include <utility>

#include "xenia/gpu/native_shader_diagnostic_scan.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"

namespace xe::gpu {
namespace {
constexpr uint32_t kFetchFirst = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
constexpr uint32_t kMaxShaders = 512;
constexpr uint32_t kMaxDraws = 256;
constexpr uint32_t kMaxVertexBytes = 4 * 1024 * 1024;
constexpr uint32_t kMaxTextureBytes = 4 * 1024 * 1024;

NativeDrawReadInventory::Error Geometry(
    const NativeDrawReadInventory::Draw& draw) {
  using Error = NativeDrawReadInventory::Error;
  if (draw.error != Error::kNone) {
    return draw.error;
  }
  if (!draw.state_known[0] || !draw.state_known[1] || !draw.state_known[2]) {
    return Error::kMissingState;
  }
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = draw.initiator;
  const uint32_t expected_low =
      uint32_t(initiator.prim_type) |
      (uint32_t(xenos::SourceSelect::kAutoIndex) << 6);
  if ((draw.initiator & 0xFFFF) != expected_low || !initiator.num_indices ||
      draw.viz_token || draw.state_words[1] || draw.state_words[2]) {
    return Error::kUnsupportedGeometry;
  }
  switch (xenos::EdramMode(draw.state_words[0])) {
    case xenos::EdramMode::kColorDepth:
      if (initiator.prim_type == xenos::PrimitiveType::kTriangleList) {
        return initiator.num_indices % 3 ? Error::kUnsupportedGeometry
                                         : Error::kNone;
      }
      if (initiator.prim_type == xenos::PrimitiveType::kRectangleList &&
          !(initiator.num_indices % 3)) {
        // Each rectangle consumes three guest vertices. The backend either
        // derives the fourth corner in a GS or runs the same three guest IDs
        // for each host strip vertex. Neither path adds a guest input index.
        // The separate shader, resource and VS bounds proofs still apply.
        return Error::kNone;
      }
      if ((initiator.prim_type == xenos::PrimitiveType::kTriangleFan ||
           initiator.prim_type == xenos::PrimitiveType::kTriangleStrip) &&
          initiator.num_indices >= 3) {
        // Direct strips/fans or host remapping only reuse guest IDs0..N-1.
        // The AutoIndex/EOP/VIZ checks above and separate VS bounds proof
        // remain mandatory; no external index memory is introduced.
        return Error::kNone;
      }
      if (initiator.prim_type == xenos::PrimitiveType::kQuadList &&
          !(initiator.num_indices % 4)) {
        // The backend either consumes each four guest vertices in a GS, or
        // converts (a,b,c,d) to (a,b,c),(a,c,d). Neither adds a guest input
        // index. This is inventory geometry only, not backend admission.
        return Error::kNone;
      }
      return Error::kUnsupportedGeometry;
    case xenos::EdramMode::kDepthOnly:
    case xenos::EdramMode::kCopy:
      return initiator.prim_type == xenos::PrimitiveType::kRectangleList &&
                     initiator.num_indices == 3
                 ? Error::kNone
                 : Error::kUnsupportedGeometry;
    default:
      return Error::kUnsupportedGeometry;
  }
}
}  // namespace

NativeDrawReadInventory::ScanResult NativeDrawReadInventory::Scan(
    std::span<const uint8_t> bytes) {
  return Scan(bytes, {});
}
NativeDrawReadInventory::ScanResult NativeDrawReadInventory::Scan(
    std::span<const uint8_t> bytes,
    std::shared_ptr<const NativeSystemTitleInput> title_input) {
  ScanResult result;
  const auto fail = [&](Error error, uint32_t offset) {
    result.error = error;
    result.error_dword_offset = offset;
    result.shaders.clear();
    result.draws.clear();
    return result;
  };
  // Shared framing / IM_LOAD operand checks cover the complete primary, even
  // when the strict execution-profile front end rejects its first draw.
  const auto framing = NativeShaderDiagnosticScan::Scan(bytes);
  if (!framing.complete_primary_scan ||
      framing.error != NativeShaderDiagnosticScan::Error::kNone) {
    return fail(Error::kInvalidPrimary, framing.error_dword_offset);
  }
  const auto word = [&](uint32_t index) {
    const size_t offset = size_t(index) * 4;
    return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
           uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
  };
  std::array<uint32_t, 192> fetch{};
  std::bitset<192> fetch_known;
  if (title_input) {
    if (!title_input->valid()) {
      return fail(Error::kInvalidPrimary, 0);
    }
    for (uint32_t i = 0; i < 6; ++i) {
      fetch[15 * 6 + i] = title_input->fetch()[i];
      fetch_known.set(15 * 6 + i);
    }
  }
  std::array<uint32_t, kStateRegisters.size()> state{};
  std::bitset<kStateRegisters.size()> state_known;
  ConstantState constants;
  std::optional<uint32_t> vs, ps;
  const auto invalidate = [&] {
    fetch_known.reset();
    state_known.reset();
    constants.float_known.reset();
    constants.bool_known.reset();
    constants.loop_known.reset();
    constants.selectors_known.reset();
    vs.reset();
    ps.reset();
    ++result.state_invalidations;
  };
  const auto write = [&](uint32_t index, uint32_t value) {
    if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
        index - XE_GPU_REG_SHADER_CONSTANT_000_X <
            constants.float_words.size()) {
      const uint32_t at = index - XE_GPU_REG_SHADER_CONSTANT_000_X;
      constants.float_words[at] = value;
      constants.float_known.set(at);
    }
    if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
        index - XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 <
            constants.bool_words.size()) {
      const uint32_t at = index - XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
      constants.bool_words[at] = value;
      constants.bool_known.set(at);
    }
    if (index >= XE_GPU_REG_SHADER_CONSTANT_LOOP_00 &&
        index - XE_GPU_REG_SHADER_CONSTANT_LOOP_00 <
            constants.loop_words.size()) {
      const uint32_t at = index - XE_GPU_REG_SHADER_CONSTANT_LOOP_00;
      constants.loop_words[at] = value;
      constants.loop_known.set(at);
    }
    if (index == XE_GPU_REG_SQ_VS_CONST || index == XE_GPU_REG_SQ_PS_CONST) {
      const uint32_t at = index == XE_GPU_REG_SQ_VS_CONST ? 0 : 1;
      constants.selectors[at] = value;
      constants.selectors_known.set(at);
    }
    if (index >= kFetchFirst && index - kFetchFirst < fetch.size()) {
      fetch[index - kFetchFirst] = value;
      fetch_known.set(index - kFetchFirst);
    }
    for (size_t i = 0; i < kStateRegisters.size(); ++i) {
      if (index == kStateRegisters[i]) {
        state[i] = value;
        state_known.set(i);
      }
    }
  };
  for (uint32_t offset = 0; offset < bytes.size() / 4;) {
    const uint32_t header = word(offset);
    const uint32_t type = header >> 30;
    const uint32_t size = type == 2   ? 1
                          : type == 1 ? 3
                                      : ((header >> 16) & 0x3FFF) + 2;
    const uint32_t count = size - 1;
    if (type == 0) {
      // Reject register ranges outside the common register file even though
      // this inventory retains only fetch/mode/VGT words.
      const uint32_t first = header & 0x7FFF;
      if (first >= RegisterFile::kRegisterCount ||
          (!(header & 0x8000) &&
           count > RegisterFile::kRegisterCount - first)) {
        return fail(Error::kInvalidRegisterRange, offset);
      }
      for (uint32_t i = 0; i < count; ++i) {
        write(first + ((header & 0x8000) ? 0 : i), word(offset + 1 + i));
      }
    } else if (type == 1) {
      // Type1 has two explicit register/value pairs, not nested packets.
      write(header & 0x7FF, word(offset + 1));
      write((header >> 11) & 0x7FF, word(offset + 2));
    } else if (type == 3) {
      const uint32_t opcode = (header >> 8) & 0x7F;
      if (header & 0xFF) {
        // Conditional/control flags cannot be modeled by a linear inventory.
        invalidate();
      } else if (opcode == xenos::PM4_IM_LOAD ||
                 opcode == xenos::PM4_IM_LOAD_IMMEDIATE) {
        if (result.shaders.size() >= kMaxShaders) {
          return fail(Error::kLimitExceeded, offset);
        }
        const bool in = opcode == xenos::PM4_IM_LOAD_IMMEDIATE;
        const uint32_t raw = in ? 0 : word(offset + 1) & ~3u;
        ShaderLoad load{xenos::ShaderType(word(offset + 1) & 3),
                        offset,
                        word(offset + 2) & 0xFFFF,
                        in,
                        in ? offset + 3 : 0,
                        raw,
                        xenos::CpuToGpu(raw)};
        const uint32_t id = uint32_t(result.shaders.size());
        result.shaders.push_back(load);
        (load.stage == xenos::ShaderType::kVertex ? vs : ps) = id;
      } else if (opcode == xenos::PM4_DRAW_INDX ||
                 opcode == xenos::PM4_DRAW_INDX_2) {
        if (result.draws.size() >= kMaxDraws) {
          return fail(Error::kLimitExceeded, offset);
        }
        const bool token = opcode == xenos::PM4_DRAW_INDX;
        Draw draw{offset, opcode, 0,           0,     vs,
                  ps,     fetch,  fetch_known, state, state_known};
        draw.constants = constants;
        draw.title_input = title_input;
        if (count != (token ? 2u : 1u)) {
          draw.error = Error::kUnsupportedGeometry;
        } else {
          draw.initiator = word(offset + (token ? 2 : 1));
          draw.viz_token = token ? word(offset + 1) : 0;
          draw.error = Geometry(draw);
        }
        result.draws.push_back(std::move(draw));
      } else if (opcode == xenos::PM4_INVALIDATE_STATE) {
        constexpr uint32_t mask = xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER |
                                  xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER;
        if (count != 1 || !word(offset + 1) || (word(offset + 1) & ~mask)) {
          invalidate();
        } else {
          if (word(offset + 1) & xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER) {
            vs.reset();
          }
          if (word(offset + 1) & xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER) {
            ps.reset();
          }
        }
      } else if (opcode == xenos::PM4_WAIT_REG_MEM) {
        // Only the known register-coherency poll; never follow a memory wait.
        if (count != 5 || word(offset + 1) != 3 ||
            word(offset + 2) != XE_GPU_REG_COHER_STATUS_HOST ||
            word(offset + 3) != 0 || word(offset + 4) != 0x80000000 ||
            word(offset + 5) >= 0x100) {
          invalidate();
        }
      } else if (opcode != xenos::PM4_NOP) {
        // No interpretation of an IB, arbitrary register loader, event,
        // memory-write packet or unknown state source. Later writes/loads may
        // re-establish known state, but nothing is inherited through this gap.
        invalidate();
      }
    }
    offset += size;
  }
  result.complete_primary_scan = true;
  return result;
}

NativeDrawReadInventory::TextureReadResult
NativeDrawReadInventory::TextureReads(
    const Draw& draw, const NativeShaderReadAnalysis::Result* pixel) {
  TextureReadResult result;
  const auto fail = [&](Error error, uint32_t slot = UINT32_MAX) {
    result.error = error;
    result.error_slot = slot;
    result.reads.clear();
    return result;
  };
  const auto geometry = Geometry(draw);
  if (geometry != Error::kNone) {
    return fail(geometry);
  }
  if (draw.state_words[0] != uint32_t(xenos::EdramMode::kColorDepth)) {
    // Copy reads fetch0 via GetResolveInfo, while DepthOnly ignores its PS.
    return result;
  }
  if (!draw.pixel_shader || !pixel || !*pixel ||
      pixel->stage != xenos::ShaderType::kPixel) {
    return fail(Error::kUnsupportedShader);
  }
  uint32_t declared_slots = 0;
  if (pixel->texture_fetches.size() > 512) {
    return fail(Error::kLimitExceeded);
  }
  for (const auto& instruction : pixel->texture_fetches) {
    if (instruction.slot >= 32) {
      return fail(Error::kInvalidTextureFetch, instruction.slot);
    }
    declared_slots |= uint32_t(1) << instruction.slot;
  }
  if (declared_slots != pixel->texture_fetch_bitmap) {
    return fail(Error::kInvalidTextureFetch);
  }
  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (!(declared_slots & (uint32_t(1) << slot))) {
      continue;
    }
    std::array<uint32_t, 6> words;
    for (uint32_t i = 0; i < words.size(); ++i) {
      if (!draw.fetch_known[slot * 6 + i]) {
        return fail(Error::kMissingState, slot);
      }
      words[i] = draw.fetch_words[slot * 6 + i];
    }
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture) {
      return fail(Error::kInvalidTextureFetch, slot);
    }
    const bool title = slot == 15 && draw.title_input &&
                       draw.title_input->valid() &&
                       words == draw.title_input->fetch();
    const auto base_format = GetBaseFormat(fetch.format);
    if (!title &&
        (fetch.dimension != xenos::DataDimension::k2DOrStacked ||
         fetch.stacked ||
         (base_format != xenos::TextureFormat::k_8 &&
          base_format != xenos::TextureFormat::k_8_8_8_8) ||
         fetch.num_format || fetch.sign_x != xenos::TextureSign::kUnsigned ||
         fetch.sign_y != xenos::TextureSign::kUnsigned ||
         fetch.sign_z != xenos::TextureSign::kUnsigned ||
         fetch.sign_w != xenos::TextureSign::kUnsigned || fetch.exp_adjust ||
         fetch.border_size || fetch.clamp_x != xenos::ClampMode::kClampToEdge ||
         fetch.clamp_y != xenos::ClampMode::kClampToEdge ||
         fetch.arbitrary_filter != xenos::ArbitraryFilter::k2x4Sym ||
         fetch.aniso_filter != xenos::AnisoFilter::kDisabled ||
         uint32_t(fetch.mag_filter) > uint32_t(xenos::TextureFilter::kLinear) ||
         uint32_t(fetch.min_filter) > uint32_t(xenos::TextureFilter::kLinear) ||
         uint32_t(fetch.mip_filter) >
             uint32_t(xenos::TextureFilter::kBaseMap))) {
      return fail(Error::kUnsupportedTexture, slot);
    }
    TextureRead read{};
    read.slot = slot;
    read.from_title_input = title;
    read.fetch_words = words;
    for (const auto& instruction : pixel->texture_fetches) {
      if (instruction.slot != slot) {
        continue;
      }
      const auto effective_filter = [&](uint32_t shader, uint32_t descriptor) {
        return shader == uint32_t(xenos::TextureFilter::kUseFetchConst)
                   ? descriptor
                   : shader;
      };
      const uint32_t mag =
          effective_filter(instruction.mag_filter, uint32_t(fetch.mag_filter));
      const uint32_t min =
          effective_filter(instruction.min_filter, uint32_t(fetch.min_filter));
      const uint32_t mip =
          effective_filter(instruction.mip_filter, uint32_t(fetch.mip_filter));
      const uint32_t aniso =
          instruction.aniso_filter ==
                  uint32_t(xenos::AnisoFilter::kUseFetchConst)
              ? uint32_t(fetch.aniso_filter)
              : instruction.aniso_filter;
      if (instruction.dimension != uint32_t(xenos::FetchOpDimension::k2D) ||
          !instruction.normalized_coordinates ||
          instruction.coordinate_relative || instruction.dest_relative ||
          !instruction.fetch_valid_only || !instruction.use_computed_lod ||
          instruction.use_register_lod || instruction.use_register_gradients ||
          instruction.arbitrary_filter || instruction.sample_location ||
          instruction.lod_bias != 0.0f || instruction.offset_x != 0.0f ||
          instruction.offset_y != 0.0f || instruction.offset_z != 0.0f ||
          mag != uint32_t(xenos::TextureFilter::kLinear) ||
          min != uint32_t(xenos::TextureFilter::kLinear) ||
          (!title && (mip > uint32_t(xenos::TextureFilter::kBaseMap) ||
                      aniso != uint32_t(xenos::AnisoFilter::kDisabled)))) {
        return fail(Error::kUnsupportedTexture, slot);
      }
      read.instructions.push_back(instruction);
    }
    if (title) {
      const auto& layout = draw.title_input->layout();
      read.raw_address = layout.range.address;
      read.physical_address = layout.range.address;
      read.byte_count = layout.range.length;
      read.logical_texel_bytes = uint64_t(layout.width) * layout.height * 4;
      read.row_pitch_bytes = layout.row_pitch_bytes;
      read.padded_span_bytes = layout.range.length;
      read.storage_stride_bytes = layout.range.length;
      read.width = layout.width;
      read.height = layout.height;
      read.format = uint32_t(fetch.format);
      read.endian = uint32_t(fetch.endianness);
      result.reads.push_back(std::move(read));
      continue;
    }
    uint32_t width_minus_1, height_minus_1, depth_minus_1;
    uint32_t base_page, mip_page, mip_min, mip_max;
    texture_util::GetSubresourcesFromFetchConstant(
        fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
        &mip_page, &mip_min, &mip_max);
    if (!base_page) {
      return fail(Error::kInvalidTextureFetch, slot);
    }
    // Match the cache's effective resource set, not ignored raw mip/LOD bits.
    // No ordinary arrays or effective extra mip resources are admitted. The
    // common helper below covers linear/tiled storage and packed base tails.
    if (depth_minus_1 || mip_page || mip_min || mip_max) {
      return fail(Error::kUnsupportedTexture, slot);
    }
    read.width = width_minus_1 + 1;
    read.height = height_minus_1 + 1;
    const uint32_t bytes_per_texel =
        base_format == xenos::TextureFormat::k_8 ? 1 : 4;
    const uint32_t pitch_texels = uint32_t(fetch.pitch) << 5;
    const uint64_t pitch_bytes = uint64_t(pitch_texels) * bytes_per_texel;
    if (!pitch_texels || pitch_texels < read.width ||
        (!fetch.tiled &&
         pitch_bytes % xenos::kTextureLinearRowAlignmentBytes)) {
      return fail(Error::kInvalidTextureLayout, slot);
    }
    const auto layout = texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, read.width, read.height, 1, fetch.tiled,
        base_format, fetch.packed_mips, true, 0);
    const uint64_t padded_rows =
        uint64_t(layout.base.row_pitch_bytes) * read.height;
    const uint32_t data_extent = layout.base.level_data_extent_bytes;
    if (layout.max_level || layout.array_size != 1 ||
        layout.mips_total_extent_bytes || !data_extent ||
        layout.base.row_pitch_bytes != pitch_bytes ||
        layout.base.array_slice_data_extent_bytes != data_extent ||
        padded_rows > UINT32_MAX) {
      return fail(Error::kInvalidTextureLayout, slot);
    }
    read.raw_address = uint32_t(fetch.base_address) << 12;
    read.physical_address = base_page << 12;
    // Both RequestGuestRanges and the Vulkan texture loader consume 16-byte
    // rounded storage. Capture/pin the same complete footprint, including a
    // partial final vector; logical texels alone are not read coverage.
    const uint64_t load_bytes = (uint64_t(data_extent) + 15) & ~UINT64_C(15);
    if (load_bytes > kMaxTextureBytes) {
      return fail(Error::kLimitExceeded, slot);
    }
    read.byte_count = uint32_t(load_bytes);
    if (read.byte_count > 0x20000000u - read.physical_address) {
      return fail(Error::kInvalidMemoryRange, slot);
    }
    read.logical_texel_bytes =
        uint64_t(read.width) * read.height * bytes_per_texel;
    read.row_pitch_bytes = layout.base.row_pitch_bytes;
    read.padded_span_bytes = uint32_t(padded_rows);
    read.storage_stride_bytes = layout.base.array_slice_stride_bytes;
    read.format = uint32_t(fetch.format);
    read.endian = uint32_t(fetch.endianness);
    result.reads.push_back(std::move(read));
  }
  return result;
}

NativeDrawReadInventory::ReadResult NativeDrawReadInventory::VertexReads(
    const Draw& draw, const NativeShaderReadAnalysis::Result* vertex) {
  ReadResult result;
  const auto fail = [&](Error error, uint32_t slot = UINT32_MAX) {
    result.error = error;
    result.error_slot = slot;
    result.reads.clear();
    return result;
  };
  const auto geometry = Geometry(draw);
  if (geometry != Error::kNone) {
    return fail(geometry);
  }
  const bool copy = draw.state_words[0] == uint32_t(xenos::EdramMode::kCopy);
  if (!copy && (!draw.vertex_shader || !vertex || !*vertex ||
                vertex->stage != xenos::ShaderType::kVertex)) {
    return fail(Error::kUnsupportedShader);
  }
  for (uint32_t slot = 0; slot < 96; ++slot) {
    const bool used = copy ? slot == 0
                           : (vertex->vertex_fetch_bitmap[slot / 32] &
                              (uint32_t(1) << (slot % 32))) != 0;
    if (!used) {
      continue;
    }
    if (!draw.fetch_known[slot * 2] || !draw.fetch_known[slot * 2 + 1]) {
      return fail(Error::kMissingState, slot);
    }
    const uint32_t first = draw.fetch_words[slot * 2];
    const uint32_t second = draw.fetch_words[slot * 2 + 1];
    const uint32_t size = ((second >> 2) & 0xFFFFFF) * 4;
    if ((first & 3) != uint32_t(xenos::FetchConstantType::kVertex) || !size ||
        size > kMaxVertexBytes || (copy && size != 24)) {
      return fail(Error::kInvalidVertexFetch, slot);
    }
    const uint32_t raw = first & ~3u;
    const uint32_t physical = xenos::CpuToGpu(raw);
    if (size > 0x20000000u - physical) {
      return fail(Error::kInvalidMemoryRange, slot);
    }
    result.reads.push_back({slot, raw, physical, size, second & 3, copy});
  }
  return result;
}

NativeDrawReadInventory::ConstantReadResult
NativeDrawReadInventory::ConstantReads(
    const Draw& draw, const NativeShaderReadAnalysis::Result* vertex,
    const NativeShaderReadAnalysis::Result* pixel) {
  ConstantReadResult result;
  const auto record_error = [&](Error error, uint32_t reg,
                                xenos::ShaderType stage, uint32_t index) {
    if (result.error == Error::kNone) {
      result.error = error;
      result.error_register = reg;
      result.error_stage = stage;
      result.error_constant_index = index;
    }
  };
  result.error = Geometry(draw);
  if (result.error != Error::kNone) {
    return result;
  }
  const auto mode = xenos::EdramMode(draw.state_words[0]);
  if (mode == xenos::EdramMode::kCopy) {
    result.used_constant_values_known = true;
    return result;
  }
  const std::array<const NativeShaderReadAnalysis::Result*, 2> stages = {
      vertex, mode == xenos::EdramMode::kDepthOnly ? nullptr : pixel};
  const uint32_t stage_count = mode == xenos::EdramMode::kDepthOnly ? 1 : 2;
  // Establish each stage's source and fixed bank before reading any constant.
  // A selector mismatch may not be repaired with the legacy title defaults.
  for (uint32_t stage_index = 0; stage_index < stage_count; ++stage_index) {
    const auto stage = stage_index == 0 ? xenos::ShaderType::kVertex
                                        : xenos::ShaderType::kPixel;
    const auto* shader = stages[stage_index];
    const uint32_t selector_reg =
        stage_index == 0 ? XE_GPU_REG_SQ_VS_CONST : XE_GPU_REG_SQ_PS_CONST;
    if (!shader || !*shader || shader->stage != stage ||
        !(stage_index == 0 ? draw.vertex_shader : draw.pixel_shader)) {
      record_error(Error::kUnsupportedShader, UINT32_MAX, stage, UINT32_MAX);
      return result;
    }
    const auto& use = shader->constant_use;
    if (use.float_dynamic_addressing) {
      record_error(Error::kUnsupportedConstantAddressing, UINT32_MAX, stage,
                   UINT32_MAX);
      return result;
    }
    uint32_t count = 0;
    for (uint64_t bits : use.float_bitmap) {
      count += std::popcount(bits);
    }
    if (count != use.float_count) {
      record_error(Error::kInvalidConstantMetadata, UINT32_MAX, stage,
                   UINT32_MAX);
      return result;
    }
    if (!draw.constants.selectors_known[stage_index]) {
      record_error(Error::kMissingState, selector_reg, stage, UINT32_MAX);
      return result;
    }
    const uint32_t expected = stage_index == 0 ? 0x000FF000u : 0x000FF100u;
    if (draw.constants.selectors[stage_index] != expected) {
      record_error(Error::kUnsupportedConstantSelector, selector_reg, stage,
                   UINT32_MAX);
      return result;
    }
  }
  for (uint32_t stage_index = 0; stage_index < stage_count; ++stage_index) {
    const auto stage = stage_index == 0 ? xenos::ShaderType::kVertex
                                        : xenos::ShaderType::kPixel;
    const auto& use = stages[stage_index]->constant_use;
    const uint32_t float_base = stage_index * 1024;
    for (uint32_t index = 0; index < 256; ++index) {
      if (!(use.float_bitmap[index / 64] & (UINT64_C(1) << (index % 64)))) {
        continue;
      }
      FloatValue value{stage, index};
      for (uint32_t component = 0; component < 4; ++component) {
        const uint32_t at = float_base + index * 4 + component;
        if (draw.constants.float_known[at]) {
          value.known_mask |= 1u << component;
          value.words[component] = draw.constants.float_words[at];
        } else {
          record_error(Error::kMissingState,
                       XE_GPU_REG_SHADER_CONSTANT_000_X + at, stage, index);
        }
      }
      result.floats.push_back(value);
    }
    for (uint32_t word = 0; word < 8; ++word) {
      if (!use.bool_bitmap[word]) {
        continue;
      }
      BoolValue value{stage, word, use.bool_bitmap[word]};
      value.known = draw.constants.bool_known[word];
      if (value.known) {
        value.value = draw.constants.bool_words[word];
      } else {
        record_error(Error::kMissingState,
                     XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + word, stage,
                     word * 32 + std::countr_zero(use.bool_bitmap[word]));
      }
      result.bools.push_back(value);
    }
    for (uint32_t index = 0; index < 32; ++index) {
      if (!(use.loop_bitmap & (uint32_t(1) << index))) {
        continue;
      }
      LoopValue value{stage, index};
      value.known = draw.constants.loop_known[index];
      if (value.known) {
        value.value = draw.constants.loop_words[index];
      } else {
        record_error(Error::kMissingState,
                     XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + index, stage, index);
      }
      result.loops.push_back(value);
    }
  }
  result.used_constant_values_known = result.error == Error::kNone;
  return result;
}

const char* NativeDrawReadInventory::ErrorName(Error error) {
  switch (error) {
#define XE_DRAW_READ_ERROR(name, text) \
  case Error::name:                    \
    return text
    XE_DRAW_READ_ERROR(kNone, "none");
    XE_DRAW_READ_ERROR(kInvalidPrimary, "invalid_primary");
    XE_DRAW_READ_ERROR(kLimitExceeded, "limit_exceeded");
    XE_DRAW_READ_ERROR(kInvalidRegisterRange, "invalid_register_range");
    XE_DRAW_READ_ERROR(kMissingState, "missing_state");
    XE_DRAW_READ_ERROR(kUnsupportedGeometry, "unsupported_geometry");
    XE_DRAW_READ_ERROR(kUnsupportedShader, "unsupported_shader");
    XE_DRAW_READ_ERROR(kInvalidVertexFetch, "invalid_vertex_fetch");
    XE_DRAW_READ_ERROR(kInvalidTextureFetch, "invalid_texture_fetch");
    XE_DRAW_READ_ERROR(kUnsupportedTexture, "unsupported_texture");
    XE_DRAW_READ_ERROR(kInvalidTextureLayout, "invalid_texture_layout");
    XE_DRAW_READ_ERROR(kInvalidMemoryRange, "invalid_memory_range");
    XE_DRAW_READ_ERROR(kInvalidConstantMetadata, "invalid_constant_metadata");
    XE_DRAW_READ_ERROR(kUnsupportedConstantAddressing,
                       "unsupported_constant_addressing");
    XE_DRAW_READ_ERROR(kUnsupportedConstantSelector,
                       "unsupported_constant_selector");
#undef XE_DRAW_READ_ERROR
  }
  return "unknown";
}
}  // namespace xe::gpu
