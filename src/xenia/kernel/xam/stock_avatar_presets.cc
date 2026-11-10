/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/stock_avatar_presets.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <new>
#include <string>
#include <utility>
#include "xenia/base/mutex.h"
#include "xenia/base/xui_resource.h"
#include "xenia/kernel/xam/system_manifest.h"
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/module_resource.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe::kernel::xam {
namespace {
std::error_code Error(std::errc value) { return std::make_error_code(value); }
constexpr char kDevice[] = "\\Device\\OwnedAvatarPresetSystemExt";
constexpr char kModule[] = "Guide.AvatarMiniCreator.xex";
std::error_code LookupError(resource::XuiPackageStatus status) {
  return Error(status == resource::XuiPackageStatus::kNotFound
                   ? std::errc::no_such_file_or_directory
                   : status == resource::XuiPackageStatus::kUnsupported
                       ? std::errc::operation_not_supported : std::errc::bad_message);
}
}  // namespace
std::error_code StockAvatarPresets::CheckAvailable() const {
  if (global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::device_or_resource_busy);
  }
  return source_ && source_->available() && system_ext_ &&
                 system_ext_->Observe().available
             ? std::error_code{} : Error(std::errc::no_such_device);
}
const StockAvatarPresets::Preset* StockAvatarPresets::Get(uint32_t index) const {
  return index < kCount ? &presets_[index] : nullptr;
}
std::shared_ptr<const StockAvatarPresets> StockAvatarPresets::Capture(
    std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> source,
    std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error=Error(std::errc::device_or_resource_busy); return {};
  }
  if (!source || !source->generation() || !source->available()) {
    error=Error(std::errc::no_such_device); return {};
  }
  try {
    auto ext=source->PartitionRoot(vfs::InstalledSystemSource::Partition::kSystemExt,error);
    if (!ext) { return {}; }
    if (!ext->read_only()) { error=Error(std::errc::permission_denied); return {}; }
    std::vector<uint8_t> manifest;
    error=ext->ReadFile("system.manifest", 1u << 20, manifest);
    if (error) { return {}; }
    const auto version=FindSystemManifestFile(manifest,kModule);
    if (!version) { error=Error(std::errc::no_such_file_or_directory); return {}; }
    vfs::VirtualFileSystem fs;
    auto device=std::make_unique<vfs::HostPathDevice>(
        kDevice,ext->path(),true,false,"FATX",ext);
    if (!device->Initialize() || !fs.RegisterDevice(std::move(device))) {
      error=Error(std::errc::permission_denied); return {};
    }
    std::vector<uint8_t> package;
    const auto path=std::string(kDevice)+"\\"+SystemExtFolderName(*version)+"\\"+kModule;
    const auto read=vfs::ReadModuleResource(fs,path,"avatarmc",{},package,4u << 20);
    if (XFAILED(read)) { error=Error(std::errc::io_error); return {}; }
    auto result=std::shared_ptr<StockAvatarPresets>(new StockAvatarPresets);
    result->source_=std::move(source); result->system_ext_=std::move(ext);
    // Publish no count/source until all twelve original paired records validate.
    for (uint32_t i=0;i<kCount;++i) {
      char name[40];
      std::snprintf(name,sizeof(name),"avatar_metadata%02u.bin",i);
      const auto binary=resource::LookupXuiPackageFile(package,name);
      if (binary.status!=resource::XuiPackageStatus::kFound) {
        error=LookupError(binary.status); return {};
      }
      if (binary.bytes.size()!=kManifestBytes) {
        error=Error(std::errc::bad_message); return {};
      }
      auto& preset=result->presets_[i];
      std::copy(binary.bytes.begin(),binary.bytes.end(),preset.appearance.begin());
      preset.body_type=AvatarManifestBodyType(preset.appearance);
      if (preset.body_type==AvatarBodyType::kUnknown) {
        error=Error(std::errc::bad_message); return {};
      }
      std::snprintf(name,sizeof(name),"avatar64%02u.png",i);
      const auto image=resource::LookupXuiPackageFile(package,name);
      if (image.status!=resource::XuiPackageStatus::kFound) {
        error=LookupError(image.status); return {};
      }
      auto png=TileService::FromOwnedPng({image.bytes.begin(),image.bytes.end()});
      auto decoded=TileService::PrepareTexture(std::move(png),64u*4u,64u);
      if (decoded.status!=X_E_SUCCESS || decoded.width!=64 || decoded.height!=64) {
        error=Error(std::errc::bad_message); return {};
      }
      preset.preview_64_png.assign(image.bytes.begin(),image.bytes.end());
    }
    error=result->CheckAvailable();
    return error ? nullptr : result;
  } catch (const std::bad_alloc&) { error=Error(std::errc::not_enough_memory); return {}; }
}
}  // namespace xe::kernel::xam
