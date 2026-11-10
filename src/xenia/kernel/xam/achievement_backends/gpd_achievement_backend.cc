/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_backends/gpd_achievement_backend.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_state.h"

DECLARE_int32(user_language);

namespace xe {
namespace kernel {
namespace xam {

void GpdAchievementBackend::EarnAchievement(const uint64_t xuid,
                                            const uint32_t title_id,
                                            const uint32_t achievement_id) {
  const auto user = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user) {
    return;
  }

  kernel_state()->xam_state()->user_tracker()->UnlockAchievement(
      xuid, achievement_id);
}

const std::optional<Achievement> GpdAchievementBackend::GetAchievementInfo(
    uint64_t xuid, uint32_t title_id, uint32_t id) const {
  const auto achievements = kernel_state()->xam_state()->user_tracker()->GetUserTitleAchievements(xuid, title_id);
  const auto found = std::find_if(
      achievements.begin(), achievements.end(),
      [id](const auto& item) {
    return item.achievement_id == id; });
  return found == achievements.end() ? std::nullopt
                                     : std::make_optional(*found);
}

bool GpdAchievementBackend::IsAchievementUnlocked(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  const auto achievement = GetAchievementInfo(xuid, title_id, achievement_id);

  if (!achievement) {
    return false;
  }

  return (achievement->flags &
          static_cast<uint32_t>(AchievementFlags::kAchieved)) != 0;
}

const std::vector<Achievement> GpdAchievementBackend::GetTitleAchievements(
    const uint64_t xuid, const uint32_t title_id) const {
  const auto user = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user) {
    return {};
  }

  return kernel_state()->xam_state()->user_tracker()->GetUserTitleAchievements(
      xuid, title_id);
}

std::vector<uint8_t> GpdAchievementBackend::GetAchievementIcon(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  const auto user = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user) {
    return {};
  }

  return kernel_state()->xam_state()->user_tracker()->GetAchievementIcon(
      xuid, title_id, achievement_id);
}

bool GpdAchievementBackend::LoadAchievementsData(const uint64_t xuid) {
  auto user = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user) {
    return false;
  }

  // GPDs are handled by UserTracker
  return true;
}

bool GpdAchievementBackend::SaveAchievementData(
    const uint64_t xuid, const uint32_t title_id,
    const Achievement* achievement) {
  auto user = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user) {
    return false;
  }

  // GPDs are handled by UserTracker
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
