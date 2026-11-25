/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

#include <algorithm>
#include <optional>
#include <utility>
#include "devices/host_path_entry.h"
#include "third_party/utfcpp/source/utf8.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/xfile.h"

namespace xe {
namespace vfs {

using namespace xe::literals;

namespace {
class ClosedManagedFile final : public ManagedFileService {
 public:
  ClosedManagedFile(std::filesystem::path path,
      std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent)
      : path_(std::move(path)), parent_(std::move(parent)) {}
  const std::filesystem::path& backing_path() const override { return path_; }
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const override { return parent_; }
  bool retiring() const override { return true; }
  X_STATUS Open(std::shared_ptr<Device>, std::string, FileDisposition,
      uint32_t, bool, File** file, FileAction* action) override {
    if (file) { *file = nullptr; }
    if (action) { *action = FileAction::kDoesNotExist; }
    return X_STATUS_CANCELLED;
  }
 private:
  std::filesystem::path path_;
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent_;
};
bool SameManagedLeaf(const std::filesystem::path& a,
                       const std::filesystem::path& b) {
  return a.parent_path() == b.parent_path() &&
         xe::utf8::equal_case(a.filename().string(), b.filename().string());
}
bool ContainsManagedLeaf(const std::filesystem::path& parent,
                           const std::filesystem::path& leaf) {
  const auto relative = leaf.lexically_relative(parent);
  return !relative.empty() && !relative.is_absolute() &&
         *relative.begin() != "..";
}
}  // namespace
bool ManagedFileRegistry::Register(std::shared_ptr<ManagedFileService> service) {
  // An inner lock release cannot undo a caller's recursive GCRC ownership.
  // All strong service rundown must follow the actual outermost release.
  if (xe::global_critical_region::is_held_by_current_thread()) { return false; }
  if (!service || !service->backing_path().is_absolute() ||
      service->backing_path().lexically_normal() != service->backing_path() ||
      service->retiring()) { return false; }
  const auto parent=service->backing_directory();
  const auto role = service->namespace_role();
  if (!parent || parent->path()!=service->backing_path().parent_path()) { return false; }
  // Weak locking can race external retirement. Retain every acquired old
  // service until AFTER the registry/GCRC owner is destroyed on every return.
  // The row cap bounds preparation; no reallocation occurs while locked.
  std::vector<std::shared_ptr<ManagedFileService>> retired_keepalive;
  retired_keepalive.reserve(4096);
  auto lock = namespace_.Acquire();
  for (auto& old : services_) {
    const bool physical=old.parent->SameEntry(old.path.filename(),*parent,
        service->backing_path().filename(),true);
    if (SameManagedLeaf(old.path, service->backing_path()) || physical) {
      const auto& owner=retired_keepalive.emplace_back(old.service.lock());
      if (owner == service) { return true; }
      if (owner && !owner->retiring()) { return false; }
      if (physical) {
        old.service = service; old.role = role; return true;
      }
      // A new physical directory at the same spelling does not erase the old
      // retained alias tombstone. The new row wins exact current-path lookup.
    }
  }
  if (services_.size() == 4096) { return false; }
  services_.push_back({service->backing_path(),parent,service,role});
  return true;
}
std::shared_ptr<ManagedFileService> ManagedFileRegistry::Find(
    const std::filesystem::path& path,
    std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> root) const {
  const auto relative=root ? path.lexically_relative(root->path()) : std::filesystem::path{};
  auto lock = namespace_.Acquire();
  const Row* lexical_tombstone=nullptr;
  for (auto it=services_.rbegin();it!=services_.rend();++it) {
    const auto& service=*it;
    const bool physical=root && service.parent->SameEntry(
        service.path.filename(),*root,relative,true);
    if (physical || (!root && SameManagedLeaf(service.path,path))) {
      auto owner=service.service.lock();
      return owner ? owner : std::make_shared<ClosedManagedFile>(service.path,service.parent);
    }
    if (!lexical_tombstone && SameManagedLeaf(service.path,path)) {
      lexical_tombstone=&service;
    }
  }
  // A source replaced at the same spelling keeps its old native identity; it
  // cannot borrow the new source's service merely because the strings match.
  return lexical_tombstone ? std::make_shared<ClosedManagedFile>(
      lexical_tombstone->path,lexical_tombstone->parent) : nullptr;
}
bool ManagedFileRegistry::Protects(const std::filesystem::path& path,
    bool subtree,
    std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> root) const {
  const auto relative=root ? path.lexically_relative(root->path()) : std::filesystem::path{};
  auto lock = namespace_.Acquire();
  for (const auto& service : services_) {
    if (SameManagedLeaf(service.path, path) ||
        (subtree && ContainsManagedLeaf(path, service.path))) {
      return true;
    }
    if (!root || !service.parent->Overlaps(service.path.filename(), *root,
                                           relative, subtree, true)) {
      continue;
    }
    if (service.role == ManagedFileService::NamespaceRole::kRawFolderProjection &&
        service.parent->OverlapsKnown(service.path.filename(), *root,
                                      relative, false, true) &&
        !service.parent->SameEntry(service.path.filename(), *root,
                                   relative, true)) {
      // This is a PROVED physical descendant of the projected package node.
      // It needs its own real member File policy, not the raw package policy.
      // Exact package/ancestor operations and unknown physical ancestry still
      // refuse. Other exact managed member rows are checked independently.
      continue;
    }
    return true;
  }
  return false;
}
bool VirtualFileSystem::BindManagedFileRegistryForPreparation(Device& device) {
  if (global_critical_region::is_held_by_current_thread() ||
      !device.weak_from_this().expired() || device.namespace_retiring()) return false;
  device.SetManagedFileRegistry(managed_files_);
  return true;
}
VirtualFileSystem::VirtualFileSystem()
    : managed_files_(std::make_shared<ManagedFileRegistry>()) {}

VirtualFileSystem::~VirtualFileSystem() {
  // Delete all devices.
  // This will explode if anyone is still using data from them.
  Clear();
}

void VirtualFileSystem::BeginStoragePreparation() {
  auto next=std::make_shared<std::stop_source>();
  std::shared_ptr<std::stop_source> old;
  auto namespace_lock=global_critical_region_.Acquire();
  { std::lock_guard lock(preparation_mutex_); old=std::exchange(preparation_stop_,std::move(next)); }
  if (old) old->request_stop();
}
void VirtualFileSystem::SealStoragePreparation() {
  std::shared_ptr<std::stop_source> stop;
  // Serialize the cutoff with metadata publication, not with filesystem IO.
  auto namespace_lock=global_critical_region_.Acquire();
  { std::lock_guard lock(preparation_mutex_); stop=preparation_stop_; }
  if (stop) stop->request_stop();
}
void VirtualFileSystem::Clear() {
  std::vector<std::shared_ptr<Device>> retired;
  {
    auto lock = global_critical_region_.Acquire();
    for (const auto& device : devices_) { device->BeginNamespaceRetirement(); }
    retired.swap(devices_);
    if (device_epoch_ != UINT64_MAX) { ++device_epoch_; }
    symlinks_.clear();
  }
  // Newest first, outside the namespace lock: devices can own other files.
  while (!retired.empty()) {
    retired.back()->Retire();
    retired.pop_back();
  }
}

std::unique_ptr<VirtualFileSystem::PreparedDeviceRegistration>
VirtualFileSystem::PrepareDeviceRegistration(std::unique_ptr<Device>& device,
                                             std::error_code& error,
                                             const std::shared_ptr<Device>& parent,
                                             std::unique_ptr<SourceParentSelection> source_selection,
                                             filesystem::HostStorageFailureObservation* diagnostic) {
  error.clear();
  filesystem::HostStorageFailureScope observation{diagnostic,error,5};
  observation.At(1);
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    observation.Capture(__LINE__); return {};
  }
  std::shared_ptr<std::stop_source> epoch;
  { std::lock_guard lock(preparation_mutex_); epoch=preparation_stop_; }
  filesystem::HostStoragePreparationWait preparation(
      std::chrono::steady_clock::now()+std::chrono::seconds(2));
  if (!epoch || !preparation.LinkCancellation(epoch->get_token())) {
    error=std::make_error_code(std::errc::operation_canceled); observation.Capture(__LINE__); return {};
  }
  const auto backing_parent = device ? device->RetainedBackingParent() : nullptr;
  observation.At(2);
  if (parent && backing_parent && parent != backing_parent) {
    error = std::make_error_code(std::errc::invalid_argument); observation.Capture(__LINE__); return {};
  }
  const auto exact_parent = parent ? parent : backing_parent;
  observation.At(3);
  if (exact_parent && exact_parent->namespace_retiring()) {
    error = std::make_error_code(std::errc::no_such_device); observation.Capture(__LINE__); return {};
  }
  observation.At(4);
  if (source_selection && (!device || source_selection->child_ != device.get() ||
      source_selection->parent_ != exact_parent || !source_selection->volume_ ||
      source_selection->volume_ != device->storage_volume() ||
      !source_selection->certificate_ || !source_selection->source_ ||
      source_selection->certificate_->retiring() || source_selection->source_->retiring())) {
    error = std::make_error_code(std::errc::invalid_argument); observation.Capture(__LINE__); return {};
  }
  observation.At(5);
  if (source_selection) {
    const auto certificate = source_selection->certificate_->directory_identity();
    const auto source = source_selection->source_->directory_identity();
    if (!certificate || !source || !certificate->SameEntry({}, *source, {})) {
      error = std::make_error_code(std::errc::permission_denied); observation.Capture(__LINE__); return {};
    }
  }
  observation.At(6);
  if (!device || device->namespace_retiring() || !device->BindMountParent(exact_parent,
      source_selection ? source_selection->volume_ : nullptr)) {
    error = std::make_error_code(std::errc::invalid_argument); observation.Capture(__LINE__); return {};
  }
  // Every path, including the legacy convenience APIs, uses the SAME actual
  // backing/projection/private-store admission. No marker or caller bool
  // substitutes for that retained common authority.
  device->FreezeStorageVolume();
  // Lifetime edges are retained and immutable. Each linked token is negative
  // cancellation only; no volume/source/parent permission is inferred.
  size_t depth=0;
  for (const Device* node=device.get();node;node=node->mount_parent().get()) {
    const auto volume=node->storage_volume();
    if (++depth>1024 || !preparation.LinkCancellation(node->namespace_retirement_token()) ||
        (volume && !preparation.LinkCancellation(volume->retirement_token()))) {
      error=std::make_error_code(std::errc::value_too_large); observation.Capture(__LINE__); return {};
    }
  }
  if (source_selection &&
      (!source_selection->certificate_->LinkPreparationCancellation(preparation) ||
       !source_selection->source_->LinkPreparationCancellation(preparation))) {
    error=std::make_error_code(std::errc::value_too_large); observation.Capture(__LINE__); return {};
  }
  // Waiting may observe a source withdrawal before the original metadata
  // checks run. Preserve its specific disconnected status; an epoch or raw
  // cache cancellation alone remains cancellation and grants no authority.
  const auto classify_cancellation = [&] {
    if (error == std::errc::operation_canceled &&
        (device->namespace_retiring() ||
         (source_selection && (source_selection->certificate_->retiring() ||
                               source_selection->source_->retiring())))) {
      error = std::make_error_code(std::errc::no_such_device);
    }
  };
  observation.At(7);
  if (!device->AdmitGuestStorage(error, diagnostic, &preparation)) {
    classify_cancellation();
    observation.Capture(__LINE__); return {};
  }
  // A prior registry can own real directory/ancestor pins. Replace it only
  // in this prepared host lane, never under commit's inherited GCRC. The VFS
  // lifetime tag fixes this binding for the exact later metadata publication.
  if ((error=preparation.Check()) || device->namespace_retiring() ||
      (source_selection && (source_selection->certificate_->retiring() || source_selection->source_->retiring()))) {
    if (!error) error=std::make_error_code(std::errc::no_such_device);
    classify_cancellation();
    observation.Capture(__LINE__); return {};
  }
  device->SetManagedFileRegistry(managed_files_);
  // Allocate the capsule before consuming the caller. shared_ptr's
  // unique_ptr conversion preserves that unique_ptr if allocation throws.
  auto prepared = std::unique_ptr<PreparedDeviceRegistration>(
      new PreparedDeviceRegistration(registration_owner_,{}));
  prepared->preparation_tokens_.assign(preparation.cancellation_tokens().begin(),preparation.cancellation_tokens().end());
  prepared->preparation_flags_.assign(preparation.cancellation_flags().begin(),preparation.cancellation_flags().end());
  prepared->device_ = std::shared_ptr<Device>(std::move(device));
  prepared->source_selection_ = std::move(source_selection);
  return prepared;
}
X_STATUS VirtualFileSystem::CommitPreparedDevice(
    PreparedDeviceRegistration& prepared,bool exclusive,
    std::shared_ptr<Device>* registered,
    const std::shared_ptr<Device>& expected,std::shared_ptr<Device>* retired) {
  // Never overwrite/release a caller's old device while its outer lock is
  // still held. All failure/success paths retain the opaque capsule itself.
  if (prepared.owner_ != registration_owner_ || !prepared.device_ ||
      (registered && *registered) || (retired && *retired) ||
      (expected && (!exclusive || !retired))) { return X_STATUS_INVALID_PARAMETER; }
  auto lock = global_critical_region_.Acquire();
  if (prepared.consumed_) { return X_STATUS_INVALID_PARAMETER; }
  const auto& device = prepared.device_;
  const auto volume = device->storage_volume();
  if (device->namespace_retiring() || (volume && !volume->available_cached())) {
    return X_STATUS(0xC000009D);
  }
  // The immutable edge names actual registered objects, never a same-name
  // relookup. This also rejects stale grandparents and impossible cycles.
  size_t remaining = devices_.size() + 1;
  for (auto parent = device->mount_parent(); parent; parent = parent->mount_parent()) {
    if (!remaining-- || parent == device || parent->namespace_retiring() ||
        std::ranges::find(devices_, parent) == devices_.end()) { return X_STATUS(0xC000009D); }
    const auto ancestor_volume = parent->storage_volume();
    if (ancestor_volume && !ancestor_volume->available_cached()) { return X_STATUS(0xC000009D); }
  }
  if (prepared.source_selection_ && (prepared.source_selection_->source_->retiring() ||
      prepared.source_selection_->certificate_->retiring())) { return X_STATUS(0xC000009D); }
  if (device_epoch_ == UINT64_MAX) { return X_STATUS_INSUFFICIENT_RESOURCES; }
  if(expected) {
    if(std::ranges::find(devices_,expected)==devices_.end() ||
        !xe::utf8::equal_case(expected->mount_path(),device->mount_path()) ||
        expected->retirement_owner_.lock() || expected->has_open_files() ||
        std::ranges::any_of(devices_,[&](const auto& candidate){
          for(auto parent=candidate->mount_parent();parent;parent=parent->mount_parent())
            if(parent==expected)return true;
          return false;
        }))return HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));
    for(auto parent=device->mount_parent();parent;parent=parent->mount_parent())
      if(parent==expected)return X_STATUS_INVALID_PARAMETER;
  }
  if (exclusive) {
    for (const auto& existing : devices_) {
      if(existing==expected)continue;
      if (xe::utf8::equal_case(existing->mount_path(),device->mount_path())) {
        return X_STATUS_OBJECT_NAME_COLLISION;
      }
    }
    for (const auto& link : symlinks_) {
      if (xe::utf8::equal_case(link.first,device->mount_path())) {
        return X_STATUS_OBJECT_NAME_COLLISION;
      }
    }
  }
  // Preserve specific source/device/parent validation failures before the
  // generic preparation cancellation result. Cancellation is still checked
  // under GCRC before any publication or mutation of registered devices.
  if (std::any_of(prepared.preparation_tokens_.begin(),prepared.preparation_tokens_.end(),
        [](const auto& token){return token.stop_requested();}) ||
      std::any_of(prepared.preparation_flags_.begin(),prepared.preparation_flags_.end(),
        [](const auto& flag){return flag->load(std::memory_order_acquire);})) return X_STATUS_CANCELLED;
  // Metadata only. push_back has its usual strong exception guarantee: the
  // capsule keeps the actual admitted device on allocation/publication failure.
  // Allocation/strong exception guarantee precedes any old-node mutation.
  devices_.push_back(device);
  if(expected) {
    *retired=expected;
    expected->BeginNamespaceRetirement();
    devices_.erase(std::ranges::find(devices_,expected));
  }
  ++device_epoch_;
  prepared.consumed_ = true;
  if (registered) { *registered = device; }
  return X_STATUS_SUCCESS;
}
bool VirtualFileSystem::RegisterPreparedDevice(PreparedDeviceRegistration& prepared) {
  return XSUCCEEDED(CommitPreparedDevice(prepared,false,nullptr));
}
X_STATUS VirtualFileSystem::RegisterPreparedDeviceExclusive(
    PreparedDeviceRegistration& prepared,std::shared_ptr<Device>& registered) {
  return CommitPreparedDevice(prepared,true,&registered);
}
X_STATUS VirtualFileSystem::ReplacePreparedDeviceExclusive(
    PreparedDeviceRegistration& prepared,const std::shared_ptr<Device>& expected,
    std::shared_ptr<Device>& registered,std::shared_ptr<Device>& retired) {
  return CommitPreparedDevice(prepared,true,&registered,expected,&retired);
}
bool VirtualFileSystem::RegisterDevice(std::unique_ptr<Device> device) {
  std::error_code error;
  auto prepared = PrepareDeviceRegistration(device,error);
  if (!prepared) {
    if (error) { XELOGE("Guest storage mount refused: {}",error.message()); }
    return false;
  }
  return RegisterPreparedDevice(*prepared);
}
X_STATUS VirtualFileSystem::RegisterDeviceExclusive(
    std::unique_ptr<Device> device,std::shared_ptr<Device>& registered) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return HostStorageError(
        std::make_error_code(std::errc::device_or_resource_busy));
  }
  registered.reset();
  std::error_code error;
  auto prepared = PrepareDeviceRegistration(device,error);
  if (!prepared) { return error ? HostStorageError(error) : X_STATUS_UNSUCCESSFUL; }
  return RegisterPreparedDeviceExclusive(*prepared,registered);
}

std::shared_ptr<Device> VirtualFileSystem::DetachDevice(
    std::string_view path, const Device* expected) {
  auto lock = global_critical_region_.Acquire();
  const auto found = std::ranges::find_if(devices_, [&](const auto& device) {
    return device->mount_path() == path;
  });
  if (found == devices_.end() || (expected && found->get() != expected) ||
      (*found)->has_open_files()) { return {}; }
  (*found)->BeginNamespaceRetirement();
  auto retired = std::move(*found);
  devices_.erase(found);
  if (device_epoch_ != UINT64_MAX) { ++device_epoch_; }
  return retired;
}

std::unique_ptr<VirtualFileSystem::DeviceRetirement>
VirtualFileSystem::PrepareDeviceRetirement(const std::shared_ptr<Device>& root,
    bool include_root, X_STATUS& status, DeviceRetirementPolicy policy) {
  status = X_STATUS_INVALID_PARAMETER;
  if (!root) { return {}; }
  auto lock = global_critical_region_.Acquire();
  // An already withdrawn exact root may still own genuinely live descendants
  // or retained Files. It cannot select a replacement through its old name.
  if (std::ranges::find(devices_, root) == devices_.end() && !root->namespace_retiring()) {
    status = X_STATUS_INVALID_HANDLE; return {};
  }
  auto plan = std::unique_ptr<DeviceRetirement>(new DeviceRetirement(
      registration_owner_, root, device_epoch_, policy));
  plan->devices_.reserve(devices_.size() + 1);
  plan->devices_.push_back(root);
  for (size_t i = 0; i != plan->devices_.size(); ++i) {
    const auto& parent = plan->devices_[i];
    for (const auto& device : devices_) {
      if (device->mount_parent() == parent &&
          std::ranges::find(plan->devices_, device) == plan->devices_.end()) {
        plan->devices_.push_back(device);
      }
    }
  }
  if (!include_root) { plan->devices_.erase(plan->devices_.begin()); }
  status = X_STATUS_SUCCESS;
  return plan;
}
X_STATUS VirtualFileSystem::CommitDeviceRetirement(DeviceRetirement& plan) {
  auto lock = global_critical_region_.Acquire();
  if (plan.owner_ != registration_owner_ || plan.committed_ ||
      plan.epoch_ != device_epoch_) { return X_STATUS_INVALID_HANDLE; }
  const auto claimed_elsewhere = [&](const auto& device) {
    const auto active = device->retirement_owner_.lock();
    return active && active != plan.closure_owner_;
  };
  if (claimed_elsewhere(plan.root_) ||
      std::ranges::any_of(plan.devices_, claimed_elsewhere)) {
    return HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));
  }
  plan.root_->retirement_owner_ = plan.closure_owner_;
  for (const auto& device : plan.devices_) {
    device->retirement_owner_ = plan.closure_owner_;
  }
  plan.root_->BeginNamespaceRetirement();
  for (const auto& device : plan.devices_) { device->BeginNamespaceRetirement(); }
  // Keep nodes in the one graph until real handle rundown. New children and
  // Entry admission are now closed, while genuine old owners can still drain.
  if (device_epoch_ != UINT64_MAX) { ++device_epoch_; }
  plan.committed_ = true;
  return X_STATUS_SUCCESS;
}
X_STATUS VirtualFileSystem::FinishDeviceRetirement(DeviceRetirement& plan,
    size_t index, std::shared_ptr<Device>& retired) {
  auto lock = global_critical_region_.Acquire();
  if (retired || plan.owner_ != registration_owner_ || !plan.committed_ ||
      index >= plan.devices_.size() || !plan.devices_[index]) {
    return X_STATUS_INVALID_HANDLE;
  }
  const auto& device = plan.devices_[index];
  if (device->retirement_owner_.lock() != plan.closure_owner_) {
    return X_STATUS_INVALID_HANDLE;
  }
  // A failed/busy descendant cannot be bypassed by finalizing an ancestor.
  for (const auto& pending : plan.devices_) {
    if (!pending || pending == device) { continue; }
    for (auto parent = pending->mount_parent(); parent;
         parent = parent->mount_parent()) {
      if (parent == device) { return HostStorageError(std::make_error_code(std::errc::device_or_resource_busy)); }
    }
  }
  if (plan.policy_ == DeviceRetirementPolicy::kDrainOpenFiles &&
      device->has_open_files()) {
    return HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));
  }
  // Exact pointer membership only. An already withdrawn old node may finish;
  // an unrelated replacement at the old name is never selected or detached.
  const auto found = std::ranges::find(devices_, device);
  if (found != devices_.end()) {
    retired = std::move(plan.devices_[index]);
    devices_.erase(found);
    if (device_epoch_ != UINT64_MAX) { ++device_epoch_; }
  } else { retired = std::move(plan.devices_[index]); }
  return X_STATUS_SUCCESS;
}
X_STATUS VirtualFileSystem::RetireDeviceBackings(DeviceRetirement& plan) {
  if (plan.owner_ != registration_owner_ || !plan.committed_ ||
      global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  X_STATUS result = X_STATUS_SUCCESS;
  for (size_t i = plan.devices_.size(); i--;) {
    std::shared_ptr<Device> retired;
    const auto status = FinishDeviceRetirement(plan, i, retired);
    if (XFAILED(status)) { result = status; continue; }
    retired->Retire();
    retired.reset(); // Release the child backing before testing its parent.
  }
  return result;
}
bool VirtualFileSystem::UnregisterDevice(const std::string_view path,
                                          const Device* expected) {
  if (xe::global_critical_region::is_held_by_current_thread()) { return false; }
  std::unique_ptr<DeviceRetirement> plan;
  {
    auto lock = global_critical_region_.Acquire();
    auto root = AcquireDevice(path);
    // Preserve the legacy convenience API's busy result. Runtime withdrawal
    // uses the explicit owned batch after arranging genuine handle rundown.
    if (!root || (expected && root.get() != expected) || root->has_open_files()) { return false; }
    X_STATUS status;
    plan = PrepareDeviceRetirement(root, true, status,
        DeviceRetirementPolicy::kDrainOpenFiles);
    if (!plan || XFAILED(CommitDeviceRetirement(*plan))) { return false; }
  }
  return XSUCCEEDED(RetireDeviceBackings(*plan));
}

bool VirtualFileSystem::RetireDevice(const std::string_view path) {
  if (xe::global_critical_region::is_held_by_current_thread()) { return false; }
  std::shared_ptr<Device> retired;
  {
    auto lock = global_critical_region_.Acquire();
    const auto found = std::ranges::find_if(devices_, [&](const auto& device) {
      return device->mount_path() == path;
    });
    if (found == devices_.end()) { return false; }
    retired = *found;
    retired->BeginNamespaceRetirement();
  }
  retired->Retire();
  return true;
}

std::shared_ptr<Device> VirtualFileSystem::AcquireDevice(
    const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& device : devices_) {
    if (device->mount_path() == path) {
      return device;
    }
  }
  return {};
}

Device* VirtualFileSystem::GetDevice(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = devices_.begin(); it != devices_.end(); ++it) {
    if ((*it)->mount_path() == path) {
      return (*it).get();
    }
  }
  return nullptr;
}

namespace {
std::optional<std::string> NamespacePath(std::string_view path) {
  if (path.empty() || path.size() > VirtualFileSystem::kMaximumTranslatedPath ||
      path.find('\0') != std::string_view::npos ||
      !::utf8::is_valid(path.begin(), path.end())) {
    return {};
  }
  auto result = xe::utf8::canonicalize_guest_path(path);
  // Bare/DosDevices names are TITLE aliases. SYSTEM aliases remain distinct.
  for (std::string_view prefix : {"\\??\\", "\\DosDevices\\"}) {
    if (xe::utf8::starts_with_case(result, prefix)) {
      result.erase(0, prefix.size());
      break;
    }
  }
  return result.empty() ? std::nullopt
                        : std::optional<std::string>(std::move(result));
}
bool AliasMatches(std::string_view path, std::string_view alias) {
  if (!xe::utf8::starts_with_case(path, alias)) {
    return false;
  }
  return path.size() == alias.size() || alias.back() == ':' ||
         alias.back() == '\\' || path[alias.size()] == '\\';
}
template <typename Links>
auto LongestAlias(const Links& links, std::string_view path) {
  auto found = links.end();
  for (auto it = links.begin(); it != links.end(); ++it) {
    if (AliasMatches(path, it->first) &&
        (found == links.end() || it->first.size() > found->first.size())) {
      found = it;
    }
  }
  return found;
}
}  // namespace

namespace {
bool MountPathMatches(std::string_view path, std::string_view mount_path);
}

bool VirtualFileSystem::SymbolicLinkCurrent(const SymbolicLink& link) const {
  if (!link.expected_device) { return true; }
  // Compare control-block identity without weak.lock(): the last strong
  // reference must never be destroyed in this namespace interval.
  return std::ranges::any_of(devices_, [&](const auto& device) {
    return device.get() == link.expected_device &&
           !link.device.owner_before(device) &&
           !device.owner_before(link.device) && !device->namespace_retiring();
  });
}

bool VirtualFileSystem::SymbolicLinkAllowsPath(const SymbolicLink& link,
                                               std::string_view path) const {
  if (!link.expected_device) { return true; }
  const Device* selected = nullptr;
  for (const auto& device : devices_) {
    if (MountPathMatches(path, device->mount_path()) &&
        (!selected || device->mount_path().size() > selected->mount_path().size())) {
      selected = device.get();
    }
  }
  // A nested mount must be a real child of the selected source. A different
  // same-prefix device or a chained generic alias cannot borrow its authority.
  for (auto* node = selected; node; node = node->mount_parent().get()) {
    if (node == link.expected_device) {
      return SymbolicLinkCurrent(link) && !selected->namespace_retiring();
    }
  }
  return false;
}

X_STATUS VirtualFileSystem::RegisterDeviceSymbolicLink(std::string_view path,
    std::string_view target, const std::shared_ptr<Device>& expected,
    std::shared_ptr<const SymbolicLinkToken>& issued) {
  if (issued || !expected) { return X_STATUS_INVALID_PARAMETER; }
  try {
    const auto alias = NamespacePath(path);
  const auto destination = NamespacePath(target);
  if (!alias || !destination ||
      !MountPathMatches(*destination, expected->mount_path())) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
    auto identity = std::make_shared<const uint8_t>(0);
    auto token = std::shared_ptr<const SymbolicLinkToken>(
        new SymbolicLinkToken(registration_owner_, *alias, identity));
    SymbolicLink row;
    row.target = *destination;
    row.device = expected;
    row.expected_device = expected.get();
    row.identity = std::move(identity);
    auto lock = global_critical_region_.Acquire();
    if (!SymbolicLinkCurrent(row)) {
      return HostStorageError(std::make_error_code(std::errc::no_such_device));
    }
    if (std::ranges::any_of(symlinks_, [&](const auto& link) {
          return xe::utf8::equal_case(*alias, link.first);
        })) {
      return X_STATUS_OBJECT_NAME_COLLISION;
    }
    symlinks_.emplace(*alias, std::move(row));
    issued = std::move(token);
    return X_STATUS_SUCCESS;
  } catch (...) { return X_STATUS_NO_MEMORY; }
}

bool VirtualFileSystem::UnregisterSymbolicLink(const SymbolicLinkToken& expected) {
  if (expected.owner_ != registration_owner_) { return false; }
  auto lock = global_critical_region_.Acquire();
  const auto found = std::ranges::find_if(symlinks_, [&](const auto& link) {
    return xe::utf8::equal_case(expected.alias_, link.first) &&
           link.second.identity == expected.identity_;
  });
  if (found == symlinks_.end()) { return false; }
  symlinks_.erase(found);
  return true;
}

bool VirtualFileSystem::RegisterSymbolicLink(const std::string_view path,
                                             const std::string_view target) {
  const auto normalized = NamespacePath(path);
  const auto normalized_target = NamespacePath(target);
  if (!normalized || !normalized_target) {
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  auto it = std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
    return xe::utf8::equal_case(*normalized, s.first);
  });
  if (it != symlinks_.end()) {
    XELOGE("Trying to re-register already registered symbolic link: {} => {}",
           path, target);
    return false;
  }

  SymbolicLink link;
  link.target = *normalized_target;
  symlinks_.emplace(*normalized, std::move(link));
  XELOGD("Registered symbolic link: {} => {}", path, target);

  return true;
}

bool VirtualFileSystem::UnregisterSymbolicLink(const std::string_view path) {
  const auto normalized = NamespacePath(path);
  if (!normalized) {
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  auto it = std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
    return xe::utf8::equal_case(*normalized, s.first);
  });
  if (it == symlinks_.end()) {
    return false;
  }
  XELOGD("Unregistered symbolic link: {} => {}", it->first, it->second.target);

  symlinks_.erase(it);
  return true;
}

bool VirtualFileSystem::IsSymbolicLinkRegistered(const std::string_view path) {
  const auto normalized = NamespacePath(path);
  if (!normalized) {
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  auto it = std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
    return xe::utf8::equal_case(*normalized, s.first);
  });

  return it != symlinks_.cend();
}

bool VirtualFileSystem::FindSymbolicLink(const std::string_view path,
                                         std::string& target) {
  const auto normalized = NamespacePath(path);
  if (!normalized) {
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  const auto it = LongestAlias(std::as_const(symlinks_), *normalized);
  if (it == symlinks_.cend() || !SymbolicLinkCurrent(it->second)) {
    return false;
  }
  target = it->second.target;
  return true;
}

X_STATUS VirtualFileSystem::TranslateSymbolicPath(std::string_view path,
                                                  std::string& output) {
  auto normalized = NamespacePath(path);
  if (!normalized) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  // Never retain an iterator/reference into a concurrently mutable namespace.
  // Keep the same namespace interval through binding validation. Only weak
  // device identities, strings and metadata tags are copied or destroyed here.
  auto namespace_lock = global_critical_region_.Acquire();
  std::vector<std::pair<std::string, SymbolicLink>> links;
  {
    auto global_lock = global_critical_region_.Acquire();
    links.assign(symlinks_.begin(), symlinks_.end());
  }
  std::vector<const SymbolicLink*> bindings;
  std::vector<std::string> visited;
  for (size_t depth = 0; depth <= 32; ++depth) {
    if (std::ranges::any_of(visited, [&](const auto& before) {
          return xe::utf8::equal_case(before, *normalized);
        })) {
      return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    visited.push_back(*normalized);
    const auto it = LongestAlias(std::as_const(links), *normalized);
    if (it == links.cend()) {
      for (const auto* bound : bindings) {
        if (!SymbolicLinkAllowsPath(*bound, *normalized)) {
          return HostStorageError(std::make_error_code(std::errc::no_such_device));
        }
      }
      output = std::move(*normalized);
      return X_STATUS_SUCCESS;
    }
    if (depth == 32) {
      return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (!SymbolicLinkCurrent(it->second)) {
      return HostStorageError(std::make_error_code(std::errc::no_such_device));
    }
    if (it->second.expected_device) { bindings.push_back(&it->second); }
    const auto suffix = normalized->substr(it->first.size());
    const auto next = suffix.empty()
                          ? it->second.target
                          : xe::utf8::join_guest_paths(it->second.target, suffix);
    if (next.size() > kMaximumTranslatedPath) {
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    normalized = NamespacePath(next);
    if (!normalized) {
      return X_STATUS_OBJECT_NAME_INVALID;
    }
  }
  return X_STATUS_OBJECT_PATH_NOT_FOUND;
}

namespace {

// A mount path matches only on a whole path component. Without this a device
// mounted at ...\Content also swallows ...\Content_Eng.
bool MountPathMatches(const std::string_view path,
                      const std::string_view mount_path) {
  if (mount_path.empty()) {
    return false;
  }
  const char last = mount_path.back();
  // Links name a \Device\Content\N\ root without the trailing separator.
  if (last == '\\' && path.size() + 1 == mount_path.size()) {
    return xe::utf8::equal_case(path, mount_path.substr(0, path.size()));
  }
  if (!xe::utf8::starts_with_case(path, mount_path)) {
    return false;
  }
  // Mounts ending in a delimiter already sit on a component boundary.
  if (last == '\\' || last == ':') {
    return true;
  }
  return path.size() == mount_path.size() || path[mount_path.size()] == '\\';
}

}  // namespace

std::shared_ptr<Device> VirtualFileSystem::AcquirePathDevice(std::string_view path) {
  std::shared_ptr<Device> selected;
  {
    // GCRC is recursive; TranslateSymbolicPath takes its bounded link copy
    // while this same transaction excludes alias/mount replacement.
    auto lock = global_critical_region_.Acquire();
    std::string translated;
    if (XFAILED(TranslateSymbolicPath(path,translated))) { return {}; }
    for (const auto& device : devices_) {
      if (MountPathMatches(translated,device->mount_path()) &&
          (!selected || device->mount_path().size()>selected->mount_path().size())) {
        selected=device;
      }
    }
    // A withdrawing most-specific mount is a denial, never permission to
    // reopen its parent's stale cached subtree under the same path.
    if(selected && selected->namespace_retiring())return {};
  }
  return selected;
}

Entry* VirtualFileSystem::ResolvePath(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  std::string normalized_path;
  if (XFAILED(TranslateSymbolicPath(path, normalized_path))) {
    return nullptr;
  }

  // Find the device with the longest matching mount path. Devices nest, e.g.
  // \Device\Harddisk0\Partition1\Content lives inside \Device\Harddisk0, so the
  // most specific mount must win regardless of registration order.
  Device* device = nullptr;
  for (const auto& d : devices_) {
    if (MountPathMatches(normalized_path, d->mount_path()) &&
        (!device || d->mount_path().size() > device->mount_path().size())) {
      device = d.get();
    }
  }
  if (device && device->namespace_retiring()) { return nullptr; }
  if (!device) {
    // Supress logging the error for ShaderDumpxe:\CompareBackEnds as this is
    // not an actual problem nor something we care about.
    if (path != "ShaderDumpxe:\\CompareBackEnds") {
      XELOGE("ResolvePath({}) failed - device not found", path);
    }
    return nullptr;
  }

  auto relative_path = normalized_path.substr(
      std::min(normalized_path.size(), device->mount_path().size()));
  return device->ResolvePath(relative_path);
}

X_STATUS VirtualFileSystem::RenameEntry(Entry* entry,
                                        const std::string_view target_path,
                                        bool replace_existing,
    std::shared_ptr<const filesystem::HostStorageFileLease> permitted) {
  // One lock, so the new folder can't go away before the entry moves into it.
  auto global_lock = global_critical_region_.Acquire();
  return entry->Rename(ResolvePath(xe::utf8::find_base_guest_path(target_path)),
                       xe::utf8::find_name_from_guest_path(target_path),
                       replace_existing, std::move(permitted));
}

Entry* VirtualFileSystem::ResolveMountRoot(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  std::string normalized_path;
  if (XFAILED(TranslateSymbolicPath(path, normalized_path))) {
    return nullptr;
  }
  for (const auto& d : devices_) {
    std::string_view mount_path = d->mount_path();
    if (!mount_path.empty() && mount_path.back() == '\\') {
      mount_path.remove_suffix(1);
    }
    if (xe::utf8::equal_case(normalized_path, mount_path)) {
      return d->namespace_retiring() ? nullptr : d->ResolvePath("");
    }
  }
  return nullptr;
}

Entry* VirtualFileSystem::CreatePath(const std::string_view path,
                                     uint32_t attributes) {
  auto path_parts = xe::utf8::split_path(path);
  if (path_parts.empty()) {
    return nullptr;
  }

  // A device mount spans several path components, not just the first.
  Entry* parent_entry = nullptr;
  size_t next_part = path_parts.size();
  while (next_part > 0) {
    const auto& part = path_parts[next_part - 1];
    const size_t prefix_length =
        static_cast<size_t>(part.data() + part.size() - path.data());
    parent_entry = ResolvePath(path.substr(0, prefix_length));
    if (parent_entry) {
      break;
    }
    --next_part;
  }
  if (!parent_entry) {
    return nullptr;
  }

  for (size_t i = next_part; i < path_parts.size(); ++i) {
    const bool is_last = i == path_parts.size() - 1;
    auto child_entry = parent_entry->CreateEntry(
        path_parts[i], is_last ? attributes : kFileAttributeDirectory);
    if (!child_entry) {
      return nullptr;
    }
    parent_entry = child_entry;
  }
  return parent_entry;
}

bool VirtualFileSystem::DeletePath(const std::string_view path) {
  auto entry = ResolvePath(path);
  if (!entry) {
    return false;
  }
  auto parent = entry->parent();
  if (!parent) {
    // Can't delete root.
    return false;
  }
  return parent->Delete(entry);
}

X_STATUS VirtualFileSystem::OpenFile(Entry* root_entry,
                                     const std::string_view requested_path,
                                     FileDisposition creation_disposition,
                                     uint32_t desired_access, bool is_directory,
                                     bool is_non_directory, File** out_file,
                                     FileAction* out_action,
                                     const FileOpenContext& open_context) {
  const auto& expected_source = open_context.expected_device;
  const bool caller_owns_namespace =
      xe::global_critical_region::is_held_by_current_thread();
  auto lifetime_lock = global_critical_region_.Acquire();
  // TODO(gibbed): should 'is_directory' remain as a bool or should it be
  // flipped to a generic FileAttributeFlags?

  if (root_entry && root_entry->device()->namespace_retiring()) {
    return HostStorageError(std::make_error_code(std::errc::no_such_device));
  }
  std::string redirected_path;
  std::string_view path=requested_path;
  if(root_entry && !path.empty()) {
    // RootDirectory is a retained Entry, but a relative path may cross a real
    // registered child mount. Resolve that same absolute guest path through
    // the common namespace instead of its parent's stale cached children.
    // Source constraints below still reject foreign or retiring descendants.
    const auto absolute=xe::utf8::join_guest_paths(root_entry->absolute_path(),path);
    std::string translated;
    const auto translated_status=TranslateSymbolicPath(absolute,translated);
    if(XFAILED(translated_status))return translated_status;
    Device* selected=nullptr;
    for(const auto& device:devices_)
      if(MountPathMatches(translated,device->mount_path()) &&
          (!selected || device->mount_path().size()>selected->mount_path().size()))selected=device.get();
    if(selected && selected->namespace_retiring())
      return HostStorageError(std::make_error_code(std::errc::no_such_device));
    if(selected && selected!=root_entry->device()) {
      redirected_path=absolute;path=redirected_path;root_entry=nullptr;
    }
  }

  // Cleanup access.
  if (desired_access & FileAccess::kGenericRead) {
    desired_access |= FileAccess::kFileReadData;
  }
  if (desired_access & FileAccess::kGenericWrite) {
    desired_access |= FileAccess::kFileWriteData;
  }
  if (desired_access & FileAccess::kGenericAll) {
    desired_access |= FileAccess::kFileReadData | FileAccess::kFileWriteData;
  }

  // Resolve exact registered folder roots and regular parent/child paths.
  Entry* parent_entry = nullptr;
  Entry* entry = nullptr;

  // An exact folder-backed mount may have no parent in the guest tree, or
  // its parent may be a different backend (the HDD parent is a NullDevice).
  // Do not require that parent to contain a placeholder for the mounted root.
  Entry* mount_root = !root_entry ? ResolveMountRoot(path) : nullptr;
  const bool host_mount_root =
      dynamic_cast<HostPathEntry*>(mount_root) != nullptr;
  auto base_path = xe::utf8::find_base_guest_path(path);
  if (!base_path.empty()) {
    parent_entry = !root_entry ? ResolvePath(base_path)
                               : root_entry->ResolvePath(base_path);
    if (!parent_entry && !host_mount_root) {
      *out_action = FileAction::kDoesNotExist;
      return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    auto file_name = xe::utf8::find_name_from_guest_path(path);
    entry = parent_entry ? parent_entry->GetChild(file_name) : nullptr;
    // An exact registered folder mount is authoritative, just as in
    // ResolvePath, regardless of its parent's backend or cached children.
    // A raw placeholder with no registered folder mount remains unchanged.
    if (mount_root &&
        (host_mount_root || dynamic_cast<HostPathEntry*>(parent_entry))) {
      entry = mount_root;
    }
  } else if (path.empty() && root_entry) {
    entry = root_entry;
  } else {
    entry = !root_entry ? ResolvePath(path) : root_entry->GetChild(path);
  }

  // Selected ordinary media never grants a weaker handle than requested.
  // Apply its exact immutable Device role BEFORE managed-file dispatch and
  // the old readonly-disc/package write-to-read compatibility downgrade.
  const auto* requested_device=entry?entry->device():
      (parent_entry?parent_entry->device():nullptr);
  if (expected_source) {
    // The operation captured this exact registered source BEFORE releasing its
    // caller/worker boundary for blocking IO. A new mount at the same string,
    // an unrelated nested device, or a detached intermediate ancestor cannot
    // borrow it. All checks precede managed dispatch and filesystem mutation.
    const bool registered = std::any_of(devices_.begin(), devices_.end(),
        [&](const auto& device) {
          return device == expected_source &&
              !device.owner_before(expected_source) &&
              !expected_source.owner_before(device);
        });
    bool descendant = false;
    for (auto* node = requested_device; node; node = node->mount_parent().get()) {
      if (node == expected_source.get()) { descendant = true; break; }
    }
    if (!registered || expected_source->namespace_retiring() ||
        !requested_device || requested_device->namespace_retiring() || !descendant) {
      *out_action = FileAction::kDoesNotExist;
      return HostStorageError(std::make_error_code(std::errc::no_such_device));
    }
    // Native source-bound creation must report genuine read-only failure. The
    // older generic disc/package downgrade below cannot weaken this request.
    const bool mutation = creation_disposition != FileDisposition::kOpen &&
        (creation_disposition != FileDisposition::kOpenIf || !entry);
    constexpr uint32_t read = FileAccess::kGenericRead | FileAccess::kGenericExecute |
        FileAccess::kFileReadData | 0x8u | 0x20u | 0x80u | 0x20000u | 0x100000u;
    if (requested_device->is_read_only() && (mutation || (desired_access & ~read))) {
      *out_action = FileAction::kDoesNotExist;
      return X_STATUS_ACCESS_DENIED;
    }
  }
  if(requested_device && requested_device->requires_strict_read_only_open()) {
    constexpr uint32_t read=FileAccess::kGenericRead|FileAccess::kGenericExecute|
        FileAccess::kFileReadData|0x8u|0x20u|0x80u|0x20000u|0x100000u;
    if((desired_access&~read) ||
        (creation_disposition!=FileDisposition::kOpen &&
         (creation_disposition!=FileDisposition::kOpenIf || !entry))) {
      *out_action=FileAction::kDoesNotExist;return X_STATUS_ACCESS_DENIED;
    }
  }

  // Protected managed leaves are resolved BEFORE exists checks, supersede,
  // overwrite or creation. Preserve the actual caller device, then release
  // namespace ownership before authentication, staging, fsync or callbacks.
  std::filesystem::path managed_path;
  std::shared_ptr<Device> managed_device;
  std::string managed_guest_path;
  if (auto* host = dynamic_cast<HostPathEntry*>(entry)) {
    managed_path = host->host_path();
    managed_guest_path = host->path();
    managed_device = host->device()->weak_from_this().lock();
  } else if (!entry) {
    if (auto* host = dynamic_cast<HostPathEntry*>(parent_entry)) {
      managed_path = host->host_path() /
          xe::to_path(xe::utf8::find_name_from_guest_path(path));
      managed_guest_path = xe::utf8::join_guest_paths(host->path(),
          xe::utf8::find_name_from_guest_path(path));
      managed_device = host->device()->weak_from_this().lock();
    }
  }
  if (!managed_path.empty()) {
    // Caller-owned GCRC cannot be released here. Refuse using metadata-only
    // denial before weak.lock can acquire the last service/native-root owner.
    // Register shares this same GCRC, so a negative fence cannot turn positive
    // between this check and lookup while our namespace owner remains held.
    if (caller_owns_namespace && managed_device &&
        managed_device->ProtectsManagedFile(managed_path)) {
      return HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));
    }
    auto service = managed_device ? managed_device->ManagedFile(managed_path) : nullptr;
    if (service) {
      if (caller_owns_namespace) {
        return HostStorageError(
            std::make_error_code(std::errc::device_or_resource_busy));
      }
      lifetime_lock.unlock();
      return service->OpenWithContext(std::move(managed_device), std::move(managed_guest_path),
          creation_disposition, desired_access, is_directory, out_file, out_action,
          open_context);
    }
  }

  // An issued native managed view must be consumed by its existing exact-leaf
  // policy. An unknown/missing/retired policy cannot turn that capability into
  // a raw pathname open, even for an otherwise writable device.
  if (open_context.managed_authority) {
    if (out_file) *out_file = nullptr;
    if (out_action) *out_action = FileAction::kDoesNotExist;
    return X_STATUS_ACCESS_DENIED;
  }

  // A selected root cannot be replaced. Check alias roots too: a bare alias
  // has no base path and therefore never enters the parent lookup above.
  const bool protected_root =
      entry && ((path.empty() && entry == root_entry) ||
                (entry == mount_root &&
                 (host_mount_root ||
                  dynamic_cast<HostPathEntry*>(parent_entry))));
  if (protected_root &&
      (creation_disposition == FileDisposition::kSuperscede ||
       creation_disposition == FileDisposition::kOverwrite ||
       creation_disposition == FileDisposition::kOverwriteIf)) {
    *out_action = FileAction::kDoesNotExist;
    return X_STATUS_ACCESS_DENIED;
  }
  // A read-only mounted root must not silently acquire a weaker handle than
  // requested. Keep the legacy compatibility policy for non-root entries.
  if (protected_root && entry->is_read_only() &&
      (desired_access & (FileAccess::kFileWriteData |
                         FileAccess::kFileAppendData |
                         FileAccess::kFileWriteAttributes))) {
    *out_action = FileAction::kDoesNotExist;
    return X_STATUS_ACCESS_DENIED;
  }

  if (entry) {
    if (entry->attributes() & kFileAttributeDirectory && is_non_directory) {
      return X_STATUS_FILE_IS_A_DIRECTORY;
    }

    // If the entry does not exist on the host then remove the cached entry. A
    // device mounted there has its own folder.
    if (parent_entry && entry->device() == parent_entry->device()) {
      const auto* host_path = dynamic_cast<const xe::vfs::HostPathEntry*>(entry);

      if (host_path) {
        // A logical HDD child may retain an installed read-only source. The
        // primary parent's host path is not that child's physical backing.
        auto const& file_path = host_path->host_path();

        if (!std::filesystem::exists(file_path)) {
          // Remove cached entry
          entry->Delete();
          entry = nullptr;
        }
      }
    }
  }

  // Check if exists (if we need it to), or that it doesn't (if it shouldn't).
  switch (creation_disposition) {
    case FileDisposition::kOpen:
    case FileDisposition::kOverwrite:
      // Must exist.
      if (!entry) {
        *out_action = FileAction::kDoesNotExist;
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
      }
      break;
    case FileDisposition::kCreate:
      // Must not exist.
      if (entry) {
        *out_action = FileAction::kExists;
        return X_STATUS_OBJECT_NAME_COLLISION;
      }
      break;
    default:
      // Either way, ok.
      break;
  }

  // Verify permissions.
  bool wants_write = desired_access & FileAccess::kFileWriteData ||
                     desired_access & FileAccess::kFileAppendData;
  if (wants_write && ((parent_entry && parent_entry->is_read_only()) ||
                      (entry && entry->is_read_only()))) {
    // Fail if read only device and wants write.
    // return X_STATUS_ACCESS_DENIED;
    // TODO(benvanik): figure out why games are opening read-only files with
    // write modes.
    XELOGW("Attempted to open read-only file/dir for write: {}", path);
    desired_access = FileAccess::kGenericRead | FileAccess::kFileReadData;
  }

  bool created = false;
  if (!entry) {
    // Remember that we are creating this new, instead of replacing.
    created = true;
    *out_action = FileAction::kCreated;
  } else {
    // May need to delete, if it exists.
    switch (creation_disposition) {
      case FileDisposition::kCreate:
        // Shouldn't be possible to hit this.
        assert_always();
        return X_STATUS_ACCESS_DENIED;
      case FileDisposition::kSuperscede:
        // Replace (by delete + recreate).
        if (!entry->Delete()) {
          return X_STATUS_ACCESS_DENIED;
        }
        entry = nullptr;
        *out_action = FileAction::kSuperseded;
        break;
      case FileDisposition::kOpen:
      case FileDisposition::kOpenIf:
        // Normal open.
        *out_action = FileAction::kOpened;
        break;
      case FileDisposition::kOverwrite:
      case FileDisposition::kOverwriteIf:
        // A supported in-place failure must preserve the original. Only an
        // explicit unsupported capability permits the legacy recreate path.
        {
          const auto overwrite = entry->OverwriteInPlace();
          if (overwrite.supported) {
            if (overwrite.status != X_STATUS_SUCCESS) {
              *out_action = FileAction::kExists;
              return XFAILED(overwrite.status)
                         ? overwrite.status : X_STATUS_INVALID_DEVICE_REQUEST;
            }
          } else {
            if (overwrite.status != X_STATUS_NOT_SUPPORTED) {
              return X_STATUS_INVALID_DEVICE_REQUEST;
            }
            if (!entry->Delete()) {
              return X_STATUS_ACCESS_DENIED;
            }
            entry = nullptr;
          }
        }
        *out_action = FileAction::kOverwritten;
        break;
    }
  }
  if (!entry) {
    // Create if needed (either new or as a replacement).
    entry = CreatePath(
        path, !is_directory ? kFileAttributeNormal : kFileAttributeDirectory);
    if (!entry) {
      return X_STATUS_ACCESS_DENIED;
    }
  }

  // Open.
  auto result = entry->Open(desired_access, out_file);
  if (XFAILED(result)) {
    *out_action = FileAction::kDoesNotExist;
  }
  return result;
}

X_STATUS VirtualFileSystem::ExtractContentFile(
    Entry* entry, std::filesystem::path base_path,
    std::atomic<uint64_t>& progress) {
  // Allocate a buffer when needed.
  size_t buffer_size = 0;
  uint8_t* buffer = nullptr;

  XELOGI("Extracting file: {}", entry->path());

  auto dest_name =
      base_path / xe::to_path(utf8::fix_path_separators(entry->path()));

  if (entry->attributes() & kFileAttributeDirectory) {
    std::error_code error_code;
    std::filesystem::create_directories(dest_name, error_code);
    if (error_code) {
      return error_code.value();
    }
    return 0;
  }

  vfs::File* in_file = nullptr;
  X_STATUS result = entry->Open(FileAccess::kFileReadData, &in_file);
  if (result != X_STATUS_SUCCESS) {
    return result;
  }

  auto file = xe::filesystem::OpenFile(dest_name, "wb");
  if (!file) {
    in_file->Destroy();
    return 1;
  }
  constexpr size_t write_buffer_size = 4_MiB;

  if (entry->can_map()) {
    auto map = entry->OpenMapped(xe::MappedMemory::Mode::kRead);

    size_t remaining_size = map->size();
    size_t offset = 0;

    while (remaining_size > 0) {
      const auto bytes_to_read = std::min(write_buffer_size, remaining_size);
      fwrite(map->data() + offset, bytes_to_read, 1, file);
      offset += bytes_to_read;
      remaining_size -= bytes_to_read;
      progress += bytes_to_read;
    }
    map->Close();
  } else {
    size_t remaining_size = entry->size();
    size_t offset = 0;
    buffer = new uint8_t[write_buffer_size];

    while (remaining_size > 0) {
      size_t bytes_read = 0;
      in_file->ReadSync(std::span<uint8_t>(buffer, write_buffer_size), offset,
                        &bytes_read);
      fwrite(buffer, bytes_read, 1, file);
      offset += bytes_read;
      remaining_size -= bytes_read;
      progress += bytes_read;
    }
  }

  fclose(file);
  in_file->Destroy();

  if (buffer) {
    delete[] buffer;
  }
  return 0;
}

X_STATUS VirtualFileSystem::ExtractContentFiles(
    Device* device, std::filesystem::path base_path,
    std::atomic<uint64_t>& progress, std::function<bool()> should_cancel) {
  // Run through all the files, breadth-first style.
  std::queue<vfs::Entry*> queue;
  auto root = device->ResolvePath("/");
  queue.push(root);

  while (!queue.empty()) {
    // Check for cancellation before processing each file
    if (should_cancel && should_cancel()) {
      return X_ERROR_CANCELLED;
    }

    auto entry = queue.front();
    queue.pop();
    for (auto& entry : entry->children()) {
      queue.push(entry.get());
    }

    ExtractContentFile(entry, base_path, progress);
  }
  return X_STATUS_SUCCESS;
}

void VirtualFileSystem::ExtractContentHeader(Device* device,
                                             std::filesystem::path base_path) {
  const XContentContainerDevice* xcontent_device =
      ((XContentContainerDevice*)device);

  if (!std::filesystem::exists(base_path.parent_path())) {
    if (!std::filesystem::create_directories(base_path.parent_path())) {
      return;
    }
  }
  auto header_filename = base_path.filename().concat(".header");
  auto header_path = base_path.parent_path() / header_filename;
  xe::filesystem::CreateEmptyFile(header_path);

  if (std::filesystem::exists(header_path)) {
    auto file = xe::filesystem::OpenFile(header_path, "wb");
    kernel::xam::XCONTENT_AGGREGATE_DATA data =
        xcontent_device->content_header();
    uint32_t license_mask = xcontent_device->license_mask();

    data.set_file_name(xe::path_to_utf8(base_path.filename()));
    fwrite(&data, 1, sizeof(kernel::xam::XCONTENT_AGGREGATE_DATA), file);
    fwrite(&license_mask, 1, sizeof(license_mask), file);
    fclose(file);
  }
  return;
}
}  // namespace vfs
}  // namespace xe
