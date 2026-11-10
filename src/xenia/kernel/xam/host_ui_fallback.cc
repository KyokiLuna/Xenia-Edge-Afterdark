/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/xam/host_ui_fallback.h"
#include <utility>
#include "xenia/hid/input_system.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/windowed_app_context.h"
namespace xe::kernel::xam {
HostUiFallbackReservation::HostUiFallbackReservation(XamState* xam,
                                                     hid::InputSystem* input)
    : xam_(xam) {
  if (input) {
    auto lock = input->lock();
    input_gap_ = input->ReserveHostUiInput(input->native_input_generation());
  }
}
HostUiFallbackReservation::~HostUiFallbackReservation() { Release(); }
void HostUiFallbackReservation::Release() {
  if (!released_.exchange(true)) {
    xam_->is_xam_dialog_present_.store(false);
    if (input_gap_) {
      input_gap_->Release();
    }
  }
}
void HostUiFallbackCompletion::Finish(Result value) {
  {
    std::lock_guard lock(mutex_);
    if (done_) {
      return;
    }
    result_ = std::move(value);
    done_ = true;
  }
  closed.Signal();
}
HostUiFallbackCompletion::Result HostUiFallbackCompletion::result() const {
  std::lock_guard lock(mutex_);
  return result_;
}
bool QueueHostUiFallback(
    xe::ui::WindowedAppContext& app,
    const std::shared_ptr<NativeUiBackendRoute::HostLease>& lease,
    const std::shared_ptr<HostUiFallbackCompletion>& completion,
    std::function<void(const std::shared_ptr<HostUiFallbackCompletion>&)>
        factory) {
  const std::weak_ptr<HostUiFallbackCompletion> weak = completion;
  lease->SetCancellation([weak] {
    if (auto value = weak.lock()) {
      value->Finish({});
    }
  });
  const bool queued = app.CallInUIThreadDeferred(
      [lease, completion, factory = std::move(factory)]() mutable {
        // No frontend/Kernel/guest pointer access before this exact stop gate.
        auto entered = lease->TryEnter();
        if (!entered) {
          completion->Finish({});
          return;
        }
        // Destroy all factory-local captured owners before entered exits.
        auto create = std::move(factory);
        try {
          create(completion);
        } catch (...) {
          completion->Finish({});
        }
      });
  if (!queued) {
    completion->Finish({});
  }
  return queued;
}
}  // namespace xe::kernel::xam
