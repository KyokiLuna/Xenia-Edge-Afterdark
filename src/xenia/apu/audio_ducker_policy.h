/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_APU_AUDIO_DUCKER_POLICY_H_
#define XENIA_APU_AUDIO_DUCKER_POLICY_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace xe::apu {

// Explicit local virtual-audio policy, not recovered retail kernel defaults.
// Native XAudio owns detection, hold and voice/category multiplication. This
// object owns one common attenuation curve and clock for every host stream.
class AudioDuckerPolicy final {
 public:
  static constexpr size_t kCategoryCount = 32;
  static constexpr float kCompensationCap = 251.18866f;
  static constexpr float kMinimumGain = 1.0f / kCompensationCap;
  static constexpr size_t kSavedBytes = 460;
  enum class Scalar { kLevel, kThreshold, kAttack, kRelease, kHold };
  enum class Status { kSuccess, kInvalid, kStopped, kNoMemory, kOverflow };
  using Clock = uint64_t (*)();

  struct Snapshot {
    uint64_t owner_epoch = 1;
    bool enabled = false;
    float level = 1.0f, threshold = 1.0f;
    float attack = 0.010f, release = 0.250f, hold = 0.050f;  // Seconds.
    uint64_t revision = 1;
    std::array<float, kCategoryCount> categories;
    std::array<uint64_t, kCategoryCount> category_versions;
    uint64_t clock_anchor_ns = 0, clock_value_ns = 0;
    bool paused = false;
    uint64_t edge_start_ns = 0, edge_duration_ns = 0;
    double q_start = 1.0, q_target = 1.0;
    Snapshot() {
      categories.fill(1.0f);
      category_versions.fill(1);
    }
  };
  // Host identities and wall-clock anchors are never serialized.
  struct SavedState {
    Snapshot state;
    uint64_t audio_time_ns = 0;
  };

  explicit AudioDuckerPolicy(Clock clock = SteadyNowNs,
                             uint64_t owner_epoch = 1);
  std::shared_ptr<const Snapshot> Read() const { return snapshot_.load(); }
  uint64_t NowNs() const { return clock_(); }
  Status SetEnabled(uint32_t enabled);
  Status SetScalar(Scalar scalar, float value);
  Status SetCategory(uint32_t category, float value);
  void Stop();
  Status SetPaused(bool paused);
  SavedState Save() const;
  Status Restore(const SavedState& saved);

  static uint64_t SteadyNowNs();
  static uint64_t AudioTime(const Snapshot& state, uint64_t wall_ns);
  static float ScalarValue(const Snapshot& state, Scalar scalar);
  static float GainAt(const Snapshot& state, uint64_t audio_ns);
  // Shared production PCM operation. A callback supplies one actual time and
  // immutable snapshot; no per-client envelope or mutable sample cursor exists.
  static bool ApplyInterleaved(const Snapshot& state, uint64_t wall_start_ns,
                               uint32_t frequency, uint32_t channels,
                               uint64_t first_frame, float constant_gain,
                               std::span<float> pcm);
  static bool Encode(const SavedState& saved, std::span<uint8_t> output);
  static std::optional<SavedState> Decode(std::span<const uint8_t> input);

 private:
  static bool ValidSaved(const SavedState& saved);
  static void Reanchor(Snapshot& next, const Snapshot& old, uint64_t wall_ns);
  static void Retarget(Snapshot& next, const Snapshot& old, uint64_t wall_ns);
  const Clock clock_;
  std::mutex mutex_;
  bool stopped_ = false;
  std::atomic<std::shared_ptr<const Snapshot>> snapshot_;
};

// One real registration's delivery history, not a globally consumed mask.
class AudioCategoryDelivery final {
 public:
  using Versions = std::array<uint64_t, AudioDuckerPolicy::kCategoryCount>;
  struct Pending {
    uint32_t mask = 0;
    Versions versions{};
  };
  Pending Stage(const AudioDuckerPolicy::Snapshot& state) const;
  void Acknowledge(const Pending& pending);
  Versions Save() const;
  void Restore(const Versions& versions);

 private:
  mutable std::mutex mutex_;
  Versions seen_{};
};

}  // namespace xe::apu
#endif  // XENIA_APU_AUDIO_DUCKER_POLICY_H_
