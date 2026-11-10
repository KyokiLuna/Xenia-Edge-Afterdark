/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_TEXT_ENTRY_DIALOG_H_
#define XENIA_UI_IMGUI_TEXT_ENTRY_DIALOG_H_

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "xenia/ui/imgui_gamepad_dialog.h"
#include "xenia/ui/text_entry.h"
#include "xenia/xbox.h"

namespace xe::ui {

// Host-rendered fallback, reusable by guest keyboard requests and host profile
// editing. This is not the dashboard's native Xbox 360 keyboard scene.
class ImGuiTextEntryDialog final : public ImGuiGamepadDialog {
 public:
  // Runs on the UI thread with game input released. During a frame the result
  // is copied and delivered after the frame; external clear/teardown delivers
  // kAborted inline. A canceled/aborted value must not replace user data.
  using Completion =
      std::function<void(TextEntryResult, const std::u16string&)>;
  using ShutdownCompletion = Completion;

  ImGuiTextEntryDialog(ImGuiDrawer* drawer, hid::InputSystem* input_system,
                       std::string title, std::string description,
                       std::u16string default_text, size_t max_utf16_units,
                       Completion completion,
                       ShutdownCompletion shutdown_completion = {});

  // If the UI loop rejects a frame's deferred completion, only the optional
  // shutdown_completion is called, with kAborted. It must only store a result /
  // signal waiters; unlike normal completion it must not mutate UI or navigate.
  // This keeps request completion safe without running host UI after loop exit.

  // UI thread only. Native window owners can additionally exclude host modals.
  void SetInputOwnershipCallback(std::function<bool()> callback) {
    input_ownership_callback_ = std::move(callback);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  bool HasInputOwnership() const;
  void PollGamepads(bool owns_input);
  void MoveSelection(int horizontal, int vertical);
  void ActivateSelection();
  void SynchronizeTextBuffer();
  void Finish(bool accepted);
  static int TextCallback(ImGuiInputTextCallbackData* data);

  TextEntry text_;
  Completion completion_;
  ShutdownCompletion shutdown_completion_;
  std::function<bool()> input_ownership_callback_;
  std::string title_;
  std::string description_;
  std::vector<char> text_buffer_;
  std::array<uint16_t, XUserMaxUserCount> previous_buttons_{};
  std::array<uint16_t, XUserMaxUserCount> consumed_buttons_{};
  std::array<double, XUserMaxUserCount> next_repeat_{};
  uint64_t input_capture_generation_ = 0;
  ImGuiID popup_id_ = 0;
  ImGuiID text_field_id_ = 0;
  unsigned int selection_ = 0;
  bool shifted_ = false;
  bool symbols_ = false;
  bool has_opened_ = false;
  bool focus_text_ = true;
  TextEntryResult result_ = TextEntryResult::kAborted;
};

}  // namespace xe::ui

#endif  // XENIA_UI_IMGUI_TEXT_ENTRY_DIALOG_H_
