/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_APU_AUDIO_MEMORY_DECODER_H_
#define XENIA_APU_AUDIO_MEMORY_DECODER_H_
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
namespace xe::apu {
// Fixed interleaved speaker slots, not channel count alone. SDL's six-channel
// sink is FL,FR,FC,LFE,BL,BR; its last pair may explicitly be SL,SR. No other
// native mask/custom layout is implicitly relabelled as this 5.1 sink.
enum class PcmChannelLayout {
  kUnknown,
  kMono,
  kStereo,
  kSurround51Back,
  kSurround51Side
};
enum class AudioChannelOrder {
  kUnknown,
  kUnspecified,
  kNative,
  kCustom,
  kAmbisonic
};
struct AudioFrameFormat {
  uint32_t frequency = 0;
  uint32_t channels = 0;
  AudioChannelOrder order = AudioChannelOrder::kUnknown;
  uint64_t native_mask = 0;
  bool operator==(const AudioFrameFormat&) const = default;
};
struct PcmAudio {
  uint32_t frequency = 0;
  uint32_t channels = 0;
  // Host-endian interleaved floats in the explicit layout below.
  std::vector<float> samples;
  PcmChannelLayout layout = PcmChannelLayout::kUnknown;
};
// Metadata gate used for every actual decoded AVFrame before sample copy.
// Unspecified mono/stereo use the standard single/stereo speaker convention;
// their source order remains part of identity and cannot change mid-stream.
class PcmFormatTracker {
 public:
  bool Accept(const AudioFrameFormat&, PcmAudio&);

 private:
  std::optional<AudioFrameFormat> source_;
};
bool PcmLayoutMatchesChannels(PcmChannelLayout, uint32_t channels);
struct AudioDecodeLimits {
  size_t maximum_input_bytes = 16 * 1024 * 1024;
  size_t maximum_sample_values = 16 * 1024 * 1024;
  uint32_t maximum_packets = 65536;
  std::chrono::milliseconds maximum_work_time{5000};
};
enum class AudioDecodeStatus {
  kSuccess,
  kInvalidInput,
  kUnsupported,
  kCorrupt,
  kLimitExceeded,
  kCancelled,
  kNoMemory
};
struct AudioDecodeResult {
  AudioDecodeStatus status = AudioDecodeStatus::kInvalidInput;
  int codec_error = 0;
  PcmAudio pcm;
};
// Complete bounded decode of one audio stream from owned input. Multiple
// audio tracks require a future explicit selector. Delayed frames are drained.
// A failure publishes no partial PCM. This service does not create a driver,
// start playback, touch guest state, or claim audibility.
AudioDecodeResult DecodeAudioMemory(
    std::vector<uint8_t> bytes, AudioDecodeLimits limits = {},
    const std::atomic<bool>* cancelled = nullptr);
}  // namespace xe::apu
#endif
