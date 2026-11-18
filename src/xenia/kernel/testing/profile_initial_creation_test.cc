/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <thread>
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/ui/windowed_app_context.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
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
#include "xenia/kernel/xam/stock_avatar_presets.h"
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/kernel/xam/tile_request.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
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
    auto expected_account=account; expected_account.reserved_flags=0x50000001u;
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
  auto expected_account=account; expected_account.reserved_flags=0x50000001u;
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

TEST_CASE("Registered local profile creation preserves native XUID branches and opaque input on failure",
          "[kernel][profile_creation][abi][failure]") {
  CreationRuntime runtime; CreationPage page(*runtime.emulator->memory()); auto& memory=page.memory;
  const auto source=runtime.content().sources().Source("hdd"); REQUIRE(source); REQUIRE(source->available);
  const auto recipe_status = MissingCreationRecipeStatus(runtime);
  struct Row {uint32_t flags,account_flags,status; bool generated,zero_xuid;};
  const std::array<Row,9> rows{{
    {0,0x100,X_ERROR_INVALID_PARAMETER,false,true},
    {0,0x100,recipe_status,false,false},
    {1,0x10000101,recipe_status,true,false},
    {0x80000001,0x50000101,X_ERROR_INVALID_PARAMETER,false,true},
    {0x80000001,0x50000101,recipe_status,false,false},
    {0xC0000001,0xD0000101,recipe_status,false,false},
    {0x40000020,0x100,recipe_status,false,false},
    {2,0x100,X_ERROR_NOT_SUPPORTED,false,false},
    {3,0x10000101,X_ERROR_NOT_SUPPORTED,true,false}
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
