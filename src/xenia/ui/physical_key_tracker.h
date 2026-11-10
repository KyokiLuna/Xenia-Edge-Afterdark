/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_UI_PHYSICAL_KEY_TRACKER_H_
#define XENIA_UI_PHYSICAL_KEY_TRACKER_H_
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include "xenia/ui/virtual_key.h"
namespace xe::ui {
// Bounded host key bookkeeping, not native input authority. X11 hardware codes
// are 8-bit. Preserve a press's original VK and ID until its real release;
// multiple sources of one VK form one logical held key.
class PhysicalKeyTracker {
 public:
  using Bitmap = std::array<uint8_t, 32>;
  enum class State { kUnknown, kReleased, kPressed };
  struct Edge {
    VirtualKey key = VirtualKey::kNone;
    uint64_t id = 0;
    bool physical_repeat = false;
    bool logical_repeat = false;
    bool deliver = false;
  };
  Edge Down(uint32_t code, VirtualKey translated) {
    if (!code || code >= held_.size()) {
      return {};
    }
    if (auto& held = held_[code]) {
      if (held->key == VirtualKey::kNone) {
        const auto vk = static_cast<uint16_t>(translated);
        if (!vk || vk >= known_.size()) {
          return {};
        }
        held->key = translated;
        known_[vk].set(code);
      }
      held->physical_repeat = true;
      held->logical_repeat = true;
      return {held->key, held->id, true, true, true};
    }
    const auto vk = static_cast<uint16_t>(translated);
    if (!vk || vk >= known_.size() || next_id_ == UINT64_MAX) {
      return {};
    }
    const bool logical_repeat = IsHeld(translated);
    held_[code] = Press{translated, next_id_++, false, logical_repeat};
    known_[vk].set(code);
    return {translated, held_[code]->id, false, logical_repeat, true};
  }
  Edge Source(uint32_t code) const {
    if (!code || code >= held_.size() || !held_[code]) {
      return {};
    }
    const auto& held = *held_[code];
    return {held.key, held.id, held.physical_repeat, held.logical_repeat, true};
  }
  Edge Up(uint32_t code) {
    if (!code || code >= held_.size() || !held_[code]) {
      return {};
    }
    const auto held = *held_[code];
    held_[code].reset();
    return {held.key, held.id, false, false, !IsHeld(held.key)};
  }
  bool IsHeld(VirtualKey key) const {
    for (const auto& held : held_) {
      if (held && held->key == key) {
        return true;
      }
    }
    return false;
  }
  void Reconcile(const Bitmap& keys) {
    for (size_t code = 1; code < held_.size(); ++code) {
      if (!Pressed(keys, code)) {
        held_[code].reset();
      } else if (!held_[code] && next_id_ != UINT64_MAX) {
        // A key pressed outside our focus must remain a held/repeat source,
        // even though its VK was never observed by this window.
        held_[code] = Press{VirtualKey::kNone, next_id_++, true, true};
      }
    }
  }
  void Clear() {
    for (auto& held : held_) {
      held.reset();
    }
  }
  State Classify(VirtualKey key, const Bitmap& keys) const {
    const auto vk = static_cast<uint16_t>(key);
    if (!vk || vk >= known_.size() || known_[vk].none()) {
      return State::kUnknown;
    }
    for (size_t code = 1; code < held_.size(); ++code) {
      if (!known_[vk].test(code)) {
        continue;
      }
      // A genuinely released/reused physical source now belongs to its NEW VK.
      // A held source can never reach this branch through a translation change.
      if (held_[code] && held_[code]->key != VirtualKey::kNone &&
          held_[code]->key != key) {
        continue;
      }
      if (Pressed(keys, code)) {
        return State::kPressed;
      }
    }
    return State::kReleased;
  }

 private:
  struct Press {
    VirtualKey key;
    uint64_t id;
    bool physical_repeat;
    bool logical_repeat;
  };
  static bool Pressed(const Bitmap& keys, size_t code) {
    return (keys[code >> 3] & (1u << (code & 7))) != 0;
  }
  std::array<std::optional<Press>, 256> held_{};
  std::array<std::bitset<256>, 256> known_{};
  uint64_t next_id_ = 1;
};
}  // namespace xe::ui
#endif  // XENIA_UI_PHYSICAL_KEY_TRACKER_H_
