/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_VFS_INSTALLED_SYSTEM_SOURCE_H_
#define XENIA_VFS_INSTALLED_SYSTEM_SOURCE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

#include "xenia/base/host_storage.h"

namespace xe::vfs {

// Finite exclusion across host installation and title launch. Reservations
// survive nested modal UI loops; they do not hold a mutex across user input.
class InstalledSystemOperationGate final {
 private:
  struct State;

 public:
  class Reservation final {
   public:
    Reservation(Reservation&&) noexcept = default;
    Reservation& operator=(Reservation&&) = delete;
    ~Reservation();

   private:
    friend class InstalledSystemOperationGate;
    Reservation(std::shared_ptr<State> state, bool install)
        : state_(std::move(state)), install_(install) {}
    std::shared_ptr<State> state_;
    bool install_;
  };
  InstalledSystemOperationGate();
  std::optional<Reservation> TryInstall();
  std::optional<Reservation> TryLaunch();
  bool installing() const;

 private:
  std::shared_ptr<State> state_;
};

// One installed-system read namespace, distinct from the local installer
// destination. Generations change only at an explicit idle host boundary.
class InstalledSystemSource final
    : public std::enable_shared_from_this<InstalledSystemSource> {
 public:
  enum class Partition { kFlash, kSystemExt, kSystemAux };
  class Snapshot final {
   public:
    uint64_t generation() const { return generation_; }
    const std::filesystem::path& path() const { return path_; }
    bool shared() const { return shared_; }
    bool available() const;
    bool valid() const;
    std::shared_ptr<filesystem::HostStorageRoot> PartitionRoot(
        Partition, std::error_code&) const;
    std::shared_ptr<filesystem::HostStorageRoot> ContentRoot(
        std::error_code&) const;
    // Used for a dashboard's Package_0 too, regardless of relative-write prefs.
    std::shared_ptr<filesystem::HostStorageRoot> DirectoryRoot(
        const std::filesystem::path& host_directory, std::error_code&) const;
    std::optional<std::filesystem::path> DashboardPath(std::error_code&) const;
    bool Contains(const std::filesystem::path&) const;

   private:
    friend class InstalledSystemSource;
    Snapshot() = default;
    std::weak_ptr<const InstalledSystemSource> owner_;
    uint64_t generation_ = 0;
    bool shared_ = false;
    std::filesystem::path path_;
    std::shared_ptr<filesystem::HostStorageRoot> root_;
    std::array<std::shared_ptr<filesystem::HostStorageRoot>, 3> partitions_;
    std::shared_ptr<filesystem::HostStorageRoot> content_;
  };

  static std::shared_ptr<InstalledSystemSource> Create(
      const std::filesystem::path& local_install_root,
      const std::filesystem::path& shared_read_root, std::error_code&);
  ~InstalledSystemSource();
  std::shared_ptr<const Snapshot> snapshot() const;
  // Does not publish or retire anything. Missing local install is a valid
  // unavailable generation; explicit shared roots never fall back or reopen.
  std::shared_ptr<const Snapshot> PrepareRefresh(
      std::error_code&, bool local_install_changed = false);
  // Caller has staged/admitted all corresponding mounts and catalog view.
  bool Commit(std::shared_ptr<const Snapshot>, std::error_code&);
  bool MatchesSelection(const std::filesystem::path&) const;

 private:
  InstalledSystemSource(std::filesystem::path local,
                        std::filesystem::path shared);
  std::shared_ptr<const Snapshot> Build(uint64_t, std::error_code&);
  const std::filesystem::path local_;
  const std::filesystem::path shared_;
  mutable std::mutex mutex_;
  std::shared_ptr<const Snapshot> current_;
};
}  // namespace xe::vfs
#endif  // XENIA_VFS_INSTALLED_SYSTEM_SOURCE_H_
