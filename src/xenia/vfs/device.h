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
#include "xenia/base/mutex.h"
#include "xenia/base/string_buffer.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/managed_file_service.h"
#include <atomic>

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
  bool AdmitGuestStorage(std::error_code& error) {
    guest_storage_admission_ =
        filesystem::HostStorageRoot::AdmitGuestMount(GuestBackings(), error);
    return bool(guest_storage_admission_);
  }

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
  virtual bool has_open_files() const { return false; }
  // Retiring a mount view must never retire its parent source capability.
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
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICE_H_
