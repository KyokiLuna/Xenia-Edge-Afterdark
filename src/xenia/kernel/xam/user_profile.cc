/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/user_profile.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
std::vector<uint8_t> CaptureProfilePicture(const ProfileGpdReadCapture& read,
                                          XTileType type) {
  if (!read.pictures) { return {}; }
  const auto named = kTileFileNames.find(type);
  if (named == kTileFileNames.end()) { return {}; }
  auto found = read.pictures->find(named->second);
  if (found == read.pictures->end()) { return {}; }
  auto token = found->second;
  if (!token->exists()) {
    const char* fallback = nullptr;
    switch (type) {
      case XTileType::kPersonalGamerTile: case XTileType::kLocalGamerTile:
        fallback = "tile_64.png"; break;
      case XTileType::kPersonalGamerTileSmall: case XTileType::kLocalGamerTileSmall:
        fallback = "tile_32.png"; break;
      default: break;
    }
    if (fallback) {
      found = read.pictures->find(fallback);
      if (found != read.pictures->end()) { token = found->second; }
    }
  }
  return token->exists() ? std::vector<uint8_t>(token->bytes().begin(), token->bytes().end())
                         : std::vector<uint8_t>{};
}
std::vector<uint8_t> CaptureSelfProfilePicture(const ProfileGpdReadCapture& read) {
  auto bytes = CaptureProfilePicture(read, XTileType::kPersonalGamerTile);
  return bytes.empty() ? CaptureProfilePicture(read, XTileType::kPersonalGamerTileSmall)
                       : bytes;
}
std::vector<uint8_t> UserProfile::GetSelfProfileIcon() const {
  if (const auto model = gpd_model_.lock()) {
    std::error_code error; const auto read = model->CaptureRead(error);
    if (!read || error) { return {}; }
    // Large/small decisions belong to ONE immutable model/picture version.
    if (read->pictures) { return CaptureSelfProfilePicture(*read); }
  }
  // Legacy GPD-only compatibility also selects both sizes under ONE lock.
  std::unique_lock lock(profile_images_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { return {}; }
  const auto select = [&](XTileType personal, XTileType fallback)
      -> const std::vector<uint8_t>* {
    auto found = profile_images_.find(personal);
    if (found == profile_images_.end()) { found = profile_images_.find(fallback); }
    return found == profile_images_.end() ? nullptr : &found->second;
  };
  const auto* large = select(XTileType::kPersonalGamerTile, XTileType::kGamerTile);
  if (large && !large->empty()) { return *large; }
  const auto* small = select(XTileType::kPersonalGamerTileSmall, XTileType::kGamerTileSmall);
  return small ? *small : std::vector<uint8_t>{};
}
std::vector<uint8_t> UserProfile::GetProfileIcon(XTileType type) const {
  if (const auto model = gpd_model_.lock()) {
    std::error_code error; const auto read = model->CaptureRead(error);
    if (!read || error) { return {}; }
    if (read->pictures) { return CaptureProfilePicture(*read, type); }
  }
  // Compatibility for a legacy GPD-only object with no issued picture tuple.
  std::unique_lock lock(profile_images_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { return {}; }
  auto found = profile_images_.find(type);
  if (found != profile_images_.end()) { return found->second; }
  if (type == XTileType::kPersonalGamerTile || type == XTileType::kLocalGamerTile) {
    type = XTileType::kGamerTile;
  } else if (type == XTileType::kPersonalGamerTileSmall || type == XTileType::kLocalGamerTileSmall) {
    type = XTileType::kGamerTileSmall;
  }
  found = profile_images_.find(type);
  return found == profile_images_.end() ? std::vector<uint8_t>{} : found->second;
}

UserProfile::UserProfile(const uint64_t xuid,
                         const X_XAMACCOUNTINFO* account_info,
                         std::shared_ptr<ProfileGpdModelAuthority> model)
    : xuid_(xuid),
      account_info_(std::make_shared<const X_XAMACCOUNTINFO>(*account_info)),
      gpd_model_(std::move(model)),
      profile_images_() {
  // 58410A1F checks the user XUID against a mask of 0x00C0000000000000 (3<<54),
  // if non-zero, it prevents the user from playing the game.
  // "You do not have permissions to perform this operation."

  if (const auto owner = gpd_model_.lock()) {
    std::error_code error; const auto read = owner->CaptureRead(error);
    if (read && read->pictures) { return; }
  }
  // Legacy GPD-only compatibility cache.
  // Load default gamer tiles
  LoadProfileIcon(XTileType::kGamerTile);
  LoadProfileIcon(XTileType::kGamerTileSmall);

  // Also load personal gamer tiles (custom profile pictures)
  // Note: These use the same filenames as regular tiles, so if a personal
  // tile exists, it will overwrite the default tile file. We load both types
  // to maintain compatibility - the personal tile will be preferred when
  // present.
  LoadProfileIcon(XTileType::kPersonalGamerTile);
  LoadProfileIcon(XTileType::kPersonalGamerTileSmall);
  // Avatar picture consumers use the persisted attachment from the same
  // profile package, including after a full emulator restart.
  LoadProfileIcon(XTileType::kAvatarGamerTile);
  LoadProfileIcon(XTileType::kAvatarGamerTileSmall);
}

std::shared_ptr<const ProfileGpdReadModels> UserProfile::CaptureGpds() const {
  std::error_code error;
  auto owner = gpd_model_.lock();
  if (!owner) {
    return {};
  }
  const auto origin = owner->CaptureOrigin(error);
  if (!origin) {
    return {};
  }
  auto current =
      kernel_state()->xam_state()->profile_manager()->AcquireProfileGpdModel(
          xuid_, error);
  if (current != owner) {
    return {};
  }
  return kernel_state()->xam_state()->profile_manager()->CaptureGpdRead(xuid_,
                                                                        error);
}
std::shared_ptr<const GpdInfo> UserProfile::GetGpd(uint32_t title) const {
  auto models = CaptureGpds();
  if (!models) {
  return {};
}
  if (title == kDashboardID) {
    return {models, &models->dashboard};
  }
  const auto found = models->titles.find(title);
  return found == models->titles.end()
             ? std::shared_ptr<const GpdInfo>{}
             : std::shared_ptr<const GpdInfo>(models, &found->second);
}
std::shared_ptr<const GpdInfoProfile> UserProfile::dashboard_gpd() const {
  auto models = CaptureGpds();
  return models
             ? std::shared_ptr<const GpdInfoProfile>(models, &models->dashboard)
             : nullptr;
  }
std::vector<uint8_t> UserProfile::GetTitleIcon(uint32_t title) const {
  const auto gpd = GetGpd(title);
  if (!gpd) {
    return {};
  }
  const auto icon = gpd->GetImage(kXdbfIdTitle);
  return {icon.begin(), icon.end()};
}
UserProfile::TitleAchievementStats UserProfile::GetTitleAchievementStats(
    uint32_t title) const {
  TitleAchievementStats out;
  const auto models = CaptureGpds();
  if (!models) {
    return out;
  }
  const auto found = models->titles.find(title);
  if (found == models->titles.end()) {
    return out;
  }
  out.achievements_total = found->second.GetAchievementCount();
  out.achievements_unlocked = found->second.GetUnlockedAchievementCount();
  out.gamerscore_total = found->second.GetTotalGamerscore();
  out.gamerscore_earned = found->second.GetGamerscore();
  return out;
}

void UserProfile::LoadProfileIcon(XTileType tile_type) {
  if (!kTileFileNames.count(tile_type)) {
    return;
  }

  const std::string path =
      fmt::format("User_{:016X}:\\{}", xuid_, kTileFileNames.at(tile_type));

  vfs::File* file = nullptr;
  vfs::FileAction action;

  const X_STATUS result = kernel_state()->file_system()->OpenFile(
      nullptr, path, vfs::FileDisposition::kOpen, vfs::FileAccess::kGenericRead,
      false, true, &file, &action);

  if (result != X_STATUS_SUCCESS) {
    return;
  }

  const auto size = file->entry()->size();
  if (!size || size > 1024 * 1024) {
    file->Destroy();
    return;
  }
  std::vector<uint8_t> data(size);
  size_t read = 0;
  const auto read_status = file->ReadSync(data, 0, &read);
  file->Destroy();
  if (XFAILED(read_status) || read != data.size()) { return; }

  std::lock_guard image_lock(profile_images_mutex_);

  profile_images_.insert_or_assign(tile_type, data);
}


}  // namespace xam
}  // namespace kernel
}  // namespace xe
