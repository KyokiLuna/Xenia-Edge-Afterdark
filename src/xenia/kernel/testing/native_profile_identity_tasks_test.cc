/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_profile_identity_tasks.h"
#include "xenia/kernel/native_system_task_continuation.h"
#include "xenia/gpu/native_frame_execution.h"
namespace xe::kernel::test {
// These are metadata cut tests of the actual retained ledger. They do not
// fabricate a Kernel, native Task, execution/readiness, callback or GPU proof.
class NativeProfileIdentityTaskTestAccess final {
 public:
  using Ledger=identity_detail::NativeProfileIdentityTasks;
  using Frame=Ledger::Frame;
  using Birth=Ledger::Birth;
  static constexpr Ledger::Root root{71,83,97};
  static Frame F(uint32_t function,uint32_t sp=0x91010000,uint32_t lr=0x8199C68C){
    return {root,3,0x90001000,sp,lr,function};
  }
  static Birth Allocate(Ledger& ledger,uint64_t source,uint32_t task,
                        uint32_t callback=0x8199C838,uint32_t argument=0x92002000){
    const auto outer=F(0x816BA440);
    const auto serial=ledger.BeginSchedule(outer,source,callback,argument,0,0x91010100);
    REQUIRE(serial);
    const auto allocation=F(0x816B9A18,outer.sp-0x90,0x816BA46C);
    REQUIRE(ledger.EnterAllocation(serial,allocation,0,allocation.sp+0x50));
    const auto born=ledger.ReturnAllocation(serial,allocation,0,task);
    REQUIRE(born);
    const auto publication=F(0x816B9BB8,outer.sp-0x90,0x816BA4D8);
    REQUIRE(ledger.BeforePublication(serial,publication,task,callback,argument,task|1u));
    REQUIRE(ledger.ReturnSchedule(serial,outer,0));
    return *born;
  }
  static Birth PeriodicAllocate(Ledger&,Ledger::PeriodicFactory&,uint32_t);
  static void PeriodicFactoryProducer();
  static Birth ProfileAllocate(Ledger&,Frame,uint64_t,uint32_t,uint32_t,bool=true,uint32_t=0x14A00004);
  static Birth ProfileParent(Ledger&);
  static void ProfileProducerPublicationAndOptions();
  static void ProfileControlOwnReturnAndReplay();
  static void ProfileCooperativeDispatchAndRestoration();
  static void ProfileSerialCancelAndRecursiveChild();
  static void ProfileProducerCloseAndReusedGeneration();
  static void PeriodicRepeatedDispatch();
  static void PeriodicStopAndReusedBody();
  static void AllNativeAllocationProducerEdges(){
    using Kind=Ledger::AllocationProducer;
    for(const auto kind:{Kind::kSchedule,Kind::kPersistentCreate,Kind::kInline}){
      const auto fn=kind==Kind::kSchedule?0x816BA440u:kind==Kind::kPersistentCreate?0x816BA668u:0x816B9D68u;
      const auto bytes=kind==Kind::kSchedule?0x90u:kind==Kind::kPersistentCreate?0x70u:0xA0u;
      const auto lr=kind==Kind::kSchedule?0x816BA46Cu:kind==Kind::kPersistentCreate?0x816BA688u:0x816B9E08u;
      const auto producer=F(fn,0x7089F760),allocation=F(0x816B9A18,producer.sp-bytes,lr);
      const uint32_t options=kind==Kind::kPersistentCreate?0:0x41000100u;
      REQUIRE(Ledger::ClassifyAllocationProducer(producer,allocation,options,options,allocation.sp+0x50)==kind);
      auto wrong=allocation;wrong.root.generation++;
      CHECK(Ledger::ClassifyAllocationProducer(producer,wrong,options,options,wrong.sp+0x50)==Kind::kUnknown);
      wrong=allocation;wrong.thread++;
      CHECK(Ledger::ClassifyAllocationProducer(producer,wrong,options,options,wrong.sp+0x50)==Kind::kUnknown);
      wrong=allocation;wrong.pcr+=0x1000;
      CHECK(Ledger::ClassifyAllocationProducer(producer,wrong,options,options,wrong.sp+0x50)==Kind::kUnknown);
      wrong=allocation;wrong.sp-=0x10;
      CHECK(Ledger::ClassifyAllocationProducer(producer,wrong,options,options,wrong.sp+0x50)==Kind::kUnknown);
      wrong=allocation;wrong.lr+=4;
      CHECK(Ledger::ClassifyAllocationProducer(producer,wrong,options,options,wrong.sp+0x50)==Kind::kUnknown);
      CHECK(Ledger::ClassifyAllocationProducer(producer,allocation,options,options+4,allocation.sp+0x50)==Kind::kUnknown);
      CHECK(Ledger::ClassifyAllocationProducer(producer,allocation,options,options,allocation.sp+0x54)==Kind::kUnknown);
    }
  }
  static void CommonInlineUsesActualParentAndChildGenerations(){
    Ledger ledger(root);const auto parent=Allocate(ledger,101,0x41001000);
    Ledger::CommonStartProducer scope;
    const auto start=F(0x816A2CF0,0x7089F830,0x816DA570);
    // Scalar metadata fixtures; app/message were NOT observed at V50 denial.
    const std::array<uint32_t,6> args{0x7A,0x1234,0,0x41000600,16,0};
    REQUIRE(scope.EnterStart(start,args));
    const auto copy=F(0x816A2A48,start.sp-0xD0,0x816A2D30);
    REQUIRE(scope.EnterCopy(copy,args[3],args[4],copy.sp+0x50));
    CHECK_FALSE(scope.ReturnStart(start));
    REQUIRE(scope.ReturnCopy(copy,0,0x41002000));
    const auto inlined=F(0x816B9D68,start.sp-0xD0,0x816A2EB8);
    REQUIRE(scope.EnterInline(inlined,0x816A2C80,scope.packet,0,{args[0],args[1],start.sp-0x70},parent));
    const auto allocation=F(0x816B9A18,inlined.sp-0xA0,0x816B9E08);
    REQUIRE(scope.EnterAllocation(allocation,0,allocation.sp+0x50));
    CHECK_FALSE(scope.ReturnInline(inlined,parent)); // Allocator has not returned.
    const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(serial);
    const auto child=ledger.ReturnDetachedAllocation(serial,allocation,0,0x41003000);REQUIRE(child);
    REQUIRE(scope.ReturnAllocation(allocation));
    CHECK(child->generation!=parent.generation);CHECK(child->origin==1);
    CHECK(ledger.FindLiveTask(child->task)->callback==0);
    CHECK_FALSE(ledger.FindLiveTask(child->task)->published);
    CHECK_FALSE(ledger.IsProfileTransportSubject(child->generation));
    CHECK(ledger.FindLiveTask(parent.task)->birth.origin==101);
    // Genuine transport lifetime is separate from copied packet/status and
    // does not claim that a dispatcher callback or a profile event succeeded.
    const auto close=F(0x816B7490,allocation.sp,0x816B9EE8);
    REQUIRE(ledger.EnterClose(close,child->task));
    const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,child->task,1));
    const auto freeing=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);
    REQUIRE(ledger.EnterTaskFree(freeing,child->task));
    REQUIRE(ledger.ReturnTaskFree(freeing));REQUIRE(ledger.ReturnDeletingDestructor(deletion));
    REQUIRE(ledger.ReturnClose(close,0));CHECK_FALSE(ledger.ObservedTransportSubject(child->task));
    auto reused_parent=parent;reused_parent.generation++;
    CHECK_FALSE(scope.ReturnInline(inlined,reused_parent));
    REQUIRE(scope.ReturnInline(inlined,parent));REQUIRE(scope.ReturnStart(start));
    CHECK_FALSE(scope.ReturnStart(start));CHECK_FALSE(scope.EnterCopy(copy,args[3],args[4],copy.sp+0x50));
    CHECK_FALSE(ledger.lost());
  }
  static void CommonInlineRejectsQueuedForeignAndPacketMismatch(){
    const auto start=F(0x816A2CF0,0x7089F830,0x816DA570);
    const std::array<uint32_t,6> args{0x7A,0x1234,0,0x41000600,16,0};
    Ledger::CommonStartProducer scope;REQUIRE(scope.EnterStart(start,args));
    const auto copy=F(0x816A2A48,start.sp-0xD0,0x816A2D30);
    REQUIRE(scope.EnterCopy(copy,args[3],args[4],copy.sp+0x50));REQUIRE(scope.ReturnCopy(copy,0,0x41002000));
    const auto inlined=F(0x816B9D68,start.sp-0xD0,0x816A2EB8);
    const Birth parent{17,101,0x41001000};
    const std::array<uint32_t,3> packet{args[0],args[1],start.sp-0x70};
    CHECK_FALSE(scope.EnterInline(inlined,0x8199C838,scope.packet,0,packet,parent));
    CHECK_FALSE(scope.EnterInline(inlined,0x816A2C80,scope.packet+4,0,packet,parent));
    CHECK_FALSE(scope.EnterInline(inlined,0x816A2C80,scope.packet,4,packet,parent));
    auto bad=packet;bad[0]++;CHECK_FALSE(scope.EnterInline(inlined,0x816A2C80,scope.packet,0,bad,parent));
    bad=packet;bad[1]++;CHECK_FALSE(scope.EnterInline(inlined,0x816A2C80,scope.packet,0,bad,parent));
    bad=packet;bad[2]+=4;CHECK_FALSE(scope.EnterInline(inlined,0x816A2C80,scope.packet,0,bad,parent));
    auto foreign=inlined;foreign.thread++;CHECK_FALSE(scope.EnterInline(foreign,0x816A2C80,scope.packet,0,packet,parent));
    CHECK_FALSE(scope.EnterInline(inlined,0x816A2C80,scope.packet,0,packet,{}));
    const auto queued=F(0x816BA440,start.sp-0xD0,0x816A2EC8);
    CHECK_FALSE(scope.EnterInline(queued,0x816A2C80,scope.packet,0,packet,parent));
    // Classifying queued ABI does not give its other worker a parent read ticket.
    const auto allocation=F(0x816B9A18,queued.sp-0x90,0x816BA46C);
    CHECK(Ledger::ClassifyAllocationProducer(queued,allocation,0,0,allocation.sp+0x50)==Ledger::AllocationProducer::kSchedule);
    REQUIRE(scope.EnterInline(inlined,0x816A2C80,scope.packet,0,packet,parent));
    CHECK_FALSE(scope.EnterInline(inlined,0x816A2C80,scope.packet,0,packet,parent));
    CHECK_FALSE(scope.ReturnStart(start));
  }
  static void CommonCopyFailureCannotInventChild(){
    const auto start=F(0x816A2CF0,0x7089F830,0x816DA570);
    const std::array<uint32_t,6> args{0x7A,0x1234,0,0x41000600,16,0};
    Ledger::CommonStartProducer scope;REQUIRE(scope.EnterStart(start,args));
    const auto copy=F(0x816A2A48,start.sp-0xD0,0x816A2D30);
    auto wrong=copy;wrong.pcr+=0x1000;
    CHECK_FALSE(scope.EnterCopy(wrong,args[3],args[4],copy.sp+0x50));
    REQUIRE(scope.EnterCopy(copy,args[3],args[4],copy.sp+0x50));
    REQUIRE(scope.ReturnCopy(copy,0x8007000E,std::nullopt));
    CHECK(scope.packet==0);REQUIRE(scope.ReturnStart(start));
    const auto inlined=F(0x816B9D68,start.sp-0xD0,0x816A2EB8);
    CHECK_FALSE(scope.MatchesInline(inlined,0x816A2C80,0x41002000,0));
    Ledger::CommonStartProducer missing;REQUIRE(missing.EnterStart(start,args));
    REQUIRE(missing.EnterCopy(copy,args[3],args[4],copy.sp+0x50));
    CHECK_FALSE(missing.ReturnCopy(copy,0,std::nullopt));CHECK_FALSE(missing.ReturnStart(start));
    Ledger::CommonStartProducer explicit_overlap;auto other=args;other[2]=0x41004000;
    REQUIRE(explicit_overlap.EnterStart(start,other));
    REQUIRE(explicit_overlap.EnterCopy(copy,args[3],args[4],copy.sp+0x50));
    REQUIRE(explicit_overlap.ReturnCopy(copy,0,0x41002000));
    CHECK_FALSE(explicit_overlap.MatchesInline(inlined,0x816A2C80,0x41002000,0));
  }
  static Birth ForeignTimed(Ledger& ledger,uint64_t source=1){
    const auto allocation=F(0x816B9A18,0x707DF0F0,0x816BA46C);
    const auto serial=ledger.BeginDetachedAllocation(allocation,source,allocation.sp+0x50);REQUIRE(serial);
    const auto birth=ledger.ReturnDetachedAllocation(serial,allocation,0,0x401EF830);REQUIRE(birth);return *birth;
  }
  static void CompleteModifyFieldABI(){
    for(uint32_t mask=0;mask<16;++mask){
      const auto f=Ledger::DecodeModifyFields(mask);
      CHECK(f.callback==bool(mask&1));CHECK(f.argument==bool(mask&2));
      CHECK(f.options==bool(mask&4));CHECK(f.requeue==bool((mask&4) && (mask&8)));
      CHECK(f.options_only()==bool((mask&4) && !(mask&3)));
    }
    CHECK(Ledger::DecodeModifyFields(0x8000000Cu).options_only());
    for(const auto mask:{4u,12u,0x8000000Cu}){
      CHECK(Ledger::ClassifyModifyCallback(mask,0x123456788199C838ull)==Ledger::CallbackClaim::kOutside);
    }
    CHECK_FALSE(Ledger::DecodeModifyFields(8).requeue); // Native bit8 is inside bit4.
    CHECK(Ledger::PreservesTimedOptions({8,500},{8,16}));
    CHECK(Ledger::PreservesTimedOptions({8,16},{8,500})); //500 is a scalar, not an address.
    CHECK_FALSE(Ledger::PreservesTimedOptions({8,500},{0x18,16}));
    CHECK_FALSE(Ledger::PreservesTimedOptions({0x10000008,500},{0x10000008,16}));
    CHECK_FALSE(Ledger::PreservesTimedOptions({0x10000004,0x41002002},{0x10000004,0x41003002}));
  }
  static void RealForeignTimedOptionsLifetime(){
    Ledger ledger(root);const auto birth=ForeignTimed(ledger);
    REQUIRE(ledger.IsExecutionOnlyOptionsSubject(birth.generation));
    CHECK(ledger.MayClaimExecutionOnlyOptions(2,12,true,birth.generation));
    CHECK_FALSE(ledger.MayClaimExecutionOnlyOptions(1,12,true,birth.generation)); // ProtectedTITLE stays HOLD.
    CHECK_FALSE(ledger.MayClaimExecutionOnlyOptions(0,12,true,birth.generation));
    CHECK_FALSE(ledger.MayClaimExecutionOnlyOptions(3,12,true,birth.generation));
    CHECK_FALSE(ledger.MayClaimExecutionOnlyOptions(2,12,false,birth.generation)); // Ordinary passthrough unchanged.
    CHECK_FALSE(ledger.MayClaimExecutionOnlyOptions(2,12,true,0));
    CHECK_FALSE(ledger.MayClaimExecutionOnlyOptions(2,7,true,birth.generation));
    CHECK_FALSE(ledger.ObservedTransportSubject(birth.task|1u)); // Public thunk, not observer, clearsbit0.
    CHECK_FALSE(ledger.ObservedTransportSubject(birth.task|2u)); // Queue tag is never normalized into a Task.
    const auto modify=F(0x816B9AA8,0x707DF2C0,0x818CA2E0);
    for(const uint32_t interval:{0u,1u,16u,500u,UINT32_MAX}){
      const auto observed=ledger.BeforeExecutionOnlyTimedOptions(modify,birth.task,12,{8,500},{8,interval});REQUIRE(observed);
      REQUIRE(observed->generation==birth.generation);CHECK(observed->origin==1);
      const auto row=std::find_if(ledger.modifications_.begin(),ledger.modifications_.end(),[](const auto& v){return bool(v);});
      REQUIRE(row!=ledger.modifications_.end());CHECK((*row)->after.interval==interval);
      CHECK_FALSE(ledger.IsProfileTransportSubject(birth.generation));
      CHECK_FALSE(ledger.FindLiveTask(birth.task)->published);
      // No native r3/HRESULT is supplied: only actual own-frame/generation join.
      REQUIRE(ledger.ReturnExecutionOnlyTimedOptions(modify,*observed));
      CHECK(ledger.ObservedTransportSubject(birth.task)->generation==birth.generation);
    }
    CHECK_FALSE(ledger.lost());CHECK_FALSE(ledger.all_native_nodes_closed());
  }
  static void TimedOptionsCannotBorrowProfileOrUnknownAuthority(){
    const auto modify=F(0x816B9AA8,0x707DF2C0,0x818CA2E0);
    for(const uint64_t source:{1ull,2ull,101ull}){
      Ledger ledger(root);const auto birth=ForeignTimed(ledger,source);
      CHECK(ledger.IsExecutionOnlyOptionsSubject(birth.generation)==(source==1));
      CHECK(ledger.MayClaimExecutionOnlyOptions(2,12,true,birth.generation)==(source==1));
      if(source!=1){CHECK_FALSE(ledger.BeforeExecutionOnlyTimedOptions(modify,birth.task,12,{8,500},{8,16}));CHECK(ledger.lost());}
    }
    Ledger profile(root);const auto born=Allocate(profile,101,0x41004000);
    CHECK_FALSE(profile.IsExecutionOnlyOptionsSubject(born.generation));
    CHECK_FALSE(profile.MayClaimExecutionOnlyOptions(2,12,true,born.generation));
    CHECK_FALSE(profile.BeforeExecutionOnlyTimedOptions(modify,born.task,12,{8,500},{8,16}));CHECK(profile.lost());
    Ledger absent(root);CHECK_FALSE(absent.BeforeExecutionOnlyTimedOptions(modify,0x401EF830,12,{8,500},{8,16}));CHECK(absent.lost());
    for(const uint32_t mask:{0u,1u,2u,3u,5u,6u,7u,8u}){
      Ledger rejected(root);const auto target=ForeignTimed(rejected);
      CHECK_FALSE(rejected.BeforeExecutionOnlyTimedOptions(modify,target.task,mask,{8,500},{8,16}));CHECK(rejected.lost());
    }
    for(const auto after:{Ledger::TimedOptions{0x18,16},Ledger::TimedOptions{0x10000008,0x41002002}}){
      Ledger rejected(root);const auto target=ForeignTimed(rejected);
      CHECK_FALSE(rejected.BeforeExecutionOnlyTimedOptions(modify,target.task,12,{8,500},after));CHECK(rejected.lost());
    }
  }
  static void TimedOptionsPartialReturnAndCloseRemainOwned(){
    const auto modify=F(0x816B9AA8,0x707DF2C0,0x818CA2E0);
    for(uint32_t fault=0;fault<4;++fault){
      Ledger ledger(root);const auto birth=ForeignTimed(ledger);
      REQUIRE(ledger.BeforeExecutionOnlyTimedOptions(modify,birth.task,4,{8,500},{8,16}));
      if(fault==0){auto wrong=modify;wrong.lr+=4;CHECK_FALSE(ledger.ReturnExecutionOnlyTimedOptions(wrong,birth));}
      if(fault==1){auto wrong=birth;wrong.generation++;CHECK_FALSE(ledger.ReturnExecutionOnlyTimedOptions(modify,wrong));}
      if(fault==2)CHECK_FALSE(ledger.BeforeExecutionOnlyTimedOptions(modify,birth.task,12,{8,500},{8,16}));
      if(fault==3){
        const auto close=F(0x816B7490,0x707DF400,0x818CCD10);REQUIRE(ledger.EnterClose(close,birth.task));
        const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
        CHECK_FALSE(ledger.EnterDeletingDestructor(deletion,birth.task,1));
      }
      CHECK(ledger.lost());CHECK(ledger.ObservedTransportSubject(birth.task)->generation==birth.generation);
      CHECK_FALSE(ledger.all_native_nodes_closed());
    }
  }
  static void DrainObservationIsPartialAndVoidIsNotSuccess(){
    using O=gpu::NativeFramePaintDrainObservation;using S=O::Stage;using V=O::Scalar;
    O cell;cell.Reset(7);cell.Begin(S::kTransportAdmission);
    CHECK_FALSE(cell.Observe(S::kTransportAdmission,0,false));
    CHECK(cell.Observe(S::kTransportAdmission,2,true));
    auto word=cell.Snapshot(S::kTransportAdmission,7);
    CHECK(O::Phase(word)==1);CHECK(O::Evaluated(word)==5);CHECK(O::Truth(word)==4);
    CHECK(cell.Snapshot(S::kPresenter,7)==(7ull<<35)); // Not attempted, not false.
    cell.Finish(S::kTransportAdmission,false);word=cell.Snapshot(S::kTransportAdmission,7);
    CHECK(O::Phase(word)==2);CHECK_FALSE(O::Result(word));
    cell.Begin(S::kPaintFenceAwait);cell.FinishVoid(S::kPaintFenceAwait);
    CHECK(O::Phase(cell.Snapshot(S::kPaintFenceAwait,7))==3); // No boolean/GPUdone result.
    CHECK_FALSE(cell.ScalarSnapshot(V::kCompletionError,7));
    CHECK(cell.Value(V::kCompletionError,0)==0);REQUIRE(cell.ScalarSnapshot(V::kCompletionError,7));
    CHECK(*cell.ScalarSnapshot(V::kCompletionError,7)==0);
    cell.Reset(8);CHECK(cell.Snapshot(S::kTransportAdmission,7)==0);CHECK_FALSE(cell.ScalarSnapshot(V::kCompletionError,7));
    CHECK_FALSE(cell.ScalarSnapshot(V::kCompletionError,8));
  }
  static void DrainObservationPreservesOriginalEvaluation(){
    using O=gpu::NativeFramePaintDrainObservation;using S=O::Stage;using V=O::Scalar;
    O cell;cell.Reset(1);cell.Begin(S::kOldPaused);uint32_t calls=0;
    const auto original=[&](bool result){++calls;return result;};
    const bool blocked=cell.Observe(S::kOldPaused,0,original(false)) && cell.Observe(S::kOldPaused,1,original(true));
    CHECK_FALSE(blocked);CHECK(calls==1);CHECK(O::Evaluated(cell.Snapshot(S::kOldPaused,1))==1);
    const uint64_t timeline=UINT64_MAX-7;
    CHECK(cell.Value(V::kCompletedSubmission,timeline)==timeline);
    REQUIRE(cell.ScalarSnapshot(V::kCompletedSubmission,1));CHECK(*cell.ScalarSnapshot(V::kCompletedSubmission,1)==timeline);
    cell.Reset(O::kMaximumAttempt+1);CHECK(cell.Observe(S::kOldPaused,0,true));
    CHECK(cell.Snapshot(S::kOldPaused,O::kMaximumAttempt+1)==0); // Exhaustion changes no caller bool.
  }
  static void SourceBlockerComesFromOriginalFirstScan(){
    Ledger empty(root);Ledger::SourceBlocker b;
    REQUIRE(empty.SourceDescendantsClosed(101,&b));CHECK(b.available==1);CHECK(b.closed);CHECK(b.kind==0);
    CHECK_FALSE(empty.SourceDescendantsClosed(0,&b));CHECK(b.available==1);CHECK(b.kind==2);CHECK_FALSE(b.closed);
    Ledger pending(root);const auto frame=F(0x816BA440);
    const auto schedule=pending.BeginSchedule(frame,101,0x8199C838,0x92002000,0,0x91010100);REQUIRE(schedule);
    CHECK_FALSE(pending.SourceDescendantsClosed(101,&b));CHECK(b.kind==4);CHECK(b.serial==schedule);
    CHECK(b.frame==frame);CHECK(b.callback==0x8199C838);CHECK(b.task==0); // No fabricated allocation body.
    Ledger detached(root);const auto allocation=F(0x816B9A18,0x707DF0F0,0x816BA46C);
    REQUIRE(detached.BeginDetachedAllocation(allocation,1,allocation.sp+0x50));
    CHECK_FALSE(detached.SourceDescendantsClosed(1,&b));CHECK(b.kind==5);CHECK(b.output==allocation.sp+0x50);
    CHECK(b.task==0);CHECK(b.generation==0);
    Ledger live(root);const auto birth=Allocate(live,101,0x41005000);
    CHECK_FALSE(live.SourceDescendantsClosed(101,&b));CHECK(b.kind==3);CHECK(b.task==birth.task);
    CHECK(b.generation==birth.generation);CHECK(b.source==101);CHECK(b.callback==0x8199C838);
    CHECK_FALSE(live.BeginExternalWork(F(0x816921C8),0x41006000));
    CHECK_FALSE(live.SourceDescendantsClosed(101,&b));CHECK(b.kind==1);CHECK(b.available==1);
  }
  static void PublicationCuts(){
    Ledger ledger(root);
    const auto outer=F(0x816BA440);
    const auto serial=ledger.BeginSchedule(outer,101,0x8199C838,0x92002000,0,0x91010100);
    REQUIRE(serial);
    const auto allocation=F(0x816B9A18,outer.sp-0x90,0x816BA46C);
    REQUIRE(ledger.EnterAllocation(serial,allocation,0,allocation.sp+0x50));
    const auto born=ledger.ReturnAllocation(serial,allocation,0,0x92001000);
    REQUIRE(born);
    // Allocation success is not callback publication. Do not call EnterWork
    // on the production ledger until the observed native B9BB8 pre-store cut.
    REQUIRE_FALSE(ledger.FindLiveTask(born->task)->published);
    const auto publication=F(0x816B9BB8,outer.sp-0x90,0x816BA4D8);
    REQUIRE(ledger.BeforePublication(serial,publication,born->task,0x8199C838,
                                    0x92002000,born->task|1u));
    const auto work=F(0x8199C838,0x93010000,0x816B8200);
    // Actual callback may run before Schedule itself returns.
    REQUIRE(ledger.EnterWork(work,born->task,0x92002000));
    REQUIRE(ledger.ReturnWork(work,*born));
    REQUIRE(ledger.ReturnSchedule(serial,outer,0));
    CHECK_FALSE(ledger.SourceDescendantsClosed(101)); // No real native Close yet.
    CHECK_FALSE(ledger.lost());
  }
  static void HresultIsNotPointer(){
    Ledger ledger(root);const auto outer=F(0x816BA440);
    const auto serial=ledger.BeginSchedule(outer,101,0x8199C838,0,0,0x91010100);
    REQUIRE(serial);
    const auto allocation=F(0x816B9A18,outer.sp-0x90,0x816BA46C);
    REQUIRE(ledger.EnterAllocation(serial,allocation,0,allocation.sp+0x50));
    CHECK_FALSE(ledger.ReturnAllocation(serial,allocation,0x8007000E,0x92001000));
    CHECK_FALSE(ledger.FindLiveTask(0x92001000));
    REQUIRE(ledger.ReturnSchedule(serial,outer,0x8007000E));
    CHECK_FALSE(ledger.lost());
    CHECK(ledger.SourceDescendantsClosed(101));
  }
  static void FreeReuseCut(bool derived){
    Ledger ledger(root);const auto old=Allocate(ledger,101,0x92001000);
    const auto work=F(0x8199C838,0x93010000,0x816B8200);
    REQUIRE(ledger.EnterWork(work,old.task,0x92002000));
    REQUIRE(ledger.ReturnWork(work,old));
    const auto close=F(0x816B7490,0x94010000,0x816B8100);
    REQUIRE(ledger.EnterClose(close,old.task));
    const auto deletion=F(derived?0x816B7D98:0x816B7500,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,old.task,1));
    CHECK(ledger.FindLiveTask(old.task)->birth.generation==old.generation);
    const auto freeing=F(0x816E6AB0,deletion.sp-(derived?0x70:0x60),
                         derived?0x816B7DC8:0x816B752C);
    REQUIRE(ledger.EnterTaskFree(freeing,old.task));
    CHECK_FALSE(ledger.FindLiveTask(old.task));
    // Native allocator may reuse this exact address before the older Free,
    // deleting destructor and Close have returned. It needs a new real output.
    const auto fresh=Allocate(ledger,202,old.task);
    REQUIRE(fresh.generation!=old.generation);
    REQUIRE(ledger.ReturnTaskFree(freeing));
    REQUIRE(ledger.ReturnDeletingDestructor(deletion));
    REQUIRE(ledger.ReturnClose(close,0));
    REQUIRE(ledger.FindLiveTask(old.task));
    CHECK(ledger.FindLiveTask(old.task)->birth.generation==fresh.generation);
    CHECK(ledger.SourceDescendantsClosed(101));
    CHECK_FALSE(ledger.SourceDescendantsClosed(202));
    CHECK_FALSE(ledger.lost());
  }
  static void ExternalBodyBlocksDelete(){
    Ledger ledger(root);const auto born=Allocate(ledger,101,0x92001000);
    const auto body=F(0x816921C8,0x93010000,0x816CCF18);
    REQUIRE(ledger.BeginExternalWork(body,born.task));
    const auto close=F(0x816B7490,0x94010000,0x816B8100);
    REQUIRE(ledger.EnterClose(close,born.task));
    const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
    CHECK_FALSE(ledger.EnterDeletingDestructor(deletion,born.task,1));
    CHECK(ledger.lost());
    CHECK_FALSE(ledger.SourceDescendantsClosed(101));
  }
  static void UnknownAddress(){
    Ledger ledger(root);const auto body=F(0x816921C8);
    CHECK_FALSE(ledger.BeginExternalWork(body,0x92001000));
    CHECK(ledger.lost());
    CHECK_FALSE(ledger.FindLiveTask(0x92001000));
  }
  static Birth Persistent(Ledger& ledger,bool second,uint32_t task=0x92003000) {
    const auto factory=F(0x816BA668,0x91010000,second?0x816F45B0:0x816F4568);
    const auto serial=ledger.BeginPersistentCreate(factory,1,second?0x81AAC274:0x81AAC270);
    REQUIRE(serial);
    const auto allocation=F(0x816B9A18,factory.sp-0x70,0x816BA688);
    REQUIRE(ledger.EnterAllocation(serial,allocation,0,allocation.sp+0x50));
    const auto birth=ledger.ReturnAllocation(serial,allocation,0,task);REQUIRE(birth);
    REQUIRE(ledger.ReturnPersistentCreate(serial,factory,0,task|1u));
    return *birth;
  }
  static void SecondPersistentAndRepeat() {
    Ledger ledger(root);const auto birth=Persistent(ledger,true);
    const auto modify=F(0x816B9AA8,0x91010000,0x816F45D4);
    REQUIRE(ledger.BeforePersistentCallbackBinding(modify,birth.task,5,0x816F3A40,0));
    const auto first=F(0x816F3A40,0x93010000,0x816B8200);
    REQUIRE(ledger.BeginPersistentWork(first,birth.task,0)); // Before Modify own return.
    REQUIRE(ledger.ReturnPersistentWork(first,birth));
    REQUIRE(ledger.ReturnPersistentCallbackBinding(modify));
    const auto second=F(0x816F3A40,0x93020000,0x816B8200);
    REQUIRE(ledger.BeginPersistentWork(second,birth.task,0));
    REQUIRE(ledger.ReturnPersistentWork(second,birth));
    CHECK(ledger.FindLiveTask(birth.task)->birth.generation==birth.generation);
    CHECK(ledger.FindLiveTask(birth.task)->birth.origin==1);
    CHECK_FALSE(ledger.SourceDescendantsClosed(1)); // Neither return is native free.
    CHECK_FALSE(ledger.lost());
  }
  static void WrongPersistentFactoryBinding() {
    Ledger ledger(root);const auto birth=Persistent(ledger,false);
    CHECK_FALSE(ledger.BeforePersistentCallbackBinding(F(0x816B9AA8,0x91010000,0x816F45D4),
        birth.task,5,0x816F3A40,0));
    CHECK(ledger.lost());CHECK_FALSE(ledger.SourceDescendantsClosed(1));
  }
  static void PersistentPrebodyCancellation() {
    Ledger ledger(root);const auto birth=Persistent(ledger,true);
    const auto modify=F(0x816B9AA8,0x91010000,0x816F45D4);
    REQUIRE(ledger.BeforePersistentCallbackBinding(modify,birth.task,5,0x816F3A40,0));
    REQUIRE(ledger.ReturnPersistentCallbackBinding(modify));
    const auto work=F(0x816F3A40,0x93010000,0x816B8200);
    REQUIRE(ledger.BeginPersistentWork(work,birth.task,0));
    REQUIRE(ledger.CancelPersistentBeforeNative(work,birth));
    CHECK_FALSE(ledger.FindLiveTask(birth.task)->persistent_active);
    CHECK_FALSE(ledger.FindLiveTask(birth.task)->freed);
    REQUIRE(ledger.BeginPersistentWork(work,birth.task,0));
    const auto close=F(0x816B7490,0x94010000,0x816B8100);
    REQUIRE(ledger.EnterClose(close,birth.task));
    CHECK_FALSE(ledger.EnterDeletingDestructor(F(0x816B7D98,close.sp-0x60,0x816B74E8),birth.task,1));
    CHECK(ledger.lost());
  }
  static void PersistentConcurrencyDenied() {
    Ledger ledger(root);const auto birth=Persistent(ledger,true);
    const auto modify=F(0x816B9AA8,0x91010000,0x816F45D4);
    REQUIRE(ledger.BeforePersistentCallbackBinding(modify,birth.task,5,0x816F3A40,0));
    REQUIRE(ledger.ReturnPersistentCallbackBinding(modify));
    REQUIRE(ledger.BeginPersistentWork(F(0x816F3A40,0x93010000,0x816B8200),birth.task,0));
    CHECK_FALSE(ledger.BeginPersistentWork(F(0x816F3A40,0x93020000,0x816B8200),birth.task,0));
    CHECK(ledger.lost());CHECK(ledger.FindLiveTask(birth.task)->persistent_active);
  }
  static void FailedPersistentCreateHasNoBirth() {
    Ledger ledger(root);const auto factory=F(0x816BA668,0x91010000,0x816F45B0);
    const auto serial=ledger.BeginPersistentCreate(factory,1,0x81AAC274);REQUIRE(serial);
    const auto allocation=F(0x816B9A18,factory.sp-0x70,0x816BA688);
    REQUIRE(ledger.EnterAllocation(serial,allocation,0,allocation.sp+0x50));
    CHECK_FALSE(ledger.ReturnAllocation(serial,allocation,0x8007000E,0x92003000));
    REQUIRE(ledger.ReturnPersistentCreate(serial,factory,0x8007000E,0x92003001));
    CHECK_FALSE(ledger.FindLiveTask(0x92003000));CHECK_FALSE(ledger.lost());
    CHECK(ledger.SourceDescendantsClosed(1));
  }
  static void PhysicalEventTaskRepeats() {
    Ledger ledger(root);const auto outer=F(0x816BA440,0x91010000,0x816F4610);
    const auto serial=ledger.BeginSchedule(outer,1,0x816F4298,0,outer.sp+0x50,0x81AAC29C);REQUIRE(serial);
    const auto allocation=F(0x816B9A18,outer.sp-0x90,0x816BA46C);
    REQUIRE(ledger.EnterAllocation(serial,allocation,outer.sp+0x50,allocation.sp+0x50));
    const auto birth=ledger.ReturnAllocation(serial,allocation,0,0x92004000);REQUIRE(birth);
    REQUIRE(ledger.BeforePublication(serial,F(0x816B9BB8,outer.sp-0x90,0x816BA4D8),
        birth->task,0x816F4298,0,birth->task|1u));
    for(uint32_t iteration=0;iteration<2;++iteration){
      const auto work=F(0x816F4298,0x93010000+iteration*0x1000,0x816B8200);
      REQUIRE(ledger.BeginPersistentWork(work,birth->task,0));
      REQUIRE(ledger.ReturnPersistentWork(work,*birth));
    }
    REQUIRE(ledger.ReturnSchedule(serial,outer,0));
    CHECK(ledger.FindLiveTask(birth->task)->resident_physical);
    CHECK_FALSE(ledger.FindLiveTask(birth->task)->created_persistent);
    CHECK_FALSE(ledger.SourceDescendantsClosed(1));CHECK_FALSE(ledger.lost());
  }
  static void ExecutedAbortIsNotTaskRelease(){
    Ledger ledger(root);const auto born=Allocate(ledger,3,0x42001000);
    const auto close=F(0x816B7490,0x7080FCF0,0xBCBCBCBC);
    Ledger::CloseReceipt denied;REQUIRE(denied.Issue(root,born,close.thread,close.pcr));
    CHECK_FALSE(denied.Complete(true,0x80004004)); // Executed guard Abort: original body never entered.
    CHECK(denied.phase==Ledger::CloseReceipt::Phase::kFailed);
    CHECK(ledger.FindLiveTask(born.task));CHECK_FALSE(ledger.FindBirth(born.generation)->delete_entered);
    // Do not reject a real observed native count merely because its bits look
    // like HRESULT. Actual entry plus own-return, not range guessing, proves it.
    Ledger::CloseReceipt actual;REQUIRE(actual.Issue(root,born,close.thread,close.pcr));
    REQUIRE(ledger.EnterClose(close,born.task));REQUIRE(actual.Enter(close,born));
    REQUIRE(ledger.ReturnClose(close,0x80004004));
    REQUIRE(actual.Returned(close,0x80004004,true));REQUIRE(actual.Complete(true,0x80004004));
    CHECK(actual.phase==Ledger::CloseReceipt::Phase::kConsumed);CHECK(ledger.FindLiveTask(born.task));
    CHECK_FALSE(actual.Complete(true,0x80004004));CHECK_FALSE(ledger.lost());
  }
  static void ZeroCloseReceiptSurvivesRealFreeReuse(bool derived){
    Ledger ledger(root);const auto old=Allocate(ledger,3,0x42001000);
    const auto callback=F(0x8199C838,0x71000000,0x816B8B64);
    REQUIRE(ledger.EnterWork(callback,old.task,0x92002000));REQUIRE(ledger.ReturnWork(callback,old));
    const auto close=F(0x816B7490,0x7080FCF0,0xBCBCBCBC);
    Ledger::CloseReceipt proof;REQUIRE(proof.Issue(root,old,close.thread,close.pcr));
    REQUIRE(ledger.EnterClose(close,old.task));REQUIRE(proof.Enter(close,old));
    const auto deletion=F(derived?0x816B7D98:0x816B7500,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,old.task,1));
    const auto freeing=F(0x816E6AB0,deletion.sp-(derived?0x70:0x60),derived?0x816B7DC8:0x816B752C);
    REQUIRE(ledger.EnterTaskFree(freeing,old.task));CHECK_FALSE(ledger.ObservedTransportSubject(old.task));
    // Real new allocation may reuse the address before old free/Close returns.
    const auto fresh=Allocate(ledger,4,old.task);REQUIRE(fresh.generation!=old.generation);
    REQUIRE(ledger.ReturnTaskFree(freeing));REQUIRE(ledger.ReturnDeletingDestructor(deletion));
    REQUIRE(ledger.ReturnClose(close,0));REQUIRE(proof.Returned(close,0,true));
    REQUIRE(proof.Complete(true,0));CHECK(proof.birth.generation==old.generation);
    REQUIRE(ledger.ObservedTransportSubject(fresh.task));
    CHECK(ledger.ObservedTransportSubject(fresh.task)->generation==fresh.generation);
    CHECK_FALSE(ledger.FindBirth(fresh.generation)->freed);CHECK_FALSE(ledger.lost());
  }
  static void ReleaseMismatchAndPartialReturnRemainOwned(){
    Ledger ledger(root);const auto born=Allocate(ledger,3,0x42001000);
    const auto close=F(0x816B7490,0x7080FCF0,0xBCBCBCBC);
    Ledger::CloseReceipt wrong_generation;REQUIRE(wrong_generation.Issue(root,born,close.thread,close.pcr));
    auto reused=born;++reused.generation;CHECK_FALSE(wrong_generation.Enter(close,reused));
    Ledger::CloseReceipt wrong_frame;REQUIRE(wrong_frame.Issue(root,born,close.thread,close.pcr));
    auto foreign=close;++foreign.pcr;CHECK_FALSE(wrong_frame.Enter(foreign,born));
    Ledger::CloseReceipt pending;REQUIRE(pending.Issue(root,born,close.thread,close.pcr));
    REQUIRE(ledger.EnterClose(close,born.task));REQUIRE(pending.Enter(close,born));
    CHECK_FALSE(pending.Returned(close,0,false)); // No actual ReturnClose/deletion/free receipt.
    CHECK_FALSE(pending.Complete(true,0));CHECK_FALSE(ledger.all_native_nodes_closed());
    REQUIRE(ledger.FindLiveTask(born.task));CHECK_FALSE(ledger.FindBirth(born.generation)->freed);
    Ledger returned(root);const auto actual=Allocate(returned,4,0x43001000);
    Ledger::CloseReceipt value_mismatch;REQUIRE(value_mismatch.Issue(root,actual,close.thread,close.pcr));
    REQUIRE(returned.EnterClose(close,actual.task));REQUIRE(value_mismatch.Enter(close,actual));
    REQUIRE(returned.ReturnClose(close,2));REQUIRE(value_mismatch.Returned(close,2,true));
    CHECK_FALSE(value_mismatch.Complete(true,3));CHECK_FALSE(ledger.lost());CHECK_FALSE(returned.lost());
  }
  static void ForeignAvatarGenerationAcrossCallerAndWorker(){
    // Caller/worker numbers are metadata after real context validation; this
    // test constructs no TITLE/SYSTEM process, Kernel, Task or callback.
    Ledger ledger(root);const auto allocation=F(0x816B9A18,0x7062FEC0,0x816BA46C);
    const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50,true);REQUIRE(serial);
    const auto born=ledger.ReturnDetachedAllocation(serial,allocation,0,0x401E6B00);REQUIRE(born);
    CHECK(born->origin==1);CHECK(ledger.IsForeignAvatarTransportSubject(born->generation));
    CHECK_FALSE(ledger.IsProfileTransportSubject(born->generation));
    const uint32_t owned_handle=born->task|1u;CHECK((owned_handle&~1u)==born->task);
    auto worker=F(0x816B7490,0x7080FCF0,0xBCBCBCBC);worker.thread=7;worker.pcr=0x90007000;
    Ledger::CloseReceipt temporary;REQUIRE(temporary.Issue(root,*born,worker.thread,worker.pcr));
    REQUIRE(ledger.EnterClose(worker,born->task));REQUIRE(temporary.Enter(worker,*born));
    REQUIRE(ledger.ReturnClose(worker,2));REQUIRE(temporary.Returned(worker,2,true));
    REQUIRE(temporary.Complete(true,2));CHECK(ledger.ObservedTransportSubject(born->task));
    const auto close=F(0x816B7490,0x7062FD00,0xBCBCBCBC);
    Ledger::CloseReceipt final;REQUIRE(final.Issue(root,*born,close.thread,close.pcr));
    REQUIRE(ledger.EnterClose(close,born->task));REQUIRE(final.Enter(close,*born));
    const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,born->task,1));
    const auto freeing=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);
    REQUIRE(ledger.EnterTaskFree(freeing,born->task));REQUIRE(ledger.ReturnTaskFree(freeing));
    REQUIRE(ledger.ReturnDeletingDestructor(deletion));REQUIRE(ledger.ReturnClose(close,0));
    REQUIRE(final.Returned(close,0,true));REQUIRE(final.Complete(true,0));
    CHECK_FALSE(ledger.ObservedTransportSubject(born->task));
    const auto fresh=Allocate(ledger,3,born->task);REQUIRE(fresh.generation!=born->generation);
    CHECK_FALSE(ledger.IsForeignAvatarTransportSubject(fresh.generation));
    CHECK(ledger.IsProfileTransportSubject(fresh.generation));CHECK_FALSE(ledger.lost());
    Ledger wrong_source(root);CHECK_FALSE(wrong_source.BeginDetachedAllocation(allocation,3,allocation.sp+0x50,true));
    CHECK_FALSE(wrong_source.ObservedTransportSubject(born->task));
  }
  static void ForeignControlToRequestReuse(){
    // Actual production ledger metadata after authentic allocation/free cuts.
    // No native Task execution, TLS, caller process or close is synthesized.
    Ledger ledger(root);const auto allocation=F(0x816B9A18,0x7050F4D0,0x816BA46C);
    constexpr uint32_t body=0x401708B0;
    CHECK_FALSE(ledger.ObservedTransportSubject(body));
    const auto first=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50,true);REQUIRE(first);
    const auto old=ledger.ReturnDetachedAllocation(first,allocation,0,body);REQUIRE(old);
    const auto close=F(0x816B7490,0x7080FCF0,0xBCBCBCBC);
    Ledger::CloseReceipt old_close;REQUIRE(old_close.Issue(root,*old,close.thread,close.pcr));
    REQUIRE(ledger.EnterClose(close,body));REQUIRE(old_close.Enter(close,*old));
    const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,body,1));
    const auto freeing=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);
    REQUIRE(ledger.EnterTaskFree(freeing,body));REQUIRE(ledger.ReturnTaskFree(freeing));
    REQUIRE(ledger.ReturnDeletingDestructor(deletion));REQUIRE(ledger.ReturnClose(close,0));
    REQUIRE(old_close.Returned(close,0,true));REQUIRE(old_close.Complete(true,0));
    CHECK_FALSE(ledger.ObservedTransportSubject(body));
    const auto second=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50,true);REQUIRE(second);
    const auto request=ledger.ReturnDetachedAllocation(second,allocation,0,body);REQUIRE(request);
    REQUIRE(request->generation!=old->generation);
    CHECK(ledger.IsForeignAvatarTransportSubject(request->generation));
    CHECK_FALSE(ledger.IsProfileTransportSubject(request->generation));
    const auto observed=ledger.ObservedTransportSubject(body);REQUIRE(observed);
    CHECK(observed->generation==request->generation);
    CHECK_FALSE(old_close.Complete(true,0)); // A consumed receipt cannot close a new birth.
    Ledger::CloseReceipt worker;REQUIRE(worker.Issue(root,*request,close.thread,close.pcr));
    REQUIRE(ledger.EnterClose(close,body));REQUIRE(worker.Enter(close,*request));
    REQUIRE(ledger.ReturnClose(close,2));REQUIRE(worker.Returned(close,2,true));
    REQUIRE(worker.Complete(true,2));CHECK(ledger.ObservedTransportSubject(body));
    CHECK_FALSE(ledger.lost());
  }
  using Admission=NativeSystemTaskContinuation::Admission;
  using Role=Admission::Role;
  static void SharedInvocationToLedger(uint32_t role_index,uint32_t ticket,bool rejected){
    const auto role=role_index==0?Role::kStartup:role_index==1?Role::kPostBootDiagnostic:Role::kOwnedMedia;
    Admission producer;
    if(role!=Role::kStartup){
      REQUIRE(producer.Claim(Role::kStartup));REQUIRE(producer.BindOwner(0xFFF0,std::make_shared<int>(1)));
      producer.Finish(true,true);
    }
    REQUIRE(producer.Claim(role));
    auto invocation=std::make_shared<int>(2);REQUIRE(producer.BindOwner(ticket,invocation));
    const auto outer=F(0x816BA440,0x7062FF50,0xBCBCBCBC);
    const std::array<uint32_t,4> args{0x81696558,ticket,0,0x41000100};
    REQUIRE(producer.MatchesSharedScheduleOwner(invocation,args,args[0],args[3]));
    // This is the actual policy-to-ledger metadata join. No native callback,
    // Task, registry/domain/module, Expected or healthy frontend is fabricated.
    Ledger ledger(root);const auto schedule=ledger.BeginSharedContinuationSchedule(outer,args[1],args[3]);REQUIRE(schedule);
    const auto allocation=F(0x816B9A18,outer.sp-0x90,0x816BA46C);
    REQUIRE(ledger.EnterAllocation(schedule,allocation,0,allocation.sp+0x50));
    const auto born=ledger.ReturnAllocation(schedule,allocation,0,0x401E6B00);REQUIRE(born);
    CHECK(born->origin==1);CHECK_FALSE(ledger.IsProfileTransportSubject(born->generation));
    const auto* node=ledger.FindBirth(born->generation);REQUIRE(node);
    CHECK(node->transport_only);CHECK(node->callback==args[0]);CHECK(node->argument==ticket);
    const auto publication=F(0x816B9BB8,allocation.sp,0x816BA4D8);
    REQUIRE(ledger.BeforePublication(schedule,publication,born->task,args[0],args[1],born->task|1u));
    CHECK_FALSE(node->started);CHECK_FALSE(node->returned); // Common Enter owns its real callback.
    if(!rejected)REQUIRE(ledger.ReturnSchedule(schedule,outer,0));
    CHECK_FALSE(ledger.all_native_nodes_closed()); // Neither Schedule result nor callback address is free proof.
    const auto temporary_close=F(0x816B7490,0x7062E000,0xBCBCBCBC);
    if(!rejected){
      // Actual common Enter checks native GetCurrentTask and closes only that
      // getter's extra reference. A nonzero native count cannot destroy Task.
      REQUIRE(ledger.EnterClose(temporary_close,born->task));
      REQUIRE(ledger.ReturnClose(temporary_close,2));CHECK(ledger.FindLiveTask(born->task));
    }
    const auto close=F(0x816B7490,0x7062D000,rejected?0x816BA518u:0xBCBCBCBCu);
    REQUIRE(ledger.EnterClose(close,born->task));
    const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,born->task,1));
    const auto freeing=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);
    REQUIRE(ledger.EnterTaskFree(freeing,born->task));REQUIRE(ledger.ReturnTaskFree(freeing));
    REQUIRE(ledger.ReturnDeletingDestructor(deletion));REQUIRE(ledger.ReturnClose(close,0));
    // Actual negative BA440 calls Close at BA514 (LR BA518) before returning to its caller.
    if(rejected)REQUIRE(ledger.ReturnSchedule(schedule,outer,0x80004005));
    CHECK(ledger.all_native_nodes_closed());CHECK_FALSE(node->returned); // Never invent identity callback return.
    producer.Finish(true,!rejected);CHECK_FALSE(producer.owner);CHECK_FALSE(ledger.lost());
    const auto fresh=Allocate(ledger,3,born->task);REQUIRE(fresh.generation!=born->generation);
    CHECK_FALSE(ledger.FindBirth(fresh.generation)->transport_only);
    CHECK_FALSE(ledger.FindBirth(fresh.generation)->continuation_ticket);
    CHECK(ledger.IsProfileTransportSubject(fresh.generation)); // New real metadata birth; no carried transport authority.

  }
  static void SharedReceiptMismatchAndStop(){
    Admission producer;REQUIRE(producer.Claim(Role::kStartup));
    auto invocation=std::make_shared<int>(1),other=std::make_shared<int>(2);
    REQUIRE(producer.BindOwner(0x10001,invocation));
    const std::array<uint32_t,4> args{0x81696558,0x10001,0,0x41000100};
    REQUIRE(producer.MatchesSharedScheduleOwner(invocation,args,args[0],args[3]));
    CHECK_FALSE(producer.MatchesSharedScheduleOwner(other,args,args[0],args[3]));
    for(size_t index=0;index<args.size();++index){auto wrong=args;++wrong[index];
      CHECK_FALSE(producer.MatchesSharedScheduleOwner(invocation,wrong,args[0],args[3]));}
    producer.stopping=true;CHECK_FALSE(producer.MatchesSharedScheduleOwner(invocation,args,args[0],args[3]));
    producer.Poison();producer.Finish(false,false);CHECK(producer.owner);CHECK(producer.uncertain);
    CHECK_FALSE(producer.MatchesSharedScheduleOwner(invocation,args,args[0],args[3]));
    Admission media;REQUIRE(media.Claim(Role::kStartup));REQUIRE(media.BindOwner(0xFFF0,std::make_shared<int>(3)));media.Finish(true,true);
    REQUIRE(media.Claim(Role::kOwnedMedia));REQUIRE(media.BindOwner(0x10001,invocation));media.stopping=true;
    CHECK(media.MatchesSharedScheduleOwner(invocation,args,args[0],args[3])); // Existing typed cleanup role remains permitted.
    media.closed=true;CHECK_FALSE(media.MatchesSharedScheduleOwner(invocation,args,args[0],args[3]));
    Ledger rejected(root);CHECK_FALSE(rejected.BeginSharedContinuationSchedule(F(0x816BA440),0,0x41000100));
    CHECK_FALSE(rejected.ObservedTransportSubject(0x401E6B00));
    // A returned negative Schedule alone cannot erase an observed allocation,
    // e.g. cleanup did not return. No destructor/free or callback is invented.
    Ledger retained(root);const auto outer=F(0x816BA440);
    const auto schedule=retained.BeginSharedContinuationSchedule(outer,0x10001,0x41000100);REQUIRE(schedule);
    const auto allocation=F(0x816B9A18,outer.sp-0x90,0x816BA46C);REQUIRE(retained.EnterAllocation(schedule,allocation,0,allocation.sp+0x50));
    const auto born=retained.ReturnAllocation(schedule,allocation,0,0x401E6B00);REQUIRE(born);
    REQUIRE(retained.ReturnSchedule(schedule,outer,0x80004005));CHECK_FALSE(retained.all_native_nodes_closed());
    REQUIRE(retained.ObservedTransportSubject(born->task));CHECK_FALSE(retained.FindBirth(born->generation)->freed);
  }
  static void TypedIdentityKeepsItsOwnObserver(){
    Admission producer;REQUIRE(producer.Claim(Role::kStartup));
    REQUIRE(producer.BindOwner(0xFFF0,std::make_shared<int>(1)));producer.Finish(true,true);
    REQUIRE(producer.Claim(Role::kOwnedProfileIdentity));auto invocation=std::make_shared<int>(2);
    REQUIRE(producer.BindOwner(0x10001,invocation));
    const std::array<uint32_t,4> args{0x81696558,0x10001,0,0x41000100};
    CHECK_FALSE(producer.MatchesSharedScheduleOwner(invocation,args,args[0],args[3]));
    Ledger ordinary(root);const auto identity_frame=F(0x816BA440);
    const auto identity=ordinary.BeginIdentityContinuationSchedule(identity_frame,3,args[1],args[3]);REQUIRE(identity);
    const auto identity_allocation=F(0x816B9A18,identity_frame.sp-0x90,0x816BA46C);
    REQUIRE(ordinary.EnterAllocation(identity,identity_allocation,0,identity_allocation.sp+0x50));
    const auto born=ordinary.ReturnAllocation(identity,identity_allocation,0,0x42001000);REQUIRE(born);
    const auto identity_publication=F(0x816B9BB8,identity_allocation.sp,0x816BA4D8);
    REQUIRE(ordinary.BeforePublication(identity,identity_publication,born->task,0x81696558,0x10001,born->task|1u));
    REQUIRE(ordinary.ReturnSchedule(identity,identity_frame,0));
    CHECK(ordinary.IsProfileTransportSubject(born->generation));CHECK_FALSE(ordinary.FindBirth(born->generation)->transport_only);
    CHECK(ordinary.FindBirth(born->generation)->continuation_ticket);CHECK(born->origin==3);
    const auto identity_work=F(0x81696558,0x71000000,0x816B8B64);
    REQUIRE(ordinary.EnterWork(identity_work,born->task,0x10001));REQUIRE(ordinary.ReturnWork(identity_work,*born));
    Ledger wrong_source(root);CHECK_FALSE(wrong_source.BeginIdentityContinuationSchedule(identity_frame,1,0x10001,args[3]));
    Ledger wrong_abi(root);CHECK_FALSE(wrong_abi.BeginSchedule(identity_frame,3,0x8199C838,0x10001,0,args[3]));
    Ledger shared(root);const auto outer=F(0x816BA440);const auto schedule=shared.BeginSharedContinuationSchedule(outer,0x10001,0x41000100);REQUIRE(schedule);
    const auto allocation=F(0x816B9A18,outer.sp-0x90,0x816BA46C);REQUIRE(shared.EnterAllocation(schedule,allocation,0,allocation.sp+0x50));
    const auto transport=shared.ReturnAllocation(schedule,allocation,0,0x42001000);REQUIRE(transport);
    REQUIRE(shared.BeforePublication(schedule,F(0x816B9BB8,allocation.sp,0x816BA4D8),transport->task,0x81696558,0x10001,transport->task|1u));
    CHECK_FALSE(shared.EnterWork(F(0x81696558),transport->task,0x10001)); // No identity source/work promotion.
    CHECK(shared.lost());CHECK_FALSE(shared.FindBirth(transport->generation)->started);
  }
  static void FirstTaskFailureRetainsOriginalSite(){
    Ledger ledger(root);const auto born=Allocate(ledger,3,0x42001000);
    CHECK(ledger.FirstFailureLine()==0);CHECK(ledger.FirstFailureReason()==0);
    const auto close=F(0x816B7490,0x7080FCF0,0xBCBCBCBC);REQUIRE(ledger.EnterClose(close,born.task));
    auto wrong_return=close;wrong_return.sp-=0x10;
    CHECK_FALSE(ledger.ReturnClose(wrong_return,1));REQUIRE(ledger.lost());
    const auto line=ledger.FirstFailureLine(),reason=ledger.FirstFailureReason();
    REQUIRE(line>0);REQUIRE(reason>1);
    CHECK_FALSE(ledger.EnterTaskFree(F(0x816E6AB0),born.task));
    CHECK(ledger.FirstFailureLine()==line);CHECK(ledger.FirstFailureReason()==reason);
    REQUIRE(ledger.FindBirth(born.generation));CHECK_FALSE(ledger.FindBirth(born.generation)->freed);
    Ledger invalid({});CHECK(invalid.lost());CHECK(invalid.FirstFailureLine()>0);CHECK(invalid.FirstFailureReason()==1);
  }
  static void ActualCommunityCallbackClaim(){
    // Actual V34 raw Schedule argument, not a fabricated native invocation.
    CHECK(Ledger::ClassifyCallback(0xFFFFFFFF91246770ull)==Ledger::CallbackClaim::kOutside);
    CHECK(Ledger::ClassifyCallback(0x91246770)==Ledger::CallbackClaim::kOutside);
    CHECK(Ledger::ClassifyCallback(0x8199C838)==Ledger::CallbackClaim::kCanonicalProfile);
    CHECK(Ledger::ClassifyCallback(0xFFFFFFFF8199C838ull)==Ledger::CallbackClaim::kCanonicalProfile);
    CHECK(Ledger::ClassifyCallback(0x123456788199C838ull)==Ledger::CallbackClaim::kMalformedProfile);
    // Membership/address shape cannot turn a real generic allocation into
    // a profile callback/source. Only its genuine output generation exists.
    Ledger ledger(root);const auto allocation=F(0x816B9A18,0x707DF8B0,0x816BA46C);
    const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(serial);
    const auto birth=ledger.ReturnDetachedAllocation(serial,allocation,0,0x40913980);REQUIRE(birth);
    REQUIRE(ledger.ObservedTransportSubject(birth->task));
    CHECK(ledger.ObservedTransportSubject(birth->task)->generation==birth->generation);
    CHECK_FALSE(ledger.IsProfileTransportSubject(birth->generation));
    const auto provider=F(0x816921C8,0x707DE000,0x816CD700);
    REQUIRE(ledger.BeginExternalWork(provider,birth->task));
    REQUIRE(ledger.ReturnExternalWork(provider,*birth));
    CHECK_FALSE(ledger.IsProfileTransportSubject(birth->generation));
    CHECK_FALSE(ledger.lost());
  }
  static void IncomingProfileBindingCannotHideBehindDetachedBirth(){
    Ledger ledger(root);const auto allocation=F(0x816B9A18,0x707DF8B0,0x816BA46C);
    const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(serial);
    const auto born=ledger.ReturnDetachedAllocation(serial,allocation,0,0x40913980);REQUIRE(born);
    // An existing execution-only allocation has no callback. Its absence does
    // not make a new profile callback publication ordinary child work.
    CHECK_FALSE(ledger.IsProfileTransportSubject(born->generation));
    CHECK(Ledger::ClassifyCallback(0xFFFFFFFF8199C838ull)==Ledger::CallbackClaim::kCanonicalProfile);
    CHECK(Ledger::ClassifyModifyCallback(1,0xFFFFFFFF8199C838ull)==Ledger::CallbackClaim::kCanonicalProfile);
    CHECK(Ledger::ClassifyModifyCallback(5,0xFFFFFFFF8199C838ull)==Ledger::CallbackClaim::kCanonicalProfile);
    CHECK(Ledger::ClassifyModifyCallback(1,0x123456788199C838ull)==Ledger::CallbackClaim::kMalformedProfile);
    // Actual mask bits 1 and 2 select argument/options, not r5 callback. A no-op
    // or these updates must not inspect a stale known-profile-shaped r5.
    for(const uint32_t mask:{0u,2u,4u,6u})
      CHECK(Ledger::ClassifyModifyCallback(mask,0x123456788199C838ull)==Ledger::CallbackClaim::kOutside);
    CHECK(Ledger::ClassifyModifyCallback(1,0xFFFFFFFF91246770ull)==Ledger::CallbackClaim::kOutside);
    const auto observed=ledger.ObservedTransportSubject(born->task);REQUIRE(observed);
    CHECK(observed->generation==born->generation);CHECK(observed->origin==1);
    CHECK_FALSE(ledger.IsProfileTransportSubject(born->generation));CHECK_FALSE(ledger.lost());
  }
  static void GenericChildCloseGeneration(bool derived){
    Ledger ledger(root);const auto allocation=F(0x816B9A18,0x707DF8B0,0x816BA46C);
    const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(serial);
    const auto old=ledger.ReturnDetachedAllocation(serial,allocation,0,0x40913980);REQUIRE(old);
    const auto close=F(0x816B7490,0x707DF940,0x912469D0);
    REQUIRE(ledger.EnterClose(close,old->task));
    const auto deletion=F(derived?0x816B7D98:0x816B7500,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,old->task,1));
    REQUIRE(ledger.OwnsDeletingFrame(deletion));
    // An ordinary nested destructor has no recorded deletion generation. Its
    // frame cannot authorize Free by borrowing the older owned frame above it.
    const auto unclaimed=F(derived?0x816B7D98:0x816B7500,deletion.sp-0x80,0x912469F0);
    CHECK_FALSE(ledger.OwnsDeletingFrame(unclaimed));
    CHECK_FALSE(ledger.ObservedTransportSubject(0x40913A00));
    const auto freeing=F(0x816E6AB0,deletion.sp-(derived?0x70:0x60),derived?0x816B7DC8:0x816B752C);
    REQUIRE(ledger.EnterTaskFree(freeing,old->task));
    CHECK_FALSE(ledger.ObservedTransportSubject(old->task));
    const auto next=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(next);
    const auto fresh=ledger.ReturnDetachedAllocation(next,allocation,0,old->task);REQUIRE(fresh);
    REQUIRE(fresh->generation!=old->generation);
    REQUIRE(ledger.ReturnTaskFree(freeing));REQUIRE(ledger.ReturnDeletingDestructor(deletion));
    CHECK_FALSE(ledger.OwnsDeletingFrame(deletion));
    REQUIRE(ledger.ReturnClose(close,0));
    const auto current=ledger.ObservedTransportSubject(fresh->task);REQUIRE(current);
    CHECK(current->generation==fresh->generation);CHECK_FALSE(ledger.IsProfileTransportSubject(fresh->generation));
    CHECK_FALSE(ledger.IsProfileTransportSubject(old->generation));CHECK_FALSE(ledger.lost());
  }

  using Ref=Ledger::ReferenceReceipt;
  static Frame Callback(uint32_t thread=3,uint32_t sp=0x708CFDA0){
    auto f=F(0x816A2C80,sp,0x816B8B64);f.thread=thread;f.pcr+=thread*0x1000;return f;
  }
  static void AcquireReference(Ref& ref,uint64_t serial,Frame callback,uint32_t task=0x401708D0){
    REQUIRE(ref.Prepare(serial,callback,task));
    REQUIRE(ref.EnterAcquire(ref.acquire));REQUIRE(ref.EnterInner(ref.inner));
    REQUIRE(ref.ReturnInner(ref.inner,task));REQUIRE(ref.ReturnAcquire(ref.acquire,task|1u));
    REQUIRE(ref.CompleteAcquire(true,task|1u,task));
  }
  static void ReferenceRequiresActualNestedGetterReturns(){
    CHECK_FALSE(Ref::MayAcquire(true,true,false,0x816A2C80));
    CHECK(Ref::MayAcquire(true,true,true,0x816BD560));
    CHECK_FALSE(Ref::MayAcquire(true,false,true,0x816BD560));
    CHECK_FALSE(Ref::MayAcquire(true,true,true,0x816BD4D8));
    const auto callback=Callback();
    Ref skipped;REQUIRE(skipped.Prepare(1,callback,0x401708D0));
    CHECK_FALSE(skipped.CompleteAcquire(true,0x401708D1,0x401708D0));
    CHECK(skipped.state()==Ref::Phase::kUncertain);
    Ref partial;REQUIRE(partial.Prepare(2,callback,0x401708D0));
    REQUIRE(partial.EnterAcquire(partial.acquire));REQUIRE(partial.EnterInner(partial.inner));
    CHECK_FALSE(partial.ReturnAcquire(partial.acquire,0x401708D1));
    CHECK_FALSE(partial.finished());
    Ref foreign;REQUIRE(foreign.Prepare(3,callback,0x401708D0));
    auto wrong=foreign.acquire;wrong.pcr+=4;CHECK_FALSE(foreign.EnterAcquire(wrong));
    Ref aborted;AcquireReference(aborted,4,callback);
    CHECK_FALSE(aborted.BeginRelease(callback,callback,0x401708D4,true));
    Ref never_entered;REQUIRE(never_entered.Prepare(5,callback,0x401708D0));
    never_entered.AbandonAcquire();CHECK(never_entered.finished());
    Ref entered;REQUIRE(entered.Prepare(6,callback,0x401708D0));
    REQUIRE(entered.EnterAcquire(entered.acquire));entered.AbandonAcquire();CHECK_FALSE(entered.finished());
  }
  static void ReferenceNonfinalBaselineUsesOwnedStack(){
    const auto callback=Callback();
    for(const auto saved:{0x816B93FCu,0x816B94FCu,0x816BA0DCu})
      CHECK(Ref::NonfinalQueueBaseline(callback,callback.sp-0x1000,callback.sp+0x1000,callback.sp+0x70,saved));
    for(const auto saved:{0x816B95B0u,0x816B971Cu,0u,0x816B93F8u})
      CHECK_FALSE(Ref::NonfinalQueueBaseline(callback,callback.sp-0x1000,callback.sp+0x1000,callback.sp+0x70,saved));
    auto inline_frame=callback;inline_frame.lr=0x816B7EA4;
    CHECK_FALSE(Ref::NonfinalQueueBaseline(inline_frame,callback.sp-0x1000,callback.sp+0x1000,callback.sp+0x70,0x816B93FC));
    CHECK_FALSE(Ref::NonfinalQueueBaseline(callback,callback.sp+0x10,callback.sp+0x1000,callback.sp+0x70,0x816B93FC));
    CHECK_FALSE(Ref::NonfinalQueueBaseline(callback,callback.sp-0x1000,callback.sp+0x6F,callback.sp+0x70,0x816B93FC));
    CHECK_FALSE(Ref::NonfinalQueueBaseline(callback,callback.sp-0x1000,callback.sp+0x1000,callback.sp+0x60,0x816B93FC));
    auto overflow=callback;overflow.sp=0xFFFFFFF0;
    CHECK_FALSE(Ref::NonfinalQueueBaseline(overflow,0xFFFF0000,0xFFFFFFFF,0x60,0x816B93FC));
  }
  static void ReferenceMovesWorkersAndUsesFreshNestedCallFrame(){
    Ledger ledger(root);Ref ref;const auto original=Callback();AcquireReference(ref,31,original);
    const auto current=Callback(9,0x707DFDA0);auto nested=current;
    nested.function=0x819657E0;nested.sp-=0x180;nested.lr=0x819659C0;
    REQUIRE(Ref::NonfinalQueueBaseline(current,current.sp-0x4000,current.sp+0x1000,current.sp+0x70,0x816BA0DC));
    REQUIRE(ref.BeginRelease(nested,current,ref.task,true));
    CHECK(ref.release.thread==current.thread);CHECK(ref.release.pcr==current.pcr);
    CHECK(ref.release.sp==nested.sp-Ref::kExecutePadding);
    CHECK(ref.release.sp!=current.sp-Ref::kExecutePadding);
    REQUIRE(ref.EnterRelease(ref.release));REQUIRE(ref.ReturnRelease(ref.release,1));
    REQUIRE(ref.CompleteRelease(true,1));CHECK(ref.finished());
    // A reference capability has no allocation/execution or read origin.
    CHECK_FALSE(ledger.ObservedTransportSubject(ref.task));CHECK(ledger.all_native_nodes_closed());
    CHECK_FALSE(ledger.IsProfileTransportSubject(ref.serial));
    Ref stale;AcquireReference(stale,32,original);auto other_worker=current;other_worker.pcr+=4;
    CHECK_FALSE(stale.BeginRelease(nested,other_worker,stale.task,true));
  }
  static void ReferenceReleaseRejectsReplayAbortZeroAndPartial(){
    const auto callback=Callback();
    for(const uint64_t raw:{0ull,0x80004004ull,0xFFFFFFFF80004004ull}){
      Ref ref;AcquireReference(ref,101+raw,callback);
      REQUIRE(ref.BeginRelease(callback,callback,ref.task,true));REQUIRE(ref.EnterRelease(ref.release));
      CHECK_FALSE(ref.ReturnRelease(ref.release,raw));CHECK_FALSE(ref.CompleteRelease(true,uint32_t(raw)));
      CHECK_FALSE(ref.finished());
    }
    Ref denied;AcquireReference(denied,7,callback);
    REQUIRE(denied.BeginRelease(callback,callback,denied.task,true));
    CHECK_FALSE(denied.ReturnRelease(denied.release,2)); // Never admitted native body.
    Ref good;AcquireReference(good,8,callback);
    REQUIRE(good.BeginRelease(callback,callback,good.task,true));REQUIRE(good.EnterRelease(good.release));
    REQUIRE(good.ReturnRelease(good.release,2));REQUIRE(good.CompleteRelease(true,2));
    Ref fresh;AcquireReference(fresh,9,callback);
    CHECK_FALSE(good.BeginRelease(callback,callback,good.task,true));
    CHECK_FALSE(good.CompleteRelease(true,2));
    CHECK(fresh.serial!=good.serial);CHECK(fresh.state()==Ref::Phase::kHeld);
    Ref partial;AcquireReference(partial,10,callback);
    REQUIRE(partial.BeginRelease(callback,callback,partial.task,true));REQUIRE(partial.EnterRelease(partial.release));
    partial.Fail();CHECK_FALSE(partial.ReturnRelease(partial.release,2));CHECK_FALSE(partial.finished());
  }
  static void TypedReferenceLookupPreservesNativeFreeReuse(){
    using Kind=Ledger::TransportLookup::Kind;
    Ledger ledger(root);Ledger::TransportLookup seen;
    CHECK_FALSE(ledger.ObservedTransportSubject(0x401708D0,&seen));CHECK(seen.kind==Kind::kAbsent);
    const auto allocation=F(0x816B9A18,0x707DF8B0,0x816BA46C);
    const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(serial);
    const auto old=ledger.ReturnDetachedAllocation(serial,allocation,0,0x401708D0);REQUIRE(old);
    REQUIRE(ledger.ObservedTransportSubject(old->task,&seen));CHECK(seen.kind==Kind::kUniqueLive);
    const auto close=F(0x816B7490,0x707DF940,0xBCBCBCBC);
    REQUIRE(ledger.EnterClose(close,old->task));REQUIRE(ledger.ReturnClose(close,2));
    REQUIRE(ledger.ObservedTransportSubject(old->task,&seen));CHECK(seen.live.generation==old->generation);
    REQUIRE(ledger.EnterClose(close,old->task));
    const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
    REQUIRE(ledger.EnterDeletingDestructor(deletion,old->task,1));
    const auto freeing=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);
    REQUIRE(ledger.EnterTaskFree(freeing,old->task));
    CHECK_FALSE(ledger.ObservedTransportSubject(old->task,&seen));CHECK(seen.kind==Kind::kFreeInProgress);
    const auto next=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(next);
    const auto fresh=ledger.ReturnDetachedAllocation(next,allocation,0,old->task);REQUIRE(fresh);
    // Original unique-live result wins despite the older in-progress free.
    const auto current=ledger.ObservedTransportSubject(old->task,&seen);REQUIRE(current);
    CHECK(current->generation==fresh->generation);CHECK(seen.live_rows==1);CHECK(seen.free_in_progress==1);
    REQUIRE(ledger.ReturnTaskFree(freeing));REQUIRE(ledger.ReturnDeletingDestructor(deletion));
    REQUIRE(ledger.ReturnClose(close,0));
    REQUIRE(ledger.ObservedTransportSubject(old->task,&seen));CHECK(seen.kind==Kind::kUniqueLive);
    REQUIRE(ledger.EnterClose(close,fresh->task));REQUIRE(ledger.EnterDeletingDestructor(deletion,fresh->task,1));
    REQUIRE(ledger.EnterTaskFree(freeing,fresh->task));REQUIRE(ledger.ReturnTaskFree(freeing));
    REQUIRE(ledger.ReturnDeletingDestructor(deletion));REQUIRE(ledger.ReturnClose(close,0));
    CHECK_FALSE(ledger.ObservedTransportSubject(old->task,&seen));CHECK(seen.kind==Kind::kHistoricalFreed);
    CHECK(seen.freed_rows==2);CHECK_FALSE(ledger.lost());
    Ledger corrupt(root);const auto live=Allocate(corrupt,101,0x401708D0);(void)live;
    // Adversarial fixture only: force two live rows to test the real lookup's
    // ambiguous outcome. Observation cannot choose/adopt either generation.
    corrupt.nodes_[1]=corrupt.nodes_[0];corrupt.nodes_[1]->birth.generation+=1000;
    CHECK_FALSE(corrupt.ObservedTransportSubject(0x401708D0,&seen));CHECK(seen.kind==Kind::kAmbiguous);
    CHECK_FALSE(corrupt.ObservedTransportSubject(0x401708D0));
  }

};
TEST_CASE("Avatar Request records fresh foreign birth after real Control Task free",
          "[identity_task][lifetime][avatar_request_task]"){
 NativeProfileIdentityTaskTestAccess::ForeignControlToRequestReuse();
}
TEST_CASE("Native callback publication precedes Schedule return", "[identity_task][publication]"){
 NativeProfileIdentityTaskTestAccess::PublicationCuts();
}
TEST_CASE("Native Task HRESULT never supplies an allocation address", "[identity_task][allocation]"){
 NativeProfileIdentityTaskTestAccess::HresultIsNotPointer();
}
TEST_CASE("Native free retires only the exact old Task address generation", "[identity_task][lifetime]"){
 NativeProfileIdentityTaskTestAccess::FreeReuseCut(GENERATE(false,true));
}
TEST_CASE("A retained whole provider operation prevents native Task deletion", "[identity_task][lifetime]"){
 NativeProfileIdentityTaskTestAccess::ExternalBodyBlocksDelete();
}
TEST_CASE("An unknown native Task address cannot supply a data owner", "[identity_task][authority]"){
 NativeProfileIdentityTaskTestAccess::UnknownAddress();
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Both auxiliary persistent factories keep exact generation and repeated invocation", "[identity_task][bootstrap]"){
 NativeProfileIdentityTaskTestAccess::SecondPersistentAndRepeat();
}
TEST_CASE("A persistent callback cannot bind the other auxiliary factory", "[identity_task][bootstrap]"){
 NativeProfileIdentityTaskTestAccess::WrongPersistentFactoryBinding();
}
TEST_CASE("Prebody cancellation does not free a Task and active persistent work blocks deletion", "[identity_task][lifetime]"){
 NativeProfileIdentityTaskTestAccess::PersistentPrebodyCancellation();
}
TEST_CASE("A persistent Task cannot run overlapping callback invocations", "[identity_task][lifetime]"){
 NativeProfileIdentityTaskTestAccess::PersistentConcurrencyDenied();
}
TEST_CASE("A failed second persistent factory never invents a Task birth", "[identity_task][bootstrap]"){
 NativeProfileIdentityTaskTestAccess::FailedPersistentCreateHasNoBirth();
}
TEST_CASE("A physical event Task keeps one allocation across sequential callbacks", "[identity_task][physical]"){
 NativeProfileIdentityTaskTestAccess::PhysicalEventTaskRepeats();
}
} // namespace xe::kernel::test

#include "xenia/kernel/native_profile_identity_wake_owner.h"
namespace xe::kernel::test {
class NativeIdentityWakeOwnerTestAccess final {
 public:
  using Wake=identity_detail::NativeIdentityWakeOwner;
  using Frame=Wake::Frame;
  static constexpr Wake::Root root{71,83,97};
  static constexpr Wake::Birth physical{11,1,0x92001000};
  static Frame F(uint32_t fn,uint32_t sp=0x93010000,uint32_t lr=0x816DA4B0){return {root,3,0x90001000,sp,lr,fn};}
  static void FirstWakeFailureRetainsOriginalSite(){
    Wake wake(root,physical,2);CHECK(wake.FirstFailureLine()==0);CHECK(wake.FirstFailureReason()==0);
    const auto frame=F(0x816F2618,0x94010000,0x816B8200);
    const auto work=wake.EnterWorker(frame,physical);REQUIRE(work);
    CHECK_FALSE(wake.EnterWorker(frame,physical));REQUIRE(wake.lost());
    const auto line=wake.FirstFailureLine(),reason=wake.FirstFailureReason();REQUIRE(line>0);REQUIRE(reason>1);
    CHECK_FALSE(wake.ReturnWorker(Wake::Work{999},frame));
    CHECK(wake.FirstFailureLine()==line);CHECK(wake.FirstFailureReason()==reason);
    Wake invalid(root,{},2);CHECK(invalid.lost());CHECK(invalid.FirstFailureLine()>0);CHECK(invalid.FirstFailureReason()==1);
  }
  static void IndependentReadBaseline(){
    Wake wake(root,physical,2);
    CHECK_FALSE(wake.EnterWake(F(0x816F25E0),1,true)); // Physical birth is not a read generation.
    auto producer=wake.EnterWake(F(0x816F25E0),2,true);REQUIRE(producer);
    const auto reschedule=F(0x816B7B68);REQUIRE(wake.ReachNativeReschedule(*producer,reschedule,physical));
    const auto body=F(0x816F2618,0x94010000,0x816B8200);auto work=wake.EnterWorker(body,physical);REQUIRE(work);
    CHECK(wake.RetainedSource(*work)==2);
    auto returned=wake.ReturnWorker(*work,body);REQUIRE(returned);CHECK(returned->prepared_origin==0);
    REQUIRE(wake.ReturnNativeReschedule(*producer,reschedule));
    CHECK(wake.InvocationsDrained(0));CHECK(wake.persistent_.origin==1);CHECK_FALSE(wake.lost());
  }
  static void JoinedBaselineAdvancesOnlyAfterProof(){
    Wake wake(root,physical,2);wake.CloseOldIngress();REQUIRE(wake.ArmPrepared(3,0));
    REQUIRE(wake.MarkNativeCachePublished(3));
    const auto producer=wake.EnterWake(F(0x816F25E0),3,true);REQUIRE(producer);
    const auto reschedule=F(0x816B7B68);REQUIRE(wake.ReachNativeReschedule(*producer,reschedule,physical));
    const auto body=F(0x816F2618,0x94010000,0x816B8200);const auto work=wake.EnterWorker(body,physical);REQUIRE(work);
    const auto refresh=F(0x816E3A98,body.sp-0xB0,0x816F2668);
    REQUIRE(wake.EnterRefreshChild(*work,refresh));REQUIRE(wake.ReturnRefreshChild(*work,refresh));
    REQUIRE(wake.ReturnWorker(*work,body));REQUIRE(wake.ReturnNativeReschedule(*producer,reschedule));
    REQUIRE(wake.FinishJoinedOrigin(3,0));
    CHECK_FALSE(wake.EnterWake(F(0x816F25E0),2,true));
    auto next=wake.EnterWorker(body,physical);REQUIRE(next);CHECK(wake.RetainedSource(*next)==3);
    REQUIRE(wake.ReturnWorker(*next,body));CHECK(wake.persistent_.origin==1);CHECK_FALSE(wake.lost());
  }
  static void PendingAndReuseNeverBecomeProof(){
    Wake wake(root,physical,2);wake.CloseOldIngress();CHECK_FALSE(wake.ArmPrepared(3,1));
    CHECK_FALSE(wake.MarkNativeCachePublished(3));CHECK_FALSE(wake.InvocationsDrained(1));
    Wake fresh(root,physical,2);auto reused=physical;reused.generation++;
    CHECK_FALSE(fresh.EnterWorker(F(0x816F2618),reused));CHECK(fresh.persistent_.generation==physical.generation);
  }
};
TEST_CASE("Wake read generation is separate from the immutable physical Task birth", "[identity_wake][source]"){
 NativeIdentityWakeOwnerTestAccess::IndependentReadBaseline();
}
TEST_CASE("Only joined refresh proof advances the ordinary wake read baseline", "[identity_wake][source]"){
 NativeIdentityWakeOwnerTestAccess::JoinedBaselineAdvancesOnlyAfterProof();
}
TEST_CASE("First Task invariant failure survives later return and cleanup errors", "[native_profile_identity][diagnostic]"){
 NativeProfileIdentityTaskTestAccess::FirstTaskFailureRetainsOriginalSite();
}
TEST_CASE("First wake invariant failure survives later worker errors", "[native_profile_identity][diagnostic]"){
 NativeIdentityWakeOwnerTestAccess::FirstWakeFailureRetainsOriginalSite();
}
TEST_CASE("Pending bits and reused Task addresses cannot supply wake readiness", "[identity_wake][source]"){
 NativeIdentityWakeOwnerTestAccess::PendingAndReuseNeverBecomeProof();
}
TEST_CASE("Foreign Avatar Task generations remain execution-only across caller and worker release", "[native_profile_identity][task_close]"){
 NativeProfileIdentityTaskTestAccess::ForeignAvatarGenerationAcrossCallerAndWorker();
}
TEST_CASE("Executed Task guard refusal is not a native reference release", "[native_profile_identity][task_close]"){
 NativeProfileIdentityTaskTestAccess::ExecutedAbortIsNotTaskRelease();
}
TEST_CASE("Zero Task Close receipt keeps the old generation through native free reuse", "[native_profile_identity][task_close]"){
 NativeProfileIdentityTaskTestAccess::ZeroCloseReceiptSurvivesRealFreeReuse(GENERATE(false,true));
}
TEST_CASE("Task release mismatches and partial native returns retain ownership", "[native_profile_identity][task_close]"){
 NativeProfileIdentityTaskTestAccess::ReleaseMismatchAndPartialReturnRemainOwned();
}
TEST_CASE("Shared continuation receipts bind opaque tickets to real native transport generations", "[native_profile_identity][shared_continuation]"){
 NativeProfileIdentityTaskTestAccess::SharedInvocationToLedger(GENERATE(0u,1u,2u),GENERATE(0x10000u,0x10001u),GENERATE(false,true));
}
TEST_CASE("Shared continuation mismatch and stopped admission cannot mint a Task", "[native_profile_identity][shared_continuation]"){
 NativeProfileIdentityTaskTestAccess::SharedReceiptMismatchAndStop();
}
TEST_CASE("Shared transport cannot replace typed identity callback authority", "[native_profile_identity][shared_continuation]"){
 NativeProfileIdentityTaskTestAccess::TypedIdentityKeepsItsOwnObserver();
}
TEST_CASE("Actual Community callback words do not claim a prepared profile Task", "[native_profile_identity][claim_scope]"){
 NativeProfileIdentityTaskTestAccess::ActualCommunityCallbackClaim();
}
TEST_CASE("Incoming profile binding cannot hide behind an execution-only allocation", "[native_profile_identity][claim_scope]"){
 NativeProfileIdentityTaskTestAccess::IncomingProfileBindingCannotHideBehindDetachedBirth();
}
TEST_CASE("Execution-only child Task close preserves allocation generations across native free reuse", "[native_profile_identity][claim_scope]"){
 NativeProfileIdentityTaskTestAccess::GenericChildCloseGeneration(GENERATE(false,true));
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Native Task allocation classifies all three actual producer ABIs", "[identity_task][common_producer][abi]"){
 NativeProfileIdentityTaskTestAccess::AllNativeAllocationProducerEdges();
}
TEST_CASE("Common inline Task preserves real parent and child lifetime generations", "[identity_task][common_producer][lifetime]"){
 NativeProfileIdentityTaskTestAccess::CommonInlineUsesActualParentAndChildGenerations();
}
TEST_CASE("Common inline proof refuses queued workers unknown callbacks and changed packets", "[identity_task][common_producer][provenance]"){
 NativeProfileIdentityTaskTestAccess::CommonInlineRejectsQueuedForeignAndPacketMismatch();
}
TEST_CASE("Common native copy failure cannot invent inline Task creation", "[identity_task][common_producer][failure]"){
 NativeProfileIdentityTaskTestAccess::CommonCopyFailureCannotInventChild();
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Task Modify decodes real field masks and preserves timed scalar ABI", "[identity_task][modify][abi]"){
 NativeProfileIdentityTaskTestAccess::CompleteModifyFieldABI();
}
TEST_CASE("Foreign timed options retain actual generation without callback authority", "[identity_task][modify][lifetime]"){
 NativeProfileIdentityTaskTestAccess::RealForeignTimedOptionsLifetime();
}
TEST_CASE("Timed options cannot borrow profile unknown or reference-bearing authority", "[identity_task][modify][provenance]"){
 NativeProfileIdentityTaskTestAccess::TimedOptionsCannotBorrowProfileOrUnknownAuthority();
}
TEST_CASE("Partial timed Modify returns and close conflicts retain native ownership", "[identity_task][modify][failure]"){
 NativeProfileIdentityTaskTestAccess::TimedOptionsPartialReturnAndCloseRemainOwned();
}
TEST_CASE("Paint drain observations distinguish partial void and unavailable results", "[identity_task][preentry][diagnostic]"){
 NativeProfileIdentityTaskTestAccess::DrainObservationIsPartialAndVoidIsNotSuccess();
}
TEST_CASE("Paint drain observations preserve original evaluation and scalar values", "[identity_task][preentry][diagnostic]"){
 NativeProfileIdentityTaskTestAccess::DrainObservationPreservesOriginalEvaluation();
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Source barriers report the original first blocker without inventing Task identity", "[identity_task][preentry][blocker]"){
 NativeProfileIdentityTaskTestAccess::SourceBlockerComesFromOriginalFirstScan();
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Owned Task reference requires both real getter entry and own returns", "[identity_task][owned_reference][abi]"){
 NativeProfileIdentityTaskTestAccess::ReferenceRequiresActualNestedGetterReturns();
}
TEST_CASE("Nonfinal Task reference baseline requires the actual owned callback stack", "[identity_task][owned_reference][baseline]"){
 NativeProfileIdentityTaskTestAccess::ReferenceNonfinalBaselineUsesOwnedStack();
}
TEST_CASE("Held Task reference survives worker migration without reusing old stack proof", "[identity_task][owned_reference][lifetime]"){
 NativeProfileIdentityTaskTestAccess::ReferenceMovesWorkersAndUsesFreshNestedCallFrame();
}
TEST_CASE("Task reference release rejects replay guard abort zero and partial execution", "[identity_task][owned_reference][failure]"){
 NativeProfileIdentityTaskTestAccess::ReferenceReleaseRejectsReplayAbortZeroAndPartial();
}
TEST_CASE("Typed Task lookup preserves native free reuse without adopting a missing birth", "[identity_task][owned_reference][lookup]"){
 NativeProfileIdentityTaskTestAccess::TypedReferenceLookupPreservesNativeFreeReuse();
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
NativeProfileIdentityTaskTestAccess::Birth NativeProfileIdentityTaskTestAccess::PeriodicAllocate(
 Ledger& ledger,Ledger::PeriodicFactory& factory,uint32_t task){
 const auto enable=F(0x8190A358,0x7083F570,0x8190B440);
 REQUIRE(factory.BeginEnable(factory.serial?factory.serial:7,enable,0x81B2A370,0));
 const auto schedule=F(0x816BA440,enable.sp-0x70,0x8190A3B0);
 const auto serial=ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC);
 REQUIRE(serial);
 const auto allocation=F(0x816B9A18,schedule.sp-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(serial,allocation,schedule.sp+0x50,allocation.sp+0x50));
 const auto birth=ledger.ReturnAllocation(serial,allocation,0,task);REQUIRE(birth);
 REQUIRE(factory.ObserveBirth(*birth));
 const auto publication=F(0x816B9BB8,schedule.sp-0x90,0x816BA4D8);
 REQUIRE(ledger.BeforePublication(serial,publication,task,0x81909CF8,factory.object,task|1u));
 REQUIRE(ledger.ReturnSchedule(serial,schedule,0));REQUIRE(factory.ReturnSchedule(schedule,0,true));
 REQUIRE(factory.ReturnEnable(enable,task|1u,1));return *birth;
}
void NativeProfileIdentityTaskTestAccess::PeriodicFactoryProducer(){
 Ledger ledger(root);Ledger::PeriodicFactory factory;
 auto enable=F(0x8190A358,0x7083F570,0x8190B440);
 CHECK_FALSE(factory.BeginEnable(7,enable,0x81B2A370,0x41001001));
 REQUIRE(factory.BeginEnable(7,enable,0x81B2A370,0));
 const auto schedule=F(0x816BA440,enable.sp-0x70,0x8190A3B0);
 CHECK_FALSE(factory.ReturnEnable(enable,0,1));
 CHECK_FALSE(factory.MatchesSchedule(schedule,0x81909CF8,factory.object+4,schedule.sp+0x50,factory.object+0xC));
 CHECK_FALSE(factory.MatchesSchedule(schedule,0x8199C838,factory.object,schedule.sp+0x50,factory.object+0xC));
 CHECK_FALSE(factory.MatchesSchedule(schedule,0x81909CF8,factory.object,schedule.sp+0x54,factory.object+0xC));
 auto wrong=schedule;wrong.root.generation++;
 CHECK_FALSE(factory.MatchesSchedule(wrong,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC));
 wrong=schedule;wrong.thread++;
 CHECK_FALSE(factory.MatchesSchedule(wrong,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC));
 wrong=schedule;wrong.lr+=4;
 CHECK_FALSE(factory.MatchesSchedule(wrong,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC));
 const auto serial=ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC);REQUIRE(serial);
 CHECK_FALSE(ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC));
 const auto allocation=F(0x816B9A18,schedule.sp-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(serial,allocation,schedule.sp+0x50,allocation.sp+0x50));
 const auto birth=ledger.ReturnAllocation(serial,allocation,0,0x41005000);REQUIRE(birth);REQUIRE(factory.ObserveBirth(*birth));
 CHECK(birth->origin==1);CHECK_FALSE(ledger.IsProfileTransportSubject(birth->generation));
 CHECK_FALSE(ledger.IsExecutionOnlyOptionsSubject(birth->generation));
 CHECK_FALSE(factory.ReturnEnable(enable,birth->task|1u,1));
 const auto publication=F(0x816B9BB8,schedule.sp-0x90,0x816BA4D8);
 REQUIRE(ledger.BeforePublication(serial,publication,birth->task,0x81909CF8,factory.object,birth->task|1u));
 REQUIRE(ledger.ReturnSchedule(serial,schedule,0));REQUIRE(factory.ReturnSchedule(schedule,0,true));
 REQUIRE(factory.ReturnEnable(enable,birth->task|1u,1));
 auto isolated=ledger;CHECK_FALSE(isolated.BeginExternalWork(F(0x816CD6E8),birth->task));
 CHECK_FALSE(ledger.all_native_nodes_closed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::PeriodicRepeatedDispatch(){
 Ledger ledger(root);Ledger::PeriodicFactory factory;const auto birth=PeriodicAllocate(ledger,factory,0x41005000);
 for(uint32_t invocation=0;invocation<3;++invocation){
  auto dispatcher=F(0x816B8B08,0x708A1000+invocation*0x1000,0x816B93FC);
  dispatcher.thread+=invocation;dispatcher.pcr+=invocation*0x1000;
  const auto joined=ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object);REQUIRE(joined);
  CHECK(joined->generation==birth.generation);CHECK(joined->origin==1);
  CHECK_FALSE(ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object));
  CHECK(ledger.FindLiveTask(birth.task)->persistent_active);
  if(invocation!=1){
   auto boundary=dispatcher;boundary.function=0x816B7600;boundary.sp-=0x70;boundary.lr=0x816B8B84;
   auto stale=birth;++stale.generation;CHECK_FALSE(ledger.PeriodicCallbackReturned(dispatcher,boundary,stale));
   REQUIRE(ledger.PeriodicCallbackReturned(dispatcher,boundary,birth));
   CHECK_FALSE(ledger.FindLiveTask(birth.task)->persistent_active);
   CHECK_FALSE(ledger.PeriodicCallbackReturned(dispatcher,boundary,birth));
  }
  REQUIRE(ledger.ReturnPeriodicDispatch(dispatcher,birth));CHECK_FALSE(ledger.ReturnPeriodicDispatch(dispatcher,birth));
  CHECK_FALSE(ledger.FindLiveTask(birth.task)->persistent_active);CHECK_FALSE(ledger.FindLiveTask(birth.task)->freed);
  CHECK_FALSE(ledger.IsProfileTransportSubject(birth.generation));CHECK_FALSE(ledger.PeriodicFactoryQuiescent(factory.serial));
 }
 CHECK_FALSE(ledger.all_native_nodes_closed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::PeriodicStopAndReusedBody(){
 Ledger ledger(root);Ledger::PeriodicFactory factory;const auto birth=PeriodicAllocate(ledger,factory,0x41005000);
 const auto dispatcher=F(0x816B8B08,0x708A1000,0x816B93FC);
 REQUIRE(ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object));
 auto boundary=dispatcher;boundary.function=0x816B7600;boundary.sp-=0x70;boundary.lr=0x816B8B84;
 REQUIRE(ledger.PeriodicCallbackReturned(dispatcher,boundary,birth));
 const auto stop=F(0x819068D0,0x708B1000,0xBCBCBCBC);REQUIRE(factory.BeginStop(stop,factory.object,birth.task|1u));
 auto cancel=stop;cancel.function=0x816B9F08;cancel.sp-=0x70;cancel.lr=0x8190690C;
 auto wait=cancel;wait.function=0x816BA058;wait.lr=0x81906914;
 auto close=cancel;close.function=0x816B7490;close.lr=0x8190691C;
 CHECK_FALSE(factory.ReturnStopChild(wait,birth,true));CHECK_FALSE(factory.ReturnStopChild(cancel,birth,false));
 auto stale=birth;++stale.generation;CHECK_FALSE(factory.ReturnStopChild(cancel,stale,true));
 REQUIRE(factory.ReturnStopChild(cancel,birth,true));
 REQUIRE(factory.ReturnStopChild(wait,birth,ledger.PeriodicWaitReturned(birth)));
 CHECK_FALSE(factory.ReturnStop(stop,0,0,0));
 REQUIRE(ledger.EnterClose(close,birth.task));
 const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
 const auto freeing=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);
 REQUIRE(ledger.EnterDeletingDestructor(deletion,birth.task,1));REQUIRE(ledger.EnterTaskFree(freeing,birth.task));
 REQUIRE(ledger.ReturnTaskFree(freeing));REQUIRE(ledger.ReturnDeletingDestructor(deletion));
 REQUIRE(factory.ReturnStopChild(close,birth,ledger.ReturnClose(close,0)));
 CHECK_FALSE(factory.ReturnStop(stop,0,0,1));REQUIRE(factory.ReturnStop(stop,0,0,0));CHECK_FALSE(factory.ReturnStop(stop,0,0,0));
 CHECK_FALSE(ledger.PeriodicFactoryQuiescent(factory.serial));
 const auto replacement=Allocate(ledger,101,birth.task);CHECK(replacement.generation!=birth.generation);
 REQUIRE(ledger.ReturnPeriodicDispatch(dispatcher,birth));CHECK(ledger.PeriodicFactoryQuiescent(factory.serial));
 CHECK_FALSE(ledger.ReturnPeriodicDispatch(dispatcher,birth));CHECK_FALSE(ledger.lost());
 Ledger restart(root);Ledger::PeriodicFactory fresh;const auto first=PeriodicAllocate(restart,fresh,0x41006000);
 CHECK(first.generation);CHECK_FALSE(fresh.BeginEnable(fresh.serial,F(0x8190A358),fresh.object,0));
 REQUIRE(fresh.BeginStop(stop,fresh.object,first.task|1u));
 REQUIRE(fresh.ReturnStopChild(cancel,first,true));
 REQUIRE(fresh.ReturnStopChild(wait,first,restart.PeriodicWaitReturned(first)));
 REQUIRE(restart.EnterClose(close,first.task));
 REQUIRE(fresh.ReturnStopChild(close,first,restart.ReturnClose(close,1)));
 REQUIRE(fresh.ReturnStop(stop,0,0,0));
 const auto second=PeriodicAllocate(restart,fresh,0x41007000);
 CHECK(second.generation!=first.generation);CHECK(second.task!=first.task);
 CHECK_FALSE(fresh.BeginStop(stop,fresh.object,first.task|1u));
 CHECK(restart.FindLiveTask(first.task)->periodic_stopped);
 CHECK_FALSE(restart.FindLiveTask(second.task)->periodic_stopped);
}
TEST_CASE("Independent periodic factory binds actual Schedule generations without profile authority", "[identity_task][periodic][abi]"){
 NativeProfileIdentityTaskTestAccess::PeriodicFactoryProducer();
}
TEST_CASE("Periodic dispatch repeats without completing its native Task lifetime", "[identity_task][periodic][lifecycle]"){
 NativeProfileIdentityTaskTestAccess::PeriodicRepeatedDispatch();
}
TEST_CASE("Periodic native stop requires ordered references and retains actual dispatcher tails", "[identity_task][periodic][stop]"){
 NativeProfileIdentityTaskTestAccess::PeriodicStopAndReusedBody();
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
NativeProfileIdentityTaskTestAccess::Birth NativeProfileIdentityTaskTestAccess::ProfileParent(Ledger& ledger){
 const auto frame=F(0x816B9A18,0x707D0000,0x816BA46C);
 const auto serial=ledger.BeginDetachedAllocation(frame,1,frame.sp+0x50,true);REQUIRE(serial);
 const auto parent=ledger.ReturnDetachedAllocation(serial,frame,0,0x401708E0);REQUIRE(parent);
 CHECK(ledger.IsForeignAvatarTransportSubject(parent->generation));
 CHECK_FALSE(ledger.IsProfileTransportSubject(parent->generation));return *parent;
}
NativeProfileIdentityTaskTestAccess::Birth NativeProfileIdentityTaskTestAccess::ProfileAllocate(
    Ledger& ledger,Frame owner,uint64_t serial,uint32_t callback,uint32_t task,bool finish,uint32_t native_flags){
 const auto body=owner.sp-Ledger::ProfileProducerFrameBytes(owner.function);
 const auto schedule=F(0x816BA440,body,owner.function==0x8199E148?0x8199E248:0x8199F01C);
 const uint32_t options=body+0x70,output=body+(owner.function==0x8199E148?0x50:0x5C);
 const auto call=ledger.BeginSchedule(schedule,101,callback,0x40170B40,options,output);REQUIRE(call);
 const std::array<uint32_t,2> supplied{owner.function==0x8199E148?0x14000000u:0x10000000u,0x401E6982};
 REQUIRE(ledger.BindProfileProducer(call,serial,owner,supplied));
 const auto allocation=F(0x816B9A18,body-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(call,allocation,options,allocation.sp+0x50));
 const auto birth=ledger.ReturnAllocation(call,allocation,0,task);REQUIRE(birth);
 CHECK_FALSE(ledger.ProfileProducerFor(*birth)->published);
 const auto publish=F(0x816B9BB8,body-0x90,0x816BA4D8);
 REQUIRE(ledger.BeforePublication(call,publish,task,callback,0x40170B40,task|1u));
 REQUIRE(ledger.BindProfileNativeState(call,task,native_flags,0x401E6982));
 if(finish)REQUIRE(ledger.ReturnSchedule(call,schedule,0,std::array<uint32_t,2>{native_flags&~0x00080000u,0x401E6982}));
 return *birth;
}
void NativeProfileIdentityTaskTestAccess::ProfileProducerPublicationAndOptions(){
 for(const auto owner_function:{0x8199E3E0u,0x8199E148u}){
  Ledger ledger(root);const auto parent=ProfileParent(ledger);
  const auto owner=F(owner_function,0x707DFCF0,0xBCBCBCBC);
  const auto birth=ProfileAllocate(ledger,owner,733,0x8199C838,0x40171770,false);
  const auto* p=ledger.ProfileProducerFor(birth);REQUIRE(p);CHECK(p->published);CHECK_FALSE(p->schedule_returned);
  CHECK(p->options[0]==(owner_function==0x8199E148?0x14000000u:0x10000000u));
  CHECK(p->options[1]==0x401E6982);CHECK(p->native_flags==0x14A00004);
  auto wait=F(0x816BA058,p->schedule.sp,0x8199F050);Ledger::ProfileControl premature;
  CHECK_FALSE(premature.Enter(*p,733,owner,wait,birth,parent,birth.task|1u));
  // A real prepublication callback may run before Schedule ownreturn. Its
  // independent callback ledger is unchanged by this neutral control lane.
  const auto callback=F(0x8199C838,0x707CC000,0x816B8B64);
  REQUIRE(ledger.EnterWork(callback,birth.task,p->argument));REQUIRE(ledger.ReturnWork(callback,birth));
  uint64_t schedule=0;for(const auto& row:ledger.schedules_)if(row && row->birth==birth.generation)schedule=row->serial;
  REQUIRE(schedule);
  REQUIRE(ledger.ReturnSchedule(schedule,p->schedule,0,std::array<uint32_t,2>{0x14A00004,0x401E6982}));
  Ledger::ProfileControl valid;REQUIRE(valid.Enter(*p,733,owner,wait,birth,parent,birth.task|1u));
  valid.body_admitted=true;REQUIRE(valid.Return(wait,birth,parent));
  CHECK_FALSE(ledger.all_native_nodes_closed());CHECK_FALSE(ledger.lost());
 }
 // Alternate E3E0->E148 is the same genuine producer/output ABI; no callback
 // or prepared-profile permission is minted by this metadata fixture.
 Ledger alternate(root);const auto owner=F(0x8199E3E0,0x707DFCF0,0xBCBCBCBC);
 const auto born=ProfileAllocate(alternate,owner,734,0x8199E148,0x40171800);
 REQUIRE(alternate.ProfileProducerFor(born));CHECK(alternate.ProfileProducerFor(born)->callback==0x8199E148);
 Ledger sanitize(root);const auto changed=ProfileAllocate(sanitize,owner,735,0x8199C838,0x40171900,false,0x14A80004);
 const auto* initial=sanitize.ProfileProducerFor(changed);REQUIRE(initial);CHECK(initial->native_flags==0x14A80004);
 uint64_t call=0;for(const auto& row:sanitize.schedules_)if(row && row->birth==changed.generation)call=row->serial;
 REQUIRE(call);REQUIRE(sanitize.ReturnSchedule(call,initial->schedule,0,std::array<uint32_t,2>{0x14A00004,0x401E6982}));
 CHECK(initial->native_flags==0x14A00004);CHECK(initial->schedule_returned);
}
void NativeProfileIdentityTaskTestAccess::ProfileControlOwnReturnAndReplay(){
 Ledger ledger(root);const auto parent=ProfileParent(ledger);const auto owner=F(0x8199E3E0,0x707DFCF0,0xBCBCBCBC);
 const auto birth=ProfileAllocate(ledger,owner,733,0x8199C838,0x40171770);const auto* p=ledger.ProfileProducerFor(birth);REQUIRE(p);
 const auto wait=F(0x816BA058,p->schedule.sp,0x8199F050);
 for(unsigned fault=0;fault<7;++fault){
  auto who=owner,at=wait;auto target=birth;uint64_t serial=733;uint32_t tagged=birth.task|1u;
  if(fault==0)serial++;if(fault==1)who.root.generation++;if(fault==2)at.thread++;
  if(fault==3)at.pcr+=4;if(fault==4)at.sp-=0x10;if(fault==5)target.generation++;if(fault==6)tagged+=4;
  Ledger::ProfileControl wrong;CHECK_FALSE(wrong.Enter(*p,serial,who,at,target,parent,tagged));
 }
 Ledger::ProfileControl denied;REQUIRE(denied.Enter(*p,733,owner,wait,birth,parent,birth.task|1u));
 CHECK_FALSE(denied.Return(wait,birth,parent));denied.body_admitted=true;
 CHECK_FALSE(denied.Return(wait,birth,parent)); // Guard-denied return cannot recover.
 Ledger::ProfileControl good;REQUIRE(good.Enter(*p,733,owner,wait,birth,parent,birth.task|1u));good.body_admitted=true;
 REQUIRE(good.Return(wait,birth,parent));CHECK_FALSE(good.Return(wait,birth,parent));
 CHECK_FALSE(ledger.FindLiveTask(birth.task)->started);CHECK_FALSE(ledger.FindLiveTask(birth.task)->returned);
 CHECK(ledger.IsForeignAvatarTransportSubject(parent.generation));CHECK_FALSE(ledger.IsProfileTransportSubject(parent.generation));
}
void NativeProfileIdentityTaskTestAccess::ProfileCooperativeDispatchAndRestoration(){
 Ledger ledger(root);const auto parent=ProfileParent(ledger);const auto owner=F(0x8199E3E0,0x707DFCF0,0xBCBCBCBC);
 const auto birth=ProfileAllocate(ledger,owner,733,0x8199E148,0x40171770);const auto* p=ledger.ProfileProducerFor(birth);REQUIRE(p);
 const auto wait=F(0x816BA058,p->schedule.sp,0x8199F050);
 Ledger::ProfileControl w;REQUIRE(w.Enter(*p,733,owner,wait,birth,parent,birth.task|1u));
 const auto dispatch=F(0x816B8B08,wait.sp-0x80,0x816BA0DC);
 Ledger::ProfileControl denied;CHECK_FALSE(denied.EnterDispatch(w,dispatch,birth));w.body_admitted=true;
 for(unsigned fault=0;fault<4;++fault){auto wrong=dispatch;auto target=birth;
  if(fault==0)wrong.lr+=4;if(fault==1)wrong.sp-=0x10;if(fault==2)wrong.pcr+=4;if(fault==3)target.generation++;
  Ledger::ProfileControl bad;CHECK_FALSE(bad.EnterDispatch(w,wrong,target));}
 Ledger::ProfileControl d;REQUIRE(d.EnterDispatch(w,dispatch,birth));d.body_admitted=true;
 // Original B8B08 clears currentTask=0 before its return; BA058 restores the
 // positive parent afterward. A dispatcher does not forge that restoration.
 REQUIRE(d.Return(dispatch,birth,{}));CHECK_FALSE(w.returned);
 auto stale=parent;stale.generation++;CHECK_FALSE(w.Return(wait,birth,stale));
 CHECK_FALSE(w.Return(wait,birth,parent)); // A failed restoration stays HOLD.
 CHECK_FALSE(ledger.FindLiveTask(birth.task)->started); // Canceled skip invents no callback completion.
}
void NativeProfileIdentityTaskTestAccess::ProfileSerialCancelAndRecursiveChild(){
 Ledger ledger(root);const auto parent=ProfileParent(ledger);const auto owner=F(0x8199E148,0x707DFCF0,0x816B8B64);
 const auto birth=ProfileAllocate(ledger,owner,733,0x8199C838,0x40171770);const auto* p=ledger.ProfileProducerFor(birth);REQUIRE(p);
 const auto cancel=F(0x816B9F08,p->schedule.sp,0x8199E2FC);
 Ledger::ProfileControl c;REQUIRE(c.Enter(*p,733,owner,cancel,birth,parent,birth.task|1u));c.body_admitted=true;
 const auto child=Allocate(ledger,101,0x40171800);const auto nested=F(0x816B9F08,cancel.sp-0x80,0x816B9FE0);
 Ledger::ProfileControl recursive;REQUIRE(recursive.EnterRecursiveCancel(c,nested,child));recursive.body_admitted=true;
 REQUIRE(recursive.Return(nested,child,parent));
 REQUIRE(ledger.ObserveProfileCancelState(birth,0x04A00004,0x401E6982));
 CHECK_FALSE(ledger.ObserveProfileCancelState(birth,0x04A0000C,0x401E6982));
 CHECK_FALSE(ledger.ObserveProfileCancelState(birth,0x04A00004,0x401E6992));
 REQUIRE(c.Return(cancel,birth,parent));
 Ledger::ProfileControl after;const auto wait=F(0x816BA058,p->schedule.sp,0x8199E314);
 REQUIRE(after.Enter(*p,733,owner,wait,birth,parent,birth.task|1u));after.body_admitted=true;
 REQUIRE(after.Return(wait,birth,parent));CHECK(p->native_flags==0x04A00004);
 CHECK_FALSE(ledger.FindLiveTask(child.task)->started);CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ProfileProducerCloseAndReusedGeneration(){
 Ledger ledger(root);const auto parent=ProfileParent(ledger);const auto owner=F(0x8199E3E0,0x707DFCF0,0xBCBCBCBC);
 const auto birth=ProfileAllocate(ledger,owner,733,0x8199C838,0x40171770);const auto* p=ledger.ProfileProducerFor(birth);REQUIRE(p);
 const auto saved=*p;const auto close=F(0x816B7490,p->schedule.sp,0x8199F0D0);
 ledger.ObserveProfileOwnerClose(close,birth);Ledger::ProfileControl closed;
 CHECK_FALSE(closed.Enter(*p,733,owner,F(0x816BA058,p->schedule.sp,0x8199F050),birth,parent,birth.task|1u));
 REQUIRE(ledger.EnterClose(close,birth.task));const auto deletion=F(0x816B7D98,close.sp-0x60,0x816B74E8);
 REQUIRE(ledger.EnterDeletingDestructor(deletion,birth.task,1));CHECK_FALSE(ledger.ProfileProducerFor(birth));
 const auto free=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);REQUIRE(ledger.EnterTaskFree(free,birth.task));
 REQUIRE(ledger.ReturnTaskFree(free));REQUIRE(ledger.ReturnDeletingDestructor(deletion));REQUIRE(ledger.ReturnClose(close,0));
 const auto fresh=ProfileAllocate(ledger,owner,734,0x8199C838,birth.task);REQUIRE(fresh.generation!=birth.generation);
 CHECK_FALSE(ledger.ProfileProducerFor(birth));Ledger::ProfileControl old;
 CHECK_FALSE(old.Enter(saved,733,owner,F(0x816BA058,saved.schedule.sp,0x8199F050),fresh,parent,fresh.task|1u));
 CHECK_FALSE(ledger.lost());
}
TEST_CASE("Profile Task control ownership requires real allocation publication and producer return", "[identity_task][profile_control][abi]"){
 NativeProfileIdentityTaskTestAccess::ProfileProducerPublicationAndOptions();
}
TEST_CASE("Profile Task controls preserve owning caller and foreign parent without replay", "[identity_task][profile_control][lifecycle]"){
 NativeProfileIdentityTaskTestAccess::ProfileControlOwnReturnAndReplay();
}
TEST_CASE("Cooperative profile Wait dispatch separates callback return from parent restoration", "[identity_task][profile_control][wait]"){
 NativeProfileIdentityTaskTestAccess::ProfileCooperativeDispatchAndRestoration();
}
TEST_CASE("Serial profile Cancel preserves real queue flags and nested child lifetime", "[identity_task][profile_control][cancel]"){
 NativeProfileIdentityTaskTestAccess::ProfileSerialCancelAndRecursiveChild();
}
TEST_CASE("Profile control receipts reject closed deleting and reused Task generations", "[identity_task][profile_control][generation]"){
 NativeProfileIdentityTaskTestAccess::ProfileProducerCloseAndReusedGeneration();
}
} // namespace xe::kernel::test
