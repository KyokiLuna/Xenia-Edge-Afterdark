/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include "third_party/catch/include/catch.hpp"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_ducker_policy.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/pacing_audio_driver.h"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/processor.h"
namespace xe::apu::test {
namespace {
struct Backend {
  std::mutex mutex;
  std::condition_variable changed;
  bool initialize = true, finished = false;
  bool fail_pause = false, fail_resume = false, paused = false,
       control_failed = false;
  uint32_t pause_requests = 0, resume_requests = 0;
  std::atomic<bool> drain = false;
  std::atomic<uint32_t> stopped = 0;
  std::atomic<uint32_t> created = 0;
  std::vector<std::vector<float>> frames;
};
class TestDriver final : public AudioDriver {
 public:
  TestDriver(threading::Semaphore* slots, std::shared_ptr<Backend> backend)
      : slots_(slots), backend_(std::move(backend)) {}
  bool Initialize() override { return backend_->initialize; }
  void Shutdown() override { ++backend_->stopped; }
  void SubmitFrame(float*) override {}
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float) override {}
  bool SupportsIndependentStream() const override { return true; }
  bool SubmitIndependentFrame(std::span<const float> bytes) override {
    if (bytes.size() != kFrameSamplesMax) {
      return false;
    }
    {
      std::lock_guard lock(backend_->mutex);
      backend_->frames.emplace_back(bytes.begin(), bytes.end());
    }
    return slots_->Release(1, nullptr);
  }
  bool SetIndependentPaused(bool paused) override {
    bool accepted;
    {
      std::lock_guard lock(backend_->mutex);
      if (paused) {
        ++backend_->pause_requests;
      } else {
        ++backend_->resume_requests;
      }
      accepted = !(paused ? backend_->fail_pause : backend_->fail_resume);
      if (accepted) {
        backend_->paused = paused;
      } else {
        backend_->control_failed = true;
      }
    }
    backend_->changed.notify_all();
    return accepted;
  }
  bool FinishIndependentStream() override {
    {
      std::lock_guard lock(backend_->mutex);
      backend_->finished = true;
    }
    backend_->changed.notify_all();
    return true;
  }
  StreamStatus QueryIndependentStream() override {
    std::lock_guard lock(backend_->mutex);
    return {backend_->control_failed ? StreamState::kFailed
            : backend_->drain        ? StreamState::kDrained
            : backend_->finished     ? StreamState::kDraining
                                     : StreamState::kAccepting,
            backend_->frames.size()};
  }

 private:
  threading::Semaphore* slots_;
  std::shared_ptr<Backend> backend_;
};
// Genuine AudioSystem/Processor/Memory, no fabricated KernelState or guest
// callback. The backend fixture controls acceptance/drain, never audibility.
class TestSystem final : public AudioSystem {
 public:
  TestSystem(cpu::Processor* processor, std::shared_ptr<Backend> backend,
             bool silent = false)
      : AudioSystem(processor), backend_(std::move(backend)), silent_(silent) {}
  ~TestSystem() override { Shutdown(); }
  std::string name() const override { return "test-output"; }
  AudioDriver* CreateDriver(threading::Semaphore* slots, uint32_t rate,
                            uint32_t channels, bool) override {
    ++backend_->created;
    return silent_ ? static_cast<AudioDriver*>(
                         new PacingAudioDriver(slots, rate, 1536 / channels))
                   : static_cast<AudioDriver*>(new TestDriver(slots, backend_));
  }

 protected:
  X_STATUS CreateDriver(size_t, threading::Semaphore*, AudioDriver**) override {
    return X_STATUS_NOT_SUPPORTED;
  }

 private:
  std::shared_ptr<Backend> backend_;
  bool silent_;
};
PcmAudio Mono() {
  return {44100, 1, {0.25f, -0.5f, 0.75f}, PcmChannelLayout::kMono};
}
struct Fixture {
  Memory memory;
  cpu::Processor processor{&memory, nullptr};
  std::shared_ptr<Backend> backend = std::make_shared<Backend>();
  TestSystem audio{&processor, backend};
  bool WaitFinished() {
    std::unique_lock lock(backend->mutex);
    return backend->changed.wait_for(lock, std::chrono::seconds(1),
                                     [&] { return backend->finished; });
  }
};
}  // namespace
TEST_CASE(
    "Independent audio prepares silently, pads mono and waits real backend "
    "drain",
    "[audio][independent]") {
  Fixture f;
  auto prepared = f.audio.PrepareIndependentAudio(Mono());
  REQUIRE(prepared.status == IndependentAudioPrepareStatus::kPrepared);
  REQUIRE(prepared.session);
  CHECK(f.backend->frames.empty());
  CHECK(f.backend->stopped == 0);
  REQUIRE(prepared.session->Start());
  CHECK_FALSE(prepared.session->Start());
  REQUIRE(f.WaitFinished());
  CHECK(prepared.session->snapshot().state !=
        IndependentAudioSession::State::kStreamDrained);
  {
    std::lock_guard lock(f.backend->mutex);
    REQUIRE(f.backend->frames.size() == 1);
    const auto& frame = f.backend->frames[0];
    CHECK(frame[0] == 0.25f);
    CHECK(frame[1] == 0.25f);
    CHECK(frame[2] == -0.5f);
    CHECK(frame[3] == -0.5f);
    CHECK(frame[4] == 0.75f);
    CHECK(frame[5] == 0.75f);
    CHECK(frame[6] == 0);
  }
  f.backend->drain = true;
  REQUIRE(prepared.session->Drain());
  CHECK(prepared.session->snapshot().state ==
        IndependentAudioSession::State::kStreamDrained);
  CHECK(prepared.session->snapshot().accepted_frames == 1);
  CHECK(prepared.session->snapshot().padding_sample_frames == 765);
  CHECK(f.backend->stopped == 1);
  REQUIRE(prepared.session->Drain());
  CHECK(f.backend->stopped == 1);
}
TEST_CASE(
    "AudioSystem shutdown owns cancellation and retained sessions cannot "
    "restart",
    "[audio][independent]") {
  Fixture f;
  auto prepared = f.audio.PrepareIndependentAudio(Mono());
  REQUIRE(prepared.session);
  REQUIRE(prepared.session->Start());
  REQUIRE(f.WaitFinished());
  f.audio.Shutdown();
  CHECK(f.backend->stopped == 1);
  CHECK(prepared.session->snapshot().state ==
        IndependentAudioSession::State::kStopped);
  CHECK_FALSE(prepared.session->Start());
  CHECK_FALSE(f.audio.PrepareIndependentAudio(Mono()).session);
}
TEST_CASE(
    "Independent audio rejects silent pacing, invalid PCM and initialization "
    "failure",
    "[audio][independent]") {
  Fixture f;
  TestSystem silent(&f.processor, f.backend, true);
  auto rejected = silent.PrepareIndependentAudio(Mono());
  CHECK(rejected.status == IndependentAudioPrepareStatus::kUnsupportedBackend);
  CHECK_FALSE(rejected.session);
  auto pcm = Mono();
  pcm.channels = 3;
  CHECK(f.audio.PrepareIndependentAudio(std::move(pcm)).status ==
        IndependentAudioPrepareStatus::kInvalidPcm);
  f.backend->initialize = false;
  rejected = f.audio.PrepareIndependentAudio(Mono());
  CHECK(rejected.status == IndependentAudioPrepareStatus::kBackendFailed);
  CHECK_FALSE(rejected.session);
  CHECK(f.backend->stopped == 1);
}
TEST_CASE(
    "Independent audio stop before start releases backend without any "
    "submitted frames",
    "[audio][independent]") {
  Fixture f;
  auto prepared = f.audio.PrepareIndependentAudio(Mono());
  REQUIRE(prepared.session);
  prepared.session->BeginStop();
  REQUIRE(prepared.session->Drain());
  CHECK_FALSE(prepared.session->Start());
  CHECK(f.backend->frames.empty());
  CHECK(f.backend->stopped == 1);
}
TEST_CASE("Independent audio rejects guest critical-region waits",
          "[audio][independent]") {
  Fixture f;
  auto prepared = f.audio.PrepareIndependentAudio(Mono());
  REQUIRE(prepared.session);
  {
    global_critical_region critical;
    auto held = critical.Acquire();
    CHECK_FALSE(prepared.session->Start());
    CHECK_FALSE(prepared.session->Drain());
    CHECK_FALSE(f.audio.PrepareIndependentAudio(Mono()).session);
  }
  prepared.session->BeginStop();
  REQUIRE(prepared.session->Drain());
}
TEST_CASE(
    "Independent audio admission bounds backend preparation before opening "
    "devices",
    "[audio][independent]") {
  Fixture f;
  std::vector<std::shared_ptr<IndependentAudioSession>> sessions;
  for (size_t i = 0; i < 16; ++i) {
    auto prepared = f.audio.PrepareIndependentAudio(Mono());
    REQUIRE(prepared.session);
    sessions.push_back(std::move(prepared.session));
  }
  CHECK(f.backend->created == 16);
  CHECK_FALSE(f.audio.PrepareIndependentAudio(Mono()).session);
  CHECK(f.backend->created == 16);
  sessions.clear();
  CHECK(f.backend->stopped == 16);
  auto prepared = f.audio.PrepareIndependentAudio(Mono());
  REQUIRE(prepared.session);
  CHECK(f.backend->created == 17);
}
TEST_CASE(
    "Independent PCM cannot relabel an unknown or mismatched speaker layout",
    "[audio][independent]") {
  Fixture f;
  auto pcm = Mono();
  pcm.layout = PcmChannelLayout::kUnknown;
  CHECK(f.audio.PrepareIndependentAudio(std::move(pcm)).status ==
        IndependentAudioPrepareStatus::kInvalidPcm);
  pcm = Mono();
  pcm.layout = PcmChannelLayout::kStereo;
  CHECK(f.audio.PrepareIndependentAudio(std::move(pcm)).status ==
        IndependentAudioPrepareStatus::kInvalidPcm);
  CHECK(f.backend->created == 0);
}
TEST_CASE(
    "Independent failed pause/resume cannot become successful drain or submit "
    "audio",
    "[audio][independent]") {
  for (bool failure_is_pause : {true, false}) {
    Fixture f;
    f.backend->fail_pause = failure_is_pause;
    f.backend->fail_resume = !failure_is_pause;
    auto prepared = f.audio.PrepareIndependentAudio(Mono());
    REQUIRE(prepared.session);
    prepared.session->SetPaused(true);
    REQUIRE(prepared.session->Start());
    if (!failure_is_pause) {
      std::unique_lock lock(f.backend->mutex);
      const bool paused = f.backend->changed.wait_for(
          lock, std::chrono::seconds(1), [&] { return f.backend->paused; });
      lock.unlock();
      if (!paused) {
        prepared.session->BeginStop();
      }
      REQUIRE(paused);
      prepared.session->SetPaused(false);
    }
    auto drain = std::async(std::launch::async, [session = prepared.session] {
      return session->Drain();
    });
    if (drain.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
      prepared.session->BeginStop();
    }
    REQUIRE(drain.get());
    CHECK(prepared.session->snapshot().state ==
          IndependentAudioSession::State::kBackendFailed);
    CHECK(f.backend->frames.empty());
    CHECK(f.backend->stopped == 1);
    CHECK(f.backend->pause_requests == 1);
    CHECK(f.backend->resume_requests == (failure_is_pause ? 0 : 1));
  }
}

TEST_CASE(
    "Audio category output uses guarded actual Memory and rejects retired "
    "owner",
    "[audio][category][memory]") {
  Fixture f;
  REQUIRE(f.memory.Initialize());
  constexpr uint32_t address = 0x90028000;
  REQUIRE(f.memory.LookupHeap(address)->AllocFixed(
      address, 0x1000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  std::memset(f.memory.TranslateVirtual(address), 0xA5, 0x1000);
  REQUIRE(f.audio.ducker_policy()->SetCategory(1, 0.5f) ==
          AudioDuckerPolicy::Status::kSuccess);
  REQUIRE(f.audio.ReadCategoryVolume(1, address + 4) == X_STATUS_SUCCESS);
  CHECK(xe::load_and_swap<float>(f.memory.TranslateVirtual(address + 4)) ==
        0.5f);
  CHECK(f.memory.TranslateVirtual(address)[3] == 0xA5);
  CHECK(f.memory.TranslateVirtual(address)[8] == 0xA5);
  CHECK(f.audio.ReadCategoryVolume(32, address + 8) == X_E_INVALIDARG);
  CHECK(f.audio.ReadCategoryChangeMask(0, address + 8) ==
        X_STATUS_INVALID_HANDLE);
  CHECK(f.memory.TranslateVirtual(address)[8] == 0xA5);
  REQUIRE(f.memory.LookupHeap(address)->Protect(address, 0x1000,
                                                kMemoryProtectRead));
  CHECK(f.audio.ReadCategoryVolume(1, address + 8) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(f.memory.TranslateVirtual(address)[8] == 0xA5);
  const auto retained = f.audio.ducker_policy();
  f.audio.Shutdown();
  CHECK(f.audio.ReadCategoryVolume(1, address + 8) == X_STATUS(0xC00000A3));
  CHECK(retained->SetCategory(1, 0) == AudioDuckerPolicy::Status::kStopped);
  CHECK(retained->Read()->categories[1] == 0.5f);
}
}  // namespace xe::apu::test
