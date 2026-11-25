/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <string>
#include <string_view>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/native_avatar_contract.h"
#include "xenia/kernel/native_avatar_service.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/native_profile_identity_contract.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
namespace xe::kernel::test {
namespace {
constexpr NativeSystemUiPublicCall kCalls[]={
  NativeSystemUiPublicCall::kAvatarInitialize,
  NativeSystemUiPublicCall::kAvatarShutdown,
  NativeSystemUiPublicCall::kAvatarGetAssetsResultSize,
  NativeSystemUiPublicCall::kAvatarGetAssets,
  NativeSystemUiPublicCall::kAvatarGetMetadataRandom,
  NativeSystemUiPublicCall::kAvatarGetManifestLocalUser,
  NativeSystemUiPublicCall::kAvatarSetManifest,
  NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfileCount,
  NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile};
ValidatedGuestRuntimePlan Family(){
  ValidatedGuestRuntimePlan plan;
  for(const auto& b:NativeAvatarBridgeProfiles())
    plan.bridges.push_back({"xam.xex",b.ordinal,0,b.bytes,std::string(b.sha256),{}});
  for(const auto& p:NativeAvatarStartupProfiles()){
    GuestRuntime::Function f;
    f.identity=p.identity;f.address=p.address;f.arguments.resize(p.arguments,0);
    f.return_kind=p.returns_void?GuestRuntime::ReturnKind::kVoid:GuestRuntime::ReturnKind::kHResult;
    plan.catalog.emplace(f.identity,f);
    ValidatedGuestRuntimePlan::Contract c;
    c.caller=ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly;
    c.result=p.returns_void?ValidatedGuestRuntimePlan::ResultPolicy::kVoid:ValidatedGuestRuntimePlan::ResultPolicy::kHResult;
    plan.contracts.emplace(f.identity,c);
  }
  for(const auto call:kCalls){
    const auto* p=NativeSystemUiPublicProfile(call);
    GuestRuntime::Function f;
    f.identity=p->identity;f.address=p->address;f.arguments.resize(p->argument_count,0);
    const bool stop=call==NativeSystemUiPublicCall::kAvatarShutdown;
    f.return_kind=stop?GuestRuntime::ReturnKind::kVoid:GuestRuntime::ReturnKind::kUInt32;
    plan.catalog.emplace(f.identity,f);
    ValidatedGuestRuntimePlan::Contract c;
    c.caller=ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem;
    c.result=stop?ValidatedGuestRuntimePlan::ResultPolicy::kVoid:ValidatedGuestRuntimePlan::ResultPolicy::kAny;
    plan.contracts.emplace(f.identity,c);
  }
  return plan;
}
void Denied(const ValidatedGuestRuntimePlan& p){
  std::string error;
  CHECK(CheckNativeAvatarPlanFamily(p,&error)==NativeAvatarPlanFamily::kInvalid);
  CHECK_FALSE(error.empty());
}
}
// Production entry-pairing metadata, using the actual QA15 nested dispatcher
// tuples. No Task, native entry, Request completion or Avatar readiness is minted.
TEST_CASE("Avatar shared dispatch pairs passthrough without completing outer work",
          "[kernel][native_avatar_contract][avatar_entry_pairing]") {
  struct Frame { uint64_t thread;uint32_t pcr,sp,lr,process;bool operator==(const Frame&) const = default; };
  const Frame outer{140658882459088ull,0x30050000,0x708CFDA0,0x816B8B64,2};
  const Frame inner{outer.thread,outer.pcr,0x708CF5E0,0x816B7EA4,2};
  NativeAvatarEntryStack<Frame> entries;
  CHECK_FALSE(entries.EnterPassthroughIfNested(inner));CHECK(entries.empty());
  entries.EnterOwned(outer);REQUIRE(entries.EnterPassthroughIfNested(inner));
  CHECK(entries.ObserveReturn(inner)==NativeAvatarEntryReturn::kPassthrough);
  REQUIRE_FALSE(entries.empty());CHECK(entries.expected()==outer);
  CHECK(entries.ObserveReturn(outer)==NativeAvatarEntryReturn::kOwned);
  CHECK(entries.empty());
  CHECK(entries.ObserveReturn(inner)==NativeAvatarEntryReturn::kAbsent);
}
TEST_CASE("Avatar entry pairing preserves exact identity and LIFO on bad returns",
          "[kernel][native_avatar_contract][avatar_entry_pairing]") {
  struct Frame { uint64_t thread;uint32_t pcr,sp,lr,process;bool operator==(const Frame&) const = default; };
  const Frame outer{140658882459088ull,0x30050000,0x708CFDA0,0x816B8B64,2};
  const Frame inner{outer.thread,outer.pcr,0x708CF5E0,0x816B7EA4,2};
  auto wrong=inner;const auto field=GENERATE(0u,1u,2u,3u,4u);
  if(field==0)wrong.thread+=8;else if(field==1)wrong.pcr+=0x1000;
  else if(field==2)wrong.sp+=0x10;else if(field==3)wrong.lr+=4;else wrong.process=1;
  NativeAvatarEntryStack<Frame> entries;entries.EnterOwned(outer);
  REQUIRE(entries.EnterPassthroughIfNested(inner));
  CHECK(entries.ObserveReturn(wrong)==NativeAvatarEntryReturn::kMismatch);
  REQUIRE_FALSE(entries.empty());CHECK(entries.expected()==inner);
  CHECK(entries.ObserveReturn(outer)==NativeAvatarEntryReturn::kMismatch);
  REQUIRE_FALSE(entries.empty());CHECK(entries.expected()==inner);
  CHECK(entries.ObserveReturn(inner)==NativeAvatarEntryReturn::kPassthrough);
  CHECK(entries.ObserveReturn(outer)==NativeAvatarEntryReturn::kOwned);CHECK(entries.empty());
}
TEST_CASE("Avatar owned callback nested through passthrough keeps each return distinct",
          "[kernel][native_avatar_contract][avatar_entry_pairing]") {
  struct Frame { uint32_t sp,lr;bool operator==(const Frame&) const = default; };
  const Frame outer{0x708CFDA0,0x816B8B64},passthrough{0x708CF5E0,0x816B7EA4},owned_child{0x708CF400,0x816B8B64};
  NativeAvatarEntryStack<Frame> entries;entries.EnterOwned(outer);
  REQUIRE(entries.EnterPassthroughIfNested(passthrough));entries.EnterOwned(owned_child);
  CHECK(entries.ObserveReturn(passthrough)==NativeAvatarEntryReturn::kMismatch);
  REQUIRE_FALSE(entries.empty());CHECK(entries.expected()==owned_child);
  CHECK(entries.ObserveReturn(owned_child)==NativeAvatarEntryReturn::kOwned);
  CHECK(entries.ObserveReturn(passthrough)==NativeAvatarEntryReturn::kPassthrough);
  CHECK(entries.ObserveReturn(outer)==NativeAvatarEntryReturn::kOwned);CHECK(entries.empty());
}

// Native-byte-derived producer arithmetic anchored to QA16 Start/packet.
// These real production checks are not Request/code/Task admission fixtures.
TEST_CASE("Avatar Init request preserves null-options Schedule producer ABI",
          "[kernel][native_avatar_contract][avatar_request_task]") {
  constexpr uint32_t start=0x7050F630,schedule=start-0xD0,packet=0x706A0;
  CHECK(MatchesNativeAvatarInitStartEdge(0x7050F6C0,start,0x81675688));
  CHECK_FALSE(MatchesNativeAvatarInitStartEdge(0x7050F6C0,start+0x10,0x81675688));
  CHECK_FALSE(MatchesNativeAvatarInitStartEdge(0x7050F6C0,start,0x81675684));
  CHECK_FALSE(MatchesNativeAvatarInitStartEdge(0x90,0,0x81675688));
  CHECK(MatchesNativeAvatarInitScheduleEdge(start,schedule,0x816A2EC8,packet,0x816A2C80,packet,0,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(start,schedule+0x10,0x816A2EC8,packet,0x816A2C80,packet,0,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(start,schedule,0x816BD664,packet,0x816A2C80,packet,0,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(start,schedule,0x816A2EC8,packet,0x816BD4D8,packet,0,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(start,schedule,0x816A2EC8,packet,0x816A2C80,packet+4,0,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(start,schedule,0x816A2EC8,packet,0x816A2C80,packet,1,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(start,schedule,0x816A2EC8,packet,0x816A2C80,packet,0,packet+0x14));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(0xC0,0,0x816A2EC8,packet,0x816A2C80,packet,0,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(0xD0,0,0x816A2EC8,packet,0x816A2C80,packet,0,packet+0x18));
  CHECK_FALSE(MatchesNativeAvatarInitScheduleEdge(start,schedule,0x816A2EC8,0xFFFFFFF0,0x816A2C80,0xFFFFFFF0,0,8));
}
TEST_CASE("Avatar Init allocator keeps null input separate from actual output",
          "[kernel][native_avatar_contract][avatar_request_task]") {
  constexpr uint32_t schedule=0x7050F560,allocation=schedule-0x90;
  CHECK(MatchesNativeAvatarInitAllocationEdge(schedule,allocation,0x816BA46C,0,allocation+0x50));
  CHECK_FALSE(MatchesNativeAvatarInitAllocationEdge(schedule,allocation,0x816BA46C,schedule+0x58,allocation+0x50));
  CHECK_FALSE(MatchesNativeAvatarInitAllocationEdge(schedule,allocation+0x10,0x816BA46C,0,allocation+0x50));
  CHECK_FALSE(MatchesNativeAvatarInitAllocationEdge(schedule,allocation,0x816A2EC8,0,allocation+0x50));
  CHECK_FALSE(MatchesNativeAvatarInitAllocationEdge(schedule,allocation,0x816BA46C,0,schedule+0x50));
  CHECK_FALSE(MatchesNativeAvatarInitAllocationEdge(0x80,0,0x816BA46C,0,0x50));
  CHECK_FALSE(MatchesNativeAvatarInitAllocationEdge(0x90,0,0x816BA46C,0,0x50));
}

TEST_CASE("Avatar catalog metadata alone never requests or admits a service family",
          "[kernel][native_avatar_contract]"){
  auto p=Family();p.bridges.clear();
  for(const auto& r:NativeAvatarStartupProfiles()){p.catalog.erase(std::string(r.identity));p.contracts.erase(std::string(r.identity));}
  CHECK(CheckNativeAvatarPlanFamily(p)==NativeAvatarPlanFamily::kAbsent);
  // A single authenticated full-entry intent must not partially admit F2/F3.
  const auto& b=NativeAvatarBridgeProfiles().front();
  p.bridges.push_back({"xam.xex",b.ordinal,0,b.bytes,std::string(b.sha256),{}});
  Denied(p);
}
// Historical case name retained for qualification identity; the required
// provider table now contains six bodies, excluding the two native queries.
TEST_CASE("Avatar provider family requires all eight exact full native bodies once",
          "[kernel][native_avatar_contract]"){
  const auto complete=Family();
  REQUIRE(CheckNativeAvatarPlanFamily(complete)==NativeAvatarPlanFamily::kComplete);
  for(size_t i=0;i<complete.bridges.size();++i){
    auto missing=complete;missing.bridges.erase(missing.bridges.begin()+i);Denied(missing);
    auto changed=complete;changed.bridges[i].code_sha256[0]^=1;Denied(changed);
    auto partial=complete;partial.bridges[i].code_size=4;Denied(partial);
    auto duplicate=complete;duplicate.bridges.push_back(duplicate.bridges[i]);Denied(duplicate);
    auto unproved=complete;unproved.bridges[i].code_size=0;unproved.bridges[i].code_sha256.clear();Denied(unproved);
  }
}
TEST_CASE("Avatar startup roles require real SYSTEM Task and zero prototypes",
          "[kernel][native_avatar_contract]"){
  const auto complete=Family();
  for(const auto& r:NativeAvatarStartupProfiles()){
    const auto name=std::string(r.identity);
    auto wrong=complete;wrong.contracts[name].caller=ValidatedGuestRuntimePlan::CallerPolicy::kSystemOnly;Denied(wrong);
    auto arguments=complete;arguments.catalog[name].arguments[0]=0xF3;Denied(arguments);
    auto context=complete;context.contracts[name].before.push_back({0x81AA13B4,1,false});Denied(context);
    auto initializer=complete;initializer.plan.phases.push_back({"heap",{initializer.catalog[name]}});Denied(initializer);
  }
}
TEST_CASE("Avatar public prototypes preserve original caller and native return semantics",
          "[kernel][native_avatar_contract]"){
  const auto complete=Family();
  for(const auto call:kCalls){
    const auto name=std::string(NativeSystemUiPublicProfile(call)->identity);
    auto caller=complete;caller.contracts[name].caller=ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly;Denied(caller);
    auto result=complete;result.catalog[name].return_kind=GuestRuntime::ReturnKind::kHResult;Denied(result);
    auto arity=complete;arity.catalog[name].arguments.push_back(0);Denied(arity);
    if(!complete.catalog.at(name).arguments.empty()){
      auto values=complete;values.catalog[name].arguments[0]=1;Denied(values);
    }
    auto post=complete;post.contracts[name].after.push_back({0x81B4AF50,1,false});Denied(post);
    auto initializer=complete;initializer.plan.phases.push_back({"heap",{initializer.catalog[name]}});Denied(initializer);
  }
}

TEST_CASE("Owned profile bridges resolve the actual native semantic exports",
          "[kernel][native_avatar_contract][native_profile_identity]") {
  // These names are independently joined from the authenticated17559 export
  // table by NATIVE ADDRESS, rather than copied from bridge ordinal constants.
  // The old Name/SigninInfo bodies paired with20C/20D fail this real registry
  // lookup even if a synthetic byte-window/plan-family test is self-consistent.
  struct NativeMeaning { uint32_t address; std::string_view name; };
  constexpr std::array<NativeMeaning,8> expected{{
    {0x816DB860,"XamProfileCreateEnumerator"},
    {0x816D7860,"XamProfileEnumerate"},
    {0x816B16A8,"XamUserReadProfileSettingsEx"},
    {0x816B1968,"XamUserWriteProfileSettings"},
    {0x816EFD80,"XamContentOpenFile"},
    {0x816EEB78,"XamContentClose"},
    {0x816DD040,"XamUserGetName"},
    {0x816DDB20,"XamUserGetSigninInfo"},
  }};
  cpu::ExportResolver resolver;
  xam::RegisterInfoExports(&resolver,nullptr);
  xam::RegisterUserExports(&resolver,nullptr);
  xam::RegisterEnumExports(&resolver,nullptr);
  xam::RegisterContentExports(&resolver,nullptr);
  xam::XamModule::RegisterExportTable(&resolver);
  std::array<bool,expected.size()> seen{};
  const auto check=[&](const auto& profile) {
    const auto meaning=std::find_if(expected.begin(),expected.end(),
        [&](const auto& row){return row.address==profile.address;});
    REQUIRE(meaning!=expected.end());
    const auto index=size_t(meaning-expected.begin());
    CHECK_FALSE(seen[index]);seen[index]=true;
    const auto* actual=resolver.GetExportByOrdinal("xam.xex",profile.ordinal);
    REQUIRE(actual);
    CHECK(std::string_view(actual->name)==meaning->name);
    CHECK(actual->get_type()==cpu::Export::Type::kFunction);
    CHECK(actual->is_implemented());
    CHECK((actual->tags & cpu::ExportTag::kStub)==0);
    CHECK(actual->function_data.trampoline!=nullptr);
  };
  for(const auto& profile:NativeAvatarBridgeProfiles())check(profile);
  for(const auto& profile:NativeProfileIdentityBridgeProfiles())check(profile);
  for(const auto visited:seen)CHECK(visited);
  // Preserve the base plan's separately accepted boot-mode provider.
  const auto* mode=resolver.GetExportByOrdinal("xam.xex",0xB46);
  REQUIRE(mode);
  CHECK(std::string_view(mode->name)=="XamIsDataCenterMode");
  CHECK(mode->get_type()==cpu::Export::Type::kFunction);
  CHECK(mode->is_implemented());
  CHECK((mode->tags & cpu::ExportTag::kStub)==0);
  CHECK(mode->function_data.trampoline!=nullptr);
}
TEST_CASE("Resident F3 frame relations preserve the saved native call sites",
          "[kernel][native_avatar_contract][native_avatar_resident]") {
  // Metadata facts independently derived from the actual17559 raw functions.
  // These checks do not claim a native call, Task, RWLock or package is ready.
  struct Edge { NativeAvatarResidentEdge kind; uint32_t parent,child,lr; };
  constexpr std::array edges{
    Edge{NativeAvatarResidentEdge::kFrontendBuiltin,0x30010000,0x3000FFA0,0x816BDEF8},
    Edge{NativeAvatarResidentEdge::kBuiltinSetup,0x3000FFA0,0x3000FEF0,0x816BD36C},
    Edge{NativeAvatarResidentEdge::kBuiltinLoad,0x3000FFA0,0x3000FEF0,0x816BD3B4},
    Edge{NativeAvatarResidentEdge::kLoadCallback,0x3000FEF0,0x3000FD30,0x816A2A24},
    Edge{NativeAvatarResidentEdge::kCallbackLock,0x3000FD30,0x3000FCC0,0x819816B4},
  };
  for(const auto& e:edges){
    CHECK(MatchesNativeAvatarResidentEdge(e.kind,e.parent,e.child,e.lr));
    CHECK_FALSE(MatchesNativeAvatarResidentEdge(e.kind,e.parent,e.child+16,e.lr));
    CHECK_FALSE(MatchesNativeAvatarResidentEdge(e.kind,e.parent,e.child,e.lr+4));
    CHECK_FALSE(MatchesNativeAvatarResidentEdge(e.kind,e.parent+1,e.child,e.lr));
    CHECK_FALSE(MatchesNativeAvatarResidentEdge(e.kind,0,e.child,e.lr));
    CHECK_FALSE(MatchesNativeAvatarResidentEdge(e.kind,16,UINT32_MAX-15,e.lr));
  }
  // Same-frame setup and load are different native call sites; a saved setup
  // observation must not be reused as an initialized loader/callback receipt.
  CHECK_FALSE(MatchesNativeAvatarResidentEdge(NativeAvatarResidentEdge::kBuiltinLoad,
      edges[1].parent,edges[1].child,edges[1].lr));
  CHECK_FALSE(MatchesNativeAvatarResidentEdge(static_cast<NativeAvatarResidentEdge>(255),
      edges[0].parent,edges[0].child,edges[0].lr));
}
TEST_CASE("Resident F3 observation covers original FE and native lock entry",
          "[kernel][native_avatar_contract][native_avatar_resident]") {
  // Both backends use this actual hook selector. The source-defined resident
  // chain was previously omitted even though FE already executes its setup.
  for(const uint32_t at:{0x816BDE90u,0x816BD270u,0x816BCC38u,0x816BCD08u,
                        0x81981740u,0x81981560u,0x81982088u})
    CHECK(NativeAvatarService::HooksFunction(at));
  CHECK_FALSE(NativeAvatarService::HooksFunction(0x8198208C));
  CHECK_FALSE(NativeAvatarService::HooksFunction(0x816BD26C));
  // The deferred source-adoption catalog stays the original exact two roles;
  // it does not gain a second resident lock-init operation or public witness.
  const auto profiles=NativeAvatarStartupProfiles();
  REQUIRE(profiles.size()==2);
  CHECK(profiles[0].address==0x816BCC38);
  CHECK(profiles[1].address==0x816BCD08);
}
TEST_CASE("Avatar native Task body metadata remains distinct from owned tagged references",
          "[kernel][native_avatar_contract][native_avatar_resident]") {
  // Actual B8098 adds a real reference then returns raw context Task|1.
  // Comparing or masking metadata never creates that actual reference.
  constexpr uint32_t body=0x300B1000,reference=body|1;
  CHECK(NativeAvatarTaskReferenceMatchesBody(reference,body));
  CHECK_FALSE(NativeAvatarTaskReferenceMatchesBody(body,body));
  CHECK_FALSE(NativeAvatarTaskReferenceMatchesBody(reference,reference));
  CHECK_FALSE(NativeAvatarTaskReferenceMatchesBody(body|2,body));
  CHECK_FALSE(NativeAvatarTaskReferenceMatchesBody(body|3,body));
  CHECK_FALSE(NativeAvatarTaskReferenceMatchesBody(reference,body+4));
  CHECK_FALSE(NativeAvatarTaskReferenceMatchesBody(1,0));
  CHECK_FALSE(NativeAvatarTaskReferenceMatchesBody(0,0));
}

TEST_CASE("Avatar animation is an exact optional original caller plan extension",
          "[kernel][native_avatar_contract][native_avatar_animation]") {
  auto base=Family();
  CHECK(CheckNativeAvatarPlanFamily(base)==NativeAvatarPlanFamily::kComplete);
  const auto* profile=NativeSystemUiPublicProfile(NativeSystemUiPublicCall::kAvatarLoadAnimation);
  REQUIRE(profile);
  CHECK(profile->address==0x81674D48);CHECK(profile->code_size==152);
  CHECK(profile->code_sha256=="79dfc783867f65459133dc1938776d496134763bb54e304a97fc95726b147a96");
  CHECK(profile->argument_count==4);CHECK(profile->uint64_register_mask==0);
  CHECK(profile->family==NativeSystemPublicFamily::kAvatarEngine);
  const std::string name(profile->identity);
  GuestRuntime::Function f;f.identity=name;f.address=profile->address;
  f.arguments={0,0,0,0};f.return_kind=GuestRuntime::ReturnKind::kUInt32;
  auto extension=base;extension.catalog.emplace(name,f);
  Denied(extension); // Catalog without its own original-caller contract.
  ValidatedGuestRuntimePlan::Contract c;
  c.caller=ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem;
  c.result=ValidatedGuestRuntimePlan::ResultPolicy::kAny;
  extension.contracts.emplace(name,c);
  CHECK(CheckNativeAvatarPlanFamily(extension)==NativeAvatarPlanFamily::kComplete);
  auto wrong=extension;wrong.catalog[name].address+=4;Denied(wrong);
  wrong=extension;wrong.catalog[name].arguments.pop_back();Denied(wrong);
  wrong=extension;wrong.contracts[name].caller=ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly;Denied(wrong);
  wrong=extension;wrong.contracts[name].result=ValidatedGuestRuntimePlan::ResultPolicy::kHResult;Denied(wrong);
  wrong=extension;wrong.plan.phases.push_back({"heap",{f}});Denied(wrong);
  CHECK(CheckNativeAvatarPlanFamily(base)==NativeAvatarPlanFamily::kComplete);
  CHECK(NativeSystemUiPublicArgumentsValid(profile->call,std::array<uint64_t,4>{0x90010000,0xFFFFFFFF,0x90020000,0}));
  CHECK_FALSE(NativeSystemUiPublicArgumentsValid(profile->call,std::array<uint64_t,4>{0x90010000,0x100000000ull,0x90020000,0}));
}
TEST_CASE("Avatar animation frame joins distinguish engine worker from request target",
          "[kernel][native_avatar_contract][native_avatar_animation]") {
  // Fixed native prologs and direct BL return addresses, not synthetic Task
  // ownership. The session-worker frame is distinct from the request Task.
  constexpr uint32_t worker=0x70840000,job=worker-0xA0,core=job-0x60,writer=core-0x4C0;
  CHECK(MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kWorkerJob,worker,job,0x819651DC));
  CHECK(MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kJobCore,job,core,0x81964E94));
  CHECK(MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kCoreWriter,core,writer,0x81968528));
  CHECK_FALSE(MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kWorkerJob,core,job,0x819651DC));
  CHECK_FALSE(MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kCoreWriter,core,writer+0x10,0x81968528));
  CHECK_FALSE(MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kCoreWriter,core,writer,0x81968524));
  CHECK_FALSE(MatchesNativeAvatarAnimationEdge(static_cast<NativeAvatarAnimationEdge>(255),core,core,0));
  for(const uint32_t at:{0x81674D48u,0x81964E70u,0x81968390u,0x81977310u})
    CHECK(NativeAvatarService::HooksFunction(at));
  CHECK_FALSE(NativeAvatarService::HooksFunction(0x81977314));
}


TEST_CASE("Avatar shared Request producers join each real wrapper to queued allocation",
          "[kernel][native_avatar_contract][native_avatar_request_producer]") {
  struct Row { NativeSystemUiPublicCall call;uint32_t bytes,lr,input,options,app,message,payload; };
  constexpr std::array rows{
    Row{NativeSystemUiPublicCall::kAvatarInitialize,0x90,0x81675688,0x50,0,0xF2,0x600001,40},
    Row{NativeSystemUiPublicCall::kAvatarGetManifestLocalUser,0x70,0x81674994,0x50,0,0xF3,0x600003,8},
    Row{NativeSystemUiPublicCall::kAvatarGetAssets,0x480,0x81674B4C,0x50,0,0xF2,0x600006,1016},
    Row{NativeSystemUiPublicCall::kAvatarSetManifest,0x460,0x81674E3C,0x50,0,0xF3,0x600009,1004},
    Row{NativeSystemUiPublicCall::kAvatarGetMetadataRandom,0x80,0x816752C0,0x58,0x50,0xF2,0x600010,12},
    Row{NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfileCount,0x70,0x81675328,0x50,0,0xF3,0x600013,4},
    Row{NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile,0x70,0x8167539C,0x50,0,0xF3,0x600014,8},
    Row{NativeSystemUiPublicCall::kAvatarLoadAnimation,0xA0,0x81674DC0,0x50,0,0xF2,0x60000F,24},
  };
  for(const auto& row:rows){
    const auto* producer=NativeAvatarRequestStart(row.call);REQUIRE(producer);
    CHECK(producer->app==row.app);CHECK(producer->message==row.message);
    CHECK(producer->payload_bytes==row.payload);
    constexpr uint32_t public_sp=0x70850000,packet=0x40170000;
    const uint32_t start=public_sp-row.bytes,input=start+row.input;
    const uint32_t options=row.options?start+row.options:0;
    CHECK(MatchesNativeAvatarRequestStartEdge(row.call,public_sp,start,row.lr,input,options));
    CHECK_FALSE(MatchesNativeAvatarRequestStartEdge(row.call,public_sp,start,row.lr+4,input,options));
    CHECK_FALSE(MatchesNativeAvatarRequestStartEdge(row.call,public_sp,start,row.lr,input+4,options));
    CHECK_FALSE(MatchesNativeAvatarRequestStartEdge(row.call,public_sp,start,row.lr,input,options?0:start+0x50));
    const uint32_t schedule=start-0xD0,allocation=schedule-0x90;
    CHECK(MatchesNativeAvatarRequestScheduleEdge(start,schedule,0x816A2EC8,packet,
        0x816A2C80,packet,options,options,packet+0x18));
    CHECK(MatchesNativeAvatarRequestAllocationEdge(schedule,allocation,0x816BA46C,
        options,options,allocation+0x50));
    CHECK_FALSE(MatchesNativeAvatarRequestScheduleEdge(start,schedule,0x816A2EC8,packet,
        0x816A2C80,packet+4,options,options,packet+0x18));
    CHECK_FALSE(MatchesNativeAvatarRequestAllocationEdge(schedule,allocation,0x816BA46C,
        options?0:start+0x50,options,allocation+0x50));
  }
  // Direct synchronous size and Shutdown's actual Control producer keep their
  // separate protocols. This table is not a generic callback exemption.
  CHECK_FALSE(NativeAvatarRequestStart(NativeSystemUiPublicCall::kAvatarGetAssetsResultSize));
  CHECK_FALSE(NativeAvatarRequestStart(NativeSystemUiPublicCall::kAvatarShutdown));
}

namespace {
using HookRundown = identity_detail::NativeIdentityHookRundown;
// Real host concurrency against the production admission/release word. The
// held frame returns only after release, and every assertion unwind joins it.
struct HeldHookUse {
  std::promise<void> entered, release;
  std::future<void> ready = entered.get_future();
  std::atomic<bool> acquired{false}, parkable{false};
  std::thread worker;
  bool joined = false;
  HeldHookUse(std::shared_ptr<HookRundown> control, bool may_park) {
    auto done = release.get_future();
    worker = std::thread([this, control = std::move(control), may_park,
                          done = std::move(done)]() mutable {
      HookRundown::Use use;
      acquired.store(use.Acquire(control, std::make_shared<int>(1), may_park),
                     std::memory_order_release);
      parkable.store(use.may_park(), std::memory_order_release);
      entered.set_value();
      done.wait();  // OS host wait, not a guest fiber suspension.
    });
  }
  void ReturnAndJoin() {
    if (joined) return;
    release.set_value();
    worker.join();
    joined = true;
  }
  ~HeldHookUse() { ReturnAndJoin(); }
};
struct HookBackingObservation {
  std::shared_ptr<HookRundown> control;
  uint32_t& active;
  uint32_t& parkable;
  bool& destroyed;
  HookBackingObservation(std::shared_ptr<HookRundown> value, uint32_t& count,
                         uint32_t& parked, bool& ended)
      : control(std::move(value)), active(count), parkable(parked),
        destroyed(ended) {}
  ~HookBackingObservation() {
    const auto value = control->Capture();
    active = value.active;
    parkable = value.parkable;
    destroyed = true;
  }
};
}  // namespace
TEST_CASE("Hook rundown refuses missing ownership without claiming a sealed drain",
          "[native_avatar_contract][hook_rundown]") {
  auto control = std::make_shared<HookRundown>();
  HookRundown::Use use;
  REQUIRE_FALSE(use.Acquire({}, std::make_shared<int>(1), true));
  REQUIRE_FALSE(use.Acquire(control, {}, true));
  REQUIRE_FALSE(use);
  REQUIRE(control->Capture().active == 0);
  REQUIRE_FALSE(control->Capture().fully_drained());
  REQUIRE_FALSE(control->Capture().retain_until_worker_join());
  control->SealParkable();
  REQUIRE(control->Capture().fully_drained());
}
TEST_CASE("Hook rundown seals parkable admission while preserving real cleanup frames",
          "[native_avatar_contract][hook_rundown]") {
  auto control = std::make_shared<HookRundown>();
  auto backing = std::make_shared<int>(1);
  HookRundown::Use parked, late_tick, cleanup;
  REQUIRE(parked.Acquire(control, backing, true));
  REQUIRE(parked.may_park());
  control->SealParkable();
  REQUIRE(late_tick.Acquire(control, backing, true));
  REQUIRE_FALSE(late_tick.may_park());
  REQUIRE(cleanup.Acquire(control, backing, false));
  REQUIRE(control->Capture().active == 3);
  REQUIRE(control->Capture().parkable == 1);
  REQUIRE_FALSE(control->Capture().retain_until_worker_join());
  late_tick.Reset();
  parked.Reset();
  REQUIRE(control->Capture().active == 1);
  REQUIRE(control->Capture().retain_until_worker_join());
  REQUIRE_FALSE(control->Capture().fully_drained());
  cleanup.Reset();
  REQUIRE(control->Capture().fully_drained());
}
TEST_CASE("Hook frame releases its last owning backing before publishing count zero",
          "[native_avatar_contract][hook_rundown]") {
  auto control = std::make_shared<HookRundown>();
  uint32_t active = 0, parkable = 0;
  bool destroyed = false;
  HookRundown::Use use;
  auto backing = std::make_shared<HookBackingObservation>(
      control, active, parkable, destroyed);
  REQUIRE(use.Acquire(control, backing, true));
  backing.reset();
  control->SealParkable();
  REQUIRE_FALSE(destroyed);
  REQUIRE_FALSE(control->Capture().retain_until_worker_join());
  use.Reset();
  REQUIRE(destroyed);
  REQUIRE(active == 1);
  REQUIRE(parkable == 1);
  REQUIRE(control->Capture().fully_drained());
}
TEST_CASE("Hook frame scope keeps outer owner and code holders counted through unwind",
          "[native_avatar_contract][hook_rundown]") {
  auto control = std::make_shared<HookRundown>();
  std::array<uint32_t, 3> active{}, parkable{};
  std::array<bool, 3> destroyed{};
  [&] {
    HookRundown::Use use;  // production wrapper order: before ALL owning locals
    auto owner = std::make_shared<HookBackingObservation>(
        control, active[0], parkable[0], destroyed[0]);
    REQUIRE(use.Acquire(control, owner, true));
    auto work = std::make_shared<HookBackingObservation>(
        control, active[1], parkable[1], destroyed[1]);
    auto code = std::make_shared<HookBackingObservation>(
        control, active[2], parkable[2], destroyed[2]);
    control->SealParkable();
  }();
  for (size_t i = 0; i < destroyed.size(); ++i) {
    REQUIRE(destroyed[i]);
    REQUIRE(active[i] == 1);
    REQUIRE(parkable[i] == 1);
  }
  REQUIRE(control->Capture().fully_drained());
}
TEST_CASE("Moving a live hook lease preserves its parked obligation and releases only replaced use",
          "[native_avatar_contract][hook_rundown]") {
  auto control = std::make_shared<HookRundown>();
  auto backing = std::make_shared<int>(1);
  HookRundown::Use first, replacement;
  REQUIRE(first.Acquire(control, backing, true));
  REQUIRE(replacement.Acquire(control, backing, false));
  REQUIRE_FALSE(first.Acquire(control, backing, false));
  replacement = std::move(first);
  first.Reset();
  REQUIRE_FALSE(first);
  REQUIRE(replacement.Matches(control.get(), backing.get()));
  REQUIRE(control->Capture().active == 1);
  REQUIRE(control->Capture().parkable == 1);
  control->SealParkable();
  REQUIRE_FALSE(control->Capture().retain_until_worker_join());
  replacement.Reset();
  replacement.Reset();
  REQUIRE(control->Capture().fully_drained());
}
TEST_CASE("Parkable hook admission racing stop has one exact production CAS outcome",
          "[native_avatar_contract][hook_rundown]") {
  for (size_t i = 0; i < 32; ++i) {
    auto control = std::make_shared<HookRundown>();
    HeldHookUse entered(control, true);
    control->SealParkable();
    REQUIRE(entered.ready.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    REQUIRE(entered.acquired.load(std::memory_order_acquire));
    const auto snapshot = control->Capture();
    REQUIRE(snapshot.active == 1);
    REQUIRE(snapshot.parkable_sealed);
    REQUIRE(snapshot.parkable ==
            uint32_t(entered.parkable.load(std::memory_order_acquire)));
    REQUIRE(snapshot.retain_until_worker_join() == !snapshot.parkable);
    REQUIRE_FALSE(snapshot.fully_drained());
    entered.ReturnAndJoin();
    REQUIRE(control->Capture().fully_drained());
  }
}
TEST_CASE("Real hook return leaves concurrent cleanup owned until its own host join",
          "[native_avatar_contract][hook_rundown]") {
  auto control = std::make_shared<HookRundown>();
  HeldHookUse parked(control, true);
  REQUIRE(parked.ready.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
  REQUIRE(parked.parkable.load(std::memory_order_acquire));
  control->SealParkable();
  HeldHookUse cleanup(control, false);
  REQUIRE(cleanup.ready.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
  REQUIRE(cleanup.acquired.load(std::memory_order_acquire));
  REQUIRE(control->Capture().active == 2);
  REQUIRE_FALSE(control->Capture().retain_until_worker_join());
  parked.ReturnAndJoin();
  REQUIRE(control->Capture().active == 1);
  REQUIRE(control->Capture().retain_until_worker_join());
  REQUIRE_FALSE(control->Capture().fully_drained());
  cleanup.ReturnAndJoin();
  REQUIRE(control->Capture().fully_drained());
}
}  // namespace xe::kernel::test
namespace xe::kernel::test {
TEST_CASE(
    "Avatar family refuses legacy host replacements of native cache queries",
    "[kernel][native_avatar_contract][native_cache_query]") {
  const auto ordinal = GENERATE(uint16_t(0x20A), uint16_t(0x211));
  const bool witnessed = GENERATE(false, true);
  auto plan = Family();
  plan.bridges.push_back(
      {"xam.xex",
       ordinal,
       0,
       witnessed ? 12u : 0u,
       witnessed
           ? "766cd6615f5d7580131ee3c03e89d7c9e83ea3a78027cf618f78909fa3886151"
           : "",
       {}});
  Denied(plan);
  CHECK(NativeAvatarBridge(ordinal) == nullptr);

  // Removing the internal replacement does not remove title-import HLE.
  cpu::ExportResolver resolver;
  xam::RegisterUserExports(&resolver, nullptr);
  xam::XamModule::RegisterExportTable(&resolver);
  const auto* host = resolver.GetExportByOrdinal("xam.xex", ordinal);
  REQUIRE(host);
  CHECK(host->is_implemented());
  CHECK(host->function_data.trampoline != nullptr);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
namespace {
struct AvatarReturnFrame {
  uint64_t thread;
  uint32_t pcr,sp,lr,process;
  bool operator==(const AvatarReturnFrame&) const = default;
};
enum class AvatarReturnKind : uint8_t { kNone, kCore, kCopy };
struct AvatarReturnRequest {
  uint64_t serial;
  NativeAvatarCallPair<AvatarReturnFrame> core;
  bool* destroyed = nullptr;
  ~AvatarReturnRequest() { if(destroyed)*destroyed=true; }
};
using AvatarReturnBinding = NativeAvatarRequestEntry<AvatarReturnRequest,AvatarReturnKind>;
using AvatarReturnStack = NativeAvatarEntryStack<AvatarReturnFrame,AvatarReturnBinding>;
constexpr AvatarReturnFrame kActualCoreReturn{
    139947520497296ull,0x30052000,0x704DFBC0,0x81965B4C,2};
}
TEST_CASE("Avatar own returns retain exact requests across identical physical frames",
          "[kernel][native_avatar_contract][avatar_request_return]") {
  auto first=std::make_shared<AvatarReturnRequest>();first->serial=7;
  auto second=std::make_shared<AvatarReturnRequest>();second->serial=8;
  REQUIRE(first->core.Enter(kActualCoreReturn,0x200720,101));
  REQUIRE(second->core.Enter(kActualCoreReturn,0x200760,102));
  AvatarReturnStack stack;
  stack.EnterOwned(kActualCoreReturn,{first,7,101,AvatarReturnKind::kCore});
  stack.EnterOwned(kActualCoreReturn,{second,8,102,AvatarReturnKind::kCore});
  AvatarReturnBinding returned;
  REQUIRE(stack.ObserveReturn(kActualCoreReturn,&returned)==NativeAvatarEntryReturn::kOwned);
  REQUIRE(returned.Matches(AvatarReturnKind::kCore));
  REQUIRE(returned.request==second);
  REQUIRE(second->core.Finish(kActualCoreReturn,returned.invocation));
  REQUIRE_FALSE(first->core.returned);
  returned={};
  REQUIRE(stack.ObserveReturn(kActualCoreReturn,&returned)==NativeAvatarEntryReturn::kOwned);
  REQUIRE(returned.request==first);
  REQUIRE(first->core.Finish(kActualCoreReturn,returned.invocation));
  REQUIRE(stack.empty());
  REQUIRE(stack.ObserveReturn(kActualCoreReturn)==NativeAvatarEntryReturn::kAbsent);
}
TEST_CASE("Avatar a new exact return does not invent an older missing request return",
          "[kernel][native_avatar_contract][avatar_request_return]") {
  auto unresolved=std::make_shared<AvatarReturnRequest>();unresolved->serial=20;
  auto current=std::make_shared<AvatarReturnRequest>();current->serial=21;
  REQUIRE(unresolved->core.Enter(kActualCoreReturn,0x200720,201));
  REQUIRE(current->core.Enter(kActualCoreReturn,0x200760,202));
  AvatarReturnStack stack;
  stack.EnterOwned(kActualCoreReturn,{current,21,202,AvatarReturnKind::kCore});
  AvatarReturnBinding returned;
  REQUIRE(stack.ObserveReturn(kActualCoreReturn,&returned)==NativeAvatarEntryReturn::kOwned);
  REQUIRE(returned.Matches(AvatarReturnKind::kCore));
  REQUIRE(returned.request==current);
  REQUIRE(current->core.Finish(kActualCoreReturn,returned.invocation));
  REQUIRE(unresolved->core.entered);
  REQUIRE_FALSE(unresolved->core.returned);
  REQUIRE_FALSE(unresolved->core.Finish(kActualCoreReturn,returned.invocation));
}
TEST_CASE("Avatar request bindings preserve neutral nesting and reject malformed frame adoption",
          "[kernel][native_avatar_contract][avatar_request_return]") {
  auto outer=std::make_shared<AvatarReturnRequest>();outer->serial=30;
  auto child=std::make_shared<AvatarReturnRequest>();child->serial=31;
  const auto actual=kActualCoreReturn;
  auto neutral=actual;neutral.sp-=0x80;neutral.lr=0x816B7EA4;
  auto inner=neutral;inner.sp-=0x80;
  REQUIRE(outer->core.Enter(actual,0x200720,301));
  REQUIRE(child->core.Enter(inner,0x200760,302));
  AvatarReturnStack stack;
  stack.EnterOwned(actual,{outer,30,301,AvatarReturnKind::kCore});
  REQUIRE(stack.EnterPassthroughIfNested(neutral));
  stack.EnterOwned(inner,{child,31,302,AvatarReturnKind::kCore});
  auto wrong=inner;const auto field=GENERATE(0u,1u,2u,3u,4u);
  if(field==0)wrong.thread+=8;else if(field==1)wrong.pcr+=0x1000;
  else if(field==2)wrong.sp+=0x10;else if(field==3)wrong.lr+=4;else wrong.process=1;
  AvatarReturnBinding returned;
  REQUIRE(stack.ObserveReturn(wrong,&returned)==NativeAvatarEntryReturn::kMismatch);
  REQUIRE_FALSE(returned.request);
  REQUIRE(stack.expected()==inner);
  REQUIRE(stack.ObserveReturn(actual,&returned)==NativeAvatarEntryReturn::kMismatch);
  REQUIRE_FALSE(returned.request);
  REQUIRE(stack.ObserveReturn(inner,&returned)==NativeAvatarEntryReturn::kOwned);
  REQUIRE(returned.request==child);
  REQUIRE(child->core.Finish(inner,returned.invocation));
  returned={};
  REQUIRE(stack.ObserveReturn(neutral,&returned)==NativeAvatarEntryReturn::kPassthrough);
  REQUIRE_FALSE(returned.request);
  REQUIRE_FALSE(outer->core.returned);
  REQUIRE(stack.ObserveReturn(actual,&returned)==NativeAvatarEntryReturn::kOwned);
  REQUIRE(returned.request==outer);
  REQUIRE(outer->core.Finish(actual,returned.invocation));
}
TEST_CASE("Avatar request return serials reject stale attempts and wrong pair kinds",
          "[kernel][native_avatar_contract][avatar_request_return]") {
  auto request=std::make_shared<AvatarReturnRequest>();request->serial=40;
  AvatarReturnBinding old{request,40,401,AvatarReturnKind::kCore};
  REQUIRE(request->core.Enter(kActualCoreReturn,0x200720,old.invocation));
  REQUIRE(old.Matches(AvatarReturnKind::kCore));
  REQUIRE_FALSE(old.Matches(AvatarReturnKind::kCopy));
  REQUIRE_FALSE(old.Matches(AvatarReturnKind::kNone));
  REQUIRE_FALSE(request->core.Finish(kActualCoreReturn,old.invocation+1));
  REQUIRE_FALSE(request->core.returned);
  REQUIRE(request->core.Finish(kActualCoreReturn,old.invocation));
  REQUIRE_FALSE(request->core.Finish(kActualCoreReturn,old.invocation));
  request->core={};
  REQUIRE(request->core.Enter(kActualCoreReturn,0x200760,402));
  REQUIRE_FALSE(request->core.Finish(kActualCoreReturn,old.invocation));
  REQUIRE_FALSE(request->core.returned);
  AvatarReturnBinding malformed{request,41,402,AvatarReturnKind::kCore};
  REQUIRE_FALSE(malformed.Matches(AvatarReturnKind::kCore));
  malformed={request,40,0,AvatarReturnKind::kCore};
  REQUIRE_FALSE(malformed.Matches(AvatarReturnKind::kCore));
  REQUIRE(request->core.Finish(kActualCoreReturn,402));
}
TEST_CASE("Avatar request entry retains its target through registry removal and return tail",
          "[kernel][native_avatar_contract][avatar_request_return]") {
  bool destroyed=false;
  auto registry_target=std::make_shared<AvatarReturnRequest>();
  registry_target->serial=50;registry_target->destroyed=&destroyed;
  REQUIRE(registry_target->core.Enter(kActualCoreReturn,0x200720,501));
  std::weak_ptr<AvatarReturnRequest> lifetime=registry_target;
  AvatarReturnStack stack;
  stack.EnterOwned(kActualCoreReturn,{registry_target,50,501,AvatarReturnKind::kCore});
  registry_target.reset();
  REQUIRE_FALSE(lifetime.expired());REQUIRE_FALSE(destroyed);
  auto wrong=kActualCoreReturn;wrong.sp+=0x10;
  AvatarReturnBinding returned;
  REQUIRE(stack.ObserveReturn(wrong,&returned)==NativeAvatarEntryReturn::kMismatch);
  REQUIRE_FALSE(returned.request);REQUIRE_FALSE(destroyed);
  REQUIRE(stack.ObserveReturn(kActualCoreReturn,&returned)==NativeAvatarEntryReturn::kOwned);
  REQUIRE(stack.empty());REQUIRE_FALSE(destroyed);
  REQUIRE(returned.Matches(AvatarReturnKind::kCore));
  REQUIRE(returned.request->core.Finish(kActualCoreReturn,returned.invocation));
  // Exact returned holder remains alive through the host completion tail;
  // no model of native Complete/free/TaskClose success is supplied here.
  returned={};
  REQUIRE(destroyed);REQUIRE(lifetime.expired());
}
}
