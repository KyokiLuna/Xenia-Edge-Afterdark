/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/imgui_confirm_dialog.h"
#include "xenia/ui/imgui_context_menu.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe::ui::test {
namespace {

class HeadlessContext final : public WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class HeadlessWindow final : public Window {
 public:
  explicit HeadlessWindow(WindowedAppContext& context)
      : Window(context, "UI regression test", 1280, 720) {}
  ~HeadlessWindow() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override { return false; }
  void RequestCloseImpl() override {}
  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags /*allowed_types*/) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};

class FakeInputDriver final : public hid::InputDriver {
 public:
  FakeInputDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  hid::InputType GetInputType() const override { return hid::Controller; }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t /*flags*/,
                           hid::X_INPUT_CAPABILITIES* caps) override {
    if (user_index >= connected.size() || !connected[user_index]) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *caps = {};
    caps->type = hid::XINPUT_DEVTYPE_GAMEPAD;
    caps->sub_type = hid::XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->gamepad.buttons = 0xFFFF;
    caps->gamepad.thumb_lx = caps->gamepad.thumb_ly = 32767;
    caps->gamepad.thumb_rx = caps->gamepad.thumb_ry = 32767;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index, hid::X_INPUT_STATE* state) override {
    if (user_index >= connected.size() || !connected[user_index]) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *state = {};
    state->packet_number = 1;
    state->gamepad.buttons = buttons[user_index];
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetState(uint32_t /*user_index*/,
                    hid::X_INPUT_VIBRATION* /*vibration*/) override {
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetKeystroke(uint32_t /*user_index*/, uint32_t /*flags*/,
                        hid::X_INPUT_KEYSTROKE* /*keystroke*/) override {
    return X_ERROR_EMPTY;
  }

  std::array<bool, XUserMaxUserCount> connected{};
  std::array<uint16_t, XUserMaxUserCount> buttons{};
};

class ContextMenuFixture {
 public:
  ContextMenuFixture()
      : window_(context_), input_(nullptr), drawer_(&window_, 0) {
    auto driver = std::make_unique<FakeInputDriver>();
    driver_ = driver.get();
    input_.AddDriver(std::move(driver));
    for (uint32_t slot = 0; slot < 2; ++slot) {
      driver_->connected[slot] = true;
      input_.BindSlot(slot, driver_, static_cast<uint8_t>(slot), {}, {});
    }
    auto& io = drawer_.GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }

  ~ContextMenuFixture() { drawer_.ClearDialogs(); }

  ImGuiContextMenu* Open(
      bool poll_gamepad = true,
      std::shared_ptr<NativeUiBackendRoute::HostLease> host = {}) {
    auto* menu =
        new ImGuiContextMenu(&drawer_, &input_, poll_gamepad, std::move(host));
    menu->AddAction("Continue", [this] { ++action_count_; });
    menu->SetOnCloseCallback([this] { ++close_count_; });
    menu->SetInputOwnershipCallback([this] { return native_input_owned_; });
    menu->Show();
    return menu;
  }

  void Draw(ImGuiDialog* dialog) {
    drawer_.GetIO();
    drawer_.RefreshInputCaptureState();
    ImGui::NewFrame();
    dialog->Draw();
    ImGui::Render();
    drawer_.RefreshInputCaptureState();
  }

  HeadlessContext context_;
  HeadlessWindow window_;
  hid::InputSystem input_;
  ImGuiDrawer drawer_;
  FakeInputDriver* driver_ = nullptr;
  unsigned int close_count_ = 0;
  unsigned int action_count_ = 0;
  bool native_input_owned_ = true;
};

TEST_CASE("Selected host menu action stays inert after pre-stop route seal",
          "[ui][input][context_menu][host_ui_fallback]") {
  ContextMenuFixture fixture;
  NativeUiBackendRoute route;
  const NativeUiBackendRoute::Cohort cohort{1, 2,          3,         4,
                                            5, 0x30020000, 0x3001C000};
  auto host =
      route
          .Admit(cohort, NativeUiBackendRoute::Input::kProvenNoInitialSwap,
                 true)
          .host;
  REQUIRE(host);
  auto* menu = fixture.Open(false, host);
  fixture.Draw(menu);
  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  inputs[1].buttons = inputs[1].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  menu->HandleGamepadInput(inputs);  // Deletes menu, queues actual Continue.
  CHECK(fixture.close_count_ == 1);
  CHECK(fixture.action_count_ == 0);
  CHECK_FALSE(fixture.drawer_.HasOpenDialogs());
  CHECK(route.CurrentHost(cohort) == host);  // Queued action still owns route.
  route.BeginStop();
  REQUIRE(route.Drain(std::chrono::steady_clock::now()));
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.action_count_ == 0);  // Actual queued widget action was gated.
}

TEST_CASE("External menu dismissal releases input without another draw",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  fixture.Draw(menu);
  REQUIRE(fixture.input_.IsUIInputBlocked());
  REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);

  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_B;
  hid::X_INPUT_STATE state{};
  REQUIRE(fixture.input_.GetStateForUI(1, hid::X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == hid::X_INPUT_GAMEPAD_B);
  REQUIRE(fixture.input_.GetState(1, hid::X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == 0);

  // No Draw follows this call: dismissal must release the blocker immediately.
  menu->CloseMenu();
  CHECK(fixture.close_count_ == 1);
  CHECK_FALSE(fixture.drawer_.HasOpenDialogs());
  CHECK_FALSE(fixture.input_.IsUIInputBlocked());
  CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);

  // The close press stays consumed, then a new guest press is delivered.
  REQUIRE(fixture.input_.GetState(1, hid::X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == 0);
  fixture.driver_->buttons[1] = 0;
  REQUIRE(fixture.input_.GetState(1, hid::X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  REQUIRE(fixture.input_.GetState(1, hid::X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == hid::X_INPUT_GAMEPAD_A);
}

TEST_CASE("Menu teardown preserves an unrelated replacement root modal",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  fixture.Draw(menu);
  REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
  const ImGuiID menu_id = ImGui::GetCurrentContext()->OpenPopupStack[0].PopupId;

  ImGui::NewFrame();
  ImGui::OpenPopup("Unrelated modal");
  if (ImGui::BeginPopupModal("Unrelated modal")) {
    ImGui::TextUnformatted("Another dialog owns this popup.");
    ImGui::EndPopup();
  }
  ImGui::Render();
  REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
  const ImGuiID unrelated_id =
      ImGui::GetCurrentContext()->OpenPopupStack[0].PopupId;
  REQUIRE(unrelated_id != menu_id);

  menu->CloseMenu();
  CHECK(fixture.close_count_ == 1);
  REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
  CHECK(ImGui::GetCurrentContext()->OpenPopupStack[0].PopupId == unrelated_id);
  ImGui::ClosePopupToLevel(0, false);
}

TEST_CASE("Externally dismissed menu reopens after idle frames",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  for (unsigned int cycle = 0; cycle < 3; ++cycle) {
    auto* menu = fixture.Open(false);
    fixture.Draw(menu);
    REQUIRE(fixture.drawer_.HasOpenDialogs());
    REQUIRE(fixture.input_.IsUIInputBlocked());
    REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    fixture.Draw(menu);
    REQUIRE(fixture.drawer_.HasOpenDialogs());
    auto* popup = ImGui::GetCurrentContext()->OpenPopupStack[0].Window;
    REQUIRE(popup != nullptr);
    CHECK(popup->Active);
    CHECK_FALSE(popup->Hidden);

    menu->CloseMenu();
    CHECK(fixture.close_count_ == cycle + 1);
    CHECK_FALSE(fixture.drawer_.HasOpenDialogs());
    CHECK_FALSE(fixture.input_.IsUIInputBlocked());
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);
    // No ImGui frame is submitted while its drawer is detached.
  }
}

TEST_CASE("Draw polling accepts dismissal from a later controller slot",
          "[ui][input][context_menu]") {
  const uint16_t close_button =
      GENERATE(hid::X_INPUT_GAMEPAD_B, hid::X_INPUT_GAMEPAD_BACK,
               hid::X_INPUT_GAMEPAD_GUIDE);
  ContextMenuFixture fixture;
  fixture.driver_->buttons[1] = close_button;
  auto* menu = fixture.Open();
  fixture.Draw(menu);
  REQUIRE(fixture.drawer_.HasOpenDialogs());
  REQUIRE(fixture.close_count_ == 0);

  fixture.driver_->buttons[1] = 0;
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = close_button;
  fixture.Draw(menu);
  CHECK(fixture.close_count_ == 1);
  CHECK_FALSE(fixture.drawer_.HasOpenDialogs());
  CHECK_FALSE(fixture.input_.IsUIInputBlocked());
}

TEST_CASE("Externally polled menus have one controller dismissal owner",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_GUIDE;
  fixture.Draw(menu);
  REQUIRE(fixture.drawer_.HasOpenDialogs());
  CHECK(fixture.close_count_ == 0);
  menu->CloseMenu();
  CHECK(fixture.close_count_ == 1);
}

TEST_CASE("Replacement popup cannot activate the old menu's action",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open();
  fixture.Draw(menu);

  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  ImGui::NewFrame();
  ImGui::OpenPopup("Replacement modal");
  REQUIRE(ImGui::BeginPopupModal("Replacement modal"));
  ImGui::TextUnformatted("A belongs to this dialog.");
  ImGui::EndPopup();
  REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
  const ImGuiID replacement_id =
      ImGui::GetCurrentContext()->OpenPopupStack[0].PopupId;
  menu->Draw();
  ImGui::Render();
  fixture.context_.ExecutePendingFunctionsFromUIThread();

  CHECK(fixture.action_count_ == 0);
  CHECK(fixture.close_count_ == 1);
  REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
  CHECK(ImGui::GetCurrentContext()->OpenPopupStack[0].PopupId ==
        replacement_id);
  ImGui::ClosePopupToLevel(0, false);
}

TEST_CASE(
    "Inactive menu consumes held A and navigation across ownership return",
    "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open();
  unsigned int second_action_count = 0;
  menu->AddAction("Second action", [&] { ++second_action_count; });
  fixture.Draw(menu);

  ImGuiDialog* other_dialog = nullptr;
  SECTION("Native modal owns input") { fixture.native_input_owned_ = false; }
  SECTION("Another registered ImGui dialog owns input") {
    other_dialog =
        ImGuiDialog::ShowMessageBox(&fixture.drawer_, "Another modal", "Busy");
  }
  SECTION("Child popup owns input") {
    ImGui::NewFrame();
    REQUIRE(ImGui::BeginPopupModal("##ContextMenu"));
    ImGui::OpenPopup("Child popup");
    REQUIRE(ImGui::BeginPopupModal("Child popup"));
    ImGui::TextUnformatted("Child owns the controller.");
    ImGui::EndPopup();
    ImGui::EndPopup();
    ImGui::Render();
    REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 2);
  }

  fixture.driver_->buttons[1] =
      hid::X_INPUT_GAMEPAD_DPAD_DOWN | hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  REQUIRE(fixture.close_count_ == 0);
  CHECK(fixture.action_count_ == 0);
  CHECK(second_action_count == 0);

  fixture.native_input_owned_ = true;
  if (other_dialog) {
    other_dialog->Destroy();
  }
  if (ImGui::GetCurrentContext()->OpenPopupStack.Size == 2) {
    ImGui::ClosePopupToLevel(1, false);
  }
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  REQUIRE(fixture.close_count_ == 0);
  CHECK(fixture.action_count_ == 0);
  CHECK(second_action_count == 0);

  fixture.driver_->buttons[1] = 0;
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.close_count_ == 1);
  CHECK(fixture.action_count_ == 1);
  CHECK(second_action_count == 0);
}

TEST_CASE("Native modal prevents first popup from stealing input",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  fixture.native_input_owned_ = false;
  auto* menu = fixture.Open();
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(menu);
  CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
  CHECK(fixture.close_count_ == 0);

  fixture.native_input_owned_ = true;
  fixture.Draw(menu);
  CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
  CHECK(fixture.close_count_ == 0);
  CHECK(fixture.action_count_ == 0);

  fixture.driver_->buttons[1] = 0;
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.action_count_ == 1);
  CHECK(fixture.close_count_ == 1);
}

TEST_CASE(
    "Controller selects Return despite a parked mouse or older ImGui focus",
    "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open();
  unsigned int other_actions = 0;
  unsigned int return_actions = 0;
  for (unsigned int i = 0; i < 7; ++i) {
    menu->AddAction("Other action " + std::to_string(i),
                    [&] { ++other_actions; });
  }
  menu->AddAction("Return to Dashboard", [&] { ++return_actions; });
  menu->AddAction("Quit", [&] { ++other_actions; });
  menu->ShowAt(100, 100);
  fixture.Draw(menu);
  fixture.Draw(menu);
  fixture.Draw(menu);
  auto* popup = ImGui::GetCurrentContext()->OpenPopupStack[0].Window;
  REQUIRE(popup != nullptr);
  auto& io = fixture.drawer_.GetIO();

  SECTION("Mouse remains parked over the first row") {
    io.AddMousePosEvent(popup->DC.CursorStartPos.x + 5,
                        popup->DC.CursorStartPos.y + ImGui::GetFontSize() / 2);
    fixture.Draw(menu);
    fixture.Draw(menu);
    REQUIRE(ImGui::GetCurrentContext()->HoveredWindow == popup);
  }
  SECTION("ImGui navigation highlights the original row") {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.AddKeyEvent(ImGuiKey_RightArrow, true);
    fixture.Draw(menu);
    io.AddKeyEvent(ImGuiKey_RightArrow, false);
    fixture.Draw(menu);
    REQUIRE(ImGui::GetCurrentContext()->NavWindow == popup);
  }

  // Real raw controller edges, with release/idle frames before activation.
  for (unsigned int i = 0; i < 8; ++i) {
    fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
    fixture.Draw(menu);
    fixture.driver_->buttons[1] = 0;
    fixture.Draw(menu);
  }
  REQUIRE(fixture.close_count_ == 0);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(return_actions == 1);
  CHECK(fixture.action_count_ == 0);
  CHECK(other_actions == 0);
}

TEST_CASE("Stationary mouse does not override keyboard menu selection",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  unsigned int return_actions = 0;
  menu->AddAction("Return to Dashboard", [&] { ++return_actions; });
  menu->ShowAt(100, 100);
  fixture.Draw(menu);
  fixture.Draw(menu);
  fixture.Draw(menu);
  auto* popup = ImGui::GetCurrentContext()->OpenPopupStack[0].Window;
  REQUIRE(popup != nullptr);
  auto& io = fixture.drawer_.GetIO();
  io.AddMousePosEvent(popup->DC.CursorStartPos.x + 5,
                      popup->DC.CursorStartPos.y + ImGui::GetFontSize() / 2);
  fixture.Draw(menu);
  fixture.Draw(menu);
  REQUIRE(ImGui::GetCurrentContext()->HoveredWindow == popup);

  io.AddKeyEvent(ImGuiKey_DownArrow, true);
  fixture.Draw(menu);
  io.AddKeyEvent(ImGuiKey_DownArrow, false);
  fixture.Draw(menu);
  io.AddKeyEvent(ImGuiKey_Enter, true);
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(return_actions == 1);
  CHECK(fixture.action_count_ == 0);
}

TEST_CASE("Fresh mouse movement can reclaim menu selection from the controller",
          "[ui][input][context_menu]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open();
  unsigned int return_actions = 0;
  menu->AddAction("Return to Dashboard", [&] { ++return_actions; });
  menu->ShowAt(100, 100);
  fixture.Draw(menu);
  fixture.Draw(menu);
  fixture.Draw(menu);
  auto* popup = ImGui::GetCurrentContext()->OpenPopupStack[0].Window;
  REQUIRE(popup != nullptr);
  const ImVec2 first_row(popup->DC.CursorStartPos.x + 5,
                         popup->DC.CursorStartPos.y + ImGui::GetFontSize() / 2);
  auto& io = fixture.drawer_.GetIO();
  io.AddMousePosEvent(first_row.x, first_row.y);
  fixture.Draw(menu);
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = 0;
  fixture.Draw(menu);

  io.AddMousePosEvent(first_row.x + 2, first_row.y);
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.action_count_ == 1);
  CHECK(return_actions == 0);
}

TEST_CASE(
    "Eight externally polled Down edges and A activate Return without Draw",
    "[ui][input][context_menu][external]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  unsigned int other_actions = 0;
  unsigned int return_actions = 0;
  for (unsigned int i = 0; i < 7; ++i) {
    menu->AddAction("Other action " + std::to_string(i),
                    [&] { ++other_actions; });
  }
  menu->AddAction("Return to Dashboard", [&] { ++return_actions; });
  menu->AddAction("Quit", [&] { ++other_actions; });
  fixture.Draw(menu);
  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  for (unsigned int i = 0; i < 8; ++i) {
    inputs[1].buttons = inputs[1].pressed_buttons =
        hid::X_INPUT_GAMEPAD_DPAD_DOWN;
    inputs[1].menu_navigation = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
    menu->HandleGamepadInput(inputs);
    inputs[1] = {};
    menu->HandleGamepadInput(inputs);
  }
  REQUIRE(fixture.close_count_ == 0);
  inputs[1].buttons = inputs[1].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  menu->HandleGamepadInput(inputs);  // Deletes the menu, without another Draw.
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.close_count_ == 1);
  CHECK_FALSE(fixture.drawer_.HasOpenDialogs());
  CHECK(return_actions == 1);
  CHECK(fixture.action_count_ == 0);
  CHECK(other_actions == 0);
}

TEST_CASE("External menu discards inactive edges and held navigation repeats",
          "[ui][input][context_menu][external]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  unsigned int second_actions = 0;
  menu->AddAction("Second action", [&] { ++second_actions; });
  fixture.Draw(menu);
  ImGuiDialog* other_dialog = nullptr;
  SECTION("Native modal") { fixture.native_input_owned_ = false; }
  SECTION("Another ImGui dialog") {
    other_dialog =
        ImGuiDialog::ShowMessageBox(&fixture.drawer_, "Other dialog", "Busy");
  }
  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  inputs[1].buttons = inputs[1].pressed_buttons =
      hid::X_INPUT_GAMEPAD_DPAD_DOWN | hid::X_INPUT_GAMEPAD_A;
  inputs[1].menu_navigation = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  menu->HandleGamepadInput(inputs);
  REQUIRE(fixture.close_count_ == 0);
  fixture.native_input_owned_ = true;
  if (other_dialog) {
    other_dialog->Destroy();
  }
  inputs[1].pressed_buttons = 0;  // Still held; repeat must not leak across.
  menu->HandleGamepadInput(inputs);
  REQUIRE(fixture.close_count_ == 0);

  // No release-only sample is delivered. A new edge proves it was released;
  // the old held Down repeat remains suppressed, so Continue is selected.
  inputs[1].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  menu->HandleGamepadInput(inputs);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.close_count_ == 1);
  CHECK(fixture.action_count_ == 1);
  CHECK(second_actions == 0);
}

TEST_CASE("External gamepad mode has no second draw or ImGui navigation owner",
          "[ui][input][context_menu][external]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  unsigned int return_actions = 0;
  menu->AddAction("Return to Dashboard", [&] { ++return_actions; });
  fixture.Draw(menu);
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  fixture.Draw(menu);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(menu);
  REQUIRE(fixture.close_count_ == 0);

  auto& io = fixture.drawer_.GetIO();
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
  fixture.Draw(menu);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
  fixture.Draw(menu);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
  fixture.Draw(menu);
  REQUIRE(fixture.close_count_ == 0);

  // Custom keyboard selection still works; raw/ImGui gamepad input did not
  // move it or activate the original ImGui navigation ID.
  io.AddKeyEvent(ImGuiKey_DownArrow, true);
  fixture.Draw(menu);
  io.AddKeyEvent(ImGuiKey_DownArrow, false);
  fixture.Draw(menu);
  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  inputs[1].buttons = inputs[1].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  menu->HandleGamepadInput(inputs);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(return_actions == 1);
  CHECK(fixture.action_count_ == 0);
}

TEST_CASE("Dismissal takes priority over external navigation and activation",
          "[ui][input][context_menu][external]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  fixture.Draw(menu);
  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  inputs[0].buttons = inputs[0].pressed_buttons =
      hid::X_INPUT_GAMEPAD_DPAD_DOWN | hid::X_INPUT_GAMEPAD_A;
  inputs[1].buttons = inputs[1].pressed_buttons = hid::X_INPUT_GAMEPAD_B;
  menu->HandleGamepadInput(inputs);
  REQUIRE(fixture.close_count_ == 0);
  CHECK(fixture.action_count_ == 0);
  menu->CloseMenu();  // External policy remains the dismissal owner.
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.action_count_ == 0);
}

TEST_CASE("Mouse can still activate a menu without ImGui navigation",
          "[ui][input][context_menu][external]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  menu->ShowAt(100, 100);
  fixture.Draw(menu);
  fixture.Draw(menu);
  fixture.Draw(menu);
  auto* popup = ImGui::GetCurrentContext()->OpenPopupStack[0].Window;
  REQUIRE(popup != nullptr);
  auto& io = fixture.drawer_.GetIO();
  io.AddMousePosEvent(popup->DC.CursorStartPos.x + 5,
                      popup->DC.CursorStartPos.y + ImGui::GetFontSize() / 2);
  fixture.Draw(menu);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  fixture.Draw(menu);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  fixture.Draw(menu);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.action_count_ == 1);
  CHECK(fixture.close_count_ == 1);
}

TEST_CASE(
    "Modal capture epoch consumes a held press after an unseen round trip",
    "[ui][input][context_menu][external]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  unsigned int second_actions = 0;
  menu->AddAction("Second action", [&] { ++second_actions; });
  const uint64_t menu_epoch = fixture.drawer_.input_capture_generation();
  fixture.Draw(menu);
  CHECK(fixture.drawer_.input_capture_generation() == menu_epoch);

  auto* other =
      ImGuiDialog::ShowMessageBox(&fixture.drawer_, "Transient dialog", "Busy");
  other->Destroy();  // No Draw or menu input sample occurred during this modal.
  REQUIRE(fixture.drawer_.input_capture_generation() != menu_epoch);

  // Old-epoch queued edges are rejected by the caller. This is the next held
  // snapshot, whose A and Down repeat must not carry across the round trip.
  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  inputs[1].buttons = hid::X_INPUT_GAMEPAD_A | hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  inputs[1].menu_navigation = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  menu->HandleGamepadInput(inputs);
  REQUIRE(fixture.close_count_ == 0);
  inputs[1].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  menu->HandleGamepadInput(inputs);
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  CHECK(fixture.action_count_ == 1);
  CHECK(second_actions == 0);
  CHECK(fixture.close_count_ == 1);
}

TEST_CASE("External activation cannot confirm the next dialog with the held A",
          "[ui][input][context_menu][external]") {
  ContextMenuFixture fixture;
  auto* menu = fixture.Open(false);
  ImGuiConfirmDialog* confirm = nullptr;
  unsigned int confirmations = 0;
  bool confirmed = false;
  menu->AddAction("Return to Dashboard", [&] {
    confirm = new ImGuiConfirmDialog(
        &fixture.drawer_, "Return to Dashboard", "Really stop title?",
        [&](bool result) {
          ++confirmations;
          confirmed = result;
        },
        &fixture.input_);
  });
  fixture.Draw(menu);
  auto& io = fixture.drawer_.GetIO();
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  SECTION("External navigation arrives without another frame") {}
  SECTION("ImGui gamepad navigation was already active") {
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
    fixture.Draw(menu);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
    fixture.Draw(menu);
  }
  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  inputs[1].buttons = inputs[1].pressed_buttons =
      hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  menu->HandleGamepadInput(inputs);

  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
  inputs[1].buttons = inputs[1].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  menu->HandleGamepadInput(inputs);  // No NewFrame has observed that A yet.
  fixture.context_.ExecutePendingFunctionsFromUIThread();
  REQUIRE(confirm != nullptr);
  REQUIRE(fixture.close_count_ == 1);

  // Model UpdateGamepads reporting the still-held physical A to the new
  // modal, even if detaching the previous menu cleared its queued IO event.
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
  for (unsigned int i = 0; i < 30; ++i) {
    fixture.Draw(confirm);
    REQUIRE(confirmations == 0);
  }
  CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));
  REQUIRE(fixture.drawer_.HasOpenDialogs());

  fixture.driver_->buttons[1] = 0;
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
  fixture.Draw(confirm);
  REQUIRE(confirmations == 0);
  fixture.driver_->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
  fixture.Draw(confirm);
  const auto& context = *ImGui::GetCurrentContext();
  CAPTURE(context.NavId, context.NavCursorVisible, context.NavInputSource,
          context.NavActivateId, context.NavActivatePressedId,
          context.NavWindow ? context.NavWindow->Name : "none",
          ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->DownDuration,
          io.ConfigFlags, io.BackendFlags);
  CHECK(confirmations == 1);
  CHECK(confirmed);
  CHECK_FALSE(fixture.drawer_.HasOpenDialogs());
}

}  // namespace
}  // namespace xe::ui::test
