/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_callback_dispatcher.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe::kernel {
namespace {
std::mutex registry_mutex;
struct RegistryEntry {
  KernelState* kernel;
  std::weak_ptr<NativeSystemCallbackDispatcher> dispatcher;
};
std::vector<RegistryEntry> registry;
std::vector<std::shared_ptr<NativeSystemCallbackDispatcher>> ForKernel(
    KernelState* kernel) {
  std::vector<std::shared_ptr<NativeSystemCallbackDispatcher>> result;
  std::lock_guard lock(registry_mutex);
  for (auto it = registry.begin(); it != registry.end();) {
    auto item = it->dispatcher.lock();
    if (!item) {
      it = registry.erase(it);
    } else {
      if (it->kernel == kernel) {
        result.push_back(std::move(item));
      }
      ++it;
    }
  }
  return result;
}
void Complete(NativeSystemCallbackDispatcher::Completion& completed,
              bool executed, uint32_t value) noexcept {
  if (completed) {
    try {
      completed(executed, value);
    } catch (...) {
      // No caller completion may unwind the kernel's SYSTEM worker.
    }
  }
}
}  // namespace

struct NativeSystemCallbackDispatcher::State {
  struct Work {
    uint32_t address;
    uint64_t input_trace_id = 0;  // metadata only, never a guest argument
    std::vector<cpu::GuestCallArgument> arguments;
    object_ref<UserModule> module;
    std::shared_ptr<KernelState::UserModuleExecutionLease> lease;
    NativeImportDomain::Operation operation;
    Completion completed;
    BeforeEnter before_enter;
  };
  KernelState* kernel;
  std::shared_ptr<NativeImportDomain> domain;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::unique_ptr<Work>> queue;
  bool stopping = false;
  object_ref<XHostThread> thread;

  int Run() {
    for (;;) {
      std::unique_ptr<Work> work;
      {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping) {
          break;
        }
        work = std::move(queue.front());
        queue.pop_front();
      }
      bool entered = false;
      uint32_t value = 0;
      // Rundown covers the completion tail too, including an executor throw.
      // Keep this outside try so unwinding cannot release the registration
      // before Complete reports that failed/partial invocation.
      std::shared_ptr<void> reservation;
      try {
        // Recheck the native module/domain after queueing. The lease keeps its
        // code mapped; retirement still suppresses late callback entry.
        const auto caller =
            kernel->ResolveNativeImportCaller(work->address + 4);
        if (caller.scoped && caller.domain == domain &&
            caller.module.get() == work->module.get() && domain->active()) {
          if (work->before_enter) {
            reservation = work->before_enter();
          }
          if ((!work->before_enter || reservation) && domain->active()) {
            auto* current = XThread::GetCurrentThread();
            static std::array<xe::NativeSystemUiTraceLimiter, 2> attempt_traces;
            if (work->input_trace_id) {
              attempt_traces[work->address == 0x816F2E60].Emit(
                  "input", "ppc_attempt",
                  {{"input_trace_id", work->input_trace_id},
                   {"domain_id", domain->id()},
                   {"target", work->address},
                   {"thread_id", current->thread_id()}});
            }
            const auto result = cpu::ExecuteGuestCall(
                kernel->processor(), current->thread_state(), work->address,
                work->arguments);
            entered = result.executed;
            value = static_cast<uint32_t>(result.value);
            static std::array<xe::NativeSystemUiTraceLimiter, 2> return_traces;
            if (work->input_trace_id) {
              return_traces[work->address == 0x816F2E60].Emit(
                  "input", "ppc_return",
                  {{"input_trace_id", work->input_trace_id},
                   {"domain_id", domain->id()},
                   {"target", work->address},
                   {"thread_id", current->thread_id()},
                   {"executed", entered},
                   {"raw_result", value}});
            }
            if (!entered) {
              std::lock_guard lock(mutex);
              stopping = true;
            }
          }
          Complete(work->completed, entered, value);
        } else {
          Complete(work->completed, false, 0);
        }
      } catch (...) {
        static xe::NativeSystemUiTraceLimiter trace;
        if (work->input_trace_id) {
          trace.Emit("input", "ppc_exception_or_preflight_failure",
                     {{"input_trace_id", work->input_trace_id},
                      {"domain_id", domain->id()},
                      {"target", work->address}});
        }
        Complete(work->completed, false, 0);
        // A failed/partial native callback cannot be retried against globals.
        std::lock_guard lock(mutex);
        stopping = true;
      }
    }
    std::deque<std::unique_ptr<Work>> discarded;
    {
      std::lock_guard lock(mutex);
      discarded.swap(queue);
    }
    for (auto& work : discarded) {
      Complete(work->completed, false, 0);
    }
    return 0;
  }
};

NativeSystemCallbackDispatcher::NativeSystemCallbackDispatcher(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}

std::shared_ptr<NativeSystemCallbackDispatcher>
NativeSystemCallbackDispatcher::Create(
    KernelState* kernel, std::shared_ptr<NativeImportDomain> domain) {
  if (!kernel || !domain || !domain->active()) {
    return {};
  }
  auto state = std::make_shared<State>();
  state->kernel = kernel;
  state->domain = std::move(domain);
  auto result = std::shared_ptr<NativeSystemCallbackDispatcher>(
      new NativeSystemCallbackDispatcher(state));
  // The worker waits for queued work, so no start suspension is needed.
  // A weak capture avoids State -> XHostThread -> State ownership cycles.
  const std::weak_ptr<State> weak_state = state;
  state->thread = object_ref<XHostThread>(new XHostThread(
      kernel, 1024 * 1024, 0,
      [weak_state] {
        const auto running = weak_state.lock();
        return running ? running->Run() : 0;
      },
      kernel->GetSystemProcess()));
  state->thread->set_name("Native SYSTEM driver callbacks");
  if (XFAILED(state->thread->Create())) {
    state->stopping = true;
    state->thread.reset();
    return {};
  }
  {
    std::lock_guard lock(registry_mutex);
    registry.push_back({kernel, result});
  }
  return result;
}

NativeSystemCallbackDispatcher::~NativeSystemCallbackDispatcher() {
  BeginStop();
  if (!DrainAndJoin()) {
    // A self-join is not a teardown boundary. Kernel lifecycle must retain the
    // service until an external owner joins it; never free live callback data.
    std::terminate();
  }
}

bool NativeSystemCallbackDispatcher::Enqueue(
    uint32_t address, std::span<const uint32_t> arguments, Completion completed,
    BeforeEnter before_enter, uint64_t input_trace_id) {
  if (!address || address > UINT32_MAX - 4 || (address & 3) ||
      arguments.size() > 8 || !state_->domain->active()) {
    return false;
  }
  const auto caller = state_->kernel->ResolveNativeImportCaller(address + 4);
  if (!caller.scoped || caller.domain != state_->domain || !caller.module) {
    return false;
  }
  auto operation = state_->domain->Acquire();
  auto lease = state_->kernel->AcquireUserModuleExecutionLease(caller.module);
  if (!operation || !lease) {
    return false;
  }
  auto work = std::make_unique<State::Work>();
  work->address = address;
  work->input_trace_id = input_trace_id;
  work->module = caller.module;
  work->lease = std::move(lease);
  work->operation = std::move(operation);
  work->arguments.reserve(arguments.size());
  for (const auto argument : arguments) {
    work->arguments.push_back(cpu::GuestCallArgument::UInt32(argument));
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->domain->active()) {
      return false;
    }
    // Queue publication owns the completion only once enqueue succeeds.
    state_->queue.push_back(std::move(work));
    auto& published = state_->queue.back();
    published->completed = std::move(completed);
    published->before_enter = std::move(before_enter);
  }
  state_->ready.notify_one();
  return true;
}

bool NativeSystemCallbackDispatcher::IsAccepting() const {
  std::lock_guard lock(state_->mutex);
  return !state_->stopping && state_->domain->active();
}

bool NativeSystemCallbackDispatcher::IsCurrentWorker() const {
  // The actual host-thread identity is immutable after Create publishes this
  // dispatcher, including after it has joined. No guest TLS is manufactured.
  return state_->thread && XThread::IsInThread(state_->thread.get());
}

void NativeSystemCallbackDispatcher::BeginStop() {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
  }
  state_->ready.notify_all();
}

bool NativeSystemCallbackDispatcher::DrainAndJoin() {
  BeginStop();
  if (!state_->thread) {
    return true;
  }
  if (XThread::IsInThread(state_->thread.get())) {
    return false;
  }
  return XSUCCEEDED(state_->thread->Wait(0, 0, false, nullptr));
}

const std::shared_ptr<NativeImportDomain>&
NativeSystemCallbackDispatcher::domain() const {
  return state_->domain;
}

void NativeSystemCallbackDispatcher::BeginStopForKernel(KernelState* kernel) {
  for (const auto& dispatcher : ForKernel(kernel)) {
    dispatcher->BeginStop();
  }
}
bool NativeSystemCallbackDispatcher::DrainAndJoinForKernel(
    KernelState* kernel) {
  const auto dispatchers = ForKernel(kernel);
  for (const auto& dispatcher : dispatchers) {
    dispatcher->BeginStop();
  }
  for (const auto& dispatcher : dispatchers) {
    if (!dispatcher->DrainAndJoin()) {
      return false;
    }
  }
  return true;
}
}  // namespace xe::kernel
