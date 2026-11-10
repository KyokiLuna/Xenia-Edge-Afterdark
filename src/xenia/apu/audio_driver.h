/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_AUDIO_DRIVER_H_
#define XENIA_APU_AUDIO_DRIVER_H_

#include <span>

#include "xenia/apu/native_audio_observation.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

class AudioDriver {
 public:
  static constexpr uint32_t kFrameFrequencyDefault = 48000;
  static constexpr uint32_t kFrameChannelsDefault = 6;
  static constexpr uint32_t kChannelSamplesDefault = 256;
  static constexpr uint32_t kFrameSamplesMax =
      kFrameChannelsDefault * kChannelSamplesDefault;
  static constexpr uint32_t kFrameSizeMax = sizeof(float) * kFrameSamplesMax;

  virtual ~AudioDriver();

  enum class StreamState {
    kUnsupported,
    kAccepting,
    kDraining,
    kDrained,
    kFailed
  };
  struct StreamStatus {
    StreamState state = StreamState::kUnsupported;
    uint64_t accepted_frames = 0;
  };
  // Checked opt-in protocol for an independent finite stream. Existing guest
  // SubmitFrame remains unchanged. Drained means handed to the host backend,
  // never proof of DAC output/audibility. Silent drivers keep unsupported.
  virtual bool SupportsIndependentStream() const { return false; }
  virtual bool SubmitIndependentFrame(std::span<const float>) { return false; }
  virtual bool FinishIndependentStream() { return false; }
  virtual bool SetIndependentPaused(bool) { return false; }
  virtual StreamStatus QueryIndependentStream() { return {}; }

  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;

  virtual void SubmitFrame(float* samples) = 0;
  // Registered-client diagnostic only. Unsupported drivers preserve their
  // ordinary submission behavior without manufacturing PCM/SDL acceptance.
  virtual NativeAudioBackendInfo NativeObservationBackend() const { return {}; }
  virtual void SubmitFrameObserved(
      float* samples, NativeAudioFrameLabel,
      const std::shared_ptr<NativeAudioObservation>& observation) {
    if (observation) { observation->Unsupported(); }
    SubmitFrame(samples);
  }
  virtual void Pause() = 0;
  virtual void Resume() = 0;
  virtual void SetVolume(float volume) = 0;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_AUDIO_DRIVER_H_
