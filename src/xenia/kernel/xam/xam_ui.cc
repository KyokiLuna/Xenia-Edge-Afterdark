/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/base/xui_resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <string_view>
#include <system_error>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "third_party/utfcpp/source/utf8.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/mutex.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#include "xenia/base/utf8.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/util/host_call_outcome.h"
#include <type_traits>
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_keyboard.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_guest_notification.h"

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/kernel/xam/ui/disc_swap_ui.h"
#include "xenia/kernel/xam/ui/game_achievements_ui.h"
#include "xenia/kernel/xam/ui/gamercard_ui.h"
#include "xenia/kernel/xam/ui/passcode_ui.h"
#include "xenia/kernel/xam/ui/signin_ui.h"
#include "xenia/kernel/xam/ui/title_info_ui.h"

DEFINE_transient_bool(storage_selection_dialog, false,
                      "Deprecated compatibility option; storage selection is "
                      "automatic unless the request requires UI.",
                      "UI");

DECLARE_int32(license_mask);

constexpr std::chrono::milliseconds kUIDelayMillis(200);

namespace xe {
namespace kernel {
namespace xam {
// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).
using HostDialogResult = HostUiFallbackCompletion::Result;
// Synchronous diagnostic output only. Never captured by queued host work.
struct NativeSigninUiDiagnostic { uint32_t route=0,stage=0,native_owned=0; };
// Original selected lease serializes this flag claim. Transfer exactly once
// before TryCreate (which already owns all success/failure/OOM flag cleanup).
struct NativeSigninHostFlagGuard {
  XamState* state=nullptr;bool owned=false;
  ~NativeSigninHostFlagGuard(){if(owned)state->is_xam_dialog_present_.store(false);}
  void Release() noexcept {owned=false;}
};

// Called on the original export caller. Reserve input BEFORE queuing UI and
// retain XamState only in joined Kernel host work, never in a frontend closure.
template <typename T>
X_RESULT xeXamDispatchDialog(
    std::function<T*(HostUiFallbackAccess&)> factory,
    std::function<HostDialogResult(T*)> snapshot,
    std::function<X_RESULT(const HostDialogResult&)> publish,
    uint32_t overlapped = 0, bool asynchronous = false,
    KernelState* original = nullptr,
    std::shared_ptr<NativeUiBackendRoute::HostLease> admitted = {},
    NativeSigninUiDiagnostic* diagnostic = nullptr,
    bool selected_native_host = false, NativeSigninHostFlagGuard* host_flag = nullptr,
    std::function<X_RESULT(const HostUiProfileSelection&,
                          const std::function<bool()>&)> signin_publish = {}) {
  auto* state = original ? original : kernel_state();
  auto* emulator = state->emulator();
  auto* window = emulator->display_window();
  if (!window || !emulator->imgui_drawer() || !emulator->input_system()) {
    if (admitted) {
      if(host_flag)host_flag->Release();
      if(selected_native_host)admitted->FinishHostOperation();
      else admitted->FinishHostOnly();
      state->xam_state()->is_xam_dialog_present_.store(false);
    }
    if(diagnostic)diagnostic->stage=5;
    return X_ERROR_NOT_READY;
  }
  auto lease = admitted ? std::move(admitted) : state->AcquireLegacyHostUi();
  if (!lease) { if(diagnostic)diagnostic->stage=6; return X_ERROR_ACCESS_DENIED; }
  // The opaque selected lease owns the Kernel ingress. Its one completion
  // guard transfers to joined host work, never to a queued frontend factory.
  auto selected_completion=selected_native_host
      ?std::make_shared<HostUiFallbackOperationCompletion>(lease):nullptr;
  if(host_flag)host_flag->Release();
  auto reservation = HostUiFallbackReservation::TryCreate(
      state->xam_state(), emulator->input_system(), !selected_native_host);
  if (!reservation || !reservation->input_gap()) { if(diagnostic)diagnostic->stage=7; return X_ERROR_NOT_READY; }
  if(diagnostic)diagnostic->stage=8;
  auto completion = std::make_shared<HostUiFallbackCompletion>();
  const auto access = state->host_ui_access();
  // The pure CPU input gap remains owned by joined host work through publication.
  QueueHostUiFallback(
      window->app_context(), lease, completion,
      [lease, access, factory = std::move(factory), snapshot = std::move(snapshot)](
          const std::shared_ptr<HostUiFallbackCompletion>& completed) mutable {
        auto owner = access.lock();
        if (!owner) { completed->Finish({}); return; }
        T* dialog = factory(*owner);  // QueueHostUiFallback already entered.
        if (!dialog) { completed->Finish({}); return; }
        dialog->SetHostLease(lease);
        dialog->SetHostCompletion(completed);
        dialog->set_close_callback([dialog, completed, snapshot = std::move(snapshot)] {
          completed->Finish(snapshot(dialog));
        });
        completed->admitted.store(true);
      });
  state->BroadcastNotification(kXNotificationSystemUI, true);
  auto run = [lease, reservation, completion, selected_native_host,
              selected_completion=std::move(selected_completion),
              publish = std::move(publish),
              signin_publish = std::move(signin_publish)]() mutable {
    // Destroy after Entry/result scopes and reservation release but BEFORE
    // the worker publishes a guest completion; old post/factory refs are inert.
    auto finished=std::move(selected_completion);
    X_RESULT result = X_ERROR_CANCELLED;
    if constexpr(std::is_same_v<T,ui::SigninUI>){
      result=RunHostSigninAttempts(lease,completion,signin_publish);
    }else{
      GuestScheduler::WaitOnFence(completion->closed);
      auto entered = lease->TryEnter();
      const auto value = completion->result();
      if (entered && value.result == xe::ui::TextEntryResult::kAccepted)
        result = publish ? publish(value) : value.status;
    }
    if(!selected_native_host)lease->FinishHostOnly();
    reservation->Release();
    return result;
  };
  auto post = [state] {
    // The existing host worker/drop join owns this delay and raw state.
    // No detached thread outlives Kernel or reaches a replacement.
    xe::threading::Sleep(kUIDelayMillis);
    if (!state->xam_state()->IsUIActive()) {
      state->BroadcastNotification(kXNotificationSystemUI, false);
    }
  };
  if (overlapped || asynchronous) {
    X_RESULT queued=X_ERROR_NOT_ENOUGH_MEMORY;
    const auto submitted=InvokeHostCall([&] {
      queued=state->QueueHostUiCompletion(std::move(run),
          [lease, reservation, selected_native_host] {
            lease->Cancel();reservation->Release();
            if(selected_native_host)lease->FinishHostOperation();else lease->FinishHostOnly();
          }, overlapped, std::move(post));
    });
    if(submitted!=HostCallOutcome::kReturned){
      // Includes std::function conversion BEFORE adapter entry. No pending
      // overlap was admitted there; cancel this exact inert frontend request.
      lease->Cancel();reservation->Release();
      if(selected_native_host)lease->FinishHostOperation();else lease->FinishHostOnly();
      if(!state->xam_state()->IsUIActive())state->BroadcastNotification(kXNotificationSystemUI,false);
      return X_ERROR_NOT_ENOUGH_MEMORY;
    }
    return queued;
  }
  const auto result = run();
  post();
  return result;
}

static HostDialogResult SnapshotMessageBox(MessageBoxDialog* dialog) {
  HostDialogResult value;
  value.result = dialog->accepted() ? xe::ui::TextEntryResult::kAccepted
                                    : xe::ui::TextEntryResult::kCanceled;
  value.button = dialog->chosen_button();
  return value;
}

X_RESULT xeXamDispatchWithoutDialog(std::function<X_RESULT()> callback,
                                    uint32_t overlapped) {
  auto* state = kernel_state();
  auto lease = state->AcquireLegacyHostUi();
  if (!lease) { return X_ERROR_ACCESS_DENIED; }
  auto reservation = HostUiFallbackReservation::TryCreate(
      state->xam_state(), state->emulator()->input_system(), true);
  if (!reservation || !reservation->input_gap()) { return X_ERROR_NOT_READY; }
  auto run = [lease, reservation, callback = std::move(callback)]() -> X_RESULT {
    X_RESULT result = X_ERROR_CANCELLED;
    if (auto entered = lease->TryEnter()) { result = callback(); }
    lease->FinishHostOnly();
    reservation->Release();
    return result;
  };
  auto pre = [state] {
    state->BroadcastNotification(kXNotificationSystemUI, true);
    xe::threading::Sleep(std::chrono::milliseconds(25));
  };
  auto post = [state] {
    xe::threading::Sleep(kUIDelayMillis);
    if (!state->xam_state()->IsUIActive()) {
      state->BroadcastNotification(kXNotificationSystemUI, false);
    }
  };
  if (overlapped) {
    state->CompleteOverlappedDeferred(std::move(run), overlapped,
                                       std::move(pre), std::move(post));
    return X_ERROR_IO_PENDING;
  }
  pre();
  const auto result = run();
  post();
  return result;
}

void MessageBoxDialog::OnDrawDialog(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
  }

  // Center every frame, a title can open one while the window is still being
  // resized at launch.
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f),
                           ImGuiCond_Always);

  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Handle keyboard escape or gamepad B/Back to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      chosen_button_ = default_button_;
      ImGui::CloseCurrentPopup();
      Close();
      ImGui::EndPopup();
      return;
    }

    if (description_.size()) {
      ImGui::TextWrapped("%s", description_.c_str());
    }

    // Buttons - ImGui handles navigation, A button activates via ImGui
    for (size_t i = 0; i < buttons_.size(); ++i) {
      if (ImGui::Button(buttons_[i].c_str())) {
        chosen_button_ = static_cast<uint32_t>(i);
        accepted_ = true;
        ImGui::CloseCurrentPopup();
        Close();
      }
      // Set default focus on the default button
      if (i == default_button_) {
        ImGui::SetItemDefaultFocus();
      }
      ImGui::SameLine();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::EndPopup();
  } else {
    Close();
  }
}

static dword_result_t XamShowMessageBoxUi(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped,
    std::shared_ptr<NativeUiBackendRoute::HostLease> host_route = {}) {
  std::string title = title_ptr ? xe::to_utf8(title_ptr.value()) : "";
  std::string text = text_ptr ? xe::to_utf8(text_ptr.value()) : "";

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = xe::load_and_swap<std::u16string>(
        kernel_state()->memory()->TranslateVirtual(button_ptr));

    if (!button.empty()) {
      buttons.push_back(xe::to_utf8(button));
    }
  }

  if (buttons.empty()) {
    buttons.push_back("OK");
  }

  X_RESULT result;
  switch (flags & 0xF) {
    case XMBox_NOICON: {
    } break;
    case XMBox_ERRORICON: {
    } break;
    case XMBox_WARNINGICON: {
    } break;
    case XMBox_ALERTICON: {
    } break;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  if (host_route) {
    // The selected host cut is the existing ordinary message-box behavior.
    // Profile/passcode scenes require separate native/profile contracts.
    if (flags & (XMBox_PASSCODEMODE | XMBox_VERIFYPASSCODEMODE)) {
      return X_ERROR_NOT_SUPPORTED;
    }
    auto* state = kernel_state();
    if (!result_ptr ||
        state->memory()->QueryRangeAccess(result_ptr.guest_address(),
                                          sizeof(MESSAGEBOX_RESULT)) !=
            memory::PageAccess::kReadWrite ||
        (overlapped &&
         state->memory()->QueryRangeAccess(overlapped.guest_address(),
                                           sizeof(XAM_OVERLAPPED)) !=
             memory::PageAccess::kReadWrite)) {
      return X_ERROR_INVALID_PARAMETER;
    }
    auto* window = state->emulator()->display_window();
    auto* drawer = state->emulator()->imgui_drawer();
    auto* input = state->emulator()->input_system();
    if (!window || !drawer || !input) {
      return X_ERROR_FUNCTION_FAILED;
    }
    bool expected = false;
    auto* xam = state->xam_state();
    if (!xam->is_xam_dialog_present_.compare_exchange_strong(expected, true)) {
      return X_ERROR_ACCESS_DENIED;
    }
    auto reservation = HostUiFallbackReservation::TryCreate(xam, input);
    if (!reservation || !reservation->input_gap()) {
      return X_ERROR_NOT_READY;
    }
    auto completion = std::make_shared<HostUiFallbackCompletion>();
    const uint32_t result_address = result_ptr.guest_address();
    auto pre = [state, host_route] {
      if (!host_route->cancelled()) {
        state->BroadcastNotification(kXNotificationSystemUI, true);
      }
    };
    auto run = [state, reservation, host_route, completion, window, drawer,
                input, title, text, buttons, active = uint32_t(active_button),
                result_address](uint32_t& extended,
                                uint32_t& length) -> X_RESULT {
      HostUiFallbackOperationCompletion operation_completion(host_route);
      const auto gap = reservation->input_gap();
      QueueHostUiFallback(
          window->app_context(), host_route, completion,
          [host_route, gap, drawer, input, title, text, buttons,
           active](const auto& completed) {
            auto dialog = std::make_unique<MessageBoxDialog>(
                drawer, input, title, text, buttons, active);
            dialog->set_close_callback(
                [raw = dialog.get(), completed, host_route] {
                  completed->Finish(SnapshotMessageBox(raw));
                });
            completed->admitted.store(true);
            if (gap) {
              gap->Release();
            }
            dialog.release();
          });
      GuestScheduler::WaitOnFence(completion->closed);
      const auto value = completion->result();
      reservation->Release();
      extended = X_ERROR_CANCELLED;
      length = 0;
      auto entered = host_route->TryEnter();
      if (!entered || value.result != xe::ui::TextEntryResult::kAccepted) {
        return X_ERROR_CANCELLED;
      }
      auto memory_lock = global_critical_region::AcquireDirect();
      if (state->memory()->QueryRangeAccess(result_address,
                                            sizeof(MESSAGEBOX_RESULT)) !=
          memory::PageAccess::kReadWrite) {
        extended = X_ERROR_INVALID_PARAMETER;
        return X_ERROR_INVALID_PARAMETER;
      }
      state->memory()
          ->TranslateVirtual<MESSAGEBOX_RESULT*>(result_address)
          ->ButtonPressed = value.button;
      extended = X_ERROR_SUCCESS;
      return X_ERROR_SUCCESS;
    };
    auto post = [state] {
      xe::threading::Sleep(kUIDelayMillis);
      if (!state->xam_state()->IsUIActive()) {
        state->BroadcastNotification(kXNotificationSystemUI, false);
      }
    };
    if (overlapped) {
      state->CompleteOverlappedDeferredEx(std::move(run),
                                          overlapped.guest_address(),
                                          std::move(pre), std::move(post));
      return X_ERROR_IO_PENDING;
    }
    pre();
    uint32_t extended = X_ERROR_CANCELLED, length = 0;
    const auto result = run(extended, length);
    post();
    return result;
  }

  auto* drawer = kernel_state()->emulator()->imgui_drawer();
  auto* input = kernel_state()->emulator()->input_system();
  if (flags & (XMBox_PASSCODEMODE | XMBox_VERIFYPASSCODEMODE)) {
    result = xeXamDispatchDialog<ui::ProfilePasscodeUI>(
        [drawer, title, text](HostUiFallbackAccess&) {
          return new ui::ProfilePasscodeUI(drawer, title, text);
        }, [](ui::ProfilePasscodeUI* dialog) {
          HostDialogResult value;
          if (dialog->SelectedSignedIn()) {
            value.result = xe::ui::TextEntryResult::kAccepted;
            value.passcode = dialog->passcode();
          }
          return value;
        }, [state = kernel_state(), result_ptr](const HostDialogResult& value) {
          auto memory_lock = global_critical_region::AcquireDirect();
          if (!result_ptr || state->memory()->QueryRangeAccess(
                  result_ptr.guest_address(), sizeof(MESSAGEBOX_RESULT)) !=
                  memory::PageAccess::kReadWrite) { return X_ERROR_INVALID_PARAMETER; }
          result_ptr.Zero();
          for (size_t i = 0; i < value.passcode.size(); ++i) {
            result_ptr->Passcode[i] = value.passcode[i];
          }
          return X_ERROR_SUCCESS;
        }, overlapped);
  } else {
    result = xeXamDispatchDialog<MessageBoxDialog>(
        [drawer, input, title, text, buttons, active = uint32_t(active_button)](
            HostUiFallbackAccess&) {
          return new MessageBoxDialog(drawer, input, title, text, buttons, active);
        }, SnapshotMessageBox,
        [state = kernel_state(), result_ptr](const HostDialogResult& value) {
          auto memory_lock = global_critical_region::AcquireDirect();
          if (!result_ptr || state->memory()->QueryRangeAccess(
                  result_ptr.guest_address(), sizeof(MESSAGEBOX_RESULT)) !=
                  memory::PageAccess::kReadWrite) { return X_ERROR_INVALID_PARAMETER; }
          result_ptr->ButtonPressed = value.button;
          return X_ERROR_SUCCESS;
        }, overlapped);
  }

  return result;
}

dword_result_t XamIsUIActive_entry() {
  const auto native = NativeSystemUiService::TryInvokePublicOnOriginalCaller(
      kernel_state(), NativeSystemUiPublicCall::kIsUiActive, {});
  const bool host_dialog = kernel_state()->xam_state()->IsUIActive();
  if (!native.handled()) {
    return host_dialog;
  }
  const auto* input = kernel_state()->emulator()->input_system();
  if (host_dialog || (input && input->IsUIInputBlocked())) {
    return 1;
  }
  if (native.disposition !=
      NativeSystemUiPublicCallResult::Disposition::kReturned) {
    // BOOL has no error channel. An admitted native service with unknown UI
    // state must not let the title treat an active UI as closed.
    return 1;
  }
  return native.value;
}
DECLARE_XAM_EXPORT2(XamIsUIActive, kUI, kImplemented, kHighFrequency);

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
dword_result_t XamShowMessageBoxUI_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  const std::array<uint32_t, 9> arguments{
      user_index,   title_ptr.guest_address(),   text_ptr.guest_address(),
      button_count, button_ptrs.guest_address(), active_button,
      flags,        result_ptr.guest_address(),  overlapped.guest_address()};
  const auto native = NativeSystemUiService::TryInvokePublicOnOriginalCaller(
      kernel_state(), NativeSystemUiPublicCall::kShowMessageBox, arguments);
  if (native.handled()) {
    return native.value;
  }
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped, native.host_fallback);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUI, kUI, kImplemented);

dword_result_t XamShowMessageBoxUIEx_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, dword_t unknown_unused,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUIEx, kUI, kImplemented);

// XamShowMessageBoxUI with the tracking ID of a Kinect skeleton first.
// TODO(knuckleslee): Selecting the buttons with Kinect is not supported.
dword_result_t XamShowNuiMessageBoxUI_entry(
    dword_t tracking_id, dword_t user_index, lpu16string_t title_ptr,
    lpu16string_t text_ptr, dword_t button_count, lpdword_t button_ptrs,
    dword_t active_button, dword_t flags,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags | 0x2000,
                             result_ptr, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowNuiMessageBoxUI, kUI, kSketchy);

dword_result_t XNotifyQueueUI_entry(dword_t exnq, dword_t dwUserIndex,
                                    qword_t qwAreas,
                                    lpu16string_t displayText_ptr,
                                    lpvoid_t contextData) {
  std::string displayText = "";
  const uint8_t position_id = static_cast<uint8_t>(qwAreas);

  if (displayText_ptr) {
    displayText = xe::to_utf8(displayText_ptr.value());
  }

  XELOGI("XNotifyQueueUI: {}", displayText);

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  new xe::ui::XNotifyWindow(imgui_drawer, "", displayText, dwUserIndex,
                            position_id);

  // XNotifyQueueUI -> XNotifyQueueUIEx -> XMsgProcessRequest ->
  // XMsgStartIORequestEx & XMsgInProcessCall
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNotifyQueueUI, kUI, kSketchy);

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
dword_result_t XamShowKeyboardUI_entry(dword_t user_index, dword_t flags,
                                       lpu16string_t default_text,
                                       lpu16string_t title,
                                       lpu16string_t description,
                                       lpvoid_t buffer, dword_t buffer_length,
                                       pointer_t<XAM_OVERLAPPED> overlapped) {
  const std::array<uint32_t, 8> arguments{user_index,
                                          flags,
                                          default_text.guest_address(),
                                          title.guest_address(),
                                          description.guest_address(),
                                          buffer.guest_address(),
                                          buffer_length,
                                          overlapped.guest_address()};
  const auto native = NativeSystemUiService::TryInvokePublicOnOriginalCaller(
      kernel_state(), NativeSystemUiPublicCall::kShowKeyboard, arguments);
  if (native.handled()) {
    return native.value;
  }
  return ShowKeyboardUI(
      user_index, flags, default_text ? default_text.value() : u"",
      title ? title.value() : u"", description ? description.value() : u"",
      buffer.guest_address(), buffer_length, overlapped.guest_address(),
      native.host_fallback);
}
DECLARE_XAM_EXPORT1(XamShowKeyboardUI, kUI, kImplemented);

// Public304 keeps its actual caller/user. Native FE owns the request, focus,
// current-app and results; no host modal or FF-switch policy is created here.
dword_result_t XamShowGuideUI_entry(dword_t user_index) {
  const std::array<uint32_t, 1> arguments{user_index};
  const auto native = NativeSystemUiService::TryInvokePublicOnOriginalCaller(
      kernel_state(), NativeSystemUiPublicCall::kShowGuide, arguments);
  return native.handled() ? native.value : X_ERROR_NOT_SUPPORTED;
}
DECLARE_XAM_EXPORT1(XamShowGuideUI, kUI, kImplemented);

dword_result_t XamShowDeviceSelectorUI_entry(
    dword_t user_index, dword_t content_type, dword_t content_flags,
    qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto* state = kernel_state();
  // Diagnostic metadata only. Dedicated request quota cannot be consumed by
  // input polling; the first exhausted request reports that later calls are unknown.
  static std::atomic<uint64_t> diagnostic_requests{0};
  const uint64_t diagnostic_id = cvars::native_system_ui_trace
      ? diagnostic_requests.fetch_add(1, std::memory_order_relaxed) : UINT64_MAX;
  auto raw_trace = [id = diagnostic_id, user = uint32_t(user_index),
                    type = uint32_t(content_type), flags = uint32_t(content_flags),
                    required = uint64_t(total_requested),
                    output = device_id_ptr.guest_address(),
                    overlap = overlapped.guest_address()](
      const char* stage, uint32_t selected, X_RESULT result,
      uint32_t choice = UINT32_MAX, uint32_t overlap_access = UINT32_MAX,
      uint32_t output_access = UINT32_MAX) noexcept {
    if (id > 16) return;
    try {
      if (id == 16) {
        if (!std::strcmp(stage, "entry"))
          XELOGI("StorageDiagnostic selector budget_exhausted limit=16 later_requests=unknown");
        return;
      }
      XELOGI("StorageDiagnostic selector id={} stage={} user={} type={} flags={:08X} "
             "required={:016X} output={:08X} overlap={:08X} choice={} device={} "
             "result={:08X} result_known={} overlap_access={} output_access={} unchecked={}",
             id, stage, user, type, flags, required, output, overlap, choice,
             selected, result, result != UINT32_MAX, overlap_access, output_access, UINT32_MAX);
    } catch (...) {}  // Diagnostics never alter guest admission or completion.
  };
  raw_trace("entry", 0, UINT32_MAX);
  if (diagnostic_id < 16) {
    try {
      auto* caller = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
      const auto execution = state->emulator()->title_execution_state().snapshot();
      XELOGI("StorageDiagnostic selector_caller id={} thread={} process={} generation={}",
             diagnostic_id, caller ? caller->thread_id() : 0,
             caller ? uint32_t(caller->native_frame_thread_identity().process_type) : UINT32_MAX,
             execution.generation);
      // Short in-memory ownership only; no Account/GPD/host file reads.
      // Capture refusal includes busy; captured=false is not proof of absence.
      for (uint8_t slot = 0; slot != XUserMaxUserCount; ++slot) {
        const auto profile = state->xam_state()->profile_manager()->CaptureProfile(slot);
        XELOGI("StorageDiagnostic selector_profile id={} slot={} captured={} xuid={:016X} signin={}",
               diagnostic_id, slot, bool(profile), profile ? profile->xuid() : 0,
               profile ? profile->signin_state() : UINT32_MAX);
      }
    } catch (...) {}
  }
  if (!overlapped) {
    raw_trace("reject_null_overlap", 0, X_ERROR_INVALID_PARAMETER);
    return X_ERROR_INVALID_PARAMETER;
  }
  const auto overlap_access = state->memory()->QueryRangeAccess(
      overlapped.guest_address(), sizeof(XAM_OVERLAPPED));
  if (overlap_access != memory::PageAccess::kReadWrite) {
    raw_trace("reject_overlap_access", 0, X_ERROR_INVALID_PARAMETER,
              UINT32_MAX, uint32_t(overlap_access));
    return X_ERROR_INVALID_PARAMETER;
  }
  const bool invalid_user = user_index >= XUserMaxUserCount && user_index != XUserIndexAny;
  const bool invalid_flags = (content_flags & 0x83F00008) != 0;
  // Preserve the old short-circuit: no output range query for already-invalid arguments.
  const bool output_checked = !invalid_user && !invalid_flags && bool(device_id_ptr);
  const auto output_access = output_checked
      ? state->memory()->QueryRangeAccess(device_id_ptr.guest_address(), 4)
      : memory::PageAccess::kNoAccess;
  if (invalid_user || invalid_flags || !device_id_ptr ||
      output_access != memory::PageAccess::kReadWrite) {
    raw_trace(invalid_user ? "reject_user" : invalid_flags ? "reject_flags"
              : !device_id_ptr ? "reject_null_output" : "reject_output_access",
              0, X_ERROR_INVALID_PARAMETER, UINT32_MAX, uint32_t(overlap_access),
              output_checked ? uint32_t(output_access) : UINT32_MAX);
    XOverlappedSetExtendedError(overlapped, X_ERROR_INVALID_PARAMETER);
    return X_ERROR_INVALID_PARAMETER;
  }
  auto trace = [raw_trace, overlap_access, output_access](
      const char* stage, uint32_t selected, X_RESULT result,
      uint32_t choice = UINT32_MAX) noexcept {
    raw_trace(stage, selected, result, choice,
              uint32_t(overlap_access), uint32_t(output_access));
  };

  const auto devices = ListStorageDevices(state);
  const uint64_t required = total_requested;
  // Automatic storage admission is not a native-UI operation. In particular,
  // a healthy HDD does not depend on the Guide's profile/provider readiness.
  // The output is never read as a preference: it may be uninitialized storage.
  const auto choice = SelectStorageDevice(devices, content_flags, required, 1);
  trace("admit", choice.device_id, UINT32_MAX, uint32_t(choice.kind));
  if (diagnostic_id < 16) {
    try {
      for (size_t row = 0; row != std::min<size_t>(devices.size(), 8); ++row) {
        const auto& device = devices[row];
        const auto proof = device.managed_format_proof;
        const std::string_view source = proof ? std::string_view(proof->source_id()) : std::string_view{};
        XELOGI("StorageDiagnostic selector_device id={} row={} device={} type={} available={} "
               "readonly={} total={} free={} required={} suitable={} source_known={} source={:.64} source_generation={}",
               diagnostic_id, row, device.device_id, uint32_t(device.device_type),
               device.available, device.read_only, device.total_bytes, device.free_bytes,
               required, CanStoreOnDevice(device, required), bool(proof), source,
               proof ? proof->generation() : 0);
      }
      if (devices.size() > 8)
        XELOGI("StorageDiagnostic selector_device id={} row_budget_exhausted total={} shown=8", diagnostic_id, devices.size());
    } catch (...) {}
  }
  std::shared_ptr<NativeUiBackendRoute::HostLease> host_route;
  if (choice.kind == StorageSelectionKind::kPrompt) {
    // Native2CB forwards the full uint64 r6 unchanged. A dword pair would
    // move the output arguments to the wrong registers. Only a genuine prompt
    // enters native UI; an entered native request is never replayed on the host.
    const std::array<uint64_t, 6> arguments{
        user_index, content_type, content_flags, total_requested,
        device_id_ptr.guest_address(), overlapped.guest_address()};
    const auto native =
        NativeSystemUiService::TryInvokePublicRegistersOnOriginalCaller(
            state, NativeSystemUiPublicCall::kShowDeviceSelector, arguments);
    trace("native_disposition", choice.device_id, native.value, uint32_t(native.disposition));
    if (native.handled()) {
      trace("native_prompt", choice.device_id, native.value);
      return native.value;
    }
    host_route = native.host_fallback;
  }
  const uint32_t output = device_id_ptr.guest_address();
  auto publish = [state, output, required, trace, diagnostic_id,
                  host_route](const StorageDeviceInfo& selected) -> X_RESULT {
    const auto device_id = selected.device_id;
    // Drain must observe publication as one operation of the selected UI
    // cohort, rather than racing a cancelled() snapshot and guest output.
    auto entered = host_route ? host_route->TryEnter() : nullptr;
    if (host_route && !entered) {
      trace("publish_host_retired", device_id, X_ERROR_CANCELLED);
      return X_ERROR_CANCELLED;
    }
    const auto current = GetStorageDeviceInfo(state, device_id);
    if (diagnostic_id < 16) {
      try {
        XELOGI("StorageDiagnostic selector_publish_device id={} device={} known={} available={} readonly={} total={} free={} required={}",
               diagnostic_id, device_id, bool(current), current && current->available,
               !current || current->read_only, current ? current->total_bytes : 0,
               current ? current->free_bytes : 0, required);
      } catch (...) {}
    }
    if (!current || !current->available || !selected.source_volume ||
        current->source_volume != selected.source_volume ||
        current->source_volume.owner_before(selected.source_volume) ||
        selected.source_volume.owner_before(current->source_volume)) {
      trace("publish_disconnected", device_id, X_ERROR_DEVICE_NOT_CONNECTED);
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (current->read_only) {
      trace("publish_readonly", device_id, X_ERROR_ACCESS_DENIED);
      return X_ERROR_ACCESS_DENIED;
    }
    if (!CanStoreOnDevice(*current, required)) {
      trace("publish_disk_full", device_id, X_RESULT_FROM_WIN32(0x70));
      return X_RESULT_FROM_WIN32(0x70);  // ERROR_DISK_FULL.
    }
    // Capacity is re-observed above without namespace/registry locks. The
    // selected row must still name its original source generation, not a
    // replacement with the same public ID. This metadata carries no backing
    // owner; the existing admission joins registry withdrawal to the store.
    auto& registry = state->content_manager()->sources();
    std::error_code source_error;
    const auto source_refusal = [&] {
      const auto result = source_error == std::errc::device_or_resource_busy
          ? X_ERROR_BUSY : source_error == std::errc::no_such_device
          ? X_ERROR_DEVICE_NOT_CONNECTED : X_ERROR_FUNCTION_FAILED;
      trace("publish_source_refused", device_id, result);
      return result;
    };
    const auto description = registry.DescribeSourceCached(
        selected.source_volume, source_error);
    if (!description || source_error) {
      return source_refusal();
    }
    {
      auto memory_lock = global_critical_region::AcquireDirect();
      if (state->memory()->QueryRangeAccess(output, 4) !=
          memory::PageAccess::kReadWrite) {
        trace("publish_output_access", device_id, X_ERROR_INVALID_PARAMETER);
        return X_ERROR_INVALID_PARAMETER;
      }
      auto admitted = registry.TryAdmitSourceGeneration(*description, source_error);
      if (!admitted) {
        return source_refusal();
      }
      *state->memory()->TranslateVirtual<xe::be<uint32_t>*>(output) = device_id;
    }
    trace("publish_success", device_id, X_ERROR_SUCCESS);
    return X_ERROR_SUCCESS;
  };
  if (choice.kind != StorageSelectionKind::kPrompt) {
    // The actual Kernel owns and joins this worker before ContentManager and
    // Memory are destroyed. Retain the actual module (not a reusable guest
    // hmodule address), and reject a superseded/stopping foreground execution.
    // This acquires neither a modal input lease nor native UI prerequisites.
    auto title = state->GetExecutableModule();
    const auto execution = state->emulator()->title_execution_state().snapshot();
    auto current = [state, title, execution] {
      if (state->GetExecutableModule().get() != title.get()) {
        return false;
      }
      const auto now = state->emulator()->title_execution_state().snapshot();
      if (now.generation != execution.generation) {
        return false;
      }
      if (execution.generation) {
        return execution.phase == TitleExecutionState::Phase::kRunning &&
               now.phase == TitleExecutionState::Phase::kRunning &&
               now.hmodule == execution.hmodule &&
               state->emulator()->OwnsTitleExecution(state);
      }
      // Kernel-only fixtures / pre-foreground compatibility retain their
      // exact generation-zero module and state; this is no Running proof.
      return now == execution &&
             now.phase != TitleExecutionState::Phase::kStopping;
    };
    const uint32_t overlap_address = overlapped.guest_address();
    std::optional<StorageDeviceInfo> selected;
    if (choice.kind == StorageSelectionKind::kAutomatic) {
      const auto found = std::find_if(devices.begin(), devices.end(),
          [&](const auto& row) { return row.device_id == choice.device_id; });
      if (found != devices.end()) selected = *found;
    }
    state->CompleteOverlappedDeferredEx(
        [state, choice, current, publish, trace, overlap_address,
         selected = std::move(selected)](
            uint32_t& extended, uint32_t& length) -> X_RESULT {
          X_RESULT result = X_ERROR_CANCELLED;
          if (current()) {
            if (state->memory()->QueryRangeAccess(overlap_address,
                                                  sizeof(XAM_OVERLAPPED)) !=
                memory::PageAccess::kReadWrite) {
              result = X_ERROR_INVALID_PARAMETER;
            } else {
              switch (choice.kind) {
                case StorageSelectionKind::kAutomatic:
                  result = selected ? publish(*selected)
                                    : X_ERROR_DEVICE_NOT_CONNECTED;
                  break;
                case StorageSelectionKind::kDiskFull:
                  result = X_RESULT_FROM_WIN32(0x70);
                  break;
                case StorageSelectionKind::kNoDevice:
                  result = X_ERROR_DEVICE_NOT_CONNECTED;
                  break;
                default:
                  break;
              }
            }
          }
          trace("complete_without_ui", choice.device_id, result);
          extended = X_HRESULT_FROM_WIN32(result);
          length = 0;
          return result;
        },
        overlap_address);
    trace("return_automatic_pending", choice.device_id, X_ERROR_IO_PENDING);
    return X_ERROR_IO_PENDING;
  }
  if (state->xam_state()->IsUIActive()) {
    trace("host_prompt_busy", choice.device_id, X_ERROR_ACCESS_DENIED);
    return X_ERROR_ACCESS_DENIED;
  }

  cxxopts::OptionNames buttons;
  for (const auto& device : devices) {
    buttons.push_back(fmt::format("{} ({} MB free)", to_utf8(device.name),
                                  device.free_bytes / (1024 * 1024)));
  }
  buttons.push_back("Cancel");
  const std::string description =
      devices.empty()
          ? "No writable storage device is connected."
          : "Select a device with enough free space. Free up space in the "
            "dashboard's storage settings if needed.";
  auto* window = state->emulator()->display_window();
  auto* drawer = state->emulator()->imgui_drawer();
  auto* input = state->emulator()->input_system();
  if (!window || !drawer || !input) {
    trace("host_prompt_unavailable", choice.device_id, X_ERROR_FUNCTION_FAILED);
    return X_ERROR_FUNCTION_FAILED;
  }
  if (host_route) {
    auto* xam = state->xam_state();
    bool expected = false;
    if (!xam->is_xam_dialog_present_.compare_exchange_strong(expected, true)) {
      trace("host_dialog_busy", 0, X_ERROR_ACCESS_DENIED);
      return X_ERROR_ACCESS_DENIED;
    }
    auto reservation = HostUiFallbackReservation::TryCreate(xam, input);
    if (!reservation || !reservation->input_gap()) {
      trace("host_reservation_denied", 0, X_ERROR_NOT_READY);
      return X_ERROR_NOT_READY;
    }
    auto completion = std::make_shared<HostUiFallbackCompletion>();
    auto pre = [state, host_route] {
      if (!host_route->cancelled()) {
        state->BroadcastNotification(kXNotificationSystemUI, true);
      }
    };
    auto run = [reservation, host_route, completion, window, drawer, input,
                devices, buttons, description,
                publish, trace](uint32_t& extended, uint32_t& length) {
      HostUiFallbackOperationCompletion operation_completion(host_route);
      const auto gap = reservation->input_gap();
      QueueHostUiFallback(
          window->app_context(), host_route, completion,
          [host_route, gap, drawer, input, buttons,
           description](const auto& completed) {
            auto dialog = std::make_unique<MessageBoxDialog>(
                drawer, input, "Select storage device", description, buttons,
                0);
            dialog->set_close_callback(
                [raw = dialog.get(), completed, host_route] {
                  completed->Finish(SnapshotMessageBox(raw));
                });
            completed->admitted.store(true);
            if (gap) {
              gap->Release();
            }
            dialog.release();
          });
      GuestScheduler::WaitOnFence(completion->closed);
      const auto value = completion->result();
      reservation->Release();
      X_RESULT result = X_ERROR_CANCELLED;
      if (!host_route->cancelled() &&
          value.result == xe::ui::TextEntryResult::kAccepted &&
          value.button < devices.size()) {
        result = publish(devices[value.button]);
      }
      trace("host_prompt_result", value.button, result);
      extended = X_HRESULT_FROM_WIN32(result);
      length = 0;
      return result;
    };
    auto post = [state] {
      xe::threading::Sleep(kUIDelayMillis);
      if (!state->xam_state()->IsUIActive()) {
        state->BroadcastNotification(kXNotificationSystemUI, false);
      }
    };
    state->CompleteOverlappedDeferredEx(std::move(run),
                                        overlapped.guest_address(),
                                        std::move(pre), std::move(post));
    trace("return_host_pending", 0, X_ERROR_IO_PENDING);
    return X_ERROR_IO_PENDING;
  }

  // Native-disabled compatibility path retains the existing host dialog. Its
  // selected device is an owned value and is re-observed before output writes.
  const auto dialog_result = xeXamDispatchDialog<MessageBoxDialog>(
      [drawer, input, description, buttons](HostUiFallbackAccess&) {
        return new MessageBoxDialog(drawer, input, "Select storage device",
                                    description, buttons, 0);
      }, SnapshotMessageBox,
      [devices, publish, trace](const HostDialogResult& value) -> X_RESULT {
        const auto result = value.button < devices.size()
                   ? publish(devices[value.button])
                   : X_ERROR_CANCELLED;
        trace("legacy_prompt_result", value.button, result);
        return result;
      }, overlapped);
  trace("return_legacy_prompt", 0, dialog_result);
  return dialog_result;
}
DECLARE_XAM_EXPORT1(XamShowDeviceSelectorUI, kUI, kImplemented);

// XamShowDeviceSelectorUI with the tracking ID of a Kinect skeleton first.
dword_result_t XamShowNuiDeviceSelectorUI_entry(
    dword_t tracking_id, dword_t user_index, dword_t content_type,
    dword_t content_flags, qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  return XamShowDeviceSelectorUI_entry(user_index, content_type, content_flags,
                                       total_requested, device_id_ptr,
                                       overlapped);
}
DECLARE_XAM_EXPORT1(XamShowNuiDeviceSelectorUI, kUI, kSketchy);

const char kDiscReadErrorTitle[] = "Disc Read Error";
const char kDiscReadErrorDescription[] =
    "There's been an issue reading content from the game disc.\nThis is "
    "likely caused by bad or unimplemented file IO calls.";

void XamShowDirtyDiscErrorUI_entry(dword_t user_index,
                                   const ppc_context_t& context) {
  XELOGE("XamShowDirtyDiscErrorUI: title reported a disc read error, lr {:08X}",
         uint32_t(context->lr));

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  xeXamDispatchDialog<MessageBoxDialog>(
      [imgui_drawer, input_system](HostUiFallbackAccess&) {
        return new MessageBoxDialog(imgui_drawer, input_system,
                                    kDiscReadErrorTitle,
                                    kDiscReadErrorDescription, {"OK"}, 0);
      }, SnapshotMessageBox, {}, 0);
  // Does not return.
  kernel_state()->ExitToDashboard();
}
DECLARE_XAM_EXPORT1(XamShowDirtyDiscErrorUI, kUI, kImplemented);

dword_result_t XamShowNuiDirtyDiscErrorUI_entry(dword_t tracking_id) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  // XamShowAndWaitForMessageBoxEx, same as XamShowDirtyDiscErrorUI except flag
  // = 0x2001, user index always set to XUserIndexAny
  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  xeXamDispatchDialog<MessageBoxDialog>(
      [imgui_drawer, input_system](HostUiFallbackAccess&) {
        return new MessageBoxDialog(imgui_drawer, input_system,
                                    kDiscReadErrorTitle,
                                    kDiscReadErrorDescription, {"OK"}, 0);
      }, SnapshotMessageBox, {}, 0);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiDirtyDiscErrorUI, kUI, kStub);

dword_result_t XamShowPartyUI_entry(dword_t user_index) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowPartyUI, kUI, kStub);

dword_result_t XamShowCommunitySessionsUI_entry(dword_t user_index,
                                                dword_t social_sessions_flags) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowCommunitySessionsUI, kUI, kStub);

// https://gitlab.com/GlitchyScripts/xlivelessness/-/blob/master/xlivelessness/xlive/xdefs.hpp?ref_type=heads#L1235
dword_result_t XamShowMarketplaceUIEx_entry(dword_t user_index, dword_t ui_type,
                                            qword_t offer_id,
                                            dword_t offer_type,
                                            dword_t content_category,
                                            unknown_t unk6, unknown_t unk7,
                                            dword_t title_id) {
  // ui_type:
  // 0 - view all content for the current title
  // 1 - view content specified by offer id
  // offer_types:
  // filter for content list, usually just -1
  // content_category:
  // filter on item types for games (e.g. cars, maps, weapons, etc)
  if (user_index >= XUserMaxUserCount || ui_type >= MaxEntrypoint) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  bool is_xbla_unlock_offer =
      (offer_id == ((uint64_t(kernel_state()->title_id()) << 32) | 1ull));

  auto close = [state = kernel_state(), ui_type, is_xbla_unlock_offer](
                   const HostDialogResult& value) -> X_RESULT {
    if (ui_type == 1 && is_xbla_unlock_offer) {
      uint32_t button = value.button;
      if (button == 0) {
        cvars::license_mask = 1;

        state->BroadcastNotification(
            kXNotificationLiveContentInstalled, 0);
      }
    }
    return X_ERROR_SUCCESS;
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  cxxopts::OptionNames buttons;

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
      desc =
          "Game requested to open marketplace page with all content for the "
          "current title ID.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      desc = fmt::format(
          "Game requested to open marketplace page for offer ID 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipList:
      desc =
          "Game requested to open marketplace page with all Xbox Live "
          "memberships.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipItem:
      desc = fmt::format(
          "Game requested to open marketplace page for an Xbox Live "
          "membership offer 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentList_Background:
      // Used when accessing microsoft points
      desc = fmt::format(
          "Xbox Marketplace requested access to Microsoft Points offer page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem_Background:
      // Used when accessing credit card information and calls
      // XamShowCreditCardUI
      desc = fmt::format(
          "Xbox Marketplace requested access to credit card information page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV1:
      // Used by XamShowForcedNameChangeUI v1888
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV2:
      // Used by XamShowForcedNameChangeUI NXE and up
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::GamerTagPurchase:
      // Used by dashboard when selecting change gamertag in profile menu
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ActiveDownloads:
      // Used in profile tabs when clicking active downloads
      desc = fmt::format(
          "There are no current plans to download files from Xbox "
          "Marketplace.");
      break;
    default:
      desc = fmt::format("Unknown marketplace op {:d}",
                         static_cast<uint32_t>(ui_type));
      break;
  }

  desc +=
      "\nNote that since Xenia cannot access Xbox Marketplace, any DLC must be "
      "installed manually using File -> Install Content.";

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
    default:
      buttons.push_back("OK");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      if (is_xbla_unlock_offer) {
        desc +=
            "\n\nTo start trial games in full mode, set license_mask to 1 in "
            "Xenia config file.\n\nDo you wish to change license_mask to 1 for "
            "*this session*?";
        buttons.push_back("Yes");
        buttons.push_back("No");
      } else {
        buttons.push_back("OK");
      }
      break;
  }

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  return xeXamDispatchDialog<MessageBoxDialog>(
      [imgui_drawer, input_system, title, desc, buttons](HostUiFallbackAccess&) {
        return new MessageBoxDialog(imgui_drawer, input_system, title, desc,
                                    buttons, 0);
      }, SnapshotMessageBox, close, 0, true);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUIEx, kUI, kSketchy);

dword_result_t XamShowMarketplaceUI_entry(dword_t user_index, dword_t ui_type,
                                          qword_t offer_id, dword_t offer_type,
                                          dword_t content_category,
                                          dword_t title_id) {
  return XamShowMarketplaceUIEx_entry(user_index, ui_type, offer_id, offer_type,
                                      content_category, 0, 0, title_id);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUI, kUI, kSketchy);

dword_result_t XamShowMarketplaceDownloadItemsUI_entry(
    dword_t user_index, dword_t ui_type, lpqword_t offers, dword_t num_offers,
    lpdword_t hresult_ptr, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (user_index >= XUserMaxUserCount || !offers || num_offers > 6) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped,
                                                  X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }


  auto close = [state = kernel_state(), hresult_ptr](
                   const HostDialogResult&) -> X_RESULT {
    auto memory_lock = global_critical_region::AcquireDirect();
    if (hresult_ptr) {
      if (state->memory()->QueryRangeAccess(hresult_ptr.guest_address(), 4) !=
          memory::PageAccess::kReadWrite) { return X_ERROR_INVALID_PARAMETER; }
      // TODO
      *hresult_ptr = X_E_SUCCESS;
    }
    return X_ERROR_SUCCESS;
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  cxxopts::OptionNames buttons = {"OK"};

  switch (ui_type) {
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::FREEITEMS:
      desc =
          "Game requested to open download page for the following free offer "
          "IDs:";
      break;
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::PAIDITEMS:
      desc =
          "Game requested to open download page for the following offer IDs:";
      break;
    default:
      return X_ERROR_INVALID_PARAMETER;
  }

  for (uint32_t i = 0; i < num_offers; i++) {
    desc += fmt::format("\n0x{:16X}", offers[i].get());
  }

  desc +=
      "\n\nNote that since Xenia cannot access Xbox Marketplace, any DLC "
      "must "
      "be installed manually using File -> Install Content.";

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xe::hid::InputSystem* input_system = emulator->input_system();
  return xeXamDispatchDialog<MessageBoxDialog>(
      [imgui_drawer, input_system, title, desc, buttons](HostUiFallbackAccess&) {
        return new MessageBoxDialog(imgui_drawer, input_system, title, desc,
                                    buttons, 0);
      }, SnapshotMessageBox, close, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceDownloadItemsUI, kUI, kSketchy);

// Largest order the purchase UI accepts and the receipt buffer size it asks
// for when queried.
constexpr uint32_t kMarketplaceOrderMaxSize = 0x13880;

// Without a receipt buffer it only reports the size it needs. A purchase needs
// a signed in user and no other system UI, then goes to the marketplace guide,
// the Kinect one for a nonzero tracking_id. There is no marketplace here, so
// the order is taken as cancelled. What the guide reports without LIVE isn't
// known. Returns an HRESULT.
dword_result_t XamShowMarketplacePurchaseOrderUIEx_entry(
    dword_t user_index, dword_t tracking_id, dword_t order_size, lpvoid_t order,
    lpdword_t receipt_size_ptr, lpvoid_t receipt, dword_t unknown,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!receipt && receipt_size_ptr) {
    *receipt_size_ptr = kMarketplaceOrderMaxSize;
    return X_E_SUCCESS;
  }
  if (user_index >= XUserMaxUserCount || !order || !receipt ||
      !receipt_size_ptr || order_size > kMarketplaceOrderMaxSize ||
      receipt_size_ptr.value() < kMarketplaceOrderMaxSize) {
    return X_E_INVALIDARG;
  }
  if (!kernel_state()->xam_state()->GetUserProfile(user_index)) {
    return X_E_NO_SUCH_USER;
  }
  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_E_ACCESS_DENIED;
  }
  XELOGI(
      "XamShowMarketplacePurchaseOrderUIEx: no Xbox LIVE Marketplace, "
      "cancelling the order");
  const X_RESULT result = xeXamDispatchWithoutDialog(
      []() { return X_ERROR_CANCELLED; }, overlapped.guest_address());
  return X_HRESULT_FROM_WIN32(result);
}
DECLARE_XAM_EXPORT1(XamShowMarketplacePurchaseOrderUIEx, kUI, kSketchy);

// Tells the user an optional media update is needed, with a message box that
// offers the marketplace to a LIVE profile. user_index may be XUserIndexAny. A
// bad one fails the request rather than the call. With no message box here
// the request succeeds as if it were dismissed. The call returns 0, queued or
// not.
dword_result_t XamShowOptionalMediaUpdateRequiredUI_entry(
    dword_t user_index, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (user_index >= XUserMaxUserCount && user_index != XUserIndexAny) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediateEx(overlapped.guest_address(),
                                                    X_ERROR_FUNCTION_FAILED,
                                                    X_E_INVALIDARG, 0);
    } else {
      XThread::SetLastError(X_E_INVALIDARG);
    }
    return X_ERROR_SUCCESS;
  }
  XELOGI("XamShowOptionalMediaUpdateRequiredUI: no message box to show");
  xeXamDispatchWithoutDialog([]() { return X_ERROR_SUCCESS; },
                             overlapped.guest_address());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowOptionalMediaUpdateRequiredUI, kUI, kSketchy);

dword_result_t XamShowGoldUpgradeUI_entry(dword_t user_index, dword_t unk,
                                          dword_t title_id) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowGoldUpgradeUI, kUI, kStub);

dword_result_t XamShowMultiplayerUpgradeUI_entry(dword_t user_index) {
  return XamShowGoldUpgradeUI_entry(user_index, 6,
                                    kernel_state()->emulator()->title_id());
}
DECLARE_XAM_EXPORT1(XamShowMultiplayerUpgradeUI, kUI, kImplemented);

dword_result_t XamShowForcedNameChangeUI_entry(dword_t user_index) {
  return XamShowMarketplaceUIEx_entry(
      user_index, X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV2, 0, 0xffffffff,
      0, 0, 0, 0);
}
DECLARE_XAM_EXPORT1(XamShowForcedNameChangeUI, kUI, kImplemented);

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid) {
  const ImVec2 start_position = ImGui::GetCursorPos();

  // Calculate the size for the selectable based on icon + text
  const float text_height = ImGui::GetTextLineHeight() * 3;  // 3 lines of text
  const ImVec2 content_size =
      ImVec2(ImGui::GetContentRegionAvail().x,
             std::max(xe::ui::default_image_icon_size.y, text_height));

  // Draw selectable first as background
  bool clicked = false;
  if (xuid && selected_xuid) {
    if (ImGui::Selectable("##Selectable", *selected_xuid == xuid,
                          ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap,
                          content_size)) {
      *selected_xuid = xuid;
      clicked = true;
    }
    // Update selection when focus moves to this item
    if (ImGui::IsItemFocused()) {
      *selected_xuid = xuid;
    }
    // Reset cursor to draw content on top
    ImGui::SetCursorPos(start_position);
  }

  // Draw content on top of selectable
  ImGui::BeginGroup();
  {
    if (profile_icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(profile_icon),
                   xe::ui::default_image_icon_size);
    } else {
      if (user_index < XUserMaxUserCount) {
        const auto icon = imgui_drawer->GetNotificationIcon(user_index);
        ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                     xe::ui::default_image_icon_size);
      } else {
        ImGui::Dummy(xe::ui::default_image_icon_size);
      }
    }

    ImGui::SameLine();

    ImGui::BeginGroup();
    {
      ImGui::TextUnformatted(
          fmt::format("User: {}\n", account->GetGamertagString()).c_str());
      ImGui::TextUnformatted(fmt::format("XUID: {:016X}  \n", xuid).c_str());
      if (user_index != XUserIndexAny) {
        ImGui::TextUnformatted(
            fmt::format("Assigned to slot: {}\n", user_index + 1).c_str());
      } else {
        ImGui::TextUnformatted(fmt::format("Profile is not signed in").c_str());
      }
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();

  if (clicked) {
    ImGui::OpenPopup("Profile Menu");
  }

  if (context_menu) {
    return context_menu();
  }

  return true;
}

X_RESULT xeXamShowSigninUI(uint32_t user_index, uint32_t users_needed,
                           uint32_t flags, uint32_t overlapped = 0,
                           NativeSigninUiDiagnostic* diagnostic = nullptr,
                           std::shared_ptr<NativeUiBackendRoute::HostLease> admitted = {}) {
  if ((users_needed != 1 && users_needed != 2 && users_needed != 4) ||
      (user_index >= XUserMaxUserCount && user_index != XUserIndexAny)) {
    if(diagnostic)diagnostic->stage=1;
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* state = kernel_state();
  auto* input = state->emulator()->input_system();
  if (!input) { if(diagnostic)diagnostic->stage=2; return X_ERROR_NOT_READY; }
  if(diagnostic)diagnostic->native_owned=input->IsNativeInputOwned();
  NativeSigninHostFlagGuard host_flag{state->xam_state(),false};
  const bool selected_native_host=bool(admitted);
  auto original_host_entry=admitted?admitted->TryEnter():nullptr;
  if(selected_native_host){
    if(!original_host_entry)return X_ERROR_CANCELLED;
    bool expected=false;
    if(!state->xam_state()->is_xam_dialog_present_.compare_exchange_strong(expected,true))
      return X_ERROR_ACCESS_DENIED;
    host_flag.owned=true;
  }
  uint32_t controller;
  {
    auto lock = input->lock();
    controller = user_index < XUserMaxUserCount
                     ? user_index : input->GetLastUsedSlot();
  }
  if (controller >= XUserMaxUserCount) { controller = 0; }
  if ((flags & static_cast<uint32_t>(SigninUiFlags::CompleteSignIn)) &&
      user_index < XUserMaxUserCount &&
      state->xam_state()->IsUserSignedIn(user_index)) {
    if(diagnostic)diagnostic->route=1;
    auto lease = admitted?std::move(admitted):state->AcquireLegacyHostUi();
    if (!lease) { if(diagnostic)diagnostic->stage=3; return X_ERROR_ACCESS_DENIED; }
    auto selected_completion=selected_native_host
        ?std::make_shared<HostUiFallbackOperationCompletion>(lease):nullptr;
    host_flag.Release();
    auto reservation = HostUiFallbackReservation::TryCreate(
        state->xam_state(), input, !selected_native_host);
    if (!reservation || !reservation->input_gap()) { if(diagnostic)diagnostic->stage=4; return X_ERROR_NOT_READY; }
    if(diagnostic)diagnostic->stage=8;
    state->BroadcastNotification(kXNotificationSystemUI, true);
    auto run = [state, lease, reservation, selected_native_host,
                selected_completion=std::move(selected_completion)]() mutable -> X_RESULT {
      auto finished=std::move(selected_completion);
      xe::threading::Sleep(kUIDelayMillis);
      X_RESULT result = X_ERROR_CANCELLED;
      if (auto entered = lease->TryEnter()) {
        uint32_t mask = 0;
        for (uint32_t slot = 0; slot < XUserMaxUserCount; ++slot) {
          if (state->xam_state()->IsUserSignedIn(slot)) { mask |= 1u << slot; }
        }
        state->BroadcastNotification(kXNotificationSystemSignInChanged, mask);
        result = X_ERROR_SUCCESS;
      }
      if(!selected_native_host)lease->FinishHostOnly();
      reservation->Release();
      if (result == X_ERROR_SUCCESS) {
        state->BroadcastNotification(kXNotificationSystemUI, false);
      }
      return result;
    };
    X_RESULT queued=X_ERROR_NOT_ENOUGH_MEMORY;
    const auto submitted=InvokeHostCall([&] {
      queued=state->QueueHostUiCompletion(std::move(run),
          [lease, reservation, selected_native_host] {
            lease->Cancel();reservation->Release();
            if(selected_native_host)lease->FinishHostOperation();else lease->FinishHostOnly();
          }, overlapped);
    });
    if(submitted!=HostCallOutcome::kReturned){
      lease->Cancel();reservation->Release();
      if(selected_native_host)lease->FinishHostOperation();else lease->FinishHostOnly();
      if(!state->xam_state()->IsUIActive())state->BroadcastNotification(kXNotificationSystemUI,false);
      return X_ERROR_NOT_ENOUGH_MEMORY;
    }
    return queued;
  }
  if(diagnostic)diagnostic->route=2;
  auto* drawer = state->emulator()->imgui_drawer();
  return xeXamDispatchDialog<ui::SigninUI>(
      [drawer, controller, users_needed](HostUiFallbackAccess& access) {
        return new ui::SigninUI(drawer,
            access.kernel()->xam_state()->profile_manager(), controller,
            users_needed);
      }, [](ui::SigninUI* dialog) {
        HostDialogResult value;
        if (dialog->accepted()) {
          value.result = xe::ui::TextEntryResult::kAccepted;
          value.signin = dialog->selection();
        }
        return value;
      }, {}, overlapped, true, nullptr, std::move(admitted), diagnostic,
      selected_native_host, &host_flag,
      [state](const HostUiProfileSelection& selection,
              const std::function<bool()>& cancelled) -> X_RESULT {
        if (!selection.valid()) return X_ERROR_INVALID_PARAMETER;
        std::map<uint8_t, uint64_t> profiles;
        for (uint8_t slot = 0; slot != 4; ++slot)
          if (selection.mask & (1u << slot)) profiles.emplace(slot, selection.xuids[slot]);
        const auto outcome = state->xam_state()->profile_manager()->LoginMultipleResult(
            profiles, cancelled);
        if (outcome.error) {
          if (outcome.error == std::errc::operation_canceled) return X_ERROR_CANCELLED;
          if (outcome.error == std::errc::device_or_resource_busy ||
              outcome.error == std::errc::resource_unavailable_try_again) return X_ERROR_BUSY;
          return X_ERROR_FUNCTION_FAILED;
        }
        return outcome.maps_committed && outcome.writers_available
            ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
      });
}

static X_RESULT ShowCreateProfileOnOriginalKernel(
    KernelState* state, uint32_t user_index,
    std::shared_ptr<NativeUiBackendRoute::HostLease> admitted = {}) {
  if (user_index >= XUserMaxUserCount && user_index != XUserIndexAny) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* drawer = state->emulator()->imgui_drawer();
  auto* input = state->emulator()->input_system();
  if (!input) {
    if (admitted) {
      admitted->FinishHostOnly();
      state->xam_state()->is_xam_dialog_present_.store(false);
    }
    return X_ERROR_NOT_READY;
  }
  uint32_t controller;
  {
    auto lock = input->lock();
    controller = user_index < XUserMaxUserCount
                     ? user_index : input->GetLastUsedSlot();
  }
  if (controller >= XUserMaxUserCount) { controller = 0; }
  return xeXamDispatchDialog<ui::CreateProfileUI>(
      [drawer, controller](HostUiFallbackAccess& access) {
        return new ui::CreateProfileUI(drawer, access.kernel(), controller);
      }, [](ui::CreateProfileUI*) {
        HostDialogResult value;
        value.result = xe::ui::TextEntryResult::kAccepted;
        return value;
      }, {}, 0, true, state, std::move(admitted));
}
X_RESULT xeXamShowCreateProfileUIEx(uint32_t user_index, uint32_t flag,
                                    char* unknown) {
  return ShowCreateProfileOnOriginalKernel(kernel_state(), user_index);
}
X_RESULT xeXamShowHostCreateProfileUI(Emulator* emulator, uint32_t user_index) {
  if (!emulator ||
      (user_index >= XUserMaxUserCount && user_index != XUserIndexAny)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  KernelState* original = nullptr;
  std::shared_ptr<NativeUiBackendRoute::HostLease> lease;
  std::unique_ptr<NativeUiBackendRoute::HostLease::Entry> entered;
  const auto result = emulator->WithHostRuntime([&](KernelState& runtime) {
    lease = runtime.AcquireLegacyHostUi();
    if (!lease) { return X_ERROR_ACCESS_DENIED; }
    entered = lease->TryEnter();
    if (!entered) {
      runtime.xam_state()->is_xam_dialog_present_.store(false);
      return X_ERROR_CANCELLED;
    }
    original = &runtime;
    return X_ERROR_SUCCESS;
  });
  if (result != X_ERROR_SUCCESS) { return result; }
  // The runtime mutex has been released. This same-thread/LIFO Entry pins the
  // captured exact Kernel through UI queue + Kernel work publication. Stop may
  // cancel it, but cannot reclaim the Kernel while this Entry is still active.
  // No global/current-Kernel lookup can remint an old host dialog's request.
  if (lease->cancelled()) {
    original->xam_state()->is_xam_dialog_present_.store(false);
    return X_ERROR_CANCELLED;
  }
  return ShowCreateProfileOnOriginalKernel(original, user_index, std::move(lease));
}

dword_result_t XamShowSigninUI_entry(dword_t users_needed, dword_t flags) {
  NativeSigninUiDiagnostic diagnostic;
  const std::array<uint32_t,2> arguments{users_needed,flags};
  const auto native=NativeSystemUiService::TryInvokePublicOnOriginalCaller(
      kernel_state(),NativeSystemUiPublicCall::kShowSignin,arguments);
  X_RESULT result;
  if(native.handled()){
    result=native.value;diagnostic.route=3;diagnostic.stage=100+uint32_t(native.disposition);
    const auto* input=kernel_state()->emulator()->input_system();
    diagnostic.native_owned=input && input->IsNativeInputOwned();
  }else{
    result=xeXamShowSigninUI(XUserIndexAny,users_needed,flags,0,&diagnostic,native.host_fallback);
    // Record selected fallback separately from inactive ordinary legacy UI.
    if(native.host_fallback)diagnostic.route+=4;
  }
  kernel_state()->xam_state()->profile_manager()->RecordNativeSigninUi(
      XUserIndexAny,users_needed,flags,0,result,diagnostic.route,
      diagnostic.stage,diagnostic.native_owned);
  return result;
}
DECLARE_XAM_EXPORT1(XamShowSigninUI, kUserProfiles, kImplemented);

dword_result_t XamShowSigninUIEx_entry(
    dword_t users_needed, dword_t flags,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamShowSigninUI(XUserIndexAny, users_needed, flags,
                            overlapped_ptr.guest_address());
}
DECLARE_XAM_EXPORT1(XamShowSigninUIEx, kUserProfiles, kSketchy);

dword_result_t XamShowNuiSigninUI_entry(dword_t tracking_id, dword_t user_index,
                                        dword_t flags) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  uint32_t users_needed = 1;
  uint32_t sent_flags = flags | static_cast<uint32_t>(SigninUiFlags::NUI);
  // xeXamNuiHudCheck(tracking_id) = success then continue else return
  return xeXamShowSigninUI(user_index, users_needed, sent_flags);
}
DECLARE_XAM_EXPORT1(XamShowNuiSigninUI, kUserProfiles, kSketchy);

dword_result_t XamShowSigninUIp_entry(dword_t user_index, dword_t users_needed,
                                      dword_t flags) {
  const std::array<uint32_t,3> arguments{user_index,users_needed,flags};
  const auto native=NativeSystemUiService::TryInvokePublicOnOriginalCaller(
      kernel_state(),NativeSystemUiPublicCall::kShowSigninWithUser,arguments);
  if(native.handled())return native.value;
  return xeXamShowSigninUI(user_index,users_needed,flags,0,nullptr,native.host_fallback);
}
DECLARE_XAM_EXPORT1(XamShowSigninUIp, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUIEx_entry(dword_t user_index, dword_t flag,
                                              lpstring_t unkn2_ptr) {
  return xeXamShowCreateProfileUIEx(user_index, flag, unkn2_ptr);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUIEx, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUI_entry(dword_t user_index, dword_t flag) {
  return xeXamShowCreateProfileUIEx(user_index, flag, 0);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUI, kUserProfiles, kImplemented);

dword_result_t XamShowAchievementsUI_entry(dword_t user_index,
                                           dword_t title_id) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }

  uint32_t proper_title_id =
      title_id ? title_id.value()
               : kernel_state()->xam_state()->spa_info()->title_id();

  const auto info =
      kernel_state()->xam_state()->user_tracker()->GetUserTitleInfo(
          user->xuid(), proper_title_id);

  if (!info) {
    return X_ERROR_NO_SUCH_USER;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();
  xe::hid::InputSystem* input_system =
      kernel_state()->emulator()->input_system();

  const auto title_info = info.value();
  const uint64_t xuid = user->xuid();
  return xeXamDispatchDialog<ui::GameAchievementsUI>(
      [imgui_drawer, input_system, title_info, xuid](HostUiFallbackAccess& access) {
        auto* user = access.kernel()->xam_state()->GetUserProfile(xuid);
        return user ? new ui::GameAchievementsUI(
                          imgui_drawer, input_system, &title_info, user)
                    : nullptr;
      }, [](ui::GameAchievementsUI*) {
        HostDialogResult value;
        value.result = xe::ui::TextEntryResult::kAccepted;
        return value;
      }, {}, 0, true);
}
DECLARE_XAM_EXPORT1(XamShowAchievementsUI, kUserProfiles, kImplemented);

dword_result_t XamShowGamerCardUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();

  auto* window = kernel_state()->emulator()->display_window();
  const uint64_t xuid = user->xuid();
  return xeXamDispatchDialog<ui::GamercardUI>(
      [window, imgui_drawer, xuid](HostUiFallbackAccess& access) {
        return new ui::GamercardUI(window, imgui_drawer, access.kernel(), xuid);
      }, [](ui::GamercardUI*) {
        HostDialogResult value;
        value.result = xe::ui::TextEntryResult::kAccepted;
        return value;
      }, {}, 0, true);
}
DECLARE_XAM_EXPORT1(XamShowGamerCardUI, kUserProfiles, kImplemented);

dword_result_t XamShowEditProfileUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();

  auto* window = kernel_state()->emulator()->display_window();
  const uint64_t xuid = user->xuid();
  return xeXamDispatchDialog<ui::GamercardUI>(
      [window, imgui_drawer, xuid](HostUiFallbackAccess& access) {
        return new ui::GamercardUI(window, imgui_drawer, access.kernel(), xuid);
      }, [](ui::GamercardUI*) {
        HostDialogResult value;
        value.result = xe::ui::TextEntryResult::kAccepted;
        return value;
      }, {}, 0, true);
}
DECLARE_XAM_EXPORT1(XamShowEditProfileUI, kUserProfiles, kImplemented);

bool xeXamChooseIndieGame(std::string* file_name, uint32_t* device_id,
                          std::string* display_name) {
  const Emulator* emulator = kernel_state()->emulator();
  if (!emulator->display_window() || !emulator->imgui_drawer()) {
    return false;
  }

  auto games = kernel_state()->content_manager()->ListContent(
      static_cast<uint32_t>(DummyDeviceId::HDD), 0, kXN_2002,
      XContentType::kMarketplaceContent);
  std::sort(
      games.begin(), games.end(),
      [](const XCONTENT_AGGREGATE_DATA& a, const XCONTENT_AGGREGATE_DATA& b) {
        return a.display_name() < b.display_name();
      });
  std::vector<ui::DiscSwapUI::DiscInfo> items;
  for (const auto& game : games) {
    const std::string name = xe::to_utf8(game.display_name());
    items.push_back({name.empty() ? game.file_name() : name,
                     xe::to_path(game.file_name())});
  }

  const bool none_installed = items.empty();
  const std::string message =
      none_installed ? "ERROR: No indie games are installed. Install one with "
                       "Tools > Install Content, then try again."
                     : std::string();
  std::filesystem::path chosen;
  xeXamDispatchDialog<ui::DiscSwapUI>(
      [drawer = emulator->imgui_drawer(), input = emulator->input_system(),
       message, items, none_installed](HostUiFallbackAccess&) {
        return new ui::DiscSwapUI(drawer, input, message, items, none_installed,
                                  "Indie Games", "Select a game to play:", false);
      }, [](ui::DiscSwapUI* dialog) {
        HostDialogResult value;
        if (dialog->result() == ui::DiscSwapResult::kSelected) {
          value.result = xe::ui::TextEntryResult::kAccepted;
          value.path = dialog->selected_path();
        }
        return value;
      }, [&chosen](const HostDialogResult& value) {
        chosen = value.path;
        return X_ERROR_SUCCESS;
      });

  for (size_t i = 0; i < games.size(); ++i) {
    if (!chosen.empty() && items[i].path == chosen) {
      *file_name = games[i].file_name();
      *device_id = games[i].device_id;
      *display_name = items[i].label;
      return true;
    }
  }
  return false;
}

// UI
dword_result_t XamShowNuiTroubleshooterUI_entry(dword_t user_index,
                                                dword_t tracking_id,
                                                dword_t flags) {
  /* Notes:
     - calls XamPackageManagerGetExperienceMode(&var) with var = 1
     - If returns less than zero or (var & 1) == 0 then get error message:
       - if XamPackageManagerGetExperienceMode = 0 then call XamShowMessageBoxUI
         - if XamShowMessageBoxUI returns 0x3e5 then XamShowNuiTroubleshooterUI
     returns 0
       - else XamShowNuiTroubleshooterUI returns 0x65b and call another func
     - else:
       - call XamNuiHudSetEngagedTrackingID(tracking_id) and doesn't care aboot
     return and set var2 = 2
       - checks if (flag & 0x800000) == 0
         - if true call XamNuiGetDeviceStatus.
           - if XamNuiGetDeviceStatus != 0 set var2 = 3
       - else var2 = 4
       - XamAppRequestLoadEx(var2);
       - if return = 0 then XamShowNuiTroubleshooterUI returns 5
       - else set buffer[8] and call
     XMsgSystemProcessCall(0xfe,0x21028,buffer,0xc);
     - XamNuiNatalCameraUpdateComplete calls
     XamShowNuiTroubleshooterUI(0xff,0,0) if param = -0x7ff8fffe
  */

  auto* drawer = kernel_state()->emulator()->imgui_drawer();
  auto* input = kernel_state()->emulator()->input_system();
  return xeXamDispatchDialog<MessageBoxDialog>(
      [drawer, input](HostUiFallbackAccess&) {
        return new MessageBoxDialog(drawer, input, "NUI Troubleshooter",
            "The game has indicated there is a problem with NUI (Kinect).",
            {"OK"}, 0);
      }, SnapshotMessageBox, {});
}
DECLARE_XAM_EXPORT1(XamShowNuiTroubleshooterUI, kNone, kStub);

dword_result_t XamShowNuiHardwareRequiredUI_entry(unknown_t unk1) {
  if (unk1 != 0) {
    return X_ERROR_INVALID_PARAMETER;
  }

  return XamShowNuiTroubleshooterUI_entry(0xff, 0, 0x400000);
}
DECLARE_XAM_EXPORT1(XamShowNuiHardwareRequiredUI, kNone, kImplemented);

dword_result_t XamShowNuiGuideUI_entry(unknown_t tracking_id, unknown_t unk2) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  /* Notes:
   - calls xeXamNuiHudCheck, if success call XamAppRequestLoadEx(4) and if
   succeed call XMsgSystemProcessCall and some sub function that ensures correct
   h_result is returned
   - if XamAppRequestLoadEx(4) fails return X_ERROR_ACCESS_DENIED
  */

  return X_ERROR_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamShowNuiGuideUI, kNone, kStub);

XLanguage xeGetLanguage(bool extended_languages_support);

// Only XUI's string tables, which titles that don't link XUI can load through
// xam.
static std::mutex xui_lock;
static bool xui_initialized = false;
static std::unordered_set<uint32_t> xui_string_tables;

dword_result_t XuiInit_entry(lpdword_t params) {
  // The parameters start with their size.
  if (params && *params > 0xC) {
    return 0x8000FFFF;
  }
  std::lock_guard<std::mutex> lock(xui_lock);
  if (xui_initialized) {
    return 1;
  }
  xui_initialized = true;
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XuiInit, kUI, kSketchy);

dword_result_t XuiUninit_entry() {
  std::lock_guard<std::mutex> lock(xui_lock);
  xui_initialized = false;
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XuiUninit, kUI, kSketchy);

using xe::resource::FindXuiPackageFile;
using xe::resource::ParseXuiStringTable;

X_HRESULT LoadXuiStringTable(XModule& module, std::string_view section,
                             std::string_view file_name, uint32_t* table_ptr) {
  auto memory = kernel_state()->memory();
  uint32_t section_data = 0;
  uint32_t section_size = 0;
  if (XFAILED(module.GetSection(section, &section_data, &section_size))) {
    return X_E_FAIL;
  }
  const std::span<const uint8_t> resource(
      memory->TranslateVirtual<const uint8_t*>(section_data), section_size);

  // XUI's locale follows the console language, a translation is in a folder
  // named after it.
  std::optional<std::span<const uint8_t>> localized;
  std::optional<std::span<const uint8_t>> unlocalized = resource;
  if (!file_name.empty()) {
    const uint32_t locale_ptr =
        kernel_state()->xam_state()->GetLanguageFallbackAddress(
            static_cast<uint32_t>(xeGetLanguage(false)));
    if (locale_ptr) {
      const std::string locale = xe::to_utf8(xe::load_and_swap<std::u16string>(
          memory->TranslateVirtual(locale_ptr)));
      if (!locale.empty()) {
        localized = FindXuiPackageFile(resource,
                                       locale + "\\" + std::string(file_name));
      }
    }
    unlocalized = FindXuiPackageFile(resource, file_name);
  }
  const auto& table_file = localized ? localized : unlocalized;
  if (!table_file) {
    return X_E_FAIL;
  }

  *table_ptr = 0;
  uint8_t flags = 0;
  auto strings = ParseXuiStringTable(*table_file, &flags);
  if (!strings) {
    return X_E_INVALIDARG;
  }
  // Strings a translation leaves empty come from the untranslated table.
  if (localized && unlocalized && (flags & 2)) {
    uint8_t unlocalized_flags = 0;
    auto fallback = ParseXuiStringTable(*unlocalized, &unlocalized_flags);
    if (fallback) {
      for (size_t i = 0; i < std::min(strings->size(), fallback->size()); ++i) {
        if ((*strings)[i].empty()) {
          (*strings)[i] = (*fallback)[i];
        }
      }
    }
  }

  // The string count, a pointer to each string or 0, then the strings.
  uint32_t table_size = 4 + 4 * uint32_t(strings->size());
  for (const auto& string : *strings) {
    if (!string.empty()) {
      table_size += uint32_t(string.size() + 1) * 2;
    }
  }
  const uint32_t table = memory->SystemHeapAlloc(table_size);
  if (!table) {
    return X_E_FAIL;
  }
  auto table_data = memory->TranslateVirtual<uint8_t*>(table);
  xe::store_and_swap<uint32_t>(table_data, uint32_t(strings->size()));
  uint32_t string_address = table + 4 + 4 * uint32_t(strings->size());
  for (size_t i = 0; i < strings->size(); ++i) {
    const auto& string = (*strings)[i];
    if (string.empty()) {
      xe::store_and_swap<uint32_t>(table_data + 4 + i * 4, 0);
      continue;
    }
    xe::store_and_swap<uint32_t>(table_data + 4 + i * 4, string_address);
    auto chars = memory->TranslateVirtual<char16_t*>(string_address);
    xe::copy_and_swap(chars, string.data(), string.size());
    chars[string.size()] = u'\0';
    string_address += uint32_t(string.size() + 1) * 2;
  }
  {
    std::lock_guard<std::mutex> lock(xui_lock);
    xui_string_tables.insert(table);
  }
  *table_ptr = table;
  return X_E_SUCCESS;
}

uint32_t LookupXuiStringTable(uint32_t table, uint32_t index) {
  {
    std::lock_guard<std::mutex> lock(xui_lock);
    if (!xui_string_tables.contains(table)) {
      return 0;
    }
  }
  auto table_data = kernel_state()->memory()->TranslateVirtual<uint8_t*>(table);
  if (index >= xe::load_and_swap<uint32_t>(table_data)) {
    return 0;
  }
  return xe::load_and_swap<uint32_t>(table_data + 4 + index * 4);
}

dword_result_t XuiLoadStringTableFromFile_entry(lpu16string_t path,
                                                lpdword_t table_ptr) {
  if (!path || !table_ptr) {
    return X_E_INVALIDARG;
  }
  // section://<module handle>,<section>#<file in its package>
  const std::string url = xe::to_utf8(path.value());
  constexpr std::string_view kSectionScheme = "section://";
  if (!xe::utf8::starts_with_case(url, kSectionScheme)) {
    XELOGW("XuiLoadStringTableFromFile: unsupported path {}", url);
    return X_E_FAIL;
  }
  std::string_view location =
      std::string_view(url).substr(kSectionScheme.size());
  const size_t comma = location.find(',');
  uint32_t hmodule = 0;
  if (comma == std::string_view::npos ||
      std::from_chars(location.data(), location.data() + comma, hmodule, 16)
              .ptr != location.data() + comma ||
      !hmodule) {
    return X_E_FAIL;
  }
  location.remove_prefix(comma + 1);
  const size_t hash = location.find('#');
  const std::string section(location.substr(0, hash));
  const std::string_view file_name = hash == std::string_view::npos
                                         ? std::string_view()
                                         : location.substr(hash + 1);

  auto module = XModule::GetFromHModule(
      kernel_state(), kernel_state()->memory()->TranslateVirtual(hmodule));
  if (!module) {
    return X_E_FAIL;
  }
  uint32_t table = *table_ptr;
  const X_HRESULT result =
      LoadXuiStringTable(*module, section, file_name, &table);
  *table_ptr = table;
  return result;
}
DECLARE_XAM_EXPORT1(XuiLoadStringTableFromFile, kUI, kSketchy);

pointer_result_t XuiLookupStringTableByIndex_entry(dword_t table,
                                                   dword_t index) {
  return LookupXuiStringTable(table, index);
}
DECLARE_XAM_EXPORT1(XuiLookupStringTableByIndex, kUI, kImplemented);

dword_result_t XuiFreeStringTable_entry(dword_t table) {
  {
    std::lock_guard<std::mutex> lock(xui_lock);
    if (!xui_string_tables.erase(table)) {
      return X_E_INVALIDARG;
    }
  }
  kernel_state()->memory()->SystemHeapFree(table);
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XuiFreeStringTable, kUI, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(UI);
