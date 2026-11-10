/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_UI_STARTUP_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_UI_STARTUP_H_
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "xenia/base/physical_range_leases.h"
#include "xenia/kernel/guest_runtime.h"
#include "xenia/kernel/native_system_title_heap_data.h"
namespace xe::kernel {
class GuestRuntimeCallExecutor;
class NativeSystemAllocationAuthority;
class XThread;
class NativeSystemUiService;
class NativeSystemContent;
struct NativeSystemInputCallbackProfile {
  uint32_t user_binding = 0, device_config = 0, failed_binding = 0;
};
// Native call roles, not a new host Task, window manager or UI policy machine.
enum class NativeSystemUiStartupPhase {
  kPrepareSeams = 0,
  kTask = 1,
  kSharedGraphics = 2,
  kConfig = 3,
  kInputAuxiliary = 4,
  kUpdatePredicate = 5,
  kFeRegistry = 6,
  kUiManager = 7,
  kNotifications = 8,
  kSystemContentSource = 9,
  kSystemContentIndex = 10,
  kSystemContentLookup = 11,
  kSystemStorageInitialized = 12,
  kSystemStorageBootCompleted = 13
};
struct NativeSystemUiStartupSymbols {
  uint32_t manager_base = 0, dc_cell = 0;
  uint32_t shared_device_cell = 0, record_context_cell = 0;
  std::array<uint32_t, 3> command_buffer_cells{};
  uint32_t manager_task_cell = 0, manager_startup_task_cell = 0;
  uint32_t task_tls_slot_cell = 0, task_worker_entry = 0;
  uint32_t configuration_object_cell = 0;
  std::array<uint32_t, 4> input_task_cells{};
};
struct NativeSystemSharedGraphicsState {
  enum class Status { kNotInitialized, kDcPending, kObserved, kInvalid };
  Status status = Status::kNotInitialized;
  NativeSystemUiStartupSymbols symbols;
  uint32_t device = 0, recording_context = 0, dc = 0;
  std::array<uint32_t, 3> command_buffers{};  // actual CPU aliases, not masked
  std::array<memory::PhysicalRange, 3> command_ranges{};
  std::shared_ptr<const memory::PhysicalRangePin> command_pin;
  // Proven existing factory/layout catalog metadata; never called again here.
  GuestRuntime::Function factory;
  std::string provenance_sha256;
};
struct NativeSystemStartupTaskFunctions {
  GuestRuntime::Function schedule, create, modify, wait, close, current, status;
};
class NativeSystemUiStartupCatalog {
 public:
  static std::shared_ptr<const NativeSystemUiStartupCatalog> Load(
      const std::shared_ptr<GuestRuntimeCallExecutor>&,
      const std::shared_ptr<NativeSystemAllocationAuthority>&,
      std::string* error);
  ~NativeSystemUiStartupCatalog();
  const GuestRuntime::Plan& plan() const;
  const GuestRuntime::Plan& early_plan() const;
  const GuestRuntime::Plan& frontend_plan() const;
  const NativeSystemStartupTaskFunctions& task_functions() const;
  // Transport-only verified unused private-root81696558 entry; never execute
  // its original whole-boot body. Root may instead use a trusted Kernel thunk.
  const GuestRuntime::Function& frontend_callback_entry() const;
  const std::shared_ptr<GuestRuntimeCallExecutor>& calls() const;
  const std::shared_ptr<NativeSystemAllocationAuthority>& allocation() const;
  // Present only for the complete checked HDD startup family. Original Guide
  // plans have none; a successful index init alone is never lookup readiness.
  std::shared_ptr<const NativeSystemContent> system_content() const;
  // Installed KRNL XboxHardwareInfo156 import, bounded actual field read.
  // Neither original r31 boot-mode nor an invented private global is used.
  std::optional<uint32_t> ReadHardwareFlags() const;
  const NativeSystemUiStartupSymbols& symbols() const;
  // Generation/hash-validated symbols; absence of native DC after synchronous
  // ctor is kDcPending, not failure or firstFrame. Root may actual-XThread
  // Delay poll while native workers/providers are live. Never guesses manager
  // fields.
  NativeSystemSharedGraphicsState ReadSharedGraphicsState() const;
  // Native NotifyStartup is void and ignores its allocator/register results.
  // Require the actual root generation, native comparer, vector allocation,
  // registered areas and event counts before frontend/listener initialization.
  // This observes initialization only, never a notification or UI owner.
  bool CheckNativeNotificationInitialization(
      std::string* error = nullptr) const;
  // Host implementation of the three verified Kernel registrations in original
  // Main. This profile is read once BEFORE the first callback can run. It never
  // executes a Main interior or writes native tables/event/voice cells.
  std::optional<NativeSystemInputCallbackProfile> ReadInputCallbackBootstrap(
      XThread& actual_bootstrap, std::string* error = nullptr) const;
  // Rare physical change/BeforeEnter check; records/event may now be native
  // mutated/materialized. No pristine-bootstrap recheck after enqueue.
  bool CheckInputCallbackContext(std::string* error = nullptr) const;

 private:
  friend class NativeSystemUiService;
  // Fixed compiled17559 profile, separate from the immutable923 heap plan.
  // Only the service's once-only prepared-Title record can execute this role.
  GuestRuntime::Plan PreparedTitleHeapPlan(
      const NativeSystemPreparedTitle&) const;
  GuestRuntime::ExecutionResult InitializePreparedTitleHeap(
      XThread& actual_bootstrap, const NativeSystemPreparedTitle&) const;
  struct State;
  explicit NativeSystemUiStartupCatalog(std::unique_ptr<State>);
  friend class NativeSystemUiStartupRuntime;
  std::unique_ptr<State> state_;
};
// Root's actual service/Vd adapter implements these observations. Defaults
// deny. A call's successful result is not substituted for native worker/DC/
// config/registration facts. Methods run with no GuestRuntime/host lock held.
class NativeSystemUiStartupSeams {
 public:
  virtual ~NativeSystemUiStartupSeams() = default;
  // Must install exact profile/domain binding + native allocation attribution,
  // retained driver/dispatcher/Vd notification/Query/record transport BEFORE
  // Task workers start; capable of progress without a title Swap.
  virtual GuestRuntime::ExecutionResult PrepareBeforeWorkers(
      const NativeSystemUiStartupCatalog&, XThread&);
  // Actual synchronous creation/ownership postconditions only. UI constructor
  // schedules its native startup; this MUST NOT demand a rendered first Guide.
  // Dispatch ONE real native Task and wait for this complete work closure.
  // Callback ABI is int32(context); native scheduler owns TLS/currentTask.
  // A trusted private-root thunk/verified-unused81696558 bridge may transport
  // this callback, but never execute whole boot/LaunchTitle. Defaults deny.
  // Stop closes new external ingress but must drain this admitted WHOLE C++
  // native Task callback (and bootstrap) BEFORE GuestScheduler::Shutdown can
  // reclaim its fiber. Keep driver/Vd/backend live until return; if unprovable,
  // fail-stop retain rather than forcekill/fakeReady/abandoned C++ references.
  // The adapter retains code/domain/context through actual Task completion,
  // adopts Schedule's owned handle only on signed success, and explicitly
  // native-Wait/Close while healthy. Wait/GetStatus are not work result.
  using FrontendWork = std::function<GuestRuntime::ExecutionResult(XThread&)>;
  virtual GuestRuntime::ExecutionResult ExecuteFrontendOnNativeTaskAndWait(
      const NativeSystemUiStartupCatalog&, XThread& bootstrap, FrontendWork);
  virtual GuestRuntime::ExecutionResult ObserveAfter(
      NativeSystemUiStartupPhase, const NativeSystemUiStartupCatalog&,
      XThread&);
};
class NativeSystemUiStartupRuntime
    : public std::enable_shared_from_this<NativeSystemUiStartupRuntime> {
 public:
  enum class Status { kBootAdmitted, kAlreadyBootAdmitted, kBusy, kPoisoned };
  static std::shared_ptr<NativeSystemUiStartupRuntime> Create(
      std::shared_ptr<const NativeSystemUiStartupCatalog>);
  Status Initialize(XThread&, std::shared_ptr<NativeSystemUiStartupSeams>);
  struct Snapshot {
    GuestRuntime::Snapshot early;
    GuestRuntime::Snapshot frontend;
  };
  Snapshot snapshot() const;
  // Separate once-only startup record. Heap GuestRuntime remains unchanged.
  // Ready here means synchronous boot admission, not Running/input owner/
  // manager state-loop/DC/firstFrame or permission for arbitrary public UI.
  bool IsBootAdmitted() const;
  GuestRuntime::DrainStatus RetireAndDrain(bool allow_wait = true);

 private:
  explicit NativeSystemUiStartupRuntime(
      std::shared_ptr<const NativeSystemUiStartupCatalog>,
      std::shared_ptr<GuestRuntime> early,
      std::shared_ptr<GuestRuntime> frontend);
  GuestRuntime::ExecutionResult InitializeFrontend(
      XThread& actual_native_task,
      const std::shared_ptr<NativeSystemUiStartupSeams>&);
  std::shared_ptr<const NativeSystemUiStartupCatalog> catalog_;
  std::shared_ptr<GuestRuntime> early_runtime_;
  std::shared_ptr<GuestRuntime> frontend_runtime_;
  std::mutex dispatch_mutex_;
  bool frontend_dispatch_claimed_ = false;
};
}  // namespace xe::kernel
#endif
