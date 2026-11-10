/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe::hid::test {
namespace {
class Context final : public ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
class Window final : public ui::Window {
 public:
  explicit Window(Context& context)
      : ui::Window(context, "native device notification", 1280, 720) {}
  ~Window() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override { return false; }
  void RequestCloseImpl() override {}
  std::unique_ptr<ui::Surface> CreateSurfaceImpl(
      ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};
class Controller final : public InputDriver {
 public:
  Controller() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  InputType GetInputType() const override { return InputType::Controller; }
  X_RESULT GetCapabilities(uint32_t slot, uint32_t,
                           X_INPUT_CAPABILITIES* out) override {
    if (!connected || slot != 0) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    out->type = XINPUT_DEVTYPE_GAMEPAD;
    out->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t slot, X_INPUT_STATE* out) override {
    if (!connected || slot != 0) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t, X_INPUT_VIBRATION*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }
  std::vector<InputDeviceInfo> EnumerateDevices() override {
    if (!connected) {
      return {};
    }
    InputDeviceInfo info{};
    info.driver_slot = 0;
    info.stable_id = "test-controller";
    info.display_name = "test controller";
    info.preferred_slot = 1;
    return {info};
  }
  void Hotplug(bool present) {
    connected = present;
    NotifyDevicesChanged();
  }
  bool connected = true;
};
}  // namespace

TEST_CASE(
    "Native device subscriber defers outside input mutation and preserves app "
    "subscriber",
    "[ui][input][native_devices]") {
  Context context;
  Window window(context);
  InputSystem input(&window);
  auto controller = std::make_unique<Controller>();
  auto* physical = controller.get();
  input.AddDriver(std::move(controller));
  unsigned app_calls = 0, native_calls = 0;
  bool inside_mutation = false, entered_during_mutation = false;
  uint8_t observed_subtype = 0;
  {
    auto lock = input.lock();
    input.SetBindingsChangedCallback([&] { ++app_calls; });
    REQUIRE(input.BeginNativeInputTransport(21, {}, [&] {
      ++native_calls;
      entered_during_mutation |= inside_mutation;
      // Avoid hanging if an incorrect implementation invokes inline.
      if (!inside_mutation) {
        auto callback_lock = input.lock();
        const auto& bindings = input.GetSlotBindings(1);
        observed_subtype = bindings.empty() ? 0 : bindings[0].subtype_override;
      }
    }));
    inside_mutation = true;
    input.BindSlot(1, physical, 0, "test-controller", "test controller");
    input.SetSubtypeOverride(physical, 0, 2);
    CHECK(app_calls == 2);
    CHECK(native_calls == 0);
    inside_mutation = false;
  }
  context.ExecutePendingFunctionsFromUIThread();
  CHECK_FALSE(entered_during_mutation);
  CHECK(native_calls == 1);
  CHECK(observed_subtype == 2);
  CHECK(app_calls == 2);
  {
    auto lock = input.lock();
    input.UnbindDevice(physical, 0);
    CHECK(native_calls == 1);
  }
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(native_calls == 2);
  CHECK(app_calls == 3);
  CHECK(observed_subtype == 0);
}

TEST_CASE(
    "Actual device hotplug reconciliation defers native notifications "
    "independently",
    "[ui][input][native_devices]") {
  Context context;
  Window window(context);
  InputSystem input(&window);
  auto controller = std::make_unique<Controller>();
  auto* physical = controller.get();
  physical->connected = false;
  input.AddDriver(std::move(controller));
  std::vector<size_t> enumerated_sizes;
  unsigned app_calls = 0;
  {
    auto lock = input.lock();
    input.SetBindingsChangedCallback([&] { ++app_calls; });
    REQUIRE(input.BeginNativeInputTransport(31, {}, [&] {
      auto callback_lock = input.lock();
      enumerated_sizes.push_back(input.EnumerateDevices().size());
    }));
  }
  physical->Hotplug(true);
  physical->Hotplug(true);
  CHECK(enumerated_sizes.empty());
  context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(enumerated_sizes.size() == 1);
  CHECK(enumerated_sizes[0] == 1);
  CHECK(app_calls == 1);
  physical->Hotplug(false);
  CHECK(enumerated_sizes.size() == 1);
  context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(enumerated_sizes.size() == 2);
  CHECK(enumerated_sizes[1] == 0);
  CHECK(app_calls == 2);
  // A list change notification is relevant even without a binding difference.
  physical->Hotplug(false);
  context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(enumerated_sizes.size() == 3);
  CHECK(enumerated_sizes.back() == 0);
}

TEST_CASE(
    "Native device notification stop rejects queued and reused generation",
    "[ui][input][native_devices]") {
  Context context;
  Window window(context);
  InputSystem input(&window);
  auto controller = std::make_unique<Controller>();
  auto* physical = controller.get();
  input.AddDriver(std::move(controller));
  unsigned old_calls = 0, new_calls = 0;
  std::shared_ptr<NativeDeviceNotificationRundown> old_rundown;
  {
    auto lock = input.lock();
    REQUIRE(input.BeginNativeInputTransport(41, {}, [&] { ++old_calls; }));
    input.BindSlot(1, physical, 0, "test-controller", "test controller");
    old_rundown = input.StopNativeDeviceNotifications(41);
    REQUIRE(old_rundown);
    CHECK_FALSE(input.BeginNativeInputTransport(41, {}, [&] { ++old_calls; }));
    input.EndNativeInputTransport(41);
    REQUIRE(input.BeginNativeInputTransport(42, {}, [&] { ++new_calls; }));
    input.StopNativeDeviceNotifications(41);  // Cannot close replacement.
    input.SetSubtypeOverride(physical, 0, 2);
  }
  context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(old_rundown);
  CHECK(old_rundown->Drain());
  CHECK(old_calls == 0);
  CHECK(new_calls == 1);
  {
    auto lock = input.lock();
    input.SetSubtypeOverride(physical, 0, 3);
    input.EndNativeInputTransport(42);
    CHECK_FALSE(input.BeginNativeInputTransport(41, {}, [&] { ++old_calls; }));
    // No-callback diagnostics retain their previous transport ABI.
    REQUIRE(input.BeginNativeInputTransport(42));
  }
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(old_calls == 0);
  CHECK(new_calls == 1);
}

TEST_CASE("Native queued notification cannot keep destroyed InputSystem alive",
          "[ui][input][native_devices]") {
  Context context;
  Window window(context);
  unsigned calls = 0;
  {
    auto input = std::make_unique<InputSystem>(&window);
    auto controller = std::make_unique<Controller>();
    auto* physical = controller.get();
    input->AddDriver(std::move(controller));
    {
      auto lock = input->lock();
      REQUIRE(input->BeginNativeInputTransport(51, {}, [&] { ++calls; }));
      input->BindSlot(1, physical, 0, "test-controller", "test controller");
    }
    // Only the new weak native closure is queued; no legacy hotplug closure.
    input.reset();
  }
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(calls == 0);
}

namespace {
struct CallbackRundownSync {
  std::mutex mutex;
  std::condition_variable changed;
  bool callback_entered = false;
  bool callback_has_owner = false;
  bool stopper_timed_out = false;
  bool callback_timed_out = false;
  bool stop_returned = false;
  bool drain_started = false;
  bool drain_returned = false;
  bool drain_returned_during_callback = false;
  bool token_present = false;
  bool drain_succeeded = false;
  bool owner_destroyed_before_drain_return = false;
};
class CallbackLocalOwner {
 public:
  explicit CallbackLocalOwner(std::atomic<unsigned>& destroyed)
      : destroyed_(destroyed) {}
  ~CallbackLocalOwner() { destroyed_.fetch_add(1, std::memory_order_release); }

 private:
  std::atomic<unsigned>& destroyed_;
};
}  // namespace

TEST_CASE(
    "Native notification rundown covers whole callback and local owner release",
    "[ui][input][native_devices][rundown]") {
  Context context;
  Window window(context);
  InputSystem input(&window);
  auto controller = std::make_unique<Controller>();
  auto* physical = controller.get();
  input.AddDriver(std::move(controller));
  std::atomic<unsigned> destroyed{0};
  auto initial_owner = std::make_shared<CallbackLocalOwner>(destroyed);
  const std::weak_ptr<CallbackLocalOwner> weak_owner = initial_owner;
  CallbackRundownSync sync;
  {
    auto lock = input.lock();
    REQUIRE(input.BeginNativeInputTransport(71, {}, [&] {
      // This is the actual callback frame, including the local strong owner
      // acquired before an owner's later internal Poll/rundown could start.
      auto local_owner = weak_owner.lock();
      std::unique_lock guard(sync.mutex);
      sync.callback_has_owner = bool(local_owner);
      sync.callback_entered = true;
      sync.changed.notify_all();
      if (!sync.changed.wait_for(guard, std::chrono::seconds(2), [&] {
            return sync.stop_returned && sync.drain_started;
          })) {
        sync.callback_timed_out = true;
        return;
      }
      // A broken scope can return from Drain while this owner is still live.
      sync.drain_returned_during_callback =
          sync.changed.wait_for(guard, std::chrono::milliseconds(50),
                                [&] { return sync.drain_returned; });
    }));
    input.BindSlot(1, physical, 0, "test-controller", "test controller");
  }
  // No Catch assertions are executed from this background stop/drain thread.
  std::jthread stopper([&] {
    {
      std::unique_lock guard(sync.mutex);
      if (!sync.changed.wait_for(guard, std::chrono::seconds(2),
                                 [&] { return sync.callback_entered; })) {
        sync.stopper_timed_out = true;
        return;
      }
    }
    initial_owner.reset();
    std::shared_ptr<NativeDeviceNotificationRundown> rundown;
    {
      auto input_lock = input.lock();
      rundown = input.StopNativeDeviceNotifications(71);
    }
    {
      std::lock_guard guard(sync.mutex);
      sync.token_present = bool(rundown);
      sync.stop_returned = true;
      sync.drain_started = true;
      sync.changed.notify_all();
    }
    // The nonrecursive InputSystem lock is released before this wait.
    const bool drained = rundown && rundown->Drain();
    {
      std::lock_guard guard(sync.mutex);
      sync.drain_succeeded = drained;
      sync.owner_destroyed_before_drain_return =
          destroyed.load(std::memory_order_acquire) == 1;
      sync.drain_returned = true;
      sync.changed.notify_all();
    }
  });
  context.ExecutePendingFunctionsFromUIThread();
  stopper.join();
  CHECK_FALSE(sync.stopper_timed_out);
  CHECK_FALSE(sync.callback_timed_out);
  CHECK(sync.callback_has_owner);
  CHECK(sync.token_present);
  CHECK_FALSE(sync.drain_returned_during_callback);
  CHECK(sync.drain_succeeded);
  CHECK(sync.owner_destroyed_before_drain_return);
  CHECK(destroyed.load(std::memory_order_acquire) == 1);
}

TEST_CASE("Native notification rundown rejects callback self wait",
          "[ui][input][native_devices][rundown]") {
  Context context;
  Window window(context);
  InputSystem input(&window);
  auto controller = std::make_unique<Controller>();
  auto* physical = controller.get();
  input.AddDriver(std::move(controller));
  std::shared_ptr<NativeDeviceNotificationRundown> rundown;
  bool entered = false, self_drain = true;
  {
    auto lock = input.lock();
    REQUIRE(input.BeginNativeInputTransport(81, {}, [&] {
      entered = true;
      {
        auto callback_lock = input.lock();
        rundown = input.StopNativeDeviceNotifications(81);
      }
      if (rundown) {
        self_drain = rundown->Drain();
      }
    }));
    input.BindSlot(1, physical, 0, "test-controller", "test controller");
  }
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(entered);
  REQUIRE(rundown);
  CHECK_FALSE(self_drain);
  CHECK(rundown->Drain());  // Outside the complete callback frame.
}

TEST_CASE("Reentrant native queue rejects an ancestor callback drain",
          "[ui][input][native_devices][rundown]") {
  Context context;
  Window window(context);
  InputSystem outer(&window), inner(&window);
  auto outer_driver = std::make_unique<Controller>();
  auto* outer_physical = outer_driver.get();
  outer.AddDriver(std::move(outer_driver));
  auto inner_driver = std::make_unique<Controller>();
  auto* inner_physical = inner_driver.get();
  inner.AddDriver(std::move(inner_driver));
  std::shared_ptr<NativeDeviceNotificationRundown> outer_rundown, inner_rundown;
  bool inner_entered = false, ancestor_drain = true, inner_self_drain = true;
  {
    auto lock = inner.lock();
    REQUIRE(inner.BeginNativeInputTransport(92, {}, [&] {
      inner_entered = true;
      if (outer_rundown) {
        ancestor_drain = outer_rundown->Drain();
      }
      {
        auto inner_lock = inner.lock();
        inner_rundown = inner.StopNativeDeviceNotifications(92);
      }
      if (inner_rundown) {
        inner_self_drain = inner_rundown->Drain();
      }
    }));
  }
  {
    auto lock = outer.lock();
    REQUIRE(outer.BeginNativeInputTransport(91, {}, [&] {
      {
        auto outer_lock = outer.lock();
        outer_rundown = outer.StopNativeDeviceNotifications(91);
      }
      {
        auto inner_lock = inner.lock();
        inner.BindSlot(1, inner_physical, 0, "inner-controller", "inner");
      }
      // Executes real queued B while the outer A notification is still active.
      context.ExecutePendingFunctionsFromUIThread();
    }));
    outer.BindSlot(1, outer_physical, 0, "outer-controller", "outer");
  }
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(inner_entered);
  REQUIRE(outer_rundown);
  REQUIRE(inner_rundown);
  CHECK_FALSE(ancestor_drain);
  CHECK_FALSE(inner_self_drain);
  CHECK(outer_rundown->Drain());
  CHECK(inner_rundown->Drain());
}

}  // namespace xe::hid::test
