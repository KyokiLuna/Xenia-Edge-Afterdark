/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"

namespace xe::gpu::vulkan {
NativeFrameReadbackResult VulkanCommandProcessor::ReadCompletedNativeFrame(
    std::shared_ptr<const NativeFrameExecutionRequest> request) {
  using Result = NativeFrameReadbackResult;
  using Admission = NativeFrameReadbackAdmission;
  Result result;
  result.status = Result::Status::kRejected;
  const auto& completion = native_frame_retained_completion_;
  const auto tag = completion.native_frame_identity();
  const auto range = native_frame_retained_written_range_;
  constexpr uint32_t kMaxReadbackBytes = 16 * 1024 * 1024;
  // This helper reads raw guest 1x storage; scaled native resolves need an
  // explicit downscale path before they can satisfy this byte contract.
  if (!worker_thread_ || !kernel::XThread::IsInThread(worker_thread_.get()) ||
      !request || request != native_frame_retained_ ||
      !native_frame_retained_submitted_ || !native_frame_retained_restored_ ||
      native_guest_render_bank_selected_ || !native_guest_render_bank_ ||
      !native_guest_render_bank_->render_target_cache ||
      native_guest_render_bank_->render_target_cache
              ->draw_resolution_scale_x() != 1 ||
      native_guest_render_bank_->render_target_cache
              ->draw_resolution_scale_y() != 1 ||
      native_frame_fatal_ || IsGuestCommandProcessingHalted() ||
      native_submissions_sealed_.load(std::memory_order_acquire) ||
      native_shutdown_in_progress_ || device_lost_ ||
      GetVulkanDevice()->IsLost() ||
      completion.status() != GpuCompletionStatus::kComplete || !tag ||
      *tag != request->identity() ||
      completion.backend_epoch() != gpu_completions_.backend_epoch() ||
      completion.backend_epoch() != request->backend_epoch() || !range.length ||
      range.length > kMaxReadbackBytes ||
      range.address < request->output_range().address ||
      uint64_t(range.address) + range.length >
          uint64_t(request->output_range().address) +
              request->output_range().length) {
    return result;
  }
  result.native_completion = completion;  // original frame association only
  result.copied_range = range;
  std::unique_ptr<Admission> admission;
  bool auxiliary_attempted = false;
  bool readback_and_snapshot_complete = false;
  try {
    // Allocate host storage before holding a Session operation or attempting
    // auxiliary GPU work. No image geometry is inferred from this byte count.
    result.bytes.resize(range.length);
    const std::vector<std::pair<uint32_t, uint32_t>> ranges = {
        {range.address, range.length}};
    admission = request->AcquireCompletedReadback(completion);
    if (!admission) {
      result.bytes.clear();
      return result;
    }
    if (request->input_provenance() ==
        NativeFrameInputProvenance::kImmutableSnapshot) {
      // Snapshot completion follows real helper visibility and a later CP
      // marker. Use its retained exact staging bytes, never mutable guest RAM
      // or current Title SharedMemory.
      if (native_frame_retained_snapshot_output_.size() != range.length) {
        admission->Finish(Admission::Outcome::kCanceledBeforeGpu);
        admission.reset();
        result.bytes.clear();
        return result;
      }
      std::memcpy(result.bytes.data(),
                  native_frame_retained_snapshot_output_.data(), range.length);
      const bool accepted = admission->Finish(Admission::Outcome::kGpuComplete);
      admission.reset();
      if (accepted) {
        result.status = Result::Status::kComplete;
        return result;
      }
      result.bytes.clear();
      return result;
    }
    {
      // The existing helper reuses pools, fences and staging ranges, so it
      // requires this same serialization as the ordinary fault-copy caller.
      std::lock_guard<std::mutex> lock(resolve_read_mutex_);
      if (!native_submissions_sealed_.load(std::memory_order_acquire) &&
          !native_frame_fatal_ && !IsGuestCommandProcessingHalted()) {
        auxiliary_attempted = true;
        try {
          if (FaultCopyResolveToGuestRam(ranges, true)) {
            // The helper's own real fence and visibility operation succeeded.
            // The physical view bypasses read watches (as used by the helper's
            // CopyPackedRangesToGuestRam), avoiding recursive fault submission.
            std::memcpy(result.bytes.data(),
                        memory_->TranslatePhysical(range.address),
                        range.length);
            readback_and_snapshot_complete = true;
          } else {
            // Close the shared-helper reuse window before releasing its mutex
            // and consulting the owner. An ordinary fault uses this pool too.
            resolve_fault_copy_failed_.store(true, std::memory_order_release);
          }
        } catch (...) {
          resolve_fault_copy_failed_.store(true, std::memory_order_release);
          throw;
        }
      }
    }
    if (readback_and_snapshot_complete) {
      const bool owner_accepted =
          admission->Finish(Admission::Outcome::kGpuComplete);
      admission.reset();  // Finish is one use, including a revoked owner.
      if (owner_accepted) {
        result.status = Result::Status::kComplete;
        return result;
      }
    }
  } catch (...) {
    // Even a zero-length/main-command count cannot prove that the existing
    // bool helper did not submit an auxiliary prefix before its failure.
  }
  if (admission) {
    // The implementation must quarantine BEFORE releasing active_operation_
    // for an uncertain GPU outcome. Its destructor does the same on abandon.
    admission->Finish(auxiliary_attempted
                          ? Admission::Outcome::kGpuUncertain
                          : Admission::Outcome::kCanceledBeforeGpu);
    admission.reset();
  }
  result.bytes.clear();
  if (auxiliary_attempted) {
    result.status = Result::Status::kFailed;
    RetainFailedNativeFrameGpuWork(request, GetCurrentSubmission(),
                                   GetCurrentSubmission() - 1, false);
    // Return the Failed result first. Halt makes the actual worker leave its
    // loop and run the sealed, device-wide native stop policy. The original
    // frame's Complete token cannot authorize retirement of this later copy.
  }
  return result;
}
}  // namespace xe::gpu::vulkan
