/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_vertex_access_analysis.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "xenia/gpu/ucode.h"

namespace xe::gpu {

NativeVertexAccessAnalysis::Result NativeVertexAccessAnalysis::Analyze(
    const NativeDrawReadInventory::Draw& draw, std::span<const uint8_t> bytes) {
  using ReadInventory = NativeDrawReadInventory;
  using ShaderAnalysis = NativeShaderReadAnalysis;
  Result result;
  const auto fail = [&](Error error, uint32_t offset = UINT32_MAX,
                        uint32_t slot = UINT32_MAX) {
    result.error = error;
    result.error_dword_offset = offset;
    result.error_slot = slot;
    result.vertex_input_bounds_verified = false;
    result.accesses.clear();
    return result;
  };
  const bool copy = draw.state_known[0] &&
                    draw.state_words[0] == uint32_t(xenos::EdramMode::kCopy);
  ShaderAnalysis::Result shader;
  if (!copy) {
    shader = ShaderAnalysis::Analyze(xenos::ShaderType::kVertex, bytes);
    if (!shader) {
      result.shader_error = shader.error;
      return fail(Error::kInvalidShader, shader.error_dword_offset);
    }
  }
  // Reuse the checked implicit geometry, binding type, declared-size budget
  // and canonical physical extent rules. Never accept caller-made metadata.
  const auto reads = ReadInventory::VertexReads(draw, copy ? nullptr : &shader);
  result.read_error = reads.error;
  if (reads.error != ReadInventory::Error::kNone) {
    return fail(Error::kInvalidDraw, draw.packet_dword_offset,
                reads.error_slot);
  }
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = draw.initiator;
  result.vertex_count = initiator.num_indices;
  if (copy) {
    if (reads.reads.size() != 1 || reads.reads[0].slot != 0 ||
        reads.reads[0].byte_count != 24 || !reads.reads[0].resolve_rectangle) {
      return fail(Error::kAnalysisMismatch);
    }
    result.copy_rectangle = true;
    result.accesses.push_back(
        {UINT32_MAX, 0, 0, 0, 0, 6, 6, reads.reads[0].physical_address, false});
    result.vertex_input_bounds_verified = true;
    return result;
  }

  // No inherited VGT values or min/max-as-a-memory-guard assumption. This
  // slice explicitly requires the ordinary zero-based auto-index contract.
  if (!draw.state_known[3] || !draw.state_known[4] || !draw.state_known[5]) {
    return fail(Error::kMissingVgtState);
  }
  if (draw.state_words[4] || draw.state_words[5] ||
      (draw.state_words[3] & ~xenos::kVertexIndexMask) ||
      draw.state_words[3] < result.vertex_count - 1) {
    return fail(Error::kUnsupportedVgtState);
  }
  if (shader.vertex_fetches.empty()) {
    return fail(Error::kMissingVertexFetch);
  }
  // Validated auto indices are at most65534, so their float representation is
  // exact. The chosen backend must actually honor the explicit contract above.
  result.uses_auto_index_backend_contract = true;

  std::vector<uint32_t> words(bytes.size() / 4);
  for (size_t i = 0; i < words.size(); ++i) {
    words[i] = uint32_t(bytes[i * 4]) << 24 | uint32_t(bytes[i * 4 + 1]) << 16 |
               uint32_t(bytes[i * 4 + 2]) << 8 | bytes[i * 4 + 3];
  }
  // The successful bounded preflight already checked every range and opcode.
  // Rewalk its actual execution order to prove index provenance, rather than
  // inferring that order from sorted fetch metadata or shader identity.
  uint32_t cf_bound = uint32_t(words.size() / 3);
  for (uint32_t i = 0; i < cf_bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (const auto& cf : pair) {
      if (cf.opcode() == ucode::ControlFlowOpcode::kExec ||
          cf.opcode() == ucode::ControlFlowOpcode::kExecEnd) {
        cf_bound = std::min(cf_bound, cf.exec.address());
      }
    }
  }
  bool seen_alu = false;
  bool initial_r0_x_unchanged = true;
  const ShaderAnalysis::VertexFetch* full = nullptr;
  size_t fetch_index = 0;
  for (uint32_t i = 0; i < cf_bound; ++i) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (const auto& cf : pair) {
      if (cf.opcode() != ucode::ControlFlowOpcode::kExec &&
          cf.opcode() != ucode::ControlFlowOpcode::kExecEnd) {
        continue;
      }
      uint32_t sequence = cf.exec.sequence();
      for (uint32_t j = 0; j < cf.exec.count(); ++j, sequence >>= 2) {
        const uint32_t offset = (cf.exec.address() + j) * 3;
        if (!(sequence & 1)) {
          // Reject even a seemingly harmless ALU before a later full fetch.
          // This first slice intentionally has no general ALU range evaluator.
          seen_alu = true;
          continue;
        }
        if (fetch_index >= shader.vertex_fetches.size()) {
          return fail(Error::kAnalysisMismatch, offset);
        }
        const auto& fetch = shader.vertex_fetches[fetch_index++];
        ucode::VertexFetchInstruction op;
        std::memcpy(&op, words.data() + offset, sizeof(op));
        if (fetch.instruction_dword_offset != offset ||
            fetch.mini != op.is_mini_fetch()) {
          return fail(Error::kAnalysisMismatch, offset, fetch.slot);
        }
        if (!fetch.mini) {
          if (seen_alu || !initial_r0_x_unchanged || fetch.index_register ||
              fetch.index_component || fetch.index_relative ||
              fetch.index_rounded) {
            return fail(Error::kUnsupportedIndexProvenance, offset, fetch.slot);
          }
          // Offset-zero full fetches match the cached-address contract used by
          // this narrow proof. Minis may have their own checked signed offset.
          if (fetch.offset_words) {
            return fail(Error::kUnsupportedFetch, offset, fetch.slot);
          }
          full = &fetch;
        } else if (!full || fetch.slot != full->slot ||
                   fetch.stride_words != full->stride_words) {
          return fail(Error::kAnalysisMismatch, offset, fetch.slot);
        }
        if (!fetch.stride_words || !fetch.needed_word_mask ||
            (fetch.needed_word_mask & ~15u)) {
          return fail(Error::kUnsupportedFetch, offset, fetch.slot);
        }
        const auto binding =
            std::find_if(reads.reads.begin(), reads.reads.end(),
                         [&](const ReadInventory::VertexRead& read) {
                           return read.slot == fetch.slot;
                         });
        if (binding == reads.reads.end()) {
          return fail(Error::kAnalysisMismatch, offset, fetch.slot);
        }
        const int64_t first = int64_t(fetch.offset_words) +
                              std::countr_zero(fetch.needed_word_mask);
        const int64_t end =
            int64_t(uint64_t(result.vertex_count - 1) * fetch.stride_words) +
            fetch.offset_words + std::bit_width(fetch.needed_word_mask);
        const uint32_t declared = binding->byte_count / 4;
        if (first < 0 || end <= first || uint64_t(end) > declared) {
          return fail(Error::kOutOfBounds, offset, fetch.slot);
        }
        if (uint64_t(binding->physical_address) + uint64_t(end) * 4 >
            UINT64_C(0x20000000)) {
          return fail(Error::kInvalidMemoryRange, offset, fetch.slot);
        }
        result.accesses.push_back({offset, fetch.slot, fetch.stride_words,
                                   fetch.offset_words, uint64_t(first),
                                   uint64_t(end), declared,
                                   binding->physical_address, fetch.mini});
        // A fetch writes after reading its own index. Relative/unknown
        // destinations conservatively clobber initial r0.x for future full
        // fetches. A mini may still use the already proven cached address.
        if (op.is_dest_relative() ||
            (!op.dest() &&
             ucode::GetFetchDestinationComponentSwizzle(op.dest_swizzle(), 0) !=
                 ucode::FetchDestinationSwizzle::kKeep)) {
          initial_r0_x_unchanged = false;
        }
      }
    }
  }
  if (fetch_index != shader.vertex_fetches.size()) {
    return fail(Error::kAnalysisMismatch);
  }
  result.vertex_input_bounds_verified = true;
  return result;
}

const char* NativeVertexAccessAnalysis::ErrorName(Error error) {
  switch (error) {
#define XE_VERTEX_ACCESS_ERROR(name, text) \
  case Error::name:                        \
    return text
    XE_VERTEX_ACCESS_ERROR(kNone, "none");
    XE_VERTEX_ACCESS_ERROR(kInvalidDraw, "invalid_draw");
    XE_VERTEX_ACCESS_ERROR(kMissingVgtState, "missing_vgt_state");
    XE_VERTEX_ACCESS_ERROR(kUnsupportedVgtState, "unsupported_vgt_state");
    XE_VERTEX_ACCESS_ERROR(kInvalidShader, "invalid_shader");
    XE_VERTEX_ACCESS_ERROR(kMissingVertexFetch, "missing_vertex_fetch");
    XE_VERTEX_ACCESS_ERROR(kUnsupportedIndexProvenance,
                           "unsupported_index_provenance");
    XE_VERTEX_ACCESS_ERROR(kUnsupportedFetch, "unsupported_fetch");
    XE_VERTEX_ACCESS_ERROR(kOutOfBounds, "out_of_bounds");
    XE_VERTEX_ACCESS_ERROR(kInvalidMemoryRange, "invalid_memory_range");
    XE_VERTEX_ACCESS_ERROR(kAnalysisMismatch, "analysis_mismatch");
#undef XE_VERTEX_ACCESS_ERROR
  }
  return "unknown";
}

}  // namespace xe::gpu
