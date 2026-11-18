/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_avatar_contract.h"
#include <algorithm>
#include <iterator>
#include <set>
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
namespace xe::kernel {
namespace {
constexpr NativeAvatarBridgeProfile kBridges[] = {
    {0x231, 0x816DB860, 236, "bac3b00a84577306d315de348e379aa6690507ea3c12129e2fde30e7a8d6ee91"},
    {0x232, 0x816D7860, 56, "d23aaadb31b86b58d4ab63b6fe17e3c7e58ebc17e5ee7dd89609db84508ee819"},
    {0x22A, 0x816B16A8, 388, "3b796e145f20c340cb0aab1f1a667552fa919dbe39129aad96597f8ac002f8c6"},
    {0x21A, 0x816B1968, 180, "93fccebe6fd57253625afd6e24dbde729bf48aeec2f0ee75a0efec57e87cc09d"},
    {0x269, 0x816EFD80, 16, "e9fb780e48b3e55766bf8ed91cab8db437611aa3541e47b6e2136a639c86cebd"},
    {0x25A, 0x816EEB78, 188, "0b6af2294c7e6fb7747029aaa683bf97ec1878f62f1d408726c478ddd230465d"},
};
constexpr NativeAvatarStartupProfile kStartup[] = {
    {"native-avatar-f3-descriptor-setup", 0x816BCC38, 108, "649c43fbcffe629316d6b1fcf4c61b58f25964e6902b12ccc01a8b70955956a9", 6, true},
    {"native-avatar-f3-load", 0x816BCD08, 600, "a7630ef93675d8db35d1c13e15ed517d54ad8e66211a1d88acb2f6ae8d438987", 4, false},
};
constexpr NativeAvatarRequestStartProfile kRequestStarts[] = {
  {NativeSystemUiPublicCall::kAvatarInitialize,0x90,0x81675688,0x50,0,0xF2,0x600001,40,0xFF},
  {NativeSystemUiPublicCall::kAvatarGetManifestLocalUser,0x70,0x81674994,0x50,0,0xF3,0x600003,8,2},
  {NativeSystemUiPublicCall::kAvatarGetAssets,0x480,0x81674B4C,0x50,0,0xF2,0x600006,1016,5},
  {NativeSystemUiPublicCall::kAvatarSetManifest,0x460,0x81674E3C,0x50,0,0xF3,0x600009,1004,2},
  {NativeSystemUiPublicCall::kAvatarGetMetadataRandom,0x80,0x816752C0,0x58,0x50,0xF2,0x600010,12,3},
  {NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfileCount,0x70,0x81675328,0x50,0,0xF3,0x600013,4,1},
  {NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile,0x70,0x8167539C,0x50,0,0xF3,0x600014,8,2},
  {NativeSystemUiPublicCall::kAvatarLoadAnimation,0xA0,0x81674DC0,0x50,0,0xF2,0x60000F,24,3},
};
constexpr NativeSystemUiPublicCall kPublic[] = {
  NativeSystemUiPublicCall::kAvatarInitialize,
  NativeSystemUiPublicCall::kAvatarShutdown,
  NativeSystemUiPublicCall::kAvatarGetAssetsResultSize,
  NativeSystemUiPublicCall::kAvatarGetAssets,
  NativeSystemUiPublicCall::kAvatarGetMetadataRandom,
  NativeSystemUiPublicCall::kAvatarGetManifestLocalUser,
  NativeSystemUiPublicCall::kAvatarSetManifest,
  NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfileCount,
  NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile,
};
}  // namespace
const NativeAvatarRequestStartProfile* NativeAvatarRequestStart(
    NativeSystemUiPublicCall call) noexcept {
  for(const auto& p:kRequestStarts)if(p.call==call)return &p;
  return nullptr;
}
bool MatchesNativeAvatarRequestStartEdge(NativeSystemUiPublicCall call,
    uint32_t public_sp,uint32_t start_sp,uint32_t start_lr,uint32_t input,
    uint32_t parameters) noexcept {
  const auto* p=NativeAvatarRequestStart(call);
  return p && public_sp && !(public_sp&15) && public_sp>p->stack_bytes &&
      start_sp==public_sp-p->stack_bytes && start_lr==p->return_lr &&
      start_sp<=UINT32_MAX-p->input_offset && input==start_sp+p->input_offset &&
      (p->options_offset ? start_sp<=UINT32_MAX-p->options_offset &&
           parameters==start_sp+p->options_offset : parameters==0);
}
bool MatchesNativeAvatarRequestScheduleEdge(uint32_t start_sp,uint32_t schedule_sp,
    uint32_t schedule_lr,uint32_t packet,uint32_t callback,uint32_t argument,
    uint32_t parameters,uint32_t expected_parameters,uint32_t output) noexcept {
  return start_sp>0xD0 && !(start_sp&15) && schedule_sp==start_sp-0xD0 &&
      schedule_lr==0x816A2EC8 && packet>=0x1000 && !(packet&3) &&
      packet<=UINT32_MAX-0x1C && callback==0x816A2C80 && argument==packet &&
      parameters==expected_parameters && output==packet+0x18;
}
bool MatchesNativeAvatarRequestAllocationEdge(uint32_t schedule_sp,uint32_t allocation_sp,
    uint32_t allocation_lr,uint32_t parameters,uint32_t expected_parameters,
    uint32_t output) noexcept {
  return schedule_sp>0x90 && !(schedule_sp&15) && allocation_sp==schedule_sp-0x90 &&
      allocation_lr==0x816BA46C && parameters==expected_parameters &&
      allocation_sp<=UINT32_MAX-0x50 && output==allocation_sp+0x50;
}
std::span<const NativeAvatarBridgeProfile> NativeAvatarBridgeProfiles() {
  return kBridges;
}
const NativeAvatarBridgeProfile* NativeAvatarBridge(uint16_t ordinal) {
  for (const auto& p : kBridges) if (p.ordinal == ordinal) return &p;
  return nullptr;
}
std::span<const NativeAvatarStartupProfile> NativeAvatarStartupProfiles() {
  return kStartup;
}
bool MatchesNativeAvatarResidentEdge(NativeAvatarResidentEdge edge,
                                    uint32_t parent_sp, uint32_t child_sp,
                                    uint32_t child_lr) {
  uint32_t bytes = 0, lr = 0;
  switch (edge) {
    case NativeAvatarResidentEdge::kFrontendBuiltin:
      bytes = 0x60; lr = 0x816BDEF8; break;
    case NativeAvatarResidentEdge::kBuiltinSetup:
      bytes = 0xB0; lr = 0x816BD36C; break;
    case NativeAvatarResidentEdge::kBuiltinLoad:
      bytes = 0xB0; lr = 0x816BD3B4; break;
    case NativeAvatarResidentEdge::kLoadCallback:
      bytes = 0xB0 + 0x90 + 0x80; lr = 0x816A2A24; break;
    case NativeAvatarResidentEdge::kCallbackLock:
      bytes = 0x70; lr = 0x819816B4; break;
    default: return false;
  }
  return parent_sp && !(parent_sp & 15) && parent_sp >= bytes && child_sp &&
         child_sp == parent_sp - bytes && child_lr == lr;
}
bool NativeAvatarTaskReferenceMatchesBody(uint32_t tagged_reference,
                                         uint32_t raw_body) {
  return tagged_reference >= 0x1000 && (tagged_reference & 3) == 1 &&
         raw_body >= 0x1000 && !(raw_body & 3) &&
         (tagged_reference & ~uint32_t(3)) == raw_body;
}
bool MatchesNativeAvatarInitStartEdge(uint32_t public_sp,uint32_t start_sp,uint32_t start_lr) {
  return public_sp>0x90 && !(public_sp&15) && start_sp==public_sp-0x90 && start_lr==0x81675688;
}
bool MatchesNativeAvatarInitScheduleEdge(uint32_t start_sp,uint32_t schedule_sp,
    uint32_t schedule_lr,uint32_t packet,uint32_t callback,uint32_t argument,
    uint32_t options,uint32_t output) {
  return start_sp>0xD0 && !(start_sp&15) && schedule_sp==start_sp-0xD0 &&
      schedule_lr==0x816A2EC8 && packet>=0x1000 && !(packet&3) &&
      packet<=UINT32_MAX-0x1C && callback==0x816A2C80 && argument==packet &&
      options==0 && output==packet+0x18;
}
bool MatchesNativeAvatarInitAllocationEdge(uint32_t schedule_sp,uint32_t allocation_sp,
    uint32_t allocation_lr,uint32_t parameters,uint32_t output) {
  return schedule_sp>0x90 && !(schedule_sp&15) && allocation_sp==schedule_sp-0x90 &&
      allocation_lr==0x816BA46C && parameters==0 && allocation_sp<=UINT32_MAX-0x50 &&
      output==allocation_sp+0x50;
}

std::optional<NativeAvatarAnimationBuffers> CaptureNativeAvatarAnimationBuffers(
    uint32_t control,uint32_t capacity,uint32_t data) noexcept {
  if(!control || NativeAvatarAnimationBuffers::kControlBytes-1>UINT32_MAX-control ||
     (capacity && (!data || capacity-1>UINT32_MAX-data)))return {};
  return NativeAvatarAnimationBuffers{control,capacity,data};
}
bool MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge edge,
    uint32_t parent_sp,uint32_t child_sp,uint32_t child_lr) noexcept {
  uint32_t bytes=0,lr=0;
  switch(edge){
    case NativeAvatarAnimationEdge::kWorkerJob:bytes=0xA0;lr=0x819651DC;break;
    case NativeAvatarAnimationEdge::kJobCore:bytes=0x60;lr=0x81964E94;break;
    case NativeAvatarAnimationEdge::kCoreWriter:bytes=0x4C0;lr=0x81968528;break;
    default:return false;
  }
  return parent_sp && !(parent_sp&15) && parent_sp>=bytes && child_sp &&
      child_sp==parent_sp-bytes && child_lr==lr;
}
NativeAvatarPlanFamily CheckNativeAvatarPlanFamily(
    const ValidatedGuestRuntimePlan& plan, std::string* error) {
  bool present = false;
  for (const auto& bridge : plan.bridges)
    present |= bridge.library == "xam.xex" && bridge.code_size &&
               NativeAvatarBridge(bridge.ordinal);
  for (const auto& role : kStartup)
    present |= plan.catalog.contains(std::string(role.identity));
  // Public catalog entries remain passive metadata in older plans. Only an
  // explicit full-entry provider/startup family requests this owned service.
  if (!present) return NativeAvatarPlanFamily::kAbsent;
  // These native entries consume the published native cache. Replacing them
  // with host-roster getters can report a user before that cache is published.
  // Refuse legacy witnessed AND bare selectors instead of silently ignoring
  // them. Ordinary title imports continue using the existing HLE exports.
  for (const auto& bridge : plan.bridges) {
    if (bridge.library == "xam.xex" &&
        (bridge.ordinal == 0x20A || bridge.ordinal == 0x211)) {
      if (error) *error = "Native avatar cache queries must retain native bodies";
      return NativeAvatarPlanFamily::kInvalid;
    }
  }
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return NativeAvatarPlanFamily::kInvalid;
  };
  for (const auto& required : kBridges) {
    size_t found = 0;
    for (const auto& b : plan.bridges) {
      if (b.library != "xam.xex" || b.ordinal != required.ordinal) continue;
      if (b.address || !b.service.empty() || b.code_size != required.bytes ||
          b.code_sha256 != required.sha256) return fail("Avatar provider witness mismatch");
      ++found;
    }
    if (found != 1) return fail("Incomplete/duplicate full-entry avatar provider");
  }
  for (const auto& role : kStartup) {
    const auto f = plan.catalog.find(std::string(role.identity));
    const auto c = plan.contracts.find(std::string(role.identity));
    if (f == plan.catalog.end() || c == plan.contracts.end() ||
        f->second.address != role.address ||
        f->second.arguments != std::vector<uint64_t>(role.arguments, 0) ||
        f->second.return_kind != (role.returns_void ? GuestRuntime::ReturnKind::kVoid
                                                  : GuestRuntime::ReturnKind::kHResult) ||
        c->second.caller != ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly ||
        c->second.result != (role.returns_void ? ValidatedGuestRuntimePlan::ResultPolicy::kVoid
                                              : ValidatedGuestRuntimePlan::ResultPolicy::kHResult) ||
        !c->second.before.empty() || !c->second.after.empty())
      return fail("Avatar named startup contract mismatch");
    for (const auto& phase : plan.plan.phases)
      for (const auto& init : phase.functions)
        if (init.address == role.address) return fail("Avatar startup appears in heap initialization");
  }
  std::vector<NativeSystemUiPublicCall> public_calls(std::begin(kPublic),std::end(kPublic));
  const auto* animation=NativeSystemUiPublicProfile(NativeSystemUiPublicCall::kAvatarLoadAnimation);
  if(plan.catalog.contains(std::string(animation->identity)) ||
     plan.contracts.contains(std::string(animation->identity)))
    public_calls.push_back(animation->call);
  for (const auto call : public_calls) {
    const auto* p = NativeSystemUiPublicProfile(call);
    const auto f = plan.catalog.find(std::string(p->identity));
    const auto c = plan.contracts.find(std::string(p->identity));
    const bool is_void = call == NativeSystemUiPublicCall::kAvatarShutdown;
    if (f == plan.catalog.end() || c == plan.contracts.end() ||
        f->second.address != p->address || f->second.arguments != std::vector<uint64_t>(p->argument_count,0) ||
        f->second.return_kind != (is_void ? GuestRuntime::ReturnKind::kVoid
                                         : GuestRuntime::ReturnKind::kUInt32) ||
        c->second.caller != ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem ||
        c->second.result != (is_void ? ValidatedGuestRuntimePlan::ResultPolicy::kVoid
                                     : ValidatedGuestRuntimePlan::ResultPolicy::kAny) ||
        !c->second.before.empty() || !c->second.after.empty())
      return fail("Avatar original-caller contract mismatch");
    for (const auto& phase : plan.plan.phases)
      for (const auto& init : phase.functions)
        if (init.address == p->address) return fail("Avatar public call appears in heap initialization");
  }
  if (error) error->clear();
  return NativeAvatarPlanFamily::kComplete;
}
}  // namespace xe::kernel
