/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/kernel/util/module_resource.h"
#include <new>
#include "third_party/utfcpp/source/utf8.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_io_buffer.h"
namespace xe::kernel::util {
X_STATUS CopyModuleResource(KernelState& kernel,
                            const object_ref<UserModule>& module,
                            std::string_view section,
                            std::vector<uint8_t>& output,
                            uint32_t maximum_bytes) {
  if (!module || module->kernel_state() != &kernel ||
      !::utf8::is_valid(section) || section.empty() || section.size() > 8 ||
      section.find('\0') != std::string_view::npos || !maximum_bytes ||
      maximum_bytes > 256u * 1024u * 1024u ||
      global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto lease = kernel.AcquireUserModuleExecutionLease(module);
  if (!lease) {
    return X_STATUS_ACCESS_DENIED;
  }
  uint32_t address = 0, size = 0;
  const auto status = module->GetSection(section, &address, &size);
  if (XFAILED(status)) {
    return status;
  }
  if (!size) {
    return X_STATUS_INVALID_IMAGE_FORMAT;
  }
  if (size > maximum_bytes) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  try {
    std::vector<uint8_t> staged(size);
    const auto copied = ReadGuestIoBuffer(*kernel.memory(), address, staged);
    if (XFAILED(copied)) {
      return copied;
    }
    output = std::move(staged);
    return X_STATUS_SUCCESS;
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  }
}
}  // namespace xe::kernel::util
