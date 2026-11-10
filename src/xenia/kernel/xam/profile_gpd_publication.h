/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_PROFILE_GPD_PUBLICATION_H_
#define XENIA_KERNEL_XAM_PROFILE_GPD_PUBLICATION_H_
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "xenia/base/managed_device_identity.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
namespace xe::vfs {
class HostPathDevice;
}
namespace xe::kernel::xam {
// Storage identity only. This does not certify sign-in or an in-memory model
// version. ProfileManager supplies these exact owners under its namespace and
// mount locks, then all observing/blocking work runs outside those locks.
class ProfileStorageLease final {
 public:
  static std::filesystem::path ProfileRelative(uint64_t xuid);
  static std::shared_ptr<ProfileStorageLease> Capture(
      uint64_t xuid, std::string source_id, uint32_t device_id,
      uint64_t source_generation,
      std::shared_ptr<vfs::HostPathDevice> exact_device,
      std::shared_ptr<const filesystem::HostStorageRoot> exact_source,
      std::shared_ptr<const filesystem::HostStorageRoot> exact_view,
      std::error_code&,
      std::shared_ptr<const filesystem::ManagedDeviceIdentity> = {},
      std::shared_ptr<const filesystem::HostStorageRoot> guest_generation = {});
  uint64_t xuid() const { return xuid_; }
  const std::string& source_id() const { return source_id_; }
  uint32_t device_id() const { return device_id_; }
  uint64_t source_generation() const { return source_generation_; }
  const auto& view() const { return view_; }
  const auto& source() const { return source_; }
  const auto& guest_generation() const { return guest_generation_; }
  const auto& device() const { return device_; }
  const auto& managed_identity() const { return managed_identity_; }
  bool retiring() const {
    return retired_.load(std::memory_order_acquire) || source_->retiring() ||
           view_->retiring() || guest_generation_->retiring() ||
           (managed_identity_ && managed_identity_->retiring());
  }
  // Logical retirement never waits for publication or borrows a manager.
  // Actual device/source retirement also invalidates their storage views.
  void Retire() { retired_.store(true, std::memory_order_release); }
  const std::atomic<bool>* cancellation() const { return &retired_; }
  std::error_code CheckAvailable() const;

 private:
  ProfileStorageLease(uint64_t xuid, std::string source_id, uint32_t device_id,
                      uint64_t generation,
                      std::shared_ptr<vfs::HostPathDevice> device,
                      std::shared_ptr<const filesystem::HostStorageRoot> source,
                      std::shared_ptr<const filesystem::HostStorageRoot> view,
                      std::shared_ptr<const filesystem::ManagedDeviceIdentity>,
                      std::shared_ptr<const filesystem::HostStorageRoot>);
  const uint64_t xuid_;
  const std::string source_id_;
  const uint32_t device_id_;
  const uint64_t source_generation_;
  const std::shared_ptr<vfs::HostPathDevice> device_;
  const std::shared_ptr<const filesystem::HostStorageRoot> source_, view_,
      guest_generation_;
  const std::shared_ptr<const filesystem::ManagedDeviceIdentity>
      managed_identity_;
  std::atomic<bool> retired_{false};
};
struct ProfileGpdModelStamp {
  uint64_t sign_in_generation = 0;
  uint64_t model_version = 0;
};
}  // namespace xe::kernel::xam
#endif
