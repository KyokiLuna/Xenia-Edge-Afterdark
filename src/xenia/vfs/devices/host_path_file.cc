/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_file.h"

#include <algorithm>

#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/folder_volume_control.h"
#include "xenia/vfs/host_storage_errors.h"

namespace xe {
namespace vfs {

HostPathFile::HostPathFile(
    uint32_t file_access, HostPathEntry* entry,
    std::unique_ptr<xe::filesystem::FileHandle> file_handle)
    : File(file_access, entry),
      device_owner_(entry->device()->weak_from_this().lock()),
      file_handle_(std::move(file_handle)) {
  entry->RetainFile();
}

HostPathFile::~HostPathFile() {
  static_cast<HostPathEntry*>(entry_)->FinishOverwrite(file_handle_);
  // Closed first, or a delete on Windows can leave the name in place until
  // this handle closes.
  file_handle_.reset();
  entry_->ReleaseFile();
}

void HostPathFile::Destroy() { delete this; }

std::optional<filesystem::FileHandleInfo> HostPathFile::QueryBackingInfo(
    std::error_code& error) const {
  return file_handle_->QueryInfo(error);
}

bool HostPathFile::HasMutatingClose() const {
  return EntryHasMutatingClose(entry_);
}

bool HostPathFile::EntryHasMutatingClose(const Entry* value) {
  auto global_lock = global_critical_region::AcquireDirect();
  if (!value || value->delete_on_close()) { return true; }
  const auto* entry = dynamic_cast<const HostPathEntry*>(value);
  return entry && (entry->overwritten_ || entry->overwrite_opened_ ||
                   entry->overwrite_lease_ || entry->overwrite_handle_ ||
                   entry->overwrite_metadata_error_ ||
                   entry->overwrite_tail_retained_ ||
                   entry->overwrite_durability_pending_);
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  *out_bytes_read = 0;
  if (!(file_access_ &
        (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  auto* owned_entry = static_cast<HostPathEntry*>(entry_);
  if (owned_entry->overwritten_ && owned_entry->overwrite_metadata_error_) {
    return HostStorageError(owned_entry->overwrite_metadata_error_);
  }
  // An exclusive overwrite session owns logical EOF until actual close,
  // including after explicit Flush has discarded its retained physical tail.
  size_t length = buffer.size();
  auto entry = static_cast<HostPathEntry*>(entry_);
  if (entry->overwritten_) {
    length = byte_offset < entry->size_
                 ? std::min(length, entry->size_ - byte_offset)
                 : 0;
  }
  const auto result =
      file_handle_->ReadResult(byte_offset, buffer.first(length));
  *out_bytes_read = result.bytes;
  return HostStorageError(result.error);
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer,
                                 size_t byte_offset,
                                 size_t* out_bytes_written) {
  *out_bytes_written = 0;
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                        FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  *out_bytes_written = 0;
  if (entry_->is_read_only()) {
    return X_STATUS_ACCESS_DENIED;
  }
  auto* entry = static_cast<HostPathEntry*>(entry_);
  if (entry->overwritten_ && entry->overwrite_metadata_error_) {
    // In particular do not recover here: XFile may already have computed an
    // append offset from the last-known metadata before this backend call.
    return HostStorageError(entry->overwrite_metadata_error_);
  }
  const auto result = file_handle_->WriteAllResult(byte_offset, buffer);
  *out_bytes_written = result.bytes;
  if (entry->overwritten_ && result.bytes &&
      byte_offset <= SIZE_MAX - result.bytes &&
      byte_offset + result.bytes > entry->size_) {
    entry->SetOverwrittenSize(byte_offset + result.bytes);
  }
  if (entry->overwritten_ && result.bytes) {
    entry->overwrite_durability_pending_ = true;
  }
  return HostStorageError(result.error);
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (entry_->is_read_only()) {
    return X_STATUS_ACCESS_DENIED;
  }
  auto entry = static_cast<HostPathEntry*>(entry_);
  if (entry->overwritten_ && entry->overwrite_metadata_error_) {
    return HostStorageError(entry->overwrite_metadata_error_);
  }
  std::error_code error;
  if (entry->overwritten_ && entry->overwrite_tail_retained_) {
    const auto info = file_handle_->QueryInfo(error);
    if (!info) {
      return HostStorageError(error);
    }
    // FATX releases clusters without clearing them. Explicit EOF changes may
    // reclaim those retained bytes too; only growth beyond the physical high
    // water mark needs a real guarded allocation. The exclusive reservation
    // keeps aliases from observing the difference until exact final trim.
    if (length > info->size) {
      error = file_handle_->ResizeResult(length);
    }
    if (!error) {
      entry->SetOverwrittenSize(length);
      entry->overwrite_durability_pending_ = true;
    }
  } else {
    error = file_handle_->ResizeResult(length);
    if (entry->overwritten_) {
      // The active exclusive owner keeps metadata coherent for the real
      // Entry size/allocation consumers even after retained tail is gone.
      if (!error) {
        entry->SetOverwrittenSize(length);
      } else {
        // Failure does not prove EOF was unchanged. Latch uncertainty before
        // observation; preserve the original resize status for this request.
        entry->overwrite_metadata_error_ = error;
        (void)entry->ReconcileOverwriteEof(*file_handle_);
      }
      entry->overwrite_durability_pending_ = true;
    }
  }
  return HostStorageError(error);
}

X_STATUS HostPathFile::Flush() {
  if (!(file_access_ &
        (FileAccess::kFileWriteData | FileAccess::kFileAppendData |
         FileAccess::kGenericWrite | FileAccess::kGenericAll))) {
    return X_STATUS_ACCESS_DENIED;
  }
  auto* entry = static_cast<HostPathEntry*>(entry_);
  if (entry->overwritten_ || entry->overwrite_lease_) {
    // Explicit Flush commits current logical contents: retaining discarded
    // FATX tail bytes after this point would contradict durable host EOF.
    return HostStorageError(entry->CompleteOverwrite(*file_handle_));
  }
  return HostStorageError(file_handle_->FlushResult());
}

std::optional<File::DeviceControlLayout> HostPathFile::DescribeDeviceControl(
    uint32_t code) const {
  const auto* device = static_cast<const HostPathDevice*>(entry_->device());
  if (entry_->parent() || !(entry_->attributes() & kFileAttributeDirectory) ||
      device->control_role() != HostPathDevice::ControlRole::kFolderVolume) {
    return {};
  }
  return DescribeFolderVolumeControl(code);
}

X_STATUS HostPathFile::DeviceControl(uint32_t code,
                                     std::span<const uint8_t> input,
                                     std::span<uint8_t> output,
                                     uint32_t* transferred) {
  *transferred = 0;
  if (!DescribeDeviceControl(code)) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  const auto access = CheckDeviceControlAccess(code);
  if (XFAILED(access)) {
    return access;
  }
  // The opened handle retains this exact mount/tree; retirement cannot be
  // bypassed by resolving a new path or querying another filesystem.
  return EncodeFolderVolumeControl(code, entry_->device()->storage_space(),
                                   entry_->device()->bytes_per_sector(), input,
                                   output, transferred);
}

X_STATUS HostPathFile::SetTimes(uint64_t create_timestamp,
                                uint64_t access_timestamp,
                                uint64_t write_timestamp) {
  if (entry_->is_read_only()) {
    return X_STATUS_ACCESS_DENIED;
  }
  // Set through this handle, since a host file system can restamp a file when
  // a handle that wrote it closes, unless the times were set through it.
  if (static_cast<HostPathEntry*>(entry_)->SetTimes(
          *file_handle_, create_timestamp, access_timestamp, write_timestamp)) {
    return X_STATUS_SUCCESS;
  } else {
    return HostStorageError(file_handle_->LastError());
  }
}

}  // namespace vfs
}  // namespace xe
