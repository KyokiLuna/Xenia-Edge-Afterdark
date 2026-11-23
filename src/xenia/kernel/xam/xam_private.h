/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_PRIVATE_H_
#define XENIA_KERNEL_XAM_XAM_PRIVATE_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/xam/xam_ordinals.h"
#include "xenia/xbox.h"

namespace xe {
namespace vfs { class Device; }
namespace kernel {
class XModule;
class KernelState;
namespace xam {

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry);

// Owned host-worker description of an explicit device-backed system target.
// The retained device owns every returned Entry while descriptor fields copy.
struct SystemLaunchTarget {
  std::filesystem::path host_path;
  std::string launch_module, package_path;
  std::shared_ptr<vfs::Device> owner;
};
std::optional<SystemLaunchTarget> CaptureSystemLaunchTarget(
    KernelState&, std::string_view guest_path);

// After an already-accepted title replacement, stop this actual caller from
// resuming guest code. Owns no launch request/resource and never returns. Call
// only after all host public-invocation/Producer resources have retired.
[[noreturn]] void ParkAfterTitleHandoff();

// XamLoaderLaunchTitleEx, which doesn't return.
void LaunchTitle(std::string path, std::string_view d_drive_path,
                 uint32_t flags);
// Launches the title at |host_path| on the host, from its default module.
// Doesn't return.
void LaunchHostTitle(const std::filesystem::path& host_path, uint32_t flags);

// Loads the XUI string table |file_name| names in |module|'s |section|
// package, or the section itself without a name, as XuiLoadStringTableFromFile
// does.
X_HRESULT LoadXuiStringTable(XModule& module, std::string_view section,
                             std::string_view file_name, uint32_t* table_ptr);
// The string at |index| in a table LoadXuiStringTable loaded, or 0.
uint32_t LookupXuiStringTable(uint32_t table, uint32_t index);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                  \
  void Register##n##Exports(xe::cpu::ExportResolver* export_resolver, \
                            KernelState* kernel_state);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_PRIVATE_H_
