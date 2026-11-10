/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/guest_runtime_call_executor.h"
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>
#include <vector>
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_managed_format_io.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
void ActiveTitleId(cpu::ppc::PPCContext* context, KernelState* state) {
  uint32_t title = 0;
  if (state && state->emulator() &&
      state->emulator()->OwnsTitleExecution(state) &&
      !state->emulator()->is_relaunching() &&
      !state->emulator()->is_restoring()) {
    title = state->emulator()->title_execution_state().active_title_id();
  }
  context->r[3] = title;
}
// This fixed planned bridge is intentionally unavailable without the owned
// request/selection service. There is never a fallback to the raw formatter.
void ManagedFormatOwnedRequest(cpu::ppc::PPCContext* context,
                               KernelState* kernel) {
  uint32_t result = 0x80004001;  // E_NOTIMPL for unowned/native-ID namespaces.
  if (context && kernel && context->kernel_state == kernel) {
    auto module = kernel->GetUserModuleForAddress(0x81684578);
    if (module) {
      result =
          NativeManagedFormatIo::TryHandleFormat(*context, *module, 0x81684578)
              .value_or(result);
    }
  }
  if (context) {
    context->r[3] = uint64_t(int64_t(int32_t(result)));
  }
}
GuestRuntime::ExecutionResult Failure(std::string text) {
  return {false, 0, std::move(text)};
}
}  // namespace
struct GuestRuntimeCallExecutor::State {
  KernelState* kernel;
  object_ref<UserModule> module;
  uint64_t load_generation;
  std::shared_ptr<KernelState::UserModuleExecutionLease> code;
  std::shared_ptr<GuestRuntime> heap;
  ValidatedGuestRuntimePlan validated;
  std::set<std::string> initialization;
  std::mutex mutex;
  bool bridges_installed = false, bridges_active = false,
       bridges_failed = false;
};
GuestRuntimeCallExecutor::GuestRuntimeCallExecutor(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
GuestRuntimeCallExecutor::~GuestRuntimeCallExecutor() = default;
const ValidatedGuestRuntimePlan& GuestRuntimeCallExecutor::validated() const {
  return state_->validated;
}
KernelState* GuestRuntimeCallExecutor::kernel() const { return state_->kernel; }
object_ref<UserModule> GuestRuntimeCallExecutor::module() const {
  return state_->module;
}
std::shared_ptr<GuestRuntime> GuestRuntimeCallExecutor::heap_runtime() const {
  return state_->heap;
}
uint64_t GuestRuntimeCallExecutor::load_generation() const {
  return state_->load_generation;
}

std::shared_ptr<GuestRuntimeCallExecutor> GuestRuntimeCallExecutor::Load(
    KernelState* kernel, const object_ref<UserModule>& module,
    const std::filesystem::path& path, std::string* error) {
  const auto fail = [&](std::string text) {
    if (error) {
      *error = std::move(text);
    }
    return std::shared_ptr<GuestRuntimeCallExecutor>{};
  };
  if (!kernel || !module || module->kernel_state() != kernel || path.empty() ||
      !module->is_system_app_module() ||
      module->module_format() != UserModule::kModuleFormatXex ||
      !module->hash() || module.get() == kernel->GetExecutableModule().get()) {
    return fail("Exact separately loaded native XEX required");
  }
  auto code = kernel->AcquireUserModuleExecutionLease(module);
  auto runtime = kernel->CreateGuestRuntime(module);
  if (!code || !runtime) {
    return fail("Native root code/runtime lease unavailable");
  }
  std::error_code file_error;
  const auto bytes = std::filesystem::file_size(path, file_error);
  if (file_error || !bytes || bytes > 1024 * 1024) {
    return fail("Missing/oversized explicit plan");
  }
  std::ifstream file(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  if (!file || text.size() != bytes) {
    return fail("Plan file read failed");
  }
  auto* image = module->xex_module();
  auto* memory = kernel->memory();
  GuestRuntimeImageView view;
  view.base = image->base_address();
  view.hmodule = module->hmodule_ptr();
  view.module_hash = *module->hash();
  view.code_sha1 = image->image_code_sha1();
  view.bytes = {memory->TranslateVirtual<const uint8_t*>(view.base),
                image->image_size()};
  const auto range = [image, memory](uint32_t address, uint32_t length,
                                     bool code) {
    if (!length || uint64_t(address) + length > (uint64_t(1) << 32) ||
        memory->QueryRangeAccess(address, length) ==
            memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint64_t current = address; current < uint64_t(address) + length;
         current = (current & ~uint64_t(0xFFF)) + 0x1000) {
      xex2_section_type type;
      if (!image->GetPageSectionType(uint32_t(current), &type) ||
          (code ? type != XEX_SECTION_CODE
                : (type != XEX_SECTION_DATA &&
                   type != XEX_SECTION_READONLY_DATA))) {
        return false;
      }
    }
    return true;
  };
  view.is_code = [range](uint32_t a, uint32_t n) { return range(a, n, true); };
  view.is_data = [range](uint32_t a, uint32_t n) { return range(a, n, false); };
  view.export_address = [image](uint16_t ordinal) {
    return image->GetProcAddress(ordinal);
  };
  std::string reason;
  auto validated = ParseGuestRuntimePlan(text, view, &reason);
  if (!validated) {
    return fail(std::move(reason));
  }
  auto state = std::make_unique<State>();
  state->kernel = kernel;
  state->module = module;
  state->load_generation = module->load_generation();
  state->code = std::move(code);
  state->heap = std::move(runtime);
  state->validated = std::move(*validated);
  for (const auto& phase : state->validated.plan.phases) {
    for (const auto& function : phase.functions) {
      state->initialization.insert(function.identity);
    }
  }
  if (error) {
    error->clear();
  }
  return std::shared_ptr<GuestRuntimeCallExecutor>(
      new GuestRuntimeCallExecutor(std::move(state)));
}

bool GuestRuntimeCallExecutor::InstallTrustedBridges(std::string* error) {
  const auto keep_alive = shared_from_this();
  const auto heap = state_->heap->GetSnapshot();
  {
    std::lock_guard lock(state_->mutex);
    if (state_->bridges_installed) {
      return true;
    }
    if (state_->bridges_failed || state_->bridges_active) {
      if (error) {
        *error = "Bridge installation failed/busy";
      }
      return false;
    }
    // Exact Ready plan includes its whole source hash in every function's ABI
    // metadata. Its original executor already installed this same bridge set.
    if (heap.state == GuestRuntime::State::kReady && heap.plan &&
        *heap.plan == state_->validated.plan) {
      state_->bridges_installed = true;
      return true;
    }
    if (heap.state != GuestRuntime::State::kResourcesLoaded &&
        heap.state != GuestRuntime::State::kInitializing) {
      if (error) {
        *error = "Native code bridge binding is no longer quiescent";
      }
      return false;
    }
    state_->bridges_active = true;
  }
  bool success = false;
  try {
    auto* image = state_->module->xex_module();
    success = true;
    for (const auto& bridge : state_->validated.bridges) {
      // Internal services remain fixed trusted functions, never configured
      // host pointers. The managed bridge has one exact original full extent.
      bool bound = false;
      if (!bridge.address) {
        bound = image->BindGuestExportToHost(bridge.ordinal, bridge.library,
                                             bridge.ordinal);
      } else if (bridge.service == "active_title_id") {
        bound = image->BindGuestFunctionToHost(
            bridge.address, bridge.code_size, bridge.code_sha256, ActiveTitleId,
            "active_title_id");
      } else if (bridge.service == "managed_format_owned_request" &&
                 bridge.address == 0x81684578 && bridge.code_size == 268 &&
                 bridge.code_sha256 ==
                     "72b85802c32d4c9ddfb4d535ddde60aee77fc4b13b9b0419dbd1c20db"
                     "afd22d8") {
        bound = image->BindGuestFunctionToHost(
            bridge.address, bridge.code_size, bridge.code_sha256,
            ManagedFormatOwnedRequest, "managed_format_owned_request");
      }
      if (!bound) {
        success = false;
        break;
      }
    }
  } catch (...) {
    std::lock_guard lock(state_->mutex);
    state_->bridges_active = false;
    state_->bridges_failed = true;
    throw;
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->bridges_active = false;
    state_->bridges_installed = success;
    state_->bridges_failed = !success;
  }
  if (!success && error) {
    *error = "Trusted implemented/quiescent bridge unavailable";
  }
  return success;
}

GuestRuntime::ExecutionResult GuestRuntimeCallExecutor::Execute(
    XThread& thread, const GuestRuntime::Function& function,
    std::span<const uint32_t> arguments, bool system_only) {
  if (arguments.size() != function.arguments.size() ||
      arguments.size() > cpu::kGuestCallMaxArguments) {
    return Failure("Validated DWORD prototype/arity mismatch");
  }
  const std::vector<uint64_t> registers(arguments.begin(), arguments.end());
  return ExecuteRegisters(thread, function, registers, 0, system_only);
}

GuestRuntime::ExecutionResult GuestRuntimeCallExecutor::ExecuteRegisters(
    XThread& thread, const GuestRuntime::Function& function,
    std::span<const uint64_t> arguments, uint8_t uint64_register_mask,
    bool system_only) {
  const auto keep_alive = shared_from_this();
  const auto id = thread.native_frame_thread_identity();
  if (!XThread::IsInThread(&thread) || id.kernel != state_->kernel ||
      (id.process_type != X_PROCTYPE_SYSTEM &&
       (system_only || id.process_type != X_PROCTYPE_TITLE)) ||
      state_->module->load_generation() != state_->load_generation ||
      state_->kernel->GetUserModuleForAddress(function.address).get() !=
          state_->module.get()) {
    return Failure("Actual original caller/code generation invalid");
  }
  if (arguments.size() != function.arguments.size() ||
      arguments.size() > cpu::kGuestCallMaxArguments) {
    return Failure("Validated register prototype/arity mismatch");
  }
  const auto found = state_->validated.catalog.find(function.identity);
  if (found == state_->validated.catalog.end() || found->second != function) {
    return Failure("Function is not the exact validated prototype");
  }
  const auto contract = state_->validated.contracts.find(function.identity);
  if (contract == state_->validated.contracts.end()) {
    return Failure("Native result contract absent");
  }
  std::string bridge_error;
  if (!InstallTrustedBridges(&bridge_error)) {
    return Failure(std::move(bridge_error));
  }
  auto* memory = state_->kernel->memory();
  const auto check = [&](const auto& list) {
    for (const auto& word : list) {
      if (memory->QueryRangeAccess(word.address, 4) ==
          memory::PageAccess::kNoAccess) {
        return false;
      }
      const auto actual =
          xe::load_and_swap<uint32_t>(memory->TranslateVirtual(word.address));
      if (word.nonzero ? !actual : actual != word.expected) {
        return false;
      }
    }
    return true;
  };
  if (!check(contract->second.before)) {
    return Failure("Native data precondition failed");
  }
  std::vector<cpu::GuestCallArgument> packed;
  packed.reserve(arguments.size());
  for (size_t i = 0; i < arguments.size(); ++i) {
    const bool wide = i < 8 && (uint64_register_mask & (1u << i));
    if (!wide && arguments[i] > UINT32_MAX) {
      return Failure("Public DWORD/pointer argument is not representable");
    }
    packed.push_back(
        wide ? cpu::GuestCallArgument::UInt64Register(arguments[i])
             : cpu::GuestCallArgument::UInt32(uint32_t(arguments[i])));
  }
  const auto called =
      cpu::ExecuteGuestCall(state_->kernel->processor(), thread.thread_state(),
                            function.address, packed);
  const auto value = uint32_t(called.value);
  using Policy = ValidatedGuestRuntimePlan::ResultPolicy;
  const auto policy = contract->second.result;
  const bool accepted = policy == Policy::kVoid || policy == Policy::kAny ||
                        (policy == Policy::kZero && value == 0) ||
                        (policy == Policy::kNonzero && value != 0) ||
                        (policy == Policy::kHResult && int32_t(value) >= 0);
  if (!called.executed || !accepted) {
    XELOGE(
        "Native call {} at {:08X}: executed={} return={:08X} "
        "result_accepted={}",
        function.identity, function.address, called.executed, value, accepted);
    return {false, value, "Native execution/return contract failed"};
  }
  if (!check(contract->second.after)) {
    return {false, value, "Native data postcondition failed"};
  }
  return {true, value, {}};
}
GuestRuntime::ExecutionResult
GuestRuntimeCallExecutor::ExecuteInitializationFunction(
    XThread& thread, const GuestRuntime::Function& function) {
  if (!state_->initialization.contains(function.identity)) {
    return Failure("Not an initialization function");
  }
  std::vector<uint32_t> arguments;
  for (const auto arg : function.arguments) {
    arguments.push_back(uint32_t(arg));
  }
  return Execute(thread, function, arguments, true);
}
GuestRuntime::ExecutionResult
GuestRuntimeCallExecutor::ExecuteDeferredSystemFunction(
    XThread& thread, std::string_view identity,
    std::span<const uint32_t> args) {
  const auto found = state_->validated.catalog.find(std::string(identity));
  if (found == state_->validated.catalog.end() ||
      state_->initialization.contains(found->first)) {
    return Failure("Not a validated deferred function");
  }
  const auto contract = state_->validated.contracts.find(found->first);
  if (contract == state_->validated.contracts.end() ||
      contract->second.caller ==
          ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly) {
    return Failure(
        "Deferred prototype requires the verified actual native Task "
        "transaction");
  }
  return Execute(thread, found->second, args, true);
}
GuestRuntime::ExecutionResult
GuestRuntimeCallExecutor::ExecuteDeferredNativeTaskFunction(
    XThread& thread, std::string_view identity,
    std::span<const uint32_t> args) {
  const auto found = state_->validated.catalog.find(std::string(identity));
  const auto contract = state_->validated.contracts.find(std::string(identity));
  if (found == state_->validated.catalog.end() ||
      contract == state_->validated.contracts.end() ||
      state_->initialization.contains(found->first) ||
      contract->second.caller !=
          ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly) {
    return Failure("Not a validated actual-native-Task frontend prototype");
  }
  return Execute(thread, found->second, args, true);
}

GuestRuntime::ExecutionResult
GuestRuntimeCallExecutor::ExecuteDeferredOnOriginalCaller(
    XThread& thread, std::string_view identity,
    std::span<const uint32_t> args) {
  for (const auto& profile : NativeSystemUiPublicProfiles()) {
    if (identity == profile.identity &&
        !NativeSystemPublicFamilyRouteEnabled(profile.family)) {
      return Failure("Native public service family lacks runtime admission");
    }
  }
  const auto found = state_->validated.catalog.find(std::string(identity));
  if (found == state_->validated.catalog.end() ||
      state_->initialization.contains(found->first)) {
    return Failure("Not a validated deferred function");
  }
  const auto contract = state_->validated.contracts.find(found->first);
  if (contract == state_->validated.contracts.end() ||
      contract->second.caller !=
          ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem) {
    return Failure(
        "This deferred prototype is SYSTEM-only, not a public original-caller "
        "API");
  }
  return Execute(thread, found->second, args, false);
}

GuestRuntime::ExecutionResult
GuestRuntimeCallExecutor::ExecuteDeferredPublicRegistersOnOriginalCaller(
    XThread& thread, NativeSystemUiPublicCall call,
    std::span<const uint64_t> args) {
  const auto* profile = NativeSystemUiPublicProfile(call);
  if (!profile || !NativeSystemPublicFamilyRouteEnabled(profile->family) ||
      !NativeSystemUiPublicArgumentsValid(call, args)) {
    return Failure("Invalid exact public register prototype");
  }
  const auto found =
      state_->validated.catalog.find(std::string(profile->identity));
  if (found == state_->validated.catalog.end() ||
      state_->initialization.contains(found->first) ||
      found->second.address != profile->address ||
      found->second.arguments.size() != profile->argument_count) {
    return Failure("Not the validated deferred public register function");
  }
  const auto contract = state_->validated.contracts.find(found->first);
  if (contract == state_->validated.contracts.end() ||
      contract->second.caller !=
          ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem) {
    return Failure("Public register prototype requires its original caller");
  }
  return ExecuteRegisters(thread, found->second, args,
                          profile->uint64_register_mask, false);
}

GuestRuntime::ExecutionResult
GuestRuntimeCallExecutor::ExecuteManagedFormatRegistersOnOriginalCaller(
    XThread& thread, NativeSystemUiPublicCall call,
    std::span<const uint64_t> args) {
  const auto* profile = NativeSystemUiPublicProfile(call);
  if (!profile ||
      profile->family != NativeSystemPublicFamily::kManagedFormatIo ||
      (call != NativeSystemUiPublicCall::kManagedFormatStart &&
       call != NativeSystemUiPublicCall::kManagedFormatCancel) ||
      !NativeSystemUiPublicArgumentsValid(call, args) ||
      !NativeManagedFormatIo::CurrentScopeMatches(this, thread, call, args)) {
    return Failure("No owned exact managed-format original-caller scope");
  }
  const auto found =
      state_->validated.catalog.find(std::string(profile->identity));
  const auto contract =
      state_->validated.contracts.find(std::string(profile->identity));
  if (found == state_->validated.catalog.end() ||
      state_->initialization.contains(found->first) ||
      found->second.address != profile->address ||
      found->second.arguments.size() != profile->argument_count ||
      contract == state_->validated.contracts.end() ||
      contract->second.caller !=
          ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem ||
      contract->second.result !=
          ValidatedGuestRuntimePlan::ResultPolicy::kAny) {
    return Failure(
        "Managed format lacks its exact validated deferred contract");
  }
  // Negative schedule HRESULT is still a genuine normal return. The original
  // caller owns its event/APC selection; no process/PCR/Task is substituted.
  return ExecuteRegisters(thread, found->second, args, 0, false);
}

GuestRuntime::InitStatus GuestRuntimeCallExecutor::InitializeHeap(
    XThread& thread,
    const std::shared_ptr<NativeSystemAllocationAuthority>& allocation) {
  const auto keep_alive = shared_from_this();
  if (allocation && (allocation->kernel() != state_->kernel ||
                     allocation->root_module().get() != state_->module.get() ||
                     allocation->owner().module_generation !=
                         state_->heap->GetSnapshot().identity.generation)) {
    return GuestRuntime::InitStatus::kFailed;
  }
  return state_->heap->Initialize(
      state_->validated.plan, [&](const GuestRuntime::Function& function) {
        if (allocation) {
          auto scope = allocation->EnterBootstrap(thread, function);
          if (!scope) {
            return Failure("Native bootstrap allocation scope denied");
          }
          auto result = ExecuteInitializationFunction(thread, function);
          if (!scope.Complete(result.succeeded)) {
            return Failure("Native bootstrap scope failed/closed");
          }
          return result;
        }
        return ExecuteInitializationFunction(thread, function);
      });
}
}  // namespace xe::kernel
