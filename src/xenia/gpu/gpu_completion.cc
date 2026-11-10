/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/gpu_completion.h"

#include <algorithm>
#include <utility>

namespace xe::gpu {
namespace {
std::atomic<uint64_t> next_backend_epoch{1};
}

uint64_t GpuBackendStopProof::backend_epoch() const {
  return state_ ? state_->epoch : 0;
}

GpuBackendStopProof::Evidence GpuBackendStopProof::evidence() const {
  return state_ ? state_->evidence.load(std::memory_order_acquire)
                : Evidence::kNotStopped;
}

GpuCompletionStatus GpuCompletion::status() const {
  return state_ ? state_->status.load(std::memory_order_acquire)
                : GpuCompletionStatus::kUnsupported;
}

uint64_t GpuCompletion::backend_epoch() const {
  return state_ ? state_->epoch : 0;
}

GpuBackendStopProof GpuCompletion::backend_stop_proof() const {
  return state_ ? state_->stop_proof : GpuBackendStopProof();
}

uint64_t GpuCompletion::submission() const {
  return state_ ? state_->submission : 0;
}

std::optional<NativeFrameSubmissionIdentity>
GpuCompletion::native_frame_identity() const {
  return state_ ? state_->native_frame : std::nullopt;
}

GpuCompletion GpuCompletion::Unsupported(Observer observer) {
  GpuCompletion token;
  if (observer) {
    observer(token);
  }
  return token;
}

GpuCompletionQueue::GpuCompletionQueue()
    : epoch_(next_backend_epoch.fetch_add(1, std::memory_order_relaxed)),
      stop_state_(std::make_shared<GpuBackendStopProof::State>(epoch_)) {}

GpuCompletionQueue::~GpuCompletionQueue() {
  // The owner must already have stopped the GPU or transferred guest leases.
  // C++ queue destruction itself proves nothing about pending GPU work.
  CleanupWithoutStopProof();
}

void GpuCompletionQueue::PublishStopProof(
    const std::shared_ptr<GpuBackendStopProof::State>& state,
    GpuBackendStopProof::Evidence evidence) {
  auto expected = GpuBackendStopProof::Evidence::kNotStopped;
  state->evidence.compare_exchange_strong(
      expected, evidence, std::memory_order_release, std::memory_order_relaxed);
}

std::function<void()> GpuCompletionQueue::MakeAfterDeviceDestroyedCallback()
    const {
  return [state = stop_state_]() {
    PublishStopProof(state, GpuBackendStopProof::Evidence::kDeviceDestroyed);
  };
}

void GpuCompletionQueue::Notify(Entry& entry) {
  // Move before invocation so an observer cannot be delivered a second time.
  auto observer = std::move(entry.observer);
  if (observer) {
    observer(entry.token);
  }
}

GpuCompletion GpuCompletionQueue::TrackSubmitted(
    uint64_t submission, GpuCompletionResources resources,
    GpuCompletion::Observer observer) {
  return TrackSubmittedImpl(submission, std::nullopt, std::move(resources),
                            std::move(observer));
}

GpuCompletion GpuCompletionQueue::TrackNativeFrameSubmitted(
    uint64_t submission, NativeFrameSubmissionIdentity identity,
    GpuCompletionResources resources, GpuCompletion::Observer observer) {
  return TrackSubmittedImpl(submission, identity, std::move(resources),
                            std::move(observer));
}

GpuCompletion GpuCompletionQueue::TrackSubmittedImpl(
    uint64_t submission,
    std::optional<NativeFrameSubmissionIdentity> native_frame,
    GpuCompletionResources resources, GpuCompletion::Observer observer) {
  const auto status =
      stopped_ || failed_ || !submission
          ? GpuCompletionStatus::kFailed
          : (submission <= completed_through_ ? GpuCompletionStatus::kComplete
                                              : GpuCompletionStatus::kPending);
  Entry entry{GpuCompletion(std::make_shared<GpuCompletion::State>(
                  epoch_, submission, status, std::move(native_frame),
                  backend_stop_proof())),
              std::move(resources), std::move(observer)};
  const auto token = entry.token;
  if (status == GpuCompletionStatus::kPending) {
    pending_.push_back(std::move(entry));
  } else if (status == GpuCompletionStatus::kFailed && !stopped_) {
    // The notification is separate from resource retirement: failed work
    // cannot acknowledge a lease or release GPU-referenced allocations.
    auto notify = std::move(entry.observer);
    quarantine_.push_back(std::move(entry));
    if (notify) {
      notify(token);
    }
  } else {
    Notify(entry);  // Keep resources alive through the observer itself.
  }
  return token;
}

void GpuCompletionQueue::CompleteThrough(uint64_t completed_submission) {
  if (failed_ || stopped_) {
    return;
  }
  completed_through_ = std::max(completed_through_, completed_submission);
  std::vector<Entry> completed;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->token.submission() <= completed_through_) {
      it->token.state_->status.store(GpuCompletionStatus::kComplete,
                                     std::memory_order_release);
      completed.push_back(std::move(*it));
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  // No queue iteration/reference survives an observer, which may inspect its
  // token or submit a separate host request. No guest callback is run here.
  for (auto& entry : completed) {
    Notify(entry);
  }
}

void GpuCompletionQueue::FailPending() {
  failed_ = true;
  auto pending = std::move(pending_);
  pending_.clear();
  std::vector<Entry> notifications;
  for (auto& entry : pending) {
    entry.token.state_->status.store(GpuCompletionStatus::kFailed,
                                     std::memory_order_release);
    notifications.push_back({entry.token, {}, std::move(entry.observer)});
    quarantine_.push_back(std::move(entry));
  }
  for (auto& entry : notifications) {
    Notify(entry);
  }
}

GpuCompletionResources GpuCompletionQueue::TakeFailedResources() {
  FailPending();
  GpuCompletionResources resources;
  for (auto& entry : quarantine_) {
    for (auto& resource : entry.resources) {
      resources.push_back(std::move(resource));
    }
  }
  quarantine_.clear();
  return resources;
}

void GpuCompletionQueue::ReleaseAfterGpuStopped() {
  PublishStopProof(stop_state_,
                   GpuBackendStopProof::Evidence::kExplicitGpuStopped);
  CleanupWithoutStopProof();
}

void GpuCompletionQueue::ReleaseAfterDeviceLostWaitIdle() {
  PublishStopProof(stop_state_,
                   GpuBackendStopProof::Evidence::kDeviceLostWaitIdle);
  CleanupWithoutStopProof();
}

void GpuCompletionQueue::CleanupWithoutStopProof() {
  stopped_ = true;
  failed_ = true;
  auto pending = std::move(pending_);
  pending_.clear();
  auto quarantine = std::move(quarantine_);
  quarantine_.clear();
  for (auto& entry : pending) {
    entry.token.state_->status.store(GpuCompletionStatus::kFailed,
                                     std::memory_order_release);
  }
  // Stopped is published before callbacks; a reentrant request fails without
  // resurrecting work. Local entries retain resources until notifications end.
  for (auto& entry : pending) {
    Notify(entry);
  }
}

}  // namespace xe::gpu
