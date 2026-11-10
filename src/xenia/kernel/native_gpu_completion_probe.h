/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_GPU_COMPLETION_PROBE_H_
#define XENIA_KERNEL_NATIVE_GPU_COMPLETION_PROBE_H_

#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe::gpu {
class GpuCompletion;
}

namespace xe::kernel {
class KernelState;
class UserModule;

// Explicit SYSTEM diagnostic after the title is already presenting. Captures
// an all-command marker on the CP thread and observes actual GPU completion,
// retaining the module until retirement. Does not submit a native UI record or
// acknowledge a guest system-buffer identifier. Caller joins before teardown.
X_HRESULT RunNativeGpuCompletionProbe(KernelState* kernel_state,
                                      const object_ref<UserModule>& module,
                                      gpu::GpuCompletion* completed = nullptr);

// Explicit allocation-only diagnostic on the CP thread. Retains no backend
// owners in guest resources and does not activate/execute a native context.
X_HRESULT RunNativeGpuContextAllocationProbe(
    KernelState* kernel_state, const object_ref<UserModule>& module);
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_GPU_COMPLETION_PROBE_H_
