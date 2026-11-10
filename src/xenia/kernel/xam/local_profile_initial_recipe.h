/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_LOCAL_PROFILE_INITIAL_RECIPE_H_
#define XENIA_KERNEL_XAM_LOCAL_PROFILE_INITIAL_RECIPE_H_
#include <memory>
#include <optional>
#include <system_error>
#include <vector>
#include "xenia/base/content_package_tree.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/stock_avatar_presets.h"
namespace xe::kernel::xam {
struct X_XAMACCOUNTINFO;
// Detached owned bytes only. A recipe grants no filesystem write, signed-in
// identity, native task completion, live provisioning or geometry readiness.
// Root/ContentManager chooses the actual source and resolves descriptor.device_id.
struct LocalProfileInitialRecipe {
  std::vector<filesystem::ContentPackageInitialMember> members;
  XCONTENT_AGGREGATE_DATA descriptor{};
  std::shared_ptr<const StockAvatarPresets> presets;
  uint32_t preset_index = 0;
};
std::optional<LocalProfileInitialRecipe> BuildInitialLocalProfile(
    const X_XAMACCOUNTINFO& account, uint64_t offline_xuid,
    std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> installed,
    uint32_t preset_index, uint32_t stock_picture_index, std::error_code&);
}  // namespace xe::kernel::xam
#endif
