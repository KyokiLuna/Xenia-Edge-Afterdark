/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/signin_ui.h"

#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

SigninUI::SigninUI(xe::ui::ImGuiDrawer* imgui_drawer,
                   ProfileManager* profile_manager, uint32_t last_used_slot,
                   uint32_t users_needed)
    : XamDialog(imgui_drawer),
      profile_manager_(profile_manager),
      last_user_(last_used_slot),
      users_needed_(users_needed),
      title_("Sign In") {}

void SigninUI::OnDraw(ImGuiIO& io) {
  bool first_draw = false;
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
    first_draw = true;
    ReloadProfiles(true);
  }

  // Center the window on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(400, 0), ImVec2(600, 500));

  // Style like Xbox - white background, black text, Xbox green highlights
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Header, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, xbox_green);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));

  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Handle keyboard escape or gamepad B/Back to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      ImGui::CloseCurrentPopup();
      Close();
    }
    for (uint32_t i = 0; i < users_needed_; i++) {
      ImGui::BeginGroup();

      std::vector<const char*> combo_items;
      int items_count = 0;
      int current_item = 0;

      // Fill slot list.
      std::vector<uint8_t> slots;
      slots.push_back(XUserIndexAny);
      combo_items.push_back("---");
      for (auto& elem : slot_data_) {
        // Select the slot or skip it if it's already used.
        bool already_taken = false;
        for (uint32_t j = 0; j < users_needed_; j++) {
          if (chosen_slots_[j] == elem.first) {
            if (i == j) {
              current_item = static_cast<int>(combo_items.size());
            } else {
              already_taken = true;
            }
            break;
          }
        }

        if (already_taken) {
          continue;
        }

        slots.push_back(elem.first);
        combo_items.push_back(elem.second.c_str());
      }
      items_count = static_cast<int>(combo_items.size());

      ImGui::BeginDisabled(users_needed_ == 1);
      ImGui::Combo(fmt::format("##Slot{:d}", i).c_str(), &current_item,
                   combo_items.data(), items_count);
      chosen_slots_[i] = slots[current_item];
      ImGui::EndDisabled();
      ImGui::Spacing();

      combo_items.clear();
      current_item = 0;

      // Fill profile list.
      std::vector<uint64_t> xuids;
      xuids.push_back(0);
      combo_items.push_back("---");
      if (chosen_slots_[i] != XUserIndexAny) {
        for (auto& elem : profile_data_) {
          // Select the profile or skip it if it's already used.
          bool already_taken = false;
          for (uint32_t j = 0; j < users_needed_; j++) {
            if (chosen_xuids_[j] == elem.first) {
              if (i == j) {
                current_item = static_cast<int>(combo_items.size());
              } else {
                already_taken = true;
              }
              break;
            }
          }

          if (already_taken) {
            continue;
          }

          xuids.push_back(elem.first);
          combo_items.push_back(elem.second.c_str());
        }
      }
      items_count = static_cast<int>(combo_items.size());

      ImGui::BeginDisabled(chosen_slots_[i] == XUserIndexAny);
      ImGui::Combo(fmt::format("##Profile{:d}", i).c_str(), &current_item,
                   combo_items.data(), items_count);
      chosen_xuids_[i] = xuids[current_item];
      ImGui::EndDisabled();
      ImGui::Spacing();

      // Draw profile badge.
      uint8_t slot = chosen_slots_[i];
      uint64_t xuid = chosen_xuids_[i];
      const auto account = profile_manager_->GetAccount(xuid);

      if (slot != XUserIndexAny && account) {
        xeDrawProfileContent(imgui_drawer(), xuid, slot, account, nullptr, {},
                             {}, nullptr);
      }

      ImGui::EndGroup();
      if (i != (users_needed_ - 1) && (i == 0 || i == 2)) {
        ImGui::SameLine();
      }
    }

    ImGui::Spacing();

    if (ImGui::Button("Create Profile")) {
      creating_profile_ = true;
      creation_failed_ = false;
      valid_gamertag_ = false;
      ImGui::OpenPopup("Create Profile");
      first_draw = true;
    }
    ImGui::Spacing();

    if (creating_profile_) {
      if (ImGui::BeginPopupModal("Create Profile", nullptr,
                                 ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_HorizontalScrollbar)) {
        if (first_draw) {
          ImGui::SetKeyboardFocusHere();
        }

        ImGui::BeginDisabled(created_profile_xuid_ != 0);
        ImGui::TextUnformatted("Gamertag:");
        if (ImGui::InputText("##Gamertag", gamertag_, sizeof(gamertag_))) {
          valid_gamertag_ =
              profile_manager_->IsGamertagValid(std::string(gamertag_));
        }

        const char* avatars[] = {"1 - Male", "2 - Female", "3 - Male", "4 - Female",
                                  "5 - Female", "6 - Male", "7 - Male", "8 - Female",
                                  "9 - Male", "10 - Male", "11 - Female", "12 - Female"};
        ImGui::Combo("Stock avatar", &avatar_preset_, avatars, 12);
        const char* pictures[] = {"1", "2", "3", "4", "5", "6", "7",
                                   "8", "9", "10", "11", "12", "13"};
        ImGui::Combo("Gamer picture", &stock_picture_, pictures, 13);
        ImGui::EndDisabled();
        if (created_profile_xuid_) {
          ImGui::TextWrapped("The profile was saved but could not be loaded. "
                             "Retry loading it, or cancel and try again later.");
        } else if (creation_failed_) {
          ImGui::TextWrapped("Could not create the profile. Check installed system "
                             "files and available storage, then try again.");
        }
        ImGui::BeginDisabled(!valid_gamertag_ && !created_profile_xuid_);
        if (ImGui::Button(created_profile_xuid_ ? "Retry loading" : "Create")) {
          bool discovered = false;
          if (created_profile_xuid_) {
            discovered = profile_manager_->LoadAccount(created_profile_xuid_);
            creation_failed_ = !discovered;
          } else {
            const auto result = profile_manager_->CreateProfileDetailed(
                std::string(gamertag_), false, false,
                uint32_t(avatar_preset_), uint32_t(stock_picture_));
            creation_failed_ = !result.published;
            if (result.published) {
              created_profile_xuid_ = result.xuid;
              discovered = result.discovered;
            }
          }
          if (discovered) {
            const uint64_t created_xuid = created_profile_xuid_;
            created_profile_xuid_ = 0;
            creation_failed_ = false;
            std::ranges::fill(gamertag_, '\0');
            valid_gamertag_ = false;
            ImGui::CloseCurrentPopup();
            creating_profile_ = false;
            ReloadProfiles(false);
            // Only an actually discovered profile is selectable. Preserve the
            // saved XUID across failed load retries, rather than create again.
            for (uint32_t i = 0; i != users_needed_; ++i) {
              if (chosen_slots_[i] < XUserMaxUserCount && !chosen_xuids_[i]) {
                chosen_xuids_[i] = created_xuid;
                break;
              }
            }
          }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        if (ImGui::Button("Cancel")) {
          // Retain the saved profile's name/XUID for a same-dialog load retry.
          // Cancel never deletes that already durable package.
          if (!created_profile_xuid_) { std::ranges::fill(gamertag_, '\0'); }
          ImGui::CloseCurrentPopup();
          creating_profile_ = false;
        }

        ImGui::EndPopup();
      } else {
        creating_profile_ = false;
      }
    }

    if (signin_failed_) {
      ImGui::TextWrapped("Sign-in could not complete. Check storage and try again.");
    }
    if (ImGui::Button("OK")) {
      std::map<uint8_t, uint64_t> profile_map;
      for (uint32_t i = 0; i < users_needed_; i++) {
        uint8_t slot = chosen_slots_[i];
        uint64_t xuid = chosen_xuids_[i];
        if (slot != XUserIndexAny && xuid != 0) {
          profile_map[slot] = xuid;
        }
      }
      profile_manager_->LoginMultiple(profile_map);
      signin_failed_ = false;
      for (const auto& [slot, xuid] : profile_map) {
        const auto actual = profile_manager_->CaptureProfile(slot);
        if (!actual || actual->xuid() != xuid) { signin_failed_ = true; }
      }
      if (!signin_failed_) {
        ImGui::CloseCurrentPopup();
        Close();
      }
    }
    ImGui::SameLine();

    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
      Close();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::EndPopup();
  } else {
    Close();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(14);
}

void SigninUI::ReloadProfiles(bool first_draw) {
  auto profile_manager = kernel_state()->xam_state()->profile_manager();
  auto profiles = profile_manager->GetAccounts();

  profile_data_.clear();
  for (auto& [xuid, account] : *profiles) {
    profile_data_.push_back({xuid, account.GetGamertagString()});
  }

  if (first_draw) {
    // If only one user is requested, request last used controller to sign in.
    if (users_needed_ == 1) {
      chosen_slots_[0] = last_user_;
    } else {
      for (uint32_t i = 0; i < users_needed_; i++) {
        // TODO: Not sure about this, needs testing on real hardware.
        chosen_slots_[i] = i;
      }
    }

    // Default profile selection to profile that is already signed in.
    for (auto& elem : profile_data_) {
      uint64_t xuid = elem.first;
      uint8_t slot = profile_manager->GetUserIndexAssignedToProfile(xuid);
      for (uint32_t j = 0; j < users_needed_; j++) {
        if (chosen_slots_[j] != XUserIndexAny && slot == chosen_slots_[j]) {
          chosen_xuids_[j] = xuid;
        }
      }
    }
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
