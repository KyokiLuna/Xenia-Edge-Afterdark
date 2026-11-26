/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_UTIL_IO_REQUEST_COMPLETION_H_
#define XENIA_KERNEL_UTIL_IO_REQUEST_COMPLETION_H_
#include <cstdint>
#include <atomic>
#include "xenia/kernel/util/host_call_outcome.h"
#include "xenia/xbox.h"

namespace xe::kernel::util {
enum class InlineIoFailurePolicy {
  kRejectWithoutCompletion,
  kCompleteAdmitted,
};

// Facts recorded at the actual backend boundary, before watch callbacks,
// position changes or XMP processing. A throw *inside* the backend has no
// normal-return count: zero in an ABI failure packet is not evidence of an
// unchanged file. Scatter retains the known prefix when a later chunk fails.
struct IoTransferReceipt {
  HostCallOutcome invocation = HostCallOutcome::kUnstarted;
  bool backend_entered = false;
  // Typed filesystem producer facts, independent of virtual method entry.
  // Known access/validation rejection has no persistent effect; entered but
  // unreturned/partial mutations remain uncertain. No guest result is altered.
  bool persistent_effect_known = false, persistent_effect_entered = false;
  bool backend_returned = false;
  bool count_known = false;
  uint32_t bytes = 0;
  uint32_t completed_prefix = 0;
  X_STATUS backend_status = X_STATUS_PENDING;
  void EnterBackend(uint32_t prefix = 0) {
    completed_prefix = prefix;
    backend_entered = true;
    backend_returned = false;
    count_known = false;
  }
  void Returned(X_STATUS status, uint32_t count) {
    backend_returned = true;
    count_known = true;
    backend_status = status;
    bytes = count;
  }
};
// One actual batch slot, shared by normal/drop/resource/rejection paths.
// Claim before invoking its owner so a later observer exception cannot cause
// the submitting caller to replay/decrement the same item a second time.
struct IoBatchArrival {
  std::atomic<bool> arrived{false};
  template <typename Complete>
  bool CompleteOnce(X_STATUS status, Complete&& complete) {
    if (arrived.exchange(true, std::memory_order_acq_rel)) { return false; }
    complete(status);
    return true;
  }
};
// The submit expression includes caller-side std::function conversion. Its
// producer guarantees that a nonreturned preparation has not admitted work.
// The rejection owner must be the same once-only slot as normal completion.
template <typename Submit, typename Reject>
void SubmitIoBatchItem(Submit&& submit, Reject&& reject) {
  X_STATUS status = X_STATUS_INSUFFICIENT_RESOURCES;
  const auto invoked = InvokeHostCall([&] { status = submit(); });
  if (invoked != HostCallOutcome::kReturned) {
    reject(X_STATUS_INSUFFICIENT_RESOURCES);
  } else if (XFAILED(status)) { reject(status); }
}

struct IoNotificationReceipt {
  HostCallOutcome apc = HostCallOutcome::kUnstarted;
  HostCallOutcome ports = HostCallOutcome::kUnstarted;
  HostCallOutcome file_event = HostCallOutcome::kUnstarted;
  HostCallOutcome external_event = HostCallOutcome::kUnstarted;
  uint32_t ports_attempted = 0;
  uint32_t ports_returned = 0;
  bool Failed() const {
    return apc == HostCallOutcome::kExecutionFailed ||
           ports == HostCallOutcome::kExecutionFailed ||
           file_event == HostCallOutcome::kExecutionFailed ||
           external_event == HostCallOutcome::kExecutionFailed;
  }
};
// Independent final channels retain their order and are each attempted once.
// A failed APC/port attempt is uncertain; never replay it to reach later events.
template <typename Apc, typename File, typename Event>
void CompleteIoNotifications(IoNotificationReceipt& receipt, bool has_apc,
                              Apc&& apc, File&& file, bool has_event, Event&& event) {
  if (has_apc) { receipt.apc = InvokeHostCall(apc); }
  const auto file_call = InvokeHostCall(file);
  if (file_call != HostCallOutcome::kReturned) {
    receipt.file_event = HostCallOutcome::kExecutionFailed;
  }
  if (has_event) { receipt.external_event = InvokeHostCall(event); }
}

// One admitted request owns this value through every phase. Attempted-but-
// unreturned publication/notification is uncertain, never safely replayable.
// No callback in this protocol is retried after an ordinary host exception.
struct IoRequestCompletion {
  IoTransferReceipt transfer;
  IoNotificationReceipt notifications;
  bool entered = false;
  HostCallOutcome transfer_call = HostCallOutcome::kUnstarted;
  HostCallOutcome pending_publication_call = HostCallOutcome::kUnstarted;
  X_STATUS pending_publication_status = X_STATUS_PENDING;
  HostCallOutcome publication_call = HostCallOutcome::kUnstarted;
  HostCallOutcome notification_call = HostCallOutcome::kUnstarted;
  X_STATUS operation_status = X_STATUS_PENDING;
  bool operation_count_known = false;
  uint32_t operation_bytes = 0;
  X_STATUS publication_status = X_STATUS_PENDING;
  X_STATUS final_status = X_STATUS_PENDING;
  bool completed = false;
  // Storage-checkpoint effect only; NEVER the guest request/IOSB result.
  // Known completed reads and genuine pre-backend rejection/cancellation
  // have no persistent-byte effect. Entered failed/partial mutations stay.
  X_STATUS CheckpointEffectStatus(bool may_mutate) const {
    if (!completed) {
      return X_STATUS_UNSUCCESSFUL;
    }
    if (!may_mutate || !transfer.backend_entered ||
        (transfer.persistent_effect_known &&
         !transfer.persistent_effect_entered)) {
      return X_STATUS_SUCCESS;
    }
    return transfer.backend_returned ? transfer.backend_status
                                     : X_STATUS_UNSUCCESSFUL;
  }
};

template <typename Transfer, typename Publish, typename Notify>
X_STATUS CompleteIoRequestOwned(IoRequestCompletion& state, bool posted,
                                bool cancelled, InlineIoFailurePolicy policy,
                                Transfer&& transfer, Publish&& publish,
                                Notify&& notify,
                                X_STATUS unstarted_failure = X_STATUS_SUCCESS) {
  if (state.entered) {
    return X_STATUS_INVALID_PARAMETER;
  }
  state.entered = true;
  uint32_t bytes = 0;
  X_STATUS status = cancelled ? X_STATUS_CANCELLED : unstarted_failure;
  if (!cancelled && XSUCCEEDED(unstarted_failure)) {
    state.transfer_call = InvokeHostCall([&] {
      status = transfer(&bytes, &state.transfer);
    });
    const bool body_failed = state.transfer_call != HostCallOutcome::kReturned ||
        state.transfer.invocation == HostCallOutcome::kExecutionFailed;
    state.operation_count_known = !body_failed || state.transfer.count_known;
    if (body_failed) {
      bytes = state.transfer.count_known ? state.transfer.bytes : 0;
    }
    if (state.transfer_call != HostCallOutcome::kReturned) {
      status = HostCallFailureStatus(state.transfer_call);
    }
  } else {
    // Genuine cancellation/resource rejection never entered its backend.
    state.operation_count_known = true;
  }
  state.operation_bytes = bytes;
  state.operation_status = status;
  if (!posted && XFAILED(status) &&
      policy == InlineIoFailurePolicy::kRejectWithoutCompletion) {
    state.final_status = status;
    state.completed = true;
    return status;
  }
  // Preserve the existing admitted control/flush ABI. The retained backend
  // receipt is separate from this guest-visible failure Information field.
  if (XFAILED(status) && policy == InlineIoFailurePolicy::kCompleteAdmitted) {
    bytes = 0;
  }
  state.publication_call = InvokeHostCall([&] {
    state.publication_status = publish(status, bytes);
  });
  if (state.publication_call != HostCallOutcome::kReturned) {
    status = HostCallFailureStatus(state.publication_call);
    bytes = 0;
  } else if (XFAILED(state.publication_status)) {
    status = state.publication_status;
    bytes = 0;
  }
  state.notification_call = InvokeHostCall([&] { notify(status, bytes); });
  // Publication may already have happened. Never overwrite or reissue it to
  // pretend a later signaling failure was a backend failure or cancellation.
  if (state.notification_call != HostCallOutcome::kReturned ||
      state.notifications.Failed()) {
    status = X_STATUS_UNSUCCESSFUL;
  }
  state.final_status = status;
  state.completed = true;
  return status;
}

// Existing consumers/tests use the same engine without a backend-specific
// receipt. They can only establish counts after their transfer returns.
template <typename Transfer, typename Publish, typename Notify>
X_STATUS CompleteIoRequest(bool posted, bool cancelled,
                           InlineIoFailurePolicy policy, Transfer transfer,
                           Publish publish, Notify notify) {
  IoRequestCompletion state;
  return CompleteIoRequestOwned(
      state, posted, cancelled, policy,
      [&](uint32_t* bytes, IoTransferReceipt*) { return transfer(bytes); },
      publish, notify);
}
}  // namespace xe::kernel::util
#endif
