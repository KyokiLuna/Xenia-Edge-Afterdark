/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_runtime.h"

#include <utility>

#include "xenia/base/assert.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {

GuestRuntime::GuestRuntime(Identity identity, std::shared_ptr<void> owner_pin)
    : owner_pin_(std::move(owner_pin)) {
  snapshot_.identity = identity;
  if (!identity.hmodule || !identity.generation || !owner_pin_) {
    snapshot_.state = State::kFailed;
    snapshot_.failure = Failure{{}, 0, "invalid runtime owner or generation"};
  }
}

GuestRuntime::~GuestRuntime() {
  std::shared_ptr<void> release;
  {
    std::lock_guard lock(mutex_);
    // Shared runtimes pin themselves during execution. Stack-owned records,
    // like every other concurrent object, must outlive their caller threads.
    assert_false(snapshot_.initialization_active);
    snapshot_.state = State::kRetiring;
    release = std::move(owner_pin_);
  }
}

GuestRuntime::InvocationKey GuestRuntime::CurrentInvocationKey() {
  if (auto* fiber = XThread::GetCurrentFiberThread()) {
    return {fiber, {}};
  }
  return {nullptr, std::this_thread::get_id()};
}

bool GuestRuntime::ValidatePlan(const Plan& plan) {
  if (plan.identity.empty() || plan.phases.empty()) {
    return false;
  }
  for (const auto& phase : plan.phases) {
    if (phase.identity.empty() || phase.functions.empty()) {
      return false;
    }
    for (const auto& function : phase.functions) {
      if (function.identity.empty() || !function.address ||
          (function.address & 3) ||
          function.return_kind == ReturnKind::kUnspecified ||
          function.abi_metadata.empty()) {
        return false;
      }
    }
  }
  return true;
}

GuestRuntime::InitStatus GuestRuntime::Initialize(Plan plan,
                                                  const Executor& executor) {
  [[maybe_unused]] auto keep_alive = weak_from_this().lock();
  // A callback may erase the registry or storage that supplied it. Own the
  // executor as well as the runtime, and destroy the copy before keep_alive.
  Executor owned_executor = executor;
  const InvocationKey key = CurrentInvocationKey();
  std::shared_ptr<const Plan> owned_plan;
  {
    std::lock_guard lock(mutex_);
    switch (snapshot_.state) {
      case State::kInitializing:
        return invocation_ == key ? InitStatus::kReentrant : InitStatus::kBusy;
      case State::kReady:
        return snapshot_.plan && *snapshot_.plan == plan
                   ? InitStatus::kAlreadyReady
                   : InitStatus::kPlanMismatch;
      case State::kFailed:
        return InitStatus::kFailed;
      case State::kRetiring:
        return InitStatus::kRetiring;
      case State::kResourcesLoaded:
        break;
    }
    if (!owned_executor || !ValidatePlan(plan)) {
      return InitStatus::kInvalidPlan;
    }
    owned_plan = std::make_shared<const Plan>(std::move(plan));
    snapshot_.plan = owned_plan;
    snapshot_.state = State::kInitializing;
    snapshot_.initialization_active = true;
    invocation_ = key;
    for (const auto& phase : owned_plan->phases) {
      snapshot_.total_functions += phase.functions.size();
    }
  }

  for (size_t phase = 0; phase < owned_plan->phases.size(); ++phase) {
    const auto& functions = owned_plan->phases[phase].functions;
    for (size_t function = 0; function < functions.size(); ++function) {
      const Position position{phase, function};
      bool retiring;
      {
        std::lock_guard lock(mutex_);
        retiring = snapshot_.state == State::kRetiring;
        if (!retiring) {
          // This is this entry's admission point. Retirement drains an entry
          // admitted before it and prevents any later entry from admission.
          snapshot_.current_function = position;
        }
      }
      if (retiring) {
        return FinishInitialization(false);
      }
      ExecutionResult result;
      try {
        // Guest execution must never inherit the runtime's host mutex.
        result = owned_executor(functions[function]);
      } catch (...) {
        FinishInitialization(false, Failure{position, 0, "executor exception"});
        throw;
      }
      if (!result.succeeded) {
        return FinishInitialization(
            false,
            Failure{position, result.return_value, std::move(result.detail)});
      }
      {
        std::lock_guard lock(mutex_);
        ++snapshot_.completed_functions;
      }
    }
  }
  return FinishInitialization(true);
}

GuestRuntime::InitStatus GuestRuntime::FinishInitialization(
    bool succeeded, std::optional<Failure> failure) {
  std::shared_ptr<void> release;
  InitStatus result;
  {
    std::lock_guard lock(mutex_);
    snapshot_.failure = std::move(failure);
    snapshot_.current_function.reset();
    snapshot_.initialization_active = false;
    if (snapshot_.state == State::kRetiring) {
      release = std::move(owner_pin_);
      result = InitStatus::kRetiring;
    } else {
      snapshot_.state = succeeded ? State::kReady : State::kFailed;
      result = succeeded ? InitStatus::kReady : InitStatus::kFailed;
    }
    drained_.notify_all();
  }
  return result;
}

GuestRuntime::Snapshot GuestRuntime::GetSnapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

GuestRuntime::DrainStatus GuestRuntime::RetireAndDrain(bool allow_wait) {
  [[maybe_unused]] auto keep_alive = weak_from_this().lock();
  const InvocationKey key = CurrentInvocationKey();
  std::shared_ptr<void> release;
  {
    std::unique_lock lock(mutex_);
    snapshot_.state = State::kRetiring;
    if (snapshot_.initialization_active) {
      if (invocation_ == key) {
        return DrainStatus::kSelfCall;
      }
      if (!allow_wait || key.fiber) {
        return DrainStatus::kWouldBlock;
      }
      drained_.wait(lock, [this] { return !snapshot_.initialization_active; });
    }
    release = std::move(owner_pin_);
  }
  return DrainStatus::kDrained;
}

}  // namespace xe::kernel
