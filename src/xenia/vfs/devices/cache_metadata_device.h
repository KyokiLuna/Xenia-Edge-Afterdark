/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_VFS_DEVICES_CACHE_METADATA_DEVICE_H_
#define XENIA_VFS_DEVICES_CACHE_METADATA_DEVICE_H_
#include <array>
#include <atomic>
#include <memory>
#include "xenia/base/cache_storage_owner.h"
#include "xenia/vfs/device.h"
namespace xe::vfs {
// Logical title-cache metadata only. It retains exact actual product/source
// roots; no host header path/private metadata or raw disk authority escapes.
class CacheMetadataDevice final : public Device {
 public:
  static std::unique_ptr<CacheMetadataDevice> Create(
      std::shared_ptr<filesystem::CacheStorageOwner>,
      std::shared_ptr<const filesystem::ConsoleStorageVolume>,std::error_code&);
  ~CacheMetadataDevice() override;
  bool Initialize() override;
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings() const override;
  Entry* ResolvePath(std::string_view) override;
  void Dump(StringBuffer*) override;
  bool is_read_only() const override {return false;}
  bool has_open_files() const override;
  bool supports_concurrent_io() const override {return true;}
  bool queues_all_requests() const override {return true;}
  void Retire() override {retired_.store(true,std::memory_order_release);}
  filesystem::HostStorageSnapshot storage_space() const override;
  const std::string& name() const override {return name_;}
  uint32_t attributes() const override {return 0;}
  uint32_t component_name_max_length() const override {return 40;}
  uint32_t total_allocation_units() const override {return 0;}
  uint32_t available_allocation_units() const override {return 0;}
  uint32_t sectors_per_allocation_unit() const override {return 0x80;}
  uint32_t bytes_per_sector() const override {return 0x200;}
 private:
  class CacheEntry;
  class CacheFile;
  CacheMetadataDevice(std::shared_ptr<filesystem::CacheStorageOwner>,
      std::array<std::shared_ptr<filesystem::HostStorageRoot>,3>);
  bool available() const noexcept;
  const std::shared_ptr<filesystem::CacheStorageOwner> owner_;
  const std::array<std::shared_ptr<filesystem::HostStorageRoot>,3> roots_;
  std::unique_ptr<CacheEntry> root_;
  const std::string name_="TitleCacheMetadata";
  std::atomic<bool> retired_{false};
};
} // namespace xe::vfs
#endif
