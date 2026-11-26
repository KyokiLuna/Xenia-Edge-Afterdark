/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_publication.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include "xenia/base/string.h"
#include "xenia/base/byte_order.h"
#include <new>
#include <utility>
#include "xenia/base/mutex.h"
#include "xenia/kernel/native_profile_save_serialization.h"
#include "xenia/kernel/native_profile_save_gpd_stage.h"
#include "xenia/kernel/native_profile_save_picture_stage.h"
#include "xenia/kernel/native_profile_gpd_read_view.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
#include "xenia/vfs/folder_content_package.h"
#include "xenia/kernel/xam/native_folder_content.h"
#include "xenia/vfs/host_storage_errors.h"
namespace xe::kernel {
namespace {
std::error_code Error(std::errc value) { return std::make_error_code(value); }
template<class T>
bool SameOwner(const std::shared_ptr<T>& a,const std::shared_ptr<T>& b) noexcept {
  return a==b && !a.owner_before(b) && !b.owner_before(a);
}
}
std::shared_ptr<NativeProfileSavePublication> NativeProfileSavePublication::Prepare(
    std::shared_ptr<xam::ProfileRosterNativeWriter> source,
    xam::ProfileGpdMutation command, std::error_code& error) {
  error.clear();
  using Kind = xam::ProfileGpdMutationKind;
  if (!source || (command.kind != Kind::kUpsertImage &&
      command.kind != Kind::kDeleteImage && command.kind != Kind::kSetPictures)) {
    error = Error(std::errc::invalid_argument); return {};
  }
  if (!source->Current(error)) { return {}; }
  auto owner = std::shared_ptr<NativeProfileSavePublication>(new NativeProfileSavePublication);
  owner->source_ = std::move(source);
  bool retry = false;
  const auto& writer = owner->source_->writer();
  owner->pending_ = writer.model->BeginNative(std::move(command), retry, error,
                                             owner->source_->origin().stamp, owner);
  if (!owner->pending_) { return {}; }
  return owner;
}
bool NativeProfileSavePublication::CheckSystemOperation(const SystemFileIssuer& issuer,std::error_code& error) {
  error.clear();
  if(!issuer.source || !issuer.actual_semantic_use || !issuer.actual_semantic_use->unchanged() ||
      !issuer.domain || !issuer.root_generation || !issuer.invocation || !issuer.title ||
      (issuer.gpd_mode!=SystemGpdMode::kNativeWriteRequired && issuer.gpd_mode!=SystemGpdMode::kNativeUnchangedNoWrite) ||
      issuer.required_artifacts.size()>7 || (!issuer.required_artifacts.empty() && issuer.title!=0xFFFE07D1)) {
    error=Error(std::errc::invalid_argument);return false;
  }
  if(!issuer.source->Current(error))return false;
  std::set<uint64_t> kinds;
  for(const auto kind:issuer.required_artifacts) {
    const auto value=uint64_t(kind);
    if(value<uint64_t(filesystem::ProfileArtifactKind::kPublic64) ||
        value>uint64_t(filesystem::ProfileArtifactKind::kPackageHeader) || !kinds.insert(value).second) {
      error=Error(std::errc::invalid_argument);return false;
    }
  }
  return true;
}
bool NativeProfileSavePublication::FinishSystemPreparation(
    const std::shared_ptr<NativeProfileSavePublication>& owner,std::error_code& error) {
  if(!owner || !owner->system_issuer_ || !owner->source_) {
    error=Error(std::errc::invalid_argument);return false;
  }
  const auto& issuer=*owner->system_issuer_;const auto& writer=owner->source_->writer();
  if(!writer.model || !writer.owner || !writer.recovery || !writer.metadata ||
      !writer.model->storage() || !writer.owner->managed_identity()) {
    error=Error(std::errc::no_such_device);return false;
  }
  const auto loads=writer.model->CaptureLoads(error);if(!loads)return false;
  auto baseline=std::make_shared<xam::ProfileGpdModelPlan>();
  for(const auto& [title,load]:*loads) {
    if(!load.token){error=Error(std::errc::bad_message);return false;}
    xam::ProfileGpdPlannedFile member;member.before=load;baseline->files.emplace(title,std::move(member));
  }
  const auto actual=baseline->files.find(issuer.title);
  if(actual==baseline->files.end() || !actual->second.before.readable()) {
    error=Error(std::errc::operation_not_supported);return false;
  }
  // Baseline is the actual original raw tuple, never a HOST kFlush rewrite.
  // Every retained binding/control block is installed before native pending.
  xam::ProfileGpdMutation command;command.kind=xam::ProfileGpdMutationKind::kFlush;
  command.title_id=issuer.title;command.request_id=writer.model->AllocateRequestId(error);
  if(error)return false;
  bool retry=false;owner->pending_=writer.model->BeginNative(std::move(command),retry,error,
      owner->source_->origin().stamp,owner,std::move(baseline));
  return bool(owner->pending_);
}
std::shared_ptr<NativeProfileSavePublication> NativeProfileSavePublication::PrepareSystemFile(
    SystemFileIssuer issuer,std::error_code& error) {
  if(!CheckSystemOperation(issuer,error))return {};
  if(issuer.gpd_mode!=SystemGpdMode::kNativeWriteRequired || !issuer.actual_before_file || !issuer.requested_device) {
    error=Error(std::errc::invalid_argument);return {};
  }
  if(issuer.actual_before_file->retired_.load(std::memory_order_acquire)) {
    error=Error(std::errc::operation_canceled);return {};
  }
  const auto& writer=issuer.source->writer();const auto& read=issuer.actual_before_file->read_;
  if(!writer.model || read.origin.owner.lock()!=writer.model || read.origin.storage!=writer.model->storage() ||
      !writer.model->IsCurrent(read.origin) || !issuer.actual_before_file->token_ || !issuer.actual_before_file->token_->exists()) {
    error=Error(std::errc::operation_canceled);return {};
  }
  char expected[13];std::snprintf(expected,sizeof(expected),"%08X.gpd",issuer.title);
  const auto& token=issuer.actual_before_file->token_;
  if(token->root()!=read.origin.storage->view() || token->relative().has_parent_path() ||
      !token->ascii_folded_leaf() || !xe::utf8::equal_case(token->relative().generic_string(),expected)) {
    error=Error(std::errc::invalid_argument);return {};
  }
  auto input=std::make_shared<const SystemGpdFileInput>(SystemGpdFileInput{issuer.actual_before_file,issuer.requested_device});
  auto owner=std::shared_ptr<NativeProfileSavePublication>(new NativeProfileSavePublication);
  owner->source_=issuer.source;owner->system_issuer_=std::move(issuer);
  owner->system_file_input_.store(std::move(input),std::memory_order_release);
  return FinishSystemPreparation(owner,error)?owner:nullptr;
}
std::shared_ptr<NativeProfileSavePublication> NativeProfileSavePublication::PrepareSystemParentOperation(
    SystemFileIssuer issuer,std::error_code& error) {
  if(!CheckSystemOperation(issuer,error))return {};
  if(issuer.actual_before_file || issuer.requested_device || issuer.title!=0xFFFE07D1 ||
      std::find(issuer.required_artifacts.begin(),issuer.required_artifacts.end(),
          filesystem::ProfileArtifactKind::kPackageHeader)==issuer.required_artifacts.end()) {
    error=Error(std::errc::invalid_argument);return {};
  }
  issuer.defer_until_whole_checkpoint=true;
  auto owner=std::shared_ptr<NativeProfileSavePublication>(new NativeProfileSavePublication);
  owner->source_=issuer.source;owner->system_issuer_=std::move(issuer);owner->parent_operation_=true;
  return FinishSystemPreparation(owner,error)?owner:nullptr;
}
std::error_code NativeProfileSavePublication::PrepareParentArtifactsAtOriginalIo() {
  if(global_critical_region::is_held_by_current_thread())return Error(std::errc::operation_would_block);
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
  if(!parent_operation_ || !system_issuer_ || canceled_ || sealed_ || transaction_)
    return Error(std::errc::operation_not_permitted);
  std::error_code error;if(!CurrentForStage(error))return error;
  if(parent_admission_attempted_)return joined_artifacts_ && !joined_artifact_error_ ?
      std::error_code{}:joined_artifact_error_?joined_artifact_error_:Error(std::errc::operation_canceled);
  if(!stages_.empty() || !picture_stages_.empty())return Error(std::errc::operation_not_permitted);
  const auto storage=source_->writer().model->storage();
  if(!storage || !storage->device())return Error(std::errc::no_such_device);
  // This is a lookup of the existing CM-issued policy, followed by exact
  // source/profile/physical identity checks. The pathname grants NO authority.
  auto policy=std::dynamic_pointer_cast<xam::NativeFolderContent>(
      storage->device()->ManagedFile(storage->view()->path()));
  if(!policy)return Error(std::errc::operation_not_supported);
  auto before=policy->CaptureForParentAdmission(*storage,error);if(!before)return error;
  // The real parent can skip C628/NtFlush when its descriptor is unchanged.
  // Reserve the SAME model/cache milestone BEFORE the Header factory's first
  // physical effect. A transient model try-lock refusal consumes no admission
  // and can be retried by the original caller without orphaning a stage.
  if(const auto cache=RequireActualCacheCheckpoint())return cache;
  parent_header_policy_=std::move(policy);header_before_at_io_=before;
  // Mark before the factory's first physical effect. An uncertain factory
  // result is retained and cannot be retried as a fresh before tuple.
  parent_admission_attempted_=true;
  const auto header=AcquireHeaderWithSerialHeld(std::move(before),error);
  if(error)joined_artifact_error_=error;
  return header && !error?std::error_code{}:error?error:Error(std::errc::io_error);
}
std::pair<std::shared_ptr<xam::NativeFolderContent>,std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>>
NativeProfileSavePublication::CaptureParentHeaderRead(std::error_code& error) {
  error.clear();std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock()){error=Error(std::errc::device_or_resource_busy);return {};}
  if(!parent_operation_ || !parent_admission_attempted_ || !joined_artifacts_ ||
      !joined_artifacts_->header() || joined_artifact_error_ || !parent_header_policy_ || canceled_ ||
      !CurrentForStage(error)) {
    if(!error)error=joined_artifact_error_?joined_artifact_error_:Error(std::errc::operation_canceled);return {};
  }
  return {parent_header_policy_,joined_artifacts_->header()};
}
std::shared_ptr<const vfs::ManagedFileOpenAuthority>
NativeProfileSavePublication::IssueParentHeaderReadForActualOpen(std::shared_ptr<vfs::Device> device,
    std::string path,std::shared_ptr<void> duty,std::error_code& error) {
  const auto [policy,issued]=CaptureParentHeaderRead(error);
  return policy && issued ? policy->IssueParentReadForActualOpen(
      issued,std::move(device),std::move(path),std::move(duty),error):nullptr;
}
std::shared_ptr<const filesystem::SnapshotPublicationSet>
NativeProfileSavePublication::CaptureParentReadMembers(const xam::ProfileGpdReadCapture& read,
    std::error_code& error) {
  error.clear();std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock()){error=Error(std::errc::device_or_resource_busy);return {};}
  if(!parent_operation_ || !parent_admission_attempted_ || !joined_artifacts_ ||
      joined_artifact_error_ || !joined_artifacts_->members() || !system_issuer_ ||
      system_issuer_->gpd_mode!=SystemGpdMode::kNativeWriteRequired || canceled_ || sealed_ || transaction_ ||
      read.origin.owner.lock()!=source_->writer().model || read.origin.storage!=pending_->origin().storage ||
      read.origin.xuid!=pending_->origin().xuid || read.origin.user_slot!=pending_->origin().user_slot ||
      read.origin.stamp.sign_in_generation!=pending_->origin().stamp.sign_in_generation ||
      read.origin.stamp.model_version!=pending_->origin().stamp.model_version || !CurrentForStage(error)) {
    if(!error)error=joined_artifact_error_?joined_artifact_error_:Error(std::errc::operation_canceled);return {};
  }
  // The operation already owns the real full member reservation. Retained
  // immutable before reads use that authority; a public snapshot recapture
  // would conflict with our OWN admission. No stage or writer is issued.
  const auto plan=pending_->BuildPlan(error);if(!plan)return {};
  for(const auto& [title,member]:plan->files) {
    if(!member.before.token || std::find(read.tokens.begin(),read.tokens.end(),member.before.token)==read.tokens.end()) {
      error=Error(std::errc::permission_denied);return {};
    }
  }
  return joined_artifacts_->members();
}
std::error_code NativeProfileSavePublication::BindSystemHeaderInputAtOriginalEdit(SystemHeaderIssuer issuer) {
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
  if(!parent_operation_ || !parent_admission_attempted_ || !joined_artifacts_ ||
      joined_artifact_error_ || !joined_artifacts_->header() || !system_issuer_ ||
      issuer.operation.source!=source_ || issuer.operation.invocation!=system_issuer_->invocation ||
      issuer.operation.domain!=system_issuer_->domain || issuer.operation.root_generation!=system_issuer_->root_generation ||
      !issuer.actual_backing_file || !issuer.actual_backing_file->file() || !issuer.actual_package ||
      issuer.actual_package!=issuer.actual_backing_file->file()->folder_content_package() ||
      issuer.actual_native_before_header.size()!=0xA000 || !header_before_at_io_ ||
      header_before_at_io_->bytes().size()<0xA000 ||
      !std::equal(issuer.actual_native_before_header.begin(),issuer.actual_native_before_header.end(),
          header_before_at_io_->bytes().begin()))return Error(std::errc::permission_denied);
  std::error_code error;if(!CurrentForStage(error))return error;
  if(header_issuer_)return header_issuer_->actual_backing_file.get()==issuer.actual_backing_file.get() &&
      header_issuer_->actual_package==issuer.actual_package &&
      header_issuer_->actual_native_before_header==issuer.actual_native_before_header ?
      std::error_code{}:Error(std::errc::operation_not_permitted);
  // No second session/stage, no recapture after earlier output. This actual
  // raw context intersects the SAME Header expected token/admission.
  system_issuer_->requested_device=issuer.actual_backing_file->file()->retained_device();
  issuer.operation=*system_issuer_;
  header_issuer_=std::move(issuer);return {};
}
std::shared_ptr<NativeProfileSavePublication> NativeProfileSavePublication::PrepareSystemHeaderOperation(
    SystemHeaderIssuer issuer,std::error_code& error) {
  if(!CheckSystemOperation(issuer.operation,error))return {};
  const auto& operation=issuer.operation;
  const auto* file=issuer.actual_backing_file?issuer.actual_backing_file->file():nullptr;
  if(operation.gpd_mode!=SystemGpdMode::kNativeWriteRequired || operation.actual_before_file ||
      operation.title!=0xFFFE07D1 || !file || !issuer.actual_package ||
      issuer.actual_package->retiring() || !SameOwner(file->folder_content_package(),issuer.actual_package) ||
      !file->retained_device() || file->retained_device()->namespace_retiring() ||
      (operation.requested_device && !SameOwner(operation.requested_device,file->retained_device())) ||
      issuer.actual_native_before_header.size()!=0xA000 ||
      xe::load_and_swap<uint32_t>(issuer.actual_native_before_header.data())!=0x434F4E20 ||
      xe::load_and_swap<uint32_t>(issuer.actual_native_before_header.data()+0x340)!=0x971A ||
      std::find(operation.required_artifacts.begin(),operation.required_artifacts.end(),
          filesystem::ProfileArtifactKind::kPackageHeader)==operation.required_artifacts.end()) {
    error=Error(std::errc::invalid_argument);return {};
  }
  // Exact CM/raw File source is physically intersected on the real IO lane
  // before stage effects. No path/cached address/generic Task grants it here.
  issuer.operation.requested_device=file->retained_device();
  issuer.operation.defer_until_whole_checkpoint=true;
  auto owner=std::shared_ptr<NativeProfileSavePublication>(new NativeProfileSavePublication);
  owner->source_=issuer.operation.source;owner->system_issuer_=issuer.operation;
  owner->header_issuer_=std::move(issuer);
  return FinishSystemPreparation(owner,error)?owner:nullptr;
}
std::error_code NativeProfileSavePublication::BindSystemGpdInputAtOriginalEdit(
    std::shared_ptr<NativeProfileGpdReadView> view,std::shared_ptr<vfs::Device> requested) {
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
  if((!header_issuer_ && !parent_operation_) || !system_issuer_ ||
      system_issuer_->gpd_mode!=SystemGpdMode::kNativeWriteRequired || !view || !requested || canceled_ || sealed_ || transaction_)
    return Error(std::errc::operation_not_permitted);
  std::error_code error;if(!CurrentForStage(error))return error;
  const auto& writer=source_->writer();const auto& read=view->read_;
  if(view->retired_.load(std::memory_order_acquire) || read.origin.owner.lock()!=writer.model ||
      read.origin.storage!=writer.model->storage() || !writer.model->IsCurrent(read.origin))
    return Error(std::errc::operation_canceled);
  const auto plan=pending_->BuildPlan(error);if(!plan)return error;
  const auto found=plan->files.find(system_issuer_->title);
  if(found==plan->files.end() || !view->token_ || found->second.before.token!=view->token_)
    return Error(std::errc::operation_canceled);
  const auto old=system_file_input_.load(std::memory_order_acquire);
  if(old)return SameOwner(old->actual_before_file,view) && SameOwner(old->requested_device,requested)
      ?std::error_code{}:Error(std::errc::operation_not_permitted);
  auto input=std::make_shared<const SystemGpdFileInput>(SystemGpdFileInput{std::move(view),std::move(requested)});
  system_file_input_.store(std::move(input),std::memory_order_release);
  return {}; // Exact later binding of an already independently issued writer.
}
std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>
NativeProfileSavePublication::AcquireHeaderAtOriginalIo(
    std::shared_ptr<const filesystem::HostStorageSnapshotToken> actual_header, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error=Error(std::errc::operation_would_block); return {};
  }
  std::unique_lock serial(mutex_,std::try_to_lock);
  if (!serial.owns_lock()) { error=Error(std::errc::device_or_resource_busy); return {}; }
  return AcquireHeaderWithSerialHeld(std::move(actual_header),error);
}
std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>
NativeProfileSavePublication::AcquireHeaderWithSerialHeld(
    std::shared_ptr<const filesystem::HostStorageSnapshotToken> actual_header,std::error_code& error) {
  error.clear();
  using Kind=filesystem::ProfileArtifactKind;
  if (!system_issuer_ || canceled_ || sealed_ || transaction_ || !actual_header ||
      std::find(system_issuer_->required_artifacts.begin(),system_issuer_->required_artifacts.end(),
          Kind::kPackageHeader)==system_issuer_->required_artifacts.end()) {
    error=Error(std::errc::operation_not_permitted); return {};
  }
  if (!CurrentForStage(error)) return {};
  if (joined_artifacts_) {
    const auto header=joined_artifacts_->header();
    const auto before=header ? header->expected() : nullptr;
    const auto& writer=source_->writer();
    if (!before || !writer.owner || actual_header->root()!=writer.owner->source() ||
        actual_header->relative()!=header->target() || !actual_header->ascii_folded_leaf() ||
        !actual_header->exists() || actual_header->info().filesystem_id!=before->info().filesystem_id ||
        actual_header->info().file_id!=before->info().file_id ||
        actual_header->bytes().size()!=before->bytes().size() ||
        !std::equal(actual_header->bytes().begin(),actual_header->bytes().end(),before->bytes().begin())) {
      error=Error(std::errc::operation_canceled); return {};
    }
    error=joined_artifact_error_; return error ? nullptr : header;
  }
  if (!stages_.empty() || !picture_stages_.empty()) {
    error=Error(std::errc::device_or_resource_busy); return {};
  }
  const auto plan=pending_->BuildPlan(error); if (!plan) return {};
  const auto& writer=source_->writer(); const auto storage=pending_->origin().storage;
  if (!writer.owner || !writer.recovery || !writer.metadata || !storage ||
      !plan->files.contains(system_issuer_->title)) {
    error=Error(std::errc::no_such_device); return {};
  }
  std::vector<filesystem::ProfileArtifactExpected> expected;
  expected.push_back({Kind::kGpd,system_issuer_->title,plan->files.at(system_issuer_->title).before.token});
  for (const auto kind:system_issuer_->required_artifacts) {
    if (kind==Kind::kPackageHeader) continue;
    const auto* leaf=NativeProfileSavePictureStage::Leaf(kind);
    if (!leaf || !pending_->pictures_before() || !pending_->pictures_before()->contains(leaf)) {
      error=Error(std::errc::operation_not_supported); return {};
    }
    expected.push_back({kind,0,pending_->pictures_before()->at(leaf)});
  }
  filesystem::GpdIntentBinding binding{storage->source_id(),storage->device_id(),storage->xuid(),
      storage->source_generation(),storage->view()->path().lexically_relative(storage->source()->path()),
      {writer.owner->managed_identity()->uuid(),writer.owner->managed_identity()->content_epoch(),
       writer.owner->managed_identity()->directory_file_id()}};
  // The factory itself preallocates before physical effects. Assignment is
  // nonthrowing and keeps an issued failed/uncertain stage rather than losing
  // its original session or attempting a new one after a caller failure.
  joined_artifacts_=filesystem::ProfilePublicationIntentStore::AcquireHeaderArtifacts(
      writer.owner,writer.recovery,writer.metadata,std::move(binding),std::move(expected),
      std::move(actual_header),error);
  if (joined_artifacts_) {
    joined_artifact_error_=error;
    if(const auto header=joined_artifacts_->header()) {
      // Retention was installed first and is never reset/replaced. Cover both
      // orderings of actual own-return racing the first IO issuance without
      // an observer mutex, shared_ptr load/final-owner release or lost close.
      header_mutation_session_.store(header.get(),std::memory_order_seq_cst);
      if(!original_byte_use_open_.load(std::memory_order_seq_cst))header->CloseMutation();
    }
  }
  return joined_artifacts_ && !error ? joined_artifacts_->header() : nullptr;
}
X_STATUS NativeProfileSavePublication::PrepareHeaderAtOriginalWrite(vfs::File& actual) {
  if(global_critical_region::is_held_by_current_thread())return X_STATUS_INVALID_DEVICE_REQUEST;
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  if(!header_issuer_ || !system_issuer_ || !header_issuer_->actual_backing_file ||
      header_issuer_->actual_backing_file->file()!=&actual ||
      !SameOwner(actual.folder_content_package(),header_issuer_->actual_package) ||
      !SameOwner(actual.retained_device(),system_issuer_->requested_device) ||
      canceled_ || sealed_ || transaction_)return X_STATUS_ACCESS_DENIED;
  std::error_code error;if(!CurrentForStage(error))return vfs::HostStorageError(error);
  if(header_adopted_) {
    // Only the same actual backend/session may service a repeated native raw
    // write. Its normal Write independently validates the issued stage/source.
    return joined_artifacts_ && !joined_artifact_error_ && joined_artifacts_->header()
        ?X_STATUS_SUCCESS:X_STATUS_ACCESS_DENIED;
  }
  if(!header_before_at_io_) {
    // This real File-owned IO lane supplies the missing physical observation;
    // the native observer supplied only owned before bytes and exact owners.
    // Retain the original token before any stage/admission effect, never learn
    // a different canonical tuple on retry after another participant changed.
    auto before=header_issuer_->actual_package->CaptureHeaderBefore(
        header_issuer_->actual_native_before_header,error);
    if(!before)return error?vfs::HostStorageError(error):X_STATUS_UNSUCCESSFUL;
    header_before_at_io_=std::move(before);
  }
  const auto header=AcquireHeaderWithSerialHeld(header_before_at_io_,error);
  if(!header)return error?vfs::HostStorageError(error):X_STATUS_UNSUCCESSFUL;
  if(!CurrentForStage(error)) {
    header->CloseMutation();return vfs::HostStorageError(error);
  }
  if(const auto error=RequireActualCacheCheckpoint())return vfs::HostStorageError(error);
  // Adoption changes only the retained raw backend's byte view. Destroy,
  // Write and Flush cannot commit; the later genuine whole checkpoint owns
  // sealing/typed journal/model/namespace/cache and original result duties.
  const auto status=header_issuer_->actual_package->AdoptHeaderPublication(
      header,header_issuer_->actual_native_before_header);
  if(XSUCCEEDED(status))header_adopted_=true;
  return status;
}
std::error_code NativeProfileSavePublication::ObserveJoinedGpdAtOriginalWrite(
    uint32_t title,std::span<const uint8_t> native_header_index) {
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
  if(!RequiresWholeOperationCheckpoint() || canceled_ || sealed_ || transaction_ ||
      title!=system_issuer_->title)return Error(std::errc::operation_not_permitted);
  std::error_code error;if(!CurrentForStage(error))return error;
  const auto stage=stages_.find(title);if(stage==stages_.end() || !stage->second)return Error(std::errc::bad_file_descriptor);
  auto owned=stage->second->CaptureWriteCheckpoint(native_header_index,error);if(!owned)return error;
  staged_checkpoints_.insert_or_assign(title,std::move(owned));
  return {}; // Stage remains mutable. No journal, model, cache or native success.
}
std::error_code NativeProfileSavePublication::SealJoinedSystemAtOriginalWholeCheckpoint() {
  std::map<uint32_t,std::shared_ptr<const NativeProfileGpdCheckpoint>> issued_checkpoints;
  std::map<uint32_t,SealedGpd> checkpoints;
  std::map<uint32_t,std::shared_ptr<NativeProfileSaveGpdStage>> stages;
  {
    std::unique_lock serial(mutex_,std::try_to_lock);
    if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
    if(!RequiresWholeOperationCheckpoint() || canceled_ || sealed_ || transaction_)
      return Error(std::errc::operation_not_permitted);
    const bool no_write=system_issuer_->gpd_mode==SystemGpdMode::kNativeUnchangedNoWrite;
    if(no_write ? (!parent_operation_ || !stages_.empty() || !staged_checkpoints_.empty() ||
                     system_file_input_.load(std::memory_order_acquire)!=nullptr)
                : (staged_checkpoints_.empty() || staged_checkpoints_.size()!=stages_.size()))
      return Error(std::errc::operation_not_permitted);
    std::error_code error;if(!CurrentForStage(error))return error;
    for(const auto& [title,checkpoint]:staged_checkpoints_) {
      const auto stage=stages_.find(title);
      if(!checkpoint || stage==stages_.end() || checkpoint->stage_.lock()!=stage->second)
        return Error(std::errc::state_not_recoverable);
      // Seal below independently reads/fsyncs the actual stage and compares
      // complete bytes; this retained prefix is not a cached success flag.
      checkpoints.emplace(title,SealedGpd{checkpoint->bytes_,checkpoint->index_});
    }
    issued_checkpoints=staged_checkpoints_;stages=stages_;
  }
  // This is an existing real owned IO lane, not a new observer wait. Closing
  // each actual stage serializes queued byte calls; a late/unknown changed
  // stage fails the exact comparison and never publishes an older snapshot.
  for(const auto& [title,stage]:stages) {
    std::error_code error;const auto bytes=stage->Seal(error);if(!bytes)return error;
    {
      std::unique_lock serial(mutex_,std::try_to_lock);
      if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
      const auto issued=issued_checkpoints.find(title),recorded=staged_checkpoints_.find(title);
      std::unique_lock physical(stage->mutex_,std::try_to_lock);
      if(!physical.owns_lock())return Error(std::errc::device_or_resource_busy);
      // Compare the epoch actually captured before Seal as well as current
      // publication metadata. A newer completed write-away/back checkpoint
      // cannot make this older whole cut current merely by restoring bytes.
      if(issued==issued_checkpoints.end() || !issued->second ||
          recorded==staged_checkpoints_.end() || !recorded->second ||
          issued->second->stage_.lock()!=stage || recorded->second->stage_.lock()!=stage ||
          issued->second->revision_!=stage->data_revision_ || recorded->second->revision_!=issued->second->revision_)
        return Error(std::errc::state_not_recoverable);
    }
    const auto checkpoint=checkpoints.find(title);
    if(checkpoint==checkpoints.end() || *bytes!=checkpoint->second.bytes)
      return Error(std::errc::state_not_recoverable);
  }
  return SealSystemOutputAtOriginalCheckpoint(std::move(checkpoints));
}
std::error_code NativeProfileSavePublication::SealSystemOutputAtOriginalCheckpoint(std::map<uint32_t,SealedGpd> staged) {
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
  if(!system_issuer_ || canceled_ || sealed_ || transaction_)return Error(std::errc::operation_canceled);
  std::error_code error;if(!CurrentForStage(error))return error;
  using Kind=filesystem::ProfileArtifactKind;
  using Action=filesystem::ProfileArtifactAction;
  std::vector<filesystem::ProfileArtifactMutation> pictures;
  std::optional<filesystem::ProfileArtifactMutation> header;
  const bool needs_header=std::find(system_issuer_->required_artifacts.begin(),system_issuer_->required_artifacts.end(),
      Kind::kPackageHeader)!=system_issuer_->required_artifacts.end();
  if (needs_header && (!joined_artifacts_ || joined_artifact_error_ || !joined_artifacts_->header()))
    return joined_artifact_error_ ? joined_artifact_error_ : Error(std::errc::operation_not_supported);
  size_t required_pictures=0;
  for (const auto kind:system_issuer_->required_artifacts) {
    if (kind==Kind::kPackageHeader) {
      // The one actual CM session validates format/source/delta before giving
      // its immutable seal; native request/DB/IO quiescence is a separate duty.
      auto bytes=filesystem::ProfilePublicationIntentStore::SealAdmittedHeader(joined_artifacts_,error);
      if (!bytes || error) return error ? error : Error(std::errc::io_error);
      const auto before=joined_artifacts_->header()->expected()->bytes();
      const bool unchanged=before.size()==bytes->size() && std::equal(before.begin(),before.end(),bytes->begin());
      header=filesystem::ProfileArtifactMutation{Kind::kPackageHeader,0,
          unchanged?Action::kObserve:Action::kReplace,unchanged?std::vector<uint8_t>{}:*bytes};
      continue;
    }
    const auto* leaf=NativeProfileSavePictureStage::Leaf(kind);
    const auto found=picture_seals_.find(kind);
    if (!leaf || found==picture_seals_.end() || !pending_->pictures_before() ||
        !pending_->pictures_before()->contains(leaf)) return Error(std::errc::operation_not_supported);
    const auto token=pending_->pictures_before()->at(leaf);
    pictures.push_back({kind,0,token->exists()?Action::kReplace:Action::kCreate,found->second});
    ++required_pictures;
  }
  if (picture_stages_.size()!=required_pictures || picture_seals_.size()!=required_pictures)
    return Error(std::errc::operation_not_supported);
  const auto title=system_issuer_->title;
  const bool no_write=system_issuer_->gpd_mode==SystemGpdMode::kNativeUnchangedNoWrite;
  if(no_write ? (!parent_operation_ || !staged.empty() || !stages_.empty() ||
                  system_file_input_.load(std::memory_order_acquire)!=nullptr)
              : (staged.size()!=1 || !staged.contains(title)))return Error(std::errc::invalid_argument);
  const auto baseline=pending_->BuildPlan(error);if(!baseline)return error;
  const auto found=baseline->files.find(title);
  if(found==baseline->files.end() || !found->second.before.readable())return Error(std::errc::operation_not_supported);
  auto native=std::make_shared<xam::ProfileGpdModelPlan>(*baseline);auto& member=native->files.at(title);
  if(no_write) {
    // Native C4D8 did not acquire/mutate a GPD on this proven branch. Its
    // original validated before token participates as Observe in the SAME05
    // guard/journal; no synthetic File write, index or native serialization
    // receipt is invented merely to satisfy the writer-required lane.
    member.action=xam::ProfileGpdFileAction::kUnchanged;member.intended.clear();
  }else {
  const auto& actual=staged.at(title);
  if(actual.bytes.size()>xam::GpdSnapshotStore::kMaximumBytes)return Error(std::errc::file_too_large);
  error=xam::ValidateGpdSnapshot(actual.bytes);if(error)return error;
  error=xam::ValidateProfileGpdRecords(actual.bytes,title==0xFFFE07D1);if(error)return error;
  // The authentic SYSTEM producer's complete valid records are authoritative.
  // Keep its exact raw bytes, unknown valid records and native layout. No
  // image-only HOST delta or partial native table substitutes for that output.
  error=save_detail::ValidateNativeGpdSerialization(actual.bytes,actual.bytes,actual.native_header_index);
  if(error)return error;
  const auto before=member.before.token->bytes();
  const bool unchanged=before.size()==actual.bytes.size() && std::equal(before.begin(),before.end(),actual.bytes.begin());
  member.action=unchanged?xam::ProfileGpdFileAction::kUnchanged:xam::ProfileGpdFileAction::kReplace;
  member.intended=unchanged?std::vector<uint8_t>{}:actual.bytes;
  }
  native->two_existing_replacements=false;
  native->pictures=std::move(pictures);native->native_artifacts=joined_artifacts_;native->package_header=std::move(header);
  error=source_->writer().model->AdoptNativePlan(pending_,shared_from_this(),std::move(native));
  if(!error)sealed_=true;
  return error;
}
std::shared_ptr<NativeProfileSaveGpdStage> NativeProfileSavePublication::CreateManagedGpdStage(
    uint32_t title,std::error_code& error) {
  error.clear(); std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock()) {error=Error(std::errc::device_or_resource_busy);return {};}
  if(canceled_ || sealed_ || transaction_ || title!=pending_->command().title_id ||
      (system_issuer_ && system_issuer_->gpd_mode!=SystemGpdMode::kNativeWriteRequired)) {
    error=Error(std::errc::operation_canceled);return {};
  }
  if(const auto old=stages_.find(title);old!=stages_.end()) {
    error=old->second->initialization_error_; return old->second;
  }
  const auto plan=pending_->BuildPlan(error);if(!plan) {return {};}
  const auto member=plan->files.find(title);
  if(member==plan->files.end()) {error=Error(std::errc::invalid_argument);return {};}
  // Allocate the actual retention node BEFORE any physical stage effect.
  // Post-create allocation failure must not lose a created/uncertain artifact.
  const auto where=stages_.try_emplace(title).first;
  try {where->second=NativeProfileSaveGpdStage::Create(shared_from_this(),member->second.before,error);}
  catch(...) {stages_.erase(where);throw;} // Stage::Create catches every post-IO failure.
  auto stage=where->second;
  // Retain failed/uncertain CREATED stages too; no retry can forget them or
  // switch to raw original bytes. Exact cancellation/rundown reports artifacts.
  if(!stage) {stages_.erase(where);} // definite before-creation rejection only
  return stage;
}
std::shared_ptr<NativeProfileSavePictureStage> NativeProfileSavePublication::CreateManagedPictureStage(
    filesystem::ProfileArtifactKind kind,std::error_code& error) {
  error.clear();std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock()) {error=Error(std::errc::device_or_resource_busy);return {};}
  if(canceled_ || sealed_ || transaction_) {error=Error(std::errc::operation_canceled);return {};}
  if(const auto old=picture_stages_.find(kind);old!=picture_stages_.end()) {
    error=old->second->initialization_error_;return old->second;
  }
  // Preallocate the request's retention node before any created/uncertain IO.
  const auto where=picture_stages_.try_emplace(kind).first;
  try {where->second=NativeProfileSavePictureStage::Create(shared_from_this(),kind,error);}
  catch(...) {picture_stages_.erase(where);throw;}
  const auto stage=where->second;if(!stage) {picture_stages_.erase(where);}return stage;
}
std::error_code NativeProfileSavePublication::SealManagedPicturesAtOriginalFileCut() {
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock()) {return Error(std::errc::device_or_resource_busy);}
  if(canceled_ || sealed_ || transaction_) {return Error(std::errc::operation_canceled);}
  std::error_code error;if(!source_->Current(error)) {return error;}
  const auto plan=pending_->BuildPlan(error);if(!plan) {return error;}
  std::vector<filesystem::ProfileArtifactKind> required;
  if (system_issuer_) {
    for (const auto kind:system_issuer_->required_artifacts) {
      if (kind==filesystem::ProfileArtifactKind::kPackageHeader) continue;
      if (!NativeProfileSavePictureStage::Leaf(kind)) return Error(std::errc::operation_not_supported);
      required.push_back(kind);
    }
  } else for (const auto& member:plan->pictures) required.push_back(member.kind);
  if(picture_stages_.size()!=required.size()) {return Error(std::errc::operation_not_supported);}
  for(const auto kind:required) {
    if(!NativeProfileSavePictureStage::Leaf(kind)) return Error(std::errc::operation_not_supported);
    const auto found=picture_stages_.find(kind);
    if(found==picture_stages_.end()) {return Error(std::errc::operation_not_supported);}
    if(picture_seals_.contains(kind)) {continue;}
    const auto where=picture_seals_.try_emplace(kind).first;
    try {
      auto bytes=found->second->Seal(error);
      if(!bytes) {picture_seals_.erase(where);return error;}
      where->second=std::move(*bytes);
    }catch(...) {picture_seals_.erase(where);throw;} // Stage keeps its actual receipt.
  }
  return {};
}
std::error_code NativeProfileSavePublication::SealManagedGpdAtOriginalDbCut(
    uint32_t title,std::span<const uint8_t> actual_native_header_index) {
  std::shared_ptr<NativeProfileSaveGpdStage> stage;
  {
    std::unique_lock serial(mutex_,std::try_to_lock);
    if(!serial.owns_lock()) {return Error(std::errc::device_or_resource_busy);}
    if(canceled_ || sealed_ || transaction_) {return Error(std::errc::operation_canceled);}
    const auto found=stages_.find(title);
    if(found==stages_.end()) {return Error(std::errc::invalid_argument);}
    stage=found->second;
  }
  std::error_code error; auto bytes=stage->Seal(error);if(!bytes) {return error;}
  std::map<uint32_t,SealedGpd> packet;
  packet.emplace(title,SealedGpd{std::move(*bytes),
      std::vector<uint8_t>(actual_native_header_index.begin(),actual_native_header_index.end())});
  return system_issuer_ ? SealSystemOutputAtOriginalCheckpoint(std::move(packet)) : SealAtOriginalDbCut(std::move(packet));
}
std::error_code NativeProfileSavePublication::SealAtOriginalDbCut(
    std::map<uint32_t, SealedGpd> staged) {
  // Called after the concrete request owner excluded all genuine staged IO;
  // that native lifetime check cannot be inferred from these byte vectors.
  std::unique_lock serial(mutex_, std::try_to_lock);
  if (!serial.owns_lock()) { return Error(std::errc::device_or_resource_busy); }
  if (canceled_) { return Error(std::errc::operation_canceled); }
  if (sealed_ || transaction_) { return Error(std::errc::device_or_resource_busy); }
  std::error_code error;
  if (!source_->Current(error)) { return error; }
  auto plan = pending_->BuildPlan(error);
  if (!plan || error) { return error; }
  // Intended HOST plan bytes alone cannot stand in for original native PNG
  // outputs. Require every actual closed/fsynced/validated stage receipt. CM
  // thumbnail and delete/observe native producers remain explicitly unjoined.
  if(picture_seals_.size()!=plan->pictures.size() || picture_stages_.size()!=plan->pictures.size()) {
    return Error(std::errc::operation_not_supported);
  }
  for(const auto& member:plan->pictures) {
    const auto found=picture_seals_.find(member.kind);
    if(member.kind==filesystem::ProfileArtifactKind::kThumbnail || found==picture_seals_.end() ||
        found->second!=member.intended ||
        (member.action!=filesystem::ProfileArtifactAction::kCreate && member.action!=filesystem::ProfileArtifactAction::kReplace)) {
      return Error(std::errc::operation_not_supported);
    }
  }
  const auto title = pending_->command().title_id;
  // Current recovered 2F1/2FB publication families mutate one actual DB.
  // Do not turn a caller-selected map into authority for another title.
  if (staged.size() != 1 || !staged.contains(title) || !plan->files.contains(title)) {
    return Error(std::errc::invalid_argument);
  }
  auto native = std::make_shared<xam::ProfileGpdModelPlan>(*plan);
  for (auto& [id, file] : native->files) {
    if (id != title) {
      if (file.action != xam::ProfileGpdFileAction::kUnchanged) {
        return Error(std::errc::operation_not_supported);
      }
      continue;
    }
    if (file.action == xam::ProfileGpdFileAction::kDelete) {
      // Native image NULL deletes an image RECORD, never the whole GPD.
      return Error(std::errc::operation_not_supported);
    }
    const auto intended = file.action == xam::ProfileGpdFileAction::kUnchanged
        ? file.before.token->bytes() : std::span<const uint8_t>(file.intended);
    const auto& actual = staged.at(id);
    error = save_detail::ValidateNativeGpdSerialization(
        intended, actual.bytes, actual.native_header_index);
    if (error) { return error; }
    if (file.before.missing()) { file.action = xam::ProfileGpdFileAction::kCreate; }
    else if (actual.bytes.size() == file.before.token->bytes().size() &&
        std::equal(actual.bytes.begin(),actual.bytes.end(),file.before.token->bytes().begin())) {
      file.action = xam::ProfileGpdFileAction::kUnchanged;
    } else { file.action = xam::ProfileGpdFileAction::kReplace; }
    file.intended = file.action == xam::ProfileGpdFileAction::kUnchanged
        ? std::vector<uint8_t>{} : actual.bytes;
  }
  native->two_existing_replacements = false; // generic typed journal path
  error = source_->writer().model->AdoptNativePlan(pending_, shared_from_this(), std::move(native));
  if (!error) { sealed_ = true; }
  return error;
}
xam::ProfileGpdTransactionAttempt NativeProfileSavePublication::PublishAtOriginalCommitCut() {
  xam::ProfileGpdTransactionAttempt out;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    out.error = Error(std::errc::operation_would_block); return out;
  }
  std::unique_lock serial(mutex_, std::try_to_lock);
  if (!serial.owns_lock()) { out.error = Error(std::errc::device_or_resource_busy); return out; }
  if (canceled_) { out.error = Error(std::errc::operation_canceled); return out; }
  if (!sealed_) { out.error = Error(std::errc::device_or_resource_busy); return out; }
  if (!transaction_) {
    if (!source_->Current(out.error)) { return out; }
    const auto& writer = source_->writer();
    transaction_ = writer.model->AcquireNativeTransaction(writer.owner, writer.recovery,
        out.error, writer.metadata, shared_from_this());
    if (!transaction_) { return out; }
  }
  // Exact transaction retry retains original intent and layout. After commit
  // the old source stamp is intentionally stale; do not rebind by XUID/model.
  auto result=transaction_->AttemptNative(shared_from_this());
  using State=xam::ProfileGpdTransactionAttempt::State;
  if(!result.error && (result.state==State::kCommitted || result.state==State::kAlreadyCommitted)) {
    committed_=true; // Actual SAME transaction model+VFS commit, never done-only.
  }
  return result;
}
xam::ProfileGpdTransactionAttempt NativeProfileSavePublication::RecoverAttemptedAtActualRundown() {
  xam::ProfileGpdTransactionAttempt out;
  if (global_critical_region::is_held_by_current_thread()) {
    out.error=Error(std::errc::operation_would_block);return out;
  }
  std::unique_lock serial(mutex_,std::try_to_lock);
  if (!serial.owns_lock()) {out.error=Error(std::errc::device_or_resource_busy);return out;}
  if (original_byte_use_open_.load(std::memory_order_acquire) || canceled_ || !sealed_ || !transaction_) {
    out.error=Error(std::errc::operation_not_permitted);return out;
  }
  // No CurrentForStage/new issuer: this is the retained recovery duty of the
  // exact already-attempted intent. Transaction validates its real remaining
  // epoch and preserves partial/uncertain state; it refuses an unstarted one.
  out=transaction_->AttemptNativeRecovery(shared_from_this());
  using State=xam::ProfileGpdTransactionAttempt::State;
  if (!out.error && (out.state==State::kCommitted || out.state==State::kAlreadyCommitted)) committed_=true;
  return out; // Native cache checkpoint and original failed HRESULT unchanged.
}
std::error_code NativeProfileSavePublication::RequireActualCacheCheckpoint() {
  // The policy already owns the exact stage/File admission; model try-lock
  // refusal is truthful and causes no File attachment or backing mutation.
  return source_->writer().model->RequireNativeCacheCheckpoint(pending_,shared_from_this());
}
NativeProfileSavePublication::CacheCheckpoint::~CacheCheckpoint()=default;
void NativeProfileSavePublication::CacheCheckpoint::Apply() noexcept {guard_->Apply();}
std::unique_ptr<NativeProfileSavePublication::CacheCheckpoint>
NativeProfileSavePublication::PrepareActualCacheCheckpoint(
    uint32_t title,const std::shared_ptr<NativeProfileSaveGpdStage>& stage,std::error_code& error) {
  error.clear();const auto found=stages_.find(title);
  if(cache_checkpoint_complete_ || !committed_ || !sealed_ || canceled_ || !transaction_ || !stage ||
      found==stages_.end() || found->second!=stage || stage->publication_.lock().get()!=this ||
      !stage->cache_view_claimed_ || stage->retired()){error=Error(std::errc::operation_canceled);return {};}
  if (joined_artifacts_ && joined_artifacts_->header()) {
    // The actual stable raw Header/971A promotion receipt is not wired yet.
    // Never let one GPD File alone declare the whole multi-artifact cache ready.
    error=Error(std::errc::operation_not_supported);return {};
  }
  auto guard=source_->writer().model->PrepareNativeCacheCheckpoint(pending_,shared_from_this(),transaction_.get(),error);
  if(!guard)return {};
  return std::unique_ptr<CacheCheckpoint>(new CacheCheckpoint(std::move(guard)));
}
X_STATUS NativeProfileSavePublication::PromoteJoinedAtActualNativeCheckpoint(
    object_ref<XFile> raw,object_ref<XFile> gpd,
    const std::shared_ptr<NativeProfileSaveManagedGpd>& service,std::span<const uint8_t> native_index) {
  if(global_critical_region::is_held_by_current_thread())return X_STATUS_INVALID_DEVICE_REQUEST;
  // Declare every holder outside the serial interval. On ANY pre-Apply failure,
  // that interval ends before the prepared cache locks/owned views are dropped.
  std::unique_ptr<vfs::FolderContentPackage::HeaderCacheCheckpoint> header;
  std::unique_ptr<NativeProfileSaveManagedGpd::PreparedCachePromotion> data;
  std::unique_ptr<xam::ProfileGpdModelAuthority::NativeCacheCheckpointGuard> model;
  {
    std::unique_lock serial(mutex_,std::try_to_lock);
    if(!serial.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
    if(!parent_operation_ || !system_issuer_ || !header_issuer_ || !joined_artifacts_ ||
        !joined_artifacts_->header() || !committed_ || !sealed_ || canceled_ || !transaction_ ||
        cache_checkpoint_complete_ || !raw || !raw->file() || raw.get()!=header_issuer_->actual_backing_file.get() ||
        raw->file()->folder_content_package()!=header_issuer_->actual_package)
      return X_STATUS_ACCESS_DENIED;
    const bool no_gpd=system_issuer_->gpd_mode==SystemGpdMode::kNativeUnchangedNoWrite;
    if(no_gpd ? (gpd || service || !native_index.empty() || !stages_.empty() ||
                    system_file_input_.load(std::memory_order_acquire)!=nullptr)
               : (!gpd || !gpd->file() || !service || native_index.empty()))return X_STATUS_ACCESS_DENIED;
    std::error_code error;
    const auto actual_header=joined_artifacts_->header()->RetainedPublishedCacheToken(error);
    if(!actual_header)return vfs::HostStorageError(error);
    X_STATUS status=X_STATUS_UNSUCCESSFUL;
    header=header_issuer_->actual_package->PrepareCommittedHeader(
        joined_artifacts_->header(),actual_header,status);
    if(!header)return status;
    if(!header->MatchesPublication(joined_artifacts_->header()))return X_STATUS_ACCESS_DENIED;
    if(!no_gpd) {
      const auto stage=stages_.find(system_issuer_->title);
      if(stage==stages_.end() || !stage->second)return X_STATUS_ACCESS_DENIED;
      data=service->PrepareCommittedCacheWithSerialHeld(*gpd->file(),shared_from_this(),native_index,error);
      if(!data)return vfs::HostStorageError(error);
      if(!data->MatchesPublication(shared_from_this(),system_issuer_->title,stage->second))return X_STATUS_ACCESS_DENIED;
    }
    model=source_->writer().model->PrepareNativeCacheCheckpoint(pending_,shared_from_this(),transaction_.get(),error);
    if(!model)return vfs::HostStorageError(error);
    // No allocation, IO, source lookup, native call, wait or fallible admission
    // follows. Both physical cache locks remain held through every swap; only
    // then does the exact model pending/cache milestone leave under its guard.
    header->Apply();if(data)data->Apply();model->Apply();cache_checkpoint_complete_=true;
  }
  // Let ordinary cached readers see the completely switched tuple before
  // releasing model admission. Retain all old views/stages/session holders
  // until AFTER the model guard and every publication/File/stage lock leave.
  header->ReleaseLocksAfterApply();if(data)data->ReleaseLocksAfterApply();
  model.reset();header.reset();data.reset();return X_STATUS_SUCCESS;
}
std::error_code NativeProfileSavePublication::CancelPristineAtActualNoopWithSerialHeld() {
  if(original_byte_use_open_.load(std::memory_order_acquire) || !system_issuer_ ||
      canceled_ || sealed_ || committed_ || transaction_ || !stages_.empty() ||
      !picture_stages_.empty() || !picture_seals_.empty() || joined_artifacts_ || header_issuer_)
    return Error(std::errc::operation_not_permitted);
  const auto error=source_->writer().model->CancelUnpublishedNative(pending_,shared_from_this());
  if(!error)canceled_=true;
  return error;
}
std::error_code NativeProfileSavePublication::CancelUnboundAtActualRundown() {
  std::unique_lock serial(mutex_,std::try_to_lock);
  if(!serial.owns_lock())return Error(std::errc::device_or_resource_busy);
  // Family authenticates the actual failed bind/whole original rundown.
  // Shared predicate proves no stage, transaction, journal or byte owner can
  // be discarded; original immutable File/model bytes were never replaced.
  return CancelPristineAtActualNoopWithSerialHeld();
}
std::error_code NativeProfileSavePublication::CancelAtOriginalQuiescence() {
  std::unique_lock serial(mutex_, std::try_to_lock);
  if (!serial.owns_lock()) { return Error(std::errc::device_or_resource_busy); }
  if (canceled_) { return {}; }
  if(header_issuer_ || (joined_artifacts_ && joined_artifacts_->header())) {
    // Byte admission can close without waiting, but that is not proof that
    // original cached971A/actual raw File has restored its canonical tuple.
    // Keep the issued stage/model closed until the genuine Header abort or
    // cache-close participant is joined; never clear pending on guessed drain.
    return Error(std::errc::operation_not_supported);
  }
  for(const auto& [title,stage]:stages_) {
    if(!stage->retired()) {return Error(std::errc::device_or_resource_busy);}
  }
  for(const auto& [kind,stage]:picture_stages_) {
    if(!stage->retired()) {return Error(std::errc::device_or_resource_busy);}
  }
  // Issued but never-prepared transactions can still cancel under their real
  // serial guard. A journal attempt/uncertain/partial publication cannot.
  auto error = transaction_ ? transaction_->CancelNativeUnpublished(shared_from_this())
      : source_->writer().model->CancelUnpublishedNative(pending_,shared_from_this());
  if (!error) { canceled_ = true; }
  return error;
}
}  // namespace xe::kernel
