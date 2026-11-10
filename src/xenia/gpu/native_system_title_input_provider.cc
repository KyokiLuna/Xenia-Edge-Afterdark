/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_system_title_input_provider.h"
namespace xe::gpu {
bool NativeSystemTitleInputProvider::Enable(uint64_t epoch) {
  std::lock_guard lock(mutex_);
  if (!epoch || closed_ || (backend_epoch_ && backend_epoch_ != epoch)) {
    return false;
  }
  backend_epoch_ = epoch;
  return true;
}
bool NativeSystemTitleInputProvider::enabled() const {
  std::lock_guard lock(mutex_);
  return backend_epoch_ && !closed_;
}
uint64_t NativeSystemTitleInputProvider::Stage(const Input::Swap& swap) {
  const auto layout = Input::GetLayout(swap);
  if (!swap.foreground_generation || !swap.title_hmodule ||
      !swap.title_load_generation) {
    return 0;
  }
  std::shared_ptr<const Input> released;
  std::lock_guard lock(mutex_);
  if (!backend_epoch_ || closed_ || swap.foreground_generation < foreground_ ||
      (foreground_ == swap.foreground_generation &&
       (title_hmodule_ != swap.title_hmodule ||
        title_load_generation_ != swap.title_load_generation))) {
    return 0;
  }
  staged_any_ = true;  // Sticky even for unsupported layouts/backpressure.
  if (foreground_ != swap.foreground_generation) {
    ++invalidation_revision_;
    foreground_ = swap.foreground_generation;
    title_hmodule_ = swap.title_hmodule;
    title_load_generation_ = swap.title_load_generation;
    for (auto& entry : pending_) {
      entry.ticket = 0;
    }
    latest_.swap(released);
  }
  if (!layout) {
    ++invalidation_revision_;
    for (auto& entry : pending_) {
      entry.ticket = 0;
    }
    if (latest_) {
      latest_.swap(released);
    }
    return 0;
  }
  if (next_ticket_ == UINT64_MAX) {
    return 0;
  }
  const uint64_t ticket = next_ticket_;
  auto& entry = pending_[ticket % kPendingSlots];
  if (entry.ticket) {
    return 0;  // Bounded backpressure; never overwrite a ticket.
  }
  ++next_ticket_;
  entry = {ticket, swap};
  return ticket;
}
std::optional<NativeSystemTitleInputProvider::Capture>
NativeSystemTitleInputProvider::Take(uint64_t ticket, uint32_t physical,
                                     uint32_t width, uint32_t height) {
  Input::Swap swap;
  uint64_t epoch;
  {
    std::lock_guard lock(mutex_);
    if (!ticket || !backend_epoch_ || closed_) {
      return {};
    }
    auto& entry = pending_[ticket % kPendingSlots];
    if (entry.ticket != ticket) {
      return {};
    }
    swap = entry.swap;
    entry.ticket = 0;  // Consume once even when the guest packet is mismatched.
    epoch = backend_epoch_;
  }
  const auto layout = Input::GetLayout(swap);
  if (!layout || layout->range.address != physical || swap.width != width ||
      swap.height != height) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (closed_ || backend_epoch_ != epoch ||
      foreground_ != swap.foreground_generation ||
      title_hmodule_ != swap.title_hmodule ||
      title_load_generation_ != swap.title_load_generation ||
      next_capture_ == UINT64_MAX || capturing_) {
    return {};
  }
  capturing_revision_ = invalidation_revision_;
  capturing_ = Capture{{epoch, foreground_, title_hmodule_,
                        title_load_generation_, next_capture_++},
                       *layout};
  return capturing_;
}
bool NativeSystemTitleInputProvider::Publish(
    std::shared_ptr<const Input> input) {
  if (!input || !input->valid()) {
    return false;
  }
  std::shared_ptr<const Input> released;
  {
    std::lock_guard lock(mutex_);
    const auto& id = input->identity();
    if (!capturing_ || id != capturing_->identity) {
      return false;
    }
    const auto& actual = input->layout();
    const auto& expected = capturing_->layout;
    if (actual.swap.fetch != expected.swap.fetch ||
        actual.swap.width != expected.swap.width ||
        actual.swap.height != expected.swap.height ||
        actual.fetch != expected.fetch ||
        actual.range.address != expected.range.address ||
        actual.range.length != expected.range.length ||
        actual.data_extent_bytes != expected.data_extent_bytes) {
      return false;
    }
    capturing_.reset();
    if (capturing_revision_ != invalidation_revision_ || closed_ ||
        id.backend_epoch != backend_epoch_ ||
        id.foreground_generation != foreground_ ||
        id.title_hmodule != title_hmodule_ ||
        id.title_load_generation != title_load_generation_ ||
        id.swap_serial >= next_capture_ ||
        (latest_ && latest_->identity().swap_serial >= id.swap_serial)) {
      return false;
    }
    latest_.swap(released);
    latest_ = std::move(input);
  }
  return true;
}
std::shared_ptr<const NativeSystemTitleInputProvider::Input>
NativeSystemTitleInputProvider::Latest(uint64_t foreground) const {
  std::lock_guard lock(mutex_);
  return !closed_ && backend_epoch_ && foreground &&
                 foreground == foreground_ && latest_ &&
                 latest_->identity().backend_epoch == backend_epoch_
             ? latest_
             : nullptr;
}
NativeSystemTitleInputProvider::Availability
NativeSystemTitleInputProvider::ObserveAvailability(
    uint64_t epoch, uint64_t foreground, uint32_t title,
    uint64_t load_generation) const {
  std::lock_guard lock(mutex_);
  if (!epoch || !foreground || !title || !load_generation || closed_ ||
      backend_epoch_ != epoch) {
    return Availability::kUnknown;
  }
  if (!staged_any_) {
    return Availability::kProvenNoInitialSwap;
  }
  if (foreground_ != foreground || title_hmodule_ != title ||
      title_load_generation_ != load_generation) {
    return Availability::kUnknown;
  }
  return latest_ && latest_->valid() &&
                 latest_->identity().backend_epoch == epoch &&
                 latest_->identity().foreground_generation == foreground &&
                 latest_->identity().title_hmodule == title &&
                 latest_->identity().title_load_generation == load_generation
             ? Availability::kReady
             : Availability::kUnknown;
}
void NativeSystemTitleInputProvider::Close() {
  std::shared_ptr<const Input> released;
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    capturing_.reset();
    ++invalidation_revision_;
    for (auto& entry : pending_) {
      entry.ticket = 0;
    }
    latest_.swap(released);
  }
}
}  // namespace xe::gpu
