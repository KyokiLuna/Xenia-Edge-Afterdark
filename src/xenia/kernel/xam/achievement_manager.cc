/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/achievement_backends/gpd_achievement_backend.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/ui/audio_helper.h"
#include "xenia/ui/imgui_guest_notification.h"
#include "xenia/kernel/xam/profile_gpd_transaction.h"

DEFINE_bool(show_achievement_notification, true,
            "Show achievement notification on screen.", "UI");

DEFINE_bool(achievement_notification_position_by_game, false,
            "Use game-specified notification position for achievements. "
            "When disabled, achievements always appear at center-bottom.",
            "UI");

DEFINE_string(
    default_achievements_backend, "GPD",
    "Defines which achievements backend should be used as an default. "
    "Possible options: GPD.",
    "Kernel");

DECLARE_int32(user_language);

namespace xe {
namespace kernel {
namespace xam {

AchievementManager::AchievementManager() {
  default_achievements_backend_ = std::make_unique<GpdAchievementBackend>();

  // Add any optional backend here.
};
void AchievementManager::EarnAchievement(const uint32_t slot,
                                         const uint32_t title,
                                         const uint32_t id) const {
  if (slot >= XUserMaxUserCount) {
    return;
  }
  std::error_code error;
  auto captured = kernel_state()->xam_state()->user_tracker()->CaptureAward(
      uint8_t(slot), title, id, error);
  if (captured) {
    CompleteCapturedAward(*captured);
  }
};

void AchievementManager::EarnAchievement(const uint64_t xuid,
                                         const uint32_t title,
                                         const uint32_t id) const {
  std::error_code error;
  auto captured = kernel_state()->xam_state()->user_tracker()->CaptureAward(
      xuid, title, id, error);
  if (captured) {
    CompleteCapturedAward(*captured);
  }
}
X_HRESULT AchievementManager::CompleteCapturedAward(
    const CapturedAchievementAward& award) const {
  const auto result = award.Execute();
  using S = ProfileGpdTransactionAttempt::State;
  if (result.error ||
      (result.state != S::kCommitted && result.state != S::kAlreadyCommitted &&
       result.state != S::kAlreadyUnlocked)) {
    return X_E_FAIL;
  }
  if (!result.event) {
    return X_E_SUCCESS;
  }  // Historical/retry/already-unlocked never re-toasts.
  const auto& origin = result.event->origin();
  const auto& command = result.event->command();
  if (!result.event->Current()) {
    return X_E_SUCCESS;
  }
  for (auto& backend : achievement_backends_) {
    backend->EarnAchievement(origin.xuid, command.title_id, command.record_id);
  }
  // Existing host fallback remains in force until native visible/hide/cue
  // acceptance. This candidate does NOT enable the public native family.
  if (cvars::show_achievement_notification) {
    const auto achievement = default_achievements_backend_->GetAchievementInfo(
        origin.xuid, command.title_id, command.record_id);

  if (achievement) {
      ShowAchievementEarnedNotification(&*achievement, result.event);
    }
  }
  return X_E_SUCCESS;
}

void AchievementManager::LoadTitleAchievements(const uint64_t xuid) const {
  default_achievements_backend_->LoadAchievementsData(xuid);
}

const std::optional<Achievement> AchievementManager::GetAchievementInfo(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  return default_achievements_backend_->GetAchievementInfo(xuid, title_id,
                                                           achievement_id);
}

const std::vector<Achievement> AchievementManager::GetTitleAchievements(
    const uint64_t xuid, const uint32_t title_id) const {
  return default_achievements_backend_->GetTitleAchievements(xuid, title_id);
}

std::vector<uint8_t> AchievementManager::GetAchievementIcon(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  return default_achievements_backend_->GetAchievementIcon(xuid, title_id,
                                                           achievement_id);
}

const std::optional<TitleAchievementsProfileInfo>
AchievementManager::GetTitleAchievementsInfo(const uint64_t xuid,
                                             const uint32_t title_id) const {
  TitleAchievementsProfileInfo info = {};

  const auto achievements = GetTitleAchievements(xuid, title_id);

  if (achievements.empty()) {
    return std::nullopt;
  }

  info.achievements_count = static_cast<uint32_t>(achievements.size());

  for (const auto& entry : achievements) {
    if (!entry.IsUnlocked()) {
      continue;
    }

    info.unlocked_achievements_count++;
    info.gamerscore += entry.gamerscore;
  }

  return info;
}

bool AchievementManager::DoesAchievementExist(
    const uint32_t achievement_id) const {
  const auto spa = kernel_state()->xam_state()->AcquireSpaInfo();
  return spa && spa->GetAchievement(
      achievement_id);
}

void AchievementManager::ShowAchievementEarnedNotification(
    const Achievement* achievement,
    std::shared_ptr<const DurableAchievementEvent> event) const {
  const std::string description =
      fmt::format("{}G - {}", achievement->gamerscore,
                  xe::to_utf8(achievement->achievement_name));

  const Emulator* emulator = kernel_state()->emulator();
  ui::WindowedAppContext& app_context =
      emulator->display_window()->app_context();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  // Use game-specified position if enabled, otherwise default to center-bottom
  const uint8_t position = cvars::achievement_notification_position_by_game
                               ? kernel_state()->notification_position_
                               : 2;

  app_context.CallInUIThread([imgui_drawer, description, position, event = std::move(event)]() {
        if (!event || !event->Current() || !event->TryConsume()) {
          return;
        }
        // Play achievement sound
    ui::AudioHelper::Instance().PlayAchievementSound();

    // Show notification
    new ui::AchievementNotificationWindow(imgui_drawer, "Achievement unlocked",
                                          description, 0, position);
  });
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
