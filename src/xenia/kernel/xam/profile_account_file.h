/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_PROFILE_ACCOUNT_FILE_H_
#define XENIA_KERNEL_XAM_PROFILE_ACCOUNT_FILE_H_
#include <condition_variable>
#include <mutex>
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/vfs/managed_file_service.h"
namespace xe::kernel::xam {
class ProfileManager;
// File handles may outlive a profile mount or title. Only this counted gate
// grants a borrowed manager during a real operation. Teardown closes admission
// and joins actual entered operations before any manager fields are destroyed.
class ProfileAccountCommitGate final
    : public std::enable_shared_from_this<ProfileAccountCommitGate> {
 public:
  explicit ProfileAccountCommitGate(ProfileManager* owner) : owner_(owner) {}
  class Use final {
   public:
    ~Use();
    ProfileManager& owner() const { return *owner_; }
   private:
    friend class ProfileAccountCommitGate;
    Use(std::shared_ptr<ProfileAccountCommitGate> gate, ProfileManager* owner)
        : gate_(std::move(gate)), owner_(owner) {}
    std::shared_ptr<ProfileAccountCommitGate> gate_;
    ProfileManager* owner_;
  };
  std::unique_ptr<Use> Acquire();
  void CloseAndDrain();
  bool closed() const { return closed_.load(std::memory_order_acquire); }
 private:
  std::mutex mutex_;
  std::condition_variable drained_;
  ProfileManager* owner_;
  size_t active_ = 0;
  std::atomic<bool> closed_{false};
};
class ProfileAccountFileService final : public vfs::ManagedFileService {
 public:
  ProfileAccountFileService(uint64_t xuid,
      std::shared_ptr<const filesystem::HostStorageRoot> view,
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery,
      std::shared_ptr<ProfileAccountCommitGate> gate)
      : xuid_(xuid), view_(std::move(view)), recovery_(std::move(recovery)),
        gate_(std::move(gate)), path_(view_->path() / "Account") {}
  const std::filesystem::path& backing_path() const override { return path_; }
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const override { return view_->directory_identity(); }
  bool retiring() const override {
    return retired_.load(std::memory_order_acquire) || view_->retiring() || gate_->closed();
  }
  void Retire() { retired_.store(true, std::memory_order_release); }
  X_STATUS Open(std::shared_ptr<vfs::Device>, std::string,
      vfs::FileDisposition, uint32_t, bool, vfs::File**,
      vfs::FileAction*) override;
 private:
  std::atomic<bool> retired_{false};
  const uint64_t xuid_;
  const std::shared_ptr<const filesystem::HostStorageRoot> view_;
  const std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_;
  const std::shared_ptr<ProfileAccountCommitGate> gate_;
  const std::filesystem::path path_;
};
}  // namespace xe::kernel::xam
#endif
