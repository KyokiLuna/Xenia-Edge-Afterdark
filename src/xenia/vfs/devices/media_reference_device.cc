/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/vfs/devices/media_reference_device.h"
#include <limits>
#include <optional>
#include "xenia/base/string.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/host_storage_errors.h"
namespace xe::vfs {
namespace {
class MediaEntry final : public Entry {
 public:
  MediaEntry(Device* d,Entry* p,std::string_view path,
             std::optional<size_t> index,uint64_t size=0)
      :Entry(d,p,path),index_(index) {
    attributes_=kFileAttributeReadOnly|(index?kFileAttributeNormal:kFileAttributeDirectory);
    size_=index ? size_t(size) : 0;
  }
  bool allows_namespace_mutation() const override { return false; }
  bool has_open_files() const override {
    if(open_.load())return true;for(const auto& c:children_)if(c->has_open_files())return true;return false;
  }
  void RetainFile() override {open_.fetch_add(1);}
  void ReleaseFile() override {open_.fetch_sub(1);}
  MediaEntry* Add(std::string_view path,std::optional<size_t> index,uint64_t bytes) {
    auto e=std::make_unique<MediaEntry>(device_,this,path,index,bytes);auto* result=e.get();children_.push_back(std::move(e));return result;
  }
  X_STATUS Open(uint32_t access,File** output) override;
  bool DeleteEntryInternal(Entry*) override {return false;}
  X_STATUS
  RenameEntryInternal(Entry *, std::string_view,
                      std::shared_ptr<const filesystem::HostStorageFileLease>,
                      bool *) override {
    return X_STATUS_ACCESS_DENIED;
  }

 private:
  std::optional<size_t> index_;
  std::atomic<size_t> open_{0};
};
class MediaFile final : public File {
 public:
  MediaFile(uint32_t a,MediaEntry* e,std::optional<size_t> i)
      :File(a,e),index_(i) {e->RetainFile();}
  ~MediaFile() override {entry_->ReleaseFile();}
  void Destroy() override {delete this;}
  X_STATUS ReadSync(std::span<uint8_t> bytes,size_t offset,size_t* transferred) override {
    *transferred=0;
    if(!index_)return X_STATUS_FILE_IS_A_DIRECTORY;
    if(!(file_access_&(filesystem::FileAccess::kGenericRead|filesystem::FileAccess::kFileReadData)))return X_STATUS_ACCESS_DENIED;
    if(!device_owner_ || device_owner_->namespace_retiring())return X_STATUS(0xC000009D);
    const auto& source=static_cast<MediaReferenceDevice*>(device_owner_.get())->selection();
    const auto result=source->Read(*index_,offset,bytes);*transferred=result.bytes;return HostStorageError(result.error);
  }
  X_STATUS WriteSync(std::span<const uint8_t>,size_t,size_t* n) override {*n=0;return X_STATUS_ACCESS_DENIED;}
  X_STATUS QueryAllocationSize(uint64_t* output) const override {
    if(!output)return X_STATUS_INVALID_PARAMETER;
    if(!index_)return X_STATUS_NOT_SUPPORTED;
    if(!device_owner_ || device_owner_->namespace_retiring())return X_STATUS(0xC000009D);
    std::error_code error;const auto bytes=static_cast<MediaReferenceDevice*>(device_owner_.get())->selection()->QueryAllocatedBytes(*index_,error);
    if(!bytes)return HostStorageError(error);
    return RoundFileAllocationSize(*bytes,uint64_t(device_owner_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector(),output);
  }
 private:
  std::optional<size_t> index_;
};
X_STATUS MediaEntry::Open(uint32_t access,File** output) {
  *output=nullptr;
  constexpr uint32_t read=filesystem::FileAccess::kGenericRead|
      filesystem::FileAccess::kGenericExecute|filesystem::FileAccess::kFileReadData|
      0x8u|0x20u|0x80u|0x20000u|0x100000u;
  if(access&~read)return X_STATUS_ACCESS_DENIED;
  if(device_->namespace_retiring())return X_STATUS(0xC000009D);
  *output=new MediaFile(access,this,index_);return X_STATUS_SUCCESS;
}
}
MediaReferenceDevice::MediaReferenceDevice(std::string_view path,
    std::shared_ptr<filesystem::MediaSelection> selection)
    :Device(path),selection_(std::move(selection)){}
bool MediaReferenceDevice::Initialize() {
  if(root_ || !selection_ || selection_->folder() || !selection_->Observe().available)return false;
  auto root=std::make_unique<MediaEntry>(this,nullptr,"",std::nullopt);
  const auto rows=selection_->entries();
  for(size_t n=0;n<rows.size();++n) {
    if(rows[n].bytes>SIZE_MAX)return false;
    auto* parent=root.get();std::string full;
    const auto parts=xe::utf8::split_path(rows[n].path);
    for(size_t i=0;i<parts.size();++i) {
      if(!full.empty())full+='\\';full+=parts[i];
      auto* child=static_cast<MediaEntry*>(parent->GetChild(parts[i]));
      if(!child)child=parent->Add(full,i+1==parts.size()?std::optional<size_t>(n):std::nullopt,rows[n].bytes);
      parent=child;
    }
  }
  root_=std::move(root);return true;
}
std::optional<std::vector<filesystem::GuestHostBacking>> MediaReferenceDevice::GuestBackings() const {
  if(!selection_)return std::nullopt;std::error_code error;auto result=selection_->GuestBackings(error);return error?std::nullopt:std::optional(std::move(result));
}
void MediaReferenceDevice::Dump(StringBuffer* out) {auto lock=global_critical_region_.Acquire();if(root_)root_->Dump(out,0);}
Entry* MediaReferenceDevice::ResolvePath(std::string_view path) {
  if(namespace_retiring() || !root_)return nullptr;return path.empty()?root_.get():root_->ResolvePath(path);
}
void MediaReferenceDevice::Retire() {BeginNamespaceRetirement();if(selection_)selection_->Retire();}
bool MediaReferenceDevice::has_open_files() const {return root_ && root_->has_open_files();}
filesystem::HostStorageSnapshot MediaReferenceDevice::storage_space() const {return selection_?selection_->Observe():filesystem::HostStorageSnapshot{};}
}  // namespace xe::vfs
