/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_BASE_XUI_RESOURCE_H_
#define XENIA_BASE_XUI_RESOURCE_H_
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace xe::resource {
enum class XuiPackageStatus { kFound, kNotFound, kInvalid, kUnsupported };
struct XuiPackageLookup {
  XuiPackageStatus status = XuiPackageStatus::kInvalid;
  std::span<const uint8_t> bytes;
};
XuiPackageLookup LookupXuiPackageFile(std::span<const uint8_t> package,
                                      std::string_view name);
// Borrowed view into caller-owned package. No guest memory or filesystem I/O.
std::optional<std::span<const uint8_t>> FindXuiPackageFile(
    std::span<const uint8_t> package, std::string_view name);
// Owned strings. Empty indexed rows retain the caller's locale fallback policy.
std::optional<std::vector<std::u16string>> ParseXuiStringTable(
    std::span<const uint8_t> data, uint8_t* out_flags);
}  // namespace xe::resource
#endif
