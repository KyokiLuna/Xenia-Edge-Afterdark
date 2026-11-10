/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/native_ui_backend_route.h"
#include <condition_variable>
#include <mutex>
#include <utility>
#include "xenia/base/assert.h"
#include "xenia/base/native_system_ui_trace.h"

namespace xe {
thread_local const NativeUiBackendRoute::HostLease::Entry*
    NativeUiBackendRoute::HostLease::Entry::current_ = nullptr;
struct NativeUiBackendRoute::State {
  std::mutex mutex;
  std::condition_variable drained;
  Cohort cohort;
  bool stopping = false, native_sealed = false;
  size_t callbacks = 0;
  uint64_t next_host_serial = 1;
  std::weak_ptr<HostLease> host;
  std::function<void(bool)> visibility;
};
bool NativeUiBackendRoute::Cohort::valid() const {
  return domain && root_load_generation && backend_epoch &&
         foreground_generation && title_load_generation && root_hmodule &&
         title_hmodule;
}
NativeUiBackendRoute::NativeUiBackendRoute(std::function<void(bool)> visibility)
    : state_(std::make_shared<State>()) {
  state_->visibility = std::move(visibility);
}
NativeUiBackendRoute::Admission NativeUiBackendRoute::Admit(
    const Cohort& cohort, Input input, bool allow_host) {
  std::lock_guard lock(state_->mutex);
  if (!cohort.valid() || state_->stopping ||
      (state_->cohort.valid() && state_->cohort != cohort)) {
    return {};
  }
  if (!state_->cohort.valid()) {
    state_->cohort = cohort;
  }
  if (!state_->host.expired()) {
    return {};
  }
  if (input == Input::kReady) {
    // Irreversible even if a subsequent queue/preflight/guest call fails.
    state_->native_sealed = true;
    static xe::NativeSystemUiTraceLimiter trace;
    trace.Emit("transport", "ui_route_native_sealed",
               {{"foreground_generation", cohort.foreground_generation},
                {"backend_epoch", cohort.backend_epoch}});
    return {Decision::kNative, {}};
  }
  // Sealed means no host replay, not permission to create an unrenderable UI.
  if (state_->native_sealed) {
    return {};
  }
  if (!allow_host || input != Input::kProvenNoInitialSwap) {
    return {};
  }
  if (state_->next_host_serial == UINT64_MAX) {
    return {};
  }
  auto host = std::shared_ptr<HostLease>(
      new HostLease(state_, cohort, state_->next_host_serial++));
  state_->host = host;
  static xe::NativeSystemUiTraceLimiter trace;
  trace.Emit("transport", "ui_route_host_reserved",
             {{"foreground_generation", cohort.foreground_generation},
              {"backend_epoch", cohort.backend_epoch},
              {"host_lease_serial", host->serial()}});
  return {Decision::kHost, std::move(host)};
}
std::shared_ptr<NativeUiBackendRoute::HostLease>
NativeUiBackendRoute::CurrentHost(const Cohort& cohort) const {
  std::lock_guard lock(state_->mutex);
  return !state_->stopping && state_->cohort == cohort ? state_->host.lock()
                                                       : nullptr;
}
NativeUiBackendRoute::HostLease::HostLease(std::shared_ptr<State> state,
                                           Cohort cohort, uint64_t serial)
    : state_(std::move(state)), cohort_(cohort), serial_(serial) {}
NativeUiBackendRoute::HostLease::~HostLease() {
  static xe::NativeSystemUiTraceLimiter trace;
  trace.Emit("transport", "ui_route_host_released",
             {{"foreground_generation", cohort_.foreground_generation},
              {"backend_epoch", cohort_.backend_epoch},
              {"host_lease_serial", serial_}});
}
NativeUiBackendRoute::HostLease::Entry::Entry(std::shared_ptr<State> state,
                                              std::shared_ptr<HostLease> lease)
    : state_(std::move(state)), lease_(std::move(lease)) {
  // Constructed only while StateMutex is held, after allocation succeeds.
  ++state_->callbacks;
  previous_ = current_;
  current_ = this;
}
NativeUiBackendRoute::HostLease::Entry::~Entry() {
  assert_true(current_ == this);
  current_ = previous_;
  lease_.reset();
  std::lock_guard lock(state_->mutex);
  --state_->callbacks;
  state_->drained.notify_all();
}
std::unique_ptr<NativeUiBackendRoute::HostLease::Entry>
NativeUiBackendRoute::HostLease::TryEnter() {
  auto self = shared_from_this();
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || cancelled_ || state_->callbacks == SIZE_MAX ||
      state_->host.lock().get() != this) {
    return {};
  }
  return std::unique_ptr<Entry>(new Entry(state_, std::move(self)));
}
bool NativeUiBackendRoute::HostLease::cancelled() const {
  std::lock_guard lock(state_->mutex);
  return state_->stopping || cancelled_;
}
void NativeUiBackendRoute::HostLease::SetCancellation(
    std::function<void()> cb) {
  bool cancelled;
  {
    std::lock_guard lock(state_->mutex);
    cancelled = cancelled_ || state_->stopping;
    if (!cancelled) {
      cancel_ = cb;
    }
  }
  if (cancelled && cb) {
    cb();
  }
}
void NativeUiBackendRoute::HostLease::Cancel() noexcept {
  try {
    std::function<void()> cb;
    {
      std::lock_guard lock(state_->mutex);
      cancelled_ = true;
      cb = std::move(cancel_);
    }
    if (cb) {
      cb();
    }
    static xe::NativeSystemUiTraceLimiter trace;
    trace.Emit("transport", "ui_route_host_cancelled",
               {{"foreground_generation", cohort_.foreground_generation},
                {"backend_epoch", cohort_.backend_epoch},
                {"host_lease_serial", serial_}});
  } catch (...) { /* Cancellation never changes guest/native success. */
  }
}
void NativeUiBackendRoute::HostLease::SetVisible(bool visible) noexcept {
  try {
    auto entered = TryEnter();
    if (!entered) {
      return;
    }
    std::function<void(bool)> cb;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->stopping || cancelled_) {
        return;
      }
      cb = state_->visibility;
    }
    if (cb) {
      cb(visible);
    }
  } catch (...) {
  }
}
void NativeUiBackendRoute::BeginStop() noexcept {
  try {
    std::shared_ptr<HostLease> host;
    {
      std::lock_guard lock(state_->mutex);
      state_->stopping = true;
      host = state_->host.lock();
    }
    if (host) {
      host->Cancel();
    }
  } catch (...) {
  }
}
bool NativeUiBackendRoute::Drain(
    std::chrono::steady_clock::time_point deadline) noexcept {
  try {
    for (auto* p = HostLease::Entry::current_; p; p = p->previous_) {
      if (p->state_.get() == state_.get()) {
        return false;
      }
    }
    std::unique_lock lock(state_->mutex);
    return state_->drained.wait_until(lock, deadline,
                                      [&] { return !state_->callbacks; });
  } catch (...) {
    return false;
  }
}
}  // namespace xe
