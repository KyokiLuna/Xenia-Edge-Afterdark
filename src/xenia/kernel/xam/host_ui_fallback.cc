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
#include "xenia/kernel/util/host_call_outcome.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/ui/windowed_app_context.h"
namespace xe::kernel::xam {
bool HostUiProfileSelection::valid() const {
  if (!mask || (mask & ~15u)) return false;
  for (uint32_t slot = 0; slot != 4; ++slot) {
    if (bool(mask & (1u << slot)) != bool(xuids[slot])) return false;
    if (!xuids[slot]) continue;
    for (uint32_t prior = 0; prior != slot; ++prior)
      if (xuids[prior] == xuids[slot]) return false;
  }
  return true;
}
void HostUiFallbackWork::Finish(X_RESULT result) {
  // Captured reservation and selected completion epilogues die before the
  // actual event/APC callback. Retained frontend closures contain CPU values.
  work_ = {};
  cancel_ = {};
  if (complete_) (void)InvokeHostCall([&] { complete_(result); });
  complete_ = {};
  if (post_) (void)InvokeHostCall(post_);
  post_ = {};
}
void HostUiFallbackWork::Run() {
  if (claimed_.exchange(true)) return;
  X_RESULT result = X_ERROR_FUNCTION_FAILED;
  const auto invoked = InvokeHostCall([&] { if (work_) result = work_(); });
  if (invoked != HostCallOutcome::kReturned && cancel_)
    (void)InvokeHostCall(cancel_);
  Finish(invoked == HostCallOutcome::kReturned ? result : X_ERROR_FUNCTION_FAILED);
}
void HostUiFallbackWork::Reject(X_RESULT result) {
  if (claimed_.exchange(true)) return;
  if (cancel_) (void)InvokeHostCall(cancel_);
  Finish(result);
}

HostUiFallbackReservation::HostUiFallbackReservation(XamState* xam,
                                                     hid::InputSystem* input,
                                                     bool host_only)
    : xam_(xam) {
  if (input) {
    auto lock = input->lock();
    input_gap_ = host_only
                     ? input->ReserveHostUiInput()
                     : input->ReserveHostUiInput(input->native_input_generation());
  }
}
std::shared_ptr<HostUiFallbackReservation> HostUiFallbackReservation::TryCreate(
    XamState* xam, hid::InputSystem* input, bool host_only) noexcept {
  try {
    auto value = std::make_shared<HostUiFallbackReservation>(xam, input, host_only);
    if (!value->input_gap()) { value->Release(); }
    return value;
  } catch (...) {
    // The caller claimed this exact operation's flag; construction never
    // published UI or handed a raw reservation to a queued factory.
    xam->is_xam_dialog_present_.store(false);
    return {};
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
uint64_t HostUiFallbackCompletion::StartSigninAttempt(HostUiProfileSelection selection) {
  if (!selection.valid()) return 0;
  uint64_t id = 0;
  {
    std::lock_guard lock(mutex_);
    if (done_ || !next_signin_attempt_ || next_signin_attempt_==UINT64_MAX ||
        signin_attempt_.phase==SigninPhase::kQueued ||
        signin_attempt_.phase==SigninPhase::kExecuting ||
        (signin_attempt_.phase==SigninPhase::kReturned &&
         signin_attempt_.status==X_ERROR_SUCCESS)) return 0;
    id=next_signin_attempt_++;
    signin_attempt_={id,SigninPhase::kQueued,selection,X_ERROR_IO_PENDING,false};
  }
  progress.Signal();
  return id;
}
HostUiFallbackCompletion::SigninAttempt HostUiFallbackCompletion::signin_attempt() const {
  std::lock_guard lock(mutex_);return signin_attempt_;
}
std::optional<HostUiFallbackCompletion::SigninAttempt>
HostUiFallbackCompletion::TakeSigninAttempt() {
  std::lock_guard lock(mutex_);
  if(done_ || signin_attempt_.phase!=SigninPhase::kQueued)return {};
  signin_attempt_.phase=SigninPhase::kExecuting;
  return signin_attempt_;
}
bool HostUiFallbackCompletion::CompleteSigninAttempt(uint64_t id,X_RESULT status,bool executed) {
  std::lock_guard lock(mutex_);
  if(!id || signin_attempt_.id!=id || signin_attempt_.phase!=SigninPhase::kExecuting)return false;
  if(status==X_ERROR_SUCCESS && !executed)status=X_ERROR_FUNCTION_FAILED;
  signin_attempt_.status=status;signin_attempt_.executed=executed;
  signin_attempt_.phase=SigninPhase::kReturned;
  if(done_ && executed && status==X_ERROR_SUCCESS){
    // The actual producer committed before/racing UI cancel. Preserve that
    // factual outcome; cancellation cannot fabricate a rollback of sign-in.
    result_.result=xe::ui::TextEntryResult::kAccepted;
    result_.status=status;result_.signin=signin_attempt_.selection;
  }
  return true;
}
bool HostUiFallbackCompletion::closed_requested() const {
  std::lock_guard lock(mutex_);return done_;
}
void HostUiFallbackCompletion::Finish(Result value) {
  {
    std::lock_guard lock(mutex_);
    if (done_) {
      return;
    }
    if(signin_attempt_.phase==SigninPhase::kReturned &&
       signin_attempt_.executed && signin_attempt_.status==X_ERROR_SUCCESS){
      value.result=xe::ui::TextEntryResult::kAccepted;
      value.status=X_ERROR_SUCCESS;value.signin=signin_attempt_.selection;
    }else if(signin_attempt_.phase==SigninPhase::kQueued){
      signin_attempt_.phase=SigninPhase::kReturned;
      signin_attempt_.status=X_ERROR_CANCELLED; // never dispatched
    }
    result_ = std::move(value);
    done_ = true;
  }
  closed.Signal();
  progress.Signal();
}
HostUiFallbackCompletion::Result HostUiFallbackCompletion::result() const {
  std::lock_guard lock(mutex_);
  return result_;
}
X_RESULT RunHostSigninAttempts(
    const std::shared_ptr<NativeUiBackendRoute::HostLease>& lease,
    const std::shared_ptr<HostUiFallbackCompletion>& completion,
    const std::function<X_RESULT(const HostUiProfileSelection&,
                                const std::function<bool()>&)>& publish) {
  for (;;) {
    if(const auto attempt=completion->TakeSigninAttempt()){
      X_RESULT status=X_ERROR_CANCELLED;bool executed=false;
      const auto invoked=InvokeHostCall([&] {
        auto entered=lease->TryEnter();
        if(entered && !completion->closed_requested()){
          const std::function<bool()> canceled=[lease,completion] {
            return lease->cancelled() || completion->closed_requested();
          };
          executed=true;
          status=publish ? publish(attempt->selection,canceled) : X_ERROR_FUNCTION_FAILED;
        }
      });
      if(invoked!=HostCallOutcome::kReturned)status=X_ERROR_FUNCTION_FAILED;
      // Result is published after the actual adoption frame and Entry unwind.
      // Only the exact current attempt can change its CPU outcome.
      if(!completion->CompleteSigninAttempt(attempt->id,status,executed))return X_ERROR_FUNCTION_FAILED;
      continue;
    }
    if(completion->closed_requested()){
      const auto result=completion->result();
      return result.result==xe::ui::TextEntryResult::kAccepted
          ? result.status : X_ERROR_CANCELLED;
    }
    GuestScheduler::WaitOnFence(completion->progress);
  }
}
HostUiFallbackOperationCompletion::~HostUiFallbackOperationCompletion() {
  if (lease_) {
    lease_->SetVisible(false);
    lease_->FinishHostOperation();
  }
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
