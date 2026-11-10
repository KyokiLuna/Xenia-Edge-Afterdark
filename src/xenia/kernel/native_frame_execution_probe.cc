/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_frame_execution_probe.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_frame_readback_capture.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/presenter.h"

#if !XE_PLATFORM_WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

DECLARE_bool(native_frame_composite_probe);

namespace xe::kernel {
namespace {
class ProbePlaneAuthority final : public ui::NativeUiPlaneAuthority {
 public:
  ProbePlaneAuthority(std::shared_ptr<NativeFrameOwnerSession> session,
                      uint64_t backend_epoch, uint64_t foreground_generation)
      : session_(std::move(session)),
        owner_{session_->owner(), backend_epoch, foreground_generation} {}
  ui::NativeUiPlaneOwner owner() const noexcept override { return owner_; }
  bool IsLive() const noexcept override {
    try {
      return session_->IsPresentationOwnerLive(owner_.backend_epoch);
    } catch (...) {
      return false;
    }
  }

 private:
  const std::shared_ptr<NativeFrameOwnerSession> session_;
  const ui::NativeUiPlaneOwner owner_;
};
}  // namespace
bool RunNativeFrameExecutionProbe(
    KernelState* kernel, std::shared_ptr<NativeFrameOwnerSession> session,
    const NativeCompletedFrameRecord& record,
    const std::filesystem::path& capture_path) {
  using Result = gpu::NativeFrameExecutionResult;
  using Presentation = gpu::NativeFramePresentationStatus;
  auto* thread = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  if (!kernel || !session || !thread || thread->kernel_state() != kernel ||
      !thread->is_host_thread() ||
      thread->native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM) {
    return false;
  }
  bool retired = false;
  struct RevokeIncomplete {
    std::shared_ptr<NativeFrameOwnerSession> session;
    const bool& retired;
    ~RevokeIncomplete() {
      // Includes exceptions while allocating/queuing host observations after
      // possible GPU work. Never reinterpret zero draws as an unsubmitted
      // abort.
      if (!retired) {
        session->Revoke();
      }
    }
  } revoke_incomplete{session, retired};
  auto report_path = capture_path;
  report_path += ".execute.json";
  const auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(
      filesystem::OpenFile(report_path, "wbx"), &std::fclose);
  if (!file) {
    return false;
  }
#if !XE_PLATFORM_WIN32
  if (::fchmod(::fileno(file.get()), S_IRUSR | S_IWUSR)) {
    return false;
  }
#endif
  struct Observation {
    Result result;
    std::atomic<bool> returned{false};
  };
  const auto observation = std::make_shared<Observation>();
  const auto request =
      PrepareNativeFrameExecutionRequest(kernel, session, record, capture_path);
  const auto identity = thread->native_frame_thread_identity();
  const auto source =
      request ? session->SourceForExecutionRequest(identity, *request)
              : nullptr;
  auto* graphics = kernel->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  struct ReadbackObservation {
    gpu::NativeFrameReadbackResult result;
    std::atomic<bool> returned{false};
  };
  const auto readback = std::make_shared<ReadbackObservation>();
  const bool presentation_required = cvars::native_frame_composite_probe;
  auto* presenter = graphics ? graphics->presenter() : nullptr;
  std::shared_ptr<const ui::NativeUiPlaneAuthority> plane_authority;
  bool plane_bound = !presentation_required;
  if (presentation_required && request && presenter) {
    const auto foreground =
        kernel->emulator()->title_execution_state().snapshot();
    plane_authority = std::make_shared<ProbePlaneAuthority>(
        session, request->backend_epoch(), foreground.generation);
    plane_bound =
        presenter->BindNativeUiPlaneAuthority(plane_authority) &&
        presenter->SetNativeUiPlaneVisibility(plane_authority, 1, true);
  }
  bool presentation_requested = false, presentation_returned = false,
       presentation_handoff_complete = false;
  Presentation presentation;
  const char* presentation_error = "presentation_deadline";
  const auto await_presentation = [&]() {
    struct PresentationObservation {
      Presentation result;
      std::atomic<bool> returned{false};
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      if (!request->IsPresentationOwnerLive()) {
        presentation_error = "presentation_owner_ended";
        return false;
      }
      const auto observed = std::make_shared<PresentationObservation>();
      presentation_requested = true;
      cp->CaptureNativeFramePresentationAsync(
          request, [observed](Presentation result) {
            observed->result = std::move(result);
            observed->returned.store(true, std::memory_order_release);
          });
      while (!observed->returned.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        if (!request->IsPresentationOwnerLive()) {
          presentation_error = "presentation_owner_ended";
          return false;
        }
        thread->Delay(0, 0, uint64_t(-100000ll));
      }
      if (!observed->returned.load(std::memory_order_acquire)) {
        return false;
      }
      presentation_returned = true;
      presentation = observed->result;
      if (presentation.status == Presentation::Status::kUnsupported ||
          presentation.status == Presentation::Status::kFailed) {
        presentation_error = "presentation_unsupported_or_failed";
        return false;
      }
      if (presentation.status == Presentation::Status::kComplete) {
        const auto tag =
            presentation.composite_completion.native_frame_identity();
        presentation_handoff_complete =
            presentation.identity == request->identity() &&
            presentation.backend_epoch == request->backend_epoch() &&
            presentation.image_version && presentation.image_ready &&
            presentation.presenter_accepted &&
            presentation.native_write_submission ==
                observation->result.completion.submission() &&
            presentation.composite_submission &&
            presentation.presentation_backend_epoch &&
            presentation.composite_completion.backend_epoch() ==
                presentation.presentation_backend_epoch &&
            presentation.composite_completion.submission() ==
                presentation.composite_submission &&
            tag && *tag == request->identity() &&
            presentation.composite_completion.status() ==
                gpu::GpuCompletionStatus::kComplete;
        presentation_error = presentation_handoff_complete
                                 ? "none"
                                 : "presentation_completion_mismatch";
        return presentation_handoff_complete;
      }
      thread->Delay(0, 0, uint64_t(-100000ll));
    }
    return false;
  };
  const auto layout =
      request ? SnapshotNativeFrameReadbackLayout(kernel, *request, record)
              : std::nullopt;
  bool attached = false, readback_requested = false, readback_saved = false;
  const char* error = !plane_bound         ? "native_plane_authority_failed"
                      : request && !layout ? "output_layout_invalid"
                                           : "prepare_failed";
  if (request && source && cp && layout && plane_bound) {
    cp->ExecuteNativeFrameAsync(request, [observation](Result result) {
      // No stopped XThread, Kernel pointer or stack reference in this callback.
      observation->result = std::move(result);
      observation->returned.store(true, std::memory_order_release);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!observation->returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      thread->Delay(0, 0, uint64_t(-100000ll));
    }
    error = "swap_boundary_deadline";
    if (observation->returned.load(std::memory_order_acquire)) {
      const auto& result = observation->result;
      error = "execution_rejected_or_failed";
      const auto tag = result.completion.native_frame_identity();
      if (result.status == Result::Status::kSubmitted &&
          result.title_state_restored &&
          (result.draw_commands + result.indexed_draw_commands) &&
          result.copy_operations && tag && *tag == request->identity() &&
          result.completion.backend_epoch() == request->backend_epoch() &&
          result.written_range.length &&
          result.written_range.address >= request->output_range().address &&
          uint64_t(result.written_range.address) +
                  result.written_range.length <=
              uint64_t(request->output_range().address) +
                  request->output_range().length) {
        attached = session->AttachGpu(identity, source, result.completion);
        error = "native_ticket_attachment_failed";
        if (attached) {
          session->BeginRetire(identity, source);
          error = "native_fence_deadline_or_failure";
          while (result.completion.status() ==
                     gpu::GpuCompletionStatus::kPending &&
                 std::chrono::steady_clock::now() < deadline) {
            thread->Delay(0, 0, uint64_t(-100000ll));
          }
          session->ObserveCompletion(result.completion);
          if (result.completion.status() ==
              gpu::GpuCompletionStatus::kComplete) {
            readback_requested = true;
            cp->ReadCompletedNativeFrameAsync(
                request, [readback](auto snapshot) {
                  readback->result = std::move(snapshot);
                  readback->returned.store(true, std::memory_order_release);
                });
            const auto readback_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!readback->returned.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < readback_deadline) {
              thread->Delay(0, 0, uint64_t(-100000ll));
            }
            error = "readback_deadline_or_failure";
            if (readback->returned.load(std::memory_order_acquire)) {
              readback_saved = SaveNativeFrameReadback(
                  capture_path, *layout, *request, readback->result);
              error = "readback_snapshot_or_file_failed";
              if (readback_saved) {
                if (!presentation_required || await_presentation()) {
                  retired = session->RetireComplete(identity, source);
                  error = retired ? "none" : "post_readback_retirement_failed";
                } else {
                  error = presentation_error;
                }
              }
            }
          }
        }
      }
    }
  }
  const bool returned = observation->returned.load(std::memory_order_acquire);
  const auto result = returned ? observation->result : Result{};
  const auto& owner = session->owner();
  const auto token_status = result.completion.status();
  const bool readback_returned =
      readback->returned.load(std::memory_order_acquire);
  const auto readback_status =
      readback_returned ? readback->result.status
                        : gpu::NativeFrameReadbackResult::Status::kUnsupported;
  const bool written =
      std::fprintf(
          file.get(),
          "{\"version\":2,\"kernel_epoch\":%llu,\"session\":%llu,\"source\":%"
          "llu,"
          "\"prepared\":%s,\"returned\":%s,\"status\":%u,\"stage\":%u,"
          "\"packet_dword\":%u,\"draw_commands\":%u,\"indexed_draw_commands\":%"
          "u,"
          "\"copy_operations\":%u,\"clear_operations\":%u,\"written_address\":%"
          "u,"
          "\"written_bytes\":%u,\"title_state_restored\":%s,\"backend_epoch\":%"
          "llu,"
          "\"submission\":%llu,\"completion_status\":%u,\"attached\":%s,"
          "\"retired_complete\":%s,\"error\":\"%s\","
          "\"output_layout_validated\":%s,\"readback_requested\":%s,"
          "\"readback_returned\":%s,\"readback_status\":%u,\"readback_saved\":%"
          "s,"
          "\"presentation_required\":%s,\"presentation_requested\":%s,"
          "\"presentation_returned\":%s,\"presentation_status\":%u,"
          "\"presentation_kernel_epoch\":%llu,\"presentation_session\":%llu,"
          "\"presentation_source\":%llu,\"presentation_backend_epoch\":%llu,"
          "\"presentation_consumer_backend_epoch\":%llu,"
          "\"presentation_image_version\":%llu,"
          "\"presentation_native_write_submission\":%llu,"
          "\"presentation_composite_submission\":%llu,"
          "\"presentation_image_ready\":%s,\"presentation_presenter_accepted\":"
          "%s,"
          "\"presentation_completion_status\":%u,"
          "\"presentation_handoff_complete\":%s,"
          "\"pixels_verified\":false,"
          "\"composition_verified\":false,\"input_verified\":false}\n",
          static_cast<unsigned long long>(owner.kernel_epoch),
          static_cast<unsigned long long>(owner.session_id),
          static_cast<unsigned long long>(source ? source->serial() : 0),
          request ? "true" : "false", returned ? "true" : "false",
          unsigned(result.status), unsigned(result.failing_stage),
          result.failing_packet_dword_offset, result.draw_commands,
          result.indexed_draw_commands, result.copy_operations,
          result.clear_operations, result.written_range.address,
          result.written_range.length,
          result.title_state_restored ? "true" : "false",
          static_cast<unsigned long long>(result.completion.backend_epoch()),
          static_cast<unsigned long long>(result.completion.submission()),
          unsigned(token_status), attached ? "true" : "false",
          retired ? "true" : "false", error, layout ? "true" : "false",
          readback_requested ? "true" : "false",
          readback_returned ? "true" : "false", unsigned(readback_status),
          readback_saved ? "true" : "false",
          presentation_required ? "true" : "false",
          presentation_requested ? "true" : "false",
          presentation_returned ? "true" : "false",
          unsigned(presentation.status),
          static_cast<unsigned long long>(
              presentation.identity.owner.kernel_epoch),
          static_cast<unsigned long long>(
              presentation.identity.owner.session_id),
          static_cast<unsigned long long>(presentation.identity.source_serial),
          static_cast<unsigned long long>(presentation.backend_epoch),
          static_cast<unsigned long long>(
              presentation.presentation_backend_epoch),
          static_cast<unsigned long long>(presentation.image_version),
          static_cast<unsigned long long>(presentation.native_write_submission),
          static_cast<unsigned long long>(presentation.composite_submission),
          presentation.image_ready ? "true" : "false",
          presentation.presenter_accepted ? "true" : "false",
          unsigned(presentation.composite_completion.status()),
          presentation_handoff_complete ? "true" : "false") > 0 &&
      !std::fflush(file.get());
  XELOGI(
      "Native frame execution: source={} status={} stage={} packet={} draws={} "
      "copy={} restored={} completion={} attached={} retired={} readback={} "
      "presentation={} error={}; "
      "pixel/composition/input acceptance pending",
      source ? source->serial() : 0, unsigned(result.status),
      unsigned(result.failing_stage), result.failing_packet_dword_offset,
      result.draw_commands, result.copy_operations, result.title_state_restored,
      unsigned(token_status), attached, retired, readback_saved,
      presentation_handoff_complete, error);
  return written && retired;
}
}  // namespace xe::kernel
