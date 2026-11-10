/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_CREATE_PROFILE_UI_H_
#define XENIA_KERNEL_XAM_UI_CREATE_PROFILE_UI_H_

#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

class CreateProfileUI final : public XamDialog {
 public:
  CreateProfileUI(xe::ui::ImGuiDrawer* imgui_drawer, Emulator* emulator)
      : XamDialog(imgui_drawer), emulator_(emulator) {
    memset(gamertag_, 0, sizeof(gamertag_));
  }

  ~CreateProfileUI() = default;

 private:
  void OnDraw(ImGuiIO& io) override;

  bool has_opened_ = false;
  bool focus_set_ = false;
  char gamertag_[16] = "";
  bool valid_gamertag_ = false;
  bool creation_failed_ = false;
  // Once durable publication succeeds, retry discovery of THIS saved UID.
  // Never create a second package merely because loading/sign-in was deferred.
  uint64_t created_profile_xuid_ = 0;
  bool created_profile_discovered_ = false;
  bool created_profile_wanted_signin_ = false;
  bool created_profile_signed_in_ = false;
  int avatar_preset_ = 0;
  int stock_picture_ = 0;
  Emulator* emulator_;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
