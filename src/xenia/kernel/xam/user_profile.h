/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_USER_PROFILE_H_
#define XENIA_KERNEL_XAM_USER_PROFILE_H_
#include <atomic>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "xenia/kernel/xam/user_property.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_info_title.h"
#include "xenia/kernel/xam/xdbf/xdbf_io.h"

namespace xe {
namespace kernel {
namespace xam {
class ProfileGpdModelAuthority;
struct ProfileGpdReadModels;

enum class X_USER_PROFILE_SETTING_SOURCE : uint32_t {
  NO_VALUE = 0,
  DEFAULT = 1,  // Default value taken from default OS values.
  TITLE = 2,    // Value written by title or OS.
  PERMISSION_DENIED = 3,
};

struct X_USER_PROFILE_SETTING {
  xe::be<X_USER_PROFILE_SETTING_SOURCE> source;
  union {
    xe::be<uint32_t> user_index;
    xe::be<uint64_t> xuid;
  };
  xe::be<uint32_t> setting_id;
  union {
    uint8_t data_bytes[sizeof(X_USER_DATA)];
    X_USER_DATA data;
  };
};
static_assert_size(X_USER_PROFILE_SETTING, 40);

enum class X_USER_PROFILE_GAMERCARD_ZONE_OPTIONS {
  GAMERCARD_ZONE_NONE,
  GAMERCARD_ZONE_RR,
  GAMERCARD_ZONE_PRO,
  GAMERCARD_ZONE_FAMILY,
  GAMERCARD_ZONE_UNDERGROUND
};

enum class XTileType {
  kAchievement,
  kGameIcon,
  kGamerTile,
  kGamerTileSmall,
  kLocalGamerTile,
  kLocalGamerTileSmall,
  kBkgnd,
  kAwardedGamerTile,
  kAwardedGamerTileSmall,
  kGamerTileByImageId,
  kPersonalGamerTile,
  kPersonalGamerTileSmall,
  kGamerTileByKey,
  kAvatarGamerTile,
  kAvatarGamerTileSmall,
  kAvatarFullBody
};

// TODO: find filenames of other tile types that are stored in profile
inline const std::map<XTileType, std::string> kTileFileNames = {
    {XTileType::kGamerTile, "tile_64.png"},
    {XTileType::kGamerTileSmall, "tile_32.png"},
    {XTileType::kLocalGamerTile, "tile_64.png"},
    {XTileType::kLocalGamerTileSmall, "tile_32.png"},
    {XTileType::kAwardedGamerTile, "64_{:08x}{:08x}{:08x}.png"},
    {XTileType::kAwardedGamerTileSmall, "32_{:08x}{:08x}{:08x}.png"},
    {XTileType::kGamerTileByImageId, "{:d}_{:08x}{:08x}{:08x}.png"},
    {XTileType::kPersonalGamerTile, "pp_64.png"},
    {XTileType::kPersonalGamerTileSmall, "pp_32.png"},
    {XTileType::kAvatarGamerTile, "avtr_64.png"},
    {XTileType::kAvatarGamerTileSmall, "avtr_32.png"},
};

static constexpr std::pair<uint16_t, uint16_t> kProfileIconSize = {64, 64};
static constexpr std::pair<uint16_t, uint16_t> kProfileIconSizeSmall = {32, 32};

enum class SignInState : uint32_t {
  NotSignedIn,
  SignedInLocally,  // Offline
  SignedInToLive,   // Online
};

class UserProfile {
 public:
  UserProfile(const uint64_t xuid, const X_XAMACCOUNTINFO* account_info,
              std::shared_ptr<ProfileGpdModelAuthority> model);

  uint64_t xuid() const { return xuid_; }
  uint64_t GetOnlineXUID() const {
    const auto account = account_info_.load(std::memory_order_acquire);
    return account->IsLiveEnabled() ? static_cast<uint64_t>(account->xuid_online)
                                    : 0;
  }
  // Titles know a user signed in to Live by the online XUID.
  uint64_t GetLogonXUID() const {
    return signin_state() == static_cast<uint32_t>(SignInState::SignedInToLive)
               ? GetOnlineXUID()
               : xuid_;
  }

  std::string name() const { return account_info_.load(std::memory_order_acquire)->GetGamertagString(); }
  // Account Live eligibility and subscription metadata do not establish an
  // active service connection. Assigned users remain local until a real
  // connected-service provider can publish actual sign-in state.
  uint32_t signin_state() const {
    return static_cast<uint32_t>(SignInState::SignedInLocally);
  };

  uint32_t GetReservedFlags() const {
    return account_info_.load(std::memory_order_acquire)->GetReservedFlags();
  };
  uint32_t GetCachedFlags() const { return account_info_.load(std::memory_order_acquire)->GetCachedFlags(); };
  uint32_t GetCountry() const {
    return static_cast<uint32_t>(account_info_.load(std::memory_order_acquire)->GetCountry());
  };
  uint32_t GetSubscriptionTier() const {
    return account_info_.load(std::memory_order_acquire)->GetSubscriptionTier();
  }
  uint32_t GetLanguage() const {
    return static_cast<uint32_t>(account_info_.load(std::memory_order_acquire)->GetLanguage());
  };

  bool IsParentalControlled() const {
    return account_info_.load(std::memory_order_acquire)->IsParentalControlled();
  };
  bool IsLiveEnabled() const { return account_info_.load(std::memory_order_acquire)->IsLiveEnabled(); }

  void ClearProfileIcon(XTileType icon_type) {
    profile_images_.erase(icon_type);
  }

  std::vector<uint8_t> GetProfileIcon(XTileType icon_type) const {
    std::unique_lock lock(profile_images_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return {};
    }
    auto found = profile_images_.find(icon_type);
    if (found != profile_images_.end()) {
      return found->second;
    }
    if (icon_type == XTileType::kPersonalGamerTile ||
        icon_type == XTileType::kLocalGamerTile) {
      icon_type = XTileType::kGamerTile;
    }
    if (icon_type == XTileType::kPersonalGamerTileSmall ||
               icon_type == XTileType::kLocalGamerTileSmall) {
      icon_type = XTileType::kGamerTileSmall;
    }
    found = profile_images_.find(icon_type);
    return found == profile_images_.end() ? std::vector<uint8_t>{}
                                          : found->second;
  }

  void GetPasscode(uint16_t* passcode) const {
    const auto account = account_info_.load(std::memory_order_acquire);
    std::memcpy(passcode, account->passcode, sizeof(account->passcode));
  };

  // Owned immutable projections; no GPD const-reference escapes a guard.
  std::shared_ptr<const GpdInfoProfile> dashboard_gpd() const;
  std::shared_ptr<const ProfileGpdReadModels> CaptureGpds() const;
  std::vector<uint8_t> GetTitleIcon(uint32_t title_id) const;

  // Public accessor for getting achievement stats from title GPD
  struct TitleAchievementStats {
    uint32_t achievements_total = 0;
    uint32_t achievements_unlocked = 0;
    uint32_t gamerscore_total = 0;
    uint32_t gamerscore_earned = 0;
  };

  TitleAchievementStats GetTitleAchievementStats(uint32_t title_id) const;

  friend class UserTracker;
  friend class GpdAchievementBackend;
  friend class ProfileManager;

 private:
  uint64_t xuid_;
  std::atomic<std::shared_ptr<const X_XAMACCOUNTINFO>> account_info_;

  std::shared_ptr<ProfileGpdModelAuthority> gpd_model_;
  std::vector<Property> properties_;  // Includes contexts!
  std::mutex prop_mutex;              // 5841089C

  mutable std::mutex profile_images_mutex_;
  std::map<XTileType, std::vector<uint8_t>> profile_images_;

  std::shared_ptr<const GpdInfo> GetGpd(uint32_t title_id) const;
  void LoadProfileIcon(XTileType tile_type);
  void WriteProfileIcon(XTileType tile_type,
                        std::span<const uint8_t> icon_data);
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_USER_PROFILE_H_
