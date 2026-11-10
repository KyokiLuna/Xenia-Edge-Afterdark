/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_allocation_authority.h"

#include <array>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_allocation_cohort.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_graphics_device_binding.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace {
constexpr uint16_t kGetCurrentTask = 0x1B6;
constexpr uint16_t kCloseTask = 0x1B1;
using Decision = NativeSystemPhysicalAllocationDecision;
using Session = NativeFrameOwnerSession;
bool CurrentSystem(KernelState* kernel, const XThread& thread) {
  const auto id = thread.native_frame_thread_identity();
  return XThread::IsInThread(const_cast<XThread*>(&thread)) &&
         id.kernel == kernel && id.thread == &thread && id.thread_id &&
         id.process_type == X_PROCTYPE_SYSTEM;
}
bool Code(const object_ref<UserModule>& module, uint32_t address) {
  xex2_section_type section;
  return module && address && !(address & 3) &&
         module->module_format() == UserModule::kModuleFormatXex &&
         module->xex_module()->GetPageSectionType(address, &section) &&
         section == XEX_SECTION_CODE;
}
// A successful GetCurrentTask adds a reference. No destructor executes Close.
// Allocate and authority-retain this holder BEFORE calling the native getter,
// so an allocation/forced unwind after its native AddRef cannot lose ownership.
struct TaskReference {
  uint32_t handle = 0;
  bool acquired = false;
  bool release_attempted = false;
};
}  // namespace

struct NativeSystemAllocationAuthority::State {
  KernelState* kernel;
  object_ref<UserModule> root;
  uint64_t root_generation;
  std::shared_ptr<GuestRuntime> runtime;
  std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
  Owner owner;
  object_ref<XThread> bootstrap_owner;
  XThread* bootstrap_thread = nullptr;
  uint32_t bootstrap_thread_id = 0;
  mutable std::mutex mutex;
  bool closed = false, retired = false;
  size_t allocations = 0;
  uint64_t next_invocation = 1;
  std::unique_ptr<NativeAllocationCohort> cohort;
  std::shared_ptr<NativeImportDomain> domain;
  std::vector<std::weak_ptr<NativeSystemFrameDelegation>> delegations;
  std::vector<std::shared_ptr<TaskReference>> task_references;
};
struct NativeSystemFrameDelegation::State {
  std::shared_ptr<NativeSystemAllocationAuthority> authority;
  std::shared_ptr<TaskReference> task;
  std::shared_ptr<Session> session;
  mutable std::mutex mutex;
  bool closed = false;
  std::weak_ptr<NativeSystemInvocationState> invocation;
};

class NativeSystemInvocationState {
 public:
  std::shared_ptr<NativeSystemAllocationAuthority> authority;
  std::shared_ptr<NativeSystemFrameDelegation> delegation;
  std::shared_ptr<TaskReference> temporary_task;
  NativeImportDomain::Operation domain_operation;
  XThread* thread = nullptr;
  uint32_t thread_id = 0;
  uint64_t serial = 0;
  bool bootstrap = false;
  std::atomic<bool> ended{false};

  bool Active() const {
    return !ended.load(std::memory_order_acquire) && authority->IsAccepting();
  }
  // Getters / Close use an exact module code lease retained by the authority.
  // Caller has already validated the original invocation and domain. Neither
  // operation runs under a Session/authority/thread/domain/Kernel mutex.
  static std::shared_ptr<TaskReference> GetTask(
      const std::shared_ptr<NativeSystemAllocationAuthority>& authority,
      XThread& thread) {
    auto& a = *authority->state_;
    const uint32_t entry = a.root->GetProcAddressByOrdinal(kGetCurrentTask);
    if (!Code(a.root, entry)) {
      return {};
    }
    auto reference = std::make_shared<TaskReference>();
    {
      std::lock_guard lock(a.mutex);
      if (a.closed) {
        return {};
      }
      a.task_references.push_back(reference);
    }
    const auto result = cpu::ExecuteGuestCall(a.kernel->processor(),
                                              thread.thread_state(), entry, {});
    if (!result.executed) {
      authority->CloseAdmission();
      return {};
    }
    reference->handle = uint32_t(result.value);
    reference->acquired = reference->handle >= 0x1000;
    // The verified public wrapper ORs tag1 into an aligned native task pointer.
    // Never interpret this as a kernel object handle or read/write Task fields.
    if (reference->handle < 0x1000 || (reference->handle & 3) != 1) {
      authority->CloseAdmission();
      return {};
    }
    return reference;
  }
  static bool CloseTask(
      const std::shared_ptr<NativeSystemAllocationAuthority>& authority,
      XThread& thread, const std::shared_ptr<TaskReference>& ref) {
    if (!ref || !ref->acquired || ref->release_attempted) {
      return false;
    }
    // Before PPC: a partial/throwing Close must never be retried on that
    // handle.
    ref->release_attempted = true;
    auto& a = *authority->state_;
    const auto entry = a.root->GetProcAddressByOrdinal(kCloseTask);
    if (!Code(a.root, entry)) {
      return false;
    }
    const std::array args{cpu::GuestCallArgument::UInt32(ref->handle)};
    const auto result = cpu::ExecuteGuestCall(
        a.kernel->processor(), thread.thread_state(), entry, args);
    if (!result.executed) {
      return false;
    }
    ref->acquired = false;
    {
      std::lock_guard lock(a.mutex);
      std::erase(a.task_references, ref);
    }
    return true;  // Close returns the native release result, not a BOOL ABI.
  }
  void Detach() noexcept {
    // Persistent XThread state survives an abandoned fiber's C++ stack.
    if (thread) {
      thread->DetachNativeSystemInvocation(this);
    }
    if (delegation) {
      std::lock_guard lock(delegation->state_->mutex);
      if (delegation->state_->invocation.lock().get() == this) {
        delegation->state_->invocation.reset();
      }
    }
  }
  void Fail() noexcept {
    if (ended.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    authority->CloseAdmission();  // closes/quarantines before scope detachment
    if (delegation && delegation->state_->session) {
      delegation->state_->session->Revoke();
    }
    Detach();
  }
};

NativeSystemInvocationScope::NativeSystemInvocationScope(
    std::shared_ptr<NativeSystemInvocationState> state)
    : state_(std::move(state)) {}
NativeSystemInvocationScope::NativeSystemInvocationScope(
    NativeSystemInvocationScope&& other) noexcept
    : state_(std::move(other.state_)) {}
NativeSystemInvocationScope& NativeSystemInvocationScope::operator=(
    NativeSystemInvocationScope&& other) noexcept {
  if (this != &other) {
    Abandon();
    state_ = std::move(other.state_);
  }
  return *this;
}
NativeSystemInvocationScope::~NativeSystemInvocationScope() { Abandon(); }
void NativeSystemInvocationScope::Abandon() noexcept {
  if (state_) {
    state_->Fail();
  }
  state_.reset();
}
NativeSystemInvocationScope::operator bool() const {
  return state_ && state_->Active();
}
bool NativeSystemInvocationScope::Complete(bool succeeded) {
  auto state = std::move(state_);
  if (!state) {
    return false;
  }
  const auto fail = [&] {
    state->Fail();
    return false;
  };
  if (!succeeded || !state->Active() ||
      !CurrentSystem(state->authority->state_->kernel, *state->thread)) {
    return fail();
  }
  if (state->delegation) {
    const auto frame = state->delegation->session()->GetSnapshot();
    if ((frame.phase != Session::Phase::kBound &&
         frame.phase != Session::Phase::kFrozen &&
         frame.phase != Session::Phase::kSubmitted &&
         frame.phase != Session::Phase::kRetiring) ||
        frame.update_active || frame.operation_active) {
      return fail();
    }
    try {
      if (!NativeSystemInvocationState::CloseTask(
              state->authority, *state->thread, state->temporary_task)) {
        return fail();
      }
    } catch (...) {
      state->Fail();
      throw;
    }
  }
  if (state->ended.exchange(true, std::memory_order_acq_rel)) {
    return false;
  }
  state->Detach();
  return state->authority->IsAccepting();
}

NativeSystemAllocationAuthority::NativeSystemAllocationAuthority(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemAllocationAuthority::~NativeSystemAllocationAuthority() = default;
const NativeSystemAllocationAuthority::Owner&
NativeSystemAllocationAuthority::owner() const {
  return state_->owner;
}
uint64_t NativeSystemAllocationAuthority::root_load_generation() const {
  return state_->root_generation;
}
KernelState* NativeSystemAllocationAuthority::kernel() const {
  std::lock_guard lock(state_->mutex);
  return state_->closed ? nullptr : state_->kernel;
}
object_ref<UserModule> NativeSystemAllocationAuthority::root_module() const {
  std::lock_guard lock(state_->mutex);
  return state_->closed ? object_ref<UserModule>{} : state_->root;
}
uint32_t NativeSystemAllocationAuthority::bootstrap_thread_id() const {
  return state_->bootstrap_thread_id;
}

std::shared_ptr<NativeSystemAllocationAuthority>
NativeSystemAllocationAuthority::Reserve(
    KernelState* kernel, const object_ref<UserModule>& root,
    const std::shared_ptr<GuestRuntime>& runtime) {
  if (!kernel || !root || !runtime || !GuestScheduler::enabled() ||
      !XThread::IsInThread()) {
    return {};
  }
  auto* bootstrap = XThread::GetCurrentThread();
  if (!bootstrap || !CurrentSystem(kernel, *bootstrap) ||
      bootstrap->native_frame_owner_session() ||
      bootstrap->native_system_invocation()) {
    return {};
  }
  const auto runtime_state = runtime->GetSnapshot();
  if (runtime_state.state != GuestRuntime::State::kResourcesLoaded ||
      runtime_state.initialization_active || runtime_state.plan ||
      runtime_state.identity.hmodule != root->hmodule_ptr()) {
    return {};
  }
  auto code = kernel->AcquireUserModuleExecutionLease(root);
  if (!code) {
    return {};
  }
  auto data = std::make_unique<State>();
  data->kernel = kernel;
  data->root = root;
  data->root_generation = root->load_generation();
  data->runtime = runtime;
  data->root_code = std::move(code);
  data->bootstrap_owner = retain_object(bootstrap);
  data->bootstrap_thread = bootstrap;
  data->bootstrap_thread_id = bootstrap->thread_id();
  data->owner = Session::ReserveAllocationOwner(
      NativeGraphicsDeviceBinding::ReserveEpoch(), runtime_state.identity);
  if (!data->owner.valid()) {
    return {};
  }
  data->cohort = std::make_unique<NativeAllocationCohort>(data->owner);
  auto authority = std::shared_ptr<NativeSystemAllocationAuthority>(
      new NativeSystemAllocationAuthority(std::move(data)));
  auto lock = kernel->global_critical_region_.Acquire();
  if (root->kernel_state() != kernel || !root->is_system_app_module() ||
      root.get() == kernel->executable_module_.get() ||
      root->module_format() != UserModule::kModuleFormatXex ||
      root->native_import_domain_id() || !root->load_generation() ||
      kernel->GetGuestRuntime(root->hmodule_ptr()) != runtime ||
      kernel->GetUserModuleForAddress(root->xex_module()->base_address())
              .get() != root.get()) {
    return {};
  }
  // Actual scheduler joins must be available for this native cohort.
  for (const auto& thread :
       kernel->object_table_.GetObjectsByType<XThread>(XObject::Type::Thread)) {
    if (!thread->is_host_thread() && thread->thread()) {
      return {};
    }
  }
  for (const auto& old : kernel->native_system_allocation_authorities_) {
    if (old->owner().module_hmodule == root->hmodule_ptr()) {
      return {};
    }
  }
  // Retain before the first getter, scope, allocation or guest invocation.
  kernel->native_system_allocation_authorities_.push_back(authority);
  return authority;
}

bool NativeSystemAllocationAuthority::IsAccepting() const {
  std::shared_ptr<GuestRuntime> runtime;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->retired) {
      return false;
    }
    runtime = state_->runtime;
  }
  if (!runtime) {
    return false;
  }
  const auto snapshot = runtime->GetSnapshot();
  return snapshot.identity.hmodule == owner().module_hmodule &&
         snapshot.identity.generation == owner().module_generation &&
         snapshot.state != GuestRuntime::State::kFailed &&
         snapshot.state != GuestRuntime::State::kRetiring;
}
bool NativeSystemAllocationAuthority::MatchesDomain(
    const std::shared_ptr<NativeImportDomain>& domain) const {
  std::lock_guard lock(state_->mutex);
  return domain && state_->domain == domain;  // includes closed tombstones
}
bool NativeSystemAllocationAuthority::ValidateCurrent(
    XThread& thread, uint32_t lr, bool require_domain,
    bool require_system) const {
  const auto id = thread.native_frame_thread_identity();
  if (!XThread::IsInThread(&thread) || id.kernel != state_->kernel ||
      id.thread != &thread || !id.thread_id ||
      (id.process_type != X_PROCTYPE_SYSTEM &&
       id.process_type != X_PROCTYPE_TITLE) ||
      (require_system && id.process_type != X_PROCTYPE_SYSTEM) ||
      !IsAccepting() || lr < 4 || (lr & 3)) {
    return false;
  }
  const auto caller = state_->kernel->ResolveNativeImportCaller(lr);
  if (require_domain) {
    std::shared_ptr<NativeImportDomain> domain;
    {
      std::lock_guard lock(state_->mutex);
      domain = state_->domain;
    }
    return caller.scoped && caller.domain == domain && domain &&
           domain->active();
  }
  return !caller.scoped && caller.module.get() == state_->root.get() &&
         caller.module->load_generation() == state_->root_generation &&
         Code(caller.module, lr - 4);
}

NativeSystemAllocationAuthority::BootstrapScope
NativeSystemAllocationAuthority::EnterBootstrap(
    XThread& thread, const GuestRuntime::Function& function) {
  std::shared_ptr<GuestRuntime> runtime_owner;
  object_ref<UserModule> root;
  object_ref<XThread> bootstrap;
  KernelState* kernel = nullptr;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->retired || !state_->runtime ||
        !state_->root || !state_->bootstrap_owner || !state_->kernel) {
      return {};
    }
    runtime_owner = state_->runtime;
    root = state_->root;
    bootstrap = state_->bootstrap_owner;
    kernel = state_->kernel;
  }
  const auto runtime = runtime_owner->GetSnapshot();
  if (!CurrentSystem(kernel, thread) || &thread != bootstrap.get() ||
      thread.thread_id() != state_->bootstrap_thread_id || !IsAccepting() ||
      runtime.state != GuestRuntime::State::kInitializing ||
      !runtime.initialization_active || !runtime.plan ||
      !runtime.current_function ||
      runtime.current_function->phase >= runtime.plan->phases.size()) {
    CloseAdmission();
    return {};
  }
  const auto& phase = runtime.plan->phases[runtime.current_function->phase];
  if (runtime.current_function->function >= phase.functions.size() ||
      phase.functions[runtime.current_function->function] != function ||
      kernel->GetUserModuleForAddress(function.address).get() != root.get() ||
      !Code(root, function.address)) {
    CloseAdmission();
    return {};
  }
  auto invocation = std::make_shared<NativeSystemInvocationState>();
  invocation->authority = shared_from_this();
  invocation->thread = &thread;
  invocation->thread_id = thread.thread_id();
  invocation->bootstrap = true;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->domain ||
        state_->next_invocation == UINT64_MAX) {
      return {};
    }
    invocation->serial = state_->next_invocation++;
  }
  if (!thread.AttachNativeSystemInvocation(invocation)) {
    CloseAdmission();
    return {};
  }
  return BootstrapScope(std::move(invocation));
}

bool NativeSystemAllocationAuthority::BindDomain(
    const std::shared_ptr<NativeImportDomain>& domain) {
  std::shared_ptr<GuestRuntime> runtime_owner;
  object_ref<UserModule> root;
  object_ref<XThread> bootstrap;
  KernelState* kernel = nullptr;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->retired || !state_->runtime ||
        !state_->root || !state_->bootstrap_owner || !state_->kernel) {
      return false;
    }
    runtime_owner = state_->runtime;
    root = state_->root;
    bootstrap = state_->bootstrap_owner;
    kernel = state_->kernel;
  }
  const auto runtime = runtime_owner->GetSnapshot();
  if (!CurrentSystem(kernel, *bootstrap) || !domain || !domain->active() ||
      !IsAccepting() || bootstrap->HasNativeSystemBootstrap(this) ||
      runtime.state != GuestRuntime::State::kReady ||
      runtime.initialization_active ||
      domain->root() != NativeImportDomain::Member{owner().module_hmodule,
                                                   state_->root_generation} ||
      domain->profile()->owner().generation != owner().module_generation ||
      root->native_import_domain_id() != domain->id()) {
    CloseAdmission();
    return false;
  }
  const auto caller = kernel->ResolveNativeImportCaller(
      root->GetProcAddressByOrdinal(kGetCurrentTask) + 4);
  if (caller.domain != domain || caller.module.get() != root.get()) {
    CloseAdmission();
    return false;
  }
  std::lock_guard lock(state_->mutex);
  if (state_->closed || state_->domain) {
    return false;
  }
  state_->domain = domain;
  return true;
}

NativeSystemPhysicalAllocationDecision
NativeSystemAllocationAuthority::DecidePhysicalAllocation(XThread& thread,
                                                          uint32_t lr) {
  bool bound;
  {
    std::lock_guard lock(state_->mutex);
    bound = bool(state_->domain);
  }
  if (!ValidateCurrent(thread, lr, bound, !bound)) {
    return {Decision::Kind::kDenied};
  }
  if (!bound && !thread.HasNativeSystemBootstrap(this)) {
    return {Decision::Kind::kDenied};
  }
  std::shared_ptr<NativeImportDomain> domain;
  {
    std::lock_guard lock(state_->mutex);
    domain = state_->domain;
  }
  struct Allocation {
    std::shared_ptr<NativeSystemAllocationAuthority> authority;
    NativeImportDomain::Operation domain;
    std::shared_ptr<KernelState::UserModuleExecutionLease> caller_code;
    bool armed = false;
    ~Allocation() {
      if (armed) {
        std::lock_guard lock(authority->state_->mutex);
        --authority->state_->allocations;
      }
    }
  };
  auto reservation = std::make_shared<Allocation>();
  reservation->authority = shared_from_this();
  if (bound) {
    reservation->domain = domain->Acquire();
    const auto caller = state_->kernel->ResolveNativeImportCaller(lr);
    reservation->caller_code =
        state_->kernel->AcquireUserModuleExecutionLease(caller.module);
    if (!reservation->domain || !reservation->caller_code ||
        caller.domain != domain) {
      return {Decision::Kind::kDenied};
    }
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->allocations == SIZE_MAX) {
      return {Decision::Kind::kDenied};
    }
    ++state_->allocations;
    reservation->armed = true;
  }
  return {Decision::Kind::kOwned, owner(), std::move(reservation)};
}

std::shared_ptr<NativeSystemFrameDelegation>
NativeSystemAllocationAuthority::DelegateCurrentRenderTask(XThread& thread,
                                                           uint32_t lr) {
  if (!ValidateCurrent(thread, lr, true) ||
      state_->runtime->GetSnapshot().state != GuestRuntime::State::kReady) {
    CloseAdmission();
    return {};
  }
  std::shared_ptr<NativeImportDomain> domain;
  {
    std::lock_guard lock(state_->mutex);
    domain = state_->domain;
  }
  auto domain_operation =
      domain ? domain->Acquire() : NativeImportDomain::Operation{};
  if (!domain_operation) {
    CloseAdmission();
    return {};
  }
  auto data = std::make_unique<NativeSystemFrameDelegation::State>();
  data->authority = shared_from_this();
  auto delegation = std::shared_ptr<NativeSystemFrameDelegation>(
      new NativeSystemFrameDelegation(std::move(data)));
  try {
    delegation->state_->task =
        NativeSystemInvocationState::GetTask(shared_from_this(), thread);
    if (!delegation->state_->task || !ValidateCurrent(thread, lr, true)) {
      CloseAdmission();
      return {};
    }
    // Repeated transport entries on the same still-referenced native Task
    // reuse its Session, not one retained Kernel holder per frame.
    std::vector<std::weak_ptr<NativeSystemFrameDelegation>> existing;
    {
      std::lock_guard lock(state_->mutex);
      existing = state_->delegations;
    }
    for (const auto& weak : existing) {
      auto old = weak.lock();
      bool same = false;
      if (old) {
        std::lock_guard lock(old->state_->mutex);
        same = !old->state_->closed && old->state_->task &&
               old->state_->task->handle == delegation->state_->task->handle;
      }
      if (same) {
        if (!NativeSystemInvocationState::CloseTask(shared_from_this(), thread,
                                                    delegation->state_->task)) {
          CloseAdmission();
          return {};
        }
        return old;
      }
    }
    // Native reference is already in task_references if any host allocation
    // throws here. Only a genuinely new Task needs a persistent Session holder.
    state_->kernel->RetainNativeResourceUntilTeardown(delegation);
    delegation->state_->session =
        Session::CreateDelegated(delegation, state_->runtime);
    if (!delegation->state_->session) {
      CloseAdmission();
      return {};
    }
    std::lock_guard lock(state_->mutex);
    if (state_->closed) {
      return {};
    }
    state_->delegations.push_back(delegation);
    return delegation;
  } catch (...) {
    CloseAdmission();
    throw;
  }
}
uint64_t NativeSystemAllocationAuthority::ReserveSourceSerial() {
  const auto value = state_->cohort->ReserveSourceSerial();
  if (!value) {
    CloseAdmission();
  }
  return value;
}
std::shared_ptr<void> NativeSystemAllocationAuthority::AcquireFrameWriter() {
  return IsAccepting() ? state_->cohort->AcquireWriter() : nullptr;
}
std::shared_ptr<void> NativeSystemFrameDelegation::AcquireFrameWriter() {
  return IsAccepting() ? state_->authority->AcquireFrameWriter() : nullptr;
}
std::shared_ptr<Session> NativeSystemInvocationSession(
    const std::shared_ptr<NativeSystemInvocationState>& state) {
  return state && state->Active() && state->delegation
             ? state->delegation->session()
             : nullptr;
}
bool NativeSystemInvocationIsBootstrap(
    const std::shared_ptr<NativeSystemInvocationState>& state,
    const NativeSystemAllocationAuthority* authority, const XThread* thread) {
  return state && state->bootstrap && state->authority.get() == authority &&
         state->thread == thread && state->thread_id == thread->thread_id() &&
         state->Active();
}
void RevokeNativeSystemInvocationState(
    const std::shared_ptr<NativeSystemInvocationState>& state) noexcept {
  if (state) {
    state->Fail();
  }
}

void NativeSystemAllocationAuthority::CloseAdmission() noexcept {
  // No allocation or calls under this mutex. Sessions consult authority
  // closure at every later admission; no new update/consume can pass it.
  state_->cohort->Close();
  std::lock_guard lock(state_->mutex);
  state_->closed = true;
}
bool NativeSystemAllocationAuthority::RetireAfterKernelStop(
    const std::function<bool(const Owner&)>& proof) {
  CloseAdmission();
  std::vector<std::shared_ptr<NativeSystemFrameDelegation>> delegations;
  object_ref<XThread> bootstrap_hold;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired) {
      return true;
    }
    if (state_->allocations) {
      return false;
    }
    for (const auto& weak : state_->delegations) {
      if (auto value = weak.lock()) {
        delegations.push_back(std::move(value));
      }
    }
  }
  if (!proof || !proof(owner())) {
    return false;
  }
  if (bootstrap_hold) {
    bootstrap_hold->RevokeNativeSystemInvocation();
  }
  for (const auto& delegation : delegations) {
    std::shared_ptr<NativeSystemInvocationState> invocation;
    {
      std::lock_guard lock(delegation->state_->mutex);
      delegation->state_->closed = true;
      invocation = delegation->state_->invocation.lock();
    }
    if (invocation) {
      invocation->Fail();
    }
    if (delegation->session() &&
        !delegation->session()->RetireAfterKernelStop(proof)) {
      return false;
    }
  }
  // Release every code/runtime/thread lifetime while the real Kernel is still
  // alive. External metadata handles can outlive it only as closed tombstones.
  object_ref<UserModule> root;
  object_ref<XThread> bootstrap;
  std::shared_ptr<GuestRuntime> runtime;
  std::shared_ptr<KernelState::UserModuleExecutionLease> code;
  std::shared_ptr<NativeImportDomain> domain;
  std::vector<std::shared_ptr<TaskReference>> native_references;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->allocations) {
      return false;
    }
    state_->retired = true;
    root = std::move(state_->root);
    bootstrap = std::move(state_->bootstrap_owner);
    runtime = std::move(state_->runtime);
    code = std::move(state_->root_code);
    domain = std::move(state_->domain);
    native_references.swap(state_->task_references);
    state_->kernel = nullptr;
    state_->bootstrap_thread = nullptr;
  }
  return true;
}

NativeSystemFrameDelegation::NativeSystemFrameDelegation(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemFrameDelegation::~NativeSystemFrameDelegation() = default;
const memory::PhysicalAllocationOwner& NativeSystemFrameDelegation::owner()
    const {
  return state_->authority->owner();
}
const std::shared_ptr<Session>& NativeSystemFrameDelegation::session() const {
  return state_->session;
}
std::shared_ptr<NativeSystemAllocationAuthority>
NativeSystemFrameDelegation::authority() const {
  return state_->authority;
}
bool NativeSystemFrameDelegation::IsAccepting() const {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed) {
      return false;
    }
  }
  return state_->authority->IsAccepting();
}
uint64_t NativeSystemFrameDelegation::ReserveSourceSerial() {
  return state_->authority->ReserveSourceSerial();
}
bool NativeSystemFrameDelegation::MatchesCurrentThread(
    const KernelState* kernel, const XThread* thread, uint32_t thread_id,
    uint32_t process_type) const {
  if (!thread || process_type != X_PROCTYPE_SYSTEM || !IsAccepting() ||
      !CurrentSystem(state_->authority->state_->kernel, *thread) ||
      kernel != state_->authority->state_->kernel ||
      thread->thread_id() != thread_id) {
    return false;
  }
  std::shared_ptr<NativeSystemInvocationState> invocation;
  {
    std::lock_guard lock(state_->mutex);
    invocation = state_->invocation.lock();
  }
  return invocation && invocation->thread == thread &&
         invocation->thread_id == thread_id && invocation->Active() &&
         thread->HasNativeSystemInvocation(invocation.get());
}
NativeSystemFrameDelegation::InvocationScope
NativeSystemFrameDelegation::EnterCurrentTask(XThread& thread, uint32_t lr) {
  auto authority = state_->authority;
  if (!IsAccepting() || !authority->ValidateCurrent(thread, lr, true)) {
    authority->CloseAdmission();
    return {};
  }
  auto invocation = std::make_shared<NativeSystemInvocationState>();
  invocation->authority = authority;
  invocation->delegation = shared_from_this();
  invocation->thread = &thread;
  invocation->thread_id = thread.thread_id();
  try {
    invocation->temporary_task =
        NativeSystemInvocationState::GetTask(authority, thread);
    if (!invocation->temporary_task ||
        invocation->temporary_task->handle != state_->task->handle ||
        !authority->ValidateCurrent(thread, lr, true)) {
      if (invocation->temporary_task) {
        NativeSystemInvocationState::CloseTask(authority, thread,
                                               invocation->temporary_task);
      }
      authority->CloseAdmission();
      return {};
    }
    {
      std::lock_guard lock(authority->state_->mutex);
      if (authority->state_->closed ||
          authority->state_->next_invocation == UINT64_MAX) {
        return {};
      }
      invocation->serial = authority->state_->next_invocation++;
      invocation->domain_operation = authority->state_->domain->Acquire();
    }
    if (!invocation->domain_operation) {
      authority->CloseAdmission();
      return {};
    }
    {
      std::lock_guard lock(state_->mutex);
      if (state_->closed || !state_->invocation.expired()) {
        authority->CloseAdmission();
        return {};
      }
      state_->invocation = invocation;
    }
    if (!thread.AttachNativeSystemInvocation(invocation)) {
      invocation->Fail();
      return {};
    }
    return InvocationScope(std::move(invocation));
  } catch (...) {
    invocation->Fail();
    throw;
  }
}
bool NativeSystemFrameDelegation::CloseOnCurrentTask(XThread& thread,
                                                     uint32_t lr) {
  auto authority = state_->authority;
  if (!IsAccepting() || !authority->ValidateCurrent(thread, lr, true)) {
    return false;
  }
  const auto frame = session()->GetSnapshot();
  if (frame.phase != Session::Phase::kBound || frame.update_active ||
      frame.operation_active || frame.source_serial || frame.gpu_attached ||
      frame.submission_attempted) {
    return false;
  }
  std::shared_ptr<TaskReference> current;
  try {
    current = NativeSystemInvocationState::GetTask(authority, thread);
    if (!current || current->handle != state_->task->handle) {
      if (current) {
        NativeSystemInvocationState::CloseTask(authority, thread, current);
      }
      authority->CloseAdmission();
      return false;
    }
    {
      std::lock_guard lock(state_->mutex);
      if (state_->closed || !state_->invocation.expired()) {
        return false;
      }
      state_->closed = true;
    }
    // Before releasing the persistent native ref, close Session admission.
    if (!session()->RetireDelegatedIdle()) {
      authority->CloseAdmission();
      return false;
    }
    if (!NativeSystemInvocationState::CloseTask(authority, thread,
                                                state_->task) ||
        !NativeSystemInvocationState::CloseTask(authority, thread, current)) {
      authority->CloseAdmission();
      return false;
    }
    return true;
  } catch (...) {
    authority->CloseAdmission();
    throw;
  }
}

bool KernelState::HasNativeSystemAllocationAuthorities() {
  auto lock = global_critical_region_.Acquire();
  return !native_system_allocation_authorities_.empty();
}
void KernelState::CloseNativeSystemAllocationAdmission() {
  decltype(native_system_allocation_authorities_) values;
  {
    auto lock = global_critical_region_.Acquire();
    values = native_system_allocation_authorities_;
  }
  for (const auto& authority : values) {
    authority->CloseAdmission();
  }
}
bool KernelState::RetireNativeSystemAllocationsAfterKernelStop(
    const std::function<bool(const memory::PhysicalAllocationOwner&)>& proof) {
  decltype(native_system_allocation_authorities_) values;
  {
    auto lock = global_critical_region_.Acquire();
    values = native_system_allocation_authorities_;
  }
  for (const auto& authority : values) {
    if (!authority->RetireAfterKernelStop(proof)) {
      return false;
    }
  }
  return true;
}

NativeSystemPhysicalAllocationDecision
KernelState::DecideNativeSystemPhysicalAllocation(uint32_t lr) {
  if (!XThread::IsInThread()) {
    return {};
  }
  auto& thread = *XThread::GetCurrentThread();
  const auto caller = ResolveNativeImportCaller(lr);
  decltype(native_system_allocation_authorities_) authorities;
  {
    auto lock = global_critical_region_.Acquire();
    authorities = native_system_allocation_authorities_;
  }
  for (const auto& authority : authorities) {
    const bool claimed = caller.scoped
                             ? authority->MatchesDomain(caller.domain)
                             : caller.module &&
                                   caller.module->hmodule_ptr() ==
                                       authority->owner().module_hmodule &&
                                   caller.module->load_generation() ==
                                       authority->root_load_generation();
    if (claimed) {
      return authority->DecidePhysicalAllocation(thread, lr);
    }
  }
  // A claimed caller / active scope can never silently enter the unowned pool.
  return (caller.scoped || thread.native_system_invocation())
             ? Decision{Decision::Kind::kDenied}
             : Decision{};
}
}  // namespace xe::kernel
