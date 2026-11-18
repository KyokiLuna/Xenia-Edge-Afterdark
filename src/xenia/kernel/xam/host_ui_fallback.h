/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_XAM_HOST_UI_FALLBACK_H_
#define XENIA_KERNEL_XAM_HOST_UI_FALLBACK_H_
#include <atomic>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include "xenia/base/native_ui_backend_route.h"
#include "xenia/base/threading.h"
#include "xenia/ui/text_entry.h"
namespace xe::ui {
class WindowedAppContext;
}
namespace xe::hid {
class InputSystem;
class UiInputReservation;
}  // namespace xe::hid
namespace xe::kernel { class KernelState; }
namespace xe::kernel::xam {
// Kernel-owned access metadata. Queued factories hold only a weak handle, and
// may lock/dereference it only inside that Kernel's active HostLease::Entry.
// Dialogs never retain it or the Kernel; every draw has a fresh lease entry.
class HostUiFallbackAccess final {
 public:
  explicit HostUiFallbackAccess(KernelState* kernel) : kernel_(kernel) {}
  KernelState* kernel() const { return kernel_; }
 private:
  KernelState* const kernel_;
};
class XamState;
// Kernel-dispatch-owned reservation only. NEVER capture this in a queued UI
// factory. Existing Kernel dispatch shutdown joins/destroys run+post before
// XamState reclamation. InputSystem is used only by the original constructor;
// tokens remain safe even after its reclamation. The blocker covers queue gaps.
class HostUiFallbackReservation final {
 public:
  HostUiFallbackReservation(XamState*, hid::InputSystem*,
                            bool host_only = false);
  ~HostUiFallbackReservation();
  static std::shared_ptr<HostUiFallbackReservation> TryCreate(
      XamState*, hid::InputSystem*, bool host_only = false) noexcept;
  void Release();
  const std::shared_ptr<hid::UiInputReservation>& input_gap() const {
    return input_gap_;
  }

 private:
  XamState* xam_;
  std::shared_ptr<hid::UiInputReservation> input_gap_;
  std::atomic<bool> released_{false};
};
// A single existing host dialog's CPU result, not another UI manager. Nothing
// here retains Kernel/Task/provider/output pointers. A canceled pending UI
// closure may remain in the app queue after Kernel teardown, inert.
class HostUiFallbackCompletion final {
 public:
  struct Result {
    xe::ui::TextEntryResult result = xe::ui::TextEntryResult::kAborted;
    std::u16string text;
    uint32_t button = 0;
    uint32_t status = 0;
    std::array<uint16_t, 4> passcode{};
    std::filesystem::path path;
  };
  void Finish(Result);
  Result result() const;
  threading::Fence closed;
  std::atomic<bool> admitted{false};

 private:
  mutable std::mutex mutex_;
  bool done_ = false;
  Result result_;
};
// A selected-host run owns this epilogue BEFORE its local Entry/GCRC guards.
// Destruction follows their release but precedes returning the result to
// CompleteOverlappedEx/event/APC publication. Post is notification delay only.
class HostUiFallbackOperationCompletion final {
 public:
  explicit HostUiFallbackOperationCompletion(
      std::shared_ptr<NativeUiBackendRoute::HostLease> lease) noexcept
      : lease_(std::move(lease)) {}
  ~HostUiFallbackOperationCompletion();
  HostUiFallbackOperationCompletion(const HostUiFallbackOperationCompletion&) = delete;
  HostUiFallbackOperationCompletion& operator=(const HostUiFallbackOperationCompletion&) = delete;
 private:
  const std::shared_ptr<NativeUiBackendRoute::HostLease> lease_;
};
// Factory captures owned values and frontend pointers ONLY. Its first body
// executes only after exact lease entry. Never borrow the waiting run's stack.
bool QueueHostUiFallback(
    xe::ui::WindowedAppContext&,
    const std::shared_ptr<NativeUiBackendRoute::HostLease>&,
    const std::shared_ptr<HostUiFallbackCompletion>&,
    std::function<void(const std::shared_ptr<HostUiFallbackCompletion>&)>
        factory);
}  // namespace xe::kernel::xam
#endif
