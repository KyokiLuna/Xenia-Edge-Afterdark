/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_profile_identity_tasks.h"
#include "xenia/kernel/native_identity_image_observation.h"
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
  struct ExecutionBorn {Birth birth;uint64_t producer;Frame frame,publication;};
  static ExecutionBorn ExecutionAllocate(Ledger&,uint32_t,uint32_t=1,uint32_t=0x901159C8,bool=false,uint32_t=3);
  static void ExecutionFree(Ledger&,Birth,uint32_t=0x7080F000);
  struct IdentityServiceFixture;
  static void IdentityServiceProducerAndCopyAbi();
  static void IdentityServiceAcceptanceAndPumpAbi();
  static void IdentityServicePrebodyAdoptionAndPendingTicks();
  static void IdentityServiceTerminalBeforeTailJoins();
  static void IdentityServiceFrameAndBirthReuse();
  static void IdentityServiceBusyCancelAndActualStop();
  static void ExecutionInstructionNoEffectCuts();
  static void ExecutionInstructionRetainedIdentity();
  static void ExecutionInstructionLedgerEffects();
  static void ExecutionUnknownDispatchReadDuty();
  static void ExecutionBeforeProducerReturn();
  static void ExecutionInlineUsesOriginalChildBirth();
  static void ExecutionOpaqueModifyAndActualOperands();
  static void ExecutionFreeReuseRetainsOldFrames();
  static void ExecutionRefusalsStayLocal();
  static void ExecutionCanceledSkipAndMissingReturn();
  static void ExecutionCommonAndOptionalProfile();
  static void ExecutionReadCellNestedRestoration();
  static void ExecutionReadCellRetainsActualOwner();
  static void ExecutionReadCellWrongFramesAndAbandonment();
  static void ExecutionReadFallbackOwningBoundaries();
  static void ExecutionUnavailableOverflowCannotBorrow();
  static Birth PeriodicAllocate(Ledger&,Ledger::PeriodicFactory&,uint32_t,uint32_t=0x04A00008);
  static void PeriodicFactoryProducer();
  static void PeriodicAllocationOutputWitness();
  static void PeriodicConsumersRetainWitness(uint32_t);
  static Birth ProfileAllocate(Ledger&,Frame,uint64_t,uint32_t,uint32_t,bool=true,uint32_t=0x14A00004);
  static Birth ProfileParent(Ledger&);
  static Birth ProfileAllocateWithOutput(Ledger&,Frame,uint64_t,uint32_t,uint32_t,Ledger::ProfileOutput,bool,uint64_t*);
  static void ProfileCallbackThenFree(Ledger&,Birth,Frame);
  static void ProfileOutputStackAndPacketAbi();
  static void ProfileOutputRejectsMalformedProvenance();
  static void ProfileAsyncDoesNotGrantStackControls();
  static void ProfileOutputPrepublicationLifetime();
  static void ProfileWorkerFreeBeforeScheduleReturn();
  static void ProfileScheduleReturnRejectsUnprovenReceipts();
  static void ProfileInitializerErrorCompletionAbi();
  static void ProfileProducerPublicationAndOptions();
  static void ProfileControlOwnReturnAndReplay();
  static void ProfileCooperativeDispatchAndRestoration();
  static void ProfileSerialCancelAndRecursiveChild();
  static void ProfileProducerCloseAndReusedGeneration();
  static void PeriodicRepeatedDispatch();
  static void PeriodicStopAndReusedBody(uint32_t);
  static void PeriodicFrameRetention();
  static void PeriodicReferenceRundown(uint32_t);
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
  static Birth ReuseAfterConcurrentFinalClose(Ledger& ledger,uint32_t task,uint32_t lane) {
    if(lane==0)return Allocate(ledger,202,task);
    if(lane==1){
      const auto allocation=F(0x816B9A18,0x95010000,0x816BA46C);
      const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50);REQUIRE(serial);
      const auto fresh=ledger.ReturnDetachedAllocation(serial,allocation,0,task);REQUIRE(fresh);return *fresh;
    }
    REQUIRE(lane==2);
    const auto made=ExecutionAllocate(ledger,task);
    REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));return made.birth;
  }
  static void ConcurrentCloseSavedResult(bool derived,uint32_t phase,uint32_t lane=0,uint32_t refusal=0) {
    // Model the ORIGINAL atomic results: A decremented 2->1 and retained that
    // value in r31; B later decremented 1->0. A's native own-return can lag B.
    // These are real ledger lifecycle transitions, not a simulated refcounter.
    REQUIRE(phase<=3);REQUIRE((!refusal || phase==3));
    Ledger ledger(root);const auto old=Allocate(ledger,101,0x92001000);
    const auto work=F(0x8199C838,0x93010000,0x816B8200);
    REQUIRE(ledger.EnterWork(work,old.task,0x92002000));REQUIRE(ledger.ReturnWork(work,old));
    auto earlier=F(0x816B7490,0x94020000,0x816B9488);earlier.thread=4;earlier.pcr+=0x1000;
    auto final=F(0x816B7490,0x94010000,0x816B8D94);final.thread=5;final.pcr+=0x2000;
    REQUIRE(ledger.EnterClose(earlier,old.task));REQUIRE(ledger.EnterClose(final,old.task));
    auto deletion=final;deletion.function=derived?0x816B7D98:0x816B7500;
    deletion.sp-=0x60;deletion.lr=0x816B74E8;
    REQUIRE(ledger.EnterDeletingDestructor(deletion,old.task,1));
    const auto finish_earlier=[&](){REQUIRE(ledger.ReturnClose(earlier,1));CHECK_FALSE(ledger.lost());};
    if(phase==0)finish_earlier(); // The other zero-result Close entered deletion.
    auto freeing=deletion;freeing.function=0x816E6AB0;freeing.sp-=derived?0x70:0x60;
    freeing.lr=derived?0x816B7DC8:0x816B752C;
    REQUIRE(ledger.EnterTaskFree(freeing,old.task));CHECK_FALSE(ledger.FindLiveTask(old.task));
    if(phase==1)finish_earlier(); // No live/native object bytes may be consulted.
    REQUIRE(ledger.ReturnTaskFree(freeing));REQUIRE(ledger.ReturnDeletingDestructor(deletion));
    if(phase==2)finish_earlier(); // Actual free/destructor returned; final Close has not.
    REQUIRE(ledger.ReturnClose(final,0));
    if(phase<3){CHECK(ledger.all_native_nodes_closed());CHECK_FALSE(ledger.lost());return;}
    CHECK_FALSE(ledger.all_native_nodes_closed()); // A still owns its exact Close frame.
    const auto fresh=ReuseAfterConcurrentFinalClose(ledger,old.task,lane);
    REQUIRE(fresh.generation!=old.generation);REQUIRE_FALSE(ledger.FindBirth(old.generation));
    // Both the actual numeric address AND the old metadata slot were reused.
    // The delayed own-return must consume only its retained memento.
    REQUIRE(ledger.FindLiveTask(old.task));CHECK(ledger.FindLiveTask(old.task)->birth.generation==fresh.generation);
    if(refusal==1){auto wrong=earlier;wrong.pcr+=0x1000;CHECK_FALSE(ledger.ReturnClose(wrong,1));CHECK(ledger.lost());}
    else if(refusal==3){CHECK_FALSE(ledger.ReturnClose(earlier,0));CHECK(ledger.lost());} // No second final release.
    else{
      finish_earlier();
      if(refusal==2){CHECK_FALSE(ledger.ReturnClose(earlier,1));CHECK(ledger.lost());} // Cannot replay A.
    }
    REQUIRE(ledger.FindLiveTask(fresh.task));CHECK(ledger.FindLiveTask(fresh.task)->birth.generation==fresh.generation);
    CHECK_FALSE(ledger.FindBirth(fresh.generation)->freed);CHECK_FALSE(ledger.FindBirth(fresh.generation)->delete_entered);
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
  static void CanonicalRescheduleTailRetainsWholeCaller(){
    // Actual B76 denied the genuine lis/addi-produced r4=FFFFFFFF815F8E38.
    // Exercise the production metadata matcher, not a duplicate predicate.
    Wake wake(root,physical,2);
    const auto producer=F(0x816F25E0,0x707DF710,0x816DCA20);
    auto reschedule=producer;reschedule.function=0x816B7B68;
    auto tail=producer;tail.function=0x816B7600;
    const auto key=wake.EnterWake(producer,2,true);REQUIRE(key);
    const std::array<uint64_t,4> native{physical.task,0xFFFFFFFF815F8E38ull,15,0};
    const auto match=[&](Frame p,Frame r,Frame t,Wake::Wake pk,Wake::Wake rk,
        uint32_t handle,const std::array<uint64_t,4>& args){
      return wake.MatchesNativeRescheduleTail(pk,rk,p,r,t,handle,args);
    };
    CHECK_FALSE(match(producer,reschedule,tail,*key,*key,physical.task|1u,native));
    REQUIRE(wake.ReachNativeReschedule(*key,reschedule,physical));
    // clrrwi produces a zero-extended r3; only lis/addi r4 has two canonical forms.
    for(const uint64_t descriptor:{0x815F8E38ull,0xFFFFFFFF815F8E38ull}){
      auto args=native;args[1]=descriptor;
      CHECK(match(producer,reschedule,tail,*key,*key,physical.task|1u,args));
      CHECK(match(producer,reschedule,tail,*key,*key,physical.task,args));
    }
    for(const auto args:std::array<std::array<uint64_t,4>,9>{{
        {physical.task+4u,native[1],15,0},
        {0xFFFFFFFF92001000ull,native[1],15,0},
        {0x0000000192001000ull,native[1],15,0},
        {physical.task,0x815F8E3Cull,15,0},
        {physical.task,0x00000001815F8E38ull,15,0},
        {physical.task,0xFFFFFFFE815F8E38ull,15,0},
        {physical.task,native[1],14,0},
        {physical.task,native[1],0xFFFFFFFF0000000Full,0},
        {physical.task,native[1],15,0x100000000ull}}})
      CHECK_FALSE(match(producer,reschedule,tail,*key,*key,physical.task|1u,args));
    CHECK_FALSE(match(producer,reschedule,tail,*key,*key,physical.task+4,native));
    CHECK_FALSE(match(producer,reschedule,tail,{},*key,physical.task,native));
    CHECK_FALSE(match(producer,reschedule,tail,*key,Wake::Wake{key->serial+1},physical.task,native));
    for(uint32_t which=0;which<3;++which)for(uint32_t field=0;field<7;++field){
      auto p=producer,r=reschedule,t=tail;auto& changed=which==0?p:which==1?r:t;
      switch(field){
       case 0:changed.root.generation++;break;
       case 1:changed.thread++;break;
       case 2:changed.pcr+=0x1000;break;
       case 3:changed.sp-=0x10;break;
       case 4:changed.lr+=4;break;
       case 5:changed.function+=4;break;
       case 6:changed.sp|=1;break;
      }
      CHECK_FALSE(match(p,r,t,*key,*key,physical.task,native));
    }
    // Neither accepted canonical forms nor refusals retire the genuine wake.
    wake.CloseOldIngress();CHECK_FALSE(wake.InvocationsDrained(0));
    CHECK_FALSE(wake.CancelBeforeNative(3,0));
    REQUIRE(match(producer,reschedule,tail,*key,*key,physical.task,native));
    REQUIRE(wake.ReturnNativeReschedule(*key,reschedule));
    CHECK_FALSE(match(producer,reschedule,tail,*key,*key,physical.task,native));
    REQUIRE(wake.InvocationsDrained(0));REQUIRE(wake.CancelBeforeNative(3,0));
    CHECK_FALSE(wake.lost());CHECK(wake.persistent_.generation==physical.generation);
  }
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
TEST_CASE("Native reschedule tails accept canonical DWORDs without releasing whole ownership", "[identity_wake][abi][lifetime]"){
 NativeIdentityWakeOwnerTestAccess::CanonicalRescheduleTailRetainsWholeCaller();
}
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
TEST_CASE("Concurrent native nonfinal Close may return after the final Close enters deletion", "[identity_task][task_close][close_interleaving]"){
 NativeProfileIdentityTaskTestAccess::ConcurrentCloseSavedResult(GENERATE(false,true),GENERATE(0u,1u,2u));
}
TEST_CASE("Delayed native Close consumes only its old frame after address and metadata slot reuse", "[identity_task][task_close][close_interleaving]"){
 NativeProfileIdentityTaskTestAccess::ConcurrentCloseSavedResult(GENERATE(false,true),3,GENERATE(0u,1u,2u));
}
TEST_CASE("Delayed Close rejects a foreign frame replay and a second final release", "[identity_task][task_close][close_interleaving][refusal]"){
 NativeProfileIdentityTaskTestAccess::ConcurrentCloseSavedResult(true,3,0,GENERATE(1u,2u,3u));
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
 Ledger& ledger,Ledger::PeriodicFactory& factory,uint32_t task,uint32_t native_flags){
 const auto enable=F(0x8190A358,0x7083F570,0x8190B440);
 REQUIRE(factory.BeginEnable(factory.serial?factory.serial:7,enable,0x81B2A370,0));
 const auto schedule=F(0x816BA440,enable.sp-0x70,0x8190A3B0);
 const auto serial=ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC,{8,5000},2);
 REQUIRE(serial);
 const auto allocation=F(0x816B9A18,schedule.sp-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(serial,allocation,schedule.sp+0x50,allocation.sp+0x50));
 const auto birth=ledger.ReturnAllocation(serial,allocation,0,task);REQUIRE(birth);
 REQUIRE(ledger.BindPeriodicAllocation(serial,allocation,*birth,{native_flags,5000}));
 REQUIRE(factory.ObserveBirth(*birth));
 const auto publication=F(0x816B9BB8,schedule.sp-0x90,0x816BA4D8);
 REQUIRE(ledger.BeforePublication(serial,publication,task,0x81909CF8,factory.object,task|1u));
 REQUIRE(ledger.ReturnSchedule(serial,schedule,0,std::array<uint32_t,2>{native_flags,5000}));REQUIRE(factory.ReturnSchedule(schedule,0,true));
 REQUIRE(factory.ReturnEnable(enable,task|1u,1));return *birth;
}
void NativeProfileIdentityTaskTestAccess::PeriodicAllocationOutputWitness(){
 Ledger ledger(root);Ledger::PeriodicFactory factory;
 const auto enable=F(0x8190A358,0x7083F570,0x8190B440);
 REQUIRE(factory.BeginEnable(7,enable,0x81B2A370,0));
 const auto schedule=F(0x816BA440,enable.sp-0x70,0x8190A3B0);
 const auto begin=[&](std::array<uint32_t,2> options,uint32_t process){
  return ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,
      schedule.sp+0x50,factory.object+0xC,options,process);
 };
 CHECK_FALSE(begin({0,5000},2));CHECK_FALSE(begin({12,5000},2));
 CHECK_FALSE(begin({8,0},2));CHECK_FALSE(begin({8,5001},2));CHECK_FALSE(begin({8,5000},1));
 const auto serial=begin({8,5000},2);REQUIRE(serial);
 const auto allocation=F(0x816B9A18,schedule.sp-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(serial,allocation,schedule.sp+0x50,allocation.sp+0x50));
 const auto born=ledger.ReturnAllocation(serial,allocation,0,0x41005000);REQUIRE(born);
 const auto publication=F(0x816B9BB8,schedule.sp-0x90,0x816BA4D8);
 auto unbound=ledger;CHECK_FALSE(unbound.BeforePublication(serial,publication,born->task,
      0x81909CF8,factory.object,born->task|1u));
 for(const auto words:std::array<std::array<uint32_t,2>,5>{{
     {0,5000},{0x04A0000C,5000},{0x02A0000C,5000},{0x04A00008,0},{0x04A00008,5001}}})
  CHECK_FALSE(ledger.BindPeriodicAllocation(serial,allocation,*born,words));
 auto stale=*born;stale.generation++;
 CHECK_FALSE(ledger.BindPeriodicAllocation(serial,allocation,stale,{0x04A00008,5000}));
 auto wrong=allocation;wrong.sp-=0x10;
 CHECK_FALSE(ledger.BindPeriodicAllocation(serial,wrong,*born,{0x04A00008,5000}));
 CHECK_FALSE(ledger.BindPeriodicAllocation(serial+100,allocation,*born,{0x04A00008,5000}));
 REQUIRE(ledger.BindPeriodicAllocation(serial,allocation,*born,{0x04A00008,5000}));
 CHECK_FALSE(ledger.BindPeriodicAllocation(serial,allocation,*born,{0x02A00008,5000}));
 const auto* node=ledger.FindBirthConst(born->generation);REQUIRE(node);REQUIRE(node->periodic_allocation);
 CHECK(node->periodic_allocation->allocation==allocation);
 CHECK(node->periodic_allocation->schedule==schedule);
 CHECK(node->periodic_allocation->options==std::array<uint32_t,2>{8,5000});
 CHECK(node->periodic_allocation->process==2);CHECK(node->periodic_allocation->factory==factory.serial);
 REQUIRE(factory.ObserveBirth(*born));
 REQUIRE(ledger.BeforePublication(serial,publication,born->task,0x81909CF8,factory.object,born->task|1u));
 auto missing=ledger;CHECK_FALSE(missing.ReturnSchedule(serial,schedule,0));
 auto changed=ledger;CHECK_FALSE(changed.ReturnSchedule(serial,schedule,0,std::array<uint32_t,2>{0x02A00008,5000}));
 REQUIRE(ledger.ReturnSchedule(serial,schedule,0,std::array<uint32_t,2>{0x04A00008,5000}));
 REQUIRE(factory.ReturnSchedule(schedule,0,true));REQUIRE(factory.ReturnEnable(enable,born->task|1u,1));
 CHECK_FALSE(ledger.IsProfileTransportSubject(born->generation));
 CHECK_FALSE(ledger.IsExecutionOnlyOptionsSubject(born->generation));CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::PeriodicConsumersRetainWitness(uint32_t flags){
 Ledger ledger(root);Ledger::PeriodicFactory factory;
 const auto birth=PeriodicAllocate(ledger,factory,0x41005000,flags);
 const std::array<uint32_t,2> native{flags,5000};
 const auto alternate=flags==0x04A00008?0x02A00008u:0x04A00008u;
 REQUIRE(ledger.MatchesPeriodicAllocation(birth,factory.serial,native));
 CHECK_FALSE(ledger.MatchesPeriodicAllocation(birth,factory.serial,{alternate,5000}));
 CHECK_FALSE(ledger.MatchesPeriodicAllocation(birth,factory.serial,{flags|4,5000}));
 CHECK_FALSE(ledger.MatchesPeriodicAllocation(birth,factory.serial,{flags,5001}));
 CHECK_FALSE(ledger.MatchesPeriodicAllocation(birth,factory.serial+1,native));
 auto stale=birth;stale.generation++;CHECK_FALSE(ledger.MatchesPeriodicAllocation(stale,factory.serial,native));
 // Timed queue/state transitions remain the original native implementation.
 // These fixtures exercise exact retained allocation/whole-return ownership,
 // including the genuine canceled skip which has no callback boundary.
 for(uint32_t i=0;i<3;++i){
  const auto dispatcher=F(0x816B8B08,0x708A1000+i*0x1000,0x816B93FC);
  CHECK_FALSE(ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object,{alternate,5000}));
  CHECK_FALSE(ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object+4,native));
  REQUIRE(ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object,native));
  if(i!=1){auto boundary=dispatcher;boundary.function=0x816B7600;
   boundary.sp-=0x70;boundary.lr=0x816B8B84;
   REQUIRE(ledger.PeriodicCallbackReturned(dispatcher,boundary,birth));}
  auto changed=ledger;
  CHECK_FALSE(changed.ReturnPeriodicDispatch(dispatcher,birth,std::array<uint32_t,2>{alternate,5000}));
  REQUIRE(ledger.ReturnPeriodicDispatch(dispatcher,birth,native));
  CHECK_FALSE(ledger.ReturnPeriodicDispatch(dispatcher,birth,native));
  REQUIRE(ledger.MatchesPeriodicAllocation(birth,factory.serial,native));
 }
 CHECK_FALSE(ledger.IsProfileTransportSubject(birth.generation));
 CHECK_FALSE(ledger.IsExecutionOnlyOptionsSubject(birth.generation));CHECK_FALSE(ledger.lost());
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
 const auto serial=ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC,{8,5000},2);REQUIRE(serial);
 CHECK_FALSE(ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,schedule.sp+0x50,factory.object+0xC,{8,5000},2));
 const auto allocation=F(0x816B9A18,schedule.sp-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(serial,allocation,schedule.sp+0x50,allocation.sp+0x50));
 const auto birth=ledger.ReturnAllocation(serial,allocation,0,0x41005000);REQUIRE(birth);
 REQUIRE(ledger.BindPeriodicAllocation(serial,allocation,*birth,{0x04A00008,5000}));REQUIRE(factory.ObserveBirth(*birth));
 CHECK(birth->origin==1);CHECK_FALSE(ledger.IsProfileTransportSubject(birth->generation));
 CHECK_FALSE(ledger.IsExecutionOnlyOptionsSubject(birth->generation));
 CHECK_FALSE(factory.ReturnEnable(enable,birth->task|1u,1));
 const auto publication=F(0x816B9BB8,schedule.sp-0x90,0x816BA4D8);
 REQUIRE(ledger.BeforePublication(serial,publication,birth->task,0x81909CF8,factory.object,birth->task|1u));
 REQUIRE(ledger.ReturnSchedule(serial,schedule,0,std::array<uint32_t,2>{0x04A00008,5000}));REQUIRE(factory.ReturnSchedule(schedule,0,true));
 REQUIRE(factory.ReturnEnable(enable,birth->task|1u,1));
 auto isolated=ledger;CHECK_FALSE(isolated.BeginExternalWork(F(0x816CD6E8),birth->task));
 CHECK_FALSE(ledger.all_native_nodes_closed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::PeriodicRepeatedDispatch(){
 Ledger ledger(root);Ledger::PeriodicFactory factory;const auto birth=PeriodicAllocate(ledger,factory,0x41005000);
 for(uint32_t invocation=0;invocation<3;++invocation){
  auto dispatcher=F(0x816B8B08,0x708A1000+invocation*0x1000,0x816B93FC);
  dispatcher.thread+=invocation;dispatcher.pcr+=invocation*0x1000;
  const auto joined=ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object,{0x04A00008,5000});REQUIRE(joined);
  CHECK(joined->generation==birth.generation);CHECK(joined->origin==1);
  CHECK_FALSE(ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object,{0x04A00008,5000}));
  CHECK(ledger.FindLiveTask(birth.task)->persistent_active);
  if(invocation!=1){
   auto boundary=dispatcher;boundary.function=0x816B7600;boundary.sp-=0x70;boundary.lr=0x816B8B84;
   auto stale=birth;++stale.generation;CHECK_FALSE(ledger.PeriodicCallbackReturned(dispatcher,boundary,stale));
   REQUIRE(ledger.PeriodicCallbackReturned(dispatcher,boundary,birth));
   CHECK_FALSE(ledger.FindLiveTask(birth.task)->persistent_active);
   CHECK_FALSE(ledger.PeriodicCallbackReturned(dispatcher,boundary,birth));
  }
  REQUIRE(ledger.ReturnPeriodicDispatch(dispatcher,birth,std::array<uint32_t,2>{0x04A00008,5000}));CHECK_FALSE(ledger.ReturnPeriodicDispatch(dispatcher,birth,std::array<uint32_t,2>{0x04A00008,5000}));
  CHECK_FALSE(ledger.FindLiveTask(birth.task)->persistent_active);CHECK_FALSE(ledger.FindLiveTask(birth.task)->freed);
  CHECK_FALSE(ledger.IsProfileTransportSubject(birth.generation));CHECK_FALSE(ledger.PeriodicFactoryQuiescent(factory.serial));
 }
 CHECK_FALSE(ledger.all_native_nodes_closed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::PeriodicStopAndReusedBody(uint32_t native_flags){
 Ledger ledger(root);Ledger::PeriodicFactory factory;const auto birth=PeriodicAllocate(ledger,factory,0x41005000,native_flags);
 const auto dispatcher=F(0x816B8B08,0x708A1000,0x816B93FC);
 REQUIRE(ledger.BeginPeriodicDispatch(dispatcher,birth.task,0x81909CF8,factory.object,{native_flags,5000}));
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
 CHECK_FALSE(ledger.ReturnPeriodicDispatch(dispatcher,birth,std::array<uint32_t,2>{native_flags,5000}));
 REQUIRE(ledger.ReturnPeriodicDispatch(dispatcher,birth,{}));CHECK(ledger.PeriodicFactoryQuiescent(factory.serial));
 CHECK_FALSE(ledger.ReturnPeriodicDispatch(dispatcher,birth,std::array<uint32_t,2>{native_flags,5000}));CHECK_FALSE(ledger.lost());
 Ledger restart(root);Ledger::PeriodicFactory fresh;const auto first=PeriodicAllocate(restart,fresh,0x41006000,native_flags);
 CHECK(first.generation);CHECK_FALSE(fresh.BeginEnable(fresh.serial,F(0x8190A358),fresh.object,0));
 REQUIRE(fresh.BeginStop(stop,fresh.object,first.task|1u));
 REQUIRE(fresh.ReturnStopChild(cancel,first,true));
 REQUIRE(fresh.ReturnStopChild(wait,first,restart.PeriodicWaitReturned(first)));
 REQUIRE(restart.EnterClose(close,first.task));
 REQUIRE(fresh.ReturnStopChild(close,first,restart.ReturnClose(close,1)));
 REQUIRE(fresh.ReturnStop(stop,0,0,0));
 const auto second=PeriodicAllocate(restart,fresh,0x41007000,native_flags);
 CHECK(second.generation!=first.generation);CHECK(second.task!=first.task);
 CHECK_FALSE(fresh.BeginStop(stop,fresh.object,first.task|1u));
 CHECK(restart.FindLiveTask(first.task)->periodic_stopped);
 CHECK_FALSE(restart.FindLiveTask(second.task)->periodic_stopped);
}
void NativeProfileIdentityTaskTestAccess::PeriodicFrameRetention(){
 Ledger ledger(root);Ledger::PeriodicFactory factory;
 const auto enable=F(0x8190A358,0x7083F570,0x8190B440);
 CHECK_FALSE(Ledger::RetainAdmittedPeriodicFactory(enable,factory));
 REQUIRE(factory.BeginEnable(7,enable,0x81B2A370,0));
 const auto producer=Ledger::RetainAdmittedPeriodicFactory(enable,factory);REQUIRE(producer);
 REQUIRE(producer->Matches(enable,factory.serial));CHECK_FALSE(producer->allocation);
 // A seal does not erase an already admitted exact frame. It does not turn
 // another factory entry into an admitted producer or authorize a reference.
 auto other=enable;other.sp-=0x100;
 CHECK_FALSE(producer->Matches(other,factory.serial));
 CHECK_FALSE(producer->Matches(enable,factory.serial+1));
 CHECK_FALSE(Ledger::RetainPeriodicProducerChild(other,*producer));
 const auto schedule=F(0x816BA440,enable.sp-0x70,0x8190A3B0);
 const auto scheduled=Ledger::RetainPeriodicProducerChild(schedule,*producer);REQUIRE(scheduled);
 for(uint32_t cut=0;cut<5;++cut){auto wrong=schedule;
  if(cut==0)wrong.root.generation++;if(cut==1)wrong.thread++;
  if(cut==2)wrong.pcr+=0x1000;if(cut==3)wrong.sp-=0x10;if(cut==4)wrong.lr+=4;
  CHECK_FALSE(Ledger::RetainPeriodicProducerChild(wrong,*producer));
 }
 const auto serial=ledger.BeginPeriodicSchedule(schedule,factory,0x81909CF8,factory.object,
     schedule.sp+0x50,factory.object+0xC,{8,5000},2);REQUIRE(serial);
 const auto allocation=F(0x816B9A18,schedule.sp-0x90,0x816BA46C);
 const auto allocated=Ledger::RetainPeriodicProducerChild(allocation,*scheduled);REQUIRE(allocated);
 REQUIRE(ledger.EnterAllocation(serial,allocation,schedule.sp+0x50,allocation.sp+0x50));
 // The producer context survives a closed admission while the actual Create
 // normal return, output binding and publication are still independently due.
 REQUIRE(allocated->Matches(allocation,factory.serial));
 const auto born=ledger.ReturnAllocation(serial,allocation,0,0x41005000);REQUIRE(born);
 REQUIRE(ledger.BindPeriodicAllocation(serial,allocation,*born,{0x04A00008,5000}));
 REQUIRE(factory.ObserveBirth(*born));
 const auto publication=F(0x816B9BB8,schedule.sp-0x90,0x816BA4D8);
 const auto published=Ledger::RetainPeriodicProducerChild(publication,*scheduled);REQUIRE(published);
 CHECK_FALSE(Ledger::RetainPeriodicProducerChild(publication,*allocated));
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(enable,factory,*born));
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(publication,factory,*born));
 REQUIRE(ledger.BeforePublication(serial,publication,born->task,0x81909CF8,factory.object,born->task|1u));
 REQUIRE(published->Matches(publication,factory.serial));
 REQUIRE(ledger.ReturnSchedule(serial,schedule,0,std::array<uint32_t,2>{0x04A00008,5000}));
 REQUIRE(factory.ReturnSchedule(schedule,0,true));REQUIRE(factory.ReturnEnable(enable,born->task|1u,1));
 REQUIRE(scheduled->Matches(schedule,factory.serial));REQUIRE(producer->Matches(enable,factory.serial));
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(enable,factory,*born));
 CHECK_FALSE(ledger.IsProfileTransportSubject(born->generation));CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::PeriodicReferenceRundown(uint32_t flags){
 Ledger ledger(root);Ledger::PeriodicFactory factory;const auto born=PeriodicAllocate(ledger,factory,0x41005000,flags);
 const std::array<uint32_t,2> words{flags,5000};
 // The timer holds a genuine temporary native reference while walking its
 // list. Its Close is independent of the Task's original owner and ready ref.
 const auto timer=F(0x816B7490,0x707AFB50,0x816B8EC8);
 const auto timer_context=ledger.RetainPeriodicSubjectContext(timer,factory,born);REQUIRE(timer_context);
 auto stale=born;stale.generation++;
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(timer,factory,stale));
 auto foreign=factory;foreign.serial++;
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(timer,foreign,born));
 foreign=factory;foreign.root.generation++;
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(timer,foreign,born));
 foreign=factory;foreign.object+=4;
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(timer,foreign,born));
 auto missing=ledger;missing.FindBirth(born.generation)->periodic_allocation.reset();
 CHECK_FALSE(missing.RetainPeriodicSubjectContext(timer,factory,born));
 REQUIRE(ledger.EnterClose(timer,born.task));REQUIRE(ledger.ReturnClose(timer,2));
 CHECK_FALSE(ledger.FindLiveTask(born.task)->freed);
 const auto dispatcher=F(0x816B8B08,0x708A1000,0x816B93FC);
 const auto dispatch_context=ledger.RetainPeriodicSubjectContext(dispatcher,factory,born);REQUIRE(dispatch_context);
 REQUIRE(ledger.BeginPeriodicDispatch(dispatcher,born.task,0x81909CF8,factory.object,words));
 auto boundary=dispatcher;boundary.function=0x816B7600;boundary.sp-=0x70;boundary.lr=0x816B8B84;
 REQUIRE(ledger.PeriodicCallbackReturned(dispatcher,boundary,born));
 REQUIRE(ledger.ReturnPeriodicDispatch(dispatcher,born,words));
 const auto ready_close=F(0x816B7490,dispatcher.sp+0x70,0x816B9488);
 REQUIRE(ledger.RetainPeriodicSubjectContext(ready_close,factory,born));
 REQUIRE(ledger.EnterClose(ready_close,born.task));REQUIRE(ledger.ReturnClose(ready_close,2));
 CHECK_FALSE(ledger.FindLiveTask(born.task)->freed);
 const auto stop=F(0x819068D0,0x708B1000,0xBCBCBCBC);
 REQUIRE(ledger.RetainPeriodicSubjectContext(stop,factory,born));
 REQUIRE(factory.BeginStop(stop,factory.object,born.task|1u));
 auto cancel=stop;cancel.function=0x816B9F08;cancel.sp-=0x70;cancel.lr=0x8190690C;
 auto wait=cancel;wait.function=0x816BA058;wait.lr=0x81906914;
 auto close=cancel;close.function=0x816B7490;close.lr=0x8190691C;
 REQUIRE(ledger.RetainPeriodicSubjectContext(cancel,factory,born));
 REQUIRE(factory.ReturnStopChild(cancel,born,true));
 REQUIRE(ledger.RetainPeriodicSubjectContext(wait,factory,born));
 REQUIRE(factory.ReturnStopChild(wait,born,ledger.PeriodicWaitReturned(born)));
 REQUIRE(ledger.RetainPeriodicSubjectContext(close,factory,born));
 REQUIRE(ledger.EnterClose(close,born.task));
 // A timer still owns its own ref; the real owner Close is nonfinal and does
 // not require/deserve a made-up deleting destructor or Task completion.
 REQUIRE(factory.ReturnStopChild(close,born,ledger.ReturnClose(close,1)));
 REQUIRE(factory.ReturnStop(stop,0,0,0));CHECK_FALSE(ledger.FindLiveTask(born.task)->freed);
 const auto late_timer=ledger.RetainPeriodicSubjectContext(timer,factory,born);REQUIRE(late_timer);
 REQUIRE(ledger.EnterClose(timer,born.task));
 const auto deletion=F(0x816B7D98,timer.sp-0x60,0x816B74E8);
 REQUIRE(ledger.RetainPeriodicSubjectContext(deletion,factory,born));
 REQUIRE(ledger.EnterDeletingDestructor(deletion,born.task,1));
 const auto freeing=F(0x816E6AB0,deletion.sp-0x70,0x816B7DC8);
 const auto free_context=ledger.RetainPeriodicSubjectContext(freeing,factory,born);REQUIRE(free_context);
 REQUIRE(ledger.EnterTaskFree(freeing,born.task));
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(timer,factory,born));
 REQUIRE(ledger.ReturnTaskFree(freeing));REQUIRE(ledger.ReturnDeletingDestructor(deletion));
 REQUIRE(ledger.ReturnClose(timer,0));
 // A captured own-return frame survives genuine free/reuse. New admission on
 // the stale birth cannot borrow the replacement's bytes or factory witness.
 const auto replacement=Allocate(ledger,101,born.task);CHECK(replacement.generation!=born.generation);
 REQUIRE(late_timer->Matches(timer,factory.serial));REQUIRE(free_context->Matches(freeing,factory.serial));
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(timer,factory,born));
 CHECK_FALSE(ledger.RetainPeriodicSubjectContext(timer,factory,replacement));
 auto wrong=timer;wrong.lr+=4;CHECK_FALSE(late_timer->Matches(wrong,factory.serial));
 CHECK_FALSE(ledger.IsProfileTransportSubject(born.generation));CHECK_FALSE(ledger.lost());
}
TEST_CASE("Periodic admitted producer frames retain only their exact original descendants", "[identity_task][periodic][context]"){
 NativeProfileIdentityTaskTestAccess::PeriodicFrameRetention();
}
TEST_CASE("Periodic timer ready and owner references retire independently through native returns", "[identity_task][periodic][references]"){
 NativeProfileIdentityTaskTestAccess::PeriodicReferenceRundown(0x04A00008);
 NativeProfileIdentityTaskTestAccess::PeriodicReferenceRundown(0x02A00008);
}
TEST_CASE("Periodic allocation witnesses bind actual normalized output before publication", "[identity_task][periodic][allocation]"){
 NativeProfileIdentityTaskTestAccess::PeriodicAllocationOutputWitness();
}
TEST_CASE("Periodic consumers preserve one birth's actual native affiliation across repeated returns", "[identity_task][periodic][lifetime]"){
 NativeProfileIdentityTaskTestAccess::PeriodicConsumersRetainWitness(GENERATE(0x04A00008u,0x02A00008u));
}
TEST_CASE("Independent periodic factory binds actual Schedule generations without profile authority", "[identity_task][periodic][abi]"){
 NativeProfileIdentityTaskTestAccess::PeriodicFactoryProducer();
}
TEST_CASE("Periodic dispatch repeats without completing its native Task lifetime", "[identity_task][periodic][lifecycle]"){
 NativeProfileIdentityTaskTestAccess::PeriodicRepeatedDispatch();
}
TEST_CASE("Periodic native stop requires ordered references and retains actual dispatcher tails", "[identity_task][periodic][stop]"){
 NativeProfileIdentityTaskTestAccess::PeriodicStopAndReusedBody(GENERATE(0x04A00008u,0x02A00008u));
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
 const uint32_t options=body+(owner.function==0x8199E148?0x58:0x70),output=body+(owner.function==0x8199E148?0x50:0x5C);
 const auto call=ledger.BeginSchedule(schedule,101,callback,0x40170B40,options,output);REQUIRE(call);
 const std::array<uint32_t,2> supplied{owner.function==0x8199E148?0x14000000u:0x10000000u,0x401E6982};
 REQUIRE(ledger.BindProfileProducer(call,serial,owner,supplied,Ledger::ProfileOutput{}));
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

NativeProfileIdentityTaskTestAccess::ExecutionBorn
NativeProfileIdentityTaskTestAccess::ExecutionAllocate(Ledger& ledger,uint32_t task,
    uint32_t argument,uint32_t callback,bool create,uint32_t thread){
 auto producer=F(create?0x816BA668u:0x816BA440u,0x7080FA00,0x90109680);
 producer.thread=thread;producer.pcr+=(thread-3)*0x1000;
 const uint32_t output=0x404E0C60;
 const auto serial=create?ledger.BeginExecutionCreate(producer,output):
     ledger.BeginExecutionSchedule(producer,callback,argument,0,output);REQUIRE(serial);
 auto allocation=F(0x816B9A18,producer.sp-(create?0x70u:0x90u),create?0x816BA688u:0x816BA46Cu);
 allocation.thread=producer.thread;allocation.pcr=producer.pcr;
 REQUIRE(ledger.EnterExecutionAllocation(serial,allocation,0,allocation.sp+0x50));
 const auto born=ledger.ReturnExecutionAllocation(serial,allocation,0,task);REQUIRE(born);
 auto publication=F(0x816B9BB8,producer.sp-0x90,0x816BA4D8);
 publication.thread=producer.thread;publication.pcr=producer.pcr;
 if(!create){
  REQUIRE(ledger.BeforeExecutionPublication(serial,publication,task,callback,argument,task|1u));
  auto body=publication;body.sp-=0x80;body.lr=0x816B9BC0;
  REQUIRE(ledger.ObserveExecutionPublication(publication,*born,body,task,callback,argument,0x04A00008));
 }
 return {*born,serial,producer,publication};
}
void NativeProfileIdentityTaskTestAccess::ExecutionFree(Ledger& ledger,Birth birth,uint32_t sp){
 Ledger::NeutralObservation local(ledger);
 const auto close=F(0x816B7490,sp,0x816B8D94);
 REQUIRE(ledger.EnterClose(close,birth.task));
 const auto deleting=F(0x816B7D98,sp-0x60,0x816B74E8);
 REQUIRE(ledger.EnterDeletingDestructor(deleting,birth.task,1));
 const auto free=F(0x816E6AB0,deleting.sp-0x70,0x816B7DC8);
 REQUIRE(ledger.EnterTaskFree(free,birth.task));REQUIRE(ledger.ReturnTaskFree(free));
 REQUIRE(ledger.ReturnDeletingDestructor(deleting));REQUIRE(ledger.ReturnClose(close,0));
}
void NativeProfileIdentityTaskTestAccess::ExecutionBeforeProducerReturn(){
 for(uint32_t argument:{1u,UINT32_MAX}){
  Ledger ledger(root);const auto made=ExecutionAllocate(ledger,0x404E0BF0,argument);
  CHECK(made.birth.origin==1);CHECK_FALSE(ledger.IsProfileTransportSubject(made.birth.generation));
  CHECK(ledger.SourceDescendantsClosed(101)); // no profile source inherited
  auto dispatcher=F(0x816B8B08,0x70850000,0x816B93FC);dispatcher.thread=4;dispatcher.pcr+=0x1000;
  const auto invocation=ledger.BeginExecutionDispatch(dispatcher,made.birth);REQUIRE(invocation);
  auto body=dispatcher;body.sp-=0x70;body.lr=0x816B8B48;
  CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp,made.birth.task));
  REQUIRE(ledger.ExecutionCallbackSelected(invocation,body,made.birth.task,0x901159C8,argument));
  const auto fact=ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp,made.birth.task);REQUIRE(fact);
  CHECK(fact->birth.generation==made.birth.generation);CHECK(fact->invocation==invocation);
  CHECK(fact->callback==0x901159C8);CHECK(fact->argument==argument);CHECK(fact->callback_lr==0x816B8B64);
  CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread+1,body.pcr,body.sp,made.birth.task));
  CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr+4,body.sp,made.birth.task));
  CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,dispatcher.sp,made.birth.task));
  CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp,made.birth.task+0x50));
  body.lr=0x816B8B64;
  REQUIRE(ledger.ExecutionCallbackReturned(invocation,body,made.birth.task,0x8000000A));
  CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp,made.birth.task));
  REQUIRE(ledger.ReturnExecutionDispatch(invocation,dispatcher,made.birth));
  CHECK(ledger.FindSchedule(made.producer)); // worker really precedes producer own-return
  REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));
  CHECK_FALSE(ledger.FindLiveTask(made.birth.task)->started); // no typed read/work grant
  ExecutionFree(ledger,made.birth);CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
 }
}
void NativeProfileIdentityTaskTestAccess::ExecutionInlineUsesOriginalChildBirth(){
 for(uint32_t loaded_lr:{0x816B7E4Cu,0x816B7E90u}){CAPTURE(loaded_lr);
  Ledger ledger(root);const auto parent=Allocate(ledger,101,0x404D0000);
  const auto producer=F(0x816B9D68,0x7080FA00,0x816A2EB8);
  const auto allocation=F(0x816B9A18,producer.sp-0xA0,0x816B9E08);
  REQUIRE(Ledger::ClassifyAllocationProducer(producer,allocation,0,0,allocation.sp+0x50)==Ledger::AllocationProducer::kInline);
  const auto serial=ledger.BeginDetachedAllocation(allocation,1,allocation.sp+0x50,false,true,0x816A2C80,0x404E0C60);REQUIRE(serial);
  const auto born=ledger.ReturnDetachedAllocation(serial,allocation,0,0x404E0BF0);REQUIRE(born);
  auto publication=producer;publication.sp-=0xA0;publication.lr=0x816B9E08;
  REQUIRE(ledger.ObserveInlineExecutionPublication(producer,*born,publication,born->task,0x816A2C80,0x404E0C60));
  const auto dispatcher=F(0x816B7DE8,producer.sp-0xA0,0x816B9EA0);
  const auto invocation=ledger.BeginExecutionDispatch(dispatcher,*born);REQUIRE(invocation);
  auto body=dispatcher;body.sp-=0x80;body.lr=loaded_lr;
  REQUIRE(ledger.ExecutionCallbackSelected(invocation,body,born->task,0x816A2C80,0x404E0C60));
  const auto fact=ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp-0x80,born->task);REQUIRE(fact);
  CHECK(fact->callback_lr==0x816B7EA4);CHECK(fact->body_sp==body.sp);CHECK(fact->argument==0x404E0C60);
  body.lr=0x816B7EA4;REQUIRE(ledger.ExecutionCallbackReturned(invocation,body,born->task,0));
  REQUIRE(ledger.ReturnExecutionDispatch(invocation,dispatcher,*born));
  CHECK(ledger.FindLiveTask(parent.task)->birth.generation==parent.generation);
  CHECK(born->origin==1);CHECK_FALSE(ledger.IsProfileTransportSubject(born->generation));
  ExecutionFree(ledger,*born);CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
 }
}
void NativeProfileIdentityTaskTestAccess::ExecutionOpaqueModifyAndActualOperands(){
 Ledger ledger(root);const auto made=ExecutionAllocate(ledger,0x404E0BF0,0,0,true);
 REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0,made.birth.task|1u));
 auto callback=F(0x816B9AA8,0x7080F800,0x90115F24);auto argument=callback;argument.sp-=0x100;argument.thread=5;
 REQUIRE(ledger.BeginExecutionControl(callback,made.birth,1,0x90115AE8,0xBAD));
 REQUIRE(ledger.BeginExecutionControl(argument,made.birth,2,0,UINT32_MAX));
 auto options=callback;options.sp-=0x200;
 REQUIRE(ledger.BeginExecutionControl(options,made.birth,12,1,1)); // unused r5/r6 are not inputs
 const auto dispatcher=F(0x816B8B08,0x7080F000,0x816B94FC);
 const auto invocation=ledger.BeginExecutionDispatch(dispatcher,made.birth);REQUIRE(invocation);
 auto body=dispatcher;body.sp-=0x70;body.lr=0x816B8B48;
 // Actual operands can mix independent native mask1/mask2 stores. Original
 // Task fields are NOT presumed an atomic callback/argument pair.
 REQUIRE(ledger.ExecutionCallbackSelected(invocation,body,made.birth.task,0x90115AE8,0));
 body.lr=0x816B8B64;REQUIRE(ledger.ExecutionCallbackReturned(invocation,body,made.birth.task,7));
 REQUIRE(ledger.ReturnExecutionControl(argument,made.birth));
 REQUIRE(ledger.ReturnExecutionControl(callback,made.birth));
 REQUIRE(ledger.ReturnExecutionControl(options,made.birth));
 REQUIRE(ledger.ReturnExecutionDispatch(invocation,dispatcher,made.birth));
 CHECK_FALSE(ledger.IsProfileTransportSubject(made.birth.generation));
 ExecutionFree(ledger,made.birth);CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ExecutionFreeReuseRetainsOldFrames(){
 Ledger ledger(root);const auto made=ExecutionAllocate(ledger,0x404E0BF0);
 const auto dispatcher=F(0x816B8B08,0x7080F000,0x816B93FC);
 const auto invocation=ledger.BeginExecutionDispatch(dispatcher,made.birth);REQUIRE(invocation);
 auto body=dispatcher;body.sp-=0x70;body.lr=0x816B8B48;
 REQUIRE(ledger.ExecutionCallbackSelected(invocation,body,made.birth.task,0x901159C8,1));
 body.lr=0x816B8B64;REQUIRE(ledger.ExecutionCallbackReturned(invocation,body,made.birth.task,0));
 ExecutionFree(ledger,made.birth); // real postcallback final release precedes older dispatcher return
 const auto fresh=ExecutionAllocate(ledger,made.birth.task,UINT32_MAX,0x90115AE8,false,5);
 REQUIRE(fresh.birth.generation!=made.birth.generation);
 auto wrong=made.birth;wrong.generation=fresh.birth.generation;
 CHECK_FALSE(ledger.ReturnExecutionDispatch(invocation,dispatcher,wrong));
 REQUIRE(ledger.ReturnExecutionDispatch(invocation,dispatcher,made.birth));
 REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));
 REQUIRE(ledger.ReturnExecutionProducer(fresh.producer,fresh.frame,0));
 CHECK(ledger.FindLiveTask(fresh.birth.task)->birth.generation==fresh.birth.generation);
 ExecutionFree(ledger,fresh.birth);CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ExecutionRefusalsStayLocal(){
 Ledger ledger(root);const auto made=ExecutionAllocate(ledger,0x404E0BF0);
 REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));
 const auto dispatcher=F(0x816B8B08,0x7080F000,0x816B93FC);
 const auto invocation=ledger.BeginExecutionDispatch(dispatcher,made.birth);REQUIRE(invocation);
 auto body=dispatcher;body.sp-=0x70;body.lr=0x816B8B48;
 for(unsigned fault=0;fault<6;++fault){CAPTURE(fault);auto wrong=body;auto task=made.birth.task;
  if(fault==0)wrong.root.generation++;if(fault==1)wrong.thread++;if(fault==2)wrong.pcr+=4;
  if(fault==3)wrong.sp-=0x10;if(fault==4)wrong.lr+=4;if(fault==5)task+=0x50;
  CHECK_FALSE(ledger.ExecutionCallbackSelected(invocation,wrong,task,0x901159C8,1));
 }
 REQUIRE(ledger.ExecutionCallbackSelected(invocation,body,made.birth.task,0x901159C8,1));
 auto premature=body;premature.lr=0x816B8B64;
 CHECK_FALSE(ledger.ExecutionCallbackReturned(invocation,body,made.birth.task,0));
 REQUIRE(ledger.ExecutionCallbackReturned(invocation,premature,made.birth.task,0));
 REQUIRE(ledger.ReturnExecutionDispatch(invocation,dispatcher,made.birth));
 // A neutral malformed Close observes no ref/free and must not poison an
 // independently valid profile ledger or consume this actual native birth.
 {Ledger::NeutralObservation local(ledger);auto wrong=F(0x816B7490);wrong.root.generation++;
  CHECK_FALSE(ledger.EnterClose(wrong,made.birth.task));}
 CHECK_FALSE(ledger.lost());CHECK(ledger.FindLiveTask(made.birth.task));
 ExecutionFree(ledger,made.birth);CHECK(ledger.ExecutionClosed());
}
void NativeProfileIdentityTaskTestAccess::ExecutionCanceledSkipAndMissingReturn(){
 Ledger ledger(root);const auto made=ExecutionAllocate(ledger,0x404E0BF0);
 REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));
 const auto skipped=F(0x816B8B08,0x7080F000,0x816BA0DC);
 const auto first=ledger.BeginExecutionDispatch(skipped,made.birth);REQUIRE(first);
 REQUIRE(ledger.ReturnExecutionDispatch(first,skipped,made.birth));
 CHECK_FALSE(ledger.FindLiveTask(made.birth.task)->returned); // no fabricated callback/result
 auto next=skipped;next.lr=0x816B93FC;
 const auto running=ledger.BeginExecutionDispatch(next,made.birth);REQUIRE(running);
 auto body=next;body.sp-=0x70;body.lr=0x816B8B48;
 REQUIRE(ledger.ExecutionCallbackSelected(running,body,made.birth.task,0x901159C8,1));
 const auto caller_close=F(0x816B7490,0x7080D000,0x90117800);
 REQUIRE(ledger.EnterClose(caller_close,made.birth.task));
 REQUIRE(ledger.ReturnClose(caller_close,1)); // real nonfinal caller reference, callback still running
 CHECK(ledger.FindLiveTask(made.birth.task)->execution_active==running);
 CHECK_FALSE(ledger.FindLiveTask(made.birth.task)->freed);
 const auto cancel=F(0x816B9F08,0x7080E000,0x90117810);
 REQUIRE(ledger.BeginExecutionControl(cancel,made.birth));REQUIRE(ledger.ReturnExecutionControl(cancel,made.birth));
 CHECK_FALSE(ledger.ReturnExecutionDispatch(running,next,made.birth)); // Cancel is NOT callback return
 body.lr=0x816B8B64;REQUIRE(ledger.ExecutionCallbackReturned(running,body,made.birth.task,9));
 REQUIRE(ledger.ReturnExecutionDispatch(running,next,made.birth));
 auto wait=F(0x816BA058,0x7080E000,0x90117818);REQUIRE(ledger.BeginExecutionControl(wait,made.birth));
 ExecutionFree(ledger,made.birth);CHECK_FALSE(ledger.ExecutionClosed()); // admitted own-return still retained
 REQUIRE(ledger.ReturnExecutionControl(wait,made.birth));CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ExecutionCommonAndOptionalProfile(){
 for(bool profile_enabled:{false,true}){CAPTURE(profile_enabled);
  Ledger ledger(root);std::optional<Birth> typed;
  if(profile_enabled)typed=Allocate(ledger,101,0x404D0000);
  const auto made=ExecutionAllocate(ledger,0x404E0BF0,1,0x8199C838);
  REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));
  CHECK(made.birth.origin==1);CHECK_FALSE(ledger.IsProfileTransportSubject(made.birth.generation));
  if(typed){
   CHECK(typed->generation!=made.birth.generation);CHECK(ledger.IsProfileTransportSubject(typed->generation));
   const auto entry=F(0x8199C838);REQUIRE(ledger.EnterWork(entry,typed->task,0x92002000));
   REQUIRE(ledger.ReturnWork(entry,*typed));ExecutionFree(ledger,*typed);
  }
  CHECK(ledger.SourceDescendantsClosed(101)); // live generic allocation never pins profile source
  CHECK_FALSE(ledger.ExecutionClosed());ExecutionFree(ledger,made.birth);CHECK(ledger.ExecutionClosed());
 }
}
void NativeProfileIdentityTaskTestAccess::ExecutionReadCellNestedRestoration(){
 auto owner=std::make_shared<int>(42);std::atomic<void*> cell{owner.get()};
 const auto origin=F(0x8199E148,0x70810000,0x816B8B64);
 const auto wait=F(0x816BA058,origin.sp-0xD0,0x8199E314);
 const auto dispatcher=F(0x816B8B08,wait.sp-0x80,0x816BA0DC);
 Ledger::ExecutionReadCell child,parent;
 REQUIRE(child.Suspend(origin,dispatcher,owner,owner,owner.get(),cell));CHECK(cell.load()==nullptr);
 // An independent actual bounded scope may enter/leave inside the neutral
 // callback. This metadata test does not mint a roster read ticket.
 int nested=0;cell.store(&nested);cell.store(nullptr);
 REQUIRE(child.TransferInlineWait(dispatcher,wait,parent));CHECK(cell.load()==nullptr);
 CHECK(child.phase()==Ledger::ExecutionReadCell::Phase::kForwarded);
 CHECK_FALSE(parent.Restore(dispatcher,owner.get(),cell));
 REQUIRE(parent.Restore(wait,owner.get(),cell));CHECK(cell.load()==owner.get());cell.store(nullptr);
 Ledger::ExecutionReadCell inline_scope;const auto inlined=F(0x816B7DE8,origin.sp-0x170,0x816B9EA0);
 cell.store(owner.get());REQUIRE(inline_scope.Suspend(origin,inlined,owner,owner,owner.get(),cell));
 auto callback_return=inlined;callback_return.sp-=0x80;callback_return.lr=0x816B7EA4;
 CHECK_FALSE(inline_scope.Restore(callback_return,owner.get(),cell)); // genuine callback return is still too early
 REQUIRE(inline_scope.Restore(inlined,owner.get(),cell));cell.store(nullptr);
}
void NativeProfileIdentityTaskTestAccess::ExecutionReadCellRetainsActualOwner(){
 unsigned destroyed=0;bool under_lock=false,destroyed_under_lock=false;
 struct Owner{
  Owner(unsigned* d,bool* locked,bool* b):destroyed(d),under_lock(locked),bad(b){}
  unsigned* destroyed;bool* under_lock;bool* bad;~Owner(){++*destroyed;*bad=*under_lock;}
 };
 std::atomic<void*> cell;const auto origin=F(0x8199E148,0x70810000,0x816B8B64);
 const auto invocation=F(0x816B7DE8,origin.sp-0x170,0x816B9EA0);
 {
  Ledger::ExecutionReadCell scope;auto owner=std::make_shared<Owner>(&destroyed,&under_lock,&destroyed_under_lock);
  void* pointer=owner.get();cell.store(pointer);under_lock=true;
  REQUIRE(scope.Suspend(origin,invocation,owner,owner,pointer,cell));owner.reset();
  CHECK(destroyed==0);REQUIRE(scope.Restore(invocation,pointer,cell));CHECK(destroyed==0);
  cell.store(nullptr); // original bounded scope's eventual Leave
  under_lock=false;
 }
 CHECK(destroyed==1);CHECK_FALSE(destroyed_under_lock);
}
void NativeProfileIdentityTaskTestAccess::ExecutionReadCellWrongFramesAndAbandonment(){
 auto owner=std::make_shared<int>(42);std::atomic<void*> cell{owner.get()};
 const auto origin=F(0x8199E148,0x70810000,0x816B8B64);
 const auto invocation=F(0x816B7DE8,origin.sp-0x170,0x816B9EA0);
 Ledger::ExecutionReadCell scope;
 auto wrong=origin;wrong.thread++;
 CHECK_FALSE(scope.Suspend(wrong,invocation,owner,owner,owner.get(),cell));CHECK(cell.load()==owner.get());
 auto impostor=std::shared_ptr<void>(owner.get(),[](void*){});
 CHECK_FALSE(scope.Suspend(origin,invocation,impostor,owner,owner.get(),cell));CHECK(cell.load()==owner.get());
 REQUIRE(scope.Suspend(origin,invocation,owner,owner,owner.get(),cell));
 int inner=0;cell.store(&inner);CHECK_FALSE(scope.Restore(invocation,owner.get(),cell));CHECK(cell.load()==&inner);
 CHECK(scope.phase()==Ledger::ExecutionReadCell::Phase::kUncertain);
 CHECK_FALSE(scope.RestoreAtActualStop(owner.get(),cell)); // cannot clobber a real deeper scope
 cell.store(nullptr); // reverse real scope detachment at actual CPU-stop
 REQUIRE(scope.RestoreAtActualStop(owner.get(),cell));CHECK(cell.load()==owner.get());cell.store(nullptr);
 Ledger::ExecutionReadCell delegated;cell.store(owner.get());
 REQUIRE(delegated.Suspend(origin,invocation,owner,owner,owner.get(),cell));
 REQUIRE(delegated.DelegateToOrigin(owner.get(),owner.get()));CHECK(cell.load()==nullptr);
 CHECK(delegated.phase()==Ledger::ExecutionReadCell::Phase::kForwarded);
 // The real originating Work owns restore+immediate Leave; delegation itself
 // neither writes the cell nor reports native completion.
 cell.store(owner.get());cell.store(nullptr);
}
void NativeProfileIdentityTaskTestAccess::ExecutionUnavailableOverflowCannotBorrow(){
 Ledger::ExecutionUnavailableFrames journal;
 const auto candidate=F(0x816B8B08,0x70810000,0x816B93FC);
 const uint32_t current_sp=candidate.sp-0x200;
 CHECK_FALSE(journal.Shadows(root,candidate.thread,candidate.pcr,current_sp,candidate.sp));
 auto younger=candidate;younger.function=0x816B7DE8;younger.sp-=0x100;
 journal.Record(younger);
 REQUIRE(journal.Shadows(root,candidate.thread,candidate.pcr,current_sp,candidate.sp));
 REQUIRE(journal.Return(younger));
 CHECK_FALSE(journal.Shadows(root,candidate.thread,candidate.pcr,current_sp,candidate.sp));
 // Fill the SAME production fixed scratch with other actual frame identities.
 // A younger unrecordable frame must deny the older candidate, even when no
 // stored row belongs to that candidate's thread/Task.
 for(uint32_t n=0;n<Ledger::ExecutionUnavailableFrames::kCapacity;++n){
  auto foreign=candidate;foreign.thread=10+n;journal.Record(foreign);
 }
 CHECK_FALSE(journal.Shadows(root,candidate.thread,candidate.pcr,current_sp,candidate.sp));
 journal.Record(younger);
 REQUIRE(journal.Shadows(root,candidate.thread,candidate.pcr,current_sp,candidate.sp));
 CHECK_FALSE(journal.Return(younger)); // no invented return for the lost cut
 for(uint32_t n=0;n<Ledger::ExecutionUnavailableFrames::kCapacity;++n){
  auto foreign=candidate;foreign.thread=10+n;REQUIRE(journal.Return(foreign));
 }
 CHECK(journal.Shadows(root,candidate.thread,candidate.pcr,current_sp,candidate.sp));
 // Only factual owner retirement/replacement creates fresh empty scratch.
 Ledger::ExecutionUnavailableFrames next_owner;
 CHECK_FALSE(next_owner.Shadows(root,candidate.thread,candidate.pcr,current_sp,candidate.sp));
}
TEST_CASE("Unavailable native Task frame storage cannot expose an older execution capsule", "[identity_task][execution][failure][capacity]"){
 NativeProfileIdentityTaskTestAccess::ExecutionUnavailableOverflowCannotBorrow();
}
void NativeProfileIdentityTaskTestAccess::ExecutionReadFallbackOwningBoundaries(){
 // These are the production CAS/obligation methods called after the genuine
 // ordinary-return or CD920 owner has authenticated its actual boundary.
 // They do not fabricate a native scope, actual CPU-stop or roster authority.
 int original=1,deeper=2;std::atomic<void*> cell{nullptr};bool fallback=true;
 REQUIRE(Ledger::RestoreReadCellForScopeLeave(fallback,&original,cell));
 CHECK_FALSE(fallback);CHECK(cell.load()==&original);
 // Already settled restoration must not mutate a subsequently nested cell.
 cell.store(&deeper);REQUIRE(Ledger::RestoreReadCellForScopeLeave(fallback,&original,cell));
 CHECK(cell.load()==&deeper);
 // A deeper retained actual scope is never clobbered or called a Leave.
 fallback=true;CHECK_FALSE(Ledger::RestoreReadCellForScopeLeave(fallback,&original,cell));
 CHECK(fallback);CHECK(cell.load()==&deeper);
 CHECK_FALSE(Ledger::RestoreReadCellForScopeLeave(fallback,nullptr,cell));CHECK(fallback);
 // Once the independently owned deeper scope really withdrew, the SAME
 // origin obligation restores exactly once at its actual owning exit.
 cell.store(nullptr);REQUIRE(Ledger::RestoreReadCellForScopeLeave(fallback,&original,cell));
 CHECK_FALSE(fallback);CHECK(cell.load()==&original);
 // A missing deeper own-return is first suppressed through the SAME actual
 // retained-owner helper. Keep the old origin suppressed too: making it
 // ambient before the deeper owner retires would invert the reverse CAS.
 auto owner=std::make_shared<int>(3);const auto origin=F(0x816CD6E8,0x70810000,0x816CD708);
 const auto child=F(0x816B7DE8,origin.sp-0x170,0x816B9EA0);
 cell.store(&original);Ledger::ExecutionReadCell handoff;
 REQUIRE(handoff.Suspend(origin,child,owner,owner,&original,cell));
 cell.store(&deeper);bool deeper_fallback=false,old_fallback=true;
 REQUIRE(Ledger::SuppressReadCellForScopeLeave(deeper_fallback,&deeper,cell));
 REQUIRE(handoff.DelegateToOrigin(owner.get(),&original));CHECK(cell.load()==nullptr);
 REQUIRE(Ledger::RestoreReadCellForScopeLeave(deeper_fallback,&deeper,cell));
 CHECK(cell.load()==&deeper);cell.store(nullptr); // fixture withdrawal; no native Scope::Leave is claimed
 REQUIRE(Ledger::RestoreReadCellForScopeLeave(old_fallback,&original,cell));
 CHECK(cell.load()==&original);CHECK_FALSE(deeper_fallback);CHECK_FALSE(old_fallback);
}
TEST_CASE("Native read fallback restoration is shared by ordinary and render owning exits", "[identity_task][execution][read_scope][boundary]"){
 NativeProfileIdentityTaskTestAccess::ExecutionReadFallbackOwningBoundaries();
}
TEST_CASE("Common native Task metadata records opaque callback execution before Schedule return", "[identity_task][execution][abi]"){
 NativeProfileIdentityTaskTestAccess::ExecutionBeforeProducerReturn();
}
TEST_CASE("Common inline Task execution observes its real child birth and native loaded operands", "[identity_task][execution][inline]"){
 NativeProfileIdentityTaskTestAccess::ExecutionInlineUsesOriginalChildBirth();
}
TEST_CASE("Common Task Modify preserves independent fields and actual loaded callback operands", "[identity_task][execution][modify]"){
 NativeProfileIdentityTaskTestAccess::ExecutionOpaqueModifyAndActualOperands();
}
TEST_CASE("Common Task producer and dispatcher retain old births through original free and reuse", "[identity_task][execution][lifetime]"){
 NativeProfileIdentityTaskTestAccess::ExecutionFreeReuseRetainsOldFrames();
}
TEST_CASE("Neutral Task metadata refusals preserve unrelated profile state and pending receipts", "[identity_task][execution][failure]"){
 NativeProfileIdentityTaskTestAccess::ExecutionRefusalsStayLocal();
}
TEST_CASE("Common Task canceled skip and running cancellation do not fabricate callback completion", "[identity_task][execution][cancel]"){
 NativeProfileIdentityTaskTestAccess::ExecutionCanceledSkipAndMissingReturn();
}
TEST_CASE("One common Task ledger works with and without optional profile source bindings", "[identity_task][execution][roles]"){
 NativeProfileIdentityTaskTestAccess::ExecutionCommonAndOptionalProfile();
}
TEST_CASE("Neutral inline Task read isolation waits for genuine parent context restoration", "[identity_task][execution][read_scope]"){
 NativeProfileIdentityTaskTestAccess::ExecutionReadCellNestedRestoration();
}
TEST_CASE("Neutral Task read isolation retains its actual scope owner until host retirement", "[identity_task][execution][read_scope][lifetime]"){
 NativeProfileIdentityTaskTestAccess::ExecutionReadCellRetainsActualOwner();
}
TEST_CASE("Neutral Task scope abandonment preserves exact nested cells until actual stop", "[identity_task][execution][read_scope][failure]"){
 NativeProfileIdentityTaskTestAccess::ExecutionReadCellWrongFramesAndAbandonment();
}
TEST_CASE("Native encoded image use requires the exact original allocation and publication", "[identity_image][encoded][lifetime]"){
 constexpr identity_detail::NativeProfileIdentityTasks::Root root{31,37,41};
 identity_detail::NativeEncodedImageEpoch epoch;
 REQUIRE(epoch.Allocate(root,7,0x92001000,0x1000));
 const auto allocation=epoch.allocation();const auto publication=epoch.publication();
 const auto admitted=epoch.Capture(7,3,17,522,allocation,publication);
 REQUIRE(admitted);CHECK(epoch.Current(*admitted));
 CHECK_FALSE(epoch.Capture(8,3,17,522,allocation,publication));
 CHECK_FALSE(epoch.Capture(7,0,17,522,allocation,publication));
 CHECK_FALSE(epoch.Capture(7,3,17,0,allocation,publication));
 CHECK_FALSE(epoch.Capture(7,3,17,0x1001,allocation,publication));
 CHECK_FALSE(epoch.Capture(7,3,17,522,allocation+1,publication));
 CHECK_FALSE(epoch.Capture(7,3,17,522,allocation,publication+1));
 auto wrong_root=*admitted;++wrong_root.root.generation;
 CHECK_FALSE(epoch.Current(wrong_root));
 CHECK_FALSE(epoch.FinishOriginalDecode(*admitted,admitted->address+1,admitted->size,true));
 CHECK_FALSE(epoch.FinishOriginalDecode(*admitted,admitted->address,admitted->size-1,true));
 CHECK_FALSE(epoch.FinishOriginalDecode(*admitted,admitted->address,admitted->size,false));
 epoch.Invalidate();
 CHECK_FALSE(epoch.Current(*admitted));
 CHECK_FALSE(epoch.FinishOriginalDecode(*admitted,admitted->address,admitted->size,true));
}
TEST_CASE("Original copied image lineage survives later provider buffer release and address reuse", "[identity_image][encoded][reuse]"){
 constexpr identity_detail::NativeProfileIdentityTasks::Root root{31,37,41};
 identity_detail::NativeEncodedImageEpoch epoch;
 REQUIRE(epoch.Allocate(root,7,0x92001000,0x1000));
 const auto borrowed=epoch.Capture(7,3,17,522,epoch.allocation(),epoch.publication());
 REQUIRE(borrowed);
 const auto copied=epoch.FinishOriginalDecode(*borrowed,borrowed->address,borrowed->size,true);
 REQUIRE(copied);
 epoch.Revoke();CHECK_FALSE(epoch.Current(*borrowed));
 REQUIRE(epoch.Allocate(root,7,borrowed->address,0x1000));
 const auto following=epoch.Capture(7,4,18,747,epoch.allocation(),epoch.publication());
 REQUIRE(following);CHECK_FALSE(epoch.Current(*borrowed));
 CHECK(copied->encoded==*borrowed);CHECK(copied->encoded.source==3);
 CHECK(copied->encoded.allocation!=following->allocation);
 identity_detail::NativeEncodedImageEpoch replaced;
 auto new_root=root;++new_root.generation;
 REQUIRE(replaced.Allocate(new_root,7,borrowed->address,0x1000));
 CHECK_FALSE(replaced.Current(*borrowed));CHECK(copied->encoded==*borrowed);
}
TEST_CASE("Covered repeated publication attempts never relearn an earlier native image receipt", "[identity_image][encoded][publication]"){
 constexpr identity_detail::NativeProfileIdentityTasks::Root root{31,37,41};
 identity_detail::NativeEncodedImageEpoch epoch;
 REQUIRE(epoch.Allocate(root,7,0x92001000,0x1000));
 const auto first=epoch.Capture(7,3,17,522,epoch.allocation(),epoch.publication());
 REQUIRE(first);
 for(uint32_t attempt=0;attempt<1024;++attempt){
  CAPTURE(attempt);epoch.Invalidate();CHECK_FALSE(epoch.Current(*first));
  CHECK_FALSE(epoch.Capture(7,3,17,522,first->allocation,first->publication));
 }
 CHECK_FALSE(epoch.Capture(7,4,18,522,first->allocation,first->publication));
}
TEST_CASE("Shared CompleteIO routes ordinary SYSTEM calls without adopting a profile job", "[identity_task][completion_route]") {
 using L=identity_detail::NativeProfileIdentityTasks;using Route=L::CompletionRoute;
 const auto frame=NativeProfileIdentityTaskTestAccess::F(0x816A2588,0x7038F950,0x916D4810);
 auto job=std::make_shared<int>(1);
 for(uint32_t mode=0;mode<4;++mode){
  auto route=Route::Capture(frame,mode==0?nullptr:job,7,9,mode==1?0:0x404E0A10,
      mode==2?0:mode==3?0x404E0B10:0x404E0A10);
  REQUIRE(route.kind()==Route::Kind::kOrdinary);CHECK(route.FrameMatches(frame));
  CHECK_FALSE(route.StillOwned(job,7,9,0x404E0A10,0x404E0A10));
  REQUIRE(route.ReturnOrdinary(frame));CHECK_FALSE(route.FrameMatches(frame));
  CHECK_FALSE(route.ReturnOrdinary(frame)); // own-return replay cannot end another call
 }
}
TEST_CASE("Claimed CompleteIO pins exact job control block source serial and overlap", "[identity_task][completion_route][authority]") {
 using L=identity_detail::NativeProfileIdentityTasks;using Route=L::CompletionRoute;
 const auto frame=NativeProfileIdentityTaskTestAccess::F(0x816A2588);
 auto job=std::make_shared<int>(1);std::weak_ptr<int> weak=job;
 {
  auto route=Route::Capture(frame,job,7,9,0x404E0A10,0x404E0A10);
  REQUIRE(route.kind()==Route::Kind::kIdentity);CHECK(route.StillOwned(job,7,9,0x404E0A10,0x404E0A10));
  auto equal=std::make_shared<int>(1);CHECK_FALSE(route.StillOwned(equal,7,9,0x404E0A10,0x404E0A10));
  std::shared_ptr<const void> forged(job.get(),[](const void*){});
  CHECK_FALSE(route.StillOwned(forged,7,9,0x404E0A10,0x404E0A10)); // same address, different ownership
  CHECK_FALSE(route.StillOwned(job,8,9,0x404E0A10,0x404E0A10));
  CHECK_FALSE(route.StillOwned(job,7,10,0x404E0A10,0x404E0A10));
  CHECK_FALSE(route.StillOwned(job,7,9,0x404E0B10,0x404E0A10));
  CHECK_FALSE(route.StillOwned(job,7,9,0x404E0A10,0x1404E0A10ull));
  job.reset();CHECK_FALSE(weak.expired()); // admission memento retains actual old owner
 }
 CHECK(weak.expired());
 auto owner=std::make_shared<int>(2);
 // Native32 high pointers may be zero- or sign-extended, exactly as DwordIs.
 auto high=Route::Capture(frame,owner,7,9,0xC04E0A10,0xFFFFFFFFC04E0A10ull);
 REQUIRE(high.kind()==Route::Kind::kIdentity);
 CHECK(high.StillOwned(owner,7,9,0xC04E0A10,0xC04E0A10));
 CHECK(high.StillOwned(owner,7,9,0xC04E0A10,0xFFFFFFFFC04E0A10ull));
 CHECK_FALSE(high.StillOwned(owner,7,9,0xC04E0A10,0x1C04E0A10ull));
 CHECK(Route::Capture(frame,owner,7,9,0xC04E0A10,0x1C04E0A10ull).kind()==Route::Kind::kInvalid);
 CHECK(Route::Capture(frame,owner,7,9,0x404E0A10,0x1404E0A10ull).kind()==Route::Kind::kInvalid);
 CHECK(Route::Capture(frame,owner,0,9,0x404E0A10,0x404E0A10).kind()==Route::Kind::kInvalid);
 CHECK(Route::Capture(frame,owner,7,0,0x404E0A10,0x404E0A10).kind()==Route::Kind::kInvalid);
}
TEST_CASE("Nested ordinary completion frames cannot consume or retag older identity returns", "[identity_task][completion_route][lifetime]") {
 using L=identity_detail::NativeProfileIdentityTasks;using Route=L::CompletionRoute;
 const auto outer=NativeProfileIdentityTaskTestAccess::F(0x816A2588);
 auto inner=outer;inner.sp-=0xA0;inner.lr=0x916D4810;
 auto job=std::make_shared<int>(1);auto identity=Route::Capture(outer,job,7,9,0x404E0A10,0x404E0A10);
 auto ordinary=Route::Capture(inner,job,7,9,0x404E0A10,0x404E0B10);
 REQUIRE(identity.kind()==Route::Kind::kIdentity);REQUIRE(ordinary.kind()==Route::Kind::kOrdinary);
 // Production return lookup considers only the youngest same-function entry.
 CHECK_FALSE(ordinary.FrameMatches(outer));CHECK(identity.FrameMatches(outer));
 REQUIRE(ordinary.ReturnOrdinary(inner));CHECK(identity.StillOwned(job,7,9,0x404E0A10,0x404E0A10));
 for(uint32_t mode=0;mode<6;++mode){
  auto retained=Route::Capture(inner,job,7,9,0x404E0A10,0x404E0B10);auto wrong=inner;
  if(mode==0)++wrong.root.generation;if(mode==1)++wrong.thread;if(mode==2)wrong.pcr+=0x1000;
  if(mode==3)wrong.sp-=0x10;if(mode==4)wrong.lr+=4;if(mode==5)wrong.function+=4;
  CHECK_FALSE(retained.ReturnOrdinary(wrong));CHECK_FALSE(retained.ReturnOrdinary(inner));
  CHECK(identity.StillOwned(job,7,9,0x404E0A10,0x404E0A10));
 }
}

namespace {
using ContextRef=identity_detail::NativeProfileIdentityTasks::ContextReferenceReceipt;
using ContextOwnedRef=identity_detail::NativeProfileIdentityTasks::ReferenceReceipt;
using ContextFrame=identity_detail::NativeProfileIdentityTasks::Frame;
ContextFrame ContextEntry(uint32_t worker=9,uint32_t sp=0x707E0100) {
  auto f=NativeProfileIdentityTaskTestAccess::F(0x816BA3C0,sp,0x816B8B64);
  f.thread=worker;f.pcr+=worker*0x1000;return f;
}
ContextFrame ContextBody(const ContextRef& ref,uint32_t function=0x816BA3C0,
                         uint32_t lr=0x816BA3D4) {
  auto f=ref.wrapper;f.sp-=0x70;f.function=function;f.lr=lr;return f;
}
void ContextHeld(ContextRef& ref,uint64_t serial=1,ContextFrame entry=ContextEntry()) {
  REQUIRE(ref.Prepare(serial,entry,0x40602000,entry.sp-0x10000,entry.sp+0x10000));
  const auto getter=ContextBody(ref,0x816B7F30,0x816BA3D4);
  REQUIRE(ref.Getter(getter,false,0));REQUIRE(ref.AdmitGetter(getter));
  REQUIRE(ref.Getter(getter,true,0x401708D0));
}
void ContextCallback(ContextRef& ref,uint32_t site,uint64_t result=0) {
  auto body=ContextBody(ref);body.lr=(site<2)?0x816BA3E8:0x816BA424;
  REQUIRE(ref.Callback(site,body,ref.task,ref.context,
      site<2?0x81965798:0x819657E0,result));
}
ContextFrame ContextCaller(const ContextRef& ref) {
  auto f=ref.callback;f.sp-=0xA0;f.function=0x81964C78;f.lr=0x819651E0;return f;
}
void ContextFinished(ContextRef& ref,uint64_t count=0) {
  const auto close=ContextBody(ref,0x816B7490,0x816BA430);
  REQUIRE(ref.Decrement(close,false,ref.task));REQUIRE(ref.AdmitDecrement(close));
  REQUIRE(ref.Decrement(close,true,count));REQUIRE(ref.Return(ref.wrapper));
}
}
TEST_CASE("context reference observes native getter body and exact own return",
          "[identity_task][task_context_reference][lifetime]") {
  ContextRef ref;const auto entry=ContextEntry();
  REQUIRE(ref.Prepare(1,entry,0x40602000,entry.sp-0x10000,entry.sp+0x10000));
  const auto getter=ContextBody(ref,0x816B7F30,0x816BA3D4);
  REQUIRE(ref.Getter(getter,false,0));CHECK_FALSE(ref.Getter(getter,true,0x401708D0));
  CHECK(ref.phase==ContextRef::Phase::kUncertain);
  for(uint32_t field=0;field<6;++field){
    ContextRef wrong;REQUIRE(wrong.Prepare(2,entry,0x40602000,entry.sp-0x10000,entry.sp+0x10000));
    auto f=ContextBody(wrong,0x816B7F30,0x816BA3D4);
    switch(field){case 0:++f.root.generation;break;case 1:++f.thread;break;
      case 2:f.pcr+=0x1000;break;case 3:f.sp-=0x10;break;case 4:f.lr+=4;break;
      default:f.function+=4;break;}
    CHECK_FALSE(wrong.Getter(f,false,0));
  }
  for(uint64_t value:{0ull,0x401708D1ull,0x1401708D0ull}){
    ContextRef bad;REQUIRE(bad.Prepare(3,entry,0x40602000,entry.sp-0x10000,entry.sp+0x10000));
    const auto f=ContextBody(bad,0x816B7F30,0x816BA3D4);
    REQUIRE(bad.Getter(f,false,0));REQUIRE(bad.AdmitGetter(f));CHECK_FALSE(bad.Getter(f,true,value));
  }
  ContextRef good;ContextHeld(good);CHECK(good.phase==ContextRef::Phase::kHeld);
  CHECK_FALSE(good.Permits(ContextBody(good),good.task));
}
TEST_CASE("fresh context callbacks close only the separately held host reference",
          "[identity_task][task_context_reference][lifetime]") {
  for(uint32_t phase:{0u,2u}){
    ContextRef native;ContextHeld(native);ContextCallback(native,0);
    if(phase==2){ContextCallback(native,1);ContextCallback(native,2);}
    ContextOwnedRef host;NativeProfileIdentityTaskTestAccess::AcquireReference(
        host,41+phase,NativeProfileIdentityTaskTestAccess::Callback());
    const auto current=ContextCaller(native);
    REQUIRE(current.thread!=host.callback.thread);REQUIRE(native.callback.function!=host.callback.function);
    CHECK_FALSE(native.Permits(current,host.task+4));
    CHECK_FALSE(host.BeginContextRelease(current,native,host.task+4));
    CHECK(host.state()==ContextOwnedRef::Phase::kHeld);
    REQUIRE(native.Permits(current,host.task));REQUIRE(host.BeginContextRelease(current,native,host.task));
    REQUIRE(host.EnterRelease(host.release));REQUIRE(host.ReturnRelease(host.release,1));
    REQUIRE(host.CompleteRelease(true,1));CHECK_FALSE(host.BeginContextRelease(current,native,host.task));
    CHECK(native.Permits(current,host.task)); // wrapper's extra native ref is independent
    ContextCallback(native,phase+1);if(phase==0){ContextCallback(native,2);ContextCallback(native,3);}
    ContextFinished(native);CHECK(native.finished());
  }
}
TEST_CASE("pending context resumption uses a fresh worker acquisition",
          "[identity_task][task_context_reference][lifetime]") {
  ContextOwnedRef host;NativeProfileIdentityTaskTestAccess::AcquireReference(
      host,71,NativeProfileIdentityTaskTestAccess::Callback());
  ContextRef first;ContextHeld(first,11);ContextCallback(first,0);ContextCallback(first,1,0x8000000A);
  CHECK_FALSE(first.Permits(ContextCaller(first),host.task));ContextFinished(first);
  ContextRef resumed;ContextHeld(resumed,12,ContextEntry(17,0x706E0100));ContextCallback(resumed,0);
  ContextCallback(resumed,1);ContextCallback(resumed,2);
  const auto current=ContextCaller(resumed);CHECK_FALSE(first.Permits(current,host.task));
  REQUIRE(host.BeginContextRelease(current,resumed,host.task));REQUIRE(host.EnterRelease(host.release));
  REQUIRE(host.ReturnRelease(host.release,2));REQUIRE(host.CompleteRelease(true,2));
  ContextCallback(resumed,3);ContextFinished(resumed);CHECK(resumed.finished());
}
TEST_CASE("innermost uncertain context shadows an older native reference",
          "[identity_task][task_context_reference][lifetime]") {
  ContextRef outer;ContextHeld(outer,21);ContextCallback(outer,0);
  ContextRef inner;auto entry=outer.wrapper;entry.sp-=0x200;
  REQUIRE(inner.Prepare(22,entry,0x40603000,entry.sp-0x10000,entry.sp+0x10000));
  CHECK(inner.NewerActiveThan(&outer));CHECK_FALSE(inner.Permits(ContextCaller(outer),outer.task));
  inner.Fail();CHECK(inner.NewerActiveThan(&outer));
  ContextRef complete;ContextHeld(complete,23,entry);ContextCallback(complete,0);
  ContextCallback(complete,1,0x8000000A);ContextFinished(complete);
  CHECK_FALSE(complete.NewerActiveThan(&outer));CHECK(outer.Permits(ContextCaller(outer),outer.task));
  auto wrong=ContextCaller(outer);++wrong.root.generation;CHECK_FALSE(outer.Permits(wrong,outer.task));
  wrong=ContextCaller(outer);++wrong.thread;CHECK_FALSE(outer.Permits(wrong,outer.task));
  wrong=ContextCaller(outer);wrong.pcr+=0x1000;CHECK_FALSE(outer.Permits(wrong,outer.task));
  wrong=ContextCaller(outer);wrong.sp=outer.limit-0x10;CHECK_FALSE(outer.Permits(wrong,outer.task));
}
TEST_CASE("context decrement revokes before body and needs actual completion",
          "[identity_task][task_context_reference][lifetime]") {
  for(bool malformed:{false,true}){
    ContextRef ref;ContextHeld(ref);ContextCallback(ref,0);ContextCallback(ref,1,0x8000000A);
    auto close=ContextBody(ref,0x816B7490,0x816BA430);if(malformed)close.sp-=0x10;
    CHECK(ref.Decrement(close,false,ref.task)==!malformed);
    CHECK_FALSE(ref.Permits(ContextCaller(ref),ref.task));
    CHECK_FALSE(ref.Decrement(close,true,0));CHECK_FALSE(ref.Return(ref.wrapper));
  }
  ContextRef good;ContextHeld(good);ContextCallback(good,0);ContextCallback(good,1,0x8000000A);
  ContextFinished(good,0);CHECK(good.finished()); // native wrapper may own the final reference
}
TEST_CASE("context baseline cannot excuse failed nonfinal host close or replay",
          "[identity_task][task_context_reference][lifetime]") {
  ContextRef native;ContextHeld(native);ContextCallback(native,0);const auto current=ContextCaller(native);
  for(uint64_t result:{0ull,0x8007139Full,0x100000001ull}){
    ContextOwnedRef host;NativeProfileIdentityTaskTestAccess::AcquireReference(
        host,90+result,NativeProfileIdentityTaskTestAccess::Callback());
    REQUIRE(host.BeginContextRelease(current,native,host.task));REQUIRE(host.EnterRelease(host.release));
    CHECK_FALSE(host.ReturnRelease(host.release,result));CHECK_FALSE(host.CompleteRelease(true,uint32_t(result)));
  }
  ContextCallback(native,1,0x8000000A);ContextFinished(native);
  ContextOwnedRef late;NativeProfileIdentityTaskTestAccess::AcquireReference(
      late,99,NativeProfileIdentityTaskTestAccess::Callback());
  CHECK_FALSE(late.BeginContextRelease(current,native,late.task));
}

} // namespace xe::kernel::test

namespace xe::kernel::test {
NativeProfileIdentityTaskTestAccess::Birth
NativeProfileIdentityTaskTestAccess::ProfileAllocateWithOutput(
    Ledger& ledger,Frame owner,uint64_t serial,uint32_t callback,uint32_t task,
    Ledger::ProfileOutput output_proof,bool finish,uint64_t* captured_call){
 const auto body=owner.sp-Ledger::ProfileProducerFrameBytes(owner.function);
 const auto schedule=F(0x816BA440,body,owner.function==0x8199E148?0x8199E248:0x8199F01C);
 const uint32_t options=body+(owner.function==0x8199E148?0x58:0x70);
 const uint32_t output=output_proof.kind==Ledger::ProfileOutput::Kind::kAsyncPacket?
     output_proof.packet+0x18:body+(owner.function==0x8199E148?0x50:0x5C);
 const auto call=ledger.BeginSchedule(schedule,101,callback,0x40170B40,options,output);REQUIRE(call);
 if(captured_call)*captured_call=call;
 const std::array<uint32_t,2> supplied{owner.function==0x8199E148?0x14000000u:0x10000000u,0x401E6982};
 REQUIRE(ledger.BindProfileProducer(call,serial,owner,supplied,output_proof));
 const auto allocation=F(0x816B9A18,body-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(call,allocation,options,allocation.sp+0x50));
 const auto born=ledger.ReturnAllocation(call,allocation,0,task);REQUIRE(born);
 const auto publication=F(0x816B9BB8,body-0x90,0x816BA4D8);
 REQUIRE(ledger.BeforePublication(call,publication,task,callback,0x40170B40,task|1u));
 REQUIRE(ledger.BindProfileNativeState(call,task,0x14A00004,0x401E6982));
 if(finish)REQUIRE(ledger.ReturnSchedule(call,schedule,0,std::array<uint32_t,2>{0x14A00004,0x401E6982}));
 return *born;
}
void NativeProfileIdentityTaskTestAccess::ProfileCallbackThenFree(
    Ledger& ledger,Birth born,Frame callback){
 const auto* node=ledger.FindBirth(born.generation);REQUIRE(node);
 REQUIRE(ledger.EnterWork(callback,born.task,node->argument));
 REQUIRE(ledger.ReturnWork(callback,born));
 Ledger::NeutralObservation local(ledger);
 auto close=callback;close.function=0x816B7490;close.sp-=0x200;close.lr=0x816B8D94;
 REQUIRE(ledger.EnterClose(close,born.task));
 auto deleting=close;deleting.function=0x816B7D98;deleting.sp-=0x60;deleting.lr=0x816B74E8;
 REQUIRE(ledger.EnterDeletingDestructor(deleting,born.task,1));
 auto freeing=deleting;freeing.function=0x816E6AB0;freeing.sp-=0x70;freeing.lr=0x816B7DC8;
 REQUIRE(ledger.EnterTaskFree(freeing,born.task));REQUIRE(ledger.ReturnTaskFree(freeing));
 REQUIRE(ledger.ReturnDeletingDestructor(deleting));REQUIRE(ledger.ReturnClose(close,0));
}
void NativeProfileIdentityTaskTestAccess::ProfileOutputStackAndPacketAbi(){
 for(const auto function:{0x8199E3E0u,0x8199E148u}){
  const auto owner=F(function,0x707DFCF0,0x90103544);
  const auto body=owner.sp-Ledger::ProfileProducerFrameBytes(function);
  const auto schedule=F(0x816BA440,body,function==0x8199E3E0?0x8199F01C:0x8199E248);
  const auto output=body+(function==0x8199E3E0?0x5C:0x50);
  const auto proof=Ledger::CaptureProfileOutput(owner,schedule,output,0,0,{});REQUIRE(proof);
  CHECK(proof->kind==Ledger::ProfileOutput::Kind::kStack);CHECK(proof->packet==0);CHECK(proof->overlap==0);
  CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,output+4,0,0,{}));
  CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,output,0,0x40172000,{}));
  CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,output,0,0,{0xFE,0,0}));
  Ledger ledger(root);const auto born=ProfileAllocate(ledger,owner,811,0x8199C838,0x40171770);
  const auto* producer=ledger.ProfileProducerFor(born);REQUIRE(producer);
  CHECK(producer->output_proof.kind==Ledger::ProfileOutput::Kind::kStack);
  CHECK(producer->output==output);CHECK(producer->schedule_returned);
 }
 const auto owner=F(0x8199E3E0,0x707DFCF0,0x90103544);
 const auto schedule=F(0x816BA440,owner.sp-0x190,0x8199F01C);
 const uint32_t packet=0x40172000,overlap=0x40173000;
 const auto proof=Ledger::CaptureProfileOutput(owner,schedule,packet+0x18,overlap,packet,{0xFE,overlap,packet});REQUIRE(proof);
 CHECK(proof->kind==Ledger::ProfileOutput::Kind::kAsyncPacket);CHECK(proof->packet==packet);CHECK(proof->overlap==overlap);
 // The native packet output is intentionally outside the producer's stack.
 CHECK(packet+0x18<schedule.sp);
 for(const auto callback:{0x8199C838u,0x8199E148u}){
  Ledger ledger(root);const auto born=ProfileAllocateWithOutput(ledger,owner,812,callback,0x40171770,*proof,true,nullptr);
  const auto* producer=ledger.ProfileProducerFor(born);REQUIRE(producer);
  CHECK(producer->callback==callback);CHECK(producer->output_proof==*proof);
  CHECK(producer->output==packet+0x18);CHECK(producer->schedule_returned);
 }
}
void NativeProfileIdentityTaskTestAccess::ProfileOutputRejectsMalformedProvenance(){
 const auto owner=F(0x8199E3E0,0x707DFCF0,0x90103544);
 const auto schedule=F(0x816BA440,owner.sp-0x190,0x8199F01C);
 const uint32_t packet=0x40172000,overlap=0x40173000;
 const std::array<uint32_t,3> linkage{0xFE,overlap,packet};
 for(unsigned variant=0;variant!=7;++variant){
  auto actual=schedule;
  if(variant==0)actual.root.generation++;
  if(variant==1)actual.thread++;
  if(variant==2)actual.pcr+=0x1000;
  if(variant==3)actual.sp-=0x10;
  if(variant==4)actual.lr+=4;
  if(variant==5)actual.function=0x816BA668;
  if(variant==6)actual.lr=0x8199E248;
  CHECK_FALSE(Ledger::CaptureProfileOutput(owner,actual,packet+0x18,overlap,packet,linkage));
 }
 for(unsigned variant=0;variant!=3;++variant){auto wrong=linkage;wrong[variant]+=4;
  CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,packet+0x18,overlap,packet,wrong));}
 CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,packet+0x14,overlap,packet,linkage));
 CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,schedule.sp+0x5C,overlap,packet,linkage));
 CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,packet+0x18,0,packet,linkage));
 CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,packet+0x18,overlap+2,packet,linkage));
 CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,packet+0x18,overlap,packet+2,linkage));
 CHECK_FALSE(Ledger::CaptureProfileOutput(owner,schedule,0x14,overlap,0xFFFFFFFC,{0xFE,overlap,0xFFFFFFFC}));
 auto nested_owner=owner;nested_owner.function=0x8199E148;
 const auto nested_schedule=F(0x816BA440,nested_owner.sp-0xD0,0x8199E248);
 CHECK_FALSE(Ledger::CaptureProfileOutput(nested_owner,nested_schedule,packet+0x18,overlap,packet,linkage));
 const auto proof=Ledger::CaptureProfileOutput(owner,schedule,packet+0x18,overlap,packet,linkage);REQUIRE(proof);
 for(unsigned variant=0;variant!=7;++variant){
  Ledger ledger(root);auto who=owner;uint32_t options=schedule.sp+0x70,callback=0x8199C838;
  if(variant==0)who.root.generation++;
  if(variant==1)who.thread++;
  if(variant==2)who.pcr+=0x1000;
  if(variant==3)who.sp-=0x10;
  if(variant==4)options+=4;
  if(variant==5)callback=0x90103528;
  const auto call=ledger.BeginSchedule(schedule,101,callback,0x40170B40,options,packet+0x18);REQUIRE(call);
  std::optional<std::array<uint32_t,2>> observed=std::array<uint32_t,2>{0x10000000,0x401E6982};
  if(variant==6)observed.reset();
  CHECK_FALSE(ledger.BindProfileProducer(call,813,who,observed,*proof));
  CHECK_FALSE(ledger.FindSchedule(call)->producer);CHECK_FALSE(ledger.lost());
 }
 // A stack-shaped output cannot silently be accepted for a non-null overlap.
 Ledger wrong_output(root);const auto call=wrong_output.BeginSchedule(schedule,101,0x8199C838,0x40170B40,schedule.sp+0x70,schedule.sp+0x5C);REQUIRE(call);
 CHECK_FALSE(wrong_output.BindProfileProducer(call,813,owner,std::array<uint32_t,2>{0x10000000,0x401E6982},*proof));
 CHECK_FALSE(wrong_output.lost());
}
void NativeProfileIdentityTaskTestAccess::ProfileAsyncDoesNotGrantStackControls(){
 Ledger ledger(root);const auto parent=ProfileParent(ledger);
 const auto owner=F(0x8199E3E0,0x707DFCF0,0x90103544);
 const Ledger::ProfileOutput proof{Ledger::ProfileOutput::Kind::kAsyncPacket,0x40172000,0x40173000};
 const auto born=ProfileAllocateWithOutput(ledger,owner,814,0x8199C838,0x40171770,proof,true,nullptr);
 const auto* producer=ledger.ProfileProducerFor(born);REQUIRE(producer);
 for(const auto function:{0x816BA058u,0x816B9F08u}){
  const auto control=F(function,producer->schedule.sp,0x8199F050);Ledger::ProfileControl refused;
  CHECK_FALSE(refused.Enter(*producer,814,owner,control,born,parent,born.task|1u));CHECK_FALSE(refused.entered);
 }
 Ledger::ProfileControl absent,dispatch;
 CHECK_FALSE(dispatch.EnterDispatch(absent,F(0x816B8B08,producer->schedule.sp-0x80,0x816BA0DC),born));
 const auto close=F(0x816B7490,producer->schedule.sp,0x8199F0D0);
 ledger.ObserveProfileOwnerClose(close,born);
 CHECK_FALSE(ledger.ProfileProducerFor(born)->owner_close_entered);
 // This withholds stack-control attribution, not the real native queue's
 // independently observed callback/ref/free bookkeeping.
 auto callback=F(0x8199C838,0x7080F000,0x816B8B64);callback.thread=4;callback.pcr+=0x1000;
 ProfileCallbackThenFree(ledger,born,callback);CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ProfileOutputPrepublicationLifetime(){
 Ledger ledger(root);const auto owner=F(0x8199E3E0,0x707DFCF0,0x90103544);
 const auto schedule=F(0x816BA440,owner.sp-0x190,0x8199F01C);
 const uint32_t packet=0x40172000,overlap=0x40173000;
 auto linkage=std::array<uint32_t,3>{0xFE,overlap,packet};
 const auto proof=Ledger::CaptureProfileOutput(owner,schedule,packet+0x18,overlap,packet,linkage);REQUIRE(proof);
 const auto call=ledger.BeginSchedule(schedule,101,0x8199C838,0x40170B40,schedule.sp+0x70,packet+0x18);REQUIRE(call);
 REQUIRE(ledger.BindProfileProducer(call,815,owner,std::array<uint32_t,2>{0x10000000,0x401E6982},*proof));
 const auto* pending=ledger.FindSchedule(call);REQUIRE(pending);REQUIRE(pending->producer);
 CHECK_FALSE(pending->birth);CHECK_FALSE(pending->producer->published);CHECK_FALSE(pending->producer->native_bound);
 linkage={}; // Caller-local observation storage is not borrowed by the receipt.
 CHECK(pending->producer->output_proof==*proof);CHECK(pending->output==packet+0x18);
 const auto allocation=F(0x816B9A18,schedule.sp-0x90,0x816BA46C);
 REQUIRE(ledger.EnterAllocation(call,allocation,schedule.sp+0x70,allocation.sp+0x50));
 const auto born=ledger.ReturnAllocation(call,allocation,0,0x40171770);REQUIRE(born);
 CHECK_FALSE(ledger.BindProfileNativeState(call,born->task,0x14A00004,0x401E6982));
 const auto publication=F(0x816B9BB8,schedule.sp-0x90,0x816BA4D8);
 REQUIRE(ledger.BeforePublication(call,publication,born->task,0x8199C838,0x40170B40,born->task|1u));
 REQUIRE(ledger.BindProfileNativeState(call,born->task,0x14A00004,0x401E6982));
 const auto* producer=ledger.ProfileProducerFor(*born);REQUIRE(producer);
 CHECK(producer->published);CHECK_FALSE(producer->schedule_returned);
 auto callback=F(0x8199C838,0x7080F000,0x816B8B64);callback.thread=4;callback.pcr+=0x1000;
 REQUIRE(ledger.EnterWork(callback,born->task,producer->argument));REQUIRE(ledger.ReturnWork(callback,*born));
 CHECK_FALSE(ledger.FindSchedule(call)->producer->schedule_returned);
 REQUIRE(ledger.ReturnSchedule(call,schedule,0,std::array<uint32_t,2>{0x14A00004,0x401E6982}));
 CHECK(ledger.ProfileProducerFor(*born)->schedule_returned);CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ProfileWorkerFreeBeforeScheduleReturn(){
 for(bool async:{false,true})for(bool same_address:{false,true}){
  Ledger ledger(root);const auto owner=F(0x8199E3E0,0x707DFCF0,0x90103544);
  Ledger::ProfileOutput proof;
  if(async)proof={Ledger::ProfileOutput::Kind::kAsyncPacket,0x40172000,0x40173000};
  uint64_t call=0;const auto born=ProfileAllocateWithOutput(ledger,owner,816,0x8199C838,0x40171770,proof,false,&call);
  const auto* producer=ledger.ProfileProducerFor(born);REQUIRE(producer);const auto saved=*producer;
  const void* old_slot=ledger.FindBirth(born.generation);REQUIRE(old_slot);
  auto callback=F(0x8199C838,0x7080F000,0x816B8B64);callback.thread=4;callback.pcr+=0x1000;
  ProfileCallbackThenFree(ledger,born,callback);
  REQUIRE(ledger.FindSchedule(call));REQUIRE(ledger.FindSchedule(call)->producer);
  CHECK(ledger.FindSchedule(call)->producer->release_started);
  CHECK_FALSE(ledger.ProfileProducerFor(born));CHECK(ledger.FindBirth(born.generation)->freed);
  auto newer_owner=owner;newer_owner.sp-=0x1000;
  auto newer_proof=proof;if(async){newer_proof.packet+=0x2000;newer_proof.overlap+=0x2000;}
  const auto fresh=ProfileAllocateWithOutput(ledger,newer_owner,817,0x8199C838,same_address?born.task:0x40171880,newer_proof,true,nullptr);
  REQUIRE(fresh.generation!=born.generation);CHECK((fresh.task==born.task)==same_address);
  CHECK(static_cast<const void*>(ledger.FindBirth(fresh.generation))==old_slot);
  CHECK_FALSE(ledger.FindBirth(born.generation));const auto newer=*ledger.ProfileProducerFor(fresh);
  // The old exact Schedule must not read/adopt either freed native bytes or
  // the replacement Node's native flags, even when its numeric Task is reused.
  CHECK_FALSE(ledger.ReturnSchedule(call,saved.schedule,0,std::array<uint32_t,2>{saved.native_flags,saved.native_data}));
  REQUIRE(ledger.FindSchedule(call));REQUIRE(ledger.ReturnSchedule(call,saved.schedule,0));
  CHECK_FALSE(ledger.FindSchedule(call));
  const auto* current=ledger.ProfileProducerFor(fresh);REQUIRE(current);
  CHECK(Ledger::SameBirth(current->birth,newer.birth));CHECK(current->owner_serial==newer.owner_serial);
  CHECK(current->output_proof==newer.output_proof);CHECK(current->native_flags==newer.native_flags);
  CHECK_FALSE(current->release_started);CHECK(current->schedule_returned);
  callback.sp-=0x1000;ProfileCallbackThenFree(ledger,fresh,callback);
  CHECK(ledger.SourceDescendantsClosed(101));CHECK_FALSE(ledger.lost());
 }
}
void NativeProfileIdentityTaskTestAccess::ProfileScheduleReturnRejectsUnprovenReceipts(){
 const auto owner=F(0x8199E3E0,0x707DFCF0,0x90103544);
 {
  Ledger ledger(root);uint64_t call=0;
  const auto born=ProfileAllocateWithOutput(ledger,owner,818,0x8199C838,0x40171770,{},false,&call);
  const auto saved=*ledger.ProfileProducerFor(born);
  CHECK_FALSE(ledger.ReturnSchedule(call,saved.schedule,0));REQUIRE(ledger.FindSchedule(call));
  CHECK_FALSE(ledger.ReturnSchedule(call,saved.schedule,0,std::array<uint32_t,2>{saved.native_flags,saved.native_data+4}));
  CHECK_FALSE(ledger.ReturnSchedule(call,saved.schedule,0,std::array<uint32_t,2>{saved.native_flags^0x40,saved.native_data}));
  CHECK_FALSE(ledger.FindSchedule(call)->producer->schedule_returned);
  REQUIRE(ledger.ReturnSchedule(call,saved.schedule,0,std::array<uint32_t,2>{saved.native_flags,saved.native_data}));
  CHECK_FALSE(ledger.ReturnSchedule(call,saved.schedule,0,std::array<uint32_t,2>{saved.native_flags,saved.native_data}));
  CHECK(ledger.FindLiveTask(born.task));CHECK_FALSE(ledger.FindLiveTask(born.task)->freed);
 }
 for(unsigned variant=0;variant!=6;++variant){
  Ledger ledger(root);uint64_t call=0;
  const auto born=ProfileAllocateWithOutput(ledger,owner,819,0x8199C838,0x40171770,{},false,&call);
  const auto saved=*ledger.ProfileProducerFor(born);auto wrong=saved.schedule;auto serial=call;
  if(variant==0)wrong.sp-=0x10;if(variant==1)wrong.lr+=4;
  if(variant==2)wrong.root.generation++;if(variant==3)wrong.thread++;
  if(variant==4)wrong.pcr+=0x1000;if(variant==5)serial+=0x1000;
  CHECK_FALSE(ledger.ReturnSchedule(serial,wrong,0,std::array<uint32_t,2>{saved.native_flags,saved.native_data}));
  REQUIRE(ledger.FindSchedule(call));CHECK_FALSE(ledger.FindSchedule(call)->producer->schedule_returned);
  CHECK(ledger.FindLiveTask(born.task));CHECK(ledger.lost());
 }
}

TEST_CASE("Profile output separates real stack and FE packet producer ABI",
          "[identity_task][identity_request_lifecycle][abi]"){
 NativeProfileIdentityTaskTestAccess::ProfileOutputStackAndPacketAbi();
}
TEST_CASE("Profile output rejects wrong reciprocal packet and producer ancestry",
          "[identity_task][identity_request_lifecycle][abi]"){
 NativeProfileIdentityTaskTestAccess::ProfileOutputRejectsMalformedProvenance();
}
TEST_CASE("Async profile producer grants no stack wait cancel or close attribution",
          "[identity_task][identity_request_lifecycle][profile_control]"){
 NativeProfileIdentityTaskTestAccess::ProfileAsyncDoesNotGrantStackControls();
}
TEST_CASE("Profile output is retained before native Task publication and Schedule return",
          "[identity_task][identity_request_lifecycle][lifetime]"){
 NativeProfileIdentityTaskTestAccess::ProfileOutputPrepublicationLifetime();
}
TEST_CASE("Profile Schedule return retains old receipt across worker free and Node reuse",
          "[identity_task][identity_request_lifecycle][lifetime][generation]"){
 NativeProfileIdentityTaskTestAccess::ProfileWorkerFreeBeforeScheduleReturn();
}
TEST_CASE("Profile Schedule return refuses stale frames and unobserved native state",
          "[identity_task][identity_request_lifecycle][lifetime]"){
 NativeProfileIdentityTaskTestAccess::ProfileScheduleReturnRejectsUnprovenReceipts();
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
void NativeProfileIdentityTaskTestAccess::ProfileInitializerErrorCompletionAbi(){
 for(bool async:{false,true}){
  Ledger ledger(root);const auto issuer=F(0x8199E3E0,0x707DFCF0,0x90103544);
  const auto scheduled=F(0x816BA440,issuer.sp-0x190,0x8199F01C);
  Ledger::ProfileOutput output_proof;
  if(async)output_proof={Ledger::ProfileOutput::Kind::kAsyncPacket,0x40172000,0x40173000};
  const uint32_t output=async?output_proof.packet+0x18:scheduled.sp+0x5C;
  const auto call=ledger.BeginSchedule(scheduled,101,0x8199C838,0x40170B40,scheduled.sp+0x70,output);REQUIRE(call);
  REQUIRE(ledger.BindProfileProducer(call,820,issuer,std::array<uint32_t,2>{0x10000000,0x401E6982},output_proof));
  const auto allocation=F(0x816B9A18,scheduled.sp-0x90,0x816BA46C);
  REQUIRE(ledger.EnterAllocation(call,allocation,scheduled.sp+0x70,allocation.sp+0x50));
  CHECK_FALSE(ledger.ReturnAllocation(call,allocation,0x8007000E,std::nullopt));
  REQUIRE(ledger.FindSchedule(call));CHECK(ledger.FindSchedule(call)->allocation_returned);
  CHECK_FALSE(ledger.FindSchedule(call)->birth);CHECK_FALSE(ledger.FindSchedule(call)->publish_entered);
  CHECK_FALSE(ledger.SourceDescendantsClosed(101));
  // A real allocation error has no output Task/Birth to resample. Its exact
  // negative producer return retires the call without inventing publication.
  REQUIRE(ledger.ReturnSchedule(call,scheduled,0x8007000E));
  CHECK_FALSE(ledger.FindSchedule(call));CHECK(ledger.SourceDescendantsClosed(101));CHECK_FALSE(ledger.lost());
 }
 const auto owner=F(0x8199E3E0,0x707DFCF0,0x90103544);
 const auto completion=F(0x816A2588,owner.sp-0x190,0x8199EEBC);
 const uint32_t overlap=0x40173000;
 for(const auto extended:{0x80004004u,0x80070570u}){
  CHECK(Ledger::MatchesInitializerErrorCompletion(owner,completion,overlap,overlap,0x65B,extended,0));
 }
 for(unsigned variant=0;variant!=7;++variant){
  auto actual=completion;
  if(variant==0)actual.root.generation++;
  if(variant==1)actual.thread++;
  if(variant==2)actual.pcr+=0x1000;
  if(variant==3)actual.sp-=0x10;
  if(variant==4)actual.lr=0x8199DE00; // Worker completion is not initializer error completion.
  if(variant==5)actual.function=0x816BA058;
  if(variant==6)actual.lr+=4;
  CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,actual,overlap,overlap,0x65B,0x80004004,0));
 }
 auto other_owner=owner;other_owner.function=0x8199E148;
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(other_owner,completion,overlap,overlap,0x65B,0x80004004,0));
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,completion,0,overlap,0x65B,0x80004004,0));
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,completion,overlap,overlap+4,0x65B,0x80004004,0));
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,completion,overlap+2,overlap+2,0x65B,0x80004004,0));
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,completion,overlap,overlap,0x65A,0x80004004,0));
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,completion,overlap,overlap,0x65B,0,0));
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,completion,overlap,overlap,0x65B,1,0));
 CHECK_FALSE(Ledger::MatchesInitializerErrorCompletion(owner,completion,overlap,overlap,0x65B,0x80004004,1));
}
TEST_CASE("Initializer error completion requires its own real overlap and frame ABI",
          "[identity_task][identity_request_lifecycle][overlapped][abi]"){
 NativeProfileIdentityTaskTestAccess::ProfileInitializerErrorCompletionAbi();
}

// Production IdentityServiceRequest and genuine Task ledger cuts only. These
// fixtures do not execute PPC, a kernel, code authentication, the finite hook
// loop, PM views, event signaling or the network backend.
struct NativeProfileIdentityTaskTestAccess::IdentityServiceFixture {
 using Service=Ledger::IdentityServiceRequest;
 struct Dispatch {Frame outer,body,frame;Ledger::CurrentExecution current;uint64_t serial=0;};
 Ledger ledger{root};Service service;Service::Origin origin{};Service::Copy copy{};Service::Pump pump{};
 ExecutionBorn request_task{},pump_task{};Frame acceptance{};
 IdentityServiceFixture(){
  origin.worker=F(0x8199C838,0x708A0000,0x816B8B64);
  origin.worker_birth=Allocate(ledger,3,0x414E0000);
  origin.producer=origin.worker;origin.producer.function=0x81830878;
  origin.producer.sp-=0xBC0;origin.producer.lr=0x8199C488;
  origin.job=401;origin.source=3;origin.work=501;origin.object=0x81AEE470;
  origin.parameters=origin.worker.sp-0x370;origin.flags=4;
  request_task=Born(0x404E5000,0x816A2C80,0x414E1000,0x816A2EC8);
  pump_task=Born(0x404E6000,0x818CEFD8,origin.object,0x818CF164);
  copy={601,701,0x414E1000,0x414E101C,origin.producer.sp-0x670,request_task.birth};
  pump={pump_task.frame,pump_task.birth,origin.object};
  acceptance=F(0x818CD588,0x70830000,0x818CDD2C);acceptance.thread=6;acceptance.pcr=0x90004000;
 }
 ExecutionBorn Born(uint32_t task,uint32_t callback,uint32_t argument,uint32_t caller){
  const auto producer=F(0x816BA440,0x7080FA00,caller);const uint32_t output=0x404E0C60;
  const auto serial=ledger.BeginExecutionSchedule(producer,callback,argument,0,output);REQUIRE(serial);
  const auto allocation=F(0x816B9A18,producer.sp-0x90,0x816BA46C);
  REQUIRE(ledger.EnterExecutionAllocation(serial,allocation,0,allocation.sp+0x50));
  const auto born=ledger.ReturnExecutionAllocation(serial,allocation,0,task);REQUIRE(born);
  const auto publication=F(0x816B9BB8,producer.sp-0x90,0x816BA4D8);
  REQUIRE(ledger.BeforeExecutionPublication(serial,publication,task,callback,argument,task|1u));
  auto body=publication;body.sp-=0x80;body.lr=0x816B9BC0;
  REQUIRE(ledger.ObserveExecutionPublication(publication,*born,body,task,callback,argument,0x04A00008));
  REQUIRE(ledger.ReturnExecutionProducer(serial,producer,0));
  return {*born,serial,producer,publication};
 }
 void BeginCopy(){REQUIRE(service.Begin(origin));REQUIRE(service.BindCopy(copy));}
 void Accept(){
  REQUIRE(service.EnterAcceptance(acceptance,copy,origin.object+0x10,copy.argument,copy.overlap));
  REQUIRE(service.ReturnAcceptance(acceptance,0,copy.argument,copy.overlap));REQUIRE(service.accepted());
 }
 Dispatch Start(Birth birth={},uint32_t thread=7,uint32_t stack=0x70850000){
  if(!birth.generation)birth=pump.birth;
  Dispatch d;d.outer=F(0x816B8B08,stack,0x816B93FC);d.outer.thread=thread;d.outer.pcr=0x90001000+(thread-3)*0x1000;
  d.serial=ledger.BeginExecutionDispatch(d.outer,birth);REQUIRE(d.serial);
  d.body=d.outer;d.body.sp-=0x70;d.body.lr=0x816B8B48;
  REQUIRE(ledger.ExecutionCallbackSelected(d.serial,d.body,birth.task,0x818CEFD8,origin.object));
  d.frame=d.body;d.frame.function=0x818CEE08;d.frame.sp-=0x60;d.frame.lr=0x818CEFE8;
  const auto current=ledger.CaptureCurrentExecution(d.serial,d.frame.thread,d.frame.pcr,d.frame.sp,birth.task);REQUIRE(current);
  d.current=*current;return d;
 }
 void End(const Dispatch& d){
  auto returned=d.body;returned.lr=0x816B8B64;
  REQUIRE(ledger.ExecutionCallbackReturned(d.serial,returned,d.current.birth.task,0));
  REQUIRE(ledger.ReturnExecutionDispatch(d.serial,d.outer,d.current.birth));
 }
 Frame Completion(const Service::Tick& tick){auto f=tick.frame;f.function=0x816A2588;f.sp-=0x4C0;f.lr=0x818CEDA0;return f;}
 Frame Free(const Frame& completion){auto f=completion;f.function=0x816E6AB0;f.sp-=0xA0;f.lr=0x816A26E8;return f;}
 void FinishCompletion(const Frame& completion){
  const auto freeing=Free(completion);REQUIRE(service.EnterEnvelopeFree(freeing,copy.envelope));
  REQUIRE(service.ReturnEnvelopeFree(freeing));REQUIRE(service.ReturnCompletion(completion));
 }
 void Complete(const Service::Tick& tick,uint32_t extended=0){
  const auto frame=Completion(tick);
  REQUIRE(service.EnterCompletion(frame,copy.overlap,copy.envelope,int32_t(extended)<0?0x65Bu:0u,extended,0));
  FinishCompletion(frame);
 }
 void Finish(){
  ExecutionFree(ledger,request_task.birth);ExecutionFree(ledger,pump_task.birth);
  ProfileCallbackThenFree(ledger,origin.worker_birth,origin.worker);
  CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
 }
};

void NativeProfileIdentityTaskTestAccess::IdentityServiceProducerAndCopyAbi(){
 IdentityServiceFixture f;using Service=IdentityServiceFixture::Service;
 for(unsigned which=0;which<17;++which){
  auto wrong=f.origin;
  switch(which){
   case 0:wrong.worker.root.generation++;break;
   case 1:wrong.worker.function=0x8199E148;break;
   case 2:wrong.producer.function=0x816A2F80;break;
   case 3:wrong.producer.thread++;break;
   case 4:wrong.producer.pcr+=0x1000;break;
   case 5:wrong.producer.sp-=0x10;break;
   case 6:wrong.producer.lr+=4;break;
   case 7:wrong.job=0;break;
   case 8:wrong.source=2;break;
   case 9:wrong.work=0;break;
   case 10:wrong.worker_birth.generation=0;break;
   case 11:wrong.worker_birth.origin=1;break;
   case 12:wrong.object+=4;break;
   case 13:wrong.rows=0x41000000;break;
   case 14:wrong.parameters+=4;break;
   case 15:wrong.flags=0x14;break;
   default:wrong.overlap=0x41000000;break;
  }
  Service rejected;CHECK_FALSE(rejected.Begin(wrong));REQUIRE(rejected.Begin(f.origin));
 }
 for(unsigned which=0;which<9;++which){
  Service request;REQUIRE(request.Begin(f.origin));auto wrong=f.copy;
  switch(which){
   case 0:wrong.capture=0;break;
   case 1:wrong.serial=0;break;
   case 2:wrong.envelope+=1;break;
   case 3:wrong.envelope=0xFFFFFE00;break;
   case 4:wrong.argument+=4;break;
   case 5:wrong.overlap+=4;break;
   case 6:wrong.request_birth.generation=0;break;
   case 7:wrong.request_birth.origin=3;break;
   default:wrong.request_birth.task+=1;break;
  }
  CHECK_FALSE(request.BindCopy(wrong));REQUIRE(request.BindCopy(f.copy));CHECK_FALSE(request.BindCopy(f.copy));
 }
 {Service boundary;REQUIRE(boundary.Begin(f.origin));auto last=f.copy;
  last.envelope=0xFFFFFA7C;last.argument=last.envelope+0x1C;
  REQUIRE(boundary.BindCopy(last)); // Exact half-open end 2^32 is representable.
  Service overflow;REQUIRE(overflow.Begin(f.origin));last.envelope+=4;last.argument+=4;
  CHECK_FALSE(overflow.BindCopy(last));}
 f.BeginCopy();f.Accept();auto d=f.Start();const auto tick=f.service.EnterTick(d.frame,f.pump,d.current);REQUIRE(tick);
 f.Complete(*tick);REQUIRE(f.service.ReturnTick(*tick,d.frame,f.pump.birth));f.End(d);
 REQUIRE(f.service.ReturnProducer(f.origin.producer));CHECK(f.service.ready_to_reap());f.Finish();
}

void NativeProfileIdentityTaskTestAccess::IdentityServiceAcceptanceAndPumpAbi(){
 // Exercise the same production read-cell isolation primitive used by Owner.
 // These are strong ownership/TLS transitions, not an invented ProfileRead.
 using Cell=Ledger::ExecutionReadCell;
 const auto enclosing=F(0x8199C838,0x70900000,0x816B8B64);
 const auto service=F(0x818CEE08,enclosing.sp-0x200,0x818CEFE8);
 auto owner=std::make_shared<int>(7);int scope=0,foreign=0;
 std::atomic<void*> cell{&scope};
 {Cell isolated;
  CHECK_FALSE(isolated.Suspend(enclosing,service,owner,owner,&scope,cell));
  REQUIRE(isolated.SuspendService(enclosing,service,owner,owner,&scope,cell));
  CHECK(cell.load()==nullptr);CHECK(isolated.owner()==owner.get());
  auto wrong=service;wrong.lr+=4;CHECK_FALSE(isolated.Restore(wrong,&scope,cell));
  REQUIRE(isolated.Restore(service,&scope,cell));CHECK(cell.load()==&scope);
  CHECK_FALSE(isolated.Restore(service,&scope,cell));}
 for(unsigned fault=0;fault<5;++fault){Cell rejected;auto wrong=service;
  if(fault==0)wrong.root.generation++;if(fault==1)wrong.thread++;
  if(fault==2)wrong.pcr+=0x1000;if(fault==3)wrong.sp=enclosing.sp;
  if(fault==4)wrong.function=0x818CEFD8;
  CHECK_FALSE(rejected.SuspendService(enclosing,wrong,owner,owner,&scope,cell));
  CHECK(cell.load()==&scope);}
 {Cell rejected;auto other=std::make_shared<int>(8);
  CHECK_FALSE(rejected.SuspendService(enclosing,service,owner,other,&scope,cell));
  cell.store(&foreign);CHECK_FALSE(rejected.SuspendService(enclosing,service,owner,owner,&scope,cell));
  CHECK(cell.load()==&foreign);cell.store(&scope);}
 {Cell delegated;REQUIRE(delegated.SuspendService(enclosing,service,owner,owner,&scope,cell));
  REQUIRE(delegated.DelegateToOrigin(owner.get(),&scope));CHECK(cell.load()==nullptr);
  bool fallback=true;REQUIRE(Ledger::RestoreReadCellForScopeLeave(fallback,&scope,cell));
  CHECK_FALSE(fallback);CHECK(cell.load()==&scope);}
 {Cell stopped;REQUIRE(stopped.SuspendService(enclosing,service,owner,owner,&scope,cell));
  REQUIRE(stopped.RestoreAtActualStop(&scope,cell));CHECK(cell.load()==&scope);}
 IdentityServiceFixture f;f.BeginCopy();
 auto wrong=f.acceptance;wrong.lr+=4;
 CHECK_FALSE(f.service.EnterAcceptance(wrong,f.copy,f.origin.object+0x10,f.copy.argument,f.copy.overlap));
 auto copy=f.copy;++copy.serial;
 CHECK_FALSE(f.service.EnterAcceptance(f.acceptance,copy,f.origin.object+0x10,f.copy.argument,f.copy.overlap));
 CHECK_FALSE(f.service.EnterAcceptance(f.acceptance,f.copy,f.origin.object+0x14,f.copy.argument,f.copy.overlap));
 CHECK_FALSE(f.service.EnterAcceptance(f.acceptance,f.copy,f.origin.object+0x10,f.copy.argument+4,f.copy.overlap));
 CHECK_FALSE(f.service.EnterAcceptance(f.acceptance,f.copy,f.origin.object+0x10,f.copy.argument,f.copy.overlap+4));
 REQUIRE(f.service.EnterAcceptance(f.acceptance,f.copy,f.origin.object+0x10,f.copy.argument,f.copy.overlap));
 CHECK_FALSE(f.service.ReturnAcceptance(wrong,0,f.copy.argument,f.copy.overlap));
 REQUIRE(f.service.ReturnAcceptance(f.acceptance,0,f.copy.argument,f.copy.overlap));
 auto d=f.Start();
 for(unsigned which=0;which<6;++which){
  auto pump=f.pump;
  switch(which){case 0:pump.schedule.lr+=4;break;case 1:pump.schedule.root.generation++;break;
   case 2:pump.object+=4;break;case 3:pump.birth.origin=3;break;case 4:pump.birth.generation=0;break;
   default:pump.birth.task+=4;break;}
  CHECK_FALSE(f.service.EnterTick(d.frame,pump,d.current));
 }
 for(unsigned which=0;which<6;++which){
  auto execution=d.current;
  switch(which){case 0:execution.invocation=0;break;case 1:execution.callback=0x8199C838;break;
   case 2:execution.argument+=4;break;case 3:execution.root.generation++;break;
   case 4:execution.birth.generation++;break;default:execution.dispatcher.pcr+=0x1000;break;}
  CHECK_FALSE(f.service.EnterTick(d.frame,f.pump,execution));
 }
 wrong=d.frame;wrong.sp-=0x10;CHECK_FALSE(f.service.EnterTick(wrong,f.pump,d.current));
 wrong=d.frame;wrong.lr+=4;CHECK_FALSE(f.service.EnterTick(wrong,f.pump,d.current));
 const auto tick=f.service.EnterTick(d.frame,f.pump,d.current);REQUIRE(tick);
 CHECK_FALSE(f.service.EnterTick(d.frame,f.pump,d.current));
 wrong=d.frame;wrong.thread++;CHECK_FALSE(f.service.ReturnTick(*tick,wrong,f.pump.birth));
 f.Complete(*tick);REQUIRE(f.service.ReturnTick(*tick,d.frame,f.pump.birth));f.End(d);
 REQUIRE(f.service.ReturnProducer(f.origin.producer));CHECK(f.service.ready_to_reap());f.Finish();
}

void NativeProfileIdentityTaskTestAccess::IdentityServicePrebodyAdoptionAndPendingTicks(){
 IdentityServiceFixture f;f.BeginCopy();auto parked=f.Start();
 const auto same_prebody_frame=parked.frame;const auto same_dispatch=parked.current.invocation;
 CHECK_FALSE(f.service.EnterTick(same_prebody_frame,f.pump,parked.current));
 f.Accept();auto tick=f.service.EnterTick(same_prebody_frame,f.pump,parked.current);REQUIRE(tick);
 CHECK(tick->frame==same_prebody_frame);CHECK(tick->dispatch==same_dispatch);
 CHECK_FALSE(f.service.EnterTick(same_prebody_frame,f.pump,parked.current));
 const auto first=*tick;
 REQUIRE(f.service.ReturnTick(*tick,same_prebody_frame,f.pump.birth));f.End(parked);
 CHECK(f.service.accepted());CHECK_FALSE(f.service.ready_to_reap());
 for(unsigned index=0;index<4;++index){
  auto d=f.Start();tick=f.service.EnterTick(d.frame,f.pump,d.current);REQUIRE(tick);
  CHECK(tick->serial!=first.serial);CHECK(tick->dispatch!=first.dispatch);
  CHECK(Ledger::SameBirth(tick->birth,f.pump.birth));
  CHECK(f.origin.source==3);CHECK(tick->birth.origin==1); // Operation attribution never remints native Task source.
  CHECK_FALSE(f.service.ready_to_reap());
  if(index==3)f.Complete(*tick,0x80151010);
  REQUIRE(f.service.ReturnTick(*tick,d.frame,f.pump.birth));f.End(d);
  if(index<3)CHECK(f.service.accepted());
 }
 REQUIRE(f.service.ReturnProducer(f.origin.producer));CHECK(f.service.ready_to_reap());f.Finish();
 // This proves same-frame receipt adoption, not execution of the Owner's
 // actual prebody loop or preservation of its State Work object/serial.
}

void NativeProfileIdentityTaskTestAccess::IdentityServiceTerminalBeforeTailJoins(){
 IdentityServiceFixture f;f.BeginCopy();f.Accept();auto d=f.Start();
 const auto tick=f.service.EnterTick(d.frame,f.pump,d.current);REQUIRE(tick);
 const auto complete=f.Completion(*tick);const uint32_t extended=0x80151010;
 CHECK_FALSE(f.service.EnterCompletion(complete,f.copy.overlap,f.copy.envelope,0,extended,0));
 CHECK_FALSE(f.service.EnterCompletion(complete,f.copy.overlap,f.copy.envelope,0x65B,extended,1));
 CHECK_FALSE(f.service.EnterCompletion(complete,f.copy.overlap+4,f.copy.envelope,0x65B,extended,0));
 CHECK_FALSE(f.service.EnterCompletion(complete,f.copy.overlap,f.copy.envelope+4,0x65B,extended,0));
 auto wrong=complete;wrong.lr+=4;
 CHECK_FALSE(f.service.EnterCompletion(wrong,f.copy.overlap,f.copy.envelope,0x65B,extended,0));
 REQUIRE(f.service.EnterCompletion(complete,f.copy.overlap,f.copy.envelope|1u,0x65B,extended,0));
 CHECK_FALSE(f.service.accepted());CHECK_FALSE(f.service.ready_to_reap());
 CHECK_FALSE(f.service.ReturnCompletion(complete)); // Native envelope/free join is not a result-bit shortcut.
 auto freeing=f.Free(complete);wrong=freeing;wrong.sp-=0x10;
 CHECK_FALSE(f.service.EnterEnvelopeFree(wrong,f.copy.envelope));
 CHECK_FALSE(f.service.EnterEnvelopeFree(freeing,f.copy.envelope+4));
 REQUIRE(f.service.EnterEnvelopeFree(freeing,f.copy.envelope));
 wrong=freeing;wrong.lr+=4;CHECK_FALSE(f.service.ReturnEnvelopeFree(wrong));
 REQUIRE(f.service.ReturnEnvelopeFree(freeing));
 wrong=complete;wrong.pcr+=0x1000;CHECK_FALSE(f.service.ReturnCompletion(wrong));
 REQUIRE(f.service.ReturnCompletion(complete));REQUIRE(f.service.ReturnProducer(f.origin.producer));
 CHECK_FALSE(f.service.ready_to_reap()); // A genuine mixed-tail frame still owns the receipt.
 auto stale=*tick;++stale.serial;CHECK_FALSE(f.service.ReturnTick(stale,d.frame,f.pump.birth));
 REQUIRE(f.service.ReturnTick(*tick,d.frame,f.pump.birth));f.End(d);
 CHECK(f.service.ready_to_reap());CHECK_FALSE(f.service.ReturnTick(*tick,d.frame,f.pump.birth));
 CHECK_FALSE(f.service.ReturnCompletion(complete));CHECK_FALSE(f.service.ReturnProducer(f.origin.producer));f.Finish();
}

void NativeProfileIdentityTaskTestAccess::IdentityServiceFrameAndBirthReuse(){
 IdentityServiceFixture f;f.BeginCopy();f.Accept();auto old=f.Start();
 const auto old_tick=f.service.EnterTick(old.frame,f.pump,old.current);REQUIRE(old_tick);
 auto nested=old.frame;nested.sp-=0x60;CHECK_FALSE(f.service.EnterTick(nested,f.pump,old.current));
 // Simulate a missing service own-return while the actual common dispatcher
 // did return and the original native Task was freed. Retained receipt must
 // not be settled using the replacement allocation at the same Task address.
 f.End(old);ExecutionFree(f.ledger,f.pump.birth);
 const auto replacement=f.Born(f.pump.birth.task,0x818CEFD8,f.origin.object,0x818CF164);
 REQUIRE(replacement.birth.generation!=f.pump.birth.generation);
 auto current=f.Start(replacement.birth);
 CHECK_FALSE(f.service.EnterTick(current.frame,f.pump,current.current));
 CHECK_FALSE(f.service.ReturnTick(*old_tick,current.frame,replacement.birth));
 CHECK_FALSE(f.service.ready_to_reap());CHECK(f.service.accepted());
 f.End(current);ExecutionFree(f.ledger,replacement.birth);
 f.service.RetireAtActualStop();CHECK_FALSE(f.service.accepted());CHECK_FALSE(f.service.ready_to_reap());
 // No forged old completion/return is supplied to make this failure close.
 CHECK_FALSE(f.ledger.lost());
}

void NativeProfileIdentityTaskTestAccess::IdentityServiceBusyCancelAndActualStop(){
 {
  IdentityServiceFixture f;f.BeginCopy();
  REQUIRE(f.service.EnterAcceptance(f.acceptance,f.copy,f.origin.object+0x10,f.copy.argument,f.copy.overlap));
  auto complete=f.acceptance;complete.function=0x816A2588;complete.sp-=0x80;complete.lr=0x818CD5F4;
  CHECK_FALSE(f.service.EnterCompletion(complete,f.copy.overlap,f.copy.envelope,0x65B,0x80070005,0));
  REQUIRE(f.service.EnterCompletion(complete,f.copy.overlap,f.copy.envelope,0x65B,0x800700AA,0));
  f.FinishCompletion(complete);
  REQUIRE(f.service.ReturnAcceptance(f.acceptance,0x800700AA,0,0));
  REQUIRE(f.service.ReturnProducer(f.origin.producer));CHECK_FALSE(f.service.accepted());
  CHECK(f.service.ready_to_reap());f.Finish();
 }
 {
  IdentityServiceFixture f;f.BeginCopy();f.Accept();auto d=f.Start();
  const auto tick=f.service.EnterTick(d.frame,f.pump,d.current);REQUIRE(tick);
  f.service.ObserveCancel();CHECK(f.service.cancel_requested());CHECK(f.service.accepted());
  CHECK_FALSE(f.service.terminal_returned());CHECK_FALSE(f.service.ready_to_reap());
  REQUIRE(f.service.ReturnTick(*tick,d.frame,f.pump.birth));f.End(d);
  auto later=f.Start();const auto last=f.service.EnterTick(later.frame,f.pump,later.current);REQUIRE(last);
  f.Complete(*last,0x800703E3);REQUIRE(f.service.ReturnTick(*last,later.frame,f.pump.birth));f.End(later);
  REQUIRE(f.service.ReturnProducer(f.origin.producer));CHECK(f.service.ready_to_reap());f.Finish();
 }
 {
  IdentityServiceFixture f;f.BeginCopy();f.Accept();auto d=f.Start();
  const auto tick=f.service.EnterTick(d.frame,f.pump,d.current);REQUIRE(tick);
  f.service.ObserveCancel();f.service.RetireAtActualStop();
  CHECK_FALSE(f.service.accepted());CHECK_FALSE(f.service.EnterTick(d.frame,f.pump,d.current));
  CHECK_FALSE(f.service.ready_to_reap());
  REQUIRE(f.service.ReturnTick(*tick,d.frame,f.pump.birth));f.End(d);
  CHECK_FALSE(f.service.ready_to_reap()); // Actual-stop marker never manufactures native terminal completion.
  CHECK_FALSE(f.ledger.lost());
 }
}

TEST_CASE("Identity service authenticates original producer and copied native request ABI",
          "[identity_task][identity_service][abi]"){
 NativeProfileIdentityTaskTestAccess::IdentityServiceProducerAndCopyAbi();
}
TEST_CASE("Identity service accepts only the exact request and real pump dispatch",
          "[identity_task][identity_service][abi][race]"){
 NativeProfileIdentityTaskTestAccess::IdentityServiceAcceptanceAndPumpAbi();
}
TEST_CASE("Identity service adopts an already parked frame and permits recurring pending ticks",
          "[identity_task][identity_service][lifetime]"){
 NativeProfileIdentityTaskTestAccess::IdentityServicePrebodyAdoptionAndPendingTicks();
}
TEST_CASE("Identity service terminal completion retains its native free and mixed tail joins",
          "[identity_task][identity_service][lifetime][race]"){
 NativeProfileIdentityTaskTestAccess::IdentityServiceTerminalBeforeTailJoins();
}
TEST_CASE("Identity service cannot borrow a replaced Task or reused physical frame",
          "[identity_task][identity_service][lifetime][race]"){
 NativeProfileIdentityTaskTestAccess::IdentityServiceFrameAndBirthReuse();
}
TEST_CASE("Identity service busy cancel and actual stop preserve true completion duties",
          "[identity_task][identity_service][lifetime][race]"){
 NativeProfileIdentityTaskTestAccess::IdentityServiceBusyCancelAndActualStop();
}

// Production selector/memento and real Task/read-cell metadata only. These
// cases do not fabricate a Kernel, code authentication or native PPC return.
void NativeProfileIdentityTaskTestAccess::ExecutionInstructionNoEffectCuts(){
 using Record=Ledger::ExecutionObservation;
 using Cut=Ledger::ExecutionInstructionParticipation;
 Ledger ledger(root);const auto made=ExecutionAllocate(ledger,0x404E1BF0);
 const auto dispatcher=F(0x816B8B08,0x70850000,0x816B93FC);
 const auto invocation=ledger.BeginExecutionDispatch(dispatcher,made.birth);REQUIRE(invocation);
 CHECK_FALSE(Cut::Capture({},dispatcher.pcr));
 auto record=std::make_shared<Record>();record->frame=dispatcher;record->serial=61;
 CHECK_FALSE(Cut::Capture(record,dispatcher.pcr)); // A real non-execution Work shadows.
 record->execution_only=true;REQUIRE(Cut::Capture(record,dispatcher.pcr));
 record->normal_return=true;CHECK_FALSE(Cut::Capture(record,dispatcher.pcr));
 record->normal_return=false;record->execution_uncertain=true;
 CHECK_FALSE(Cut::Capture(record,dispatcher.pcr));
 record->execution_uncertain=false;CHECK_FALSE(Cut::Capture(record,dispatcher.pcr+4));
 // No selector refusal pretends to select/return the native callback.
 auto body=dispatcher;body.sp-=0x70;body.lr=0x816B8B48;
 CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp,made.birth.task));
 REQUIRE(ledger.ExecutionCallbackSelected(invocation,body,made.birth.task,0x901159C8,1));
 body.lr=0x816B8B64;REQUIRE(ledger.ExecutionCallbackReturned(invocation,body,made.birth.task,0));
 REQUIRE(ledger.ReturnExecutionDispatch(invocation,dispatcher,made.birth));
 REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));
 ExecutionFree(ledger,made.birth);CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ExecutionInstructionRetainedIdentity(){
 using Record=Ledger::ExecutionObservation;
 using Cut=Ledger::ExecutionInstructionParticipation;
 auto record=std::make_shared<Record>();record->frame=F(0x816B8B08);record->serial=71;
 record->execution_only=true;std::weak_ptr<Record> retained=record;
 auto cut=Cut::Capture(record,record->frame.pcr);REQUIRE(cut);
 auto replacement=std::make_shared<Record>(*record);
 CHECK_FALSE(cut->Matches(replacement,replacement->frame.pcr)); // Scalar/frame reuse is not the old Work.
 CHECK(cut->Matches(record,record->frame.pcr));
 ++record->serial;CHECK_FALSE(cut->Matches(record,record->frame.pcr));--record->serial;
 record->frame.lr+=4;CHECK_FALSE(cut->Matches(record,record->frame.pcr));record->frame.lr-=4;
 record->execution_uncertain=true;CHECK_FALSE(cut->Matches(record,record->frame.pcr));
 record->execution_uncertain=false;record->normal_return=true;
 CHECK_FALSE(cut->Matches(record,record->frame.pcr));
 record.reset();CHECK_FALSE(retained.expired()); // Slow validation cannot release/reuse the selected record.
 cut.reset();CHECK(retained.expired());
}
void NativeProfileIdentityTaskTestAccess::ExecutionInstructionLedgerEffects(){
 using Record=Ledger::ExecutionObservation;
 using Cut=Ledger::ExecutionInstructionParticipation;
 Ledger ledger(root);const auto made=ExecutionAllocate(ledger,0x404E2BF0,17);
 const auto dispatcher=F(0x816B8B08,0x70850000,0x816B93FC);
 const auto invocation=ledger.BeginExecutionDispatch(dispatcher,made.birth);REQUIRE(invocation);
 auto record=std::make_shared<Record>();record->frame=dispatcher;record->serial=81;record->execution_only=true;
 const auto cut=Cut::Capture(record,dispatcher.pcr);REQUIRE(cut);
 auto body=dispatcher;body.sp-=0x70;body.lr=0x816B8B48;
 // A participating malformed loaded operand still reaches the genuine ledger
 // refusal; the cheap selector does not silently filter it or issue a fact.
 REQUIRE(cut->Matches(record,dispatcher.pcr));
 auto bad=ledger;
 CHECK_FALSE(bad.ExecutionCallbackSelected(invocation,body,made.birth.task+4,0x901159C8,17));
 record->execution_uncertain=true;CHECK_FALSE(cut->Matches(record,dispatcher.pcr));
 CHECK_FALSE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp,made.birth.task));
 record->execution_uncertain=false;
 const auto fresh=Cut::Capture(record,dispatcher.pcr);REQUIRE(fresh);REQUIRE(fresh->Matches(record,dispatcher.pcr));
 REQUIRE(ledger.ExecutionCallbackSelected(invocation,body,made.birth.task,0x901159C8,17));
 REQUIRE(ledger.CaptureCurrentExecution(invocation,body.thread,body.pcr,body.sp,made.birth.task));
 body.lr=0x816B8B64;REQUIRE(ledger.ExecutionCallbackReturned(invocation,body,made.birth.task,0));
 REQUIRE(ledger.ReturnExecutionDispatch(invocation,dispatcher,made.birth));record->normal_return=true;
 CHECK_FALSE(fresh->Matches(record,dispatcher.pcr));
 REQUIRE(ledger.ReturnExecutionProducer(made.producer,made.frame,0));
 ExecutionFree(ledger,made.birth);CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
}
void NativeProfileIdentityTaskTestAccess::ExecutionUnknownDispatchReadDuty(){
 Ledger ledger(root);const Birth unknown{0,1,0x404E3BF0};
 auto origin=F(0x8199C838,0x70860000,0x816B8B64);
 auto dispatcher=F(0x816B8B08,0x70850000,0x816B93FC);
 CHECK_FALSE(ledger.BeginExecutionDispatch(dispatcher,unknown));
 std::atomic<void*> actual_cell{nullptr};
 CHECK_FALSE(Ledger::DispatchCodeAuthenticationRequired(unknown,actual_cell.load()));
 auto owner=std::make_shared<int>(17);void* scope=owner.get();actual_cell.store(scope);
 // A read appearing after the provisional null sample needs the original
 // full proof at admission. It must not degrade to unavailable restoration.
 REQUIRE(Ledger::DispatchCodeAuthenticationRequired(unknown,actual_cell.load()));
 Ledger::ExecutionReadCell older;
 REQUIRE(older.Suspend(origin,dispatcher,owner,owner,scope,actual_cell));
 REQUIRE(older.pending());CHECK(actual_cell.load()==nullptr);
 const auto original_owner=older.owner();
 // An independently suspended older read is still strongly retained even
 // though another unknown dispatch has no ambient read to authenticate.
 CHECK_FALSE(Ledger::DispatchCodeAuthenticationRequired(unknown,actual_cell.load()));
 CHECK(older.pending());CHECK(older.owner()==original_owner);CHECK(older.scope()==scope);
 REQUIRE(older.Restore(dispatcher,scope,actual_cell));CHECK(actual_cell.load()==scope);
 CHECK(Ledger::DispatchCodeAuthenticationRequired(unknown,actual_cell.load()));
 const auto known=ExecutionAllocate(ledger,0x404E4BF0);
 CHECK(Ledger::DispatchCodeAuthenticationRequired(known.birth,nullptr));
 REQUIRE(ledger.ReturnExecutionProducer(known.producer,known.frame,0));
 ExecutionFree(ledger,known.birth);CHECK(ledger.ExecutionClosed());CHECK_FALSE(ledger.lost());
}
TEST_CASE("Task instruction participation leaves absent returned and uncertain records effect free",
          "[identity_task][execution][task_participation]"){
 NativeProfileIdentityTaskTestAccess::ExecutionInstructionNoEffectCuts();
}
TEST_CASE("Task instruction participation retains the exact Work across reuse and state changes",
          "[identity_task][execution][task_participation][lifetime]"){
 NativeProfileIdentityTaskTestAccess::ExecutionInstructionRetainedIdentity();
}
TEST_CASE("Task instruction participation preserves genuine ledger operand refusal and callback joins",
          "[identity_task][execution][task_participation][race]"){
 NativeProfileIdentityTaskTestAccess::ExecutionInstructionLedgerEffects();
}
TEST_CASE("Unknown Task dispatch participation preserves actual ambient and older suspended reads",
          "[identity_task][execution][task_participation][read_scope]"){
 NativeProfileIdentityTaskTestAccess::ExecutionUnknownDispatchReadDuty();
}
} // namespace xe::kernel::test
