/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HOST_PATH_ENTRY_H_
#define XENIA_VFS_DEVICES_HOST_PATH_ENTRY_H_

#include <atomic>
#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/vfs/entry.h"

namespace xe {
namespace vfs {

class HostPathDevice;

class HostPathEntry : public Entry {
 public:
  HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                const std::filesystem::path& host_path);
  ~HostPathEntry() override;

  static HostPathEntry* Create(Device* device, Entry* parent,
                               const std::filesystem::path& full_path,
                               xe::filesystem::FileInfo file_info);

  const std::filesystem::path& host_path() const { return host_path_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

  bool can_map() const override { return true; }
  std::unique_ptr<MappedMemory> OpenMapped(MappedMemory::Mode mode,
                                           size_t offset,
                                           size_t length) override;
  void update() override;
  bool has_open_files() const override;
  void RetainFile() override { open_files_.fetch_add(1); }
  void ReleaseFile() override;
  bool Overwrite() override;
  InPlaceOverwriteResult OverwriteInPlace() override;

  // Sets the FILETIMEs that aren't 0 through |file_handle|, one of this
  // entry's.
  bool SetTimes(xe::filesystem::FileHandle& file_handle,
                uint64_t create_timestamp, uint64_t access_timestamp,
                uint64_t write_timestamp);

 private:
  friend class HostPathDevice;
  friend class HostPathFile;

  void SetOverwrittenSize(size_t size);
  void AbortOverwrite();
  std::error_code CompleteOverwrite(filesystem::FileHandle&);
  std::error_code ReconcileOverwriteEof(filesystem::FileHandle&);
  std::error_code RetryRetainedOverwrite();
  void ReleaseOverwriteOwnership();
  void FinishOverwrite(std::unique_ptr<filesystem::FileHandle>&);

  std::unique_ptr<Entry> CreateEntryInternal(const std::string_view name,
                                             uint32_t attributes) override;
  bool DeleteEntryInternal(Entry* entry) override;
  X_STATUS
  RenameEntryInternal(Entry* new_parent, const std::string_view name,
                      std::shared_ptr<const filesystem::HostStorageFileLease>
                          permitted) override;

  std::filesystem::path host_path_;
  std::atomic<size_t> open_files_{0};
  // When set, size_ is a synthesized value (e.g. an STFS container size for a
  // collapsed content package) and update() must not overwrite it with the
  // backing file's real size.
  bool reported_size_fixed_ = false;
  // Exclusive metadata owner/session: remains set across explicit Flush until
  // actual successful close, so namespace update cannot replace owned EOF.
  std::atomic<bool> overwritten_{false};
  // Accessed by the serialized XFile consumer, or by GCRC namespace retry
  // only after its last File has closed. Tail publication is not owner release.
  bool overwrite_tail_retained_ = false;
  bool overwrite_durability_pending_ = false;
  // Nonzero means owned EOF is unknown after a failed physical EOF operation.
  // Only explicit Flush/close retry may reconcile before data I/O resumes.
  std::error_code overwrite_metadata_error_;
  uint64_t overwrite_filesystem_id_ = 0, overwrite_file_id_ = 0;
  // Reservation survives any completion error, independently of retained-tail
  // mode. The next Open retries completion without trimming a published EOF.
  std::unique_ptr<filesystem::FileHandle> overwrite_handle_;
  std::shared_ptr<const filesystem::HostStorageFileLease> overwrite_lease_;
  size_t overwrite_original_size_ = 0, overwrite_original_allocation_ = 0;
  bool overwrite_opened_ = false;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HOST_PATH_ENTRY_H_
