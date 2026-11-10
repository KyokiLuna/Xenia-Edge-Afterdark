/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_FRAME_EXECUTION_PROBE_H_
#define XENIA_KERNEL_NATIVE_FRAME_EXECUTION_PROBE_H_
#include <filesystem>
#include <memory>
namespace xe::kernel {
class KernelState;
class NativeFrameOwnerSession;
struct NativeCompletedFrameRecord;
// Explicit opt-in only. Executes one verified frozen native record at a title
// swap boundary, then requires its exact real fence before owner retirement.
// Submission does not establish image correctness, composition or input.
bool RunNativeFrameExecutionProbe(
    KernelState* kernel, std::shared_ptr<NativeFrameOwnerSession> session,
    const NativeCompletedFrameRecord& record,
    const std::filesystem::path& capture_path);
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_FRAME_EXECUTION_PROBE_H_
