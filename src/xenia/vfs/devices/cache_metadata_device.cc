/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/vfs/devices/cache_metadata_device.h"
#include <algorithm>
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/base/string.h"
#include "xenia/vfs/file.h"
namespace xe::vfs {
namespace {
constexpr uint64_t kTableOffset=0x800;
X_STATUS Removed(){return X_STATUS(0xC000009D);}
}
class CacheMetadataDevice::CacheEntry final : public Entry {
 public:
  CacheEntry(CacheMetadataDevice* device,Entry* parent,std::string path,int role)
      :Entry(device,parent,std::move(path)),role_(role){
    attributes_=parent?kFileAttributeNormal:kFileAttributeDirectory;
    // No fictitious whole-partition EOF/capacity. The table window is checked
    // per request; adjacent raw ranges remain unsupported.
    size_=allocation_size_=0;
  }
  X_STATUS Open(uint32_t,File**) override;
  bool can_map() const override {return false;}
  bool has_open_files() const override {
    if(files_.load(std::memory_order_acquire))return true;
    return std::any_of(children_.begin(),children_.end(),[](const auto& c){return c->has_open_files();});
  }
  void RetainFile() override {files_.fetch_add(1,std::memory_order_acq_rel);}
  void ReleaseFile() override {files_.fetch_sub(1,std::memory_order_acq_rel);}
  bool DeleteEntryInternal(Entry*) override {return false;}
  const int role_;
  std::atomic<size_t> files_{0};
  friend class CacheMetadataDevice;
};
class CacheMetadataDevice::CacheFile final : public File {
 public:
  CacheFile(uint32_t access,CacheEntry* entry,
      std::shared_ptr<filesystem::CacheStorageOwner::MetadataFileUse> use)
      :File(access,entry),entry_cache_(entry),use_(std::move(use)){entry_->RetainFile();}
  ~CacheFile() override {entry_->ReleaseFile();}
  void Destroy() override {delete this;}
  X_STATUS ReadSync(std::span<uint8_t> out,size_t offset,size_t* transferred) override {
    if(!transferred)return X_STATUS_INVALID_PARAMETER;*transferred=0;
    const auto& device=*static_cast<CacheMetadataDevice*>(entry_cache_->device());
    if(!device.available())return Removed();
    if(!(file_access_&filesystem::FileAccess::kFileReadData))return X_STATUS_ACCESS_DENIED;
    if(entry_cache_->role_!=0 || offset<kTableOffset ||
        offset-kTableOffset>filesystem::CacheStorageOwner::kTableBytes ||
        out.size()>filesystem::CacheStorageOwner::kTableBytes-(offset-kTableOffset))return X_STATUS_NOT_SUPPORTED;
    const auto error=device.owner_->ReadTable(offset-kTableOffset,out);
    if(error)return HostStorageError(error);*transferred=out.size();return X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(std::span<const uint8_t> bytes,size_t offset,size_t* transferred) override {
    if(!transferred)return X_STATUS_INVALID_PARAMETER;*transferred=0;
    const auto& device=*static_cast<CacheMetadataDevice*>(entry_cache_->device());
    if(!device.available())return Removed();
    if(!(file_access_&filesystem::FileAccess::kFileWriteData))return X_STATUS_ACCESS_DENIED;
    if(entry_cache_->role_!=0 || offset<kTableOffset ||
        offset-kTableOffset>filesystem::CacheStorageOwner::kTableBytes ||
        bytes.size()>filesystem::CacheStorageOwner::kTableBytes-(offset-kTableOffset))return X_STATUS_NOT_SUPPORTED;
    const auto receipt=device.owner_->WriteTable(offset-kTableOffset,bytes);
    if(receipt.error)return HostStorageError(receipt.error);
    if(!receipt.complete || !receipt.table_durable)return X_STATUS_UNSUCCESSFUL;
    *transferred=bytes.size();return X_STATUS_SUCCESS;
  }
  X_STATUS SetLength(size_t) override {
    const auto& device=*static_cast<CacheMetadataDevice*>(entry_cache_->device());
    if(!device.available())return Removed();
    return (file_access_&filesystem::FileAccess::kFileWriteData)?X_STATUS_NOT_SUPPORTED:X_STATUS_ACCESS_DENIED;
  }
  X_STATUS Flush() override {
    if(entry_cache_->role_!=0)return X_STATUS_NOT_SUPPORTED;
    const auto& device=*static_cast<CacheMetadataDevice*>(entry_cache_->device());
    if(!device.available())return Removed();
    const auto error=device.owner_->FlushTable();return error?HostStorageError(error):X_STATUS_SUCCESS;
  }
 private:
  CacheEntry* const entry_cache_;
  const std::shared_ptr<filesystem::CacheStorageOwner::MetadataFileUse> use_;
};
X_STATUS CacheMetadataDevice::CacheEntry::Open(uint32_t access,File** output) {
  if(!output)return X_STATUS_INVALID_PARAMETER;
  const auto& parent=*static_cast<CacheMetadataDevice*>(device());
  if(!parent.available())return Removed();
  auto use=parent.owner_->AcquireMetadataFile();if(!use)return HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));
  auto file=std::make_unique<CacheFile>(access,this,std::move(use));
  *output=file.release();return X_STATUS_SUCCESS;
}
CacheMetadataDevice::CacheMetadataDevice(std::shared_ptr<filesystem::CacheStorageOwner> owner,
    std::array<std::shared_ptr<filesystem::HostStorageRoot>,3> roots)
    :Device(R"(\Device\Harddisk0)"),owner_(std::move(owner)),roots_(std::move(roots)){}
CacheMetadataDevice::~CacheMetadataDevice()=default;
std::unique_ptr<CacheMetadataDevice> CacheMetadataDevice::Create(
    std::shared_ptr<filesystem::CacheStorageOwner> owner,
    std::shared_ptr<const filesystem::ConsoleStorageVolume> volume,std::error_code& error) {
  error.clear();if(!owner || !volume || !volume->available_cached() || !volume->generation()){
    error=std::make_error_code(std::errc::permission_denied);return {};}
  const auto identity=owner->CaptureIdentity(filesystem::CacheStorageOwner::Role::kLegacy);
  if(!identity || volume->source_id()!=identity->source_id() || volume->device_id()!=identity->device_id() ||
      volume->type()!=filesystem::ConsoleStorageType::HDD){error=std::make_error_code(std::errc::permission_denied);return {};}
  std::array<std::shared_ptr<filesystem::HostStorageRoot>,3> roots;
  for(size_t i=0;i<3;++i){roots[i]=owner->CaptureRoot(filesystem::CacheStorageOwner::Role(i),error);
    if(!roots[i] || error)return {};}
  auto result=std::unique_ptr<CacheMetadataDevice>(new CacheMetadataDevice(std::move(owner),std::move(roots)));
  if(!result->SetStorageVolume(std::move(volume)) || !result->Initialize()){
    error=std::make_error_code(std::errc::permission_denied);return {};}
  return result;
}
bool CacheMetadataDevice::Initialize(){
  if(root_ || !available())return false;
  root_=std::make_unique<CacheEntry>(this,nullptr,mount_path_,-1);
  for(const auto& [path,role]:{std::pair{R"(\Partition0)",0},std::pair{R"(\Cache0)",1},std::pair{R"(\Cache1)",2}})
    root_->children_.push_back(std::make_unique<CacheEntry>(this,root_.get(),path,role));
  return true;
}
bool CacheMetadataDevice::available() const noexcept {
  return !retired_.load(std::memory_order_acquire) && owner_->current_cached() &&
      std::all_of(roots_.begin(),roots_.end(),[](const auto& root){return root && !root->retiring();});
}
std::optional<std::vector<filesystem::GuestHostBacking>> CacheMetadataDevice::GuestBackings() const {
  std::vector<filesystem::GuestHostBacking> backings;backings.reserve(roots_.size());
  for(const auto& root:roots_)backings.push_back(root->GuestBacking());return backings;
}
Entry* CacheMetadataDevice::ResolvePath(std::string_view path){
  auto lock=global_critical_region_.Acquire();if(!available() || !root_)return nullptr;
  if(path.empty())return root_.get();
  for(const auto& child:root_->children())if(xe::utf8::equal_case(child->path(),path))return child.get();return nullptr;
}
void CacheMetadataDevice::Dump(StringBuffer* out){auto lock=global_critical_region_.Acquire();if(root_)root_->Dump(out,0);}
bool CacheMetadataDevice::has_open_files() const {return root_ && root_->has_open_files();}
filesystem::HostStorageSnapshot CacheMetadataDevice::storage_space() const {
  filesystem::HostStorageSnapshot value;
  value.error=std::make_error_code(available()?std::errc::operation_not_supported:std::errc::no_such_device);return value;
}
} // namespace xe::vfs
