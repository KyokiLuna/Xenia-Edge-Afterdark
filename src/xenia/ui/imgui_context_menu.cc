/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_context_menu.h"

#include <algorithm>
#include <cfloat>

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"

namespace xe {
namespace ui {

ImGuiContextMenu::ImGuiContextMenu(
    ImGuiDrawer* drawer, hid::InputSystem* input_system, bool poll_gamepad,
    std::shared_ptr<NativeUiBackendRoute::HostLease> lifetime,
    std::function<void(bool)> visibility)
    : ImGuiDialog(drawer),
      lifetime_(std::move(lifetime)),
      visibility_(std::move(visibility)),
      input_system_(input_system),
      poll_gamepad_(poll_gamepad),
      input_capture_generation_(drawer->input_capture_generation()) {
  if (input_system_) {
    input_system_->AddUIInputBlocker();

    // Initialize prev_buttons_ to current state so held buttons aren't
    // detected as "just pressed" when the menu opens
    hid::X_INPUT_STATE state;
    for (uint32_t user_index = 0; user_index < XUserMaxUserCount;
         user_index++) {
      if (input_system_->GetStateForUI(user_index, 1, &state) == 0) {
        prev_buttons_[user_index] = state.gamepad.buttons;
        consumed_buttons_[user_index] = state.gamepad.buttons;
      }
    }
  }
  if (visibility_) {
    visibility_(true);
  }
}

ImGuiContextMenu::~ImGuiContextMenu() {
  if (input_system_) {
    input_system_->RemoveUIInputBlocker();
  }
  if (visibility_) {
    visibility_(false);
  }
}

void ImGuiContextMenu::OnClose() {
  // CloseMenu also runs between frames. CloseCurrentPopup needs an active
  // BeginPopup scope, so remove only this menu's popup from its drawer context.
  // Leaving its modal entry alive would continue to capture ImGui input.
  if (popup_id_) {
    GetIO();
    auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
    for (int i = 0; i < popups.Size; ++i) {
      if (popups[i].PopupId == popup_id_) {
        ImGui::ClosePopupToLevel(i, true);
        break;
      }
    }
    popup_id_ = 0;
  }
  if (on_close_callback_) {
    on_close_callback_();
  }
  // Defer action callback to execute after the current frame completes
  // This prevents issues with operations like fullscreen that need the
  // ImGui state to be fully cleaned up first
  if (pending_callback_) {
    if (lifetime_) {
      imgui_drawer()->PostDeferredCallback(
          [owner = lifetime_,
           callback = std::move(pending_callback_)]() mutable {
            auto entered = owner->TryEnter();
            if (!entered) {
              return;
            }
            // All callback-local captured owners die before the route entry.
            auto run = std::move(callback);
            run();
          });
    } else {
      imgui_drawer()->PostDeferredCallback(std::move(pending_callback_));
    }
  }
}

void ImGuiContextMenu::AddAction(const std::string& text,
                                 std::function<void()> callback,
                                 const std::string& shortcut) {
  items_.push_back({text, shortcut, callback, false});
}

void ImGuiContextMenu::AddSeparator() {
  items_.push_back({"", "", nullptr, true});
}

void ImGuiContextMenu::Show() {
  center_on_screen_ = true;
  // Find first selectable item
  focused_index_ = GetNextSelectableItem(-1, 1);
}

void ImGuiContextMenu::ShowAt(float x, float y) {
  center_on_screen_ = false;
  position_x_ = x;
  position_y_ = y;
  // Find first selectable item
  focused_index_ = GetNextSelectableItem(-1, 1);
}

int ImGuiContextMenu::GetNextSelectableItem(int current, int direction) {
  if (items_.empty()) {
    return -1;
  }

  int count = static_cast<int>(items_.size());
  int next = current;

  for (int i = 0; i < count; i++) {
    next += direction;
    if (next < 0) {
      next = count - 1;
    }
    if (next >= count) {
      next = 0;
    }

    if (!items_[next].is_separator) {
      return next;
    }
  }

  return current;  // No selectable items found
}

bool ImGuiContextMenu::HasInputOwnership() const {
  if (!imgui_drawer()->IsOnlyDialog(this) ||
      (input_ownership_callback_ && !input_ownership_callback_())) {
    return false;
  }
  const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
  if (popups.empty()) {
    return !has_opened_;
  }
  return popup_id_ && popups.back().PopupId == popup_id_;
}

void ImGuiContextMenu::PollGamepad(bool owns_input) {
  if (!input_system_) {
    return;
  }

  std::array<GamepadUiControllerInput, XUserMaxUserCount> inputs{};
  for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
    hid::X_INPUT_STATE state;
    if (input_system_->GetStateForUI(i, 1, &state) == 0) {
      uint16_t buttons = state.gamepad.buttons;
      inputs[i].buttons = buttons;
      inputs[i].pressed_buttons = buttons & ~prev_buttons_[i];
      prev_buttons_[i] = buttons;
    } else {
      prev_buttons_[i] = 0;
    }
  }
  ApplyGamepadInput(inputs, owns_input, true);
}

void ImGuiContextMenu::HandleGamepadInput(
    const std::array<GamepadUiControllerInput, XUserMaxUserCount>& inputs) {
  GetIO();  // External samples need this drawer's ImGui context too.
  const uint64_t capture_generation =
      imgui_drawer()->input_capture_generation();
  if (input_capture_generation_ != capture_generation) {
    for (size_t i = 0; i < inputs.size(); ++i) {
      consumed_buttons_[i] |= inputs[i].buttons & ~inputs[i].pressed_buttons;
    }
    input_capture_generation_ = capture_generation;
  }
  ApplyGamepadInput(inputs, HasInputOwnership(), false);
  if (activation_pending_) {
    Destroy();
  }
}

void ImGuiContextMenu::ApplyGamepadInput(
    const std::array<GamepadUiControllerInput, XUserMaxUserCount>& inputs,
    bool owns_input, bool allow_dismissal) {
  for (size_t i = 0; i < inputs.size(); ++i) {
    consumed_buttons_[i] &= inputs[i].buttons;
    if (!owns_input) {
      consumed_buttons_[i] |= inputs[i].buttons;
    } else {
      // A fresh edge proves a release happened even when the external poller
      // did not deliver an idle/release-only sample to this menu.
      consumed_buttons_[i] &= ~inputs[i].pressed_buttons;
    }
  }
  if (!owns_input || items_.empty()) {
    return;
  }
  for (const auto& input : inputs) {
    if (input.pressed_buttons &
        (hid::X_INPUT_GAMEPAD_B | hid::X_INPUT_GAMEPAD_BACK |
         hid::X_INPUT_GAMEPAD_GUIDE)) {
      if (allow_dismissal) {
        Close();
      }
      return;
    }
  }
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& input = inputs[i];
    const uint16_t pressed = input.pressed_buttons & ~consumed_buttons_[i];
    const uint16_t navigate =
        (pressed | input.menu_navigation | input.navigate) &
        ~consumed_buttons_[i];
    if ((navigate &
         (hid::X_INPUT_GAMEPAD_DPAD_UP | hid::X_INPUT_GAMEPAD_DPAD_DOWN)) ||
        (pressed & hid::X_INPUT_GAMEPAD_A)) {
      // External controller edges may never reach an ImGui frame. Preserve
      // controller modality for a following dialog without giving ImGui a
      // second selection owner in this menu.
      ImGui::SetNavCursorVisible(true);
    }
    // D-pad navigation
    if (navigate & hid::X_INPUT_GAMEPAD_DPAD_UP) {
      focused_index_ = GetNextSelectableItem(focused_index_, -1);
    }
    if (navigate & hid::X_INPUT_GAMEPAD_DPAD_DOWN) {
      focused_index_ = GetNextSelectableItem(focused_index_, 1);
    }

    // A button to activate focused item
    if (pressed & hid::X_INPUT_GAMEPAD_A) {
      ActivateItem(focused_index_);
      return;
    }
  }
}

void ImGuiContextMenu::ActivateItem(int index) {
  if (activation_pending_) {
    return;  // Already activating an item
  }
  if (index >= 0 && index < static_cast<int>(items_.size()) &&
      !items_[index].is_separator && items_[index].callback) {
    activation_pending_ = true;
    // Store callback to execute after dialog is fully closed
    pending_callback_ = items_[index].callback;
    Close();
  }
}

void ImGuiContextMenu::OnDraw(ImGuiIO& io) {
  // Open popup on first draw
  if (!has_opened_ && HasInputOwnership()) {
    popup_id_ = ImGui::GetID("##ContextMenu");
    ImGui::OpenPopup("##ContextMenu");
    has_opened_ = true;
  }

  const bool owns_input = has_opened_ && HasInputOwnership();
  if (poll_gamepad_) {
    PollGamepad(owns_input);
  }
  if (!has_opened_) {
    return;
  }

  // Style the popup - white background, black text
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

  // Position the popup
  if (center_on_screen_) {
    ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  } else {
    ImGui::SetNextWindowPos(ImVec2(position_x_, position_y_),
                            ImGuiCond_Appearing);
  }

  // Calculate required width for menu items
  float max_text_width = 0.0f;
  float max_shortcut_width = 0.0f;
  for (const auto& item : items_) {
    if (!item.is_separator) {
      float text_width = ImGui::CalcTextSize(item.text.c_str()).x;
      max_text_width = std::max(max_text_width, text_width);
      if (!item.shortcut.empty()) {
        float shortcut_width = ImGui::CalcTextSize(item.shortcut.c_str()).x;
        max_shortcut_width = std::max(max_shortcut_width, shortcut_width);
      }
    }
  }
  float padding = ImGui::GetStyle().WindowPadding.x;
  float item_spacing = ImGui::GetStyle().ItemSpacing.x;
  float gap = item_spacing * 3.0f;  // Gap between text and shortcut
  float min_width = max_text_width + gap + max_shortcut_width + padding * 2;

  bool is_open = true;
  if (ImGui::BeginPopupModal("##ContextMenu", &is_open,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove)) {
    ImGui::BeginDisabled(!owns_input);
    // Handle keyboard input
    if (owns_input && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
      Close();
    }
    if (owns_input && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      focused_index_ = GetNextSelectableItem(focused_index_, -1);
    }
    if (owns_input && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      focused_index_ = GetNextSelectableItem(focused_index_, 1);
    }
    if (owns_input && (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                       ImGui::IsKeyPressed(ImGuiKey_Space))) {
      ActivateItem(focused_index_);
    }

    // Custom controller/keyboard selection is the only navigation owner.
    // Keep the window navigation-capable so ImGui doesn't hide its global
    // navigation cursor before a deferred action opens another dialog.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    // Draw menu items
    for (int i = 0; i < static_cast<int>(items_.size()); i++) {
      const auto& item = items_[i];

      if (item.is_separator) {
        ImGui::PushStyleColor(ImGuiCol_Separator,
                              ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        continue;
      }

      bool is_focused = (i == focused_index_);

      // Item background - Xbox green (#107C10) for highlight
      const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
      if (is_focused) {
        ImGui::PushStyleColor(ImGuiCol_Header, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
      }

      // Create selectable item with minimum width
      ImGui::PushID(i);
      const bool clicked =
          ImGui::Selectable("##item", is_focused,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_NoAutoClosePopups,
                            ImVec2(min_width, 0));
      if (clicked && owns_input &&
          ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        ActivateItem(i);
      }

      // Hover must represent fresh pointer movement, not a parked cursor or
      // ImGui's older navigation ID. Otherwise it overwrites the controller /
      // keyboard selection again on every draw before that choice activates.
      if (owns_input && !io.WantSetMousePos &&
          (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride)) {
        focused_index_ = i;
      }

      // Draw text on same line
      ImGui::SameLine();
      ImGui::SetCursorPosX(padding);
      ImGui::TextUnformatted(item.text.c_str());

      // Draw shortcut if present
      if (!item.shortcut.empty()) {
        float shortcut_width =
            ImGui::CalcTextSize(item.shortcut.c_str()).x + padding;
        float window_width = ImGui::GetWindowWidth();
        ImGui::SameLine(window_width - shortcut_width);
        // Lighter text for shortcut, white if focused for contrast on green
        ImGui::PushStyleColor(ImGuiCol_Text,
                              is_focused ? ImVec4(0.85f, 0.85f, 0.85f, 1.0f)
                                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::TextUnformatted(item.shortcut.c_str());
        ImGui::PopStyleColor();
      }

      ImGui::PopID();
      ImGui::PopStyleColor(4);
    }

    ImGui::PopItemFlag();
    ImGui::EndDisabled();
    ImGui::EndPopup();
  } else {
    // Popup was closed (clicked outside)
    Close();
  }

  ImGui::PopStyleVar(1);
  ImGui::PopStyleColor(3);
}

}  // namespace ui
}  // namespace xe
