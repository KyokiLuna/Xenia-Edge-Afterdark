/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_identity_contract.h"
#include <vector>
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
