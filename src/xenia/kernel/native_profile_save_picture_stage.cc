/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_picture_stage.h"
#include <utility>
#include <algorithm>
#include <new>
#include <random>
#include "xenia/base/mutex.h"
#include "xenia/base/clock.h"
#include "xenia/base/string.h"
#include "xenia/kernel/native_profile_save_publication.h"
#include "xenia/kernel/xam/profile_picture_codec.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/host_storage_errors.h"
namespace xe::kernel {
namespace {
std::error_code Error(std::errc e) {return std::make_error_code(e);}
std::filesystem::path StageLeaf() {
  constexpr char hex[]="0123456789abcdef";std::string leaf="snapshot-";std::random_device random;
  for(unsigned i=0;i<4;++i) {const uint32_t value=random();for(unsigned j=0;j<8;++j) {leaf.push_back(hex[(value>>(j*4))&15]);}}
  return leaf+".bin";
}
class NativePictureEntry final : public vfs::Entry {
 public:
  NativePictureEntry(vfs::Device* device,vfs::Entry* root,std::string path,
      const vfs::File::Information& info,std::filesystem::path physical):Entry(device,root,path) {
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
}
class NativeProfileSavePictureFile final : public vfs::File {
  friend class NativeProfileSavePictureStage;
 public:
  NativeProfileSavePictureFile(uint32_t access,std::unique_ptr<NativePictureEntry> entry,
      std::shared_ptr<NativeProfileSavePictureStage> stage)
      :File(access,entry.get()),entry_owner_(std::move(entry)),stage_(std::move(stage)),canonical_(stage_->storage_->device()) {
    device_owner_->RetainManagedFile();if(device_owner_!=canonical_) {canonical_->RetainManagedFile();}
    stage_->managed_views_.fetch_add(1,std::memory_order_acq_rel);
    if(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericAll|
        vfs::FileAccess::kFileWriteData|vfs::FileAccess::kFileAppendData|vfs::FileAccess::kFileWriteAttributes))
      stage_->managed_writer_views_.fetch_add(1,std::memory_order_acq_rel);
  }
  ~NativeProfileSavePictureFile() override {
    device_owner_->ReleaseManagedFile();if(device_owner_!=canonical_) {canonical_->ReleaseManagedFile();}
    stage_->managed_views_.fetch_sub(1,std::memory_order_acq_rel);
    if(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericAll|
        vfs::FileAccess::kFileWriteData|vfs::FileAccess::kFileAppendData|vfs::FileAccess::kFileWriteAttributes))
      stage_->managed_writer_views_.fetch_sub(1,std::memory_order_acq_rel);
    if(checkpoint_close_)checkpoint_close_->data_status.store(X_STATUS_SUCCESS,std::memory_order_release);
  }
  void ObserveCheckpointClose(std::shared_ptr<CheckpointCloseObservation> receipt) override {checkpoint_close_=std::move(receipt);}
  void Destroy() override {delete this;}
  X_STATUS ReadSync(std::span<uint8_t> bytes,size_t offset,size_t* out) override {
    if(!out) {return X_STATUS_INVALID_PARAMETER;}*out=0;
    if(!(file_access_&(vfs::FileAccess::kGenericRead|vfs::FileAccess::kFileReadData))) {return X_STATUS_ACCESS_DENIED;}
    const auto result=stage_->Read(offset,bytes);*out=result.bytes;return vfs::HostStorageError(result.error);
  }
  X_STATUS WriteSync(std::span<const uint8_t> bytes,size_t offset,size_t* out) override {
    if(!out) {return X_STATUS_INVALID_PARAMETER;}*out=0;
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kFileWriteData|vfs::FileAccess::kFileAppendData))) {return X_STATUS_ACCESS_DENIED;}
    const auto result=stage_->Write(offset,bytes);*out=result.bytes;return vfs::HostStorageError(result.error);
  }
  X_STATUS SetLength(size_t length) override {
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kFileWriteData))) {return X_STATUS_ACCESS_DENIED;}
    return vfs::HostStorageError(stage_->Resize(length));
  }
  X_STATUS SetTimes(uint64_t creation,uint64_t access,uint64_t write) override {
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericAll|vfs::FileAccess::kFileWriteAttributes)))
      return X_STATUS_ACCESS_DENIED;
    return vfs::HostStorageError(stage_->SetTimes(creation,access,write));
  }
  X_STATUS Flush() override {return vfs::HostStorageError(stage_->Flush());}
  X_STATUS QueryInformation(Information* out) const override {
    if(!out) {return X_STATUS_INVALID_PARAMETER;}std::error_code error;const auto actual=stage_->Query(error);
    if(!actual) {return stage_->retired()?X_STATUS_INVALID_HANDLE:vfs::HostStorageError(error);}
    Information info{};info.attributes=vfs::kFileAttributeNormal;info.create_timestamp=stage_->creation_timestamp_;
    info.end_of_file=actual->size;info.access_timestamp=actual->access_timestamp;
    info.write_timestamp=actual->write_timestamp;info.change_timestamp=actual->change_timestamp;
    const uint64_t unit=uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector();
    const auto status=vfs::RoundFileAllocationSize(actual->allocated_bytes,unit,&info.allocation_size);
    if(XFAILED(status)) {return status;}*out=info;return X_STATUS_SUCCESS;
  }
  X_STATUS QueryAllocationSize(uint64_t* out) const override {
    if(!out) {return X_STATUS_INVALID_PARAMETER;}std::error_code error;const auto actual=stage_->Query(error);
    if(!actual) {return stage_->retired()?X_STATUS_INVALID_HANDLE:vfs::HostStorageError(error);}
    const uint64_t unit=uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector();
    return vfs::RoundFileAllocationSize(actual->allocated_bytes,unit,out);
  }
 private:
  std::shared_ptr<CheckpointCloseObservation> checkpoint_close_;
  std::unique_ptr<NativePictureEntry> entry_owner_;
  std::shared_ptr<NativeProfileSavePictureStage> stage_;
  std::shared_ptr<vfs::HostPathDevice> canonical_;
};
bool NativeProfileSavePictureStage::MatchesOwnedFile(const vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication,filesystem::ProfileArtifactKind kind) {
  const auto* actual=dynamic_cast<const NativeProfileSavePictureFile*>(&file);
  return actual && actual->stage_ && actual->stage_->kind_==kind &&
      actual->stage_->publication_.lock()==publication;
}
std::shared_ptr<const NativeProfilePictureCheckpoint>
NativeProfileSavePictureStage::CaptureOwnedFileCheckpoint(const vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    filesystem::ProfileArtifactKind kind,std::error_code& error) {
  error.clear();
  const auto* actual=dynamic_cast<const NativeProfileSavePictureFile*>(&file);
  if(!actual || !actual->stage_ || actual->stage_->kind_!=kind ||
      !publication || actual->stage_->publication_.lock()!=publication) {
    error=Error(std::errc::invalid_argument);return {};
  }
  return actual->stage_->CaptureWriteCheckpoint(error);
}
bool NativeProfilePictureCheckpoint::CheckCurrent(std::error_code& error) const {
  error.clear();const auto stage=stage_.lock();
  if(!stage) {error=Error(std::errc::operation_canceled);return false;}
  return stage->CheckCheckpoint(*this,error);
}
bool NativeProfileSavePictureStage::CheckCheckpoint(
    const NativeProfilePictureCheckpoint& checkpoint,std::error_code& error) {
  error.clear();std::unique_lock lock(mutex_,std::try_to_lock);
  if(!lock.owns_lock()) {error=Error(std::errc::device_or_resource_busy);return false;}
  // Metadata-only currentness. Physical identity/availability is checked by
  // real capture/seal IO and the map's final full-member snapshot guard; do
  // not repeat stat/space observations on every retained-checkpoint check.
  if(global_critical_region::is_held_by_current_thread()) {
    error=Error(std::errc::operation_would_block);return false;
  }
  if(initialization_error_) {error=initialization_error_;return false;}
  if(retired_.load(std::memory_order_acquire) || !stage_ || !storage_ || storage_->retiring()) {
    error=Error(std::errc::operation_canceled);return false;
  }
  const auto owner=publication_.lock();
  if(!owner || !owner->CurrentForStage(error)) {
    if(!error)error=Error(std::errc::operation_canceled);return false;
  }
  if(checkpoint.stage_.lock().get()!=this || before_!=checkpoint.before_ || kind_!=checkpoint.kind_ ||
      output_revision_!=checkpoint.revision_ ||
      !flushed_revision_ || *flushed_revision_!=checkpoint.revision_) {
    error=Error(std::errc::state_not_recoverable);return false;
  }
  return true;
}
const char* NativeProfileSavePictureStage::Leaf(filesystem::ProfileArtifactKind kind) {
  using Kind=filesystem::ProfileArtifactKind;
  switch(kind) {
    case Kind::kPublic64:return "tile_64.png";case Kind::kPublic32:return "tile_32.png";
    case Kind::kPersonal64:return "pp_64.png";case Kind::kPersonal32:return "pp_32.png";
    case Kind::kAvatar64:return "avtr_64.png";case Kind::kAvatar32:return "avtr_32.png";
    default:return nullptr; // GPD and CM thumbnail have distinct typed owners.
  }
}
std::shared_ptr<NativeProfileSavePictureStage> NativeProfileSavePictureStage::Create(
    std::shared_ptr<NativeProfileSavePublication> publication,filesystem::ProfileArtifactKind kind,
    std::error_code& error) {
  error.clear();const auto* leaf=Leaf(kind);
  if(global_critical_region::is_held_by_current_thread() || !publication || !leaf) {error=Error(std::errc::invalid_argument);return {};}
  if(!publication->CurrentForDataStage(error)) {return {};}
  const auto plan=publication->pending_->BuildPlan(error);if(!plan) {return {};}
  const auto found=std::find_if(plan->pictures.begin(),plan->pictures.end(),[&](const auto& member){return member.kind==kind;});
  const bool system_output=publication->system_issuer_ &&
      std::find(publication->system_issuer_->required_artifacts.begin(),publication->system_issuer_->required_artifacts.end(),kind)
          !=publication->system_issuer_->required_artifacts.end();
  if(!system_output && (found==plan->pictures.end() || found->title_id ||
      (found->action!=filesystem::ProfileArtifactAction::kCreate && found->action!=filesystem::ProfileArtifactAction::kReplace))) {
    error=Error(std::errc::operation_not_supported);return {};
  }
  const auto before=publication->pending_->pictures_before();
  if(!before || !before->contains(leaf) || !before->at(leaf)) {error=Error(std::errc::invalid_argument);return {};}
  const auto& writer=publication->WriterForStage();const auto storage=publication->OriginForStage().storage;
  const auto token=before->at(leaf);
  if(token->root()!=storage->view() || token->relative().has_parent_path() || !token->ascii_folded_leaf() ||
      !xe::utf8::equal_case(token->relative().generic_string(),leaf) || !writer.recovery || writer.recovery->error()) {
    error=writer.recovery && writer.recovery->error()?writer.recovery->error():Error(std::errc::invalid_argument);return {};
  }
  error=writer.recovery->CheckTarget(*storage->view());if(error) {return {};}
  auto result=std::shared_ptr<NativeProfileSavePictureStage>(new NativeProfileSavePictureStage);
  result->publication_=publication;result->storage_=storage;result->kind_=kind;result->before_=token;result->logical_exists_=token->exists();
  result->system_output_=system_output;
  if(!system_output)result->expected_=found->intended;
  result->recovery_=writer.recovery;
  {
    const auto checked=publication->joined_artifacts_
        ? publication->joined_artifacts_->members()->Validate({token},error)
        : storage->view()->ValidateSnapshotBatch({token},error);
    if(!checked) {return {};}
    if(!publication->CurrentForDataStage(error)) {return {};}
    if(checked->metadata().size()!=1) {error=Error(std::errc::bad_message);return {};}
    // Generic snapshot FileInfo may carry POSIX ctime. It is not native birth.
    // Only a future durable native metadata/repair producer may supply that fact.
    result->creation_timestamp_=0;
    // An absent target has no retained creation timestamp; report unknown0,
    // never substitute POSIX ctime as a fabricated native birth time.
  }
  try {
    for(unsigned i=0;i<8;++i) {
      result->artifact_=StageLeaf();result->stage_=result->recovery_->root()->CreateSnapshotStage(result->artifact_,result->created_,error);
      if(result->stage_ || result->created_ || error!=std::errc::file_exists) {break;}
    }
    if(!result->stage_) {if(!error) {error=Error(std::errc::io_error);}result->initialization_error_=error;return result;}
    size_t done=0;const auto bytes=token->bytes();
    while(done<bytes.size()) {
      const auto part=bytes.subspan(done,std::min(bytes.size()-done,size_t(1<<20)));
      const auto actual=result->stage_->WriteResult(done,part);
      if(actual.bytes>part.size()) {error=Error(std::errc::io_error);break;}
      done+=actual.bytes;if(actual.error || !actual.bytes) {error=actual.error?actual.error:Error(std::errc::io_error);break;}
    }
    if(!error) {error=result->stage_->ResizeResult(bytes.size());}
  }catch(const std::bad_alloc&) {error=Error(std::errc::not_enough_memory);}
   catch(...) {error=Error(std::errc::io_error);}
  result->initialization_error_=error;return result; // Real created/uncertain owner retained.
}
std::error_code NativeProfileSavePictureStage::ReadAvailable() const {
  if(global_critical_region::is_held_by_current_thread()) {return Error(std::errc::operation_would_block);}
  if(initialization_error_) {return initialization_error_;}
  if(retired_.load(std::memory_order_acquire) || !stage_) {return Error(std::errc::operation_canceled);}
  return storage_->CheckAvailable();
}
std::error_code NativeProfileSavePictureStage::WriteAvailable() const {
  if(auto error=ReadAvailable()) {return error;}if(sealed_) {return Error(std::errc::device_or_resource_busy);}
  const auto owner=publication_.lock();std::error_code error;
  if(!owner || !owner->CurrentForDataStage(error)) {return error?error:Error(std::errc::operation_canceled);}return {};
}
std::error_code NativeProfileSavePictureStage::ValidateRequestedView(
    const std::shared_ptr<vfs::HostPathDevice>& requested,const std::string& path) const {
  if(!requested || requested->is_read_only() || requested->storage()->retiring()) {return Error(std::errc::permission_denied);}
  if(auto error=ReadAvailable()) {return error;}
  const auto actual=requested->storage()->directory_identity(),expected=storage_->view()->directory_identity();
  return actual && expected && expected->SameEntry(before_->relative(),*actual,
      xe::to_path(xe::utf8::fix_path_separators(path,std::filesystem::path::preferred_separator)),true)
      ?std::error_code{}:Error(std::errc::permission_denied);
}
uint32_t NativeProfileSavePictureStage::OpenManagedView(std::shared_ptr<vfs::HostPathDevice> requested,
    std::string path,vfs::FileDisposition disposition,uint32_t access,vfs::File** out,vfs::FileAction* action) {
  if(!out || !action) {return X_STATUS_INVALID_PARAMETER;}*out=nullptr;*action=vfs::FileAction::kDoesNotExist;
  if(global_critical_region::is_held_by_current_thread()) {return X_STATUS_INVALID_DEVICE_REQUEST;}
  if(auto error=ValidateRequestedView(requested,path)) {return vfs::HostStorageError(error);}
  if(uint32_t(disposition)>uint32_t(vfs::FileDisposition::kOverwriteIf)) {return X_STATUS_INVALID_PARAMETER;}
  std::lock_guard serial(mutex_);if(auto error=WriteAvailable()) {return vfs::HostStorageError(error);}
  const bool existed=logical_exists_;
  if(disposition==vfs::FileDisposition::kCreate && existed) {*action=vfs::FileAction::kExists;return X_STATUS_OBJECT_NAME_COLLISION;}
  if(!existed && (disposition==vfs::FileDisposition::kOpen || disposition==vfs::FileDisposition::kOverwrite)) {return X_STATUS_OBJECT_NAME_NOT_FOUND;}
  const bool truncate=disposition==vfs::FileDisposition::kSuperscede || disposition==vfs::FileDisposition::kOverwrite || disposition==vfs::FileDisposition::kOverwriteIf;
  if(truncate && !(access&(vfs::FileAccess::kFileWriteData|vfs::FileAccess::kFileAppendData|vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericAll))) {return X_STATUS_ACCESS_DENIED;}
  std::error_code error;
  if(truncate || !existed) {
    if(output_revision_==UINT64_MAX)return vfs::HostStorageError(Error(std::errc::state_not_recoverable));
    ++output_revision_;error=stage_->ResizeResult(0);if(error) {return vfs::HostStorageError(error);}
  }
  if(!existed) {
    // The original namespace disposition has now performed its real creation
    // effect. Use the emulated filesystem clock once; the temporary recovery
    // file's creation/ctime and future opens cannot supply native birth. A
    // later failed open/publication retains this same staged effect, never a
    // successful canonical-file or native-completion claim.
    new_creation_filetime_=Clock::QueryGuestSystemTime();
    creation_timestamp_=*new_creation_filetime_;
  }
  logical_exists_=true;
  const auto actual=stage_->QueryMetadata(error);if(!actual) {return vfs::HostStorageError(error);}
  vfs::File::Information info{};info.attributes=vfs::kFileAttributeNormal;info.create_timestamp=creation_timestamp_;
  info.end_of_file=actual->size;info.access_timestamp=actual->access_timestamp;info.write_timestamp=actual->write_timestamp;
  info.change_timestamp=actual->change_timestamp;
  const auto status=vfs::RoundFileAllocationSize(actual->allocated_bytes,
      uint64_t(requested->sectors_per_allocation_unit())*requested->bytes_per_sector(),&info.allocation_size);
  if(XFAILED(status)) {return status;}
  auto lock=global_critical_region::AcquireDirect();
  if(retired() || storage_->retiring() || requested->storage()->retiring()) {return X_STATUS_CANCELLED;}
  auto* root=requested->ResolvePath("");if(!root) {return X_STATUS_OBJECT_PATH_NOT_FOUND;}
  auto entry=std::make_unique<NativePictureEntry>(requested.get(),root,std::move(path),info,before_->root()->path()/before_->relative());
  *out=new NativeProfileSavePictureFile(access,std::move(entry),shared_from_this());
  *action=!existed?vfs::FileAction::kCreated:truncate?
      (disposition==vfs::FileDisposition::kSuperscede?vfs::FileAction::kSuperseded:vfs::FileAction::kOverwritten):vfs::FileAction::kOpened;
  return X_STATUS_SUCCESS;
}
filesystem::FileIoResult NativeProfileSavePictureStage::Read(size_t offset,std::span<uint8_t> bytes) {
  std::lock_guard lock(mutex_);if(auto error=ReadAvailable()) {return {error,0};}return stage_->ReadResult(offset,bytes);
}
filesystem::FileIoResult NativeProfileSavePictureStage::Write(size_t offset,std::span<const uint8_t> bytes) {
  std::lock_guard lock(mutex_);if(auto error=WriteAvailable()) {return {error,0};}
  if(!logical_exists_) {return {Error(std::errc::no_such_file_or_directory),0};}
  constexpr auto max=xam::GpdSnapshotStore::kMaximumBytes;
  if(offset>max || bytes.size()>max-offset) {return {Error(std::errc::file_too_large),0};}
  if(!bytes.empty()) {
    if(output_revision_==UINT64_MAX)return {Error(std::errc::state_not_recoverable),0};
    ++output_revision_;
  }
  const auto result=stage_->WriteAllResult(offset,bytes);
  if(result.bytes) {produced_=true;}return result;
}
std::error_code NativeProfileSavePictureStage::Resize(size_t length) {
  std::lock_guard lock(mutex_);if(auto error=WriteAvailable()) {return error;}
  if(!logical_exists_) {return Error(std::errc::no_such_file_or_directory);}
  if(length>xam::GpdSnapshotStore::kMaximumBytes)return Error(std::errc::file_too_large);
  if(output_revision_==UINT64_MAX)return Error(std::errc::state_not_recoverable);
  ++output_revision_;return stage_->ResizeResult(length);
}
std::error_code NativeProfileSavePictureStage::SetTimes(uint64_t creation,uint64_t access,uint64_t write) {
  std::lock_guard lock(mutex_);if(auto error=WriteAvailable())return error;
  // Native CopyFile propagates its source FileBasic fields after byte copy.
  // Zero birth is a genuine unknown/leave-alone value. Nonzero native birth
  // needs the real durable FAT owner; host POSIX ctime cannot satisfy it.
  if(creation)return Error(std::errc::operation_not_supported);
  if(!logical_exists_)return Error(std::errc::no_such_file_or_directory);
  if(access || write) {
    if(output_revision_==UINT64_MAX)return Error(std::errc::state_not_recoverable);
    ++output_revision_;
  }
  return stage_->SetTimesResult(access,write);
}
std::error_code NativeProfileSavePictureStage::Flush() {
  std::lock_guard lock(mutex_);if(auto error=ReadAvailable()) {return error;}
  const auto error=stage_->FlushResult();
  if(!error)flushed_revision_=output_revision_;
  return error; // Actual stage fsync; no canonical/native completion is implied.
}
std::shared_ptr<const NativeProfilePictureCheckpoint>
NativeProfileSavePictureStage::CaptureWriteCheckpoint(std::error_code& error) {
  error.clear();std::unique_lock lock(mutex_,std::try_to_lock);
  if(!lock.owns_lock()) {error=Error(std::errc::device_or_resource_busy);return {};}
  error=ReadAvailable();if(error)return {};
  const auto owner=publication_.lock();
  if(!owner || !owner->CurrentForStage(error)) {
    if(!error)error=Error(std::errc::operation_canceled);return {};
  }
  if(!logical_exists_ || !produced_ || !flushed_revision_ ||
      *flushed_revision_!=output_revision_) {
    error=Error(std::errc::operation_would_block);return {};
  }
  try {
    const auto before=stage_->QueryInfo(error);if(!before)return {};
    if(!before->regular || before->links!=1 || before->size>xam::GpdSnapshotStore::kMaximumBytes) {
      error=Error(std::errc::bad_message);return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(before->size));size_t done=0;
    while(done<bytes.size()) {
      const auto part=std::span(bytes).subspan(done,std::min(bytes.size()-done,size_t(1<<20)));
      const auto actual=stage_->ReadResult(done,part);
      if(actual.bytes>part.size()) {error=Error(std::errc::io_error);return {};}
      done+=actual.bytes;
      if(actual.error || !actual.bytes) {error=actual.error?actual.error:Error(std::errc::io_error);return {};}
    }
    const bool small=kind_==filesystem::ProfileArtifactKind::kPublic32 ||
        kind_==filesystem::ProfileArtifactKind::kPersonal32 || kind_==filesystem::ProfileArtifactKind::kAvatar32;
    error=xam::ValidateProfilePicturePng(bytes,small?32:64);if(error)return {};
    if(!system_output_ && bytes!=expected_) {error=Error(std::errc::state_not_recoverable);return {};}
    const auto after=stage_->QueryInfo(error);if(!after)return {};
    if(after->file_id!=before->file_id || after->filesystem_id!=before->filesystem_id ||
        after->size!=before->size || !after->regular || after->links!=1) {
      error=Error(std::errc::state_not_recoverable);return {};
    }
    const auto metadata=stage_->QueryMetadata(error);if(!metadata)return {};
    if(metadata->size!=bytes.size()) {error=Error(std::errc::state_not_recoverable);return {};}
    return std::shared_ptr<const NativeProfilePictureCheckpoint>(new NativeProfilePictureCheckpoint(
        weak_from_this(),output_revision_,before_,kind_,std::move(bytes),*metadata,new_creation_filetime_));
  } catch(const std::bad_alloc&) {error=Error(std::errc::not_enough_memory);return {};}
}
std::optional<filesystem::RetainedFileMetadata> NativeProfileSavePictureStage::Query(std::error_code& error) {
  std::lock_guard lock(mutex_);error=ReadAvailable();return error?std::nullopt:stage_->QueryMetadata(error);
}
std::optional<std::vector<uint8_t>> NativeProfileSavePictureStage::Seal(std::error_code& error) {
  std::lock_guard lock(mutex_);error=ReadAvailable();if(error) {return {};}
  if(sealed_) {return sealed_bytes_;} // SAME immutable actual-output receipt.
  const auto owner=publication_.lock();
  if(!owner || !owner->CurrentForStage(error))return {};
  if(managed_writer_views_.load(std::memory_order_acquire)) {error=Error(std::errc::device_or_resource_busy);return {};}
  if(!logical_exists_ || !produced_) {error=Error(std::errc::operation_would_block);return {};}
  const auto before=stage_->QueryInfo(error);if(!before) {return {};}
  if(!before->regular || before->links!=1 || before->size>xam::GpdSnapshotStore::kMaximumBytes) {error=Error(std::errc::bad_message);return {};}
  std::vector<uint8_t> bytes(static_cast<size_t>(before->size));size_t done=0;
  while(done<bytes.size()) {
    const auto part=std::span(bytes).subspan(done,std::min(bytes.size()-done,size_t(1<<20)));
    const auto actual=stage_->ReadResult(done,part);
    if(actual.bytes>part.size()) {error=Error(std::errc::io_error);return {};}
    done+=actual.bytes;if(actual.error || !actual.bytes) {error=actual.error?actual.error:Error(std::errc::io_error);return {};}
  }
  const bool small=kind_==filesystem::ProfileArtifactKind::kPublic32 ||
      kind_==filesystem::ProfileArtifactKind::kPersonal32 || kind_==filesystem::ProfileArtifactKind::kAvatar32;
  error=xam::ValidateProfilePicturePng(bytes,small?32:64);if(error) {return {};}
  if(!system_output_ && bytes!=expected_) {error=Error(std::errc::state_not_recoverable);return {};}
  error=stage_->FlushResult();if(error) {return {};}
  const auto after=stage_->QueryInfo(error);
  if(!after || after->file_id!=before->file_id || after->filesystem_id!=before->filesystem_id ||
      after->size!=before->size || !after->regular || after->links!=1) {if(!error) {error=Error(std::errc::state_not_recoverable);}return {};}
  sealed_bytes_=std::move(bytes);sealed_=true;return sealed_bytes_;
}
void NativeProfileSavePictureStage::RetireAtActualQuiescence() {
  std::lock_guard lock(mutex_);retired_.store(true,std::memory_order_release);
}
}  // namespace xe::kernel
