/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/console_data_owner.h"
#include "xenia/base/platform.h"
#include <algorithm>
#include <span>
#include <string_view>
#include <vector>
namespace xe::kernel {
namespace {
#if XE_PLATFORM_LINUX
// Software ownership and format marker, not a secret or a host security token.
// Portable across deliberate copies of a storage directory. HostStorageRoot
// separately pins and checks the actual live directory/mount generation.
constexpr std::string_view kOwnership =
    "Xenia Edge console data\nversion=1\npurpose=console-xconfig\n";
#endif
} // namespace
std::unique_ptr<ConsoleDataOwner>
ConsoleDataOwner::Open(std::shared_ptr<filesystem::HostStorageRoot> parent,
                       std::error_code parent_error) {
  auto owner = std::unique_ptr<ConsoleDataOwner>(new ConsoleDataOwner);
  auto &error = owner->error_;
  error = parent_error;
  if (error) {
    return owner;
  }
#if !XE_PLATFORM_LINUX
  static_cast<void>(parent);
  // The shared protection primitive cannot yet prove mount equivalence on
  // other platforms. Preserve their storage without creating console files.
  error = std::make_error_code(std::errc::operation_not_supported);
  return owner;
#else
  if (!parent || parent->read_only() || parent->is_guest_private()) {
    error = std::make_error_code(std::errc::permission_denied);
    return owner;
  }
  bool created = false;
  auto raw = parent->Subroot(kDirectory, false, error);
  if (!raw && error == std::errc::no_such_file_or_directory) {
    error = parent->Create(kDirectory, true);
    created = !error;
    if (error && error != std::errc::file_exists) {
      return owner;
    }
    raw = parent->Subroot(kDirectory, false, error);
  }
  if (!raw) {
    return owner;
  }
  if (created) {
    // Flush directory creation separately. A partial bootstrap is preserved
    // and rejected on restart, never silently adopted or deleted.
    error = parent->FlushDirectory();
    if (error) {
      return owner;
    }
    const auto bytes = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(kOwnership.data()),
        kOwnership.size());
    const auto write = raw->WriteNewFile(kMarker, bytes, true);
    error = write.error;
    if (!error && write.bytes != bytes.size()) {
      error = std::make_error_code(std::errc::io_error);
    }
    if (!error) {
      error = raw->FlushDirectory();
    }
    if (error) {
      return owner;
    }
  }
  std::vector<uint8_t> marker;
  error = raw->ReadFile(kMarker, 256, marker);
  if (error) {
    return owner;
  }
  if (marker.size() != kOwnership.size() ||
      !std::equal(marker.begin(), marker.end(), kOwnership.begin())) {
    error = std::make_error_code(std::errc::bad_message);
    return owner;
  }
  owner->private_root_ = raw->ProtectFlat(error);
  if (!owner->private_root_) {
    return owner;
  }
  owner->settings_ = ConsoleSettingsStore::Open(owner->private_root_, error);
  return owner;
#endif
}
} // namespace xe::kernel
