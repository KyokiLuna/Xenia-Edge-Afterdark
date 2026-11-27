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
#include <optional>
#include <string>
#include <utility>
#include "xenia/base/native_ui_backend_route.h"
#include "xenia/base/threading.h"
#include "xenia/ui/text_entry.h"
#include "xenia/xbox.h"
namespace xe::ui {
class WindowedAppContext;
}
namespace xe::hid {
class InputSystem;
class UiInputReservation;
}  // namespace xe::hid
namespace xe::kernel { class KernelState; }
namespace xe::kernel::xam {
// Value-only request choices. Zero is never a profile identity and mask bits
// select explicit slots; this confers no durable Account/native authority.
struct HostUiProfileSelection final {
  std::array<uint64_t, 4> xuids{};
  uint32_t mask = 0;
  bool valid() const;
};
// One existing guest-scheduler host-call ticket. It owns the WHOLE UI wait,
// publish and completion, rather than blocking Kernel Dispatch on another FIFO.
// Scheduler workers/drop settlement are joined before XamState reclamation.
// The body releases dialog/input/ingress holders before guest event/APC writes.
class HostUiFallbackWork final {
 public:
  HostUiFallbackWork() = default;
  // Allocate the ticket and complete callback BEFORE moving caller cleanup.
  // No failure can strand a transferred cancel/reservation before queue commit.
  void Initialize(std::function<X_RESULT()>&& work,
      std::function<void()>&& cancel, std::function<void(X_RESULT)>&& complete,
      std::function<void()>&& post) noexcept {
    work_ = std::move(work); cancel_ = std::move(cancel);
    complete_ = std::move(complete); post_ = std::move(post);
  }
  void Run();
  void Reject(X_RESULT);
 private:
  void Finish(X_RESULT);
  std::atomic<bool> claimed_{false};
  std::function<X_RESULT()> work_;
  std::function<void()> cancel_;
  std::function<void(X_RESULT)> complete_;
  std::function<void()> post_;
};
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
// Kernel-owned worker reservation only. NEVER capture this in a queued UI
// factory. The existing guest-scheduler host worker/drop join destroys run+post
// before XamState reclamation. InputSystem is used only by the original constructor;
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
    HostUiProfileSelection signin;
  };
  // Frontend and worker exchange only copied choices/status, never a Kernel,
  // PM, native request, callback, or transferable HostLease::Entry.
  enum class SigninPhase : uint8_t { kNone, kQueued, kExecuting, kReturned };
  struct SigninAttempt {
    uint64_t id = 0;
    SigninPhase phase = SigninPhase::kNone;
    HostUiProfileSelection selection;
    X_RESULT status = X_ERROR_IO_PENDING;
    bool executed = false;
  };
  uint64_t StartSigninAttempt(HostUiProfileSelection);
  SigninAttempt signin_attempt() const;
  std::optional<SigninAttempt> TakeSigninAttempt();
  bool CompleteSigninAttempt(uint64_t, X_RESULT, bool executed);
  bool closed_requested() const;
  void Finish(Result);
  Result result() const;
  threading::Fence closed;
  // One worker waiter only. Signaled by a new attempt or final UI/stop close;
  // result arrival is sampled by the existing active dialog drawing frames.
  threading::Fence progress;
  std::atomic<bool> admitted{false};

 private:
  mutable std::mutex mutex_;
  bool done_ = false;
  Result result_;
  uint64_t next_signin_attempt_ = 1;
  SigninAttempt signin_attempt_;
};
// Synchronous body on the already admitted host worker. Its callback stays on
// that worker's stack; the shared frontend CPU model never retains it.
X_RESULT RunHostSigninAttempts(
    const std::shared_ptr<NativeUiBackendRoute::HostLease>&,
    const std::shared_ptr<HostUiFallbackCompletion>&,
    const std::function<X_RESULT(const HostUiProfileSelection&,
                                const std::function<bool()>&)>& publish);
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
