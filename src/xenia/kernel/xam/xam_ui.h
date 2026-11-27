/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_UI_H_
#define XENIA_KERNEL_XAM_XAM_UI_H_

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/base/native_ui_backend_route.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace hid {
class InputSystem;
}  // namespace hid
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

class HostUiFallbackCompletion;

// A dialog's lease is a callback rundown, not Kernel ownership. Every draw and
// result snapshot enters it anew. Canceled/stale dialogs may remain in the UI
// queue, but never touch profile/Kernel services and hold only CPU input tokens.
class XamDialog : public xe::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> callback) {
    close_callback_ = std::move(callback);
  }
  void SetHostLease(std::shared_ptr<NativeUiBackendRoute::HostLease> lease) {
    host_lease_ = std::move(lease);
  }
  const auto& host_lease() const { return host_lease_; }
  void SetHostCompletion(std::shared_ptr<HostUiFallbackCompletion> completion) {
    host_completion_=std::move(completion);
  }
  const auto& host_completion() const { return host_completion_; }

 protected:
  explicit XamDialog(xe::ui::ImGuiDrawer* drawer)
      : xe::ui::ImGuiDialog(drawer) {}
  ~XamDialog() override = default;
  void OnDraw(ImGuiIO& io) final {
    auto entered = host_lease_ ? host_lease_->TryEnter() : nullptr;
    if (host_lease_ && !entered) { Close(); return; }
    OnDrawDialog(io);
  }
  virtual void OnDrawDialog(ImGuiIO&) = 0;
  void OnClose() final {
    auto entered = host_lease_ ? host_lease_->TryEnter() : nullptr;
    if ((!host_lease_ || entered) && close_callback_) { close_callback_(); }
  }

 private:
  std::shared_ptr<NativeUiBackendRoute::HostLease> host_lease_;
  std::shared_ptr<HostUiFallbackCompletion> host_completion_;
  std::function<void()> close_callback_;
};

class XamGamepadDialog : public XamDialog {
 protected:
  XamGamepadDialog(xe::ui::ImGuiDrawer* drawer, hid::InputSystem* input)
      : XamDialog(drawer), input_system_(input) {
    if (input) {
      auto lock = input->lock();
      input_ = input->ReserveHostUiInput();
    }
  }
  ~XamGamepadDialog() override = default;
  // Existing achievement scrolling reads it only during the guarded draw.
  // Destruction and delayed/canceled closures never dereference this pointer.
  hid::InputSystem* input_system_ = nullptr;
 private:
  // No raw InputSystem in a delayed dialog destructor. The next real title
  // poll captures held buttons and discards modal keystrokes before unblocking.
  std::shared_ptr<hid::UiInputReservation> input_;
};

class MessageBoxDialog : public XamGamepadDialog {
 public:
  MessageBoxDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                   xe::hid::InputSystem* input_system, const std::string& title,
                   const std::string& description,
                   std::vector<std::string> buttons, uint32_t default_button)
      : XamGamepadDialog(imgui_drawer, input_system),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }
  bool accepted() const { return accepted_; }

  void OnDrawDialog(ImGuiIO& io) override;
  virtual ~MessageBoxDialog() {}

 private:
  bool accepted_ = false;
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
};

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid);

X_RESULT xeXamShowCreateProfileUIEx(uint32_t user_index, uint32_t flag,
                                    char* unknown);
X_RESULT xeXamShowHostCreateProfileUI(Emulator*, uint32_t user_index);

// Picks an installed indie game, or reports that there are none.
bool xeXamChooseIndieGame(std::string* file_name, uint32_t* device_id,
                          std::string* display_name);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
