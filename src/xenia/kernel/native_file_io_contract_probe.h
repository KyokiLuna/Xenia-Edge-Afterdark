/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_NATIVE_FILE_IO_CONTRACT_PROBE_H_
#define XENIA_KERNEL_NATIVE_FILE_IO_CONTRACT_PROBE_H_
#include <filesystem>
#include <memory>
#include "xenia/kernel/native_system_ui_service.h"

namespace xe::kernel {
// Explicit diagnostic decorator, never a production readiness provider.
// Retains fixture handles through actual queue drain. APC-visible code/result
// and IOSB/output memory transfer to Kernel's all-CPU-stopped lifetime boundary.
std::shared_ptr<NativeSystemUiDiagnosticObservation> WrapFileIoContractProbe(
    KernelState*, std::shared_ptr<NativeSystemUiDiagnosticObservation>,
    const std::filesystem::path& exclusive_report);
}  // namespace xe::kernel
#endif
