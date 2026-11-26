/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_gpd_stage.h"
#include <algorithm>
#include <array>
#include <random>
#include <new>
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/base/mutex.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/kernel/native_profile_save_publication.h"
#include "xenia/kernel/native_profile_save_serialization.h"
namespace xe::kernel {
namespace {
std::error_code Error(std::errc value) { return std::make_error_code(value); }
std::filesystem::path StageLeaf() {
  // Reuse the existing strict RecoveryLeaf grammar. Ownership/meaning comes
  // from this retained actual capsule, never scanning/adopting a filename.
  constexpr char hex[]="0123456789abcdef";
  std::string leaf="snapshot-"; std::random_device random;
  for(unsigned i=0;i<4;++i) {
    const uint32_t value=random();
    for(unsigned j=0;j<8;++j) { leaf.push_back(hex[(value>>(j*4))&15]); }
  }
  return leaf+".bin";
}
}
namespace {
class NativeGpdStageEntry final : public vfs::Entry {
 public:
  NativeGpdStageEntry(vfs::Device* device,vfs::Entry* root,std::string path,uint64_t size,uint64_t creation,std::filesystem::path physical)
      : Entry(device,root,path) {
    coordinate_=std::make_shared<CheckpointCoordinate>(CheckpointCoordinate{std::move(physical)});
    attributes_=vfs::kFileAttributeNormal; size_=size;create_timestamp_=creation;
    allocation_size_=size ? xe::round_up(size,size_t(512)) : 0;
  }
  std::shared_ptr<const CheckpointCoordinate> RetainCheckpointCoordinate() override {return coordinate_;}
  bool allows_namespace_mutation() const override {return false;}
  X_STATUS Open(uint32_t,vfs::File**) override {return X_STATUS_ACCESS_DENIED;}
 private:
  std::shared_ptr<const CheckpointCoordinate> coordinate_;
  bool DeleteEntryInternal(vfs::Entry*) override {return false;}
};
}
class NativeProfileSaveManagedGpdFile final : public vfs::File {
 public:
  NativeProfileSaveManagedGpdFile(uint32_t access,std::unique_ptr<NativeGpdStageEntry> entry,
      std::shared_ptr<NativeProfileSaveGpdStage> stage)
      : File(access,entry.get()),entry_owner_(std::move(entry)),stage_(std::move(stage)),
        canonical_(stage_->storage_->device()) {
    device_owner_->RetainManagedFile();
    if(device_owner_!=canonical_) {canonical_->RetainManagedFile();}
    stage_->managed_views_.fetch_add(1,std::memory_order_acq_rel);
  }
  ~NativeProfileSaveManagedGpdFile() override {
    // No stage mutex under a caller's GCRC: the opposite open order is
    // stage→GCRC. No method can enter this genuinely destroying File again.
    device_owner_->ReleaseManagedFile();
    if(device_owner_!=canonical_) {canonical_->ReleaseManagedFile();}
    stage_->managed_views_.fetch_sub(1,std::memory_order_acq_rel);
    if(checkpoint_close_)checkpoint_close_->data_status.store(X_STATUS_SUCCESS,std::memory_order_release);
  }
  void ObserveCheckpointClose(std::shared_ptr<CheckpointCloseObservation> receipt) override {checkpoint_close_=std::move(receipt);}
  void Destroy() override {delete this;}
  X_STATUS ReadSync(std::span<uint8_t> bytes,size_t offset,size_t* transferred) override {
    if(!transferred) {return X_STATUS_INVALID_PARAMETER;} *transferred=0;
    if(!(file_access_&(vfs::FileAccess::kGenericRead|vfs::FileAccess::kFileReadData))) {return X_STATUS_ACCESS_DENIED;}
    const auto result=stage_->Read(offset,bytes); *transferred=result.bytes;
    return vfs::HostStorageError(result.error);
  }
  X_STATUS WriteSync(std::span<const uint8_t> bytes,size_t offset,size_t* transferred) override {
    if(!transferred) {return X_STATUS_INVALID_PARAMETER;} *transferred=0;
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kFileWriteData|vfs::FileAccess::kFileAppendData))) {return X_STATUS_ACCESS_DENIED;}
    const auto result=stage_->Write(offset,bytes); *transferred=result.bytes;
    return vfs::HostStorageError(result.error);
  }
  X_STATUS SetLength(size_t length) override {
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kFileWriteData))) {return X_STATUS_ACCESS_DENIED;}
    return vfs::HostStorageError(stage_->Resize(length));
  }
  X_STATUS Flush() override {return vfs::HostStorageError(stage_->Flush());}
  X_STATUS QueryInformation(Information* output) const override {
    if(!output) {return X_STATUS_INVALID_PARAMETER;}
    std::error_code error; const auto actual=stage_->Query(error);
    if(!actual) {return stage_->retired() ? X_STATUS_INVALID_HANDLE : vfs::HostStorageError(error);}
    Information result{}; result.attributes=vfs::kFileAttributeNormal;
    result.create_timestamp=entry_->create_timestamp(); result.end_of_file=actual->size;
    result.access_timestamp=actual->access_timestamp; result.write_timestamp=actual->write_timestamp;
    result.change_timestamp=actual->change_timestamp;
    const uint64_t unit=uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector();
    const auto status=vfs::RoundFileAllocationSize(actual->allocated_bytes,unit,&result.allocation_size);
    if(XFAILED(status)) {return status;} *output=result; return X_STATUS_SUCCESS;
  }
  X_STATUS QueryAllocationSize(uint64_t* output) const override {
    if(!output) {return X_STATUS_INVALID_PARAMETER;}
    std::error_code error; const auto actual=stage_->Query(error);
    if(!actual) {return stage_->retired() ? X_STATUS_INVALID_HANDLE : vfs::HostStorageError(error);}
    const uint64_t unit=uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector();
    return vfs::RoundFileAllocationSize(actual->allocated_bytes,unit,output);
  }
 private:
  std::shared_ptr<CheckpointCloseObservation> checkpoint_close_;
  std::unique_ptr<NativeGpdStageEntry> entry_owner_;
  std::shared_ptr<NativeProfileSaveGpdStage> stage_;
  std::shared_ptr<vfs::HostPathDevice> canonical_;
};
std::error_code NativeProfileSaveGpdStage::ValidateRequestedView(
    const std::shared_ptr<vfs::HostPathDevice>& requested,const std::string& path) const {
  if(!requested || requested->is_read_only()) {return Error(std::errc::permission_denied);}
  if(auto error=ReadAvailable()) {return error;}
  const auto actual_directory=requested->storage()->directory_identity();
  const auto expected_directory=storage_->view()->directory_identity();
  if(!actual_directory || !expected_directory ||
      !expected_directory->SameEntry(before_.token->relative(),*actual_directory,
          xe::to_path(xe::utf8::fix_path_separators(path,std::filesystem::path::preferred_separator)),true)) {
    return Error(std::errc::permission_denied);
  }
  return {};
}
uint32_t NativeProfileSaveGpdStage::OpenManagedView(
    std::shared_ptr<vfs::HostPathDevice> requested,std::string path,uint32_t access,vfs::File** output) {
  if(!output) {return X_STATUS_INVALID_PARAMETER;} *output=nullptr;
  if(global_critical_region::is_held_by_current_thread() || !requested || requested->is_read_only()) {
    return X_STATUS_ACCESS_DENIED;
  }
  if(auto error=ValidateRequestedView(requested,path)) {return vfs::HostStorageError(error);}
  std::unique_lock stage_lock(mutex_);
  if(auto error=ReadAvailable()) {return vfs::HostStorageError(error);}
  if(sealed_ || cache_view_claimed_) {return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));}
  std::error_code error; const auto actual=stage_->QueryMetadata(error);
  if(!actual) {return vfs::HostStorageError(error);}
  auto lock=global_critical_region::AcquireDirect();
  if(retired() || storage_->retiring() || requested->storage()->retiring()) {return X_STATUS_CANCELLED;}
  auto* root=requested->ResolvePath("");if(!root) {return X_STATUS_OBJECT_PATH_NOT_FOUND;}
  auto entry=std::make_unique<NativeGpdStageEntry>(requested.get(),root,std::move(path),actual->size,creation_timestamp_,before_.token->root()->path()/before_.token->relative());
  *output=new NativeProfileSaveManagedGpdFile(access,std::move(entry),shared_from_this());
  return X_STATUS_SUCCESS;
}
std::shared_ptr<NativeProfileSaveGpdStage> NativeProfileSaveGpdStage::Create(
    std::shared_ptr<NativeProfileSavePublication> publication,
    xam::GpdSnapshotLoad before,std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !publication || !before.token || before.error) {
    error=Error(std::errc::invalid_argument); return {};
  }
  if (!publication->CurrentForStage(error)) { return {}; }
  const auto expected=publication->pending_->BuildPlan(error);
  if(!expected || error) {return {};}
  const auto member=expected->files.find(before.identity.title_id);
  if(before.identity.title_id!=publication->pending_->command().title_id ||
      member==expected->files.end() || member->second.before.token!=before.token) {
    error=Error(std::errc::operation_canceled);return {};
  }
  if(before.missing()) {
    // Genuine native UserInitialize/default DB creation needs its own proved
    // producer. Never turn absent bytes into an empty successful GPD here.
    error=Error(std::errc::operation_not_supported);return {};
  }
  const auto& writer=publication->WriterForStage();
  const auto& storage=publication->OriginForStage().storage;
  if (before.token->root()!=storage->view() || before.identity.xuid!=storage->xuid() ||
      before.identity.source_generation!=storage->source_generation()) {
    error=Error(std::errc::operation_canceled); return {};
  }
  if (!writer.recovery || writer.recovery->error()) {
    error=writer.recovery ? writer.recovery->error() : Error(std::errc::no_such_device); return {};
  }
  error=writer.recovery->CheckTarget(*storage->view()); if(error) {return {};}
  auto result=std::shared_ptr<NativeProfileSaveGpdStage>(new NativeProfileSaveGpdStage);
  result->publication_=publication; result->storage_=storage;
  result->before_=std::move(before); result->recovery_=writer.recovery;
  {
    // Native cached raw handles must first be genuinely replaced/closed at
    // the DB-use boundary. Do not exempt an unretired raw file lease or seed
    // a stage from stale/untracked bytes. Immutable read views own no raw FD.
    const auto checked=publication->joined_artifacts_
        ? publication->joined_artifacts_->members()->Validate({result->before_.token},error)
        : storage->view()->ValidateSnapshotBatch({result->before_.token},error);
    if(!checked) {return {};}
    if(!publication->CurrentForDataStage(error)) {return {};}
    if(checked->metadata().size()!=1 || !checked->metadata()[0]) {error=Error(std::errc::bad_message);return {};}
    result->creation_timestamp_=0; // Generic POSIX ctime is not native birth.
  }
  try {
  for(unsigned i=0;i<8;++i) {
    result->artifact_=StageLeaf();
    result->stage_=result->recovery_->root()->CreateSnapshotStage(result->artifact_,result->created_,error);
    if(result->stage_ || result->created_ || error!=std::errc::file_exists) {break;}
  }
  // Return the real created/uncertain artifact owner even on failure; the
  // enclosing request retains it for report/recovery and cannot raw-fallback.
  if(!result->stage_) {
    if(!error) {error=Error(std::errc::io_error);}
    result->initialization_error_=error; return result;
  }
  size_t done=0; const auto bytes=result->before_.token->bytes();
  while(done<bytes.size()) {
    const auto part=bytes.subspan(done,std::min(bytes.size()-done,size_t(1<<20)));
    const auto written=result->stage_->WriteResult(done,part);
    if(written.bytes>part.size()) {error=Error(std::errc::io_error);result->initialization_error_=error;return result;}
    done+=written.bytes;
    if(written.error || !written.bytes) {error=written.error ? written.error : Error(std::errc::io_error);result->initialization_error_=error;return result;}
  }
  error=result->stage_->ResizeResult(bytes.size());
  } catch(const std::bad_alloc&) {error=Error(std::errc::not_enough_memory);}
    catch(...) {error=Error(std::errc::io_error);}
  result->initialization_error_=error; return result;
}
std::error_code NativeProfileSaveGpdStage::ReadAvailable() const {
  if(global_critical_region::is_held_by_current_thread()) {return Error(std::errc::operation_would_block);}
  if(initialization_error_) {return initialization_error_;}
  if(retired_.load(std::memory_order_acquire) || !stage_) {return Error(std::errc::operation_canceled);}
  return storage_->CheckAvailable();
}
std::error_code NativeProfileSaveGpdStage::WriteAvailable() const {
  if(auto error=ReadAvailable()) {return error;}
  if(sealed_) {return Error(std::errc::device_or_resource_busy);}
  const auto owner=publication_.lock(); std::error_code error;
  if(!owner || !owner->CurrentForDataStage(error)) {return error ? error : Error(std::errc::operation_canceled);}
  return {};
}
filesystem::FileIoResult NativeProfileSaveGpdStage::Read(size_t offset,std::span<uint8_t> bytes) {
  std::lock_guard lock(mutex_);
  if(auto error=ReadAvailable()) {return {error,0};}
  return stage_->ReadResult(offset,bytes);
}
filesystem::FileIoResult NativeProfileSaveGpdStage::Write(size_t offset,std::span<const uint8_t> bytes) {
  std::lock_guard lock(mutex_);
  if(auto error=WriteAvailable()) {return {error,0};}
  if(offset>kMaximumBytes || bytes.size()>kMaximumBytes-offset) {return {Error(std::errc::file_too_large),0};}
  // The existing empty WriteAll path enters no native byte operation. Keep
  // its original attempted-write bookkeeping without staling a valid epoch.
  if(!bytes.empty() && data_revision_==UINT64_MAX)return {Error(std::errc::state_not_recoverable),0};
  mutation_attempted_=true;if(!bytes.empty())++data_revision_;
  return stage_->WriteAllResult(offset,bytes);
}
std::error_code NativeProfileSaveGpdStage::Resize(size_t length) {
  std::lock_guard lock(mutex_);
  if(auto error=WriteAvailable()) {return error;}
  if(length>kMaximumBytes) {return Error(std::errc::file_too_large);}
  if(data_revision_==UINT64_MAX)return Error(std::errc::state_not_recoverable);
  mutation_attempted_=true;++data_revision_;
  return stage_->ResizeResult(length);
}
std::error_code NativeProfileSaveGpdStage::Flush() {
  std::lock_guard lock(mutex_);
  if(auto error=ReadAvailable()) {return error;}
  return stage_->FlushResult(); // actual stage fsync, never target publication
}
std::optional<filesystem::RetainedFileMetadata> NativeProfileSaveGpdStage::Query(std::error_code& error) {
  std::lock_guard lock(mutex_); error=ReadAvailable();
  return error ? std::nullopt : stage_->QueryMetadata(error);
}
std::optional<std::vector<uint8_t>> NativeProfileSaveGpdStage::SnapshotAtActualWriteCheckpoint(std::error_code& error) {
  std::lock_guard lock(mutex_);error=WriteAvailable();if(error)return {};
  return ReadSnapshotLocked(false,error);
}
std::shared_ptr<const NativeProfileGpdCheckpoint>
NativeProfileSaveGpdStage::CaptureWriteCheckpoint(
    std::span<const uint8_t> native_index, std::error_code& error) {
  std::lock_guard lock(mutex_);
  error = WriteAvailable();
  if (error) {
    return {};
  }
  auto bytes = ReadSnapshotLocked(false, error);
  if (!bytes) {
    return {};
  }
  error = xam::ValidateGpdSnapshot(*bytes);
  if (error) {
    return {};
  }
  error = xam::ValidateProfileGpdRecords(
      *bytes, before_.identity.title_id == 0xFFFE07D1);
  if (error) {
    return {};
  }
  error =
      save_detail::ValidateNativeGpdSerialization(*bytes, *bytes, native_index);
  if (error) {
    return {};
  }
  return std::shared_ptr<const NativeProfileGpdCheckpoint>(
      new NativeProfileGpdCheckpoint(
          weak_from_this(), data_revision_, std::move(*bytes),
          std::vector<uint8_t>(native_index.begin(), native_index.end())));
}
std::optional<std::vector<uint8_t>> NativeProfileSaveGpdStage::ReadSnapshotLocked(bool flush,std::error_code& error) {
  const auto before=stage_->QueryInfo(error); if(!before) {return {};}
  if(!before->regular || before->links!=1 || before->size>kMaximumBytes) {error=Error(std::errc::bad_message);return {};}
  std::vector<uint8_t> bytes(static_cast<size_t>(before->size)); size_t done=0;
  while(done<bytes.size()) {
    const auto part=std::span(bytes).subspan(done,std::min(bytes.size()-done,size_t(1<<20)));
    const auto read=stage_->ReadResult(done,part);
    if(read.bytes>part.size()) {error=Error(std::errc::io_error);return {};}
    done+=read.bytes;
    if(read.error || !read.bytes) {error=read.error ? read.error : Error(std::errc::io_error);return {};}
  }
  if(flush) {error=stage_->FlushResult(); if(error) {return {};}}
  const auto after=stage_->QueryInfo(error); if(!after) {return {};}
  if(after->filesystem_id!=before->filesystem_id || after->file_id!=before->file_id ||
      after->size!=before->size || after->links!=1 || !after->regular) {
    error=Error(std::errc::state_not_recoverable); return {};
  }
  return bytes;
}
std::optional<std::vector<uint8_t>> NativeProfileSaveGpdStage::Seal(std::error_code& error) {
  std::lock_guard lock(mutex_);error=ReadAvailable();if(error)return {};
  if(sealed_)return sealed_bytes_;
  const auto owner=publication_.lock();
  if(!owner || !owner->CurrentForStage(error))return {};
  auto bytes=ReadSnapshotLocked(true,error);if(!bytes)return {};
  // Allocate retained receipt BEFORE closing write admission. A later bad
  // supplied native prefix may be retried against these SAME sealed bytes;
  // no fresh layout/source or second physical writer can be adopted.
  sealed_bytes_=*bytes;sealed_=true;return bytes;
}
void NativeProfileSaveGpdStage::RetireAtActualQuiescence() {
  // Serialize with all actual retained host IO before withdrawing this real
  // stage generation. The native owner must additionally prove DB quiescence.
  std::lock_guard lock(mutex_); retired_.store(true,std::memory_order_release);
}
}  // namespace xe::kernel
