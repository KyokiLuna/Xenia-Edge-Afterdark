/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_FRAME_COMMAND_PLAN_H_
#define XENIA_GPU_NATIVE_FRAME_COMMAND_PLAN_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/native_overlay.h"
#include "xenia/gpu/native_system_title_input.h"

namespace xe::gpu {

// Complete bounded CPU packet-profile validation, separate from the older
// NativeCommandStream profile. Acceptance is NEVER execution permission.
// Selected immutable shader bytes/semantics, used constants/input bounds,
// allocation/module leases, targets/copy destinations and isolated backend
// execution must be proven separately against this exact owned primary.
class NativeFrameCommandPlan {
 public:
  enum class Kind { kRenderedOutput, kOverlayControl };
  Kind kind() const noexcept { return kind_; }
  bool is_overlay_control() const noexcept {
    return kind_ == Kind::kOverlayControl;
  }
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
    kInvalidMemoryRange,
    kInvalidDraw,
    kMissingLocalState,
    kUnsupportedSelector,
    kInventoryMismatch,
    kNoDraw,
    kInvalidOverlayControl,
  };
  enum class PacketKind {
    kRegisterWrite,
    kNop,
    kCoherencyWait,
    kShaderLoad,
    kInlineShader,
    kShaderInvalidation,
    kDraw,
    kCacheFlush,
    kNativeOverlayControl,
  };
  struct Limits {
    uint32_t max_dwords = 0x10000;
    uint32_t max_packets = 4096;
    uint32_t max_draws = 256;
    uint32_t max_shader_loads = 512;
    uint32_t max_shader_dwords = 4096;
  };
  struct Packet {
    PacketKind kind;
    uint32_t dword_offset;
    uint32_t dword_count;  // Header included.
  };
  struct RegisterWrite {
    uint32_t packet_dword_offset;
    uint32_t index;
    uint32_t value;
  };
  struct CoherencyWait {
    uint32_t packet_dword_offset;
    uint32_t status;
    uint32_t base;
    uint32_t size;
  };
  struct Result {
    Error error = Error::kNone;
    uint32_t error_dword_offset = UINT32_MAX;
    uint32_t error_register = UINT32_MAX;
    std::shared_ptr<const NativeFrameCommandPlan> plan;
    bool execution_ready = false;
    bool shader_semantics_verified = false;
    bool ownership_verified = false;
    explicit operator bool() const {
      return error == Error::kNone && bool(plan);
    }
  };

  static Result Analyze(std::span<const uint8_t> big_endian_primary);
  static Result Analyze(std::span<const uint8_t> big_endian_primary,
                        const Limits& limits);
  // Production initial state is a completed CP-minted input, never a caller
  // bitmap, zero reset image or borrowed Title RF.
  static Result AnalyzeWithTitleInput(
      std::span<const uint8_t> big_endian_primary,
      std::shared_ptr<const NativeSystemTitleInput> title_input);
  const std::shared_ptr<const NativeSystemTitleInput>& title_input() const {
    return title_input_;
  }
  static const char* ErrorName(Error error);

  const std::vector<uint32_t>& words() const { return words_; }
  const std::vector<Packet>& packets() const { return packets_; }
  const std::vector<RegisterWrite>& register_writes() const {
    return register_writes_;
  }
  const std::vector<CoherencyWait>& coherency_waits() const {
    return coherency_waits_;
  }
  const NativeDrawReadInventory::ScanResult& inventory() const {
    return inventory_;
  }
  bool has_final_copy() const { return final_copy_; }
  bool has_cache_flush() const { return cache_flush_; }
  const std::shared_ptr<const NativeOverlayProgram>& native_overlay_program()
      const noexcept {
    return native_overlay_program_;
  }
  std::optional<uint32_t> final_copy_packet_dword_offset() const noexcept {
    return final_copy_packet_dword_offset_;
  }

 private:
  NativeFrameCommandPlan() = default;
  static Result AnalyzeWithInput(std::span<const uint8_t>, const Limits&,
                                 std::shared_ptr<const NativeSystemTitleInput>);
  std::shared_ptr<const NativeSystemTitleInput> title_input_;
  std::vector<uint32_t> words_;
  std::vector<Packet> packets_;
  std::vector<RegisterWrite> register_writes_;
  std::vector<CoherencyWait> coherency_waits_;
  NativeDrawReadInventory::ScanResult inventory_;
  std::shared_ptr<const NativeOverlayProgram> native_overlay_program_;
  std::optional<uint32_t> final_copy_packet_dword_offset_;
  Kind kind_ = Kind::kRenderedOutput;
  bool final_copy_ = false;
  bool cache_flush_ = false;
};

}  // namespace xe::gpu
#endif  // XENIA_GPU_NATIVE_FRAME_COMMAND_PLAN_H_
