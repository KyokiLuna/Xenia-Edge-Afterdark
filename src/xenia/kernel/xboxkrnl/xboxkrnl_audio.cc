/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cmath>
#include "xenia/apu/audio_ducker_policy.h"
#include "xenia/apu/audio_system.h"
#include "xenia/kernel/native_notification_presentation.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The speaker configuration starts out as the dashboard audio setting.
// XAudioSetSpeakerConfig replaces it and XAudioGetSpeakerConfig returns it
// until the title ends.
dword_result_t XAudioGetSpeakerConfig_entry(lpdword_t config_ptr) {
  if (!config_ptr) {
    return X_E_INVALIDARG;
  }
  if (const auto config = GetXboxkrnlModule()->speaker_config()) {
    *config_ptr = *config;
    return X_ERROR_SUCCESS;
  }
  kernel_state()->xconfig()->ReadSetting(
      XCONFIG_USER_CATEGORY,
      XCONFIG_USER_CATEGORY_ENTRIES::XCONFIG_USER_AUDIO_FLAGS, config_ptr);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetSpeakerConfig, kAudio, kImplemented);

dword_result_t XAudioSetSpeakerConfig_entry(dword_t config) {
  // Host output keeps its own channel layout and the guest mixes 5.1 frames
  // regardless. Only the value reported back to titles changes.
  GetXboxkrnlModule()->set_speaker_config(config);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioSetSpeakerConfig, kAudio, kImplemented);

// Structure for XAudioQueryDriverPerformance
struct XAudioDriverPerformance {
  xe::be<uint32_t> frames_submitted;
  xe::be<uint32_t> frames_processed;
  xe::be<uint32_t> frames_dropped;
  xe::be<uint32_t> latency_ms;
  xe::be<uint32_t> reserved[4];
};

dword_result_t XAudioQueryDriverPerformance_entry(
    lpunknown_t driver_ptr, pointer_t<XAudioDriverPerformance> perf_ptr) {
  if (!driver_ptr || !perf_ptr) {
    return X_E_INVALIDARG;
  }

  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  size_t client_index = driver_ptr.guest_address() & 0x0000FFFF;
  auto audio_system = kernel_state()->emulator()->audio_system();

  // Get real performance metrics from the audio system
  apu::AudioSystem::ClientPerformance perf{};
  if (audio_system->GetClientPerformance(client_index, &perf)) {
    perf_ptr->frames_submitted = perf.frames_submitted;
    perf_ptr->frames_processed = perf.frames_processed;
    perf_ptr->frames_dropped = perf.frames_dropped;
  } else {
    // Client not found, return zeros
    perf_ptr->frames_submitted = 0;
    perf_ptr->frames_processed = 0;
    perf_ptr->frames_dropped = 0;
  }

  // Calculate latency based on queued frames
  // At 48kHz with 256 samples per frame for 6-channel audio:
  // Frame duration = 256 samples / 48000 Hz = ~5.33ms per frame
  // With default 8 queued frames: 8 * 5.33ms = ~42.67ms latency
  uint32_t queued_frames = perf.frames_submitted - perf.frames_processed;
  constexpr float kFrameDurationMs = 256.0f / 48000.0f * 1000.0f;  // ~5.33ms
  perf_ptr->latency_ms =
      static_cast<uint32_t>(queued_frames * kFrameDurationMs);

  for (int i = 0; i < 4; i++) {
    perf_ptr->reserved[i] = 0;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioQueryDriverPerformance, kAudio, kStub);

namespace {
X_STATUS PolicyStatus(apu::AudioDuckerPolicy::Status status) {
  switch (status) {
    case apu::AudioDuckerPolicy::Status::kSuccess:
      return X_STATUS_SUCCESS;
    case apu::AudioDuckerPolicy::Status::kInvalid:
      return X_E_INVALIDARG;
    case apu::AudioDuckerPolicy::Status::kStopped:
      return X_STATUS(0xC00000A3);
    case apu::AudioDuckerPolicy::Status::kNoMemory:
      return X_STATUS_NO_MEMORY;
    case apu::AudioDuckerPolicy::Status::kOverflow:
      return X_STATUS(0xC0000095);
  }
  return X_STATUS_UNSUCCESSFUL;
}
apu::AudioSystem* CurrentAudioSystem() {
  const auto state = kernel_state();
  const auto emulator = state ? state->emulator() : nullptr;
  return emulator ? emulator->audio_system() : nullptr;
}
std::shared_ptr<apu::AudioDuckerPolicy> AudioPolicy() {
  const auto audio = CurrentAudioSystem();
  return audio ? audio->ducker_policy() : nullptr;
}
float ReadDucker(apu::AudioDuckerPolicy::Scalar scalar) {
  const auto policy = AudioPolicy();
  if (!policy) {
    // Scalar f1 APIs cannot encode a failed lifecycle lookup as an HRESULT.
    // APU construction precedes guest execution; keep this invariant truthful.
    xe::FatalError("XAudio scalar getter without its AudioSystem owner");
  }
  return apu::AudioDuckerPolicy::ScalarValue(*policy->Read(), scalar);
}
}  // namespace

dword_result_t XAudioGetVoiceCategoryVolumeChangeMask_entry(dword_t driver,
                                                            dword_t output) {
  if ((driver & 0xFFFF0000u) != 0x41550000u) {
    return X_E_INVALIDARG;
  }
  xe::threading::MaybeYield();
  const auto audio = CurrentAudioSystem();
  return audio ? audio->ReadCategoryChangeMask(driver & 0xFFFFu, output)
               : X_STATUS(0xC00000A3);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolumeChangeMask, kAudio,
                         kImplemented, kHighFrequency);

dword_result_t XAudioGetVoiceCategoryVolume_entry(dword_t category,
                                                  dword_t output) {
  const auto audio = CurrentAudioSystem();
  return audio ? audio->ReadCategoryVolume(category, output)
               : X_STATUS(0xC00000A3);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolume, kAudio, kImplemented,
                         kHighFrequency);

dword_result_t XAudioSetVoiceCategoryVolume_entry(dword_t category,
                                                  float_t volume) {
  if (category >= apu::AudioDuckerPolicy::kCategoryCount ||
      !std::isfinite(float(volume)) || volume < 0.0f || volume > 1.0f) {
    return X_E_INVALIDARG;
  }
  const auto policy = AudioPolicy();
  return policy ? PolicyStatus(policy->SetCategory(category, volume))
                : X_STATUS(0xC00000A3);
}
DECLARE_XBOXKRNL_EXPORT1(XAudioSetVoiceCategoryVolume, kAudio, kImplemented);

dword_result_t XAudioEnableDucker_entry(dword_t enabled) {
  if (enabled > 1) {
    return X_E_INVALIDARG;
  }
  const auto policy = AudioPolicy();
  return policy ? PolicyStatus(policy->SetEnabled(enabled))
                : X_STATUS(0xC00000A3);
}
DECLARE_XBOXKRNL_EXPORT1(XAudioEnableDucker, kAudio, kImplemented);

dword_result_t XAudioSetDuckerLevel_entry(float_t level) {
  if (!std::isfinite(float(level)) || level < 0.0f || level > 1.0f) {
    return X_E_INVALIDARG;
  }
  const auto policy = AudioPolicy();
  return policy ? PolicyStatus(policy->SetScalar(
                      apu::AudioDuckerPolicy::Scalar::kLevel, level))
                : X_STATUS(0xC00000A3);
}
DECLARE_XBOXKRNL_EXPORT1(XAudioSetDuckerLevel, kAudio, kImplemented);

float_result_t XAudioGetDuckerLevel_entry() {
  return ReadDucker(apu::AudioDuckerPolicy::Scalar::kLevel);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerLevel, kAudio, kImplemented,
                         kHighFrequency);
float_result_t XAudioGetDuckerThreshold_entry() {
  return ReadDucker(apu::AudioDuckerPolicy::Scalar::kThreshold);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerThreshold, kAudio, kImplemented,
                         kHighFrequency);
float_result_t XAudioGetDuckerAttackTime_entry() {
  return ReadDucker(apu::AudioDuckerPolicy::Scalar::kAttack);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerAttackTime, kAudio, kImplemented,
                         kHighFrequency);
float_result_t XAudioGetDuckerReleaseTime_entry() {
  return ReadDucker(apu::AudioDuckerPolicy::Scalar::kRelease);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerReleaseTime, kAudio, kImplemented,
                         kHighFrequency);
float_result_t XAudioGetDuckerHoldTime_entry() {
  return ReadDucker(apu::AudioDuckerPolicy::Scalar::kHold);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerHoldTime, kAudio, kImplemented,
                         kHighFrequency);
// 34F/352/354/356/358 remain unimplemented: their native ABI was not recovered.

dword_result_t XAudioRegisterRenderDriverClient_entry(lpdword_t callback_ptr,
                                                      lpdword_t driver_ptr) {
  auto* memory = kernel_state()->memory();
  if (!callback_ptr || !driver_ptr || (callback_ptr.guest_address() & 3) ||
      (driver_ptr.guest_address() & 3) ||
      memory->QueryRangeAccess(callback_ptr.guest_address(), 8) ==
          xe::memory::PageAccess::kNoAccess ||
      memory->QueryRangeAccess(driver_ptr.guest_address(), 4) !=
          xe::memory::PageAccess::kReadWrite) {
    return X_E_INVALIDARG;
  }

  uint32_t callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  uint32_t callback_arg = callback_ptr[1];

  auto audio_system = kernel_state()->emulator()->audio_system();

  apu::AudioSystem::PreparedClient prepared;
  auto result = audio_system->PrepareClient(callback, callback_arg, &prepared);
  if (XFAILED(result)) {
    return result;
  }

  const size_t index = prepared.index();
  assert_true(!(index & ~0x0000FFFF));
  // Commit the actual native driver's output before admitting its callback.
  // Activation's mutex release publishes this store to the audio worker.
  *driver_ptr = 0x41550000 | (static_cast<uint32_t>(index) & 0x0000FFFF);
  if (XThread::IsInThread()) {
    NativeNotificationPresentationObserver::ObserveNativeAudioPublished(
        XThread::GetCurrentThread()->thread_state()->context(), prepared,
        callback_ptr.guest_address(), driver_ptr.guest_address());
  }
  result = audio_system->ActivateClient(prepared);
  if (XFAILED(result)) {
    *driver_ptr = 0;
    audio_system->CancelPreparedClient(prepared);
    return result;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioRegisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioUnregisterRenderDriverClient_entry(
    lpunknown_t driver_ptr) {
  if ((driver_ptr.guest_address() & 0xFFFF0000) != 0x41550000) {
    return X_E_INVALIDARG;
  }

  auto audio_system = kernel_state()->emulator()->audio_system();
  return audio_system->UnregisterClient(driver_ptr.guest_address() &
                                        0x0000FFFF);
}
DECLARE_XBOXKRNL_EXPORT1(XAudioUnregisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioSubmitRenderDriverFrame_entry(lpunknown_t driver_ptr,
                                                   lpunknown_t samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto audio_system = kernel_state()->emulator()->audio_system();
  const auto native_origin = XThread::IsInThread()
      ? NativeNotificationPresentationObserver::DescribeNativeCueFrame(
            XThread::GetCurrentThread()->thread_state()->context(),
            driver_ptr.guest_address(), samples_ptr.guest_address()) : nullptr;
  auto samples =
      kernel_state()->memory()->TranslateVirtual<float*>(samples_ptr);
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples,
                            native_origin);

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioSubmitRenderDriverFrame, kAudio, kImplemented,
                         kHighFrequency);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Audio);
