/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <optional>
#include <atomic>
#include <memory>
namespace xe::kernel {
class NativeProfileIdentityOwner;
namespace test {
class NativeProfileIdentityTaskTestAccess;
class NativeIdentityOrdinaryTransportTestAccess;
}
namespace identity_detail {
// Factual native Task provenance only. A Task address/return, completed work,
// empty ledger or later equal XUID never grants profile/provider/pixel readiness.
// The concrete owner authenticates actual Root/domain/code/XThread before these
// metadata operations and retains resources in parallel preallocated holders.
// No guest call, allocator, lock, callback or native-resource destructor occurs
// here. Only the real owner can supply an earlier prepared intent origin.
class NativeProfileIdentityTasks final {
 public:
  struct Root {
    uint64_t kernel=0,domain=0,generation=0;
    bool valid() const { return kernel && domain && generation; }
    bool operator==(const Root&) const=default;
  };
  struct Frame {
    Root root;
    uint32_t thread=0,pcr=0,sp=0,lr=0,function=0;
    bool valid() const {
      return root.valid() && thread && pcr && sp>=0x200 && !(sp&15) &&
          lr>=4 && !(lr&3) && function>=4 && !(function&3);
    }
    bool operator==(const Frame&) const=default;
  };
  // Semantic routing of the shared native CompleteIO body. This memento
  // grants no Task/read/profile/completion authority. Concrete ownership is
  // still authenticated by the actual owner before any claimed body enters.
  class CompletionRoute final {
   public:
    enum class Kind : uint8_t { kInvalid, kOrdinary, kIdentity };
    static CompletionRoute Capture(Frame frame, std::shared_ptr<const void> job,
        uint64_t serial, uint64_t source, uint32_t overlap, uint64_t actual) {
      CompletionRoute value; value.frame_ = frame;
      if (!frame.valid() || frame.function != 0x816A2588) return value;
      if (!job || !overlap || uint32_t(actual) != overlap) {
        value.kind_ = Kind::kOrdinary; return value;
      }
      // A malformed apparent owned overlap cannot downgrade to ordinary IO.
      if (!CanonicalOverlap(actual, overlap) || !serial || !source) return value;
      value.kind_ = Kind::kIdentity; value.job_ = std::move(job);
      value.serial_ = serial; value.source_ = source; value.overlap_ = overlap;
      return value;
    }
    Kind kind() const noexcept { return kind_; }
    bool FrameMatches(Frame actual) const noexcept {
      return kind_ != Kind::kInvalid && !returned_ && !failed_ && actual.valid() && actual == frame_;
    }
    bool StillOwned(const std::shared_ptr<const void>& job, uint64_t serial,
        uint64_t source, uint32_t overlap, uint64_t actual) const noexcept {
      return kind_ == Kind::kIdentity && !returned_ && !failed_ && job &&
          job.get() == job_.get() && !job.owner_before(job_) && !job_.owner_before(job) &&
          serial == serial_ && source == source_ && overlap == overlap_ && CanonicalOverlap(actual, overlap_);
    }
    bool ReturnOrdinary(Frame actual) noexcept {
      if (kind_ != Kind::kOrdinary || !FrameMatches(actual)) { failed_ = true; return false; }
      returned_ = true; return true;
    }
   private:
    // Preserve the owner's established PPC DWORD ABI: lis/addi may yield a
    // canonical sign extension; arbitrary high bits are never truncated.
    static bool CanonicalOverlap(uint64_t actual, uint32_t expected) noexcept {
      return uint32_t(actual) == expected &&
          (actual == uint64_t(expected) || actual == uint64_t(int64_t(int32_t(expected))));
    }
    Kind kind_ = Kind::kInvalid;
    Frame frame_{};
    std::shared_ptr<const void> job_;
    uint64_t serial_ = 0, source_ = 0;
    uint32_t overlap_ = 0;
    bool returned_ = false, failed_ = false;
  };
  struct Birth { uint64_t generation=0,origin=0; uint32_t task=0; };
  // An acquired native reference is NOT an allocation/execution generation.
  // This receipt observes one actual B8098/B7F30 acquisition and consumes only
  // that reference. It cannot publish a callback, source, read or Task node.
  // Independent native BA3C0 CurrentTask AddRef, not a transport Birth.
  // Only the concrete owner supplies authenticated code/thread/stack cuts.
  struct ContextReferenceReceipt {
    enum class Phase:uint8_t {kEmpty,kEntry,kGetter,kHeld,kCallback0,kAfter0,
      kCallback1,kAfter1,kClosing,kClosed,kReturned,kUncertain,kRetired};
    Phase phase=Phase::kEmpty;
    uint64_t serial=0;
    Frame wrapper{},callback{};
    uint32_t context=0,task=0,limit=0,base=0;
    bool pending=false,getter_entered=false,decrement_entered=false;
    void Fail() noexcept {phase=Phase::kUncertain;}
    bool finished() const noexcept {return phase==Phase::kReturned;}
    bool NewerActiveThan(const ContextReferenceReceipt* older) const noexcept {
      return !finished() && (!older || serial>older->serial);
    }
    bool Prepare(uint64_t id,Frame entry,uint32_t argument,uint32_t low,uint32_t high) noexcept {
      if(phase!=Phase::kEmpty || !id || !entry.valid() || entry.function!=0x816BA3C0 ||
         !argument || (argument&3) || low>=high || entry.sp<low+0x70ull || entry.sp>high){Fail();return false;}
      serial=id;wrapper=entry;context=argument;limit=low;base=high;phase=Phase::kEntry;return true;
    }
    bool Body(Frame f) const noexcept {
      return f.valid() && f.root==wrapper.root && f.thread==wrapper.thread &&
          f.pcr==wrapper.pcr && f.sp==wrapper.sp-0x70;
    }
    bool Getter(Frame f,bool returned,uint64_t value) noexcept {
      if(!Body(f) || f.function!=0x816B7F30 || f.lr!=0x816BA3D4 ||
         phase!=(returned?Phase::kGetter:Phase::kEntry)){Fail();return false;}
      if(!returned){phase=Phase::kGetter;return true;}
      if(!getter_entered || value<0x1000 || value>UINT32_MAX || (value&3)){Fail();return false;}
      task=uint32_t(value);phase=Phase::kHeld;return true;
    }
    bool AdmitGetter(Frame f) noexcept {
      if(!Body(f) || f.function!=0x816B7F30 || f.lr!=0x816BA3D4 || phase!=Phase::kGetter){Fail();return false;}
      getter_entered=true;return true;
    }
    bool AdmitDecrement(Frame f) noexcept {
      if(!Body(f) || f.function!=0x816B7490 || f.lr!=0x816BA430 || phase!=Phase::kClosing){Fail();return false;}
      decrement_entered=true;return true;
    }
    bool Callback(uint32_t site,Frame body,uint32_t actual_task,uint32_t actual_context,
        uint32_t target,uint64_t value) noexcept {
      if(!Body(body) || body.function!=0x816BA3C0 || actual_task!=task ||
         actual_context!=context){Fail();return false;}
      if(site==0 || site==2){
        if(phase!=(site==0?Phase::kHeld:Phase::kAfter0) || (site==2 && pending) ||
           target<4 || (target&3)){Fail();return false;}
        callback={wrapper.root,wrapper.thread,wrapper.pcr,body.sp,
            site==0?0x816BA3E8u:0x816BA424u,target};
        phase=site==0?Phase::kCallback0:Phase::kCallback1;return true;
      }
      if(site!=1 && site!=3){Fail();return false;}
      if(phase!=(site==1?Phase::kCallback0:Phase::kCallback1) || body.lr!=callback.lr){Fail();return false;}
      if(site==1)pending=uint32_t(value)==0x8000000Au;
      phase=site==1?Phase::kAfter0:Phase::kAfter1;return true;
    }
    bool Permits(Frame current,uint32_t body) const noexcept {
      return (phase==Phase::kCallback0 || phase==Phase::kCallback1) && task==body &&
          current.valid() && current.root==wrapper.root && current.thread==wrapper.thread &&
          current.pcr==wrapper.pcr && current.sp>=limit && current.sp<=callback.sp;
    }
    bool Decrement(Frame f,bool returned,uint64_t body_or_count) noexcept {
      const bool source=Body(f) && f.function==0x816B7490 && f.lr==0x816BA430;
      if(!returned){
        const bool ready=(phase==Phase::kAfter0 && pending) || phase==Phase::kAfter1;
        // Revoke before original decrement, including malformed entry.
        phase=Phase::kClosing;
        if(!source || !ready || body_or_count!=task){Fail();return false;}return true;
      }
      if(!source || phase!=Phase::kClosing || !decrement_entered || body_or_count>INT32_MAX){Fail();return false;}
      phase=Phase::kClosed;return true;
    }
    bool Return(Frame f) noexcept {
      if(f!=wrapper || phase!=Phase::kClosed){Fail();return false;}
      phase=Phase::kReturned;return true;
    }
    void Retire() noexcept {phase=Phase::kRetired;}
  };
  struct ReferenceReceipt {
    enum class Phase:uint32_t {kEmpty,kPrepared,kOuterEntered,kInnerEntered,
      kInnerReturned,kAcquireReturned,kHeld,kReleaseIssued,kReleaseEntered,
      kReleaseReturned,kConsumed,kCanceled,kUncertain,kRetired};
    static constexpr uint32_t kExecutePadding=176;
    static bool MayAcquire(bool stopping,bool accepting,bool retained_unload,uint32_t function) noexcept {
      return accepting && (!stopping || (retained_unload && function==0x816BD560));
    }
    std::atomic<Phase> phase{Phase::kEmpty};
    uint64_t serial=0;
    Frame callback{},acquire{},inner{},release{},release_callback{};
    uint32_t task=0,remaining=0;
    bool nonfinal=false;
    Phase state() const noexcept {return phase.load(std::memory_order_acquire);}
    bool finished() const noexcept {const auto p=state();return p==Phase::kConsumed || p==Phase::kCanceled;}
    bool Prepare(uint64_t id,Frame caller,uint32_t body) noexcept {
      if(state()!=Phase::kEmpty || !id || !caller.valid() || body<0x1000 || (body&3) ||
          caller.sp<kExecutePadding+0x60)return false;
      serial=id;callback=caller;task=body;
      acquire={caller.root,caller.thread,caller.pcr,caller.sp-kExecutePadding,0xBCBCBCBC,0x816B8098};
      inner={caller.root,caller.thread,caller.pcr,acquire.sp-0x60,0x816B80A8,0x816B7F30};
      phase.store(Phase::kPrepared,std::memory_order_release);return true;
    }
    void Fail() noexcept {phase.store(Phase::kUncertain,std::memory_order_release);}
    bool Step(Phase before,Phase after,Frame actual,Frame expected) noexcept {
      if(actual!=expected || !phase.compare_exchange_strong(before,after,std::memory_order_acq_rel)){
        Fail();return false;}return true;
    }
    bool EnterAcquire(Frame actual) noexcept {
      return Step(Phase::kPrepared,Phase::kOuterEntered,actual,acquire);
    }
    bool EnterInner(Frame actual) noexcept {
      return Step(Phase::kOuterEntered,Phase::kInnerEntered,actual,inner);
    }
    bool ReturnInner(Frame actual,uint64_t raw) noexcept {
      if(raw!=task){Fail();return false;}
      return Step(Phase::kInnerEntered,Phase::kInnerReturned,actual,inner);
    }
    bool ReturnAcquire(Frame actual,uint64_t raw) noexcept {
      if(raw!=uint64_t(task|1u)){Fail();return false;}
      return Step(Phase::kInnerReturned,Phase::kAcquireReturned,actual,acquire);
    }
    bool CompleteAcquire(bool executed,uint64_t raw,uint32_t current) noexcept {
      if(!executed || raw!=uint64_t(task|1u) || current!=task){Fail();return false;}
      auto before=Phase::kAcquireReturned;
      if(!phase.compare_exchange_strong(before,Phase::kHeld,std::memory_order_acq_rel)){
        Fail();return false;}return true;
    }
    void AbandonAcquire() noexcept {
      auto before=Phase::kPrepared;
      if(!phase.compare_exchange_strong(before,Phase::kCanceled,std::memory_order_acq_rel))Fail();
    }
    // A rescheduled callback has a fresh frame. The acquisition's serial and
    // held reference survive it; an old saved stack witness does not.
    bool BeginRelease(Frame current,Frame baseline,uint32_t body,bool must_be_nonfinal) noexcept {
      if(!current.valid() || current.root!=callback.root || body!=task || current.sp<kExecutePadding ||
          (must_be_nonfinal && (!baseline.valid() || baseline.root!=current.root ||
           baseline.thread!=current.thread || baseline.pcr!=current.pcr || baseline.sp<current.sp ||
           baseline.function!=callback.function))){Fail();return false;}
      if(state()!=Phase::kHeld)return false;
      release={current.root,current.thread,current.pcr,current.sp-kExecutePadding,0xBCBCBCBC,0x816B7490};
      nonfinal=must_be_nonfinal;release_callback=baseline;
      auto before=Phase::kHeld;
      if(!phase.compare_exchange_strong(before,Phase::kReleaseIssued,std::memory_order_acq_rel)){
        Fail();return false;}return true;
    }
    bool BeginContextRelease(Frame current,const ContextReferenceReceipt& native,uint32_t body) noexcept {
      if(state()!=Phase::kHeld || current.root!=callback.root || body!=task ||
         current.sp<kExecutePadding || !native.Permits(current,body))return false;
      release={current.root,current.thread,current.pcr,current.sp-kExecutePadding,0xBCBCBCBC,0x816B7490};
      nonfinal=true;release_callback=native.callback;
      auto before=Phase::kHeld;
      return phase.compare_exchange_strong(before,Phase::kReleaseIssued,std::memory_order_acq_rel);
    }
    bool EnterRelease(Frame actual) noexcept {
      return Step(Phase::kReleaseIssued,Phase::kReleaseEntered,actual,release);
    }
    bool ReturnRelease(Frame actual,uint64_t raw) noexcept {
      // Guard-abort HRESULTs and void/raw residue are not ref counts.
      if(raw>INT32_MAX || (nonfinal && !raw)){Fail();return false;}
      remaining=uint32_t(raw);
      return Step(Phase::kReleaseEntered,Phase::kReleaseReturned,actual,release);
    }
    bool CompleteRelease(bool executed,uint32_t value) noexcept {
      if(!executed || value!=remaining){Fail();return false;}
      auto before=Phase::kReleaseReturned;
      if(!phase.compare_exchange_strong(before,Phase::kConsumed,std::memory_order_acq_rel)){
        Fail();return false;}return true;
    }
    static bool NonfinalQueueBaseline(Frame actual,uint32_t limit,uint32_t base,
        uint32_t backchain,uint32_t saved_caller) noexcept {
      return actual.valid() && actual.lr==0x816B8B64 && limit<base &&
          actual.sp>=limit && uint64_t(actual.sp)+0x70<=base &&
          backchain==uint64_t(actual.sp)+0x70 &&
          (saved_caller==0x816B93FC || saved_caller==0x816B94FC || saved_caller==0x816BA0DC);
    }
  };
  // Same original scan/early ambiguous return. Counts at ambiguity are only
  // the visited prefix, not an invented complete history census.
  struct TransportLookup {
    enum class Kind:uint32_t {kAbsent,kHistoricalFreed,kFreeInProgress,kUniqueLive,kAmbiguous};
    Kind kind=Kind::kAbsent;
    uint32_t matching_rows=0,freed_rows=0,free_in_progress=0,live_rows=0;
    Birth first{},live{};
  };
  enum class CallbackClaim { kOutside,kCanonicalProfile,kMalformedProfile };
  static CallbackClaim ClassifyCallback(uint64_t raw) noexcept {
    const uint32_t at=uint32_t(raw);
    const bool profile=at==0x8199C838 || at==0x8199E148 || at==0x816DA428 ||
        at==0x816F4298 || at==0x816F4410 || at==0x81696558;
    if(!profile)return CallbackClaim::kOutside;
    return raw==uint64_t(at) || raw==uint64_t(int64_t(int32_t(at))) ?
        CallbackClaim::kCanonicalProfile:CallbackClaim::kMalformedProfile;
  }
  // All native Modify field selectors. Other bits have no effect in the
  // authenticated body; bit8 only requests requeue inside an options update.
  struct ModifyFields {
    bool callback=false,argument=false,options=false,requeue=false;
    bool options_only() const noexcept {return options && !callback && !argument;}
  };
  static ModifyFields DecodeModifyFields(uint32_t mask) noexcept {
    return {bool(mask&1),bool(mask&2),bool(mask&4),bool((mask&4) && (mask&8))};
  }
  // Timed options hold a scalar period, not an aligned guest address. This
  // source-independent lane changes only the interval of an existing timed
  // Task. New flags/queue contexts need their own original producer contract.
  struct TimedOptions {uint32_t flags=0,interval=0;};
  static bool PreservesTimedOptions(TimedOptions before,TimedOptions after) noexcept {
    return before.flags==after.flags && (before.flags&8) && !(before.flags&0x10000000u);
  }
  // Native B9AA8 only reads r5 when mask bit 0 requests callback storage
  // (9AC0 tests bit 0; 9AD0 stores r5 at Task+30). Unused GPRs are not inputs.
  static CallbackClaim ClassifyModifyCallback(uint32_t mask,uint64_t raw) noexcept {
    return mask&1 ? ClassifyCallback(raw):CallbackClaim::kOutside;
  }
  static constexpr size_t kCapacity=64;
  explicit NativeProfileIdentityTasks(Root root):root_(root),lost_(!root.valid()) {
    if(lost_){first_failure_line_=__LINE__;first_failure_reason_=1;}
  }
  bool lost() const { return lost_; }
  // Advisory scalar metadata only; production accesses hold owner's mutex.
  uint32_t FirstFailureLine() const { return first_failure_line_; }
  uint32_t FirstFailureReason() const { return first_failure_reason_; }
  bool IsProfileTransportSubject(uint64_t generation) const noexcept {
    for(const auto& node:nodes_)if(node && node->birth.generation==generation &&
        !node->freed && !node->free_entered && !node->transport_only && !node->foreign_avatar && !node->periodic_factory && !node->execution_only)
      return ClassifyCallback(node->callback)==CallbackClaim::kCanonicalProfile;
    return false;
  }

  bool IsForeignAvatarTransportSubject(uint64_t generation) const noexcept {
    for(const auto& node:nodes_)if(node && node->birth.generation==generation &&
        !node->freed && !node->free_entered)return node->foreign_avatar;
    return false;
  }
  bool IsExecutionOnlyOptionsSubject(uint64_t generation) const noexcept {
    for(const auto& node:nodes_)if(node && node->birth.generation==generation &&
        !node->freed && !node->free_entered)return node->birth.origin==1 &&
        !node->created_persistent && !node->resident_physical && !node->transport_only &&
        !node->continuation_ticket && !node->periodic_factory && !node->execution_only && ClassifyCallback(node->callback)==CallbackClaim::kOutside;
    return false;
  }
  // Process2 is actual native SYSTEM, supplied by the concrete XThread owner.
  // An observed foreign birth alone cannot bypass protected TITLE/unknown
  // callers or claim ordinary unowned work. This is metadata policy only.
  bool MayClaimExecutionOnlyOptions(uint32_t actual_process,uint32_t mask,
      bool protected_scope,uint64_t generation) const noexcept {
    return actual_process==2 && protected_scope && DecodeModifyFields(mask).options_only() &&
        IsExecutionOnlyOptionsSubject(generation);
  }
  std::optional<Birth> ObservedTransportSubject(uint32_t task,
      TransportLookup* observed=nullptr) const noexcept {
    if(observed)*observed={};
    std::optional<Birth> found;
    for(const auto& node:nodes_)if(node && !node->freed && !node->free_entered && node->birth.task==task){
      if(observed){if(!observed->matching_rows)observed->first=node->birth;
        ++observed->matching_rows;++observed->live_rows;observed->live=node->birth;
        if(node->delete_entered)++observed->free_in_progress;}
      if(found){if(observed)observed->kind=TransportLookup::Kind::kAmbiguous;return {};}
      found=node->birth;
    }else if(observed && node && node->birth.task==task){
      if(!observed->matching_rows)observed->first=node->birth;
      ++observed->matching_rows;
      if(node->freed)++observed->freed_rows;else ++observed->free_in_progress;
    }
    if(observed)observed->kind=observed->free_in_progress?TransportLookup::Kind::kFreeInProgress:
        found?TransportLookup::Kind::kUniqueLive:observed->freed_rows?
        TransportLookup::Kind::kHistoricalFreed:TransportLookup::Kind::kAbsent;
    return found;
  }
  bool OwnsDeletingFrame(const Frame& frame) const noexcept {
    for(const auto& call:deletions_)if(call && call->frame==frame)return true;
    return false;
  }
  // Actual retained nodes remain even after poison/stop admission. Caller may
  // reap resources only after their real completion or genuine Kernel stop.
  bool all_native_nodes_closed() const {
    if (lost_) { return false; }
    for (const auto& node:nodes_) { if (node && !node->freed) { return false; } }
    for (const auto& call:schedules_) { if (call) { return false; } }
    for (const auto& call:closes_) { if (call) { return false; } }
    for (const auto& call:deletions_) { if (call) { return false; } }
    for (const auto& call:modifications_) { if (call) { return false; } }
    for (const auto& call:detached_) { if (call) { return false; } }
    for (const auto& call:external_) { if (call) { return false; } }
    for (const auto& call:periodic_dispatches_) { if (call) { return false; } }
    for (const auto& call:execution_dispatches_) { if(call) { return false; } }
    for (const auto& call:execution_controls_) { if(call) { return false; } }
    return true;
  }
 private:
  friend class xe::kernel::NativeProfileIdentityOwner;
  friend class xe::kernel::test::NativeProfileIdentityTaskTestAccess;
  friend class xe::kernel::test::NativeIdentityOrdinaryTransportTestAccess;
  // Existing host observation fields shared with the concrete retained Work.
  // This metadata is not a Task/code/read capability. Only the owner mutates
  // it, under State; a skipped observation cannot issue any native fact.
  struct ExecutionObservation {
    Frame frame;
    uint64_t serial=0;
    bool execution_only=false,execution_uncertain=false,normal_return=false;
  };
  // Only explicitly finite host facade requests expire preparation. Original
  // guest logon and SYSTEM bootstrap have no host wall-clock timeout.
  struct PreparationDeadline {
    using Clock=std::chrono::steady_clock;
    static Clock::time_point Unbounded() noexcept { return Clock::time_point::max(); }
    static bool Expired(Clock::time_point deadline,Clock::time_point now) noexcept {
      return deadline!=Clock::time_point{} && now>=deadline;
    }
  };
  // A terminal queue has already had its original Task/read or pre-body
  // reservation ended by its producer. Capture only that queue on the real
  // host cut, even when unrelated native ownership is uncertain. Remaining
  // native close/free/reference/service owners use the separate healthy lane.
  enum class HostReapLane { kRetired, kTerminalOnly, kNativeOwners };
  template <typename Queue>
  static HostReapLane CaptureHostReap(bool retired, bool native_owners_healthy,
      Queue& terminal, Queue& captured) noexcept {
    if (retired) return HostReapLane::kRetired;
    terminal.swap(captured); // no allocation or resource destructor at this cut
    return native_owners_healthy ? HostReapLane::kNativeOwners : HostReapLane::kTerminalOnly;
  }
  // Exact prepared operation outcome, distinct from its scheduled Task's
  // return and native Wait/Close. Only the real owner supplies these facts.
  // A false host Work result is not evidence of lost PPC execution when its
  // sole original-ingress producer proved that native entry never happened.
  class PreparedIngress final {
   public:
    enum class Phase { kEmpty, kPrepared, kEntering, kNotEntered,
                       kNativeEntered, kNativeReturned };
    bool Bind(uint64_t serial, uint64_t source) noexcept {
      if (phase_ != Phase::kEmpty || !serial || !source) return false;
      serial_ = serial; source_ = source; phase_ = Phase::kPrepared; return true;
    }
    bool BeginEntry(uint64_t serial, uint64_t source) noexcept {
      if (!Matches(serial, source) || phase_ != Phase::kPrepared) return false;
      phase_ = Phase::kEntering; return true;
    }
    bool RetryEntry(uint64_t serial, uint64_t source) noexcept {
      if (!Matches(serial, source) || phase_ != Phase::kEntering) return false;
      phase_ = Phase::kPrepared; return true;
    }
    bool RefuseBeforeEntry(uint64_t serial, uint64_t source) noexcept {
      if (!Matches(serial, source) || body_returned_ ||
          (phase_ != Phase::kPrepared && phase_ != Phase::kEntering)) return false;
      phase_ = Phase::kNotEntered; return true;
    }
    bool NativeEntered(uint64_t serial, uint64_t source) noexcept {
      if (!Matches(serial, source) || phase_ != Phase::kEntering) return false;
      phase_ = Phase::kNativeEntered; return true;
    }
    bool NativeReturned(uint64_t serial, uint64_t source) noexcept {
      if (!Matches(serial, source) || phase_ != Phase::kNativeEntered) return false;
      phase_ = Phase::kNativeReturned; return true;
    }
    bool KnownNotEntered(uint64_t serial, uint64_t source) const noexcept {
      return Matches(serial, source) && phase_ == Phase::kNotEntered;
    }
    bool CanReturnBody(uint64_t serial, uint64_t source, bool succeeded) const noexcept {
      return Matches(serial, source) && !body_returned_ && !transport_rejected_ &&
          ((phase_ == Phase::kNotEntered && !succeeded) ||
           (phase_ == Phase::kNativeReturned && succeeded));
    }
    bool ReturnBody(uint64_t serial, uint64_t source, bool succeeded,
        bool task_returned, bool read_left) noexcept {
      if (!CanReturnBody(serial, source, succeeded) || !task_returned || !read_left)
        return false;
      body_returned_ = true; return true;
    }
    bool JoinTransport(uint64_t serial, uint64_t source, bool schedule_returned,
        uint32_t completed_task) noexcept {
      if (!Matches(serial, source) || !body_returned_ || transport_joined_ ||
          !schedule_returned || completed_task < 0x1000 || (completed_task & 3) != 1)
        return false;
      transport_joined_ = true; return true;
    }
    bool RefuseUnenteredTransport(uint64_t serial,uint64_t source,
        bool actual_schedule_rejection,bool schedule_returned,bool descendants_closed) noexcept {
      if(!Matches(serial,source) || phase_!=Phase::kPrepared || body_returned_ ||
          transport_joined_ || !actual_schedule_rejection || !schedule_returned ||
          !descendants_closed)return false;
      // The callback never ran. This is neither Task/read return nor a
      // fabricated Wait/Close; native descendant closure is proved separately.
      phase_=Phase::kNotEntered;transport_rejected_=true;transport_joined_=true;return true;
    }
    bool RefuseUnscheduledTransport(uint64_t serial,uint64_t source,
        bool invocation_cleaned,bool no_schedule_reservation,bool descendants_closed) noexcept {
      if(!Matches(serial,source) || phase_!=Phase::kPrepared || body_returned_ ||
          transport_joined_ || !invocation_cleaned || !no_schedule_reservation ||
          !descendants_closed)return false;
      phase_=Phase::kNotEntered;transport_rejected_=true;transport_joined_=true;return true;
    }
    bool NotEnteredAndJoined(uint64_t serial, uint64_t source) const noexcept {
      return KnownNotEntered(serial, source) && transport_joined_ &&
          (body_returned_ || transport_rejected_);
    }
   private:
    bool Matches(uint64_t serial, uint64_t source) const noexcept {
      return serial && source && serial == serial_ && source == source_;
    }
    uint64_t serial_ = 0, source_ = 0;
    Phase phase_ = Phase::kEmpty;
    bool body_returned_ = false, transport_joined_ = false, transport_rejected_ = false;
  };
  // A strong exact-Work memento for deciding whether this instruction can
  // have a host metadata effect. It never replaces the original code proof.
  class ExecutionInstructionParticipation final {
   public:
    static std::optional<ExecutionInstructionParticipation> Capture(
        const std::shared_ptr<ExecutionObservation>& current,uint32_t actual_pcr) noexcept {
      if(!Eligible(current,actual_pcr))return {};
      ExecutionInstructionParticipation value;
      value.owner_=current;value.serial_=current->serial;value.frame_=current->frame;
      return value;
    }
    bool Matches(const std::shared_ptr<ExecutionObservation>& current,
        uint32_t actual_pcr) const noexcept {
      return current.get()==owner_.get() && Eligible(current,actual_pcr) &&
          current->serial==serial_ && current->frame==frame_;
    }
   private:
    static bool Eligible(const std::shared_ptr<ExecutionObservation>& current,
        uint32_t actual_pcr) noexcept {
      // Exactly the original instruction observer's no-effect return set.
      // Body operands/SP/LR remain for the full proof and ledger to reject.
      return current && current->execution_only && !current->normal_return &&
          !current->execution_uncertain && current->frame.pcr==actual_pcr;
    }
    std::shared_ptr<const ExecutionObservation> owner_;
    uint64_t serial_=0;Frame frame_;
  };
  static bool DispatchCodeAuthenticationRequired(Birth birth,const void* actual_read_scope) noexcept {
    // A generation-zero dispatch cannot issue BeginExecutionDispatch. Its
    // actual ambient read still needs the original proof for restoration.
    return birth.generation || actual_read_scope;
  }
  // Scalar receipt paired with an existing one-shot caller reference. Only
  // the real owner supplies observed generation and native ledger outcomes.
  struct CloseReceipt {
    enum class Phase { kEmpty,kIssued,kEntered,kReturned,kConsumed,kFailed };
    Phase phase=Phase::kEmpty;
    Root root;Birth birth;uint32_t thread=0,pcr=0,value=0;Frame frame;
    bool Issue(Root r,Birth b,uint32_t t,uint32_t p){
      if(phase!=Phase::kEmpty || !r.valid() || !b.generation || !b.origin ||
          !Address(b.task) || !t || !p){phase=Phase::kFailed;return false;}
      root=r;birth=b;thread=t;pcr=p;phase=Phase::kIssued;return true;
    }
    bool Enter(Frame f,Birth b){
      if(phase!=Phase::kIssued || !f.valid() || f.root!=root || f.function!=0x816B7490 ||
          f.thread!=thread || f.pcr!=pcr || b.generation!=birth.generation ||
          b.origin!=birth.origin || b.task!=birth.task){phase=Phase::kFailed;return false;}
      frame=f;phase=Phase::kEntered;return true;
    }
    bool Returned(Frame f,uint32_t actual_value,bool actual_ledger_return){
      if(phase!=Phase::kEntered || f!=frame || !actual_ledger_return){phase=Phase::kFailed;return false;}
      value=actual_value;phase=Phase::kReturned;return true;
    }
    bool Complete(bool actual_executed,uint32_t actual_value){
      if(phase!=Phase::kReturned || !actual_executed || actual_value!=value){phase=Phase::kFailed;return false;}
      phase=Phase::kConsumed;return true;
    }
    void Fail(){phase=Phase::kFailed;}
  };
  // Exact producer frames for ALL three direct native B9A18 callers. This
  // classifies ABI ancestry only; a callsite does not supply profile origin,
  // a callback/read ticket, an allocation result, or permission to spawn.
  enum class AllocationProducer { kUnknown,kSchedule,kPersistentCreate,kInline };
  static AllocationProducer ClassifyAllocationProducer(Frame producer,Frame allocation,
      uint32_t expected_parameters,uint32_t parameters,uint32_t output) noexcept {
    if(!producer.valid() || !allocation.valid() || producer.root!=allocation.root ||
        producer.thread!=allocation.thread || producer.pcr!=allocation.pcr ||
        allocation.function!=0x816B9A18 || parameters!=expected_parameters ||
        allocation.sp>UINT32_MAX-0x50 || output!=allocation.sp+0x50)return AllocationProducer::kUnknown;
    uint32_t bytes=0,lr=0;AllocationProducer kind=AllocationProducer::kUnknown;
    switch(producer.function){
      case 0x816BA440:bytes=0x90;lr=0x816BA46C;kind=AllocationProducer::kSchedule;break;
      case 0x816BA668:bytes=0x70;lr=0x816BA688;kind=AllocationProducer::kPersistentCreate;break;
      case 0x816B9D68:bytes=0xA0;lr=0x816B9E08;kind=AllocationProducer::kInline;break;
      default:return AllocationProducer::kUnknown;
    }
    return producer.sp>=bytes && allocation.sp==producer.sp-bytes && allocation.lr==lr ?
        kind:AllocationProducer::kUnknown;
  }
  // Passive, exact same-XThread common Start/copy/inline history. The concrete
  // owner retains actual domain/code/thread holders in parallel and accepts
  // only a SYSTEM Start inside earlier protected work. These scalars NEVER
  // replace that ancestor, native TLS, native refs, or callback authority.
  struct CommonStartProducer {
    Frame start,copy,inline_call,allocation;
    std::array<uint32_t,6> args{}; // app,message,overlap,input,bytes,options
    Birth parent{};
    uint32_t copy_output=0,packet=0;
    bool start_returned=false,copy_entered=false,copy_returned=false,inline_entered=false;
    bool inline_returned=false,allocation_entered=false,allocation_returned=false;
    bool SameThread(Frame f) const noexcept {
      return start.valid() && !start_returned && f.valid() && f.root==start.root && f.thread==start.thread && f.pcr==start.pcr;
    }
    bool EnterStart(Frame f,std::array<uint32_t,6> input) noexcept {
      if(start.valid() || !f.valid() || f.function!=0x816A2CF0 || f.sp<=0xD0)return false;
      start=f;args=input;return true;
    }
    bool EnterCopy(Frame f,uint32_t input,uint32_t bytes,uint32_t output) noexcept {
      if(copy_entered || !SameThread(f) || f.function!=0x816A2A48 ||
          f.lr!=0x816A2D30 || f.sp!=start.sp-0xD0 || f.sp>UINT32_MAX-0x50 ||
          input!=args[3] || bytes!=args[4] || output!=f.sp+0x50)return false;
      copy=f;copy_output=output;copy_entered=true;return true;
    }
    bool ReturnCopy(Frame f,uint32_t hresult,std::optional<uint32_t> actual_output) noexcept {
      if(!copy_entered || copy_returned || f!=copy)return false;
      if(int32_t(hresult)>=0){
        if(!actual_output || *actual_output<0x1000 || (*actual_output&3) ||
            uint64_t(*actual_output)+0x1C+args[4]>UINT32_MAX)return false;
        packet=*actual_output;
      }
      copy_returned=true;return true;
    }
    bool MatchesInline(Frame f,uint32_t callback,uint32_t argument,uint32_t options) const noexcept {
      return !inline_entered && copy_returned && packet && !args[2] &&
          SameThread(f) && f.function==0x816B9D68 && f.lr==0x816A2EB8 &&
          f.sp==start.sp-0xD0 && callback==0x816A2C80 && argument==packet && options==args[5];
    }
    bool EnterInline(Frame f,uint32_t callback,uint32_t argument,uint32_t options,
        std::array<uint32_t,3> actual_packet,Birth actual_parent) noexcept {
      if(!MatchesInline(f,callback,argument,options) || actual_packet[0]!=args[0] ||
          actual_packet[1]!=args[1] || actual_packet[2]!=start.sp-0x70 ||
          !actual_parent.generation || !actual_parent.origin || !Address(actual_parent.task))return false;
      inline_call=f;parent=actual_parent;inline_entered=true;return true;
    }
    bool MatchesAllocation(Frame f,uint32_t parameters,uint32_t output) const noexcept {
      return inline_entered && !inline_returned && !allocation_entered &&
          ClassifyAllocationProducer(inline_call,f,args[5],parameters,output)==AllocationProducer::kInline;
    }
    bool EnterAllocation(Frame f,uint32_t parameters,uint32_t output) noexcept {
      if(!MatchesAllocation(f,parameters,output))return false;
      allocation=f;allocation_entered=true;return true;
    }
    bool ReturnAllocation(Frame f) noexcept {
      if(!allocation_entered || allocation_returned || f!=allocation)return false;
      allocation_returned=true;return true;
    }
    bool ReturnInline(Frame f,Birth actual_parent) noexcept {
      if(!inline_entered || inline_returned || f!=inline_call ||
          (allocation_entered && !allocation_returned) ||
          actual_parent.generation!=parent.generation || actual_parent.origin!=parent.origin ||
          actual_parent.task!=parent.task)return false;
      inline_returned=true;return true;
    }
    bool ReturnStart(Frame f) noexcept {
      if(!start.valid() || start_returned || f!=start || (copy_entered && !copy_returned) ||
          (inline_entered && !inline_returned))return false;
      start_returned=true;return true;
    }
  };
  // Typed class-factory receipt. The concrete owner authenticates its real
  // module-resident native object, original code and exact class/epoch before
  // issuing this scalar metadata. It grants no read/event/profile origin.
  struct PeriodicFactory {
    uint64_t serial=0;Root root;uint32_t object=0;Birth birth;
    Frame enable{},schedule{},stop{};
    bool enabling=false,scheduled=false,schedule_returned=false,enabled=false;
    bool stopping=false,cancel_returned=false,wait_returned=false,close_returned=false,stopped=false;
    bool failed=false;
    bool BeginEnable(uint64_t id,Frame frame,uint32_t actual_object,uint32_t handle) noexcept {
      if(failed || enabling || stopping || !frame.valid() || frame.function!=0x8190A358 ||
          actual_object<0x1000 || (actual_object&3) || actual_object>UINT32_MAX-0x208 ||
          (serial && (serial!=id || root!=frame.root || object!=actual_object)) ||
          (!serial && (!id || handle)) ||
          (serial && handle!=(enabled && !stopped ? (birth.task|1u):0u)))return false;
      serial=id;root=frame.root;object=actual_object;enable=frame;enabling=true;
      scheduled=schedule_returned=false;stopped=false;return true;
    }
    bool MatchesSchedule(Frame frame,uint32_t callback,uint32_t argument,uint32_t options,uint32_t output) const noexcept {
      return !failed && enabling && !scheduled && frame.valid() && frame.root==root &&
          frame.function==0x816BA440 && frame.thread==enable.thread && frame.pcr==enable.pcr &&
          enable.sp>=0x70 && frame.sp==enable.sp-0x70 && frame.lr==0x8190A3B0 &&
          callback==0x81909CF8 && argument==object && options==frame.sp+0x50 && output==object+0xC;
    }
    bool EnterSchedule(Frame frame) noexcept {if(scheduled)return false;schedule=frame;scheduled=true;return true;}
    bool ObserveBirth(Birth actual) noexcept {
      if(!scheduled || actual.origin!=1 || !actual.generation || actual.task<0x1000 || (actual.task&3))return false;
      birth=actual;return true;
    }
    bool ReturnSchedule(Frame frame,uint32_t raw,bool ledger_returned) noexcept {
      if(!scheduled || schedule_returned || frame!=schedule || int32_t(raw)<0 || !ledger_returned || !birth.generation)return false;
      schedule_returned=true;return true;
    }
    bool ReturnEnable(Frame frame,uint32_t handle,uint32_t actual_enabled) noexcept {
      if(failed || !enabling || frame!=enable || actual_enabled!=1 || !birth.generation ||
          handle!=(birth.task|1u) || (scheduled && !schedule_returned))return false;
      enabling=false;enabled=true;stopped=false;return true;
    }
    bool BeginStop(Frame frame,uint32_t actual_object,uint32_t handle) noexcept {
      if(failed || enabling || stopping || !enabled || stopped || !frame.valid() ||
          frame.root!=root || frame.function!=0x819068D0 || object!=actual_object || handle!=(birth.task|1u))return false;
      stop=frame;stopping=true;cancel_returned=wait_returned=close_returned=false;return true;
    }
    bool MatchesStopChild(Frame frame,uint32_t function,uint32_t task) const noexcept {
      const uint32_t lr=function==0x816B9F08?0x8190690Cu:function==0x816BA058?0x81906914u:
          function==0x816B7490?0x8190691Cu:0u;
      return !failed && stopping && lr && frame.valid() && frame.root==root && frame.function==function &&
          frame.thread==stop.thread && frame.pcr==stop.pcr && stop.sp>=0x70 && frame.sp==stop.sp-0x70 &&
          frame.lr==lr && task==birth.task &&
          (function==0x816B9F08 ? !cancel_returned : function==0x816BA058 ? cancel_returned && !wait_returned : wait_returned && !close_returned);
    }
    bool ReturnStopChild(Frame frame,Birth observed,bool actual_ledger_return) noexcept {
      if(!actual_ledger_return || observed.generation!=birth.generation || observed.origin!=birth.origin ||
          observed.task!=birth.task || !MatchesStopChild(frame,frame.function,observed.task))return false;
      if(frame.function==0x816B9F08)cancel_returned=true;
      else if(frame.function==0x816BA058)wait_returned=true;
      else if(frame.function==0x816B7490)close_returned=true;
      else return false;
      return true;
    }
    bool ReturnStop(Frame frame,uint32_t handle,uint32_t actual_enabled,uint32_t busy) noexcept {
      if(failed || !stopping || frame!=stop || handle || actual_enabled || busy ||
          !cancel_returned || !wait_returned || !close_returned)return false;
      stopping=false;enabled=false;stopped=true;return true;
    }
  };
  static uint32_t ProfileProducerFrameBytes(uint32_t function) noexcept {
    return function==0x8199E3E0?0x190u:function==0x8199E148?0xD0u:0u;
  }
  // Scalar prepublication provenance only, never an allocation lease.
  struct ProfileOutput {
    enum class Kind:uint8_t {kStack,kAsyncPacket};
    Kind kind=Kind::kStack;uint32_t packet=0,overlap=0;
    bool operator==(const ProfileOutput&) const=default;
  };
  static std::optional<ProfileOutput> CaptureProfileOutput(Frame owner,Frame schedule,
      uint32_t output,uint32_t overlap,uint32_t packet,std::array<uint32_t,3> linkage) noexcept {
    const auto bytes=ProfileProducerFrameBytes(owner.function);
    if(!owner.valid() || !schedule.valid() || owner.root!=schedule.root ||
       owner.thread!=schedule.thread || owner.pcr!=schedule.pcr || !bytes || owner.sp<bytes ||
       schedule.function!=0x816BA440 || schedule.sp!=owner.sp-bytes ||
       schedule.lr!=(owner.function==0x8199E3E0?0x8199F01Cu:0x8199E248u))return {};
    if(!overlap){
      if(packet || linkage!=std::array<uint32_t,3>{} ||
         output!=schedule.sp+(owner.function==0x8199E3E0?0x5Cu:0x50u))return {};
      return ProfileOutput{};
    }
    if(owner.function!=0x8199E3E0 || !Address(overlap) || overlap>UINT32_MAX-0x1B ||
       !Address(packet) || packet>UINT32_MAX-0x1B || output!=packet+0x18 ||
       linkage!=std::array<uint32_t,3>{0xFEu,overlap,packet})return {};
    return ProfileOutput{ProfileOutput::Kind::kAsyncPacket,packet,overlap};
  }
  // Original224's error tail completes its own caller on the original frame,
  // without a scheduled worker Birth. This proves only that actual ABI edge.
  static bool MatchesInitializerErrorCompletion(Frame owner,Frame completion,
      uint32_t owner_overlap,uint32_t overlap,uint32_t error,uint32_t extended,uint32_t length) noexcept {
    return owner.valid() && completion.valid() && owner.function==0x8199E3E0 &&
        completion.function==0x816A2588 && completion.lr==0x8199EEBC &&
        owner.root==completion.root && owner.thread==completion.thread && owner.pcr==completion.pcr &&
        owner.sp>=0x190 && completion.sp==owner.sp-0x190 && Address(owner_overlap) &&
        overlap==owner_overlap && int32_t(extended)<0 && error==0x65B && !length;
  }
  // A producer receipt preserves only the actual creator/transport/output
  // metadata. It owns no Work, read ticket, stack pointer or native reference.
  struct ProfileProducer {
    uint64_t owner_serial=0;Frame owner,schedule;
    Birth birth{};uint32_t callback=0,argument=0,output=0;ProfileOutput output_proof{};
    std::array<uint32_t,2> options{};bool has_options=false;
    uint32_t native_flags=0,native_data=0;bool native_bound=false;
    bool published=false,schedule_returned=false,schedule_succeeded=false,owner_close_entered=false;
    bool release_started=false; // Real deleting-destructor entry, not completion.
    bool MatchesOwner(uint64_t serial,Frame actual) const noexcept {
      return owner_serial && serial==owner_serial && owner==actual;
    }
  };
  static bool SameBirth(Birth a,Birth b) noexcept {
    return a.generation && a.generation==b.generation && a.origin==b.origin && a.task==b.task;
  }
  // This is execution transport only. Callback entry continues to need its
  // independent published Birth; no scope below issues a profile/read source.
  struct ProfileControl {
    enum class Kind:uint8_t {kWait,kCancel,kDispatch};
    Kind kind=Kind::kWait;Frame frame;Birth target{},parent{};
    bool entered=false,returned=false,body_admitted=false,failed=false;
    bool Enter(const ProfileProducer& producer,uint64_t serial,Frame owner,
        Frame actual,Birth actual_target,Birth actual_parent,uint32_t tagged) noexcept {
      if(entered || failed || producer.output_proof.kind!=ProfileOutput::Kind::kStack ||
         !producer.MatchesOwner(serial,owner) || !producer.published ||
         !producer.schedule_returned || !producer.schedule_succeeded || producer.owner_close_entered || !producer.native_bound ||
         !SameBirth(producer.birth,actual_target) || !actual_parent.generation || !actual_parent.origin ||
         !Address(actual_parent.task) || SameBirth(actual_target,actual_parent) ||
         !actual.valid() || actual.root!=owner.root || actual.thread!=owner.thread || actual.pcr!=owner.pcr ||
         actual.sp!=producer.schedule.sp || tagged!=(actual_target.task|1u) ||
         (actual.function!=0x816BA058 && actual.function!=0x816B9F08))return false;
      kind=actual.function==0x816BA058?Kind::kWait:Kind::kCancel;
      frame=actual;target=actual_target;parent=actual_parent;entered=true;return true;
    }
    bool EnterDispatch(const ProfileControl& wait,Frame actual,Birth actual_target) noexcept {
      if(entered || failed || !wait.entered || wait.returned || wait.failed || !wait.body_admitted || wait.kind!=Kind::kWait ||
         !actual.valid() || actual.function!=0x816B8B08 || actual.lr!=0x816BA0DC ||
         actual.root!=wait.frame.root || actual.thread!=wait.frame.thread || actual.pcr!=wait.frame.pcr ||
         wait.frame.sp<0x80 || actual.sp!=wait.frame.sp-0x80 || !SameBirth(wait.target,actual_target))return false;
      kind=Kind::kDispatch;frame=actual;target=actual_target;parent=wait.parent;entered=true;return true;
    }
    bool EnterRecursiveCancel(const ProfileControl& cancel,Frame actual,Birth child) noexcept {
      if(entered || failed || !cancel.entered || cancel.returned || cancel.failed || !cancel.body_admitted || cancel.kind!=Kind::kCancel ||
         !actual.valid() || actual.function!=0x816B9F08 || actual.lr!=0x816B9FE0 ||
         actual.root!=cancel.frame.root || actual.thread!=cancel.frame.thread || actual.pcr!=cancel.frame.pcr ||
         cancel.frame.sp<0x80 || actual.sp!=cancel.frame.sp-0x80 || !child.generation || !child.origin || !Address(child.task))return false;
      kind=Kind::kCancel;frame=actual;target=child;parent=cancel.parent;entered=true;return true;
    }
    bool Return(Frame actual,Birth actual_target,Birth actual_parent) noexcept {
      if(!entered || failed || returned || !body_admitted || frame!=actual || !SameBirth(target,actual_target) ||
         (kind!=Kind::kDispatch && !SameBirth(parent,actual_parent))){failed=true;return false;}
      returned=true;return true; // No HRESULT/Task/IO completion from void residue.
    }
  };
  struct Schedule {
    uint64_t serial=0,origin=0;
    Frame frame,allocation;
    uint32_t callback=0,argument=0,parameters=0,output=0,allocation_output=0;
    uint64_t birth=0;
    bool allocation_entered=false,allocation_returned=false,publish_entered=false;
    bool created_persistent=false,resident_physical=false,transport_only=false,continuation_ticket=false,execution_only=false;
    uint64_t periodic_factory=0;
    std::array<uint32_t,2> periodic_options{};
    uint32_t periodic_process=0;
    std::optional<ProfileProducer> producer;
    std::optional<Birth> execution_birth;
  };
  struct PeriodicAllocation {
    uint64_t factory=0;Birth birth;Frame schedule,allocation;
    std::array<uint32_t,2> options{};
    uint32_t process=0,flags=0,period=0;
  };
  // Context/lifetime only. Issued after an original periodic body is admitted,
  // or from its exact already-bound Task allocation/producer parent. This is
  // never a Task reference, CloseReceipt, callback/read source or completion.
  // The complete owning Work keeps it until that same body's own return.
  struct PeriodicFrameContext {
    Frame frame;uint64_t factory=0;uint32_t object=0;
    std::optional<PeriodicAllocation> allocation;
    bool Matches(Frame actual,uint64_t expected_factory) const noexcept {
      return factory && factory==expected_factory && object && frame.valid() &&
          actual==frame;
    }
  };
  struct Node {
    Birth birth;
    uint32_t callback=0,argument=0;
    Frame work;
    bool published=false,started=false,returned=false,freed=false;
    bool schedule_rejected=false; // genuine negative Schedule, not a callback return
    bool execution_only=false,execution_publication=false;
    uint32_t execution_flags=0;uint64_t execution_active=0;
    bool delete_entered=false,delete_returned=false,free_entered=false,free_returned=false;
    bool created_persistent=false,resident_physical=false,persistent_callback_bound=false,persistent_active=false,transport_only=false,continuation_ticket=false,foreign_avatar=false;
    Frame persistent_work;
    uint32_t persistent_output=0;
    uint64_t periodic_factory=0;bool periodic_stopped=false;
    std::optional<PeriodicAllocation> periodic_allocation;
    std::optional<ProfileProducer> producer;
  };
  struct DetachedAllocation { uint64_t serial=0,origin=0;Frame frame;uint32_t output=0;bool foreign_avatar=false,execution_only=false;uint32_t callback=0,argument=0; };
  struct ExternalWork { Frame frame;Birth birth; };
  struct PeriodicDispatch {Frame frame;Birth birth;bool callback_returned=false;uint64_t factory=0;PeriodicAllocation allocation;};
  struct Close { Frame frame; uint64_t birth=0;bool execution_only=false; };
  struct Deletion { Frame frame,free; uint64_t birth=0;bool execution_only=false; };
  struct Modification {
    Frame frame;uint64_t birth=0;
    enum class Kind {kPersistentBinding,kTimedOptions};
    Kind kind=Kind::kPersistentBinding;
    TimedOptions before{},after{}; // observed pre-body words, not applied-status
  };
  // Optional scalar witness from the ORIGINAL first blocking scan/return.
  // Recorded callback/frame fields are ledger metadata, never fresh native
  // pointer reads. Each caller owns a separate cut; no cross-cut snapshot.
  struct SourceBlocker {
    uint32_t available=0,kind=0,task=0,callback=0,argument=0,output=0,state=0;
    uint64_t source=0,generation=0,serial=0;Frame frame;bool closed=false;
  };
  bool SourceDescendantsClosed(uint64_t source,SourceBlocker* observation=nullptr) const {
    if(observation){*observation={};observation->available=1;observation->source=source;}
    if(lost_){if(observation)observation->kind=1;return false;}
    if(!source){if(observation)observation->kind=2;return false;}
    for(const auto& node:nodes_)if(node && node->birth.origin==source &&
        (!node->freed || node->persistent_active ||
         (!node->created_persistent && !node->resident_physical && !node->periodic_factory && !node->execution_only &&
          !node->returned && !(node->schedule_rejected && !node->started)))){
      if(observation){
        observation->kind=3;observation->available|=2;observation->task=node->birth.task;
        observation->generation=node->birth.generation;observation->callback=node->callback;
        observation->argument=node->argument;
        observation->state=uint32_t(node->published)|(uint32_t(node->started)<<1)|(uint32_t(node->returned)<<2)|
            (uint32_t(node->freed)<<3)|(uint32_t(node->delete_entered)<<4)|(uint32_t(node->free_entered)<<5)|
            (uint32_t(node->created_persistent)<<6)|(uint32_t(node->resident_physical)<<7)|
            (uint32_t(node->persistent_active)<<8)|(uint32_t(node->transport_only)<<9)|(uint32_t(node->foreign_avatar)<<10)|
            (uint32_t(node->schedule_rejected)<<11);
        observation->frame=node->persistent_active?node->persistent_work:node->work;
        if(observation->frame.valid())observation->available|=4;
      }
      return false;
    }
    for(const auto& call:schedules_)if(call && call->origin==source){
      if(observation){
        observation->kind=4;observation->available|=4;observation->generation=call->birth;
        observation->serial=call->serial;observation->frame=call->frame;
        observation->callback=call->callback;observation->argument=call->argument;observation->output=call->output;
        observation->state=uint32_t(call->allocation_entered)|(uint32_t(call->allocation_returned)<<1)|
            (uint32_t(call->publish_entered)<<2)|(uint32_t(call->created_persistent)<<3)|
            (uint32_t(call->resident_physical)<<4)|(uint32_t(call->transport_only)<<5);
      }
      return false;
    }
    for(const auto& call:detached_)if(call && call->origin==source){
      if(observation){
        observation->kind=5;observation->available|=4;observation->serial=call->serial;
        observation->frame=call->frame;observation->output=call->output;observation->state=uint32_t(call->foreign_avatar);
      }
      return false;
    }
    if(observation)observation->closed=true;
    return true;
  }
  bool Match(const Frame& frame) const { return frame.valid() && frame.root==root_; }
  static bool Address(uint32_t at) { return at>=0x1000 && !(at&3) && at<=UINT32_MAX-0x48; }
  void Lose(uint32_t line,uint32_t reason) {
    if(neutral_depth_){if(neutral_failures_!=UINT64_MAX)++neutral_failures_;return;}
    if(!first_failure_line_){first_failure_line_=line;first_failure_reason_=reason;}
    lost_=true;
  }
  uint64_t Next() { if (!next_ || next_==UINT64_MAX) { Lose(__LINE__,17); return 0; } return next_++; }
  Schedule* FindSchedule(uint64_t serial) {
    for (auto& value:schedules_) { if (value && value->serial==serial) { return &*value; } }
    return nullptr;
  }
  const Node* FindBirthConst(uint64_t serial) const noexcept {
    for(const auto& value:nodes_)if(value && value->birth.generation==serial)return &*value;
    return nullptr;
  }
  Node* FindBirth(uint64_t serial) {
    for (auto& value:nodes_) { if (value && value->birth.generation==serial) { return &*value; } }
    return nullptr;
  }
  Node* FindLiveTask(uint32_t task) {
    Node* found=nullptr;
    for (auto& value:nodes_) { if (value && !value->freed && !value->free_entered && value->birth.task==task) {
      if (found) { Lose(__LINE__,16); return nullptr; } found=&*value;
    } }
    return found;
  }
  // BA440 saves callback=r3,argument=r4,parameters=r5,output=r6. The caller's
  // earlier owned original224/worker/publisher establishes origin; never Current
  // profile/epoch or an unowned persistent provider invocation.
  // Native B9A18 explicitly accepts params==0 and keeps the allocator defaults;
  // original224 uses that genuine path. A nonzero params value is still a
  // checked DWORD-aligned guest pointer, never an inferred Task/output.
  // Other actual SYSTEM-root consumers use the SAME native allocator. Record
  // their real output generation at B9A18 before any callback can execute;
  // this grants execution identity only, never a prepared profile/data origin.
  uint64_t BeginDetachedAllocation(Frame frame,uint64_t earlier_origin,uint32_t output,bool foreign_avatar=false,bool execution_only=false,uint32_t callback=0,uint32_t argument=0) {
    if (lost_ || !Match(frame) || frame.function!=0x816B9A18 || !earlier_origin ||
        ((foreign_avatar || execution_only) && earlier_origin!=1) || (foreign_avatar && execution_only) || !Address(output)) { Lose(__LINE__,4);return 0; }
    for (const auto& row:detached_) if (row && row->frame==frame) { Lose(__LINE__,4);return 0; }
    const auto serial=Next();if (!serial) { return 0; }
    for (auto& row:detached_) if (!row) {
      row=DetachedAllocation{serial,earlier_origin,frame,output,foreign_avatar,execution_only,callback,argument};return serial;
    }
    Lose(__LINE__,4);return 0;
  }
  std::optional<Birth> ReturnDetachedAllocation(uint64_t serial,Frame frame,
      uint32_t actual_hresult,std::optional<uint32_t> actual_output) {
    for (auto& row:detached_) if (row && row->serial==serial) {
      if (lost_ || row->frame!=frame) { Lose(__LINE__,21);return {}; }
      if (int32_t(actual_hresult)<0) { row.reset();return {}; }
      if (!actual_output || !Address(*actual_output) || FindLiveTask(*actual_output)) {
        // A new allocation cannot silently complete/reuse an older unfinished
        // generation. The concurrent-close/free handoff needs an explicit
        // earlier free witness; retain uncertainty until its real resolution.
        Lose(__LINE__,21);return {};
      }
      const auto generation=Next();if (!generation) { return {}; }
      for (auto& node:nodes_) if (!node || node->freed) {
        const Birth result{generation,row->origin,*actual_output};
        node=Node{result,row->callback,row->argument};node->foreign_avatar=row->foreign_avatar;node->execution_only=row->execution_only;row.reset();return result;
      }
      Lose(__LINE__,21);return {};
    }
    Lose(__LINE__,21);return {};
  }
  // A whole provider operation borrows an ALREADY observed allocation. The
  // currentTask pointer alone never creates a generation or a data-origin
  // upgrade, and this is not an assumed callback-entry/return observation.
  std::optional<Birth> BeginExternalWork(Frame frame,uint32_t actual_current_task) {
    auto* node=FindLiveTask(actual_current_task);
    if (lost_ || !Match(frame) || !node || node->periodic_factory) { Lose(__LINE__,5);return {}; }
    for (const auto& row:external_) if (row && row->frame==frame) { Lose(__LINE__,5);return {}; }
    for (auto& row:external_) if (!row) {
      row=ExternalWork{frame,node->birth};return node->birth;
    }
    Lose(__LINE__,5);return {};
  }
  bool ReturnExternalWork(Frame frame,Birth birth) {
    for (auto& row:external_) if (row && row->frame==frame) {
      const auto* node=FindBirth(birth.generation);
      if (lost_ || !node || node->freed || row->birth.task!=birth.task ||
          row->birth.generation!=birth.generation || row->birth.origin!=birth.origin) {
        Lose(__LINE__,22);return false;
      }
      row.reset();return true;
    }
    Lose(__LINE__,22);return false;
  }
  // A generic native Task owns execution identity ONLY. Its opaque DWORD
  // argument is not a pointer/read scope and its lifetime never pins a roster.
  // The concrete owner separately retains actual producer/Modify code leases.
  // Native queues, references, TLS, callback selection and results stay native.
  bool IsExecutionSubject(uint64_t generation) const noexcept {
    const auto* node=FindBirthConst(generation);
    return node && node->execution_only && node->birth.origin==1 && !node->freed && !node->free_entered;
  }
  bool ExecutionClosed() const noexcept {
    for(const auto& node:nodes_)if(node && node->execution_only && !node->freed)return false;
    for(const auto& call:schedules_)if(call && call->execution_only)return false;
    for(const auto& call:execution_controls_)if(call)return false;
    for(const auto& call:execution_dispatches_)if(call)return false;
    for(const auto& call:closes_)if(call && call->execution_only)return false;
    for(const auto& call:deletions_)if(call && call->execution_only)return false;
    return true;
  }
  uint64_t BeginExecutionSchedule(Frame frame,uint32_t callback,uint32_t argument,
      uint32_t parameters,uint32_t output) {
    if(lost_ || !Match(frame) || frame.function!=0x816BA440 || !Address(callback) ||
       (parameters && !Address(parameters)) || !Address(output))return 0;
    for(const auto& call:schedules_)if(call && call->frame==frame)return 0;
    if(!next_ || next_==UINT64_MAX)return 0;
    for(auto& call:schedules_)if(!call){
      const auto serial=next_++;call=Schedule{serial,1,frame,{},callback,argument,parameters,output};
      call->execution_only=true;return serial;
    }
    return 0;
  }
  uint64_t BeginExecutionCreate(Frame frame,uint32_t output) {
    if(lost_ || !Match(frame) || frame.function!=0x816BA668 || !Address(output))return 0;
    for(const auto& call:schedules_)if(call && call->frame==frame)return 0;
    if(!next_ || next_==UINT64_MAX)return 0;
    const auto serial=next_++;
    for(auto& call:schedules_)if(!call){
      call=Schedule{};call->serial=serial;call->origin=1;call->frame=frame;
      call->output=output;call->created_persistent=true;call->execution_only=true;return serial;
    }
    return 0;
  }
  // Original successful allocator return is the sole generic birth issuer.
  // Local observer uncertainty never poisons an unrelated typed profile use.
  std::optional<Birth> ReturnExecutionAllocation(uint64_t serial,Frame frame,
      uint32_t status,std::optional<uint32_t> output) {
    auto* call=FindSchedule(serial);
    if(lost_ || !call || !call->execution_only || !call->allocation_entered ||
       call->allocation_returned || call->allocation!=frame)return {};
    call->allocation_returned=true;
    if(int32_t(status)<0)return {};
    if(!output || !Address(*output) || ObservedTransportSubject(*output) || !next_ || next_==UINT64_MAX)return {};
    for(auto& node:nodes_)if(!node || node->freed){
      const Birth birth{next_++,1,*output};node=Node{birth,call->callback,call->argument};
      node->execution_only=true;node->created_persistent=call->created_persistent;
      call->birth=birth.generation;call->execution_birth=birth;return birth;
    }
    return {};
  }
  bool EnterExecutionAllocation(uint64_t serial,Frame frame,uint32_t parameters,uint32_t output) {
    auto* call=FindSchedule(serial);
    if(lost_ || !call || !call->execution_only || call->allocation_entered ||
       ClassifyAllocationProducer(call->frame,frame,call->parameters,parameters,output)!=
          (call->created_persistent?AllocationProducer::kPersistentCreate:AllocationProducer::kSchedule))return false;
    call->allocation=frame;call->allocation_output=output;call->allocation_entered=true;return true;
  }
  bool BeforeExecutionPublication(uint64_t serial,Frame frame,uint32_t task,
      uint32_t callback,uint32_t argument,std::optional<uint32_t> tagged) {
    auto* call=FindSchedule(serial);auto* node=call?FindBirth(call->birth):nullptr;
    if(lost_ || !call || !node || !call->execution_only || call->created_persistent ||
       !call->allocation_returned || call->publish_entered || !Match(frame) ||
       frame.function!=0x816B9BB8 || frame.lr!=0x816BA4D8 ||
       frame.thread!=call->frame.thread || frame.pcr!=call->frame.pcr ||
       call->frame.sp<0x90 || frame.sp!=call->frame.sp-0x90 || node->birth.task!=task ||
       callback!=call->callback || argument!=call->argument || !tagged || *tagged!=(task|1u))return false;
    call->publish_entered=true;node->published=true;return true;
  }
  bool ReturnExecutionProducer(uint64_t serial,Frame frame,uint32_t status,
      std::optional<uint32_t> tagged={}) {
    for(auto& call:schedules_)if(call && call->serial==serial){
      if(lost_ || !call->execution_only || call->frame!=frame || !call->allocation_returned)return false;
      if(int32_t(status)>=0){
        if(call->created_persistent){
          if(!call->execution_birth || !tagged || *tagged!=(call->execution_birth->task|1u))return false;
        }else if(!call->publish_entered)return false;
      }
      call.reset();return true;
    }
    return false;
  }
  // Actual B9C14 is AFTER original callback/argument stores and BEFORE every
  // initial mode's list/thread publication, including timed and serial paths.
  // Entry intent is not a claim that any native field was written or queued.
  bool ObserveExecutionPublication(Frame publication,Birth birth,Frame body,
      uint32_t task,uint32_t callback,uint32_t argument,uint32_t flags) {
    auto* node=FindBirth(birth.generation);
    if(lost_ || !node || !SameBirth(node->birth,birth) || !node->execution_only ||
       !node->published || node->freed || node->free_entered || node->execution_publication ||
       publication.function!=0x816B9BB8 || body.function!=publication.function ||
       body.root!=publication.root || body.thread!=publication.thread || body.pcr!=publication.pcr ||
       publication.sp<0x80 || body.sp!=publication.sp-0x80 ||
       !Match(body) || (body.lr!=0x816B9BC0 && body.lr!=0x816B9BF0) ||
       task!=birth.task || callback!=node->callback || argument!=node->argument)return false;
    node->execution_publication=true;node->execution_flags=flags;return true;
  }
  bool ObserveInlineExecutionPublication(Frame producer,Birth birth,Frame body,
      uint32_t task,uint32_t callback,uint32_t argument) {
    auto* node=FindBirth(birth.generation);
    if(lost_ || !node || !SameBirth(node->birth,birth) || !node->execution_only ||
       node->published || node->freed || node->free_entered || node->execution_publication ||
       producer.function!=0x816B9D68 || body.function!=producer.function ||
       body.root!=producer.root || body.thread!=producer.thread || body.pcr!=producer.pcr ||
       producer.sp<0xA0 || body.sp!=producer.sp-0xA0 || !Match(body) ||
       body.lr!=0x816B9E08 || task!=birth.task || callback!=node->callback || argument!=node->argument)return false;
    node->published=true;node->execution_publication=true;return true;
  }
  // Existing fixed observer-frame scratch; no Task identity/birth registry.
  // Exhaustion is deny-only and sticky until this actual owner retires. An
  // unrecorded younger dispatcher must never expose an older capsule.
  class ExecutionUnavailableFrames final {
   public:
    static constexpr size_t kCapacity=256;
    void Record(Frame frame) noexcept {
      for(const auto& value:frames_)if(value && *value==frame)return;
      for(auto& value:frames_)if(!value){value=frame;return;}
      overflow_=true;
    }
    bool Return(Frame frame) noexcept {
      bool found=false;
      for(auto& value:frames_)if(value && *value==frame){value.reset();found=true;}
      return found; // Freed capacity cannot prove the lost frame returned.
    }
    bool Shadows(Root root,uint32_t thread,uint32_t pcr,uint32_t actual_sp,
        uint32_t candidate_sp) const noexcept {
      if(overflow_)return true;
      for(const auto& frame:frames_)if(frame && frame->root==root && frame->thread==thread &&
          frame->pcr==pcr && (frame->function==0x816B8B08 || frame->function==0x816B7DE8) &&
          frame->sp<=candidate_sp && actual_sp<=frame->sp)return true;
      return false;
    }
   private:
    std::array<std::optional<Frame>,kCapacity> frames_{};bool overflow_=false;
  };
  // Temporarily isolates one ALREADY entered ambient profile read scope
  // during a genuine nested source-neutral Task invocation. The owner binds
  // this only from its exact active Work/read object and keeps that Work
  // strongly alive; it issues no ticket, read capability or native Task ref.
  // Resource holders retire with returned Work outside State/GCRC locks.
  class ExecutionReadCell final {
   public:
    enum class Phase {kEmpty,kSuspended,kTransferred,kForwarded,kRestored,kUncertain};
    ExecutionReadCell()=default;
    ExecutionReadCell(const ExecutionReadCell&)=delete;
    ExecutionReadCell& operator=(const ExecutionReadCell&)=delete;
    Phase phase() const noexcept {return phase_;}
    bool pending() const noexcept {return phase_==Phase::kSuspended || phase_==Phase::kTransferred || phase_==Phase::kUncertain;}
    const void* scope() const noexcept {return scope_;}
    const void* owner() const noexcept {return owner_.get();}
    const Frame& origin() const noexcept {return origin_;}
    const Frame& restoration() const noexcept {return restoration_;}
    bool Suspend(Frame origin,Frame invocation,std::shared_ptr<void> owner,
        const std::shared_ptr<void>& actual_owner,void* scope,std::atomic<void*>& cell) noexcept {
      if(phase_!=Phase::kEmpty || !origin.valid() || !invocation.valid() ||
         origin.root!=invocation.root || origin.thread!=invocation.thread ||
         origin.pcr!=invocation.pcr || origin.sp<=invocation.sp || !scope ||
         !owner || !actual_owner || owner.get()!=actual_owner.get() ||
         owner.owner_before(actual_owner) || actual_owner.owner_before(owner) ||
         (invocation.function!=0x816B8B08 && invocation.function!=0x816B7DE8))return false;
      void* expected=scope;
      if(!cell.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel))return false;
      // shared_ptr move is noexcept. No ref gap/final release follows the CAS.
      owner_=std::move(owner);scope_=scope;origin_=origin;restoration_=invocation;
      phase_=Phase::kSuspended;return true;
    }
    // A positively owned, same-thread enclosing scope may also surround an
    // exact request-owned CEE08 service frame. This only isolates its TLS cell;
    // it supplies no read to the service and does not change native currentTask.
    bool SuspendService(Frame origin,Frame invocation,std::shared_ptr<void> owner,
        const std::shared_ptr<void>& actual_owner,void* scope,std::atomic<void*>& cell) noexcept {
      if(phase_!=Phase::kEmpty || !origin.valid() || !invocation.valid() ||
         invocation.function!=0x818CEE08 || origin.root!=invocation.root ||
         origin.thread!=invocation.thread || origin.pcr!=invocation.pcr ||
         origin.sp<=invocation.sp || !scope || !owner || !actual_owner ||
         owner.get()!=actual_owner.get() || owner.owner_before(actual_owner) ||
         actual_owner.owner_before(owner))return false;
      void* expected=scope;
      if(!cell.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel))return false;
      owner_=std::move(owner);scope_=scope;origin_=origin;restoration_=invocation;
      phase_=Phase::kSuspended;return true;
    }
    // Original BA058's inline dispatch returns BEFORE it restores parent
    // currentTask. Transfer to its exact admitted original own-return instead
    // of exposing the old read scope at callback/dispatcher return.
    bool TransferInlineWait(Frame dispatcher,Frame wait,ExecutionReadCell& into) noexcept {
      if(phase_!=Phase::kSuspended || restoration_!=dispatcher ||
         dispatcher.function!=0x816B8B08 || wait.function!=0x816BA058 ||
         !wait.valid() || wait.root!=dispatcher.root || wait.thread!=dispatcher.thread ||
         wait.pcr!=dispatcher.pcr || wait.sp<0x80 || dispatcher.sp!=wait.sp-0x80 ||
         dispatcher.lr!=0x816BA0DC || origin_.sp<=wait.sp || into.phase_!=Phase::kEmpty)return false;
      into.owner_=std::move(owner_);into.scope_=scope_;into.origin_=origin_;
      into.restoration_=wait;into.phase_=Phase::kTransferred;
      scope_=nullptr;phase_=Phase::kForwarded;return true;
    }
    // The real originating Work has already retained its entered scope. This
    // transfers ONLY the restoration obligation to that Work's genuine own
    // return/actual-stop lane; no cell or native completion changes here.
    bool DelegateToOrigin(const void* actual_owner,const void* still_owned_scope) noexcept {
      if(!pending() || owner_.get()!=actual_owner || scope_!=still_owned_scope)return false;
      scope_=nullptr;phase_=Phase::kForwarded;return true;
    }
    bool Restore(Frame actual,const void* still_owned_scope,std::atomic<void*>& cell) noexcept {
      if((phase_!=Phase::kSuspended && phase_!=Phase::kTransferred) ||
         actual!=restoration_ || still_owned_scope!=scope_)return false;
      void* expected=nullptr;
      if(!cell.compare_exchange_strong(expected,scope_,std::memory_order_acq_rel)){
        phase_=Phase::kUncertain;return false;
      }
      scope_=nullptr;phase_=Phase::kRestored;return true;
    }
    // Only the enclosing private actual-CPU-stop retirement may call this.
    // Reverse actual entry order first detaches any deeper genuine scope.
    bool RestoreAtActualStop(const void* still_owned_scope,std::atomic<void*>& cell) noexcept {
      if(!pending())return phase_==Phase::kEmpty || phase_==Phase::kForwarded || phase_==Phase::kRestored;
      if(!scope_ || still_owned_scope!=scope_)return false;
      void* expected=nullptr;
      if(!cell.compare_exchange_strong(expected,scope_,std::memory_order_acq_rel))return false;
      scope_=nullptr;phase_=Phase::kRestored;return true;
    }
   private:
    Phase phase_=Phase::kEmpty;Frame origin_,restoration_;
    std::shared_ptr<void> owner_;void* scope_=nullptr;
  };
  // Shared by BOTH genuine scope-owning exits: ordinary native own-return
  // and the authenticated render iteration end. No PPC/data access is allowed
  // between successful restoration and that actual scope's immediate Leave.
  static bool SuppressReadCellForScopeLeave(bool& fallback,void* still_owned_scope,
      std::atomic<void*>& cell) noexcept {
    if(!still_owned_scope)return false;
    void* expected=still_owned_scope;
    if(!cell.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel))return false;
    fallback=true;return true;
  }
  static bool RestoreReadCellForScopeLeave(bool& fallback,void* still_owned_scope,
      std::atomic<void*>& cell) noexcept {
    if(!fallback)return true;
    if(!still_owned_scope)return false;
    void* expected=nullptr;
    if(!cell.compare_exchange_strong(expected,still_owned_scope,std::memory_order_acq_rel))return false;
    fallback=false;return true;
  }
  struct ExecutionDispatch {
    uint64_t serial=0;Frame frame;Birth birth;
    bool selected=false,callback_returned=false;
    uint32_t callback=0,argument=0,result=0;
  };
  struct CurrentExecution {
    Root root;Birth birth;uint64_t invocation=0;
    Frame dispatcher;uint32_t body_sp=0,callback=0,argument=0,callback_lr=0;
  };
  std::optional<CurrentExecution> CaptureCurrentExecution(uint64_t serial,
      uint32_t actual_thread,uint32_t actual_pcr,uint32_t actual_sp,uint32_t actual_task) const noexcept {
    if(!actual_sp || (actual_sp&15))return {};
    for(const auto& call:execution_dispatches_)if(call && call->serial==serial){
      const uint32_t depth=call->frame.function==0x816B8B08?0x70u:0x80u;
      if(!call->selected || call->callback_returned || !call->birth.generation ||
         call->frame.thread!=actual_thread || call->frame.pcr!=actual_pcr ||
         call->birth.task!=actual_task || call->frame.sp<depth || actual_sp>call->frame.sp-depth)return {};
      return CurrentExecution{root_,call->birth,call->serial,call->frame,
          call->frame.sp-depth,call->callback,call->argument,
          call->frame.function==0x816B8B08?0x816B8B64u:0x816B7EA4u};
    }
    return {};
  }
  // Exact selected, still-entered generic dispatch metadata. This supplies
  // execution identity/lifetime only, never a profile read or dataful grant.
  // The concrete owner separately proves its actual code/module holders and
  // any independently entered profile/read/Commit obligations.
  std::optional<CurrentExecution> CaptureEnteredExecution(uint64_t serial,
      Frame dispatcher,Birth birth) const noexcept {
    const auto* node=FindBirthConst(birth.generation);
    if(lost_ || !Match(dispatcher) || !node || !SameBirth(node->birth,birth) ||
       !node->execution_only || birth.origin!=1 || node->freed || node->delete_entered ||
       node->free_entered || node->execution_active!=serial)return {};
    for(const auto& call:execution_dispatches_)if(call && call->serial==serial){
      const uint32_t depth=dispatcher.function==0x816B8B08?0x70u:0x80u;
      if(call->frame!=dispatcher || !SameBirth(call->birth,birth) || !call->selected ||
         call->callback_returned || dispatcher.sp<depth)return {};
      return CurrentExecution{root_,birth,serial,dispatcher,dispatcher.sp-depth,
          call->callback,call->argument,
          dispatcher.function==0x816B8B08?0x816B8B64u:0x816B7EA4u};
    }
    return {};
  }
  // One exact identity-owned FC58001 operation. These are retained native
  // observations only: no method issues a read, writer, Task or completion.
  class IdentityServiceRequest final {
   public:
    struct Origin {
      Frame worker,producer;Birth worker_birth;
      uint64_t job=0,source=0,work=0;
      uint32_t object=0,rows=0,parameters=0,flags=0,overlap=0;
      uint32_t worker_argument=0; // Retained C838 request, already validated by its real Work.
    };
    struct Copy {
      uint64_t capture=0,serial=0;uint32_t envelope=0,argument=0,overlap=0;
      Birth request_birth;
      bool operator==(const Copy& b) const {return capture==b.capture && serial==b.serial && envelope==b.envelope && argument==b.argument && overlap==b.overlap && SameBirth(request_birth,b.request_birth);}
    };
    struct Pump {
      Frame schedule;Birth birth;uint32_t object=0;
      bool operator==(const Pump& b) const {return schedule==b.schedule && object==b.object && SameBirth(birth,b.birth);}
    };
    struct Tick {
      uint64_t serial=0,dispatch=0;Frame frame;Birth birth;
      bool operator==(const Tick& b) const {return serial==b.serial && dispatch==b.dispatch && frame==b.frame && SameBirth(birth,b.birth);}
    };
    // These are the four actual callsites in the matching C838/C310 bodies,
    // not four interchangeable service requests. No path repeats a callsite:
    // successful cleanup can precede initial; retry and recovery are forward
    // error branches. Keep older completion/free/tick tails independently.
    enum class ProducerLane : uint32_t {kCleanup=1,kInitial=2,kRetry=3,kRecovery=4};
    static std::optional<ProducerLane> ClassifyProducer(const Origin& value) noexcept {
      if(!value.worker.valid() || !value.producer.valid() ||
         value.worker.function!=0x8199C838 || value.producer.function!=0x81830878 ||
         value.worker.root!=value.producer.root || value.worker.thread!=value.producer.thread ||
         value.worker.pcr!=value.producer.pcr || value.worker.sp<0xBC0 ||
         !value.job || value.source<=2 || !value.work || !value.worker_birth.generation ||
         value.worker_birth.origin!=value.source || !Address(value.worker_birth.task) ||
         value.object!=0x81AEE470 || value.overlap ||
         !Address(value.worker_argument) || (value.worker_argument&7) ||
         uint64_t(value.worker_argument)+0xC18>uint64_t(UINT32_MAX)+1 ||
         value.parameters!=value.worker.sp-0x370)return {};
      const bool packed_rows=value.rows==value.worker_argument+0xA68;
      if(value.producer.sp==value.worker.sp-0xBC0 && value.producer.lr==0x8199C488 &&
         !value.rows && value.flags==4)return ProducerLane::kCleanup;
      if(value.producer.sp!=value.worker.sp-0x510)return {};
      if(value.producer.lr==0x8199D52C && packed_rows &&
         (value.flags==4 || value.flags==5))return ProducerLane::kInitial;
      if(value.producer.lr==0x8199D5AC && packed_rows &&
         !value.flags)return ProducerLane::kRetry;
      if(value.producer.lr==0x8199D6A4 && !value.rows && value.flags==2)return ProducerLane::kRecovery;
      return {};
    }
    bool Begin(Origin value) noexcept {
      const auto lane=ClassifyProducer(value);if(begun_ || !lane)return false;
      origin_=value;lane_=*lane;begun_=true;return true;
    }
    // A later native call may start while this operation's genuine provider
    // or Task-free tail is still retained. Its own synchronous producer must
    // have returned, and the native forward-only callsite order must match.
    bool CanPrecede(const Origin& next) const noexcept {
      const auto lane=ClassifyProducer(next);
      if(!begun_ || retired_ || !producer_returned_ || !lane ||
         next.worker!=origin_.worker || !SameBirth(next.worker_birth,origin_.worker_birth) ||
         next.job!=origin_.job || next.source!=origin_.source || next.work!=origin_.work ||
         next.object!=origin_.object || next.parameters!=origin_.parameters ||
         next.worker_argument!=origin_.worker_argument)return false;
      return (lane_==ProducerLane::kCleanup && *lane==ProducerLane::kInitial) ||
          ((lane_==ProducerLane::kCleanup || lane_==ProducerLane::kInitial) &&
              (*lane==ProducerLane::kRetry || *lane==ProducerLane::kRecovery)) ||
          (lane_==ProducerLane::kRetry && *lane==ProducerLane::kRecovery);
    }
    ProducerLane lane() const noexcept {return lane_;}
    bool producer_returned() const noexcept {return producer_returned_;}
    const Origin& origin() const noexcept {return origin_;}
    const Copy& copy() const noexcept {return copy_;}
    bool BindCopy(Copy value) noexcept {
      if(!begun_ || copy_.serial || !value.capture || !value.serial ||
         !Address(value.envelope) || uint64_t(value.envelope)+0x584>uint64_t(UINT32_MAX)+1 ||
         value.argument!=value.envelope+0x1C || origin_.producer.sp<0x670 ||
         value.overlap!=origin_.producer.sp-0x670 || !value.request_birth.generation ||
         value.request_birth.origin!=1 || !Address(value.request_birth.task))return false;
      copy_=value;return true;
    }
    bool EnterAcceptance(Frame frame,const Copy& copy,uint32_t state,
        uint32_t request,uint32_t overlap) noexcept {
      if(!begun_ || accepting_ || acceptance_returned_ || !copy_.serial || copy!=copy_ ||
         !frame.valid() || frame.root!=origin_.worker.root || frame.function!=0x818CD588 ||
         frame.lr!=0x818CDD2C || state!=origin_.object+0x10 ||
         request!=copy_.argument || overlap!=copy_.overlap)return false;
      acceptance_=frame;accepting_=true;return true;
    }
    bool ReturnAcceptance(Frame frame,uint32_t value,uint32_t pending_request,
        uint32_t pending_overlap) noexcept {
      if(!accepting_ || acceptance_returned_ || frame!=acceptance_)return false;
      acceptance_returned_=true;
      if(int32_t(value)<0)return !accepted_;
      if(value || terminal_entered_ || pending_request!=copy_.argument || pending_overlap!=copy_.overlap)return false;
      accepted_=true;return true;
    }
    bool accepted() const noexcept {return accepted_ && !terminal_entered_ && !retired_;}
    bool awaiting_acceptance_return() const noexcept {return accepting_ && !acceptance_returned_;}
    static bool PumpMatches(const Pump& pump,const Origin& origin) noexcept {
      return pump.schedule.valid() && pump.schedule.root==origin.worker.root &&
          pump.schedule.function==0x816BA440 && pump.schedule.lr==0x818CF164 &&
          pump.object==origin.object && pump.birth.generation && pump.birth.origin==1 &&
          Address(pump.birth.task);
    }
    std::optional<Tick> EnterTick(Frame frame,const Pump& pump,const CurrentExecution& dispatch) noexcept {
      if(!accepted() || !PumpMatches(pump,origin_) || !frame.valid() ||
         frame.root!=origin_.worker.root || frame.function!=0x818CEE08 || frame.lr!=0x818CEFE8 ||
         !SameBirth(pump.birth,dispatch.birth) || dispatch.root!=origin_.worker.root ||
         !dispatch.invocation || dispatch.callback!=0x818CEFD8 || dispatch.argument!=origin_.object ||
         dispatch.dispatcher.thread!=frame.thread || dispatch.dispatcher.pcr!=frame.pcr ||
         dispatch.body_sp<0x60 || frame.sp!=dispatch.body_sp-0x60 ||
         !next_tick_ || next_tick_==UINT64_MAX)return {};
      for(const auto& tick:ticks_)if(tick && tick->frame==frame)return {};
      for(auto& tick:ticks_)if(!tick){tick=Tick{next_tick_++,dispatch.invocation,frame,pump.birth};return tick;}
      return {};
    }
    bool WithdrawUnenteredTick(Tick tick) noexcept {
      for(auto& current:ticks_)if(current && *current==tick){current.reset();return true;}
      return false;
    }
    bool ReturnTick(Tick tick,Frame actual,Birth birth) noexcept {
      for(auto& current:ticks_)if(current && *current==tick){
        if(actual!=tick.frame || !SameBirth(birth,tick.birth))return false;
        current.reset();return true;
      }
      return false;
    }
    bool EnterCompletion(Frame frame,uint32_t overlap,uint32_t context,
        uint32_t result,uint32_t extended,uint32_t length) noexcept {
      if(!begun_ || terminal_entered_ || !copy_.serial || !frame.valid() ||
         frame.root!=origin_.worker.root || frame.function!=0x816A2588 ||
         overlap!=copy_.overlap || (context&~1u)!=copy_.envelope || length)return false;
      bool issuer=false;
      if(accepted_ && frame.lr==0x818CEDA0){
        for(const auto& tick:ticks_)if(tick && tick->frame.thread==frame.thread &&
            tick->frame.pcr==frame.pcr && tick->frame.sp>=0x4C0 && frame.sp==tick->frame.sp-0x4C0)issuer=true;
        if(result!=(int32_t(extended)<0?0x65Bu:0u))return false;
      }else if(accepting_ && !acceptance_returned_ && frame.lr==0x818CD5F4 &&
          acceptance_.thread==frame.thread && acceptance_.pcr==frame.pcr &&
          acceptance_.sp>=0x80 && frame.sp==acceptance_.sp-0x80 &&
          result==0x65B && extended==0x800700AA)issuer=true;
      if(!issuer)return false;
      completion_=frame;terminal_entered_=true;return true;
    }
    bool EnterEnvelopeFree(Frame frame,uint32_t envelope) noexcept {
      if(!terminal_entered_ || free_entered_ || envelope!=copy_.envelope ||
         !frame.valid() || frame.root!=completion_.root || frame.thread!=completion_.thread ||
         frame.pcr!=completion_.pcr || completion_.sp<0xA0 || frame.sp!=completion_.sp-0xA0 ||
         frame.function!=0x816E6AB0 || frame.lr!=0x816A26E8)return false;
      free_=frame;free_entered_=true;return true;
    }
    bool ReturnEnvelopeFree(Frame frame) noexcept {
      if(!free_entered_ || free_returned_ || frame!=free_)return false;
      free_returned_=true;return true;
    }
    bool ReturnCompletion(Frame frame) noexcept {
      if(!terminal_entered_ || terminal_returned_ || !free_returned_ || frame!=completion_)return false;
      terminal_returned_=true;return true;
    }
    bool ReturnProducer(Frame frame) noexcept {
      if(!begun_ || producer_returned_ || frame!=origin_.producer)return false;
      producer_returned_=true;return true;
    }
    void ObserveCancel() noexcept {cancel_requested_=true;}
    bool cancel_requested() const noexcept {return cancel_requested_;}
    bool terminal_returned() const noexcept {return terminal_returned_;}
    bool ready_to_reap() const noexcept {
      if(!terminal_returned_ || !producer_returned_ || !acceptance_returned_)return false;
      for(const auto& tick:ticks_)if(tick)return false;
      return true;
    }
    void RetireAtActualStop() noexcept {retired_=true;}
   private:
    Origin origin_{};ProducerLane lane_=ProducerLane::kCleanup;
    Copy copy_{};Frame acceptance_{},completion_{},free_{};
    std::array<std::optional<Tick>,16> ticks_{};uint64_t next_tick_=1;
    bool begun_=false,accepting_=false,acceptance_returned_=false,accepted_=false;
    bool terminal_entered_=false,terminal_returned_=false,free_entered_=false,free_returned_=false;
    bool producer_returned_=false,cancel_requested_=false,retired_=false;
  };
  struct ExecutionControl {
    Frame frame;Birth birth;uint32_t mask=0,callback=0,argument=0;
  };
  uint64_t BeginExecutionDispatch(Frame frame,Birth birth) {
    const auto* node=FindBirthConst(birth.generation);
    if(lost_ || !Match(frame) || (frame.function!=0x816B8B08 && frame.function!=0x816B7DE8) || !node ||
       !SameBirth(node->birth,birth) || !node->execution_only || node->freed || node->free_entered)return 0;
    for(const auto& call:execution_dispatches_)if(call && call->frame==frame)return 0;
    const auto serial=Next();if(!serial)return 0;
    for(auto& call:execution_dispatches_)if(!call){call=ExecutionDispatch{serial,frame,birth};return serial;}
    return 0;
  }
  // Registers at the genuine common mtctr, AFTER lwz argument/callback. The
  // owner also matches an already retained producer/rebind module generation.
  // Do not resample Task bytes or assume Modify made the pair atomically.
  bool ExecutionCallbackSelected(uint64_t serial,Frame body,uint32_t task,
      uint32_t callback,uint32_t argument) {
    for(auto& call:execution_dispatches_)if(call && call->serial==serial){
      auto* node=FindBirth(call->birth.generation);
      if(lost_ || call->selected || !node || !SameBirth(node->birth,call->birth) ||
         node->freed || node->free_entered || node->execution_active || !Address(callback) ||
         !ExecutionBody(*call,body,task) ||
         (call->frame.function==0x816B8B08 ? body.lr!=0x816B8B48 :
          body.lr!=0x816B7E4C && body.lr!=0x816B7E90))return false;
      call->selected=true;call->callback=callback;call->argument=argument;
      node->execution_active=serial;return true;
    }
    return false;
  }
  bool ExecutionCallbackReturned(uint64_t serial,Frame body,uint32_t task,uint32_t result) {
    for(auto& call:execution_dispatches_)if(call && call->serial==serial){
      auto* node=FindBirth(call->birth.generation);
      if(lost_ || !call->selected || call->callback_returned || !node ||
         !SameBirth(node->birth,call->birth) || node->freed || node->free_entered ||
         node->execution_active!=serial || !ExecutionBody(*call,body,task) ||
         body.lr!=(call->frame.function==0x816B8B08?0x816B8B64u:0x816B7EA4u))return false;
      call->callback_returned=true;call->result=result;node->execution_active=0;return true;
    }
    return false;
  }
  bool ReturnExecutionDispatch(uint64_t serial,Frame frame,Birth birth) {
    for(auto& call:execution_dispatches_)if(call && call->serial==serial){
      if(lost_ || call->frame!=frame || !SameBirth(call->birth,birth) ||
         (call->selected && !call->callback_returned))return false;
      // This exact retained record survives real native free/reuse. No read
      // from a replacement Task can settle this older own-return.
      call.reset();return true;
    }
    return false;
  }
  bool BeginExecutionControl(Frame frame,Birth birth,uint32_t mask=0,
      uint32_t callback=0,uint32_t argument=0) {
    const auto* node=FindBirthConst(birth.generation);
    if(lost_ || !Match(frame) || !node || !SameBirth(node->birth,birth) ||
       !node->execution_only || node->freed || node->free_entered)return false;
    switch(frame.function){
      case 0x816B9AA8:case 0x816B9F08:case 0x816BA058:
      case 0x816B7B68:case 0x816B7600:break;
      default:return false;
    }
    if(frame.function==0x816B9AA8 && (mask&1) && !Address(callback))return false;
    for(const auto& call:execution_controls_)if(call && call->frame==frame)return false;
    for(auto& call:execution_controls_)if(!call){
      call=ExecutionControl{frame,birth,mask,mask&1?callback:0u,mask&2?argument:0u};return true;
    }
    return false;
  }
  bool ReturnExecutionControl(Frame frame,Birth birth) {
    for(auto& call:execution_controls_)if(call && call->frame==frame){
      if(lost_ || !SameBirth(call->birth,birth))return false;
      // Void/residue and Cancel return are NOT completion/ref/free receipts.
      // Retain the admitted old birth even if original postwork freed it.
      call.reset();return true;
    }
    return false;
  }
  static bool ExecutionBody(const ExecutionDispatch& call,Frame body,uint32_t task) noexcept {
    return body.valid() && body.root==call.frame.root && body.thread==call.frame.thread &&
        body.pcr==call.frame.pcr && body.function==call.frame.function &&
        call.frame.sp>=(call.frame.function==0x816B8B08?0x70u:0x80u) &&
        body.sp==call.frame.sp-(call.frame.function==0x816B8B08?0x70u:0x80u) && task==call.birth.task;
  }
  uint64_t BeginSchedule(Frame frame,uint64_t earlier_origin,uint32_t callback,
      uint32_t argument,uint32_t parameters,uint32_t output) {
    return BeginScheduleImpl(frame,earlier_origin,callback,argument,parameters,output,false,false);
  }
  // Only a prebound common Invocation may supply this transport-only route.
  // Its argument is an opaque ticket DWORD (10001 is valid), not a guest
  // payload pointer. Source1/callback/zero params cannot be retargeted.
  uint64_t BeginSharedContinuationSchedule(Frame frame,uint32_t ticket,uint32_t output) {
    if(!ticket){Lose(__LINE__,10);return 0;}
    return BeginScheduleImpl(frame,1,0x81696558,ticket,0,output,true,true);
  }
  // The typed identity Expected proves the same scalar-ticket ABI while
  // retaining its actual prepared job source, never transport source1.
  uint64_t BeginIdentityContinuationSchedule(Frame frame,uint64_t source,uint32_t ticket,uint32_t output) {
    if(source<=2 || !ticket){Lose(__LINE__,6);return 0;}
    return BeginScheduleImpl(frame,source,0x81696558,ticket,0,output,false,true);
  }
  uint64_t BeginScheduleImpl(Frame frame,uint64_t earlier_origin,uint32_t callback,
      uint32_t argument,uint32_t parameters,uint32_t output,bool transport_only,bool continuation_ticket,bool execution_only=false) {
    if (lost_ || !Match(frame) || frame.function!=0x816BA440 || !earlier_origin ||
        !Address(callback) || (!continuation_ticket && !execution_only && argument && !Address(argument)) ||
        (parameters && !Address(parameters)) || !Address(output)) { Lose(__LINE__,9); return 0; }
    for (const auto& old:schedules_) {
      if (old && old->frame==frame) { Lose(__LINE__,9); return 0; }
    }
    const auto serial=Next();if (!serial) { return 0; }
    for (auto& value:schedules_) { if (!value) {
      value=Schedule{serial,earlier_origin,frame,{},callback,argument,parameters,output};
      value->transport_only=transport_only;value->continuation_ticket=continuation_ticket;value->execution_only=execution_only;
      value->resident_physical=(callback==0x816F4298 && frame.lr==0x816F4610 && output==0x81AAC29C) ||
          (callback==0x816F4410 && frame.lr==0x816F4644 && output==0x81AAC298);
      return serial;
    } }
    Lose(__LINE__,9); return 0;
  }
  uint64_t BeginPeriodicSchedule(Frame frame,PeriodicFactory& factory,uint32_t callback,
      uint32_t argument,uint32_t parameters,uint32_t output,
      std::array<uint32_t,2> options,uint32_t actual_process) {
    if(!factory.MatchesSchedule(frame,callback,argument,parameters,output) ||
        options!=std::array<uint32_t,2>{8,5000} || actual_process!=2)return 0;
    const auto serial=BeginScheduleImpl(frame,1,callback,argument,parameters,output,false,false);
    auto* call=FindSchedule(serial);
    if(!call || !factory.EnterSchedule(frame)){Lose(__LINE__,30);return 0;}
    call->periodic_factory=factory.serial;call->periodic_options=options;
    call->periodic_process=actual_process;return serial;
  }
  // Matched successful ORIGINAL Create output, before native publication.
  // The original normalizer executes process/TLS affiliation itself. Preserve
  // that output on this exact birth instead of predicting affiliation or
  // accepting either value afresh at each consumer.
  bool BindPeriodicAllocation(uint64_t serial,Frame allocation,Birth actual,
      std::array<uint32_t,2> native_state) {
    auto* call=FindSchedule(serial);auto* node=FindBirth(actual.generation);
    if(lost_ || !call || !node || !call->periodic_factory ||
       call->periodic_options!=std::array<uint32_t,2>{8,5000} || call->periodic_process!=2 ||
       !call->allocation_returned || call->allocation!=allocation || call->publish_entered ||
       node->published || node->periodic_allocation || node->freed || node->free_entered ||
       node->birth.task!=actual.task || node->birth.origin!=actual.origin || actual.origin!=1 ||
       call->birth!=actual.generation || node->periodic_factory!=call->periodic_factory ||
       (native_state[0]!=0x04A00008 && native_state[0]!=0x02A00008) || native_state[1]!=5000)return false;
    node->periodic_allocation=PeriodicAllocation{call->periodic_factory,actual,
        call->frame,allocation,call->periodic_options,call->periodic_process,native_state[0],native_state[1]};
    return true;
  }
  bool MatchesPeriodicAllocation(Birth actual,uint64_t factory,
      std::array<uint32_t,2> native_state) const noexcept {
    const auto* node=FindBirthConst(actual.generation);
    if(lost_ || !node || !node->periodic_allocation)return false;
    const auto& receipt=*node->periodic_allocation;
    return factory && node->periodic_factory==factory && receipt.factory==factory &&
        receipt.birth.generation==actual.generation && receipt.birth.origin==actual.origin &&
        receipt.birth.task==actual.task && node->birth.origin==actual.origin && node->birth.task==actual.task &&
        receipt.schedule.root==root_ && receipt.allocation.root==root_ &&
        receipt.options==std::array<uint32_t,2>{8,5000} && receipt.process==2 &&
        native_state==std::array<uint32_t,2>{receipt.flags,receipt.period};
  }
  bool RetainsPeriodicLifetime(const PeriodicFactory& factory,Birth birth) const noexcept {
    const auto* node=FindBirthConst(birth.generation);
    return factory.serial && factory.root==root_ && factory.object && node &&
        SameBirth(node->birth,birth) && node->published && !node->free_entered && !node->freed &&
        node->callback==0x81909CF8 && node->argument==factory.object && node->periodic_allocation &&
        MatchesPeriodicAllocation(birth,factory.serial,
            {node->periodic_allocation->flags,node->periodic_allocation->period});
  }
  std::optional<PeriodicFrameContext> RetainPeriodicSubjectContext(Frame frame,
      const PeriodicFactory& factory,Birth birth) const noexcept {
    if(!Match(frame) || !RetainsPeriodicLifetime(factory,birth))return {};
    switch(frame.function){
      case 0x819068D0:case 0x816B8B08:case 0x816B7600:
      case 0x816B9F08:case 0x816BA058:case 0x816B7490:
      case 0x816B7D98:case 0x816B7500:case 0x816E6AB0:break;
      default:return {}; // No new factory/Schedule/Create/publication authority.
    }
    return PeriodicFrameContext{frame,factory.serial,factory.object,
        FindBirthConst(birth.generation)->periodic_allocation};
  }
  static std::optional<PeriodicFrameContext> RetainAdmittedPeriodicFactory(
      Frame frame,const PeriodicFactory& factory) noexcept {
    if(!frame.valid() || !factory.serial || factory.failed || !factory.enabling ||
        factory.enable!=frame || factory.root!=frame.root || frame.function!=0x8190A358)return {};
    return PeriodicFrameContext{frame,factory.serial,factory.object,{}};
  }
  // Only the exact in-flight producer's native children inherit context after
  // admission closes. A new factory call is never such a child. State still
  // proves the parent is the active admitted owning Work before using this.
  static std::optional<PeriodicFrameContext> RetainPeriodicProducerChild(
      Frame frame,const PeriodicFrameContext& parent) noexcept {
    if(!parent.Matches(parent.frame,parent.factory) || parent.allocation || !frame.valid() ||
        frame.root!=parent.frame.root || frame.thread!=parent.frame.thread ||
        frame.pcr!=parent.frame.pcr)return {};
    bool child=false;
    if(parent.frame.function==0x8190A358)
      child=frame.function==0x816BA440 && parent.frame.sp>=0x70 &&
          frame.sp==parent.frame.sp-0x70 && frame.lr==0x8190A3B0;
    else if(parent.frame.function==0x816BA440)
      child=parent.frame.sp>=0x90 && frame.sp==parent.frame.sp-0x90 &&
          ((frame.function==0x816B9A18 && frame.lr==0x816BA46C) ||
           (frame.function==0x816B9BB8 && frame.lr==0x816BA4D8));
    if(!child)return {};
    return PeriodicFrameContext{frame,parent.factory,parent.object,{}};
  }
  bool IsPeriodicSubject(uint64_t generation,uint64_t factory=0) const noexcept {
    for(const auto& node:nodes_)if(node && node->birth.generation==generation && !node->freed && !node->free_entered)
      return node->periodic_factory && (!factory || node->periodic_factory==factory) && node->birth.origin==1;
    return false;
  }
  std::optional<Birth> BeginPeriodicDispatch(Frame frame,uint32_t actual_task,uint32_t callback,uint32_t argument,
      std::array<uint32_t,2> native_state) {
    auto* node=FindLiveTask(actual_task);
    if(lost_ || !Match(frame) || frame.function!=0x816B8B08 || !node || !node->periodic_factory ||
        !node->published || node->persistent_active || node->birth.origin!=1 ||
        node->callback!=callback || node->argument!=argument ||
        !MatchesPeriodicAllocation(node->birth,node->periodic_factory,native_state))return {};
    for(auto& call:periodic_dispatches_)if(!call){
      call=PeriodicDispatch{frame,node->birth,false,node->periodic_factory,*node->periodic_allocation};node->persistent_active=true;node->persistent_work=frame;return node->birth;
    }
    Lose(__LINE__,31);return {};
  }
  bool PeriodicCallbackReturned(Frame dispatcher,Frame boundary,Birth birth) {
    auto* node=FindBirth(birth.generation);
    if(lost_ || !node || node->freed || !node->periodic_factory || !node->persistent_active ||
        node->persistent_work!=dispatcher || node->birth.task!=birth.task || node->birth.origin!=1 ||
        boundary.root!=dispatcher.root || boundary.thread!=dispatcher.thread || boundary.pcr!=dispatcher.pcr ||
        dispatcher.sp<0x70 || boundary.sp!=dispatcher.sp-0x70 || boundary.function!=0x816B7600 || boundary.lr!=0x816B8B84)return false;
    for(auto& call:periodic_dispatches_)if(call && call->frame==dispatcher && call->birth.generation==birth.generation){
      if(call->callback_returned)return false;
      call->callback_returned=true;node->persistent_active=false;return true;
    }
    return false;
  }
  bool ReturnPeriodicDispatch(Frame frame,Birth birth,
      std::optional<std::array<uint32_t,2>> native_state) {
    // A real free/reuse can replace the numeric Node before this older
    // dispatch returns. Its own entry keeps the exact immutable allocation
    // witness; no word from the replacement Task can complete the old call.
    for(auto& call:periodic_dispatches_)if(call && call->frame==frame && call->birth.generation==birth.generation &&
        call->birth.task==birth.task && call->birth.origin==birth.origin){
      const auto* retained=FindBirthConst(birth.generation);
      const auto& allocation=call->allocation;
      if(!SameBirth(allocation.birth,birth) || allocation.factory!=call->factory)return false;
      const bool live=retained && !retained->free_entered && !retained->freed;
      if(live ? !native_state || !MatchesPeriodicAllocation(birth,call->factory,*native_state) :
          bool(native_state) || !call->callback_returned)return false;
      // Without the exact postcallback cut, originalB8B08 reached its canceled
      // skip path; it must still own a live nonserial periodic generation.
      if(!call->callback_returned){
        auto* node=FindBirth(birth.generation);
        if(!node || node->freed || !node->periodic_factory || !node->persistent_active || node->persistent_work!=frame)return false;
        node->persistent_active=false;
      }
      call.reset();return !lost_;
    }
    return false;
  }
  bool PeriodicWaitReturned(Birth birth) {
    auto* node=FindBirth(birth.generation);
    if(lost_ || !node || node->freed || node->birth.task!=birth.task || node->birth.origin!=1 ||
        !node->periodic_factory)return false;
    node->periodic_stopped=true;return true;
  }
  bool PeriodicFactoryQuiescent(uint64_t factory) const noexcept {
    if(lost_ || !factory)return false;
    for(const auto& call:periodic_dispatches_)if(call && call->factory==factory)return false;
    for(const auto& call:schedules_)if(call && call->periodic_factory==factory)return false;
    for(const auto& node:nodes_)if(node && node->periodic_factory==factory &&
        (node->persistent_active || (!node->freed && !node->periodic_stopped)))return false;
    return true;
  }
  // Actual F44D0 creates separate persistent F2618/F3A40 Tasks via BA668 (NOT Schedule).
  // Retain the same real B9A18 output allocation generation before Modify or
  // Reschedule can publish any work. This factory is only for that known path.
  uint64_t BeginPersistentCreate(Frame frame,uint64_t earlier_boot_origin,uint32_t output) {
    if (lost_ || !Match(frame) || frame.function!=0x816BA668 ||
        !earlier_boot_origin ||
        !((frame.lr==0x816F4568 && output==0x81AAC270) ||
          (frame.lr==0x816F45B0 && output==0x81AAC274))) { Lose(__LINE__,7);return 0; }
    const auto serial=Next();if (!serial) { return 0; }
    for (auto& value:schedules_) { if (!value) {
      value=Schedule{};value->serial=serial;value->origin=earlier_boot_origin;
      value->frame=frame;value->output=output;value->created_persistent=true;return serial;
    } }
    Lose(__LINE__,7);return 0;
  }
  bool BindProfileProducer(uint64_t schedule,uint64_t serial,Frame owner,
      std::optional<std::array<uint32_t,2>> options,ProfileOutput output_proof) {
    auto* call=FindSchedule(schedule);
    if(lost_ || !call || call->producer || !serial || !Match(owner) || call->origin<=2 ||
       call->created_persistent || call->resident_physical || call->transport_only ||
       call->continuation_ticket || call->periodic_factory || call->allocation_entered ||
       owner.thread!=call->frame.thread || owner.pcr!=call->frame.pcr ||
       !ProfileProducerFrameBytes(owner.function) || owner.sp<ProfileProducerFrameBytes(owner.function) ||
       call->frame.sp!=owner.sp-ProfileProducerFrameBytes(owner.function) ||
       CaptureProfileOutput(owner,call->frame,call->output,output_proof.overlap,output_proof.packet,
           output_proof.kind==ProfileOutput::Kind::kAsyncPacket?
             std::array<uint32_t,3>{0xFEu,output_proof.overlap,output_proof.packet}:std::array<uint32_t,3>{})!=
         std::optional<ProfileOutput>{output_proof} ||
       (output_proof.kind==ProfileOutput::Kind::kStack && (output_proof.packet || output_proof.overlap)) ||
       call->parameters!=call->frame.sp+(owner.function==0x8199E3E0?0x70u:0x58u) ||
       !options ||
       (owner.function==0x8199E3E0 ? call->callback!=0x8199C838 && call->callback!=0x8199E148 :
        call->callback!=0x8199C838))return false;
    ProfileProducer p;p.owner_serial=serial;p.owner=owner;p.schedule=call->frame;
    p.callback=call->callback;p.argument=call->argument;p.output=call->output;p.output_proof=output_proof;
    p.has_options=bool(options);if(options)p.options=*options;call->producer=p;return true;
  }
  bool BindProfileNativeState(uint64_t schedule,uint32_t task,uint32_t flags,uint32_t data) {
    auto* call=FindSchedule(schedule);auto* node=call?FindBirth(call->birth):nullptr;
    if(lost_ || !call || !node || node->birth.task!=task || !node->producer ||
       !node->published || node->producer->native_bound)return false;
    node->producer->native_flags=flags;node->producer->native_data=data;
    node->producer->native_bound=true;call->producer=node->producer;return true;
  }
  bool ObserveProfileCancelState(Birth birth,uint32_t flags,uint32_t data) {
    auto* node=FindBirth(birth.generation);
    if(lost_ || !node || !SameBirth(node->birth,birth) || node->freed || node->delete_entered || node->free_entered)return false;
    if(!node->producer)return true; // A native recursive child issues no producer.
    auto& p=*node->producer;
    if(!p.native_bound || p.native_data!=data ||
       (flags!=p.native_flags && flags!=(p.native_flags&~0x10000000u)))return false;
    p.native_flags=flags;
    for(auto& call:schedules_)if(call && call->birth==birth.generation && call->producer)
      call->producer->native_flags=flags;
    return true; // Observe only real native Cancel mutation.
  }
  const ProfileProducer* ProfileProducerFor(Birth birth) const noexcept {
    const auto* node=FindBirthConst(birth.generation);
    return !lost_ && node && SameBirth(node->birth,birth) && !node->freed && !node->free_entered &&
        !node->delete_entered && node->producer?&*node->producer:nullptr;
  }
  void ObserveProfileOwnerClose(Frame actual,Birth birth) noexcept {
    auto* node=FindBirth(birth.generation);
    if(!node || !SameBirth(node->birth,birth) || !node->producer)return;
    auto& p=*node->producer;
    if(p.output_proof.kind==ProfileOutput::Kind::kStack && p.schedule_returned && actual.function==0x816B7490 && actual.root==p.owner.root &&
       actual.thread==p.owner.thread && actual.pcr==p.owner.pcr && actual.sp==p.schedule.sp)
      p.owner_close_entered=true; // Conservative withdrawal, never a release receipt.
  }
  bool EnterAllocation(uint64_t schedule,Frame frame,uint32_t parameters,uint32_t output) {
    auto* call=FindSchedule(schedule);
    if (lost_ || !call || call->allocation_entered || !Match(frame) ||
        ClassifyAllocationProducer(call->frame,frame,call->parameters,parameters,output)!=
            (call->created_persistent?AllocationProducer::kPersistentCreate:AllocationProducer::kSchedule)) { Lose(__LINE__,11); return false; }
    call->allocation=frame;call->allocation_output=output;call->allocation_entered=true;return true;
  }
  // CRITICAL ABI: r3 is HRESULT. Only the actual r4 OUTPUT after this matched
  // successful normal return supplies a Task pointer. Never r3-as-address.
  std::optional<Birth> ReturnAllocation(uint64_t schedule,Frame frame,
      uint32_t actual_hresult,std::optional<uint32_t> actual_output) {
    auto* call=FindSchedule(schedule);
    if (lost_ || !call || !call->allocation_entered || call->allocation_returned ||
        frame!=call->allocation) { Lose(__LINE__,18); return {}; }
    call->allocation_returned=true;
    if (int32_t(actual_hresult)<0) { return {}; }
    if (!actual_output || !Address(*actual_output) || FindLiveTask(*actual_output)) { Lose(__LINE__,18); return {}; }
    const auto generation=Next(); if (!generation) { return {}; }
    for (auto& value:nodes_) { if (!value || value->freed) {
      const Birth birth{generation,call->origin,*actual_output};
      value=Node{birth,call->callback,call->argument};
      value->created_persistent=call->created_persistent;value->resident_physical=call->resident_physical;value->execution_only=call->execution_only;
      value->transport_only=call->transport_only;value->continuation_ticket=call->continuation_ticket;
      value->persistent_output=call->created_persistent?call->output:0;
      value->periodic_factory=call->periodic_factory;
      if(call->producer){call->producer->birth=birth;value->producer=call->producer;}
      call->birth=generation;return birth;
    } }
    Lose(__LINE__,18);return {};
  }
  // B9BB8 ENTRY precedes stores callback+30/argument+34 AND native publication.
  // Schedule's caller output was already written tagged at BA4C8. This earlier
  // binding therefore covers a worker running before Schedule returns.
  bool BeforePublication(uint64_t schedule,Frame frame,uint32_t actual_task,
      uint32_t callback,uint32_t argument,std::optional<uint32_t> actual_tagged_output) {
    auto* call=FindSchedule(schedule);auto* node=call ? FindBirth(call->birth) : nullptr;
    if (lost_ || !call || !node || call->created_persistent ||
        !call->allocation_returned || call->publish_entered ||
        !Match(frame) || frame.function!=0x816B9BB8 || frame.lr!=0x816BA4D8 ||
        frame.thread!=call->frame.thread || frame.pcr!=call->frame.pcr ||
        frame.sp+0x90!=call->frame.sp || node->birth.task!=actual_task ||
        callback!=call->callback || argument!=call->argument ||
        !actual_tagged_output || *actual_tagged_output!=(actual_task|1u) ||
        (node->periodic_factory && !node->periodic_allocation)) { Lose(__LINE__,3);return false; }
    call->publish_entered=true;node->published=true;
    if(node->producer){node->producer->published=true;call->producer=node->producer;}
    return true;
  }
  bool ReturnSchedule(uint64_t schedule,Frame frame,uint32_t actual_hresult,
      std::optional<std::array<uint32_t,2>> actual_native_state={}) {
    auto* call=FindSchedule(schedule);
    if (!call || lost_ || call->created_persistent || frame!=call->frame || !call->allocation_returned ||
        (int32_t(actual_hresult)>=0 && !call->publish_entered)) { Lose(__LINE__,26);return false; }
    if(call->producer && call->birth){
      auto& p=*call->producer;auto* node=FindBirth(call->birth);
      if(!SameBirth(p.birth,Birth{call->birth,call->origin,p.birth.task}))return false;
      if(int32_t(actual_hresult)>=0){
        // Genuine deletion can race this older own-return. The exact scalar
        // receipt survives address/slot reuse; no freed Task/packet is read.
        if(!p.native_bound)return false;
        if(p.release_started){if(actual_native_state)return false;}
        else {
          if(!node || !SameBirth(node->birth,p.birth) || node->delete_entered || node->free_entered || node->freed ||
             !actual_native_state || (*actual_native_state)[1]!=p.native_data ||
             ((*actual_native_state)[0]!=p.native_flags &&
              (*actual_native_state)[0]!=(p.native_flags&~0x00080000u)))return false;
          p.native_flags=(*actual_native_state)[0];
        }
      }
      p.schedule_returned=true;p.schedule_succeeded=int32_t(actual_hresult)>=0;
      if(node && SameBirth(node->birth,p.birth) && node->producer)node->producer=p;
    }
    if(const auto* node=FindBirthConst(call->birth);node && node->periodic_factory &&
        int32_t(actual_hresult)>=0 && (!actual_native_state ||
        !MatchesPeriodicAllocation(node->birth,node->periodic_factory,*actual_native_state)))return false;
    if(int32_t(actual_hresult)<0)if(auto* node=FindBirth(call->birth))
      node->schedule_rejected=true;
    // A failed Schedule can still have a real allocated/published Task: its
    // native zero-ref Close is independently required; never discard its node.
    // After actual free, a rejected never-started Task has no callback return
    // duty. A started/unreturned Task still cannot be treated as closed.
    for (auto& value:schedules_) { if (value && value->serial==schedule) { value.reset();return true; } }
    Lose(__LINE__,26);return false;
  }
  bool ReturnPersistentCreate(uint64_t serial,Frame frame,uint32_t actual_hresult,
      std::optional<uint32_t> actual_tagged_output) {
    auto* call=FindSchedule(serial);auto* node=call ? FindBirth(call->birth) : nullptr;
    if (lost_ || !call || !call->created_persistent || frame!=call->frame ||
        !call->allocation_returned ||
        (int32_t(actual_hresult)>=0 && (!node || !actual_tagged_output ||
            *actual_tagged_output!=(node->birth.task|1u)))) { Lose(__LINE__,24);return false; }
    for (auto& value:schedules_) { if (value && value->serial==serial) { value.reset();return true; } }
    Lose(__LINE__,24);return false;
  }
  std::optional<Birth> BeforeExecutionOnlyTimedOptions(Frame frame,uint32_t task,uint32_t mask,
      TimedOptions before,TimedOptions after) {
    const auto* node=FindLiveTask(task);
    if(lost_ || !node || !Match(frame) || frame.function!=0x816B9AA8 ||
        !IsExecutionOnlyOptionsSubject(node->birth.generation) ||
        !DecodeModifyFields(mask).options_only() || !PreservesTimedOptions(before,after)){
      Lose(__LINE__,29);return {};
    }
    for(const auto& value:modifications_)if(value &&
        (value->frame==frame || value->birth==node->birth.generation)){Lose(__LINE__,29);return {};}
    for(auto& value:modifications_)if(!value){
      value=Modification{frame,node->birth.generation,Modification::Kind::kTimedOptions,before,after};return node->birth;
    }
    Lose(__LINE__,29);return {};
  }
  bool ReturnExecutionOnlyTimedOptions(Frame frame,Birth birth) {
    for(auto& value:modifications_)if(value && value->frame==frame){
      const auto* node=FindBirth(value->birth);
      if(lost_ || !node || node->freed || node->free_entered ||
          value->kind!=Modification::Kind::kTimedOptions || birth.generation!=node->birth.generation ||
          birth.origin!=node->birth.origin || birth.task!=node->birth.task ||
          !IsExecutionOnlyOptionsSubject(node->birth.generation)){Lose(__LINE__,30);return false;}
      value.reset();return true;
    }
    Lose(__LINE__,30);return false;
  }
  // Exact original Modify at F4588 sets callback plus options (mask5), keeps
  // its genuinely observed existing argument, and may publish work before its
  // own return. Bind before B9AA8 writes callback+30, not at later worker entry.
  std::optional<Birth> BeforePersistentCallbackBinding(Frame frame,uint32_t actual_task,
      uint32_t mask,uint32_t callback,std::optional<uint32_t> actual_existing_argument) {
    auto* node=FindLiveTask(actual_task);
    if (lost_ || !node || !node->created_persistent || node->persistent_callback_bound ||
        !Match(frame) || frame.function!=0x816B9AA8 || mask!=5 ||
        !((frame.lr==0x816F458C && callback==0x816F2618) ||
          (frame.lr==0x816F45D4 && callback==0x816F3A40)) ||
        !actual_existing_argument ||
        (*actual_existing_argument && !Address(*actual_existing_argument))) { Lose(__LINE__,2);return {}; }
    const uint32_t required_output=callback==0x816F2618?0x81AAC270u:0x81AAC274u;
    bool matching_factory=false;
    // The allocator generation records which exact persistent factory created it.
    if(node->persistent_output==required_output)matching_factory=true;
    if(!matching_factory){Lose(__LINE__,2);return {};}
    for (const auto& value:modifications_) {
      if (value && (value->frame==frame || value->birth==node->birth.generation)) { Lose(__LINE__,2);return {}; }
    }
    for (auto& value:modifications_) { if (!value) {
      value=Modification{frame,node->birth.generation};
      node->callback=callback;node->argument=*actual_existing_argument;
      node->persistent_callback_bound=true;node->published=true;return node->birth;
    } }
    Lose(__LINE__,2);return {};
  }
  bool ReturnPersistentCallbackBinding(Frame frame) {
    for (auto& value:modifications_) { if (value && value->frame==frame) {
      const auto* node=FindBirth(value->birth);
      if (lost_ || !node || node->freed || value->kind!=Modification::Kind::kPersistentBinding || !node->persistent_callback_bound) { Lose(__LINE__,23);return false; }
      value.reset();return true;
    } }
    Lose(__LINE__,23);return false;
  }
  // A real persistent allocation may run repeatedly. Invocation identity is
  // distinct from allocation generation; neither return nor cancellation frees it.
  std::optional<Birth> BeginPersistentWork(Frame frame,uint32_t actual_current_task,uint32_t argument) {
    auto* node=FindLiveTask(actual_current_task);
    if(lost_ || !node || !Match(frame) ||
       !((node->created_persistent && node->persistent_callback_bound) || node->resident_physical) ||
       !node->published || node->persistent_active ||
       frame.function!=node->callback || argument!=node->argument){Lose(__LINE__,8);return {};}
    node->persistent_active=true;node->persistent_work=frame;return node->birth;
  }
  bool ReturnPersistentWork(Frame frame,Birth birth) {
    auto* node=FindBirth(birth.generation);
    if(lost_ || !node || node->freed || node->birth.task!=birth.task ||
       node->birth.origin!=birth.origin || !node->persistent_active ||
       node->persistent_work!=frame){Lose(__LINE__,25);return false;}
    node->persistent_active=false;node->persistent_work={};return true;
  }
  // Only the owner-controlled pre-body park can cancel this borrowed invocation.
  // No native body return, Task completion, close or free is asserted.
  bool CancelPersistentBeforeNative(Frame frame,Birth birth) {
    return ReturnPersistentWork(frame,birth);
  }
  bool CancelExternalBeforeNative(Frame frame,Birth birth) {
    return ReturnExternalWork(frame,birth);
  }
  std::optional<Birth> EnterWork(Frame frame,uint32_t actual_current_task,uint32_t actual_argument) {
    auto* node=FindLiveTask(actual_current_task);
    if (lost_ || !node || node->created_persistent || node->transport_only || node->periodic_factory || node->execution_only || !Match(frame) || !node->published || node->started ||
        frame.function!=node->callback || actual_argument!=node->argument) { Lose(__LINE__,15);return {}; }
    node->work=frame;node->started=true;return node->birth;
  }
  bool ReturnWork(Frame frame,Birth birth) {
    auto* node=FindBirth(birth.generation);
    if (lost_ || !node || node->birth.origin!=birth.origin || node->birth.task!=birth.task ||
        !node->started || node->returned || node->freed || node->work!=frame) { Lose(__LINE__,28);return false; }
    node->returned=true;return true;
  }
  bool EnterClose(Frame frame,uint32_t actual_task) {
    auto* node=FindLiveTask(actual_task);
    if (lost_ || !node || !Match(frame) || frame.function!=0x816B7490) { Lose(__LINE__,12);return false; }
    for (const auto& old:closes_) { if (old && old->frame==frame) { Lose(__LINE__,12);return false; } }
    for (auto& value:closes_) { if (!value) { value=Close{frame,node->birth.generation,node->execution_only};return true; } }
    Lose(__LINE__,12);return false;
  }
  // Actual zero-ref Close invokes slot0(task,1). These exact deleting
  // destructors call native XMemFree; the original Task allocation can be
  // reused by another real B9A18 before Close's own return. Keep the old
  // generation/Close frame while removing only its live numeric-address join
  // at the authenticated free entry. New generation still requires a genuine
  // successful allocation OUTPUT; neither free entry nor pointer equality
  // alone creates one. No native Task bytes are read after free entry.
  bool EnterDeletingDestructor(Frame frame,uint32_t actual_task,uint32_t flags) {
    auto* node=FindLiveTask(actual_task);
    if(lost_ || !node || !Match(frame) || flags!=1 ||
       (frame.function!=0x816B7D98 && frame.function!=0x816B7500) ||
       frame.lr!=0x816B74E8 || node->delete_entered ||
       ((node->started && !node->returned) || node->persistent_active || node->execution_active)){Lose(__LINE__,13);return false;}
    bool parent=false;
    for(const auto& close:closes_)if(close && close->birth==node->birth.generation &&
        close->frame.thread==frame.thread && close->frame.pcr==frame.pcr &&
        close->frame.sp>=0x60 && frame.sp==close->frame.sp-0x60)parent=true;
    for(const auto& work:external_)if(work && work->birth.generation==node->birth.generation){Lose(__LINE__,13);return false;}
    for(const auto& modify:modifications_)if(modify && modify->birth==node->birth.generation){Lose(__LINE__,13);return false;}
    if(!parent){Lose(__LINE__,13);return false;}
    for(auto& d:deletions_)if(!d){
      d=Deletion{frame,{},node->birth.generation,node->execution_only};node->delete_entered=true;
      if(node->producer){
        node->producer->release_started=true;
        for(auto& call:schedules_)if(call && call->birth==node->birth.generation && call->producer)
          call->producer->release_started=true;
      }
      return true;
    }
    Lose(__LINE__,13);return false;
  }
  bool EnterTaskFree(Frame frame,uint32_t actual_task) {
    if(lost_ || !Match(frame) || frame.function!=0x816E6AB0){Lose(__LINE__,14);return false;}
    for(auto& d:deletions_)if(d){
      auto* node=FindBirth(d->birth);
      const bool derived=d->frame.function==0x816B7D98;
      if(node && !node->free_entered && node->birth.task==actual_task &&
         frame.thread==d->frame.thread && frame.pcr==d->frame.pcr &&
         d->frame.sp>=(derived?0x70u:0x60u) &&
         frame.sp==d->frame.sp-(derived?0x70u:0x60u) &&
         frame.lr==(derived?0x816B7DC8u:0x816B752Cu)){
        d->free=frame;node->free_entered=true;return true;
      }
    }
    Lose(__LINE__,14);return false;
  }
  bool ReturnTaskFree(Frame frame) {
    for(auto& d:deletions_)if(d && d->free==frame){
      auto* node=FindBirth(d->birth);
      if(lost_ || !node || !node->free_entered || node->free_returned){Lose(__LINE__,27);return false;}
      node->free_returned=true;return true;
    }
    Lose(__LINE__,27);return false;
  }
  bool ReturnDeletingDestructor(Frame frame) {
    for(auto& d:deletions_)if(d && d->frame==frame){
      auto* node=FindBirth(d->birth);
      if(lost_ || !node || !node->free_returned || node->delete_returned){Lose(__LINE__,20);return false;}
      node->delete_returned=true;d.reset();return true;
    }
    Lose(__LINE__,20);return false;
  }
  bool ReturnClose(Frame frame,uint32_t actual_remaining_references) {
    for (auto& value:closes_) { if (value && value->frame==frame) {
      // Original7490 saves its successful atomic decrement in r31. A
      // nonzero caller may return AFTER another Close deleted/freed/reused
      // both the Task address and its metadata slot. Its retained own-frame
      // consumes only this call; it does not grant any native Task lifetime.
      if (actual_remaining_references && !lost_) { value.reset();return true; }
      auto* node=FindBirth(value->birth);
      const bool external=std::any_of(external_.begin(),external_.end(),[&](const auto& work) {
        return work && work->birth.generation==value->birth;
      });
      if (lost_ || !node || node->freed ||
          (actual_remaining_references && node->delete_entered) ||
          (!actual_remaining_references && ((node->started && !node->returned) || node->persistent_active || node->execution_active || external ||
              !node->delete_returned || !node->free_returned))) {
        Lose(__LINE__,19);return false;
      }
      // Original7490 zero-ref path has ALREADY called destructor/free before
      // its own normal return. Nonzero is just a reference decrement.
      if (!actual_remaining_references) {
        for (const auto& modify:modifications_) {
          if (modify && modify->birth==node->birth.generation) { Lose(__LINE__,19);return false; }
        }
        node->freed=true;
      }
      value.reset();return true;
    } }
    Lose(__LINE__,19);return false;
  }
  // Concrete owner holds its ONE metadata mutex while using this local
  // execution-only observer lane. The native body still runs unchanged;
  // failure cannot poison an unrelated typed profile capability/ledger.
  class NeutralObservation final {
   public:
    explicit NeutralObservation(NativeProfileIdentityTasks& ledger) noexcept:ledger_(ledger){++ledger_.neutral_depth_;}
    ~NeutralObservation(){--ledger_.neutral_depth_;}
    NeutralObservation(const NeutralObservation&)=delete;
    NeutralObservation& operator=(const NeutralObservation&)=delete;
   private:NativeProfileIdentityTasks& ledger_;
  };
  uint32_t neutral_depth_=0;
  uint64_t neutral_failures_=0;
  Root root_;
  bool lost_=false;
  uint32_t first_failure_line_=0,first_failure_reason_=0;
  uint64_t next_=1;
  std::array<std::optional<Schedule>,kCapacity> schedules_{};
  std::array<std::optional<Node>,kCapacity> nodes_{};
  std::array<std::optional<DetachedAllocation>,kCapacity> detached_{};
  std::array<std::optional<ExternalWork>,kCapacity> external_{};
  std::array<std::optional<PeriodicDispatch>,kCapacity> periodic_dispatches_{};
  std::array<std::optional<ExecutionDispatch>,kCapacity> execution_dispatches_{};
  std::array<std::optional<ExecutionControl>,kCapacity> execution_controls_{};
  std::array<std::optional<Close>,kCapacity> closes_{};
  std::array<std::optional<Deletion>,kCapacity> deletions_{};
  std::array<std::optional<Modification>,kCapacity> modifications_{};
};
}  // namespace identity_detail
}  // namespace xe::kernel
