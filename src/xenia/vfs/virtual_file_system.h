/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
#define XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  void Clear();

  // The opaque capsule owns actual common guest-storage admission and the
  // exact, unpublished device. Prepare performs native observation outside
  // GCRC; commit only publishes metadata and retains failure ownership.
  class PreparedDeviceRegistration final {
   public:
    ~PreparedDeviceRegistration() = default;
    PreparedDeviceRegistration(const PreparedDeviceRegistration&) = delete;
   private:
    friend class VirtualFileSystem;
    PreparedDeviceRegistration(std::shared_ptr<const uint8_t> owner,
        std::shared_ptr<Device> device)
        : owner_(std::move(owner)), device_(std::move(device)) {}
    const std::shared_ptr<const uint8_t> owner_;
    std::shared_ptr<Device> device_;
    bool consumed_ = false;
  };
  // On refusal the caller still owns device; no last native-FD rundown is
  // hidden in a failing call that inherited an outer namespace lock.
  std::unique_ptr<PreparedDeviceRegistration> PrepareDeviceRegistration(
      std::unique_ptr<Device>& device, std::error_code&);
  bool RegisterPreparedDevice(PreparedDeviceRegistration&);
  X_STATUS RegisterPreparedDeviceExclusive(PreparedDeviceRegistration&,
                                           std::shared_ptr<Device>& registered);
  bool RegisterDevice(std::unique_ptr<Device> device);
  bool RegisterManagedFile(std::shared_ptr<ManagedFileService> service) {
    return managed_files_->Register(std::move(service));
  }
  // Exact native named-device publication; no duplicate-name replacement.
  X_STATUS RegisterDeviceExclusive(std::unique_ptr<Device> device,
                                    std::shared_ptr<Device>& registered);
  bool UnregisterDevice(const std::string_view path,
                        const Device* expected = nullptr);
  bool RetireDevice(const std::string_view path);
  Device* GetDevice(const std::string_view path);
  // Retain the exact registered device before releasing the namespace lock.
  std::shared_ptr<Device> AcquireDevice(const std::string_view path);

  bool RegisterSymbolicLink(const std::string_view path,
                            const std::string_view target);
  bool UnregisterSymbolicLink(const std::string_view path);
  bool IsSymbolicLinkRegistered(const std::string_view path);
  bool FindSymbolicLink(const std::string_view path, std::string& target);

  // Owned guest names only: no existence check, device I/O or guest copy.
  // Alias selection is case insensitive, longest whole component first, with
  // bounded depth/length and cycle rejection. Output is unchanged on error.
  static constexpr size_t kMaximumTranslatedPath = 0xFFFF;
  X_STATUS TranslateSymbolicPath(std::string_view path, std::string& output);

  Entry* ResolvePath(const std::string_view path);

  Entry* CreatePath(const std::string_view path, uint32_t attributes);
  // Moves |entry| to |target_path|, which can be on a device mounted below the
  // one a link in it names.
  X_STATUS RenameEntry(Entry* entry, const std::string_view target_path,
                       bool replace_existing,
      std::shared_ptr<const filesystem::HostStorageFileLease> permitted = {});
  bool DeletePath(const std::string_view path);

  X_STATUS OpenFile(Entry* root_entry, const std::string_view path,
                    FileDisposition creation_disposition,
                    uint32_t desired_access, bool is_directory,
                    bool is_non_directory, File** out_file,
                    FileAction* out_action);

  static X_STATUS ExtractContentFile(Entry* entry,
                                     std::filesystem::path base_path,
                                     std::atomic<uint64_t>& progress);
  static X_STATUS ExtractContentFiles(
      Device* device, std::filesystem::path base_path,
      std::atomic<uint64_t>& progress,
      std::function<bool()> should_cancel = nullptr);
  static void ExtractContentHeader(Device* device,
                                   std::filesystem::path base_path);

 private:
  X_STATUS CommitPreparedDevice(PreparedDeviceRegistration&,bool exclusive,
                                 std::shared_ptr<Device>* registered);
  // Retaining the tag prevents a capsule from another/previous VFS lifetime
  // being accepted even if the new object reuses the same host address.
  const std::shared_ptr<const uint8_t> registration_owner_ =
      std::make_shared<const uint8_t>(0);
  xe::global_critical_region global_critical_region_;
  std::shared_ptr<ManagedFileRegistry> managed_files_;
  std::vector<std::shared_ptr<Device>> devices_;
  std::unordered_map<std::string, std::string> symlinks_;

  // The root of the device mounted at exactly |path|, null when none is.
  Entry* ResolveMountRoot(const std::string_view path);
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
