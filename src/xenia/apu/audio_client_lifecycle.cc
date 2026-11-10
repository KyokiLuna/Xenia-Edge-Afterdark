/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/apu/audio_client_lifecycle.h"

#include <utility>

namespace xe::apu {

AudioClientLifecycle::AudioClientLifecycle(Identity identity,
                                           std::shared_ptr<void> module_lease)
    : identity_(identity), module_lease_(std::move(module_lease)) {}

AudioClientLifecycle::Phase AudioClientLifecycle::phase() const {
  std::lock_guard lock(mutex_);
  return phase_;
}

bool AudioClientLifecycle::CompletePreparation() {
  std::lock_guard lock(mutex_);
  if (!preparing_) {
    return false;
  }
  preparing_ = false;
  if (phase_ == Phase::kPreparing) {
    phase_ = Phase::kReady;
  }
  drained_.notify_all();
  return phase_ == Phase::kReady;
}

bool AudioClientLifecycle::Activate() {
  std::lock_guard lock(mutex_);
  const bool module_owned =
      identity_.owner_kind == Identity::OwnerKind::kModule;
  if (phase_ != Phase::kReady || !identity_.owner_epoch ||
      !identity_.generation ||
      (module_owned ? (!module_lease_ || !identity_.owner_hmodule)
                    : (bool(module_lease_) || identity_.owner_hmodule))) {
    return false;
  }
  phase_ = Phase::kActive;
  return true;
}

bool AudioClientLifecycle::BeginCallback() {
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kActive || calling_) {
    return false;
  }
  calling_ = true;
  callback_thread_ = std::this_thread::get_id();
  return true;
}

void AudioClientLifecycle::EndCallback(bool executed) {
  std::lock_guard lock(mutex_);
  if (!calling_ || callback_thread_ != std::this_thread::get_id()) {
    return;
  }
  calling_ = false;
  callback_thread_ = {};
  if (!executed) {
    phase_ = Phase::kFailed;
  }
  drained_.notify_all();
}

void AudioClientLifecycle::CallbackWorkerStopped() {
  std::lock_guard lock(mutex_);
  if (calling_) {
    calling_ = false;
    callback_thread_ = {};
    phase_ = Phase::kFailed;
    drained_.notify_all();
  }
}

bool AudioClientLifecycle::IsCurrentCallback() const {
  std::lock_guard lock(mutex_);
  return calling_ && callback_thread_ == std::this_thread::get_id();
}

void AudioClientLifecycle::Retire(bool audio_worker_stopped) {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::kRetired ||
      (phase_ == Phase::kFailed && !audio_worker_stopped)) {
    return;
  }
  if (audio_worker_stopped && (calling_ || preparing_)) {
    return;
  }
  phase_ = Phase::kRetiring;
  drained_.notify_all();
}

bool AudioClientLifecycle::WaitForQuiescence() {
  std::unique_lock lock(mutex_);
  if (calling_ && callback_thread_ == std::this_thread::get_id()) {
    return false;
  }
  drained_.wait(lock, [this] { return !calling_ && !preparing_; });
  return phase_ != Phase::kFailed;
}

bool AudioClientLifecycle::ClaimCleanup() {
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kRetiring || calling_ || preparing_ ||
      cleanup_claimed_) {
    return false;
  }
  cleanup_claimed_ = true;
  return true;
}

void AudioClientLifecycle::CompleteCleanup() {
  std::shared_ptr<void> release;
  {
    std::lock_guard lock(mutex_);
    if (cleanup_claimed_ && !calling_ && !preparing_) {
      phase_ = Phase::kRetired;
      release = std::move(module_lease_);
      drained_.notify_all();
    }
  }
}

bool AudioClientLifecycle::WaitForCleanup() {
  std::unique_lock lock(mutex_);
  if (calling_ && callback_thread_ == std::this_thread::get_id()) {
    return false;
  }
  drained_.wait(lock, [this] {
    return phase_ == Phase::kRetired || phase_ == Phase::kFailed;
  });
  return phase_ == Phase::kRetired;
}

}  // namespace xe::apu
