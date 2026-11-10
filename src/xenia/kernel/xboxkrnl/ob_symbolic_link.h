/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XBOXKRNL_OB_SYMBOLIC_LINK_H_
#define XENIA_KERNEL_XBOXKRNL_OB_SYMBOLIC_LINK_H_
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "xenia/xbox.h"
namespace xe {
class Memory;
namespace vfs {
class VirtualFileSystem;
}
}  // namespace xe
namespace xe::kernel::xboxkrnl {
// A real handle provider retains its XFile and registered device/tree until
// this synchronous guest copy finishes. No guest selects the provider.
struct RetainedSymbolicLinkRoot {
  std::string guest_path;
  std::shared_ptr<void> owner;
  bool directory = false;
};
using SymbolicLinkRootResolver =
    std::function<X_STATUS(uint32_t, RetainedSymbolicLinkRoot&)>;
// Actual two-pointer guest-buffer adapter. No fabricated Kernel is required
// for ABI/memory tests. The production root provider uses real object handles.
X_STATUS TranslateSymbolicLinkGuest(Memory&, vfs::VirtualFileSystem&,
                                    uint32_t attributes, uint32_t output_ansi,
                                    const SymbolicLinkRootResolver& = {});
}  // namespace xe::kernel::xboxkrnl
#endif
