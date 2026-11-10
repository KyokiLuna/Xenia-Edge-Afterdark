/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_TESTING_NATIVE_FRAME_EXECUTION_TEST_ACCESS_H_
#define XENIA_KERNEL_TESTING_NATIVE_FRAME_EXECUTION_TEST_ACCESS_H_
#include "xenia/kernel/native_frame_input_capture.h"
#include "xenia/kernel/native_frame_owner.h"
namespace xe::kernel::test {
// Exercises the private host admission state machine with actual Memory pins.
// Production has only the checked NativeFrameLeaseBuilder caller. This peer
// never creates or claims a native guest device/DC, CP, GPU or submission.
class NativeFrameExecutionTestAccess {
 public:
  // TEST ONLY: storage-proof model data copied from an actual Memory/Session
  // VD fixture. This bypasses no production path, creates no native output
  // provenance, and never proves a Task, GPU submission or visibility.
  struct ModelPm4ProofData {
    gpu::NativeFrameSubmissionIdentity identity;
    std::array<uint8_t, 148> descriptor;
    gpu::NativeFramePm4ResolveProof::Mapping primary, output;
    uint32_t copy_packet;
    memory::PhysicalRange rectangle;
    uint32_t endian;
    std::array<uint32_t, 6> words;
    gpu::NativeFramePm4ResolveProof::OutputOrigin origin;
    std::array<uint8_t, 0x58> display;
    std::optional<gpu::NativeOverlayState> input_state;
  };
  static ModelPm4ProofData ModelProofData(
      const gpu::NativeFramePm4ResolveProof& base) {
    return {base.identity(), base.descriptor(), base.primary_mapping(),
            base.output_mapping(), base.copy_packet_dword_offset(),
            base.rectangle(), base.rectangle_endian(), base.rectangle_words(),
            base.output_origin(), base.display_information(),
            base.overlay_input_state()};
  }
  static std::shared_ptr<const gpu::NativeFramePm4ResolveProof> ModelProof(
      const ModelPm4ProofData& data) {
    return std::shared_ptr<const gpu::NativeFramePm4ResolveProof>(
        new gpu::NativeFramePm4ResolveProof(
            data.identity, data.descriptor, data.primary, data.output,
            data.copy_packet, data.rectangle, data.endian, data.words,
            data.origin, data.display, data.input_state));
  }
  static std::unique_ptr<NativeFrameInputCapture> Capture(
      Memory* memory, const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      memory::PhysicalRange primary, memory::PhysicalRange output) {
    return session->BeginInputCapture(memory, thread, source, plan, primary,
                                      output);
  }

  static std::shared_ptr<const gpu::NativeFrameExecutionRequest> Mint(
      const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source,
      std::shared_ptr<const gpu::NativeFrameCommandPlan> plan,
      memory::PhysicalRange output, std::vector<memory::PhysicalRange> inputs,
      std::vector<gpu::NativeFrameExecutionRequest::Shader> shaders = {},
      std::optional<gpu::NativeFrameOutputLayout> layout = std::nullopt,
      std::shared_ptr<const gpu::NativeFrameInputSnapshot> snapshot = {}) {
    return session->MintExecutionRequest(
        thread, source, std::move(plan), output, std::move(inputs),
        std::move(shaders), std::move(layout), std::move(snapshot));
  }
  static std::optional<gpu::NativeFrameOutputLayout> Layout(
      Memory* guest_memory,
      const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      std::span<const memory::PhysicalRange> inputs,
      const NativeCompletedFrameRecord& record) {
    return session->SnapshotOutputLayout(guest_memory, thread, source, plan,
                                         inputs, record);
  }
  static std::optional<gpu::NativeFrameOutputLayout> Pm4Layout(
      Memory* memory, const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      std::span<const memory::PhysicalRange> inputs,
      const NativeCompletedFrameRecord& record,
      const std::array<uint8_t, 148>& descriptor,
      const std::shared_ptr<const gpu::NativeFrameInputSnapshot>& snapshot =
          {}) {
    return session->SnapshotPm4OutputLayout(
        memory, thread, source, plan, inputs, record, descriptor, snapshot);
  }
};
}  // namespace xe::kernel::test
#endif
