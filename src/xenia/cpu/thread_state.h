/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_THREAD_STATE_H_
#define XENIA_CPU_THREAD_STATE_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe::kernel {
class NativeProfileIdentityOwner;
class NativeSystemUiService;
namespace xam { class ProfileRosterNativeReadScope; }
}
namespace xe {
namespace cpu {

class Processor;

// Per-guest-thread FTRACE bookkeeping. Owned here rather than thread_local so
// the frame stack follows the thread across dispatch CPUs under the guest
// scheduler.
struct FunctionTraceFrame {
  uint32_t address;
  uint64_t token;  // MicroProfileToken
  uint64_t tick;
  bool profiled;
};
struct FunctionTraceState {
  std::vector<FunctionTraceFrame> stack;
  uint32_t open_count = 0;
};

class ThreadState {
 public:
  ThreadState(Processor* processor, uint32_t thread_id, uint32_t stack_base = 0,
              uint32_t pcr_address = 0);
  ~ThreadState();

  Processor* processor() const { return processor_; }
  Memory* memory() const { return memory_; }
  void* backend_data() const { return backend_data_; }
  ppc::PPCContext* context() const { return context_; }
  uint32_t thread_id() const { return thread_id_; }

  static void Bind(ThreadState* thread_state);
  static ThreadState* Get();
  static uint32_t GetThreadID();

  FunctionTraceState& function_trace_state() { return function_trace_state_; }

 private:
  friend class xe::kernel::NativeProfileIdentityOwner;
  friend class xe::kernel::NativeSystemUiService;
  friend class xe::kernel::xam::ProfileRosterNativeReadScope;
  // Whole actual native work records own these prebound cells. Never host TLS,
  // current UID or a borrowed guest address; no owning ref drops at publication
  // or native listener unlock. Actual owner clears them before joined teardown.
  // Same actual original public invocation owns this scalar-only cell through
  // guest execution. Only its normal host return withdraws the exact pointer;
  // uncertain native execution retains the invocation, never a borrowed stack.
  std::atomic<void*> native_signin_creation_cell_{nullptr};
  // Exact SYSTEM Editor public invocation owns this paired loader capability.
  // Unclaimed native loader calls retain their original body; no ambient TLS.
  std::atomic<void*> native_editor_handoff_cell_{nullptr};
  // Scalar-only task diagnostic cells belong to the retained identity State;
  // actual work entry/return stacks them, stopped retirement detaches them.
  std::atomic<void*> native_identity_task_diagnostic_cell_{nullptr};
  // One retained native getter use prebinds its exact acquisition receipt.
  // Getter hooks touch only scalars/atomics, even inside native lock scopes.
  std::atomic<void*> native_identity_task_reference_cell_{nullptr};
  std::atomic<void*> native_identity_publication_cell_{nullptr};
  std::atomic<void*> native_identity_listener_cell_{nullptr};
  std::atomic<void*> native_profile_roster_read_{nullptr};
  Processor* processor_;
  Memory* memory_;
  void* backend_data_;

  uint32_t pcr_address_ = 0;
  uint32_t thread_id_ = 0;

  FunctionTraceState function_trace_state_;

  // NOTE: must be 64b aligned for SSE ops.
  ppc::PPCContext* context_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_THREAD_STATE_H_
