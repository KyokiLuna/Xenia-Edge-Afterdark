/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_TITLE_EXECUTION_STATE_H_
#define XENIA_KERNEL_TITLE_EXECUTION_STATE_H_

#include <cstdint>
#include <mutex>

namespace xe::kernel {

// Host-owned title execution admission, independent of loaded-module metadata.
// An assigned title ID or a resumed thread is not proof of guest execution.
// The owner binds a suspended main thread and publishes Running only when that
// exact thread actually reaches its guest entry path. No guest addresses are
// dereferenced by this record.
class TitleExecutionState {
 public:
  enum class Phase { kUnloaded, kPreparing, kRunning, kStopping };

  struct Snapshot {
    uint64_t generation = 0;
    Phase phase = Phase::kUnloaded;
    uint32_t title_id = 0;
    uint32_t hmodule = 0;
    uint32_t main_thread_id = 0;
    bool operator==(const Snapshot&) const = default;
  };

  // Only an unloaded record can begin another attempt. Zero is never a valid
  // generation and is returned on rejection or generation exhaustion.
  uint64_t BeginPreparing();
  // Bind once, while Preparing. Repeating the identical identity is harmless;
  // changing it requires cancelling/stopping and a fresh generation. A zero
  // title ID is valid for an executable without execution-info metadata.
  bool SetPreparedIdentity(uint64_t generation, uint32_t title_id,
                           uint32_t hmodule, uint32_t main_thread_id);
  bool MarkRunning(uint64_t generation, uint32_t hmodule,
                   uint32_t main_thread_id);

  // Zero selects the current generation. Repeated stopping of the same live
  // generation is idempotent. Completing/cancelling retains the generation
  // number, but clears all identity fields and rejects stale notifications.
  uint64_t BeginStopping(uint64_t generation = 0);
  bool CompleteStop(uint64_t generation);
  bool CancelPreparing(uint64_t generation);

  Snapshot snapshot() const;
  // Zero selects the current generation. A stale generation or any phase
  // other than Running returns zero, without exposing a preparing title ID.
  uint32_t active_title_id(uint64_t generation = 0) const;

 private:
  void ClearIdentity();

  mutable std::mutex mutex_;
  Snapshot snapshot_;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_TITLE_EXECUTION_STATE_H_
