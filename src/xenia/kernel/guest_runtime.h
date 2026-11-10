/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_RUNTIME_H_
#define XENIA_KERNEL_GUEST_RUNTIME_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace xe::kernel {

class XThread;

// One loaded module generation, not a module name or an "attached" flag.
// Construct only after its resources have actually loaded. KernelState owns
// the record and supplies a pin to the module; the module must not own its own
// pinned runtime. Shared instances retain themselves across executor callbacks.
class GuestRuntime : public std::enable_shared_from_this<GuestRuntime> {
 public:
  struct Identity {
    uint32_t hmodule = 0;
    uint64_t generation = 0;
    bool operator==(const Identity&) const = default;
  };

  enum class State {
    kResourcesLoaded,
    kInitializing,
    kReady,
    kFailed,
    kRetiring
  };
  enum class ReturnKind { kUnspecified, kVoid, kUInt32, kUInt64, kHResult };
  struct Function {
    std::string identity;
    uint32_t address = 0;
    std::vector<uint64_t> arguments;
    ReturnKind return_kind = ReturnKind::kUnspecified;
    // Caller-verified ABI/provenance, retained verbatim, not interpreted or
    // inferred by this class. The executor validates/uses the actual ABI.
    std::string abi_metadata;
    bool operator==(const Function&) const = default;
  };
  struct Phase {
    std::string identity;
    std::vector<Function> functions;
    bool operator==(const Phase&) const = default;
  };
  struct Plan {
    std::string identity;
    std::vector<Phase> phases;
    bool operator==(const Plan&) const = default;
  };
  struct Position {
    size_t phase = 0;
    size_t function = 0;
    bool operator==(const Position&) const = default;
  };
  struct ExecutionResult {
    bool succeeded = false;
    uint64_t return_value = 0;
    std::string detail;
  };
  using Executor = std::function<ExecutionResult(const Function&)>;
  struct Failure {
    Position position;
    uint64_t return_value = 0;
    std::string detail;
  };
  struct Snapshot {
    Identity identity;
    State state = State::kResourcesLoaded;
    std::shared_ptr<const Plan> plan;
    size_t completed_functions = 0;
    size_t total_functions = 0;
    std::optional<Position> current_function;
    std::optional<Failure> failure;
    bool initialization_active = false;
  };
  enum class InitStatus {
    kReady,
    kAlreadyReady,
    kBusy,
    kReentrant,
    kFailed,
    kRetiring,
    kInvalidPlan,
    kPlanMismatch,
  };
  enum class DrainStatus { kDrained, kSelfCall, kWouldBlock };

  GuestRuntime(Identity identity, std::shared_ptr<void> owner_pin);
  ~GuestRuntime();
  GuestRuntime(const GuestRuntime&) = delete;
  GuestRuntime& operator=(const GuestRuntime&) = delete;

  // The first valid plan becomes an immutable owned copy. Entries, including
  // duplicate function addresses, execute in their supplied phase/entry order
  // exactly once. An executor copy is retained too, outside the mutex, so a
  // callback may erase its original owning storage. Reentry/concurrent init
  // returns immediately; failed/partial execution poisons this generation.
  // Executor C++ exceptions also poison the generation, then propagate.
  InitStatus Initialize(Plan plan, const Executor& executor);
  Snapshot GetSnapshot() const;

  // Close admission before draining. Never wait on the active executor itself
  // or on a scheduler fiber; kSelfCall/kWouldBlock require a native system
  // thread to retry. allow_wait=false also permits explicit nonblocking
  // teardown preflight. Call without the kernel/module registry locks held.
  // The module pin is released outside this mutex after the executor drains.
  DrainStatus RetireAndDrain(bool allow_wait = true);

 private:
  struct InvocationKey {
    const XThread* fiber = nullptr;
    std::thread::id host;
    bool operator==(const InvocationKey&) const = default;
  };
  static InvocationKey CurrentInvocationKey();
  static bool ValidatePlan(const Plan& plan);
  InitStatus FinishInitialization(
      bool succeeded, std::optional<Failure> failure = std::nullopt);

  mutable std::mutex mutex_;
  std::condition_variable drained_;
  Snapshot snapshot_;
  InvocationKey invocation_;
  std::shared_ptr<void> owner_pin_;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_GUEST_RUNTIME_H_
