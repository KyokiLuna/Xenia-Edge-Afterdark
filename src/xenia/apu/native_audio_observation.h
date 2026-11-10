/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_APU_NATIVE_AUDIO_OBSERVATION_H_
#define XENIA_APU_NATIVE_AUDIO_OBSERVATION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include "xenia/apu/audio_client_lifecycle.h"

namespace xe::apu {
class AudioSystem;

inline bool SameNativeAudioOwner(const AudioClientLifecycle::Identity& a,
                                const AudioClientLifecycle::Identity& b) {
  return a.owner_epoch == b.owner_epoch && a.generation == b.generation &&
         a.process_type == b.process_type && a.owner_hmodule == b.owner_hmodule &&
         a.owner_kind == b.owner_kind;
}

// Minted only from an actual PreparedClient. This retains copied identities,
// never a registration, driver, module-code lease or guest allocation.
class NativeAudioClientKey final {
 public:
  NativeAudioClientKey(const NativeAudioClientKey&) = delete;
  NativeAudioClientKey& operator=(const NativeAudioClientKey&) = delete;
  const AudioClientLifecycle::Identity& identity() const { return identity_; }
  size_t index() const { return index_; }
  uint32_t callback() const { return callback_; }
  uint32_t callback_arg() const { return callback_arg_; }
  uint32_t wrapped_callback_arg() const { return wrapped_callback_arg_; }

 private:
  friend class AudioSystem;
  friend class NativeAudioObservationTestAccess;
  NativeAudioClientKey(AudioClientLifecycle::Identity identity, size_t index,
                       uint32_t callback, uint32_t argument, uint32_t wrapper)
      : identity_(identity), index_(index), callback_(callback),
        callback_arg_(argument), wrapped_callback_arg_(wrapper) {}
  const AudioClientLifecycle::Identity identity_;
  const size_t index_;
  const uint32_t callback_, callback_arg_, wrapped_callback_arg_;
};

struct NativeAudioFrameLabel {
  uint64_t owner_epoch = 0, generation = 0, sequence = 0;
  bool operator==(const NativeAudioFrameLabel&) const = default;
};
struct NativeAudioBackendInfo {
  enum class Kind : uint32_t { kUnsupported, kSdl };
  Kind kind = Kind::kUnsupported;
  bool open = false, resumed = false;
  uint32_t frequency = 0, native_channels = 0, output_channels = 0;
  bool failed = false;
};

// Bounded copied-data observation only. Its configuration and synthetic-model
// use confer no native/UI authority. Actual attachment requires the opaque key
// and exact live registration in AudioSystem. No method invokes Kernel, guest,
// provider, driver, SDL or file APIs. Allocate Create outside their locks.
class NativeAudioObservation final {
 public:
  static constexpr uint32_t kMaximumFrames = 512;
  static constexpr uint32_t kSamplesPerChannel = 256;
  static constexpr uint32_t kNativeChannels = 6;
  static constexpr size_t kMaximumFrameBytes =
      kSamplesPerChannel * kNativeChannels * sizeof(float);
  static constexpr size_t kMaximumPcmBytes =
      kMaximumFrames * kMaximumFrameBytes * 2;
  static_assert(kMaximumPcmBytes == 6u * 1024u * 1024u);

  struct Frame {
    NativeAudioFrameLabel label;
    uint32_t native_bytes = 0, output_bytes = 0, output_channels = 0;
    bool sink_recorded = false, put_called = false, put_accepted = false;
    std::array<char, 192> error{};
    std::array<uint8_t, kMaximumFrameBytes> native_pcm{};
    std::array<uint8_t, kMaximumFrameBytes> output_pcm{};
  };
  struct Summary {
    AudioClientLifecycle::Identity identity{
        0, 0, 0, 0, AudioClientLifecycle::Identity::OwnerKind::kUnclassified};
    // Actual backend cached facts at attachment, not live device/audibility state.
    NativeAudioBackendInfo backend;
    uint64_t baseline_sequence = 0, last_sequence = 0;
    uint32_t queued_frames = 0, sink_frames = 0, accepted_frames = 0;
    uint32_t pending_frames = 0;
    bool attached = false, closed = false, capped = false, canceled = false;
    bool retired = false, failed = false, unsupported = false;
    bool sequence_gap = false, taken = false;
  };
  struct Result {
    Summary summary;
    uint32_t frame_count = 0;
    // Take moves this allocation; native/output PCM are never duplicated.
    std::unique_ptr<Frame[]> frames;
  };

  static std::shared_ptr<NativeAudioObservation> Create(
      const AudioClientLifecycle::Identity& expected) {
    if (!expected.owner_epoch || !expected.generation ||
        !expected.process_type || !expected.owner_hmodule ||
        expected.owner_kind != AudioClientLifecycle::Identity::OwnerKind::kModule) {
      return {};
    }
    return std::shared_ptr<NativeAudioObservation>(
        new NativeAudioObservation(expected));
  }
  NativeAudioObservation(const NativeAudioObservation&) = delete;
  NativeAudioObservation& operator=(const NativeAudioObservation&) = delete;

  Summary Peek() const {
    std::lock_guard lock(mutex_);
    return summary_;
  }
  // Normal detach closes new ingress but permits receipts for already queued
  // labelled blocks. It never flushes/stops a continuous native stream.
  void Close() noexcept try {
    std::lock_guard lock(mutex_);
    summary_.closed = true;
  } catch (...) {}
  void Cancel() noexcept try {
    std::lock_guard lock(mutex_);
    summary_.closed = summary_.canceled = true;
  } catch (...) {}
  void Retire() noexcept try {
    std::lock_guard lock(mutex_);
    summary_.closed = summary_.retired = true;
  } catch (...) {}
  void Fail() noexcept try {
    std::lock_guard lock(mutex_);
    summary_.closed = summary_.failed = true;
  } catch (...) {}
  void Unsupported() noexcept try {
    std::lock_guard lock(mutex_);
    summary_.closed = summary_.failed = summary_.unsupported = true;
  } catch (...) {}
  std::unique_ptr<Result> Take() {
    auto result = std::make_unique<Result>();  // Called outside audio locks.
    std::lock_guard lock(mutex_);
    if (summary_.taken) { return {}; }
    summary_.closed = summary_.taken = true;
    result->summary = summary_;
    result->frame_count = summary_.queued_frames;
    result->frames = std::move(frames_);
    return result;
  }

  // Caller supplies the SAME owned native block that entered the real queue.
  // No guest pointer is read here. Failure never changes playback or its queue.
  bool RecordQueued(const NativeAudioFrameLabel& label,
                    std::span<const uint8_t> native_pcm) noexcept try {
    std::lock_guard lock(mutex_);
    if (!summary_.attached || summary_.closed || summary_.taken) { return false; }
    if (!Owns(label) || !label.sequence ||
        summary_.last_sequence == UINT64_MAX ||
        label.sequence != summary_.last_sequence + 1) {
      SequenceFailure();
      return false;
    }
    if (native_pcm.size() != kMaximumFrameBytes || !native_pcm.data()) {
      summary_.closed = summary_.failed = true;
      return false;
    }
    auto& frame = frames_[summary_.queued_frames];
    frame.label = label;
    frame.native_bytes = uint32_t(native_pcm.size());
    std::memcpy(frame.native_pcm.data(), native_pcm.data(), native_pcm.size());
    ++summary_.queued_frames;
    ++summary_.pending_frames;
    summary_.last_sequence = label.sequence;
    if (summary_.queued_frames == kMaximumFrames) {
      summary_.closed = summary_.capped = true;
    }
    return true;
  } catch (...) { return false; }

  // The scratch is owned by this real SDL callback and is copied before it is
  // reused. Put success describes API acceptance only, never audibility.
  bool RecordSink(const NativeAudioFrameLabel& label,
                  std::span<const uint8_t> output_pcm, uint32_t channels,
                  bool put_called, bool put_accepted,
                  const char* error = nullptr) noexcept try {
    std::lock_guard lock(mutex_);
    if (!summary_.attached || summary_.taken || summary_.canceled ||
        summary_.retired) { return false; }
    const uint64_t next_sink = uint64_t(summary_.sink_frames) + 1;
    if (summary_.baseline_sequence > UINT64_MAX - next_sink ||
        label.sequence != summary_.baseline_sequence + next_sink) {
      SequenceFailure();
      return false;
    }
    Frame* frame = nullptr;
    for (uint32_t i = 0; i != summary_.queued_frames; ++i) {
      if (frames_[i].label == label) { frame = &frames_[i]; break; }
    }
    if (!Owns(label) || !frame || frame->sink_recorded) {
      SequenceFailure();
      return false;
    }
    if ((channels != 2 && channels != kNativeChannels) ||
        channels != summary_.backend.output_channels ||
        output_pcm.size() != kSamplesPerChannel * channels * sizeof(float) ||
        !output_pcm.data() || (put_accepted && !put_called)) {
      summary_.closed = summary_.failed = true;
      return false;
    }
    frame->output_bytes = uint32_t(output_pcm.size());
    frame->output_channels = channels;
    frame->sink_recorded = true;
    frame->put_called = put_called;
    frame->put_accepted = put_accepted;
    std::memcpy(frame->output_pcm.data(), output_pcm.data(), output_pcm.size());
    if (error) {
      size_t i = 0;
      for (; i + 1 < frame->error.size() && error[i]; ++i) {
        frame->error[i] = error[i];
      }
      frame->error[i] = 0;
    }
    ++summary_.sink_frames;
    --summary_.pending_frames;
    if (put_called && put_accepted) { ++summary_.accepted_frames; }
    else { summary_.closed = summary_.failed = true; }
    return true;
  } catch (...) { return false; }

 private:
  friend class AudioSystem;
  friend class NativeAudioObservationTestAccess;
  // Origin rejection races normal Close/cap on other threads. Only an open
  // admission window may be poisoned by a later unrelated submission.
  void FailIfOpen() noexcept try {
    std::lock_guard lock(mutex_);
    if (!summary_.closed) { summary_.closed = summary_.failed = true; }
  } catch (...) {}
  explicit NativeAudioObservation(AudioClientLifecycle::Identity expected)
      : frames_(std::make_unique<Frame[]>(kMaximumFrames)) {
    summary_.identity = expected;
  }
  bool Bind(const NativeAudioClientKey& key, uint64_t baseline,
            NativeAudioBackendInfo backend) noexcept try {
    std::lock_guard lock(mutex_);
    if (summary_.attached || summary_.closed || summary_.taken ||
        !SameNativeAudioOwner(summary_.identity, key.identity())) { return false; }
    summary_.attached = true;
    summary_.baseline_sequence = summary_.last_sequence = baseline;
    summary_.backend = backend;
    if (baseline == UINT64_MAX) { SequenceFailure(); }
    if (backend.kind != NativeAudioBackendInfo::Kind::kSdl || backend.failed ||
        !backend.open ||
        !backend.resumed || backend.frequency != 48000 ||
        backend.native_channels != kNativeChannels ||
        (backend.output_channels != 2 && backend.output_channels != kNativeChannels)) {
      summary_.closed = summary_.failed = summary_.unsupported = true;
    }
    return true;  // Binding is distinct from backend success.
  } catch (...) { return false; }
  bool Owns(const NativeAudioFrameLabel& label) const {
    return label.owner_epoch == summary_.identity.owner_epoch &&
           label.generation == summary_.identity.generation;
  }
  void SequenceFailure() {
    summary_.closed = summary_.failed = summary_.sequence_gap = true;
  }
  mutable std::mutex mutex_;
  Summary summary_;
  std::unique_ptr<Frame[]> frames_;
};
}  // namespace xe::apu
#endif  // XENIA_APU_NATIVE_AUDIO_OBSERVATION_H_
