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
    REQUIRE(bridge.FinishWholeWorkNormally(*work,key));
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
    REQUIRE(bridge.FinishWholeWorkNormally(*work,key));
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
    REQUIRE(bridge.FinishWholeWorkNormally(*work,work_key));CHECK(bridge.SourceOnlyProducer(*origin));
    CHECK_FALSE(bridge.lost());CHECK(bridge.FirstFailure().state==0);
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
TEST_CASE("Prepared identity origins survive actual whole-work retirement cuts", "[identity_listener][lifetime]"){
 NativeIdentityListenerBridgeTestAccess::PreparedOriginWorkLifetime();
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
TEST_CASE("Returned listener cells remain reusable while exact frame owners await host reaping", "[identity_listener][returned_lease]"){
 NativeIdentityListenerBridgeTestAccess::ReturnedCellReuseAndHostLifetime();
}
TEST_CASE("Listener cell reuse rejects old return generations and foreign shared owners", "[identity_listener][returned_lease]"){
 NativeIdentityListenerBridgeTestAccess::ReturnedLeaseIdentity(GENERATE(false,true));
}
TEST_CASE("Unreturned listener cells retain the exact concurrent capacity bound", "[identity_listener][returned_lease]"){
 NativeIdentityListenerBridgeTestAccess::ActiveCellCapacityRemainsStrict();
}
} // namespace xe::kernel
