/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/graphics_interrupt_registry.h"

#include <algorithm>
#include <utility>

#include "xenia/base/byte_stream.h"
#include "xenia/kernel/xthread.h"

namespace xe::gpu {

GraphicsInterruptRegistry::Registration::Registration(
    Pair pair, uint32_t process_type, uint64_t generation,
    uint32_t owner_hmodule, std::shared_ptr<void> owner_pin)
    : callback_(pair.callback),
      user_data_(pair.user_data),
      process_type_(process_type),
      owner_hmodule_(owner_hmodule),
      generation_(generation),
      owner_pin_(std::move(owner_pin)) {}

GraphicsInterruptRegistry::Registration::InvocationKey
GraphicsInterruptRegistry::Registration::CurrentInvocationKey() {
  if (auto* fiber = kernel::XThread::GetCurrentFiberThread()) {
    return {fiber, {}};
  }
  return {nullptr, std::this_thread::get_id()};
}

bool GraphicsInterruptRegistry::Registration::Invoke(
    const Executor& executor) const {
  const auto key = CurrentInvocationKey();
  {
    std::lock_guard lock(call_mutex_);
    if (retired_ || !callback_ || !executor) {
      return false;
    }
    ++active_calls_;
    if (key.fiber) {
      ++active_fibers_[key.fiber];
    } else {
      ++active_hosts_[key.host];
    }
  }
  Invocation invocation{this, key};
  executor(*this);  // Never hold a registry/admission lock across guest code.
  return true;
}

void GraphicsInterruptRegistry::Registration::EndCall(
    const InvocationKey& key) const {
  std::lock_guard lock(call_mutex_);
  if (key.fiber) {
    auto it = active_fibers_.find(key.fiber);
    if (--it->second == 0) {
      active_fibers_.erase(it);
    }
  } else {
    auto it = active_hosts_.find(key.host);
    if (--it->second == 0) {
      active_hosts_.erase(it);
    }
  }
  if (--active_calls_ == 0) {
    calls_drained_.notify_all();
  }
}

void GraphicsInterruptRegistry::Registration::Retire() const {
  std::lock_guard lock(call_mutex_);
  retired_ = true;
}

bool GraphicsInterruptRegistry::Registration::CanWaitOnCurrentThread() const {
  const auto key = CurrentInvocationKey();
  std::lock_guard lock(call_mutex_);
  return !((key.fiber && active_fibers_.contains(key.fiber)) ||
           (!key.fiber && active_hosts_.contains(key.host)) ||
           (key.fiber && active_calls_));
}

bool GraphicsInterruptRegistry::Registration::StopAndWait() const {
  const auto key = CurrentInvocationKey();
  std::unique_lock lock(call_mutex_);
  retired_ = true;
  if ((key.fiber && active_fibers_.contains(key.fiber)) ||
      (!key.fiber && active_hosts_.contains(key.host)) ||
      (key.fiber && active_calls_)) {
    return false;
  }
  calls_drained_.wait(lock, [this] { return active_calls_ == 0; });
  return true;
}

size_t GraphicsInterruptRegistry::OwnerSlot(uint32_t process_type) {
  return process_type <= 2 ? process_type : 3;
}

void GraphicsInterruptRegistry::Set(Pair pair, uint32_t process_type,
                                    uint32_t owner_hmodule,
                                    std::shared_ptr<void> owner_pin) {
  Snapshot previous;
  Snapshot previous_legacy;
  {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      return;
    }
    auto& slot = owners_[OwnerSlot(process_type)];
    previous = std::move(slot);
    if (previous) {
      previous->Retire();
    }
    if (pair.callback) {
      slot = Snapshot(new Registration(pair, process_type, next_generation_++,
                                       owner_hmodule, std::move(owner_pin)));
      std::erase_if(registrations_,
                    [](const auto& weak) { return weak.expired(); });
      registrations_.push_back(slot);
    }
    previous_legacy = std::move(legacy_);
    legacy_ = slot;
    legacy_pair_ = pair;
  }
  // Last module references may enter loader teardown; release outside mutex_.
}

GraphicsInterruptRegistry::Pair GraphicsInterruptRegistry::legacy_pair() const {
  std::lock_guard lock(mutex_);
  return legacy_pair_;
}

bool GraphicsInterruptRegistry::DispatchLegacy(const Executor& executor) const {
  Snapshot snapshot;
  {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      return false;
    }
    snapshot = legacy_;
  }
  return snapshot && snapshot->Invoke(executor);
}

GraphicsInterruptRegistry::Snapshot GraphicsInterruptRegistry::CaptureOwner(
    uint32_t process_type) const {
  if (process_type > 2) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return {};
  }
  auto token = owners_[process_type];
  return token && token->owner_pin_ ? token : Snapshot{};
}

bool GraphicsInterruptRegistry::DispatchOwner(const Snapshot& token,
                                              uint32_t process_type,
                                              uint64_t generation,
                                              const Executor& executor) const {
  if (!token || process_type > 2 || token->process_type_ != process_type ||
      token->generation_ != generation || !token->owner_pin_) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (stopped_ || owners_[process_type] != token) {
      return false;
    }
  }
  return token->Invoke(executor);
}

std::vector<GraphicsInterruptRegistry::Snapshot>
GraphicsInterruptRegistry::ModuleSnapshots(uint32_t hmodule) const {
  std::vector<Snapshot> live;
  if (hmodule) {
    std::lock_guard lock(mutex_);
    for (const auto& weak : registrations_) {
      if (auto registration = weak.lock();
          registration && registration->owner_hmodule_ == hmodule) {
        live.push_back(std::move(registration));
      }
    }
  }
  return live;
}

bool GraphicsInterruptRegistry::PrepareModuleShutdown(uint32_t hmodule) {
  const auto live = ModuleSnapshots(hmodule);
  for (const auto& registration : live) {
    registration->Retire();
  }
  // Detect every self/fiber dependency before waiting on any other callback.
  // Another admitted callback may itself be waiting for this invocation.
  for (const auto& registration : live) {
    if (!registration->CanWaitOnCurrentThread()) {
      return false;
    }
  }
  return true;
}

bool GraphicsInterruptRegistry::BeginModuleShutdown(uint32_t hmodule) {
  if (!PrepareModuleShutdown(hmodule)) {
    return false;
  }
  const auto live = ModuleSnapshots(hmodule);
  bool drained = true;
  for (const auto& registration : live) {
    drained &= registration->StopAndWait();
  }
  return drained;
}

void GraphicsInterruptRegistry::RetireModule(uint32_t hmodule) {
  if (!hmodule) {
    return;
  }
  std::vector<Snapshot> retired;
  {
    std::lock_guard lock(mutex_);
    for (auto& slot : owners_) {
      if (slot && slot->owner_hmodule_ == hmodule) {
        slot->Retire();
        retired.push_back(std::move(slot));
      }
    }
    if (legacy_ && legacy_->owner_hmodule_ == hmodule) {
      retired.push_back(std::move(legacy_));
      legacy_pair_ = {};
    }
  }
}

bool GraphicsInterruptRegistry::StopAndWait() {
  std::vector<Snapshot> live;
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    for (const auto& weak : registrations_) {
      if (auto registration = weak.lock()) {
        registration->Retire();
        live.push_back(std::move(registration));
      }
    }
  }
  // Do not block on a different registration before discovering that this
  // caller itself must return before the cohort can finish draining.
  for (const auto& registration : live) {
    if (!registration->CanWaitOnCurrentThread()) {
      return false;
    }
  }
  for (const auto& registration : live) {
    if (!registration->StopAndWait()) {
      return false;
    }
  }
  // Last module/code pins may enter the loader. Move their owning references
  // out of the registry and release them only after leaving its lock.
  decltype(owners_) retired;
  Snapshot retired_legacy;
  {
    std::lock_guard lock(mutex_);
    retired.swap(owners_);
    retired_legacy = std::move(legacy_);
    legacy_pair_ = {};
    registrations_.clear();
  }
  return true;
}

bool GraphicsInterruptRegistry::IsStopped() const {
  std::lock_guard lock(mutex_);
  return stopped_;
}

void GraphicsInterruptRegistry::Clear() {
  decltype(owners_) retired;
  Snapshot retired_legacy;
  std::vector<Snapshot> live;
  {
    std::lock_guard lock(mutex_);
    for (const auto& weak : registrations_) {
      if (auto registration = weak.lock()) {
        registration->Retire();
        live.push_back(std::move(registration));
      }
    }
    retired.swap(owners_);
    retired_legacy = std::move(legacy_);
    legacy_pair_ = {};
    // A captured/active old generation must remain discoverable by the final
    // whole-kernel drain even after Clear or a legacy state restore.
    std::erase_if(registrations_,
                  [](const auto& weak) { return weak.expired(); });
  }
}

void GraphicsInterruptRegistry::RestoreLegacy(Pair pair) {
  Clear();
  Set(pair);
}

void GraphicsInterruptRegistry::SaveLegacy(ByteStream* stream) const {
  const auto pair = legacy_pair();
  stream->Write<uint32_t>(pair.callback);
  stream->Write<uint32_t>(pair.user_data);
}

void GraphicsInterruptRegistry::RestoreLegacy(ByteStream* stream) {
  Pair pair;
  pair.callback = stream->Read<uint32_t>();
  pair.user_data = stream->Read<uint32_t>();
  RestoreLegacy(pair);
}

}  // namespace xe::gpu
