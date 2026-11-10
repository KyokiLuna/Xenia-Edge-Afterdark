/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

void CreateProfileUI::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup("Create Profile");
    has_opened_ = true;
  }

  // Center the window on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(350, 0), ImVec2(400, 300));

  // Style like Xbox - white background, black text, Xbox green highlights
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));

  if (ImGui::BeginPopupModal("Create Profile", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Handle B button or Back to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      ImGui::CloseCurrentPopup();
      Close();
      ImGui::EndPopup();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(10);
      return;
    }

    auto profile_manager =
        emulator_->kernel_state()->xam_state()->profile_manager();

    ImGui::BeginDisabled(created_profile_xuid_ != 0);
    ImGui::Text("Gamertag:");
    // Set focus to InputText on first frame
    if (!focus_set_) {
      ImGui::SetKeyboardFocusHere();
      focus_set_ = true;
    }
    bool enter_pressed = ImGui::InputText("##gt", gamertag_, sizeof(gamertag_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);

    std::string gt(gamertag_);
    bool valid = profile_manager->IsGamertagValid(gt);
    const char* avatars[] = {"1 - Male", "2 - Female", "3 - Male", "4 - Female",
                              "5 - Female", "6 - Male", "7 - Male", "8 - Female",
                              "9 - Male", "10 - Male", "11 - Female", "12 - Female"};
    ImGui::Combo("Stock avatar", &avatar_preset_, avatars, 12);
    const char* pictures[] = {"1", "2", "3", "4", "5", "6", "7",
                               "8", "9", "10", "11", "12", "13"};
    ImGui::Combo("Gamer picture", &stock_picture_, pictures, 13);
    ImGui::EndDisabled();
    if (created_profile_xuid_ && !created_profile_discovered_) {
      ImGui::TextWrapped("The profile was saved but could not be loaded. "
                         "Retry loading it, or close and try again later.");
    } else if (created_profile_xuid_ && created_profile_wanted_signin_ &&
               !created_profile_signed_in_) {
      ImGui::TextWrapped("The profile was saved. Restart Xenia to sign in.");
    } else if (creation_failed_) {
      ImGui::TextWrapped("Could not create the profile. Check installed system "
                         "files and available storage, then try again.");
    }
    const bool pending_load = created_profile_xuid_ && !created_profile_discovered_;
    ImGui::BeginDisabled(!created_profile_xuid_ && !valid);
    if (ImGui::Button(created_profile_xuid_ ? (pending_load ? "Retry loading" : "Done")
                                         : "Create") ||
        (!created_profile_xuid_ && enter_pressed && valid)) {
      bool done = false;
      if (!created_profile_xuid_) {
        created_profile_wanted_signin_ = (profile_manager->GetAccountCount() == 0);
        const auto result = profile_manager->CreateProfileDetailed(
            gt, created_profile_wanted_signin_, false,
            uint32_t(avatar_preset_), uint32_t(stock_picture_));
        creation_failed_ = !result.published;
        if (result.published) {
          created_profile_xuid_ = result.xuid;
          created_profile_discovered_ = result.discovered;
          created_profile_signed_in_ = result.signed_in;
        }
      } else if (pending_load) {
        created_profile_discovered_ = profile_manager->LoadAccount(created_profile_xuid_);
        if (created_profile_discovered_) {
          // Observe an actual existing sign-in only. Do not mutate an active
          // native identity or invent a successful deferred autologin here.
          created_profile_signed_in_ = bool(profile_manager->CaptureProfile(created_profile_xuid_));
        }
      } else {
        done = true;
      }
      if (created_profile_xuid_ && created_profile_discovered_ &&
          (done || !created_profile_wanted_signin_ || created_profile_signed_in_)) {
        ImGui::CloseCurrentPopup();
        Close();
      }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button(created_profile_xuid_ ? "Close" : "Cancel")) {
      ImGui::CloseCurrentPopup();
      Close();
    }

    ImGui::EndPopup();
  } else {
    Close();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(10);
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
