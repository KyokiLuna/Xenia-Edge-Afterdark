/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/local_profile_initial_recipe.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
namespace xe::kernel::xam {
namespace {
std::error_code Error(std::errc value) { return std::make_error_code(value); }
bool NameValid(std::u16string_view name) {
  if (name.empty() || name.size()>15) { return false; }
  for (size_t i=0;i<name.size();++i) {
    const auto c=uint16_t(name[i]);
    if (!c || (c>=0xDC00 && c<=0xDFFF)) { return false; }
    if (c>=0xD800 && c<=0xDBFF) {
      if (++i==name.size() || name[i]<0xDC00 || name[i]>0xDFFF) { return false; }
    }
  }
  return true;
}
std::error_code CheckPicture(const TileService::OwnedPng& png,uint32_t side) {
  if (png.status!=X_E_SUCCESS || png.bytes.empty()) { return Error(std::errc::io_error); }
  const auto decoded=TileService::PrepareTexture(png,side*4,side);
  return decoded.status==X_E_SUCCESS && decoded.width==side && decoded.height==side
      ? std::error_code{} : Error(std::errc::bad_message);
}
}  // namespace
std::optional<LocalProfileInitialRecipe> BuildInitialLocalProfile(
    const X_XAMACCOUNTINFO& account, uint64_t xuid,
    std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> installed,
    uint32_t preset_index, uint32_t stock_picture_index, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error=Error(std::errc::device_or_resource_busy); return {};
  }
  if (!xuid || preset_index>=StockAvatarPresets::kCount ||
      stock_picture_index>=TileService::kStockCount) {
    error=Error(std::errc::invalid_argument); return {};
  }
  // This gate creates local profiles. Preserve typed opaque account data;
  // never provision remote credentials/Gold or silently strip a caller's Live.
  if (account.IsLiveEnabled() || account.GetOnlineXUID() ||
      account.GetSubscriptionTier()!=X_XAMACCOUNTINFO::kSubscriptionTierNone) {
    error=Error(std::errc::operation_not_supported); return {};
  }
  const auto name=xe::string_util::read_u16string_and_swap(
      account.gamertag,xe::countof(account.gamertag));
  if (!NameValid(name)) { error=Error(std::errc::invalid_argument); return {}; }
  try {
    const auto presets=StockAvatarPresets::Capture(installed,error);
    if (!presets) { return {}; }
    const auto* preset=presets->Get(preset_index);
    if (!preset) { error=Error(std::errc::invalid_argument); return {}; }
    X_HRESULT tile_status;
    const auto tiles=TileService::CaptureStockSource(installed,tile_status);
    if (!tiles || tile_status!=X_E_SUCCESS) {
      error=Error(std::errc::no_such_device); return {};
    }
    auto large=TileService::ReadStock(*tiles,{stock_picture_index,false});
    auto small=TileService::ReadStock(*tiles,{stock_picture_index,true});
    if ((error=CheckPicture(large,64)) || (error=CheckPicture(small,32))) { return {}; }
    if (large.source_generation!=installed->generation() ||
        small.source_generation!=installed->generation()) {
      error=Error(std::errc::state_not_recoverable); return {};
    }
    // Same new-retail Account format as ProfileAccountWriter::Create. New
    // account bytes contain the complete input struct. No existing suffix or
    // devkit flavor is inferred/replaced; this is not a repair operation.
    std::vector<uint8_t> account_bytes(ProfileAccountSnapshot::kPrefixBytes);
    ProfileManager::EncryptAccountFile(&account,account_bytes.data(),false);
    X_XAMACCOUNTINFO verified{};
    if (!ProfileManager::DecryptAccountFile(account_bytes,&verified,false) ||
        std::memcmp(&verified,&account,sizeof(account))) {
      error=Error(std::errc::bad_message); return {};
    }
    auto appearance=preset->appearance;  // Never mutate the installed preset.
    if ((error=NormalizeAvatarManifest(appearance,xuid))) { return {}; }
    GpdInfoProfile dashboard;
    // Recovered native17559 missing-setting defaults, not current legacy Gold
    // fallback or a blanket persisted collection of zero/empty binary records.
    const std::array<std::pair<UserSettingId,int32_t>,5> defaults = {{
        {UserSettingId::XPROFILE_OPTION_VOICE_VOLUME,100},
        {UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION,3},
        {UserSettingId::XPROFILE_GAMER_ACTION_AUTO_AIM,1},
        {UserSettingId::XPROFILE_USER_PREFERENCES,3},
        {UserSettingId::XPROFILE_GAMER_TIER,0}}};
    for (const auto& [id,value]:defaults) {
      if ((error=dashboard.UpsertSettingPreserving(UserSetting(id,value)))) { return {}; }
    }
    const auto key=xe::to_utf16(fmt::format("{:08X}{:08X}{:08X}",
        kDashboardID,0x00020000u|stock_picture_index,0x00010000u|stock_picture_index));
    if ((error=dashboard.UpsertSettingPreserving(UserSetting(
        UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY,key)))) { return {}; }
    if ((error=dashboard.UpsertSettingPreserving(UserSetting(
        UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1,
        std::vector<uint8_t>(appearance.begin(),appearance.end()))))) { return {}; }
    // Native F3 writes44 only. Fresh45 is absent/typed-empty by native default;
    // duplicate/zero1000-byte INFO2 would invent an appearance. Existing45 is
    // never consumed or edited by this new-profile-only producer.
    std::vector<uint8_t> dashboard_bytes;
    if ((error=dashboard.SerializeChecked(dashboard_bytes,GpdSnapshotStore::kMaximumBytes)) ||
        (error=ValidateGpdSnapshot(dashboard_bytes)) ||
        (error=ValidateProfileGpdRecords(dashboard_bytes,true))) { return {}; }
    GpdInfoProfile reparsed(dashboard_bytes);
    std::optional<UserSetting> read;
    if (UserSetting::ReadFromGpd(reparsed,
            uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1),read)!=X_ERROR_SUCCESS ||
        !read || read->get_extended_data().size()!=appearance.size() ||
        !std::equal(read->get_extended_data().begin(),read->get_extended_data().end(),
                    appearance.begin())) {
      error=Error(std::errc::bad_message); return {};
    }
    LocalProfileInitialRecipe out;
    out.presets=presets; out.preset_index=preset_index;
    out.descriptor.device_id=0;  // Selected managed source fills actual device.
    out.descriptor.content_type=XContentType::kProfile;
    out.descriptor.title_id=kDashboardID;
    out.descriptor.xuid=xuid;
    out.descriptor.set_file_name(fmt::format("{:016X}",xuid));
    out.descriptor.set_display_name(name);
    out.members.push_back({"Account",std::move(account_bytes),false});
    out.members.push_back({"FFFE07D1.gpd",std::move(dashboard_bytes),false});
    out.members.push_back({"tile_64.png",std::move(large.bytes),false});
    out.members.push_back({"tile_32.png",std::move(small.bytes),false});
    // Genuine shipped avatar preview; a live rendered/generated avatar tile is
    // a separate engine result and cannot be claimed from this resource PNG.
    out.members.push_back({"avtr_64.png",preset->preview_64_png,false});
    if ((error=presets->CheckAvailable())) { return {}; }
    return out;
  } catch (const std::bad_alloc&) { error=Error(std::errc::not_enough_memory); return {}; }
}
}  // namespace xe::kernel::xam
