/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <mutex>
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"

DECLARE_bool(native_frame_composite_probe);

namespace xe::gpu::vulkan {

#define TRACE_NATIVE_REQUEST(component, event, request, ...)               \
  do {                                                                     \
    if (cvars::native_system_ui_trace && request) {                        \
      static xe::NativeSystemUiTraceLimiter limiter;                       \
      const auto& trace_identity = request->identity();                    \
      limiter.Emit(component, event,                                       \
                   {{"kernel_epoch", trace_identity.owner.kernel_epoch},   \
                    {"runtime_module_generation",                          \
                     trace_identity.owner.module_generation},              \
                    {"owner_session_id", trace_identity.owner.session_id}, \
                    {"root_hmodule", trace_identity.owner.module_hmodule}, \
                    {"source_serial", trace_identity.source_serial},       \
                    {"backend_epoch", request->backend_epoch()}},          \
                   {__VA_ARGS__});                                         \
    }                                                                      \
  } while (false)

namespace {
using Plan = NativeFrameCommandPlan;
using Request = NativeFrameExecutionRequest;
using Result = NativeFrameExecutionResult;
using Known = std::bitset<RegisterFile::kRegisterCount>;

// A conservative, explicit consumed-state profile. It expands the .cc audit
// with inline viewport reads and the four color/blend entries. Shader-used
// constant/fetch families are proved by the Request builder, not this table.
// VGT_DRAW_INITIATOR is established by the typed DRAW operand. VGT_DMA_SIZE is
// not consumed by the admitted AutoIndex path. No zero RF reset is admitted.
constexpr uint32_t kRequiredNativeState[] = {
    XE_GPU_REG_PA_CL_CLIP_CNTL,
    XE_GPU_REG_PA_CL_VPORT_XOFFSET,
    XE_GPU_REG_PA_CL_VPORT_XSCALE,
    XE_GPU_REG_PA_CL_VPORT_YOFFSET,
    XE_GPU_REG_PA_CL_VPORT_YSCALE,
    XE_GPU_REG_PA_CL_VPORT_ZOFFSET,
    XE_GPU_REG_PA_CL_VPORT_ZSCALE,
    XE_GPU_REG_PA_CL_VTE_CNTL,
    XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR,
    XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL,
    XE_GPU_REG_PA_SC_VIZ_QUERY,
    XE_GPU_REG_PA_SC_WINDOW_OFFSET,
    XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
    XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL,
    XE_GPU_REG_PA_SU_POINT_MINMAX,
    XE_GPU_REG_PA_SU_POINT_SIZE,
    XE_GPU_REG_PA_SU_SC_MODE_CNTL,
    XE_GPU_REG_PA_SU_VTX_CNTL,
    XE_GPU_REG_RB_ALPHA_REF,
    XE_GPU_REG_RB_BLENDCONTROL0,
    XE_GPU_REG_RB_BLENDCONTROL1,
    XE_GPU_REG_RB_BLENDCONTROL2,
    XE_GPU_REG_RB_BLENDCONTROL3,
    XE_GPU_REG_RB_BLEND_ALPHA,
    XE_GPU_REG_RB_BLEND_BLUE,
    XE_GPU_REG_RB_BLEND_GREEN,
    XE_GPU_REG_RB_BLEND_RED,
    XE_GPU_REG_RB_COLOR1_INFO,
    XE_GPU_REG_RB_COLOR2_INFO,
    XE_GPU_REG_RB_COLOR3_INFO,
    XE_GPU_REG_RB_COLORCONTROL,
    XE_GPU_REG_RB_COLOR_CLEAR,
    XE_GPU_REG_RB_COLOR_CLEAR_LO,
    XE_GPU_REG_RB_COLOR_INFO,
    XE_GPU_REG_RB_COLOR_MASK,
    XE_GPU_REG_RB_COPY_CONTROL,
    XE_GPU_REG_RB_COPY_DEST_BASE,
    XE_GPU_REG_RB_COPY_DEST_INFO,
    XE_GPU_REG_RB_COPY_DEST_PITCH,
    XE_GPU_REG_RB_COPY_FUNC,
    XE_GPU_REG_RB_COPY_MASK,
    XE_GPU_REG_RB_COPY_REF,
    XE_GPU_REG_RB_COPY_SURFACE_SLICE,
    XE_GPU_REG_RB_DEPTHCONTROL,
    XE_GPU_REG_RB_DEPTH_CLEAR,
    XE_GPU_REG_RB_DEPTH_INFO,
    XE_GPU_REG_RB_HIZCONTROL,
    XE_GPU_REG_RB_MODECONTROL,
    XE_GPU_REG_RB_STENCILREFMASK,
    XE_GPU_REG_RB_STENCILREFMASK_BF,
    XE_GPU_REG_RB_SURFACE_INFO,
    XE_GPU_REG_SQ_CONTEXT_MISC,
    XE_GPU_REG_SQ_PROGRAM_CNTL,
    XE_GPU_REG_SQ_PS_CONST,
    XE_GPU_REG_SQ_VS_CONST,
    XE_GPU_REG_VGT_DRAW_INITIATOR,
    XE_GPU_REG_VGT_HOS_CNTL,
    XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL,
    XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL,
    XE_GPU_REG_VGT_INDX_OFFSET,
    XE_GPU_REG_VGT_MAX_VTX_INDX,
    XE_GPU_REG_VGT_MIN_VTX_INDX,
    XE_GPU_REG_VGT_OUTPUT_PATH_CNTL,
};

bool ClosedStateAtDraw(const RegisterFile& regs, const Known& known,
                       uint32_t packet) {
  for (const uint32_t index : kRequiredNativeState) {
    if (!known[index]) {
      XELOGE("Native frame executor: DWORD{} missing explicit register {:04X}",
             packet, index);
      return false;
    }
  }
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (initiator.source_select != xenos::SourceSelect::kAutoIndex ||
      initiator.not_eop ||
      xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) ||
      regs.Get<reg::PA_SC_VIZ_QUERY>().value) {
    return false;
  }
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (!clip.clip_disable) {
    for (uint32_t plane = 0; plane < 6; ++plane) {
      if (!(clip.ucp_ena & (UINT32_C(1) << plane))) {
        continue;
      }
      for (uint32_t word = 0; word < 4; ++word) {
        if (!known[XE_GPU_REG_PA_CL_UCP_0_X + plane * 4 + word]) {
          return false;
        }
      }
    }
  }
  const auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (sc.poly_offset_front_enable || sc.poly_offset_back_enable ||
      sc.poly_offset_para_enable) {
    for (const uint32_t index :
         {uint32_t(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
          uint32_t(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
          uint32_t(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
          uint32_t(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET)}) {
      if (!known[index]) {
        return false;
      }
    }
  }
  // With explicitly disabled planes/offset units their unused values are not
  // native defaults; the backend's disabled-unit paths do not consume them.
  return true;
}

bool PreflightNativeState(const Request& request, uint32_t& failing_packet) {
  RegisterFile regs;
  Known known;
  const auto& plan = *request.plan();
  if (const auto& input = plan.title_input()) {
    for (uint32_t i = 0; i < 6; ++i) {
      const uint32_t index = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 15 * 6 + i;
      regs.values[index] = input->fetch()[i];
      known.set(index);
    }
  }
  const auto& writes = plan.register_writes();
  size_t write_index = 0, draw_index = 0, shader_index = 0;
  bool copy_seen = false, coherency_status_dirty = false;
  NativeOverlayState overlay_requested;
  for (const auto& packet : plan.packets()) {
    failing_packet = packet.dword_offset;
    switch (packet.kind) {
      case Plan::PacketKind::kRegisterWrite:
        while (write_index < writes.size() &&
               writes[write_index].packet_dword_offset == packet.dword_offset) {
          const auto& write = writes[write_index++];
          regs.values[write.index] = write.value;
          if (write.index == XE_GPU_REG_COHER_STATUS_HOST) {
            regs.values[write.index] |= UINT32_C(0x80000000);
            coherency_status_dirty = true;
          }
          known.set(write.index);
        }
        break;
      case Plan::PacketKind::kNativeOverlayControl:
        if ((!plan.is_overlay_control() && !copy_seen) ||
            !plan.native_overlay_program() ||
            !plan.native_overlay_program()->ApplyAtPacket(packet.dword_offset,
                                                          overlay_requested)) {
          return false;
        }
        break;
      case Plan::PacketKind::kDraw: {
        if (draw_index >= plan.inventory().draws.size()) {
          return false;
        }
        const auto& draw = plan.inventory().draws[draw_index++];
        if (draw.packet_dword_offset != packet.dword_offset) {
          return false;
        }
        regs.values[XE_GPU_REG_VGT_DRAW_INITIATOR] = draw.initiator;
        known.set(XE_GPU_REG_VGT_DRAW_INITIATOR);
        if (!ClosedStateAtDraw(regs, known, packet.dword_offset)) {
          return false;
        }
        copy_seen = regs.Get<reg::RB_MODECONTROL>().edram_mode ==
                    xenos::EdramMode::kCopy;
        break;
      }
      case Plan::PacketKind::kCoherencyWait:
        // The admitted shaders have no memexport, all CPU inputs are frozen
        // before entry, and the sole guest-memory write is the final copy.
        // A post-copy guest visibility wait requires a different proven path.
        if (copy_seen) {
          return false;
        }
        {
          const auto wait = std::find_if(
              plan.coherency_waits().begin(), plan.coherency_waits().end(),
              [&](const auto& value) {
                return value.packet_dword_offset == packet.dword_offset;
              });
          if (wait == plan.coherency_waits().end() ||
              regs.values[XE_GPU_REG_COHER_STATUS_HOST] !=
                  (wait->status |
                   (coherency_status_dirty ? UINT32_C(0x80000000) : 0)) ||
              regs.values[XE_GPU_REG_COHER_BASE_HOST] != wait->base ||
              regs.values[XE_GPU_REG_COHER_SIZE_HOST] != wait->size) {
            return false;
          }
          regs.values[XE_GPU_REG_COHER_STATUS_HOST] = 0;
          coherency_status_dirty = false;
        }
        break;
      case Plan::PacketKind::kShaderLoad:
      case Plan::PacketKind::kInlineShader:
        if (shader_index >= request.shaders().size() ||
            request.shaders()[shader_index++].load.packet_dword_offset !=
                packet.dword_offset) {
          return false;
        }
        break;
      case Plan::PacketKind::kShaderInvalidation:
      case Plan::PacketKind::kCacheFlush:
      case Plan::PacketKind::kNop:
        break;
      default:
        return false;
    }
  }
  return write_index == writes.size() &&
         draw_index == plan.inventory().draws.size() &&
         shader_index == request.shaders().size() &&
         (plan.is_overlay_control() ? !copy_seen : copy_seen) &&
         (!plan.native_overlay_program() ||
          overlay_requested == plan.native_overlay_program()->Apply({}));
}

bool ShaderMatchesOwnedProgram(const Shader& actual,
                               const Request::Shader& frozen) {
  // The existing cache key is only a content hash. Exact stage and canonical
  // words must still match the analyzed program, including a cache hit.
  if (actual.type() != frozen.load.stage ||
      actual.ucode_dword_count() != frozen.load.dword_count ||
      frozen.code.size() != size_t(frozen.load.dword_count) * 4) {
    return false;
  }
  const auto& canonical = actual.ucode_data();
  for (size_t i = 0; i < canonical.size(); ++i) {
    const uint8_t* bytes = frozen.code.data() + i * 4;
    const uint32_t expected = (uint32_t(bytes[0]) << 24) |
                              (uint32_t(bytes[1]) << 16) |
                              (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
    if (canonical[i] != expected) {
      return false;
    }
  }
  return true;
}

// The completion queue retains guest leases only, never Vulkan-owning banks.
struct RetainedRequest {
  std::shared_ptr<const Request> request;
};
}  // namespace

void VulkanCommandProcessor::CompleteNativeFrameCallback(
    std::function<void(NativeFrameExecutionResult)> completed,
    NativeFrameExecutionResult result) noexcept {
  if (!completed) {
    return;
  }
  try {
    completed(std::move(result));
  } catch (...) {
    // The CP retains the exact source independently of a host callback. Never
    // unwind into the guest ring or release submitted leases from this path.
    try {
      XELOGE("Native frame result callback threw; source remains retained");
    } catch (...) {
    }
  }
}

bool VulkanCommandProcessor::HasNativeFrameGpuStopRetention() const {
  const auto* device = GetVulkanDevice();
  const auto proof = gpu_completions_.backend_stop_proof();
  return native_shutdown_retention_installed_ &&
         !native_shutdown_in_progress_ && !native_submissions_sealed_ &&
         device && device->device() != VK_NULL_HANDLE &&
         device->functions().vkDeviceWaitIdle &&
         proof.backend_epoch() == gpu_completions_.backend_epoch() &&
         !proof.stopped();
}

void VulkanCommandProcessor::QueueNativeFrameAtSafeBoundary(
    std::shared_ptr<const NativeFrameExecutionRequest> request,
    std::function<void(NativeFrameExecutionResult)> completed) {
  Result rejected;
  rejected.status = Result::Status::kRejected;
  rejected.failing_stage = Result::Stage::kAdmission;
  bool admitted = false;
  try {
    admitted = request && request->IsAdmitted();
  } catch (...) {
  }
  TRACE_NATIVE_REQUEST(
      "gpu", "queue_admission_observed", request, {"admitted", admitted},
      {"fatal", native_frame_fatal_},
      {"halted", IsGuestCommandProcessingHalted()},
      {"shutdown_in_progress", native_shutdown_in_progress_},
      {"submissions_sealed", bool(native_submissions_sealed_)},
      {"queue_backend_epoch", gpu_completions_.backend_epoch()});
  if (!worker_thread_ || !kernel::XThread::IsInThread(worker_thread_.get()) ||
      native_frame_fatal_ || IsGuestCommandProcessingHalted() ||
      native_shutdown_in_progress_ || native_submissions_sealed_ || !admitted ||
      request->backend_epoch() != gpu_completions_.backend_epoch()) {
    CompleteNativeFrameCallback(std::move(completed), std::move(rejected));
    return;
  }
  if (native_frame_pending_) {
    rejected.status = Result::Status::kBusy;
    CompleteNativeFrameCallback(std::move(completed), std::move(rejected));
    return;
  }
  if (!HasNativeFrameGpuStopRetention()) {
    rejected.status = Result::Status::kUnsupported;
    rejected.failing_stage = Result::Stage::kRetirement;
    CompleteNativeFrameCallback(std::move(completed), std::move(rejected));
    return;
  }
  if (native_frame_retained_) {
    if (!native_frame_retained_restored_ || !native_frame_retained_submitted_ ||
        native_frame_retained_completion_.status() !=
            GpuCompletionStatus::kComplete) {
      rejected.status = native_frame_retained_restored_ &&
                                native_frame_retained_submitted_ &&
                                native_frame_retained_completion_.status() ==
                                    GpuCompletionStatus::kPending
                            ? Result::Status::kBusy
                            : Result::Status::kRejected;
      CompleteNativeFrameCallback(std::move(completed), std::move(rejected));
      return;
    }
    // Actual fence completion and CPU restoration permit replacing this host
    // hold. The kernel still performs its own exact Source retirement checks.
    native_frame_retained_.reset();
    native_frame_retained_completion_ = {};
    native_frame_retained_written_range_ = {};
    native_frame_retained_snapshot_output_.clear();
    native_frame_retained_submitted_ = false;
    native_frame_retained_restored_ = false;
  }
  native_frame_pending_ = std::move(request);
  native_frame_pending_completed_ = std::move(completed);
  TRACE_NATIVE_REQUEST("gpu", "queue_stored", native_frame_pending_,
                       {"retained_previous", bool(native_frame_retained_)});
  native_frame_waiting_capacity_ = false;
  RefreshNativeFrameCapacity();
}

void VulkanCommandProcessor::CancelPendingNativeFrame() noexcept {
  auto request = std::move(native_frame_pending_);
  auto completed = std::move(native_frame_pending_completed_);
  if (!request && !completed) {
    return;
  }
  Result rejected;
  rejected.status = Result::Status::kRejected;
  rejected.failing_stage = Result::Stage::kAdmission;
  rejected.title_state_restored =
      !native_guest_render_bank_selected_ &&
      register_file_ == graphics_system_->register_file();
  CompleteNativeFrameCallback(std::move(completed), std::move(rejected));
  // No MarkSubmissionAttempted call was made for this pending source.
}

void VulkanCommandProcessor::RetainFailedNativeFrameGpuWork(
    std::shared_ptr<const NativeFrameExecutionRequest> request,
    uint64_t first_native_submission, uint64_t latest_native_submission,
    bool all_work_submitted) noexcept {
  // The safe-point wrapper assigns this hold before entry. This fallback
  // increments an existing shared control block only; it allocates no failure
  // container.
  if (!native_frame_retained_) {
    native_frame_retained_ = std::move(request);
  }
  if (latest_native_submission >= first_native_submission) {
    native_frame_last_submission_ =
        std::max(native_frame_last_submission_, latest_native_submission);
  }
  (void)all_work_submitted;
  native_gpu_work_ever_attempted_ = true;
  native_frame_fatal_ = true;
  HaltGuestCommandProcessing();
  // Keep both banks and common layouts in this CP. A failed ticket, a missing
  // ticket or CPU restoration never destroys them. Shutdown must stop the GPU
  // before child destruction; the fatal signal is delivered after the result.
}

void VulkanCommandProcessor::PumpQueuedNativeFramesAtSafePoint() {
  try {
    if (!native_frame_boundary_active_ && !native_shutdown_in_progress_ &&
        !native_submissions_sealed_ && !native_frame_fatal_) {
      PollGpuCompletions();
      if (native_frame_pending_) {
        DrainNativeFrameAtSafePoint();
      }
    }
  } catch (...) {
    RetainFailedNativeFrameGpuWork(
        native_frame_pending_ ? native_frame_pending_ : native_frame_retained_,
        GetCurrentSubmission(), GetCurrentSubmission() - 1, false);
    if (native_frame_pending_) {
      auto callback = std::move(native_frame_pending_completed_);
      native_frame_pending_.reset();
      Result failed;
      failed.status = Result::Status::kFailed;
      failed.failing_stage = Result::Stage::kRetirement;
      CompleteNativeFrameCallback(std::move(callback), std::move(failed));
    }
  }
  RefreshNativeFrameCapacity();
  SignalNativeFrameFailure();
}

void VulkanCommandProcessor::DrainNativeFrameAtSafePoint() {
  if (!native_frame_pending_) {
    return;
  }
  auto request = std::move(native_frame_pending_);
  auto completed = std::move(native_frame_pending_completed_);
  Result result;
  result.status = Result::Status::kRejected;
  result.failing_stage = Result::Stage::kAdmission;
  bool admitted = false;
  try {
    admitted = request->IsAdmitted();
  } catch (...) {
  }
  if (!native_frame_fatal_ && !IsGuestCommandProcessingHalted() &&
      HasNativeFrameGpuStopRetention() && admitted) {
    // This assignment is before every possible native attempt or exception.
    // No success/failure command count is used as a lifetime decision.
    native_frame_retained_ = request;
    native_frame_retained_completion_ = {};
    native_frame_retained_written_range_ = {};
    native_frame_retained_snapshot_output_.clear();
    native_frame_retained_submitted_ = false;
    native_frame_retained_restored_ = false;
    native_gpu_work_ever_attempted_ = true;
    native_frame_boundary_active_ = true;
    RefreshNativeFrameCapacity();
    try {
      auto* presenter = graphics_system_->presenter();
      if (presenter &&
          !presenter->BeginNativeGpuCohort(gpu_completions_.backend_epoch())) {
        result.status = Result::Status::kUnsupported;
        result.failing_stage = Result::Stage::kRetirement;
        result.title_state_restored =
            !native_guest_render_bank_selected_ &&
            register_file_ == graphics_system_->register_file();
      } else {
        result = ExecutePreparedNativeFrame(request);
      }
    } catch (...) {
      result.status = Result::Status::kFailed;
      result.failing_stage = Result::Stage::kRetirement;
      RetainFailedNativeFrameGpuWork(request, GetCurrentSubmission(),
                                     GetCurrentSubmission() - 1, false);
    }
    native_frame_boundary_active_ = false;
    native_frame_retained_completion_ = result.completion;
    native_frame_retained_written_range_ = result.written_range;
    native_frame_retained_submitted_ =
        result.status == Result::Status::kSubmitted;
    native_frame_retained_restored_ = result.title_state_restored;
    if (result.status == Result::Status::kBusy) {
      // This typed result is emitted only by the pre-Mark capacity boundary.
      // No native source GPU work used this Request. The caller retains its
      // exact frozen Source; do not leave an unsupported completion hold that
      // would cause QueueNativeFrameAtSafeBoundary to reject every retry.
      // Failures/uncertain attempts keep their conservative CP hold unchanged.
      native_frame_retained_.reset();
      native_frame_retained_completion_ = {};
      native_frame_retained_written_range_ = {};
      native_frame_retained_snapshot_output_.clear();
      native_frame_retained_submitted_ = false;
      native_frame_retained_restored_ = false;
    }
  }
  TRACE_NATIVE_REQUEST(
      "gpu", "execution_result", request,
      {"result_status", uint32_t(result.status)},
      {"failing_stage", uint32_t(result.failing_stage)},
      {"failing_packet_dword", result.failing_packet_dword_offset},
      {"draw_commands", result.draw_commands},
      {"indexed_draw_commands", result.indexed_draw_commands},
      {"copy_operations", result.copy_operations},
      {"clear_operations", result.clear_operations},
      {"written_address", result.written_range.address},
      {"written_length", result.written_range.length},
      {"title_state_restored", result.title_state_restored},
      {"submission", result.completion.submission()},
      {"completion_status", uint32_t(result.completion.status())},
      {"fatal", native_frame_fatal_});
  if (result.status == Result::Status::kBusy && !native_frame_fatal_) {
    // The exact unconsumed Source remains frozen/owned. Progress is retried by
    // real CP safe points after fence/capacity changes, never by a host Task.
    native_frame_pending_ = std::move(request);
    native_frame_pending_completed_ = std::move(completed);
    native_frame_waiting_capacity_ = true;
  } else {
    native_frame_waiting_capacity_ = false;
    CompleteNativeFrameCallback(std::move(completed), std::move(result));
  }
  RefreshNativeFrameCapacity();
  SignalNativeFrameFailure();
}

void VulkanCommandProcessor::SignalNativeFrameFailure() noexcept {
  if (!native_frame_fatal_ || native_frame_fatal_notified_) {
    return;
  }
  native_frame_fatal_notified_ = true;
  if (native_frame_fatal_) {
    // The no-allocation retention hook has already halted the ring and kept
    // the entire CP alive. Publish a durable diagnostic before the existing
    // host fatal path, which quick-exits without destroying GPU children.
    try {
      XELOGE(
          "Native frame GPU state cannot resume safely; source and Vulkan "
          "banks retained, last native submission {}",
          native_frame_last_submission_);
      xe::FlushLog();
      graphics_system_->OnHostGpuLossFromAnyThread(false);
    } catch (...) {
      // Diagnostic/UI allocation failure must not unwind the CP worker past
      // its real-stop shutdown and permit unsafe Vulkan child destruction.
      std::quick_exit(EXIT_FAILURE);
    }
  }
}

void VulkanCommandProcessor::StopNativeGpuBeforeDestruction() {
  SetNativeFrameCapacityState(NativeSystemFrameCapacity::State::kStopping);
  // This runs on the real CP worker before Kernel/Memory teardown. Complete a
  // final submit attempt before sealing; an open suffix can contain auxiliary
  // uploads/sparse binds even when no main draw was emitted.
  auto fail_without_destruction = [](const char* reason) {
    try {
      XELOGE(
          "Native GPU shutdown cannot prove resource retirement: {}. "
          "Exiting before Vulkan, Kernel or Memory destruction.",
          reason);
      xe::FlushLog();
    } catch (...) {
    }
    std::quick_exit(EXIT_FAILURE);
  };
  // No UI paint/refresh/capture may submit after the device-wide stop proof.
  // Sealing drains active host recording callbacks without dropping GPU refs.
  if (auto* presenter = graphics_system_->presenter()) {
    try {
      if (!presenter->SealForBackendStop()) {
        fail_without_destruction("Presenter submission seal rejected");
      }
    } catch (...) {
      fail_without_destruction("Presenter submission seal threw");
    }
  }
  if (!native_submissions_sealed_) {
    if (submission_open_) {
      try {
        EndSubmission(false);
      } catch (...) {
        // Actual device-wide idle below covers an uncertain submitted prefix.
      }
    }
    // Remove and drain fault callbacks before the stop proof. Their direct
    // queue submits bypass Begin/EndSubmission; unregister waits every active
    // read callback, including its fence wait, while the device remains live.
    try {
      ResetResolveReadWatch();
    } catch (...) {
      fail_without_destruction("resolve read-watch drain failed");
    }
    native_submissions_sealed_.store(true, std::memory_order_release);
  }
  VkResult wait_result = VK_ERROR_INITIALIZATION_FAILED;
  for (uint32_t attempt = 0; attempt < 3; ++attempt) {
    try {
      wait_result = GetVulkanDevice()->WaitIdleForResourceRetirement();
    } catch (...) {
      wait_result = VK_ERROR_UNKNOWN;
    }
    if (wait_result == VK_SUCCESS || wait_result == VK_ERROR_DEVICE_LOST) {
      break;
    }
    try {
      XELOGE("Native GPU shutdown device-wide wait attempt {} failed: {}",
             attempt + 1, int32_t(wait_result));
    } catch (...) {
    }
  }
  if (wait_result != VK_SUCCESS && wait_result != VK_ERROR_DEVICE_LOST) {
    fail_without_destruction("three device-wide waits failed");
  }
  try {
    if (wait_result == VK_ERROR_DEVICE_LOST) {
      // A device-loss stop proof releases resources, never successful work.
      gpu_completions_.FailPending();
      gpu_completions_.ReleaseAfterDeviceLostWaitIdle();
    } else {
      if (!device_lost_ && !GetVulkanDevice()->IsLost()) {
        completion_timeline_.UpdateCompletedSubmission();
        if (completion_timeline_.last_completion_error() == VK_SUCCESS) {
          gpu_completions_.CompleteThrough(GetCompletedSubmission());
        }
      }
      gpu_completions_.ReleaseAfterGpuStopped();
    }
  } catch (...) {
    // Observer failures cannot invalidate an already-published stop proof.
    // Without that proof no guest or Vulkan child may be released.
    if (!gpu_completions_.backend_stop_proof().stopped()) {
      fail_without_destruction("stop publication failed");
    }
  }
  const auto proof = gpu_completions_.backend_stop_proof();
  if (!proof.stopped() ||
      proof.backend_epoch() != gpu_completions_.backend_epoch()) {
    fail_without_destruction("backend stop epoch mismatch");
  }
  if (auto* presenter = graphics_system_->presenter()) {
    try {
      if (!presenter->ReleaseNativeUiPlaneAfterBackendStopped(proof)) {
        fail_without_destruction("Presenter image retirement rejected");
      }
    } catch (...) {
      fail_without_destruction("Presenter image retirement threw");
    }
  }
  // A permanently sealed, actually stopped GPU permits discarding a failed
  // deferred suffix and restoring only CPU identities for safe destruction.
  deferred_command_buffer_.Reset();
  submission_open_ = false;
  in_render_pass_ = false;
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_ = {};
  if (native_guest_render_bank_selected_) {
    try {
      if (!SelectNativeGuestRenderBankAtClosedSubmission(false, true)) {
        fail_without_destruction("stopped native bank restore rejected");
      }
    } catch (...) {
      fail_without_destruction("stopped native bank restore threw");
    }
  }
  native_frame_retained_.reset();
  native_frame_retained_completion_ = {};
  native_frame_retained_written_range_ = {};
  native_frame_retained_snapshot_output_.clear();
  native_frame_retained_submitted_ = false;
  if (!RestoreTitleQueries(true)) {
    fail_without_destruction("stopped title query context restore rejected");
  }
  native_shutdown_retention_installed_ = false;
}

NativeFrameExecutionResult VulkanCommandProcessor::ExecutePreparedNativeFrame(
    std::shared_ptr<const NativeFrameExecutionRequest> request) {
  Result result;
  result.status = Result::Status::kRejected;
  result.failing_stage = Result::Stage::kAdmission;
  if (!worker_thread_ || !kernel::XThread::IsInThread(worker_thread_.get()) ||
      !request || !request->IsAdmitted() ||
      request->backend_epoch() != gpu_completions_.backend_epoch() ||
      !native_frame_boundary_active_ || native_guest_render_bank_selected_ ||
      device_lost_ || GetVulkanDevice()->IsLost() ||
      completion_timeline_.last_completion_error() != VK_SUCCESS ||
      (!request->plan()->is_overlay_control() &&
       !request->plan()->has_final_copy())) {
    return result;
  }
  // The title logical query/report context will be parked before selection;
  // its normal retirement pump cannot publish through the private native RF.
  // CPU restoration or a Failed token is not authority to destroy submitted
  // Vulkan bank resources. A real stop policy must already be installed.
  if (!HasNativeFrameGpuStopRetention()) {
    result.status = Result::Status::kUnsupported;
    result.failing_stage = Result::Stage::kRetirement;
    return result;
  }
  if (!PreflightNativeState(*request, result.failing_packet_dword_offset)) {
    result.failing_stage = Result::Stage::kState;
    return result;
  }
  // Display permission is independent of one-use Source execution. Capture
  // resident permission before native work; withdrawal only suppresses later
  // publication, never an otherwise complete Source submission.
  auto* presentation_host = graphics_system_->presenter();
  auto presentation_authority =
      presentation_host
          ? presentation_host->GetNativeUiPlaneAuthority(
                request->identity().owner, request->backend_epoch())
          : nullptr;
  const bool control = request->plan()->is_overlay_control();
  const bool presentation_requested = !control && bool(presentation_authority);
  // Allocate all host-only ownership wrappers before consuming the request.
  std::vector<std::vector<uint32_t>> shader_words;
  shader_words.reserve(request->shaders().size());
  for (const auto& shader : request->shaders()) {
    auto& aligned = shader_words.emplace_back(shader.load.dword_count);
    std::memcpy(aligned.data(), shader.code.data(), shader.code.size());
  }
  auto retained = std::make_shared<RetainedRequest>(RetainedRequest{request});
  GpuCompletionResources resources;
  resources.reserve(1);
  resources.emplace_back(retained);

  const bool immutable_inputs = request->input_provenance() ==
                                NativeFrameInputProvenance::kImmutableSnapshot;
  // Native trace readers currently serialize live RAM. Never reintroduce it.
  if (immutable_inputs && trace_writer_.is_open()) {
    result.status = Result::Status::kRejected;
    result.failing_stage = Result::Stage::kAdmission;
    return result;
  }
  const auto allocation =
      AllocateNativeGuestRenderBankForInput(immutable_inputs);
  if (allocation.status != GuestRenderBankAllocationResult::Status::kReady) {
    result.status = Result::Status::kFailed;
    result.failing_stage = Result::Stage::kState;
    return result;
  }
  auto* const title_rf = register_file_;
  auto* const title_shared_memory = shared_memory_.get();
  auto* const title_mmio_rf = graphics_system_->register_file();
  auto* const title_rt = render_target_cache_.get();
  auto* const title_pipeline = pipeline_cache_.get();
  auto* const title_primitive = primitive_processor_.get();
  auto* const title_texture = texture_cache_.get();
  Shader* const title_vs = active_vertex_shader_;
  Shader* const title_ps = active_pixel_shader_;
  const uint32_t title_vs_address = active_vertex_shader_ucode_address_;
  const uint64_t title_frame = frame_current_;
  const uint64_t title_completed_frame = frame_completed_;
  const bool title_frame_was_open = frame_open_;
  std::array<uint64_t, kMaxFramesInFlight> title_closed_frames;
  std::copy(std::begin(closed_frame_submissions_),
            std::end(closed_frame_submissions_), title_closed_frames.begin());
  const uint64_t title_bin_mask = bin_mask_, title_bin_select = bin_select_;

  VulkanNativeResourceEpoch resource_epoch;
  // Close only the preceding TITLE submission. This is not the native token,
  // not EndFrame, and never swap/vblank/IRQ publication.
  try {
    const auto parked = ParkTitleQueries(*request);
    TRACE_NATIVE_REQUEST("gpu", "title_query_park_result", request,
                         {"park_status", uint32_t(parked)});
    if (parked == NativeQueryParkStatus::kBusy) {
      result.status = Result::Status::kBusy;
      result.failing_stage = Result::Stage::kRetirement;
      result.title_state_restored = register_file_ == title_rf;
      return result;
    }
    if (parked != NativeQueryParkStatus::kReady) {
      result.status = Result::Status::kFailed;
      result.failing_stage = Result::Stage::kSubmission;
      result.title_state_restored = register_file_ == title_rf;
      // EndSubmission may already have queued sparse binds. The existing
      // title failure is not converted to an empty/native rollback.
      RetainFailedNativeFrameGpuWork(request, GetCurrentSubmission(),
                                     GetCurrentSubmission() - 1, false);
      return result;
    }
    const auto begin_epoch = BeginNativeResourceEpoch(*request);
    TRACE_NATIVE_REQUEST("gpu", "resource_epoch_begin_result", request,
                         {"epoch_status", uint32_t(begin_epoch.status)});
    if (begin_epoch.status != NativeResourceEpochBeginStatus::kReady) {
      result.status =
          begin_epoch.status == NativeResourceEpochBeginStatus::kBusy
              ? Result::Status::kBusy
              : Result::Status::kRejected;
      result.failing_stage = Result::Stage::kRetirement;
      result.title_state_restored = RestoreTitleQueries();
      if (!result.title_state_restored) {
        RetainFailedNativeFrameGpuWork(request, GetCurrentSubmission(),
                                       GetCurrentSubmission() - 1, false);
        result.status = Result::Status::kFailed;
      }
      return result;
    }
    resource_epoch = begin_epoch.epoch;
    if (submission_open_ ||
        !SelectNativeGuestRenderBankAtClosedSubmission(true)) {
      const bool canceled = CloseNativeResourceEpoch(
          resource_epoch, NativeResourceEpochClose::kCanceledBeforeGpu, 0, 0);
      const bool queries_restored = canceled &&
                                    !native_guest_render_bank_selected_ &&
                                    RestoreTitleQueries();
      if (!queries_restored) {
        RetainFailedNativeFrameGpuWork(request, GetCurrentSubmission(),
                                       GetCurrentSubmission() - 1, false);
      }
      result.status = Result::Status::kFailed;
      result.failing_stage = Result::Stage::kState;
      result.title_state_restored =
          queries_restored && register_file_ == title_rf;
      return result;
    }
  } catch (...) {
    // Selection has no GPU commands, but allocation/BeginFrame failure may
    // leave a partial CPU bank selection. Never resume title through it.
    result.status = Result::Status::kFailed;
    result.failing_stage = Result::Stage::kState;
    RetainFailedNativeFrameGpuWork(request, GetCurrentSubmission(),
                                   GetCurrentSubmission() - 1, false);
    return result;
  }
  if (immutable_inputs) {
    const auto& snapshot = request->input_snapshot();
    bool bound = false;
    try {
      bound = snapshot && snapshot->identity() == request->identity() &&
              snapshot->plan() == request->plan() &&
              shared_memory_->BindNativeSnapshot(snapshot,
                                                 request->output_range()) &&
              texture_cache_->InvalidateAllDataFromExternalSource();
    } catch (...) {
      bound = false;
    }
    if (!bound) {
      const bool closed = CloseNativeResourceEpoch(
          resource_epoch, NativeResourceEpochClose::kCanceledBeforeGpu, 0, 0);
      const bool restored =
          closed && SelectNativeGuestRenderBankAtClosedSubmission(false) &&
          RestoreTitleQueries();
      result.title_state_restored = restored;
      result.status =
          restored ? Result::Status::kRejected : Result::Status::kFailed;
      result.failing_stage = Result::Stage::kAdmission;
      if (!restored) {
        RetainFailedNativeFrameGpuWork(request, GetCurrentSubmission(),
                                       GetCurrentSubmission() - 1, false);
      }
      return result;
    }
  }
  const uint64_t first_native_submission = GetCurrentSubmission();
  bool attempted = false, decoded = false, all_work_submitted = false;
  // Host-only aggregate observations. The last uploaded shader maps and UBO
  // current bits are the same ones used by WriteRegister/UpdateBindings.
  struct FloatDirtyCounts {
    uint64_t writes = 0, changed = 0, changed_used = 0;
    uint64_t current_before = 0, invalidated_after = 0, current_after = 0;
    uint64_t vertex_current_before = 0, vertex_invalidated_after = 0;
    uint64_t pixel_current_before = 0, pixel_invalidated_after = 0;
  } float_dirty;
  const bool observe_float_dirty = cvars::native_system_ui_trace;
  NativePresentationImage* presentation_image = nullptr;
  result.status = Result::Status::kFailed;
  result.failing_stage = Result::Stage::kAdmission;
  try {
    // Final owner/epoch/one-use check immediately before native GPU work.
    // Mark may have made source uncertainty sticky before a host exception.
    // Treat an exception as attempted; only an explicit false clears it.
    attempted = true;
    if (!request->MarkSubmissionAttempted()) {
      attempted = false;
    }
    TRACE_NATIVE_REQUEST("gpu", "source_submission_marked", request,
                         {"attempted", attempted},
                         {"presentation_requested", presentation_requested},
                         {"title_frame_was_open", title_frame_was_open},
                         {"first_native_submission", first_native_submission});
    if (attempted) {
      // The consumer-owned initial bank comes from this exact completed input,
      // never the live Title RF or reset values. These writes also invalidate
      // the private texture-cache binding through the normal backend hook.
      if (const auto& input = request->plan()->title_input()) {
        for (uint32_t i = 0; i < 6; ++i) {
          WriteRegister(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 15 * 6 + i,
                        input->fetch()[i]);
        }
      }
      // Native logical frames have their own cache age. No title frame is
      // advanced/opened to reset bindings or age changing private RT keys.
      render_target_cache_->BeginFrame();
      render_target_cache_->AdvanceRenderTargetUseFrame();
      texture_cache_->BeginFrame();
      result.failing_stage = Result::Stage::kSubmission;
      // Even a later CPU-side failure gets a dedicated actual native submit,
      // never a token for the previous title fence. This covers auxiliary
      // upload/transfer work when no main draw is eventually emitted.
      if (BeginSubmission(true)) {
        decoded = true;
        size_t write_index = 0, shader_index = 0, draw_index = 0;
        const auto& plan = *request->plan();
        const auto& words = plan.words();
        const auto& writes = plan.register_writes();
        std::optional<uint32_t> selected_vertex, selected_pixel;
        bool coherency_status_dirty = false;
        NativeOverlayState overlay_requested;
        for (const auto& packet : plan.packets()) {
          result.failing_packet_dword_offset = packet.dword_offset;
          result.failing_stage = Result::Stage::kPacket;
          switch (packet.kind) {
            case Plan::PacketKind::kRegisterWrite:
              while (write_index < writes.size() &&
                     writes[write_index].packet_dword_offset ==
                         packet.dword_offset) {
                const auto& write = writes[write_index++];
                // Only the plan's render/constant/cache-control allowlist can
                // reach this. No scratch/gamma/fence/IRQ registers are allowed.
                uint32_t observed_current_float_mask = 0;
                bool observed_vertex_float = false;
                if (observe_float_dirty &&
                    write.index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
                    write.index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
                  ++float_dirty.writes;
                  if (register_file_->values[write.index] != write.value) {
                    ++float_dirty.changed;
                    uint32_t index =
                        (write.index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
                    observed_vertex_float = index < 256;
                    const auto* map = observed_vertex_float
                                          ? current_float_constant_map_vertex_
                                          : current_float_constant_map_pixel_;
                    index &= 255;
                    if (map[index >> 6] & (UINT64_C(1) << (index & 63))) {
                      ++float_dirty.changed_used;
                      const uint32_t mask =
                          UINT32_C(1) << (observed_vertex_float
                                              ? SpirvShaderTranslator::
                                                    kConstantBufferFloatVertex
                                              : SpirvShaderTranslator::
                                                    kConstantBufferFloatPixel);
                      if (current_constant_buffers_up_to_date_ & mask) {
                        observed_current_float_mask = mask;
                        ++float_dirty.current_before;
                        if (observed_vertex_float) {
                          ++float_dirty.vertex_current_before;
                        } else {
                          ++float_dirty.pixel_current_before;
                        }
                      }
                    }
                  }
                }
                // Observation never suppresses/replaces the real write. It
                // performs no guest read, float arithmetic or per-write log.
                WriteRegister(write.index, write.value);
                if (write.index == XE_GPU_REG_COHER_STATUS_HOST) {
                  coherency_status_dirty = true;
                }
                if (observed_current_float_mask) {
                  if (current_constant_buffers_up_to_date_ &
                      observed_current_float_mask) {
                    ++float_dirty.current_after;
                  } else {
                    ++float_dirty.invalidated_after;
                    if (observed_vertex_float) {
                      ++float_dirty.vertex_invalidated_after;
                    } else {
                      ++float_dirty.pixel_invalidated_after;
                    }
                  }
                }
              }
              break;
            case Plan::PacketKind::kNativeOverlayControl:
              // Complete typed native display transactions are reduced only
              // in this private requested-state value. Never WriteRegister:
              // no global title MMIO, CPU ACK or scanout event is fabricated.
              decoded = plan.native_overlay_program() &&
                        plan.native_overlay_program()->ApplyAtPacket(
                            packet.dword_offset, overlay_requested);
              break;
            case Plan::PacketKind::kShaderLoad:
            case Plan::PacketKind::kInlineShader: {
              result.failing_stage = Result::Stage::kShader;
              const uint32_t index = uint32_t(shader_index++);
              const auto& shader = request->shaders()[index];
              Shader* actual =
                  LoadShader(shader.load.stage, shader_words[index].data(),
                             shader.load.dword_count);
              if (!actual || !ShaderMatchesOwnedProgram(*actual, shader)) {
                decoded = false;
                break;
              }
              if (shader.load.stage == xenos::ShaderType::kVertex) {
                active_vertex_shader_ = actual;
                active_vertex_shader_ucode_address_ =
                    shader.load.is_inline ? 0 : shader.load.physical_address;
                selected_vertex = index;
              } else {
                active_pixel_shader_ = actual;
                selected_pixel = index;
              }
              break;
            }
            case Plan::PacketKind::kShaderInvalidation: {
              const uint32_t mask = words[packet.dword_offset + 1];
              if (mask & xenos::XE_GPU_INVALIDATE_MASK_VERTEX_SHADER) {
                active_vertex_shader_ = nullptr;
                active_vertex_shader_ucode_address_ = 0;
                selected_vertex.reset();
              }
              if (mask & xenos::XE_GPU_INVALIDATE_MASK_PIXEL_SHADER) {
                active_pixel_shader_ = nullptr;
                selected_pixel.reset();
              }
              break;
            }
            case Plan::PacketKind::kCoherencyWait:
              // Used inputs are frozen/pinned and uploaded/resident by the
              // strict draw's normal SharedMemory paths. No memexport or
              // post-copy write wait is admitted. Clear only private status;
              // this CPU cache-control transition never completes GPU work.
              {
                const auto wait = std::find_if(
                    plan.coherency_waits().begin(),
                    plan.coherency_waits().end(), [&](const auto& value) {
                      return value.packet_dword_offset == packet.dword_offset;
                    });
                decoded =
                    wait != plan.coherency_waits().end() &&
                    register_file_->values[XE_GPU_REG_COHER_STATUS_HOST] ==
                        (wait->status |
                         (coherency_status_dirty ? UINT32_C(0x80000000) : 0)) &&
                    register_file_->values[XE_GPU_REG_COHER_BASE_HOST] ==
                        wait->base &&
                    register_file_->values[XE_GPU_REG_COHER_SIZE_HOST] ==
                        wait->size;
                if (decoded) {
                  // Use the common dirty->coherent transition in the selected
                  // private bank. This is not GPU completion or a guest ACK.
                  CommandProcessor::MakeCoherent();
                  decoded =
                      register_file_->values[XE_GPU_REG_COHER_STATUS_HOST] == 0;
                  coherency_status_dirty = false;
                }
              }
              break;
            case Plan::PacketKind::kCacheFlush:
              register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR] =
                  xenos::CACHE_FLUSH;
              break;
            case Plan::PacketKind::kDraw: {
              const auto& draw = plan.inventory().draws[draw_index++];
              result.failing_stage = Result::Stage::kState;
              const auto mode =
                  register_file_->Get<reg::RB_MODECONTROL>().edram_mode;
              if (mode != xenos::EdramMode::kCopy &&
                  (selected_vertex != draw.vertex_shader ||
                   (mode == xenos::EdramMode::kColorDepth &&
                    selected_pixel != draw.pixel_shader))) {
                decoded = false;
                break;
              }
              // The normal PM4 AutoIndex path derives this register from the
              // DRAW operand; no DMA/index-memory register is manufactured.
              register_file_->values[XE_GPU_REG_VGT_DRAW_INITIATOR] =
                  draw.initiator;
              const auto initiator =
                  register_file_->Get<reg::VGT_DRAW_INITIATOR>();
              VulkanRenderTargetCache::NativeResolveExpectation resolve;
              resolve.physical_address = request->output_range().address;
              resolve.length = request->output_range().length;
              resolve.format = xenos::ColorFormat::k_8_8_8_8;
              resolve.require_copy = true;
              result.failing_stage = mode == xenos::EdramMode::kCopy
                                         ? Result::Stage::kCopy
                                         : Result::Stage::kDraw;
              const auto outcome = IssueNativeDraw(
                  initiator.prim_type, initiator.num_indices, nullptr, false,
                  mode == xenos::EdramMode::kCopy ? &resolve : nullptr);
              result.draw_commands += outcome.draw_commands;
              result.indexed_draw_commands += outcome.indexed_draw_commands;
              result.copy_operations += outcome.resolve.copy_commands;
              result.clear_operations += outcome.resolve.clear_commands;
              if (outcome.resolve.written_length) {
                result.written_range = {outcome.resolve.written_address,
                                        outcome.resolve.written_length};
              }
              if (!outcome.recorded()) {
                // Counts can be zero after texture/RT auxiliary commands.
                // Always commit the opened native submission below.
                decoded = false;
              }
              break;
            }
            case Plan::PacketKind::kNop:
              break;
            default:
              decoded = false;
              break;
          }
          if (!decoded) {
            break;
          }
        }
        if (decoded && plan.native_overlay_program()) {
          decoded =
              overlay_requested == plan.native_overlay_program()->Apply({});
        }
        if (decoded && presentation_requested) {
          result.failing_stage = Result::Stage::kCopy;
          presentation_image =
              BeginNativePresentationImage(request, presentation_authority);
          decoded = presentation_image &&
                    RecordNativePresentationCopy(*presentation_image, *request,
                                                 result.written_range);
        }
      }
    }
  } catch (const std::exception& error) {
    XELOGE("Native frame executor: host exception after admission: {}",
           error.what());
    decoded = false;
  } catch (...) {
    XELOGE("Native frame executor: unknown host exception after admission");
    decoded = false;
  }

  // No returns after admission skip this boundary. A failed draw can have
  // recorded auxiliary work; success counts are never a no-GPU-work proof.
  bool end_ok = true;
  if (attempted && submission_open_) {
    try {
      const uint64_t ending = GetCurrentSubmission();
      end_ok = EndSubmission(false) && !submission_open_ &&
               GetCurrentSubmission() > ending;
    } catch (...) {
      // The finalizer itself may have submitted a sparse bind or prefix.
      // Do not retry it or infer safety from zero main command counts.
      end_ok = false;
    }
  }
  // Private output visibility precedes the published Source fence. Close
  // the main suffix, await the strict helper fence with THIS bank selected,
  // then emit a fresh actual CP submission after helper/CPU visibility.
  if (immutable_inputs && attempted && decoded && end_ok && !submission_open_ &&
      result.written_range.length) {
    bool mirrored = false;
    try {
      {
        std::lock_guard<std::mutex> lock(resolve_read_mutex_);
        const std::vector<std::pair<uint32_t, uint32_t>> ranges = {
            {result.written_range.address, result.written_range.length}};
        native_frame_retained_snapshot_output_.resize(
            result.written_range.length);
        mirrored =
            shared_memory_->native_snapshot_valid() &&
            FaultCopyResolveToGuestRam(ranges, true, shared_memory_.get());
        if (mirrored && resolve_fault_readback_.mapped) {
          // Keep exact GPU bytes before unlocking the reusable helper. Reading
          // guest RAM later could observe a concurrent CPU writer instead.
          std::memcpy(native_frame_retained_snapshot_output_.data(),
                      resolve_fault_readback_.mapped,
                      result.written_range.length);
          // This physical write replaces any old Title resolve ownership.
          // Clear under the helper lock so no delayed Title fault can copy an
          // older buffer over the newly mirrored bytes after we unlock.
          SetResolvePageOwner(result.written_range.address,
                              result.written_range.length, 0);
        } else {
          mirrored = false;
        }
        if (!mirrored) {
          resolve_fault_copy_failed_.store(true, std::memory_order_release);
        }
      }
      if (mirrored) {
        // Physical write bypassed watch faults. Invalidate Title's stale copy.
        title_shared_memory->MemoryInvalidationCallback(
            result.written_range.address, result.written_range.length, true);
        const uint64_t final_submission = GetCurrentSubmission();
        if (BeginSubmission(true)) {
          PushBufferMemoryBarrier(
              shared_memory_->buffer(), result.written_range.address,
              result.written_range.length, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
              VK_ACCESS_MEMORY_READ_BIT);
          SubmitBarriers(true);
          mirrored = EndSubmission(false) && !submission_open_ &&
                     GetCurrentSubmission() > final_submission;
        } else {
          mirrored = false;
        }
      }
    } catch (...) {
      mirrored = false;
    }
    if (!mirrored) {
      end_ok = false;
      result.failing_stage = Result::Stage::kCopy;
    }
  }
  if (immutable_inputs && attempted &&
      (!decoded || (!control && !result.written_range.length))) {
    // A rendered snapshot Source needs its complete actual output. A typed
    // control Source has none; its fresh real submission/fence still gates the
    // whole-program success below, never a CPU ACK for an unfinished prefix.
    end_ok = false;
  }
  const uint64_t last_submitted = GetCurrentSubmission() - 1;
  all_work_submitted = attempted && end_ok && !submission_open_ &&
                       last_submitted >= first_native_submission;
  if (last_submitted >= first_native_submission) {
    native_frame_last_submission_ =
        std::max(native_frame_last_submission_, last_submitted);
    try {
      completion_timeline_.UpdateCompletedSubmission();
      if (device_lost_ || GetVulkanDevice()->IsLost() ||
          completion_timeline_.last_completion_error() != VK_SUCCESS) {
        gpu_completions_.FailPending();
      } else {
        gpu_completions_.CompleteThrough(GetCompletedSubmission());
      }
      if (all_work_submitted) {
        result.completion = gpu_completions_.TrackNativeFrameSubmitted(
            last_submitted, request->identity(), std::move(resources));
      } else {
        // A submitted prefix cannot impersonate a completion for the entire
        // source. Its resources remain queued, with the failed/open suffix
        // additionally retained by the stopped-GPU failure hook below.
        result.completion = gpu_completions_.TrackSubmitted(
            last_submitted, std::move(resources));
      }
    } catch (...) {
      all_work_submitted = false;
    }
  }
  if (attempted && !all_work_submitted) {
    result.failing_stage = Result::Stage::kSubmission;
  }
  const bool resource_epoch_closed = CloseNativeResourceEpoch(
      resource_epoch,
      !attempted           ? NativeResourceEpochClose::kCanceledBeforeGpu
      : all_work_submitted ? NativeResourceEpochClose::kSubmitted
                           : NativeResourceEpochClose::kUncertain,
      first_native_submission, last_submitted);
  if (!resource_epoch_closed) {
    all_work_submitted = false;
  }
  bool restored = false;
  if (!submission_open_ && resource_epoch_closed) {
    try {
      restored = SelectNativeGuestRenderBankAtClosedSubmission(false);
    } catch (...) {
      restored = false;
    }
  }
  result.title_state_restored =
      restored && RestoreTitleQueries() && register_file_ == title_rf &&
      graphics_system_->register_file() == title_mmio_rf &&
      shared_memory_.get() == title_shared_memory &&
      render_target_cache_.get() == title_rt &&
      pipeline_cache_.get() == title_pipeline &&
      primitive_processor_.get() == title_primitive &&
      texture_cache_.get() == title_texture &&
      active_vertex_shader_ == title_vs && active_pixel_shader_ == title_ps &&
      active_vertex_shader_ucode_address_ == title_vs_address &&
      frame_open_ == title_frame_was_open && frame_current_ == title_frame &&
      frame_completed_ == title_completed_frame &&
      std::equal(title_closed_frames.begin(), title_closed_frames.end(),
                 std::begin(closed_frame_submissions_)) &&
      bin_mask_ == title_bin_mask && bin_select_ == title_bin_select;
  if (!resource_epoch_closed || (attempted && !all_work_submitted) ||
      !result.title_state_restored) {
    // Block ordinary CP continuation and retain both banks and their common
    // layout dependencies until an actual device-wide GPU stop. A Failed
    // token alone never permits resource destruction.
    RetainFailedNativeFrameGpuWork(request, first_native_submission,
                                   last_submitted, all_work_submitted);
  }
  const bool complete_profile =
      decoded && NativeFrameOperationsMatchPlan(*request->plan(), result) &&
      (!presentation_requested ||
       (presentation_image && presentation_image->copy_recorded));
  if (complete_profile && all_work_submitted && result.title_state_restored &&
      result.completion.status() != GpuCompletionStatus::kFailed &&
      result.completion.status() != GpuCompletionStatus::kUnsupported) {
    result.status = Result::Status::kSubmitted;
    result.failing_stage = Result::Stage::kNone;
    result.failing_packet_dword_offset = UINT32_MAX;
  } else if (!attempted) {
    result.status = Result::Status::kRejected;
  } else {
    result.status = Result::Status::kFailed;
  }
  TRACE_NATIVE_REQUEST(
      "gpu", "float_constant_dirty_summary", request,
      {"title_frame_was_open", title_frame_was_open},
      {"result_status", uint32_t(result.status)},
      {"float_writes", float_dirty.writes},
      {"float_changed_writes", float_dirty.changed},
      {"float_changed_used_writes", float_dirty.changed_used},
      {"float_ubo_current_before", float_dirty.current_before},
      {"float_ubo_invalidated_after", float_dirty.invalidated_after},
      {"float_ubo_current_after", float_dirty.current_after},
      {"vs_float_ubo_current_before", float_dirty.vertex_current_before},
      {"vs_float_ubo_invalidated_after", float_dirty.vertex_invalidated_after},
      {"ps_float_ubo_current_before", float_dirty.pixel_current_before},
      {"ps_float_ubo_invalidated_after", float_dirty.pixel_invalidated_after});
  CommitNativePresentationImage(presentation_image, result);
  return result;
}
}  // namespace xe::gpu::vulkan
