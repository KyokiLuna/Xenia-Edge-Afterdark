/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include "xenia/kernel/native_profile_identity_tasks.h"
namespace xe::kernel {
class NativeProfileIdentityOwner;
namespace test { class NativeIdentityWakeOwnerTestAccess; }
namespace identity_detail {
// The concrete identity owner holds its actual retained XThread/domain/Task
// records in parallel. These methods own ONLY finite origin/disposition
// metadata under that owner's mutex, never guest calls, waits, filesystem IO,
// resource destruction or a caller-filled provider-readiness permission.
//
// Single-source law: after old F25E0/F2618 ingress closes and genuinely drains,
// only ONE already retained prepared identity may write the native pending
// flag. Native F25E0 tail reaches 7B68 only AFTER its real pending store. This
// prevents an older producer from racing a new generation's sidecar. Without
// that enforced gate, the later Reschedule entry alone is NOT linearization.
class NativeIdentityWakeOwner final {
 public:
  using Tasks=NativeProfileIdentityTasks;
  using Root=Tasks::Root;
  using Frame=Tasks::Frame;
  using Birth=Tasks::Birth;
  struct Wake { uint64_t serial=0; };
  struct Work { uint64_t serial=0; };
  struct Observation {
    uint64_t prepared_origin=0,wake_sequence=0;
    bool whole_worker_returned=false,refresh_child_returned=false;
  };
  // Factual only. Equal pending bits or a normal callback return do not grant
  // provider/host publication/pixel readiness. Actual native data/read receipts
  // and every other required provider still belong to the concrete owner.
  bool lost() const { return lost_; }
  // Advisory scalar metadata only; production accesses hold owner's mutex.
  uint32_t FirstFailureLine() const { return first_failure_line_; }
  uint32_t FirstFailureReason() const { return first_failure_reason_; }
 private:
  friend class xe::kernel::NativeProfileIdentityOwner;
  friend class xe::kernel::test::NativeIdentityWakeOwnerTestAccess;
  enum class Phase : uint8_t { kOpenOld,kClosedOld,kPrepared,kPublished,kRetired };
  static_assert(std::atomic<Phase>::is_always_lock_free);
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  static constexpr size_t kMaximumInvocations=64;
  struct WakeEntry {
    Wake key;Frame frame,reschedule;
    uint64_t source=0,sequence=0;
    bool joined=false,set_pending=false,reached_reschedule=false;
  };
  struct WorkEntry {
    Work key;Frame frame,refresh;
    uint64_t source=0,sequence=0;
    bool joined=false,refresh_entered=false,refresh_returned=false;
  };
  NativeIdentityWakeOwner(Root root,Birth actual_persistent_task,uint64_t old_read_source)
      :root_(root),persistent_(actual_persistent_task),old_read_source_(old_read_source) {
    lost_=!root.valid() || !persistent_.generation || !Address(persistent_.task) || !old_read_source_;
    if(lost_){first_failure_line_=__LINE__;first_failure_reason_=1;}
  }
  static bool Address(uint32_t address) { return address>=0x1000 && !(address&3); }
  bool Matches(const Frame& frame,uint32_t function) const {
    return frame.valid() && frame.root==root_ && frame.function==function;
  }
  bool SameCaller(const Frame& left,const Frame& right) const {
    return left.root==right.root && left.thread==right.thread && left.pcr==right.pcr &&
        left.sp==right.sp && left.lr==right.lr;
  }
  bool ActualPersistent(Birth actual) const {
    // The original bootstrap allocation generation is immutable. A later
    // equal Task address, current UID or presumed reschedule is not a bind.
    return actual.generation==persistent_.generation && actual.task==persistent_.task &&
        actual.origin==persistent_.origin;
  }
  void Lose(uint32_t line,uint32_t reason) {
    if(!first_failure_line_){first_failure_line_=line;first_failure_reason_=reason;}
    lost_=true;
  }
  uint64_t Next() {
    if (!next_ || next_==UINT64_MAX) { Lose(__LINE__,5);return 0; }
    return next_++;
  }
  WakeEntry* Find(Wake key) {
    for (auto& value:wakes_) { if (value && value->key.serial==key.serial) { return &*value; } }
    return nullptr;
  }
  WorkEntry* Find(Work key) {
    for (auto& value:works_) { if (value && value->key.serial==key.serial) { return &*value; } }
    return nullptr;
  }
  bool Empty() const {
    if (lost_) { return false; }
    for (const auto& value:wakes_) { if (value) { return false; } }
    for (const auto& value:works_) { if (value) { return false; } }
    return true;
  }
  void CloseOldIngress() {
    if (phase_==Phase::kOpenOld) { phase_=Phase::kClosedOld; }
  }
  bool ArmPrepared(uint64_t retained_prepared_origin,std::optional<uint32_t> actual_pending) {
    if (lost_ || phase_!=Phase::kClosedOld || !retained_prepared_origin ||
        !Empty() || !actual_pending || *actual_pending) { return false; }
    prepared_=retained_prepared_origin;phase_=Phase::kPrepared;return true;
  }
  bool MarkNativeCachePublished(uint64_t retained_prepared_origin) noexcept {
    // Genuine publisher seam: only preallocated atomics here. The owner first
    // performs the matching atomic host-view publication and phase selection.
    if (!retained_prepared_origin || prepared_.load(std::memory_order_acquire)!=retained_prepared_origin) { return false; }
    auto expected=Phase::kPrepared;
    return phase_.compare_exchange_strong(expected,Phase::kPublished,std::memory_order_acq_rel);
  }
  // Called before F25E0 consumes its r3 or performs ANY guest side effect.
  // A denial is an actual pre-body HRESULT; no replay or post-entry fallback.
  std::optional<Wake> EnterWake(Frame frame,uint64_t earlier_source,bool native_r3_nonzero) {
    if (lost_ || !Matches(frame,0x816F25E0) || !earlier_source) { return {}; }
    const bool joined=phase_==Phase::kPublished && earlier_source==prepared_;
    if ((phase_!=Phase::kOpenOld || earlier_source!=old_read_source_) && !joined) { return {}; }
    for (const auto& value:wakes_) {
      if (value && value->frame==frame) { Lose(__LINE__,3);return {}; }
    }
    const auto serial=Next();if (!serial) { return {}; }
    for (auto& value:wakes_) { if (!value) {
      value=WakeEntry{Wake{serial},frame,{},earlier_source,0,joined,native_r3_nonzero};
      return Wake{serial};
    } }
    // Local bounded admission may reject BEFORE guest entry. It never retires
    // an older retained invocation to make room or invents a native capacity.
    return {};
  }
  // F25E0 is a frameless tail to real XamTaskReschedule816B7B68. Retain its
  // origin through that real tail instead of pretending F25E0 returned. This
  // hook must match the earlier whole producer/frame and real Task lifetime.
  bool ReachNativeReschedule(Wake key,Frame actual,Birth task) {
    auto* value=Find(key);
    if (lost_ || !value || value->reached_reschedule || !Matches(actual,0x816B7B68) ||
        !SameCaller(value->frame,actual) || !ActualPersistent(task)) { Lose(__LINE__,6);return false; }
    if (value->joined && (phase_!=Phase::kPublished || value->source!=prepared_)) { Lose(__LINE__,6);return false; }
    if (!value->joined && phase_!=Phase::kOpenOld && phase_!=Phase::kClosedOld) { Lose(__LINE__,6);return false; }
    value->reschedule=actual;value->reached_reschedule=true;
    if (value->joined && value->set_pending) {
      if (!next_sequence_ || next_sequence_==UINT64_MAX) { Lose(__LINE__,6);return false; }
      value->sequence=next_sequence_++;latest_sequence_=value->sequence;
      // Only one prepared origin is allowed to write during this phase. An
      // ordinary/unjoined producer cannot replace this factual wake source.
    }
    return true;
  }
  bool ReturnNativeReschedule(Wake key,Frame actual) {
    auto* value=Find(key);
    if (lost_ || !value || !value->reached_reschedule || actual!=value->reschedule) {
      Lose(__LINE__,7);return false;
    }
    for (auto& entry:wakes_) { if (entry && entry->key.serial==key.serial) { entry.reset();return true; } }
    Lose(__LINE__,7);return false;
  }
  // Whole336B worker entry precedes its pending exchange, settings/native-user
  // reads and output cache writes. No incoming r3/profile pointer is consumed
  // by this verified body. The concrete bridge authenticates ALL native input
  // roots and borrows the immutable prepared phase before the body can run.
  std::optional<Work> EnterWorker(Frame frame,Birth task) {
    if (lost_ || !Matches(frame,0x816F2618) || !ActualPersistent(task)) { return {}; }
    const bool joined=phase_==Phase::kPublished && latest_sequence_;
    if (phase_!=Phase::kOpenOld && !joined) { return {}; }
    for (const auto& value:works_) {
      // One actual persistent Task cannot concurrently run two invocations.
      if (value) { Lose(__LINE__,4);return {}; }
    }
    const auto serial=Next();if (!serial) { return {}; }
    for (auto& value:works_) { if (!value) {
      value=WorkEntry{Work{serial},frame,{},joined?prepared_.load(std::memory_order_acquire):old_read_source_,
                     joined?latest_sequence_:0,joined};
      return Work{serial};
    } }
    return {};
  }
  uint64_t RetainedSource(Work key) {
    const auto* value=Find(key);
    return !lost_ && value ? value->source : 0;
  }
  bool EnterRefreshChild(Work key,Frame frame) {
    auto* value=Find(key);
    if (lost_ || !value || value->refresh_entered || !Matches(frame,0x816E3A98) ||
        frame.thread!=value->frame.thread || frame.pcr!=value->frame.pcr ||
        uint64_t(frame.sp)+0xB0!=value->frame.sp || frame.lr!=0x816F2668) { Lose(__LINE__,2);return false; }
    value->refresh=frame;value->refresh_entered=true;return true;
  }
  bool ReturnRefreshChild(Work key,Frame frame) {
    auto* value=Find(key);
    if (lost_ || !value || !value->refresh_entered || value->refresh_returned ||
        frame!=value->refresh) { Lose(__LINE__,8);return false; }
    value->refresh_returned=true;return true;
  }
  std::optional<Observation> ReturnWorker(Work key,Frame frame) {
    auto* value=Find(key);
    if (lost_ || !value || value->frame!=frame ||
        (value->refresh_entered && !value->refresh_returned)) { Lose(__LINE__,9);return {}; }
    const Observation observed{value->joined?value->source:0,value->sequence,true,
                               value->refresh_returned};
    if (value->joined) {
      last_complete_sequence_=std::max(last_complete_sequence_,value->sequence);
      if (value->refresh_returned) { last_refresh_sequence_=std::max(last_refresh_sequence_,value->sequence); }
    }
    for (auto& entry:works_) { if (entry && entry->key.serial==key.serial) {
      entry.reset();return observed;
    } }
    Lose(__LINE__,9);return {};
  }
  // Actual provider/settings/listener/descendant acknowledgments are separate
  // unforgeable owner records. This predicate only tells the owner whether all
  // wake/worker invocation holders and the actual native flag have drained.
  bool InvocationsDrained(std::optional<uint32_t> actual_pending) const {
    return Empty() && actual_pending && !*actual_pending;
  }
  bool FinishJoinedOrigin(uint64_t origin,std::optional<uint32_t> actual_pending) {
    // Called by the concrete owner only AFTER its real provider/read-output,
    // task/child/listener and renderer receipt joins. These metadata facts are
    // necessary but deliberately do not construct that completion permission.
    if (lost_ || phase_!=Phase::kPublished || origin!=prepared_ || !latest_sequence_ ||
        last_complete_sequence_<latest_sequence_ || last_refresh_sequence_<latest_sequence_ ||
        !InvocationsDrained(actual_pending)) { return false; }
    old_read_source_=origin;
    prepared_=0;latest_sequence_=0;last_complete_sequence_=0;last_refresh_sequence_=0;
    phase_=Phase::kOpenOld;return true;
  }
  bool CancelBeforeNative(uint64_t origin,std::optional<uint32_t> pending) {
    if(lost_ || !origin || !InvocationsDrained(pending))return false;
    if(phase_==Phase::kPrepared){if(prepared_!=origin)return false;prepared_=0;}
    else if(phase_!=Phase::kClosedOld && phase_!=Phase::kOpenOld)return false;
    phase_=Phase::kOpenOld;return true;
  }
  void RetireAtActualKernelStop() {
    // ONLY the genuine root/module/kernel stop grants this final retirement;
    // stop requests/timeouts/missing return never call this in production.
    phase_=Phase::kRetired;
    for (auto& value:wakes_) { value.reset(); }
    for (auto& value:works_) { value.reset(); }
  }
  Root root_;
  const Birth persistent_;
  std::atomic<Phase> phase_{Phase::kOpenOld};
  bool lost_=false;
  uint32_t first_failure_line_=0,first_failure_reason_=0;
  std::atomic<uint64_t> prepared_{0};
  uint64_t old_read_source_=0;
  uint64_t next_=1,next_sequence_=1,latest_sequence_=0;
  uint64_t last_complete_sequence_=0,last_refresh_sequence_=0;
  std::array<std::optional<WakeEntry>,kMaximumInvocations> wakes_{};
  std::array<std::optional<WorkEntry>,kMaximumInvocations> works_{};
};
}  // namespace identity_detail
}  // namespace xe::kernel
