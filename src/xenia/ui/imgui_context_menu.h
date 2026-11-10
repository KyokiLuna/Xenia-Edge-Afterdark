/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_CONTEXT_MENU_H_
#define XENIA_UI_IMGUI_CONTEXT_MENU_H_

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "xenia/base/native_ui_backend_route.h"
#include "xenia/ui/gamepad_ui_input.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/xbox.h"

namespace xe {
namespace hid {
class InputSystem;
}  // namespace hid
}  // namespace xe

namespace xe {
namespace ui {

// ImGui-based context menu with gamepad support.
// This is a modal popup that blocks game input while open.
class ImGuiContextMenu : public ImGuiDialog {
 public:
  struct MenuItem {
    std::string text;
    std::string shortcut;
    std::function<void()> callback;
    bool is_separator;
  };

  // Creates a context menu. If input_system is provided, gamepad input
  // will be supported and game input will be blocked while menu is open.
  // Set poll_gamepad=false when HandleGamepadInput supplies all gamepad input.
  ImGuiContextMenu(
      ImGuiDrawer* drawer, hid::InputSystem* input_system,
      bool poll_gamepad = true,
      std::shared_ptr<NativeUiBackendRoute::HostLease> lifetime = {},
      std::function<void(bool)> visibility = {});
  ~ImGuiContextMenu() override;

  // Add a menu item with optional keyboard shortcut hint
  void AddAction(const std::string& text, std::function<void()> callback,
                 const std::string& shortcut = "");

  // Add a visual separator line
  void AddSeparator();

  // Show the menu centered on screen
  void Show();

  // Show the menu at a specific position (in screen coordinates)
  void ShowAt(float x, float y);

  // Close on the UI thread without waiting for another rendered frame.
  void CloseMenu() { Destroy(); }

  // UI-thread edge samples from an external poller. May close/delete this menu
  // on activation, queuing its action without waiting for another draw. The
  // caller resolves dismissal first and must not reuse the pointer afterward.
  void HandleGamepadInput(
      const std::array<GamepadUiControllerInput, XUserMaxUserCount>& inputs);

  // Set a callback to be invoked when the menu closes
  void SetOnCloseCallback(std::function<void()> callback) {
    on_close_callback_ = std::move(callback);
  }

  // Native modal ownership is supplied by the window's UI thread. ImGui
  // dialog/popup ownership is checked by the menu itself.
  void SetInputOwnershipCallback(std::function<bool()> callback) {
    input_ownership_callback_ = std::move(callback);
  }

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  bool HasInputOwnership() const;
  void PollGamepad(bool owns_input);
  void ApplyGamepadInput(
      const std::array<GamepadUiControllerInput, XUserMaxUserCount>& inputs,
      bool owns_input, bool allow_dismissal);
  void ActivateItem(int index);
  int GetNextSelectableItem(int current, int direction);

  std::shared_ptr<NativeUiBackendRoute::HostLease> lifetime_;
  std::function<void(bool)> visibility_;
  std::vector<MenuItem> items_;
  hid::InputSystem* input_system_;
  std::function<void()> on_close_callback_;
  std::function<void()> pending_callback_;  // Callback to execute after close
  std::function<bool()> input_ownership_callback_;
  int focused_index_ = 0;
  std::array<uint16_t, XUserMaxUserCount> prev_buttons_{};
  std::array<uint16_t, XUserMaxUserCount> consumed_buttons_{};
  bool poll_gamepad_;
  uint64_t input_capture_generation_;
  uint32_t popup_id_ = 0;
  bool has_opened_ = false;
  bool activation_pending_ = false;
  bool center_on_screen_ = true;
  float position_x_ = 0;
  float position_y_ = 0;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_CONTEXT_MENU_H_
