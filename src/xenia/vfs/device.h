/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICE_H_
#define XENIA_VFS_DEVICE_H_

#include <memory>
#include <mutex>
#include <string>

#include "xenia/base/host_storage.h"
#include "xenia/base/console_storage_volume.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string_buffer.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/managed_file_service.h"
#include <atomic>

namespace xe::kernel::xam { class ContentManager; class ContentSourceRegistry; }
namespace xe::kernel { class XStfsDevice; }

namespace xe {
namespace vfs {

// Models the time storage needs to service a request, since a completion that
// lands sooner than hardware could deliver it breaks some titles' loaders.
// A request costs its transfer, plus a seek unless it continues the last one.
class DriveTiming {
 public:
  // Enables the model, which is inert on a device that never calls this.
  void Configure();

  // Returns the host uptime millisecond it completes at, or 0 when untimed.
  uint64_t Reserve(const Entry* entry, uint64_t offset, size_t length);

 private:
  bool enabled_ = false;
  std::mutex lock_;
  // When the medium frees up, fractional so short transfers accumulate.
  double free_at_ms_ = 0.0;
  // Where the previous request ended.
  const Entry* head_entry_ = nullptr;
  uint64_t head_offset_ = 0;
};

class Device : public std::enable_shared_from_this<Device> {
  // Exact immutable parent, released after this child's backing/admission.
  // Active descendants are enumerated only in VirtualFileSystem::devices_.
  std::shared_ptr<Device> mount_parent_;
  // Released after derived and base native admission/backing members.
  // This is independent owned data, never a raw ContentManager callback.
  std::shared_ptr<const void> mount_resource_;
 public:
  explicit Device(const std::string_view mount_path);
  virtual ~Device();

  virtual bool Initialize() = 0;
  // Explicit host provenance for common admission; nullopt is unknown, an
  // empty list is genuinely host-free. Archive children inherit their actual
  // retained package device's backing declaration, never a guessed path.
  virtual std::optional<std::vector<filesystem::GuestHostBacking>>
  GuestBackings() const {
    return std::nullopt;
  }
  // Actual already-owned opened reader identity, never a pathname inference.
  virtual std::shared_ptr<Device> RetainedBackingParent() const { return {}; }
  bool AdmitGuestStorage(std::error_code& error,
      filesystem::HostStorageFailureObservation* diagnostic = nullptr,
      filesystem::HostStoragePreparationWait* preparation = nullptr) {
    if (!mount_parent_) {
      const auto parent = RetainedBackingParent();
      if (parent && !BindMountParent(parent)) {
        error = std::make_error_code(std::errc::no_such_device); return false;
      }
    }
    FreezeStorageVolume();
    guest_storage_admission_ =
        filesystem::HostStorageRoot::AdmitGuestMount(GuestBackings(), error, diagnostic, preparation);
    return bool(guest_storage_admission_);
  }

  // Set once while this device is uniquely owned, before common admission.
  // Volume metadata is not filesystem access authority or a deployment value.
  bool SetStorageVolume(std::shared_ptr<const filesystem::ConsoleStorageVolume> volume) {
    std::lock_guard lock(storage_volume_mutex_);
    if (!volume || !volume->available_cached() || storage_volume_ ||
        storage_volume_frozen_ || guest_storage_admission_) { return false; }
    storage_volume_ = std::move(volume); return true;
  }
  void FreezeStorageVolume() {
    std::lock_guard lock(storage_volume_mutex_); storage_volume_frozen_ = true;
  }
  std::shared_ptr<const filesystem::ConsoleStorageVolume> storage_volume() const {
    std::lock_guard lock(storage_volume_mutex_); return storage_volume_;
  }
  const std::shared_ptr<Device>& mount_parent() const { return mount_parent_; }
  const std::string& mount_path() const { return mount_path_; }
  void SetManagedFileRegistry(std::shared_ptr<ManagedFileRegistry> registry) {
    managed_files_ = std::move(registry);
  }
  std::shared_ptr<ManagedFileService> ManagedFile(
      const std::filesystem::path& path) const {
    return managed_files_ ? managed_files_->Find(path,ManagedDirectoryIdentity()) : nullptr;
  }
  bool ProtectsManagedFile(const std::filesystem::path& path,
                            bool subtree = false) const {
    return managed_files_ && managed_files_->Protects(path, subtree,ManagedDirectoryIdentity());
  }
  virtual std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  ManagedDirectoryIdentity() const { return {}; }
  void RetainManagedFile() { managed_file_handles_.fetch_add(1); }
  void ReleaseManagedFile() {
    if (managed_file_handles_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      OnManagedFileRundown();
    }
  }
  bool has_managed_files() const { return managed_file_handles_.load() != 0; }

  virtual bool is_read_only() const { return true; }
  // Only explicitly selected ordinary media refuses write-open requests
  // instead of the legacy disc/package compatibility downgrade. Immutable
  // child edges preserve the same selected-source restriction.
  bool requires_strict_read_only_open() const {
    for(const Device* node=this;node;node=node->mount_parent_.get())
      if(node->selected_media_read_only_)return true;
    return false;
  }
  virtual bool has_open_files() const { return false; }
  // Retiring a mount view must never retire its parent source capability.
  // Namespace admission is metadata only. The actual backing retirement and
  // retained resource destruction run after the caller releases GCRC.
  // Denial/observation only: distinguishes an actual graph namespace closure
  // from a separately withdrawn logical source descriptor. No IO authority.
  bool namespace_closing_cached() const {
    for(const Device* node=this;node;node=node->mount_parent_.get())
      if(node->namespace_retiring_.load(std::memory_order_acquire))return true;
    return false;
  }
  bool namespace_retiring() const {
    // Lifetime edges are immutable and acyclic after registration. A raw
    // source withdrawal also blocks descendants immediately, even before a
    // coordinated closure performs their actual backend rundown.
    for (const Device* node = this; node; node = node->mount_parent_.get()) {
      if (node->namespace_retiring_.load(std::memory_order_acquire)) { return true; }
      const auto volume = node->storage_volume();
      if (volume && !volume->available_cached()) { return true; }
    }
    return false;
  }
  std::stop_token namespace_retirement_token() const { return namespace_stop_.get_token(); }
  void BeginNamespaceRetirement() {
    namespace_retiring_.store(true, std::memory_order_release);
    namespace_stop_.request_stop();
    // Same once-bound selected source ingress, not a callback or another
    // registry. Child closure does not retire the parent physical source.
    if(selected_media_ingress_)selected_media_ingress_->store(false,std::memory_order_release);
  }
  virtual void Retire() {}
  virtual filesystem::HostStorageSnapshot storage_space() const {
    filesystem::HostStorageSnapshot result;
    result.available = true;
    result.read_only = is_read_only();
    const uint64_t unit =
        uint64_t(sectors_per_allocation_unit()) * bytes_per_sector();
    result.capacity_bytes = uint64_t(total_allocation_units()) * unit;
    result.available_bytes = uint64_t(available_allocation_units()) * unit;
    result.usable_bytes = result.read_only ? 0 : result.available_bytes;
    return result;
  }

  // True when two of this device's files can be read at the same time. False
  // for a shared file cursor, or a reader that serializes internally.
  virtual bool supports_concurrent_io() const { return false; }

  // True when the file system queues every request, which then always pends on
  // an async handle. FATX and GDFX complete a buffered transfer inline.
  virtual bool queues_all_requests() const { return false; }

  virtual void Dump(StringBuffer* string_buffer) = 0;
  virtual Entry* ResolvePath(const std::string_view path) = 0;

  virtual const std::string& name() const = 0;
  virtual uint32_t attributes() const = 0;
  virtual uint32_t component_name_max_length() const = 0;

  virtual uint32_t total_allocation_units() const = 0;
  virtual uint32_t available_allocation_units() const = 0;
  virtual uint32_t sectors_per_allocation_unit() const = 0;
  virtual uint32_t bytes_per_sector() const = 0;

  // Request timing for this device, configured where the device is mounted.
  DriveTiming& drive_timing() { return drive_timing_; }

 protected:
  // Metadata-only retirement hook after the LAST actual managed File/write
  // owner released. This never runs a guest/provider callback or observing IO.
  virtual void OnManagedFileRundown() {}
  xe::global_critical_region global_critical_region_;
  std::string mount_path_;
  std::shared_ptr<ManagedFileRegistry> managed_files_;
  std::atomic<size_t> managed_file_handles_{0};
  std::shared_ptr<const filesystem::HostStorageGuestMount>
      guest_storage_admission_;
  DriveTiming drive_timing_;
  mutable std::mutex storage_volume_mutex_;
  std::shared_ptr<const filesystem::ConsoleStorageVolume> storage_volume_;
  bool storage_volume_frozen_ = false;
 private:
  friend class VirtualFileSystem;
  friend class xe::kernel::xam::ContentSourceRegistry;
  bool BindSelectedMediaReadOnly(std::shared_ptr<std::atomic<bool>> ingress) {
    if(!ingress || selected_media_read_only_ || storage_volume_frozen_ ||
        guest_storage_admission_ || !is_read_only())return false;
    selected_media_ingress_=std::move(ingress);selected_media_read_only_=true;return true;
  }
  bool selected_media_read_only_=false;
  std::shared_ptr<std::atomic<bool>> selected_media_ingress_;
  // Only an active ephemeral common graph batch owns this metadata claim.
  // Busy return expires it; no persistent tracker or Device->Kernel callback.
  std::weak_ptr<const uint8_t> retirement_owner_;
  bool BindMountParent(const std::shared_ptr<Device>& parent,
      const std::shared_ptr<const filesystem::ConsoleStorageVolume>& selected = {}) {
    if (!parent) { return true; }
    const auto volume = parent->storage_volume();
    std::lock_guard lock(storage_volume_mutex_);
    if (mount_parent_ == parent) { return true; }
    if (parent.get() == this || mount_parent_ || storage_volume_frozen_ ||
        guest_storage_admission_ || parent->namespace_retiring() ||
        (volume && !volume->available_cached()) ||
        (storage_volume_ && storage_volume_ != volume && storage_volume_ != selected)) { return false; }
    mount_parent_ = parent;
    if (!selected && volume) { storage_volume_ = volume; }
    return true;
  }
  friend class xe::kernel::xam::ContentManager;
  friend class xe::kernel::XStfsDevice;
  bool RetainMountResource(std::shared_ptr<const void> resource) {
    if (!resource || mount_resource_) { return false; }
    mount_resource_ = std::move(resource);
    return true;
  }
  std::atomic<bool> namespace_retiring_{false};
  std::stop_source namespace_stop_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICE_H_
