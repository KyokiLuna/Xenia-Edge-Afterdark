/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APP_MANAGER_H_
#define XENIA_KERNEL_XAM_APP_MANAGER_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
class UserModule;
class XThread;
template <typename T>
class object_ref;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

class App {
 public:
  uint32_t app_id() const { return app_id_; }

  virtual X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                        uint32_t buffer_length) = 0;

  virtual ~App() = default;

 protected:
  App(KernelState* kernel_state, uint32_t app_id);

  KernelState* kernel_state_;
  Memory* memory_;
  uint32_t app_id_;
};

// Guest system apps keep their own registration generation. A request already
// holding one can never accidentally call a replacement registered at that ID.
class GuestAppRegistry {
 public:
  struct ExecutionResult {
    bool executed;
    X_HRESULT result;
    // Synthetic/host callbacks returning an HRESULT completed normally.
    ExecutionResult(X_HRESULT value) : executed(true), result(value) {}
    ExecutionResult(bool completed, X_HRESULT value)
        : executed(completed), result(value) {}
  };
  using Callback = std::function<ExecutionResult(uint32_t message,
                                                 uint32_t arg1, uint32_t arg2)>;

  class Registration {
   public:
    uint32_t app_id() const { return app_id_; }
    uint32_t owner_hmodule() const { return owner_hmodule_; }
    uint32_t callback_address() const { return callback_address_; }
    bool execution_failed() const {
      return execution_failed_.load(std::memory_order_acquire);
    }
    X_HRESULT Dispatch(uint32_t message, uint32_t arg1, uint32_t arg2) const;
    X_HRESULT DispatchLifecycle(uint32_t message, uint32_t arg1,
                                uint32_t arg2) const;

   private:
    friend class GuestAppRegistry;
    Registration(uint32_t app_id, uint32_t owner_hmodule,
                 uint32_t callback_address, Callback callback,
                 std::shared_ptr<void> owner_pin);
    X_HRESULT Invoke(uint32_t message, uint32_t arg1, uint32_t arg2,
                     bool lifecycle) const;
    bool EnableNormalCalls() const;
    void StopNormalCalls() const;
    bool StopAndWait() const;
    bool IsCallingOnCurrentThread() const;
    struct InvocationKey {
      const XThread* fiber = nullptr;
      std::thread::id host;
    };
    static InvocationKey CurrentInvocationKey();
    bool IsCallingLocked(const InvocationKey& key) const;
    void EndCall(const InvocationKey& key) const;
    struct Invocation {
      const Registration* registration;
      InvocationKey key;
      ~Invocation() { registration->EndCall(key); }
    };
    uint32_t app_id_;
    uint32_t owner_hmodule_;
    uint32_t callback_address_;
    Callback callback_;
    std::shared_ptr<void> owner_pin_;
    std::atomic<bool> retired_{false};
    mutable std::atomic<bool> execution_failed_{false};
    mutable std::mutex call_mutex_;
    mutable std::condition_variable calls_drained_;
    mutable bool normal_calls_enabled_ = false;
    mutable bool stopping_ = false;
    mutable size_t active_calls_ = 0;
    mutable std::unordered_map<const XThread*, size_t> active_fibers_;
    mutable std::unordered_map<std::thread::id, size_t> active_hosts_;
  };
  using Snapshot = std::shared_ptr<const Registration>;

  GuestAppRegistry();
  ~GuestAppRegistry();
  // Called by the system-app loader before DLL_PROCESS_ATTACH. FF is the
  // dynamically created HUD descriptor; EF..FE are the verified static range.
  bool Prepare(uint32_t app_id, uint32_t hmodule,
               std::shared_ptr<void> owner_pin);
  X_RESULT Register(uint32_t hmodule, uint32_t app_id,
                    uint32_t callback_address, uint32_t resource_slot,
                    Callback callback, std::shared_ptr<void> owner_pin);
  X_RESULT Unregister(uint32_t hmodule, uint32_t app_id,
                      uint32_t resource_slot);
  // Lifecycle dispatch is allowed before normal XMsg routing is enabled.
  Snapshot Lookup(uint32_t app_id, bool lifecycle = false) const;
  X_HRESULT Dispatch(uint32_t app_id, uint32_t message, uint32_t arg1,
                     uint32_t arg2) const;
  bool Activate(const Snapshot& registration);
  // After the close notification, stop normal admission and drain existing
  // calls before sending the destructive lifecycle callback.
  // False means a callback or scheduler fiber must defer and retry the drain
  // from a native system thread.
  bool BeginShutdown(const Snapshot& registration);
  bool StopNormalAdmission(const Snapshot& registration);
  bool BeginModuleShutdown(uint32_t hmodule);
  bool HasDescriptor(uint32_t app_id) const;
  uint32_t ResourceModule(uint32_t slot) const;
  void RetireModule(uint32_t hmodule);
  // Close the registry to new loads and invalidate every queued snapshot.
  void Clear();

 private:
  struct Descriptor {
    uint32_t hmodule = 0;
    bool initialized = false;
    bool stopping = false;
    std::shared_ptr<void> owner_pin;
    std::shared_ptr<Registration> registration;
  };
  struct ResourceBinding {
    uint32_t hmodule = 0;
    std::shared_ptr<void> owner_pin;
  };
  mutable std::mutex mutex_;
  bool closed_ = false;
  std::unordered_map<uint32_t, Descriptor> descriptors_;
  std::vector<std::weak_ptr<Registration>> registrations_;
  std::array<ResourceBinding, 64> resource_modules_{};
};

class AppManager {
 public:
  static void RegisterApps(KernelState* kernel_state, AppManager* manager);

  void RegisterApp(std::unique_ptr<App> app);

  bool PrepareGuestApp(uint32_t app_id, const object_ref<UserModule>& module);
  X_RESULT RegisterGuestApp(KernelState* kernel_state,
                            const object_ref<UserModule>& module,
                            uint32_t app_id, uint32_t callback_address,
                            uint32_t resource_slot);
  GuestAppRegistry& guest_apps() { return guest_apps_; }
  void RetireGuestAppsForModule(uint32_t hmodule) {
    guest_apps_.RetireModule(hmodule);
  }
  bool BeginGuestAppModuleShutdown(uint32_t hmodule) {
    return guest_apps_.BeginModuleShutdown(hmodule);
  }
  void ClearGuestApps() { guest_apps_.Clear(); }

  X_HRESULT DispatchMessageSync(uint32_t app_id, uint32_t message,
                                uint32_t buffer_ptr, uint32_t buffer_length);
  // With an overlapped the message runs later on a copy of the buffer and this
  // returns X_ERROR_IO_PENDING.
  X_HRESULT DispatchMessageAsync(uint32_t app_id, uint32_t message,
                                 uint32_t buffer_ptr, uint32_t buffer_length,
                                 uint32_t overlapped_ptr = 0);

 private:
  std::vector<std::unique_ptr<App>> apps_;
  std::unordered_map<uint32_t, App*> app_lookup_;
  GuestAppRegistry guest_apps_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APP_MANAGER_H_
