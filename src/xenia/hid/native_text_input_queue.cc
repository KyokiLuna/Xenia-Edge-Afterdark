/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/native_text_input_queue.h"

#include <algorithm>
#include "xenia/base/clock.h"

namespace xe::hid {

void NativeTextInputQueue::InvalidateLocked() {
  pending_.clear();
  down_.clear();
  committed_.clear();
  if (epoch_ == UINT64_MAX) {
    poisoned_ = true;
  } else {
    ++epoch_;
  }
}

bool NativeTextInputQueue::EnabledLocked() const {
  return generation_ && revision_ && user_ < XUserMaxUserCount && enabled_ &&
         focused_ && !modal_ && !poisoned_;
}

void NativeTextInputQueue::SetGeneration(uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (generation_ == generation) {
    return;
  }
  InvalidateLocked();
  generation_ = generation;
  revision_ = revision_watermark_ = 0;
  user_ = UINT32_MAX;
  enabled_ = false;
  last_input_time_ = 0;
}

bool NativeTextInputQueue::SetOwner(uint64_t generation, uint64_t revision,
                                    uint32_t user, bool enabled) {
  std::lock_guard lock(mutex_);
  if (!generation || generation != generation_ || poisoned_) {
    return false;
  }
  const bool clear = !revision && user == UINT32_MAX && !enabled;
  if (!clear && (!revision || user >= XUserMaxUserCount)) {
    return false;
  }
  if (!clear && (revision < revision_watermark_ ||
                 (revision == revision_watermark_ &&
                  (revision != revision_ || user != user_)))) {
    return false;
  }
  if (revision_ == revision && user_ == user && enabled_ == enabled) {
    return true;
  }
  const bool new_owner = revision_ != revision || user_ != user;
  InvalidateLocked();
  revision_ = revision;
  revision_watermark_ = std::max(revision_watermark_, revision);
  user_ = user;
  enabled_ = enabled;
  if (new_owner) {
    last_input_time_ = 0;
  }
  return !poisoned_;
}

void NativeTextInputQueue::SetFocused(bool focused) {
  std::lock_guard lock(mutex_);
  if (focused_ == focused) {
    return;
  }
  InvalidateLocked();
  focused_ = focused;
}

void NativeTextInputQueue::SetHostModal(uint64_t generation, bool blocked) {
  std::lock_guard lock(mutex_);
  if (generation != generation_ || modal_ == blocked) {
    return;
  }
  InvalidateLocked();
  modal_ = blocked;
}

NativeTextState NativeTextInputQueue::State(uint64_t generation) const {
  std::lock_guard lock(mutex_);
  if (!generation || generation != generation_) {
    return {};
  }
  NativeTextState state;
  state.available = !poisoned_;
  state.focused = focused_;
  state.enabled = EnabledLocked();
  state.owner_revision = revision_;
  state.user = user_;
  state.invalidation_epoch = epoch_;
  state.last_input_time = last_input_time_;
  return state;
}

void NativeTextInputQueue::Reset(uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (generation && generation == generation_) {
    InvalidateLocked();
  }
}

void NativeTextInputQueue::PushKey(uint16_t vk, uint16_t modifiers, uint8_t hid,
                                   bool down, bool repeat, bool wait,
                                   uint64_t source) {
  std::lock_guard lock(mutex_);
  if (!EnabledLocked() || !vk || repeat) {
    return;
  }
  if (down) {
    if (down_.contains(vk) || (wait && !source)) {
      return;
    }
    if (pending_.size() >= kMaxPendingEdges ||
        down_.size() >= kMaxPendingEdges) {
      InvalidateLocked();
      return;
    }
    down_.emplace(vk, source);
  } else {
    auto held = down_.find(vk);
    if (held == down_.end()) {
      return;
    }
    down_.erase(held);
    if (!committed_.erase(vk)) {
      std::erase_if(pending_, [vk](const Pending& p) {
        return !p.ready && uint16_t(p.stroke.virtual_key) == vk;
      });
      return;
    }
    if (pending_.size() >= kMaxPendingEdges) {
      InvalidateLocked();
      return;
    }
  }
  X_INPUT_KEYSTROKE stroke{};
  stroke.virtual_key = vk;
  stroke.flags =
      modifiers | (down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP);
  stroke.user_index = uint8_t(user_);
  stroke.hid_code = hid;
  const bool ready = !down || !wait;
  pending_.push_back({stroke, source, ready});
  if (down && ready) {
    committed_.insert(vk);
  }
  if (ready) {
    last_input_time_ = Clock::QueryGuestInterruptTime();
  }
}

bool NativeTextInputQueue::CommitText(uint64_t source, std::u32string_view text,
                                      bool repeat, bool composed) {
  std::lock_guard lock(mutex_);
  if (!EnabledLocked() || !source || repeat) {
    return false;
  }
  auto found = std::find_if(pending_.begin(), pending_.end(),
                            [source](const Pending& p) {
                              return !p.ready && p.source_key == source;
                            });
  if (found == pending_.end()) {
    return false;
  }
  if (composed || text.size() != 1 || text[0] < 0x20 || text[0] == 0x7F ||
      text[0] > 0xFFFF || (text[0] >= 0xD800 && text[0] <= 0xDFFF)) {
    InvalidateLocked();
    return false;
  }
  found->stroke.unicode = uint16_t(text[0]);
  found->stroke.flags = uint16_t(found->stroke.flags) | 0x1000;
  found->ready = true;
  committed_.insert(uint16_t(found->stroke.virtual_key));
  last_input_time_ = Clock::QueryGuestInterruptTime();
  return true;
}

X_RESULT NativeTextInputQueue::Pop(uint64_t generation, uint32_t user,
                                   X_INPUT_KEYSTROKE* output) {
  if (!output || user >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard lock(mutex_);
  if (!generation || generation != generation_ || !EnabledLocked() ||
      user != user_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (pending_.empty() || !pending_.front().ready) {
    return X_ERROR_EMPTY;
  }
  *output = pending_.front().stroke;
  pending_.pop_front();
  return X_ERROR_SUCCESS;
}

}  // namespace xe::hid
