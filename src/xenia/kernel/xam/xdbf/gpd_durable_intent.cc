/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/xdbf/gpd_durable_intent.h"
namespace xe::kernel::xam::detail {
std::error_code AdmitGpdIntentCompletion(size_t occupied, size_t unfinished) {
  return filesystem::detail::AdmitProfileIntentCompletion(occupied, unfinished);
}
}  // namespace xe::kernel::xam::detail
