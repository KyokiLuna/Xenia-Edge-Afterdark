/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <thread>
#include <utility>
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/ui/windowed_app_context.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/mutex.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/base/string.h"
#include "xenia/base/managed_volume_authority.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/emulator.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/kernel/native_profile_save_publication.h"
#include "xenia/kernel/native_profile_save_owner.h"
#include "xenia/kernel/native_profile_save_gpd_stage.h"
#include "xenia/kernel/native_profile_save_picture_stage.h"
#include "xenia/kernel/native_profile_save_managed_gpd.h"
#include "xenia/kernel/native_profile_save_managed_picture.h"
#include "xenia/kernel/native_profile_gpd_read_view.h"
#include "xenia/kernel/xam/stock_avatar_presets.h"
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/kernel/xam/tile_request.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/profile_picture_codec.h"
#include "xenia/kernel/xam/folder_content_header.h"
#include "xenia/vfs/folder_content_package.h"
#include "xenia/kernel/testing/profile_picture_fixture.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/file_write_checkpoint.h"
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/kernel/util/io_request_completion.h"
#include "xenia/vfs/file.h"

namespace xe::kernel::xam::test {
namespace {
struct CreationRoot {
  std::filesystem::path path;
  CreationRoot() {
    static std::atomic<uint64_t> sequence{0};
    path=std::filesystem::temp_directory_path()/
        ("xenia-initial-profile-"+std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count())+"-"+std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
    std::filesystem::permissions(path,std::filesystem::perms::owner_all);
    REQUIRE(std::filesystem::create_directory(path/"content"));
    REQUIRE(std::filesystem::create_directory(path/"cache"));
  }
  ~CreationRoot() { std::error_code error; std::filesystem::remove_all(path,error); }
};
struct CreationRuntime {
  // Restored after actual Emulator teardown, root cleanup and constructor unwind.
  // This retains the direct runner CWD's real 20GiB protected filesystem policy.
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;
  CreationRoot root;
  std::unique_ptr<Emulator> emulator;
  explicit CreationRuntime(bool installed=false) {
    REQUIRE(KernelState::shared()==nullptr);
    if (installed) {
      const auto* source=std::getenv("XENIA_TEST_SYSTEM_SOURCE_ROOT");
      REQUIRE(source); REQUIRE(*source);
      REQUIRE(std::filesystem::is_directory(source));
      cvars::system_readonly_source_root=source;
    }
    Restart();
  }
  void Restart() {
    emulator.reset();
    REQUIRE(KernelState::shared()==nullptr);
    emulator=std::make_unique<Emulator>("",root.path,root.path/"content",root.path/"cache");
    REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
    REQUIRE(KernelState::shared()==emulator->kernel_state());
  }
  ~CreationRuntime() { emulator.reset(); }
  ProfileManager& profiles() { return *emulator->kernel_state()->xam_state()->profile_manager(); }
  ContentManager& content() { return *emulator->kernel_state()->xam_state()->content_manager(); }
};
struct CreationPage {
  Memory& memory;
  uint32_t address;
  explicit CreationPage(Memory& memory,uint32_t extent=0x3000):memory(memory),address(memory.SystemHeapAlloc(extent,0x1000)) {
    REQUIRE(address); std::memset(memory.TranslateVirtual(address),0xA5,extent);
  }
  ~CreationPage() { memory.SystemHeapFree(address); }
};
uint32_t RegionSize(Memory& memory,uint32_t address) {
  auto* heap=memory.LookupHeap(address); REQUIRE(heap);
  uint32_t size=0; REQUIRE(heap->QuerySize(address,&size)); return size;
}
PPCContext CreationContext(CreationRuntime& runtime,uint32_t stack) {
  PPCContext c{}; c.virtual_membase=runtime.emulator->memory()->virtual_membase();
  c.kernel_state=runtime.emulator->kernel_state();
  c.processor=runtime.emulator->processor(); c.r[1]=stack; return c;
}
uint32_t CallCreationExport(CreationRuntime& runtime,uint16_t ordinal,PPCContext& context) {
  const auto* entry=runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex",ordinal);
  REQUIRE(entry); REQUIRE(entry->function_data.trampoline);
  entry->function_data.trampoline(&context); return uint32_t(context.r[3]);
}
std::vector<uint8_t> CreationRead(const std::filesystem::path& path) {
  CAPTURE(path.string());
  std::ifstream file(path,std::ios::binary); REQUIRE(file);
  return {std::istreambuf_iterator<char>(file),{}};
}
uint32_t MissingCreationRecipeStatus(CreationRuntime& runtime) {
  const auto& installed = runtime.emulator->installed_system_source();
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  // POSIX Setup retains a valid empty local source, not a null source object.
  REQUIRE(installed);
#endif
  if (!installed) { return X_ERROR_FILE_NOT_FOUND; }
  const auto snapshot = installed->snapshot();
  REQUIRE(snapshot);
  REQUIRE_FALSE(snapshot->shared());
  REQUIRE(snapshot->valid());
  REQUIRE_FALSE(snapshot->available());
  std::error_code error;
  REQUIRE_FALSE(StockAvatarPresets::Capture(snapshot, error));
  REQUIRE(error == std::errc::no_such_device);
  // CreateProfileDetailed maps this unavailable required recipe to one exact
  // failure. FILE_NOT_FOUND is reserved for the null-object branch above.
  return X_ERROR_FUNCTION_FAILED;
}
void CheckNoCreatedProfile(CreationRuntime& runtime, uint64_t xuid) {
  REQUIRE(xuid != 0);
  const auto relative = ProfileStorageLease::ProfileRelative(xuid);
  REQUIRE_FALSE(relative.empty());
  const auto package = runtime.root.path / "content" / relative;
  REQUIRE_FALSE(std::filesystem::exists(package / "Account"));
  REQUIRE_FALSE(std::filesystem::exists(package / "FFFE07D1.gpd"));
  const auto header = package.parent_path().parent_path() / "Headers" /
      "00010000" / (package.filename().string() + ".header");
  REQUIRE_FALSE(std::filesystem::exists(header));
  REQUIRE_FALSE(std::filesystem::exists(package));
}
std::vector<uint8_t> Appearance(const GpdInfoProfile& gpd) {
  std::optional<UserSetting> value;
  REQUIRE(UserSetting::ReadFromGpd(gpd,uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1),value)==X_ERROR_SUCCESS);
  REQUIRE(value); REQUIRE(value->get_type()==X_USER_DATA_TYPE::BINARY);
  const auto bytes=value->get_extended_data(); REQUIRE(bytes.size()==1000);
  return {bytes.begin(),bytes.end()};
}
using CreationFiles=std::map<std::string,std::vector<uint8_t>>;
CreationFiles CheckCreatedProfile(CreationRuntime& runtime,uint64_t xuid,
    std::string_view name,const StockAvatarPresets::Preset& preset,uint32_t picture) {
  const auto package=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(xuid);
  CreationFiles files;
  for (const auto* leaf:{"Account","FFFE07D1.gpd","tile_64.png","tile_32.png","avtr_64.png"}) {
    files.emplace(leaf,CreationRead(package/leaf));
  }
  REQUIRE(files.size()==5);
  REQUIRE(files.at("Account").size()==ProfileAccountSnapshot::kPrefixBytes);
  X_XAMACCOUNTINFO account{};
  REQUIRE(ProfileManager::DecryptAccountFile(files.at("Account"),&account));
  REQUIRE(account.GetGamertagString()==name); REQUIRE_FALSE(account.IsLiveEnabled());
  REQUIRE(uint64_t(account.xuid_online)==0);
  REQUIRE(account.GetSubscriptionTier()==X_XAMACCOUNTINFO::kSubscriptionTierNone);
  REQUIRE_FALSE(ValidateGpdSnapshot(files.at("FFFE07D1.gpd")));
  REQUIRE_FALSE(ValidateProfileGpdRecords(files.at("FFFE07D1.gpd"),true));
  GpdInfoProfile gpd(files.at("FFFE07D1.gpd")); const auto appearance=Appearance(gpd);
  std::optional<UserSetting> picture_key;
  REQUIRE(UserSetting::ReadFromGpd(gpd,uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY),picture_key)==X_ERROR_SUCCESS);
  REQUIRE(picture_key);
  const auto key_bytes=picture_key->get_extended_data(); REQUIRE(key_bytes.size()==50);
  const std::array<uint32_t,3> key_tuple{kDashboardID,0x00020000u|picture,0x00010000u|picture};
  constexpr char hex[]="0123456789ABCDEF";
  for(size_t field=0;field!=key_tuple.size();++field) {
    for(size_t digit=0;digit!=8;++digit) {
      const size_t at=(field*8+digit)*2;
      REQUIRE(key_bytes[at]==0);
      REQUIRE(key_bytes[at+1]==uint8_t(hex[(key_tuple[field]>>((7-digit)*4))&15]));
    }
  }
  REQUIRE(key_bytes[48]==0); REQUIRE(key_bytes[49]==0);
  REQUIRE(AvatarManifestBodyType(appearance)==preset.body_type);
  // The native SetManifest normalization is independently checked byte by byte;
  // all unknown input bytes outside the XUID/clear range must survive unchanged.
  REQUIRE(xe::load_and_swap<uint64_t>(appearance.data()+0x380)==xuid);
  for (size_t i=0;i<appearance.size();++i) {
    if (i>=0x380 && i<0x388) { continue; }
    if (i>=0x388 && i<0x38D) { REQUIRE(appearance[i]==0); }
    else { REQUIRE(appearance[i]==preset.appearance[i]); }
  }
  std::optional<UserSetting> absent;
  REQUIRE(UserSetting::ReadFromGpd(gpd,uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_2),absent)==X_ERROR_SUCCESS);
  REQUIRE_FALSE(absent);  // Never synthesize a second zero/duplicated manifest.
  const auto& installed=runtime.emulator->installed_system_source(); REQUIRE(installed);
  X_HRESULT status;
  auto source=TileService::CaptureStockSource(installed->snapshot(),status);
  REQUIRE(source); REQUIRE(status==X_E_SUCCESS);
  const auto large=TileService::ReadStock(*source,{picture,false});
  const auto small=TileService::ReadStock(*source,{picture,true});
  REQUIRE(large.status==X_E_SUCCESS); REQUIRE(small.status==X_E_SUCCESS);
  REQUIRE(files.at("tile_64.png")==large.bytes); REQUIRE(files.at("tile_32.png")==small.bytes);
  const auto large_image=TileService::PrepareTexture(TileService::FromOwnedPng(files.at("tile_64.png")),256,64);
  const auto small_image=TileService::PrepareTexture(TileService::FromOwnedPng(files.at("tile_32.png")),128,32);
  REQUIRE(large_image.status==X_E_SUCCESS); REQUIRE(large_image.width==64); REQUIRE(large_image.height==64);
  REQUIRE(small_image.status==X_E_SUCCESS); REQUIRE(small_image.width==32); REQUIRE(small_image.height==32);
  REQUIRE(files.at("avtr_64.png")==preset.preview_64_png);
  const auto preview=TileService::PrepareTexture(TileService::FromOwnedPng(files.at("avtr_64.png")),256,64);
  REQUIRE(preview.status==X_E_SUCCESS); REQUIRE(preview.width==64); REQUIRE(preview.height==64);
  XCONTENT_AGGREGATE_DATA descriptor{};
  REQUIRE(runtime.content().ReadContentHeaderFile(package,xuid,kDashboardID,XContentType::kProfile,descriptor)==X_STATUS_SUCCESS);
  const auto device=runtime.content().sources().Source("hdd"); REQUIRE(device);
  REQUIRE(uint32_t(descriptor.device_id)==device->source.device_id);
  REQUIRE(uint64_t(descriptor.xuid)==xuid); REQUIRE(uint32_t(descriptor.title_id)==kDashboardID);
  REQUIRE(descriptor.content_type==XContentType::kProfile);
  REQUIRE(descriptor.display_name()==xe::to_utf16(name));
  REQUIRE(descriptor.file_name()==package.filename().string());
  return files;
}
void CheckSignedInProjection(CreationRuntime& runtime,uint64_t xuid,
                             const CreationFiles& files) {
  auto profile=runtime.profiles().CaptureProfile(xuid); REQUIRE(profile);
  REQUIRE(profile->xuid()==xuid); REQUIRE(profile->GetOnlineXUID()==0);
  REQUIRE(profile->GetProfileIcon(XTileType::kGamerTile)==files.at("tile_64.png"));
  REQUIRE(profile->GetProfileIcon(XTileType::kAvatarGamerTile)==files.at("avtr_64.png"));
  const auto slot=runtime.profiles().GetUserIndexAssignedToProfile(xuid);
  REQUIRE(slot<4);
  CreationPage page(*runtime.emulator->memory()); auto& memory=page.memory;
  auto context=CreationContext(runtime,page.address+0x2800);
  context.r[3]=slot; context.r[4]=1; context.r[5]=page.address+0x40;
  REQUIRE(CallCreationExport(runtime,0x20A,context)==X_E_SUCCESS);
  REQUIRE(xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x40))==xuid);
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x3F)==0xA5);
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x48)==0xA5);
  context=CreationContext(runtime,page.address+0x2800);
  context.r[3]=slot; context.r[4]=page.address+0x100; context.r[5]=0;
  REQUIRE(CallCreationExport(runtime,0x5DE,context)==X_ERROR_SUCCESS);
  GpdInfoProfile gpd(files.at("FFFE07D1.gpd")); const auto expected=Appearance(gpd);
  REQUIRE(std::equal(expected.begin(),expected.end(),memory.TranslateVirtual<uint8_t*>(page.address+0x100)));
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0xFF)==0xA5);
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x100+1000)==0xA5);
  // Exercise actual settings publication and the registered tile-key consumer,
  // rather than comparing two host serializers that share the same defect.
  constexpr uint32_t key_id=uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY);
  const uint32_t ids=page.address+0x600,size=page.address+0x610,output=page.address+0x800;
  const uint32_t required=8+sizeof(X_USER_PROFILE_SETTING)+UserSetting::get_max_size(key_id);
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(ids),key_id);
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(size),required);
  context=CreationContext(runtime,page.address+0x2800);
  context.r[3]=kDashboardID;context.r[4]=slot;context.r[5]=context.r[6]=0;
  context.r[7]=1;context.r[8]=ids;context.r[9]=0xffffffff;context.r[10]=size;
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(util::get_arg_stack_ptr<uint32_t>(&context,0)),output);
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(util::get_arg_stack_ptr<uint32_t>(&context,1)),0);
  REQUIRE(CallCreationExport(runtime,0x22A,context)==X_ERROR_SUCCESS);
  X_USER_PROFILE_SETTING row{};
  std::memcpy(&row,memory.TranslateVirtual(output+8),sizeof(row));
  REQUIRE(row.user_index==slot);
  REQUIRE(uint64_t(row.xuid)==((uint64_t(slot)<<32)|UINT64_C(0xFFFFFFFF)));
  REQUIRE(row.setting_id==key_id);
  REQUIRE(row.data.type==X_USER_DATA_TYPE::WSTRING);REQUIRE(row.data.data.unicode.size==50);
  std::optional<UserSetting> stored_key;
  REQUIRE(UserSetting::ReadFromGpd(gpd,key_id,stored_key)==X_ERROR_SUCCESS);REQUIRE(stored_key);
  const auto expected_key=stored_key->get_extended_data();REQUIRE(expected_key.size()==50);
  REQUIRE(std::equal(expected_key.begin(),expected_key.end(),memory.TranslateVirtual<uint8_t*>(row.data.data.unicode.ptr)));
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(output-1)==0xA5);
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(output+required)==0xA5);
  context=CreationContext(runtime,page.address+0x2800);
  context.r[3]=output+8+offsetof(X_USER_PROFILE_SETTING,data);
  context.r[4]=page.address+0x620;context.r[5]=page.address+0x624;context.r[6]=page.address+0x628;
  REQUIRE(CallCreationExport(runtime,0x30A,context)==X_ERROR_SUCCESS);
  std::array<uint32_t,3> parsed{};
  for(size_t field=0;field!=3;++field) {
    std::string text;
    for(size_t i=0;i!=8;++i){REQUIRE(expected_key[(field*8+i)*2]==0);text.push_back(char(expected_key[(field*8+i)*2+1]));}
    parsed[field]=uint32_t(std::strtoul(text.c_str(),nullptr,16));
    REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x620+uint32_t(field*4)))==parsed[field]);
  }
  REQUIRE(parsed[0]==kDashboardID);
}
XCONTENT_AGGREGATE_DATA CreationSaveDescriptor(CreationRuntime& runtime) {
  const auto source=runtime.content().sources().Source("hdd"); REQUIRE(source);
  XCONTENT_AGGREGATE_DATA save{}; save.device_id=source->source.device_id;
  save.title_id=0x12345678; save.content_type=XContentType::kSavedGame;
  save.set_file_name("same-save"); save.set_display_name(u"Creation save"); return save;
}
void WriteCreationSave(CreationRuntime& runtime,uint64_t xuid,const char* root,
                       std::span<const uint8_t> bytes) {
  const auto save=CreationSaveDescriptor(runtime);
  REQUIRE(runtime.content().CreateContent(root,xuid,save)==X_ERROR_SUCCESS);
  vfs::File* file=nullptr; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,std::string(root)+":\\marker.bin",
      vfs::FileDisposition::kCreate,vfs::FileAccess::kGenericWrite,false,true,&file,&action)==X_STATUS_SUCCESS);
  REQUIRE(file);
  struct Close { vfs::File* file; ~Close(){if(file)file->Destroy();} } close{file};
  size_t written=0; REQUIRE(file->WriteSync(bytes,0,&written)==X_STATUS_SUCCESS);
  REQUIRE(written==bytes.size()); REQUIRE(file->Flush()==X_STATUS_SUCCESS);
  file->Destroy(); close.file=nullptr;
  REQUIRE(runtime.content().CloseContent(root)==X_ERROR_SUCCESS);
}
void ReadCreationSave(CreationRuntime& runtime,uint64_t xuid,const char* root,
                      std::span<const uint8_t> expected) {
  const auto save=CreationSaveDescriptor(runtime); uint32_t license=0;
  REQUIRE(runtime.content().OpenContent(root,xuid,save,license)==X_ERROR_SUCCESS);
  vfs::File* file=nullptr; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,std::string(root)+":\\marker.bin",
      vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,false,true,&file,&action)==X_STATUS_SUCCESS);
  REQUIRE(file); struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} close{file};
  std::vector<uint8_t> bytes(expected.size()); size_t count=0;
  REQUIRE(file->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS); REQUIRE(count==expected.size());
  REQUIRE(std::equal(bytes.begin(),bytes.end(),expected.begin(),expected.end()));
  file->Destroy(); close.file=nullptr;
  REQUIRE(runtime.content().CloseContent(root)==X_ERROR_SUCCESS);
}
}

TEST_CASE("Installed initial local profiles create real Account GPD stock tiles and avatar then restart with isolated saves",
          "[.installed_profile_creation][kernel][profile_creation][persistence][avatar]") {
  CreationRuntime runtime(true); const auto& installed=runtime.emulator->installed_system_source();
  REQUIRE(installed); std::error_code error;
  const auto presets=StockAvatarPresets::Capture(installed->snapshot(),error);
  REQUIRE(presets); REQUIRE_FALSE(error); const auto* first_preset=presets->Get(0); REQUIRE(first_preset);
  REQUIRE(first_preset->body_type!=AvatarBodyType::kUnknown);
  uint32_t alternate=StockAvatarPresets::kCount; unsigned male=0,female=0;
  for (uint32_t i=0;i<StockAvatarPresets::kCount;++i) {
    const auto* preset=presets->Get(i); REQUIRE(preset);
    male+=preset->body_type==AvatarBodyType::kMale;
    female+=preset->body_type==AvatarBodyType::kFemale;
    if (preset->body_type!=first_preset->body_type && preset->body_type!=AvatarBodyType::kUnknown) { alternate=i; }
  }
  REQUIRE(male==6); REQUIRE(female==6); REQUIRE(alternate<StockAvatarPresets::kCount);
  const auto* second_preset=presets->Get(alternate); REQUIRE(second_preset);
  const auto one=runtime.profiles().CreateProfileDetailed("CheckpointOne",true);
  REQUIRE(one.status==X_ERROR_SUCCESS); REQUIRE(one.published); REQUIRE(one.discovered); REQUIRE(one.signed_in); REQUIRE(one.xuid);
  const auto first=CheckCreatedProfile(runtime,one.xuid,"CheckpointOne",*first_preset,0);
  CheckSignedInProjection(runtime,one.xuid,first);
  const auto two=runtime.profiles().CreateProfileDetailed("CheckpointTwo",true,false,alternate,1);
  REQUIRE(two.status==X_ERROR_SUCCESS); REQUIRE(two.published); REQUIRE(two.discovered); REQUIRE(two.signed_in);
  REQUIRE(two.xuid); REQUIRE(two.xuid!=one.xuid);
  const auto second=CheckCreatedProfile(runtime,two.xuid,"CheckpointTwo",*second_preset,1);
  CheckSignedInProjection(runtime,two.xuid,second);
  // The real registered guest producer must succeed as well as the host path.
  // Its actual guest page/Memory owner ends BEFORE Emulator reset below.
  uint64_t native_xuid=0xe0000000000000a4ull;
  while (native_xuid==one.xuid || native_xuid==two.xuid) { ++native_xuid; }
  CreationFiles native_initial,native_edited;
  const std::vector<uint8_t> opaque_info2{0x71,0,0x82,0x13,0xF4};
  {
    CreationPage page(*runtime.emulator->memory()); auto& memory=page.memory;
    const auto source=runtime.content().sources().Source("hdd"); REQUIRE(source);
    X_XAMACCOUNTINFO account{};
    string_util::copy_and_swap_truncating(account.gamertag,u"CheckpointTri",xe::countof(account.gamertag));
    account.passport_password[7]=char(0x8e);
    std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
    auto context=CreationContext(runtime,page.address+0x2800);
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50),source->source.device_id);
    context.r[3]=0x80000021u; context.r[4]=page.address+0x50;
    context.r[5]=native_xuid; context.r[6]=page.address+0x100;
    context.r[7]=context.r[8]=context.r[9]=0; context.r[10]=page.address+0x40;
    REQUIRE(CallCreationExport(runtime,0x230,context)==X_E_SUCCESS);
    const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));
    REQUIRE(record); REQUIRE(RegionSize(memory,record)>=0xAC0);
    const auto* completion=memory.TranslateVirtual<uint8_t*>(record);
    REQUIRE(xe::load_and_swap<uint32_t>(completion)==0x21);
    REQUIRE(xe::load_and_swap<uint32_t>(completion+4)==page.address+0x50);
    auto expected_account=account; expected_account.reserved_flags=0x40000001u;
    REQUIRE(std::memcmp(completion+8,&expected_account,sizeof(account))==0);
    REQUIRE(std::memcmp(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account))==0);
    REQUIRE(xe::load_and_swap<uint64_t>(completion+0xA78)==native_xuid);
    context=CreationContext(runtime,page.address+0x2800);
    context.r[3]=record; context.r[4]=page.address+0x80;
    REQUIRE(CallCreationExport(runtime,0x234,context)==X_E_SUCCESS);
    REQUIRE(xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x80))==native_xuid);
    REQUIRE(RegionSize(memory,record)==0);
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x7F)==0xA5);
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x88)==0xA5);
    native_initial=CheckCreatedProfile(runtime,native_xuid,"CheckpointTri",*first_preset,0);
    X_XAMACCOUNTINFO preserved{};
    REQUIRE(ProfileManager::DecryptAccountFile(native_initial.at("Account"),&preserved));
    REQUIRE(std::memcmp(&preserved,&expected_account,sizeof(account))==0);
    REQUIRE_FALSE(runtime.profiles().CaptureProfile(native_xuid));
    runtime.profiles().Login(native_xuid,3,false);
    REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(native_xuid)==3);
    CheckSignedInProjection(runtime,native_xuid,native_initial);
    // The typed tile request captures the exact earlier profile storage/sign-in
    // origin and owned PNG. An ordinary model commit does not retire identity.
    CreationPage tile_page(memory,uint32_t(native_initial.at("avtr_64.png").size()+0x2000));
    TileRequestArguments tile_args;
    tile_args.type=13; tile_args.title=kDashboardID; tile_args.item=native_xuid;
    tile_args.user=3; tile_args.destination=tile_page.address+0x1000;
    tile_args.size_address=tile_page.address+0x40;
    const uint32_t tile_capacity=uint32_t(native_initial.at("avtr_64.png").size()+31);
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(tile_args.size_address),tile_capacity);
    X_HRESULT tile_status=X_E_FAIL;
    auto earlier_tile=CaptureTileRequest(*runtime.emulator->kernel_state(),tile_args,tile_status);
    REQUIRE(earlier_tile); REQUIRE(tile_status==X_E_SUCCESS);
    // Preserve a genuine durable opaque INFO2 row. This small BINARY value is
    // deliberately NOT presented as a manifest/default or native readiness.
    {
      auto writer=runtime.profiles().CaptureGpdWriter(native_xuid,error);
      REQUIRE(writer.model); REQUIRE_FALSE(error);
      ProfileGpdMutation command; command.kind=ProfileGpdMutationKind::kSetSetting;
      command.title_id=kDashboardID; command.request_id=writer.model->AllocateRequestId(error);
      REQUIRE_FALSE(error);
      command.setting.emplace(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_2,opaque_info2);
      const auto stored=writer.Submit(std::move(command));
      REQUIRE_FALSE(stored.error);
      REQUIRE(stored.state==ProfileGpdTransactionAttempt::State::kCommitted);
    }
    uint32_t tile_error=X_E_FAIL,tile_length=99;
    REQUIRE(CompleteTileRequest(*runtime.emulator->kernel_state(),*earlier_tile,tile_error,tile_length)==X_ERROR_SUCCESS);
    REQUIRE(tile_error==X_E_SUCCESS); REQUIRE(tile_length==0);
    const auto& captured_preview=native_initial.at("avtr_64.png");
    REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(tile_args.size_address))==captured_preview.size());
    REQUIRE(std::equal(captured_preview.begin(),captured_preview.end(),memory.TranslateVirtual<uint8_t*>(tile_args.destination)));
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(tile_args.destination-1)==0xA5);
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(tile_args.destination+captured_preview.size())==0xA5);
    earlier_tile.reset();
    const auto native_package=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(native_xuid);
    const auto before_edit=CreationRead(native_package/"FFFE07D1.gpd");
    const GpdInfoProfile prior_model(before_edit);
    const auto* prior_info2=prior_model.GetEntry(uint16_t(GpdSection::kSetting),
        uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_2));
    REQUIRE(prior_info2);
    auto incoming=second_preset->appearance;
    std::fill(incoming.begin()+0x380,incoming.begin()+0x38D,0x6B);
    std::memcpy(memory.TranslateVirtual(page.address+0x400),incoming.data(),incoming.size());
    context=CreationContext(runtime,page.address+0x2800);
    context.r[3]=3; context.r[4]=page.address+0x400; context.r[5]=0;
    REQUIRE(CallCreationExport(runtime,0x5E4,context)==X_ERROR_SUCCESS);
    REQUIRE(std::equal(incoming.begin(),incoming.end(),memory.TranslateVirtual<uint8_t*>(page.address+0x400)));
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x3FF)==0xA5);
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x400+1000)==0xA5);
    for (const auto& [leaf,bytes]:native_initial) {
      native_edited.emplace(leaf,CreationRead(native_package/leaf));
      if (leaf!="FFFE07D1.gpd") { REQUIRE(native_edited.at(leaf)==bytes); }
    }
    REQUIRE(native_edited.at("FFFE07D1.gpd")!=before_edit);
    REQUIRE_FALSE(ValidateGpdSnapshot(native_edited.at("FFFE07D1.gpd")));
    REQUIRE_FALSE(ValidateProfileGpdRecords(native_edited.at("FFFE07D1.gpd"),true));
    const GpdInfoProfile changed(native_edited.at("FFFE07D1.gpd"));
    const auto* after_info2=changed.GetEntry(uint16_t(GpdSection::kSetting),
        uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_2));
    REQUIRE(after_info2); REQUIRE(after_info2->data==prior_info2->data);
    const auto edited_manifest=Appearance(changed);
    REQUIRE(AvatarManifestBodyType(edited_manifest)==second_preset->body_type);
    REQUIRE(xe::load_and_swap<uint64_t>(edited_manifest.data()+0x380)==native_xuid);
    for (size_t i=0;i<edited_manifest.size();++i) {
      if (i>=0x380 && i<0x388) { continue; }
      if (i>=0x388 && i<0x38D) { REQUIRE(edited_manifest[i]==0); }
      else { REQUIRE(edited_manifest[i]==incoming[i]); }
    }
    std::optional<UserSetting> kept;
    REQUIRE(UserSetting::ReadFromGpd(changed,uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_2),kept)==X_ERROR_SUCCESS);
    REQUIRE(kept); REQUIRE(kept->get_type()==X_USER_DATA_TYPE::BINARY);
    const auto kept_bytes=kept->get_extended_data();
    REQUIRE(std::equal(kept_bytes.begin(),kept_bytes.end(),opaque_info2.begin(),opaque_info2.end()));
    CheckSignedInProjection(runtime,native_xuid,native_edited);
    std::memset(memory.TranslateVirtual(tile_args.destination),0xA5,tile_capacity);
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(tile_args.size_address),tile_capacity);
    auto retired_tile=CaptureTileRequest(*runtime.emulator->kernel_state(),tile_args,tile_status);
    REQUIRE(retired_tile); REQUIRE(tile_status==X_E_SUCCESS);
    runtime.profiles().Logout(3,false);
    REQUIRE(CompleteTileRequest(*runtime.emulator->kernel_state(),*retired_tile,tile_error,tile_length)==X_ERROR_FUNCTION_FAILED);
    REQUIRE(tile_error==X_E_FAIL); REQUIRE(tile_length==0);
    REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(tile_args.size_address))==tile_capacity);
    const auto* untouched=memory.TranslateVirtual<const uint8_t*>(tile_args.destination);
    REQUIRE(std::all_of(untouched,untouched+tile_capacity,[](uint8_t byte){return byte==0xA5;}));
    retired_tile.reset(); // release retired storage BEFORE the next actual login
    runtime.profiles().Login(native_xuid,3,false);
    REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(native_xuid)==3);
    CheckSignedInProjection(runtime,native_xuid,native_edited);
    // INFO1 changed durably; the shipped preview remains the existing image.
    // No live renderer or newly generated32/64 avatar tile is implied.
    REQUIRE(native_edited.at("avtr_64.png")==native_initial.at("avtr_64.png"));
  }
  const std::array<uint8_t,3> save_one{1,0,2},save_two{8,0,9};
  WriteCreationSave(runtime,one.xuid,"CreationA",save_one);
  WriteCreationSave(runtime,two.xuid,"CreationB",save_two);
  runtime.profiles().Logout(runtime.profiles().GetUserIndexAssignedToProfile(native_xuid),false);
  REQUIRE_FALSE(runtime.profiles().CaptureProfile(native_xuid));
  runtime.profiles().Logout(runtime.profiles().GetUserIndexAssignedToProfile(two.xuid),false);
  runtime.profiles().Logout(runtime.profiles().GetUserIndexAssignedToProfile(one.xuid),false);
  REQUIRE_FALSE(runtime.profiles().CaptureProfile(one.xuid)); REQUIRE_FALSE(runtime.profiles().CaptureProfile(two.xuid));
  runtime.Restart();
  REQUIRE(runtime.profiles().GetAccountCount()==3);
  REQUIRE(CheckCreatedProfile(runtime,one.xuid,"CheckpointOne",*first_preset,0)==first);
  REQUIRE(CheckCreatedProfile(runtime,two.xuid,"CheckpointTwo",*second_preset,1)==second);
  runtime.profiles().Login(one.xuid,2,false); runtime.profiles().Login(two.xuid,0,false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(one.xuid)==2);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(two.xuid)==0);
  CheckSignedInProjection(runtime,one.xuid,first); CheckSignedInProjection(runtime,two.xuid,second);
  const auto native_package=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(native_xuid);
  for (const auto& [leaf,bytes]:native_edited) { REQUIRE(CreationRead(native_package/leaf)==bytes); }
  GpdInfoProfile restored(native_edited.at("FFFE07D1.gpd"));
  REQUIRE(AvatarManifestBodyType(Appearance(restored))==second_preset->body_type);
  std::optional<UserSetting> restored_info2;
  REQUIRE(UserSetting::ReadFromGpd(restored,uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_2),restored_info2)==X_ERROR_SUCCESS);
  REQUIRE(restored_info2);
  const auto restored_bytes=restored_info2->get_extended_data();
  REQUIRE(std::equal(restored_bytes.begin(),restored_bytes.end(),opaque_info2.begin(),opaque_info2.end()));
  runtime.profiles().Login(native_xuid,3,false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(native_xuid)==3);
  CheckSignedInProjection(runtime,native_xuid,native_edited);
  ReadCreationSave(runtime,one.xuid,"CreationA",save_one); ReadCreationSave(runtime,two.xuid,"CreationB",save_two);
  // Reproduce the actual older host constructor's LE key on copied fixture
  // state only, with unknown record header/tail bytes to protect in repair.
  runtime.profiles().Logout(3,false);runtime.profiles().Logout(0,false);runtime.profiles().Logout(2,false);
  runtime.emulator.reset();REQUIRE(KernelState::shared()==nullptr);
  class LegacyKeyDatabase : public GpdInfoProfile {
   public:
    explicit LegacyKeyDatabase(const std::vector<uint8_t>& bytes):GpdInfoProfile(bytes){}
    void BreakHostKey() {
      const auto* original=GetEntry(uint16_t(GpdSection::kSetting),uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY));REQUIRE(original);
      Entry copy=*original;REQUIRE(copy.data.size()>=sizeof(X_XDBF_GPD_SETTING_HEADER)+50);
      auto* header=reinterpret_cast<X_XDBF_GPD_SETTING_HEADER*>(copy.data.data());
      header->unknown_1=0x1234ABCD;std::fill_n(header->unknown,7,char(0x5A));
      header->base_data.unicode.ptr=0x89ABCDEF;
      auto* payload=copy.data.data()+sizeof(*header);
      for(size_t i=0;i!=50;i+=2){std::swap(payload[i],payload[i+1]);}
      copy.data.insert(copy.data.end(),{0xC1,0xA0,0xF9,0});copy.info.size=uint32_t(copy.data.size());
      UpsertEntry(&copy);
    }
  } legacy(first.at("FFFE07D1.gpd"));
  legacy.BreakHostKey();std::vector<uint8_t> legacy_wire;
  REQUIRE_FALSE(legacy.SerializeChecked(legacy_wire,GpdSnapshotStore::kMaximumBytes));
  const auto one_package=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(one.xuid);
  {
    std::ofstream output(one_package/"FFFE07D1.gpd",std::ios::binary|std::ios::trunc);REQUIRE(output);
    output.write(reinterpret_cast<const char*>(legacy_wire.data()),legacy_wire.size());REQUIRE(output.good());
  }
  runtime.Restart();runtime.profiles().Login(one.xuid,2,false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(one.xuid)==2);
  auto repaired=first;repaired["FFFE07D1.gpd"]=CreationRead(one_package/"FFFE07D1.gpd");
  REQUIRE(repaired.at("FFFE07D1.gpd")!=legacy_wire);
  const GpdInfoProfile repaired_db(repaired.at("FFFE07D1.gpd"));
  const auto* repaired_key=repaired_db.GetEntry(uint16_t(GpdSection::kSetting),uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY));REQUIRE(repaired_key);
  const GpdInfoProfile& legacy_read = legacy;
  const auto* legacy_key=legacy_read.GetEntry(uint16_t(GpdSection::kSetting),uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY));REQUIRE(legacy_key);
  auto intended=legacy_key->data;
  for(size_t i=sizeof(X_XDBF_GPD_SETTING_HEADER);i!=sizeof(X_XDBF_GPD_SETTING_HEADER)+50;i+=2){std::swap(intended[i],intended[i+1]);}
  REQUIRE(repaired_key->data==intended); // exact unknown header, pointer and suffix retained
  const auto before_view=XdbfTableView::Parse(legacy_wire);
  const auto after_view=XdbfTableView::Parse(repaired.at("FFFE07D1.gpd"));
  REQUIRE(before_view);REQUIRE(after_view);REQUIRE(before_view->entries_used==after_view->entries_used);
  for(size_t i=0;i!=before_view->entries_used;++i){
    const auto* record=before_view->entries.data()+i*18;
    const uint16_t section=uint16_t(record[0])<<8|record[1];
    const uint64_t id=uint64_t(XdbfTableView::U32(record+2))<<32|XdbfTableView::U32(record+6);
    if(section==uint16_t(GpdSection::kSetting)&&id==uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY)){continue;}
    const auto old=before_view->Find(section,id),next=after_view->Find(section,id);REQUIRE(old);REQUIRE(next);
    REQUIRE(std::equal(old->begin(),old->end(),next->begin(),next->end()));
  }
  REQUIRE(Appearance(repaired_db)==Appearance(legacy));
  for(const auto& [leaf,bytes]:first){if(leaf!="FFFE07D1.gpd"){REQUIRE(CreationRead(one_package/leaf)==bytes);}}
  CheckSignedInProjection(runtime,one.xuid,repaired);
  {
    auto writer=runtime.profiles().CaptureGpdWriter(one.xuid,error);REQUIRE(writer.model);REQUIRE_FALSE(error);
    auto rows=GpdDurableIntentStore::Scan(*writer.owner,error);REQUIRE_FALSE(error);
    size_t recovered_before=0;
    for(const auto& row:rows){
      if(row.binding.xuid!=one.xuid){continue;}
      REQUIRE_FALSE(row.error);REQUIRE(row.done_record_valid);
      std::vector<uint8_t> journal;
      REQUIRE_FALSE(writer.owner->root()->ReadFile(row.journal,4*GpdSnapshotStore::kMaximumBytes+16384,journal));
      REQUIRE(std::search(journal.begin(),journal.end(),legacy_wire.begin(),legacy_wire.end())!=journal.end());
      ++recovered_before;
    }
    REQUIRE(recovered_before==1); // real durable journal retains full original bytes
    const auto before_repair=writer.model->CaptureLoads(error);REQUIRE(before_repair);
    const auto original_id=before_repair->at(kDashboardID).token->info().file_id;
    REQUIRE_FALSE(runtime.profiles().ReloadGpdModel(one.xuid));
    REQUIRE(CreationRead(one_package/"FFFE07D1.gpd")==repaired.at("FFFE07D1.gpd"));
    const auto after_repair=writer.model->CaptureLoads(error);REQUIRE(after_repair);
    REQUIRE(after_repair->at(kDashboardID).token->info().file_id==original_id);
    REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==rows.size());REQUIRE_FALSE(error);
  }
  runtime.profiles().Logout(2,false);runtime.Restart();runtime.profiles().Login(one.xuid,2,false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(one.xuid)==2);
  REQUIRE(CreationRead(one_package/"FFFE07D1.gpd")==repaired.at("FFFE07D1.gpd"));
  CheckSignedInProjection(runtime,one.xuid,repaired);
  ReadCreationSave(runtime,one.xuid,"CreationA",save_one);ReadCreationSave(runtime,two.xuid,"CreationB",save_two);
}

TEST_CASE("Both host editors' common picture service commits pair and selection then resets and restarts without changing identity or saves",
          "[.installed_profile_creation][kernel][profile_picture][persistence]") {
  CreationRuntime runtime(true);
  const auto created = runtime.profiles().CreateProfileDetailed("HostPictures", true);
  REQUIRE(created.status == X_ERROR_SUCCESS); REQUIRE(created.signed_in);
  const auto xuid = created.xuid; REQUIRE(xuid);
  const auto package = runtime.root.path / "content" / ProfileStorageLease::ProfileRelative(xuid);
  const auto account = CreationRead(package / "Account");
  const auto public_large = CreationRead(package / "tile_64.png");
  const auto public_small = CreationRead(package / "tile_32.png");
  const auto avatar_large = CreationRead(package / "avtr_64.png");
  // The actual fresh recipe ships avtr_64 only. Preserve a genuine existing
  // small tile byte-for-byte when present, and preserve its absence otherwise.
  std::optional<std::vector<uint8_t>> avatar_small;
  if (std::filesystem::exists(package / "avtr_32.png")) {
    avatar_small = CreationRead(package / "avtr_32.png");
  }
  const auto check_avatar_small = [&] {
    REQUIRE(std::filesystem::exists(package / "avtr_32.png") == avatar_small.has_value());
    if (avatar_small) { REQUIRE(CreationRead(package / "avtr_32.png") == *avatar_small); }
  };
  const GpdInfoProfile original_gpd(CreationRead(package / "FFFE07D1.gpd"));
  const auto appearance = Appearance(original_gpd);
  std::optional<UserSetting> public_key;
  REQUIRE(UserSetting::ReadFromGpd(original_gpd,
      uint32_t(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY), public_key) == X_ERROR_SUCCESS);
  REQUIRE(public_key);
  const std::array<uint8_t,4> save{0x61,0,0x72,0x83};
  WriteCreationSave(runtime, xuid, "HostPictureSave", save);
  auto* tracker = runtime.emulator->kernel_state()->xam_state()->user_tracker();
  REQUIRE(tracker);
  const auto self_picture = [&]() {
    auto profile = runtime.profiles().CaptureProfile(xuid);
    REQUIRE(profile); return profile->GetSelfProfileIcon();
  };
  REQUIRE(self_picture() == public_large);
  const auto before_rejected = CreationRead(package / "FFFE07D1.gpd");
  REQUIRE_FALSE(tracker->UpdateUserIcon(xuid, std::array<uint8_t,3>{0x89,0,1}));
  REQUIRE(CreationRead(package / "FFFE07D1.gpd") == before_rejected);
  REQUIRE_FALSE(std::filesystem::exists(package / "pp_64.png"));
  REQUIRE_FALSE(std::filesystem::exists(package / "pp_32.png"));
  const auto png = xe::kernel::test::SyntheticProfilePicture64();
  std::error_code error;
  const auto pair = PrepareHostProfilePicturePair(png, error);
  REQUIRE(pair); REQUIRE_FALSE(error);
  REQUIRE(tracker->UpdateUserIcon(xuid, png));
  REQUIRE(CreationRead(package / "pp_64.png") == pair->large);
  REQUIRE(CreationRead(package / "pp_32.png") == pair->small);
  REQUIRE(runtime.profiles().CaptureProfileIcon(xuid, XTileType::kPersonalGamerTile) == pair->large);
  REQUIRE(self_picture() == pair->large);
  REQUIRE(runtime.profiles().CaptureProfileIcon(xuid, XTileType::kPersonalGamerTileSmall) == pair->small);
  const GpdInfoProfile changed_gpd(CreationRead(package / "FFFE07D1.gpd"));
  std::optional<UserSetting> selected;
  REQUIRE(UserSetting::ReadFromGpd(changed_gpd,
      uint32_t(UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE), selected) == X_ERROR_SUCCESS);
  REQUIRE(selected); REQUIRE(selected->get_type() == X_USER_DATA_TYPE::WSTRING);
  const auto text = std::get<std::u16string>(selected->get_host_data());
  REQUIRE(text.size() == 25); REQUIRE(text.back() == 0);
  REQUIRE(text.substr(0,8) == u"fffe07d1");
  REQUIRE(Appearance(changed_gpd) == appearance);
  REQUIRE(CreationRead(package / "Account") == account);
  REQUIRE(CreationRead(package / "tile_64.png") == public_large);
  REQUIRE(CreationRead(package / "tile_32.png") == public_small);
  REQUIRE(CreationRead(package / "avtr_64.png") == avatar_large);
  check_avatar_small();
  runtime.profiles().Logout(runtime.profiles().GetUserIndexAssignedToProfile(xuid), false);
  runtime.Restart();
  runtime.profiles().Login(xuid, 2, false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(xuid) == 2);
  REQUIRE(CreationRead(package / "avtr_64.png") == avatar_large);
  check_avatar_small();
  REQUIRE(runtime.profiles().CaptureProfileIcon(xuid, XTileType::kPersonalGamerTile) == pair->large);
  REQUIRE(self_picture() == pair->large);
  const GpdInfoProfile persisted(CreationRead(package / "FFFE07D1.gpd"));
  std::optional<UserSetting> persisted_key;
  REQUIRE(UserSetting::ReadFromGpd(persisted,
      uint32_t(UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE), persisted_key) == X_ERROR_SUCCESS);
  REQUIRE(persisted_key); REQUIRE(persisted_key->Serialize() == selected->Serialize());
  tracker = runtime.emulator->kernel_state()->xam_state()->user_tracker();
  REQUIRE(tracker->ClearUserIcon(xuid));
  REQUIRE_FALSE(std::filesystem::exists(package / "pp_64.png"));
  REQUIRE_FALSE(std::filesystem::exists(package / "pp_32.png"));
  REQUIRE(runtime.profiles().CaptureProfileIcon(xuid, XTileType::kPersonalGamerTile) == public_large);
  REQUIRE(self_picture() == public_large);
  REQUIRE(runtime.profiles().CaptureProfileIcon(xuid, XTileType::kPersonalGamerTileSmall) == public_small);
  const GpdInfoProfile reset_gpd(CreationRead(package / "FFFE07D1.gpd"));
  std::optional<UserSetting> reset_key;
  REQUIRE(UserSetting::ReadFromGpd(reset_gpd,
      uint32_t(UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE), reset_key) == X_ERROR_SUCCESS);
  REQUIRE(reset_key); REQUIRE(reset_key->get_host_data() == public_key->get_host_data());
  REQUIRE(Appearance(reset_gpd) == appearance);
  REQUIRE(CreationRead(package / "avtr_64.png") == avatar_large);
  check_avatar_small();
  REQUIRE(CreationRead(package / "Account") == account);
  runtime.profiles().Logout(2, false); runtime.Restart();
  runtime.profiles().Login(xuid, 0, false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(xuid) == 0);
  REQUIRE(CreationRead(package / "avtr_64.png") == avatar_large);
  check_avatar_small();
  REQUIRE(runtime.profiles().CaptureProfileIcon(xuid, XTileType::kPersonalGamerTile) == public_large);
  REQUIRE(self_picture() == public_large);
  REQUIRE_FALSE(std::filesystem::exists(package / "pp_64.png"));
  REQUIRE_FALSE(std::filesystem::exists(package / "pp_32.png"));
  ReadCreationSave(runtime, xuid, "HostPictureSave", save);
}

// This fixture exercises the common row/writer component, not native getter
// authentication or PPC routing. Production constructors remain owner-only.
class NativeProfileSaveWriterTestAccess {
 public:
  static auto Capture(ProfileManager& profiles, const std::shared_ptr<const HostRoster>& selected,
      const std::shared_ptr<ProfileRosterIntent>& job, uint8_t slot, uint64_t getter_xuid, std::error_code& error) {
    return ProfileRosterNativeWriter::CaptureFromActualSelectedRow(profiles, selected, job, slot, getter_xuid, error);
  }
  static auto CaptureGetter(ProfileManager& profiles, const std::shared_ptr<const HostRoster>& current,
      const std::shared_ptr<ProfileRosterIntent>& job, uint8_t slot, uint64_t getter_xuid, std::error_code& error) {
    return ProfileRosterNativeWriter::CaptureFromActualGetter(profiles,current,job,slot,getter_xuid,error);
  }
  static bool Current(const std::shared_ptr<ProfileRosterNativeWriter>& token, std::error_code& error) {
    return token->Current(error);
  }
  static auto Writer(const std::shared_ptr<ProfileRosterNativeWriter>& token) { return token->writer(); }
  static auto Origin(const std::shared_ptr<ProfileRosterNativeWriter>& token) { return token->origin(); }
  static void Retry(ProfileManager& profiles) { profiles.RetryRosterSettlement(); }
};
// Storage participant tests deliberately supply the byte/cut fixture through
// this private test seam; they do NOT authenticate native PPC or clean a DB.
class NativeProfileSavePublicationTestAccess {
 public:
  static auto Prepare(std::shared_ptr<ProfileRosterNativeWriter> source,
      ProfileGpdMutation command, std::error_code& error) {
    return NativeProfileSavePublication::Prepare(std::move(source),std::move(command),error);
  }
  // Component fixture only: synthetic issuer scalars never authenticate PPC.
  static auto PrepareSystem(std::shared_ptr<ProfileRosterNativeWriter> source,
      std::shared_ptr<NativeProfileGpdReadView> before,uint32_t title,
      std::error_code& error,std::shared_ptr<const save_detail::SemanticUses::Span> semantic,
      std::vector<filesystem::ProfileArtifactKind> required={},uint64_t domain=1,bool deferred=false) {
    NativeProfileSavePublication::SystemFileIssuer issuer;
    issuer.requested_device=NativeProfileSaveWriterTestAccess::Writer(source).model->storage()->device();
    issuer.source=std::move(source);issuer.actual_before_file=std::move(before);
    issuer.domain=domain;issuer.root_generation=2;issuer.invocation=3;issuer.title=title;
    issuer.required_artifacts=std::move(required);issuer.actual_semantic_use=std::move(semantic);
    issuer.defer_until_whole_checkpoint=deferred;
    return NativeProfileSavePublication::PrepareSystemFile(std::move(issuer),error);
  }
  static auto PrepareHeader(std::shared_ptr<ProfileRosterNativeWriter> source,
      object_ref<XFile> raw,std::vector<uint8_t> before,
      std::shared_ptr<const save_detail::SemanticUses::Span> semantic,std::error_code& error) {
    NativeProfileSavePublication::SystemHeaderIssuer issuer;
    issuer.operation.source=std::move(source);issuer.operation.title=kDashboardID;
    issuer.operation.domain=1;issuer.operation.root_generation=2;issuer.operation.invocation=3;
    issuer.operation.actual_semantic_use=std::move(semantic);
    issuer.operation.required_artifacts={filesystem::ProfileArtifactKind::kPackageHeader};
    issuer.actual_backing_file=std::move(raw);
    issuer.actual_package=issuer.actual_backing_file->file()->folder_content_package();
    issuer.actual_native_before_header=std::move(before);
    return NativeProfileSavePublication::PrepareSystemHeaderOperation(std::move(issuer),error);
  }
  static auto PrepareParent(std::shared_ptr<ProfileRosterNativeWriter> source,
      std::shared_ptr<const save_detail::SemanticUses::Span> semantic,std::error_code& error,bool no_gpd_write=false) {
    NativeProfileSavePublication::SystemFileIssuer issuer;
    issuer.source=std::move(source);issuer.title=kDashboardID;
    issuer.domain=1;issuer.root_generation=2;issuer.invocation=3;
    issuer.actual_semantic_use=std::move(semantic);
    issuer.required_artifacts={filesystem::ProfileArtifactKind::kAvatar64,filesystem::ProfileArtifactKind::kPackageHeader};
    issuer.gpd_mode=no_gpd_write ? NativeProfileSavePublication::SystemGpdMode::kNativeUnchangedNoWrite
                               : NativeProfileSavePublication::SystemGpdMode::kNativeWriteRequired;
    return NativeProfileSavePublication::PrepareSystemParentOperation(std::move(issuer),error);
  }
  static bool ParentHasAdmission(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    std::lock_guard lock(owner->mutex_);return bool(owner->joined_artifacts_);
  }
  static size_t PictureStageCount(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    std::lock_guard lock(owner->mutex_);return owner->picture_stages_.size();
  }
  static vfs::FileOpenContext ParentPictureContext(const std::shared_ptr<NativeProfileSaveManagedPicture>& service,
      const std::shared_ptr<NativeProfileSavePublication>& owner,std::shared_ptr<vfs::Device> device,
      std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueParentForActualOpen(owner,std::move(device),"avtr_64.png",
      std::make_shared<int>(1),error);return context;
  }
  static vfs::FileOpenContext ParentPictureSourceContext(const std::shared_ptr<NativeProfileSaveManagedPicture>& service,
      const std::shared_ptr<NativeProfileSavePublication>& owner,std::shared_ptr<vfs::Device> device,std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueParentStageReadForActualOpen(owner,std::move(device),"avtr_64.png",std::make_shared<int>(1),error);
    return context;
  }
  static vfs::FileOpenContext ParentHeaderContext(const std::shared_ptr<NativeProfileSavePublication>& owner,
      std::shared_ptr<vfs::Device> device,std::string path,std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=owner->IssueParentHeaderReadForActualOpen(std::move(device),std::move(path),
      std::make_shared<int>(1),error);return context;
  }
  static vfs::FileOpenContext ParentGpdContext(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      const std::shared_ptr<NativeProfileSavePublication>& owner,std::shared_ptr<vfs::Device> device,
      std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueParentReadForActualOpen(owner,std::move(device),"FFFE07D1.gpd",
      std::make_shared<int>(1),{},error);return context;
  }
  static auto BindParentHeader(const std::shared_ptr<NativeProfileSavePublication>& owner,
      object_ref<XFile> raw,std::vector<uint8_t> before) {
    NativeProfileSavePublication::SystemHeaderIssuer issuer;issuer.operation=*owner->system_issuer_;
    issuer.actual_backing_file=std::move(raw);issuer.actual_package=issuer.actual_backing_file->file()->folder_content_package();
    issuer.actual_native_before_header=std::move(before);
    return owner->BindSystemHeaderInputAtOriginalEdit(std::move(issuer));
  }
  static auto HoldCacheCheckpoint(const std::shared_ptr<NativeProfileSavePublication>& owner,
      uint32_t title,const std::shared_ptr<NativeProfileSaveGpdStage>& stage,std::error_code& error) {
    std::lock_guard serial(owner->mutex_);std::lock_guard held_stage(stage->mutex_);
    return owner->PrepareActualCacheCheckpoint(title,stage,error);
  }
  static size_t StageCount(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    std::lock_guard held(owner->mutex_);return owner->stages_.size();
  }
  static auto Stage(const std::shared_ptr<NativeProfileSavePublication>& owner,uint32_t title) {
    std::lock_guard held(owner->mutex_);return owner->stages_.at(title);
  }
  static X_STATUS BindSystem(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      vfs::File& file,const std::shared_ptr<NativeProfileGpdReadView>& expected,
      const std::shared_ptr<NativeProfileSavePublication>& publication,const std::vector<uint8_t>& raw) {
    REQUIRE(raw.size()>=24);const uint64_t prefix=24+uint64_t(XdbfTableView::U32(raw.data()+8))*18+
        uint64_t(XdbfTableView::U32(raw.data()+16))*8;REQUIRE(prefix<=raw.size());
    return service->BindSystemFileBeforeActualEdit(file,expected,publication,{raw.begin(),raw.begin()+size_t(prefix)});
  }
  static auto StagedCheckpoint(vfs::File& file,
      const std::shared_ptr<NativeProfileSavePublication>& owner,
      std::span<const uint8_t> native,std::error_code& error) {
    REQUIRE(native.size()>=24);const uint64_t prefix=24+uint64_t(XdbfTableView::U32(native.data()+8))*18+
        uint64_t(XdbfTableView::U32(native.data()+16))*8;REQUIRE(prefix<=native.size());
    return NativeProfileSaveManagedGpd::CaptureStagedFileCheckpointAtActualDbUse(file,owner,native.first(size_t(prefix)),error);
  }
  static const auto& CheckpointBytes(const std::shared_ptr<const NativeProfileGpdCheckpoint>& value) {
    return value->bytes_;
  }
  static auto PristineCheckpoint(vfs::File& file,
      const std::shared_ptr<NativeProfileSavePublication>& owner,
      std::span<const uint8_t> native,std::error_code& error) {
    REQUIRE(native.size()>=24);const uint64_t prefix=24+uint64_t(XdbfTableView::U32(native.data()+8))*18+
        uint64_t(XdbfTableView::U32(native.data()+16))*8;REQUIRE(prefix<=native.size());
    return NativeProfileSaveManagedGpd::CapturePristineBoundFileCheckpointAtActualDbUse(
        file,owner,native.first(size_t(prefix)),error);
  }
  static bool PristineMatches(vfs::File& file,
      const std::shared_ptr<NativeProfileSavePublication>& owner,
      const std::shared_ptr<const NativeProfileGpdCheckpoint>& checkpoint,
      std::span<const uint8_t> native,std::error_code& error) {
    REQUIRE(native.size()>=24);const uint64_t prefix=24+uint64_t(XdbfTableView::U32(native.data()+8))*18+
        uint64_t(XdbfTableView::U32(native.data()+16))*8;REQUIRE(prefix<=native.size());
    return NativeProfileSaveManagedGpd::MatchesPristineBoundBefore(
        file,owner,checkpoint,native.first(size_t(prefix)),error);
  }
  static auto PrepareCache(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,vfs::File& file,
      const std::shared_ptr<NativeProfileSavePublication>& owner,std::span<const uint8_t> native,std::error_code& error) {
    REQUIRE(native.size()>=24);const uint64_t prefix=24+uint64_t(XdbfTableView::U32(native.data()+8))*18+
        uint64_t(XdbfTableView::U32(native.data()+16))*8;REQUIRE(prefix<=native.size());
    std::unique_lock serial(owner->mutex_);
    return service->PrepareCommittedCacheWithSerialHeld(file,owner,native.first(size_t(prefix)),error);
  }
  static bool PreparedMatches(const auto& prepared,const std::shared_ptr<NativeProfileSavePublication>& owner,
      const std::shared_ptr<NativeProfileSaveGpdStage>& stage) {
    return prepared && prepared->MatchesPublication(owner,kDashboardID,stage);
  }
  struct FileCheckpoint {
    std::shared_ptr<const vfs::FileWriteCheckpointAuthority> authority;
    std::shared_ptr<NativeProfileSaveOwner::WriteOutcome> outcome;
  };
  static FileCheckpoint Checkpoint(XFile& file,
      const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      const std::shared_ptr<NativeProfileSavePublication>& owner,uint32_t offset,
      std::vector<uint8_t> input,std::vector<uint8_t> header,std::error_code& error) {
    FileCheckpoint result;
    result.authority=NativeProfileSaveOwner::PrepareWriteCheckpointForActualCut(
        file,service,owner,kDashboardID,offset,std::move(input),std::move(header),result.outcome,error);
    return result;
  }
  static FileCheckpoint PictureCheckpoint(XFile& file,
      const std::shared_ptr<NativeProfileSavePublication>& owner,std::vector<uint8_t> input,std::error_code& error) {
    FileCheckpoint result;result.authority=NativeProfileSaveOwner::PreparePictureWriteForActualCut(file,owner,
        filesystem::ProfileArtifactKind::kAvatar64,0,std::move(input),result.outcome,error);return result;
  }
  static auto PictureOutputCheckpoint(const vfs::File& file,
      const std::shared_ptr<NativeProfileSavePublication>& owner,
      filesystem::ProfileArtifactKind kind,std::error_code& error) {
    return NativeProfileSavePictureStage::CaptureOwnedFileCheckpoint(file,owner,kind,error);
  }
  static FileCheckpoint HeaderCheckpoint(XFile& file,
      const std::shared_ptr<NativeProfileSavePublication>& owner,uint32_t offset,
      std::vector<uint8_t> input,std::error_code& error) {
    FileCheckpoint result;
    result.authority=NativeProfileSaveOwner::PrepareHeaderWriteForActualCut(
        file,owner,offset,std::move(input),result.outcome,error);return result;
  }
  static bool CheckpointStaged(const FileCheckpoint& value) {
    return value.outcome->phase.load(std::memory_order_acquire)==NativeProfileSaveOwner::WriteOutcome::Phase::kStagedAwaitingWholeCheckpoint;
  }
  static auto SealWhole(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    return owner->SealJoinedSystemAtOriginalWholeCheckpoint();
  }
  static bool CheckpointReady(const FileCheckpoint& value) {
    return value.outcome->phase.load(std::memory_order_acquire)==NativeProfileSaveOwner::WriteOutcome::Phase::kReady;
  }
  static bool CheckpointRefused(const FileCheckpoint& value) {
    return value.outcome->phase.load(std::memory_order_acquire)==NativeProfileSaveOwner::WriteOutcome::Phase::kTransferRefused;
  }
  static bool CheckpointPublicationFailed(const FileCheckpoint& value) {
    return value.outcome->phase.load(std::memory_order_acquire)==NativeProfileSaveOwner::WriteOutcome::Phase::kPublicationFailed;
  }
  static void EndByteUse(const std::shared_ptr<NativeProfileSavePublication>& owner) {owner->CloseOriginalByteMutation();}
  static void EndDataUse(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    owner->CloseDataByteMutationAtOriginalAllocationCut();
  }
  static X_STATUS PristineNoop(XFile& file,const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      const std::shared_ptr<NativeProfileGpdReadView>& expected,
      const std::shared_ptr<NativeProfileSavePublication>& publication,std::span<const uint8_t> raw) {
    REQUIRE(raw.size()>=24);const uint64_t prefix=24+uint64_t(XdbfTableView::U32(raw.data()+8))*18+
        uint64_t(XdbfTableView::U32(raw.data()+16))*8;REQUIRE(prefix<=raw.size());
    auto cut=std::make_shared<NativeProfileSaveOwner::ManagedFileCut>();
    cut->kind=NativeProfileSaveOwner::ManagedFileCut::Kind::kCancelPristine;
    cut->file=retain_object(&file);cut->service=service;cut->expected=expected;cut->publication=publication;
    cut->actual_native_header_index.assign(raw.begin(),raw.begin()+size_t(prefix));
    return NativeProfileSaveOwner::PrepareManagedFileCut(cut);
  }

  static auto Pending(const std::shared_ptr<NativeProfileSavePublication>& owner) { return owner->pending_; }
  static auto Plan(const std::shared_ptr<NativeProfileSavePublication>& owner, std::error_code& error) {
    return owner->pending_->BuildPlan(error);
  }
  static std::error_code Seal(const std::shared_ptr<NativeProfileSavePublication>& owner,
      uint32_t title, const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 24) { return std::make_error_code(std::errc::invalid_argument); }
    const uint64_t prefix = 24 + uint64_t(XdbfTableView::U32(bytes.data()+8))*18 +
                           uint64_t(XdbfTableView::U32(bytes.data()+16))*8;
    if (prefix > bytes.size()) { return std::make_error_code(std::errc::invalid_argument); }
    NativeProfileSavePublication::SealedGpd staged{bytes,
        std::vector<uint8_t>(bytes.begin(),bytes.begin()+size_t(prefix))};
    std::map<uint32_t,NativeProfileSavePublication::SealedGpd> packet;
    packet.emplace(title,std::move(staged));
    return owner->SealAtOriginalDbCut(std::move(packet));
  }
  static auto Publish(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    return owner->PublishAtOriginalCommitCut();
  }
  static auto RecoverAttempted(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    return owner->RecoverAttemptedAtActualRundown();
  }
  static auto CancelUnbound(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    return owner->CancelUnboundAtActualRundown();
  }
  static auto PublicAttempt(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    REQUIRE(owner->transaction_); return owner->transaction_->Attempt();
  }
  static auto Cancel(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    return owner->CancelAtOriginalQuiescence();
  }
  static auto CreateStage(const std::shared_ptr<NativeProfileSavePublication>& owner,uint32_t title,std::error_code& error) {
    return owner->CreateManagedGpdStage(title,error);
  }
  static auto OpenStage(const std::shared_ptr<NativeProfileSaveGpdStage>& stage,
      std::shared_ptr<vfs::HostPathDevice> device,vfs::File** file) {
    return stage->OpenManagedView(std::move(device),"FFFE07D1.gpd",
        vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,file);
  }
  static auto PictureService(std::shared_ptr<ProfileGpdModelAuthority> model,
      filesystem::ProfileArtifactKind kind,std::error_code& error) {
    return NativeProfileSaveManagedPicture::CreateSource(std::move(model),kind,error);
  }
  static vfs::FileOpenContext PictureContext(const std::shared_ptr<NativeProfileSaveManagedPicture>& service,
      const std::shared_ptr<NativeProfileSavePublication>& publication,std::shared_ptr<vfs::Device> device,
      const char* leaf,std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueForActualOpen(publication,std::move(device),leaf,error);return context;
  }
  static vfs::FileOpenContext PictureReadContext(const std::shared_ptr<NativeProfileSaveManagedPicture>& service,
      ProfileGpdReadCapture read,std::shared_ptr<vfs::Device> device,const char* leaf,std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueReadForActualOpen(std::move(read),std::move(device),leaf,error);return context;
  }
  static auto CreatePictureStage(const std::shared_ptr<NativeProfileSavePublication>& owner,
      filesystem::ProfileArtifactKind kind,std::error_code& error) {return owner->CreateManagedPictureStage(kind,error);}
  static auto OpenPictureStage(const std::shared_ptr<NativeProfileSavePictureStage>& stage,
      const std::shared_ptr<vfs::HostPathDevice>& device,vfs::File** file) {
    const auto* leaf=NativeProfileSavePictureStage::Leaf(stage->kind_);REQUIRE(leaf);
    vfs::FileAction action{};
    return stage->OpenManagedView(device,leaf,vfs::FileDisposition::kOpenIf,
        vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,file,&action);
  }
  static auto SealPictures(const std::shared_ptr<NativeProfileSavePublication>& owner) {
    return owner->SealManagedPicturesAtOriginalFileCut();
  }
  static void RetirePicture(const std::shared_ptr<NativeProfileSavePictureStage>& stage) {stage->RetireAtActualQuiescence();}
  static auto StagePath(const std::shared_ptr<NativeProfileSaveGpdStage>& stage) {
    return stage->recovery_->root()->path()/stage->artifact_;
  }
  static auto SealStage(const std::shared_ptr<NativeProfileSavePublication>& owner,
      uint32_t title,const std::vector<uint8_t>& raw) {
    const uint64_t prefix=24+uint64_t(XdbfTableView::U32(raw.data()+8))*18+
                            uint64_t(XdbfTableView::U32(raw.data()+16))*8;
    REQUIRE(prefix<=raw.size());
    return owner->SealManagedGpdAtOriginalDbCut(title,std::span(raw).first(size_t(prefix)));
  }
  static void RetireStage(const std::shared_ptr<NativeProfileSaveGpdStage>& stage) {stage->RetireAtActualQuiescence();}
  static auto SourceService(std::shared_ptr<ProfileGpdModelAuthority> model,std::error_code& error,
      std::shared_ptr<const std::atomic<bool>> family_closed={}) {
    return NativeProfileSaveManagedGpd::CreateSource(std::move(model),kDashboardID,error,std::move(family_closed));
  }
  static auto JoinReadModel(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      const std::shared_ptr<ProfileGpdModelAuthority>& model) {return service->JoinCommittedReadModel(model);}
  static auto Service(const std::shared_ptr<NativeProfileSaveGpdStage>& stage,std::error_code& error) {
    return NativeProfileSaveManagedGpd::Create(stage,error);
  }
  static vfs::FileOpenContext OpenContext(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      const std::shared_ptr<NativeProfileSavePublication>& publication,
      std::shared_ptr<vfs::Device> device,std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueForActualOpen(publication,std::move(device),"FFFE07D1.gpd",error);
    return context;
  }
  static vfs::FileOpenContext ReadContext(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      ProfileGpdReadCapture read,std::shared_ptr<vfs::Device> device,std::error_code& error) {
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueReadForActualOpen(std::move(read),std::move(device),"FFFE07D1.gpd",error);
    return context;
  }
  static auto ReadContextWithDuty(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      ProfileGpdReadCapture read,std::shared_ptr<vfs::Device> device,std::shared_ptr<void> duty,std::error_code& error) {
    auto receipt=NativeProfileSaveManagedGpd::IssueReadReceipt();
    vfs::FileOpenContext context;context.expected_device=device;
    context.managed_authority=service->IssueReadForActualOpen(std::move(read),std::move(device),
        "FFFE07D1.gpd",error,std::move(duty),receipt);
    return std::make_pair(std::move(context),std::move(receipt));
  }
  static auto NewReadReceipt() {return NativeProfileSaveManagedGpd::IssueReadReceipt();}
  static auto MatchReadReceipt(vfs::File& file,const auto& receipt,std::error_code& error) {
    return NativeProfileSaveManagedGpd::TryCaptureOpenedRead(file,receipt,error);
  }
  static auto ReadView(vfs::File& file) {return NativeProfileSaveManagedGpd::CaptureOwnedReadView(file);}
  static auto CurrentFileRead(vfs::File& file,const std::shared_ptr<ProfileGpdModelAuthority>& model,std::error_code& error) {
    return NativeProfileSaveManagedGpd::TryCaptureCurrentFileRead(file,model,kDashboardID,error);
  }

  static X_STATUS Upgrade(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      vfs::File& file,const std::shared_ptr<NativeProfileGpdReadView>& expected,
      const vfs::FileOpenContext& context,std::span<const uint8_t> bytes) {
    REQUIRE(bytes.size()>=24);
    const uint64_t prefix=24+uint64_t(XdbfTableView::U32(bytes.data()+8))*18+
                            uint64_t(XdbfTableView::U32(bytes.data()+16))*8;
    REQUIRE(prefix<=bytes.size());
    return service->UpgradeAtActualDbQuiescence(file,expected,context,bytes.first(size_t(prefix)));
  }
  static X_STATUS Promote(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      vfs::File& file,const std::shared_ptr<NativeProfileSavePublication>& publication,
      std::span<const uint8_t> bytes) {
    REQUIRE(bytes.size()>=24);
    const uint64_t prefix=24+uint64_t(XdbfTableView::U32(bytes.data()+8))*18+
                            uint64_t(XdbfTableView::U32(bytes.data()+16))*8;
    REQUIRE(prefix<=bytes.size());
    return service->PromoteCommittedAtActualDbQuiescence(file,publication,bytes.first(size_t(prefix)));
  }
  // Shared cache CONSUMER component only. Synthetic fixture setup does not
  // authenticate the original native control/DB/current-table issuer.
  static X_STATUS JointCache(const std::shared_ptr<NativeProfileSavePublication>& owner,
      object_ref<XFile> raw,object_ref<XFile> gpd,
      const std::shared_ptr<NativeProfileSaveManagedGpd>& service,std::span<const uint8_t> bytes={}) {
    std::span<const uint8_t> index;
    if(gpd) {
      REQUIRE(bytes.size()>=24);
      const uint64_t prefix=24+uint64_t(XdbfTableView::U32(bytes.data()+8))*18+
          uint64_t(XdbfTableView::U32(bytes.data()+16))*8;
      REQUIRE(prefix<=bytes.size());index=bytes.first(size_t(prefix));
    }
    return owner->PromoteJoinedAtActualNativeCheckpoint(std::move(raw),std::move(gpd),service,index);
  }
  static X_STATUS FileCut(const std::shared_ptr<NativeProfileSaveManagedGpd>& service,
      object_ref<XFile> file,const std::shared_ptr<NativeProfileGpdReadView>& expected,
      const vfs::FileOpenContext& context,const std::shared_ptr<NativeProfileSavePublication>& publication,
      std::span<const uint8_t> bytes,bool promote=false) {
    REQUIRE(bytes.size()>=24);
    const uint64_t prefix=24+uint64_t(XdbfTableView::U32(bytes.data()+8))*18+
                            uint64_t(XdbfTableView::U32(bytes.data()+16))*8;
    REQUIRE(prefix<=bytes.size());
    auto cut=std::make_shared<NativeProfileSaveOwner::ManagedFileCut>();
    cut->kind=promote?NativeProfileSaveOwner::ManagedFileCut::Kind::kPromote:NativeProfileSaveOwner::ManagedFileCut::Kind::kAdopt;
    cut->file=std::move(file);cut->service=service;cut->expected=expected;cut->open=context;cut->publication=publication;
    cut->actual_native_header_index.assign(bytes.begin(),bytes.begin()+size_t(prefix));
    return NativeProfileSaveOwner::RunManagedFileCut(cut);
  }
  static void RetireRead(vfs::File& file) {
    const auto view=NativeProfileSaveManagedGpd::CaptureOwnedReadView(file);REQUIRE(view);
    view->RetireAtActualQuiescence();
  }
  static auto HoldStageMetadataForClose(const std::shared_ptr<NativeProfileSaveGpdStage>& stage) {
    return std::unique_lock(stage->mutex_);
  }
  static auto HoldStageMetadataForClose(const std::shared_ptr<NativeProfileSavePictureStage>& stage) {
    return std::unique_lock(stage->mutex_);
  }
  static size_t StageViews(const std::shared_ptr<NativeProfileSaveGpdStage>& stage) {
    return stage->managed_views_.load(std::memory_order_acquire);
  }
  static size_t StageViews(const std::shared_ptr<NativeProfileSavePictureStage>& stage) {
    return stage->managed_views_.load(std::memory_order_acquire);
  }
};
namespace {
template<class Stage>
void NativeStageCloseUnderActualGcrc(const std::shared_ptr<Stage>& stage,vfs::File*& file) {
  REQUIRE(file);REQUIRE(NativeProfileSavePublicationTestAccess::StageViews(stage)==1);
  std::thread worker;
  std::promise<void> entered,done;
  auto entering=entered.get_future(),finished=done.get_future();
  auto held=NativeProfileSavePublicationTestAccess::HoldStageMetadataForClose(stage);
  // Declare BEFORE constructing the worker: all assertion/exception paths
  // unlock the deliberately held stage metadata and then join the real close.
  struct ReleaseAndJoin {
    decltype(held)& lock;std::thread& worker;
    ~ReleaseAndJoin(){if(lock.owns_lock())lock.unlock();if(worker.joinable())worker.join();}
  } cleanup{held,worker};
  auto* raw=file;
  worker=std::thread([raw,&entered,&done] {
    const auto destroy=[](vfs::File* actual){if(actual)actual->Destroy();};
    std::unique_ptr<vfs::File,decltype(destroy)> owner(raw,destroy);
    try {
      auto actual_gcrc=global_critical_region::AcquireDirect();
      entered.set_value();owner.reset();done.set_value();
    }catch(...) {
      const auto error=std::current_exception();
      try {entered.set_exception(error);}catch(...){}
      try {done.set_exception(error);}catch(...){}
    }
  });
  file=nullptr; // Worker construction succeeded; exactly that real close owns it.
  const bool did_enter=entering.wait_for(std::chrono::seconds(2))==std::future_status::ready;
  const bool retired=did_enter && finished.wait_for(std::chrono::seconds(2))==std::future_status::ready;
  // Old mutex-taking destruction fails this assertion but cannot strand the
  // process: unlock/join occurs BEFORE REQUIRE consumes the observed result.
  held.unlock();worker.join();
  REQUIRE(did_enter);entering.get();REQUIRE(retired);finished.get();
  CHECK(NativeProfileSavePublicationTestAccess::StageViews(stage)==0);
}
std::vector<uint8_t> NativeTestRepack(std::span<const uint8_t> canonical) {
  const auto view=XdbfTableView::Parse(canonical); REQUIRE(view);
  const uint32_t used=view->entries_used, capacity=used+3;
  const size_t prefix=24+size_t(capacity)*18+8;
  std::vector<uint8_t> out(prefix,0);
  std::copy_n(canonical.begin(),8,out.begin());
  auto put=[&](size_t at,uint32_t value){
    out[at]=uint8_t(value>>24); out[at+1]=uint8_t(value>>16);
    out[at+2]=uint8_t(value>>8); out[at+3]=uint8_t(value);
  };
  put(8,capacity); put(12,used); put(16,1); put(20,1);
  for(size_t i=0;i<used;++i) {
    const auto* old=view->entries.data()+(used-1-i)*18;
    const size_t at=24+i*18;
    std::copy_n(old,18,out.data()+at);
    put(at+10,uint32_t(out.size()-prefix));
    const auto data=view->data.subspan(XdbfTableView::U32(old+10),XdbfTableView::U32(old+14));
    out.insert(out.end(),data.begin(),data.end());
  }
  const uint32_t eof=uint32_t(out.size()-prefix);
  put(24+size_t(capacity)*18,eof); put(24+size_t(capacity)*18+4,UINT32_MAX-eof);
  // Opaque trailing/slack bytes are legal and stay exact in the native image.
  out.push_back(0xAC); out.push_back(0x71);
  REQUIRE_FALSE(ValidateGpdSnapshot(out)); return out;
}
}
TEST_CASE("Native pending ownership excludes host flush and preserves exact sealed layout or original bytes across cancellation expiry and restart",
          "[.installed_profile_creation][kernel][native-save][pending][persistence]") {
  CreationRuntime runtime(true);
  const auto created=runtime.profiles().CreateProfileDetailed("NativeOwner",true);
  REQUIRE(created.status==X_ERROR_SUCCESS); REQUIRE(created.signed_in);
  const auto xuid=created.xuid;
  const auto package=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(xuid);
  const auto before=CreationRead(package/"FFFE07D1.gpd"),account=CreationRead(package/"Account");
  const auto appearance=Appearance(GpdInfoProfile(before));
  const std::array<uint8_t,3> save{0x31,0,0x91}; WriteCreationSave(runtime,xuid,"NativeOwnedSave",save);
  std::vector<uint8_t> expected=before;
  {
    std::error_code error; const auto roster=runtime.profiles().CaptureRosterExpected(error);
    REQUIRE_FALSE(error); REQUIRE(roster.roster);
    auto source=NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),roster.roster,{},0,xuid,error);
    REQUIRE(source); REQUIRE_FALSE(error);
    auto writer=NativeProfileSaveWriterTestAccess::Writer(source);
    ProfileGpdMutation command; command.kind=ProfileGpdMutationKind::kUpsertImage;
    command.title_id=kDashboardID; command.image_id=0x1234567800008007ull;
    command.image_data={0x62,0,0x93}; command.request_id=writer.model->AllocateRequestId(error); REQUIRE_FALSE(error);
    auto owner=NativeProfileSavePublicationTestAccess::Prepare(source,command,error);
    REQUIRE(owner); REQUIRE_FALSE(error);
    const auto pending=NativeProfileSavePublicationTestAccess::Pending(owner); REQUIRE(pending->native_issued());
    const auto plan=NativeProfileSavePublicationTestAccess::Plan(owner,error); REQUIRE(plan); REQUIRE_FALSE(error);
    REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    REQUIRE(NativeProfileSavePublicationTestAccess::Publish(owner).error==std::errc::device_or_resource_busy);
    REQUIRE(writer.Submit(command).error==std::errc::device_or_resource_busy);
    REQUIRE(writer.ExecutePending(pending).error==std::errc::device_or_resource_busy);
    REQUIRE_FALSE(writer.model->AcquireTransaction(writer.owner,writer.recovery,error,writer.metadata));
    REQUIRE(error==std::errc::device_or_resource_busy);
    REQUIRE_FALSE(ProfileGpdTransaction::Create(writer.model,pending,writer.owner,writer.recovery,error,writer.metadata));
    REQUIRE(error==std::errc::device_or_resource_busy);
    auto dependency=writer.model->Dependencies(pending,error); REQUIRE(dependency); REQUIRE_FALSE(error);
    REQUIRE_FALSE(writer.model->PrepareCommit(pending,*dependency,error));
    REQUIRE(error==std::errc::device_or_resource_busy);
    const auto initial_rows=GpdDurableIntentStore::Scan(*writer.owner,error); REQUIRE_FALSE(error);
    SECTION("ordinary filesystem reads preserve RW intent without native object authority and refuse every backing mutation across publication restart") {
      const auto service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error);
      REQUIRE(service);REQUIRE_FALSE(error);
      const auto picture_service=NativeProfileSavePublicationTestAccess::PictureService(
          writer.model,filesystem::ProfileArtifactKind::kPublic64,error);
      REQUIRE(picture_service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));REQUIRE(fs->RegisterManagedFile(picture_service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      constexpr uint32_t rw=vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite;
      vfs::File* opened=nullptr;vfs::File* picture=nullptr;vfs::File* refused=nullptr;
      struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} opened_close{opened},picture_close{picture},refused_close{refused};
      vfs::FileAction action{};
      // The actual existing native pending owner remains busy. No guessed
      // native profile object, caller slot, Task or raw-open fallback is used.
      REQUIRE(XFAILED(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,rw,false,true,&refused,&action)));
      REQUIRE_FALSE(refused);REQUIRE(action==vfs::FileAction::kDoesNotExist);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,rw,false,true,&opened,&action)==X_STATUS_SUCCESS);
      REQUIRE(opened);REQUIRE(action==vfs::FileAction::kOpened);
      REQUIRE((opened->file_access()&rw)==rw); // Original requested access ABI is intact.
      const auto picture_before=CreationRead(package/"tile_64.png");
      REQUIRE(fs->OpenFile(root,"tile_64.png",vfs::FileDisposition::kOpenIf,rw,false,true,&picture,&action)==X_STATUS_SUCCESS);
      REQUIRE(picture);REQUIRE(action==vfs::FileAction::kOpened);
      std::vector<uint8_t> picture_bytes(picture_before.size());size_t count=0;
      REQUIRE(picture->ReadSync(picture_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==picture_before.size());REQUIRE(picture_bytes==picture_before);
      size_t denied=99;REQUIRE(picture->WriteSync(std::array<uint8_t,1>{0},0,&denied)==X_STATUS_ACCESS_DENIED);REQUIRE(denied==0);
      REQUIRE(picture->SetLength(0)==X_STATUS_ACCESS_DENIED);
      REQUIRE(picture->SetAllocationSize(picture_before.size()+4096)==X_STATUS_ACCESS_DENIED);
      REQUIRE(picture->SetTimes(1,2,3)==X_STATUS_ACCESS_DENIED);
      auto file=object_ref<XFile>(new XFile(runtime.emulator->kernel_state(),opened,true,false,true));opened=nullptr;
      struct Release {object_ref<XFile>& file;~Release(){if(file){file->ReleaseHandle();file.reset();}}} release{file};
      file->set_position(23);
      vfs::File::Information initial{};REQUIRE(file->QueryInformation(&initial)==X_STATUS_SUCCESS);
      REQUIRE(initial.end_of_file==before.size());
      REQUIRE(file->SetLength(0)==X_STATUS_ACCESS_DENIED);
      REQUIRE(file->SetAllocationSize(before.size()+8192)==X_STATUS_ACCESS_DENIED);
      REQUIRE(file->SetTimes(initial.create_timestamp+1,0,0)==X_STATUS_ACCESS_DENIED);
      REQUIRE(file->Rename(xe::to_path(device->mount_path()+"\\forbidden.gpd"),false)==X_STATUS_ACCESS_DENIED);
      REQUIRE_FALSE(file->entry()->allows_namespace_mutation());REQUIRE_FALSE(file->entry()->delete_on_close());
      CreationPage page(*runtime.emulator->memory());
      const auto set=runtime.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",0xF7);
      REQUIRE(set);REQUIRE(set->function_data.trampoline);
      const auto invoke=[&](uint32_t kind,uint32_t bytes) {
        auto context=CreationContext(runtime,page.address+0x2000);
        context.r[3]=file->handle();context.r[4]=page.address;context.r[5]=page.address+0x100;
        context.r[6]=bytes;context.r[7]=kind;
        set->function_data.trampoline(&context);return uint32_t(context.r[3]);
      };
      auto* iosb=runtime.emulator->memory()->TranslateVirtual(page.address);
      std::array<uint8_t,sizeof(X_IO_STATUS_BLOCK)> iosb_before{};std::memcpy(iosb_before.data(),iosb,iosb_before.size());
      xe::store_and_swap<uint64_t>(runtime.emulator->memory()->TranslateVirtual(page.address+0x100),before.size()+8192);
      REQUIRE(invoke(XFileAllocationInformation,8)==X_STATUS_ACCESS_DENIED); // >=EOF must reach the backend.
      REQUIRE(std::memcmp(iosb_before.data(),iosb,iosb_before.size())==0);
      xe::store_and_swap<uint64_t>(runtime.emulator->memory()->TranslateVirtual(page.address+0x100),0);
      REQUIRE(invoke(XFileAllocationInformation,8)==X_STATUS_ACCESS_DENIED); // shrink is denied too.
      *runtime.emulator->memory()->TranslateVirtual(page.address+0x100)=1;
      REQUIRE(invoke(XFileDispositionInformation,1)==X_STATUS_ACCESS_DENIED);
      REQUIRE_FALSE(file->entry()->delete_on_close());
      REQUIRE(std::memcmp(iosb_before.data(),iosb,iosb_before.size())==0);
      std::array<uint8_t,16> control;control.fill(0xA5);const auto prior_control=control;uint32_t transferred=99;
      REQUIRE_FALSE(file->file()->DescribeDeviceControl(0x70000));
      REQUIRE(file->file()->DeviceControl(0x70000,{},control,&transferred)==X_STATUS_INVALID_DEVICE_REQUEST);
      REQUIRE(transferred==0);REQUIRE(control==prior_control);
      const auto flush_callbacks=std::make_shared<std::atomic<unsigned>>(0);
      REQUIRE(file->FlushQueued([flush_callbacks](X_STATUS){++*flush_callbacks;})==X_STATUS_NOT_SUPPORTED);
      REQUIRE(flush_callbacks->load()==0); // No queue/backend entry for this immutable File class.
      REQUIRE(file->Flush()==X_STATUS_SUCCESS); // Observes real committed token durability only.
      REQUIRE(file->position()==23);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);REQUIRE(CreationRead(package/"tile_64.png")==picture_before);
      // Ordinary old read ownership has no foreign writer lease. The real
      // shared HOST transaction can publish while these captured Files live.
      command.request_id=writer.model->AllocateRequestId(error);REQUIRE_FALSE(error);
      const auto committed_read=writer.model->CaptureCommittedFileRead(error);REQUIRE(committed_read);REQUIRE_FALSE(error);
      auto held=writer.model->storage()->view()->ValidateSnapshotBatch(committed_read->tokens,error);
      REQUIRE(held);REQUIRE_FALSE(error);
      std::promise<ProfileGpdTransactionAttempt> publication;auto outcome=publication.get_future();std::thread publisher;
      struct ReleaseAndJoin {
        std::unique_ptr<filesystem::HostStorageSnapshotBatch>& held;std::thread& worker;
        ~ReleaseAndJoin(){held.reset();if(worker.joinable())worker.join();}
      } join{held,publisher}; // BEFORE thread construction or any assertion while the fence is held.
      publisher=std::thread([writer,command,promise=std::move(publication)]()mutable{
        try{promise.set_value(writer.Submit(command));}catch(...){promise.set_exception(std::current_exception());}
      });
      bool pending_seen=false;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while(std::chrono::steady_clock::now()<until){
        std::error_code sampled;pending_seen=bool(writer.model->Pending(sampled));
        if(pending_seen || outcome.wait_for(std::chrono::milliseconds(0))==std::future_status::ready)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      REQUIRE(pending_seen);
      REQUIRE(outcome.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready);
      REQUIRE(XFAILED(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,rw,false,true,&refused,&action)));
      REQUIRE_FALSE(refused);REQUIRE(action==vfs::FileAction::kDoesNotExist);
      held.reset();publisher.join();
      const auto committed=outcome.get();REQUIRE_FALSE(committed.error);
      REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
      const auto after=CreationRead(package/"FFFE07D1.gpd");REQUIRE(after!=before);expected=after;
      std::vector<uint8_t> retained(before.size());count=0;
      REQUIRE(file->file()->ReadSync(retained,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());REQUIRE(retained==before);
      vfs::File::Information retained_info{};REQUIRE(file->QueryInformation(&retained_info)==X_STATUS_SUCCESS);
      REQUIRE(retained_info.end_of_file==initial.end_of_file);REQUIRE(retained_info.write_timestamp==initial.write_timestamp);
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpenIf,rw,false,true,&refused,&action)==X_STATUS_SUCCESS);
      REQUIRE(refused);REQUIRE(action==vfs::FileAction::kOpened);
      std::vector<uint8_t> fresh(after.size());count=0;
      REQUIRE(refused->ReadSync(fresh,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==after.size());REQUIRE(fresh==after);
      REQUIRE_FALSE(std::filesystem::exists(package/"forbidden.gpd"));
    }
    SECTION("ordinary read model census handles both insertion orders with old alive snapshots and a later real pending owner") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      const auto exercise=[&](bool reverse) {
        const auto old=writer.model;
        const auto next=ProfileGpdModelAuthority::Create(xuid,1,old->sign_in_generation()+1,old->storage(),error);
        REQUIRE(next);REQUIRE_FALSE(error);
        const ProfileGpdWriter current{next,writer.owner,writer.recovery,writer.metadata};
        REQUIRE_FALSE(current.Reload()); // Genuine common recovery/load, including all fixed PNG tokens.
        const auto service=NativeProfileSavePublicationTestAccess::SourceService(reverse?next:old,error);
        REQUIRE(service);REQUIRE_FALSE(error);
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::JoinReadModel(service,reverse?old:next));
        auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
        const auto device=old->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
        vfs::File* retained=nullptr;vfs::File* fresh=nullptr;vfs::File* refused=nullptr;
        struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} a{retained},b{fresh},c{refused};
        vfs::FileAction action{};
        REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,
            false,true,&retained,&action)==X_STATUS_SUCCESS);REQUIRE(retained);
        auto edit=command;edit.request_id=next->AllocateRequestId(error);REQUIRE_FALSE(error);
        const auto published=current.Submit(edit);REQUIRE_FALSE(published.error);
        REQUIRE(published.state==ProfileGpdTransactionAttempt::State::kCommitted);
        const auto after=CreationRead(package/"FFFE07D1.gpd");REQUIRE(after!=before);expected=after;
        // Deliberately keep the old model nonretired/current and strongly alive:
        // its immutable full token set is stale on disk, not unavailable metadata.
        const auto old_read=old->CaptureRead(error);REQUIRE(old_read);REQUIRE_FALSE(error);
        REQUIRE(old->IsCurrent(old_read->origin));
        REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,
            false,true,&fresh,&action)==X_STATUS_SUCCESS);REQUIRE(fresh);
        std::vector<uint8_t> old_bytes(before.size()),new_bytes(after.size());size_t count=0;
        REQUIRE(retained->ReadSync(old_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());REQUIRE(old_bytes==before);
        REQUIRE(fresh->ReadSync(new_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==after.size());REQUIRE(new_bytes==after);
        const auto census=old->CaptureCommittedFileRead(error);REQUIRE(census);REQUIRE_FALSE(error);
        REQUIRE(old->IsCommittedFileReadCurrent(*census));
        auto pending_edit=command;pending_edit.image_id^=0x100;pending_edit.request_id=old->AllocateRequestId(error);REQUIRE_FALSE(error);
        const auto pending_owner=NativeProfileSavePublicationTestAccess::Prepare(source,pending_edit,error);
        REQUIRE(pending_owner);REQUIRE_FALSE(error);
        REQUIRE(old->Pending(error));REQUIRE_FALSE(error);
        // In the new-first order the first candidate still fully matches disk.
        // The later genuine pending owner must nevertheless refuse this open.
        REQUIRE(XFAILED(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,
            false,true,&refused,&action)));
        REQUIRE_FALSE(refused);REQUIRE(action==vfs::FileAction::kDoesNotExist);
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(pending_owner));
        REQUIRE(old->IsCurrent(census->origin)); // No semantic/model version changed.
        REQUIRE_FALSE(old->IsCommittedFileReadCurrent(*census)); // Real begin/cancel ABA is nevertheless stale.
        const auto resampled=old->CaptureCommittedFileRead(error);REQUIRE(resampled);REQUIRE_FALSE(error);
        REQUIRE(old->IsCommittedFileReadCurrent(*resampled));
        REQUIRE(CreationRead(package/"FFFE07D1.gpd")==after);
        REQUIRE(retained->ReadSync(old_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(old_bytes==before);
      };
      SECTION("old tuple inserted before new tuple"){exercise(false);}
      SECTION("new tuple inserted before old tuple"){exercise(true);}
    }
    SECTION("joining an actual loaded model while an ordinary read waits on the real physical fence refuses that census and permits a fresh retry") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      const auto old=writer.model;
      const auto next=ProfileGpdModelAuthority::Create(xuid,1,old->sign_in_generation()+1,old->storage(),error);
      REQUIRE(next);REQUIRE_FALSE(error);
      const ProfileGpdWriter next_writer{next,writer.owner,writer.recovery,writer.metadata};REQUIRE_FALSE(next_writer.Reload());
      const auto service=NativeProfileSavePublicationTestAccess::SourceService(old,error);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=old->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto read=old->CaptureCommittedFileRead(error);REQUIRE(read);REQUIRE_FALSE(error);
      auto held=old->storage()->view()->ValidateSnapshotBatch(read->tokens,error);REQUIRE(held);REQUIRE_FALSE(error);
      struct Result {vfs::File* file=nullptr;vfs::FileAction action{};X_STATUS status=X_STATUS_CANCELLED;
        std::promise<void> done;~Result(){if(file)file->Destroy();}};
      const auto result=std::make_shared<Result>();auto finished=result->done.get_future();std::thread reader;
      struct ReleaseAndJoin {std::unique_ptr<filesystem::HostStorageSnapshotBatch>& held;std::thread& worker;
        ~ReleaseAndJoin(){held.reset();if(worker.joinable())worker.join();}} join{held,reader};
      const auto baseline=old.use_count();
      reader=std::thread([fs,root,result]{try{
          result->status=fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,
              false,true,&result->file,&result->action);result->done.set_value();
        }catch(...){result->done.set_exception(std::current_exception());}});
      // This isolated worker captures no model in its closure. Its extra
      // strong model owner proves it has copied the real source candidate set;
      // the held physical fence prevents it from successfully completing.
      const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while(old.use_count()<=baseline && std::chrono::steady_clock::now()<until &&
          finished.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      REQUIRE(old.use_count()>baseline);
      REQUIRE(finished.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::JoinReadModel(service,next));
      held.reset();reader.join();finished.get();
      REQUIRE(XFAILED(result->status));REQUIRE_FALSE(result->file);REQUIRE(result->action==vfs::FileAction::kDoesNotExist);
      vfs::File* fresh=nullptr;struct Close{vfs::File*& file;~Close(){if(file)file->Destroy();}} close{fresh};vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,
          false,true,&fresh,&action)==X_STATUS_SUCCESS);REQUIRE(fresh);
      std::vector<uint8_t> bytes(before.size());size_t count=0;
      REQUIRE(fresh->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());REQUIRE(bytes==before);
    }
    SECTION("ordinary GPD open rejects a changed sibling picture tuple and missing OpenIf never creates a protected PNG") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      const auto service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error);REQUIRE(service);REQUIRE_FALSE(error);
      const auto missing=NativeProfileSavePublicationTestAccess::PictureService(
          writer.model,filesystem::ProfileArtifactKind::kPersonal64,error);REQUIRE(missing);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));REQUIRE(fs->RegisterManagedFile(missing));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      vfs::File* old=nullptr;vfs::File* refused=nullptr;
      struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} old_close{old},refused_close{refused};vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,vfs::FileAccess::kGenericRead,
          false,true,&old,&action)==X_STATUS_SUCCESS);REQUIRE(old);
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOpenIf,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&refused,&action)==X_STATUS_OBJECT_NAME_NOT_FOUND);
      REQUIRE_FALSE(refused);REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));
      const auto target=package/"tile_64.png";const auto backup=package/"committed-picture-original.fixture";
      const auto original=CreationRead(target);
      struct Restore {std::filesystem::path target,backup;bool active=false;
        ~Restore(){if(active){std::error_code e;std::filesystem::remove(target,e);std::filesystem::rename(backup,target,e);}}} restore{target,backup};
      std::filesystem::rename(target,backup,error);REQUIRE_FALSE(error);restore.active=true;
      {std::ofstream out(target,std::ios::binary);REQUIRE(out);out.put('!');REQUIRE(out);}
      // The requested GPD bytes did not change. Still, no source may combine
      // its old committed model with this foreign/partial picture generation.
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE(XFAILED(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&refused,&action)));
      REQUIRE_FALSE(refused);REQUIRE(action==vfs::FileAction::kDoesNotExist);
      std::vector<uint8_t> retained(before.size());size_t count=0;
      REQUIRE(old->ReadSync(retained,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());REQUIRE(retained==before);
      REQUIRE(CreationRead(backup)==original);
    }
    SECTION("real GPD stage final File destruction under GCRC does not wait on its held IO mutex") {
      const auto stage=NativeProfileSavePublicationTestAccess::CreateStage(owner,kDashboardID,error);
      REQUIRE(stage);REQUIRE_FALSE(error);
      const auto service=NativeProfileSavePublicationTestAccess::Service(stage,error);
      REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto context=NativeProfileSavePublicationTestAccess::OpenContext(service,owner,device,error);
      REQUIRE(context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* file=nullptr;vfs::FileAction action{};
      struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} close{file};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&file,&action,context)==X_STATUS_SUCCESS);
      REQUIRE(file);REQUIRE(action==vfs::FileAction::kOpened);
      NativeStageCloseUnderActualGcrc(stage,file);REQUIRE_FALSE(file);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      NativeProfileSavePublicationTestAccess::RetireStage(stage);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size());REQUIRE_FALSE(error);
    }
    SECTION("real PNG stage final File destruction under GCRC does not wait on its held IO mutex") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      ProfileGpdMutation pictures;pictures.kind=ProfileGpdMutationKind::kSetPictures;
      pictures.title_id=kDashboardID;pictures.request_id=writer.model->AllocateRequestId(error);REQUIRE_FALSE(error);
      const auto pair=PrepareHostProfilePicturePair(xe::kernel::test::SyntheticProfilePicture64(),error);
      REQUIRE(pair);REQUIRE_FALSE(error);
      pictures.picture_changes={{filesystem::ProfileArtifactKind::kPersonal64,false,pair->large},
                                {filesystem::ProfileArtifactKind::kPersonal32,false,pair->small}};
      const auto picture_owner=NativeProfileSavePublicationTestAccess::Prepare(source,pictures,error);
      REQUIRE(picture_owner);REQUIRE_FALSE(error);
      const auto stage=NativeProfileSavePublicationTestAccess::CreatePictureStage(
          picture_owner,filesystem::ProfileArtifactKind::kPersonal64,error);REQUIRE(stage);REQUIRE_FALSE(error);
      const auto service=NativeProfileSavePublicationTestAccess::PictureService(
          writer.model,filesystem::ProfileArtifactKind::kPersonal64,error);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto context=NativeProfileSavePublicationTestAccess::PictureContext(
          service,picture_owner,device,"pp_64.png",error);REQUIRE(context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* file=nullptr;vfs::FileAction action{};
      struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} close{file};
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOpenIf,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&file,&action,context)==X_STATUS_SUCCESS);
      REQUIRE(file);REQUIRE(action==vfs::FileAction::kCreated);
      NativeStageCloseUnderActualGcrc(stage,file);REQUIRE_FALSE(file);
      NativeProfileSavePublicationTestAccess::RetirePicture(stage);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(picture_owner));
      REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));
      REQUIRE_FALSE(std::filesystem::exists(package/"pp_32.png"));
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size());REQUIRE_FALSE(error);
    }
    SECTION("genuine complete typed delta keeps native table and raw layout through durable model VFS publication") {
      auto native=NativeTestRepack(plan->files.at(kDashboardID).intended);
      REQUIRE(native!=plan->files.at(kDashboardID).intended);
      auto bad=native; const auto parsed=XdbfTableView::Parse(bad); REQUIRE(parsed);
      const auto data=parsed->Find(uint16_t(GpdSection::kImage),command.image_id); REQUIRE(data); REQUIRE_FALSE(data->empty());
      bad[size_t(data->data()-bad.data())]^=0x40;
      REQUIRE(NativeProfileSavePublicationTestAccess::Seal(owner,kDashboardID,bad)==std::errc::state_not_recoverable);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Seal(owner,kDashboardID,native));
      REQUIRE(writer.ExecutePending(pending).error==std::errc::device_or_resource_busy);
      auto committed=NativeProfileSavePublicationTestAccess::Publish(owner);
      REQUIRE_FALSE(committed.error); REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==native);
      REQUIRE(NativeProfileSavePublicationTestAccess::PublicAttempt(owner).error==std::errc::device_or_resource_busy);
      const auto again=NativeProfileSavePublicationTestAccess::Publish(owner);
      REQUIRE_FALSE(again.error); REQUIRE(again.state==ProfileGpdTransactionAttempt::State::kAlreadyCommitted);
      const auto read=writer.model->CaptureRead(error); REQUIRE(read); REQUIRE_FALSE(error);
      const auto saved=read->models->dashboard.GetEntry(uint16_t(GpdSection::kImage),command.image_id);
      REQUIRE(saved); REQUIRE(saved->data==command.image_data);
      REQUIRE_FALSE(NativeProfileSaveWriterTestAccess::Current(source,error));
      REQUIRE(error==std::errc::operation_canceled);
      const auto rows=GpdDurableIntentStore::Scan(*writer.owner,error); REQUIRE_FALSE(error);
      REQUIRE(rows.size()==initial_rows.size()+1);
      const auto receipt=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.journal==committed.storage.journal;});
      REQUIRE(receipt!=rows.end()); REQUIRE_FALSE(receipt->error); REQUIRE(receipt->done_record_valid);
      expected=std::move(native);
    }
    SECTION("real retained staged File IO preserves original until durable publication and keeps native layout readable until actual view retirement") {
      const auto native=NativeTestRepack(plan->files.at(kDashboardID).intended);
      const auto stage=NativeProfileSavePublicationTestAccess::CreateStage(owner,kDashboardID,error);
      REQUIRE(stage); REQUIRE_FALSE(error);
      const auto artifact=NativeProfileSavePublicationTestAccess::StagePath(stage);
      REQUIRE(std::filesystem::is_regular_file(artifact)); REQUIRE(CreationRead(artifact)==before);
      vfs::File* file=nullptr;
      REQUIRE(NativeProfileSavePublicationTestAccess::OpenStage(stage,writer.model->storage()->device(),&file)==X_STATUS_SUCCESS);
      REQUIRE(file); struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} close{file};
      REQUIRE(file->SetLength(native.size())==X_STATUS_SUCCESS);
      size_t written=0; REQUIRE(file->WriteSync(native,0,&written)==X_STATUS_SUCCESS); REQUIRE(written==native.size());
      REQUIRE(file->Flush()==X_STATUS_SUCCESS); // genuine protected physical stage fsync
      REQUIRE(CreationRead(artifact)==native); REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      vfs::File::Information info{};
      REQUIRE(file->QueryInformation(&info)==X_STATUS_SUCCESS); REQUIRE(info.end_of_file==native.size());
      REQUIRE(file->SetLength(GpdSnapshotStore::kMaximumBytes+1)==X_STATUS_INVALID_PARAMETER);
      REQUIRE(file->QueryInformation(&info)==X_STATUS_SUCCESS); REQUIRE(info.end_of_file==native.size());
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealStage(owner,kDashboardID,native));
      REQUIRE(file->WriteSync(std::array<uint8_t,1>{0},0,&written)!=X_STATUS_SUCCESS); REQUIRE(written==0);
      const auto committed=NativeProfileSavePublicationTestAccess::Publish(owner);
      REQUIRE_FALSE(committed.error); REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==native);
      // Own model-version advance is NOT a reason to fake GetFileSize failure.
      REQUIRE(file->QueryInformation(&info)==X_STATUS_SUCCESS); REQUIRE(info.end_of_file==native.size());
      std::vector<uint8_t> read(native.size());size_t count=0;
      REQUIRE(file->ReadSync(read,0,&count)==X_STATUS_SUCCESS); REQUIRE(count==native.size()); REQUIRE(read==native);
      NativeProfileSavePublicationTestAccess::RetireStage(stage); // fixture cut; no native DB claim
      std::array<uint8_t,sizeof(info)> prior{};std::memcpy(prior.data(),&info,sizeof(info));
      REQUIRE(file->QueryInformation(&info)==X_STATUS_INVALID_HANDLE);
      REQUIRE(std::memcmp(prior.data(),&info,sizeof(info))==0);
      file->Destroy();close.file=nullptr;expected=native;
    }
    SECTION("real VFS exact-leaf service consumes issued context once and rejects unknown or absent authority before disposition") {
      const auto native=NativeTestRepack(plan->files.at(kDashboardID).intended);
      const auto stage=NativeProfileSavePublicationTestAccess::CreateStage(owner,kDashboardID,error);
      REQUIRE(stage);REQUIRE_FALSE(error);
      const auto service=NativeProfileSavePublicationTestAccess::Service(stage,error);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      vfs::File* file=reinterpret_cast<vfs::File*>(uintptr_t(1));vfs::FileAction action=vfs::FileAction::kCreated;
      const auto open=[&](const vfs::FileOpenContext& context,vfs::FileDisposition disposition){
        return fs->OpenFile(root,"FFFE07D1.gpd",disposition,
            vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&file,&action,context);
      };
      REQUIRE(open({},vfs::FileDisposition::kOverwrite)==X_STATUS_ACCESS_DENIED);
      REQUIRE(file==nullptr);REQUIRE(action==vfs::FileAction::kDoesNotExist);
      class UnknownAuthority final:public vfs::ManagedFileOpenAuthority {};
      vfs::FileOpenContext unknown{device,std::make_shared<UnknownAuthority>()};
      REQUIRE(open(unknown,vfs::FileDisposition::kOverwrite)==X_STATUS_ACCESS_DENIED);
      REQUIRE(file==nullptr);REQUIRE(action==vfs::FileAction::kDoesNotExist);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE(CreationRead(NativeProfileSavePublicationTestAccess::StagePath(stage))==before);
      const auto context=NativeProfileSavePublicationTestAccess::OpenContext(service,owner,device,error);
      REQUIRE(context.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(open(context,vfs::FileDisposition::kOpen)==X_STATUS_SUCCESS);REQUIRE(file);
      struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} close{file};
      REQUIRE(file->SetLength(native.size())==X_STATUS_SUCCESS);size_t written=0;
      REQUIRE(file->WriteSync(native,0,&written)==X_STATUS_SUCCESS);REQUIRE(written==native.size());
      REQUIRE(file->Flush()==X_STATUS_SUCCESS);
      auto* retained=file;
      REQUIRE(open(context,vfs::FileDisposition::kOverwrite)==X_STATUS_ACCESS_DENIED);
      REQUIRE(file==nullptr);REQUIRE(action==vfs::FileAction::kDoesNotExist);
      REQUIRE(CreationRead(NativeProfileSavePublicationTestAccess::StagePath(stage))==native);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealStage(owner,kDashboardID,native));
      const auto committed=NativeProfileSavePublicationTestAccess::Publish(owner);
      REQUIRE_FALSE(committed.error);REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
      vfs::File::Information actual{};REQUIRE(retained->QueryInformation(&actual)==X_STATUS_SUCCESS);
      REQUIRE(actual.end_of_file==native.size());NativeProfileSavePublicationTestAccess::RetireStage(stage);
      retained->Destroy();close.file=nullptr;expected=native;
    }
    SECTION("real managed open retires only its host duty and File keeps a unique immutable receipt without request ownership") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      const auto service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error);
      REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto read=writer.model->CaptureRead(error);REQUIRE(read);REQUIRE_FALSE(error);
      struct Duty {
        std::atomic<unsigned>& retired;
        explicit Duty(std::atomic<unsigned>& value):retired(value){}
        ~Duty(){++retired;}
      };
      std::atomic<unsigned> retired{0};auto duty=std::make_shared<Duty>(retired);std::weak_ptr<Duty> weak=duty;
      auto issued=NativeProfileSavePublicationTestAccess::ReadContextWithDuty(service,*read,device,duty,error);
      REQUIRE(issued.first.managed_authority);REQUIRE_FALSE(error);duty.reset();
      REQUIRE_FALSE(weak.expired());REQUIRE(retired.load()==0);
      vfs::File* file=nullptr;vfs::FileAction action{};
      struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} close{file};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&file,&action,issued.first)==X_STATUS_SUCCESS);
      REQUIRE(file);REQUIRE(action==vfs::FileAction::kOpened);
      // The source/VFS host duty ended before return, even though its Context
      // and real File remain alive. No request/roster lease is pinned by File.
      CHECK(weak.expired());CHECK(retired.load()==1);
      const auto view=NativeProfileSavePublicationTestAccess::MatchReadReceipt(*file,issued.second,error);
      REQUIRE(view);REQUIRE_FALSE(error);
      CHECK(view==NativeProfileSavePublicationTestAccess::ReadView(*file));
      const auto different=NativeProfileSavePublicationTestAccess::NewReadReceipt();
      CHECK_FALSE(NativeProfileSavePublicationTestAccess::MatchReadReceipt(*file,different,error));
      CHECK(error==std::errc::operation_canceled);
      vfs::File* refused=nullptr;Close refused_close{refused};
      CHECK(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&refused,&action,issued.first)==X_STATUS_ACCESS_DENIED);
      CHECK_FALSE(refused);CHECK(action==vfs::FileAction::kDoesNotExist);
      std::vector<uint8_t> bytes(before.size());size_t count=0;
      REQUIRE(file->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);CHECK(count==before.size());CHECK(bytes==before);
      std::atomic<unsigned> dropped{0};
      {
        auto unused=std::make_shared<Duty>(dropped);
        auto pending=NativeProfileSavePublicationTestAccess::ReadContextWithDuty(service,*read,device,unused,error);
        REQUIRE(pending.first.managed_authority);REQUIRE_FALSE(error);unused.reset();
        std::function<void()> owned=[context=std::move(pending.first)]{(void)context;};
        CHECK_FALSE(pending.first.managed_authority);CHECK(dropped.load()==0);
        owned={};CHECK(dropped.load()==1); // Real owned closure destruction, no claim of a native queue run.
      }
      // A denied mutation can leave original native RAM dirty despite unchanged
      // disk bytes. This exact before-view receipt is no longer a clean-read
      // witness after that actual attempt; another open does not cure THIS File.
      size_t denied=99;
      CHECK(file->WriteSync(std::array<uint8_t,1>{0xFF},0,&denied)==X_STATUS_ACCESS_DENIED);CHECK(denied==0);
      CHECK_FALSE(NativeProfileSavePublicationTestAccess::MatchReadReceipt(*file,issued.second,error));
      CHECK(error==std::errc::state_not_recoverable);
      CHECK(CreationRead(package/"FFFE07D1.gpd")==before);
    }
    SECTION("one source provider keeps an owned committed read tuple through exact staged publication and serves a distinct fresh read afterward") {
      const auto native=NativeTestRepack(plan->files.at(kDashboardID).intended);
      const auto stage=NativeProfileSavePublicationTestAccess::CreateStage(owner,kDashboardID,error);
      REQUIRE(stage);REQUIRE_FALSE(error);
      const auto service=NativeProfileSavePublicationTestAccess::Service(stage,error);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto old_read=writer.model->CaptureRead(error);REQUIRE(old_read);REQUIRE_FALSE(error);
      const auto old_context=NativeProfileSavePublicationTestAccess::ReadContext(service,*old_read,device,error);
      REQUIRE(old_context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* old_file=nullptr;vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&old_file,&action,old_context)==X_STATUS_SUCCESS);
      REQUIRE(old_file);struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} old_close{old_file};
      size_t written=9;REQUIRE(old_file->WriteSync(std::array<uint8_t,1>{0},0,&written)==X_STATUS_ACCESS_DENIED);
      REQUIRE(written==0);REQUIRE(old_file->SetLength(0)==X_STATUS_ACCESS_DENIED);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      vfs::File* write_file=nullptr;
      const auto write_context=NativeProfileSavePublicationTestAccess::OpenContext(service,owner,device,error);
      REQUIRE(write_context.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&write_file,&action,write_context)==X_STATUS_SUCCESS);
      REQUIRE(write_file);Close write_close{write_file};
      REQUIRE(write_file->SetLength(native.size())==X_STATUS_SUCCESS);
      REQUIRE(write_file->WriteSync(native,0,&written)==X_STATUS_SUCCESS);REQUIRE(written==native.size());
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealStage(owner,kDashboardID,native));
      const auto committed=NativeProfileSavePublicationTestAccess::Publish(owner);
      // A still-owned immutable read has no raw descriptor lease to conflict
      // with the genuine journal; foreign raw handles in another section DO.
      REQUIRE_FALSE(committed.error);REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
      std::vector<uint8_t> old_bytes(before.size());size_t count=0;
      REQUIRE(old_file->ReadSync(old_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());REQUIRE(old_bytes==before);
      vfs::File::Information old_info{};REQUIRE(old_file->QueryInformation(&old_info)==X_STATUS_SUCCESS);
      REQUIRE(old_info.end_of_file==before.size());
      const auto fresh=writer.model->CaptureRead(error);REQUIRE(fresh);REQUIRE_FALSE(error);
      const auto fresh_context=NativeProfileSavePublicationTestAccess::ReadContext(service,*fresh,device,error);
      REQUIRE(fresh_context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* fresh_file=nullptr;
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&fresh_file,&action,fresh_context)==X_STATUS_SUCCESS);
      REQUIRE(fresh_file);Close fresh_close{fresh_file};
      std::vector<uint8_t> new_bytes(native.size());
      REQUIRE(fresh_file->ReadSync(new_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==native.size());REQUIRE(new_bytes==native);
      const auto stale=NativeProfileSavePublicationTestAccess::ReadContext(service,*old_read,device,error);
      REQUIRE_FALSE(stale.managed_authority);REQUIRE(error==std::errc::operation_canceled);
      NativeProfileSavePublicationTestAccess::RetireRead(*old_file); // actual-DB-quiescence fixture only
      std::array<uint8_t,sizeof(old_info)> prior{};std::memcpy(prior.data(),&old_info,sizeof(old_info));
      REQUIRE(old_file->QueryInformation(&old_info)==X_STATUS_INVALID_HANDLE);
      REQUIRE(std::memcmp(prior.data(),&old_info,sizeof(old_info))==0);
      NativeProfileSavePublicationTestAccess::RetireStage(stage);
      expected=native;
    }
    SECTION("one physical leaf policy accepts a separately issued new model read while retaining the genuine old owned tuple") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      const auto family_closed=std::make_shared<std::atomic<bool>>(false);
      const auto service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error,family_closed);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto old_read=writer.model->CaptureRead(error);REQUIRE(old_read);REQUIRE_FALSE(error);
      const auto old_context=NativeProfileSavePublicationTestAccess::ReadContext(service,*old_read,device,error);
      REQUIRE(old_context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* old_file=nullptr;vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&old_file,&action,old_context)==X_STATUS_SUCCESS);
      REQUIRE(old_file);struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} old_close{old_file};
      // This is the actual model/storage service component, not a claim that
      // the private test factory authenticates a native getter or selected row.
      const auto loads=writer.model->CaptureLoads(error);REQUIRE(loads);REQUIRE_FALSE(error);
      auto next=ProfileGpdModelAuthority::Create(xuid,1,writer.model->sign_in_generation()+1,writer.model->storage(),error);
      REQUIRE(next);REQUIRE_FALSE(error);REQUIRE_FALSE(next->InstallLoaded(*loads));
      const auto next_read=next->CaptureRead(error);REQUIRE(next_read);REQUIRE_FALSE(error);
      const auto next_context=NativeProfileSavePublicationTestAccess::ReadContext(service,*next_read,device,error);
      REQUIRE(next_context.managed_authority);REQUIRE_FALSE(error);
      // The registry still has precisely the live old source policy. A second
      // model cannot replace it, but its independent current read may use it.
      const auto second=NativeProfileSavePublicationTestAccess::SourceService(next,error);REQUIRE(second);REQUIRE_FALSE(error);
      REQUIRE_FALSE(fs->RegisterManagedFile(second));REQUIRE_FALSE(service->retiring());
      vfs::File* next_file=nullptr;
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&next_file,&action,next_context)==X_STATUS_SUCCESS);
      REQUIRE(next_file);Close next_close{next_file};
      std::vector<uint8_t> old_bytes(before.size()),next_bytes(before.size());size_t count=0;
      REQUIRE(old_file->ReadSync(old_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());REQUIRE(old_bytes==before);
      REQUIRE(next_file->ReadSync(next_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());REQUIRE(next_bytes==before);
      // Retiring this separate model withdraws only its new model admission. The
      // source policy and the independent new-model File remain usable.
      next->Retire();
      const auto retired=NativeProfileSavePublicationTestAccess::ReadContext(service,*next_read,device,error);
      REQUIRE_FALSE(retired.managed_authority);REQUIRE(error==std::errc::operation_canceled);
      REQUIRE_FALSE(service->retiring());
      REQUIRE(next_file->ReadSync(next_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(next_bytes==before);
      const auto& held=writer.model->storage();
      auto changed_generation=ProfileStorageLease::Capture(held->xuid(),held->source_id(),held->device_id(),
          held->source_generation()+1,held->device(),held->source(),held->view(),error,
          held->managed_identity(),held->guest_generation());
      REQUIRE(changed_generation);REQUIRE_FALSE(error);
      auto changed=ProfileGpdModelAuthority::Create(xuid,1,writer.model->sign_in_generation()+2,changed_generation,error);
      REQUIRE(changed);REQUIRE_FALSE(error);REQUIRE_FALSE(changed->InstallLoaded(*loads));
      const auto changed_read=changed->CaptureRead(error);REQUIRE(changed_read);REQUIRE_FALSE(error);
      const auto foreign_generation=NativeProfileSavePublicationTestAccess::ReadContext(service,*changed_read,device,error);
      REQUIRE_FALSE(foreign_generation.managed_authority);REQUIRE(error==std::errc::operation_canceled);
      SECTION("a queued metadata read authority cannot follow an identical-byte foreign physical file") {
        const auto queued_read=NativeProfileSavePublicationTestAccess::ReadContext(service,*old_read,device,error);
        REQUIRE(queued_read.managed_authority);REQUIRE_FALSE(error);
        const auto target=package/"FFFE07D1.gpd";
        const auto backup=package/"queued-read-original.fixture";
        struct Restore {
          std::filesystem::path target,backup;bool active=false;
          ~Restore(){if(active){std::error_code ignored;std::filesystem::remove(target,ignored);
            std::filesystem::rename(backup,target,ignored);}}
        } restore{target,backup};
        // Real external namespace replacement in this disposable fixture.
        // Equal bytes deliberately exclude a content-only comparison: the
        // current physical identity still cannot satisfy the old token.
        std::filesystem::rename(target,backup,error);REQUIRE_FALSE(error);restore.active=true;
        {std::ofstream out(target,std::ios::binary|std::ios::trunc);REQUIRE(out);
          out.write(reinterpret_cast<const char*>(before.data()),std::streamsize(before.size()));
          REQUIRE(out);}
        REQUIRE(CreationRead(target)==before);REQUIRE(CreationRead(backup)==before);
        vfs::File* refused=reinterpret_cast<vfs::File*>(uintptr_t(1));
        struct CloseRefused {
          vfs::File*& file;
          ~CloseRefused(){if(file && file!=reinterpret_cast<vfs::File*>(uintptr_t(1)))file->Destroy();}
        } close_refused{refused}; // before the call/assertion, including unexpected success
        vfs::FileAction refused_action=vfs::FileAction::kCreated;
        REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead,false,true,&refused,&refused_action,queued_read)!=X_STATUS_SUCCESS);
        REQUIRE(refused==nullptr);REQUIRE(refused_action==vfs::FileAction::kDoesNotExist);
        REQUIRE(CreationRead(target)==before);REQUIRE(CreationRead(backup)==before);
        std::fill(old_bytes.begin(),old_bytes.end(),0);
        REQUIRE(old_file->ReadSync(old_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(old_bytes==before);
        REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size());REQUIRE_FALSE(error);
        // Restore owns cleanup even if any assertion throws. It does not
        // manufacture a native DB close or make a stale new open succeed.
      }
      const auto queued=NativeProfileSavePublicationTestAccess::ReadContext(service,*old_read,device,error);
      REQUIRE(queued.managed_authority);REQUIRE_FALSE(error);
      family_closed->store(true,std::memory_order_release);REQUIRE(service->retiring());
      vfs::File* rejected=nullptr;
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&rejected,&action,queued)==X_STATUS_ACCESS_DENIED);
      REQUIRE_FALSE(rejected);
      // Admission closure is not a fabricated native File close or read-view
      // retirement. This already-owned source remains exactly readable.
      std::fill(old_bytes.begin(),old_bytes.end(),0);
      REQUIRE(old_file->ReadSync(old_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(old_bytes==before);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size());REQUIRE_FALSE(error);
    }
    SECTION("one actual issued cached File upgrades only its exact before layout while the XFile handle entry position and backend pointer remain stable") {
      const auto native=NativeTestRepack(plan->files.at(kDashboardID).intended);
      const auto stage=NativeProfileSavePublicationTestAccess::CreateStage(owner,kDashboardID,error);
      REQUIRE(stage);REQUIRE_FALSE(error);
      const auto service=NativeProfileSavePublicationTestAccess::Service(stage,error);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto read=writer.model->CaptureRead(error);REQUIRE(read);REQUIRE_FALSE(error);
      const auto read_context=NativeProfileSavePublicationTestAccess::ReadContext(service,*read,device,error);
      REQUIRE(read_context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* opened=nullptr;vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action,read_context)==X_STATUS_SUCCESS);
      REQUIRE(opened);
      auto file=object_ref<XFile>(new XFile(runtime.emulator->kernel_state(),opened,true,false,true));
      file->set_position(37);const auto* original_entry=file->entry();const auto original_path=file->path();
      const auto view=NativeProfileSavePublicationTestAccess::ReadView(*opened);REQUIRE(view);
      // A read capability cannot be promoted by this consumer or by RW open rights.
      REQUIRE(NativeProfileSavePublicationTestAccess::Upgrade(service,*opened,view,read_context,before)==X_STATUS_ACCESS_DENIED);
      const auto write_context=NativeProfileSavePublicationTestAccess::OpenContext(service,owner,device,error);
      REQUIRE(write_context.managed_authority);REQUIRE_FALSE(error);
      auto bad_header=before;bad_header[4]^=1;
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Upgrade(service,*opened,view,write_context,bad_header)));
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*opened)==view);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      // A real open stage File is a conflicting owner even before any IO.
      vfs::File* foreign=nullptr;REQUIRE(NativeProfileSavePublicationTestAccess::OpenStage(stage,device,&foreign)==X_STATUS_SUCCESS);
      REQUIRE(foreign);struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} foreign_close{foreign};
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Upgrade(service,*opened,view,write_context,before)));
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*opened)==view);
      foreign->Destroy();foreign_close.file=nullptr;
      // A REAL running PostIo request also excludes this cut, independent of
      // the backend's own mutex. All callback state is owned, never stack-borrowed.
      struct PostedGate {threading::Fence entered,release,done;std::atomic<bool> rejected{false};};
      const auto gate=std::make_shared<PostedGate>();
      struct ReleasePosted {std::shared_ptr<PostedGate> gate;~ReleasePosted(){gate->release.Signal();}} release_posted{gate};
      file->PostIo(nullptr,[gate](bool canceled){
        gate->entered.Signal();if(!canceled) {GuestScheduler::WaitOnFence(gate->release);}gate->done.Signal();
      },[gate]{gate->rejected.store(true);gate->done.Signal();},
        [gate]{gate->rejected.store(true);gate->done.Signal();});
      bool started=false;const auto start_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      do {started=gate->entered.TryWait();if(!started) {std::this_thread::yield();}}
      while(!started && !gate->rejected.load() && std::chrono::steady_clock::now()<start_deadline);
      REQUIRE(started);REQUIRE_FALSE(gate->rejected.load());
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::FileCut(service,file,view,write_context,owner,before)));
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*opened)==view);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      gate->release.Signal();
      bool drained=false;const auto drain_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      do {drained=!file->HasPostedIo(nullptr);if(!drained) {std::this_thread::yield();}}
      while(!drained && std::chrono::steady_clock::now()<drain_deadline);
      REQUIRE(drained);
      // This real XFile queue/file/backend cut is still not native DB/getter
      // authentication; the concrete original native owner remains mandatory.
      REQUIRE(NativeProfileSavePublicationTestAccess::FileCut(service,file,view,write_context,owner,before)==X_STATUS_SUCCESS);
      REQUIRE(file->file()==opened);REQUIRE(file->entry()==original_entry);REQUIRE(file->path()==original_path);
      REQUIRE(file->position()==37);REQUIRE(file->is_synchronous());
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ReadView(*opened));
      REQUIRE(NativeProfileSavePublicationTestAccess::Upgrade(service,*opened,view,write_context,before)==X_STATUS_ACCESS_DENIED);
      foreign=nullptr;REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::OpenStage(stage,device,&foreign)));REQUIRE_FALSE(foreign);
      size_t written=0;REQUIRE(opened->SetLength(native.size())==X_STATUS_SUCCESS);
      REQUIRE(opened->WriteSync(native,0,&written)==X_STATUS_SUCCESS);REQUIRE(written==native.size());
      REQUIRE(opened->Flush()==X_STATUS_SUCCESS);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealStage(owner,kDashboardID,native));
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Promote(service,*opened,owner,native)));
      const auto committed=NativeProfileSavePublicationTestAccess::Publish(owner);REQUIRE_FALSE(committed.error);
      REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
      REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
      REQUIRE(writer.Submit(command).error==std::errc::device_or_resource_busy);
      REQUIRE_FALSE(writer.model->CaptureCommittedFileRead(error));REQUIRE(error==std::errc::device_or_resource_busy);
      vfs::File::Information actual{};REQUIRE(opened->QueryInformation(&actual)==X_STATUS_SUCCESS);REQUIRE(actual.end_of_file==native.size());
      std::vector<uint8_t> bytes(native.size());size_t count=0;
      REQUIRE(opened->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==native.size());REQUIRE(bytes==native);
      REQUIRE(file->file()==opened);REQUIRE(file->position()==37);
      auto wrong=native;wrong[4]^=1;
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Promote(service,*opened,owner,wrong)));
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ReadView(*opened));
      // A genuine held model release guard forces truthful pre-swap refusal.
      // Its un-applied destruction retains the SAME pending owner; retry can
      // still join the actual cached File rather than leaking a readiness debt.
      auto held_checkpoint=NativeProfileSavePublicationTestAccess::HoldCacheCheckpoint(owner,kDashboardID,stage,error);
      REQUIRE(held_checkpoint);REQUIRE_FALSE(error);
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::FileCut(service,file,{}, {},owner,native,true)));
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ReadView(*opened));
      held_checkpoint.reset();REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
      // The SAME real committed transaction plus matching actual native cache
      // layout permits read promotion; it is not a new writer or fake EOF.
      REQUIRE(NativeProfileSavePublicationTestAccess::FileCut(service,file,{}, {},owner,native,true)==X_STATUS_SUCCESS);
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*opened));
      REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
      REQUIRE(writer.model->CaptureCommittedFileRead(error));REQUIRE_FALSE(error);
      NativeProfileSavePublicationTestAccess::RetireStage(stage); // already-consumed stage is independent now
      REQUIRE(opened->QueryInformation(&actual)==X_STATUS_SUCCESS);REQUIRE(actual.end_of_file==native.size());
      std::fill(bytes.begin(),bytes.end(),0);REQUIRE(opened->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(bytes==native);
      REQUIRE(opened->WriteSync(std::array<uint8_t,1>{0},0,&written)==X_STATUS_ACCESS_DENIED);REQUIRE(written==0);
      REQUIRE(file->file()==opened);REQUIRE(file->entry()==original_entry);REQUIRE(file->position()==37);
      NativeProfileSavePublicationTestAccess::RetireRead(*opened); // actual-DB-use fixture only
      const auto prior=actual;REQUIRE(opened->QueryInformation(&actual)==X_STATUS_INVALID_HANDLE);
      REQUIRE(std::memcmp(&prior,&actual,sizeof(actual))==0);expected=native;
    }
    SECTION("a refused native File write keeps that cached generation ineligible until actual File close and a distinct fresh open") {
      const auto stage=NativeProfileSavePublicationTestAccess::CreateStage(owner,kDashboardID,error);REQUIRE(stage);REQUIRE_FALSE(error);
      const auto service=NativeProfileSavePublicationTestAccess::Service(stage,error);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto read=writer.model->CaptureRead(error);REQUIRE(read);REQUIRE_FALSE(error);
      auto context=NativeProfileSavePublicationTestAccess::ReadContext(service,*read,device,error);
      REQUIRE(context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* cached=nullptr;vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&cached,&action,context)==X_STATUS_SUCCESS);
      REQUIRE(cached);struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} close{cached};
      const auto old_view=NativeProfileSavePublicationTestAccess::ReadView(*cached);REQUIRE(old_view);
      size_t written=11;REQUIRE(cached->WriteSync(before,0,&written)==X_STATUS_ACCESS_DENIED);REQUIRE(written==0);
      const auto writer_context=NativeProfileSavePublicationTestAccess::OpenContext(service,owner,device,error);
      REQUIRE(writer_context.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Upgrade(service,*cached,old_view,writer_context,before)));
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*cached)==old_view);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      cached->Destroy();close.file=nullptr; // actual HOST File close; not native cache acceptance
      context=NativeProfileSavePublicationTestAccess::ReadContext(service,*read,device,error);
      REQUIRE(context.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&cached,&action,context)==X_STATUS_SUCCESS);
      REQUIRE(cached);close.file=cached;
      const auto fresh_view=NativeProfileSavePublicationTestAccess::ReadView(*cached);REQUIRE(fresh_view);REQUIRE(fresh_view!=old_view);
      REQUIRE(NativeProfileSavePublicationTestAccess::Upgrade(service,*cached,old_view,writer_context,before)==X_STATUS_ACCESS_DENIED);
      REQUIRE(NativeProfileSavePublicationTestAccess::Upgrade(service,*cached,fresh_view,writer_context,before)==X_STATUS_SUCCESS);
      NativeProfileSavePublicationTestAccess::RetireStage(stage);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    }
    SECTION("an attempted stage mutation cannot be attached behind an already cached before layout") {
      const auto stage=NativeProfileSavePublicationTestAccess::CreateStage(owner,kDashboardID,error);REQUIRE(stage);REQUIRE_FALSE(error);
      const auto service=NativeProfileSavePublicationTestAccess::Service(stage,error);REQUIRE(service);REQUIRE_FALSE(error);
      auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
      const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
      const auto read=writer.model->CaptureRead(error);REQUIRE(read);REQUIRE_FALSE(error);
      const auto read_context=NativeProfileSavePublicationTestAccess::ReadContext(service,*read,device,error);
      REQUIRE(read_context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* cached=nullptr;vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&cached,&action,read_context)==X_STATUS_SUCCESS);
      REQUIRE(cached);struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} close{cached};
      const auto view=NativeProfileSavePublicationTestAccess::ReadView(*cached);REQUIRE(view);
      vfs::File* other=nullptr;REQUIRE(NativeProfileSavePublicationTestAccess::OpenStage(stage,device,&other)==X_STATUS_SUCCESS);
      REQUIRE(other);Close other_close{other};size_t count=0;
      // Even identical bytes count as a real mutation attempt, rather than guessing clean.
      REQUIRE(other->WriteSync(before,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==before.size());
      other->Destroy();other_close.file=nullptr;
      const auto context=NativeProfileSavePublicationTestAccess::OpenContext(service,owner,device,error);
      REQUIRE(context.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Upgrade(service,*cached,view,context,before)));
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*cached)==view);
      std::vector<uint8_t> bytes(before.size());REQUIRE(cached->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(bytes==before);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      NativeProfileSavePublicationTestAccess::RetireStage(stage);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size());REQUIRE_FALSE(error);
    }
    SECTION("native GPD sealing cannot silently stand in for an unjoined actual native PNG pair") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      ProfileGpdMutation pictures;pictures.kind=ProfileGpdMutationKind::kSetPictures;
      pictures.title_id=kDashboardID;pictures.request_id=writer.model->AllocateRequestId(error);REQUIRE_FALSE(error);
      const auto pair=PrepareHostProfilePicturePair(xe::kernel::test::SyntheticProfilePicture64(),error);
      REQUIRE(pair);REQUIRE_FALSE(error);
      pictures.picture_changes={{filesystem::ProfileArtifactKind::kPersonal64,false,pair->large},
                                {filesystem::ProfileArtifactKind::kPersonal32,false,pair->small}};
      auto pair_owner=NativeProfileSavePublicationTestAccess::Prepare(source,pictures,error);REQUIRE(pair_owner);REQUIRE_FALSE(error);
      const auto pair_plan=NativeProfileSavePublicationTestAccess::Plan(pair_owner,error);REQUIRE(pair_plan);REQUIRE_FALSE(error);
      REQUIRE(pair_plan->pictures.size()==2);
      const auto& file=pair_plan->files.at(kDashboardID);
      const auto bytes=file.action==ProfileGpdFileAction::kUnchanged ?
          std::vector<uint8_t>(file.before.token->bytes().begin(),file.before.token->bytes().end()):file.intended;
      REQUIRE(NativeProfileSavePublicationTestAccess::Seal(pair_owner,kDashboardID,bytes)==std::errc::operation_not_supported);
      REQUIRE(NativeProfileSavePublicationTestAccess::Publish(pair_owner).error==std::errc::device_or_resource_busy);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));REQUIRE_FALSE(std::filesystem::exists(package/"pp_32.png"));
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size());REQUIRE_FALSE(error);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(pair_owner));
    }
    SECTION("real closed native PNG stages join the same exact GPD key transaction or retain all original bytes on malformed output") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      ProfileGpdMutation pictures;pictures.kind=ProfileGpdMutationKind::kSetPictures;
      pictures.title_id=kDashboardID;pictures.request_id=writer.model->AllocateRequestId(error);REQUIRE_FALSE(error);
      const auto pair=PrepareHostProfilePicturePair(xe::kernel::test::SyntheticProfilePicture64(),error);REQUIRE(pair);REQUIRE_FALSE(error);
      const std::u16string key=u"fffe08540001000200010003";
      pictures.settings.emplace_back(UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE,key);
      pictures.picture_changes={{filesystem::ProfileArtifactKind::kPersonal64,false,pair->large},
                                {filesystem::ProfileArtifactKind::kPersonal32,false,pair->small}};
      auto pair_owner=NativeProfileSavePublicationTestAccess::Prepare(source,pictures,error);REQUIRE(pair_owner);REQUIRE_FALSE(error);
      const auto pair_plan=NativeProfileSavePublicationTestAccess::Plan(pair_owner,error);REQUIRE(pair_plan);REQUIRE_FALSE(error);
      const auto large=NativeProfileSavePublicationTestAccess::CreatePictureStage(pair_owner,filesystem::ProfileArtifactKind::kPersonal64,error);
      REQUIRE(large);REQUIRE_FALSE(error);
      const auto small=NativeProfileSavePublicationTestAccess::CreatePictureStage(pair_owner,filesystem::ProfileArtifactKind::kPersonal32,error);
      REQUIRE(small);REQUIRE_FALSE(error);
      const auto device=writer.model->storage()->device();auto* fs=runtime.emulator->file_system();
      const auto large_service=NativeProfileSavePublicationTestAccess::PictureService(writer.model,filesystem::ProfileArtifactKind::kPersonal64,error);
      REQUIRE(large_service);REQUIRE_FALSE(error);REQUIRE(fs->RegisterManagedFile(large_service));
      const auto small_service=NativeProfileSavePublicationTestAccess::PictureService(writer.model,filesystem::ProfileArtifactKind::kPersonal32,error);
      REQUIRE(small_service);REQUIRE_FALSE(error);REQUIRE(fs->RegisterManagedFile(small_service));
      auto* root=device->ResolvePath("");REQUIRE(root);vfs::File* file=nullptr;vfs::FileAction action{};
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOverwriteIf,
          vfs::FileAccess::kGenericWrite,false,true,&file,&action)==X_STATUS_ACCESS_DENIED);
      REQUIRE_FALSE(file);REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));
      const auto committed_read=writer.model->CaptureRead(error);REQUIRE(committed_read);REQUIRE_FALSE(error);
      const auto absent_read=NativeProfileSavePublicationTestAccess::PictureReadContext(large_service,*committed_read,device,"pp_64.png",error);
      REQUIRE(absent_read.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&file,&action,absent_read)==X_STATUS_OBJECT_NAME_NOT_FOUND);
      REQUIRE_FALSE(file);REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));
      const auto avatar_service=NativeProfileSavePublicationTestAccess::PictureService(writer.model,filesystem::ProfileArtifactKind::kAvatar64,error);
      REQUIRE(avatar_service);REQUIRE_FALSE(error);REQUIRE(fs->RegisterManagedFile(avatar_service));
      const auto avatar_context=NativeProfileSavePublicationTestAccess::PictureReadContext(avatar_service,*committed_read,device,"avtr_64.png",error);
      REQUIRE(avatar_context.managed_authority);REQUIRE_FALSE(error);
      vfs::File* avatar_file=nullptr;
      REQUIRE(fs->OpenFile(root,"avtr_64.png",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&avatar_file,&action,avatar_context)==X_STATUS_SUCCESS);
      REQUIRE(avatar_file);struct SourceClose {vfs::File* file;~SourceClose(){if(file)file->Destroy();}} avatar_close{avatar_file};
      const auto avatar_bytes=CreationRead(package/"avtr_64.png");
      std::vector<uint8_t> avatar_output(avatar_bytes.size());size_t avatar_count=0;
      REQUIRE(avatar_file->ReadSync(avatar_output,0,&avatar_count)==X_STATUS_SUCCESS);REQUIRE(avatar_count==avatar_bytes.size());REQUIRE(avatar_output==avatar_bytes);
      REQUIRE(avatar_file->WriteSync(std::array<uint8_t,1>{0},0,&avatar_count)==X_STATUS_ACCESS_DENIED);REQUIRE(avatar_count==0);
      REQUIRE(avatar_file->SetLength(0)==X_STATUS_ACCESS_DENIED);REQUIRE(CreationRead(package/"avtr_64.png")==avatar_bytes);
      const auto large_context=NativeProfileSavePublicationTestAccess::PictureContext(large_service,pair_owner,device,"pp_64.png",error);
      REQUIRE(large_context.managed_authority);REQUIRE_FALSE(error);
      const auto before_native_create=Clock::QueryGuestSystemTime();
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOverwriteIf,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&file,&action,large_context)==X_STATUS_SUCCESS);
      REQUIRE(file);REQUIRE(action==vfs::FileAction::kCreated);
      struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} close{file};
      const auto after_native_create=Clock::QueryGuestSystemTime();
      vfs::File::Information created_info{};REQUIRE(file->QueryInformation(&created_info)==X_STATUS_SUCCESS);
      REQUIRE(created_info.create_timestamp>=before_native_create);
      REQUIRE(created_info.create_timestamp<=after_native_create);
      const auto genuine_new_birth=created_info.create_timestamp;
      REQUIRE(file->SetLength(pair->large.size())==X_STATUS_SUCCESS);size_t count=0;
      REQUIRE(file->WriteSync(pair->large,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==pair->large.size());
      REQUIRE(file->Flush()==X_STATUS_SUCCESS);
      const auto created_checkpoint=NativeProfileSavePublicationTestAccess::PictureOutputCheckpoint(
          *file,pair_owner,filesystem::ProfileArtifactKind::kPersonal64,error);
      REQUIRE(created_checkpoint);REQUIRE_FALSE(error);REQUIRE_FALSE(created_checkpoint->before()->exists());
      REQUIRE(created_checkpoint->new_creation_filetime().has_value());
      REQUIRE(*created_checkpoint->new_creation_filetime()==genuine_new_birth);
      REQUIRE(created_checkpoint->CheckCurrent(error));REQUIRE_FALSE(error);
      vfs::File* reused=nullptr;
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOverwriteIf,
          vfs::FileAccess::kGenericWrite,false,true,&reused,&action,large_context)==X_STATUS_ACCESS_DENIED);REQUIRE_FALSE(reused);
      std::vector<uint8_t> retained(pair->large.size());size_t retained_count=0;
      REQUIRE(file->ReadSync(retained,0,&retained_count)==X_STATUS_SUCCESS);REQUIRE(retained_count==retained.size());REQUIRE(retained==pair->large);
      vfs::File::Information info{};REQUIRE(file->QueryInformation(&info)==X_STATUS_SUCCESS);REQUIRE(info.end_of_file==pair->large.size());
      // An actual still-owned File can issue later IO, so it is not a seal cut.
      REQUIRE(NativeProfileSavePublicationTestAccess::SealPictures(pair_owner)==std::errc::device_or_resource_busy);
      file->Destroy();close.file=nullptr;
      // The transaction's new PNG now EXISTS for its genuine later native
      // CopyFile source open, although the immutable original token is absent.
      const auto read_back=NativeProfileSavePublicationTestAccess::PictureContext(large_service,pair_owner,device,"pp_64.png",error);
      REQUIRE(read_back.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead,false,true,&file,&action,read_back)==X_STATUS_SUCCESS);
      REQUIRE(file);close.file=file;REQUIRE(action==vfs::FileAction::kOpened);
      REQUIRE(file->ReadSync(retained,0,&retained_count)==X_STATUS_SUCCESS);REQUIRE(retained==pair->large);
      vfs::File::Information reopened_info{};REQUIRE(file->QueryInformation(&reopened_info)==X_STATUS_SUCCESS);
      REQUIRE(reopened_info.create_timestamp==genuine_new_birth);
      REQUIRE(created_checkpoint->CheckCurrent(error));REQUIRE_FALSE(error);
      file->Destroy();close.file=nullptr;
      const auto collide=NativeProfileSavePublicationTestAccess::PictureContext(large_service,pair_owner,device,"pp_64.png",error);
      REQUIRE(collide.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kCreate,
          vfs::FileAccess::kGenericWrite,false,true,&file,&action,collide)==X_STATUS_OBJECT_NAME_COLLISION);
      REQUIRE_FALSE(file);REQUIRE(action==vfs::FileAction::kExists);
      REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));
      const auto small_context=NativeProfileSavePublicationTestAccess::PictureContext(small_service,pair_owner,device,"pp_32.png",error);
      REQUIRE(small_context.managed_authority);REQUIRE_FALSE(error);
      REQUIRE(fs->OpenFile(root,"pp_32.png",vfs::FileDisposition::kOverwriteIf,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&file,&action,small_context)==X_STATUS_SUCCESS);
      REQUIRE(file);close.file=file;
      bool valid=true;
      SECTION("both genuine physical outputs publish with matching key model cache and restart") {}
      SECTION("a malformed second physical output never publishes the first output or key") {valid=false;}
      const std::array<uint8_t,3> malformed{0x89,0x50,0};
      const auto input=valid?std::span<const uint8_t>(pair->small):std::span<const uint8_t>(malformed);
      REQUIRE(file->SetLength(input.size())==X_STATUS_SUCCESS);
      REQUIRE(file->WriteSync(input,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==input.size());
      file->Destroy();close.file=nullptr;
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));REQUIRE_FALSE(std::filesystem::exists(package/"pp_32.png"));
      if(!valid) {
        REQUIRE(NativeProfileSavePublicationTestAccess::SealPictures(pair_owner));
        REQUIRE(NativeProfileSavePublicationTestAccess::SealPictures(pair_owner)); // retained first seal, exact retry
        REQUIRE(NativeProfileSavePublicationTestAccess::Publish(pair_owner).error==std::errc::device_or_resource_busy);
        REQUIRE(NativeProfileSavePublicationTestAccess::Cancel(pair_owner)==std::errc::device_or_resource_busy);
        NativeProfileSavePublicationTestAccess::RetirePicture(large);NativeProfileSavePublicationTestAccess::RetirePicture(small);
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(pair_owner));
        REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
        REQUIRE_FALSE(std::filesystem::exists(package/"pp_64.png"));REQUIRE_FALSE(std::filesystem::exists(package/"pp_32.png"));
        REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size());REQUIRE_FALSE(error);
      }else {
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealPictures(pair_owner));
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealPictures(pair_owner));
        const auto native=NativeTestRepack(pair_plan->files.at(kDashboardID).intended);
        const auto gpd=NativeProfileSavePublicationTestAccess::CreateStage(pair_owner,kDashboardID,error);REQUIRE(gpd);REQUIRE_FALSE(error);
        REQUIRE(NativeProfileSavePublicationTestAccess::OpenStage(gpd,device,&file)==X_STATUS_SUCCESS);REQUIRE(file);close.file=file;
        REQUIRE(file->SetLength(native.size())==X_STATUS_SUCCESS);
        REQUIRE(file->WriteSync(native,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==native.size());
        file->Destroy();close.file=nullptr;
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealStage(pair_owner,kDashboardID,native));
        const auto committed=NativeProfileSavePublicationTestAccess::Publish(pair_owner);REQUIRE_FALSE(committed.error);
        REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
        REQUIRE(CreationRead(package/"pp_64.png")==pair->large);REQUIRE(CreationRead(package/"pp_32.png")==pair->small);
        REQUIRE(runtime.profiles().CaptureProfileIcon(xuid,XTileType::kPersonalGamerTile)==pair->large);
        // A genuine precommit immutable picture source owns its old tuple;
        // publishing new personal PNGs neither rewrites nor invalidates it.
        std::fill(avatar_output.begin(),avatar_output.end(),0);
        REQUIRE(avatar_file->ReadSync(avatar_output,0,&avatar_count)==X_STATUS_SUCCESS);REQUIRE(avatar_output==avatar_bytes);
        const auto fresh_read=writer.model->CaptureRead(error);REQUIRE(fresh_read);REQUIRE_FALSE(error);
        const auto fresh_context=NativeProfileSavePublicationTestAccess::PictureReadContext(large_service,*fresh_read,device,"pp_64.png",error);
        REQUIRE(fresh_context.managed_authority);REQUIRE_FALSE(error);
        vfs::File* committed_file=nullptr;
        REQUIRE(fs->OpenFile(root,"pp_64.png",vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead,false,true,&committed_file,&action,fresh_context)==X_STATUS_SUCCESS);
        REQUIRE(committed_file);SourceClose committed_close{committed_file};
        std::fill(retained.begin(),retained.end(),0);REQUIRE(committed_file->ReadSync(retained,0,&retained_count)==X_STATUS_SUCCESS);
        REQUIRE(retained_count==pair->large.size());REQUIRE(retained==pair->large);
        std::optional<UserSetting> persisted;
        REQUIRE(UserSetting::ReadFromGpd(GpdInfoProfile(CreationRead(package/"FFFE07D1.gpd")),
            uint32_t(UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE),persisted)==X_ERROR_SUCCESS);
        REQUIRE(persisted);REQUIRE(persisted->Serialize()==pictures.settings.front().Serialize());
        NativeProfileSavePublicationTestAccess::RetirePicture(large);NativeProfileSavePublicationTestAccess::RetirePicture(small);
        NativeProfileSavePublicationTestAccess::RetireStage(gpd);expected=native;
      }
    }
    SECTION("real quiescent prepublication cancellation leaves no journal and frees the SAME model admission") {
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE_FALSE(writer.model->Pending(error)); REQUIRE_FALSE(error);
      REQUIRE(NativeProfileSavePublicationTestAccess::Publish(owner).error==std::errc::operation_canceled);
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size()); REQUIRE_FALSE(error);
    }
    SECTION("an actually open foreign GPD writer refuses first publication and permits exact no-journal cancellation") {
      const auto native=NativeTestRepack(plan->files.at(kDashboardID).intended);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Seal(owner,kDashboardID,native));
      vfs::File* file=nullptr; vfs::FileAction action{};
      REQUIRE(runtime.emulator->file_system()->OpenFile(writer.model->storage()->device()->ResolvePath(""),
          "FFFE07D1.gpd",vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&file,&action)==X_STATUS_SUCCESS);
      REQUIRE(file); struct Close {vfs::File* file;~Close(){if(file)file->Destroy();}} close{file};
      const auto refused=NativeProfileSavePublicationTestAccess::Publish(owner);
      REQUIRE(refused.error==std::errc::device_or_resource_busy);
      REQUIRE(refused.attempted_journal.empty()); REQUIRE(refused.storage.journal.empty());
      NativeProfileSavePublicationTestAccess::EndByteUse(owner);
      // A real refused original publication never started a journal. Closing
      // byte use or later removal of this conflict must NOT silently commit it.
      const auto recovery=NativeProfileSavePublicationTestAccess::RecoverAttempted(owner);
      REQUIRE(recovery.error==std::errc::operation_not_permitted);
      REQUIRE(recovery.attempted_journal.empty());REQUIRE(recovery.storage.journal.empty());
      REQUIRE_FALSE(recovery.storage.intent_durable);REQUIRE_FALSE(recovery.storage.done_durable);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE_FALSE(writer.model->Pending(error)); REQUIRE_FALSE(error);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size()); REQUIRE_FALSE(error);
    }
    SECTION("actual source retirement still permits exact prepublication cleanup without rebinding a replacement") {
      const auto native=NativeTestRepack(plan->files.at(kDashboardID).intended);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Seal(owner,kDashboardID,native));
      writer.model->storage()->Retire();
      REQUIRE(NativeProfileSavePublicationTestAccess::Publish(owner).error==std::errc::operation_canceled);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE_FALSE(writer.model->Pending(error)); REQUIRE_FALSE(error);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    }
    SECTION("expired opaque native owner remains busy and cannot silently become a host write") {
      owner.reset();
      REQUIRE(writer.ExecutePending(pending).error==std::errc::device_or_resource_busy);
      REQUIRE(writer.Submit(command).error==std::errc::device_or_resource_busy);
      REQUIRE(writer.model->Pending(error)==pending); REQUIRE_FALSE(error);
      REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==initial_rows.size()); REQUIRE_FALSE(error);
    }
  }
  REQUIRE(CreationRead(package/"Account")==account);
  REQUIRE(Appearance(GpdInfoProfile(CreationRead(package/"FFFE07D1.gpd")))==appearance);
  runtime.Restart(); runtime.profiles().Login(xuid,1,false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(xuid)==1);
  REQUIRE(CreationRead(package/"FFFE07D1.gpd")==expected);
  REQUIRE(CreationRead(package/"Account")==account);
  ReadCreationSave(runtime,xuid,"NativeOwnedSave",save);
}

namespace {
constexpr uint64_t kNativeImageConsumerItem = 0xCAFE541200002439ull;
void CheckNativeGpdImageConsumers(CreationRuntime& runtime, uint64_t xuid) {
  auto& kernel = *runtime.emulator->kernel_state();
  const auto main = xe::kernel::test::SyntheticProfilePicture64();
  const auto avatar = xe::kernel::test::SyntheticProfilePicture64(true);
  std::error_code error;
  const auto pair = PrepareHostProfilePicturePair(main, error);
  REQUIRE(pair); REQUIRE_FALSE(error);
  auto* tracker = kernel.xam_state()->user_tracker();
  // The requests deliberately supply unrelated items for fixed-key types and
  // an absent title for the native dashboard-only background family.
  struct Expected { XTileType type; uint32_t title; uint64_t item; const std::vector<uint8_t>* png; };
  const std::array cases{
      Expected{XTileType::kAchievement, kDashboardID, kNativeImageConsumerItem, &main},
      Expected{XTileType::kGameIcon, kDashboardID, 0x8765432100012345ull, &main},
      Expected{XTileType::kBkgnd, 0x12345678, 0xEDCB123400003579ull, &pair->small},
      Expected{XTileType::kAvatarFullBody, kDashboardID, 0xAABBCCDD00002468ull, &avatar}};
  const auto slot = runtime.profiles().GetUserIndexAssignedToProfile(xuid);
  REQUIRE(slot < 4);
  for (const auto& test : cases) {
    INFO("native GPD image consumer type=" << uint32_t(test.type));
    REQUIRE(tracker->GetIcon(xuid, test.title, test.type, test.item) == *test.png);
    CreationPage page(*runtime.emulator->memory());
    TileRequestArguments args;
    args.type = uint32_t(test.type); args.title = test.title; args.item = test.item;
    args.user = slot; args.destination = page.address + 0x100;
    args.size_address = page.address + 0x40;
    xe::store_and_swap<uint32_t>(page.memory.TranslateVirtual(args.size_address), 0x1000);
    X_HRESULT status = X_E_FAIL;
    const auto owned = CaptureTileRequest(kernel, args, status);
    REQUIRE(owned); REQUIRE(status == X_E_SUCCESS);
    uint32_t extended = X_E_FAIL, length = 99;
    REQUIRE(CompleteTileRequest(kernel, *owned, extended, length) == X_ERROR_SUCCESS);
    REQUIRE(extended == X_E_SUCCESS); REQUIRE(length == 0);
    REQUIRE(xe::load_and_swap<uint32_t>(page.memory.TranslateVirtual(args.size_address)) == test.png->size());
    REQUIRE(std::equal(test.png->begin(), test.png->end(), page.memory.TranslateVirtual<uint8_t*>(args.destination)));
    REQUIRE(*page.memory.TranslateVirtual<uint8_t*>(args.destination - 1) == 0xA5);
    REQUIRE(*page.memory.TranslateVirtual<uint8_t*>(args.destination + test.png->size()) == 0xA5);
  }
  REQUIRE(runtime.profiles().CaptureProfileIcon(xuid, XTileType::kAvatarFullBody) == avatar);
  // Exercise the actual texture request/ARGB publication, with padding that
  // must remain untouched. The first two source quadrants are independent
  // synthetic fixture colors, not generated by the production decoder.
  constexpr uint32_t stride = 64 * 4 + 16;
  CreationPage texture(*runtime.emulator->memory(), stride * 64 + 0x1000);
  TileRequestArguments args;
  args.texture = true; args.type = uint32_t(XTileType::kAvatarFullBody);
  args.title = kDashboardID; args.item = 0xFFEEDDCC00001122ull;
  args.user = slot; args.destination = texture.address + 0x100;
  args.stride = stride; args.height = 64;
  X_HRESULT status = X_E_FAIL;
  const auto owned = CaptureTileRequest(kernel, args, status);
  REQUIRE(owned); REQUIRE(status == X_E_SUCCESS);
  uint32_t extended = X_E_FAIL, length = 99;
  REQUIRE(CompleteTileRequest(kernel, *owned, extended, length) == X_ERROR_SUCCESS);
  REQUIRE(extended == X_E_SUCCESS); REQUIRE(length == 0);
  const auto* actual = texture.memory.TranslateVirtual<uint8_t*>(args.destination);
  const std::array<uint8_t, 4> left{255, 0, 255, 255}, right{128, 255, 0, 255};
  REQUIRE(std::equal(left.begin(), left.end(), actual));
  REQUIRE(std::equal(right.begin(), right.end(), actual + 32 * 4));
  for (uint32_t y = 0; y < 64; ++y) {
    REQUIRE(std::all_of(actual + y * stride + 64 * 4, actual + (y + 1) * stride,
                        [](uint8_t byte) { return byte == 0xA5; }));
  }
  REQUIRE(*texture.memory.TranslateVirtual<uint8_t*>(args.destination - 1) == 0xA5);
  REQUIRE(*texture.memory.TranslateVirtual<uint8_t*>(args.destination + stride * 64) == 0xA5);
}
}  // namespace

TEST_CASE("Scoped SYSTEM File publication adopts complete native settings and opaque records only at the joined real cache checkpoint",
          "[.installed_profile_creation][kernel][native-save][system-file][persistence]") {
  save_detail::SemanticUses semantic_uses;const auto semantic=semantic_uses.BeginGetter();REQUIRE(semantic);
  CreationRuntime runtime(true);
  const auto created=runtime.profiles().CreateProfileDetailed("SystemFile",true);
  REQUIRE(created.status==X_ERROR_SUCCESS);REQUIRE(created.signed_in);const auto xuid=created.xuid;
  const auto package=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(xuid);
  const auto before=CreationRead(package/"FFFE07D1.gpd"),account=CreationRead(package/"Account");
  const auto appearance=Appearance(GpdInfoProfile(before));
  const std::array<uint8_t,4> save{0xC1,0,0xE7,0x39};WriteCreationSave(runtime,xuid,"SystemFileSave",save);
  std::vector<uint8_t> expected=before;
  {
    std::error_code error;const auto roster=runtime.profiles().CaptureRosterExpected(error);REQUIRE(roster.roster);REQUIRE_FALSE(error);
    const auto source=NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),roster.roster,{},0,xuid,error);REQUIRE(source);REQUIRE_FALSE(error);
    const auto writer=NativeProfileSaveWriterTestAccess::Writer(source);
    const auto service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error);REQUIRE(service);REQUIRE_FALSE(error);
    auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
    const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
    vfs::File* file=nullptr;vfs::FileAction action{};
    struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} close{file};
    // Real ordinary filesystem bytes/RW intent are not themselves a native
    // issuer. Only the private fixture below supplies its component capability.
    REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
        vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&file,&action)==X_STATUS_SUCCESS);
    REQUIRE(file);const auto view=NativeProfileSavePublicationTestAccess::ReadView(*file);REQUIRE(view);
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PrepareSystem(source,view,kDashboardID,error,semantic,{},0));
    REQUIRE(error==std::errc::invalid_argument);REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PrepareSystem(source,view,0x11223344,error,semantic));
    REQUIRE(error==std::errc::invalid_argument);REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
    bool cancel=false,missing_thumbnail=false,recover_durable=false;
    SECTION("complete native setting and image output survives cache join and restart") {}
    SECTION("an actually durable journal recovers the SAME native transaction after a real namespace-cache conflict") {recover_durable=true;}
    SECTION("genuine pre-journal cancellation leaves all originals and saves") {cancel=true;}
    SECTION("missing actual native CON thumbnail participant refuses the combined output") {missing_thumbnail=true;}
    const auto owner=NativeProfileSavePublicationTestAccess::PrepareSystem(source,view,kDashboardID,error,semantic,
        missing_thumbnail?std::vector{filesystem::ProfileArtifactKind::kThumbnail}:std::vector<filesystem::ProfileArtifactKind>{});
    REQUIRE(owner);REQUIRE_FALSE(error);const auto pending=NativeProfileSavePublicationTestAccess::Pending(owner);REQUIRE(pending->native_issued());
    const auto baseline=NativeProfileSavePublicationTestAccess::Plan(owner,error);REQUIRE(baseline);REQUIRE_FALSE(error);
    for(const auto& [id,member]:baseline->files){REQUIRE(member.action==ProfileGpdFileAction::kUnchanged);REQUIRE(member.intended.empty());}
    // This deliberately changes a non-image setting plus a full64-bit opaque
    // record, outside the old image-only host command. The complete validated
    // SYSTEM output, including native table placement/slack, stays authoritative.
    GpdInfoProfile modified(before);UserSetting vibration(UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION,0);
    REQUIRE_FALSE(modified.UpsertSettingPreserving(vibration));
    constexpr uint64_t opaque_id=0xBADC0FFE00008127ull;const std::array<uint8_t,5> opaque{0x82,0,0xF4,0x11,0x7D};
    REQUIRE_FALSE(modified.UpsertImageChecked(opaque_id,opaque));
    const auto icon = xe::kernel::test::SyntheticProfilePicture64();
    const auto fullbody = xe::kernel::test::SyntheticProfilePicture64(true);
    const auto picture_pair = PrepareHostProfilePicturePair(icon, error);
    REQUIRE(picture_pair); REQUIRE_FALSE(error);
    REQUIRE_FALSE(modified.UpsertImageChecked(0x8000, icon));
    REQUIRE_FALSE(modified.UpsertImageChecked(0x8003, picture_pair->small));
    REQUIRE_FALSE(modified.UpsertImageChecked(0x8007, fullbody));
    REQUIRE_FALSE(modified.UpsertImageChecked(kNativeImageConsumerItem, icon));
    // A distinct existing low-DWORD image catches accidental key truncation.
    REQUIRE_FALSE(modified.UpsertImageChecked(uint32_t(kNativeImageConsumerItem), fullbody));
    std::vector<uint8_t> canonical;REQUIRE_FALSE(modified.SerializeChecked(canonical,GpdSnapshotStore::kMaximumBytes));
    const auto native=NativeTestRepack(canonical);REQUIRE(native!=before);
    REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
    REQUIRE(NativeProfileSavePublicationTestAccess::BindSystem(service,*file,view,owner,before)==X_STATUS_SUCCESS);
    REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0); // genuine metadata cut performs no stage/IO
    size_t written=0;REQUIRE(file->SetLength(native.size())==X_STATUS_SUCCESS);
    REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==1);
    const auto stage=NativeProfileSavePublicationTestAccess::Stage(owner,kDashboardID);REQUIRE(stage);
    REQUIRE(file->WriteSync(native,0,&written)==X_STATUS_SUCCESS);REQUIRE(written==native.size());REQUIRE(file->Flush()==X_STATUS_SUCCESS);
    REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    auto wrong_header=native;wrong_header[4]^=1;
    REQUIRE(NativeProfileSavePublicationTestAccess::SealStage(owner,kDashboardID,wrong_header));
    REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
    const auto sealed=NativeProfileSavePublicationTestAccess::SealStage(owner,kDashboardID,native);
    if(missing_thumbnail){REQUIRE(sealed==std::errc::operation_not_supported);cancel=true;}
    else REQUIRE_FALSE(sealed);
    if(cancel){
      file->Destroy();file=nullptr;NativeProfileSavePublicationTestAccess::RetireStage(stage);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
      REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    }else{
      auto committed=ProfileGpdTransactionAttempt{};
      if(recover_durable) {
        const auto old_model=writer.model->CaptureRead(error);REQUIRE(old_model);REQUIRE_FALSE(error);
        // A real other-thread GCRC holder makes namespace cache admission
        // refuse AFTER this actual journal has reached durable Done. It is
        // neither a fabricated transaction result nor a fault-success seam.
        std::future<ProfileGpdTransactionAttempt> worker;
        auto namespace_owner=global_critical_region::AcquireDirect();
        struct Drain {
          decltype(namespace_owner)& owner;decltype(worker)& future;
          ~Drain(){if(owner.owns_lock())owner.unlock();if(future.valid())future.wait();}
        } drain{namespace_owner,worker};
        worker=std::async(std::launch::async,[owner]{return NativeProfileSavePublicationTestAccess::Publish(owner);});
        const auto finished=worker.wait_for(std::chrono::seconds(5));
        namespace_owner.unlock();
        REQUIRE(finished==std::future_status::ready);
        const auto failed=worker.get();
        REQUIRE(failed.error==std::errc::device_or_resource_busy);
        REQUIRE(failed.state==ProfileGpdTransactionAttempt::State::kDurablePendingCommit);
        REQUIRE(failed.storage.intent_durable);REQUIRE(failed.storage.done_durable);
        REQUIRE_FALSE(failed.attempted_journal.empty());
        REQUIRE(CreationRead(package/"FFFE07D1.gpd")==native);
        const auto unpublished=writer.model->CaptureRead(error);REQUIRE(unpublished);REQUIRE_FALSE(error);
        REQUIRE(unpublished->origin.stamp.model_version==old_model->origin.stamp.model_version);
        REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
        NativeProfileSavePublicationTestAccess::EndByteUse(owner);
        committed=NativeProfileSavePublicationTestAccess::RecoverAttempted(owner);
        // Historical failure is still failure. Recovery advances only the
        // already-attempted intent; native cache readiness remains separate.
        REQUIRE(failed.error==std::errc::device_or_resource_busy);
        REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
      } else committed=NativeProfileSavePublicationTestAccess::Publish(owner);
      REQUIRE_FALSE(committed.error);
      REQUIRE(committed.state==ProfileGpdTransactionAttempt::State::kCommitted);
      REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
      REQUIRE_FALSE(writer.model->CaptureCommittedFileRead(error));REQUIRE(error==std::errc::device_or_resource_busy);
      auto held=NativeProfileSavePublicationTestAccess::HoldCacheCheckpoint(owner,kDashboardID,stage,error);REQUIRE(held);REQUIRE_FALSE(error);
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Promote(service,*file,owner,native)));
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ReadView(*file));held.reset();
      REQUIRE(writer.model->Pending(error)==pending);REQUIRE_FALSE(error);
      REQUIRE(NativeProfileSavePublicationTestAccess::Promote(service,*file,owner,native)==X_STATUS_SUCCESS);
      REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
      REQUIRE(writer.model->CaptureCommittedFileRead(error));REQUIRE_FALSE(error);
      const auto models=writer.model->Read(error);REQUIRE(models);REQUIRE_FALSE(error);
      REQUIRE(std::vector(models->dashboard.GetImage(opaque_id).begin(),models->dashboard.GetImage(opaque_id).end())==std::vector(opaque.begin(),opaque.end()));
      const auto actual_setting=models->dashboard.GetSettingData(uint32_t(UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION));
      const auto expected_setting=modified.GetSettingData(uint32_t(UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION));
      REQUIRE(std::equal(actual_setting.begin(),actual_setting.end(),expected_setting.begin(),expected_setting.end()));
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==native);expected=native;
      CheckNativeGpdImageConsumers(runtime, xuid);
    }
  }
  REQUIRE(CreationRead(package/"Account")==account);REQUIRE(Appearance(GpdInfoProfile(CreationRead(package/"FFFE07D1.gpd")))==appearance);
  runtime.Restart();runtime.profiles().Login(xuid,1,false);REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(xuid)==1);
  REQUIRE(CreationRead(package/"FFFE07D1.gpd")==expected);REQUIRE(CreationRead(package/"Account")==account);
  if (expected != before) { CheckNativeGpdImageConsumers(runtime, xuid); }
  ReadCreationSave(runtime,xuid,"SystemFileSave",save);
}

TEST_CASE("One Header-first scoped operation stages real raw CON then GPD through the same File IO and durable transaction",
          "[.installed_profile_creation][kernel][native-save][profile_header][host_io][persistence]") {
  save_detail::SemanticUses semantic_uses;const auto semantic=semantic_uses.BeginGetter();REQUIRE(semantic);
  CreationRuntime runtime(true);
  const auto created=runtime.profiles().CreateProfileDetailed("HeaderFirst",true);
  REQUIRE(created.status==X_ERROR_SUCCESS);REQUIRE(created.signed_in);const auto xuid=created.xuid;
  const auto relative=ProfileStorageLease::ProfileRelative(xuid);
  const auto physical=runtime.root.path/"content"/relative;
  const auto before=CreationRead(physical/"FFFE07D1.gpd"),account=CreationRead(physical/"Account");
  const auto avatar=Appearance(GpdInfoProfile(before));
  const std::array<uint8_t,4> save{0x19,0,0xE2,0x7A};WriteCreationSave(runtime,xuid,"HeaderFirstSave",save);
  std::vector<uint8_t> expected;
  std::filesystem::path header_path;
  std::vector<uint8_t> intended_header;
  {
    std::error_code error;const auto roster=runtime.profiles().CaptureRosterExpected(error);REQUIRE(roster.roster);REQUIRE_FALSE(error);
    const auto source=NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),roster.roster,{},0,xuid,error);REQUIRE(source);REQUIRE_FALSE(error);
    const auto writer=NativeProfileSaveWriterTestAccess::Writer(source);REQUIRE(writer.metadata);
    auto* const kernel=runtime.emulator->kernel_state();REQUIRE(kernel->console_signing_identity());
    // Explicit real fixture setup only: the current HOST profile recipe uses a
    // legacy aggregate Header. Build a mathematically valid local CON through
    // its actual CM CAS producer; this does not claim shipping migration/native
    //80128 acceptance or use a retail attestation.
    const auto target=relative.parent_path().parent_path()/"Headers"/"00010000"/(relative.filename().string()+".header");
    header_path=runtime.root.path/"content"/target;
    const auto legacy=writer.metadata->CaptureMetadata(filesystem::ContentMutationOwner::Kind::kHeader,relative,target,error);
    REQUIRE(legacy);REQUIRE_FALSE(error);REQUIRE(legacy->exists());
    vfs::XContentContainerHeader native_header{};
    native_header.content_header.magic=vfs::XContentPackageType::kCon;
    native_header.content_header.header_size=sizeof(native_header);
    native_header.content_metadata.metadata_version=2;
    native_header.content_metadata.content_type=XContentType::kProfile;
    native_header.content_metadata.profile_id=xuid;
    native_header.content_metadata.execution_info.title_id=kDashboardID;
    native_header.content_metadata.volume_descriptor.stfs.descriptor_length=36;
    const auto actual_layout=writer.owner->source()->CaptureContentLayout(relative,error);
    REQUIRE(actual_layout);REQUIRE_FALSE(error);
    const auto counts=folder_content_detail::CaptureFolderLayoutCounts(*actual_layout,error);
    REQUIRE(counts);REQUIRE_FALSE(error);
    native_header.content_metadata.volume_descriptor.stfs.file_table_block_count=counts->table_blocks;
    native_header.content_metadata.volume_descriptor.stfs.total_block_count=counts->blocks;
    const auto console=kernel->console_signing_identity()->console_id();
    std::copy(console.begin(),console.end(),native_header.content_metadata.console_id);
    // Explicit local synthetic source owns real nonzero alignment bytes. The
    // current shared native digest consumes them; H-only legacy setup is not a
    // genuine existing-CON source and must not be silently zero-extended.
    std::vector<uint8_t> header(0xA000,0xD3);std::memcpy(header.data(),&native_header,sizeof(native_header));
    REQUIRE_FALSE(folder_content_detail::SignNativeHeader(header,*kernel->console_signing_identity()));
    const auto converted=writer.metadata->WriteMetadata(filesystem::ContentMutationOwner::Kind::kHeader,relative,legacy,header);
    REQUIRE_FALSE(converted.error);REQUIRE(converted.complete);REQUIRE(CreationRead(header_path)==header);
    REQUIRE(runtime.content().MountManagedSourceDevices("hdd")==X_STATUS_SUCCESS);
    const auto content_source=runtime.content().sources().Source("hdd");REQUIRE(content_source);
    std::string guest=content_source->source.guest_root;
    for(const auto& part:relative)guest=xe::utf8::join_guest_paths(guest,part.string());
    auto* fs=runtime.emulator->file_system();vfs::FileAction action{};vfs::File* opened=nullptr;
    struct CloseRaw {vfs::File*& file;~CloseRaw(){if(file)file->Destroy();}} close_raw{opened};
    REQUIRE(fs->OpenFile(nullptr,guest,vfs::FileDisposition::kOpen,
        vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action)==X_STATUS_SUCCESS);
    REQUIRE(opened);REQUIRE(opened->folder_content_package());
    auto raw=object_ref<XFile>(new XFile(kernel,opened,true,false,true));opened=nullptr;
    struct CloseXFile {object_ref<XFile>& file;~CloseXFile(){if(file){file->ReleaseHandle();file.reset();}}} close_header{raw};
    const auto service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error);REQUIRE(service);REQUIRE_FALSE(error);
    REQUIRE(fs->RegisterManagedFile(service));const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
    REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
        vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action)==X_STATUS_SUCCESS);
    REQUIRE(opened);const auto view=NativeProfileSavePublicationTestAccess::ReadView(*opened);REQUIRE(view);
    auto gpd=object_ref<XFile>(new XFile(kernel,opened,true,false,true));opened=nullptr;CloseXFile close_gpd{gpd};
    bool close_after_header=false;
    SECTION("Header then later GPD publish once; cache readiness stays a distinct truthful duty") {}
    SECTION("actual operation close refuses later raw writes and keeps unknown Header cache pending until restart") {close_after_header=true;}
    // Refuse the incomplete native before image BEFORE creating model pending
    // or the physical Header/PIS stage. Matching H alone cannot admit this use.
    auto incomplete=header;incomplete.resize(0x971A);
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PrepareHeader(source,raw,incomplete,semantic,error));
    REQUIRE(error==std::errc::invalid_argument);
    REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
    REQUIRE(CreationRead(header_path)==header);
    const auto owner=NativeProfileSavePublicationTestAccess::PrepareHeader(source,raw,header,semantic,error);
    REQUIRE(owner);REQUIRE_FALSE(error);REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
    // Ordinary RW intention on the SAME raw File is still not an issuer.
    const auto picture=xe::kernel::test::SyntheticProfilePicture64();
    intended_header=header;auto* metadata=reinterpret_cast<vfs::XContentContainerHeader*>(intended_header.data());
    metadata->content_metadata.thumbnail_size=uint32_t(picture.size());
    std::copy(picture.begin(),picture.end(),metadata->content_metadata.thumbnail);
    REQUIRE_FALSE(folder_content_detail::SignNativeHeader(intended_header,*kernel->console_signing_identity()));
    CreationPage input(*runtime.emulator->memory(),0x20000);
    std::memcpy(input.memory.TranslateVirtual(input.address),intended_header.data(),intended_header.size());
    uint32_t count=0;
    REQUIRE(XFAILED(raw->Write(input.address,uint32_t(intended_header.size()),0,&count,0,false)));
    REQUIRE(count==0);REQUIRE(CreationRead(header_path)==header);
    const auto packet=NativeProfileSavePublicationTestAccess::HeaderCheckpoint(*raw,owner,0,intended_header,error);
    REQUIRE(packet.authority);REQUIRE_FALSE(error);
    REQUIRE(raw->Write(input.address,uint32_t(intended_header.size()),0,&count,0,false,nullptr,packet.authority)==X_STATUS_SUCCESS);
    REQUIRE(count==intended_header.size());REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(packet));
    REQUIRE(CreationRead(header_path)==header);REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==before);
    REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
    REQUIRE(NativeProfileSavePublicationTestAccess::Publish(owner).error==std::errc::device_or_resource_busy);
    if(close_after_header) {
      NativeProfileSavePublicationTestAccess::EndByteUse(owner);
      count=0xDEADBEEF;
      REQUIRE(XFAILED(raw->Write(input.address,uint32_t(intended_header.size()),0,&count,0,false)));
      REQUIRE(count==0);REQUIRE(CreationRead(header_path)==header);
      REQUIRE(NativeProfileSavePublicationTestAccess::Cancel(owner)==std::errc::operation_not_supported);
      REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
      REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==before);
      expected=before;intended_header=header;
    } else {
    // GPD input joins only at its actual later validated before-edit use.
    REQUIRE(NativeProfileSavePublicationTestAccess::BindSystem(service,*gpd->file(),view,owner,before)==X_STATUS_SUCCESS);
    GpdInfoProfile modified(before);UserSetting vibration(UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION,0);
    REQUIRE_FALSE(modified.UpsertSettingPreserving(vibration));std::vector<uint8_t> canonical;
    REQUIRE_FALSE(modified.SerializeChecked(canonical,GpdSnapshotStore::kMaximumBytes));
    const auto old_table=XdbfTableView::Parse(before),new_table=XdbfTableView::Parse(canonical);
    REQUIRE(old_table);REQUIRE(new_table);
    const auto old_setting=old_table->Find(uint16_t(GpdSection::kSetting),vibration.get_setting_id());
    const auto new_setting=new_table->Find(uint16_t(GpdSection::kSetting),vibration.get_setting_id());
    REQUIRE(old_setting);REQUIRE(new_setting);REQUIRE(old_setting->size()==new_setting->size());
    // This fixture exercises the existing no-geometry-change Header contract:
    // make an actual in-place native setting output and keep every original
    // table/unknown/slack byte and member EOF. Growth/allocation needs the real
    // native volume/descriptor duty; the Header-only checkpoint cannot invent it.
    expected=before;
    std::copy(new_setting->begin(),new_setting->end(),expected.begin()+(old_setting->data()-before.data()));
    REQUIRE(expected!=before);REQUIRE_FALSE(ValidateGpdSnapshot(expected));
    const uint64_t prefix=24+uint64_t(XdbfTableView::U32(expected.data()+8))*18+uint64_t(XdbfTableView::U32(expected.data()+16))*8;
    REQUIRE(prefix<=expected.size());const std::vector<uint8_t> table(expected.begin(),expected.begin()+size_t(prefix));
    std::memcpy(input.memory.TranslateVirtual(input.address),expected.data(),expected.size());
    const auto gpd_packet=NativeProfileSavePublicationTestAccess::Checkpoint(*gpd,service,owner,0,expected,table,error);
    REQUIRE(gpd_packet.authority);REQUIRE_FALSE(error);
    REQUIRE(gpd->Write(input.address,uint32_t(expected.size()),0,&count,0,false,nullptr,gpd_packet.authority)==X_STATUS_SUCCESS);
    REQUIRE(count==expected.size());REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(gpd_packet));
    REQUIRE(CreationRead(header_path)==header);REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==before);
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealWhole(owner));
    const auto committed=NativeProfileSavePublicationTestAccess::Publish(owner);
    REQUIRE_FALSE(committed.error);REQUIRE(committed.storage.intent_durable);REQUIRE(committed.storage.done_durable);
    REQUIRE(CreationRead(header_path)==intended_header);REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==expected);
    // This component has NOT joined original cached971A/ContentClose/native
    // readiness. Preserve that pending duty rather than publish fake readiness.
    REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Promote(service,*gpd->file(),owner,expected)));
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ReadView(*gpd->file()));
    REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(packet));
    REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(gpd_packet));
    NativeProfileSavePublicationTestAccess::EndByteUse(owner);
    }
  }
  runtime.Restart();
  REQUIRE(CreationRead(header_path)==intended_header);REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==expected);
  REQUIRE(CreationRead(physical/"Account")==account);REQUIRE(Appearance(GpdInfoProfile(expected))==avatar);
  REQUIRE(runtime.content().MountManagedSourceDevices("hdd")==X_STATUS_SUCCESS);
  const auto source=runtime.content().sources().Source("hdd");REQUIRE(source);
  std::string guest=source->source.guest_root;
  for(const auto& part:relative)guest=xe::utf8::join_guest_paths(guest,part.string());
  vfs::File* reopened=nullptr;vfs::FileAction action{};
  struct CloseReopened {vfs::File*& file;~CloseReopened(){if(file)file->Destroy();}} close_reopened{reopened};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,guest,vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead,false,true,&reopened,&action)==X_STATUS_SUCCESS);
  REQUIRE(reopened);REQUIRE(reopened->folder_content_package());
  ReadCreationSave(runtime,xuid,"HeaderFirstSave",save);
}

TEST_CASE("Early parent admission precedes actual PNG open and shares raw Header and GPD reads through late adoption and restart",
          "[.installed_profile_creation][kernel][native-save][profile_header][host_io][persistence]") {
  save_detail::SemanticUses semantic_uses;const auto semantic=semantic_uses.BeginGetter();REQUIRE(semantic);
  CreationRuntime runtime(true);const auto created=runtime.profiles().CreateProfileDetailed("ParentFirst",true);
  REQUIRE(created.status==X_ERROR_SUCCESS);REQUIRE(created.signed_in);const auto xuid=created.xuid;
  const auto relative=ProfileStorageLease::ProfileRelative(xuid);
  const auto physical=runtime.root.path/"content"/relative;
  const auto account=CreationRead(physical/"Account"),before=CreationRead(physical/"FFFE07D1.gpd");
  const auto png=CreationRead(physical/"avtr_64.png");REQUIRE_FALSE(ValidateProfilePicturePng(png,64));
  auto output=xe::kernel::test::SyntheticProfilePicture64(true);
  // Independent deterministic valid PNG fixture: an ancillary text chunk
  // keeps the original native block COUNT without pretending a new FAT map.
  // Its pixels and whole bytes differ from the installed avatar preview.
  const size_t desired=((png.size()+4095)/4096-1)*4096+1024;
  REQUIRE(desired>output.size()+12);REQUIRE(desired<=0x4000);
  const size_t text_bytes=desired-output.size()-12;
  std::vector<uint8_t> chunk(text_bytes+12,uint8_t('a'));
  const auto be32=[](uint8_t* p,uint32_t value){p[0]=uint8_t(value>>24);p[1]=uint8_t(value>>16);p[2]=uint8_t(value>>8);p[3]=uint8_t(value);};
  be32(chunk.data(),uint32_t(text_bytes));std::memcpy(chunk.data()+4,"tEXtAfterdark",12);chunk[16]=0;
  uint32_t crc=~uint32_t(0);
  for(size_t i=4;i<text_bytes+8;++i){crc^=chunk[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xEDB88320u&uint32_t(-int32_t(crc&1)));}
  be32(chunk.data()+text_bytes+8,~crc);
  output.insert(output.end()-12,chunk.begin(),chunk.end());REQUIRE(output.size()==desired);REQUIRE(output!=png);
  REQUIRE_FALSE(ValidateProfilePicturePng(output,64));
  REQUIRE((output.size()+4095)/4096==(png.size()+4095)/4096);
  auto expected_picture=png;
  const std::array<uint8_t,4> save{0x5A,0,0xC2,0x17};WriteCreationSave(runtime,xuid,"ParentSave",save);
  std::filesystem::path header_path;std::vector<uint8_t> expected_header,expected_gpd=before;
  {
    std::error_code error;const auto roster=runtime.profiles().CaptureRosterExpected(error);REQUIRE(roster.roster);REQUIRE_FALSE(error);
    const auto source=NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),roster.roster,{},0,xuid,error);REQUIRE(source);REQUIRE_FALSE(error);
    const auto writer=NativeProfileSaveWriterTestAccess::Writer(source);
    auto* kernel=runtime.emulator->kernel_state();auto* fs=runtime.emulator->file_system();
    // Same explicit synthetic CON component source as HeaderFirst above. This
    // owns real nonzero padding and validates crypto/extent. It does NOT claim
    // recovered native allocator/FAT history or shipping legacy migration.
    const auto target=relative.parent_path().parent_path()/"Headers"/"00010000"/(relative.filename().string()+".header");
    header_path=runtime.root.path/"content"/target;
    const auto legacy=writer.metadata->CaptureMetadata(filesystem::ContentMutationOwner::Kind::kHeader,relative,target,error);
    REQUIRE(legacy);REQUIRE_FALSE(error);
    vfs::XContentContainerHeader native{};native.content_header.magic=vfs::XContentPackageType::kCon;
    native.content_header.header_size=sizeof(native);native.content_metadata.metadata_version=2;
    native.content_metadata.content_type=XContentType::kProfile;native.content_metadata.profile_id=xuid;
    native.content_metadata.execution_info.title_id=kDashboardID;
    const auto layout=writer.owner->source()->CaptureContentLayout(relative,error);REQUIRE(layout);REQUIRE_FALSE(error);
    const auto counts=folder_content_detail::CaptureFolderLayoutCounts(*layout,error);REQUIRE(counts);REQUIRE_FALSE(error);
    auto& descriptor=native.content_metadata.volume_descriptor.stfs;descriptor.descriptor_length=36;
    descriptor.file_table_block_count=counts->table_blocks;descriptor.total_block_count=counts->blocks;
    const auto console=kernel->console_signing_identity()->console_id();std::copy(console.begin(),console.end(),native.content_metadata.console_id);
    std::vector<uint8_t> header(0xA000,0xB7);std::memcpy(header.data(),&native,sizeof(native));
    REQUIRE_FALSE(folder_content_detail::SignNativeHeader(header,*kernel->console_signing_identity()));
    const auto converted=writer.metadata->WriteMetadata(filesystem::ContentMutationOwner::Kind::kHeader,relative,legacy,header);
    REQUIRE_FALSE(converted.error);REQUIRE(converted.complete);expected_header=header;
    REQUIRE(runtime.content().MountManagedSourceDevices("hdd")==X_STATUS_SUCCESS);
    const auto content_source=runtime.content().sources().Source("hdd");REQUIRE(content_source);
    const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
    std::string guest=content_source->source.guest_root,raw_path;
    for(const auto& part:relative)guest=xe::utf8::join_guest_paths(guest,part.string());
    const auto raw_device=fs->AcquirePathDevice(guest);REQUIRE(raw_device);
    REQUIRE(guest.starts_with(raw_device->mount_path()));raw_path=guest.substr(raw_device->mount_path().size());
    while(!raw_path.empty() && raw_path.front()=='\\')raw_path.erase(raw_path.begin());
    const auto picture_service=NativeProfileSavePublicationTestAccess::PictureService(writer.model,filesystem::ProfileArtifactKind::kAvatar64,error);
    REQUIRE(picture_service);REQUIRE_FALSE(error);REQUIRE(fs->RegisterManagedFile(picture_service));
    const auto gpd_service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error);
    REQUIRE(gpd_service);REQUIRE_FALSE(error);REQUIRE(fs->RegisterManagedFile(gpd_service));
    bool competing=false,cancel_before_adoption=false,no_gpd_write=false,joint_cache=false;
    SECTION("actual first PNG open owns one Header reservation before effects then real late raw and GPD writes publish once") {}
    SECTION("native no-key branch observes the real unchanged GPD without inventing a GPD write or index") {no_gpd_write=true;}
    SECTION("joint cache consumer retries after actual GPD backend refusal then switches retained Files and releases only the SAME model milestone") {joint_cache=true;}
    SECTION("joint cache consumer releases the genuine no-GPD lane only after the retained raw Header switch") {joint_cache=true;no_gpd_write=true;}
    SECTION("real competing CM flush rejects first PNG open with no output stage or canonical effects") {competing=true;}
    SECTION("ending original byte use after actual PNG creation refuses late adoption and preserves canonical bytes on restart") {cancel_before_adoption=true;}
    auto competition=competing?writer.metadata->BeginFlush(relative,error):nullptr;
    if(competing){REQUIRE(competition);REQUIRE_FALSE(error);}
    const auto owner=NativeProfileSavePublicationTestAccess::PrepareParent(source,semantic,error,no_gpd_write);REQUIRE(owner);REQUIRE_FALSE(error);
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ParentHasAdmission(owner));
    REQUIRE(NativeProfileSavePublicationTestAccess::PictureStageCount(owner)==0);
    auto context=NativeProfileSavePublicationTestAccess::ParentPictureContext(picture_service,owner,device,error);
    REQUIRE(context.managed_authority);REQUIRE_FALSE(error);
    vfs::File* opened=nullptr;vfs::FileAction action{};
    struct CloseRaw {vfs::File*& file;~CloseRaw(){if(file)file->Destroy();}} close_opened{opened};
    const auto first=fs->OpenFile(root,"avtr_64.png",vfs::FileDisposition::kOverwriteIf,
        vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action,context);
    if(competing) {
      REQUIRE(first==vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)));REQUIRE_FALSE(opened);REQUIRE(NativeProfileSavePublicationTestAccess::PictureStageCount(owner)==0);
      REQUIRE(CreationRead(header_path)==header);REQUIRE(CreationRead(physical/"avtr_64.png")==png);
      competition.reset();NativeProfileSavePublicationTestAccess::EndByteUse(owner);
      // This is not successful native cancel/cache cleanup. Real restart owns
      // a still-unpublished pending source with no canonical byte effects.
      REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
    } else {
      REQUIRE(first==X_STATUS_SUCCESS);REQUIRE(opened);REQUIRE(NativeProfileSavePublicationTestAccess::ParentHasAdmission(owner));
      REQUIRE(NativeProfileSavePublicationTestAccess::PictureStageCount(owner)==1);
      // The original overwrite-if has already truncated its REAL output view.
      // The canonical tuple must still be intact before a later write could
      // accidentally restore it and conceal a raw-path/staging regression.
      REQUIRE(CreationRead(header_path)==header);REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==before);
      REQUIRE(CreationRead(physical/"avtr_64.png")==png);
      size_t count=0;
      std::shared_ptr<const NativeProfilePictureCheckpoint> picture_output;
      {
        auto picture=object_ref<XFile>(new XFile(kernel,opened,true,false,true));opened=nullptr;
        struct ClosePicture {object_ref<XFile>& file;~ClosePicture(){if(file){file->ReleaseHandle();file.reset();}}} close_picture{picture};
        CreationPage input(*runtime.emulator->memory(),0x20000);std::memcpy(input.memory.TranslateVirtual(input.address),output.data(),output.size());
        const auto packet=NativeProfileSavePublicationTestAccess::PictureCheckpoint(*picture,owner,output,error);
        REQUIRE(packet.authority);REQUIRE_FALSE(error);uint32_t written=0;
        REQUIRE(picture->Write(input.address,uint32_t(output.size()),0,&written,0,false,nullptr,packet.authority)==X_STATUS_SUCCESS);
        REQUIRE(written==output.size());REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(packet));
        const auto capture_picture=[&] {
          return NativeProfileSavePublicationTestAccess::PictureOutputCheckpoint(*picture->file(),owner,
              filesystem::ProfileArtifactKind::kAvatar64,error);
        };
        // A complete decoded output alone is not a real Flush receipt. This
        // source-owned overlay capture adds no second fsync or terminal seal.
        REQUIRE_FALSE(capture_picture());REQUIRE(error==std::errc::operation_would_block);
        REQUIRE(picture->Flush()==X_STATUS_SUCCESS);
        picture_output=capture_picture();REQUIRE(picture_output);REQUIRE_FALSE(error);
        REQUIRE(picture_output->CheckCurrent(error));REQUIRE_FALSE(error);
        REQUIRE(picture_output->before()->relative()=="avtr_64.png");
        REQUIRE(picture_output->kind()==filesystem::ProfileArtifactKind::kAvatar64);
        REQUIRE_FALSE(picture_output->new_creation_filetime()); // Existing-row birth is never inferred from host ctime.
        REQUIRE(std::equal(picture_output->bytes().begin(),picture_output->bytes().end(),output.begin(),output.end()));
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PictureOutputCheckpoint(*picture->file(),owner,
            filesystem::ProfileArtifactKind::kPublic64,error));REQUIRE(error==std::errc::invalid_argument);
        // Actual no-byte/no-time requests retain the completed epoch. Direct
        // VFS calls here exercise the backend, not native/XFile admission.
        count=123;REQUIRE(picture->file()->WriteSync({},0,&count)==X_STATUS_SUCCESS);REQUIRE(count==0);
        REQUIRE(picture->SetTimes(0,0,0)==X_STATUS_SUCCESS);
        REQUIRE(picture_output->CheckCurrent(error));REQUIRE_FALSE(error);
        vfs::File::Information times{};REQUIRE(picture->file()->QueryInformation(&times)==X_STATUS_SUCCESS);
        REQUIRE(times.create_timestamp==0);REQUIRE(times.write_timestamp<=UINT64_MAX-20000000);
        const auto requested_time=times.write_timestamp+20000000;
        REQUIRE(picture->SetTimes(0,0,requested_time)==X_STATUS_SUCCESS);
        REQUIRE(picture->file()->QueryInformation(&times)==X_STATUS_SUCCESS);REQUIRE(times.write_timestamp==requested_time);
        REQUIRE_FALSE(picture_output->CheckCurrent(error));REQUIRE(error==std::errc::state_not_recoverable);
        REQUIRE_FALSE(capture_picture());REQUIRE(error==std::errc::operation_would_block);
        REQUIRE(picture->Flush()==X_STATUS_SUCCESS);
        const auto timed=capture_picture();REQUIRE(timed);REQUIRE_FALSE(error);
        REQUIRE(timed->metadata().write_timestamp==requested_time);
        REQUIRE(std::equal(timed->bytes().begin(),timed->bytes().end(),output.begin(),output.end()));
        // Byte-identical writes still enter a new real output revision. An
        // older immutable checkpoint must not authorize that later mutation.
        count=0;REQUIRE(picture->file()->WriteSync(output,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==output.size());
        REQUIRE_FALSE(timed->CheckCurrent(error));REQUIRE(error==std::errc::state_not_recoverable);
        REQUIRE_FALSE(capture_picture());REQUIRE(error==std::errc::operation_would_block);
        REQUIRE(picture->Flush()==X_STATUS_SUCCESS);
        const auto repeated=capture_picture();REQUIRE(repeated);REQUIRE_FALSE(error);
        REQUIRE(picture->file()->SetLength(0)==X_STATUS_SUCCESS);
        REQUIRE_FALSE(repeated->CheckCurrent(error));REQUIRE(error==std::errc::state_not_recoverable);
        const std::array<uint8_t,8> malformed{};
        REQUIRE(picture->file()->WriteSync(malformed,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==malformed.size());
        REQUIRE(picture->Flush()==X_STATUS_SUCCESS);
        REQUIRE_FALSE(capture_picture());REQUIRE(error); // Real Flush is not a PNG-validity receipt.
        REQUIRE(picture->file()->WriteSync(output,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==output.size());
        REQUIRE(picture->Flush()==X_STATUS_SUCCESS);
        picture_output=capture_picture();REQUIRE(picture_output);REQUIRE_FALSE(error);
        // No stage-owned capture history: releasing each complete PNG capsule
        // releases its payload while this SAME mutable parent/File stays live.
        for(unsigned i=0;i<64;++i) {
          auto transient=capture_picture();REQUIRE(transient);REQUIRE_FALSE(error);
          std::weak_ptr<const NativeProfilePictureCheckpoint> released=transient;
          transient.reset();REQUIRE(released.expired());
        }
        REQUIRE(picture_output->CheckCurrent(error));REQUIRE_FALSE(error);
        REQUIRE(CreationRead(physical/"avtr_64.png")==png); // No intermediate canonical publication.
        if(!joint_cache)REQUIRE(XFAILED(picture->SetTimes(1,0,0))); // Never convert ctime into native birth.
        REQUIRE(picture_output->CheckCurrent(error));REQUIRE_FALSE(error); // Known pre-backend refusal has no effect.

        // Issued transfer authority cannot replay against a second write.
        REQUIRE(XFAILED(picture->Write(input.address,uint32_t(output.size()),0,&written,0,false,nullptr,packet.authority)));
        REQUIRE(written==0);
      }
      REQUIRE(CreationRead(header_path)==header);REQUIRE(CreationRead(physical/"avtr_64.png")==png);
      {
        // Ordinary sections include the deliberately refused birth call;
        // that actual managed IO failure survives close in THIS package only.
        // Joint-cache sections omit it and require a genuinely clean IO cut.
        // A clean completed close itself is no failure-history prerequisite.
        X_STATUS admitted=X_STATUS_UNSUCCESSFUL;
        const auto cut=XFile::CaptureIoCheckpoint(*kernel,*writer.model->storage()->view(),admitted);
        REQUIRE(cut);REQUIRE(admitted==X_STATUS_SUCCESS);
        if(joint_cache)REQUIRE(cut->Status()==X_STATUS_SUCCESS);
        else {REQUIRE(cut->member_count()>0);
          REQUIRE(cut->Status()==vfs::HostStorageError(std::make_error_code(std::errc::operation_not_supported)));}
        const auto unrelated_relative=*relative.begin()/"12345678"/"00000001"/"same-save";
        const auto unrelated=writer.owner->source()->Subroot(unrelated_relative,false,error);REQUIRE(unrelated);REQUIRE_FALSE(error);
        const auto other=XFile::CaptureIoCheckpoint(*kernel,*unrelated,admitted);
        REQUIRE(other);REQUIRE(admitted==X_STATUS_SUCCESS);REQUIRE(other->Status()==X_STATUS_SUCCESS);
        // This fixture only exercises storage/persistence separately. A real
        // native whole SAVE would refuse this failed IO cut, never publish
        // success merely because its final PNG bytes happen to decode.
      }
      {
        auto source_context=NativeProfileSavePublicationTestAccess::ParentPictureSourceContext(picture_service,owner,device,error);
        REQUIRE(source_context.managed_authority);REQUIRE_FALSE(error);
        REQUIRE(XFAILED(fs->OpenFile(root,"avtr_64.png",vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action,source_context)));REQUIRE_FALSE(opened);
        REQUIRE(fs->OpenFile(root,"avtr_64.png",vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead,false,true,&opened,&action,source_context)==X_STATUS_SUCCESS);
        std::vector<uint8_t> actual(output.size());
        REQUIRE(opened->ReadSync(actual,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==output.size());REQUIRE(actual==output);
        REQUIRE(XFAILED(opened->WriteSync(png,0,&count)));REQUIRE(count==0);
        opened->Destroy();opened=nullptr;
        REQUIRE(CreationRead(physical/"avtr_64.png")==png);
      }
      if(cancel_before_adoption) {
        NativeProfileSavePublicationTestAccess::EndByteUse(owner);
        REQUIRE(picture_output);
        REQUIRE_FALSE(picture_output->CheckCurrent(error));REQUIRE(error==std::errc::operation_canceled);
        REQUIRE(std::equal(picture_output->bytes().begin(),picture_output->bytes().end(),output.begin(),output.end()));
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ParentHeaderContext(owner,raw_device,raw_path,error).managed_authority);
        REQUIRE(error==std::errc::operation_canceled);
        REQUIRE(NativeProfileSavePublicationTestAccess::Cancel(owner)==std::errc::operation_not_supported);
      } else {
        // Ordinary reads self-conflict with the genuine parent reservation.
        REQUIRE(XFAILED(fs->OpenFile(nullptr,guest,vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead,false,true,&opened,&action)));REQUIRE_FALSE(opened);
        auto raw_context=NativeProfileSavePublicationTestAccess::ParentHeaderContext(owner,raw_device,raw_path,error);
        REQUIRE(raw_context.managed_authority);REQUIRE_FALSE(error);
        auto wrong_device=raw_context;wrong_device.expected_device=device;
        REQUIRE(XFAILED(fs->OpenFile(nullptr,guest,vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead,false,true,&opened,&action,wrong_device)));REQUIRE_FALSE(opened);
        REQUIRE(CreationRead(header_path)==header);REQUIRE(CreationRead(physical/"avtr_64.png")==png);
        // A pre-consumption device mismatch cannot burn the valid authority.
        REQUIRE(fs->OpenFile(nullptr,guest,vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action,raw_context)==X_STATUS_SUCCESS);
        REQUIRE(opened);std::vector<uint8_t> actual_header(header.size());
        REQUIRE(opened->ReadSync(actual_header,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==header.size());REQUIRE(actual_header==header);
        vfs::File* replay=nullptr;
        REQUIRE(XFAILED(fs->OpenFile(nullptr,guest,vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead,false,true,&replay,&action,raw_context)));REQUIRE_FALSE(replay);
        REQUIRE(CreationRead(header_path)==header);REQUIRE(CreationRead(physical/"avtr_64.png")==png);
        REQUIRE(XFAILED(opened->WriteSync(header,0,&count)));REQUIRE(count==0);
        REQUIRE(XFAILED(opened->Flush())); // Read observation does not own the write/barrier duty.
        auto raw=object_ref<XFile>(new XFile(kernel,opened,true,false,true));opened=nullptr;
        struct CloseXFile {object_ref<XFile>& file;~CloseXFile(){if(file){file->ReleaseHandle();file.reset();}}} close_raw{raw};
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::BindParentHeader(owner,raw,header));
        expected_header=header;auto* changed=reinterpret_cast<vfs::XContentContainerHeader*>(expected_header.data());
        changed->content_metadata.thumbnail_size=uint32_t(output.size());std::copy(output.begin(),output.end(),changed->content_metadata.thumbnail);
        REQUIRE_FALSE(folder_content_detail::SignNativeHeader(expected_header,*kernel->console_signing_identity()));
        CreationPage input(*runtime.emulator->memory(),0x20000);std::memcpy(input.memory.TranslateVirtual(input.address),expected_header.data(),expected_header.size());
        const auto raw_packet=NativeProfileSavePublicationTestAccess::HeaderCheckpoint(*raw,owner,0,expected_header,error);
        REQUIRE(raw_packet.authority);REQUIRE_FALSE(error);uint32_t transferred=0;
        REQUIRE(raw->Write(input.address,uint32_t(expected_header.size()),0,&transferred,0,false,nullptr,raw_packet.authority)==X_STATUS_SUCCESS);
        REQUIRE(transferred==expected_header.size());REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(raw_packet));
        auto gpd_context=NativeProfileSavePublicationTestAccess::ParentGpdContext(gpd_service,owner,device,error);
        object_ref<XFile> gpd;
        if(no_gpd_write) {
          REQUIRE_FALSE(gpd_context.managed_authority);REQUIRE(error==std::errc::operation_canceled);
          REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
        }else {
        REQUIRE(gpd_context.managed_authority);REQUIRE_FALSE(error);
        REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action,gpd_context)==X_STATUS_SUCCESS);
        REQUIRE(opened);const auto view=NativeProfileSavePublicationTestAccess::ReadView(*opened);REQUIRE(view);
        gpd=object_ref<XFile>(new XFile(kernel,opened,true,false,true));opened=nullptr;
        REQUIRE(NativeProfileSavePublicationTestAccess::BindSystem(gpd_service,*gpd->file(),view,owner,before)==X_STATUS_SUCCESS);
        // A real staged unchanged GPD is used here; native no-write/setting
        // lanes and native FAT/map activation remain separate prerequisites.
        const uint64_t prefix=24+uint64_t(XdbfTableView::U32(before.data()+8))*18+uint64_t(XdbfTableView::U32(before.data()+16))*8;
        REQUIRE(prefix<=before.size());const std::vector<uint8_t> table(before.begin(),before.begin()+size_t(prefix));
        std::memcpy(input.memory.TranslateVirtual(input.address),before.data(),before.size());
        const auto gpd_packet=NativeProfileSavePublicationTestAccess::Checkpoint(*gpd,gpd_service,owner,0,before,table,error);
        REQUIRE(gpd_packet.authority);REQUIRE_FALSE(error);
        REQUIRE(gpd->Write(input.address,uint32_t(before.size()),0,&transferred,0,false,nullptr,gpd_packet.authority)==X_STATUS_SUCCESS);
        REQUIRE(transferred==before.size());
        }
        CloseXFile close_gpd{gpd};
        // Retained immutable source readers are real dependencies, not active
        // writers. Keep one alive across the real data seal/transaction; it
        // must still read the same immutable staged bytes after publication.
        auto retained_context=NativeProfileSavePublicationTestAccess::ParentPictureSourceContext(picture_service,owner,device,error);
        REQUIRE(retained_context.managed_authority);REQUIRE_FALSE(error);
        vfs::File* retained_reader=nullptr;
        struct CloseReader {vfs::File*& file;~CloseReader(){if(file)file->Destroy();}} close_reader{retained_reader};
        REQUIRE(fs->OpenFile(root,"avtr_64.png",vfs::FileDisposition::kOpen,
            vfs::FileAccess::kGenericRead,false,true,&retained_reader,&action,retained_context)==X_STATUS_SUCCESS);
        NativeProfileSavePublicationTestAccess::EndDataUse(owner);
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealPictures(owner));
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealWhole(owner));
        const auto committed=NativeProfileSavePublicationTestAccess::Publish(owner);
        REQUIRE_FALSE(committed.error);REQUIRE(committed.storage.intent_durable);REQUIRE(committed.storage.done_durable);
        expected_picture=output;REQUIRE(CreationRead(physical/"avtr_64.png")==output);
        std::vector<uint8_t> retained_bytes(output.size());
        REQUIRE(retained_reader->ReadSync(retained_bytes,0,&count)==X_STATUS_SUCCESS);REQUIRE(count==output.size());REQUIRE(retained_bytes==output);

        REQUIRE(CreationRead(header_path)==expected_header);REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==before);
        REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error); // Durable storage alone did not settle cache ownership.
        if(gpd)REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::Promote(gpd_service,*gpd->file(),owner,before)));
        if(joint_cache) {
          if(gpd) {
            // The actual raw File is a genuinely wrong GPD backend. Header
            // preparation has already succeeded before this real type refusal;
            // retrying SAME committed tokens proves no partial Header swap.
            REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::JointCache(owner,raw,raw,gpd_service,before)));
            REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
          }
          const auto* raw_backend=raw->file();const auto* gpd_backend=gpd?gpd->file():nullptr;
          const auto ready=NativeProfileSavePublicationTestAccess::JointCache(owner,raw,gpd,
              no_gpd_write?nullptr:gpd_service,no_gpd_write?std::span<const uint8_t>{}:std::span<const uint8_t>(before));
          REQUIRE(ready==X_STATUS_SUCCESS);
          REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
          CHECK(raw->file()==raw_backend);CHECK((!gpd || gpd->file()==gpd_backend));
          actual_header.resize(expected_header.size());
          REQUIRE(raw->file()->ReadSync(actual_header,0,&count)==X_STATUS_SUCCESS);
          REQUIRE(count==expected_header.size());REQUIRE(actual_header==expected_header);
          // This is below native issuance: no actual C4D8/FE/control success,
          // native callback/cancellation or whole-IO factory pass is claimed.
        }
        NativeProfileSavePublicationTestAccess::EndByteUse(owner);
      }
    }
  }
  runtime.Restart();REQUIRE(CreationRead(header_path)==expected_header);REQUIRE(CreationRead(physical/"Account")==account);
  REQUIRE(CreationRead(physical/"FFFE07D1.gpd")==expected_gpd);REQUIRE(CreationRead(physical/"avtr_64.png")==expected_picture);
  ReadCreationSave(runtime,xuid,"ParentSave",save);
}

TEST_CASE("One scoped SYSTEM checkpoint joins actual XFile transfer durable native bytes cache readiness and restart while preserving truthful refusals",
          "[.installed_profile_creation][kernel][native-save][system-file][host_io][checkpoint][persistence]") {
  save_detail::SemanticUses semantic_uses;const auto semantic=semantic_uses.BeginGetter();REQUIRE(semantic);
  CreationRuntime runtime(true);
  const auto created=runtime.profiles().CreateProfileDetailed("FileCheckpoint",true);
  REQUIRE(created.status==X_ERROR_SUCCESS);REQUIRE(created.signed_in);const auto xuid=created.xuid;
  const auto package=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(xuid);
  const auto before=CreationRead(package/"FFFE07D1.gpd"),account=CreationRead(package/"Account");
  const auto avatar=Appearance(GpdInfoProfile(before));
  const std::array<uint8_t,4> save{0x81,0,0x53,0xDE};WriteCreationSave(runtime,xuid,"FileCheckpointSave",save);
  std::vector<uint8_t> expected=before;
  {
    std::error_code error;const auto roster=runtime.profiles().CaptureRosterExpected(error);REQUIRE(roster.roster);REQUIRE_FALSE(error);
    const auto source=NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),roster.roster,{},0,xuid,error);REQUIRE(source);REQUIRE_FALSE(error);
    const auto writer=NativeProfileSaveWriterTestAccess::Writer(source);
    const auto service=NativeProfileSavePublicationTestAccess::SourceService(writer.model,error);REQUIRE(service);REQUIRE_FALSE(error);
    auto* fs=runtime.emulator->file_system();REQUIRE(fs->RegisterManagedFile(service));
    const auto device=writer.model->storage()->device();auto* root=device->ResolvePath("");REQUIRE(root);
    vfs::File* opened=nullptr;vfs::FileAction action{};
    struct CloseRaw {vfs::File*& file;~CloseRaw(){if(file)file->Destroy();}} close_raw{opened};
    REQUIRE(fs->OpenFile(root,"FFFE07D1.gpd",vfs::FileDisposition::kOpen,
        vfs::FileAccess::kGenericRead|vfs::FileAccess::kGenericWrite,false,true,&opened,&action)==X_STATUS_SUCCESS);
    REQUIRE(opened);const auto view=NativeProfileSavePublicationTestAccess::ReadView(*opened);REQUIRE(view);
    auto file=object_ref<XFile>(new XFile(runtime.emulator->kernel_state(),opened,true,false,true));opened=nullptr;
    struct CloseFile {object_ref<XFile>& file;~CloseFile(){if(file){file->ReleaseHandle();file.reset();}}} close_file{file};
    // This tests the production one-shot packet through real File/IO22. The
    // private PrepareSystem fixture remains synthetic, not a native PPC issuer.
    auto owner=NativeProfileSavePublicationTestAccess::PrepareSystem(source,view,kDashboardID,error,semantic);
    REQUIRE(owner);REQUIRE_FALSE(error);
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error));
    REQUIRE(error==std::errc::operation_canceled); // An actual open is not the original binding.
    REQUIRE(NativeProfileSavePublicationTestAccess::BindSystem(service,*file->file(),view,owner,before)==X_STATUS_SUCCESS);
    auto pristine=NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error);
    REQUIRE(pristine);REQUIRE_FALSE(error);
    REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointBytes(pristine)==before);
    REQUIRE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,pristine,before,error));REQUIRE_FALSE(error);
    REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::StagedCheckpoint(*file->file(),owner,before,error));REQUIRE(error);
    auto wrong_before=before;wrong_before[4]^=1;
    REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,wrong_before,error));
    REQUIRE(error==std::errc::bad_message);
    // Complete read capsules own their bytes; this still-live native File and
    // pending operation do not retain a history of every no-change capture.
    for(unsigned i=0;i<64;++i) {
      auto transient=NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error);
      REQUIRE(transient);REQUIRE_FALSE(error);
      std::weak_ptr<const NativeProfileGpdCheckpoint> released=transient;
      transient.reset();REQUIRE(released.expired());
    }
    REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
    REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    GpdInfoProfile modified(before);UserSetting vibration(UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION,0);
    REQUIRE_FALSE(modified.UpsertSettingPreserving(vibration));
    UserSetting public_key(UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY,
        std::u16string(u"FFFE07D10002000300010003"));
    REQUIRE_FALSE(modified.UpsertSettingPreserving(public_key));
    constexpr uint64_t opaque_id=0xF00DCAFE00009637ull;const std::array<uint8_t,5> opaque{0x39,0,0xF1,0x72,0xB4};
    REQUIRE_FALSE(modified.UpsertImageChecked(opaque_id,opaque));
    std::vector<uint8_t> canonical;REQUIRE_FALSE(modified.SerializeChecked(canonical,GpdSnapshotStore::kMaximumBytes));
    auto native=NativeTestRepack(canonical);REQUIRE(native!=before);
    const uint64_t prefix=24+uint64_t(XdbfTableView::U32(native.data()+8))*18+uint64_t(XdbfTableView::U32(native.data()+16))*8;
    REQUIRE(prefix<=native.size());const std::vector<uint8_t> header(native.begin(),native.begin()+size_t(prefix));
    bool cancel_before=false,bad_header=false,wrong_offset=false,repeat=false,close_byte_use=false,semantic_loss=false,pristine_noop=false,deferred=false,unobserved=false,whole_refused=false,restored_after_unobserved=false,denied_before=false;
    size_t final_header_size=header.size();
    SECTION("same actual File publishes complete opaque native layout and restarts") {}
    SECTION("two actual staged GPD writes stay mutable and unjournaled until one whole-operation checkpoint") {deferred=true;}
    SECTION("a later unobserved staged byte mutation cannot publish an earlier whole-operation snapshot") {deferred=true;unobserved=true;}
    SECTION("write away and back still requires a new completed native DB checkpoint epoch") {
      deferred=true;unobserved=true;restored_after_unobserved=true;
    }
    SECTION("genuine no-byte no-op releases only its exact pending binding and a fresh scoped operation can use the same File") {pristine_noop=true;}
    SECTION("a real refused cached-before mutation invalidates its no-write receipt without creating a stage or changing restart bytes") {denied_before=true;cancel_before=true;}
    SECTION("a second genuine scoped producer uses the current promoted File without pretending it was reopened") {repeat=true;}
    SECTION("cancelled unstarted byte session refuses before backend without changing a save") {cancel_before=true;}
    SECTION("original edit own-return closes future byte authority without pretending the pending intent was cancelled") {close_byte_use=true;}
    SECTION("covered semantic observation loss reaches the actual byte IO without adopting a new stage") {semantic_loss=true;}
    SECTION("wrong actual transfer offset refuses then an exact fresh packet can finish") {wrong_offset=true;}
    SECTION("real backend success followed by invalid checkpoint header reports failure and preserves original target") {bad_header=true;}
    if(pristine_noop) {
      const auto original_read=writer.model->CaptureRead(error);REQUIRE(original_read);REQUIRE_FALSE(error);
      // An ongoing byte operation cannot be discarded as a no-op.
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::PristineNoop(*file,service,view,owner,before)));
      REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
      NativeProfileSavePublicationTestAccess::EndByteUse(owner);
      // Closing mutation does not destroy a valid retained-cache before tuple.
      // It DOES prevent this observation from minting another active receipt.
      REQUIRE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,pristine,before,error));REQUIRE_FALSE(error);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error));
      REQUIRE(error==std::errc::operation_canceled);
      auto wrong=before;wrong[4]^=1;
      REQUIRE(XFAILED(NativeProfileSavePublicationTestAccess::PristineNoop(*file,service,view,owner,wrong)));
      REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
      REQUIRE(NativeProfileSavePublicationTestAccess::PristineNoop(*file,service,view,owner,before)==X_STATUS_SUCCESS);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,pristine,before,error));
      REQUIRE(error==std::errc::operation_canceled);
      REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
      const auto after_noop=writer.model->CaptureRead(error);REQUIRE(after_noop);REQUIRE_FALSE(error);
      REQUIRE(after_noop->origin.stamp.model_version==original_read->origin.stamp.model_version);
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*file->file())==view);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      owner=NativeProfileSavePublicationTestAccess::PrepareSystem(source,view,kDashboardID,error,semantic);
      REQUIRE(owner);REQUIRE_FALSE(error);
      // A distinct issued operation whose File bind never happened also owns
      // no replacement bytes. Only closed-byte-use/full-rundown fixture may
      // release it; it cannot recover into a later unstarted commit.
      REQUIRE(NativeProfileSavePublicationTestAccess::CancelUnbound(owner)==std::errc::operation_not_permitted);
      NativeProfileSavePublicationTestAccess::EndByteUse(owner);
      REQUIRE(NativeProfileSavePublicationTestAccess::RecoverAttempted(owner).error==std::errc::operation_not_permitted);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::CancelUnbound(owner));
      REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*file->file())==view);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      owner=NativeProfileSavePublicationTestAccess::PrepareSystem(source,view,kDashboardID,error,semantic);
      REQUIRE(owner);REQUIRE_FALSE(error);
      REQUIRE(NativeProfileSavePublicationTestAccess::BindSystem(service,*file->file(),view,owner,before)==X_STATUS_SUCCESS);
      // Rebinding the SAME retained File/view to a new genuine writer cannot
      // retarget an old immutable receipt to that different publication.
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,pristine,before,error));
      REQUIRE(error==std::errc::operation_not_permitted);
      pristine=NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error);
      REQUIRE(pristine);REQUIRE_FALSE(error);
    }
    if(deferred) {
      NativeProfileSavePublicationTestAccess::EndByteUse(owner);
      REQUIRE(NativeProfileSavePublicationTestAccess::PristineNoop(*file,service,view,owner,before)==X_STATUS_SUCCESS);
      owner=NativeProfileSavePublicationTestAccess::PrepareSystem(source,view,kDashboardID,error,semantic,{},1,true);
      REQUIRE(owner);REQUIRE_FALSE(error);
      REQUIRE(NativeProfileSavePublicationTestAccess::BindSystem(service,*file->file(),view,owner,before)==X_STATUS_SUCCESS);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,pristine,before,error));
      REQUIRE(error==std::errc::operation_not_permitted);
      pristine=NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error);
      REQUIRE(pristine);REQUIRE_FALSE(error);
    }
    CreationPage page(*runtime.emulator->memory(),uint32_t(native.size()+0x1000));
    uint32_t count=0;util::IoTransferReceipt preparation;
    if(denied_before) {
      REQUIRE(file->file()->SetTimes(1,0,0)==X_STATUS_ACCESS_DENIED);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,pristine,before,error));
      REQUIRE(error==std::errc::operation_canceled);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error));
      REQUIRE(error==std::errc::operation_canceled);
      REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
      REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointBytes(pristine)==before);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    }
    if(!cancel_before && !close_byte_use && !semantic_loss){
      REQUIRE(file->file()->SetLength(native.size())==X_STATUS_SUCCESS);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,pristine,before,error));
      REQUIRE(error==std::errc::operation_canceled); // Actual stage adoption wins, even before its next Write.
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineCheckpoint(*file->file(),owner,before,error));
      REQUIRE(error==std::errc::operation_canceled);
      REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointBytes(pristine)==before);
      std::memcpy(page.memory.TranslateVirtual(page.address),native.data(),native.size());
      REQUIRE(file->Write(page.address,uint32_t(native.size()),0,&count,0,false,&preparation)==X_STATUS_SUCCESS);
      REQUIRE(count==native.size());REQUIRE(preparation.backend_returned);
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
    }
    auto claimed_header=header;if(bad_header)claimed_header[4]^=1;
    auto packet=NativeProfileSavePublicationTestAccess::Checkpoint(*file,service,owner,0,header,claimed_header,error);
    REQUIRE(packet.authority);REQUIRE(packet.outcome);REQUIRE_FALSE(error);
    if(cancel_before)REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
    if(close_byte_use)NativeProfileSavePublicationTestAccess::EndByteUse(owner);
    if(semantic_loss)semantic_uses.Unavailable();
    file->set_position(93);
    std::memset(page.memory.TranslateVirtual(page.address),0xD3,header.size());
    util::IoTransferReceipt transfer;count=0xDEADBEEF;
    const auto status=file->Write(page.address,uint32_t(header.size()),wrong_offset?1:0,&count,0,false,&transfer,packet.authority);
    REQUIRE(transfer.invocation==HostCallOutcome::kReturned);
    if(cancel_before || wrong_offset || close_byte_use || semantic_loss){
      REQUIRE(XFAILED(status));REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointRefused(packet));
      REQUIRE(count==0);REQUIRE_FALSE(transfer.backend_entered);REQUIRE_FALSE(transfer.backend_returned);
      REQUIRE(file->position()==93);REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE(packet.outcome->bytes.load()==0);REQUIRE(packet.outcome->status.load()==status);
      if(wrong_offset){
        packet=NativeProfileSavePublicationTestAccess::Checkpoint(*file,service,owner,0,header,header,error);
        REQUIRE(packet.authority);REQUIRE_FALSE(error);transfer={};count=0;
        REQUIRE(file->Write(page.address,uint32_t(header.size()),0,&count,0,false,&transfer,packet.authority)==X_STATUS_SUCCESS);
      }
    }else if(bad_header){
      REQUIRE(XFAILED(status));REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointPublicationFailed(packet));
      REQUIRE(count==header.size());REQUIRE(transfer.backend_entered);REQUIRE(transfer.backend_returned);
      REQUIRE(transfer.backend_status==X_STATUS_SUCCESS);REQUIRE(transfer.bytes==header.size());
      REQUIRE(file->position()==93);REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
      REQUIRE_FALSE(packet.outcome->intent_durable.load());REQUIRE_FALSE(packet.outcome->done_durable.load());
    }else REQUIRE(status==X_STATUS_SUCCESS);
    if(!cancel_before && !bad_header && !close_byte_use && !semantic_loss){
      if(deferred) {
        REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(packet));
        REQUIRE_FALSE(packet.outcome->journal_attempted.load());REQUIRE_FALSE(packet.outcome->intent_durable.load());
        REQUIRE_FALSE(packet.outcome->done_durable.load());REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
        REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
        const auto actual_stage=NativeProfileSavePublicationTestAccess::Stage(owner,kDashboardID);
        auto first_checkpoint=NativeProfileSavePublicationTestAccess::StagedCheckpoint(*file->file(),owner,native,error);
        REQUIRE(first_checkpoint);REQUIRE_FALSE(error);
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PristineMatches(*file->file(),owner,first_checkpoint,native,error));
        REQUIRE(error==std::errc::operation_not_permitted);
        REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointBytes(first_checkpoint)==native);
        REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==1);
        // A real zero-length backend call cannot mutate this stage. Its
        // truthful successful receipt must preserve the existing byte epoch.
        util::IoTransferReceipt empty_transfer;uint32_t empty_count=0xDEADBEEF;
        REQUIRE(file->Write(page.address,0,0,&empty_count,0,false,&empty_transfer)==X_STATUS_SUCCESS);
        REQUIRE(empty_count==0);REQUIRE(empty_transfer.backend_returned);REQUIRE(empty_transfer.bytes==0);
        REQUIRE(NativeProfileSavePublicationTestAccess::StagedCheckpoint(*file->file(),owner,native,error)==first_checkpoint);
        REQUIRE_FALSE(error);REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);
        auto rows=GpdDurableIntentStore::Scan(*writer.owner,error);REQUIRE_FALSE(error);
        const auto row_count=rows.size();
        // A second real setting producer changes the SAME physical stage;
        // successful first write never sealed it or changed canonical/model.
        GpdInfoProfile second(native);UserSetting restored_vibration(UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION,1);
        REQUIRE_FALSE(second.UpsertSettingPreserving(restored_vibration));
        UserSetting personal_key(UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE,
            std::u16string(u"FFFE07D10002000400010004"));
        REQUIRE_FALSE(second.UpsertSettingPreserving(personal_key));
        std::vector<uint8_t> second_canonical;REQUIRE_FALSE(second.SerializeChecked(second_canonical,GpdSnapshotStore::kMaximumBytes));
        const auto second_native=NativeTestRepack(second_canonical);REQUIRE(second_native!=native);
        REQUIRE(second_native.size()<=RegionSize(page.memory,page.address));
        REQUIRE(file->file()->SetLength(second_native.size())==X_STATUS_SUCCESS);
        std::memcpy(page.memory.TranslateVirtual(page.address),second_native.data(),second_native.size());
        util::IoTransferReceipt second_body;uint32_t second_count=0;
        REQUIRE(file->Write(page.address,uint32_t(second_native.size()),0,&second_count,0,false,&second_body)==X_STATUS_SUCCESS);
        REQUIRE(second_count==second_native.size());REQUIRE(second_body.backend_returned);
        // Native-cache reuse is neither an old BEFORE recapture nor a new
        // pending/File. Actual later IO invalidates the previous byte epoch,
        // even while its independently owned immutable bytes remain readable.
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::StagedCheckpoint(*file->file(),owner,native,error));
        REQUIRE(error==std::errc::state_not_recoverable);
        REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointBytes(first_checkpoint)==native);
        REQUIRE(NativeProfileSavePublicationTestAccess::Stage(owner,kDashboardID)==actual_stage);
        const uint64_t second_prefix=24+uint64_t(XdbfTableView::U32(second_native.data()+8))*18+
            uint64_t(XdbfTableView::U32(second_native.data()+16))*8;REQUIRE(second_prefix<=second_native.size());
        const std::vector<uint8_t> second_header(second_native.begin(),second_native.begin()+size_t(second_prefix));
        final_header_size=second_header.size();
        const auto second_packet=NativeProfileSavePublicationTestAccess::Checkpoint(*file,service,owner,0,second_header,second_header,error);
        REQUIRE(second_packet.authority);REQUIRE_FALSE(error);
        util::IoTransferReceipt second_transfer;
        REQUIRE(file->Write(page.address,uint32_t(second_header.size()),0,&second_count,0,false,&second_transfer,second_packet.authority)==X_STATUS_SUCCESS);
        REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(second_packet));
        REQUIRE_FALSE(second_packet.outcome->journal_attempted.load());REQUIRE(second_transfer.backend_returned);
        auto second_checkpoint=NativeProfileSavePublicationTestAccess::StagedCheckpoint(*file->file(),owner,second_native,error);
        REQUIRE(second_checkpoint);REQUIRE_FALSE(error);REQUIRE(second_checkpoint!=first_checkpoint);
        REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointBytes(second_checkpoint)==second_native);
        REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointBytes(first_checkpoint)==native);
        REQUIRE(NativeProfileSavePublicationTestAccess::Stage(owner,kDashboardID)==actual_stage);
        REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==1);
        REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==row_count);REQUIRE_FALSE(error);
        REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
        // Synthetic actual-quiescence fixture only. The real original native
        // ContentClose/whole checkpoint is not activated by this component.
        if(unobserved) {
          const uint8_t changed=uint8_t(second_native.back()^0x49);
          *page.memory.TranslateVirtual<uint8_t*>(page.address)=changed;
          util::IoTransferReceipt late;uint32_t late_count=0;
          REQUIRE(file->Write(page.address,1,uint32_t(second_native.size()-1),&late_count,0,false,&late)==X_STATUS_SUCCESS);
          REQUIRE(late_count==1);REQUIRE(late.backend_returned);
          if(restored_after_unobserved) {
            *page.memory.TranslateVirtual<uint8_t*>(page.address)=second_native.back();
            util::IoTransferReceipt restored;
            REQUIRE(file->Write(page.address,1,uint32_t(second_native.size()-1),&late_count,0,false,&restored)==X_STATUS_SUCCESS);
            REQUIRE(late_count==1);REQUIRE(restored.backend_returned);
            const auto stage_path=NativeProfileSavePublicationTestAccess::StagePath(actual_stage);
            REQUIRE(CreationRead(stage_path)==second_native);
          }
          REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::StagedCheckpoint(*file->file(),owner,second_native,error));
          REQUIRE(error==std::errc::state_not_recoverable);
          REQUIRE(NativeProfileSavePublicationTestAccess::SealWhole(owner)==std::errc::state_not_recoverable);
          REQUIRE(NativeProfileSavePublicationTestAccess::Publish(owner).error==std::errc::device_or_resource_busy);
          REQUIRE(GpdDurableIntentStore::Scan(*writer.owner,error).size()==row_count);REQUIRE_FALSE(error);
          REQUIRE(CreationRead(package/"FFFE07D1.gpd")==before);REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
          // Only this real no-journal, actually closed staged File may retire
          // for cancellation. No older snapshot is published after refusal.
          const auto stage=NativeProfileSavePublicationTestAccess::Stage(owner,kDashboardID);
          file->ReleaseHandle();file.reset();NativeProfileSavePublicationTestAccess::RetireStage(stage);
          NativeProfileSavePublicationTestAccess::EndByteUse(owner);
          REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
          REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);whole_refused=true;
        } else {
          REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::SealWhole(owner));
          const auto whole=NativeProfileSavePublicationTestAccess::Publish(owner);
          REQUIRE_FALSE(whole.error);REQUIRE(whole.storage.intent_durable);REQUIRE(whole.storage.done_durable);
          REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
          // Preparation is real exact committed-view work, but abandoning it
          // changes no File/model. A retry can prepare the SAME stage/source;
          // only the existing real model guard and no-fail Apply settle it.
          auto prepared=NativeProfileSavePublicationTestAccess::PrepareCache(service,*file->file(),owner,second_native,error);
          REQUIRE(prepared);REQUIRE_FALSE(error);
          REQUIRE(NativeProfileSavePublicationTestAccess::PreparedMatches(prepared,owner,actual_stage));
          prepared.reset();
          REQUIRE(NativeProfileSavePublicationTestAccess::Stage(owner,kDashboardID)==actual_stage);
          REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::ReadView(*file->file()));
          REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
          REQUIRE(NativeProfileSavePublicationTestAccess::Promote(service,*file->file(),owner,second_native)==X_STATUS_SUCCESS);
          REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(packet)); // Historical first receipt is unchanged.
          native=second_native;
        }
      } else REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointReady(packet));
      if(!whole_refused) {
      REQUIRE(count==header.size());REQUIRE(transfer.backend_returned);REQUIRE(transfer.backend_status==X_STATUS_SUCCESS);
      REQUIRE(packet.outcome->bytes.load()==header.size());REQUIRE(packet.outcome->status.load()==X_STATUS_SUCCESS);
      if(!deferred) {REQUIRE(packet.outcome->intent_durable.load());REQUIRE(packet.outcome->done_durable.load());}
      REQUIRE(file->position()==final_header_size);REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
      REQUIRE(NativeProfileSavePublicationTestAccess::ReadView(*file->file()));
      REQUIRE(CreationRead(package/"FFFE07D1.gpd")==native);expected=native;
      const auto models=writer.model->Read(error);REQUIRE(models);REQUIRE_FALSE(error);
      REQUIRE(std::vector(models->dashboard.GetImage(opaque_id).begin(),models->dashboard.GetImage(opaque_id).end())==std::vector(opaque.begin(),opaque.end()));
      const auto old_position=file->position();util::IoTransferReceipt repeated;uint32_t repeated_count=0xDEADBEEF;
      REQUIRE(file->Write(page.address,uint32_t(header.size()),0,&repeated_count,0,false,&repeated,packet.authority)==X_STATUS_ACCESS_DENIED);
      REQUIRE(repeated_count==0);REQUIRE_FALSE(repeated.backend_entered);REQUIRE(file->position()==old_position);
      if(deferred) REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointStaged(packet));
      else REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointReady(packet)); // historical metadata remains factual
      if(repeat) {
        // The immutable old File tuple still owns valid old bytes, but it is
        // not a fresh semantic before-CAS. The SAME real promoted backend is
        // captured from the current source by a second independent issuer.
        const auto second_source=NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),roster.roster,{},0,xuid,error);
        REQUIRE(second_source);REQUIRE_FALSE(error);
        const auto second_view=NativeProfileSavePublicationTestAccess::CurrentFileRead(*file->file(),writer.model,error);
        REQUIRE(second_view);REQUIRE_FALSE(error);REQUIRE(second_view!=view);
        REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::PrepareSystem(second_source,view,kDashboardID,error,semantic));
        REQUIRE(error==std::errc::operation_canceled);REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
        const auto second_owner=NativeProfileSavePublicationTestAccess::PrepareSystem(second_source,second_view,kDashboardID,error,semantic);
        REQUIRE(second_owner);REQUIRE_FALSE(error);
        REQUIRE(NativeProfileSavePublicationTestAccess::BindSystem(service,*file->file(),second_view,second_owner,native)==X_STATUS_SUCCESS);
        GpdInfoProfile second_model(native);const std::array<uint8_t,7> second_opaque{0x92,0,0x43,0xA1,0x31,0x76,0xE8};
        REQUIRE_FALSE(second_model.UpsertImageChecked(opaque_id,second_opaque));
        std::vector<uint8_t> second_canonical;REQUIRE_FALSE(second_model.SerializeChecked(second_canonical,GpdSnapshotStore::kMaximumBytes));
        const auto second_native=NativeTestRepack(second_canonical);
        REQUIRE(second_native.size()<=RegionSize(page.memory,page.address));
        REQUIRE(file->file()->SetLength(second_native.size())==X_STATUS_SUCCESS);
        std::memcpy(page.memory.TranslateVirtual(page.address),second_native.data(),second_native.size());
        util::IoTransferReceipt second_preparation;uint32_t second_count=0;
        REQUIRE(file->Write(page.address,uint32_t(second_native.size()),0,&second_count,0,false,&second_preparation)==X_STATUS_SUCCESS);
        REQUIRE(second_count==second_native.size());REQUIRE(CreationRead(package/"FFFE07D1.gpd")==native);
        const uint64_t second_prefix=24+uint64_t(XdbfTableView::U32(second_native.data()+8))*18+
            uint64_t(XdbfTableView::U32(second_native.data()+16))*8;
        REQUIRE(second_prefix<=second_native.size());
        const std::vector<uint8_t> second_header(second_native.begin(),second_native.begin()+size_t(second_prefix));
        const auto second_packet=NativeProfileSavePublicationTestAccess::Checkpoint(*file,service,second_owner,0,second_header,second_header,error);
        REQUIRE(second_packet.authority);REQUIRE_FALSE(error);
        std::memset(page.memory.TranslateVirtual(page.address),0xE1,second_header.size());
        util::IoTransferReceipt second_transfer;
        REQUIRE(file->Write(page.address,uint32_t(second_header.size()),0,&second_count,0,false,&second_transfer,second_packet.authority)==X_STATUS_SUCCESS);
        REQUIRE(NativeProfileSavePublicationTestAccess::CheckpointReady(second_packet));
        REQUIRE(second_count==second_header.size());REQUIRE(second_transfer.backend_returned);
        REQUIRE_FALSE(writer.model->Pending(error));REQUIRE_FALSE(error);
        REQUIRE(CreationRead(package/"FFFE07D1.gpd")==second_native);expected=second_native;
        REQUIRE(NativeProfileSavePublicationTestAccess::CurrentFileRead(*file->file(),writer.model,error));REQUIRE_FALSE(error);
      }
      } // Successful whole checkpoint only; refused path preserves before.
    }else if(close_byte_use || semantic_loss){
      REQUIRE(NativeProfileSavePublicationTestAccess::StageCount(owner)==0);
      REQUIRE(writer.model->Pending(error));REQUIRE_FALSE(error);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
    }else if(bad_header){
      const auto stage=NativeProfileSavePublicationTestAccess::Stage(owner,kDashboardID);REQUIRE(stage);
      file->ReleaseHandle();file.reset();NativeProfileSavePublicationTestAccess::RetireStage(stage);
      REQUIRE_FALSE(NativeProfileSavePublicationTestAccess::Cancel(owner));
    }
  }
  REQUIRE(CreationRead(package/"Account")==account);REQUIRE(Appearance(GpdInfoProfile(CreationRead(package/"FFFE07D1.gpd")))==avatar);
  runtime.Restart();runtime.profiles().Login(xuid,1,false);REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(xuid)==1);
  REQUIRE(CreationRead(package/"FFFE07D1.gpd")==expected);REQUIRE(CreationRead(package/"Account")==account);
  ReadCreationSave(runtime,xuid,"FileCheckpointSave",save);
}

TEST_CASE("Bounded selected-row SAVE writer keeps exact following model publication through roster settlement and cleanup",
          "[.installed_profile_creation][kernel][native-save][source][persistence]") {
  CreationRuntime runtime(true);
  const auto created = runtime.profiles().CreateProfileDetailed("WriterRows", true);
  REQUIRE(created.status == X_ERROR_SUCCESS); REQUIRE(created.signed_in); REQUIRE(created.xuid);
  const auto xuid = created.xuid;
  const auto package = runtime.root.path / "content" / ProfileStorageLease::ProfileRelative(xuid);
  const auto account = CreationRead(package / "Account");
  const GpdInfoProfile original(CreationRead(package / "FFFE07D1.gpd"));
  const auto appearance = Appearance(original);
  std::vector<uint8_t> edited_appearance;
  {
    std::error_code error;
    const auto installed = runtime.emulator->installed_system_source(); REQUIRE(installed);
    const auto presets = StockAvatarPresets::Capture(installed->snapshot(),error);
    REQUIRE(presets); REQUIRE_FALSE(error);
    for (uint32_t i = 0; i != StockAvatarPresets::kCount; ++i) {
      const auto* preset = presets->Get(i); REQUIRE(preset);
      if (preset->body_type != AvatarManifestBodyType(appearance)) {
        edited_appearance.assign(preset->appearance.begin(),preset->appearance.end()); break;
      }
    }
    REQUIRE_FALSE(edited_appearance.empty());
    REQUIRE_FALSE(NormalizeAvatarManifest(edited_appearance,xuid));
    REQUIRE(edited_appearance != appearance);
  }
  const std::array<uint8_t,3> save{0x6A,0,0x7B};
  WriteCreationSave(runtime,xuid,"NativeWriterSave",save);
  {
    std::error_code error;
    const auto current = runtime.profiles().CaptureRosterExpected(error);
    REQUIRE_FALSE(error); REQUIRE(current.roster); REQUIRE(current.roster->slots()[0].xuid == xuid);
    REQUIRE_FALSE(NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),current.roster,{},0,xuid^1,error));
    REQUIRE(error == std::errc::operation_canceled);
    auto first = NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),current.roster,{},0,xuid,error);
    REQUIRE(first); REQUIRE_FALSE(error); REQUIRE(NativeProfileSaveWriterTestAccess::Current(first,error));
    auto writer = NativeProfileSaveWriterTestAccess::Writer(first);
    const auto first_origin = NativeProfileSaveWriterTestAccess::Origin(first);
    ProfileGpdMutation command; command.kind = ProfileGpdMutationKind::kSetSetting;
    command.title_id = kDashboardID; command.request_id = writer.model->AllocateRequestId(error);
    REQUIRE_FALSE(error); command.setting.emplace(UserSettingId::XPROFILE_GAMERCARD_ZONE,int32_t(3));
    const auto committed = writer.Submit(command, first_origin.stamp);
    REQUIRE_FALSE(committed.error); REQUIRE(committed.state == ProfileGpdTransactionAttempt::State::kCommitted);
    REQUIRE_FALSE(NativeProfileSaveWriterTestAccess::Current(first,error));
    REQUIRE(error == std::errc::operation_canceled);
    const auto expected = runtime.profiles().CaptureRosterExpected(error); REQUIRE_FALSE(error); REQUIRE(expected.roster);
    ProfileRosterDesired desired;
    for (size_t slot = 0; slot != 4; ++slot) { desired.xuids[slot] = expected.roster->slots()[slot].xuid; }
    const auto intent = runtime.profiles().PrepareRoster(desired,expected,error);
    REQUIRE(intent); REQUIRE_FALSE(error);
    const auto blocked = runtime.profiles().CommitHostRoster(intent);
    REQUIRE(blocked.error == std::errc::device_or_resource_busy);
    REQUIRE(blocked.phase == ProfileRosterPhase::kPublished);
    // Owner's timeline still names old current, while actual PM selector is
    // the prepared envelope. Only the exact active intent qualifies it.
    REQUIRE_FALSE(NativeProfileSaveWriterTestAccess::CaptureGetter(runtime.profiles(),expected.roster,{},0,xuid,error));
    REQUIRE(error == std::errc::operation_canceled);
    auto following = NativeProfileSaveWriterTestAccess::CaptureGetter(runtime.profiles(),expected.roster,intent,0,xuid,error);
    REQUIRE(following); REQUIRE_FALSE(error); REQUIRE(NativeProfileSaveWriterTestAccess::Current(following,error));
    const auto following_origin = NativeProfileSaveWriterTestAccess::Origin(following);
    auto following_writer = NativeProfileSaveWriterTestAccess::Writer(following);
    REQUIRE(following_writer.model == writer.model);
    REQUIRE(following_origin.storage == first_origin.storage);
    REQUIRE(following_origin.stamp.model_version == first_origin.stamp.model_version + 1);
    command.kind = ProfileGpdMutationKind::kSetSettings;
    command.request_id = following_writer.model->AllocateRequestId(error); REQUIRE_FALSE(error);
    command.setting.reset();
    command.settings.emplace_back(UserSettingId::XPROFILE_GAMER_DIFFICULTY,int32_t(2));
    command.settings.emplace_back(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1,edited_appearance);
    const auto changed = following_writer.Submit(command,following_origin.stamp);
    REQUIRE_FALSE(changed.error); REQUIRE(changed.state == ProfileGpdTransactionAttempt::State::kCommitted);
    const auto durable_before_settlement = CreationRead(package / "FFFE07D1.gpd");
    first.reset(); NativeProfileSaveWriterTestAccess::Retry(runtime.profiles());
    REQUIRE(intent->result().phase == ProfileRosterPhase::kPublished);
    following.reset(); NativeProfileSaveWriterTestAccess::Retry(runtime.profiles());
    REQUIRE(intent->result().phase == ProfileRosterPhase::kCommitted);
    REQUIRE(CreationRead(package / "FFFE07D1.gpd") == durable_before_settlement);
    const auto model = runtime.profiles().AcquireProfileGpdModel(xuid,error);
    REQUIRE(model); REQUIRE_FALSE(error); REQUIRE(model == writer.model);
    const auto read = model->CaptureRead(error); REQUIRE(read); REQUIRE_FALSE(error);
    REQUIRE(read->origin.stamp.model_version == first_origin.stamp.model_version + 2);
    const auto zone = read->models->Setting(kDashboardID,uint32_t(UserSettingId::XPROFILE_GAMERCARD_ZONE),error);
    REQUIRE(zone); REQUIRE_FALSE(error); REQUIRE(std::get<int32_t>(zone->get_host_data()) == 3);
    const auto difficulty = read->models->Setting(kDashboardID,uint32_t(UserSettingId::XPROFILE_GAMER_DIFFICULTY),error);
    REQUIRE(difficulty); REQUIRE_FALSE(error); REQUIRE(std::get<int32_t>(difficulty->get_host_data()) == 2);
    // Same logical XUID/epoch is not the same retained roster object.
    REQUIRE_FALSE(NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),expected.roster,{},0,xuid,error));
    REQUIRE(error == std::errc::operation_canceled);
    const auto settled = runtime.profiles().CaptureRosterExpected(error); REQUIRE_FALSE(error); REQUIRE(settled.roster);
    auto cleanup = NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),settled.roster,intent,0,xuid,error);
    REQUIRE(cleanup); REQUIRE_FALSE(error); REQUIRE(NativeProfileSaveWriterTestAccess::Current(cleanup,error));
    cleanup.reset(); // genuine completed-job cleanup window, same exact envelope
  }
  REQUIRE(CreationRead(package / "Account") == account);
  REQUIRE(Appearance(GpdInfoProfile(CreationRead(package / "FFFE07D1.gpd"))) == edited_appearance);
  runtime.profiles().Logout(0,false); runtime.Restart(); runtime.profiles().Login(xuid,1,false);
  REQUIRE(runtime.profiles().GetUserIndexAssignedToProfile(xuid) == 1);
  const GpdInfoProfile restarted(CreationRead(package / "FFFE07D1.gpd"));
  std::optional<UserSetting> zone,difficulty;
  REQUIRE(UserSetting::ReadFromGpd(restarted,uint32_t(UserSettingId::XPROFILE_GAMERCARD_ZONE),zone) == X_ERROR_SUCCESS);
  REQUIRE(zone); REQUIRE(std::get<int32_t>(zone->get_host_data()) == 3);
  REQUIRE(UserSetting::ReadFromGpd(restarted,uint32_t(UserSettingId::XPROFILE_GAMER_DIFFICULTY),difficulty) == X_ERROR_SUCCESS);
  REQUIRE(difficulty); REQUIRE(std::get<int32_t>(difficulty->get_host_data()) == 2);
  REQUIRE(Appearance(restarted) == edited_appearance); REQUIRE(CreationRead(package / "Account") == account);
  ReadCreationSave(runtime,xuid,"NativeWriterSave",save);
  // Exact retired cleanup must not require a now-current source/model or look
  // up a replacement by XUID. PM teardown must observe the real count drained.
  {
    std::error_code error; const auto selected = runtime.profiles().CaptureRosterExpected(error);
    REQUIRE_FALSE(error); REQUIRE(selected.roster);
    auto retired = NativeProfileSaveWriterTestAccess::Capture(runtime.profiles(),selected.roster,{},1,xuid,error);
    REQUIRE(retired); REQUIRE_FALSE(error);
    auto writer = NativeProfileSaveWriterTestAccess::Writer(retired);
    writer.model->storage()->Retire();
    REQUIRE_FALSE(NativeProfileSaveWriterTestAccess::Current(retired,error));
    REQUIRE(error == std::errc::operation_canceled);
    retired.reset();
  }
}

TEST_CASE("Registered profile creation takes a device DWORD pointer and reports missing System as a failed owned completion",
          "[kernel][profile_creation][abi][failure]") {
  CreationRuntime runtime; CreationPage page(*runtime.emulator->memory()); auto& memory=page.memory;
  const auto source=runtime.content().sources().Source("hdd"); REQUIRE(source); REQUIRE(source->available);
  constexpr uint64_t xuid=0xe0000000000000a1ull;
  X_XAMACCOUNTINFO account{}; string_util::copy_and_swap_truncating(account.gamertag,u"Native Failure",xe::countof(account.gamertag));
  account.passport_password[7]=char(0x8e);
  std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
  auto context=CreationContext(runtime,page.address+0x2800);
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50),source->source.device_id);
  context.r[3]=0x80000011u; context.r[4]=page.address+0x50; context.r[5]=xuid;
  context.r[6]=page.address+0x100; context.r[7]=context.r[8]=context.r[9]=0; context.r[10]=page.address+0x40;
  REQUIRE(CallCreationExport(runtime,0x230,context)==X_E_SUCCESS);
  const uint32_t record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));
  REQUIRE(record); REQUIRE(RegionSize(memory,record)>=0xAC0);
  const auto* bytes=memory.TranslateVirtual<uint8_t*>(record);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes)==0x11);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes+4)==page.address+0x50);
  auto expected_account=account; expected_account.reserved_flags=0x40000001u;
  REQUIRE(std::memcmp(bytes+8,&expected_account,sizeof(account))==0);
  REQUIRE(std::memcmp(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account))==0);
  REQUIRE(xe::load_and_swap<uint64_t>(bytes+0xA78)==xuid);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes+0xABC)==1);
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x3F)==0xA5);
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x44)==0xA5);
  context=CreationContext(runtime,page.address+0x2800); context.r[3]=record; context.r[4]=page.address+0x60;
  REQUIRE(CallCreationExport(runtime,0x234,context)!=X_E_SUCCESS);
  REQUIRE(RegionSize(memory,record)==0);
  REQUIRE(runtime.profiles().GetAccountCount()==0); REQUIRE_FALSE(runtime.profiles().CaptureProfile(xuid));
  REQUIRE_FALSE(std::filesystem::exists(runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(xuid)));
  const auto previous=xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x60));
  context=CreationContext(runtime,page.address+0x2800); context.r[3]=record; context.r[4]=page.address+0x60;
  REQUIRE(CallCreationExport(runtime,0x234,context)==X_E_INVALIDARG);
  REQUIRE(xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x60))==previous);
}

TEST_CASE("Registered profile creation refuses null inputs and device values used as pointers without allocating output",
          "[kernel][profile_creation][abi][guards]") {
  CreationRuntime runtime; CreationPage page(*runtime.emulator->memory()); auto& memory=page.memory;
  const auto source=runtime.content().sources().Source("hdd"); REQUIRE(source);
  X_XAMACCOUNTINFO account{}; string_util::copy_and_swap_truncating(account.gamertag,u"Native Guard",xe::countof(account.gamertag));
  std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
  for (unsigned scenario=0;scenario!=4;++scenario) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50),source->source.device_id);
    auto context=CreationContext(runtime,page.address+0x2800);
    context.r[3]=0; context.r[4]=page.address+0x50; context.r[5]=0xe0000000000000a2ull;
    context.r[6]=page.address+0x100; context.r[7]=context.r[8]=context.r[9]=0; context.r[10]=page.address+0x40;
    if (scenario==0) { context.r[6]=0; }
    if (scenario==1) { context.r[10]=0; }
    if (scenario==2) { context.r[4]=source->source.device_id; }
    if (scenario==3) { context.r[4]=0; }
    const auto result=CallCreationExport(runtime,0x230,context);
    REQUIRE(result==X_E_INVALIDARG);
    REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40))==0xA5A5A5A5u);
    REQUIRE(runtime.profiles().GetAccountCount()==0);
  }
}

TEST_CASE("Profile creation completion ownership rejects pending null repeated and foreign status pointers",
          "[kernel][profile_creation][abi][lifetime]") {
  CreationRuntime runtime; CreationPage page(*runtime.emulator->memory()); auto& memory=page.memory;
  std::array<uint8_t,0xAC0> bytes{}; std::error_code error;
  const auto record=runtime.profiles().AllocateProfileCreationResult(bytes,error);
  REQUIRE(record); REQUIRE_FALSE(error);
  auto status=[&](uint32_t pointer,uint32_t destination) {
    auto context=CreationContext(runtime,page.address+0x2800); context.r[3]=pointer; context.r[4]=destination;
    return CallCreationExport(runtime,0x234,context);
  };
  REQUIRE(status(record,0)==0x8000000Au);
  REQUIRE(status(record,1)==0x8000000Au);
  REQUIRE(status(record,record)==0x8000000Au);
  REQUIRE(status(record,page.address+0x80)==0x8000000Au); REQUIRE(RegionSize(memory,record)>=0xAC0);
  REQUIRE(xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x80))==0xA5A5A5A5A5A5A5A5ull);
  ProfileManager::ProfileCreationResult failed; failed.status=X_ERROR_FUNCTION_FAILED;
  failed.xuid=0xe0000000000000a3ull;
  REQUIRE(runtime.profiles().CompleteProfileCreationResult(record,failed));
  REQUIRE_FALSE(runtime.profiles().CompleteProfileCreationResult(record,failed));
  REQUIRE(status(record,0)==X_E_INVALIDARG); REQUIRE(RegionSize(memory,record)>=0xAC0);
  REQUIRE(status(record,1)==X_E_INVALIDARG); REQUIRE(RegionSize(memory,record)>=0xAC0);
  REQUIRE(status(record,record)==X_E_INVALIDARG); REQUIRE(RegionSize(memory,record)>=0xAC0);
  REQUIRE(status(record,record+0xABC)==X_E_INVALIDARG); REQUIRE(RegionSize(memory,record)>=0xAC0);
  REQUIRE(status(record,page.address+0x80)==X_HRESULT_FROM_WIN32(X_ERROR_FUNCTION_FAILED));
  REQUIRE(xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x80))==failed.xuid);
  REQUIRE(RegionSize(memory,record)==0); REQUIRE(status(record,page.address+0x80)==X_E_INVALIDARG);
  REQUIRE(status(0,page.address+0x80)==X_E_INVALIDARG);
  const auto foreign=memory.SystemHeapAlloc(0xAC0); REQUIRE(foreign);
  const auto foreign_size=RegionSize(memory,foreign); REQUIRE(foreign_size>=0xAC0);
  std::memset(memory.TranslateVirtual(foreign),0xCC,0xAC0);
  REQUIRE(status(foreign,page.address+0x80)==X_E_INVALIDARG);
  REQUIRE(RegionSize(memory,foreign)==foreign_size);
  REQUIRE(*memory.TranslateVirtual<uint8_t*>(foreign)==0xCC);
  runtime.profiles().DiscardProfileCreationResult(foreign);
  REQUIRE(RegionSize(memory,foreign)==foreign_size); memory.SystemHeapFree(foreign);
  REQUIRE(runtime.profiles().GetAccountCount()==0);
}

TEST_CASE("Registered profile creation preserves native passcode flag truth table and caller account",
          "[kernel][profile_creation][abi][passcode]") {
  CreationRuntime runtime;
  CreationPage page(*runtime.emulator->memory());
  auto& memory=page.memory;
  const auto source=runtime.content().sources().Source("hdd"); REQUIRE(source);
  struct Row {
    uint32_t flags,initial,expected;
    std::array<uint8_t,4> passcode;
  };
  // Native17559: zero helper returns1 for four zero bytes, so the native
  // callback skips its OR10000000. Any nonzero byte sets it; existing bits
  // are never cleared. Flag2 terminates before filesystem/profile work.
  const std::array<Row,12> rows{{
    {3,0x100,0x00000101,{{0,0,0,0}}},
    {3,0x100,0x10000101,{{1,0,0,0}}},
    {3,0x100,0x10000101,{{0,2,0,0}}},
    {3,0x100,0x10000101,{{0,0,0x80,0}}},
    {3,0x100,0x10000101,{{0,0,0,0xFF}}},
    {3,0x10000100,0x10000101,{{0,0,0,0}}},
    {0x80000002,0x100,0x40000101,{{0,0,0,0}}},
    {0x80000002,0x100,0x50000101,{{0xFF,0,0,0}}},
    {0xC0000002,0x100,0xC0000101,{{0,0,0,0}}},
    {0xC0000002,0x100,0xD0000101,{{0,1,0,1}}},
    {2,0x100,0x00000100,{{0,0,0,0}}},
    {2,0x100,0x00000100,{{1,2,3,4}}}
  }};
  for(size_t i=0;i<rows.size();++i) {
    INFO("passcode contract row="<<i);
    const auto& row=rows[i];
    X_XAMACCOUNTINFO account{}; account.reserved_flags=row.initial;
    string_util::copy_and_swap_truncating(account.gamertag,u"Passcode ABI",xe::countof(account.gamertag));
    std::memcpy(account.passcode,row.passcode.data(),row.passcode.size());
    account.passport_password[7]=char(0x8E);
    account.owner_passport_membername[13]=char(0xB7);
    std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50),source->source.device_id);
    auto context=CreationContext(runtime,page.address+0x2800);
    context.r[3]=row.flags;context.r[4]=page.address+0x50;
    context.r[5]=0xE000000000000100ull+i;context.r[6]=page.address+0x100;
    context.r[7]=context.r[8]=context.r[9]=0;context.r[10]=page.address+0x40;
    REQUIRE(CallCreationExport(runtime,0x230,context)==X_E_SUCCESS);
    const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));
    REQUIRE(record); REQUIRE(RegionSize(memory,record)>=0xAC0);
    const auto* packet=memory.TranslateVirtual<uint8_t*>(record);
    auto expected=account; expected.reserved_flags=row.expected;
    REQUIRE(std::memcmp(packet+8,&expected,sizeof(account))==0);
    REQUIRE(std::memcmp(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account))==0);
    REQUIRE(xe::load_and_swap<uint32_t>(packet+0xAB8)==0); // No invented native Task.
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x3F)==0xA5);
    REQUIRE(*memory.TranslateVirtual<uint8_t*>(page.address+0x44)==0xA5);
    context=CreationContext(runtime,page.address+0x2800);
    context.r[3]=record;context.r[4]=page.address+0x60;
    REQUIRE(CallCreationExport(runtime,0x234,context)==X_HRESULT_FROM_WIN32(X_ERROR_NOT_SUPPORTED));
    REQUIRE(RegionSize(memory,record)==0);
    REQUIRE(runtime.profiles().GetAccountCount()==0);
  }
}

TEST_CASE("Registered local profile creation preserves native XUID branches and opaque input on failure",
          "[kernel][profile_creation][abi][failure]") {
  CreationRuntime runtime; CreationPage page(*runtime.emulator->memory()); auto& memory=page.memory;
  const auto source=runtime.content().sources().Source("hdd"); REQUIRE(source); REQUIRE(source->available);
  const auto recipe_status = MissingCreationRecipeStatus(runtime);
  struct Row {uint32_t flags,account_flags,status; bool generated,zero_xuid;};
  const std::array<Row,9> rows{{
    {0,0x100,X_ERROR_INVALID_PARAMETER,false,true},
    {0,0x100,recipe_status,false,false},
    {1,0x00000101,recipe_status,true,false},
    {0x80000001,0x40000101,X_ERROR_INVALID_PARAMETER,false,true},
    {0x80000001,0x40000101,recipe_status,false,false},
    {0xC0000001,0xC0000101,recipe_status,false,false},
    {0x40000020,0x100,recipe_status,false,false},
    {2,0x100,X_ERROR_NOT_SUPPORTED,false,false},
    {3,0x00000101,X_ERROR_NOT_SUPPORTED,true,false}
  }};
  std::array<uint8_t,0x8F0> payment{}; for(size_t i=0;i<payment.size();++i)payment[i]=uint8_t(i^0xC7);
  const std::array<uint8_t,0x1C> token{{0xA7,0,0x38,0xC1}};
  for(size_t i=0;i<rows.size();++i) {
    INFO("row="<<i); const auto& row=rows[i];
    const uint64_t input_xuid=row.zero_xuid?0:0xE000000000000100ull+i;
    X_XAMACCOUNTINFO account{}; account.reserved_flags=0x100;
    string_util::copy_and_swap_truncating(account.gamertag,u"Opaque Local",xe::countof(account.gamertag));
    account.passport_password[7]=char(0x8E); account.owner_passport_membername[13]=char(0xB7);
    std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
    std::memcpy(memory.TranslateVirtual(page.address+0x400),payment.data(),payment.size());
    std::memcpy(memory.TranslateVirtual(page.address+0x1000),token.data(),token.size());
    std::memcpy(memory.TranslateVirtual(page.address+0x1040),token.data(),token.size());
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50),source->source.device_id);
    auto c=CreationContext(runtime,page.address+0x2800);
    c.r[3]=row.flags;c.r[4]=page.address+0x50;c.r[5]=input_xuid;c.r[6]=page.address+0x100;
    c.r[7]=page.address+0x400;c.r[8]=page.address+0x1000;c.r[9]=page.address+0x1040;c.r[10]=page.address+0x40;
    REQUIRE(CallCreationExport(runtime,0x230,c)==X_E_SUCCESS);
    const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));REQUIRE(record);
    const auto* packet=memory.TranslateVirtual<uint8_t*>(record);
    REQUIRE(xe::load_and_swap<uint32_t>(packet)==(row.flags&0x7FFFFFFFu));
    REQUIRE(xe::load_and_swap<uint32_t>(packet+4)==page.address+0x50);
    REQUIRE(xe::load_and_swap<uint32_t>(packet+0xABC)==(row.flags>>31));
    auto expected_account=account;expected_account.reserved_flags=row.account_flags;
    REQUIRE(std::memcmp(packet+8,&expected_account,sizeof(account))==0);
    REQUIRE(std::memcmp(packet+0x184,payment.data(),payment.size())==0);
    REQUIRE(std::memcmp(packet+0xA80,token.data(),token.size())==0);
    REQUIRE(std::memcmp(packet+0xA9C,token.data(),token.size())==0);
    REQUIRE(xe::load_and_swap<uint32_t>(packet+0xAB8)==0); // No fabricated retail Task.
    const auto actual_xuid=xe::load_and_swap<uint64_t>(packet+0xA78);
    if(row.generated){REQUIRE(actual_xuid!=input_xuid);REQUIRE((actual_xuid>>60)==0xE);}
    else {REQUIRE(actual_xuid==input_xuid);}
    REQUIRE(std::memcmp(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account))==0);
    c=CreationContext(runtime,page.address+0x2800);c.r[3]=record;c.r[4]=page.address+0x60;
    REQUIRE(CallCreationExport(runtime,0x234,c)==X_HRESULT_FROM_WIN32(row.status));
    REQUIRE(xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x60))==actual_xuid);
    REQUIRE(RegionSize(memory,record)==0);
    REQUIRE(runtime.profiles().GetAccountCount()==0);
    // XUID zero has no profile-relative path; it denotes no candidate profile,
    // rather than the existing managed content root itself.
    REQUIRE(std::filesystem::is_directory(runtime.root.path / "content"));
    if (actual_xuid) { CheckNoCreatedProfile(runtime, actual_xuid); }
    if (input_xuid && input_xuid != actual_xuid) {
      CheckNoCreatedProfile(runtime, input_xuid);
    }
  }
}

TEST_CASE("Registered local profile creation preserves default and explicit device write ordering",
          "[kernel][profile_creation][abi][device]") {
  CreationRoot readonly;CreationRuntime runtime;CreationPage page(*runtime.emulator->memory());auto& memory=page.memory;
  const auto source=runtime.content().sources().Source("hdd");REQUIRE(source);REQUIRE(source->available);
  const auto recipe_status = MissingCreationRecipeStatus(runtime);
  std::error_code error;
  const auto read_root=filesystem::HostStorageRoot::Open(readonly.path/"content",true,error);
  REQUIRE(read_root);REQUIRE_FALSE(error);
  ContentSource read_source{"readonly-profile-target",3,DeviceType::MU,ContentSourceKind::kInstalledSystem,
      read_root->path(),"\\Device\\ReadonlyProfileTarget",u"Readonly profile target",true,0};
  REQUIRE(runtime.content().sources().RegisterReadOnly(read_source,read_root,1));
  X_XAMACCOUNTINFO account{};string_util::copy_and_swap_truncating(account.gamertag,u"Device Order",xe::countof(account.gamertag));
  std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
  for(const uint32_t device:{0u,source->source.device_id,3u,0x71324311u}) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50),device);
    auto c=CreationContext(runtime,page.address+0x2800);
    c.r[3]=0;c.r[4]=page.address+0x50;c.r[5]=0xE0000000000001D1ull;c.r[6]=page.address+0x100;
    c.r[7]=c.r[8]=c.r[9]=0;c.r[10]=page.address+0x40;
    REQUIRE(CallCreationExport(runtime,0x230,c)==X_E_SUCCESS);
    const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));REQUIRE(record);
    REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50))==(device?device:source->source.device_id));
    c=CreationContext(runtime,page.address+0x2800);c.r[3]=record;c.r[4]=page.address+0x60;
    const auto terminal=CallCreationExport(runtime,0x234,c);
    const uint32_t expected=device==0x71324311u?X_ERROR_DEVICE_NOT_CONNECTED:
        device==3?X_ERROR_ACCESS_DENIED:recipe_status;
    REQUIRE(terminal==X_HRESULT_FROM_WIN32(expected));
    REQUIRE(xe::load_and_swap<uint64_t>(memory.TranslateVirtual(page.address+0x60))==0xE0000000000001D1ull);
    REQUIRE(RegionSize(memory,record)==0);REQUIRE(runtime.profiles().GetAccountCount()==0);
    CheckNoCreatedProfile(runtime, 0xE0000000000001D1ull);
  }
  // This synchronous HLE provider deterministically publishes pending first.
  // Native Schedule can run its callback on either side of the output store;
  // this test makes no claim that all retail interleavings match this choice.
  // With r4==r10 here, processing reads the result pointer as an invalid ID.
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40),0);
  auto c=CreationContext(runtime,page.address+0x2800);
  c.r[3]=0;c.r[4]=page.address+0x40;c.r[5]=0xE0000000000001D2ull;c.r[6]=page.address+0x100;
  c.r[7]=c.r[8]=c.r[9]=0;c.r[10]=page.address+0x40;
  REQUIRE(CallCreationExport(runtime,0x230,c)==X_E_SUCCESS);
  const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));REQUIRE(record);
  REQUIRE(record!=source->source.device_id);
  c=CreationContext(runtime,page.address+0x2800);c.r[3]=record;c.r[4]=page.address+0x60;
  REQUIRE(CallCreationExport(runtime,0x234,c)==X_E_DEVICE_NOT_CONNECTED);
  REQUIRE(RegionSize(memory,record)==0);REQUIRE(runtime.profiles().GetAccountCount()==0);
}

TEST_CASE("Registered local profile creation reads explicit readonly device DWORD and refuses only a default write",
          "[kernel][profile_creation][abi][device][memory]") {
  CreationRuntime runtime;CreationPage page(*runtime.emulator->memory());auto& memory=page.memory;
  const auto source=runtime.content().sources().Source("hdd");REQUIRE(source);REQUIRE(source->available);
  const auto recipe_status = MissingCreationRecipeStatus(runtime);
  auto* heap=memory.LookupHeap(page.address);REQUIRE(heap);
  X_XAMACCOUNTINFO account{};string_util::copy_and_swap_truncating(account.gamertag,u"Readonly Device",xe::countof(account.gamertag));
  std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
  for(const uint32_t value:{source->source.device_id,0u}) {
    const auto device=page.address+0x1100;
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(device),value);
    REQUIRE(heap->Protect(page.address+0x1000,0x1000,kMemoryProtectRead));
    auto c=CreationContext(runtime,page.address+0x2800);
    c.r[3]=0;c.r[4]=device;c.r[5]=0xE0000000000001D3ull;c.r[6]=page.address+0x100;
    c.r[7]=c.r[8]=c.r[9]=0;c.r[10]=page.address+0x40;
    REQUIRE(CallCreationExport(runtime,0x230,c)==X_E_SUCCESS);
    const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));REQUIRE(record);
    REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(device))==value);
    c=CreationContext(runtime,page.address+0x2800);c.r[3]=record;c.r[4]=page.address+0x60;
    REQUIRE(CallCreationExport(runtime,0x234,c)==X_HRESULT_FROM_WIN32(value?recipe_status:X_ERROR_INVALID_PARAMETER));
    REQUIRE(RegionSize(memory,record)==0);
    REQUIRE(heap->Protect(page.address+0x1000,0x1000,kMemoryProtectRead|kMemoryProtectWrite));
    CheckNoCreatedProfile(runtime, 0xE0000000000001D3ull);
  }
  // With no real available device, readonly-zero must still fail the actual
  // default output check before enumeration can report disconnected.
  source->storage->Retire();
  REQUIRE(ListStorageDevices(runtime.emulator->kernel_state(),true).empty());
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x1100),0);
  REQUIRE(heap->Protect(page.address+0x1000,0x1000,kMemoryProtectRead));
  auto c=CreationContext(runtime,page.address+0x2800);
  c.r[3]=0;c.r[4]=page.address+0x1100;c.r[5]=0xE0000000000001D4ull;c.r[6]=page.address+0x100;
  c.r[7]=c.r[8]=c.r[9]=0;c.r[10]=page.address+0x40;
  REQUIRE(CallCreationExport(runtime,0x230,c)==X_E_SUCCESS);
  const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));REQUIRE(record);
  c=CreationContext(runtime,page.address+0x2800);c.r[3]=record;c.r[4]=page.address+0x60;
  REQUIRE(CallCreationExport(runtime,0x234,c)==X_E_INVALIDARG);
  REQUIRE(RegionSize(memory,record)==0);
  REQUIRE(heap->Protect(page.address+0x1000,0x1000,kMemoryProtectRead|kMemoryProtectWrite));
  REQUIRE(runtime.profiles().GetAccountCount()==0);
}

TEST_CASE("Initial package publication rejects a different selected generation before metadata mutation",
          "[kernel][profile_creation][content][generation]") {
  CreationRuntime runtime;
  const auto source=runtime.content().sources().Source("hdd");REQUIRE(source);REQUIRE(source->available);
  XCONTENT_AGGREGATE_DATA data{};data.device_id=source->source.device_id;data.title_id=0x12345678;
  data.content_type=XContentType::kSavedGame;data.set_file_name("SelectedGeneration");
  const std::vector<filesystem::ContentPackageInitialMember> members{{"state.bin",{0xA7,0,0xB8,0xC9}}};
  REQUIRE(runtime.content().CreateInitialPackage(0,data,members,*source)==X_ERROR_SUCCESS);
  const auto package=source->storage->path()/"0000000000000000"/"12345678"/"00000001"/"SelectedGeneration";
  const auto member_before=CreationRead(package/"state.bin");
  const auto header=package.parent_path().parent_path()/"Headers"/"00000001"/"SelectedGeneration.header";
  const auto header_before=CreationRead(header);
  auto stale=*source;++stale.generation;
  const std::vector<filesystem::ContentPackageInitialMember> changed{{"state.bin",{1,2,3}}};
  REQUIRE(runtime.content().CreateInitialPackage(0,data,changed,std::move(stale))==X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(CreationRead(package/"state.bin")==member_before);
  REQUIRE(CreationRead(header)==header_before);
  REQUIRE(runtime.content().sources().CaptureStorageVolume(*source)==source->storage_volume);
}


TEST_CASE("Registered local profile creation publishes through the explicit managed device generation",
          "[.installed_source][kernel][profile_creation][device][generation]") {
  // The secondary source lives until AFTER actual Emulator/registry teardown.
  CreationRoot secondary;CreationRuntime runtime(true);
  std::error_code error;
  auto content_root=filesystem::HostStorageRoot::Open(secondary.path/"content",false,error);
  REQUIRE(content_root);REQUIRE_FALSE(error);
  auto state_parent=filesystem::HostStorageRoot::Open(secondary.path/"cache",false,error);
  REQUIRE(state_parent);REQUIRE_FALSE(error);
  ContentSource descriptor{"profile-create-secondary",3,DeviceType::MU,ContentSourceKind::kManaged,
      content_root->path(),"\\Device\\CreationSecondary",u"Creation secondary",false,0};
  descriptor.private_state_parent = state_parent->path();
  REQUIRE_FALSE(runtime.content().sources().WritableSource(3));
  // Use the production producer: its journal/recovery owner and registry
  // publication are a single managed-device prerequisite for profile reads.
  REQUIRE(runtime.content().RegisterManagedSource(descriptor));
  const auto selected=runtime.content().sources().Source(descriptor.id);REQUIRE(selected);REQUIRE(selected->profile_writable);
  const auto volume = runtime.content().Volume(descriptor.id);
  REQUIRE(volume); REQUIRE(volume == selected->volume);
  REQUIRE_FALSE(volume->error()); REQUIRE(volume->profile_writable());
  REQUIRE(runtime.content().ProfileIntents(descriptor.id) == volume->intents());
  auto device=std::make_unique<vfs::HostPathDevice>(descriptor.guest_root,
      selected->storage->path(),false,false,"FATX",selected->storage);
  REQUIRE(device->SetStorageVolume(selected->storage_volume));REQUIRE(device->Initialize());
  REQUIRE(runtime.emulator->file_system()->RegisterDevice(std::move(device)));
  CreationPage page(*runtime.emulator->memory());auto& memory=page.memory;
  constexpr uint64_t xuid=0xE000000000000231ull;
  X_XAMACCOUNTINFO account{};account.reserved_flags=1;
  string_util::copy_and_swap_truncating(account.gamertag,u"Device Three",xe::countof(account.gamertag));
  account.passport_password[7]=char(0x8E);
  std::memcpy(memory.TranslateVirtual(page.address+0x100),&account,sizeof(account));
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50),3);
  auto c=CreationContext(runtime,page.address+0x2800);
  c.r[3]=0x20;c.r[4]=page.address+0x50;c.r[5]=xuid;c.r[6]=page.address+0x100;
  c.r[7]=c.r[8]=c.r[9]=0;c.r[10]=page.address+0x40;
  REQUIRE(CallCreationExport(runtime,0x230,c)==X_E_SUCCESS);
  const auto record=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x40));REQUIRE(record);
  c=CreationContext(runtime,page.address+0x2800);c.r[3]=record;c.r[4]=page.address+0x60;
  REQUIRE(CallCreationExport(runtime,0x234,c)==X_E_SUCCESS);REQUIRE(RegionSize(memory,record)==0);
  REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.address+0x50))==3);
  const auto package=selected->storage->path()/ProfileStorageLease::ProfileRelative(xuid);
  const auto account_bytes=CreationRead(package/"Account");X_XAMACCOUNTINFO saved{};
  REQUIRE(ProfileManager::DecryptAccountFile(account_bytes,&saved));
  REQUIRE(std::memcmp(&saved,&account,sizeof(account))==0);
  REQUIRE_FALSE(ValidateGpdSnapshot(CreationRead(package/"FFFE07D1.gpd")));
  for(const auto* member:{"tile_64.png","tile_32.png","avtr_64.png"})REQUIRE_FALSE(CreationRead(package/member).empty());
  REQUIRE_FALSE(std::filesystem::exists(runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(xuid)));
  const auto cached=runtime.profiles().CaptureCachedAccount(xuid,error);REQUIRE(cached);REQUIRE_FALSE(error);
  REQUIRE(cached->device_id()==3);REQUIRE(cached->source_id()==descriptor.id);
  REQUIRE(cached->source_generation()==selected->generation);
  REQUIRE(runtime.content().sources().CaptureStorageVolume(*selected)==selected->storage_volume);

  // Registered Open must describe its actual mounted source, including cached
  // reference reuse. A secondary profile is not on the primary HDD device.
  constexpr char alias[] = "creationsecondary:";
  constexpr uint32_t alias_offset = 0x1000, content_offset = 0x1100;
  std::memcpy(memory.TranslateVirtual(page.address + alias_offset), alias,
              sizeof(alias));
  auto* fs = runtime.emulator->file_system();
  REQUIRE_FALSE(fs->AcquireDevice(alias));
  std::shared_ptr<vfs::HostPathDevice> mounted;
  for (unsigned reference = 0; reference != 2; ++reference) {
    std::memset(memory.TranslateVirtual(page.address + content_offset), 0,
                sizeof(XCONTENT_AGGREGATE_DATA));
    c = CreationContext(runtime, page.address + 0x2800);
    c.r[3] = xuid; c.r[4] = page.address + alias_offset;
    c.r[5] = 0; c.r[6] = page.address + content_offset;
    REQUIRE(CallCreationExport(runtime, 0x237, c) == X_ERROR_SUCCESS);
    const auto* opened = memory.TranslateVirtual<const XCONTENT_AGGREGATE_DATA*>(
        page.address + content_offset);
    CHECK(opened->device_id == 3);
    CHECK(opened->content_type == XContentType::kProfile);
    CHECK(opened->title_id == kDashboardID);
    CHECK(opened->file_name() == xe::string_util::to_hex_string(xuid));
    const auto actual = std::dynamic_pointer_cast<vfs::HostPathDevice>(
        fs->AcquireDevice(alias));
    REQUIRE(actual);
    if (mounted) { CHECK(actual == mounted); }
    mounted = actual;
    REQUIRE(mounted->storage());
    CHECK(mounted->storage()->path() == package);
  }
  c = CreationContext(runtime, page.address + 0x2800);
  c.r[3] = page.address + alias_offset;
  REQUIRE(CallCreationExport(runtime, 0x238, c) == X_ERROR_SUCCESS);
  CHECK(fs->AcquireDevice(alias) == mounted);
  c = CreationContext(runtime, page.address + 0x2800);
  c.r[3] = page.address + alias_offset;
  REQUIRE(CallCreationExport(runtime, 0x238, c) == X_ERROR_SUCCESS);
  CHECK_FALSE(fs->AcquireDevice(alias));
  mounted.reset();

  uint32_t refused_device = 0xAABBCCDD;
  CHECK_FALSE(runtime.profiles().MountProfile(
      xuid ^ 1, "missingsecondary:", &refused_device));
  CHECK(refused_device == 0xAABBCCDD);
}

}  // namespace xe::kernel::xam::test

namespace xe::kernel::xam::test {
TEST_CASE("Actual host profile ingress pins Kernel before a concurrent shutdown and UI queue",
          "[kernel][profile][host_ui_fallback][lifetime]") {
  CreationRuntime runtime;
  class Context final : public xe::ui::WindowedAppContext {
    void NotifyUILoopOfPendingFunctions() override {}
    void PlatformQuitFromUIThread() override {}
  } app;
  KernelState* original = nullptr;
  std::shared_ptr<NativeUiBackendRoute::HostLease> lease;
  std::unique_ptr<NativeUiBackendRoute::HostLease::Entry> entered;
  std::weak_ptr<HostUiFallbackAccess> access;
  REQUIRE(runtime.emulator->WithHostRuntime([&](KernelState& state) -> X_RESULT {
    lease = state.AcquireLegacyHostUi();
    if (!lease) { return X_ERROR_ACCESS_DENIED; }
    entered = lease->TryEnter();
    if (!entered) { return X_ERROR_CANCELLED; }
    CHECK_FALSE(state.AcquireNativeUiCreation());
    original = &state;
    access = state.host_ui_access();
    return X_ERROR_SUCCESS;
  }) == X_ERROR_SUCCESS);
  REQUIRE(original);
  REQUIRE(entered);
  auto completion = std::make_shared<HostUiFallbackCompletion>();
  auto reservation = HostUiFallbackReservation::TryCreate(
      original->xam_state(), runtime.emulator->input_system(), true);
  REQUIRE(reservation);
  REQUIRE(reservation->input_gap());
  struct Observation {
    std::atomic<bool> system_identity{false}, work_finished{false};
    threading::Fence started;
  };
  const auto observation = std::make_shared<Observation>();
  struct CompleteOnExit {
    std::shared_ptr<HostUiFallbackCompletion> completion;
    ~CompleteOnExit() { completion->Finish({}); }
  } completion_cleanup{completion};
  REQUIRE(original->QueueHostUiWork([original, lease, completion, reservation,
                                   observation] {
    auto* current = XThread::GetCurrentThread();
    observation->system_identity.store(
        current && current->kernel_state() == original &&
        !current->is_guest_thread() &&
        current->native_frame_thread_identity().process_type == X_PROCTYPE_SYSTEM);
    observation->started.Signal();
    GuestScheduler::WaitOnFence(completion->closed);
    reservation->Release();
    observation->work_finished.store(true);
  }));
  reservation.reset();  // The raw-Xam reservation now belongs only to dispatch.
  bool dispatch_started = false;
  const auto start_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    dispatch_started = observation->started.TryWait();
    if (!dispatch_started) { std::this_thread::yield(); }
  } while (!dispatch_started && std::chrono::steady_clock::now() < start_deadline);
  REQUIRE(dispatch_started);
  struct Join {
    std::unique_ptr<NativeUiBackendRoute::HostLease::Entry>& entry;
    std::shared_ptr<HostUiFallbackCompletion> completion;
    std::thread worker;
    ~Join() {
      completion->Finish({});
      entry.reset();
      if (worker.joinable()) { worker.join(); }
    }
  } stopped{entered, completion,
      std::thread([&] { runtime.emulator->Shutdown(); })};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!lease->cancelled() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  CHECK(lease->cancelled());
  CHECK_FALSE(access.expired());  // Shutdown cannot reclaim this active Entry.
  unsigned frontend_accesses = 0;
  CHECK(QueueHostUiFallback(app, lease, completion,
      [&frontend_accesses](const auto&) { ++frontend_accesses; }));
  entered.reset();
  stopped.worker.join();
  CHECK(observation->system_identity.load());  // Also exercises idle SYSTEM dispatch startup.
  CHECK(observation->work_finished.load());
  CHECK(access.expired());
  CHECK_FALSE(lease->TryEnter());
  app.ExecutePendingFunctionsFromUIThread();
  CHECK(frontend_accesses == 0);
  runtime.Restart();
  REQUIRE(runtime.emulator->WithHostRuntime([&](KernelState& state) -> X_RESULT {
    auto replacement = state.AcquireLegacyHostUi();
    if (!replacement) { return X_ERROR_ACCESS_DENIED; }
    auto active = replacement->TryEnter();
    if (!active) { return X_ERROR_CANCELLED; }
    // Failed original-call reservation must release the claimed UI flag.
    auto unavailable = HostUiFallbackReservation::TryCreate(
        state.xam_state(), nullptr, true);
    REQUIRE(unavailable);
    CHECK_FALSE(unavailable->input_gap());
    CHECK_FALSE(state.xam_state()->IsUIActive());
    replacement->FinishHostOnly();
    return X_ERROR_SUCCESS;
  }) == X_ERROR_SUCCESS);
}
}  // namespace xe::kernel::xam::test
