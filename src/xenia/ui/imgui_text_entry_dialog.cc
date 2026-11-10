/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_text_entry_dialog.h"

#include <algorithm>
#include <cstring>

#include "third_party/imgui/imgui_internal.h"
#include "xenia/hid/input_system.h"

namespace xe::ui {
namespace {
constexpr unsigned int kColumns = 10;
constexpr unsigned int kCharacterCount = 40;
constexpr unsigned int kCommandCount = 6;
constexpr std::string_view kLetters =
    "qwertyuiop"
    "asdfghjkl'"
    "zxcvbnm,.?"
    "0123456789";
constexpr std::string_view kSymbols =
    "!@#$%^&*()"
    "-_=+[]{}\\|"
    ";:\"<>/`~.?"
    "0123456789";
constexpr std::array<const char*, kCommandCount> kCommands = {
    "Space", "Delete", "Shift", "Symbols", "Done", "Cancel"};
constexpr uint16_t kDirections =
    hid::X_INPUT_GAMEPAD_DPAD_UP | hid::X_INPUT_GAMEPAD_DPAD_DOWN |
    hid::X_INPUT_GAMEPAD_DPAD_LEFT | hid::X_INPUT_GAMEPAD_DPAD_RIGHT;
}  // namespace

ImGuiTextEntryDialog::ImGuiTextEntryDialog(
    ImGuiDrawer* drawer, hid::InputSystem* input_system, std::string title,
    std::string description, std::u16string default_text,
    size_t max_utf16_units, Completion completion,
    ShutdownCompletion shutdown_completion)
    : ImGuiGamepadDialog(drawer, input_system),
      text_(max_utf16_units, default_text),
      completion_(std::move(completion)),
      shutdown_completion_(std::move(shutdown_completion)),
      title_(title.empty() ? "Keyboard" : std::move(title)),
      description_(std::move(description)),
      input_capture_generation_(drawer->input_capture_generation()) {
  SynchronizeTextBuffer();
  if (input_system_) {
    for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
      hid::X_INPUT_STATE state{};
      if (input_system_->GetStateForUI(i, hid::X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS) {
        previous_buttons_[i] = consumed_buttons_[i] = state.gamepad.buttons;
      }
    }
  }
}

bool ImGuiTextEntryDialog::HasInputOwnership() const {
  if (!imgui_drawer()->IsOnlyDialog(this) ||
      (input_ownership_callback_ && !input_ownership_callback_())) {
    return false;
  }
  const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
  return popups.empty() ? !has_opened_
                        : popup_id_ && popups.back().PopupId == popup_id_;
}

void ImGuiTextEntryDialog::SynchronizeTextBuffer() {
  const std::string text = text_.utf8();
  text_buffer_.resize(std::max<size_t>(128, text.size() + 1));
  std::memcpy(text_buffer_.data(), text.c_str(), text.size() + 1);
  // InputText owns a separate edit buffer while active. Controller edits must
  // replace that buffer as well, rather than being overwritten on the draw.
  if (text_field_id_) {
    if (auto* edit = ImGui::GetInputTextState(text_field_id_)) {
      edit->ReloadUserBufAndMoveToEnd();
    }
  }
}

void ImGuiTextEntryDialog::Finish(bool accepted) {
  if (!IsClosing()) {
    result_ =
        accepted ? TextEntryResult::kAccepted : TextEntryResult::kCanceled;
    Close();
  }
}

void ImGuiTextEntryDialog::MoveSelection(int horizontal, int vertical) {
  int row = int(selection_ / kColumns);
  int column = int(selection_ % kColumns);
  if (vertical) {
    row = (row + vertical + 5) % 5;
    column = std::min(column, row == 4 ? int(kCommandCount) - 1 : 9);
  } else {
    int columns = row == 4 ? kCommandCount : kColumns;
    column = (column + horizontal + columns) % columns;
  }
  selection_ = unsigned(row * kColumns + column);
}

void ImGuiTextEntryDialog::ActivateSelection() {
  if (IsClosing()) {
    return;
  }
  if (selection_ < kCharacterCount) {
    char32_t character = (symbols_ ? kSymbols : kLetters)[selection_];
    if (shifted_ && character >= 'a' && character <= 'z') {
      character -= 'a' - 'A';
    }
    text_.Append(character);
  } else {
    switch (selection_ - kCharacterCount) {
      case 0:
        text_.Append(U' ');
        break;
      case 1:
        text_.Backspace();
        break;
      case 2:
        shifted_ = !shifted_;
        break;
      case 3:
        symbols_ = !symbols_;
        break;
      case 4:
        Finish(true);
        break;
      case 5:
        Finish(false);
        break;
    }
  }
  SynchronizeTextBuffer();
}

void ImGuiTextEntryDialog::PollGamepads(bool owns_input) {
  if (!input_system_) {
    return;
  }
  const uint64_t generation = imgui_drawer()->input_capture_generation();
  const bool ownership_changed = generation != input_capture_generation_;
  input_capture_generation_ = generation;
  std::array<uint16_t, XUserMaxUserCount> buttons{}, pressed{};
  for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
    hid::X_INPUT_STATE state{};
    if (input_system_->GetStateForUI(i, hid::X_INPUT_FLAG_GAMEPAD, &state) ==
        X_ERROR_SUCCESS) {
      buttons[i] = state.gamepad.buttons;
    }
    pressed[i] = buttons[i] & ~previous_buttons_[i];
    previous_buttons_[i] = buttons[i];
    consumed_buttons_[i] &= buttons[i];
    if (!owns_input || ownership_changed) {
      consumed_buttons_[i] |= buttons[i];
    }
    pressed[i] &= ~consumed_buttons_[i];
  }
  if (!owns_input || ownership_changed) {
    return;
  }
  for (auto edge : pressed) {
    if (edge & (kDirections | hid::X_INPUT_GAMEPAD_A | hid::X_INPUT_GAMEPAD_B |
                hid::X_INPUT_GAMEPAD_BACK | hid::X_INPUT_GAMEPAD_START |
                hid::X_INPUT_GAMEPAD_X | hid::X_INPUT_GAMEPAD_Y)) {
      ImGui::SetNavCursorVisible(true);
      break;
    }
  }
  // Cancellation wins across all slots, including simultaneous A or Start.
  for (auto edge : pressed) {
    if (edge & (hid::X_INPUT_GAMEPAD_B | hid::X_INPUT_GAMEPAD_BACK)) {
      Finish(false);
      return;
    }
  }
  for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
    if (pressed[i] & hid::X_INPUT_GAMEPAD_START) {
      Finish(true);
      return;
    }
    uint16_t navigate = pressed[i] & kDirections;
    const auto held = buttons[i] & kDirections & ~consumed_buttons_[i];
    if (navigate) {
      next_repeat_[i] = ImGui::GetTime() + 0.4;
    } else if (held && ImGui::GetTime() >= next_repeat_[i]) {
      navigate = held;
      next_repeat_[i] = ImGui::GetTime() + 0.12;
    }
    if (navigate & hid::X_INPUT_GAMEPAD_DPAD_UP) {
      MoveSelection(0, -1);
    }
    if (navigate & hid::X_INPUT_GAMEPAD_DPAD_DOWN) {
      MoveSelection(0, 1);
    }
    if (navigate & hid::X_INPUT_GAMEPAD_DPAD_LEFT) {
      MoveSelection(-1, 0);
    }
    if (navigate & hid::X_INPUT_GAMEPAD_DPAD_RIGHT) {
      MoveSelection(1, 0);
    }
    if (pressed[i] & hid::X_INPUT_GAMEPAD_X) {
      text_.Backspace();
      SynchronizeTextBuffer();
    }
    if (pressed[i] & hid::X_INPUT_GAMEPAD_Y) {
      shifted_ = !shifted_;
    }
    if (pressed[i] & hid::X_INPUT_GAMEPAD_A) {
      ActivateSelection();
      return;
    }
  }
}

int ImGuiTextEntryDialog::TextCallback(ImGuiInputTextCallbackData* data) {
  auto* dialog = static_cast<ImGuiTextEntryDialog*>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    dialog->text_buffer_.resize(size_t(data->BufSize));
    data->Buf = dialog->text_buffer_.data();
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
    if (!dialog->text_.SetUtf8({data->Buf, size_t(data->BufTextLen)})) {
      const auto previous = dialog->text_.utf8();
      data->DeleteChars(0, data->BufTextLen);
      data->InsertChars(0, previous.c_str());
    }
  }
  return 0;
}

void ImGuiTextEntryDialog::OnDraw(ImGuiIO& io) {
  const std::string popup = title_ + "###XeniaTextEntry";
  if (!has_opened_ && HasInputOwnership()) {
    popup_id_ = ImGui::GetID(popup.c_str());
    ImGui::OpenPopup(popup.c_str());
    has_opened_ = true;
  }
  bool owns_input = has_opened_ && HasInputOwnership();
  if (owns_input && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    Finish(false);
  }
  if (IsClosing()) {
    return;
  }
  if (!has_opened_) {
    PollGamepads(false);
    return;
  }
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 38, 0),
                           ImGuiCond_Always);
  if (ImGui::BeginPopupModal(
          popup.c_str(), nullptr,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::BeginDisabled(!owns_input);
    ImGui::TextDisabled("Xenia keyboard (host fallback)");
    if (!description_.empty()) {
      ImGui::TextWrapped("%s", description_.c_str());
    }
    if (focus_text_ && owns_input) {
      ImGui::SetKeyboardFocusHere();
      focus_text_ = false;
    }
    text_field_id_ = ImGui::GetID("##text");
    const auto config_flags = io.ConfigFlags;
    // InputText treats gamepad A as Enter even on a NoNav item. The raw
    // controller keyboard is its sole controller owner; host typing still uses
    // InputText's ordinary Unicode/cursor/clipboard editing.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui::SetNextItemWidth(-1);
    const bool submitted =
        ImGui::InputText("##text", text_buffer_.data(), text_buffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_CallbackResize |
                             ImGuiInputTextFlags_CallbackEdit,
                         TextCallback, this);
    io.ConfigFlags = config_flags;
    // Apply physical text queued for this frame before controller edits or
    // acceptance. A simultaneous Start must include this frame's text.
    PollGamepads(owns_input);
    if (submitted && owns_input && !IsClosing()) {
      Finish(true);
    }
    ImGui::TextDisabled("%zu / %zu UTF-16 units", text_.text().size(),
                        text_.max_utf16_units());
    const float cell = (ImGui::GetContentRegionAvail().x -
                        ImGui::GetStyle().ItemSpacing.x * 9) /
                       10;
    // Keep the field navigation-capable for SetKeyboardFocusHere. Only the
    // controller grid excludes ImGui navigation as a second selection owner.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (unsigned int key = 0; key < kCharacterCount + kCommandCount; ++key) {
      if (key % kColumns && key != kCharacterCount) {
        ImGui::SameLine();
      }
      ImGui::PushID(int(key));
      std::string label;
      if (key < kCharacterCount) {
        char character = (symbols_ ? kSymbols : kLetters)[key];
        if (shifted_ && character >= 'a' && character <= 'z') {
          character -= 'a' - 'A';
        }
        label.assign(1, character);
      } else {
        label = kCommands[key - kCharacterCount];
      }
      const bool selected = selection_ == key;
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.49f, 0.06f, 1));
      }
      const float width = key < kCharacterCount ? cell : cell * 1.5f;
      const bool clicked = ImGui::Button(label.c_str(), ImVec2(width, 0));
      if (selected) {
        ImGui::PopStyleColor();
      }
      if (clicked && owns_input &&
          ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        selection_ = key;
        ActivateSelection();
      }
      ImGui::PopID();
    }
    ImGui::PopItemFlag();
    ImGui::TextDisabled("D-pad: move   A: key   X: delete   Y: shift");
    ImGui::TextDisabled("Start: done   B/Back: cancel");
    ImGui::EndDisabled();
    ImGui::EndPopup();
  } else {
    PollGamepads(false);
  }
}

void ImGuiTextEntryDialog::OnClose() {
  GetIO();
  const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
  for (int i = 0; i < popups.Size; ++i) {
    if (popup_id_ && popups[i].PopupId == popup_id_) {
      ImGui::ClosePopupToLevel(i, true);
      break;
    }
  }
  // A completion may navigate away and ClearDialogs. Detach before exposing
  // that callback so it cannot recursively destroy this still-closing dialog.
  // The base destructor's duplicate removal is a no-op.
  imgui_drawer()->RemoveDialog(this);
  if (input_system_ && blocking_input_) {
    input_system_->RemoveUIInputBlocker();
    blocking_input_ = false;
  }
  if (completion_) {
    auto complete = [completion = std::move(completion_), result = result_,
                     text = text_.text()] { completion(result, text); };
    const bool during_frame = ImGui::GetCurrentContext()->WithinFrameScope;
    if (during_frame) {
      if (!imgui_drawer()->PostDeferredCallback(std::move(complete)) &&
          shutdown_completion_) {
        // An overlapped request's result sink is safe during teardown. A host
        // navigation continuation must not run inside Draw on queue rejection.
        auto shutdown = std::move(shutdown_completion_);
        shutdown(TextEntryResult::kAborted, text_.text());
      }
    } else {
      complete();
    }
  }
}

}  // namespace xe::ui
