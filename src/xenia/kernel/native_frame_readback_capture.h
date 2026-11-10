/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_FRAME_READBACK_CAPTURE_H_
#define XENIA_KERNEL_NATIVE_FRAME_READBACK_CAPTURE_H_
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>
#include "xenia/gpu/native_frame_execution.h"
namespace xe::gpu {
struct NativeFramePresentationStatus;
}
namespace xe {
class Memory;
}
namespace xe::kernel {
class KernelState;
class NativeFrameSource;
struct NativeCompletedFrameRecord;
// Owned producer metadata and copied resolve vertices, checked while the
// prepared Source is frozen and before its execution request is consumed.
struct NativeFrameReadbackLayout {
  std::array<uint32_t, 6> texture_fetch{}, rectangle_words{};
  std::array<uint32_t, 6> original_texture_fetch{};
  uint32_t width = 0, height = 0, pitch = 0, physical_address = 0;
  uint32_t required_bytes = 0, copy_dest_info = 0, copy_dest_pitch = 0;
};
// Caller must hold the exact frozen Source operation and real backing pins.
// This pre-mint form uses the checked plan/input inventory, not a Request whose
// construction would itself require the resulting output descriptor.
std::optional<NativeFrameReadbackLayout> SnapshotNativeFrameReadbackLayout(
    Memory* guest_memory, const gpu::NativeFrameCommandPlan& plan,
    std::span<const memory::PhysicalRange> inputs,
    const NativeCompletedFrameRecord& record);
std::optional<NativeFrameReadbackLayout> SnapshotNativeFrameReadbackLayout(
    KernelState* kernel, const gpu::NativeFrameExecutionRequest& request,
    const NativeCompletedFrameRecord& record);
// Host-owned raw snapshot only. The backend must already have completed its
// operation-scoped auxiliary fence/copy. Does not claim image correctness.
bool SaveNativeFrameReadback(const std::filesystem::path& capture_path,
                             const NativeFrameReadbackLayout& layout,
                             const gpu::NativeFrameExecutionRequest& request,
                             const gpu::NativeFrameReadbackResult& readback);
// An observed controller edge, not proof of a native animation phase or of
// commands being generated in response to that edge. Loader and runtime module
// generations are separate identities and must not be compared numerically.
struct NativeFrameOutputBDownTrigger {
  uint64_t input_generation = 0, domain_id = 0, root_load_generation = 0;
  uint64_t foreground_generation = 0, visibility_sequence = 0;
  uint64_t press_sequence = 0, binding_epoch = 0;
  uint32_t root_hmodule = 0, slot = 0, native_user = 0, device_handle = 0;
  uint32_t binding_identity = 0, packet = 0, buttons = 0;
};
// An explicit research selection, not an observed input edge or native cause.
// The caller already owns the exact frozen Source; this carries no authority.
struct NativeFrameOutputExactSourceDiagnosticTrigger {
  uint64_t source_serial = 0;
  // Copied from this frozen Source's genuine completed Title input. Runtime
  // owner/module generations remain separate from this Title-input identity.
  gpu::NativeSystemTitleInput::Identity title_input;
};
// Complete copies of immutable execution inputs. Offsets address bytes only;
// these records retain no Source, Request, pin, completion or GPU authority.
struct NativeFrameOutputInputs {
  static constexpr uint32_t kMaxShaderLoads = 512;
  static constexpr uint32_t kMaxShaderBytes = 16 * 1024;
  static constexpr uint32_t kMaxBytes =
      gpu::NativeFrameInputSnapshot::kMaxBytes +
      kMaxShaderLoads * kMaxShaderBytes;
  struct Block {
    memory::PhysicalRange range{};
    uint32_t offset = 0;
  };
  struct Shader {
    gpu::NativeDrawReadInventory::ShaderLoad load{};
    uint32_t offset = 0, length = 0;
  };
  struct Title {
    gpu::NativeSystemTitleInput::Identity identity;
    gpu::NativeSystemTitleInput::Layout layout;
    uint32_t offset = 0, length = 0;
  };
  gpu::NativeFrameSubmissionIdentity identity;
  uint64_t backend_epoch = 0;
  memory::PhysicalRange primary_range{}, output_range{};
  uint32_t snapshot_bytes = 0;
  std::vector<Block> blocks;
  std::vector<Shader> shaders;
  std::optional<Title> title;
  // Ordered complete native blocks, every shader copy, then optional Title.
  // Shader copies intentionally duplicate their native/primary backing bytes.
  std::vector<uint8_t> bytes;
};
// Pure CPU diagnostic data only: no completion token, guest lease or GPU
// object. This structure never grants readback/submit authority.
struct NativeFrameOutputCapture {
  gpu::NativeFrameSubmissionIdentity identity;
  uint64_t backend_epoch = 0, native_submission = 0;
  gpu::NativeFrameOutputLayout layout;
  memory::PhysicalRange output_range{}, written_range{};
  memory::PhysicalRange copied_range{};
  std::vector<uint8_t> bytes;
  // Optional full already-frozen primary, never read back from guest RAM.
  // Identity and trigger describe the same selected Source as the output.
  gpu::NativeFrameSubmissionIdentity primary_identity;
  std::vector<uint8_t> primary_bytes;
  std::optional<NativeFrameOutputBDownTrigger> b_down_trigger;
  std::optional<NativeFrameOutputExactSourceDiagnosticTrigger>
      exact_source_diagnostic;
  std::optional<NativeFrameOutputInputs> inputs;
  gpu::NativeFrameSubmissionIdentity readback_identity;
  uint64_t readback_backend_epoch = 0, readback_native_submission = 0;
  uint32_t readback_status = 0;
  bool readback_complete = false;
  uint64_t image_version = 0, presentation_backend_epoch = 0;
  uint64_t paint_submission = 0;
  uint32_t presentation_status = 0, paint_completion_status = 0;
  bool image_associated = false, image_ready = false,
       presenter_accepted = false;
};
// Copy exact already-validated metadata before the Source retires. No guest
// read, IsAdmitted recheck, GPU work, file write or new authority.
std::optional<NativeFrameOutputCapture> SnapshotNativeFrameOutputCapture(
    const gpu::NativeFrameExecutionRequest& request,
    const gpu::NativeFrameExecutionResult& execution);
// Caller holds the exact frozen Source scope. Copy only its existing primary
// span, full and DWORD-aligned, at most 128 KiB. Does not inspect guest memory,
// parse new packets, mint authority, or retain a Source/Request holder.
bool AttachNativeFrameOutputPrimary(
    NativeFrameOutputCapture& capture, const NativeFrameSource& source,
    const NativeFrameOutputBDownTrigger& trigger);
// Separate explicit positive-source research attachment. It never invents an
// input event. Copies the same bounded frozen primary, leaving capture
// unchanged on failure. Production mode exposes only the exact Source256
// diagnostic.
bool AttachNativeFrameOutputExactSourceDiagnostic(
    NativeFrameOutputCapture &, const NativeFrameSource &,
    const NativeFrameOutputExactSourceDiagnosticTrigger &);
// Copy the complete already-sealed snapshot, actual shader copies and retained
// completed Title input. No live RAM read, IsAdmitted check or retained holder.
// Failure leaves capture unchanged; caller's diagnostic catch owns bad_alloc.
bool AttachNativeFrameOutputInputs(
    NativeFrameOutputCapture &capture,
    const gpu::NativeFrameExecutionRequest &request);
// Consume the existing completed readback; completion holders are dropped
// before return. The caller must not publish callback completion before this
// returns.
bool CompleteNativeFrameOutputCapture(NativeFrameOutputCapture& capture,
                                      gpu::NativeFrameReadbackResult readback);
// Consume actual image metadata, without requiring visibility/paint completion.
bool AssociateNativeFrameOutputCapture(
    NativeFrameOutputCapture& capture,
    gpu::NativeFramePresentationStatus presentation);
// Private raw file, optional full primary sidecar and version2 JSON; no
// Request/Kernel/Session references. Metadata is written after all byte files.
// The caller owns the one-shot policy. File failures never alter Source state.
bool SaveNativeFrameOutputCapture(const std::filesystem::path& capture_path,
                                  const NativeFrameOutputCapture& capture);
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_FRAME_READBACK_CAPTURE_H_
