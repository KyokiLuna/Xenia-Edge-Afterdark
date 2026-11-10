/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_OVERLAY_H_
#define XENIA_GPU_NATIVE_OVERLAY_H_

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace xe::gpu {

// Host presentation metadata only. This grants no guest-memory, native-frame,
// GPU completion or visibility authority. Offscreen windows are legitimate;
// the presenter clips them while retaining the original UV rectangle.
struct NativeOverlayPresentation {
  int32_t canvas_x = 0, canvas_y = 0, x = 0, y = 0;
  uint32_t canvas_width = 0, canvas_height = 0, width = 0, height = 0;
  uint8_t opacity = 255;
  bool valid() const noexcept {
    constexpr uint32_t kMaxDimension = 16384;
    const auto axis = [](int32_t origin, uint32_t size) {
      const int64_t end = int64_t(origin) + int64_t(size);
      return end >= std::numeric_limits<int32_t>::min() &&
             end <= std::numeric_limits<int32_t>::max();
    };
    return canvas_width && canvas_height && width && height &&
           canvas_width <= kMaxDimension && canvas_height <= kMaxDimension &&
           width <= kMaxDimension && height <= kMaxDimension &&
           axis(canvas_x, canvas_width) && axis(canvas_y, canvas_height) &&
           axis(x, width) && axis(y, height);
  }
  bool operator==(const NativeOverlayPresentation&) const = default;
};

// Raw requested native register state, NOT hardware-active/scanout state.
// An unknown field must remain unknown; zero is not an observed reset value.
struct NativeOverlayState {
  enum class Field : uint8_t {
    kEnable,
    kControl1,
    kSurfaceAddress,
    kPitch,
    kOffsetX,
    kOffsetY,
    kStart,
    kEnd,
    kAlphaOutside,
    kAlphaInside,
    kAlphaControl,
  };
  static constexpr uint32_t kEnable = 1u << 0;
  static constexpr uint32_t kControl1 = 1u << 1;
  static constexpr uint32_t kSurfaceAddress = 1u << 2;
  static constexpr uint32_t kPitch = 1u << 3;
  static constexpr uint32_t kOffsetX = 1u << 4;
  static constexpr uint32_t kOffsetY = 1u << 5;
  static constexpr uint32_t kStart = 1u << 6;
  static constexpr uint32_t kEnd = 1u << 7;
  static constexpr uint32_t kAlphaOutside = 1u << 8;
  static constexpr uint32_t kAlphaInside = 1u << 9;
  static constexpr uint32_t kAlphaControl = 1u << 10;
  static constexpr uint32_t kAllKnown = (1u << 11) - 1;
  static constexpr uint32_t kGeometryKnown = kControl1 | kSurfaceAddress |
                                             kPitch | kOffsetX | kOffsetY |
                                             kStart | kEnd;
  static constexpr uint32_t kAlphaKnown =
      kAlphaOutside | kAlphaInside | kAlphaControl;
  uint32_t enable = 0, control1 = 0, surface_address = 0, pitch = 0;
  uint32_t offset_x = 0, offset_y = 0, start = 0, end = 0;
  uint32_t alpha_outside = 0, alpha_inside = 0, alpha_control = 0;
  uint32_t known_mask = 0;
  static constexpr uint32_t Mask(Field field) noexcept {
    return uint32_t(field) < 11 ? 1u << uint32_t(field) : 0;
  }
  bool Has(uint32_t mask) const noexcept { return (known_mask & mask) == mask; }
  static bool ValueAllowed(Field field, uint32_t value) noexcept {
    switch (field) {
      case Field::kEnable:
        return value <= 1;
      // Only the genuine observed RGBA native register profile is admitted.
      // Related desktop GPU depth/alpha layouts are not assumed for Xenos.
      case Field::kControl1:
        return value == 0;
      case Field::kSurfaceAddress:
        return value && value < 0x20000000 && !(value & 0xFFF);
      case Field::kPitch:
        return value && value <= 0x3FE0 && !(value & 31);
      case Field::kOffsetX:
        return !(value & ~UINT32_C(0x00000F00));
      case Field::kOffsetY:
        return !(value & ~UINT32_C(0x00000FFE));
      case Field::kStart:
      case Field::kEnd:
        return !(value & ~UINT32_C(0x0FFF0FFF));
      case Field::kAlphaOutside:
      case Field::kAlphaControl:
        return value == 0;
      case Field::kAlphaInside:
        return value == 255;
    }
    return false;
  }
  uint32_t Get(Field field) const noexcept {
    switch (field) {
      case Field::kEnable:
        return enable;
      case Field::kControl1:
        return control1;
      case Field::kSurfaceAddress:
        return surface_address;
      case Field::kPitch:
        return pitch;
      case Field::kOffsetX:
        return offset_x;
      case Field::kOffsetY:
        return offset_y;
      case Field::kStart:
        return start;
      case Field::kEnd:
        return end;
      case Field::kAlphaOutside:
        return alpha_outside;
      case Field::kAlphaInside:
        return alpha_inside;
      case Field::kAlphaControl:
        return alpha_control;
    }
    return 0;
  }
  void Set(Field field, uint32_t value) noexcept {
    switch (field) {
      case Field::kEnable:
        enable = value;
        break;
      case Field::kControl1:
        control1 = value;
        break;
      case Field::kSurfaceAddress:
        surface_address = value;
        break;
      case Field::kPitch:
        pitch = value;
        break;
      case Field::kOffsetX:
        offset_x = value;
        break;
      case Field::kOffsetY:
        offset_y = value;
        break;
      case Field::kStart:
        start = value;
        break;
      case Field::kEnd:
        end = value;
        break;
      case Field::kAlphaOutside:
        alpha_outside = value;
        break;
      case Field::kAlphaInside:
        alpha_inside = value;
        break;
      case Field::kAlphaControl:
        alpha_control = value;
        break;
    }
    known_mask |= Mask(field);
  }
  bool valid() const noexcept {
    if (known_mask & ~kAllKnown) {
      return false;
    }
    for (uint32_t i = 0; i < 11; ++i) {
      const auto field = Field(i);
      if (Has(Mask(field)) && !ValueAllowed(field, Get(field))) {
        return false;
      }
    }
    return true;
  }
  bool operator==(const NativeOverlayState&) const = default;
};

class NativeFrameCommandPlan;
// Immutable after complete packet-profile validation. Transactions describe
// native lock/write/unlock requests only; Apply does not execute, acknowledge,
// latch, display or grant authority to a physical address.
class NativeOverlayProgram {
 public:
  struct Write {
    uint32_t packet_dword_offset;
    NativeOverlayState::Field field;
    uint32_t value;
    bool operator==(const Write&) const = default;
  };
  struct Transaction {
    uint32_t lock_packet_dword_offset = 0;
    uint32_t unlock_packet_dword_offset = 0;
    std::vector<Write> writes;
    bool operator==(const Transaction&) const = default;
  };
  static constexpr uint32_t kMaxTransactions = 64;
  const std::vector<Transaction>& transactions() const noexcept {
    return transactions_;
  }
  NativeOverlayState Apply(const NativeOverlayState& initial) const noexcept {
    auto state = initial;
    for (const auto& transaction : transactions_) {
      for (const auto& write : transaction.writes) {
        state.Set(write.field, write.value);
      }
    }
    return state;
  }
  // Executor-local requested-state reduction. A transaction becomes requested
  // at its real unlock packet; none of these packets enter the title RF/MMIO.
  bool ApplyAtPacket(uint32_t offset,
                     NativeOverlayState& state) const noexcept {
    for (const auto& transaction : transactions_) {
      if (offset == transaction.unlock_packet_dword_offset) {
        for (const auto& write : transaction.writes) {
          state.Set(write.field, write.value);
        }
        return true;
      }
      if (offset == transaction.lock_packet_dword_offset) {
        return true;
      }
      for (const auto& write : transaction.writes) {
        if (offset == write.packet_dword_offset) {
          return true;
        }
      }
    }
    return false;
  }

 private:
  friend class NativeFrameCommandPlan;
  explicit NativeOverlayProgram(std::vector<Transaction> transactions)
      : transactions_(std::move(transactions)) {}
  const std::vector<Transaction> transactions_;
};

}  // namespace xe::gpu
#endif  // XENIA_GPU_NATIVE_OVERLAY_H_
