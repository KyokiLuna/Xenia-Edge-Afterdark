/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include "xenia/kernel/native_identity_event_origins.h"

namespace xe::kernel {
class NativeProfileIdentityOwner;
class NativeIdentityListenerBridgeTestAccess;
using NativeIdentityEventLedger = notification_detail::NativeIdentityEventOrigins;
// Provenance only. The original native224/Task owner alone constructs these
// AFTER binding its actual earlier input and lifetime. No public ready flag,
// current-user factory, conversion from Origin enum, or consumer-time upgrade.
class NativeIdentityEventSource final {
 private:
  friend class NativeProfileIdentityOwner;
  friend class NativeIdentityListenerBridge;
  friend class NativeIdentityListenerBridgeTestAccess;
  NativeIdentityEventSource(NativeIdentityEventLedger::Root root,
      uint64_t source_serial, uint64_t prepared_owner_key,
      std::shared_ptr<const void> actual_producer_lifetime)
      : root_(root), source_serial_(source_serial),
        prepared_owner_key_(prepared_owner_key),
        lifetime_(std::move(actual_producer_lifetime)) {}
  const NativeIdentityEventLedger::Root root_;
  const uint64_t source_serial_, prepared_owner_key_;
  const std::shared_ptr<const void> lifetime_;
  // One-shot bridge binding, not semantic data or a readiness flag.
  mutable std::atomic<bool> adopted_{false};
};
class NativeIdentityEventWorkSource final {
 private:
  friend class NativeProfileIdentityOwner;
  friend class NativeIdentityListenerBridge;
  friend class NativeIdentityListenerBridgeTestAccess;
  NativeIdentityEventWorkSource(NativeIdentityEventLedger::WorkKey key,
      std::shared_ptr<const NativeIdentityEventSource> earlier_source,
      std::shared_ptr<const void> whole_work_lifetime)
      : key_(key), earlier_(std::move(earlier_source)),
        lifetime_(std::move(whole_work_lifetime)) {}
  const NativeIdentityEventLedger::WorkKey key_;
  const std::shared_ptr<const NativeIdentityEventSource> earlier_;
  const std::shared_ptr<const void> lifetime_;
  mutable std::atomic<bool> adopted_{false};
};

// Specific original17559 listener bridge. All slow calls are made by the
// authenticated concrete owner OUTSIDE native locks. It installs a FastCell
// before93C8/8C00 can acquire ANY native lock. Under the native listener-area
// lock the only operations are scalar reads, fixed-array lookup and the
// ledger's lock-free metadata operations. No shared_ptr/mutex/IO/guest mapping
// or Root/current-user lookup occurs in the fast methods.
// This component supplies no provider-ready/pixel-rearm/publication API.
class NativeIdentityListenerBridge final {
 public:
  using Ledger = NativeIdentityEventLedger;
  using Root = Ledger::Root;
  using Origin = Ledger::Origin;
  using Listener = Ledger::Listener;
  using Work = Ledger::Work;
  using WorkKey = Ledger::WorkKey;
  static constexpr size_t kAreas = 16, kCells = 64;
  struct Registers {
    uintptr_t context = 0, thread_state = 0, kernel = 0;
    uint64_t lr = 0;
    std::array<uint64_t,32> r{};
  };
  // Values copied from actual initialized area headers before8498 publishes
  // the object. The concrete owner retains the actual allocation/root/domain;
  // retaining a native OBJECT REF across88E8 would create a destructor cycle.
  struct Area { uint32_t address = 0, minimum = 0, maximum = 0; };
  struct CapturedAreas {
    std::array<Area,kAreas> areas{};
    size_t count=0;
  };
  class Frame final {
   private:
    friend class NativeProfileIdentityOwner;
    friend class NativeIdentityListenerBridge;
    friend class NativeIdentityListenerBridgeTestAccess;
    Frame(Root root, uintptr_t context, uintptr_t thread_state,
        uintptr_t kernel, uint32_t thread, uint32_t pcr, uint32_t sp,
        uint32_t lr, uint32_t function, std::shared_ptr<const void> lifetime)
        : root_(root), context_(context), thread_state_(thread_state),
          kernel_(kernel), thread_(thread), pcr_(pcr), sp_(sp), lr_(lr),
          function_(function), lifetime_(std::move(lifetime)) {}
    const Root root_;
    const uintptr_t context_, thread_state_, kernel_;
    const uint32_t thread_, pcr_, sp_, lr_, function_;
    const std::shared_ptr<const void> lifetime_;
  };
  class FastCell final {
   private:
    friend class NativeIdentityListenerBridge;
    friend class NativeProfileIdentityOwner;
    friend class NativeIdentityListenerBridgeTestAccess;
    enum class Kind { kBroadcast, kGetNext };
    NativeIdentityListenerBridge* bridge_ = nullptr;
    std::shared_ptr<const Frame> frame_;
    std::atomic<void*>* thread_cell_ = nullptr;
    FastCell* previous_ = nullptr;
    Kind kind_ = Kind::kBroadcast;
    std::optional<Origin> origin_;
    std::optional<Work> work_;
    uint32_t argument_ = 0,handle_ = 0;
    Listener referenced_listener_;
    bool reference_returned_=false,reference_taken_=false;
    bool dereference_entered_=false,dereference_returned_=false;
    uint32_t id_output_ = 0, data_output_ = 0;
    const uint8_t* selected_id_ = nullptr;
    const uint8_t* actual_data_ = nullptr;
    uint32_t inner_ = 0, inner_sp_ = 0, area_ = 0;
    Listener listener_;
    bool linearized_ = false;
    // Copies of existing actual-thread scalars only; no extra native read.
    uint64_t diagnostic_incoming_id_=0;
    uint32_t diagnostic_inner_function_=0,diagnostic_selected_id_=0;
    bool diagnostic_selected_available_=false;
    std::atomic<bool> active_{false};
  };
  struct Failure {
    uint32_t line=0,reason=0,available=0,checks=0;
    uintptr_t context=0,thread_state=0,kernel=0,candidate=0;
    uint64_t lr=0,sp=0,r3=0,r4=0,r5=0,r6=0,r13=0,r25=0,r28=0,r29=0,r30=0,r31=0;
    uint64_t ledger_at_capture=0,detail0=0,detail1=0,detail2=0,incoming_id=0;
    uint32_t frame_function=0,frame_thread=0,frame_pcr=0,frame_sp=0,frame_lr=0;
    uint32_t kind=0,inner=0,inner_sp=0,area=0,argument=0,handle=0,linearized=0;
    uint32_t listener_object=0,selected_id=0,reference_flags=0;
    uint64_t listener_serial=0,origin=0,work=0;
  };
  struct FailureSnapshot { uint32_t state=0;uint64_t ledger=0;Failure data; };
  FailureSnapshot FirstFailure() const noexcept {
    FailureSnapshot result;
    result.state=first_failure_state_.load(std::memory_order_acquire);
    if(result.state==2)result.data=first_failure_;
    result.ledger=ledger_.FirstFailure();return result;
  }
  struct Dependencies {
    std::shared_ptr<const NativeIdentityEventSource> entry;
    std::array<std::shared_ptr<const NativeIdentityEventSource>,Ledger::kOriginsPerWork> dequeued{};
    uint32_t count = 0;
    bool unknown = true;
  };
  // Moved under the metadata mutex, destroyed only by the host owner after
  // unlocking AND outside guest/GCRC context. Never run reaping in HLE hooks.
  struct Retired {
    std::array<std::shared_ptr<const void>,Ledger::kOrigins+Ledger::kWorks+kCells+Ledger::kListeners*5> items{};
    size_t count = 0;
  };
 private:
  friend class NativeProfileIdentityOwner;
  friend class NativeIdentityListenerBridgeTestAccess;
  explicit NativeIdentityListenerBridge(Root root) : root_(root), ledger_(root) {}
  static bool Address(uint32_t a) { return a >= 0x1000 && !(a & 3); }
  // Xenon DWORD/address arguments may be zero- or sign-extended in a GPR.
  // Arbitrary upper bits never alias an accepted native32 value.
  static bool DwordIs(uint64_t actual,uint32_t value) noexcept {
    return actual==uint64_t(value) || actual==uint64_t(int64_t(int32_t(value)));
  }
  static bool Dword(uint64_t actual) noexcept {return DwordIs(actual,uint32_t(actual));}
  // Native computed word operands are consumed by cmplw/word indexing, not
  // by the canonical pointer/ABI checks above. Wrapping rlwinm can retain a
  // different upper word; never rewrite the guest GPR or derive an address.
  static uint32_t WordOperand(uint64_t actual) noexcept {return uint32_t(actual);}
  struct OuterArguments {
    uint32_t manager=0,object=0,handle=0,event=0,data=0,filter=0,id_output=0,data_output=0;
  };
  static std::optional<OuterArguments> DecodeOuter(uint32_t function,const Registers& r) noexcept {
    OuterArguments a;
    if(function==0x816A8498){
      if(!DwordIs(r.r[3],0x81A821FCu) || !Dword(r.r[4]) ||
         !Address(uint32_t(r.r[4])) || uint32_t(r.r[4])>UINT32_MAX-0x30)return {};
      a.manager=uint32_t(r.r[3]);a.object=uint32_t(r.r[4]);
    }else if(function==0x816A88E8){
      if(!Dword(r.r[3]) || !Address(uint32_t(r.r[3])) || uint32_t(r.r[3])>UINT32_MAX-0x30)return {};
      a.object=uint32_t(r.r[3]);
    }else if(function==0x816A93C8){
      if(!Dword(r.r[3]) || !Dword(r.r[4]))return {};
      a.event=uint32_t(r.r[3]);a.data=uint32_t(r.r[4]);
    }else if(function==0x816A8C00){
      if(!Dword(r.r[3]) || !Dword(r.r[4]) || !Dword(r.r[5]) || !Dword(r.r[6]))return {};
      a.handle=uint32_t(r.r[3]);a.filter=uint32_t(r.r[4]);
      a.id_output=uint32_t(r.r[5]);a.data_output=uint32_t(r.r[6]);
      if((a.id_output && (!Address(a.id_output) || a.id_output>UINT32_MAX-3)) ||
         (a.data_output && (!Address(a.data_output) || a.data_output>UINT32_MAX-3)))return {};
    }else return {};
    return a;
  }

  // Real8498 pre-publication input, not a guessed stride/count array. Read
  // every header word through the owner's checked native mapping before any
  // registry/area lock. The real allocation receipt remains a separate input.
  template<class ReadWord> static std::optional<CapturedAreas> CaptureAreas(
      uint32_t object,ReadWord&& word) {
    if (!Address(object) || object>UINT32_MAX-0x30) return {};
    const auto first=word(object+0x24);if (!first) return {};
    CapturedAreas out;uint32_t next=*first;
    while (next) {
      if (!Address(next) || next>UINT32_MAX-0x24 || out.count==kAreas) return {};
      for (size_t i=0;i<out.count;++i) if (out.areas[i].address==next) return {};
      std::array<uint32_t,9> header{};
      for (size_t i=0;i<header.size();++i) {
        const auto value=word(next+uint32_t(4*i));if (!value) return {};
        header[i]=*value;
      }
      if (header[0]!=object || header[1]>header[2]) return {};
      out.areas[out.count++]={next,header[1],header[2]};next=header[8];
    }
    return out;
  }
  bool FrameValid(const Frame& f,uint32_t function) const {
    return f.root_==root_ && f.context_ && f.thread_state_ && f.kernel_ &&
        f.thread_ && Address(f.pcr_) && f.sp_>=0x1000 && !(f.sp_&15) &&
        f.lr_>=4 && !(f.lr_&3) && f.function_==function && f.lifetime_;
  }
  static bool SameFrame(const Frame& a,const Frame& b) {
    return a.root_==b.root_ && a.context_==b.context_ && a.thread_state_==b.thread_state_ &&
        a.kernel_==b.kernel_ && a.thread_==b.thread_ && a.pcr_==b.pcr_ &&
        a.sp_==b.sp_ && a.lr_==b.lr_ && a.function_==b.function_;
  }
  static bool SameThread(const Frame& a,const Frame& b) {
    return a.root_==b.root_ && a.context_==b.context_ && a.thread_state_==b.thread_state_ &&
        a.kernel_==b.kernel_ && a.thread_==b.thread_ && a.pcr_==b.pcr_;
  }
  std::optional<Origin> AdoptSource(std::shared_ptr<const NativeIdentityEventSource> source) {
    std::lock_guard lock(mutex_);
    if (!source || source->root_!=root_ || !source->source_serial_ || source->source_serial_==UINT64_MAX ||
        source->prepared_owner_key_==UINT64_MAX || !source->lifetime_ || lost()) return {};
    for (size_t i=0;i<sources_.size();++i) if (sources_[i]) {
      if (sources_[i]==source) return origins_[i];
      if (sources_[i]->source_serial_==source->source_serial_) { Lose(__LINE__,1);return {}; }
    }
    bool unadopted=false;
    if (!source->adopted_.compare_exchange_strong(unadopted,true)) { Lose(__LINE__,1);return {}; }
    const auto kind=source->prepared_owner_key_ ? Ledger::Provenance::kPreparedNativeRequest :
                                               Ledger::Provenance::kUnjoinedNativeRequest;
    auto origin=ledger_.BeginProducerWithProvenance(kind);
    if (!origin || sources_[origin->slot]) { Lose(__LINE__,1);return {}; }
    sources_[origin->slot]=std::move(source);origins_[origin->slot]=*origin;
    return origin;
  }
  std::optional<Origin> SourceOrigin(const std::shared_ptr<const NativeIdentityEventSource>& source) const {
    if (!source) return {};
    for (size_t i=0;i<sources_.size();++i) if (sources_[i]==source) return origins_[i];
    return {};
  }
  std::shared_ptr<const NativeIdentityEventSource> RetainEarlierSource(uint64_t serial) {
    std::lock_guard lock(mutex_);
    for (const auto& source:sources_) if (source && source->source_serial_==serial) return source;
    return {};
  }
  // Scalar refcount join only. Exactly the producer reference remains: all
  // queued/coalesced event and actual whole-work dependencies have drained.
  // This never proves provider output, native Task, renderer or pixels alone.
  bool SourceOnlyProducer(Origin origin) {
    std::lock_guard lock(mutex_);
    auto* value=ledger_.Find(origin);
    return !lost() && SourceFor(origin) && value &&
        value->producer.load(std::memory_order_acquire) &&
        value->references.load(std::memory_order_acquire)==1;
  }
  bool FinishSourceAfterDescendants(Origin origin) {
    std::lock_guard lock(mutex_);
    if (!SourceFor(origin)) { Lose(__LINE__,2);return false; }
    for (const auto& cell:cells_) if (cell.active_.load() && cell.origin_==origin) { Lose(__LINE__,2);return false; }
    return ledger_.FinishProducerNormally(origin);
  }
  std::optional<Work> AdoptWholeWork(std::shared_ptr<const NativeIdentityEventWorkSource> source) {
    std::lock_guard lock(mutex_);
    if (!source || source->key_.root!=root_ || !source->key_.valid() || !source->lifetime_ || lost()) return {};
    bool unadopted=false;
    if (!source->adopted_.compare_exchange_strong(unadopted,true)) { Lose(__LINE__,3);return {}; }
    auto origin=SourceOrigin(source->earlier_);
    if (source->earlier_ && !origin) { Lose(__LINE__,3);return {}; }
    auto work=ledger_.BeginWork(source->key_,origin);
    if (!work) { Lose(__LINE__,3);return {}; }
    if (works_[work->slot].source && (!works_[work->slot].finished ||
        works_[work->slot].cells.load() || !RetireResource(works_[work->slot].source))) { Lose(__LINE__,3);return {}; }
    works_[work->slot].finished=false;
    works_[work->slot].source=std::move(source);works_[work->slot].id=*work;
    return work;
  }
  bool FinishWholeWorkNormally(Work work,WorkKey actual_return) {
    std::lock_guard lock(mutex_);
    auto* row=WorkFor(work);
    if (!row || row->cells.load() || !ledger_.FinishWorkNormally(work,actual_return)) { Lose(__LINE__,4);return false; }
    row->finished=true;return true;
  }
  std::optional<Dependencies> CaptureDependencies(Work work) {
    std::lock_guard lock(mutex_);
    auto facts=ledger_.Facts(work);auto* row=WorkFor(work);
    if (!facts || !row || row->finished) return {};
    Dependencies result;result.unknown=facts->unknown_input;result.count=facts->count;
    if (facts->entry_origin) result.entry=SourceForSerial(facts->entry_origin);
    if (facts->entry_origin && !result.entry) { Lose(__LINE__,5);return {}; }
    for (uint32_t i=0;i<facts->count;++i) {
      result.dequeued[i]=SourceForSerial(facts->dequeued_origins[i]);
      if (!result.dequeued[i]) { Lose(__LINE__,5);return {}; }
    }
    return result;
  }
  // Before8498 native registry insertion. No registration is inferred from
  // dequeue/address equality. The external holder retains actual native root,
  // domain and allocation provenance without adding a counted object ref.
  std::optional<Listener> BeforeRegistration(std::shared_ptr<const Frame> frame,
      uint64_t registry,uint64_t object,std::span<const Area> areas,
      std::shared_ptr<const void> actual_allocation_lifetime) {
    if (!frame || !FrameValid(*frame,0x816A8498) || !DwordIs(registry,0x81A821FCu) ||
        !Dword(object) || uint32_t(object)>UINT32_MAX-0x30 || !Address(uint32_t(object)) ||
        areas.empty() || areas.size()>kAreas || !actual_allocation_lifetime) { Lose(__LINE__,6,nullptr,nullptr,frame.get());return {}; }
    std::lock_guard lock(mutex_);
    for (size_t i=0;i<areas.size();++i) {
      if (!Address(areas[i].address) || areas[i].address>UINT32_MAX-0x24 || areas[i].minimum>areas[i].maximum) { Lose(__LINE__,6,nullptr,nullptr,frame.get());return {}; }
      for (size_t j=0;j<i;++j) if (areas[i].address==areas[j].address) { Lose(__LINE__,6,nullptr,nullptr,frame.get());return {}; }
      if (FindArea(areas[i].address)) { Lose(__LINE__,6,nullptr,nullptr,frame.get());return {}; }
    }
    auto listener=ledger_.RegisterListener(uint32_t(object));
    if (!listener) return {};
    auto& row=listeners_[listener->slot];
    // A retired generation's native resources await host reaping; do not
    // destroy them in a guest hook when the address/ledger slot is recycled.
    if (row.lifetime && !RetireResource(row.lifetime)) { Lose(__LINE__,6,nullptr,nullptr,frame.get());return {}; }
    if (row.registration && !RetireResource(row.registration)) { Lose(__LINE__,6,nullptr,nullptr,frame.get());return {}; }
    if (row.destruction && !RetireResource(row.destruction)) { Lose(__LINE__,6,nullptr,nullptr,frame.get());return {}; }
    row.active.store(false,std::memory_order_release);
    row.lifetime=std::move(actual_allocation_lifetime);row.registration=std::move(frame);
    row.registered=false;row.retired=false;row.id=*listener;row.area_count.store(uint32_t(areas.size()),std::memory_order_relaxed);
    for (size_t i=0;i<areas.size();++i) {
      row.areas[i].minimum.store(areas[i].minimum);row.areas[i].maximum.store(areas[i].maximum);
      row.areas[i].address.store(areas[i].address);
    }
    row.object.store(listener->object,std::memory_order_relaxed);
    row.serial.store(listener->serial,std::memory_order_release);
    row.active.store(true,std::memory_order_release);return listener;
  }
  bool RegistrationReturned(Listener listener,const Frame& actual_return) {
    std::lock_guard lock(mutex_);auto* row=ListenerFor(listener);
    if (!row || !row->registration || !SameFrame(*row->registration,actual_return)) { Lose(__LINE__,7,nullptr,nullptr,&actual_return);return false; }
    row->registered=true;
    if (!RetireResource(row->registration)) { Lose(__LINE__,7,nullptr,nullptr,&actual_return);return false; }
    return true;
  }
  bool BeforeDestruction(std::shared_ptr<const Frame> frame,uint64_t object) {
    if (!frame || !FrameValid(*frame,0x816A88E8) || !Dword(object) || uint32_t(object)>UINT32_MAX-0x30 ||
        !Address(uint32_t(object))) { Lose(__LINE__,8,nullptr,nullptr,frame.get());return false; }
    std::lock_guard lock(mutex_);auto id=ledger_.FindListener(uint32_t(object));
    auto* row=id ? ListenerFor(*id) : nullptr;
    if (!row || !row->registered || row->destruction) { Lose(__LINE__,8,nullptr,nullptr,frame.get());return false; }
    row->destruction=std::move(frame);return true; // still fully active
  }
  bool DestructionReturned(uint32_t object,const Frame& actual_return) {
    std::lock_guard lock(mutex_);auto id=ledger_.FindListener(uint32_t(object));
    auto* row=id ? ListenerFor(*id) : nullptr;
    if (!row || !row->destruction || !SameFrame(*row->destruction,actual_return)) { Lose(__LINE__,9,nullptr,nullptr,&actual_return);return false; }
    if (!ledger_.RetireListenerAfterNativeDestruction(*id)) return false;
    row->active.store(false,std::memory_order_release);row->retired=true;return true;
  }
  FastCell* BeforeBroadcast(std::shared_ptr<const Frame> frame,
      std::shared_ptr<const NativeIdentityEventSource> earlier,uint64_t actual_data,
      std::atomic<void*>& actual_thread_cell) {
    std::lock_guard lock(mutex_);
    if (!frame || !FrameValid(*frame,0x816A93C8) || !Dword(actual_data)) { Lose(__LINE__,10,nullptr,nullptr,frame.get());return nullptr; }
    const auto origin=SourceOrigin(earlier);
    if (earlier && !origin) { Lose(__LINE__,10,nullptr,nullptr,frame.get());return nullptr; }
    return OpenCell(std::move(frame),FastCell::Kind::kBroadcast,origin,{},uint32_t(actual_data),
                    0,0,nullptr,nullptr,actual_thread_cell);
  }
  FastCell* BeforeGetNext(std::shared_ptr<const Frame> frame,std::optional<Work> earlier_whole_work,
      uint64_t actual_handle,uint64_t actual_filter,uint64_t id_output,uint64_t data_output,
      const uint8_t* prebound_future_selected_id,const uint8_t* prebound_actual_data,
      std::atomic<void*>& actual_thread_cell) {
    std::lock_guard lock(mutex_);
    if (!frame || !FrameValid(*frame,0x816A8C00) || frame->sp_<0x140 ||
        !Dword(actual_handle) || !Dword(actual_filter) || !Dword(id_output) || !Dword(data_output) ||
        (id_output && (!Address(uint32_t(id_output)) || uint32_t(id_output)>UINT32_MAX-3)) ||
        !prebound_future_selected_id ||
        (data_output && (!Address(uint32_t(data_output)) || uint32_t(data_output)>UINT32_MAX-3 || !prebound_actual_data))) { Lose(__LINE__,11,nullptr,nullptr,frame.get());return nullptr; }
    if (earlier_whole_work && !WorkFor(*earlier_whole_work)) { Lose(__LINE__,11,nullptr,nullptr,frame.get());return nullptr; }
    auto* cell=OpenCell(std::move(frame),FastCell::Kind::kGetNext,{},earlier_whole_work,
        uint32_t(actual_filter),uint32_t(id_output),uint32_t(data_output),prebound_future_selected_id,
        data_output?prebound_actual_data:nullptr,actual_thread_cell);
    if(cell)cell->handle_=uint32_t(actual_handle);return cell;
  }
  // The real Object Manager primitive has already returned outside GCRC.
  // Handle->body comes ONLY from its actual counted-success branch, never
  // from a handle-shaped address or a guest-writable output word.
  bool ReferenceReturned(FastCell* cell,uint32_t handle,uint32_t status,uint32_t body) {
    std::lock_guard lock(mutex_);
    if(!KnownCell(cell) || cell->kind_!=FastCell::Kind::kGetNext || cell->inner_ ||
       cell->reference_returned_ || cell->handle_!=handle || lost()){Lose(__LINE__,12,nullptr,nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),handle,status,body);return false;}
    if(int32_t(status)<0){
      if(body){Lose(__LINE__,12,nullptr,nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),handle,status,body);return false;}
      cell->reference_returned_=true;return true;
    }
    const auto id=ledger_.FindListener(body);auto* row=id?ListenerFor(*id):nullptr;
    if(status || !row || !row->registered || row->destruction){Lose(__LINE__,12,nullptr,nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),handle,status,body);return false;}
    cell->referenced_listener_=*id;cell->reference_taken_=true;
    cell->reference_returned_=true;return true;
  }
  bool BeforeReferenceRelease(FastCell* cell,uint32_t body) {
    std::lock_guard lock(mutex_);
    if(!KnownCell(cell) || cell->kind_!=FastCell::Kind::kGetNext || cell->inner_ ||
       !cell->reference_returned_ || !cell->reference_taken_ ||
       cell->referenced_listener_.object!=body || cell->dereference_entered_ || lost()){
      Lose(__LINE__,13,nullptr,nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),body);return false;
    }
    cell->dereference_entered_=true;return true;
  }
  bool ReferenceReleaseReturned(FastCell* cell,uint32_t body) {
    std::lock_guard lock(mutex_);
    if(!KnownCell(cell) || !cell->reference_taken_ || cell->referenced_listener_.object!=body ||
       !cell->dereference_entered_ || cell->dereference_returned_ || cell->inner_ || lost()){
      Lose(__LINE__,14,nullptr,nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),body);return false;
    }
    // Native final deref may already have genuinely destroyed this listener.
    // Its retained serial stays historical; no active-row lookup or remint.
    cell->dereference_returned_=true;return true;
  }
  bool AllowsReferenceReleaseDestruction(void* candidate,const Registers& regs,uint32_t body) {
    std::lock_guard lock(mutex_);
    FastCell* cell=nullptr;for(auto& known:cells_)if(&known==candidate){cell=&known;break;}
    return KnownCell(cell) && cell->kind_==FastCell::Kind::kGetNext &&
        FastContext(*cell,regs) && cell->reference_taken_ && cell->dereference_entered_ &&
        !cell->dereference_returned_ && !cell->inner_ && cell->referenced_listener_.object==body;
  }
  // Owner delegates from the ThreadState's prebound cell BEFORE any ordinary
  // owner lookup. These methods never touch shared_ptr/host mutex/guest maps.
  static bool FastEnter(FastCell* cell,const Registers& regs,uint32_t function) noexcept {
    if (!cell || !cell->bridge_) return false;
    auto& self=*cell->bridge_;
    const bool context_current=self.FastContext(*cell,regs);
    if (!context_current || cell->inner_ || self.lost()) { self.Lose(__LINE__,15,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),function);return false; }
    const bool enqueue=function==0x816A8328;
    if ((!enqueue && function!=0x816A8648) ||
        (enqueue ? cell->kind_!=FastCell::Kind::kBroadcast || !DwordIs(regs.lr,0x816A8478u) ||
                       !DwordIs(regs.r[1],cell->frame_->sp_-0xF0) || !DwordIs(regs.r[5],cell->argument_)
                 : cell->kind_!=FastCell::Kind::kGetNext || !DwordIs(regs.lr,0x816A8C94u) ||
                       !DwordIs(regs.r[1],cell->frame_->sp_-0x90) ||
                       !DwordIs(regs.r[4],cell->id_output_) || !DwordIs(regs.r[5],cell->data_output_) ||
                       !DwordIs(regs.r[6],cell->argument_) || !DwordIs(regs.r[7],cell->argument_))) { self.Lose(__LINE__,15,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),function);return false; }
    if (!Dword(regs.r[3])) { self.Lose(__LINE__,15,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),function);return false; }
    auto area=self.FindArea(uint32_t(regs.r[3]));
    if (!area || (!enqueue && (!cell->reference_returned_ || !cell->reference_taken_ ||
        cell->dereference_entered_ || cell->referenced_listener_!=area->first))) { self.Lose(__LINE__,15,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),function);return false; }
    cell->diagnostic_inner_function_=function;cell->diagnostic_incoming_id_=regs.r[4];
    cell->diagnostic_selected_id_=0;cell->diagnostic_selected_available_=false;
    cell->inner_=function;cell->inner_sp_=uint32_t(regs.r[1]);cell->area_=uint32_t(regs.r[3]);
    cell->listener_=area->first;cell->linearized_=false;return true;
  }
  static bool FastBeforeUnlock(FastCell* cell,const Registers& regs) noexcept {
    if (!cell || !cell->bridge_) return false;
    // The same HLE handles unrelated native locks: only these exact area
    // release callsites are ours. The actual thunk is xboxkrnl:B4,81A72B44.
    if (!DwordIs(regs.lr,0x816A8410u) && !DwordIs(regs.lr,0x816A8874u)) return false;
    auto& self=*cell->bridge_;
    const bool context_current=self.FastContext(*cell,regs);
    if (!context_current || !cell->inner_ || cell->linearized_ || self.lost() ||
        !DwordIs(regs.r[3],cell->listener_.object+0x1C)) { self.Lose(__LINE__,16,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),uint32_t(regs.lr));return false; }
    auto area=self.FindArea(cell->area_);
    if (!area || area->first!=cell->listener_) { self.Lose(__LINE__,16,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),uint32_t(regs.lr));return false; }
    if (uint32_t(regs.lr)==0x816A8410) {
      const uint32_t normalized_id=WordOperand(regs.r[29]);
      if (cell->inner_!=0x816A8328 || !DwordIs(regs.r[1],cell->inner_sp_-0x80) ||
          !DwordIs(regs.r[31],cell->area_) || !DwordIs(regs.r[28],cell->argument_) ||
          normalized_id<area->second.minimum || normalized_id>area->second.maximum) { self.Lose(__LINE__,16,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),uint32_t(regs.lr));return false; }
      cell->linearized_=self.ledger_.ReplaceQueued(cell->listener_,cell->area_,
          normalized_id,uint32_t(regs.r[28]),cell->origin_);
    } else {
      if (cell->inner_!=0x816A8648 || !DwordIs(regs.r[1],cell->inner_sp_-0xB0) ||
          !DwordIs(regs.r[30],cell->area_) || !DwordIs(regs.r[22],cell->id_output_) ||
          !DwordIs(regs.r[23],cell->data_output_) || !Dword(regs.r[25])) { self.Lose(__LINE__,16,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),uint32_t(regs.lr));return false; }
      // Native success only. Its ID has not yet undergone the later global
      // inverse mapping at88A0. Read only mappings retained before8C00 body.
      if (regs.r[25]==0) {
        const uint32_t selected=ReadBe(cell->selected_id_);
        cell->diagnostic_selected_id_=selected;cell->diagnostic_selected_available_=true;
        if (selected<area->second.minimum || selected>area->second.maximum) { self.Lose(__LINE__,16,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),uint32_t(regs.lr));return false; }
        self.ledger_.TransferSuccessfulDequeue(cell->listener_,cell->area_,selected,
            cell->actual_data_ ? std::optional<uint32_t>(ReadBe(cell->actual_data_)) : std::nullopt,
            cell->work_);
      }
      cell->linearized_=true; // genuine native failure consumed no queue node
    }
    const bool healthy=!self.lost();
    if(!healthy)self.RecordFailure(__LINE__,16,&regs,cell,nullptr,reinterpret_cast<uintptr_t>(cell),uint32_t(regs.lr));
    return healthy;
  }
  static bool FastReturn(FastCell* cell,const Registers& regs,uint32_t function) noexcept {
    if (!cell || !cell->bridge_) return false;
    auto& self=*cell->bridge_;
    const uint32_t lr=function==0x816A8328?0x816A8478:0x816A8C94;
    const bool context_current=self.FastContext(*cell,regs);
    if (!context_current || cell->inner_!=function ||
        (function!=0x816A8328 && function!=0x816A8648) ||
        !DwordIs(regs.r[1],cell->inner_sp_) || !DwordIs(regs.lr,lr) || !Dword(regs.r[3])) { self.Lose(__LINE__,17,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),function);return false; }
    // Native failures can skip the lock. Success without the observed
    // pre-unlock seam is ambiguous (8328 also has a high-bit early-success
    // branch). Preserve resources and refuse provenance, never assume no write.
    if (regs.r[3]==0 && !cell->linearized_) { self.Lose(__LINE__,17,&regs,context_current?cell:nullptr,nullptr,reinterpret_cast<uintptr_t>(cell),function);return false; }
    cell->diagnostic_inner_function_=0;cell->diagnostic_incoming_id_=0;
    cell->diagnostic_selected_id_=0;cell->diagnostic_selected_available_=false;
    cell->inner_=0;cell->area_=0;cell->linearized_=false;return !self.lost();
  }
  // The exact outer Work retains this same shared Frame until its normal
  // host retirement. Value-equal frames or another control block cannot
  // complete a lease, including a later call at the same native SP/LR.
  bool OuterReturned(FastCell* cell,
      const std::shared_ptr<const Frame>& retained_return,uint32_t native_result) {
    std::lock_guard lock(mutex_);
    // Original short-circuit order, each predicate evaluated exactly once.
    const bool frame_present=cell && cell->bridge_==this && cell->active_.load() && cell->frame_;
    const bool inner_clear=frame_present && !cell->inner_;
    const bool same_owner=inner_clear && retained_return &&
        &retained_return!=&cell->frame_ && cell->frame_.get()==retained_return.get() &&
        !cell->frame_.owner_before(retained_return) && !retained_return.owner_before(cell->frame_);
    const bool frame_matches=same_owner && SameFrame(*cell->frame_,*retained_return);
    const bool healthy=frame_matches && !lost();
    const uint32_t checks=uint32_t(frame_present) | (uint32_t(inner_clear)<<1) |
        (uint32_t(frame_matches)<<2) | (uint32_t(healthy)<<3);
    if (!healthy) { Lose(__LINE__,18,nullptr,frame_matches?cell:nullptr,retained_return.get(),
        reinterpret_cast<uintptr_t>(cell),native_result,0,0,checks);return false; }
    if(cell->kind_==FastCell::Kind::kGetNext &&
       (!cell->reference_returned_ || native_result>1 ||
        (cell->reference_taken_ ? !cell->dereference_returned_ : native_result!=0))){Lose(__LINE__,18,nullptr,cell,retained_return.get(),reinterpret_cast<uintptr_t>(cell),native_result,0,0,checks);return false;}
    if (cell->previous_ && (!cell->previous_->active_.load() ||
        !SameThread(*cell->previous_->frame_,*retained_return))) { Lose(__LINE__,18,nullptr,cell,retained_return.get(),reinterpret_cast<uintptr_t>(cell),native_result,0,0,checks);return false; }
    void* expected=cell;
    if (!cell->thread_cell_->compare_exchange_strong(expected,cell->previous_)) { Lose(__LINE__,18,nullptr,cell,retained_return.get(),reinterpret_cast<uintptr_t>(cell),native_result,0,reinterpret_cast<uintptr_t>(expected),checks);return false; }
    if (cell->work_) {
      auto* row=WorkFor(*cell->work_);
      if (!row || !row->cells.load()) { Lose(__LINE__,18,nullptr,cell,retained_return.get(),reinterpret_cast<uintptr_t>(cell),native_result,0,0,checks);return false; }
      row->cells.fetch_sub(1);
    }
    cell->active_.store(false,std::memory_order_release);
    // The genuine outer/inner/ref lifetime ended and the exact ThreadState
    // binding was withdrawn. The retained Work still owns this same control
    // block, so dropping only our duplicate cannot destroy Frame/lifetime
    // under the guest/GCRC/owner locks. No owning resource lives in the
    // remaining scalar slot, which may now serve another actual invocation.
    cell->frame_.reset();cell->bridge_=nullptr;cell->thread_cell_=nullptr;
    cell->previous_=nullptr;cell->origin_.reset();cell->work_.reset();
    cell->argument_=0;cell->handle_=0;cell->referenced_listener_={};
    cell->reference_returned_=false;cell->reference_taken_=false;
    cell->dereference_entered_=false;cell->dereference_returned_=false;
    cell->id_output_=0;cell->data_output_=0;
    cell->selected_id_=nullptr;cell->actual_data_=nullptr;
    cell->inner_=0;cell->inner_sp_=0;cell->area_=0;cell->listener_={};cell->linearized_=false;
    cell->diagnostic_incoming_id_=0;cell->diagnostic_inner_function_=0;
    cell->diagnostic_selected_id_=0;cell->diagnostic_selected_available_=false;
    return true;
  }
  Retired ReapOnHostOutsideGuestLocks() {
    Retired result;
    std::lock_guard lock(mutex_);
    if (lost()) return result; // uncertain native execution retains everything
    auto take=[&](auto& value) {
      if (value) result.items[result.count++]=std::move(value);
    };
    for (auto& value:retired_) take(value);
    std::array<Origin,Ledger::kOrigins> completed{};
    const size_t count=ledger_.TakeCompletedOrigins(completed);
    for (size_t i=0;i<count;++i) {
      const auto id=completed[i];
      if (id.slot>=sources_.size() || origins_[id.slot]!=id || !sources_[id.slot]) { Lose(__LINE__,19);return result; }
      take(sources_[id.slot]);origins_[id.slot]={};
    }
    for (auto& work:works_) if (work.finished && !work.cells.load()) {
      take(work.source);work.finished=false;work.id={};
    }
    for (auto& cell:cells_) if (!cell.active_.load() && cell.frame_) {
      take(cell.frame_);cell.bridge_=nullptr;cell.thread_cell_=nullptr;
      cell.origin_.reset();cell.work_.reset();cell.selected_id_=nullptr;cell.actual_data_=nullptr;
    }
    for (auto& listener:listeners_) if (listener.retired) {
      take(listener.registration);take(listener.destruction);take(listener.lifetime);
      listener.retired=false;
    }
    return result;
  }
  void CloseNewSources() { stopping_.store(true,std::memory_order_release);ledger_.CloseNewProducers(); }
  // Private actual-stop seam only. The owner has already joined every guest
  // CPU/fiber and graph consumer; a timeout or metadata poison is insufficient.
  // Leave all owning resources in this object for out-of-lock host destruction.
  bool DetachCellsAtActualKernelStop() {
    std::lock_guard lock(mutex_);
    for (auto& cell:cells_) if (cell.thread_cell_) {
      void* expected=&cell;
      if (!cell.thread_cell_->compare_exchange_strong(expected,nullptr) && expected) return false;
      cell.active_.store(false,std::memory_order_release);
    }
    return true;
  }
  // Capture adds no lock/allocation/IO/native read/lookup or authority.
  // current_cell is nonnull ONLY after the original actual-context/frame proof.
  void Lose(uint32_t line=0,uint32_t reason=0,const Registers* regs=nullptr,
      const FastCell* current_cell=nullptr,const Frame* frame=nullptr,
      uintptr_t candidate=0,uint64_t detail0=0,uint64_t detail1=0,uint64_t detail2=0,
      uint32_t checks=0) noexcept {
    // Finish the first tuple before lost becomes visible to another outer
    // observer. Its ledger_at_capture may be zero; the ledger latch is separate.
    RecordFailure(line,reason,regs,current_cell,frame,candidate,detail0,detail1,detail2,checks);
    ledger_.Lose(line,12);
  }
  void RecordFailure(uint32_t line,uint32_t reason,const Registers* regs=nullptr,
      const FastCell* current_cell=nullptr,const Frame* frame=nullptr,
      uintptr_t candidate=0,uint64_t detail0=0,uint64_t detail1=0,uint64_t detail2=0,
      uint32_t checks=0) noexcept {
    uint32_t empty=0;
    if(!first_failure_state_.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
    Failure d;d.line=line;d.reason=reason;d.candidate=candidate;d.detail0=detail0;
    d.detail1=detail1;d.detail2=detail2;d.checks=checks;d.ledger_at_capture=ledger_.FirstFailure();
    if(regs){
      d.available|=1;d.context=regs->context;d.thread_state=regs->thread_state;d.kernel=regs->kernel;
      d.lr=regs->lr;d.sp=regs->r[1];d.r3=regs->r[3];d.r4=regs->r[4];d.r5=regs->r[5];d.r6=regs->r[6];
      d.r13=regs->r[13];d.r25=regs->r[25];d.r28=regs->r[28];d.r29=regs->r[29];d.r30=regs->r[30];d.r31=regs->r[31];
    }
    if(current_cell){
      d.available|=4;d.kind=uint32_t(current_cell->kind_);d.inner=current_cell->inner_;
      d.inner_sp=current_cell->inner_sp_;d.area=current_cell->area_;d.argument=current_cell->argument_;
      d.handle=current_cell->handle_;d.linearized=current_cell->linearized_;
      d.listener_object=current_cell->listener_.object;d.listener_serial=current_cell->listener_.serial;
      d.origin=current_cell->origin_?current_cell->origin_->serial:0;d.work=current_cell->work_?current_cell->work_->serial:0;
      d.reference_flags=uint32_t(current_cell->reference_returned_) | (uint32_t(current_cell->reference_taken_)<<1) |
          (uint32_t(current_cell->dereference_entered_)<<2) | (uint32_t(current_cell->dereference_returned_)<<3);
      if(current_cell->diagnostic_inner_function_==0x816A8328){d.available|=8;d.incoming_id=current_cell->diagnostic_incoming_id_;}
      if(current_cell->diagnostic_selected_available_){d.available|=16;d.selected_id=current_cell->diagnostic_selected_id_;}
      if(!frame)frame=current_cell->frame_.get();
    }
    if(frame){
      d.available|=2;d.frame_function=frame->function_;d.frame_thread=frame->thread_;d.frame_pcr=frame->pcr_;
      d.frame_sp=frame->sp_;d.frame_lr=frame->lr_;
    }
    first_failure_=d;first_failure_state_.store(2,std::memory_order_release);
  }
  bool lost() const noexcept { return ledger_.lost(); }
  static uint32_t ReadBe(const uint8_t* p) noexcept {
    return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|uint32_t(p[3]);
  }
  bool FastContext(const FastCell& cell,const Registers& regs) const noexcept {
    const auto* f=cell.frame_.get();
    return cell.active_.load(std::memory_order_acquire) && f &&
        cell.thread_cell_->load(std::memory_order_acquire)==&cell &&
        regs.context==f->context_ && regs.thread_state==f->thread_state_ &&
        regs.kernel==f->kernel_ && DwordIs(regs.r[13],f->pcr_);
  }
  struct AtomicArea {
    std::atomic<uint32_t> address{0},minimum{0},maximum{0};
  };
  struct ListenerRow {
    std::atomic<bool> active{false};
    std::atomic<uint32_t> area_count{0},object{0};
    std::atomic<uint64_t> serial{0};
    std::array<AtomicArea,kAreas> areas{};
    Listener id;
    std::shared_ptr<const Frame> registration,destruction;
    std::shared_ptr<const void> lifetime;
    bool registered=false,retired=false;
  };
  struct WorkRow {
    Work id;
    std::shared_ptr<const NativeIdentityEventWorkSource> source;
    std::atomic<uint32_t> cells{0};
    bool finished=false;
  };
  bool KnownCell(const FastCell* cell) const noexcept {
    if(!cell)return false;
    for(const auto& known:cells_)if(&known==cell)return known.active_.load(std::memory_order_acquire) &&
        known.bridge_==this && known.frame_ && known.thread_cell_ &&
        known.thread_cell_->load(std::memory_order_acquire)==cell;
    return false;
  }
  std::shared_ptr<const NativeIdentityEventSource> SourceFor(Origin origin) const {
    return origin.slot<sources_.size() && origins_[origin.slot]==origin ? sources_[origin.slot] : nullptr;
  }
  std::shared_ptr<const NativeIdentityEventSource> SourceForSerial(uint64_t serial) const {
    for (size_t i=0;i<sources_.size();++i) if (origins_[i].serial==serial) return sources_[i];
    return {};
  }
  WorkRow* WorkFor(Work work) {
    return work.slot<works_.size() && works_[work.slot].source && works_[work.slot].id==work &&
        !works_[work.slot].finished ? &works_[work.slot] : nullptr;
  }
  ListenerRow* ListenerFor(Listener id) {
    return id.slot<listeners_.size() && listeners_[id.slot].active.load(std::memory_order_acquire) &&
        listeners_[id.slot].id==id ? &listeners_[id.slot] : nullptr;
  }
  std::optional<std::pair<Listener,Area>> FindArea(uint32_t address) const noexcept {
    for (uint32_t slot=0;slot<listeners_.size();++slot) {
      const auto& row=listeners_[slot];
      if (!row.active.load(std::memory_order_acquire)) continue;
      const uint64_t serial=row.serial.load(std::memory_order_acquire);
      const uint32_t object=row.object.load(std::memory_order_relaxed);
      const uint32_t count=row.area_count.load(std::memory_order_relaxed);
      for (uint32_t i=0;i<count;++i) if (row.areas[i].address.load()==address) {
        const Area area{address,row.areas[i].minimum.load(),row.areas[i].maximum.load()};
        // Native registry/object-reference lifetime excludes concurrent
        // destruction of the actually selected object/area. No host lookup.
        if (row.serial.load(std::memory_order_acquire)!=serial ||
            !row.active.load(std::memory_order_acquire)) return {};
        return std::pair<Listener,Area>(Listener{root_,serial,slot,object},area);
      }
    }
    return {};
  }
  template<class T> bool RetireResource(std::shared_ptr<const T>& resource) {
    for (auto& slot:retired_) if (!slot) { slot=std::move(resource);return true; }
    return false;
  }
  FastCell* OpenCell(std::shared_ptr<const Frame> frame,FastCell::Kind kind,
      std::optional<Origin> origin,std::optional<Work> work,uint32_t argument,
      uint32_t id_output,uint32_t data_output,const uint8_t* selected_id,
      const uint8_t* actual_data,std::atomic<void*>& thread_cell) {
    if (stopping_.load() || lost()) return nullptr;
    // Actual mapped17559 routes do not recursively call these outer owners.
    // Reject collision, retaining the EXACT earlier cell; never overwrite or
    // dereference a foreign/stale void pointer. Owner must perform the same
    // atomic non-null rejection BEFORE its slow root/frame preparation.
    if (thread_cell.load(std::memory_order_acquire)) { Lose(__LINE__,20);return nullptr; }
    WorkRow* row=work?WorkFor(*work):nullptr;
    if (work && (!row || row->source->key_.thread!=frame->thread_ ||
        row->source->key_.pcr!=frame->pcr_ || row->source->key_.sp<frame->sp_)) { Lose(__LINE__,20);return nullptr; }
    for (auto& cell:cells_) {
      if (cell.frame_) continue;
      cell.bridge_=this;cell.frame_=std::move(frame);cell.thread_cell_=&thread_cell;
      cell.previous_=nullptr;cell.kind_=kind;cell.origin_=origin;cell.work_=work;
      cell.argument_=argument;cell.handle_=0;cell.referenced_listener_={};
      cell.reference_returned_=false;cell.reference_taken_=false;
      cell.dereference_entered_=false;cell.dereference_returned_=false;cell.id_output_=id_output;cell.data_output_=data_output;
      cell.selected_id_=selected_id;cell.actual_data_=actual_data;
      cell.inner_=0;cell.area_=0;cell.linearized_=false;
      cell.diagnostic_incoming_id_=0;cell.diagnostic_inner_function_=0;
      cell.diagnostic_selected_id_=0;cell.diagnostic_selected_available_=false;
      cell.active_.store(true,std::memory_order_release);
      void* expected=nullptr;
      if (!thread_cell.compare_exchange_strong(expected,&cell)) { Lose(__LINE__,20);return nullptr; }
      if (row) row->cells.fetch_add(1);return &cell;
    }
    Lose(__LINE__,20);return nullptr;
  }
  const Root root_;
  std::atomic<uint32_t> first_failure_state_{0};
  Failure first_failure_;
  Ledger ledger_;
  std::mutex mutex_;
  std::array<std::shared_ptr<const NativeIdentityEventSource>,Ledger::kOrigins> sources_{};
  std::array<Origin,Ledger::kOrigins> origins_{};
  std::array<WorkRow,Ledger::kWorks> works_{};
  std::array<ListenerRow,Ledger::kListeners> listeners_{};
  std::array<FastCell,kCells> cells_{};
  std::array<std::shared_ptr<const void>,Ledger::kListeners*2> retired_{};
  std::atomic<bool> stopping_{false};
};
}  // namespace xe::kernel
