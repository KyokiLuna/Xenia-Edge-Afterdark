/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_NATIVE_FOLDER_CONTENT_H_
#define XENIA_KERNEL_XAM_NATIVE_FOLDER_CONTENT_H_
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>
#include "xenia/base/content_package_tree.h"
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/retained_file_metadata.h"
#include "xenia/base/string.h"
#include "xenia/kernel/xam/folder_content_header.h"
#include "xenia/kernel/xam/profile_gpd_publication.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/folder_content_package.h"
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/vfs/managed_file_service.h"
namespace xe::kernel::xam {
class ContentManager;
class ProfileStorageLease;
class NativeFolderContentTestAccess;
// One existing CM source/package policy in the SAME ManagedFileRegistry.
// Ordinary Open exposes a complete verified owned native Header and genuinely
// retained member extent. It reads NO member payload through HostStorage and
// grants no native writer by path/access bits. Protected/raw payload ranges
// remain unsupported; the genuine member Device uses normal per-open policy.
class NativeFolderContent final : public vfs::ManagedFileService,
    public std::enable_shared_from_this<NativeFolderContent> {
 public:
  const std::filesystem::path& backing_path() const override { return physical_package_; }
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const override { return parent_pin_; }
  bool retiring() const override {
    return !source_.root || source_.root->retiring() || !source_.volume ||
        !source_.volume->available_cached() || !source_.managed ||
        !source_.managed->identity() || source_.managed->identity()->retiring();
  }
  NamespaceRole namespace_role() const noexcept override { return NamespaceRole::kRawFolderProjection; }
  std::shared_ptr<const vfs::ManagedFileNamespaceView> NamespaceView(
      const vfs::Device& device) const override {
    return !retiring() && device.storage_volume() == source_.volume ? namespace_ : nullptr;
  }
  X_STATUS Open(std::shared_ptr<vfs::Device> requested, std::string path,
      vfs::FileDisposition disposition, uint32_t access, bool directory,
      vfs::File** output, vfs::FileAction* action) override;
 private:
  friend class ContentManager;
  friend class xe::kernel::NativeProfileSaveOwner;
  friend class xe::kernel::NativeProfileSavePublication;
  friend class NativeFolderContentTestAccess;
  struct Capture;
  using HeaderSession=filesystem::ContentMutationOwner::ProfileHeaderPublication;
  class ParentReadAuthority final:public vfs::ManagedFileOpenAuthority {
   private:
    friend class NativeFolderContent;
    ParentReadAuthority(std::weak_ptr<NativeFolderContent> policy,std::shared_ptr<HeaderSession> issued,
        std::shared_ptr<vfs::Device> requested,std::string path,std::shared_ptr<void> duty)
        :policy_(std::move(policy)),issued_(std::move(issued)),requested_(std::move(requested)),
         path_(std::move(path)),duty_(std::move(duty)) {}
    const std::weak_ptr<NativeFolderContent> policy_;
    const std::shared_ptr<HeaderSession> issued_;
    const std::shared_ptr<vfs::Device> requested_;const std::string path_;
    const std::shared_ptr<void> duty_;
    mutable std::atomic<bool> consumed_{false};
  };
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueParentReadForActualOpen(
      std::shared_ptr<HeaderSession>,std::shared_ptr<vfs::Device>,std::string,
      std::shared_ptr<void>,std::error_code&);
  X_STATUS OpenWithContext(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*,const vfs::FileOpenContext&) override;
  std::optional<Capture> CaptureIssued(const std::shared_ptr<HeaderSession>&,std::error_code&) const;
  struct Source {
    std::shared_ptr<filesystem::HostStorageRoot> root;
    std::shared_ptr<const filesystem::ManagedVolumeAuthority> managed;
    std::shared_ptr<const filesystem::ConsoleStorageVolume> volume;
    ConsoleSigningIdentity::Certificate certificate{}; // PUBLIC material only
  };
  struct Capture {
    // Exact immutable bytes captured from the CM-issued source token, after
    // native aligned-range hash/signature validation. [0,data_offset) is the
    // owned raw Header (including actual padding); any later sidecar tail is
    // preserved separately by its position and whole-token CAS, never exposed
    // as padding or member data. No long-lived cached File retains a PIS/Header
    // namespace token, member scope, journal or stage reservation. No zero-pad
    // creator is issued here: native 86DA0 creation remains a separate producer.
    std::shared_ptr<const std::vector<uint8_t>> header;
    filesystem::FileHandleInfo header_info;
    std::shared_ptr<filesystem::HostStorageContentLayout> layout;
    folder_content_detail::HeaderFacts facts;
    folder_content_detail::FolderVirtualExtent virtual_extent;
    vfs::File::Information information;
  };
  struct State;
  class RawEntry;
  class RawFile;
  NativeFolderContent() = default;
  static std::shared_ptr<NativeFolderContent> Prepare(Source source,
      std::filesystem::path exact_package,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> actual_source_header,
      folder_content_detail::PackageIdentity expected, std::error_code& error);
  std::optional<Capture> CaptureCurrent(std::error_code&,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken>* = nullptr) const;
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> CaptureForParentAdmission(
      const ProfileStorageLease&,std::error_code&) const;
  bool SameSource(const Source& source, const std::filesystem::path& package) const {
    return !retiring() && source_.root == source.root && source_.managed == source.managed &&
        source_.volume == source.volume && source_.certificate == source.certificate && package_ == package;
  }
  std::error_code RefreshNamespace() {
    std::error_code error; const auto captured = CaptureCurrent(error);
    if (!captured) { return error ? error : std::make_error_code(std::errc::io_error); }
    namespace_->PublishPrepared(std::make_shared<const vfs::File::Information>(captured->information));
    return {};
  }
  bool MatchesDevice(const vfs::Device&, const std::string&) const;
  Source source_;
  std::filesystem::path package_, target_, physical_package_;
  folder_content_detail::PackageIdentity expected_;
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent_pin_;
  std::shared_ptr<vfs::ManagedFileNamespaceView> namespace_;
};
class NativeFolderContent::RawEntry final : public vfs::Entry {
 public:
  RawEntry(vfs::Device* device, std::string path, const vfs::File::Information& info,std::filesystem::path actual_package)
      : Entry(device, nullptr, path) {
    coordinate_=std::make_shared<CheckpointCoordinate>(CheckpointCoordinate{std::move(actual_package)});
    attributes_ = info.attributes; size_ = size_t(info.end_of_file);
    allocation_size_ = size_t(info.allocation_size);
    create_timestamp_ = info.create_timestamp; access_timestamp_ = info.access_timestamp;
    write_timestamp_ = info.write_timestamp;
  }
  std::shared_ptr<const CheckpointCoordinate> RetainCheckpointCoordinate() override {return coordinate_;}
  bool allows_namespace_mutation() const override { return false; }
  X_STATUS Open(uint32_t, vfs::File**) override { return X_STATUS_ACCESS_DENIED; }
 private:
  std::shared_ptr<const CheckpointCoordinate> coordinate_;
  bool DeleteEntryInternal(vfs::Entry*) override { return false; }
};
struct NativeFolderContent::State final : public vfs::FolderContentPackage,
    public std::enable_shared_from_this<NativeFolderContent::State> {
  std::shared_ptr<NativeFolderContent> policy;
  std::shared_ptr<vfs::Device> requested;
  Capture current;
  std::mutex mutex;
  std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication> publication;
  bool mutation_adopted=false;
  bool retiring() const noexcept override {
    return !policy || policy->retiring() || !requested || requested->namespace_retiring();
  }
  X_STATUS Current() const {
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    if (retiring()) { return X_STATUS(0xC000009D); }
    if (!publication) { return vfs::HostStorageError(current.layout->CheckCurrent()); }
    std::error_code error; const auto actual = publication->CaptureMemberLayout(error);
    if (!actual) { return vfs::HostStorageError(error); }
    const auto& a = actual->top_info(); const auto& b = current.layout->top_info();
    if (a.filesystem_id != b.filesystem_id || a.file_id != b.file_id ||
        actual->entries() != current.layout->entries() ||
        actual->statistics() != current.layout->statistics()) {
      return vfs::HostStorageError(std::make_error_code(std::errc::state_not_recoverable));
    }
    return X_STATUS_SUCCESS;
  }
  X_STATUS PrepareMemberDevice(std::string_view name, const MountRequest& packet,
      std::shared_ptr<vfs::File> exact_backing, std::unique_ptr<vfs::Device>& output,
      std::unique_ptr<MountAdmission>& admission) override {
    output.reset(); admission.reset();
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(mutex);
    if (!exact_backing || exact_backing->folder_content_package().get() != this ||
        exact_backing->retained_device() != requested) { return X_STATUS_INVALID_HANDLE; }
    if (const auto status = Current(); XFAILED(status)) { return status; }
    const auto& actual = current.facts;
    if (packet.data_offset != actual.data_offset || packet.logical_geometry != actual.logical_geometry ||
        packet.context_bytes != actual.context_bytes || packet.descriptor != actual.descriptor ||
        packet.content_nonzero != (actual.content_size != 0) ||
        packet.signature_class != actual.signature_class || packet.owner_option > 1) {
      return X_STATUS_INVALID_PARAMETER;
    }
    // A new native mount must not lend current namespace authority from an
    // old owned Header after canonical replacement. Hold one actual read
    // lease ONLY through kernel publication, then release before native resume.
    // Existing cached native context updates use their separate issued session.
    if (publication && mutation_adopted) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    struct HeaderAdmission final : MountAdmission {
      std::unique_ptr<filesystem::FileHandle> retained;
      std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication> issued;
    };
    std::error_code error;auto held=std::make_unique<HeaderAdmission>();
    if(publication) {
      if(!publication->Query(error))return vfs::HostStorageError(error);
      held->issued=publication; // SAME reservation, never a public recapture.
    }else {
      auto source_header = policy->source_.managed->mutations()->CaptureMetadata(
          filesystem::ContentMutationOwner::Kind::kHeader, policy->package_, policy->target_, error);
      if (!source_header) { return vfs::HostStorageError(error); }
      if (source_header->info().file_id != current.header_info.file_id ||
          source_header->info().filesystem_id != current.header_info.filesystem_id ||
          source_header->bytes().size() != current.header->size() ||
          !std::equal(source_header->bytes().begin(),source_header->bytes().end(),current.header->begin()))
        return vfs::HostStorageError(std::make_error_code(std::errc::state_not_recoverable));
      held->retained=source_header->root()->OpenFile(source_header->relative(),
          filesystem::FileAccess::kFileReadData,error);
      if(!held->retained)return vfs::HostStorageError(error);
      const auto identity=held->retained->QueryInfo(error);
      const auto observed=source_header->root()->CaptureSnapshot(source_header->relative(),
          filesystem::content_mutation_wire::kMaximumMetadataBytes,error,true);
      if(!identity || !observed || identity->file_id!=source_header->info().file_id ||
          identity->filesystem_id!=source_header->info().filesystem_id || identity->size!=source_header->info().size ||
          observed->info().file_id!=identity->file_id || observed->info().filesystem_id!=identity->filesystem_id ||
          observed->bytes().size()!=source_header->bytes().size() ||
          !std::equal(observed->bytes().begin(),observed->bytes().end(),source_header->bytes().begin()))
        return error?vfs::HostStorageError(error):X_STATUS_UNSUCCESSFUL;
    }
    auto package = policy->source_.root->Subroot(policy->package_, false, error);
    if (!package) { return vfs::HostStorageError(error); }
    auto root = package->WithDependency(policy->source_.root, error);
    if (!root) { return vfs::HostStorageError(error); }
    // No storage-volume/mount-resource is installed here. The actual kernel
    // handoff retains the original backing XFile and SAME source volume, then
    // binds the SAME managed registry before Initialize/exclusive publication.
    output = std::make_unique<vfs::HostPathDevice>(name, root->path(),
        requested->is_read_only() || actual.read_only_format, false, "STFS", root);
    admission = std::move(held);
    return X_STATUS_SUCCESS;
  }
  X_STATUS Control(uint32_t code, std::span<uint8_t> output, uint32_t* count) override {
    if (!count) { return X_STATUS_INVALID_PARAMETER; } *count = 0;
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(mutex);
    if (const auto status = Current(); XFAILED(status)) { return status; }
    if (code != 3) {
      // No fake block-cache/volume flush claim. The actual joined native IO
      // owner must supply that complete control duty before it is enabled.
      return X_STATUS_INVALID_DEVICE_REQUEST;
    }
    if (output.size() != current.facts.descriptor.size()) { return X_STATUS_INFO_LENGTH_MISMATCH; }
    std::copy(current.facts.descriptor.begin(), current.facts.descriptor.end(), output.begin());
    *count = uint32_t(current.facts.descriptor.size()); return X_STATUS_SUCCESS;
  }
 private:
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> CaptureHeaderBefore(
      std::span<const uint8_t> actual_before, std::error_code& error) override {
    error.clear();
    if (global_critical_region::is_held_by_current_thread()) {
      error = std::make_error_code(std::errc::operation_would_block); return {};
    }
    std::lock_guard lock(mutex);
    if (publication) {
      if(mutation_adopted || actual_before.size()!=current.facts.data_offset ||
          !std::equal(actual_before.begin(),actual_before.end(),current.header->begin())) {
        error=std::make_error_code(std::errc::permission_denied);return {};
      }
      if(const auto status=Current();XFAILED(status)) {
        error=std::make_error_code(std::errc::operation_canceled);return {};
      }
      if(!publication->Query(error))return {};
      return publication->expected();
    }
    if (retiring()) { error = std::make_error_code(std::errc::no_such_device); return {}; }
    if (actual_before.size() != current.facts.data_offset ||
        actual_before.size() > current.header->size() ||
        !std::equal(actual_before.begin(), actual_before.end(), current.header->begin())) {
      error = std::make_error_code(std::errc::permission_denied); return {};
    }
    error = current.layout->CheckCurrent(); if (error) { return {}; }
    auto actual = policy->source_.managed->mutations()->CaptureMetadata(
        filesystem::ContentMutationOwner::Kind::kHeader, policy->package_, policy->target_, error);
    if (!actual) { return {}; }
    if (!actual->exists() || actual->root() != policy->source_.managed->content() ||
        actual->relative() != policy->target_ ||
        actual->info().file_id != current.header_info.file_id ||
        actual->info().filesystem_id != current.header_info.filesystem_id ||
        actual->info().size != current.header_info.size ||
        actual->bytes().size() != current.header->size() ||
        !std::equal(actual->bytes().begin(), actual->bytes().end(), current.header->begin())) {
      error = std::make_error_code(std::errc::state_not_recoverable); return {};
    }
    error = current.layout->CheckCurrent(); if (error) { return {}; }
    if (retiring()) { error = std::make_error_code(std::errc::no_such_device); return {}; }
    // This metadata snapshot is not a File/buffer/guest lifetime lease. The
    // existing CM/PIS factory revalidates and reserves its exact source token
    // before effects; same-guard checks still own actual publication.
    return actual;
  }
  X_STATUS ObserveHeaderPublication(
      const std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>& issued) override {
    if(global_critical_region::is_held_by_current_thread())return X_STATUS_INVALID_DEVICE_REQUEST;
    std::unique_lock held(mutex,std::try_to_lock);
    if(!held.owns_lock())return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));
    if(retiring() || !issued || issued->package()!=policy->package_ || issued->target()!=policy->target_ ||
        !issued->expected() || (publication && publication!=issued))return X_STATUS_ACCESS_DENIED;
    const auto& before=issued->expected();
    if(!before->root()->directory_identity() || !policy->source_.root->directory_identity() ||
        !before->root()->directory_identity()->SameEntry(before->relative(),
          *policy->source_.root->directory_identity(),policy->target_,true) ||
        before->info().file_id!=current.header_info.file_id ||
        before->info().filesystem_id!=current.header_info.filesystem_id ||
        before->bytes().size()!=current.header->size() ||
        !std::equal(before->bytes().begin(),before->bytes().end(),current.header->begin()))return X_STATUS_ACCESS_DENIED;
    publication=issued;return X_STATUS_SUCCESS; // Read observation only.
  }
  X_STATUS AdoptHeaderPublication(
      std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication> issued,
      std::span<const uint8_t> actual_before) override {
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(mutex);
    if (retiring()) { return X_STATUS(0xC000009D); }
    if (current.facts.read_only_format || !issued || (publication && publication!=issued) || issued->package() != policy->package_ ||
        issued->target() != policy->target_ || !issued->expected() ||
        actual_before.size() != current.facts.data_offset ||
        !std::equal(actual_before.begin(), actual_before.end(), std::span<const uint8_t>(*current.header).begin())) {
      return X_STATUS_ACCESS_DENIED;
    }
    const auto& before = issued->expected(); const auto& ours = current.header_info;
    if (!before->exists() || !before->root()->directory_identity() ||
        !policy->source_.root->directory_identity() || !before->root()->directory_identity()->SameEntry(
          before->relative(), *policy->source_.root->directory_identity(), policy->target_, true) ||
        before->info().filesystem_id != ours.filesystem_id ||
        before->info().file_id != ours.file_id || before->info().size != ours.size ||
        before->bytes().size() != current.header->size() ||
        !std::equal(before->bytes().begin(), before->bytes().end(), current.header->begin())) {
      return X_STATUS_ACCESS_DENIED;
    }
    std::error_code error;
    // Issuance already owns the SAME GPD/PNG slots. A public-source census
    // would conflict with that legitimate admission before publication was
    // installed on this File. Observe through the exact issued scope instead.
    const auto layout = issued->CaptureMemberLayout(error);
    if (!layout) { return vfs::HostStorageError(error); }
    if (layout->top_info().filesystem_id != current.layout->top_info().filesystem_id ||
        layout->top_info().file_id != current.layout->top_info().file_id ||
        layout->entries() != current.layout->entries() ||
        layout->statistics() != current.layout->statistics()) {
      return vfs::HostStorageError(std::make_error_code(std::errc::state_not_recoverable));
    }
    if (!issued->Query(error)) { return vfs::HostStorageError(error); }
    publication = std::move(issued);mutation_adopted=true;return X_STATUS_SUCCESS;
  }
  class PreparedHeaderCacheCheckpoint final:public HeaderCacheCheckpoint {
   public:
    PreparedHeaderCacheCheckpoint(std::shared_ptr<State> owner,std::unique_lock<std::mutex> lock,
        Capture next,std::shared_ptr<const vfs::File::Information> information,
        std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication> exact)
        :owner_(std::move(owner)),next_(std::move(next)),information_(std::move(information)),
         exact_(std::move(exact)),lock_(std::move(lock)) {}
    void Apply() noexcept override {
      if(applied_)return;
      // Every allocation, retained read and source/format admission completed
      // before the joint cut. Swap only owned values; retain old resources for
      // destruction AFTER the aggregate model/publication/File/stage locks.
      static_assert(std::is_nothrow_swappable_v<Capture>);
      using std::swap;swap(owner_->current,next_);
      dropped_publication_=std::move(owner_->publication);
      owner_->mutation_adopted=false;
      owner_->policy->namespace_->PublishPrepared(std::move(information_));
      applied_=true;
    }
   private:
    void ReleaseLocksAfterApply() noexcept override {
      if(applied_ && lock_.owns_lock())lock_.unlock();
    }
    bool MatchesPublication(const std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>& exact) const noexcept override {
      return !applied_ && lock_.owns_lock() && exact && exact_==exact && owner_->publication==exact;
    }
    std::shared_ptr<State> owner_;
    Capture next_;
    std::shared_ptr<const vfs::File::Information> information_;
    std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication> exact_,dropped_publication_;
    bool applied_=false;
    // Destroyed FIRST. No old source/stage/session holder drops under its lock.
    std::unique_lock<std::mutex> lock_;
  };
  X_STATUS PromoteCommittedHeader(
      const std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>& exact,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> committed) override {
    X_STATUS status;auto prepared=PrepareCommittedHeader(exact,std::move(committed),status,true);
    if(prepared)prepared->Apply();return status;
  }
  std::unique_ptr<HeaderCacheCheckpoint> PrepareCommittedHeader(
      const std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>& exact,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> committed,X_STATUS& status,
      bool original_nonjoined_wait=false) override {
    status=X_STATUS_UNSUCCESSFUL;
    if(global_critical_region::is_held_by_current_thread()) {
      status=vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));return {};
    }
    std::unique_lock lock(mutex,std::defer_lock);
    if(original_nonjoined_wait)lock.lock();
    else if(!lock.try_lock()){status=vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));return {};}

    if (!exact || exact != publication || !committed || !committed->exists() ||
        committed->root() != exact->expected()->root() || committed->relative() != exact->expected()->relative() ||
        retiring()) { status=X_STATUS_ACCESS_DENIED;return {}; }
    std::error_code error;
    // This is invoked only by the authenticated whole native cache-checkpoint
    // owner, outside its model/GCRC interval. Validate the actual committed
    // Header token and actual NEW member identities before the stable switch.
    {
      const auto batch = committed->root()->ValidateSnapshotBatch({committed}, error);
      if (!batch) { status=vfs::HostStorageError(error);return {}; }
    }
    auto layout = exact->CaptureMemberLayout(error);
    if (!layout) { status=vfs::HostStorageError(error);return {}; }
    const auto virtual_extent = folder_content_detail::CaptureFolderVirtualExtent(*layout, committed->bytes(), error);
    if (!virtual_extent) { status=vfs::HostStorageError(error);return {}; }
    const auto facts = folder_content_detail::ValidateNativeHeader(committed->bytes(), policy->expected_,
        virtual_extent->bytes, policy->source_.certificate, error);
    if (!facts) { status=vfs::HostStorageError(error);return {}; }
    // Commit released the renamed stage handle; this current token's OWN
    // namespace may now open its exact canonical retained File for metadata.
    auto header_file = committed->root()->OpenFile(committed->relative(),
        filesystem::FileAccess::kFileReadData, error);
    if (!header_file) { status=vfs::HostStorageError(error);return {}; }
    const auto actual = header_file->QueryInfo(error);
    if (!actual || actual->file_id != committed->info().file_id ||
        actual->filesystem_id != committed->info().filesystem_id ||
        actual->size != committed->info().size) {
      status=error?vfs::HostStorageError(error):X_STATUS_UNSUCCESSFUL;return {};
    }
    const auto header_metadata = filesystem::QueryRetainedFileMetadata(*header_file, error);
    if (!header_metadata) { status=vfs::HostStorageError(error);return {}; }
    header_file.reset();
    auto info = current.information;
    info.end_of_file = facts->raw_extent;
    if (header_metadata->allocated_bytes > UINT64_MAX - layout->statistics().allocated_bytes) {
      status=X_STATUS_INVALID_PARAMETER;return {};
    }
    const auto allocation_status = vfs::RoundFileAllocationSize(header_metadata->allocated_bytes +
        layout->statistics().allocated_bytes, uint64_t(requested->bytes_per_sector()) *
        requested->sectors_per_allocation_unit(), &info.allocation_size);
    if (XFAILED(allocation_status)) { status=allocation_status;return {}; }
    info.access_timestamp = header_metadata->access_timestamp;
    info.write_timestamp = header_metadata->write_timestamp;
    info.change_timestamp = header_metadata->change_timestamp;
    auto value = std::make_shared<const vfs::File::Information>(info);
    auto bytes = std::make_shared<const std::vector<uint8_t>>(committed->bytes().begin(), committed->bytes().end());
    auto next=current;
    next.header=std::move(bytes);next.header_info=committed->info();
    next.layout=std::move(layout);next.facts=*facts;
    next.virtual_extent=*virtual_extent;next.information=info;
    auto prepared=std::unique_ptr<HeaderCacheCheckpoint>(new PreparedHeaderCacheCheckpoint(
        shared_from_this(),std::move(lock),std::move(next),std::move(value),exact));
    status=X_STATUS_SUCCESS;return prepared;
  }
};
class NativeFolderContent::RawFile final : public vfs::File {
 public:
  RawFile(uint32_t access, std::unique_ptr<RawEntry> entry, std::shared_ptr<State> state)
      : File(access, entry.get()), entry_(std::move(entry)), state_(std::move(state)) {
    device_owner_->RetainManagedFile();
  }
  ~RawFile() override {
    device_owner_->ReleaseManagedFile();
    if(checkpoint_close_)checkpoint_close_->data_status.store(X_STATUS_SUCCESS,std::memory_order_release);
  }
  void ObserveCheckpointClose(std::shared_ptr<CheckpointCloseObservation> receipt) override {checkpoint_close_=std::move(receipt);}
  void Destroy() override { delete this; }
  std::shared_ptr<vfs::FolderContentPackage> folder_content_package() const override { return state_; }
  X_STATUS ReadSync(std::span<uint8_t> output, size_t offset, size_t* count) override {
    if (!count) { return X_STATUS_INVALID_PARAMETER; } *count = 0;
    if (!(file_access_ & (vfs::FileAccess::kGenericRead | vfs::FileAccess::kGenericAll |
                         vfs::FileAccess::kFileReadData))) { return X_STATUS_ACCESS_DENIED; }
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(state_->mutex);
    if (const auto status = state_->Current(); XFAILED(status)) { return status; }
    if (output.empty()) { return X_STATUS_SUCCESS; }
    if (offset >= state_->current.facts.raw_extent) { return X_STATUS_END_OF_FILE; }
    // Ordinary retained File reads stop at the actual owned virtual EOF; a
    // caller's larger output span cannot turn a real final short read into an
    // unsupported raw-sector request beyond the owned extent.
    output = output.first(size_t(std::min<uint64_t>(output.size(),
        state_->current.facts.raw_extent - offset)));
    const auto native_size = state_->current.facts.data_offset;
    if (offset < native_size) {
      const auto part = output.first(std::min(output.size(), size_t(native_size) - offset));
      if (state_->publication) {
        const auto actual = state_->publication->Read(offset, part);
        *count = actual.bytes; if (actual.error || actual.bytes != part.size()) {
          return actual.error ? vfs::HostStorageError(actual.error) : X_STATUS_UNSUCCESSFUL;
        }
      } else {
        std::copy_n(std::span<const uint8_t>(*state_->current.header).data() + offset, part.size(), part.data());
        *count = part.size();
      }
      offset += part.size(); output = output.subspan(part.size());
    }
    // No catalog->HostStorage member payload bypass. The real member Device
    // and its exact managed policies own those reads; unsupported raw ranges
    // preserve known already-copied Header bytes and a truthful failure.
    return output.empty() ? X_STATUS_SUCCESS : X_STATUS_NOT_SUPPORTED;
  }
  X_STATUS WriteSync(std::span<const uint8_t> input, size_t offset, size_t* count) override {
    if (!count) { return X_STATUS_INVALID_PARAMETER; } *count = 0;
    if (!(file_access_ & (vfs::FileAccess::kGenericWrite | vfs::FileAccess::kGenericAll |
        vfs::FileAccess::kFileWriteData | vfs::FileAccess::kFileAppendData))) { return X_STATUS_ACCESS_DENIED; }
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(state_->mutex);
    if (const auto status = state_->Current(); XFAILED(status)) { return status; }
    if (state_->current.facts.read_only_format || !state_->publication || !state_->mutation_adopted) { return X_STATUS_ACCESS_DENIED; }
    // C628 may issue its full second write [0x971A,data_offset), including
    // actual alignment bytes. Unwritten remainder/tail stays in the one stage;
    // its format validator still rejects any unauthorized byte change at seal.
    if (offset > state_->current.facts.data_offset || input.size() > state_->current.facts.data_offset - offset) {
      return X_STATUS_NOT_SUPPORTED;
    }
    const auto wrote = state_->publication->Write(offset, input); *count = wrote.bytes;
    return vfs::HostStorageError(wrote.error);
  }
  X_STATUS QueryInformation(Information* output) const override {
    if (!output) { return X_STATUS_INVALID_PARAMETER; }
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(state_->mutex);
    if (const auto status = state_->Current(); XFAILED(status)) { return status; }
    auto info = state_->current.information;
    if (state_->publication) {
      std::error_code error;
      const auto actual = state_->publication->Query(error);
      if (!actual) { return vfs::HostStorageError(error); }
      if (actual->allocated_bytes > UINT64_MAX - state_->current.layout->statistics().allocated_bytes) {
        return X_STATUS_INVALID_PARAMETER;
      }
      const auto status = vfs::RoundFileAllocationSize(actual->allocated_bytes +
          state_->current.layout->statistics().allocated_bytes,
          uint64_t(device_owner_->bytes_per_sector()) * device_owner_->sectors_per_allocation_unit(),
          &info.allocation_size);
      if (XFAILED(status)) { return status; }
      info.access_timestamp = actual->access_timestamp;
      info.write_timestamp = actual->write_timestamp; info.change_timestamp = actual->change_timestamp;
    }
    *output = info; return X_STATUS_SUCCESS;
  }
  X_STATUS QueryAllocationSize(uint64_t* output) const override {
    if (!output) { return X_STATUS_INVALID_PARAMETER; }
    Information info; const auto status = QueryInformation(&info);
    if (XSUCCEEDED(status)) { *output = info.allocation_size; } return status;
  }
  X_STATUS SetLength(size_t size) override {
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(state_->mutex);
    if (const auto status = state_->Current(); XFAILED(status)) { return status; }
    if (!(file_access_ & (vfs::FileAccess::kGenericWrite | vfs::FileAccess::kGenericAll |
                         vfs::FileAccess::kFileWriteData))) { return X_STATUS_ACCESS_DENIED; }
    return size == state_->current.facts.raw_extent ? X_STATUS_SUCCESS : X_STATUS_NOT_SUPPORTED;
  }
  X_STATUS Flush() override {
    if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    std::lock_guard lock(state_->mutex);
    if (const auto status = state_->Current(); XFAILED(status)) { return status; }
    return state_->publication && state_->mutation_adopted
        ? vfs::HostStorageError(state_->publication->Flush()) : X_STATUS_NOT_SUPPORTED;
  }
 private:
  std::shared_ptr<CheckpointCloseObservation> checkpoint_close_;
  std::unique_ptr<RawEntry> entry_;
  std::shared_ptr<State> state_;
};
inline bool NativeFolderContent::MatchesDevice(const vfs::Device& device,
    const std::string& requested_path) const {
  if (retiring() || device.namespace_retiring() || device.storage_volume() != source_.volume ||
      !device.ManagedDirectoryIdentity()) { return false; }
  const auto relative = xe::to_path(xe::utf8::fix_path_separators(requested_path,
      std::filesystem::path::preferred_separator));
  if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
      std::find(relative.begin(), relative.end(), "..") != relative.end()) { return false; }
  return parent_pin_->SameEntry(physical_package_.filename(), *device.ManagedDirectoryIdentity(), relative, true);
}
inline std::optional<NativeFolderContent::Capture>
NativeFolderContent::CaptureCurrent(std::error_code& error,
    std::shared_ptr<const filesystem::HostStorageSnapshotToken>* exact_before) const {
  if (global_critical_region::is_held_by_current_thread() || retiring()) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  Capture result;
  auto layout = source_.root->CaptureContentLayout(package_, error); if (!layout) { return {}; }
  const auto source_header = source_.managed->mutations()->CaptureMetadata(
      filesystem::ContentMutationOwner::Kind::kHeader, package_, target_, error);
  if (!source_header || !source_header->exists()) {
    if (!error) { error = std::make_error_code(std::errc::no_such_file_or_directory); } return {};
  }
  const auto virtual_extent = folder_content_detail::CaptureFolderVirtualExtent(*layout, source_header->bytes(), error);
  if (!virtual_extent) { return {}; }
  const auto facts = folder_content_detail::ValidateNativeHeader(source_header->bytes(), expected_,
      virtual_extent->bytes, source_.certificate, error);
  if (!facts) { return {}; }
  result.facts = *facts; result.virtual_extent = *virtual_extent;
  auto file = source_header->root()->OpenFile(source_header->relative(),
      filesystem::FileAccess::kFileReadData, error);
  if (!file) { return {}; }
  const auto identity = file->QueryInfo(error);
  if (!identity || identity->filesystem_id != source_header->info().filesystem_id ||
      identity->file_id != source_header->info().file_id || identity->size != source_header->info().size) {
    if (!error) { error = std::make_error_code(std::errc::state_not_recoverable); } return {};
  }
  const auto metadata = filesystem::QueryRetainedFileMetadata(*file, error);
  if (!metadata) { return {}; }
  // CaptureSnapshot allows compatible retained readers; a mutation batch
  // would conflict with our own read lease. Re-read full bytes under the same
  // existing storage guard while that lease blocks cooperating replacement.
  const auto observed = source_header->root()->CaptureSnapshot(source_header->relative(),
      filesystem::content_mutation_wire::kMaximumMetadataBytes, error, true);
  if (!observed || observed->info().file_id != identity->file_id ||
      observed->info().filesystem_id != identity->filesystem_id ||
      observed->info().size != identity->size ||
      observed->bytes().size() != source_header->bytes().size() ||
      !std::equal(observed->bytes().begin(), observed->bytes().end(), source_header->bytes().begin())) {
    if (!error) { error = std::make_error_code(std::errc::state_not_recoverable); } return {};
  }
  // No native birth-time fact was supplied by the retained descriptor. Zero
  // is truthful unknown; POSIX ctime must not masquerade as creation time.
  result.information.create_timestamp = 0;
  file.reset();
  error = layout->CheckCurrent(); if (error) { return {}; }
  if (metadata->allocated_bytes > UINT64_MAX - layout->statistics().allocated_bytes ||
      result.facts.raw_extent > SIZE_MAX) {
    error = std::make_error_code(std::errc::value_too_large); return {};
  }
  result.information.attributes = vfs::kFileAttributeNormal |
      ((result.facts.read_only_format || source_.root->read_only()) ? vfs::kFileAttributeReadOnly : 0);
  result.information.end_of_file = result.facts.raw_extent;
  result.information.access_timestamp = metadata->access_timestamp;
  result.information.write_timestamp = metadata->write_timestamp;
  result.information.change_timestamp = metadata->change_timestamp;
  const auto status = vfs::RoundFileAllocationSize(metadata->allocated_bytes +
      layout->statistics().allocated_bytes, 512, &result.information.allocation_size);
  if (XFAILED(status) || result.information.allocation_size > SIZE_MAX) {
    error = std::make_error_code(std::errc::value_too_large); return {};
  }
  result.header = std::make_shared<const std::vector<uint8_t>>(
      source_header->bytes().begin(), source_header->bytes().end());
  result.header_info = source_header->info();
  result.layout = std::move(layout);
  if(exact_before)*exact_before=source_header;
  error.clear(); return result;
}
inline std::shared_ptr<const filesystem::HostStorageSnapshotToken>
NativeFolderContent::CaptureForParentAdmission(const ProfileStorageLease& storage,std::error_code& error) const {
  error.clear();
  const auto actual=storage.source()->directory_identity();
  const auto ours=source_.root->directory_identity();
  if(retiring() || storage.retiring() || !actual || !ours ||
      !actual->SameEntry({},*ours,{}) ||
      package_!=ProfileStorageLease::ProfileRelative(storage.xuid()) ||
      expected_.xuid!=storage.xuid() || expected_.title_id!=0xFFFE07D1 ||
      expected_.content_type!=XContentType::kProfile) {
    error=std::make_error_code(std::errc::permission_denied);return {};
  }
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> before;
  if(!CaptureCurrent(error,&before))return {};
  return before;
}
inline std::shared_ptr<const vfs::ManagedFileOpenAuthority>
NativeFolderContent::IssueParentReadForActualOpen(std::shared_ptr<HeaderSession> issued,
    std::shared_ptr<vfs::Device> requested,std::string path,std::shared_ptr<void> duty,std::error_code& error) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || retiring() || !issued || !duty || !requested ||
      issued->package()!=package_ || issued->target()!=target_ || !issued->expected() ||
      !MatchesDevice(*requested,path)) {
    error=std::make_error_code(std::errc::permission_denied);return {};
  }
  const auto& before=issued->expected();
  if(!before->root()->directory_identity() || !source_.root->directory_identity() ||
      !before->root()->directory_identity()->SameEntry(before->relative(),*source_.root->directory_identity(),target_,true)) {
    error=std::make_error_code(std::errc::permission_denied);return {};
  }
  return std::shared_ptr<const vfs::ManagedFileOpenAuthority>(new ParentReadAuthority(
      weak_from_this(),std::move(issued),std::move(requested),std::move(path),std::move(duty)));
}
inline std::optional<NativeFolderContent::Capture> NativeFolderContent::CaptureIssued(
    const std::shared_ptr<HeaderSession>& issued,std::error_code& error) const {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || retiring() || !issued ||
      issued->package()!=package_ || issued->target()!=target_ || !issued->expected()) {
    error=std::make_error_code(std::errc::permission_denied);return {};
  }
  const auto before=issued->expected();auto layout=issued->CaptureMemberLayout(error);
  if(!layout)return {};
  const auto extent=folder_content_detail::CaptureFolderVirtualExtent(*layout,before->bytes(),error);
  if(!extent)return {};
  const auto facts=folder_content_detail::ValidateNativeHeader(before->bytes(),expected_,extent->bytes,source_.certificate,error);
  if(!facts)return {};
  const auto metadata=issued->Query(error);if(!metadata)return {};
  Capture captured;captured.facts=*facts;captured.virtual_extent=*extent;
  captured.header=std::make_shared<const std::vector<uint8_t>>(before->bytes().begin(),before->bytes().end());
  captured.header_info=before->info();captured.layout=std::move(layout);
  captured.information.attributes=vfs::kFileAttributeNormal;
  captured.information.end_of_file=facts->raw_extent;
  captured.information.create_timestamp=0;
  captured.information.access_timestamp=metadata->access_timestamp;
  captured.information.write_timestamp=metadata->write_timestamp;
  captured.information.change_timestamp=metadata->change_timestamp;
  if(metadata->allocated_bytes>UINT64_MAX-captured.layout->statistics().allocated_bytes ||
      XFAILED(vfs::RoundFileAllocationSize(metadata->allocated_bytes+captured.layout->statistics().allocated_bytes,
        512,&captured.information.allocation_size))) {
    error=std::make_error_code(std::errc::value_too_large);return {};
  }
  return captured;
}
inline X_STATUS NativeFolderContent::OpenWithContext(std::shared_ptr<vfs::Device> requested,
    std::string path,vfs::FileDisposition disposition,uint32_t access,bool directory,
    vfs::File** output,vfs::FileAction* action,const vfs::FileOpenContext& context) {
  if(!context.managed_authority)return Open(std::move(requested),std::move(path),disposition,access,directory,output,action);
  if(output)*output=nullptr;if(action)*action=vfs::FileAction::kDoesNotExist;
  const auto authority=std::dynamic_pointer_cast<const ParentReadAuthority>(context.managed_authority);
  if(!output || !action || directory || !authority || authority->policy_.lock().get()!=this ||
      requested!=authority->requested_ || context.expected_device!=requested || path!=authority->path_ ||
      !MatchesDevice(*requested,path) || (disposition!=vfs::FileDisposition::kOpen && disposition!=vfs::FileDisposition::kOpenIf) ||
      authority->consumed_.exchange(true,std::memory_order_acq_rel))return X_STATUS_ACCESS_DENIED;
  std::error_code error;auto captured=CaptureIssued(authority->issued_,error);
  if(!captured)return vfs::HostStorageError(error);
  auto state=std::make_shared<State>();state->policy=shared_from_this();state->requested=requested;
  state->current=std::move(*captured);state->publication=authority->issued_;
  auto entry=std::make_unique<RawEntry>(requested.get(),std::move(path),state->current.information,physical_package_);
  auto file=std::make_unique<RawFile>(access,std::move(entry),std::move(state));
  *output=file.release();*action=vfs::FileAction::kOpened;return X_STATUS_SUCCESS;
}
inline std::shared_ptr<NativeFolderContent> NativeFolderContent::Prepare(Source source,
    std::filesystem::path package, std::shared_ptr<const filesystem::HostStorageSnapshotToken> actual,
    folder_content_detail::PackageIdentity expected, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !source.root || !source.managed ||
      source.managed->error() || !source.managed->identity() || !source.managed->mutations() ||
      !source.volume || source.volume->origin() != filesystem::ConsoleStorageVolume::Origin::kRegisteredSource ||
      source.volume->source_id() != source.managed->identity()->source_id() ||
      source.volume->device_id() != source.managed->identity()->device_id() ||
      !source.root->directory_identity() || !source.managed->content()->directory_identity() ||
      !source.root->directory_identity()->SameEntry({}, *source.managed->content()->directory_identity(), {}) ||
      !actual || actual->root() != source.managed->content() || !actual->ascii_folded_leaf() ||
      !actual->exists()) {
    error = std::make_error_code(std::errc::permission_denied); return {};
  }
  filesystem::content_mutation_wire::Record slot;
  slot.kind = filesystem::ContentMutationOwner::Kind::kHeader;
  slot.package = package; slot.target = actual->relative();
  if (filesystem::content_mutation_wire::Target(slot).empty()) {
    error = std::make_error_code(std::errc::permission_denied); return {};
  }
  std::vector<std::string> parts;
  for (const auto& part : package) { parts.push_back(part.generic_string()); }
  const auto hex = [](std::string_view text) {
    uint64_t value = 0;
    for (unsigned char c : text) {
      const auto digit = c >= '0' && c <= '9' ? c - '0'
          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c - 'a' + 10;
      value = value * 16 + uint64_t(digit);
    }
    return value;
  };
  // ValidPackage above supplies exact ASCII hex widths. Metadata may not
  // choose another owner/title/type after a real physical package was selected.
  if (parts.size() != 4 || hex(parts[0]) != expected.xuid ||
      hex(parts[1]) != expected.title_id || hex(parts[2]) != uint32_t(expected.content_type) ||
      (expected.content_type == XContentType::kProfile &&
       !xe::utf8::equal_case(parts[0], parts[3]))) {
    error = std::make_error_code(std::errc::permission_denied); return {};
  }
  if (!source.managed->mutations()->InstallNativeHeaderFormat(
      std::make_shared<const folder_content_detail::NativeFolderHeaderFormat>(source.certificate), error)) {
    return {};
  }
  auto result = std::shared_ptr<NativeFolderContent>(new NativeFolderContent);
  result->source_ = std::move(source); result->package_ = std::move(package);
  result->target_ = actual->relative(); result->expected_ = expected;
  result->physical_package_ = result->source_.root->path() / result->package_;
  auto parent = result->source_.root->Subroot(result->package_.parent_path(), false, error);
  if (!parent) { return {}; } result->parent_pin_ = parent->directory_identity();
  const auto captured = result->CaptureCurrent(error); if (!captured) { return {}; }
  if (captured->header_info.file_id != actual->info().file_id ||
      captured->header_info.filesystem_id != actual->info().filesystem_id ||
      captured->header->size() != actual->bytes().size() ||
      !std::equal(actual->bytes().begin(), actual->bytes().end(), captured->header->begin())) {
    error = std::make_error_code(std::errc::state_not_recoverable); return {};
  }
  result->namespace_ = std::make_shared<vfs::ManagedFileNamespaceView>(
      std::make_shared<const vfs::File::Information>(captured->information));
  return result;
}
inline X_STATUS NativeFolderContent::Open(std::shared_ptr<vfs::Device> requested,
    std::string path, vfs::FileDisposition disposition, uint32_t access,
    bool directory, vfs::File** output, vfs::FileAction* action) {
  if (output) { *output = nullptr; } if (action) { *action = vfs::FileAction::kDoesNotExist; }
  if (!output || !action || !requested || directory ||
      (disposition != vfs::FileDisposition::kOpen && disposition != vfs::FileDisposition::kOpenIf)) {
    return X_STATUS_ACCESS_DENIED;
  }
  if (global_critical_region::is_held_by_current_thread()) { return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
  if (!MatchesDevice(*requested, path)) { return X_STATUS_ACCESS_DENIED; }
  std::error_code error; auto captured = CaptureCurrent(error);
  if (!captured) { return vfs::HostStorageError(error); }
  auto info = std::make_shared<const vfs::File::Information>(captured->information);
  auto state = std::make_shared<State>(); state->policy = shared_from_this();
  state->requested = requested; state->current = std::move(*captured);
  auto entry = std::make_unique<RawEntry>(requested.get(), std::move(path), state->current.information,physical_package_);
  auto file = std::make_unique<RawFile>(access, std::move(entry), std::move(state));
  namespace_->PublishPrepared(std::move(info));
  *output = file.release(); *action = vfs::FileAction::kOpened; return X_STATUS_SUCCESS;
}
}  // namespace xe::kernel::xam
#endif
