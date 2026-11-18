/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_task_continuation.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_profile_identity_owner.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe::kernel {
namespace {
constexpr uint32_t kContinuationFailed = 0x80004005;
constexpr uint32_t kContinuationStopped = 0x80004004;
std::atomic<uint32_t> next_ticket{0x10000};

GuestRuntime::ExecutionResult Failure(const char* detail) {
  return {false, kContinuationFailed, detail};
}
bool IsSystem(XThread& thread, KernelState* kernel) {
  return XThread::IsInThread(&thread) && thread.kernel_state() == kernel &&
         thread.guest_object<X_KTHREAD>()->process_type == X_PROCTYPE_SYSTEM;
}
cpu::GuestCallResult Call(KernelState* kernel, XThread& thread,
                          const GuestRuntime::Function& function,
                          std::span<const uint32_t> args) {
  if (!IsSystem(thread, kernel) || args.size() != function.arguments.size() ||
      args.size() > 8) {
    return {};
  }
  std::array<cpu::GuestCallArgument, 8> packed;
  for (size_t i = 0; i < args.size(); ++i) {
    packed[i] = cpu::GuestCallArgument::UInt32(args[i]);
  }
  return cpu::ExecuteGuestCall(kernel->processor(), thread.thread_state(),
                               function.address,
                               std::span(packed).first(args.size()));
}
}  // namespace

struct NativeSystemTaskContinuation::State {
  enum class Queue { kPending, kAccepted, kRejected, kUncertain };
  struct Invocation {
    Work work;
    std::function<bool()> owner_current;
    std::function<bool(XThread&,std::span<const uint32_t>)> before_schedule;
    std::function<void(const cpu::GuestCallResult&)> after_schedule;
    bool owned_service=false;
    std::function<bool()> stop_retention_current;
    // Exact guest objects pin PCR/TLS after scheduler fiber reclamation. No
    // pointer into an abandoned C++ stack is used by stopped cleanup.
    object_ref<XThread> bootstrap_thread,callback_thread;
    bool schedule_returned=false,host_wait_interrupted=false;
    bool covered_identity_retention=false;
    bool post_wait_close=false,caller_release_attempted=false,caller_reference_released=false;
    uint32_t temporary_task_reference=0;
    bool temporary_reference_owned=false,temporary_release_attempted=false;
    NativeImportDomain::Operation domain_operation;
    uint32_t ticket = 0, output = 0, handle = 0;
    Queue queue = Queue::kPending;
    bool entered = false, completed = false;
    GuestRuntime::ExecutionResult result;
  };
  KernelState* kernel = nullptr;
  std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
  std::shared_ptr<NativeImportDomain> domain;
  std::mutex mutex;
  std::condition_variable drained;
  Admission admission;
  std::shared_ptr<Invocation> ActiveInvocation() const {
    return std::static_pointer_cast<Invocation>(admission.owner);
  }
  size_t bootstrap_calls = 0, callbacks = 0;
  bool stopped_retired=false;
  // Captured only after the real outer host wait returned. Same invocation,
  // not a reusable ready flag or a replacement callback ticket.
  std::shared_ptr<Invocation> retained_for_stop;

  static std::mutex registry_mutex;
  static std::map<uint32_t, std::weak_ptr<State>> registry;
  static std::vector<std::weak_ptr<State>> installations;
  static std::vector<std::shared_ptr<State>> ForKernel(KernelState* kernel) {
    std::vector<std::shared_ptr<State>> result;
    std::lock_guard lock(registry_mutex);
    for (auto it = installations.begin(); it != installations.end();) {
      auto item = it->lock();
      if (!item) {
        it = installations.erase(it);
      } else {
        if (item->kernel == kernel) {
          result.push_back(std::move(item));
        }
        ++it;
      }
    }
    return result;
  }
  bool DrainUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex);
    admission.stopping = true;
    const bool done = drained.wait_until(lock, deadline, [&] {
      return admission.uncertain || (!bootstrap_calls && !callbacks);
    });
    if(done && !admission.uncertain)admission.closed=true;
    return done && !admission.uncertain;
  }
};
std::mutex NativeSystemTaskContinuation::State::registry_mutex;
std::map<uint32_t, std::weak_ptr<NativeSystemTaskContinuation::State>>
    NativeSystemTaskContinuation::State::registry;
std::vector<std::weak_ptr<NativeSystemTaskContinuation::State>>
    NativeSystemTaskContinuation::State::installations;

NativeSystemTaskContinuation::NativeSystemTaskContinuation(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemTaskContinuation::~NativeSystemTaskContinuation() {
  BeginStop();
  std::shared_ptr<State::Invocation> invocation;
  {
    std::lock_guard lock(state_->mutex);
    invocation = state_->ActiveInvocation();
  }
  if (invocation) {
    std::lock_guard lock(State::registry_mutex);
    State::registry.erase(invocation->ticket);
  }
}

std::shared_ptr<NativeSystemTaskContinuation>
NativeSystemTaskContinuation::Create(
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog,
    std::shared_ptr<NativeImportDomain> domain) {
  if (!catalog || !domain || !domain->active() ||
      !catalog->allocation()->IsAccepting() ||
      !catalog->allocation()->MatchesDomain(domain) ||
      catalog->calls()->kernel() != catalog->allocation()->kernel() ||
      domain->root() !=
          NativeImportDomain::Member{catalog->calls()->module()->hmodule_ptr(),
                                     catalog->calls()->load_generation()} ||
      domain->profile()->owner().generation !=
          catalog->calls()->heap_runtime()->GetSnapshot().identity.generation) {
    return {};
  }
  const auto heap = catalog->calls()->heap_runtime()->GetSnapshot();
  if (heap.state != GuestRuntime::State::kReady || heap.initialization_active) {
    return {};
  }
  auto* bootstrap = XThread::GetCurrentThread();
  if (!bootstrap || !IsSystem(*bootstrap, catalog->calls()->kernel()) ||
      bootstrap->thread_id() != catalog->allocation()->bootstrap_thread_id()) {
    return {};
  }
  auto state = std::make_shared<State>();
  state->kernel = catalog->calls()->kernel();
  state->catalog = std::move(catalog);
  state->domain = std::move(domain);
  auto result = std::shared_ptr<NativeSystemTaskContinuation>(
      new NativeSystemTaskContinuation(state));
  // Retain before installing executable ingress. Partial initialization may
  // leave the entry bound; Kernel teardown still owns its control block.
  state->kernel->RetainNativeResourceUntilTeardown(result);
  {
    std::lock_guard lock(State::registry_mutex);
    State::installations.push_back(state);
  }
  // The catalog admits only the supplied 17559 root. This independent CPU
  // check verifies its full callback / Schedule .pdata extents and the actual
  // nine-instruction native registration dataflow; no direct BL is invented
  // for an entry that is normally reached by the native worker's indirect BL.
  const cpu::XexModule::GuestTaskCallbackRegistration registration{
      0x81696A10,
      36,
      "d3c7f725ddb2d088324fc2cc84534a645dce9c4cf1f011af7629c4a57fcd49e1",
      0x816BA440,
      232,
      "54499cbb31f67e590a4c36e36708688274d2c696f1a03529bd26782955c73fee"};
  if (!state->catalog->calls()
           ->module()
           ->xex_module()
           ->BindGuestTaskCallbackToHost(
               state->catalog->frontend_callback_entry().address, 592,
               "2274bb755437b3b81ae716552b93cd87c81d95b108efb4f47a127f66b65edd4"
               "d",
               registration, Enter, "Native SYSTEM startup continuation")) {
    result->BeginStop();
    return {};
  }
  return result;
}

GuestRuntime::ExecutionResult NativeSystemTaskContinuation::InvokeAndWait(
    XThread& bootstrap, Work work) {
  return InvokeAndWaitImpl(bootstrap, std::move(work), Admission::Role::kStartup);
}
NativeSystemTaskContinuation::DiagnosticResult
NativeSystemTaskContinuation::InvokePostBootDiagnosticAndWait(
    XThread& bootstrap, Work work) {
  DiagnosticResult result;
  result.execution = InvokeAndWaitImpl(bootstrap, std::move(work),
      Admission::Role::kPostBootDiagnostic, &result.completed_native_task);
  return result;
}
bool NativeSystemTaskContinuation::MatchesSharedSchedule(XThread& bootstrap,
    std::span<const uint32_t> args,
    const std::shared_ptr<const NativeSystemUiStartupCatalog>& catalog,
    const std::shared_ptr<NativeImportDomain>& domain) const {
  if(global_critical_region::is_held_by_current_thread() || !catalog || !domain ||
      state_->catalog!=catalog || state_->domain!=domain || !domain->active() ||
      !IsSystem(bootstrap,state_->kernel) ||
      bootstrap.thread_id()!=catalog->allocation()->bootstrap_thread_id() ||
      catalog->calls()->module()->load_generation()!=catalog->calls()->load_generation())return false;
  std::shared_ptr<State::Invocation> invocation;
  {
    std::lock_guard lock(state_->mutex);
    invocation=state_->ActiveInvocation();
    if(!invocation || !state_->bootstrap_calls || state_->stopped_retired ||
        invocation->bootstrap_thread.get()!=&bootstrap || !invocation->domain_operation ||
        invocation->schedule_returned || invocation->entered ||
        invocation->queue!=State::Queue::kPending ||
        !state_->admission.MatchesSharedScheduleOwner(invocation,args,
            catalog->frontend_callback_entry().address,invocation->output))return false;
  }
  // No State/identity metadata lock crosses the existing registry lookup.
  std::lock_guard lock(State::registry_mutex);
  const auto found=State::registry.find(invocation->ticket);
  return found!=State::registry.end() && found->second.lock().get()==state_.get();
}

bool NativeSystemTaskContinuation::MatchesTaskReleaseCaller(
    XThread& caller,uint32_t native_task) const {
  if(global_critical_region::is_held_by_current_thread() ||
      !IsSystem(caller,state_->kernel))return false;
  std::shared_ptr<State::Invocation> invocation;
  {
    std::lock_guard lock(state_->mutex);
    invocation=state_->ActiveInvocation();
    if(!invocation || state_->stopped_retired || !state_->catalog || !state_->domain ||
        !state_->domain->active() || !invocation->domain_operation ||
        state_->admission.owner.get()!=invocation.get() || state_->admission.ticket!=invocation->ticket ||
        invocation->queue!=State::Queue::kAccepted || !invocation->schedule_returned ||
        (invocation->handle&~1u)!=native_task)return false;
    const bool callback=invocation->entered && !invocation->completed && state_->callbacks &&
        invocation->callback_thread.get()==&caller;
    const bool final_release=invocation->completed && invocation->post_wait_close &&
        invocation->caller_release_attempted && !invocation->caller_reference_released &&
        invocation->bootstrap_thread.get()==&caller;
    if(!callback && !final_release)return false;
  }
  std::lock_guard lock(State::registry_mutex);
  const auto found=State::registry.find(invocation->ticket);
  return found!=State::registry.end() && found->second.lock().get()==state_.get();
}

GuestRuntime::ExecutionResult NativeSystemTaskContinuation::InvokeAndWaitImpl(
    XThread& bootstrap, Work work, Admission::Role role,
    uint32_t* completed_native_task,std::function<bool()> owner_current,
    std::function<bool(XThread&,std::span<const uint32_t>)> before_schedule,
    std::function<void(const cpu::GuestCallResult&)> after_schedule,
    std::function<bool()> stop_retention_current,
    std::function<void()> identity_host_maintenance) {
  // Private synchronous host phase, supplied only by the retained identity
  // owner. Never store it in Invocation or give it result/admission authority.
  if(identity_host_maintenance && role!=Admission::Role::kOwnedProfileIdentity)
    return Failure("Host identity maintenance requires its typed owned service");
  if(bool(before_schedule)!=bool(after_schedule) ||
      (!Admission::IsOwned(role) && before_schedule))
    return Failure("Schedule observation requires its typed owned service");
  if(Admission::IsOwned(role)!=bool(owner_current) ||
      (owner_current && !owner_current()))return Failure("Owned service serial closed or unproved");
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->admission.Claim(role)) {
      return Failure("native continuation role stopped, overlapping or consumed");
    }
    // Cover preflight, native-memory allocation and every exception tail too.
    // The service additionally joins its whole outer bootstrap host frame.
    state_->bootstrap_calls = 1;
  }
  struct BootstrapAdmission {
    std::shared_ptr<State> state;
    bool succeeded = false;
    ~BootstrapAdmission() {
      std::lock_guard lock(state->mutex);
      state->admission.Finish(!state->admission.uncertain, succeeded);
      state->bootstrap_calls = 0;
      state->drained.notify_all();
    }
  } bootstrap_admission{state_};
  if (!work || !IsSystem(bootstrap, state_->kernel) ||
      bootstrap.thread_id() !=
          state_->catalog->allocation()->bootstrap_thread_id()) {
    return Failure("native continuation requires its SYSTEM bootstrap");
  }
  auto invocation = std::make_shared<State::Invocation>();
  invocation->work = std::move(work);
  invocation->owner_current=std::move(owner_current);
  invocation->before_schedule=std::move(before_schedule);
  invocation->after_schedule=std::move(after_schedule);
  invocation->owned_service=Admission::IsOwned(role);
  invocation->stop_retention_current=std::move(stop_retention_current);
  invocation->bootstrap_thread=retain_object(&bootstrap);
  invocation->domain_operation = state_->domain->Acquire();
  if (!invocation->domain_operation) {
    return Failure("native continuation domain is closed");
  }
  uint32_t ticket = next_ticket.load();
  while (ticket && ticket < UINT32_MAX &&
         !next_ticket.compare_exchange_weak(ticket, ticket + 1)) {
  }
  if (!ticket || ticket == UINT32_MAX) {
    return Failure("native continuation ticket namespace exhausted");
  }
  invocation->ticket = ticket;
  invocation->output = state_->kernel->memory()->SystemHeapAlloc(4);
  if (!invocation->output) {
    return Failure("native continuation handle output allocation failed");
  }
  xe::store_and_swap<uint32_t>(
      state_->kernel->memory()->TranslateVirtual(invocation->output), 0);
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->admission.BindOwner(ticket, invocation)) {
      state_->kernel->memory()->SystemHeapFree(invocation->output);
      invocation->output = 0;
      return Failure("native continuation stopped before native Schedule");
    }
  }
  const auto finish = [&](bool release_completed_invocation = false) {
    if (release_completed_invocation) {
      // Successful native Wait proves the scheduled callback has returned to
      // its native completion path; Close has consumed our owned handle. A
      // rejected Schedule never transferred the callback. Unknown/partial
      // execution retains the whole invocation instead of guessing cleanup.
      {
        std::lock_guard lock(State::registry_mutex);
        State::registry.erase(invocation->ticket);
      }
      invocation->work = {};
      invocation->owner_current = {};
      invocation->before_schedule = {};
      invocation->after_schedule = {};
      invocation->domain_operation = {};
      state_->kernel->memory()->SystemHeapFree(invocation->output);
      invocation->output = 0;
    }
  };
  try {
    {
      std::lock_guard lock(State::registry_mutex);
      State::registry.emplace(ticket, state_);
    }
    const std::array<uint32_t, 4> args = {
        state_->catalog->frontend_callback_entry().address, ticket, 0,
        invocation->output};
    if(invocation->before_schedule && !invocation->before_schedule(bootstrap,args)) {
      // No native Schedule was attempted; the exact ticket/output remains ours.
      finish(true);return Failure("Owned service refused exact Schedule genealogy");
    }
    NativeProfileIdentityOwner::SharedScheduleObservation transport_observation;
    if(role!=Admission::Role::kOwnedProfileIdentity){
      transport_observation=NativeProfileIdentityOwner::ObserveSharedContinuationSchedule(
          *this,bootstrap,args);
      if(!transport_observation.admitted){
        finish(true);return Failure("Shared continuation Schedule receipt refused");
      }
    }
    const auto scheduled =
        Call(state_->kernel, bootstrap,
             state_->catalog->task_functions().schedule, args);
    {std::lock_guard lock(state_->mutex);invocation->schedule_returned=true;}
    if(transport_observation.returned)transport_observation.returned(scheduled);
    transport_observation.returned={}; // No observation value spans callback Work.
    if(invocation->after_schedule)invocation->after_schedule(scheduled);
    {
      std::lock_guard lock(state_->mutex);
      if (!scheduled.executed) {
        invocation->queue = State::Queue::kUncertain;
        state_->admission.Poison();
      } else if (int32_t(scheduled.value) < 0) {
        // Native Schedule may leave a freed tagged pointer in output. Never
        // adopt/Wait/Close it on a negative queue result.
        invocation->queue = State::Queue::kRejected;
        if (invocation->entered) state_->admission.Poison();
      } else {
        invocation->handle = xe::load_and_swap<uint32_t>(
            state_->kernel->memory()->TranslateVirtual(invocation->output));
        if (invocation->handle < 0x1000 || (invocation->handle & 3) != 1) {
          invocation->queue = State::Queue::kUncertain;
          state_->admission.Poison();
        } else {
          invocation->queue = State::Queue::kAccepted;
        }
      }
    }
    bool accepted, rejected_without_entry;
    {
      std::lock_guard lock(state_->mutex);
      accepted = invocation->queue == State::Queue::kAccepted;
      rejected_without_entry = invocation->queue == State::Queue::kRejected &&
                               !invocation->entered && !state_->admission.uncertain;
    }
    if (!accepted) {
      finish(rejected_without_entry);
      return Failure("native continuation Schedule failed or is uncertain");
    }
    for (;;) {
      bool completed,stopping,entered;
      {
        std::lock_guard lock(state_->mutex);
        completed = invocation->completed;
        stopping = state_->admission.stopping;
        entered = invocation->entered;
      }
      // Native workers may return many listener calls while this existing
      // synchronous wait is pending. Retire their proved host owners without
      // waiting for StepOnBootstrap to resume. No lock crosses this call and
      // failure cannot replace completion, native Wait/Close or stop handling.
      if(identity_host_maintenance){
        try{identity_host_maintenance();}catch(...){/* retain native control flow */}
      }
      if (completed) { break; }
      // Only a normally returned Schedule reaches this wait. A never-entered
      // callback can be denied before Work; active identity Work additionally
      // needs its exact owner's heap-retention proof, evaluated without locks.
      // Active STARTUP/GuestRuntime::Initialize and arbitrary media remain HOLD.
      bool covered_identity=false;
      if(stopping && entered && role==Admission::Role::kOwnedProfileIdentity &&
          invocation->stop_retention_current)
        covered_identity=invocation->stop_retention_current();
      if(stopping && (!entered || covered_identity)) {
        std::lock_guard lock(state_->mutex);
        if(!invocation->completed && state_->ActiveInvocation()==invocation &&
            invocation->schedule_returned && invocation->queue==State::Queue::kAccepted &&
            (!invocation->entered || covered_identity)) {
          invocation->host_wait_interrupted=true;
          invocation->covered_identity_retention=invocation->entered && covered_identity;
          state_->admission.Poison();
          // Do not Wait/Close, publish completion or release captured owners.
          // The real host frame can now return and be OS-joined by Service.
          return Failure("native continuation host wait interrupted; exact work retained");
        }
      }
      bootstrap.Delay(0, 0, uint64_t(-100000ll));
    }
    const std::array<uint32_t, 1> handle{invocation->handle};
    const auto waited = Call(state_->kernel, bootstrap,
                             state_->catalog->task_functions().wait, handle);
    if (!waited.executed || int32_t(waited.value) < 0) {
      std::lock_guard lock(state_->mutex);
      state_->admission.Poison();
    } else {
      {
        std::lock_guard lock(state_->mutex);
        if(invocation->caller_release_attempted){state_->admission.Poison();
          return Failure("Native continuation caller reference already attempted");}
        invocation->caller_release_attempted=true;invocation->post_wait_close=true;
      }
      struct ReleasePhase {
        std::shared_ptr<State> state;std::shared_ptr<State::Invocation> invocation;
        ~ReleasePhase(){std::lock_guard lock(state->mutex);invocation->post_wait_close=false;}
      } release_phase{state_,invocation};
      auto observed=NativeProfileIdentityOwner::BeforeNativeTaskClose(state_->kernel,bootstrap,handle[0]);
      cpu::GuestCallResult closed{};
      if(observed.admitted())closed=Call(state_->kernel,bootstrap,state_->catalog->task_functions().close,handle);
      const bool released=observed.Complete(closed.executed,uint32_t(closed.value));
      std::lock_guard lock(state_->mutex);
      invocation->caller_reference_released=released;
      if(!released)state_->admission.Poison();
    }
    GuestRuntime::ExecutionResult result;
    bool healthy;
    {
      std::lock_guard lock(state_->mutex);
      healthy = !state_->admission.uncertain;
      result = state_->admission.uncertain
                   ? Failure("native continuation completion rundown uncertain")
                   : invocation->result;
    }
    // Work result is captured by the callback. Wait/status are not its result.
    if (healthy && completed_native_task)
      *completed_native_task = invocation->handle;
    finish(healthy);
    bootstrap_admission.succeeded = healthy && result.succeeded;
    return result;
  } catch (...) {
    {
      std::lock_guard lock(state_->mutex);
      state_->admission.Poison();
      invocation->queue = State::Queue::kUncertain;
    }
    finish();
    throw;
  }
}

void NativeSystemTaskContinuation::Enter(cpu::ppc::PPCContext* context,
                                         KernelState* kernel) {
  const uint32_t ticket = uint32_t(context->r[3]);
  context->r[3] = kContinuationStopped;
  std::shared_ptr<State> state;
  {
    std::lock_guard lock(State::registry_mutex);
    const auto found = State::registry.find(ticket);
    if (found != State::registry.end()) {
      state = found->second.lock();
    }
  }
  if (!state || state->kernel != kernel || !XThread::IsInThread()) {
    return;
  }
  auto* worker = XThread::GetCurrentThread();
  if (!IsSystem(*worker, kernel)) {
    return;
  }
  std::shared_ptr<State::Invocation> invocation;
  {
    std::lock_guard lock(state->mutex);
    invocation = state->ActiveInvocation();
    if (!state->admission.MatchesTicket(ticket) || !invocation ||
        invocation->entered || invocation->host_wait_interrupted) {
      return;
    }
    invocation->callback_thread=retain_object(worker);
    invocation->entered = true;
    ++state->callbacks;
  }
  auto result = Failure("native continuation callback was stopped");
  try {
    State::Queue queue;
    bool stopping;
    for (;;) {
      {
        std::lock_guard lock(state->mutex);
        queue = invocation->queue;
        stopping = state->admission.stopping;
      }
      if (queue != State::Queue::kPending) {
        break;
      }
      worker->Delay(0, 0, uint64_t(-10000ll));
    }
    if (queue == State::Queue::kAccepted &&
        (!stopping || invocation->owned_service) && state->domain->active() &&
        (!invocation->owner_current || invocation->owner_current())) {
      bool work_admitted=false;
      {
        // The receipt's frame gate outlives the caller module/domain copies.
        std::optional<NativeProfileIdentityOwner::TaskCloseUse> close_observation;
        const auto caller =
            kernel->ResolveNativeImportCaller(uint32_t(context->lr));
        if (caller.domain == state->domain &&
            caller.module.get() == state->catalog->calls()->module().get()) {
          const auto current =
              Call(kernel, *worker, state->catalog->task_functions().current, {});
          if (current.executed && uint32_t(current.value) >= 0x1000 &&
              (uint32_t(current.value) & 3) == 1) {
            const std::array<uint32_t, 1> handle{uint32_t(current.value)};
            {
              std::lock_guard lock(state->mutex);
              invocation->temporary_task_reference=handle[0];
              invocation->temporary_reference_owned=true;
              invocation->temporary_release_attempted=true;
            }
            close_observation.emplace(NativeProfileIdentityOwner::BeforeNativeTaskClose(kernel,*worker,handle[0]));
            cpu::GuestCallResult closed{};
            if(close_observation->admitted())closed=Call(kernel,*worker,state->catalog->task_functions().close,handle);
            const bool released=close_observation->Complete(closed.executed,uint32_t(closed.value));
            {
              std::lock_guard lock(state->mutex);
              if(released)invocation->temporary_reference_owned=false;
              else state->admission.Poison();
            }
            work_admitted=released && uint32_t(current.value)==invocation->handle &&
                (!invocation->owner_current || invocation->owner_current());
          } else {
            // Failure/invalid ownership from the native AddRef getter cannot
            // certify that no reference or partial native operation remains.
            std::lock_guard lock(state->mutex);
            state->admission.Poison();
          }
        }
      }
      // Caller module/domain copies must finish before potentially retained
      // Work. Only the explicitly revocable heap State/Invocation spans Work;
      // a reclaimed C++ stack must not retain an unrevocable native code pin.
      if(work_admitted)result=invocation->work(*worker);
    }
  } catch (...) {
    {
      std::lock_guard lock(state->mutex);
      state->admission.Poison();
    }
    result = Failure("native continuation callback threw");
  }
  {
    std::lock_guard lock(state->mutex);
    invocation->result = std::move(result);
    invocation->completed = true;
    context->r[3] = invocation->result.succeeded ? 0 : kContinuationFailed;
    --state->callbacks;
    state->drained.notify_all();
  }
}

void NativeSystemTaskContinuation::BeginStop() {
  std::lock_guard lock(state_->mutex);
  state_->admission.stopping = true;
}
bool NativeSystemTaskContinuation::Drain() {
  if (XThread::IsInThread()) {
    return false;
  }
  return state_->DrainUntil(std::chrono::steady_clock::now() +
                            std::chrono::seconds(5));
}
bool NativeSystemTaskContinuation::RetainedForActualKernelStop() const noexcept try {
  if(XThread::IsInThread())return false;
  std::lock_guard lock(state_->mutex);
  if(!state_->admission.stopping || state_->bootstrap_calls || state_->stopped_retired ||
      !state_->kernel || !state_->catalog || !state_->domain)return false;
  const auto invocation=state_->ActiveInvocation();
  if(!invocation)return !state_->callbacks && !state_->admission.uncertain;
  if(!invocation->schedule_returned || !invocation->bootstrap_thread ||
      invocation->bootstrap_thread->is_running())return false;
  // An arbitrary still-running frontend initialization cannot be abandoned:
  // its GuestRuntime finalizer and C++ stack ownership are not this receipt.
  if(!invocation->completed &&
      (!invocation->host_wait_interrupted ||
       (invocation->entered && !invocation->covered_identity_retention)))return false;
  if(invocation->entered && (!invocation->callback_thread ||
      invocation->callback_thread->kernel_state()!=state_->kernel ||
      (state_->callbacks && invocation->callback_thread->is_host_thread())))return false;
  state_->retained_for_stop=invocation;
  return true;
} catch(...) {return false;}
bool NativeSystemTaskContinuation::RetireAfterActualKernelStop() noexcept try {
  if(XThread::IsInThread() || global_critical_region::is_held_by_current_thread())return false;
  KernelState* kernel;
  std::shared_ptr<State::Invocation> invocation;
  {
    std::lock_guard lock(state_->mutex);
    if(state_->stopped_retired)return true;
    kernel=state_->kernel;invocation=state_->ActiveInvocation();
    if(!kernel || !state_->admission.stopping || state_->bootstrap_calls ||
        (invocation && state_->retained_for_stop!=invocation) ||
        (!invocation && (state_->callbacks || state_->admission.uncertain)))return false;
  }
  // Service calls only after its actual CPU/IO and backend-GPU stop proof.
  // Check retained objects too: scheduler reclaim may have removed handles.
  for(const auto& thread:kernel->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread))
    if(thread->is_running())return false;
  if(invocation && ((invocation->callback_thread && invocation->callback_thread->is_running()) ||
      (invocation->bootstrap_thread && invocation->bootstrap_thread->is_running())))return false;
  if(invocation) {
    std::lock_guard lock(State::registry_mutex);
    const auto found=State::registry.find(invocation->ticket);
    if(found!=State::registry.end()) {
      const auto actual=found->second.lock();
      if(actual && actual!=state_)return false;
      State::registry.erase(found);
    }
  }
  std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
  std::shared_ptr<NativeImportDomain> domain;
  {
    std::lock_guard lock(state_->mutex);
    if(state_->ActiveInvocation()!=invocation || state_->bootstrap_calls)return false;
    state_->stopped_retired=true;state_->admission.closed=true;
    state_->admission.owner.reset();state_->retained_for_stop.reset();
    // This is stopped-resource retirement, NOT callback normal return.
    catalog=std::move(state_->catalog);domain=std::move(state_->domain);state_->kernel=nullptr;
  }
  if(invocation) {
    // Reclaimed guest stacks may retain shared_ptr copies to this same object.
    // Revoke its resources IN PLACE, after identity's exact scope detachment.
    invocation->work={};invocation->owner_current={};invocation->before_schedule={};
    invocation->after_schedule={};invocation->stop_retention_current={};
    invocation->domain_operation={};
    if(invocation->output){kernel->memory()->SystemHeapFree(invocation->output);invocation->output=0;}
    invocation->callback_thread.reset();invocation->bootstrap_thread.reset();
  }
  return true;
} catch(...) {return false;}
void NativeSystemTaskContinuation::BeginStopForKernel(KernelState* kernel) {
  for (const auto& state : State::ForKernel(kernel)) {
    std::lock_guard lock(state->mutex);
    state->admission.stopping = true;
  }
}
bool NativeSystemTaskContinuation::DrainForKernel(KernelState* kernel) {
  if (XThread::IsInThread()) {
    return false;
  }
  const auto states = State::ForKernel(kernel);
  for (const auto& state : states) {
    std::lock_guard lock(state->mutex);
    state->admission.stopping = true;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (const auto& state : states) {
    if (!state->DrainUntil(deadline)) {
      return false;
    }
  }
  return true;
}
}  // namespace xe::kernel
