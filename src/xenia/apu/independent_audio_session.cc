/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/apu/independent_audio_session.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <system_error>
#include "xenia/apu/audio_driver.h"
#include "xenia/base/mutex.h"
namespace xe::apu {
bool IndependentAudioSession::ValidPcm(const PcmAudio& pcm) {
  return pcm.frequency >= 8000 && pcm.frequency <= 192000 &&
         PcmLayoutMatchesChannels(pcm.layout, pcm.channels) &&
         !pcm.samples.empty() && pcm.samples.size() <= 64u * 1024u * 1024u &&
         pcm.samples.size() % pcm.channels == 0 &&
         std::all_of(pcm.samples.begin(), pcm.samples.end(),
                     [](float v) { return std::isfinite(v); });
}
IndependentAudioSession::IndependentAudioSession(
    PcmAudio pcm, std::unique_ptr<AudioDriver> driver,
    std::unique_ptr<threading::Semaphore> semaphore, bool paused)
    : pcm_(std::move(pcm)),
      semaphore_(std::move(semaphore)),
      driver_(std::move(driver)),
      paused_(paused) {}
IndependentAudioPreparation IndependentAudioSession::Prepare(
    PcmAudio pcm, std::unique_ptr<AudioDriver> driver,
    std::unique_ptr<threading::Semaphore> semaphore, bool paused) {
  IndependentAudioPreparation result;
  if (!driver || !semaphore) {
    return result;
  }
  if (!driver->SupportsIndependentStream()) {
    result.status = IndependentAudioPrepareStatus::kUnsupportedBackend;
    return result;
  }
  // Establish cleanup ownership before initialization or shared_ptr allocation.
  std::unique_ptr<IndependentAudioSession> session(new IndependentAudioSession(
      std::move(pcm), std::move(driver), std::move(semaphore), paused));
  if (!session->driver_->Initialize()) {
    result.status = IndependentAudioPrepareStatus::kBackendFailed;
    return result;
  }
  result.session = std::shared_ptr<IndependentAudioSession>(std::move(session));
  result.status = IndependentAudioPrepareStatus::kPrepared;
  return result;
}
IndependentAudioSession::~IndependentAudioSession() {
  BeginStop();
  if (!Drain()) {
    std::terminate();
  }
}
bool IndependentAudioSession::Start() {
  if (global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (state_ != State::kPrepared || stopping_.load()) {
    return false;
  }
  try {
    state_ = State::kPlaying;
    worker_ = std::thread([this] { Worker(); });
    return true;
  } catch (const std::exception&) {
    state_ = State::kBackendFailed;
    return false;
  }
}
void IndependentAudioSession::BeginStop() {
  stopping_.store(true, std::memory_order_release);
}
bool IndependentAudioSession::Drain() {
  if (global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  std::lock_guard join(join_mutex_);
  {
    // Serialize Start against stop-before-start. A prepared session is closed
    // without ever submitting a frame.
    std::lock_guard lock(mutex_);
    if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) {
      return false;
    }
    if (!worker_.joinable()) {
      if (state_ == State::kPrepared) {
        stopping_ = true;
        state_ = State::kStopped;
      }
    }
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  if (driver_) {
    driver_->Shutdown();
    driver_.reset();
  }
  semaphore_.reset();
  return true;
}
IndependentAudioSession::Snapshot IndependentAudioSession::snapshot() const {
  std::lock_guard lock(mutex_);
  return {state_, accepted_frames_, padding_sample_frames_};
}
void IndependentAudioSession::SetPaused(bool paused) { paused_.store(paused); }
void IndependentAudioSession::Worker() {
  State final_state = State::kStopped;
  bool paused = false;
  bool failed = false;
  auto ready = [&] {
    while (!stopping_.load(std::memory_order_acquire)) {
      const bool requested = paused_.load();
      if (requested != paused) {
        if (!driver_->SetIndependentPaused(requested)) {
          failed = true;
          return false;
        }
        paused = requested;
      }
      const auto control_status = driver_->QueryIndependentStream();
      if (control_status.state == AudioDriver::StreamState::kFailed ||
          control_status.state == AudioDriver::StreamState::kUnsupported) {
        failed = true;
        return false;
      }
      if (!paused) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  };
  try {
    const uint32_t output_channels = pcm_.channels == 1 ? 2 : pcm_.channels;
    const size_t per_frame = AudioDriver::kFrameSamplesMax / output_channels;
    const size_t source_frames = pcm_.samples.size() / pcm_.channels;
    std::array<float, AudioDriver::kFrameSamplesMax> frame{};
    for (size_t at = 0; at < source_frames && ready();) {
      const auto wait = threading::Wait(semaphore_.get(), false,
                                        std::chrono::milliseconds(10));
      if (wait == threading::WaitResult::kTimeout) {
        continue;
      }
      if (wait != threading::WaitResult::kSuccess) {
        failed = true;
        break;
      }
      if (!ready()) {
        break;
      }
      frame.fill(0);
      const size_t count = std::min(per_frame, source_frames - at);
      for (size_t i = 0; i < count; ++i) {
        for (uint32_t ch = 0; ch < output_channels; ++ch) {
          frame[i * output_channels + ch] =
              pcm_.samples[(at + i) * pcm_.channels +
                           (pcm_.channels == 1 ? 0 : ch)];
        }
      }
      if (!driver_->SubmitIndependentFrame(frame)) {
        failed = true;
        break;
      }
      {
        std::lock_guard lock(mutex_);
        ++accepted_frames_;
        padding_sample_frames_ = uint32_t(per_frame - count);
      }
      at += count;
    }
    if (!stopping_.load() && !failed) {
      if (!driver_->FinishIndependentStream()) {
        failed = true;
      } else {
        {
          std::lock_guard lock(mutex_);
          state_ = State::kDraining;
        }
        while (ready()) {
          const auto status = driver_->QueryIndependentStream();
          if (status.accepted_frames != accepted_frames_) {
            failed = true;
            break;
          }
          if (status.state == AudioDriver::StreamState::kDrained) {
            final_state = State::kStreamDrained;
            break;
          }
          if (status.state != AudioDriver::StreamState::kDraining) {
            failed = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    }
    if (failed) {
      final_state = State::kBackendFailed;
    }
  } catch (const std::exception&) {
    final_state = State::kBackendFailed;
  }
  {
    std::lock_guard lock(mutex_);
    state_ = final_state;
  }
}
}  // namespace xe::apu
