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

namespace xe::kernel::xam { class ContentManager; class ProfileManager; }

namespace xe {
namespace vfs {

// One-use read-routing proof from the actual HLE source-selection producer.
// No public type/boolean override can manufacture a selected-source boundary.
class SourceParentSelection final {
 public:
  ~SourceParentSelection() = default;
 private:
  friend class xe::kernel::xam::ContentManager;
  friend class VirtualFileSystem;
  SourceParentSelection(const SourceParentSelection&) = delete;
  SourceParentSelection& operator=(const SourceParentSelection&) = delete;
  SourceParentSelection(Device* child, std::shared_ptr<Device> parent,
      std::shared_ptr<const filesystem::ConsoleStorageVolume> volume,
      std::shared_ptr<const filesystem::HostStorageRoot> certificate,
      std::shared_ptr<const filesystem::HostStorageRoot> source)
      : child_(child), parent_(std::move(parent)), volume_(std::move(volume)),
        certificate_(std::move(certificate)), source_(std::move(source)) {}
  Device* child_;
  std::shared_ptr<Device> parent_;
  std::shared_ptr<const filesystem::ConsoleStorageVolume> volume_;
  std::shared_ptr<const filesystem::HostStorageRoot> certificate_, source_;
};

enum class DeviceRetirementPolicy {
  kWithdraw,       // Actual source/runtime withdrawal revokes backend access.
  kDrainOpenFiles, // Ordinary close retains a busy backend/root for retry.
};

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  void Clear();
  // New Kernel setup only. Old captured epochs remain sealed after reopening.
  void BeginStoragePreparation();
  // Registration only; ordinary reads, writes, flushes and closes stay live.
  void SealStoragePreparation();

  // The opaque capsule owns actual common guest-storage admission and the
  // exact, unpublished device. Prepare performs native observation outside
  // GCRC; commit only publishes metadata and retains failure ownership.
  class PreparedDeviceRegistration final {
   public:
    ~PreparedDeviceRegistration() = default;
    // Exact already admitted unpublished device, never a pathname relookup.
    const std::shared_ptr<Device>& device() const { return device_; }
    PreparedDeviceRegistration(const PreparedDeviceRegistration&) = delete;
   private:
    friend class VirtualFileSystem;
    PreparedDeviceRegistration(std::shared_ptr<const uint8_t> owner,
        std::shared_ptr<Device> device)
        : owner_(std::move(owner)), device_(std::move(device)) {}
    const std::shared_ptr<const uint8_t> owner_;
    std::shared_ptr<Device> device_;
    std::unique_ptr<SourceParentSelection> source_selection_;
    bool consumed_ = false;
    // Original negative cuts, not the temporary aggregated forwarding token.
    std::vector<std::stop_token> preparation_tokens_;
    std::vector<std::shared_ptr<const std::atomic<bool>>> preparation_flags_;
  };
  // On refusal the caller still owns device; no last native-FD rundown is
  // hidden in a failing call that inherited an outer namespace lock.
  std::unique_ptr<PreparedDeviceRegistration> PrepareDeviceRegistration(
      std::unique_ptr<Device>& device, std::error_code&,
      const std::shared_ptr<Device>& parent = {},
      std::unique_ptr<SourceParentSelection> source_selection = {},
      filesystem::HostStorageFailureObservation* diagnostic = nullptr);
  // Ephemeral owned transaction over the one registered device graph. It is
  // neither a second child registry nor a callback into Kernel-owned state.
  class DeviceRetirement final {
   public:
    ~DeviceRetirement() = default;
    DeviceRetirement(const DeviceRetirement&) = delete;
    DeviceRetirement& operator=(const DeviceRetirement&) = delete;
    const std::vector<std::shared_ptr<Device>>& devices() const { return devices_; }
   private:
    friend class VirtualFileSystem;
    DeviceRetirement(std::shared_ptr<const uint8_t> owner,
        std::shared_ptr<Device> root, uint64_t epoch,
        DeviceRetirementPolicy policy)
        : owner_(std::move(owner)), root_(std::move(root)),
          epoch_(epoch), policy_(policy) {}
    const std::shared_ptr<const uint8_t> closure_owner_ =
        std::make_shared<const uint8_t>(0);
    std::shared_ptr<const uint8_t> owner_;
    std::shared_ptr<Device> root_;
    std::vector<std::shared_ptr<Device>> devices_;
    bool committed_ = false;
    uint64_t epoch_;
    const DeviceRetirementPolicy policy_;
  };
  // Both phases are metadata-only and may join one outer GCRC interval with
  // the ContentManager root-map/alias transition. Native retirement is later.
  std::unique_ptr<DeviceRetirement> PrepareDeviceRetirement(
      const std::shared_ptr<Device>& root, bool include_root, X_STATUS&,
      DeviceRetirementPolicy policy = DeviceRetirementPolicy::kWithdraw);
  // Closes namespace admission atomically, without retiring any busy backend.
  X_STATUS CommitDeviceRetirement(DeviceRetirement&);
  // Children first; success transfers exactly this old device into an empty
  // output. Caller retires/releases it outside GCRC before testing its parent.
  // A busy descendant or actual open File keeps this exact node registered.
  X_STATUS FinishDeviceRetirement(DeviceRetirement&, size_t index,
                                  std::shared_ptr<Device>& retired);
  X_STATUS RetireDeviceBackings(DeviceRetirement&);
  bool RegisterPreparedDevice(PreparedDeviceRegistration&);
  X_STATUS RegisterPreparedDeviceExclusive(PreparedDeviceRegistration&,
                                           std::shared_ptr<Device>& registered);
  // Namespace-only exact replacement. New backing/admission is prepared first;
  // failure leaves the old node and aliases untouched. Actual old Retire and
  // last-resource destruction belong to the caller after releasing GCRC.
  X_STATUS ReplacePreparedDeviceExclusive(PreparedDeviceRegistration&,
      const std::shared_ptr<Device>& expected,
      std::shared_ptr<Device>& registered,std::shared_ptr<Device>& retired);

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
  // One owned alias+longest whole-component mount snapshot; metadata only.
  std::shared_ptr<Device> AcquirePathDevice(std::string_view path);

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
  friend class xe::kernel::xam::ContentManager;
  friend class xe::kernel::xam::ProfileManager;
  // Metadata-only expected-owner withdrawal for a failed atomic package
  // publication. The caller retains the returned owner until outside GCRC.
  std::shared_ptr<Device> DetachDevice(std::string_view, const Device*);
  // Already canonical owned names; metadata-only/no allocation after closure commit.
  void UnregisterExpectedSymbolicLink(std::string_view alias,
      std::string_view target, const Device* expected);
  X_STATUS CommitPreparedDevice(PreparedDeviceRegistration&,bool exclusive,
      std::shared_ptr<Device>* registered,
      const std::shared_ptr<Device>& expected = {},
      std::shared_ptr<Device>* retired = nullptr);
  // Retaining the tag prevents a capsule from another/previous VFS lifetime
  // being accepted even if the new object reuses the same host address.
  const std::shared_ptr<const uint8_t> registration_owner_ =
      std::make_shared<const uint8_t>(0);
  xe::global_critical_region global_critical_region_;
  std::shared_ptr<ManagedFileRegistry> managed_files_;
  std::vector<std::shared_ptr<Device>> devices_;
  uint64_t device_epoch_ = 0;
  std::mutex preparation_mutex_;
  std::shared_ptr<std::stop_source> preparation_stop_ = std::make_shared<std::stop_source>();
  std::unordered_map<std::string, std::string> symlinks_;

  // The root of the device mounted at exactly |path|, null when none is.
  Entry* ResolveMountRoot(const std::string_view path);
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
