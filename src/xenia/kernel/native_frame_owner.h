/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_FRAME_OWNER_H_
#define XENIA_KERNEL_NATIVE_FRAME_OWNER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "xenia/base/physical_range_leases.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/native_overlay.h"
#include "xenia/kernel/guest_runtime.h"

namespace xe {
class Memory;
}
namespace xe::kernel {
class KernelState;
class XThread;
class NativeFrameLeaseBuilder;
class NativeFrameInputCapture;
class NativeNonmodalOutputProvenance;
class NativeNonmodalControlProvenance;
struct NativeCompletedFrameRecord;
namespace test {
class NativeFrameExecutionTestAccess;
}

class NativeSystemAllocationAuthority;
class NativeSystemFrameDelegation;
class NativeFrameOwnerSession;
class NativeFrameSource {
 public:
  const memory::PhysicalAllocationOwner& owner() const { return owner_; }
  uint64_t serial() const { return serial_; }
  std::span<const uint8_t> primary() const { return primary_; }
  const std::shared_ptr<const gpu::NativeSystemTitleInput>& title_input()
      const {
    return title_input_;
  }

 private:
  friend class NativeFrameOwnerSession;
  NativeFrameSource(
      memory::PhysicalAllocationOwner owner, uint64_t serial,
      std::vector<uint8_t> primary,
      std::shared_ptr<const gpu::NativeSystemTitleInput> title_input)
      : owner_(owner),
        serial_(serial),
        primary_(std::move(primary)),
        title_input_(std::move(title_input)) {}
  const memory::PhysicalAllocationOwner owner_;
  const uint64_t serial_;
  const std::vector<uint8_t> primary_;
  const std::shared_ptr<const gpu::NativeSystemTitleInput> title_input_;
};

// Production implementation belongs to NativeGraphicsDeviceBinding and owns
// that actual binding, the same module execution lease and kernel epoch.
// Host-only model tests may supply a mock authority; never a fake guest
// object. All methods run OUTSIDE the session mutex, never execute PPC, and
// must not release their own final graph while a method is executing.
class NativeFrameGraphAuthority {
 public:
  virtual ~NativeFrameGraphAuthority() = default;
  virtual bool Matches(const memory::PhysicalAllocationOwner& owner) const = 0;
  virtual bool AbortUnsubmitted(const NativeFrameSource& source) = 0;
  // New production path only: actual shared binding validates its device/DC.
  // Diagnostic callers retain the existing independent record checks.
  virtual bool MatchesCompletedRecord(const NativeFrameSource&, uint32_t device,
                                      uint32_t dc) const {
    return false;
  }
  // Actual retained binding supplies its verified backend identity. Zero is
  // default-deny for prepared execution requests, not a guessed title epoch.
  virtual uint64_t backend_epoch() const { return 0; }
  virtual bool MatchesTitleInput(const gpu::NativeSystemTitleInput&) const {
    return false;
  }
  // Preparation is separate from actual execution. The final one-use consume
  // requires explicit policy from the real binding, default-deny elsewhere.
  virtual bool CanExecuteNativeFrame(const NativeFrameSource&) const {
    return false;
  }
  // Separate visibility lifetime for an independent completed image. The real
  // binding must also verify the exact foreground title generation is running.
  // Defaults to deny, and never invokes guest code or submits GPU work.
  virtual bool IsPresentationOwnerLive(
      const memory::PhysicalAllocationOwner&) const {
    return false;
  }
  // Default deny until an actual CP-owned native-frame submission ticket
  // proves this exact source/tag. A title completion is not such a ticket.
  virtual bool ValidateNativeSubmission(const NativeFrameSource&,
                                        const gpu::GpuCompletion&) const {
    return false;
  }
  virtual bool RetireComplete(const NativeFrameSource&,
                              const gpu::GpuCompletion&) {
    return false;
  }
  virtual bool RetireAfterKernelStop(const memory::PhysicalAllocationOwner&) {
    return false;
  }
};

// Persistent host control block, also retained by its actual XThread. A parked
// fiber may never unwind an Update guard: explicit revoke closes admission
// independently, and uncertain ownership stays pinned until checked retirement.
class NativeFrameOwnerSession
    : public std::enable_shared_from_this<NativeFrameOwnerSession> {
 public:
  using Owner = memory::PhysicalAllocationOwner;
  struct ThreadIdentity {
    const KernelState* kernel = nullptr;
    const XThread* thread = nullptr;
    uint32_t thread_id = 0;
    uint32_t process_type = 0;
    bool operator==(const ThreadIdentity&) const = default;
  };
  enum class Phase {
    kProvisional,
    kBound,
    kFrozen,
    kSubmitted,
    kRetiring,
    kQuarantined,
    kRetired
  };
  using Source = NativeFrameSource;
  using GraphAuthority = NativeFrameGraphAuthority;
  class Update {
   public:
    Update() = default;
    Update(Update&& other) noexcept;
    Update& operator=(Update&& other) noexcept;
    ~Update();
    Update(const Update&) = delete;
    Update& operator=(const Update&) = delete;
    explicit operator bool() const;
    void Reset();

   private:
    friend class NativeFrameOwnerSession;
    Update(std::weak_ptr<NativeFrameOwnerSession> session, uint64_t serial)
        : session_(std::move(session)), serial_(serial) {}
    std::weak_ptr<NativeFrameOwnerSession> session_;
    uint64_t serial_ = 0;
  };
  struct Snapshot {
    Owner owner;
    ThreadIdentity thread;
    Phase phase = Phase::kProvisional;
    bool update_active = false;
    bool operation_active = false;
    bool gpu_attached = false;
    uint64_t source_serial = 0;
    size_t physical_pins = 0;
    gpu::GpuCompletionStatus completion =
        gpu::GpuCompletionStatus::kUnsupported;
    // An admitted attachment may represent a real submit even when its ticket
    // fails validation. CPU-only teardown must reject this sticky uncertainty.
    bool submission_attempted = false;
  };

  static std::shared_ptr<NativeFrameOwnerSession> CreateProvisional(
      ThreadIdentity thread, uint64_t kernel_epoch,
      std::shared_ptr<GuestRuntime> runtime);
  static std::shared_ptr<NativeFrameOwnerSession> CreateDelegated(
      const std::shared_ptr<NativeSystemFrameDelegation>& delegation,
      std::shared_ptr<GuestRuntime> runtime);
  const Owner& owner() const { return owner_; }
  // Host metadata only; independent of a per-frame Request/Source. Does not
  // grant GPU execution, run PPC or reopen a consumed preparation.
  bool IsPresentationOwnerLive(uint64_t backend_epoch) const;
  bool MatchesThread(const ThreadIdentity& thread) const;
  Update BeginUpdate(const ThreadIdentity& thread);
  std::optional<Owner> AllocationOwner(const ThreadIdentity& thread) const;
  bool BindGraph(const ThreadIdentity& thread,
                 std::shared_ptr<GraphAuthority> graph);
  // Seals a completed native record and consumes the exact active Update.
  // The original guard's eventual destruction is then harmless/idempotent.
  std::shared_ptr<const Source> Freeze(
      const ThreadIdentity& thread, Update& update,
      std::span<const uint8_t> primary,
      std::shared_ptr<const gpu::NativeSystemTitleInput> title_input = {});
  // Nested post-End seal callback may consume the persistent active update;
  // the original outer guard need not be accessible in that callback.
  std::shared_ptr<const Source> FreezeCurrentUpdate(
      const ThreadIdentity& thread, std::span<const uint8_t> primary,
      std::shared_ptr<const gpu::NativeSystemTitleInput> title_input = {});
  bool AttachPhysicalPin(const ThreadIdentity& thread,
                         const std::shared_ptr<const Source>& source,
                         std::shared_ptr<const memory::PhysicalRangePin> pin);
  bool AbortUnsubmitted(const ThreadIdentity& thread,
                        const std::shared_ptr<const Source>& source);
  // Invalid/unverifiable attachment quarantines, since a native submit may
  // already have occurred. Do not call this as a capability query. Only an
  // opted-in real binding accepts an exact native CP ticket.
  // Retrieves the exact frozen source before handing its unforgeable prepared
  // request to the CP. Never reconstruct a source from public serial numbers.
  std::shared_ptr<const Source> SourceForExecutionRequest(
      const ThreadIdentity& thread,
      const gpu::NativeFrameExecutionRequest& request) const;
  bool AttachGpu(const ThreadIdentity& thread,
                 const std::shared_ptr<const Source>& source,
                 gpu::GpuCompletion completion);
  bool BeginRetire(const ThreadIdentity& thread,
                   const std::shared_ptr<const Source>& source);
  // Observer only publishes the stored token's state; never PPC or reopening.
  void ObserveCompletion(const gpu::GpuCompletion& completion);
  // Actual healthy owner thread must explicitly finish frame retirement.
  // Success returns to Bound; a NEW Update is still needed for allocations.
  bool RetireComplete(const ThreadIdentity& thread,
                      const std::shared_ptr<const Source>& source);
  void Revoke();
  // Kernel teardown supplies a real stopped-epoch proof after all producer,
  // consumer and GPU work stopped. Never infer it from a timeout/CPU progress.
  bool RetireAfterKernelStop(const std::function<bool(const Owner&)>& proof);
  Snapshot GetSnapshot() const;

 private:
  friend class NativeFrameLeaseBuilder;
  friend class NativeFrameInputCapture;
  std::unique_ptr<NativeFrameInputCapture> BeginInputCapture(
      Memory*, const ThreadIdentity&, const std::shared_ptr<const Source>&,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>&,
      memory::PhysicalRange primary, memory::PhysicalRange output);
  friend class NativeSystemAllocationAuthority;
  friend class NativeSystemFrameDelegation;
  static Owner ReserveAllocationOwner(uint64_t kernel_epoch,
                                      GuestRuntime::Identity runtime);
  bool RetireDelegatedIdle();
  bool MatchesCompletedRecord(const ThreadIdentity& thread,
                              const std::shared_ptr<const Source>& source,
                              uint32_t device, uint32_t dc);
  bool HoldPreparationPins(const ThreadIdentity& thread,
                           const std::shared_ptr<const Source>& source,
                           std::shared_ptr<void> pins);
  // Host-only admission tests use real Memory pins and no guest GPU objects.
  friend class test::NativeFrameExecutionTestAccess;
  class ExecutionAuthority;
  class CompletedReadbackAdmission;
  bool ValidateControlFrame(
      Memory*, const ThreadIdentity&, const std::shared_ptr<const Source>&,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>&,
      const NativeCompletedFrameRecord&, const std::array<uint8_t, 148>&,
      const std::shared_ptr<const NativeNonmodalControlProvenance>&);
  std::optional<gpu::NativeFrameOutputLayout> SnapshotOutputLayout(
      Memory* guest_memory, const ThreadIdentity& thread,
      const std::shared_ptr<const Source>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      std::span<const memory::PhysicalRange> input_ranges,
      const NativeCompletedFrameRecord& record);
  std::optional<gpu::NativeFrameOutputLayout> SnapshotPm4OutputLayout(
      Memory* guest_memory, const ThreadIdentity& thread,
      const std::shared_ptr<const Source>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      std::span<const memory::PhysicalRange> input_ranges,
      const NativeCompletedFrameRecord& record,
      const std::array<uint8_t, 148>& descriptor,
      const std::shared_ptr<const gpu::NativeFrameInputSnapshot>& snapshot =
          {},
      const std::shared_ptr<const NativeNonmodalOutputProvenance>&
          output_provenance = {},
      const std::optional<gpu::NativeOverlayState>& overlay_input_state = {});
  std::shared_ptr<const gpu::NativeFrameExecutionRequest> MintExecutionRequest(
      const ThreadIdentity& thread, const std::shared_ptr<const Source>& source,
      std::shared_ptr<const gpu::NativeFrameCommandPlan> plan,
      memory::PhysicalRange output,
      std::vector<memory::PhysicalRange> input_ranges,
      std::vector<gpu::NativeFrameExecutionRequest::Shader> shaders,
      std::optional<gpu::NativeFrameOutputLayout> output_layout = std::nullopt,
      std::shared_ptr<const gpu::NativeFrameInputSnapshot> input_snapshot = {});
  bool CheckExecutionRequest(
      const std::shared_ptr<const Source>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      uint64_t preparation, uint64_t backend_epoch,
      const gpu::NativeFrameExecutionRequest& request, bool consume);
  std::unique_ptr<gpu::NativeFrameReadbackAdmission> AcquireCompletedReadback(
      const std::shared_ptr<const Source>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      uint64_t backend_epoch, const gpu::NativeFrameExecutionRequest& request,
      const gpu::GpuCompletion& completion);
  bool FinishCompletedReadback(
      uint64_t operation, const std::shared_ptr<const Source>& source,
      const gpu::GpuCompletion& completion,
      gpu::NativeFrameReadbackAdmission::Outcome outcome) noexcept;
  // Owns one nonreused callback admission, and retains this control block even
  // if a callback erases the caller's reference. Revocation never drains it.
  class Operation {
   public:
    explicit Operation(std::shared_ptr<NativeFrameOwnerSession> session)
        : session_(std::move(session)) {}
    ~Operation();
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;

   private:
    friend class NativeFrameOwnerSession;
    const std::shared_ptr<NativeFrameOwnerSession> session_;
    uint64_t serial_ = 0;
  };
  NativeFrameOwnerSession(ThreadIdentity thread, Owner owner,
                          std::shared_ptr<GuestRuntime> runtime);
  uint64_t StartOperationLocked();
  void EndOperation(uint64_t serial);
  bool RuntimeAdmitted(bool provisional) const;
  bool SourceMatches(const std::shared_ptr<const Source>& source) const;
  bool FinishFrame(const ThreadIdentity& thread,
                   const std::shared_ptr<const Source>& source, bool submitted);
  bool UpdateActive(uint64_t serial) const;
  void EndUpdate(uint64_t serial);
  void QuarantineLocked();

  const ThreadIdentity thread_;
  const Owner owner_;
  std::weak_ptr<NativeSystemFrameDelegation> delegation_;
  bool delegated_ = false;
  std::shared_ptr<void> frame_writer_;
  std::shared_ptr<void> preparation_pins_;
  std::shared_ptr<GuestRuntime> runtime_;
  mutable std::mutex mutex_;
  Phase phase_ = Phase::kProvisional;
  uint64_t next_update_ = 1, active_update_ = 0, next_source_ = 1;
  uint64_t next_operation_ = 1, active_operation_ = 0;
  uint64_t next_preparation_ = 1, active_preparation_ = 0;
  bool gpu_attached_ = false;
  bool submission_attempted_ = false;
  gpu::GpuCompletionStatus observed_ = gpu::GpuCompletionStatus::kUnsupported;
  gpu::GpuCompletion completion_;
  std::shared_ptr<GraphAuthority> graph_;
  std::shared_ptr<const Source> source_;
  std::vector<std::shared_ptr<const memory::PhysicalRangePin>> pins_;
  std::shared_ptr<NativeFrameOwnerSession> quarantine_pin_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_FRAME_OWNER_H_
