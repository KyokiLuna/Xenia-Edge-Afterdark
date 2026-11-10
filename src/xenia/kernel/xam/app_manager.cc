/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/app_manager.h"
#include <algorithm>
#include <cstring>
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/apps/messenger_app.h"
#include "xenia/kernel/xam/apps/xam_app.h"
#include "xenia/kernel/xam/tile_request.h"
#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xam/apps/xlivebase_app.h"
#include "xenia/kernel/xam/apps/xmp_app.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace xam {

GuestAppRegistry::Registration::Registration(uint32_t app_id,
                                             uint32_t owner_hmodule,
                                             uint32_t callback_address,
                                             Callback callback,
                                             std::shared_ptr<void> owner_pin)
    : app_id_(app_id),
      owner_hmodule_(owner_hmodule),
      callback_address_(callback_address),
      callback_(std::move(callback)),
      owner_pin_(std::move(owner_pin)) {}

X_HRESULT GuestAppRegistry::Registration::Dispatch(uint32_t message,
                                                   uint32_t arg1,
                                                   uint32_t arg2) const {
  return Invoke(message, arg1, arg2, false);
}

X_HRESULT GuestAppRegistry::Registration::DispatchLifecycle(
    uint32_t message, uint32_t arg1, uint32_t arg2) const {
  return Invoke(message, arg1, arg2, true);
}

X_HRESULT GuestAppRegistry::Registration::Invoke(uint32_t message,
                                                 uint32_t arg1, uint32_t arg2,
                                                 bool lifecycle) const {
  const auto key = CurrentInvocationKey();
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    if (retired_.load() || execution_failed() || !callback_address_ ||
        !callback_ || (!lifecycle && (!normal_calls_enabled_ || stopping_))) {
      return X_E_FAIL;
    }
    ++active_calls_;
    if (key.fiber) {
      ++active_fibers_[key.fiber];
    } else {
      ++active_hosts_[key.host];
    }
  }
  Invocation invocation{this, key};
  // No registry lock is held across guest execution. The snapshot pins its
  // module, including when this callback unregisters itself or unloads its DLL.
  try {
    const auto result = callback_(message, arg1, arg2);
    if (!result.executed) {
      execution_failed_.store(true, std::memory_order_release);
      return X_E_FAIL;
    }
    return result.result;
  } catch (...) {
    execution_failed_.store(true, std::memory_order_release);
    throw;
  }
}

GuestAppRegistry::Registration::InvocationKey
GuestAppRegistry::Registration::CurrentInvocationKey() {
  if (auto* fiber = XThread::GetCurrentFiberThread()) {
    return {fiber, {}};
  }
  return {nullptr, std::this_thread::get_id()};
}

bool GuestAppRegistry::Registration::IsCallingLocked(
    const InvocationKey& key) const {
  return key.fiber ? active_fibers_.contains(key.fiber)
                   : active_hosts_.contains(key.host);
}

void GuestAppRegistry::Registration::EndCall(const InvocationKey& key) const {
  std::lock_guard<std::mutex> lock(call_mutex_);
  if (key.fiber) {
    auto it = active_fibers_.find(key.fiber);
    if (--it->second == 0) {
      active_fibers_.erase(it);
    }
  } else {
    auto it = active_hosts_.find(key.host);
    if (--it->second == 0) {
      active_hosts_.erase(it);
    }
  }
  if (--active_calls_ == 0) {
    calls_drained_.notify_all();
  }
}

bool GuestAppRegistry::Registration::EnableNormalCalls() const {
  std::lock_guard<std::mutex> lock(call_mutex_);
  if (retired_.load() || execution_failed() || stopping_) {
    return false;
  }
  normal_calls_enabled_ = true;
  return true;
}

bool GuestAppRegistry::Registration::StopAndWait() const {
  std::unique_lock<std::mutex> lock(call_mutex_);
  stopping_ = true;
  if (IsCallingLocked(CurrentInvocationKey())) {
    return false;
  }
  // Blocking a scheduler fiber can also park the CPU hosting another active
  // callback. The loader must retry this drain from a native system thread.
  if (XThread::GetCurrentFiberThread() && active_calls_) {
    return false;
  }
  calls_drained_.wait(lock, [this] { return active_calls_ == 0; });
  return true;
}

void GuestAppRegistry::Registration::StopNormalCalls() const {
  std::lock_guard<std::mutex> lock(call_mutex_);
  stopping_ = true;
}

bool GuestAppRegistry::Registration::IsCallingOnCurrentThread() const {
  std::lock_guard<std::mutex> lock(call_mutex_);
  return IsCallingLocked(CurrentInvocationKey());
}

GuestAppRegistry::GuestAppRegistry() {
  for (uint32_t id = 0xEF; id <= 0xFE; ++id) {
    descriptors_.try_emplace(id);
  }
}

GuestAppRegistry::~GuestAppRegistry() { Clear(); }

bool GuestAppRegistry::Prepare(uint32_t app_id, uint32_t hmodule,
                               std::shared_ptr<void> owner_pin) {
  if (app_id < 0xEF || app_id > 0xFF || !hmodule || !owner_pin) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return false;
  }
  auto& descriptor = descriptors_[app_id];
  if (descriptor.hmodule || descriptor.registration) {
    return false;
  }
  descriptor.hmodule = hmodule;
  descriptor.owner_pin = std::move(owner_pin);
  return true;
}

X_RESULT GuestAppRegistry::Register(uint32_t hmodule, uint32_t app_id,
                                    uint32_t callback_address,
                                    uint32_t resource_slot, Callback callback,
                                    std::shared_ptr<void> owner_pin) {
  std::shared_ptr<Registration> retired;
  ResourceBinding previous_resource;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return X_ERROR_FUNCTION_FAILED;
    }
    // The console updates resource bindings even when registration then fails.
    if (resource_slot < resource_modules_.size()) {
      previous_resource = std::move(resource_modules_[resource_slot]);
      resource_modules_[resource_slot] = {hmodule, owner_pin};
    }
    auto it = descriptors_.find(app_id);
    if (it == descriptors_.end() || it->second.stopping ||
        (it->second.registration &&
         it->second.registration->callback_address())) {
      return X_ERROR_FUNCTION_FAILED;
    }
    retired = std::move(it->second.registration);
    if (retired) {
      retired->retired_ = true;
    }
    it->second.registration = std::shared_ptr<Registration>(
        new Registration(app_id, hmodule, callback_address, std::move(callback),
                         std::move(owner_pin)));
    if (it->second.initialized) {
      it->second.registration->EnableNormalCalls();
    }
    std::erase_if(registrations_,
                  [](const auto& weak) { return weak.expired(); });
    registrations_.push_back(it->second.registration);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT GuestAppRegistry::Unregister(uint32_t hmodule, uint32_t app_id,
                                      uint32_t resource_slot) {
  std::shared_ptr<Registration> retired;
  ResourceBinding previous_resource;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (resource_slot < resource_modules_.size()) {
      previous_resource = std::move(resource_modules_[resource_slot]);
      resource_modules_[resource_slot] = {};
    }
    auto it = descriptors_.find(app_id);
    if (closed_ || it == descriptors_.end() || !it->second.hmodule ||
        !it->second.initialized) {
      return X_ERROR_FUNCTION_FAILED;
    }
    if (it->second.hmodule == hmodule) {
      retired = std::move(it->second.registration);
      if (retired) {
        retired->retired_ = true;
      }
    }
  }
  return X_ERROR_SUCCESS;
}

GuestAppRegistry::Snapshot GuestAppRegistry::Lookup(uint32_t app_id,
                                                    bool lifecycle) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = descriptors_.find(app_id);
  if (closed_ || it == descriptors_.end() ||
      (!lifecycle &&
       (!it->second.hmodule || !it->second.initialized || it->second.stopping ||
        (it->second.registration &&
         it->second.registration->execution_failed())))) {
    return nullptr;
  }
  return it->second.registration;
}

X_HRESULT GuestAppRegistry::Dispatch(uint32_t app_id, uint32_t message,
                                     uint32_t arg1, uint32_t arg2) const {
  Snapshot registration;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = descriptors_.find(app_id);
    if (closed_ || it == descriptors_.end() || !it->second.hmodule ||
        !it->second.initialized || it->second.stopping) {
      return X_E_INVALIDARG;
    }
    registration = it->second.registration;
  }
  return registration ? registration->Dispatch(message, arg1, arg2) : X_E_FAIL;
}

bool GuestAppRegistry::Activate(const Snapshot& registration) {
  if (!registration || registration->retired_.load() ||
      !registration->callback_address()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = descriptors_.find(registration->app_id());
  if (closed_ || it == descriptors_.end() || !it->second.hmodule ||
      it->second.stopping || it->second.registration != registration ||
      !registration->EnableNormalCalls()) {
    return false;
  }
  it->second.initialized = true;
  return true;
}

bool GuestAppRegistry::StopNormalAdmission(const Snapshot& registration) {
  if (!registration) {
    return false;
  }
  std::vector<std::shared_ptr<Registration>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = descriptors_.find(registration->app_id());
    if (closed_ || it == descriptors_.end() ||
        it->second.registration != registration) {
      return false;
    }
    it->second.stopping = true;
    for (const auto& weak : registrations_) {
      if (auto generation = weak.lock();
          generation && generation->app_id() == registration->app_id()) {
        live.push_back(std::move(generation));
      }
    }
  }
  for (const auto& generation : live) {
    generation->StopNormalCalls();
  }
  return true;
}

bool GuestAppRegistry::BeginShutdown(const Snapshot& registration) {
  if (!registration) {
    return false;
  }
  std::vector<std::shared_ptr<Registration>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = descriptors_.find(registration->app_id());
    if (closed_ || it == descriptors_.end() ||
        it->second.registration != registration) {
      return false;
    }
    it->second.initialized = false;
    it->second.stopping = true;
    for (const auto& weak : registrations_) {
      if (auto generation = weak.lock()) {
        live.push_back(std::move(generation));
      }
    }
  }
  for (const auto& generation : live) {
    if (generation->app_id() == registration->app_id()) {
      generation->StopNormalCalls();
    }
  }
  for (const auto& generation : live) {
    if (generation->app_id() == registration->app_id() &&
        generation->IsCallingOnCurrentThread()) {
      return false;
    }
  }
  bool drained = true;
  for (const auto& generation : live) {
    if (generation->app_id() == registration->app_id()) {
      drained &= generation->StopAndWait();
    }
  }
  return drained && !registration->retired_.load();
}

bool GuestAppRegistry::BeginModuleShutdown(uint32_t hmodule) {
  if (!hmodule) {
    return true;
  }
  std::vector<std::shared_ptr<Registration>> live;
  std::vector<uint32_t> owned_ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, descriptor] : descriptors_) {
      if (descriptor.hmodule == hmodule ||
          (descriptor.registration &&
           descriptor.registration->owner_hmodule() == hmodule)) {
        descriptor.initialized = false;
        descriptor.stopping = true;
        owned_ids.push_back(id);
      }
    }
    for (const auto& weak : registrations_) {
      if (auto generation = weak.lock()) {
        live.push_back(std::move(generation));
      }
    }
  }
  // An unload requested from a callback must be deferred, not waited on from
  // that callback (nor while the kernel's global critical region is held).
  auto owns_generation = [&](const auto& registration) {
    return registration->owner_hmodule() == hmodule ||
           std::ranges::find(owned_ids, registration->app_id()) !=
               owned_ids.end();
  };
  for (const auto& registration : live) {
    if (owns_generation(registration)) {
      registration->StopNormalCalls();
    }
  }
  for (const auto& registration : live) {
    if (owns_generation(registration) &&
        registration->IsCallingOnCurrentThread()) {
      return false;
    }
  }
  bool drained = true;
  for (const auto& registration : live) {
    if (owns_generation(registration)) {
      drained &= registration->StopAndWait();
    }
  }
  return drained;
}

bool GuestAppRegistry::HasDescriptor(uint32_t app_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return descriptors_.contains(app_id);
}

uint32_t GuestAppRegistry::ResourceModule(uint32_t slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return slot < resource_modules_.size() ? resource_modules_[slot].hmodule : 0;
}

void GuestAppRegistry::RetireModule(uint32_t hmodule) {
  if (!hmodule) {
    return;
  }
  std::vector<Descriptor> retired;
  std::vector<ResourceBinding> retired_resources;
  std::vector<std::shared_ptr<Registration>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, descriptor] : descriptors_) {
      if (descriptor.hmodule == hmodule ||
          (descriptor.registration &&
           descriptor.registration->owner_hmodule() == hmodule)) {
        if (descriptor.registration) {
          descriptor.registration->retired_ = true;
        }
        retired.push_back(std::move(descriptor));
        descriptor = {};
      }
    }
    for (auto& binding : resource_modules_) {
      if (binding.hmodule == hmodule) {
        retired_resources.push_back(std::move(binding));
        binding = {};
      }
    }
    for (const auto& weak : registrations_) {
      if (auto generation = weak.lock()) {
        if (generation->owner_hmodule() == hmodule) {
          generation->retired_ = true;
        }
        live.push_back(std::move(generation));
      }
    }
  }
  // Last module references can enter loader teardown; release outside mutex_.
}

void GuestAppRegistry::Clear() {
  decltype(descriptors_) retired;
  decltype(resource_modules_) retired_resources;
  std::vector<std::shared_ptr<Registration>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    for (auto& [id, descriptor] : descriptors_) {
      if (descriptor.registration) {
        descriptor.registration->retired_ = true;
      }
    }
    for (const auto& weak : registrations_) {
      if (auto generation = weak.lock()) {
        generation->retired_ = true;
        live.push_back(std::move(generation));
      }
    }
    registrations_.clear();
    retired.swap(descriptors_);
    retired_resources.swap(resource_modules_);
  }
}

bool AppManager::PrepareGuestApp(uint32_t app_id,
                                 const object_ref<UserModule>& module) {
  return module &&
         guest_apps_.Prepare(app_id, module->hmodule_ptr(),
                             std::make_shared<object_ref<UserModule>>(module));
}

X_RESULT AppManager::RegisterGuestApp(KernelState* kernel_state,
                                      const object_ref<UserModule>& module,
                                      uint32_t app_id,
                                      uint32_t callback_address,
                                      uint32_t resource_slot) {
  if (!module) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return guest_apps_.Register(
      module->hmodule_ptr(), app_id, callback_address, resource_slot,
      [kernel_state, callback_address](
          uint32_t message, uint32_t arg1,
          uint32_t arg2) -> GuestAppRegistry::ExecutionResult {
        if (!XThread::IsInThread()) {
          return {false, X_E_FAIL};
        }
        auto* thread = XThread::GetCurrentThread();
        if (!thread || !thread->thread_state()) {
          return {false, X_E_FAIL};
        }
        auto* state = thread->thread_state();
        state->context()->r[3] = message;
        state->context()->r[4] = arg1;
        state->context()->r[5] = arg2;
        const bool executed =
            kernel_state->processor()->Execute(state, callback_address);
        return {executed, static_cast<uint32_t>(state->context()->r[3])};
      },
      std::make_shared<object_ref<UserModule>>(module));
}

App::App(KernelState* kernel_state, uint32_t app_id)
    : kernel_state_(kernel_state),
      memory_(kernel_state->memory()),
      app_id_(app_id) {}

void AppManager::RegisterApps(KernelState* kernel_state, AppManager* manager) {
  manager->RegisterApp(std::make_unique<apps::MessengerApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XmpApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XgiApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XLiveBaseApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XamApp>(kernel_state));
}

void AppManager::RegisterApp(std::unique_ptr<App> app) {
  assert_zero(app_lookup_.count(app->app_id()));
  app_lookup_.insert({app->app_id(), app.get()});
  apps_.push_back(std::move(app));
}

X_HRESULT AppManager::DispatchMessageSync(uint32_t app_id, uint32_t message,
                                          uint32_t buffer_ptr,
                                          uint32_t buffer_length) {
  if (auto guest = guest_apps_.Lookup(app_id)) {
    return guest->Dispatch(message, buffer_ptr, buffer_length);
  }
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return guest_apps_.Dispatch(app_id, message, buffer_ptr, buffer_length);
  }
  return it->second->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

X_HRESULT AppManager::DispatchMessageAsync(uint32_t app_id, uint32_t message,
                                           uint32_t buffer_ptr,
                                           uint32_t buffer_length,
                                           uint32_t overlapped_ptr) {
  if (guest_apps_.Lookup(app_id)) {
    // System-process calls must run under a system PCR with an owned request
    // buffer/task context. Neither a title caller nor the builtin overlapped
    // completion wrapper provides that guest SysApp dispatch contract yet.
    return X_E_NOT_IMPLEMENTED;
  }
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return X_E_NOTFOUND;
  }
  App* app = it->second;
  if (!overlapped_ptr) {
    return app->DispatchMessageSync(message, buffer_ptr, buffer_length);
  }

  if (app_id == 0xFB && message == 0xB0008) {
    X_HRESULT captured_status;
    auto owned = apps::XgiApp::CaptureAchievementBatch(
        *kernel_state(), buffer_ptr, buffer_length, captured_status);
    if (!owned) {
      return captured_status;
    }
    // Deep nested capture occurs on the actual caller before it can release
    // buffers or change slot assignment. Existing kernel owns cancel/drain.
    auto* kernel = kernel_state();
    kernel->CompleteOverlappedDeferredEx(
        [kernel, owned](uint32_t& extended_error,
                        uint32_t& length) -> X_RESULT {
          const auto result =
              apps::XgiApp::ExecuteAchievementBatch(*kernel, *owned);
          extended_error = result;
          length = result ? 0xFFFFFFFF : 0;
          return result;
        },
        overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }

  if (app_id == 0xFB && IsTileMessage(message)) {
    X_HRESULT status;
    auto request = CaptureTileMessage(*kernel_state(), message, buffer_ptr,
                                      buffer_length, overlapped_ptr, status);
    if (!request) { return status; }
    auto* kernel = kernel_state();
    kernel->CompleteOverlappedDeferredEx(
        [kernel, request](uint32_t& error, uint32_t& length) {
          return CompleteTileRequest(*kernel, *request, error, length);
        }, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }

  // A title can depend on the request staying pending for a while after this
  // returns: one registers its in-game message handler only in a frame that
  // sees the session it hosts still being created. The delay before deferred
  // work runs provides that. The caller's buffer may be gone by then.
  // TODO(has207): Give XMsg requests their own worker. The kernel dispatch
  // thread also runs DPCs and XAM dialogs, which hold it until they close.
  Memory* memory = kernel_memory();
  uint32_t copy_ptr = 0;
  if (buffer_ptr) {
    // A zero-length request still gets a valid pointer.
    copy_ptr = memory->SystemHeapAlloc(std::max(buffer_length, 1u));
    if (!copy_ptr) {
      return X_E_FAIL;
    }
    std::memcpy(memory->TranslateVirtual(copy_ptr),
                memory->TranslateVirtual(buffer_ptr), buffer_length);
  }
  kernel_state()->CompleteOverlappedDeferredEx(
      [app, message, copy_ptr, buffer_length](uint32_t& extended_error,
                                              uint32_t& length) -> X_RESULT {
        const X_HRESULT result =
            app->DispatchMessageSync(message, copy_ptr, buffer_length);
        extended_error = result;
        // Some titles take a nonzero length as failure.
        length = result ? 0xFFFFFFFF : 0;
        return result;
      },
      overlapped_ptr, nullptr,
      [memory, copy_ptr]() {
        if (copy_ptr) {
          memory->SystemHeapFree(copy_ptr);
        }
      });
  return X_ERROR_IO_PENDING;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
