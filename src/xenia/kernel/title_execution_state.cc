/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/title_execution_state.h"

#include <limits>

namespace xe::kernel {

uint64_t TitleExecutionState::BeginPreparing() {
  std::lock_guard lock(mutex_);
  if (snapshot_.phase != Phase::kUnloaded ||
      snapshot_.generation == std::numeric_limits<uint64_t>::max()) {
    return 0;
  }
  ++snapshot_.generation;
  snapshot_.phase = Phase::kPreparing;
  return snapshot_.generation;
}

bool TitleExecutionState::SetPreparedIdentity(uint64_t generation,
                                              uint32_t title_id,
                                              uint32_t hmodule,
                                              uint32_t main_thread_id) {
  std::lock_guard lock(mutex_);
  if (!generation || generation != snapshot_.generation ||
      snapshot_.phase != Phase::kPreparing || !hmodule || !main_thread_id) {
    return false;
  }
  if (snapshot_.hmodule || snapshot_.main_thread_id) {
    return snapshot_.title_id == title_id && snapshot_.hmodule == hmodule &&
           snapshot_.main_thread_id == main_thread_id;
  }
  snapshot_.title_id = title_id;
  snapshot_.hmodule = hmodule;
  snapshot_.main_thread_id = main_thread_id;
  return true;
}

bool TitleExecutionState::MarkRunning(uint64_t generation, uint32_t hmodule,
                                      uint32_t main_thread_id) {
  std::lock_guard lock(mutex_);
  if (!generation || generation != snapshot_.generation ||
      snapshot_.phase != Phase::kPreparing || !hmodule || !main_thread_id ||
      snapshot_.hmodule != hmodule ||
      snapshot_.main_thread_id != main_thread_id) {
    return false;
  }
  snapshot_.phase = Phase::kRunning;
  return true;
}

uint64_t TitleExecutionState::BeginStopping(uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (snapshot_.phase == Phase::kUnloaded ||
      (generation && generation != snapshot_.generation)) {
    return 0;
  }
  snapshot_.phase = Phase::kStopping;
  return snapshot_.generation;
}

bool TitleExecutionState::CompleteStop(uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (!generation || generation != snapshot_.generation ||
      snapshot_.phase != Phase::kStopping) {
    return false;
  }
  snapshot_.phase = Phase::kUnloaded;
  ClearIdentity();
  return true;
}

bool TitleExecutionState::CancelPreparing(uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (!generation || generation != snapshot_.generation ||
      snapshot_.phase != Phase::kPreparing) {
    return false;
  }
  snapshot_.phase = Phase::kUnloaded;
  ClearIdentity();
  return true;
}

TitleExecutionState::Snapshot TitleExecutionState::snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

uint32_t TitleExecutionState::active_title_id(uint64_t generation) const {
  std::lock_guard lock(mutex_);
  return snapshot_.phase == Phase::kRunning &&
                 (!generation || generation == snapshot_.generation)
             ? snapshot_.title_id
             : 0;
}

void TitleExecutionState::ClearIdentity() {
  snapshot_.title_id = 0;
  snapshot_.hmodule = 0;
  snapshot_.main_thread_id = 0;
}

}  // namespace xe::kernel
