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
  auto owner = gpd_model_;
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

void UserProfile::WriteProfileIcon(XTileType tile_type,
                                   std::span<const uint8_t> icon_data) {
  const std::string path =
      fmt::format("User_{:016X}:\\{}", xuid_, kTileFileNames.at(tile_type));

  vfs::File* file = nullptr;
  vfs::FileAction action;

  const X_STATUS result = kernel_state()->file_system()->OpenFile(
      nullptr, path, vfs::FileDisposition::kOverwriteIf,
      vfs::FileAccess::kGenericWrite | vfs::FileAccess::kFileWriteData, false,
      true, &file, &action);

  if (result != X_STATUS_SUCCESS) {
    return;
  }

  // Set the file length first to ensure we can write
  X_STATUS set_length_result = file->SetLength(icon_data.size());
  if (set_length_result != X_STATUS_SUCCESS &&
      set_length_result != X_STATUS_NOT_IMPLEMENTED) {
    XELOGW("WriteProfileIcon: SetLength failed with status {:08X}",
           set_length_result);
  }

  size_t written_bytes = 0;

  X_STATUS write_result =
      file->WriteSync({icon_data.data(), icon_data.size()}, 0, &written_bytes);
  if (write_result != X_STATUS_SUCCESS) {
    XELOGW("WriteProfileIcon: WriteSync failed with status {:08X}",
           write_result);
  } else {
    XELOGI("WriteProfileIcon: Successfully wrote {} bytes to {}", written_bytes,
           path);
  }

  file->Destroy();

  std::lock_guard image_lock(profile_images_mutex_);
  profile_images_.insert_or_assign(
      tile_type, std::vector<uint8_t>(icon_data.begin(), icon_data.end()));
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
