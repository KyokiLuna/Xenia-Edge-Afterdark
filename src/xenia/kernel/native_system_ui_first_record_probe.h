/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_UI_FIRST_RECORD_PROBE_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_UI_FIRST_RECORD_PROBE_H_

#include <filesystem>
#include <memory>
#include "xenia/kernel/native_system_ui_service.h"

namespace xe::kernel {
// Explicit private first-record diagnostic. Native XAM constructs its own
// Tasks, FE, UI manager and Guide. GPU submission remains disabled; this cannot
// satisfy production Prepared or Guide/keyboard/input acceptance.
class NativeSystemUiFirstRecordProbe final
    : public NativeSystemUiDiagnosticObservation {
 public:
  static std::shared_ptr<NativeSystemUiFirstRecordProbe> Create(
      KernelState*, const std::filesystem::path& exclusive_report_path);
  ~NativeSystemUiFirstRecordProbe() override;
  GuestRuntime::ExecutionResult PrepareBeforeWorkers(
      const NativeSystemUiServiceContext&, XThread&) override;
  gpu::GpuBackendStopProof backend_stop_proof() const override;
  GuestRuntime::ExecutionResult ObserveNativePhase(
      NativeSystemUiStartupPhase, const NativeSystemUiServiceContext&,
      XThread&) override;
  GuestRuntime::ExecutionResult ObserveAfterNativeBoot(
      const NativeSystemUiServiceContext&, XThread&) override;
  void CloseExternalRequests() noexcept override;
  void BeginStopTransport() noexcept override;
  bool DrainCallbacksBeforeCpuStop() noexcept override;
  bool RetireAfterKernelStop() noexcept override;

 private:
  struct State;
  explicit NativeSystemUiFirstRecordProbe(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif
