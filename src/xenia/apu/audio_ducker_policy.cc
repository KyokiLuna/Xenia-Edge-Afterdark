/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/apu/audio_ducker_policy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace xe::apu {
namespace {
constexpr uint64_t kNsPerSecond = 1000000000;
bool Unit(float value) {
  return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}
bool ValidScalar(AudioDuckerPolicy::Scalar scalar, float value) {
  if (!std::isfinite(value)) {
    return false;
  }
  switch (scalar) {
    case AudioDuckerPolicy::Scalar::kLevel:
      return Unit(value);
    case AudioDuckerPolicy::Scalar::kThreshold:
      return value >= 0.0f;
    case AudioDuckerPolicy::Scalar::kHold:
      return value >= 0.0f && value <= 3600.0f;
    case AudioDuckerPolicy::Scalar::kAttack:
    case AudioDuckerPolicy::Scalar::kRelease:
      return value >= 1.0f / 48000.0f && value <= 3600.0f;
  }
  return false;
}
}  // namespace

uint64_t AudioDuckerPolicy::SteadyNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
AudioDuckerPolicy::AudioDuckerPolicy(Clock clock, uint64_t owner_epoch)
    : clock_(clock ? clock : SteadyNowNs) {
  auto state = std::make_shared<Snapshot>();
  state->owner_epoch = owner_epoch;
  state->clock_anchor_ns = clock_();
  snapshot_.store(std::move(state));
}
uint64_t AudioDuckerPolicy::AudioTime(const Snapshot& state, uint64_t wall_ns) {
  uint64_t delta = !state.paused && wall_ns > state.clock_anchor_ns
                       ? wall_ns - state.clock_anchor_ns
                       : 0;
  return delta > UINT64_MAX - state.clock_value_ns
             ? UINT64_MAX
             : state.clock_value_ns + delta;
}
float AudioDuckerPolicy::ScalarValue(const Snapshot& state, Scalar scalar) {
  switch (scalar) {
    case Scalar::kLevel:
      return state.level;
    case Scalar::kThreshold:
      return state.threshold;
    case Scalar::kAttack:
      return state.attack;
    case Scalar::kRelease:
      return state.release;
    case Scalar::kHold:
      return state.hold;
  }
  return 1.0f;
}
float AudioDuckerPolicy::GainAt(const Snapshot& state, uint64_t audio_ns) {
  double alpha = 1.0;
  if (state.edge_duration_ns) {
    alpha = audio_ns <= state.edge_start_ns
                ? 0.0
                : std::min(double(audio_ns - state.edge_start_ns) /
                               state.edge_duration_ns,
                           1.0);
  }
  double q = state.q_start + (state.q_target - state.q_start) * alpha;
  return float(std::clamp(1.0 / q, double(kMinimumGain), 1.0));
}
void AudioDuckerPolicy::Reanchor(Snapshot& next, const Snapshot& old,
                                 uint64_t wall_ns) {
  next.clock_value_ns = AudioTime(old, wall_ns);
  next.clock_anchor_ns = wall_ns;
}
void AudioDuckerPolicy::Retarget(Snapshot& next, const Snapshot& old,
                                 uint64_t wall_ns) {
  const auto now = AudioTime(old, wall_ns);
  const double target = next.enabled ? std::max(next.level, kMinimumGain) : 1.0;
  if (1.0 / old.q_target == target) {
    return;
  }
  const double current = GainAt(old, now);
  next.q_start = 1.0 / current;
  next.q_target = 1.0 / target;
  next.edge_start_ns = now;
  next.edge_duration_ns = uint64_t(
      double(target < current ? next.attack : next.release) * kNsPerSecond);
}
AudioDuckerPolicy::Status AudioDuckerPolicy::SetEnabled(uint32_t enabled) {
  if (enabled > 1) {
    return Status::kInvalid;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return Status::kStopped;
  }
  const auto old = Read();
  if (old->paused) {
    return Status::kStopped;
  }
  if (old->enabled == bool(enabled)) {
    return Status::kSuccess;
  }
  if (old->revision == UINT64_MAX) {
    return Status::kOverflow;
  }
  try {
    auto next = std::make_shared<Snapshot>(*old);
    const auto now = clock_();
    next->enabled = bool(enabled);
    ++next->revision;
    Reanchor(*next, *old, now);
    Retarget(*next, *old, now);
    snapshot_.store(std::move(next));
    return Status::kSuccess;
  } catch (const std::bad_alloc&) {
    return Status::kNoMemory;
  }
}
AudioDuckerPolicy::Status AudioDuckerPolicy::SetScalar(Scalar scalar,
                                                       float value) {
  if (!ValidScalar(scalar, value)) {
    return Status::kInvalid;
  }
  if (value == 0.0f) {
    value = 0.0f;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return Status::kStopped;
  }
  const auto old = Read();
  if (old->paused) {
    return Status::kStopped;
  }
  if (ScalarValue(*old, scalar) == value) {
    return Status::kSuccess;
  }
  if (old->revision == UINT64_MAX) {
    return Status::kOverflow;
  }
  try {
    auto next = std::make_shared<Snapshot>(*old);
    switch (scalar) {
      case Scalar::kLevel:
        next->level = value;
        break;
      case Scalar::kThreshold:
        next->threshold = value;
        break;
      case Scalar::kAttack:
        next->attack = value;
        break;
      case Scalar::kRelease:
        next->release = value;
        break;
      case Scalar::kHold:
        next->hold = value;
        break;
    }
    ++next->revision;
    const auto now = clock_();
    Reanchor(*next, *old, now);
    if (scalar == Scalar::kLevel) {
      Retarget(*next, *old, now);
    }
    snapshot_.store(std::move(next));
    return Status::kSuccess;
  } catch (const std::bad_alloc&) {
    return Status::kNoMemory;
  }
}
AudioDuckerPolicy::Status AudioDuckerPolicy::SetCategory(uint32_t category,
                                                         float value) {
  if (category >= kCategoryCount || !Unit(value)) {
    return Status::kInvalid;
  }
  if (value == 0.0f) {
    value = 0.0f;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return Status::kStopped;
  }
  const auto old = Read();
  if (old->paused) {
    return Status::kStopped;
  }
  if (old->categories[category] == value) {
    return Status::kSuccess;
  }
  if (old->revision == UINT64_MAX) {
    return Status::kOverflow;
  }
  try {
    auto next = std::make_shared<Snapshot>(*old);
    next->categories[category] = value;
    next->category_versions[category] = ++next->revision;
    Reanchor(*next, *old, clock_());
    snapshot_.store(std::move(next));
    return Status::kSuccess;
  } catch (const std::bad_alloc&) {
    return Status::kNoMemory;
  }
}
void AudioDuckerPolicy::Stop() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
}
AudioDuckerPolicy::Status AudioDuckerPolicy::SetPaused(bool paused) {
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return Status::kStopped;
  }
  const auto old = Read();
  if (old->paused == paused) {
    return Status::kSuccess;
  }
  try {
    auto next = std::make_shared<Snapshot>(*old);
    Reanchor(*next, *old, clock_());
    next->paused = paused;
    snapshot_.store(std::move(next));
    return Status::kSuccess;
  } catch (const std::bad_alloc&) {
    return Status::kNoMemory;
  }
}
bool AudioDuckerPolicy::ApplyInterleaved(const Snapshot& state,
                                         uint64_t wall_start_ns,
                                         uint32_t frequency, uint32_t channels,
                                         uint64_t first_frame,
                                         float constant_gain,
                                         std::span<float> pcm) {
  if (!frequency || !channels || pcm.size() % channels ||
      !std::isfinite(constant_gain) || constant_gain < 0.0f) {
    return false;
  }
  const auto start = AudioTime(state, wall_start_ns);
  if (!ValidSaved(SavedState{state, start})) {
    return false;
  }
  const size_t frames = pcm.size() / channels;
  if (frames && first_frame > UINT64_MAX - (frames - 1)) {
    return false;
  }
  const auto offset_ns =
      [frequency](uint64_t frame) -> std::optional<uint64_t> {
    const uint64_t seconds = frame / frequency;
    const uint64_t remainder = frame % frequency;
    if (seconds > UINT64_MAX / kNsPerSecond) {
      return std::nullopt;
    }
    const uint64_t whole = seconds * kNsPerSecond;
    const uint64_t part = remainder * kNsPerSecond / frequency;
    if (whole > UINT64_MAX - part) {
      return std::nullopt;
    }
    return whole + part;
  };
  const auto end_ns = offset_ns(frames ? first_frame + frames - 1 : 0);
  if (!end_ns || *end_ns > UINT64_MAX - start) {
    return false;
  }
  for (size_t i = 0; i < frames; ++i) {
    const auto ns = *offset_ns(first_frame + i);
    const auto time = state.paused ? start : start + ns;
    const float gain = GainAt(state, time) * constant_gain;
    for (size_t c = 0; c < channels; ++c) {
      pcm[i * channels + c] *= gain;
    }
  }
  return true;
}
AudioDuckerPolicy::SavedState AudioDuckerPolicy::Save() const {
  auto state = Read();
  return {*state, AudioTime(*state, clock_())};
}
bool AudioDuckerPolicy::ValidSaved(const SavedState& saved) {
  const auto& s = saved.state;
  if (!s.revision || !ValidScalar(Scalar::kLevel, s.level) ||
      !ValidScalar(Scalar::kThreshold, s.threshold) ||
      !ValidScalar(Scalar::kAttack, s.attack) ||
      !ValidScalar(Scalar::kRelease, s.release) ||
      !ValidScalar(Scalar::kHold, s.hold) ||
      s.edge_start_ns > saved.audio_time_ns ||
      s.edge_duration_ns > 3600 * kNsPerSecond || !std::isfinite(s.q_start) ||
      !std::isfinite(s.q_target) || s.q_start < 1 ||
      s.q_start > 1.0 / kMinimumGain || s.q_target < 1 ||
      s.q_target > 1.0 / kMinimumGain) {
    return false;
  }
  const double target = s.enabled ? std::max(s.level, kMinimumGain) : 1.0;
  if (s.q_target != 1.0 / target ||
      (!s.edge_duration_ns && s.q_start != s.q_target)) {
    return false;
  }
  for (size_t i = 0; i < kCategoryCount; ++i) {
    if (!Unit(s.categories[i]) || !s.category_versions[i] ||
        s.category_versions[i] > s.revision) {
      return false;
    }
  }
  return true;
}
AudioDuckerPolicy::Status AudioDuckerPolicy::Restore(const SavedState& saved) {
  if (!ValidSaved(saved)) {
    return Status::kInvalid;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return Status::kStopped;
  }
  try {
    auto next = std::make_shared<Snapshot>(saved.state);
    next->owner_epoch = Read()->owner_epoch;
    next->clock_anchor_ns = clock_();
    next->clock_value_ns = saved.audio_time_ns;
    next->paused = true;
    snapshot_.store(std::move(next));
    return Status::kSuccess;
  } catch (const std::bad_alloc&) {
    return Status::kNoMemory;
  }
}
bool AudioDuckerPolicy::Encode(const SavedState& saved,
                               std::span<uint8_t> output) {
  if (output.size() != kSavedBytes || !ValidSaved(saved)) {
    return false;
  }
  size_t offset = 0;
  const auto write = [&](const auto& value) {
    std::memcpy(output.data() + offset, &value, sizeof(value));
    offset += sizeof(value);
  };
  write(uint32_t(1));
  write(uint32_t(saved.state.enabled));
  write(saved.state.revision);
  write(saved.state.level);
  write(saved.state.threshold);
  write(saved.state.attack);
  write(saved.state.release);
  write(saved.state.hold);
  write(saved.audio_time_ns);
  write(saved.state.edge_start_ns);
  write(saved.state.edge_duration_ns);
  write(saved.state.q_start);
  write(saved.state.q_target);
  for (size_t i = 0; i < kCategoryCount; ++i) {
    write(saved.state.categories[i]);
    write(saved.state.category_versions[i]);
  }
  return offset == output.size();
}
std::optional<AudioDuckerPolicy::SavedState> AudioDuckerPolicy::Decode(
    std::span<const uint8_t> input) {
  if (input.size() != kSavedBytes) {
    return std::nullopt;
  }
  SavedState saved;
  size_t offset = 0;
  const auto read = [&](auto& value) {
    std::memcpy(&value, input.data() + offset, sizeof(value));
    offset += sizeof(value);
  };
  uint32_t version, enabled;
  read(version);
  read(enabled);
  if (version != 1 || enabled > 1) {
    return std::nullopt;
  }
  saved.state.enabled = bool(enabled);
  read(saved.state.revision);
  read(saved.state.level);
  read(saved.state.threshold);
  read(saved.state.attack);
  read(saved.state.release);
  read(saved.state.hold);
  read(saved.audio_time_ns);
  read(saved.state.edge_start_ns);
  read(saved.state.edge_duration_ns);
  read(saved.state.q_start);
  read(saved.state.q_target);
  for (size_t i = 0; i < kCategoryCount; ++i) {
    read(saved.state.categories[i]);
    read(saved.state.category_versions[i]);
  }
  return ValidSaved(saved) ? std::optional(saved) : std::nullopt;
}
AudioCategoryDelivery::Pending AudioCategoryDelivery::Stage(
    const AudioDuckerPolicy::Snapshot& state) const {
  std::lock_guard lock(mutex_);
  Pending pending;
  pending.versions = state.category_versions;
  for (size_t i = 0; i < seen_.size(); ++i) {
    if (state.category_versions[i] > seen_[i]) {
      pending.mask |= uint32_t(1) << i;
    }
  }
  return pending;
}
void AudioCategoryDelivery::Acknowledge(const Pending& pending) {
  std::lock_guard lock(mutex_);
  for (size_t i = 0; i < seen_.size(); ++i) {
    seen_[i] = std::max(seen_[i], pending.versions[i]);
  }
}
AudioCategoryDelivery::Versions AudioCategoryDelivery::Save() const {
  std::lock_guard lock(mutex_);
  return seen_;
}
void AudioCategoryDelivery::Restore(const Versions& versions) {
  std::lock_guard lock(mutex_);
  seen_ = versions;
}
}  // namespace xe::apu
