/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_
#define XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>

#include <SDL3/SDL.h>
#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
class AudioDuckerPolicy;
namespace sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(xe::threading::Semaphore* semaphore,
                 uint32_t frequency = kFrameFrequencyDefault,
                 uint32_t channels = kFrameChannelsDefault,
                 bool need_format_conversion = true,
                 std::shared_ptr<AudioDuckerPolicy> policy = {});
  ~SDLAudioDriver() override;

  bool Initialize() override;
  void SubmitFrame(float* frame) override;
  NativeAudioBackendInfo NativeObservationBackend() const override;
  void SubmitFrameObserved(
      float* frame, NativeAudioFrameLabel label,
      const std::shared_ptr<NativeAudioObservation>& observation) override;
  bool SupportsIndependentStream() const override { return true; }
  bool SubmitIndependentFrame(std::span<const float> frame) override;
  bool FinishIndependentStream() override;
  bool SetIndependentPaused(bool paused) override;
  StreamStatus QueryIndependentStream() override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override { volume_ = volume; };
  void Shutdown() override;

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream,
                          int additional_amount, int total_amount);

  bool QueueFrame(const float* frame, bool independent,
                  NativeAudioFrameLabel label = {},
                  std::shared_ptr<NativeAudioObservation> observation = {});
  // Protected by frames_mutex_; SDL stream calls take SDL's stream lock first.
  bool stream_finished_ = false;
  bool stream_flushed_ = false;
  bool stream_failed_ = false;
  uint64_t independent_frames_ = 0;
  xe::threading::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  std::atomic<bool> observation_open_{false}, observation_resumed_{false};
  std::atomic<bool> observation_failed_{false};

  float volume_ = 1.0f;
  const std::shared_ptr<AudioDuckerPolicy> policy_;

  uint32_t frame_frequency_;
  uint32_t frame_channels_;
  uint32_t channel_samples_;
  uint32_t frame_size_;
  bool need_format_conversion_;
  // Guest is 5.1 and device is stereo: do the downmix here, not in SDL3.
  bool manual_downmix_5_1_to_stereo_ = false;
  struct QueuedFrame {
    float* samples;
    NativeAudioFrameLabel label;
    std::shared_ptr<NativeAudioObservation> observation;
  };
  std::queue<QueuedFrame> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
};

}  // namespace sdl
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_
