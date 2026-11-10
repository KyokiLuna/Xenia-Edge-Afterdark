/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_keyboard.h"

#include <chrono>
#include <limits>

#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/imgui_text_entry_dialog.h"
#include "xenia/ui/window.h"

namespace xe::kernel::xam {
namespace {
// A queued request may be discarded during shutdown before run is called.
// Release exactly once so a later request's UI ownership cannot be cleared by
// the destruction of a completed dispatch closure.
class KeyboardUiReservation {
 public:
  explicit KeyboardUiReservation(XamState* state) : state_(state) {}
  ~KeyboardUiReservation() { Release(); }
  void Release() {
    if (!released_.exchange(true)) {
      state_->is_xam_dialog_present_.store(false);
    }
  }

 private:
  XamState* state_;
  std::atomic<bool> released_{false};
};
}  // namespace

bool WriteKeyboardText(std::u16string_view text,
                       std::span<xe::be<uint16_t>> buffer) {
  if (text.size() >= buffer.size()) {
    return false;
  }
  for (size_t i = 0; i < text.size(); ++i) {
    buffer[i] = uint16_t(text[i]);
  }
  buffer[text.size()] = 0;
  return true;
}

KeyboardUiCompletion CompleteKeyboardText(xe::ui::TextEntryResult result,
                                          std::u16string_view text,
                                          std::span<xe::be<uint16_t>> buffer) {
  if (result == xe::ui::TextEntryResult::kAborted) {
    return {X_ERROR_CANCELLED, X_ERROR_CANCELLED, 0};
  }
  if (result == xe::ui::TextEntryResult::kCanceled) {
    return {X_ERROR_FUNCTION_FAILED, X_ERROR_CANCELLED, 0};
  }
  // Native VK explicitly skips copyback at capacity zero.
  if (!buffer.empty() && !WriteKeyboardText(text, buffer)) {
    return {X_ERROR_INVALID_PARAMETER, X_ERROR_INVALID_PARAMETER, 0};
  }
  return {X_ERROR_SUCCESS, X_ERROR_SUCCESS, 0};
}

X_RESULT ShowKeyboardUI(
    uint32_t user_index, uint32_t flags, std::u16string default_text,
    std::u16string title, std::u16string description, uint32_t buffer,
    uint32_t buffer_length, uint32_t overlapped,
    std::shared_ptr<NativeUiBackendRoute::HostLease> host_route) {
  // This shared host UI currently provides ordinary single-line entry. The
  // native user/style flag-specific scene behaviors remain outside its scope;
  // do not invent a console validation mask for the other request values.
  auto* state = kernel_state();
  if (buffer_length > std::numeric_limits<uint32_t>::max() / sizeof(char16_t) ||
      (buffer_length && (!buffer || state->memory()->QueryRangeAccess(
                                        buffer, buffer_length * 2) !=
                                        xe::memory::PageAccess::kReadWrite))) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (overlapped &&
      state->memory()->QueryRangeAccess(overlapped, sizeof(XAM_OVERLAPPED)) !=
          xe::memory::PageAccess::kReadWrite) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* window = state->emulator()->display_window();
  auto* drawer = state->emulator()->imgui_drawer();
  if (!window || !drawer) {
    return X_ERROR_FUNCTION_FAILED;
  }
  auto* xam = state->xam_state();
  bool expected = false;
  if (!xam->is_xam_dialog_present_.compare_exchange_strong(expected, true)) {
    return X_ERROR_ACCESS_DENIED;
  }
  if (host_route) {
    auto selected_reservation = std::make_shared<HostUiFallbackReservation>(
        xam, state->emulator()->input_system());
    if (!selected_reservation->input_gap()) {
      return X_ERROR_NOT_READY;
    }
    auto completion = std::make_shared<HostUiFallbackCompletion>();
    auto pre = [state, host_route] {
      if (!host_route->cancelled()) {
        state->BroadcastNotification(kXNotificationSystemUI, true);
      }
    };
    auto run = [state, selected_reservation, host_route, completion, window,
                drawer, buffer, buffer_length,
                default_text = std::move(default_text),
                title = std::move(title), description = std::move(description)](
                   uint32_t& extended_error, uint32_t& length) -> X_RESULT {
      constexpr size_t kNoCopyEditingBudget = 256;
      const size_t max_units =
          buffer_length ? buffer_length - 1 : kNoCopyEditingBudget;
      auto* input = state->emulator()->input_system();
      const auto gap = selected_reservation->input_gap();
      QueueHostUiFallback(
          window->app_context(), host_route, completion,
          [host_route, gap, input, window, drawer, max_units, default_text,
           title, description](const auto& completed) {
            const auto finish = [completed, host_route](
                                    xe::ui::TextEntryResult result,
                                    const std::u16string& text) {
              completed->Finish({result, text, 0});
            };
            auto dialog = std::make_unique<xe::ui::ImGuiTextEntryDialog>(
                drawer, input, xe::to_utf8_lossy(title),
                xe::to_utf8_lossy(description), default_text, max_units, finish,
                finish);
            dialog->SetInputOwnershipCallback(
                [window] { return window->HasFocus(); });
            completed->admitted.store(true);
            if (gap) {
              gap->Release();
            }
            dialog.release();
          });
      GuestScheduler::WaitOnFence(completion->closed);
      const auto value = completion->result();
      selected_reservation->Release();
      length = 0;
      if (value.result != xe::ui::TextEntryResult::kAccepted) {
        const auto result = CompleteKeyboardText(value.result, {}, {});
        extended_error = result.extended_error;
        return result.result;
      }
      auto memory_lock = global_critical_region::AcquireDirect();
      if (buffer_length &&
          state->memory()->QueryRangeAccess(buffer, buffer_length * 2) !=
              memory::PageAccess::kReadWrite) {
        extended_error = X_ERROR_INVALID_PARAMETER;
        return X_ERROR_INVALID_PARAMETER;
      }
      auto* output =
          buffer_length
              ? state->memory()->TranslateVirtual<xe::be<uint16_t>*>(buffer)
              : nullptr;
      const auto result = CompleteKeyboardText(value.result, value.text,
                                               {output, buffer_length});
      extended_error = result.extended_error;
      return result.result;
    };
    auto post = [state, host_route] {
      xe::threading::Sleep(std::chrono::milliseconds(200));
      state->BroadcastNotification(kXNotificationSystemUI, false);
    };
    if (overlapped) {
      state->CompleteOverlappedDeferredEx(std::move(run), overlapped,
                                          std::move(pre), std::move(post));
      return X_ERROR_IO_PENDING;
    }
    pre();
    uint32_t extended = X_ERROR_CANCELLED, length = 0;
    const auto result = run(extended, length);
    post();
    return completion->admitted.load() ? X_ERROR_IO_PENDING : result;
  }

  auto reservation = std::make_shared<KeyboardUiReservation>(xam);
  auto admitted = std::make_shared<std::atomic<bool>>(false);

  // Keep the existing host fallback's message/notification route. This does
  // not load a native dashboard keyboard scene or a guest XAM task.
  auto pre = [state] {
    state->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto run = [state, reservation, admitted, window, drawer, buffer,
              buffer_length, default_text = std::move(default_text),
              title = std::move(title), description = std::move(description)](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    struct Completion {
      xe::ui::TextEntryResult result = xe::ui::TextEntryResult::kAborted;
      std::u16string text;
      xe::threading::Fence closed;
    };
    auto completion = std::make_shared<Completion>();
    // Construction, callback installation and waiter registration are all on
    // the UI thread. A first draw can no longer delete an unarmed request.
    const bool dispatched =
        window->app_context().CallInUIThreadSynchronous([&] {
          const auto finish = [completion](xe::ui::TextEntryResult result,
                                           const std::u16string& value) {
            completion->result = result;
            completion->text = value;
            completion->closed.Signal();
          };
          // No-copy requests use an explicit host-only editing budget. The
          // native zero-capacity scene's editing limit is not reconstructed.
          constexpr size_t kNoCopyEditingBudget = 256;
          const size_t max_units =
              buffer_length ? buffer_length - 1 : kNoCopyEditingBudget;
          auto* dialog = new xe::ui::ImGuiTextEntryDialog(
              drawer, state->emulator()->input_system(),
              xe::to_utf8_lossy(title), xe::to_utf8_lossy(description),
              default_text, max_units, finish, finish);
          dialog->SetInputOwnershipCallback(
              [window] { return window->HasFocus(); });
        });
    admitted->store(dispatched);
    if (dispatched) {
      GuestScheduler::WaitOnFence(completion->closed);
    }
    reservation->Release();
    length = 0;
    if (!dispatched) {
      extended_error = X_ERROR_CANCELLED;
      return X_ERROR_CANCELLED;
    }
    if (completion->result != xe::ui::TextEntryResult::kAccepted) {
      // User cancellation and forced abort use distinct native result fields;
      // neither path writes the caller's text buffer.
      const auto result = CompleteKeyboardText(completion->result, {}, {});
      extended_error = result.extended_error;
      length = result.length;
      return result.result;
    }
    auto* output =
        buffer_length
            ? state->memory()->TranslateVirtual<xe::be<uint16_t>*>(buffer)
            : nullptr;
    if (buffer_length &&
        state->memory()->QueryRangeAccess(buffer, buffer_length * 2) !=
            xe::memory::PageAccess::kReadWrite) {
      extended_error = X_ERROR_INVALID_PARAMETER;
      return X_ERROR_INVALID_PARAMETER;
    }
    const auto result = CompleteKeyboardText(
        completion->result, completion->text, {output, buffer_length});
    extended_error = result.extended_error;
    length = result.length;
    return result.result;
  };
  auto post = [state] {
    xe::threading::Sleep(std::chrono::milliseconds(200));
    state->BroadcastNotification(kXNotificationSystemUI, false);
  };
  if (overlapped) {
    state->CompleteOverlappedDeferredEx(std::move(run), overlapped,
                                        std::move(pre), std::move(post));
    return X_ERROR_IO_PENDING;
  }
  pre();
  uint32_t extended_error = X_ERROR_CANCELLED, length = 0;
  const X_RESULT result = run(extended_error, length);
  post();
  // NULL overlap waits internally, but the native outer export still maps
  // accepted scheduling to 997, even after user cancellation.
  return admitted->load() ? X_ERROR_IO_PENDING : result;
}

}  // namespace xe::kernel::xam
