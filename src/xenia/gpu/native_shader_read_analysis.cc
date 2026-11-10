/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_shader_read_analysis.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string_view>

#include "third_party/xxhash/xxhash.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/ucode.h"

namespace xe::gpu {
namespace {
bool ValidFormat(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

bool ValidTextureFilter(xenos::TextureFilter filter) {
  return filter == xenos::TextureFilter::kPoint ||
         filter == xenos::TextureFilter::kLinear ||
         filter == xenos::TextureFilter::kUseFetchConst;
}

struct PixelFlowResult {
  NativeShaderReadAnalysis::Error error =
      NativeShaderReadAnalysis::Error::kNone;
  uint32_t offset = 0;
};

// Only used when a pixel program contains forward jumps. The existing vertex
// profile remains straight-line because its independent auto-index/mini-fetch
// bounds walker relies on that order. No loop/call stack or predicate-dependent
// read pruning is introduced here.
PixelFlowResult ValidateForwardPixelFlow(const std::vector<uint32_t>& words,
                                         uint32_t cf_bound) {
  using Error = NativeShaderReadAnalysis::Error;
  using Op = ucode::ControlFlowOpcode;
  const uint32_t node_count = cf_bound * 2;
  if (!node_count) {
    return {Error::kMissingEnd, 0};
  }
  const auto instruction = [&](uint32_t node) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(words.data() + (node / 2) * 3, pair);
    return pair[node & 1];
  };
  // Validate every lexical reference, even if unreachable: the legacy analyzer
  // inventories the whole CF table. Disjoint payload ranges avoid ambiguous
  // repeated fetch/ALU analysis, and all ranges stay outside the CF table.
  std::vector<bool> payload_used(words.size() / 3);
  for (uint32_t node = 0; node < node_count; ++node) {
    const auto cf = instruction(node);
    if (cf.opcode() == Op::kCondJmp) {
      const uint32_t target = cf.cond_jmp.address();
      if ((cf.dword_0 & ~UINT32_C(0x7FFF)) || (cf.dword_1 & 1) ||
          target <= node || target >= node_count) {
        return {Error::kUnsupportedControlFlow, (node / 2) * 3};
      }
    } else if (cf.opcode() == Op::kExec || cf.opcode() == Op::kExecEnd) {
      if (cf.exec.address() < cf_bound) {
        return {Error::kInvalidExec, (node / 2) * 3};
      }
      for (uint32_t i = 0; i < cf.exec.count(); ++i) {
        const uint32_t at = cf.exec.address() + i;
        if (payload_used[at]) {
          return {Error::kInvalidExec, (node / 2) * 3};
        }
        payload_used[at] = true;
      }
    }
  }
  // Every edge increases the CF index. A reverse pass therefore proves that
  // every reachable branch terminates, without recursion or an iteration cap
  // masquerading as a proof. Unreachable trailing NOPs need not themselves end.
  std::vector<bool> terminates(node_count);
  for (uint32_t node = node_count; node-- > 0;) {
    const auto cf = instruction(node);
    if (cf.opcode() == Op::kExecEnd) {
      terminates[node] = true;
    } else if (cf.opcode() == Op::kCondJmp) {
      const bool target_ends = terminates[cf.cond_jmp.address()];
      terminates[node] =
          target_ends && (cf.cond_jmp.is_unconditional() ||
                          (node + 1 < node_count && terminates[node + 1]));
    } else {
      terminates[node] = node + 1 < node_count && terminates[node + 1];
    }
  }
  return terminates[0] ? PixelFlowResult{}
                       : PixelFlowResult{Error::kMissingEnd, cf_bound * 3};
}

}  // namespace

NativeShaderReadAnalysis::Result NativeShaderReadAnalysis::Analyze(
    xenos::ShaderType stage, std::span<const uint8_t> bytes) {
  return Analyze(stage, bytes, Limits{});
}

NativeShaderReadAnalysis::Result NativeShaderReadAnalysis::Analyze(
    xenos::ShaderType stage, std::span<const uint8_t> bytes,
    const Limits& limits) {
  Result result;
  const auto fail = [&](Error error, uint32_t offset) {
    result.error = error;
    result.error_dword_offset = offset;
    result.vertex_fetches.clear();
    result.vertex_fetch_bitmap.fill(0);
    result.texture_fetches.clear();
    result.texture_fetch_bitmap = 0;
    result.constant_use = {};
    return result;
  };
  constexpr Limits maximum;
  if (!limits.max_dwords || limits.max_dwords > maximum.max_dwords ||
      !limits.max_exec_instructions ||
      limits.max_exec_instructions > maximum.max_exec_instructions ||
      !limits.max_vertex_fetches ||
      limits.max_vertex_fetches > maximum.max_vertex_fetches ||
      !limits.max_texture_fetches ||
      limits.max_texture_fetches > maximum.max_texture_fetches) {
    return fail(Error::kInvalidLimits, 0);
  }
  if (stage != xenos::ShaderType::kVertex &&
      stage != xenos::ShaderType::kPixel) {
    return fail(Error::kInvalidStage, 0);
  }
  result.stage = stage;
  if (bytes.empty() || bytes.size() % 12) {
    return fail(Error::kInvalidByteCount, 0);
  }
  if (bytes.size() / 4 > limits.max_dwords) {
    return fail(Error::kLimitExceeded, 0);
  }
  // AnalyzeUcode may write globally configured shader dumps. Never change that
  // configuration or allow diagnostic work to write outside its capture.
  if (!cvars::dump_shaders.empty()) {
    return fail(Error::kShaderDumpEnabled, 0);
  }
  std::vector<uint32_t> words(bytes.size() / 4);
  for (size_t i = 0; i < words.size(); ++i) {
    words[i] = uint32_t(bytes[i * 4]) << 24 | uint32_t(bytes[i * 4 + 1]) << 16 |
               uint32_t(bytes[i * 4 + 2]) << 8 | bytes[i * 4 + 3];
  }
  const uint32_t group_count = uint32_t(words.size() / 3);
  uint32_t cf_bound = group_count;
  bool pixel_forward_flow = false;
  // Match the legacy analyzer's CF-table bound, while refusing an instruction
  // range that overlaps the table or leaves the owned bytecode.
  for (uint32_t i = 0; i < cf_bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (const auto& cf : pair) {
      using Op = ucode::ControlFlowOpcode;
      switch (cf.opcode()) {
        case Op::kNop:
        case Op::kMarkVsFetchDone:
          break;
        case Op::kAlloc:
          if (cf.alloc.alloc_type() == ucode::AllocType::kMemory) {
            return fail(Error::kUnsupportedMemexport, i * 3);
          }
          break;
        case Op::kCondJmp:
          if (stage != xenos::ShaderType::kPixel) {
            return fail(Error::kUnsupportedControlFlow, i * 3);
          }
          pixel_forward_flow = true;
          break;
        case Op::kExec:
        case Op::kExecEnd:
          if (cf.exec.address() <= i || cf.exec.count() > 6 ||
              cf.exec.address() > group_count ||
              cf.exec.count() > group_count - cf.exec.address()) {
            return fail(Error::kInvalidExec, i * 3);
          }
          cf_bound = std::min(cf_bound, cf.exec.address());
          break;
        default:
          return fail(Error::kUnsupportedControlFlow, i * 3);
      }
    }
  }
  if (pixel_forward_flow) {
    const auto flow = ValidateForwardPixelFlow(words, cf_bound);
    if (flow.error != Error::kNone) {
      return fail(flow.error, flow.offset);
    }
  }
  bool ended = false;
  bool previous_full_valid = false;
  ucode::VertexFetchInstruction previous_full{};
  uint32_t executed_instructions = 0;
  for (uint32_t i = 0; i < cf_bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (const auto& cf : pair) {
      using Op = ucode::ControlFlowOpcode;
      if (!pixel_forward_flow && ended && cf.opcode() != Op::kNop) {
        return fail(Error::kUnsupportedControlFlow, i * 3);
      }
      if (cf.opcode() != Op::kExec && cf.opcode() != Op::kExecEnd) {
        continue;
      }
      if (cf.exec.address() < cf_bound) {
        return fail(Error::kInvalidExec, i * 3);
      }
      if (cf.exec.count() >
          limits.max_exec_instructions - executed_instructions) {
        return fail(Error::kLimitExceeded, i * 3);
      }
      executed_instructions += cf.exec.count();
      uint32_t sequence = cf.exec.sequence();
      for (uint32_t j = 0; j < cf.exec.count(); ++j, sequence >>= 2) {
        const uint32_t offset = (cf.exec.address() + j) * 3;
        if (sequence & 1) {
          ucode::FetchInstruction raw;
          std::memcpy(&raw, words.data() + offset, sizeof(raw));
          if (raw.opcode() != ucode::FetchOpcode::kVertexFetch) {
            if (raw.opcode() != ucode::FetchOpcode::kTextureFetch ||
                stage != xenos::ShaderType::kPixel) {
              return fail(Error::kUnsupportedTexture, offset);
            }
            const auto& op = raw.texture_fetch();
            // Only the normalized 2D sample instruction is inventoried.
            // Reject stateful LOD/gradient operations and all reserved/unknown
            // fields before the legacy parser. Its assertions are not input
            // validation. Sampler/layout/coordinate bounds remain unproven.
            const uint32_t arbitrary_filter = (words[offset + 1] >> 21) & 7;
            if (op.is_predicated() || op.predicate_condition() ||
                op.is_src_relative() || op.is_dest_relative() ||
                op.dimension() != xenos::FetchOpDimension::k2D ||
                op.unnormalized_coordinates() || !op.fetch_valid_only() ||
                !ValidTextureFilter(op.mag_filter()) ||
                !ValidTextureFilter(op.min_filter()) ||
                op.vol_mag_filter() != xenos::TextureFilter::kUseFetchConst ||
                op.vol_min_filter() != xenos::TextureFilter::kUseFetchConst ||
                (op.aniso_filter() != xenos::AnisoFilter::kDisabled &&
                 op.aniso_filter() != xenos::AnisoFilter::kUseFetchConst) ||
                arbitrary_filter || (words[offset + 1] & (1u << 30)) ||
                (words[offset + 2] & (31u << 9)) ||
                op.sample_location() != xenos::SampleLocation::kCentroid ||
                !op.use_computed_lod() || op.use_register_lod() ||
                op.use_register_gradients() || op.offset_z() != 0.0f) {
              return fail(Error::kUnsupportedTexture, offset);
            }
            for (uint32_t k = 0; k < 4; ++k) {
              if (((op.dest_swizzle() >> (k * 3)) & 7) == 6) {
                return fail(Error::kInvalidFetch, offset);
              }
            }
            if (op.fetch_constant_index() >= 32 ||
                result.texture_fetches.size() >= limits.max_texture_fetches) {
              return fail(op.fetch_constant_index() >= 32
                              ? Error::kInvalidFetch
                              : Error::kLimitExceeded,
                          offset);
            }
            const uint32_t slot = op.fetch_constant_index();
            result.texture_fetch_bitmap |= uint32_t(1) << slot;
            result.texture_fetches.push_back(
                {offset,
                 slot,
                 uint32_t(op.dimension()),
                 !op.unnormalized_coordinates(),
                 op.src(),
                 op.src_swizzle(),
                 op.is_src_relative(),
                 op.dest(),
                 op.dest_swizzle(),
                 op.is_dest_relative(),
                 op.fetch_valid_only(),
                 uint32_t(op.mag_filter()),
                 uint32_t(op.min_filter()),
                 uint32_t(op.mip_filter()),
                 uint32_t(op.aniso_filter()),
                 arbitrary_filter,
                 uint32_t(op.vol_mag_filter()),
                 uint32_t(op.vol_min_filter()),
                 uint32_t(op.sample_location()),
                 op.use_computed_lod(),
                 op.use_register_lod(),
                 op.use_register_gradients(),
                 op.lod_bias(),
                 op.offset_x(),
                 op.offset_y(),
                 op.offset_z(),
                 {words[offset], words[offset + 1], words[offset + 2]}});
            continue;
          }
          const auto& op = raw.vertex_fetch();
          if (stage != xenos::ShaderType::kVertex || op.is_predicated() ||
              !(words[offset] & (uint32_t(1) << 19)) ||
              (words[offset + 1] & 0x00C00000) ||
              !ValidFormat(op.data_format()) ||
              (op.is_mini_fetch() && !previous_full_valid) ||
              (!op.is_mini_fetch() &&
               (op.fetch_constant_index() >= 96 ||
                ((words[offset] >> 25) & 3) == 3 || !op.stride()))) {
            return fail(Error::kInvalidFetch, offset);
          }
          for (uint32_t k = 0; k < 4; ++k) {
            if (((op.dest_swizzle() >> (k * 3)) & 7) == 6) {
              return fail(Error::kInvalidFetch, offset);
            }
          }
          ParsedVertexFetchInstruction parsed;
          ParseVertexFetchInstruction(op, previous_full, parsed);
          if (!op.is_mini_fetch()) {
            previous_full = op;
            previous_full_valid = true;
          }
          const uint32_t slot = parsed.operands[1].storage_index;
          if (slot >= 96 ||
              result.vertex_fetches.size() >= limits.max_vertex_fetches) {
            return fail(
                slot >= 96 ? Error::kInvalidFetch : Error::kLimitExceeded,
                offset);
          }
          result.vertex_fetch_bitmap[slot / 32] |= uint32_t(1) << (slot % 32);
          result.vertex_fetches.push_back(
              {offset, slot, parsed.attributes.stride, parsed.attributes.offset,
               uint32_t(parsed.attributes.data_format),
               xenos::GetVertexFormatNeededWords(
                   parsed.attributes.data_format,
                   parsed.result.GetUsedResultComponents()),
               parsed.operands[0].storage_index,
               uint32_t(parsed.operands[0].components[0]),
               parsed.operands[0].storage_addressing_mode !=
                   InstructionStorageAddressingMode::kAbsolute,
               parsed.attributes.is_index_rounded, parsed.is_mini_fetch});
        } else {
          ucode::AluInstruction op;
          std::memcpy(&op, words.data() + offset, sizeof(op));
          const auto& vector =
              ucode::GetAluVectorOpcodeInfo(op.vector_opcode());
          const auto& scalar =
              ucode::GetAluScalarOpcodeInfo(op.scalar_opcode());
          if (op.is_predicated() ||
              std::string_view(vector.name).starts_with("opcode_") ||
              std::string_view(scalar.name).starts_with("opcode_")) {
            return fail(Error::kInvalidAlu, offset);
          }
          if (op.is_export()) {
            using Export = ucode::ExportRegister;
            const auto target = Export(op.vector_dest());
            if (target == Export::kExportAddress ||
                (target >= Export::kExportData0 &&
                 target <= Export::kExportData4)) {
              return fail(Error::kUnsupportedMemexport, offset);
            }
            const bool valid =
                stage == xenos::ShaderType::kVertex
                    ? ((target >= Export::kVSInterpolator0 &&
                        target <= Export::kVSInterpolator15) ||
                       target == Export::kVSPosition ||
                       target == Export::kVSPointSizeEdgeFlagKillVertex)
                    : ((target >= Export::kPSColor0 &&
                        target <= Export::kPSColor3) ||
                       target == Export::kPSDepth);
            if (!valid) {
              return fail(Error::kInvalidAlu, offset);
            }
          }
        }
      }
      ended = cf.opcode() == Op::kExecEnd;
    }
  }
  if (!pixel_forward_flow && !ended) {
    return fail(Error::kMissingEnd, cf_bound * 3);
  }
  // All legacy analysis dereferences and asserted instruction categories above
  // have been preflighted on this same immutable aligned copy. This creates no
  // translations/backend resources and grants no memory-access permission.
  Shader shader(stage, XXH3_64bits(bytes.data(), bytes.size()), words.data(),
                words.size(), std::endian::native);
  StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  const auto& map = shader.constant_register_map();
  for (size_t i = 0; i < result.vertex_fetch_bitmap.size(); ++i) {
    if (result.vertex_fetch_bitmap[i] != map.vertex_fetch_bitmap[i]) {
      return fail(Error::kAnalysisMismatch, 0);
    }
  }
  uint32_t legacy_texture_bitmap = 0;
  if (shader.texture_bindings().size() != result.texture_fetches.size()) {
    return fail(Error::kAnalysisMismatch, 0);
  }
  for (size_t i = 0; i < shader.texture_bindings().size(); ++i) {
    const auto& binding = shader.texture_bindings()[i];
    const auto& fetch = result.texture_fetches[i];
    if (binding.fetch_constant >= 32 || binding.fetch_constant != fetch.slot ||
        binding.fetch_instr.opcode != ucode::FetchOpcode::kTextureFetch ||
        uint32_t(binding.fetch_instr.dimension) != fetch.dimension ||
        binding.fetch_instr.operands[0].storage_index !=
            fetch.coordinate_register) {
      return fail(Error::kAnalysisMismatch, fetch.instruction_dword_offset);
    }
    legacy_texture_bitmap |= uint32_t(1) << binding.fetch_constant;
  }
  if (legacy_texture_bitmap != result.texture_fetch_bitmap ||
      shader.memexport_eM_written() ||
      !shader.memexport_stream_constants().empty()) {
    return fail(Error::kAnalysisMismatch, 0);
  }
  result.writes_depth = shader.writes_depth();
  result.kills_pixels = shader.kills_pixels();
  result.metadata_analyzed = shader.is_ucode_analyzed();
  if (result.metadata_analyzed) {
    for (size_t i = 0; i < result.constant_use.float_bitmap.size(); ++i) {
      result.constant_use.float_bitmap[i] = map.float_bitmap[i];
    }
    result.constant_use.float_count = map.float_count;
    result.constant_use.float_dynamic_addressing = map.float_dynamic_addressing;
    for (size_t i = 0; i < result.constant_use.bool_bitmap.size(); ++i) {
      result.constant_use.bool_bitmap[i] = map.bool_bitmap[i];
    }
    result.constant_use.loop_bitmap = map.loop_bitmap;
  }
  return result;
}

const char* NativeShaderReadAnalysis::ErrorName(Error error) {
  switch (error) {
#define XE_SHADER_READ_ERROR(name, text) \
  case Error::name:                      \
    return text
    XE_SHADER_READ_ERROR(kNone, "none");
    XE_SHADER_READ_ERROR(kInvalidLimits, "invalid_limits");
    XE_SHADER_READ_ERROR(kInvalidByteCount, "invalid_byte_count");
    XE_SHADER_READ_ERROR(kLimitExceeded, "limit_exceeded");
    XE_SHADER_READ_ERROR(kInvalidStage, "invalid_stage");
    XE_SHADER_READ_ERROR(kShaderDumpEnabled, "shader_dump_enabled");
    XE_SHADER_READ_ERROR(kUnsupportedControlFlow, "unsupported_control_flow");
    XE_SHADER_READ_ERROR(kInvalidExec, "invalid_exec");
    XE_SHADER_READ_ERROR(kMissingEnd, "missing_end");
    XE_SHADER_READ_ERROR(kInvalidFetch, "invalid_fetch");
    XE_SHADER_READ_ERROR(kUnsupportedTexture, "unsupported_texture");
    XE_SHADER_READ_ERROR(kUnsupportedMemexport, "unsupported_memexport");
    XE_SHADER_READ_ERROR(kInvalidAlu, "invalid_alu");
    XE_SHADER_READ_ERROR(kAnalysisMismatch, "analysis_mismatch");
#undef XE_SHADER_READ_ERROR
  }
  return "unknown";
}
}  // namespace xe::gpu
