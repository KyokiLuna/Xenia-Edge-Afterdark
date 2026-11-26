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
#include <limits>

#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/folder_volume_control.h"
#include "xenia/base/retained_file_metadata.h"
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
  // Capture the real pending-close scalar BEFORE ReleaseFile can remove this
  // child Entry. XFile consumes it only after this genuine destructor returns.
  if (checkpoint_close_) {
    checkpoint_close_->data_status.store(
        HasPendingCheckpointClose() ? X_STATUS_UNSUCCESSFUL : X_STATUS_SUCCESS,
        std::memory_order_release);
  }
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

X_STATUS HostPathFile::QueryInformation(Information* output) const {
  if (!output) return X_STATUS_INVALID_PARAMETER;
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  Information result{};
  const auto status = File::QueryInformation(&result);
  if (XFAILED(status)) return status;
  if (!file_handle_) return X_STATUS_INVALID_HANDLE;
  const auto* entry = static_cast<const HostPathEntry*>(entry_);
  if (entry->overwritten_ && entry->overwrite_metadata_error_) {
    return HostStorageError(entry->overwrite_metadata_error_);
  }
  std::error_code error;
  const auto actual = filesystem::QueryRetainedFileMetadata(*file_handle_, error);
  if (!actual) {
    if (error == std::errc::value_too_large) return X_STATUS(0xC0000095);
    return HostStorageError(error);
  }
  result.access_timestamp = actual->access_timestamp;
  result.write_timestamp = actual->write_timestamp;
  result.change_timestamp = actual->change_timestamp;
  // Preserve deliberate guest views: directories and collapsed package
  // headers have logical sizes; an exclusive overwrite owns its logical EOF
  // even while its old physical FATX tail is retained. Never publish tail EOF.
  if (!entry->reported_size_fixed_ &&
      !(result.attributes & kFileAttributeDirectory)) {
    if (!entry->overwritten_) result.end_of_file = actual->size;
    const uint64_t unit = uint64_t(entry->sectors_per_allocation_unit()) *
                          retained_device()->bytes_per_sector();
    const auto allocation = RoundFileAllocationSize(actual->allocated_bytes,
                                                    unit, &result.allocation_size);
    if (XFAILED(allocation)) return allocation;
  }
  *output = result;
  return X_STATUS_SUCCESS;
}

X_STATUS HostPathFile::QueryAllocationSize(uint64_t* output) const {
  if (!output) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  // The real opened file retains this tree. Never reselect a device/path or
  // refresh Entry metadata after a rename, replacement or logical overwrite.
  const auto& device = retained_device();
  if (!entry_ || !device || device.get() != entry_->device() || !file_handle_) {
    return X_STATUS_INVALID_HANDLE;
  }
  const uint64_t unit = uint64_t(entry_->sectors_per_allocation_unit()) *
                        device->bytes_per_sector();
  // Validate geometry before entering the native backend.
  uint64_t zero = 0;
  auto status = RoundFileAllocationSize(0, unit, &zero);
  if (XFAILED(status)) {
    return status;
  }
  std::error_code error;
  const auto bytes = file_handle_->QueryAllocatedBytes(error);
  if (error == std::errc::value_too_large) {
    return X_STATUS(0xC0000095);  // STATUS_INTEGER_OVERFLOW
  }
  if (error) {
    return HostStorageError(error);
  }
  if (!bytes) {
    return X_STATUS_NOT_SUPPORTED;
  }
  return RoundFileAllocationSize(*bytes, unit, output);
}

bool HostPathFile::HasPendingCheckpointClose() const {
  auto lock = global_critical_region::AcquireDirect();
  const auto* entry = static_cast<const HostPathEntry*>(entry_);
  return entry->delete_on_close() || entry->overwrite_handle_ ||
         entry->overwrite_tail_retained_ ||
         entry->overwrite_durability_pending_ ||
         bool(entry->overwrite_metadata_error_);
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
  CheckpointEffectKnown();
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
  CheckpointEffectEntered();
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

X_STATUS HostPathFile::SetAllocationSize(uint64_t length) {
  if(!(file_access_&(FileAccess::kGenericWrite|FileAccess::kFileWriteData)))return X_STATUS_ACCESS_DENIED;
  if(length>std::numeric_limits<size_t>::max())return X_STATUS(0xC0000095);
  Information before{};const auto observed=QueryInformation(&before);
  if(XFAILED(observed))return observed;
  // Preserve the legacy no-reservation no-op, but still reconcile metadata
  // from this actual descriptor after a prior failed observation.
  const auto resized=length<before.end_of_file ? SetLength(size_t(length)) : X_STATUS_SUCCESS;
  // A failing resize may have changed backing EOF. Reobserve the actual owned
  // descriptor in either case; preserve the original mutation error if present.
  // A later metadata failure does not claim rollback of successful shrink.
  std::error_code error;const auto actual=filesystem::QueryRetainedFileMetadata(*file_handle_,error);
  const auto refreshed=actual ? static_cast<HostPathEntry*>(entry_)->PublishRetainedFileMetadata(*actual)
                              : HostStorageError(error ? error : std::make_error_code(std::errc::io_error));
  return XFAILED(resized) ? resized : refreshed;
}
X_STATUS HostPathFile::SetLength(size_t length) {
  CheckpointEffectKnown();
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
    CheckpointEffectEntered();
    if (length > info->size) {
      error = file_handle_->ResizeResult(length);
    }
    if (!error) {
      entry->SetOverwrittenSize(length);
      entry->overwrite_durability_pending_ = true;
    }
  } else {
    CheckpointEffectEntered();
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
  CheckpointEffectKnown();
  if (!(file_access_ &
        (FileAccess::kFileWriteData | FileAccess::kFileAppendData |
         FileAccess::kGenericWrite | FileAccess::kGenericAll))) {
    return X_STATUS_ACCESS_DENIED;
  }
  auto* entry = static_cast<HostPathEntry*>(entry_);
  if (entry->overwritten_ || entry->overwrite_lease_) {
    // The exact overwrite owner marks the actual trim/fsync effect only
    // after its retained-identity/EOF observation has succeeded.
    return HostStorageError(
        entry->CompleteOverwrite(*file_handle_, effect_entered_));
  }
  CheckpointEffectEntered();
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
  // These fixed folder-volume controls only encode retained metadata. The
  // actual supported implementation has no persistence/namespace effect.
  CheckpointEffectKnown();
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
  CheckpointEffectKnown();
  if (entry_->is_read_only()) {
    return X_STATUS_ACCESS_DENIED;
  }
  // Set through this handle, since a host file system can restamp a file when
  // a handle that wrote it closes, unless the times were set through it.
  CheckpointEffectEntered();
  if (static_cast<HostPathEntry*>(entry_)->SetTimes(
          *file_handle_, create_timestamp, access_timestamp, write_timestamp)) {
    return X_STATUS_SUCCESS;
  } else {
    return HostStorageError(file_handle_->LastError());
  }
}

}  // namespace vfs
}  // namespace xe
