/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <array>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/base/native_system_ui_trace.h"
#include "xenia/gpu/native_system_title_input.h"

namespace xe::gpu::vulkan {
namespace {
enum class CaptureOutcome : uint32_t {
  kUnavailable = 1,
  kTitleStateMismatch,
  kScaleUnsupported,
  kCpuAllocationFailed,
  kCohortRejected,
  kGpuUncertain,
  kPublished,
  kWithdrawnAfterCopy,
  kLayoutInvalid,
};

enum class CapturePhase : uint32_t {
  kMetadata = 1,
  kCpuStorage,
  kCohort,
  kResidency,
  kTitleSubmit,
  kHostCopy,
  kPublication,
};

void TraceTitleInput(const NativeSystemTitleInput::Identity& id,
                     const NativeSystemTitleInput::Layout& layout,
                     CaptureOutcome outcome, uint64_t title_submission,
                     bool copy_completed = false,
                     CapturePhase phase = CapturePhase::kMetadata,
                     bool copy_attempted = false) noexcept {
  if (!cvars::native_system_ui_trace) {
    return;
  }
  static NativeSystemUiTraceLimiter limiter;
  // The transition key is only this fixed outcome enum, never a unique Swap.
  limiter.EmitTransition(uint32_t(outcome), "gpu", "title_input_capture",
                         {{"backend_epoch", id.backend_epoch},
                          {"foreground_generation", id.foreground_generation},
                          {"title_hmodule", id.title_hmodule},
                          {"title_load_generation", id.title_load_generation},
                          {"swap_serial", id.swap_serial},
                          {"outcome", uint32_t(outcome)},
                          {"phase", uint32_t(phase)},
                          {"title_submission", title_submission},
                          {"copy_attempted", copy_attempted},
                          {"copy_fence_completed", copy_completed},
                          {"physical_address", layout.range.address},
                          {"byte_count", layout.range.length},
                          {"data_extent_bytes", layout.data_extent_bytes},
                          {"width", layout.width},
                          {"height", layout.height},
                          {"pitch", layout.pitch},
                          {"row_pitch_bytes", layout.row_pitch_bytes},
                          {"tiled", layout.tiled},
                          {"endian", uint32_t(layout.endianness)},
                          {"raw_fetch_0", layout.swap.fetch[0]},
                          {"raw_fetch_1", layout.swap.fetch[1]},
                          {"raw_fetch_2", layout.swap.fetch[2]},
                          {"raw_fetch_3", layout.swap.fetch[3]},
                          {"raw_fetch_4", layout.swap.fetch[4]},
                          {"raw_fetch_5", layout.swap.fetch[5]},
                          {"normalized_fetch_0", layout.fetch[0]},
                          {"normalized_fetch_1", layout.fetch[1]},
                          {"normalized_fetch_2", layout.fetch[2]},
                          {"normalized_fetch_3", layout.fetch[3]},
                          {"normalized_fetch_4", layout.fetch[4]},
                          {"normalized_fetch_5", layout.fetch[5]}});
}
}  // namespace

bool VulkanCommandProcessor::EnableNativeSystemTitleInput(uint64_t epoch) {
  // This may be called by the service thread. Read only the coherent capacity
  // word and immutable same-queue proof; never worker-mutated Vulkan state.
  const auto capacity = GetNativeSystemFrameCapacity();
  using State = NativeSystemFrameCapacity::State;
  if (!epoch || capacity.backend_epoch != epoch || !capacity.supported ||
      !capacity.immutable_input_snapshot_supported ||
      capacity.backend_stop_proof.backend_epoch() != epoch ||
      capacity.backend_stop_proof.stopped() ||
      capacity.state == State::kUnsupported ||
      capacity.state == State::kStopping || capacity.state == State::kFailed) {
    return false;
  }
  return native_title_input_.Enable(epoch);
}

void VulkanCommandProcessor::CaptureNativeSystemTitleInput(
    const NativeSystemTitleInput::Identity& identity,
    const NativeSystemTitleInput::Layout& layout) noexcept {
  uint64_t title_submission = 0;
  bool gpu_preparation_entered = false;
  bool copy_completed = false;
  bool copy_attempted = false;
  auto phase = CapturePhase::kMetadata;
  auto outcome = CaptureOutcome::kUnavailable;
  try {
    if (!worker_thread_ || !kernel::XThread::IsInThread(worker_thread_.get()) ||
        !identity.valid() ||
        identity.foreground_generation != layout.swap.foreground_generation ||
        identity.title_hmodule != layout.swap.title_hmodule ||
        identity.title_load_generation != layout.swap.title_load_generation ||
        identity.backend_epoch != gpu_completions_.backend_epoch() ||
        !native_title_input_.enabled() || !HasNativeFrameGpuStopRetention() ||
        native_frame_fatal_ || IsGuestCommandProcessingHalted() ||
        device_lost_ || GetVulkanDevice()->IsLost()) {
      native_title_input_.Close();
      TraceTitleInput(identity, layout, outcome, 0);
      return;
    }
    // The ticket carries the actual VdSwap fetch. It must match the Title
    // register writes at this ordered XE_SWAP, never a private native bank or
    // an inherited/default slot. No guest RAM or Kernel metadata is read here.
    const auto title_fetch = register_file_->GetTextureFetch(0);
    if (native_guest_render_bank_selected_ ||
        register_file_ != graphics_system_->register_file() ||
        !title_shared_memory_ || shared_memory_.get() != title_shared_memory_ ||
        title_shared_memory_->buffer() == VK_NULL_HANDLE ||
        std::memcmp(&title_fetch, layout.swap.fetch.data(),
                    sizeof(title_fetch))) {
      native_title_input_.Close();
      TraceTitleInput(identity, layout, CaptureOutcome::kTitleStateMismatch, 0);
      return;
    }
    // This first profile snapshots guest 1x raw storage. Scaled resolves have
    // a separate backing and need an explicit conversion before being input.
    if (!render_target_cache_ ||
        render_target_cache_->draw_resolution_scale_x() != 1 ||
        render_target_cache_->draw_resolution_scale_y() != 1) {
      native_title_input_.Close();
      TraceTitleInput(identity, layout, CaptureOutcome::kScaleUnsupported, 0);
      return;
    }
    const auto checked_layout = NativeSystemTitleInput::GetLayout(layout.swap);
    if (!checked_layout ||
        checked_layout->range.address != layout.range.address ||
        checked_layout->range.length != layout.range.length ||
        checked_layout->data_extent_bytes != layout.data_extent_bytes ||
        checked_layout->fetch != layout.fetch) {
      native_title_input_.Close();
      TraceTitleInput(identity, layout, CaptureOutcome::kLayoutInvalid, 0);
      return;
    }
    // Allocate all CPU storage before entering any auxiliary GPU work. This is
    // bounded by the typed layout's 8 MiB limit, not the allocation's capacity.
    phase = CapturePhase::kCpuStorage;
    std::vector<uint8_t> bytes;
    try {
      bytes.resize(layout.range.length);
    } catch (...) {
      native_title_input_.Close();
      TraceTitleInput(identity, layout, CaptureOutcome::kCpuAllocationFailed, 0,
                      false, phase);
      return;
    }
    const std::vector<std::pair<uint32_t, uint32_t>> ranges = {
        {layout.range.address, layout.range.length}};
    phase = CapturePhase::kCohort;
    auto* presenter = graphics_system_->presenter();
    if (!presenter) {
      native_title_input_.Close();
      TraceTitleInput(identity, layout, CaptureOutcome::kCohortRejected, 0,
                      false, phase);
      return;
    }
    // From this point even a partial upload or a zero-main-draw exception uses
    // the strict native stop policy. No count can prove an empty GPU prefix.
    // The sticky flag precedes the shared Presenter cohort gate too.
    native_gpu_work_ever_attempted_ = true;
    gpu_preparation_entered = true;
    if (!presenter->BeginNativeGpuCohort(identity.backend_epoch)) {
      gpu_preparation_entered =
          false;  // Known gate rejection recorded no work.
      native_title_input_.Close();
      TraceTitleInput(identity, layout, CaptureOutcome::kCohortRejected, 0,
                      false, phase);
      return;
    }
    phase = CapturePhase::kResidency;
    if (!BeginSubmission(false)) {
      throw CaptureOutcome::kGpuUncertain;
    }
    // Preserve the normal Title GPU residency path, including host-buffer
    // memexport forwarding, before the same actual submission is closed. This
    // is not a guest RAM readback, frame advance or title-state reset.
    EnsureMemexportRangeInDeviceBuffer(layout.range.address,
                                       layout.range.length);
    if (!title_shared_memory_->RequestRange(layout.range.address,
                                            layout.range.length)) {
      throw CaptureOutcome::kGpuUncertain;
    }
    phase = CapturePhase::kTitleSubmit;
    title_submission = GetCurrentSubmission();
    if (!EndSubmission(false) || submission_open_ ||
        GetCurrentSubmission() <= title_submission || device_lost_ ||
        GetVulkanDevice()->IsLost() ||
        completion_timeline_.last_completion_error() != VK_SUCCESS) {
      throw CaptureOutcome::kGpuUncertain;
    }
    phase = CapturePhase::kHostCopy;
    {
      // Fault copies and native output copies use this same pool/fence. Force
      // staging even for zero-copy/host-buffer Title configurations, then copy
      // directly from the mapped allocation after its actual fence and CPU
      // visibility succeeded. Never mirror/re-read guest RAM for this input.
      std::lock_guard<std::mutex> lock(resolve_read_mutex_);
      try {
        copy_attempted = true;
        copy_completed =
            CopyResolveOutput(ranges, true, *title_shared_memory_, bytes);
      } catch (...) {
        resolve_fault_copy_failed_.store(true, std::memory_order_release);
        throw;
      }
      if (!copy_completed) {
        resolve_fault_copy_failed_.store(true, std::memory_order_release);
      }
    }
    if (!copy_completed) {
      throw CaptureOutcome::kGpuUncertain;
    }
    // The direct-copy fence followed the real Title submission on the same
    // canonical queue and completed. Publish rechecks epoch/foreground/title
    // generation and monotonic swap serial under the provider lock. A revoked
    // ticket drops only CPU bytes; it cannot invalidate a completed copy.
    phase = CapturePhase::kPublication;
    auto input =
        NativeSystemTitleInput::Create(identity, layout, std::move(bytes));
    const auto capacity = GetNativeSystemFrameCapacity();
    using State = NativeSystemFrameCapacity::State;
    if (!input || capacity.backend_epoch != identity.backend_epoch ||
        !capacity.supported || !capacity.immutable_input_snapshot_supported ||
        capacity.backend_stop_proof.backend_epoch() != identity.backend_epoch ||
        capacity.backend_stop_proof.stopped() ||
        capacity.state == State::kStopping ||
        capacity.state == State::kFailed ||
        native_submissions_sealed_.load(std::memory_order_acquire)) {
      native_title_input_.Close();
      outcome = CaptureOutcome::kWithdrawnAfterCopy;
    } else if (!native_title_input_.Publish(std::move(input))) {
      // A newer foreground/ticket may supersede this finished copy. Do not
      // close its provider or disturb a newer immutable publication.
      outcome = CaptureOutcome::kWithdrawnAfterCopy;
    } else {
      outcome = CaptureOutcome::kPublished;
    }
    TraceTitleInput(identity, layout, outcome, title_submission, true, phase,
                    copy_attempted);
    return;
  } catch (...) {
    if (gpu_preparation_entered && !copy_completed) {
      // No allocation or guest object is needed for retention. The whole CP's
      // banks/pool/fence live until the real sealed device-wide stop policy.
      RetainFailedNativeFrameGpuWork({}, GetCurrentSubmission(),
                                     GetCurrentSubmission() - 1, false);
      outcome = CaptureOutcome::kGpuUncertain;
    } else if (copy_completed) {
      // CPU allocation/publication failure after actual completion has no
      // uncertain GPU work. Withdraw visibility/input without fake failure.
      outcome = CaptureOutcome::kWithdrawnAfterCopy;
    }
    try {
      native_title_input_.Close();
    } catch (...) {
      // Do not unwind or drop Vulkan children if a CPU mutex failure
      // accompanies an uncertain GPU prefix. Sticky halt still reaches actual
      // GPU stop.
    }
    TraceTitleInput(identity, layout, outcome, title_submission, copy_completed,
                    phase, copy_attempted);
    if (gpu_preparation_entered && !copy_completed) {
      SignalNativeFrameFailure();
    }
  }
}
}  // namespace xe::gpu::vulkan
