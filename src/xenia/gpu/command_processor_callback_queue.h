/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_COMMAND_PROCESSOR_CALLBACK_QUEUE_H_
#define XENIA_GPU_COMMAND_PROCESSOR_CALLBACK_QUEUE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

namespace xe::gpu {

// Multiple producers, one command-processor consumer. The owner supplies its
// actual thread identity; callbacks may reenter Submit on that same thread.
// Join all users before destroying the queue, as with the owning CP itself.
class CommandProcessorCallbackQueue {
 public:
  using Callback = std::function<void()>;

  ~CommandProcessorCallbackQueue() {
    // The joined owner may leave callbacks unexecuted. Discard them with all
    // queue state still alive, outside the lock; a captured object's destructor
    // can synchronously submit more cleanup, which must not run after CP stop.
    decltype(callbacks_) discarded;
    {
      std::lock_guard lock(mutex_);
      destroying_ = true;
      discarded.swap(callbacks_);
      has_pending_.store(false, std::memory_order_release);
    }
  }

  void Submit(Callback callback, bool is_command_processor_thread) {
    // Construct outside the lock. Queue entries own pointers so popping or an
    // allocation failure cannot destroy a callable/capture under the lock.
    auto work = std::make_unique<Callback>(std::move(callback));
    {
      std::lock_guard lock(mutex_);
      if (destroying_) {
        return;
      }
      if (!is_command_processor_thread || !callbacks_.empty()) {
        callbacks_.push(std::move(work));
        has_pending_.store(true, std::memory_order_release);
      }
    }
    if (work) {
      (*work)();
    }
  }

  // Owner thread only. Removes exactly one queued callback, then executes it
  // without the lock. Exceptions propagate after unlocking, just as inline
  // callbacks do. An empty std::function still throws when actually invoked.
  bool RunOne() {
    std::unique_ptr<Callback> work;
    {
      std::lock_guard lock(mutex_);
      if (callbacks_.empty()) {
        return false;
      }
      work = std::move(callbacks_.front());
      callbacks_.pop();
      has_pending_.store(!callbacks_.empty(), std::memory_order_release);
    }
    (*work)();
    return true;
  }

  // Idle-loop hint only, not proof that the CP or an executing callback is
  // idle. Actual inline/FIFO decisions and removal always take the mutex.
  bool HasPending() const {
    return has_pending_.load(std::memory_order_acquire);
  }

 private:
  std::mutex mutex_;
  std::queue<std::unique_ptr<Callback>> callbacks_;
  std::atomic<bool> has_pending_{false};
  bool destroying_ = false;
};

}  // namespace xe::gpu

#endif  // XENIA_GPU_COMMAND_PROCESSOR_CALLBACK_QUEUE_H_
