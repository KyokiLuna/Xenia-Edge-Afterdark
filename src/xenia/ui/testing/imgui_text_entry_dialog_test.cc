/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <memory>

#include "third_party/catch/include/catch.hpp"
#include "third_party/imgui/imgui_internal.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/imgui_text_entry_dialog.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe::ui::test {
namespace {
class KeyboardContext final : public WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
class KeyboardWindow final : public Window {
 public:
  explicit KeyboardWindow(WindowedAppContext& context)
      : Window(context, "Keyboard test", 1280, 720) {}
  ~KeyboardWindow() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override { return false; }
  void RequestCloseImpl() override {}
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};
class KeyboardDriver final : public hid::InputDriver {
 public:
  KeyboardDriver() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  hid::InputType GetInputType() const override { return hid::Controller; }
  X_RESULT GetCapabilities(uint32_t slot, uint32_t,
                           hid::X_INPUT_CAPABILITIES* caps) override {
    if (slot >= 2) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *caps = {};
    caps->type = hid::XINPUT_DEVTYPE_GAMEPAD;
    caps->sub_type = hid::XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->gamepad.buttons = 0xFFFF;
    caps->vibration.left_motor_speed = 1;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t slot, hid::X_INPUT_STATE* state) override {
    if (slot >= 2) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *state = {};
    state->gamepad.buttons = buttons[slot];
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t, hid::X_INPUT_VIBRATION*) override {
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, hid::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }
  std::array<uint16_t, 2> buttons{};
};

class KeyboardFixture {
 public:
  KeyboardFixture() : window(context), input(nullptr), drawer(&window, 0) {
    auto driver_ptr = std::make_unique<KeyboardDriver>();
    driver = driver_ptr.get();
    input.AddDriver(std::move(driver_ptr));
    for (uint32_t i = 0; i < 2; ++i) {
      input.BindSlot(i, driver, uint8_t(i), {}, {});
    }
    auto& io = drawer.GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  }
  ~KeyboardFixture() {
    drawer.ClearDialogs();
    context.ExecutePendingFunctionsFromUIThread();
  }
  ImGuiTextEntryDialog* Open(std::u16string text = {}, size_t capacity = 32) {
    auto* dialog = new ImGuiTextEntryDialog(
        &drawer, &input, "Text entry", "", std::move(text), capacity,
        [this](TextEntryResult result, const std::u16string& text) {
          ++completions;
          accepted = result == TextEntryResult::kAccepted;
          last_result = result;
          value = text;
        });
    dialog->SetInputOwnershipCallback([this] { return native_owns_input; });
    return dialog;
  }
  void Draw(ImGuiDialog* dialog) {
    drawer.GetIO();
    drawer.RefreshInputCaptureState();
    ImGui::NewFrame();
    dialog->Draw();
    ImGui::Render();
    drawer.RefreshInputCaptureState();
    context.ExecutePendingFunctionsFromUIThread();
  }
  void Press(ImGuiDialog* dialog, uint16_t buttons) {
    driver->buttons[1] = 0;
    Draw(dialog);
    driver->buttons[1] = buttons;
    Draw(dialog);
  }

  KeyboardContext context;
  KeyboardWindow window;
  hid::InputSystem input;
  ImGuiDrawer drawer;
  KeyboardDriver* driver;
  unsigned int completions = 0;
  bool accepted = false;
  TextEntryResult last_result = TextEntryResult::kAborted;
  bool native_owns_input = true;
  std::u16string value;
};

TEST_CASE("Controller keyboard seeds held buttons and accepts another slot",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  fixture.driver->buttons[0] = hid::X_INPUT_GAMEPAD_X;
  fixture.driver->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  auto* dialog = fixture.Open(u"a");
  fixture.Draw(dialog);
  for (unsigned int i = 0; i < 5; ++i) {
    fixture.Draw(dialog);
  }
  REQUIRE(fixture.completions == 0);
  // The held opening A is not q, and the held first-pad X cannot starve pad1.
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_A);
  for (unsigned int i = 0; i < 5; ++i) {
    fixture.Draw(dialog);
  }
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_Y);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_DPAD_RIGHT);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_A);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(fixture.completions == 1);
  CHECK(fixture.accepted);
  CHECK(fixture.value == u"aqW");
  CHECK_FALSE(fixture.drawer.HasOpenDialogs());
  CHECK_FALSE(fixture.input.IsUIInputBlocked());
  CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
}

TEST_CASE("Raw A types one key while ImGui InputText is active",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  auto* dialog = fixture.Open();
  fixture.Draw(dialog);
  fixture.Draw(dialog);
  auto& io = fixture.drawer.GetIO();
  REQUIRE(ImGui::GetActiveID() != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
  fixture.driver->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(dialog);
  REQUIRE(fixture.completions == 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(fixture.completions == 1);
  CHECK(fixture.value == u"q");
}

TEST_CASE("Controller delete removes a full UTF-16 surrogate pair",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  auto* dialog = fixture.Open(u"a\U0001F642", 3);
  fixture.Draw(dialog);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_A);  // Full: q is refused.
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_X);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_A);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(fixture.accepted);
  CHECK(fixture.value == u"aq");
}

TEST_CASE("Keyboard cancellation wins simultaneous accept and teardown once",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  TextEntryResult expected = TextEntryResult::kAborted;
  SECTION("B wins Start and A across slots") {
    expected = TextEntryResult::kCanceled;
    auto* dialog = fixture.Open(u"default");
    fixture.Draw(dialog);
    fixture.driver->buttons[0] = hid::X_INPUT_GAMEPAD_START;
    fixture.driver->buttons[1] =
        hid::X_INPUT_GAMEPAD_B | hid::X_INPUT_GAMEPAD_A;
    fixture.Draw(dialog);
  }
  SECTION("Clear before first draw cancels") {
    fixture.Open(u"default");
    fixture.drawer.ClearDialogs();
  }
  SECTION("Clear after popup draw cancels") {
    auto* dialog = fixture.Open(u"default");
    fixture.Draw(dialog);
    fixture.drawer.ClearDialogs();
  }
  CHECK(fixture.completions == 1);
  CHECK_FALSE(fixture.accepted);
  CHECK(fixture.last_result == expected);
  CHECK(fixture.value == u"default");
  CHECK_FALSE(fixture.drawer.HasOpenDialogs());
  CHECK_FALSE(fixture.input.IsUIInputBlocked());
  CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
}

TEST_CASE("Keyboard ownership loss consumes held inputs across a modal",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  auto* dialog = fixture.Open();
  fixture.Draw(dialog);
  fixture.native_owns_input = false;
  fixture.driver->buttons[1] = hid::X_INPUT_GAMEPAD_A;
  fixture.Draw(dialog);
  fixture.native_owns_input = true;
  fixture.Draw(dialog);
  REQUIRE(fixture.completions == 0);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(fixture.value.empty());
}

TEST_CASE(
    "Keyboard completion may clear dialogs without destroying itself twice",
    "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  unsigned int completions = 0;
  bool expected_acceptance = false;
  auto* dialog = new ImGuiTextEntryDialog(
      &fixture.drawer, &fixture.input, "Text entry", "", u"kept", 16,
      [&](TextEntryResult result, const std::u16string& text) {
        CHECK((result == TextEntryResult::kAccepted) == expected_acceptance);
        CHECK(text == u"kept");
        CHECK_FALSE(fixture.input.IsUIInputBlocked());
        CHECK_FALSE(ImGui::GetCurrentContext()->WithinFrameScope);
        ++completions;
        fixture.drawer.ClearDialogs();
      });
  fixture.Draw(dialog);
  SECTION("External destruction completes inline") { dialog->Destroy(); }
  SECTION("Draw completion defers destructive navigation") {
    expected_acceptance = true;
    fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  }
  CHECK(completions == 1);
  CHECK_FALSE(fixture.drawer.HasOpenDialogs());
  CHECK_FALSE(fixture.input.IsUIInputBlocked());
}

TEST_CASE("Physical keyboard text is limited by UTF-16 units",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  auto* dialog = fixture.Open({}, 2);
  fixture.Draw(dialog);
  fixture.Draw(dialog);
  auto& io = fixture.drawer.GetIO();
  io.AddInputCharactersUTF8("\xE6\x97\xA5\xE6\x9C\xAC");
  fixture.Draw(dialog);
  io.AddInputCharacter('x');
  fixture.Draw(dialog);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(fixture.accepted);
  CHECK(fixture.value == u"\u65E5\u672C");
}

TEST_CASE("Physical typing continues after controller text edits",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  auto* dialog = fixture.Open();
  fixture.Draw(dialog);
  fixture.Draw(dialog);
  auto& io = fixture.drawer.GetIO();
  io.AddInputCharacter('a');
  fixture.Draw(dialog);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_A);
  fixture.Draw(dialog);  // The text widget reloads the controller's edit.
  REQUIRE(ImGui::GetActiveID() != 0);
  auto& edit = ImGui::GetCurrentContext()->InputTextState;
  CHECK(std::string(edit.TextA.Data) == "aq");
  io.AddInputCharacter('b');
  fixture.Draw(dialog);
  CHECK(std::string(edit.TextA.Data) == "aqb");
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_X);
  fixture.Draw(dialog);
  CHECK(std::string(edit.TextA.Data) == "aq");
  io.AddInputCharacter('c');
  fixture.Draw(dialog);
  CHECK(std::string(edit.TextA.Data) == "aqc");
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(fixture.accepted);
  CHECK(fixture.value == u"aqc");
}
TEST_CASE(
    "Controller acceptance includes physical text queued in the same frame",
    "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  auto* dialog = fixture.Open();
  fixture.Draw(dialog);
  fixture.Draw(dialog);
  fixture.drawer.GetIO().AddInputCharacter('x');
  fixture.driver->buttons[1] = hid::X_INPUT_GAMEPAD_START;
  fixture.Draw(dialog);
  CHECK(fixture.completions == 1);
  CHECK(fixture.accepted);
  CHECK(fixture.value == u"x");
}
TEST_CASE("Physical modifier events select keyboard text across idle frames",
          "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  WindowInputListener& keyboard = fixture.drawer;
  auto* dialog = fixture.Open(u"original");
  fixture.Draw(dialog);
  fixture.Draw(dialog);
  KeyEvent ctrl(&fixture.window, VirtualKey::kControl, 1, false, false, true,
                false, false);
  keyboard.OnKeyDown(ctrl);
  fixture.Draw(dialog);
  for (unsigned int i = 0; i < 5; ++i) {
    fixture.Draw(dialog);
  }
  REQUIRE(fixture.drawer.GetIO().KeyCtrl);
  KeyEvent a_down(&fixture.window, VirtualKey::kA, 1, false, false, true, false,
                  false);
  keyboard.OnKeyDown(a_down);
  fixture.Draw(dialog);
  KeyEvent a_up(&fixture.window, VirtualKey::kA, 1, true, false, true, false,
                false);
  keyboard.OnKeyUp(a_up);
  fixture.Draw(dialog);
  KeyEvent ctrl_up(&fixture.window, VirtualKey::kControl, 1, true, false, false,
                   false, false);
  keyboard.OnKeyUp(ctrl_up);
  fixture.Draw(dialog);
  REQUIRE_FALSE(fixture.drawer.GetIO().KeyCtrl);
  KeyEvent character(&fixture.window, static_cast<VirtualKey>('z'), 1, false,
                     false, false, false, false);
  keyboard.OnKeyChar(character);
  fixture.Draw(dialog);
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(fixture.completions == 1);
  CHECK(fixture.accepted);
  CHECK(fixture.value == u"z");
}

TEST_CASE(
    "Rejected deferred keyboard completion uses only the safe shutdown sink",
    "[ui][input][keyboard]") {
  KeyboardFixture fixture;
  unsigned int host_continuations = 0;
  unsigned int shutdown_completions = 0;
  xe::threading::Fence finished;
  auto* dialog = new ImGuiTextEntryDialog(
      &fixture.drawer, &fixture.input, "Text entry", "", u"kept", 16,
      [&](TextEntryResult, const std::u16string&) {
        ++host_continuations;
        fixture.drawer.ClearDialogs();
      },
      [&](TextEntryResult result, const std::u16string& text) {
        CHECK(result == TextEntryResult::kAborted);
        CHECK(text == u"kept");
        ++shutdown_completions;
        finished.Signal();
      });
  fixture.Draw(dialog);
  fixture.context.QuitFromUIThread();
  fixture.Press(dialog, hid::X_INPUT_GAMEPAD_START);
  CHECK(host_continuations == 0);
  CHECK(shutdown_completions == 1);
  CHECK(finished.TryWait());
  CHECK_FALSE(fixture.drawer.HasOpenDialogs());
  CHECK_FALSE(fixture.input.IsUIInputBlocked());
}
}  // namespace
}  // namespace xe::ui::test
