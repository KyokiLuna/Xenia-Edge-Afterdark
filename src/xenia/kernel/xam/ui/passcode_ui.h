/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_PASSCODE_UI_H_
#define XENIA_KERNEL_XAM_UI_PASSCODE_UI_H_

#include <array>
#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

class ProfilePasscodeUI final : public XamDialog {
 public:
  ProfilePasscodeUI(xe::ui::ImGuiDrawer* imgui_drawer, std::string_view title,
                    std::string_view description);
  ~ProfilePasscodeUI() = default;

  bool SelectedSignedIn() const { return selected_signed_in_; }
  const auto& passcode() const { return passcode_; }

 private:
  const char* labelled_keys_[11] = {"None", "X",  "Y",    "RB",   "LB",   "LT",
                                    "RT",   "Up", "Down", "Left", "Right"};

  const std::map<std::string, uint16_t> keys_map_ = {
      {"None", 0},
      {"X", X_BUTTON_PASSCODE},
      {"Y", Y_BUTTON_PASSCODE},
      {"RB", RIGHT_BUMPER_PASSCODE},
      {"LB", LEFT_BUMPER_PASSCODE},
      {"LT", LEFT_TRIGGER_PASSCODE},
      {"RT", RIGHT_TRIGGER_PASSCODE},
      {"Up", DPAD_UP_PASSCODE},
      {"Down", DPAD_DOWN_PASSCODE},
      {"Left", DPAD_LEFT_PASSCODE},
      {"Right", DPAD_RIGHT_PASSCODE}};

  void OnDrawDialog(ImGuiIO& io) override;

  void DrawPasscodeField(uint8_t key_id);

  bool has_opened_ = false;
  bool selected_signed_in_ = false;
  std::string title_;
  std::string description_;

  static constexpr uint8_t passcode_length = sizeof(X_XAMACCOUNTINFO::passcode);
  int key_indexes_[passcode_length] = {0, 0, 0, 0};
  std::array<uint16_t, 4> passcode_{};
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
