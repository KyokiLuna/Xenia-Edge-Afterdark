/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_managed_picture.h"
#include <utility>
#include <algorithm>
#include "xenia/base/retained_file_metadata.h"
#include "xenia/kernel/xam/profile_picture_codec.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/kernel/native_profile_save_publication.h"
#include "xenia/kernel/native_profile_save_picture_stage.h"
#include "xenia/kernel/native_profile_save_source.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/host_storage_errors.h"
namespace xe::kernel {
namespace {
std::error_code Error(std::errc e) {return std::make_error_code(e);}
bool Same(const std::shared_ptr<vfs::Device>& a,const std::shared_ptr<vfs::Device>& b) {
  return a && b && a.get()==b.get() && !a.owner_before(b) && !b.owner_before(a);
}
}
namespace {
class NativePictureReadEntry final : public vfs::Entry {
 public:
  NativePictureReadEntry(vfs::Device* device,vfs::Entry* root,std::string path,
      const vfs::File::Information& info,std::filesystem::path physical):Entry(device,root,std::move(path)) {
    coordinate_=std::make_shared<CheckpointCoordinate>(CheckpointCoordinate{std::move(physical)});
    attributes_=info.attributes;size_=info.end_of_file;allocation_size_=info.allocation_size;
    create_timestamp_=info.create_timestamp;access_timestamp_=info.access_timestamp;write_timestamp_=info.write_timestamp;
  }
  std::shared_ptr<const CheckpointCoordinate> RetainCheckpointCoordinate() override {return coordinate_;}
  bool allows_namespace_mutation() const override {return false;}
  X_STATUS Open(uint32_t,vfs::File**) override {return X_STATUS_ACCESS_DENIED;}
 private:
  std::shared_ptr<const CheckpointCoordinate> coordinate_;
  bool DeleteEntryInternal(vfs::Entry*) override {return false;}
};
// An independently owned immutable source for the original native CopyFile
// consumer. This File has no write-stage/publication pointer or writer right.
class NativePictureReadFile final : public vfs::File {
 public:
  NativePictureReadFile(uint32_t access,std::unique_ptr<NativePictureReadEntry> entry,
      xam::ProfileGpdReadCapture read,std::shared_ptr<const filesystem::HostStorageSnapshotToken> token,
      Information info,uint64_t allocated)
      :File(access,entry.get()),entry_owner_(std::move(entry)),read_(std::move(read)),token_(std::move(token)),
       info_(info),allocated_(allocated),canonical_(read_.origin.storage->device()) {
    device_owner_->RetainManagedFile();if(device_owner_!=canonical_) {canonical_->RetainManagedFile();}
  }
  ~NativePictureReadFile() override {
    device_owner_->ReleaseManagedFile();if(device_owner_!=canonical_) {canonical_->ReleaseManagedFile();}
    if(checkpoint_close_)checkpoint_close_->data_status.store(X_STATUS_SUCCESS,std::memory_order_release);
  }
  void ObserveCheckpointClose(std::shared_ptr<CheckpointCloseObservation> receipt) override {checkpoint_close_=std::move(receipt);}
  void Destroy() override {delete this;}
  X_STATUS ReadSync(std::span<uint8_t> out,size_t offset,size_t* count) override {
    if(!count) {return X_STATUS_INVALID_PARAMETER;}*count=0;
    if(!(file_access_&(vfs::FileAccess::kGenericRead|vfs::FileAccess::kFileReadData))) {return X_STATUS_ACCESS_DENIED;}
    if(auto error=read_.origin.storage->CheckAvailable()) {return vfs::HostStorageError(error);}
    const auto bytes=token_->bytes();
    if(offset<bytes.size()) {*count=std::min(out.size(),bytes.size()-offset);std::copy_n(bytes.data()+offset,*count,out.data());}
    return X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(std::span<const uint8_t>,size_t,size_t* count) override {
    if(!count) {return X_STATUS_INVALID_PARAMETER;}*count=0;return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t) override {return X_STATUS_ACCESS_DENIED;}
  X_STATUS SetAllocationSize(uint64_t) override {return X_STATUS_ACCESS_DENIED;}
  X_STATUS SetTimes(uint64_t,uint64_t,uint64_t) override {return X_STATUS_ACCESS_DENIED;}
  X_STATUS Flush() override {
    if(auto error=read_.origin.storage->CheckAvailable()) {return vfs::HostStorageError(error);}
    const auto observed=read_.origin.storage->view()->ObserveSnapshotDurability(*token_);
    return vfs::HostStorageError(observed.error?observed.error:observed.durable?std::error_code{}:Error(std::errc::io_error));
  }
  X_STATUS QueryInformation(Information* out) const override {
    if(!out) {return X_STATUS_INVALID_PARAMETER;}
    if(auto error=read_.origin.storage->CheckAvailable()) {return vfs::HostStorageError(error);}
    auto info=info_;const auto status=QueryAllocationSize(&info.allocation_size);
    if(XFAILED(status)) {return status;}*out=info;return X_STATUS_SUCCESS;
  }
  X_STATUS QueryAllocationSize(uint64_t* out) const override {
    if(!out) {return X_STATUS_INVALID_PARAMETER;}
    if(auto error=read_.origin.storage->CheckAvailable()) {return vfs::HostStorageError(error);}
    return vfs::RoundFileAllocationSize(allocated_,
        uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector(),out);
  }
 private:
  std::shared_ptr<CheckpointCloseObservation> checkpoint_close_;
  std::unique_ptr<NativePictureReadEntry> entry_owner_;
  const xam::ProfileGpdReadCapture read_;
  const std::shared_ptr<const filesystem::HostStorageSnapshotToken> token_;
  const Information info_;const uint64_t allocated_;
  const std::shared_ptr<vfs::HostPathDevice> canonical_;
};
}
class NativeProfileSaveManagedPicture::ReadAuthority final : public vfs::ManagedFileOpenAuthority {
 private:
  friend class NativeProfileSaveManagedPicture;
  ReadAuthority(std::weak_ptr<NativeProfileSaveManagedPicture> service,
      std::shared_ptr<xam::ProfileGpdModelAuthority> model,xam::ProfileGpdReadCapture read,
      std::shared_ptr<vfs::Device> device,std::string path)
      :service_(std::move(service)),model_(std::move(model)),read_(std::move(read)),
       requested_(std::move(device)),path_(std::move(path)) {}
  const std::weak_ptr<NativeProfileSaveManagedPicture> service_;
  const std::shared_ptr<xam::ProfileGpdModelAuthority> model_;
  const xam::ProfileGpdReadCapture read_;
  const std::shared_ptr<vfs::Device> requested_;const std::string path_;
  mutable std::atomic<bool> consumed_{false};
};
std::shared_ptr<const vfs::ManagedFileOpenAuthority> NativeProfileSaveManagedPicture::IssueReadForActualOpen(
    xam::ProfileGpdReadCapture read,std::shared_ptr<vfs::Device> requested,std::string path,std::error_code& error) {
  error.clear();const auto model=read.origin.owner.lock();const auto* leaf=NativeProfileSavePictureStage::Leaf(kind_);
  if(global_critical_region::is_held_by_current_thread() || !model || !leaf || !requested || path.empty() ||
      retiring() || read.origin.storage!=model->storage() || !source_->MatchesMetadata(*model->storage()) ||
      !model->IsCurrent(read.origin) || !read.pictures || !read.pictures->contains(leaf)) {
    error=Error(std::errc::operation_canceled);return {};
  }
  const auto token=read.pictures->at(leaf);
  if(!token || token->root()!=read.origin.storage->view() || token->relative().has_parent_path() ||
      !token->ascii_folded_leaf() || !xe::utf8::equal_case(token->relative().generic_string(),leaf)) {
    error=Error(std::errc::invalid_argument);return {};
  }
  return std::shared_ptr<const vfs::ManagedFileOpenAuthority>(new ReadAuthority(
      weak_from_this(),model,std::move(read),std::move(requested),std::move(path)));
}
X_STATUS NativeProfileSaveManagedPicture::OpenRead(const std::shared_ptr<const ReadAuthority>& authority,
    const std::shared_ptr<vfs::Device>& requested,const std::string& path,vfs::FileDisposition disposition,
    uint32_t access,vfs::File** out,vfs::FileAction* action,const vfs::FileOpenContext& context) {
  if(authority->service_.lock().get()!=this || retiring() || !Same(requested,authority->requested_) ||
      !Same(context.expected_device,authority->requested_) || path!=authority->path_ ||
      !source_->Matches(*authority->model_->storage()) || !authority->model_->IsCurrent(authority->read_.origin)) {
    return X_STATUS_ACCESS_DENIED;
  }
  if(disposition!=vfs::FileDisposition::kOpen && disposition!=vfs::FileDisposition::kOpenIf) {return X_STATUS_ACCESS_DENIED;}
  const auto host=std::dynamic_pointer_cast<vfs::HostPathDevice>(requested);const auto& read=authority->read_;
  const auto* leaf=NativeProfileSavePictureStage::Leaf(kind_);const auto token=read.pictures->at(leaf);
  const auto actual=host?host->storage()->directory_identity():nullptr;
  const auto expected=read.origin.storage->view()->directory_identity();
  if(!host || !actual || !expected || !expected->SameEntry(token->relative(),*actual,
      xe::to_path(xe::utf8::fix_path_separators(path,std::filesystem::path::preferred_separator)),true)) {return X_STATUS_ACCESS_DENIED;}
  if(authority->consumed_.exchange(true,std::memory_order_acq_rel)) {return X_STATUS_ACCESS_DENIED;}
  if(!token->exists()) {return X_STATUS_OBJECT_NAME_NOT_FOUND;}
  std::error_code error=read.origin.storage->CheckAvailable();if(error) {return vfs::HostStorageError(error);}
  const bool small=kind_==filesystem::ProfileArtifactKind::kPublic32 || kind_==filesystem::ProfileArtifactKind::kPersonal32 ||
      kind_==filesystem::ProfileArtifactKind::kAvatar32;
  error=xam::ValidateProfilePicturePng(token->bytes(),small?32:64);if(error) {return vfs::HostStorageError(error);}
  vfs::File::Information info{};uint64_t allocated=0;
  {
    // Observe only through the exact retained source. Close this temporary
    // normal descriptor before final snapshot admission to avoid self-conflict.
    auto file=read.origin.storage->view()->OpenFile(token->relative(),filesystem::FileAccess::kGenericRead,error);
    if(!file) {return vfs::HostStorageError(error);}
    const auto observed=file->QueryInfo(error);if(!observed) {return vfs::HostStorageError(error);}
    const auto metadata=filesystem::QueryRetainedFileMetadata(*file,error);if(!metadata) {return vfs::HostStorageError(error);}
    if(!observed->regular || observed->links!=1 || observed->filesystem_id!=token->info().filesystem_id ||
        observed->file_id!=token->info().file_id || observed->size!=token->bytes().size() || metadata->size!=observed->size) {
      return X_STATUS_CANCELLED;
    }
    info.end_of_file=metadata->size;info.access_timestamp=metadata->access_timestamp;
    info.write_timestamp=metadata->write_timestamp;info.change_timestamp=metadata->change_timestamp;
    allocated=metadata->allocated_bytes;info.attributes=vfs::kFileAttributeNormal;
  }
  {
    const auto checked=read.origin.storage->view()->ValidateSnapshotBatch(read.tokens,error);
    if(!checked) {return vfs::HostStorageError(error);}
    const auto found=std::find(read.tokens.begin(),read.tokens.end(),token);
    const auto index=size_t(found-read.tokens.begin());
    if(found==read.tokens.end() || checked->metadata().size()!=read.tokens.size() || !checked->metadata()[index]) {return X_STATUS_UNSUCCESSFUL;}
    if(!authority->model_->IsCurrent(read.origin) || retiring() || read.origin.storage->retiring()) {return X_STATUS_CANCELLED;}
    info.create_timestamp=0; // Generic POSIX ctime is not native creation time.
  }
  const auto rounded=vfs::RoundFileAllocationSize(allocated,
      uint64_t(host->sectors_per_allocation_unit())*host->bytes_per_sector(),&info.allocation_size);
  if(XFAILED(rounded)) {return rounded;}
  auto lock=global_critical_region::AcquireDirect();
  if(retiring() || host->namespace_retiring() || host->storage()->retiring() || read.origin.storage->retiring()) {return X_STATUS_CANCELLED;}
  auto* root=host->ResolvePath("");if(!root) {return X_STATUS_OBJECT_PATH_NOT_FOUND;}
  auto entry=std::make_unique<NativePictureReadEntry>(host.get(),root,path,info,token->root()->path()/token->relative());
  *out=new NativePictureReadFile(access,std::move(entry),read,token,info,allocated);
  *action=vfs::FileAction::kOpened;return X_STATUS_SUCCESS;
}
NativeProfileSaveManagedPicture::Authority::Authority(std::weak_ptr<NativeProfileSaveManagedPicture> service,
    std::shared_ptr<NativeProfileSavePublication> publication,std::shared_ptr<NativeProfileSavePictureStage> stage,
    std::shared_ptr<vfs::Device> requested,std::string path,std::shared_ptr<void> actual_use,bool read_only)
    :service_(std::move(service)),publication_(std::move(publication)),stage_(std::move(stage)),
      requested_(std::move(requested)),path_(std::move(path)),actual_open_use_(std::move(actual_use)),read_only_(read_only) {}
NativeProfileSaveManagedPicture::NativeProfileSaveManagedPicture(
    std::shared_ptr<NativeProfileSaveSource> source,filesystem::ProfileArtifactKind kind,std::filesystem::path path)
    :source_(std::move(source)),kind_(kind),path_(std::move(path)) {}
std::shared_ptr<NativeProfileSaveManagedPicture> NativeProfileSaveManagedPicture::CreateSource(
    std::shared_ptr<xam::ProfileGpdModelAuthority> model,filesystem::ProfileArtifactKind kind,std::error_code& error,
    std::shared_ptr<const std::atomic<bool>> family_closed) {
  error.clear();const auto* leaf=NativeProfileSavePictureStage::Leaf(kind);
  if(global_critical_region::is_held_by_current_thread() || !model || !leaf) {error=Error(std::errc::invalid_argument);return {};}
  const auto read=model->CaptureRead(error);if(!read) {return {};}
  if(!read->pictures || !read->pictures->contains(leaf)) {error=Error(std::errc::invalid_argument);return {};}
  const auto token=read->pictures->at(leaf);
  if(!token || token->root()!=model->storage()->view() || token->relative().has_parent_path() ||
      !token->ascii_folded_leaf() || !xe::utf8::equal_case(token->relative().generic_string(),leaf)) {
    error=Error(std::errc::invalid_argument);return {};
  }
  const auto path=(token->root()->path()/token->relative()).lexically_normal();
  auto source=NativeProfileSaveSource::Capture(model->storage(),error,std::move(family_closed));if(!source) {return {};}
  if((error=source->JoinCommittedReadModel(model)))return {};
  return std::shared_ptr<NativeProfileSaveManagedPicture>(new NativeProfileSaveManagedPicture(std::move(source),kind,path));
}
std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
NativeProfileSaveManagedPicture::backing_directory() const {return source_->parent_;}
bool NativeProfileSaveManagedPicture::retiring() const {return source_->retiring();}
std::shared_ptr<const vfs::ManagedFileOpenAuthority> NativeProfileSaveManagedPicture::IssueForActualOpen(
    const std::shared_ptr<NativeProfileSavePublication>& publication,std::shared_ptr<vfs::Device> requested,
    std::string path,std::error_code& error) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || !publication || !requested || path.empty() || retiring() ||
      (!publication->WriterForStage().model || !source_->MatchesMetadata(*publication->WriterForStage().model->storage()))) {error=Error(std::errc::operation_canceled);return {};}
  std::unique_lock serial(publication->mutex_,std::try_to_lock);
  if(!serial.owns_lock()) {error=Error(std::errc::device_or_resource_busy);return {};}
  if(publication->canceled_ || publication->sealed_ || !publication->CurrentForStage(error)) {
    if(!error) {error=Error(std::errc::operation_canceled);}return {};
  }
  const auto found=publication->picture_stages_.find(kind_);
  if(found==publication->picture_stages_.end() || !found->second || found->second->publication_.lock()!=publication) {
    error=Error(std::errc::invalid_argument);return {};
  }
  // Preparatory metadata only; OpenRead/OpenWithContext still prove physical
  // source, exact target token/path and currentness on the actual IO worker.
  return std::shared_ptr<const vfs::ManagedFileOpenAuthority>(new Authority(
      weak_from_this(),publication,found->second,std::move(requested),std::move(path)));
}
std::shared_ptr<const vfs::ManagedFileOpenAuthority> NativeProfileSaveManagedPicture::IssueParentForActualOpen(
    const std::shared_ptr<NativeProfileSavePublication>& publication,std::shared_ptr<vfs::Device> requested,
    std::string path,std::shared_ptr<void> actual_use,std::error_code& error) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || !publication || !publication->parent_operation_ ||
      !requested || path.empty() || !actual_use || retiring() ||
      !source_->MatchesMetadata(*publication->WriterForStage().model->storage()) ||
      !publication->system_issuer_ || std::find(publication->system_issuer_->required_artifacts.begin(),
        publication->system_issuer_->required_artifacts.end(),kind_)==publication->system_issuer_->required_artifacts.end() ||
      !publication->CurrentForStage(error)) {
    if(!error)error=Error(std::errc::operation_canceled);return {};
  }
  return std::shared_ptr<const vfs::ManagedFileOpenAuthority>(new Authority(
      weak_from_this(),publication,{},std::move(requested),std::move(path),std::move(actual_use)));
}
std::shared_ptr<const vfs::ManagedFileOpenAuthority> NativeProfileSaveManagedPicture::IssueParentStageReadForActualOpen(
    const std::shared_ptr<NativeProfileSavePublication>& publication,std::shared_ptr<vfs::Device> requested,
    std::string path,std::shared_ptr<void> actual_use,std::error_code& error) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || !publication || !publication->parent_operation_ ||
      !requested || path.empty() || !actual_use || retiring() ||
      !source_->MatchesMetadata(*publication->WriterForStage().model->storage())) {
    error=Error(std::errc::operation_canceled);return {};
  }
  std::unique_lock serial(publication->mutex_,std::try_to_lock);
  if(!serial.owns_lock()){error=Error(std::errc::device_or_resource_busy);return {};}
  if(publication->canceled_ || publication->sealed_ || !publication->CurrentForStage(error)) {
    if(!error)error=Error(std::errc::operation_canceled);return {};
  }
  // Actual native CopyFile/read MUST consume the prior original PNG output,
  // not the unchanged canonical before file hidden behind this reservation.
  // No source-only open creates a writer stage or chooses another source.
  const auto stage=publication->picture_stages_.find(kind_);
  if(stage==publication->picture_stages_.end() || !stage->second ||
      stage->second->publication_.lock()!=publication) {
    error=Error(std::errc::no_such_file_or_directory);return {};
  }
  return std::shared_ptr<const vfs::ManagedFileOpenAuthority>(new Authority(
      weak_from_this(),publication,stage->second,std::move(requested),std::move(path),std::move(actual_use),true));
}
std::error_code NativeProfileSaveManagedPicture::JoinCommittedReadModel(
    const std::shared_ptr<xam::ProfileGpdModelAuthority>& model) {
  return source_->JoinCommittedReadModel(model);
}
X_STATUS NativeProfileSaveManagedPicture::Open(std::shared_ptr<vfs::Device> device,std::string path,
    vfs::FileDisposition disposition,uint32_t access,bool directory,vfs::File** file,vfs::FileAction* action) {
  return OpenCommitted(std::move(device),std::move(path),disposition,access,directory,file,action,{});
}
X_STATUS NativeProfileSaveManagedPicture::OpenCommitted(std::shared_ptr<vfs::Device> device,std::string path,
    vfs::FileDisposition disposition,uint32_t access,bool directory,vfs::File** file,vfs::FileAction* action,
    const vfs::FileOpenContext& context) {
  if(!file || !action)return X_STATUS_INVALID_PARAMETER;
  *file=nullptr;*action=vfs::FileAction::kDoesNotExist;
  if(directory)return X_STATUS_NOT_A_DIRECTORY;
  if(context.managed_authority || !device || device->namespace_retiring() ||
      (disposition!=vfs::FileDisposition::kOpen && disposition!=vfs::FileDisposition::kOpenIf))return X_STATUS_ACCESS_DENIED;
  std::error_code error;auto read=source_->CaptureCommittedFileRead(error);
  if(!read)return vfs::HostStorageError(error);
  // The common VFS has already authenticated the exact registered device and
  // any ancestor constraint before selecting this existing physical-leaf policy.
  // Strengthen only this local read view to that actual device; never weaken an
  // incoming issued authority or turn requested RW bits into backing permission.
  auto local=context;local.expected_device=device;
  local.managed_authority=IssueReadForActualOpen(std::move(*read),device,path,error);
  if(!local.managed_authority)return vfs::HostStorageError(error);
  return OpenRead(std::dynamic_pointer_cast<const ReadAuthority>(local.managed_authority),
      device,path,disposition,access,file,action,local);
}
X_STATUS NativeProfileSaveManagedPicture::OpenWithContext(std::shared_ptr<vfs::Device> requested,
    std::string path,vfs::FileDisposition disposition,uint32_t access,bool directory,
    vfs::File** file,vfs::FileAction* action,const vfs::FileOpenContext& context) {
  if(!file || !action) {return X_STATUS_INVALID_PARAMETER;}*file=nullptr;*action=vfs::FileAction::kDoesNotExist;
  if(directory) {return X_STATUS_NOT_A_DIRECTORY;}
  if(global_critical_region::is_held_by_current_thread()) {return X_STATUS_INVALID_DEVICE_REQUEST;}
  if(uint32_t(disposition)>uint32_t(vfs::FileDisposition::kOverwriteIf)) {return X_STATUS_INVALID_PARAMETER;}
  if(!context.managed_authority)return OpenCommitted(requested,path,disposition,access,directory,file,action,context);
  if(const auto read=std::dynamic_pointer_cast<const ReadAuthority>(context.managed_authority)) {
    return OpenRead(read,requested,path,disposition,access,file,action,context);
  }
  const auto authority=std::dynamic_pointer_cast<const Authority>(context.managed_authority);
  if(!authority || authority->service_.lock().get()!=this || retiring() ||
      (!authority->publication_->WriterForStage().model || !source_->Matches(*authority->publication_->WriterForStage().model->storage())) ||
      (authority->stage_ && (authority->stage_->kind_!=kind_ ||
       authority->stage_->publication_.lock()!=authority->publication_)) || !Same(requested,authority->requested_) ||
      !Same(context.expected_device,authority->requested_) || path!=authority->path_) {return X_STATUS_ACCESS_DENIED;}
  if(authority->read_only_) {
    constexpr uint32_t mutation_access=vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericAll|0x000D0156;
    if(!authority->stage_ || (access&mutation_access) ||
        (disposition!=vfs::FileDisposition::kOpen && disposition!=vfs::FileDisposition::kOpenIf))return X_STATUS_ACCESS_DENIED;
  }
  const auto host=std::dynamic_pointer_cast<vfs::HostPathDevice>(requested);
  if(!host || host->is_read_only() || host->storage()->retiring()) {return X_STATUS_ACCESS_DENIED;}
  std::error_code error;if(!authority->publication_->CurrentForStage(error)) {return vfs::HostStorageError(error);}
  if(authority->consumed_.exchange(true,std::memory_order_acq_rel)) {return X_STATUS_ACCESS_DENIED;}
  auto stage=authority->stage_;
  if(!stage) {
    // Physical first-open work remains inside this EXISTING owned IO22 call.
    // It is forbidden to create even a PNG stage before the whole parent05
    // Header/main admission succeeds. A failed created admission stays owned.
    error=authority->publication_->PrepareParentArtifactsAtOriginalIo();
    if(error)return vfs::HostStorageError(error);
    stage=authority->publication_->CreateManagedPictureStage(kind_,error);
    if(!stage || error)return vfs::HostStorageError(error?error:Error(std::errc::io_error));
  }
  error=stage->ValidateRequestedView(host,path);if(error) {return vfs::HostStorageError(error);}
  // The actual stage serializes logical existence + disposition + File open.
  // Its original-before token remains reserved for final CAS, never reused as
  // current guest existence after a new native output has been created.
  return stage->OpenManagedView(host,std::move(path),disposition,access,file,action);
}
}  // namespace xe::kernel
