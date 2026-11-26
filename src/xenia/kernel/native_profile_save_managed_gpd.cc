/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_managed_gpd.h"
#include <algorithm>
#include <cstdio>
#include <new>
#include <utility>
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/kernel/native_profile_gpd_read_view.h"
#include "xenia/kernel/native_profile_save_gpd_stage.h"
#include "xenia/kernel/native_profile_save_publication.h"
#include "xenia/kernel/native_profile_save_serialization.h"
#include "xenia/kernel/native_profile_save_source.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/host_storage_errors.h"
namespace xe::kernel {
namespace {
std::error_code Error(std::errc value) {return std::make_error_code(value);}
bool SameOwner(const std::shared_ptr<vfs::Device>& a,const std::shared_ptr<vfs::Device>& b) {
  return a && b && a.get()==b.get() && !a.owner_before(b) && !b.owner_before(a);
}
std::shared_ptr<const filesystem::HostStorageSnapshotToken> ReadToken(
    const xam::ProfileGpdReadCapture& read,uint32_t title) {
  char leaf[13];std::snprintf(leaf,sizeof(leaf),"%08X.gpd",title);
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> result;
  for(const auto& token:read.tokens) {
    if(!token || token->root()!=read.origin.storage->view() || token->relative().has_parent_path() ||
        !token->ascii_folded_leaf()) {continue;}
    const auto actual=token->relative().generic_string();
    if(!xe::utf8::equal_case(actual,leaf)) {continue;}
    if(result) {return {};}
    result=token;
  }
  return result;
}
bool PhysicalLeaf(const std::shared_ptr<xam::ProfileStorageLease>& storage,
    const std::shared_ptr<vfs::HostPathDevice>& requested,const std::string& path,
    const filesystem::HostStorageSnapshotToken& token) {
  const auto actual=requested->storage()->directory_identity();
  const auto expected=storage->view()->directory_identity();
  return actual && expected && token.root()==storage->view() &&
      expected->SameEntry(token.relative(),*actual,
          xe::to_path(xe::utf8::fix_path_separators(path,std::filesystem::path::preferred_separator)),true);
}
class NativeGpdReadEntry final : public vfs::Entry {
 public:
  NativeGpdReadEntry(vfs::Device* device,vfs::Entry* root,std::string path,
      const vfs::File::Information& information,std::filesystem::path physical)
      : Entry(device,root,path) {
    coordinate_=std::make_shared<CheckpointCoordinate>(CheckpointCoordinate{std::move(physical)});
    attributes_=information.attributes;size_=information.end_of_file;
    allocation_size_=information.allocation_size;create_timestamp_=information.create_timestamp;
    access_timestamp_=information.access_timestamp;write_timestamp_=information.write_timestamp;
  }
  std::shared_ptr<const CheckpointCoordinate> RetainCheckpointCoordinate() override {return coordinate_;}
  bool allows_namespace_mutation() const override {return false;}
  X_STATUS Open(uint32_t,vfs::File**) override {return X_STATUS_ACCESS_DENIED;}
 private:
  std::shared_ptr<const CheckpointCoordinate> coordinate_;
  bool DeleteEntryInternal(vfs::Entry*) override {return false;}
};
}
class NativeProfileGpdReadFile final : public vfs::File {
 public:
  NativeProfileGpdReadFile(uint32_t access,std::unique_ptr<NativeGpdReadEntry> entry,
      std::shared_ptr<NativeProfileGpdReadView> view,
      std::shared_ptr<const NativeProfileSaveManagedGpd::ReadReceipt> receipt)
      : File(access,entry.get()),entry_owner_(std::move(entry)),view_(std::move(view)),
        receipt_(std::move(receipt)),
        canonical_(view_->read_.origin.storage->device()) {
    device_owner_->RetainManagedFile();if(device_owner_!=canonical_) {canonical_->RetainManagedFile();}
  }
  ~NativeProfileGpdReadFile() override {
    device_owner_->ReleaseManagedFile();if(device_owner_!=canonical_) {canonical_->ReleaseManagedFile();}
    if(checkpoint_close_)checkpoint_close_->data_status.store(X_STATUS_SUCCESS,std::memory_order_release);
  }
  void ObserveCheckpointClose(std::shared_ptr<CheckpointCloseObservation> receipt) override {checkpoint_close_=std::move(receipt);}
  void Destroy() override {delete this;}
  X_STATUS ReadSync(std::span<uint8_t> output,size_t offset,size_t* count) override {
    std::lock_guard lock(mutex_);
    if(!count) {return X_STATUS_INVALID_PARAMETER;}*count=0;
    if(!(file_access_&(vfs::FileAccess::kGenericRead|vfs::FileAccess::kFileReadData))) {return X_STATUS_ACCESS_DENIED;}
    if(stage_) {const auto actual=stage_->Read(offset,output);*count=actual.bytes;return vfs::HostStorageError(actual.error);}
    if(auto error=view_->Available()) {return vfs::HostStorageError(error);}
    const auto bytes=view_->token_->bytes();
    if(offset<bytes.size()) {*count=std::min(output.size(),bytes.size()-offset);std::copy_n(bytes.data()+offset,*count,output.data());}
    return X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(std::span<const uint8_t> bytes,size_t offset,size_t* count) override {
    std::lock_guard lock(mutex_);
    if(!count) {return X_STATUS_INVALID_PARAMETER;}*count=0;
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kFileWriteData|vfs::FileAccess::kFileAppendData))) {if(!stage_)denied_mutation_attempted_=true;return X_STATUS_ACCESS_DENIED;}
    const auto admitted=NativeProfileSaveManagedGpd::AdoptBoundSystemStage(*this);
    if(XFAILED(admitted)){denied_mutation_attempted_=true;return admitted;}
    const auto actual=stage_->Write(offset,bytes);*count=actual.bytes;
    if(actual.error) {denied_mutation_attempted_=true;}return vfs::HostStorageError(actual.error);
  }
  X_STATUS SetLength(size_t length) override {
    std::lock_guard lock(mutex_);
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kFileWriteData))) {if(!stage_)denied_mutation_attempted_=true;return X_STATUS_ACCESS_DENIED;}
    const auto admitted=NativeProfileSaveManagedGpd::AdoptBoundSystemStage(*this);
    if(XFAILED(admitted)){denied_mutation_attempted_=true;return admitted;}
    const auto error=stage_->Resize(length);if(error) {denied_mutation_attempted_=true;}
    return vfs::HostStorageError(error);
  }
  X_STATUS SetAllocationSize(uint64_t length) override {
    std::lock_guard lock(mutex_);
    if(!(file_access_&(vfs::FileAccess::kGenericWrite|vfs::FileAccess::kFileWriteData))){if(!stage_)denied_mutation_attempted_=true;return X_STATUS_ACCESS_DENIED;}
    const auto admitted=NativeProfileSaveManagedGpd::AdoptBoundSystemStage(*this);
    if(XFAILED(admitted)){denied_mutation_attempted_=true;return admitted;}
    std::error_code error;const auto actual=stage_->Query(error);
    if(!actual)return vfs::HostStorageError(error);
    if(length<actual->size)error=stage_->Resize(size_t(length));
    if(error)denied_mutation_attempted_=true;
    return vfs::HostStorageError(error);
  }
  X_STATUS SetTimes(uint64_t,uint64_t,uint64_t) override {
    std::lock_guard lock(mutex_);
    if(!stage_)denied_mutation_attempted_=true;
    return stage_ ? X_STATUS_UNSUCCESSFUL : X_STATUS_ACCESS_DENIED;
  }
  X_STATUS Flush() override {
    std::lock_guard lock(mutex_);
    if(stage_) {return vfs::HostStorageError(stage_->Flush());}
    if(auto error=view_->Available()) {return vfs::HostStorageError(error);}
    const auto observed=view_->read_.origin.storage->view()->ObserveSnapshotDurability(*view_->token_);
    return vfs::HostStorageError(observed.error ? observed.error : observed.durable ? std::error_code{} : Error(std::errc::io_error));
  }
  X_STATUS QueryInformation(Information* output) const override {
    std::lock_guard lock(mutex_);
    if(!output) {return X_STATUS_INVALID_PARAMETER;}
    if(stage_) {
      std::error_code error;const auto actual=stage_->Query(error);
      if(!actual) {return stage_->retired() ? X_STATUS_INVALID_HANDLE : vfs::HostStorageError(error);}
      Information result{};result.attributes=vfs::kFileAttributeNormal;
      result.create_timestamp=stage_->creation_timestamp_;result.end_of_file=actual->size;
      result.access_timestamp=actual->access_timestamp;result.write_timestamp=actual->write_timestamp;
      result.change_timestamp=actual->change_timestamp;
      const uint64_t unit=uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector();
      const auto status=vfs::RoundFileAllocationSize(actual->allocated_bytes,unit,&result.allocation_size);
      if(XFAILED(status)) {return status;}*output=result;return X_STATUS_SUCCESS;
    }
    if(auto error=view_->Available()) {return view_->retired_.load(std::memory_order_acquire) ? X_STATUS_INVALID_HANDLE : vfs::HostStorageError(error);}
    auto result=view_->information_;
    const uint64_t unit=uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector();
    const auto status=vfs::RoundFileAllocationSize(view_->native_allocated_bytes_,unit,&result.allocation_size);
    if(XFAILED(status)) {return status;}*output=result;return X_STATUS_SUCCESS;
  }
  X_STATUS QueryAllocationSize(uint64_t* output) const override {
    std::lock_guard lock(mutex_);
    if(!output) {return X_STATUS_INVALID_PARAMETER;}
    const uint64_t unit=uint64_t(entry_->sectors_per_allocation_unit())*device_owner_->bytes_per_sector();
    if(stage_) {
      std::error_code error;const auto actual=stage_->Query(error);
      if(!actual) {return stage_->retired() ? X_STATUS_INVALID_HANDLE : vfs::HostStorageError(error);}
      return vfs::RoundFileAllocationSize(actual->allocated_bytes,unit,output);
    }
    if(auto error=view_->Available()) {return vfs::HostStorageError(error);}
    return vfs::RoundFileAllocationSize(view_->native_allocated_bytes_,unit,output);
  }
 private:
  std::shared_ptr<CheckpointCloseObservation> checkpoint_close_;
  friend class NativeProfileSaveManagedGpd;
  std::unique_ptr<NativeGpdReadEntry> entry_owner_;
  mutable std::mutex mutex_;
  // An original native writer may have changed its in-memory cache before a
  // refused File write. Never infer that cached object is clean on a retry;
  // this exact File must reach real native close/reopen before stage adoption.
  bool denied_mutation_attempted_=false;
  std::shared_ptr<NativeProfileGpdReadView> view_;
  // No raw File pointer replacement: all backend methods observe this exact
  // owned stage under the same lock, while Entry/device/handle identity stays.
  std::shared_ptr<NativeProfileSaveGpdStage> stage_;
  // The request registry retains the real publication duty. The File carries
  // only a weak issued byte-session binding, so original File destruction
  // cannot become the last publication/source destructor under ObjectTable's
  // GCRC. Expiry refuses; it never grants a raw/ordinary-write fallback.
  bool system_adoption_bound_=false;
  std::weak_ptr<NativeProfileSavePublication> system_publication_;
  std::weak_ptr<NativeProfileSaveManagedGpd> system_service_;
  std::vector<uint8_t> system_before_header_;
  // This metadata may outlive its original request, like the owned read bytes.
  // No original caller/source-writer/IO duty is retained by the File.
  const std::shared_ptr<const NativeProfileSaveManagedGpd::ReadReceipt> receipt_;
  std::shared_ptr<vfs::HostPathDevice> canonical_;
};
std::shared_ptr<NativeProfileGpdReadView> NativeProfileSaveManagedGpd::CaptureOwnedReadView(const vfs::File& file) {
  const auto* actual=dynamic_cast<const NativeProfileGpdReadFile*>(&file);
  if(!actual) {return {};}
  std::lock_guard lock(actual->mutex_);
  return actual->stage_ ? std::shared_ptr<NativeProfileGpdReadView>{} : actual->view_;
}
std::shared_ptr<const NativeProfileSaveManagedGpd::ReadReceipt>
NativeProfileSaveManagedGpd::IssueReadReceipt() {
  return std::shared_ptr<const ReadReceipt>(new ReadReceipt);
}
std::shared_ptr<NativeProfileGpdReadView> NativeProfileSaveManagedGpd::TryCaptureOpenedRead(
    const vfs::File& file,const std::shared_ptr<const ReadReceipt>& receipt,std::error_code& error) {
  error.clear();const auto* actual=dynamic_cast<const NativeProfileGpdReadFile*>(&file);
  if(!actual || !receipt || actual->receipt_!=receipt ||
      actual->receipt_.owner_before(receipt) || receipt.owner_before(actual->receipt_)) {
    error=Error(std::errc::operation_canceled);return {};
  }
  std::unique_lock lock(actual->mutex_,std::try_to_lock);
  if(!lock.owns_lock()) {error=Error(std::errc::device_or_resource_busy);return {};}
  if(actual->stage_ || !actual->view_) {error=Error(std::errc::operation_canceled);return {};}
  if(actual->denied_mutation_attempted_) {error=Error(std::errc::state_not_recoverable);return {};}
  return actual->view_;
}
std::shared_ptr<NativeProfileGpdReadView> NativeProfileSaveManagedGpd::TryCaptureCurrentFileRead(
    const vfs::File& file,const std::shared_ptr<xam::ProfileGpdModelAuthority>& model,
    uint32_t title,std::error_code& error) {
  error.clear();const auto* actual=dynamic_cast<const NativeProfileGpdReadFile*>(&file);
  if(!actual || !model || !title){error=Error(std::errc::invalid_argument);return {};}
  std::unique_lock lock(actual->mutex_,std::try_to_lock);
  if(!lock.owns_lock()){error=Error(std::errc::device_or_resource_busy);return {};}
  const auto& view=actual->view_;
  if(actual->stage_ || !view || actual->denied_mutation_attempted_ ||
      view->retired_.load(std::memory_order_acquire) || view->read_.origin.owner.lock()!=model ||
      view->read_.origin.storage!=model->storage() || !model->IsCurrent(view->read_.origin) ||
      ReadToken(view->read_,title)!=view->token_){
    error=Error(std::errc::operation_canceled);return {};
  }
  return view;
}
class NativeProfileSaveManagedGpd::ReadAuthority final : public vfs::ManagedFileOpenAuthority {
 private:
  friend class NativeProfileSaveManagedGpd;
  ReadAuthority(std::weak_ptr<NativeProfileSaveManagedGpd> service,
      std::shared_ptr<xam::ProfileGpdModelAuthority> model,xam::ProfileGpdReadCapture read,
      std::shared_ptr<vfs::Device> device,std::string path,
      std::shared_ptr<void> io_duty,std::shared_ptr<const ReadReceipt> receipt,
      std::shared_ptr<const filesystem::SnapshotPublicationSet> issued_members)
      : host_io_duty_(std::move(io_duty)),requires_host_duty_(bool(host_io_duty_)),
        service_(std::move(service)),model_(std::move(model)),
        read_(std::move(read)),requested_(std::move(device)),path_(std::move(path)),receipt_(std::move(receipt)),
        members_(std::move(issued_members)) {}
  // Queued context destruction releases a not-entered duty. A real worker
  // moves it to its OpenRead stack, finishing this FAMILY's VFS/source duty
  // before caller wake; later XFile/guest-output work belongs to common IO22.
  mutable std::mutex duty_mutex_;
  mutable std::shared_ptr<void> host_io_duty_;
  const bool requires_host_duty_;
  std::shared_ptr<void> TakeHostDuty() const {
    std::lock_guard lock(duty_mutex_);return std::exchange(host_io_duty_,{});
  }
  const std::weak_ptr<NativeProfileSaveManagedGpd> service_;
  const std::shared_ptr<xam::ProfileGpdModelAuthority> model_;
  const xam::ProfileGpdReadCapture read_;
  const std::shared_ptr<vfs::Device> requested_;const std::string path_;
  const std::shared_ptr<const ReadReceipt> receipt_;
  const std::shared_ptr<const filesystem::SnapshotPublicationSet> members_;
  mutable std::atomic<bool> consumed_{false};
};
X_STATUS NativeProfileSaveManagedGpd::UpgradeAtActualDbQuiescence(vfs::File& file,
    const std::shared_ptr<NativeProfileGpdReadView>& expected,
    const vfs::FileOpenContext& context,std::span<const uint8_t> native_table) {
  if(global_critical_region::is_held_by_current_thread()) {return X_STATUS_INVALID_DEVICE_REQUEST;}
  auto* actual=dynamic_cast<NativeProfileGpdReadFile*>(&file);
  const auto authority=std::dynamic_pointer_cast<const OpenAuthority>(context.managed_authority);
  if(!actual || !expected || !authority || authority->service_.lock().get()!=this || retiring() ||
      !SameOwner(actual->retained_device(),authority->requested_) ||
      !SameOwner(context.expected_device,authority->requested_) || actual->entry()->path()!=authority->path_ ||
      (!authority->publication_->WriterForStage().model || !source_->Matches(*authority->publication_->WriterForStage().model->storage())) ||
      authority->stage_->publication_.lock()!=authority->publication_) {return X_STATUS_ACCESS_DENIED;}
  const auto requested=std::dynamic_pointer_cast<vfs::HostPathDevice>(authority->requested_);
  if(!requested || requested->is_read_only() || requested->storage()->retiring()) {return X_STATUS_ACCESS_DENIED;}
  if(auto error=authority->stage_->ValidateRequestedView(requested,authority->path_)) {return vfs::HostStorageError(error);}
  std::lock_guard file_lock(actual->mutex_);
  if(actual->stage_ || actual->view_!=expected) {return X_STATUS_ACCESS_DENIED;}
  if(actual->denied_mutation_attempted_) {return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));}
  if(auto error=expected->Available()) {return vfs::HostStorageError(error);}
  const auto& stage=authority->stage_;
  if(stage->before_.token!=expected->token_ || expected->read_.origin.owner.lock()!=authority->publication_->WriterForStage().model) {return X_STATUS_ACCESS_DENIED;}
  // A logical cached table must refer to the exact immutable before layout,
  // not merely an equivalent set of records at different native offsets.
  const auto bytes=expected->token_->bytes();
  if(auto error=save_detail::ValidateNativeGpdSerialization(bytes,bytes,native_table)) {return vfs::HostStorageError(error);}
  std::lock_guard stage_lock(stage->mutex_);
  if(auto error=stage->WriteAvailable()) {return vfs::HostStorageError(error);}
  if(stage->cache_view_claimed_ || stage->managed_views_.load(std::memory_order_acquire) || stage->mutation_attempted_) {
    return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  }
  // The stage was seeded from this exact token and has never entered a
  // mutation or another File owner. Commit contains no allocation, IO, native
  // call or file-pointer replacement; failed checks leave the read view intact.
  // Join the cached File milestone BEFORE its nonfallible stage attachment.
  // The actual native-issued model pending owner survives durable publication
  // until this File's matching committed view is genuinely ready.
  if(authority->consumed_.exchange(true,std::memory_order_acq_rel)) {return X_STATUS_ACCESS_DENIED;}
  if(auto error=authority->publication_->RequireActualCacheCheckpoint())return vfs::HostStorageError(error);
  stage->cache_view_claimed_=true;
  actual->stage_=stage;
  return X_STATUS_SUCCESS;
}
X_STATUS NativeProfileSaveManagedGpd::BindSystemFileBeforeActualEdit(vfs::File& file,
    const std::shared_ptr<NativeProfileGpdReadView>& expected,
    const std::shared_ptr<NativeProfileSavePublication>& publication,std::vector<uint8_t> native_table) {
  if(global_critical_region::is_held_by_current_thread())return X_STATUS_INVALID_DEVICE_REQUEST;
  auto* actual=dynamic_cast<NativeProfileGpdReadFile*>(&file);
  if(!actual || !expected || !publication || retiring() ||
      actual->retained_device()->namespace_retiring())return X_STATUS_ACCESS_DENIED;
  const auto& issuer=publication->system_issuer_;
  const auto model=publication->WriterForStage().model;
  if(!issuer || issuer->title!=title_ || !model || !source_->MatchesMetadata(*model->storage()) ||
      expected->read_.origin.owner.lock()!=model || expected->read_.origin.storage!=model->storage())return X_STATUS_ACCESS_DENIED;
  std::error_code error;if(!publication->CurrentForStage(error))return vfs::HostStorageError(error);
  if(expected->retired_.load(std::memory_order_acquire) || expected->read_.origin.storage->retiring())return X_STATUS_CANCELLED;
  const auto bytes=expected->token_->bytes();
  if((error=save_detail::ValidateNativeGpdSerialization(bytes,bytes,native_table)))return vfs::HostStorageError(error);
  // A Header-first operation must not latch an input from a rejected native
  // cache/table. Its independently issued writer binds only this fully checked
  // later use; this metadata cut creates no stage or filesystem permission.
  auto input=publication->system_file_input_.load(std::memory_order_acquire);
  if(!input && publication->header_issuer_) {
    if(auto error=publication->BindSystemGpdInputAtOriginalEdit(expected,actual->retained_device()))
      return vfs::HostStorageError(error);
    input=publication->system_file_input_.load(std::memory_order_acquire);
  }
  if(!input || input->actual_before_file!=expected ||
      !SameOwner(actual->retained_device(),input->requested_device))return X_STATUS_ACCESS_DENIED;
  // All validation here is owned scalar/control-block/model/raw-vector work.
  // Actual device/physical leaf/token/space checks stay in worker stage Create.
  std::unique_lock file_lock(actual->mutex_,std::try_to_lock);
  if(!file_lock.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  if(actual->stage_ || actual->view_!=expected || actual->system_adoption_bound_ ||
      actual->denied_mutation_attempted_)return X_STATUS_ACCESS_DENIED;
  if((error=publication->RequireActualCacheCheckpoint()))return vfs::HostStorageError(error);
  actual->system_before_header_=std::move(native_table);
  actual->system_publication_=publication;actual->system_service_=weak_from_this();
  actual->system_adoption_bound_=true;return X_STATUS_SUCCESS;
}
X_STATUS NativeProfileSaveManagedGpd::CancelPristineSystemBindingAtActualNoop(vfs::File& file,
    const std::shared_ptr<NativeProfileGpdReadView>& expected,
    const std::shared_ptr<NativeProfileSavePublication>& publication,std::span<const uint8_t> native_table) {
  if(global_critical_region::is_held_by_current_thread() || !expected || !publication)return X_STATUS_INVALID_DEVICE_REQUEST;
  auto* actual=dynamic_cast<NativeProfileGpdReadFile*>(&file);if(!actual)return X_STATUS_ACCESS_DENIED;
  const auto bytes=expected->token_->bytes();
  if(const auto error=save_detail::ValidateNativeGpdSerialization(bytes,bytes,native_table))return vfs::HostStorageError(error);
  std::unique_lock file_lock(actual->mutex_,std::try_to_lock);
  if(!file_lock.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  if(actual->stage_ || actual->view_!=expected || !actual->system_adoption_bound_ ||
      actual->system_publication_.lock()!=publication ||
      actual->system_service_.lock().get()!=this)return X_STATUS_ACCESS_DENIED;
  std::unique_lock serial(publication->mutex_,std::try_to_lock);
  if(!serial.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  if(publication->original_byte_use_open_.load(std::memory_order_acquire) || !publication->system_issuer_ ||
      publication->system_issuer_->title!=title_ ||
      !publication->system_file_input_.load(std::memory_order_acquire) ||
      publication->system_file_input_.load(std::memory_order_acquire)->actual_before_file!=expected ||
      publication->canceled_ || publication->sealed_ || publication->committed_ || publication->transaction_ ||
      !publication->stages_.empty() || !publication->picture_stages_.empty() || !publication->picture_seals_.empty())return X_STATUS_ACCESS_DENIED;
  // Exact old metadata cleanup needs no new source eligibility. No artifact
  // or journal ever existed; the native owner proved complete cache equality.
  const auto error=publication->CancelPristineAtActualNoopWithSerialHeld();
  if(error)return vfs::HostStorageError(error);
  // No fallible operation follows cleanup; unbind this same File only.
  // Preserve any later denied-mutation taint; this is old pending cleanup,
  // never a declaration that a different later native cache is clean.
  actual->system_adoption_bound_=false;
  actual->system_publication_.reset();actual->system_service_.reset();actual->system_before_header_.clear();
  return X_STATUS_SUCCESS;
}
X_STATUS NativeProfileSaveManagedGpd::AdoptBoundSystemStage(NativeProfileGpdReadFile& actual) {
  if(actual.stage_)return X_STATUS_SUCCESS;
  if(!actual.system_adoption_bound_ || actual.denied_mutation_attempted_)return X_STATUS_ACCESS_DENIED;
  const auto publication=actual.system_publication_.lock();const auto service=actual.system_service_.lock();
  if(!publication || !service || service->retiring() || !actual.view_ ||
      actual.retained_device()->namespace_retiring())return X_STATUS_ACCESS_DENIED;
  const auto model=publication->WriterForStage().model;
  if(!model || !service->source_->MatchesMetadata(*model->storage()) ||
      actual.view_->read_.origin.owner.lock()!=model || actual.view_->read_.origin.storage!=model->storage())
    return X_STATUS_ACCESS_DENIED;
  // This is the ordinary backend mutation already inside its genuine File/
  // IO22 host call, not a new observer wait or a native callback. First perform
  // all actual physical source/whole-before/space checks through existing
  // protected stage authority; original committed bytes remain untouched.
  std::error_code error;const auto stage=publication->CreateManagedGpdStage(service->title_,error);
  if(!stage || error)return vfs::HostStorageError(error?error:Error(std::errc::io_error));
  if(stage->before_.token!=actual.view_->token_)return X_STATUS_ACCESS_DENIED;
  const auto requested=std::dynamic_pointer_cast<vfs::HostPathDevice>(actual.retained_device());
  if((error=stage->ValidateRequestedView(requested,actual.entry()->path())))return vfs::HostStorageError(error);
  const auto bytes=actual.view_->token_->bytes();
  if((error=save_detail::ValidateNativeGpdSerialization(bytes,bytes,actual.system_before_header_)))return vfs::HostStorageError(error);
  std::lock_guard stage_lock(stage->mutex_);
  if((error=stage->WriteAvailable()))return vfs::HostStorageError(error);
  if(stage->cache_view_claimed_ || stage->managed_views_.load(std::memory_order_acquire) || stage->mutation_attempted_)
    return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  stage->cache_view_claimed_=true;actual.stage_=stage;
  actual.system_adoption_bound_=false;actual.system_publication_.reset();actual.system_service_.reset();
  actual.system_before_header_.clear();return X_STATUS_SUCCESS;
}
bool NativeProfileSaveManagedGpd::MatchesOwnedPublication(const vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication) {
  const auto* actual=dynamic_cast<const NativeProfileGpdReadFile*>(&file);
  if(!actual || !publication)return false;
  std::unique_lock lock(actual->mutex_,std::try_to_lock);
  return lock.owns_lock() && !actual->denied_mutation_attempted_ &&
      (actual->stage_?actual->stage_->publication_.lock()==publication:
       actual->system_adoption_bound_ && actual->system_publication_.lock()==publication);
}
std::shared_ptr<const NativeProfileGpdCheckpoint>
NativeProfileSaveManagedGpd::CaptureStagedFileCheckpointAtActualDbUse(
    const vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    std::span<const uint8_t> native_index, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !publication) {
    error = Error(std::errc::operation_not_permitted);
    return {};
  }
  const auto* actual = dynamic_cast<const NativeProfileGpdReadFile*>(&file);
  if (!actual) {
    error = Error(std::errc::bad_file_descriptor);
    return {};
  }
  std::unique_lock serial(publication->mutex_, std::try_to_lock);
  if (!serial.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (!publication->RequiresWholeOperationCheckpoint() ||
      !publication->CurrentForDataStage(error) || publication->sealed_ ||
      publication->transaction_) {
    if (!error) {
      error = Error(std::errc::operation_not_permitted);
    }
    return {};
  }
  std::unique_lock file_lock(actual->mutex_, std::try_to_lock);
  if (!file_lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (!actual->stage_ || !actual->view_ || actual->denied_mutation_attempted_ ||
      actual->stage_->publication_.lock() != publication ||
      actual->view_->read_.origin.owner.lock() !=
          publication->WriterForStage().model ||
      actual->view_->read_.origin.storage !=
          publication->WriterForStage().model->storage()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  const auto title = actual->stage_->before_.identity.title_id;
  const auto member = publication->stages_.find(title);
  const auto recorded = publication->staged_checkpoints_.find(title);
  if (member == publication->stages_.end() ||
      member->second != actual->stage_ ||
      recorded == publication->staged_checkpoints_.end() || !recorded->second) {
    error = Error(std::errc::operation_in_progress);
    return {};
  }
  std::unique_lock physical(actual->stage_->mutex_, std::try_to_lock);
  if (!physical.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  const auto& checkpoint = recorded->second;
  if (checkpoint->kind_ != NativeProfileGpdCheckpoint::Kind::kStagedWrite ||
      actual->stage_->retired() ||
      checkpoint->stage_.lock() != actual->stage_ ||
      checkpoint->revision_ != actual->stage_->data_revision_) {
    error = Error(std::errc::state_not_recoverable);
    return {};
  }
  if (native_index.size() != checkpoint->index_.size() ||
      !std::equal(native_index.begin(), native_index.end(),
                  checkpoint->index_.begin())) {
    error = Error(std::errc::bad_message);
    return {};
  }
  // Scalar/owned-memory comparison only: no new filesystem read, raw open,
  // pending model, writer grant, seal, flush, native success or cache death.
  return checkpoint;
}
bool NativeProfileSaveManagedGpd::CheckPristineBoundFileWithSerialAndFileHeld(
    const NativeProfileGpdReadFile& actual,
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    std::span<const uint8_t> native_index, std::error_code& error) {
  const auto service = actual.system_service_.lock();
  const auto model = publication->WriterForStage().model;
  const auto input = publication->system_file_input_.load(std::memory_order_acquire);
  if (!actual.system_adoption_bound_ || actual.stage_ || !actual.view_ ||
      actual.denied_mutation_attempted_ ||
      actual.system_publication_.lock() != publication || !service ||
      !publication->system_issuer_ ||
      publication->system_issuer_->title != service->title_ || !model ||
      !input || input->actual_before_file != actual.view_ ||
      !SameOwner(actual.retained_device(), input->requested_device) ||
      actual.retained_device()->namespace_retiring() || service->retiring() ||
      !service->source_->MatchesMetadata(*model->storage()) ||
      actual.view_->read_.origin.owner.lock() != model ||
      actual.view_->read_.origin.storage != model->storage() ||
      actual.view_->retired_.load(std::memory_order_acquire) ||
      actual.view_->read_.origin.storage->retiring()) {
    error = Error(std::errc::operation_canceled);
    return false;
  }
  if (native_index.size() != actual.system_before_header_.size() ||
      !std::equal(native_index.begin(), native_index.end(),
                  actual.system_before_header_.begin())) {
    error = Error(std::errc::bad_message);
    return false;
  }
  return true;
}
std::shared_ptr<const NativeProfileGpdCheckpoint>
NativeProfileSaveManagedGpd::CapturePristineBoundFileCheckpointAtActualDbUse(
    const vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    std::span<const uint8_t> native_index, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !publication) {
    error = Error(std::errc::operation_not_permitted);
    return {};
  }
  const auto* actual = dynamic_cast<const NativeProfileGpdReadFile*>(&file);
  if (!actual) {
    error = Error(std::errc::bad_file_descriptor);
    return {};
  }
  std::unique_lock serial(publication->mutex_, std::try_to_lock);
  if (!serial.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (!publication->CurrentForDataStage(error) || publication->canceled_ ||
      publication->sealed_ || publication->committed_ || publication->transaction_) {
    if (!error) error = Error(std::errc::operation_not_permitted);
    return {};
  }
  std::unique_lock file_lock(actual->mutex_, std::try_to_lock);
  if (!file_lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (!CheckPristineBoundFileWithSerialAndFileHeld(*actual, publication,
                                                 native_index, error)) return {};
  // Binding already proved this exact immutable source/table. No physical
  // read, parse, hash, Flush or fresh writer is needed at this native CS cut.
  // This capsule owns its copy; no File/publication history retains it.
  try {
    const auto bytes = actual->view_->token_->bytes();
    return std::shared_ptr<const NativeProfileGpdCheckpoint>(
        new NativeProfileGpdCheckpoint(actual, actual->view_, publication,
            actual->system_service_, {bytes.begin(), bytes.end()},
            {native_index.begin(), native_index.end()}));
  } catch (const std::bad_alloc&) {
    error = Error(std::errc::not_enough_memory);
    return {};
  }
}
bool NativeProfileSaveManagedGpd::MatchesPristineBoundBefore(
    const vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    const std::shared_ptr<const NativeProfileGpdCheckpoint>& checkpoint,
    std::span<const uint8_t> native_index, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !publication ||
      !checkpoint ||
      checkpoint->kind_ != NativeProfileGpdCheckpoint::Kind::kPristineBoundBefore ||
      checkpoint->before_file_ != &file ||
      checkpoint->before_publication_.lock() != publication) {
    error = Error(std::errc::operation_not_permitted);
    return false;
  }
  const auto* actual = dynamic_cast<const NativeProfileGpdReadFile*>(&file);
  if (!actual) {
    error = Error(std::errc::bad_file_descriptor);
    return false;
  }
  std::unique_lock serial(publication->mutex_, std::try_to_lock);
  if (!serial.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return false;
  }
  std::unique_lock file_lock(actual->mutex_, std::try_to_lock);
  if (!file_lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return false;
  }
  const auto service = checkpoint->before_service_.lock();
  if (checkpoint->before_view_.lock() != actual->view_ || !service ||
      service != actual->system_service_.lock() ||
      checkpoint->index_.size() != native_index.size() ||
      !std::equal(native_index.begin(), native_index.end(),
                  checkpoint->index_.begin())) {
    error = Error(std::errc::operation_canceled);
    return false;
  }
  // This deliberately does not call source Current or read physical metadata.
  // A successful own publication need not destroy its legitimate cached before
  // File. This fact grants no current-map authority: the caller must join the
  // actual unchanged publication token and its exact source/epoch transition.
  return CheckPristineBoundFileWithSerialAndFileHeld(*actual, publication,
                                                    native_index, error);
}
struct NativeProfileSaveManagedGpd::PreparedCachePromotion::Data {
  std::shared_ptr<NativeProfileSavePublication> publication;
  std::shared_ptr<NativeProfileSaveGpdStage> stage, old_stage;
  std::shared_ptr<NativeProfileGpdReadView> next_view, old_view;
  NativeProfileGpdReadFile* file = nullptr;
  uint32_t title = 0;
  bool applied = false;
  std::unique_lock<std::mutex> file_lock, stage_lock;
  ~Data() {
    // Caller destroys outside the aggregate serial/model/Header locks. These
    // exact File/stage locks still must unwind before ANY retained old owner.
    if (stage_lock.owns_lock()) {
      stage_lock.unlock();
    }
    if (file_lock.owns_lock()) {
      file_lock.unlock();
    }
  }
};
NativeProfileSaveManagedGpd::PreparedCachePromotion::PreparedCachePromotion(
    std::unique_ptr<Data> data)
    : data_(std::move(data)) {}
NativeProfileSaveManagedGpd::PreparedCachePromotion::~PreparedCachePromotion() =
    default;
bool NativeProfileSaveManagedGpd::PreparedCachePromotion::MatchesPublication(
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    uint32_t title,
    const std::shared_ptr<NativeProfileSaveGpdStage>& stage) const noexcept {
  return data_ && !data_->applied && data_->publication == publication &&
         data_->title == title && data_->stage == stage &&
         data_->file_lock.owns_lock() && data_->stage_lock.owns_lock();
}
void NativeProfileSaveManagedGpd::PreparedCachePromotion::Apply() noexcept {
  NativeProfileSaveManagedGpd::ApplyPreparedCache(*this);
}
void NativeProfileSaveManagedGpd::ApplyPreparedCache(
    PreparedCachePromotion& prepared) noexcept {
  auto& data = *prepared.data_;
  if (data.applied) {
    return;
  }
  data.old_view = std::move(data.file->view_);
  data.old_stage = std::move(data.file->stage_);
  data.file->view_ = std::move(data.next_view);
  data.file->denied_mutation_attempted_ = false;
  data.old_stage->retired_.store(true, std::memory_order_release);
  data.applied = true;
}
void NativeProfileSaveManagedGpd::PreparedCachePromotion::
    ReleaseLocksAfterApply() noexcept {
  if (!data_->applied) {
    return;
  }
  if (data_->stage_lock.owns_lock()) {
    data_->stage_lock.unlock();
  }
  if (data_->file_lock.owns_lock()) {
    data_->file_lock.unlock();
  }
}
std::unique_ptr<NativeProfileSaveManagedGpd::PreparedCachePromotion>
NativeProfileSaveManagedGpd::PrepareCommittedCacheWithSerialHeld(
    vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    std::span<const uint8_t> native_table, std::error_code& error, bool wait) {
  error.clear();
  const auto fail =
      [&](std::errc why) -> std::unique_ptr<PreparedCachePromotion> {
    error = Error(why);
    return {};
  };
  if (global_critical_region::is_held_by_current_thread()) {
    return fail(std::errc::operation_not_permitted);
  }
  auto* actual = dynamic_cast<NativeProfileGpdReadFile*>(&file);
  if (!actual || !publication || retiring()) {
    return fail(std::errc::permission_denied);
  }
  const auto model = publication->WriterForStage().model;
  if (!model || !source_->Matches(*model->storage())) {
    return fail(std::errc::permission_denied);
  }
  if (!publication->committed_ || !publication->sealed_ ||
      publication->canceled_ || !publication->transaction_) {
    return fail(std::errc::device_or_resource_busy);
  }
  const auto member = publication->stages_.find(title_);
  if (member == publication->stages_.end() || !member->second ||
      member->second->publication_.lock() != publication) {
    return fail(std::errc::permission_denied);
  }
  const auto plan = publication->pending_->BuildPlan(error);
  if (!plan) {
    return {};
  }
  const auto intended = plan->files.find(title_);
  if (intended == plan->files.end()) {
    return fail(std::errc::permission_denied);
  }
  const auto sealed =
      intended->second.action == xam::ProfileGpdFileAction::kUnchanged
          ? intended->second.before.token->bytes()
          : std::span<const uint8_t>(intended->second.intended);
  const auto read = model->CaptureRead(error);
  if (!read) {
    return {};
  }
  const auto token = ReadToken(*read, title_);
  if (!token || token->bytes().size() != sealed.size() ||
      !std::equal(sealed.begin(), sealed.end(), token->bytes().begin())) {
    return fail(std::errc::state_not_recoverable);
  }
  error = save_detail::ValidateNativeGpdSerialization(sealed, token->bytes(),
                                                      native_table);
  if (error) {
    return {};
  }
  const auto* members = publication->joined_artifacts_
                            ? publication->joined_artifacts_->members().get()
                            : nullptr;
  auto view =
      NativeProfileGpdReadView::Capture(model, *read, title_, error, members);
  if (!view) {
    return {};
  }
  auto data = std::make_unique<PreparedCachePromotion::Data>();
  data->publication = publication;
  data->stage = member->second;
  data->next_view = std::move(view);
  data->file = actual;
  data->title = title_;
  data->file_lock = std::unique_lock(actual->mutex_, std::defer_lock);
  if (wait) {
    data->file_lock.lock();
  } else if (!data->file_lock.try_lock()) {
    return fail(std::errc::device_or_resource_busy);
  }
  if (actual->denied_mutation_attempted_ || actual->stage_ != member->second ||
      !actual->view_ || actual->view_->read_.origin.owner.lock() != model ||
      actual->view_->read_.origin.storage != read->origin.storage) {
    return fail(std::errc::permission_denied);
  }
  data->stage_lock = std::unique_lock(member->second->mutex_, std::defer_lock);
  if (wait) {
    data->stage_lock.lock();
  } else if (!data->stage_lock.try_lock()) {
    return fail(std::errc::device_or_resource_busy);
  }
  if (member->second->retired() || !member->second->sealed_ ||
      !member->second->cache_view_claimed_ ||
      member->second->managed_views_.load(std::memory_order_acquire) ||
      !model->IsCurrent(read->origin)) {
    return fail(std::errc::device_or_resource_busy);
  }
  // No source/layout allocation or other fallible step remains in Apply.
  return std::unique_ptr<PreparedCachePromotion>(
      new PreparedCachePromotion(std::move(data)));
}
X_STATUS NativeProfileSaveManagedGpd::PromoteCommittedAtActualDbQuiescence(
    vfs::File& file,
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    std::span<const uint8_t> native_table) {
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  if (!dynamic_cast<NativeProfileGpdReadFile*>(&file) || !publication ||
      retiring()) {
    return X_STATUS_ACCESS_DENIED;
  }
  const auto model = publication->WriterForStage().model;
  if (!model || !source_->Matches(*model->storage())) {
    return X_STATUS_ACCESS_DENIED;
  }
  std::unique_ptr<PreparedCachePromotion> prepared;
  std::unique_ptr<NativeProfileSavePublication::CacheCheckpoint> checkpoint;
  {
    std::unique_lock serial(publication->mutex_, std::try_to_lock);
    if (!serial.owns_lock()) {
      return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
    }
    std::error_code error;
    prepared = PrepareCommittedCacheWithSerialHeld(file, publication,
                                                   native_table, error, true);
    if (!prepared) {
      return vfs::HostStorageError(error);
    }
    checkpoint = publication->PrepareActualCacheCheckpoint(
        title_, prepared->data_->stage, error);
    if (!checkpoint) {
      return vfs::HostStorageError(error);
    }
    prepared->Apply();
    checkpoint->Apply();
    publication->cache_checkpoint_complete_ = true;
  }
  // These retain native/host source holders. Destruction follows publication
  // and model guards, then releases File/stage locks before old read objects.
  prepared->ReleaseLocksAfterApply();
  checkpoint.reset();
  prepared.reset();
  return X_STATUS_SUCCESS;
}
NativeProfileSaveManagedGpd::NativeProfileSaveManagedGpd(std::shared_ptr<NativeProfileSaveSource> source,
    uint32_t title,std::filesystem::path path)
    : source_(std::move(source)),title_(title),path_(std::move(path)) {}
std::shared_ptr<NativeProfileSaveManagedGpd> NativeProfileSaveManagedGpd::CreateSource(
    std::shared_ptr<xam::ProfileGpdModelAuthority> model,uint32_t title,std::error_code& error,
    std::shared_ptr<const std::atomic<bool>> family_closed) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || !model || !title) {error=Error(std::errc::invalid_argument);return {};}
  const auto read=model->CaptureRead(error);if(!read) {return {};}
  const auto token=ReadToken(*read,title);
  if(!token || !token->exists()) {error=Error(std::errc::no_such_file_or_directory);return {};}
  auto source=NativeProfileSaveSource::Capture(model->storage(),error,std::move(family_closed));if(!source) {return {};}
  const auto path=(model->storage()->view()->path()/token->relative()).lexically_normal();
  if((error=source->JoinCommittedReadModel(model)))return {};
  return std::shared_ptr<NativeProfileSaveManagedGpd>(new NativeProfileSaveManagedGpd(std::move(source),title,path));
}
std::shared_ptr<NativeProfileSaveManagedGpd> NativeProfileSaveManagedGpd::Create(
    std::shared_ptr<NativeProfileSaveGpdStage> stage,std::error_code& error) {
  error=stage ? stage->ReadAvailable() : Error(std::errc::invalid_argument);if(error) {return {};}
  const auto publication=stage->publication_.lock();if(!publication) {error=Error(std::errc::operation_canceled);return {};}
  return CreateSource(publication->WriterForStage().model,stage->before_.identity.title_id,error);
}
std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
NativeProfileSaveManagedGpd::backing_directory() const {return source_->parent_;}
bool NativeProfileSaveManagedGpd::retiring() const {return source_->retiring();}
std::shared_ptr<const vfs::ManagedFileOpenAuthority> NativeProfileSaveManagedGpd::IssueForActualOpen(
    const std::shared_ptr<NativeProfileSavePublication>& publication,
    std::shared_ptr<vfs::Device> requested,std::string path,std::error_code& error) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || !publication || !requested || path.empty() || retiring() ||
      !publication->WriterForStage().model || !source_->MatchesMetadata(*publication->WriterForStage().model->storage()) || publication->pending_->command().title_id!=title_) {
    error=Error(std::errc::operation_canceled);return {};
  }
  std::unique_lock serial(publication->mutex_,std::try_to_lock);
  if(!serial.owns_lock()) {error=Error(std::errc::device_or_resource_busy);return {};}
  if(publication->canceled_ || publication->sealed_ || !publication->CurrentForStage(error)) {
    if(!error) {error=Error(std::errc::operation_canceled);}return {};
  }
  const auto stage=publication->stages_.find(title_);
  if(stage==publication->stages_.end() || !stage->second) {error=Error(std::errc::invalid_argument);return {};}
  // Caller capture is metadata-only. OpenWithContext performs the SAME full
  // physical source/leaf/stage proof in the actual owned IO closure before
  // disposition. This token alone never permits raw or unrelated policy IO.
  return std::shared_ptr<const vfs::ManagedFileOpenAuthority>(new OpenAuthority(
      weak_from_this(),publication,stage->second,std::move(requested),std::move(path)));
}
std::shared_ptr<const vfs::ManagedFileOpenAuthority> NativeProfileSaveManagedGpd::IssueReadForActualOpen(
    xam::ProfileGpdReadCapture read,std::shared_ptr<vfs::Device> requested,std::string path,std::error_code& error,
    std::shared_ptr<void> host_io_duty,std::shared_ptr<const ReadReceipt> receipt,
    std::shared_ptr<const filesystem::SnapshotPublicationSet> issued_members) {
  error.clear();const auto model=read.origin.owner.lock();
  if(global_critical_region::is_held_by_current_thread() || !requested || path.empty() || retiring() ||
      !model || read.origin.storage!=model->storage() || !source_->MatchesMetadata(*model->storage()) || !model->IsCurrent(read.origin)) {
    error=Error(std::errc::operation_canceled);return {};
  }
  const auto token=ReadToken(read,title_);
  if(!token || !token->exists()) {error=Error(std::errc::no_such_file_or_directory);return {};}
  if(!receipt)receipt=IssueReadReceipt();
  return std::shared_ptr<const vfs::ManagedFileOpenAuthority>(new ReadAuthority(
      weak_from_this(),model,std::move(read),std::move(requested),std::move(path),
      std::move(host_io_duty),std::move(receipt),std::move(issued_members)));
}
 std::shared_ptr<const vfs::ManagedFileOpenAuthority>
NativeProfileSaveManagedGpd::IssueParentReadForActualOpen(
    const std::shared_ptr<NativeProfileSavePublication>& publication,std::shared_ptr<vfs::Device> requested,
    std::string path,std::shared_ptr<void> duty,std::shared_ptr<const ReadReceipt> receipt,std::error_code& error) {
  error.clear();
  if(!publication || !publication->parent_operation_ || !publication->system_issuer_ ||
      publication->system_issuer_->title!=title_ || !duty) {
    error=Error(std::errc::permission_denied);return {};
  }
  auto read=publication->WriterForStage().model->CaptureRead(error);if(!read)return {};
  const auto members=publication->CaptureParentReadMembers(*read,error);if(!members)return {};
  return IssueReadForActualOpen(std::move(*read),std::move(requested),std::move(path),error,
      std::move(duty),std::move(receipt),members);
}
std::error_code NativeProfileSaveManagedGpd::JoinCommittedReadModel(
    const std::shared_ptr<xam::ProfileGpdModelAuthority>& model) {
  return source_->JoinCommittedReadModel(model);
}
X_STATUS NativeProfileSaveManagedGpd::Open(std::shared_ptr<vfs::Device> device,std::string path,
    vfs::FileDisposition disposition,uint32_t access,bool directory,vfs::File** file,vfs::FileAction* action) {
  return OpenCommitted(std::move(device),std::move(path),disposition,access,directory,file,action,{});
}
X_STATUS NativeProfileSaveManagedGpd::OpenCommitted(std::shared_ptr<vfs::Device> device,std::string path,
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
X_STATUS NativeProfileSaveManagedGpd::OpenRead(const std::shared_ptr<const ReadAuthority>& authority,
    const std::shared_ptr<vfs::Device>& requested,const std::string& path,vfs::FileDisposition disposition,
    uint32_t access,vfs::File** output,vfs::FileAction* action,const vfs::FileOpenContext& context) {
  // Declared before all worker guards: source/namespace/physical admission
  // finishes before this actual host duty leaves. A duplicate native authority
  // cannot replay an already completed original source-open duty.
  const auto actual_host_duty=authority->TakeHostDuty();
  if(authority->requires_host_duty_ && !actual_host_duty)return X_STATUS_ACCESS_DENIED;
  if(authority->service_.lock().get()!=this || retiring() || !SameOwner(requested,authority->requested_) ||
      !SameOwner(context.expected_device,authority->requested_) || path!=authority->path_) {return X_STATUS_ACCESS_DENIED;}
  if(disposition!=vfs::FileDisposition::kOpen && disposition!=vfs::FileDisposition::kOpenIf) {return X_STATUS_ACCESS_DENIED;}
  const auto host=std::dynamic_pointer_cast<vfs::HostPathDevice>(requested);
  const auto token=ReadToken(authority->read_,title_);
  if(!host || !token || !source_->Matches(*authority->model_->storage()) ||
      !PhysicalLeaf(authority->model_->storage(),host,path,*token)) {return X_STATUS_ACCESS_DENIED;}
  if(authority->consumed_.exchange(true,std::memory_order_acq_rel)) {return X_STATUS_ACCESS_DENIED;}
  std::error_code error;auto view=NativeProfileGpdReadView::Capture(authority->model_,authority->read_,title_,error,authority->members_.get());
  if(!view) {return vfs::HostStorageError(error);}
  auto information=view->information_;
  const uint64_t unit=uint64_t(host->sectors_per_allocation_unit())*host->bytes_per_sector();
  const auto rounded=vfs::RoundFileAllocationSize(view->native_allocated_bytes_,unit,&information.allocation_size);
  if(XFAILED(rounded)) {return rounded;}
  auto lock=global_critical_region::AcquireDirect();
  if(retiring() || host->namespace_retiring() || host->storage()->retiring()) {return X_STATUS_CANCELLED;}
  auto* root=host->ResolvePath("");if(!root) {return X_STATUS_OBJECT_PATH_NOT_FOUND;}
  auto entry=std::make_unique<NativeGpdReadEntry>(host.get(),root,path,information,view->token_->root()->path()/view->token_->relative());
  *output=new NativeProfileGpdReadFile(access,std::move(entry),std::move(view),authority->receipt_);
  *action=vfs::FileAction::kOpened;return X_STATUS_SUCCESS;
}
X_STATUS NativeProfileSaveManagedGpd::OpenWithContext(std::shared_ptr<vfs::Device> requested,
    std::string path,vfs::FileDisposition disposition,uint32_t access,bool directory,
    vfs::File** output,vfs::FileAction* action,const vfs::FileOpenContext& context) {
  if(!output || !action) {return X_STATUS_INVALID_PARAMETER;}
  *output=nullptr;*action=vfs::FileAction::kDoesNotExist;
  if(directory) {return X_STATUS_NOT_A_DIRECTORY;}
  if(global_critical_region::is_held_by_current_thread()) {return X_STATUS_INVALID_DEVICE_REQUEST;}
  if(uint32_t(disposition)>uint32_t(vfs::FileDisposition::kOverwriteIf)) {return X_STATUS_INVALID_PARAMETER;}
  if(!context.managed_authority)return OpenCommitted(requested,path,disposition,access,directory,output,action,context);
  if(const auto read=std::dynamic_pointer_cast<const ReadAuthority>(context.managed_authority)) {
    return OpenRead(read,requested,path,disposition,access,output,action,context);
  }
  const auto authority=std::dynamic_pointer_cast<const OpenAuthority>(context.managed_authority);
  if(!authority || authority->service_.lock().get()!=this || retiring() ||
      authority->stage_->publication_.lock()!=authority->publication_ || (!authority->publication_->WriterForStage().model || !source_->Matches(*authority->publication_->WriterForStage().model->storage())) ||
      !SameOwner(requested,authority->requested_) || !SameOwner(context.expected_device,authority->requested_) || path!=authority->path_) {
    return X_STATUS_ACCESS_DENIED;
  }
  std::error_code error;if(!authority->publication_->CurrentForStage(error)) {return vfs::HostStorageError(error);}
  const auto host=std::dynamic_pointer_cast<vfs::HostPathDevice>(requested);
  if(!host || host->is_read_only() || host->storage()->retiring()) {return X_STATUS_ACCESS_DENIED;}
  const bool exists=authority->stage_->before_.token->exists();
  if(disposition==vfs::FileDisposition::kCreate && exists) {*action=vfs::FileAction::kExists;return X_STATUS_OBJECT_NAME_COLLISION;}
  if(!exists && (disposition==vfs::FileDisposition::kOpen || disposition==vfs::FileDisposition::kOverwrite)) {return X_STATUS_OBJECT_NAME_NOT_FOUND;}
  const bool truncate=disposition==vfs::FileDisposition::kSuperscede || disposition==vfs::FileDisposition::kOverwrite || disposition==vfs::FileDisposition::kOverwriteIf;
  if(truncate && !(access&(vfs::FileAccess::kFileWriteData|vfs::FileAccess::kFileAppendData|vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericAll))) {return X_STATUS_ACCESS_DENIED;}
  error=authority->stage_->ValidateRequestedView(host,path);if(error) {return vfs::HostStorageError(error);}
  if(authority->consumed_.exchange(true,std::memory_order_acq_rel)) {return X_STATUS_ACCESS_DENIED;}
  if(truncate) {error=authority->stage_->Resize(0);if(error) {return vfs::HostStorageError(error);}}
  const auto status=authority->stage_->OpenManagedView(host,std::move(path),access,output);
  if(XFAILED(status)) {return status;}
  *action=!exists ? vfs::FileAction::kCreated : truncate ?
      (disposition==vfs::FileDisposition::kSuperscede ? vfs::FileAction::kSuperseded : vfs::FileAction::kOverwritten) : vfs::FileAction::kOpened;
  return X_STATUS_SUCCESS;
}
}  // namespace xe::kernel
