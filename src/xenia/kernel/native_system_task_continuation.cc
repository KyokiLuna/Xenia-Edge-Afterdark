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
#include <utility>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
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
GuestRuntime::ExecutionResult NativeSystemTaskContinuation::InvokeAndWaitImpl(
    XThread& bootstrap, Work work, Admission::Role role,
    uint32_t* completed_native_task) {
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
    const auto scheduled =
        Call(state_->kernel, bootstrap,
             state_->catalog->task_functions().schedule, args);
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
      bool completed;
      {
        std::lock_guard lock(state_->mutex);
        completed = invocation->completed;
      }
      if (completed) {
        break;
      }
      // Cooperative guest wait, not a host CV parked on a native worker.
      // BeginStop still drains this admitted complete callback before CPU stop.
      bootstrap.Delay(0, 0, uint64_t(-100000ll));
    }
    const std::array<uint32_t, 1> handle{invocation->handle};
    const auto waited = Call(state_->kernel, bootstrap,
                             state_->catalog->task_functions().wait, handle);
    if (!waited.executed || int32_t(waited.value) < 0) {
      std::lock_guard lock(state_->mutex);
      state_->admission.Poison();
    } else {
      const auto closed = Call(state_->kernel, bootstrap,
                               state_->catalog->task_functions().close, handle);
      if (!closed.executed) {
        std::lock_guard lock(state_->mutex);
        state_->admission.Poison();
      }
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
        invocation->entered) {
      return;
    }
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
    if (queue == State::Queue::kAccepted && !stopping &&
        state->domain->active()) {
      const auto caller =
          kernel->ResolveNativeImportCaller(uint32_t(context->lr));
      if (caller.domain == state->domain &&
          caller.module.get() == state->catalog->calls()->module().get()) {
        const auto current =
            Call(kernel, *worker, state->catalog->task_functions().current, {});
        if (current.executed && uint32_t(current.value) >= 0x1000 &&
            (uint32_t(current.value) & 3) == 1) {
          const std::array<uint32_t, 1> handle{uint32_t(current.value)};
          const auto closed = Call(
              kernel, *worker, state->catalog->task_functions().close, handle);
          if (!closed.executed) {
            std::lock_guard lock(state->mutex);
            state->admission.Poison();
          }
          if (closed.executed &&
              uint32_t(current.value) == invocation->handle) {
            result = invocation->work(*worker);
          }
        } else {
          // Failure/invalid ownership from the native AddRef getter cannot
          // certify that no reference or partial native operation remains.
          std::lock_guard lock(state->mutex);
          state->admission.Poison();
        }
      }
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
