/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/native_avatar_contract.h"
#include "xenia/kernel/native_avatar_service.h"
#include "xenia/kernel/native_profile_identity_contract.h"
#include "xenia/kernel/native_profile_identity_owner.h"
#include "xenia/kernel/native_identity_tile_work.h"
#include "xenia/kernel/native_media_service.h"
#include "xenia/kernel/native_content_resolve.h"
#include "xenia/kernel/native_xmsg_provenance.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"

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
bool IsSigninPublic(NativeSystemUiPublicCall call) {
  return call==NativeSystemUiPublicCall::kShowSignin ||
         call==NativeSystemUiPublicCall::kShowSigninWithUser;
}
// Before PPC, with a real retained root-code lease and no Service lock. These
// internal helpers are authenticated separately from the public wrapper.
bool ValidSigninObservedCode(const GuestRuntimeCallExecutor& calls) {
  struct Body {uint32_t address,size;std::string_view hash;};
  constexpr Body bodies[]{
    {0x816C19C8,80,"ebc223b4b6327c41551102ff58d70f43159bf96ab6cc4d6acaf25f7b18edceb7"},
    {0x816C16C8,80,"337f172066ab31099747c3f6da1b906765f59e4cdacb70fc6e931535a6ea4c12"},
    {0x816C1868,256,"3ba5a52c7417a9a0e542e4da79b59fd14120c7063f3a295604f311e2b354d4d3"},
    {0x816A2FF8,300,"ba25abae13eded052f59a02e329eba7add73e847ea975cc67b2eeac809fd9754"}};
  auto lock=global_critical_region::AcquireDirect();
  const auto module=calls.module();auto* memory=calls.kernel()->memory();
  for(const auto& body:bodies){
    if(memory->QueryRangeAccess(body.address,body.size)==memory::PageAccess::kNoAccess)return false;
    for(uint32_t at=body.address;at<body.address+body.size;at+=4){
      xex2_section_type section;
      if(calls.kernel()->GetUserModuleForAddress(at).get()!=module.get() ||
         !module->xex_module()->GetPageSectionType(at,&section) || section!=XEX_SECTION_CODE ||
         module->xex_module()->TranslateCode(at)!=memory->TranslateVirtual<const uint8_t*>(at))return false;
    }
    std::array<uint8_t,32> digest;
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(body.address),body.size,digest.data(),0);
    constexpr char hex[]="0123456789abcdef";
    for(size_t n=0;n<digest.size();++n)if(body.hash[n*2]!=hex[digest[n]>>4] || body.hash[n*2+1]!=hex[digest[n]&15])return false;
  }
  return true;
}
// A paired provider is requested by exact deferred metadata, never by a cvar
// or a public caller's claimed readiness. No native initializer/Task is invented.
bool ValidEditorLoaderSource(const GuestRuntimeCallExecutor& calls) {
  const auto& plan = calls.validated();
  const auto module = calls.module();
  if (!module || module->GetProcAddressByOrdinal(0x1A6) != 0x8169F4A8 ||
      module->GetProcAddressByOrdinal(0x1A5) != 0x816A0D70) return false;
  if (!NativeEditorLoaderPlanPairValid(plan)) return false;
  auto lock = global_critical_region::AcquireDirect();
  auto* memory = calls.kernel()->memory();
  for (const auto& body : NativeEditorLoaderCodeProfiles()) {
    if (memory->QueryRangeAccess(body.address,body.bytes) == memory::PageAccess::kNoAccess) return false;
    for (uint32_t at=body.address;at<body.address+body.bytes;at+=4) {
      xex2_section_type section;
      if (calls.kernel()->GetUserModuleForAddress(at).get()!=module.get() ||
          !module->xex_module()->GetPageSectionType(at,&section) || section!=XEX_SECTION_CODE ||
          module->xex_module()->TranslateCode(at)!=memory->TranslateVirtual<const uint8_t*>(at)) return false;
    }
    std::array<uint8_t,32> digest;
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(body.address),body.bytes,digest.data(),0);
    constexpr char hex[]="0123456789abcdef";
    for (size_t n=0;n<digest.size();++n)
      if (body.sha256[n*2]!=hex[digest[n]>>4] || body.sha256[n*2+1]!=hex[digest[n]&15]) return false;
  }
  return true;
}
struct EditorHandoffPayload {
  std::array<uint8_t,0x10C> data{};
  std::array<char,0x104> path{};
  uint32_t path_length=0;
  bool accepted=false;
  // Worker may read the plain payload only after actual public own-return.
  std::atomic<bool> sealed{false};
};
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
// Independent fixed scalar storage. It retains no native owner, State,
// Kernel, PM, code/domain or provider backing across a failed stop/CPU stop.
struct NativeIdentityStopDiagnostic {
 using Observation=NativeProfileIdentityOwner::StopObservation;
 struct Payload {uint32_t phase=0,line=0;Observation drained,retained;};
 struct Cell {std::atomic<uint32_t> state{0};Payload payload;};
 std::array<Cell,2> first;
 struct XmsgPayload {
  uint32_t phase=0,line=0;
  bool available=false,stopping=false,retired=false,result=false;
  uint64_t hooks=0;
 };
 struct XmsgCell {std::atomic<uint32_t> state{0};XmsgPayload payload;} xmsg;
 void CaptureXmsg(uint32_t phase,uint32_t line,bool available,bool stopping,
                  bool retired,bool result,uint64_t hooks) noexcept {
  if(!cvars::native_system_ui_trace)return;
  uint32_t empty=0;
  if(!xmsg.state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
  xmsg.payload={phase,line,available,stopping,retired,result,hooks};
  xmsg.state.store(2,std::memory_order_release);
 }
 void Capture(uint32_t phase,uint32_t line,const Observation& drained,
              const Observation& retained) noexcept {
  if(!cvars::native_system_ui_trace || !phase || phase>first.size())return;
  auto& cell=first[phase-1];uint32_t empty=0;
  if(!cell.state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
  cell.payload={phase,line,drained,retained};
  cell.state.store(2,std::memory_order_release);
 }
};
std::shared_ptr<NativeIdentityStopDiagnostic> PrepareIdentityStopDiagnostic() noexcept {
 try{return std::make_shared<NativeIdentityStopDiagnostic>();}catch(...){return {};}
}
void ReportIdentityStopDiagnosticOnHost(
    const std::shared_ptr<NativeIdentityStopDiagnostic>& diagnostic) noexcept {
 if(!cvars::native_system_ui_trace || XThread::IsInThread() ||
    XThread::GetCurrentFiberThread() ||
    global_critical_region::is_held_by_current_thread())return;
 try{
  static NativeSystemUiTraceLimiter trace;
  if(!diagnostic){
   trace.Emit("identity","stop_conjuncts_unavailable",{{"sink_available",0}});return;
  }
  if(diagnostic->xmsg.state.load(std::memory_order_acquire)==2){
   const auto d=diagnostic->xmsg.payload;
   static NativeSystemUiTraceLimiter xmsg_trace;
   xmsg_trace.Emit("xmsg","stop_retained_predicate",
       {{"phase",d.phase},{"source_line",d.line},{"result_available",d.available},
        {"result",d.result},{"stopping",d.stopping},{"retired",d.retired},
        {"hooks",d.hooks},{"advisory_only",1},{"native_completion",0}});
  }
  for(size_t phase=0;phase<diagnostic->first.size();++phase){
   const auto& cell=diagnostic->first[phase];
   const auto state=cell.state.load(std::memory_order_acquire);
   if(state!=2){
    trace.Emit("identity","stop_conjuncts_unavailable",
        {{"sink_available",1},{"phase",phase+1},{"capture_state",state}});
    continue; // Never wait/read an incomplete writer or rerun a predicate.
   }
   const auto payload=cell.payload;
   const auto emit=[&](uint32_t predicate,const NativeProfileIdentityOwner::StopObservation& d){
    trace.Emit("identity","stop_conjuncts",
        {{"phase",payload.phase},{"source_line",payload.line},{"predicate",predicate},
         {"result_available",d.result_available},{"result",d.result},{"exception",d.exception},
         {"evaluated",d.evaluated},{"truth",d.truth},{"first_failed",d.first_failed},
         {"row_class",d.row_class},{"row_evaluated",d.row_evaluated},{"row_truth",d.row_truth},
         {"counted_visited",d.visited[0]},{"context_refs_visited",d.visited[1]},
         {"task_refs_visited",d.visited[2]},{"execution_retained_visited",d.visited[3]},
         {"active_visited",d.visited[4]},{"advisory_only",1},{"native_completion",0}});
   };
   emit(1,payload.drained);emit(2,payload.retained);
  }
 }catch(...){/* Diagnostic IO never changes the original fail-stop. */}
}
// One service-local first terminal stop failure. The scope is declared before
// existing locks/holders, so publication happens only after they unwind. No
// guest read, allocation, logging, wait or policy result occurs at this cut.
class NativeStopFailureObservation {
 public:
  NativeStopFailureObservation(std::atomic<uint64_t>& first, uint32_t function,
                               uint32_t line) noexcept
      : first_(first), function_(function), line_(line) {}
  ~NativeStopFailureObservation() noexcept {
    if (passed_ || !cvars::native_system_ui_trace) return;
    uint64_t empty = 0;
    first_.compare_exchange_strong(empty, (uint64_t(function_) << 32) | line_,
                                   std::memory_order_relaxed);
  }
  bool Result(uint32_t line, bool passed) noexcept {
    line_ = line;
    passed_ = passed;
    return passed;
  }
 private:
  std::atomic<uint64_t>& first_;
  const uint32_t function_;
  uint32_t line_;
  bool passed_ = false;
};
// Called only after the original failing operation returned and its locks
// unwound, immediately before the existing fail-stop. Values are source-bound
// scalar observations, not a new native completion or stopped-resource proof.
void ReportNativeStopFailure(uint64_t first, uint32_t boundary,
                             uint32_t line) noexcept {
  if (!cvars::native_system_ui_trace || XThread::IsInThread() ||
      XThread::GetCurrentFiberThread() ||
      global_critical_region::is_held_by_current_thread()) return;
  const uint64_t captured = first ? first : (uint64_t(boundary) << 32) | line;
  static NativeSystemUiTraceLimiter trace;
  trace.Emit("service", "stop_first_failure",
             {{"first_function", uint32_t(captured >> 32)},
              {"first_source_line", uint32_t(captured)},
              {"nested_failure_available", first != 0},
              {"boundary_function", boundary},
              {"boundary_source_line", line},
              {"advisory_only", 1}, {"native_completion", 0}});
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
  explicit State(Config value)
      : diagnostic_emulator_identity(value.kernel ? value.kernel->emulator() : nullptr),
        config(std::move(value)) {}
  // Bound while Create owns its valid Kernel, before registry publication.
  // Never read through this pointer and never replace it during retirement.
  const ::xe::Emulator* const diagnostic_emulator_identity;
  Config config;
  // Serializes Start/Resume with actual OS join, never acquired by bootstrap
  // or native callbacks. No metadata mutex crosses a guest call or wait.
  std::mutex lifecycle;
  std::mutex stop;
  mutable std::mutex mutex;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> stop_first_failure{0};  // Diagnostic only; never authority.
  // Allocation failure makes diagnostics unavailable, never service admission.
  const std::shared_ptr<NativeIdentityStopDiagnostic> identity_stop_diagnostic=
      PrepareIdentityStopDiagnostic();
  Snapshot status;
  struct KernelAdmission {
    explicit KernelAdmission(KernelState* kernel):image_observer(kernel) {}
    std::atomic<bool> closed{false};
    std::weak_ptr<NativeSystemUiService> service;
    NativeIdentityTileObserverSlot image_observer;
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
  bool avatar_requested = false;
  std::shared_ptr<NativeAvatarService> avatar;
  bool profile_identity_requested = false;
  std::shared_ptr<const xam::ProfileRosterNativeBinding> profile_identity_binding;
  std::shared_ptr<NativeProfileIdentityOwner> profile_identity;
  std::shared_ptr<NativeProfileIdentityOwner> task_execution;
  std::shared_ptr<NativeMediaService> media;
  std::shared_ptr<NativeContentResolve> content_resolve;
  std::shared_ptr<NativeXmsgProvenance> xmsg_provenance;
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
  struct RetainedStop {
    KernelState* kernel = nullptr;
    object_ref<XHostThread> bootstrap;
    std::shared_ptr<NativeProfileIdentityOwner> identity;
    std::shared_ptr<NativeSystemTaskContinuation> continuation;
    std::shared_ptr<NativeAvatarService> avatar;
    std::shared_ptr<NativeMediaService> media;
    std::shared_ptr<NativeContentResolve> content_resolve;
    std::shared_ptr<NativeXmsgProvenance> xmsg_provenance;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
  };
  // Embedded before native execution: stop needs no allocate-on-failure tail.
  std::optional<RetainedStop> retained_stop;
  struct PublicInvocation {
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    std::shared_ptr<void> transport_operation;
    NativeImportDomain::Operation domain_operation;
    std::shared_ptr<KernelState::UserModuleExecutionLease> caller_code;
    object_ref<XThread> caller;
    std::shared_ptr<NativeUiBackendRoute::HostLease> host_input;
    bool host_input_blocked = false;
    // Exact prebound original call, not a profile/read/Task capability. No
    // observer performs a lookup or reads guest memory. Same actual ThreadState
    // serializes fields; the bound cell is never published to another actor.
    struct SigninObservation : NativeSigninCreationFacts {
      cpu::ppc::PPCContext* context=nullptr;
      cpu::ThreadState* cpu=nullptr;
      XThread* thread=nullptr;KernelState* kernel=nullptr;
      uint32_t pcr=0,process=0,helper_sp=0,packet=0;
    } signin;
    std::shared_ptr<KernelState::UserModuleExecutionLease> signin_root_code;
    // Incomplete observation retains this exact temporary creation receipt;
    // stop never interprets a native0 or a missing hook as completion.
    std::shared_ptr<NativeUiBackendRoute::NativeCreationLease> creation_ingress;
    struct EditorHandoff {
      cpu::ppc::PPCContext* context=nullptr;
      cpu::ThreadState* cpu=nullptr;
      KernelState* kernel=nullptr;
      Emulator* emulator=nullptr;
      XThread* thread=nullptr;  // Retained by the enclosing invocation.
      uint32_t pcr=0,process=0,user=0,flags=0;
      NativeEditorLoaderStack stack;
      const uint8_t* native_data=nullptr;
      const char* native_path=nullptr;
      bool copied=false,requested=false,lost=false;
      std::shared_ptr<EditorHandoffPayload> payload;
      std::optional<EmulatorLifecycle::Producer> producer;
      std::optional<EmulatorLifecycle::PreparedRequest> command;
      std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
    };
    std::unique_ptr<EditorHandoff> editor;

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
    const auto found=registry.find(kernel);
    auto admission=found==registry.end()?nullptr:found->second.lock();
    if (!admission) {
      // No new resident resource may be appended during actual Kernel
      // teardown, including a late stopped call after exact withdrawal.
      if(kernel->guest_scheduler()->shutting_down())return {};
      admission = std::make_shared<KernelAdmission>(kernel);
      // Preserve the established registry -> GCRC order. The aggregate writer
      // reloads this exact metadata slot atomically, without registry lookup.
      kernel->native_identity_tile_observer_slot_.store(
          std::shared_ptr<NativeIdentityTileObserverSlot>(admission,&admission->image_observer),
          std::memory_order_release);
      kernel->RetainNativeResourceUntilTeardown(admission);
      registry[kernel] = admission;
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
    std::shared_ptr<NativeProfileIdentityOwner> identity;
    std::shared_ptr<NativeMediaService> media_owner;
    std::shared_ptr<NativeContentResolve> resolve_owner;
    std::shared_ptr<NativeXmsgProvenance> xmsg_owner;
    std::shared_ptr<NativeAvatarService> avatar_owner;
    std::shared_ptr<NativeManagedFormatFrontdoor> format_owner;
    std::shared_ptr<NativeNotificationPreferenceCoordinator> preference_owner;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared_owner;
    std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic_owner;
    {
      std::lock_guard lock(mutex);
      if (status.failure.empty()) {
        status.failure = std::move(reason);
      }
      status.boot_admitted = status.source_frame_ready =
          status.title_heap_ready = false;
      owner = allocation;
      identity = task_execution;
      media_owner = media;resolve_owner=content_resolve;xmsg_owner=xmsg_provenance;
      if (identity || media_owner || resolve_owner || xmsg_owner) {
        stopping.store(true, std::memory_order_release);
        avatar_owner = avatar;
        format_owner = format_frontdoor;
        preference_owner = preferences;
        prepared_owner = prepared;
        diagnostic_owner = config.diagnostic;
      }
    }
    if (identity || media_owner || resolve_owner || xmsg_owner) {
      // Seal every new ingress without PPC. Captured identity descendants may
      // still need the real observer, Task/provider and owned allocation to
      // return. Uncertain work is retained; Poison never invents its drain.
      if (identity) identity->BeginStop();
      if (media_owner) media_owner->BeginStop();
      if (resolve_owner) resolve_owner->BeginStop();
      if (xmsg_owner) xmsg_owner->BeginStop();
      if (avatar_owner) avatar_owner->BeginStop();
      if (format_owner) format_owner->BeginStop();
      if (preference_owner) preference_owner->BeginStop();
      if (prepared_owner) prepared_owner->CloseExternalRequests();
      if (diagnostic_owner) diagnostic_owner->CloseExternalRequests();
      if ((identity && !identity->Drained()) ||
          (media_owner && !media_owner->Drained()) ||
          (resolve_owner && !resolve_owner->Drained() && !resolve_owner->RetainedForActualKernelStop()) ||
          (xmsg_owner && !xmsg_owner->Drained() && !xmsg_owner->RetainedForActualKernelStop())) return false;
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
    if (!NativeSystemUiService::BindProfileIdentityContinuation(
            state_, continuation) ||
        !NativeSystemUiService::BindMediaContinuation(state_, continuation)) {
      return Fail("Exact native identity/media continuation binding failed");
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
      case NativeSystemUiStartupPhase::kProfileData:
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
    } else if (phase == NativeSystemUiStartupPhase::kProfileData) {
      // Runtime has already executed DB788 and enforced its mandatory native
      // postcondition. A public check/empty snapshot cannot mint this witness.
      auto witness=catalog.TakeInitialEmptyProfileWitness(thread);
      std::shared_ptr<NativeProfileIdentityOwner> identity;
      {std::lock_guard lock(state_->mutex);identity=state_->profile_identity;}
      if(witness && identity)identity->ObserveInitialEmptyProfileBaseline(thread,std::move(*witness));
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
      std::shared_ptr<NativeMediaService> media;
      { std::lock_guard lock(state_->mutex); media = state_->media; }
      std::string media_error;
      if (media && !media->ObserveFeReady(thread, &media_error))
        return Fail("Actual existing FA owner observation: " + media_error);
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
      config.kernel->guest_scheduler()->shutting_down() ||
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
    if (!admission || admission->closed.load(std::memory_order_acquire) ||
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
        ReportNativeStopFailure(
            service->state_->stop_first_failure.load(std::memory_order_relaxed),
            7, __LINE__);
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
  bool native_startup_observed = false;
  // Bootstrap-local first failed stage only; no guest lookup or logging at the
  // failure cut. fail() includes it in the first published reason before cleanup.
  const char* first_progress_failure_stage = nullptr;
  const auto progress_failed = [&](const char* stage) {
    if (!first_progress_failure_stage) first_progress_failure_stage = stage;
    return false;
  };
  const auto progress_owned = [&] {
    std::shared_ptr<NativeProfileIdentityOwner> identity;
    std::shared_ptr<NativeMediaService> media;
    std::shared_ptr<NativeAvatarService> diagnostic_avatar;
    { std::lock_guard lock(state->mutex); identity = state->task_execution; media = state->media;
      diagnostic_avatar=state->avatar; }
    using Handoff=NativeProfileIdentityOwner::HandoffStage;
    const auto observe=[&](Handoff stage){if(identity)identity->ObserveBootstrapHandoff(stage);};
    if(identity)identity->ObserveBootstrapHandoff(Handoff::kOwnersCaptured,true);
    // Existing actual OS Bootstrap only, outside all owner/PM/native locks.
    if(thread && XThread::IsInThread(thread) && !XThread::GetCurrentFiberThread() &&
       !global_critical_region::is_held_by_current_thread()){
      if(identity){observe(Handoff::kAlignmentReport);identity->ReportAlignmentOnHost("Bootstrap");observe(Handoff::kAlignmentReported);}
      observe(Handoff::kProfileReport);
      state->config.kernel->xam_state()->profile_manager()->ReportNativeProfileDiagnostics("Bootstrap");
      observe(Handoff::kProfileReported);
      if(diagnostic_avatar){observe(Handoff::kAvatarReport);diagnostic_avatar->ReportDiagnosticsOnHost("Bootstrap");observe(Handoff::kAvatarReported);}
    }
    if (!identity && !media) return true;
    if(identity && !identity->HasProfileIdentityAuthority() && !media)
      return thread && identity->StepOnBootstrap(*thread);
    if (!native_startup_observed || !thread){
      observe(Handoff::kStartupUnavailable);
      return progress_failed("native-startup-or-bootstrap-unavailable");
    }
    // One fair original-bootstrap turn services each owner before the delay.
    // Both use the SAME actual Schedule/Wait/Close continuation transport.
    observe(Handoff::kStepGate);
    if (identity && !(state->IsStopping() &&
        (identity->Drained() || identity->RetainedForActualKernelStop()))){
      observe(Handoff::kStepCall);
      const bool stepped=identity->StepOnBootstrap(*thread);
      observe(stepped?Handoff::kStepReturnedTrue:Handoff::kStepReturnedFalse);
      if(!stepped)return progress_failed("identity-step");
    }
    if (media){
      observe(Handoff::kMediaStep);
      const bool stepped=media->StepOnBootstrap(*thread);
      observe(stepped?Handoff::kMediaReturnedTrue:Handoff::kMediaReturnedFalse);
      if(!stepped)return progress_failed("media-step");
    }
    observe(Handoff::kProgressReturned);
    return true;
  };
  const auto stop_owned_on_bootstrap = [&] {
    NativeStopFailureObservation stop_observation(
        state->stop_first_failure, 1, __LINE__);
    std::shared_ptr<NativeProfileIdentityOwner> identity;
    std::shared_ptr<NativeMediaService> media;
    std::shared_ptr<NativeAvatarService> avatar;
    std::shared_ptr<NativeContentResolve> resolve;
    std::shared_ptr<NativeXmsgProvenance> xmsg;
    {
      std::lock_guard lock(state->mutex);
      identity = state->task_execution; media = state->media; avatar = state->avatar;
      resolve=state->content_resolve;xmsg=state->xmsg_provenance;
    }
    if (!identity && !media && !avatar && !resolve && !xmsg) return stop_observation.Result(__LINE__, true);
    state->stopping.store(true, std::memory_order_release);
    if (identity && thread) identity->ReportFirstRefusalOnBootstrap(*thread);
    if (identity) identity->BeginStop();
    if (media) media->BeginStop();
    if (resolve) resolve->BeginStop();
    if (xmsg) xmsg->BeginStop();
    if (avatar) avatar->BeginStop();
    if (!thread) return stop_observation.Result(__LINE__, false);
    NativeInputDriver::SealCallbackAdmissionForKernel(state->config.kernel);
    if (!NativeInputDriver::DrainCallbackAdmissionForKernel(state->config.kernel)) return stop_observation.Result(__LINE__, false);
    // Periodic callbacks returning normally do not stop their native Task.
    // Execute its authenticated original Cancel/Wait/Close while SYSTEM,
    // providers and allocation remain live, before waiting on host ownership.
    if (identity && !identity->StopPeriodicOnBootstrap(*thread)) return stop_observation.Result(__LINE__, false);
    const auto owned_drained = [&] {
      return (!identity || identity->Drained() || identity->RetainedForActualKernelStop()) &&
             (!media || media->Drained()) &&
             (!resolve || resolve->Drained() || resolve->RetainedForActualKernelStop()) &&
             (!xmsg || xmsg->Drained() || xmsg->RetainedForActualKernelStop());
    };
    if (!native_startup_observed && !owned_drained())
      return stop_observation.Result(__LINE__, false);  // Partial FE cannot mint a usable native Task.
    if (native_startup_observed) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!owned_drained()) {
        if (!progress_owned()) return stop_observation.Result(__LINE__, false);
        if (owned_drained()) break;
        if (std::chrono::steady_clock::now() >= deadline) return stop_observation.Result(__LINE__, false);
        thread->Delay(0, 0, uint64_t(-160000ll));
      }
      if (identity && !identity->Drained() &&
          !identity->RetainedForActualKernelStop() &&
          !identity->StopOnBootstrap(*thread)) return stop_observation.Result(__LINE__, false);
      if (media && !media->StopOnBootstrap(*thread)) return stop_observation.Result(__LINE__, false);
    }
    // Media Drained is execution quiescence, not a claimed successful Close.
    // Normally-returned failed backend closes retain their body/source until
    // the actual post-CPU/GPU-stop hook, without preventing that stop.
    return stop_observation.Result(
        __LINE__, !avatar || avatar->ShutdownOnBootstrap(*thread));
  };
  const auto fail = [&](std::string text) {
    if (first_progress_failure_stage) {
      text += "; first owned progress failure stage=";
      text += first_progress_failure_stage;
    }
    // Preserve the first actual failure before native cleanup may wait. The
    // external coordinator can observe stopping during that cleanup; an empty
    // reason must not turn this failure into an apparent backend failure.
    {
      std::lock_guard lock(state->mutex);
      if (state->status.failure.empty()) {
        state->status.failure = text;
      }
    }
    // Healthy actual bootstrap only. Poison itself never executes PPC and
    // cannot seal allocation beneath undrained captured identity ownership.
    if (!stop_owned_on_bootstrap())
      text += "; native owned cleanup uncertain (ownership retained)";
    state->Poison(std::move(text));
    return 1;
  };
  if (!thread || !ActualSystem(*thread, state->config.kernel)) {
    return fail("Bootstrap is not its actual SYSTEM XHostThread");
  }
  const auto finish_owners = [&] {
    bool retained = false;
    {
      std::lock_guard lock(state->mutex);
      retained = bool(state->task_execution) || bool(state->media) || bool(state->avatar) ||
          bool(state->content_resolve) || bool(state->xmsg_provenance);
    }
    if (!retained) return true;
    // Shared original-bootstrap progress prevents an avatar idle wait from
    // starving identity jobs. No unrelated BootAdmitted/title-input wait here.
    while (!state->IsStopping()) {
      if (!progress_owned()) return false;
      thread->Delay(0, 0, uint64_t(-160000ll));
    }
    return stop_owned_on_bootstrap();
  };
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
    {std::lock_guard lock(state->mutex);
      state->avatar_requested=CheckNativeAvatarPlanFamily(calls->validated(),&error)==
          NativeAvatarPlanFamily::kComplete;}
    const auto identity_family =
        CheckNativeProfileIdentityPlanFamily(calls->validated(), &error);
    if (identity_family == NativeProfileIdentityPlanFamily::kInvalid)
      return fail("Invalid native identity plan family: " + error);
    if (identity_family == NativeProfileIdentityPlanFamily::kComplete) {
      const auto pm = state->config.kernel->xam_state()->profile_manager();
      auto binding = pm ? pm->ClaimNativeRosterFactory() : nullptr;
      if (!binding || !NativeProfileIdentityOwner::ClaimKernelBeforeHeap(
                          *state->config.kernel, *calls, binding))
        return fail("Exact pre-heap native identity factory claim failed");
      std::lock_guard lock(state->mutex);
      state->profile_identity_requested = true;
      state->profile_identity_binding = std::move(binding);
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
    std::shared_ptr<const xam::ProfileRosterNativeBinding> identity_binding;
    {
      std::lock_guard lock(state->mutex);
      identity_binding = state->profile_identity_binding;
    }
    auto execution = identity_binding ?
        NativeProfileIdentityOwner::CreateBeforeWorkers(
            {catalog,domain,allocation,notification_observer,identity_binding},&error):
        NativeProfileIdentityOwner::CreateExecutionBeforeWorkers(
            {catalog,domain,allocation,{},{}},&error);
    if(!execution)return fail("Native shared Task execution owner install: "+error);
    std::shared_ptr<NativeIdentityTileObserverSlot> image_slot;
    {std::lock_guard lock(state->mutex);if(state->admission)
      image_slot=std::shared_ptr<NativeIdentityTileObserverSlot>(state->admission,
          &state->admission->image_observer);}
    // Observation unavailability never rejects the original native startup.
    execution->BindImageObserverSlot(image_slot);
    {
      std::lock_guard lock(state->mutex);
      state->task_execution=execution;
      if(identity_binding)state->profile_identity=execution;
    }
    if(state->IsStopping()){
      execution->BeginStop();return fail("Service stopped after shared Task owner install");
    }
    auto xmsg=NativeXmsgProvenance::CreateBeforeWorkers({catalog,domain,allocation},&error);
    if(!xmsg)return fail("Common native XMsg provenance owner install: "+error);
    {std::lock_guard lock(state->mutex);state->xmsg_provenance=xmsg;}
    if(state->IsStopping()){
      xmsg->BeginStop();return fail("Service stopped after XMsg provenance install");
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
    if (catalog->system_content()) {
      auto resolve = NativeContentResolve::CreateBeforeWorkers(
          {catalog, domain, allocation}, &error);
      if (!resolve) return fail("Native content Resolve ownership: " + error);
      { std::lock_guard lock(state->mutex); state->content_resolve = resolve; }
      if (state->IsStopping()) resolve->BeginStop();
    }
    if (catalog->system_content()) {
      auto media = NativeMediaService::CreateBeforeWorkers({catalog, domain, allocation}, &error);
      if (!media) return fail("Native media retained producer ownership: " + error);
      { std::lock_guard lock(state->mutex); state->media = media; }
      if (state->IsStopping()) media->BeginStop();
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
    if(state->avatar_requested){
      const auto pm=state->config.kernel->xam_state()->profile_manager();
      auto avatar=NativeAvatarService::Create({state->config.kernel,calls,domain,allocation,
          catalog->system_content(),pm?pm->CaptureProfileSettingsReadProvider():nullptr,
          state->config.prepared_title},&error);
      if(!avatar)return fail("Native avatar complete provider/root ownership: "+error);
      {std::lock_guard lock(state->mutex);state->avatar=avatar;}
      if(state->IsStopping())avatar->BeginStop();
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
    native_startup_observed = true;  // Actual FE/startup result, not public readiness.
    // Native CRT deliberately leaves heap0 disabled. Its genuine per-Title
    // initializer belongs here, after native boot and before original public
    // calls/diagnostic Guide or the prepared Main can execute. Never attach it
    // lazily to2C1, change allocation flags, or fabricate native Task context.
    // The same once record also calls the real loader metadata setter: host
    // XEX loading replaces the console loader path that normally invokes it.
    // Both complete before initial profile alignment or Title Main/public UI.
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
    const auto title_setup = title_runtime->Initialize(
        catalog->PreparedTitlePlan(expected_title),
        [&](const GuestRuntime::Function& function) {
          if (state->IsStopping()) {
            return Fail("Prepared Title initialization stopped before entry");
          }
          if (function.address == 0x816E6870) {
            return catalog->InitializePreparedTitleHeap(*thread, expected_title);
          }
          if (function.address == 0x8169BDD0) {
            return catalog->InitializePreparedTitleLoader(*thread, expected_title);
          }
          return Fail("Prepared Title role is not in the compiled profile");
        });
    if (title_setup != GuestRuntime::InitStatus::kReady || state->IsStopping()) {
      // Preserve the once record's actual failure. An absent post-call trace
      // is not proof that PPC was never entered. Diagnostic errors must not
      // change the original failure/poison path or retry the native call.
      try {
        const auto failed = title_runtime->GetSnapshot();
        if (failed.failure) {
          XELOGE(
              "Native prepared Title transaction: init_status={} "
              "state={} functions={}/{} stopping={} phase={} function={} "
              "return={:08X} detail={}",
              uint32_t(title_setup), uint32_t(failed.state),
              failed.completed_functions, failed.total_functions,
              state->IsStopping(), failed.failure->position.phase,
              failed.failure->position.function, failed.failure->return_value,
              failed.failure->detail);
        } else {
          XELOGE(
              "Native prepared Title transaction: init_status={} "
              "state={} functions={}/{} stopping={} failure_detail_known=0",
              uint32_t(title_setup), uint32_t(failed.state),
              failed.completed_functions, failed.total_functions,
              state->IsStopping());
        }
      } catch (...) {
      }
      return fail("Native prepared Title initialization failed/uncertain");
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
      if (!progress_owned())
        return fail("Native owned post-FE progress failed");
      const auto observed = state->config.diagnostic->ObserveAfterNativeBoot(
          state->Context(), *thread);
      if (!observed.succeeded && !state->IsStopping()) {
        return fail("Native first-record observation: " + observed.detail);
      }
      return finish_owners() ? 0 : fail("Native owned bootstrap drain failed");
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
      if (!progress_owned())
        return fail("Native owned bootstrap progress failed");
      if (ObserveSharedGraphics(state, *thread, false)) {
        break;
      }
      bool identity_failure = false;
      {
        std::lock_guard lock(state->mutex);
        if (!state->status.failure.empty()) {
          if (!state->profile_identity && !state->media) return 1;
          identity_failure = true;
        }
      }
      if (identity_failure)
        return fail("Native owned shared-graphics progress failed");
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
        if (!progress_owned())
          return fail("Native owned bootstrap progress failed");
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
      if (state->IsStopping())
        return finish_owners() ? 0 : fail("Native owned bootstrap drain failed");
      if (!input_ready) return fail("Render diagnostic real Title input unavailable");
      const auto observed = InvokeFixedNotificationDiagnostic(
          state->config.kernel, *thread);
      if (!observed.succeeded && !state->IsStopping())
        return fail("Production notification diagnostic: " + observed.detail);
    }
    return finish_owners() ? 0 : fail("Native owned bootstrap drain failed");
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
  KernelState* kernel = nullptr;
  std::shared_ptr<NativeSystemTaskContinuation> continuation;
  std::shared_ptr<NativeManagedFormatFrontdoor> format_frontdoor;
  std::shared_ptr<NativeAvatarService> avatar;
  std::shared_ptr<NativeProfileIdentityOwner> identity;
  std::shared_ptr<NativeMediaService> media;
  std::shared_ptr<NativeContentResolve> content_resolve;
  std::shared_ptr<NativeXmsgProvenance> xmsg_provenance;
  std::shared_ptr<NativeNotificationPreferenceCoordinator> preferences;
  std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
  std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
  std::shared_ptr<NativeNotificationPresentationObserver> observer;
  {
    std::lock_guard lock(state_->mutex);
    kernel = state_->config.kernel;
    continuation = state_->continuation;
    format_frontdoor = state_->format_frontdoor;
    avatar = state_->avatar;
    identity = state_->task_execution;
    media = state_->media;
    content_resolve = state_->content_resolve;
    xmsg_provenance = state_->xmsg_provenance;
    prepared = state_->prepared;
    diagnostic = state_->config.diagnostic;
    observer = state_->notification_observer;
    preferences = state_->preferences;
    if (!preferences && state_->config.kernel) {
      preferences = state_->config.kernel->notification_preferences();
    }
  }
  // Seal NEW input callbacks only. Keep the real driver poll/output lane and
  // dispatcher live for F4660/owned native cleanup; full BeginStop/Drain stays
  // in Emulator after this native host-frame/owner phase. No history rewrite
  // or synthetic physical disconnect is performed here.
  if (kernel) {
    kernel->SealStoragePreparation();
    NativeInputDriver::SealCallbackAdmissionForKernel(kernel);
  }
  if (continuation) continuation->BeginStop();
  if (identity) identity->BeginStop();
  if (media) media->BeginStop();
  if (content_resolve) content_resolve->BeginStop();
  if (xmsg_provenance) xmsg_provenance->BeginStop();
  if (avatar) avatar->BeginStop();
  if (format_frontdoor) format_frontdoor->BeginStop();
  if (preferences) {
    preferences->BeginStop();
  }
  // Captured identity frames/clears still require real Set/storage observation.
  // StopBeforeCpuAndProviders stops this observer only AFTER identity Drained.
  if (observer && !identity && !media) {
    observer->BeginStop();
  }
  if (prepared) {
    prepared->CloseExternalRequests();
  }
  if (diagnostic) {
    diagnostic->CloseExternalRequests();
  }
}
void NativeSystemUiService::ReportForEmulatorOnHostExit(
    const ::xe::Emulator* emulator) noexcept {
 if(!cvars::native_system_ui_trace || XThread::IsInThread() ||
    XThread::GetCurrentFiberThread() || global_critical_region::is_held_by_current_thread())return;
 try{
  struct Captured {
   KernelState* kernel_identity=nullptr;
   std::shared_ptr<State::KernelAdmission> admission;
   std::shared_ptr<NativeSystemUiService> service;
   std::shared_ptr<NativeProfileIdentityOwner> identity;
   std::shared_ptr<NativeAvatarService> avatar;
   bool matches=false;
  };
  // All strong holders are declared BEFORE locks. Even a nonmatching/retired
  // last Service/admission reference is released only after every lock exits.
  std::array<Captured,16> captured{};
  size_t registry_entries=0,count=0,matches=0,selected=0;
  bool complete=emulator!=nullptr;
  if(emulator){
   std::lock_guard registry_lock(State::registry_mutex);
   registry_entries=State::registry.size();complete=registry_entries<=captured.size();
   for(const auto& [kernel_key,weak]:State::registry){
    if(count==captured.size())break;
    auto& item=captured[count++];item.kernel_identity=kernel_key;
    item.admission=weak.lock();
    item.service=item.admission?item.admission->service.lock():nullptr;
    if(!item.service)continue;
    auto& state=*item.service->state_;
    std::lock_guard owner_lock(state.mutex);
    // Retired is checked first: config/admission resource fields may be
    // revoked after its original mutex-protected retirement publication.
    if(state.retired || state.diagnostic_emulator_identity!=emulator)continue;
    item.matches=state.admission==item.admission && state.config.kernel==kernel_key;
    if(item.matches){
     item.identity=state.profile_identity;item.avatar=state.avatar;
     selected=count-1;++matches;
    }
   }
  }
  // No Kernel/PM/Emulator backing is accessed. The captured owner methods
  // inspect only their synchronized scalar metadata, even if real teardown
  // concurrently revokes their code/domain/provider holders in place.
  XELOGI("Native GUI exit diagnostic snapshot: external_host=1 live_snapshot=1 "
         "stopped_proof=0 emulator_identity={:016X} registry_entries={} "
         "captured={} inventory_complete={} matching_captured_services={} ambiguous={}; advisory only",
         reinterpret_cast<uintptr_t>(emulator),registry_entries,count,complete,matches,matches>1);
  if(!complete || matches!=1)return; // Explicit unavailable; never guess Current.
  const auto& item=captured[selected];
  XELOGI("Native GUI exit diagnostic owner: service_identity={:016X} "
         "admission_identity={:016X} kernel_identity={:016X} "
         "identity_present={} avatar_present={}; live metadata, not native drain",
         reinterpret_cast<uintptr_t>(item.service.get()),
         reinterpret_cast<uintptr_t>(item.admission.get()),
         reinterpret_cast<uintptr_t>(item.kernel_identity),bool(item.identity),bool(item.avatar));
  if(item.identity){
   item.identity->ReportFirstRefusalOnCoordinator();
   item.identity->ReportAlignmentOnHost("GuiExit");
  }
  if(item.avatar)item.avatar->ReportDiagnosticsOnHost("GuiExit");
 }catch(...){/* Advisory only; preserve all original GUI/async shutdown behavior. */}
}

void NativeSystemUiService::ReportProfileIdentityRefusalOnCoordinator(
    KernelState* kernel,const char* boundary) const noexcept {
 if(!kernel || XThread::IsInThread() || XThread::GetCurrentFiberThread() ||
    global_critical_region::is_held_by_current_thread())return;
 try{
  std::shared_ptr<State::KernelAdmission> admission;
  std::shared_ptr<NativeSystemUiService> registered;
  std::shared_ptr<NativeProfileIdentityOwner> identity;
  std::shared_ptr<NativeAvatarService> avatar;
  bool current_service=false,same_kernel=false,diagnostic_current=false;
  {
   // Hold only existing metadata locks while observing the exact old entry;
   // no allocation, native call, wait, teardown or diagnostic IO in this scope.
   std::lock_guard registry_lock(State::registry_mutex);
   const auto found=State::registry.find(kernel);
   admission=found==State::registry.end()?nullptr:found->second.lock();
   registered=admission?admission->service.lock():nullptr;
   std::lock_guard owner_lock(state_->mutex);
   current_service=registered.get()==this && admission && admission==state_->admission;
   same_kernel=state_->config.kernel==kernel;
   diagnostic_current=current_service && same_kernel && !state_->retired;
   if(diagnostic_current){identity=state_->profile_identity;avatar=state_->avatar;}
  }
  // The external Emulator coordinator retains this exact Service/Kernel.
  // All metadata locks are gone before either log, even on absent-owner paths.
  XELOGE("Native identity coordinator diagnostic: external_host=1 "
         "current_service={} same_kernel={} identity_present={}; advisory only",
         current_service,same_kernel,bool(identity));
  if(identity){
    if(!boundary || std::strcmp(boundary,"ForegroundReady"))identity->ReportFirstRefusalOnCoordinator();
    identity->ReportAlignmentOnHost(boundary);
  }
  if(diagnostic_current){
    kernel->xam_state()->profile_manager()->ReportNativeProfileDiagnostics(boundary);
    if(avatar)avatar->ReportDiagnosticsOnHost(boundary);
  }
 }catch(...){/* Advisory only: never alter startup failure or its stop path. */}
}

std::shared_ptr<NativeProfileIdentityOwner>
NativeSystemUiService::CaptureProfileIdentityOwner(KernelState* kernel) {
  std::shared_ptr<NativeSystemUiService> service;
  {
    std::lock_guard lock(State::registry_mutex);
    const auto it = State::registry.find(kernel);
    const auto admission = it == State::registry.end() ? nullptr : it->second.lock();
    service = admission ? admission->service.lock() : nullptr;
  }
  if (!service) return {};
  std::lock_guard lock(service->state_->mutex);
  return service->state_->retired || service->state_->config.kernel != kernel
             ? nullptr : service->state_->profile_identity;
}
std::shared_ptr<NativeProfileIdentityOwner>
NativeSystemUiService::CaptureTaskExecutionOwner(KernelState* kernel) {
  std::shared_ptr<NativeSystemUiService> service;
  {
    std::lock_guard lock(State::registry_mutex);
    const auto it = State::registry.find(kernel);
    const auto admission = it == State::registry.end() ? nullptr : it->second.lock();
    service = admission ? admission->service.lock() : nullptr;
  }
  if (!service) return {};
  std::lock_guard lock(service->state_->mutex);
  return service->state_->retired || service->state_->config.kernel != kernel
             ? nullptr : service->state_->task_execution;
}
bool NativeSystemUiService::BindProfileIdentityContinuation(
    const std::shared_ptr<State>& state,
    const std::shared_ptr<NativeSystemTaskContinuation>& continuation) {
  std::shared_ptr<NativeProfileIdentityOwner> identity;
  { std::lock_guard lock(state->mutex); identity = state->task_execution; }
  return !identity || identity->BindContinuation(continuation);
}
std::shared_ptr<NativeMediaService> NativeSystemUiService::CaptureMediaService(KernelState* kernel) {
  std::shared_ptr<NativeSystemUiService> service;
  { std::lock_guard lock(State::registry_mutex); const auto found=State::registry.find(kernel);
    const auto admission=found==State::registry.end()?nullptr:found->second.lock();
    service=admission?admission->service.lock():nullptr; }
  if(!service)return {};
  std::lock_guard lock(service->state_->mutex);
  return service->state_->retired || service->state_->config.kernel!=kernel?nullptr:service->state_->media;
}
bool NativeSystemUiService::BindMediaContinuation(const std::shared_ptr<State>& state,
    const std::shared_ptr<NativeSystemTaskContinuation>& continuation) {
  std::shared_ptr<NativeMediaService> media;
  { std::lock_guard lock(state->mutex); media=state->media; }
  return !media || media->BindContinuation(continuation);
}
std::shared_ptr<NativeAvatarService> NativeSystemUiService::CaptureAvatarService(KernelState* kernel){
  std::shared_ptr<NativeSystemUiService> service;
  {std::lock_guard lock(State::registry_mutex);const auto it=State::registry.find(kernel);
    const auto admission=it==State::registry.end()?nullptr:it->second.lock();
    service=admission?admission->service.lock():nullptr;}
  if(!service)return {};
  std::lock_guard lock(service->state_->mutex);
  return service->state_->retired || service->state_->config.kernel!=kernel
             ? nullptr : service->state_->avatar;
}
bool NativeSystemUiService::AvatarFamilyRequested(KernelState* kernel){
  std::shared_ptr<NativeSystemUiService> service;
  {std::lock_guard lock(State::registry_mutex);const auto it=State::registry.find(kernel);
    const auto admission=it==State::registry.end()?nullptr:it->second.lock();
    service=admission?admission->service.lock():nullptr;}
  if(!service)return false;
  std::lock_guard lock(service->state_->mutex);return service->state_->avatar_requested;
}
bool NativeSystemUiService::JoinBootstrap(
    std::chrono::milliseconds timeout) noexcept {
  NativeStopFailureObservation stop_observation(
      state_->stop_first_failure, 2, __LINE__);
  try {
    if (XThread::IsInThread()) {
      return stop_observation.Result(__LINE__, false);
    }
    std::lock_guard lifecycle(state_->lifecycle);
    BeginStop();
    KernelState* kernel;
    {std::lock_guard lock(state_->mutex);kernel=state_->config.kernel;}
    // Input provider polls/output remain live for genuine native cleanup.
    // Join only their already-reserved callbacks before the Bootstrap stop tail.
    if (kernel && !NativeInputDriver::DrainCallbackAdmissionForKernel(kernel)) return stop_observation.Result(__LINE__, false);
    object_ref<XHostThread> bootstrap;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->status.bootstrap_joined) {
        return stop_observation.Result(__LINE__, true);
      }
      bootstrap = state_->bootstrap;
    }
    if (bootstrap) {
      auto* native_thread = bootstrap->thread();
      if (!native_thread ||
          xe::threading::Wait(native_thread, false, timeout) !=
              xe::threading::WaitResult::kSuccess) {
        return stop_observation.Result(__LINE__, false);
      }
    }
    // OS wait joins the actual host thread, including Exit/cleanup tails. Its
    // guest KTHREAD signal or merely returning from Bootstrap is insufficient.
    BeginStop();  // Also closes a prepared transport published during the join.
    std::lock_guard lock(state_->mutex);
    state_->status.bootstrap_joined = true;
    return stop_observation.Result(__LINE__, true);
  } catch (...) {
    return stop_observation.Result(__LINE__, false);
  }
}
bool NativeSystemUiService::StopBeforeCpuAndProviders() noexcept {
  NativeStopFailureObservation stop_observation(
      state_->stop_first_failure, 3, __LINE__);
  try {
    if (XThread::IsInThread()) {
      return stop_observation.Result(__LINE__, false);
    }
    const auto identity_stop_diagnostic=state_->identity_stop_diagnostic;
    std::lock_guard stop(state_->stop);
    if (!JoinBootstrap()) {
      return stop_observation.Result(__LINE__, false);
    }
    std::shared_ptr<NativeManagedFormatFrontdoor> format_frontdoor;
    std::shared_ptr<NativeSystemTaskContinuation> continuation;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
    std::shared_ptr<NativeContentStorageDriver> storage;
    std::shared_ptr<NativeContentResolve> content_resolve;
    std::shared_ptr<NativeXmsgProvenance> xmsg_provenance;
    std::shared_ptr<NativeNotificationPresentationObserver> observer;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->status.stop_drained || state_->retained_stop) {
        return stop_observation.Result(__LINE__, true);
      }
      format_frontdoor = state_->format_frontdoor;
      continuation = state_->continuation;
      prepared = state_->prepared;
      diagnostic = state_->config.diagnostic;
      allocation = state_->allocation;
      storage = state_->storage;
      content_resolve = state_->content_resolve;
      xmsg_provenance = state_->xmsg_provenance;
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
        return stop_observation.Result(__LINE__, false);
      }
    }
    // All public original-caller C++ invocations have returned while providers
    // remain live. Only separately certified heap-owned identity work may be
    // retained across actual CPU stop; never an unknown host/STARTUP frame.
    // Bootstrap creation is now sealed and its entire C++ stack joined, so a
    // late continuation install cannot escape this snapshot/rundown.
    // Native Task/Complete stay runnable for normal cleanup. Covered retained
    // work remains explicitly unjoined to actual stop; this grants no success.
    const auto identity = CaptureTaskExecutionOwner(state_->config.kernel);
    NativeProfileIdentityOwner::StopObservation identity_drain_observation,
        identity_retained_observation;
    const bool observe_identity=bool(identity_stop_diagnostic) && cvars::native_system_ui_trace;
    const bool identity_drained = !identity || identity->Drained(
        observe_identity?&identity_drain_observation:nullptr);
    if (!identity_drained && !identity->RetainedForActualKernelStop(
        observe_identity?&identity_retained_observation:nullptr)) {
      if(identity_stop_diagnostic)identity_stop_diagnostic->Capture(
          1,__LINE__,identity_drain_observation,identity_retained_observation);
      return stop_observation.Result(__LINE__, false);
    }
    const auto avatar=CaptureAvatarService(state_->config.kernel);
    if(avatar && !avatar->Drained())return stop_observation.Result(__LINE__, false);
    const auto media=CaptureMediaService(state_->config.kernel);
    if(media && !media->Drained())return stop_observation.Result(__LINE__, false);
    const bool resolve_drained=!content_resolve || content_resolve->Drained();
    if(!resolve_drained && !content_resolve->RetainedForActualKernelStop())return stop_observation.Result(__LINE__, false);
    NativeXmsgProvenance::RetainedStopObservation xmsg_stop_observation;
    const bool xmsg_drained=!xmsg_provenance || xmsg_provenance->Drained();
    if(!xmsg_drained && !xmsg_provenance->RetainedForActualKernelStop(
        observe_identity?&xmsg_stop_observation:nullptr)){
      const uint32_t line=__LINE__;
      if(identity_stop_diagnostic)identity_stop_diagnostic->CaptureXmsg(1,line,
          xmsg_stop_observation.available,xmsg_stop_observation.stopping,
          xmsg_stop_observation.retired,xmsg_stop_observation.result,xmsg_stop_observation.hooks);
      return stop_observation.Result(line, false);
    }
    if (format_frontdoor && !format_frontdoor->Drain()) return stop_observation.Result(__LINE__, false);
    bool continuation_drained = true;
    if (continuation) {
      continuation->BeginStop();
      continuation_drained = continuation->Drain();
      if (!continuation_drained && !continuation->RetainedForActualKernelStop()) return stop_observation.Result(__LINE__, false);
    }
    if (storage) {
      storage->BeginStop();
      if (!storage->Drain()) {
        return stop_observation.Result(__LINE__, false);
      }
    }
    if (prepared) {
      prepared->BeginStopTransport();
      if (!prepared->DrainCallbacksBeforeCpuStop()) {
        return stop_observation.Result(__LINE__, false);
      }
    }
    if (diagnostic) {
      diagnostic->BeginStopTransport();
      if (!diagnostic->DrainCallbacksBeforeCpuStop()) {
        return stop_observation.Result(__LINE__, false);
      }
    }
    if (observer) {
      observer->BeginStop();
      if (!observer->Drain()) {
        return stop_observation.Result(__LINE__, false);
      }
    }
    if (allocation) {
      allocation->CloseAdmission();
    }
    // Recheck the exact identity owner after all host callback drains. Strong
    // holders below are a retained-stop fact, never normal native completion.
    NativeProfileIdentityOwner::StopObservation identity_retained_late_observation;
    if (!identity_drained && !identity->RetainedForActualKernelStop(
        observe_identity?&identity_retained_late_observation:nullptr)) {
      if(identity_stop_diagnostic)identity_stop_diagnostic->Capture(
          2,__LINE__,identity_drain_observation,identity_retained_late_observation);
      return stop_observation.Result(__LINE__, false);
    }
    if(!resolve_drained && !content_resolve->RetainedForActualKernelStop())return stop_observation.Result(__LINE__, false);
    if(!xmsg_drained && !xmsg_provenance->RetainedForActualKernelStop(
        observe_identity?&xmsg_stop_observation:nullptr)){
      const uint32_t line=__LINE__;
      if(identity_stop_diagnostic)identity_stop_diagnostic->CaptureXmsg(2,line,
          xmsg_stop_observation.available,xmsg_stop_observation.stopping,
          xmsg_stop_observation.retired,xmsg_stop_observation.result,xmsg_stop_observation.hooks);
      return stop_observation.Result(line, false);
    }
    std::lock_guard lock(state_->mutex);
    if (state_->config.kernel == nullptr || state_->task_execution != identity ||
        state_->continuation != continuation || state_->content_resolve!=content_resolve ||
        state_->xmsg_provenance!=xmsg_provenance || !state_->status.bootstrap_joined) return stop_observation.Result(__LINE__, false);
    if (identity_drained && continuation_drained && resolve_drained && xmsg_drained) {
      state_->status.stop_drained = true;
    } else {
      state_->retained_stop.emplace(State::RetainedStop{
          state_->config.kernel,state_->bootstrap,identity,continuation,avatar,media,content_resolve,xmsg_provenance,
          state_->allocation,state_->domain,state_->catalog,state_->calls,
          state_->prepared,state_->config.diagnostic});
      state_->status.stop_retained = true;
    }
    return stop_observation.Result(__LINE__, true);
  } catch (...) {
    return stop_observation.Result(__LINE__, false);
  }
}
bool NativeSystemUiService::RetireAfterKernelStop() noexcept {
  NativeStopFailureObservation stop_observation(
      state_->stop_first_failure, 4, __LINE__);
  try {
    KernelState* kernel;
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    std::shared_ptr<NativeSystemUiDiagnosticObservation> diagnostic;
    std::shared_ptr<NativeSystemUiStartupRuntime> startup;
    std::shared_ptr<NativeProfileIdentityOwner> identity;
    std::shared_ptr<NativeMediaService> media;
    std::shared_ptr<NativeContentResolve> content_resolve;
    std::shared_ptr<NativeXmsgProvenance> xmsg_provenance;
    std::shared_ptr<NativeSystemTaskContinuation> continuation_owner;
    std::shared_ptr<GuestRuntime> title_runtime;
    gpu::GpuBackendStopProof stop;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->retired) {
        return stop_observation.Result(__LINE__, true);
      }
      if (!state_->status.bootstrap_joined ||
          (!state_->status.stop_drained && !state_->retained_stop) ||
          !state_->IsStopping() || state_->public_calls ||
          state_->public_uncertain || !state_->public_invocations.empty()) {
        return stop_observation.Result(__LINE__, false);
      }
      kernel = state_->config.kernel;
      if (state_->retained_stop) {
        const auto& held = *state_->retained_stop;
        if (held.kernel != kernel || held.bootstrap.get() != state_->bootstrap.get() ||
            held.identity != state_->task_execution || held.continuation != state_->continuation ||
            held.avatar != state_->avatar || held.media != state_->media ||
            held.content_resolve != state_->content_resolve ||
            held.xmsg_provenance != state_->xmsg_provenance ||
            held.allocation != state_->allocation || held.domain != state_->domain ||
            held.catalog != state_->catalog || held.calls != state_->calls ||
            held.prepared != state_->prepared || held.diagnostic != state_->config.diagnostic) return stop_observation.Result(__LINE__, false);
      }
      continuation_owner = state_->continuation;
      prepared = state_->prepared;
      diagnostic = state_->config.diagnostic;
      startup = state_->startup;
      identity = state_->task_execution;
      media = state_->media;
      content_resolve = state_->content_resolve;
      xmsg_provenance = state_->xmsg_provenance;
      title_runtime = state_->title_heap_runtime;
      stop = state_->backend_stop;
    }
    if (!kernel || XThread::IsInThread()) {
      return stop_observation.Result(__LINE__, false);
    }
    for (const auto& thread : kernel->object_table()->GetObjectsByType<XThread>(
             XObject::Type::Thread)) {
      if (thread->is_running()) {
        return stop_observation.Result(__LINE__, false);
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
        return stop_observation.Result(__LINE__, false);
      }
    }
    if (diagnostic) {
      if (!stop.backend_epoch()) {
        stop = diagnostic->backend_stop_proof();
      }
      if (!stop.backend_epoch() || !stop.stopped() ||
          !diagnostic->RetireAfterKernelStop()) {
        return stop_observation.Result(__LINE__, false);
      }
    }
    if (identity && !identity->RetireAfterActualKernelStop()) return stop_observation.Result(__LINE__, false);
    if (media && !media->RetireAfterActualKernelStop()) return stop_observation.Result(__LINE__, false);
    if (content_resolve && !content_resolve->RetireAfterActualKernelStop()) return stop_observation.Result(__LINE__, false);
    if (xmsg_provenance && !xmsg_provenance->RetireAfterActualKernelStop()) return stop_observation.Result(__LINE__, false);
    const auto avatar = CaptureAvatarService(kernel);
    if (avatar && !avatar->RetireAfterActualKernelStop()) return stop_observation.Result(__LINE__, false);
    // Identity first detaches/revokes heap-held read/commit cells, then this
    // transport releases exact XThread/PCR and invocation backing resources.
    if (continuation_owner && !continuation_owner->RetireAfterActualKernelStop()) return stop_observation.Result(__LINE__, false);
    std::shared_ptr<NativeSystemCallbackDispatcher> dispatcher_owner;
    {std::lock_guard lock(state_->mutex);dispatcher_owner=state_->dispatcher;}
    std::shared_ptr<NativeContentStorageDriver> storage_owner;
    {std::lock_guard lock(state_->mutex);storage_owner=state_->storage;}
    if(storage_owner && !storage_owner->RetireAfterActualKernelStop(kernel))return stop_observation.Result(__LINE__, false);
    if(dispatcher_owner && !dispatcher_owner->RetireAfterActualKernelStop())return stop_observation.Result(__LINE__, false);
    if (startup &&
        startup->RetireAndDrain(false) != GuestRuntime::DrainStatus::kDrained) {
      return stop_observation.Result(__LINE__, false);
    }
    if (title_runtime && title_runtime->RetireAndDrain(false) !=
                             GuestRuntime::DrainStatus::kDrained) {
      return stop_observation.Result(__LINE__, false);
    }
    // Synchronous public_calls==0 is NOT native XMsg request drainage. Keep
    // heap0 until this actual all-CPU/GPU-stopped whole-Kernel boundary; never
    // call native6160 from stop/destruction or free it beneath native workers.
    // Original DB788's serial queue base reference and eight profile buffers
    // are resident in this same root/heap. Do not Close/free them on service
    // quiescence: native queued Tasks may still own references. This actual
    // whole-Kernel stopped boundary retains heap lifetime until teardown and
    // makes no native queue drain/destructor-completion claim.
    // Same-Kernel Title replacement is deliberately not supported.
    // Drop holders before Kernel/Memory disappear, outside the metadata lock.
    // No native Close or guest callback is executed here.
    std::shared_ptr<NativeAvatarService> retired_avatar;
    std::shared_ptr<NativeProfileIdentityOwner> retired_identity;
    std::shared_ptr<NativeMediaService> retired_media;
    std::shared_ptr<NativeContentResolve> retired_content_resolve;
    std::shared_ptr<NativeXmsgProvenance> retired_xmsg_provenance;
    std::shared_ptr<const xam::ProfileRosterNativeBinding> retired_identity_binding;
    Config retired_config;
    std::shared_ptr<State::KernelAdmission> retired_admission;
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
    std::optional<State::RetainedStop> retained_stop;
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
      retired_admission=std::move(state_->admission);
      if(retired_admission)
        retired_admission->image_observer.current_.store({},std::memory_order_release);
      bootstrap = std::move(state_->bootstrap);
      root = std::move(state_->root);
      prepared_title_module = std::move(state_->prepared_title_module);
      prepared_title_lease = std::move(state_->prepared_title_lease);
      title_runtime = std::move(state_->title_heap_runtime);
      calls = std::move(state_->calls);
      retired_avatar = std::move(state_->avatar);
      retired_identity = std::move(state_->task_execution);
      state_->profile_identity.reset();
      retired_media = std::move(state_->media);
      retired_content_resolve = std::move(state_->content_resolve);
      retired_xmsg_provenance = std::move(state_->xmsg_provenance);
      retired_identity_binding = std::move(state_->profile_identity_binding);
      state_->profile_identity_requested = false;
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
      retained_stop = std::move(state_->retained_stop);
      state_->retained_stop.reset();
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
    // The old Kernel's closed admission must not survive through an escaped
    // Service and block a new Kernel allocated at the same host address. This
    // withdraws ONLY our exact old entry; an already replaced owner is kept.
    {
      std::lock_guard lock(State::registry_mutex);
      const auto found=State::registry.find(kernel);
      if(found!=State::registry.end() && found->second.lock()==retired_admission)
        State::registry.erase(found);
    }
    return stop_observation.Result(__LINE__, true);
  } catch (...) {
    return stop_observation.Result(__LINE__, false);
  }
}

void NativeSystemUiService::ObserveSigninEntry(void* raw,uint64_t address) noexcept {
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);auto* actual_cpu=c?c->thread_state:nullptr;
  if(!actual_cpu || !HooksSigninFunction(uint32_t(address)))return;
  auto* s=static_cast<State::PublicInvocation::SigninObservation*>(
      actual_cpu->native_signin_creation_cell_.load(std::memory_order_acquire));
  if(!s)return;
  auto* thread=XThread::GetCurrentFiberThread();
  if(!thread && XThread::IsInThread())thread=XThread::GetCurrentThread();
  if(!s->armed || s->cpu!=actual_cpu || actual_cpu->context()!=c ||
     cpu::ThreadState::Get()!=actual_cpu || s->context!=c || s->thread!=thread ||
     s->kernel!=c->kernel_state || !thread){s->lost=true;return;}
  // Actual same ThreadState executes these scalar observations serially; no
  // lookup, mutex, allocation, IO, native read or result write, even under GCRC.
  const auto canonical=[](uint64_t v,uint32_t e){return v==e || v==uint64_t(int64_t(int32_t(e)));};
  if(c->r[13]!=s->pcr || thread->native_frame_thread_identity().process_type!=s->process){s->lost=true;return;}
  if(address==0x816C1868){
    if(s->entered || !canonical(c->r[1],s->helper_sp) || !canonical(c->lr,0x816C1A08) ||
       !canonical(c->r[3],s->packet)){s->lost=true;return;}
    s->entered=true;
  }else if(canonical(c->lr,0x816C1950)){
    if(!s->entered || s->returned || s->sender_entered || !canonical(c->r[1],s->helper_sp-0x90) ||
       !canonical(c->r[3],0xFE) || !canonical(c->r[4],0x21001) ||
       !canonical(c->r[5],s->packet) || !canonical(c->r[6],0x14)){s->lost=true;return;}
    s->sender_entered=true;
  }
}
void NativeSystemUiService::ObserveSigninReturn(void* raw,uint64_t address) noexcept {
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);auto* actual_cpu=c?c->thread_state:nullptr;
  if(!actual_cpu || !HooksSigninFunction(uint32_t(address)))return;
  auto* s=static_cast<State::PublicInvocation::SigninObservation*>(
      actual_cpu->native_signin_creation_cell_.load(std::memory_order_acquire));
  if(!s)return;
  auto* thread=XThread::GetCurrentFiberThread();
  if(!thread && XThread::IsInThread())thread=XThread::GetCurrentThread();
  if(!s->armed || s->cpu!=actual_cpu || actual_cpu->context()!=c ||
     cpu::ThreadState::Get()!=actual_cpu || s->context!=c || s->thread!=thread ||
     s->kernel!=c->kernel_state || !thread){s->lost=true;return;}
  const auto canonical=[](uint64_t v,uint32_t e){return v==e || v==uint64_t(int64_t(int32_t(e)));};
  if(c->r[13]!=s->pcr || thread->native_frame_thread_identity().process_type!=s->process){s->lost=true;return;}
  if(address==0x816C1868){
    if(!s->entered || s->returned || !canonical(c->r[1],s->helper_sp) ||
       !canonical(c->lr,0x816C1A08)){s->lost=true;return;}
    s->returned=true;s->helper_value=uint32_t(c->r[3]);
  }else if(canonical(c->lr,0x816C1950)){
    if(!s->sender_entered || s->sender_returned || !canonical(c->r[1],s->helper_sp-0x90)){s->lost=true;return;}
    s->sender_returned=true;s->sender_value=uint32_t(c->r[3]);
  }
}

uint64_t NativeSystemUiService::GuardEditorLoaderEntry(void* raw,uint64_t address) noexcept {
  if (!raw) return 1;
  auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
  auto* cell=c.thread_state?static_cast<State::PublicInvocation::EditorHandoff*>(
      c.thread_state->native_editor_handoff_cell_.load(std::memory_order_acquire)):nullptr;
  if (!cell) return 1; // Every unrelated loader caller runs original bytes.
  const bool launch=address==0x816A0D70;
  const auto deny=[&]() noexcept {
    cell->lost=true;c.r[3]=NativeEditorLoaderRefusal(launch);return uint64_t(0);
  };
  if ((address!=0x8169F4A8 && !launch) || cell->lost ||
      cell->context!=&c || cell->cpu!=c.thread_state || cell->kernel!=c.kernel_state ||
      c.r[13]!=cell->pcr || !cell->thread || !XThread::IsInThread(cell->thread) ||
      cell->thread->thread_state()!=cell->cpu ||
      cell->thread->native_frame_thread_identity().process_type!=cell->process ||
      !MatchesNativeEditorLoaderEdge(launch,cell->stack.producer_sp,c.r[1],c.lr) ||
      !cell->producer || !cell->command || !cell->payload) return deny();
  if (!launch) {
    if (cell->copied || cell->requested || c.r[3]!=cell->stack.data || c.r[4]!=0x10C ||
        !cell->native_data) return deny();
    // Same actual retained XThread stack, bound before PPC. This is the actual
    // producer's byte copy; no host file/namespace call, allocation or ref drop.
    std::memcpy(cell->payload->data.data(),cell->native_data,cell->payload->data.size());
    const auto* bytes=cell->payload->data.data();
    if (xe::load_and_swap<uint32_t>(bytes)!=0x45504958u ||
        xe::load_and_swap<uint32_t>(bytes+4)!=cell->user ||
        xe::load_and_swap<uint32_t>(bytes+8)!=cell->flags) return deny();
    cell->copied=true;c.r[3]=0;return 0;
  }
  if (!cell->copied || cell->requested || c.r[3]!=cell->stack.path ||
      c.r[4] || c.r[5] || c.r[6] || !cell->native_path) return deny();
  auto& payload=*cell->payload;
  std::memcpy(payload.path.data(),cell->native_path,payload.path.size());
  const auto end=std::find(payload.path.begin(),payload.path.end(),'\0');
  cell->requested=true;
  if (end==payload.path.end() || end==payload.path.begin()) {c.r[3]=0;return 0;}
  payload.path_length=uint32_t(end-payload.path.begin());
  std::string_view path(payload.path.data(),payload.path_length);
  const auto split=path.find_last_of("\\/");
  const auto name=split==std::string_view::npos?path:path.substr(split+1);
  constexpr std::string_view expected="AvatarEditor.xex";
  if (name.size()!=expected.size()) {c.r[3]=0;return 0;}
  for (size_t n=0;n<name.size();++n) {
    const auto lower=[](char value) {return value>='A'&&value<='Z'?char(value-'A'+'a'):value;};
    if (lower(name[n])!=lower(expected[n])) {c.r[3]=0;return 0;}
  }
  // This preallocated host metadata publication performs no wake, allocation,
  // filesystem access, native call, callback or lifetime release under GCRC.
  payload.accepted=cell->emulator->PublishPreparedLifecycle(*cell->command,*cell->producer);
  c.r[3]=payload.accepted?1u:0u;return 0;
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
  // Refuse the new handoff before registry/trace/payload preparation can
  // allocate or call out under an inherited native global critical region.
  if (call==NativeSystemUiPublicCall::kLaunchAvatarEditor &&
      global_critical_region::is_held_by_current_thread()) return denied;
  const auto avatar=NativeAvatarService::TryInvokeOnOriginalCaller(kernel,call,arguments);
  if(avatar.handled())return avatar;
  static std::atomic<uint64_t> selector_requests{0};
  const auto selector_id = cvars::native_system_ui_trace &&
      call == NativeSystemUiPublicCall::kShowDeviceSelector
      ? selector_requests.fetch_add(1, std::memory_order_relaxed) : UINT64_MAX;
  auto selector_trace = [selector_id, arguments](const char* stage, const Result& value,
      bool executed = false, bool healthy = true) noexcept {
    if (selector_id > 16) return;
    try {
      if (selector_id == 16) {
        if (std::string_view(stage) == "entry")
          XELOGI("StorageDiagnostic native_selector budget_exhausted limit=16 later_requests=unknown");
        return;
      }
      const bool known = std::string_view(stage) != "entry";
      XELOGI("StorageDiagnostic native_selector id={} stage={} arguments={} overlap_known={} overlap={:08X} "
             "disposition={} disposition_known={} result={:08X} result_known={} executed={} healthy={} health_known={}",
             selector_id, stage, arguments.size(), arguments.size() == 6,
             arguments.size() == 6 ? uint32_t(arguments[5]) : 0,
             known ? uint32_t(value.disposition) : UINT32_MAX, known,
             known ? value.value : UINT32_MAX, known,
             executed, executed && healthy, executed);
    } catch (...) {}
  };
  selector_trace("entry", denied);
  static xe::NativeSystemUiTraceLimiter entry_trace;
  entry_trace.Emit(
      "input", "public_call_entry",
      {{"call", uint16_t(call)}, {"argument_count", arguments.size()}});
  try {
    if (!kernel) {
      selector_trace("inactive_null_kernel", Result{});
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
      selector_trace("inactive_no_service", Result{});
      static xe::NativeSystemUiTraceLimiter trace;
      trace.Emit("input", "public_call_inactive",
                 {{"call", uint16_t(call)}, {"reason", 1}});
      return {};
    }
    auto* state = service->state_.get();
    {
      std::lock_guard lock(state->mutex);
      if (!state->public_ever_admitted) {
        selector_trace("inactive_not_admitted", Result{});
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
        selector_trace("inactive_admission_changed", Result{});
        return {};  // Existing HLE remains authoritative while native is
                    // inactive.
      }
      const bool editor = call == NativeSystemUiPublicCall::kLaunchAvatarEditor;
      if (profile && !NativeSystemPublicFamilyRouteEnabled(profile->family) && !editor) {
        selector_trace("deny_family", denied);
        return denied;  // Catalog is not a provider/engine admission witness.
      }
      if (selector_id < 16) {
        try {
          XELOGI("StorageDiagnostic native_selector_readiness id={} profile={} thread={} "
                 "boot={} title_heap={} prepared={} allocation={} stopping={} retired={} uncertain={} active_calls={}",
                 selector_id, bool(profile), thread ? thread->thread_id() : 0,
                 state->status.boot_admitted, state->status.title_heap_ready,
                 bool(state->prepared), bool(state->allocation), state->IsStopping(),
                 state->retired, state->public_uncertain, state->public_calls);
        } catch (...) {}
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
        selector_trace("deny_public_conditions", denied);
        return denied;
      }
      const auto process = thread->native_frame_thread_identity().process_type;
      if (editor && global_critical_region::is_held_by_current_thread()) {
        // Never allocate/prepare the copied handoff under inherited locks.
        return denied;
      }
      if (process != X_PROCTYPE_TITLE && process != X_PROCTYPE_SYSTEM) {
        selector_trace("deny_process", denied);
        return denied;
      }
      invocation->calls = state->calls;
      invocation->prepared = state->prepared;
      invocation->domain_operation = state->domain->Acquire();
      if (!invocation->domain_operation) {
        selector_trace("deny_domain", denied);
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
    bool editor_title_transfer = false;
    uint32_t signin_raw_flags=0,signin_decision=0,signin_helper_value=0,signin_sender_value=0;
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
          ValidPublicFunction(*invocation->calls, *profile) &&
          (call != NativeSystemUiPublicCall::kLaunchAvatarEditor ||
           ValidEditorLoaderSource(*invocation->calls))) {
        const bool creating =
            call == NativeSystemUiPublicCall::kShowGuide ||
            call == NativeSystemUiPublicCall::kShowKeyboard ||
            call == NativeSystemUiPublicCall::kShowMessageBox ||
            call == NativeSystemUiPublicCall::kShowDeviceSelector ||
            IsSigninPublic(call);
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
        auto ingress = creating ? kernel->AcquireNativeUiCreation() : nullptr;
        if (creating && ingress) {
          ui = invocation->prepared->AdmitUiCreating(title_caller);
        }
        if (creating && ui.decision == NativeUiBackendRoute::Decision::kHost) {
          ui.host->RetainHostIngress(std::move(ingress));
          result = {Disposition::kHostFallback, 0, std::move(ui.host)};
        } else if (creating &&
                   ui.decision != NativeUiBackendRoute::Decision::kNative) {
          result = denied;
        } else if (call == NativeSystemUiPublicCall::kIsUiActive &&
                   invocation->host_input) {
          // Existing wrapper composes actual XamState/host InputBlocker state.
          result = {Disposition::kReturned, 0};
        } else {
          if (call == NativeSystemUiPublicCall::kLaunchAvatarEditor) {
            auto cell = std::make_unique<State::PublicInvocation::EditorHandoff>();
            const auto* c = thread->thread_state()->context();
            const auto layout = c && c->r[1] <= UINT32_MAX &&
                c->r[1] >= cpu::kGuestCallExecuteStackPadding
                ? CaptureNativeEditorLoaderStack(
                    uint32_t(c->r[1])-cpu::kGuestCallExecuteStackPadding,
                    thread->stack_limit(),thread->stack_base()) : std::nullopt;
            cell->root_code=kernel->AcquireUserModuleExecutionLease(invocation->calls->module());
            if (!layout || !cell->root_code || c->r[13]!=thread->pcr_ptr() ||
                arguments[0]>=XUserMaxUserCount ||
                kernel->memory()->QueryRangeAccess(layout->producer_sp,
                    uint32_t(c->r[1])-cpu::kGuestCallExecuteStackPadding-layout->producer_sp)
                    !=memory::PageAccess::kReadWrite) goto public_execution_done;
            cell->context=thread->thread_state()->context();cell->cpu=thread->thread_state();
            cell->kernel=kernel;cell->emulator=kernel->emulator();cell->pcr=thread->pcr_ptr();
            cell->thread=thread;cell->process=thread->native_frame_thread_identity().process_type;
            cell->user=uint32_t(arguments[0]);cell->flags=uint32_t(arguments[1]);cell->stack=*layout;
            cell->native_data=kernel->memory()->TranslateVirtual<const uint8_t*>(layout->data);
            cell->native_path=kernel->memory()->TranslateVirtual<const char*>(layout->path);
            cell->payload=std::make_shared<EditorHandoffPayload>();
            auto producer=cell->emulator->AdmitLifecyclePreparation();
            const auto generation=cell->emulator->title_execution_state().snapshot().generation;
            if (!producer || !generation) goto public_execution_done;
            cell->producer.emplace(std::move(producer));
            auto payload=cell->payload;auto* emulator=cell->emulator;
            auto command=emulator->PrepareLifecycleWork(
                EmulatorLifecycle::Kind::kReplace,generation,*cell->producer,
                [payload,emulator,expected=kernel,generation] {
                  if (!payload->sealed.load(std::memory_order_acquire) || !payload->accepted) {
                    XELOGE("Editor command lacks its real public-return seal");
                    return;
                  }
                  if (!emulator->RelaunchSystemApplication(expected,generation,
                      std::string(payload->path.data(),payload->path_length),0,
                      std::vector<uint8_t>(payload->data.begin(),payload->data.end())))
                    XELOGE("Copied Editor request could not resolve/admit its owned target; title replacement was not started");
                });
            if (!command) goto public_execution_done;
            cell->command.emplace(std::move(*command));
            invocation->editor=std::move(cell);
            void* empty=nullptr;
            if (!invocation->editor->cpu->native_editor_handoff_cell_.compare_exchange_strong(
                    empty,invocation->editor.get(),std::memory_order_acq_rel)) {
              invocation->editor.reset();goto public_execution_done;
            }
          }
          if(IsSigninPublic(call)){
            invocation->signin_root_code=kernel->AcquireUserModuleExecutionLease(invocation->calls->module());
            const auto* c=thread->thread_state()->context();
            if(!invocation->signin_root_code || !ValidSigninObservedCode(*invocation->calls) ||
               !c || c->r[1]>UINT32_MAX || c->r[1]<(cpu::kGuestCallExecuteStackPadding+0x70) ||
               (c->r[1]&15) || c->r[13]!=thread->pcr_ptr()){
              // No PPC attempt or host replay. Ordinary denied result remains.
              ingress->Finish(false);
              goto public_execution_done;
            }
            auto& s=invocation->signin;s.context=thread->thread_state()->context();
            s.cpu=thread->thread_state();s.thread=thread;s.kernel=kernel;
            s.pcr=thread->pcr_ptr();s.process=thread->native_frame_thread_identity().process_type;
            s.helper_sp=uint32_t(c->r[1])-cpu::kGuestCallExecuteStackPadding-0x70;
            s.packet=s.helper_sp+0x50;s.armed=true;
            invocation->creation_ingress=ingress;
            void* empty=nullptr;
            if(!s.cpu->native_signin_creation_cell_.compare_exchange_strong(empty,&s,
                std::memory_order_acq_rel)){
              // Never displace an existing actor or clear its cell.
              s.armed=false;invocation->creation_ingress.reset();ingress->Finish(false);
              goto public_execution_done;
            }
          }
          executed = true;  // A false/throw after this point is uncertain.
          if (ingress) { ingress->MarkExecutionAttempt(); }
          const auto native = invocation->editor
              ? invocation->calls->ExecuteEditorOnOriginalCaller(*thread, arguments)
              : invocation->calls->ExecuteDeferredPublicRegistersOnOriginalCaller(
                    *thread, call, arguments);
          healthy = native.succeeded;
          if (invocation->editor && native.succeeded) {
            auto& cell=*invocation->editor;void* exact=&cell;
            const bool withdrawn=cell.cpu->native_editor_handoff_cell_.compare_exchange_strong(
                exact,nullptr,std::memory_order_acq_rel);
            healthy=withdrawn && !cell.lost &&
                thread->thread_state()==cell.cpu &&
                thread->native_frame_thread_identity().process_type==cell.process;
            if (healthy && cell.requested && cell.payload->accepted) {
              if (uint32_t(native.return_value)!=0) healthy=false;
              else {
                cell.payload->sealed.store(true,std::memory_order_release);
                editor_title_transfer=cell.process==X_PROCTYPE_TITLE;
              }
            }
          }
          // Failed/partial execution keeps the exact invocation/cell/Producer;
          // neither a host queue nor native0 supplies an invented own-return.
          if (ingress) {
            if(IsSigninPublic(call)){
              auto& s=invocation->signin;void* exact=&s;
              const bool withdrawn=s.cpu->native_signin_creation_cell_.compare_exchange_strong(
                  exact,nullptr,std::memory_order_acq_rel);
              const auto decision=DecideNativeSigninCreation(s,native.succeeded,withdrawn,
                  uint32_t(native.return_value));
              signin_raw_flags=uint32_t(s.armed)|(uint32_t(s.entered)<<1)|(uint32_t(s.returned)<<2)|
                  (uint32_t(s.sender_entered)<<3)|(uint32_t(s.sender_returned)<<4)|(uint32_t(s.lost)<<5)|
                  (uint32_t(withdrawn)<<6)|(uint32_t(native.succeeded)<<7);
              signin_decision=uint32_t(decision);
              signin_helper_value=s.returned?s.helper_value:0;signin_sender_value=s.sender_returned?s.sender_value:0;
              if(decision!=NativeSigninCreationDecision::kUncertain){
                ingress->Finish(decision==NativeSigninCreationDecision::kQueued);
                invocation->creation_ingress.reset();s.armed=false;
              }else healthy=false; // Exact returned bits preserved; ownership HOLD.
            }else ingress->Finish(!healthy || !uint32_t(native.return_value));
          }
          result = native.succeeded ? Result{Disposition::kReturned,
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
    public_execution_done:;
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
           {"signin_raw_flags",signin_raw_flags},{"signin_decision",signin_decision},
           {"signin_helper_return",signin_helper_value},{"signin_sender_return",signin_sender_value},
           {"editor_data_copied",invocation->editor && invocation->editor->copied},
           {"editor_launch_seen",invocation->editor && invocation->editor->requested},
           {"editor_request_accepted",invocation->editor && invocation->editor->payload->accepted},
           {"editor_uncertain",invocation->editor && invocation->editor->lost},
           {"host_lease_serial",
            result.host_fallback     ? result.host_fallback->serial()
            : invocation->host_input ? invocation->host_input->serial()
                                     : 0}});
    }
    // Drop all healthy reservation/code/caller closures before reporting drain.
    // These are host-only destructors, never guest Task/overlap/FF destruction.
    std::optional<EmulatorLifecycle::Producer> editor_producer;
    if (healthy && invocation->editor && invocation->editor->producer)
      editor_producer.emplace(std::move(*invocation->editor->producer));
    invocation.reset();
    {
      std::lock_guard lock(state->mutex);
      --state->public_calls;
      state->public_drained.notify_all();
    }
    // Actual public own-return and resource/counter retirement precede this
    // Producer wake. The prepared command cannot run native stop before it.
    editor_producer.reset();
    if (healthy && editor_title_transfer) {
      // Only the outer fixed7DB shim consumes this. Its Service/shared owners
      // unwind normally before the common never-return TITLE park begins.
      result.disposition=Disposition::kTransferred;
    }
    // This Service performs no guest wait/PPC after decrement. Shutdown joins
    // actual CPU workers before reclaiming either the SYSTEM shim's return
    // tail or the TITLE shim's subsequent resource-free handoff park.
    selector_trace("return", result, executed, healthy);
    return result;
  } catch (...) {
    selector_trace("outer_exception", denied);
    return denied;  // Never unwind a guest shim or trigger active HLE fallback.
  }
}

void NativeSystemUiService::SealHostUiFallbackForKernelOrFailStop(
    KernelState* kernel) noexcept {
  try {
    if (!kernel) {
      return;
    }
    kernel->BeginStopLegacyHostUi();
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
      if (!kernel->SealLegacyHostUi()) {
        ReportNativeStopFailure(0, 5, __LINE__);
        FailStop();
      }
      return;
    }
    // Close ingress first; existing visible dialogs are still closed by the
    // caller's subsequent on_title_closing, before full native/provider stop.
    service->BeginStop();
    if (!kernel->SealLegacyHostUi()) {
      ReportNativeStopFailure(
          service->state_->stop_first_failure.load(std::memory_order_relaxed),
          5, __LINE__);
      FailStop();
    }
    std::shared_ptr<NativeSystemUiPreparedTransport> prepared;
    {
      std::lock_guard lock(service->state_->mutex);
      prepared = service->state_->prepared;
    }
    if (prepared && !prepared->SealHostUiFallback()) {
      ReportNativeStopFailure(
          service->state_->stop_first_failure.load(std::memory_order_relaxed),
          5, __LINE__);
      FailStop();
    }
  } catch (...) {
    ReportNativeStopFailure(0, 5, __LINE__);
    FailStop();
  }
}

void NativeSystemUiService::StopForKernelOrFailStop(
    KernelState* kernel) noexcept {
  try {
    if (!kernel) {
      return;
    }
    kernel->SealStoragePreparation();
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
      if(admission){
        admission->closed.store(true, std::memory_order_release);
        service = admission->service.lock();
      }
    }
    // Capture the independent scalar sink BEFORE stop may fail or retain its
    // native owners across CPU stop. The reporter touches only this sink.
    const auto identity_stop_diagnostic=service?service->state_->identity_stop_diagnostic:nullptr;
    if (service && !service->StopBeforeCpuAndProviders()) {
      ReportIdentityStopDiagnosticOnHost(identity_stop_diagnostic);
      // Original stop operation and its locks have unwound. Publish the
      // already-captured scalar Avatar refusal before the unchanged FailStop;
      // its reporter itself rejects guest/fiber/inherited-GCRC contexts.
      if(cvars::native_system_ui_trace && !XThread::IsInThread() &&
         !XThread::GetCurrentFiberThread() &&
         !global_critical_region::is_held_by_current_thread()){
        const auto avatar=CaptureAvatarService(kernel);
        if(avatar)avatar->ReportDiagnosticsOnHost("StopFailure");
      }
      ReportNativeStopFailure(
          service->state_->stop_first_failure.load(std::memory_order_relaxed),
          6, __LINE__);
      FailStop();
    }
    // Also covers an inactive/never-created native service: fallback IO still
    // owns the shared Console authority until its admitted operation returns.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (preferences && !preferences->SafelyDrained()) {
      if (preferences->snapshot().uncertain ||
          std::chrono::steady_clock::now() >= deadline) {
        ReportNativeStopFailure(
            service ? service->state_->stop_first_failure.load(std::memory_order_relaxed) : 0,
            6, __LINE__);
        FailStop();
      }
      xe::threading::Sleep(std::chrono::milliseconds(1));
    }
  } catch (...) {
    ReportNativeStopFailure(0, 6, __LINE__);
    FailStop();
  }
}
}  // namespace xe::kernel
