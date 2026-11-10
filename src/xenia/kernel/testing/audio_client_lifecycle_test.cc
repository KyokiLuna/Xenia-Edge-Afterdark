/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include "third_party/catch/include/catch.hpp"
#include "xenia/apu/audio_client_lifecycle.h"
#include "xenia/apu/audio_ducker_policy.h"
#include "xenia/apu/native_audio_observation.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC || XE_PLATFORM_ANDROID
#include <pthread.h>
#endif

// This seam creates synthetic copied metadata only. It is not an AudioSystem
// registration, a native voice owner, a module-code lease or SDL acceptance.
namespace xe::apu {
class NativeAudioObservationTestAccess {
 public:
  static std::shared_ptr<const NativeAudioClientKey> Key(
      AudioClientLifecycle::Identity identity) {
    return std::shared_ptr<const NativeAudioClientKey>(
        new NativeAudioClientKey(identity, 3, 0x815F1000, 0x3000, 0x4000));
  }
  static bool Bind(NativeAudioObservation& capture,
                   const NativeAudioClientKey& key, uint64_t baseline = 0,
                   NativeAudioBackendInfo backend = {
                       NativeAudioBackendInfo::Kind::kSdl, true, true,
                       48000, 6, 2}) {
    return capture.Bind(key, baseline, backend);
  }
  static void FailIfOpen(NativeAudioObservation& capture) {
    capture.FailIfOpen();
  }
  static const NativeAudioObservation::Frame* Storage(
      const NativeAudioObservation& capture) {
    std::lock_guard lock(capture.mutex_);
    return capture.frames_.get();
  }
};
}  // namespace xe::apu

namespace xe::apu::test {
namespace {
using State = AudioClientLifecycle;

TEST_CASE("Audio callback admission follows publication and preparation",
          "[audio][lifecycle]") {
  State state({1, 1, 2, 0x1000}, std::make_shared<int>(1));
  CHECK_FALSE(state.BeginCallback());
  CHECK_FALSE(state.Activate());
  REQUIRE(state.CompletePreparation());
  CHECK_FALSE(state.BeginCallback());
  uint32_t guest_handle = 0;
  guest_handle = 0x41550000;
  REQUIRE(state.Activate());
  REQUIRE(state.BeginCallback());
  CHECK(guest_handle == 0x41550000);
  state.EndCallback(true);
}

TEST_CASE("Audio self-unregister defers cleanup until execution unwinds",
          "[audio][lifecycle]") {
  State state({1, 2, 2, 0x1000}, std::make_shared<int>(1));
  REQUIRE(state.CompletePreparation());
  REQUIRE(state.Activate());
  REQUIRE(state.BeginCallback());
  state.Retire();
  CHECK(state.IsCurrentCallback());
  CHECK_FALSE(state.WaitForQuiescence());
  CHECK_FALSE(state.ClaimCleanup());
  CHECK_FALSE(state.BeginCallback());
  state.EndCallback(true);
  REQUIRE(state.WaitForQuiescence());
  REQUIRE(state.ClaimCleanup());
  CHECK_FALSE(state.ClaimCleanup());
  CHECK(state.phase() == State::Phase::kRetiring);
  state.CompleteCleanup();
  CHECK(state.WaitForCleanup());
}

TEST_CASE("Audio external retirement drains the old invocation before reuse",
          "[audio][lifecycle]") {
  auto pin = std::make_shared<int>(1);
  std::weak_ptr<int> weak = pin;
  auto state = std::make_shared<State>(State::Identity{3, 7, 2, 0x1000}, pin);
  pin.reset();
  REQUIRE(state->CompletePreparation());
  REQUIRE(state->Activate());
  std::promise<void> entered;
  std::promise<void> release;
  auto release_signal = release.get_future();
  auto callback =
      std::async(std::launch::async, [state, &entered, &release_signal] {
        if (!state->BeginCallback()) {
          return false;
        }
        entered.set_value();
        release_signal.wait();
        state->EndCallback(true);
        return true;
      });
  entered.get_future().wait();
  state->Retire();
  auto cleanup = std::async(std::launch::async, [state] {
    if (!state->WaitForQuiescence() || !state->ClaimCleanup()) {
      return false;
    }
    // Driver destruction and semaphore drain belong before this completion.
    state->CompleteCleanup();
    return true;
  });
  CHECK(cleanup.wait_for(std::chrono::milliseconds(1)) !=
        std::future_status::ready);
  CHECK_FALSE(state->BeginCallback());
  CHECK_FALSE(weak.expired());
  release.set_value();
  CHECK(callback.get());
  CHECK(cleanup.get());
  REQUIRE(state->WaitForCleanup());
  CHECK_FALSE(state->Activate());
  CHECK(weak.expired());
  state.reset();
  CHECK(weak.expired());
}

TEST_CASE("Failed audio execution stays quarantined until audio stop",
          "[audio][lifecycle]") {
  State state({4, 8, 2, 0x1000}, std::make_shared<int>(1));
  REQUIRE(state.CompletePreparation());
  REQUIRE(state.Activate());
  REQUIRE(state.BeginCallback());
  state.Retire();
  state.EndCallback(false);
  CHECK(state.phase() == State::Phase::kFailed);
  CHECK_FALSE(state.BeginCallback());
  state.Retire();
  CHECK_FALSE(state.ClaimCleanup());
  CHECK_FALSE(state.WaitForQuiescence());
  state.Retire(true);
  REQUIRE(state.ClaimCleanup());
  state.CompleteCleanup();
  CHECK(state.WaitForCleanup());
}

TEST_CASE("Audio retirement also fences an unfinished preparation",
          "[audio][lifecycle]") {
  State state({5, 9, 2, 0x1000}, std::make_shared<int>(1));
  state.Retire();
  CHECK_FALSE(state.ClaimCleanup());
  CHECK_FALSE(state.CompletePreparation());
  CHECK_FALSE(state.Activate());
  REQUIRE(state.ClaimCleanup());
  state.CompleteCleanup();
  CHECK(state.WaitForCleanup());
}

TEST_CASE("Audio unclassified compatibility never fabricates a module lease",
          "[audio][lifecycle]") {
  State classified({6, 10, 2, 0x1000}, {});
  REQUIRE(classified.CompletePreparation());
  CHECK_FALSE(classified.Activate());
  State unclassified({6, 11, 2, 0, State::Identity::OwnerKind::kUnclassified},
                     {});
  REQUIRE(unclassified.CompletePreparation());
  REQUIRE(unclassified.Activate());
  CHECK(unclassified.identity().owner_hmodule == 0);
  CHECK(unclassified.identity().owner_kind ==
        State::Identity::OwnerKind::kUnclassified);
  REQUIRE(unclassified.BeginCallback());
  unclassified.EndCallback(true);
  unclassified.Retire();
  REQUIRE(unclassified.ClaimCleanup());
  unclassified.CompleteCleanup();
}

TEST_CASE("Audio callback scope poisons execution interrupted by unwinding",
          "[audio][lifecycle]") {
  State state({7, 12, 2, 0x1000}, std::make_shared<int>(1));
  REQUIRE(state.CompletePreparation());
  REQUIRE(state.Activate());
  REQUIRE(state.BeginCallback());
  try {
    State::CallbackScope callback(state);
    throw std::runtime_error("synthetic interrupted execution");
  } catch (const std::exception&) {
  }
  CHECK(state.phase() == State::Phase::kFailed);
  CHECK_FALSE(state.IsCurrentCallback());
  CHECK_FALSE(state.WaitForQuiescence());
  state.Retire(true);
  REQUIRE(state.ClaimCleanup());
  state.CompleteCleanup();
}

TEST_CASE("Confirmed audio worker stop abandons non-unwound callback state",
          "[audio][lifecycle]") {
  State state({8, 13, 2, 0x1000}, std::make_shared<int>(1));
  REQUIRE(state.CompletePreparation());
  REQUIRE(state.Activate());
  REQUIRE(state.BeginCallback());
  state.Retire();
  CHECK_FALSE(state.ClaimCleanup());
  // Model a host thread exit without destructors; only the owner's actual
  // worker join authorizes this boundary in production.
  state.CallbackWorkerStopped();
  CHECK_FALSE(state.WaitForQuiescence());
  state.Retire(true);
  REQUIRE(state.ClaimCleanup());
  state.CompleteCleanup();
  CHECK(state.WaitForCleanup());
}

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC || XE_PLATFORM_ANDROID
TEST_CASE("POSIX thread exit unwinds the actual audio completion scope",
          "[audio][lifecycle]") {
  State state({9, 14, 2, 0x1000}, std::make_shared<int>(1));
  REQUIRE(state.CompletePreparation());
  REQUIRE(state.Activate());
  std::atomic<bool> admitted{false};
  std::thread worker([&] {
    if (!state.BeginCallback()) {
      return;
    }
    State::CallbackScope callback(state);
    admitted.store(true);
    pthread_exit(nullptr);
  });
  worker.join();
  REQUIRE(admitted.load());
  CHECK(state.phase() == State::Phase::kFailed);
  CHECK_FALSE(state.WaitForQuiescence());
  state.CallbackWorkerStopped();
  state.Retire(true);
  REQUIRE(state.ClaimCleanup());
  state.CompleteCleanup();
}
#endif

namespace {
uint64_t audio_test_time = 1000000000;
uint64_t AudioTestNow() { return audio_test_time; }
using Policy = AudioDuckerPolicy;
using Scalar = Policy::Scalar;
using Result = Policy::Status;
}  // namespace
TEST_CASE("Local audio initializes finite policy without retail default claims",
          "[audio][ducker]") {
  audio_test_time = 1000000000;
  Policy policy(AudioTestNow);
  const auto state = policy.Read();
  CHECK_FALSE(state->enabled);
  CHECK(state->level == 1.0f);
  CHECK(state->threshold == 1.0f);
  CHECK(state->attack == 0.010f);
  CHECK(state->release == 0.250f);
  CHECK(state->hold == 0.050f);
  CHECK(Policy::GainAt(*state, 0) == 1.0f);
  CHECK(policy.SetScalar(Scalar::kLevel, 0.25f) == Result::kSuccess);
  CHECK(policy.Read()->level == 0.25f);
  CHECK(Policy::GainAt(*policy.Read(), 0) == 1.0f);
}
TEST_CASE("Audio scalar rejection preserves the exact committed snapshot",
          "[audio][ducker]") {
  Policy policy(AudioTestNow);
  const auto initial = policy.Read();
  for (const float bad :
       {std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -1.0f, 1.01f}) {
    CHECK(policy.SetScalar(Scalar::kLevel, bad) == Result::kInvalid);
    CHECK(policy.Read() == initial);
  }
  CHECK(policy.SetScalar(Scalar::kAttack, 0) == Result::kInvalid);
  CHECK(policy.SetScalar(Scalar::kRelease, -0.1f) == Result::kInvalid);
  CHECK(policy.SetScalar(Scalar::kHold, 3601) == Result::kInvalid);
  CHECK(policy.SetEnabled(2) == Result::kInvalid);
  CHECK(policy.SetCategory(32, 0.5f) == Result::kInvalid);
  CHECK(policy.SetCategory(1, -0.01f) == Result::kInvalid);
  CHECK(policy.Read() == initial);
  CHECK(policy.SetScalar(Scalar::kHold, 0) == Result::kSuccess);
}
TEST_CASE("All streams read one audio curve without advancing it",
          "[audio][ducker][pcm]") {
  audio_test_time = 1000000000;
  Policy policy(AudioTestNow);
  REQUIRE(policy.SetScalar(Scalar::kLevel, 0.25f) == Result::kSuccess);
  REQUIRE(policy.SetEnabled(1) == Result::kSuccess);
  audio_test_time += 5000000;
  const auto state = policy.Read();
  std::array<float, 12> game, voice;
  game.fill(1);
  voice.fill(1);
  REQUIRE(
      Policy::ApplyInterleaved(*state, audio_test_time, 48000, 6, 0, 1, game));
  REQUIRE(
      Policy::ApplyInterleaved(*state, audio_test_time, 48000, 6, 0, 1, voice));
  CHECK(game == voice);
  CHECK(game[0] == Approx(0.4f));
  CHECK(game[6] < game[0]);
  CHECK(policy.Read() == state);
  REQUIRE(policy.SetEnabled(1) == Result::kSuccess);
  CHECK(policy.Read() == state);
}
TEST_CASE("Audio edge retargets are continuous and release reaches unity",
          "[audio][ducker][pcm]") {
  audio_test_time = 1000000000;
  Policy policy(AudioTestNow);
  REQUIRE(policy.SetScalar(Scalar::kLevel, 0.25f) == Result::kSuccess);
  REQUIRE(policy.SetEnabled(1) == Result::kSuccess);
  audio_test_time += 5000000;
  const auto old = policy.Read();
  const auto at_edge =
      Policy::GainAt(*old, Policy::AudioTime(*old, audio_test_time));
  REQUIRE(policy.SetScalar(Scalar::kLevel, 0.5f) == Result::kSuccess);
  const auto next = policy.Read();
  CHECK(Policy::GainAt(*next, Policy::AudioTime(*next, audio_test_time)) ==
        Approx(at_edge));
  REQUIRE(policy.SetEnabled(0) == Result::kSuccess);
  const auto release = policy.Read();
  CHECK_FALSE(release->enabled);
  audio_test_time += 250000001;
  CHECK(Policy::GainAt(*release,
                       Policy::AudioTime(*release, audio_test_time)) == 1.0f);
}
TEST_CASE("Pause freezes the common clock and rejects concurrent mutation",
          "[audio][ducker]") {
  audio_test_time = 1000000000;
  Policy policy(AudioTestNow);
  REQUIRE(policy.SetScalar(Scalar::kLevel, 0.25f) == Result::kSuccess);
  REQUIRE(policy.SetEnabled(1) == Result::kSuccess);
  audio_test_time += 5000000;
  REQUIRE(policy.SetPaused(true) == Result::kSuccess);
  const auto frozen = policy.Read();
  audio_test_time += 3000000000;
  CHECK(Policy::AudioTime(*frozen, audio_test_time) == 5000000);
  CHECK(policy.SetCategory(1, 0) == Result::kStopped);
  CHECK(policy.SetScalar(Scalar::kLevel, 0.5f) == Result::kStopped);
  std::array<float, 12> pcm;
  pcm.fill(1);
  REQUIRE(
      Policy::ApplyInterleaved(*frozen, audio_test_time, 48000, 6, 0, 1, pcm));
  CHECK(pcm[0] == pcm[6]);
  REQUIRE(policy.SetPaused(false) == Result::kSuccess);
  audio_test_time += 5000001;
  const auto resumed = policy.Read();
  CHECK(
      Policy::GainAt(*resumed, Policy::AudioTime(*resumed, audio_test_time)) ==
      Approx(0.25f));
}
TEST_CASE(
    "Audio output uses bounded global attenuation and master exactly once",
    "[audio][ducker][pcm]") {
  audio_test_time = 1000000000;
  Policy policy(AudioTestNow);
  REQUIRE(policy.SetScalar(Scalar::kLevel, 0) == Result::kSuccess);
  REQUIRE(policy.SetEnabled(1) == Result::kSuccess);
  audio_test_time += 20000000;
  const auto state = policy.Read();
  std::array<float, 12> pcm;
  pcm.fill(1);
  REQUIRE(Policy::ApplyInterleaved(*state, audio_test_time, 48000, 6, 0, 0.5f,
                                   pcm));
  CHECK(pcm[0] == Approx(Policy::kMinimumGain * 0.5f));
  for (auto sample : pcm) {
    CHECK(std::isfinite(sample));
  }
  const auto before = pcm;
  CHECK_FALSE(
      Policy::ApplyInterleaved(*state, audio_test_time, 0, 6, 0, 1, pcm));
  CHECK(pcm == before);
  CHECK_FALSE(Policy::ApplyInterleaved(*state, audio_test_time, 48000, 6,
                                       UINT64_MAX, 1, pcm));
  CHECK(pcm == before);
  auto malformed = *state;
  malformed.q_start = std::numeric_limits<double>::quiet_NaN();
  CHECK_FALSE(Policy::ApplyInterleaved(malformed, audio_test_time, 48000, 6, 0,
                                       1, pcm));
  CHECK(pcm == before);
}
TEST_CASE(
    "One global gain ducks game and preserves an aligned compensated voice",
    "[audio][ducker][pcm]") {
  audio_test_time = 1000000000;
  Policy policy(AudioTestNow);
  REQUIRE(policy.SetScalar(Scalar::kLevel, 0.25f) == Result::kSuccess);
  REQUIRE(policy.SetEnabled(1) == Result::kSuccess);
  audio_test_time += 20000000;
  const auto state = policy.Read();
  std::array<float, 6> game, compensated;
  game.fill(1);
  compensated.fill(4);
  REQUIRE(
      Policy::ApplyInterleaved(*state, audio_test_time, 48000, 6, 0, 1, game));
  REQUIRE(Policy::ApplyInterleaved(*state, audio_test_time, 48000, 6, 0, 1,
                                   compensated));
  CHECK(game[0] == Approx(0.25f));
  CHECK(compensated[0] == Approx(1.0f));
  // This is a PCM model vector, not proof of actual native/host alignment.
}
TEST_CASE(
    "Category changes are acknowledged per registration only after publication",
    "[audio][ducker][category]") {
  Policy policy(AudioTestNow);
  AudioCategoryDelivery a, b;
  auto initial = a.Stage(*policy.Read());
  REQUIRE(initial.mask == UINT32_MAX);
  a.Acknowledge(initial);
  CHECK(a.Stage(*policy.Read()).mask == 0);
  CHECK(b.Stage(*policy.Read()).mask == UINT32_MAX);
  REQUIRE(policy.SetCategory(1, 0) == Result::kSuccess);
  const auto pending = a.Stage(*policy.Read());
  REQUIRE(pending.mask == 2);
  // A failed output copy does not call Acknowledge.
  CHECK(a.Stage(*policy.Read()).mask == 2);
  REQUIRE(policy.SetCategory(1, 0.5f) == Result::kSuccess);
  a.Acknowledge(pending);
  CHECK(a.Stage(*policy.Read()).mask == 2);
  a.Acknowledge(a.Stage(*policy.Read()));
  CHECK(a.Stage(*policy.Read()).mask == 0);
  const auto revision = policy.Read()->revision;
  REQUIRE(policy.SetCategory(1, 0.5f) == Result::kSuccess);
  CHECK(policy.Read()->revision == revision);
  REQUIRE(policy.SetCategory(31, 0) == Result::kSuccess);
  CHECK(a.Stage(*policy.Read()).mask == 0x80000000u);
  AudioCategoryDelivery replacement;
  CHECK(replacement.Stage(*policy.Read()).mask == UINT32_MAX);
}
TEST_CASE(
    "Audio policy save roundtrip preserves the future curve under a new clock",
    "[audio][ducker][save]") {
  audio_test_time = 1000000000;
  Policy original(AudioTestNow);
  REQUIRE(original.SetScalar(Scalar::kLevel, 0.25f) == Result::kSuccess);
  REQUIRE(original.SetCategory(1, 0.5f) == Result::kSuccess);
  REQUIRE(original.SetEnabled(1) == Result::kSuccess);
  audio_test_time += 5000000;
  REQUIRE(original.SetPaused(true) == Result::kSuccess);
  const auto saved = original.Save();
  std::array<uint8_t, Policy::kSavedBytes + 2> guarded;
  guarded.fill(0xA5);
  REQUIRE(Policy::Encode(saved,
                         std::span(guarded).subspan(1, Policy::kSavedBytes)));
  CHECK(guarded.front() == 0xA5);
  CHECK(guarded.back() == 0xA5);
  const auto decoded =
      Policy::Decode(std::span(guarded).subspan(1, Policy::kSavedBytes));
  REQUIRE(decoded);
  audio_test_time = 50000000000;
  Policy restored(AudioTestNow, 99);
  REQUIRE(restored.Restore(*decoded) == Result::kSuccess);
  REQUIRE(restored.SetPaused(false) == Result::kSuccess);
  audio_test_time += 2000000;
  const auto state = restored.Read();
  CHECK(state->owner_epoch == 99);
  CHECK(state->categories[1] == 0.5f);
  CHECK(Policy::AudioTime(*state, audio_test_time) == 7000000);
  CHECK(Policy::GainAt(*state, 7000000) ==
        Approx(Policy::GainAt(saved.state, 7000000)));
}
TEST_CASE("Malformed audio policy bytes never replace a live policy",
          "[audio][ducker][save]") {
  Policy policy(AudioTestNow);
  auto saved = policy.Save();
  std::array<uint8_t, Policy::kSavedBytes> bytes;
  REQUIRE(Policy::Encode(saved, bytes));
  auto corrupt = bytes;
  corrupt[0] = 2;
  CHECK_FALSE(Policy::Decode(corrupt));
  CHECK_FALSE(Policy::Decode(std::span(bytes).first(bytes.size() - 1)));
  saved.state.attack = 0;
  const auto initial = policy.Read();
  const auto before = bytes;
  CHECK_FALSE(Policy::Encode(saved, bytes));
  CHECK(bytes == before);
  CHECK(policy.Restore(saved) == Result::kInvalid);
  CHECK(policy.Read() == initial);
}
TEST_CASE(
    "Audio policy stop leaves retained readers valid without accepting writes",
    "[audio][ducker][lifetime]") {
  auto policy = std::make_shared<Policy>(AudioTestNow);
  const auto retained = policy->Read();
  policy->Stop();
  CHECK(policy->SetEnabled(1) == Result::kStopped);
  CHECK(policy->SetCategory(1, 0) == Result::kStopped);
  CHECK(policy->SetScalar(Scalar::kLevel, 0.5f) == Result::kStopped);
  policy.reset();
  CHECK(Policy::GainAt(*retained, 100000) == 1.0f);
}

TEST_CASE(
    "Audio policy publication retains coherent old snapshots during writes",
    "[audio][ducker][lifetime]") {
  Policy policy(AudioTestNow);
  const auto old = policy.Read();
  std::atomic<bool> valid{true};
  std::thread writer([&] {
    for (size_t i = 0; i < 64; ++i) {
      if (policy.SetScalar(Scalar::kLevel, i & 1 ? 0.25f : 0.5f) !=
          Result::kSuccess) {
        valid = false;
      }
    }
  });
  for (size_t i = 0; i < 128; ++i) {
    const auto state = policy.Read();
    if (!std::isfinite(state->level) || state->level < 0 || state->level > 1) {
      valid = false;
    }
  }
  writer.join();
  CHECK(valid.load());
  CHECK(old->level == 1.0f);
  CHECK(old->revision == 1);
  CHECK(policy.Read()->revision > 1);
}

using Capture = NativeAudioObservation;
using CaptureAccess = NativeAudioObservationTestAccess;
State::Identity ObservationIdentity(uint64_t generation = 23) {
  return {17, generation, 2, 0x1000, State::Identity::OwnerKind::kModule};
}
std::array<uint8_t, Capture::kMaximumFrameBytes> ObservationNativeBytes() {
  std::array<uint8_t, Capture::kMaximumFrameBytes> bytes{};
  // Six sequential native channels, each with a distinct big-endian value.
  for (size_t channel = 0; channel != 6; ++channel) {
    for (size_t sample = 0; sample != 256; ++sample) {
      const size_t offset = (channel * 256 + sample) * 4;
      bytes[offset] = 0x3E;
      bytes[offset + 1] = uint8_t(channel << 4);
      bytes[offset + 2] = uint8_t(sample);
      bytes[offset + 3] = 0xA5;
    }
  }
  return bytes;
}
std::array<uint8_t, 256 * 2 * sizeof(float)> ObservationOutputBytes() {
  std::array<uint8_t, 256 * 2 * sizeof(float)> bytes{};
  // Concrete owned post-stage bytes: interleaved LE +0.25, -0.25. This tests
  // capture byte preservation, not the production converter or gain policy.
  for (size_t sample = 0; sample != 256; ++sample) {
    bytes[sample * 8 + 2] = bytes[sample * 8 + 6] = 0x80;
    bytes[sample * 8 + 3] = 0x3E;
    bytes[sample * 8 + 7] = 0xBE;
  }
  return bytes;
}

TEST_CASE("Native audio capture preserves labelled owned blocks and moves storage",
          "[audio][observation][pcm]") {
  const auto identity = ObservationIdentity();
  const auto key = CaptureAccess::Key(identity);
  auto capture = Capture::Create(identity);
  REQUIRE(capture);
  REQUIRE(CaptureAccess::Bind(*capture, *key, 41));
  CHECK_FALSE(CaptureAccess::Bind(*capture, *key, 900));
  CHECK(capture->Peek().baseline_sequence == 41);
  CHECK(key->index() == 3);
  CHECK(key->callback() == 0x815F1000);
  CHECK(key->callback_arg() == 0x3000);
  CHECK(key->wrapped_callback_arg() == 0x4000);
  const auto* allocation = CaptureAccess::Storage(*capture);
  auto native = ObservationNativeBytes();
  auto output = ObservationOutputBytes();
  const auto native_before = native;
  const auto output_before = output;
  const NativeAudioFrameLabel label{17, 23, 42};
  REQUIRE(capture->RecordQueued(label, native));
  CHECK(native == native_before);
  native.fill(0xCC);
  CHECK(capture->Peek().pending_frames == 1);
  capture->Close();
  CHECK_FALSE(capture->RecordQueued({17, 23, 43}, native));
  REQUIRE(capture->RecordSink(label, output, 2, true, true));
  CHECK(output == output_before);
  output.fill(0xDD);
  auto result = capture->Take();
  REQUIRE(result);
  REQUIRE(result->frames);
  CHECK(result->frames.get() == allocation);
  REQUIRE(result->frame_count == 1);
  CHECK(result->summary.baseline_sequence == 41);
  CHECK(result->summary.last_sequence == 42);
  CHECK(result->summary.accepted_frames == 1);
  CHECK(result->summary.pending_frames == 0);
  CHECK_FALSE(result->summary.failed);
  CHECK(result->frames[0].label == label);
  CHECK(result->frames[0].native_bytes == native_before.size());
  CHECK(result->frames[0].native_pcm == native_before);
  CHECK(result->frames[0].output_bytes == output_before.size());
  CHECK(std::memcmp(result->frames[0].output_pcm.data(), output_before.data(),
                    output_before.size()) == 0);
  CHECK(result->frames[0].output_pcm[output_before.size()] == 0);
  CHECK(result->frames[0].sink_recorded);
  CHECK(result->frames[0].put_called);
  CHECK(result->frames[0].put_accepted);
  CHECK_FALSE(capture->RecordSink(label, output, 2, true, true));
  CHECK_FALSE(capture->Take());
}

TEST_CASE("Native audio capture rejects stale ownership and non-FIFO receipts",
          "[audio][observation][sequence]") {
  const auto identity = ObservationIdentity();
  const auto key = CaptureAccess::Key(identity);
  auto native = ObservationNativeBytes();
  const auto output = ObservationOutputBytes();
  auto stale = Capture::Create(identity);
  REQUIRE(stale);
  CHECK_FALSE(CaptureAccess::Bind(*stale,
                                *CaptureAccess::Key(ObservationIdentity(24))));
  REQUIRE(CaptureAccess::Bind(*stale, *key));
  CHECK_FALSE(stale->RecordQueued({17, 24, 1}, native));
  CHECK(stale->Peek().sequence_gap);
  CHECK(stale->Peek().queued_frames == 0);

  auto foreign = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*foreign, *key));
  CHECK_FALSE(foreign->RecordQueued({18, 23, 1}, native));
  CHECK(foreign->Peek().sequence_gap);
  CHECK(foreign->Peek().queued_frames == 0);

  auto skipped = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*skipped, *key));
  CHECK_FALSE(skipped->RecordQueued({17, 23, 2}, native));
  CHECK(skipped->Peek().sequence_gap);

  auto reordered = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*reordered, *key, 100));
  REQUIRE(reordered->RecordQueued({17, 23, 101}, native));
  REQUIRE(reordered->RecordQueued({17, 23, 102}, native));
  CHECK_FALSE(reordered->RecordSink({17, 23, 102}, output, 2, true, true));
  CHECK(reordered->Peek().failed);
  CHECK(reordered->Peek().sequence_gap);
  CHECK(reordered->Peek().sink_frames == 0);
  CHECK(reordered->Peek().pending_frames == 2);

  auto duplicate = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*duplicate, *key));
  REQUIRE(duplicate->RecordQueued({17, 23, 1}, native));
  REQUIRE(duplicate->RecordSink({17, 23, 1}, output, 2, true, true));
  CHECK_FALSE(duplicate->RecordSink({17, 23, 1}, output, 2, true, true));
  CHECK(duplicate->Peek().accepted_frames == 1);
  CHECK(duplicate->Peek().sequence_gap);

  auto overflow = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*overflow, *key, UINT64_MAX - 1));
  REQUIRE(overflow->RecordQueued({17, 23, UINT64_MAX}, native));
  REQUIRE(overflow->RecordSink({17, 23, UINT64_MAX}, output, 2, true, true));
  CHECK_FALSE(overflow->RecordQueued({17, 23, 0}, native));
  CHECK(overflow->Peek().sequence_gap);
}

TEST_CASE("Native audio capture records failed Put without accepted sound claims",
          "[audio][observation][failure]") {
  const auto identity = ObservationIdentity();
  const auto key = CaptureAccess::Key(identity);
  const auto native = ObservationNativeBytes();
  const auto output = ObservationOutputBytes();
  auto capture = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*capture, *key));
  REQUIRE(capture->RecordQueued({17, 23, 1}, native));
  std::array<char, 256> error;
  error.fill('E');
  error.back() = 0;
  REQUIRE(capture->RecordSink({17, 23, 1}, output, 2, true, false,
                             error.data()));
  const auto summary = capture->Peek();
  CHECK(summary.failed);
  CHECK(summary.closed);
  CHECK(summary.sink_frames == 1);
  CHECK(summary.accepted_frames == 0);
  CHECK(summary.pending_frames == 0);
  auto result = capture->Take();
  REQUIRE(result);
  CHECK(result->frames[0].put_called);
  CHECK_FALSE(result->frames[0].put_accepted);
  CHECK(result->frames[0].error.front() == 'E');
  CHECK(result->frames[0].error.back() == 0);

  auto unsupported = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*unsupported, *key, 0, {}));
  CHECK(unsupported->Peek().unsupported);
  CHECK(unsupported->Peek().failed);
  CHECK_FALSE(unsupported->RecordQueued({17, 23, 1}, native));
  CHECK(unsupported->Peek().accepted_frames == 0);

  auto failed_backend = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(
      *failed_backend, *key, 0,
      {NativeAudioBackendInfo::Kind::kSdl, true, true, 48000, 6, 2, true}));
  CHECK(failed_backend->Peek().backend.open);
  CHECK(failed_backend->Peek().backend.resumed);
  CHECK(failed_backend->Peek().backend.failed);
  CHECK(failed_backend->Peek().unsupported);
  CHECK_FALSE(failed_backend->RecordQueued({17, 23, 1}, native));

  auto not_put = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*not_put, *key));
  REQUIRE(not_put->RecordQueued({17, 23, 1}, native));
  REQUIRE(not_put->RecordSink({17, 23, 1}, output, 2, false, false,
                             "policy rejected"));
  CHECK(not_put->Peek().failed);
  CHECK(not_put->Peek().accepted_frames == 0);
  auto not_put_result = not_put->Take();
  REQUIRE(not_put_result);
  CHECK_FALSE(not_put_result->frames[0].put_called);
}

TEST_CASE("Native audio capture bounds both stages and never accepts malformed PCM",
          "[audio][observation][bounds]") {
  const auto identity = ObservationIdentity();
  const auto key = CaptureAccess::Key(identity);
  const auto native = ObservationNativeBytes();
  const auto output = ObservationOutputBytes();
  CHECK_FALSE(Capture::Create({0, 23, 2, 0x1000}));
  CHECK_FALSE(Capture::Create({17, 0, 2, 0x1000}));
  CHECK_FALSE(Capture::Create({17, 23, 2, 0,
                             State::Identity::OwnerKind::kUnclassified}));
  auto surround = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(
      *surround, *key, 0,
      {NativeAudioBackendInfo::Kind::kSdl, true, true, 48000, 6, 6}));
  REQUIRE(surround->RecordQueued({17, 23, 1}, native));
  // Arbitrary raw bits are retained as data rather than silently normalized.
  REQUIRE(surround->RecordSink({17, 23, 1}, native, 6, true, true));
  auto surround_result = surround->Take();
  REQUIRE(surround_result);
  CHECK(surround_result->frames[0].output_bytes == 6144);
  CHECK(surround_result->frames[0].output_pcm == native);

  auto short_native = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*short_native, *key));
  CHECK_FALSE(short_native->RecordQueued(
      {17, 23, 1}, std::span(native).first(native.size() - 1)));
  CHECK(short_native->Peek().failed);
  CHECK(short_native->Peek().queued_frames == 0);

  auto short_output = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*short_output, *key));
  REQUIRE(short_output->RecordQueued({17, 23, 1}, native));
  CHECK_FALSE(short_output->RecordSink(
      {17, 23, 1}, std::span(output).first(output.size() - 1), 2, true, true));
  CHECK(short_output->Peek().pending_frames == 1);
  CHECK(short_output->Peek().accepted_frames == 0);

  auto bad_channels = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*bad_channels, *key));
  REQUIRE(bad_channels->RecordQueued({17, 23, 1}, native));
  CHECK_FALSE(bad_channels->RecordSink({17, 23, 1}, output, 6, true, true));
  CHECK(bad_channels->Peek().failed);
  CHECK(bad_channels->Peek().accepted_frames == 0);

  auto contradictory = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*contradictory, *key));
  REQUIRE(contradictory->RecordQueued({17, 23, 1}, native));
  CHECK_FALSE(contradictory->RecordSink({17, 23, 1}, output, 2, false, true));
  CHECK(contradictory->Peek().failed);
}

TEST_CASE("Native audio capture cap and cancellation preserve explicit incompleteness",
          "[audio][observation][lifetime]") {
  const auto identity = ObservationIdentity();
  const auto key = CaptureAccess::Key(identity);
  const auto native = ObservationNativeBytes();
  const auto output = ObservationOutputBytes();
  auto capped = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*capped, *key));
  for (uint64_t sequence = 1; sequence <= Capture::kMaximumFrames; ++sequence) {
    REQUIRE(capped->RecordQueued({17, 23, sequence}, native));
  }
  CHECK(capped->Peek().capped);
  CHECK(capped->Peek().closed);
  CHECK(capped->Peek().queued_frames == 512);
  CHECK_FALSE(capped->RecordQueued({17, 23, 513}, native));
  // Normal cap closes ingress, while receipts already in the real queue may
  // still complete in exact order. This does not flush or drain any stream.
  for (uint64_t sequence = 1; sequence <= Capture::kMaximumFrames; ++sequence) {
    REQUIRE(capped->RecordSink({17, 23, sequence}, output, 2, true, true));
  }
  CHECK(capped->Peek().accepted_frames == 512);
  CHECK(capped->Peek().pending_frames == 0);
  CHECK_FALSE(capped->Peek().failed);
  auto capped_result = capped->Take();
  REQUIRE(capped_result);
  CHECK(capped_result->frame_count == Capture::kMaximumFrames);
  CHECK(capped_result->frames[511].label.sequence == 512);
  CHECK(Capture::kMaximumPcmBytes == 6u * 1024u * 1024u);

  auto closed = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*closed, *key));
  closed->Close();
  // Models Close winning the race after an admission snapshot. An unrelated
  // later caller must not retroactively poison the completed window.
  CaptureAccess::FailIfOpen(*closed);
  CHECK_FALSE(closed->Peek().failed);
  auto open = Capture::Create(identity);
  REQUIRE(CaptureAccess::Bind(*open, *key));
  CaptureAccess::FailIfOpen(*open);
  CHECK(open->Peek().closed);
  CHECK(open->Peek().failed);

  for (bool retire : {false, true}) {
    auto stopped = Capture::Create(identity);
    REQUIRE(CaptureAccess::Bind(*stopped, *key));
    REQUIRE(stopped->RecordQueued({17, 23, 1}, native));
    if (retire) { stopped->Retire(); }
    else { stopped->Cancel(); }
    CHECK_FALSE(stopped->RecordQueued({17, 23, 2}, native));
    CHECK_FALSE(stopped->RecordSink({17, 23, 1}, output, 2, true, true));
    auto result = stopped->Take();
    REQUIRE(result);
    CHECK(result->frame_count == 1);
    CHECK(result->summary.pending_frames == 1);
    CHECK(result->summary.accepted_frames == 0);
    CHECK(result->summary.retired == retire);
    CHECK(result->summary.canceled == !retire);
    CHECK_FALSE(result->frames[0].sink_recorded);
  }
}
}  // namespace
}  // namespace xe::apu::test
