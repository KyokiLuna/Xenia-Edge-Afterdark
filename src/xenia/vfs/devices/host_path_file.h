/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HOST_PATH_FILE_H_
#define XENIA_VFS_DEVICES_HOST_PATH_FILE_H_

#include <atomic>
#include <memory>
#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/vfs/file.h"

namespace xe::kernel {
class XFile;
}

namespace xe {
namespace vfs {

class HostPathEntry;
class Device;

class HostPathFile : public File {
 public:
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<xe::filesystem::FileHandle> file_handle);
  ~HostPathFile() override;

  void Destroy() override;
  // Pure admission query; never completes, closes or repairs the session.
  bool HasMutatingClose() const;
  // Exact pending DATA effects of FinishOverwrite, distinct from the broader
  // open/exclusive-lease lifetime guard above. Cached metadata only.
  bool HasPendingCheckpointClose() const;
  using CheckpointCloseObservation=File::CheckpointCloseObservation;
  // Nonmutating query of the retained open handle, never Entry's cached EOF.
  // The caller supplies the usual blocking-I/O scheduling boundary.
  std::optional<filesystem::FileHandleInfo> QueryBackingInfo(
      std::error_code& error) const;
  X_STATUS QueryInformation(Information* output) const override;
  X_STATUS QueryAllocationSize(uint64_t* output) const override;
  // An Entry may hold a pending overwrite before any File has opened it.
  static bool EntryHasMutatingClose(const Entry*);
  std::shared_ptr<const filesystem::HostStorageFileLease> storage_lease()
      const override {
    return file_handle_->storage_lease();
  }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override;
  X_STATUS SetAllocationSize(uint64_t length) override;
  X_STATUS Flush() override;
  std::optional<DeviceControlLayout> DescribeDeviceControl(
      uint32_t code) const override;
  X_STATUS DeviceControl(uint32_t code, std::span<const uint8_t> input,
                         std::span<uint8_t> output,
                         uint32_t* transferred) override;
  X_STATUS SetTimes(uint64_t create_timestamp, uint64_t access_timestamp,
                    uint64_t write_timestamp) override;

 private:
  friend class xe::kernel::XFile;
  // Stack-only observation of this exact serialized backend call. These
  // negative/entered facts grant no mutation permission or guest success.
  // XFile binds it only under its existing file_lock_; no polling, allocation,
  // extra thread, lock or lifetime registry is introduced.
  class CheckpointEffectScope final {
  public:
    CheckpointEffectScope(HostPathFile &file, bool &known, bool &entered)
        : file_(file), previous_known_(file.effect_known_),
          previous_entered_(file.effect_entered_) {
      file_.effect_known_ = &known;
      file_.effect_entered_ = &entered;
    }
    ~CheckpointEffectScope() {
      file_.effect_known_ = previous_known_;
      file_.effect_entered_ = previous_entered_;
    }
    CheckpointEffectScope(const CheckpointEffectScope &) = delete;

  private:
    HostPathFile &file_;
    bool *previous_known_;
    bool *previous_entered_;
  };
  void CheckpointEffectKnown() noexcept {
    if (effect_known_) {
      *effect_known_ = true;
    }
  }
  void CheckpointEffectEntered() noexcept {
    CheckpointEffectKnown();
    if (effect_entered_) {
      *effect_entered_ = true;
    }
  }
  bool *effect_known_ = nullptr;
  bool *effect_entered_ = nullptr;
  void ObserveCheckpointClose(
      std::shared_ptr<CheckpointCloseObservation> value) override {
    checkpoint_close_ =
        std::move(value);  // actual unique File, before publication
  }
  std::shared_ptr<CheckpointCloseObservation> checkpoint_close_;
  // Tests may wrap the real owned backend at its typed I/O boundary, keeping
  // the actual reservation/root/handle while injecting host failures.
  friend class HostPathFileTestAccess;
  // Registered mount/tree stays alive through final Entry::ReleaseFile.
  std::shared_ptr<Device> device_owner_;
  std::unique_ptr<xe::filesystem::FileHandle> file_handle_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HOST_PATH_FILE_H_
