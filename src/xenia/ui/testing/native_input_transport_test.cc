/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <memory>
#include "third_party/catch/include/catch.hpp"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/ui/windowed_app_context.h"
namespace xe::hid::test {
class NativeGuideButtonEdgeTestAccess {
 public:
  static std::shared_ptr<const NativeGuideButtonEdge> Make(
      uint64_t epoch, uint32_t slot, std::function<bool()> dispatch) {
    return std::shared_ptr<const NativeGuideButtonEdge>(
        new NativeGuideButtonEdge(epoch, slot, std::move(dispatch)));
  }
};
namespace {
class Controller final : public InputDriver {
 public:
  Controller() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  InputType GetInputType() const override { return InputType::Controller; }
  X_RESULT GetCapabilities(uint32_t slot, uint32_t,
                           X_INPUT_CAPABILITIES* out) override {
    if (slot >= XUserMaxUserCount) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    out->type = XINPUT_DEVTYPE_GAMEPAD;
    out->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t slot, X_INPUT_STATE* out) override {
    if (slot >= XUserMaxUserCount) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    out->packet_number = packets[slot];
    out->gamepad.buttons = buttons[slot];
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t, X_INPUT_VIBRATION*) override {
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }
  std::array<uint16_t, XUserMaxUserCount> buttons{};
  std::array<uint32_t, XUserMaxUserCount> packets{};
};
}  // namespace
TEST_CASE(
    "Native physical read admission is independent of host plane ownership",
    "[ui][input][native_transport]") {
  InputSystem input(nullptr);
  auto driver = std::make_unique<Controller>();
  auto* physical = driver.get();
  input.AddDriver(std::move(driver));
  auto guard = input.lock();
  input.BindSlot(2, physical, 2, "physical2", "controller2");
  physical->buttons[2] = X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_GUIDE;
  physical->packets[2] = 41;
  REQUIRE(input.BeginNativeInputTransport(7));
  X_INPUT_STATE state{};
  REQUIRE(input.GetStateForNativeDriver(2, InputType::Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_GUIDE));
  // The native191/196 audience gate sits above this physical adapter. Even a
  // separate HLE native-owner flag cannot replace that native counter policy.
  REQUIRE(input.SetNativeInputOwner(7, true));
  REQUIRE(input.GetStateForNativeDriver(2, InputType::Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_GUIDE));
  X_INPUT_STATE title{};
  REQUIRE(input.GetState(2, InputType::Controller, &title) == X_ERROR_SUCCESS);
  CHECK(title.gamepad.buttons == 0);
}
TEST_CASE(
    "Native physical readers preserve host modal and held-release suppression",
    "[ui][input][native_transport]") {
  InputSystem input(nullptr);
  auto driver = std::make_unique<Controller>();
  auto* physical = driver.get();
  input.AddDriver(std::move(driver));
  auto guard = input.lock();
  input.BindSlot(1, physical, 1, "physical1", "controller1");
  REQUIRE(input.BeginNativeInputTransport(8));
  physical->buttons[1] = X_INPUT_GAMEPAD_B;
  physical->packets[1] = 17;
  input.AddUIInputBlocker();
  X_INPUT_STATE blocked{};
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &blocked) ==
          X_ERROR_SUCCESS);
  CHECK(blocked.gamepad.buttons == 0);
  input.RemoveUIInputBlocker();
  X_INPUT_STATE returned{};
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &returned) ==
          X_ERROR_SUCCESS);
  CHECK(returned.gamepad.buttons == 0);
  CHECK(returned.packet_number != blocked.packet_number);
  physical->buttons[1] = 0;
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &returned) ==
          X_ERROR_SUCCESS);
  physical->buttons[1] = X_INPUT_GAMEPAD_B;
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &returned) ==
          X_ERROR_SUCCESS);
  CHECK(returned.gamepad.buttons == X_INPUT_GAMEPAD_B);
  REQUIRE(input.GetStateForNativeDriver(0, InputType::Controller, &returned) ==
          X_ERROR_DEVICE_NOT_CONNECTED);
}
TEST_CASE(
    "Native Guide edge keeps its originating slot and is consumed exactly once",
    "[ui][input][native_transport]") {
  InputSystem input(nullptr);
  uint32_t received = XUserIndexAny;
  unsigned calls = 0;
  {
    auto guard = input.lock();
    REQUIRE(input.BeginNativeInputTransport(9, [&](uint32_t slot) {
      return NativeGuideButtonEdgeTestAccess::Make(9, slot, [&, slot] {
        ++calls;
        received = slot;
        return true;
      });
    }));
    REQUIRE(input.EnableNativeGuideTransport(9));
  }
  for (uint32_t slot = 0; slot < XUserMaxUserCount; ++slot) {
    auto edge = input.CaptureNativeGuideButtonEdge(slot, 9);
    REQUIRE(edge);
    CHECK(edge->source_slot() == slot);
    CHECK(input.DispatchNativeGuideButtonEdge(edge, 9) ==
          NativeGuideDisposition::kAccepted);
    CHECK(received == slot);
    CHECK(input.DispatchNativeGuideButtonEdge(edge, 9) ==
          NativeGuideDisposition::kDenied);
  }
  CHECK(calls == XUserMaxUserCount);
}
TEST_CASE(
    "Native Guide stale/modal/unsupported edges never authorize host fallback",
    "[ui][input][native_transport]") {
  InputSystem input(nullptr);
  unsigned calls = 0;
  {
    auto guard = input.lock();
    REQUIRE(input.BeginNativeInputTransport(10, [&](uint32_t slot) {
      return NativeGuideButtonEdgeTestAccess::Make(10, slot, [&] {
        ++calls;
        return true;
      });
    }));
    REQUIRE(input.EnableNativeGuideTransport(10));
  }
  auto edge = input.CaptureNativeGuideButtonEdge(3, 10);
  REQUIRE(edge);
  {
    auto guard = input.lock();
    input.AddUIInputBlocker();
  }
  CHECK(input.DispatchNativeGuideButtonEdge(edge, 10) ==
        NativeGuideDisposition::kDenied);
  {
    auto guard = input.lock();
    input.RemoveUIInputBlocker();
  }
  CHECK(input.DispatchNativeGuideButtonEdge(edge, 10) ==
        NativeGuideDisposition::kDenied);
  CHECK(calls == 0);
  auto stale = input.CaptureNativeGuideButtonEdge(2, 10);
  REQUIRE(stale);
  {
    auto guard = input.lock();
    input.EndNativeInputTransport(10);
    REQUIRE(input.BeginNativeInputTransport(11));
  }
  CHECK(input.DispatchNativeGuideButtonEdge(stale, 10) ==
        NativeGuideDisposition::kDenied);
  CHECK_FALSE(input.CaptureNativeGuideButtonEdge(0, 10));
  CHECK(input.native_guide_generation() == 0);
  CHECK(input.DispatchNativeGuideButtonEdge({}, 11) ==
        NativeGuideDisposition::kDenied);
  CHECK(input.DispatchNativeGuideButtonEdge({}, 0) ==
        NativeGuideDisposition::kInactive);
  CHECK(calls == 0);
}
TEST_CASE("Diagnostic input transport does not acquire the host Guide edge",
          "[ui][input][native_transport]") {
  InputSystem input(nullptr);
  {
    auto guard = input.lock();
    REQUIRE(input.BeginNativeInputTransport(12));
    CHECK_FALSE(input.EnableNativeGuideTransport(12));
  }
  CHECK(input.native_input_generation() == 12);
  CHECK(input.native_guide_generation() == 0);
  CHECK_FALSE(input.CaptureNativeGuideButtonEdge(0, 12));
  CHECK(input.DispatchNativeGuideButtonEdge({}, 0) ==
        NativeGuideDisposition::kInactive);
}
TEST_CASE(
    "Native Guide sample shares held-release masking while Title polling "
    "stalls",
    "[ui][input][native_transport]") {
  InputSystem input(nullptr);
  auto driver = std::make_unique<Controller>();
  auto* physical = driver.get();
  input.AddDriver(std::move(driver));
  auto guard = input.lock();
  input.BindSlot(3, physical, 3, "physical3", "controller3");
  // The host modal closes between polling samples with Guide held. Raw host
  // input must remain available, while native intent stays suppressed.
  input.AddUIInputBlocker();
  physical->buttons[3] = X_INPUT_GAMEPAD_GUIDE;
  input.RemoveUIInputBlocker();
  X_INPUT_STATE raw{};
  REQUIRE(input.GetStateForUI(3, InputType::Controller, &raw) ==
          X_ERROR_SUCCESS);
  CHECK(raw.gamepad.buttons == X_INPUT_GAMEPAD_GUIDE);
  auto native = raw;
  input.ApplyNativeHostModalSuppression(3, &native);
  CHECK(native.gamepad.buttons == 0);
  // Only the frontend samples now; no Title191 or native UI read occurs.
  physical->buttons[3] = 0;
  REQUIRE(input.GetStateForUI(3, InputType::Controller, &raw) ==
          X_ERROR_SUCCESS);
  input.ApplyNativeHostModalSuppression(3, &raw);
  physical->buttons[3] = X_INPUT_GAMEPAD_GUIDE;
  REQUIRE(input.GetStateForUI(3, InputType::Controller, &raw) ==
          X_ERROR_SUCCESS);
  input.ApplyNativeHostModalSuppression(3, &raw);
  CHECK(raw.gamepad.buttons == X_INPUT_GAMEPAD_GUIDE);
}
TEST_CASE("Selected host queue gap transfers to real modal without input leak",
          "[ui][input][native_transport][host_ui_fallback]") {
  InputSystem input(nullptr);
  auto driver = std::make_unique<Controller>();
  auto* physical = driver.get();
  input.AddDriver(std::move(driver));
  auto guard = input.lock();
  input.BindSlot(1, physical, 1, "physical1", "controller1");
  physical->buttons[1] = X_INPUT_GAMEPAD_A;
  REQUIRE(input.BeginNativeInputTransport(19));
  auto gap = input.ReserveHostUiInput(19);
  REQUIRE(gap);
  REQUIRE(input.IsUIInputBlocked());
  X_INPUT_STATE raw{}, title{};
  REQUIRE(input.GetStateForUI(1, InputType::Controller, &raw) ==
          X_ERROR_SUCCESS);
  CHECK(raw.gamepad.buttons ==
        X_INPUT_GAMEPAD_A);  // Host UI still receives input.
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &title) ==
          X_ERROR_SUCCESS);
  CHECK(title.gamepad.buttons == 0);
  // This is the same normal blocker installed by actual dialog constructors.
  input.AddUIInputBlocker();
  gap->Release();
  CHECK(input.IsUIInputBlocked());
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &title) ==
          X_ERROR_SUCCESS);
  CHECK(title.gamepad.buttons == 0);
  input.RemoveUIInputBlocker();
  CHECK_FALSE(input.IsUIInputBlocked());
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &title) ==
          X_ERROR_SUCCESS);
  CHECK(title.gamepad.buttons == 0);  // Existing actual close quarantine.
  physical->buttons[1] = 0;
  REQUIRE(input.GetStateForUI(1, InputType::Controller, &raw) ==
          X_ERROR_SUCCESS);
  physical->buttons[1] = X_INPUT_GAMEPAD_A;
  REQUIRE(input.GetStateForNativeDriver(1, InputType::Controller, &title) ==
          X_ERROR_SUCCESS);
  CHECK(title.gamepad.buttons == X_INPUT_GAMEPAD_A);
}
TEST_CASE(
    "Canceled queued host gap outlives InputSystem without provider access",
    "[ui][input][native_transport][host_ui_fallback]") {
  class Context final : public ui::WindowedAppContext {
    void NotifyUILoopOfPendingFunctions() override {}
    void PlatformQuitFromUIThread() override {}
  } app;
  using Route = NativeUiBackendRoute;
  Route route;
  const Route::Cohort cohort{1, 2, 3, 4, 5, 0x30020000, 0x3001C000};
  auto host =
      route.Admit(cohort, Route::Input::kProvenNoInitialSwap, true).host;
  REQUIRE(host);
  auto completion = std::make_shared<kernel::xam::HostUiFallbackCompletion>();
  auto input = std::make_unique<InputSystem>(nullptr);
  std::shared_ptr<UiInputReservation> old;
  {
    auto guard = input->lock();
    REQUIRE(input->BeginNativeInputTransport(29));
    old = input->ReserveHostUiInput(29);
  }
  REQUIRE(old);
  unsigned accessed = 0;
  REQUIRE(kernel::xam::QueueHostUiFallback(app, host, completion,
                                           [old, &accessed](const auto&) {
                                             ++accessed;
                                             old->Release();
                                           }));
  route.BeginStop();
  REQUIRE(completion->closed.TryWait());
  {
    auto guard = input->lock();
    input->EndNativeInputTransport(29);
    REQUIRE(input->BeginNativeInputTransport(30));
  }
  CHECK_FALSE(input->IsUIInputBlocked());
  std::shared_ptr<UiInputReservation> current;
  {
    auto guard = input->lock();
    current = input->ReserveHostUiInput(30);
  }
  REQUIRE(current);
  old->Release();  // A stale token cannot unblock or poison this new
                   // generation.
  CHECK(input->IsUIInputBlocked());
  current->Release();
  CHECK_FALSE(input->IsUIInputBlocked());
  input.reset();  // Queue retains only CPU counter state, never this provider.
  app.ExecutePendingFunctionsFromUIThread();
  CHECK(accessed == 0);
}
}  // namespace xe::hid::test
