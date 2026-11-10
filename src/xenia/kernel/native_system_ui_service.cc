/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_ui_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/host_storage.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_content_storage_driver.h"
#include "xenia/kernel/native_managed_format_frontdoor.h"
#include "xenia/kernel/native_current_user_provider.h"
#include "xenia/kernel/native_graphics_device_binding.h"
#include "xenia/kernel/native_hud_module_ownership.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_input_driver.h"
#include "xenia/kernel/native_notification_preference_coordinator.h"
#include "xenia/kernel/native_notification_preference_readback.h"
#include "xenia/kernel/native_notification_presentation.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_callback_dispatcher.h"
#include "xenia/kernel/native_system_content.h"
#include "xenia/kernel/native_system_graphics_transport.h"
#include "xenia/kernel/native_system_task_continuation.h"
#include "xenia/kernel/native_xui_imports.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

DECLARE_bool(native_system_first_record_probe);
DECLARE_bool(native_system_ui);
DECLARE_int64(native_system_ui_output_capture_source);
DECLARE_string(native_system_ui_output_capture_base);
DECLARE_bool(native_system_ui_output_capture_b_down);
DECLARE_path(native_system_ui_startup_plan);
DEFINE_path(native_notification_render_probe_report, "",
            "Explicit one-request production native-render diagnostic on marked "
            "disposable storage. Requires real native_system_ui and an exact "
            "native-notification-render.jsonl report under its disposable root. "
            "Never enables public Notifications or certifies pixels/audio.",
            "Kernel.Debug");
DEFINE_path(native_notification_preferences_probe_report, "",
            "Explicit private native292/293 diagnostic on disposable seeded "
            "storage. Requires first-record diagnostic and a new "
            "native-preferences-set.jsonl under that storage root. Never "
            "enables notification routes or changes normal launches.",
            "Kernel.Debug");
DEFINE_path(native_notification_queue_probe_report, "",
            "Explicit one-job native290 Task diagnostic on marked disposable "
            "storage. Requires the first-record probe. Never enables the "
            "notification family or awards an achievement.", "Kernel.Debug");

namespace xe::kernel {
namespace {
GuestRuntime::ExecutionResult Fail(std::string text) {
  return {false, 0x80004005u, std::move(text)};
}
GuestRuntime::ExecutionResult Ok() { return {true, 0, {}}; }
std::optional<uint32_t> Word(Memory* memory, uint32_t address) {
  if (!address || (address & 3) ||
      memory->QueryRangeAccess(address, 4) == memory::PageAccess::kNoAccess) {
    return {};
  }
  return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
}
bool ActualSystem(XThread& thread, KernelState* kernel) {
  return XThread::IsInThread(&thread) && thread.kernel_state() == kernel &&
         thread.guest_object<X_KTHREAD>()->process_type == X_PROCTYPE_SYSTEM;
}
// Observe only calls the real status API was already going to make. Task state
// may move concurrently, so the separately sampled state is not an atomic
// witness of the returned status. No guest call or task mutation is added.
void TraceNativeTaskObservation(
    const NativeSystemUiServiceContext& context, XThread& thread,
    NativeSystemUiStartupPhase phase, uint32_t cell,
    std::optional<uint32_t> handle, bool pointer_checked, uint32_t reason,
    const GuestRuntime::ExecutionResult* status) noexcept {
  if (!cvars::native_system_first_record_probe ||
      !ActualSystem(thread, context.kernel)) {
    return;
  }
  static std::atomic<uint32_t> count{0};
  uint32_t record = count.load(std::memory_order_relaxed);
  while (record < 8 && !count.compare_exchange_weak(
                           record, record + 1, std::memory_order_relaxed)) {
  }
  if (record >= 8) {
    return;
  }
  try {
    const auto& symbols = context.catalog->symbols();
    auto* memory = context.kernel->memory();
    constexpr std::array<uint32_t, 2> startup_flags{0x81AA2190, 0x81AA2194};
    const std::array<uint32_t, 5> cells{
        symbols.manager_task_cell, symbols.manager_startup_task_cell,
        startup_flags[0], startup_flags[1], symbols.dc_cell};
    uint32_t readable = 0;
    std::array<uint32_t, 5> values{};
    for (size_t i = 0; i < cells.size(); ++i) {
      if (const auto word = Word(memory, cells[i])) {
        readable |= 1u << i;
        values[i] = *word;
      }
    }
    std::optional<uint32_t> state;
    if (pointer_checked && handle && (*handle & 3) == 1 &&
        (*handle & ~1u) <= UINT32_MAX - 0x10) {
      // Exact17559 GetStatus/Task initialization prove the state DWORD +0x10.
      state = Word(memory, (*handle & ~1u) + 0x10);
    }
    const auto flags = context.catalog->ReadHardwareFlags();
    const auto root = context.catalog->calls()->module();
    XELOGI(
        "Native first-record Task check: phase={} cell={:08X} readable={} "
        "handle={:08X} tag={} pointer_checked={} state_readable={} "
        "sampled_state={:08X} status_called={} call_succeeded={} "
        "status={:016X} "
        "reason={} tid={:08X} root={:08X}/{} domain={}",
        uint32_t(phase), cell, handle.has_value(), handle.value_or(0),
        handle.value_or(0) & 3, pointer_checked, state.has_value(),
        state.value_or(0), status != nullptr, status && status->succeeded,
        status ? status->return_value : 0, reason, thread.thread_id(),
        root->hmodule_ptr(), root->load_generation(), context.domain->id());
    XELOGI(
        "Native first-record manager guards: readable={:02X} "
        "created_task={:08X} startup_task={:08X} flag2190={:08X} "
        "flag2194={:08X} dc={:08X} hardware_readable={} hardware={:08X}",
        readable, values[0], values[1], values[2], values[3], values[4],
        flags.has_value(), flags.value_or(0));
  } catch (...) {
    // Optional diagnostics never replace the ownership result or unwind PPC.
  }
}
bool ObserveFeFields(const NativeSystemUiServiceContext& context,
                     bool first_record_diagnostic) {
  const auto hud = ObserveNativeHudModuleOwnership(
      context, first_record_diagnostic
                   ? NativeHudOriginPolicy::kFirstRecordSystemRoot
                   : NativeHudOriginPolicy::kInstalledFlash);
  // Exact 17559 constructor proves +4=FE,+8=AAAABBBB,+C=callback,+10=1.
  // Validate its complete original code before interpreting those fields;
  // list/linkage internals are NOT inferred here and remain adapter proof.
  constexpr uint32_t address = 0x816BCCA8, size = 96;
  constexpr std::string_view expected =
      "67e2768dffed4e98d377274c666b60c967ce8c20313a3d058bed75da5ac4bc4e";
  const auto module = context.catalog->calls()->module();
  auto* memory = context.kernel->memory();
  xex2_section_type section;
  if (context.kernel->GetUserModuleForAddress(address).get() != module.get() ||
      context.kernel->GetUserModuleForAddress(address + size - 1).get() !=
          module.get() ||
      !module->xex_module()->GetPageSectionType(address, &section) ||
      section != XEX_SECTION_CODE ||
      memory->QueryRangeAccess(address, size) ==
          memory::PageAccess::kNoAccess) {
    return false;
  }
  for (uint32_t offset = 0; offset < size; offset += 4) {
    if (module->xex_module()->TranslateCode(address + offset) !=
        memory->TranslateVirtual<const uint8_t*>(address + offset)) {
      return false;  // Guest memory SHA alone cannot rule out a host overlay.
    }
  }
  std::array<uint8_t, 32> digest;
  mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(address), size,
                 digest.data(), 0);
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < digest.size(); ++i) {
    if (expected[2 * i] != hex[digest[i] >> 4] ||
        expected[2 * i + 1] != hex[digest[i] & 15]) {
      return false;
    }
  }
  constexpr uint32_t descriptor = 0x81AA0BA0, callback = 0x816BD930;
  return Word(memory, descriptor + 4) == 0xFEu &&
         Word(memory, descriptor + 8) == 0xAAAABBBBu &&
         Word(memory, descriptor + 12) == callback &&
         Word(memory, descriptor + 16) == 1u && hud.established &&
         context.kernel->ResolveNativeImportCaller(callback + 4).domain ==
             context.domain;
}
bool ValidPublicFunction(const GuestRuntimeCallExecutor& calls,
                         const NativeSystemUiPublicCallProfile& profile) {
  const auto& plan = calls.validated();
  const auto function = plan.catalog.find(std::string(profile.identity));
  const auto contract = plan.contracts.find(std::string(profile.identity));
  auto module = calls.module();
  if (function == plan.catalog.end() || contract == plan.contracts.end() ||
      function->second.address != profile.address ||
      function->second.arguments.size() != profile.argument_count ||
      function->second.return_kind != GuestRuntime::ReturnKind::kUInt32 ||
      contract->second.result !=
          ValidatedGuestRuntimePlan::ResultPolicy::kAny ||
      contract->second.caller !=
          ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem ||
      !contract->second.before.empty() || !contract->second.after.empty() ||
      module->GetProcAddressByOrdinal(uint16_t(profile.call)) !=
          profile.address) {
    return false;
  }
  for (const auto& phase : plan.plan.phases) {
    for (const auto& entry : phase.functions) {
      if (entry.identity == profile.identity) {
        return false;  // Public functions may never become heap initializers.
      }
    }
  }
  auto* memory = calls.kernel()->memory();
  if (memory->QueryRangeAccess(profile.address, profile.code_size) ==
      memory::PageAccess::kNoAccess) {
    return false;
  }
  for (uint32_t at = profile.address; at < profile.address + profile.code_size;
       at += 4) {
    xex2_section_type section;
    if (calls.kernel()->GetUserModuleForAddress(at).get() != module.get() ||
        !module->xex_module()->GetPageSectionType(at, &section) ||
        section != XEX_SECTION_CODE ||
        module->xex_module()->TranslateCode(at) !=
            memory->TranslateVirtual<const uint8_t*>(at)) {
      return false;
    }
  }
  std::array<uint8_t, 32> digest;
  mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(profile.address),
                 profile.code_size, digest.data(), 0);
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < digest.size(); ++i) {
    if (profile.code_sha256[i * 2] != hex[digest[i] >> 4] ||
        profile.code_sha256[i * 2 + 1] != hex[digest[i] & 15]) {
      return false;
    }
  }
  return true;
}
[[noreturn]] void FailStop() noexcept {
  try {
    XELOGE(
        "Native SYSTEM service: bootstrap/callback stop unproven; retaining "
        "process");
    xe::FlushLog();
  } catch (...) {
  }
  std::quick_exit(EXIT_FAILURE);
}
}  // namespace

GuestRuntime::ExecutionResult
NativeSystemUiPreparedTransport::ObserveNativePhase(
    NativeSystemUiStartupPhase, const NativeSystemUiServiceContext&, XThread&) {
  return Fail("Actual transport/native registration observations unavailable");
}
std::shared_ptr<void>
NativeSystemUiPreparedTransport::AcquireSharedGraphicsObservation(XThread&) {
  return {};
}
GuestRuntime::ExecutionResult
NativeSystemUiPreparedTransport::AttachSharedGraphics(
    const NativeSystemUiServiceContext&, const NativeSystemSharedGraphicsState&,
    const std::shared_ptr<NativeGraphicsDeviceBinding>&, XThread&) {
  return Fail("Actual shared graphics transport unavailable");
}
std::shared_ptr<void>
NativeSystemUiPreparedTransport::AcquirePublicCallOperation(
    NativeSystemUiPublicCall, XThread&) {
  return {};
}
NativeUiBackendRoute::Admission
NativeSystemUiPreparedTransport::AdmitUiCreating(bool) {
  return {};
}
std::shared_ptr<NativeUiBackendRoute::HostLease>
NativeSystemUiPreparedTransport::CurrentHostUi() {
  return {};
}
bool NativeSystemUiPreparedTransport::SealHostUiFallback() noexcept {
  return true;
}
bool NativeSystemUiPreparedTransport::AllowPhysicalInput() const noexcept {
  return false;
}
void NativeSystemUiPreparedTransport::CloseExternalRequests() noexcept {}
void NativeSystemUiPreparedTransport::BeginStopTransport() noexcept {}
bool NativeSystemUiPreparedTransport::DrainCallbacksBeforeCpuStop() noexcept {
  return false;
}
bool NativeSystemUiPreparedTransport::RetireAfterKernelStop() noexcept {
  return false;
}
std::shared_ptr<NativeSystemUiPreparedTransport>
NativeSystemUiServiceTransport::PrepareBeforeWorkers(
    const NativeSystemUiServiceContext&, XThread&) {
  return {};
}

struct NativeSystemUiService::State {
  explicit State(Config value) : config(std::move(value)) {}
  Config config;
  // Serializes Start/Resume with actual OS join, never acquired by bootstrap
  // or native callbacks. No metadata mutex crosses a guest call or wait.
  std::mutex lifecycle;
  std::mutex stop;
  mutable std::mutex mutex;
  std::atomic<bool> stopping{false};
  Snapshot status;
  struct KernelAdmission {
    std::atomic<bool> closed{false};
    std::weak_ptr<NativeSystemUiService> service;
  };
  std::shared_ptr<KernelAdmission> admission;
  gpu::GpuBackendStopProof backend_stop;
  NativeSystemSharedGraphicsState shared_graphics;
  object_ref<XHostThread> bootstrap;
  object_ref<UserModule> root;
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  std::shared_ptr<NativeNotificationPreferenceCoordinator> preferences;
  // Installed while the actual SYSTEM bootstrap is quiescent, before heap
  // initialization or workers can compile/use the native user exports.
  std::shared_ptr<NativeCurrentUserProviderBinding> current_users;
  std::shared_ptr<NativeNotificationPresentationObserver> notification_observer;
  std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
  std::shared_ptr<NativeImportDomain> domain;
  std::shared_ptr<NativeSystemCallbackDispatcher> dispatcher;
  std::shared_ptr<NativeInputDriver> input;
  std::unique_ptr<NativeInputDriver::Installation> input_installation;
  std::shared_ptr<NativeManagedFormatFrontdoor> format_frontdoor;
  std::shared_ptr<NativeContentStorageDriver> storage;
  std::unique_ptr<NativeContentStorageDriver::Installation>
      storage_installation;
  std::shared_ptr<NativeSystemTaskContinuation> continuation;
  std::shared_ptr<NativeSystemUiStartupRuntime> startup;
  // Retained even after partial/uncertain initialization. All native XMsg
  // users stop with this Kernel before these holders/Memory are reclaimed.
  std::shared_ptr<GuestRuntime> title_heap_runtime;
  object_ref<UserModule> prepared_title_module;
  std::shared_ptr<KernelState::UserModuleExecutionLease> prepared_title_lease;
  std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
  std::shared_ptr<NativeGraphicsDeviceBinding> graphics;
  std::shared_ptr<NativeGraphicsDeviceBinding> device_publication;
  // This is only the trusted host adapter's actual preworker bootstrap scope,
  // not a guest LR, native Task or execution privilege.
  bool preparing_driver_callbacks = false;
  bool retired = false;
  struct PublicInvocation {
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    std::shared_ptr<void> transport_operation;
    NativeImportDomain::Operation domain_operation;
    std::shared_ptr<KernelState::UserModuleExecutionLease> caller_code;
    object_ref<XThread> caller;
    std::shared_ptr<NativeUiBackendRoute::HostLease> host_input;
    bool host_input_blocked = false;
  };
  // Fixed, opt-in bootstrap diagnostic only. No callback or public admission.
  // Capture binds before real IO; Complete fills after Store locks return.
  struct PreferenceProbeReceipt {
    std::mutex mutex;
    uint32_t serial = 0, expected_or_bits = 0;
    XThread* caller = nullptr;
    cpu::ppc::PPCContext* context = nullptr;
    uint64_t domain_id = 0, root_generation = 0;
    uint32_t root_hmodule = 0;
    uint32_t captured = 0, completed = 0;
    bool interleaved = false, abandoned = false, actual_return = false;
    std::optional<NativeNotificationPreferenceCoordinator::
                      PersistenceObservation> observation;
  };
  std::shared_ptr<PreferenceProbeReceipt> preference_probe_receipt;
  bool preference_probe_claimed = false;
  bool notification_probe_claimed = false;
  // Published before the genuine Task Schedule/queue entry. Work also retains
  // this owner; uncertain execution never needs an allocate-on-failure tail.
  std::shared_ptr<void> notification_probe_owner;
  bool public_ever_admitted = false, public_uncertain = false;
  size_t public_calls = 0;
  std::condition_variable public_drained;
  // Reserve before guest execution. Failed/partial calls remain discoverable
  // with their C++/module/transport ownership; never allocate a quarantine
  // tail.
  std::vector<std::shared_ptr<PublicInvocation>> public_invocations;

  static std::mutex registry_mutex;
  static std::map<KernelState*, std::weak_ptr<KernelAdmission>> registry;
  // Caller holds registry_mutex. The Kernel-owned tombstone prevents a late
  // Create even if stop happened before the service existed. No PPC deleter.
  static std::shared_ptr<KernelAdmission> AdmissionForKernel(
      KernelState* kernel) {
    auto& slot = registry[kernel];
    auto admission = slot.lock();
    if (!admission) {
      admission = std::make_shared<KernelAdmission>();
      kernel->RetainNativeResourceUntilTeardown(admission);
      slot = admission;
    }
    return admission;
  }

  NativeSystemUiServiceContext Context() const {
    std::lock_guard lock(mutex);
    return {config.kernel, catalog, allocation,        domain,
            dispatcher,    input,   device_publication};
  }
  bool Poison(std::string reason) {
    XELOGE("Native SYSTEM service startup failed: {}", reason);
    std::shared_ptr<NativeSystemAllocationAuthority> owner;
    {
      std::lock_guard lock(mutex);
      if (status.failure.empty()) {
        status.failure = std::move(reason);
      }
      status.boot_admitted = status.source_frame_ready =
          status.title_heap_ready = false;
      owner = allocation;
    }
    if (owner) {
      owner->CloseAdmission();
    }
    return false;
  }
  bool IsStopping() const { return stopping.load(std::memory_order_acquire); }
  bool PreparedTitleIsCurrent() const {
    if (IsStopping() || !config.kernel || !config.prepared_title.valid()) {
      return false;
    }
    auto* emulator = config.kernel->emulator();
    if (!emulator) {
      return false;
    }
    const auto foreground = emulator->title_execution_state().snapshot();
    const auto title = config.kernel->GetExecutableModule();
    return emulator->OwnsTitleExecution(config.kernel) &&
           (foreground.phase == TitleExecutionState::Phase::kPreparing ||
            foreground.phase == TitleExecutionState::Phase::kRunning) &&
           foreground.hmodule == (title ? title->hmodule_ptr() : 0) &&
           SameNativeSystemPreparedTitle(
               config.prepared_title,
               {foreground.generation, title ? title->hmodule_ptr() : 0,
                title ? title->load_generation() : 0,
                foreground.main_thread_id});
  }
};
std::mutex NativeSystemUiService::State::registry_mutex;
std::map<KernelState*,
         std::weak_ptr<NativeSystemUiService::State::KernelAdmission>>
    NativeSystemUiService::State::registry;

struct NativeSystemUiService::NotificationPreferencePersistence::State {
  std::weak_ptr<NativeSystemUiService::State> service;
  std::shared_ptr<NativeSystemUiService::State::PublicInvocation> invocation;
  std::shared_ptr<NativeNotificationPreferenceCoordinator> preferences;
  cpu::ppc::PPCContext* context = nullptr;
  std::shared_ptr<NativeSystemUiService::State::PreferenceProbeReceipt> probe;
  bool admitted = false, completed = false;
};
NativeSystemUiService::NotificationPreferencePersistence::
    NotificationPreferencePersistence(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemUiService::NotificationPreferencePersistence::
    ~NotificationPreferencePersistence() {
  if (!state_ || !state_->admitted || state_->completed) {
    return;
  }
  state_->preferences->BeginStop();
  if (state_->probe) {
    std::lock_guard lock(state_->probe->mutex);
    state_->probe->abandoned = true;
  }
  if (const auto service = state_->service.lock()) {
    // Store IO did not supply a normally returned rich outcome. The preowned
    // invocation remains in the existing quarantine; no uncertainty allocation.
    std::lock_guard lock(service->mutex);
    service->public_uncertain = true;
    --service->public_calls;
    service->public_drained.notify_all();
  }
}
void NativeSystemUiService::NotificationPreferencePersistence::Complete(
    const ConsoleSettingsUpdate& update) noexcept {
  if (!state_ || !state_->admitted || state_->completed) {
    return;
  }
  const auto service = state_->service.lock();
  if (!service) {
    return;
  }
  NativeNotificationPreferenceCoordinator::PersistenceObservation observation;
  observation.update_observed = true;
  observation.update_error = update.error;
  observation.published = update.published;
  observation.durable = update.durable;
  const auto& caller = state_->invocation->caller;
  const bool actual_return =
      XThread::IsInThread(caller.get()) &&
      caller->thread_state()->context() == state_->context &&
      state_->context->kernel_state == state_->invocation->calls->kernel() &&
      uint32_t(state_->context->lr) == 0x816ABBB4 &&
      state_->invocation->calls->module()->load_generation() ==
          state_->invocation->calls->load_generation();
  if (!actual_return) {
    state_->preferences->BeginStop();
    service->Poison("Native preference IO returned on a different caller");
  }
  try {
    uint32_t flags;
    observation.read_error =
        global_critical_region::is_held_by_current_thread()
            ? std::make_error_code(std::errc::operation_not_permitted)
            : state_->invocation->calls->kernel()->xconfig()->ReadRetailFlags(
                  flags);
    if (!observation.read_error) {
      observation.retail_flags = flags;
    }
  } catch (...) {
    observation.read_error = std::make_error_code(std::errc::io_error);
  }
  if (state_->probe) {
    std::lock_guard lock(state_->probe->mutex);
    ++state_->probe->completed;
    state_->probe->actual_return = actual_return;
    state_->probe->observation = observation;
  }
  // Native SetOptions already changed C4/C8/CC/D0.
  // Failure/publication/durability remain distinct; never replace its void
  // result or roll back private cells.
  const bool coherent =
      state_->preferences->ObserveNativeSetterPersistence(observation);
  if (!coherent) {
    service->Poison("Native preference setter preceded owned handover");
  }
  try {
    XELOGI(
        "Native preference persistence observed: tid={} root={:08X}/{} "
        "error={} published={} durable={} read_error={} coherent={}",
        state_->invocation->caller->thread_id(),
        state_->invocation->calls->module()->hmodule_ptr(),
        state_->invocation->calls->load_generation(), update.error.value(),
        update.published, update.durable, observation.read_error.value(),
        coherent);
  } catch (...) {
  }
  {
    std::lock_guard lock(service->mutex);
    const auto found =
        std::find(service->public_invocations.begin(),
                  service->public_invocations.end(), state_->invocation);
    if (found != service->public_invocations.end()) {
      service->public_invocations.erase(found);
    }
  }
  // Release code/domain/caller leases before publishing the rundown count.
  state_->invocation.reset();
  state_->completed = true;
  {
    std::lock_guard lock(service->mutex);
    --service->public_calls;
    service->public_drained.notify_all();
  }
}

std::unique_ptr<NativeSystemUiService::NotificationPreferencePersistence>
NativeSystemUiService::CaptureNotificationPreferencePersistence(
    KernelState* kernel, cpu::ppc::PPCContext* context, uint16_t category,
    uint16_t setting, uint32_t and_mask, uint32_t or_bits) noexcept {
  std::shared_ptr<NativeSystemUiService> service;
  try {
    if (!kernel || !context || context->kernel_state != kernel ||
        category != 3 || setting != 0xC || and_mask != 0xFFFFBDCFu ||
        (or_bits & ~0x4230u) || uint32_t(context->lr) != 0x816ABBB4u ||
        !XThread::IsInThread()) {
      return {};
    }
    auto* current = XThread::GetCurrentThread();
    if (!current || current->kernel_state() != kernel ||
        current->thread_state()->context() != context) {
      return {};
    }
    {
      std::lock_guard lock(State::registry_mutex);
      const auto found = State::registry.find(kernel);
      const auto admission =
          found == State::registry.end() ? nullptr : found->second.lock();
      service = admission ? admission->service.lock() : nullptr;
    }
    if (!service) {
      return {};
    }
    auto state = service->state_;
    const auto cohort = state->Context();
    const auto* profile = NativeSystemUiPublicProfile(
        NativeSystemUiPublicCall::kNotifySetOptions);
    const auto caller =
        kernel->ResolveNativeImportCaller(uint32_t(context->lr));
    const auto process = current->native_frame_thread_identity().process_type;
    if (!cohort.catalog || !cohort.domain || !cohort.allocation ||
        !cohort.allocation->IsAccepting() ||
        !cohort.allocation->MatchesDomain(cohort.domain) ||
        caller.domain != cohort.domain ||
        caller.module.get() != cohort.catalog->calls()->module().get() ||
        (process != X_PROCTYPE_SYSTEM && process != X_PROCTYPE_TITLE)) {
      return {};
    }
    const auto close_unobserved = [&] {
      if (const auto preferences = kernel->notification_preferences()) {
        preferences->BeginStop();
      }
      state->Poison("Actual native preference setter outcome cannot be owned");
    };
    if (!profile || !ValidPublicFunction(*cohort.catalog->calls(), *profile)) {
      close_unobserved();
      return {};
    }
    bool import = false;
    for (const auto& library :
         *caller.module->xex_module()->import_libraries()) {
      if (library.name != "xboxkrnl") {
        continue;
      }
      for (const auto& item : library.imports) {
        import |= item.ordinal == 0x2BC && item.thunk_address == 0x81A72214;
      }
    }
    if (!import) {
      close_unobserved();
      return {};
    }
    auto invocation = std::make_shared<State::PublicInvocation>();
    invocation->calls = cohort.catalog->calls();
    invocation->domain_operation = cohort.domain->Acquire();
    invocation->caller_code =
        kernel->AcquireUserModuleExecutionLease(caller.module);
    invocation->caller = retain_object(current);
    auto receipt = std::make_unique<NotificationPreferencePersistence::State>();
    receipt->service = state;
    receipt->invocation = invocation;
    receipt->preferences = kernel->notification_preferences();
    receipt->context = context;
    if (!invocation->domain_operation || !invocation->caller_code ||
        !receipt->preferences) {
      close_unobserved();
      return {};
    }
    // Allocate the returned scope before admission. No allocation is needed if
    // an exception later forces retention of the already indexed invocation.
    auto result = std::unique_ptr<NotificationPreferencePersistence>(
        new NotificationPreferencePersistence(std::move(receipt)));
    bool admitted = false;
    {
      std::lock_guard lock(state->mutex);
      if (!state->IsStopping() && !state->retired && !state->public_uncertain &&
          state->public_calls < 64 && state->public_invocations.size() < 64) {
        state->public_invocations.push_back(invocation);
        ++state->public_calls;
        result->state_->admitted = admitted = true;
        if (const auto& probe = state->preference_probe_receipt) {
          // State->probe is the only nested lock order; Complete drops probe
          // before taking State for rundown. No guest/IO under either lock.
          std::lock_guard probe_lock(probe->mutex);
          if (probe->caller == current && probe->context == context &&
              probe->domain_id == cohort.domain->id() &&
              probe->root_hmodule == caller.module->hmodule_ptr() &&
              probe->root_generation == caller.module->load_generation() &&
              probe->expected_or_bits == or_bits) {
            ++probe->captured;
            result->state_->probe = probe;
          } else {
            probe->interleaved = true;
          }
        }
      }
    }
    if (!admitted) {
      close_unobserved();
      return {};
    }
    return result;
  } catch (...) {
    if (service && kernel) {
      if (const auto preferences = kernel->notification_preferences()) {
        preferences->BeginStop();
      }
      try {
        service->state_->Poison("Native preference attribution failed");
      } catch (...) {
      }
    }
    return {};
  }
}

// Native startup roles remain owned by the immutable startup runtime. This
// adapter supplies concrete system observations and the one native Task seam.
class NativeSystemUiService::StartupSeams final
    : public NativeSystemUiStartupSeams {
 public:
  explicit StartupSeams(std::shared_ptr<State> state)
      : state_(std::move(state)) {}
  GuestRuntime::ExecutionResult PrepareBeforeWorkers(
      const NativeSystemUiStartupCatalog&, XThread& thread) override {
    const auto context = state_->Context();
    if (state_->IsStopping() || !ActualSystem(thread, context.kernel) ||
        thread.thread_id() != context.allocation->bootstrap_thread_id()) {
      return Fail(
          "Service preworker admission closed or wrong actual bootstrap");
    }
    const auto before = context.catalog->ReadHardwareFlags();
    if (!before) {
      return Fail("Actual native hardware import unreadable");
    }
    {
      std::lock_guard lock(state_->mutex);
      state_->preparing_driver_callbacks = true;
    }
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    const auto diagnostic = state_->config.diagnostic;
    GuestRuntime::ExecutionResult observation_prepared;
    bool callbacks_registered = false;
    std::string input_error;
    try {
      // The original Main performs these Kernel registrations before TaskInit.
      // Observe ALL pristine data before the first callback may mutate it. No
      // Main interior execution, fabricated LR, Task/TLS or native table write.
      const auto profile =
          context.catalog->ReadInputCallbackBootstrap(thread, &input_error);
      callbacks_registered =
          profile && context.input &&
          context.input->SetCallback(NativeInputDriver::Callback::kUserBinding,
                                     profile->user_binding) &&
          context.input->SetCallback(NativeInputDriver::Callback::kDeviceConfig,
                                     profile->device_config) &&
          context.input->SetCallback(
              NativeInputDriver::Callback::kFailedConnectionOrBind,
              profile->failed_binding);
      if (callbacks_registered && diagnostic) {
        observation_prepared =
            diagnostic->PrepareBeforeWorkers(context, thread);
      } else if (callbacks_registered) {
        prepared =
            state_->config.transport->PrepareBeforeWorkers(context, thread);
      }
    } catch (...) {
      std::lock_guard lock(state_->mutex);
      state_->preparing_driver_callbacks = false;
      throw;
    }
    {
      std::lock_guard lock(state_->mutex);
      state_->preparing_driver_callbacks = false;
      state_->prepared = prepared;  // Retain before any later validation fails.
    }
    if (!callbacks_registered) {
      return Fail("Actual native physical input registrations/enumeration: " +
                  input_error);
    }
    if ((!diagnostic && !prepared) ||
        (diagnostic && !observation_prepared.succeeded)) {
      return Fail("Actual NoSwap/Vd/backend capability not installed");
    }
    if (state_->IsStopping()) {
      if (diagnostic) {
        diagnostic->CloseExternalRequests();
      } else {
        prepared->CloseExternalRequests();
      }
      return Fail("Service stopped during preworker preparation");
    }
    const auto stop = diagnostic ? diagnostic->backend_stop_proof()
                                 : prepared->backend_stop_proof();
    const auto after = context.catalog->ReadHardwareFlags();
    if (!stop.backend_epoch() || stop.stopped() || !after ||
        *after != (*before | 0x200u)) {
      return Fail("Actual backend/virtual hardware profile not proven");
    }
    {
      std::lock_guard lock(state_->mutex);
      state_->backend_stop = stop;
    }
    // Bind only this separate quiescent root's exact verified callback entry.
    auto continuation =
        NativeSystemTaskContinuation::Create(context.catalog, context.domain);
    if (!continuation) {
      return Fail("Verified native Task continuation bridge unavailable");
    }
    {
      std::lock_guard lock(state_->mutex);
      state_->continuation = std::move(continuation);
    }
    return Ok();
  }
  GuestRuntime::ExecutionResult ExecuteFrontendOnNativeTaskAndWait(
      const NativeSystemUiStartupCatalog&, XThread& bootstrap,
      FrontendWork work) override {
    std::shared_ptr<NativeSystemTaskContinuation> continuation;
    {
      std::lock_guard lock(state_->mutex);
      continuation = state_->continuation;
    }
    if (!continuation || state_->IsStopping()) {
      return Fail("Frontend continuation admission closed");
    }
    return continuation->InvokeAndWait(bootstrap, std::move(work));
  }
  GuestRuntime::ExecutionResult ObserveAfter(
      NativeSystemUiStartupPhase phase,
      const NativeSystemUiStartupCatalog& catalog, XThread& thread) override {
    const auto context = state_->Context();
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    {
      std::lock_guard lock(state_->mutex);
      prepared = state_->prepared;
    }
    const auto diagnostic = state_->config.diagnostic;
    if (state_->IsStopping() || (!prepared && !diagnostic) ||
        !ActualSystem(thread, context.kernel) ||
        !context.allocation->IsAccepting() ||
        !context.allocation->MatchesDomain(context.domain)) {
      return Fail("Service phase observer lost actual cohort admission");
    }
    // A new observation ID may never fall through to the diagnostic adapter's
    // logging-only success. Every admitted role needs a mandatory native check.
    switch (phase) {
      case NativeSystemUiStartupPhase::kTask:
      case NativeSystemUiStartupPhase::kSharedGraphics:
      case NativeSystemUiStartupPhase::kConfig:
      case NativeSystemUiStartupPhase::kNotifications:
      case NativeSystemUiStartupPhase::kInputAuxiliary:
      case NativeSystemUiStartupPhase::kUpdatePredicate:
      case NativeSystemUiStartupPhase::kFeRegistry:
      case NativeSystemUiStartupPhase::kUiManager:
      case NativeSystemUiStartupPhase::kSystemContentSource:
      case NativeSystemUiStartupPhase::kSystemContentIndex:
      case NativeSystemUiStartupPhase::kSystemContentLookup:
      case NativeSystemUiStartupPhase::kSystemStorageInitialized:
      case NativeSystemUiStartupPhase::kSystemStorageBootCompleted:
        break;
      default:
        return Fail("Startup role has no mandatory native postcondition");
    }
    const auto& symbols = catalog.symbols();
    auto* memory = context.kernel->memory();
    const auto readable_pointer = [&](uint32_t cell) {
      const auto pointer = Word(memory, cell);
      return pointer && *pointer && Word(memory, *pointer).has_value();
    };
    const auto observe_tasks = [&](auto cells) {
      std::set<uint32_t> handles;
      for (const auto cell : cells) {
        const auto handle = Word(memory, cell);
        if (!handle || *handle < 0x1000 || (*handle & 3) != 1) {
          TraceNativeTaskObservation(context, thread, phase, cell, handle,
                                     false, 1, nullptr);
          return false;
        }
        if (!Word(memory, *handle & ~1u)) {
          TraceNativeTaskObservation(context, thread, phase, cell, handle,
                                     false, 2, nullptr);
          return false;
        }
        if (!handles.insert(*handle).second) {
          TraceNativeTaskObservation(context, thread, phase, cell, handle, true,
                                     3, nullptr);
          return false;
        }
        const std::array<uint32_t, 1> args{*handle};
        const auto status =
            context.catalog->calls()->ExecuteDeferredSystemFunction(
                thread, "native-system-task-status", args);
        // The UInt32/Any native getter returns1 for Created state0,0 for
        // states2/40, and exact E_PENDING for active/queued states such as
        // Reschedule's1000. This establishes no completion/readiness promise.
        const bool known_status = status.return_value == 0 ||
                                  status.return_value == 1 ||
                                  status.return_value == X_E_PENDING;
        TraceNativeTaskObservation(context, thread, phase, cell, handle, true,
                                   !status.succeeded ? 4
                                   : known_status    ? 0
                                                     : 5,
                                   &status);
        if (!status.succeeded || !known_status) {
          return false;
        }
      }
      return true;
    };
    if (phase == NativeSystemUiStartupPhase::kSystemStorageInitialized ||
        phase == NativeSystemUiStartupPhase::kSystemStorageBootCompleted) {
      const auto content = catalog.system_content();
      const auto expected =
          phase == NativeSystemUiStartupPhase::kSystemStorageInitialized
              ? NativeSystemContent::Status::kStorageInitialized
              : NativeSystemContent::Status::kStorageBootCompleted;
      if (!content || content->snapshot().status != expected ||
          !observe_tasks(std::array<uint32_t, 2>{0x81A9CB28, 0x81AA279C})) {
        return Fail("Actual native storage workers/boot observation absent");
      }
    } else if (phase == NativeSystemUiStartupPhase::kSystemContentSource ||
               phase == NativeSystemUiStartupPhase::kSystemContentIndex ||
               phase == NativeSystemUiStartupPhase::kSystemContentLookup) {
      const auto content = catalog.system_content();
      if (!content) {
        return Fail("Actual native system-content family absent");
      }
      const auto observed = content->snapshot();
      using S = NativeSystemContent::Status;
      const auto expected =
          phase == NativeSystemUiStartupPhase::kSystemContentSource
              ? S::kSourceAttached
          : phase == NativeSystemUiStartupPhase::kSystemContentIndex
              ? S::kIndexInitialized
              : S::kLookupAndOpenVerified;
      if (observed.status != expected || !observed.source_device ||
          observed.manifest_sha256.empty() ||
          (expected == S::kLookupAndOpenVerified &&
           (observed.package_path.empty() || observed.toc_sha256.empty() ||
            !observed.toc_size))) {
        return Fail(
            "Actual native source/index/lookup/content-open witness absent");
      }
    } else if (phase == NativeSystemUiStartupPhase::kTask) {
      const auto slot = Word(memory, symbols.task_tls_slot_cell);
      // Actual Kernel KeTlsAlloc is limited to its 8x32-bit process bitmap.
      if (!slot || *slot >= 256) {
        return Fail("Native Task TLS slot was not actually initialized");
      }
      // TaskInit's six pool creates ignore failures. Require all eight actual
      // SYSTEM kernel workers and their native-written TLS roots, not its r3.
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      for (;;) {
        size_t workers = 0;
        for (const auto& worker :
             context.kernel->object_table()->GetObjectsByType<XThread>(
                 XObject::Type::Thread)) {
          if (worker->kernel_state() == context.kernel &&
              worker->start_address() == symbols.task_worker_entry &&
              worker->guest_object<X_KTHREAD>()->process_type ==
                  X_PROCTYPE_SYSTEM &&
              !worker->guest_object<X_KTHREAD>()->terminated &&
              !worker->thread()) {
            uint32_t root = 0;
            if (worker->GetTLSValue(*slot, &root) && root) {
              // Verified entry writes the actual worker at native TLSroot+0.
              const auto actual_worker = Word(memory, root);
              if (actual_worker && *actual_worker &&
                  Word(memory, *actual_worker).has_value()) {
                ++workers;
              }
            }
          }
        }
        if (workers == 8) {
          break;
        }
        if (workers > 8 || state_->IsStopping() ||
            std::chrono::steady_clock::now() >= deadline) {
          return Fail(
              "Actual eight native Task workers/TLS roots not established");
        }
        thread.Delay(0, 0, uint64_t(-10000ll));
      }
    } else if (phase == NativeSystemUiStartupPhase::kSharedGraphics) {
      const auto observed = catalog.ReadSharedGraphicsState();
      if (observed.status !=
              NativeSystemSharedGraphicsState::Status::kDcPending &&
          observed.status !=
              NativeSystemSharedGraphicsState::Status::kObserved) {
        return Fail("Actual cohort-owned shared device/context/buffers absent");
      }
      // Native resource locking selects the kernel global independently of
      // shared XUI's explicit device. Publish the real light device before
      // FE/UI callbacks, in BOTH production and CPU-only first-record mode.
      gpu::GpuBackendStopProof stop;
      {
        std::lock_guard lock(state_->mutex);
        if (state_->device_publication) {
          return Fail("Native device publication phase repeated");
        }
        stop = state_->backend_stop;
      }
      std::string error;
      auto publication = NativeGraphicsDeviceBinding::PublishForNativeSystem(
          context, thread, stop, &error);
      if (!publication) {
        return Fail("Early native shared device publication: " + error);
      }
      {
        std::lock_guard lock(state_->mutex);
        state_->device_publication = std::move(publication);
      }
      if (state_->IsStopping()) {
        return Fail("Native publication completed after stop admission closed");
      }
    } else if (phase == NativeSystemUiStartupPhase::kConfig) {
      if (!readable_pointer(symbols.configuration_object_cell)) {
        return Fail("Native configuration object was not published");
      }
    } else if (phase == NativeSystemUiStartupPhase::kNotifications) {
      std::string notification_error;
      if (!catalog.CheckNativeNotificationInitialization(&notification_error)) {
        return Fail("Actual native notification initialization incomplete: " +
                    notification_error);
      }
      if (!context.input ||
          !context.input->ObserveNativeNotificationsInitialized()) {
        return Fail("Actual native input notification adapter closed");
      }
    } else if (phase == NativeSystemUiStartupPhase::kInputAuxiliary) {
      if (!observe_tasks(symbols.input_task_cells)) {
        return Fail(
            "All four actual native input Task handles not established");
      }
      // Tasks are resident and may report E_PENDING. Readiness here concerns
      // the already-admitted initial physical callbacks, not Task completion,
      // TitleRunning, input ownership or a first rendered Source.
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      for (;;) {
        const auto input = context.input
                               ? context.input->initial_binding_snapshot()
                               : NativeInputDriver::InitialBindingSnapshot{};
        static xe::NativeSystemUiTraceLimiter trace;
        const uint64_t key = uint64_t(input.connected_mask) |
                             (uint64_t(input.bound_mask) << 8) |
                             (uint64_t(input.failed) << 16) |
                             (uint64_t(input.callbacks_registered) << 17) |
                             (uint64_t(input.enumeration_observed) << 18);
        trace.EmitTransition(
            key, "input", "initial_bindings_observed",
            {{"input_generation",
              context.input ? context.input->trace_input_generation() : 0},
             {"callbacks_registered", input.callbacks_registered},
             {"enumeration_observed", input.enumeration_observed},
             {"connected_mask", input.connected_mask},
             {"bound_mask", input.bound_mask},
             {"pending_callbacks", input.pending_callbacks},
             {"failed", input.failed},
             {"settled", input.ready()}});
        if (input.ready()) {
          break;
        }
        if (!input.accepting || input.failed || state_->IsStopping() ||
            std::chrono::steady_clock::now() >= deadline) {
          return Fail("Actual initial native device/user binding not settled");
        }
        thread.Delay(0, 0, uint64_t(-10000ll));
      }
    } else if (phase == NativeSystemUiStartupPhase::kFeRegistry) {
      if (!ObserveFeFields(context, bool(diagnostic))) {
        return Fail(
            "Actual FE descriptor/native XAM callback or HUD dependency "
            "ownership not established");
      }
    } else if (phase == NativeSystemUiStartupPhase::kUiManager) {
      const std::array<uint32_t, 2> cells{symbols.manager_task_cell,
                                          symbols.manager_startup_task_cell};
      if (!observe_tasks(cells)) {
        return Fail("Actual native manager Task ownership not established");
      }
    }
    // FE registration linkage and transitive native import/driver/kernel facts
    // are mandatory adapter observations, never a successful constructor stub.
    const auto after = state_->Context();
    return diagnostic ? diagnostic->ObserveNativePhase(phase, after, thread)
                      : prepared->ObserveNativePhase(phase, after, thread);
  }

 private:
  std::shared_ptr<State> state_;
};

NativeSystemUiService::NativeSystemUiService(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemUiService::~NativeSystemUiService() = default;

std::shared_ptr<NativeSystemUiService> NativeSystemUiService::Create(
    Config config) {
  if (!config.kernel || !GuestScheduler::enabled() ||
      config.native_root_guest_path.empty() ||
      config.validated_plan_path.empty() || !config.prepared_title.valid() ||
      (bool(config.transport) == bool(config.diagnostic)) ||
      XThread::IsInThread()) {
    return {};
  }
  auto state = std::make_shared<State>(std::move(config));
  auto service = std::shared_ptr<NativeSystemUiService>(
      new NativeSystemUiService(std::move(state)));
  {
    std::lock_guard lock(State::registry_mutex);
    auto admission = State::AdmissionForKernel(service->state_->config.kernel);
    if (admission->closed.load(std::memory_order_acquire) ||
        !admission->service.expired()) {
      return {};
    }
    service->state_->admission = admission;
    admission->service = service;  // Before any bootstrap ingress exists.
  }
  // External handles may outlive Kernel. Retire guest/code holders before
  // Memory independently of the last shared_ptr, including partial startup.
  struct KernelRetirement {
    explicit KernelRetirement(std::shared_ptr<NativeSystemUiService> service)
        : service(std::move(service)) {}
    ~KernelRetirement() {
      if (!service->RetireAfterKernelStop()) {
        FailStop();
      }
    }
    std::shared_ptr<NativeSystemUiService> service;
  };
  service->state_->config.kernel->RetainNativeResourceUntilTeardown(
      std::make_shared<KernelRetirement>(service));
  return service;
}

bool NativeSystemUiService::Start() {
  if (XThread::IsInThread()) {
    return false;
  }
  std::lock_guard lifecycle(state_->lifecycle);
  {
    std::lock_guard lock(state_->mutex);
    if (state_->IsStopping() || state_->status.start_claimed) {
      return false;
    }
    state_->status.start_claimed = true;
  }
  try {
    const std::weak_ptr<State> weak = state_;
    auto bootstrap = object_ref<XHostThread>(new XHostThread(
        state_->config.kernel, 1024 * 1024, X_CREATE_SUSPENDED,
        [weak] {
          const auto state = weak.lock();
          return state ? Bootstrap(state) : 1;
        },
        state_->config.kernel->GetSystemProcess()));
    bootstrap->set_name("Native SYSTEM service bootstrap");
    bootstrap->set_can_debugger_suspend(true);
    {
      std::lock_guard lock(state_->mutex);
      state_->bootstrap = bootstrap;
    }
    if (XFAILED(bootstrap->Create()) || XFAILED(bootstrap->Resume())) {
      return state_->Poison("Actual SYSTEM bootstrap creation/resume failed");
    }
    return true;
  } catch (...) {
    return state_->Poison("SYSTEM bootstrap creation threw");
  }
}

int NativeSystemUiService::Bootstrap(const std::shared_ptr<State>& state) {
  auto* thread = XThread::GetCurrentThread();
  const auto fail = [&](std::string text) {
    state->Poison(std::move(text));
    return 1;
  };
  if (!thread || !ActualSystem(*thread, state->config.kernel)) {
    return fail("Bootstrap is not its actual SYSTEM XHostThread");
  }
  try {
    if (state->IsStopping()) {
      return fail("Service stopped before native root load");
    }
    object_ref<UserModule> root;
    {
      // Preserve freshness atomically with Kernel's existing loader namespace.
      // No guest entry executes and no native worker exists during this load.
      auto lock = xe::global_critical_region::AcquireDirect();
      if (state->config.kernel->GetModule(state->config.native_root_guest_path,
                                          true)) {
        return fail("Native service refuses an already loaded root");
      }
      root = state->config.kernel->LoadUserModule(
          state->config.native_root_guest_path, false, true);
    }
    if (!root ||
        root.get() == state->config.kernel->GetExecutableModule().get() ||
        !root->is_system_app_module()) {
      return fail("Separate native root load failed");
    }
    // LoadUserModule only maps the XEX. Finish its loader metadata/imports and
    // code identity before validating the native plan, without DllMain or
    // compiling discovered callers before the verified bridges are installed.
    const auto loaded =
        state->config.kernel->FinishLoadingUserModule(root, false, false);
    if (XFAILED(loaded)) {
      return fail("Separate native root loader completion failed");
    }
    std::string error;
    auto calls = GuestRuntimeCallExecutor::Load(
        state->config.kernel, root, state->config.validated_plan_path, &error);
    if (!calls) {
      return fail("Validated native call plan: " + error);
    }
    auto allocation = NativeSystemAllocationAuthority::Reserve(
        state->config.kernel, root, calls->heap_runtime());
    if (!allocation) {
      return fail("Pre-heap native allocation authority reservation failed");
    }
    {
      std::lock_guard lock(state->mutex);
      state->root = root;
      state->calls = calls;
      state->allocation = allocation;
    }
    auto catalog =
        NativeSystemUiStartupCatalog::Load(calls, allocation, &error);
    if (!catalog || !calls->InstallTrustedBridges(&error)) {
      return fail("Native startup catalogue/bridges: " + error);
    }
    auto current_users =
        NativeCurrentUserProviderBinding::InstallBeforeHeapAndWorkers(calls,
                                                                    &error);
    if (!current_users || !current_users->bindings_complete() ||
        current_users->root_generation() != calls->load_generation()) {
      return fail("Pre-heap shared current user bindings: " + error);
    }
    {
      std::lock_guard lock(state->mutex);
      state->current_users = std::move(current_users);
    }
    if (state->IsStopping()) {
      return fail("Service stopped after current user binding");
    }
    const auto preferences = state->config.kernel->notification_preferences();
    if (!preferences || !preferences->BeginNativeTransition(allocation)) {
      return fail("Console preference authority/handover unavailable");
    }
    {
      std::lock_guard lock(state->mutex);
      state->preferences = preferences;
    }
    // No native root PPC has executed. Retain existing fallback operations
    // through actual console IO; never hold metadata/GCRC locks across Delay.
    const auto preference_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!preferences->FallbackDrained()) {
      if (state->IsStopping() ||
          std::chrono::steady_clock::now() >= preference_deadline) {
        return fail("Fallback preference IO did not drain before native PPC");
      }
      thread->Delay(0, 0, uint64_t(-10000ll));
    }
    if (state->IsStopping() || !preferences->MarkNativeMayRun()) {
      return fail("Native preference handover closed before first PPC");
    }
    const auto heap = calls->InitializeHeap(*thread, allocation);
    if (heap != GuestRuntime::InitStatus::kReady &&
        heap != GuestRuntime::InitStatus::kAlreadyReady) {
      return fail("Native once-only heap/bootstrap scopes failed");
    }
    {
      std::lock_guard lock(state->mutex);
      state->status.heap_ready = true;
      state->catalog = catalog;
    }
    if (state->IsStopping()) {
      return fail("Service stopped after heap initialization");
    }
    auto profile =
        CreateNativeSystemUiImportProvider(state->config.kernel, root, &error);
    auto domain =
        profile ? state->config.kernel->CreateNativeImportDomain(root, profile)
                : nullptr;
    if (!domain || !allocation->BindDomain(domain)) {
      return fail("Exact native allocator/import domain bind failed: " + error);
    }
    auto dispatcher =
        NativeSystemCallbackDispatcher::Create(state->config.kernel, domain);
    if (!dispatcher) {
      return fail("Actual SYSTEM callback dispatcher install failed");
    }
    {
      std::lock_guard lock(state->mutex);
      state->domain = domain;
      state->dispatcher = dispatcher;
    }
    // Code/root/domain ownership is established, but no native workers have
    // begun. This observes genuine native frame/clear paths; it grants no
    // public notification, presentation, preference or cue readiness.
    auto notification_observer =
        NativeNotificationPresentationObserver::Create({catalog, domain});
    if (!notification_observer) {
      return fail("Owned native nonmodal observer install failed");
    }
    {
      std::lock_guard lock(state->mutex);
      state->notification_observer = notification_observer;
    }
    if (state->IsStopping()) {
      notification_observer->BeginStop();
      return fail("Service stopped after nonmodal observer install");
    }
    const std::weak_ptr<State> weak = state;
    NativeInputDriver::Config input_config;
    input_config.kernel = state->config.kernel;
    input_config.generation = domain->id();
    input_config.dispatcher = dispatcher;
    input_config.authorize_current_caller = [weak] {
      const auto self = weak.lock();
      if (!self || !XThread::IsInThread()) {
        return false;
      }
      const auto context = self->Context();
      auto* current = XThread::GetCurrentThread();
      if (!context.allocation->IsAccepting() ||
          current->kernel_state() != context.kernel) {
        return false;
      }
      {
        std::lock_guard lock(self->mutex);
        if (self->preparing_driver_callbacks &&
            ActualSystem(*current, context.kernel) &&
            current->thread_id() == context.allocation->bootstrap_thread_id()) {
          return true;  // Trusted host adapter; no guest LR is fabricated.
        }
      }
      return context.kernel
                 ->ResolveNativeImportCaller(
                     uint32_t(current->thread_state()->context()->lr))
                 .domain == context.domain;
    };
    input_config.validate_callback = [weak](uint32_t address) {
      const auto self = weak.lock();
      if (!self || address > UINT32_MAX - 4 || (address & 3)) {
        return false;
      }
      const auto context = self->Context();
      return context.allocation->IsAccepting() &&
             context.kernel->ResolveNativeImportCaller(address + 4).domain ==
                 context.domain;
    };
    input_config.validate_device_context = [weak] {
      const auto self = weak.lock();
      if (!self || self->IsStopping()) {
        return false;
      }
      const auto context = self->Context();
      return context.catalog && context.allocation &&
             context.allocation->IsAccepting() &&
             context.catalog->CheckInputCallbackContext();
    };
    input_config.protocol_failed = [weak] {
      if (const auto self = weak.lock()) {
        self->Poison(
            "Native physical input callback/refresh protocol uncertain");
      }
    };
    input_config.allow_physical_input = [weak] {
      const auto self = weak.lock();
      if (!self || !self->PreparedTitleIsCurrent()) {
        return false;
      }
      std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
      {
        std::lock_guard lock(self->mutex);
        prepared = self->prepared;
      }
      return prepared && prepared->AllowPhysicalInput();
    };
    const auto authorize_kernel_driver = input_config.authorize_current_caller;
    auto input = NativeInputDriver::Create(std::move(input_config));
    auto installed = input ? NativeInputDriver::Install(input) : nullptr;
    if (!installed) {
      return fail("Retained native physical input driver install failed");
    }
    {
      std::lock_guard lock(state->mutex);
      state->input = input;
      state->input_installation = std::move(installed);
    }
    if (const auto content = catalog->system_content()) {
      NativeContentStorageDriver::Config storage_config;
      storage_config.kernel = state->config.kernel;
      storage_config.generation = domain->id();
      storage_config.dispatcher = dispatcher;
      storage_config.source = state->config.kernel->content_manager()
                                  ->sources()
                                  .storage_notifications();
      storage_config.authorize_current_caller = authorize_kernel_driver;
      storage_config.validate_context = [weak] {
        const auto self = weak.lock();
        if (!self) {
          return false;
        }
        const auto context = self->Context();
        const auto source =
            context.catalog ? context.catalog->system_content() : nullptr;
        return context.allocation && context.allocation->IsAccepting() &&
               source && source->CheckStorageDriverContext();
      };
      storage_config.validate_callback = [weak](uint32_t address) {
        const auto self = weak.lock();
        if (!self) {
          return false;
        }
        const auto context = self->Context();
        const auto source =
            context.catalog ? context.catalog->system_content() : nullptr;
        return context.allocation && context.allocation->IsAccepting() &&
               source && source->CheckStorageDriverContext(address);
      };
      storage_config.protocol_failed = [weak] {
        if (const auto self = weak.lock()) {
          self->Poison(
              "Native content-storage snapshot/callback protocol failed");
        }
      };
      auto storage =
          NativeContentStorageDriver::Create(std::move(storage_config));
      {
        std::lock_guard lock(state->mutex);
        state->storage = storage;
      }
      auto installation =
          storage ? NativeContentStorageDriver::Install(storage) : nullptr;
      if (!installation) {
        return fail("Retained native content-storage driver install failed");
      }
      std::lock_guard lock(state->mutex);
      state->storage_installation = std::move(installation);
    }
    if (cvars::native_system_ui_trace) {
      const auto owner = allocation->owner();
      static xe::NativeSystemUiTraceLimiter trace;
      trace.Emit("input", "input_transport_installed",
                 {{"kernel_epoch", owner.kernel_epoch},
                  {"runtime_module_generation", owner.module_generation},
                  {"root_hmodule", owner.module_hmodule},
                  {"root_load_generation", allocation->root_load_generation()},
                  {"domain_id", domain->id()},
                  {"input_generation", input->trace_input_generation()}});
    }
    // Optional explicit plan family only. No global TITLE XMsg routing and no
    // missing native processed-ID/selection producer is inferred from this.
    bool managed_format = false;
    for (const auto& bridge : calls->validated().bridges)
      managed_format |= bridge.service == "managed_format_owned_request";
    if (managed_format) {
      auto frontdoor = NativeManagedFormatFrontdoor::Create(
          {state->config.kernel, calls, domain, state->config.kernel->content_manager()});
      if (!frontdoor) return fail("Managed format exact plan/owner installation failed");
      { std::lock_guard lock(state->mutex); state->format_frontdoor = frontdoor; }
      if (state->IsStopping()) frontdoor->BeginStop();
    }
    auto startup = NativeSystemUiStartupRuntime::Create(catalog);
    if (!startup) {
      return fail("Separate once-only native startup transaction unavailable");
    }
    {
      std::lock_guard lock(state->mutex);
      state->startup = startup;
    }
    const auto status =
        startup->Initialize(*thread, std::make_shared<StartupSeams>(state));
    if ((status != NativeSystemUiStartupRuntime::Status::kBootAdmitted &&
         status !=
             NativeSystemUiStartupRuntime::Status::kAlreadyBootAdmitted) ||
        !startup->IsBootAdmitted() || state->IsStopping()) {
      const auto failed = startup->snapshot();
      for (const auto* phase : {&failed.early, &failed.frontend}) {
        if (phase->failure) {
          XELOGE(
              "Native SYSTEM startup transaction: functions={}/{} "
              "return={:08X} detail={}",
              phase->completed_functions, phase->total_functions,
              phase->failure->return_value, phase->failure->detail);
        }
      }
      return fail("Native synchronous service boot was not admitted");
    }
    // Native CRT deliberately leaves heap0 disabled. Its genuine per-Title
    // initializer belongs here, after native boot and before original public
    // calls/diagnostic Guide or the prepared Main can execute. Never attach it
    // lazily to2C1, change allocation flags, or fabricate native Task context.
    const auto expected_title = state->config.prepared_title;
    auto prepared_title = state->config.kernel->GetExecutableModule();
    auto title_lease =
        prepared_title ? state->config.kernel->AcquireUserModuleExecutionLease(
                             prepared_title)
                       : nullptr;
    if (!expected_title.valid() || !prepared_title || !title_lease ||
        prepared_title->hmodule_ptr() != expected_title.hmodule ||
        prepared_title->load_generation() != expected_title.load_generation) {
      return fail("Actual prepared Title heap lifetime unavailable");
    }
    auto title_runtime = std::make_shared<GuestRuntime>(
        calls->heap_runtime()->GetSnapshot().identity, calls);
    bool title_claimed = false;
    {
      std::lock_guard lock(state->mutex);
      if (!state->IsStopping() && !state->title_heap_runtime) {
        // Publish ownership/once record BEFORE the first possible guest call.
        state->prepared_title_module = std::move(prepared_title);
        state->prepared_title_lease = std::move(title_lease);
        state->title_heap_runtime = title_runtime;
        title_claimed = true;
      }
    }
    if (!title_claimed) {
      return fail("Native Title heap generation already claimed or stopped");
    }
    const auto title_heap = title_runtime->Initialize(
        catalog->PreparedTitleHeapPlan(expected_title),
        [&](const GuestRuntime::Function&) {
          if (state->IsStopping()) {
            return Fail("Title heap initialization stopped before entry");
          }
          return catalog->InitializePreparedTitleHeap(*thread, expected_title);
        });
    if (title_heap != GuestRuntime::InitStatus::kReady || state->IsStopping()) {
      // Preserve the once record's actual failure. An absent post-call trace
      // is not proof that PPC was never entered. Diagnostic errors must not
      // change the original failure/poison path or retry the native call.
      try {
        const auto failed = title_runtime->GetSnapshot();
        if (failed.failure) {
          XELOGE(
              "Native prepared Title heap transaction: init_status={} "
              "state={} functions={}/{} stopping={} phase={} function={} "
              "return={:08X} detail={}",
              uint32_t(title_heap), uint32_t(failed.state),
              failed.completed_functions, failed.total_functions,
              state->IsStopping(), failed.failure->position.phase,
              failed.failure->position.function, failed.failure->return_value,
              failed.failure->detail);
        } else {
          XELOGE(
              "Native prepared Title heap transaction: init_status={} "
              "state={} functions={}/{} stopping={} failure_detail_known=0",
              uint32_t(title_heap), uint32_t(failed.state),
              failed.completed_functions, failed.total_functions,
              state->IsStopping());
        }
      } catch (...) {
      }
      return fail("Native prepared Title heap initialization failed/uncertain");
    }
    {
      std::lock_guard lock(state->mutex);
      state->status.title_heap_ready = true;
    }
    if (!ObserveInitialNotificationPreferences(state, *thread)) {
      return fail("Native preference control observation failed");
    }
    if (state->config.diagnostic) {
      {
        std::lock_guard lock(state->mutex);
        state->status.diagnostic_boot_observed = true;
      }
      // The native runtime's synchronous boot was actually observed, but the
      // diagnostic transport is NOT production Prepared or Guide admission.
      // The original actual SYSTEM bootstrap may now observe the first record.
      const auto observed = state->config.diagnostic->ObserveAfterNativeBoot(
          state->Context(), *thread);
      if (!observed.succeeded && !state->IsStopping()) {
        return fail("Native first-record observation: " + observed.detail);
      }
      return 0;
    }
    {
      std::lock_guard lock(state->mutex);
      state->status.boot_admitted = true;
      state->public_ever_admitted = true;
    }
    // DC publication belongs to the actual async native manager startup.
    // BootAdmitted is already visible; this metadata observer waits without
    // requiring a Guide request or first frame, until actual DC or stop. The
    // entire real host thread is joined before any provider/CPU shutdown.
    while (!state->IsStopping()) {
      if (ObserveSharedGraphics(state, *thread, false)) {
        break;
      }
      {
        std::lock_guard lock(state->mutex);
        if (!state->status.failure.empty()) {
          return 1;
        }
      }
      thread->Delay(0, 0, uint64_t(-160000ll));
    }
    if (!state->IsStopping() &&
        !cvars::native_notification_render_probe_report.empty()) {
      // Boot/actual shared binding is already published. The pre-title host
      // coordinator may resume Main while this actual SYSTEM bootstrap waits
      // for the game's own completed Title-input publication; no deadlock on
      // a first notification Source and no fabricated ready flag.
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(12);
      bool input_ready = false;
      while (!state->IsStopping()) {
        const auto current = state->Context();
        auto* emulator = current.kernel->emulator();
        auto* graphics = emulator->graphics_system();
        auto* cp = graphics ? graphics->command_processor() : nullptr;
        const auto foreground = emulator->title_execution_state().snapshot();
        const auto title = current.kernel->GetExecutableModule();
        const auto capacity = cp ? cp->GetNativeSystemFrameCapacity()
                                 : gpu::NativeSystemFrameCapacity{};
        const auto input = cp && foreground.phase == TitleExecutionState::Phase::kRunning
                               ? cp->GetNativeSystemTitleInput(foreground.generation)
                               : nullptr;
        input_ready = state->PreparedTitleIsCurrent() && title && input &&
            input->valid() && emulator->OwnsTitleExecution(current.kernel) &&
            input->identity().foreground_generation == foreground.generation &&
            input->identity().title_hmodule == title->hmodule_ptr() &&
            input->identity().title_load_generation == title->load_generation() &&
            input->identity().backend_epoch == capacity.backend_epoch &&
            capacity.supports_immutable_input_admission() &&
            !capacity.backend_stop_proof.stopped();
        if (input_ready || std::chrono::steady_clock::now() >= deadline) break;
        thread->Delay(0, 0, uint64_t(-160000ll));
      }
      if (state->IsStopping()) return 0;
      if (!input_ready) return fail("Render diagnostic real Title input unavailable");
      const auto observed = InvokeFixedNotificationDiagnostic(
          state->config.kernel, *thread);
      if (!observed.succeeded && !state->IsStopping())
        return fail("Production notification diagnostic: " + observed.detail);
    }
    return 0;
  } catch (...) {
    return fail("Native SYSTEM bootstrap threw; once-only cohort poisoned");
  }
}

bool NativeSystemUiService::ObserveInitialNotificationPreferences(
    const std::shared_ptr<State>& state, XThread& thread) {
  using Coordinator = NativeNotificationPreferenceCoordinator;
  using Readback = NativeNotificationPreferenceReadback;
  const auto context = state->Context();
  const auto* profile =
      NativeSystemUiPublicProfile(NativeSystemUiPublicCall::kNotifyGetOptions);
  if (!profile || !context.catalog || !context.domain || !context.allocation ||
      !state->preferences || !ActualSystem(thread, context.kernel) ||
      thread.thread_id() != context.allocation->bootstrap_thread_id() ||
      !context.allocation->MatchesDomain(context.domain)) {
    return false;
  }
  if (!ValidPublicFunction(*context.catalog->calls(), *profile)) {
    // The accepted older Guide plan need not catalogue notification controls.
    // Native may already cache preferences, so fallback never reopens here.
    XELOGI(
        "Native preference projection unavailable: exact293 not catalogued; "
        "notification family remains closed");
    return true;
  }
  struct Lifetime {
    Memory* memory = nullptr;
    uint32_t output = 0;
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
    NativeImportDomain::Operation domain;
    std::shared_ptr<KernelState::UserModuleExecutionLease> code;
    object_ref<XThread> caller;
    ~Lifetime() {
      if (output) {
        memory->SystemHeapFree(output);
      }
    }
  };
  auto owner = std::make_shared<Lifetime>();
  owner->memory = context.kernel->memory();
  owner->calls = context.catalog->calls();
  owner->allocation = context.allocation;
  owner->domain = context.domain->Acquire();
  owner->code =
      context.kernel->AcquireUserModuleExecutionLease(owner->calls->module());
  owner->caller = retain_object(&thread);
  owner->output = owner->memory->SystemHeapAlloc(Readback::kBytes);
  if (!owner->domain || !owner->code || !owner->output) {
    return false;
  }
  auto operation = state->preferences->TryAcquireNative(
      Coordinator::NativeCall::kGet, owner);
  if (!operation || state->IsStopping()) {
    return false;
  }
  Readback::Result observed;
  unsigned attempts = 0;
  bool entered = false;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (state->IsStopping() || !context.allocation->IsAccepting() ||
        !context.domain->active() ||
        !ValidPublicFunction(*owner->calls, *profile)) {
      break;
    }
    const auto initialized =
        Readback::Prepare(*owner->memory, owner->output, attempt);
    if (XFAILED(initialized)) {
      observed = {Readback::Kind::kInvalid, initialized, {}};
      break;
    }
    if (!entered) {
      if (!operation->MarkEntering()) {
        return false;
      }
      entered = true;
    }
    const auto arguments = Readback::Arguments(owner->output);
    // Exact293-only internal control admission, independent of presentation.
    // This is the actual retained SYSTEM bootstrap, not a host UI turn, native
    // Task substitution, arbitrary callback or enabling the queue family.
    const auto called = owner->calls->ExecuteDeferredSystemFunction(
        thread, profile->identity, arguments);
    ++attempts;
    if (!called.succeeded) {
      // Operation abandonment retains this preowned argument/code/domain/caller
      // record in coordinator quarantine through actual Kernel stop.
      return false;
    }
    observed = Readback::Read(*owner->memory, owner->output, attempt);
    if (observed.kind != Readback::Kind::kAmbiguous) {
      break;
    }
  }
  if (!entered) {
    operation->CancelBeforeEntry();
    XELOGE("Native preference getter denied before entry: status={:08X}",
           observed.status);
    return false;
  }
  Coordinator::PersistenceObservation persistence;
  uint32_t flags;
  try {
    persistence.read_error = context.kernel->xconfig()->ReadRetailFlags(flags);
    if (!persistence.read_error) {
      persistence.retail_flags = flags;
    }
  } catch (...) {
    persistence.read_error = std::make_error_code(std::errc::io_error);
  }
  const bool complete =
      observed.kind == Readback::Kind::kWritten
          ? operation->CompleteReadback(observed.options, persistence)
          : operation->CompleteWithoutReadback(persistence);
  const auto projection = state->preferences->ObservedProjection();
  XELOGI(
      "Native notification preference readback: calls={} kind={} "
      "status={:08X} options={:08X}/{:08X}/{:08X}/{:08X} "
      "console_read_error={} cache_projection_observed={} "
      "console_flags_present={} console_flags={:08X} console_flags_match={} "
      "queue_family_enabled={}",
      attempts, uint32_t(observed.kind), observed.status, observed.options[0],
      observed.options[1], observed.options[2], observed.options[3],
      persistence.read_error.value(), bool(projection),
      persistence.retail_flags.has_value(),
      persistence.retail_flags.value_or(0),
      projection && projection->observed_flags_match,
      NativeSystemPublicFamilyRouteEnabled(
          NativeSystemPublicFamily::kNotifications));
  // This only establishes (or explicitly withholds) an observed cache
  // projection. No phase Ready, presentation, native cue or durable-save claim.
  if (!complete || cvars::native_notification_preferences_probe_report.empty()) {
    return complete;
  }
  // A fixed diagnostic sequence only, on this already-owned bootstrap. The
  // production public family stays closed and normal launches take no branch.
  operation.reset();
  std::unique_ptr<filesystem::FileHandle> report;
  size_t report_bytes = 0;
  const auto emit = [&](std::string_view name, bool passed,
                        std::string fields = {}) {
    const auto line = fmt::format(
        "{{\"event\":\"native_preferences_set_probe\",\"case\":\"{}\","
        "\"passed\":{}{}{} }}\n", name, passed,
        fields.empty() ? "" : ",", fields);
    if (!report || line.size() > 65536 - report_bytes) {
      throw std::runtime_error("Bounded native preference report unavailable");
    }
    const auto written = report->WriteResult(
        report_bytes, {reinterpret_cast<const uint8_t*>(line.data()),
                       line.size()});
    if (written.error || written.bytes != line.size()) {
      throw std::runtime_error("Native preference diagnostic report write");
    }
    report_bytes += line.size();
  };
  try {
    const auto& report_path = cvars::native_notification_preferences_probe_report;
    if (!cvars::native_system_first_record_probe || !state->config.diagnostic ||
        state->prepared || state->IsStopping() ||
        observed.kind != Readback::Kind::kWritten ||
        !projection || !projection->observed_flags_match ||
        !persistence.retail_flags || persistence.read_error ||
        *persistence.retail_flags != 0xA5A5DEADu ||
        NativeSystemPublicFamilyRouteEnabled(
            NativeSystemPublicFamily::kNotifications) ||
        (!report_path.is_absolute() && report_path.has_parent_path()) ||
        report_path.filename() != "native-preferences-set.jsonl") {
      return false;
    }
    std::error_code storage_error;
    const auto storage = filesystem::HostStorageRoot::Open(
        context.kernel->emulator()->storage_root(), false, storage_error);
    if (!storage || storage_error) {
      return false;
    }
    if (report_path.is_absolute() &&
        (!std::filesystem::equivalent(report_path.parent_path(), storage->path(),
                                      storage_error) || storage_error)) {
      return false;
    }
    std::vector<uint8_t> marker;
    constexpr std::string_view marker_text =
        "Xenia native preference diagnostic\ndisposable=1\nversion=1\n";
    if (storage->ReadFile("native-preferences-disposable.v1", 128, marker) ||
        marker.size() != marker_text.size() ||
        !std::equal(marker.begin(), marker.end(), marker_text.begin())) {
      return false;
    }
    report = storage->OpenFile(
        report_path.filename(), filesystem::FileAccess::kFileWriteData,
        storage_error, true);
    if (!report || storage_error) {
      return false;
    }
    {
      std::lock_guard lock(state->mutex);
      if (state->preference_probe_claimed || state->preference_probe_receipt ||
          state->IsStopping()) {
        return false;
      }
      state->preference_probe_claimed = true;
    }
    const auto* set_profile =
        NativeSystemUiPublicProfile(NativeSystemUiPublicCall::kNotifySetOptions);
    if (!set_profile || !ValidPublicFunction(*owner->calls, *set_profile) ||
        !ValidPublicFunction(*owner->calls, *profile)) {
      emit("admission", false, "\"reason\":\"exact_profiles\"");
      return false;
    }
    const uint32_t initial_flags = *persistence.retail_flags;
    const Coordinator::Options initial_options = observed.options;
    const std::array<Coordinator::Options, 4> inputs{{
        initial_options, {0, 0x80000000u, 0xFFFFFFFFu, 0},
        {0, 1, 0x10203040u, 0}, initial_options}};
    constexpr std::array<const char*, 4> labels{
        "same_current", "changed_raw", "same_enabled_raw", "restore_initial"};
    constexpr std::array<bool, 4> expected_published{false, true, false, true};
    emit("context", true,
         fmt::format("\"thread_id\":{},\"domain_id\":{},\"root_hmodule\":{},"
                     "\"root_generation\":{},\"initial_flags\":{},"
                     "\"guest_bytes\":48,\"queue_family_enabled\":false,"
                     "\"production_ready\":false,\"scenario_count\":4",
                     thread.thread_id(), context.domain->id(),
                     owner->calls->module()->hmodule_ptr(),
                     owner->calls->load_generation(), initial_flags));
    bool all_passed = true;
    uint32_t completed_cases = 0;
    for (size_t index = 0; index < inputs.size(); ++index) {
      if (state->IsStopping() || !context.allocation->IsAccepting() ||
          !context.domain->active() || !ActualSystem(thread, context.kernel) ||
          thread.thread_id() != context.allocation->bootstrap_thread_id() ||
          !context.allocation->MatchesDomain(context.domain) ||
          !ValidPublicFunction(*owner->calls, *set_profile) ||
          !ValidPublicFunction(*owner->calls, *profile)) {
        emit("all_implemented_cases", false,
             "\"reason\":\"closed_before_entry\",\"restored\":false");
        return state->IsStopping();
      }
      auto probe_operation = state->preferences->TryAcquireNative(
          Coordinator::NativeCall::kSetThenReadback, owner);
      if (!probe_operation) {
        emit("all_implemented_cases", false,
             "\"reason\":\"coordinator_denied\",\"restored\":false");
        return false;
      }
      // NativeOperation abandonment means execution may still be uncertain,
      // not merely that a host observation/report failed. Track that boundary
      // around each real call; known normal returns must drain without output.
      struct NormalReturnScope {
        Coordinator::NativeOperation* operation;
        bool entered = false, in_flight = false, normally_returned = false;
        bool completed = false;
        Coordinator::PersistenceObservation best;
        explicit NormalReturnScope(Coordinator::NativeOperation* value)
            : operation(value) {
          // Missing attribution is unknown, never fabricated update success.
          best.read_error = std::make_error_code(std::errc::io_error);
        }
        ~NormalReturnScope() noexcept {
          if (entered && normally_returned && !in_flight && !completed) {
            try {
              if (!operation->CompleteWithoutReadback(best)) {
                FailStop();  // Rundown invariant failed; do not relabel PPC.
              }
            } catch (...) {
              FailStop();
            }
          }
        }
        void Calling() { in_flight = true; }
        void Returned() { in_flight = false; normally_returned = true; }
      } normal_return{probe_operation.get()};
      const auto& input = inputs[index];
      const uint32_t expected_flags =
          (initial_flags & ~Coordinator::kOptionMask) |
          Coordinator::DisabledOptionBits(input);
      auto receipt = std::make_shared<State::PreferenceProbeReceipt>();
      receipt->serial = uint32_t(index + 1);
      receipt->expected_or_bits = Coordinator::DisabledOptionBits(input);
      receipt->caller = &thread;
      receipt->context = thread.thread_state()->context();
      receipt->domain_id = context.domain->id();
      receipt->root_hmodule = owner->calls->module()->hmodule_ptr();
      receipt->root_generation = owner->calls->load_generation();
      struct ReceiptScope {
        std::shared_ptr<State> state;
        std::shared_ptr<State::PreferenceProbeReceipt> receipt;
        ~ReceiptScope() {
          std::lock_guard lock(state->mutex);
          if (state->preference_probe_receipt == receipt) {
            state->preference_probe_receipt.reset();
          }
        }
      } receipt_scope{state, receipt};
      {
        std::lock_guard lock(state->mutex);
        if (state->IsStopping() || state->preference_probe_receipt) {
          probe_operation->CancelBeforeEntry();
          return state->IsStopping();
        }
        state->preference_probe_receipt = receipt;
      }
      if (XFAILED(Readback::Prepare(*owner->memory, owner->output, 0)) ||
          !probe_operation->MarkEntering()) {
        probe_operation->CancelBeforeEntry();
        emit("all_implemented_cases", false,
             "\"reason\":\"denied_before_entry\",\"restored\":false");
        return state->IsStopping();
      }
      normal_return.entered = true;
      normal_return.Calling();
      const auto set = owner->calls->ExecuteDeferredSystemFunction(
          thread, set_profile->identity, input);
      if (!set.succeeded) {
        emit(labels[index], false,
             "\"reason\":\"native_execution_uncertain\","
             "\"restored\":false");
        // Abandonment retains this preowned code/domain/caller/output through
        // actual Kernel stop. Never replay a possibly applied native setter.
        return false;
      }
      normal_return.Returned();
      Coordinator::PersistenceObservation actual_persistence;
      bool receipt_valid;
      {
        std::lock_guard lock(receipt->mutex);
        receipt_valid = receipt->captured == 1 && receipt->completed == 1 &&
                        !receipt->interleaved && !receipt->abandoned &&
                        receipt->actual_return && receipt->observation;
        if (receipt->observation) {
          actual_persistence = *receipt->observation;
          if (receipt->captured == 1 && receipt->completed == 1 &&
              !receipt->abandoned && receipt->actual_return) {
            normal_return.best = actual_persistence;
          }
        }
      }
      if (!receipt_valid) {
        emit(labels[index], false,
             "\"reason\":\"missing_or_interleaved_actual_receipt\","
             "\"restored\":false");
        return false;
      }
      if (state->IsStopping()) {
        const bool returned =
            probe_operation->CompleteWithoutReadback(actual_persistence);
        normal_return.completed = returned;
        emit("all_implemented_cases", false,
             "\"reason\":\"stopped_after_normal_set_return\","
             "\"restored\":false");
        return returned;
      }
      Readback::Result result;
      unsigned get_attempts = 0;
      for (unsigned attempt = 0; attempt < 2; ++attempt) {
        if (state->IsStopping() || !context.allocation->IsAccepting() ||
            !context.domain->active() ||
            !ValidPublicFunction(*owner->calls, *profile)) {
          normal_return.completed =
              probe_operation->CompleteWithoutReadback(actual_persistence);
          emit("all_implemented_cases", false,
               "\"reason\":\"closed_before_readback\",\"restored\":false");
          return state->IsStopping();
        }
        const auto prepared =
            Readback::Prepare(*owner->memory, owner->output, attempt);
        if (XFAILED(prepared)) {
          normal_return.completed =
              probe_operation->CompleteWithoutReadback(actual_persistence);
          emit(labels[index], false,
               "\"reason\":\"readback_prepare_failed\",\"restored\":false");
          return true;  // SET returned; diagnostic fails without fake replay.
        }
        const auto arguments = Readback::Arguments(owner->output);
        normal_return.Calling();
        const auto got = owner->calls->ExecuteDeferredSystemFunction(
            thread, profile->identity, arguments);
        ++get_attempts;
        if (!got.succeeded) {
          emit(labels[index], false,
               "\"reason\":\"getter_execution_uncertain\",\"restored\":false");
          return false;
        }
        normal_return.Returned();
        result = Readback::Read(*owner->memory, owner->output, attempt);
        if (result.kind != Readback::Kind::kAmbiguous) {
          break;
        }
      }
      // Keep this exact SET receipt, not snapshot.latest_persistence from a
      // different call. A later Store read is a separate non-atomic observation.
      const auto receipt_persistence = actual_persistence;
      uint32_t flags = 0;
      actual_persistence.retail_flags.reset();
      actual_persistence.read_error =
          context.kernel->xconfig()->ReadRetailFlags(flags);
      if (!actual_persistence.read_error) {
        actual_persistence.retail_flags = flags;
      }
      const bool read_written = result.kind == Readback::Kind::kWritten;
      const bool completed = read_written
          ? probe_operation->CompleteReadback(result.options, actual_persistence)
          : probe_operation->CompleteWithoutReadback(actual_persistence);
      normal_return.completed = completed;
      bool interleaved;
      uint32_t captured_count, completed_count;
      {
        // Seal the observation window in the same State->receipt order used by
        // Capture, so a previously admitted receipt cannot register afterward.
        std::lock_guard lock(state->mutex);
        std::lock_guard receipt_lock(receipt->mutex);
        captured_count = receipt->captured;
        completed_count = receipt->completed;
        interleaved = receipt->interleaved || captured_count != 1 ||
                      completed_count != 1 || receipt->abandoned;
        state->preference_probe_receipt.reset();
      }
      const bool passed = completed && read_written && result.options == input &&
          !interleaved && receipt_persistence.update_observed &&
          !receipt_persistence.update_error &&
          receipt_persistence.published == expected_published[index] &&
          receipt_persistence.durable && !receipt_persistence.read_error &&
          receipt_persistence.retail_flags == expected_flags &&
          !actual_persistence.read_error &&
          actual_persistence.retail_flags == expected_flags &&
          !NativeSystemPublicFamilyRouteEnabled(
              NativeSystemPublicFamily::kNotifications);
      emit(labels[index], passed,
           fmt::format("\"serial\":{},\"input\":[{},{},{},{}],"
                       "\"output\":[{},{},{},{}],\"getter_attempts\":{},"
                       "\"readback_kind\":{},\"readback_status\":{},"
                       "\"expected_flags\":{},\"receipt_error\":{},"
                       "\"receipt_error_category\":\"{}\","
                       "\"published\":{},\"durable\":{},"
                       "\"receipt_read_error\":{},\"receipt_flags_present\":{},"
                       "\"receipt_flags\":{},\"read_error\":{},"
                       "\"flags_present\":{},\"flags\":{},"
                       "\"receipt_captured\":{},\"receipt_completed\":{},"
                       "\"receipt_interleaved\":{},\"set_returned_normally\":true,"
                       "\"getter_returned_normally\":true,"
                       "\"native_return_used_as_status\":false,"
                       "\"queue_family_enabled\":false",
                       receipt->serial, input[0], input[1], input[2], input[3],
                       result.options[0], result.options[1], result.options[2],
                       result.options[3], get_attempts, uint32_t(result.kind),
                       result.status, expected_flags,
                       receipt_persistence.update_error.value(),
                       receipt_persistence.update_error.category().name(),
                       receipt_persistence.published, receipt_persistence.durable,
                       receipt_persistence.read_error.value(),
                       receipt_persistence.retail_flags.has_value(),
                       receipt_persistence.retail_flags.value_or(0),
                       actual_persistence.read_error.value(),
                       actual_persistence.retail_flags.has_value(),
                       actual_persistence.retail_flags.value_or(0),
                       captured_count, completed_count, interleaved));
      ++completed_cases;
      all_passed = all_passed && passed;
      if (!passed) {
        break;  // Known truthful failure: no automatic retry or live rollback.
      }
    }
    emit("all_implemented_cases", all_passed,
         fmt::format("\"restored\":{},\"scenario_count\":4,"
                     "\"completed_cases\":{},"
                     "\"fault_injection_implemented\":false,"
                     "\"restart_verified\":false,\"cancel_stop_verified\":false,"
                     "\"public_title_caller_verified\":false,"
                     "\"queue_family_enabled\":false,\"production_ready\":false",
                     all_passed, completed_cases));
    if (const auto error = report->FlushResult()) {
      XELOGE("Native preference diagnostic report flush failed: {}",
             error.message());
      return false;
    }
    return true;  // Actual calls returned; report carries scenario pass/failure.
  } catch (...) {
    XELOGE("Native preference diagnostic failed; isolated evidence preserved");
    return false;
  }
}

GuestRuntime::ExecutionResult
NativeSystemUiService::InvokeFixedNotificationDiagnostic(
    KernelState* kernel, XThread& bootstrap) {
  using Readback = NativeNotificationPreferenceReadback;
  using Coordinator = NativeNotificationPreferenceCoordinator;
  const bool render = !cvars::native_notification_render_probe_report.empty();
  const bool exact_source_diagnostic =
      render && cvars::native_system_ui_output_capture_source == 256;
  if (render && ((cvars::native_system_ui_output_capture_source != 0 &&
                  !exact_source_diagnostic) ||
                 !cvars::native_system_ui_output_capture_base.empty() ||
                 cvars::native_system_ui_output_capture_b_down))
    return Fail("Render probe explicit source mode/capture conflict");
  const auto& path = render ? cvars::native_notification_render_probe_report
                            : cvars::native_notification_queue_probe_report;
  if (!kernel || !ActualSystem(bootstrap, kernel) || path.empty() ||
      (render ? (!cvars::native_system_ui || cvars::native_system_first_record_probe ||
                 !cvars::native_notification_queue_probe_report.empty())
              : !cvars::native_system_first_record_probe) ||
      !cvars::native_notification_preferences_probe_report.empty() ||
      NativeSystemPublicFamilyRouteEnabled(
          NativeSystemPublicFamily::kNotifications) ||
      (!path.is_absolute() && path.has_parent_path()) ||
      path.filename() != (render ? "native-notification-render.jsonl"
                                : "native-notification-queue.jsonl")) {
    return Fail("Fixed native queue diagnostic admission/filename");
  }
  std::shared_ptr<NativeSystemUiService> service;
  {
    std::lock_guard lock(State::registry_mutex);
    const auto found = State::registry.find(kernel);
    const auto admission =
        found == State::registry.end() ? nullptr : found->second.lock();
    service = admission ? admission->service.lock() : nullptr;
  }
  if (!service) return Fail("Fixed native queue has no actual service");
  const auto state = service->state_;
  const auto context = state->Context();
  const auto probe_transport = render ? NativeSystemGraphicsTransport::Lookup(kernel)
                                      : nullptr;
  std::shared_ptr<NativeSystemTaskContinuation> continuation;
  {
    std::lock_guard lock(state->mutex);
    if (state->IsStopping() || state->retired ||
        (render ? (!state->prepared || !probe_transport ||
                   state->config.transport.get() != probe_transport.get() ||
                   state->config.diagnostic || !state->status.boot_admitted ||
                   !state->status.source_frame_ready ||
                   state->config.validated_plan_path != cvars::native_system_ui_startup_plan)
                : (state->prepared || !state->config.diagnostic ||
                   !state->status.diagnostic_boot_observed)) ||
        !state->status.title_heap_ready || state->notification_probe_claimed ||
        !state->continuation || !state->preferences || !context.catalog ||
        !context.domain || !context.domain->active() || !context.allocation ||
        !context.allocation->IsAccepting() ||
        !context.allocation->MatchesDomain(context.domain) ||
        bootstrap.thread_id() != context.allocation->bootstrap_thread_id()) {
      return Fail("Fixed native queue actual cohort/once-only admission");
    }
    state->notification_probe_claimed = true;
    continuation = state->continuation;
  }
  const auto* queue =
      NativeSystemUiPublicProfile(NativeSystemUiPublicCall::kNotifyQueue);
  const auto* get =
      NativeSystemUiPublicProfile(NativeSystemUiPublicCall::kNotifyGetOptions);
  if (!queue || !get || queue->argument_count != 5 ||
      queue->uint64_register_mask != 0x14 ||
      !ValidPublicFunction(*context.catalog->calls(), *queue) ||
      !ValidPublicFunction(*context.catalog->calls(), *get)) {
    return Fail("Fixed native queue exact290/293 profiles missing");
  }
  std::error_code error;
  auto storage = filesystem::HostStorageRoot::Open(
      kernel->emulator()->storage_root(), false, error,
      filesystem::kHostStorageReserve);
  if (!storage || error) return Fail("Fixed native queue disposable root");
  if (path.is_absolute() &&
      (!std::filesystem::equivalent(path.parent_path(), storage->path(), error) ||
       error)) return Fail("Fixed native queue report outside isolated root");
  std::vector<uint8_t> marker;
  constexpr std::string_view required_marker =
      "Xenia native notification diagnostic\ndisposable=1\nversion=1\n";
  if (storage->ReadFile("native-notification-disposable.v1", 128, marker) ||
      marker.size() != required_marker.size() ||
      !std::equal(marker.begin(), marker.end(), required_marker.begin())) {
    return Fail("Fixed native queue requires explicit disposable marker");
  }
  auto report = storage->OpenFile(
      path.filename(), filesystem::FileAccess::kFileWriteData, error, true);
  if (!report || error) return Fail("Fixed native queue exclusive report");
  // This diagnostic observes the existing queue only. Space checks and all
  // detached artifact writes are outside native/client/SDL/queue locks.
  const auto cue_observer = render && cvars::native_system_ui_trace
      ? NativeNotificationPresentationObserver::Lookup(kernel) : nullptr;
  const auto cue_space = cue_observer ? storage->Observe() : filesystem::HostStorageSnapshot{};
  const bool cue_space_ready = cue_space.available && !cue_space.read_only &&
      cue_space.usable_bytes >= apu::NativeAudioObservation::kMaximumPcmBytes + 512 * 1024;
  struct CueWindow {
    std::shared_ptr<NativeNotificationPresentationObserver> observer;
    ~CueWindow() { if (observer) { observer->CancelNativeCueObservation(); } }
  } cue_window{cue_observer};
  size_t report_bytes = 0;
  const auto emit = [&](std::string_view name, bool passed,
                        std::string fields = {}) {
    const auto line = fmt::format(
        "{{\"event\":\"{}\",\"case\":\"{}\","
        "\"passed\":{}{}{} }}\n",
        exact_source_diagnostic ? "native_notification_render_source_diagnostic"
        : render                ? "native_notification_render_probe"
                                : "native_notification_queue_probe",
        name, passed, fields.empty() && !exact_source_diagnostic ? "" : ",",
        exact_source_diagnostic
            ? fmt::format(
                  "\"diagnostic\":true,\"kind\":\"exact_source_256\","
                  "\"requested_source\":256,\"technical_acceptance\":false,"
                  "\"feature_acceptance\":false{}{}",
                  fields.empty() ? "" : ",", fields)
            : fields);
    const size_t limit = render ? 256 * 1024 : 16384;
    if (line.size() > limit - report_bytes)
      throw std::runtime_error("Fixed native queue bounded report");
    const auto written = report->WriteResult(
        report_bytes, {reinterpret_cast<const uint8_t*>(line.data()),
                       line.size()});
    if (written.error || written.bytes != line.size())
      throw std::runtime_error("Fixed native queue report write");
    report_bytes += line.size();
  };
  struct Lifetime {
    Memory* memory = nullptr;
    uint32_t allocation = 0;
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeSystemAllocationAuthority> authority;
    NativeImportDomain::Operation domain;
    std::shared_ptr<KernelState::UserModuleExecutionLease> code;
    object_ref<XThread> bootstrap, worker;
    uint64_t foreground = 0, xuid = 0;
    uint32_t worker_id = 0, options_calls = 0, queue_value = 0;
    bool options_written = false, queue_entered = false, queue_returned = false;
    bool cue_armed = false;
    Coordinator::Options options{};
    ~Lifetime() {
      if (allocation) memory->SystemHeapFree(allocation);
    }
  };
  auto owner = std::make_shared<Lifetime>();
  owner->memory = kernel->memory();
  owner->calls = context.catalog->calls();
  owner->authority = context.allocation;
  owner->domain = context.domain->Acquire();
  owner->code = kernel->AcquireUserModuleExecutionLease(owner->calls->module());
  owner->bootstrap = retain_object(&bootstrap);
  const auto foreground = kernel->emulator()->title_execution_state().snapshot();
  if (!owner->domain || !owner->code ||
      foreground.phase != TitleExecutionState::Phase::kRunning ||
      !kernel->emulator()->OwnsTitleExecution(kernel) ||
      !state->PreparedTitleIsCurrent()) {
    emit("admission", false, "\"reason\":\"foreground_or_code\"");
    return Fail("Fixed native queue foreground/code ownership");
  }
  owner->foreground = foreground.generation;
  {
    auto lock = global_critical_region::AcquireDirect();
    const auto* user = kernel->xam_state()->GetUserProfile(uint32_t(0));
    if (user && user->signin_state()) owner->xuid = user->xuid();
  }
  if (!owner->xuid) {
    emit("admission", false, "\"reason\":\"slot0_not_assigned\"");
    return Fail("Fixed native queue requires actual signed-in slot0");
  }
  constexpr std::u16string_view text = u"0|Native notification diagnostic";
  constexpr uint32_t text_offset = 64;
  constexpr uint32_t guest_bytes = 160;
  static_assert(text_offset + (text.size() + 1) * 2 <= guest_bytes);
  owner->allocation = owner->memory->SystemHeapAlloc(guest_bytes, 16);
  if (!owner->allocation) return Fail("Fixed native queue guest allocation");
  {
    auto lock = global_critical_region::AcquireDirect();
    for (size_t i = 0; i <= text.size(); ++i) {
      xe::store_and_swap<uint16_t>(
          owner->memory->TranslateVirtual(owner->allocation + text_offset +
                                         uint32_t(i * 2)),
          i == text.size() ? 0 : uint16_t(text[i]));
    }
  }
  {
    std::lock_guard lock(state->mutex);
    if (state->IsStopping()) return Fail("Fixed queue stopped before ownership");
    state->notification_probe_owner = owner;
  }
  emit("context", true,
       fmt::format("\"bootstrap_thread\":{},\"domain_id\":{},"
                   "\"root_generation\":{},\"foreground_generation\":{},"
                   "\"slot\":0,\"xuid\":\"{:016X}\",\"guest_bytes\":{},"
                   "\"native_family_enabled\":false",
                   bootstrap.thread_id(), context.domain->id(),
                   context.domain->root().generation, owner->foreground,
                   owner->xuid, guest_bytes));
  if (render) {
    if (!probe_transport->ArmNotificationRenderProbe(
            bootstrap, storage->path() / "native-notification-render-output",
            exact_source_diagnostic
                ? NativeSystemGraphicsTransport::NotificationRenderProbeMode::
                      kExactSource256Diagnostic
                : NativeSystemGraphicsTransport::NotificationRenderProbeMode::
                      kFirstEligible))
      return Fail("Render diagnostic pre-Schedule arm refused");
    const auto arm = probe_transport->PollNotificationRenderProbe(bootstrap);
    if (!arm) return Fail("Render diagnostic pre-Schedule observation refused");
    const auto actual = service->snapshot();
    bool prepared_present = false, diagnostic_present = false, configured_transport = false;
    {
      std::lock_guard lock(state->mutex);
      prepared_present = bool(state->prepared);
      diagnostic_present = bool(state->config.diagnostic);
      configured_transport = bool(state->config.transport);
    }
    const auto transport = probe_transport->snapshot();
    const auto& accepted = context.catalog->calls()->validated();
    const auto allocation_owner = context.allocation->owner();
    if (!allocation_owner.valid() || allocation_owner.module_hmodule !=
            context.catalog->calls()->module()->hmodule_ptr())
      return Fail("Render diagnostic actual runtime owner/root mismatch");
    std::array<uint8_t, 32> identity_hash;
    mbedtls_sha256(reinterpret_cast<const uint8_t*>(accepted.plan.identity.data()),
                   accepted.plan.identity.size(), identity_hash.data(), 0);
    std::string identity_hex;
    for (const auto byte : identity_hash) identity_hex += fmt::format("{:02x}", byte);
    emit("render_arm", true,
         fmt::format(
             "\"baseline_source\":{},\"before_schedule\":true,"
             "\"backend_epoch\":{},\"title_input_swap\":{},"
             "\"native_system_ui\":{},\"first_record_probe\":{},"
             "\"transport_mode\":{},\"transport_ready\":{},"
             "\"prepared_present\":{},\"diagnostic_present\":{},\"configured_"
             "transport\":{},"
             "\"boot_admitted\":{},\"title_heap_ready\":{},\"source_frame_"
             "ready\":{},"
             "\"kernel_epoch\":{},\"runtime_module_generation\":{},\"owner_"
             "session_id\":{},"
             "\"root_hmodule\":{},\"root_load_generation\":{},"
             "\"accepted_plan_identity_sha256\":\"{}\",\"accepted_catalog_"
             "size\":{},"
             "\"capture_file_budget_bytes\":{},\"retained_memory_budget_"
             "bytes\":{},"
             "\"run_copy_peak_preflight_required\":true,\"paint_fields_are_"
             "historical\":true",
             arm->baseline_source, arm->backend_epoch, arm->title_input_swap,
             cvars::native_system_ui, cvars::native_system_first_record_probe,
             uint32_t(transport.mode), transport.ready, prepared_present,
             diagnostic_present, configured_transport, actual.boot_admitted,
             actual.title_heap_ready, actual.source_frame_ready,
             allocation_owner.kernel_epoch, allocation_owner.module_generation,
             allocation_owner.session_id, allocation_owner.module_hmodule,
             context.catalog->calls()->load_generation(), identity_hex,
             accepted.catalog.size(),
             (exact_source_diagnostic ? 64 : 16) * 1024 * 1024,
             64 * 1024 * 1024));
  }
  // Work is bound into the preowned continuation invocation before Schedule.
  // The continuation validates the actual current native Task equals its own
  // scheduled handle before this callback. No null-Task queue readiness poll.
  const auto task = continuation->InvokePostBootDiagnosticAndWait(
      bootstrap, [state, context, owner, queue, get, render,
                  probe_transport, cue_observer, cue_space_ready](XThread& worker) {
        bool in_flight = false;
        std::unique_ptr<Coordinator::NativeOperation> options;
        bool getter_returned = false;
        try {
          const auto current =
              context.kernel->emulator()->title_execution_state().snapshot();
          if (state->IsStopping() || !ActualSystem(worker, context.kernel) ||
              !context.domain->active() ||
              !context.allocation->IsAccepting() ||
              !state->PreparedTitleIsCurrent() ||
              current.phase != TitleExecutionState::Phase::kRunning ||
              current.generation != owner->foreground ||
              !ValidPublicFunction(*owner->calls, *queue) ||
              !ValidPublicFunction(*owner->calls, *get)) {
            return Fail("Fixed queue worker lost actual ownership before entry");
          }
          {
            auto lock = global_critical_region::AcquireDirect();
            const auto* user =
                context.kernel->xam_state()->GetUserProfile(uint32_t(0));
            if (!user || !user->signin_state() || user->xuid() != owner->xuid)
              return Fail("Fixed queue slot0 assignment changed before entry");
          }
          owner->worker = retain_object(&worker);
          owner->worker_id = worker.thread_id();
          options = state->preferences->TryAcquireNative(
              Coordinator::NativeCall::kGet, owner);
          if (!options) return Fail("Fixed queue genuine options owner unavailable");
          Readback::Result observed;
          for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto initialized = Readback::Prepare(
                *owner->memory, owner->allocation, attempt);
            if (XFAILED(initialized)) {
              if (getter_returned) options->CompleteWithoutReadback({});
              else options->CancelBeforeEntry();
              return Fail("Fixed queue options readback preparation");
            }
            if (!getter_returned && !options->MarkEntering())
              return Fail("Fixed queue options cancelled before entry");
            in_flight = true;
            const auto got = owner->calls->ExecuteDeferredSystemFunction(
                worker, get->identity, Readback::Arguments(owner->allocation));
            if (!got.succeeded)
              throw std::runtime_error("Fixed queue getter execution uncertain");
            in_flight = false;
            getter_returned = true;
            ++owner->options_calls;
            observed = Readback::Read(*owner->memory, owner->allocation, attempt);
            if (observed.kind != Readback::Kind::kAmbiguous) break;
          }
          if (observed.kind != Readback::Kind::kWritten) {
            options->CompleteWithoutReadback({});
            return Fail("Fixed queue genuine options writes unavailable");
          }
          owner->options = observed.options;
          owner->options_written = true;
          // Actual cache readback is authoritative here; this does not claim a
          // Store match/durable mutation or roll cache back after a void setter.
          if (!options->CompleteReadback(observed.options, {}))
            return Fail("Fixed queue actual options completion refused");
          options.reset();
          // A single private UI-only request. Every value fits DWORD; the exact
          //290 profile still pins the two public qword register positions.
          const std::array<uint32_t, 5> args{
              0x1B, 0, 0, owner->allocation + text_offset, 0};
          const auto before_queue =
              context.kernel->emulator()->title_execution_state().snapshot();
          if (state->IsStopping() || !context.allocation->IsAccepting() ||
              !context.domain->active() || !state->PreparedTitleIsCurrent() ||
              before_queue.phase != TitleExecutionState::Phase::kRunning ||
              before_queue.generation != owner->foreground)
            return Fail("Fixed queue lost ownership after options before entry");
          {
            auto lock = global_critical_region::AcquireDirect();
            const auto* user =
                context.kernel->xam_state()->GetUserProfile(uint32_t(0));
            if (!user || !user->signin_state() || user->xuid() != owner->xuid)
              return Fail("Fixed queue slot0 changed after options before entry");
          }
          if (render && !probe_transport->ObserveNotificationRenderProbeQueue(worker, false))
            return Fail("Render diagnostic queue-entry observation refused");
          if (cue_observer && cue_space_ready) {
            owner->cue_armed = cue_observer->ArmNativeCueObservation(
                worker, owner->options[2] != 0);
          }
          owner->queue_entered = true;
          in_flight = true;
          const auto queued = owner->calls->ExecuteDeferredSystemFunction(
              worker, queue->identity, args);
          if (!queued.succeeded)
            throw std::runtime_error("Fixed queue execution uncertain");
          in_flight = false;
          owner->queue_returned = true;
          owner->queue_value = uint32_t(queued.return_value);
          if (render && !probe_transport->ObserveNotificationRenderProbeQueue(worker, true))
            return Fail("Render diagnostic normal-return observation refused");
          // kAny profile: a negative native queue HRESULT is a known normal
          // rejection, not execution uncertainty. Preserve it in the receipt.
          return Ok();
        } catch (...) {
          if (in_flight) throw;  // Existing Task owner quarantines all captures.
          if (options && getter_returned)
            options->CompleteWithoutReadback({});
          else if (options) options->CancelBeforeEntry();
          return Fail("Fixed queue normal observation failure");
        }
      });
  const auto& executed = task.execution;
  if (!task.completed_native_task) {
    // No normal Wait+Close proof: the real worker may remain in flight. Never
    // race its mutable receipt fields; all captures remain in the owner.
    emit("native_queue", false,
         "\"task_transport_completed\":false,"
         "\"worker_receipt_unavailable\":true,"
         "\"fallback_started\":false,"
         "\"visual_or_audible_acceptance\":false");
    if (report->FlushResult())
      return Fail("Fixed queue failed report durability");
    return Fail("Fixed native queue Task execution failed/uncertain");
  }
  // A nonzero completed handle proves real Wait+Close even when Work failed.
  // Only this branch may inspect the now-stable worker receipt fields.
  const bool accepted = executed.succeeded && owner->queue_returned &&
                        int32_t(owner->queue_value) >= 0;
  emit("native_queue", accepted,
       fmt::format("\"task_transport_completed\":true,\"work_succeeded\":{},"
                   "\"native_task\":{},\"worker_thread\":{},"
                   "\"options_written\":{},\"options_calls\":{},"
                   "\"options\":{},\"sound_enabled\":{},"
                   "\"queue_entered\":{},\"normal_return\":{},"
                   "\"raw_result\":{},\"queue_disposition\":\"{}\","
                   "\"fallback_started\":false,"
                   "\"visual_or_audible_acceptance\":false",
                   executed.succeeded, task.completed_native_task,
                   owner->worker_id ? std::to_string(owner->worker_id) : "null",
                   owner->options_written, owner->options_calls,
                   owner->options_written
                       ? fmt::format("[{},{},{},{}]", owner->options[0],
                                     owner->options[1], owner->options[2],
                                     owner->options[3]) : "null",
                   !owner->options_written ? "null"
                       : owner->options[2] ? "true" : "false",
                   owner->queue_entered, owner->queue_returned,
                   owner->queue_returned
                       ? std::to_string(owner->queue_value) : "null",
                   !owner->queue_returned ? "not_returned"
                   : int32_t(owner->queue_value) < 0 ? "rejected"
                   : owner->queue_value == 1 ? "suppressed"
                                            : "returned_or_coalesced"));
  const auto synced = report->FlushResult();
  if (synced) return Fail("Fixed queue diagnostic report durability");
  const auto finish_cue = [&]() {
    if (!cue_observer) { return; }
    auto result = cue_observer && owner->cue_armed
        ? cue_observer->CloseNativeCueObservation(bootstrap) : nullptr;
    if (!result || !result->audio) {
      emit("cue_observation", false, fmt::format(
          "\"armed\":{},\"space_ready\":{},\"detached_result\":false,"
          "\"cue_verified\":false,\"physical_audibility_proven\":false",
          owner->cue_armed, cue_space_ready));
      return;
    }
    // No native/runtime borrow remains: the result owns only copied metadata
    // and the moved finite PCM allocation. All file IO is detached here.
    const auto& audio = *result->audio;
    const auto& s = audio.summary;
    const auto hash = [](std::span<const uint8_t> bytes) {
      std::array<uint8_t, 32> digest{};
      mbedtls_sha256(bytes.data(), bytes.size(), digest.data(), 0);
      std::string value;
      value.reserve(64);
      for (uint8_t b : digest) { value += fmt::format("{:02x}", b); }
      return value;
    };
    const auto words = [](const auto& values) {
      std::string text = "[";
      for (size_t i = 0; i < values.size(); ++i) {
        if (i) { text += ','; }
        text += std::to_string(values[i]);
      }
      return text + ']';
    };
    std::string filename;
    for (size_t i = 0; i < result->filename.count; ++i) {
      filename += fmt::format("{:04x}", result->filename.units[i]);
    }
    std::error_code artifact_error;
    auto metadata = storage->OpenFile("native-notification-cue-v1.jsonl",
        filesystem::FileAccess::kFileWriteData, artifact_error, true);
    auto native = artifact_error ? nullptr : storage->OpenFile(
        "native-notification-cue-native-v1.pcm", filesystem::FileAccess::kFileWriteData,
        artifact_error, true);
    auto post = artifact_error ? nullptr : storage->OpenFile(
        "native-notification-cue-post-v1.pcm", filesystem::FileAccess::kFileWriteData,
        artifact_error, true);
    uint64_t native_offset = 0, post_offset = 0, metadata_offset = 0;
    bool written = metadata && native && post && !artifact_error &&
        audio.frame_count <= apu::NativeAudioObservation::kMaximumFrames &&
        (!audio.frame_count || audio.frames);
    const auto write_metadata = [&](std::string text) {
      if (!written || text.size() > 512 * 1024 - metadata_offset) { written = false; return; }
      const auto output = metadata->WriteResult(metadata_offset,
          {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
      written = !output.error && output.bytes == text.size();
      if (written) { metadata_offset += text.size(); }
    };
    write_metadata(fmt::format(
        "{{\"schema\":1,\"event\":\"native_cue_owned_observation\","
        "\"domain\":{},\"root_generation\":{},\"foreground\":{},"
        "\"owner_epoch\":{},\"registration_generation\":{},\"owner_hmodule\":{},"
        "\"client_index\":{},\"callback\":{},\"callback_arg\":{},"
        "\"engine\":{},\"driver\":{},\"publication_serial\":{},"
        "\"sound_enabled\":{},\"request_seen\":{},\"control\":{},\"hxui\":{},"
        "\"control_serial\":{},\"request_serial\":{},\"start_serial\":{},"
        "\"voice\":{},\"cache\":{},\"suppression\":{},"
        "\"voice_created\":{},\"buffer_submitted\":{},\"start_returned\":{},"
        "\"start_accepted\":{},\"stop_returned\":{},\"stop_accepted\":{},"
        "\"detached\":{},\"cleanup_returned\":{},\"release_returned_raw_only\":{},"
        "\"class_destroyed\":{},\"engine_retiring\":{},\"metadata_lost\":{},"
        "\"events_overflow\":{},\"multiple_requests\":{},"
        "\"filename_utf16be_hex\":\"{}\",\"filename_terminated\":{},"
        "\"filename_read_failed\":{},\"backend_kind\":{},"
        "\"backend_open_at_attach\":{},\"backend_resumed_at_attach\":{},"
        "\"backend_failed_at_attach\":{},\"frequency\":{},\"output_channels\":{},"
        "\"baseline_sequence\":{},\"last_sequence\":{},\"queued_frames\":{},"
        "\"sink_frames\":{},\"accepted_frames\":{},\"pending_frames\":{},"
        "\"closed\":{},\"capped\":{},\"canceled\":{},\"retired\":{},"
        "\"failed\":{},\"unsupported\":{},\"sequence_gap\":{},"
        "\"native_encoding\":\"float32_bits_big_endian_planar_6x256\","
        "\"post_encoding\":\"float32_native_endian_interleaved_256x_channels\","
        "\"post_little_endian\":{},"
        "\"mixed_output_ambiguity\":true,\"per_voice_isolation\":false,"
        "\"queue_to_cue_causality_proven\":false,\"cue_verified\":false,"
        "\"physical_audibility_proven\":false}}\n",
        result->domain, result->root_generation, result->foreground,
        s.identity.owner_epoch, s.identity.generation, s.identity.owner_hmodule,
        result->registration->index(), result->registration->callback(), result->registration->callback_arg(),
        result->engine, result->driver, result->publication_serial,
        result->sound_enabled, result->request_seen, result->control, result->hxui,
        result->control_serial, result->request_serial, result->start_serial,
        result->voice, result->cache, result->suppression,
        result->voice_created, result->buffer_submitted, result->start_returned,
        result->start_accepted, result->stop_returned, result->stop_accepted,
        result->detached, result->cleanup_returned, result->release_returned,
        result->class_destroyed, result->engine_retiring, result->metadata_lost,
        result->events_overflow, result->multiple_requests, filename,
        result->filename.terminated, result->filename.read_failed, uint32_t(s.backend.kind),
        s.backend.open, s.backend.resumed, s.backend.failed, s.backend.frequency, s.backend.output_channels,
        s.baseline_sequence, s.last_sequence, s.queued_frames, s.sink_frames,
        s.accepted_frames, s.pending_frames, s.closed, s.capped, s.canceled,
        s.retired, s.failed, s.unsupported, s.sequence_gap,
        std::endian::native == std::endian::little));
    for (uint32_t i = 0; written && i < result->event_count; ++i) {
      const auto& event = result->events[i];
      const auto& call = event.call;
      const auto& id = call.identity;
      write_metadata(fmt::format(
          "{{\"event\":\"normal_return\",\"index\":{},\"function\":{},"
          "\"thread\":{},\"pcr\":{},\"sp\":{},\"lr\":{},\"arguments\":{},"
          "\"audio_sequence_at_entry\":{},\"queued_frames_at_entry\":{},\"sink_frames_at_entry\":{},"
          "\"raw_result\":{},\"instance_readable\":{},\"instance_words\":{},"
          "\"message_readable\":{},\"message_words\":{},\"buffer_readable\":{},"
          "\"buffer_words\":{}}}\n",
          i, id.function, id.thread, id.pcr, id.sp, id.lr, words(call.arguments),
          call.audio_sequence_at_entry, call.queued_frames_at_entry, call.sink_frames_at_entry,
          event.result, call.instance.readable, words(call.instance.words),
          call.message.readable, words(call.message.words), call.buffer.readable, words(call.buffer.words)));
    }
    bool error_recorded = false;
    for (uint32_t i = 0; written && i < audio.frame_count; ++i) {
      const auto& frame = audio.frames[i];
      if (frame.native_bytes > frame.native_pcm.size() || frame.output_bytes > frame.output_pcm.size() ||
          native_offset + post_offset + frame.native_bytes + frame.output_bytes >
              apu::NativeAudioObservation::kMaximumPcmBytes) { written = false; break; }
      const std::span<const uint8_t> raw(frame.native_pcm.data(), frame.native_bytes);
      const std::span<const uint8_t> converted(frame.output_pcm.data(), frame.output_bytes);
      uint32_t native_nonfinite = 0, post_nonfinite = 0;
      float native_peak = 0, post_peak = 0;
      for (size_t at = 0; at + 4 <= raw.size(); at += 4) {
        const float value = std::bit_cast<float>(xe::load_and_swap<uint32_t>(raw.data() + at));
        if (std::isfinite(value)) { native_peak = std::max(native_peak, std::abs(value)); }
        else { ++native_nonfinite; }
      }
      for (size_t at = 0; at + 4 <= converted.size(); at += 4) {
        uint32_t bits;
        std::memcpy(&bits, converted.data() + at, sizeof(bits));
        const float value = std::bit_cast<float>(bits);
        if (std::isfinite(value)) { post_peak = std::max(post_peak, std::abs(value)); }
        else { ++post_nonfinite; }
      }
      const auto raw_written = native->WriteResult(native_offset, raw);
      const auto post_written = post->WriteResult(post_offset, converted);
      written = !raw_written.error && raw_written.bytes == raw.size() &&
                !post_written.error && post_written.bytes == converted.size();
      write_metadata(fmt::format(
          "{{\"event\":\"frame\",\"index\":{},\"owner_epoch\":{},"
          "\"generation\":{},\"sequence\":{},\"native_offset\":{},\"native_bytes\":{},"
          "\"native_sha256\":\"{}\",\"native_nonfinite\":{},\"native_peak\":{},"
          "\"post_offset\":{},\"post_bytes\":{},\"post_sha256\":\"{}\","
          "\"post_nonfinite\":{},\"post_peak\":{},\"channels\":{},"
          "\"sink_recorded\":{},\"put_called\":{},\"put_accepted\":{}}}\n",
          i, frame.label.owner_epoch, frame.label.generation, frame.label.sequence,
          native_offset, frame.native_bytes, hash(raw), native_nonfinite, native_peak,
          post_offset, frame.output_bytes, hash(converted), post_nonfinite, post_peak,
          frame.output_channels, frame.sink_recorded, frame.put_called, frame.put_accepted));
      native_offset += frame.native_bytes;
      post_offset += frame.output_bytes;
      if (!error_recorded && frame.error[0]) {
        std::string error_hex;
        for (size_t at = 0; at < frame.error.size() && frame.error[at]; ++at) {
          error_hex += fmt::format("{:02x}", uint8_t(frame.error[at]));
        }
        write_metadata(fmt::format("{{\"event\":\"first_sink_error\",\"frame\":{},\"bytes_hex\":\"{}\"}}\n", i, error_hex));
        error_recorded = true;
      }
    }
    if (written) {
      written = !native->FlushResult() && !post->FlushResult() && !metadata->FlushResult();
    }
    emit("cue_observation", written, fmt::format(
        "\"armed\":true,\"detached_result\":true,\"artifacts_written\":{},"
        "\"native_bytes\":{},\"post_bytes\":{},\"metadata_bytes\":{},"
        "\"queued_frames\":{},\"sink_frames\":{},\"accepted_frames\":{},"
        "\"pending_frames\":{},\"native_start_accepted\":{},"
        "\"mixed_output_ambiguity\":true,\"cue_verified\":false,"
        "\"physical_audibility_proven\":false",
        written, native_offset, post_offset, metadata_offset, s.queued_frames,
        s.sink_frames, s.accepted_frames, s.pending_frames, result->start_accepted));
  };
  if (render && accepted) {
    bool complete = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    if (!owner->queue_value) {
      for (uint32_t sample = 0; sample < 120 && !state->IsStopping(); ++sample) {
        if (!state->PreparedTitleIsCurrent()) break;
        const auto observed = probe_transport->PollNotificationRenderProbe(bootstrap);
        if (!observed) break;
        const auto& v = *observed;
        std::string fields = fmt::format("\"sample\":{}", sample);
        const auto add = [&](std::string_view key, auto value) {
          fields += fmt::format(",\"{}\":{}", key, value);
        };
        add("armed", v.armed);
        add("closed", v.closed);
        add("queue_entered", v.queue_entered);
        add("queue_returned", v.queue_returned);
        add("failed", v.failed);
        add("backend_epoch", v.backend_epoch);
        add("foreground_generation", v.foreground_generation);
        add("title_input_swap", v.title_input_swap);
        add("baseline_source", v.baseline_source);
        add("queue_enter_source", v.queue_enter_source);
        add("queue_return_source", v.queue_return_source);
        add("completed_sources", v.completed_sources);
        add("first_disabled_source", v.first_disabled_source);
        add("last_retired_source", v.last_retired_source);
        add("query_reopened_source", v.query_reopened_source);
        add("selected_source", v.selected_source);
        add("selected_retired", v.selected_retired);
        add("native_submission", v.native_submission);
        add("retained_bytes", v.retained_bytes);
        add("width", v.width);
        add("height", v.height);
        add("capture_saved", v.capture_saved);
        add("readback_complete", v.readback_complete);
        add("readback_status", v.readback_status);
        add("permission_for_selected", v.permission_for_selected);
        add("image_ready", v.image_ready);
        add("image_version", v.image_version);
        add("presenter_accepted", v.presenter_accepted);
        add("paint_backend_epoch", v.paint_backend_epoch);
        add("paint_submission", v.paint_submission);
        add("paint_status", v.paint_status);
        add("paint_complete_observed", v.paint_complete_observed);
        add("hidden_after_selected", v.hidden_after_selected);
        add("genuine_clear", v.genuine_clear);
        add("pixels_verified", false);
        add("cue_verified", false);
        complete = !v.failed && v.queue_returned && v.selected_source &&
            v.selected_retired && v.capture_saved && v.readback_complete &&
            v.permission_for_selected && v.paint_complete_observed && v.hidden_after_selected &&
            v.query_reopened_source >= v.selected_source;
        emit("render_snapshot", !v.failed, std::move(fields));
        if (complete || v.failed || std::chrono::steady_clock::now() >= deadline) break;
        bootstrap.Delay(0, 0, uint64_t(-2500000ll));
      }
    }
    const bool probe_closed = probe_transport->CloseNotificationRenderProbe(bootstrap);
    emit("render_summary", !exact_source_diagnostic && complete && probe_closed,
         fmt::format("\"actual_source_fence_paint_and_hide_observed\":{},"
                     "\"suppressed\":{},\"probe_closed\":{},\"pixels_"
                     "verified\":false,\"cue_verified\":false,"
                     "\"gate_complete\":false,\"queue_causality_proven\":false,"
                     "\"native_family_enabled\":false",
                     complete, owner->queue_value == 1, probe_closed));
    if (report->FlushResult()) return Fail("Render diagnostic final report durability");
  }
  finish_cue();
  if (report->FlushResult()) return Fail("Cue diagnostic final report durability");
  if (!accepted)
    return Fail("Fixed native queue Work failed or normally rejected; see receipt");
  // The Task transport's real Wait+Close completed. Native cloned inputs
  // before normal queue return; no native argument borrower survives here.
  {
    std::lock_guard lock(state->mutex);
    state->notification_probe_owner.reset();
  }
  return {true, owner->queue_value, {}};
}

bool NativeSystemUiService::ObserveSharedGraphics(
    const std::shared_ptr<State>& state, XThread& thread, bool required) {
  const auto context = state->Context();
  std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
  {
    std::lock_guard lock(state->mutex);
    prepared = state->prepared;
    if (state->graphics) {
      return true;
    }
  }
  if (!prepared || state->IsStopping() ||
      !ActualSystem(thread, context.kernel) ||
      !context.allocation->IsAccepting()) {
    return false;
  }
  auto observation = prepared->AcquireSharedGraphicsObservation(thread);
  if (!observation) {
    return required ? state->Poison("Shared graphics observation unavailable")
                    : false;
  }
  const auto actual = context.catalog->ReadSharedGraphicsState();
  if (actual.status != NativeSystemSharedGraphicsState::Status::kObserved) {
    return required ? state->Poison("Actual native shared DC not observable")
                    : false;
  }
  gpu::GpuBackendStopProof stop;
  {
    std::lock_guard lock(state->mutex);
    stop = state->backend_stop;
  }
  if (!stop.backend_epoch() || stop.stopped() ||
      prepared->backend_stop_proof().backend_epoch() != stop.backend_epoch()) {
    return state->Poison("Shared graph's actual backend lifetime ended");
  }
  NativeSystemGraphicsDeviceRecord record;
  record.allocation = context.allocation;
  record.root_load_generation = context.allocation->root_load_generation();
  record.factory = actual.factory;
  record.device = actual.device;
  record.shared_device_cell = actual.symbols.shared_device_cell;
  record.recording_context = actual.recording_context;
  record.recording_context_cell = actual.symbols.record_context_cell;
  record.dc = actual.dc;
  record.dc_cell = actual.symbols.dc_cell;
  record.stop_proof = stop;
  record.device_publication = context.device_publication;
  std::string error;
  auto binding =
      NativeGraphicsDeviceBinding::CreateForNativeSystem(record, &error);
  if (!binding) {
    return state->Poison("Borrowed native shared graphics binding: " + error);
  }
  const auto attached =
      prepared->AttachSharedGraphics(context, actual, binding, thread);
  if (!attached.succeeded) {
    return state->Poison("Shared graphics transport attachment: " +
                         attached.detail);
  }
  {
    std::lock_guard lock(state->mutex);
    if (state->IsStopping() || !context.allocation->IsAccepting()) {
      return false;
    }
    state->graphics = std::move(binding);
    state->shared_graphics = actual;  // Retain the real cohort-owned pins.
    state->status.source_frame_ready = true;
  }
  return true;
}

NativeSystemUiService::Snapshot NativeSystemUiService::snapshot() const {
  std::lock_guard lock(state_->mutex);
  auto result = state_->status;
  result.stopping = state_->IsStopping();
  if (result.stopping || !state_->allocation ||
      !state_->allocation->IsAccepting() || state_->backend_stop.stopped()) {
    result.boot_admitted = result.source_frame_ready = false;
  }
  return result;
}
std::shared_ptr<NativeGraphicsDeviceBinding>
NativeSystemUiService::graphics_binding() const {
  std::lock_guard lock(state_->mutex);
  return state_->IsStopping() ? nullptr : state_->graphics;
}
void NativeSystemUiService::BeginStop() noexcept {
  state_->stopping.store(true, std::memory_order_release);
  std::shared_ptr<NativeManagedFormatFrontdoor> format_frontdoor;
  std::shared_ptr<NativeNotificationPreferenceCoordinator> preferences;
  std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
  std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
  std::shared_ptr<NativeNotificationPresentationObserver> observer;
  {
    std::lock_guard lock(state_->mutex);
    format_frontdoor = state_->format_frontdoor;
    prepared = state_->prepared;
    diagnostic = state_->config.diagnostic;
    observer = state_->notification_observer;
    preferences = state_->preferences;
    if (!preferences && state_->config.kernel) {
      preferences = state_->config.kernel->notification_preferences();
    }
  }
  if (format_frontdoor) format_frontdoor->BeginStop();
  if (preferences) {
    preferences->BeginStop();
  }
  if (observer) {
    observer->BeginStop();
  }
  if (prepared) {
    prepared->CloseExternalRequests();
  }
  if (diagnostic) {
    diagnostic->CloseExternalRequests();
  }
}
bool NativeSystemUiService::JoinBootstrap(
    std::chrono::milliseconds timeout) noexcept {
  try {
    if (XThread::IsInThread()) {
      return false;
    }
    std::lock_guard lifecycle(state_->lifecycle);
    BeginStop();
    object_ref<XHostThread> bootstrap;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->status.bootstrap_joined) {
        return true;
      }
      bootstrap = state_->bootstrap;
    }
    if (bootstrap) {
      auto* native_thread = bootstrap->thread();
      if (!native_thread ||
          xe::threading::Wait(native_thread, false, timeout) !=
              xe::threading::WaitResult::kSuccess) {
        return false;
      }
    }
    // OS wait joins the actual host thread, including Exit/cleanup tails. Its
    // guest KTHREAD signal or merely returning from Bootstrap is insufficient.
    BeginStop();  // Also closes a prepared transport published during the join.
    std::lock_guard lock(state_->mutex);
    state_->status.bootstrap_joined = true;
    return true;
  } catch (...) {
    return false;
  }
}
bool NativeSystemUiService::StopBeforeCpuAndProviders() noexcept {
  try {
    if (XThread::IsInThread()) {
      return false;
    }
    std::lock_guard stop(state_->stop);
    if (!JoinBootstrap()) {
      return false;
    }
    std::shared_ptr<NativeManagedFormatFrontdoor> format_frontdoor;
    std::shared_ptr<NativeSystemTaskContinuation> continuation;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
    std::shared_ptr<NativeContentStorageDriver> storage;
    std::shared_ptr<NativeNotificationPresentationObserver> observer;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->status.stop_drained) {
        return true;
      }
      format_frontdoor = state_->format_frontdoor;
      continuation = state_->continuation;
      prepared = state_->prepared;
      diagnostic = state_->config.diagnostic;
      allocation = state_->allocation;
      storage = state_->storage;
      observer = state_->notification_observer;
    }
    {
      std::unique_lock lock(state_->mutex);
      if (!state_->public_drained.wait_for(lock, std::chrono::seconds(5),
                                           [&] {
                                             return state_->public_uncertain ||
                                                    !state_->public_calls;
                                           }) ||
          state_->public_uncertain) {
        return false;
      }
    }
    // All original caller C++ invocations have returned while providers/native
    // tasks are still live. No guest fiber with an admitted call is reclaimed.
    // Bootstrap creation is now sealed and its entire C++ stack joined, so a
    // late continuation install cannot escape this snapshot/rundown.
    // Native Task and original Complete must remain runnable until every real
    // managed worker/request drains; do this before dispatcher/storage stop.
    if (format_frontdoor && !format_frontdoor->Drain()) return false;
    if (continuation) {
      continuation->BeginStop();
      if (!continuation->Drain()) {
        return false;
      }
    }
    if (storage) {
      storage->BeginStop();
      if (!storage->Drain()) {
        return false;
      }
    }
    if (prepared) {
      prepared->BeginStopTransport();
      if (!prepared->DrainCallbacksBeforeCpuStop()) {
        return false;
      }
    }
    if (diagnostic) {
      diagnostic->BeginStopTransport();
      if (!diagnostic->DrainCallbacksBeforeCpuStop()) {
        return false;
      }
    }
    if (observer) {
      observer->BeginStop();
      if (!observer->Drain()) {
        return false;
      }
    }
    if (allocation) {
      allocation->CloseAdmission();
    }
    std::lock_guard lock(state_->mutex);
    state_->status.stop_drained = true;
    return true;
  } catch (...) {
    return false;
  }
}
bool NativeSystemUiService::RetireAfterKernelStop() noexcept {
  try {
    KernelState* kernel;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
    std::shared_ptr<NativeSystemUiStartupRuntime> startup;
    std::shared_ptr<GuestRuntime> title_runtime;
    gpu::GpuBackendStopProof stop;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->retired) {
        return true;
      }
      if (!state_->status.bootstrap_joined || !state_->status.stop_drained ||
          !state_->IsStopping() || state_->public_calls ||
          state_->public_uncertain || !state_->public_invocations.empty()) {
        return false;
      }
      kernel = state_->config.kernel;
      prepared = state_->prepared;
      diagnostic = state_->config.diagnostic;
      startup = state_->startup;
      title_runtime = state_->title_heap_runtime;
      stop = state_->backend_stop;
    }
    if (!kernel || XThread::IsInThread()) {
      return false;
    }
    for (const auto& thread : kernel->object_table()->GetObjectsByType<XThread>(
             XObject::Type::Thread)) {
      if (thread->is_running()) {
        return false;
      }
    }
    if (prepared) {
      if (!stop.backend_epoch()) {
        // Preparation may fail after install but before the profile/cache
        // check.
        stop = prepared->backend_stop_proof();
      }
      if (!stop.backend_epoch() || !stop.stopped() ||
          !prepared->RetireAfterKernelStop()) {
        return false;
      }
    }
    if (diagnostic) {
      if (!stop.backend_epoch()) {
        stop = diagnostic->backend_stop_proof();
      }
      if (!stop.backend_epoch() || !stop.stopped() ||
          !diagnostic->RetireAfterKernelStop()) {
        return false;
      }
    }
    if (startup &&
        startup->RetireAndDrain(false) != GuestRuntime::DrainStatus::kDrained) {
      return false;
    }
    if (title_runtime && title_runtime->RetireAndDrain(false) !=
                             GuestRuntime::DrainStatus::kDrained) {
      return false;
    }
    // Synchronous public_calls==0 is NOT native XMsg request drainage. Keep
    // heap0 until this actual all-CPU/GPU-stopped whole-Kernel boundary; never
    // call native6160 from stop/destruction or free it beneath native workers.
    // Same-Kernel Title replacement is deliberately not supported.
    // Drop holders before Kernel/Memory disappear, outside the metadata lock.
    // No native Close or guest callback is executed here.
    Config retired_config;
    object_ref<XHostThread> bootstrap;
    object_ref<UserModule> root, prepared_title_module;
    std::shared_ptr<KernelState::UserModuleExecutionLease> prepared_title_lease;
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeCurrentUserProviderBinding> current_users;
    std::shared_ptr<NativeNotificationPresentationObserver> notification_observer;
    std::shared_ptr<NativeNotificationPreferenceCoordinator> preferences;
    std::shared_ptr<void> notification_probe_owner;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemCallbackDispatcher> dispatcher;
    std::shared_ptr<NativeInputDriver> input;
    std::unique_ptr<NativeInputDriver::Installation> input_installation;
    std::shared_ptr<NativeManagedFormatFrontdoor> format_frontdoor;
    std::shared_ptr<NativeContentStorageDriver> storage;
    std::unique_ptr<NativeContentStorageDriver::Installation>
        storage_installation;
    std::shared_ptr<NativeSystemTaskContinuation> continuation;
    std::shared_ptr<NativeGraphicsDeviceBinding> graphics;
    std::shared_ptr<NativeGraphicsDeviceBinding> device_publication;
    NativeSystemSharedGraphicsState shared_graphics;
    {
      std::lock_guard lock(state_->mutex);
      state_->retired = true;
      state_->status.boot_admitted = state_->status.source_frame_ready = false;
      state_->status.title_heap_ready = false;
      retired_config = std::move(state_->config);
      state_->config.kernel = nullptr;
      bootstrap = std::move(state_->bootstrap);
      root = std::move(state_->root);
      prepared_title_module = std::move(state_->prepared_title_module);
      prepared_title_lease = std::move(state_->prepared_title_lease);
      title_runtime = std::move(state_->title_heap_runtime);
      calls = std::move(state_->calls);
      current_users = std::move(state_->current_users);
      notification_observer = std::move(state_->notification_observer);
      preferences = std::move(state_->preferences);
      notification_probe_owner = std::move(state_->notification_probe_owner);
      allocation = std::move(state_->allocation);
      catalog = std::move(state_->catalog);
      domain = std::move(state_->domain);
      dispatcher = std::move(state_->dispatcher);
      input = std::move(state_->input);
      input_installation = std::move(state_->input_installation);
      format_frontdoor = std::move(state_->format_frontdoor);
      storage = std::move(state_->storage);
      storage_installation = std::move(state_->storage_installation);
      continuation = std::move(state_->continuation);
      graphics = std::move(state_->graphics);
      device_publication = std::move(state_->device_publication);
      shared_graphics = std::move(state_->shared_graphics);
      prepared = std::move(state_->prepared);
      startup = std::move(state_->startup);
    }
    // This exact all-CPU/GPU-stopped retirement still has Kernel/Memory alive.
    // Drop even normal rejected/report-failed probe buffers outside State's
    // mutex; an external stopped service handle must retain metadata only.
    notification_probe_owner.reset();
    return true;
  } catch (...) {
    return false;
  }
}

NativeSystemUiPublicCallResult
NativeSystemUiService::TryInvokePublicOnOriginalCaller(
    KernelState* kernel, NativeSystemUiPublicCall call,
    std::span<const uint32_t> arguments) noexcept {
  // Keep the established DWORD API source compatible. Bound the copy before
  // allocation, and use the same admission/ownership path as register calls.
  if (arguments.size() > cpu::kGuestCallMaxArguments) {
    return {NativeSystemUiPublicCallResult::Disposition::kDenied,
            X_ERROR_INVALID_PARAMETER};
  }
  std::array<uint64_t, cpu::kGuestCallMaxArguments> registers{};
  std::copy(arguments.begin(), arguments.end(), registers.begin());
  return TryInvokePublicRegistersOnOriginalCaller(
      kernel, call, std::span(registers).first(arguments.size()));
}

NativeSystemUiPublicCallResult
NativeSystemUiService::TryInvokePublicRegistersOnOriginalCaller(
    KernelState* kernel, NativeSystemUiPublicCall call,
    std::span<const uint64_t> arguments) noexcept {
  using Result = NativeSystemUiPublicCallResult;
  using Disposition = Result::Disposition;
  const Result denied{Disposition::kDenied, X_ERROR_NOT_READY};
  static xe::NativeSystemUiTraceLimiter entry_trace;
  entry_trace.Emit(
      "input", "public_call_entry",
      {{"call", uint16_t(call)}, {"argument_count", arguments.size()}});
  try {
    if (!kernel) {
      return {};
    }
    std::shared_ptr<NativeSystemUiService> service;
    {
      std::lock_guard lock(State::registry_mutex);
      const auto found = State::registry.find(kernel);
      const auto admission =
          found == State::registry.end() ? nullptr : found->second.lock();
      service = admission ? admission->service.lock() : nullptr;
    }
    if (!service) {
      static xe::NativeSystemUiTraceLimiter trace;
      trace.Emit("input", "public_call_inactive",
                 {{"call", uint16_t(call)}, {"reason", 1}});
      return {};
    }
    auto* state = service->state_.get();
    {
      std::lock_guard lock(state->mutex);
      if (!state->public_ever_admitted) {
        static xe::NativeSystemUiTraceLimiter trace;
        trace.Emit("input", "public_call_inactive",
                   {{"call", uint16_t(call)}, {"reason", 2}});
        return {};  // Preserve inactive HLE without allocating a native call.
      }
    }
    auto invocation = std::make_shared<State::PublicInvocation>();
    const auto* profile = NativeSystemUiPublicProfile(call);
    auto* thread =
        XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
    {
      std::lock_guard lock(state->mutex);
      if (!state->public_ever_admitted) {
        return {};  // Existing HLE remains authoritative while native is
                    // inactive.
      }
      if (profile && !NativeSystemPublicFamilyRouteEnabled(profile->family)) {
        return denied;  // Catalog is not a provider/engine admission witness.
      }
      if (!NativeSystemUiPublicArgumentsValid(call, arguments) || !thread ||
          thread->kernel_state() != kernel || state->IsStopping() ||
          state->retired || !state->status.boot_admitted ||
          !state->status.title_heap_ready || state->config.kernel != kernel ||
          !state->prepared || !state->allocation ||
          !state->allocation->IsAccepting() || state->backend_stop.stopped() ||
          state->public_uncertain || state->public_calls >= 64 ||
          state->public_invocations.size() >= 64) {
        static xe::NativeSystemUiTraceLimiter trace;
        trace.Emit("input", "public_call_admission_denied",
                   {{"call", uint16_t(call)},
                    {"reason", 3},
                    {"boot_admitted", state->status.boot_admitted},
                    {"retired", state->retired},
                    {"public_uncertain", state->public_uncertain}});
        return denied;
      }
      const auto process = thread->native_frame_thread_identity().process_type;
      if (process != X_PROCTYPE_TITLE && process != X_PROCTYPE_SYSTEM) {
        return denied;
      }
      invocation->calls = state->calls;
      invocation->prepared = state->prepared;
      invocation->domain_operation = state->domain->Acquire();
      if (!invocation->domain_operation) {
        return denied;
      }
      invocation->caller = retain_object(thread);
      state->public_invocations.push_back(invocation);
      ++state->public_calls;
    }
    // No service/domain/Kernel/backend lock crosses any actual guest call,
    // wait, output access or transport reservation. Current process/TLS/Task
    // stay real.
    Result result = denied;
    bool executed = false, healthy = true;
    try {
      const uint32_t lr = uint32_t(thread->thread_state()->context()->lr);
      const auto caller = lr >= 4 && !(lr & 3)
                              ? kernel->GetUserModuleForAddress(lr - 4)
                              : object_ref<UserModule>{};
      xex2_section_type section;
      if (caller && caller->module_format() == UserModule::kModuleFormatXex &&
          caller->xex_module()->GetPageSectionType(lr - 4, &section) &&
          section == XEX_SECTION_CODE) {
        invocation->caller_code =
            kernel->AcquireUserModuleExecutionLease(caller);
      }
      // The single Kernel-owned heap cannot be reused for an executable
      // replacement, even if a caller failed to advance foreground generation.
      const bool title_matches = state->PreparedTitleIsCurrent();
      invocation->transport_operation =
          title_matches
              ? invocation->prepared->AcquirePublicCallOperation(call, *thread)
              : nullptr;
      bool accepting;
      {
        std::lock_guard lock(state->mutex);
        accepting = !state->IsStopping() && state->status.boot_admitted &&
                    !state->public_uncertain && !state->backend_stop.stopped();
      }
      if (accepting && invocation->caller_code &&
          invocation->transport_operation &&
          ValidPublicFunction(*invocation->calls, *profile)) {
        const bool creating =
            call == NativeSystemUiPublicCall::kShowGuide ||
            call == NativeSystemUiPublicCall::kShowKeyboard ||
            call == NativeSystemUiPublicCall::kShowMessageBox ||
            call == NativeSystemUiPublicCall::kShowDeviceSelector;
        const bool title_caller =
            thread->native_frame_thread_identity().process_type ==
            X_PROCTYPE_TITLE;
        // Snapshot host ownership at ENTRY, before actual input PPC. Its CPU
        // lease also keeps a modal that closes concurrently from switching the
        // route before this call's native cached transition is filtered.
        invocation->host_input = invocation->prepared->CurrentHostUi();
        const auto* host_input = kernel->emulator()->input_system();
        invocation->host_input_blocked =
            title_caller && host_input && host_input->IsUIInputBlocked();
        NativeUiBackendRoute::Admission ui;
        if (creating) {
          ui = invocation->prepared->AdmitUiCreating(title_caller);
        }
        if (creating && ui.decision == NativeUiBackendRoute::Decision::kHost) {
          result = {Disposition::kHostFallback, 0, std::move(ui.host)};
        } else if (creating &&
                   ui.decision != NativeUiBackendRoute::Decision::kNative) {
          result = denied;
        } else if (call == NativeSystemUiPublicCall::kIsUiActive &&
                   invocation->host_input) {
          // Existing wrapper composes actual XamState/host InputBlocker state.
          result = {Disposition::kReturned, 0};
        } else {
          executed = true;  // A false/throw after this point is uncertain.
          const auto native =
              invocation->calls->ExecuteDeferredPublicRegistersOnOriginalCaller(
                  *thread, call, arguments);
          healthy = native.succeeded;
          result = healthy ? Result{Disposition::kReturned,
                                    uint32_t(native.return_value)}
                           : Result{Disposition::kExecutionFailed,
                                    X_ERROR_FUNCTION_FAILED};
          if (healthy && !result.value && title_caller &&
              (invocation->host_input || invocation->host_input_blocked) &&
              (call == NativeSystemUiPublicCall::kInputGetKeystroke ||
               call == NativeSystemUiPublicCall::kInputGetKeystrokeEx)) {
            // The real native call has already validated user/flags/device and
            // advanced its own audience cache. Preserve every native error,
            // including DNC. Swallow only a valid successful 8-byte stroke.
            const uint32_t output = uint32_t(arguments[2]);
            auto memory_lock = global_critical_region::AcquireDirect();
            if (kernel->memory()->QueryRangeAccess(output, 8) ==
                memory::PageAccess::kReadWrite) {
              std::memset(kernel->memory()->TranslateVirtual(output), 0, 8);
              result.value = X_ERROR_EMPTY;
            } else {
              result.value = X_ERROR_INVALID_PARAMETER;
            }
          }
        }
      }
    } catch (...) {
      healthy = !executed;
      result = executed ? Result{Disposition::kExecutionFailed,
                                 X_ERROR_FUNCTION_FAILED}
                        : denied;
    }
    {
      std::lock_guard lock(state->mutex);
      if (healthy) {
        const auto found =
            std::find(state->public_invocations.begin(),
                      state->public_invocations.end(), invocation);
        if (found != state->public_invocations.end()) {
          state->public_invocations.erase(found);
        }
      } else {
        state->public_uncertain = true;
      }
    }
    // Metadata only: this is the original caller result, not a successful
    // async UI request or physical input delivery proof.
    if (cvars::native_system_ui_trace) {
      static std::array<xe::NativeSystemUiTraceLimiter, 16> traces;
      static xe::NativeSystemUiTraceLimiter ui_active_trace;
      const size_t trace_index = uint32_t(call) % traces.size();
      auto& trace =
          uint16_t(call) == 0x2EC ? ui_active_trace : traces[trace_index];
      const uint64_t key = (uint64_t(uint16_t(call)) << 48) |
                           (uint64_t(result.disposition) << 32) | result.value;
      trace.EmitTransition(
          key, "input", "public_call_return",
          {{"call", uint16_t(call)},
           {"thread_id", thread->thread_id()},
           {"process_type",
            thread->native_frame_thread_identity().process_type},
           {"executed_attempted", executed},
           {"disposition", uint32_t(result.disposition)},
           {"result_value", result.value},
           {"argument_count", arguments.size()},
           {"host_lease_serial",
            result.host_fallback     ? result.host_fallback->serial()
            : invocation->host_input ? invocation->host_input->serial()
                                     : 0}});
    }
    // Drop all healthy reservation/code/caller closures before reporting drain.
    // These are host-only destructors, never guest Task/overlap/FF destruction.
    invocation.reset();
    {
      std::lock_guard lock(state->mutex);
      --state->public_calls;
      state->public_drained.notify_all();
    }
    // No guest wait/PPC follows decrement. Scheduler shutdown joins actual CPU
    // dispatch workers before reclaiming the ordinary wrapper's trivial tail.
    return result;
  } catch (...) {
    return denied;  // Never unwind a guest shim or trigger active HLE fallback.
  }
}

void NativeSystemUiService::SealHostUiFallbackForKernelOrFailStop(
    KernelState* kernel) noexcept {
  try {
    if (!kernel) {
      return;
    }
    if (const auto preferences = kernel->notification_preferences()) {
      preferences->BeginStop();
    }
    std::shared_ptr<NativeSystemUiService> service;
    {
      std::lock_guard lock(State::registry_mutex);
      const auto found = State::registry.find(kernel);
      const auto admission =
          found == State::registry.end() ? nullptr : found->second.lock();
      if (admission) {
        admission->closed.store(true, std::memory_order_release);
        service = admission->service.lock();
      }
    }
    if (!service) {
      return;
    }
    // Close ingress first; existing visible dialogs are still closed by the
    // caller's subsequent on_title_closing, before full native/provider stop.
    service->BeginStop();
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    {
      std::lock_guard lock(service->state_->mutex);
      prepared = service->state_->prepared;
    }
    if (prepared && !prepared->SealHostUiFallback()) {
      FailStop();
    }
  } catch (...) {
    FailStop();
  }
}

void NativeSystemUiService::StopForKernelOrFailStop(
    KernelState* kernel) noexcept {
  try {
    if (!kernel) {
      return;
    }
    const auto preferences = kernel->notification_preferences();
    if (preferences) {
      preferences->BeginStop();
    }
    std::shared_ptr<NativeSystemUiService> service;
    {
      std::lock_guard lock(State::registry_mutex);
      if (!kernel) {
        return;
      }
      const auto admission = State::AdmissionForKernel(kernel);
      admission->closed.store(true, std::memory_order_release);
      service = admission->service.lock();
    }
    if (service && !service->StopBeforeCpuAndProviders()) {
      FailStop();
    }
    // Also covers an inactive/never-created native service: fallback IO still
    // owns the shared Console authority until its admitted operation returns.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (preferences && !preferences->SafelyDrained()) {
      if (preferences->snapshot().uncertain ||
          std::chrono::steady_clock::now() >= deadline) {
        FailStop();
      }
      xe::threading::Sleep(std::chrono::milliseconds(1));
    }
  } catch (...) {
    FailStop();
  }
}
}  // namespace xe::kernel
