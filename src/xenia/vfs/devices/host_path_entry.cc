/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_entry.h"

#include <algorithm>
#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_file.h"
#include "xenia/vfs/host_storage_errors.h"

namespace xe {
namespace vfs {

namespace {
std::filesystem::path RelativeStoragePath(const std::filesystem::path& path,
                                        const std::filesystem::path& root) {
  auto relative = path.lexically_relative(root);
  // The capability spells its own root as an empty relative path. The C++
  // filesystem returns "." for equal paths, including native volume opens.
  if (relative == ".") relative.clear();
  return relative;
}
}  // namespace

HostPathEntry::HostPathEntry(Device* device, Entry* parent,
                             const std::string_view path,
                             const std::filesystem::path& host_path)
    : Entry(device, parent, path), host_path_(host_path) {}

HostPathEntry::~HostPathEntry() {
  if (overwrite_handle_ && !overwrite_opened_) {
    AbortOverwrite();
  }
}

bool HostPathEntry::has_open_files() const {
  auto global_lock = global_critical_region_.Acquire();
  // Deleting a directory frees its children, so their handles count too.
  return open_files_.load() != 0 ||
         std::ranges::any_of(children_, [](const auto& child) {
           return child->has_open_files();
         });
}

void HostPathEntry::ReleaseFile() {
  auto global_lock = global_critical_region_.Acquire();
  if (open_files_.fetch_sub(1) != 1) {
    return;
  }
  if (delete_on_close() && parent()) {
    Delete();
  }
#if XE_PLATFORM_WIN32
  else if (overwritten_) {
    // The old bytes past the size go with the last handle. A host file
    // already that size is left alone so its times stay.
    overwritten_ = false;
    auto file_info = xe::filesystem::GetInfo(host_path_);
    if (file_info && file_info->total_size != size_) {
      std::error_code ec;
      std::filesystem::resize_file(host_path_, size_, ec);
      if (ec) {
        XELOGE("{}: Cannot trim {}: {}", __func__,
               xe::strip_pii(xe::path_to_utf8(host_path_)), ec.message());
      }
    }
  }
#endif
}

bool HostPathEntry::Overwrite() {
  const auto result = OverwriteInPlace();
  return result.supported && XSUCCEEDED(result.status);
}
InPlaceOverwriteResult HostPathEntry::OverwriteInPlace() {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only() || device_->ProtectsManagedFile(host_path_)) {
    return {true, X_STATUS_ACCESS_DENIED};
  }
  if (attributes_ & kFileAttributeDirectory) {
    return {true, X_STATUS_FILE_IS_A_DIRECTORY};
  }
  if (reported_size_fixed_) {
    return {};  // This synthesized view has no in-place overwrite capability.
  }
  if (has_open_files()) {
    return {true, HostStorageError(
                      std::make_error_code(std::errc::device_or_resource_busy))};
  }
#if !XE_PLATFORM_WIN32
  if (const auto error = RetryRetainedOverwrite()) {
    return {true, HostStorageError(error)};
  }
  if (overwritten_ || overwrite_handle_ || overwrite_lease_) {
    return {true, HostStorageError(
                      std::make_error_code(std::errc::device_or_resource_busy))};
  }
  const auto& storage = static_cast<HostPathDevice*>(device_)->storage();
  std::error_code error;
  auto reservation = storage->ReserveOverwrite(
      RelativeStoragePath(host_path_, storage->path()), error);
  if (!reservation) {
    return {true, HostStorageError(
                      error ? error : std::make_error_code(std::errc::io_error))};
  }
  const auto info = reservation->QueryInfo(error);
  if (!info) {
    return {true, HostStorageError(
                      error ? error : std::make_error_code(std::errc::io_error))};
  }
  if (!info->regular) {
    return {true, X_STATUS_INVALID_DEVICE_REQUEST};
  }
  if (info->size > SIZE_MAX) {
    return {true, X_STATUS_INVALID_PARAMETER};
  }
  overwrite_filesystem_id_ = info->filesystem_id;
  overwrite_file_id_ = info->file_id;
  overwrite_original_size_ = size_t(info->size);
  overwrite_original_allocation_ =
      xe::round_up(overwrite_original_size_, device()->bytes_per_sector());
  overwrite_handle_ = std::move(reservation);
  overwrite_lease_ = overwrite_handle_->storage_lease();
  overwrite_opened_ = false;
#else
  if (overwritten_) {
    return {true, X_STATUS_ACCESS_DENIED};
  }
#endif
  overwrite_metadata_error_.clear();
  overwrite_tail_retained_ = true;
  overwrite_durability_pending_ = true;
  overwritten_ = true;
  size_ = 0;
  allocation_size_ = 0;
  return {true, X_STATUS_SUCCESS};
}
void HostPathEntry::AbortOverwrite() {
  if (!overwrite_handle_ || overwrite_opened_) {
    return;
  }
  size_ = overwrite_original_size_;
  allocation_size_ = overwrite_original_allocation_;
  overwrite_metadata_error_.clear();
  overwrite_filesystem_id_ = 0;
  overwrite_file_id_ = 0;
  overwrite_tail_retained_ = false;
  overwrite_durability_pending_ = false;
  overwrite_lease_.reset();
  overwrite_handle_.reset();
  overwritten_ = false;
}
std::error_code HostPathEntry::ReconcileOverwriteEof(
    filesystem::FileHandle& file) {
  if (!overwrite_metadata_error_) {
    return {};
  }
  if (!overwrite_opened_ || !overwrite_lease_ ||
      file.storage_lease() != overwrite_lease_) {
    overwrite_metadata_error_ =
        std::make_error_code(std::errc::no_such_device);
    overwrite_durability_pending_ = true;
    return overwrite_metadata_error_;
  }
  std::error_code observed_error;
  const auto observed = file.QueryInfo(observed_error);
  if (!observed) {
    overwrite_metadata_error_ = observed_error
                                   ? observed_error
                                   : std::make_error_code(std::errc::io_error);
  } else if (!observed->regular || observed->links != 1 ||
             observed->filesystem_id != overwrite_filesystem_id_ ||
             observed->file_id != overwrite_file_id_) {
    overwrite_metadata_error_ =
        std::make_error_code(std::errc::no_such_device);
  } else if (observed->size > SIZE_MAX) {
    overwrite_metadata_error_ =
        std::make_error_code(std::errc::value_too_large);
  } else {
    SetOverwrittenSize(size_t(observed->size));
    overwrite_metadata_error_.clear();
  }
  if (overwrite_metadata_error_) {
    overwrite_durability_pending_ = true;
  }
  return overwrite_metadata_error_;
}
std::error_code HostPathEntry::CompleteOverwrite(filesystem::FileHandle& file) {
  if (!overwrite_opened_ || !overwrite_lease_ ||
      file.storage_lease() != overwrite_lease_) {
    return std::make_error_code(std::errc::operation_not_supported);
  }
  // A prior physical EOF failure may have published an unknown length.
  // Observe this same owned inode before fsync can acknowledge a coherent
  // commit. No old logical EOF is used to trim or roll back that state.
  if (const auto error = ReconcileOverwriteEof(file)) {
    return error;
  }
  std::error_code error;
  if (overwrite_tail_retained_) {
    const size_t logical_length = size_;
    const auto result = file.FlushLengthResult(logical_length);
    if (result.published_length &&
        *result.published_length == logical_length) {
      overwrite_tail_retained_ = false;
    }
    error = result.error;
    if (!error && (!result.published_length ||
                   *result.published_length != logical_length ||
                   !result.durable)) {
      error = std::make_error_code(std::errc::io_error);
    }
  } else {
    // Physical EOF already follows the owned logical EOF. Retry durability
    // without trimming to any prior committed length.
    error = file.FlushResult();
  }
  overwrite_durability_pending_ = bool(error);
  return error;
}
void HostPathEntry::ReleaseOverwriteOwnership() {
  overwrite_metadata_error_.clear();
  overwrite_filesystem_id_ = 0;
  overwrite_file_id_ = 0;
  overwrite_tail_retained_ = false;
  overwrite_durability_pending_ = false;
  overwrite_opened_ = false;
  overwrite_lease_.reset();
  overwritten_ = false;
}
std::error_code HostPathEntry::RetryRetainedOverwrite() {
  if (!overwrite_handle_ || !overwrite_opened_) {
    return {};
  }
  const auto error = CompleteOverwrite(*overwrite_handle_);
  if (!error) {
    ReleaseOverwriteOwnership();
    overwrite_handle_.reset();
  }
  return error;
}
void HostPathEntry::FinishOverwrite(
    std::unique_ptr<filesystem::FileHandle>& file) {
#if !XE_PLATFORM_WIN32
  auto global_lock = global_critical_region_.Acquire();
  if (!overwrite_opened_ || !overwrite_lease_ || !file ||
      file->storage_lease() != overwrite_lease_) {
    return;
  }
  const auto error = (overwrite_tail_retained_ || overwrite_durability_pending_ ||
                      overwrite_metadata_error_)
                         ? CompleteOverwrite(*file) : std::error_code{};
  if (error) {
    overwrite_handle_ = std::move(file);
    XELOGE("Overwrite completion retained for retry: {}", error.message());
  } else {
    ReleaseOverwriteOwnership();
  }
#endif
}
void HostPathEntry::SetOverwrittenSize(size_t size) {
  size_ = size;
  allocation_size_ = xe::round_up(size, device()->bytes_per_sector());
}

HostPathEntry* HostPathEntry::Create(Device* device, Entry* parent,
                                     const std::filesystem::path& full_path,
                                     xe::filesystem::FileInfo file_info) {
  auto path = xe::utf8::join_guest_paths(parent->path(),
                                         xe::path_to_utf8(file_info.name));
  auto entry = new HostPathEntry(device, parent, path, full_path);

  entry->create_timestamp_ = file_info.create_timestamp;
  entry->access_timestamp_ = file_info.access_timestamp;
  entry->write_timestamp_ = file_info.write_timestamp;
  if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
    entry->attributes_ = kFileAttributeDirectory;
  } else {
    entry->attributes_ = kFileAttributeNormal;
    if (device->is_read_only()) {
      entry->attributes_ |= kFileAttributeReadOnly;
    }
    entry->size_ = file_info.total_size;
    entry->allocation_size_ =
        xe::round_up(file_info.total_size, device->bytes_per_sector());
  }
  return entry;
}

X_STATUS HostPathEntry::Open(uint32_t desired_access, File** out_file) {
  if (device_->ProtectsManagedFile(host_path_)) { return X_STATUS_ACCESS_DENIED; }
  auto global_lock = global_critical_region_.Acquire();
  if (!out_file) {
    AbortOverwrite();
    return X_STATUS_INVALID_PARAMETER;
  }
  if (is_read_only() && (desired_access & (FileAccess::kFileWriteData |
                                           FileAccess::kFileAppendData))) {
    XELOGE("Attempting to open file for write access on read-only device");
    AbortOverwrite();
    return X_STATUS_ACCESS_DENIED;
  }
  // FATX sets times through any handle, even one opened only to read.
  uint32_t host_access = desired_access & ~FileAccess::kFileWriteAttributes;
  if (!is_read_only()) {
    host_access |= FileAccess::kFileWriteAttributes;
  }
  std::error_code error;
#if XE_PLATFORM_WIN32
  auto file_handle =
      xe::filesystem::FileHandle::OpenExisting(host_path_, host_access);
#else
  const auto& storage = static_cast<HostPathDevice*>(device_)->storage();
  error = RetryRetainedOverwrite();
  if (error) {
    return HostStorageError(error);
  }
  std::unique_ptr<filesystem::FileHandle> file_handle;
  if (overwrite_handle_) {
    // Validate liveness before consuming the reservation. Failed following Open
    // has not changed host bytes and rolls back the speculative logical size.
    if (!overwrite_handle_->QueryInfo(error)) {
      AbortOverwrite();
      return HostStorageError(error);
    }
    file_handle = std::move(overwrite_handle_);
    overwrite_opened_ = true;
  } else {
    file_handle = storage->OpenFile(
        RelativeStoragePath(host_path_, storage->path()), host_access, error);
  }
#endif
  if (!file_handle) {
    AbortOverwrite();
    return error ? HostStorageError(error) : X_STATUS_NO_SUCH_FILE;
  }
  *out_file = new HostPathFile(desired_access, this, std::move(file_handle));
  return X_STATUS_SUCCESS;
}

std::unique_ptr<MappedMemory> HostPathEntry::OpenMapped(MappedMemory::Mode mode,
                                                        size_t offset,
                                                        size_t length) {
  if (device_->ProtectsManagedFile(host_path_)) { return nullptr; }
  if (mode != MappedMemory::Mode::kRead && is_read_only()) {
    return nullptr;
  }
#if XE_PLATFORM_WIN32
  // Preserve the legacy Windows path for writable views. Its capability
  // implementation remains explicitly unsupported/unverified in this slice.
  return MappedMemory::Open(host_path_, mode, offset, length);
#else
  // Managed writable mappings can allocate outside per-write admission.
  if (mode != MappedMemory::Mode::kRead) {
    return nullptr;
  }
  const auto& storage = static_cast<HostPathDevice*>(device_)->storage();
  std::error_code error;
  return storage->MapRead(RelativeStoragePath(host_path_, storage->path()),
                          offset, length, error);
#endif
}

std::unique_ptr<Entry> HostPathEntry::CreateEntryInternal(
    const std::string_view name, uint32_t attributes) {
  auto full_path = host_path_ / xe::to_path(name);
  // Direct Entry creation has no Account transaction authority, even when
  // this mounted device owns the underlying namespace reservation.
  if (device_->ProtectsManagedFile(full_path)) { return nullptr; }
#if !XE_PLATFORM_WIN32
  const auto& storage = static_cast<HostPathDevice*>(device_)->storage();
  const auto error =
      storage->Create(full_path.lexically_relative(storage->path()),
                      attributes & kFileAttributeDirectory);
  if (error) {
    return nullptr;
  }
#else
  if (attributes & kFileAttributeDirectory) {
    if (!std::filesystem::create_directories(full_path)) {
      return nullptr;
    }
  } else {
    auto file = xe::filesystem::OpenFile(full_path, "wb");
    if (!file) {
      return nullptr;
    }
    fclose(file);
  }
#endif
  auto file_info = xe::filesystem::GetInfo(full_path);
  if (!file_info) {
    return nullptr;
  }
  return std::unique_ptr<Entry>(
      HostPathEntry::Create(device_, this, full_path, file_info.value()));
}

bool HostPathEntry::DeleteEntryInternal(Entry* entry) {
  auto full_path = host_path_ / xe::to_path(entry->name());
  if (device_->ProtectsManagedFile(full_path, true)) { return false; }
#if !XE_PLATFORM_WIN32
  const auto& storage = static_cast<HostPathDevice*>(device_)->storage();
  return !storage->Remove(full_path.lexically_relative(storage->path()),
                          entry->attributes() & kFileAttributeDirectory);
#else
  std::error_code ec;  // avoid exception on remove/remove_all failure
  if (entry->attributes() & kFileAttributeDirectory) {
    // Delete entire directory and contents.
    auto removed = std::filesystem::remove_all(full_path, ec);
    return removed >= 1 && removed != static_cast<std::uintmax_t>(-1);
  } else {
    // Skip directories, they we're handled above.
    if (std::filesystem::is_directory(full_path)) {
      return false;
    }

    if (std::filesystem::exists(full_path)) {
      const auto result = std::filesystem::remove(full_path, ec);
      if (ec) {
        XELOGE("{}: Cannot remove file entry. File: {} Error: {}", __func__,
               xe::strip_pii(xe::path_to_utf8(full_path)), ec.message());
        return false;
      }
    }
    return true;
  }
#endif
}

X_STATUS HostPathEntry::RenameEntryInternal(
    Entry* new_parent, const std::string_view name,
    std::shared_ptr<const filesystem::HostStorageFileLease> permitted) {
  const std::filesystem::path new_host_path =
      static_cast<HostPathEntry*>(new_parent)->host_path_ / xe::to_path(name);
  if (device_->ProtectsManagedFile(host_path_, true) ||
      device_->ProtectsManagedFile(new_host_path, true)) {
    return X_STATUS_ACCESS_DENIED;
  }

  // Actual host suffixes can differ from guest names (collapsed package
  // payloads), so preserve each descendant's real relative backing path.
  std::vector<std::pair<HostPathEntry*, std::filesystem::path>> host_paths;
  auto prepare = [&](auto&& self, HostPathEntry* entry) -> bool {
    const auto relative = entry->host_path_.lexically_relative(host_path_);
    if (relative.empty() || relative.is_absolute() ||
        *relative.begin() == "..") {
      return false;
    }
    host_paths.emplace_back(
        entry, entry == this ? new_host_path : new_host_path / relative);
    for (const auto& child : entry->children_) {
      if (!self(self, static_cast<HostPathEntry*>(child.get()))) {
        return false;
      }
    }
    return true;
  };
  if (!prepare(prepare, this)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  std::error_code ec;
#if XE_PLATFORM_WIN32
  if (is_read_only() || new_parent->is_read_only()) {
    return X_STATUS_ACCESS_DENIED;
  }
  std::filesystem::rename(host_path_, new_host_path, ec);
#else
  const auto& storage = static_cast<HostPathDevice*>(device_)->storage();
  ec = storage->Rename(host_path_.lexically_relative(storage->path()),
                       new_host_path.lexically_relative(storage->path()), true,
                       std::move(permitted));
#endif
  if (ec) {
    XELOGE("RenameEntryInternal: Failed to rename '{}' to '{}': {}",
           xe::strip_pii(xe::path_to_utf8(host_path_)),
           xe::strip_pii(xe::path_to_utf8(new_host_path)), ec.message());
    return HostStorageError(ec);
  }
  for (auto& [entry, path] : host_paths) {
    entry->host_path_.swap(path);
  }
  return X_STATUS_SUCCESS;
}

void HostPathEntry::update() {
  if (device_->ProtectsManagedFile(host_path_)) { return; }
  auto file_info = xe::filesystem::GetInfo(host_path_);
  if (!file_info) {
    return;
  }
  // Creation time never changes, and posix reports st_ctime here, which
  // moves on every write.
  access_timestamp_ = file_info->access_timestamp;
  write_timestamp_ = file_info->write_timestamp;
  // A synthesized size, such as a collapsed package container, must stand, as
  // must an overwritten file's.
  if (file_info->type == xe::filesystem::FileInfo::Type::kFile &&
      !reported_size_fixed_ && !overwritten_) {
    size_ = file_info->total_size;
    allocation_size_ =
        xe::round_up(file_info->total_size, device()->bytes_per_sector());
  }
}

bool HostPathEntry::SetTimes(xe::filesystem::FileHandle& file_handle,
                             uint64_t create_timestamp,
                             uint64_t access_timestamp,
                             uint64_t write_timestamp) {
  if (device_->is_read_only()) {
    XELOGW("{}: Tried to set the times of read-only file {}", __func__, name_);
    return false;
  }
  // FATX keeps times in 2 s units, rounding up.
  constexpr uint64_t kTimeUnit = 2 * 10000000;
  create_timestamp = xe::round_up(create_timestamp, kTimeUnit, false);
  access_timestamp = xe::round_up(access_timestamp, kTimeUnit, false);
  write_timestamp = xe::round_up(write_timestamp, kTimeUnit, false);
  if (!file_handle.SetTimes(create_timestamp, access_timestamp,
                            write_timestamp)) {
    XELOGE("{}: Cannot set the times of {}", __func__,
           xe::strip_pii(xe::path_to_utf8(host_path_)));
    return false;
  }
  // update() never rereads the creation time, which a POSIX host can't keep.
  if (create_timestamp) {
    create_timestamp_ = create_timestamp;
  }
  if (access_timestamp) {
    access_timestamp_ = access_timestamp;
  }
  if (write_timestamp) {
    write_timestamp_ = write_timestamp;
  }
  return true;
}

}  // namespace vfs
}  // namespace xe
