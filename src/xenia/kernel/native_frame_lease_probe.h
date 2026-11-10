/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_FRAME_LEASE_PROBE_H_
#define XENIA_KERNEL_NATIVE_FRAME_LEASE_PROBE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "xenia/base/physical_range_leases.h"
#include "xenia/gpu/native_overlay.h"

namespace xe::gpu {
class NativeFrameExecutionRequest;
class NativeSystemTitleInput;
}  // namespace xe::gpu

namespace xe::kernel {
class KernelState;
class NativeFrameOwnerSession;
class NativeSystemFrameDelegation;
class NativeNonmodalOutputProvenance;
class NativeNonmodalControlProvenance;

// Supplied only after the genuine native recording End and target/record
// validation. primary is an owned host copy held for this call, copied under
// the allocator lock while a genuine owned full-record backing pin was held;
// it must never be a borrowed live guest-memory span. Addresses are already
// canonical physical addresses, not aliases.
// record_allocation_bytes covers the full native allocation, including the
// vertex data appended after primary PM4. Output size is a verified storage
// extent; this public diagnostic structure cannot manufacture its provenance.
struct NativeCompletedFrameRecord {
  std::span<const uint8_t> primary;
  uint32_t device = 0;
  uint32_t dc = 0;
  uint32_t record_physical_address = 0;
  uint32_t record_allocation_bytes = 0;
  uint32_t output_physical_address = 0;
  uint32_t output_allocation_bytes = 0;
  bool native_end_completed = false;
  uint32_t primary_physical_address = 0;
  // Genuine post-End XG/VD CPU alias where present; NativeOverlay retains
  // the actual absent value0. Use output_physical_address for GPU output,
  // never synthesize an optional VD field or mask a CPU allocation alias.
  uint32_t output_cpu_address = 0;
  std::array<uint32_t, 6> output_texture_fetch{};
};

// CPU-only lifetime gate. Consumes the current producer Update, pins genuine
// owned backing before physical reads, checks every draw's bounded input and
// explicit constant inventory, and captures while frozen. Normal success
// explicitly aborts the unsubmitted frame and requires a new Update afterward.
// Never submits GPU work, executes PPC, or claims complete shader semantics.
// Failed/exceptional paths quarantine with pins retained until Kernel teardown;
// the caller must stop native calls on false. Unknown exceptions propagate.
// Writes capture_path and its normal capture sidecar, plus an exclusively new
// private capture_path + ".lease.json". All parents must already exist.
// Optional canceled_request receives the checked prepared handle only after
// successful report finalization; it is already invalidated by the CPU abort.
bool RunNativeFrameLeaseProbe(
    KernelState* kernel, std::shared_ptr<NativeFrameOwnerSession> session,
    const NativeCompletedFrameRecord& record,
    const std::filesystem::path& capture_path,
    std::shared_ptr<const gpu::NativeFrameExecutionRequest>* canceled_request =
        nullptr);

// Same complete proof/capture builder, but retains the frozen frame instead
// of aborting it. Checks the genuine post-End header, final-copy registers and
// pinned resolve rectangle before minting its immutable output layout. This
// handoff does not itself submit or authorize GPU work.
// Failure revokes the owner and retains uncertain backing until kernel stop.
std::shared_ptr<const gpu::NativeFrameExecutionRequest>
PrepareNativeFrameExecutionRequest(
    KernelState* kernel, std::shared_ptr<NativeFrameOwnerSession> session,
    const NativeCompletedFrameRecord& record,
    const std::filesystem::path& capture_path);
// Only the real Vd/record-helper adapter mints this after actual End, complete
// record pin + immutable primary snapshot and device/DC provenance. Production
// inputs require an actual writer grant or complete checked immutable capture.
// Public flags in the diagnostic record cannot construct production provenance.
class NativeSystemCompletedFrameRecord {
 public:
  enum class InputProvenance {
    kUnspecified,
    kActualWriterGrant,
    kImmutableSnapshot
  };

 private:
  friend class NativeSystemGraphicsTransport;
  friend std::shared_ptr<const gpu::NativeFrameExecutionRequest>
  PrepareNativeSystemFrameExecutionRequest(
      KernelState*,
      const std::shared_ptr<const NativeSystemCompletedFrameRecord>&);
  NativeSystemCompletedFrameRecord() = default;
  std::shared_ptr<NativeSystemFrameDelegation> delegation;
  std::shared_ptr<const std::vector<uint8_t>> immutable_primary;
  std::shared_ptr<const memory::PhysicalRangePin> full_record_pin;
  std::shared_ptr<const gpu::NativeSystemTitleInput> title_input;
  InputProvenance input_provenance = InputProvenance::kUnspecified;
  // Required only for kActualWriterGrant; snapshot mode MUST leave it null.
  std::shared_ptr<void> all_writer_exclusion;
  // Immediate immutable copy of the actual 148B native Vd Set descriptor.
  // Its raw CPU aliases are preserved, never fabricated from an XG header.
  std::array<uint8_t, 148> vd_descriptor{};
  // Only the actual same-Set observer/transport may supply this declaration.
  // A native-overlay declaration never fills absent optional VD fields.
  std::shared_ptr<const NativeNonmodalOutputProvenance> output_provenance;
  std::shared_ptr<const NativeNonmodalControlProvenance> control_provenance;
  // Prior *completed requested* overlay state, not hardware-active state.
  std::optional<gpu::NativeOverlayState> overlay_input_state;
  NativeCompletedFrameRecord record;
};

// Production path: same complete bounds/read/layout/pin proof, no capture,
// diagnostic free attempt, report files or per-frame Kernel retirement holder.
// Requires an actual active delegated Task invocation. Input authority is an
// actual all-writer grant OR a complete sealed byte view captured/validated by
// the builder; a cohort producer token never means all guest writers stopped.
// Source/pins stay in the persistent Session.
std::shared_ptr<const gpu::NativeFrameExecutionRequest>
PrepareNativeSystemFrameExecutionRequest(
    KernelState* kernel,
    const std::shared_ptr<const NativeSystemCompletedFrameRecord>& completed);
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_FRAME_LEASE_PROBE_H_
