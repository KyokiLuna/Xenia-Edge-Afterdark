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
#include <string>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/ui/imgui_context_menu.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_text_entry_dialog.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe::ui::test {
// Access the actual production mapper without a fake renderer or public API.
class ImGuiDrawerGamepadTestAccess {
 public:
  static void Poll(ImGuiDrawer& drawer) { drawer.UpdateGamepads(); }
};
namespace {
class Context final : public WindowedAppContext {
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
class Window final : public ui::Window {
 public:
  explicit Window(Context& context)
      : ui::Window(context, "controller modality", 1280, 720) {}
  ~Window() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override { return false; }
  void RequestCloseImpl() override {}
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags) override {
    return {};
  }
  void RequestPaintImpl() override {}
};
class Pad final : public hid::InputDriver {
 public:
  Pad() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  hid::InputType GetInputType() const override { return hid::Controller; }
  X_RESULT GetCapabilities(uint32_t slot, uint32_t,
                           hid::X_INPUT_CAPABILITIES* caps) override {
    if (slot != 1) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *caps = {};
    caps->type = hid::XINPUT_DEVTYPE_GAMEPAD;
    caps->sub_type = hid::XINPUT_DEVSUBTYPE_GAMEPAD;
    // Actual SDL-shaped capabilities; FFFF/zero-rumble is the keyboard sentinel
    // deliberately ignored by UpdateGamepads and is not a suitable pad mock.
    caps->gamepad.buttons = 0xF7FF;
    caps->vibration.left_motor_speed = caps->vibration.right_motor_speed =
        0xFFFF;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t slot, hid::X_INPUT_STATE* state) override {
    if (slot != 1) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    ++polls;
    *state = {};
    state->packet_number = packet;
    state->gamepad.buttons = buttons;
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t, hid::X_INPUT_VIBRATION*) override {
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, hid::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }
  void Set(uint16_t value) {
    if (value != buttons) {
      ++packet;
    }
    buttons = value;
  }
  uint16_t buttons = 0;
  uint32_t packet = 1, polls = 0;
};
class Fixture {
 public:
  Fixture() : window(context), input(nullptr), drawer(&window, 0) {
    auto pad = std::make_unique<Pad>();
    physical = pad.get();
    input.AddDriver(std::move(pad));
    {
      auto lock = input.lock();
      input.BindSlot(1, physical, 1, "pad1", "physical pad1");
    }
    drawer.LoadInputSystem(&input);
    auto& io = drawer.GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }
  ~Fixture() { drawer.ClearDialogs(); }
  void Message(std::vector<std::string> buttons = {"OK"},
               uint32_t initial = 0) {
    message = new kernel::xam::MessageBoxDialog(
        &drawer, &input, "Test message", "Body", std::move(buttons), initial);
    auto* current = message;
    message->set_close_callback([this, current] {
      chosen = current->chosen_button();
      ++closed;
      message = nullptr;
    });
  }
  void Menu(std::function<void()> action = {}) {
    menu = new ImGuiContextMenu(&drawer, &input, false);
    menu->AddAction("Continue", action ? std::move(action) : [] {});
    menu->SetOnCloseCallback([this] { menu = nullptr; });
    menu->Show();
  }
  void Frame() {
    auto& io = drawer.GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    drawer.RefreshInputCaptureState();
    // Exact production order: GetStateForUI mapper BEFORE NewFrame, actual
    // widget AFTER event processing. No hand-written gamepad event mapping.
    if (drawer.HasOpenDialogs()) {
      ImGuiDrawerGamepadTestAccess::Poll(drawer);
    }
    ImGui::NewFrame();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
      ++fresh_a_seen;
    }
    if (menu) {
      menu->Draw();
    }
    if (message) {
      message->Draw();
    }
    if (entry) {
      entry->Draw();
      if (!drawer.HasOpenDialogs()) {
        entry = nullptr;
      }
    }
    ImGui::Render();
    drawer.RefreshInputCaptureState();
  }
  void Frames(unsigned count = 3) {
    while (count--) {
      Frame();
    }
  }
  void Pulse(uint16_t buttons) {
    physical->Set(buttons);
    Frames(2);
    physical->Set(0);
    Frames(2);
  }
  void ExternalMenuEdge(uint16_t buttons) {
    std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
    inputs[1].buttons = inputs[1].pressed_buttons = buttons;
    menu->HandleGamepadInput(inputs);
  }
  Context context;
  Window window;
  hid::InputSystem input;
  ImGuiDrawer drawer;
  Pad* physical = nullptr;
  kernel::xam::MessageBoxDialog* message = nullptr;
  ImGuiContextMenu* menu = nullptr;
  ImGuiTextEntryDialog* entry = nullptr;
  unsigned closed = 0, fresh_a_seen = 0;
  uint32_t chosen = UINT32_MAX;
};
TEST_CASE("Cold message accepts its default from one fresh physical A",
          "[ui][input][controller_modality][message_box]") {
  Fixture f;
  SECTION("Single OK") { f.Message(); }
  SECTION("Requested nonzero default") { f.Message({"Back", "OK", "More"}, 1); }
  f.Frames();
  REQUIRE(ImGui::GetCurrentContext()->NavId != 0);
  CHECK_FALSE(ImGui::GetCurrentContext()->NavCursorVisible);
  REQUIRE(f.closed == 0);
  const auto expected = f.message->chosen_button();
  const auto polls = f.physical->polls;
  f.Pulse(hid::X_INPUT_GAMEPAD_A);
  CHECK(f.physical->polls > polls);
  CHECK(f.fresh_a_seen == 1);
  CHECK(f.closed == 1);
  CHECK(f.chosen == expected);
}
TEST_CASE("Guide and B-only menu handoff accepts a fresh message A",
          "[ui][input][controller_modality][message_box]") {
  Fixture f;
  f.physical->Set(hid::X_INPUT_GAMEPAD_GUIDE);
  f.Menu();
  f.Frames();
  f.physical->Set(hid::X_INPUT_GAMEPAD_B);
  // The Window resolves B dismissal before forwarding menu navigation. The
  // external HandleGamepadInput path deliberately disallows B dismissal.
  f.menu->CloseMenu();
  REQUIRE(f.menu == nullptr);
  f.physical->Set(0);
  f.Message();
  f.Frames();
  REQUIRE(f.closed == 0);
  CHECK_FALSE(ImGui::GetCurrentContext()->NavCursorVisible);
  f.Pulse(hid::X_INPUT_GAMEPAD_A);
  CHECK(f.fresh_a_seen == 1);
  CHECK(f.closed == 1);
  CHECK(f.chosen == 0);
}
TEST_CASE("Controller navigation keeps its selected message button",
          "[ui][input][controller_modality][message_box]") {
  Fixture f;
  f.Message({"Back", "OK", "More"}, 1);
  f.Frames();
  f.Pulse(hid::X_INPUT_GAMEPAD_DPAD_LEFT);
  REQUIRE(f.closed == 0);
  f.Pulse(hid::X_INPUT_GAMEPAD_A);
  CHECK(f.closed == 1);
  CHECK(f.chosen == 0);  // Do not reapply default index1 on every A press.
}
TEST_CASE("Fresh controller activation resumes after actual mouse title focus",
          "[ui][input][controller_modality][message_box]") {
  Fixture f;
  f.Message();
  f.Frames();
  f.Pulse(hid::X_INPUT_GAMEPAD_DPAD_RIGHT);
  REQUIRE(ImGui::GetCurrentContext()->NavCursorVisible);
  auto* nav = ImGui::GetCurrentContext()->NavWindow;
  REQUIRE(nav);
  auto& io = f.drawer.GetIO();
  io.AddMousePosEvent(nav->Pos.x + 8, nav->Pos.y + 8);
  io.AddMouseButtonEvent(0, true);
  f.Frame();
  io.AddMouseButtonEvent(0, false);
  f.Frames();
  REQUIRE(f.closed == 0);
  CHECK_FALSE(ImGui::GetCurrentContext()->NavCursorVisible);
  f.Pulse(hid::X_INPUT_GAMEPAD_A);
  CHECK(f.closed == 1);
  CHECK(f.chosen == 0);
}
TEST_CASE("Held menu A cannot accept the following message before a new press",
          "[ui][input][controller_modality][message_box]") {
  Fixture f;
  f.Menu([&] { f.Message(); });
  f.Frames();
  f.physical->Set(hid::X_INPUT_GAMEPAD_A);
  f.ExternalMenuEdge(hid::X_INPUT_GAMEPAD_A);
  f.context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(f.message);
  f.Frames(40);
  CHECK(f.closed == 0);
  REQUIRE(f.message);
  f.physical->Set(0);
  f.Frames();
  f.Pulse(hid::X_INPUT_GAMEPAD_A);
  CHECK(f.closed == 1);
  CHECK(f.chosen == 0);
}
TEST_CASE("Shared controller mapper preserves text entry's single A owner",
          "[ui][input][controller_modality][text_entry]") {
  struct Completion {
    unsigned count = 0;
    TextEntryResult result = TextEntryResult::kAborted;
    std::u16string text;
  };
  auto completion = std::make_shared<Completion>();
  Fixture f;
  f.entry = new ImGuiTextEntryDialog(
      &f.drawer, &f.input, "Host keyboard", "Body", u"keep", 8,
      [completion](TextEntryResult value, const std::u16string& written) {
        ++completion->count;
        completion->result = value;
        completion->text = written;
      });
  f.Frames(4);
  f.physical->Set(hid::X_INPUT_GAMEPAD_A);
  f.Frames(30);
  CHECK(completion->count ==
        0);  // A is a grid character, not InputText Enter/Done.
  REQUIRE(f.entry);
  f.physical->Set(0);
  f.Frames();
  f.Pulse(hid::X_INPUT_GAMEPAD_START);
  f.context.ExecutePendingFunctionsFromUIThread();
  CHECK(completion->count == 1);
  CHECK(completion->result == TextEntryResult::kAccepted);
  CHECK(completion->text ==
        u"keepq");  // Actual default host grid character q, exactly once.
}
}  // namespace
}  // namespace xe::ui::test
