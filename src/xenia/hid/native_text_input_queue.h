/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NATIVE_TEXT_INPUT_QUEUE_H_
#define XENIA_HID_NATIVE_TEXT_INPUT_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "xenia/hid/input_driver.h"

namespace xe::hid {

// Native XAM owns repeats. Whole BMP commits are finalized against their exact
// physical press ID before readers can see them. Native audience invalidation
// uses the non-consuming state epoch; the Kernel tracks each bank separately.
class NativeTextInputQueue {
 public:
  static constexpr size_t kMaxPendingEdges = 512;
  void SetGeneration(uint64_t generation);
  bool SetOwner(uint64_t generation, uint64_t revision, uint32_t user,
                bool enabled);
  void SetFocused(bool focused);
  void SetHostModal(uint64_t generation, bool blocked);
  NativeTextState State(uint64_t generation) const;
  void Reset(uint64_t generation);
  void PushKey(uint16_t virtual_key, uint16_t modifiers, uint8_t hid_code,
               bool down, bool host_repeat, bool wait_for_character = false,
               uint64_t source_key = 0);
  bool CommitText(uint64_t source_key, std::u32string_view text,
                  bool host_repeat = false, bool composed = false);
  X_RESULT Pop(uint64_t generation, uint32_t user_index,
               X_INPUT_KEYSTROKE* output);

 private:
  void InvalidateLocked();
  bool EnabledLocked() const;
  mutable std::mutex mutex_;
  uint64_t generation_ = 0, revision_ = 0, revision_watermark_ = 0;
  uint64_t epoch_ = 1, last_input_time_ = 0;
  uint32_t user_ = UINT32_MAX;
  bool enabled_ = false, focused_ = false, modal_ = false, poisoned_ = false;
  struct Pending {
    X_INPUT_KEYSTROKE stroke;
    uint64_t source_key;
    bool ready;
  };
  std::deque<Pending> pending_;
  std::unordered_map<uint16_t, uint64_t> down_;
  std::unordered_set<uint16_t> committed_;
};

}  // namespace xe::hid
#endif  // XENIA_HID_NATIVE_TEXT_INPUT_QUEUE_H_
