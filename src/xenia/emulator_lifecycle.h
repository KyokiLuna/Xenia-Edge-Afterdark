/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
namespace xe {
class Emulator;
namespace app {
class EmulatorApp;
}
namespace kernel::test {
class EmulatorLifecycleTestAccess;
}
// Host command ownership only. A queued/returned command is never guest title,
// native drain, publication, save-flush or GPU completion evidence.
class EmulatorLifecycle final {
  struct State;

 public:
  enum class Kind { kLaunch, kReplace, kReset, kQuit, kReplaceProcess };
  struct Request {
    Kind kind = Kind::kLaunch;
    uint64_t expected_generation = 0;
    std::function<void()> work;
    std::function<void()> prepare_ui;
    std::function<bool()> after_shutdown_ui;
    template <class F>
    void SetWork(F&& function) {
      // Existing launch reservations are move-only. One owned payload keeps
      // them and copied launch arguments alive without a detached host owner.
      auto owned = std::make_shared<std::decay_t<F>>(std::forward<F>(function));
      work = [owned] { (*owned)(); };
    }
  };

 private:
  struct Command {
    uint64_t serial = 0;
    Request request;
    uint64_t prepared_producer = 0;
  };

 public:
  class Producer final {
   public:
    Producer() = default;
    Producer(Producer&& other) noexcept
        : state_(std::move(other.state_)),
          serial_(std::exchange(other.serial_, 0)) {}
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;
    ~Producer();
    explicit operator bool() const noexcept { return bool(state_); }

   private:
    friend class EmulatorLifecycle;
    Producer(std::shared_ptr<State> state, uint64_t serial)
        : state_(std::move(state)), serial_(serial) {}
    std::shared_ptr<State> state_;
    uint64_t serial_ = 0;
  };
  // One host-owned request prepared before entering PPC. It carries no guest
  // admission or native completion authority. An unpublished payload must be
  // released at its owner's safe host cleanup boundary.
  class PreparedRequest final {
   public:
    PreparedRequest() = default;
    PreparedRequest(PreparedRequest&&) noexcept = default;
    PreparedRequest(const PreparedRequest&) = delete;
    PreparedRequest& operator=(const PreparedRequest&) = delete;
    explicit operator bool() const noexcept { return !command_.empty(); }

   private:
    friend class EmulatorLifecycle;
    PreparedRequest(std::shared_ptr<State> state, uint64_t producer,
                    std::list<Command>&& command) noexcept
        : state_(std::move(state)), producer_(producer),
          command_(std::move(command)) {}
    std::shared_ptr<State> state_;
    uint64_t producer_ = 0;
    std::list<Command> command_;
  };
  class Work final {
   public:
    Work(Work&&) = default;
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;
    ~Work();

   private:
    friend class EmulatorLifecycle;
    friend class Emulator;
    friend class app::EmulatorApp;
    friend class kernel::test::EmulatorLifecycleTestAccess;
    explicit Work(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
    std::list<Command> command_;
    bool host_returned_ = false, shutdown_returned_ = false;
    Kind kind() const;
  };
  explicit EmulatorLifecycle(std::function<void()> wake);
  bool Submit(Request request) noexcept;
  Producer AdmitProducer() noexcept;
  std::optional<PreparedRequest> Prepare(Request request,
                                       const Producer& producer) noexcept;
  bool PublishPreparedWithoutWake(PreparedRequest& request,
                                 const Producer& producer) noexcept;
  bool closing() const noexcept;
  bool final_ui_approved() const noexcept;
  bool DeferUserClose() noexcept;

 private:
  friend class Emulator;
  friend class app::EmulatorApp;
  friend class kernel::test::EmulatorLifecycleTestAccess;
  struct State {
    mutable std::mutex mutex;
    Emulator* owner = nullptr;
    std::thread::id worker;
    std::function<void()> wake;
    std::list<Command> pending, retired;
    uint64_t next = 1, active = 0, producers = 0;
    uint64_t next_producer = 1, active_generation = 0, active_prepared = 0;
    Kind active_kind = Kind::kLaunch;
    bool closing = false, stopped = false, failed = false, ui_approved = false;
  };
  std::shared_ptr<State> state_;
  void Wake() const noexcept;
  bool Install(Emulator*) noexcept;
  bool BindWorker() noexcept;
  std::optional<Work> Take(uint64_t current_generation) noexcept;
  std::list<Command> RetirePendingOnWorker() noexcept;
  bool Pending() const noexcept;
  bool Preparing() const noexcept;
  bool Failed() const noexcept;
  bool IsActive(const Work&, Emulator*, bool terminal) const noexcept;
  bool IsActivePreparedWorker(Emulator*, uint64_t generation) const noexcept;
  bool Execute(Work&);
  bool Finish(Work&) noexcept;
  bool MarkShutdownReturned(Work&, Emulator*) noexcept;
  bool ApproveFinalUiClose() noexcept;
};
inline EmulatorLifecycle::EmulatorLifecycle(std::function<void()> wake)
    : state_(std::make_shared<State>()) {
  state_->wake = std::move(wake);
}
inline void EmulatorLifecycle::Wake() const noexcept {
  try {
    if (state_->wake) {
      state_->wake();
    }
  } catch (...) { /* A wake failure is not command completion. */
  }
}
inline EmulatorLifecycle::Producer::~Producer() {
  if (!state_) {
    return;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->producers) {
      --state_->producers;
    } else {
      state_->failed = true;
    }
  }
  try {
    if (state_->wake) {
      state_->wake();
    }
  } catch (...) {
  }
}
inline EmulatorLifecycle::Work::~Work() {
  if (!state_ || command_.empty()) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  if (state_->active == command_.front().serial) {
    state_->failed = true;
  }
}
inline EmulatorLifecycle::Kind EmulatorLifecycle::Work::kind() const {
  return command_.front().request.kind;
}
inline bool EmulatorLifecycle::Install(Emulator* owner) noexcept {
  std::lock_guard lock(state_->mutex);
  if (!owner || state_->owner || state_->worker != std::thread::id{}) {
    return false;
  }
  state_->owner = owner;
  return true;
}
inline bool EmulatorLifecycle::BindWorker() noexcept {
  std::lock_guard lock(state_->mutex);
  if (!state_->owner || state_->worker != std::thread::id{}) {
    return false;
  }
  state_->worker = std::this_thread::get_id();
  return true;
}
inline EmulatorLifecycle::Producer EmulatorLifecycle::AdmitProducer() noexcept {
  std::lock_guard lock(state_->mutex);
  if (!state_->owner || state_->closing || state_->active || state_->failed ||
      state_->producers == UINT64_MAX || !state_->next_producer ||
      state_->next_producer == UINT64_MAX) {
    return {};
  }
  ++state_->producers;
  return Producer(state_, state_->next_producer++);
}
inline std::optional<EmulatorLifecycle::PreparedRequest>
EmulatorLifecycle::Prepare(Request request, const Producer& producer) noexcept try {
  if (request.kind != Kind::kReplace || !request.expected_generation ||
      !request.work || request.prepare_ui || request.after_shutdown_ui) {
    return {};
  }
  // All payload and node allocation precedes the native publication cut.
  std::list<Command> incoming;
  incoming.push_back({0, std::move(request)});
  {
    std::lock_guard lock(state_->mutex);
    if (producer.state_ != state_ || !producer.serial_ ||
        !state_->owner || !state_->producers || state_->closing ||
        state_->stopped || state_->failed || state_->active) {
      return {};
    }
  }
  return PreparedRequest(state_, producer.serial_, std::move(incoming));
} catch (...) {
  return {};
}
inline bool EmulatorLifecycle::PublishPreparedWithoutWake(
    PreparedRequest& request, const Producer& producer) noexcept try {
  std::lock_guard lock(state_->mutex);
  if (request.state_ != state_ || producer.state_ != state_ ||
      !producer.serial_ || request.producer_ != producer.serial_ ||
      request.command_.size() != 1 || request.command_.front().serial ||
      request.command_.front().prepared_producer || !state_->owner ||
      !state_->producers || state_->closing || state_->stopped ||
      state_->failed || state_->active || !state_->next ||
      state_->next == UINT64_MAX) {
    return false;
  }
  auto& command = request.command_.front();
  if (command.request.kind != Kind::kReplace ||
      !command.request.expected_generation || !command.request.work ||
      command.request.prepare_ui || command.request.after_shutdown_ui) {
    return false;
  }
  command.serial = state_->next++;
  command.prepared_producer = producer.serial_;
  state_->retired.splice(state_->retired.end(), state_->pending);
  state_->pending.splice(state_->pending.end(), request.command_);
  // No allocation, payload destruction, callback or wake. The exact Producer
  // remains held until the caller's real return and wakes the existing worker
  // only when that owner releases it outside the native frame.
  return true;
} catch (...) {
  return false;
}
inline bool EmulatorLifecycle::DeferUserClose() noexcept {
  if (final_ui_approved()) {
    return false;
  }
  Request quit;
  quit.kind = Kind::kQuit;
  (void)Submit(std::move(quit));
  return true;  // Failure is not permission to destroy live providers.
}
inline bool EmulatorLifecycle::Submit(Request request) noexcept try {
  if ((request.kind == Kind::kQuit &&
       (request.work || request.prepare_ui || request.after_shutdown_ui)) ||
      (request.kind == Kind::kReplaceProcess && request.work)) {
    return false;
  }
  std::list<Command> incoming;
  incoming.push_back({0, std::move(request)});
  bool accepted = false;
  {
    std::lock_guard lock(state_->mutex);
    const auto kind = incoming.front().request.kind;
    if (!state_->owner || state_->stopped || state_->failed) {
      return false;
    }
    if (state_->closing) {
      if (kind != Kind::kQuit) {
        return false;
      }
      // A global UI quit may supersede a not-yet-started guest handoff.
      if (!incoming.front().request.expected_generation &&
          !state_->pending.empty()) {
        state_->retired.splice(state_->retired.end(), state_->pending);
      } else {
        return true;  // Coalesce; never cancel an active native operation.
      }
    }
    if (!state_->next || state_->next == UINT64_MAX) {
      return false;
    }
    incoming.front().serial = state_->next++;
    state_->retired.splice(state_->retired.end(), state_->pending);
    state_->pending.splice(state_->pending.end(), incoming);
    if (kind == Kind::kQuit || kind == Kind::kReplaceProcess) {
      state_->closing = true;
    }
    accepted = true;
  }
  Wake();
  return accepted;
} catch (...) {
  return false;
}
inline bool EmulatorLifecycle::closing() const noexcept {
  std::lock_guard lock(state_->mutex);
  return state_->closing;
}
inline bool EmulatorLifecycle::final_ui_approved() const noexcept {
  std::lock_guard lock(state_->mutex);
  return state_->ui_approved;
}
inline bool EmulatorLifecycle::Pending() const noexcept {
  std::lock_guard lock(state_->mutex);
  return !state_->pending.empty();
}
inline bool EmulatorLifecycle::Preparing() const noexcept {
  std::lock_guard lock(state_->mutex);
  return state_->producers != 0;
}
inline bool EmulatorLifecycle::Failed() const noexcept {
  std::lock_guard lock(state_->mutex);
  return state_->failed;
}
inline std::optional<EmulatorLifecycle::Work> EmulatorLifecycle::Take(
    uint64_t generation) noexcept {
  std::lock_guard lock(state_->mutex);
  if (state_->worker != std::this_thread::get_id() || state_->active ||
      state_->producers || state_->failed) {
    return {};
  }
  while (!state_->pending.empty()) {
    auto first = state_->pending.begin();
    const auto& request = first->request;
    if (request.expected_generation &&
        request.expected_generation != generation) {
      if (request.kind == Kind::kQuit ||
          request.kind == Kind::kReplaceProcess) {
        state_->closing = false;
      }
      state_->retired.splice(state_->retired.end(), state_->pending, first);
      continue;
    }
    Work work(state_);
    state_->active = first->serial;
    state_->active_kind = request.kind;
    state_->active_generation = request.expected_generation;
    state_->active_prepared = first->prepared_producer;
    work.command_.splice(work.command_.end(), state_->pending, first);
    return std::optional<Work>(std::move(work));
  }
  return {};
}
inline std::list<EmulatorLifecycle::Command>
EmulatorLifecycle::RetirePendingOnWorker() noexcept {
  std::list<Command> retired;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->worker == std::this_thread::get_id()) {
      retired.splice(retired.end(), state_->retired);
    }
  }
  return retired;  // All payload destruction is after the metadata lock, on
                   // worker.
}
inline bool EmulatorLifecycle::IsActive(const Work& work, Emulator* owner,
                                        bool terminal) const noexcept {
  std::lock_guard lock(state_->mutex);
  return work.state_ == state_ && !work.command_.empty() &&
         state_->owner == owner &&
         state_->worker == std::this_thread::get_id() && !state_->failed &&
         state_->active == work.command_.front().serial &&
         (!terminal ||
          (state_->closing && (work.kind() == Kind::kQuit ||
                               work.kind() == Kind::kReplaceProcess)));
}
inline bool EmulatorLifecycle::Execute(Work& work) {
  if (!IsActive(work, state_->owner, false) || work.host_returned_ ||
      work.kind() == Kind::kQuit || work.kind() == Kind::kReplaceProcess) {
    return false;
  }
  if (work.command_.front().request.work) {
    work.command_.front().request.work();
  }
  work.host_returned_ = true;
  return true;  // Host return only; no guest result claimed.
}
inline bool EmulatorLifecycle::IsActivePreparedWorker(
    Emulator* owner, uint64_t generation) const noexcept {
  std::lock_guard lock(state_->mutex);
  return owner && state_->owner == owner && generation &&
         state_->worker == std::this_thread::get_id() && state_->active &&
         state_->active_kind == Kind::kReplace &&
         state_->active_generation == generation && state_->active_prepared &&
         !state_->stopped && !state_->failed;
}
inline bool EmulatorLifecycle::MarkShutdownReturned(Work& work,
                                                    Emulator* owner) noexcept {
  if (!IsActive(work, owner, true)) {
    return false;
  }
  work.shutdown_returned_ = true;
  return true;
}
inline bool EmulatorLifecycle::Finish(Work& work) noexcept {
  std::lock_guard lock(state_->mutex);
  if (work.state_ != state_ || work.command_.empty() ||
      state_->worker != std::this_thread::get_id() ||
      state_->active != work.command_.front().serial || state_->failed) {
    return false;
  }
  const bool terminal =
      work.kind() == Kind::kQuit || work.kind() == Kind::kReplaceProcess;
  if (terminal ? !work.shutdown_returned_ : !work.host_returned_) {
    return false;
  }
  if (terminal) {
    state_->stopped = true;
  }
  state_->active = 0;
  state_->active_generation = state_->active_prepared = 0;
  state_->active_kind = Kind::kLaunch;
  return true;
}
inline bool EmulatorLifecycle::ApproveFinalUiClose() noexcept {
  std::lock_guard lock(state_->mutex);
  if (!state_->stopped || state_->active || state_->producers ||
      !state_->pending.empty() || state_->failed) {
    return false;
  }
  state_->ui_approved = true;
  return true;
}
}  // namespace xe
