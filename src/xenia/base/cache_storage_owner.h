/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_CACHE_STORAGE_OWNER_H_
#define XENIA_BASE_CACHE_STORAGE_OWNER_H_
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <system_error>
#include "xenia/base/managed_device_identity.h"
#include "xenia/base/snapshot_publisher.h"
namespace xe { class Emulator; }
namespace xe::vfs { class CacheMetadataDevice; }
namespace xe::kernel::xam { class ContentSourceRegistry; }
namespace xe::filesystem {
class CacheStorageOwnerTestAccess;
class ManagedBootstrapGuestAdmissionPermit;
class ManagedFormatGuestAdmissionPermit;
// Actual launch product, independent of executable/module/media and native
// SYSTEM SysCache identities. Unknown/XNA without a recovered provider has no
// value. Only the actual launch producer can mint one.
class CacheProductIdentity final {
 public:
  enum class Kind : uint32_t { kXbox360 = 1, kXboxOriginal = 2 };
  Kind kind() const { return kind_; }
  uint32_t id() const { return id_; }
  uint64_t launch_generation() const { return launch_generation_; }
  std::string key() const;
 private:
  friend class xe::Emulator;
  friend class CacheStorageOwnerTestAccess;
  CacheProductIdentity(Kind kind,uint32_t id,uint64_t generation)
      :kind_(kind),id_(id),launch_generation_(generation) {}
  const Kind kind_;
  const uint32_t id_;
  const uint64_t launch_generation_;
};
class CacheFormatHandoff;
// One product's three retained physical folders and one logical metadata
// table. No package/profile registry, guest raw disk or second format engine.
// All factories and operations perform observing IO outside GCRC.
class CacheStorageOwner final : public std::enable_shared_from_this<CacheStorageOwner> {
 public:
  enum class Role : uint32_t { kLegacy = 0, kCache0 = 1, kCache1 = 2 };
  static constexpr size_t kRoleCount = 3, kTableBytes = 0x400;
  static constexpr uint64_t kMinimumFreeBytes = 20ull << 30;
  struct Options {
    std::array<std::shared_ptr<HostStorageRoot>, kRoleCount> role_parents;
    std::shared_ptr<HostStorageRoot> private_parent;
  };
  struct LogicalTable {
    std::array<uint8_t, kTableBytes> bytes{};
    std::array<uint32_t, 2> sectors_per_cluster{};
    bool operator==(const LogicalTable&) const = default;
  };
  struct Receipt {
    std::error_code error;
    bool prepared_durable=false,epoch_published=false,epoch_observed=false,
         epoch_durable=false,data_started=false,table_published=false,
         table_durable=false,complete=false;
    uint64_t removed_entries=0,epoch=0;
    std::filesystem::path preserved;
  };
  static std::shared_ptr<CacheStorageOwner> Open(
      CacheProductIdentity, Options, std::error_code&);
  ~CacheStorageOwner();
  const CacheProductIdentity& product() const;
  std::error_code error() const;
  std::vector<std::filesystem::path> preserved_paths() const;
  std::error_code CheckCurrent() const;
  bool current_cached() const noexcept;
  void Retire() noexcept;
  // Retire is nonwaiting cached cancellation. After actual VFS/operation IO
  // drains, this try-acquired teardown revokes even escaped metadata tickets
  // and drops native marker/EX owners outside the operation lock. Busy keeps
  // those owners retained; the caller must finish the real drain and retry.
  bool CloseNativeAfterIoDrained() noexcept;
  std::shared_ptr<HostStorageRoot> CaptureRoot(Role,std::error_code&) const;

  LogicalTable table(std::error_code&) const;
  // The whole request must be within the authenticated 0x400-byte logical
  // table. Unsupported adjacent sectors and integer overflow fail untouched.
  std::error_code ReadTable(uint64_t offset,std::span<uint8_t>) const;
  std::error_code FlushTable() const;
  Receipt WriteTable(uint64_t offset,std::span<const uint8_t>,
      const std::atomic<bool>* canceled=nullptr);
  std::unique_ptr<CacheFormatHandoff> CaptureFormat(Role,uint32_t sectors_per_cluster,
      const std::atomic<bool>* canceled=nullptr);
  std::unique_ptr<CacheFormatHandoff> InspectRecovery(Role);
  std::vector<Role> PendingFormats(std::error_code&) const;
  static Receipt ExecuteFormat(CacheFormatHandoff&);
  static Receipt ResumeFormat(CacheFormatHandoff&);
 private:
  friend class CacheStorageOwnerTestAccess;
  friend class xe::vfs::CacheMetadataDevice;
  friend class CacheFormatHandoff;
  friend class ManagedStorageBootstrap;
  friend class ManagedDeviceIdentity;
  friend class ManagedBootstrapGuestAdmissionPermit;
  friend class ManagedFormatGuestAdmissionPermit;
  friend class xe::kernel::xam::ContentSourceRegistry;
  struct State;
  explicit CacheStorageOwner(std::unique_ptr<State>);
  static std::error_code PublishControl(const HostStorageRoot&,const std::filesystem::path&,
      std::span<const uint8_t>,std::string nonce);
  bool AllowsPending(const HostStorageRoot&,std::string_view,uint32_t,
      const ManagedBootstrapSourceDescription* = nullptr) const;
  static bool IsCacheSource(std::string_view);
  static std::error_code BeforeNormalOpen(const HostStorageRoot&,std::string_view,uint32_t);
  static std::error_code CheckNormal(const HostStorageRoot&,
      const ManagedBootstrapSourceDescription&);
  static std::error_code CheckFormatGuestAdmission(const ManagedFormatGuestAdmissionPermit&);
  std::error_code CheckGuestAdmission(const ManagedBootstrapGuestAdmissionPermit&) const;
  bool AdmissionCurrent() const noexcept;
  bool BindRegistry(const std::shared_ptr<const uint8_t>&) noexcept;
  // Metadata-only try admission excludes a concurrent native format/table
  // mutation during exact registry/VFS publication. It performs no IO and
  // owns no callback. The caller destroys it after all namespace/model locks.
  class RegistrationAdmission final {
   public:
    ~RegistrationAdmission();
   private:
    friend class CacheStorageOwner;
    struct Impl;
    explicit RegistrationAdmission(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
  };
  std::unique_ptr<RegistrationAdmission> TryAdmitRegistration(Role,
      const std::shared_ptr<HostStorageRoot>&,
      const std::shared_ptr<const ManagedDeviceIdentity>&,uint32_t sectors) const;

  struct MetadataFileUse;
  std::shared_ptr<MetadataFileUse> AcquireMetadataFile() const;
  static Receipt FinishFormat(CacheFormatHandoff&,bool resume,unsigned stop_boundary=0);
  static std::shared_ptr<CacheStorageOwner> OpenInternal(CacheProductIdentity,Options,
      unsigned stop_boundary,std::error_code&);
  std::shared_ptr<const ManagedDeviceIdentity> CaptureIdentity(Role) const;
  std::shared_ptr<const HostStorageRoot> NativeSource(Role) const;
  std::unique_ptr<State> state_;
};
// This metadata handle owns real source EX/native handoff only until explicit
// completion/cancellation/teardown. A receipt never remints format authority.
class CacheFormatHandoff final {
 public:
  ~CacheFormatHandoff();
  CacheFormatHandoff(const CacheFormatHandoff&)=delete;
  CacheFormatHandoff& operator=(const CacheFormatHandoff&)=delete;
  const CacheStorageOwner::Receipt& receipt() const { return receipt_; }
 private:
  friend class CacheStorageOwner;
  struct State;
  explicit CacheFormatHandoff(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
  CacheStorageOwner::Receipt receipt_;
};
}  // namespace xe::filesystem
#endif
