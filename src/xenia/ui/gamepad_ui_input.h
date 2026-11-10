/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GAMEPAD_UI_INPUT_H_
#define XENIA_UI_GAMEPAD_UI_INPUT_H_

#include <array>
#include <cstdint>

#include "xenia/hid/input.h"
#include "xenia/xbox.h"

namespace xe::ui {

struct GamepadUiControllerInput {
  uint16_t buttons = 0;
  uint16_t pressed_buttons = 0;
  uint16_t menu_navigation = 0;
  uint16_t navigate = 0;
  bool activate = false;
};

struct GamepadUiState {
  bool context_menu_open = false;
  bool other_dialog_open = false;
  bool title_open = false;
  bool guide_enabled = true;
};

enum class GamepadUiAction {
  kNone,
  kOpenContextMenu,
  kCloseContextMenu,
  kContextMenuInput,
  kLibraryInput,
};

// Resolve one UI action for the entire poll, so several controllers pressing
// Guide together cannot close a menu that the same poll just opened.
constexpr GamepadUiAction GetGamepadUiAction(
    const GamepadUiState& state,
    const std::array<GamepadUiControllerInput, XUserMaxUserCount>&
        controllers) {
  if (state.other_dialog_open) {
    return GamepadUiAction::kNone;
  }

  if (state.context_menu_open) {
    constexpr uint16_t kDismissButtons = hid::X_INPUT_GAMEPAD_B |
                                         hid::X_INPUT_GAMEPAD_BACK |
                                         hid::X_INPUT_GAMEPAD_GUIDE;
    for (const auto& controller : controllers) {
      if (controller.pressed_buttons & kDismissButtons) {
        return GamepadUiAction::kCloseContextMenu;
      }
    }
    constexpr uint16_t kMenuDirections =
        hid::X_INPUT_GAMEPAD_DPAD_UP | hid::X_INPUT_GAMEPAD_DPAD_DOWN;
    for (const auto& controller : controllers) {
      if ((controller.pressed_buttons &
           (kMenuDirections | hid::X_INPUT_GAMEPAD_A)) ||
          (controller.menu_navigation & kMenuDirections)) {
        return GamepadUiAction::kContextMenuInput;
      }
    }
    return GamepadUiAction::kNone;
  }

  if (state.guide_enabled) {
    for (const auto& controller : controllers) {
      if (controller.buttons == hid::X_INPUT_GAMEPAD_GUIDE &&
          (controller.pressed_buttons & hid::X_INPUT_GAMEPAD_GUIDE)) {
        return GamepadUiAction::kOpenContextMenu;
      }
    }
  }

  if (!state.title_open) {
    for (const auto& controller : controllers) {
      if (controller.navigate || controller.activate) {
        return GamepadUiAction::kLibraryInput;
      }
    }
  }

  return GamepadUiAction::kNone;
}

}  // namespace xe::ui

#endif  // XENIA_UI_GAMEPAD_UI_INPUT_H_
