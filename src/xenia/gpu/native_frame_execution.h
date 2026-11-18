/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_FRAME_EXECUTION_H_
#define XENIA_GPU_NATIVE_FRAME_EXECUTION_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "xenia/base/physical_range_leases.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/gpu/native_frame_command_plan.h"
#include "xenia/gpu/native_frame_input_snapshot.h"
#include "xenia/gpu/native_overlay.h"
#include "xenia/gpu/native_shader_read_analysis.h"

namespace xe::kernel {
class NativeFrameOwnerSession;
}

namespace xe::kernel::test {
class NativeFrameExecutionTestAccess;
}

namespace xe::gpu {
class NativeFrameExecutionRequest;

// Advisory, prebound scalar cells for ONE synchronous original preentry/drain
// attempt. No pointers, owners, locks, IO, native calls or readiness live here.
// Only the actual Bootstrap caller writes; host reporters read atomic words.
// Reset is under that owner's metadata lock before the synchronous call chain;
// no pointer may escape to deferred GPU recording, callbacks or an awaiter.
class NativeFramePaintDrainObservation final {
 public:
  enum class Stage : uint32_t {
    kOldPaused,kOldPaint,kPostPaint,kWakeSourceListener,kArm,kContinuation,
    kTransportAdmission,kTransportState,kTransportHandoff,kPresenter,kPaintRetirement,
    kPaintFence,kPaintFenceAwait,
    kCount
  };
  enum class Scalar : uint32_t {kCompletionError,kCompletedSubmission,kUpcomingSubmission,kCount};
  static constexpr uint32_t kCount=uint32_t(Stage::kCount),kScalarCount=uint32_t(Scalar::kCount),kMaximumAttempt=(1u<<29)-1;
  void Reset(uint32_t attempt) noexcept {
    attempt_=attempt<=kMaximumAttempt?attempt:0;
    scalar_available_.store(0,std::memory_order_release);
    scalar_attempt_.store(attempt_,std::memory_order_release);
    for(uint32_t i=0;i<kCount;++i){evaluated_[i]=truth_[i]=0;begun_[i]=false;cells_[i].store(uint64_t(attempt_)<<35,std::memory_order_release);}
  }
  void Begin(Stage stage) noexcept {
    const auto i=uint32_t(stage);if(i>=kCount || !attempt_)return;
    begun_[i]=true;evaluated_[i]=truth_[i]=0;Publish(i,1,false);
  }
  // Receives the original expression ONCE and forwards its exact boolean.
  bool Observe(Stage stage,uint32_t bit,bool value) noexcept {
    const auto i=uint32_t(stage);
    if(i<kCount && bit<16 && attempt_ && begun_[i]){
      evaluated_[i]|=uint16_t(1u<<bit);if(value)truth_[i]|=uint16_t(1u<<bit);
      Publish(i,1,false);
    }
    return value;
  }
  void Finish(Stage stage,bool value) noexcept {
    const auto i=uint32_t(stage);if(i<kCount && attempt_ && begun_[i])Publish(i,2,value);
  }
  // An actual void call returned; this supplies NO boolean/GPU-done result.
  void FinishVoid(Stage stage) noexcept {
    const auto i=uint32_t(stage);if(i<kCount && attempt_ && begun_[i])Publish(i,3,false);
  }
  template<typename T> T Value(Scalar scalar,T value) noexcept {
    const auto i=uint32_t(scalar);if(i<kScalarCount && attempt_){
      scalars_[i].store(uint64_t(value),std::memory_order_relaxed);
      scalar_available_.fetch_or(1u<<i,std::memory_order_release);
    }
    return value;
  }
  std::optional<uint64_t> ScalarSnapshot(Scalar scalar,uint32_t expected_attempt) const noexcept {
    const auto i=uint32_t(scalar);if(i>=kScalarCount || !expected_attempt ||
        scalar_attempt_.load(std::memory_order_acquire)!=expected_attempt ||
        !(scalar_available_.load(std::memory_order_acquire)&(1u<<i)))return {};
    const auto value=scalars_[i].load(std::memory_order_relaxed);
    return scalar_attempt_.load(std::memory_order_acquire)==expected_attempt?std::optional<uint64_t>(value):std::nullopt;
  }
  bool Begun(Stage stage) const noexcept {
    const auto i=uint32_t(stage);return i<kCount && begun_[i]; // writer only
  }
  uint64_t Snapshot(Stage stage,uint32_t expected_attempt) const noexcept {
    const auto i=uint32_t(stage);if(i>=kCount || !expected_attempt)return 0;
    const auto word=cells_[i].load(std::memory_order_acquire);
    return uint32_t(word>>35)==expected_attempt?word:0;
  }
  static constexpr uint32_t Phase(uint64_t word){return uint32_t(word&3);}
  static constexpr bool Result(uint64_t word){return bool(word&4);}
  static constexpr uint32_t Evaluated(uint64_t word){return uint32_t((word>>3)&0xFFFF);}
  static constexpr uint32_t Truth(uint64_t word){return uint32_t((word>>19)&0xFFFF);}
 private:
  void Publish(uint32_t i,uint32_t phase,bool result) noexcept {
    const auto word=(uint64_t(attempt_)<<35)|(uint64_t(truth_[i])<<19)|
        (uint64_t(evaluated_[i])<<3)|(uint64_t(result)<<2)|phase;
    cells_[i].store(word,std::memory_order_release);
  }
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  uint32_t attempt_=0;
  std::array<uint16_t,kCount> evaluated_{},truth_{}; // actual single writer only
  std::array<bool,kCount> begun_{};
  std::array<std::atomic<uint64_t>,kCount> cells_{};
  std::array<std::atomic<uint64_t>,kScalarCount> scalars_{};
  std::atomic<uint32_t> scalar_available_{0},scalar_attempt_{0};
};

// Immutable per-Query presentation origin. Implementations own metadata only,
// never guest/storage/code/GPU resources. Revocation is irreversible. IsLive
// runs outside backend locks; IsRevoked is atomic-only and safe under them.
// Null guards preserve ordinary frames. These methods grant no execution,
// storage, submission, native-clear or completion authority.
class NativeFramePresentationGuard {
 public:
  virtual ~NativeFramePresentationGuard() = default;
  virtual bool IsLive() const noexcept = 0;
  virtual bool IsRevoked() const noexcept = 0;
};

// Immutable storage provenance, minted only while the exact Session Source
// operation and actual owner backing pins protect the rectangle/mapping read.
// No Kernel pointer, guest header, GPU permission or visibility lives here.
class NativeFramePm4ResolveProof {
 public:
  enum class OutputOrigin { kVdTexture, kNativeOverlay };
  OutputOrigin output_origin() const { return output_origin_; }
  const std::array<uint8_t, 0x58>& display_information() const {
    return display_information_;
  }
  const std::optional<NativeOverlayState>& overlay_input_state() const {
    return overlay_input_state_;
  }
  struct Mapping {
    enum class Kind { kUnspecified, kCanonicalPhysical, kPhysicalHeapAlias };
    Kind kind = Kind::kUnspecified;
    uint32_t observed = 0, physical = 0;
  };
  const NativeFrameSubmissionIdentity& identity() const { return identity_; }
  const std::array<uint8_t, 148>& descriptor() const { return descriptor_; }
  const Mapping& primary_mapping() const { return primary_mapping_; }
  const Mapping& output_mapping() const { return output_mapping_; }
  uint32_t copy_packet_dword_offset() const { return copy_packet_; }
  const memory::PhysicalRange& rectangle() const { return rectangle_; }
  uint32_t rectangle_endian() const { return rectangle_endian_; }
  const std::array<uint32_t, 6>& rectangle_words() const {
    return rectangle_words_;
  }

 private:
  friend class xe::kernel::NativeFrameOwnerSession;
  friend class xe::kernel::test::NativeFrameExecutionTestAccess;
  NativeFramePm4ResolveProof(NativeFrameSubmissionIdentity identity,
                             std::array<uint8_t, 148> descriptor,
                             Mapping primary, Mapping output,
                             uint32_t copy_packet,
                             memory::PhysicalRange rectangle,
                             uint32_t rectangle_endian,
                             std::array<uint32_t, 6> rectangle_words,
                             OutputOrigin output_origin = OutputOrigin::kVdTexture,
                             std::array<uint8_t, 0x58> display_information = {},
                             std::optional<NativeOverlayState> overlay_input_state = {})
      : identity_(identity),
        descriptor_(std::move(descriptor)),
        primary_mapping_(primary),
        output_mapping_(output),
        copy_packet_(copy_packet),
        rectangle_(rectangle),
        rectangle_endian_(rectangle_endian),
        rectangle_words_(rectangle_words),
        output_origin_(output_origin),
        display_information_(display_information),
        overlay_input_state_(std::move(overlay_input_state)) {}
  const NativeFrameSubmissionIdentity identity_;
  const std::array<uint8_t, 148> descriptor_;
  const Mapping primary_mapping_, output_mapping_;
  const uint32_t copy_packet_;
  const memory::PhysicalRange rectangle_;
  const uint32_t rectangle_endian_;
  const std::array<uint32_t, 6> rectangle_words_;
  const OutputOrigin output_origin_;
  const std::array<uint8_t, 0x58> display_information_;
  const std::optional<NativeOverlayState> overlay_input_state_;
};

// This describes bytes only; it grants no owner or execution authority.
struct NativeFrameOutputLayout {
  enum class Origin { kUnspecified, kOriginalXgHeader, kPm4Resolve,
                      kNativeOverlayResolve };
  Origin origin = Origin::kUnspecified;
  uint32_t width = 0, height = 0, pitch = 0, required_bytes = 0;
  // Original path: base-normalized genuine fetch. PM4 path: canonical HOST
  // level0 storage adapter only, never relabeled as an observed guest header.
  std::array<uint32_t, 6> texture_fetch{};
  std::optional<std::array<uint32_t, 6>> original_texture_fetch;
  std::shared_ptr<const NativeFramePm4ResolveProof> pm4_resolve;
  // Requested state from the exact ordered native command program. This is
  // not hardware scanout visibility. The completed image and its placement
  // are selected together at actual host paint. Native RGBA remains intact.
  std::optional<NativeOverlayState> overlay_state;
  std::optional<NativeOverlayPresentation> overlay_presentation;
};

inline bool NativeFrameOutputOriginUsesPm4Resolve(
    NativeFrameOutputLayout::Origin origin) noexcept {
  return origin == NativeFrameOutputLayout::Origin::kPm4Resolve ||
         origin == NativeFrameOutputLayout::Origin::kNativeOverlayResolve;
}

// Pure bounded storage validation shared by request and backend consumers.
// It checks the canonical 1x tiled RGBA8 layout and its exact byte footprint,
// never Source/owner authority, guest memory, GPU readiness or presentation.
// Original-XG origin retains its existing 640x480/pitch640 profile.
bool ValidateNativeFrameOutputStorage(const NativeFrameOutputLayout& layout,
                                      memory::PhysicalRange output);

// Pure rederivation from immutable Plan and sealed Session snapshot. Physical
// heap aliases were normalized by actual Memory before the proof was minted;
// this layer must never bitmask them into a guessed physical destination.
std::optional<NativeFrameOutputLayout> DeriveNativePm4OutputLayout(
    const NativeFrameCommandPlan& plan, memory::PhysicalRange output,
    std::span<const memory::PhysicalRange> input_ranges,
    const std::shared_ptr<const NativeFramePm4ResolveProof>& proof);

// Host-only owner operation held through the backend copy, wait and snapshot.
// An abandoned admission must quarantine before releasing its Session
// operation.
class NativeFrameReadbackAdmission {
 public:
  enum class Outcome { kCanceledBeforeGpu, kGpuComplete, kGpuUncertain };
  virtual ~NativeFrameReadbackAdmission() = default;
  virtual bool Finish(Outcome outcome) noexcept = 0;
};

// Implemented by the real kernel Session/Source owner. The GPU layer knows
// only this typed interface, never a raw Kernel/XThread or untyped authority.
// Calls must not execute PPC, wait for a GPU, or run under backend locks.
class NativeFrameExecutionAuthority {
 public:
  virtual ~NativeFrameExecutionAuthority() = default;
  virtual bool IsAdmitted(const NativeFrameExecutionRequest& request) const = 0;
  virtual bool MarkSubmissionAttempted(
      const NativeFrameExecutionRequest& request) = 0;
  // Visibility only, including after the completed Source has retired. This
  // must validate the live owner/title generation and grants no GPU authority.
  virtual bool IsPresentationOwnerLive(
      const NativeFrameExecutionRequest&) const {
    return false;
  }
  // No GPU wait. Default deny until the exact frozen Source and its attached
  // Complete native token can reserve a Session operation through readback.
  virtual std::unique_ptr<NativeFrameReadbackAdmission>
  AcquireCompletedReadback(const NativeFrameExecutionRequest&,
                           const GpuCompletion&) {
    return {};
  }
};

// A prepared CPU handoff, NOT permission to execute or proof of a submission.
// Only the kernel's checked frame builder can ask its Session to mint one.
// Public const data retains the exact plan, shader copies and real backing
// pins. Copies of the shared handle share one consumption token in the owner.
class NativeFrameExecutionRequest {
 public:
  struct Shader {
    NativeDrawReadInventory::ShaderLoad load;
    std::vector<uint8_t> code;
    NativeShaderReadAnalysis::Result metadata;
  };
  NativeFrameExecutionRequest(const NativeFrameExecutionRequest&) = delete;
  NativeFrameExecutionRequest& operator=(const NativeFrameExecutionRequest&) =
      delete;
  const std::shared_ptr<const NativeFrameCommandPlan>& plan() const {
    return plan_;
  }
  const NativeFrameSubmissionIdentity& identity() const { return identity_; }
  uint64_t backend_epoch() const { return backend_epoch_; }
  const memory::PhysicalRange& output_range() const { return output_range_; }
  const std::optional<NativeFrameOutputLayout>& output_layout() const {
    return output_layout_;
  }
  const std::vector<memory::PhysicalRange>& input_ranges() const {
    return input_ranges_;
  }
  const std::vector<Shader>& shaders() const { return shaders_; }
  const std::vector<std::shared_ptr<const memory::PhysicalRangePin>>& pins()
      const {
    return pins_;
  }
  NativeFrameInputProvenance input_provenance() const {
    return input_snapshot_ ? NativeFrameInputProvenance::kImmutableSnapshot
                           : NativeFrameInputProvenance::kLiveOwnerBacking;
  }
  const std::shared_ptr<const NativeFrameInputSnapshot>& input_snapshot()
      const {
    return input_snapshot_;
  }
  bool IsAdmitted() const;
  // One use only. Must be the executor's final owner check before first GPU
  // work; sets sticky uncertainty even when no completion ticket is returned.
  // Preparation alone is insufficient; the retained graph must also explicitly
  // permit execution at this final check.
  bool MarkSubmissionAttempted() const;
  const std::shared_ptr<const NativeFramePresentationGuard>& presentation_guard()
      const {
    return presentation_guard_;
  }
  // Physical completed-state lineage only. This is the exact owner/backend
  // currentness predicate before optional pixel visibility is considered.
  // It grants no GPU submission, source completion or presentation permission.
  bool IsPresentationOwnerCurrent() const noexcept;
  bool IsPresentationOwnerLive() const noexcept;
  // IsAdmitted is consumed at native submission and is not readback authority.
  std::unique_ptr<NativeFrameReadbackAdmission> AcquireCompletedReadback(
      const GpuCompletion& native_completion) const;

 private:
  friend class xe::kernel::NativeFrameOwnerSession;
  NativeFrameExecutionRequest(
      std::shared_ptr<const NativeFrameCommandPlan> plan,
      NativeFrameSubmissionIdentity identity, uint64_t backend_epoch,
      memory::PhysicalRange output_range,
      std::vector<memory::PhysicalRange> input_ranges,
      std::vector<Shader> shaders,
      std::vector<std::shared_ptr<const memory::PhysicalRangePin>> pins,
      std::shared_ptr<NativeFrameExecutionAuthority> authority,
      std::optional<NativeFrameOutputLayout> output_layout,
      std::shared_ptr<const NativeFrameInputSnapshot> input_snapshot,
      std::shared_ptr<const NativeFramePresentationGuard> presentation_guard);
  bool ResourcesValid() const;
  const std::shared_ptr<const NativeFrameCommandPlan> plan_;
  const NativeFrameSubmissionIdentity identity_;
  const uint64_t backend_epoch_;
  const memory::PhysicalRange output_range_;
  const std::optional<NativeFrameOutputLayout> output_layout_;
  const std::vector<memory::PhysicalRange> input_ranges_;
  const std::vector<Shader> shaders_;
  const std::vector<std::shared_ptr<const memory::PhysicalRangePin>> pins_;
  const std::shared_ptr<NativeFrameExecutionAuthority> authority_;
  const std::shared_ptr<const NativeFrameInputSnapshot> input_snapshot_;
  const std::shared_ptr<const NativeFramePresentationGuard> presentation_guard_;
};

// Backend-neutral facts only. A failure can still carry a submitted completion
// that must retire; status/CPU restoration never establishes a stopped GPU.
struct NativeFrameExecutionResult {
  enum class Status {
    kUnsupported = 0,
    kRejected = 1,
    kFailed = 2,
    kSubmitted = 3,
    // Bounded backend resource capacity is currently unavailable. Returned
    // BEFORE MarkSubmissionAttempted: the exact Source/Request is unconsumed.
    // The caller retains it frozen and may queue it again after real resource
    // completion; this result does not run CPU/PPC retries or permit reuse of
    // guest backing. Semantic invalidity and failures are never kBusy.
    kBusy = 4,
  };
  enum class Stage {
    kNone,
    kAdmission,
    kPacket,
    kState,
    kShader,
    kDraw,
    kCopy,
    kSubmission,
    kRetirement
  };
  Status status = Status::kUnsupported;
  Stage failing_stage = Stage::kNone;
  uint32_t failing_packet_dword_offset = UINT32_MAX;
  uint32_t draw_commands = 0;
  uint32_t indexed_draw_commands = 0;
  uint32_t copy_operations = 0;
  uint32_t clear_operations = 0;
  memory::PhysicalRange written_range{};
  bool title_state_restored = false;
  GpuCompletion completion;
};

// Pure operation accounting, not execution or completion authority. A control
// Source has no output and cannot borrow the rendered-frame draw-minus-copy
// arithmetic. The real backend ticket/owner checks remain separate.
bool NativeFrameOperationsMatchPlan(const NativeFrameCommandPlan&,
                                    const NativeFrameExecutionResult&);

// Derive physical overlay lineage from a genuinely completed exact Request.
// Hidden sources still update physical requested state. Pixel guards are not
// consulted and never upgraded. The owner must snapshot prior outside its
// lock, then compare the same current Request again before publishing this
// result; an old completed Request cannot retarget a newer stream by itself.
std::optional<NativeOverlayState> DeriveCompletedNativeOverlayState(
    const NativeFrameExecutionRequest&, const NativeFrameExecutionResult&,
    std::optional<NativeOverlayState> prior);

// Complete certifies the readback helper's own real fence and owned byte copy.
// native_completion identifies the original frame, not the later readback.
struct NativeFrameReadbackResult {
  enum class Status { kUnsupported, kRejected, kFailed, kComplete };
  Status status = Status::kUnsupported;
  GpuCompletion native_completion;
  memory::PhysicalRange copied_range{};
  std::vector<uint8_t> bytes;
};

}  // namespace xe::gpu
#endif  // XENIA_GPU_NATIVE_FRAME_EXECUTION_H_
