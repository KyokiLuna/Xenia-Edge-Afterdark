/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <chrono>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_identity_listener_bridge.h"
namespace xe::kernel {
// Exercises the production private bridge's retained metadata/resource cuts.
// The small resource stands for a lifetime only; no Kernel/native call/ready
// or renderer/GPU acknowledgment is constructed by these source tests.
class NativeIdentityListenerBridgeTestAccess final {
 public:
  static void SystemThreadFrameConsumer(bool wrong_return){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(23);
    const auto source_a=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life));
    const auto source_b=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,life));
    const auto origin_a=bridge.AdoptSource(source_a),origin_b=bridge.AdoptSource(source_b);REQUIRE(origin_a);REQUIRE(origin_b);
    const auto registration=ListenerFrame(root,0x816A8498,life);const B::Area area{0x90005000,0xA,0xA};
    const auto listener=bridge.BeforeRegistration(registration,0x81A821FC,0x90004000,std::span(&area,1),life);
    REQUIRE(listener);REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const L::WorkKey key{root,0,43,41,0x90002000,0,0x707DF740,0x81A4011C,0x81A3F848,L::WorkOwnerKind::kSystemThreadFrame};
    REQUIRE(key.valid());CHECK(key.task==0);CHECK(key.task_allocation_generation==0);
    const auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,source_a,life));
    const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,area.address,0xA,0,*origin_b));
    const auto next=std::shared_ptr<const B::Frame>(new B::Frame(root,101,103,107,41,0x90002000,
        0x707DF640,0x81A3F890,0x816A8C00,life,2));
    std::array<uint8_t,4> selected{0,0,0,0xA};std::atomic<void*> cell_slot{nullptr};
    auto* cell=bridge.BeforeGetNext(next,*work,0x10000180,0,0,0,selected.data(),nullptr,cell_slot);REQUIRE(cell);
    REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0,0x90004000));
    auto regs=FastRegisters();regs.lr=0x816A8C94;regs.r[1]=0x707DF640-0x90;regs.r[3]=area.address;
    REQUIRE(B::FastEnter(cell,regs,0x816A8648));
    regs.lr=0x816A8874;regs.r[1]-=0xB0;regs.r[3]=0x9000401C;regs.r[30]=area.address;regs.r[25]=0;
    REQUIRE(B::FastBeforeUnlock(cell,regs));
    regs.lr=0x816A8C94;regs.r[1]+=0xB0;regs.r[3]=0;REQUIRE(B::FastReturn(cell,regs,0x816A8648));
    REQUIRE(bridge.BeforeReferenceRelease(cell,0x90004000));REQUIRE(bridge.ReferenceReleaseReturned(cell,0x90004000));
    REQUIRE(bridge.OuterReturned(cell,next,1));CHECK(cell_slot.load()==nullptr);
    // BOOL1 completed the public API, but the real finite F848 consumer has
    // not returned. Its entry A and consumed B remain separate owned duties.
    B::SourceProducerObservation observation;
    CHECK_FALSE(bridge.SourceProducerQuiescent(*origin_a,&observation));CHECK(observation.consumer_reason==4);
    CHECK_FALSE(bridge.SourceProducerQuiescent(*origin_b,&observation));CHECK(observation.consumer_reason==5);CHECK_FALSE(observation.orphaned);
    auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);CHECK(facts->entry==source_a);
    CHECK(facts->count==1);CHECK(facts->dequeued[0]==source_b);CHECK_FALSE(facts->unknown);
    auto returned=key;if(wrong_return)returned.lr+=4;
    if(wrong_return){CHECK_FALSE(bridge.FinishWholeWorkNormally(*work,returned,whole));CHECK(bridge.lost());return;}
    REQUIRE(bridge.FinishWholeWorkNormally(*work,returned,whole));
    CHECK(bridge.SourceOnlyProducer(*origin_a));CHECK(bridge.SourceOnlyProducer(*origin_b));CHECK_FALSE(bridge.lost());
  }
  static void SystemThreadFrameKeyRefusesTaskClaims(uint32_t variant){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(23);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life));
    REQUIRE(bridge.AdoptSource(source));
    L::WorkKey key{root,0,43,41,0x90002000,0,0x707DF740,0x81A4011C,0x81A3F848,L::WorkOwnerKind::kSystemThreadFrame};
    if(variant==0)key.owner_kind=L::WorkOwnerKind::kTask;
    if(variant==1)key.task=0x90003000;
    if(variant==2)key.task_allocation_generation=41;
    if(variant==3)key.owner_kind=static_cast<L::WorkOwnerKind>(2);
    CHECK_FALSE(key.valid());
    const auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,source,life));
    CHECK_FALSE(bridge.AdoptWholeWork(whole));CHECK(bridge.SourceOnlyProducer(*bridge.SourceOrigin(source)));
  }
  static void SystemFrameParentAuthorityRefusesMixedClaims(uint32_t variant){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(23);
    const auto a=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life));
    const auto b=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,life));
    const auto oa=bridge.AdoptSource(a),ob=bridge.AdoptSource(b);REQUIRE(oa);REQUIRE(ob);
    L::WorkKey parent_key{root,41,43,47,0x90001000,0x90002000,0x71000000,0x816CD70C,0x8187DE10};
    if(variant==1){parent_key.task=0;parent_key.task_allocation_generation=0;parent_key.owner_kind=L::WorkOwnerKind::kSystemThreadFrame;}
    const auto parent=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(parent_key,a,life));
    const auto parent_work=bridge.AdoptWholeWork(parent);REQUIRE(parent_work);
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,0,*ob));
    REQUIRE(bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,0xA,0,*parent_work).retained_by_work);
    auto child_key=parent_key;child_key.invocation=44;child_key.sp-=0x90;child_key.lr=0x8187DEC0;child_key.function=0x81879EA0;
    auto entry=a;auto claimed_parent=parent;
    if(variant==0){child_key.task=0;child_key.task_allocation_generation=0;child_key.owner_kind=L::WorkOwnerKind::kSystemThreadFrame;}
    if(variant==1){child_key.task=0x90002000;child_key.task_allocation_generation=41;child_key.owner_kind=L::WorkOwnerKind::kTask;}
    if(variant==2)entry=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life)); // unregistered equal source
    if(variant==3)claimed_parent=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(parent_key,a,life)); // equal value, different retained owner
    if(variant==4)++child_key.task_allocation_generation; // recycled Task cannot become synchronous parent
    const auto child=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(child_key,entry,life,claimed_parent));
    CHECK_FALSE(bridge.AdoptWholeWork(child));CHECK(bridge.lost());
    // Fail-closed refusal keeps the genuine whole parent and both registered
    // source owners retained; lost ledger metadata is deliberately unreadable.
    const auto* retained=bridge.WorkFor(*parent_work);REQUIRE(retained);
    CHECK(retained->source==parent);CHECK(retained->id==*parent_work);
    CHECK_FALSE(retained->finished);
    CHECK(bridge.sources_[oa->slot]==a);CHECK(bridge.origins_[oa->slot]==*oa);
    CHECK(bridge.sources_[ob->slot]==b);CHECK(bridge.origins_[ob->slot]==*ob);
  }
  static void TypedScalarDelivery(uint32_t variant,bool sui_caller=false){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(23);
    auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life));
    const std::weak_ptr<const NativeIdentityEventSource> source_lifetime=source;
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    const auto registration=ListenerFrame(root,0x816A8498,life);const B::Area area{0x90005000,0xA,0xA};
    const auto listener=bridge.BeforeRegistration(registration,0x81A821FC,0x90004000,std::span(&area,1),life);
    REQUIRE(listener);REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const auto enqueue=[&](std::shared_ptr<const NativeIdentityEventSource> from,uint32_t public_id,uint32_t data){
      const auto frame=ListenerFrame(root,0x816A93C8,life);std::atomic<void*> slot{nullptr};
      auto* cell=bridge.BeforeBroadcast(frame,from,data,slot,public_id);REQUIRE(cell);
      auto regs=FastRegisters();regs.lr=0x816A8478;regs.r[1]=0x707DF640-0xF0;
      regs.r[3]=area.address;regs.r[5]=data;REQUIRE(B::FastEnter(cell,regs,0x816A8328));
      regs.lr=0x816A8410;regs.r[1]-=0x80;regs.r[3]=0x9000401C;regs.r[31]=area.address;regs.r[28]=data;regs.r[29]=0xA;
      REQUIRE(B::FastBeforeUnlock(cell,regs));regs.lr=0x816A8478;regs.r[1]+=0x80;regs.r[3]=0;
      REQUIRE(B::FastReturn(cell,regs,0x816A8328));REQUIRE(bridge.OuterReturned(cell,frame,0));
    };
    if(variant==16){
      enqueue(source,0xA,0);const auto orphan=bridge.ledger_.TransferSuccessfulDequeue(*listener,area.address,0xA,0,{});
      CHECK(orphan.origin_known);CHECK_FALSE(orphan.scalar_delivery.origin.serial);
    }
    const uint32_t data=variant==3?0x10u:variant==12?0xFu:0u;
    enqueue(variant==13?nullptr:source,variant==2?0xBu:0xAu,data);
    auto expected_source=source;auto expected_origin=*origin;
    if(variant==10)enqueue(source,0xB,data); // identical data cannot retain old typedness
    if(variant==11){
      expected_source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,life));
      const auto second=bridge.AdoptSource(expected_source);REQUIRE(second);expected_origin=*second;
      enqueue(expected_source,0xA,data);CHECK(bridge.SourceOnlyProducer(*origin));
    }
    std::shared_ptr<const NativeIdentityEventWorkSource> whole;std::optional<L::Work> work;
    std::shared_ptr<const NativeIdentityEventSource> replacement_source;std::optional<L::Origin> replacement_origin;
    L::WorkKey whole_key{root,41,43,41,0x90002000,0x90003000,0x707DF740,0x816CD70C,0x816CD6E8};
    if(variant==4 || variant==21){whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(whole_key,source,life));work=bridge.AdoptWholeWork(whole);REQUIRE(work);}
    const auto mode=variant==1?B::ScalarDeliveryMode::kNone:
        variant==15?static_cast<B::ScalarDeliveryMode>(2):B::ScalarDeliveryMode::kSystemDiscardedIdentityHint;
    // The SUI fixture supplies the proven caller shape only. It does not
    // execute Owner's module/code/import admission or the native caller body.
    const uint32_t caller_lr=sui_caller?0x90117798u:0x9124F05Cu;
    const auto next=std::shared_ptr<const B::Frame>(new B::Frame(root,101,103,107,41,0x90002000,
        0x707DF640,caller_lr,0x816A8C00,life,variant==14?1u:2u,mode));
    std::array<uint8_t,4> selected{0,0,0,0xA},actual_data{0,0,0,uint8_t(data)},public_id{0,0,0,0};
    std::atomic<void*> slot{nullptr};
    const uint32_t id_output=0x707DF690,data_output=variant==12?0u:0x707DF694u;
    auto* cell=bridge.BeforeGetNext(next,work,0x10000180,0,id_output,data_output,selected.data(),
        data_output?actual_data.data():nullptr,slot,variant==18?nullptr:public_id.data());
    if(variant==14 || variant==15 || variant==18){CHECK_FALSE(cell);CHECK(bridge.lost());return;}
    REQUIRE(cell);
    if(variant==20 || variant==21)bridge.CloseNewSources();
    REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0,0x90004000));
    auto regs=FastRegisters();regs.lr=0x816A8C94;regs.r[1]=0x707DF640-0x90;regs.r[3]=area.address;regs.r[4]=id_output;regs.r[5]=data_output;
    REQUIRE(B::FastEnter(cell,regs,0x816A8648));regs.lr=0x816A8874;regs.r[1]-=0xB0;regs.r[3]=0x9000401C;
    regs.r[30]=area.address;regs.r[22]=id_output;regs.r[23]=data_output;regs.r[25]=0;
    REQUIRE(B::FastBeforeUnlock(cell,regs));
    const bool delivery=variant!=1 && variant!=2 && variant!=3 && variant!=4 && variant!=21 && variant!=10 && variant!=13;
    CHECK(bool(cell->scalar_delivery_.origin.serial)==delivery);
    if(delivery){CHECK(cell->scalar_delivery_.origin==expected_origin);CHECK(cell->scalar_delivery_.enqueue_sequence!=0);}
    B::SourceProducerObservation before_return;
    CHECK_FALSE(bridge.SourceProducerQuiescent(expected_origin,&before_return)); // actual API cell still active
    CHECK_FALSE(bridge.OldSourceDataQuiescent(expected_origin)); // genuine scalar/API duty, not passive queue
    if(variant==0){
      B::SourceProducerObservation ownership;
      CHECK_FALSE(bridge.SourceOnlyProducer(expected_origin,&ownership,true));
      REQUIRE(ownership.ownership);CHECK_FALSE(ownership.ownership->available);
      CHECK(ownership.ownership->active_cells==1);CHECK(ownership.ownership->first_active_cell!=0);
      CHECK(ownership.ownership->queued==0);CHECK(ownership.ownership->work_entry_pins==0);
    }
    if(variant==1 || variant==2 || variant==3 || variant==10 || variant==16)CHECK(before_return.orphaned);
    // Original inverse mapping writes the caller output before inner return.
    public_id=variant==17?std::array<uint8_t,4>{0x8A,0,0,0xA}:
        (variant==4 || variant==21 || (sui_caller && variant==2))?std::array<uint8_t,4>{0,0,0,0xB}:std::array<uint8_t,4>{0,0,0,0xA};
    regs.lr=0x816A8C94;regs.r[1]+=0xB0;regs.r[3]=0;REQUIRE(B::FastReturn(cell,regs,0x816A8648));
    if(variant!=6){
      REQUIRE(bridge.BeforeReferenceRelease(cell,0x90004000));
      if(variant==8){const auto destroy=ListenerFrame(root,0x816A88E8,life);REQUIRE(bridge.BeforeDestruction(destroy,0x90004000));REQUIRE(bridge.DestructionReturned(0x90004000,*destroy));}
      REQUIRE(bridge.ReferenceReleaseReturned(cell,0x90004000));
    }
    if(variant==9 || (sui_caller && variant==0))REQUIRE(bridge.FinishSourceAfterDescendants(expected_origin));
    if(sui_caller && variant==0){
      // A producer's real finish cannot release its already-dequeued scalar
      // dependency while GetNext's actual own-return is still pending.
      source.reset();expected_source.reset();
      CHECK_FALSE(source_lifetime.expired());
      {auto not_yet=bridge.ReapOnHostOutsideGuestLocks();}
      CHECK_FALSE(source_lifetime.expired());
    }
    if(variant==19){
      // After native dequeue, a separate serialized native publication may
      // reuse the empty Event slot before this older API returns.
      replacement_source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,life));
      replacement_origin=bridge.AdoptSource(replacement_source);REQUIRE(replacement_origin);
      REQUIRE(bridge.ledger_.ReplaceQueued(*listener,area.address,0xA,0,*replacement_origin,true));
      CHECK_FALSE(bridge.SourceOnlyProducer(*replacement_origin));
    }
    auto returned=next;
    if(variant==7)returned=std::shared_ptr<const B::Frame>(new B::Frame(root,101,103,107,41,0x90002000,
        0x707DF640,caller_lr,0x816A8C00,life,2,mode));
    const bool complete=bridge.OuterReturned(cell,returned,variant==5?0u:1u);
    if(variant==5 || variant==6 || variant==7 || variant==17){CHECK_FALSE(complete);CHECK(bridge.lost());CHECK(slot.load()==cell);CHECK(cell->scalar_delivery_.origin==expected_origin);return;}
    REQUIRE(complete);CHECK(slot.load()==nullptr);CHECK_FALSE(cell->scalar_delivery_.origin.serial);
    if(variant==20 || variant==21){
      auto status=B::CellAdmissionStatus::kOpened;
      CHECK_FALSE(bridge.BeforeGetNext(next,work,0x10000180,0,id_output,data_output,selected.data(),
          data_output?actual_data.data():nullptr,slot,public_id.data(),&status));
      CHECK(status==B::CellAdmissionStatus::kClosedHealthy);CHECK_FALSE(bridge.lost());
    }
    if(variant==19){
      // Scalar completion released only A's transferred pin; the newer B
      // event is still queued and must independently reach its real consumer.
      CHECK(bridge.SourceOnlyProducer(*origin));CHECK_FALSE(bridge.SourceOnlyProducer(*replacement_origin));
      CHECK(bridge.OldSourceDataQuiescent(*origin));CHECK(bridge.OldSourceDataQuiescent(*replacement_origin));
      const auto later=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(whole_key,source,life));
      const auto later_work=bridge.AdoptWholeWork(later);REQUIRE(later_work);
      const auto consumed=bridge.ledger_.TransferSuccessfulDequeue(*listener,area.address,0xA,0,*later_work);
      CHECK(consumed.origin==*replacement_origin);CHECK(consumed.retained_by_work);
      REQUIRE(bridge.FinishWholeWorkNormally(*later_work,whole_key,later));CHECK(bridge.SourceOnlyProducer(*replacement_origin));
    }
    if(variant==4 || variant==21){
      auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);CHECK(facts->count==1);CHECK(facts->dequeued[0]==source);
      CHECK_FALSE(bridge.SourceOnlyProducer(*origin));CHECK_FALSE(bridge.OldSourceDataQuiescent(*origin));
      REQUIRE(bridge.FinishWholeWorkNormally(*work,whole_key,whole));
    }
    if(variant==1 || variant==2 || variant==3 || variant==10 || variant==16){
      B::SourceProducerObservation after;CHECK_FALSE(bridge.SourceProducerQuiescent(expected_origin,&after));CHECK(after.orphaned);
      CHECK_FALSE(bridge.SourceOnlyProducer(expected_origin));CHECK_FALSE(bridge.OldSourceDataQuiescent(expected_origin));
    }else if(variant==9 || (sui_caller && variant==0)){
      {
        auto retired=bridge.ReapOnHostOutsideGuestLocks();CHECK(retired.count>0); // producer+delivery both genuinely ended
        if(sui_caller && variant==0)CHECK_FALSE(source_lifetime.expired());
      } // actual source holders are dropped outside bridge/native locks
      if(sui_caller && variant==0){
        CHECK(source_lifetime.expired());
        CHECK_FALSE(bridge.SourceProducerQuiescent(expected_origin));
        const auto successor=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,life));
        const auto successor_origin=bridge.AdoptSource(successor);REQUIRE(successor_origin);
        CHECK(*successor_origin!=expected_origin);CHECK(bridge.SourceOnlyProducer(*successor_origin));
        // Restart owns a new root generation, never an old scalar/read grant.
        B restarted({root.kernel,root.domain,root.generation+1});
        CHECK_FALSE(restarted.SourceProducerQuiescent(*successor_origin));
      }
    }else{CHECK(bridge.SourceOnlyProducer(expected_origin));CHECK(bridge.OldSourceDataQuiescent(expected_origin));}
    CHECK_FALSE(bridge.lost());
  }
  static void ClosedCellAdmissionIsCancellation(bool broadcast){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(23);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);bridge.CloseNewSources();
    const auto frame=ListenerFrame(root,broadcast?0x816A93C8:0x816A8C00,life);
    std::atomic<void*> slot{nullptr};std::array<uint8_t,4> selected{0xDE,0xAD,0xBE,0xEF},data{0xCA,0xFE,0xBA,0xBE};
    auto status=B::CellAdmissionStatus::kOpened;
    auto* cell=broadcast?bridge.BeforeBroadcast(frame,source,0,slot,0xA,&status):
        bridge.BeforeGetNext(frame,{},0x10000180,0,0x707DF690,0x707DF694,
            selected.data(),data.data(),slot,nullptr,&status);
    CHECK_FALSE(cell);CHECK(status==B::CellAdmissionStatus::kClosedHealthy);
    CHECK(slot.load()==nullptr);CHECK_FALSE(bridge.lost());CHECK(bridge.FirstFailure().state==0);
    CHECK(selected[0]==0xDE);CHECK(data[0]==0xCA);CHECK(bridge.SourceOnlyProducer(*origin));
    // Closing admission never claims the retained producer completed.
    REQUIRE(bridge.FinishSourceAfterDescendants(*origin));
    CHECK(bridge.ReapOnHostOutsideGuestLocks().count>0);
  }
  static void ClosedAdmissionPreservesFailures(uint32_t variant){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(23);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life));
    REQUIRE(bridge.AdoptSource(source));
    auto frame=ListenerFrame(root,0x816A8C00,life);std::atomic<void*> slot{nullptr};std::array<uint8_t,4> selected{};
    std::optional<L::Work> work;
    if(variant==2){
      const L::WorkKey key{root,41,43,42,0x90002000,0x90003000,0x707DF740,0x816CD70C,0x816CD6E8};
      const auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,source,life));
      work=bridge.AdoptWholeWork(whole);REQUIRE(work); // genuine Work, wrong actual thread
    }
    B::FastCell* prior=nullptr;
    if(variant==3){prior=bridge.BeforeGetNext(frame,{},0x10000180,0,0,0,selected.data(),nullptr,slot);REQUIRE(prior);}
    if(variant==4)work=L::Work{root,999,0};
    if(variant==5)CHECK_FALSE(bridge.ledger_.ReplaceQueued({},0x90005000,0,0,{}));
    std::array<std::atomic<void*>,B::kCells+1> slots{};
    if(variant==6){
      for(uint32_t i=0;i<B::kCells;++i){
        const auto next=std::shared_ptr<const B::Frame>(new B::Frame(root,101+i,103+i,107,41+i,
            0x90002000+i*0x1000,0x707DF640,0x816CD70C,0x816A8C00,life));
        REQUIRE(bridge.BeforeGetNext(next,{},0x10000180,0,0,0,selected.data(),nullptr,slots[i]));
      }
      auto open=B::CellAdmissionStatus::kClosedHealthy;
      CHECK_FALSE(bridge.BeforeGetNext(frame,{},0x10000180,0,0,0,selected.data(),nullptr,slots[B::kCells],nullptr,&open));
      CHECK(open==B::CellAdmissionStatus::kFailed);CHECK(bridge.lost());
    }
    bridge.CloseNewSources();
    auto status=B::CellAdmissionStatus::kClosedHealthy;
    if(variant==0)frame=ListenerFrame(root,0x816A93C8,life); // wrong original ABI
    B::FastCell* result=nullptr;
    if(variant==7){
      const auto broadcast=ListenerFrame(root,0x816A93C8,life);
      result=bridge.BeforeBroadcast(broadcast,source,0x1234567800000000ull,slot,0xA,&status);
    }else result=bridge.BeforeGetNext(frame,work,variant==1?0x1234567810000180ull:0x10000180,0,0,0,
        selected.data(),nullptr,slot,nullptr,&status);
    CHECK_FALSE(result);CHECK(status==B::CellAdmissionStatus::kFailed);CHECK(bridge.lost());
    if(variant==3)CHECK(slot.load()==prior); // seal cannot erase an existing collision
  }
  static void AdmittedBroadcastSurvivesSeal(){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(23);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,life));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    const auto registration=ListenerFrame(root,0x816A8498,life);const B::Area area{0x90005000,0xA,0xA};
    const auto listener=bridge.BeforeRegistration(registration,0x81A821FC,0x90004000,std::span(&area,1),life);
    REQUIRE(listener);REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const auto frame=ListenerFrame(root,0x816A93C8,life);std::atomic<void*> slot{nullptr};auto status=B::CellAdmissionStatus::kFailed;
    auto* cell=bridge.BeforeBroadcast(frame,source,9,slot,0xA,&status);REQUIRE(cell);CHECK(status==B::CellAdmissionStatus::kOpened);
    bridge.CloseNewSources();
    auto regs=FastRegisters();regs.lr=0x816A8478;regs.r[1]=0x707DF640-0xF0;regs.r[3]=area.address;regs.r[5]=9;
    REQUIRE(B::FastEnter(cell,regs,0x816A8328));regs.lr=0x816A8410;regs.r[1]-=0x80;
    regs.r[3]=0x9000401C;regs.r[31]=area.address;regs.r[28]=9;regs.r[29]=0xA;REQUIRE(B::FastBeforeUnlock(cell,regs));
    regs.lr=0x816A8478;regs.r[1]+=0x80;regs.r[3]=0;REQUIRE(B::FastReturn(cell,regs,0x816A8328));
    REQUIRE(bridge.OuterReturned(cell,frame,0));CHECK(slot.load()==nullptr);CHECK_FALSE(bridge.SourceOnlyProducer(*origin));
    status=B::CellAdmissionStatus::kOpened;
    CHECK_FALSE(bridge.BeforeBroadcast(frame,source,9,slot,0xA,&status));
    CHECK(status==B::CellAdmissionStatus::kClosedHealthy);CHECK_FALSE(bridge.lost());
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin)); // queue wasn't cleared or falsely consumed
    const auto destroy=ListenerFrame(root,0x816A88E8,life);REQUIRE(bridge.BeforeDestruction(destroy,0x90004000));
    REQUIRE(bridge.DestructionReturned(0x90004000,*destroy));CHECK(bridge.SourceOnlyProducer(*origin));
  }
  static void PreparedOriginWorkLifetime(){
    using Bridge=NativeIdentityListenerBridge;using Ledger=NativeIdentityEventLedger;
    const Ledger::Root root{11,13,17};Bridge bridge(root);
    auto resource=std::make_shared<int>(23);std::weak_ptr<int> lifetime=resource;
    auto source=std::shared_ptr<const NativeIdentityEventSource>(
        new NativeIdentityEventSource(root,31,37,resource));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    REQUIRE(origin->provenance==Ledger::Provenance::kPreparedNativeRequest);
    REQUIRE(bridge.SourceOnlyProducer(*origin));
    auto wrong=*origin;wrong.provenance=Ledger::Provenance::kUnjoinedNativeRequest;
    CHECK_FALSE(bridge.SourceOnlyProducer(wrong));
    const Ledger::WorkKey key{root,41,43,47,0x90001000,0x90002000,
                              0x91000000,0x816CD70C,0x816CD6E8};
    auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(key,source,resource));
    const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin));
    auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);
    CHECK(facts->entry==source);CHECK_FALSE(facts->unknown);
    REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));
    REQUIRE(bridge.SourceOnlyProducer(*origin));
    REQUIRE(bridge.FinishSourceAfterDescendants(*origin));
    facts.reset();source.reset();whole.reset();resource.reset();
    CHECK_FALSE(lifetime.expired());
    // Actual host reaping moves references; they release only after its
    // returned retirement holder leaves the metadata-lock scope.
    auto retired=bridge.ReapOnHostOutsideGuestLocks();
    CHECK_FALSE(lifetime.expired());
    CHECK(retired.count>0);CHECK_FALSE(bridge.lost());
    retired={};CHECK(lifetime.expired());
  }
  static void InitialProducerCustodySurvivesProjectionAndReap(){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);
    auto life=std::make_shared<int>(23);std::weak_ptr<int> initial_lifetime=life;
    auto boot=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,1,0,life));
    auto initial=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,2,0,life));
    const auto boot_origin=bridge.AdoptSource(boot),initial_origin=bridge.AdoptSource(initial);
    REQUIRE(boot_origin);REQUIRE(initial_origin);REQUIRE(*boot_origin!=*initial_origin);
    std::optional<B::ProducerCustody> producer(B::ProducerCustody(bridge,initial,*initial_origin));
    auto current=initial;auto current_origin=*initial_origin;
    // A real pre-entry cancellation ends only the NEW producer. The existing
    // initial read source remains admitted for a later real publication.
    auto canceled=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,3,37,std::make_shared<int>(29)));
    const auto canceled_origin=bridge.AdoptSource(canceled);REQUIRE(canceled_origin);
    REQUIRE(bridge.FinishSourceAfterDescendants(*canceled_origin));
    canceled.reset();{auto retired=bridge.ReapOnHostOutsideGuestLocks();CHECK(retired.count>0);}
    CHECK(bridge.SourceOnlyProducer(*initial_origin));
    auto selected_life=std::make_shared<int>(31);
    auto selected=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,4,41,selected_life));
    const auto selected_origin=bridge.AdoptSource(selected);REQUIRE(selected_origin);
    current=selected;current_origin=*selected_origin; // genuine current projection advances
    CHECK(current!=producer->source());CHECK(current_origin!=producer->origin());
    CHECK_FALSE(bridge.FinishRetainedProducer(*producer,*selected_origin));
    CHECK_FALSE(bridge.FinishRetainedProducer(*producer,*boot_origin));
    B::ProducerCustody wrong_pair(bridge,selected,*initial_origin);
    CHECK_FALSE(bridge.FinishRetainedProducer(wrong_pair,*initial_origin));
    CHECK(bridge.SourceOnlyProducer(*initial_origin));CHECK(bridge.SourceOnlyProducer(*selected_origin));
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,2,*initial_origin,true));
    REQUIRE(bridge.OldSourceDataQuiescent(*initial_origin));
    REQUIRE(bridge.FinishRetainedProducer(*producer,*initial_origin));
    CHECK_FALSE(bridge.SourceProducerQuiescent(*initial_origin));
    CHECK(bridge.SourceOnlyProducer(*selected_origin)); // old finish cannot finish current
    CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0); // queue still owns its causal pin
    const L::WorkKey key{root,43,47,53,0x90001000,0x90002000,0x91000000,0x816CD70C,0x816CD6E8};
    auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,current,selected_life));
    const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
    const auto delivered=bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,0xA,2,*work);
    REQUIRE(delivered.retained_by_work);CHECK(delivered.origin==*initial_origin);
    auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);
    CHECK(facts->entry==selected);REQUIRE(facts->count==1);CHECK(facts->dequeued[0]==initial);
    REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));facts.reset();whole.reset();
    initial.reset();life.reset();
    {auto retired=bridge.ReapOnHostOutsideGuestLocks();REQUIRE(retired.count>0);CHECK_FALSE(initial_lifetime.expired());}
    // Repeated cleanup after real slot reuse is the SAME spent capability,
    // never another Finish of the replacement's producer reference.
    auto replacement=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,5,59,selected_life));
    const auto replacement_origin=bridge.AdoptSource(replacement);REQUIRE(replacement_origin);
    CHECK(replacement_origin->slot==initial_origin->slot);CHECK(replacement_origin->serial!=initial_origin->serial);
    REQUIRE(bridge.FinishRetainedProducer(*producer,*initial_origin));
    CHECK(bridge.SourceOnlyProducer(*replacement_origin));
    B other(root),restarted({root.kernel+1,root.domain+1,root.generation+1});
    CHECK_FALSE(other.FinishRetainedProducer(*producer,*initial_origin));
    CHECK_FALSE(restarted.FinishRetainedProducer(*producer,*initial_origin));
    CHECK_FALSE(other.lost());CHECK_FALSE(restarted.lost());
    producer.reset();CHECK_FALSE(initial_lifetime.expired()); // boot shares the real lifetime
    REQUIRE(bridge.FinishSourceAfterDescendants(*boot_origin));boot.reset();
    {auto retired=bridge.ReapOnHostOutsideGuestLocks();CHECK_FALSE(initial_lifetime.expired());}
    CHECK(initial_lifetime.expired());
    REQUIRE(bridge.FinishSourceAfterDescendants(*selected_origin));
    REQUIRE(bridge.FinishSourceAfterDescendants(*replacement_origin));
    REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*listener));CHECK_FALSE(bridge.lost());
  }
  static void OldQueuedHintsKeepLifetimeAfterProducer(){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);
    auto resource=std::make_shared<int>(23);std::weak_ptr<int> old_lifetime=resource;
    auto old=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,resource));
    const auto origin=bridge.AdoptSource(old);REQUIRE(origin);
    auto current_resource=std::make_shared<int>(29);
    auto current=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,current_resource));
    const auto current_origin=bridge.AdoptSource(current);REQUIRE(current_origin);
    const auto first=bridge.ledger_.RegisterListener(0x90004000),second=bridge.ledger_.RegisterListener(0x90006000);
    REQUIRE(first);REQUIRE(second);
    REQUIRE(bridge.ledger_.ReplaceQueued(*first,0x90005000,0xA,2,*origin,true));
    REQUIRE(bridge.ledger_.ReplaceQueued(*second,0x90007000,0xA,2,*origin,true));
    B::SourceProducerObservation cut;
    REQUIRE(bridge.OldSourceDataQuiescent(*origin,&cut));CHECK(cut.references==3);
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin)); // No queue reference discarded.
    // Scalar delivery/return is exercised through the real Bridge FastCell
    // and OuterReturned lifecycle in TypedScalarDelivery below. This case
    // retains two genuinely queued pins through producer Finish and transfer.
    REQUIRE(bridge.FinishSourceAfterDescendants(*origin));
    CHECK_FALSE(bridge.OldSourceDataQuiescent(*origin));CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0);
    // A later native whole consumer belongs to the current entry source.
    // Dequeue adds the old causal pin; it does not replace entry or mint a read.
    const L::WorkKey key{root,41,43,47,0x90001000,0x90002000,0x91000000,0x816CD70C,0x816CD6E8};
    auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,current,current_resource));
    const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
    const auto consumed=bridge.ledger_.TransferSuccessfulDequeue(*first,0x90005000,0xA,2,*work);
    REQUIRE(consumed.retained_by_work);CHECK(consumed.origin==*origin);
    auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);CHECK(facts->entry==current);
    CHECK(facts->count==1);CHECK(facts->dequeued[0]==old);CHECK_FALSE(facts->unknown);
    CHECK_FALSE(bridge.OldSourceDataQuiescent(*current_origin));
    old.reset();resource.reset();CHECK_FALSE(old_lifetime.expired());
    REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));
    facts.reset();whole.reset();CHECK_FALSE(old_lifetime.expired());
    // Native coalescing with an unowned publication releases only its old pin.
    REQUIRE(bridge.ledger_.ReplaceQueued(*second,0x90007000,0xA,2,{}));
    auto retired=bridge.ReapOnHostOutsideGuestLocks();CHECK(retired.count>0);CHECK_FALSE(old_lifetime.expired());
    retired={};CHECK(old_lifetime.expired());
    REQUIRE(bridge.OldSourceDataQuiescent(*current_origin));
    REQUIRE(bridge.FinishSourceAfterDescendants(*current_origin));
    REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*first));
    REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*second));CHECK_FALSE(bridge.lost());
  }
  static void CurrentProducerKeepsQueuedHints(){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);
    auto resource=std::make_shared<int>(23);std::weak_ptr<int> lifetime=resource;
    auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,resource));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,9,*origin,true));
    B::SourceProducerObservation held,strict;
    REQUIRE(bridge.SourceProducerQuiescent(*origin,&held));CHECK(held.mode==1);
    B::SourceProducerObservation old;
    REQUIRE(bridge.OldSourceDataQuiescent(*origin,&old));CHECK(old.mode==2);
    CHECK(old.evaluated==1023);CHECK(old.truth==1007);CHECK(old.references==2);
    CHECK(held.evaluated==255);CHECK(held.truth==239);CHECK(held.references_available);CHECK(held.references==2);
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin,&strict,true));CHECK(strict.mode==0);
    CHECK(strict.evaluated==31);CHECK(strict.truth==15);CHECK(strict.references==2);
    REQUIRE(strict.ownership);CHECK(strict.ownership->available);CHECK(strict.ownership->active_cells==0);
    CHECK(strict.ownership->queued==1);CHECK(strict.ownership->other_queued==0);
    CHECK(strict.ownership->typed_public_a_queued==1);CHECK(strict.ownership->work_entry_pins==0);
    CHECK(strict.ownership->work_dequeued_pins==0);CHECK(strict.ownership->first_queue_listener==listener->serial);
    // Genuine native coalescing replaces payload, not a completed consumer.
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,17,*origin));
    REQUIRE(bridge.SourceProducerQuiescent(*origin,&held));CHECK(held.references==2);
    CHECK_FALSE(bridge.OldSourceDataQuiescent(*origin,&old));CHECK(old.evaluated==511);
    CHECK(old.truth==239); // Untyped coalesced replacement is not public A.
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin,&strict,true));
    REQUIRE(strict.ownership);CHECK(strict.ownership->queued==1);
    CHECK(strict.ownership->typed_public_a_queued==0);CHECK(strict.ownership->other_queued==1);
    const L::WorkKey key{root,41,43,47,0x90001000,0x90002000,0x91000000,0x816CD70C,0x816CD6E8};
    auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,source,resource));
    const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
    // Calling ProducerHeld did not consume or replace the queued entry.
    const auto consumed=bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,0xA,17,*work);
    CHECK(consumed.native_success);CHECK(consumed.origin_known);CHECK(consumed.retained_by_work);
    auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);CHECK(facts->count==1);CHECK(facts->dequeued[0]==source);
    REQUIRE(bridge.WorkFor(*work));CHECK_FALSE(bridge.WorkFor(*work)->finished);
    CHECK_FALSE(bridge.SourceProducerQuiescent(*origin,&held));CHECK(held.consumer_reason==4);
    CHECK_FALSE(bridge.OldSourceDataQuiescent(*origin,&old));CHECK(old.consumer_reason==4);
    CHECK(held.consumer_work==work->serial);CHECK_FALSE(bridge.SourceOnlyProducer(*origin,&strict,true));
    REQUIRE(strict.ownership);CHECK(strict.ownership->available);CHECK(strict.references==3);
    CHECK(strict.ownership->queued==0);CHECK(strict.ownership->work_entry_pins==1);
    CHECK(strict.ownership->work_dequeued_pins==1);CHECK(strict.ownership->first_work==work->serial);
    CHECK(strict.ownership->first_owner_invocation==key.invocation);
    CHECK(strict.ownership->first_work_function==key.function);CHECK(strict.ownership->unknown_works==0);
    // Entered consumption remains a separate real whole-work obligation.
    REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));REQUIRE(bridge.SourceOnlyProducer(*origin));
    REQUIRE(bridge.FinishSourceAfterDescendants(*origin));CHECK_FALSE(bridge.SourceProducerQuiescent(*origin));
    REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*listener));
    facts.reset();whole.reset();source.reset();resource.reset();CHECK_FALSE(lifetime.expired());
    auto retired=bridge.ReapOnHostOutsideGuestLocks();CHECK(retired.count>0);CHECK_FALSE(lifetime.expired());
    retired={};CHECK(lifetime.expired());CHECK_FALSE(bridge.lost());
  }
  static void CurrentProducerRejectsUnownedOrFinished(uint32_t variant){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto resource=std::make_shared<int>(23);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,resource));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    auto wrong=*origin;
    if(variant==0)++wrong.root.generation;
    if(variant==1)++wrong.serial;
    if(variant==2)++wrong.slot;
    if(variant==3)wrong.provenance=L::Provenance::kUnjoinedNativeRequest;
    if(variant<4){CHECK_FALSE(bridge.SourceProducerQuiescent(wrong));CHECK_FALSE(bridge.OldSourceDataQuiescent(wrong));CHECK_FALSE(bridge.lost());return;}
    REQUIRE(variant==4);
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,9,*origin));
    REQUIRE(bridge.SourceProducerQuiescent(*origin));REQUIRE(bridge.FinishSourceAfterDescendants(*origin));
    B::SourceProducerObservation observation;
    CHECK_FALSE(bridge.SourceProducerQuiescent(*origin,&observation));CHECK(observation.mode==1);
    CHECK_FALSE(bridge.OldSourceDataQuiescent(*origin));
    CHECK(observation.evaluated==15);CHECK(observation.truth==7);CHECK_FALSE(observation.references_available);
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin));CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0);
    // The unread queued hint still owns its lifetime after producer completion.
    REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*listener));
    auto retired=bridge.ReapOnHostOutsideGuestLocks();CHECK(retired.count>0);CHECK_FALSE(bridge.lost());
  }
  static void CurrentSourceOrphanStillRefuses(bool unrelated){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto resource=std::make_shared<int>(23);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,resource));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    auto consumed_origin=*origin;
    if(unrelated){
      const auto other=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,resource));
      const auto other_origin=bridge.AdoptSource(other);REQUIRE(other_origin);consumed_origin=*other_origin;
    }
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,9,consumed_origin));
    REQUIRE(bridge.SourceProducerQuiescent(*origin));
    const auto result=bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,0xA,9,{});
    CHECK(result.native_success);CHECK(result.origin_known);CHECK_FALSE(result.retained_by_work);
    B::SourceProducerObservation observation;
    CHECK_FALSE(bridge.SourceProducerQuiescent(consumed_origin,&observation));CHECK(observation.orphan_available);CHECK(observation.orphaned);
    CHECK_FALSE(bridge.OldSourceDataQuiescent(consumed_origin));
    CHECK(bridge.SourceProducerQuiescent(*origin)==unrelated); // Orphan state belongs to its exact origin.
    REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*listener));
    CHECK_FALSE(bridge.SourceProducerQuiescent(consumed_origin)); // Destruction is not downstream completion.
    REQUIRE(bridge.FinishSourceAfterDescendants(consumed_origin));CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0);
    CHECK_FALSE(bridge.lost());
  }
  static void CurrentSourceRejectsCriticalOrUnknown(bool critical){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto resource=std::make_shared<int>(23);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,resource));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);REQUIRE(bridge.SourceProducerQuiescent(*origin));
    B::SourceProducerObservation observation;
    if(critical){
      auto frame=ListenerFrame(root,0x816A8C00,resource);std::atomic<void*> slot{nullptr};std::array<uint8_t,4> selected{};
      auto* cell=bridge.BeforeGetNext(frame,{},0x10000180,0,0,0,selected.data(),nullptr,slot);REQUIRE(cell);
      CHECK_FALSE(bridge.SourceProducerQuiescent(*origin,&observation));CHECK(observation.consumer_reason==1);CHECK(observation.active_cell>0);
      CHECK_FALSE(bridge.OldSourceDataQuiescent(*origin));
      REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0xC0000008,0));REQUIRE(bridge.OuterReturned(cell,frame,0));
    }else{
      const L::WorkKey key{root,41,43,47,0x90001000,0x90002000,0x91000000,0x816CD70C,0x816CD6E8};
      const auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,{},resource));
      const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
      CHECK_FALSE(bridge.SourceProducerQuiescent(*origin,&observation));CHECK(observation.consumer_reason==3);CHECK(observation.consumer_work==work->serial);
      CHECK_FALSE(bridge.OldSourceDataQuiescent(*origin));
      REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));
    }
    CHECK(bridge.SourceProducerQuiescent(*origin));CHECK_FALSE(bridge.lost());
  }
  static void CurrentSourceNestedCrossOriginConsumer(bool duplicate,bool system_frame=false,bool finite_task=false){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto resource=std::make_shared<int>(23);
    const auto source_a=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,resource));
    const auto source_b=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,32,38,resource));
    const auto origin_a=bridge.AdoptSource(source_a),origin_b=bridge.AdoptSource(source_b);REQUIRE(origin_a);REQUIRE(origin_b);
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    L::WorkKey parent_key{root,41,43,47,0x90001000,0x90002000,0x71000000,0x816CD70C,0x816CD6E8};
    if(system_frame){
      parent_key.task=0;parent_key.task_allocation_generation=0;
      parent_key.owner_kind=L::WorkOwnerKind::kSystemThreadFrame;
      parent_key.function=0x8187DE10;
    }
    if(finite_task)parent_key.function=0x8187DE10;
    const auto parent=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(parent_key,source_a,resource));
    const auto parent_work=bridge.AdoptWholeWork(parent);REQUIRE(parent_work);
    const auto consume_b=[&](L::Work work){
      REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,9,*origin_b));
      const auto consumed=bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,0xA,9,work);
      CHECK(consumed.origin_known);CHECK(consumed.retained_by_work);
    };
    if(duplicate)consume_b(*parent_work);
    auto child_key=parent_key;child_key.invocation=44;child_key.sp-=system_frame?0x90:0x100;
    child_key.function=system_frame || finite_task?0x81879EA0:0x816921C8;
    if(system_frame || finite_task)child_key.lr=0x8187DEC0;
    const auto child=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(child_key,source_a,resource,parent));
    const auto child_work=bridge.AdoptWholeWork(child);REQUIRE(child_work);consume_b(*child_work);
    B::SourceProducerObservation observation;
    CHECK_FALSE(bridge.SourceProducerQuiescent(*origin_b,&observation));CHECK(observation.consumer_reason==5);
    CHECK(observation.consumer_work==(duplicate?parent_work->serial:child_work->serial));
    CHECK_FALSE(bridge.SourceProducerQuiescent(*origin_a));
    auto facts=bridge.CaptureDependencies(*child_work);REQUIRE(facts);CHECK(facts->entry==source_a);
    CHECK(facts->count==1);CHECK(facts->dequeued[0]==source_b);CHECK_FALSE(facts->unknown);
    REQUIRE(bridge.FinishWholeWorkNormally(*child_work,child_key,child));
    CHECK_FALSE(bridge.WorkFor(*child_work));
    facts=bridge.CaptureDependencies(*parent_work);REQUIRE(facts);CHECK(facts->entry==source_a);
    CHECK(facts->count==1);CHECK(facts->dequeued[0]==source_b);CHECK_FALSE(facts->unknown);
    // Child normal return transferred B's dependency into the earlier parent;
    // dedup may release the child's extra pin, never the parent's real duty.
    CHECK_FALSE(bridge.SourceProducerQuiescent(*origin_b,&observation));CHECK(observation.consumer_reason==5);
    CHECK(observation.consumer_work==parent_work->serial);CHECK_FALSE(bridge.SourceOnlyProducer(*origin_b));
    REQUIRE(bridge.FinishWholeWorkNormally(*parent_work,parent_key,parent));
    CHECK(bridge.SourceProducerQuiescent(*origin_b));CHECK(bridge.SourceOnlyProducer(*origin_b));
    CHECK(bridge.SourceProducerQuiescent(*origin_a));
    REQUIRE(bridge.FinishSourceAfterDescendants(*origin_a));REQUIRE(bridge.FinishSourceAfterDescendants(*origin_b));
    REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*listener));CHECK_FALSE(bridge.lost());
  }
  static void DroppedQueueIsNotConsumption(bool destroy){
    using Bridge=NativeIdentityListenerBridge;using Ledger=NativeIdentityEventLedger;
    const Ledger::Root root{11,13,17};Bridge bridge(root);
    auto lifetime=std::make_shared<int>(1);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(
        new NativeIdentityEventSource(root,31,37,lifetime));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,9,*origin));
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin));
    if(destroy)REQUIRE(bridge.ledger_.RetireListenerAfterNativeDestruction(*listener));
    else REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,9,{}));
    // Both genuine lifecycle operations release the origin ref without any
    // consumer. Equal data and a drained refcount cannot authorize new pixels.
    REQUIRE(bridge.SourceOnlyProducer(*origin));
    const Ledger::WorkKey key{root,41,43,47,0x90001000,0x90002000,
                              0x91000000,0x816CD70C,0x816CD6E8};
    const auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(key,source,lifetime));
    const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
    const auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);
    CHECK(facts->entry==source);CHECK(facts->count==0);
    CHECK_FALSE(facts->unknown);
    REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));
    CHECK(bridge.SourceOnlyProducer(*origin));
    // Production requires an exact transferred dequeued source at CD920;
    // neither this entry source nor the two drain cases supplies that fact.
  }
  static void UnknownCannotBePrepared(){
    using Bridge=NativeIdentityListenerBridge;using Ledger=NativeIdentityEventLedger;
    const Ledger::Root root{11,13,17};Bridge bridge(root);
    auto token=std::make_shared<int>(1);
    const Ledger::WorkKey key{root,41,43,47,0x90001000,0x90002000,
                              0x91000000,0x816CD70C,0x816CD6E8};
    const auto unknown=std::shared_ptr<const NativeIdentityEventSource>(
        new NativeIdentityEventSource(root,31,37,token));
    const auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(key,unknown,token));
    // A matching source serial/owner enum never replaces actual one-shot
    // issuer registration, including for prepared sources.
    CHECK_FALSE(bridge.AdoptWholeWork(whole));CHECK(bridge.lost());
  }
  // Production private metadata only. These register/frame tokens do not
  // pretend to execute native PPC or mint an Object Manager reference.
  static void QuartetArguments(){
    using B=NativeIdentityListenerBridge;B::Registers r;
    r.r[3]=0xFFFFFFFF81A821FCull;r.r[4]=0x300B1018;r.r[5]=0;r.r[6]=0x707DF694;
    const auto registration=B::DecodeOuter(0x816A8498,r);REQUIRE(registration);
    CHECK(registration->manager==0x81A821FC);CHECK(registration->object==0x300B1018);
    CHECK(registration->object!=registration->manager);CHECK(registration->handle==0);
    r.r[3]=0x1234567881A821FCull;CHECK_FALSE(B::DecodeOuter(0x816A8498,r));
    r.r[3]=0x81A821FC;r.r[4]=0x12345678300B1018ull;CHECK_FALSE(B::DecodeOuter(0x816A8498,r));
    r.r[3]=0xFFFFFFFF90004000ull;const auto destruction=B::DecodeOuter(0x816A88E8,r);
    REQUIRE(destruction);CHECK(destruction->object==0x90004000);
    r.r[3]=0xFFFFFFFF8000000Aull;r.r[4]=0xFFFFFFFF80000009ull;
    const auto broadcast=B::DecodeOuter(0x816A93C8,r);REQUIRE(broadcast);
    CHECK(broadcast->event==0x8000000A);CHECK(broadcast->data==0x80000009);
    r.r[3]=0x01000180;r.r[4]=0xFFFFFFFF8000000Aull;r.r[5]=0xFFFFFFFF90006000ull;r.r[6]=0;
    const auto next=B::DecodeOuter(0x816A8C00,r);REQUIRE(next);
    CHECK(next->handle==0x01000180);CHECK(next->object==0);CHECK(next->filter==0x8000000A);
    CHECK(next->id_output==0x90006000);CHECK(next->data_output==0);
    r.r[5]=0;CHECK(B::DecodeOuter(0x816A8C00,r)); // native rejects null ID later
    r.r[6]=0x1234567890006000ull;CHECK_FALSE(B::DecodeOuter(0x816A8C00,r));
  }
  static std::shared_ptr<const NativeIdentityListenerBridge::Frame> ListenerFrame(
      NativeIdentityListenerBridge::Root root,uint32_t function,std::shared_ptr<const void> lifetime){
    return std::shared_ptr<const NativeIdentityListenerBridge::Frame>(
      new NativeIdentityListenerBridge::Frame(root,101,103,107,41,0x90002000,0x707DF640,
          function==0x816A8498?0x816A8BE0:0x816CD70C,function,std::move(lifetime)));
  }
  static NativeIdentityListenerBridge::Registers FastRegisters(){
    NativeIdentityListenerBridge::Registers r;r.context=101;r.thread_state=103;r.kernel=107;
    r.r[13]=0xFFFFFFFF90002000ull;return r;
  }
  static void CanonicalRegistrationAndEnqueue(){
    using B=NativeIdentityListenerBridge;const B::Root root{11,13,17};B bridge(root);
    auto life=std::make_shared<int>(1);const auto registration=ListenerFrame(root,0x816A8498,life);
    const B::Area area{0x90005000,0xA,0xA};
    const auto listener=bridge.BeforeRegistration(registration,0xFFFFFFFF81A821FCull,
        0xFFFFFFFF90004000ull,std::span(&area,1),life);REQUIRE(listener);
    REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const auto broadcast=ListenerFrame(root,0x816A93C8,life);std::atomic<void*> cell_slot{nullptr};
    auto* cell=bridge.BeforeBroadcast(broadcast,{},0xFFFFFFFF80000009ull,cell_slot);REQUIRE(cell);
    auto r=FastRegisters();r.lr=0xFFFFFFFF816A8478ull;r.r[1]=0x707DF640-0xF0;
    r.r[3]=0xFFFFFFFF90005000ull;r.r[5]=0x80000009;
    REQUIRE(B::FastEnter(cell,r,0x816A8328));
    r.lr=0xFFFFFFFF816A8410ull;r.r[1]-=0x80;r.r[3]=0xFFFFFFFF9000401Cull;
    r.r[31]=0xFFFFFFFF90005000ull;r.r[28]=0xFFFFFFFF80000009ull;r.r[29]=0xA;
    REQUIRE(B::FastBeforeUnlock(cell,r));
    r.lr=0x816A8478;r.r[1]+=0x80;r.r[3]=0;REQUIRE(B::FastReturn(cell,r,0x816A8328));
    REQUIRE(bridge.OuterReturned(cell,broadcast,0));CHECK(cell_slot.load()==nullptr);
    CHECK_FALSE(bridge.lost()); // no Prepared source or consumption was minted
  }
  static void GetNextReferenceFailure(bool missing){
    using B=NativeIdentityListenerBridge;const B::Root root{11,13,17};B bridge(root);
    auto life=std::make_shared<int>(1);const auto next=ListenerFrame(root,0x816A8C00,life);
    std::atomic<void*> cell_slot{nullptr};std::array<uint8_t,4> selected{};
    auto* cell=bridge.BeforeGetNext(next,{},0x10000180,0,0,0,selected.data(),nullptr,cell_slot);REQUIRE(cell);
    if(missing){CHECK_FALSE(bridge.OuterReturned(cell,next,0));CHECK(bridge.lost());CHECK(cell_slot.load()==cell);}
    else{
      REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0xC0000008,0));
      REQUIRE(bridge.OuterReturned(cell,next,0));CHECK_FALSE(bridge.lost());CHECK(cell_slot.load()==nullptr);
    }
  }
  static void GetNextReferenceLifetime(bool wrong_area,bool omit_release){
    using B=NativeIdentityListenerBridge;const B::Root root{11,13,17};B bridge(root);
    auto life=std::make_shared<int>(1);const auto registration=ListenerFrame(root,0x816A8498,life);
    const B::Area area{0x90005000,0xA,0xA},other{0x90007000,0xA,0xA};
    const auto listener=bridge.BeforeRegistration(registration,0x81A821FC,0x90004000,std::span(&area,1),life);
    REQUIRE(listener);REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const auto second=bridge.BeforeRegistration(registration,0x81A821FC,0x90006000,std::span(&other,1),life);
    REQUIRE(second);REQUIRE(bridge.RegistrationReturned(*second,*registration));
    const auto next=ListenerFrame(root,0x816A8C00,life);std::atomic<void*> cell_slot{nullptr};
    std::array<uint8_t,4> selected{};
    auto* cell=bridge.BeforeGetNext(next,{},0x10000180,0,0,0,selected.data(),nullptr,cell_slot);REQUIRE(cell);
    REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0,0x90004000));
    auto r=FastRegisters();r.lr=0x816A8C94;r.r[1]=0x707DF640-0x90;
    r.r[3]=wrong_area?other.address:area.address;r.r[4]=0;r.r[5]=0;r.r[6]=0;r.r[7]=0;
    if(wrong_area){CHECK_FALSE(B::FastEnter(cell,r,0x816A8648));CHECK(bridge.lost());return;}
    REQUIRE(B::FastEnter(cell,r,0x816A8648));
    r.r[3]=0xFFFFFFFF80070057ull;REQUIRE(B::FastReturn(cell,r,0x816A8648));
    if(omit_release){CHECK_FALSE(bridge.OuterReturned(cell,next,0));CHECK(bridge.lost());CHECK(cell_slot.load()==cell);return;}
    REQUIRE(bridge.BeforeReferenceRelease(cell,0x90004000));
    auto destroying=FastRegisters();CHECK(bridge.AllowsReferenceReleaseDestruction(cell,destroying,0x90004000));
    CHECK_FALSE(bridge.AllowsReferenceReleaseDestruction(cell,destroying,0x90006000));
    const auto destruction=ListenerFrame(root,0x816A88E8,life);
    REQUIRE(bridge.BeforeDestruction(destruction,0xFFFFFFFF90004000ull));
    REQUIRE(bridge.DestructionReturned(0x90004000,*destruction));
    // The paired return is valid AFTER metadata for actual destruction, with
    // no native-body reread or active-row remint. This is metadata, not PPC.
    REQUIRE(bridge.ReferenceReleaseReturned(cell,0x90004000));
    REQUIRE(bridge.OuterReturned(cell,next,0));CHECK_FALSE(bridge.lost());CHECK(cell_slot.load()==nullptr);
  }

  // Actual private metadata failure paths only; these do not execute native
  // PPC, take native locks or assert that V41 followed the native no-op branch.
  static void FirstFastFailureRetention(bool wrong_context){
    using B=NativeIdentityListenerBridge;const B::Root root{11,13,17};B bridge(root);
    auto life=std::make_shared<int>(1);const auto registration=ListenerFrame(root,0x816A8498,life);
    const B::Area area{0x90005000,0xA,0xA};
    const auto listener=bridge.BeforeRegistration(registration,0x81A821FC,0x90004000,std::span(&area,1),life);
    REQUIRE(listener);REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const auto broadcast=ListenerFrame(root,0x816A93C8,life);std::atomic<void*> slot{nullptr};
    auto* cell=bridge.BeforeBroadcast(broadcast,{},9,slot);REQUIRE(cell);
    auto r=FastRegisters();r.lr=0x816A8478;r.r[1]=0x707DF640-0xF0;
    r.r[3]=area.address;r.r[4]=0x8000000A;r.r[5]=9;
    if(wrong_context){
      ++r.context;CHECK_FALSE(B::FastEnter(cell,r,0x816A8328));
    }else{
      // One Broadcast can complete one area and enter another. Do not label
      // completed-inner metadata as the next inner's selected/current values.
      const auto second=r;r.r[4]=0xA;REQUIRE(B::FastEnter(cell,r,0x816A8328));
      r.lr=0x816A8410;r.r[1]-=0x80;r.r[3]=0x9000401C;
      r.r[31]=area.address;r.r[28]=9;r.r[29]=0xA;REQUIRE(B::FastBeforeUnlock(cell,r));
      r.lr=0x816A8478;r.r[1]+=0x80;r.r[3]=0;REQUIRE(B::FastReturn(cell,r,0x816A8328));
      CHECK(cell->diagnostic_inner_function_==0);CHECK_FALSE(cell->diagnostic_selected_available_);
      r=second;REQUIRE(B::FastEnter(cell,r,0x816A8328));
      r.r[3]=0;CHECK_FALSE(B::FastReturn(cell,r,0x816A8328));
    }
    const auto first=bridge.FirstFailure();REQUIRE(first.state==2);
    CHECK(first.data.reason==(wrong_context?15u:17u));CHECK(first.data.line!=0);
    CHECK(first.data.r4==0x8000000A);CHECK(uint32_t(first.ledger>>32)==12);
    if(wrong_context){
      CHECK(first.data.available==1);CHECK(first.data.inner==0);CHECK(first.data.area==0);
      CHECK(first.data.frame_function==0);CHECK(first.data.incoming_id==0);
    }else{
      CHECK(first.data.available==15);CHECK(first.data.inner==0x816A8328);
      CHECK(first.data.area==area.address);CHECK(first.data.linearized==0);
      CHECK(first.data.incoming_id==0x8000000A);CHECK(first.data.frame_function==0x816A93C8);
    }
    // A later outer symptom cannot replace the original fast failure/tuple.
    CHECK_FALSE(bridge.OuterReturned(cell,broadcast,0x81A82204));
    const auto later=bridge.FirstFailure();CHECK(later.state==2);
    CHECK(later.data.line==first.data.line);CHECK(later.data.reason==first.data.reason);
    CHECK(later.data.available==first.data.available);CHECK(later.data.r4==first.data.r4);
    CHECK(later.ledger==first.ledger);CHECK(slot.load()==cell);CHECK(bridge.lost());
  }
  static void FirstLedgerFailureRetention(){
    using B=NativeIdentityListenerBridge;using Ledger=NativeIdentityEventLedger;
    Ledger invalid(Ledger::Root{});REQUIRE(invalid.lost());CHECK(uint32_t(invalid.FirstFailure()>>32)==1);
    const B::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(1);
    const auto registration=ListenerFrame(root,0x816A8498,life);
    const B::Area area{0x90005000,0xA,0xA};
    const auto listener=bridge.BeforeRegistration(registration,0x81A821FC,0x90004000,std::span(&area,1),life);
    REQUIRE(listener);REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const auto broadcast=ListenerFrame(root,0x816A93C8,life);std::atomic<void*> slot{nullptr};
    auto* cell=bridge.BeforeBroadcast(broadcast,{},7,slot);REQUIRE(cell);
    // Invoke the actual queue invariant failure, independently of Bridge Lose.
    CHECK_FALSE(bridge.ledger_.ReplaceQueued(*listener,area.address,0,7,{}));
    const auto original=bridge.ledger_.FirstFailure();REQUIRE(original!=0);
    CHECK(uint32_t(original>>32)==6);CHECK(bridge.FirstFailure().state==0);
    CHECK_FALSE(bridge.OuterReturned(cell,broadcast,0x81A82204));
    const auto symptom=bridge.FirstFailure();REQUIRE(symptom.state==2);
    CHECK(symptom.data.reason==18);CHECK(symptom.data.checks==7);
    CHECK(symptom.data.ledger_at_capture==original);CHECK(symptom.ledger==original);
    CHECK_FALSE(bridge.ledger_.FinishProducerNormally({}));
    CHECK(bridge.FirstFailure().ledger==original);
    CHECK(slot.load()==cell);CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0);
    // The actual lost observer is concurrent and immediately emits a later
    // external symptom. Acquire/release publication must expose the original
    // internal queue site before that observer can win the first-site latch.
    Ledger concurrent(root);const auto other=concurrent.RegisterListener(0x90008000);REQUIRE(other);
    bool observed_loss=false;
    std::thread observer([&]{
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
      while(!concurrent.lost() && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
      observed_loss=concurrent.lost();
      if(observed_loss)concurrent.Lose(0x1234,12);
    });
    const bool replaced=concurrent.ReplaceQueued(*other,0x90009000,0,7,{});
    observer.join();CHECK_FALSE(replaced);CHECK(observed_loss);
    CHECK(uint32_t(concurrent.FirstFailure()>>32)==6);
    CHECK(uint32_t(concurrent.FirstFailure())!=0x1234);
  }

  // Golden wrapping-word results are from authenticated8354/8360 and the
  // existing CPU mask contract. This fixture executes metadata, not PPC.
  static void NativeWordQueueKey(unsigned mode){
    using B=NativeIdentityListenerBridge;using Ledger=NativeIdentityEventLedger;
    const B::Root root{11,13,17};B bridge(root);auto life=std::make_shared<int>(1);
    const uint32_t input=mode==1?0x12345678u:0x02000003u;
    const uint32_t key=mode==1?0x12005678u:0x02000003u;
    const uint64_t computed=mode==1?0x1234567812005678ull:0x0200000302000003ull;
    auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,0,life));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    CHECK(origin->provenance==Ledger::Provenance::kUnjoinedNativeRequest);
    const auto registration=ListenerFrame(root,0x816A8498,life);
    const B::Area area{0x90005000,key,key};
    const auto listener=bridge.BeforeRegistration(registration,0x81A821FC,0x90004000,std::span(&area,1),life);
    REQUIRE(listener);REQUIRE(bridge.RegistrationReturned(*listener,*registration));
    const auto broadcast=ListenerFrame(root,0x816A93C8,life);std::atomic<void*> slot{nullptr};
    auto* cell=bridge.BeforeBroadcast(broadcast,source,0,slot);REQUIRE(cell);
    auto r=FastRegisters();r.lr=0x816A8478;r.r[1]=0x707DF640-0xF0;r.r[3]=area.address;r.r[4]=input;r.r[5]=0;
    REQUIRE(B::FastEnter(cell,r,0x816A8328));
    r.lr=0x816A8410;r.r[1]-=0x80;r.r[3]=0x9000401C;r.r[31]=area.address;r.r[28]=0;r.r[29]=computed;
    if(mode==2)r.r[29]=0x0200000302000004ull; // Wrong low word still fails range.
    if(mode==3)r.r[31]=0x1234567890005000ull; // Pointer high bits remain strict.
    const uint64_t raw=r.r[29];
    if(mode>=2){CHECK_FALSE(B::FastBeforeUnlock(cell,r));CHECK(bridge.lost());CHECK(r.r[29]==raw);return;}
    REQUIRE(B::FastBeforeUnlock(cell,r));CHECK(r.r[29]==raw);CHECK_FALSE(bridge.SourceOnlyProducer(*origin));
    r.lr=0x816A8478;r.r[1]+=0x80;r.r[3]=0;REQUIRE(B::FastReturn(cell,r,0x816A8328));
    REQUIRE(bridge.OuterReturned(cell,broadcast,0x81A82204));CHECK(slot.load()==nullptr);
    const Ledger::WorkKey work_key{root,41,43,41,0x90002000,0x90003000,0x707DF740,0x816CD70C,0x816CD6E8};
    auto whole=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(work_key,source,life));
    const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
    // The actual production queue metadata consumer selects the same native
    // low32 key. Its unjoined origin transfers to the retained whole work.
    const auto dequeued=bridge.ledger_.TransferSuccessfulDequeue(*listener,area.address,key,0,*work);
    CHECK(dequeued.origin==*origin);CHECK(dequeued.retained_by_work);
    const auto facts=bridge.CaptureDependencies(*work);REQUIRE(facts);CHECK(facts->count==1);
    CHECK(facts->dequeued[0]==source);CHECK_FALSE(bridge.SourceOnlyProducer(*origin));
    REQUIRE(bridge.FinishWholeWorkNormally(*work,work_key,whole));CHECK(bridge.SourceOnlyProducer(*origin));
    CHECK_FALSE(bridge.lost());CHECK(bridge.FirstFailure().state==0);
  }

  static void ReturnedWholeWorkBurstAndHostLifetime(){
    using B=NativeIdentityListenerBridge;using Ledger=NativeIdentityEventLedger;
    using Source=NativeIdentityEventWorkSource;const B::Root root{11,13,17};
    bool guest_phase=true;size_t destroyed=0;B bridge(root);
    std::vector<std::shared_ptr<const Source>> returned_owners;
    std::vector<std::weak_ptr<const Source>> sources;
    std::vector<std::weak_ptr<int>> lifetimes;
    const auto origin_source=std::shared_ptr<const NativeIdentityEventSource>(
        new NativeIdentityEventSource(root,31,37,std::make_shared<int>(1)));
    const auto origin=bridge.AdoptSource(origin_source);REQUIRE(origin);
    // More than the old128 queued duplicates, without any Bootstrap reap.
    // No native Task, readiness or GPU completion is manufactured here.
    constexpr size_t burst=Ledger::kListeners*3;
    for(size_t i=0;i<burst;++i){
      auto lifetime=std::shared_ptr<int>(new int(1),[&](int* p){
        CHECK_FALSE(guest_phase);++destroyed;delete p;
      });
      lifetimes.push_back(lifetime);
      const Ledger::WorkKey key{root,41,43+i,41,0x90002000,0x90003000,
                                0x707DF740,0x816CD70C,0x816CD6E8};
      auto whole=std::shared_ptr<const Source>(new Source(key,origin_source,lifetime));
      sources.push_back(whole);const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
      CHECK_FALSE(bridge.SourceOnlyProducer(*origin));
      REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));
      REQUIRE(bridge.SourceOnlyProducer(*origin));
      CHECK_FALSE(bridge.works_[work->slot].source);CHECK_FALSE(bridge.lost());
      returned_owners.push_back(std::move(whole));CHECK_FALSE(whole);
      lifetime.reset();CHECK(destroyed==0);
    }
    CHECK(returned_owners.size()==burst);
    for(const auto& value:bridge.retired_)CHECK_FALSE(value);
    for(const auto& weak:sources)CHECK_FALSE(weak.expired());
    for(const auto& weak:lifetimes)CHECK_FALSE(weak.expired());
    // Mirror the existing outer State::Work host queue: last owners release
    // only here, outside guest/GCRC and all bridge/owner mutexes.
    guest_phase=false;returned_owners.clear();CHECK(destroyed==burst);
    for(const auto& weak:sources)CHECK(weak.expired());
    for(const auto& weak:lifetimes)CHECK(weak.expired());
    auto retired=bridge.ReapOnHostOutsideGuestLocks();CHECK(retired.count==0);
    CHECK_FALSE(bridge.lost());CHECK(bridge.SourceOnlyProducer(*origin));
  }
  static void WholeWorkReturnRequiresRetainedOwner(uint32_t mode){
    using B=NativeIdentityListenerBridge;using Source=NativeIdentityEventWorkSource;
    const B::Root root{11,13,17};bool guest_phase=true;size_t destroyed=0;
    std::weak_ptr<int> weak;
    {
      B bridge(root);auto lifetime=std::shared_ptr<int>(new int(1),[&](int* p){
        CHECK_FALSE(guest_phase);++destroyed;delete p;
      });weak=lifetime;
      const B::WorkKey key{root,41,43,41,0x90002000,0x90003000,
                           0x707DF740,0x816CD70C,0x816CD6E8};
      auto whole=std::shared_ptr<const Source>(new Source(key,{},lifetime));
      const auto work=bridge.AdoptWholeWork(whole);REQUIRE(work);
      const auto other=std::shared_ptr<const Source>(new Source(key,{},lifetime));
      std::shared_ptr<const Source> candidate;auto actual=key;
      std::atomic<void*> cell_slot{nullptr};std::array<uint8_t,4> selected{};
      std::shared_ptr<const B::Frame> cell_frame;
      if(mode==0)candidate=other; // Value-equal fields are not the same source.
      if(mode==1)candidate=std::shared_ptr<const Source>(whole.get(),[](const Source*){});
      if(mode==2)candidate=std::shared_ptr<const Source>(whole,other.get());
      if(mode==4){
        cell_frame=ListenerFrame(root,0x816A8C00,lifetime);
        REQUIRE(bridge.BeforeGetNext(cell_frame,*work,0x10000180,0,0,0,
            selected.data(),nullptr,cell_slot));
        REQUIRE(bridge.works_[work->slot].cells.load()==1);
      }
      if(mode==5){candidate=whole;actual.invocation++;}
      if(mode==6){
        REQUIRE(bridge.FinishWholeWorkNormally(*work,key,whole));candidate=whole;
      }
      if(mode==4)candidate=whole;
      // mode3 passes the bridge's own member; mode7 supplies no owner.
      const bool finished=mode==3?
          bridge.FinishWholeWorkNormally(*work,actual,bridge.works_[work->slot].source):
          bridge.FinishWholeWorkNormally(*work,actual,candidate);
      CHECK_FALSE(finished);CHECK(bridge.lost());
      if(mode==6)CHECK_FALSE(bridge.works_[work->slot].source);
      else CHECK(bridge.works_[work->slot].source==whole);
      if(mode==4)CHECK(cell_slot.load()!=nullptr);
      CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0);
      // Partial/refused returns retain the exact source/cell. Even replay's
      // already-returned source stays in its caller-owned host retirement.
      auto returned_owner=std::move(whole);CHECK_FALSE(whole);REQUIRE(returned_owner);
      CHECK_FALSE(weak.expired());CHECK(destroyed==0);
      guest_phase=false;
    }
    CHECK(weak.expired());CHECK(destroyed==1);
  }
  static void ReturnedCellReuseAndHostLifetime(){
    using B=NativeIdentityListenerBridge;const B::Root root{11,13,17};B bridge(root);
    bool guest_phase=true;size_t destroyed=0;
    std::vector<std::shared_ptr<const B::Frame>> returned_owners;
    std::vector<std::weak_ptr<int>> lifetimes;
    std::atomic<void*> slot{nullptr};std::array<uint8_t,4> selected{};
    // Real production return checks, with no host bridge reaping or Step
    // between these calls. The old implementation exhausted its 64 slots.
    for(size_t i=0;i<B::kCells*3;++i){
      auto lifetime=std::shared_ptr<int>(new int(1),[&](int* p){
        CHECK_FALSE(guest_phase);++destroyed;delete p;
      });
      lifetimes.push_back(lifetime);
      auto frame=ListenerFrame(root,0x816A8C00,lifetime);
      auto* cell=bridge.BeforeGetNext(frame,{},0x10000180,0,0,0,selected.data(),nullptr,slot);
      REQUIRE(cell);REQUIRE(slot.load()==cell);
      REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0xC0000008,0));
      REQUIRE(bridge.OuterReturned(cell,frame,0));REQUIRE(slot.load()==nullptr);
      // Mirror the actual final-use handoff: no retiring local survives in
      // guest code after the queue becomes eligible for host destruction.
      returned_owners.push_back(std::move(frame));CHECK_FALSE(frame);
      lifetime.reset();CHECK(destroyed==0);CHECK_FALSE(bridge.lost());
    }
    CHECK(returned_owners.size()==B::kCells*3);
    for(const auto& lifetime:lifetimes)CHECK_FALSE(lifetime.expired());
    // Slot reuse itself released no frame/resource. The existing host-owned
    // normal-return queue destroys these exact owners only outside guest work.
    guest_phase=false;returned_owners.clear();CHECK(destroyed==B::kCells*3);
    for(const auto& lifetime:lifetimes)CHECK(lifetime.expired());
    // The synchronous maintenance cut can repeat while the larger native
    // operation remains pending; owners do not accumulate for its duration.
    for(size_t batch=0;batch<4;++batch){
      guest_phase=true;
      for(size_t i=0;i<8;++i){
        auto lifetime=std::shared_ptr<int>(new int(1),[&](int* p){
          CHECK_FALSE(guest_phase);++destroyed;delete p;
        });
        auto frame=ListenerFrame(root,0x816A8C00,lifetime);
        auto* cell=bridge.BeforeGetNext(frame,{},0x10000180,0,0,0,selected.data(),nullptr,slot);REQUIRE(cell);
        REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0xC0000008,0));
        REQUIRE(bridge.OuterReturned(cell,frame,0));CHECK(slot.load()==nullptr);
        returned_owners.push_back(std::move(frame));CHECK_FALSE(frame);
      }
      CHECK(returned_owners.size()==8);guest_phase=false;returned_owners.clear();
      CHECK(destroyed==B::kCells*3+(batch+1)*8);CHECK_FALSE(bridge.lost());
    }
  }
  static void ReturnedLeaseIdentity(bool foreign_control_block){
    using B=NativeIdentityListenerBridge;const B::Root root{11,13,17};B bridge(root);
    auto lifetime=std::make_shared<int>(1);std::atomic<void*> slot{nullptr};
    std::array<uint8_t,4> selected{};
    const auto old=ListenerFrame(root,0x816A8C00,lifetime);
    auto* old_cell=bridge.BeforeGetNext(old,{},0x10000180,0,0,0,selected.data(),nullptr,slot);REQUIRE(old_cell);
    REQUIRE(bridge.ReferenceReturned(old_cell,0x10000180,0xC0000008,0));
    REQUIRE(bridge.OuterReturned(old_cell,old,0));
    auto current=ListenerFrame(root,0x816A8C00,lifetime);
    REQUIRE(B::SameFrame(*old,*current)); // actual field equality is insufficient
    auto* cell=bridge.BeforeGetNext(current,{},0x10000180,0,0,0,selected.data(),nullptr,slot);
    REQUIRE(cell);CHECK(cell==old_cell);
    REQUIRE(bridge.ReferenceReturned(cell,0x10000180,0xC0000008,0));
    const auto wrong=foreign_control_block?
        std::shared_ptr<const B::Frame>(current.get(),[](const B::Frame*){}):old;
    if(foreign_control_block){CHECK(wrong.get()==current.get());CHECK((wrong.owner_before(current)||current.owner_before(wrong)));}
    CHECK_FALSE(bridge.OuterReturned(cell,wrong,0));CHECK(bridge.lost());CHECK(slot.load()==cell);
    std::weak_ptr<const B::Frame> kept=current;current.reset();CHECK_FALSE(kept.expired());
    CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0);CHECK(slot.load()==cell);
  }
  static void NestedNativeConsumption(bool unknown){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);
    auto lifetime=std::make_shared<int>(1);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(
        new NativeIdentityEventSource(root,31,37,lifetime));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    const auto other=std::shared_ptr<const NativeIdentityEventSource>(
        new NativeIdentityEventSource(root,32,38,lifetime));
    const auto other_origin=bridge.AdoptSource(other);REQUIRE(other_origin);
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    const L::WorkKey root_key{root,41,43,47,0x90001000,0x90002000,
                             0x71000000,0x816CD70C,0x816CD6E8};
    const auto outer=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(root_key,source,lifetime));
    const auto outer_work=bridge.AdoptWholeWork(outer);REQUIRE(outer_work);
    auto mid_key=root_key;mid_key.invocation=44;mid_key.sp-=0x100;mid_key.function=0x816921C8;
    const auto middle=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(mid_key,source,lifetime,outer));
    const auto mid_work=bridge.AdoptWholeWork(middle);REQUIRE(mid_work);
    auto leaf_key=mid_key;leaf_key.invocation=45;leaf_key.sp-=0x100;leaf_key.function=0x8199C838;
    const auto leaf=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(leaf_key,source,lifetime,middle));
    const auto leaf_work=bridge.AdoptWholeWork(leaf);REQUIRE(leaf_work);
    // A native dequeue already owned by the outer body must deduplicate
    // without losing the independent child receipt or its other source.
    const auto consume=[&](L::Work work,uint32_t id,std::optional<L::Origin> from){
      REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,id,9,from));
      const auto result=bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,id,9,work);
      CHECK(result.native_success);CHECK(result.retained_by_work==bool(from));
    };
    consume(*outer_work,0xA,*origin);
    consume(*leaf_work,0xA,*origin);consume(*leaf_work,0xE,*other_origin);
    if(unknown)consume(*leaf_work,0xF,{});
    auto facts=bridge.CaptureDependencies(*outer_work);REQUIRE(facts);CHECK(facts->count==1);
    CHECK_FALSE(facts->unknown); // The child has not really returned yet.
    bridge.CloseNewSources(); // Closing does not complete or discard admitted children.
    REQUIRE(bridge.FinishWholeWorkNormally(*leaf_work,leaf_key,leaf));
    facts=bridge.CaptureDependencies(*mid_work);REQUIRE(facts);CHECK(facts->count==2);
    CHECK(facts->unknown==unknown);CHECK(facts->dequeued[0]==source);CHECK(facts->dequeued[1]==other);
    REQUIRE(bridge.FinishWholeWorkNormally(*mid_work,mid_key,middle));
    facts=bridge.CaptureDependencies(*outer_work);REQUIRE(facts);CHECK(facts->count==2);
    CHECK(facts->unknown==unknown);CHECK(facts->dequeued[0]==source);CHECK(facts->dequeued[1]==other);
    CHECK_FALSE(bridge.SourceOnlyProducer(*origin));CHECK_FALSE(bridge.SourceOnlyProducer(*other_origin));
    REQUIRE(bridge.FinishWholeWorkNormally(*outer_work,root_key,outer));
    CHECK(bridge.SourceOnlyProducer(*origin));CHECK(bridge.SourceOnlyProducer(*other_origin));
    CHECK_FALSE(bridge.lost());
  }
  static void IndependentNativeConsumption(){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto lifetime=std::make_shared<int>(1);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31,37,lifetime));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    const L::WorkKey key{root,41,43,47,0x90001000,0x90002000,0x71000000,0x816CD70C,0x816CD6E8};
    const auto outer=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,source,lifetime));
    const auto outer_work=bridge.AdoptWholeWork(outer);REQUIRE(outer_work);
    auto other_key=key;++other_key.thread;++other_key.invocation;other_key.pcr+=0x1000;
    const auto other=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(other_key,source,lifetime));
    const auto other_work=bridge.AdoptWholeWork(other);REQUIRE(other_work);
    REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA,9,*origin));
    CHECK(bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,0xA,9,*other_work).retained_by_work);
    REQUIRE(bridge.FinishWholeWorkNormally(*other_work,other_key,other));
    const auto facts=bridge.CaptureDependencies(*outer_work);REQUIRE(facts);
    CHECK(facts->count==0);CHECK(facts->entry==source);CHECK_FALSE(facts->unknown);
    REQUIRE(bridge.FinishWholeWorkNormally(*outer_work,key,outer));CHECK_FALSE(bridge.lost());
  }
  static void NestedParentBindingRefusals(uint32_t mode){
    CAPTURE(mode);
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto lifetime=std::make_shared<int>(1);
    const auto source=std::shared_ptr<const NativeIdentityEventSource>(
        new NativeIdentityEventSource(root,31,37,lifetime));
    const auto origin=bridge.AdoptSource(source);REQUIRE(origin);
    const L::WorkKey key{root,41,43,47,0x90001000,0x90002000,
                         0x71000000,0x816CD70C,0x816CD6E8};
    const auto outer=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(key,source,lifetime));
    const auto outer_work=bridge.AdoptWholeWork(outer);REQUIRE(outer_work);
    auto child_key=key;child_key.invocation=44;child_key.sp-=0x100;child_key.function=0x816921C8;
    std::shared_ptr<const NativeIdentityEventWorkSource> parent=outer;
    if(mode==0)parent=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(key,source,lifetime)); // value-equal is not owned
    if(mode==1)parent=std::shared_ptr<const NativeIdentityEventWorkSource>(lifetime,outer.get());
    if(mode==2)++child_key.task_allocation_generation;
    if(mode==3)++child_key.thread;
    if(mode==4)child_key.pcr+=0x1000;
    if(mode==5)child_key.task+=0x1000;
    if(mode==6)child_key.sp=key.sp;
    if(mode==7)child_key.sp=key.sp+0x100;
    if(mode==8)child_key.invocation=key.invocation;
    if(mode==9){REQUIRE(bridge.FinishWholeWorkNormally(*outer_work,key,outer));}
    if(mode==10)++child_key.root.generation;
    const auto child=std::shared_ptr<const NativeIdentityEventWorkSource>(
        new NativeIdentityEventWorkSource(child_key,source,lifetime,parent));
    CHECK_FALSE(bridge.AdoptWholeWork(child));
    // Foreign roots are refused by the existing outer admission guard before
    // any one-shot/source/ledger mutation. An owned-root mismatch loses.
    if(mode==10)CHECK_FALSE(bridge.lost());else CHECK(bridge.lost());
    if(mode!=9){const auto* retained=bridge.WorkFor(*outer_work);REQUIRE(retained);CHECK(retained->source==outer);}
  }
  static void NestedReturnRefusals(uint32_t mode){
    using B=NativeIdentityListenerBridge;using L=NativeIdentityEventLedger;
    const L::Root root{11,13,17};B bridge(root);auto lifetime=std::make_shared<int>(1);
    std::vector<std::shared_ptr<const NativeIdentityEventSource>> sources;
    std::vector<L::Origin> origins;
    for(uint32_t i=0;i<9;++i){
      auto source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(root,31+i,37+i,lifetime));
      const auto origin=bridge.AdoptSource(source);REQUIRE(origin);sources.push_back(source);origins.push_back(*origin);
    }
    const auto listener=bridge.ledger_.RegisterListener(0x90004000);REQUIRE(listener);
    const L::WorkKey key{root,41,43,47,0x90001000,0x90002000,
                         0x71000000,0x816CD70C,0x816CD6E8};
    const auto outer=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(key,sources[0],lifetime));
    const auto outer_work=bridge.AdoptWholeWork(outer);REQUIRE(outer_work);
    auto child_key=key;child_key.invocation=44;child_key.sp-=0x100;child_key.function=0x816921C8;
    const auto child=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(child_key,sources[0],lifetime,outer));
    const auto child_work=bridge.AdoptWholeWork(child);REQUIRE(child_work);
    const auto consume=[&](L::Work work,uint32_t i){
      REQUIRE(bridge.ledger_.ReplaceQueued(*listener,0x90005000,0xA+i,9,origins[i]));
      CHECK(bridge.ledger_.TransferSuccessfulDequeue(*listener,0x90005000,0xA+i,9,work).retained_by_work);
    };
    consume(*child_work,8);
    if(mode==0){ // An abandoned admitted child prevents outer completion.
      CHECK_FALSE(bridge.FinishWholeWorkNormally(*outer_work,key,outer));
    }else if(mode==1){
      auto wrong=child_key;wrong.lr+=4;
      CHECK_FALSE(bridge.FinishWholeWorkNormally(*child_work,wrong,child));
    }else{ // The complete capacity decision must precede child release.
      for(uint32_t i=0;i<8;++i)consume(*outer_work,i);
      const auto before=bridge.CaptureDependencies(*outer_work);REQUIRE(before);CHECK(before->count==8);
      CHECK_FALSE(bridge.FinishWholeWorkNormally(*child_work,child_key,child));
    }
    CHECK(bridge.lost());
    const auto* retained_parent=bridge.WorkFor(*outer_work);REQUIRE(retained_parent);CHECK(retained_parent->source==outer);
    const auto* retained_child=bridge.WorkFor(*child_work);REQUIRE(retained_child);CHECK(retained_child->source==child);
    CHECK_FALSE(retained_parent->finished);CHECK_FALSE(retained_child->finished);
  }
  static void ActiveCellCapacityRemainsStrict(){
    using B=NativeIdentityListenerBridge;const B::Root root{11,13,17};B bridge(root);
    auto lifetime=std::make_shared<int>(1);std::array<std::atomic<void*>,B::kCells+1> slots{};
    std::array<uint8_t,4> selected{};std::vector<std::shared_ptr<const B::Frame>> owners;
    for(size_t i=0;i<=B::kCells;++i){
      auto frame=std::shared_ptr<const B::Frame>(new B::Frame(root,101+i,1001+i,107,
          uint32_t(41+i),0x90002000+uint32_t(i)*0x1000,0x707DF640,0x816CD70C,0x816A8C00,lifetime));
      owners.push_back(frame);
      auto* cell=bridge.BeforeGetNext(frame,{},0x10000180,0,0,0,selected.data(),nullptr,slots[i]);
      if(i<B::kCells){REQUIRE(cell);CHECK(slots[i].load()==cell);}
      else{CHECK(cell==nullptr);CHECK(slots[i].load()==nullptr);}
    }
    CHECK(bridge.lost());const auto failure=bridge.FirstFailure();REQUIRE(failure.state==2);
    CHECK(failure.data.reason==20);CHECK(failure.data.available==0);
    for(size_t i=0;i<B::kCells;++i)CHECK(slots[i].load()!=nullptr);
    CHECK(bridge.ReapOnHostOutsideGuestLocks().count==0);
  }
};
TEST_CASE("Dedicated SYSTEM thread consumption outlives GetNext until exact finite-body return", "[identity_listener][system_thread_frame][lifetime]"){
 NativeIdentityListenerBridgeTestAccess::SystemThreadFrameConsumer(GENERATE(false,true));
}
TEST_CASE("Dedicated SYSTEM thread work cannot fabricate or erase a native Task claim", "[identity_listener][system_thread_frame][authority]"){
 NativeIdentityListenerBridgeTestAccess::SystemThreadFrameKeyRefusesTaskClaims(GENERATE(0u,1u,2u,3u));
}
TEST_CASE("Typed discarded identity hints retain real API delivery without fabricating read duty", "[identity_listener][scalar_delivery][lifetime]"){
 NativeIdentityListenerBridgeTestAccess::TypedScalarDelivery(GENERATE(0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16u,17u,18u,19u,20u,21u));
}
TEST_CASE("SUI discarded identity delivery retains genuine API duties through cancel return and restart", "[identity_listener][sui_scalar_delivery][lifetime]"){
 SECTION("Finished producer is retained through pending delivery then reaped before a new root"){
  NativeIdentityListenerBridgeTestAccess::TypedScalarDelivery(0,true);
 }
 SECTION("Non-A untyped malformed and aborted deliveries cannot settle the source"){
  NativeIdentityListenerBridgeTestAccess::TypedScalarDelivery(GENERATE(1u,2u,3u,5u,6u,7u,10u,13u,14u,15u,16u,17u,18u),true);
 }
 SECTION("Admission close preserves genuine return replacement queue and whole callback duties"){
  NativeIdentityListenerBridgeTestAccess::TypedScalarDelivery(GENERATE(4u,8u,9u,11u,12u,19u,20u,21u),true);
 }
}
TEST_CASE("Closed healthy listener admission cancels new calls without claiming completion", "[identity_listener][closed_admission]"){
 NativeIdentityListenerBridgeTestAccess::ClosedCellAdmissionIsCancellation(GENERATE(false,true));
}
TEST_CASE("Listener seal cannot hide ABI ownership collision loss or capacity failures", "[identity_listener][closed_admission][authority]"){
 NativeIdentityListenerBridgeTestAccess::ClosedAdmissionPreservesFailures(GENERATE(0u,1u,2u,3u,4u,5u,6u,7u));
}
TEST_CASE("Already admitted native broadcasts finish after sealing without dropping queues", "[identity_listener][closed_admission][lifetime]"){
 NativeIdentityListenerBridgeTestAccess::AdmittedBroadcastSurvivesSeal();
}
TEST_CASE("Prepared identity origins survive actual whole-work retirement cuts", "[identity_listener][lifetime]"){
 NativeIdentityListenerBridgeTestAccess::PreparedOriginWorkLifetime();
}
TEST_CASE("Old scalar notification queues outlive producer finish through real transfer and reap", "[identity_listener][old_source][phase_ownership][lifetime]"){
 NativeIdentityListenerBridgeTestAccess::OldQueuedHintsKeepLifetimeAfterProducer();
}
TEST_CASE("Current identity producer preserves queued hints and genuine whole consumer ownership", "[identity_listener][current_source][lifetime]"){
 NativeIdentityListenerBridgeTestAccess::CurrentProducerKeepsQueuedHints();
}
TEST_CASE("Current producer ownership rejects forged origins and a finished producer", "[identity_listener][current_source][authority]"){
 NativeIdentityListenerBridgeTestAccess::CurrentProducerRejectsUnownedOrFinished(GENERATE(0u,1u,2u,3u,4u));
}
TEST_CASE("Current source quiescence never retires unowned consumed events", "[identity_listener][current_source][orphan]"){
 NativeIdentityListenerBridgeTestAccess::CurrentSourceOrphanStillRefuses(GENERATE(false,true));
}
TEST_CASE("Current source quiescence excludes critical cells and unknown whole consumers", "[identity_listener][current_source][quiescence]"){
 NativeIdentityListenerBridgeTestAccess::CurrentSourceRejectsCriticalOrUnknown(GENERATE(false,true));
}
TEST_CASE("Cross-origin child consumption remains a parent duty through transfer and dedup", "[identity_listener][current_source][nested_consumption]"){
 NativeIdentityListenerBridgeTestAccess::CurrentSourceNestedCrossOriginConsumer(GENERATE(false,true));
}
TEST_CASE("Nested finite SYSTEM consumers retain transferred origin duty through full parent return", "[identity_listener][system_thread_frame][nested_consumption]"){
 NativeIdentityListenerBridgeTestAccess::CurrentSourceNestedCrossOriginConsumer(GENERATE(false,true),true);
}
TEST_CASE("Finite helpers with actual Task keys preserve parent render dependency credit", "[identity_listener][system_thread_frame][task_borrow]"){
 NativeIdentityListenerBridgeTestAccess::CurrentSourceNestedCrossOriginConsumer(GENERATE(false,true),false,true);
}
TEST_CASE("Finite SYSTEM frame parent claims reject Task and source ancestry substitution", "[identity_listener][system_thread_frame][parent_authority]"){
 NativeIdentityListenerBridgeTestAccess::SystemFrameParentAuthorityRefusesMixedClaims(GENERATE(0u,1u,2u,3u,4u));
}
TEST_CASE("An unregistered prepared-looking source cannot be adopted by work", "[identity_listener][authority]"){
 NativeIdentityListenerBridgeTestAccess::UnknownCannotBePrepared();
}
TEST_CASE("Coalescing and listener destruction cannot prove renderer consumption", "[identity_listener][render_consumption]"){
 NativeIdentityListenerBridgeTestAccess::DroppedQueueIsNotConsumption(GENERATE(false,true));
}
TEST_CASE("Listener quartet decodes the actual V33 register roles and canonical DWORDs", "[identity_listener][abi]"){
 NativeIdentityListenerBridgeTestAccess::QuartetArguments();
}
TEST_CASE("Listener registration and locked enqueue preserve canonical native32 values", "[identity_listener][abi]"){
 NativeIdentityListenerBridgeTestAccess::CanonicalRegistrationAndEnqueue();
}
TEST_CASE("GetNext explicit failed reference differs from an unobserved reference", "[identity_listener][reference]"){
 NativeIdentityListenerBridgeTestAccess::GetNextReferenceFailure(GENERATE(false,true));
}
TEST_CASE("GetNext metadata requires the exact referenced listener and paired actual release", "[identity_listener][reference]"){
 const auto mode=GENERATE(0,1,2);
 NativeIdentityListenerBridgeTestAccess::GetNextReferenceLifetime(mode==1,mode==2);
}
TEST_CASE("Listener first fast failure retains full or explicitly unavailable cell scalars", "[identity_listener][diagnostic]"){
 NativeIdentityListenerBridgeTestAccess::FirstFastFailureRetention(GENERATE(false,true));
}
TEST_CASE("Listener original queue-ledger failure survives a later outer-return symptom", "[identity_listener][diagnostic]"){
 NativeIdentityListenerBridgeTestAccess::FirstLedgerFailureRetention();
}
TEST_CASE("Native listener word operands preserve the actual normalized queue key", "[identity_listener][word_operand]"){
 NativeIdentityListenerBridgeTestAccess::NativeWordQueueKey(GENERATE(0u,1u,2u,3u));
}
TEST_CASE("Returned whole listener work can burst without retiring resources in guest locks", "[identity_listener][returned_work]"){
 NativeIdentityListenerBridgeTestAccess::ReturnedWholeWorkBurstAndHostLifetime();
}
TEST_CASE("Whole listener work requires the exact caller-retained normal-return owner", "[identity_listener][returned_work]"){
 NativeIdentityListenerBridgeTestAccess::WholeWorkReturnRequiresRetainedOwner(GENERATE(0u,1u,2u,3u,4u,5u,6u,7u));
}
TEST_CASE("Returned listener cells remain reusable while exact frame owners await host reaping", "[identity_listener][returned_lease]"){
 NativeIdentityListenerBridgeTestAccess::ReturnedCellReuseAndHostLifetime();
}
TEST_CASE("Listener cell reuse rejects old return generations and foreign shared owners", "[identity_listener][returned_lease]"){
 NativeIdentityListenerBridgeTestAccess::ReturnedLeaseIdentity(GENERATE(false,true));
}
TEST_CASE("Unreturned listener cells retain the exact concurrent capacity bound", "[identity_listener][returned_lease]"){
 NativeIdentityListenerBridgeTestAccess::ActiveCellCapacityRemainsStrict();
}
TEST_CASE("Nested native listener consumption reaches the exact whole-frame owner", "[identity_listener][nested_consumption]"){
 NativeIdentityListenerBridgeTestAccess::NestedNativeConsumption(GENERATE(false,true));
}
TEST_CASE("Independent native consumption cannot refresh a matching-source frame", "[identity_listener][nested_consumption]"){
 NativeIdentityListenerBridgeTestAccess::IndependentNativeConsumption();
}
TEST_CASE("Nested listener admission rejects unrelated and recycled parent ownership", "[identity_listener][nested_consumption]"){
 NativeIdentityListenerBridgeTestAccess::NestedParentBindingRefusals(GENERATE(0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u));
}
TEST_CASE("Nested listener return retains abandoned mismatched and over-capacity work", "[identity_listener][nested_consumption]"){
 NativeIdentityListenerBridgeTestAccess::NestedReturnRefusals(GENERATE(0u,1u,2u));
}
TEST_CASE("Issued initial event producer survives current replacement cancellation queue delivery and restart", "[identity_listener][initial_event_producer][lifetime][restart]"){
 NativeIdentityListenerBridgeTestAccess::InitialProducerCustodySurvivesProjectionAndReap();
}
} // namespace xe::kernel
