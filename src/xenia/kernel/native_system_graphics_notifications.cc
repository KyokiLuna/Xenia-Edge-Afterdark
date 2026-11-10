/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_graphics_notifications.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_callback_dispatcher.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {
std::mutex installations_mutex;
std::map<KernelState*, std::weak_ptr<NativeSystemGraphicsNotifications>>
    installations;
constexpr uint32_t kReceiver = 0x816CBFD8;
constexpr uint32_t kReceiverBytes = 1308;
constexpr std::array<uint8_t, 32> kReceiverSha256 = {
    0xB9, 0x23, 0x56, 0x3F, 0x0B, 0x2D, 0xB6, 0xFA, 0x49, 0x0C, 0xA3,
    0xBE, 0x11, 0x63, 0x05, 0x32, 0xA4, 0x39, 0x17, 0xD0, 0xB0, 0x78,
    0x03, 0xB9, 0x77, 0x84, 0xB1, 0xD4, 0xE4, 0x1E, 0x01, 0xE5};
uint32_t PayloadBytes(uint32_t kind) {
  switch (kind) {
    case 1:
      return 0x40;
    case 3:
      return 0x58;
    case 9:
      return 8;
    default:
      return 0;
  }
}
[[noreturn]] void FailStop() noexcept {
  try {
    XELOGE(
        "Native graphics notifications: callback/caller stop unproven; "
        "retaining providers until process exit");
    xe::FlushLog();
  } catch (...) {
  }
  std::quick_exit(EXIT_FAILURE);
}
}  // namespace

struct NativeSystemGraphicsNotifications::State {
  struct Request {
    std::mutex mutex;
    uint32_t payload = 0;
    bool completed = false, executed = false;
  };
  struct Operation {
    std::shared_ptr<State> owner;
    Config config;
    KernelState* kernel = nullptr;
    ~Operation() {
      if (!owner) {
        return;
      }
      // Release code/domain/provider references while this admitted operation
      // still keeps Kernel retirement closed, outside the state lock.
      config = {};
      std::lock_guard lock(owner->mutex);
      --owner->active_calls;
      owner->drained.notify_all();
    }
  };
  Config config;
  KernelState* kernel = nullptr;
  mutable std::mutex mutex;
  std::condition_variable drained;
  bool stopping = false, retired = false;
  uint64_t registration = 1;
  uint32_t callback = 0;
  size_t active_calls = 0;
  // Published before enqueue/native entry. Unknown callback failures keep the
  // payload until stopped Kernel memory reclamation, never a PPC destructor.
  std::vector<std::shared_ptr<Request>> requests;

  std::unique_ptr<Operation> Acquire(const std::shared_ptr<State>& self) {
    auto operation = std::make_unique<Operation>();
    std::lock_guard lock(mutex);
    if (stopping || retired || !kernel || active_calls == SIZE_MAX) {
      return {};
    }
    operation->owner = self;
    operation->config = config;
    operation->kernel = kernel;
    ++active_calls;
    return operation;
  }
};

NativeSystemGraphicsNotifications::NativeSystemGraphicsNotifications(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemGraphicsNotifications::~NativeSystemGraphicsNotifications() =
    default;

std::shared_ptr<NativeSystemGraphicsNotifications>
NativeSystemGraphicsNotifications::Create(Config config) {
  if (!config.catalog || !config.domain || !config.domain->active() ||
      !config.catalog->allocation()->IsAccepting() ||
      !config.catalog->allocation()->MatchesDomain(config.domain) ||
      !config.dispatcher || config.dispatcher->domain() != config.domain ||
      !config.dispatcher->IsAccepting() ||
      config.domain->root() !=
          NativeImportDomain::Member{
              config.catalog->calls()->module()->hmodule_ptr(),
              config.catalog->calls()->load_generation()}) {
    return {};
  }
  auto state = std::make_shared<State>();
  state->kernel = config.catalog->calls()->kernel();
  {
    std::lock_guard lock(installations_mutex);
    if (!installations[state->kernel].expired()) {
      return {};
    }
  }
  state->config = std::move(config);
  auto result = std::shared_ptr<NativeSystemGraphicsNotifications>(
      new NativeSystemGraphicsNotifications(state));
  struct KernelRetirement {
    explicit KernelRetirement(
        std::shared_ptr<NativeSystemGraphicsNotifications> value)
        : owner(std::move(value)) {}
    std::shared_ptr<NativeSystemGraphicsNotifications> owner;
    ~KernelRetirement() { owner->RetireAfterKernelStop(); }
  };
  state->kernel->RetainNativeResourceUntilTeardown(
      std::make_shared<KernelRetirement>(result));
  {
    std::lock_guard lock(installations_mutex);
    if (!installations[state->kernel].expired()) {
      result->BeginStop();
      return {};
    }
    installations[state->kernel] = result;
  }
  return result;
}

std::shared_ptr<NativeSystemGraphicsNotifications>
NativeSystemGraphicsNotifications::Lookup(KernelState* kernel) {
  std::lock_guard lock(installations_mutex);
  const auto found = installations.find(kernel);
  return found == installations.end() ? nullptr : found->second.lock();
}

bool NativeSystemGraphicsNotifications::Register(uint32_t callback,
                                                 uint32_t return_address) {
  auto operation = state_->Acquire(state_);
  if (!operation || !XThread::IsInThread()) {
    return false;
  }
  auto* current = XThread::GetCurrentThread();
  const auto& config = operation->config;
  if (current->kernel_state() != operation->kernel ||
      current->native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM) {
    return false;
  }
  const auto caller =
      operation->kernel->ResolveNativeImportCaller(return_address);
  if (!caller.scoped || caller.domain != config.domain ||
      caller.module.get() != config.catalog->calls()->module().get() ||
      !config.domain->active()) {
    return false;
  }
  if (callback) {
    auto* memory = operation->kernel->memory();
    if (callback != kReceiver ||
        memory->QueryRangeAccess(callback, kReceiverBytes) ==
            memory::PageAccess::kNoAccess) {
      return false;
    }
    auto* image = config.catalog->calls()->module()->xex_module();
    for (uint32_t offset = 0; offset < kReceiverBytes; offset += 4) {
      if (image->TranslateCode(callback + offset) !=
          memory->TranslateVirtual<const uint8_t*>(callback + offset)) {
        return false;
      }
    }
    std::array<uint8_t, 32> digest;
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(callback),
                   kReceiverBytes, digest.data(), 0);
    if (digest != kReceiverSha256) {
      return false;
    }
  }
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || state_->registration == UINT64_MAX) {
    return false;
  }
  ++state_->registration;
  state_->callback = callback;
  return true;
}

bool NativeSystemGraphicsNotifications::IsRegistered() const {
  std::lock_guard lock(state_->mutex);
  return !state_->stopping && !state_->retired && state_->callback == kReceiver;
}

bool NativeSystemGraphicsNotifications::Call(uint32_t kind, uint32_t payload,
                                             uint32_t return_address) {
  auto operation = state_->Acquire(state_);
  const uint32_t bytes = PayloadBytes(kind);
  if (!operation || !bytes || !payload ||
      uint64_t(payload) + bytes > (uint64_t(1) << 32) ||
      !XThread::IsInThread()) {
    return false;
  }
  auto* caller = XThread::GetCurrentThread();
  auto* kernel = operation->kernel;
  const auto& config = operation->config;
  const auto process = caller->native_frame_thread_identity().process_type;
  if (caller->kernel_state() != kernel ||
      (process != X_PROCTYPE_TITLE && process != X_PROCTYPE_SYSTEM) ||
      !kernel->GetUserModuleForAddress(return_address) ||
      !config.domain->active() || config.dispatcher->IsCurrentWorker()) {
    // The known receiver's copy-only variants do not recursively notify. A
    // new recursive producer requires its own proven context/ordering contract.
    return false;
  }
  uint32_t callback;
  uint64_t registration;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->callback) {
      return false;
    }
    callback = state_->callback;
    registration = state_->registration;
  }
  auto request = std::make_shared<State::Request>();
  auto* memory = kernel->memory();
  {
    auto memory_lock = global_critical_region::Acquire();
    if (memory->QueryRangeAccess(payload, bytes) ==
        memory::PageAccess::kNoAccess) {
      return false;
    }
    request->payload = memory->SystemHeapAlloc(bytes);
    if (!request->payload) {
      return false;
    }
    std::memcpy(memory->TranslateVirtual(request->payload),
                memory->TranslateVirtual(payload), bytes);
  }
  try {
    bool stopping;
    {
      std::lock_guard lock(state_->mutex);
      stopping = state_->stopping;
      if (!stopping) {
        state_->requests.push_back(request);
      }
    }
    if (stopping) {
      memory->SystemHeapFree(request->payload);
      return false;
    }
  } catch (...) {
    memory->SystemHeapFree(request->payload);
    throw;
  }
  const auto complete = [request](bool executed, uint32_t) {
    std::lock_guard lock(request->mutex);
    request->executed = executed;
    request->completed = true;
  };
  const std::array<uint32_t, 3> args{0, kind, request->payload};
  const bool queued = config.dispatcher->Enqueue(
      callback, args, complete,
      [state = state_, callback, registration]() -> std::shared_ptr<void> {
        std::lock_guard lock(state->mutex);
        if (state->stopping || state->callback != callback ||
            state->registration != registration) {
          return {};
        }
        // Dispatcher owns the exact code/domain execution reservation through
        // completion. This generation witness suppresses replaced queued calls.
        return state;
      });
  if (!queued) {
    memory->SystemHeapFree(request->payload);
    std::lock_guard lock(state_->mutex);
    std::erase(state_->requests, request);
    return false;
  }
  bool executed;
  for (;;) {
    bool completed;
    {
      std::lock_guard lock(request->mutex);
      completed = request->completed;
      executed = request->executed;
    }
    if (completed) {
      break;
    }
    caller->Delay(0, 0, uint64_t(-10000ll));
  }
  if (executed) {
    // The exact receiver copies these payloads and retains no caller pointer.
    memory->SystemHeapFree(request->payload);
    std::lock_guard lock(state_->mutex);
    std::erase(state_->requests, request);
  } else {
    // A partial receiver may have changed native state. Keep the captured
    // payload and reject further ingress until the stopped Kernel retires it.
    BeginStop();
  }
  return executed;
}

void NativeSystemGraphicsNotifications::BeginStop() noexcept {
  std::lock_guard lock(state_->mutex);
  state_->stopping = true;
}
bool NativeSystemGraphicsNotifications::Drain() noexcept {
  if (XThread::IsInThread()) {
    return false;
  }
  BeginStop();
  std::unique_lock lock(state_->mutex);
  return state_->drained.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return !state_->active_calls; });
}
void NativeSystemGraphicsNotifications::RetireAfterKernelStop() noexcept {
  if (!Drain()) {
    FailStop();
  }
  Config retired_config;
  KernelState* retired_kernel;
  std::vector<std::shared_ptr<State::Request>> retired_requests;
  {
    std::lock_guard lock(state_->mutex);
    state_->retired = true;
    retired_config = std::move(state_->config);
    retired_requests.swap(state_->requests);
    retired_kernel = state_->kernel;
    state_->kernel = nullptr;
    state_->callback = 0;
  }
  {
    std::lock_guard lock(installations_mutex);
    const auto found = installations.find(retired_kernel);
    if (found != installations.end() && found->second.lock().get() == this) {
      installations.erase(found);
    }
  }
  // No native release/free/PPC here. Unknown guest payloads are reclaimed by
  // the stopped Kernel memory; code/provider holders drop while Kernel lives.
}
}  // namespace xe::kernel
