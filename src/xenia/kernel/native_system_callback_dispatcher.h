/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_CALLBACK_DISPATCHER_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_CALLBACK_DISPATCHER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace xe::kernel {
class KernelState;
class NativeImportDomain;

// Transport for verified kernel-driver callbacks, not a replacement XAM task
// scheduler. The callback must permit an ordinary SYSTEM context: this worker
// never manufactures a native task/TLS/current-app chain. Native task callbacks
// and UI requests belong to the real XAM scheduler/manager instead.
class NativeSystemCallbackDispatcher {
 public:
  using Completion = std::function<void(bool executed, uint32_t result)>;
  // Acquire the exact driver registration's operation immediately before PPC.
  // A null reservation rejects stale queued work. The reservation is retained
  // through execution and completion, outside all dispatcher locks.
  using BeforeEnter = std::function<std::shared_ptr<void>()>;

  static std::shared_ptr<NativeSystemCallbackDispatcher> Create(
      KernelState* kernel, std::shared_ptr<NativeImportDomain> domain);
  ~NativeSystemCallbackDispatcher();
  NativeSystemCallbackDispatcher(const NativeSystemCallbackDispatcher&) =
      delete;
  NativeSystemCallbackDispatcher& operator=(
      const NativeSystemCallbackDispatcher&) = delete;

  // At most eight DWORD register arguments. True transfers the callback and
  // promises one completion, including false for discarded/stale work. False
  // rejects the operation without completion; the caller handles rejection.
  // No PPC on the producer.
  bool Enqueue(uint32_t address, std::span<const uint32_t> arguments,
               Completion completed, BeforeEnter before_enter = {},
               uint64_t input_trace_id = 0);
  bool IsAccepting() const;
  bool IsCurrentWorker() const;
  void BeginStop();
  // Call while native providers/tasks/module memory are still alive, after
  // driver ingress closes. False for self-join; callers must defer destruction.
  bool DrainAndJoin();
  const std::shared_ptr<NativeImportDomain>& domain() const;

  // Kernel lifecycle boundary. Entries are weak and keyed by the actual Kernel;
  // no global service takes ownership of a Kernel or a native graph.
  static void BeginStopForKernel(KernelState* kernel);
  static bool DrainAndJoinForKernel(KernelState* kernel);

 private:
  struct State;
  explicit NativeSystemCallbackDispatcher(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_SYSTEM_CALLBACK_DISPATCHER_H_
