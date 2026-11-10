/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_frame_command_plan.h"

#include <array>
#include <bitset>
#include <optional>
#include <utility>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/xenos.h"

namespace xe::gpu {
namespace {
constexpr uint32_t kPhysicalSize = 0x20000000;

// This context-state group is emitted by the native record epilogue. It is
// storage-only at WriteRegister: no event, MMIO, memory write or GPU work is
// triggered until a future draw (which is forbidden after the final copy).
// Preserve all meaningful signed-offset and offset-disable bits; exclude only
// reserved bits from the actual register definitions in registers.h.
bool TerminalWindowStateAllowed(uint32_t index, uint32_t value) {
  switch (index) {
    case XE_GPU_REG_PA_SC_WINDOW_OFFSET:
      return !(value & ~UINT32_C(0x7FFF7FFF));
    case XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL:
      return !(value & ~UINT32_C(0xBFFF3FFF));
    case XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR:
      return !(value & ~UINT32_C(0x3FFF3FFF));
    default:
      return false;
  }
}

bool RegisterAllowed(uint32_t index, uint32_t value, bool epilogue) {
  if (epilogue) {
    switch (index) {
      case XE_GPU_REG_COHER_DEST_BASE_1:
      case XE_GPU_REG_RB_COPY_CONTROL:
      case XE_GPU_REG_RB_COPY_DEST_INFO:
        return value == 0;
      case XE_GPU_REG_WAIT_UNTIL:
        return value == 0x20000;
      case XE_GPU_REG_TC_CNTL_STATUS:
      case XE_GPU_REG_VC_CNTL:
        return value == 1;
      default:
        return false;
    }
  }
  // These three low controls have no address payload. Their eventual executor
  // semantics must remain private-bank operations, not global title MMIO.
  if (index == XE_GPU_REG_WAIT_UNTIL) {
    return value == 0x20000;
  }
  if (index == XE_GPU_REG_TC_CNTL_STATUS || index == XE_GPU_REG_VC_CNTL) {
    return value == 1;
  }
  if (index == XE_GPU_REG_COHER_SIZE_HOST) {
    return value <= kPhysicalSize;
  }
  if (index == XE_GPU_REG_COHER_BASE_HOST) {
    return value < kPhysicalSize;
  }
  if (index == XE_GPU_REG_COHER_STATUS_HOST) {
    return !(value & ~UINT32_C(0x0701FFFF));
  }
  // Render state and the four declared constant families. Scratch, gamma,
  // arbitrary MMIO, event initiators and memory-write controls are excluded.
  // The broad render family is only a packet profile: bitfield/state-use
  // semantics are not proved by storing a value in this owned plan.
  return (index >= 0x2000 && index <= 0x23FF &&
          index != XE_GPU_REG_VGT_EVENT_INITIATOR) ||
         (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
          index <= XE_GPU_REG_SHADER_CONSTANT_511_W) ||
         (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
          index < XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 192) ||
         (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
          index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31);
}
}  // namespace

NativeFrameCommandPlan::Result NativeFrameCommandPlan::Analyze(
    std::span<const uint8_t> primary) {
  return Analyze(primary, Limits{});
}

NativeFrameCommandPlan::Result NativeFrameCommandPlan::Analyze(
    std::span<const uint8_t> primary, const Limits& limits) {
  return AnalyzeWithInput(primary, limits, {});
}
NativeFrameCommandPlan::Result NativeFrameCommandPlan::AnalyzeWithTitleInput(
    std::span<const uint8_t> primary,
    std::shared_ptr<const NativeSystemTitleInput> title_input) {
  return AnalyzeWithInput(primary, Limits{}, std::move(title_input));
}
NativeFrameCommandPlan::Result NativeFrameCommandPlan::AnalyzeWithInput(
    std::span<const uint8_t> primary, const Limits& limits,
    std::shared_ptr<const NativeSystemTitleInput> title_input) {
  const auto fail = [](Error error, uint32_t offset,
                       uint32_t reg = UINT32_MAX) {
    Result result;
    result.error = error;
    result.error_dword_offset = offset;
    result.error_register = reg;
    return result;  // No rejected prefix or partly validated plan escapes.
  };
  if (title_input && !title_input->valid()) {
    return fail(Error::kInvalidMemoryRange, 0);
  }
  constexpr Limits maximum;
  if (!limits.max_dwords || limits.max_dwords > maximum.max_dwords ||
      !limits.max_packets || limits.max_packets > maximum.max_packets ||
      !limits.max_draws || limits.max_draws > maximum.max_draws ||
      !limits.max_shader_loads ||
      limits.max_shader_loads > maximum.max_shader_loads ||
      !limits.max_shader_dwords ||
      limits.max_shader_dwords > maximum.max_shader_dwords) {
    return fail(Error::kInvalidLimits, 0);
  }
  if (primary.empty() || primary.size() % 4) {
    return fail(Error::kInvalidByteCount, 0);
  }
  if (primary.size() / 4 > limits.max_dwords) {
    return fail(Error::kLimitExceeded, 0);
  }
  // Parse and inventory the SAME immutable owned copy. Nothing retains the
  // caller's span and a later input mutation cannot change this plan.
  const std::vector<uint8_t> bytes(primary.begin(), primary.end());
  auto plan =
      std::shared_ptr<NativeFrameCommandPlan>(new NativeFrameCommandPlan);
  plan->title_input_ = std::move(title_input);
  auto& words = plan->words_;
  words.reserve(bytes.size() / 4);
  for (size_t i = 0; i < bytes.size(); i += 4) {
    words.push_back(uint32_t(bytes[i]) << 24 | uint32_t(bytes[i + 1]) << 16 |
                    uint32_t(bytes[i + 2]) << 8 | bytes[i + 3]);
  }
  std::array<uint32_t, RegisterFile::kRegisterCount> registers{};
  std::bitset<RegisterFile::kRegisterCount> known;
  if (plan->title_input_) {
    for (uint32_t i = 0; i < 6; ++i) {
      const uint32_t index = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 15 * 6 + i;
      registers[index] = plan->title_input_->fetch()[i];
      known.set(index);
    }
  }
  std::optional<uint32_t> vertex, pixel;
  struct Selection {
    uint32_t offset;
    std::optional<uint32_t> vertex, pixel;
  };
  std::vector<Selection> draws;
  uint32_t shaders = 0;
  std::vector<NativeOverlayProgram::Transaction> overlay_transactions;
  for (uint32_t offset = 0; offset < words.size();) {
    if (plan->packets_.size() >= limits.max_packets) {
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
    const uint32_t count = size - 1;
    auto kind = PacketKind::kNop;
    if (type == 0) {
      if (header & 0x8000) {
        return fail(Error::kUnsupportedPacket, offset);
      }
      const uint32_t first = header & 0x7FFF;
      if (first == XE_GPU_REG_D1OVL_UPDATE) {
        // The genuine native helpers emit one of two complete transactions,
        // after the final resolve/cache flush. Their display-only writes do
        // not enter render state, the title RF, arbitrary MMIO or ownership.
        const bool control_candidate = draws.empty() && !shaders &&
                                       !plan->final_copy_ &&
                                       !plan->cache_flush_;
        if ((!control_candidate &&
             (!plan->final_copy_ || !plan->cache_flush_)) ||
            count != 1 || words[offset + 1] != 0x10000 ||
            overlay_transactions.size() >=
                NativeOverlayProgram::kMaxTransactions) {
          return fail(Error::kInvalidOverlayControl, offset, first);
        }
        NativeOverlayProgram::Transaction transaction;
        transaction.lock_packet_dword_offset = offset;
        struct Group {
          uint32_t index, count;
          NativeOverlayState::Field field;
        };
        using Field = NativeOverlayState::Field;
        constexpr Group enable_groups[] = {
            {XE_GPU_REG_D1OVL_ALPHA_OUTSIDE_OVL_WINDOW, 3,
             Field::kAlphaOutside},
            {XE_GPU_REG_D1OVL_ENABLE, 1, Field::kEnable},
        };
        constexpr Group geometry_groups[] = {
            {XE_GPU_REG_D1OVL_CONTROL1, 1, Field::kControl1},
            {XE_GPU_REG_D1OVL_SURFACE_ADDRESS, 1, Field::kSurfaceAddress},
            {XE_GPU_REG_D1OVL_PITCH, 1, Field::kPitch},
            {XE_GPU_REG_D1OVL_SURFACE_OFFSET_X, 2, Field::kOffsetX},
            {XE_GPU_REG_D1OVL_START, 2, Field::kStart},
        };
        uint32_t cursor = offset + 2;
        if (cursor >= words.size()) {
          return fail(Error::kInvalidOverlayControl, cursor);
        }
        const std::span<const Group> groups =
            words[cursor] ==
                    (2u << 16 | XE_GPU_REG_D1OVL_ALPHA_OUTSIDE_OVL_WINDOW)
                ? std::span<const Group>(enable_groups)
                : std::span<const Group>(geometry_groups);
        std::vector<Packet> packets;
        packets.push_back({PacketKind::kNativeOverlayControl, offset, 2});
        for (const auto& group : groups) {
          if (cursor >= words.size() ||
              group.count + 1 > words.size() - cursor ||
              words[cursor] != ((group.count - 1) << 16 | group.index)) {
            return fail(Error::kInvalidOverlayControl, cursor, group.index);
          }
          for (uint32_t i = 0; i < group.count; ++i) {
            const auto field = Field(uint32_t(group.field) + i);
            const uint32_t value = words[cursor + i + 1];
            if (!NativeOverlayState::ValueAllowed(field, value)) {
              return fail(Error::kInvalidOverlayControl, cursor,
                          group.index + i);
            }
            transaction.writes.push_back({cursor, field, value});
          }
          packets.push_back(
              {PacketKind::kNativeOverlayControl, cursor, group.count + 1});
          cursor += group.count + 1;
        }
        if (cursor >= words.size() || words.size() - cursor < 2 ||
            words[cursor] != XE_GPU_REG_D1OVL_UPDATE ||
            words[cursor + 1] != 0) {
          return fail(Error::kInvalidOverlayControl, cursor,
                      XE_GPU_REG_D1OVL_UPDATE);
        }
        transaction.unlock_packet_dword_offset = cursor;
        packets.push_back({PacketKind::kNativeOverlayControl, cursor, 2});
        if (packets.size() > limits.max_packets - plan->packets_.size()) {
          return fail(Error::kLimitExceeded, offset);
        }
        plan->packets_.insert(plan->packets_.end(), packets.begin(),
                              packets.end());
        overlay_transactions.push_back(std::move(transaction));
        offset = cursor + 2;
        continue;
      }
      // A terminal native window/scissor restoration is one complete context
      // group after the final copy and its cache flush. It remains three real
      // ordered writes, not an ignored epilogue or inherited/reset state. All
      // draw/output proofs refer to the state at their earlier packet offset.
      const bool terminal_window_state =
          plan->cache_flush_ && plan->final_copy_ &&
          first == XE_GPU_REG_PA_SC_WINDOW_OFFSET && count == 3;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = first + i;
        const uint32_t value = words[offset + 1 + i];
        if (plan->title_input_ &&
            index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 15 * 6 &&
            index < XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 16 * 6) {
          return fail(Error::kUnsupportedRegister, offset, index);
        }
        if (index >= RegisterFile::kRegisterCount ||
            !(terminal_window_state
                  ? TerminalWindowStateAllowed(index, value)
                  : RegisterAllowed(index, value, plan->cache_flush_))) {
          return fail(Error::kUnsupportedRegister, offset, index);
        }
        registers[index] = value;
        known.set(index);
        plan->register_writes_.push_back({offset, index, value});
      }
      kind = PacketKind::kRegisterWrite;
    } else if (type == 2) {
      if (header != 0x80000000) {
        return fail(Error::kUnsupportedPacket, offset);
      }
    } else if (type != 3 || (header & 0x80FF)) {
      return fail(Error::kUnsupportedPacket, offset);
    } else {
      const uint32_t opcode = (header >> 8) & 0x7F;
      if (plan->cache_flush_ && opcode != xenos::PM4_NOP) {
        return fail(Error::kUnsupportedPacket, offset);
      }
      switch (opcode) {
        case xenos::PM4_NOP:
          break;
        case xenos::PM4_WAIT_REG_MEM: {
          if (count != 5 || words[offset + 1] != 3 ||
              words[offset + 2] != XE_GPU_REG_COHER_STATUS_HOST ||
              words[offset + 3] != 0 || words[offset + 4] != 0x80000000 ||
              words[offset + 5] >= 0x100 ||
              !known[XE_GPU_REG_COHER_STATUS_HOST] ||
              !known[XE_GPU_REG_COHER_BASE_HOST] ||
              !known[XE_GPU_REG_COHER_SIZE_HOST]) {
            return fail(Error::kUnsupportedWait, offset);
          }
          const uint32_t base = registers[XE_GPU_REG_COHER_BASE_HOST];
          const uint32_t size_bytes = registers[XE_GPU_REG_COHER_SIZE_HOST];
          if (size_bytes > kPhysicalSize - base) {
            return fail(Error::kUnsupportedWait, offset);
          }
          plan->coherency_waits_.push_back(
              {offset, registers[XE_GPU_REG_COHER_STATUS_HOST], base,
               size_bytes});
          // A future checked coherency operation clears this register. This
          // transition is NOT a CPU acknowledgment or any GPU completion proof.
          registers[XE_GPU_REG_COHER_STATUS_HOST] = 0;
          kind = PacketKind::kCoherencyWait;
        } break;
        case xenos::PM4_IM_LOAD:
        case xenos::PM4_IM_LOAD_IMMEDIATE: {
          if (count < 2) {
            return fail(Error::kInvalidShader, offset);
          }
          const bool in = opcode == xenos::PM4_IM_LOAD_IMMEDIATE;
          const uint32_t address_type = words[offset + 1];
          const uint32_t stage = in ? address_type : address_type & 3;
          const uint32_t start_size = words[offset + 2];
          const uint32_t dwords = start_size & 0xFFFF;
          if (stage > 1 || (start_size >> 16) || !dwords ||
              dwords > limits.max_shader_dwords ||
              count != (in ? dwords + 2 : 2)) {
            return fail(Error::kInvalidShader, offset);
          }
          if (shaders >= limits.max_shader_loads) {
            return fail(Error::kLimitExceeded, offset);
          }
          if (!in) {
            const uint32_t address = xenos::CpuToGpu(address_type & ~3u);
            if (address >= kPhysicalSize ||
                dwords * 4 > kPhysicalSize - address) {
              return fail(Error::kInvalidMemoryRange, offset);
            }
          }
          (stage ? pixel : vertex) = shaders++;
          kind = in ? PacketKind::kInlineShader : PacketKind::kShaderLoad;
        } break;
        case xenos::PM4_INVALIDATE_STATE: {
          constexpr uint32_t mask =
              xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER |
              xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER;
          if (count != 1 || !words[offset + 1] || (words[offset + 1] & ~mask)) {
            return fail(Error::kUnsupportedPacket, offset);
          }
          if (words[offset + 1] & xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER) {
            vertex.reset();
          }
          if (words[offset + 1] & xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER) {
            pixel.reset();
          }
          kind = PacketKind::kShaderInvalidation;
        } break;
        case xenos::PM4_DRAW_INDX:
        case xenos::PM4_DRAW_INDX_2: {
          if (plan->final_copy_ ||
              count != (opcode == xenos::PM4_DRAW_INDX ? 2u : 1u) ||
              (opcode == xenos::PM4_DRAW_INDX && words[offset + 1])) {
            return fail(Error::kInvalidDraw, offset);
          }
          for (uint32_t reg : {uint32_t(XE_GPU_REG_RB_MODECONTROL),
                               uint32_t(XE_GPU_REG_PA_SC_VIZ_QUERY),
                               uint32_t(XE_GPU_REG_VGT_OUTPUT_PATH_CNTL)}) {
            if (!known[reg]) {
              return fail(Error::kMissingLocalState, offset, reg);
            }
          }
          const uint32_t mode = registers[XE_GPU_REG_RB_MODECONTROL];
          if (mode != uint32_t(xenos::EdramMode::kColorDepth) &&
              mode != uint32_t(xenos::EdramMode::kDepthOnly) &&
              mode != uint32_t(xenos::EdramMode::kCopy)) {
            return fail(Error::kInvalidDraw, offset);
          }
          if (mode != uint32_t(xenos::EdramMode::kCopy)) {
            if (!vertex ||
                (mode == uint32_t(xenos::EdramMode::kColorDepth) && !pixel)) {
              return fail(Error::kMissingLocalState, offset);
            }
            const uint32_t stages =
                mode == uint32_t(xenos::EdramMode::kDepthOnly) ? 1 : 2;
            for (uint32_t i = 0; i < stages; ++i) {
              const uint32_t reg =
                  i == 0 ? XE_GPU_REG_SQ_VS_CONST : XE_GPU_REG_SQ_PS_CONST;
              if (!known[reg]) {
                return fail(Error::kMissingLocalState, offset, reg);
              }
              if (registers[reg] != (i == 0 ? 0x000FF000u : 0x000FF100u)) {
                return fail(Error::kUnsupportedSelector, offset, reg);
              }
            }
          }
          if (draws.size() >= limits.max_draws) {
            return fail(Error::kLimitExceeded, offset);
          }
          draws.push_back({offset, vertex, pixel});
          plan->final_copy_ = mode == uint32_t(xenos::EdramMode::kCopy);
          if (plan->final_copy_) {
            plan->final_copy_packet_dword_offset_ = offset;
          }
          kind = PacketKind::kDraw;
        } break;
        case xenos::PM4_EVENT_WRITE:
          if (count != 1 || words[offset + 1] != xenos::CACHE_FLUSH ||
              draws.empty()) {
            return fail(Error::kUnsupportedPacket, offset);
          }
          // No address, memory write, IRQ, guest signal or completion token.
          plan->cache_flush_ = true;
          kind = PacketKind::kCacheFlush;
          break;
        default:
          return fail(Error::kUnsupportedPacket, offset);
      }
    }
    plan->packets_.push_back({kind, offset, size});
    offset += size;
  }
  if (draws.empty()) {
    // A no-image native teardown record is a complete, independently typed
    // control program. Never admit a generic no-draw prefix as a Source.
    if (shaders || plan->final_copy_ || plan->cache_flush_ ||
        plan->coherency_waits_.empty() || overlay_transactions.size() != 1) {
      return fail(Error::kNoDraw, 0);
    }
    for (const auto& write : plan->register_writes_) {
      const bool window = TerminalWindowStateAllowed(write.index, write.value);
      const bool coherency = write.index == XE_GPU_REG_COHER_STATUS_HOST ||
                             write.index == XE_GPU_REG_COHER_SIZE_HOST ||
                             write.index == XE_GPU_REG_COHER_BASE_HOST;
      if (!window && !coherency) {
        return fail(Error::kUnsupportedRegister, write.packet_dword_offset,
                    write.index);
      }
    }
    const auto& control = overlay_transactions.front();
    using Field = NativeOverlayState::Field;
    if (control.writes.size() != 4 ||
        control.writes[0].field != Field::kAlphaOutside ||
        control.writes[1].field != Field::kAlphaInside ||
        control.writes[2].field != Field::kAlphaControl ||
        control.writes[3].field != Field::kEnable ||
        control.writes[3].value != 0 ||
        control.unlock_packet_dword_offset + 2 != words.size()) {
      return fail(Error::kInvalidOverlayControl,
                  control.lock_packet_dword_offset);
    }
    for (const auto& packet : plan->packets_) {
      if (packet.kind != PacketKind::kRegisterWrite &&
          packet.kind != PacketKind::kCoherencyWait &&
          packet.kind != PacketKind::kNativeOverlayControl &&
          packet.kind != PacketKind::kNop) {
        return fail(Error::kUnsupportedPacket, packet.dword_offset);
      }
      if (packet.kind != PacketKind::kNativeOverlayControl &&
          packet.dword_offset > control.lock_packet_dword_offset) {
        return fail(Error::kInvalidOverlayControl, packet.dword_offset);
      }
    }
    plan->kind_ = Kind::kOverlayControl;
  }
  plan->inventory_ = NativeDrawReadInventory::Scan(bytes, plan->title_input_);
  const auto& inventory = plan->inventory_;
  if (!inventory.complete_primary_scan ||
      inventory.error != NativeDrawReadInventory::Error::kNone ||
      inventory.shaders.size() != shaders ||
      inventory.draws.size() != draws.size() ||
      inventory.state_invalidations != uint32_t(plan->cache_flush_)) {
    return fail(Error::kInventoryMismatch, inventory.error_dword_offset);
  }
  for (size_t i = 0; i < draws.size(); ++i) {
    const auto& draw = inventory.draws[i];
    if (draw.error != NativeDrawReadInventory::Error::kNone) {
      return fail(Error::kInvalidDraw, draw.packet_dword_offset);
    }
    if (draw.packet_dword_offset != draws[i].offset ||
        draw.vertex_shader != draws[i].vertex ||
        draw.pixel_shader != draws[i].pixel) {
      return fail(Error::kInventoryMismatch, draws[i].offset);
    }
  }
  if (!overlay_transactions.empty()) {
    plan->native_overlay_program_ = std::shared_ptr<const NativeOverlayProgram>(
        new NativeOverlayProgram(std::move(overlay_transactions)));
  }
  Result result;
  result.plan = std::move(plan);
  return result;
}

const char* NativeFrameCommandPlan::ErrorName(Error error) {
  switch (error) {
#define XE_FRAME_PLAN_ERROR(name, text) \
  case Error::name:                     \
    return text
    XE_FRAME_PLAN_ERROR(kNone, "none");
    XE_FRAME_PLAN_ERROR(kInvalidLimits, "invalid_limits");
    XE_FRAME_PLAN_ERROR(kInvalidByteCount, "invalid_byte_count");
    XE_FRAME_PLAN_ERROR(kLimitExceeded, "limit_exceeded");
    XE_FRAME_PLAN_ERROR(kTruncatedPacket, "truncated_packet");
    XE_FRAME_PLAN_ERROR(kUnsupportedPacket, "unsupported_packet");
    XE_FRAME_PLAN_ERROR(kUnsupportedRegister, "unsupported_register");
    XE_FRAME_PLAN_ERROR(kUnsupportedWait, "unsupported_wait");
    XE_FRAME_PLAN_ERROR(kInvalidShader, "invalid_shader");
    XE_FRAME_PLAN_ERROR(kInvalidMemoryRange, "invalid_memory_range");
    XE_FRAME_PLAN_ERROR(kInvalidDraw, "invalid_draw");
    XE_FRAME_PLAN_ERROR(kMissingLocalState, "missing_local_state");
    XE_FRAME_PLAN_ERROR(kUnsupportedSelector, "unsupported_selector");
    XE_FRAME_PLAN_ERROR(kInventoryMismatch, "inventory_mismatch");
    XE_FRAME_PLAN_ERROR(kNoDraw, "no_draw");
    XE_FRAME_PLAN_ERROR(kInvalidOverlayControl, "invalid_overlay_control");
#undef XE_FRAME_PLAN_ERROR
  }
  return "unknown";
}

}  // namespace xe::gpu
