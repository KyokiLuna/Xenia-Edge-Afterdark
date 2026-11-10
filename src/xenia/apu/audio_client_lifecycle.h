/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_APU_AUDIO_CLIENT_LIFECYCLE_H_
#define XENIA_APU_AUDIO_CLIENT_LIFECYCLE_H_

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace xe::apu {

// One host registration generation. Guest AU|slot handles retain their legacy
// encoding; generation/epoch protect host work, not stale guest handles.
// No guest calls, driver teardown or module-pin destruction under mutex_.
class AudioClientLifecycle {
 public:
  struct Identity {
    enum class OwnerKind { kModule, kUnclassified };
    uint64_t owner_epoch;
    uint64_t generation;
    uint32_t process_type;
    uint32_t owner_hmodule;
    OwnerKind owner_kind = OwnerKind::kModule;
  };
  enum class Phase {
    kPreparing,
    kReady,
    kActive,
    kRetiring,
    kFailed,
    kRetired
  };

  class CallbackScope {
   public:
    // Construct immediately after BeginCallback, while its registration
    // snapshot is pinned. Unwinding poisons and drains the invocation.
    explicit CallbackScope(AudioClientLifecycle& owner) : owner_(owner) {}
    ~CallbackScope() { owner_.EndCallback(executed_); }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    void SetExecuted(bool executed) { executed_ = executed; }

   private:
    AudioClientLifecycle& owner_;
    bool executed_ = false;
  };

  AudioClientLifecycle(Identity identity, std::shared_ptr<void> module_lease);
  const Identity& identity() const { return identity_; }
  Phase phase() const;
  bool CompletePreparation();
  // The owner must publish its guest output before calling Activate.
  bool Activate();
  bool BeginCallback();
  void EndCallback(bool executed);
  // Only after the sole callback worker has actually joined. Covers thread
  // exit paths that do not unwind C++ invocation scopes.
  void CallbackWorkerStopped();
  bool IsCurrentCallback() const;
  // Failed execution is quarantined until the actual audio worker stops.
  void Retire(bool audio_worker_stopped = false);
  bool WaitForQuiescence();
  bool ClaimCleanup();
  void CompleteCleanup();
  bool WaitForCleanup();

 private:
  const Identity identity_;
  std::shared_ptr<void> module_lease_;
  mutable std::mutex mutex_;
  std::condition_variable drained_;
  Phase phase_ = Phase::kPreparing;
  bool preparing_ = true;
  bool calling_ = false;
  bool cleanup_claimed_ = false;
  std::thread::id callback_thread_;
};

}  // namespace xe::apu
#endif  // XENIA_APU_AUDIO_CLIENT_LIFECYCLE_H_
