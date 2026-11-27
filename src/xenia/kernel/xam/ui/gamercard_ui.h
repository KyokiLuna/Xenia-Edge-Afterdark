/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_GAMERCARD_UI_H_
#define XENIA_KERNEL_XAM_UI_GAMERCARD_UI_H_

#include <span>
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
class UserProfile;
namespace ui {

struct GamercardSettings {
  // Account settings
  char gamertag[16 * 4 + 1];  // Bounded UTF8 display of all16 native units.
  xe::XOnlineCountry country;
  xe::XLanguage language;
  bool is_live_enabled;
  char online_xuid[0x11];
  char online_domain[0x14];
  X_XAMACCOUNTINFO::AccountSubscriptionTier account_subscription_tier;

  // GPD settings
  std::map<UserSettingId, UserDataTypes> gpd_settings;

  // GPD string buffers
  char gamer_name[0x104];
  char gamer_motto[0x2C];
  char gamer_bio[kMaxUserDataSize];

  // Other
  std::vector<uint8_t> profile_icon;
  // Immediate Texture?
  xe::ui::ImmediateTexture* icon_texture;
};

class GamercardUI final : public XamDialog {
 public:
  GamercardUI(xe::ui::Window* window, xe::ui::ImGuiDrawer* imgui_drawer,
              KernelState* kernel_state, uint64_t xuid);

  ~GamercardUI() = default;

 private:
  void OnDrawDialog(ImGuiIO& io) override;
  void DrawBaseSettings(ImGuiIO& io);
  void DrawOnlineSettings(ImGuiIO& io);
  void DrawGpdSettings(ImGuiIO& io);

  void LoadGamercardInfo();
  void LoadSetting(UserProfile& profile, UserSettingId setting_id);
  void LoadStringSetting(UserSettingId setting_id, std::span<char> buffer);
  void SaveSettings();
  bool SaveAccountData();
  bool SaveProfileIcon();

  void DrawSettingComboBox(UserSettingId setting_id, std::string label,
                           const char* const items[], int item_count,
                           float alignment);

  void DrawInputTextBox(
      std::string label, char* buffer, size_t buffer_size, float alignment,
      std::function<bool(std::span<char>)> on_input_change = {});

  void SelectNewIcon();

  const uint64_t xuid_ = 0;
  bool profile_data_available_ = false;
  const bool is_valid_gamertag_ = true;

  bool has_opened_ = false;
  KernelState* kernel_state_;
  xe::ui::Window* window_;

  // We're storing OG and current values to compare at the end and send what was
  // changed.
  GamercardSettings gamercardOriginalValues_ = {};
  GamercardSettings gamercardValues_ = {};
  ProfileAccountFieldValues account_form_original_;
  ProfileAccountFieldValues account_pending_values_;
  std::shared_ptr<HostProfileAccountEdit> account_edit_;
  std::string account_save_error_;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
