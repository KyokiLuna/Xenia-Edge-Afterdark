/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_UI_SERVICE_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_UI_SERVICE_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
#include "xenia/kernel/native_system_ui_startup.h"

namespace xe::kernel {
class KernelState;
class NativeGraphicsDeviceBinding;
class NativeImportDomain;
class NativeInputDriver;
class NativeSystemUiFirstRecordProbe;
class NativeSystemAllocationAuthority;
class NativeSystemCallbackDispatcher;
class XThread;
struct ConsoleSettingsUpdate;

struct NativeSystemUiServiceContext {
  KernelState* kernel = nullptr;
  std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
  std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  std::shared_ptr<NativeImportDomain> domain;
  std::shared_ptr<NativeSystemCallbackDispatcher> dispatcher;
  std::shared_ptr<NativeInputDriver> input;
  // Staged Kernel/cohort-owned publication; not DC/Source/Boot readiness.
  std::shared_ptr<NativeGraphicsDeviceBinding> device_publication;
};

// Explicit CPU startup/first-record observation, separate from the production
// Prepared transport below. This may pause the native producer after copying a
// real Set descriptor, but never submits/retires a Source, acknowledges a GPU
// identifier, or establishes production Boot/SourceFrameReady. It shares the
// real bootstrap/Task/provider lifetime and has the same stopped-Kernel drain.
class NativeSystemUiDiagnosticObservation {
 public:
  virtual ~NativeSystemUiDiagnosticObservation() = default;
  virtual GuestRuntime::ExecutionResult PrepareBeforeWorkers(
      const NativeSystemUiServiceContext&, XThread&) = 0;
  virtual gpu::GpuBackendStopProof backend_stop_proof() const = 0;
  virtual GuestRuntime::ExecutionResult ObserveNativePhase(
      NativeSystemUiStartupPhase, const NativeSystemUiServiceContext&,
      XThread&) = 0;
  virtual GuestRuntime::ExecutionResult ObserveAfterNativeBoot(
      const NativeSystemUiServiceContext&, XThread&) = 0;
  virtual void CloseExternalRequests() noexcept = 0;
  virtual void BeginStopTransport() noexcept = 0;
  virtual bool DrainCallbacksBeforeCpuStop() noexcept = 0;
  virtual bool RetireAfterKernelStop() noexcept = 0;
};

// Implemented only by the real Vd/backend adapter. Returning this retained
// control record attests actual NoSwap safe points/query/resource epochs,
// exclusive Set/Get execution ownership, record/provenance/write exclusion,
// real completion, graphics notification rundown and a real resident plane.
// No host GPU handles may be owned here: Kernel retains this through teardown.
// No host UI/FF policy or native Task manager belongs in this interface.
class NativeSystemUiPreparedTransport {
 public:
  virtual ~NativeSystemUiPreparedTransport() = default;
  virtual gpu::GpuBackendStopProof backend_stop_proof() const = 0;
  // Additional actual kernel/import/registration postconditions. In particular
  // FE registry linkage must be observed, not inferred from constructor status.
  virtual GuestRuntime::ExecutionResult ObserveNativePhase(
      NativeSystemUiStartupPhase, const NativeSystemUiServiceContext&,
      XThread&);
  // Actual native writer/publication exclusion spanning catalogue readback and
  // borrowed binding creation and AttachSharedGraphics below. Null means not
  // yet observable; no fake lease. Attach must not reacquire an excluded mutex.
  virtual std::shared_ptr<void> AcquireSharedGraphicsObservation(XThread&);
  virtual GuestRuntime::ExecutionResult AttachSharedGraphics(
      const NativeSystemUiServiceContext&,
      const NativeSystemSharedGraphicsState&,
      const std::shared_ptr<NativeGraphicsDeviceBinding>&, XThread&);
  // Original-caller public entry only: a real transport operation/rundown
  // reservation, not a native Task, GPU submission or host modal owner. Null
  // denies. It must remain valid for this entire synchronous native call.
  virtual std::shared_ptr<void> AcquirePublicCallOperation(
      NativeSystemUiPublicCall, XThread&);
  // Called with an existing public operation; no PPC or UI work.
  virtual NativeUiBackendRoute::Admission AdmitUiCreating(bool allow_host);
  virtual std::shared_ptr<NativeUiBackendRoute::HostLease> CurrentHostUi();
  virtual bool SealHostUiFallback() noexcept;
  virtual bool AllowPhysicalInput() const noexcept;
  // Close only NEW external requests; an admitted bootstrap/frontend must still
  // finish or poison between calls with driver/Vd/backend providers alive.
  virtual void CloseExternalRequests() noexcept;
  // Invoked only after actual bootstrap join and continuation drain. Close Vd
  // source/callback ingress, retaining uncertain Sources until actual stop.
  virtual void BeginStopTransport() noexcept;
  virtual bool DrainCallbacksBeforeCpuStop() noexcept;
  // Actual joined CPU + same-backend stop already proved. Clear retained
  // Context/Source/module/control ownership and raw Kernel pointers; no PPC or
  // host GPU calls. External Prepared handles become closed metadata only.
  virtual bool RetireAfterKernelStop() noexcept;
};

class NativeSystemUiServiceTransport {
 public:
  virtual ~NativeSystemUiServiceTransport() = default;
  // The service already installed the exact bound domain, dispatcher and
  // driver. Install actual Vd/query/record/notification/backend support first.
  // Only then may the adapter advertise the validated virtual hardware profile
  // through actual KRNL156, preserving old flags and adding known0x200. Default
  // denies. Trusted early driver callback registration is permitted only within
  // this actual SYSTEM bootstrap method; the service never fabricates a guest
  // LR.
  virtual std::shared_ptr<NativeSystemUiPreparedTransport> PrepareBeforeWorkers(
      const NativeSystemUiServiceContext&, XThread&);
};

class NativeSystemUiService
    : public std::enable_shared_from_this<NativeSystemUiService> {
 public:
  // A real ExRMW invocation receipt, captured before console IO with the actual
  // native caller/root/domain leases. Complete observes the immutable outcome
  // after that transaction returned. It cannot change native cache/return.
  class NotificationPreferencePersistence {
   public:
    ~NotificationPreferencePersistence();
    void Complete(const ConsoleSettingsUpdate&) noexcept;

   private:
    friend class NativeSystemUiService;
    struct State;
    explicit NotificationPreferencePersistence(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
  };
  static std::unique_ptr<NotificationPreferencePersistence>
  CaptureNotificationPreferencePersistence(KernelState*, cpu::ppc::PPCContext*,
                                           uint16_t category, uint16_t setting,
                                           uint32_t and_mask,
                                           uint32_t or_bits) noexcept;
  struct Config {
    KernelState* kernel = nullptr;
    std::string native_root_guest_path;
    std::filesystem::path validated_plan_path;
    std::shared_ptr<NativeSystemUiServiceTransport> transport;
    // Mutually exclusive with production transport; an explicit diagnostic
    // coordinator uses this interface, never a fabricated Prepared instance.
    std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
    // Actual executable/Main prepared by this Kernel before its launch suspend
    // is released. A replacement needs a fresh Kernel, never heap0 reinit.
    NativeSystemPreparedTitle prepared_title;
  };
  struct Snapshot {
    bool start_claimed = false, stopping = false, heap_ready = false;
    bool boot_admitted = false, source_frame_ready = false;
    bool bootstrap_joined = false, stop_drained = false;
    bool title_heap_ready = false;
    bool diagnostic_boot_observed = false;
    std::string failure;
  };
  // Exactly one resident service per actual Kernel; retained before Start.
  // Resource paths identify a fresh, separate native root, never title XAM.
  static std::shared_ptr<NativeSystemUiService> Create(Config);
  ~NativeSystemUiService();
  bool Start();
  Snapshot snapshot() const;
  std::shared_ptr<NativeGraphicsDeviceBinding> graphics_binding() const;
  void BeginStop() noexcept;
  // External host only. Seals Start/creation and joins the actual XHostThread
  // native host handle before touching continuation/allocation/provider stop.
  bool JoinBootstrap(
      std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
  // Does not claim full CPU/GPU stop. Must precede the existing Kernel cohort
  // driver/dispatcher/timer/IRQ/scheduler/CP shutdown; false requires fail-stop
  // retention. No native PPC is executed by stop/destruction.
  bool StopBeforeCpuAndProviders() noexcept;
  static void StopForKernelOrFailStop(KernelState*) noexcept;
  // Pre-ClearDialogs phase, before full provider/native teardown.
  static void SealHostUiFallbackForKernelOrFailStop(KernelState*) noexcept;
  // Only kInactive or an explicit host_fallback lease permits HLE. After native
  // admission, denied/failed/closed native calls never silently switch
  // implementation. Executes synchronously on the ACTUAL original TITLE or
  // SYSTEM XThread.
  static NativeSystemUiPublicCallResult TryInvokePublicOnOriginalCaller(
      KernelState*, NativeSystemUiPublicCall,
      std::span<const uint32_t>) noexcept;
  // Explicit register values for profiles such as 2CB. Only the verified
  // per-profile wide slots may exceed DWORD; all pointer slots stay DWORD.
  static NativeSystemUiPublicCallResult
  TryInvokePublicRegistersOnOriginalCaller(KernelState*,
                                           NativeSystemUiPublicCall,
                                           std::span<const uint64_t>) noexcept;

 private:
  friend class NativeSystemUiFirstRecordProbe;
  // Fixed opt-in diagnostic only: old first-record observation or the strict
  // production-render branch on its already-owned bootstrap. One existing
  // postboot Task; no public family admission or settings mutation.
  static GuestRuntime::ExecutionResult InvokeFixedNotificationDiagnostic(
      KernelState*, XThread&);
  struct State;
  class StartupSeams;
  explicit NativeSystemUiService(std::shared_ptr<State>);
  static int Bootstrap(const std::shared_ptr<State>&);
  static bool ObserveSharedGraphics(const std::shared_ptr<State>&, XThread&,
                                    bool required);
  static bool ObserveInitialNotificationPreferences(
      const std::shared_ptr<State>&, XThread&);
  bool RetireAfterKernelStop() noexcept;
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_SYSTEM_UI_SERVICE_H_
