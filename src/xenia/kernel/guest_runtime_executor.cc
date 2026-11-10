/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/guest_runtime_executor.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
namespace xe::kernel {
X_HRESULT InitializeGuestRuntimeFromPlan(KernelState* state,
                                         const object_ref<UserModule>& module,
                                         const std::filesystem::path& path,
                                         ValidatedGuestRuntimePlan* verified) {
  if (!XThread::IsInThread()) {
    return X_E_INVALIDARG;
  }
  auto* thread = XThread::GetCurrentThread();
  if (!thread || thread->native_frame_thread_identity().process_type !=
                     X_PROCTYPE_SYSTEM) {
    return X_E_INVALIDARG;
  }
  std::string error;
  auto calls = GuestRuntimeCallExecutor::Load(state, module, path, &error);
  if (!calls) {
    XELOGE("Guest runtime: {}", error);
    return X_E_INVALIDARG;
  }
  const auto result = calls->InitializeHeap(*thread);
  const bool ready = result == GuestRuntime::InitStatus::kReady ||
                     result == GuestRuntime::InitStatus::kAlreadyReady;
  if (ready && verified) {
    *verified = calls->validated();
  }
  return ready ? X_E_SUCCESS : X_E_FAIL;
}
}  // namespace xe::kernel
