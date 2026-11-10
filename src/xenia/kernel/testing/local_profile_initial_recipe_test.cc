/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/string.h"
#include "xenia/kernel/xam/avatar_manifest.h"
#include "xenia/kernel/xam/local_profile_initial_recipe.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
namespace xe::kernel::xam::test {
TEST_CASE("Avatar manifest body type requires the complete actual native body GUID",
          "[kernel][profile_creation][avatar_manifest]") {
  std::array<uint8_t,kAvatarManifestBytes> bytes{};
  const std::array<uint8_t,16> male={
      0,0,0,2,0,0,0,1,0xC1,0xC8,0xF1,9,0xA1,0x9C,0xB2,0xE0};
  auto female=male; female[5]=1; female[7]=2;
  CHECK(AvatarManifestBodyType(bytes)==AvatarBodyType::kUnknown);
  std::copy(male.begin(),male.end(),bytes.begin()+0x120);
  CHECK(AvatarManifestBodyType(bytes)==AvatarBodyType::kMale);
  bytes[0x12F]^=1;
  CHECK(AvatarManifestBodyType(bytes)==AvatarBodyType::kUnknown);
  std::copy(female.begin(),female.end(),bytes.begin()+0x120);
  CHECK(AvatarManifestBodyType(bytes)==AvatarBodyType::kFemale);
  CHECK(AvatarManifestBodyType(std::span<const uint8_t>(bytes).first(999))==
        AvatarBodyType::kUnknown);
  CHECK(AvatarManifestBodyType({})==AvatarBodyType::kUnknown);
}
TEST_CASE("Avatar normalization changes only native owner bytes and preserves opaque appearance",
          "[kernel][profile_creation][avatar_manifest]") {
  std::array<uint8_t,kAvatarManifestBytes> bytes;
  for (size_t i=0;i<bytes.size();++i) { bytes[i]=uint8_t((i*37+11)%251); }
  const auto original=bytes;
  constexpr uint64_t xuid=0xE0123456789ABCDEull;
  REQUIRE_FALSE(NormalizeAvatarManifest(bytes,xuid));
  const std::array<uint8_t,13> expected={
      0xE0,0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0,0,0,0,0};
  REQUIRE(std::equal(expected.begin(),expected.end(),bytes.begin()+0x380));
  size_t untouched=0;
  for (size_t i=0;i<bytes.size();++i) {
    if (i<0x380 || i>=0x38D) { REQUIRE(bytes[i]==original[i]); ++untouched; }
  }
  CHECK(untouched==987);
  const auto normalized=bytes;
  REQUIRE_FALSE(NormalizeAvatarManifest(bytes,xuid));
  CHECK(bytes==normalized);
}
TEST_CASE("Avatar normalization refuses invalid size or missing identity before mutation",
          "[kernel][profile_creation][avatar_manifest]") {
  std::array<uint8_t,1001> bytes; bytes.fill(0xB7);
  const auto original=bytes;
  CHECK(NormalizeAvatarManifest(bytes,1)==std::errc::invalid_argument);
  CHECK(bytes==original);
  CHECK(NormalizeAvatarManifest(std::span<uint8_t>(bytes).first(999),1)==
        std::errc::invalid_argument);
  CHECK(bytes==original);
  CHECK(NormalizeAvatarManifest(std::span<uint8_t>(bytes).first(1000),0)==
        std::errc::invalid_argument);
  CHECK(bytes==original);
  CHECK(NormalizeAvatarManifest({},1)==std::errc::invalid_argument);
}
namespace {
struct RecipeScratch {
  std::filesystem::path path;
  RecipeScratch() {
    static std::atomic<uint64_t> sequence{0};
    path=std::filesystem::temp_directory_path()/
        ("xenia-profile-recipe-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
         "-"+std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~RecipeScratch() { std::error_code e; std::filesystem::remove_all(path,e); }
};
const std::vector<uint8_t>& Member(const LocalProfileInitialRecipe& recipe,
                                   const std::filesystem::path& name) {
  const auto found=std::find_if(recipe.members.begin(),recipe.members.end(),
      [&](const auto& row) { return row.relative==name; });
  REQUIRE(found!=recipe.members.end()); REQUIRE_FALSE(found->directory);
  return found->bytes;
}
int32_t Scalar(const GpdInfoProfile& gpd,UserSettingId id) {
  std::optional<UserSetting> row;
  REQUIRE(UserSetting::ReadFromGpd(gpd,uint32_t(id),row)==X_ERROR_SUCCESS);
  REQUIRE(row); REQUIRE(row->get_type()==X_USER_DATA_TYPE::INT32);
  return int32_t(row->get_data()->data.s32);
}
}  // namespace
// Optional environment, mandatory genuine input if selected. No fixture assets
// are redistributed. A missing source is a failed prerequisite, never a skip.
TEST_CASE("Installed profile defaults preserve authentic presets complete Account and checked GPD",
          "[.installed-profile-defaults][kernel][profile_creation][persistence]") {
  const char* selected=std::getenv("XENIA_TEST_SYSTEM_SOURCE_ROOT");
  REQUIRE(selected); REQUIRE(*selected);
  RecipeScratch scratch;
  std::error_code error;
  auto system=vfs::InstalledSystemSource::Create(scratch.path/"System",selected,error);
  REQUIRE(system); REQUIRE_FALSE(error);
  const auto snapshot=system->snapshot();
  REQUIRE(snapshot); REQUIRE(snapshot->available());
  const auto presets=StockAvatarPresets::Capture(snapshot,error);
  REQUIRE(presets); REQUIRE_FALSE(error);
  unsigned male=0,female=0;
  for (uint32_t i=0;i<StockAvatarPresets::kCount;++i) {
    const auto* preset=presets->Get(i); REQUIRE(preset);
    REQUIRE(preset->appearance.size()==1000);
    CHECK(AvatarManifestBodyType(preset->appearance)==preset->body_type);
    const auto image=TileService::PrepareTexture(
        TileService::FromOwnedPng(preset->preview_64_png),256,64);
    REQUIRE(image.status==X_E_SUCCESS); CHECK(image.width==64); CHECK(image.height==64);
    if (preset->body_type==AvatarBodyType::kMale) { ++male; }
    if (preset->body_type==AvatarBodyType::kFemale) { ++female; }
  }
  CHECK(male==6); CHECK(female==6); CHECK_FALSE(presets->Get(12));
  const auto original0=presets->Get(0)->appearance;
  const auto original1=presets->Get(1)->appearance;
  X_XAMACCOUNTINFO account{};
  xe::string_util::copy_and_swap_truncating(account.gamertag,u"NewProfile",16);
  account.passcode[0]=0x51;  // Opaque typed Account bytes survive unchanged.
  constexpr uint64_t xuid=0xE000000000000123ull;
  const auto recipe=BuildInitialLocalProfile(account,xuid,snapshot,0,2,error);
  REQUIRE(recipe); REQUIRE_FALSE(error);
  CHECK(recipe->descriptor.device_id==0); // Actual selected source fills this.
  CHECK(recipe->descriptor.content_type==XContentType::kProfile);
  CHECK(recipe->descriptor.title_id==kDashboardID);
  CHECK(recipe->descriptor.xuid==xuid);
  CHECK(recipe->descriptor.file_name()=="E000000000000123");
  CHECK(recipe->descriptor.display_name()==u"NewProfile");
  const auto& encoded=Member(*recipe,"Account");
  REQUIRE(encoded.size()==ProfileAccountSnapshot::kPrefixBytes);
  X_XAMACCOUNTINFO decoded{};
  REQUIRE(ProfileManager::DecryptAccountFile(encoded,&decoded,false));
  CHECK(std::memcmp(&decoded,&account,sizeof(account))==0);
  const auto& wire=Member(*recipe,"FFFE07D1.gpd");
  REQUIRE_FALSE(ValidateGpdSnapshot(wire)); REQUIRE_FALSE(ValidateProfileGpdRecords(wire,true));
  GpdInfoProfile gpd(wire);
  CHECK(Scalar(gpd,UserSettingId::XPROFILE_GAMER_TIER)==0);
  CHECK(Scalar(gpd,UserSettingId::XPROFILE_OPTION_VOICE_VOLUME)==100);
  CHECK(Scalar(gpd,UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION)==3);
  CHECK(Scalar(gpd,UserSettingId::XPROFILE_GAMER_ACTION_AUTO_AIM)==1);
  CHECK(Scalar(gpd,UserSettingId::XPROFILE_USER_PREFERENCES)==3);
  std::optional<UserSetting> row;
  REQUIRE(UserSetting::ReadFromGpd(gpd,uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1),row)==X_ERROR_SUCCESS);
  REQUIRE(row); REQUIRE(row->get_extended_data().size()==1000);
  auto normalized=original0; REQUIRE_FALSE(NormalizeAvatarManifest(normalized,xuid));
  CHECK(std::equal(row->get_extended_data().begin(),row->get_extended_data().end(),normalized.begin()));
  row.reset();
  REQUIRE(UserSetting::ReadFromGpd(gpd,uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_2),row)==X_ERROR_SUCCESS);
  CHECK_FALSE(row);
  row.reset();
  REQUIRE(UserSetting::ReadFromGpd(gpd,uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY),row)==X_ERROR_SUCCESS);
  REQUIRE(row);
  const UserSetting expected_key(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY,std::u16string(u"FFFE07D10002000200010002"));
  CHECK(row->Serialize()==expected_key.Serialize());
  X_HRESULT status;
  const auto stock=TileService::CaptureStockSource(snapshot,status);
  REQUIRE(stock); REQUIRE(status==X_E_SUCCESS);
  CHECK(Member(*recipe,"tile_64.png")==TileService::ReadStock(*stock,{2,false}).bytes);
  CHECK(Member(*recipe,"tile_32.png")==TileService::ReadStock(*stock,{2,true}).bytes);
  CHECK(Member(*recipe,"avtr_64.png")==presets->Get(0)->preview_64_png);
  const auto female_recipe=BuildInitialLocalProfile(account,xuid,snapshot,1,2,error);
  REQUIRE(female_recipe); REQUIRE_FALSE(error);
  GpdInfoProfile female_gpd(Member(*female_recipe,"FFFE07D1.gpd"));
  REQUIRE(UserSetting::ReadFromGpd(female_gpd,uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1),row)==X_ERROR_SUCCESS);
  REQUIRE(row); CHECK(AvatarManifestBodyType(row->get_extended_data())==AvatarBodyType::kFemale);
  CHECK(presets->Get(0)->appearance==original0); CHECK(presets->Get(1)->appearance==original1);
  const auto unsupported=account;
  account.ToggleLiveFlag(true);
  CHECK_FALSE(BuildInitialLocalProfile(account,xuid,snapshot,0,2,error));
  CHECK(error==std::errc::operation_not_supported);
  account=unsupported;
  CHECK_FALSE(BuildInitialLocalProfile(account,xuid,snapshot,12,2,error));
  CHECK(error==std::errc::invalid_argument);
}
}  // namespace xe::kernel::xam::test
