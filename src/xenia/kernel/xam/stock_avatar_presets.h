/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_STOCK_AVATAR_PRESETS_H_
#define XENIA_KERNEL_XAM_STOCK_AVATAR_PRESETS_H_
#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <system_error>
#include <vector>
#include "xenia/vfs/installed_system_source.h"
#include "xenia/kernel/xam/avatar_manifest.h"
namespace xe::kernel::xam {
// Actual installed MiniCreator inputs, not a native engine-ready receipt.
// No profile, title, SYSTEM task, host path reopening or asset extraction.
class StockAvatarPresets final {
 public:
  static constexpr uint32_t kCount = 12;
  static constexpr size_t kManifestBytes = kAvatarManifestBytes;
  using BodyType = AvatarBodyType;
  struct Preset {
    std::array<uint8_t, kManifestBytes> appearance;
    std::vector<uint8_t> preview_64_png;
    BodyType body_type;
  };
  static std::shared_ptr<const StockAvatarPresets> Capture(
      std::shared_ptr<const vfs::InstalledSystemSource::Snapshot>,
      std::error_code&);
  const Preset* Get(uint32_t index) const;
  uint64_t source_generation() const { return source_->generation(); }
  std::error_code CheckAvailable() const;
 private:
  StockAvatarPresets() = default;
  std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> source_;
  std::shared_ptr<filesystem::HostStorageRoot> system_ext_;
  std::array<Preset, kCount> presets_;
};
}  // namespace xe::kernel::xam
#endif
