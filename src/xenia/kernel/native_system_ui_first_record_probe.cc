/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_ui_first_record_probe.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_graphics_notifications.h"
#include "xenia/kernel/native_system_graphics_transport.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DECLARE_path(native_notification_queue_probe_report);

namespace xe::kernel {
namespace {
GuestRuntime::ExecutionResult Fail(const char* reason) {
  return {false, 0x80004005, reason};
}
GuestRuntime::ExecutionResult Observed() { return {true, 0, {}}; }
bool System(XThread& thread, KernelState* kernel) {
  return XThread::IsInThread(&thread) && thread.kernel_state() == kernel &&
         thread.native_frame_thread_identity().process_type ==
             X_PROCTYPE_SYSTEM;
}
std::optional<uint32_t> Word(Memory& memory, uint32_t at) {
  if (!at || (at & 3) ||
      memory.QueryRangeAccess(at, 4) == xe::memory::PageAccess::kNoAccess) {
    return {};
  }
  return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at));
}
struct ManagerReadiness {
  static constexpr uint32_t kRequired = 0x7FF;
  uint32_t passed = 0, readable = 0;
  std::array<uint32_t, 3> flags{};  // 2190,2194,21BC
  std::array<uint32_t, 2> tasks{}, task_state{}, task_callback{};
  std::array<bool, 2> status_called{}, status_succeeded{};
  std::array<uint64_t, 2> task_status{};
  uint32_t event_header_type = UINT32_MAX, event_header_signal = 0;
  uint32_t event_type = UINT32_MAX, event_state = 0;
  bool event_resolved = false, first_record = false, hardware_readable = false;
  uint32_t graph_status = 0, device = 0, dc = 0, recording_context = 0;
  uint32_t hardware = 0, transport_mode = 0, blocker = 0;
  uint64_t foreground_generation = 0, backend_epoch = 0, domain_id = 0;
  uint32_t root_hmodule = 0, thread_id = 0;
  uint64_t root_load_generation = 0, runtime_generation = 0;
  bool ready() const { return passed == kRequired; }
};

ManagerReadiness ObserveManagerReadiness(
    const NativeSystemUiServiceContext& c, XThread& bootstrap,
    const std::shared_ptr<NativeSystemGraphicsTransport>& graphics,
    const gpu::GpuBackendStopProof& stop, bool sample_status) {
  ManagerReadiness out;
  const auto pass = [&](unsigned bit, bool value) {
    if (value) {
      out.passed |= 1u << bit;
    }
  };
  const bool live = System(bootstrap, c.kernel) && c.catalog && c.domain &&
                    c.allocation && c.domain->active() &&
                    c.allocation->IsAccepting() &&
                    c.allocation->MatchesDomain(c.domain) && !stop.stopped();
  pass(0, live);
  if (!live || !graphics) {
    return out;
  }
  out.thread_id = bootstrap.thread_id();
  out.root_hmodule = c.domain->root().hmodule;
  out.root_load_generation = c.domain->root().generation;
  out.domain_id = c.domain->id();
  out.runtime_generation = c.allocation->owner().module_generation;
  const auto foreground =
      c.kernel->emulator()->title_execution_state().snapshot();
  out.foreground_generation = foreground.generation;
  pass(1, foreground.phase == TitleExecutionState::Phase::kRunning &&
              c.kernel->emulator()->OwnsTitleExecution(c.kernel));
  const auto transport = graphics->snapshot();
  out.transport_mode = uint32_t(transport.mode);
  out.blocker = uint32_t(transport.blocker);
  out.backend_epoch = transport.backend_epoch;
  out.first_record = bool(transport.first_record);
  pass(
      2,
      NativeSystemGraphicsTransport::Lookup(c.kernel).get() == graphics.get() &&
          transport.mode ==
              NativeSystemGraphicsTransport::Mode::kObservationOnly &&
          !transport.stopping && !transport.retired &&
          transport.blocker ==
              NativeSystemGraphicsTransport::Blocker::kNativeWriterExclusion &&
          transport.backend_epoch == stop.backend_epoch());
  const auto notifications = graphics->notifications();
  pass(3, notifications && notifications->IsRegistered());
  auto& memory = *c.kernel->memory();
  const auto& symbols = c.catalog->symbols();
  object_ref<XEvent> event;
  {
    auto lock = global_critical_region::AcquireDirect();
    constexpr std::array<uint32_t, 3> flags{0x81AA2190, 0x81AA2194, 0x81AA21BC};
    for (size_t i = 0; i < flags.size(); ++i) {
      if (const auto value = Word(memory, flags[i])) {
        out.readable |= 1u << i;
        out.flags[i] = *value;
      }
    }
    pass(4, (out.readable & 7) == 7 && out.flags[0] == 1 && out.flags[1] == 1 &&
                out.flags[2] == 0);
    const std::array<uint32_t, 2> cells{symbols.manager_task_cell,
                                        symbols.manager_startup_task_cell};
    bool tasks_valid = true;
    for (size_t i = 0; i < cells.size(); ++i) {
      const auto handle = Word(memory, cells[i]);
      if (handle) {
        out.readable |= 1u << (3 + i);
        out.tasks[i] = *handle;
      }
      if (!handle || *handle < 0x1000 || (*handle & 3) != 1 ||
          (*handle & ~1u) > UINT32_MAX - 0x30 || !Word(memory, *handle & ~1u)) {
        tasks_valid = false;
        continue;
      }
      const auto state = Word(memory, (*handle & ~1u) + 0x10);
      const auto callback = Word(memory, (*handle & ~1u) + 0x30);
      if (state) {
        out.readable |= 1u << (5 + i);
        out.task_state[i] = *state;
      }
      if (callback) {
        out.readable |= 1u << (7 + i);
        out.task_callback[i] = *callback;
      }
      tasks_valid &= state.has_value() && callback.has_value();
    }
    pass(5, tasks_valid && out.tasks[0] != out.tasks[1]);
    // +4 remains the Created request Task until a real request configures it.
    pass(6, (out.readable & (1u << 8)) && out.task_callback[1] == 0x816CE108);
    constexpr uint32_t event_address = 0x81AA21A4;
    if (memory.QueryRangeAccess(event_address, sizeof(X_KEVENT)) !=
        xe::memory::PageAccess::kNoAccess) {
      X_DISPATCH_HEADER header;
      std::memcpy(&header, memory.TranslateVirtual(event_address),
                  sizeof(header));
      out.readable |= 1u << 9;
      out.event_header_type = uint32_t(header.type);
      out.event_header_signal = header.signal_state;
      // Observe the real object already wrapped by KeInitialize/SetEvent.
      // Never create a wrapper or write a guest wait-list during observation.
      if (header.wait_list.flink_ptr == kXObjSignature) {
        event = c.kernel->object_table()->LookupObject<XEvent>(
            header.wait_list.blink_ptr, true);
        if (event && event->guest_object() != event_address) {
          event.reset();
        }
      }
    }
  }
  if (event) {
    out.event_resolved = true;
    // This host query neither consumes nor changes the native manual event.
    event->Query(&out.event_type, &out.event_state);
  }
  pass(7, out.event_resolved && out.event_header_type == 0 &&
              out.event_header_signal == 1 && out.event_type == 0 &&
              out.event_state == 1);
  {
    // This is an ephemeral ownership observation, not writer exclusion or a
    // Source. All pins drop BEFORE any Guide call or Delay below.
    const auto graph = c.catalog->ReadSharedGraphicsState();
    out.graph_status = uint32_t(graph.status);
    out.device = graph.device;
    out.dc = graph.dc;
    out.recording_context = graph.recording_context;
    bool owned =
        graph.status == NativeSystemSharedGraphicsState::Status::kObserved &&
        graph.command_pin && graph.command_pin->valid();
    if (owned) {
      for (const auto& a : graph.command_pin->allocations()) {
        owned &= a.owner == c.allocation->owner();
      }
    }
    pass(8, owned);
    auto lock = global_critical_region::AcquireDirect();
    pass(9, graph.dc && graph.dc <= UINT32_MAX - 4 && graph.device &&
                graph.device <= UINT32_MAX - 0x3C &&
                Word(memory, graph.dc) == 0x81604CC0 &&
                Word(memory, 0x81604CC4) == 0x8177B8B8 &&
                Word(memory, graph.dc + 4).value_or(0) != 0 &&
                Word(memory, graph.dc + 4).value_or(UINT32_MAX) != UINT32_MAX &&
                Word(memory, graph.device + 0x3C).value_or(0) != 0 &&
                Word(memory, graph.device + 0x3C).value_or(UINT32_MAX) !=
                    UINT32_MAX);
  }
  if (const auto flags = c.catalog->ReadHardwareFlags()) {
    out.hardware_readable = true;
    out.hardware = *flags;
  }
  if (sample_status && (out.passed & 0x3FF) == 0x3FF) {
    bool known = true;
    for (size_t i = 0; i < 2; ++i) {
      const std::array<uint32_t, 1> args{out.tasks[i]};
      const auto status = c.catalog->calls()->ExecuteDeferredSystemFunction(
          bootstrap, "native-system-task-status", args);
      out.status_called[i] = true;
      out.status_succeeded[i] = status.succeeded;
      out.task_status[i] = status.return_value;
      known &= status.succeeded &&
               (status.return_value == 0 || status.return_value == 1 ||
                status.return_value == X_E_PENDING);
    }
    pass(10, known);
  }
  return out;
}
template <typename T, size_t N>
std::string JsonArray(const std::array<T, N>& values) {
  std::string result = "[";
  for (size_t i = 0; i < N; ++i) {
    if (i) {
      result += ',';
    }
    result += std::to_string(values[i]);
  }
  result += ']';
  return result;
}
bool ActualGuideProfile(const GuestRuntimeCallExecutor& calls) {
  constexpr std::string_view identity = "native-public-show-guide";
  constexpr uint32_t address = 0x816C3580;
  constexpr std::array<uint32_t, 3> words{0x38A00000, 0x38800001, 0x4BFFF568};
  const auto& plan = calls.validated();
  const auto found = plan.catalog.find(std::string(identity));
  const auto contract = plan.contracts.find(std::string(identity));
  const auto module = calls.module();
  if (found == plan.catalog.end() || contract == plan.contracts.end() ||
      found->second.address != address || found->second.arguments.size() != 1 ||
      found->second.arguments.front() != 0 ||
      found->second.return_kind != GuestRuntime::ReturnKind::kUInt32 ||
      contract->second.result !=
          ValidatedGuestRuntimePlan::ResultPolicy::kAny ||
      contract->second.caller !=
          ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem ||
      !contract->second.before.empty() || !contract->second.after.empty() ||
      module->GetProcAddressByOrdinal(0x304) != address) {
    return false;
  }
  for (const auto& phase : plan.plan.phases) {
    for (const auto& function : phase.functions) {
      if (function.identity == identity) {
        return false;
      }
    }
  }
  auto* memory = calls.kernel()->memory();
  if (memory->QueryRangeAccess(address, 12) == memory::PageAccess::kNoAccess) {
    return false;
  }
  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t at = address + uint32_t(i) * 4;
    xex2_section_type section;
    if (calls.kernel()->GetUserModuleForAddress(at).get() != module.get() ||
        !module->xex_module()->GetPageSectionType(at, &section) ||
        section != XEX_SECTION_CODE ||
        module->xex_module()->TranslateCode(at) !=
            memory->TranslateVirtual<const uint8_t*>(at) ||
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(at)) != words[i]) {
      return false;
    }
  }
  return true;
}
}  // namespace

struct NativeSystemUiFirstRecordProbe::State {
  KernelState* kernel = nullptr;
  gpu::GpuBackendStopProof stop;
  mutable std::mutex mutex;
  std::shared_ptr<NativeSystemGraphicsTransport> graphics;
  std::atomic<bool> closing{false}, stopping{false}, retired{false};
  std::mutex report_mutex;
  std::filesystem::path report_path;
  std::unique_ptr<FILE, decltype(&std::fclose)> report{nullptr, &std::fclose};
  bool Write(std::string text) {
    std::lock_guard lock(report_mutex);
    return report &&
           std::fwrite(text.data(), 1, text.size(), report.get()) ==
               text.size() &&
           std::fflush(report.get()) == 0;
  }
  bool WriteReadiness(std::string_view stage, const ManagerReadiness& r) {
    const auto counters =
        NativeSystemGraphicsTransport::ReadDiagnosticCounters();
    return Write(fmt::format(
        "{{\"event\":\"native_manager_readiness_snapshot\",\"stage\":\"{}\","
        "\"steady_milliseconds\":{},\"required_bits\":{},\"passed_bits\":{},"
        "\"readable_bits\":{},\"ready_sample\":{},\"flags_2190_2194_21bc\":{},"
        "\"task_handles\":{},\"task_state_readback\":{},\"task_callbacks\":{},"
        "\"task_status_called\":{},\"task_status_succeeded\":{},\"task_status_"
        "raw\":{},"
        "\"status_and_memory_samples_atomic\":false,\"event_header_type\":{},"
        "\"event_header_signal\":{},\"event_resolved\":{},\"event_type\":{},"
        "\"event_state\":{},"
        "\"graph_status\":{},\"device\":{},\"dc\":{},\"recording_context\":{},"
        "\"hardware_readable\":{},\"hardware_flags\":{},\"transport_mode\":{},"
        "\"blocker\":{},"
        "\"foreground_generation\":{},\"backend_epoch\":{},\"first_record_"
        "present\":{},"
        "\"root_hmodule\":{},\"root_load_generation\":{},\"domain_id\":{},"
        "\"runtime_generation\":{},\"thread_id\":{},"
        "\"hook_counter_scope\":\"process_first_record\",\"hook_counters_"
        "atomic_snapshot\":false,"
        "\"hook_counters\":{},\"production_ready\":false}}\n",
        stage,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        ManagerReadiness::kRequired, r.passed, r.readable, r.ready(),
        JsonArray(r.flags), JsonArray(r.tasks), JsonArray(r.task_state),
        JsonArray(r.task_callback), JsonArray(r.status_called),
        JsonArray(r.status_succeeded), JsonArray(r.task_status),
        r.event_header_type, r.event_header_signal, r.event_resolved,
        r.event_type, r.event_state, r.graph_status, r.device, r.dc,
        r.recording_context, r.hardware_readable, r.hardware, r.transport_mode,
        r.blocker, r.foreground_generation, r.backend_epoch, r.first_record,
        r.root_hmodule, r.root_load_generation, r.domain_id,
        r.runtime_generation, r.thread_id, JsonArray(counters)));
  }
  bool WriteRecord(const NativeSystemGraphicsTransport::FirstRecord& record,
                   bool before_guide) {
    auto pm4_path = report_path;
    pm4_path += before_guide ? ".pre-guide.pm4" : ".pm4";
    std::unique_ptr<FILE, decltype(&std::fclose)> pm4{
        filesystem::OpenFile(pm4_path, "wbx"), &std::fclose};
    if (!pm4 ||
        std::fwrite(record.observed_primary.data(), 1,
                    record.observed_primary.size(),
                    pm4.get()) != record.observed_primary.size() ||
        std::fflush(pm4.get())) {
      return false;
    }
    std::string descriptor;
    for (uint8_t byte : record.descriptor) {
      fmt::format_to(std::back_inserter(descriptor), "{:02x}", byte);
    }
    return Write(fmt::format(
        "{{\"event\":\"{}\",\"kind\":{},\"thread_id\":{},"
        "\"native_task\":{},\"manager_task\":{},\"command_physical\":{},"
        "\"command_dwords\":{},\"output_physical\":{},\"command_owned\":{},"
        "\"output_owned\":{},\"return_address\":{},"
        "\"renderer_return_address\":{},\"genuine_set_helper\":{},"
        "\"matching_native_task\":{},\"native_end_path\":{},"
        "\"descriptor_hex\":\"{}\",\"observed_bytes\":{},"
        "\"record_observed_before_guide_request\":{},\"guide_causality_"
        "proven\":false,"
        "\"source_frozen\":false,\"submitted\":false,\"retired\":false,"
        "\"production_ready\":false}}\n",
        before_guide ? "native_record_before_guide_observed"
                     : "first_native_record_observed",
        uint32_t(record.kind), record.thread_id, record.native_task,
        record.manager_task, record.command_physical, record.command_dwords,
        record.output_physical, record.command_owned, record.output_owned,
        record.return_address, record.renderer_return_address,
        record.genuine_set_helper, record.matching_native_task,
        record.native_end_path, descriptor, record.observed_primary.size(),
        before_guide));
  }
  std::shared_ptr<NativeSystemGraphicsTransport> Graphics() const {
    std::lock_guard lock(mutex);
    return graphics;
  }
};

NativeSystemUiFirstRecordProbe::NativeSystemUiFirstRecordProbe(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemUiFirstRecordProbe::~NativeSystemUiFirstRecordProbe() = default;

std::shared_ptr<NativeSystemUiFirstRecordProbe>
NativeSystemUiFirstRecordProbe::Create(
    KernelState* kernel, const std::filesystem::path& report_path) {
  if (!kernel || report_path.empty() || !report_path.is_absolute() ||
      !kernel->emulator() || !kernel->emulator()->graphics_system()) {
    return {};
  }
  auto* cp = kernel->emulator()->graphics_system()->command_processor();
  if (!cp) {
    return {};
  }
  const auto capacity = cp->GetNativeSystemFrameCapacity();
  if (!capacity.backend_epoch ||
      capacity.backend_stop_proof.backend_epoch() != capacity.backend_epoch ||
      capacity.backend_stop_proof.stopped()) {
    return {};
  }
  auto state = std::make_shared<State>();
  state->kernel = kernel;
  state->stop = capacity.backend_stop_proof;
  state->report_path = report_path;
  state->report.reset(filesystem::OpenFile(report_path, "wbx"));
  if (!state->report || !state->Write(fmt::format(
                            "{{\"event\":\"first_record_diagnostic_created\","
                            "\"backend_epoch\":{},\"production_ready\":false,"
                            "\"gpu_submission_enabled\":false}}\n",
                            capacity.backend_epoch))) {
    return {};
  }
  return std::shared_ptr<NativeSystemUiFirstRecordProbe>(
      new NativeSystemUiFirstRecordProbe(std::move(state)));
}

gpu::GpuBackendStopProof NativeSystemUiFirstRecordProbe::backend_stop_proof()
    const {
  return state_->stop;
}

GuestRuntime::ExecutionResult
NativeSystemUiFirstRecordProbe::PrepareBeforeWorkers(
    const NativeSystemUiServiceContext& context, XThread& bootstrap) {
  if (!System(bootstrap, state_->kernel) || context.kernel != state_->kernel ||
      state_->closing.load() || !context.allocation->IsAccepting()) {
    return Fail("First-record diagnostic actual bootstrap admission failed");
  }
  auto* cp = context.kernel->emulator()->graphics_system()->command_processor();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!cp->supports_native_system_frames()) {
    if (state_->closing.load() || state_->stop.stopped() ||
        std::chrono::steady_clock::now() >= deadline) {
      return Fail("Real independent native backend capability unavailable");
    }
    bootstrap.Delay(0, 0, uint64_t(-10000ll));
  }
  auto graphics = NativeSystemGraphicsTransport::Create();
  if (!graphics || !graphics->InstallObservationOnly(context, bootstrap)) {
    return Fail("First-record observation transport installation failed");
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->graphics = std::move(graphics);
  }

  // Explicit diagnostic virtual profile, independent of production Prepared.
  // The real backend supports native work, but this observer intentionally
  // holds the first completed CPU record without executing/ACKing it.
  const auto before = context.catalog->ReadHardwareFlags();
  const auto* variable =
      context.kernel->processor()->export_resolver()->GetExportByOrdinal(
          "xboxkrnl.exe", 0x156);
  if (!before || !variable ||
      variable->get_type() != cpu::Export::Type::kVariable ||
      std::string_view(variable->name) != "XboxHardwareInfo" ||
      !variable->variable_ptr || (variable->variable_ptr & 3)) {
    return Fail("Actual diagnostic hardware import unavailable");
  }
  auto* memory = context.kernel->memory();
  const auto access = memory->QueryRangeAccess(variable->variable_ptr, 4);
  if (access != memory::PageAccess::kReadWrite &&
      access != memory::PageAccess::kExecuteReadWrite) {
    return Fail("Actual diagnostic hardware variable not writable");
  }
  auto* flags = memory->TranslateVirtual<uint32_t*>(variable->variable_ptr);
  uint32_t expected = xe::byte_swap(*before);
  if (!std::atomic_ref<uint32_t>(*flags).compare_exchange_strong(
          expected, xe::byte_swap(*before | 0x200u),
          std::memory_order_acq_rel) ||
      context.catalog->ReadHardwareFlags() != (*before | 0x200u)) {
    return Fail("Diagnostic virtual hardware publication changed concurrently");
  }
  if (!state_->Write(
          fmt::format("{{\"event\":\"diagnostic_transport_installed\","
                      "\"old_hardware_flags\":{},\"hardware_flags\":{},"
                      "\"production_ready\":false}}\n",
                      *before, *before | 0x200u))) {
    return Fail("Diagnostic report write failed");
  }
  return Observed();
}

GuestRuntime::ExecutionResult
NativeSystemUiFirstRecordProbe::ObserveNativePhase(
    NativeSystemUiStartupPhase phase,
    const NativeSystemUiServiceContext& context, XThread& thread) {
  if (!System(thread, state_->kernel) || context.kernel != state_->kernel ||
      !context.allocation->IsAccepting() || !state_->Graphics()) {
    return Fail("Diagnostic phase lost actual native context");
  }
  if (phase == NativeSystemUiStartupPhase::kSharedGraphics &&
      !state_->Graphics()->ObserveSharedDevicePublication(context, thread)) {
    return Fail("Diagnostic actual early device publication handoff failed");
  }
  if (!state_->Write(
          fmt::format("{{\"event\":\"native_phase_observed\",\"phase\":{},"
                      "\"thread_id\":{},\"production_ready\":false}}\n",
                      uint32_t(phase), thread.thread_id()))) {
    return Fail("Diagnostic phase report write failed");
  }
  // Service's common observer already checked actual worker/TLS/cohort/config/
  // input/FE fields/manager handles. Unproved registration/writer closure is
  // deliberately NOT promoted to production admission by this observation.
  return Observed();
}

GuestRuntime::ExecutionResult
NativeSystemUiFirstRecordProbe::ObserveAfterNativeBoot(
    const NativeSystemUiServiceContext& context, XThread& bootstrap) {
  if (!System(bootstrap, state_->kernel) || state_->closing.load() ||
      !state_->Graphics()) {
    return Fail("Diagnostic request admission closed");
  }
  const bool notification = !cvars::native_notification_queue_probe_report.empty();
  if (!notification && !ActualGuideProfile(*context.catalog->calls())) {
    return Fail("Diagnostic Guide304 export/body/prototype contract mismatch");
  }
  const auto graphics = state_->Graphics();
  // Share the old foreground budget with actual native readiness. Waiting for
  // proven publication does not add a blind delay or replay a public request.
  const auto foreground_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(12);
  auto readiness = ObserveManagerReadiness(context, bootstrap, graphics,
                                           state_->stop, false);
  if (!state_->WriteReadiness("before_readiness_wait", readiness)) {
    return Fail("Diagnostic readiness report write failed");
  }
  while (!state_->closing.load() && !state_->stop.stopped()) {
    if (const auto early = graphics->snapshot().first_record) {
      if (!state_->WriteRecord(*early, true)) {
        return Fail("Exclusive pre-request PM4/record report write failed");
      }
      return Fail("A native record preceded the diagnostic Guide request");
    }
    readiness = ObserveManagerReadiness(context, bootstrap, graphics,
                                        state_->stop, true);
    if (readiness.ready()) {
      break;
    }
    if (readiness.status_called[0] || readiness.status_called[1]) {
      if (!state_->WriteReadiness("task_status_rejected", readiness)) {
        return Fail("Diagnostic readiness report write failed");
      }
      return Fail("Actual native manager Task status rejected readiness");
    }
    if (std::chrono::steady_clock::now() >= foreground_deadline) {
      if (!state_->WriteReadiness("readiness_timeout", readiness)) {
        return Fail("Diagnostic readiness report write failed");
      }
      return Fail(
          "Actual native manager readiness not observed within diagnostic "
          "bound");
    }
    bootstrap.Delay(0, 0, uint64_t(-10000ll));
  }
  if (state_->closing.load() || state_->stop.stopped() || !readiness.ready()) {
    return Fail("Diagnostic request admission closed");
  }
  if (!state_->WriteReadiness(notification ? "before_notification" : "before_guide", readiness)) {
    return Fail("Diagnostic readiness report write failed");
  }
  if (const auto early = graphics->snapshot().first_record) {
    if (!state_->WriteRecord(*early, true)) {
      return Fail("Exclusive pre-request PM4/record report write failed");
    }
    return Fail("A native record preceded the diagnostic Guide request");
  }
  // Report output and readonly Task calls can yield. Recheck actual ownership
  // and the same foreground before the ONE original-caller public request.
  const auto foreground =
      context.kernel->emulator()->title_execution_state().snapshot();
  if (state_->closing.load() || state_->stop.stopped() ||
      !System(bootstrap, context.kernel) || !context.domain->active() ||
      !context.allocation->IsAccepting() ||
      !context.allocation->MatchesDomain(context.domain) ||
      foreground.phase != TitleExecutionState::Phase::kRunning ||
      foreground.generation != readiness.foreground_generation ||
      !context.kernel->emulator()->OwnsTitleExecution(context.kernel)) {
    return Fail("Diagnostic request lost actual ready foreground ownership");
  }
  const std::array<uint32_t, 1> args{0};
  // Guide remains on its original actual SYSTEM caller. The fixed opt-in
  // notification uses the service's same genuine Schedule/Wait/Close owner;
  // no fabricated Task/TLS, driver callback diversion or public family enable.
  const auto requested = notification
      ? NativeSystemUiService::InvokeFixedNotificationDiagnostic(context.kernel,
                                                                bootstrap)
      : context.catalog->calls()->ExecuteDeferredOnOriginalCaller(
            bootstrap, "native-public-show-guide", args);
  if (!state_->Write(fmt::format(
          "{{\"event\":\"{}\",\"executed\":{},"
          "\"raw_result\":{},\"production_ready\":false}}\n",
          notification ? "native_notification_request_returned"
                       : "native_guide_request_returned",
          requested.succeeded ? "true" : "false", requested.return_value))) {
    return Fail("Diagnostic request report write failed");
  }
  // Metadata-only snapshots after the public call never execute another Task
  // status query. A successful public return reports scheduling, not UI
  // success.
  if (!state_->WriteReadiness(
          notification ? "after_notification" : "after_guide", ObserveManagerReadiness(context, bootstrap, graphics,
                                                 state_->stop, false))) {
    return Fail("Diagnostic readiness report write failed");
  }
  if (!requested.succeeded) {
    return Fail(notification ? "Native notification diagnostic request failed"
                             : "Native Guide request execution failed");
  }
  if (notification && requested.return_value == 1)
    return Fail("Native notification deliberately suppressed; no fallback");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!state_->closing.load()) {
    const auto snapshot = graphics->snapshot();
    if (snapshot.first_record) {
      if (!state_->WriteReadiness(
              "first_record_captured",
              ObserveManagerReadiness(context, bootstrap, graphics,
                                      state_->stop, false))) {
        return Fail("Diagnostic readiness report write failed");
      }
      if (!state_->WriteRecord(*snapshot.first_record, false)) {
        return Fail("Exclusive observed PM4/record report write failed");
      }
      return Observed();
    }
    if (state_->stop.stopped() ||
        std::chrono::steady_clock::now() >= deadline) {
      if (!state_->WriteReadiness(
              "first_record_timeout",
              ObserveManagerReadiness(context, bootstrap, graphics,
                                      state_->stop, false))) {
        return Fail("Diagnostic readiness report write failed");
      }
      return Fail(
          "No actual native manager Set observed within diagnostic bound");
    }
    bootstrap.Delay(0, 0, uint64_t(-100000ll));
  }
  return Fail("First-record diagnostic stopped");
}

void NativeSystemUiFirstRecordProbe::CloseExternalRequests() noexcept {
  state_->closing.store(true);
  if (const auto graphics = state_->Graphics()) {
    graphics->CloseExternalRequests();
  }
}
void NativeSystemUiFirstRecordProbe::BeginStopTransport() noexcept {
  state_->stopping.store(true);
  if (const auto graphics = state_->Graphics()) {
    graphics->BeginStop();
  }
}
bool NativeSystemUiFirstRecordProbe::DrainCallbacksBeforeCpuStop() noexcept {
  const auto graphics = state_->Graphics();
  return !graphics || graphics->Drain();
}
bool NativeSystemUiFirstRecordProbe::RetireAfterKernelStop() noexcept {
  const auto graphics = state_->Graphics();
  if (!state_->stopping.load() || !state_->stop.stopped() ||
      (graphics && !graphics->RetireAfterKernelStop())) {
    return false;
  }
  state_->retired.store(true);
  {
    std::lock_guard lock(state_->mutex);
    state_->graphics.reset();
  }
  state_->kernel = nullptr;
  return true;
}
}  // namespace xe::kernel
