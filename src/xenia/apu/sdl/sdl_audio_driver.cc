/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/sdl_audio_driver.h"

#include <cstring>
#include <memory>

#include <utility>
#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_ducker_policy.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/helper/sdl/sdl_helper.h"

namespace xe {
namespace apu {
namespace sdl {

SDLAudioDriver::SDLAudioDriver(xe::threading::Semaphore* semaphore,
                               uint32_t frequency, uint32_t channels,
                               bool need_format_conversion,
                               std::shared_ptr<AudioDuckerPolicy> policy)
    : semaphore_(semaphore),
      policy_(std::move(policy)),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  switch (frame_channels_) {
    case 6:
      channel_samples_ = 256;
      break;
    case 2:
      channel_samples_ = 768;
      break;
    default:
      assert_unhandled_case(frame_channels_);
  }
  frame_size_ = sizeof(float) * frame_channels_ * channel_samples_;
  assert_true(frame_size_ <= kFrameSizeMax);
  assert_true(!need_format_conversion_ || frame_channels_ == 6);
}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
};

bool SDLAudioDriver::Initialize() {
  if (!xe::helper::sdl::SDLHelper::Prepare()) {
    return false;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return false;
  }
  sdl_initialized_ = true;

  // SDL3 always converts between the stream spec and the device spec, so we
  // just request our native channel count and let SDL3 downmix/upmix to the
  // hardware's actual configuration.
  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_F32;
  spec.channels = static_cast<int>(frame_channels_);
  spec.freq = static_cast<int>(frame_frequency_);

  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, SDLCallback, this);
  if (!sdl_stream_) {
    XELOGE("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }
  observation_open_.store(true, std::memory_order_release);

  // Downmix 5.1->stereo ourselves; SDL3's matrix mixer normalizes off ~6dB.
  // Shared downmix expects BE-sequential input, so only engage with the
  // format-conversion path (i.e. main audio system, not XMP-stereo).
  if (frame_channels_ == 6 && need_format_conversion_) {
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(sdl_stream_);
    SDL_AudioSpec device_spec = {};
    int sample_frames = 0;
    if (dev != 0 &&
        SDL_GetAudioDeviceFormat(dev, &device_spec, &sample_frames) &&
        device_spec.channels == 2) {
      SDL_AudioSpec stereo_spec = spec;
      stereo_spec.channels = 2;
      if (SDL_SetAudioStreamFormat(sdl_stream_, &stereo_spec, nullptr)) {
        manual_downmix_5_1_to_stereo_ = true;
      } else {
        XELOGW("SDL_SetAudioStreamFormat() failed: {}", SDL_GetError());
      }
    }
  }

  return SetIndependentPaused(false);
}

bool SDLAudioDriver::QueueFrame(
    const float* frame, bool independent, NativeAudioFrameLabel label,
    std::shared_ptr<NativeAudioObservation> observation) {
  if (!frame || !sdl_stream_) {
    if (observation) { observation->Fail(); }
    return false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  if (independent && (stream_finished_ || stream_failed_ ||
                      independent_frames_ == UINT64_MAX)) {
    return false;
  }
  std::unique_ptr<float[]> output_frame;
  if (frames_unused_.empty()) {
    output_frame =
        std::make_unique<float[]>(frame_channels_ * channel_samples_);
  } else {
    output_frame.reset(frames_unused_.top());
    frames_unused_.pop();
  }
  std::memcpy(output_frame.get(), frame, frame_size_);
  frames_queued_.push({output_frame.get(), label, observation});
  const float* queued_samples = output_frame.release();
  if (observation && !observation->RecordQueued(
          label, {reinterpret_cast<const uint8_t*>(queued_samples), frame_size_})) {
    // Only the diagnostic tag is dropped; the same ordinary PCM stays queued.
    frames_queued_.back().observation.reset();
  }
  if (independent) {
    ++independent_frames_;
  }
  return true;
}
void SDLAudioDriver::SubmitFrame(float* frame) { QueueFrame(frame, false); }
NativeAudioBackendInfo SDLAudioDriver::NativeObservationBackend() const {
  return {NativeAudioBackendInfo::Kind::kSdl,
          observation_open_.load(std::memory_order_acquire),
          observation_resumed_.load(std::memory_order_acquire), frame_frequency_,
          frame_channels_, manual_downmix_5_1_to_stereo_ ? 2u : frame_channels_,
          observation_failed_.load(std::memory_order_acquire)};
}
void SDLAudioDriver::SubmitFrameObserved(
    float* frame, NativeAudioFrameLabel label,
    const std::shared_ptr<NativeAudioObservation>& observation) {
  QueueFrame(frame, false, label, observation);
}
bool SDLAudioDriver::SubmitIndependentFrame(std::span<const float> frame) {
  return frame.size() == frame_size_ / sizeof(float) &&
         QueueFrame(frame.data(), true);
}
bool SDLAudioDriver::FinishIndependentStream() {
  std::lock_guard guard(frames_mutex_);
  if (!sdl_stream_ || stream_failed_ || !independent_frames_) {
    return false;
  }
  stream_finished_ = true;
  return true;
}
AudioDriver::StreamStatus SDLAudioDriver::QueryIndependentStream() {
  if (!sdl_stream_ || !SDL_LockAudioStream(sdl_stream_)) {
    observation_failed_.store(true, std::memory_order_release);
    return {StreamState::kFailed, 0};
  }
  StreamStatus result;
  {
    // Same stream->queue lock order as SDLCallback. No SDL call is made with
    // only frames_mutex_ held, avoiding inversion against callback admission.
    std::lock_guard guard(frames_mutex_);
    result.accepted_frames = independent_frames_;
    if (stream_failed_) {
      result.state = StreamState::kFailed;
    } else if (!stream_finished_) {
      result.state = StreamState::kAccepting;
    } else if (!frames_queued_.empty()) {
      result.state = StreamState::kDraining;
    } else {
      if (!stream_flushed_) {
        stream_flushed_ = SDL_FlushAudioStream(sdl_stream_);
        stream_failed_ = !stream_flushed_;
        if (stream_failed_) {
          observation_failed_.store(true, std::memory_order_release);
        }
      }
      const int queued = SDL_GetAudioStreamQueued(sdl_stream_);
      const int available = SDL_GetAudioStreamAvailable(sdl_stream_);
      if (stream_failed_ || queued < 0 || available < 0) {
        stream_failed_ = true;
        observation_failed_.store(true, std::memory_order_release);
        result.state = StreamState::kFailed;
      } else {
        result.state = queued == 0 && available == 0 ? StreamState::kDrained
                                                     : StreamState::kDraining;
      }
    }
  }
  SDL_UnlockAudioStream(sdl_stream_);
  return result;
}

bool SDLAudioDriver::SetIndependentPaused(bool paused) {
  const bool accepted =
      sdl_stream_ && (paused ? SDL_PauseAudioStreamDevice(sdl_stream_)
                             : SDL_ResumeAudioStreamDevice(sdl_stream_));
  if (accepted) { observation_resumed_.store(!paused, std::memory_order_release); }
  if (!accepted) {
    std::lock_guard guard(frames_mutex_);
    stream_failed_ = true;
    observation_failed_.store(true, std::memory_order_release);
  }
  return accepted;
}

void SDLAudioDriver::Pause() {
  if (sdl_stream_) {
    if (SDL_PauseAudioStreamDevice(sdl_stream_)) {
      observation_resumed_.store(false, std::memory_order_release);
    } else {
      observation_failed_.store(true, std::memory_order_release);
    }
  }
}

void SDLAudioDriver::Resume() {
  if (sdl_stream_) {
    const bool accepted = SDL_ResumeAudioStreamDevice(sdl_stream_);
    observation_resumed_.store(accepted, std::memory_order_release);
    if (!accepted) {
      observation_failed_.store(true, std::memory_order_release);
    }
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    // SDL_OpenAudioDeviceStream-created streams own the underlying logical
    // device and close it on destroy.
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    observation_open_.store(false, std::memory_order_release);
    observation_resumed_.store(false, std::memory_order_release);
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  };
  while (!frames_queued_.empty()) {
    if (frames_queued_.front().observation) {
      frames_queued_.front().observation->Retire();
    }
    delete[] frames_queued_.front().samples;
    frames_queued_.pop();
  };
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream,
                                 int additional_amount, int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream || additional_amount <= 0) {
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int frame_bytes = static_cast<int>(driver->frame_size_);
  const auto policy = driver->policy_ ? driver->policy_->Read() : nullptr;
  const auto wall_start = driver->policy_ ? driver->policy_->NowNs() : 0;
  uint64_t first_frame = 0;

  // SDL3 pulls until additional_amount is satisfied or we run out of frames.
  // If the queue empties, returning early lets SDL3 fill the remainder with
  // silence — the producer will catch up on the next callback.
  while (additional_amount > 0) {
    QueuedFrame queued;
    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      if (driver->frames_queued_.empty()) {
        return;
      }
      queued = std::move(driver->frames_queued_.front());
      driver->frames_queued_.pop();
    }
    float* buffer = queued.samples;

    // Produce a scratch buffer in SDL3's expected format. With manual downmix
    // we go straight from BE-sequential 5.1 to interleaved LE stereo via the
    // shared conversion; otherwise feed SDL3 our native channel count and let
    // its matrix mixer handle any conversion to the device's actual format.
    float scratch[kFrameSizeMax / sizeof(float)];
    int out_bytes;
    if (driver->manual_downmix_5_1_to_stereo_) {
      conversion::sequential_6_BE_to_interleaved_2_LE(scratch, buffer,
                                                      driver->channel_samples_);
      out_bytes =
          static_cast<int>(driver->channel_samples_ * 2 * sizeof(float));
    } else if (driver->need_format_conversion_) {
      conversion::sequential_6_BE_to_interleaved_6_LE(scratch, buffer,
                                                      driver->channel_samples_);
      out_bytes = frame_bytes;
    } else {
      std::memcpy(scratch, buffer, frame_bytes);
      out_bytes = frame_bytes;
    }
    const uint32_t output_channels =
        driver->manual_downmix_5_1_to_stereo_ ? 2u : driver->frame_channels_;

    // Scale by master (cvar) and per-driver volume.
    const uint32_t mv = cvars::volume > 100 ? 100 : cvars::volume;
    const float volume = driver->volume_ * (mv / 100.0f);
    if (policy) {
      const auto channels =
          driver->manual_downmix_5_1_to_stereo_ ? 2u : driver->frame_channels_;
      if (!AudioDuckerPolicy::ApplyInterleaved(
              *policy, wall_start, driver->frame_frequency_, channels,
              first_frame, volume,
              {scratch, size_t(out_bytes) / sizeof(float)})) {
        if (queued.observation) {
          queued.observation->RecordSink(
              queued.label,
              {reinterpret_cast<const uint8_t*>(scratch), size_t(out_bytes)},
              output_channels, false, false, "AudioDuckerPolicy rejected PCM");
        }
        XELOGE("APU: invalid local policy PCM block; stream failed");
        {
          std::lock_guard guard(driver->frames_mutex_);
          driver->stream_failed_ = true;
          driver->observation_failed_.store(true, std::memory_order_release);
          driver->frames_unused_.push(buffer);
        }
        driver->semaphore_->Release(1, nullptr);
        return;
      }
      first_frame += size_t(out_bytes) / (sizeof(float) * channels);
    } else if (volume != 1.0f) {
      for (size_t i = 0; i < size_t(out_bytes) / sizeof(float); ++i) {
        scratch[i] *= volume;
      }
    }

    const bool accepted = SDL_PutAudioStreamData(stream, scratch, out_bytes);
    if (queued.observation) {
      queued.observation->RecordSink(
          queued.label,
          {reinterpret_cast<const uint8_t*>(scratch), size_t(out_bytes)},
          output_channels, true, accepted, accepted ? nullptr : SDL_GetError());
    }
    if (!accepted) {
      std::lock_guard guard(driver->frames_mutex_);
      driver->stream_failed_ = true;
      driver->observation_failed_.store(true, std::memory_order_release);
    }
    additional_amount -= out_bytes;

    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      driver->frames_unused_.push(buffer);
    }

    auto ret = driver->semaphore_->Release(1, nullptr);
    assert_true(ret);
  }
}
}  // namespace sdl
}  // namespace apu
}  // namespace xe
