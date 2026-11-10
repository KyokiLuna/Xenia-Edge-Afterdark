/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_RUNTIME_EXECUTOR_H_
#define XENIA_KERNEL_GUEST_RUNTIME_EXECUTOR_H_

#include <filesystem>

#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe::kernel {
class KernelState;
class UserModule;
struct ValidatedGuestRuntimePlan;

// Explicit isolated diagnostic operation, not DLL attach or a generic PE rule.
// Requires a SYSTEM thread and a verified plan for this loaded image. Keeps a
// successfully or partially initialized runtime resident until kernel teardown.
X_HRESULT InitializeGuestRuntimeFromPlan(
    KernelState* kernel_state, const object_ref<UserModule>& module,
    const std::filesystem::path& path,
    ValidatedGuestRuntimePlan* verified = nullptr);
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_GUEST_RUNTIME_EXECUTOR_H_
