/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_state.h"
#include "xenia/emulator.h"
#include "xenia/vfs/installed_system_source.h"
#include "xenia/vfs/virtual_file_system.h"
#include "third_party/fmt/include/fmt/format.h"

DECLARE_bool(mount_memory_unit);
DECLARE_path(managed_device_state_root);

namespace xe {
namespace kernel {
namespace xam {

XamState::XamState(Emulator* emulator, KernelState* kernel_state)
    : kernel_state_(kernel_state) {
  app_manager_ = std::make_unique<AppManager>();

  auto content_root = emulator->content_root();
  if (!content_root.empty()) {
    content_root = std::filesystem::absolute(content_root);
  }
  // Retain explicit native storage inputs independently of guest execution.
  // ConsoleDataOwner is not a general data-parent producer.
  std::error_code storage_error;
  auto data_parent = filesystem::HostStorageRoot::Open(emulator->storage_root(),
                                                       false, storage_error, 0);
  std::shared_ptr<filesystem::HostStorageRoot> state_parent;
  if (data_parent && !cvars::managed_device_state_root.empty()) {
    state_parent = filesystem::HostStorageRoot::Open(
        cvars::managed_device_state_root, false, storage_error, 0);
  }
  std::shared_ptr<const filesystem::ManagedStorageContext> storage_context;
  if (!storage_error) {
    storage_context = filesystem::ManagedStorageContext::Capture(
        std::move(data_parent), kernel_state->snapshot_recovery_owner(),
        std::move(state_parent), storage_error);
  }
  content_manager_ = std::make_unique<ContentManager>(
      std::move(storage_context), content_root, kernel_state);
  if (const auto image = emulator->installed_system_source()) {
    const auto source = image->snapshot();
    std::error_code source_error;
    auto content = source->ContentRoot(source_error);
    // Missing local/system content stays absent until an explicit host refresh.
    // The common root was validated before XAM construction.
    content_manager_->sources().RegisterReadOnly(
        {"installed-system", static_cast<uint32_t>(DummyDeviceId::HDD),
         DeviceType::HDD, ContentSourceKind::kInstalledSystem,
         source->path() / "sysext" / "Content",
         "\\Device\\Harddisk0\\SystemExtPartition\\Content", u"System Content",
         true, 0, "\\Device\\Harddisk0\\SystemExtPartition"},
        std::move(content), source->generation());
  } else {
    content_manager_->sources().Register(
        {"installed-system", static_cast<uint32_t>(DummyDeviceId::HDD),
         DeviceType::HDD, ContentSourceKind::kInstalledSystem,
         emulator->system_files_path() / "sysext" / "Content",
         "\\Device\\Harddisk0\\SystemExtPartition\\Content", u"System Content",
         true, 0, "\\Device\\Harddisk0\\SystemExtPartition"});
  }
  if (cvars::mount_memory_unit) {
    // Preserve the existing MU: folder/mount, while making it an actual XAM
    // content source too. 17559 presents such storage as USB mass storage.
    content_manager_->RegisterRequiredManagedSource(
        {"memory-unit", 3, DeviceType::USBMASS, ContentSourceKind::kManaged,
         emulator->storage_root() / "mu" / "Content",
         "\\Device\\Mass0\\Content", u"USB Storage", false,
         0, "\\Device\\Mass0", emulator->storage_root() / "mu",
         emulator->storage_root()});
  }

  if (const auto installed = emulator->installed_system_source()) {
    stock_tiles_ = std::make_shared<TileService::StockProvider>(installed);
  }

  user_tracker_ = std::make_unique<UserTracker>();
  profile_manager_ =
      std::make_unique<ProfileManager>(kernel_state, user_tracker_.get(),
                                       *content_manager_);
  achievement_manager_ = std::make_unique<AchievementManager>();

  LoadLanguageLocaleFallback();
  LoadLanguageTypefacePatch();
  LoadIptvServiceName();

  AppManager::RegisterApps(kernel_state, app_manager_.get());
}

void XamState::LoadLanguageLocaleFallback() {
  const std::array<std::u16string, 18> locale_data = {
      u"",      u"",      u"ja-JP", u"de-DE", u"fr-FR",  u"es-ES",
      u"it-IT", u"ko-KR", u"zh-TW", u"pt-BR", u"zh-CHS", u"pl-PL",
      u"ru-RU", u"sv-SE", u"tr-TR", u"nb-NO", u"nl-NL",  u"zh-CHS"};

  constexpr uint32_t array_start = 0x80D00000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(array_start, 0xC8, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    char16_t* ptr =
        kernel_state_->memory()->TranslateVirtual<char16_t*>(array_start);

    for (size_t i = 1; i < locale_data.size(); i++) {
      language_fallback_address_[i] =
          kernel_state_->memory()->HostToGuestVirtual(ptr);
      ptr += xe::string_util::copy_and_swap_truncating(
                 ptr, locale_data.at(i), locale_data.at(i).size() + 1) +
             1;
    }
  }
}

void XamState::LoadLanguageTypefacePatch() {
  const std::array<std::u16string, 7> patch_data = {
      u"",
      u"file://media:/XenonSCLatin.xttp2",
      u"file://media:/XenonCLatin.xttp2",
      u"file://media:/XenonJKLatin.xttp2",
      u"file://media:/XenonSCLatin.xttp1",
      u"file://media:/XenonCLatin.xttp1",
      u"file://media:/XenonJKLatin.xttp1"};

  constexpr uint32_t array_start = 0x80D10000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(array_start, 0x168, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    char16_t* ptr =
        kernel_state_->memory()->TranslateVirtual<char16_t*>(array_start);

    for (size_t i = 1; i < patch_data.size(); i++) {
      language_type_face_patch_[i] =
          kernel_state_->memory()->HostToGuestVirtual(ptr);
      ptr += xe::string_util::copy_and_swap_truncating(
                 ptr, patch_data.at(i), patch_data.at(i).size() + 1) +
             1;
    }
  }
}

uint32_t XamState::GetLanguageTypefacePatch(uint32_t language) const {
  // The console's xam returns the xttp1 patch when the boot loader flags in
  // XboxHardwareInfo have 0x1000, the xttp2 one with 0x2000, and none
  // otherwise. Those flags tell which patch its flash has, so take the patch
  // the system files have here.
  uint32_t index = 0;
  if (language == 17) {  // kSChinese
    index = 1;
  } else if (language == 8) {  // kTChinese
    index = 2;
  } else {
    index = 3;
  }
  static constexpr std::string_view kNames[] = {"XenonSCLatin", "XenonCLatin",
                                                "XenonJKLatin"};
  const std::string_view name = kNames[index - 1];
  auto file_system = kernel_state_->file_system();
  if (file_system->ResolvePath(fmt::format("media:\\{}.xttp1", name))) {
    return language_type_face_patch_[index + 3];
  }
  if (file_system->ResolvePath(fmt::format("media:\\{}.xttp2", name))) {
    return language_type_face_patch_[index];
  }
  return 0;
}

void XamState::LoadIptvServiceName() {
  constexpr uint32_t address = 0x80D20000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(address, 0x78, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    iptv_name_address_ = address;
  }
}

UserProfile* XamState::GetUserProfile(uint32_t user_index) const {
  if (user_index >= XUserMaxUserCount && user_index < XUserIndexLatest) {
    return nullptr;
  }

  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index));
}

UserProfile* XamState::GetUserProfile(uint64_t xuid) const {
  return profile_manager_->GetProfile(xuid);
}

UserProfile* XamState::GetUserProfileLive(uint64_t xuid) const {
  return profile_manager_->GetProfileLive(xuid);
}

bool XamState::IsUserSignedIn(uint32_t user_index) const {
  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index)) !=
         nullptr;
}

bool XamState::IsUserSignedIn(uint64_t xuid) const {
  return GetUserProfile(xuid) != nullptr;
}

void XamState::LoadSpaInfo(const SpaInfo* info) {
  if (!info) {
    return;
  }
  auto loaded = std::make_shared<SpaInfo>(*info);
  loaded->Load();  // No metadata/GCRC lock is held across resource parsing.
  {
    std::lock_guard lock(spa_mutex_);
    if (spa_info_ && *loaded <= *spa_info_) {
      return;
    }
    spa_info_ = loaded;
  }
  user_tracker_->UpdateSpaInfo(loaded);
}

void XamState::SetContentRegisterCallback(uint32_t callback) {
  content_register_callback = callback;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
