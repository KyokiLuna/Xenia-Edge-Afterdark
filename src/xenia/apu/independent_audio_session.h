/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_APU_INDEPENDENT_AUDIO_SESSION_H_
#define XENIA_APU_INDEPENDENT_AUDIO_SESSION_H_
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "xenia/apu/audio_memory_decoder.h"
#include "xenia/base/threading.h"
namespace xe::apu {
class AudioDriver;
class AudioSystem;
class IndependentAudioSession;
enum class IndependentAudioPrepareStatus {
  kPrepared,
  kInvalidPcm,
  kUnavailable,
  kUnsupportedBackend,
  kBackendFailed,
  kNoMemory
};
struct IndependentAudioPreparation {
  IndependentAudioPrepareStatus status =
      IndependentAudioPrepareStatus::kUnavailable;
  std::shared_ptr<IndependentAudioSession> session;
};
class IndependentAudioSession {
 public:
  enum class State {
    kPrepared,
    kPlaying,
    kDraining,
    kStreamDrained,
    kStopped,
    kBackendFailed
  };
  struct Snapshot {
    State state;
    uint64_t accepted_frames;
    uint32_t padding_sample_frames;
  };
  ~IndependentAudioSession();
  // Preparing never starts sound. Explicit first-visible ownership belongs to
  // the caller; a native-accepted event must not start a fallback session.
  bool Start();
  void BeginStop();
  // Joins host work; false only if called by this session's worker. Never call
  // while holding guest/global/provider locks. Safe repeatedly/concurrently.
  bool Drain();
  Snapshot snapshot() const;
  // Requests normal pause/resume. It does not claim backend success; checked
  // execution failures become kBackendFailed and remain visible to the owner.
  void SetPaused(bool);

 private:
  friend class AudioSystem;
  static bool ValidPcm(const PcmAudio&);
  static IndependentAudioPreparation Prepare(
      PcmAudio, std::unique_ptr<AudioDriver>,
      std::unique_ptr<threading::Semaphore>, bool);
  IndependentAudioSession(PcmAudio, std::unique_ptr<AudioDriver>,
                          std::unique_ptr<threading::Semaphore>, bool);
  void Worker();
  // Driver shutdown precedes semaphore/PCM release. No AudioSystem pointer.
  PcmAudio pcm_;
  std::unique_ptr<threading::Semaphore> semaphore_;
  std::unique_ptr<AudioDriver> driver_;
  mutable std::mutex mutex_;
  std::mutex join_mutex_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> paused_{false};
  State state_ = State::kPrepared;
  uint64_t accepted_frames_ = 0;
  uint32_t padding_sample_frames_ = 0;
};
}  // namespace xe::apu
#endif
