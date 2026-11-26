/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_identity_contract.h"
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/native_avatar_contract.h"
namespace xe::kernel {
namespace {
constexpr NativeProfileIdentityBridgeProfile kBridges[]={
 {0x20E,0x816DD040,216,"c4ef112e193048eacb415e7655aee203c126a439ec7d4274dd8a488e59b6470f"},
 {0x227,0x816DDB20,524,"e07774f3a7f3405cdbf3db3c410a3805ef2496e1942b5b52e783afd7e60a06b0"},
};
constexpr NativeProfileIdentityIngressProfile kIngress[]={
 {0x20B,"native-owned-profile-logon-20b",0x8199C630,124,
  "a0aa03126283a4f01087fb97d1a89046b3c93d3673fbc445c7bcf500331552af"},
 {0x224,"native-owned-profile-logon-224",0x8199E3E0,3324,
  "de904615c0b3a5147b78cedaa7593a0d8482fbc2192274694d96e4eb0464fea1"},
};
}
std::span<const NativeProfileIdentityBridgeProfile> NativeProfileIdentityBridgeProfiles(){return kBridges;}
const NativeProfileIdentityBridgeProfile* NativeProfileIdentityBridge(uint16_t ordinal){
 for(const auto& b:kBridges)if(b.ordinal==ordinal)return &b;return nullptr;
}
std::span<const NativeProfileIdentityIngressProfile> NativeProfileIdentityIngressProfiles(){return kIngress;}
NativeProfileIdentityPlanFamily CheckNativeProfileIdentityPlanFamily(
    const ValidatedGuestRuntimePlan& plan,std::string* error){
 bool present=false;
 for(const auto& b:plan.bridges)present|=b.library=="xam.xex" && b.code_size && NativeProfileIdentityBridge(b.ordinal);
 for(const auto& role:kIngress)present|=plan.catalog.contains(std::string(role.identity));
 if(!present)return NativeProfileIdentityPlanFamily::kAbsent;
 const auto fail=[&](const char* why){if(error)*error=why;return NativeProfileIdentityPlanFamily::kInvalid;};
 // Reuse the actual current common six-profile/content bridge and F3 owner
 // family. This does not initialize F2 geometry or enable broader UI exports.
 if(CheckNativeAvatarPlanFamily(plan)!=NativeAvatarPlanFamily::kComplete)
   return fail("Owned identity requires the complete shared profile/content family");
 for(const auto& expected:kBridges){
  size_t found=0;
  for(const auto& actual:plan.bridges){
   if(actual.library!="xam.xex" || actual.ordinal!=expected.ordinal)continue;
   if(actual.address || !actual.service.empty() || actual.code_size!=expected.bytes || actual.code_sha256!=expected.sha256)
    return fail("Identity getter full-entry witness mismatch");
   ++found;
  }
  if(found!=1)return fail("Missing or duplicate implemented identity getter bridge");
 }
 for(const auto& role:kIngress){
  const auto f=plan.catalog.find(std::string(role.identity));
  const auto c=plan.contracts.find(std::string(role.identity));
  if(f==plan.catalog.end() || c==plan.contracts.end() || f->second.address!=role.address ||
     f->second.arguments!=std::vector<uint64_t>(3,0) ||
     f->second.return_kind!=GuestRuntime::ReturnKind::kUInt32 ||
     c->second.caller!=ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem ||
     c->second.result!=ValidatedGuestRuntimePlan::ResultPolicy::kAny ||
     !c->second.before.empty() || !c->second.after.empty())
    return fail("Owned original logon ingress contract mismatch");
  for(const auto& p:plan.plan.phases)for(const auto& init:p.functions)
    if(init.address==role.address)return fail("Original logon cannot be a heap initializer");
 }
 if(error)error->clear();return NativeProfileIdentityPlanFamily::kComplete;
}
} // namespace xe::kernel

namespace xe::kernel::identity_detail {
std::optional<NativeIdentityJoinedHostStack>
NativeIdentityJoinedHostStack::JoinActual(XThread& thread,std::chrono::milliseconds timeout) {
  // This replaces the Service's existing exact Bootstrap OS wait, not an
  // additional poll or a guest/Task wait. On-fiber host routines have no
  // enclosing OS-thread join here and remain unproved.
  if(XThread::IsInThread() || XThread::GetCurrentFiberThread() ||
      global_critical_region::is_held_by_current_thread() || !thread.is_host_thread() ||
      !thread.kernel_state() || !thread.thread_state() || !thread.thread())return {};
  auto* host=thread.thread();
  if(threading::Wait(host,false,timeout)!=threading::WaitResult::kSuccess)return {};
  auto owned=std::make_shared<object_ref<XThread>>(retain_object(&thread));
  std::shared_ptr<const XThread> exact(owned,&thread);
  return NativeIdentityJoinedHostStack(thread.kernel_state(),std::move(exact),
      thread.thread_state(),host);
}
bool NativeIdentityJoinedHostStack::Matches(const XThread& thread) const noexcept {
  return thread_.get()==&thread && kernel_==thread.kernel_state() &&
      cpu_==thread.thread_state();
}
bool NativeIdentityOrdinaryTransport::TaskTransportFunction(uint32_t at) noexcept {
  // Exact shared primitive entry-routing family, not a callback whitelist or
  // native execution/read permission. ClaimTaskTransport remains its issuer.
  switch(at){
    case 0x816A2CF0:case 0x816A2A48:case 0x8190A358:case 0x819068D0:
    case 0x816BA440:case 0x816BA668:case 0x816B9D68:
    case 0x816B9A18:case 0x816B9BB8:case 0x816B9AA8:
    case 0x816B8B08:case 0x816B9F08:case 0x816BA058:case 0x816B7490:
    case 0x816B7D98:case 0x816B7500:case 0x816E6AB0:
    case 0x816B7B68:case 0x816B7600:return true;
    default:return false;
  }
}
std::optional<NativeIdentityOrdinaryTransport>
NativeIdentityOrdinaryTransport::Capture(Frame frame,XThread& thread,Kind kind,
    const CompletionRoute* route) noexcept {
  const auto actual=thread.native_frame_thread_identity();
  if(!frame.valid() || !thread.kernel_state() || !thread.thread_state() ||
      frame.thread!=thread.thread_id() || frame.pcr!=thread.pcr_ptr() ||
      (actual.process_type!=X_PROCTYPE_SYSTEM && actual.process_type!=X_PROCTYPE_TITLE))return {};
  // is_host_thread=false alone also describes XHostThread(on_fiber=true).
  // Such host_fn/GuestRuntime facades are not a pure guest stack witness.
  const bool guest=thread.is_guest_thread() && !thread.is_host_thread() &&
      thread.fiber() && !thread.thread();
  return NativeIdentityOrdinaryTransport(frame,thread,thread.kernel_state(),
      thread.thread_state(),thread.fiber(),thread.thread(),guest,kind,route);
}
std::optional<NativeIdentityOrdinaryTransport>
NativeIdentityOrdinaryTransport::CaptureTask(Frame frame,XThread& thread) noexcept {
  return TaskTransportFunction(frame.function)?Capture(frame,thread,Kind::kTaskMetadata):std::nullopt;
}
std::optional<NativeIdentityOrdinaryTransport>
NativeIdentityOrdinaryTransport::CaptureCompletion(Frame frame,XThread& thread,
    const CompletionRoute& route) noexcept {
  if(route.kind()!=CompletionRoute::Kind::kOrdinary || !route.FrameMatches(frame) ||
      thread.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM)return {};
  return Capture(frame,thread,Kind::kOrdinaryCompletion,&route);
}
bool NativeIdentityOrdinaryTransport::Matches(Frame frame,const XThread& thread) const noexcept {
  return frame==frame_ && &thread==thread_ && kernel_==thread.kernel_state() &&
      cpu_==thread.thread_state() && frame.thread==thread.thread_id() && frame.pcr==thread.pcr_ptr();
}
bool NativeIdentityOrdinaryTransport::Return(Frame frame,const XThread& thread) noexcept {
  if(phase_==Phase::kReturned)return false; // Never erase an actual own-return.
  if(phase_!=Phase::kEntered || !Matches(frame,thread)){
    phase_=Phase::kUncertain;return false;
  }
  phase_=Phase::kReturned;return true;
}
bool NativeIdentityOrdinaryTransport::RetainThroughActualStop(KernelState* kernel,Root root,
    Frame frame,const XThread& thread,const CompletionRoute* route,
    const NativeIdentityJoinedHostStack* joined) const noexcept {
  if(phase_==Phase::kReturned || kernel!=kernel_ || root!=frame_.root || !Matches(frame,thread))return false;
  if(kind_==Kind::kOrdinaryCompletion ? !route || route!=route_ ||
                                      route->kind()!=CompletionRoute::Kind::kOrdinary
                                    : route!=nullptr || route_!=nullptr)return false;
  // Known missing/malformed native return keeps its uncertainty. This is only
  // retained resource coverage until the existing all-CPU/IO/GPU stop, never
  // a normal return or Task/ref/free/completion receipt.
  if(guest_fiber_)return thread.is_guest_thread() && !thread.is_host_thread() &&
      thread.fiber()==fiber_ && !host_;
  return joined && host_ && joined->host_==host_ && joined->Matches(thread);
}
} // namespace xe::kernel::identity_detail
