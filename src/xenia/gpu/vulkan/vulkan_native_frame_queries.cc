/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <utility>

#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_query_pool.h"
#include "xenia/kernel/xthread.h"

namespace xe::gpu::vulkan {

NativeSystemFrameCapacity VulkanCommandProcessor::GetNativeSystemFrameCapacity()
    const noexcept {
  NativeSystemFrameCapacity result;
  const uint32_t word =
      native_frame_capacity_word_.load(std::memory_order_acquire);
  result.supported = (word & kNativeFrameCapabilitySupported) != 0;
  result.immutable_input_snapshot_supported =
      result.supported && (word & kNativeImmutableSnapshotSupported) != 0;
  result.state = NativeSystemFrameCapacity::State(word & 0xFF);
  result.backend_epoch = gpu_completions_.backend_epoch();
  result.backend_stop_proof = gpu_completions_.backend_stop_proof();
  if (!native_frame_admissions_open_.load(std::memory_order_acquire) ||
      result.backend_stop_proof.stopped()) {
    result.state = NativeSystemFrameCapacity::State::kStopping;
  }
  return result;
}

void VulkanCommandProcessor::RefreshNativeFrameCapacity() noexcept {
  using State = NativeSystemFrameCapacity::State;
  State state = State::kUnsupported;
  if (native_shutdown_in_progress_ || native_submissions_sealed_) {
    state = State::kStopping;
  } else if (native_frame_fatal_ || IsGuestCommandProcessingHalted() ||
             device_lost_) {
    state = State::kFailed;
  } else if (native_frame_capacity_word_.load(std::memory_order_relaxed) &
             kNativeFrameCapabilitySupported) {
    if (native_frame_boundary_active_) {
      state = State::kExecuting;
    } else if (native_frame_pending_) {
      state = native_frame_waiting_capacity_ ? State::kBackpressured
                                             : State::kQueued;
    } else if (native_frame_retained_) {
      if (!native_frame_retained_restored_ ||
          !native_frame_retained_submitted_) {
        state = State::kFailed;
      } else {
        const auto status = native_frame_retained_completion_.status();
        state = status == GpuCompletionStatus::kComplete ? State::kAccepting
                : status == GpuCompletionStatus::kPending
                    ? State::kBackpressured
                    : State::kFailed;
      }
    } else {
      state = State::kAccepting;
    }
  }
  SetNativeFrameCapacityState(state);
}

bool VulkanCommandProcessor::HasNativeQueryIsolation() const {
  const auto* context = native_parked_title_queries_.get();
  return context && context->isolated && context->serial &&
         context->backend_epoch == gpu_completions_.backend_epoch() &&
         context->title_register_file == graphics_system_->register_file() &&
         QueryTransportSuspended();
}

VulkanCommandProcessor::NativeQueryParkStatus
VulkanCommandProcessor::ParkTitleQueries(
    const NativeFrameExecutionRequest& request) {
  if (!worker_thread_ || !kernel::XThread::IsInThread(worker_thread_.get()) ||
      !native_frame_boundary_active_ || native_guest_render_bank_selected_ ||
      native_parked_title_queries_ || QueryTransportSuspended() ||
      query_segment_opening_ || scratch_buffer_used_ ||
      register_file_ != graphics_system_->register_file()) {
    return NativeQueryParkStatus::kBusy;
  }
  if (native_query_scope_serial_ == UINT64_MAX || device_lost_ ||
      GetVulkanDevice()->IsLost() ||
      completion_timeline_.last_completion_error() != VK_SUCCESS) {
    return NativeQueryParkStatus::kFailed;
  }
  // Allocate host ownership before recording a physical query end or submit.
  auto context = std::make_unique<ParkedTitleQueryContext>();
  context->source = request.identity();
  context->serial = ++native_query_scope_serial_;
  context->backend_epoch = gpu_completions_.backend_epoch();
  context->title_register_file = register_file_;
  native_parked_title_queries_ = std::move(context);
  auto& saved = *native_parked_title_queries_;
  // No readback or logical guest publication is needed to park: pending
  // resolves stay owned, generation-tagged and unavailable to native helpers.
  SuspendQueryTransport(true);
  const auto closed = CloseQuerySegmentChecked();
  if (closed == QuerySegmentCloseStatus::kFailed ||
      (submission_open_ && !EndSubmission(false)) || submission_open_ ||
      in_render_pass_ || active_segment_.segment_active ||
      active_query_index_ != UINT32_MAX || active_query_is_fsi_ ||
      !viz_pending_copies_.empty() ||
      (host_query_pool_ && host_query_pool_->has_pending_resolve_batch()) ||
      !pending_barriers_.empty() || current_pending_barrier_.src_stage_mask ||
      current_pending_barrier_.dst_stage_mask) {
    return NativeQueryParkStatus::kFailed;
  }
  saved.prefix_submission = GetCurrentSubmission() - 1;
  // Existing CloseQuerySegment preserved pending logical measurement and
  // accumulated counts. Do not restore its former physical active/index state.
  ExchangeTitleQueryLogicalState(saved.logical);
  using std::swap;
  swap(host_query_pool_, saved.pool);
  swap(query_resolves_in_flight_, saved.resolves);
  swap(viz_pending_copies_, saved.viz_copies);
  swap(viz_predicate_buffer_, saved.predicate_buffer);
  swap(viz_predicate_buffer_memory_, saved.predicate_memory);
  swap(viz_predicate_buffer_failed_, saved.predicate_failed);
  swap(zpd_fsi_path_, saved.fsi_path);
  swap(zpd_counter_index_force_update_, saved.counter_dirty);
  swap(zpd_counter_descriptor_buffer_, saved.counter_descriptor_buffer);
  swap(zpd_counter_descriptor_range_, saved.counter_descriptor_range);
  saved.isolated = true;
  return NativeQueryParkStatus::kReady;
}

bool VulkanCommandProcessor::RestoreTitleQueries(bool after_gpu_stopped) {
  if (!native_parked_title_queries_) {
    return !QueryTransportSuspended();
  }
  auto& saved = *native_parked_title_queries_;
  const auto proof = gpu_completions_.backend_stop_proof();
  const bool stopped = after_gpu_stopped && native_submissions_sealed_ &&
                       proof.backend_epoch() == saved.backend_epoch &&
                       proof.stopped();
  if ((after_gpu_stopped && !stopped) || !worker_thread_ ||
      !kernel::XThread::IsInThread(worker_thread_.get()) ||
      saved.backend_epoch != gpu_completions_.backend_epoch() ||
      native_guest_render_bank_selected_ ||
      register_file_ != saved.title_register_file ||
      register_file_ != graphics_system_->register_file() ||
      (!stopped &&
       (submission_open_ || in_render_pass_ || device_lost_ ||
        GetVulkanDevice()->IsLost() ||
        completion_timeline_.last_completion_error() != VK_SUCCESS))) {
    return false;
  }
  if (!saved.isolated) {
    // A failed physical split never exchanged the live title objects. This
    // path is legal only after the actual GPU has stopped, never a CPU
    // rollback.
    if (!stopped) {
      return false;
    }
    SuspendQueryTransport(false);
    native_parked_title_queries_.reset();
    return true;
  }
  const bool native_empty =
      !active_segment_.segment_active &&
      !active_segment_.segment_pending_begin && !query_segment_opening_ &&
      zpd_current_report_.handle == kInvalidReportHandle &&
      zpd_reports_.empty() && !viz_pending_resolves_ &&
      active_query_index_ == UINT32_MAX && !active_query_is_fsi_ &&
      !host_query_pool_ && query_resolves_in_flight_.empty() &&
      viz_pending_copies_.empty() && viz_predicate_buffer_ == VK_NULL_HANDLE &&
      viz_predicate_buffer_memory_ == VK_NULL_HANDLE;
  if (!native_empty && !stopped) {
    return false;
  }
  ExchangeTitleQueryLogicalState(saved.logical);
  using std::swap;
  swap(host_query_pool_, saved.pool);
  swap(query_resolves_in_flight_, saved.resolves);
  swap(viz_pending_copies_, saved.viz_copies);
  swap(viz_predicate_buffer_, saved.predicate_buffer);
  swap(viz_predicate_buffer_memory_, saved.predicate_memory);
  swap(viz_predicate_buffer_failed_, saved.predicate_failed);
  swap(zpd_fsi_path_, saved.fsi_path);
  swap(zpd_counter_index_force_update_, saved.counter_dirty);
  swap(zpd_counter_descriptor_buffer_, saved.counter_descriptor_buffer);
  swap(zpd_counter_descriptor_range_, saved.counter_descriptor_range);
  zpd_counter_index_force_update_ = true;
  SuspendQueryTransport(false);
  // Normally these are null/empty. A stopped-only unexpected native context
  // can be destroyed now, after the real all-queue/device stop, never before.
  if (stopped) {
    const auto& dfn = GetVulkanDevice()->functions();
    const auto device = GetVulkanDevice()->device();
    if (saved.predicate_buffer != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, saved.predicate_buffer, nullptr);
    }
    if (saved.predicate_memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, saved.predicate_memory, nullptr);
    }
  }
  native_parked_title_queries_.reset();
  return true;
}
}  // namespace xe::gpu::vulkan
