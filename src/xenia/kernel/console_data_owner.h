/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_CONSOLE_DATA_OWNER_H_
#define XENIA_KERNEL_CONSOLE_DATA_OWNER_H_
#include "xenia/base/host_storage.h"
#include "xenia/kernel/console_settings_store.h"
#include <memory>
#include <system_error>
namespace xe::kernel {
// Host console lifetime, independent of a title's VFS and KernelState. Create
// before any guest source admission. Never adopt an unmarked directory or
// replace an unreadable settings database with an empty one.
class ConsoleDataOwner {
public:
  static constexpr char kDirectory[] = ".xenia-console-v1";
  static constexpr char kMarker[] = "owner.v1";
  static std::unique_ptr<ConsoleDataOwner>
  Open(std::shared_ptr<filesystem::HostStorageRoot> managed_parent,
       std::error_code parent_error = {});
  const std::error_code &error() const { return error_; }
  const std::shared_ptr<ConsoleSettingsStore> &settings() const {
    return settings_;
  }
  const std::shared_ptr<filesystem::HostStorageRoot> &private_root() const {
    return private_root_;
  }

private:
  ConsoleDataOwner() = default;
  // Keep protection even if opening an existing store fails. An error must not
  // make the same preserved bytes guest-readable later in this process.
  std::shared_ptr<filesystem::HostStorageRoot> private_root_;
  std::shared_ptr<ConsoleSettingsStore> settings_;
  std::error_code error_;
};
} // namespace xe::kernel
#endif
