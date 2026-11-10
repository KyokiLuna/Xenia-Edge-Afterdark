/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_graphics_transport.h"
#include "xenia/kernel/native_notification_presentation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/native_frame_command_plan.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/system_command_buffer.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_frame_readback_capture.h"
#include "xenia/kernel/native_graphics_device_binding.h"
#include "xenia/kernel/native_hud_module_ownership.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_input_driver.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_callback_dispatcher.h"
#include "xenia/kernel/native_system_graphics_notifications.h"
#include "xenia/kernel/native_system_ui_notifications.h"
#include "xenia/kernel/title_execution_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"
#include "xenia/xbox.h"

DECLARE_bool(native_system_first_record_probe);
DECLARE_path(native_notification_render_probe_report);
DEFINE_string(native_system_ui_output_capture_base, "",
              "Private one-source native output export base; empty disables.",
              "Research");
DEFINE_int64(native_system_ui_output_capture_source, 0,
             "Exact positive Source serial for the one-shot output export.",
             "Research");
DEFINE_bool(native_system_ui_output_capture_b_down, false,
            "One Source frozen after an observed native B-down while visible; "
            "requires private output base and exact Source selector zero.",
            "Research");

namespace xe::kernel {
namespace {
using Transport = NativeSystemGraphicsTransport;
using Hook = Transport::HookResult;
using Blocker = Transport::Blocker;
using Counter = Transport::DiagnosticCounter;
std::array<std::atomic<uint64_t>, size_t(Counter::kCount)> hook_counters{};
Transport::QueryResult RejectedQuery(Counter reason) {
  Transport::RecordDiagnosticCounter(reason);
  Transport::RecordDiagnosticCounter(Counter::kQueryRejected);
  return {Hook::kRejected, 0};
}
Transport::QueryResult HandledQuery(uint32_t available) {
  Transport::RecordDiagnosticCounter(available ? Counter::kQueryAvailable
                                               : Counter::kQueryBusy);
  return {Hook::kHandled, available};
}
Hook RejectedSet(Counter reason) {
  Transport::RecordDiagnosticCounter(reason);
  Transport::RecordDiagnosticCounter(Counter::kSetRejected);
  return Hook::kRejected;
}
Hook HandledSet(Counter extra = Counter::kSetHandled) {
  Transport::RecordDiagnosticCounter(Counter::kSetHandled);
  if (extra != Counter::kSetHandled) {
    Transport::RecordDiagnosticCounter(extra);
  }
  return Hook::kHandled;
}
// This helper only reads already-owned immutable cohort metadata. Callers keep
// their existing State::Operation; it grants no new input or Source permission.
void TraceTransport(xe::NativeSystemUiTraceLimiter& limiter,
                    const NativeSystemUiServiceContext& c, const char* event,
                    std::initializer_list<xe::NativeSystemUiTraceLimiter::Field>
                        fields) noexcept {
  if (!cvars::native_system_ui_trace) {
    return;
  }
  try {
    if (!c.allocation || !c.domain) {
      return;
    }
    const auto owner = c.allocation->owner();
    limiter.Emit(
        "transport", event,
        {{"kernel_epoch", owner.kernel_epoch},
         {"runtime_module_generation", owner.module_generation},
         {"owner_session_id", owner.session_id},
         {"root_hmodule", owner.module_hmodule},
         {"root_load_generation", c.allocation->root_load_generation()},
         {"domain_id", c.domain->id()}},
        fields);
  } catch (...) {
  }
}
#define TRACE_TRANSPORT(context, event, ...)                \
  do {                                                      \
    static xe::NativeSystemUiTraceLimiter limiter;          \
    TraceTransport(limiter, context, event, {__VA_ARGS__}); \
  } while (false)

constexpr uint32_t kDescriptorBytes = 148;
constexpr uint32_t kMaxPrimaryBytes = 0x40000;
constexpr uint32_t kSetReturn = 0x81842BFC;
constexpr uint32_t kRecordedSetCaller = 0x816CD678;
constexpr uint32_t kFailedEndSetCaller = 0x816CD620;
constexpr uint32_t kClearSetCaller = 0x816C9A64;
constexpr std::array<uint32_t, 3> kQueryReturns{0x816CD70C, 0x816C9A38,
                                                0x816C9A68};

GuestRuntime::ExecutionResult Fail(std::string_view reason) {
  return {false, 0x80004005u, std::string(reason)};
}
GuestRuntime::ExecutionResult Ok() { return {true, 0, {}}; }
[[noreturn]] void FailStop() noexcept {
  try {
    XELOGE("Native graphics transport: callback/CPU/GPU retirement unproven");
    xe::FlushLog();
  } catch (...) {
  }
  std::quick_exit(EXIT_FAILURE);
}
std::optional<uint32_t> ReadWord(Memory& memory, uint32_t at) {
  if (!at || (at & 3) ||
      memory.QueryRangeAccess(at, 4) == memory::PageAccess::kNoAccess) {
    return {};
  }
  return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at));
}
std::optional<uint32_t> CanonicalPhysical(Memory& memory, uint32_t observed) {
  if (observed < 0x20000000) {
    return observed;
  }
  const auto* heap = memory.LookupHeap(observed);
  if (!heap || heap->heap_type() != HeapType::kGuestPhysical) {
    return {};
  }
  const uint32_t physical = memory.GetPhysicalAddress(observed);
  return physical < 0x20000000 ? std::optional<uint32_t>(physical)
                               : std::nullopt;
}
bool ActualSystem(const NativeSystemUiServiceContext& c, XThread& thread) {
  return c.kernel && XThread::IsInThread(&thread) &&
         thread.kernel_state() == c.kernel &&
         thread.native_frame_thread_identity().process_type ==
             X_PROCTYPE_SYSTEM;
}
bool NativeCaller(const NativeSystemUiServiceContext& c, XThread& thread,
                  uint32_t lr) {
  return ActualSystem(c, thread) && lr >= 4 && !(lr & 3) && c.allocation &&
         c.catalog && c.domain && c.domain->active() &&
         c.allocation->IsAccepting() && c.allocation->MatchesDomain(c.domain) &&
         c.kernel->ResolveNativeImportCaller(lr).domain == c.domain &&
         c.kernel->GetUserModuleForAddress(lr - 4).get() ==
             c.catalog->calls()->module().get();
}
bool AdvertiseHardwareGraphics(const NativeSystemUiServiceContext& c) {
  // Actual exported Kernel variable bound by this exact native root, never a
  // copied private global or arbitrary caller-supplied guest address.
  auto* memory = c.kernel->memory();
  auto lock = global_critical_region::AcquireDirect();
  const auto* exported =
      c.kernel->processor()->export_resolver()->GetExportByOrdinal(
          "xboxkrnl.exe", 0x156);
  if (!exported || exported->get_type() != cpu::Export::Type::kVariable ||
      std::string_view(exported->name) != "XboxHardwareInfo") {
    return false;
  }
  const auto bound = ReadWord(*memory, 0x815F0480);
  if (!bound || *bound != exported->variable_ptr ||
      memory->QueryRangeAccess(*bound, 4) != memory::PageAccess::kReadWrite) {
    return false;
  }
  const auto before = c.catalog->ReadHardwareFlags();
  if (!before) {
    return false;
  }
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(*bound),
                               *before | 0x200u);
  return c.catalog->ReadHardwareFlags() == (*before | 0x200u);
}

struct CodeProfile {
  uint32_t at, bytes;
  std::string_view hash;
};
// Each full function is the verified installed17559 original. The two Query
// instructions tail-call the import, so LR is its real caller (not81842118).
constexpr CodeProfile kCodeProfiles[] = {
    {0x816BC968, 104,
     "18f0d8086b2be8d51033df4edc6f33d813bc24bc58c977dd0d9982371c2538d"
     "8"},  // native FE lookup leaf
    {0x816BC9D0, 216,
     "3e807a18474b022a6f51e040af76df21cba5a8395b18504cb714d2b44f8845a"
     "1"},  // native public RegisterSysApp
    {0x816BCCA8, 96,
     "67e2768dffed4e98d377274c666b60c967ce8c20313a3d058bed75da5ac4bc4"
     "e"},  // native FE constructor
    {0x816BDE90, 172,
     "7689eff393cd3d203d2de6f2b6564b7a2c2649715a6849b41ac25b0974c5a7b"
     "8"},  // native FE init
    {0x816CE8B0, 804,
     "caedda02e6001448ea4bb773a4986f2f4fe27cfa8bcc83014150921970d4549"
     "4"},  // native manager startup callback
    {0x816CEBD8, 192,
     "99f827bd62230d14b2a5704c3d7f1f340f448d185eb0eaf258f39794e0275f0"
     "e"},  // native manager constructor
    {0x816C8328, 296,
     "f92f68e758c525b2ad127c092f782539af94c2e0b68f8c0170b63adf426935f"
     "d"},  // native shared renderer/DC init
    {0x8177DAC8, 176,
     "b184e69850a19cfca4163af5bdc048c9c9d43de4f22b337bd07864dac7b7131"
     "3"},  // native CreateDC
    {0x81783018, 140,
     "efd8cc5b94c066dedbe4c3736c083305aa7ce214132781896914fb7375fa166"
     "b"},  // native actual DC factory
    {0x81780C50, 144,
     "50c758b75b16fe7db9fd4ec3396c8710a57824ef1a0575707fe1ec62af84fd5"
     "a"},  // native DC constructor
    {0x8177B8B8, 40,
     "1227093b997c4547c95ef463cb585e026a38c3451f0ce433476a917dd56cc19"
     "d"},  // native DC AddRef leaf
    {0x8183E610, 40,
     "124bac340c841c468c3fec726080842099dc5d2b70446a2ede3aff8f7b6427a"
     "3"},  // native D3D device AddRef leaf
    {0x8172D3C0, 32,
     "7ce082330530783a3159ad60051b5a3fa13dec6ad5814a3b3c1ada096e71430"
     "3"},
    {0x81842C08, 1056,
     "bb85906c1cf56734ffbd8c79d1ff5dc271a0ffa17a2e2ec40189d077182a1e7"
     "3"},
    {0x81842110, 8,
     "f2f293a16c53f645570e79ac5664e194c5e1a21454aa2c631cb3469f1dbd0c7"
     "4"},
    {0x81842B08, 252,
     "0defc3e9b18c063565d3b941b7bbbfd2c61ac30a58f211567fba81307cb3358"
     "5"},
    {0x816CCF00, 2020,
     "02431c49d69ca45aa6a073b1991aff96ae8ce3a50557828dba68e57131495bc"
     "6"},
    {0x816C9A20, 148,
     "f3bd2e50127dd0ea949bc71da36d653a9d9e352ac748101e06353e3a8235867"
     "a"},
};
bool ValidateCode(const NativeSystemUiServiceContext& c) {
  auto module = c.catalog->calls()->module();
  auto* memory = c.kernel->memory();
  for (const auto& p : kCodeProfiles) {
    if (memory->QueryRangeAccess(p.at, p.bytes) ==
        memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint64_t at = p.at; at < uint64_t(p.at) + p.bytes; at += 4) {
      xex2_section_type type;
      if (c.kernel->GetUserModuleForAddress(uint32_t(at)).get() !=
              module.get() ||
          !module->xex_module()->GetPageSectionType(uint32_t(at), &type) ||
          type != XEX_SECTION_CODE ||
          module->xex_module()->TranslateCode(uint32_t(at)) !=
              memory->TranslateVirtual<const uint8_t*>(uint32_t(at))) {
        return false;
      }
    }
    std::array<uint8_t, 32> digest;
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(p.at), p.bytes,
                   digest.data(), 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
      if (p.hash[2 * i] != hex[digest[i] >> 4] ||
          p.hash[2 * i + 1] != hex[digest[i] & 15]) {
        return false;
      }
    }
  }
  return true;
}

// Native FE is in the actual fixed EF..FE descriptor array. For FE only,
// the hashed native lookup computes81AA0BA0+BA88-FE*BC ==81AA0BA0. There is
// no linked list to fabricate and no HLE GuestAppRegistry lookup here.
bool NativeFeRegistryPresent(const NativeSystemUiServiceContext& c) {
  const auto hud = ObserveNativeHudModuleOwnership(
      c, NativeHudOriginPolicy::kInstalledFlash);
  constexpr uint32_t descriptor = 0x81AA0BA0;
  if (c.catalog->calls()->module()->GetProcAddressByOrdinal(0x24B) !=
          0x816BC9D0 ||
      !c.allocation->IsAccepting() || !c.allocation->MatchesDomain(c.domain)) {
    return false;
  }
  auto& memory = *c.kernel->memory();
  auto lock = global_critical_region::AcquireDirect();
  const auto module = ReadWord(memory, descriptor + 8);
  const auto initialized = ReadWord(memory, descriptor + 16);
  if (!module || !*module || !initialized || !*initialized) {
    return false;
  }
  // The real Get(FE) result is now this exact native descriptor, not a host
  // registration inferred from success of the initializer.
  return ReadWord(memory, descriptor + 4) == 0xFE && *module == 0xAAAABBBB &&
         ReadWord(memory, descriptor + 12) == 0x816BD930 && *initialized == 1 &&
         hud.established &&
         c.kernel->ResolveNativeImportCaller(0x816BD934).domain == c.domain;
}
bool SnapshotBackend(const gpu::NativeSystemFrameCapacity& capacity) {
  return capacity.supports_immutable_input_admission();
}

// The observer and its control tokens contain CPU data only. A control stays
// counted through the destruction of the queued callback closure, including
// inline rejection, late sealed work and a throwing enqueue.
struct OutputCaptureObservation {
  struct Control {
    explicit Control(std::shared_ptr<OutputCaptureObservation> value)
        : observation(std::move(value)) {
      std::lock_guard lock(observation->mutex);
      ++observation->controls;
    }
    ~Control() {
      std::lock_guard lock(observation->mutex);
      --observation->controls;
      observation->changed.notify_all();
    }
    std::shared_ptr<OutputCaptureObservation> observation;
  };
  bool Done() const {
    std::lock_guard lock(mutex);
    return controls == 0;
  }
  bool Drain(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex);
    return changed.wait_until(lock, deadline, [&] { return controls == 0; });
  }
  mutable std::mutex mutex;
  std::condition_variable changed;
  NativeFrameOutputCapture capture;
  std::filesystem::path path;
  size_t controls = 0;
  bool readback_returned = false, presentation_returned = false;
  bool enqueue_failed = false;
};

std::mutex installations_mutex;
std::map<KernelState*, std::weak_ptr<Transport>> installations;
}  // namespace

struct NativeSystemGraphicsTransport::State {
  struct SharedObservation {
    NativeSystemUiServiceContext context;
    NativeSystemSharedGraphicsState graph;
    object_ref<XEvent> startup_event;
    uint32_t startup_task = 0;
    bool device_attempted = false, device_owned = false;
    bool dc_attempted = false, dc_owned = false, proven = false;
    // No native Release in a deleter. These bounded real native references
    // survive until actual stopped Kernel memory reclamation.
  };
  struct TaskReference {
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    uint32_t handle = 0;
    bool owned = false, close_attempted = false;
  };
  class PlaneAuthority final : public ui::NativeUiPlaneAuthority {
   public:
    PlaneAuthority(std::weak_ptr<State> state, ui::NativeUiPlaneOwner owner)
        : state_(std::move(state)), owner_(owner) {}
    ui::NativeUiPlaneOwner owner() const noexcept override { return owner_; }
    bool IsLive() const noexcept override;

   private:
    std::weak_ptr<State> state_;
    ui::NativeUiPlaneOwner owner_;
  };
  class NonmodalEpoch final : public ui::NativeUiPlaneImageEpoch {
   public:
    NonmodalEpoch(std::weak_ptr<State> state,
                  const NativeNonmodalFrameWitness& e, uint64_t backend)
        : state_(std::move(state)),
          owner_(e.owner()),
          backend_(backend),
          foreground_(e.foreground_generation()),
          modal_known_(e.modal_visibility_known()),
          modal_sequence_(e.modal_visibility_sequence()),
          modal_foreground_(e.modal_foreground_generation()) {}
    bool IsLive() const noexcept override;
    bool IsRevoked() const noexcept override {
      return revoked_.load(std::memory_order_acquire);
    }
    void Revoke() noexcept { revoked_.store(true, std::memory_order_release); }
    bool Matches(const NativeNonmodalFrameWitness& e, uint64_t backend) const {
      return !IsRevoked() && owner_ == e.owner() && backend_ == backend &&
             foreground_ == e.foreground_generation() &&
             modal_known_ == e.modal_visibility_known() &&
             modal_sequence_ == e.modal_visibility_sequence() &&
             modal_foreground_ == e.modal_foreground_generation();
    }
    uint64_t foreground() const { return foreground_; }

   private:
    std::weak_ptr<State> state_;
    const memory::PhysicalAllocationOwner owner_;
    const uint64_t backend_, foreground_;
    const bool modal_known_;
    const uint64_t modal_sequence_, modal_foreground_;
    std::atomic<bool> revoked_{false};
  };
  struct Operation {
    // Host-only scope; also protects epoch metadata reads during teardown.
    std::shared_ptr<State> state;
    NativeSystemUiServiceContext context;
    ~Operation() {
      if (!state) {
        return;
      }
      context = {};  // All admitted holders drop while the call still counts.
      std::lock_guard lock(state->mutex);
      --state->active_calls;
      state->changed.notify_all();
    }
  };
  mutable std::mutex mutex;
  std::mutex retirement;
  std::condition_variable changed;
  NativeSystemUiServiceContext context;
  Snapshot status;
  gpu::GpuBackendStopProof stop_proof;
  std::shared_ptr<NativeSystemGraphicsNotifications> notifications;
  std::shared_ptr<NativeSystemUiNotifications> visibility;
  std::shared_ptr<NativeNotificationPresentationObserver> nonmodal;
  std::optional<NativeNonmodalFrameWitness> source_nonmodal;
  std::shared_ptr<NonmodalEpoch> nonmodal_epoch;
  std::shared_ptr<const ui::NativeUiPlaneImagePermission> nonmodal_permission;
  uint64_t nonmodal_revision = 0;
  // Last real completed Source requested registers, not physical scanout.
  std::optional<gpu::NativeOverlayState> overlay_requested;
  uint64_t overlay_requested_foreground = 0;
  uint64_t presentation_revision = 0;
  std::shared_ptr<NativeUiBackendRoute> ui_route;
  uint64_t visibility_sequence = 0, visibility_foreground = 0;
  bool visibility_known = false, visibility_value = false,
       visibility_revoked = false;
  std::shared_ptr<NativeGraphicsDeviceBinding> binding;
  NativeSystemSharedGraphicsState graph;
  std::shared_ptr<SharedObservation> publication;
  bool startup_initial_zero = false;
  std::shared_ptr<NativeSystemFrameDelegation> delegation;
  std::shared_ptr<const gpu::NativeFrameExecutionRequest> request;
  std::shared_ptr<const gpu::NativeSystemTitleInput> title_input_reservation;
  std::shared_ptr<const NativeFrameSource> source;
  std::shared_ptr<const NativeSystemCompletedFrameRecord> record;
  gpu::NativeFrameExecutionResult result;
  std::shared_ptr<const ui::NativeUiPlaneAuthority> plane;
  std::vector<std::shared_ptr<TaskReference>> task_references;
  size_t active_calls = 0;
  bool result_returned = false, attached = false, attempted = false,
       poisoned = false;
  bool external_closed = false;
  // Default-off diagnostic selection is copied once at installation. No
  // callback captures this State or any Kernel/Request/Session holder.
  std::filesystem::path output_capture_base;
  uint64_t output_capture_source = 0;
  bool output_capture_b_down = false;
  std::optional<NativeInputDriver::BDownCaptureObservation>
      output_capture_b_event;
  bool output_capture_claimed = false;
  std::shared_ptr<OutputCaptureObservation> output_capture;
  NotificationRenderProbeSnapshot render_probe;
  uint64_t latest_frozen_source = 0;
  uint32_t render_probe_polls = 0;
  NotificationRenderProbeMode render_probe_mode =
      NotificationRenderProbeMode::kFirstEligible;
  std::shared_ptr<OutputCaptureObservation> render_probe_capture;

  std::unique_ptr<Operation> Acquire(const std::shared_ptr<State>& self) {
    auto op = std::make_unique<Operation>();
    std::lock_guard lock(mutex);
    if (status.stopping || status.retired ||
        status.mode == Mode::kUninstalled || !context.kernel ||
        active_calls == SIZE_MAX) {
      return {};
    }
    op->context = context;
    op->state = self;
    ++active_calls;
    return op;
  }
  struct UiObservation {
    NativeUiBackendRoute::Cohort cohort;
    NativeUiBackendRoute::Input input = NativeUiBackendRoute::Input::kUnknown;
    gpu::CommandProcessor* cp = nullptr;
  };
  UiObservation ObserveUi(const Operation& op) {
    UiObservation result;
    uint64_t epoch;
    {
      std::lock_guard lock(mutex);
      if (external_closed || status.stopping || !status.ready || poisoned ||
          visibility_revoked || status.mode != Mode::kProduction ||
          stop_proof.stopped() || !ui_route) {
        return result;
      }
      epoch = status.backend_epoch;
    }
    auto* emulator = op.context.kernel->emulator();
    const auto fg = emulator->title_execution_state().snapshot();
    if (!emulator->OwnsTitleExecution(op.context.kernel) ||
        fg.phase != TitleExecutionState::Phase::kRunning) {
      return result;
    }
    const auto title = op.context.kernel->GetExecutableModule();
    auto* graphics = emulator->graphics_system();
    auto* cp = graphics ? graphics->command_processor() : nullptr;
    const auto capacity = cp ? cp->GetNativeSystemFrameCapacity()
                             : gpu::NativeSystemFrameCapacity{};
    if (!title || !cp || !SnapshotBackend(capacity) ||
        capacity.backend_epoch != epoch) {
      return result;
    }
    const auto root = op.context.domain->root();
    result.cohort = {
        op.context.domain->id(), root.generation,          epoch,
        fg.generation,           title->load_generation(), root.hmodule,
        title->hmodule_ptr()};
    result.cp = cp;
    using Availability = gpu::NativeSystemTitleInputProvider::Availability;
    const auto observed = cp->ObserveNativeSystemTitleInput(
        epoch, fg.generation, title->hmodule_ptr(), title->load_generation());
    result.input = observed == Availability::kReady
                       ? NativeUiBackendRoute::Input::kReady
                   : observed == Availability::kProvenNoInitialSwap
                       ? NativeUiBackendRoute::Input::kProvenNoInitialSwap
                       : NativeUiBackendRoute::Input::kUnknown;
    return result;
  }
  NativeUiBackendRoute::Admission AdmitUi(const std::shared_ptr<State>& self,
                                          bool allow_host) {
    auto op = Acquire(self);
    if (!op) {
      return {};
    }
    // A selected menu action can open an existing host settings/confirmation
    // modal before releasing its action lease. Its normal blocker keeps new
    // UI-creating admission denied until that child modal closes.
    const auto* input = op->context.kernel->emulator()->input_system();
    if (!input || input->IsUIInputBlocked()) {
      return {};
    }
    const auto observed = ObserveUi(*op);
    std::shared_ptr<NativeUiBackendRoute> route;
    {
      std::lock_guard lock(mutex);
      route = ui_route;
    }
    if (!route || !observed.cohort.valid()) {
      return {};
    }
    auto admission = route->Admit(observed.cohort, observed.input, allow_host);
    // A selected action installs its child modal's blocker before releasing
    // the parent lease. Recheck AFTER route selection so that handoff cannot
    // pass through ObserveUi with a previously-false blocker sample.
    if (input->IsUIInputBlocked()) {
      if (admission.host) {
        admission.host->Cancel();
      }
      return {};  // Keep any native seal, but do not create a UI.
    }
    // A real Stage that won before the host lease was reserved invalidates the
    // no-initial-Swap proof. Once revalidated, a later first Swap cannot switch
    // this dialog's already-reserved route.
    if (admission.host) {
      using Availability = gpu::NativeSystemTitleInputProvider::Availability;
      const auto& c = observed.cohort;
      const auto capacity = observed.cp->GetNativeSystemFrameCapacity();
      if (!SnapshotBackend(capacity) ||
          capacity.backend_epoch != c.backend_epoch ||
          observed.cp->ObserveNativeSystemTitleInput(
              c.backend_epoch, c.foreground_generation, c.title_hmodule,
              c.title_load_generation) != Availability::kProvenNoInitialSwap) {
        admission.host->Cancel();
        return {};
      }
    }
    return admission;
  }
  std::shared_ptr<NativeUiBackendRoute::HostLease> CurrentHostUi(
      const std::shared_ptr<State>& self) {
    auto op = Acquire(self);
    if (!op) {
      return {};
    }
    const auto observed = ObserveUi(*op);
    std::shared_ptr<NativeUiBackendRoute> route;
    {
      std::lock_guard lock(mutex);
      route = ui_route;
    }
    return route && observed.cohort.valid()
               ? route->CurrentHost(observed.cohort)
               : nullptr;
  }
  void Reject(Blocker reason, bool revoke = false) noexcept {
    std::shared_ptr<NativeSystemFrameDelegation> owner;
    try {
      {
        std::lock_guard lock(mutex);
        status.blocker = reason;
        status.ready = false;
        TRACE_TRANSPORT(context, "transport_blocked",
                        {"blocker", uint32_t(reason)},
                        {"revoke_requested", revoke},
                        {"backend_epoch", status.backend_epoch},
                        {"source_serial", status.source_serial});
        owner = delegation;
      }
      if (revoke) {
        std::shared_ptr<NativeSystemAllocationAuthority> allocation;
        {
          std::lock_guard lock(mutex);
          poisoned = true;
          if (nonmodal_epoch) {
            nonmodal_epoch->Revoke();
          }
          allocation = context.allocation;
        }
        if (owner) {
          owner->session()->Revoke();
        }
        if (allocation) {
          allocation->CloseAdmission();
        }
      }
    } catch (...) {
      FailStop();
    }
  }
  std::shared_ptr<TaskReference> CurrentTask(XThread& thread) {
    auto ref = std::make_shared<TaskReference>();
    {
      std::lock_guard lock(mutex);
      ref->calls = context.catalog->calls();
      task_references.push_back(ref);
    }
    // Retain before actual public GetCurrentTask's AddRef can happen/throw.
    const auto value = ref->calls->ExecuteDeferredSystemFunction(
        thread, "native-system-current-task", {});
    const uint32_t handle = uint32_t(value.return_value);
    // Public1B6 ORs tag1 into a null Task too. That value is not an owned
    // reference and must never be sent to the native Close wrapper.
    if (!value.succeeded || handle < 0x1000 || (handle & 3) != 1) {
      return {};
    }
    ref->handle = handle;
    ref->owned = true;
    return ref;
  }
  bool CloseTask(XThread& thread, const std::shared_ptr<TaskReference>& ref) {
    if (!ref || !ref->owned || ref->close_attempted) {
      return false;
    }
    ref->close_attempted = true;
    const std::array args{ref->handle};
    const auto closed = ref->calls->ExecuteDeferredSystemFunction(
        thread, "native-system-close-task", args);
    if (!closed.succeeded) {
      return false;
    }  // Never replay an uncertain Close.
    ref->owned = false;
    std::lock_guard lock(mutex);
    std::erase(task_references, ref);
    return true;
  }
};

bool NativeSystemGraphicsTransport::State::NonmodalEpoch::IsLive()
    const noexcept {
  try {
    if (IsRevoked()) {
      return false;
    }
    const auto state = state_.lock();
    if (!state) {
      return false;
    }
    auto op = state->Acquire(state);
    if (!op) {
      return false;
    }
    std::shared_ptr<NativeNotificationPresentationObserver> observer;
    std::shared_ptr<NativeSystemUiNotifications> visibility;
    {
      std::lock_guard lock(state->mutex);
      if (state->status.mode != Mode::kProduction || state->poisoned ||
          state->visibility_revoked || state->nonmodal_epoch.get() != this ||
          state->status.backend_epoch != backend_ ||
          state->stop_proof.stopped() ||
          op->context.allocation->owner() != owner_) {
        return false;
      }
      observer = state->nonmodal;
      visibility = state->visibility;
    }
    if (!observer || !observer->IsAccepting() || !visibility ||
        !op->context.allocation->IsAccepting() ||
        !op->context.allocation->MatchesDomain(op->context.domain)) {
      return false;
    }
    const auto* emulator = op->context.kernel->emulator();
    const auto title = emulator->title_execution_state().snapshot();
    if (!emulator->OwnsTitleExecution(op->context.kernel) ||
        title.phase != TitleExecutionState::Phase::kRunning ||
        title.generation != foreground_) {
      return false;
    }
    // Read the producer state itself, not the possibly delayed host sink.
    // This read is the bounded paint/publication linearization point and must
    // occur outside all Vulkan backend/submission locks.
    const auto actual = visibility->snapshot();
    return actual.accepting && !actual.poisoned &&
           actual.known == modal_known_ &&
           actual.last_sequence == modal_sequence_ &&
           actual.foreground_generation == modal_foreground_ && !IsRevoked();
  } catch (...) {
    return false;
  }
}

bool NativeSystemGraphicsTransport::State::PlaneAuthority::IsLive()
    const noexcept {
  try {
    const auto state = state_.lock();
    if (!state || !owner_.valid()) {
      return false;
    }
    Operation op;
    std::shared_ptr<NativeGraphicsDeviceBinding> binding;
    std::shared_ptr<NativeSystemUiNotifications> visibility;
    {
      std::lock_guard lock(state->mutex);
      if (state->status.stopping || state->status.retired || state->poisoned ||
          state->visibility_revoked ||
          state->status.mode != Mode::kProduction || !state->context.kernel ||
          state->active_calls == SIZE_MAX ||
          state->stop_proof.backend_epoch() != owner_.backend_epoch ||
          state->stop_proof.stopped()) {
        return false;
      }
      op.context = state->context;
      op.state = state;
      ++state->active_calls;
      binding = state->binding;
      visibility = state->visibility;
    }
    if (!binding || !visibility || !visibility->IsAccepting() ||
        !op.context.allocation->IsAccepting() ||
        op.context.allocation->owner() != owner_.allocation_owner ||
        !op.context.allocation->MatchesDomain(op.context.domain) ||
        !binding->CheckCpuResourceContext()) {
      return false;
    }
    const auto* emulator = op.context.kernel->emulator();
    const auto title = emulator->title_execution_state().snapshot();
    return emulator->OwnsTitleExecution(op.context.kernel) &&
           title.phase == TitleExecutionState::Phase::kRunning &&
           title.generation == owner_.foreground_generation;
  } catch (...) {
    return false;
  }
}

// This cannot be created by callers or observer installation. Returning it is
// the production attestation consumed by Service after actual snapshot support.
class NativeSystemGraphicsTransport::Prepared final
    : public NativeSystemUiPreparedTransport {
 public:
  explicit Prepared(std::shared_ptr<Transport> owner)
      : owner_(std::move(owner)) {}
  gpu::GpuBackendStopProof backend_stop_proof() const override {
    std::lock_guard lock(owner_->state_->mutex);
    return owner_->state_->stop_proof;
  }
  GuestRuntime::ExecutionResult ObserveNativePhase(
      NativeSystemUiStartupPhase phase, const NativeSystemUiServiceContext& c,
      XThread& thread) override {
    auto op = owner_->state_->Acquire(owner_->state_);
    if (!op || !ActualSystem(c, thread) || c.catalog != op->context.catalog ||
        c.allocation != op->context.allocation ||
        c.domain != op->context.domain) {
      return Fail("Native phase does not belong to the prepared cohort");
    }
    if (phase == NativeSystemUiStartupPhase::kSharedGraphics &&
        !owner_->ObserveSharedDevicePublication(c, thread)) {
      owner_->state_->Reject(Blocker::kSharedGraphObservation, true);
      return Fail("Actual early native publication handoff failed");
    }
    const bool fe_present = phase != NativeSystemUiStartupPhase::kFeRegistry ||
                            NativeFeRegistryPresent(c);
    if (phase == NativeSystemUiStartupPhase::kFeRegistry) {
      std::lock_guard lock(owner_->state_->mutex);
      owner_->state_->status.native_publication.fe_registry_linked = fe_present;
    }
    if (!fe_present) {
      owner_->state_->Reject(Blocker::kFeRegistryObservation);
      return Fail(
          "Actual native Get(FE) does not resolve the initialized root "
          "callback");
    }
    return Ok();  // Service separately validates the real worker/config/input
                  // objects.
  }
  std::shared_ptr<void> AcquireSharedGraphicsObservation(
      XThread& thread) override {
    auto op = owner_->state_->Acquire(owner_->state_);
    if (!op || !ActualSystem(op->context, thread) ||
        thread.thread_id() != op->context.allocation->bootstrap_thread_id()) {
      return {};
    }
    const auto& c = op->context;
    std::shared_ptr<State::SharedObservation> existing;
    bool cold = false;
    std::shared_ptr<NativeSystemGraphicsNotifications> notifications;
    {
      std::lock_guard lock(owner_->state_->mutex);
      cold = owner_->state_->startup_initial_zero;
      existing = owner_->state_->publication;
      notifications = owner_->state_->notifications;
    }
    if (existing) {
      return existing->proven ? existing : nullptr;
    }
    if (!cold || !notifications || !notifications->IsRegistered() ||
        !NativeFeRegistryPresent(c)) {
      return {};
    }
    auto* memory = c.kernel->memory();
    uint32_t task = 0, flag_2190 = 0, flag_2194 = 0;
    bool event_readable = false;
    {
      auto lock = global_critical_region::AcquireDirect();
      task = ReadWord(*memory, c.catalog->symbols().manager_startup_task_cell)
                 .value_or(0);
      flag_2190 = ReadWord(*memory, 0x81AA2190).value_or(0);
      flag_2194 = ReadWord(*memory, 0x81AA2194).value_or(0);
      event_readable = memory->QueryRangeAccess(0x81AA21A4, sizeof(X_KEVENT)) !=
                       memory::PageAccess::kNoAccess;
    }
    {
      std::lock_guard lock(owner_->state_->mutex);
      auto& metadata = owner_->state_->status.native_publication;
      metadata.startup_task = task;
      metadata.startup_flag_2190 = flag_2190;
      metadata.startup_flag_2194 = flag_2194;
    }
    if (task < 0x1000 || (task & 3) != 1 || flag_2190 != 1 || flag_2194 != 1 ||
        !event_readable) {
      return {};
    }
    const std::array task_args{task};
    const auto status = c.catalog->calls()->ExecuteDeferredSystemFunction(
        thread, "native-system-task-status", task_args);
    {
      std::lock_guard lock(owner_->state_->mutex);
      owner_->state_->status.native_publication.task_status =
          uint32_t(status.return_value);
    }
    // Native UInt32/Any Task status preserves exact E_PENDING for queued or
    // active states. The flags/event/DC checks here prove readiness separately.
    if (!status.succeeded ||
        (status.return_value != 0 && status.return_value != 1 &&
         status.return_value != X_E_PENDING)) {
      return {};
    }
    auto event = XObject::GetNativeObject<XEvent>(
        c.kernel, memory->TranslateVirtual<X_KEVENT*>(0x81AA21A4),
        EventNotificationObject);
    uint32_t event_type = UINT32_MAX, event_state = 0;
    if (!event) {
      return {};
    }
    event->Query(&event_type, &event_state);
    {
      std::lock_guard lock(owner_->state_->mutex);
      auto& metadata = owner_->state_->status.native_publication;
      metadata.startup_event_observed = true;
      metadata.event_type = event_type;
      metadata.event_state = event_state;
    }
    if (event_type != 0 || !event_state) {
      return {};
    }
    auto graph = c.catalog->ReadSharedGraphicsState();
    if (!c.device_publication ||
        !c.device_publication->MatchesNativeSystemPublication(c, graph)) {
      return {};
    }
    const auto early =
        c.device_publication->native_system_publication_metadata();
    if (!early.addref_attempted || !early.addref_owned || !early.published) {
      return {};
    }
    {
      std::lock_guard lock(owner_->state_->mutex);
      auto& metadata = owner_->state_->status.native_publication;
      metadata.device = graph.device;
      metadata.dc = graph.dc;
      metadata.recording_context = graph.recording_context;
    }
    if (graph.status != NativeSystemSharedGraphicsState::Status::kObserved ||
        !graph.command_pin || !graph.command_pin->valid()) {
      return {};
    }
    for (const auto& allocation : graph.command_pin->allocations()) {
      if (allocation.owner != c.allocation->owner()) {
        return {};
      }
    }
    {
      auto lock = global_critical_region::AcquireDirect();
      // Exact DC constructor and real vtable establish AddRef+4. D3D's own
      // atomic AddRef directly establishes its+3C native reference ABI.
      if (!graph.dc || graph.dc > UINT32_MAX - 4 || !graph.device ||
          graph.device > UINT32_MAX - 0x3C ||
          ReadWord(*memory, graph.dc) != 0x81604CC0 ||
          ReadWord(*memory, 0x81604CC4) != 0x8177B8B8 ||
          memory->QueryRangeAccess(graph.dc + 4, 4) !=
              memory::PageAccess::kReadWrite ||
          memory->QueryRangeAccess(graph.device + 0x3C, 4) !=
              memory::PageAccess::kReadWrite ||
          ReadWord(*memory, graph.dc + 4).value_or(0) == 0 ||
          ReadWord(*memory, graph.dc + 4).value_or(UINT32_MAX) == UINT32_MAX ||
          ReadWord(*memory, graph.device + 0x3C).value_or(0) == 0 ||
          ReadWord(*memory, graph.device + 0x3C).value_or(UINT32_MAX) ==
              UINT32_MAX) {
        return {};
      }
    }
    auto observation = std::make_shared<State::SharedObservation>();
    observation->context = c;
    observation->graph = graph;
    observation->startup_event = event;
    observation->startup_task = task;
    // Adopt the ONE common phase2 native AddRef; never repeat it here.
    observation->device_attempted = early.addref_attempted;
    observation->device_owned = early.addref_owned;
    {
      std::lock_guard lock(owner_->state_->mutex);
      if (owner_->state_->status.stopping || owner_->state_->publication) {
        return {};
      }
      owner_->state_->publication =
          observation;  // BEFORE any native AddRef/exception.
    }
    const auto add_ref = [&](uint32_t entry, uint32_t pointer, bool& attempted,
                             bool& owned) {
      attempted = true;
      {
        std::lock_guard lock(owner_->state_->mutex);
        auto& metadata = owner_->state_->status.native_publication;
        (entry == 0x8183E610 ? metadata.device_addref_attempted
                             : metadata.dc_addref_attempted) = true;
      }
      const std::array args{cpu::GuestCallArgument::UInt32(pointer)};
      const auto result = cpu::ExecuteGuestCall(
          c.kernel->processor(), thread.thread_state(), entry, args);
      owned = result.executed && uint32_t(result.value) >= 2;
      {
        std::lock_guard lock(owner_->state_->mutex);
        auto& metadata = owner_->state_->status.native_publication;
        (entry == 0x8183E610 ? metadata.device_addref_owned
                             : metadata.dc_addref_owned) = owned;
        (entry == 0x8183E610 ? metadata.device_addref_result
                             : metadata.dc_addref_result) =
            uint32_t(result.value);
      }
      return owned;
    };
    if (!add_ref(0x8177B8B8, graph.dc, observation->dc_attempted,
                 observation->dc_owned)) {
      owner_->state_->Reject(Blocker::kSharedGraphObservation, true);
      return {};
    }
    const auto after = c.catalog->ReadSharedGraphicsState();
    if (after.status != NativeSystemSharedGraphicsState::Status::kObserved ||
        after.device != graph.device || after.dc != graph.dc ||
        after.recording_context != graph.recording_context ||
        after.command_buffers != graph.command_buffers || !after.command_pin ||
        !after.command_pin->valid() || !c.allocation->IsAccepting()) {
      owner_->state_->Reject(Blocker::kSharedGraphObservation, true);
      return {};
    }
    // Flags/event are after the real startup callback's object initialization
    // and callback Modify to state-loop. That Task continues; Pending is not
    // mislabeled as whole Task completion. Native system/global references
    // persist in this selected Kernel scope; our real refs additionally retain
    // both objects across binding/readback/Attach without a fictitious lock.
    {
      std::lock_guard lock(owner_->state_->mutex);
      observation->proven = true;
      owner_->state_->status.native_publication.shared_graph_proven = true;
    }
    return observation;
  }
  GuestRuntime::ExecutionResult AttachSharedGraphics(
      const NativeSystemUiServiceContext& c,
      const NativeSystemSharedGraphicsState& graph,
      const std::shared_ptr<NativeGraphicsDeviceBinding>& binding,
      XThread& thread) override {
    if (!ActualSystem(c, thread) ||
        graph.status != NativeSystemSharedGraphicsState::Status::kObserved ||
        !binding || c.device_publication != binding ||
        !binding->MatchesNativeSystemPublication(c, graph) ||
        !binding->CheckCpuResourceContext()) {
      return Fail("Actual shared graphics binding rejected");
    }
    std::lock_guard lock(owner_->state_->mutex);
    if (owner_->state_->status.stopping ||
        owner_->state_->context.allocation != c.allocation) {
      return Fail("Shared graphics attachment stopped");
    }
    const auto& witness = owner_->state_->publication;
    if (!witness || !witness->proven || !witness->device_owned ||
        !witness->dc_owned || witness->graph.device != graph.device ||
        witness->graph.dc != graph.dc ||
        witness->graph.recording_context != graph.recording_context ||
        witness->graph.command_buffers != graph.command_buffers) {
      return Fail(
          "Shared binding does not match retained actual native publication");
    }
    owner_->state_->binding = binding;
    owner_->state_->graph = graph;
    owner_->state_->status.ready = true;
    owner_->state_->status.blocker = Blocker::kNone;
    TRACE_TRANSPORT(c, "prepared_ready",
                    {"backend_epoch", owner_->state_->status.backend_epoch},
                    {"device", graph.device}, {"dc", graph.dc},
                    {"recording_context", graph.recording_context},
                    {"startup_task", witness->startup_task},
                    {"startup_flags_observed", 1},
                    {"startup_event_observed", 1});
    return Ok();
  }
  std::shared_ptr<void> AcquirePublicCallOperation(NativeSystemUiPublicCall,
                                                   XThread& thread) override {
    auto op = owner_->state_->Acquire(owner_->state_);
    if (!op || thread.kernel_state() != op->context.kernel ||
        !XThread::IsInThread(&thread)) {
      return {};
    }
    {
      std::lock_guard lock(owner_->state_->mutex);
      if (owner_->state_->external_closed || !owner_->state_->status.ready ||
          owner_->state_->poisoned ||
          owner_->state_->status.mode != Mode::kProduction) {
        return {};
      }
    }
    return std::shared_ptr<State::Operation>(std::move(op));
  }
  NativeUiBackendRoute::Admission AdmitUiCreating(bool allow_host) override {
    return owner_->state_->AdmitUi(owner_->state_, allow_host);
  }
  std::shared_ptr<NativeUiBackendRoute::HostLease> CurrentHostUi() override {
    return owner_->state_->CurrentHostUi(owner_->state_);
  }
  bool SealHostUiFallback() noexcept override {
    try {
      std::shared_ptr<NativeUiBackendRoute> route;
      {
        std::lock_guard lock(owner_->state_->mutex);
        route = owner_->state_->ui_route;
      }
      if (!route) {
        return true;
      }
      route->BeginStop();
      return route->Drain(std::chrono::steady_clock::now() +
                          std::chrono::seconds(5));
    } catch (...) {
      return false;
    }
  }
  bool AllowPhysicalInput() const noexcept override {
    try {
      auto op = owner_->state_->Acquire(owner_->state_);
      if (!op) {
        return false;
      }
      std::shared_ptr<NativeGraphicsDeviceBinding> binding;
      std::shared_ptr<NativeSystemUiNotifications> visibility;
      {
        std::lock_guard lock(owner_->state_->mutex);
        if (owner_->state_->status.mode != Mode::kProduction ||
            !owner_->state_->status.ready || owner_->state_->poisoned ||
            owner_->state_->visibility_revoked ||
            owner_->state_->stop_proof.stopped()) {
          return false;
        }
        binding = owner_->state_->binding;
        visibility = owner_->state_->visibility;
      }
      // This is a physical driver capability, below native191/196. Requiring
      // a visible plane here deadlocks Guide opening and zeros hidden TITLE
      // state. Native counters select the audience; host modals are applied by
      // InputSystem's physical adapter, independent of native visibility.
      return binding && visibility && visibility->IsAccepting() &&
             op->context.allocation->IsAccepting() &&
             op->context.allocation->MatchesDomain(op->context.domain);
    } catch (...) {
      return false;
    }
  }
  void CloseExternalRequests() noexcept override {
    owner_->CloseExternalRequests();
  }
  void BeginStopTransport() noexcept override { owner_->BeginStop(); }
  bool DrainCallbacksBeforeCpuStop() noexcept override {
    return owner_->Drain();
  }
  bool RetireAfterKernelStop() noexcept override {
    return owner_->RetireAfterKernelStop();
  }

 private:
  std::shared_ptr<Transport> owner_;
};

NativeSystemGraphicsTransport::NativeSystemGraphicsTransport(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemGraphicsTransport::~NativeSystemGraphicsTransport() = default;
void NativeSystemGraphicsTransport::RecordDiagnosticCounter(
    DiagnosticCounter which) noexcept {
  if (!cvars::native_system_first_record_probe ||
      size_t(which) >= hook_counters.size()) {
    return;
  }
  auto& counter = hook_counters[size_t(which)];
  uint64_t value = counter.load(std::memory_order_relaxed);
  while (value != UINT64_MAX &&
         !counter.compare_exchange_weak(value, value + 1,
                                        std::memory_order_relaxed)) {
  }
}
NativeSystemGraphicsTransport::DiagnosticCounters
NativeSystemGraphicsTransport::ReadDiagnosticCounters() noexcept {
  DiagnosticCounters result{};
  if (cvars::native_system_first_record_probe) {
    for (size_t i = 0; i < result.size(); ++i) {
      result[i] = hook_counters[i].load(std::memory_order_relaxed);
    }
  }
  return result;
}
std::shared_ptr<Transport> NativeSystemGraphicsTransport::Create() {
  return std::shared_ptr<Transport>(new Transport(std::make_shared<State>()));
}
std::shared_ptr<Transport> NativeSystemGraphicsTransport::Lookup(
    KernelState* kernel) {
  std::lock_guard lock(installations_mutex);
  const auto found = installations.find(kernel);
  auto result = found == installations.end() ? nullptr : found->second.lock();
  return result && !result->snapshot().retired ? result : nullptr;
}
bool NativeSystemGraphicsTransport::Install(
    const NativeSystemUiServiceContext& c, XThread& thread, Mode mode) {
  if (!ActualSystem(c, thread) || !c.catalog || !c.allocation || !c.domain ||
      !c.dispatcher || c.catalog->allocation() != c.allocation ||
      c.allocation->kernel() != c.kernel ||
      c.allocation->bootstrap_thread_id() != thread.thread_id() ||
      !c.allocation->MatchesDomain(c.domain) ||
      c.dispatcher->domain() != c.domain || !c.dispatcher->IsAccepting() ||
      !ValidateCode(c)) {
    return false;
  }
  const auto self = shared_from_this();
  std::lock_guard install_lock(installations_mutex);
  auto existing = installations[c.kernel].lock();
  if (existing && !existing->snapshot().retired) {
    return false;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.mode != Mode::kUninstalled || state_->status.stopping) {
      return false;
    }
  }
  // One Kernel guard, before publication or native callbacks; no per-frame
  // bag append and no host GPU object in the Kernel resource bag.
  struct KernelRetirement {
    explicit KernelRetirement(std::shared_ptr<Transport> value)
        : owner(std::move(value)) {}
    ~KernelRetirement() {
      if (!owner->RetireAfterKernelStop()) {
        FailStop();
      }
    }
    std::shared_ptr<Transport> owner;
  };
  c.kernel->RetainNativeResourceUntilTeardown(
      std::make_shared<KernelRetirement>(self));
  auto notifications = NativeSystemGraphicsNotifications::Lookup(c.kernel);
  if (!notifications) {
    notifications = NativeSystemGraphicsNotifications::Create(
        {c.catalog, c.domain, c.dispatcher});
  }
  if (!notifications) {
    return false;
  }
  std::shared_ptr<NativeSystemUiNotifications> visibility;
  std::shared_ptr<NativeNotificationPresentationObserver> nonmodal;
  if (mode == Mode::kProduction) {
    const std::weak_ptr<Transport> weak = self;
    visibility = NativeSystemUiNotifications::Create(
        {c.catalog, c.domain,
         [weak](const NativeSystemUiVisibilityEvent& event) {
           if (auto owner = weak.lock()) {
             owner->OnNativeSystemUiVisibility(event);
           }
         },
         [weak] {
           if (auto owner = weak.lock()) {
             owner->OnNativeSystemUiVisibilityRevoked();
           }
         }});
    if (!visibility || !visibility->IsAccepting()) {
      return false;
    }
    // Service owns the one observer before workers; Query, draw and Set must
    // share that same live cohort rather than attempt a second installation.
    nonmodal = NativeNotificationPresentationObserver::LookupForConfig(
        {c.catalog, c.domain});
    TRACE_TRANSPORT(c, "nonmodal_observer_attachment",
                    {"attached", bool(nonmodal)});
    if (!nonmodal) {
      return false;
    }
  }
  const std::weak_ptr<Transport> weak_route = self;
  auto ui_route =
      std::make_shared<NativeUiBackendRoute>([weak_route](bool visible) {
        const auto owner = weak_route.lock();
        if (!owner) {
          return;
        }
        auto op = owner->state_->Acquire(owner->state_);
        if (op) {
          op->context.kernel->BroadcastNotification(kXNotificationSystemUI,
                                                    visible);
        }
      });
  auto* graphics = c.kernel->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  const auto capacity = cp ? cp->GetNativeSystemFrameCapacity()
                           : gpu::NativeSystemFrameCapacity{};
  {
    std::lock_guard lock(state_->mutex);
    state_->context = c;
    if (mode == Mode::kProduction &&
        ((cvars::native_system_ui_output_capture_source > 0 &&
          !cvars::native_system_ui_output_capture_b_down) ||
         (cvars::native_system_ui_output_capture_source == 0 &&
          cvars::native_system_ui_output_capture_b_down)) &&
        !cvars::native_system_ui_output_capture_base.empty()) {
      try {
        std::filesystem::path path =
            cvars::native_system_ui_output_capture_base;
        if (path.is_absolute()) {
          state_->output_capture_base = std::move(path);
          state_->output_capture_source =
              uint64_t(cvars::native_system_ui_output_capture_source);
          state_->output_capture_b_down =
              cvars::native_system_ui_output_capture_b_down;
        }
      } catch (...) {
        // Invalid diagnostic configuration never changes native readiness.
      }
    }
    state_->ui_route = std::move(ui_route);
    state_->notifications = std::move(notifications);
    state_->visibility = std::move(visibility);
    state_->nonmodal = std::move(nonmodal);
    state_->startup_initial_zero =
        ReadWord(*c.kernel->memory(), 0x81AA2190) == 0 &&
        ReadWord(*c.kernel->memory(), 0x81AA2194) == 0 &&
        ReadWord(*c.kernel->memory(), c.catalog->symbols().dc_cell) == 0;
    state_->status.native_publication.cold_zero_observed =
        state_->startup_initial_zero;
    state_->status.mode = mode;
    state_->status.backend_epoch = capacity.backend_epoch;
    state_->stop_proof = capacity.backend_stop_proof;
    state_->status.blocker = mode == Mode::kObservationOnly
                                 ? Blocker::kNativeWriterExclusion
                                 : Blocker::kNone;
  }
  installations[c.kernel] = self;
  return true;
}
bool NativeSystemGraphicsTransport::InstallObservationOnly(
    const NativeSystemUiServiceContext& c, XThread& thread) {
  return Install(c, thread, Mode::kObservationOnly);
}
std::shared_ptr<NativeSystemUiPreparedTransport>
NativeSystemGraphicsTransport::PrepareBeforeWorkers(
    const NativeSystemUiServiceContext& c, XThread& thread) {
  auto* graphics = c.kernel ? c.kernel->emulator()->graphics_system() : nullptr;
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  if (!cp || !SnapshotBackend(cp->GetNativeSystemFrameCapacity()) ||
      !graphics->presenter() || !Install(c, thread, Mode::kProduction)) {
    return {};
  }
  const std::weak_ptr<Transport> weak = shared_from_this();
  const auto guide_requests_allowed = [weak] {
    const auto owner = weak.lock();
    if (!owner) {
      return false;
    }
    auto op = owner->state_->Acquire(owner->state_);
    if (!op) {
      return false;
    }
    const auto* emulator = op->context.kernel->emulator();
    const auto foreground = emulator->title_execution_state().snapshot();
    if (!emulator->OwnsTitleExecution(op->context.kernel) ||
        foreground.phase != TitleExecutionState::Phase::kRunning) {
      return false;
    }
    const auto observed = owner->state_->ObserveUi(*op);
    return observed.cohort.valid() &&
           observed.input != NativeUiBackendRoute::Input::kUnknown;
  };
  if (!cp->EnableNativeSystemTitleInput(
          cp->GetNativeSystemFrameCapacity().backend_epoch)) {
    BeginStop();
    return {};
  }
  if (!c.input || !c.input->EnableGuideIntent(guide_requests_allowed, [weak] {
        const auto owner = weak.lock();
        return owner ? owner->state_->AdmitUi(owner->state_, true)
                     : NativeUiBackendRoute::Admission{};
      })) {
    BeginStop();
    return {};
  }
  // Actual same-byteview backend support is required above. The observer
  // installation never calls this production hardware-variable seam.
  if (!AdvertiseHardwareGraphics(c)) {
    BeginStop();
    return {};
  }
  return std::make_shared<Prepared>(shared_from_this());
}

bool NativeSystemGraphicsTransport::ObserveSharedDevicePublication(
    const NativeSystemUiServiceContext& c, XThread& thread) {
  auto op = state_->Acquire(state_);
  if (!op || !ActualSystem(c, thread) ||
      thread.thread_id() != c.allocation->bootstrap_thread_id() ||
      c.kernel != op->context.kernel || c.catalog != op->context.catalog ||
      c.domain != op->context.domain ||
      c.allocation != op->context.allocation || !c.device_publication) {
    return false;
  }
  const auto graph = c.catalog->ReadSharedGraphicsState();
  if (!c.device_publication->MatchesNativeSystemPublication(c, graph)) {
    return false;
  }
  const auto metadata =
      c.device_publication->native_system_publication_metadata();
  std::lock_guard lock(state_->mutex);
  if (state_->status.stopping || state_->status.retired ||
      (state_->context.device_publication &&
       state_->context.device_publication != c.device_publication)) {
    return false;
  }
  // No other cached identity is replaced. The pointer is one cohort's real
  // staged publication, not a frame/DC/Ready capability.
  state_->context.device_publication = c.device_publication;
  auto& observed = state_->status.native_publication;
  observed.device_addref_attempted = metadata.addref_attempted;
  observed.device_addref_owned = metadata.addref_owned;
  observed.device_addref_result = metadata.addref_result;
  return metadata.published && metadata.addref_owned;
}

NativeSystemGraphicsTransport::QueryResult NativeSystemGraphicsTransport::Query(
    uint32_t mode, uint32_t lr) try {
  RecordDiagnosticCounter(Counter::kQueryTransport);
  auto op = state_->Acquire(state_);
  if (!op) {
    static xe::NativeSystemUiTraceLimiter limiter;
    limiter.Emit("transport", "query_acquire_rejected",
                 {{"return_address", lr}, {"mode", mode}});
    return RejectedQuery(Counter::kQueryAcquire);
  }
  const auto reject = [&](Counter reason) {
    TRACE_TRANSPORT(op->context, "query_rejected", {"reason", uint32_t(reason)},
                    {"return_address", lr}, {"mode", mode});
    return RejectedQuery(reason);
  };
  auto* thread = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  if (mode) {
    return reject(Counter::kQueryMode);
  }
  if (!thread || !NativeCaller(op->context, *thread, lr)) {
    return reject(Counter::kQueryContext);
  }
  if (std::find(kQueryReturns.begin(), kQueryReturns.end(), lr) ==
      kQueryReturns.end()) {
    return reject(Counter::kQueryReturn);
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.mode == Mode::kObservationOnly) {
      return HandledQuery(state_->poisoned || state_->status.first_record ? 0u
                                                                          : 1u);
    }
  }
  if (!PollOnNativeTask(*thread, lr)) {
    TRACE_TRANSPORT(op->context, "query_retirement_busy",
                    {"return_address", lr}, {"thread_id", thread->thread_id()});
    return HandledQuery(0);
  }
  auto* graphics = op->context.kernel->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  const auto capacity = cp ? cp->GetNativeSystemFrameCapacity()
                           : gpu::NativeSystemFrameCapacity{};
  const auto* emulator = op->context.kernel->emulator();
  const auto foreground = emulator->title_execution_state().snapshot();
  const uint64_t input_foreground =
      emulator->OwnsTitleExecution(op->context.kernel) &&
              foreground.phase == TitleExecutionState::Phase::kRunning
          ? foreground.generation
          : 0;
  const auto title_input =
      cp ? cp->GetNativeSystemTitleInput(input_foreground) : nullptr;
  const auto actual_title = op->context.kernel->GetExecutableModule();
  const bool input_ready =
      title_input && title_input->valid() && actual_title &&
      title_input->identity().backend_epoch == capacity.backend_epoch &&
      title_input->identity().title_hmodule == actual_title->hmodule_ptr() &&
      title_input->identity().title_load_generation ==
          actual_title->load_generation();
  bool refresh_text_owner = false;
  if (input_ready && capacity.can_queue() && op->context.input) {
    std::lock_guard lock(state_->mutex);
    refresh_text_owner =
        !state_->status.stopping && !state_->request && state_->binding &&
        state_->status.ready && !state_->poisoned &&
        state_->status.blocker == Blocker::kNone &&
        capacity.backend_epoch == state_->status.backend_epoch;
  }
  // Validate the actual UI Task/scene before admitting a usable frame, outside
  // StateMutex. Busy retirement/capacity polls cannot advance native input and
  // must not repeatedly hash the full native code profile under Global lock.
  // A newly selected user may lack class2, so keep this bootstrap independent
  // of native198/277; the final Query decision below still rechecks all state.
  if (refresh_text_owner) {
    op->context.input->RefreshNativeTextOwner();
  }
  std::unique_lock lock(state_->mutex);
  const uint32_t available =
      uint32_t(!state_->status.stopping && !state_->request &&
               state_->binding && state_->status.ready && !state_->poisoned &&
               state_->status.blocker == Blocker::kNone && input_ready &&
               capacity.can_queue() &&
               capacity.backend_epoch == state_->status.backend_epoch);
  // A successful Query reserves a real immutable input until Set consumes it.
  // A later busy/other Query cannot erase that already-fulfilled promise.
  if (available &&
      (!state_->title_input_reservation ||
       state_->title_input_reservation->identity().backend_epoch !=
           capacity.backend_epoch ||
       state_->title_input_reservation->identity().foreground_generation !=
           input_foreground ||
       state_->title_input_reservation->identity().title_hmodule !=
           actual_title->hmodule_ptr() ||
       state_->title_input_reservation->identity().title_load_generation !=
           actual_title->load_generation())) {
    state_->title_input_reservation = title_input;
  }
  const uint64_t trace_query_key =
      uint64_t(available) | (uint64_t(state_->status.ready) << 1) |
      (uint64_t(state_->status.blocker) << 2) |
      (uint64_t(state_->poisoned) << 8) |
      (uint64_t(bool(state_->request)) << 9) |
      (uint64_t(state_->status.callback_pending) << 10) |
      (uint64_t(capacity.state) << 16);
  static xe::NativeSystemUiTraceLimiter query_limiter;
  if (cvars::native_system_ui_trace) {
    const auto owner = op->context.allocation->owner();
    query_limiter.EmitTransition(
        trace_query_key, "transport", "query_answer",
        {{"kernel_epoch", owner.kernel_epoch},
         {"runtime_module_generation", owner.module_generation},
         {"owner_session_id", owner.session_id},
         {"root_hmodule", owner.module_hmodule},
         {"root_load_generation",
          op->context.allocation->root_load_generation()},
         {"domain_id", op->context.domain->id()}},
        {{"return_address", lr},
         {"thread_id", thread->thread_id()},
         {"available", available},
         {"ready", state_->status.ready},
         {"blocker", uint32_t(state_->status.blocker)},
         {"stopping", state_->status.stopping},
         {"poisoned", state_->poisoned},
         {"source_pending", bool(state_->request)},
         {"callback_pending", state_->status.callback_pending},
         {"backend_epoch", state_->status.backend_epoch},
         {"capacity_backend_epoch", capacity.backend_epoch},
         {"capacity_state", uint32_t(capacity.state)},
         {"capacity_supported", capacity.supported},
         {"snapshot_supported", capacity.immutable_input_snapshot_supported},
         {"can_queue", capacity.can_queue()},
         {"title_input_ready", input_ready},
         {"title_input_swap_serial",
          title_input ? title_input->identity().swap_serial : 0}});
  }
  if (available && state_->render_probe.armed) {
    state_->render_probe.query_reopened_source =
        state_->render_probe.last_retired_source;
  }
  const auto nonmodal = state_->nonmodal;
  lock.unlock();
  if (available && nonmodal) {
    nonmodal->ObserveSuccessfulNativeFrameQuery(
        *thread->thread_state()->context());
  }
  return HandledQuery(available);
} catch (...) {
  state_->Reject(Blocker::kBackendFailure, true);
  return RejectedQuery(Counter::kQueryException);
}

NativeSystemGraphicsTransport::HookResult NativeSystemGraphicsTransport::Set(
    uint32_t descriptor_address, uint32_t lr) try {
  RecordDiagnosticCounter(Counter::kSetTransport);
  auto op = state_->Acquire(state_);
  if (!op) {
    static xe::NativeSystemUiTraceLimiter limiter;
    limiter.Emit(
        "transport", "set_acquire_rejected",
        {{"return_address", lr}, {"descriptor_address", descriptor_address}});
    return RejectedSet(Counter::kSetAcquire);
  }
  TRACE_TRANSPORT(op->context, "set_entered",
                  {"descriptor_address", descriptor_address},
                  {"return_address", lr});
  const auto reject = [&](Counter reason, uint32_t stage = 0) {
    TRACE_TRANSPORT(op->context, "set_rejected", {"reason", uint32_t(reason)},
                    {"stage", stage}, {"return_address", lr});
    return RejectedSet(reason);
  };
  auto* thread = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  if (!thread || !NativeCaller(op->context, *thread, lr)) {
    return reject(Counter::kSetContext, 1);
  }
  if (lr != kSetReturn) {
    return reject(Counter::kSetReturn, 2);
  }
  auto& memory = *op->context.kernel->memory();
  const auto sp = uint32_t(thread->thread_state()->context()->r[1]);
  // Verified helper allocates120Bhex, passesSP+50 and saves callerLR atSP+118.
  if ((sp & 15) || sp > UINT32_MAX - 0x120 || descriptor_address != sp + 0x50) {
    return reject(Counter::kSetStack, 3);
  }
  std::optional<gpu::SystemCommandDescriptor> descriptor;
  uint32_t renderer_lr = 0;
  {
    auto memory_lock = global_critical_region::AcquireDirect();
    if (memory.QueryRangeAccess(descriptor_address, kDescriptorBytes) ==
        memory::PageAccess::kNoAccess) {
      return reject(Counter::kSetDescriptor, 4);
    }
    descriptor = gpu::SystemCommandDescriptor::Copy(
        {memory.TranslateVirtual<const uint8_t*>(descriptor_address),
         kDescriptorBytes});
    const auto caller = ReadWord(memory, sp + 0x118);
    if (!caller) {
      return reject(Counter::kSetFrame, 5);
    }
    renderer_lr = *caller;
  }
  if (renderer_lr != kRecordedSetCaller && renderer_lr != kFailedEndSetCaller &&
      renderer_lr != kClearSetCaller) {
    return reject(Counter::kSetRenderer, 6);
  }
  const auto task = state_->CurrentTask(*thread);
  uint32_t manager_task = 0;
  {
    auto lock = global_critical_region::AcquireDirect();
    manager_task =
        ReadWord(memory,
                 op->context.catalog->symbols().manager_startup_task_cell)
            .value_or(0);
  }
  const bool actual_task = task && task->handle && task->handle == manager_task;
  const uint32_t task_handle = task ? task->handle : 0;
  if (!task || !state_->CloseTask(*thread, task) || !actual_task) {
    state_->Reject(Blocker::kInvalidCaller, true);
    return reject(Counter::kSetTask, 7);
  }
  TRACE_TRANSPORT(
      op->context, "set_native_identity", {"thread_id", thread->thread_id()},
      {"native_task", task_handle}, {"manager_task", manager_task},
      {"return_address", lr}, {"renderer_return_address", renderer_lr},
      {"command_address", descriptor->command_address()},
      {"command_dwords", descriptor->command_dwords()},
      {"texture_address", descriptor->texture_address()},
      {"has_texture", descriptor->has_texture()},
      {"width", descriptor->width()}, {"height", descriptor->height()},
      {"texture_format", descriptor->texture_format()},
      {"empty", descriptor->empty()});
  Mode mode;
  {
    std::lock_guard lock(state_->mutex);
    mode = state_->status.mode;
  }
  if (mode == Mode::kObservationOnly) {
    {
      std::lock_guard lock(state_->mutex);
      if (state_->status.stopping || state_->status.first_record) {
        return reject(Counter::kSetStoppedOrObserved, 8);
      }
      if (descriptor->empty()) {
        // Before any nonempty command record this observer has submitted no
        // native GPU work. Preserve real clear Query->empty Set->Query
        // progress; an empty reset is not the record the diagnostic intends to
        // pause.
        if (state_->status.empty_reset_count == UINT64_MAX) {
          state_->poisoned = true;
          state_->status.blocker = Blocker::kInvalidRecord;
          return reject(Counter::kSetOtherRejected, 9);
        }
        ++state_->status.empty_reset_count;
        state_->status.last_empty_renderer_return_address = renderer_lr;
        return HandledSet(Counter::kSetEmptyAccepted);
      }
    }
    auto first = std::make_shared<FirstRecord>();
    first->descriptor = descriptor->bytes;
    first->owner = op->context.allocation->owner();
    first->root_load_generation =
        op->context.allocation->root_load_generation();
    first->thread_id = thread->thread_id();
    first->native_task = task_handle;
    first->manager_task = manager_task;
    first->renderer_task_cell =
        op->context.catalog->symbols().manager_startup_task_cell;
    first->return_address = lr;
    first->renderer_return_address = renderer_lr;
    first->genuine_set_helper = true;
    first->matching_native_task = true;
    first->native_end_path = renderer_lr == kRecordedSetCaller;
    first->kind = descriptor->empty()         ? FirstRecord::Kind::kEmptyReset
                  : descriptor->has_texture() ? FirstRecord::Kind::kOutput
                                              : FirstRecord::Kind::kCommandOnly;
    {
      auto memory_lock = global_critical_region::AcquireDirect();
      const auto& symbols = op->context.catalog->symbols();
      first->device = ReadWord(memory, symbols.shared_device_cell).value_or(0);
      first->dc = ReadWord(memory, symbols.dc_cell).value_or(0);
      first->recording_context =
          ReadWord(memory, symbols.record_context_cell).value_or(0);
      first->command_dwords = descriptor->command_dwords();
      const auto command =
          CanonicalPhysical(memory, descriptor->command_address());
      const uint64_t count = uint64_t(first->command_dwords) * 4;
      if (command && count && count <= kMaxPrimaryBytes &&
          !(first->command_dwords & 0xFF000000) && *command < 0x20000000 &&
          count <= 0x20000000 - *command) {
        first->command_physical = *command;
        const std::array ranges{
            memory::PhysicalRange{*command, uint32_t(count)}};
        const auto pin = memory.PinOwnedPhysicalRanges(first->owner, ranges);
        if (pin && pin.pin->valid()) {
          for (const auto& allocation : pin.pin->allocations()) {
            if (allocation.owner == first->owner &&
                *command >= allocation.physical_address &&
                uint64_t(*command) + count <=
                    uint64_t(allocation.physical_address) + allocation.length) {
              first->command_allocation = allocation;
              first->command_owned = true;
              first->observed_primary.resize(size_t(count));
              std::memcpy(first->observed_primary.data(),
                          memory.TranslatePhysical(*command), size_t(count));
              break;
            }
          }
        }
      }
      if (descriptor->has_texture()) {
        const auto output =
            CanonicalPhysical(memory, descriptor->texture_address());
        if (output) {
          first->output_physical = *output;
          const std::array ranges{memory::PhysicalRange{*output, 1}};
          const auto pin = memory.PinOwnedPhysicalRanges(first->owner, ranges);
          if (pin && pin.pin->valid()) {
            for (const auto& allocation : pin.pin->allocations()) {
              if (allocation.owner == first->owner &&
                  *output >= allocation.physical_address &&
                  uint64_t(*output) < uint64_t(allocation.physical_address) +
                                          allocation.length) {
                first->output_allocation = allocation;
                first->output_owned = true;
                break;
              }
            }
          }
        }
      }
    }
    // Pin ownership was needed only for the bounded CPU copy. It is not a
    // Source, and no observer pin survives here to masquerade as writer freeze.
    std::lock_guard lock(state_->mutex);
    if (state_->status.stopping || state_->status.first_record) {
      return reject(Counter::kSetStoppedOrObserved, 10);
    }
    state_->status.first_record = std::move(first);
    return HandledSet(Counter::kSetNonemptyAccepted);
  }
  if (mode != Mode::kProduction) {
    return reject(Counter::kSetOtherRejected, 11);
  }
  std::shared_ptr<NativeNotificationPresentationObserver> nonmodal;
  {
    std::lock_guard lock(state_->mutex);
    nonmodal = state_->nonmodal;
  }
  auto nonmodal_set =
      nonmodal ? nonmodal->BeforeNativeSet(*thread->thread_state()->context(),
                                           *descriptor)
               : NativeNotificationPresentationObserver::SetObservation{};
  if (descriptor->empty()) {
    // A genuine empty Set is no new Source/no GPU submission. Existing direct
    // Source work still gates Query until its actual fence+owner retirement.
    // It still consumes this Query's one-Set promise. Drop the unused CPU-only
    // input after releasing the state mutex so the next Query gets Latest.
    std::shared_ptr<const gpu::NativeSystemTitleInput> unused_input;
    {
      std::lock_guard lock(state_->mutex);
      unused_input = std::move(state_->title_input_reservation);
      state_->status.reset_pending = true;
      TRACE_TRANSPORT(op->context, "empty_reset_accepted",
                      {"renderer_return_address", renderer_lr},
                      {"source_pending", bool(state_->request)});
    }
    if (const auto clear = nonmodal_set.CompleteNativeClear()) {
      OnNativeNonmodalFrame(*clear);
    }
    return HandledSet();
  }
  const auto output_provenance = descriptor->has_texture()
                                     ? nullptr
                                     : nonmodal_set.output_provenance();
  const auto control_provenance =
      descriptor->has_texture() ? nullptr : nonmodal_set.control_provenance();
  if (renderer_lr != kRecordedSetCaller || !descriptor->command_dwords() ||
      (!descriptor->has_texture() && !output_provenance &&
       !control_provenance)) {
    // A nonempty maintenance/clear-only program is not proven to be no work.
    // Retain busy/unsupported; never invent a fence or replay the last image.
    state_->Reject(Blocker::kUnsupportedRecordProfile, true);
    return reject(Counter::kSetOtherRejected, 12);
  }
  auto* graphics = op->context.kernel->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  if (!cp || !SnapshotBackend(cp->GetNativeSystemFrameCapacity()) ||
      cp->GetNativeSystemFrameCapacity().backend_epoch !=
          state_->status.backend_epoch) {
    state_->Reject(Blocker::kNativeWriterExclusion, true);
    return reject(Counter::kSetOtherRejected, 13);
  }
  // Source/Plan capture below requests a real complete immutable input view.
  // No all-writer token is created or substituted for asynchronous CPU writes.
  std::shared_ptr<const gpu::NativeSystemTitleInput> title_input;
  {
    std::lock_guard lock(state_->mutex);
    title_input = std::move(state_->title_input_reservation);
  }
  const auto input_foreground =
      op->context.kernel->emulator()->title_execution_state().snapshot();
  const auto actual_title = op->context.kernel->GetExecutableModule();
  // A successful Query reserves these completed CPU bytes for one Set.
  // Withdrawing/replacing the provider cache blocks new leases, not this
  // already-issued immutable lease. Actual owner/stop identity still revokes.
  if (!title_input || !title_input->valid() ||
      !op->context.kernel->emulator()->OwnsTitleExecution(op->context.kernel) ||
      input_foreground.phase != TitleExecutionState::Phase::kRunning ||
      title_input->identity().foreground_generation !=
          input_foreground.generation ||
      title_input->identity().backend_epoch != state_->status.backend_epoch ||
      !actual_title ||
      title_input->identity().title_hmodule != actual_title->hmodule_ptr() ||
      title_input->identity().title_load_generation !=
          actual_title->load_generation()) {
    // This is backpressure/stopped-foreground, not a successful consumed
    // Source. Query cannot reopen without a genuine completed input
    // reservation.
    return reject(Counter::kSetOtherRejected, 25);
  }
  auto delegation =
      op->context.allocation->DelegateCurrentRenderTask(*thread, lr);
  if (!delegation) {
    return reject(Counter::kSetOtherRejected, 14);
  }
  auto invocation = delegation->EnterCurrentTask(*thread, lr);
  if (!invocation) {
    return reject(Counter::kSetOtherRejected, 15);
  }
  auto session = delegation->session();
  std::shared_ptr<NativeGraphicsDeviceBinding> binding;
  NativeSystemSharedGraphicsState graph;
  bool busy = false;
  {
    std::lock_guard lock(state_->mutex);
    busy = bool(state_->request) || state_->status.callback_pending;
    binding = state_->binding;
    graph = state_->graph;
  }
  if (busy) {
    invocation.Complete(true);
    return reject(Counter::kSetOtherRejected, 16);
  }
  if (!binding || !binding->CheckCpuResourceContext()) {
    invocation.Complete(false);
    return reject(Counter::kSetOtherRejected, 17);
  }
  if (session->GetSnapshot().phase ==
      NativeFrameOwnerSession::Phase::kProvisional) {
    auto authority = binding->CreateFrameAuthority(delegation);
    if (!authority ||
        !session->BindGraph(thread->native_frame_thread_identity(),
                            std::move(authority))) {
      invocation.Complete(false);
      return reject(Counter::kSetOtherRejected, 18);
    }
  }
  std::shared_ptr<const ui::NativeUiPlaneAuthority> plane;
  {
    std::lock_guard lock(state_->mutex);
    plane = state_->plane;
  }
  if (plane && !plane->IsLive()) {
    if (graphics && graphics->presenter()) {
      graphics->presenter()->WithdrawNativeUiPlaneAuthority(plane);
    }
    std::lock_guard lock(state_->mutex);
    if (state_->plane == plane) {
      state_->plane.reset();
    }
    plane.reset();
  }
  if (!plane) {
    const auto* emulator = op->context.kernel->emulator();
    const auto foreground = emulator->title_execution_state().snapshot();
    const ui::NativeUiPlaneOwner owner{
        delegation->owner(), state_->stop_proof.backend_epoch(),
        emulator->OwnsTitleExecution(op->context.kernel) &&
                foreground.phase == TitleExecutionState::Phase::kRunning
            ? foreground.generation
            : 0};
    if (owner.valid()) {
      auto authority = std::make_shared<State::PlaneAuthority>(state_, owner);
      auto* graphics = op->context.kernel->emulator()->graphics_system();
      if (graphics && graphics->presenter() &&
          graphics->presenter()->BindNativeUiPlaneAuthority(authority)) {
        std::lock_guard lock(state_->mutex);
        state_->plane = authority;
        plane = authority;
      }
      if (plane) {
        PublishPlaneVisibility();
      }
    }
    // Binding starts hidden. Genuine modal visibility and exact completed
    // nonmodal output have separate publication conditions below.
    // Source/fence validity does not depend on optional display publication.
  }
  auto update = session->BeginUpdate(thread->native_frame_thread_identity());
  if (!update) {
    invocation.Complete(false);
    return reject(Counter::kSetOtherRejected, 19);
  }
  auto completed = std::shared_ptr<NativeSystemCompletedFrameRecord>(
      new NativeSystemCompletedFrameRecord());
  completed->delegation = delegation;
  completed->title_input = std::move(title_input);
  completed->input_provenance =
      NativeSystemCompletedFrameRecord::InputProvenance::kImmutableSnapshot;
  completed->all_writer_exclusion.reset();
  completed->vd_descriptor = descriptor->bytes;
  completed->output_provenance = output_provenance;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->overlay_requested_foreground ==
        completed->title_input->identity().foreground_generation) {
      completed->overlay_input_state = state_->overlay_requested;
    }
  }
  std::shared_ptr<const memory::PhysicalRangePin> output_pin;
  {
    auto memory_lock = global_critical_region::AcquireDirect();
    const auto command =
        CanonicalPhysical(memory, descriptor->command_address());
    const uint64_t bytes = uint64_t(descriptor->command_dwords()) * 4;
    if (!command || !bytes || bytes > kMaxPrimaryBytes ||
        *command >= 0x20000000 || bytes > 0x20000000 - *command) {
      session->Revoke();
      invocation.Complete(false);
      return reject(Counter::kSetOtherRejected, 20);
    }
    const std::array command_range{
        memory::PhysicalRange{*command, uint32_t(bytes)}};
    const auto command_pin =
        memory.PinOwnedPhysicalRanges(delegation->owner(), command_range);
    const auto find = [&](const auto& pin, uint32_t address, uint32_t length)
        -> std::optional<memory::PhysicalAllocationIdentity> {
      for (const auto& a : pin->allocations()) {
        if (a.owner == delegation->owner() && address >= a.physical_address &&
            uint64_t(address) + length <=
                uint64_t(a.physical_address) + a.length) {
          return a;
        }
      }
      return {};
    };
    if (!command_pin) {
      session->Revoke();
      invocation.Complete(false);
      return reject(Counter::kSetOtherRejected, 21);
    }
    const auto command_allocation =
        find(command_pin.pin, *command, uint32_t(bytes));
    if (!command_allocation) {
      session->Revoke();
      invocation.Complete(false);
      return reject(Counter::kSetOtherRejected, 22);
    }
    auto primary = std::make_shared<std::vector<uint8_t>>(size_t(bytes));
    std::memcpy(primary->data(), memory.TranslatePhysical(*command),
                size_t(bytes));
    std::optional<uint32_t> output;
    bool control = false;
    if (descriptor->has_texture()) {
      output = CanonicalPhysical(memory, descriptor->texture_address());
    } else {
      // Same immutable primary will be independently sealed by the complete
      // Source builder. This parse names a CANDIDATE only: actual owner and
      // allocation extent must still be obtained from real Memory pins.
      const auto analyzed = gpu::NativeFrameCommandPlan::AnalyzeWithTitleInput(
          *primary, completed->title_input);
      control = analyzed && analyzed.plan->is_overlay_control();
      if (control && (!control_provenance || output_provenance)) {
        session->Revoke();
        invocation.Complete(false);
        return reject(Counter::kSetOtherRejected, 20);
      }
      const bool has_overlay =
          analyzed && bool(analyzed.plan->native_overlay_program());
      bool state_checked = false, requested_valid = false, current_valid = false;
      uint32_t current_known = 0, current_surface = 0, requested_surface = 0;
      if (has_overlay) {
        const auto requested = analyzed.plan->native_overlay_program()->Apply(
            completed->overlay_input_state.value_or(gpu::NativeOverlayState{}));
        const auto current =
            analyzed.plan->native_overlay_program()->Apply({});
        state_checked = true;
        requested_valid = requested.valid();
        current_valid = current.valid();
        current_known = current.known_mask;
        current_surface = current.surface_address;
        requested_surface = requested.surface_address;
        if (!control && requested_valid && current_valid &&
            current.Has(gpu::NativeOverlayState::kGeometryKnown) &&
            requested.surface_address == current.surface_address) {
          output = current.surface_address;
        }
      }
      const auto* first_draw =
          analyzed && !analyzed.plan->inventory().draws.empty()
              ? &analyzed.plan->inventory().draws.front()
              : nullptr;
      // State fields are observations only when state_checked is true; a
      // rejected Plan exposes no partly validated draw inventory or authority.
      TRACE_TRANSPORT(
          op->context, "native_output_candidate_analysis",
          {"accepted", bool(analyzed)}, {"control_only", control},
          {"error", uint32_t(analyzed.error)},
          {"error_dword", analyzed.error_dword_offset},
          {"error_register", analyzed.error_register},
          {"overlay_program", has_overlay}, {"state_checked", state_checked},
          {"requested_valid", requested_valid},
          {"current_valid", current_valid},
          {"current_known_mask", current_known},
          {"current_surface", current_surface},
          {"requested_surface", requested_surface},
          {"candidate_selected", bool(output)},
          {"parsed_draw_count",
           analyzed ? analyzed.plan->inventory().draws.size() : 0},
          {"parsed_first_draw_offset",
           first_draw ? first_draw->packet_dword_offset : UINT32_MAX},
          {"parsed_first_draw_initiator",
           first_draw ? first_draw->initiator : 0});
    }
    std::optional<memory::PhysicalAllocationIdentity> destination_allocation;
    if (!control) {
      if (!output || !*output || *output >= 0x20000000) {
        session->Revoke();
        invocation.Complete(false);
        return reject(Counter::kSetOtherRejected, 20);
      }
      const std::array output_range{memory::PhysicalRange{*output, 1}};
      const auto destination_pin =
          memory.PinOwnedPhysicalRanges(delegation->owner(), output_range);
      if (!destination_pin) {
        session->Revoke();
        invocation.Complete(false);
        return reject(Counter::kSetOtherRejected, 21);
      }
      destination_allocation = find(destination_pin.pin, *output, 1);
      if (!destination_allocation) {
        session->Revoke();
        invocation.Complete(false);
        return reject(Counter::kSetOtherRejected, 22);
      }
      output_pin = destination_pin.pin;
    }
    if (control) {
      completed->control_provenance = control_provenance;
    }
    completed->immutable_primary = primary;
    completed->full_record_pin = command_pin.pin;
    auto& record = completed->record;
    record.primary = *primary;
    record.device = graph.device;
    record.dc = graph.dc;
    record.record_physical_address = command_allocation->physical_address;
    record.record_allocation_bytes = command_allocation->length;
    record.primary_physical_address = *command;
    record.native_end_completed = true;
    record.output_physical_address = control ? 0 : *output;
    // Native-overlay origin retains the genuine absent optional CPU alias.
    record.output_cpu_address = descriptor->texture_address();
    record.output_allocation_bytes =
        control ? 0
                : uint32_t(uint64_t(destination_allocation->physical_address) +
                           destination_allocation->length - *output);
  }
  std::optional<NativeInputDriver::BDownCaptureObservation>
      b_down_before_freeze;
  bool inspect_b_down = false;
  {
    std::lock_guard lock(state_->mutex);
    inspect_b_down =
        state_->output_capture_b_down && !state_->output_capture_claimed &&
        !state_->output_capture_source && !state_->output_capture_base.empty();
  }
  if (inspect_b_down && op->context.input) {
    b_down_before_freeze = op->context.input->GetBDownCaptureObservation();
  }
  auto request =
      PrepareNativeSystemFrameExecutionRequest(op->context.kernel, completed);
  if (!request ||
      request->input_provenance() !=
          gpu::NativeFrameInputProvenance::kImmutableSnapshot ||
      !request->input_snapshot() || !request->input_snapshot()->valid()) {
    invocation.Complete(false);
    return reject(Counter::kSetOtherRejected, 23);
  }
  auto source = session->SourceForExecutionRequest(
      thread->native_frame_thread_identity(), *request);
  if (!source) {
    invocation.Complete(false);
    return reject(Counter::kSetOtherRejected, 24);
  }
  const bool frozen_control = request->plan()->is_overlay_control();
  const auto frozen_identity = request->identity();
  const auto frozen_backend_epoch = request->backend_epoch();
  auto nonmodal_witness = nonmodal_set.CompleteSource(frozen_identity);
  // Both samples are diagnostic POD from existing native controller reads.
  // The first precedes Freeze. Matching after Freeze excludes a released B,
  // owner/registration change, hide/show or revoked stream during capture.
  // Commands may have been generated before the observed button; no causal
  // animation-phase or Title-pixel claim follows from this boundary.
  std::optional<NativeInputDriver::BDownCaptureObservation> b_down_after_freeze;
  if (b_down_before_freeze && op->context.input) {
    b_down_after_freeze = op->context.input->GetBDownCaptureObservation();
  }
  bool selected_b_down = false;
  if (request->output_layout()) {
    const auto& layout = *request->output_layout();
    TRACE_TRANSPORT(op->context, "source_output_layout",
                    {"source_serial", frozen_identity.source_serial},
                    {"backend_epoch", frozen_backend_epoch},
                    {"origin", uint32_t(layout.origin)},
                    {"width", layout.width}, {"height", layout.height},
                    {"pitch", layout.pitch},
                    {"required_bytes", layout.required_bytes},
                    {"output_address", request->output_range().address},
                    {"output_length", request->output_range().length});
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->delegation = delegation;
    state_->request = std::move(request);
    state_->source = std::move(source);
    state_->record = std::move(completed);
    state_->source_nonmodal = std::move(nonmodal_witness);
    state_->result = {};
    state_->result_returned = false;
    state_->attached = false;
    state_->status.source_serial = state_->source->serial();
    state_->latest_frozen_source = state_->source->serial();
    state_->status.blocker = Blocker::kNone;
    auto& probe = state_->render_probe;
    const auto& layout = state_->request->output_layout();
    if (probe.armed && !probe.closed && probe.queue_entered &&
        !probe.selected_source && !probe.failed &&
        frozen_identity.source_serial > probe.queue_enter_source &&
        (state_->render_probe_mode ==
             NotificationRenderProbeMode::kFirstEligible ||
         frozen_identity.source_serial == 256) &&
        layout &&
        layout->origin ==
            gpu::NativeFrameOutputLayout::Origin::kNativeOverlayResolve &&
        layout->overlay_state && layout->overlay_presentation &&
        layout->overlay_state->Has(gpu::NativeOverlayState::kEnable |
                                   gpu::NativeOverlayState::kAlphaKnown) &&
        layout->overlay_state->enable && state_->source_nonmodal &&
        state_->source_nonmodal->kind() ==
            NativeNonmodalFrameWitness::Kind::kEligibleDrawCandidate &&
        state_->source_nonmodal->source() == frozen_identity) {
      uint64_t retained = uint64_t(layout->required_bytes) * 3 +
                          state_->request->input_snapshot()->byte_size() +
                          state_->request->plan()->words().size() * 4;
      if (state_->request->plan()->title_input())
        retained += state_->request->plan()->title_input()->bytes().size();
      for (const auto& shader : state_->request->shaders()) {
        retained += shader.code.size();
      }
      if (state_->render_probe_mode ==
          NotificationRenderProbeMode::kExactSource256Diagnostic) {
        // The detached full input/primary copies add to the existing budget.
        retained += state_->request->input_snapshot()->byte_size() +
                    state_->source->primary().size();
        for (const auto &shader : state_->request->shaders())
          retained += shader.code.size();
      }
      probe.selected_source = frozen_identity.source_serial;
      probe.width = layout->width;
      probe.height = layout->height;
      probe.retained_bytes = retained;
      if (layout->required_bytes > 16 * 1024 * 1024 ||
          retained > 64 * 1024 * 1024) {
        probe.failed = true;  // One bounded attempt; never select a replacement.
      } else {
        state_->output_capture_source = frozen_identity.source_serial;
      }
    }
    if (probe.armed && !probe.closed && probe.queue_entered &&
        !probe.selected_source && !probe.failed &&
        state_->render_probe_mode ==
            NotificationRenderProbeMode::kExactSource256Diagnostic &&
        frozen_identity.source_serial >= 256) {
      probe.failed = true; // Missed/ineligible exact Source; never substitute.
    }
    if (state_->request->output_layout() && b_down_before_freeze &&
        b_down_after_freeze && *b_down_before_freeze == *b_down_after_freeze &&
        state_->output_capture_b_down && !state_->output_capture_claimed &&
        !state_->output_capture_source && !state_->visibility_revoked &&
        state_->visibility_known && state_->visibility_value &&
        state_->visibility_sequence ==
            b_down_before_freeze->visibility_sequence &&
        state_->visibility_foreground ==
            b_down_before_freeze->foreground_generation &&
        b_down_before_freeze->input_generation ==
            op->context.input->trace_input_generation() &&
        b_down_before_freeze->domain_id == op->context.domain->id() &&
        b_down_before_freeze->root_hmodule ==
            op->context.domain->root().hmodule &&
        b_down_before_freeze->root_load_generation ==
            op->context.domain->root().generation &&
        b_down_before_freeze->foreground_generation ==
            input_foreground.generation) {
      state_->output_capture_source = state_->source->serial();
      state_->output_capture_b_event = b_down_before_freeze;
      selected_b_down = true;
    }
  }
  if (selected_b_down) {
    TRACE_TRANSPORT(
        op->context, "output_capture_b_down_selected",
        {"source_serial", frozen_identity.source_serial},
        {"backend_epoch", frozen_backend_epoch},
        {"input_generation", b_down_before_freeze->input_generation},
        {"press_sequence", b_down_before_freeze->press_sequence},
        {"native_user", b_down_before_freeze->native_user},
        {"native_notify_sequence", b_down_before_freeze->visibility_sequence},
        {"foreground_generation", b_down_before_freeze->foreground_generation});
  }
  TRACE_TRANSPORT(
      op->context, "source_frozen",
      {"source_serial", frozen_identity.source_serial},
      {"control_only", frozen_control}, {"backend_epoch", frozen_backend_epoch},
      {"input_provenance",
       uint32_t(gpu::NativeFrameInputProvenance::kImmutableSnapshot)});
  if (!invocation.Complete(true) || !SubmitPrepared()) {
    state_->Reject(Blocker::kBackendFailure, true);
    return reject(Counter::kSetOtherRejected, 25);
  }
  return HandledSet();
} catch (...) {
  state_->Reject(Blocker::kBackendFailure, true);
  return RejectedSet(Counter::kSetException);
}

bool NativeSystemGraphicsTransport::SubmitPrepared() {
  std::shared_ptr<const gpu::NativeFrameExecutionRequest> request;
  KernelState* kernel = nullptr;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.stopping || !state_->request ||
        state_->status.callback_pending) {
      return false;
    }
    request = state_->request;
    kernel = state_->context.kernel;
    state_->status.callback_pending = true;
    state_->attempted = true;
  }
  auto* graphics = kernel->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  if (!cp) {
    std::lock_guard lock(state_->mutex);
    state_->status.callback_pending = false;
    state_->changed.notify_all();
    return false;
  }
  try {
    const auto state = state_;
    TRACE_TRANSPORT(state->context, "source_enqueue",
                    {"source_serial", request->identity().source_serial},
                    {"backend_epoch", request->backend_epoch()});
    cp->ExecuteNativeFrameAsync(
        request,
        [state, request](gpu::NativeFrameExecutionResult result) noexcept {
          std::lock_guard lock(state->mutex);
          if (state->request != request) {
            FailStop();
          }
          TRACE_TRANSPORT(
              state->context, "source_result",
              {"source_serial", request->identity().source_serial},
              {"backend_epoch", request->backend_epoch()},
              {"result_status", uint32_t(result.status)},
              {"failing_stage", uint32_t(result.failing_stage)},
              {"failing_packet_dword", result.failing_packet_dword_offset},
              {"draw_commands", result.draw_commands},
              {"indexed_draw_commands", result.indexed_draw_commands},
              {"copy_operations", result.copy_operations},
              {"clear_operations", result.clear_operations},
              {"written_address", result.written_range.address},
              {"written_length", result.written_range.length},
              {"title_state_restored", result.title_state_restored},
              {"submission", result.completion.submission()},
              {"completion_status", uint32_t(result.completion.status())});
          state->result = std::move(result);
          state->result_returned = true;
          state->status.callback_pending = false;
          state->status.native_submission =
              state->result.completion.submission();
          state->changed.notify_all();
        });
    return true;
  } catch (...) {
    // Enqueue may have become visible. Retain exact Source/lease until actual
    // stop; no CPU rollback, zero-command inference or callback fabrication.
    state_->Reject(Blocker::kBackendFailure, true);
    return false;
  }
}

bool NativeSystemGraphicsTransport::PollOnNativeTask(XThread& thread,
                                                     uint32_t lr) {
  std::shared_ptr<NativeSystemFrameDelegation> delegation;
  std::shared_ptr<const gpu::NativeFrameExecutionRequest> request;
  std::shared_ptr<const NativeFrameSource> source;
  gpu::NativeFrameExecutionResult result;
  std::shared_ptr<OutputCaptureObservation> output_capture;
  std::optional<NativeNonmodalFrameWitness> nonmodal;
  bool attached = false, returned = false;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->request) {
      if (state_->status.reset_pending) {
        state_->status.reset_pending = false;
      }
      return true;
    }
    if (state_->status.callback_pending) {
      return false;
    }
    delegation = state_->delegation;
    request = state_->request;
    source = state_->source;
    result = state_->result;
    returned = state_->result_returned;
    attached = state_->attached;
    output_capture = state_->output_capture;
    nonmodal = state_->source_nonmodal;
  }
  if (!returned || !delegation || !source) {
    return false;
  }
  // Readback owns a legitimate cross-thread Session operation. While either
  // control closure is alive, Query is busy and must not open/complete another
  // native Task scope whose completion requires no active Session operation.
  if (output_capture && !output_capture->Done()) {
    return false;
  }
  auto invocation = delegation->EnterCurrentTask(thread, lr);
  if (!invocation) {
    state_->Reject(Blocker::kInvalidCaller, true);
    return false;
  }
  if (result.status == gpu::NativeFrameExecutionResult::Status::kBusy) {
    // Exact frozen Request is unconsumed; no CPU/PPC replay or new Source.
    if (!invocation.Complete(true)) {
      return false;
    }
    auto* graphics = thread.kernel_state()->emulator()->graphics_system();
    auto* cp = graphics ? graphics->command_processor() : nullptr;
    if (cp && cp->GetNativeSystemFrameCapacity().can_queue()) {
      SubmitPrepared();
    }
    return false;
  }
  auto session = delegation->session();
  const auto identity = result.completion.native_frame_identity();
  if (result.status != gpu::NativeFrameExecutionResult::Status::kSubmitted ||
      !result.title_state_restored || !identity ||
      *identity != request->identity() || !result.completion.submission() ||
      result.completion.backend_epoch() != request->backend_epoch() ||
      result.completion.backend_stop_proof().backend_epoch() !=
          request->backend_epoch()) {
    session->Revoke();
    invocation.Complete(false);
    state_->Reject(Blocker::kBackendFailure, true);
    return false;
  }
  const auto current = thread.native_frame_thread_identity();
  if (!attached) {
    if (!session->AttachGpu(current, source, result.completion) ||
        !session->BeginRetire(current, source)) {
      invocation.Complete(false);
      state_->Reject(Blocker::kBackendFailure, true);
      return false;
    }
    std::lock_guard lock(state_->mutex);
    state_->attached = true;
  }
  const auto completion = result.completion.status();
  TRACE_TRANSPORT(state_->context, "source_fence_observed",
                  {"source_serial", source->serial()},
                  {"backend_epoch", result.completion.backend_epoch()},
                  {"submission", result.completion.submission()},
                  {"completion_status", uint32_t(completion)}, {"attached", 1});
  if (completion == gpu::GpuCompletionStatus::kPending) {
    invocation.Complete(true);
    return false;
  }
  bool start_output_capture = false;
  bool capture_scope_consumed = false;
  std::optional<NativeInputDriver::BDownCaptureObservation>
      b_down_capture_event;
  if (completion == gpu::GpuCompletionStatus::kComplete) {
    std::lock_guard lock(state_->mutex);
    output_capture = state_->output_capture;
    if (state_->render_probe.selected_source == source->serial() &&
        request->output_layout() && !state_->render_probe.failed &&
        !state_->output_capture_claimed) {
      auto& probe = state_->render_probe;
      uint64_t retained = uint64_t(result.written_range.length) * 3 +
          request->input_snapshot()->byte_size() + request->plan()->words().size() * 4;
      if (request->plan()->title_input()) retained += request->plan()->title_input()->bytes().size();
      for (const auto& shader : request->shaders()) retained += shader.code.size();
      if (state_->render_probe_mode ==
          NotificationRenderProbeMode::kExactSource256Diagnostic) {
        retained +=
            request->input_snapshot()->byte_size() + source->primary().size();
        for (const auto &shader : request->shaders())
          retained += shader.code.size();
      }
      probe.retained_bytes = retained;
      if (result.written_range.length > 16 * 1024 * 1024 ||
          probe.retained_bytes > 64 * 1024 * 1024) probe.failed = true;
    }
    if (!state_->status.stopping && !state_->output_capture_claimed &&
        !(state_->render_probe.selected_source == source->serial() &&
          state_->render_probe.failed) &&
        state_->output_capture_source == source->serial() &&
        !state_->output_capture_base.empty()) {
      // Claim once before allocations/enqueue: no filesystem or control retry.
      state_->output_capture_claimed = true;
      b_down_capture_event = state_->output_capture_b_event;
      start_output_capture = true;
    }
  }
  bool exact_source_capture = false;
  if (start_output_capture) {
    {
      std::lock_guard lock(state_->mutex);
      exact_source_capture =
          state_->render_probe.selected_source == source->serial() &&
          state_->render_probe_mode ==
              NotificationRenderProbeMode::kExactSource256Diagnostic;
    }
    try {
      if (request->input_provenance() ==
              gpu::NativeFrameInputProvenance::kImmutableSnapshot &&
          request->output_layout() &&
          gpu::NativeFrameOutputOriginUsesPm4Resolve(
              request->output_layout()->origin)) {
        auto metadata = SnapshotNativeFrameOutputCapture(*request, result);
        if (metadata && b_down_capture_event) {
          const auto& event = *b_down_capture_event;
          const NativeFrameOutputBDownTrigger trigger{
              event.input_generation,
              event.domain_id,
              event.root_load_generation,
              event.foreground_generation,
              event.visibility_sequence,
              event.press_sequence,
              event.binding_epoch,
              event.root_hmodule,
              event.slot,
              event.native_user,
              event.device_handle,
              event.binding_identity,
              event.packet,
              event.buttons};
          if (!AttachNativeFrameOutputPrimary(*metadata, *source, trigger) ||
              !AttachNativeFrameOutputInputs(*metadata, *request)) {
            metadata.reset();
            TRACE_TRANSPORT(state_->context, "output_capture_inputs_refused",
                            {"source_serial", source->serial()},
                            {"primary_bytes", source->primary().size()});
          }
        }
        if (metadata && exact_source_capture &&
            (!source->title_input() ||
             !AttachNativeFrameOutputExactSourceDiagnostic(
                 *metadata, *source,
                 NativeFrameOutputExactSourceDiagnosticTrigger{
                     256, source->title_input()->identity()}) ||
             !AttachNativeFrameOutputInputs(*metadata, *request))) {
          metadata.reset();
          TRACE_TRANSPORT(state_->context,
                          "output_capture_exact_inputs_refused",
                          {"source_serial", source->serial()});
        }
        auto* graphics = thread.kernel_state()->emulator()->graphics_system();
        auto* cp = graphics ? graphics->command_processor() : nullptr;
        if (metadata && cp) {
          output_capture = std::make_shared<OutputCaptureObservation>();
          output_capture->capture = std::move(*metadata);
          auto readback = std::make_shared<OutputCaptureObservation::Control>(
              output_capture);
          auto presentation =
              std::make_shared<OutputCaptureObservation::Control>(
                  output_capture);
          {
            std::lock_guard lock(state_->mutex);
            output_capture->path = state_->output_capture_base;
            // Publish only after both pending controls have been reserved.
            state_->output_capture = output_capture;
            if (state_->render_probe.selected_source == source->serial()) {
              state_->render_probe_capture = output_capture;
            }
          }
          // Complete consumes this scope, including exceptional/failure paths.
          // No CP readback may reserve its Session operation until this exact
          // native Task scope has closed successfully.
          capture_scope_consumed = true;
          const bool scope_completed = invocation.Complete(true);
          TRACE_TRANSPORT(state_->context, "output_capture_scope_completed",
                          {"source_serial", source->serial()},
                          {"backend_epoch", request->backend_epoch()},
                          {"submission", result.completion.submission()},
                          {"completed", scope_completed});
          if (!scope_completed) {
            return false;
          }
          try {
            cp->ReadCompletedNativeFrameAsync(
                request,
                [readback](gpu::NativeFrameReadbackResult value) noexcept {
                  auto& observer = *readback->observation;
                  try {
                    std::lock_guard lock(observer.mutex);
                    CompleteNativeFrameOutputCapture(observer.capture,
                                                     std::move(value));
                  } catch (...) {
                  }
                  value = {};  // Drop completion before returned publication.
                  std::lock_guard lock(observer.mutex);
                  observer.readback_returned = true;
                });
          } catch (...) {
            std::lock_guard lock(output_capture->mutex);
            output_capture->enqueue_failed = true;
          }
          readback.reset();
          try {
            cp->CaptureNativeFramePresentationAsync(
                request,
                [presentation](
                    gpu::NativeFramePresentationStatus value) noexcept {
                  auto& observer = *presentation->observation;
                  try {
                    std::lock_guard lock(observer.mutex);
                    AssociateNativeFrameOutputCapture(observer.capture,
                                                      std::move(value));
                  } catch (...) {
                  }
                  value = {};  // Never publish while a token is still held.
                  std::lock_guard lock(observer.mutex);
                  observer.presentation_returned = true;
                });
          } catch (...) {
            std::lock_guard lock(output_capture->mutex);
            output_capture->enqueue_failed = true;
          }
          presentation.reset();
          TRACE_TRANSPORT(state_->context, "output_capture_queued",
                          {"source_serial", source->serial()},
                          {"backend_epoch", request->backend_epoch()},
                          {"submission", result.completion.submission()});
        }
      }
    } catch (...) {
      // CPU diagnostic preparation failed. The actual native completion and
      // normal Source retirement remain independent of this optional export.
    }
  }
  if (capture_scope_consumed) {
    // Even inline controls or enqueue failures cannot reuse the closed scope.
    // A later genuine native poll re-enters only after both closures are gone.
    return false;
  }
  if (completion != gpu::GpuCompletionStatus::kComplete ||
      !session->RetireComplete(current, source)) {
    invocation.Complete(false);
    state_->Reject(Blocker::kBackendFailure, true);
    return false;
  }
  if (!invocation.Complete(true)) {
    state_->Reject(Blocker::kBackendFailure, true);
    return false;
  }
  const bool control = request->plan()->is_overlay_control();
  const auto requested_overlay =
      control
          ? std::optional(request->plan()->native_overlay_program()->Apply({}))
      : request->output_layout() ? request->output_layout()->overlay_state
                                 : std::nullopt;
  bool overlay_hide = false;
  if (requested_overlay && request->IsPresentationOwnerLive()) {
    const auto& requested = *requested_overlay;
    std::lock_guard lock(state_->mutex);
    if (!state_->status.stopping && !state_->status.retired &&
        !state_->poisoned && state_->request == request && requested.valid()) {
      state_->overlay_requested =
          control ? request->plan()->native_overlay_program()->Apply(
                        state_->overlay_requested.value_or(
                            gpu::NativeOverlayState{}))
                  : requested;
      if (request->plan()->title_input()) {
        state_->overlay_requested_foreground =
            request->plan()->title_input()->identity().foreground_generation;
      }
      // A genuine completed native disable may suppress a prior nonmodal
      // image even for transition-work frames with no positive draw witness.
      // This is desired host-paint state, never a GPU ACK or scanout claim.
      if (requested.Has(gpu::NativeOverlayState::kEnable) && !requested.enable) {
        if (state_->nonmodal_epoch) state_->nonmodal_epoch->Revoke();
        state_->nonmodal_permission.reset();
        state_->nonmodal_epoch.reset();
        overlay_hide = true;
      }
    }
  }
  if (nonmodal) {
    OnNativeNonmodalFrame(*nonmodal, request.get(), &result);
  }
  if (requested_overlay) {
    std::lock_guard lock(state_->mutex);
    if (state_->request == request &&
        (!state_->nonmodal_permission ||
         state_->nonmodal_permission->image() != request->identity())) {
      // A completed display transaction cannot retain pixels authorized only
      // by an older Source. Pending image retention is permitted only after
      // this exact Source obtained a genuine eligible visibility witness.
      if (state_->nonmodal_epoch) state_->nonmodal_epoch->Revoke();
      state_->nonmodal_permission.reset();
      state_->nonmodal_epoch.reset();
      overlay_hide = true;
    }
  }
  if (overlay_hide) {
    PublishPlaneVisibility();
  }
  {
    std::lock_guard lock(state_->mutex);
    auto& probe = state_->render_probe;
    if (probe.armed && !probe.closed && source->serial() > probe.baseline_source) {
      if (probe.completed_sources == 1024) {
        probe.failed = true;
      } else {
        ++probe.completed_sources;
      }
      probe.last_retired_source = source->serial();
      if (requested_overlay) {
        const auto& requested = *requested_overlay;
        if (requested.Has(gpu::NativeOverlayState::kEnable) && !requested.enable) {
          if (!probe.first_disabled_source) probe.first_disabled_source = source->serial();
          if (probe.selected_source && source->serial() > probe.selected_source)
            probe.hidden_after_selected = true;
        }
      }
      if (source->serial() == probe.selected_source) {
        probe.selected_retired = true;
        probe.native_submission = result.completion.submission();
        probe.permission_for_selected = state_->nonmodal_permission &&
            state_->nonmodal_permission->image() == request->identity();
        if (!probe.permission_for_selected) probe.failed = true;
      }
    }
  }
  TRACE_TRANSPORT(state_->context, "source_retired",
                  {"source_serial", source->serial()},
                  {"backend_epoch", result.completion.backend_epoch()},
                  {"submission", result.completion.submission()});
  // Source retirement alone releases guest inputs. Presenter independently
  // keeps any converted host image through its actual UI sampling fence.
  std::shared_ptr<const NativeSystemCompletedFrameRecord> record;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->request != request) {
      FailStop();
    }
    state_->request.reset();
    state_->source.reset();
    state_->source_nonmodal.reset();
    record = std::move(state_->record);
    state_->result = {};
    state_->result_returned = false;
    state_->attached = false;
    state_->status.source_serial = 0;
    state_->status.reset_pending = false;
    if (output_capture && state_->output_capture == output_capture) {
      state_->output_capture.reset();
    }
  }
  if (output_capture) {
    bool saved = false;
    try {
      // Controls are gone. Only owned CPU bytes/proof metadata survive here;
      // this existing admitted hook operation spans the bounded file write.
      bool is_render_probe = false;
      {
        std::lock_guard lock(state_->mutex);
        is_render_probe = state_->render_probe.selected_source == source->serial();
      }
      bool space_allowed = true;
      if (is_render_probe) {
        std::error_code error;
        const auto space = std::filesystem::space(output_capture->path.parent_path(), error);
        const bool exact =
            output_capture->capture.exact_source_diagnostic.has_value();
        space_allowed =
            !error &&
            space.available >
                (uint64_t(20) << 30) + uint64_t(exact ? 64 : 32) * 1024 * 1024;
      }
      saved = space_allowed && output_capture->readback_returned &&
              output_capture->presentation_returned &&
              !output_capture->enqueue_failed &&
              SaveNativeFrameOutputCapture(output_capture->path,
                                           output_capture->capture);
    } catch (...) {
    }
    {
      std::lock_guard lock(state_->mutex);
      if (state_->render_probe.selected_source == source->serial()) {
        state_->render_probe.capture_saved = saved;
        if (!saved) state_->render_probe.failed = true;
      }
    }
    TRACE_TRANSPORT(
        state_->context, "output_capture_finished",
        {"source_serial", source->serial()},
        {"backend_epoch", request->backend_epoch()},
        {"submission", result.completion.submission()},
        {"image_version", output_capture->capture.image_version},
        {"readback_returned", output_capture->readback_returned},
        {"readback_status", output_capture->capture.readback_status},
        {"presentation_returned", output_capture->presentation_returned},
        {"image_associated", output_capture->capture.image_associated},
        {"enqueue_failed", output_capture->enqueue_failed}, {"saved", saved});
    try {
      XELOGI("Native output capture: source={} saved={}", source->serial(),
             saved);
    } catch (...) {
    }
  }
  return true;
}

NativeSystemGraphicsTransport::HookResult NativeSystemGraphicsTransport::Get(
    uint32_t descriptor, uint32_t identifier, uint32_t) {
  auto op = state_->Acquire(state_);
  if (!op) {
    return Hook::kUnhandled;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.mode == Mode::kObservationOnly) {
      return Hook::kUnhandled;
    }
  }
  // Selected direct-only profile: no title checkout exists, so its consumer
  // receives a genuine empty record/id0 and cannot execute direct work twice.
  auto& memory = *op->context.kernel->memory();
  auto lock = global_critical_region::AcquireDirect();
  if (!descriptor || (descriptor & 3) || !identifier || (identifier & 3) ||
      uint64_t(descriptor) + kDescriptorBytes > UINT32_MAX ||
      (uint64_t(descriptor) < uint64_t(identifier) + 4 &&
       uint64_t(identifier) < uint64_t(descriptor) + kDescriptorBytes) ||
      memory.QueryRangeAccess(descriptor, kDescriptorBytes) !=
          memory::PageAccess::kReadWrite ||
      memory.QueryRangeAccess(identifier, 4) !=
          memory::PageAccess::kReadWrite) {
    return Hook::kRejected;
  }
  std::memset(memory.TranslateVirtual(descriptor), 0, kDescriptorBytes);
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(identifier), 0);
  return Hook::kHandled;
}
NativeSystemGraphicsTransport::HookResult
NativeSystemGraphicsTransport::SetIdentifierAddress(uint32_t address,
                                                    uint32_t) {
  auto op = state_->Acquire(state_);
  if (!op) {
    return Hook::kUnhandled;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.mode == Mode::kObservationOnly) {
      return Hook::kUnhandled;
    }
  }
  if (address) {
    auto lock = global_critical_region::AcquireDirect();
    if ((address & 3) || op->context.kernel->memory()->QueryRangeAccess(
                             address, 4) != memory::PageAccess::kReadWrite) {
      return Hook::kRejected;
    }
  }
  std::lock_guard lock(state_->mutex);
  if (state_->status.identifier_binding_generation == UINT64_MAX) {
    return Hook::kRejected;
  }
  state_->status.identifier_address = address;
  ++state_->status.identifier_binding_generation;
  // No ACK/retirement store: CPU skip/reset cannot retire an attempted direct
  // Source or replace its actual same-backend native completion.
  return Hook::kHandled;
}
bool NativeSystemGraphicsTransport::ArmNotificationRenderProbe(
    XThread &bootstrap, const std::filesystem::path &base,
    NotificationRenderProbeMode mode) {
  auto op = state_->Acquire(state_);
  const bool exact =
      mode == NotificationRenderProbeMode::kExactSource256Diagnostic;
  if ((mode != NotificationRenderProbeMode::kFirstEligible && !exact) ||
      cvars::native_system_ui_output_capture_b_down ||
      !cvars::native_system_ui_output_capture_base.empty() ||
      cvars::native_system_ui_output_capture_source != (exact ? 256 : 0))
    return false;
  if (!op || cvars::native_notification_render_probe_report.empty() ||
      cvars::native_system_first_record_probe ||
      NativeSystemPublicFamilyRouteEnabled(NativeSystemPublicFamily::kNotifications) ||
      !XThread::IsInThread(&bootstrap) || bootstrap.kernel_state() != op->context.kernel ||
      bootstrap.thread_id() != op->context.allocation->bootstrap_thread_id() ||
      bootstrap.guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      !base.is_absolute() || base.filename() != "native-notification-render-output") return false;
  std::error_code error;
  if (!std::filesystem::equivalent(base.parent_path(),
          op->context.kernel->emulator()->storage_root(), error) || error) return false;
  const auto space = std::filesystem::space(base.parent_path(), error);
  if (error || space.available <= (uint64_t(20) << 30) +
                                      uint64_t(exact ? 64 : 32) * 1024 * 1024)
    return false;
  auto* emulator = op->context.kernel->emulator();
  const auto foreground = emulator->title_execution_state().snapshot();
  const auto title = op->context.kernel->GetExecutableModule();
  auto* graphics = emulator->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  const auto capacity = cp ? cp->GetNativeSystemFrameCapacity() : gpu::NativeSystemFrameCapacity{};
  const auto input = cp ? cp->GetNativeSystemTitleInput(foreground.generation) : nullptr;
  if (!emulator->OwnsTitleExecution(op->context.kernel) ||
      foreground.phase != TitleExecutionState::Phase::kRunning || !title || !input ||
      !input->valid() || !capacity.supports_immutable_input_admission() ||
      capacity.backend_stop_proof.backend_epoch() != capacity.backend_epoch ||
      capacity.backend_stop_proof.stopped() ||
      input->identity().backend_epoch != capacity.backend_epoch ||
      input->identity().foreground_generation != foreground.generation ||
      input->identity().title_hmodule != title->hmodule_ptr() ||
      input->identity().title_load_generation != title->load_generation()) return false;
  std::lock_guard lock(state_->mutex);
  if (state_->status.mode != Mode::kProduction || !state_->status.ready ||
      state_->status.stopping || state_->poisoned || state_->external_closed ||
      capacity.backend_epoch != state_->status.backend_epoch ||
      state_->stop_proof.stopped() || state_->render_probe.armed || !state_->output_capture_base.empty()) return false;
  if (exact && state_->latest_frozen_source >= 256)
    return false;
  state_->render_probe_mode = mode;
  state_->render_probe.armed = true;
  state_->render_probe.baseline_source = state_->latest_frozen_source;
  state_->render_probe.backend_epoch = capacity.backend_epoch;
  state_->render_probe.foreground_generation = foreground.generation;
  state_->render_probe.title_input_swap = input->identity().swap_serial;
  state_->output_capture_base = base;
  return true;
}
bool NativeSystemGraphicsTransport::ObserveNotificationRenderProbeQueue(
    XThread& worker, bool normal_return) {
  auto op = state_->Acquire(state_);
  if (!op || !XThread::IsInThread(&worker) || worker.kernel_state() != op->context.kernel ||
      worker.guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      !op->context.allocation->IsAccepting() ||
      !op->context.allocation->MatchesDomain(op->context.domain)) return false;
  std::lock_guard lock(state_->mutex);
  auto& probe = state_->render_probe;
  if (!probe.armed || probe.closed || state_->status.stopping || state_->poisoned ||
      (normal_return ? (!probe.queue_entered || probe.queue_returned)
                     : probe.queue_entered)) return false;
  if (normal_return) {
    probe.queue_returned = true;
    probe.queue_return_source = state_->latest_frozen_source;
  } else {
    if (state_->render_probe_mode ==
            NotificationRenderProbeMode::kExactSource256Diagnostic &&
        state_->latest_frozen_source >= 256) {
      probe.failed = true;
      return false;
    }
    probe.queue_entered = true;
    probe.queue_enter_source = state_->latest_frozen_source;
  }
  return true;
}
bool NativeSystemGraphicsTransport::CloseNotificationRenderProbe(XThread& bootstrap) {
  auto op = state_->Acquire(state_);
  if (!op || !XThread::IsInThread(&bootstrap) || bootstrap.kernel_state() != op->context.kernel ||
      bootstrap.thread_id() != op->context.allocation->bootstrap_thread_id()) return false;
  std::lock_guard lock(state_->mutex);
  if (!state_->render_probe.armed) return false;
  state_->render_probe.closed = true;  // No new selection; admitted Source/control still retires normally.
  return true;
}
std::optional<NativeSystemGraphicsTransport::NotificationRenderProbeSnapshot>
NativeSystemGraphicsTransport::PollNotificationRenderProbe(XThread& bootstrap) {
  auto op = state_->Acquire(state_);
  if (!op || !XThread::IsInThread(&bootstrap) || bootstrap.kernel_state() != op->context.kernel ||
      bootstrap.thread_id() != op->context.allocation->bootstrap_thread_id()) return {};
  std::shared_ptr<OutputCaptureObservation> capture;
  NotificationRenderProbeSnapshot result;
  {
    std::lock_guard lock(state_->mutex);
    result = state_->render_probe;
    if (!result.armed || state_->status.stopping || state_->status.retired) return {};
    if (state_->poisoned || state_->stop_proof.stopped() ||
        state_->status.backend_epoch != result.backend_epoch) {
      result.failed = true;
      return result;
    }
    if (result.selected_retired && result.capture_saved && !result.failed &&
        !result.paint_complete_observed &&
        state_->render_probe_polls < 128) {
      capture = state_->render_probe_capture;
      ++state_->render_probe_polls;
    }
  }
  const auto foreground = op->context.kernel->emulator()->title_execution_state().snapshot();
  if (!op->context.kernel->emulator()->OwnsTitleExecution(op->context.kernel) ||
      foreground.phase != TitleExecutionState::Phase::kRunning ||
      foreground.generation != result.foreground_generation ||
      !op->context.allocation->IsAccepting() ||
      !op->context.allocation->MatchesDomain(op->context.domain)) {
    result.failed = true;
    return result;
  }
  auto* graphics = op->context.kernel->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  const auto capacity = cp ? cp->GetNativeSystemFrameCapacity()
                           : gpu::NativeSystemFrameCapacity{};
  if (!capacity.supports_immutable_input_admission() ||
      capacity.backend_epoch != result.backend_epoch ||
      capacity.backend_stop_proof.backend_epoch() != result.backend_epoch ||
      capacity.backend_stop_proof.stopped()) {
    result.failed = true;
    return result;
  }
  if (capture) {
    gpu::NativeFrameSubmissionIdentity identity;
    uint64_t version = 0, backend = 0;
    {
      std::lock_guard lock(capture->mutex);
      result.readback_complete = capture->capture.readback_complete;
      result.readback_status = capture->capture.readback_status;
      identity = capture->capture.identity;
      version = capture->capture.image_version;
      backend = capture->capture.backend_epoch;
      result.failed |= capture->enqueue_failed;
    }
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    if (!presenter || !version || !identity.valid() ||
        identity.owner != op->context.allocation->owner() ||
        identity.source_serial != result.selected_source ||
        backend != result.backend_epoch) {
      result.failed = true;
      return result;
    }
    // Metadata only after Source retirement: retain no Request/Source or
    // PhysicalRangePin. Presenter performs its own lock/liveness/fence checks.
    // The original counted CP controls already dropped every request before
    // RetireComplete, and the detached capture supplies exact image identity.
    const auto paint = presenter->GetNativeUiPlaneStatus(identity, version);
    result.failed |= paint.failed || paint.identity != identity ||
        paint.image_version != version || paint.producer_backend_epoch != backend;
    result.image_ready = paint.image_ready;
    result.presenter_accepted = paint.presenter_accepted;
    result.image_version = paint.image_version;
    result.paint_submission = paint.paint_submission;
    result.paint_backend_epoch = paint.presentation_backend_epoch;
    result.paint_status = uint32_t(paint.paint_completion.status());
    result.paint_complete_observed = !result.failed && result.readback_complete &&
        result.image_ready && result.presenter_accepted && result.image_version &&
        result.paint_submission && result.paint_backend_epoch &&
        paint.paint_completion.status() == gpu::GpuCompletionStatus::kComplete;
    if (result.paint_complete_observed) {
      // The actual completed consumer ticket was observed above. Persist only
      // copied facts: no ticket, publication, request, pin or GPU resource is
      // retained. Later native hide may outlive this host image's residency.
      const auto after = op->context.kernel->emulator()->title_execution_state().snapshot();
      if (after.phase != TitleExecutionState::Phase::kRunning ||
          after.generation != result.foreground_generation ||
          !op->context.kernel->emulator()->OwnsTitleExecution(op->context.kernel) ||
          !op->context.allocation->IsAccepting() ||
          !op->context.allocation->MatchesDomain(op->context.domain)) {
        result.failed = true;
        return result;
      }
      std::lock_guard lock(state_->mutex);
      auto& observed = state_->render_probe;
      if (state_->status.stopping || state_->status.retired || state_->poisoned ||
          state_->stop_proof.stopped() || observed.failed ||
          observed.selected_source != identity.source_serial ||
          observed.backend_epoch != backend ||
          state_->status.backend_epoch != backend ||
          observed.foreground_generation != after.generation ||
          op->context.allocation->owner() != identity.owner) {
        result.failed = true;
        return result;
      }
      if (!observed.paint_complete_observed) {
        observed.readback_complete = result.readback_complete;
        observed.readback_status = result.readback_status;
        observed.image_ready = result.image_ready;
        observed.presenter_accepted = result.presenter_accepted;
        observed.image_version = result.image_version;
        observed.paint_submission = result.paint_submission;
        observed.paint_backend_epoch = result.paint_backend_epoch;
        observed.paint_status = result.paint_status;
        observed.paint_complete_observed = true;
      }
      result = observed;  // Include a hide that raced this exact observation.
    }
  }
  return result;
}

std::shared_ptr<NativeSystemGraphicsNotifications>
NativeSystemGraphicsTransport::notifications() const {
  std::lock_guard lock(state_->mutex);
  return state_->notifications;
}
NativeSystemGraphicsTransport::Snapshot
NativeSystemGraphicsTransport::snapshot() const {
  std::lock_guard lock(state_->mutex);
  auto result = state_->status;
  result.visibility_known = state_->visibility_known;
  result.visible = state_->visibility_value;
  result.visibility_revoked = state_->visibility_revoked;
  result.visibility_sequence = state_->visibility_sequence;
  result.visibility_foreground_generation = state_->visibility_foreground;
  result.completion = state_->result.completion.status();
  return result;
}
void NativeSystemGraphicsTransport::PublishPlaneVisibility() noexcept {
  try {
    auto op = state_->Acquire(state_);
    if (!op) {
      return;
    }
    std::shared_ptr<const ui::NativeUiPlaneAuthority> plane;
    std::shared_ptr<const ui::NativeUiPlaneImagePermission> permission;
    uint64_t revision = 0;
    bool visible = false;
    {
      std::lock_guard lock(state_->mutex);
      plane = state_->plane;
      if (!plane || state_->visibility_revoked ||
          state_->status.mode != Mode::kProduction) {
        return;
      }
      if (state_->presentation_revision == UINT64_MAX) {
        state_->poisoned = true;
        if (state_->nonmodal_epoch) {
          state_->nonmodal_epoch->Revoke();
        }
        return;
      }
      revision = ++state_->presentation_revision;
      const uint64_t foreground = plane->owner().foreground_generation;
      visible = state_->visibility_known && state_->visibility_value &&
                state_->visibility_foreground == foreground;
      if (!visible && state_->nonmodal_epoch &&
          state_->nonmodal_epoch->foreground() == foreground &&
          state_->nonmodal_permission) {
        visible = true;
        permission = state_->nonmodal_permission;
      }
    }
    auto* graphics = op->context.kernel->emulator()->graphics_system();
    if (graphics && graphics->presenter()) {
      graphics->presenter()->SetNativeUiPlaneVisibility(
          plane, revision, visible, std::move(permission));
    }
  } catch (...) {
    // Presentation permission fails closed; no native hide, input counter,
    // GPU completion or fallback replay is synthesized.
    state_->Reject(Blocker::kBackendFailure, true);
  }
}

void NativeSystemGraphicsTransport::OnNativeNonmodalFrame(
    const NativeNonmodalFrameWitness& event,
    const gpu::NativeFrameExecutionRequest* request,
    const gpu::NativeFrameExecutionResult* result) noexcept {
  try {
    auto op = state_->Acquire(state_);
    if (!op || !event.revision() ||
        event.owner() != op->context.allocation->owner() ||
        event.domain_id() != op->context.domain->id() ||
        event.root_load_generation() != op->context.domain->root().generation) {
      return;
    }
    std::shared_ptr<NativeNotificationPresentationObserver> observer;
    std::shared_ptr<NativeSystemUiNotifications> visibility;
    {
      std::lock_guard lock(state_->mutex);
      observer = state_->nonmodal;
      visibility = state_->visibility;
    }
    if (!observer || !observer->IsAccepting() || !visibility) {
      return;
    }
    const auto modal = visibility->snapshot();
    if (!modal.accepting || modal.poisoned ||
        modal.known != event.modal_visibility_known() ||
        modal.last_sequence != event.modal_visibility_sequence() ||
        modal.foreground_generation != event.modal_foreground_generation()) {
      return;
    }
    const auto* emulator = op->context.kernel->emulator();
    const auto foreground = emulator->title_execution_state().snapshot();
    if (!emulator->OwnsTitleExecution(op->context.kernel) ||
        foreground.phase != TitleExecutionState::Phase::kRunning ||
        !event.foreground_generation() ||
        event.foreground_generation() != foreground.generation) {
      return;
    }
    const bool draw = event.kind() ==
                      NativeNonmodalFrameWitness::Kind::kEligibleDrawCandidate;
    bool show_draw = draw;
    std::optional<gpu::NativeOverlayPresentation> presentation;
    if (draw) {
      if (!event.source() || !request || !result ||
          request->identity() != *event.source() || !request->output_layout() ||
          result->status !=
              gpu::NativeFrameExecutionResult::Status::kSubmitted ||
          !result->title_state_restored || !result->written_range.length ||
          result->completion.status() != gpu::GpuCompletionStatus::kComplete ||
          result->completion.native_frame_identity() != event.source() ||
          !result->completion.submission() ||
          result->completion.backend_epoch() != request->backend_epoch()) {
        return;
      }
      const auto& layout = *request->output_layout();
      if (layout.origin ==
          gpu::NativeFrameOutputLayout::Origin::kNativeOverlayResolve) {
        if (!layout.overlay_state || !layout.overlay_presentation ||
            !layout.overlay_state->valid() ||
            !layout.overlay_presentation->valid()) return;
        const auto& requested = *layout.overlay_state;
        show_draw = requested.Has(gpu::NativeOverlayState::kEnable |
                                  gpu::NativeOverlayState::kAlphaKnown) &&
                    requested.enable != 0;
        presentation = layout.overlay_presentation;
      } else if (request->plan()->native_overlay_program()) {
        // A mixed profile without same-frame overlay placement may execute,
        // but cannot silently become a centered visible nonmodal plane.
        show_draw = false;
      }
    }
    {
      std::lock_guard lock(state_->mutex);
      // Both the real native producer and its delivered host state must match
      // the captured modal epoch. A transition can race a completed GPU image.
      if (state_->status.mode != Mode::kProduction ||
          state_->visibility_revoked || state_->poisoned ||
          event.revision() <= state_->nonmodal_revision ||
          state_->visibility_known != event.modal_visibility_known() ||
          state_->visibility_sequence != event.modal_visibility_sequence() ||
          state_->visibility_foreground !=
              event.modal_foreground_generation() ||
          (draw && request->backend_epoch() != state_->status.backend_epoch)) {
        return;
      }
      state_->nonmodal_revision = event.revision();
      if (show_draw) {
        if (!state_->nonmodal_epoch ||
            !state_->nonmodal_epoch->Matches(event, request->backend_epoch())) {
          if (state_->nonmodal_epoch) {
            state_->nonmodal_epoch->Revoke();
          }
          state_->nonmodal_epoch = std::make_shared<State::NonmodalEpoch>(
              state_, event, request->backend_epoch());
        }
        state_->nonmodal_permission =
            std::make_shared<const ui::NativeUiPlaneImagePermission>(
                *event.source(), state_->nonmodal_epoch, presentation);
      } else {
        if (state_->nonmodal_epoch) {
          state_->nonmodal_epoch->Revoke();
        }
        state_->nonmodal_permission.reset();
        state_->nonmodal_epoch.reset();
      }
    }
    if (event.kind() != NativeNonmodalFrameWitness::Kind::kEligibleDrawCandidate) {
      std::lock_guard lock(state_->mutex);
      if (state_->render_probe.selected_retired) {
        state_->render_probe.hidden_after_selected = true;
        state_->render_probe.genuine_clear |= event.kind() ==
            NativeNonmodalFrameWitness::Kind::kNativeEmptyClear;
      }
    }
    PublishPlaneVisibility();
  } catch (...) {
    state_->Reject(Blocker::kBackendFailure, true);
  }
}

void NativeSystemGraphicsTransport::OnNativeSystemUiVisibility(
    const NativeSystemUiVisibilityEvent& event) noexcept {
  try {
    auto op = state_->Acquire(state_);
    if (!op || event.owner() != op->context.allocation->owner() ||
        event.domain_id() != op->context.domain->id() ||
        event.root_load_generation() != op->context.domain->root().generation ||
        !event.sequence()) {
      return;
    }
    TRACE_TRANSPORT(op->context, "visibility_received",
                    {"native_notify_sequence", event.sequence()},
                    {"foreground_generation", event.foreground_generation()},
                    {"visible", event.visible()});
    // Delivery may follow an actual foreground transition after the native
    // broadcast was recorded. Neither its sequence nor an old hide grants
    // authority over the new title generation.
    const auto* emulator = op->context.kernel->emulator();
    const auto foreground = emulator->title_execution_state().snapshot();
    if (!event.foreground_generation() ||
        !emulator->OwnsTitleExecution(op->context.kernel) ||
        foreground.phase != TitleExecutionState::Phase::kRunning ||
        foreground.generation != event.foreground_generation()) {
      TRACE_TRANSPORT(
          op->context, "visibility_foreground_rejected",
          {"native_notify_sequence", event.sequence()},
          {"event_foreground_generation", event.foreground_generation()},
          {"actual_foreground_generation", foreground.generation},
          {"actual_foreground_phase", uint32_t(foreground.phase)});
      return;
    }
    std::shared_ptr<const ui::NativeUiPlaneAuthority> plane;
    std::shared_ptr<NativeNotificationPresentationObserver> nonmodal;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->status.mode != Mode::kProduction ||
          state_->visibility_revoked ||
          event.foreground_generation() < state_->visibility_foreground ||
          (state_->plane && event.foreground_generation() <
                                state_->plane->owner().foreground_generation) ||
          event.sequence() <= state_->visibility_sequence) {
        return;
      }
      state_->visibility_sequence = event.sequence();
      state_->visibility_foreground = event.foreground_generation();
      state_->visibility_known = true;
      state_->visibility_value = event.visible();
      state_->source_nonmodal.reset();
      if (state_->nonmodal_epoch) {
        state_->nonmodal_epoch->Revoke();
      }
      state_->nonmodal_permission.reset();
      state_->nonmodal_epoch.reset();
      nonmodal = state_->nonmodal;
      plane = state_->plane;
    }
    if (nonmodal) {
      nonmodal->ObserveNativeModalTransition(event);
    }
    if (op->context.input) {
      op->context.input->ObserveNativeUiVisibility(event);
    }
    TRACE_TRANSPORT(op->context, "visibility_cached",
                    {"native_notify_sequence", event.sequence()},
                    {"foreground_generation", event.foreground_generation()},
                    {"visible", event.visible()}, {"plane_bound", bool(plane)});
    if (plane &&
        plane->owner().foreground_generation == event.foreground_generation()) {
      PublishPlaneVisibility();
    }
  } catch (...) {
    OnNativeSystemUiVisibilityRevoked();
  }
}
void NativeSystemGraphicsTransport::
    OnNativeSystemUiVisibilityRevoked() noexcept {
  try {
    auto op = state_->Acquire(state_);
    if (!op) {
      return;
    }
    std::shared_ptr<const ui::NativeUiPlaneAuthority> plane;
    {
      std::lock_guard lock(state_->mutex);
      state_->visibility_revoked = true;
      if (state_->nonmodal_epoch) {
        state_->nonmodal_epoch->Revoke();
      }
      state_->nonmodal_permission.reset();
      state_->nonmodal_epoch.reset();
      state_->source_nonmodal.reset();
      plane = std::move(state_->plane);
    }
    if (op->context.input) {
      op->context.input->RevokeNativeTextVisibility();
    }
    auto* graphics = op->context.kernel->emulator()->graphics_system();
    if (plane && graphics && graphics->presenter()) {
      graphics->presenter()->WithdrawNativeUiPlaneAuthority(plane);
    }
  } catch (...) {
    FailStop();
  }
}
void NativeSystemGraphicsTransport::CloseExternalRequests() noexcept {
  std::shared_ptr<NativeUiBackendRoute> route;
  {
    std::lock_guard lock(state_->mutex);
    state_->external_closed = true;
    route = state_->ui_route;
  }
  if (route) {
    route->BeginStop();
  }
}
void NativeSystemGraphicsTransport::BeginStop() noexcept try {
  auto operation = std::make_unique<State::Operation>();
  std::shared_ptr<NativeSystemGraphicsNotifications> notifications;
  std::shared_ptr<NativeSystemUiNotifications> visibility;
  std::shared_ptr<NativeNotificationPresentationObserver> nonmodal;
  std::shared_ptr<const ui::NativeUiPlaneAuthority> plane;
  std::shared_ptr<const gpu::NativeSystemTitleInput> input_reservation;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.stopping) {
      return;
    }
    if (state_->active_calls == SIZE_MAX) {
      FailStop();
    }
    // Count this entire host stop tail before it releases the mutex and uses
    // Kernel/Presenter. Retirement cannot tombstone/free them underneath it.
    operation->context = state_->context;
    operation->state = state_;
    ++state_->active_calls;
    state_->status.stopping = true;
    state_->status.ready = false;
    state_->render_probe.closed = true;
    input_reservation = std::move(state_->title_input_reservation);
    notifications = state_->notifications;
    visibility = state_->visibility;
    nonmodal = state_->nonmodal;
    state_->source_nonmodal.reset();
    if (state_->nonmodal_epoch) {
      state_->nonmodal_epoch->Revoke();
    }
    state_->nonmodal_permission.reset();
    state_->nonmodal_epoch.reset();
    plane = state_->plane;
  }
  if (state_->ui_route) {
    state_->ui_route->BeginStop();
  }
  // Seal CPU tickets and completed-input publication before any callback/CP
  // teardown. The admitted Operation protects this host-only access; an
  // already-entered GPU copy can finish but its post-fence Publish must fail.
  if (operation->context.kernel) {
    auto* graphics = operation->context.kernel->emulator()->graphics_system();
    if (graphics && graphics->command_processor()) {
      graphics->command_processor()->CloseNativeSystemTitleInput();
    }
  }
  if (notifications) {
    notifications->BeginStop();
  }
  if (visibility) {
    visibility->BeginStop();
  }
  if (nonmodal) {
    nonmodal->BeginStop();
  }
  if (plane && operation->context.kernel) {
    auto* graphics = operation->context.kernel->emulator()->graphics_system();
    if (graphics && graphics->presenter()) {
      graphics->presenter()->WithdrawNativeUiPlaneAuthority(plane);
    }
  }
} catch (...) {
  FailStop();
}

bool NativeSystemGraphicsTransport::Drain() noexcept {
  try {
    if (XThread::IsInThread()) {
      return false;
    }
    BeginStop();
    std::shared_ptr<NativeSystemGraphicsNotifications> notifications;
    std::shared_ptr<NativeSystemUiNotifications> visibility;
    std::shared_ptr<NativeNotificationPresentationObserver> nonmodal;
    std::shared_ptr<OutputCaptureObservation> output_capture, render_capture;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    {
      std::unique_lock lock(state_->mutex);
      if (!state_->changed.wait_until(lock, deadline, [&] {
            return !state_->active_calls && !state_->status.callback_pending;
          })) {
        return false;
      }
      notifications = state_->notifications;
      visibility = state_->visibility;
      nonmodal = state_->nonmodal;
      output_capture = state_->output_capture;
      render_capture = state_->render_probe_capture;
    }
    // No State/global/resolve lock while callback closures drain. Inline work
    // already returned with its admitted caller; queued CP-owned Requests are
    // additionally bounded by actual CP shutdown/join before Kernel teardown.
    if (state_->ui_route && !state_->ui_route->Drain(deadline)) {
      return false;
    }
    if (output_capture && !output_capture->Drain(deadline)) {
      return false;
    }
    if (render_capture && render_capture != output_capture &&
        !render_capture->Drain(deadline)) {
      return false;
    }
    if (visibility && !visibility->Drain()) {
      return false;
    }
    if (nonmodal && !nonmodal->Drain()) {
      return false;
    }
    if (notifications && !notifications->Drain()) {
      return false;
    }
    std::lock_guard lock(state_->mutex);
    state_->status.drained = true;
    return true;
  } catch (...) {
    return false;
  }
}
bool NativeSystemGraphicsTransport::RetireAfterKernelStop() noexcept {
  try {
    std::lock_guard retirement(state_->retirement);
    if (!Drain()) {
      return false;
    }
    NativeSystemUiServiceContext context;
    bool attempted = false;
    gpu::GpuBackendStopProof stop;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->status.retired) {
        return true;
      }
      context = state_->context;
      attempted = state_->attempted;
      stop = state_->stop_proof;
    }
    if (context.kernel) {
      for (const auto& thread :
           context.kernel->object_table()->GetObjectsByType<XThread>(
               XObject::Type::Thread)) {
        if (thread->is_running()) {
          return false;
        }
      }
    }
    if (attempted && (!stop.backend_epoch() || !stop.stopped())) {
      return false;
    }
    // Keep externally retained Transport handles as closed metadata only.
    // Move guest/code holders out while Kernel/Memory still live; no PPC/GPU
    // release, native Close, host image handle or speculative CPU rollback.
    NativeSystemUiServiceContext old_context;
    std::shared_ptr<NativeSystemGraphicsNotifications> notifications;
    std::shared_ptr<NativeGraphicsDeviceBinding> binding;
    std::shared_ptr<State::SharedObservation> publication;
    std::shared_ptr<NativeSystemUiNotifications> visibility;
    std::shared_ptr<NativeNotificationPresentationObserver> nonmodal;
    NativeSystemSharedGraphicsState graph;
    std::shared_ptr<NativeSystemFrameDelegation> delegation;
    std::shared_ptr<const gpu::NativeFrameExecutionRequest> request;
    std::shared_ptr<const NativeFrameSource> source;
    std::shared_ptr<const NativeSystemCompletedFrameRecord> record;
    std::shared_ptr<OutputCaptureObservation> render_capture;
    std::shared_ptr<const ui::NativeUiPlaneAuthority> plane;
    std::vector<std::shared_ptr<State::TaskReference>> tasks;
    {
      std::lock_guard lock(state_->mutex);
      state_->status.retired = true;
      state_->status.mode = Mode::kStopped;
      old_context = std::move(state_->context);
      state_->context = {};
      notifications = std::move(state_->notifications);
      binding = std::move(state_->binding);
      publication = std::move(state_->publication);
      visibility = std::move(state_->visibility);
      nonmodal = std::move(state_->nonmodal);
      state_->source_nonmodal.reset();
      if (state_->nonmodal_epoch) {
        state_->nonmodal_epoch->Revoke();
      }
      state_->nonmodal_permission.reset();
      state_->nonmodal_epoch.reset();
      graph = std::move(state_->graph);
      delegation = std::move(state_->delegation);
      request = std::move(state_->request);
      render_capture = std::move(state_->render_probe_capture);
      source = std::move(state_->source);
      record = std::move(state_->record);
      plane = std::move(state_->plane);
      tasks = std::move(state_->task_references);
    }
    return true;
  } catch (...) {
    return false;
  }
}
}  // namespace xe::kernel
