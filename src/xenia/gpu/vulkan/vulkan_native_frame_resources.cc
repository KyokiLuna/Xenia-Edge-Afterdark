/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <algorithm>
#include <limits>
#include <tuple>

#include "xenia/base/math.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/vulkan/vulkan_primitive_processor.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/kernel/xthread.h"

namespace xe::gpu::vulkan {

VulkanCommandProcessor::NativeTransientResources::NativeTransientResources(
    ui::vulkan::VulkanDevice& device)
    : uniform_descriptors(&device, &kDescriptorPoolSizeUniformBufferDynamic, 1,
                          kLinkedTypeDescriptorPoolSetCount),
      storage_descriptors(&device, &kDescriptorPoolSizeStorageBuffer, 1,
                          kLinkedTypeDescriptorPoolSetCount),
      storage_image_descriptors(&device,
                                kDescriptorPoolSizeStorageBufferAndImage, 2,
                                kLinkedTypeDescriptorPoolSetCount),
      texture_descriptors(&device, kDescriptorPoolSizeTextures, 2,
                          kLinkedTypeDescriptorPoolSetCount) {
  uniform_buffer_pool = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      &device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      xe::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                         size_t(16384)),
                size_t(device.properties().minUniformBufferOffsetAlignment)));
}

void VulkanCommandProcessor::NativeTransientResources::Reclaim(
    uint64_t completed_epoch) {
  uniform_buffer_pool->Reclaim(completed_epoch);
  while (!singles_used.empty() &&
         singles_used.front().frame <= completed_epoch) {
    const auto& used = singles_used.front();
    singles_free[size_t(used.layout)].push_back(used.set);
    singles_used.pop_front();
  }
  while (!constants_used.empty() &&
         constants_used.front().first <= completed_epoch) {
    constants_free.push_back(constants_used.front().second);
    constants_used.pop_front();
  }
  while (!textures_used.empty() &&
         textures_used.front().frame <= completed_epoch) {
    const auto& used = textures_used.front();
    textures_free[used.layout].push_back(used.set);
    textures_used.pop_front();
  }
}

uint64_t VulkanCommandProcessor::GetCurrentTransientResourceEpoch() const {
  if (!native_guest_render_bank_selected_) {
    return frame_current_;
  }
  assert_true(native_resource_epoch_active_.valid());
  assert_true(native_resource_epoch_active_.backend_epoch() ==
              gpu_completions_.backend_epoch());
  return native_resource_epoch_active_.serial();
}

ui::vulkan::VulkanUploadBufferPool&
VulkanCommandProcessor::CurrentUniformBufferPool() {
  if (!native_guest_render_bank_selected_) {
    return *uniform_buffer_pool_;
  }
  assert_true(native_resource_epoch_active_.valid());
  return *native_guest_render_bank_->transient_resources->uniform_buffer_pool;
}

VulkanCommandProcessor::NativeResourceEpochBeginResult
VulkanCommandProcessor::BeginNativeResourceEpoch(
    const NativeFrameExecutionRequest& request) {
  NativeResourceEpochBeginResult result;
  if (!worker_thread_ || !kernel::XThread::IsInThread(worker_thread_.get()) ||
      !native_frame_boundary_active_ || !HasNativeQueryIsolation() ||
      native_parked_title_queries_->source != request.identity() ||
      submission_open_ || native_guest_render_bank_selected_ ||
      !native_guest_render_bank_ ||
      !native_guest_render_bank_->transient_resources ||
      native_resource_epoch_active_.valid() ||
      native_resource_epochs_poisoned_ || native_submissions_sealed_ ||
      native_frame_fatal_ || IsGuestCommandProcessingHalted() ||
      !request.IsAdmitted() ||
      request.backend_epoch() != gpu_completions_.backend_epoch() ||
      native_resource_epoch_serial_ == std::numeric_limits<uint64_t>::max()) {
    return result;
  }
  // Poll actual CP fences. No title frame close/open, query emission, IRQ,
  // read-pointer change, or GPU wait is invented to make resources available.
  completion_timeline_.UpdateCompletedSubmission();
  if (device_lost_ || GetVulkanDevice()->IsLost() ||
      completion_timeline_.last_completion_error() != VK_SUCCESS) {
    return result;
  }
  ReclaimNativeResourceEpochs();
  if (native_resource_epoch_count_ == kMaxNativeResourceEpochsInFlight) {
    result.status = NativeResourceEpochBeginStatus::kBusy;
    return result;
  }
  const VulkanNativeResourceEpoch epoch(gpu_completions_.backend_epoch(),
                                        native_resource_epoch_serial_ + 1);
  // This is the parked PRIVATE primitive processor. It has no CPU handle
  // references into title resources, and performs no GPU commands here.
  if (!native_guest_render_bank_->primitive_processor->BeginNativeResourceEpoch(
          epoch, native_resource_epoch_completed_)) {
    return result;
  }
  const size_t tail =
      (native_resource_epoch_head_ + native_resource_epoch_count_) %
      kMaxNativeResourceEpochsInFlight;
  auto& record = native_resource_epochs_[tail];
  record.epoch = epoch;
  record.bank = native_guest_render_bank_.get();
  record.source = request.identity();
  record.last_submission = 0;
  record.state = NativeResourceEpochRetirement::State::kReserved;
  ++native_resource_epoch_count_;
  native_resource_epoch_serial_ = epoch.serial();
  native_resource_epoch_active_ = epoch;
  result.status = NativeResourceEpochBeginStatus::kReady;
  result.epoch = epoch;
  return result;
}

bool VulkanCommandProcessor::CloseNativeResourceEpoch(
    VulkanNativeResourceEpoch epoch, NativeResourceEpochClose outcome,
    uint64_t first_submission, uint64_t last_submission) noexcept {
  // No allocation, callback, GPU submission or logging in this finalizer.
  if (!epoch.valid() || epoch != native_resource_epoch_active_ ||
      epoch.backend_epoch() != gpu_completions_.backend_epoch() ||
      !native_resource_epoch_count_ || !native_guest_render_bank_) {
    native_resource_epochs_poisoned_ = true;
    return false;
  }
  const size_t tail =
      (native_resource_epoch_head_ + native_resource_epoch_count_ - 1) %
      kMaxNativeResourceEpochsInFlight;
  auto& record = native_resource_epochs_[tail];
  const bool canceled = outcome == NativeResourceEpochClose::kCanceledBeforeGpu;
  const bool submitted = outcome == NativeResourceEpochClose::kSubmitted &&
                         first_submission &&
                         last_submission >= first_submission &&
                         last_submission < GetCurrentSubmission();
  if (record.epoch != epoch || record.bank != native_guest_render_bank_.get() ||
      record.state != NativeResourceEpochRetirement::State::kReserved ||
      (!canceled && !submitted) || submission_open_ || device_lost_ ||
      GetVulkanDevice()->IsLost() ||
      completion_timeline_.last_completion_error() != VK_SUCCESS) {
    record.state = NativeResourceEpochRetirement::State::kPoisoned;
    native_resource_epochs_poisoned_ = true;
    return false;
  }
  auto* primitive = native_guest_render_bank_selected_
                        ? primitive_processor_.get()
                        : native_guest_render_bank_->primitive_processor.get();
  if (!primitive->EndNativeResourceEpoch(epoch)) {
    record.state = NativeResourceEpochRetirement::State::kPoisoned;
    native_resource_epochs_poisoned_ = true;
    return false;
  }
  record.last_submission = canceled ? 0 : last_submission;
  record.state = canceled ? NativeResourceEpochRetirement::State::kCanceled
                          : NativeResourceEpochRetirement::State::kSubmitted;
  native_resource_epoch_active_ = {};
  return true;
}

void VulkanCommandProcessor::ReclaimNativeResourceEpochs() {
  if (!native_guest_render_bank_ || native_resource_epochs_poisoned_ ||
      device_lost_ || GetVulkanDevice()->IsLost() ||
      completion_timeline_.last_completion_error() != VK_SUCCESS) {
    return;
  }
  const uint64_t completed_submission = GetCompletedSubmission();
  while (native_resource_epoch_count_) {
    auto& record = native_resource_epochs_[native_resource_epoch_head_];
    if (record.epoch.backend_epoch() != gpu_completions_.backend_epoch() ||
        (record.state != NativeResourceEpochRetirement::State::kCanceled &&
         (record.state != NativeResourceEpochRetirement::State::kSubmitted ||
          record.last_submission > completed_submission))) {
      break;
    }
    if (!record.bank) {
      native_resource_epochs_poisoned_ = true;
      return;
    }
    const bool selected = native_guest_render_bank_selected_ &&
                          record.bank == native_guest_render_bank_.get();
    auto* primitive = selected ? primitive_processor_.get()
                               : record.bank->primitive_processor.get();
    // These are ONLY native objects. Title upload pages and descriptor sets
    // continue to use the existing title-frame lifetime and are untouched.
    try {
      record.bank->transient_resources->Reclaim(record.epoch.serial());
      primitive->ReclaimNativeResourceEpochs(record.epoch.serial());
      auto* snapshot_memory =
          selected ? shared_memory_.get() : record.bank->shared_memory.get();
      if (record.bank->immutable_snapshot && snapshot_memory) {
        snapshot_memory->CompletedSubmissionUpdated();
      }
    } catch (...) {
      // Extending a descriptor free list can fail on the host. Keep this
      // fixed record pending. Any resources already moved were independently
      // proven idle, so retrying retirement cannot create premature reuse.
      return;
    }
    native_resource_epoch_completed_ = record.epoch.serial();
    record = {};
    native_resource_epoch_head_ =
        (native_resource_epoch_head_ + 1) % kMaxNativeResourceEpochsInFlight;
    --native_resource_epoch_count_;
  }
}
}  // namespace xe::gpu::vulkan
