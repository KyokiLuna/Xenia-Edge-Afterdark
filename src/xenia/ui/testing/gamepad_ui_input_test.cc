/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/gamepad_ui_input.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::ui::test {
namespace {

using Controllers = std::array<GamepadUiControllerInput, XUserMaxUserCount>;

TEST_CASE("Guide actions accept input from any controller slot",
          "[ui][input]") {
  GamepadUiState state;
  Controllers controllers{};
  controllers[1].buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  controllers[1].pressed_buttons = hid::X_INPUT_GAMEPAD_GUIDE;

  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kOpenContextMenu);

  state.context_menu_open = true;
  for (uint16_t button : {hid::X_INPUT_GAMEPAD_B, hid::X_INPUT_GAMEPAD_BACK,
                          hid::X_INPUT_GAMEPAD_GUIDE}) {
    controllers[1].buttons = button;
    controllers[1].pressed_buttons = button;
    CHECK(GetGamepadUiAction(state, controllers) ==
          GamepadUiAction::kCloseContextMenu);
  }
}

TEST_CASE("Held Guide cannot toggle the context menu again", "[ui][input]") {
  GamepadUiState state;
  Controllers controllers{};
  controllers[0].buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  controllers[0].pressed_buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  REQUIRE(GetGamepadUiAction(state, controllers) ==
          GamepadUiAction::kOpenContextMenu);

  state.context_menu_open = true;
  controllers[0].pressed_buttons = 0;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);

  // Releasing a different button while Guide remains held is not a Guide edge.
  state.context_menu_open = false;
  controllers[0].buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
}

TEST_CASE("Disabling Guide opening does not trap an open menu", "[ui][input]") {
  GamepadUiState state;
  state.guide_enabled = false;
  Controllers controllers{};
  controllers[0].buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  controllers[0].pressed_buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);

  state.context_menu_open = true;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kCloseContextMenu);
}

TEST_CASE("Another modal captures Guide and library input", "[ui][input]") {
  GamepadUiState state;
  state.other_dialog_open = true;
  Controllers controllers{};
  controllers[0].buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  controllers[0].pressed_buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  controllers[1].navigate = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  controllers[1].activate = true;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);

  state.context_menu_open = true;
  controllers[0].pressed_buttons = hid::X_INPUT_GAMEPAD_B;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
}

TEST_CASE("Library navigation is captured by the current title or menu",
          "[ui][input]") {
  GamepadUiState state;
  Controllers controllers{};
  controllers[1].navigate = hid::X_INPUT_GAMEPAD_DPAD_LEFT;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kLibraryInput);

  controllers[1].navigate = 0;
  controllers[1].activate = true;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kLibraryInput);

  state.title_open = true;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
  state.title_open = false;
  state.context_menu_open = true;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
}

TEST_CASE("Guide takes priority over another controller's library action",
          "[ui][input]") {
  GamepadUiState state;
  Controllers controllers{};
  controllers[0].activate = true;
  controllers[0].navigate = hid::X_INPUT_GAMEPAD_DPAD_RIGHT;
  controllers[1].buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  controllers[1].pressed_buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kOpenContextMenu);
}

TEST_CASE("Several Guide edges resolve to one action per poll", "[ui][input]") {
  GamepadUiState state;
  Controllers controllers{};
  for (auto& controller : controllers) {
    controller.buttons = hid::X_INPUT_GAMEPAD_GUIDE;
    controller.pressed_buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  }
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kOpenContextMenu);
  state.context_menu_open = true;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kCloseContextMenu);
}

TEST_CASE("Menu edges and repeats never become library commands",
          "[ui][input]") {
  Controllers controllers{};
  controllers[1].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  GamepadUiState state;
  // This was sampled in a title, so no library activation was authorized even
  // if the title has ended by delivery time.
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
  state.context_menu_open = true;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kContextMenuInput);
  controllers[1].pressed_buttons = 0;
  controllers[1].menu_navigation = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kContextMenuInput);
  state.context_menu_open = false;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
}

TEST_CASE("Menu dismissal wins over simultaneous activation or navigation",
          "[ui][input]") {
  Controllers controllers{};
  controllers[0].pressed_buttons = hid::X_INPUT_GAMEPAD_A;
  controllers[0].menu_navigation = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
  controllers[1].pressed_buttons = hid::X_INPUT_GAMEPAD_B;
  GamepadUiState state;
  state.context_menu_open = true;
  CHECK(GetGamepadUiAction(state, controllers) ==
        GamepadUiAction::kCloseContextMenu);
  state.other_dialog_open = true;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
}

TEST_CASE("A Guide chord cannot open the context menu", "[ui][input]") {
  GamepadUiState state;
  Controllers controllers{};
  controllers[0].buttons = hid::X_INPUT_GAMEPAD_GUIDE | hid::X_INPUT_GAMEPAD_A;
  controllers[0].pressed_buttons = hid::X_INPUT_GAMEPAD_GUIDE;
  CHECK(GetGamepadUiAction(state, controllers) == GamepadUiAction::kNone);
}

}  // namespace
}  // namespace xe::ui::test
