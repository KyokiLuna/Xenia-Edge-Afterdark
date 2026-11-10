/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_GRAPHICS_TRANSPORT_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_GRAPHICS_TRANSPORT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <vector>

#include "xenia/base/physical_range_leases.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/kernel/native_system_ui_service.h"

namespace xe::gpu {
class NativeFrameExecutionRequest;
struct NativeFrameExecutionResult;
}  // namespace xe::gpu

namespace xe::kernel {
class NativeSystemGraphicsNotifications;
class NativeSystemUiVisibilityEvent;
class NativeNonmodalFrameWitness;

// The system renderer has one execution owner: direct native CP, never a
// Title Get/Swap checkout as well. Native FE still owns all UI/Task policy.
class NativeSystemGraphicsTransport final
    : public NativeSystemUiServiceTransport,
      public std::enable_shared_from_this<NativeSystemGraphicsTransport> {
 public:
  enum class HookResult { kUnhandled, kHandled, kRejected };
  struct QueryResult {
    HookResult disposition = HookResult::kUnhandled;
    uint32_t available = 0;
  };
  // Process-scoped, saturating first-record diagnostics. No Kernel pointer,
  // guest object, admission or production readiness is stored by these.
  enum class DiagnosticCounter : uint32_t {
    kQueryExport,
    kQueryNoTransport,
    kQueryTransport,
    kQueryRejected,
    kQueryAcquire,
    kQueryMode,
    kQueryContext,
    kQueryReturn,
    kQueryAvailable,
    kQueryBusy,
    kQueryException,
    kSetExport,
    kSetNoTransport,
    kSetTransport,
    kSetRejected,
    kSetAcquire,
    kSetContext,
    kSetReturn,
    kSetStack,
    kSetDescriptor,
    kSetFrame,
    kSetRenderer,
    kSetTask,
    kSetStoppedOrObserved,
    kSetOtherRejected,
    kSetHandled,
    kSetEmptyAccepted,
    kSetNonemptyAccepted,
    kSetException,
    kCount
  };
  using DiagnosticCounters =
      std::array<uint64_t, std::size_t(DiagnosticCounter::kCount)>;
  static void RecordDiagnosticCounter(DiagnosticCounter) noexcept;
  static DiagnosticCounters ReadDiagnosticCounters() noexcept;
  enum class Mode { kUninstalled, kObservationOnly, kProduction, kStopped };
  enum class Blocker {
    kNone,
    kNativeWriterExclusion,
    kSharedGraphObservation,
    kFeRegistryObservation,
    kUnsupportedRecordProfile,
    kInvalidCaller,
    kInvalidRecord,
    kBackendFailure,
    kStopped
  };
  struct FirstRecord {
    enum class Kind { kEmptyReset, kCommandOnly, kOutput, kMalformed };
    Kind kind = Kind::kMalformed;
    std::array<uint8_t, 148> descriptor{};
    memory::PhysicalAllocationOwner owner;
    uint64_t root_load_generation = 0;
    // manager_task is the actual renderer/state-loop task at manager+28,
    // not the separate load/request task at manager+4.
    uint32_t thread_id = 0, native_task = 0, manager_task = 0;
    uint32_t renderer_task_cell = 0;
    uint32_t return_address = 0, renderer_return_address = 0;
    uint32_t device = 0, dc = 0, recording_context = 0;
    uint32_t command_physical = 0, command_dwords = 0, output_physical = 0;
    memory::PhysicalAllocationIdentity command_allocation, output_allocation;
    bool genuine_set_helper = false, matching_native_task = false;
    bool native_end_path = false, command_owned = false, output_owned = false;
    bool source_frozen = false, submitted = false, retired = false;
    // An owned bounded CPU observation, NOT an immutable Source or proof that
    // asynchronous native writers were excluded. No GPU read/permission.
    std::vector<uint8_t> observed_primary;
  };
  struct NativePublicationMetadata {
    bool cold_zero_observed = false, fe_registry_linked = false;
    bool startup_event_observed = false, shared_graph_proven = false;
    bool device_addref_attempted = false, device_addref_owned = false;
    bool dc_addref_attempted = false, dc_addref_owned = false;
    uint32_t startup_task = 0, task_status = UINT32_MAX;
    uint32_t startup_flag_2190 = 0, startup_flag_2194 = 0;
    uint32_t event_type = UINT32_MAX, event_state = 0;
    uint32_t device = 0, dc = 0, recording_context = 0;
    uint32_t device_addref_result = 0, dc_addref_result = 0;
  };
  struct Snapshot {
    Mode mode = Mode::kUninstalled;
    Blocker blocker = Blocker::kNativeWriterExclusion;
    uint64_t backend_epoch = 0, identifier_binding_generation = 0;
    uint32_t identifier_address = 0;
    bool stopping = false, drained = false, retired = false;
    bool ready = false, callback_pending = false, reset_pending = false;
    uint64_t source_serial = 0, native_submission = 0;
    uint64_t empty_reset_count = 0;
    uint32_t last_empty_renderer_return_address = 0;
    bool visibility_known = false, visible = false, visibility_revoked = false;
    uint64_t visibility_sequence = 0, visibility_foreground_generation = 0;
    gpu::GpuCompletionStatus completion =
        gpu::GpuCompletionStatus::kUnsupported;
    std::shared_ptr<const FirstRecord> first_record;
    NativePublicationMetadata native_publication;
  };

  // Fixed opt-in production diagnostic metadata, never display/ACK authority.
  // One actual postboot Task owns the queue boundaries; the transport owns
  // real Source/readback/paint observations and retains them through stop.
  struct NotificationRenderProbeSnapshot {
    bool armed = false, closed = false, queue_entered = false, queue_returned = false;
    bool failed = false, selected_retired = false, capture_saved = false;
    bool readback_complete = false, permission_for_selected = false;
    bool hidden_after_selected = false, genuine_clear = false;
    // Once complete is observed, these image/paint fields retain immutable
    // historical facts. They never assert current visibility or residency.
    bool image_ready = false, presenter_accepted = false, paint_complete_observed = false;
    uint64_t backend_epoch = 0, foreground_generation = 0, title_input_swap = 0;
    uint64_t baseline_source = 0, queue_enter_source = 0, queue_return_source = 0;
    uint64_t completed_sources = 0, first_disabled_source = 0;
    uint64_t last_retired_source = 0, query_reopened_source = 0;
    uint64_t selected_source = 0, native_submission = 0, retained_bytes = 0;
    uint64_t image_version = 0, paint_submission = 0, paint_backend_epoch = 0;
    uint32_t width = 0, height = 0, readback_status = 0, paint_status = 0;
  };
  enum class NotificationRenderProbeMode {
    kFirstEligible,
    kExactSource256Diagnostic
  };
  bool
  ArmNotificationRenderProbe(XThread &, const std::filesystem::path &,
                             NotificationRenderProbeMode =
                                 NotificationRenderProbeMode::kFirstEligible);
  bool ObserveNotificationRenderProbeQueue(XThread&, bool normal_return);
  bool CloseNotificationRenderProbe(XThread&);
  std::optional<NotificationRenderProbeSnapshot> PollNotificationRenderProbe(
      XThread&);

  static std::shared_ptr<NativeSystemGraphicsTransport> Create();
  static std::shared_ptr<NativeSystemGraphicsTransport> Lookup(KernelState*);
  ~NativeSystemGraphicsTransport() override;
  std::shared_ptr<NativeSystemUiPreparedTransport> PrepareBeforeWorkers(
      const NativeSystemUiServiceContext&, XThread&) override;
  // Explicit diagnostic install, outside Prepared's production attestation.
  // Never changes hardware0x200, Boot/SourceReady, guest ACK or GPU completion.
  // Empty reset/drain handshakes remain available. Query pauses only after
  // the first nonempty command descriptor, for host inspection.
  bool InstallObservationOnly(const NativeSystemUiServiceContext&, XThread&);
  // Common phase2 handoff for production and first-record observation. Only
  // the typed publication is adopted; every other cached identity is fixed.
  bool ObserveSharedDevicePublication(const NativeSystemUiServiceContext&,
                                      XThread&);
  QueryResult Query(uint32_t mode, uint32_t return_address);
  HookResult Set(uint32_t descriptor, uint32_t return_address);
  HookResult Get(uint32_t descriptor_out, uint32_t identifier_out,
                 uint32_t return_address);
  HookResult SetIdentifierAddress(uint32_t address, uint32_t return_address);
  std::shared_ptr<NativeSystemGraphicsNotifications> notifications() const;
  Snapshot snapshot() const;
  // Genuine native SYS_UI9 only; does not replace native196 input counters.
  void OnNativeSystemUiVisibility(
      const NativeSystemUiVisibilityEvent&) noexcept;
  // Host authority failure/stop, not a fabricated native hide notification.
  void OnNativeSystemUiVisibilityRevoked() noexcept;
  // Root closes native ingress/bootstrap before Drain. Actual backend/CPU stop
  // precedes Retire; external handles become metadata-only tombstones.
  void CloseExternalRequests() noexcept;
  void BeginStop() noexcept;
  bool Drain() noexcept;
  bool RetireAfterKernelStop() noexcept;

 private:
  struct State;
  class Prepared;
  explicit NativeSystemGraphicsTransport(std::shared_ptr<State>);
  bool Install(const NativeSystemUiServiceContext&, XThread&, Mode);
  bool SubmitPrepared();
  bool PollOnNativeTask(XThread&, uint32_t return_address);
  void PublishPlaneVisibility() noexcept;
  void OnNativeNonmodalFrame(
      const NativeNonmodalFrameWitness&,
      const gpu::NativeFrameExecutionRequest* = nullptr,
      const gpu::NativeFrameExecutionResult* = nullptr) noexcept;
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_SYSTEM_GRAPHICS_TRANSPORT_H_
