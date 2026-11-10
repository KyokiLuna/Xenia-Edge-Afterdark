/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/emulator.h"
#include "xenia/kernel/xam/user_profile.h"
#include <algorithm>
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/stb/stb_image.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_data.h"
#include "xenia/kernel/xam/user_property.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/kernel/xconfig.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {
std::shared_ptr<const ProfileGpdReadModels> Models(uint64_t xuid) {
  std::error_code error;
  return kernel_state()->xam_state()->profile_manager()->CaptureGpdRead(xuid,
                                                                        error);
}
bool Committed(const ProfileGpdTransactionAttempt& result) {
  using S = ProfileGpdTransactionAttempt::State;
  return !result.error && (result.state == S::kCommitted ||
                           result.state == S::kAlreadyCommitted ||
                           result.state == S::kAlreadyUnlocked);
}
std::vector<uint8_t> Owned(std::span<const uint8_t> bytes) {
  return {bytes.begin(), bytes.end()};
}
}  // namespace
std::shared_ptr<const SpaInfo> UserTracker::Spa() const {
  std::lock_guard lock(metadata_mutex_);
  return spa_data_;
}
std::vector<uint64_t> UserTracker::Tracked() const {
  std::lock_guard lock(metadata_mutex_);
  return {tracked_xuids_.begin(), tracked_xuids_.end()};
}
ProfileGpdTransactionAttempt UserTracker::Mutate(
    uint64_t xuid, ProfileGpdMutation command) const {
  std::error_code error;
  auto writer =
      kernel_state()->xam_state()->profile_manager()->CaptureGpdWriter(xuid,
                                                                       error);
  if (error) {
    ProfileGpdTransactionAttempt result;
    result.error = error;
    return result;
  }
  if (!command.request_id) {
    command.request_id = writer.model->AllocateRequestId(error);
    if (error) {
      ProfileGpdTransactionAttempt result;
      result.error = error;
      return result;
    }
  }
  return writer.Submit(std::move(command));
}
std::optional<CapturedAchievementAward> UserTracker::CaptureAward(
    uint8_t slot, uint32_t title, uint32_t achievement,
    std::error_code& error) const {
  auto writer =
      kernel_state()->xam_state()->profile_manager()->CaptureGpdWriter(slot,
                                                                       error);
  if (!writer.model) {
    return {};
  }
  auto award = CaptureAward(writer.model->xuid(), title, achievement, error);
  if (!award || award->writer.model != writer.model) {
    if (!error) {
      error = std::make_error_code(std::errc::operation_canceled);
    }
    return {};
  }
  return award;
}
std::optional<CapturedAchievementAward> UserTracker::CaptureAward(
    uint64_t xuid, uint32_t title, uint32_t achievement,
    std::error_code& error) const {
  error.clear();
  auto writer =
      kernel_state()->xam_state()->profile_manager()->CaptureGpdWriter(xuid,
                                                                       error);
  if (!writer.model) {
    return {};
  }
  auto spa = kernel_state()->xam_state()->AcquireSpaInfo();
  const auto foreground =
      kernel_state()->emulator()->title_execution_state().snapshot();
  if (!spa || spa->title_id() != title || foreground.title_id != title ||
      foreground.phase != TitleExecutionState::Phase::kRunning ||
      !kernel_state()->emulator()->OwnsTitleExecution(kernel_state())) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kUnlock;
  command.title_id = title;
  command.record_id = achievement;
  command.guest_time = Clock::QueryGuestSystemTime();
  command.foreground_generation = foreground.generation;
  command.title_data = ProfileGpdTitleData::Capture(
      std::move(spa),
      static_cast<XLanguage>(kernel_state()->xconfig()->ReadSetting<uint32_t>(
          XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE)));
  // A TITLE HLE caller has no invented SYSTEM domain; origin0 stays0.
  if (!command.title_data) {
    error = std::make_error_code(std::errc::bad_message);
    return {};
  }
  auto origin = writer.CaptureCurrentOrigin(command, error);
  if (!origin) {
    return {};
  }
  return CapturedAchievementAward{std::move(writer), std::move(command),
                                  std::move(*origin)};
}

bool UserTracker::AddUser(uint64_t xuid) {
  {
    std::lock_guard lock(metadata_mutex_);
    if (!tracked_xuids_.insert(xuid).second) {
      return false;
    }
  }
  if (Spa()) {
    AddTitleToPlayedList(xuid);
  }
  return true;
}

bool UserTracker::RemoveUser(uint64_t xuid) {
  std::lock_guard lock(metadata_mutex_);
    return tracked_xuids_.erase(xuid) != 0;
}

bool UserTracker::UnlockAchievement(uint64_t xuid, uint32_t achievement_id) {
  std::error_code error;
  auto award =
      CaptureAward(xuid, kernel_state()->title_id(), achievement_id, error);
    return award && Committed(award->Execute());
  }

void UserTracker::FlushUserData(uint64_t xuid) {
  // Every mutator submits a durable intent directly. No untracked dirty model
  // exists and a flush must never bypass/replace a pending cloned operation.
  std::error_code error;
  auto writer = kernel_state()->xam_state()->profile_manager()->CaptureGpdWriter(xuid,
                                                                       error);
  if (writer.model && writer.model->Pending(error)) {
    auto transaction =
        writer.model->AcquireTransaction(writer.owner, writer.recovery, error);
  if (transaction) {
      transaction->Attempt();
    }
  }
}

void UserTracker::AddTitleToPlayedList() {
  for (auto xuid : Tracked()) {
    AddTitleToPlayedList(xuid);
  }
}

void UserTracker::AddTitleToPlayedList(uint64_t xuid) {
  auto spa = Spa();
  if (!spa || !spa->include_in_profile() || spa->is_system_app()) {
    return;
  }
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kTouchTitle;
  command.title_id = spa->title_id();
  command.guest_time = Clock::QueryGuestSystemTime();
  command.title_data =
      ProfileGpdTitleData::Capture(std::move(spa), XLanguage::kEnglish);
  const auto result = Mutate(xuid, std::move(command));
  if (result.error) {
    XELOGW("Profile title mutation pending/refused: {}",
           result.error.message());
  }
}

void UserTracker::RemoveTitleFromPlayedList(uint64_t xuid, uint32_t title_id) {
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kRemoveTitle;
  command.title_id = title_id;
  const auto result = Mutate(xuid, std::move(command));
  if (result.error) {
    XELOGW("Profile title deletion pending/refused: {}",
           result.error.message());
  }
}

// Privates
bool UserTracker::IsUserTracked(uint64_t xuid) const {
  std::lock_guard lock(metadata_mutex_);
  return tracked_xuids_.contains(xuid);
}

std::optional<TitleInfo> UserTracker::GetUserTitleInfo(
    uint64_t xuid, uint32_t title_id) const {
  const auto all = GetPlayedTitles(xuid);
  const auto found = std::find_if(
      all.begin(), all.end(),
      [title_id](const auto& info) { return info.id == title_id; });
    return found == all.end() ? std::nullopt : std::make_optional(*found);
  }

std::vector<TitleInfo> UserTracker::GetPlayedTitles(uint64_t xuid) const {
  const auto models = Models(xuid);
  if (!models) {
    return {};
  }
  std::vector<TitleInfo> result;
  for (const auto* title : models->dashboard.GetTitlesInfo()) {
    if (!title->include_in_enumerator()) {
      continue;
  }

  TitleInfo info{};
  info.id = title->title_id;
  info.achievements_count = title->achievements_count;
  info.unlocked_achievements_count = title->achievements_unlocked;
  info.gamerscore_amount = title->gamerscore_total;
  info.title_earned_gamerscore = title->gamerscore_earned;
  info.title_name = models->dashboard.GetTitleName(title->title_id);
  info.flags = title->flags;
    info.all_avatar_awards = title->all_avatar_awards;
    info.male_avatar_awards = title->male_avatar_awards;
    info.female_avatar_awards = title->female_avatar_awards;
    info.online_unlocked_achievements = title->online_achievement_count;

  if (title->last_played.is_valid()) {
    info.last_played =
        chrono::WinSystemClock::to_sys(title->last_played.to_time_point());
  }
    if (auto game = models->titles.find(title->title_id);
        game != models->titles.end()) {
      info.icon = Owned(game->second.GetImage(kXdbfIdTitle));
    }
    result.push_back(std::move(info));
  }

  std::ranges::sort(result,
                    [](const auto& a, const auto& b) {
                      return a.last_played > b.last_played;
                    });

  return result;
}

void UserTracker::UpdateMissingAchievemntsIcons() {
  auto spa = Spa();
    if (!spa) {
    return;
    }

    auto data = ProfileGpdTitleData::Capture(spa, XLanguage::kEnglish);
  for (auto xuid : Tracked()) {
    ProfileGpdMutation command;
    command.kind = ProfileGpdMutationKind::kFillMissingIcons;
    command.title_id = spa->title_id();
    command.title_data = data;
    const auto result = Mutate(xuid, std::move(command));
    if (result.error) {
      XELOGW("Profile SPA mutation pending/refused: {}",
             result.error.message());
    }
  }
}

void UserTracker::UpdateSpaInfo(std::shared_ptr<const SpaInfo> spa_info) {
  {
    std::lock_guard lock(metadata_mutex_);
    spa_data_ = std::move(spa_info);
  }
  if (!Spa()) {
    return;
  }
  AddTitleToPlayedList();
  UpdateProfileGpd();
  UpdateTitleGpdFile();
  UpdateMissingAchievemntsIcons();
}

void UserTracker::UpdateTitleGpdFile() {
  auto spa = Spa();
    if (!spa) {
    return;
    }

    auto data = ProfileGpdTitleData::Capture(spa, XLanguage::kEnglish);
  for (auto xuid : Tracked()) {
    ProfileGpdMutation command;
    command.kind = ProfileGpdMutationKind::kRefreshTitleData;
    command.title_id = spa->title_id();
    command.title_data = data;
    const auto result = Mutate(xuid, std::move(command));
    if (result.error) {
      XELOGW("Profile SPA mutation pending/refused: {}",
             result.error.message());
    }
  }
}

void UserTracker::UpdateProfileGpd() {
  auto spa = Spa();
    if (!spa) {
    return;
    }

    auto data = ProfileGpdTitleData::Capture(spa, XLanguage::kEnglish);
  for (auto xuid : Tracked()) {
    ProfileGpdMutation command;
    command.kind = ProfileGpdMutationKind::kRefreshProfileData;
    command.title_id = spa->title_id();
    command.title_data = data;
    const auto result = Mutate(xuid, std::move(command));
    if (result.error) {
      XELOGW("Profile SPA mutation pending/refused: {}",
             result.error.message());
    }
  }
}

std::vector<Achievement> UserTracker::GetUserTitleAchievements(
    uint64_t xuid, uint32_t title_id) const {
  auto models = Models(xuid);
  if (!models) {
    return {};
  }
  const auto found = models->titles.find(title_id);
  if (found == models->titles.end()) {
    return {};
  }

  std::vector<Achievement> result;

  for (auto id : found->second.GetAchievementsIds()) {
    auto entry = found->second.GetAchievementEntry(id);
    if (!entry) {
      continue;
    }
    Achievement achievement(entry);

    achievement.achievement_name = found->second.GetAchievementTitle(id);
    achievement.unlocked_description =
        found->second.GetAchievementDescription(id);
    achievement.locked_description =
        found->second.GetAchievementUnachievedDescription(id);
    result.push_back(std::move(achievement));
  }

  return result;
};

std::vector<uint8_t> UserTracker::GetAchievementIcon(
    uint64_t xuid, uint32_t title_id, uint32_t achievement_id) const {
  auto models = Models(xuid);
  if (!models) {
    return {};
  }
  const auto game = models->titles.find(title_id);
  if (game == models->titles.end()) {
    return {};
  }

  const auto entry = game->second.GetAchievementEntry(achievement_id);
  return entry ? Owned(game->second.GetImage(entry->image_id))
               : std::vector<uint8_t>{};
}

void UserTracker::AddProperty(const uint64_t xuid, const Property* property) {
  auto spa = Spa();
  if (!spa) {
    return;
  }
  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user) {
    return;
  }

  const auto property_id = property->GetPropertyId();

  if (property->IsContext()) {
    const auto context_data = spa->GetContext(property_id.value);
    if (!context_data) {
      return;
    }
  } else {
    const auto property_data = spa->GetProperty(property_id.value);
    if (!property_data) {
      return;
    }
  }

  std::lock_guard prop_lock(user->prop_mutex);

  auto entry = std::ranges::find_if(
      user->properties_, [property_id](const Property& property_data) {
        return property_data.GetPropertyId().value == property_id.value;
      });

  if (entry != user->properties_.end()) {
    *entry = *property;
    return;
  }

  user->properties_.push_back(*property);
}

X_STATUS UserTracker::GetProperty(const uint64_t xuid, uint32_t* property_size,
                                  XUSER_PROPERTY* property) {
  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user) {
    return X_E_NOTFOUND;
  }

  *property_size = 0;
  const auto& property_id = property->property_id;

  std::lock_guard prop_lock(user->prop_mutex);

  const auto entry = std::ranges::find_if(
      std::as_const(user->properties_),
      [property_id](const Property& property_data) {
        return property_data.GetPropertyId().value == property_id;
      });

  if (entry == user->properties_.cend()) {
    return X_E_INVALIDARG;
  }

  if (entry->requires_additional_data()) {
    if (!property->data.data.binary.ptr) {
      return X_E_INVALIDARG;
    }
  }

  *property_size = static_cast<uint32_t>(entry->get_data_size());
  entry->WriteToGuest(property);
  return X_E_SUCCESS;
}

const Property* UserTracker::GetProperty(const uint64_t xuid,
                                         const uint32_t id) const {
  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user) {
    return nullptr;
  }

  std::lock_guard prop_lock(user->prop_mutex);

  const auto entry = std::ranges::find_if(
      std::as_const(user->properties_), [id](const Property& property_data) {
        return property_data.GetPropertyId().value == id;
      });

  if (entry == user->properties_.cend()) {
    return nullptr;
  }

  return &(*entry);
}

std::optional<UserSetting> UserTracker::GetGpdSetting(
    UserProfile* user, uint32_t title_id, uint32_t setting_id) const {
  if (!user) {
    return {};
  }
  auto models = Models(user->xuid());
  if (!models) {
    return {};
  }
  if (auto game = models->titles.find(title_id); game != models->titles.end()) {
    if (auto setting = game->second.GetSetting(setting_id)) {
      return UserSetting(
          setting, game->second.GetSettingData(setting_id));
    }
  }
  if (auto setting = models->dashboard.GetSetting(setting_id)) {
    return UserSetting(
        setting, models->dashboard.GetSettingData(setting_id));
  }

  return {};
}

std::optional<UserSetting> UserTracker::GetSetting(UserProfile* user,
                                                   uint32_t title_id,
                                                   uint32_t setting_id) const {
  if (!user) {
    return {};
  }
  auto models = Models(user->xuid());
  if (!models) {
    return {};
  }
  std::error_code error;
  return models->Setting(title_id, setting_id, error);
}

bool UserTracker::GetUserSetting(uint64_t xuid, uint32_t title_id,
                                 uint32_t setting_id,
                                 X_USER_PROFILE_SETTING* setting_ptr,
                                 uint32_t& extended_data_address) const {
  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user) {
    return false;
  }

  auto setting = GetSetting(user.get(), title_id, setting_id);
  if (!setting) {
    return false;
  }
  setting_ptr->setting_id = setting_id;
  setting_ptr->source = setting->get_setting_source();

  setting->WriteToGuest(setting_ptr, extended_data_address);
  return true;
}

std::pair<uint32_t, uint32_t> UserTracker::GetUserSubscriptionData(
    UserProfile* user) const {
  std::pair<uint32_t, uint32_t> data;
  if (const auto setting = GetSetting(
          user, kDashboardID,
          static_cast<uint32_t>(
              UserSettingId::XPROFILE_SUBSCRIPTION_TYPE_LENGTH_IN_MONTHS))) {
    data.first = std::get<int32_t>(setting->get_host_data());
  } else {
    data.first = 0;
  }

  if (const auto setting =
          GetSetting(user, kDashboardID,
                     static_cast<uint32_t>(
                         UserSettingId::XPROFILE_SUBSCRIPTION_PAYMENT_TYPE))) {
    data.second = std::get<int32_t>(setting->get_host_data());
  } else {
    data.second = 0;
  }

  return data;
}

void UserTracker::UpdateContext(uint64_t xuid, uint32_t id, uint32_t value) {
  auto spa = Spa();
  if (!spa) {
    return;
  }
  if (!IsUserTracked(xuid)) {
    return;
  }

  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user || !spa) {
    return;
  }

  const auto& context_data = spa->GetContext(id);
  if (!context_data) {
    return;
  }

  if (value > context_data->max_value) {
    return;
  }

  std::lock_guard prop_lock(user->prop_mutex);

  const auto entry = std::ranges::find_if(
      user->properties_, [id](const Property& property_data) {
        return property_data.IsContext() &&
               property_data.GetPropertyId().value == id;
      });

  if (entry != user->properties_.cend()) {
    *entry = Property(id, value);
    return;
  }

  user->properties_.push_back(Property(id, value));
}

std::optional<uint32_t> UserTracker::GetUserContext(uint64_t xuid,
                                                    uint32_t id) const {
  auto spa = Spa();
  if (!spa) {
    return std::nullopt;
  }
  if (!IsUserTracked(xuid)) {
    return std::nullopt;
  }

  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user || !spa) {
    return std::nullopt;
  }

  const auto& context_data = spa->GetContext(id);
  if (!context_data) {
    return std::nullopt;
  }

  std::lock_guard prop_lock(user->prop_mutex);

  const auto entry = std::ranges::find_if(
      std::as_const(user->properties_), [id](const Property& property_data) {
        return property_data.get_type() == X_USER_DATA_TYPE::CONTEXT &&
               property_data.GetPropertyId().value == id;
      });

  if (entry == user->properties_.cend()) {
    return std::nullopt;
  }

  return entry->get_data()->data.u32;
}

std::vector<AttributeKey> UserTracker::GetUserContextIds(uint64_t xuid) const {
  if (!IsUserTracked(xuid)) {
    return {};
  }

  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user) {
    return {};
  }

  std::vector<AttributeKey> entries;

  std::lock_guard prop_lock(user->prop_mutex);

  for (const auto& property : user->properties_) {
    if (!property.IsContext()) {
      continue;
    }

    entries.push_back(property.GetPropertyId());
  }
  return entries;
}

std::vector<AttributeKey> UserTracker::GetUserPropertyIds(uint64_t xuid) const {
  if (!IsUserTracked(xuid)) {
    return {};
  }

  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user) {
    return {};
  }

  std::lock_guard prop_lock(user->prop_mutex);

  std::vector<AttributeKey> entries;

  for (const auto& property : user->properties_) {
    if (property.IsContext()) {
      continue;
    }

    entries.push_back(property.GetPropertyId());
  }
  return entries;
}

void UserTracker::UpdateSettingValue(uint64_t xuid, uint32_t title_id,
                                     UserSettingId id,
                                     int32_t difference) {
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kAdjustSetting;
  command.title_id = title_id;
  command.record_id = uint32_t(id);
  command.delta = difference;
  Mutate(xuid, std::move(command));
}

bool UserTracker::UpsertSetting(uint64_t xuid, uint32_t title_id,
                                const UserSetting* setting) {
  if (!setting) {
    return false;
  }
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kSetSetting;
  command.title_id = title_id ? title_id : kernel_state()->title_id();
  command.setting = *setting;
  return Committed(Mutate(xuid, std::move(command)));
}

bool UserTracker::UpdateUserIcon(uint64_t xuid,
                                 std::span<const uint8_t> icon_data) {
  auto user = kernel_state()->xam_state()->profile_manager()->CaptureProfile(xuid);
  if (!user) {
    return false;
  }

  int width, height, channels;
  if (!stbi_info_from_memory(icon_data.data(),
                             static_cast<int>(icon_data.size()), &width,
                             &height, &channels)) {
    return false;
  }

  XTileType icon_type = XTileType::kGameIcon;

  if (std::pair<uint16_t, uint16_t>(width, height) == kProfileIconSize) {
    icon_type = XTileType::kPersonalGamerTile;
  } else if (std::pair<uint16_t, uint16_t>(width, height) ==
             kProfileIconSizeSmall) {
    icon_type = XTileType::kPersonalGamerTileSmall;
  } else {
    return false;
  }

  user->WriteProfileIcon(icon_type, icon_data);
  return true;
}

std::vector<uint8_t> UserTracker::GetIcon(uint64_t xuid, uint32_t title_id,
                                              XTileType tile_type,
                                              uint64_t tile_id) const {
  if (!title_id) {
      title_id = kernel_state()->title_id();
    }
  auto spa = kernel_state()->xam_state()->AcquireSpaInfo();
  if (tile_type == XTileType::kAchievement && spa &&
      title_id == spa->title_id()) {
        return Owned(spa->GetIcon(uint32_t(tile_id)));
      }
  if (tile_type == XTileType::kAchievement ||
      tile_type == XTileType::kGameIcon) {
    auto models = Models(xuid);
        if (!models) {
          return {};
        }
    auto found = models->titles.find(title_id);
    return found == models->titles.end()
               ? std::vector<uint8_t>{}
               : Owned(found->second.GetImage(tile_id));
  }

  return kernel_state()->xam_state()->profile_manager()->CaptureProfileIcon(
      xuid, tile_type);
}

void UserTracker::RefreshTitleSummary(uint64_t xuid, uint32_t title_id) {
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kRefreshSummary;
  command.title_id = title_id;
  const auto result = Mutate(xuid, std::move(command));
  if (result.error) {
    XELOGW("Profile summary mutation pending/refused: {}",
           result.error.message());
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
