/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_identity_owner.h"
#include "xenia/kernel/native_xmsg_provenance.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/kernel/native_system_graphics_transport.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_avatar_service.h"
#include "xenia/kernel/native_identity_listener_bridge.h"
#include "xenia/kernel/native_identity_tile_work.h"
#include "xenia/kernel/native_identity_image_observation.h"
#include "xenia/kernel/native_identity_image_journal.h"
#include "xenia/kernel/native_identity_image_frame.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/native_identity_image_code.h"
#include "xenia/kernel/native_identity_default_image.h"
#include "xenia/kernel/native_profile_identity_code.h"
#include "xenia/kernel/native_profile_identity_contract.h"
#include "xenia/kernel/native_profile_identity_hooks.h"
#include "xenia/kernel/native_profile_identity_request.h"
#include "xenia/kernel/native_profile_identity_task_context.h"
#include "xenia/kernel/native_profile_identity_tasks.h"
#include "xenia/kernel/native_profile_identity_wake_owner.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_task_continuation.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/base/native_system_ui_trace.h"
#include <cstring>
#include "xenia/kernel/xam/profile_roster.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
using Tasks=identity_detail::NativeProfileIdentityTasks;
using Wake=identity_detail::NativeIdentityWakeOwner;
using Listeners=NativeIdentityListenerBridge;
using Frame=Tasks::Frame;
using Events=NativeIdentityEventLedger;
using PaintObservation=gpu::NativeFramePaintDrainObservation;
using PaintStage=PaintObservation::Stage;
using AllocationFact=NativeProfileIdentityOwner::TaskAllocationReturnObservation;
struct TaskAllocationCut {
 const void* owner=nullptr;cpu::ppc::PPCContext* context=nullptr;
 cpu::ThreadState* cpu=nullptr;AllocationFact fact;bool available=false;
};
// Scalar cut scratch, not a Task/identity registry. Only the private common
// return adapter can consume it, on this exact host thread before PPC resumes.
thread_local TaskAllocationCut task_allocation_cut;
void ClearTaskAllocationCut() noexcept {task_allocation_cut={};}
struct PublishTaskAllocationCut {
 const void* owner;cpu::ppc::PPCContext& context;std::optional<AllocationFact> value;
 ~PublishTaskAllocationCut(){
  if(value)task_allocation_cut={owner,&context,context.thread_state,*value,true};
 }
};
AllocationFact AllocationReturnFact(Frame allocator,Frame producer,uint32_t status,
    std::optional<Tasks::Birth> birth) noexcept {
 const auto frame=[](Frame value){return AllocationFact::FrameFact{
     value.thread,value.pcr,value.sp,value.lr,value.function};};
 AllocationFact fact{{allocator.root.kernel,allocator.root.domain,allocator.root.generation},
     frame(allocator),frame(producer),status};
 if(int32_t(status)>=0 && birth){fact.task=birth->task;fact.birth_generation=birth->generation;
  fact.birth_origin=birth->origin;fact.validated_birth=true;}
 return fact;
}
static_assert(X_PROCTYPE_SYSTEM==2); // native Task/process ABI metadata policy
constexpr uint32_t kAbort=0x80004004u;
constexpr uint64_t kBootUnjoinedSource=1;
// Physical input keeps its boot origin for its entire real Task lifetime.
// The initial common read envelope is separately unjoined, never Prepared.
constexpr uint64_t kInitialReadSource=2;
// A Query captures one immutable identity choice. Revocation is deny-only;
// neither hidden refresh nor an old cached image can ever be re-enabled.
struct IdentityPixelLifetime {std::atomic<bool> closed{false};};
class IdentityPixelGuard final : public gpu::NativeFramePresentationGuard {
 public:
  IdentityPixelGuard(uint64_t source,bool visible,
      std::shared_ptr<const IdentityPixelLifetime> lifetime)
      :source_(source),visible_(visible),lifetime_(std::move(lifetime)){}
  bool IsLive() const noexcept override {return !IsRevoked();}
  bool IsRevoked() const noexcept override {
    return !visible_ || !lifetime_ || lifetime_->closed.load(std::memory_order_acquire) ||
        revoked_.load(std::memory_order_acquire);
  }
  void Revoke() noexcept {revoked_.store(true,std::memory_order_release);}
  uint64_t source() const noexcept {return source_;}
 private:
  const uint64_t source_;
  const bool visible_;
  const std::shared_ptr<const IdentityPixelLifetime> lifetime_;
  std::atomic<bool> revoked_{false};
};
// Metadata attached to the EXISTING immutable Source/pixel guard. It owns no
// native object, buffer, module, cache, request or roster. Mutable capture bits
// below are guarded only by the same actual Owner mutex.
class IdentityImageFrameGuard final : public gpu::NativeFramePresentationGuard {
 public:
  IdentityImageFrameGuard(std::shared_ptr<const gpu::NativeFramePresentationGuard> parent,
      std::weak_ptr<NativeProfileIdentityOwner> owner,Tasks::Root root,uint64_t source,uint64_t render,
      std::array<std::optional<identity_detail::NativeImageDraw>,64> draws,bool empty_initial=false)
      :parent(std::move(parent)),owner(std::move(owner)),root(root),source(source),render(render),
       draws(std::move(draws)),empty_initial(empty_initial){}
  bool IsLive() const noexcept override {return parent && parent->IsLive();}
  bool IsRevoked() const noexcept override {return !parent || parent->IsRevoked();}
  const std::shared_ptr<const gpu::NativeFramePresentationGuard> parent;
  const std::weak_ptr<NativeProfileIdentityOwner> owner;
  const Tasks::Root root;const uint64_t source,render;
  const std::array<std::optional<identity_detail::NativeImageDraw>,64> draws;
  const bool empty_initial;
  mutable bool captured=false,completed=false;
  mutable uintptr_t request=0;mutable uint64_t backend=0;
  mutable gpu::NativeFrameSubmissionIdentity identity;
};
XThread* ActualThread() noexcept {
 auto* thread=XThread::GetCurrentFiberThread();
 return thread?thread:(XThread::IsInThread()?XThread::GetCurrentThread():nullptr);
}
std::optional<uint8_t> Byte(Memory& memory,uint32_t at) {
 if(at<0x1000 || memory.QueryRangeAccess(at,1)==memory::PageAccess::kNoAccess)return {};
 return std::atomic_ref<uint8_t>(*memory.TranslateVirtual<uint8_t*>(at)).load(std::memory_order_acquire);
}
std::optional<uint32_t> Word(Memory& memory,uint32_t at) {
 if(at<0x1000 || (at&3) || at>UINT32_MAX-3 ||
    memory.QueryRangeAccess(at,4)==memory::PageAccess::kNoAccess)return {};
 return xe::byte_swap(std::atomic_ref<uint32_t>(*memory.TranslateVirtual<uint32_t*>(at)).load(std::memory_order_acquire));
}
std::optional<uint64_t> Qword(Memory& memory,uint32_t at) {
 if(at<0x1000 || (at&7) || at>UINT32_MAX-7 ||
    memory.QueryRangeAccess(at,8)==memory::PageAccess::kNoAccess)return {};
 return xe::byte_swap(std::atomic_ref<uint64_t>(*memory.TranslateVirtual<uint64_t*>(at)).load(std::memory_order_acquire));
}
bool CommonTaskFunction(uint32_t at) noexcept {
 switch(at){
  case 0x816BA440:case 0x816BA668:case 0x816B9D68:
  case 0x816B9A18:case 0x816B9BB8:case 0x816B9AA8:
  case 0x816B8B08:case 0x816B7DE8:case 0x816B7600:case 0x816B7B68:
  case 0x816B9F08:case 0x816BA058:case 0x816B7490:
  case 0x816B7D98:case 0x816B7500:case 0x816E6AB0:return true;
  default:return false;
 }
}
bool ProfileCallback(uint32_t at) noexcept {
 return Tasks::ClassifyCallback(at)==Tasks::CallbackClaim::kCanonicalProfile;
}
bool ListenerOuter(uint32_t at) noexcept {
 return at==0x816A8498 || at==0x816A88E8 || at==0x816A93C8 || at==0x816A8C00;
}
Listeners::Registers ListenerRegisters(const cpu::ppc::PPCContext& c) noexcept {
 Listeners::Registers out;
 out.context=reinterpret_cast<uintptr_t>(&c);
 out.thread_state=reinterpret_cast<uintptr_t>(c.thread_state);
 out.kernel=reinterpret_cast<uintptr_t>(c.kernel_state);out.lr=c.lr;
 std::copy(std::begin(c.r),std::end(c.r),out.r.begin());return out;
}
using identity_detail::DwordIs;
uint32_t NativeAtomicWord(const uint32_t* word) noexcept {
 return xe::byte_swap(std::atomic_ref<uint32_t>(*const_cast<uint32_t*>(word)).load(std::memory_order_acquire));
}
uint64_t NativeAtomicQword(const uint64_t* word) noexcept {
 return xe::byte_swap(std::atomic_ref<uint64_t>(*const_cast<uint64_t*>(word)).load(std::memory_order_acquire));
}
uint64_t Abort(cpu::ppc::PPCContext& c,uint32_t entry=0) noexcept {
 // XNotifyGetNext returns BOOL. An HRESULT is nonzero and would make its
 // original caller read untouched output words as a successful dequeue.
 c.r[3]=entry==0x816A8C00?0:uint64_t(int64_t(int32_t(kAbort)));return 0;
}
bool CheckIdentityCode(GuestRuntimeCallExecutor& calls) {
 auto module=calls.module();
 auto* memory=calls.kernel()->memory();
 if(!module || !module->xex_module() || !module->hash() ||
    *module->hash()!=0xDD9939F8B910C18Dull ||
    module->xex_module()->base_address()!=0x815F0000 ||
    module->xex_module()->image_size()!=0x610000 ||
    module->xex_module()->image_code_sha1()!="D8F94A9D7D09F731B12215B7BAB23847564F06AE")return false;
 auto read=global_critical_region::AcquireDirect();
 for(const auto& code:identity_detail::kNativeIdentityCode){
  if(code.address<0x815F0000 || uint64_t(code.address)+code.bytes>0x81C00000ull ||
     memory->QueryRangeAccess(code.address,code.bytes)==memory::PageAccess::kNoAccess)return false;
  for(uint64_t at=code.address;at<uint64_t(code.address)+code.bytes;at=(at&~uint64_t(0xFFF))+0x1000){
   xex2_section_type type;
   if(calls.kernel()->GetUserModuleForAddress(uint32_t(at)).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(uint32_t(at),&type) || type!=XEX_SECTION_CODE)return false;
  }
  std::array<uint8_t,32> digest{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(code.address),code.bytes,digest.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t i=0;i<digest.size();++i)
   if(code.sha256[i*2]!=hex[digest[i]>>4] || code.sha256[i*2+1]!=hex[digest[i]&15])return false;
 }
 return true;
}
// Functional authentication of the common original Task lifecycle. Raw page
// bytes AND actual translated code are checked; load generation alone is not
// an immutability receipt. Neutral failure remains local to that observation.
bool CheckTaskExecutionCode(GuestRuntimeCallExecutor& calls) {
 static constexpr uint32_t group[]={0x816BA440,0x816B9A18,0x816B9BB8,0x816B99C0,0x816B9958,0x816B9890,0x816B88D8,0x816B7600,0x816B8B08,0x816B8A88,0x816B8BA0,0x816B8F50,0x816B8D98,0x816B8EE0,0x816B91A8,0x816B9748,0x816B8998,0x816B82B0,0x816B8320,0x816B8530,0x816B9AA8,0x816BA6B0,0x816BA530,0x816BA3C0,0x816B9F08,0x816BA658,0x816BA058,0x816BA660,0x816B7490,0x816B7B80,0x816B7D98,0x816B7D28,0x816B7500,0x816E6AB0,0x816B8D28,0x816B8FC8,0x816B9018,0x816B9218,0x816BA8D0,0x816B77B0,0x816B79E8,0x816B7A28,0x816B9258,0x816B9D68,0x816B7DE8,0x816B9528,0x816B9668,0x816BA668,0x816B7B68,0x816B8098,0x816B7F30,0x816B7EF0,0x816B92C8,0x816B94A0};
 const auto module=calls.module();auto* memory=calls.kernel()->memory();
 if(!module || !module->xex_module() || !module->native_import_ready())return false;
 auto read=global_critical_region::AcquireDirect();
 for(uint32_t at:group){
  const auto found=std::find_if(std::begin(identity_detail::kNativeIdentityCode),
      std::end(identity_detail::kNativeIdentityCode),[&](const auto& item){return item.address==at;});
  if(found==std::end(identity_detail::kNativeIdentityCode) ||
     memory->QueryRangeAccess(at,found->bytes)==memory::PageAccess::kNoAccess)return false;
  // XEX section ownership is page based. Keep the full byte/hash and
  // instruction-level actual CPU/bridge checks, without retaining/releasing
  // the same Kernel module once for every four-byte instruction.
  for(uint64_t page=at;page<uint64_t(at)+found->bytes;page=(page&~uint64_t(0xFFF))+0x1000){
   xex2_section_type type;
   if(calls.kernel()->GetUserModuleForAddress(uint32_t(page)).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(uint32_t(page),&type) || type!=XEX_SECTION_CODE)return false;
  }
  for(uint32_t n=0;n<found->bytes;n+=4)
   if(calls.kernel()->processor()->LookupModule(at+n)!=module->xex_module() ||
      module->xex_module()->TranslateCode(at+n)!=memory->TranslateVirtual<const uint8_t*>(at+n))return false;
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),found->bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)
   if(found->sha256[n*2]!=hex[sum[n]>>4] || found->sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 static constexpr identity_detail::NativeIdentityCodeExtent tables[]={
  {0x815F8BA8,256,"32a16c3960c589a3660475aac6f0eae6c7a770f073af5a04b16d912d6d1b41ef"},
  {0x815F8CA8,96,"e95a45d8ae4db6a8ab804f604c8e967158e1513d90da3cb9a595fd105196d18d"},
  {0x815F8D08,80,"f46eace64e7aa320c1ff489f5159ae819551c9dd75d12612bac9be5ba27756db"},
  {0x815F8D58,224,"8eceeec04134e3abc579c387802ad636f27a7b9a501acc16061d2b86d01be5d1"},
  {0x815F8E38,240,"f6d3716b2a24b411696e247df1ba2aa70464a5bf2620d4189e3f8a39c664b0e1"},
  {0x815F8F88,112,"1f1b58ff7f87578600f839c2ac3ed9fb33a3949f91d9a7df76bccb8b173b5b5e"},
 };
 for(const auto& item:tables){xex2_section_type type;
  if(calls.kernel()->GetUserModuleForAddress(item.address).get()!=module.get() ||
     !module->xex_module()->GetPageSectionType(item.address,&type) || type!=XEX_SECTION_READONLY_DATA ||
     memory->QueryRangeAccess(item.address,item.bytes)==memory::PageAccess::kNoAccess)return false;
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(item.address),item.bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)
   if(item.sha256[n*2]!=hex[sum[n]>>4] || item.sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 return true;
}
// This exact native service group remains fully checked at each owned
// binding. A load generation is lifetime, not an immutable-code receipt.
bool CheckIdentityServiceCode(GuestRuntimeCallExecutor& calls) {
 struct Extent {uint32_t at,bytes;const char* sha;};
 static constexpr Extent code[]={
  {0x81830878u,352u,"2d25796d52b308dec369b0f1e2de415d0688950364c1cfc3cbcd042118ab230e"},
  {0x8199C310u,720u,"0a5a1441e337decaedb5b5027fad8756dc69584981d6975c4e2782570c58a7c1"},
  {0x81671E60u,20u,"2d530349d8d9d0d733ac14a3eab9af19f9116e089c697a622866b839823ea138"},
  {0x818CF000u,404u,"e59205e53458a35e572223d2361b42e6d137b2660e0aa97fa67299e08d02b5f1"},
  {0x818CEFD8u,36u,"75f77da6a14dd00e987f254a52994a0d8715bcc3cab5587a04e16d7c1398cff0"},
  {0x818CEE08u,460u,"a5a9467159717f0d026333c40264e8b6730d8db29d454bac903c1112d124e916"},
  {0x818CE270u,2964u,"1d4a23253b2f6b421e8f73be0e858c1d11fbed8fa51d2804f5d25f26f5e1b4b4"},
  {0x818CD588u,252u,"c8980e538f08e68fb8a1202e4e3b447d01388d885abb38d83dc3e06ecc096361"},
  {0x818CDBD0u,1696u,"02d1bf246cd1c2b5055266fc1d6b97986786a411e80826b0885cbf523c28c1fc"},
  {0x818C9940u,176u,"68e5703b89a1913fbf42566406d64d9a5f5bae05ce45ef9c092f9a152f8942c3"},
  {0x816A2588u,364u,"cb8e83d7817263c223f0ac9719175540bbd78746a39ce1b5a38221550bf953d1"},
  {0x816E6AB0u,96u,"cd4506be3ef237ff9fa71650947f227043d3e76d06559fea0ed12f6693a78149"},
 };
 const auto module=calls.module();auto* kernel=calls.kernel();
 if(!kernel || !module || !module->xex_module() || !module->native_import_ready() ||
    module->load_generation()!=calls.load_generation())return false;
 auto read=global_critical_region::AcquireDirect();auto& memory=*kernel->memory();
 for(const auto& e:code){
  if(memory.QueryRangeAccess(e.at,e.bytes)==memory::PageAccess::kNoAccess)return false;
  for(uint32_t n=0;n<e.bytes;n+=4){xex2_section_type type;
   if(kernel->GetUserModuleForAddress(e.at+n).get()!=module.get() ||
      kernel->processor()->LookupModule(e.at+n)!=module->xex_module() ||
      !module->xex_module()->GetPageSectionType(e.at+n,&type) || type!=XEX_SECTION_CODE ||
      module->xex_module()->TranslateCode(e.at+n)!=memory.TranslateVirtual<const uint8_t*>(e.at+n))return false;
  }
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory.TranslateVirtual<const uint8_t*>(e.at),e.bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)if(e.sha[n*2]!=hex[sum[n]>>4] || e.sha[n*2+1]!=hex[sum[n]&15])return false;
 }
 // Exact constructed default FC instance and request virtual method. These
 // are public runtime bindings, never Account/payload or a generic heap grant.
 return Word(memory,0x81A94E18)==0x81AEE470u && Word(memory,0x81AEE470)==0x8162A6D8u &&
     Word(memory,0x8162A6D8+0x18)==0x818CDBD0u;
}

bool NativeMemoryKeyMatches(Memory& memory,uint32_t at,uint32_t address,
    uint32_t bytes,uint64_t version) noexcept {
 return identity_detail::NativeImageFullKeyMatches(memory,at,address,bytes,version);
}
template<typename Character,size_t Count>
bool ReadImageLabel(Memory& memory,uint32_t pointer,std::array<Character,Count>& output){
 std::array<Character,Count> owned{};
 for(size_t i=0;i<Count;++i){
  std::array<uint8_t,sizeof(Character)> bytes;
  if(uint64_t(pointer)+(i+1)*sizeof(Character)>0x100000000ull ||
     !internal::ReadNativeWaitSnapshot(memory,pointer+uint32_t(i*sizeof(Character)),bytes))return false;
  uint16_t value=bytes[0];
  if constexpr(sizeof(Character)==2)value=uint16_t((uint16_t(bytes[0])<<8)|bytes[1]);
  owned[i]=Character(value);if(!value){output=owned;return true;}
 }
 return false;
}
template<typename Character,size_t Count>
std::basic_string_view<Character> ImageLabel(const std::array<Character,Count>& label){
 return {label.data(),size_t(std::find(label.begin(),label.end(),Character{})-label.begin())};
}
bool NativeSectionUrlMatches(Memory& memory,uint32_t pointer,uint32_t module,
    std::u16string_view section,std::u16string_view entry){
 std::array<char16_t,128> expected{},actual{};size_t n=0;
 const auto put=[&](char16_t c){if(n>=expected.size()-1)return false;expected[n++]=c;return true;};
 for(auto c:std::u16string_view(u"section://"))if(!put(c))return false;
 bool started=false;constexpr char16_t hex[]=u"0123456789ABCDEF";
 for(int shift=28;shift>=0;shift-=4){const auto digit=(module>>shift)&15;
  if(digit || started || !shift){started=true;if(!put(hex[digit]))return false;}}
 if(!put(u','))return false;for(auto c:section)if(!put(c))return false;
 if(!put(u'#'))return false;for(auto c:entry)if(!put(c))return false;
 return ReadImageLabel(memory,pointer,actual) && ImageLabel(actual)==std::u16string_view(expected.data(),n);
}
const identity_detail::NativeImageCodeExtent* NativeImageCode(uint32_t at) noexcept {
 for(const auto& e:identity_detail::kNativeImageCode)if(e.address==at)return &e;
 return nullptr;
}
bool CheckContextReferenceCode(GuestRuntimeCallExecutor& calls) {
 const auto module=calls.module();auto* memory=calls.kernel()->memory();
 if(!module || !module->xex_module() || !module->native_import_ready() ||
    module->load_generation()!=calls.load_generation())return false;
 auto read=global_critical_region::AcquireDirect();
 for(uint32_t at:{0x816BA3C0u,0x816B7F30u,0x816B7490u,0x8172D3CCu,0x8172D41Cu}){
  const auto body=std::find_if(std::begin(identity_detail::kNativeIdentityCode),
      std::end(identity_detail::kNativeIdentityCode),[&](const auto& code){return code.address==at;});
  if(body==std::end(identity_detail::kNativeIdentityCode) ||
     memory->QueryRangeAccess(at,body->bytes)==memory::PageAccess::kNoAccess)return false;
  for(uint32_t n=0;n<body->bytes;n+=4){xex2_section_type type;
   if(calls.kernel()->processor()->LookupModule(at+n)!=module->xex_module() ||
      calls.kernel()->GetUserModuleForAddress(at+n).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(at+n,&type) || type!=XEX_SECTION_CODE ||
      module->xex_module()->TranslateCode(at+n)!=memory->TranslateVirtual<const uint8_t*>(at+n))return false;
  }
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),body->bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)
   if(body->sha256[n*2]!=hex[sum[n]>>4] || body->sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 return true;
}
bool CheckImageBody(GuestRuntimeCallExecutor& calls,uint32_t at) {
 const auto* body=NativeImageCode(at);const auto module=calls.module();
 auto* memory=calls.kernel()->memory();
 if(!body || !module || !module->xex_module() || !module->native_import_ready() ||
    !module->load_generation() || module->load_generation()!=calls.load_generation())return false;
 auto global=global_critical_region::AcquireDirect();
 if(memory->QueryRangeAccess(at,body->bytes)==memory::PageAccess::kNoAccess)return false;
 for(uint64_t page=at;page<uint64_t(at)+body->bytes;page=(page&~uint64_t(0xFFF))+0x1000){
  xex2_section_type type;
  if(calls.kernel()->GetUserModuleForAddress(uint32_t(page)).get()!=module.get() ||
     !module->xex_module()->GetPageSectionType(uint32_t(page),&type) || type!=XEX_SECTION_CODE)return false;
 }
 for(uint32_t n=0;n<body->bytes;n+=4)
  if(calls.kernel()->processor()->LookupModule(at+n)!=module->xex_module() ||
     module->xex_module()->TranslateCode(at+n)!=memory->TranslateVirtual<const uint8_t*>(at+n))return false;
 std::array<uint8_t,32> sum{};
 if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),body->bytes,sum.data(),0))return false;
 constexpr char hex[]="0123456789abcdef";
 for(size_t n=0;n<sum.size();++n)
  if(body->sha256[n*2]!=hex[sum[n]>>4] || body->sha256[n*2+1]!=hex[sum[n]&15])return false;
 return true;
}
// Actual entry authentication for bounded finite SYSTEM consumers.
// A code lease retains lifetime; these raw and translated-code checks retain
// the whole body and its real save/restore return ABI at the admission cut.
bool CheckFiniteSystemThreadCode(GuestRuntimeCallExecutor& calls,uint32_t function) {
 const auto* consumer=FindNativeFiniteSystemConsumer(function);
 if(!consumer)return false;
 const uint32_t group[]={consumer->entry,consumer->save,consumer->restore};
 const auto module=calls.module();auto* memory=calls.kernel()->memory();
 if(!module || !module->xex_module() || !module->native_import_ready() ||
    module->load_generation()!=calls.load_generation())return false;
 auto guard=global_critical_region::AcquireDirect();
 for(uint32_t at:group){
  const auto entry=std::find_if(std::begin(identity_detail::kNativeIdentityCode),
      std::end(identity_detail::kNativeIdentityCode),[&](const auto& code){return code.address==at;});
  if(entry==std::end(identity_detail::kNativeIdentityCode) ||
      memory->QueryRangeAccess(at,entry->bytes)==memory::PageAccess::kNoAccess)return false;
  for(uint64_t page=at;page<uint64_t(at)+entry->bytes;page=(page&~uint64_t(0xFFF))+0x1000){
   xex2_section_type type;
   if(calls.kernel()->GetUserModuleForAddress(uint32_t(page)).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(uint32_t(page),&type) || type!=XEX_SECTION_CODE)return false;
  }
  for(uint32_t offset=0;offset<entry->bytes;offset+=4)
   if(module->xex_module()->TranslateCode(at+offset)!=memory->TranslateVirtual<const uint8_t*>(at+offset))return false;
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),entry->bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)
   if(entry->sha256[n*2]!=hex[sum[n]>>4] || entry->sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 return true;
}
// The ref-only permission depends on these finite native bodies, including
// the exact save ABI and ordinary/Wait baseline. Reject bridge overlays too.
bool CheckTaskReferenceCode(GuestRuntimeCallExecutor& calls) {
 static constexpr uint32_t group[]={0x816B8098,0x816B7F30,0x816B7EF0,0x816B7B80,
   0x816B7490,0x816B7D98,0x816B7500,0x816E6AB0,0x816B8B08,0x816B8BA0,0x816B92C8,0x816B94A0,0x816BA058,
   0x8172D3CC,0x8172D41C};
 const auto module=calls.module();auto* memory=calls.kernel()->memory();
 if(!module || !module->xex_module() || !module->native_import_ready())return false;
 auto guard=global_critical_region::AcquireDirect();
 for(uint32_t at:group){
  const auto entry=std::find_if(std::begin(identity_detail::kNativeIdentityCode),
      std::end(identity_detail::kNativeIdentityCode),[&](const auto& code){return code.address==at;});
  if(entry==std::end(identity_detail::kNativeIdentityCode) ||
      memory->QueryRangeAccess(at,entry->bytes)==memory::PageAccess::kNoAccess)return false;
  // XEX ownership and section type are page facts under this SAME GCRC.
  // Preserve every original translated-word equality below, including an
  // interior guest-host bridge within an otherwise valid XEX code page.
  for(uint64_t page=at;page<uint64_t(at)+entry->bytes;page=(page&~uint64_t(0xFFF))+0x1000){
   xex2_section_type type;
   if(calls.kernel()->GetUserModuleForAddress(uint32_t(page)).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(uint32_t(page),&type) || type!=XEX_SECTION_CODE)return false;
  }
  for(uint32_t offset=0;offset<entry->bytes;offset+=4)
   if(module->xex_module()->TranslateCode(at+offset)!=memory->TranslateVirtual<const uint8_t*>(at+offset))return false;
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),entry->bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)
   if(entry->sha256[n*2]!=hex[sum[n]>>4] || entry->sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 return true;
}
// Actual-cut functional authentication; code leases retain lifetime only.
bool CheckProfileControlCode(GuestRuntimeCallExecutor& calls) {
 static constexpr uint32_t group[]={0x816DF990,0x8199B4F0,0x816712c8,0x817f2eb0,0x81671c18,0x81671c30,0x81671c38,0x81671c58,0x81671c90,0x81671c98,0x8199c720,0x816ba658,0x816ba660,0x816b7b80,0x816df980,0x8199e3e0,0x8199e148,0x8199c838,0x816ba440,0x816b9a18,0x816b9bb8,0x816b99c0,0x816b88d8,0x816b82b0,0x816b8320,0x816b8998,0x816b8a88,0x816b8ba0,0x816b8b08,0x816ba058,0x816b9f08,0x816b7490,0x816b7d98,0x816b7500,0x816e6ab0,0x816b8530,0x816b8208,0x816b8190,0x816b7600,0x816b92c8,0x816b94a0,0x8172D390,0x8172D3E0,0x816B77B0,0x816B79E8,0x816B7A28,0x816B8D28,0x816B8FC8,0x816B9018,0x816B91A8,0x816B9218,0x816B9258,0x816BA8D0};
 const auto module=calls.module();auto* memory=calls.kernel()->memory();
 if(!module)return false;
 auto guard=global_critical_region::AcquireDirect();
 for(uint32_t at:group){
  const auto entry=std::find_if(std::begin(identity_detail::kNativeIdentityCode),std::end(identity_detail::kNativeIdentityCode),[&](const auto& code){return code.address==at;});
  if(entry==std::end(identity_detail::kNativeIdentityCode) || memory->QueryRangeAccess(at,entry->bytes)==memory::PageAccess::kNoAccess)return false;
  for(uint32_t offset=0;offset<entry->bytes;offset+=4){
   xex2_section_type type;
   if(calls.kernel()->GetUserModuleForAddress(at+offset).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(at+offset,&type) || type!=XEX_SECTION_CODE ||
      module->xex_module()->TranslateCode(at+offset)!=memory->TranslateVirtual<const uint8_t*>(at+offset))return false;
  }
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),entry->bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)if(entry->sha256[n*2]!=hex[sum[n]>>4] || entry->sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 static constexpr identity_detail::NativeIdentityCodeExtent data[]={
  {0x815F2CA8,64,"9a17da8c418cbc014dfbe857fded71d427efd567969a3c9e274126d0834e17ce"},
  {0x815F8BA8,656,"1bcd1649e45d1214bf3b9b652ecd1ceca8ae9d7ccdbaef8bc1c0ab6111dad8fd"},
  {0x815F8E38,240,"f6d3716b2a24b411696e247df1ba2aa70464a5bf2620d4189e3f8a39c664b0e1"},
 };
 for(const auto& entry:data){
  xex2_section_type type;
  if(calls.kernel()->GetUserModuleForAddress(entry.address).get()!=module.get() ||
     !module->xex_module()->GetPageSectionType(entry.address,&type) || type!=XEX_SECTION_READONLY_DATA ||
     memory->QueryRangeAccess(entry.address,entry.bytes)==memory::PageAccess::kNoAccess)return false;
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(entry.address),entry.bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)if(entry.sha256[n*2]!=hex[sum[n]>>4] || entry.sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 return true;
}
// Optional-role FUNCTIONAL authentication precedes typed factory and stop.
bool CheckPeriodicCode(GuestRuntimeCallExecutor& calls) {
 if(!CheckTaskReferenceCode(calls))return false;
 static constexpr uint32_t group[]={0x8190A358,0x81909CF8,0x81907DA0,0x819068D0,
   0x81907C98,0x81907B18,0x8190AAC0,0x8190B528,0x8190B530,0x81909B68,
   0x81913EF8,0x819133B0,0x8190E510,0x8190E310,0x81913F18,0x819114B8,0x8190D248,
   0x816BA658,0x816BA660,0x816B9F08,0x816B8A88,0x816B8F50,0x816B7600,
   0x816BA440,0x816B9A18,0x816B9BB8,0x816B99C0,0x816B9958,0x816B9890,
   0x816B8D98};
 const auto module=calls.module();auto* memory=calls.kernel()->memory();
 auto guard=global_critical_region::AcquireDirect();
 for(uint32_t at:group){
  const auto entry=std::find_if(std::begin(identity_detail::kNativeIdentityCode),std::end(identity_detail::kNativeIdentityCode),[&](const auto& code){return code.address==at;});
  if(entry==std::end(identity_detail::kNativeIdentityCode) || memory->QueryRangeAccess(at,entry->bytes)==memory::PageAccess::kNoAccess)return false;
  for(uint32_t offset=0;offset<entry->bytes;offset+=4){
   xex2_section_type type;
   if(calls.kernel()->GetUserModuleForAddress(at+offset).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(at+offset,&type) || type!=XEX_SECTION_CODE ||
      module->xex_module()->TranslateCode(at+offset)!=memory->TranslateVirtual<const uint8_t*>(at+offset))return false;
  }
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),entry->bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)if(entry->sha256[n*2]!=hex[sum[n]>>4] || entry->sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 static constexpr identity_detail::NativeIdentityCodeExtent data[]={
  {0x815F8BA8,656,"1bcd1649e45d1214bf3b9b652ecd1ceca8ae9d7ccdbaef8bc1c0ab6111dad8fd"},
  {0x8162F258,44,"5030476dd87105eb88ec480de45348067a8d7aa6374357889b1b71235ad54950"},
  {0x815F8F88,112,"1f1b58ff7f87578600f839c2ac3ed9fb33a3949f91d9a7df76bccb8b173b5b5e"},
 };
 for(const auto& item:data){
  xex2_section_type type;
  if(calls.kernel()->GetUserModuleForAddress(item.address).get()!=module.get() ||
     calls.kernel()->GetUserModuleForAddress(item.address+item.bytes-1).get()!=module.get() ||
     !module->xex_module()->GetPageSectionType(item.address,&type) || type!=XEX_SECTION_READONLY_DATA ||
     memory->QueryRangeAccess(item.address,item.bytes)==memory::PageAccess::kNoAccess)return false;
  std::array<uint8_t,32> sum{};
  if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(item.address),item.bytes,sum.data(),0))return false;
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<sum.size();++n)if(item.sha256[n*2]!=hex[sum[n]>>4] || item.sha256[n*2+1]!=hex[sum[n]&15])return false;
 }
 return true;
}
// Optional diagnostic code admission only. Failure disables observations,
// never changes the existing native/profile admission result. Other observed
// bodies are already authenticated by CheckIdentityCode and the same lease.
bool CheckTaskDiagnosticCode(GuestRuntimeCallExecutor& calls) noexcept try {
 auto* memory=calls.kernel()->memory();const auto root=calls.module();
 auto read=global_critical_region::AcquireDirect();
 constexpr uint32_t at=0x816B9890,bytes=200;
 if(!root || memory->QueryRangeAccess(at,bytes)==memory::PageAccess::kNoAccess ||
    calls.kernel()->GetUserModuleForAddress(at).get()!=root.get())return false;
 std::array<uint8_t,32> digest{};
 if(mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(at),bytes,digest.data(),0))return false;
 constexpr char expected[]="968564812af2f105bc62d7f8efa2615fd3038ab667034dd28e8bfb2e1c0b6a07";
 constexpr char hex[]="0123456789abcdef";
 for(size_t i=0;i<digest.size();++i)
  if(expected[i*2]!=hex[digest[i]>>4] || expected[i*2+1]!=hex[digest[i]&15])return false;
 return true;
}catch(...){return false;}
}
struct NativeIdentityTileWork::Data {
 uint64_t source=0,provider_generation=0,version=0;
 uint32_t provider=0,output=0,size_address=0,capacity=0,user=0,type=0;
 std::atomic<uint32_t> status{0x80004005u},required{0};
 std::atomic<bool> claimed{false},published{false},written{false};
 std::atomic<uint64_t> image_store_allocation{0},image_store_publication{0};
 std::atomic<uint64_t> image_first_allocation{0},image_first_publication{0};
};
NativeIdentityTileWork::NativeIdentityTileWork(std::shared_ptr<Data> data):data_(std::move(data)){}
void NativeIdentityTileWork::RecordPublication(uint32_t status,uint32_t required,bool written) noexcept {
 bool no=false;
 if(!data_ || !data_->claimed.compare_exchange_strong(no,true,std::memory_order_acq_rel))return;
 data_->status.store(status,std::memory_order_relaxed);
 data_->required.store(required,std::memory_order_relaxed);
 data_->written.store(written,std::memory_order_relaxed);
 if(!status && written){
  data_->image_first_allocation.store(data_->image_store_allocation.load(std::memory_order_acquire),std::memory_order_relaxed);
  data_->image_first_publication.store(data_->image_store_publication.load(std::memory_order_acquire),std::memory_order_relaxed);
 }
 data_->published.store(true,std::memory_order_release);
}
// A bound cell never owns/drops resources from a getter hook. The actual
// retained acquisition use owns its entire synchronous PPC/C++ tail.
struct NativeProfileIdentityOwner::TaskReference::Data {
 std::weak_ptr<State> owner;
 std::weak_ptr<void> producer;
 void* producer_object=nullptr;
 std::shared_ptr<KernelState::UserModuleExecutionLease> code;
 NativeImportDomain::Operation domain;
 object_ref<XThread> thread;
 KernelState* kernel=nullptr;
 cpu::ppc::PPCContext* context=nullptr;
 cpu::ThreadState* cpu=nullptr;
 cpu::Processor* processor=nullptr;
 Tasks::ReferenceReceipt receipt;
 std::atomic<bool> retired{false},bound{false},resources_retired{false},reaping{false};
 bool MatchesProducer(const std::shared_ptr<void>& actual) const noexcept {
  return actual && actual.get()==producer_object &&
      !producer.owner_before(actual) && !actual.owner_before(producer);
 }
 std::optional<Frame> Actual(cpu::ppc::PPCContext& c,uint32_t at) const noexcept {
  if(retired.load(std::memory_order_acquire) || &c!=context || c.kernel_state!=kernel ||
      c.thread_state!=cpu || c.processor!=processor ||
      !DwordIs(c.r[1],uint32_t(c.r[1])) || !DwordIs(c.lr,uint32_t(c.lr)) ||
      c.r[13]!=receipt.callback.pcr)return {};
  return Frame{receipt.callback.root,receipt.callback.thread,receipt.callback.pcr,
      uint32_t(c.r[1]),uint32_t(c.lr),at};
 }
 bool Enter(cpu::ppc::PPCContext& c,uint32_t at) noexcept {
  const auto f=Actual(c,at);
  if(!f){receipt.Fail();return false;}
  return at==0x816B8098?receipt.EnterAcquire(*f):receipt.EnterInner(*f);
 }
 void Return(cpu::ppc::PPCContext& c,uint32_t at) noexcept {
  const auto f=Actual(c,at);
  if(!f){receipt.Fail();return;}
  if(at==0x816B8098)receipt.ReturnAcquire(*f,c.r[3]);else receipt.ReturnInner(*f,c.r[3]);
 }
 void ReleaseResourcesOnHost() noexcept {
  // Only actual safe host maintenance or actual stopped retirement calls this.
  domain={};code.reset();thread.reset();producer.reset();
  context=nullptr;cpu=nullptr;processor=nullptr;kernel=nullptr;producer_object=nullptr;
  resources_retired.store(true,std::memory_order_release);
 }
};
namespace {
// Diagnostic classification only. The low four latest_kind bits retain the
// existing Work kind; bits4..11 carry the source table index+1. Bits16..31
// are reserved for one incompatible reentrant Work category/role pair.
// No guest read, fresh code-authentication claim or read/admission authority.
constexpr uint32_t OriginalLogonFunctionRole(uint32_t function) noexcept {
 for(size_t i=0;i<std::size(identity_detail::kNativeIdentityCode);++i)
  if(identity_detail::kNativeIdentityCode[i].address==function)return uint32_t(i+1);
 return 0; // Unknown/not in the source-owned extent table.
}
static_assert(std::size(identity_detail::kNativeIdentityCode)<=0xFFu);
static_assert(OriginalLogonFunctionRole(0)==0);
using OriginalLogonDiagnostic=xam::ProfileManager::NativeOriginalLogonDiagnostic;
class OriginalLogonCut final {
 public:
  explicit OriginalLogonCut(const xam::NativeOriginalLogonObservation* value)
      :observation(value?*value:xam::NativeOriginalLogonObservation{}){}
  template<class T> bool Refused(uint32_t reason,T&& condition,uint32_t line) noexcept {
    const bool refused=bool(condition);
    if(refused && !value.reason){value.reason=reason;value.line=line;}
    return refused;
  }
  OriginalLogonCut& Error(const std::error_code& error) noexcept {
    value.error_available=1;value.error_value=uint32_t(error.value());
    value.error_category=error.category()==std::generic_category()?1:
        error.category()==std::system_category()?2:0;return *this;
  }
  NativeProfileIdentityOwner::Result Finish(NativeProfileIdentityOwner::Result result) noexcept {
    value.stage=OriginalLogonDiagnostic::kAdmissionReturn;
    value.disposition_available=1;value.disposition=uint32_t(result.disposition);
    value.result_available=1;value.result=result.value;
    xam::ProfileManager::RecordNativeOriginalLogon(observation,value);return result;
  }
  xam::NativeOriginalLogonObservation observation;
  OriginalLogonDiagnostic value;
};
void RecordOriginalLogonRosterResult(const xam::NativeOriginalLogonObservation& observation,
    uint32_t stage,const xam::ProfileRosterResult& result) noexcept {
  OriginalLogonDiagnostic d;d.stage=stage;d.phase_available=1;d.phase=uint32_t(result.phase);
  d.error_available=1;d.error_value=uint32_t(result.error.value());
  d.error_category=result.error.category()==std::generic_category()?1:
      result.error.category()==std::system_category()?2:0;
  d.maps_committed=result.maps_committed;d.writers_available=result.writers_available;
  xam::ProfileManager::RecordNativeOriginalLogon(observation,d);
}
}

struct NativeProfileIdentityOwner::State : std::enable_shared_from_this<State> {
 using HookUse=identity_detail::NativeIdentityHookRundown::Use;
 struct CodeOwner {
  object_ref<UserModule> module;
  std::shared_ptr<KernelState::UserModuleExecutionLease> lease;
  uint64_t load_generation=0;
 };
 // Parallel resource holders for the SAME scalar Task ledger. These are
 // execution-only module lifetimes, never a second Task/source registry.
 struct ExecutionTarget {uint32_t callback=0;CodeOwner code;};
 struct ContextReference {
  Tasks::ContextReferenceReceipt receipt;
  object_ref<XThread> thread;
  cpu::ThreadState* cpu=nullptr;
  cpu::ppc::PPCContext* context=nullptr;
  std::shared_ptr<KernelState::UserModuleExecutionLease> code;
  NativeImportDomain::Operation domain;
  std::array<std::shared_ptr<ExecutionTarget>,2> targets;
 };
 struct ExecutionLifetime {
  Tasks::Birth birth{};std::vector<std::shared_ptr<ExecutionTarget>> targets;
  bool uncertain=false,native_freed=false;
 };
 struct EventLifetime {
  std::shared_ptr<KernelState::UserModuleExecutionLease> code;
  NativeImportDomain::Operation domain;
  object_ref<XThread> thread;
  std::shared_ptr<xam::ProfileRosterIntent> intent;
 };
 struct Commit {
  enum class Phase:uint8_t {kArmed,kPublishing,kPublished,kUncertain};
  static_assert(std::atomic<Phase>::is_always_lock_free);
  std::atomic<Phase> phase{Phase::kArmed};
  std::atomic<bool> publisher_entered{false},publisher_returned{false};
  xam::ProfileRosterPublicationOutcome outcome=xam::ProfileRosterPublicationOutcome::kRequested;
  uint32_t publisher_lr=0,publisher_title_context=0;
  const uint32_t *canceled_title_pointer=nullptr,*default_provider=nullptr,*default_vtable=nullptr,*default_accessor=nullptr;
  // Strong holders are prepared/retained before native entry and never
  // assigned, reset, copied or destroyed in either publisher method below.
  std::shared_ptr<xam::ProfileRosterIntent> intent;
  std::shared_ptr<xam::ProfileRosterPublicationCell> publication;
  State* owner=nullptr;
  Wake* wake=nullptr;
  cpu::ppc::PPCContext* context=nullptr;
  cpu::ThreadState* thread_state=nullptr;
  uint64_t source=0,claimed_nonce=0,worker_birth=0,whole_work=0;
  uint32_t pcr=0,publisher_sp=0,input=0,original_flags=0,title_context=0;
  uint32_t* request_flags=nullptr;
  uint32_t* native_title_context=nullptr;
  // Optional advisory RAM bindings prepared at the existing C838 entry.
  // Actual Work retains the same thread/Task through this callback. These
  // pointers confer no native read, source or publication authority.
  uint32_t diagnostic_thread=0,diagnostic_task=0;
  const uint32_t *diagnostic_status=nullptr,*diagnostic_index=nullptr;
  std::array<uint64_t,4> desired{};
  std::array<uint32_t*,4> input_flags{},native_flags{};
  std::array<uint64_t*,4> input_xuids{},native_xuids{};
 };
 using ReadPool=std::array<std::unique_ptr<xam::ProfileRosterNativeReadScope>,64>;
 struct TaskOperands {
  uint64_t available=0,source=0,work=0,birth=0,schedule=0,allocated_birth=0;
  uint32_t origin=0,thread=0,pcr=0,process=0,native_tls_task=0,consumer_native_tls_task=0,site=0;
  uint64_t entry_sp=0,entry_lr=0,body_sp=0,sp=0,lr=0;
  uint64_t schedule_sp=0,schedule_lr=0,callback=0,argument=0,options=0,output=0;
  uint64_t provider_object=0,provider_function=0,provider_ctr=0,provider_return=0;
  uint64_t producer_object=0,producer_word=0,producer_options=0;
  uint64_t allocation_options=0,allocation_task=0,supplied_flags=0,supplied_object=0;
  uint64_t native_task=0,native_flags=0,raw_object=0,masked_object=0,operand=0;
  uint64_t native_process_first=0,native_process_second=0,issues=0;
 };
 struct TaskOperandCell {
  std::atomic<uint32_t> state{0};TaskOperands payload;
  void Record(const TaskOperands& value) noexcept {
   uint32_t empty=0;if(!state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
   payload=value;state.store(2,std::memory_order_release);
  }
 } first_task_operands,first_null_task_operands;
 struct TaskDiagnostic {
  State* owner=nullptr;cpu::ppc::PPCContext* context=nullptr;
  cpu::ThreadState* cpu=nullptr;KernelState* kernel=nullptr;
  TaskDiagnostic* previous=nullptr;TaskOperands value;bool bound=false;
 };
 // Pure scalar metadata only; these cells have no Work/domain/code/thread
 // owners and never recycle during this Kernel lifetime. A missed return
 // cannot dangle an earlier cell even if its genuine Work is host-reaped.
 std::array<TaskDiagnostic,256> task_diagnostic_cells{};
 uint32_t task_diagnostic_used=0;
 bool task_diagnostics_enabled=false;
 std::atomic<uint64_t> task_diagnostic_unavailable{0};
 std::atomic<uint32_t> task_diagnostic_bindings{0};
 TaskDiagnostic* OwnTaskDiagnostic(void* raw) noexcept {
  const auto at=reinterpret_cast<uintptr_t>(raw),begin=reinterpret_cast<uintptr_t>(task_diagnostic_cells.data());
  if(at<begin || at-begin>=sizeof(task_diagnostic_cells) || (at-begin)%sizeof(TaskDiagnostic))return nullptr;
  return &task_diagnostic_cells[(at-begin)/sizeof(TaskDiagnostic)];
 }
 // First exact protected options claim/admission/return, independently
 // captured. Availability distinguishes missing joins/words from actual zero.
 // These three snapshots need explicit frame/generation correlation; they do
 // not assert one request, callback success or an HRESULT from void Modify.
 struct ModifyOptionsPayload {
  uint32_t available=0,line=0,thread=0,process=0,stage=0,evaluated=0,predicate_true=0;
  uint64_t task=0,mask=0,options=0,sp=0,lr=0,pcr=0,origin=0,generation=0;
  uint32_t node_flags=0,old_flags=0,old_data=0,new_flags=0,new_data=0;
  uint64_t raw_return_residue=0;
  bool execution_only=false,profile=false,protected_scope=false,abi_eligible=false,own_return_matched=false;
 };
 struct ModifyOptionsCell {
  std::atomic<uint32_t> state{0};ModifyOptionsPayload payload;
  void Capture(const ModifyOptionsPayload& value) noexcept {
   uint32_t empty=0;if(!state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
   payload=value;state.store(2,std::memory_order_release);
  }
 } first_modify_options_claim,first_modify_options_admission,first_modify_options_return;
 enum class PeriodicRole:uint8_t {kNone,kFactory,kStop,kDispatcher,kBoundary,kCancel,kWait,kTransport,kOwnerClose};
 struct CountedAllocation;
 struct ServiceCell;
 struct Work : Tasks::ExecutionObservation {
  // Operation source is lifetime-only. Generic authority selectors omit this row.
  bool service_progress=false,service_pump_candidate=false;
  uint32_t service_pump_object=0;
  std::shared_ptr<IdentityServiceOrigin> service_origin;
  std::shared_ptr<ServiceCell> service_cell;
  std::optional<Tasks::IdentityServiceRequest::Tick> service_tick;
  TaskDiagnostic* task_diagnostic=nullptr;
  uint64_t source=kBootUnjoinedSource;
  Tasks::Birth birth{};
  object_ref<XThread> thread;
  NativeImportDomain::Operation domain;
  std::unique_ptr<xam::ProfileRosterNativeReadScope> read;
  ReadPool* read_pool=nullptr;
  std::shared_ptr<Commit> commit;
  std::shared_ptr<EventLifetime> event_lifetime;
  uint64_t schedule=0,detached_allocation=0;
  // Passive producer metadata is excluded from ambient work/source lookup.
  // Each child retains its exact actual parent until its own normal return.
  std::optional<Tasks::CommonStartProducer> creation;
  std::shared_ptr<Work> creation_parent;
  bool creation_only=false,execution_options=false,profile_producer=false;
  std::shared_ptr<Work> initializer_completion_owner;
  bool execution_control=false,execution_code_proven=false;
  bool execution_title=false;
  std::optional<NativeXmsgProvenance::TitleTaskProducerFact> execution_title_producer;
  bool execution_read_fallback=false;
  uint64_t execution_dispatch=0;
  Tasks::ExecutionReadCell execution_read;
  std::shared_ptr<Work> execution_read_owner,execution_wait_parent,execution_producer;
  std::shared_ptr<ExecutionLifetime> execution_lifetime;
  std::shared_ptr<ExecutionTarget> execution_target;

  std::optional<Tasks::ProfileControl> profile_control;
  std::shared_ptr<Work> control_creator,control_parent;
  uint64_t periodic_factory=0;PeriodicRole periodic_role=PeriodicRole::kNone;
  std::optional<Tasks::PeriodicFrameContext> periodic_context;
  bool periodic_body_admitted=false;
  std::shared_ptr<TaskReference::Data> reference_release;
  bool reference_only=false;
  uint32_t first=0,second=0,output=0;
  Tasks::Birth external_birth{};
  std::optional<Wake::Wake> wake;
  std::optional<Wake::Work> wake_work;
  std::shared_ptr<Work> tail_reschedule,tail_wake;
  std::shared_ptr<const Listeners::Frame> listener_frame;
  Listeners::FastCell* listener_cell=nullptr;
  std::optional<Events::Listener> listener_registration;
  std::optional<Listeners::OuterArguments> listener_arguments;
  std::shared_ptr<CountedAllocation> listener_allocation;
  bool listener_reference_started=false,listener_unregistered_destruction=false;
  std::optional<Events::Work> listener_work;
  std::shared_ptr<const NativeIdentityEventWorkSource> listener_source;
  std::shared_ptr<Work> listener_parent;
  std::optional<Listeners::Dependencies> listener_dependencies;
  bool owns_task_work=false,owns_persistent_work=false,host_body=false,registered_executor=false;
  bool system_thread_frame=false; // finite real SYSTEM XThread invocation, never a Task birth
  bool system_discarded_identity_hint=false; // exact member's A branch reads no profile/payload
  // Observed ambient execution only. Periodic/unobserved Tasks are never
  // relabeled as a borrow; SYS0 event keys still carry zero Task/birth fields.
  uint32_t system_frame_ambient_task=0;
  Tasks::Birth system_frame_ambient_birth{};
  bool system_frame_ambient_periodic=false;
  bool physical_work=false,prebody_parked=false,transport_unclaimed=false,transport_only=false,identity_continuation=false,task_release=false,foreign_task_cleanup=false,foreign_avatar_task=false;
  uint64_t release_generation=0,release_receipt=0;
  CodeOwner caller_code;
  std::shared_ptr<const NativeAvatarService::CleanupTaskCreationProof> avatar_allocation;
  uint64_t provider_generation=0;
  std::optional<identity_detail::NativeImageInput> image_key_candidate,image_default_result;
  uint64_t image_uri_generation=0;
  struct ImageQueryNoReadEntry {uint64_t uri_generation=0,version=0;};
  // Presence records an actual pending==0 entry sample and an intact prior
  // no-image-history chain, before this same original Query body executes.
  std::optional<ImageQueryNoReadEntry> image_no_read_entry;
  uint32_t image_event=0,image_modified=0;

  std::shared_ptr<NativeIdentityTileWork::Data> tile;
  bool render_iteration=false;
  std::array<std::optional<identity_detail::NativeImageDraw>,64> image_draws;
  bool image_draws_unavailable=false;
  std::shared_ptr<const gpu::NativeFramePresentationGuard> presentation_guard;
  std::optional<Tasks::CompletionRoute> completion_route; // exact shared-IO entry routing only
  bool uncertain=false;
 };
 struct InitialEmptyBaseline {
  explicit InitialEmptyBaseline(NativeInitialEmptyProfileWitness&& value):witness(std::move(value)){}
  NativeInitialEmptyProfileWitness witness;
  xam::ProfileRosterExpected expected;
  std::shared_ptr<xam::ProfileRosterNativeReadTicket> ticket;
  uint64_t operation_sequence=0;
 };
 std::shared_ptr<InitialEmptyBaseline> initial_empty_baseline;
 bool initial_empty_capture_seen=false,initial_empty_consumed=false;
 bool InitialEmptyWindow() const {
  return profile_enabled && !aligned && !boot_alignment_claimed && !bootstrap_cleanup && !bootstrap_fatal &&
      !job && !stopping.load() && !retired.load() && !uncertain.load() &&
      !tasks.lost() && (!wake || !wake->lost()) && (!listeners || !listeners->lost()) &&
      current_read_source==kInitialReadSource && !published_source.load(std::memory_order_acquire) &&
      render_phase==RenderPhase::kOpen && current_roster && current_ticket &&
      !public_calls.load() && !external_public_calls.load() && !identity_executor_frames.load() &&
      std::none_of(expected.begin(),expected.end(),[](const auto& e){return e && e->identity_continuation;});
 }
 bool ConsumeInitialEmptyBaseline(XThread& bootstrap) noexcept;
 struct WorkBlocker {
  uint32_t available=0,reason=0,state=0,first_argument=0;uint64_t serial=0,source=0;
  Frame frame;Tasks::Birth birth{};bool closed=false;
 };
 // A worker stopped at its genuine native callback entry has no data/read or
 // callback-completion authority yet. Keep its actual execution provenance
 // separately from active whole Work, so it cannot block the OLD-frame cut.
 struct PrebodyIdentityWorker {
  Frame frame;Tasks::Birth birth;
  object_ref<XThread> thread;
  std::shared_ptr<KernelState::UserModuleExecutionLease> code;
  NativeImportDomain::Operation domain;
  uint32_t argument=0;
  bool admitted=false,refused=false;
 };
 struct Job {
  std::chrono::steady_clock::time_point preparation_deadline{};
  uint64_t serial=0,source=0;
  std::optional<NativeProfileIdentityRequest> original;
  std::shared_ptr<xam::ProfileRosterIntent> intent;
  std::shared_ptr<xam::ProfileRosterPublicationCell> publication;
  std::shared_ptr<xam::ProfileRosterNativeReadTicket> old_ticket,writer_ticket;
  // Scope allocation precedes native Schedule. Entry/Leave bind only actual
  // thread/Task/work metadata; raw ThreadState pointers outlive their work.
  std::array<std::unique_ptr<xam::ProfileRosterNativeReadScope>,64> old_reads,writer_reads;
  object_ref<XThread> original_caller;
  std::vector<CodeOwner> caller_code;
  std::optional<CodeOwner> apc_code;
  NativeImportDomain::Operation domain;
  std::shared_ptr<const NativeServiceBackingLease> caller_backing;
  std::shared_ptr<const NativeServiceBackingLease> overlap_backing;
  std::optional<Frame> completion_frame;
  Tasks::Birth completion_birth{};
  std::shared_ptr<Work> initializer_completion_owner;
  bool initializer_scheduled=false;
  uint32_t completion_context=0,completion_error=0,completion_extended=0,completion_length=0;
  std::atomic<bool> caller_published{false},completion_returned{false};
  xam::NativeOriginalLogonObservation original_logon_observation;
  uint32_t input=0,overlap=0,flags=0;
  uint32_t value=kAbort;
  std::atomic<bool> bootstrap_alignment{false},external_closed{false},native_entered{false};
  std::atomic<bool> native_returned{false},execution_uncertain{false},publisher_observed{false};
  // Original initializer and dataful worker are different native phases.
  // The reentrant async initializer may return under its retained OLD read;
  // no worker reads/publishes until the existing full old-source/paint cut.
  std::atomic<bool> initializer_ready{false},data_worker_ready{false};
  std::atomic<bool> entering{false},canceled{false},transport_running{false};
  std::shared_ptr<Work> original_render_frame;
  std::array<std::shared_ptr<PrebodyIdentityWorker>,2> prebody_workers;
  std::atomic<bool> owned_input{false},done{false};
  std::atomic<bool> old_paint_drained{false},new_paint_drained{false},event_producer_finished{false};
  uint32_t preentry_attempt=0; // State-locked, saturating diagnostic generation
  uint32_t preentry_pending=0;bool preentry_pending_available=false,preentry_observation_exhausted=false;
  PaintObservation preentry_observation; // actual synchronous Bootstrap writer only
  std::array<WorkBlocker,2> preentry_work_blockers{};
  std::array<Tasks::SourceBlocker,2> preentry_source_blockers{};
  std::atomic<bool> consumer_joined{false},old_event_finished{false},render_refresh_observed{false};
  std::atomic<bool> want_final_barrier{false},execution_quiescent{false};
  std::shared_ptr<const NativeIdentityEventSource> old_event_source;
  std::optional<Events::Origin> old_event_origin;
  // The exact matched C838 own-return is a host commitment fact, not the
  // listener/provider/frame/GPU join and not public224's input-copy return.
  bool protocol_returned=false;
  uint64_t protocol_work=0;
  Frame protocol_frame{};
  Tasks::Birth protocol_birth{};
  uint32_t protocol_value=0;
  std::shared_ptr<const xam::ProfileRosterNativeCompletion> protocol,joined;
  std::shared_ptr<IdentityPixelGuard> old_pixels,hidden_pixels,new_pixels;
  std::shared_ptr<const NativeIdentityEventSource> event_source;
  std::optional<Events::Origin> event_origin;
 };
 // Registration is made by the real executor/continuation before Call; the
 // actual padded SP/LR are bound by the first native entry, never guessed.
 struct Expected {
  uint64_t serial=0,source=0;
  object_ref<XThread> thread;
  std::array<uint32_t,4> args{};
  uint32_t count=0,function=0;
  std::optional<Frame> actual;
  bool entered=false,returned=false,call_returned=false,healthy=false;
  bool transport_only=false,identity_continuation=false;
  std::optional<Tasks::CloseReceipt> task_release;
  std::shared_ptr<TaskReference::Data> reference_release;
  bool reference_only=false;
  uint32_t reference_lookup=0,reference_parent=0;
  std::shared_ptr<ContextReference> context_reference;
  Frame context_reference_call;
 };
 struct Provider {
  uint64_t generation=0,completed_source=0,completed_version=0;
  uint32_t object=0,buffer=0,capacity=0,type=0,listener=0;
  bool constructing=true,initialized=false,buffer_revoked=false,revoked=false,destroyed=false;
  bool completed_empty=false;uint32_t completed_status=0;
  std::shared_ptr<NativeIdentityTileWork::Data> last_output;
  uint64_t image_uri_generation=0;
  identity_detail::NativeEncodedImageEpoch image_epoch;
  std::optional<identity_detail::EncodedImageSource> image_output;
  std::optional<identity_detail::NativeImageInput> image_key;
  uint64_t image_key_generation=0;
  struct NoReadQuery {
   Frame frame;Tasks::Birth birth;
   uint64_t work=0,source=0,provider_generation=0,uri_generation=0,version=0;
  };
  std::optional<NoReadQuery> no_read_query;
 };
 struct CountedAllocation {
  uint64_t generation=0;
  uint32_t body=0,allocation=0;
  Frame producer;
  object_ref<XThread> creator;
  NativeImportDomain::Operation domain;
  bool allocated=false,failed=false,delete_entered=false,delete_returned=false;
  bool listener_registration_entered=false,listener_registered=false;
  bool free_entered=false,free_returned=false;
 };
 explicit State(Config c):config(std::move(c)),kernel(config.catalog->calls()->kernel()),
   root{config.allocation->owner().kernel_epoch,config.domain->id(),config.domain->root().generation},images(root),tasks(root){}
 Config config;
 const bool profile_enabled=bool(config.factory_binding);
 std::shared_ptr<NativeIdentityTileObserverSlot> image_slot;
 using ImageObject=identity_detail::NativeImageJournal::Object;
 using ImageInput=identity_detail::NativeImageInput;
 using ImageTransfer=identity_detail::NativeDecodedImageTransfer;
 struct ImageCall {
  Frame frame;uint32_t body_sp=0;uintptr_t context=0,cpu=0;
  uint64_t serial=0,parent=0,render=0,draw_selection=0;
  std::array<uint64_t,8> args{};
  std::optional<ImageObject> object,other,dc;
  std::optional<identity_detail::NativeImageJournal::Selection> selection;
  std::optional<ImageInput> input;
  std::optional<ImageTransfer> copied;
  std::optional<identity_detail::NativeImageJournal::Draw> emitted_draw;
  std::optional<identity_detail::EncodedImageSource> borrowed;
  uint32_t native_texture=0,device=0,sampler=0,slot_offset=0,tail=0,prior_texture=0,prior_cursor=0;
  uint32_t copied_bytes=0,copy_calls=0,copy_returns=0;
  uint32_t packet_address=0,packet_header=0,packet_viz=0,packet_initiator=0,packet_base=0,packet_size=0;
  std::array<uint32_t,6> texture_fetch{};
  bool packet_returned=false;
  uint32_t source_pointer=0,copy_destination=0,copy_length=0;
  uint32_t key_pointer=0,png_context=0,read_state=0,index_destination=0,stride=0;
  uint32_t address_output=0,size_output=0,module=0;
  uint64_t key_provider=0,key_generation=0,uri_generation=0;
  bool uncertain=false,error=false,copy_entered=false,copy_returned=false;
  bool native_call_entered=false,native_call_returned=false;
  bool native_store_entered=false,draw_store_entered=false;
  bool default_format=false,section_called=false,section_returned=false;
  std::array<char,32> section_name{};
  std::array<char16_t,32> section_label{},entry_label{};
  uint64_t initializer=0,resource_open=0,module_generation=0,default_source=0,default_birth=0;
  uint32_t default_index=UINT32_MAX,row_output=0,stream=0;
  uint32_t section_address=0,section_size=0,section_status=0x80004005,open_status=0x80004005;
 };
 struct ImageString {
  uint64_t generation=0;uint32_t pointer=0,field=0;
  ImageInput input;bool live=false;
 };
 struct ImageDefaultRow {
  uint32_t address=0,size=0,module=0,section=0,section_size=0;
  uint64_t module_generation=0;bool observed=false;
 };
 std::array<std::optional<ImageCall>,256> image_calls;
 std::array<ImageString,256> image_strings;
 std::array<std::optional<identity_detail::NativeDefaultImageRow>,3> image_defaults;
 uint64_t next_image_call=0,next_image_string=0;
 ImageCall* ImageBySerial(uint64_t serial){
  for(auto& c:image_calls)if(c && c->serial==serial)return &*c;return nullptr;
 }
 ImageCall* ImageAt(const Frame& frame,uintptr_t context,bool body){
  ImageCall* found=nullptr;
  for(auto& c:image_calls)if(c && c->frame.root==frame.root &&
      c->frame.thread==frame.thread && c->frame.pcr==frame.pcr &&
      c->frame.function==frame.function && c->context==context &&
      (body?c->body_sp==frame.sp:c->frame.sp==frame.sp && c->frame.lr==frame.lr))
    if(!found || c->serial>found->serial)found=&*c;
  return found;
 }
 ImageCall* ImageParent(const Frame& frame,uintptr_t context){
  ImageCall* found=nullptr;
  for(auto& c:image_calls)if(c && !c->uncertain && c->context==context &&
      c->frame.root==frame.root && c->frame.thread==frame.thread &&
      c->frame.pcr==frame.pcr && c->body_sp==frame.sp &&
      frame.lr>c->frame.function && NativeImageCode(c->frame.function) &&
      uint64_t(frame.lr)<=uint64_t(c->frame.function)+NativeImageCode(c->frame.function)->bytes &&
      (!found || c->serial>found->serial))found=&*c;
  return found;
 }
 void LoseImageObservation() noexcept {
  if(image_slot)image_slot->unavailable_.store(true,std::memory_order_release);
 }
 void RevokeImageString(uint32_t pointer,uint32_t field=0) noexcept {
  for(auto& key:image_strings)if(key.live &&
      ((pointer && key.pointer==pointer) || (field && key.field==field)))key.live=false;
 }
 struct ImageKeyCapture {
  ImageInput input;uint64_t provider=0,uri_generation=0,string_generation=0;
 };
 std::optional<ImageKeyCapture> ImageInputAt(uint32_t pointer) const {
  if(!pointer)return {};
  std::optional<ImageKeyCapture> found;
  for(const auto& provider:providers)if(provider && provider->image_key &&
      !provider->revoked && !provider->destroyed &&
      provider->image_key_generation==provider->image_uri_generation &&
      pointer==provider->object+0x10){
   ImageKeyCapture actual{*provider->image_key,provider->object,provider->image_uri_generation,0};
   if(found && found->input!=actual.input)return {};found=actual;
  }
  for(const auto& key:image_strings)if(key.live && key.pointer==pointer){
   ImageKeyCapture actual{key.input,0,0,key.generation};
   if(found && found->input!=actual.input)return {};found=actual;
  }
  return found;
 }
 bool ImageInputStillCurrent(const ImageCall& call) const {
  if(!call.input || !call.input->valid())return false;
  const auto actual=ImageInputAt(call.key_pointer);
  return actual && actual->input==*call.input && actual->provider==call.key_provider &&
      actual->uri_generation==call.uri_generation && actual->string_generation==call.key_generation;
 }
 bool ImageBytesCurrent(const ImageInput& input) const {
  if(!input.valid() || input.root()!=root)return false;
  if(input.provider){
   for(const auto& provider:providers)if(provider &&
       provider->generation==input.provider->provider && !provider->revoked &&
       !provider->destroyed && provider->image_epoch.Current(*input.provider))return true;
   return false;
  }
  // The section issuer separately authenticated the actual resource/entry
  // tuple. This existing root code/domain owner keeps its original bytes
  // resident through this synchronous use; no new native reference is taken.
  return input.section && root_code && config.domain && config.catalog &&
      config.domain->root().hmodule==input.section->module &&
      config.domain->root().generation==input.section->module_generation &&
      config.catalog->calls()->module()->load_generation()==input.section->module_generation;
 }
 ImageCall* ImageDecoder(const ImageCall& nested,uint32_t png) {
  ImageCall* found=nullptr;
  for(auto& decoder:image_calls)if(decoder && !decoder->uncertain && !decoder->error &&
      decoder->frame.function==0x81784D80 && decoder->input && decoder->png_context==png &&
      decoder->context==nested.context && decoder->cpu==nested.cpu &&
      decoder->frame.root==nested.frame.root && decoder->frame.thread==nested.frame.thread &&
      decoder->frame.pcr==nested.frame.pcr && nested.frame.sp<=decoder->body_sp &&
      (!found || decoder->serial>found->serial))found=&*decoder;
  return found;
 }
 bool RememberImageString(uint32_t pointer,uint32_t field,const ImageInput& input){
  if(!pointer || !field || !input.valid() || input.root()!=root ||
     next_image_string==UINT64_MAX){LoseImageObservation();return false;}
  const auto version=input.provider?input.provider->version:input.section->version;
  if(!NativeMemoryKeyMatches(*kernel->memory(),pointer,input.address(),input.size(),version))return false;
  const auto slot=std::find_if(image_strings.begin(),image_strings.end(),
      [](const auto& r){return !r.live;});
  if(slot==image_strings.end()){LoseImageObservation();return false;}
  *slot={++next_image_string,pointer,field,input,true};return true;
 }
 // Caller holds State::mutex. This narrow initial absence fact comes from
 // the actual PM envelopes and genuine native protocol, never an empty
 // provider census, lack of notification or a Guide draw alone.
 bool EmptyInitialFrameCurrent(const std::shared_ptr<const gpu::NativeFramePresentationGuard>& guard,
     uint64_t source) const {
  return job && job->bootstrap_alignment.load() && current_read_source==kInitialReadSource &&
      job->source==source && job->intent && current_roster==job->intent->old_roster() &&
      job->native_entered.load() && job->native_returned.load() && job->publisher_observed.load() &&
      !job->transport_running.load() && !job->execution_uncertain.load() && !job->canceled.load() && !job->done.load() &&
      job->protocol_returned && job->protocol_work && job->protocol_birth.generation &&
      job->protocol_birth.task && job->protocol_frame.root==root && job->protocol_frame.function==0x8199C838 &&
      published_source.load(std::memory_order_acquire)==job->source &&
      render_phase==RenderPhase::kHiddenRefresh && job->hidden_pixels && pixels==job->hidden_pixels &&
      guard.get()==job->hidden_pixels.get() && !guard.owner_before(job->hidden_pixels) &&
      !job->hidden_pixels.owner_before(guard) && !stopping.load() && !uncertain.load() && !retired.load() &&
      job->intent->HasPublishedEmptyUnchangedRows(job->protocol_value);
 }
 std::optional<Frame> ImageShape(cpu::ppc::PPCContext& c,uint32_t at) const {
  auto* thread=ActualThread();
  if(!thread || thread->kernel_state()!=kernel || c.kernel_state!=kernel ||
     c.processor!=kernel->processor() || c.thread_state!=thread->thread_state() ||
     thread->thread_state()->context()!=&c || c.r[13]!=thread->pcr_ptr() ||
     !DwordIs(c.r[1],uint32_t(c.r[1])) || (uint32_t(c.r[1])&15) ||
     !DwordIs(c.lr,uint32_t(c.lr)) || (uint32_t(c.lr)&3) ||
     uint32_t(c.r[1])<thread->stack_limit() || uint32_t(c.r[1])>thread->stack_base())return {};
  return Frame{root,thread->thread_id(),thread->pcr_ptr(),uint32_t(c.r[1]),uint32_t(c.lr),at};
 }

 KernelState* kernel;
 const Tasks::Root root;
 identity_detail::NativeImageJournal images;
 std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
 CodeOwner prepared_title;
 uint64_t prepared_title_generation=0;
 std::shared_ptr<const GuestRuntimeCallExecutor::ProfileIdentityProviderBinding> provider_binding;
 NativeImportDomain::Operation resident_domain;
 std::weak_ptr<NativeSystemTaskContinuation> continuation;
 std::shared_ptr<xam::ProfileRosterNativeReadTicket> current_ticket;
 std::shared_ptr<const xam::HostRoster> current_roster;
 std::array<std::unique_ptr<xam::ProfileRosterNativeReadScope>,64> boot_reads;
 mutable std::mutex mutex;
 std::condition_variable changed;
 std::atomic<bool> stopping{false},retired{false},uncertain{false};
 std::atomic<uint64_t> published_source{0};
 // Advisory only. The one writer publishes scalar payload with release; no
 // capture lookup, lock, allocation, logging, guest-memory read, or reset.
 std::atomic<uint32_t> first_loss_line{0},first_denial_state{0};
 std::atomic<bool> first_refusal_reported{false};
 // Single lock-free scalar: high DWORD reason, low DWORD tagged target. No
 // native lock/path logging or allocation; emitted only at existing host cuts.
 std::atomic<uint64_t> first_task_close_refusal{0};
 struct PublisherAbortSnapshot {
  uint32_t available=0,thread=0,pcr=0,worker_sp=0,input=0,task=0;
  uint32_t stack_status=0,stack_index=0;
  uint64_t source=0,nonce=0,birth=0,work=0,lr=0,sp=0;
  uint64_t argument=0,flags=0,title=0,packet_register=0,r29_residue=0;
 };
 std::atomic<uint32_t> first_publisher_abort_state{0};
 PublisherAbortSnapshot first_publisher_abort;
 mutable NativeSystemUiTraceLimiter publisher_abort_trace;
 void RecordPublisherAbort(const Commit& cell,const cpu::ppc::PPCContext& c) noexcept {
  // Observe the authenticated common abort callsite ONLY. The original
  // DBEC-only admission below is unchanged. No lookup, lock, allocation,
  // guest helper, Task/TLS mutation or logging occurs at this hot cut.
  if(!DwordIs(c.lr,0x8199DA38) || cell.owner!=this || kernel!=c.kernel_state ||
     cell.context!=&c || cell.thread_state!=c.thread_state ||
     !DwordIs(c.r[13],cell.pcr) || !DwordIs(c.r[1],cell.publisher_sp) ||
     !DwordIs(c.r[3],cell.input) || !cell.source || !cell.claimed_nonce ||
     !cell.worker_birth || !cell.whole_work)return;
  uint32_t empty=0;
  if(!first_publisher_abort_state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
  PublisherAbortSnapshot d;
  d.available=1;d.thread=cell.diagnostic_thread;d.pcr=cell.pcr;
  d.worker_sp=cell.publisher_sp;d.input=cell.input;d.task=cell.diagnostic_task;
  d.source=cell.source;d.nonce=cell.claimed_nonce;d.birth=cell.worker_birth;d.work=cell.whole_work;
  d.lr=c.lr;d.sp=c.r[1];d.argument=c.r[3];d.flags=c.r[4];d.title=c.r[5];
  d.packet_register=c.r[28];d.r29_residue=c.r[29];
  if(cell.diagnostic_status){d.stack_status=NativeAtomicWord(cell.diagnostic_status);d.available|=2;}
  if(cell.diagnostic_index){d.stack_index=NativeAtomicWord(cell.diagnostic_index);d.available|=4;}
  first_publisher_abort=d;first_publisher_abort_state.store(2,std::memory_order_release);
 }
 void ReportPublisherAbort() const noexcept try {
  const auto state=first_publisher_abort_state.load(std::memory_order_acquire);
  const auto d=state==2?first_publisher_abort:PublisherAbortSnapshot{};
  publisher_abort_trace.EmitTransition(state,"identity","publisher_abort_entry",
      {{"capture_state",state},{"available",d.available},{"thread",d.thread},{"pcr",d.pcr},
       {"worker_sp",d.worker_sp},{"input",d.input},{"task",d.task},{"source",d.source},
       {"nonce",d.nonce},{"birth",d.birth},{"work",d.work},{"lr",d.lr},{"sp",d.sp},
       {"argument",d.argument},{"flags",d.flags},{"title",d.title},
       {"packet_register",d.packet_register},{"r29_loop_residue",d.r29_residue},
       {"stack_B0_status",d.stack_status},{"stack_D0_index",d.stack_index}});
  // Readable RAM does not prove that a branch initialized every word. B0 is
  // preserved status, not every first negative; r29 is the clear-loop residue.
  // These values do not prove a particular branch, cancellation or drain.
 }catch(...){/* Safe-host advisory output never changes native permission/results. */}
 struct ReferenceCloseSnapshot {
  uint64_t serial=0;uint32_t task=0,remaining=0,lookup=0,parent=0;
  Frame acquired{},callback{},release{};
 };
 std::atomic<uint32_t> first_reference_close_state{0};
 ReferenceCloseSnapshot first_reference_close;
 struct ReferenceLookupSnapshot {uint64_t serial=0;uint32_t task=0,kind=0,rows=0,freed=0,free_in_progress=0,live=0;};
 std::atomic<uint32_t> first_reference_lookup_state{0};
 ReferenceLookupSnapshot first_reference_lookup;
 void ReportReferenceClose() const noexcept try {
  const auto lookup_state=first_reference_lookup_state.load(std::memory_order_acquire);
  ReferenceLookupSnapshot lookup;if(lookup_state==2)lookup=first_reference_lookup;
  static NativeSystemUiTraceLimiter lookup_trace;
  lookup_trace.EmitTransition(lookup_state,"identity","owned_reference_lookup",
      {{"capture_state",lookup_state},{"available",lookup_state==2},{"serial",lookup.serial},
       {"task",lookup.task},{"lookup_kind",lookup.kind},{"matching_rows",lookup.rows},
       {"freed_rows",lookup.freed},{"free_in_progress",lookup.free_in_progress},{"live_rows",lookup.live}});
  const auto state=first_reference_close_state.load(std::memory_order_acquire);
  ReferenceCloseSnapshot v;if(state==2)v=first_reference_close;
  static NativeSystemUiTraceLimiter trace;
  trace.EmitTransition(state,"identity","owned_reference_nonfinal_close",
      {{"capture_state",state},{"available",state==2},{"serial",v.serial},{"task",v.task},
       {"remaining",v.remaining},{"lookup_kind",v.lookup},{"saved_worker_caller",v.parent},
       {"acquire_thread",v.acquired.thread},{"acquire_pcr",v.acquired.pcr},
       {"acquire_sp",v.acquired.sp},{"acquire_lr",v.acquired.lr},
       {"callback_thread",v.callback.thread},{"callback_pcr",v.callback.pcr},
       {"callback_sp",v.callback.sp},{"callback_lr",v.callback.lr},
       {"callback_function",v.callback.function},{"release_thread",v.release.thread},
       {"release_pcr",v.release.pcr},{"release_sp",v.release.sp},{"release_lr",v.release.lr}});
 }catch(...){/* Safe-host observations never change permission or results. */}
 void RecordTaskCloseRefusal(uint32_t reason,uint32_t target) noexcept {
  uint64_t empty=0;first_task_close_refusal.compare_exchange_strong(empty,
      (uint64_t(reason)<<32)|target,std::memory_order_relaxed);
 }
 // Advisory first failed own-return. Publication is allocation/lock/IO-free
 // even under native GCRC/State. Only existing safe host reporters read it.
 struct ReturnFailure {
  uint32_t at=0,line=0,thread=0,pcr=0,phase=0,flags=0,prior_loss=0,capture_kind=0;
  uint32_t tasks_line=0,tasks_reason=0,wake_line=0,wake_reason=0;
  uint32_t birth_kind=0,birth_task=0,input_first=0,input_second=0;
  uint64_t lr=0,sp=0,value=0,source=0,work=0,birth_generation=0,birth_origin=0;
  uint64_t provider_generation=0,release_generation=0,current_read_source=0,job_source=0,job_serial=0;
 };
 std::atomic<uint32_t> first_return_state{0};
 ReturnFailure first_return_failure;
 void RecordReturnFailure(const cpu::ppc::PPCContext& c,uint32_t at,
      const Frame& frame,const Work& work,const Tasks::Birth& birth,uint32_t birth_kind,
      bool exact,uint32_t phase,uint32_t line,uint32_t capture_kind=1) noexcept {
  uint32_t empty=0;
  if(!first_return_state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
  ReturnFailure d;
  d.at=at;d.line=line;d.thread=frame.thread;d.pcr=frame.pcr;d.phase=phase;d.capture_kind=capture_kind;
  d.prior_loss=first_loss_line.load(std::memory_order_relaxed);
  d.lr=c.lr;d.sp=c.r[1];d.value=c.r[3];d.source=work.source;d.work=work.serial;
  d.birth_kind=birth_kind;d.birth_task=birth.task;d.birth_generation=birth.generation;d.birth_origin=birth.origin;
  d.input_first=work.first;d.input_second=work.second;d.provider_generation=work.provider_generation;
  d.release_generation=work.release_generation;d.current_read_source=current_read_source;
  d.job_source=job?job->source:0;d.job_serial=job?job->serial:0;
  d.tasks_line=tasks.FirstFailureLine();d.tasks_reason=tasks.FirstFailureReason();
  d.wake_line=wake?wake->FirstFailureLine():0;d.wake_reason=wake?wake->FirstFailureReason():0;
  d.flags=uint32_t(exact) | (uint32_t(tasks.lost())<<1) | (uint32_t(wake && wake->lost())<<2) |
      (uint32_t(work.task_release)<<3) | (uint32_t(work.foreign_avatar_task)<<4) |
      (uint32_t(work.foreign_task_cleanup)<<5) | (uint32_t(work.transport_only)<<6) |
      (uint32_t(work.transport_unclaimed)<<7) | (uint32_t(work.owns_task_work)<<8) |
      (uint32_t(work.owns_persistent_work)<<9) | (uint32_t(bool(work.listener_work))<<10) |
      (uint32_t(bool(work.external_birth.generation))<<11) | (uint32_t(bool(work.commit))<<12) |
      (uint32_t(work.registered_executor)<<13) | (uint32_t(work.host_body)<<14) |
      (uint32_t(bool(work.read))<<15) | (uint32_t(work.physical_work)<<16) |
      (uint32_t(bool(job))<<17) | (uint32_t(job && job->native_entered)<<18);
  first_return_failure=d;
  first_return_state.store(2,std::memory_order_release);
 }
 void RecordReturnPartial(const cpu::ppc::PPCContext& c,uint32_t at,uint32_t phase,
      uint32_t line,uint32_t thread=0,uint32_t pcr=0) noexcept {
  // Unlocked return/frame/exception cut: only already available scalar inputs
  // and atomics. No ledger/work/config/plain-State read or new lock here.
  uint32_t empty=0;
  if(!first_return_state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
  ReturnFailure d;d.at=at;d.line=line;d.phase=phase;d.capture_kind=3;
  d.thread=thread;d.pcr=pcr;d.flags=0x80000000u;
  d.lr=c.lr;d.sp=c.r[1];d.value=c.r[3];
  d.prior_loss=first_loss_line.load(std::memory_order_relaxed);
  first_return_failure=d;first_return_state.store(2,std::memory_order_release);
 }
 void ReportReturnFailure(const char* boundary) const {
  const auto state=first_return_state.load(std::memory_order_acquire);
  const auto d=state==2?first_return_failure:ReturnFailure{};
  XELOGE("Native identity own-return scalar snapshot at safe host {}: state={} payload_available={} "
         "at={:08X} line={} thread={} pcr={:08X} lr={:016X} sp={:016X} value={:016X} "
         "phase={} capture_kind={} flags={:08X} prior_loss_line={} source={} work={} "
         "birth_kind={} birth_generation={} birth_origin={} birth_task={:08X} "
         "tasks_reason={} tasks_line={} wake_reason={} wake_line={} "
         "provider_generation={} release_generation={} current_read_source={} job_source={} job_serial={} "
         "input_first={:08X} input_second={:08X}; advisory only",
         boundary,state,state==2,d.at,d.line,d.thread,d.pcr,d.lr,d.sp,d.value,d.phase,d.capture_kind,d.flags,d.prior_loss,
         d.source,d.work,d.birth_kind,d.birth_generation,d.birth_origin,d.birth_task,
         d.tasks_reason,d.tasks_line,d.wake_reason,d.wake_line,d.provider_generation,d.release_generation,
         d.current_read_source,d.job_source,d.job_serial,d.input_first,d.input_second);
 }
 struct ListenerDiagnostic {bool present=false;Listeners::FailureSnapshot snapshot;};
 ListenerDiagnostic CaptureListenerDiagnostic() const {
  // Existing State mutex protects the unique Bridge against actual stopped
  // retirement. Copy only permanent scalar diagnostics; log after unlocking.
  std::lock_guard lock(mutex);ListenerDiagnostic d;
  if(listeners){d.present=true;d.snapshot=listeners->FirstFailure();}return d;
 }
 static void ReportListenerDiagnostic(const char* boundary,const ListenerDiagnostic& observed) {
  const auto& snapshot=observed.snapshot;const auto& d=snapshot.data;
  XELOGE("Native listener first-failure scalar snapshot at safe host {}: present={} state={} payload_available={} "
         "line={} reason={} available={:08X} checks={:08X} ledger_reason={} ledger_line={} "
         "ledger_at_capture_reason={} ledger_at_capture_line={} context={:X} thread_state={:X} kernel={:X} candidate={:X} "
         "lr={:016X} sp={:016X} r3={:016X} r4={:016X} r5={:016X} r6={:016X} r13={:016X} "
         "r25={:016X} r28={:016X} r29={:016X} r30={:016X} r31={:016X} "
         "frame_function={:08X} frame_thread={} frame_pcr={:08X} frame_sp={:08X} frame_lr={:08X} "
         "kind={} inner={:08X} inner_sp={:08X} area={:08X} argument={:08X} handle={:08X} linearized={} "
         "incoming_id={:016X} selected_id={:08X} listener={:08X} listener_serial={} origin={} work={} "
         "reference_flags={:08X} detail0={:016X} detail1={:016X} detail2={:016X}; advisory only, availability required",
         boundary,observed.present,snapshot.state,snapshot.state==2,d.line,d.reason,d.available,d.checks,
         uint32_t(snapshot.ledger>>32),uint32_t(snapshot.ledger),
         uint32_t(d.ledger_at_capture>>32),uint32_t(d.ledger_at_capture),d.context,d.thread_state,d.kernel,d.candidate,
         d.lr,d.sp,d.r3,d.r4,d.r5,d.r6,d.r13,d.r25,d.r28,d.r29,d.r30,d.r31,
         d.frame_function,d.frame_thread,d.frame_pcr,d.frame_sp,d.frame_lr,d.kind,d.inner,d.inner_sp,d.area,d.argument,d.handle,
         d.linearized,d.incoming_id,d.selected_id,d.listener_object,d.listener_serial,d.origin,d.work,d.reference_flags,
         d.detail0,d.detail1,d.detail2);
 }
 uint32_t first_denial_line=0,first_denial_entry=0;
 uint64_t first_denial_lr=0,first_denial_sp=0;
 std::array<uint64_t,4> first_denial_args{};
 void RecordDenial(const cpu::ppc::PPCContext& c,uint32_t entry,uint32_t line) noexcept {
  uint32_t empty=0;
  if(!first_denial_state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
  first_denial_line=line;first_denial_entry=entry;
  first_denial_lr=c.lr;first_denial_sp=c.r[1];
  for(size_t i=0;i<first_denial_args.size();++i)first_denial_args[i]=c.r[i+3];
  first_denial_state.store(2,std::memory_order_release);
 }
 uint64_t Deny(cpu::ppc::PPCContext& c,uint32_t entry,uint32_t line) noexcept {
  RecordDenial(c,entry,line);return Abort(c,entry);
 }

 enum class RenderPhase {kOpen,kDrainingOld,kClosingOld,kOldPaused,kHiddenRefresh,kClosingPrepared,kPreparedPaused,kClosingStop,kStopPaused};
 // These receipts observe original cuts only. No pointer in the payload is
 // dereferenced by the reporter, and no reporter acquires the owner mutex.
 struct QueryObservation {
  bool enabled=false,source_retired=false,parked=false;
  uint32_t reason=0,loops=0,phase=0,active=0,expected=0,attempt=0,flags=0;
  uint32_t old_evaluated=0,old_truth=0,task=0,first_loss=0;
  uint32_t prepared_evaluated=0,prepared_truth=0,prepared_pending=0;
  bool prepared_pending_available=false;
  WorkBlocker prepared_work;Tasks::SourceBlocker prepared_source;
  Listeners::SourceProducerObservation prepared_origin;
  uint64_t observation=0,job=0,source=0,current_read=0,next_work=0,sp=0,lr=0,pcr=0;
  uint64_t context_thread_state=0,expected_thread_state=0,actual_thread=0;
  Frame frame;Tasks::Birth birth{};
  WorkBlocker work_blocker;Tasks::SourceBlocker source_blocker;
  std::array<uint64_t,PaintObservation::kCount> paint{};
  bool Failed(uint32_t why,bool value) noexcept {
   if(enabled && value)reason=why;return value;
  }
  bool Old(uint32_t bit,bool value) noexcept {
   if(enabled){old_evaluated|=1u<<bit;if(value)old_truth|=1u<<bit;}return value;
  }
  bool Prepared(uint32_t bit,bool value) noexcept {
   if(enabled){prepared_evaluated|=1u<<bit;if(value)prepared_truth|=1u<<bit;}return value;
  }
  XThread* Actual(XThread* value) noexcept {
   if(enabled)actual_thread=reinterpret_cast<uintptr_t>(value);return value;
  }
 };
 uint64_t query_observation=0; // original State mutex, advisory correlation only
 std::atomic<uint64_t> bootstrap_handoff{0}; // turn:32, actual cut:32
 struct QueryExitCell {std::atomic<uint32_t> state{0};QueryObservation payload;};
 QueryExitCell first_parked_query_exit;
 // Called only after the original State lock has left scope. Unlike the
 // legacy summary, this bounded cut report includes normal admission reason24.
 void ReportPreparedQuery(const QueryObservation& q) noexcept try {
  if(!q.enabled || !(q.prepared_evaluated&~1u) ||
     global_critical_region::is_held_by_current_thread())return;
  const auto& w=q.prepared_work;const auto& b=q.prepared_source;const auto& o=q.prepared_origin;
  static NativeSystemUiTraceLimiter trace;
  const uint64_t key=q.job^(uint64_t(q.prepared_evaluated)<<32)^q.prepared_truth^
      w.serial^b.generation^b.serial^(o.references<<16);
  trace.EmitTransition(key,"identity","frame_query_prepared_predicates",
   {{"observation",q.observation},{"job_serial",q.job},{"job_source",q.source},{"current_read",q.current_read},
    {"reason",q.reason},{"render_phase",q.phase},{"source_retired",q.source_retired},{"parked",q.parked},
    {"evaluated",q.prepared_evaluated},{"predicate_true",q.prepared_truth},{"expected",q.expected},
    {"work_available",w.available},{"work_closed",w.closed},{"work_reason",w.reason},
    {"work",w.serial},{"work_source",w.source},{"work_state",w.state},{"work_first_argument",w.first_argument},
    {"work_birth",w.birth.generation},{"work_task",w.birth.task},{"work_origin",w.birth.origin},
    {"work_thread",w.frame.thread},{"work_pcr",w.frame.pcr},{"work_sp",w.frame.sp},
    {"work_lr",w.frame.lr},{"work_function",w.frame.function},
    {"listener_available",o.available},{"listener_mode",o.mode},{"listener_evaluated",o.evaluated},{"listener_true",o.truth},
    {"listener_references_available",o.references_available},{"listener_references",o.references},
    {"listener_orphan_available",o.orphan_available},{"listener_orphaned",o.orphaned},
    {"listener_consumers_available",o.consumers_available},{"listener_consumer_reason",o.consumer_reason},
    {"listener_consumer_work",o.consumer_work},{"listener_active_cell",o.active_cell},
    {"orphan_capture_state",o.orphan_capture_state},{"orphan_origin_matches",o.orphan_origin_matches},
    {"orphan_origin",o.first_orphan.origin.serial},{"orphan_origin_slot",o.first_orphan.origin.slot},
    {"orphan_sequence",o.first_orphan.enqueue_sequence},{"orphan_work_hint",o.first_orphan.consumer_work_hint},
    {"orphan_thread",o.first_orphan.thread},{"orphan_pcr",o.first_orphan.pcr},
    {"orphan_sp",o.first_orphan.sp},{"orphan_lr",o.first_orphan.lr},{"orphan_function",o.first_orphan.function},
    {"orphan_process_type",o.first_orphan.process_type},
    {"orphan_context",o.first_orphan.context},{"orphan_thread_state",o.first_orphan.thread_state},
    {"orphan_handle",o.first_orphan.handle},{"orphan_listener",o.first_orphan.listener},
    {"orphan_area",o.first_orphan.area},{"orphan_event",o.first_orphan.event},
    {"orphan_data_available",o.first_orphan.data_available},{"orphan_data",o.first_orphan.data},
    {"orphan_outer_return",o.orphan_outer_return},
    {"source_available",b.available},{"source_closed",b.closed},{"source_reason",b.kind},
    {"source",b.source},{"source_generation",b.generation},{"source_task",b.task},{"source_serial",b.serial},
    {"pending_available",q.prepared_pending_available},{"native_pending",q.prepared_pending}});
 }catch(...){/* Scalar observation failure never changes native admission. */}
 void ReportQuery(const QueryObservation& q,bool exiting) noexcept try {
  if(!q.enabled || (exiting && q.reason==24))return;
  if(exiting && q.parked){
   uint32_t empty=0;
   if(first_parked_query_exit.state.compare_exchange_strong(empty,1,std::memory_order_relaxed)){
    first_parked_query_exit.payload=q;
    first_parked_query_exit.state.store(2,std::memory_order_release);
   }
  }
  if(global_critical_region::is_held_by_current_thread())return; // scalar capture only at an unsafe logging cut
  const uint64_t bootstrap=bootstrap_handoff.load(std::memory_order_acquire);
  const auto first_capture=first_parked_query_exit.state.load(std::memory_order_acquire);
  const auto emit=[&](const QueryObservation& v,uint32_t kind){
   // Loop/attempt/turn are payload, never transition keys. Sampling stays
   // bounded when a parked manager keeps observing the same original cut.
   const uint64_t key=v.job^(uint64_t(v.reason)<<48)^(uint64_t(v.phase)<<40)^uint32_t(bootstrap);
   static std::array<NativeSystemUiTraceLimiter,2> summary,context,conditions;
   summary[kind].EmitTransition(key,"identity","frame_query_handoff",
    {{"kind",kind},{"observation",v.observation},{"exiting",kind==1 || exiting},{"reason",v.reason},{"loops",v.loops},
     {"parked",v.parked},{"source_retired",v.source_retired},{"render_phase",v.phase},
     {"job_serial",v.job},{"job_source",v.source},{"current_read",v.current_read},
     {"active",v.active},{"expected",v.expected},{"flags",v.flags},{"next_work",v.next_work},
     {"bootstrap_turn",bootstrap>>32},{"bootstrap_cut",uint32_t(bootstrap)},{"first_loss_at_loop_cut",v.first_loss},
     {"first_parked_exit_state",first_capture}});
   context[kind].EmitTransition(key,"identity","frame_query_context",
    {{"kind",kind},{"observation",v.observation},{"reason",v.reason},{"job_serial",v.job},{"task",v.task},
     {"frame_thread",v.frame.thread},{"frame_pcr",v.frame.pcr},{"frame_sp",v.frame.sp},
     {"frame_lr",v.frame.lr},{"frame_function",v.frame.function},{"birth",v.birth.generation},
     {"birth_task",v.birth.task},{"birth_origin",v.birth.origin},{"raw_sp",v.sp},{"raw_lr",v.lr},
     {"raw_pcr",v.pcr},{"context_thread_state",v.context_thread_state},
     {"expected_thread_state",v.expected_thread_state},{"actual_thread",v.actual_thread}});
   conditions[kind].EmitTransition(key^v.old_evaluated^(uint64_t(v.old_truth)<<16),"identity","frame_query_old_predicates",
    {{"kind",kind},{"observation",v.observation},{"job_serial",v.job},{"evaluated",v.old_evaluated},{"predicate_true",v.old_truth},
     {"work_available",v.work_blocker.available},{"work_closed",v.work_blocker.closed},
     {"work_reason",v.work_blocker.reason},{"work",v.work_blocker.serial},{"work_source",v.work_blocker.source},
     {"work_state",v.work_blocker.state},{"source_available",v.source_blocker.available},
     {"source_closed",v.source_blocker.closed},{"source_reason",v.source_blocker.kind},
     {"source",v.source_blocker.source},{"source_generation",v.source_blocker.generation},
     {"source_task",v.source_blocker.task},{"source_serial",v.source_blocker.serial}});
   static_assert(PaintObservation::kCount==13);
   static std::array<NativeSystemUiTraceLimiter,2> paint;
   // Existing attempt-tagged atomic words, captured under the original Query
   // metadata lock. Decode with NativeFramePaintDrainObservation; never read
   // the Bootstrap writer's non-atomic scratch or add a guest/fence read.
   paint[kind].EmitTransition(key,"identity","frame_query_preentry",
    {{"kind",kind},{"observation",v.observation},{"job_serial",v.job},{"attempt",v.attempt},
     {"bootstrap_turn",bootstrap>>32},{"bootstrap_cut",uint32_t(bootstrap)},
     {"paint0",v.paint[0]},{"paint1",v.paint[1]},{"paint2",v.paint[2]},
     {"paint3",v.paint[3]},{"paint4",v.paint[4]},{"paint5",v.paint[5]},
     {"paint6",v.paint[6]},{"paint7",v.paint[7]},{"paint8",v.paint[8]},
     {"paint9",v.paint[9]},{"paint10",v.paint[10]},{"paint11",v.paint[11]},{"paint12",v.paint[12]}});
  };
  emit(q,0);
  if(first_capture==2)emit(first_parked_query_exit.payload,1);
 }catch(...){/* Observation failure never changes the original result. */}

 RenderPhase render_phase=RenderPhase::kOpen;
 const std::shared_ptr<IdentityPixelLifetime> pixel_lifetime=profile_enabled?std::make_shared<IdentityPixelLifetime>():nullptr;
 std::shared_ptr<IdentityPixelGuard> pixels=profile_enabled?std::make_shared<IdentityPixelGuard>(kInitialReadSource,false,pixel_lifetime):nullptr;
 bool render_query_parked=false;
 object_ref<XThread> parked_render_thread;
 uint32_t parked_render_pcr=0,parked_render_sp=0;
 uint64_t parked_render_birth=0;
 bool aligned=false,boot_alignment_claimed=false;
 // Pre-native attempts may contend with real old readers/writers. Retain the
 // exact unpublished common intent until cancellation actually finishes.
 std::shared_ptr<xam::ProfileRosterIntent> bootstrap_cleanup;
 std::chrono::steady_clock::time_point bootstrap_deadline{};
 uint32_t bootstrap_attempts=0;
 bool bootstrap_fatal=false;
 struct BootstrapFailure {
  uint32_t stage=0,line=0,error=0,category=0,retryable=0,evaluated=0,failed=0,intent_phase=UINT32_MAX;
 };
 struct BootstrapFailureCell {
  std::atomic<uint32_t> state{0};BootstrapFailure payload;
  void Record(const BootstrapFailure& value) noexcept {
   uint32_t empty=0;if(!state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
   payload=value;state.store(2,std::memory_order_release);
  }
 } first_bootstrap_refusal,first_bootstrap_fatal;
 static bool RetryableBootstrapError(const std::error_code& error) noexcept {
  return error==std::errc::device_or_resource_busy ||
         error==std::errc::resource_unavailable_try_again;
 }
 void RecordBootstrapFailure(uint32_t stage,uint32_t line,const std::error_code& error,
     bool retryable,uint32_t evaluated=0,uint32_t failed=0,uint32_t phase=UINT32_MAX) noexcept {
  const BootstrapFailure value{stage,line,uint32_t(error.value()),
      error.category()==std::generic_category()?1u:error.category()==std::system_category()?2u:0u,
      uint32_t(retryable),evaluated,failed,phase};
  first_bootstrap_refusal.Record(value);if(!retryable)first_bootstrap_fatal.Record(value);
 }
 uint64_t current_read_source=kInitialReadSource,next_source=3,next_work=1;
 bool auxiliary_claimed=false,auxiliary_returned=false,auxiliary_ready=false;
 std::atomic<bool> auxiliary_host_active{false};
 std::atomic<uint64_t> identity_executor_frames{0},listener_object_frames{0},task_close_frames{0};
 Tasks tasks;
 std::unique_ptr<Wake> wake;
 std::unique_ptr<Listeners> listeners;
 std::shared_ptr<Job> job;
 std::vector<std::shared_ptr<Work>> active,host_retired;
 std::shared_ptr<IdentityServiceOrigin> identity_service;
 std::optional<Tasks::IdentityServiceRequest::Pump> identity_service_pump;
 bool TryAdoptIdentityServiceTick(cpu::ppc::PPCContext&,const Frame&,
     const std::shared_ptr<IdentityServiceOrigin>&,const std::shared_ptr<Work>&,bool);
 bool IdentityServiceJoined(const std::shared_ptr<Job>&) const;
 bool IdentityServiceRetainable() const;
 std::vector<std::shared_ptr<ExecutionLifetime>> execution_lifetimes;
 std::vector<std::shared_ptr<Work>> execution_retained;
 std::atomic<bool> execution_unavailable{false};
 Tasks::ExecutionUnavailableFrames unavailable_execution_frames;
 // Fixed native App registration owns this class instance for the retained
 // root/load generation. No arbitrary heap object or writable pointer issues it.
 uint32_t periodic_object=0;uint64_t next_periodic=1;
 std::array<std::optional<Tasks::PeriodicFactory>,8> periodic_factories{};
 Tasks::PeriodicFactory* Periodic(uint64_t serial) {
  for(auto& row:periodic_factories)if(row && row->serial==serial)return &*row;return nullptr;
 }
 const Tasks::PeriodicFactory* Periodic(uint64_t serial) const {
  for(const auto& row:periodic_factories)if(row && row->serial==serial)return &*row;return nullptr;
 }
 const Tasks::PeriodicFactory* PeriodicObject(uint32_t object) const {
  for(const auto& row:periodic_factories)if(row && row->object==object)return &*row;return nullptr;
 }
 bool LivePeriodic(const Tasks::PeriodicFactory& factory,const std::optional<Tasks::Birth>& live) const {
  if(tasks.lost() || !live || live->generation!=factory.birth.generation ||
     live->origin!=1 || live->task!=factory.birth.task)return false;
  const auto* node=tasks.FindBirthConst(live->generation);
  return node && !node->freed && !node->delete_entered && !node->free_entered &&
      node->periodic_factory==factory.serial;
 }
 bool PeriodicStopped() const {
  for(const auto& row:periodic_factories)if(row && (row->failed || row->enabling || row->stopping || !row->stopped))return false;
  return true;
 }
 bool PeriodicObjectCurrent(uint32_t object) const {
  auto guard=global_critical_region::AcquireDirect();
  const auto calls=config.catalog->calls();const auto module=calls->module();auto* memory=kernel->memory();
  if(!periodic_object || object!=periodic_object || object>UINT32_MAX-0x208 ||
     Word(*memory,0x81A96838)!=object || Word(*memory,object)!=0x8162F258u)return false;
  for(uint32_t at:std::array<uint32_t,3>{object,object+0x207,0x8162F258}){
   xex2_section_type type;
   if(kernel->GetUserModuleForAddress(at).get()!=module.get() ||
      !module->xex_module()->GetPageSectionType(at,&type) || type==XEX_SECTION_CODE)return false;
  }
  return Word(*memory,0x8162F258+0x18)==0x8190B530u &&
      Word(*memory,0x8162F258+0x20)==0x81907C98u &&
      Word(*memory,0x8162F258+0x28)==0x8190B528u;
 }
 std::vector<std::shared_ptr<Expected>> expected,retired_expected;
 std::array<std::shared_ptr<TaskReference::Data>,64> task_references{};
 uint64_t next_task_reference=1;
 std::array<std::shared_ptr<ContextReference>,64> context_references{};
 uint64_t next_context_reference=1;
 bool context_reference_unavailable=false;
 std::shared_ptr<ContextReference> CurrentContextReference(const XThread& thread) const {
  std::shared_ptr<ContextReference> found;
  for(const auto& row:context_references)if(row && row->thread.get()==&thread &&
      row->receipt.NewerActiveThan(found?&found->receipt:nullptr))found=row;
  return found; // An uncertain younger frame shadows every older native ref.
 }
 std::vector<CodeOwner> apc_code_until_actual_stop;
 std::shared_ptr<const NativeIdentityEventSource> boot_source;
 std::optional<Events::Origin> boot_origin;
 std::shared_ptr<const NativeIdentityEventSource> current_event_source;
 std::optional<Events::Origin> current_event_origin;
 uint32_t* native_pending=nullptr;
 uint64_t next_provider=1;
 std::array<std::shared_ptr<Provider>,64> providers;
 std::array<std::shared_ptr<CountedAllocation>,64> counted;
 std::vector<std::shared_ptr<CountedAllocation>> retired_counted;
 uint64_t next_counted=1;
 std::shared_ptr<CountedAllocation> FindCounted(uint32_t body) const {
  std::shared_ptr<CountedAllocation> found;
  for(const auto& row:counted)if(row && row->allocated && !row->failed &&
      !row->free_entered && row->body==body){if(found)return {};found=row;}
  return found;
 }
 std::shared_ptr<Provider> FindProvider(uint32_t object) const {
  for(const auto& p:providers)if(p && p->object==object && !p->destroyed)return p;
  return {};
 }
 const std::shared_ptr<identity_detail::NativeIdentityHookRundown> hook_rundown=
     std::make_shared<identity_detail::NativeIdentityHookRundown>();
 std::atomic<uint64_t> public_calls{0},external_public_calls{0};
 bool EnterPublic(bool external) {
  std::lock_guard lock(mutex);
  if(stopping.load() || retired.load() || uncertain.load() ||
     public_calls.load()==UINT64_MAX || external_public_calls.load()==UINT64_MAX)return false;
  public_calls.fetch_add(1,std::memory_order_acq_rel);
  if(external)external_public_calls.fetch_add(1,std::memory_order_acq_rel);
  return true;
 }
 void LeavePublic(bool external) noexcept {
  if(retired.load(std::memory_order_acquire))return;
  if(external)external_public_calls.fetch_sub(1,std::memory_order_acq_rel);
  public_calls.fetch_sub(1,std::memory_order_acq_rel);changed.notify_all();
 }

 bool old_ingress_closed=false;
 void Lose(uint32_t line) noexcept {
  uint32_t empty=0;first_loss_line.compare_exchange_strong(empty,line,std::memory_order_relaxed);
  uncertain.store(true,std::memory_order_release);
  if(pixel_lifetime)pixel_lifetime->closed.store(true,std::memory_order_release);
 }
 void BindTaskDiagnostic(Work& work,cpu::ppc::PPCContext& c,std::optional<uint32_t> native_task) noexcept {
  if(!task_diagnostics_enabled){task_diagnostic_unavailable.fetch_or(1,std::memory_order_relaxed);return;}
  if(task_diagnostic_used==task_diagnostic_cells.size()){
   task_diagnostic_unavailable.fetch_or(2,std::memory_order_relaxed);return;
  }
  auto& d=task_diagnostic_cells[task_diagnostic_used++];
  d.owner=this;d.context=&c;d.cpu=c.thread_state;d.kernel=c.kernel_state;
  auto& v=d.value;v.available=1;v.origin=work.frame.function;v.source=work.source;
  v.work=work.serial;v.birth=work.birth.generation;v.thread=work.frame.thread;v.pcr=work.frame.pcr;
  v.process=work.thread->native_frame_thread_identity().process_type; // immutable XThread metadata, no lock/readback
  v.entry_sp=work.frame.sp;v.entry_lr=work.frame.lr;
  v.body_sp=work.frame.sp-(v.origin==0x8199E3E0?0x190u:0xD0u);
  if(native_task){v.native_tls_task=*native_task;v.available|=2;}
  void* prior=d.cpu->native_identity_task_diagnostic_cell_.load(std::memory_order_acquire);
  if(prior && !OwnTaskDiagnostic(prior)){
   task_diagnostic_unavailable.fetch_or(4,std::memory_order_relaxed);return;
  }
  d.previous=static_cast<TaskDiagnostic*>(prior);
  if(!d.cpu->native_identity_task_diagnostic_cell_.compare_exchange_strong(prior,&d,std::memory_order_acq_rel)){
   task_diagnostic_unavailable.fetch_or(8,std::memory_order_relaxed);return;
  }
  d.bound=true;work.task_diagnostic=&d;task_diagnostic_bindings.fetch_add(1,std::memory_order_release);
 }
 void ReturnTaskDiagnostic(Work& work,cpu::ppc::PPCContext& c) noexcept {
  auto* d=work.task_diagnostic;if(!d || !d->bound)return;
  if(d->context!=&c || d->cpu!=c.thread_state){task_diagnostic_unavailable.fetch_or(16,std::memory_order_relaxed);return;}
  void* exact=d;
  if(!d->cpu->native_identity_task_diagnostic_cell_.compare_exchange_strong(exact,d->previous,std::memory_order_acq_rel)){
   task_diagnostic_unavailable.fetch_or(16,std::memory_order_relaxed);return;
  }
  d->bound=false;
 }
 void DetachTaskDiagnosticsAtActualStop() noexcept {
  // Only the existing actual-stopped lane, with these genuine active Work
  // XThread pins still held. Restore the actual per-ThreadState chain LIFO;
  // diagnostic mismatches remain observations, never new stop authority.
  for(auto it=active.rbegin();it!=active.rend();++it){
   auto* cpu=(*it)->thread?(*it)->thread->thread_state():nullptr;if(!cpu)continue;
   for(size_t n=0;n<task_diagnostic_cells.size();++n){
    void* raw=cpu->native_identity_task_diagnostic_cell_.load(std::memory_order_acquire);
    auto* d=OwnTaskDiagnostic(raw);if(!d)break;
    if(d->cpu!=cpu){task_diagnostic_unavailable.fetch_or(32,std::memory_order_relaxed);break;}
    if(!cpu->native_identity_task_diagnostic_cell_.compare_exchange_strong(raw,d->previous,std::memory_order_acq_rel)){
     task_diagnostic_unavailable.fetch_or(32,std::memory_order_relaxed);break;
    }
    d->bound=false;
   }
  }
  for(uint32_t i=0;i<task_diagnostic_used;++i){auto& d=task_diagnostic_cells[i];
   if(d.bound)task_diagnostic_unavailable.fetch_or(32,std::memory_order_relaxed);
   d.owner=nullptr;d.context=nullptr;d.cpu=nullptr;d.kernel=nullptr;d.previous=nullptr;
  }
 }
 void ReportTaskOperands(const char* boundary) const noexcept {
  try{
   const auto emit=[&](NativeSystemUiTraceLimiter& trace,const char* event,const TaskOperandCell& cell){
    const auto state=cell.state.load(std::memory_order_acquire);TaskOperands v;if(state==2)v=cell.payload;
    const auto unavailable=task_diagnostic_unavailable.load(std::memory_order_acquire);
    const auto bindings=task_diagnostic_bindings.load(std::memory_order_acquire);
    const uint64_t key=uint64_t(state)|(uint64_t(task_diagnostics_enabled)<<2)|
        (unavailable<<3)|(uint64_t(bindings)<<10);
    trace.EmitTransition(key,"identity",event,
      {{"capture_state",state},{"available",v.available},{"site",v.site},{"origin",v.origin},
       {"source",v.source},{"work",v.work},{"birth",v.birth},{"schedule",v.schedule},
       {"thread",v.thread},{"pcr",v.pcr},{"process",v.process},{"native_tls_task",v.native_tls_task},
       {"consumer_native_tls_task",v.consumer_native_tls_task},
       {"entry_sp",v.entry_sp},{"entry_lr",v.entry_lr},{"body_sp",v.body_sp},{"sp",v.sp},{"lr",v.lr},
       {"schedule_sp",v.schedule_sp},{"schedule_lr",v.schedule_lr},{"callback",v.callback},
       {"argument",v.argument},{"options",v.options},{"output",v.output},{"allocated_birth",v.allocated_birth}},
      {{"provider_object",v.provider_object},{"provider_function",v.provider_function},{"provider_ctr",v.provider_ctr},
       {"provider_return",v.provider_return},{"producer_object",v.producer_object},{"producer_word",v.producer_word},
       {"producer_options",v.producer_options},{"allocation_options",v.allocation_options},{"allocation_task",v.allocation_task},
       {"supplied_flags",v.supplied_flags},{"supplied_object",v.supplied_object},{"native_task",v.native_task},
       {"native_flags",v.native_flags},{"raw_object",v.raw_object},{"masked_object",v.masked_object},{"operand",v.operand},
       {"native_process_first",v.native_process_first},{"native_process_second",v.native_process_second},
       {"issues",v.issues},{"unavailable",unavailable},
       {"enabled",task_diagnostics_enabled},{"bindings",bindings},{"boundary",boundary && !std::strcmp(boundary,"Bootstrap")?1u:2u}});
   };
   static NativeSystemUiTraceLimiter first,null_operand;
   emit(first,"task_schedule_operands",first_task_operands);
   emit(null_operand,"task_null_schedule_operands",first_null_task_operands);
  }catch(...){/* Pure diagnostic; no source/native/admission result changes. */}
 }
 // Caller holds State::mutex. Initialization is not a read demand. A
 // later Query may also positively finish without issuing one: its actual
 // refresh branch ALWAYS increments full A0 before calling the read wrapper.
 bool ProviderImageDataAbsent(const Provider& p) const {
  return p.generation && !p.constructing && p.initialized && p.buffer &&
      ((p.capacity==0x1000 && p.type==0xB) || (p.capacity==0x4000 && p.type==0xA)) &&
      !p.revoked && !p.buffer_revoked && !p.destroyed &&
      !p.completed_source && !p.completed_version && !p.completed_status && !p.completed_empty &&
      !p.last_output && !p.image_output && !p.image_key && !p.image_key_generation;
 }
 bool ProviderNoImageHistory(const Provider& p) const {
  if(!ProviderImageDataAbsent(p))return false;
  if(!p.image_uri_generation)return !p.no_read_query; // Exact constructor/init birth, no prior Query.
  const auto& q=p.no_read_query;
  return q && q->frame.valid() && q->frame.root==root && q->frame.function==0x816921C8 &&
      q->birth.generation && q->birth.origin && q->birth.task && q->work && q->source &&
      q->provider_generation==p.generation && q->uri_generation==p.image_uri_generation;
 }
 bool ProviderWithoutIssuedImageDuty(const Provider& p) const {
  // The parent event handler may legitimately issue the Query. History
  // capture is separate from this eventual owner-quiescent acceptance.
  return ProviderNoImageHistory(p) &&
      std::none_of(active.begin(),active.end(),[&](const auto& work){
       return work->provider_generation==p.generation;
      });
 }
 const Provider* FirstUnfreshProvider(uint64_t source) const {
  // Pristine initialization or an exact returned no-read Query has no
  // issued image duty. Neither is an output completion or fresh image.
  for(const auto& p:providers)if(p && p->initialized && !p->revoked &&
      !p->buffer_revoked && !p->destroyed && p->completed_source!=source &&
      !ProviderWithoutIssuedImageDuty(*p))return p.get();
  return nullptr;
 }
 bool ProvidersFresh(uint64_t source) const {return !FirstUnfreshProvider(source);}
 bool RecycleRead(const std::shared_ptr<Work>& work) {
  if(!work->read)return true;
  auto* pool=work->read_pool;if(!pool)return false;
  const auto slot=std::find_if(pool->begin(),pool->end(),[](const auto& p){return !p;});
  if(slot==pool->end())return false;
  *slot=std::move(work->read);return true;
 }

 // Actual native words only while this exact allocated Task is still live.
 // An older dispatch may return after free/reuse; never read replacement bytes.
 std::optional<std::array<uint32_t,2>> LivePeriodicState(Tasks::Birth birth) const {
  const auto* node=tasks.FindBirthConst(birth.generation);
  if(!node || !Tasks::SameBirth(node->birth,birth) || !node->periodic_factory ||
      node->free_entered || node->freed)return {};
  const auto flags=Word(*kernel->memory(),birth.task+8),period=Word(*kernel->memory(),birth.task+0xC);
  if(!flags || !period)return {};
  return std::array<uint32_t,2>{*flags,*period};
 }
 bool PeriodicAllocationCurrent(Tasks::Birth birth,uint64_t factory) const {
  const auto words=LivePeriodicState(birth);
  return words && tasks.MatchesPeriodicAllocation(birth,factory,*words);
 }
 bool IdentityWorkEmpty(WorkBlocker* observation=nullptr) const {
  if(observation){*observation={};observation->available=1;}
  const bool empty=std::none_of(active.begin(),active.end(),[&](const auto& work){
   if(!work->physical_work && !work->execution_only){
    if(observation){
     observation->available|=2;observation->reason=1;observation->serial=work->serial;
     observation->source=work->source;observation->frame=work->frame;observation->birth=work->birth;
     if(work->birth.generation)observation->available|=4;
     observation->first_argument=work->first;
     observation->state=uint32_t(work->owns_task_work)|(uint32_t(work->owns_persistent_work)<<1)|
         (uint32_t(work->transport_unclaimed)<<2)|(uint32_t(work->creation_only)<<3)|
         (uint32_t(work->execution_options)<<4)|(uint32_t(work->normal_return)<<5)|
         (uint32_t(work->uncertain)<<6)|(uint32_t(work->prebody_parked)<<7)|
         (uint32_t(work->render_iteration)<<8)|(uint32_t(bool(work->read))<<9)|(uint32_t(bool(work->commit))<<10);
    }
    return true;
   }
   return false;
  });
  if(observation)observation->closed=empty;
  return empty;
 }
 std::shared_ptr<Work> Latest(uint32_t thread,uint32_t function=0) const {
  for(auto i=active.rbegin();i!=active.rend();++i)
   if(!(*i)->service_progress && !(*i)->normal_return && (*i)->frame.thread==thread && (!function || (*i)->frame.function==function) &&
       (function || (!(*i)->transport_unclaimed && !(*i)->creation_only && !(*i)->execution_options && !(*i)->profile_control && !(*i)->periodic_factory)))return *i;
  return {};
 }
 // This is a dependency/lifetime classification, not a promise that a native
 // consumer never updates an auth scalar. The proven initializer does not
 // replace the OLD allocations. Keep their exact scopes and genuine joins;
 // only the later data worker may replace the roster/cache resources.
 enum class ReentrantWorkConflict:uint8_t {
  kNone,kUnhealthy,kFrameLifetime,kSource,kIdentityWriter,kForeignLifetime,
  kReadIssuer,kOpaqueExecution,kBorrowedRead,kUntyped,kCallerContext,
  kCallerExecution,kCallerCell,kCallerOldData,kCallerOldBarrier,kCallerExpected,
  kCallerWake,kCallerShadow,kCallerBootstrap
 };
 ReentrantWorkConflict ReentrantWorkHealth(const Work& work) const {
  if(work.uncertain || work.execution_uncertain || work.execution_read_fallback)
   return ReentrantWorkConflict::kUnhealthy;
  if(!work.serial || !work.frame.valid() || work.frame.root!=root || !work.thread ||
     work.thread->kernel_state()!=kernel || work.thread->thread_id()!=work.frame.thread ||
     work.thread->pcr_ptr()!=work.frame.pcr || !root_code || !resident_domain ||
     (!work.domain && !work.transport_unclaimed))return ReentrantWorkConflict::kFrameLifetime;
  // Passive native routing can use the factual boot source, but it cannot
  // borrow some other published/prepared identity merely by retaining code.
  if(work.source!=current_read_source && work.source!=kBootUnjoinedSource)
   return ReentrantWorkConflict::kSource;
  if(work.commit || work.host_body || work.identity_continuation ||
     work.frame.function==0x8199C630 || work.frame.function==0x8199E3E0 ||
     work.frame.function==0x8199E148 || work.frame.function==0x8199C838 ||
     work.frame.function==0x816DC628)
   return ReentrantWorkConflict::kIdentityWriter;
  if(work.foreign_task_cleanup)return ReentrantWorkConflict::kForeignLifetime;
  // Registered/broad host execution alone is not a proved cache writer.
  if((work.registered_executor && !work.transport_only) || work.frame.function==0x81696558)
   return ReentrantWorkConflict::kOpaqueExecution;
  return ReentrantWorkConflict::kNone;
 }
 bool RetainedFixedOldRead(const Work& work) const {
  const auto* node=work.birth.generation?tasks.FindBirthConst(work.birth.generation):nullptr;
  const bool actual_task=work.birth.generation && node && Tasks::SameBirth(node->birth,work.birth) &&
      !node->freed && !node->delete_entered && !node->free_entered;
  const bool actual_system_frame=work.system_thread_frame && !work.birth.generation && !work.birth.task;
  // Every scope issued from this pool was prepared from current_ticket.
  // No Job exists at this admission cut, so no following writer ticket can
  // enter this pool. A nested/suspended scope stays owned until real Leave.
  return ReentrantWorkHealth(work)==ReentrantWorkConflict::kNone && !work.normal_return &&
      work.source==current_read_source && (actual_task || actual_system_frame) &&
      work.read && work.read_pool==&boot_reads &&
      !work.physical_work && !work.prebody_parked && !work.execution_only &&
      !work.transport_unclaimed && !work.transport_only && !work.creation_only &&
      !work.execution_options && !work.execution_read_owner && !work.execution_read.pending();
 }
 ReentrantWorkConflict ReentrantOldWorkConflict(const Work& work) const {
  const auto health=ReentrantWorkHealth(work);
  if(health!=ReentrantWorkConflict::kNone)return health;
  if(work.read)return RetainedFixedOldRead(work)?ReentrantWorkConflict::kNone:
      ReentrantWorkConflict::kReadIssuer;
  if(work.execution_read_owner || work.execution_read.pending()){
   // Only a completed inline dispatch's real Wait restoration is transport.
   // An executing arbitrary callback remains opaque even with an old scope.
   const auto& parent=work.execution_read_owner;
   const bool active_parent=parent && std::find(active.begin(),active.end(),parent)!=active.end();
   if(!work.execution_only || !work.execution_control || !work.execution_code_proven ||
      work.frame.function!=0x816BA058 ||
      work.execution_read.phase()!=Tasks::ExecutionReadCell::Phase::kTransferred ||
      !active_parent || !RetainedFixedOldRead(*parent) ||
      work.thread.get()!=parent->thread.get() || work.frame.root!=parent->frame.root ||
      work.frame.thread!=parent->frame.thread || work.frame.pcr!=parent->frame.pcr ||
      work.serial<=parent->serial || work.frame.sp>=parent->frame.sp ||
      work.execution_read.owner()!=parent.get() || work.execution_read.scope()!=parent->read.get() ||
      work.execution_read.origin()!=parent->frame || work.execution_read.restoration()!=work.frame)
    return ReentrantWorkConflict::kBorrowedRead;
   return ReentrantWorkConflict::kNone;
  }
  // Task metadata never certifies its callback. The actual dispatch/callback
  // has a distinct whole record and remains incompatible while it is opaque.
  if(work.frame.function==0x816B8B08 || work.frame.function==0x816B7DE8 ||
     (work.execution_only && (!work.execution_code_proven || !CommonTaskFunction(work.frame.function))))
   return ReentrantWorkConflict::kOpaqueExecution;
  if(CommonTaskFunction(work.frame.function))return ReentrantWorkConflict::kNone;
  if(work.transport_unclaimed){
   // The ordinary CompleteIO shadow has no identity overlap/APC authority;
   // all other neutral shadows above must be actual Task metadata calls.
   return work.frame.function==0x816A2588?ReentrantWorkConflict::kNone:
       ReentrantWorkConflict::kUntyped;
  }
  if((work.listener_frame && work.listener_arguments && ListenerOuter(work.frame.function)) ||
     work.provider_generation || work.wake || work.wake_work ||
     (work.physical_work && (work.prebody_parked || work.owns_persistent_work ||
       (work.periodic_role!=PeriodicRole::kNone && work.periodic_body_admitted))))
   return ReentrantWorkConflict::kNone;
  return ReentrantWorkConflict::kUntyped;
 }
 // Capture only the actual native Task scalar outside State. This does not
 // issue a read/source or certify a callback; the final metadata cut below
 // binds the real selected execution and every containing old-data duty.
 std::optional<uint32_t> OriginalCallerTask(XThread* caller,uint16_t ordinal) const {
  if(!caller || ActualThread()!=caller || !caller->thread_state() ||
     !caller->thread_state()->context() || global_critical_region::is_held_by_current_thread() ||
     (ordinal!=0x20B && ordinal!=0x224))return {};
  return NativeProfileIdentityTaskContext::Read(*config.catalog->calls(),*config.domain,
      ordinal==0x20B?0x8199C630u:0x8199E3E0u,*caller->thread_state()->context(),
      caller->native_frame_thread_identity().process_type==X_PROCTYPE_TITLE);
 }
 ReentrantWorkConflict NormalOriginalCaller(XThread* caller,std::optional<uint32_t> actual_task,
     uint32_t* first_function=nullptr) const {
  const auto refuse=[&](ReentrantWorkConflict why,uint32_t function=0){
   if(first_function)*first_function=function;return why;
  };
  auto* cpu=caller?caller->thread_state():nullptr;const auto* c=cpu?cpu->context():nullptr;
  if(!caller || ActualThread()!=caller || caller->kernel_state()!=kernel || !c ||
     c->kernel_state!=kernel || c->thread_state!=cpu || c->processor!=kernel->processor() ||
     !DwordIs(c->r[13],caller->pcr_ptr()) || !DwordIs(c->r[1],uint32_t(c->r[1])) ||
     (c->r[1]&15) || caller->stack_limit()>=caller->stack_base() ||
     c->r[1]<caller->stack_limit() || c->r[1]>caller->stack_base() ||
     !aligned || job || stopping.load() || retired.load() || uncertain.load() ||
     render_phase!=RenderPhase::kOpen || !current_ticket || !current_roster ||
     current_read_source==kBootUnjoinedSource)return refuse(ReentrantWorkConflict::kCallerContext);
  if(caller->thread_id()==config.allocation->bootstrap_thread_id())
   return refuse(ReentrantWorkConflict::kCallerBootstrap);
  if(cpu->native_profile_roster_read_.load(std::memory_order_acquire) ||
     cpu->native_identity_publication_cell_.load(std::memory_order_acquire) ||
     cpu->native_identity_listener_cell_.load(std::memory_order_acquire))
   return refuse(ReentrantWorkConflict::kCallerCell);
  if(unavailable_execution_frames.Shadows(root,caller->thread_id(),caller->pcr_ptr(),
      uint32_t(c->r[1]),caller->stack_base()))return refuse(ReentrantWorkConflict::kCallerShadow);
  // This common receipt covers either native dispatcher. A code-pinned
  // execution-only Task owns callback lifetime, never an old roster read.
  std::shared_ptr<Work> issuer;
  for(auto i=active.rbegin();i!=active.rend();++i){const auto& work=*i;
   if(work && !work->normal_return && work->frame.thread==caller->thread_id() &&
      (work->frame.function==0x816B8B08 || work->frame.function==0x816B7DE8)){issuer=work;break;}
  }
  if(!issuer || !actual_task || issuer->thread.get()!=caller || issuer->frame.root!=root ||
     issuer->frame.pcr!=caller->pcr_ptr() || !issuer->domain || issuer->uncertain ||
     !issuer->execution_only || !issuer->execution_code_proven || issuer->execution_uncertain ||
     issuer->source!=kBootUnjoinedSource || issuer->birth.task!=*actual_task ||
     !tasks.IsExecutionSubject(issuer->birth.generation) || !issuer->execution_dispatch ||
     !issuer->execution_lifetime || issuer->execution_lifetime->uncertain ||
     issuer->execution_lifetime->native_freed || !issuer->execution_target)
   return refuse(ReentrantWorkConflict::kCallerExecution,issuer?issuer->frame.function:0);
  const auto selected=tasks.CaptureCurrentExecution(issuer->execution_dispatch,
      caller->thread_id(),caller->pcr_ptr(),uint32_t(c->r[1]),*actual_task);
  const auto* node=tasks.FindBirthConst(issuer->birth.generation);
  const auto& target=issuer->execution_target;
  if(!selected || !Tasks::SameBirth(selected->birth,issuer->birth) || selected->dispatcher!=issuer->frame ||
     !node || node->execution_active!=issuer->execution_dispatch || selected->callback!=target->callback ||
     !target->code.module || !target->code.lease || !target->code.load_generation ||
     target->code.module->load_generation()!=target->code.load_generation)
   return refuse(ReentrantWorkConflict::kCallerExecution,issuer->frame.function);
  const auto same_frame=[&](const Frame& f){return f.root==root &&
      (f.thread==caller->thread_id() || f.pcr==caller->pcr_ptr());};
  const auto check=[&](const auto& work,bool active_record){
   if(!work)return ReentrantWorkConflict::kNone;
   const auto& cell=work->execution_read;
   const auto& parent=work->execution_read_owner;
   const bool own=work->thread.get()==caller || same_frame(work->frame);
   const bool borrowed=same_frame(cell.origin()) || same_frame(cell.restoration()) ||
       (parent && (parent->thread.get()==caller || same_frame(parent->frame)));
   if(!own && !borrowed)return ReentrantWorkConflict::kNone;
   if(own && (work->thread.get()!=caller || work->frame.root!=root ||
       work->frame.thread!=caller->thread_id() || work->frame.pcr!=caller->pcr_ptr()))
    return ReentrantWorkConflict::kFrameLifetime;
   if(work->read || work->execution_read_fallback || cell.pending() || cell.scope() ||
      (cell.owner() && (!parent || cell.owner()!=parent.get())) ||
      (parent && (parent->read || parent->execution_read_fallback || parent->execution_read.pending())))
    return ReentrantWorkConflict::kCallerOldData;
   if(!work->normal_return && (work->commit || work->identity_continuation || work->host_body))
    return ReentrantWorkConflict::kCallerOldData;
   // Mirror only the actual existing old-cut join. A returned code/ref holder
   // is not new data, while a containing ordinary native operation must end.
   if(active_record && !work->physical_work && !work->execution_only)
    return ReentrantWorkConflict::kCallerOldBarrier;
   return ReentrantWorkConflict::kNone;
  };
  for(const auto* records:{&active,&execution_retained,&host_retired})
   for(const auto& work:*records){const auto conflict=check(work,records==&active);
    if(conflict!=ReentrantWorkConflict::kNone)return refuse(conflict,work->frame.function);}
  for(const auto& call:expected)if(call && !call->call_returned &&
      (call->thread.get()==caller || (call->actual && same_frame(*call->actual))))
   return refuse(ReentrantWorkConflict::kCallerExpected,call->function);
  if(wake){
   for(const auto& row:wake->wakes_)if(row && (same_frame(row->frame) || same_frame(row->reschedule)))
    return refuse(ReentrantWorkConflict::kCallerWake,row->frame.function);
   for(const auto& row:wake->works_)if(row && (same_frame(row->frame) || same_frame(row->refresh)))
    return refuse(ReentrantWorkConflict::kCallerWake,row->frame.function);
  }
  return ReentrantWorkConflict::kNone;
 }
 // Positive ownership proof for the primary-proven local async families only.
 // Keeping the exact read/frame is essential: the outer native scene still
 // owns resources after its callback returns, through the actual CD920 cut.
 std::shared_ptr<Work> ReentrantOriginalFrame(XThread* caller,uint16_t ordinal,
     uint32_t flags,uint32_t overlap,OriginalLogonDiagnostic* diagnostic=nullptr) const {
  const auto rejected=[&](uint32_t bit,bool condition){
   if(diagnostic){diagnostic->evaluated|=1u<<bit;if(condition)diagnostic->failed|=1u<<bit;}
   return condition;
  };
  if(rejected(0,!caller || caller!=ActualThread() || (ordinal!=0x20B && ordinal!=0x224)))return {};
  const bool proven_async_flags=flags==0 || flags==4 || flags==0xC || flags==0x20 ||
      flags==0x24 || flags==0x2C || flags==0x120420 || flags==0x120424;
  if(rejected(1,!proven_async_flags || !overlap) ||
     rejected(2,bool(job) || render_phase!=RenderPhase::kOpen || !aligned || !current_ticket ||
         !current_roster || !config.factory_binding || !pixels) ||
     rejected(3,stopping.load() || retired.load() || uncertain.load()) ||
     rejected(11,!expected.empty()))return {};
  auto* cpu=caller->thread_state();const auto* c=cpu?cpu->context():nullptr;
  const auto work=Latest(caller->thread_id());
  if(rejected(4,!c || caller->kernel_state()!=kernel || c->kernel_state!=kernel ||
         c->thread_state!=cpu || c->r[13]!=caller->pcr_ptr()) ||
     rejected(5,!work || !work->render_iteration || work->frame.function!=0x816CD6E8 ||
         work->frame.root!=root || work->frame.thread!=caller->thread_id() ||
         work->frame.pcr!=caller->pcr_ptr() || work->thread.get()!=caller ||
         work->source!=current_read_source) ||
     rejected(7,!work->domain || work->normal_return || work->uncertain || work->physical_work ||
         work->execution_only || work->execution_read_owner || work->execution_read_fallback) ||
     rejected(6,!work->read || work->read_pool!=&boot_reads || work->presentation_guard!=pixels ||
         xam::ProfileRosterNativeReadScope::FromActualThread(kernel,cpu)!=work->read.get()))return {};
  const auto* node=tasks.FindBirthConst(work->external_birth.generation);
  if(rejected(8,!node || !Tasks::SameBirth(node->birth,work->external_birth) ||
         !Tasks::SameBirth(work->birth,work->external_birth) || node->freed || node->free_entered) ||
     rejected(9,c->r[1]>UINT32_MAX || (c->r[1]&15) || caller->stack_limit()>=caller->stack_base() ||
         c->r[1]<caller->stack_limit() || c->r[1]>work->frame.sp ||
         work->frame.sp>caller->stack_base()))return {};
  // Keep both original old-source mutation-descendant checks in Enqueue.
  // A retained fixed-old consumer or pure transport is not a destructive
  // publisher. Its full read/source/listener/paint lifetime still joins before
  // data_worker_ready; no scope, native result or callback is shortened here.
  bool other_owner=false;
  for(const auto& other:active){
   if(!other || other==work || other->normal_return)continue;
   const auto conflict=ReentrantOldWorkConflict(*other);
   if(conflict==ReentrantWorkConflict::kNone)continue;
   other_owner=true;
   // One representative at the actual API cut, never a per-frame census.
   // Existing kind/role/caller/stack metadata in bits0..15 is unchanged.
   if(diagnostic)diagnostic->latest_kind=(diagnostic->latest_kind&0xFFFFu) |
       (uint32_t(conflict)<<16) | (OriginalLogonFunctionRole(other->frame.function)<<24);
   break;
  }
  if(rejected(10,other_owner))return {};
  return work;
 }
 bool AwaitPreparedWorker(cpu::ppc::PPCContext&,uint32_t) noexcept;
 uint64_t EarlierSource(uint32_t thread) const {
  const auto prior=Latest(thread);return prior?prior->source:kBootUnjoinedSource;
 }
 std::shared_ptr<Work> EarlierListenerParent(uint32_t thread) const {
  for(auto i=active.rbegin();i!=active.rend();++i)
   if(!(*i)->service_progress && (*i)->frame.thread==thread && (*i)->listener_work)return *i;
  return {};
 }
 std::optional<Events::Work> EarlierListenerWork(uint32_t thread) const {
  const auto parent=EarlierListenerParent(thread);
  return parent?parent->listener_work:std::nullopt;
 }
 std::shared_ptr<Work> SynchronousListenerParent(const Work& child,const Events::WorkKey& key) const {
  const auto parent=EarlierListenerParent(child.frame.thread);
  // Inspect only the innermost earlier owner. A scheduler callback on a
  // different actual Task, or a tail/equal stack frame, remains independent;
  // never skip it to borrow an older frame that happens to have our source.
  if(!parent || parent->thread.get()!=child.thread.get() || parent->normal_return || parent->uncertain ||
     parent->frame.root!=child.frame.root || parent->frame.pcr!=key.pcr ||
     (parent->system_thread_frame && !parent->external_birth.generation?Events::WorkOwnerKind::kSystemThreadFrame:Events::WorkOwnerKind::kTask)!=key.owner_kind ||
     parent->birth.generation!=key.task_allocation_generation || parent->birth.task!=key.task ||
     parent->frame.sp<=key.sp || parent->serial>=key.invocation)return {};
  return parent;
 }
 // The innermost deleting frame decides this call. A neutral nested
 // destructor cannot borrow an older owned frame or its allocation generation.
 std::shared_ptr<Work> TaskFreeParent(uint32_t thread) const {
  for(auto i=active.rbegin();i!=active.rend();++i){
   const auto& work=*i;
   if(work->frame.thread==thread &&
      (work->frame.function==0x816B7D98 || work->frame.function==0x816B7500))
    return !work->transport_unclaimed && tasks.OwnsDeletingFrame(work->frame)?work:nullptr;
  }
  return {};
 }
 std::shared_ptr<ExecutionLifetime> ExecutionLifetimeFor(Tasks::Birth birth) const {
  for(const auto& item:execution_lifetimes)if(item && Tasks::SameBirth(item->birth,birth))return item;
  return {};
 }
 std::shared_ptr<ExecutionTarget> RetainExecutionTarget(uint32_t callback) const {
  if(callback<4 || (callback&3))return {};
  auto target=std::make_shared<ExecutionTarget>();
  auto read=global_critical_region::AcquireDirect();
  auto module=kernel->GetUserModuleForAddress(callback);xex2_section_type section;
  if(!module || !module->xex_module() || !module->load_generation() ||
     !module->xex_module()->GetPageSectionType(callback,&section) || section!=XEX_SECTION_CODE ||
     module->xex_module()->TranslateCode(callback)!=kernel->memory()->TranslateVirtual<const uint8_t*>(callback) ||
     kernel->memory()->QueryRangeAccess(callback,4)==memory::PageAccess::kNoAccess ||
     !config.domain->Contains({module->hmodule_ptr(),module->load_generation()}))return {};
  auto lease=kernel->AcquireUserModuleExecutionLease(module);if(!lease)return {};
  target->callback=callback;
  target->code={module,std::move(lease),module->load_generation()};return target;
 }
 std::shared_ptr<ExecutionTarget> ExecutionTargetFor(
     const std::shared_ptr<ExecutionLifetime>& lifetime,uint32_t callback) const {
  if(!lifetime || lifetime->uncertain)return {};
  for(const auto& target:lifetime->targets)if(target && target->callback==callback &&
      target->code.module && target->code.lease && target->code.load_generation &&
      target->code.module->load_generation()==target->code.load_generation)return target;
  return {};
 }
 // A resource/unavailable neutral observer may suppress an older ambient
 // read only through its ALREADY active real scope owner. That Work remains
 // strongly held in active until its genuine own-return or actual CPU-stop.
 // Restoration immediately followed by Leave grants no child read access.
 void SuspendAmbientOnUnavailable(cpu::ppc::PPCContext& c,uint32_t thread) noexcept {
  if(!c.thread_state)return;
  void* current=c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire);
  if(!current)return;
  for(const auto& parent:active)if(parent && parent->frame.thread==thread &&
      !parent->normal_return && parent->read.get()==current){
   if(Tasks::SuppressReadCellForScopeLeave(parent->execution_read_fallback,
       current,c.thread_state->native_profile_roster_read_))parent->uncertain=true;
   return;
  }
 }
 bool RestoreOwnedReadOnUnwind(const std::shared_ptr<Work>& work) noexcept {
  if(!work->execution_read_fallback)return true;
  if(!work->read || !work->thread || !work->thread->thread_state())return false;
  return Tasks::RestoreReadCellForScopeLeave(work->execution_read_fallback,
      work->read.get(),work->thread->thread_state()->native_profile_roster_read_);
 }
 void MarkNeutralUnavailable(Frame frame,cpu::ppc::PPCContext& c) noexcept {
  execution_unavailable.store(true,std::memory_order_release);
  unavailable_execution_frames.Record(frame);
  if(frame.function==0x816B8B08 || frame.function==0x816B7DE8)
   SuspendAmbientOnUnavailable(c,frame.thread);
 }
 struct TaskClaim {
  bool transport=false,relevant=true,unsupported_descendant=false;
  uint64_t subject=0,parent=0,neutral_parent=0,release_receipt=0;
  bool retained_cleanup=false,title_task_lifetime=false,creation_only=false,execution_options=false,protected_scope=false;
  bool reference_only=false,profile_control=false,profile_producer=false;
  uint64_t control_creator=0,control_parent=0;
  uint64_t creation_parent=0,periodic_factory=0;PeriodicRole periodic_role=PeriodicRole::kNone;
  bool operator==(const TaskClaim&) const=default;
 };
 // Caller holds only State metadata. Release it before caller/module/GCRC
 // resolution, then recheck this exact subject/parent at actual admission.
 TaskClaim ClaimTaskTransport(uint32_t at,const cpu::ppc::PPCContext& c,uint32_t thread,
     std::optional<uint32_t> periodic_handle={}) const {
  TaskClaim claim;claim.transport=true;
  const auto parent=[&](uint32_t function){return Latest(thread,function);};
  const auto protected_parent=[&]{
   for(auto i=active.rbegin();i!=active.rend();++i){const auto& work=*i;
    if(!work || work->frame.thread!=thread || work->service_progress)continue;
    if(work->execution_only)return false;
    if(!work->transport_unclaimed && !work->creation_only && !work->execution_options && !work->profile_control && !work->periodic_factory &&
       (work->read || work->commit || (job && job->native_entered && work->source==job->source)))return true;
   }
   return false;
  };
  const auto subject=tasks.ObservedTransportSubject(uint32_t(c.r[3]));
  if(subject)claim.subject=subject->generation;
  const auto common_start=[&]()->std::shared_ptr<Work>{
   const auto work=parent(0x816A2CF0);
   return work && work->creation_only && work->creation?work:nullptr;
  };
  const auto shape=[&]()->Frame{
   if(!DwordIs(c.r[1],uint32_t(c.r[1])) || !DwordIs(c.lr,uint32_t(c.lr)) ||
       !DwordIs(c.r[13],uint32_t(c.r[13])))return {};
   return {root,thread,uint32_t(c.r[13]),uint32_t(c.r[1]),uint32_t(c.lr),at};
  };
  const auto* native=ActualThread();const bool system=native && native->native_frame_thread_identity().process_type==X_PROCTYPE_SYSTEM;
  const auto periodic_subject=subject?tasks.FindBirthConst(subject->generation):nullptr;
  const auto periodic_parent=[&](uint32_t function)->std::shared_ptr<Work>{
   const auto value=parent(function);return value && value->periodic_factory && value->periodic_body_admitted?value:nullptr;
  };
  if(at==0x8190A358){
   const auto* row=PeriodicObject(periodic_object);
   const auto live=row && row->enabled && !row->stopped?tasks.ObservedTransportSubject(row->birth.task):std::optional<Tasks::Birth>{};
   claim.relevant=system && periodic_object && config.catalog->periodic_service_stop() &&
       DwordIs(c.r[3],periodic_object) && periodic_handle &&
       (row && row->enabled && !row->stopped ?
         *periodic_handle==(row->birth.task|1u) && LivePeriodic(*row,live):*periodic_handle==0);
   if(claim.relevant){claim.periodic_factory=row?row->serial:0;claim.periodic_role=PeriodicRole::kFactory;}
   claim.unsupported_descendant=!claim.relevant;return claim;
  }
  if(at==0x819068D0){
   const auto* row=system?PeriodicObject(uint32_t(c.r[3])):nullptr;
   const auto live=row && row->enabled && !row->stopped?tasks.ObservedTransportSubject(row->birth.task):std::optional<Tasks::Birth>{};
   claim.relevant=row && DwordIs(c.r[3],row->object) && row->enabled && !row->stopped &&
       periodic_handle && *periodic_handle==(row->birth.task|1u) && LivePeriodic(*row,live);
   if(claim.relevant){claim.periodic_factory=row->serial;claim.periodic_role=PeriodicRole::kStop;}
   claim.unsupported_descendant=!claim.relevant && DwordIs(c.r[3],periodic_object) &&
       (!periodic_handle || *periodic_handle || (row && row->enabled && !row->stopped));
   return claim; // Only a genuine initial/stopped no-handle stop is unowned.
  }
  if(at==0x816B8B08){
   const auto wait=parent(0x816BA058);
   if(!periodic_subject || !periodic_subject->periodic_factory){
    const auto f=shape();
    claim.relevant=system && subject && periodic_subject && !periodic_subject->delete_entered &&
        !periodic_subject->free_entered && !periodic_subject->freed && DwordIs(c.r[3],subject->task) &&
        wait && wait->profile_control && wait->profile_control->body_admitted &&
        wait->profile_control->kind==Tasks::ProfileControl::Kind::kWait &&
        Tasks::SameBirth(wait->profile_control->target,*subject) &&
        f.valid() && f.lr==0x816BA0DC && wait->frame.sp>=0x80 && f.sp==wait->frame.sp-0x80;
    if(claim.relevant){claim.profile_control=true;claim.control_parent=wait->serial;
      claim.control_creator=wait->control_creator?wait->control_creator->serial:0;}
    claim.unsupported_descendant=wait && wait->profile_control && !claim.relevant;
    return claim;
   }
   claim.relevant=system && periodic_subject && periodic_subject->periodic_factory &&
       Periodic(periodic_subject->periodic_factory) && DwordIs(c.r[3],periodic_subject->birth.task);
   if(claim.relevant){claim.periodic_factory=periodic_subject->periodic_factory;claim.periodic_role=PeriodicRole::kDispatcher;}
   claim.unsupported_descendant=periodic_subject && periodic_subject->periodic_factory && !claim.relevant;
   return claim;
  }
  if(at==0x816BA440){
   const auto factory=periodic_parent(0x8190A358);
   const auto* row=factory?Periodic(factory->periodic_factory):nullptr;
   if(row){
    claim.relevant=system && DwordIs(c.r[3],uint32_t(c.r[3])) && DwordIs(c.r[4],uint32_t(c.r[4])) &&
        DwordIs(c.r[5],uint32_t(c.r[5])) && DwordIs(c.r[6],uint32_t(c.r[6])) &&
        row->MatchesSchedule(shape(),uint32_t(c.r[3]),uint32_t(c.r[4]),uint32_t(c.r[5]),uint32_t(c.r[6]));
    claim.unsupported_descendant=!claim.relevant;
    if(claim.relevant){claim.periodic_factory=row->serial;claim.periodic_role=PeriodicRole::kTransport;}
    return claim;
   }
  }
  if(at==0x816B9F08 || at==0x816BA058){
   const auto stop=periodic_parent(0x819068D0);const auto* row=stop?Periodic(stop->periodic_factory):nullptr;
   claim.relevant=system && row && DwordIs(c.r[3],row->birth.task) && LivePeriodic(*row,subject) &&
       row->MatchesStopChild(shape(),at,uint32_t(c.r[3]));
   if(claim.relevant){claim.periodic_factory=row->serial;claim.periodic_role=at==0x816B9F08?PeriodicRole::kCancel:PeriodicRole::kWait;}
   if(row){claim.unsupported_descendant=!claim.relevant;return claim;}
   const auto* producer=subject?tasks.ProfileProducerFor(*subject):nullptr;
   const auto f=shape();
   if(system && subject && DwordIs(c.r[3],subject->task) && producer &&
       producer->published && producer->schedule_returned && producer->schedule_succeeded &&
       producer->native_bound && !producer->owner_close_entered && f.valid() && f.sp==producer->schedule.sp){
    for(const auto& owner:active)if(owner && !owner->normal_return &&
        producer->MatchesOwner(owner->serial,owner->frame) && owner->source==subject->origin &&
        owner->frame.thread==thread && owner->frame.pcr==f.pcr){
      claim.relevant=true;claim.profile_control=true;claim.control_creator=owner->serial;break;
    }
   }
   if(!claim.relevant && at==0x816B9F08 && system && subject && periodic_subject &&
       !periodic_subject->periodic_factory && !periodic_subject->delete_entered &&
       !periodic_subject->free_entered && !periodic_subject->freed){
    const auto cancel=parent(at);
    if(cancel && cancel->profile_control && cancel->profile_control->body_admitted &&
        cancel->profile_control->kind==Tasks::ProfileControl::Kind::kCancel &&
        f.valid() && f.lr==0x816B9FE0 && cancel->frame.sp>=0x80 && f.sp==cancel->frame.sp-0x80 &&
        DwordIs(c.r[3],subject->task) && DwordIs(c.r[30],subject->task) &&
        DwordIs(c.r[31],cancel->profile_control->target.task)){
     claim.relevant=true;claim.profile_control=true;claim.control_parent=cancel->serial;
     claim.control_creator=cancel->control_creator?cancel->control_creator->serial:0;
    }
   }
   claim.unsupported_descendant=!claim.relevant && (protected_parent() ||
       (periodic_subject && periodic_subject->periodic_factory));return claim;
  }
  if(at==0x816B7600){
   const auto dispatch=periodic_parent(0x816B8B08);
   if(dispatch && c.lr==0x816B8B84 && DwordIs(c.r[3],dispatch->birth.task)){
    claim.relevant=system;claim.parent=dispatch->serial;claim.periodic_factory=dispatch->periodic_factory;
    claim.periodic_role=PeriodicRole::kBoundary;return claim;
   }
  }
  if(at==0x816B7490){
   const auto stop=periodic_parent(0x819068D0);const auto* row=stop?Periodic(stop->periodic_factory):nullptr;
   if(row && row->MatchesStopChild(shape(),at,uint32_t(c.r[3])) && !LivePeriodic(*row,subject)){
    claim.relevant=false;claim.unsupported_descendant=true;return claim;
   }
  }
  if(at==0x816B9AA8 && periodic_subject && periodic_subject->periodic_factory){
   // This class has no native Modify producer. No generic options lane may
   // silently retag its fixed nonserial periodic classification.
   claim.relevant=false;claim.unsupported_descendant=true;return claim;
  }
  switch(at){
   case 0x816A2CF0:{
    const auto* actual=ActualThread();
    claim.relevant=actual && actual->native_frame_thread_identity().process_type==X_PROCTYPE_SYSTEM &&
        protected_parent();
    claim.creation_only=claim.relevant;break;
   }
   case 0x816A2A48:{
    const auto start=common_start();claim.relevant=bool(start);
    if(start){claim.creation_parent=start->serial;claim.creation_only=true;}break;
   }
   case 0x816B9D68:{
    const auto start=common_start();const auto f=shape();
    claim.relevant=start && DwordIs(c.r[3],0x816A2C80) &&
        DwordIs(c.r[4],uint32_t(c.r[4])) && DwordIs(c.r[5],uint32_t(c.r[5])) &&
        start->creation->MatchesInline(f,uint32_t(c.r[3]),uint32_t(c.r[4]),uint32_t(c.r[5]));
    if(claim.relevant){claim.creation_parent=start->serial;claim.creation_only=true;}
    claim.unsupported_descendant=!claim.relevant && protected_parent();break;
   }
   case 0x816BA440:{
    const auto callback=Tasks::ClassifyCallback(c.r[3]);
    claim.relevant=callback!=Tasks::CallbackClaim::kOutside;
    if(c.lr==0xBCBCBCBC && callback==Tasks::CallbackClaim::kCanonicalProfile){
     bool expected_call=false;
     for(const auto& e:expected)if(e && !e->entered && !e->call_returned && e->function==at &&
         e->thread && e->thread->thread_id()==thread){
      bool args=true;for(uint32_t i=0;i<e->count;++i)args=args && DwordIs(c.r[i+3],e->args[i]);
      expected_call=expected_call || args;
     }
     claim.relevant=expected_call;
    }
    const auto creator=Latest(thread);
    claim.profile_producer=system && claim.relevant && creator && creator->source>2 &&
        !creator->physical_work && Tasks::ProfileProducerFrameBytes(creator->frame.function) && protected_parent();
    if(claim.profile_producer)claim.control_creator=creator->serial;
    claim.unsupported_descendant=callback==Tasks::CallbackClaim::kMalformedProfile ||
        (!claim.relevant && protected_parent());break;
   }
   case 0x816BA668:
    claim.relevant=c.lr==0x816F4568 || c.lr==0x816F45B0;
    claim.unsupported_descendant=!claim.relevant && protected_parent();break;
   case 0x816B9A18:{
    const auto schedule=c.lr==0x816BA688?parent(0x816BA668):
        c.lr==0x816BA46C?parent(0x816BA440):nullptr;
    if(schedule && schedule->schedule)claim.parent=schedule->serial;
    if(schedule && schedule->transport_unclaimed && !schedule->schedule)claim.neutral_parent=schedule->serial;
    // The third native producer is synchronous common Start, not Schedule.
    // Only its retained actual copier/inline scope can explain this child;
    // a known LR, SYSTEM worker, or protected ancestor alone cannot.
    const auto inlined=c.lr==0x816B9E08?parent(0x816B9D68):nullptr;
    if(inlined && inlined->creation_only && inlined->creation_parent &&
        inlined->creation_parent->creation &&
        DwordIs(c.r[3],uint32_t(c.r[3])) && DwordIs(c.r[4],uint32_t(c.r[4])) &&
        inlined->creation_parent->creation->MatchesAllocation(shape(),uint32_t(c.r[3]),uint32_t(c.r[4]))){
     claim.creation_parent=inlined->serial;claim.creation_only=true;
    }
    // Detached actual outputs remain execution-only provider prerequisites.
    claim.unsupported_descendant=!claim.parent && !claim.creation_parent && protected_parent();break;
   }
   case 0x816B9BB8:{
    const auto schedule=parent(0x816BA440);
    claim.relevant=schedule && schedule->schedule;
    if(claim.relevant)claim.parent=schedule->serial;
    const auto incoming=Tasks::ClassifyCallback(c.r[4]);
    claim.unsupported_descendant=incoming==Tasks::CallbackClaim::kMalformedProfile ||
        (!claim.relevant && (incoming!=Tasks::CallbackClaim::kOutside ||
         (subject && tasks.IsProfileTransportSubject(subject->generation)) || protected_parent()));
    break;
   }
   case 0x816B9AA8:{
    claim.relevant=c.lr==0x816F458C || c.lr==0x816F45D4;
    const auto mask=uint32_t(c.r[4]);
    const auto fields=Tasks::DecodeModifyFields(mask);
    const auto incoming=Tasks::ClassifyModifyCallback(mask,c.r[5]);
    if(!claim.relevant && fields.options_only()){
     claim.protected_scope=protected_parent();
     const auto* actual=ActualThread();
     const auto process=actual?actual->native_frame_thread_identity().process_type:0;
     if(tasks.MayClaimExecutionOnlyOptions(process,mask,claim.protected_scope,subject?subject->generation:0)){
      claim.relevant=true;claim.execution_options=true;
     }
    }
    claim.unsupported_descendant=incoming==Tasks::CallbackClaim::kMalformedProfile ||
        (!claim.relevant && (incoming!=Tasks::CallbackClaim::kOutside ||
         ((mask&7) && ((subject && tasks.IsProfileTransportSubject(subject->generation)) ||
             (fields.options_only()?claim.protected_scope:protected_parent())))));
    break;
   }
   case 0x816B7490:
    claim.relevant=bool(subject);
    claim.title_task_lifetime=subject && tasks.IsForeignAvatarTransportSubject(subject->generation);
    if(subject)for(const auto& e:expected)if(e && e->task_release && !e->entered && !e->call_returned &&
        e->thread && e->thread->thread_id()==thread && e->function==at &&
        e->task_release->phase==Tasks::CloseReceipt::Phase::kIssued &&
        e->task_release->birth.generation==subject->generation && DwordIs(c.r[3],subject->task)){
     claim.retained_cleanup=true;claim.release_receipt=e->serial;break;
    }
    if(DwordIs(c.r[3],uint32_t(c.r[3])))for(const auto& e:expected)
     if(e && e->reference_release && !e->entered && !e->call_returned && e->function==at &&
        e->thread && e->thread->thread_id()==thread && e->args[0]==uint32_t(c.r[3]) &&
        e->reference_release->receipt.state()==Tasks::ReferenceReceipt::Phase::kReleaseIssued){
      // The only no-Birth permission is this previously acquired reference.
      claim.relevant=true;claim.retained_cleanup=true;claim.release_receipt=e->serial;
      claim.reference_only=e->reference_only;break;
     }
    break;
   case 0x816B7D98:case 0x816B7500:{
    const auto reference_parent=parent(0x816B7490);
    if(reference_parent && reference_parent->reference_only){
     claim.reference_only=true;claim.parent=reference_parent->serial;
     claim.unsupported_descendant=true;break; // Nonfinal baseline contradicts zero-ref destruction.
    }
    claim.relevant=bool(subject);
    const auto close=parent(0x816B7490);
    if(subject && close && !close->transport_unclaimed && close->foreign_avatar_task &&
        close->first==subject->task && close->release_generation==subject->generation &&
        tasks.IsForeignAvatarTransportSubject(subject->generation)){
     claim.title_task_lifetime=true;claim.parent=close->serial;
    }
    if(subject && close && !close->transport_unclaimed && close->task_release &&
        close->first==subject->task && close->release_generation==subject->generation){
     claim.retained_cleanup=true;claim.release_receipt=close->release_receipt;claim.parent=close->serial;
    }
    break;
   }
   case 0x816E6AB0:{
    const auto owned=TaskFreeParent(thread);
    claim.relevant=bool(owned);if(owned){claim.parent=owned->serial;
     claim.title_task_lifetime=owned->foreign_avatar_task;
     if(owned->task_release){claim.retained_cleanup=true;claim.release_receipt=owned->release_receipt;}}
    break;
   }
   case 0x816B7B68:{
    const auto wake=parent(0x816F25E0);claim.relevant=wake && wake->wake;
    if(claim.relevant)claim.parent=wake->serial;break;
   }
   case 0x816B7600:{
    const auto wake=parent(0x816B7B68);claim.relevant=wake && wake->wake;
    if(claim.relevant)claim.parent=wake->serial;break;
   }
   default:claim.transport=false;claim.relevant=true;claim.subject=0;break;
  }
  if(system){
   uint64_t periodic=periodic_subject?periodic_subject->periodic_factory:0;
   if(!periodic && claim.parent)for(const auto& work:active)if(work->serial==claim.parent)periodic=work->periodic_factory;
   if(periodic && Periodic(periodic) && (at==0x816B9A18 || at==0x816B9BB8 || at==0x816B7490 ||
       at==0x816B7D98 || at==0x816B7500 || at==0x816E6AB0)){
    claim.periodic_factory=periodic;claim.periodic_role=PeriodicRole::kTransport;
    const auto stop=periodic_parent(0x819068D0);const auto* row=Periodic(periodic);
    if(at==0x816B7490 && stop && stop->periodic_factory==periodic &&
        row->MatchesStopChild(shape(),at,uint32_t(c.r[3])))claim.periodic_role=PeriodicRole::kOwnerClose;
   }
  }
  return claim;
 }
 // Caller holds State only. This grants context lifetime, never a reference,
 // native result, allocation or profile-read/source. Entry is still checked by
 // the existing full family guard and exact native allocation/return ledger.
 std::optional<Tasks::PeriodicFrameContext> PeriodicContextForEntry(
     const Frame& frame,const TaskClaim& claim) const {
  if(!claim.relevant || claim.unsupported_descendant || !claim.periodic_factory)return {};
  const auto* factory=Periodic(claim.periodic_factory);if(!factory)return {};
  const auto* node=claim.subject?tasks.FindBirthConst(claim.subject):nullptr;
  if(frame.function==0x819068D0)node=tasks.FindBirthConst(factory->birth.generation);
  if(node)if(auto context=tasks.RetainPeriodicSubjectContext(frame,*factory,node->birth))return context;
  const uint32_t parent_function=frame.function==0x816BA440?0x8190A358u:
      (frame.function==0x816B9A18 || frame.function==0x816B9BB8)?0x816BA440u:0u;
  const auto parent=parent_function?Latest(frame.thread,parent_function):nullptr;
  if(!parent || parent->periodic_factory!=factory->serial || !parent->periodic_body_admitted ||
      parent->normal_return || !parent->periodic_context ||
      !parent->periodic_context->Matches(parent->frame,factory->serial))return {};
  return Tasks::RetainPeriodicProducerChild(frame,*parent->periodic_context);
 }
 ModifyOptionsPayload ModifyOptionsSnapshot(const cpu::ppc::PPCContext& c,
     const TaskClaim& claim,uint32_t stage,uint32_t line) const noexcept {
  ModifyOptionsPayload d;d.available=1;d.stage=stage;d.line=line;
  if(const auto* thread=ActualThread()){d.thread=thread->thread_id();d.process=thread->native_frame_thread_identity().process_type;}
  d.task=c.r[3];d.mask=c.r[4];d.options=c.r[7];d.sp=c.r[1];d.lr=c.lr;d.pcr=c.r[13];
  d.protected_scope=claim.protected_scope;
  if(const auto subject=tasks.ObservedTransportSubject(uint32_t(c.r[3]))){
   d.available|=2;d.origin=subject->origin;d.generation=subject->generation;
   d.execution_only=tasks.IsExecutionOnlyOptionsSubject(subject->generation);
   d.profile=tasks.IsProfileTransportSubject(subject->generation);d.available|=4;
   for(const auto& node:tasks.nodes_)if(node && node->birth.generation==subject->generation){
    d.node_flags=uint32_t(node->created_persistent)|(uint32_t(node->resident_physical)<<1)|
        (uint32_t(node->transport_only)<<2)|(uint32_t(node->continuation_ticket)<<3)|
        (uint32_t(node->foreign_avatar)<<4);break;
   }
  }
  return d;
 }
 void ReportModifyOptionsOnHost() const noexcept try {
  const auto emit=[](NativeSystemUiTraceLimiter& trace,const char* event,const ModifyOptionsCell& cell){
   const auto state=cell.state.load(std::memory_order_acquire);ModifyOptionsPayload d;
   if(state==2)d=cell.payload; // Never read incomplete state1 payload.
   trace.EmitTransition(state,"identity",event,
    {{"capture_state",state},{"available",d.available},{"stage",d.stage},{"line",d.line},
     {"thread",d.thread},{"process",d.process},{"task",d.task},{"mask",d.mask},{"options",d.options},{"sp",d.sp},{"lr",d.lr},
     {"pcr",d.pcr},{"origin",d.origin},{"generation",d.generation},{"node_flags",d.node_flags},
     {"evaluated",d.evaluated},{"predicate_true",d.predicate_true}},
    {{"old_flags",d.old_flags},{"old_data",d.old_data},{"new_flags",d.new_flags},{"new_data",d.new_data},
     {"execution_only",d.execution_only},{"profile",d.profile},{"protected_scope",d.protected_scope},
     {"abi_eligible",d.abi_eligible},{"own_return_matched",d.own_return_matched},{"raw_return_residue",d.raw_return_residue}});
  };
  static NativeSystemUiTraceLimiter claim,admission,returned;
  emit(claim,"task_modify_options_claim",first_modify_options_claim);
  emit(admission,"task_modify_options_admission",first_modify_options_admission);
  emit(returned,"task_modify_options_return",first_modify_options_return);
 }catch(...){/* Diagnostic only; no return/admission/ownership change. */}
 std::optional<Frame> TransportShape(cpu::ppc::PPCContext& c,uint32_t function) const {
  auto* thread=ActualThread();
  if(!thread || thread->kernel_state()!=kernel || c.kernel_state!=kernel || c.processor!=kernel->processor() ||
     c.thread_state!=thread->thread_state() || thread->thread_state()->context()!=&c ||
     c.r[13]!=thread->pcr_ptr() || c.r[1]>UINT32_MAX || (c.r[1]&15) ||
     c.lr>UINT32_MAX || c.lr<4 || (c.lr&3))return {};
  const auto process=thread->native_frame_thread_identity().process_type;
  if(process!=X_PROCTYPE_SYSTEM && process!=X_PROCTYPE_TITLE)return {};
  Frame frame{root,thread->thread_id(),thread->pcr_ptr(),uint32_t(c.r[1]),uint32_t(c.lr),function};
  return frame.valid()?std::optional<Frame>(frame):std::nullopt;
 }
 bool OriginalTitleTaskDescendant(const XThread* thread,uint32_t function) const {
  switch(function){
   case 0x816BA440:case 0x816B9A18:case 0x816B9BB8:
   case 0x816B7490:case 0x816B7D98:case 0x816B7500:case 0x816E6AB0:break;
   default:return false;
  }
  std::lock_guard lock(mutex);
  const auto parent=thread?Latest(thread->thread_id()):nullptr;
  return job && job->native_entered && !job->execution_uncertain &&
      job->original_caller.get()==thread && parent && parent->source==job->source;
 }
 // This is API delivery policy for verified non-reading SYSTEM branches,
 // not a foreign callback/read/Task owner. Only actual slow caller admission
 // and own-return invoke it, outside State and native listener locks.
 bool DiscardedIdentityHintCaller(const Work& work,
     const Listeners::OuterArguments& args,uint32_t stage) const {
  struct Consumer {
   uint64_t module_hash;
   uint32_t base,image_bytes,body,body_bytes,return_lr,import_thunk;
   const char* body_sha256;
  };
  static constexpr Consumer consumers[]={
   {0xA4E817FC8CE261F3ull,0x91240000,0x4F000,0x9124F010,104,0x9124F05C,0x912853EC,
    "9c0dce274b1aeeb0494fe461dca1470eb1b3e5283ddb5e77d549452f776708d3"},
   {0x83B2411DBA4B3FA5ull,0x913E0000,0x49000,0x913F0F00,200,0x913F0F30,0x913FD6D4,
    "1a58ef30b034936f4435078eac3c481a83901dcf64abcf54a89218bb170bc33f"},
  };
  const auto* profile=std::find_if(std::begin(consumers),std::end(consumers),
      [&](const auto& p){return work.frame.lr==p.return_lr;});
  if(profile==std::end(consumers))return false; // No ordinary/unrelated API log.
  struct Observation {
   uint32_t role=0,stage=0,first_failure=UINT32_MAX;
   uint64_t evaluated=0,truth=0,work=0;
   bool authorized=false;
  } observed;
  observed.stage=stage;observed.work=work.serial;
  if(work.thread && work.frame.function==0x816A8C00 &&
     work.thread->native_frame_thread_identity().process_type==X_PROCTYPE_SYSTEM)
   observed.role=uint32_t(profile-std::begin(consumers))+1;
  struct Report {
   Observation& value;
   ~Report() {
    // Declared BEFORE the later GCRC guard: reporting follows its unlock.
    if(!value.role || value.stage>1 || !cvars::native_system_ui_trace)return;
    static std::array<NativeSystemUiTraceLimiter,4> traces;
    const uint64_t key=(uint64_t(value.first_failure)<<32)^value.evaluated^value.truth;
    traces[(value.role-1)*2+value.stage].EmitTransition(key,"identity","discarded_hint_caller_policy",
      {{"policy_role",value.role},{"policy_stage",value.stage},{"work",value.work},
       {"evaluated",value.evaluated},{"predicate_true",value.truth},
       {"first_failed_predicate",value.first_failure},{"authorized",value.authorized}});
   }
  } report{observed};
  const auto check=[&](uint32_t bit,bool actual){
   observed.evaluated|=uint64_t(1)<<bit;
   if(actual)observed.truth|=uint64_t(1)<<bit;
   else if(observed.first_failure==UINT32_MAX)observed.first_failure=bit;
   return actual;
  };
  const auto& code=work.caller_code;const auto module=code.module;
  if(!check(0,kernel && config.domain && config.catalog && work.thread && work.domain) ||
     !check(1,work.frame.root==root && work.frame.function==0x816A8C00 &&
         work.thread->native_frame_thread_identity().process_type==X_PROCTYPE_SYSTEM) ||
     !check(2,module && code.lease && code.load_generation) ||
     !check(3,module->module_format()==UserModule::kModuleFormatXex) ||
     !check(4,module->xex_module()!=nullptr) ||
     !check(5,module->is_system_app_module()) || !check(6,bool(module->hash())) ||
     !check(7,*module->hash()==profile->module_hash) ||
     !check(8,module->xex_module()->base_address()==profile->base) ||
     !check(9,module->xex_module()->image_size()==profile->image_bytes) ||
     !check(10,code.lease->hmodule()==module->hmodule_ptr()) ||
     !check(11,code.load_generation==module->load_generation()) ||
     !check(12,module->native_import_ready() && module->native_import_domain_id()==config.domain->id()) ||
     !check(13,config.domain->active() && config.domain->Contains({module->hmodule_ptr(),code.load_generation})) ||
     !check(14,args.filter==0) ||
     !check(15,work.frame.sp<=UINT32_MAX-0x58 && args.id_output==work.frame.sp+0x50 &&
         args.data_output==work.frame.sp+0x54 && work.frame.sp>=work.thread->stack_limit() &&
         uint64_t(work.frame.sp)+0x58<=work.thread->stack_base()))return false;
  auto read=global_critical_region::AcquireDirect();
  const auto caller=kernel->ResolveNativeImportCaller(work.frame.lr);
  const auto calls=config.catalog->calls();
  if(!check(16,caller.scoped && caller.module.get()==module.get() && caller.domain==config.domain) ||
     !check(17,config.domain->root()==NativeImportDomain::Member{calls->module()->hmodule_ptr(),calls->load_generation()}) ||
     !check(18,calls->module()->GetProcAddressByOrdinal(0x28B)==0x816A8C00) ||
     !check(19,kernel->GetExecutableModule().get()!=module.get()))return false;
  bool import=false;
  // SetupLibraryImports stores the extensionless base name here, unlike
  // the on-disk XEX import header. Keep the exact loader-metadata identity.
  for(const auto& library:*module->xex_module()->import_libraries())if(library.name=="xam")
   for(const auto& fn:library.imports)if(fn.ordinal==0x28B && fn.thunk_address==profile->import_thunk)
    import=true;
  if(!check(20,import))return false;
  auto* memory=kernel->memory();
  const auto original_code=[&](uint32_t address,uint32_t bytes){
   if(memory->QueryRangeAccess(address,bytes)==memory::PageAccess::kNoAccess)return false;
   for(uint32_t offset=0;offset<bytes;offset+=4){
    xex2_section_type type;
    if(kernel->GetUserModuleForAddress(address+offset).get()!=module.get() ||
       !module->xex_module()->GetPageSectionType(address+offset,&type) || type!=XEX_SECTION_CODE ||
       module->xex_module()->TranslateCode(address+offset)!=memory->TranslateVirtual<const uint8_t*>(address+offset))return false;
   }
   return true;
  };
  if(!check(21,original_code(profile->body,profile->body_bytes)) ||
     !check(22,original_code(profile->import_thunk,16)))return false;
  // Match the loader's actual native binding, not decoded ordinal placeholders.
  constexpr std::array<uint32_t,4> thunk={0x3D60816A,0x616B8C00,0x7D6903A6,0x4E800420};
  bool same_thunk=true;
  for(size_t i=0;i<thunk.size();++i)
   same_thunk=same_thunk && xe::load_and_swap<uint32_t>(memory->TranslateVirtual(profile->import_thunk+uint32_t(i)*4))==thunk[i];
  if(!check(23,same_thunk))return false;
  std::array<uint8_t,32> sum{};
  if(!check(24,mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(profile->body),profile->body_bytes,sum.data(),0)==0))return false;
  constexpr char hex[]="0123456789abcdef";bool same_body=true;
  for(size_t i=0;i<sum.size();++i)
   same_body=same_body && profile->body_sha256[i*2]==hex[sum[i]>>4] && profile->body_sha256[i*2+1]==hex[sum[i]&15];
  if(!check(25,same_body))return false;
  observed.authorized=true;return true;
 }
 std::optional<Frame> ReadFrame(cpu::ppc::PPCContext& c,uint32_t function,
                                bool allow_title=false,CodeOwner* caller_code=nullptr,bool retained_task_cleanup=false,
                                const Tasks::PeriodicFrameContext* periodic_context=nullptr,
                                uint64_t periodic_factory=0) const {
  if(retired.load(std::memory_order_acquire) || !kernel || !config.catalog || !config.domain || !config.allocation)return {};
  auto* thread=ActualThread();
  const auto calls=config.catalog->calls();
  allow_title=allow_title || OriginalTitleTaskDescendant(thread,function);
  if(!thread || thread->kernel_state()!=kernel || c.kernel_state!=kernel ||
     c.processor!=kernel->processor() || c.thread_state!=thread->thread_state() ||
     thread->thread_state()->context()!=&c || c.r[13]!=thread->pcr_ptr() ||
     c.r[1]>UINT32_MAX || (c.r[1]&15) || c.lr>UINT32_MAX || c.lr<4 || (c.lr&3) ||
     (thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM &&
      !(allow_title && thread->native_frame_thread_identity().process_type==X_PROCTYPE_TITLE)) ||
     !config.domain->active() ||
     config.domain->root()!=NativeImportDomain::Member{calls->module()->hmodule_ptr(),calls->load_generation()} ||
     kernel->GetUserModuleForAddress(function).get()!=calls->module().get())return {};
  const Frame frame{root,thread->thread_id(),thread->pcr_ptr(),uint32_t(c.r[1]),uint32_t(c.lr),function};
  if(!frame.valid() || (!retained_task_cleanup &&
      !(periodic_context && periodic_context->Matches(frame,periodic_factory)) &&
      !config.allocation->IsAccepting()))return {};
  // New allocation admission and an already-retained native frame's context
  // lifetime are separate. Every root/domain/thread/caller check still runs;
  // no periodic context is a CloseReceipt or a native reference.
  // The sentinel is only a shape. Exact earlier registration is joined in
  // GuardEntry while holding this owner's metadata lock.
  if(c.lr!=0xBCBCBCBC){
   KernelState::NativeImportCaller caller;CodeOwner retained;
   {
    auto lock=global_critical_region::AcquireDirect();
    caller=kernel->ResolveNativeImportCaller(uint32_t(c.lr));
    if(!caller.scoped || !caller.module || caller.domain!=config.domain)return {};
    if(caller.module.get()!=calls->module().get()){
     // Only relevant Task transport and verified slow listener outer callers
     // use this optional existing owner. Fast queue cuts never resolve callers.
     // Membership/lease is code lifetime, never a Task/read/source authority.
     if(!caller_code)return {};
     retained.module=caller.module;retained.load_generation=caller.module->load_generation();
     retained.lease=kernel->AcquireUserModuleExecutionLease(caller.module);
     if(!retained.lease || caller.module->load_generation()==0 ||
        !config.domain->Contains({caller.module->hmodule_ptr(),caller.module->load_generation()}))return {};
    }
   }
   if(caller_code)*caller_code=std::move(retained);
  }
  return frame;
 }
};
// Acyclic ownership: State owns the slot; the capsule owns Job/Work/resources,
// but only weakly names State. Native XMsg Record weakly names the capsule.
struct NativeProfileIdentityOwner::IdentityServiceOrigin::Data {
 std::weak_ptr<State> state;
 std::shared_ptr<State::Job> job;
 std::shared_ptr<State::Work> worker;
 Tasks::IdentityServiceRequest request;
 std::shared_ptr<NativeXmsgIdentityServiceLink> link;
 std::shared_ptr<State::ExecutionLifetime> request_lifetime,pump_lifetime;
 std::shared_ptr<State::ServiceCell> acceptance_cell;
 std::optional<Tasks::IdentityServiceRequest::Pump> pump;
 std::shared_ptr<KernelState::UserModuleExecutionLease> code;
 NativeImportDomain::Operation domain;
 bool unavailable=false;
 void ReleaseResourcesOnHost() noexcept;
};
// Bound before original CD588/CEE08. All fast native-critical cuts below are
// scalar-only and single-thread-affine; no guest memory, locks or ownership
// release occurs. Strong holders live in the state slot/actual Work.
struct NativeProfileIdentityOwner::State::ServiceCell {
 static_assert(std::atomic<void*>::is_always_lock_free);
 static_assert(std::atomic<uint32_t>::is_always_lock_free);
 enum :uint32_t {kEntered=1,kFreeEntered=2,kFreeReturned=4,kReturned=8,kBad=16};
 cpu::ppc::PPCContext* context=nullptr;
 cpu::ThreadState* thread_state=nullptr;
 KernelState* kernel=nullptr;
 cpu::Processor* processor=nullptr;
 object_ref<XThread> thread; // Actual provider thread may differ from C838.
 Frame scope{},completion{},free{};
 Tasks::IdentityServiceRequest::Copy copy;
 std::atomic<bool>* available=nullptr;
 std::atomic<uint32_t> facts{0};
 uint32_t result=0,extended=0,length=0;
 bool acceptance=false;
 bool Bind(cpu::ppc::PPCContext& c,Frame frame,
     Tasks::IdentityServiceRequest::Copy copied,std::atomic<bool>* availability,bool accept) noexcept {
  context=&c;thread_state=c.thread_state;kernel=c.kernel_state;processor=c.processor;
  scope=frame;copy=copied;available=availability;acceptance=accept;
  auto* actual=ActualThread();
  if(!thread_state || !available || !actual || actual->thread_state()!=thread_state)return false;
  thread=retain_object(actual);
  void* empty=nullptr;
  return thread_state->native_identity_service_cell_.compare_exchange_strong(empty,this,std::memory_order_release);
 }
 void ClearRaw() noexcept {context=nullptr;thread_state=nullptr;kernel=nullptr;processor=nullptr;available=nullptr;}
 bool Unbind() noexcept {
  if(!thread_state)return false;void* expected=this;
  if(!thread_state->native_identity_service_cell_.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel))return false;
  ClearRaw(); // Same-thread CAS ended the scope; later stop never dereferences stale TLS.
  return true; // Strong actual Thread releases only on host reap/outside guards.
 }
 bool DetachAtActualStop() noexcept {
  if(!thread_state)return true;void* expected=this;
  if(!thread_state->native_identity_service_cell_.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel) && expected)return false;
  ClearRaw();return true;
 }
 bool Covers(const cpu::ppc::PPCContext& c,uint32_t at,bool returned) const noexcept {
  if(&c!=context || c.thread_state!=thread_state || c.kernel_state!=kernel || c.processor!=processor ||
     c.thread_id!=scope.thread || !DwordIs(c.r[13],scope.pcr) ||
     !DwordIs(c.r[1],uint32_t(c.r[1])))return false;
  const auto flags=facts.load(std::memory_order_acquire);
  if(at==0x816A2630){
   // Fixed HIR cut before the ORIGINAL status store; LR now names an inner
   // call, not the completion entry. No guest read or fact mutation here.
   return !returned && flags==kEntered && completion.sp>=0xA0 && c.r[1]==completion.sp-0xA0 &&
       DwordIs(c.r[30],copy.overlap) && DwordIs(c.r[31],copy.envelope) &&
       DwordIs(c.r[28],result) && DwordIs(c.r[27],extended) && DwordIs(c.r[26],length);
  }
  if(!DwordIs(c.lr,uint32_t(c.lr)))return false;
  const Frame actual{scope.root,scope.thread,scope.pcr,uint32_t(c.r[1]),uint32_t(c.lr),at};
  if(at==0x816A2588 && actual==completion && (flags&kEntered) && !(flags&kReturned))
   return returned || (flags==kEntered && DwordIs(c.r[3],copy.overlap) &&
       DwordIs(c.r[4],result) && DwordIs(c.r[5],extended) && DwordIs(c.r[6],length));
  if(at==0x816E6AB0 && actual==free && (flags&kFreeEntered) && !(flags&kFreeReturned))
   return returned || DwordIs(c.r[3],copy.envelope);
  return false;
 }
 bool Observe(cpu::ppc::PPCContext& c,uint32_t at,bool returned) noexcept {
  if(&c!=context || c.thread_state!=thread_state || c.kernel_state!=kernel || c.processor!=processor ||
     c.thread_id!=scope.thread || !DwordIs(c.r[13],scope.pcr) ||
     !DwordIs(c.r[1],uint32_t(c.r[1])) || !DwordIs(c.lr,uint32_t(c.lr)))return false;
  const Frame actual{scope.root,scope.thread,scope.pcr,uint32_t(c.r[1]),uint32_t(c.lr),at};
  auto flags=facts.load(std::memory_order_relaxed);
  if(at==0x816A2588){
   if(!returned){
    if(!DwordIs(c.r[3],copy.overlap))return false; // A foreign completion is not this request.
    const uint32_t depth=acceptance?0x80:0x4C0;
    const uint32_t link=acceptance?0x818CD5F4:0x818CEDA0;
    if(flags || scope.sp<depth || actual.sp!=scope.sp-depth || actual.lr!=link ||
       !DwordIs(c.r[4],uint32_t(c.r[4])) || !DwordIs(c.r[5],uint32_t(c.r[5])) ||
       !DwordIs(c.r[6],0) ||
       (acceptance?(uint32_t(c.r[4])!=0x65B || uint32_t(c.r[5])!=0x800700AA):
         uint32_t(c.r[4])!=(int32_t(uint32_t(c.r[5]))<0?0x65Bu:0u))){
     facts.fetch_or(kBad,std::memory_order_release);available->store(false,std::memory_order_release);return true;
    }
    completion=actual;result=uint32_t(c.r[4]);extended=uint32_t(c.r[5]);length=0;
    facts.store(kEntered,std::memory_order_release);return true;
   }
   if(!(flags&kEntered) || actual!=completion)return false;
   if((flags&(kFreeEntered|kFreeReturned|kBad))!=(kFreeEntered|kFreeReturned)){
    facts.fetch_or(kBad,std::memory_order_release);return true;
   }
   facts.fetch_or(kReturned,std::memory_order_release);return true;
  }
  if(at!=0x816E6AB0)return false;
  if(!returned){
   if(!DwordIs(c.r[3],copy.envelope))return false;
   if(flags!=kEntered || completion.sp<0xA0 || actual.sp!=completion.sp-0xA0 ||
      actual.lr!=0x816A26E8){facts.fetch_or(kBad,std::memory_order_release);available->store(false,std::memory_order_release);return true;}
   // The pinned original free edge is dominated by the real atomic overlap
   // claim and status/event/APC publication. Never reread a freed packet.
   free=actual;available->store(false,std::memory_order_release);
   facts.fetch_or(kFreeEntered,std::memory_order_release);return true;
  }
  if(!(flags&kFreeEntered) || actual!=free)return false;
  facts.fetch_or(kFreeReturned,std::memory_order_release);return true;
 }
 bool Consume(Tasks::IdentityServiceRequest& request) const noexcept {
  const auto flags=facts.load(std::memory_order_acquire);
  if(!flags)return true;
  if(flags!=(kEntered|kFreeEntered|kFreeReturned|kReturned))return false;
  return request.EnterCompletion(completion,copy.overlap,copy.envelope,result,extended,length) &&
      request.EnterEnvelopeFree(free,copy.envelope) && request.ReturnEnvelopeFree(free) &&
      request.ReturnCompletion(completion);
 }
};

void NativeProfileIdentityOwner::IdentityServiceOrigin::Data::ReleaseResourcesOnHost() noexcept {
 // Actual joins/CPU-stop have already detached every cell. Revoke inside
 // escaped heap holders too: stopped guest stacks may retain shared capsules.
 if(acceptance_cell){acceptance_cell->thread.reset();acceptance_cell.reset();}
 link.reset();request_lifetime.reset();pump_lifetime.reset();
 worker.reset();job.reset();code.reset();domain={};state.reset();
}
bool NativeProfileIdentityOwner::CoversIdentityServiceBoundary(
    void* raw,uint32_t at,bool returned) noexcept {
 if(!raw)return false;auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 auto* cell=c.thread_state?static_cast<State::ServiceCell*>(
     c.thread_state->native_identity_service_cell_.load(std::memory_order_acquire)):nullptr;
 return cell && cell->Covers(c,at,returned);
}
bool NativeProfileIdentityOwner::ObserveIdentityServiceFastBoundary(
    void* raw,uint32_t at,bool returned) noexcept {
 if(!raw)return false;auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 auto* cell=c.thread_state?static_cast<State::ServiceCell*>(
     c.thread_state->native_identity_service_cell_.load(std::memory_order_acquire)):nullptr;
 return cell && cell->Observe(c,at,returned);
}

void NativeProfileIdentityOwner::BindIdentityServiceLink(
    const std::shared_ptr<IdentityServiceOrigin>& origin,
    std::shared_ptr<NativeXmsgIdentityServiceLink> link) noexcept try {
 if(!origin || !origin->data_)return;const auto data=origin->data_;const auto state=data->state.lock();
 if(!state)return;std::lock_guard lock(state->mutex);
 if(state->identity_service==origin && !data->unavailable && !data->link)data->link=std::move(link);
}catch(...){}
std::shared_ptr<NativeProfileIdentityOwner::IdentityServiceOrigin>
NativeProfileIdentityOwner::CaptureIdentityServiceStart(void* raw) noexcept try {
 if(!raw)return {};auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 const auto owner=Lookup(c.kernel_state);if(!owner)return {};auto& s=*owner->state_;
 State::HookUse use;if(!use.Acquire(s.hook_rundown,owner->state_,false))return {};
 std::shared_ptr<IdentityServiceOrigin> origin;
 {std::lock_guard lock(s.mutex);origin=s.identity_service;}
 if(!origin || !origin->data_)return {};const auto data=origin->data_;
 const auto current=s.ReadFrame(c,0x81830878,false,nullptr,true);
 if(!current || !CheckIdentityServiceCode(*s.config.catalog->calls()))return {};
 std::lock_guard lock(s.mutex);const auto& source=data->request.origin();
 if(s.stopping.load() || s.retired.load() || s.uncertain.load() || data->unavailable || data->link ||
    s.identity_service!=origin || s.job!=data->job || !data->job->native_entered ||
    !data->job->data_worker_ready || data->job->execution_uncertain ||
    data->worker->normal_return || data->worker->uncertain || source.producer.sp<0x600 ||
    current->root!=source.producer.root || current->thread!=source.producer.thread ||
    current->pcr!=source.producer.pcr || current->sp!=source.producer.sp-0x600 || current->lr!=0x818309A0 ||
    !DwordIs(c.r[3],0xFC) || !DwordIs(c.r[4],0x58001) || !DwordIs(c.r[5],0) ||
    !DwordIs(c.r[6],current->sp+0x50) || !DwordIs(c.r[7],0x568))return {};
 return origin;
}catch(...){return {};}

void NativeProfileIdentityOwner::ObserveIdentityServiceBoundary(void* raw,uint32_t at,bool returned) noexcept try {
 if(!raw || (at!=0x81830878 && at!=0x818CD588 && at!=0x816A26F8))return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto owner=Lookup(c.kernel_state);if(!owner)return;
 auto& s=*owner->state_;State::HookUse use;if(!use.Acquire(s.hook_rundown,owner->state_,false))return;
 if(global_critical_region::is_held_by_current_thread())return;
 const auto frame=s.ReadFrame(c,at,false,nullptr,true);if(!frame || !CheckIdentityServiceCode(*s.config.catalog->calls()))return;
 if(at==0x81830878 && !returned){
  const auto task=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c);
  auto data=std::make_shared<IdentityServiceOrigin::Data>();data->state=owner->state_;
  data->code=s.root_code;data->domain=s.config.domain->Acquire();
  if(!data->domain || !task)return;
  std::shared_ptr<IdentityServiceOrigin> origin(new IdentityServiceOrigin(data));
  std::lock_guard lock(s.mutex);const auto worker=s.Latest(frame->thread,0x8199C838);const auto job=s.job;
  if(s.stopping.load() || s.retired.load() || s.uncertain.load() || s.identity_service || !job || !job->original ||
     !job->native_entered || !job->data_worker_ready || job->execution_uncertain || !worker ||
     worker->normal_return || worker->uncertain || worker->source!=job->source || !worker->read || !worker->commit ||
     worker->read_pool!=&job->writer_reads || worker->commit->intent!=job->intent ||
     worker->commit->publication!=job->publication || worker->birth.task!=*task || !worker->birth.generation ||
     worker->birth.origin!=job->source || c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire)!=worker->read.get() ||
     !job->intent->old_roster() || job->intent->old_roster()!=s.current_roster ||
     !DwordIs(c.r[3],0) || !DwordIs(c.r[4],worker->frame.sp-0x370) ||
     !DwordIs(c.r[5],4) || !DwordIs(c.r[6],0))return;
  Tasks::IdentityServiceRequest::Origin facts{worker->frame,*frame,worker->birth,job->serial,job->source,
      worker->serial,0x81AEE470,0,uint32_t(c.r[4]),4,0};
  if(!data->request.Begin(facts))return;
  data->job=job;data->worker=worker;s.identity_service=std::move(origin);return;
 }
 std::shared_ptr<IdentityServiceOrigin> origin;
 {std::lock_guard lock(s.mutex);origin=s.identity_service;}
 if(!origin || !origin->data_)return;const auto data=origin->data_;
 if(at==0x81830878){
  std::lock_guard lock(s.mutex);
  if(s.identity_service==origin && !data->request.ReturnProducer(*frame))data->unavailable=true;
  return;
 }
 if(at==0x816A26F8){
  std::lock_guard lock(s.mutex);
  if(!returned && s.identity_service==origin && DwordIs(c.r[3],data->request.copy().overlap))data->request.ObserveCancel();
  return;
 }
 if(!returned){
  const auto observed=NativeXmsgProvenance::IdentityServiceRequest(data->link);
  const auto dispatch=NativeXmsgProvenance::IdentityServiceDispatch(raw,data->link);
  if(!observed || !dispatch || dispatch->app!=0xFC || dispatch->message!=0x58001 ||
     dispatch->provider_callback!=0x818C9940 || dispatch->provider_message!=0x80000007 ||
     dispatch->provider_argument!=observed->envelope || dispatch->copied_argument_bytes!=0x568 ||
     !DwordIs(c.r[3],0x81AEE480) || !DwordIs(c.r[4],observed->argument) || !DwordIs(c.r[5],observed->overlap))return;
  Tasks::IdentityServiceRequest::Copy copy{observed->capture,observed->copy,observed->envelope,observed->argument,
      observed->overlap,{observed->birth,observed->origin,observed->task}};
  auto cell=std::make_shared<State::ServiceCell>();auto* available=NativeXmsgProvenance::IdentityServiceAvailability(data->link);
  std::lock_guard lock(s.mutex);
  if(s.identity_service!=origin || s.job!=data->job || data->unavailable || !data->job->native_entered ||
     !data->job->data_worker_ready || data->job->execution_uncertain ||
     !data->request.BindCopy(copy))return;
  data->request_lifetime=s.ExecutionLifetimeFor(copy.request_birth);
  if(!data->request_lifetime || data->request_lifetime->uncertain ||
     !data->request.EnterAcceptance(*frame,copy,uint32_t(c.r[3]),uint32_t(c.r[4]),uint32_t(c.r[5])) ||
     !cell->Bind(c,*frame,copy,available,true)){data->unavailable=true;return;}
  data->acceptance_cell=std::move(cell);return;
 }
 const auto cell=data->acceptance_cell;if(!cell)return;
 if(!cell->Unbind())return; // Exact cell remains retained on a mismatch.
 // Native CD588 has released its component critical section at this own-return.
 const auto pending=Word(*s.kernel->memory(),0x81AEE484),overlap=Word(*s.kernel->memory(),0x81AEE488);
 std::lock_guard lock(s.mutex);
 if(s.identity_service!=origin || !cell->Consume(data->request) || !pending || !overlap ||
    !data->request.ReturnAcceptance(*frame,uint32_t(c.r[3]),*pending,*overlap))data->unavailable=true;
}catch(...){/* Unavailable observation never substitutes a native result. */}

bool NativeProfileIdentityOwner::State::IdentityServiceJoined(const std::shared_ptr<Job>& current) const {
 if(!identity_service)return true;
 if(!identity_service->data_ || identity_service->data_->job!=current)return false;
 const auto& data=*identity_service->data_;
 return !data.unavailable && data.request.ready_to_reap() && data.request_lifetime &&
     !data.request_lifetime->uncertain && data.request_lifetime->native_freed;
}
bool NativeProfileIdentityOwner::State::IdentityServiceRetainable() const {
 if(!identity_service)return true;const auto data=identity_service->data_;
 return data && data->state.lock().get()==this && data->job && data->worker &&
     data->job==job && data->job->intent && data->job->native_entered && data->code && data->domain &&
     data->worker->thread && data->request.origin().worker.root==root;
}
bool NativeProfileIdentityOwner::State::TryAdoptIdentityServiceTick(
    cpu::ppc::PPCContext& c,const Frame& frame,
    const std::shared_ptr<IdentityServiceOrigin>& origin,const std::shared_ptr<Work>& work,bool fresh) {
 if(!origin || !origin->data_ || !work)return false;const auto data=origin->data_;
 std::optional<Tasks::IdentityServiceRequest::Pump> pump;
 {
  std::lock_guard lock(mutex);
  if(identity_service!=origin || data->unavailable || !data->request.accepted() ||
     retired.load() || uncertain.load() || job!=data->job || !job || !job->native_entered ||
     !job->data_worker_ready || job->execution_uncertain || !identity_service_pump)return false;
  pump=identity_service_pump;
 }
 // No State lock is held across any native/Kernel/XMsg/code observation.
 const auto envelope=NativeXmsgProvenance::IdentityServiceRequest(data->link);
 if(!envelope || envelope->capture!=data->request.copy().capture ||
    envelope->copy!=data->request.copy().serial || !CheckIdentityServiceCode(*config.catalog->calls()))return false;
 const auto current=ReadFrame(c,0x818CEE08,false,nullptr,true);
 const auto task=NativeProfileIdentityTaskContext::Read(*config.catalog->calls(),*config.domain,0x818CEE08,c);
 const auto actual=task?NativeProfileIdentityOwner::CurrentTaskExecution(&c,*task):std::nullopt;
 if(!current || *current!=frame || !task || !actual || !DwordIs(c.r[3],pump->object) ||
    frame.lr!=0x818CEFE8)return false;
 if(actual->callback_lr!=0x816B8B64 && actual->callback_lr!=0x816B7EA4)return false;
 Tasks::CurrentExecution dispatch{{actual->kernel,actual->domain,actual->root_generation},pump->birth,
     actual->invocation,{},actual->body_sp,actual->callback,actual->argument,actual->callback_lr};
 auto cell=std::make_shared<ServiceCell>();auto* availability=NativeXmsgProvenance::IdentityServiceAvailability(data->link);
 std::lock_guard lock(mutex);
 if(identity_service!=origin || data->unavailable || !data->request.accepted() || job!=data->job ||
    retired.load() || uncertain.load() || !job->native_entered || !job->data_worker_ready ||
    job->execution_uncertain || (render_phase!=RenderPhase::kOldPaused && render_phase!=RenderPhase::kClosingStop) ||
    !identity_service_pump || *identity_service_pump!=*pump ||
    (data->pump && *data->pump!=*pump) || work->frame!=frame || work->thread.get()!=ActualThread() || !work->domain ||
    work->read || work->commit || work->service_progress ||
    (!fresh && (!work->prebody_parked || work->system_frame_ambient_task!=*task ||
      std::find(active.begin(),active.end(),work)==active.end())) ||
    (fresh && (active.size()>=256 || !next_work || next_work==UINT64_MAX ||
      std::any_of(active.begin(),active.end(),[&](const auto& old){return old->frame==frame;}))))return false;
 const auto* node=tasks.FindBirthConst(pump->birth.generation);
 if(!node || !Tasks::SameBirth(node->birth,pump->birth) || node->freed || node->delete_entered || node->free_entered ||
    !node->execution_only || !node->published || !node->execution_publication ||
    node->callback!=0x818CEFD8 || node->argument!=pump->object || *task!=pump->birth.task ||
    actual->birth_generation!=pump->birth.generation || actual->task!=pump->birth.task ||
    actual->callback!=0x818CEFD8 || actual->argument!=pump->object)return false;
 const auto lifetime=ExecutionLifetimeFor(pump->birth);
 if(!lifetime || lifetime->uncertain || lifetime->native_freed)return false;
 const auto retained=Latest(frame.thread,actual->callback_lr==0x816B8B64?0x816B8B08u:0x816B7DE8u);
 if(!retained || !retained->execution_only || retained->normal_return || retained->execution_uncertain ||
    retained->execution_dispatch!=actual->invocation || !Tasks::SameBirth(retained->birth,pump->birth))return false;
 dispatch.dispatcher=retained->frame;
 // Preserve an already suspended dispatcher scope. If a real scope is still
 // ambient, select its unique actual owner, never its source tag or an older
 // pending cell. The service receives no read ticket/Commit authority.
 void* ambient=c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire);
 std::shared_ptr<Work> read_owner;
 if(ambient){
  for(const auto& parent:active)if(parent!=work && parent->read.get()==ambient){
   if(read_owner || parent->service_progress || parent->normal_return || parent->uncertain ||
      parent->thread.get()!=work->thread.get() || parent->frame.root!=frame.root ||
      parent->frame.thread!=frame.thread || parent->frame.pcr!=frame.pcr || parent->frame.sp<=frame.sp)return false;
   read_owner=parent;
  }
  if(!read_owner)return false;
 }
 const auto tick=data->request.EnterTick(frame,*pump,dispatch);if(!tick)return false;
 if(!cell->Bind(c,frame,data->request.copy(),availability,false)){
  // The genuine body is still unentered. Withdraw only this reservation,
  // never manufacture a native callback/Task return or completion.
  data->request.WithdrawUnenteredTick(*tick);return false;
 }
 if(ambient && !work->execution_read.SuspendService(read_owner->frame,frame,read_owner,read_owner,
      ambient,c.thread_state->native_profile_roster_read_)){
  cell->Unbind();data->request.WithdrawUnenteredTick(*tick);return false;
 }
 // Capacity was reserved at State construction. After the successful scope CAS
 // only nonthrowing pointer/scalar stores and this bounded insertion remain.
 if(fresh){work->serial=next_work++;active.push_back(work);}
 work->execution_read_owner=std::move(read_owner);
 data->pump=*pump;data->pump_lifetime=lifetime;
 work->service_origin=origin;work->service_tick=*tick;work->service_cell=std::move(cell);
 work->service_progress=true;work->system_thread_frame=false;work->prebody_parked=false;work->physical_work=true;
 work->source=job->source;work->birth=pump->birth; // Lifetime source and actual Boot1 Task are distinct.
 work->system_frame_ambient_task=*task;work->system_frame_ambient_birth=pump->birth;
 return true;
}
std::optional<uint64_t> NativeProfileIdentityOwner::TryEnterIdentityServiceTick(void* raw,uint32_t at) noexcept try {
 if(!raw || at!=0x818CEE08)return {};auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto& s=*state_;
 std::shared_ptr<IdentityServiceOrigin> origin;
 {std::lock_guard lock(s.mutex);origin=s.identity_service;
  if(!origin || !origin->data_ || !origin->data_->request.accepted() || origin->data_->unavailable)return {};}
 State::HookUse use;if(!use.Acquire(s.hook_rundown,state_,false))return {};
 // Retained-context validation is restricted to an already-issued live request.
 const auto frame=s.ReadFrame(c,at,false,nullptr,true);auto* thread=ActualThread();
 if(!frame || !thread || thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM)return {};
 auto work=std::make_shared<State::Work>();work->frame=*frame;work->thread=retain_object(thread);
 work->domain=s.config.domain->Acquire();work->first=uint32_t(c.r[3]);
 if(s.TryAdoptIdentityServiceTick(c,*frame,origin,work,true))return 1;
 return {};
}catch(...){return {};}
bool NativeProfileIdentityOwner::ReturnIdentityServiceTick(void* raw,uint32_t at) noexcept try {
 if(!raw || at!=0x818CEE08)return false;auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto& s=*state_;
 State::HookUse use;if(!use.Acquire(s.hook_rundown,state_,false))return false;
 auto* thread=ActualThread();if(!thread)return false;
 std::shared_ptr<State::Work> work;
 {std::lock_guard lock(s.mutex);
  for(auto i=s.active.rbegin();i!=s.active.rend();++i)
   if((*i)->frame.thread==thread->thread_id() && (*i)->frame.function==at){
    if(!(*i)->service_progress)return false; // A younger ordinary frame shadows an older service call.
    work=*i;break;}}
 if(!work)return false;const auto origin=work->service_origin;const auto data=origin?origin->data_:nullptr;
 const auto frame=s.ReadFrame(c,at,false,nullptr,true);
 const auto task=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c);
 const auto actual=task?CurrentTaskExecution(raw,*task):std::nullopt;
 if(!data || !frame || *frame!=work->frame || !task || !actual ||
    !work->service_tick || actual->invocation!=work->service_tick->dispatch ||
    actual->birth_generation!=work->birth.generation || *task!=work->birth.task ||
    !CheckIdentityServiceCode(*s.config.catalog->calls()) || !work->service_cell)return true;
 if(c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire)){
  std::lock_guard lock(s.mutex);s.SuspendAmbientOnUnavailable(c,frame->thread);data->unavailable=true;return true;
 }
 if(!work->service_cell->Unbind())return true;
 std::lock_guard lock(s.mutex);
 if(work->normal_return || !work->service_cell->Consume(data->request) ||
    !data->request.ReturnTick(*work->service_tick,*frame,work->birth)){data->unavailable=true;return true;}
 const auto current=std::find(s.active.begin(),s.active.end(),work);
 if(current==s.active.end()){data->unavailable=true;return true;}
 if(work->execution_read.pending()){
  const auto parent=work->execution_read_owner;
  if(!parent || !parent->read || parent->normal_return || parent->uncertain ||
     parent->thread.get()!=work->thread.get() || parent->frame!=work->execution_read.origin() ||
     std::find(s.active.begin(),s.active.end(),parent)==s.active.end()){
   data->unavailable=true;return true;
  }
  const auto observed=s.tasks.ObservedTransportSubject(*task);
  const bool same_task=observed && Tasks::SameBirth(*observed,parent->birth) &&
      Tasks::SameBirth(parent->birth,work->birth);
  if(same_task){
   if(!work->execution_read.Restore(*frame,parent->read.get(),c.thread_state->native_profile_roster_read_)){
    data->unavailable=true;return true;
   }
   parent->execution_read_fallback=false;
  }else{
   // Native currentTask is still the pump. An enclosing different Task read
   // stays hidden until that exact origin's genuine restore-to-Leave boundary.
   if(!work->execution_read.DelegateToOrigin(parent.get(),parent->read.get())){
    data->unavailable=true;return true;
   }
   parent->execution_read_fallback=true;
  }
 }
 work->normal_return=true;s.host_retired.push_back(work);s.active.erase(current);s.changed.notify_all();return true;
}catch(...){return true;}

// Both native224 queue variants may reach their worker before the original
// initializer returns. Park at the actual callback entry, never after native
// data/cache instructions or under State/GCRC/native locks. No Task callback
// start/return, read scope or listener whole-work exists until normal Core
// admission follows this gate.
bool NativeProfileIdentityOwner::State::AwaitPreparedWorker(
    cpu::ppc::PPCContext& c,uint32_t at) noexcept {
 HookUse hook_use;
 try{
  if(!hook_use.Acquire(hook_rundown,shared_from_this(),true)){Lose(__LINE__);return false;}
  std::shared_ptr<Job> pending;
  {
   std::lock_guard lock(mutex);pending=job;
   if(!pending || !pending->original_render_frame || pending->data_worker_ready)return true;
  }
  if(global_critical_region::is_held_by_current_thread()){Lose(__LINE__);return false;}
  auto* thread=ActualThread();
  const auto task=NativeProfileIdentityTaskContext::Read(*config.catalog->calls(),*config.domain,at,c);
  if(!task || !thread || !DwordIs(c.r[3],uint32_t(c.r[3]))){Lose(__LINE__);return false;}
  Tasks::Birth captured;
  {
   std::lock_guard lock(mutex);
   const auto* node=tasks.FindLiveTask(*task);
   if(job!=pending || !pending->native_entered || !pending->overlap || pending->canceled ||
      !node || node->birth.origin!=pending->source || !node->published || node->started ||
      node->returned || node->callback!=at || node->argument!=uint32_t(c.r[3]) ||
      node->execution_only || node->created_persistent || node->periodic_factory || node->transport_only){
    Lose(__LINE__);return false;
   }
   captured=node->birth;
  }
  // This ALREADY published Task may arrive after stop sealed allocation.
  // Retained-context validation captures metadata only, never body permission;
  // every caller/domain/code/frame predicate still runs. Actual Core admission
  // below remains strict and is not replaced by this prebody reservation.
  const bool accepting=config.allocation->IsAccepting();
  const auto frame=ReadFrame(c,at,false,nullptr,true);
  if(!frame){Lose(__LINE__);return false;}
  auto worker=std::make_shared<PrebodyIdentityWorker>();worker->frame=*frame;
  worker->thread=retain_object(thread);worker->code=root_code;worker->domain=config.domain->Acquire();
  worker->argument=uint32_t(c.r[3]);
  {
   std::lock_guard lock(mutex);
   const auto* node=tasks.FindLiveTask(*task);
   if(job!=pending || !worker->code || !worker->domain || !node ||
      !Tasks::SameBirth(node->birth,captured) || node->birth.origin!=pending->source ||
      !node->published || node->started || node->returned || node->callback!=at ||
      node->argument!=worker->argument){Lose(__LINE__);return false;}
   const auto slot=std::find_if(pending->prebody_workers.begin(),pending->prebody_workers.end(),
       [](const auto& old){return !old;});
   if(slot==pending->prebody_workers.end() || std::any_of(pending->prebody_workers.begin(),
       pending->prebody_workers.end(),[&](const auto& old){return old && old->frame==*frame;})){
    Lose(__LINE__);return false;
   }
   worker->birth=node->birth;*slot=worker;
   if(stopping.load() || retired.load() || uncertain.load() || pending->execution_uncertain){
    worker->refused=true;pending->execution_uncertain=true;return false;
   }
   if(!accepting){worker->refused=true;pending->execution_uncertain=true;Lose(__LINE__);return false;}
  }
  for(;;){
   // Actual frame/Task/code checks precede State, just like original Core.
   const auto current_frame=ReadFrame(c,at);
   const auto current=NativeProfileIdentityTaskContext::Read(*config.catalog->calls(),*config.domain,at,c);
   {
    std::lock_guard lock(mutex);
    const auto* node=current?tasks.FindLiveTask(*current):nullptr;
    if(job!=pending || stopping.load() || retired.load() || uncertain.load() ||
       pending->execution_uncertain || pending->canceled){
     // Native initializer already entered. Retain the exact request, code and
     // worker metadata for factual stop; never CancelRoster or invent a body
     // return/CompleteIO/Task close to make shutdown look drained.
     worker->refused=true;pending->execution_uncertain=true;return false;
    }
    if(ActualThread()!=thread || !current_frame || *current_frame!=*frame ||
       !DwordIs(c.r[3],worker->argument) || !node ||
       !Tasks::SameBirth(node->birth,worker->birth) || node->birth.origin!=pending->source ||
       !node->published || node->started || node->returned || node->callback!=at ||
       node->argument!=worker->argument){
     worker->refused=true;pending->execution_uncertain=true;Lose(__LINE__);return false;
    }
    if(pending->data_worker_ready){
     worker->admitted=true;return true; // existing EnterWork/read/Commit follows
    }
   }
   if(!hook_use.may_park()){Lose(__LINE__);return false;}
   thread->Delay(0,0,uint64_t(-10000ll));
  }
 }catch(...){Lose(__LINE__);return false;}
}
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);


struct NativeProfileIdentityOwner::TaskReferenceAcquire::Data {
 std::shared_ptr<State> state;
 std::shared_ptr<TaskReference> reference;
 std::shared_ptr<void> producer;
};
NativeProfileIdentityOwner::TaskReferenceAcquire::TaskReferenceAcquire(
    std::shared_ptr<Data> data,void* frame,bool admitted)
    :data_(std::move(data)),frame_state_(frame),admitted_(admitted){}
NativeProfileIdentityOwner::TaskReferenceAcquire::TaskReferenceAcquire(TaskReferenceAcquire&& other) noexcept
    :data_(std::move(other.data_)),frame_state_(std::exchange(other.frame_state_,nullptr)),
     admitted_(std::exchange(other.admitted_,false)),finished_(std::exchange(other.finished_,true)){}
std::shared_ptr<NativeProfileIdentityOwner::TaskReference>
NativeProfileIdentityOwner::TaskReferenceAcquire::reference() const noexcept {
 return data_?data_->reference:nullptr;
}
void NativeProfileIdentityOwner::TaskReferenceAcquire::Withdraw() noexcept {
 if(!data_ || !data_->reference)return;
 const auto& d=data_->reference->data_;
 if(!d->bound.load(std::memory_order_acquire))return;
 void* exact=d.get();
 if(!d->cpu || !d->cpu->native_identity_task_reference_cell_.compare_exchange_strong(
     exact,nullptr,std::memory_order_acq_rel)){
  d->receipt.Fail();data_->state->Lose(__LINE__);
 }
 d->bound.store(false,std::memory_order_release);
}
NativeProfileIdentityOwner::TaskReferenceAcquire::~TaskReferenceAcquire(){
 Withdraw();
 if(data_ && !finished_){
  auto& receipt=data_->reference->data_->receipt;
  if(receipt.state()==Tasks::ReferenceReceipt::Phase::kPrepared)receipt.AbandonAcquire();
  else if(receipt.state()!=Tasks::ReferenceReceipt::Phase::kCanceled){receipt.Fail();data_->state->Lose(__LINE__);}
 }
 data_.reset(); // All independent owners precede the final host-frame gate.
 if(frame_state_)static_cast<State*>(frame_state_)->task_close_frames.fetch_sub(1,std::memory_order_acq_rel);
}
bool NativeProfileIdentityOwner::TaskReferenceAcquire::Complete(
    bool executed,uint64_t actual_tag,uint32_t actual_current) noexcept {
 if(finished_)return false;finished_=true;
 if(!admitted_)return false;
 if(!data_)return executed; // Explicit feature-absent original native route.
 Withdraw();
 const bool complete=data_->reference->data_->receipt.CompleteAcquire(executed,actual_tag,actual_current);
 if(!complete)data_->state->Lose(__LINE__);
 return complete;
}
NativeProfileIdentityOwner::TaskReferenceAcquire
NativeProfileIdentityOwner::BeforeNativeTaskReferenceAcquire(KernelState* kernel,
    XThread& thread,TaskReferenceFrame caller,uint32_t current,
    std::shared_ptr<void> producer,TaskReferenceAcquireKind kind) noexcept {
 static_assert(Tasks::ReferenceReceipt::kExecutePadding==cpu::kGuestCallExecuteStackPadding);
 State* frame_state=nullptr;
 try{
  if(!kernel || ActualThread()!=&thread || thread.kernel_state()!=kernel || !producer ||
      global_critical_region::is_held_by_current_thread() ||
      thread.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM)return {{},nullptr,false};
  const auto owner=NativeSystemUiService::CaptureTaskExecutionOwner(kernel);if(!owner)return {{},nullptr,true};
  auto& s=*owner->state_;
  // The existing exact retained Unload Control may already be executing when
  // identity BeginStop runs. This is its getter reference, never new work.
  const bool retained_unload=kind==TaskReferenceAcquireKind::kRetainedUnload && caller.function==0x816BD560;
  {
   std::lock_guard lock(s.mutex);
   if(s.retired.load())return {{},nullptr,false};
   const auto old=s.task_close_frames.fetch_add(1,std::memory_order_acq_rel);
   if(old==UINT64_MAX){s.task_close_frames.fetch_sub(1,std::memory_order_acq_rel);s.Lose(__LINE__);return {{},nullptr,false};}
   frame_state=&s;
  }
  const auto reject=[&]{return TaskReferenceAcquire({},frame_state,false);};
  // Authority/GuestRuntime checks take their own locks. The strict frame gate
  // already protects these owners; never nest those locks under State.
  const bool accepting=s.config.allocation->IsAccepting();
  if(!Tasks::ReferenceReceipt::MayAcquire(s.stopping.load(),accepting,
      retained_unload,caller.function))return reject();
  auto* cpu=thread.thread_state();auto* c=cpu?cpu->context():nullptr;
  if(!c || c->kernel_state!=kernel || c->thread_state!=cpu || c->processor!=kernel->processor() ||
      c->r[1]!=caller.sp || c->lr!=caller.lr || c->r[13]!=caller.pcr || caller.pcr!=thread.pcr_ptr() ||
      (caller.function!=0x816A2C80 && caller.function!=0x816BD4D8 &&
       caller.function!=0x816BD560 && caller.function!=0x819650F8) ||
      !CheckTaskReferenceCode(*s.config.catalog->calls()))return reject();
  auto d=std::make_shared<TaskReference::Data>();d->owner=owner->state_;
  d->producer=producer;d->producer_object=producer.get();d->kernel=kernel;
  d->thread=retain_object(&thread);d->cpu=cpu;d->context=c;d->processor=c->processor;
  d->code=s.root_code;d->domain=s.config.domain->Acquire();
  if(!d->code || !d->domain)return reject();
  auto use=std::make_shared<TaskReferenceAcquire::Data>();use->state=owner->state_;use->producer=std::move(producer);
  use->reference=std::shared_ptr<TaskReference>(new TaskReference(d));
  std::shared_ptr<TaskReference::Data> replaced;
  {
   std::lock_guard lock(s.mutex);
   const auto slot=std::find_if(s.task_references.begin(),s.task_references.end(),[](const auto& old){
    return !old || old->resources_retired.load(std::memory_order_acquire);
   });
   if(s.retired.load() || (s.stopping.load() && !retained_unload) ||
       (s.uncertain.load() && !retained_unload) || !s.next_task_reference ||
       s.next_task_reference==UINT64_MAX || slot==s.task_references.end() ||
       !d->receipt.Prepare(s.next_task_reference++,{s.root,thread.thread_id(),caller.pcr,
            caller.sp,caller.lr,caller.function},current))return reject();
   replaced=std::move(*slot);*slot=d;
  }
  void* empty=nullptr;
  if(!cpu->native_identity_task_reference_cell_.compare_exchange_strong(empty,d.get(),std::memory_order_acq_rel)){
   d->receipt.AbandonAcquire();return TaskReferenceAcquire(std::move(use),frame_state,false);
  }
  d->bound.store(true,std::memory_order_release);
  return TaskReferenceAcquire(std::move(use),frame_state,true);
 }catch(...){if(frame_state)frame_state->Lose(__LINE__);return {{},frame_state,false};}
}

struct NativeProfileIdentityOwner::TaskCloseUse::Data {
 std::shared_ptr<State> state;
 std::shared_ptr<State::Expected> expected;
 std::shared_ptr<void> producer;
};
NativeProfileIdentityOwner::TaskCloseUse::TaskCloseUse(
    std::shared_ptr<Data> data,void* frame_state,bool admitted)
    :data_(std::move(data)),frame_state_(frame_state),admitted_(admitted){}
NativeProfileIdentityOwner::TaskCloseUse::TaskCloseUse(TaskCloseUse&& other) noexcept
    :data_(std::move(other.data_)),frame_state_(std::exchange(other.frame_state_,nullptr)),
     admitted_(std::exchange(other.admitted_,false)),finished_(std::exchange(other.finished_,true)){}
NativeProfileIdentityOwner::TaskCloseUse::~TaskCloseUse(){
 if(data_ && !finished_){
  auto& s=*data_->state;s.RecordTaskCloseRefusal(14,data_->expected->args[0]|1u);s.Lose(__LINE__);
  if(!global_critical_region::is_held_by_current_thread()){
   std::lock_guard lock(s.mutex);if(data_->expected->task_release)data_->expected->task_release->Fail();
   if(data_->expected->reference_release)data_->expected->reference_release->receipt.Fail();
  }
 }
 data_.reset(); // Release all independent owners before the final frame gate.
 if(frame_state_)static_cast<State*>(frame_state_)->task_close_frames.fetch_sub(1,std::memory_order_acq_rel);
}
bool NativeProfileIdentityOwner::TaskCloseUse::Complete(bool executed,uint32_t value) noexcept {
 if(finished_)return false;finished_=true;
 if(!admitted_)return false;
 if(!data_)return executed; // Explicit unobserved feature-absent native route.
 try{
  auto& s=*data_->state;const auto& e=data_->expected;
  if(global_critical_region::is_held_by_current_thread() || ActualThread()!=e->thread.get()){
   s.RecordTaskCloseRefusal(11,e->args[0]|1u);s.Lose(__LINE__);return false;
  }
  std::lock_guard lock(s.mutex);
  const auto found=std::find(s.expected.begin(),s.expected.end(),e);
  if(found==s.expected.end() || e->call_returned || (!e->task_release && !e->reference_release)){
   s.RecordTaskCloseRefusal(12,e->args[0]|1u);s.Lose(__LINE__);return false;}
  e->call_returned=true;
  const bool context_current=!e->context_reference ||
      (!s.context_reference_unavailable && s.CurrentContextReference(*e->thread)==e->context_reference &&
       e->context_reference->receipt.Permits(e->context_reference_call,e->args[0]));
  e->healthy=context_current && e->entered && e->returned && e->actual &&
      (!e->task_release || e->task_release->Complete(executed,value)) &&
      (!e->reference_release || e->reference_release->receipt.CompleteRelease(executed,value));
  if(!e->healthy){if(e->task_release)e->task_release->Fail();
   if(e->reference_release)e->reference_release->receipt.Fail();
   s.RecordTaskCloseRefusal(13,e->args[0]|1u);s.Lose(__LINE__);return false;}
  if(e->reference_only){
   uint32_t empty=0;
   if(s.first_reference_close_state.compare_exchange_strong(empty,1,std::memory_order_relaxed)){
    const auto& r=e->reference_release->receipt;
    s.first_reference_close={r.serial,r.task,value,e->reference_lookup,e->reference_parent,
        r.acquire,r.release_callback,r.release};
    s.first_reference_close_state.store(2,std::memory_order_release);
   }
  }
  s.retired_expected.push_back(e);s.expected.erase(found);return true;
 }catch(...){if(data_){data_->state->RecordTaskCloseRefusal(10,data_->expected->args[0]|1u);
   data_->state->Lose(__LINE__);}return false;}
}
NativeProfileIdentityOwner::TaskCloseUse NativeProfileIdentityOwner::BeforeNativeTaskClose(
    KernelState* kernel,XThread& thread,uint32_t tagged_target) noexcept {
 return BeforeNativeTaskCloseImpl(kernel,thread,tagged_target,{},nullptr,nullptr,{});
}
NativeProfileIdentityOwner::TaskCloseUse NativeProfileIdentityOwner::BeforeNativeTaskReferenceClose(
    KernelState* kernel,XThread& thread,uint32_t tagged_target,
    const std::shared_ptr<TaskReference>& reference,TaskReferenceFrame actual_call,TaskReferenceFrame callback,
    std::shared_ptr<void> producer) noexcept {
 return BeforeNativeTaskCloseImpl(kernel,thread,tagged_target,reference,&actual_call,&callback,std::move(producer));
}
NativeProfileIdentityOwner::TaskCloseUse NativeProfileIdentityOwner::BeforeNativeTaskCloseImpl(
    KernelState* kernel,XThread& thread,uint32_t tagged_target,
    const std::shared_ptr<TaskReference>& reference,const TaskReferenceFrame* actual_call,
    const TaskReferenceFrame* callback,std::shared_ptr<void> producer) noexcept {
 State* frame_state=nullptr;
 try{
  if(!kernel || ActualThread()!=&thread || thread.kernel_state()!=kernel ||
      global_critical_region::is_held_by_current_thread() || tagged_target<0x1000 ||
      (tagged_target&3)!=1)return TaskCloseUse({},nullptr,false);
  const auto owner=NativeSystemUiService::CaptureTaskExecutionOwner(kernel);
  if(!owner)return TaskCloseUse({},nullptr,true);
  auto& s=*owner->state_;
  const auto reject=[&](uint32_t reason){s.RecordTaskCloseRefusal(reason,tagged_target);
    return TaskCloseUse({},frame_state,false);};
  {
   std::lock_guard lock(s.mutex);
   if(s.retired.load())return reject(1);
   const auto prior=s.task_close_frames.fetch_add(1,std::memory_order_acq_rel);
   if(prior==UINT64_MAX){s.task_close_frames.fetch_sub(1,std::memory_order_acq_rel);s.Lose(__LINE__);
    return reject(2);}
   frame_state=&s;
  }
  auto data=std::make_shared<TaskCloseUse::Data>();data->state=owner->state_;data->producer=std::move(producer);
  auto expected=std::make_shared<State::Expected>();data->expected=expected;
  const uint32_t body=tagged_target&~1u;
  const auto acquired=reference?reference->data_:nullptr;
  Frame callback_frame,release_call;
  bool nonfinal_baseline=false;
  bool context_code=false;
  std::shared_ptr<State::ContextReference> context_reference;
  uint32_t baseline_parent=0;
  if(actual_call){
   auto* cpu=thread.thread_state();auto* c=cpu?cpu->context():nullptr;
   if(!acquired || acquired->retired.load(std::memory_order_acquire) ||
       acquired->receipt.state()!=Tasks::ReferenceReceipt::Phase::kHeld ||
       acquired->resources_retired.load(std::memory_order_acquire) || acquired->kernel!=kernel ||
       acquired->owner.lock().get()!=owner->state_.get() || acquired->bound.load(std::memory_order_acquire) ||
       acquired->receipt.task!=body ||
       !acquired->MatchesProducer(data->producer) || !c || c->kernel_state!=kernel ||
       c->thread_state!=cpu || c->processor!=kernel->processor() || c->r[1]!=actual_call->sp ||
       c->lr!=actual_call->lr || c->r[13]!=actual_call->pcr || actual_call->pcr!=thread.pcr_ptr() ||
       thread.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
       !CheckTaskReferenceCode(*s.config.catalog->calls()))return reject(15);
   release_call={s.root,thread.thread_id(),actual_call->pcr,actual_call->sp,actual_call->lr,actual_call->function};
   if(callback)callback_frame={s.root,thread.thread_id(),callback->pcr,callback->sp,callback->lr,callback->function};
   const auto actual_task=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),
       *s.config.domain,actual_call->function,*c);
   if(!actual_task || *actual_task!=body)return reject(15);
   {std::lock_guard lock(s.mutex);context_reference=s.CurrentContextReference(thread);}
   context_code=context_reference && CheckContextReferenceCode(*s.config.catalog->calls());
   context_reference.reset();
   // One owned-stack snapshot, no retained guest pointer. An unsupported
   // baseline does not change the original unique-Birth close route.
   const uint32_t limit=thread.stack_limit(),base=thread.stack_base();
   std::array<uint8_t,0x70> stack{};
   if(callback_frame.valid() && callback_frame.pcr==release_call.pcr &&
       callback_frame.function==acquired->receipt.callback.function && callback_frame.sp>=release_call.sp &&
       callback_frame.lr==0x816B8B64 && limit<base &&
       callback_frame.sp>=limit && uint64_t(callback_frame.sp)+stack.size()<=base &&
       internal::ReadNativeWaitSnapshot(*kernel->memory(),callback_frame.sp,stack)){
    baseline_parent=xe::load_and_swap<uint32_t>(stack.data()+0x68);
    nonfinal_baseline=Tasks::ReferenceReceipt::NonfinalQueueBaseline(callback_frame,limit,base,
       xe::load_and_swap<uint32_t>(stack.data()),baseline_parent);
   }
  }
  std::shared_ptr<NativeSystemTaskContinuation> continuation;
  {
   std::lock_guard lock(s.mutex);continuation=s.continuation.lock();
  }
  const bool accepting=s.config.allocation && s.config.allocation->IsAccepting();
  if(!accepting && !acquired && (!continuation || !continuation->MatchesTaskReleaseCaller(thread,body)))
   return reject(3);
  expected->thread=retain_object(&thread);expected->function=0x816B7490;
  expected->count=1;expected->args[0]=body;expected->task_release.emplace();
  {
   std::lock_guard lock(s.mutex);
   Tasks::TransportLookup lookup;
   const auto birth=s.tasks.ObservedTransportSubject(body,actual_call?&lookup:nullptr);
   if(s.retired.load())return reject(1);
   if(s.tasks.lost())return reject(4);
   if(!birth && acquired){
    uint32_t empty=0;
    if(s.first_reference_lookup_state.compare_exchange_strong(empty,1,std::memory_order_relaxed)){
     s.first_reference_lookup={acquired->receipt.serial,body,uint32_t(lookup.kind),
         lookup.matching_rows,lookup.freed_rows,lookup.free_in_progress,lookup.live_rows};
     s.first_reference_lookup_state.store(2,std::memory_order_release);
    }
   }
   if(!birth && acquired && context_code && !s.context_reference_unavailable){
    auto current=s.CurrentContextReference(thread);
    if(current && current->cpu==thread.thread_state() && current->context==thread.thread_state()->context() &&
       current->code && current->domain && current->receipt.Permits(release_call,body))context_reference=current;
   }
   if(!birth && (!acquired || (!nonfinal_baseline && !context_reference) ||
       (lookup.kind!=Tasks::TransportLookup::Kind::kAbsent &&
        lookup.kind!=Tasks::TransportLookup::Kind::kHistoricalFreed)))return reject(5);
   if(s.expected.size()>=64)return reject(6);
   if(!s.next_work || s.next_work==UINT64_MAX)return reject(9);
   if(std::any_of(s.expected.begin(),s.expected.end(),[&](const auto& e){return e &&
        (e->task_release || e->reference_release) && e->thread.get()==&thread && !e->call_returned;}))return reject(7);
   if(birth){
    if(!expected->task_release->Issue(s.root,*birth,thread.thread_id(),thread.pcr_ptr()))return reject(8);
    expected->source=birth->origin;
   }else{
    expected->task_release.reset();expected->reference_only=true;expected->source=kBootUnjoinedSource;
   }
   if(acquired){
    const bool begun=context_reference?
        acquired->receipt.BeginContextRelease(release_call,context_reference->receipt,body):
        acquired->receipt.BeginRelease(release_call,callback_frame,body,!birth);
    if(!begun)return reject(15);
    expected->context_reference=context_reference;expected->context_reference_call=release_call;
    expected->reference_release=acquired;expected->reference_lookup=uint32_t(lookup.kind);
    expected->reference_parent=baseline_parent;
   }
   expected->serial=s.next_work++;s.expected.push_back(expected);
  }
  return TaskCloseUse(std::move(data),frame_state,true);
 }catch(...){if(frame_state){frame_state->RecordTaskCloseRefusal(10,tagged_target);frame_state->Lose(__LINE__);}
  return TaskCloseUse({},frame_state,false);}
}

void NativeProfileIdentityOwner::ReportFirstRefusalOnBootstrap(
    XThread& bootstrap) noexcept {
 // Only Service's host Bootstrap turn/failure and Step call this, outside
 // their metadata locks and before executing any next native cleanup call.
 // Reject guest fibers and a foreign host thread, even with a matching id.
 auto& s=*state_;
 if(s.retired.load(std::memory_order_acquire) || XThread::GetCurrentFiberThread() ||
    !XThread::IsInThread(&bootstrap) || bootstrap.kernel_state()!=s.kernel ||
    !s.config.allocation || bootstrap.thread_id()!=s.config.allocation->bootstrap_thread_id() ||
    bootstrap.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    global_critical_region::is_held_by_current_thread())return;
 try{
  s.ReportTaskOperands("Bootstrap");s.ReportReferenceClose();s.ReportPublisherAbort();
  const uint32_t denial=s.first_denial_state.load(std::memory_order_acquire);
  if(denial==1)return; // Incomplete concurrent capture is never read or waited.
  const uint32_t loss=s.first_loss_line.load(std::memory_order_relaxed);
  const uint64_t task_close=s.first_task_close_refusal.load(std::memory_order_relaxed);
  const auto listener_diagnostic=s.CaptureListenerDiagnostic();
  if(!loss && denial!=2 && !task_close && s.first_return_state.load(std::memory_order_acquire)!=2 &&
     !listener_diagnostic.snapshot.state && !listener_diagnostic.snapshot.ledger)return;
  bool no=false;
  if(!s.first_refusal_reported.compare_exchange_strong(no,true,std::memory_order_relaxed))return;
  const uint32_t entry=denial==2?s.first_denial_entry:0;
  const uint32_t line=denial==2?s.first_denial_line:0;
  const uint64_t lr=denial==2?s.first_denial_lr:0;
  const uint64_t sp=denial==2?s.first_denial_sp:0;
  const auto args=denial==2?s.first_denial_args:std::array<uint64_t,4>{};
  XELOGE("Native identity first captured refusal at safe host Bootstrap: "
         "first_loss_line={} first_denial_line={} guard_entry={:08X} "
         "caller_lr={:016X} sp={:016X} r3={:016X} r4={:016X} "
         "r5={:016X} r6={:016X} task_close_reason={} task_close_target={:08X}; advisory only, source SHA binds lines; "
         "no-cell publisher refusals are not captured; listener snapshot follows",
         loss,line,entry,lr,sp,args[0],args[1],args[2],args[3],uint32_t(task_close>>32),uint32_t(task_close));
  s.ReportReturnFailure("Bootstrap");
  s.ReportListenerDiagnostic("Bootstrap",listener_diagnostic);
 }catch(...){/* Diagnostics never alter native return, admission, or ownership. */}
}

void NativeProfileIdentityOwner::ReportFirstRefusalOnCoordinator() const noexcept {
 // Service has already joined this exact identity owner to its current
 // Kernel/service snapshot. No Kernel/config/code/provider backing is read.
 if(XThread::IsInThread() || XThread::GetCurrentFiberThread() ||
    global_critical_region::is_held_by_current_thread())return;
 try{
  const auto& s=*state_;
  s.ReportTaskOperands("Coordinator");s.ReportReferenceClose();s.ReportPublisherAbort();
  const uint32_t denial=s.first_denial_state.load(std::memory_order_acquire);
  const uint32_t loss=s.first_loss_line.load(std::memory_order_relaxed);
  const uint64_t task_close=s.first_task_close_refusal.load(std::memory_order_relaxed);
  const auto listener_diagnostic=s.CaptureListenerDiagnostic();
  // State1 is an actual in-progress writer. Never wait or read its payload.
  const bool available=denial==2;
  const uint32_t entry=available?s.first_denial_entry:0;
  const uint32_t line=available?s.first_denial_line:0;
  const uint64_t lr=available?s.first_denial_lr:0;
  const uint64_t sp=available?s.first_denial_sp:0;
  const auto args=available?s.first_denial_args:std::array<uint64_t,4>{};
  // Force an explicit snapshot including state0. Bootstrap once-only output
  // never suppresses this independent external failure-boundary observation.
  XELOGE("Native identity coordinator scalar snapshot: observed=1 "
         "first_loss_line={} first_denial_state={} payload_available={} "
         "first_denial_line={} guard_entry={:08X} caller_lr={:016X} "
         "sp={:016X} r3={:016X} r4={:016X} r5={:016X} r6={:016X} "
         "owner_retired={} task_close_reason={} task_close_target={:08X}; advisory only, source SHA binds lines; "
         "no-cell publisher refusals are not captured; listener snapshot follows",
         loss,denial,available,line,entry,lr,sp,args[0],args[1],args[2],args[3],
         s.retired.load(std::memory_order_acquire),uint32_t(task_close>>32),uint32_t(task_close));
  s.ReportReturnFailure("Coordinator");
  s.ReportListenerDiagnostic("Coordinator",listener_diagnostic);
 }catch(...){/* Diagnostics never change native admission/results/ownership. */}
}

void NativeProfileIdentityOwner::ObserveBootstrapHandoff(HandoffStage stage,bool begin_turn) noexcept {
 if(!cvars::native_system_ui_trace)return;
 auto& word=state_->bootstrap_handoff;
 const auto prior=word.load(std::memory_order_relaxed);
 if(uint32_t(prior)==UINT32_MAX)return; // saturated observation, never reused
 auto turn=uint32_t(prior>>32);
 if(begin_turn){if(turn==UINT32_MAX){word.store(UINT64_MAX,std::memory_order_release);return;}++turn;}
 if(turn)word.store((uint64_t(turn)<<32)|uint32_t(stage),std::memory_order_release);
}

void NativeProfileIdentityOwner::ReportAlignmentOnHost(const char* boundary) const noexcept {
 if(!state_->profile_enabled)return;
 if(XThread::GetCurrentFiberThread() || global_critical_region::is_held_by_current_thread())return;
 try {
  state_->ReportTaskOperands(boundary);
  state_->ReportModifyOptionsOnHost();
  struct Snapshot {
   uint64_t flags=0,read=0,published=0,epoch=0,job=0,source=0,intent=0,job_flags=0;
   uint32_t render=0,active=0,expected=0,attempts=0,cleanup=0,admission_fatal=0;
   uint32_t preentry_attempt=0,preentry_pending=0;
   bool preentry_pending_available=false,preentry_exhausted=false;
   std::array<uint64_t,PaintObservation::kCount> preentry{};
   uint32_t preentry_fence_available=0;std::array<uint64_t,PaintObservation::kScalarCount> preentry_fence{};
   std::array<State::WorkBlocker,2> preentry_work_blockers{};
   std::array<Tasks::SourceBlocker,2> preentry_source_blockers{};
   std::array<uint64_t,4> xuids{};
  } d;
  {
   const auto& s=*state_;std::lock_guard lock(s.mutex);
   if(s.retired.load(std::memory_order_acquire))return; // stopped revocation is outside this lock
   d.flags=uint64_t(s.aligned)|(uint64_t(s.boot_alignment_claimed)<<1)|
      (uint64_t(s.stopping.load())<<2)|(uint64_t(s.retired.load())<<3)|(uint64_t(s.uncertain.load())<<4)|
      (uint64_t(s.tasks.lost())<<5)|(uint64_t(s.wake && s.wake->lost())<<6);
   d.read=s.current_read_source;d.published=s.published_source.load(std::memory_order_acquire);
   d.render=uint32_t(s.render_phase);d.active=uint32_t(s.active.size());d.expected=uint32_t(s.expected.size());
   d.attempts=s.bootstrap_attempts;d.cleanup=bool(s.bootstrap_cleanup);d.admission_fatal=s.bootstrap_fatal;
   if(s.current_roster){d.epoch=s.current_roster->epoch();for(size_t i=0;i<4;++i)d.xuids[i]=s.current_roster->slots()[i].xuid;}
   if(s.job){const auto& j=*s.job;d.job=j.serial;d.source=j.source;d.intent=reinterpret_cast<uintptr_t>(j.intent.get());
    d.preentry_attempt=j.preentry_attempt;d.preentry_pending=j.preentry_pending;
    d.preentry_pending_available=j.preentry_pending_available;d.preentry_exhausted=j.preentry_observation_exhausted;
    d.preentry_work_blockers=j.preentry_work_blockers;d.preentry_source_blockers=j.preentry_source_blockers;
    for(uint32_t i=0;i<PaintObservation::kCount;++i)d.preentry[i]=j.preentry_observation.Snapshot(PaintStage(i),j.preentry_attempt);
    for(uint32_t i=0;i<PaintObservation::kScalarCount;++i)
     if(const auto value=j.preentry_observation.ScalarSnapshot(PaintObservation::Scalar(i),j.preentry_attempt)){
      d.preentry_fence_available|=1u<<i;d.preentry_fence[i]=*value;
     }
    d.job_flags=uint64_t(j.bootstrap_alignment.load())|(uint64_t(j.canceled.load())<<1)|
      (uint64_t(j.entering.load())<<2)|(uint64_t(j.native_entered.load())<<3)|(uint64_t(j.native_returned.load())<<4)|
      (uint64_t(j.transport_running.load())<<5)|(uint64_t(j.execution_uncertain.load())<<6)|
      (uint64_t(j.data_worker_ready.load())<<7)|(uint64_t(j.publisher_observed.load())<<8)|
      (uint64_t(j.render_refresh_observed.load())<<9)|(uint64_t(j.old_paint_drained.load())<<10)|
      (uint64_t(j.new_paint_drained.load())<<11)|(uint64_t(j.want_final_barrier.load())<<12)|
      (uint64_t(j.done.load())<<14); // bit13 unavailable: joined is not State-locked
   }
  }
  const uint32_t cut=boundary && !std::strcmp(boundary,"ForegroundReady")?3:
      boundary && !std::strcmp(boundary,"Bootstrap")?1:2;
  static NativeSystemUiTraceLimiter trace;
  trace.EmitTransition(d.flags^(d.job_flags<<16)^d.source^(uint64_t(d.render)<<56),"identity","alignment_state",
    {{"boundary",cut},{"flags",d.flags},{"current_read",d.read},{"published",d.published},{"current_epoch",d.epoch},
     {"render_phase",d.render},{"active",d.active},{"expected",d.expected},{"job_serial",d.job},{"job_source",d.source},
     {"intent",d.intent},{"job_flags",d.job_flags},{"admission_attempts",d.attempts},
     {"admission_cleanup",d.cleanup},{"admission_fatal",d.admission_fatal}},
     {{"xuid0",d.xuids[0]},{"xuid1",d.xuids[1]},{"xuid2",d.xuids[2]},{"xuid3",d.xuids[3]}});
  static std::array<NativeSystemUiTraceLimiter,PaintObservation::kCount> preentry_trace;
  for(uint32_t i=0;i<PaintObservation::kCount;++i){
   const auto word=d.preentry[i];const auto phase=PaintObservation::Phase(word);
   // Attempt identity is payload, not a reason to defeat bounded sampling.
   preentry_trace[i].EmitTransition((word&((1ull<<35)-1))^d.job,"identity","preentry_original_cut",
    {{"boundary",cut},{"job_serial",d.job},{"job_source",d.source},{"current_read",d.read},
     {"stage",i},{"attempt",d.preentry_attempt},{"phase",phase},{"result_available",phase==2},
     {"result",phase==2 && PaintObservation::Result(word)},
     {"evaluated",PaintObservation::Evaluated(word)},{"predicate_true",PaintObservation::Truth(word)},
     {"captured_pending_available",d.preentry_pending_available},{"captured_pending",d.preentry_pending},
     {"exhausted",d.preentry_exhausted}},
     {{"fence_available",d.preentry_fence_available},{"raw_completion_error",d.preentry_fence[0]},
      {"raw_completed_submission",d.preentry_fence[1]},{"raw_upcoming_submission",d.preentry_fence[2]}});
  }
  static std::array<NativeSystemUiTraceLimiter,2> work_blockers,source_blockers;
  for(uint32_t i=0;i<2;++i){
   const auto& w=d.preentry_work_blockers[i];
   work_blockers[i].EmitTransition(d.job^w.serial^(uint64_t(w.state)<<32)^w.closed,"identity","preentry_work_blocker",
    {{"boundary",cut},{"cut",i},{"job_serial",d.job},{"attempt",d.preentry_attempt},{"available",w.available},
     {"closed",w.closed},{"reason",w.reason},{"work",w.serial},{"source",w.source},{"state",w.state},
     {"thread",w.frame.thread},{"pcr",w.frame.pcr},{"sp",w.frame.sp},{"lr",w.frame.lr},
     {"function",w.frame.function},{"generation",w.birth.generation},{"task",w.birth.task},
     {"birth_origin",w.birth.origin},{"first_argument",w.first_argument}});
   const auto& b=d.preentry_source_blockers[i];
   source_blockers[i].EmitTransition(d.job^b.source^b.generation^b.serial^(uint64_t(b.state)<<32)^(uint64_t(b.kind)<<56)^b.closed,"identity","preentry_source_blocker",
    {{"boundary",cut},{"cut",i},{"job_serial",d.job},{"attempt",d.preentry_attempt},{"available",b.available},
     {"closed",b.closed},{"reason",b.kind},{"source",b.source},{"generation",b.generation},{"task",b.task},
     {"recorded_callback",b.callback},{"recorded_argument",b.argument},{"output_slot",b.output},{"state",b.state},
     {"serial",b.serial},{"thread",b.frame.thread},{"pcr",b.frame.pcr},{"sp",b.frame.sp},
     {"lr",b.frame.lr},{"function",b.frame.function}});
  }
  const auto emit=[&](NativeSystemUiTraceLimiter& limiter,const char* event,
                      const State::BootstrapFailureCell& cell){
   const auto complete=cell.state.load(std::memory_order_acquire);
   State::BootstrapFailure value;if(complete==2)value=cell.payload;
   limiter.EmitTransition(complete,"identity",event,
     {{"boundary",cut},{"capture_state",complete},{"stage",value.stage},{"line",value.line},
      {"error",value.error},{"category",value.category},{"retryable",value.retryable},
      {"evaluated",value.evaluated},{"failed",value.failed},{"intent_phase",value.intent_phase}});
  };
  static NativeSystemUiTraceLimiter refusal,fatal;
  emit(refusal,"initial_admission_refusal",state_->first_bootstrap_refusal);
  emit(fatal,"initial_admission_fatal",state_->first_bootstrap_fatal);
 }catch(...){/* Observations across cuts are not a roster transaction. */}
}

NativeProfileIdentityOwner::NativeProfileIdentityOwner(std::shared_ptr<State> state)
    :state_(std::move(state)){}

bool NativeProfileIdentityOwner::ClaimKernelBeforeHeap(
    KernelState& kernel,const GuestRuntimeCallExecutor& calls,
    const std::shared_ptr<const xam::ProfileRosterNativeBinding>& binding) {
 if(calls.kernel()!=&kernel || !binding || !binding->nonce() ||
    CheckNativeProfileIdentityPlanFamily(calls.validated())!=NativeProfileIdentityPlanFamily::kComplete ||
    calls.heap_runtime()->GetSnapshot().state!=GuestRuntime::State::kResourcesLoaded)return false;
 bool no=false;
 return kernel.native_profile_identity_claimed_.compare_exchange_strong(no,true,std::memory_order_acq_rel);
}

void NativeProfileIdentityOwner::ObserveInitialEmptyProfileBaseline(
    XThread& bootstrap, NativeInitialEmptyProfileWitness&& witness) noexcept try {
 auto& s=*state_;
 if(!s.profile_enabled || ActualThread()!=&bootstrap || bootstrap.kernel_state()!=s.kernel ||
    bootstrap.thread_id()!=s.config.allocation->bootstrap_thread_id() ||
    bootstrap.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    XThread::GetCurrentFiberThread() || global_critical_region::is_held_by_current_thread())return;
 auto candidate=std::make_shared<State::InitialEmptyBaseline>(std::move(witness));
 {
  std::lock_guard lock(s.mutex);
  if(s.initial_empty_capture_seen || s.initial_empty_consumed)return;
  s.initial_empty_capture_seen=true;
  if(!s.InitialEmptyWindow())return;
  candidate->ticket=s.current_ticket;
 }
 // Runtime already sealed the native constructor witness in this exact
 // phase. Capture PM metadata here; the first full-startup Step performs the
 // single fresh native validation outside State/PM locks, without minting again.
 auto* pm=s.kernel->xam_state()->profile_manager();std::error_code error;
 candidate->expected=pm->CaptureRosterExpected(error);
 if(error || !candidate->expected.roster)return;
 const auto sequence=pm->CaptureInitialEmptyNativeRosterSequence(
     s.config.factory_binding,candidate->expected,candidate->ticket);
 if(!sequence)return;
 candidate->operation_sequence=*sequence;
 {
  std::lock_guard lock(s.mutex);
  if(s.initial_empty_consumed || !s.InitialEmptyWindow() || candidate->ticket!=s.current_ticket ||
     candidate->expected.roster!=s.current_roster)return;
  s.initial_empty_baseline=candidate;
 }
}catch(...){/* Optional equivalence unavailable: keep genuine mutation path. */}

bool NativeProfileIdentityOwner::State::ConsumeInitialEmptyBaseline(XThread& bootstrap) noexcept try {
 std::shared_ptr<InitialEmptyBaseline> candidate;
 {
  std::lock_guard lock(mutex);
  if(initial_empty_consumed)return false;
  initial_empty_consumed=true;candidate=std::move(initial_empty_baseline);
 }
 if(!candidate || ActualThread()!=&bootstrap || bootstrap.kernel_state()!=kernel ||
    bootstrap.thread_id()!=config.allocation->bootstrap_thread_id() ||
    bootstrap.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    XThread::GetCurrentFiberThread() || global_critical_region::is_held_by_current_thread() ||
    !candidate->witness.Matches(config.catalog->calls(),config.allocation))return false;
 auto visible=std::make_shared<IdentityPixelGuard>(kInitialReadSource,true,pixel_lifetime);
 auto* pm=kernel->xam_state()->profile_manager();
 std::shared_ptr<IdentityPixelGuard> displaced;
 {
  std::lock_guard lock(mutex);
  // Existing friend/recursive metadata gate, TRY only: never add a blocking
  // State -> PM inversion or run native/IO work while either lock is held.
  std::unique_lock metadata(pm->profile_mount_mutex_,std::try_to_lock);
  if(!metadata.owns_lock() || !InitialEmptyWindow() || candidate->ticket!=current_ticket ||
     candidate->expected.roster!=current_roster)return false;
  const auto sequence=pm->CaptureInitialEmptyNativeRosterSequence(
      config.factory_binding,candidate->expected,candidate->ticket);
  if(!sequence || *sequence!=candidate->operation_sequence)return false;
  displaced=std::move(pixels);pixels=std::move(visible);aligned=true;
  // No publisher/Job/Intent/epoch/event/Wake completion. Source2, nonce0,
  // fixed ticket and next_source3 remain the truthful constructor baseline.
 }
 changed.notify_all();return true;
}catch(...){return false;}

std::shared_ptr<NativeProfileIdentityOwner> NativeProfileIdentityOwner::CreateBeforeWorkers(
    Config config,std::string* error) {
 const auto fail=[&](const char* why)->std::shared_ptr<NativeProfileIdentityOwner>{if(error)*error=why;return {};};
 if(!config.catalog || !config.domain || !config.allocation || !config.renderer ||
    !config.factory_binding || !config.factory_binding->nonce())return fail("Missing actual identity factory owners");
 const auto calls=config.catalog->calls();
 auto* kernel=calls->kernel();
 auto* actual=XThread::IsInThread()?XThread::GetCurrentThread():nullptr;
 if(!actual || actual->kernel_state()!=kernel ||
    actual->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    actual->thread_id()!=config.allocation->bootstrap_thread_id() ||
    !kernel->native_profile_identity_claimed_.load(std::memory_order_acquire) ||
     !config.domain->active() || !config.allocation->IsAccepting() ||
    !config.allocation->MatchesDomain(config.domain) ||
    config.domain->root()!=NativeImportDomain::Member{calls->module()->hmodule_ptr(),calls->load_generation()} ||
    CheckNativeProfileIdentityPlanFamily(calls->validated())!=NativeProfileIdentityPlanFamily::kComplete ||
    !config.catalog->system_content())return fail("Actual selected identity SYSTEM/root/content proof failed");
 auto state=std::make_shared<State>(std::move(config));
 state->provider_binding=calls->CaptureProfileIdentityProviderBinding();
 state->root_code=kernel->AcquireUserModuleExecutionLease(calls->module());
 state->prepared_title.module=kernel->GetExecutableModule();
 if(state->prepared_title.module){
  state->prepared_title_generation=state->prepared_title.module->load_generation();
  state->prepared_title.lease=kernel->AcquireUserModuleExecutionLease(state->prepared_title.module);
 }
 state->resident_domain=state->config.domain->Acquire();
 if(!state->provider_binding || state->provider_binding->issuer_!=calls.get() ||
    !state->root_code || !state->prepared_title.lease || !state->prepared_title_generation ||
    !state->resident_domain || !CheckIdentityCode(*calls))
  return fail("Identity exact implemented bridge transaction/code generation unavailable");
 state->task_diagnostics_enabled=CheckTaskDiagnosticCode(*calls);
 if(state->config.catalog->periodic_service_stop()){
  if(!CheckPeriodicCode(*calls))return fail("Independent periodic factory native code unavailable");
  auto guard=global_critical_region::AcquireDirect();
  const auto object=Word(*kernel->memory(),0x81A96838);
  if(!object || *object!=0x81B2A370u ||
     kernel->GetUserModuleForAddress(*object).get()!=calls->module().get() ||
     kernel->GetUserModuleForAddress(*object+0x207).get()!=calls->module().get())
   return fail("Independent periodic class registration/root generation unavailable");
  state->periodic_object=*object;
 }
 auto* pm=kernel->xam_state()->profile_manager();std::error_code ec;
 const auto expected=pm->CaptureRosterExpected(ec);
 if(ec || !expected.roster)return fail("Current common roster authority unavailable");
 state->current_roster=expected.roster;
 state->current_ticket=pm->CaptureCurrentNativeReadTicket(state->config.factory_binding,ec);
 if(ec || !state->current_ticket)return fail("Actual common native roster holder unavailable");
 for(auto& slot:state->boot_reads){
  slot=xam::ProfileRosterNativeReadScope::Prepare(state->current_ticket,state->config.factory_binding);
  if(!slot)return fail("Preallocated native whole-work roster scope unavailable");
 }
 const NativeIdentityEventLedger::Root event_root{state->root.kernel,state->root.domain,state->root.generation};
 state->listeners=std::unique_ptr<Listeners>(new Listeners(event_root));
 auto boot_lifetime=std::make_shared<State::EventLifetime>();
 boot_lifetime->code=state->root_code;boot_lifetime->domain=state->config.domain->Acquire();
 boot_lifetime->thread=retain_object(actual);
 if(!boot_lifetime->domain)return fail("Native boot event domain no longer accepting");
 state->boot_source=std::shared_ptr<const NativeIdentityEventSource>(
     new NativeIdentityEventSource(event_root,kBootUnjoinedSource,0,boot_lifetime));
 state->boot_origin=state->listeners->AdoptSource(state->boot_source);
 if(!state->boot_origin)return fail("Native boot event source registration failed");
 state->current_event_source=std::shared_ptr<const NativeIdentityEventSource>(
     new NativeIdentityEventSource(event_root,kInitialReadSource,0,boot_lifetime));
 state->current_event_origin=state->listeners->AdoptSource(state->current_event_source);
 if(!state->current_event_origin)return fail("Initial unjoined common read source registration failed");
 {auto lock=global_critical_region::AcquireDirect();
  if(kernel->memory()->QueryRangeAccess(0x81AAC26C,4)==memory::PageAccess::kNoAccess)
   return fail("Actual native pending mapping unavailable");
  state->native_pending=kernel->memory()->TranslateVirtual<uint32_t*>(0x81AAC26C);
 }
 state->active.reserve(256);state->host_retired.reserve(256);
 state->expected.reserve(64);state->retired_expected.reserve(64);
 state->apc_code_until_actual_stop.reserve(256);
 auto owner=std::shared_ptr<NativeProfileIdentityOwner>(new NativeProfileIdentityOwner(std::move(state)));
 if(!pm->InstallNativeRosterOwner(owner->state_->config.factory_binding,owner))
  return fail("Exact common roster factory target installation failed");
 kernel->RetainNativeResourceUntilTeardown(owner);
 if(error)error->clear();return owner;
}

std::optional<NativeProfileIdentityOwner::SaveSelectionCandidates>
NativeProfileIdentityOwner::CaptureSaveSelectionCandidates() const noexcept try {
 const auto& s=*state_;std::lock_guard lock(s.mutex);
 if(!s.profile_enabled || s.retired.load(std::memory_order_acquire))return {};
 return SaveSelectionCandidates{s.current_roster,s.job?s.job->intent:nullptr,s.job?s.job->source:0};
}catch(...){return {};}

bool NativeProfileIdentityOwner::HasProfileIdentityAuthority() const noexcept {
 return state_->profile_enabled && !state_->retired.load(std::memory_order_acquire);
}
std::shared_ptr<NativeProfileIdentityOwner> NativeProfileIdentityOwner::CreateExecutionBeforeWorkers(
    Config config,std::string* error) {
 const auto fail=[&](const char* why)->std::shared_ptr<NativeProfileIdentityOwner>{if(error)*error=why;return {};};
 if(!config.catalog || !config.domain || !config.allocation || config.renderer || config.factory_binding)
  return fail("Common Task factory requires execution-only owners");
 const auto calls=config.catalog->calls();auto* kernel=calls?calls->kernel():nullptr;
 auto* thread=ActualThread();
 if(!kernel || !thread || thread->kernel_state()!=kernel ||
    thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    thread->thread_id()!=config.allocation->bootstrap_thread_id() ||
    !config.domain->active() || !config.allocation->IsAccepting() ||
    !config.allocation->MatchesDomain(config.domain) ||
    config.domain->root()!=NativeImportDomain::Member{calls->module()->hmodule_ptr(),calls->load_generation()} ||
    !CheckTaskExecutionCode(*calls))return fail("Actual common Task root/domain/code unavailable");
 auto state=std::make_shared<State>(std::move(config));
 state->root_code=kernel->AcquireUserModuleExecutionLease(calls->module());
 state->resident_domain=state->config.domain->Acquire();
 if(!state->root_code || !state->resident_domain)return fail("Common Task execution lifetime unavailable");
 state->active.reserve(256);state->host_retired.reserve(256);
 state->expected.reserve(64);state->retired_expected.reserve(64);
 state->execution_lifetimes.reserve(Tasks::kCapacity);
 auto owner=std::shared_ptr<NativeProfileIdentityOwner>(new NativeProfileIdentityOwner(std::move(state)));
 kernel->RetainNativeResourceUntilTeardown(owner);
 if(error)error->clear();return owner;
}

bool NativeProfileIdentityOwner::BindImageObserverSlot(
    const std::shared_ptr<NativeIdentityTileObserverSlot>& slot) noexcept {
 try{
  if(!slot || slot->kernel_!=state_->kernel)return false;
  const auto installed=state_->kernel->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  if(!installed || installed.get()!=slot.get() || installed.owner_before(slot) ||
     slot.owner_before(installed))return false;
  std::lock_guard lock(state_->mutex);
  const auto actual=slot->current_.load(std::memory_order_acquire);
  const auto self=weak_from_this();
  if(state_->retired.load() || (state_->image_slot &&
     (state_->image_slot.get()!=slot.get() || state_->image_slot.owner_before(slot) ||
      slot.owner_before(state_->image_slot))) ||
     (!actual.expired() && (actual.owner_before(self) || self.owner_before(actual))))return false;
  state_->image_slot=slot;slot->current_.store(self,std::memory_order_release);return true;
 }catch(...){if(slot)slot->unavailable_.store(true,std::memory_order_release);return false;}
}
void NativeProfileIdentityOwner::ObserveImageTileWrite(
    const std::shared_ptr<NativeIdentityTileObserverSlot>& slot,
    const std::array<std::pair<uint32_t,uint32_t>,2>& spans,uint32_t phase,
    const NativeIdentityTileWork* actual_work) noexcept {
 try{
  if(!global_critical_region::is_held_by_current_thread() || !slot || phase>3)return;
  auto& s=*state_;std::lock_guard lock(s.mutex);
  if(s.retired.load() || slot->kernel_!=s.kernel)return;
  const auto current=slot->current_.load(std::memory_order_acquire);
  const auto self=weak_from_this();
  if(s.retired.load() || !s.image_slot || s.image_slot.get()!=slot.get() ||
     s.image_slot.owner_before(slot) || slot.owner_before(s.image_slot) || current.owner_before(self) ||
     self.owner_before(current))return;
  // This is deny-only producer overlap, not adoption or a permission lookup.
  // GuestIo already owns GCRC. Its existing overlap helper accounts for actual
  // physical aliases while all provider fields remain under the one owner.
  for(const auto& provider:s.providers){
   if(!provider || !provider->initialized || provider->destroyed)continue;
   const bool overlap=std::any_of(spans.begin(),spans.end(),[&](const auto& span){
    // Revocation already closes this buffer's native image epoch. Its old
    // numeric address no longer owns unrelated writes after free/reuse;
    // the still-live provider object remains independently observed below.
    return span.second && ((!provider->buffer_revoked &&
        util::GuestIoRangesOverlap(*s.kernel->memory(),span.first,
            span.second,provider->buffer,provider->capacity)) ||
        util::GuestIoRangesOverlap(*s.kernel->memory(),span.first,span.second,
            provider->object,0xD8));
   });
   if(!overlap)continue;
   provider->image_epoch.Invalidate();provider->image_output.reset();provider->no_read_query.reset();
   if(provider->image_uri_generation==UINT64_MAX)
    slot->unavailable_.store(true,std::memory_order_release);
   else ++provider->image_uri_generation;
   const auto data=actual_work?actual_work->data_:nullptr;
   if(data && provider->last_output.get()==data.get() &&
      !provider->last_output.owner_before(data) && !data.owner_before(provider->last_output) &&
      data->provider_generation==provider->generation &&
      data->provider==provider->object && data->output==provider->buffer &&
      data->capacity==provider->capacity){
    data->image_store_publication.store(0,std::memory_order_release);
    if(phase==2 && provider->image_epoch.live()){
     data->image_store_allocation.store(provider->image_epoch.allocation(),std::memory_order_relaxed);
     data->image_store_publication.store(provider->image_epoch.publication(),std::memory_order_release);
    }
   }
  }
 }catch(...){slot->unavailable_.store(true,std::memory_order_release);}
}
NativeIdentityTilePublication::NativeIdentityTilePublication(KernelState* kernel,
    std::array<Span,2> spans,const NativeIdentityTileWork* actual_work) noexcept
    :kernel_(kernel),spans_(spans),actual_work_(actual_work){
 // A caller already inside GCRC cannot promise an out-of-GCRC final
 // Owner release. Keep only metadata aliases and deny current image credit
 // at EVERY actual cut, including owner replacement before/after stores.
 borrow_owners_=!global_critical_region::is_held_by_current_thread();
 Observe(0);
}
NativeIdentityTilePublication::~NativeIdentityTilePublication(){
 if(stores_entered_ && !stores_settled_)StoresUncertain();
 // Inline strong Owner/slot holders retire only here, after GuestIo's GCRC.
}
void NativeIdentityTilePublication::Observe(uint32_t phase) noexcept {
 const size_t index=phase==0?0:phase==1?1:2;
 try{
  if(!kernel_)return;
  // A call-attempt capture may precede the aggregate IO. Actual pre/poststore
  // cuts reload the Kernel alias, so null/stale earlier captures grant nothing.
  auto global=global_critical_region::AcquireDirect();
  slots_[index]=kernel_->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  const auto& slot=slots_[index];if(!slot || slot->kernel_!=kernel_)return;
  if(!borrow_owners_){slot->unavailable_.store(true,std::memory_order_release);return;}
  owners_[index]=slot->current_.load(std::memory_order_acquire).lock();
  if(owners_[index])owners_[index]->ObserveImageTileWrite(slot,
      {{{spans_[0].address,spans_[0].bytes},{spans_[1].address,spans_[1].bytes}}},phase,actual_work_);
 }catch(...){if(slots_[index])slots_[index]->unavailable_.store(true,std::memory_order_release);}
}
void NativeIdentityTilePublication::BeforeStores() noexcept {
 if(stores_entered_)return;stores_entered_=true;Observe(1);
}
void NativeIdentityTilePublication::StoresCommitted() noexcept {
 if(!stores_entered_ || stores_settled_)return;stores_settled_=true;stores_committed_=true;Observe(2);
}
void NativeIdentityTilePublication::StoresUncertain() noexcept {
 if(!stores_entered_ || stores_settled_)return;stores_settled_=true;Observe(3);
}

void NativeProfileIdentityOwner::ObserveImageEntry(void* raw,uint32_t at) noexcept {
 State::HookUse hook_use;
 if(!raw)return;auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 std::shared_ptr<NativeIdentityTileObserverSlot> slot,current_slot;
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 try{
  if(!c.kernel_state)return;
  slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  if(!slot || slot->kernel_!=c.kernel_state)return;
  owner=slot->current_.load(std::memory_order_acquire).lock();if(!owner)return;
  auto& state=*owner->state_;if(!state.profile_enabled)return;
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  std::shared_ptr<NativeImportDomain> domain;
  {
   std::lock_guard lock(state.mutex);
   if(state.retired.load() || !state.config.catalog)return;
   if(!hook_use.Acquire(state.hook_rundown,owner->state_,false))return;
   calls=state.config.catalog->calls();domain=state.config.domain;
  }
  if(at==0x8172D590){
   const auto actual=state.ImageShape(c,at);if(!actual)return;
   std::lock_guard lock(state.mutex);
   const auto* parent=state.ImageParent(*actual,reinterpret_cast<uintptr_t>(&c));
   if(!parent || !((parent->frame.function==0x816B0B50 && actual->lr==0x816B0B88 && parent->input) ||
       (parent->frame.function==0x81839C18 && actual->lr==0x81839C50 && parent->copied) ||
       (parent->frame.function==0x8183A180 &&
        (actual->lr==0x8183A1E4 || actual->lr==0x8183A210) && parent->copied)))return;
  }
  auto global=global_critical_region::AcquireDirect();
  if(!calls || !CheckImageBody(*calls,at)){slot->unavailable_.store(true,std::memory_order_release);return;}
  const auto frame=state.ImageShape(c,at);
  if(!frame){slot->unavailable_.store(true,std::memory_order_release);return;}
  std::optional<uint32_t> actual_task;
  // Only these draw wrappers/real upload callers consume ImageCall::render.
  // Constructors, rebinding, withdrawal and decode still observe their actual
  // native lifecycle; they do not borrow a Task merely to fill unused metadata.
  const bool consumes_render=at==0x81798740 || at==0x81798788 ||
      at==0x81839C18 || at==0x8183A180;
  bool possible_render=false;
  if(consumes_render){
   std::lock_guard lock(state.mutex);
   possible_render=std::any_of(state.active.begin(),state.active.end(),[&](const auto& work){
    return work->render_iteration && !work->normal_return && !work->uncertain &&
        work->frame.thread==frame->thread && work->frame.pcr==frame->pcr && frame->sp<=work->frame.sp;
   });
  }
  // The preliminary metadata filter grants nothing. A useful candidate still
  // performs the complete original code check and real TLS/Task/birth match.
  if(possible_render && domain && CheckTaskReferenceCode(*calls))
   actual_task=NativeProfileIdentityTaskContext::Read(*calls,*domain,at,c);
  current_slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  std::lock_guard lock(state.mutex);
  const auto current=slot->current_.load(std::memory_order_acquire);const auto self=owner->weak_from_this();
  if(state.retired.load() || state.image_slot.get()!=slot.get() || current_slot.get()!=slot.get() ||
     current_slot.owner_before(slot) || slot.owner_before(current_slot) ||
     current.owner_before(self) || self.owner_before(current) || slot->unavailable_.load())return;
  const auto* code=NativeImageCode(at);
  if(!code || frame->sp<code->frame_bytes || state.next_image_call==UINT64_MAX){state.LoseImageObservation();return;}
  const auto empty=std::find_if(state.image_calls.begin(),state.image_calls.end(),[](const auto& v){return !v;});
  if(empty==state.image_calls.end()){state.LoseImageObservation();return;}
  State::ImageCall call;call.frame=*frame;call.body_sp=frame->sp-code->frame_bytes;
  call.context=reinterpret_cast<uintptr_t>(&c);call.cpu=reinterpret_cast<uintptr_t>(c.thread_state);
  call.serial=++state.next_image_call;
  std::copy(std::begin(c.r)+3,std::begin(c.r)+11,call.args.begin());
  // Only operands used by this observation are typed DWORDs/pointers. Other
  // original argument words remain opaque and execution is never denied.
  uint32_t used=0;
  switch(at){
   case 0x817B4A80:case 0x817ADD70:case 0x817ACA98:case 0x8177C870:used=3;break;
   case 0x817B4A50:case 0x8177EA08:case 0x81780C50:case 0x81780CE0:case 0x8177EB58:
   case 0x8177E728:case 0x81783688:case 0x81780DD0:case 0x817C7B38:
   case 0x8177D1D0:case 0x81785308:case 0x8177D000:case 0x81798740:
   case 0x81798788:case 0x81839590:
   case 0x81839780:case 0x81839C60:used=1;break;
   case 0x817CC858:case 0x817CDF08:case 0x8177D288:case 0x8178E138:
   case 0x816B0B50:case 0x8172D590:case 0x817DB560:case 0x817DD2E0:case 0x816C8E10:
   case 0x81798270:used=7;break;
   case 0x81839C18:used=0x09;break; // R3 device/R6 vertex pointer; scalars are native MULLW operands
   case 0x8183A180:used=0xA1;break; // R3 device/R8 index pointer/R10 vertex pointer
   case 0x8177E778:case 0x816C8D68:used=3;break;
   case 0x817851B8:case 0x816C82C8:case 0x817836D8:used=15;break;
   case 0x81784D80:used=31;break;
  }
  for(size_t i=0;i<call.args.size();++i)if((used&(1u<<i)) && !DwordIs(call.args[i],uint32_t(call.args[i]))){
   state.LoseImageObservation();return;
  }
  if(auto* parent=state.ImageParent(*frame,call.context)){
   const auto branch=Word(*state.kernel->memory(),frame->lr-4);
   bool actual_call=branch && *branch==0x4E800421;
   if(branch && (*branch&0xFC000001u)==0x48000001u){
    int32_t delta=int32_t(*branch&0x03FFFFFCu);if(delta&0x02000000)delta|=int32_t(0xFC000000u);
    const uint32_t target=(*branch&2)?uint32_t(delta):uint32_t(frame->lr-4+delta);
    actual_call=target==at;
   }
   if(actual_call)call.parent=parent->serial;
  }
  if(actual_task)if(const auto* task=state.tasks.FindLiveTask(*actual_task))
   for(auto i=state.active.rbegin();i!=state.active.rend();++i){const auto& render=*i;
    if(render->render_iteration && !render->normal_return && !render->uncertain &&
       render->frame.thread==frame->thread && render->frame.pcr==frame->pcr &&
       frame->sp<=render->frame.sp && render->birth.task==*actual_task &&
       render->birth.generation==task->birth.generation){call.render=render->serial;break;}
   }
  const auto capture_key=[&](uint64_t actual){
   if(!DwordIs(actual,uint32_t(actual)))return;
   if(const auto key=state.ImageInputAt(uint32_t(actual))){
    const auto version=key->input.provider?key->input.provider->version:key->input.section->version;
    if(!NativeMemoryKeyMatches(*state.kernel->memory(),uint32_t(actual),key->input.address(),key->input.size(),version))return;
    call.key_pointer=uint32_t(actual);call.input=key->input;
    call.key_provider=key->provider;call.uri_generation=key->uri_generation;call.key_generation=key->string_generation;}
  };
  using Kind=identity_detail::NativeImageJournal::Kind;
  if(at==0x8177EA08 || at==0x81780C50){
   if(DwordIs(call.args[0],uint32_t(call.args[0])))
    call.object=state.images.BeginNativeObject(at==0x8177EA08?Kind::kBrush:Kind::kDc,uint32_t(call.args[0]));
  }else if(at==0x817B4A50 || at==0x8177EB58 || at==0x8177E728 || at==0x81783688 || at==0x81780DD0){
   const auto kind=at==0x817B4A50?Kind::kInterface:at==0x8177EB58?Kind::kBrush:
       at==0x8177E728?Kind::kWrapper:Kind::kDc;
   call.object=state.images.Current(kind,uint32_t(call.args[0]));
   if(call.object)state.images.RetireObject(*call.object);
  }else if(at==0x817ADD70){
   call.object=state.images.Current(Kind::kInterface,uint32_t(call.args[0]));
  }else if(at==0x817ACA98){
   capture_key(call.args[1]);state.RevokeImageString(0,uint32_t(call.args[0]));
   if(call.args[0]>=4)call.object=state.images.Current(Kind::kInterface,uint32_t(call.args[0])-4);
   if(call.object)state.images.BeforePathChange(*call.object);
  }else if(at==0x817C7B38){
   if(call.args[0]>=4)call.object=state.images.Current(Kind::kInterface,uint32_t(call.args[0])-4);
   if(call.object)state.images.WithdrawResource(*call.object);
  }else if(at==0x817CDF08 || at==0x817CC858){
   capture_key(call.args[2]);
  }else if(at==0x8177D288){
   call.object=state.images.Current(Kind::kBrush,uint32_t(call.args[0]));capture_key(call.args[2]);
   if(call.object)state.images.WithdrawResource(*call.object);
  }else if(at==0x8177D1D0){
   call.object=state.images.Current(Kind::kBrush,uint32_t(call.args[0]));
   if(call.object)state.images.WithdrawResource(*call.object);
  }else if(at==0x8177E778){
   const auto old=state.images.Current(Kind::kWrapper,uint32_t(call.args[0]));
   const auto old_copy=old?state.images.Copied(*old):std::optional<State::ImageTransfer>{};
   auto* parent=state.ImageBySerial(call.parent);
   const bool original_creation=parent && parent->frame.function==0x817836D8 && frame->lr==0x81783758;
   if(old || original_creation){call.object=state.images.BeginNativeObject(Kind::kWrapper,uint32_t(call.args[0]));
    if(parent && parent->copied && parent->copied->texture==uint32_t(call.args[1]))call.copied=parent->copied;
    else if(old_copy && old_copy->texture==uint32_t(call.args[1]))call.copied=old_copy;
   }
  }else if(at==0x817851B8 || at==0x816C82C8 || at==0x817836D8){
   capture_key(call.args[1]);
  }else if(at==0x81784D80){
   auto* parent=state.ImageBySerial(call.parent);
   if(parent && parent->frame.function==0x817851B8 && frame->lr==0x817852A0 &&
      parent->input && state.ImageInputStillCurrent(*parent) &&
      DwordIs(call.args[0],parent->input->address()) &&
      DwordIs(call.args[1],parent->input->size()) &&
      DwordIs(call.args[3],uint32_t(parent->args[3])) && state.ImageBytesCurrent(*parent->input)){
    call.input=parent->input;call.read_state=call.body_sp+0x98;
   }
  }else if(at==0x8178E138){
   auto* decoder=state.ImageBySerial(call.parent);
   if(decoder && decoder->frame.function==0x81784D80 && frame->lr==0x81784F18 &&
      decoder->input && DwordIs(call.args[0],uint32_t(call.args[0])) && call.args[0] &&
      DwordIs(call.args[1],decoder->read_state) && DwordIs(call.args[2],0x816B0B50)){
    const auto pointer=Word(*state.kernel->memory(),decoder->read_state);
    const auto remaining=Word(*state.kernel->memory(),decoder->read_state+4);
    if(pointer && remaining && *pointer==decoder->input->address() && *remaining==decoder->input->size()){
     call.png_context=uint32_t(call.args[0]);call.read_state=decoder->read_state;
     decoder->png_context=call.png_context;
    }
   }
  }else if(at==0x816B0B50){
   if(DwordIs(call.args[0],uint32_t(call.args[0])))
    if(auto* decoder=state.ImageDecoder(call,uint32_t(call.args[0]))){
     call.parent=decoder->serial;call.png_context=decoder->png_context;call.read_state=decoder->read_state;
     call.input=decoder->input;
    }
  }else if(at==0x8172D590){
   auto* parent=state.ImageBySerial(call.parent);bool matches=false;
   if(parent && parent->frame.function==0x816B0B50 && parent->input && parent->copy_entered &&
      frame->lr==0x816B0B88){
    matches=DwordIs(call.args[0],parent->copy_destination) && DwordIs(call.args[1],parent->source_pointer) &&
        DwordIs(call.args[2],parent->copy_length) && !parent->copy_calls &&
        !util::GuestIoRangesOverlap(*state.kernel->memory(),uint32_t(call.args[0]),uint32_t(call.args[2]),
            parent->input->address(),parent->input->size());
   }else if(parent && parent->frame.function==0x81839C18 && parent->packet_returned && frame->lr==0x81839C50){
    // Original39C44 MULLW consumes low32 and sign-extends its DWORD result.
    // Parent scalar high words are deliberately opaque; actual memcpy operands
    // still have their independently checked canonical DWORD/pointer forms.
    const uint32_t length=uint32_t(uint64_t(uint32_t(parent->args[2]))*uint32_t(parent->args[4]));
    matches=DwordIs(call.args[0],parent->copy_destination) &&
        DwordIs(call.args[1],uint32_t(parent->args[3])) && DwordIs(call.args[2],length) && !parent->copy_calls;
   }else if(parent && parent->frame.function==0x8183A180 && parent->packet_returned && parent->stride){
    if(frame->lr==0x8183A1E4){
     // Original3A1D0/1D8 MULLW;3A1DC ADD. Preserve the low32 address
     // arithmetic including negative base offsets. A noncanonical full actual
     // ADD result cannot pass this independently checked memcpy entry.
     const uint32_t offset=uint32_t(uint64_t(uint32_t(parent->args[2]))*parent->stride);
     const uint32_t source=uint32_t(uint64_t(uint32_t(parent->args[7]))+offset);
     const uint32_t length=uint32_t(uint64_t(uint32_t(parent->args[3]))*parent->stride);
     matches=!parent->copy_calls && DwordIs(call.args[0],parent->copy_destination) &&
         DwordIs(call.args[1],source) && DwordIs(call.args[2],length);
    }else if(frame->lr==0x8183A210){
     const uint32_t width=(uint32_t(parent->args[6])&4)?4:2;
     // Original3A200 MULLW,3A204 ADDI,3A208 RLWINM: each DWORD
     // wraps as native, then low2 alignment bits clear.
     const uint32_t product=uint32_t(uint64_t(uint32_t(parent->args[4]))*width);
     const uint32_t rounded=product+uint32_t(3);
     const uint32_t length=rounded&~uint32_t(3);
     matches=parent->copy_calls==1 && parent->copy_returns==1 &&
         DwordIs(call.args[0],parent->index_destination) && DwordIs(call.args[1],uint32_t(parent->args[5])) &&
         DwordIs(call.args[2],length);
    }
   }
   if(matches){call.copy_entered=true;call.copy_destination=uint32_t(call.args[0]);
    call.source_pointer=uint32_t(call.args[1]);call.copy_length=uint32_t(call.args[2]);++parent->copy_calls;
   }else{call.uncertain=true;if(parent)parent->uncertain=true;}
  }else if(at==0x81785308){
   if(DwordIs(call.args[0],uint32_t(call.args[0])))
    if(auto* decoder=state.ImageDecoder(call,uint32_t(call.args[0]))){
     decoder->error=true;call.parent=decoder->serial;
    }
  }else if(at==0x816C8D68){
   const bool range=DwordIs(call.args[0],uint32_t(call.args[0])) && DwordIs(call.args[1],uint32_t(call.args[1])) &&
       uint64_t(uint32_t(call.args[0]))+uint64_t(uint32_t(call.args[1]))*16<=0x100000000ull;
   for(uint32_t i=0;i<state.image_defaults.size();++i)
    if(!range || (uint32_t(call.args[0])<0x81A82724u+i*16 &&
       uint64_t(uint32_t(call.args[0]))+uint64_t(uint32_t(call.args[1]))*16>0x81A82714u+i*16))state.image_defaults[i].reset();
  }else if(at==0x817DB560){
   auto* init=state.ImageBySerial(call.parent);
   if(init && init->frame.function==0x816C8D68 && frame->lr==0x816C8DBC && init->default_format &&
      DwordIs(call.args[0],init->body_sp+0x60) && DwordIs(call.args[1],init->body_sp+0x50) &&
      DwordIs(call.args[2],init->body_sp+0x54) && NativeSectionUrlMatches(*state.kernel->memory(),
       uint32_t(call.args[0]),init->module,ImageLabel(init->section_label),ImageLabel(init->entry_label))){
    call.initializer=init->serial;call.resource_open=call.serial;call.row_output=init->row_output;
    call.default_index=init->default_index;call.module=init->module;call.module_generation=init->module_generation;
   }
  }else if(at==0x817DAF80 || at==0x817DD2E0){
   auto* parent=state.ImageBySerial(call.parent);
   const bool edge=parent && ((at==0x817DAF80 && parent->frame.function==0x817DB560 && frame->lr==0x817DB630) ||
       (at==0x817DD2E0 && parent->frame.function==0x817DAF80 && frame->lr==0x817DB054));
   if(edge && parent->initializer && parent->resource_open){
    call.initializer=parent->initializer;call.resource_open=parent->resource_open;
    call.row_output=parent->row_output;call.default_index=parent->default_index;
    call.module=parent->module;call.module_generation=parent->module_generation;
   }
  }else if(at==0x816C8E10){
   const auto work=state.Latest(frame->thread,0x816921C8);
   if(work && !work->uncertain && !work->normal_return && frame->lr==0x81692330 &&
      work->frame.sp>=0xB0 && work->frame.sp-0xB0==frame->sp && work->frame.pcr==frame->pcr &&
      DwordIs(call.args[0],uint32_t(call.args[0])) && uint32_t(call.args[0])<3 &&
      DwordIs(call.args[1],work->first+0x90) && DwordIs(call.args[2],work->first+0x94)){
    const auto provider=state.FindProvider(work->first);
    if(provider && provider->generation==work->provider_generation && !provider->revoked && !provider->destroyed){
     call.default_index=uint32_t(call.args[0]);call.default_source=work->source;call.default_birth=provider->generation;
     call.key_provider=provider->object;call.uri_generation=provider->image_uri_generation;
    }
   }
  }else if(at==0x81780CE0){
   call.dc=state.images.Current(Kind::kDc,uint32_t(call.args[0]));
   if(call.dc)state.images.WithdrawResource(*call.dc); // real reset, same object birth
  }else if(at==0x8177C870){
   call.dc=state.images.Current(Kind::kDc,uint32_t(call.args[0]));
   call.other=state.images.Current(Kind::kWrapper,uint32_t(call.args[1]));
   if(call.dc){call.selection=state.images.BeginDcSelection(*call.dc);
    const auto prior=Word(*state.kernel->memory(),call.dc->address+0x250);
    if(prior)call.prior_texture=*prior;
   }
   if(call.other)call.copied=state.images.Copied(*call.other);
   if(call.copied)call.native_texture=call.copied->texture;
  }else if(at==0x8177D000){
   call.dc=state.images.Current(Kind::kDc,uint32_t(call.args[0]));
  }else if(at==0x81798270 || at==0x81798740 || at==0x81798788){
   auto* parent=state.ImageBySerial(call.parent);
   const bool texture=at==0x81798270 && parent && parent->frame.function==0x8177C870 && frame->lr==0x8177C8D4;
   const bool draw=at!=0x81798270 && parent && parent->frame.function==0x8177D000 &&
       frame->lr==(at==0x81798740?0x8177D060:0x8177D08C);
   if(texture)parent->native_call_entered=true;
   if((texture || draw) && parent->dc){
    const auto low=Word(*state.kernel->memory(),parent->dc->address+0x1CC);
    if(low && DwordIs(call.args[0],*low)){
     call.dc=parent->dc;call.other=parent->other;call.selection=parent->selection;
     call.copied=texture?parent->copied:state.images.Copied(*parent->dc);
     if(draw)call.draw_selection=state.images.SelectionSerial(*parent->dc);
     if(call.copied)call.native_texture=call.copied->texture;
    }
   }
  }else if(at==0x81839780 || at==0x81839C60){
   auto* parent=state.ImageBySerial(call.parent);
   const auto original=at==0x81839780?0x81839C18u:0x8183A180u;
   const auto edge=at==0x81839780?0x81839C3Cu:0x8183A1C8u;
   if(parent && parent->frame.function==original && frame->lr==edge && parent->dc && parent->copied &&
      DwordIs(call.args[0],parent->device)){
    call.dc=parent->dc;call.copied=parent->copied;call.device=parent->device;call.native_texture=parent->native_texture;
    call.draw_selection=parent->draw_selection;
   }
  }else if(at==0x81839590 || at==0x81839C18 || at==0x8183A180){
   auto* parent=state.ImageBySerial(call.parent);
   const auto wrapper=at==0x81839590?0x81798270u:at==0x81839C18?0x81798740u:0x81798788u;
   const auto edge=at==0x81839590?0x81798298u:at==0x81839C18?0x81798774u:0x817987C4u;
   if(parent && parent->frame.function==wrapper && frame->lr==edge &&
      parent->native_call_entered && parent->dc && parent->copied &&
      DwordIs(call.args[0],parent->device)){
    call.dc=parent->dc;call.other=parent->other;call.selection=parent->selection;call.copied=parent->copied;
    call.device=parent->device;call.native_texture=parent->native_texture;call.sampler=parent->sampler;
    call.draw_selection=parent->draw_selection;
    const auto before=Word(*state.kernel->memory(),call.device+0x30);if(before)call.prior_cursor=*before;
    if(at==0x8183A180){const auto stride=Word(*state.kernel->memory(),frame->sp+0x54);if(stride)call.stride=*stride;}
   }
  }
  *empty=std::move(call);
 }catch(...){if(slot)slot->unavailable_.store(true,std::memory_order_release);}
}
void NativeProfileIdentityOwner::ObserveImageReturn(void* raw,uint32_t at) noexcept {
 State::HookUse hook_use;
 if(!raw)return;auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 std::shared_ptr<NativeIdentityTileObserverSlot> slot,current_slot;
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 try{
  if(!c.kernel_state)return;
  slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  if(!slot || slot->kernel_!=c.kernel_state)return;
  owner=slot->current_.load(std::memory_order_acquire).lock();if(!owner)return;
  auto& state=*owner->state_;if(!state.profile_enabled)return;
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  {
   std::lock_guard lock(state.mutex);
   if(state.retired.load() || !state.config.catalog)return;
   if(!hook_use.Acquire(state.hook_rundown,owner->state_,false))return;calls=state.config.catalog->calls();
  }
  {
   // Reject an unobserved own-return before whole-body authentication. This
   // proves no use or return: the final exact owner lookup still runs below.
   const auto actual=state.ImageShape(c,at);
   if(!actual){
    // CRT memcpy has always been neutral without an observed copy frame.
    // Other malformed returns keep the original deny-only availability cut.
    if(at!=0x8172D590)slot->unavailable_.store(true,std::memory_order_release);
    return;
   }
   std::lock_guard lock(state.mutex);
   if(!state.ImageAt(*actual,reinterpret_cast<uintptr_t>(&c),false))return;
  }
  auto global=global_critical_region::AcquireDirect();
  if(!calls || !CheckImageBody(*calls,at)){slot->unavailable_.store(true,std::memory_order_release);return;}
  const auto frame=state.ImageShape(c,at);
  if(!frame){slot->unavailable_.store(true,std::memory_order_release);return;}
  current_slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  std::lock_guard lock(state.mutex);
  const auto current=slot->current_.load(std::memory_order_acquire);const auto self=owner->weak_from_this();
  if(state.retired.load() || current_slot.get()!=slot.get() ||
     current_slot.owner_before(slot) || slot.owner_before(current_slot) ||
     current.owner_before(self) || self.owner_before(current) || slot->unavailable_.load())return;
  auto* call=state.ImageAt(*frame,reinterpret_cast<uintptr_t>(&c),false);if(!call)return;
  using Kind=identity_detail::NativeImageJournal::Kind;
  const uint32_t value=uint32_t(c.r[3]);
  if(at==0x817B4A80 && !value && DwordIs(call->args[1],uint32_t(call->args[1]))){
   const auto output=Word(*state.kernel->memory(),uint32_t(call->args[1]));
   if(output && *output)if(const auto object=state.images.BeginNativeObject(Kind::kInterface,*output))
    state.images.FinishNativeObject(*object,true);
  }else if((at==0x8177EA08 || at==0x81780C50) && call->object){
   state.images.FinishNativeObject(*call->object,DwordIs(c.r[3],uint32_t(call->args[0])));
  }else if(at==0x817ACA98 && !value && call->input && state.ImageInputStillCurrent(*call)){
   const auto output=Word(*state.kernel->memory(),uint32_t(call->args[0]));
   if(output && *output && state.RememberImageString(*output,uint32_t(call->args[0]),*call->input)){
    if(call->object)state.images.CopyOwnedPath(*call->object,*call->input,true);}
  }else if(at==0x8177E778 && call->object){
   const auto output=Word(*state.kernel->memory(),call->object->address+8);
   const bool assigned=output && *output==uint32_t(call->args[1]);
   state.images.FinishNativeObject(*call->object,assigned);
   if(assigned && call->copied)state.images.FinishWrapperAssignment(*call->object,*call->copied,true);
  }else if(at==0x8177D288 && call->object && call->input && int32_t(value)>=0){
   const auto actual=Word(*state.kernel->memory(),call->object->address+0x14);
   const auto wrapper=actual?state.images.Current(Kind::kWrapper,*actual):std::optional<State::ImageObject>{};
   if(wrapper)state.images.AdoptKnownWinner(*call->object,*wrapper,*call->input,true);
  }
  else if(at==0x817CC858 && !value && call->input && state.ImageInputStillCurrent(*call)){
   const auto output=Word(*state.kernel->memory(),uint32_t(call->args[0]));
   if(output && *output)state.RememberImageString(*output,uint32_t(call->args[0]),*call->input);
  }else if(at==0x8178E138 && call->png_context){
   auto* decoder=state.ImageBySerial(call->parent);
   const auto callback=Word(*state.kernel->memory(),call->png_context+0x550);
   const auto data=Word(*state.kernel->memory(),call->png_context+0x554);
   if(!decoder || !callback || *callback!=0x816B0B50 || !data || *data!=call->read_state)
    {if(decoder)decoder->uncertain=true;}
  }else if(at==0x816B0B50){
   auto* decoder=state.ImageBySerial(call->parent);
   const auto pointer=Word(*state.kernel->memory(),call->read_state);
   const auto remaining=Word(*state.kernel->memory(),call->read_state+4);
   const bool returned=decoder && call->input && call->copy_entered && !call->uncertain &&
       call->copy_calls==1 && call->copy_returns==1 &&
       pointer && remaining && *pointer==uint64_t(call->source_pointer)+call->copy_length &&
       *remaining==call->input->size()-decoder->copied_bytes-call->copy_length;
   if(decoder){
    if(returned && decoder->copy_returns!=UINT32_MAX &&
       uint64_t(decoder->copied_bytes)+call->copy_length<=decoder->input->size()){
     ++decoder->copy_returns;decoder->copied_bytes+=call->copy_length;call->copy_returned=true;
    }else decoder->uncertain=true;
   }
  }else if(at==0x8172D590){
   auto* parent=state.ImageBySerial(call->parent);
   if(parent && call->copy_entered && !call->uncertain && DwordIs(c.r[3],call->copy_destination) &&
      parent->copy_returns<parent->copy_calls)++parent->copy_returns;
   else if(parent)parent->uncertain=true;
  }else if(at==0x81784D80){
   auto* parent=state.ImageBySerial(call->parent);
   const auto texture=DwordIs(call->args[3],uint32_t(call->args[3]))?
       Word(*state.kernel->memory(),uint32_t(call->args[3])):std::optional<uint32_t>{};
   const bool actual_output=int32_t(value)>=0 && !call->uncertain && !call->error &&
       call->input && texture && *texture && call->copy_calls && call->copied_bytes &&
       call->copy_calls==call->copy_returns && state.ImageBytesCurrent(*call->input);
   if(actual_output && parent && parent->frame.function==0x817851B8){
    // This genuine decoder creates a new native texture. A reused native
    // object address cannot preserve an older future adoption/draw birth.
    state.images.RevokeNativeTexture(*texture);
    for(const auto& work:state.active)for(auto& draw:work->image_draws)
     if(draw && draw->draw.copied.texture==*texture)draw.reset();
    parent->copied=State::ImageTransfer{*call->input,call->serial,*texture};
   }
   // A failed nonlocal PNG return abandons nested observer metadata only.
   // It never synthesizes a missing callback/memcpy return or native cleanup.
   for(auto& nested:state.image_calls)if(nested && nested->serial!=call->serial &&
       nested->context==call->context && nested->cpu==call->cpu &&
       nested->frame.thread==call->frame.thread && nested->frame.pcr==call->frame.pcr &&
       nested->frame.sp<=call->body_sp)nested.reset();
  }else if((at==0x817851B8 || at==0x816C82C8) && int32_t(value)>=0 && call->copied){
   const auto texture=Word(*state.kernel->memory(),uint32_t(call->args[3]));
   auto* parent=state.ImageBySerial(call->parent);
   if(texture && *texture==call->copied->texture && parent &&
      ((at==0x817851B8 && parent->frame.function==0x816C82C8 && call->frame.lr==0x816C8320) ||
       (at==0x816C82C8 && parent->frame.function==0x817836D8 && call->frame.lr==0x8178371C &&
        uint32_t(call->args[3])==parent->body_sp+0x50)))parent->copied=call->copied;
  }
  else if(at==0x81839590 && call->dc && call->selection){
   auto* parent=state.ImageBySerial(call->parent);
   const auto stored=call->device<=UINT32_MAX-call->slot_offset?
       Word(*state.kernel->memory(),call->device+call->slot_offset):std::optional<uint32_t>{};
   if(parent && call->native_store_entered && stored && *stored==call->native_texture &&
      parent->frame.function==0x81798270){parent->native_store_entered=true;parent->native_call_returned=true;}
  }else if((at==0x81839C18 || at==0x8183A180) && call->dc && call->copied){
   auto* parent=state.ImageBySerial(call->parent);
   const auto actual=Word(*state.kernel->memory(),call->device+0x30);
   const auto texture=Word(*state.kernel->memory(),call->device+0x3278);
   const bool another_setter=std::any_of(state.image_calls.begin(),state.image_calls.end(),[&](const auto& other){
    return other && other->serial!=call->serial && other->frame.function==0x81839590 &&
        DwordIs(other->args[0],call->device);
   });
   if(parent && !call->uncertain && call->draw_store_entered && actual && *actual==call->tail &&
      texture && *texture==call->native_texture && !another_setter && call->draw_selection &&
      state.images.SelectionSerial(*call->dc)==call->draw_selection &&
      state.images.Copied(*call->dc)==call->copied &&
      call->copy_calls==(at==0x81839C18?1u:2u) && call->copy_returns==call->copy_calls){
    parent->draw_store_entered=true;parent->tail=call->tail;parent->prior_cursor=call->prior_cursor;
    parent->native_call_returned=true;parent->packet_returned=call->packet_returned;
    if(call->render==parent->render)
     parent->emitted_draw=state.images.ObserveRetainedDrawCall(*call->dc,call->draw_selection,*call->copied,
         call->render,call->serial,call->device,call->tail,true);
    parent->packet_address=call->packet_address;parent->packet_header=call->packet_header;
    parent->packet_viz=call->packet_viz;parent->packet_initiator=call->packet_initiator;
    parent->texture_fetch=call->texture_fetch;
   }
  }else if(at==0x81798270 && call->dc && call->selection){
   auto* parent=state.ImageBySerial(call->parent);
   if(parent && parent->frame.function==0x8177C870 && call->native_call_entered &&
      call->native_call_returned && call->native_store_entered && !call->uncertain){
    parent->device=call->device;parent->native_call_returned=true;parent->native_store_entered=true;
   }
  }else if(at==0x8177C870 && call->dc && call->selection){
   const auto actual=Word(*state.kernel->memory(),call->dc->address+0x250);
   const auto low=Word(*state.kernel->memory(),call->dc->address+0x1CC);
   const auto device=low?Word(*state.kernel->memory(),*low+0xC):std::optional<uint32_t>{};
   const bool known=actual && device && call->other && call->copied && *actual==call->native_texture;
   const bool stored=known && call->native_call_returned && call->native_store_entered && call->device==*device;
   const bool same=known && !call->native_call_entered && call->prior_texture==*actual;
   // Every genuine C870 return settles this one observation attempt. A
   // missing/unknown input grants nothing, while an absent return stays pending.
   state.images.SelectDcTexture(*call->selection,call->other.value_or(State::ImageObject{}),
       device.value_or(0),actual.value_or(0),stored,same);
  }else if((at==0x81798740 || at==0x81798788) && call->dc && call->copied && call->render &&
      call->native_call_entered && call->native_call_returned && call->draw_store_entered && !call->uncertain){
   // The inner actual upload/store/own-return froze A. A later independent
   // B selection cannot rewrite that emitted fact; genuine native object and
   // Source/pin currentness are checked again at immutable frame capture.
   const auto observed=call->emitted_draw;
   if(observed && observed->selection==call->draw_selection && observed->render==call->render &&
      observed->copied==*call->copied && observed->copied.texture==call->native_texture)for(const auto& work:state.active)if(work->serial==call->render &&
       work->render_iteration && !work->normal_return && !work->uncertain &&
       work->source==observed->copied.input.source()){
    const auto empty=std::find_if(work->image_draws.begin(),work->image_draws.end(),[](const auto& v){return !v;});
    if(empty==work->image_draws.end())work->image_draws_unavailable=true;
    else if(call->packet_returned)*empty=identity_detail::NativeImageDraw{*observed,call->packet_address,
        call->packet_header,call->packet_viz,call->packet_initiator,call->texture_fetch};
    break;
   }
  }
  else if((at==0x81839780 || at==0x81839C60) && call->packet_address && !call->uncertain){
   auto* parent=state.ImageBySerial(call->parent);
   const auto header=Word(*state.kernel->memory(),call->packet_address);
   const auto viz=Word(*state.kernel->memory(),call->packet_address+4);
   const auto initiator=Word(*state.kernel->memory(),call->packet_address+8);
   bool actual=parent && header && *header==call->packet_header && viz && *viz==call->packet_viz &&
       initiator && *initiator==call->packet_initiator;
   if(at==0x81839C60){
    const auto base=Word(*state.kernel->memory(),call->packet_address+12),size=Word(*state.kernel->memory(),call->packet_address+16);
    actual=actual && base && *base==call->packet_base && size && *size==call->packet_size;
   }
   if(actual){parent->packet_returned=true;parent->packet_address=call->packet_address;
    parent->packet_header=call->packet_header;parent->packet_viz=call->packet_viz;parent->packet_initiator=call->packet_initiator;
    if(at==0x81839780 && DwordIs(c.r[3],uint32_t(c.r[3])) && c.r[3])parent->copy_destination=uint32_t(c.r[3]);
    if(at==0x81839C60 && !value){
     const auto vertices=Word(*state.kernel->memory(),parent->body_sp+0x60),indices=Word(*state.kernel->memory(),parent->body_sp+0x64);
     if(vertices && indices){parent->copy_destination=*vertices;parent->index_destination=*indices;}
    }
   }
  }else if((at==0x817DD2E0 || at==0x817DAF80) && int32_t(value)>=0 && call->section_returned){
   auto* parent=state.ImageBySerial(call->parent);
   if(parent && parent->initializer==call->initializer && parent->resource_open==call->resource_open){
    parent->section_called=true;parent->section_returned=true;parent->section_name=call->section_name;
    parent->section_address=call->section_address;parent->section_size=call->section_size;
    parent->section_status=call->section_status;
   }
  }else if(at==0x817DB560 && int32_t(value)>=0 && call->section_returned){
   auto* init=state.ImageBySerial(call->initializer);
   const auto stream=Word(*state.kernel->memory(),uint32_t(call->args[1]));
   if(init && init->frame.function==0x816C8D68 && init->default_format &&
      init->row_output==call->row_output && stream && *stream){
    init->resource_open=call->serial;init->stream=*stream;init->open_status=value;
    init->section_returned=true;init->section_name=call->section_name;
    init->section_address=call->section_address;init->section_size=call->section_size;
    init->section_status=call->section_status;
   }
  }else if(at==0x816C8E10 && !value && call->default_index<state.image_defaults.size()){
   const auto work=state.Latest(frame->thread,0x816921C8);
   const auto provider=state.FindProvider(uint32_t(call->key_provider));
   const auto address=Word(*state.kernel->memory(),uint32_t(call->args[1]));
   const auto size=Word(*state.kernel->memory(),uint32_t(call->args[2]));
   const auto version=provider?Qword(*state.kernel->memory(),provider->object+0xA0):std::optional<uint64_t>{};
   const auto& row=state.image_defaults[call->default_index];
   if(row && work && work->source==call->default_source && work->provider_generation==call->default_birth &&
      provider && provider->generation==call->default_birth && !provider->revoked && !provider->destroyed &&
      provider->image_uri_generation==call->uri_generation && address && size && version && state.config.domain){
    const auto member=state.config.domain->root();
    const auto original=row->AtOriginalFallback({state.root,member.hmodule,member.generation,
        call->default_index,provider->generation,work->source,*version,*address,*size,value});
    if(original)work->image_default_result=State::ImageInput{{},original};
   }
  }
  // These are original native call/store facts. Completion/pixels require
  // the exact independently captured native record and real GPU Source join.
  for(auto& entry:state.image_calls)if(entry && &*entry==call){entry.reset();break;}
 }catch(...){if(slot)slot->unavailable_.store(true,std::memory_order_release);}
}

void NativeProfileIdentityOwner::ObserveImageInstruction(void* raw,uint32_t site) noexcept {
 State::HookUse hook_use;
 if(!raw || site>=kImageObservationSites.size())return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto point=kImageObservationSites[site];
 std::shared_ptr<NativeIdentityTileObserverSlot> slot,current_slot;
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 try{
  if(!c.kernel_state)return;
  slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  if(!slot || slot->kernel_!=c.kernel_state)return;
  owner=slot->current_.load(std::memory_order_acquire).lock();if(!owner)return;
  auto& state=*owner->state_;if(!state.profile_enabled)return;
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  std::shared_ptr<NativeImportDomain> domain;
  {
   std::lock_guard lock(state.mutex);
   if(state.retired.load() || !state.config.catalog)return;
   if(!hook_use.Acquire(state.hook_rundown,owner->state_,false))return;
   calls=state.config.catalog->calls();domain=state.config.domain;
  }
  {
   const auto actual=state.ImageShape(c,point.function);
   if(!actual){slot->unavailable_.store(true,std::memory_order_release);return;}
   std::lock_guard lock(state.mutex);
   if(site==14){
    // This formatter cut belongs to an already-issued provider whole use,
    // unlike the image-call cuts. No source or Task is issued by this filter.
    const auto work=state.Latest(actual->thread,0x816921C8);
    if(!work || work->uncertain || work->normal_return ||
       work->frame.pcr!=actual->pcr || work->frame.sp<0xB0 ||
       work->frame.sp-0xB0!=actual->sp)return;
   }else if(!state.ImageAt(*actual,reinterpret_cast<uintptr_t>(&c),true)){
    // Match the existing deny-only missing-work disposition before any hash;
    // an unknown body may not borrow an older call's consumption credit.
    state.LoseImageObservation();return;
   }
  }
  auto global=global_critical_region::AcquireDirect();
  if(!calls || !CheckImageBody(*calls,point.function)){
   slot->unavailable_.store(true,std::memory_order_release);return;
  }
  const auto frame=state.ImageShape(c,point.function);
  const auto original=Word(*state.kernel->memory(),point.address);
  if(!frame || !original || *original!=point.word){slot->unavailable_.store(true,std::memory_order_release);return;}
  std::optional<uint32_t> actual_task;
  if(site==14 && domain && CheckTaskReferenceCode(*calls))
   actual_task=NativeProfileIdentityTaskContext::Read(*calls,*domain,point.function,c);
  current_slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  std::lock_guard lock(state.mutex);
  const auto current=slot->current_.load(std::memory_order_acquire);const auto self=owner->weak_from_this();
  if(state.retired.load() || state.image_slot.get()!=slot.get() || current_slot.get()!=slot.get() ||
     current_slot.owner_before(slot) || slot.owner_before(current_slot) ||
     current.owner_before(self) || self.owner_before(current) || slot->unavailable_.load())return;
  auto& memory=*state.kernel->memory();
  if(site==14){
   // BEFORE the original event key store, bind the actual formatter output to
   // this exact admitted Query, current Provider birth and committed bytes.
   const auto work=state.Latest(frame->thread,0x816921C8);
   if(!work || work->uncertain || work->normal_return || !actual_task ||
      work->birth.task!=*actual_task || work->frame.pcr!=frame->pcr ||
      work->frame.sp<0xB0 || work->frame.sp-0xB0!=frame->sp ||
      !DwordIs(c.r[31],work->first) || !DwordIs(c.r[25],work->image_event) ||
      !DwordIs(c.r[24],work->image_modified) || !DwordIs(c.r[30],work->first+0x10))return;
   const auto provider=state.FindProvider(work->first);
   if(!provider || provider->generation!=work->provider_generation || provider->revoked || provider->destroyed)return;
   const auto ready=Word(memory,provider->object+0x90),size=Word(memory,provider->object+0x94);
   const auto version=Qword(memory,provider->object+0xA0);
   if(!ready || !*ready || !size || !*size || !version ||
      !NativeMemoryKeyMatches(memory,provider->object+0x10,*ready,*size,*version))return;
   std::optional<State::ImageInput> input;
   if(provider->last_output){
    const auto& output=provider->last_output;
    const auto terminal=Word(memory,provider->object+0xB8),extended=Word(memory,provider->object+0xD0);
    if(output->provider_generation==provider->generation && output->source==work->source &&
       output->published.load(std::memory_order_acquire) && !output->status.load(std::memory_order_relaxed) &&
       output->written.load(std::memory_order_relaxed) && terminal && !*terminal && extended && !*extended &&
       *ready==provider->buffer && *size==output->required.load(std::memory_order_relaxed) &&
       *version==output->version){
     const auto encoded=provider->image_epoch.Capture(provider->generation,output->source,*version,*size,
         output->image_first_allocation.load(std::memory_order_acquire),
         output->image_first_publication.load(std::memory_order_acquire));
     if(encoded)input=State::ImageInput{encoded,{}};
    }
   }
   if(!input && work->image_default_result && work->image_default_result->section &&
      work->image_default_result->source()==work->source &&
      work->image_default_result->section->provider==provider->generation &&
      work->image_default_result->section->version==*version &&
      work->image_default_result->address()==*ready && work->image_default_result->size()==*size)
    input=work->image_default_result;
   if(input && state.ImageBytesCurrent(*input)){
    work->image_key_candidate=input;work->image_uri_generation=provider->image_uri_generation;
   }
   return;
  }
  auto* call=state.ImageAt(*frame,reinterpret_cast<uintptr_t>(&c),true);
  if(!call){state.LoseImageObservation();return;} // missing nested work cannot borrow older credit
  if(site==0){
   auto* decoder=state.ImageBySerial(call->parent);
   const bool scalar=DwordIs(c.r[3],uint32_t(c.r[3])) && DwordIs(c.r[4],uint32_t(c.r[4])) &&
       DwordIs(c.r[5],uint32_t(c.r[5])) && DwordIs(c.r[31],call->read_state);
   const auto pointer=Word(memory,call->read_state),remaining=Word(memory,call->read_state+4);
   const bool actual=decoder && decoder->input && call->input && scalar && !call->copy_entered &&
       !decoder->error && !decoder->uncertain && decoder->copy_calls==decoder->copy_returns &&
       state.ImageBytesCurrent(*call->input) && pointer && remaining &&
       *pointer==uint64_t(call->input->address())+decoder->copied_bytes &&
       *remaining==call->input->size()-decoder->copied_bytes &&
       DwordIs(c.r[4],*pointer) && DwordIs(c.r[3],uint32_t(call->args[1])) &&
       DwordIs(c.r[5],uint32_t(call->args[2])) && uint32_t(c.r[5])<=*remaining &&
       !util::GuestIoRangesOverlap(memory,uint32_t(c.r[3]),uint32_t(c.r[5]),
           call->input->address(),call->input->size()) && decoder->copy_calls!=UINT32_MAX;
   if(actual){call->source_pointer=uint32_t(c.r[4]);call->copy_destination=uint32_t(c.r[3]);
    call->copy_length=uint32_t(c.r[5]);call->copy_entered=true;++decoder->copy_calls;}
   else {call->uncertain=true;if(decoder)decoder->uncertain=true;}
  }
  else if(site==1 || site==3 || site==5){
   if(call->native_call_entered){call->uncertain=true;return;}
   if(!call->dc || !call->copied || !DwordIs(c.r[3],uint32_t(c.r[3])) || !c.r[3])return;
   const auto low=Word(memory,call->dc->address+0x1CC);
   const auto device=low?Word(memory,*low+0xC):std::optional<uint32_t>{};
   if(!device || !DwordIs(c.r[3],*device))return;
   if(site==1 && (!DwordIs(c.r[4],0) || !DwordIs(c.r[5],call->native_texture)))return;
   if(site!=1 && (!call->draw_selection || state.images.SelectionSerial(*call->dc)!=call->draw_selection ||
      state.images.Copied(*call->dc)!=call->copied)){call->uncertain=true;return;}
   call->device=*device;call->sampler=site==1?uint32_t(c.r[4]):0;call->native_call_entered=true;
  }else if(site==2 || site==4 || site==6){
   // A forced wrapper zero is not interpreted. The actual inner own-return
   // already retained the factual native store/upload branch above.
   if(!call->native_call_entered || !call->native_call_returned)call->uncertain=true;
  }else if(site==11){
   if(!call->dc || !call->selection || !call->copied || call->native_store_entered ||
      !DwordIs(c.r[31],call->device) || !DwordIs(c.r[5],call->native_texture) ||
      !DwordIs(c.r[29],(call->sampler+0xC9Eu)*4u))return;
   call->slot_offset=uint32_t(c.r[29]);call->native_store_entered=true;
  }else if(site==12 || site==13){
   if(!call->dc || !call->copied || call->draw_store_entered || !call->draw_selection ||
      state.images.SelectionSerial(*call->dc)!=call->draw_selection || state.images.Copied(*call->dc)!=call->copied ||
      !DwordIs(c.r[31],call->device) || !DwordIs(c.r[11],uint32_t(c.r[11])) || !c.r[11])return;
   const auto texture=Word(memory,call->device+0x3278);
   if(!texture || *texture!=call->native_texture)return;
   if(!call->packet_returned)return;
   for(uint32_t n=0;n<call->texture_fetch.size();++n){
    const auto word=Word(memory,call->device+0x480+n*4);if(!word)return;call->texture_fetch[n]=*word;
   }
   call->tail=uint32_t(c.r[11]);call->draw_store_entered=true;
  }
  else if(site==7){
   call->default_format=false;call->section_returned=false;call->resource_open=0;call->stream=0;
   if(!DwordIs(c.r[31],uint32_t(c.r[31])) || uint32_t(c.r[31])<0x81A8271C ||
      (uint32_t(c.r[31])-0x81A8271C)%16 || !domain)return;
   const auto index=(uint32_t(c.r[31])-0x81A8271C)/16;
   if(index>=state.image_defaults.size())return;
   state.image_defaults[index].reset();
   const auto member=domain->root();
   if(!DwordIs(c.r[3],member.hmodule) || member.generation!=state.root.generation ||
      calls->module()->hmodule_ptr()!=member.hmodule || calls->module()->load_generation()!=member.generation ||
      !DwordIs(c.r[4],uint32_t(c.r[4])) || !DwordIs(c.r[5],uint32_t(c.r[5])) ||
      !DwordIs(call->args[0],0x81A82714) || !DwordIs(call->args[1],3))return;
   const auto section=Word(memory,uint32_t(c.r[31])-8),entry=Word(memory,uint32_t(c.r[31])-4);
   if(!section || !entry || !DwordIs(c.r[4],*section) || !DwordIs(c.r[5],*entry) ||
      !ReadImageLabel(memory,*section,call->section_label) || !ReadImageLabel(memory,*entry,call->entry_label))return;
   call->module=member.hmodule;call->module_generation=member.generation;
   call->initializer=call->serial;call->default_index=index;call->row_output=uint32_t(c.r[31]);call->default_format=true;
  }else if(site==8){
   if(!call->initializer || !call->resource_open || call->section_called || !domain)return;
   const auto member=domain->root();
   if(!DwordIs(c.r[3],member.hmodule) || member.generation!=call->module_generation ||
      !DwordIs(c.r[5],call->body_sp+0x5C) || !DwordIs(c.r[6],call->body_sp+0x58) ||
      !DwordIs(c.r[4],uint32_t(c.r[4])) || !ReadImageLabel(memory,uint32_t(c.r[4]),call->section_name))return;
   call->address_output=uint32_t(c.r[5]);call->size_output=uint32_t(c.r[6]);call->section_called=true;
  }else if(site==9){
   if(!call->section_called || call->section_returned || !DwordIs(c.r[3],uint32_t(c.r[3])))return;
   const auto address=Word(memory,call->address_output),size=Word(memory,call->size_output);
   uint32_t root_address=0,root_size=0;
   if(int32_t(uint32_t(c.r[3]))<0 || !address || !size ||
      calls->module()->GetSection(ImageLabel(call->section_name),&root_address,&root_size)!=X_STATUS_SUCCESS ||
      root_address!=*address || root_size!=*size)return;
   call->section_address=*address;call->section_size=*size;call->section_status=uint32_t(c.r[3]);call->section_returned=true;
  }else if(site==10){
   if(!call->default_format || !call->section_returned || !call->resource_open || !call->stream ||
      !DwordIs(c.r[31],call->row_output) || !DwordIs(c.r[3],uint32_t(c.r[3])))return;
   const auto stream=Word(memory,call->body_sp+0x50),address=Word(memory,call->row_output),size=Word(memory,call->row_output+4);
   if(!stream || *stream!=call->stream || !address || !size)return;
   const identity_detail::NativeDefaultRootSection current{state.root,call->module,call->module_generation,
       ImageLabel(call->section_name),call->section_address,call->section_size};
   const identity_detail::NativeDefaultRowObservation row{state.root,call->serial,call->default_index,
       call->row_output,call->module,call->module_generation,ImageLabel(call->section_label),ImageLabel(call->entry_label)};
   const identity_detail::NativeDefaultResourceOpenObservation opened{state.root,call->serial,call->resource_open,
       call->row_output,call->module,call->stream,call->open_status,ImageLabel(call->section_label),ImageLabel(call->entry_label)};
   const identity_detail::NativeDefaultGetSectionObservation section{state.root,call->module,call->module_generation,
       ImageLabel(call->section_name),call->section_address,call->section_size,call->section_status};
   const identity_detail::NativeDefaultGetBufferObservation buffer{state.root,call->serial,call->resource_open,
       call->row_output,call->stream,*address,*size,uint32_t(c.r[3])};
   state.image_defaults[call->default_index]=identity_detail::NativeDefaultImageRow::Observe(current,row,opened,section,buffer);
  }
  else if(site==15 || site==16){
   if(!call->dc || !call->copied || call->packet_address || !DwordIs(c.r[31],call->device) ||
      !DwordIs(c.r[30],uint32_t(c.r[30])) || uint64_t(uint32_t(c.r[30]))+24>0x100000000ull ||
      !DwordIs(c.r[11],site==15?0xC0012200u:0xC0032200u))return;
   if(site==15){
    if(!DwordIs(c.r[29],uint32_t(c.r[29])) || !DwordIs(c.r[10],uint32_t(c.r[10])) ||
       !DwordIs(c.r[9],uint32_t(c.r[9])))return;
    call->packet_viz=uint32_t(c.r[29]);call->packet_initiator=uint32_t(c.r[10])|uint32_t(c.r[9])|0x80u;
   }else{
    if(!DwordIs(c.r[24],uint32_t(c.r[24])) || !DwordIs(c.r[29],uint32_t(c.r[29])) ||
       !DwordIs(c.r[25],uint32_t(c.r[25])) || !DwordIs(c.r[27],uint32_t(c.r[27])))return;
    call->packet_viz=uint32_t(c.r[24]);call->packet_initiator=uint32_t(c.r[29]);
    call->packet_base=uint32_t(c.r[25]);call->packet_size=uint32_t(c.r[27]);
   }
   call->packet_address=uint32_t(c.r[30])+4;call->packet_header=uint32_t(c.r[11]);
  }
  // All original instructions execute independently of observation results.
 }catch(...){if(slot)slot->unavailable_.store(true,std::memory_order_release);}
}

void NativeProfileIdentityOwner::ObserveImageAllocationFree(void* raw) noexcept {
 State::HookUse hook_use;
 if(!raw)return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 std::shared_ptr<NativeIdentityTileObserverSlot> slot,current_slot;
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 try{
  if(!c.kernel_state)return;
  slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  if(!slot || slot->kernel_!=c.kernel_state)return;
  owner=slot->current_.load(std::memory_order_acquire).lock();if(!owner)return;
  auto& state=*owner->state_;
  if(!state.profile_enabled || !DwordIs(c.r[3],uint32_t(c.r[3])))return;
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  {
   std::lock_guard lock(state.mutex);
   if(state.retired.load() || !state.config.catalog)return;
   if(!hook_use.Acquire(state.hook_rundown,owner->state_,false))return;
   calls=state.config.catalog->calls();
  }
  auto global=global_critical_region::AcquireDirect();
  if(!calls || !CheckImageBody(*calls,0x816E6AB0)){
   slot->unavailable_.store(true,std::memory_order_release);return;
  }
  const auto frame=state.ImageShape(c,0x816E6AB0);if(!frame)return;
  current_slot=c.kernel_state->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  std::lock_guard lock(state.mutex);
  const auto actual=slot->current_.load(std::memory_order_acquire);
  const auto self=owner->weak_from_this();
  if(state.retired.load() || current_slot.get()!=slot.get() ||
     current_slot.owner_before(slot) || slot.owner_before(current_slot) ||
     actual.owner_before(self) || self.owner_before(actual))return;
  const auto pointer=uint32_t(c.r[3]);if(!pointer)return;
  // Entry revokes future observation before ORIGINAL free. It is neither a
  // successful deallocation receipt nor a native reference/cancellation call.
  state.RevokeImageString(pointer);state.images.RevokeNativeTexture(pointer);
  for(const auto& work:state.active)for(auto& draw:work->image_draws)
   if(draw && draw->draw.copied.texture==pointer)draw.reset();
  for(const auto& provider:state.providers)if(provider &&
      (provider->buffer==pointer || provider->object==pointer)){
   provider->image_epoch.Revoke();provider->image_output.reset();provider->image_key.reset();
  }
  for(const auto kind:{identity_detail::NativeImageJournal::Kind::kInterface,
      identity_detail::NativeImageJournal::Kind::kBrush,
      identity_detail::NativeImageJournal::Kind::kWrapper,
      identity_detail::NativeImageJournal::Kind::kDc})
   if(const auto object=state.images.Current(kind,pointer))state.images.RetireObject(*object);
 }catch(...){if(slot)slot->unavailable_.store(true,std::memory_order_release);}
}

std::shared_ptr<NativeProfileIdentityOwner> NativeProfileIdentityOwner::Lookup(KernelState* kernel) {
 return NativeSystemUiService::CaptureProfileIdentityOwner(kernel);
}
NativeProfileIdentityOwner::SharedScheduleObservation
NativeProfileIdentityOwner::ObserveSharedContinuationSchedule(
    NativeSystemTaskContinuation& continuation,XThread& bootstrap,
    std::span<const uint32_t> args) noexcept {
 try{
  const auto owner=NativeSystemUiService::CaptureTaskExecutionOwner(bootstrap.kernel_state());
  if(!owner)return {true,{}}; // No identity observer means no identity claim.
  auto& s=*owner->state_;
  std::shared_ptr<NativeSystemTaskContinuation> bound;
  std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
  std::shared_ptr<NativeImportDomain> domain;
  {
   std::lock_guard lock(s.mutex);
   if(s.retired.load() || s.uncertain.load() ||
       std::any_of(s.active.begin(),s.active.end(),[&](const auto& work){
        return work && !work->service_progress && work->frame.thread==bootstrap.thread_id() && !work->transport_unclaimed &&
            (work->read || work->commit || (s.job && s.job->native_entered && work->source==s.job->source));
       }))return {};
   bound=s.continuation.lock();catalog=s.config.catalog;domain=s.config.domain;
  }
  if(bound.get()!=&continuation || ActualThread()!=&bootstrap ||
      !continuation.MatchesSharedSchedule(bootstrap,args,catalog,domain))return {};
  auto expected=std::make_shared<State::Expected>();
  expected->source=kBootUnjoinedSource;expected->transport_only=true;
  expected->thread=retain_object(&bootstrap);expected->function=0x816BA440;expected->count=4;
  std::copy(args.begin(),args.end(),expected->args.begin());
  // Allocate the return observer before publishing the expected ingress. The
  // exact host Bootstrap must really return from Schedule; its OS join remains
  // mandatory. This observer never changes native status or closes a Task.
  std::function<void(const cpu::GuestCallResult&)> returned=[owner,expected](const auto& actual){
   auto& s=*owner->state_;
   if(global_critical_region::is_held_by_current_thread() || ActualThread()!=expected->thread.get()){
    s.Lose(__LINE__);return; // Keep the unreturned Expected; no callback/lock under GCRC.
   }
   std::lock_guard lock(s.mutex);
   const auto found=std::find(s.expected.begin(),s.expected.end(),expected);
   if(found==s.expected.end() || expected->call_returned || !expected->transport_only){s.Lose(__LINE__);return;}
   expected->call_returned=true;
   expected->healthy=actual.executed ?
       expected->entered && expected->returned && bool(expected->actual):!expected->entered;
   if(!expected->healthy)s.Lose(__LINE__);
   s.retired_expected.push_back(expected);s.expected.erase(found);
  };
  {
   std::lock_guard lock(s.mutex);
   if(s.retired.load() || s.uncertain.load() || s.continuation.lock().get()!=&continuation ||
       s.config.catalog!=catalog || s.config.domain!=domain || s.expected.size()>=64 ||
       !s.next_work || s.next_work==UINT64_MAX)return {};
   expected->serial=s.next_work++;s.expected.push_back(expected);
  }
  return {true,std::move(returned)};
 }catch(...){return {};}
}

bool NativeProfileIdentityOwner::BindContinuation(std::shared_ptr<NativeSystemTaskContinuation> continuation) {
 std::lock_guard lock(state_->mutex);
 if(!continuation || !state_->continuation.expired() || state_->retired.load())return false;
 state_->continuation=continuation;return true;
}
bool NativeProfileIdentityOwner::OwnsScheduledWork(uint64_t serial) const noexcept {
 try{std::lock_guard lock(state_->mutex);
  return serial && state_->job && state_->job->serial==serial && !state_->job->execution_uncertain &&
      !state_->retired.load(std::memory_order_acquire) && !state_->uncertain.load(std::memory_order_acquire);
 }catch(...){return false;}
}

bool NativeProfileIdentityOwner::EnqueuePrepared(std::shared_ptr<xam::ProfileRosterIntent> intent,
    const NativeProfileIdentityRequest* original,XThread* caller,uint32_t input,
    uint32_t flags,uint32_t overlap,bool bootstrap_alignment,std::error_code& error,
    bool* retryable_bootstrap,const xam::NativeOriginalLogonObservation* observation,
    std::chrono::steady_clock::time_point preparation_deadline) {
 error.clear();auto& s=*state_;
 if(retryable_bootstrap)*retryable_bootstrap=false;
 const auto fail_error=[&](uint32_t line,uint32_t stage,const std::error_code& reason,bool retryable=false,
                     uint32_t evaluated=0,uint32_t failed=0){
  error=reason;
  if(retryable_bootstrap)*retryable_bootstrap=retryable;
  if(bootstrap_alignment)s.RecordBootstrapFailure(stage,line,error,retryable,evaluated,failed);
  if(observation){
   OriginalLogonDiagnostic d;d.stage=OriginalLogonDiagnostic::kPrepareFailure;
   d.reason=stage;d.line=line;d.error_available=1;d.error_value=uint32_t(error.value());
   d.error_category=error.category()==std::generic_category()?1:error.category()==std::system_category()?2:0;
   d.evaluated=evaluated;d.failed=failed;
   xam::ProfileManager::RecordNativeOriginalLogon(*observation,d);
  }
  return false;
 };
 const auto fail=[&](uint32_t line,uint32_t stage,std::errc reason,bool retryable=false,
                     uint32_t evaluated=0,uint32_t failed=0){
  return fail_error(line,stage,std::make_error_code(reason),retryable,evaluated,failed);
 };
 if(!intent || global_critical_region::is_held_by_current_thread() ||
    intent->result().phase!=xam::ProfileRosterPhase::kPrepared ||
    !s.prepared_title.module || !s.prepared_title.lease ||
    s.prepared_title.module->load_generation()!=s.prepared_title_generation ||
    s.kernel->GetExecutableModule().get()!=s.prepared_title.module.get()){
  return fail(__LINE__,3,std::errc::operation_canceled);
 }
 const auto& rows=intent->NativeRowsForPrepared();
 // The supported local producer must not silently turn a real Live Account
 // into an offline replacement. Preserve its opaque bytes and refuse before
 // entry until the separate actual online-service contract is implemented.
 for(size_t i=0;i<4;++i)if(intent->prepared_roster()->slots()[i].xuid &&
     (xe::load_and_swap<uint32_t>(rows.data()+i*0x188+8)&0x20000000u)){
  return fail(__LINE__,4,std::errc::not_supported);
 }
 if(original && !original->MatchesPreparedRows(rows)){
  return fail(__LINE__,5,std::errc::operation_canceled);
 }
 auto job=std::make_shared<State::Job>();job->intent=intent;job->flags=flags;job->overlap=overlap;
 job->preparation_deadline=preparation_deadline==std::chrono::steady_clock::time_point{}?
     std::chrono::steady_clock::now()+std::chrono::seconds(5):preparation_deadline;
 if(observation)job->original_logon_observation=*observation;
 job->bootstrap_alignment=bootstrap_alignment;
 if(original)job->original=*original;
 job->domain=s.config.domain->Acquire();
 if(!job->domain){return fail(__LINE__,6,std::errc::operation_canceled);}
 job->old_ticket=intent->NativeReadTicket(false,*s.config.factory_binding,&error);
 if(!job->old_ticket)return fail_error(__LINE__,7,error,State::RetryableBootstrapError(error));
 job->writer_ticket=intent->NativeReadTicket(true,*s.config.factory_binding,&error);
 if(!job->writer_ticket)return fail_error(__LINE__,7,error,State::RetryableBootstrapError(error));
 for(size_t i=0;i<job->old_reads.size();++i){
  job->old_reads[i]=xam::ProfileRosterNativeReadScope::Prepare(job->old_ticket,s.config.factory_binding);
  job->writer_reads[i]=xam::ProfileRosterNativeReadScope::Prepare(job->writer_ticket,s.config.factory_binding);
  if(!job->old_reads[i] || !job->writer_reads[i]){return fail(__LINE__,8,std::errc::not_enough_memory);}
 }
 auto* memory=s.kernel->memory();
 struct Scratch {
  Memory* memory;uint32_t address=0;
  ~Scratch(){if(address)memory->SystemHeapFree(address);}
 } scratch{memory};
 if(caller){
  if(!original || ActualThread()!=caller || caller->kernel_state()!=s.kernel ||
     !caller->thread_state() || !input || (input&7)){
   return fail(__LINE__,3,std::errc::invalid_argument);
  }
  job->original_caller=retain_object(caller);job->input=input;
  const auto& c=*caller->thread_state()->context();
  if(c.lr<4 || c.lr>UINT32_MAX || (c.lr&3)){
   return fail(__LINE__,3,std::errc::operation_canceled);
  }
  const auto retain_code=[&](uint32_t at)->std::optional<State::CodeOwner>{
   auto module=s.kernel->GetUserModuleForAddress(at);xex2_section_type section;
   if(!module || !module->xex_module() || !module->load_generation() ||
      !module->xex_module()->GetPageSectionType(at,&section) || section!=XEX_SECTION_CODE ||
      memory->QueryRangeAccess(at,4)==memory::PageAccess::kNoAccess)return {};
   auto lease=s.kernel->AcquireUserModuleExecutionLease(module);
   if(!lease)return {};return State::CodeOwner{std::move(module),std::move(lease)};
  };
  const auto code=retain_code(uint32_t(c.lr)-4);
  if(!code){return fail(__LINE__,3,std::errc::operation_canceled);}
  job->caller_code.push_back(*code);
  const std::array<NativeServiceBackingRange,1> ranges{{{input,uint32_t(original->original().size()),false}}};
  job->caller_backing=memory->BorrowNativeServiceBacking(ranges);
  if(!job->caller_backing){return fail(__LINE__,3,std::errc::bad_address);}
  if(overlap){
   if(overlap&3){return fail(__LINE__,3,std::errc::invalid_argument);}
   const std::array<NativeServiceBackingRange,1> range{{{overlap,sizeof(XAM_OVERLAPPED),true}}};
   job->overlap_backing=memory->BorrowNativeServiceBacking(range);
   if(!job->overlap_backing){return fail(__LINE__,3,std::errc::bad_address);}
   const auto routine=Word(*memory,overlap+0x10);
   if(!routine){return fail(__LINE__,3,std::errc::bad_address);}
   if(*routine){
    job->apc_code=retain_code(*routine);
    if(!job->apc_code){return fail(__LINE__,3,std::errc::operation_canceled);}
   }
  }
 }else{
  // Host facade/startup alignment supplies the exact common canonical224
  // rows to the actual native SYSTEM continuation, never writes native cache.
  if(original || input || overlap || flags){return fail(__LINE__,3,std::errc::invalid_argument);}
  scratch.address=memory->SystemHeapAlloc(uint32_t(rows.size()));
  if(!scratch.address){return fail(__LINE__,9,std::errc::not_enough_memory);}
  if(util::WriteGuestIoBuffer(*memory,scratch.address,rows)!=0){
   return fail(__LINE__,10,std::errc::bad_address);
  }
  job->input=scratch.address;job->owned_input=true;
 }
 const auto caller_task=s.OriginalCallerTask(caller,original?original->ordinal():0);
 {
  std::lock_guard lock(s.mutex);
  uint32_t evaluated=0,failed=0;
  const auto rejected=[&](uint32_t bit,bool value){evaluated|=1u<<bit;if(value)failed|=1u<<bit;return value;};
  if(rejected(0,bool(s.job)) || rejected(1,s.stopping.load()) || rejected(2,s.retired.load()) ||
     rejected(3,s.uncertain.load()) || rejected(4,!bootstrap_alignment && !s.aligned) ||
     rejected(5,intent->old_roster()!=s.current_roster) ||
     rejected(6,!s.tasks.SourceDescendantsClosed(s.current_read_source)) ||
     rejected(7,!s.next_source || s.next_source==UINT64_MAX) || rejected(10,bool(s.bootstrap_cleanup))){
   return fail(__LINE__,11,std::errc::device_or_resource_busy,failed==(1u<<6) || failed==(1u<<10),evaluated,failed);
  }
  if(caller && s.Latest(caller->thread_id()) &&
     s.NormalOriginalCaller(caller,caller_task)!=State::ReentrantWorkConflict::kNone &&
     !s.ReentrantOriginalFrame(caller,original?original->ordinal():0,flags,overlap))
   return fail(__LINE__,11,std::errc::device_or_resource_busy,false,1u<<9,1u<<9);
  job->serial=job->source=s.next_source++;
 }
 auto lifetime=std::make_shared<State::EventLifetime>();
 lifetime->code=s.root_code;lifetime->domain=s.config.domain->Acquire();
 lifetime->thread=job->original_caller;lifetime->intent=intent;
 if(!lifetime->domain){return fail(__LINE__,12,std::errc::operation_canceled);}
 const Events::Root root{s.root.kernel,s.root.domain,s.root.generation};
 job->event_source=std::shared_ptr<const NativeIdentityEventSource>(new NativeIdentityEventSource(
     root,job->source,s.config.factory_binding->nonce(),lifetime));
 job->event_origin=s.listeners->AdoptSource(job->event_source);
 if(!job->event_origin){return fail(__LINE__,13,std::errc::operation_canceled);}
 // A failed late reservation never exposed this source through s.job. Its
 // exact producer must be retired before another attempt can consume a slot.
 // Declared before the next lock so cleanup always runs after its unlock.
 struct UnpublishedSource {
  State& state;std::shared_ptr<State::Job>& job;std::error_code& error;bool* retryable;
  ~UnpublishedSource(){
   if(job && job->event_origin && !state.listeners->FinishSourceAfterDescendants(*job->event_origin)){
    error=std::make_error_code(std::errc::operation_canceled);if(retryable)*retryable=false;
    state.RecordBootstrapFailure(16,__LINE__,error,false);state.Lose(__LINE__);
   }
  }
 } unpublished{s,job,error,retryable_bootstrap};
 const auto reserved_caller_task=s.OriginalCallerTask(caller,original?original->ordinal():0);
 {
  std::lock_guard lock(s.mutex);
  uint32_t evaluated=0,failed=0;
  const auto rejected=[&](uint32_t bit,bool value){evaluated|=1u<<bit;if(value)failed|=1u<<bit;return value;};
  if(rejected(0,bool(s.job)) || rejected(1,s.stopping.load()) || rejected(2,s.retired.load()) ||
     rejected(3,s.uncertain.load()) || rejected(6,!s.tasks.SourceDescendantsClosed(s.current_read_source)) ||
     rejected(10,bool(s.bootstrap_cleanup))){
   return fail(__LINE__,14,std::errc::device_or_resource_busy,failed==(1u<<6) || failed==(1u<<10),evaluated,failed);
  }
  const bool normal_caller=caller &&
      s.NormalOriginalCaller(caller,reserved_caller_task)==State::ReentrantWorkConflict::kNone;
  if(!normal_caller)job->original_render_frame=s.ReentrantOriginalFrame(caller,original?original->ordinal():0,flags,overlap);
  // Readless issuers use the existing full drain BEFORE initializer entry.
  // They do not manufacture a render read or enable the early worker gate.
  job->initializer_ready=bool(job->original_render_frame);
  if(caller && s.Latest(caller->thread_id()) && !normal_caller && !job->original_render_frame)
   return fail(__LINE__,14,std::errc::device_or_resource_busy,false,1u<<9,1u<<9);
  job->old_pixels=s.pixels;
  job->old_event_source=s.current_event_source;job->old_event_origin=s.current_event_origin;
  job->hidden_pixels=std::make_shared<IdentityPixelGuard>(job->source,false,s.pixel_lifetime);
  job->new_pixels=std::make_shared<IdentityPixelGuard>(job->source,true,s.pixel_lifetime);
  // Reserve the transition while holding the SAME mutex as Query admission.
  // Any Query already admitted keeps its old source/read scope until CD920.
  if(s.render_phase!=State::RenderPhase::kOpen){
   return fail(__LINE__,15,std::errc::device_or_resource_busy,false,1u<<8,1u<<8);
  }
  if(job->old_pixels)job->old_pixels->Revoke();
  s.render_phase=State::RenderPhase::kDrainingOld;s.render_query_parked=false;
  s.old_ingress_closed=true;
  s.job=std::move(job);scratch.address=0;
 }
 s.changed.notify_all();return true;
}

bool NativeProfileIdentityOwner::CancelQueued(const std::shared_ptr<xam::ProfileRosterIntent>& intent) {
 auto& s=*state_;std::shared_ptr<State::Job> job;
 std::shared_ptr<const NativeServiceBackingLease> input,overlap;
 uint32_t owned_input=0;
 {
  std::lock_guard lock(s.mutex);job=s.job;
  if(!job || job->intent!=intent || job->entering || job->native_entered || job->transport_running)return false;
  job->canceled=true;job->external_closed=true;
  input=std::move(job->caller_backing);overlap=std::move(job->overlap_backing);
  if(job->owned_input){owned_input=job->input;job->owned_input=false;job->input=0;}
 }
 // No native ingress can consume these caller buffers after cancellation.
 // Genuine mount/old-render cleanup may remain pending without retaining a
 // client backing denial. The bootstrap completes that separate rundown.
 input.reset();overlap.reset();
 if(owned_input)s.kernel->memory()->SystemHeapFree(owned_input);
 const auto canceled=s.kernel->xam_state()->profile_manager()->CancelRoster(intent);
 s.changed.notify_all();return canceled.phase==xam::ProfileRosterPhase::kCanceled;
}

GuestRuntime::ExecutionResult NativeProfileIdentityOwner::ExecutePreparedOn(XThread& thread) {
 auto& s=*state_;
 // This actual C++ frame contains the generic executor's keep_alive and must
 // genuinely unwind before a retained-stop capsule may admit CPU shutdown.
 // Declared first so every later local holder is destroyed before decrement.
 struct ExecutorFrame {
  State& s;bool entered=false;
  ~ExecutorFrame(){if(entered)s.identity_executor_frames.fetch_sub(1,std::memory_order_acq_rel);}
 } host_frame{s};
 {
  std::lock_guard lock(s.mutex);
  if(s.stopping.load() || s.retired.load() || s.identity_executor_frames.load())
   return {false,kAbort,"Owned identity executor admission sealed"};
  s.identity_executor_frames.fetch_add(1,std::memory_order_acq_rel);host_frame.entered=true;
 }
 std::shared_ptr<State::Job> job;
 std::shared_ptr<State::Expected> expected;
 std::shared_ptr<xam::ProfileRosterPublicationCell> publication;
 for(;;){
  {std::lock_guard lock(s.mutex);job=s.job;}
  if(job && std::chrono::steady_clock::now()>=job->preparation_deadline){
   CancelQueued(job->intent);return {false,kAbort,"Common native preparation deadline expired"};
  }
  expected=std::make_shared<State::Expected>();
  {
   std::lock_guard lock(s.mutex);job=s.job;
   if(!job || !job->initializer_ready || job->canceled || job->entering || job->native_entered ||
      job->execution_uncertain || s.stopping.load() || s.retired.load() || s.uncertain.load() ||
      ActualThread()!=&thread || thread.kernel_state()!=s.kernel || s.expected.size()>=64 ||
      (job->original_caller && job->original_caller.get()!=&thread))
    return {false,kAbort,"Prepared original native ingress is not admitted"};
   expected->serial=job->serial;expected->source=job->source;
   expected->thread=retain_object(&thread);expected->count=3;
   expected->function=job->original && job->original->ordinal()==0x20B?0x8199C630:0x8199E3E0;
   expected->args[0]=job->input;expected->args[1]=job->flags;expected->args[2]=job->overlap;
   // Every reservation precedes its actual PM arm. No original PPC or newly
   // introduced Delay executes while a never-entered Expected is registered.
   s.expected.push_back(expected);job->entering=true;
  }
  std::error_code error;
  publication=s.kernel->xam_state()->profile_manager()->EnterNativeRoster(
      job->intent,s.config.factory_binding,error);
  if(publication)break;
  bool retryable=false;
  {
   std::lock_guard lock(s.mutex);job->entering=false;
   const auto i=std::find(s.expected.begin(),s.expected.end(),expected);
   if(i==s.expected.end() || expected->entered){s.Lose(__LINE__);return {false,kAbort,"Pre-entry observer ownership changed"};}
   s.retired_expected.push_back(*i);s.expected.erase(i);
   retryable=s.job==job && !job->canceled && !s.stopping.load() && !s.retired.load() && !s.uncertain.load() &&
       State::RetryableBootstrapError(error) && std::chrono::steady_clock::now()<job->preparation_deadline;
  }
  // EnterNativeRoster's Busy also covers an inherited caller namespace
  // lock. That is a self dependency: never Delay while retaining GCRC.
  if(global_critical_region::is_held_by_current_thread())retryable=false;
  if(!retryable){CancelQueued(job->intent);return {false,kAbort,"Common native entry validation refused"};}
  s.changed.notify_all();
  thread.Delay(0,0,uint64_t(-160000ll)); // no Expected, PM/model/cache/namespace guard
 }
 {
  std::lock_guard lock(s.mutex);
  if(s.job!=job || !job->entering || job->canceled){s.Lose(__LINE__);job->execution_uncertain=true;
   return {false,kAbort,"Common/native entry owner changed"};}
  job->publication=std::move(publication);job->entering=false;job->native_entered=true;
 }
 const std::array<uint32_t,3> args{job->input,job->flags,job->overlap};
 const auto result=s.config.catalog->calls()->ExecuteOwnedIdentityIngressOnOriginalCaller(
     thread,job->original?job->original->ordinal():0x224,args);
 std::shared_ptr<const NativeServiceBackingLease> retired_input;
 uint32_t owned_input=0;
 {
  std::lock_guard lock(s.mutex);
  if(!result.succeeded || s.job!=job || !expected->entered || !expected->returned ||
     !expected->actual || expected->call_returned){
   s.Lose(__LINE__);job->execution_uncertain=true;
   return {false,result.return_value,"Original native ingress own-return was not authenticated"};
  }
  expected->call_returned=true;expected->healthy=true;
  job->native_returned=true;job->value=uint32_t(result.return_value);
  {OriginalLogonDiagnostic d;d.stage=OriginalLogonDiagnostic::kNativeReturn;
   d.entered=1;d.returned=1;d.result_available=1;d.result=uint32_t(result.return_value);
   xam::ProfileManager::RecordNativeOriginalLogon(job->original_logon_observation,d);}
  const auto i=std::find(s.expected.begin(),s.expected.end(),expected);
  if(i==s.expected.end()){s.Lose(__LINE__);job->execution_uncertain=true;
   return {false,result.return_value,"Original native ingress expected owner disappeared"};}
  s.retired_expected.push_back(*i);s.expected.erase(i);
  // A genuine early return is an actual DWORD, but is not proof that the PM,
  // native cache, render/source or caller completion reached a terminal join.
  // Preserve that distinction instead of waiting forever for an absent worker.
  if((!job->initializer_scheduled || int32_t(uint32_t(result.return_value))<0) &&
     s.published_source.load(std::memory_order_acquire)!=job->source){
   // Actual error/early-return ABI is preserved. Its caller completion is not
   // a PM rollback or a published outcome; retain that unresolved lifecycle.
   job->execution_uncertain=true;s.Lose(__LINE__);
  }
  // Both original public functions have now copied/cloned their entire input.
  // An async callback owns the native request, not this original input range.
  retired_input=std::move(job->caller_backing);
  if(job->owned_input){owned_input=job->input;job->owned_input=false;}
 }
 retired_input.reset();
 if(owned_input)s.kernel->memory()->SystemHeapFree(owned_input);
 s.changed.notify_all();return result;
}

NativeProfileIdentityOwner::Result NativeProfileIdentityOwner::TryLogonOnOriginalCaller(
    KernelState* kernel,uint16_t ordinal,std::span<const uint64_t> args,
    const xam::NativeOriginalLogonObservation* observation) noexcept {
 OriginalLogonCut diagnostic(observation);
 const Result rejected{Disposition::kRejectedBeforeEntry,kAbort};
 try{
  if(diagnostic.Refused(1,!kernel,__LINE__))return diagnostic.Finish({});
  const auto owner=Lookup(kernel);
  if(!owner)return diagnostic.Finish(diagnostic.Refused(2,kernel->native_profile_identity_claimed_.load(std::memory_order_acquire),__LINE__)?rejected:Result{});
  auto& s=*owner->state_;auto* caller=ActualThread();
  if(diagnostic.Refused(3,(ordinal!=0x20B && ordinal!=0x224),__LINE__) || diagnostic.Refused(4,args.size()!=3,__LINE__) ||
     diagnostic.Refused(5,std::any_of(args.begin(),args.end(),[](uint64_t value){return value>UINT32_MAX;}),__LINE__) ||
     diagnostic.Refused(6,!caller,__LINE__) || diagnostic.Refused(7,caller->kernel_state()!=kernel,__LINE__) || diagnostic.Refused(8,!caller->thread_state(),__LINE__) ||
     diagnostic.Refused(9,(caller->native_frame_thread_identity().process_type!=X_PROCTYPE_TITLE &&
      caller->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM),__LINE__) ||
     diagnostic.Refused(10,global_critical_region::is_held_by_current_thread(),__LINE__))return diagnostic.Finish(rejected);
  const auto caller_task=s.OriginalCallerTask(caller,ordinal);
  {
   std::lock_guard lock(s.mutex);
   const auto latest=[&]{
    const auto work=s.Latest(caller->thread_id());
    diagnostic.value.latest_available=1;diagnostic.value.latest_present=bool(work);
    if(work){
     diagnostic.value.latest_kind=work->execution_only?1:work->transport_only?2:work->creation_only?3:4;
     diagnostic.value.latest_kind|=OriginalLogonFunctionRole(work->frame.function)<<4;
     // Provenance categories only: retained object equality, then two scalar
     // SPs inside that same caller's host-recorded guest stack bounds. Below
     // does not prove live ancestry; above does not authorize retirement.
     const bool same_caller=work->thread.get()==caller;
     diagnostic.value.latest_kind|=uint32_t(work->thread?(same_caller?1:2):0)<<12;
     if(same_caller){
      const auto* context=caller->thread_state()->context();
      const uint64_t current_sp=context?context->r[1]:0;
      const uint32_t entry_sp=work->frame.sp,limit=caller->stack_limit(),base=caller->stack_base();
      if(context && limit<base && DwordIs(current_sp,uint32_t(current_sp)) &&
         !(uint32_t(current_sp)&15) && !(entry_sp&15) &&
         uint32_t(current_sp)>=limit && uint32_t(current_sp)<=base &&
         entry_sp>=limit && entry_sp<=base)
       diagnostic.value.latest_kind|=(uint32_t(current_sp)<=entry_sp?1u:2u)<<14;
     }
     diagnostic.value.latest_read_present=bool(work->read);
     diagnostic.value.latest_borrowed_read_present=bool(work->execution_read_owner);
     diagnostic.value.latest_same_source=work->source==s.current_read_source;
    }
    if(!work)return false;
    uint32_t conflict_function=0;
    const auto normal=s.NormalOriginalCaller(caller,caller_task,&conflict_function);
    if(normal==State::ReentrantWorkConflict::kNone)return false;
    if(work->render_iteration)
     return !s.ReentrantOriginalFrame(caller,ordinal,uint32_t(args[1]),uint32_t(args[2]),&diagnostic.value);
    // One actual normal-admission refusal; low16 Latest metadata is intact.
    // No failed render bit is fabricated for a successful readless issuer.
    diagnostic.value.latest_kind=(diagnostic.value.latest_kind&0xFFFFu) |
        (uint32_t(normal)<<16) | (OriginalLogonFunctionRole(conflict_function)<<24);
    return true;
   };
   if(diagnostic.Refused(11,!s.aligned,__LINE__) || diagnostic.Refused(12,s.job,__LINE__) ||
      diagnostic.Refused(13,s.stopping.load(),__LINE__) || diagnostic.Refused(14,s.retired.load(),__LINE__) ||
      diagnostic.Refused(15,s.uncertain.load(),__LINE__) || diagnostic.Refused(16,latest(),__LINE__))return diagnostic.Finish(rejected);
   // A proven local async initializer runs on this same caller while its
   // exact OLD render/read stays retained. All other owned caller work still
   // refuses here; null overlap never receives an async substitute.
  }
  if(diagnostic.Refused(17,!s.EnterPublic(false),__LINE__))return diagnostic.Finish(rejected);
  struct PublicExit{State& s;~PublicExit(){s.LeavePublic(false);}} public_exit{s};
  auto* pm=kernel->xam_state()->profile_manager();std::error_code error;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  const auto wait=[&]{
   {
    std::lock_guard lock(s.mutex);
    if(s.stopping.load() || s.retired.load() || s.uncertain.load())return false;
   }
   if(global_critical_region::is_held_by_current_thread() ||
       std::chrono::steady_clock::now()>=deadline)return false;
   caller->Delay(0,0,uint64_t(-160000ll));
   return ActualThread()==caller && caller->kernel_state()==kernel;
  };
  const auto bytes=ordinal==0x20B?32u:0x620u;
  const std::array<NativeServiceBackingRange,1> input_range{{{uint32_t(args[0]),bytes,false}}};
  auto preparation_input=kernel->memory()->BorrowNativeServiceBacking(input_range);
  if(diagnostic.Refused(20,!preparation_input,__LINE__))return diagnostic.Finish(rejected);
  std::shared_ptr<const NativeServiceBackingLease> preparation_overlap;
  if(args[2]){
   const std::array<NativeServiceBackingRange,1> range{{{uint32_t(args[2]),sizeof(XAM_OVERLAPPED),true}}};
   preparation_overlap=kernel->memory()->BorrowNativeServiceBacking(range);
   if(diagnostic.Refused(20,!preparation_overlap,__LINE__))return diagnostic.Finish(rejected);
  }
  auto expected=pm->CaptureRosterExpected(error);
  while(error==std::errc::device_or_resource_busy && wait())expected=pm->CaptureRosterExpected(error);
  if(diagnostic.Refused(18,error,__LINE__) || diagnostic.Refused(19,!expected.roster,__LINE__))return diagnostic.Error(error).Finish(rejected);
  std::array<uint64_t,4> before{};
  diagnostic.value.expected_mask_available=1;
  for(size_t i=0;i<4;++i){
   before[i]=expected.roster->slots()[i].xuid;
   if(before[i]){diagnostic.value.expected_nonempty_mask|=1u<<i;++diagnostic.value.expected_nonempty_count;}
  }
  std::array<uint8_t,0x620> wire{};
  if(diagnostic.Refused(20,util::ReadGuestIoBuffer(*kernel->memory(),uint32_t(args[0]),std::span(wire).first(bytes))!=0,__LINE__))return diagnostic.Finish(rejected);
  const auto original=NativeProfileIdentityRequest::Capture(ordinal,
      std::span(wire).first(bytes),uint32_t(args[1]),before,error);
  if(diagnostic.Refused(21,!original,__LINE__) || diagnostic.Refused(22,error,__LINE__))return diagnostic.Error(error).Finish(rejected);
  xam::ProfileRosterDesired desired;desired.xuids=original->desired();
  diagnostic.value.input_mask_available=1;diagnostic.value.desired_mask_available=1;
  diagnostic.value.selected_mask=original->selected_mask_;diagnostic.value.remove_mask=original->remove_mask_;
  diagnostic.value.input_nonempty_mask=original->selected_mask_|original->remove_mask_;
  for(size_t i=0;i<4;++i){
   if(diagnostic.value.input_nonempty_mask&(1u<<i))++diagnostic.value.input_nonempty_count;
   if(desired.xuids[i]){diagnostic.value.desired_nonempty_mask|=1u<<i;++diagnostic.value.desired_nonempty_count;}
  }
  desired.notify_host_compatibility=false;
  // Original224's actual device selector resolves through the registered
  // writable source capability. Canonical durable Account comparison follows
  // in Prepare; bytes/names/XUID alone never authorize a source or adoption.
  if(ordinal==0x224)for(size_t slot=0;slot<4;++slot){
   if(!(original->selected_mask_&(1u<<slot)))continue;
   const auto device=xe::load_and_swap<uint32_t>(wire.data()+slot*0x188+0x184);
   const auto source=kernel->content_manager()->sources().WritableSource(device);
   if(diagnostic.Refused(23,!source,__LINE__) || diagnostic.Refused(24,!source->available,__LINE__) ||
      diagnostic.Refused(25,!source->profile_writable,__LINE__) || diagnostic.Refused(26,!source->storage,__LINE__) ||
      diagnostic.Refused(27,source->storage->retiring(),__LINE__) || diagnostic.Refused(28,!source->managed_identity,__LINE__) ||
      diagnostic.Refused(29,source->managed_identity->retiring(),__LINE__))return diagnostic.Finish(rejected);
   xam::ProfileRosterSelectionOrigin origin;
   origin.device_id=source->source.device_id;origin.source_id=source->source.id;
   origin.source_generation=source->generation;origin.original_account_row.emplace();
   std::copy_n(wire.data()+slot*0x188,0x188,origin.original_account_row->data());
   desired.origins[slot]=std::move(origin);
  }
  const auto intent=pm->PrepareRosterWithProgress(desired,expected,error,deadline,wait);
  if(diagnostic.Refused(30,!intent,__LINE__) || diagnostic.Refused(31,error,__LINE__))return diagnostic.Error(error).Finish(rejected);
  for(;;){
   bool retryable=false;
   if(owner->EnqueuePrepared(intent,&*original,caller,uint32_t(args[0]),uint32_t(args[1]),
                            uint32_t(args[2]),false,error,&retryable,observation,deadline))break;
   if(!retryable || !wait()){
    pm->CancelRosterPreparation(intent);
    diagnostic.Refused(32,true,__LINE__);return diagnostic.Error(error).Finish(rejected);
   }
  }
  preparation_input.reset();preparation_overlap.reset(); // Job now owns exact backing.
  for(;;){
   bool ready=false,closed=false;
   {
    std::lock_guard lock(s.mutex);
    closed=!s.job || s.job->intent!=intent || s.job->canceled || s.stopping.load() ||
        s.retired.load() || s.uncertain.load();
    ready=!closed && s.job->initializer_ready;
   }
   if(ready)break;
   if(diagnostic.Refused(33,closed,__LINE__) || diagnostic.Refused(34,std::chrono::steady_clock::now()>=deadline,__LINE__)){owner->CancelQueued(intent);return diagnostic.Finish(rejected);}
   // Preserve the original caller's process/PCR/fiber and permit genuine old
   // native work to drain; no host CV or metadata/FS lock blocks guest CPU.
   caller->Delay(0,0,uint64_t(-160000ll));
  }
  const auto native=owner->ExecutePreparedOn(*caller);
  if(diagnostic.Refused(35,!native.succeeded,__LINE__))return diagnostic.Finish({Disposition::kExecutionUncertain,uint32_t(native.return_value)});
  // Every actual native DWORD, including negative HRESULTs and async normal
  // returns, goes back unchanged. Later consumer completion remains separate.
  return diagnostic.Finish({Disposition::kActualReturned,uint32_t(native.return_value)});
 }catch(...){if(const auto owner=Lookup(kernel))owner->state_->Lose(__LINE__);
  diagnostic.Refused(36,true,__LINE__);return diagnostic.Finish({Disposition::kExecutionUncertain,kAbort});}
}

void NativeProfileIdentityOwner::RetainPreparationCleanup(
    const std::shared_ptr<xam::ProfileRosterIntent>& intent) noexcept {
 if(!intent)return;
 try{
  const auto phase=intent->result().phase;
  if(phase!=xam::ProfileRosterPhase::kPreparing && phase!=xam::ProfileRosterPhase::kPrepared &&
      phase!=xam::ProfileRosterPhase::kCanceling)return;
  std::shared_ptr<xam::ProfileRosterIntent> retired;
  {
   std::lock_guard lock(state_->mutex);
   // retired is published only by the actual all-CPU/GPU-stopped lane. PM
   // still owns its pending operation until its subsequent Close authority.
   if(state_->retired.load(std::memory_order_acquire))return;
   if(state_->job && state_->job->intent==intent)return; // actual queued/entered owner already retains outcome
   if(state_->job || (state_->bootstrap_cleanup && state_->bootstrap_cleanup!=intent &&
       state_->bootstrap_cleanup->result().phase!=xam::ProfileRosterPhase::kCanceled)){
    state_->Lose(__LINE__);return; // never replace another live cleanup owner
   }
   retired=std::move(state_->bootstrap_cleanup);state_->bootstrap_cleanup=intent;
  }
  state_->changed.notify_all(); // displaced canceled metadata dies outside State
 }catch(...){state_->Lose(__LINE__);}
}
xam::ProfileRosterResult NativeProfileIdentityOwner::SubmitPrepared(
    std::shared_ptr<xam::ProfileRosterIntent> intent) noexcept {
 return SubmitPreparedUntil(std::move(intent),std::chrono::steady_clock::now()+std::chrono::seconds(15));
}
xam::ProfileRosterResult NativeProfileIdentityOwner::SubmitPreparedUntil(
    std::shared_ptr<xam::ProfileRosterIntent> intent,
    std::chrono::steady_clock::time_point deadline) noexcept {
 try{
  const bool external=ActualThread()==nullptr;
  if(!state_->EnterPublic(external)){
   auto result=intent?intent->result():xam::ProfileRosterResult{};
   result.error=std::make_error_code(std::errc::operation_canceled);return result;
  }
  struct PublicExit{State& s;bool external;~PublicExit(){s.LeavePublic(external);}} public_exit{*state_,external};
  std::error_code error;auto* actual=ActualThread();
  for(;;){
   bool retryable=false;
   if(EnqueuePrepared(intent,nullptr,nullptr,0,0,0,false,error,&retryable,nullptr,deadline))break;
   if(!retryable || state_->stopping.load() || state_->retired.load() || state_->uncertain.load() ||
       std::chrono::steady_clock::now()>=deadline){
    auto result=state_->kernel->xam_state()->profile_manager()->CancelRosterPreparation(intent);
    result.error=error?error:std::make_error_code(std::errc::operation_canceled);return result;
   }
   if(actual)actual->Delay(0,0,uint64_t(-160000ll));
   else{std::unique_lock lock(state_->mutex);state_->changed.wait_for(lock,std::chrono::milliseconds(16));}
  }
  for(;;){
   const auto result=intent->result();
   if((result.maps_committed && result.writers_available) ||
      result.phase==xam::ProfileRosterPhase::kCommitted || result.phase==xam::ProfileRosterPhase::kCanceled ||
      result.phase==xam::ProfileRosterPhase::kUncertain)return result;
   if(state_->stopping.load(std::memory_order_acquire)){
    auto pending=result;pending.error=std::make_error_code(std::errc::operation_canceled);return pending;
   }
   if(std::chrono::steady_clock::now()>=deadline || state_->retired.load() || state_->uncertain.load()){
    CancelQueued(intent);auto pending=intent->result();
    pending.error=std::make_error_code(std::errc::device_or_resource_busy);return pending;
   }
   if(actual){actual->Delay(0,0,uint64_t(-160000ll));}
   else{
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_for(lock,std::chrono::milliseconds(16));
   }
  }
 }catch(...){state_->Lose(__LINE__);xam::ProfileRosterResult result;
  result.error=std::make_error_code(std::errc::operation_canceled);
  result.phase=xam::ProfileRosterPhase::kUncertain;return result;}
}

struct NativeProfileIdentityOwner::CountedCreate::Data {
 std::shared_ptr<State> state;
 std::shared_ptr<State::CountedAllocation> allocation;
 cpu::ppc::PPCContext* context=nullptr;
 bool complete=false;
};
NativeProfileIdentityOwner::CountedCreate::CountedCreate(std::shared_ptr<Data> data,bool admitted)
 :data_(std::move(data)),admitted_(admitted){}
NativeProfileIdentityOwner::CountedCreate::CountedCreate(CountedCreate&&) noexcept=default;
NativeProfileIdentityOwner::CountedCreate::~CountedCreate(){if(data_ && !data_->complete)data_->state->Lose(__LINE__);}
void NativeProfileIdentityOwner::CountedCreate::Complete(uint32_t status,uint32_t body) noexcept {
 if(!data_ || data_->complete)return;
 try{
  auto& s=*data_->state;auto& row=*data_->allocation;auto& c=*data_->context;
  if(ActualThread()!=row.creator.get() || c.thread_state!=row.creator->thread_state() ||
     c.kernel_state!=s.kernel || c.lr!=row.producer.lr || c.r[1]!=row.producer.sp ||
     c.r[13]!=row.producer.pcr){s.Lose(__LINE__);return;}
  if(int32_t(status)<0){
   if(body){s.Lose(__LINE__);return;}
   std::lock_guard lock(s.mutex);row.failed=true;data_->complete=true;return;
  }
  const auto header_bytes=uint32_t(sizeof(X_OBJECT_HEADER));
  if(body<0x1000+header_bytes || (body&3) || body>UINT32_MAX-0x30 ||
     s.kernel->memory()->QueryRangeAccess(body-header_bytes,header_bytes+0x30)==memory::PageAccess::kNoAccess){s.Lose(__LINE__);return;}
  const auto* header=s.kernel->memory()->TranslateVirtual<const X_OBJECT_HEADER*>(body-header_bytes);
  if(uint32_t(header->object_type_ptr)!=0x81A821E0 || int32_t(header->pointer_count)!=1 ||
     int32_t(header->handle_count)){s.Lose(__LINE__);return;}
  std::lock_guard lock(s.mutex);
  if(s.FindCounted(body)){s.Lose(__LINE__);return;}
  row.body=body;row.allocation=body-header_bytes;row.allocated=true;data_->complete=true;
 }catch(...){data_->state->Lose(__LINE__);}
}
NativeProfileIdentityOwner::CountedCreate NativeProfileIdentityOwner::BeforeCountedCreate(
    KernelState* kernel,void* raw,uint32_t type,uint32_t attributes,uint32_t bytes,uint32_t output) noexcept {
 try{
  if(!raw || type!=0x81A821E0)return {{},true};
  const auto owner=Lookup(kernel);if(!owner)return {{},true};
  auto& s=*owner->state_;
  if(s.retired.load(std::memory_order_acquire))return {{},false};
  auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
  if(c.lr!=0x816A8B5C || kernel->GetUserModuleForAddress(0x816A8B20).get()!=s.config.catalog->calls()->module().get())return {{},true};
  auto* thread=ActualThread();
  if(!thread || thread->kernel_state()!=kernel || thread->thread_state()->context()!=&c ||
     c.kernel_state!=kernel || c.processor!=kernel->processor() || c.thread_state!=thread->thread_state() ||
     (thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM &&
      thread->native_frame_thread_identity().process_type!=X_PROCTYPE_TITLE) ||
     c.r[1]>UINT32_MAX-0x50 || (c.r[1]&15) || c.r[13]!=thread->pcr_ptr() || attributes ||
     bytes!=0x30 || output!=c.r[1]+0x50 ||
     kernel->ResolveNativeImportCaller(0x816A8B5C).domain!=s.config.domain ||
     global_critical_region::is_held_by_current_thread())return {{},false};
  auto row=std::make_shared<State::CountedAllocation>();
  row->producer={s.root,thread->thread_id(),thread->pcr_ptr(),uint32_t(c.r[1]),0x816A8B5C,0x816A8B20};
  row->creator=retain_object(thread);row->domain=s.config.domain->Acquire();
  if(!row->domain)return {{},false};
  auto data=std::make_shared<CountedCreate::Data>();data->state=owner->state_;
  data->context=&c;data->allocation=row;
  {
   std::lock_guard lock(s.mutex);
   const auto parent=s.Latest(thread->thread_id());
   const bool captured=s.job && parent && parent->source==s.job->source;
   if((s.stopping.load() && !captured) || s.retired.load() || s.uncertain.load() || !s.next_counted ||
      s.next_counted==UINT64_MAX || s.retired_counted.size()>=128)return {{},false};
   const auto slot=std::find_if(s.counted.begin(),s.counted.end(),[](const auto& prior){
    return !prior || ((prior->failed || prior->free_returned) && prior.use_count()==1);
   });
   if(slot==s.counted.end())return {{},false};
   row->generation=s.next_counted++;
   if(*slot)s.retired_counted.push_back(std::move(*slot));
   *slot=row;
  }
  return {std::move(data),true};
 }catch(...){return {{},false};}
}

struct NativeProfileIdentityOwner::CountedDelete::Data {
 std::shared_ptr<State> state;
 std::shared_ptr<State::CountedAllocation> allocation;
 std::shared_ptr<State::Expected> expected;
 cpu::ppc::PPCContext* context=nullptr;
 uint32_t sp=0,pcr=0;
 object_ref<XThread> thread;
 bool deleted=false,freeing=false,complete=false;
};
NativeProfileIdentityOwner::CountedDelete::CountedDelete(std::shared_ptr<Data> data):data_(std::move(data)){}
NativeProfileIdentityOwner::CountedDelete::CountedDelete(CountedDelete&&) noexcept=default;
NativeProfileIdentityOwner::CountedDelete::~CountedDelete(){if(data_ && !data_->complete)data_->state->Lose(__LINE__);}
void NativeProfileIdentityOwner::CountedDelete::DeleteReturned(bool executed) noexcept {
 if(!data_ || data_->deleted || data_->complete)return;
 try{
  auto& s=*data_->state;std::lock_guard lock(s.mutex);
  const auto& e=*data_->expected;
  if(!executed || !e.entered || !e.returned || e.call_returned || !e.actual ||
     ActualThread()!=data_->thread.get() || data_->context->r[1]!=data_->sp ||
     data_->context->r[13]!=data_->pcr){s.Lose(__LINE__);return;}
  data_->expected->call_returned=true;data_->expected->healthy=true;
  data_->allocation->delete_returned=true;data_->deleted=true;
 }catch(...){data_->state->Lose(__LINE__);}
}
void NativeProfileIdentityOwner::CountedDelete::BeforeFree() noexcept {
 if(!data_ || data_->freeing || data_->complete)return;
 try{std::lock_guard lock(data_->state->mutex);
  if(!data_->deleted || ActualThread()!=data_->thread.get() ||
     data_->context->r[1]!=data_->sp || data_->context->r[13]!=data_->pcr){data_->state->Lose(__LINE__);return;}
  data_->allocation->free_entered=true;data_->freeing=true;
 }catch(...){data_->state->Lose(__LINE__);}
}
void NativeProfileIdentityOwner::CountedDelete::FreeReturned(bool executed) noexcept {
 if(!data_ || data_->complete)return;
 try{auto& s=*data_->state;std::lock_guard lock(s.mutex);
  if(!data_->freeing || !executed || ActualThread()!=data_->thread.get() ||
     data_->context->r[1]!=data_->sp || data_->context->r[13]!=data_->pcr){s.Lose(__LINE__);return;}
  const auto e=std::find(s.expected.begin(),s.expected.end(),data_->expected);
  if(e==s.expected.end()){s.Lose(__LINE__);return;}
  data_->allocation->free_returned=true;data_->complete=true;
  s.retired_expected.push_back(*e);s.expected.erase(e);
 }catch(...){data_->state->Lose(__LINE__);}
}
NativeProfileIdentityOwner::CountedDelete NativeProfileIdentityOwner::BeforeCountedDelete(
    KernelState* kernel,void* raw,uint32_t body,uint32_t allocation,uint32_t type,
    uint32_t deletion,uint32_t freeing) noexcept {
 try{
  if(!raw || type!=0x81A821E0)return CountedDelete({});
  const auto owner=Lookup(kernel);if(!owner)return CountedDelete({});
  auto& s=*owner->state_;
  if(s.retired.load(std::memory_order_acquire))return CountedDelete({});
  auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto* thread=ActualThread();
  if(!thread || thread->kernel_state()!=kernel || thread->thread_state()->context()!=&c ||
     c.kernel_state!=kernel || c.processor!=kernel->processor() || c.thread_state!=thread->thread_state() ||
     (thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM &&
      thread->native_frame_thread_identity().process_type!=X_PROCTYPE_TITLE) ||
     c.r[1]>UINT32_MAX || (c.r[1]&15) || c.r[13]!=thread->pcr_ptr() ||
     deletion!=0x816A7EE0 || freeing!=0x81A72734 ||
     Word(*kernel->memory(),body)!=std::optional<uint32_t>(0x815F7E60) ||
     Word(*kernel->memory(),0x815F7E60)!=std::optional<uint32_t>(0x816A9208) ||
     global_critical_region::is_held_by_current_thread()){s.Lose(__LINE__);return CountedDelete({});}
  auto expected=std::make_shared<State::Expected>();
  expected->thread=retain_object(thread);expected->function=0x816A88E8;
  expected->count=1;expected->args[0]=body;
  auto data=std::make_shared<CountedDelete::Data>();data->state=owner->state_;
  data->expected=expected;data->context=&c;data->sp=uint32_t(c.r[1]);data->pcr=thread->pcr_ptr();
  data->thread=retain_object(thread);
  {
   std::lock_guard lock(s.mutex);const auto row=s.FindCounted(body);
   if(!row || row->allocation!=allocation || row->delete_entered || s.expected.size()>=64 ||
      s.retired.load() || !s.next_work){s.Lose(__LINE__);return CountedDelete({});}
   // Actual Ob manager owns Processor::Execute(delete_proc,{body}). The two
   // pinned frameless tails reach88E8; its first real entry records padded
   // SP/LR. Never calculate/fabricate that frame or add a counted object ref.
   expected->serial=s.next_work++;expected->source=s.EarlierSource(thread->thread_id());
   data->allocation=row;row->delete_entered=true;s.expected.push_back(expected);
  }
  return CountedDelete(std::move(data));
 }catch(...){return CountedDelete({});}
}

struct NativeProfileIdentityOwner::ListenerObjectUse::Data {
 std::shared_ptr<State> state;
 std::shared_ptr<State::Work> work;
 cpu::ppc::PPCContext* context=nullptr;
 uint32_t handle=0,body=0;
 bool release=false,counted_reference_observed=false,complete=false;
};
NativeProfileIdentityOwner::ListenerObjectUse::ListenerObjectUse(std::shared_ptr<Data> data,void* state)
    :data_(std::move(data)),frame_state_(state){}
NativeProfileIdentityOwner::ListenerObjectUse::ListenerObjectUse(ListenerObjectUse&& other) noexcept
    :data_(std::move(other.data_)),frame_state_(std::exchange(other.frame_state_,nullptr)){}
NativeProfileIdentityOwner::ListenerObjectUse::~ListenerObjectUse(){
 auto* state=static_cast<State*>(frame_state_);if(!state)return;
 if(data_ && !data_->complete)state->Lose(__LINE__);
 // No independent shared/native-resource local survives the frame counter.
 // Kernel/Service retain State until its strict zero-counter stop admission.
 // The raw counter also covers observer allocation/validation failure tails.
 data_.reset();state->listener_object_frames.fetch_sub(1,std::memory_order_acq_rel);
}
void NativeProfileIdentityOwner::ListenerObjectUse::ObserveCountedReference(uint32_t actual_body) noexcept {
 if(!data_)return;
 // Sole callsite is the real counted-success branch AFTER ReferenceCounted.
 // GCRC is held: only this prebound leaf's scalar fields may be touched here.
 data_->body=actual_body;data_->counted_reference_observed=true;
}
void NativeProfileIdentityOwner::ListenerObjectUse::ReferenceReturned(uint32_t status) noexcept {
 if(!data_ || data_->complete)return;
 try{
  auto& d=*data_;auto& s=*d.state;
  if(global_critical_region::is_held_by_current_thread()){s.Lose(__LINE__);return;}
  std::lock_guard lock(s.mutex);
  auto* thread=ActualThread();
  if(d.release || !thread || d.context!=thread->thread_state()->context() ||
     thread->thread_id()!=d.work->frame.thread || !DwordIs(d.context->lr,0x816A8C5Cu) ||
     !DwordIs(d.context->r[1],d.work->frame.sp-0x90) ||
     !DwordIs(d.context->r[13],d.work->frame.pcr) || s.retired.load() || s.uncertain.load()){
   s.Lose(__LINE__);return;
  }
  if(int32_t(status)>=0){
   const auto allocation=d.counted_reference_observed?s.FindCounted(d.body):nullptr;
   if(status || !allocation || allocation->delete_entered || !allocation->listener_registered ||
      !allocation->listener_registration_entered){s.Lose(__LINE__);return;}
   d.work->listener_allocation=allocation;
  }else if(d.counted_reference_observed){s.Lose(__LINE__);return;}
  if(!s.listeners || !s.listeners->ReferenceReturned(d.work->listener_cell,d.handle,status,d.body)){
   s.Lose(__LINE__);return;
  }
  d.complete=true;
 }catch(...){data_->state->Lose(__LINE__);}
}
void NativeProfileIdentityOwner::ListenerObjectUse::DereferenceReturned() noexcept {
 if(!data_ || data_->complete)return;
 try{
  auto& d=*data_;auto& s=*d.state;
  if(global_critical_region::is_held_by_current_thread()){s.Lose(__LINE__);return;}
  std::lock_guard lock(s.mutex);auto* thread=ActualThread();
  if(!d.release || !thread || d.context!=thread->thread_state()->context() ||
     thread->thread_id()!=d.work->frame.thread || !DwordIs(d.context->lr,0x816A8CB4u) ||
     !DwordIs(d.context->r[1],d.work->frame.sp-0x90) ||
     !DwordIs(d.context->r[13],d.work->frame.pcr) || !d.work->listener_allocation ||
     d.work->listener_allocation->body!=d.body || s.retired.load() || s.uncertain.load() ||
     !s.listeners || !s.listeners->ReferenceReleaseReturned(d.work->listener_cell,d.body)){
   s.Lose(__LINE__);return;
  }
  // Do not read the native body here: actual final deref may have freed it.
  // The counted-delete/free receipts alone describe destruction, not this call.
  d.complete=true;
 }catch(...){data_->state->Lose(__LINE__);}
}
NativeProfileIdentityOwner::ListenerObjectUse NativeProfileIdentityOwner::BeforeListenerHandleReference(
    KernelState* kernel,void* raw,uint32_t handle,uint32_t type,uint32_t output) noexcept {
 if(!raw)return ListenerObjectUse({});
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 if(!DwordIs(c.lr,0x816A8C5Cu))return ListenerObjectUse({});
 State* frame_state=nullptr;
 try{
  const auto owner=Lookup(kernel);if(!owner)return ListenerObjectUse({});auto& s=*owner->state_;
  frame_state=&s;s.listener_object_frames.fetch_add(1,std::memory_order_acq_rel);
  if(global_critical_region::is_held_by_current_thread()){s.Lose(__LINE__);return ListenerObjectUse({},frame_state);}
  auto* thread=ActualThread();std::lock_guard lock(s.mutex);
  const auto work=thread?s.Latest(thread->thread_id(),0x816A8C00):nullptr;
  if(!thread || thread->kernel_state()!=kernel || c.kernel_state!=kernel ||
     c.thread_state!=thread->thread_state() || thread->thread_state()->context()!=&c ||
     !work || !work->listener_arguments || !work->listener_cell || work->listener_reference_started ||
     handle!=work->listener_arguments->handle || type!=0x81A821E0 ||
     !DwordIs(c.r[3],handle) || !DwordIs(c.r[4],type) || !DwordIs(c.r[5],output) ||
     !DwordIs(c.r[1],work->frame.sp-0x90) || !DwordIs(c.r[13],work->frame.pcr) ||
     output!=work->frame.sp-0x40 || s.retired.load() || s.uncertain.load()){s.Lose(__LINE__);return ListenerObjectUse({},frame_state);}
  auto data=std::make_shared<ListenerObjectUse::Data>();data->state=owner->state_;data->work=work;
  data->context=&c;data->handle=handle;work->listener_reference_started=true;
  return ListenerObjectUse(std::move(data),frame_state);
 }catch(...){if(frame_state)frame_state->Lose(__LINE__);
  else if(const auto owner=Lookup(kernel))owner->state_->Lose(__LINE__);
  return ListenerObjectUse({},frame_state);}
}
NativeProfileIdentityOwner::ListenerObjectUse NativeProfileIdentityOwner::BeforeListenerObjectDereference(
    KernelState* kernel,void* raw,uint32_t body) noexcept {
 if(!raw)return ListenerObjectUse({});
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 if(!DwordIs(c.lr,0x816A8CB4u))return ListenerObjectUse({});
 State* frame_state=nullptr;
 try{
  const auto owner=Lookup(kernel);if(!owner)return ListenerObjectUse({});auto& s=*owner->state_;
  frame_state=&s;s.listener_object_frames.fetch_add(1,std::memory_order_acq_rel);
  if(global_critical_region::is_held_by_current_thread()){s.Lose(__LINE__);return ListenerObjectUse({},frame_state);}
  auto* thread=ActualThread();std::lock_guard lock(s.mutex);
  const auto work=thread?s.Latest(thread->thread_id(),0x816A8C00):nullptr;
  if(!thread || thread->kernel_state()!=kernel || c.kernel_state!=kernel ||
     c.thread_state!=thread->thread_state() || thread->thread_state()->context()!=&c ||
     !work || !work->listener_allocation || work->listener_allocation->body!=body || !DwordIs(c.r[3],body) ||
     !DwordIs(c.r[1],work->frame.sp-0x90) || !DwordIs(c.r[13],work->frame.pcr) ||
     s.retired.load() || s.uncertain.load() ||
     !s.listeners || !s.listeners->BeforeReferenceRelease(work->listener_cell,body)){
   s.Lose(__LINE__);return ListenerObjectUse({},frame_state);
  }
  auto data=std::make_shared<ListenerObjectUse::Data>();data->state=owner->state_;data->work=work;
  data->context=&c;data->body=body;data->release=true;
  return ListenerObjectUse(std::move(data),frame_state);
 }catch(...){if(frame_state)frame_state->Lose(__LINE__);
  else if(const auto owner=Lookup(kernel))owner->state_->Lose(__LINE__);
  return ListenerObjectUse({},frame_state);}
}

bool NativeProfileIdentityOwner::OwnsIngressCall(GuestRuntimeCallExecutor* calls,
    XThread& thread,uint16_t ordinal,std::span<const uint32_t> args) {
 const auto owner=Lookup(thread.kernel_state());
 if(!owner || !calls || args.size()!=3 || ActualThread()!=&thread ||
    owner->state_->config.catalog->calls().get()!=calls)return false;
 auto& s=*owner->state_;std::lock_guard lock(s.mutex);
 const auto job=s.job;
 if(!job || !job->native_entered || job->execution_uncertain || s.uncertain.load() ||
    s.retired.load() || args[0]!=job->input || args[1]!=job->flags || args[2]!=job->overlap ||
    ordinal!=(job->original?job->original->ordinal():0x224))return false;
 if(job->original_caller)return job->original_caller.get()==&thread;
 const auto work=s.Latest(thread.thread_id(),0x81696558);
 return thread.native_frame_thread_identity().process_type==X_PROCTYPE_SYSTEM &&
     work && work->host_body && work->birth.generation && work->source==job->source;
}

GuestRuntime::ExecutionResult NativeProfileIdentityOwner::RunScheduledBody(
    XThread& thread,uint64_t serial,
    const std::function<GuestRuntime::ExecutionResult(XThread&)>& work) {
  auto& s=*state_;auto* context=thread.thread_state()->context();
  const auto frame=s.ReadFrame(*context,0x81696558);
  const auto task=NativeProfileIdentityTaskContext::Read(
      *s.config.catalog->calls(),*s.config.domain,0x81696558,*context);
  if(!frame || !task || ActualThread()!=&thread)
   return GuestRuntime::ExecutionResult{false,kAbort,"Actual identity callback Task unavailable"};
  auto whole=std::make_shared<NativeProfileIdentityOwner::State::Work>();
  whole->frame=*frame;whole->thread=retain_object(&thread);
  whole->domain=s.config.domain->Acquire();whole->host_body=true;
  {
   std::lock_guard lock(s.mutex);
   const auto* node=s.tasks.FindLiveTask(*task);
   if(!s.job || s.job->serial!=serial || !node || node->callback!=0x81696558 ||
      !node->continuation_ticket || node->transport_only || node->birth.origin!=s.job->source || !whole->domain || s.active.size()>=256){s.Lose(__LINE__);
    return GuestRuntime::ExecutionResult{false,kAbort,"Identity callback genealogy mismatch"};}
   const auto birth=s.tasks.EnterWork(*frame,*task,node->argument);
   if(!birth){s.Lose(__LINE__);return GuestRuntime::ExecutionResult{false,kAbort,"Identity callback already entered"};}
   whole->birth=*birth;whole->source=birth->origin;whole->serial=s.next_work++;
   const auto slot=std::find_if(s.job->writer_reads.begin(),s.job->writer_reads.end(),
                               [](const auto& read){return bool(read);});
   if(!whole->serial || !s.next_work || slot==s.job->writer_reads.end()){
    s.Lose(__LINE__);return GuestRuntime::ExecutionResult{false,kAbort,"Preallocated callback scope exhausted"};}
   whole->read_pool=&s.job->writer_reads;whole->read=std::move(*slot);
   s.active.push_back(whole); // Retain BEFORE an actual ThreadState attachment.
   if(!whole->read->Enter(s.kernel,&thread,context->thread_state,task.value(),birth->generation,
       whole->serial,s.config.catalog->calls()->module()->hmodule_ptr(),s.root.generation)){
    s.Lose(__LINE__);return GuestRuntime::ExecutionResult{false,kAbort,"Actual callback roster scope refused"};}
  }
  const auto result=work(thread); // No owner/PM/GCRC lock crosses actual work.
  {
   std::lock_guard lock(s.mutex);
   if(!result.succeeded || !s.tasks.ReturnWork(whole->frame,whole->birth) ||
      !whole->read->Leave()){whole->uncertain=true;s.Lose(__LINE__);return result;}
   if(!s.RecycleRead(whole)){s.Lose(__LINE__);return GuestRuntime::ExecutionResult{false,kAbort,"Callback scope recycling failed"};}
   whole->normal_return=true;
   const auto i=std::find(s.active.begin(),s.active.end(),whole);
   if(i==s.active.end()){s.Lose(__LINE__);return GuestRuntime::ExecutionResult{false,kAbort,"Missing callback owner"};}
   // Transfer the final retiring local before State unlock; a concurrent
   // host reap must never make this guest epilogue the last owner.
   s.host_retired.push_back(std::move(whole));s.active.erase(i);
  }
  return result;
}

GuestRuntime::ExecutionResult NativeSystemTaskContinuation::InvokeOwnedProfileIdentityAndWait(
    NativeProfileIdentityOwner& owner,uint64_t serial,Work work) {
 auto* bootstrap=ActualThread();
 if(!bootstrap || !serial || !owner.OwnsScheduledWork(serial))
  return {false,kAbort,"Owned identity operation no longer admitted"};
 auto retained=owner.shared_from_this();
 auto expected=std::make_shared<NativeProfileIdentityOwner::State::Expected>();
 const auto before=[retained,expected,serial](XThread& actual,std::span<const uint32_t> args){
  auto& s=*retained->state_;
  if(args.size()!=4 || args[0]!=0x81696558 || !args[1] || args[2] || !args[3] ||
     ActualThread()!=&actual || actual.kernel_state()!=s.kernel ||
     actual.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
     global_critical_region::is_held_by_current_thread())return false;
  std::lock_guard lock(s.mutex);
  if(!s.job || s.job->serial!=serial || s.job->execution_uncertain ||
     s.uncertain.load() || s.retired.load() || s.expected.size()>=64)return false;
  expected->serial=serial;expected->source=s.job->source;
  expected->thread=retain_object(&actual);expected->count=4;
  expected->function=0x816BA440;expected->identity_continuation=true;
  std::copy(args.begin(),args.end(),expected->args.begin());
  s.expected.push_back(expected);return true;
 };
 const auto after=[retained,expected](const cpu::GuestCallResult& actual){
  auto& s=*retained->state_;std::lock_guard lock(s.mutex);
  if(!expected->serial || expected->call_returned || !expected->entered ||
     !expected->returned || !expected->actual || !actual.executed){s.Lose(__LINE__);return;}
  expected->call_returned=true;expected->healthy=true;
  // Retain the actual frame/ticket/output receipt through real Wait+Close.
  // The host step reaps only after InvokeAndWaitImpl has itself returned.
 };
 const auto actual_work=[retained,serial,work=std::move(work)](XThread& thread){
  return retained->RunScheduledBody(thread,serial,work);
 };
 const auto result=InvokeAndWaitImpl(*bootstrap,std::move(actual_work),Admission::Role::kOwnedProfileIdentity,
     nullptr,[retained,serial]{return retained->OwnsScheduledWork(serial);},before,after,
     [retained]{return retained->RetainedForActualKernelStop();},
     [retained,bootstrap]{(void)retained->ReapReturnedOnBootstrap(*bootstrap,false);});
 {
  auto& s=*retained->state_;std::lock_guard lock(s.mutex);
  if(!result.succeeded || !expected->healthy || !expected->call_returned){s.Lose(__LINE__);return result;}
  const auto i=std::find(s.expected.begin(),s.expected.end(),expected);
  if(i==s.expected.end()){s.Lose(__LINE__);return {false,kAbort,"Missing actual Schedule owner"};}
  s.retired_expected.push_back(expected);s.expected.erase(i);
 }
 return result;
}


// Observe source-neutral original Task operations. True means ONLY that the
// optional profile guard must not claim this native invocation; it is never
// a synthetic native success, reference, callback or data permission.
bool NativeProfileIdentityOwner::ObserveExecutionEntry(void* raw,uint32_t at) noexcept {
 State::HookUse hook_use;
 if(!raw || (!CommonTaskFunction(at) && !ProfileCallback(at)))return false;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 std::optional<Frame> shape;bool handled=false;
 try {
  owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);
  if(!owner)return false;auto& s=*owner->state_;auto* thread=ActualThread();
  if(!thread || s.retired.load(std::memory_order_acquire))return false;
  const auto actual_process=thread->native_frame_thread_identity().process_type;
  if(actual_process!=X_PROCTYPE_SYSTEM && actual_process!=X_PROCTYPE_TITLE)return false;
  // The shared Core entry observer precedes this guard. No family callback,
  // source/writer grant or invented SYSTEM context is involved in this fact.
  const auto common_xmsg=NativeXmsgProvenance::Lookup(c.kernel_state);
  const auto title_producer=actual_process==X_PROCTYPE_TITLE && common_xmsg?
      NativeXmsgProvenance::CaptureTitleTaskProducer(&c,at):
      std::optional<NativeXmsgProvenance::TitleTaskProducerFact>{};
  shape=s.TransportShape(c,at);if(!shape)return false;
  if(!hook_use.Acquire(s.hook_rundown,owner->state_,false))return false;
  enum class Kind {kSchedule,kCreate,kInline,kAllocation,kPublication,kDispatch,kControl,kClose,kDelete,kFree,kCallback};
  Kind kind=Kind::kCallback;Tasks::Birth birth{};
  std::shared_ptr<State::Work> parent;
  {
   std::lock_guard lock(s.mutex);
   const uint32_t task=at==0x816B7DE8?uint32_t(c.r[4]):
       at==0x816B7B68?(uint32_t(c.r[3])&~1u):uint32_t(c.r[3]);
   const auto subject=s.tasks.ObservedTransportSubject(task);
   const bool neutral=subject && s.tasks.IsExecutionSubject(subject->generation);
   if(neutral)birth=*subject;
   if(at==0x816BA440){
    if(c.lr==0xBCBCBCBC)return false; // actual registered continuation keeps its existing owner
    const auto claim=s.ClaimTaskTransport(at,c,thread->thread_id());
    const auto earlier=s.Latest(thread->thread_id());
    if(claim.periodic_factory || claim.profile_producer ||
       (Tasks::ClassifyCallback(c.r[3])==Tasks::CallbackClaim::kCanonicalProfile && earlier &&
        !earlier->execution_only && (earlier->read || earlier->commit || earlier->physical_work)))return false;
    kind=Kind::kSchedule;
   }else if(at==0x816BA668){
    if(s.profile_enabled && (c.lr==0x816F4568 || c.lr==0x816F45B0))return false;
    kind=Kind::kCreate;
   }else if(at==0x816B9D68){
    // The old passive CommonStartProducer remains its sole owner until the
    // common XMsg extraction deliberately replaces that whole chain.
    if(!common_xmsg && s.profile_enabled && s.ClaimTaskTransport(at,c,thread->thread_id()).creation_only)return false;
    kind=Kind::kInline;
   }else if(at==0x816B9A18){
    parent=s.Latest(thread->thread_id(),c.lr==0x816BA46C?0x816BA440u:
        c.lr==0x816BA688?0x816BA668u:c.lr==0x816B9E08?0x816B9D68u:0u);
    if(!parent || !parent->execution_only || !parent->frame.function)return false;
    kind=Kind::kAllocation;
   }else if(at==0x816B9BB8){
    parent=s.Latest(thread->thread_id(),0x816BA440);
    if(!parent || !parent->execution_only || !parent->schedule)return false;
    kind=Kind::kPublication;
   }else if(at==0x816B8B08 || at==0x816B7DE8){
    if(subject && !neutral)return false;
    kind=Kind::kDispatch;
    if(!subject)birth={0,1,task}; // factual scalar argument, NOT an issued allocation birth
   }else if(at==0x816B9AA8 || at==0x816B9F08 || at==0x816BA058 || at==0x816B7600 || at==0x816B7B68){
    if(!neutral)return false;kind=Kind::kControl;
   }else if(at==0x816B7490){
    if(!neutral || s.ClaimTaskTransport(at,c,thread->thread_id()).retained_cleanup)return false;
    // Preserve an ALREADY issued real getter/task-reference release receipt.
    // Generic execution birth is not permission to consume that owner's ref.
    kind=Kind::kClose;
   }else if(at==0x816B7D98 || at==0x816B7500){if(!neutral)return false;kind=Kind::kDelete;
   }else if(at==0x816E6AB0){
    parent=s.TaskFreeParent(thread->thread_id());
    if(!parent || !parent->execution_only)return false;
    birth=parent->birth;kind=Kind::kFree;
   }else if(ProfileCallback(at)){
    const auto earlier=s.Latest(thread->thread_id());
    if(!earlier || !earlier->execution_only)return false;
    // This marks only the innermost neutral execution, never an older data
    // scope. Actual bounded profile consumers retain their own real issuer.
    parent=earlier;birth=earlier->birth;kind=Kind::kCallback;
   }else return false;
   if(actual_process==X_PROCTYPE_TITLE){
    const bool producer=title_producer && title_producer->actual_process==X_PROCTYPE_TITLE &&
        title_producer->root==NativeXmsgProvenance::RootFact{s.root.kernel,s.root.domain,s.root.generation} &&
        title_producer->frame==NativeXmsgProvenance::FrameFact{shape->thread,shape->pcr,shape->sp,shape->lr,at} &&
        title_producer->capture && title_producer->copy &&
        title_producer->callback==0x816A2C80 && title_producer->argument &&
        (kind==Kind::kSchedule || kind==Kind::kInline);
    const bool child=parent && parent->execution_only && parent->execution_title &&
        (kind==Kind::kAllocation || kind==Kind::kPublication || kind==Kind::kCallback);
    // A current SAME-ledger neutral birth permits observing real native
    // controls/dispatch/close only, never creating a new allocation/source.
    const bool known=neutral && birth.generation && s.ExecutionLifetimeFor(birth) &&
        kind!=Kind::kSchedule && kind!=Kind::kCreate && kind!=Kind::kInline;
    if(!common_xmsg || (!producer && !child && !known))return false;
   }
   handled=true;
  }
  // Existing Avatar service independently owns its typed Request/Control.
  // Preserve that real producer annotation on the SAME common birth; it is
  // not profile/read authority or a callback whitelist.
  std::shared_ptr<const NativeAvatarService::CleanupTaskCreationProof> avatar;
  if(kind==Kind::kAllocation)avatar=NativeAvatarService::CaptureOwnedTaskAllocation(s.kernel,c);
  State::CodeOwner caller;
  const bool retained=birth.generation || parent;
  auto frame=s.ReadFrame(c,at,actual_process==X_PROCTYPE_TITLE,&caller,retained);
  bool code_checked=kind!=Kind::kDispatch || Tasks::DispatchCodeAuthenticationRequired(
      birth,c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire));
  bool code=frame && code_checked && CheckTaskExecutionCode(*s.config.catalog->calls());
  const bool service_pump_code=code && kind==Kind::kSchedule && shape->lr==0x818CF164 &&
      DwordIs(c.r[3],0x818CEFD8) && DwordIs(c.r[4],0x81AEE470) &&
      CheckIdentityServiceCode(*s.config.catalog->calls());
  auto work=std::make_shared<State::Work>();work->frame=*shape;work->source=kBootUnjoinedSource;
  work->execution_only=true;work->execution_title=actual_process==X_PROCTYPE_TITLE;
  work->execution_title_producer=title_producer;
  work->execution_uncertain=!code;work->execution_code_proven=code;work->birth=birth;
  work->execution_producer=parent;
  work->thread=retain_object(thread);work->domain=s.config.domain->Acquire();
  work->caller_code.module=caller.module;work->caller_code.load_generation=caller.load_generation;
  // Caller identity has no future guest-code access; its temporary lease
  // drops after this entry's locks. Declared callback targets remain owned.
  work->first=uint32_t(c.r[3]);work->second=uint32_t(c.r[4]);
  if(avatar && avatar->Matches(c)){work->avatar_allocation=avatar;work->foreign_avatar_task=true;}
  const bool selected_target=kind==Kind::kSchedule || kind==Kind::kInline ||
      (at==0x816B9AA8 && (uint32_t(c.r[4])&1));
  if(code && selected_target){
   const uint64_t raw_target=at==0x816B9AA8?c.r[5]:c.r[3];
   if(DwordIs(raw_target,uint32_t(raw_target)))work->execution_target=s.RetainExecutionTarget(uint32_t(raw_target));
   if(!work->execution_target)work->execution_uncertain=true;
  }
  // The final native-memory/ledger admission is one cut. Match the return
  // path's GCRC -> State order: Word/QueryRangeAccess below also takes GCRC.
  // Earlier strong holders outlive both guards and unwind after their release.
  const auto admit=[&]() -> bool {
  auto memory_read=global_critical_region::AcquireDirect();
  std::lock_guard lock(s.mutex);
  // This is the definitive same-caller read cut. A newly present scope must
  // get the unchanged slow proof outside both guards before any admission.
  if(!code_checked && Tasks::DispatchCodeAuthenticationRequired(
      birth,c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire)))return false;
  if(s.active.size()>=256 || std::any_of(s.active.begin(),s.active.end(),[&](const auto& old){return old->frame==*shape;})){
   for(const auto& old:s.active)if(old->execution_only && old->frame==*shape){
    old->execution_uncertain=true;if(old->execution_lifetime)old->execution_lifetime->uncertain=true;
   }
   s.MarkNeutralUnavailable(*shape,c);return true;
  }
  work->serial=s.next_work++;
  if(!work->serial || !s.next_work){s.MarkNeutralUnavailable(*shape,c);return true;}
  s.active.push_back(work); // retain BEFORE any scalar birth/publication admission
  if(birth.generation)work->execution_lifetime=s.ExecutionLifetimeFor(birth);
  if(!work->domain)work->execution_uncertain=true;
  Tasks::NeutralObservation observation(s.tasks);
  bool exact=!work->execution_uncertain;
  if(exact && kind==Kind::kSchedule){
   exact=DwordIs(c.r[3],work->first) && DwordIs(c.r[4],work->second) &&
       DwordIs(c.r[5],uint32_t(c.r[5])) && DwordIs(c.r[6],uint32_t(c.r[6]));
   work->output=uint32_t(c.r[6]);
   if(exact)work->schedule=s.tasks.BeginExecutionSchedule(*shape,work->first,work->second,uint32_t(c.r[5]),work->output);
   exact=exact && work->schedule;
   if(exact && service_pump_code && DwordIs(c.r[5],shape->sp+0x50) &&
       DwordIs(c.r[6],0x81AEE470+0x3BC04) &&
       Word(*s.kernel->memory(),shape->sp+0x50)==8u &&
       Word(*s.kernel->memory(),shape->sp+0x54)==500u){
    work->service_pump_candidate=true;work->service_pump_object=0x81AEE470;
   }
  }else if(exact && kind==Kind::kCreate){
   work->output=work->first;exact=DwordIs(c.r[3],work->first);
   if(exact)work->schedule=s.tasks.BeginExecutionCreate(*shape,work->output);
   exact=exact && work->schedule;
  }else if(exact && kind==Kind::kInline){
   work->output=uint32_t(c.r[5]);
   exact=DwordIs(c.r[3],work->first) && DwordIs(c.r[4],work->second) &&
       DwordIs(c.r[5],work->output) && (!work->output || !(work->output&3));
  }else if(exact && kind==Kind::kAllocation){
   work->output=work->second;
   if(parent->schedule){work->schedule=parent->schedule;
    exact=s.tasks.EnterExecutionAllocation(work->schedule,*shape,work->first,work->output);
   }else{
    exact=parent->frame.function==0x816B9D68 &&
        Tasks::ClassifyAllocationProducer(parent->frame,*shape,parent->output,work->first,work->output)==Tasks::AllocationProducer::kInline;
    if(exact)work->detached_allocation=s.tasks.BeginDetachedAllocation(*shape,1,work->output,false,true,parent->first,parent->second);
    exact=exact && work->detached_allocation;
   }
   work->execution_target=parent->execution_target;
  }else if(exact && kind==Kind::kPublication){
   work->schedule=parent->schedule;work->output=uint32_t(c.r[5]);
   const auto* call=s.tasks.FindSchedule(work->schedule);const auto* node=call?s.tasks.FindBirthConst(call->birth):nullptr;
   exact=node && node->execution_only && DwordIs(c.r[3],work->first) &&
       DwordIs(c.r[4],work->second) && DwordIs(c.r[5],work->output) &&
       s.tasks.BeforeExecutionPublication(work->schedule,*shape,work->first,work->second,work->output,Word(*s.kernel->memory(),parent->output));
   if(node){work->birth=node->birth;work->execution_lifetime=s.ExecutionLifetimeFor(node->birth);}
  }else if(exact && kind==Kind::kDispatch){
   exact=birth.generation && work->execution_lifetime && DwordIs(c.r[at==0x816B7DE8?4:3],birth.task);
   if(exact)work->execution_dispatch=s.tasks.BeginExecutionDispatch(*shape,birth);
   exact=exact && work->execution_dispatch;
   if(at==0x816B8B08){const auto wait=s.Latest(shape->thread,0x816BA058);
    if(wait && wait->execution_only && wait->execution_control &&
       Tasks::SameBirth(wait->birth,birth) && shape->lr==0x816BA0DC &&
       wait->frame.sp>=0x80 && shape->sp==wait->frame.sp-0x80)work->execution_wait_parent=wait;
   }
  }else if(exact && kind==Kind::kControl){
   const auto fields=Tasks::DecodeModifyFields(work->second);
   exact=DwordIs(c.r[3],at==0x816B7B68?(birth.task|1u):birth.task) ||
       (at==0x816B7B68 && DwordIs(c.r[3],birth.task));
   if(at==0x816B9AA8){
    exact=exact && DwordIs(c.r[4],work->second) && (!fields.callback || DwordIs(c.r[5],uint32_t(c.r[5]))) &&
        (!fields.argument || DwordIs(c.r[6],uint32_t(c.r[6]))) &&
        (!fields.options || (DwordIs(c.r[7],uint32_t(c.r[7])) && c.r[7]>=0x1000 && !(c.r[7]&3)));
    if(exact && fields.callback){
     if(!work->execution_lifetime || !work->execution_target)exact=false;
     else {
      const auto& target=work->execution_target;
      const bool retained=std::any_of(work->execution_lifetime->targets.begin(),work->execution_lifetime->targets.end(),
          [&](const auto& old){return old && old->callback==target->callback &&
              old->code.module.get()==target->code.module.get() && old->code.load_generation==target->code.load_generation;});
      if(!retained)work->execution_lifetime->targets.push_back(target); // BEFORE original native mask1 store
      // A duplicate newly acquired lease stays in this Work until host reap;
      // never release its last resource reference under State/GCRC.
     }
    }
   }
   if(exact)exact=s.tasks.BeginExecutionControl(*shape,birth,at==0x816B9AA8?work->second:0,
       uint32_t(c.r[5]),uint32_t(c.r[6]));
   work->execution_control=exact;
   if(exact && at==0x816B7600){
    const auto tail=s.Latest(shape->thread,0x816B7B68);
    if(tail && tail->execution_only && tail->execution_control &&
        Tasks::SameBirth(tail->birth,birth) && tail->frame.root==shape->root &&
        tail->frame.thread==shape->thread && tail->frame.pcr==shape->pcr &&
        tail->frame.sp==shape->sp && tail->frame.lr==shape->lr &&
        DwordIs(c.r[3],birth.task) && DwordIs(c.r[4],0x815F8E38) && c.r[5]==15 && !c.r[6])
     work->tail_reschedule=tail; // genuine frameless original tail, not a new return
   }
  }else if(exact && kind==Kind::kClose){exact=s.tasks.EnterClose(*shape,work->first);
  }else if(exact && kind==Kind::kDelete){exact=s.tasks.EnterDeletingDestructor(*shape,work->first,uint32_t(c.r[4]));
  }else if(exact && kind==Kind::kFree){exact=s.tasks.EnterTaskFree(*shape,work->first);
  }
  if(!exact){work->execution_uncertain=true;s.execution_unavailable.store(true,std::memory_order_release);}
  if(kind==Kind::kDispatch){
   void* scope=c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire);
   if(scope){
    for(const auto& origin:s.active)if(origin.get()!=work.get() && origin->read.get()==scope &&
        origin->frame.thread==shape->thread && !origin->normal_return){
     if(work->execution_read.Suspend(origin->frame,*shape,origin,origin,scope,c.thread_state->native_profile_roster_read_))
      work->execution_read_owner=origin;
     break;
    }
    if(!work->execution_read.pending())s.SuspendAmbientOnUnavailable(c,shape->thread);
   }
  }
  return true;
  };
  if(!admit()){
   // No Work/Task/read mutation occurred in the first cut. Keep the original
   // full authentication when an ambient scope appeared during preparation.
   caller={};frame=s.ReadFrame(c,at,actual_process==X_PROCTYPE_TITLE,&caller,retained);
   code_checked=true;code=frame && CheckTaskExecutionCode(*s.config.catalog->calls());
   work->execution_uncertain=!code;work->execution_code_proven=code;
   work->caller_code.module=caller.module;work->caller_code.load_generation=caller.load_generation;
   (void)admit();
  }
  return true;
 }catch(...){
  if(handled && owner && shape){auto& s=*owner->state_;std::lock_guard lock(s.mutex);
   const auto found=std::find_if(s.active.begin(),s.active.end(),[&](const auto& active){return active->execution_only && active->frame==*shape;});
   if(found!=s.active.end()){
    (*found)->execution_uncertain=true;
    if((*found)->execution_lifetime)(*found)->execution_lifetime->uncertain=true;
    s.execution_unavailable.store(true,std::memory_order_release);
    if(shape->function==0x816B8B08 || shape->function==0x816B7DE8)s.SuspendAmbientOnUnavailable(c,shape->thread);
   }else s.MarkNeutralUnavailable(*shape,c);
  }
  return handled;
 }
}

bool NativeProfileIdentityOwner::ObserveExecutionReturn(void* raw,uint32_t at) noexcept {
 State::HookUse hook_use;
 if(!raw || (!CommonTaskFunction(at) && !ProfileCallback(at)))return false;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 std::shared_ptr<State::Work> work;
 try {
  owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);
  if(!owner)return false;auto& s=*owner->state_;
  const auto shape=s.TransportShape(c,at);if(!shape)return false;
  if(!hook_use.Acquire(s.hook_rundown,owner->state_,false))return false;
  PublishTaskAllocationCut allocation_cut{owner.get(),c,{}};
  {
   std::lock_guard lock(s.mutex);
   const bool unavailable_return=s.unavailable_execution_frames.Return(*shape);
   for(auto i=s.active.rbegin();i!=s.active.rend();++i)if((*i)->frame==*shape){
    if(!(*i)->execution_only)return false;work=*i;break;
   }
   if(!work)return unavailable_return;
   if(work->normal_return)return true; // duplicate observation is not another native receipt
  }
  State::CodeOwner caller;
  const auto frame=s.ReadFrame(c,at,work->execution_title,&caller,true);
  const bool code=frame && *frame==work->frame &&
      caller.module.get()==work->caller_code.module.get() &&
      caller.load_generation==work->caller_code.load_generation &&
      CheckTaskExecutionCode(*s.config.catalog->calls()) &&
      (!work->service_pump_candidate || CheckIdentityServiceCode(*s.config.catalog->calls()));
  // Real parent context, sampled only at an original restoration own-return.
  // It is never written or supplied by the host.
  const auto actual_task=(at==0x816BA058 || at==0x816B7DE8) && code?
      NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c,work->execution_title):std::optional<uint32_t>{};
  std::shared_ptr<State::ExecutionLifetime> prepared;
  const uint32_t value=uint32_t(c.r[3]);
  if(at==0x816B9A18 && code && int32_t(value)>=0){
   prepared=std::make_shared<State::ExecutionLifetime>();prepared->targets.reserve(1);
   if(work->execution_target)prepared->targets.push_back(work->execution_target);
  }
  auto read=global_critical_region::AcquireDirect();
  std::lock_guard lock(s.mutex);
  Tasks::NeutralObservation observation(s.tasks);
  bool exact=code && work->execution_code_proven && !work->execution_uncertain;
  if(exact && at==0x816B9A18){
   const auto output=int32_t(value)>=0?Word(*s.kernel->memory(),work->output):std::optional<uint32_t>{};
   const auto born=work->schedule?s.tasks.ReturnExecutionAllocation(work->schedule,*shape,value,output):
       work->detached_allocation?s.tasks.ReturnDetachedAllocation(work->detached_allocation,*shape,value,output):std::optional<Tasks::Birth>{};
   exact=int32_t(value)<0 || bool(born);
   if(born){
    if(work->avatar_allocation)if(auto* node=s.tasks.FindBirth(born->generation))node->foreign_avatar=true;
    exact=prepared && work->execution_producer &&
        (work->execution_target || work->execution_producer->frame.function==0x816BA668);
    if(exact){
     prepared->birth=*born;s.execution_lifetimes.push_back(prepared);work->execution_lifetime=prepared;work->birth=*born;
     work->execution_producer->birth=*born;work->execution_producer->execution_lifetime=prepared;
    }
   }
  }else if(exact && (at==0x816BA440 || at==0x816BA668)){
   const auto tagged=at==0x816BA668 && int32_t(value)>=0?Word(*s.kernel->memory(),work->output):std::optional<uint32_t>{};
   exact=s.tasks.ReturnExecutionProducer(work->schedule,*shape,value,tagged);
   if(exact && work->service_pump_candidate && int32_t(value)>=0){
    const auto* node=s.tasks.FindBirthConst(work->birth.generation);
    const auto* old=s.identity_service_pump?s.tasks.FindBirthConst(s.identity_service_pump->birth.generation):nullptr;
    if(node && Tasks::SameBirth(node->birth,work->birth) && node->birth.origin==1 && node->execution_only &&
       node->published && node->execution_publication && !node->freed && !node->free_entered &&
       !node->delete_entered && node->callback==0x818CEFD8 && node->argument==work->service_pump_object &&
       (!s.identity_service_pump || Tasks::SameBirth(s.identity_service_pump->birth,work->birth) || !old || old->freed))
     s.identity_service_pump=Tasks::IdentityServiceRequest::Pump{*shape,work->birth,work->service_pump_object};
   }
  }else if(exact && (at==0x816B8B08 || at==0x816B7DE8)){
   exact=s.tasks.ReturnExecutionDispatch(work->execution_dispatch,*shape,work->birth);
  }else if(exact && work->execution_control){
   exact=s.tasks.ReturnExecutionControl(*shape,work->birth);
   if(exact && work->tail_reschedule){
    const auto& tail=work->tail_reschedule;
    exact=tail->execution_only && tail->execution_control && !tail->normal_return &&
        s.tasks.ReturnExecutionControl(tail->frame,tail->birth);
    if(exact){
     tail->normal_return=true;
     const auto old=std::find(s.active.begin(),s.active.end(),tail);
     if(old!=s.active.end()){s.host_retired.push_back(tail);s.active.erase(old);}else exact=false;
    }
   }
  }else if(exact && at==0x816B7490){
   exact=s.tasks.ReturnClose(*shape,value);
   if(exact && !value && work->execution_lifetime)work->execution_lifetime->native_freed=true;
  }else if(exact && (at==0x816B7D98 || at==0x816B7500)){exact=s.tasks.ReturnDeletingDestructor(*shape);
  }else if(exact && at==0x816E6AB0){exact=s.tasks.ReturnTaskFree(*shape);}
  // Any read still current at a neutral dispatcher return belongs to an
  // independently entered bounded typed scope whose own-return is missing.
  // Suppress ONLY that exact retained owner before PPC resumes, even when no
  // older scope existed at dispatcher entry. Never fake its Leave/return.
  bool unexpected_read=false;
  if(at==0x816B8B08 || at==0x816B7DE8 || (at==0x816BA058 && work->execution_read.pending())){
   unexpected_read=c.thread_state && c.thread_state->native_profile_roster_read_.load(std::memory_order_acquire);
   s.SuspendAmbientOnUnavailable(c,shape->thread);
  }
  // A valid body own-return can restore context independently of whether a
  // neutral callback/argument observation was available. It cannot fabricate
  // a missing Task completion or immutable allocation receipt.
  if(work->execution_read.pending()){
   const auto origin=work->execution_read_owner;auto* cpu=work->thread?work->thread->thread_state():nullptr;
   bool restored=false;
   // An independently unreturned deeper scope must retire before the older
   // origin can become ambient again. Delegate both exact owners rather than
   // invert their genuine reverse-stop/restore-to-Leave order.
   if(origin && origin->read && cpu && code && work->execution_code_proven && !unexpected_read){
    if(at==0x816B8B08 && work->execution_wait_parent && !work->execution_wait_parent->normal_return){
     auto& wait=work->execution_wait_parent;
     restored=work->execution_read.TransferInlineWait(*shape,wait->frame,wait->execution_read);
     if(restored)wait->execution_read_owner=origin;
    }else if((at==0x816B7DE8 || at==0x816BA058) && actual_task &&
        origin->birth.generation && origin->birth.task==*actual_task){
     const auto live=s.tasks.ObservedTransportSubject(*actual_task);
     restored=live && Tasks::SameBirth(*live,origin->birth) &&
         work->execution_read.Restore(*shape,origin->read.get(),cpu->native_profile_roster_read_);
     if(restored)origin->execution_read_fallback=false; // exact cell is restored now
    }
   }
   if(!restored && origin && origin->read &&
       work->execution_read.DelegateToOrigin(origin.get(),origin->read.get())){
    // The exact originating scope remains in its actual active Work. Its
    // normal own-return restores only to Leave immediately, or actual-stop
    // reverse detachment settles it. No child obtains the old read ticket.
    origin->execution_read_fallback=true;origin->uncertain=true;
   }
  }
  if(!exact){work->execution_uncertain=true;s.execution_unavailable.store(true,std::memory_order_release);
   if(work->execution_lifetime)work->execution_lifetime->uncertain=true;
  }
  if(exact && at==0x816B9A18 && work->execution_producer)
   allocation_cut.value=AllocationReturnFact(*shape,work->execution_producer->frame,value,
       work->birth.generation?std::optional<Tasks::Birth>{work->birth}:std::nullopt);
  // Entry intent whose native field store may have occurred retains its code
  // holder until real final free or actual CPU-stop when its join is unknown.
  const auto* node=work->birth.generation?s.tasks.FindBirthConst(work->birth.generation):nullptr;
  const bool hold_target=work->execution_uncertain && work->execution_target &&
      (!node || !node->freed) && (at==0x816B9AA8 || at==0x816BA440 || at==0x816B9D68 || at==0x816B9A18);
  work->normal_return=true; // factual ORIGINAL body return, not callback/service success
  const auto found=std::find(s.active.begin(),s.active.end(),work);
  if(found!=s.active.end()){
   // Returned immutable resource holders are NOT active caller/source frames.
   // In particular a later Task may reuse this worker's identical SP/LR.
   if(hold_target || work->execution_read.pending())s.execution_retained.push_back(work);
   else s.host_retired.push_back(work);
   s.active.erase(found);
  }
  s.changed.notify_all();return true;
 }catch(...){
  if(owner){std::lock_guard lock(owner->state_->mutex);
   owner->state_->execution_unavailable.store(true,std::memory_order_release);
   if(work){
    work->execution_uncertain=true;work->normal_return=true;
    if(work->execution_lifetime)work->execution_lifetime->uncertain=true;
    if(at==0x816B8B08 || at==0x816B7DE8 || (at==0x816BA058 && work->execution_read.pending()))
     owner->state_->SuspendAmbientOnUnavailable(c,work->frame.thread);
    const auto origin=work->execution_read_owner;
    if(work->execution_read.pending() && origin && origin->read &&
        work->execution_read.DelegateToOrigin(origin.get(),origin->read.get())){
     origin->execution_read_fallback=true;origin->uncertain=true;
    }
    // Keep this returned holder in active on allocation/setup failure rather
    // than risk a last-owner release. Latest excludes its factual own-return;
    // a later worker reusing the same SP/LR cannot borrow it as a caller.
   }
  }
  return bool(work);
 }
}

// This native context owns a separate real B7F30 AddRef until BA42C.
// It issues only a nonfinal-reference baseline, never a Birth/read/Task result.
void NativeProfileIdentityOwner::ObserveContextReference(void* raw,uint32_t event) noexcept {
 State::HookUse hook_use;
 if(!raw || event>11)return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 std::shared_ptr<State::ContextReference> record;
 std::shared_ptr<State::ExecutionTarget> target;
 try{
  owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);if(!owner)return;
  auto& s=*owner->state_;auto* thread=ActualThread();
  if(!thread || s.retired.load(std::memory_order_acquire) || thread->kernel_state()!=s.kernel ||
     thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM)return;
  if(!hook_use.Acquire(s.hook_rundown,owner->state_,false))return;
  const uint32_t function=event==1 || event==2 || event==11?0x816B7F30:
      event==3 || event==4 || event==10?0x816B7490:0x816BA3C0;
  if(event!=0){
   {std::lock_guard lock(s.mutex);record=s.CurrentContextReference(*thread);}
   if(!record)return;
   if((event==1 || event==2 || event==11) && c.lr!=0x816BA3D4)return;
   if((event==3 || event==4 || event==10) && c.lr!=0x816BA430)return;
  }
  const auto frame=s.TransportShape(c,function);
  if(!frame || !CheckContextReferenceCode(*s.config.catalog->calls())){
   std::lock_guard lock(s.mutex);s.context_reference_unavailable=true;return;
  }
  if(event==0){
   record=std::make_shared<State::ContextReference>();
   record->thread=retain_object(thread);record->cpu=c.thread_state;record->context=&c;
   record->code=s.root_code;record->domain=s.config.domain->Acquire();
   std::lock_guard lock(s.mutex);
   const auto slot=std::find(s.context_references.begin(),s.context_references.end(),nullptr);
   if(s.retired.load() || slot==s.context_references.end() || !record->code || !record->domain ||
      !s.next_context_reference || s.next_context_reference==UINT64_MAX ||
      !record->receipt.Prepare(s.next_context_reference++,*frame,uint32_t(c.r[3]),thread->stack_limit(),thread->stack_base()) ||
      !DwordIs(c.r[3],record->receipt.context)){
    s.context_reference_unavailable=true;return;
   }
   *slot=record;return;
  }
  if(event==6 || event==8){
   if(DwordIs(c.r[11],uint32_t(c.r[11])))target=s.RetainExecutionTarget(uint32_t(c.r[11]));
  }
  std::lock_guard lock(s.mutex);
  if(s.CurrentContextReference(*thread)!=record || record->cpu!=c.thread_state || record->context!=&c)return;
  auto& receipt=record->receipt;
  if(event==1 || event==2){
   // Unrelated nested getters cannot complete this exact native call.
   if(frame->lr==0x816BA3D4)receipt.Getter(*frame,event==2,c.r[3]);
  }else if(event==3 || event==4){
   if(frame->lr==0x816BA430)receipt.Decrement(*frame,event==4,c.r[3]);
  }else if(event==10){
   if(frame->lr==0x816BA430)receipt.AdmitDecrement(*frame);
  }else if(event==11){
   if(frame->lr==0x816BA3D4)receipt.AdmitGetter(*frame);
  }else if(event==5){receipt.Return(*frame);
  }else{
   const uint32_t site=event-6;
   if((site==0 || site==2) && !target){receipt.Fail();return;}
   if(!DwordIs(c.r[29],receipt.task) || !DwordIs(c.r[31],receipt.context)){
    receipt.Fail();return;
   }
   if(receipt.Callback(site,*frame,uint32_t(c.r[29]),uint32_t(c.r[31]),
       target?target->callback:0,c.r[3]) && target)record->targets[site/2]=target;
  }
 }catch(...){if(owner){std::lock_guard lock(owner->state_->mutex);owner->state_->context_reference_unavailable=true;}}
}

// These functional cuts observe registers the original native code already
// loaded. Neutral observer uncertainty is LOCAL: original PPC still executes,
// no HRESULT/Task completion is synthesized, and no data capability is issued.
void NativeProfileIdentityOwner::ObserveTaskExecutionInstruction(void* raw,uint32_t site) noexcept {
 State::HookUse hook_use;
 ClearTaskAllocationCut();
 if(!raw || site>=kTaskExecutionSites.size())return;
 if(site>=6){ObserveContextReference(raw,site);return;}
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 std::shared_ptr<State::Work> work;
 std::shared_ptr<NativeProfileIdentityOwner> owner;
 try {
  owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);if(!owner)return;auto& s=*owner->state_;
  if(s.retired.load(std::memory_order_acquire))return;
  if(!hook_use.Acquire(s.hook_rundown,owner->state_,false))return;
  auto* thread=ActualThread();
  if(!thread || thread->kernel_state()!=s.kernel || c.processor!=s.kernel->processor() ||
     c.thread_state!=thread->thread_state() || c.thread_state->context()!=&c ||
     !DwordIs(c.r[13],thread->pcr_ptr()) || !DwordIs(c.r[1],uint32_t(c.r[1])) ||
     !DwordIs(c.lr,uint32_t(c.lr)))return;
  const auto function=kTaskExecutionSites[site].function;
  std::shared_ptr<State::ExecutionTarget> actual_target;
  std::optional<Tasks::ExecutionInstructionParticipation> participation;
  {
   std::lock_guard lock(s.mutex);
   work=s.Latest(thread->thread_id(),function);
   participation=Tasks::ExecutionInstructionParticipation::Capture(work,thread->pcr_ptr());
   if(!participation)return; // No host Task/read effect can follow this cut.
   if((site==2 || site==4) && c.r[11]==uint64_t(uint32_t(c.r[11])) &&
       c.r[3]==uint64_t(uint32_t(c.r[3])))
    actual_target=s.ExecutionTargetFor(work->execution_lifetime,uint32_t(c.r[11]));
  }
  bool target_current=false;
  if(site==2 || site==4){
   if(actual_target){
    auto read=global_critical_region::AcquireDirect();xex2_section_type section;
    const auto& code=actual_target->code;
    target_current=code.module && code.module->xex_module() && code.lease && code.load_generation &&
        code.module->load_generation()==code.load_generation &&
        s.kernel->GetUserModuleForAddress(actual_target->callback).get()==code.module.get() &&
        s.config.domain->Contains({code.module->hmodule_ptr(),code.load_generation}) &&
        code.module->xex_module()->GetPageSectionType(actual_target->callback,&section) && section==XEX_SECTION_CODE &&
        code.module->xex_module()->TranslateCode(actual_target->callback)==
            s.kernel->memory()->TranslateVirtual<const uint8_t*>(actual_target->callback);
   }
  }
  // Revalidate the real producer/dispatch cut as well as the selected target.
  // Load generation alone does not prove unchanged code/bridge translation.
  const bool current_code=CheckTaskExecutionCode(*s.config.catalog->calls());
  std::lock_guard lock(s.mutex);
  const auto latest=s.Latest(thread->thread_id(),function);
  if(!participation->Matches(latest,thread->pcr_ptr()))return;
  work=latest;
  if(!current_code){work->execution_uncertain=true;
   if(work->execution_lifetime)work->execution_lifetime->uncertain=true;return;}
  Frame body{s.root,thread->thread_id(),thread->pcr_ptr(),uint32_t(c.r[1]),uint32_t(c.lr),function};
  bool exact=false;
  if(site==0){
   exact=DwordIs(c.r[31],work->birth.task) && DwordIs(c.r[30],work->second) &&
       DwordIs(c.r[29],work->output) && DwordIs(c.r[10],uint32_t(c.r[10])) &&
       s.tasks.ObserveExecutionPublication(work->frame,work->birth,body,
           uint32_t(c.r[31]),uint32_t(c.r[30]),uint32_t(c.r[29]),uint32_t(c.r[10]));
  }else if(site==1){
   exact=DwordIs(c.r[30],work->birth.task) && DwordIs(c.r[28],work->first) &&
       DwordIs(c.r[27],work->second) &&
       s.tasks.ObserveInlineExecutionPublication(work->frame,work->birth,body,
           uint32_t(c.r[30]),uint32_t(c.r[28]),uint32_t(c.r[27]));
  }else if(site==2 || site==4){
   // Both original lwz operands are zero-extended DWORDs. Do not normalize
   // arbitrary upper bits or resample concurrent Task+30/+34 stores.
   auto target=target_current?actual_target:nullptr;
   exact=target && DwordIs(c.r[31],work->birth.task) &&
       s.tasks.ExecutionCallbackSelected(work->execution_dispatch,body,
           uint32_t(c.r[31]),uint32_t(c.r[11]),uint32_t(c.r[3]));
   if(exact)work->execution_target=std::move(target);
  }else{
   exact=DwordIs(c.r[31],work->birth.task) && work->execution_target &&
       s.tasks.ExecutionCallbackReturned(work->execution_dispatch,body,
           uint32_t(c.r[31]),uint32_t(c.r[3]));
  }
  if(!exact){work->execution_uncertain=true;
   if(work->execution_lifetime)work->execution_lifetime->uncertain=true;}
 }catch(...){
  // No success/backfilled receipt, no global profile poison, no native skip.
  // The enclosing retained Work/resource holder survives until real cleanup.
  if(owner && work){std::lock_guard lock(owner->state_->mutex);
   work->execution_uncertain=true;
   if(work->execution_lifetime)work->execution_lifetime->uncertain=true;
  }
 }
}
std::optional<NativeProfileIdentityOwner::TaskExecutionObservation>
NativeProfileIdentityOwner::CurrentTaskExecution(void* raw,uint32_t actual_task) noexcept try {
 if(!raw || !actual_task)return {};
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);
 if(!owner)return {};auto& s=*owner->state_;auto* thread=ActualThread();
 if(!thread || thread->kernel_state()!=s.kernel || c.processor!=s.kernel->processor() ||
    c.thread_state!=thread->thread_state() || c.thread_state->context()!=&c ||
    !DwordIs(c.r[13],thread->pcr_ptr()) || !DwordIs(c.r[1],uint32_t(c.r[1])))return {};
 std::lock_guard lock(s.mutex);
 // Only the innermost original dispatcher decides. Never skip an unrelated
 // nested Task and borrow an older equal callback or a current roster.
 for(auto i=s.active.rbegin();i!=s.active.rend();++i){const auto& work=*i;
  if(!work || work->normal_return || work->frame.thread!=thread->thread_id() ||
      (work->frame.function!=0x816B8B08 && work->frame.function!=0x816B7DE8))continue;
  // An actually entered younger dispatcher shadows this frame even when its
  // observer could not allocate/issue a Work or invocation serial. A stale
  // uncertain marker may refuse a fact, but can never lend an older one.
  if(s.unavailable_execution_frames.Shadows(s.root,thread->thread_id(),thread->pcr_ptr(),
      uint32_t(c.r[1]),work->frame.sp))return {};
  if(!work->execution_only || !work->execution_dispatch || work->execution_uncertain ||
     work->birth.task!=actual_task || !work->execution_lifetime ||
     work->execution_lifetime->uncertain || !work->execution_target)return {};
  const auto fact=s.tasks.CaptureCurrentExecution(work->execution_dispatch,
      thread->thread_id(),thread->pcr_ptr(),uint32_t(c.r[1]),actual_task);
  const auto& target=work->execution_target;
  if(!fact || !Tasks::SameBirth(fact->birth,work->birth) || fact->dispatcher!=work->frame ||
     fact->callback!=target->callback || !target->code.module || !target->code.lease ||
     target->code.module->load_generation()!=target->code.load_generation)return {};
  return TaskExecutionObservation{fact->root.kernel,fact->root.domain,fact->root.generation,
      fact->birth.generation,fact->invocation,target->code.load_generation,
      fact->birth.task,fact->dispatcher.thread,fact->dispatcher.pcr,fact->dispatcher.sp,fact->body_sp,
      fact->callback,fact->argument,target->code.module->hmodule_ptr(),fact->callback_lr};
 }
 return {};
}catch(...){return {};}

std::optional<NativeProfileIdentityOwner::TaskAllocationReturnObservation>
NativeProfileIdentityOwner::TakeTaskAllocationReturnAtNativeCut(void* raw) noexcept {
 const auto cut=task_allocation_cut;ClearTaskAllocationCut(); // consume even on refusal
 if(!cut.available || raw!=cut.context || !raw)return {};
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto* thread=ActualThread();
 if(!thread || !c.kernel_state || c.thread_state!=cut.cpu || !cut.cpu ||
    cpu::ThreadState::Get()!=cut.cpu || thread->thread_state()!=cut.cpu ||
    cut.cpu->context()!=&c || thread->kernel_state()!=c.kernel_state ||
    thread->thread_id()!=cut.fact.allocator.thread || thread->pcr_ptr()!=cut.fact.allocator.pcr ||
    !DwordIs(c.r[1],cut.fact.allocator.sp) || !DwordIs(c.lr,cut.fact.allocator.lr) ||
    !DwordIs(c.r[13],cut.fact.allocator.pcr) || uint32_t(c.r[3])!=cut.fact.native_status ||
    cut.fact.allocator.function!=0x816B9A18)return {};
 const auto owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);
 if(!owner || owner.get()!=cut.owner || owner->state_->root!=Tasks::Root{
     cut.fact.root.kernel,cut.fact.root.domain,cut.fact.root.generation})return {};
 return cut.fact;
}

void NativeProfileIdentityOwner::ObserveTaskDiagnosticInstruction(void* raw,uint32_t site) noexcept {
 if(!raw || site>=kTaskDiagnosticSites.size())return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto* actual_cpu=c.thread_state;if(!actual_cpu)return;
 auto* d=static_cast<State::TaskDiagnostic*>(actual_cpu->native_identity_task_diagnostic_cell_.load(std::memory_order_acquire));
 if(!d)return;
 // Validate immutable binding before touching the same-ThreadState payload.
 if(d->context!=&c || d->cpu!=actual_cpu || d->kernel!=c.kernel_state ||
    cpu::ThreadState::Get()!=actual_cpu || !DwordIs(c.r[13],d->value.pcr) || !d->owner)return;
 auto& v=d->value;const auto at=kTaskDiagnosticSites[site].address;
 const uint64_t depth=site<6?0:site<10?0x110u:0x1E0u;
 if(v.body_sp<depth || !DwordIs(c.r[1],uint32_t(v.body_sp-depth)))return;
 if((site<2 || site==4) && v.origin!=0x8199E3E0)return;
 if((site==2 || site==3 || site==5) && v.origin!=0x8199E148)return;
 v.site=at;v.sp=c.r[1];v.lr=c.lr;
 if(site==0 || site==2){
  v.provider_object=c.r[3];v.provider_function=c.r[11];v.provider_ctr=c.ctr;v.available|=8;
 }else if(site==1 || site==3){
  v.provider_return=c.r[3];v.available|=16;
  if(!(v.available&8))v.issues|=1;
 }else if(site==4 || site==5){
  v.producer_object=site==4?c.r[10]:c.r[28];v.producer_word=c.r[11];v.producer_options=c.r[6];v.available|=32;
  if(!(v.available&16) || uint32_t(v.producer_object)!=uint32_t(v.provider_return))v.issues|=2;
 }else if(site==6){
  v.allocation_options=c.r[31];v.allocation_task=c.r[11];v.supplied_flags=c.r[10];v.available|=64;
 }else if(site==7){
  v.supplied_object=c.r[10];v.available|=128;
  if(!(v.available&64) || c.r[31]!=v.allocation_options || c.r[11]!=v.allocation_task)v.issues|=4;
 }else if(site==8){
  v.native_task=c.r[31];v.native_flags=c.r[10];v.raw_object=c.r[11];v.available|=512;
  if(!(v.available&128) || uint32_t(v.native_task)!=uint32_t(v.allocation_task))v.issues|=8;
 }else if(site==9){
  v.masked_object=c.r[30];v.operand=c.r[11];v.available|=1024;
  if(!(v.available&512) || uint32_t(c.r[31])!=uint32_t(v.native_task))v.issues|=16;
  d->owner->first_task_operands.Record(v);
  if(uint32_t(v.masked_object)==0 && uint32_t(v.operand)==4)
   d->owner->first_null_task_operands.Record(v);
 }else if(site==10){v.native_process_first=c.r[3];v.available|=2048;}
 else {v.native_process_second=c.r[3];v.available|=4096;}
}

void NativeProfileIdentityOwner::BeforeNativeListenerUnlock(void* raw) noexcept {
 if(!raw)return;auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 auto* cell=c.thread_state?static_cast<Listeners::FastCell*>(
     c.thread_state->native_identity_listener_cell_.load(std::memory_order_acquire)):nullptr;
 if(cell)Listeners::FastBeforeUnlock(cell,ListenerRegisters(c));
}

void NativeProfileIdentityOwner::BeforeCompletionPublication(void* raw) noexcept {
 if(!raw || CoversIdentityServiceBoundary(raw,0x816A2630,false))return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 try{
  const auto owner=Lookup(c.kernel_state);if(!owner)return;auto& s=*owner->state_;
  const auto frame=s.ReadFrame(c,0x816A2588);
  const auto task=NativeProfileIdentityTaskContext::Read(
      *s.config.catalog->calls(),*s.config.domain,0x816A2588,c);
  if(!frame)return;
  std::shared_ptr<const NativeServiceBackingLease> retired_overlap;
  {
   std::lock_guard lock(s.mutex);const auto job=s.job;
   const auto whole=s.Latest(frame->thread,0x816A2588);
   const auto* node=task?s.tasks.FindLiveTask(*task):nullptr;
   if(!job || !whole || whole->transport_unclaimed || !whole->completion_route ||
      !whole->completion_route->StillOwned(job,job->serial,job->source,job->overlap,whole->first) ||
      !job->overlap || whole->source!=job->source ||
      !job->completion_frame)return; // Unrelated actual app completion.
   const auto& entry=*job->completion_frame;
   if(s.uncertain.load() || job->caller_published || !job->overlap_backing ||
      (job->initializer_completion_owner ?
       whole->initializer_completion_owner!=job->initializer_completion_owner ||
       job->initializer_completion_owner->normal_return ||
       s.Latest(entry.thread,0x8199E3E0)!=job->initializer_completion_owner :
       !node || node->birth.generation!=job->completion_birth.generation ||
       node->birth.task!=job->completion_birth.task || node->birth.origin!=job->source) ||
      entry.thread!=frame->thread || entry.pcr!=frame->pcr || entry.sp<0xA0 ||
      entry.sp-0xA0!=frame->sp || uint32_t(c.r[30])!=job->overlap ||
      uint32_t(c.r[31])!=job->completion_context || uint32_t(c.r[28])!=job->completion_error ||
      uint32_t(c.r[27])!=job->completion_extended || uint32_t(c.r[26])!=job->completion_length){s.Lose(__LINE__);return;}
   job->caller_published=true;retired_overlap=std::move(job->overlap_backing);
   {OriginalLogonDiagnostic d;d.stage=OriginalLogonDiagnostic::kOverlapBeforeStatus;
    d.completion_error_available=1;d.completion_error=job->completion_error;
    d.completion_extended=job->completion_extended;d.completion_length=job->completion_length;
    xam::ProfileManager::RecordNativeOriginalLogon(job->original_logon_observation,d);}
  }
  // Exact original Status store follows this HIR barrier. The saved364B body
  // has no later caller-overlap access; event/polling callers can reuse their
  // completed backing. Native request, Task, callback code/domain and private
  // cleanup remain retained independently. No APC delivery is asserted.
  retired_overlap.reset();
 }catch(...){if(const auto owner=Lookup(c.kernel_state))owner->state_->Lose(__LINE__);}
}

std::shared_ptr<NativeIdentityTileWork> NativeProfileIdentityOwner::CaptureTileWork(
    KernelState* kernel,uint32_t type,uint32_t title,uint64_t item,uint32_t user,
    uint32_t output,uint32_t size_address,uint32_t capacity,std::error_code& error) {
 error.clear();const auto owner=Lookup(kernel);if(!owner)return {};
 auto* actual=ActualThread();if(!actual || actual->kernel_state()!=kernel)return {};
 auto& s=*owner->state_;std::shared_ptr<NativeIdentityTileWork::Data> tile;
 {
  std::lock_guard lock(s.mutex);
  const auto work=s.Latest(actual->thread_id(),0x81670B68);
  if(!work)return {}; // Ordinary shared stock/profile tile use is unchanged.
  if((s.stopping.load() && (!s.job || work->source!=s.job->source)) ||
     s.retired.load() || s.uncertain.load() || !work->tile ||
     title || item || work->tile->type!=type || work->tile->user!=user ||
     work->tile->output!=output || work->tile->size_address!=size_address ||
     work->tile->capacity!=capacity){
   error=std::make_error_code(std::errc::operation_canceled);return {};
  }
  tile=work->tile;
 }
 return std::shared_ptr<NativeIdentityTileWork>(new NativeIdentityTileWork(std::move(tile)));
}

NativeProfileIdentityOwner::FrameQueryAdmission
NativeProfileIdentityOwner::AdmitNativeFrameIteration(XThread& thread,uint32_t lr,
                                                      bool source_retired) noexcept {
 FrameQueryAdmission result;
 if(lr!=0x816CD70C)return result; // Nested real clear Polls must keep progressing.
 const auto owner=Lookup(thread.kernel_state());
 if(!owner)return result;
 result.managed=true;auto& s=*owner->state_;
 State::QueryObservation observation;observation.enabled=cvars::native_system_ui_trace;
 observation.source_retired=source_retired;
 struct Reporter {State& state;State::QueryObservation& value;
  ~Reporter(){state.ReportPreparedQuery(value);state.ReportQuery(value,true);}} reporter{s,observation};
 try{
  if(observation.Failed(1,s.retired.load(std::memory_order_acquire)) ||
     observation.Failed(2,global_critical_region::is_held_by_current_thread()) ||
     observation.Failed(3,observation.Actual(ActualThread())!=&thread))return result;
  auto& c=*thread.thread_state()->context();
  if(observation.enabled){observation.sp=c.r[1];observation.lr=c.lr;observation.pcr=c.r[13];
   observation.context_thread_state=reinterpret_cast<uintptr_t>(c.thread_state);}
  const auto frame=s.ReadFrame(c,0x816CD6E8);
  const auto task=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,0x816CD6E8,c);
  if(observation.enabled){if(frame)observation.frame=*frame;if(task)observation.task=*task;}
  if(observation.Failed(4,!frame) || observation.Failed(5,frame->lr!=lr) ||
     observation.Failed(6,!task))return result;
  // Poll has already performed genuine Source retirement. Waiting here parks
  // the actual manager before its resource load, outside owner/native locks.
  // Query==0 iterations also borrow a source: the native state body after
  // CD71C still runs and reaches the same authenticated CD920 boundary.
  for(;;){
   std::shared_ptr<State::Work> work;
   {
    std::lock_guard lock(s.mutex);
    const bool captured=s.job && (s.job->native_entered || s.job->entering || s.job->transport_running);
    if(observation.enabled && !observation.observation){
     if(s.query_observation==UINT64_MAX)observation.enabled=false;
     else observation.observation=++s.query_observation;
    }
    if(observation.enabled){
     if(observation.loops!=UINT32_MAX)++observation.loops;
     observation.phase=uint32_t(s.render_phase);observation.active=uint32_t(s.active.size());
     observation.expected=uint32_t(s.expected.size());observation.current_read=s.current_read_source;
     observation.next_work=s.next_work;observation.flags=uint32_t(captured);
     observation.old_evaluated=observation.old_truth=0;observation.work_blocker={};observation.source_blocker={};
     observation.prepared_evaluated=observation.prepared_truth=0;
     observation.prepared_work={};observation.prepared_source={};observation.prepared_origin={};
     observation.prepared_pending_available=false;observation.prepared_pending=0;
     observation.first_loss=s.first_loss_line.load(std::memory_order_relaxed);
     observation.job=observation.source=0;observation.attempt=0;observation.paint={};
     if(s.job){observation.job=s.job->serial;observation.source=s.job->source;observation.attempt=s.job->preentry_attempt;
      for(uint32_t i=0;i<PaintObservation::kCount;++i)
       observation.paint[i]=s.job->preentry_observation.Snapshot(PaintStage(i),observation.attempt);}
    }
    if(observation.Failed(7,s.retired.load()) || observation.Failed(8,s.uncertain.load()) ||
       observation.Failed(9,s.stopping.load() && !captured))return result;
    const auto* node=s.tasks.FindLiveTask(*task);
    if(!node){observation.reason=10;s.Lose(__LINE__);return result;}
    if(observation.enabled)observation.birth=node->birth;
    if(observation.Old(0,s.render_phase==State::RenderPhase::kDrainingOld) && observation.Old(1,source_retired) &&
       observation.Old(2,bool(s.job)) && observation.Old(3,bool(s.wake)) && observation.Old(4,bool(s.native_pending)) &&
       observation.Old(5,s.IdentityWorkEmpty(observation.enabled?&observation.work_blocker:nullptr)) &&
       observation.Old(6,s.expected.empty()) && observation.Old(7,s.tasks.SourceDescendantsClosed(s.current_read_source,observation.enabled?&observation.source_blocker:nullptr)) &&
       observation.Old(8,bool(s.job->old_event_origin)) && observation.Old(9,s.listeners->SourceOnlyProducer(*s.job->old_event_origin)) &&
       observation.Old(10,s.wake->InvocationsDrained(NativeAtomicWord(s.native_pending)))){
      s.render_phase=State::RenderPhase::kClosingOld;
    }
    const auto prepared_drained=[&](){
     // No added guest read: retain the single original last-cut pending load.
     const auto pending=NativeAtomicWord(s.native_pending);
     if(observation.enabled){observation.prepared_pending_available=true;observation.prepared_pending=pending;}
     return s.wake->InvocationsDrained(pending);
    };
    if(observation.Prepared(0,s.render_phase==State::RenderPhase::kHiddenRefresh) && observation.Prepared(1,source_retired) &&
       observation.Prepared(2,bool(s.job)) && observation.Prepared(3,s.job->want_final_barrier) && observation.Prepared(4,s.job->render_refresh_observed) &&
       observation.Prepared(5,s.ProvidersFresh(s.job->source)) &&
       observation.Prepared(6,s.IdentityWorkEmpty(observation.enabled?&observation.prepared_work:nullptr)) && observation.Prepared(7,s.expected.empty()) &&
       observation.Prepared(8,bool(s.job->event_origin)) && observation.Prepared(9,s.listeners->SourceProducerQuiescent(*s.job->event_origin,
           observation.enabled?&observation.prepared_origin:nullptr)) &&
       observation.Prepared(10,s.tasks.SourceDescendantsClosed(s.job->source,observation.enabled?&observation.prepared_source:nullptr)) &&
       observation.Prepared(11,bool(s.wake)) && observation.Prepared(12,bool(s.native_pending)) && observation.Prepared(13,prepared_drained())){
      s.render_phase=State::RenderPhase::kClosingPrepared;
    }
    if(s.render_phase==State::RenderPhase::kClosingStop && source_retired &&
       s.job && s.job->native_returned && !s.job->transport_running &&
       s.IdentityWorkEmpty() && s.expected.empty() &&
       s.tasks.SourceDescendantsClosed(s.job->source) && s.wake && s.native_pending &&
       s.wake->InvocationsDrained(NativeAtomicWord(s.native_pending))){
      s.render_phase=State::RenderPhase::kStopPaused;
      s.render_query_parked=true;s.parked_render_thread=retain_object(&thread);
      s.parked_render_pcr=frame->pcr;s.parked_render_sp=frame->sp;
      s.parked_render_birth=node->birth.generation;
      observation.parked=true;observation.phase=uint32_t(s.render_phase);observation.reason=11;
      return result; // Real unavailable Query; no new iteration/source is admitted.
    }
    if(s.render_phase==State::RenderPhase::kStopPaused){observation.reason=11;return result;}
    const bool closing=s.render_phase==State::RenderPhase::kClosingOld ||
        s.render_phase==State::RenderPhase::kOldPaused ||
        s.render_phase==State::RenderPhase::kClosingPrepared ||
        s.render_phase==State::RenderPhase::kPreparedPaused;
    const bool iterations=std::any_of(s.active.begin(),s.active.end(),
        [](const auto& item){return item->render_iteration;});
    if(closing && source_retired && !iterations){
     s.render_query_parked=true;s.parked_render_thread=retain_object(&thread);
     s.parked_render_pcr=frame->pcr;s.parked_render_sp=frame->sp;
     s.parked_render_birth=node->birth.generation;
     observation.parked=true;
     if(s.render_phase==State::RenderPhase::kClosingOld)s.render_phase=State::RenderPhase::kOldPaused;
     if(s.render_phase==State::RenderPhase::kClosingPrepared)s.render_phase=State::RenderPhase::kPreparedPaused;
     if(observation.enabled)observation.phase=uint32_t(s.render_phase);
    }else if(!closing || !source_retired){
     if(observation.Failed(12,iterations) || observation.Failed(13,s.active.size()>=256) ||
        observation.Failed(14,!s.next_work || s.next_work==UINT64_MAX)){s.Lose(__LINE__);return result;}
     work=std::make_shared<State::Work>();work->frame=*frame;work->serial=s.next_work++;
     work->source=s.pixels->source();work->presentation_guard=s.pixels;
     work->render_iteration=true;work->thread=retain_object(&thread);
     work->domain=s.config.domain->Acquire();
     const auto birth=s.tasks.BeginExternalWork(*frame,*task);
     if(observation.Failed(15,!birth) || observation.Failed(16,!work->domain)){s.Lose(__LINE__);return result;}
     work->birth=work->external_birth=*birth;
     auto lifetime=std::make_shared<State::EventLifetime>();
     lifetime->code=s.root_code;lifetime->domain=s.config.domain->Acquire();
     lifetime->thread=work->thread;if(s.job)lifetime->intent=s.job->intent;
     if(!lifetime->domain){observation.reason=17;s.Lose(__LINE__);return result;}
     const Events::Root eroot{s.root.kernel,s.root.domain,s.root.generation};
     const Events::WorkKey key{eroot,birth->generation,work->serial,frame->thread,
         frame->pcr,birth->task,frame->sp,frame->lr,frame->function};
     work->event_lifetime=lifetime;
     work->listener_source=std::shared_ptr<const NativeIdentityEventWorkSource>(
         new NativeIdentityEventWorkSource(key,s.listeners->RetainEarlierSource(work->source),lifetime));
     work->listener_work=s.listeners->AdoptWholeWork(work->listener_source);
     if(!work->listener_work){observation.reason=18;s.Lose(__LINE__);return result;}
     auto* pool=&s.boot_reads;
     if(s.job)pool=work->source==s.job->source?&s.job->writer_reads:&s.job->old_reads;
     const auto slot=std::find_if(pool->begin(),pool->end(),[](const auto& p){return bool(p);});
     if(slot==pool->end()){observation.reason=19;s.Lose(__LINE__);return result;}
     work->read_pool=pool;work->read=std::move(*slot);
     s.active.push_back(work); // Retain before actual ThreadState attachment.
     if(!work->read->Enter(s.kernel,&thread,c.thread_state,birth->task,birth->generation,
         work->serial,s.config.catalog->calls()->module()->hmodule_ptr(),s.root.generation)){
      observation.reason=20;work->uncertain=true;s.Lose(__LINE__);return result;
     }
     s.render_query_parked=false;
     observation.reason=24;result.guard=work->presentation_guard;result.admitted=true;return result;
    }
   }
   // A stop request releases this pause without claiming iteration, clear,
   // GPU completion, or restored pixels. Existing admitted work stays owned.
   s.ReportPreparedQuery(observation);
   s.ReportQuery(observation,false); // independently live while Bootstrap may be blocked
   thread.Delay(0,0,uint64_t(-10000ll));
   if(observation.Failed(21,observation.Actual(ActualThread())!=&thread))return result;
   // This getter was the original RHS of the short-circuited comparison.
   // It is still evaluated only after the original ActualThread check passes.
   auto* expected_thread_state=thread.thread_state();
   auto* context_thread_state=c.thread_state;
   if(observation.enabled){
    observation.expected_thread_state=reinterpret_cast<uintptr_t>(expected_thread_state);
    observation.context_thread_state=reinterpret_cast<uintptr_t>(context_thread_state);
   }
   if(observation.Failed(22,context_thread_state!=expected_thread_state))return result;
  }
 }catch(...){observation.reason=23;s.Lose(__LINE__);return result;}
}
NativeProfileIdentityOwner::FrameQueryAdmission
NativeProfileIdentityOwner::CaptureNativeFrameGuard(XThread& thread) noexcept {
 FrameQueryAdmission result;
 const auto owner=Lookup(thread.kernel_state());if(!owner)return result;
 result.managed=true;auto& s=*owner->state_;
 try{
  std::lock_guard lock(s.mutex);
  if(ActualThread()!=&thread || s.retired.load() || s.uncertain.load())return result;
  for(auto i=s.active.rbegin();i!=s.active.rend();++i){const auto& work=*i;
   if(work->render_iteration && work->thread.get()==&thread &&
      work->frame.pcr==thread.pcr_ptr() && !work->normal_return && !work->uncertain){
    if(work->listener_work){
     const auto facts=s.listeners->CaptureDependencies(*work->listener_work);
     if(!facts || facts->unknown)s.Lose(__LINE__); // Suppress every image before Set.
    }
    result.guard=work->presentation_guard;result.admitted=bool(result.guard);
    // Optional observation allocation cannot reject an otherwise admitted
    // native frame. The base visibility guard and execution remain unchanged.
    try{
     const bool empty_initial=s.EmptyInitialFrameCurrent(result.guard,work->source);
     const bool image_observed=!work->image_draws_unavailable && s.image_slot && !s.image_slot->unavailable_.load() &&
        std::any_of(work->image_draws.begin(),work->image_draws.end(),[](const auto& d){return bool(d);});
     if(empty_initial || image_observed)
      result.guard=std::make_shared<IdentityImageFrameGuard>(result.guard,owner,s.root,
          work->source,work->serial,work->image_draws,empty_initial);
    }catch(...){}
    return result;
   }
  }
 }catch(...){s.Lose(__LINE__);}
 return result;
}
void NativeProfileIdentityOwner::ObserveNativeImageFrameCaptured(XThread& thread,
    const gpu::NativeFrameExecutionRequest& request,const NativeCompletedFrameRecord& record) noexcept {
 const auto image=std::dynamic_pointer_cast<const IdentityImageFrameGuard>(request.presentation_guard());
 if(!image)return;const auto owner=image->owner.lock();if(!owner)return;auto& s=*owner->state_;
 std::shared_ptr<NativeIdentityTileObserverSlot> current_slot;
 try{
  if(ActualThread()!=&thread || thread.kernel_state()!=s.kernel || !record.native_end_completed ||
     !request.plan() || request.plan()->is_overlay_control() ||
     request.input_provenance()!=gpu::NativeFrameInputProvenance::kImmutableSnapshot ||
     !request.input_snapshot() || !request.input_snapshot()->valid() || !request.identity().valid() ||
     // Runtime allocation generation and native module LOAD generation are
     // distinct real counters. Match the complete original allocation owner.
     request.identity().owner!=s.config.allocation->owner() ||
     !record.primary_physical_address || record.primary.size()!=request.plan()->words().size()*4 ||
     record.primary_physical_address<record.record_physical_address ||
     uint64_t(record.primary_physical_address)+record.primary.size()>
         uint64_t(record.record_physical_address)+record.record_allocation_bytes ||
     !record.record_allocation_bytes || uint64_t(record.record_physical_address)+record.record_allocation_bytes>0x20000000ull)return;
  for(size_t i=0;i<request.plan()->words().size();++i)
   if(xe::load_and_swap<uint32_t>(record.primary.data()+i*4)!=request.plan()->words()[i])return;
  // Canonicalization occurs now, while the real captured full-record pin and
  // immutable primary protect the existing Source. No native pointer is used
  // by the later completion acknowledgment.
  auto global=global_critical_region::AcquireDirect();
  current_slot=s.kernel->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  std::lock_guard lock(s.mutex);
  if(s.retired.load() || image->root!=s.root || image->captured || !s.image_slot ||
     !s.config.domain || !s.config.domain->active() ||
     s.config.domain->root()!=NativeImportDomain::Member{s.config.catalog->calls()->module()->hmodule_ptr(),s.root.generation} ||
     s.config.catalog->calls()->module()->load_generation()!=s.root.generation ||
     request.identity().owner.module_hmodule!=s.config.domain->root().hmodule ||
     current_slot.get()!=s.image_slot.get() || current_slot.owner_before(s.image_slot) || s.image_slot.owner_before(current_slot))return;
  const auto current=s.image_slot->current_.load(std::memory_order_acquire);const auto self=owner->weak_from_this();
  if(current.owner_before(self) || self.owner_before(current))return;
  const auto render=std::find_if(s.active.begin(),s.active.end(),[&](const auto& work){
   return work->serial==image->render && work->render_iteration && work->thread.get()==&thread &&
       work->source==image->source && !work->normal_return && !work->uncertain;
  });
  if(render==s.active.end())return;
  const auto* task=s.tasks.FindLiveTask((*render)->birth.task);
  if(!task || task->birth.generation!=(*render)->birth.generation)return;
  if(image->empty_initial){
   if(!s.EmptyInitialFrameCurrent(image->parent,image->source))return;
   // The retained completed record/immutable Source proves this exact hidden
   // frame; no profile image, texture/default readiness or visibility is issued.
   image->request=reinterpret_cast<uintptr_t>(&request);image->identity=request.identity();
   image->backend=request.backend_epoch();image->captured=true;return;
  }
  if(s.image_slot->unavailable_.load())return;
  bool match=false;
  for(const auto& candidate:image->draws){
   if(!candidate || candidate->draw.render!=image->render || candidate->draw.dc.root!=s.root ||
      candidate->draw.copied.input.source()!=image->source || candidate->draw.dc.address!=record.dc ||
      candidate->draw.device!=record.device ||
      s.images.Current(identity_detail::NativeImageJournal::Kind::kDc,record.dc)!=
          std::optional(candidate->draw.dc) || !s.images.NativeTextureOwned(candidate->draw.copied))continue;
   auto actual=*candidate;
   if(actual.packet_address>=0x20000000){
    const auto* heap=s.kernel->memory()->LookupHeap(actual.packet_address);
    if(!heap || heap->heap_type()!=HeapType::kGuestPhysical)continue;
    actual.packet_address=s.kernel->memory()->GetPhysicalAddress(actual.packet_address);
   }
   if(actual.packet_address<record.record_physical_address ||
      uint64_t(actual.packet_address)+12>uint64_t(record.record_physical_address)+record.record_allocation_bytes ||
      !identity_detail::ImageDrawMatchesPlan(actual,record.primary_physical_address,*request.plan()))continue;
   const auto offset=(actual.packet_address-record.primary_physical_address)/4;
   for(const auto& parsed:request.plan()->inventory().draws)if(parsed.packet_dword_offset==offset && parsed.pixel_shader &&
       *parsed.pixel_shader<request.shaders().size()){
    const auto& shader=request.shaders()[*parsed.pixel_shader];
    if(shader.load.stage!=gpu::xenos::ShaderType::kPixel)continue;
    const auto reads=gpu::NativeDrawReadInventory::TextureReads(parsed,&shader.metadata);
    if(reads.error!=gpu::NativeDrawReadInventory::Error::kNone)continue;
    for(const auto& read:reads.reads)if(read.slot==0 && read.fetch_words==actual.fetch)match=true;
   }
  }
  if(match){image->request=reinterpret_cast<uintptr_t>(&request);image->identity=request.identity();
   image->backend=request.backend_epoch();image->captured=true;}
 }catch(...){/* optional original-use observation unavailable; native frame unchanged */}
}
void NativeProfileIdentityOwner::ObserveNativeImageFrameCompleted(
    const gpu::NativeFrameExecutionRequest& request,const gpu::NativeFrameExecutionResult& result) noexcept {
 // This call is placed only AFTER genuine Complete + Session::RetireComplete
 // + actual transport invocation success. It does not retire or submit GPU IO.
 const auto image=std::dynamic_pointer_cast<const IdentityImageFrameGuard>(request.presentation_guard());
 if(!image)return;const auto owner=image->owner.lock();if(!owner)return;auto& s=*owner->state_;
 std::shared_ptr<NativeIdentityTileObserverSlot> current_slot;
 try{
  if(result.status!=gpu::NativeFrameExecutionResult::Status::kSubmitted || !result.title_state_restored ||
     result.completion.status()!=gpu::GpuCompletionStatus::kComplete ||
     result.completion.backend_epoch()!=request.backend_epoch() ||
     result.completion.native_frame_identity()!=std::optional(request.identity()) ||
     !gpu::NativeFrameOperationsMatchPlan(*request.plan(),result))return;
  if(s.retired.load() || !request.IsPresentationOwnerCurrent())return;
  current_slot=s.kernel->native_identity_tile_observer_slot_.load(std::memory_order_acquire);
  std::lock_guard lock(s.mutex);
  if(s.retired.load() || !s.image_slot || image->root!=s.root ||
     request.identity().owner!=s.config.allocation->owner() ||
     current_slot.get()!=s.image_slot.get() || current_slot.owner_before(s.image_slot) || s.image_slot.owner_before(current_slot) ||
     !image->captured || image->completed || image->request!=reinterpret_cast<uintptr_t>(&request) ||
     image->identity!=request.identity() || image->backend!=request.backend_epoch())return;
  const auto current=s.image_slot->current_.load(std::memory_order_acquire);const auto self=owner->weak_from_this();
  if(current.owner_before(self) || self.owner_before(current))return;
  if(image->empty_initial){
   if(!request.plan() || request.plan()->is_overlay_control() ||
      !s.config.domain || !s.config.domain->active() ||
      s.config.domain->root()!=NativeImportDomain::Member{s.config.catalog->calls()->module()->hmodule_ptr(),s.root.generation} ||
      s.config.catalog->calls()->module()->load_generation()!=s.root.generation ||
      !s.EmptyInitialFrameCurrent(image->parent,image->source))return;
   image->completed=true;
   // A genuinely completed empty initial frame acknowledges only that no
   // profile pixel consumption is required. The existing prepared Query,
   // descendant/wake/provider/paint/PM joins still own eventual reveal.
   s.job->render_refresh_observed.store(true,std::memory_order_release);s.changed.notify_all();return;
  }
  if(s.image_slot->unavailable_.load())return;
  image->completed=true;
  if(s.job && s.job->source==image->source && s.job->native_returned &&
     !s.job->canceled && !s.job->execution_uncertain){
   s.job->render_refresh_observed.store(true,std::memory_order_release);s.changed.notify_all();
  }
 }catch(...){/* no receipt, no synthesized return or pixel permission */}
}

void NativeProfileIdentityOwner::ObserveRenderIterationEnd(void* raw) noexcept {
 if(!raw)return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 const auto owner=Lookup(c.kernel_state);if(!owner)return;auto& s=*owner->state_;
 try{
  if(s.retired.load(std::memory_order_acquire))return;
  auto* thread=ActualThread();
  if(!thread || thread->kernel_state()!=s.kernel || c.thread_state!=thread->thread_state() ||
     c.r[13]!=thread->pcr_ptr() || c.r[1]>UINT32_MAX || (c.r[1]&15))return;
  const auto task=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,0x816CD6E8,c);
  std::lock_guard lock(s.mutex);
  std::shared_ptr<State::Work> work;
  for(auto i=s.active.rbegin();i!=s.active.rend();++i)
   if((*i)->render_iteration && (*i)->thread.get()==thread){work=*i;break;}
  if(!work)return; // Unmanaged/denied Query did not acquire an iteration.
  if(work->frame.sp!=c.r[1] || work->frame.pcr!=c.r[13] || !task || *task!=work->birth.task ||
     !work->read){work->uncertain=true;s.Lose(__LINE__);return;}
  // The genuine authenticated CD920 iteration boundary owns this read just
  // like an original function own-return. A delegated neutral restoration
  // restores only to Leave immediately, with no PPC/data access between.
  if(work->execution_read_fallback && !s.RestoreOwnedReadOnUnwind(work)){
   work->uncertain=true;return; // retain that exact owner; no global neutral poison
  }
  if(!s.tasks.ReturnExternalWork(work->frame,work->external_birth) ||
      !work->read->Leave()){work->uncertain=true;s.Lose(__LINE__);return;}
  if(work->listener_work){
   work->listener_dependencies=s.listeners->CaptureDependencies(*work->listener_work);
   const Events::Root root{s.root.kernel,s.root.domain,s.root.generation};
   const auto& f=work->frame;
   const Events::WorkKey key{root,work->birth.generation,work->serial,f.thread,
       f.pcr,work->birth.task,f.sp,f.lr,f.function};
   if(!work->listener_dependencies || work->listener_dependencies->unknown ||
      !s.listeners->FinishWholeWorkNormally(*work->listener_work,key,work->listener_source)){
    work->uncertain=true;s.Lose(__LINE__);return;
   }
   if(s.job && work->source==s.job->source){
    for(uint32_t i=0;i<work->listener_dependencies->count;++i)
     if(work->listener_dependencies->dequeued[i]==s.job->event_source)
      s.job->render_refresh_observed=true;
   }
  }
  if(!s.RecycleRead(work)){work->uncertain=true;s.Lose(__LINE__);return;}
  work->normal_return=true;
  const auto retiring=std::find(s.active.begin(),s.active.end(),work);
  if(retiring==s.active.end()){s.Lose(__LINE__);return;}
  s.host_retired.push_back(std::move(work));s.active.erase(retiring);s.changed.notify_all();
 }catch(...){s.Lose(__LINE__);}
}

bool NativeProfileIdentityOwner::ReapReturnedOnBootstrap(XThread& bootstrap,bool include_expected) noexcept try {
 auto& s=*state_;
 if(s.retired.load(std::memory_order_acquire) || ActualThread()!=&bootstrap || bootstrap.kernel_state()!=s.kernel ||
    bootstrap.thread_id()!=s.config.allocation->bootstrap_thread_id() ||
    bootstrap.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    XThread::GetCurrentFiberThread() || global_critical_region::is_held_by_current_thread())return false;
 std::vector<std::shared_ptr<State::Work>> retired;
 std::vector<std::shared_ptr<State::Expected>> retired_calls;
 std::vector<std::shared_ptr<State::ExecutionLifetime>> retired_execution;
 struct ReferenceReapGate {State& state;bool active=false;
  ~ReferenceReapGate(){if(active)state.task_close_frames.fetch_sub(1,std::memory_order_acq_rel);}
 } reference_reap{s};
 std::array<std::shared_ptr<TaskReference::Data>,64> references;
 std::array<std::shared_ptr<State::ContextReference>,64> retired_contexts;
 std::shared_ptr<IdentityServiceOrigin> retired_service;
 {
  std::lock_guard lock(s.mutex);
  if(s.retired.load() || s.uncertain.load() || s.tasks.lost() ||
     (s.wake && s.wake->lost()) || (s.listeners && s.listeners->lost()))return false;
  for(auto i=s.active.begin();i!=s.active.end();){const auto& work=*i;
   if(work->execution_only && work->normal_return && !work->execution_read.pending() &&
       work->execution_lifetime && work->execution_lifetime->native_freed){
    s.host_retired.push_back(work);i=s.active.erase(i);
   }else ++i;
  }
  for(auto i=s.execution_retained.begin();i!=s.execution_retained.end();){const auto& work=*i;
   if(work->normal_return && !work->execution_read.pending() &&
       work->execution_lifetime && work->execution_lifetime->native_freed){
    s.host_retired.push_back(work);i=s.execution_retained.erase(i);
   }else ++i;
  }
  for(auto i=s.execution_lifetimes.begin();i!=s.execution_lifetimes.end();){const auto& lifetime=*i;
   const bool running=std::any_of(s.active.begin(),s.active.end(),[&](const auto& work){
    return work->execution_only && work->execution_lifetime==lifetime && !work->normal_return;
   });
   if(lifetime && lifetime->native_freed && !running){retired_execution.push_back(lifetime);i=s.execution_lifetimes.erase(i);}
   else ++i;
  }
  if(!s.hook_rundown->Capture().active && !s.task_close_frames.load(std::memory_order_acquire))
   for(size_t n=0;n<s.context_references.size();++n)
    if(s.context_references[n] && s.context_references[n]->receipt.finished())
     retired_contexts[n]=std::move(s.context_references[n]);
  retired.swap(s.host_retired);
  // Expected receipts have separate native Data/callback ownership. Preserve
  // their original Step boundary; pending maintenance retires Work only.
  if(include_expected)retired_calls.swap(s.retired_expected);
  // A reference receipt may be shared by its producer. Revoke its independent
  // resources on this actual host cut, only after every getter/close tail and
  // hook has left; never transfer last resource ownership to a guest local.
  if(!s.task_close_frames.load(std::memory_order_acquire) && !s.hook_rundown->Capture().active)
   for(size_t n=0;n<s.task_references.size();++n){const auto& d=s.task_references[n];
    if(d && d->receipt.finished() && !d->bound.load(std::memory_order_acquire) &&
        !d->resources_retired.load(std::memory_order_acquire) && !d->reaping.exchange(true))references[n]=d;
   }
  if(s.identity_service && !s.hook_rundown->Capture().active &&
      s.IdentityServiceJoined(s.identity_service->data_->job))retired_service=std::move(s.identity_service);
  if(retired_service || std::any_of(references.begin(),references.end(),[](const auto& d){return bool(d);}) ||
     std::any_of(retired_contexts.begin(),retired_contexts.end(),[](const auto& d){return bool(d);})) { 
   s.task_close_frames.fetch_add(1,std::memory_order_acq_rel);reference_reap.active=true;
  }
 }
 if(retired_service){NativeXmsgProvenance::ReleaseIdentityService(retired_service->data_->link);
  retired_service->data_->ReleaseResourcesOnHost();}
 retired_service.reset(); // All source/code/Job/Work owners drop outside State/GCRC.
 for(const auto& d:references)if(d)d->ReleaseResourcesOnHost();
 // These existing queues hold completed returns or explicitly canceled
 // pre-body reservations, never unreturned native work. Destroy them on this
 // real OS Bootstrap host phase after dropping State/GCRC, including while
 // its real scheduled identity call is still pending.
 retired.clear();retired_calls.clear();retired_execution.clear();
 if(s.listeners){auto listener_retired=s.listeners->ReapOnHostOutsideGuestLocks();}
 return true;
}catch(...){return false;} // Maintenance is never a native result/readiness source.

bool NativeProfileIdentityOwner::StepOnBootstrap(XThread& bootstrap) noexcept try {
 auto& s=*state_;
 if(s.retired.load(std::memory_order_acquire) || ActualThread()!=&bootstrap || bootstrap.kernel_state()!=s.kernel ||
    bootstrap.thread_id()!=s.config.allocation->bootstrap_thread_id() ||
    bootstrap.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    global_critical_region::is_held_by_current_thread())return false;
 if(!s.profile_enabled)return ReapReturnedOnBootstrap(bootstrap);
 ObserveBootstrapHandoff(HandoffStage::kStepAdmission);
 ObserveBootstrapHandoff(HandoffStage::kRefusalReport);
 ReportFirstRefusalOnBootstrap(bootstrap);
 ObserveBootstrapHandoff(HandoffStage::kRefusalReported);
 ObserveBootstrapHandoff(HandoffStage::kReap);
 if(!ReapReturnedOnBootstrap(bootstrap))return false;
 ObserveBootstrapHandoff(HandoffStage::kReaped);
 // Called only after full native startup by the service's existing progress
 // turn. Missing/used/mismatched/busy capture leaves normal alignment intact.
 s.ConsumeInitialEmptyBaseline(bootstrap);
 auto* pm=s.kernel->xam_state()->profile_manager();
 pm->RetryRosterSettlement(); // genuine exact flagged pre-entry cancellation only
 std::shared_ptr<State::Job> job;
 std::shared_ptr<xam::ProfileRosterIntent> cleanup;
 {
  std::lock_guard lock(s.mutex);job=s.job;cleanup=s.bootstrap_cleanup;
 }
 const auto exhausted=[&]{
  std::lock_guard lock(s.mutex);
  return s.bootstrap_deadline!=std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now()>=s.bootstrap_deadline;
 };
 const auto deadline_failure=[&]{
  const auto error=std::make_error_code(std::errc::timed_out);
  s.RecordBootstrapFailure(21,__LINE__,error,false);
  std::lock_guard lock(s.mutex);s.bootstrap_fatal=true;return false;
 };
 // A failed Prepare may already have published a canceling operation even
 // when it returned null. Retain and cancel that EXACT attempt, never infer
 // cleanup from a missing result or prepare over a live pending operation.
 if(cleanup){
  pm->RetryRosterSettlement();
  const auto canceled=pm->CancelRoster(cleanup);
  if(canceled.phase!=xam::ProfileRosterPhase::kCanceled){
   const bool retryable=State::RetryableBootstrapError(canceled.error);
   s.RecordBootstrapFailure(20,__LINE__,canceled.error,retryable,0,0,uint32_t(canceled.phase));
   if(!retryable){std::lock_guard lock(s.mutex);s.bootstrap_fatal=true;return false;}
   return exhausted()?deadline_failure():true;
  }
  bool fatal=false;
  {std::lock_guard lock(s.mutex);
   if(s.bootstrap_cleanup!=cleanup){
    // The old exact intent is factually Canceled. A later failed preparation
    // may replace only that canceled metadata with its own pending cleanup.
    // Keep the new owner for the next turn; never clear it by an old receipt.
    if(s.bootstrap_cleanup && !s.job)return true;
    s.Lose(__LINE__);return false;
   }
   if(s.job){s.Lose(__LINE__);return false;}
   s.bootstrap_cleanup.reset();fatal=s.bootstrap_fatal;
   if(!fatal && !s.stopping.load())s.boot_alignment_claimed=false;
  }
  if(s.stopping.load())return true;
  if(fatal)return false;
  return exhausted()?deadline_failure():true; // Fresh Prepare is a later turn.
 }
 bool initial=false;
 {
  std::lock_guard lock(s.mutex);
  if(s.bootstrap_fatal && !s.stopping.load())return false;
  initial=!job && !s.aligned && !s.boot_alignment_claimed && !s.stopping.load();
  if(initial){
   s.boot_alignment_claimed=true;++s.bootstrap_attempts;
   if(s.bootstrap_deadline==std::chrono::steady_clock::time_point{})
    s.bootstrap_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  }
 }
 if(initial){
  if(exhausted())return deadline_failure();
  const auto failed=[&](uint32_t line,uint32_t stage,std::error_code error,bool retryable,
                        const std::shared_ptr<xam::ProfileRosterIntent>& attempt){
   if(!error){error=std::make_error_code(std::errc::operation_canceled);retryable=false;}
   s.RecordBootstrapFailure(stage,line,error,retryable);
   {std::lock_guard lock(s.mutex);
    if(s.bootstrap_cleanup || s.job){s.Lose(__LINE__);return false;}
    s.bootstrap_cleanup=attempt;
    if(!retryable)s.bootstrap_fatal=true;
    if(!attempt && retryable)s.boot_alignment_claimed=false;
   }
   // Native entry has not happened; common cancellation owns every staged
   // mount/pause until its factual outcome. Busy is retried on the next turn.
   if(attempt){pm->RetryRosterSettlement();pm->CancelRoster(attempt);}
   return retryable?(exhausted()?deadline_failure():true):false;
  };
  std::error_code error;const auto expected=pm->CaptureRosterExpected(error);
  if(error || !expected.roster)return failed(__LINE__,1,error,State::RetryableBootstrapError(error),{});
  xam::ProfileRosterDesired desired;
  desired.notify_host_compatibility=false;desired.remember_slots=false;
  for(size_t i=0;i<4;++i)desired.xuids[i]=expected.roster->slots()[i].xuid;
  std::shared_ptr<xam::ProfileRosterIntent> attempt;
  const auto intent=pm->PrepareRosterAttempt(desired,expected,error,&attempt);
  if(!intent || error)return failed(__LINE__,2,error,State::RetryableBootstrapError(error),attempt);
  if(exhausted())return failed(__LINE__,21,std::make_error_code(std::errc::timed_out),false,attempt);
  bool retryable=false;
  if(!EnqueuePrepared(intent,nullptr,nullptr,0,0,0,true,error,&retryable))
   return failed(__LINE__,17,error,retryable,attempt);
  {std::lock_guard lock(s.mutex);job=s.job;}
 }
 if(!job){
  bool close=false;{std::lock_guard lock(s.mutex);close=s.stopping.load() && s.IdentityWorkEmpty();}
  if(close)s.listeners->CloseNewSources();
  return true;
 }
 if(s.stopping.load() && !job->native_entered && !job->entering && !job->transport_running)
  CancelQueued(job->intent);
 auto phase=job->intent->result().phase;
 if(job->canceled){
  if(phase!=xam::ProfileRosterPhase::kCanceled){pm->CancelRoster(job->intent);return true;}
  bool safe=false;
  {
   std::lock_guard lock(s.mutex);
   if(s.job==job && !job->native_entered && !job->entering && !job->transport_running &&
      s.render_phase==State::RenderPhase::kDrainingOld)
    s.render_phase=State::RenderPhase::kClosingOld;
   safe=s.job==job && s.IdentityWorkEmpty() && s.expected.empty() &&
       !job->native_entered && !job->entering && !job->transport_running &&
       (s.render_phase==State::RenderPhase::kOldPaused || s.stopping.load());
  }
  if(!safe)return true;
  const auto transport=NativeSystemGraphicsTransport::Lookup(s.kernel);
  if(!transport || !transport->DrainIdentityFramePaint(bootstrap,job->old_pixels))return true;
  std::error_code error;auto ticket=pm->CaptureCurrentNativeReadTicket(s.config.factory_binding,error);
  if(!ticket || error)return true;
  std::array<std::unique_ptr<xam::ProfileRosterNativeReadScope>,64> reads;
  for(auto& read:reads){read=xam::ProfileRosterNativeReadScope::Prepare(ticket,s.config.factory_binding);if(!read)return false;}
  auto pixels=std::make_shared<IdentityPixelGuard>(job->old_pixels->source(),s.aligned && !s.stopping.load(),s.pixel_lifetime);
  std::shared_ptr<State::Job> release;
  std::shared_ptr<xam::ProfileRosterNativeReadTicket> old_ticket;
  std::array<std::unique_ptr<xam::ProfileRosterNativeReadScope>,64> old_reads;
  {
   std::lock_guard lock(s.mutex);
   if(s.job!=job || !s.IdentityWorkEmpty() || !s.expected.empty())return false;
   if(s.wake && !s.wake->CancelBeforeNative(job->source,NativeAtomicWord(s.native_pending)))return true;
   if(job->event_origin && !s.listeners->FinishSourceAfterDescendants(*job->event_origin))return false;
   old_ticket=std::move(s.current_ticket);old_reads.swap(s.boot_reads);
   s.current_ticket=std::move(ticket);s.boot_reads=std::move(reads);s.pixels=std::move(pixels);
   s.old_ingress_closed=false;s.render_phase=State::RenderPhase::kOpen;
   s.render_query_parked=false;job->done=true;release=std::move(s.job);
   if(!s.aligned && !s.stopping.load())s.Lose(__LINE__); // No pretend startup alignment.
  }
  s.changed.notify_all();return !s.uncertain.load();
 }
 if(job->execution_uncertain || phase==xam::ProfileRosterPhase::kUncertain)return false;
 if(s.stopping.load() && job->native_entered){
  bool ready=false;
  {std::lock_guard lock(s.mutex);
   if(s.job!=job)return false;
   if(job->execution_quiescent)return true;
   if(s.render_phase!=State::RenderPhase::kStopPaused)s.render_phase=State::RenderPhase::kClosingStop;
  }
  // The Query itself changes ClosingStop to StopPaused only after real Poll,
  // no entered work/expected frames, closed native descendants and pending0.
  {std::lock_guard lock(s.mutex);
   ready=s.render_phase==State::RenderPhase::kStopPaused && s.render_query_parked &&
       job->native_returned && !job->transport_running && s.IdentityWorkEmpty() && s.expected.empty() &&
       s.tasks.SourceDescendantsClosed(job->source) && s.wake && s.native_pending &&
       s.wake->InvocationsDrained(NativeAtomicWord(s.native_pending));
  }
  if(!ready)return true;
  const auto transport=NativeSystemGraphicsTransport::Lookup(s.kernel);
  if(!transport || !transport->DrainIdentityFramePaint(bootstrap,s.pixels))return true;
  {std::lock_guard lock(s.mutex);
   if(s.job!=job || s.render_phase!=State::RenderPhase::kStopPaused ||
      !s.IdentityWorkEmpty() || !s.expected.empty())return false;
   job->execution_quiescent=true;
  }
  // No ForJoined/Finalize, canceled phase, new pixels, host notification or
  // source release. Exact entered op/account/backing/code survives to real stop.
  s.changed.notify_all();return true;
 }
 ObserveBootstrapHandoff(HandoffStage::kPreentry);
 bool old_paused=false;
 auto& observation=job->preentry_observation;
 const auto observed=[&](PaintStage stage,uint32_t bit,bool value) noexcept {
  return observation.Observe(stage,bit,value);
 };
 {
  std::lock_guard lock(s.mutex);
  // Diagnostic-only generation; never reset an in-flight synchronous drain or
  // the already-armed continuation. No native pending read is added here.
  if(!job->data_worker_ready && !job->preentry_observation_exhausted){
   if(job->preentry_attempt==PaintObservation::kMaximumAttempt){
    job->preentry_observation_exhausted=true;job->preentry_attempt=0;
   }else ++job->preentry_attempt;
   observation.Reset(job->preentry_attempt);job->preentry_pending_available=false;
   job->preentry_work_blockers={};job->preentry_source_blockers={};
  }
  observation.Begin(PaintStage::kOldPaused);
  job->preentry_work_blockers[0]={};job->preentry_source_blockers[0]={};
  old_paused=observed(PaintStage::kOldPaused,0,s.job==job) && observed(PaintStage::kOldPaused,1,s.render_phase==State::RenderPhase::kOldPaused) &&
      observed(PaintStage::kOldPaused,2,s.render_query_parked) && observed(PaintStage::kOldPaused,3,s.IdentityWorkEmpty(&job->preentry_work_blockers[0])) && observed(PaintStage::kOldPaused,4,s.expected.empty()) &&
      observed(PaintStage::kOldPaused,5,s.tasks.SourceDescendantsClosed(s.current_read_source,&job->preentry_source_blockers[0]));
  observation.Finish(PaintStage::kOldPaused,old_paused);
 }
 if(old_paused && !job->old_paint_drained){
  ObserveBootstrapHandoff(HandoffStage::kPaint);
  observation.Begin(PaintStage::kOldPaint);
  const auto transport=NativeSystemGraphicsTransport::Lookup(s.kernel);
  if(observed(PaintStage::kOldPaint,0,!transport) ||
     observed(PaintStage::kOldPaint,1,!transport->DrainIdentityFramePaint(bootstrap,job->old_pixels,&observation))){
   observation.Finish(PaintStage::kOldPaint,false);return true;
  }
  observation.Finish(PaintStage::kOldPaint,true);
  ObserveBootstrapHandoff(HandoffStage::kPaintReturned);
  std::lock_guard lock(s.mutex);
  observation.Begin(PaintStage::kPostPaint);job->preentry_work_blockers[1]={};
  if(observed(PaintStage::kPostPaint,0,s.job!=job) || observed(PaintStage::kPostPaint,1,s.render_phase!=State::RenderPhase::kOldPaused) ||
     observed(PaintStage::kPostPaint,2,!s.IdentityWorkEmpty(&job->preentry_work_blockers[1])) || observed(PaintStage::kPostPaint,3,!s.expected.empty()) ||
     observed(PaintStage::kPostPaint,4,!s.wake) || observed(PaintStage::kPostPaint,5,!s.native_pending)){
   observation.Finish(PaintStage::kPostPaint,false);return true;
  }
  observation.Finish(PaintStage::kPostPaint,true);
  const auto pending=NativeAtomicWord(s.native_pending);
  job->preentry_pending=pending;job->preentry_pending_available=true;
  observation.Begin(PaintStage::kWakeSourceListener);job->preentry_source_blockers[1]={};
  if(observed(PaintStage::kWakeSourceListener,0,!s.wake->InvocationsDrained(pending)) || observed(PaintStage::kWakeSourceListener,1,!s.tasks.SourceDescendantsClosed(s.current_read_source,&job->preentry_source_blockers[1])) ||
     observed(PaintStage::kWakeSourceListener,2,!job->old_event_origin) ||
     observed(PaintStage::kWakeSourceListener,3,!s.listeners->SourceOnlyProducer(*job->old_event_origin))){
   observation.Finish(PaintStage::kWakeSourceListener,false);return true;
  }
  observation.Finish(PaintStage::kWakeSourceListener,true);
  // Already pending old workers were allowed to finish in OpenOld. Only now
  // close and arm the exact next source, without stranding an old pending bit.
  s.wake->CloseOldIngress();
  observation.Begin(PaintStage::kArm);
  const bool armed=s.wake->ArmPrepared(job->source,pending);
  observation.Finish(PaintStage::kArm,armed);
  if(!armed)return false;
  // This is still the only dataful-worker grant: true CD920/read/Task end,
  // actual source/listener closure, physical paint drain and native wake arm.
  job->old_paint_drained=true;job->initializer_ready=true;
  job->data_worker_ready=true;s.changed.notify_all();
 }
 bool run_host=false;
 std::shared_ptr<NativeSystemTaskContinuation> continuation;
 {
  std::lock_guard lock(s.mutex);
  observation.Begin(PaintStage::kContinuation);
  run_host=observed(PaintStage::kContinuation,0,s.job==job) && observed(PaintStage::kContinuation,1,job->data_worker_ready) && observed(PaintStage::kContinuation,2,!job->original_caller) &&
      observed(PaintStage::kContinuation,3,!job->native_entered) && observed(PaintStage::kContinuation,4,!job->entering) && observed(PaintStage::kContinuation,5,!job->transport_running) && observed(PaintStage::kContinuation,6,!s.stopping.load());
  if(run_host){continuation=s.continuation.lock();if(observed(PaintStage::kContinuation,7,bool(continuation)))job->transport_running=true;}
  else observation.Finish(PaintStage::kContinuation,false);
 }
 if(run_host){
  if(!continuation){observation.Finish(PaintStage::kContinuation,false);return true;}
  ObserveBootstrapHandoff(HandoffStage::kContinuation);
  const auto result=continuation->InvokeOwnedProfileIdentityAndWait(*this,job->serial,
      [self=shared_from_this()](XThread& actual){return self->ExecutePreparedOn(actual);});
  ObserveBootstrapHandoff(HandoffStage::kContinuationReturned);
  {std::lock_guard lock(s.mutex);job->transport_running=false;
   const bool failed=observed(PaintStage::kContinuation,8,!result.succeeded);
   observation.Finish(PaintStage::kContinuation,result.succeeded);
   if(failed){job->execution_uncertain=true;s.Lose(__LINE__);return false;}}
 }
 phase=job->intent->result().phase;
 if(phase!=xam::ProfileRosterPhase::kPublished && phase!=xam::ProfileRosterPhase::kFinalizing &&
    phase!=xam::ProfileRosterPhase::kCommitted)return true;
 // Native protocol/map work is independent of visual refresh and final
 // resource retirement. Keep the one Job/Operation and all existing owners.
 bool protocol_ready=false;
 {
  std::lock_guard lock(s.mutex);
  protocol_ready=s.job==job && job->protocol_returned && job->protocol_work &&
      job->protocol_frame.root==s.root && job->protocol_frame.function==0x8199C838 &&
      job->protocol_birth.generation && job->protocol_birth.task &&
      !job->execution_uncertain && !s.uncertain.load() &&
      s.published_source.load(std::memory_order_acquire)==job->source;
 }
 if(protocol_ready){
  if(!job->protocol){
   job->protocol=xam::ProfileRosterNativeCompletion::ForProtocol(job->intent,
       s.config.factory_binding,s.config.factory_binding->nonce(),job->protocol_value);
   if(!job->protocol)return false;
  }
  const auto committed=pm->PublishNativeRosterProtocol(job->intent,*job->protocol);
  RecordOriginalLogonRosterResult(job->original_logon_observation,OriginalLogonDiagnostic::kPublicationFirst,committed);
  if(committed.maps_committed && committed.writers_available)
   RecordOriginalLogonRosterResult(job->original_logon_observation,OriginalLogonDiagnostic::kPublicationReady,committed);
  if(committed.phase==xam::ProfileRosterPhase::kUncertain){s.Lose(__LINE__);return false;}
 }
 // Scalar witness of this ORIGINAL final-readiness evaluation, never a
 // cross-cut readiness fact. No pointer/resource escapes the State lock.
 struct FinalReadiness {
  bool enabled=false;uint32_t evaluated=0,truth=0,phase=0;
  uint64_t job=0,source=0;bool want=false;
  uint32_t provider=0,provider_type=0,provider_status=0,provider_flags=0;
  uint64_t provider_generation=0,provider_source=0,provider_version=0,provider_uri_generation=0;
  Tasks::SourceBlocker blocker;
  bool pending_available=false;uint32_t pending=0,no_read_providers=0;
  uint64_t no_read_work=0,no_read_uri=0;
 } final_readiness;
 final_readiness.enabled=cvars::native_system_ui_trace;
 {
  std::lock_guard lock(s.mutex);
  if(s.job!=job || s.published_source.load(std::memory_order_acquire)!=job->source)return false;
  job->publisher_observed=true;
  if(s.render_phase==State::RenderPhase::kOldPaused){
   s.pixels=job->hidden_pixels;s.render_phase=State::RenderPhase::kHiddenRefresh;
   s.render_query_parked=false;
  }
  const auto* provider=s.FirstUnfreshProvider(job->source);
  const bool providers=!provider;
  if(final_readiness.enabled)for(const auto& p:s.providers)
   if(p && p->no_read_query && s.ProviderWithoutIssuedImageDuty(*p)){
    ++final_readiness.no_read_providers;final_readiness.no_read_work=p->no_read_query->work;
    final_readiness.no_read_uri=p->no_read_query->uri_generation;
   }
  if(provider){
   if(final_readiness.enabled){
    final_readiness.provider=provider->object;final_readiness.provider_type=provider->type;
    final_readiness.provider_generation=provider->generation;
    final_readiness.provider_uri_generation=provider->image_uri_generation;
    final_readiness.provider_source=provider->completed_source;
    final_readiness.provider_version=provider->completed_version;
    final_readiness.provider_status=provider->completed_status;
    final_readiness.provider_flags=uint32_t(provider->completed_empty)|
        (uint32_t(bool(provider->last_output))<<1)|(uint32_t(bool(provider->image_output))<<2)|
        (uint32_t(bool(provider->image_key))<<3);
   }
  }
  // An output is either actual freshly written bytes or the native consumer's
  // exact cleared-pointer handling of a real failed read. These are never
  // conflated with successful image/asset readiness.
  const auto host_duties=job->intent->result();
  const auto observed=[&](uint32_t bit,bool value){
   if(final_readiness.enabled){final_readiness.evaluated|=1u<<bit;if(value)final_readiness.truth|=1u<<bit;}
   return value;
  };
  const auto drained=[&](){
   // Preserve the single original pending-word read at the final original cut.
   const auto pending=NativeAtomicWord(s.native_pending);
   if(final_readiness.enabled){final_readiness.pending_available=true;final_readiness.pending=pending;}
   return s.wake->InvocationsDrained(pending);
  };
  if(observed(0,job->protocol_returned) && observed(1,host_duties.maps_committed) && observed(2,host_duties.writers_available) &&
     observed(3,job->native_returned) && observed(4,!job->transport_running) && observed(5,job->render_refresh_observed) && observed(6,providers) &&
     observed(7,s.tasks.SourceDescendantsClosed(job->source,final_readiness.enabled?&final_readiness.blocker:nullptr)) &&
     observed(8,bool(s.wake)) && observed(9,bool(s.native_pending)) && observed(10,drained()))job->want_final_barrier=true;
  if(final_readiness.enabled){
   final_readiness.job=job->serial;final_readiness.source=job->source;
   final_readiness.phase=uint32_t(s.render_phase);final_readiness.want=job->want_final_barrier;
  }
 }
 // Bounded reporting occurs after State unlock. Unevaluated predicates and
 // unavailable descendant/pending fields remain explicitly unavailable.
 if(final_readiness.enabled){
  static NativeSystemUiTraceLimiter trace;
  const auto& d=final_readiness;const auto& b=d.blocker;
  trace.EmitTransition(d.job^(uint64_t(d.evaluated)<<32)^d.truth^d.provider_generation^b.generation^
      b.serial^(uint64_t(d.pending)<<16),"identity","final_readiness",
    {{"job_serial",d.job},{"job_source",d.source},{"render_phase",d.phase},
     {"evaluated",d.evaluated},{"predicate_true",d.truth},{"want_final_barrier",d.want},
     {"provider",d.provider},{"provider_generation",d.provider_generation},{"provider_type",d.provider_type},
     {"provider_uri_generation",d.provider_uri_generation},
     {"provider_no_read_count",d.no_read_providers},{"provider_no_read_work",d.no_read_work},
     {"provider_no_read_uri",d.no_read_uri},
     {"provider_completed_source",d.provider_source},{"provider_completed_version",d.provider_version},
     {"provider_completed_status",d.provider_status},{"provider_flags",d.provider_flags},
     {"source_available",b.available},{"source_closed",b.closed},{"source_reason",b.kind},
     {"source",b.source},{"source_generation",b.generation},{"source_task",b.task},
     {"source_serial",b.serial},{"recorded_callback",b.callback},{"recorded_argument",b.argument},
     {"output_slot",b.output},{"source_state",b.state},{"source_thread",b.frame.thread},
     {"source_pcr",b.frame.pcr},{"source_sp",b.frame.sp},{"source_lr",b.frame.lr},
     {"source_function",b.frame.function},{"pending_available",d.pending_available},{"native_pending",d.pending}});
 }
 s.changed.notify_all();
 bool new_paused=false;
 {
  std::lock_guard lock(s.mutex);
  new_paused=s.job==job && job->want_final_barrier && job->render_refresh_observed &&
      s.render_phase==State::RenderPhase::kPreparedPaused && s.render_query_parked &&
      s.IdentityWorkEmpty() && s.expected.empty() && job->event_origin &&
      s.listeners->SourceProducerQuiescent(*job->event_origin) && s.ProvidersFresh(job->source) &&
      s.tasks.SourceDescendantsClosed(job->source);
 }
 if(!new_paused)return true;
 if(!job->new_paint_drained){
  const auto transport=NativeSystemGraphicsTransport::Lookup(s.kernel);
  if(!transport || !transport->DrainIdentityFramePaint(bootstrap,job->hidden_pixels))return true;
  std::lock_guard lock(s.mutex);
  if(s.job!=job || s.render_phase!=State::RenderPhase::kPreparedPaused ||
     !s.IdentityWorkEmpty() || !s.expected.empty())return false;
  if(!s.wake->FinishJoinedOrigin(job->source,NativeAtomicWord(s.native_pending)))return false;
  if(job->old_event_origin && !s.listeners->FinishSourceAfterDescendants(*job->old_event_origin))return false;
  job->old_event_finished=true;job->new_paint_drained=true;
 }
 {std::lock_guard lock(s.mutex);if(!s.IdentityServiceJoined(job))return true;}
 if(!job->joined){
  job->joined=xam::ProfileRosterNativeCompletion::ForJoined(job->intent,
      s.config.factory_binding,s.config.factory_binding->nonce());
  if(!job->joined)return false;
 }
 const auto settled=phase==xam::ProfileRosterPhase::kCommitted?job->intent->result():
     pm->FinalizeNativeRoster(job->intent,*job->joined);
 RecordOriginalLogonRosterResult(job->original_logon_observation,OriginalLogonDiagnostic::kSettlementFirst,settled);
 if(settled.phase!=xam::ProfileRosterPhase::kCommitted){
  if(settled.phase==xam::ProfileRosterPhase::kUncertain){s.Lose(__LINE__);return false;}
  return true; // Exact factual joined receipt retained across real settle IO.
 }
 RecordOriginalLogonRosterResult(job->original_logon_observation,OriginalLogonDiagnostic::kSettlementCommitted,settled);
 std::error_code error;const auto current=pm->CaptureRosterExpected(error);
 const auto ticket=error?nullptr:pm->CaptureCurrentNativeReadTicket(s.config.factory_binding,error);
 if(error || !ticket)return true; // A concurrent rejected Prepare/Cancel may still hold the PM gate.
 if(current.roster!=job->intent->published_roster()){s.Lose(__LINE__);return false;}
 std::array<std::unique_ptr<xam::ProfileRosterNativeReadScope>,64> reads;
 for(auto& read:reads){read=xam::ProfileRosterNativeReadScope::Prepare(ticket,s.config.factory_binding);if(!read)return false;}
 std::shared_ptr<State::Job> release;
 std::shared_ptr<xam::ProfileRosterNativeReadTicket> old_ticket;
 std::array<std::unique_ptr<xam::ProfileRosterNativeReadScope>,64> old_reads;
 {
  std::lock_guard lock(s.mutex);
  if(s.job!=job || s.render_phase!=State::RenderPhase::kPreparedPaused ||
     s.uncertain.load() || !job->event_origin)return false;
  // PM settlement may do real IO outside State. Recheck actual consumers at
  // the final pixel cut; healthy late work waits for another idempotent pass.
  if(!s.IdentityWorkEmpty() || !s.expected.empty() || !s.IdentityServiceJoined(job) ||
     !s.listeners->SourceProducerQuiescent(*job->event_origin))return true;
  if(job->apc_code){
   if(s.apc_code_until_actual_stop.size()>=256){s.Lose(__LINE__);return false;}
   s.apc_code_until_actual_stop.push_back(*job->apc_code);
  }
  old_ticket=std::move(s.current_ticket);old_reads.swap(s.boot_reads);
  s.current_ticket=ticket;s.current_roster=current.roster;s.boot_reads=std::move(reads);
  s.current_read_source=job->source;
  s.current_event_source=job->event_source;s.current_event_origin=job->event_origin;
  if(s.stopping.load())job->new_pixels->Revoke();
  s.pixels=job->new_pixels; // Brand-new token; old/hidden tokens stay revoked.
  s.render_phase=State::RenderPhase::kOpen;s.render_query_parked=false;
  s.old_ingress_closed=false;s.aligned=true;job->done=true;release=std::move(s.job);
 }
 s.changed.notify_all();return true;
}catch(...){state_->Lose(__LINE__);return false;}

void NativeProfileIdentityOwner::BeginStop() noexcept {
 state_->stopping.store(true,std::memory_order_release);
 // Admission and parkable count share one atomic cut. Native cleanup and
 // true own-return observers stay counted/runnable until the CPU worker join.
 state_->hook_rundown->SealParkable();
 if(state_->pixel_lifetime)state_->pixel_lifetime->closed.store(true,std::memory_order_release);
 try{std::lock_guard lock(state_->mutex);
  if(state_->pixels)state_->pixels->Revoke();
  if(state_->job && state_->job->new_pixels)state_->job->new_pixels->Revoke();
  if(state_->job && (state_->job->native_entered || state_->job->entering ||
      state_->job->transport_running) && state_->render_phase!=State::RenderPhase::kStopPaused)
   state_->render_phase=State::RenderPhase::kClosingStop;
 }catch(...){state_->Lose(__LINE__);}
 state_->changed.notify_all();
}
namespace {
using IdentityStopObservation=NativeProfileIdentityOwner::StopObservation;
bool ObserveIdentityStopConjunct(IdentityStopObservation* d,uint32_t bit,bool passed) noexcept {
 if(d){d->evaluated|=uint64_t(1)<<bit;if(passed)d->truth|=uint64_t(1)<<bit;
  else if(!d->first_failed)d->first_failed=bit+1;}
 return passed;
}
bool FinishIdentityStopObservation(IdentityStopObservation* d,bool result) noexcept {
 if(d){d->result_available=1;d->result=result;}return result;
}
// Only the visited leaves of one ORIGINAL row predicate are observed. No
// missing leaf/row is sampled after native short-circuiting has stopped it.
struct IdentityStopRowObservation {
 uint64_t evaluated=0,truth=0;
 bool Test(uint32_t bit,bool value) noexcept {
  evaluated|=uint64_t(1)<<bit;if(value)truth|=uint64_t(1)<<bit;return value;
 }
 void Rejected(IdentityStopObservation* d,uint32_t kind) const noexcept {
  if(d && !d->row_class){d->row_class=kind;d->row_evaluated=evaluated;d->row_truth=truth;}
 }
};
} // namespace
bool NativeProfileIdentityOwner::Drained(StopObservation* observation) const noexcept {
 if(observation)*observation={};
 try{
  std::lock_guard lock(state_->mutex);
  IdentityStopRowObservation first_counted;
  const bool counted_work=std::any_of(state_->counted.begin(),state_->counted.end(),[&](const auto& row){
   if(observation)++observation->visited[0];
   IdentityStopRowObservation r;
   const bool bad=r.Test(0,bool(row)) &&
       ((r.Test(1,!row->allocated) && r.Test(2,!row->failed)) ||
        (r.Test(3,row->delete_entered) && r.Test(4,!row->free_returned)));
   if(bad)first_counted=r;return bad;
  });
  const auto checked=[&](uint32_t bit,bool value){
   if(bit==15 && !value)first_counted.Rejected(observation,1);
   return ObserveIdentityStopConjunct(observation,bit,value);
  };
  return FinishIdentityStopObservation(observation,
      checked(0,state_->stopping.load(std::memory_order_acquire)) &&
      checked(1,!state_->uncertain.load(std::memory_order_acquire)) &&
      checked(2,state_->hook_rundown->Capture().fully_drained()) &&
      checked(3,!state_->identity_executor_frames.load(std::memory_order_acquire)) &&
      checked(4,state_->PeriodicStopped()) && checked(5,state_->tasks.ExecutionClosed()) &&
      checked(6,!state_->execution_unavailable.load(std::memory_order_acquire)) &&
      checked(7,!state_->listener_object_frames.load(std::memory_order_acquire)) &&
      checked(8,!state_->task_close_frames.load(std::memory_order_acquire)) &&
      checked(9,!state_->context_reference_unavailable) &&
      checked(10,std::all_of(state_->context_references.begin(),state_->context_references.end(),[&](const auto& d){
       if(observation)++observation->visited[1];IdentityStopRowObservation r;
       const bool good=r.Test(0,!d) || r.Test(1,d->receipt.finished());
       if(!good)r.Rejected(observation,2);return good;
      })) &&
      checked(11,!state_->public_calls.load(std::memory_order_acquire)) &&
      checked(12,!state_->bootstrap_cleanup) &&
      checked(13,std::all_of(state_->task_references.begin(),state_->task_references.end(),[&](const auto& d){
       if(observation)++observation->visited[2];IdentityStopRowObservation r;
       const bool good=r.Test(0,!d) || r.Test(1,d->receipt.finished());
       if(!good)r.Rejected(observation,3);return good;
      })) &&
      checked(14,!state_->job || state_->job->execution_quiescent) &&
      checked(15,!counted_work) &&
      checked(16,state_->active.empty()) && checked(17,state_->expected.empty()) &&
      checked(18,!state_->listeners || !state_->listeners->lost()) &&
      checked(19,!state_->wake ||
          (checked(20,!state_->wake->lost()) && checked(21,state_->wake->Empty()))) &&
      checked(22,!state_->identity_service || state_->IdentityServiceJoined(state_->identity_service->data_->job)));
 }catch(...){if(observation)observation->exception=1;
  return FinishIdentityStopObservation(observation,false);}
}
bool NativeProfileIdentityOwner::StopPeriodicOnBootstrap(XThread& bootstrap) noexcept {
 auto owner=shared_from_this();auto& s=*state_;
 if(ActualThread()!=&bootstrap || bootstrap.kernel_state()!=s.kernel ||
    bootstrap.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    bootstrap.thread_id()!=s.config.allocation->bootstrap_thread_id() ||
    global_critical_region::is_held_by_current_thread())return false;
 s.identity_executor_frames.fetch_add(1,std::memory_order_acq_rel);
 struct Exit{State& s;~Exit(){s.identity_executor_frames.fetch_sub(1,std::memory_order_acq_rel);s.changed.notify_all();}} exit{s};
 try{
  const auto* function=s.config.catalog->periodic_service_stop();
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  for(;;){
   uint64_t serial=0;uint32_t object=0;bool pending=false,retained_lifetime=false;
   {
    std::lock_guard lock(s.mutex);
    if(s.retired.load() || !s.stopping.load() || s.tasks.lost())return false;
    for(const auto& row:s.periodic_factories)if(row && !row->stopped){
     if(row->failed)return false;
     if(row->enabling || row->stopping){pending=true;break;}
     if(!row->enabled || !row->birth.generation)return false;
     retained_lifetime=s.tasks.RetainsPeriodicLifetime(*row,row->birth);
     serial=row->serial;object=row->object;break;
    }
    if(!serial && !pending)return true; // No native factory ever issued, or actual stops returned.
   }
   if(pending){
    if(std::chrono::steady_clock::now()>=deadline)return false;
    bootstrap.Delay(0,0,uint64_t(-160000ll));continue;
   }
   if(!function || function->address!=0x819068D0 || !s.config.domain->active() ||
       (!s.config.allocation->IsAccepting() && !retained_lifetime) || !s.PeriodicObjectCurrent(object) ||
       !CheckPeriodicCode(*s.config.catalog->calls()))return false;
   auto expected=std::make_shared<State::Expected>();
   expected->source=kBootUnjoinedSource;expected->thread=retain_object(&bootstrap);
   expected->function=0x819068D0;expected->count=1;expected->args[0]=object;
   {
    std::lock_guard lock(s.mutex);const auto* row=s.Periodic(serial);
    if(!row || row->failed || !row->enabled || row->enabling || row->stopping || row->stopped ||
        s.retired.load() || s.expected.size()>=64 || !s.next_work || s.next_work==UINT64_MAX)return false;
    expected->serial=s.next_work++;s.expected.push_back(expected);
   }
   const std::array<uint32_t,1> args{object};
   const auto actual=s.config.catalog->calls()->ExecuteDeferredSystemFunction(bootstrap,function->identity,args);
   {
    std::lock_guard lock(s.mutex);const auto* row=s.Periodic(serial);
    const auto at=std::find(s.expected.begin(),s.expected.end(),expected);
    if(at==s.expected.end())return false;
    expected->call_returned=true;
    // Void native return residue is never an HRESULT. Only real body/ownreturn,
    // exact Cancel/Wait/ownerClose and original object postconditions prove stop.
    expected->healthy=actual.succeeded && expected->entered && expected->returned &&
        row && row->stopped && !row->failed;
    if(!expected->healthy){s.Lose(__LINE__);return false;}
    s.retired_expected.push_back(expected);s.expected.erase(at);
   }
   // Other workers may still be leaving the genuine dispatcher epilog. The
   // existing host drain retains those whole Work frames; no hook wait/fake exit.
  }
 }catch(...){s.Lose(__LINE__);return false;}
}
bool NativeProfileIdentityOwner::StopOnBootstrap(XThread& bootstrap) noexcept {
 if(ActualThread()!=&bootstrap || bootstrap.kernel_state()!=state_->kernel ||
    bootstrap.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    global_critical_region::is_held_by_current_thread())return false;
 BeginStop();
 // Stop is not completion. Entered/uncertain native requests and pending
 // listener/provider/renderer generations remain owned until factual drain.
 return StepOnBootstrap(bootstrap) && Drained();
}
bool NativeProfileIdentityOwner::RetainedForActualKernelStop(StopObservation* observation) const noexcept {
 if(observation)*observation={};
 try{
  const auto& s=*state_;std::lock_guard lock(s.mutex);
  const auto failed=[&](uint32_t bit,bool value){
   ObserveIdentityStopConjunct(observation,bit,!value);return value;
  };
  // Full non-fiber-yielding HLE frames may finish during the real worker join.
  // Their honest total stays live; only sealed+zero parkable frames are safe
  // here. Drained and factual post-stop retirement still require total zero.
  if(failed(0,!s.stopping.load(std::memory_order_acquire)) || failed(1,s.retired.load()) ||
     failed(2,!s.hook_rundown->Capture().retain_until_worker_join()) || failed(3,s.public_calls.load(std::memory_order_acquire)) ||
     failed(4,s.auxiliary_host_active.load(std::memory_order_acquire)) ||
     failed(5,s.identity_executor_frames.load(std::memory_order_acquire)) ||
     failed(6,s.listener_object_frames.load(std::memory_order_acquire)) ||
     failed(7,s.task_close_frames.load(std::memory_order_acquire)) || failed(8,!s.kernel) ||
     failed(9,!s.config.catalog) || failed(10,!s.config.domain) || failed(11,!s.config.allocation) ||
     failed(12,s.profile_enabled && !s.config.factory_binding) || failed(13,!s.root_code) ||
     failed(14,!s.resident_domain))return FinishIdentityStopObservation(observation,false);
  if(failed(15,std::any_of(s.counted.begin(),s.counted.end(),[&](const auto& row){
    if(observation)++observation->visited[0];IdentityStopRowObservation r;
    const bool bad=r.Test(0,bool(row)) &&
        ((r.Test(1,!row->allocated) && r.Test(2,!row->failed)) ||
         (r.Test(3,row->delete_entered) && r.Test(4,!row->free_returned)));
    if(bad)r.Rejected(observation,1);return bad;
   })))return FinishIdentityStopObservation(observation,false);
  // Unreturned Ob primitive/deleter host frames are not covered.
  for(const auto& d:s.task_references){
   if(observation)++observation->visited[2];IdentityStopRowObservation r;
   const bool bad=r.Test(0,bool(d)) &&
       (r.Test(1,d->bound.load(std::memory_order_acquire)) ||
        (r.Test(2,d->reaping.load(std::memory_order_acquire)) &&
         r.Test(3,!d->resources_retired.load(std::memory_order_acquire))) ||
        (r.Test(4,!d->resources_retired.load(std::memory_order_acquire)) &&
         (r.Test(5,!d->thread) || r.Test(6,!d->code) || r.Test(7,!d->domain))));
   if(bad){r.Rejected(observation,4);failed(16,true);
    return FinishIdentityStopObservation(observation,false);}
  }
  failed(16,false);
  // A live native modal wait has no proved heap-only host-frame coverage.
  // Its genuine return is required; never cancel/Complete/Close it by fiat.
  for(const auto& work:s.execution_retained){
   if(observation)++observation->visited[3];IdentityStopRowObservation r;
   const bool bad=r.Test(0,!work) || r.Test(1,!work->normal_return) ||
       r.Test(2,work->execution_read.pending());
   if(bad){r.Rejected(observation,5);failed(17,true);
    return FinishIdentityStopObservation(observation,false);}
  }
  failed(17,false);
  for(const auto& work:s.active){
   if(observation)++observation->visited[4];IdentityStopRowObservation r;
   // A neutral hook has already returned to original PPC. Its heap-only
   // metadata/code/thread owners remain retained until the separate real
   // CPU-stop barrier; this is not a native callback/Task completion receipt
   // and does not certify an arbitrary active HLE/public/IO frame.
   if(r.Test(0,bool(work)) && r.Test(1,work->execution_only)){
    if(r.Test(2,!work->thread) || r.Test(3,!work->domain) ||
       r.Test(4,!work->frame.valid()) || r.Test(5,work->frame.root!=s.root)){
     r.Rejected(observation,6);failed(18,true);
     return FinishIdentityStopObservation(observation,false);
    }
    continue;
   }
   if(r.Test(6,!work) || r.Test(7,work->transport_unclaimed) || r.Test(8,work->transport_only) ||
      r.Test(9,work->creation_only) || r.Test(10,work->execution_options) ||
      r.Test(11,bool(work->profile_control)) || r.Test(12,bool(work->periodic_factory)) ||
      r.Test(13,work->task_release) || r.Test(14,work->foreign_task_cleanup) ||
      r.Test(15,work->foreign_avatar_task) || r.Test(16,!work->thread) ||
      r.Test(17,!work->domain) || r.Test(18,!work->frame.valid()) ||
      r.Test(19,work->frame.root!=s.root) ||
      (r.Test(20,work->frame.function==0x816F3A40) && r.Test(21,!work->prebody_parked))){
    r.Rejected(observation,7);failed(18,true);
    return FinishIdentityStopObservation(observation,false);
   }
  }
  failed(18,false);
  return FinishIdentityStopObservation(observation,
      ObserveIdentityStopConjunct(observation,19,s.PeriodicStopped()) &&
      ObserveIdentityStopConjunct(observation,20,s.IdentityServiceRetainable()));
 }catch(...){if(observation)observation->exception=1;
  return FinishIdentityStopObservation(observation,false);}
}
bool NativeProfileIdentityOwner::RetireAfterActualKernelStop() noexcept {
 // Private Service-only all-CPU/GPU-stopped lane. No PPC, GuestStatus,
 // timeout, ordinary Leave failure or BeginStop flag can invoke this proof.
 if(global_critical_region::is_held_by_current_thread())return false;
 try{
  std::vector<std::shared_ptr<State::Work>> active,retired,execution_retained;
  std::vector<std::shared_ptr<State::ExecutionLifetime>> execution_lifetimes;
  std::vector<std::shared_ptr<State::Expected>> expected,retired_expected;
  std::array<std::shared_ptr<TaskReference::Data>,64> references;
  std::array<std::shared_ptr<State::ContextReference>,64> contexts;
  std::shared_ptr<State::Job> job;
  std::shared_ptr<IdentityServiceOrigin> service;
  std::shared_ptr<xam::ProfileRosterIntent> bootstrap_cleanup;
  std::unique_ptr<Listeners> listeners;
  std::array<std::shared_ptr<State::CountedAllocation>,64> counted;
  std::vector<std::shared_ptr<State::CountedAllocation>> retired_counted;
  {
   std::lock_guard lock(state_->mutex);
   if(state_->retired.load(std::memory_order_acquire))return true;
   if(!state_->hook_rundown->Capture().fully_drained() ||
      state_->public_calls.load(std::memory_order_acquire) ||
      state_->identity_executor_frames.load(std::memory_order_acquire) ||
      state_->listener_object_frames.load(std::memory_order_acquire) ||
      state_->task_close_frames.load(std::memory_order_acquire) || state_->auxiliary_host_active.load(std::memory_order_acquire))return false;
   // Scope insertion order is the actual serialized entry order. Reverse it
   // globally, retaining the actual XThreads/ThreadStates throughout; each
   // top CAS restores its real previous scope. A mismatch retains everything.
   for(auto i=state_->active.rbegin();i!=state_->active.rend();++i){
    const auto& work=*i;
    if(work->execution_read_fallback && !state_->RestoreOwnedReadOnUnwind(work))return false;
    if(work->execution_read.pending()){
     const auto parent=work->execution_read_owner;
     auto* cpu=work->thread?work->thread->thread_state():nullptr;
     if(!parent || !parent->read || !cpu ||
        !work->execution_read.RestoreAtActualStop(parent->read.get(),cpu->native_profile_roster_read_))return false;
    }
    if(work->read && !work->read->DetachAtActualKernelStop())return false;
   }
   for(const auto& work:state_->active)if(work->service_cell && !work->service_cell->DetachAtActualStop())return false;
   for(const auto& work:state_->host_retired)if(work->service_cell && !work->service_cell->DetachAtActualStop())return false;
   if(state_->identity_service && state_->identity_service->data_->acceptance_cell &&
       !state_->identity_service->data_->acceptance_cell->DetachAtActualStop())return false;
   if(state_->listeners && !state_->listeners->DetachCellsAtActualKernelStop())return false;
   for(const auto& work:state_->active)if(work->commit){
    auto* cpu=work->thread?work->thread->thread_state():nullptr;
    if(!cpu || cpu!=work->commit->thread_state)return false;
    void* cell=work->commit.get();
    if(!cpu->native_identity_publication_cell_.compare_exchange_strong(cell,nullptr,
        std::memory_order_acq_rel) && cell)return false;
   }
   for(const auto& d:state_->task_references)if(d && d->bound.load(std::memory_order_acquire)){
    if(!d->cpu)return false;
    void* exact=d.get();
    if(!d->cpu->native_identity_task_reference_cell_.compare_exchange_strong(exact,nullptr,
        std::memory_order_acq_rel) && exact)return false;
    d->bound.store(false,std::memory_order_release);
   }
   state_->DetachTaskDiagnosticsAtActualStop();
   references.swap(state_->task_references);contexts.swap(state_->context_references);
   state_->retired.store(true,std::memory_order_release);
   state_->stopping.store(true,std::memory_order_release);
   // Whole original-caller/PM-use frames genuinely returned before this cut.
   active.swap(state_->active);retired.swap(state_->host_retired);
   execution_lifetimes.swap(state_->execution_lifetimes);
   execution_retained.swap(state_->execution_retained);
   expected.swap(state_->expected);retired_expected.swap(state_->retired_expected);
   service=std::move(state_->identity_service);
   if(service)service->data_->request.RetireAtActualStop();
   job=std::move(state_->job);bootstrap_cleanup=std::move(state_->bootstrap_cleanup);
   listeners=std::move(state_->listeners);
   counted.swap(state_->counted);retired_counted.swap(state_->retired_counted);
   if(state_->wake)state_->wake->RetireAtActualKernelStop();
  }
  if(service){NativeXmsgProvenance::ReleaseIdentityService(service->data_->link);
   service->data_->ReleaseResourcesOnHost();}

  // Actual stopped guest stacks may still contain local shared_ptr copies.
  // Revoke resources INSIDE their heap holders before dropping owner vectors;
  // escaped metadata can never keep a live ThreadState/domain/backing owner.
  const auto release_work=[](const std::shared_ptr<State::Work>& work){
   if(!work)return;
   work->task_diagnostic=nullptr;
   if(work->service_cell)work->service_cell->thread.reset();
   work->service_cell.reset();work->service_origin.reset();work->service_tick.reset();
   work->read.reset();work->read_pool=nullptr;
   if(work->commit){work->commit->intent.reset();work->commit->publication.reset();
    work->commit->owner=nullptr;work->commit->wake=nullptr;work->commit->context=nullptr;
    work->commit->thread_state=nullptr;work->commit->request_flags=nullptr;
    work->commit->native_title_context=nullptr;work->commit->input_flags={};
    work->commit->native_flags={};work->commit->input_xuids={};work->commit->native_xuids={};}
   if(work->event_lifetime){work->event_lifetime->intent.reset();work->event_lifetime->domain={};
    work->event_lifetime->code.reset();work->event_lifetime->thread.reset();}
   work->commit.reset();work->event_lifetime.reset();work->listener_source.reset();
   work->listener_dependencies.reset();work->listener_parent.reset();work->listener_frame.reset();work->listener_cell=nullptr;
   work->listener_allocation.reset();
   work->tail_reschedule.reset();work->tail_wake.reset();work->presentation_guard.reset();
   work->creation_parent.reset();work->creation.reset();work->reference_release.reset();
   work->execution_target.reset();work->execution_lifetime.reset();
   work->execution_read_owner.reset();work->execution_wait_parent.reset();work->execution_producer.reset();
   work->caller_code.lease.reset();work->caller_code.module.reset();
   if(work->avatar_allocation)work->avatar_allocation->RetireAfterActualKernelStop();
   work->avatar_allocation.reset();
   work->domain={};work->thread.reset();
  };
  for(const auto& work:active)release_work(work);
  for(const auto& work:retired)release_work(work);
  for(const auto& work:execution_retained)release_work(work);
  for(const auto& call:expected)call->thread.reset();
  for(const auto& call:retired_expected)call->thread.reset();
  for(const auto& d:contexts)if(d){
   d->receipt.Retire();d->targets={};d->domain={};d->code.reset();d->thread.reset();d->cpu=nullptr;d->context=nullptr;
  }
  for(const auto& d:references)if(d){
   d->retired.store(true,std::memory_order_release);
   d->receipt.phase.store(Tasks::ReferenceReceipt::Phase::kRetired,std::memory_order_release);
   d->ReleaseResourcesOnHost();
  }
  if(job){
   if(job->owned_input.exchange(false) && job->input)state_->kernel->memory()->SystemHeapFree(job->input);
   job->input=0;job->caller_backing.reset();job->overlap_backing.reset();
   job->old_reads={};job->writer_reads={};job->old_ticket.reset();job->writer_ticket.reset();
   job->original_render_frame.reset();
   for(const auto& worker:job->prebody_workers)if(worker){
    worker->thread.reset();worker->code.reset();worker->domain={};
   }
   job->prebody_workers={};
   job->publication.reset();job->joined.reset();job->intent.reset();job->original_caller.reset();
   job->caller_code.clear();job->apc_code.reset();job->domain={};
   job->old_event_source.reset();job->event_source.reset();
   job->old_pixels.reset();job->hidden_pixels.reset();job->new_pixels.reset();
  }
  for(const auto& row:counted)if(row){row->creator.reset();row->domain={};}
  for(const auto& row:retired_counted)if(row){row->creator.reset();row->domain={};}
  // No native Close/free/body-return fact is added. The same actual Kernel and
  // Memory remain alive throughout these host releases, outside all locks.
  active.clear();retired.clear();execution_retained.clear();expected.clear();retired_expected.clear();
  for(const auto& lifetime:execution_lifetimes)if(lifetime)lifetime->targets.clear();
  execution_lifetimes.clear();
  listeners.reset();job.reset();bootstrap_cleanup.reset();
  state_->boot_source.reset();state_->boot_origin.reset();
  counted={};retired_counted.clear();state_->providers={};
  // Actual all-CPU/GPU-stop retires only these value observations. It does
  // not fabricate missing native calls, reference releases or healthy drain.
  state_->image_calls={};state_->image_strings={};state_->image_defaults={};
  state_->images.RetireMetadataAfterActualStop();
  state_->boot_reads={};state_->current_ticket.reset();state_->current_roster.reset();
  state_->resident_domain={};state_->root_code.reset();state_->provider_binding.reset();
  state_->prepared_title={};state_->apc_code_until_actual_stop.clear();
  state_->current_event_source.reset();state_->current_event_origin.reset();
  state_->parked_render_thread.reset();state_->pixels.reset();state_->continuation.reset();
  state_->config={};state_->kernel=nullptr;state_->native_pending=nullptr;
  state_->changed.notify_all();return true;
 }catch(...){return false;}
}

GuestRuntime::ExecutionResult NativeProfileIdentityOwner::ExecuteBootstrapInputAuxiliary(
    GuestRuntimeCallExecutor& calls,XThread& thread,const GuestRuntime::Function& function,
    std::span<const uint32_t> args) {
 const auto owner=Lookup(calls.kernel());
 if(!owner){
  if(calls.kernel()->native_profile_identity_claimed_.load(std::memory_order_acquire))
   return {false,kAbort,"Claimed native identity bootstrap owner absent"};
  return calls.Execute(thread,function,args,true); // Ordinary Guide-only family.
 }
 auto& s=*owner->state_;auto expected=std::make_shared<State::Expected>();
 if(function.address!=0x816F44D0 || function.identity!="native-system-input-aux-init" || !args.empty() ||
    ActualThread()!=&thread || thread.kernel_state()!=s.kernel ||
    thread.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
    thread.thread_id()!=s.config.allocation->bootstrap_thread_id() ||
    s.config.catalog->calls().get()!=&calls || global_critical_region::is_held_by_current_thread())
  return {false,kAbort,"Not the exact actual auxiliary bootstrap ingress"};
 {
  std::lock_guard lock(s.mutex);
  if(s.auxiliary_claimed || s.stopping.load() || s.retired.load() || s.uncertain.load() ||
     s.expected.size()>=64 || !s.next_work || s.next_work==UINT64_MAX)
   return {false,kAbort,"Auxiliary bootstrap is once-only and sealed"};
  s.auxiliary_claimed=true;expected->serial=s.next_work++;expected->source=kBootUnjoinedSource;
  expected->thread=retain_object(&thread);expected->function=0x816F44D0;expected->count=0;
  s.expected.push_back(expected);s.auxiliary_host_active.store(true,std::memory_order_release);
 }
 struct HostExit{State& s;~HostExit(){s.auxiliary_host_active.store(false,std::memory_order_release);s.changed.notify_all();}} host_exit{s};
 auto result=calls.Execute(thread,function,args,true);
 {
  auto memory_read=global_critical_region::AcquireDirect();std::lock_guard lock(s.mutex);
  const auto i=std::find(s.expected.begin(),s.expected.end(),expected);
  if(i==s.expected.end() || expected->call_returned || (expected->entered && !expected->returned)){
   s.Lose(__LINE__);return {false,kAbort,"Auxiliary bootstrap execution did not return normally"};
  }
  expected->call_returned=true;expected->healthy=!expected->entered || expected->returned;
  s.auxiliary_returned=expected->entered && expected->returned;
  const auto bound=[&](uint32_t output,uint32_t callback,bool persistent){
   const auto tagged=Word(*s.kernel->memory(),output);
   const auto* node=tagged?s.tasks.FindLiveTask(*tagged&~1u):nullptr;
   return tagged && (*tagged&1) && node && node->published && node->callback==callback &&
       node->birth.origin==kBootUnjoinedSource && node->created_persistent==persistent &&
       (persistent?(node->persistent_callback_bound && node->persistent_output==output):node->resident_physical);
  };
  s.auxiliary_ready=s.auxiliary_returned && result.succeeded &&
      bound(0x81AAC270,0x816F2618,true) && bound(0x81AAC274,0x816F3A40,true) &&
      bound(0x81AAC29C,0x816F4298,false) && bound(0x81AAC298,0x816F4410,false) && bool(s.wake);
  s.retired_expected.push_back(*i);s.expected.erase(i);
  if(result.succeeded && !s.auxiliary_ready)
   result={false,result.return_value,"Auxiliary returned without all four actual Task/publication owners"};
 }
 s.changed.notify_all();return result;
}

uint64_t NativeProfileIdentityOwner::EnterFiniteInputBoundary(void* raw,uint32_t at) noexcept {
 State::HookUse hook_use;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto& s=*state_;
 try{
  if(!hook_use.Acquire(s.hook_rundown,state_,true)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  // Seal and count share one cut. A late unentered tick really refuses
  // before frame/allocation access; it cannot park after the stop handoff.
  if(!hook_use.may_park())return s.Deny(c,at,__LINE__);
  const auto frame=s.ReadFrame(c,at);
  const auto task=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c);
  auto* thread=ActualThread();
  if(!frame || !task || !thread || !DwordIs(c.r[3],uint32_t(c.r[3])) ||
     (at==0x816F4088 && (!DwordIs(c.r[4],uint32_t(c.r[4])) || c.r[3]>15 || c.r[4]>15))){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  auto work=std::make_shared<State::Work>();work->frame=*frame;work->thread=retain_object(thread);
  work->domain=s.config.domain->Acquire();work->first=uint32_t(c.r[3]);work->second=uint32_t(c.r[4]);
  work->physical_work=true;work->prebody_parked=true;work->source=kBootUnjoinedSource;
  {
   auto memory_read=global_critical_region::AcquireDirect();std::lock_guard lock(s.mutex);
   if(s.stopping.load() || s.retired.load() || s.uncertain.load() || !work->domain ||
      s.active.size()>=256 || !s.next_work || s.next_work==UINT64_MAX ||
      std::any_of(s.active.begin(),s.active.end(),[&](const auto& old){return old->frame==*frame;}))return s.Deny(c,at,__LINE__);
   if(at==0x816F4088){
    const auto parent=frame->lr==0x816F437C?s.Latest(frame->thread,0x816F4298):
        frame->lr==0x816F44B0?s.Latest(frame->thread,0x816F4410):nullptr;
    const auto delta=frame->lr==0x816F437C?0xE0u:0x90u;
    if(!parent || !parent->physical_work || !parent->owns_persistent_work ||
       parent->birth.task!=*task || uint64_t(frame->sp)+delta!=parent->frame.sp){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    const auto birth=s.tasks.BeginExternalWork(*frame,*task);
    if(!birth){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}work->birth=work->external_birth=*birth;
   }else{
    const auto* node=s.tasks.FindLiveTask(*task);
    if(!node || node->persistent_output!=0x81AAC274 || node->birth.origin!=kBootUnjoinedSource){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    const auto birth=s.tasks.BeginPersistentWork(*frame,*task,work->first);
    if(!birth){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}work->birth=*birth;work->owns_persistent_work=true;
   }
   work->serial=s.next_work++;s.active.push_back(work);
  }
  for(;;){
   // Revalidate actual thread/Task outside the owner's mutex: ReadFrame itself
   // checks nested original-caller ownership under that same mutex.
   const auto current_frame=s.ReadFrame(c,at);
   const auto current=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c);
   {
    auto memory_read=global_critical_region::AcquireDirect();std::lock_guard lock(s.mutex);
    if(s.stopping.load() || s.retired.load() || s.uncertain.load()){
     // Original body has not entered. End only this metadata reservation;
     // no native callback return/Task Close/event completion is invented.
     const bool canceled=work->owns_persistent_work?s.tasks.CancelPersistentBeforeNative(*frame,work->birth):
         s.tasks.CancelExternalBeforeNative(*frame,work->birth);
     if(!canceled){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     const auto retiring=std::find(s.active.begin(),s.active.end(),work);
     if(retiring==s.active.end()){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     s.host_retired.push_back(std::move(work));s.active.erase(retiring);return s.Deny(c,at,__LINE__);
    }
    const bool open=s.render_phase==State::RenderPhase::kOpen ||
        s.render_phase==State::RenderPhase::kHiddenRefresh;
    if(open){
     const auto* node=current?s.tasks.FindLiveTask(*current):nullptr;
     if(ActualThread()!=thread || !current_frame || *current_frame!=*frame ||
        !DwordIs(c.r[3],work->first) || (at==0x816F4088 && !DwordIs(c.r[4],work->second)) ||
        !node || node->birth.generation!=work->birth.generation ||
        node->birth.task!=work->birth.task){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     const bool prepared=s.job && s.render_phase==State::RenderPhase::kHiddenRefresh;
     work->source=prepared?s.job->source:s.current_read_source;
     auto* pool=prepared?&s.job->writer_reads:&s.boot_reads;
     const auto slot=std::find_if(pool->begin(),pool->end(),[](const auto& read){return bool(read);});
     if(slot==pool->end()){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     auto lifetime=std::make_shared<State::EventLifetime>();lifetime->code=s.root_code;
     lifetime->domain=s.config.domain->Acquire();lifetime->thread=work->thread;
     if(prepared)lifetime->intent=s.job->intent;
     if(!lifetime->domain){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     work->event_lifetime=lifetime;
     const Events::Root root{s.root.kernel,s.root.domain,s.root.generation};
     const Events::WorkKey key{root,node->birth.generation,work->serial,frame->thread,frame->pcr,
         node->birth.task,frame->sp,frame->lr,at};
     work->listener_parent=s.SynchronousListenerParent(*work,key);
     work->listener_source=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(
         key,s.listeners->RetainEarlierSource(work->source),lifetime,
         work->listener_parent?work->listener_parent->listener_source:nullptr));
     work->listener_work=s.listeners->AdoptWholeWork(work->listener_source);
     if(!work->listener_work){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     work->read_pool=pool;work->read=std::move(*slot);
     work->physical_work=false;work->prebody_parked=false;
     if(!work->read->Enter(s.kernel,thread,c.thread_state,node->birth.task,node->birth.generation,
         work->serial,s.config.catalog->calls()->module()->hmodule_ptr(),s.root.generation)){
      s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
     }
     return 1;
    }
   }
   // No owner/native/global-critical lock crosses this actual same-Task pause.
   thread->Delay(0,0,uint64_t(-10000ll));
  }
 }catch(...){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
}

// The descriptor's finite SYSTEM frame owns reads independently of any native
// Task containing it. Its code/domain/XThread/frame are held BEFORE the body
// (and its object mutex/GetNext) until this same body genuinely returns. This
// reuses the same roster scope stack, read pools and whole-consumer ledger.
uint64_t NativeProfileIdentityOwner::EnterFiniteSystemThreadBoundary(void* raw,uint32_t at) noexcept {
 State::HookUse hook_use;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto& s=*state_;
 try{
  if(!hook_use.Acquire(s.hook_rundown,state_,true)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  // Seal and count share one cut. A late unentered tick really refuses
  // before frame/allocation access; it cannot park after the stop handoff.
  if(!hook_use.may_park())return s.Deny(c,at,__LINE__);
  // A legitimate stop can seal allocation admission before the next tick.
  // This body has not entered: refuse it without poisoning an owned frame.
  if(s.stopping.load(std::memory_order_acquire) || s.retired.load(std::memory_order_acquire))
   return s.Deny(c,at,__LINE__);
  if(global_critical_region::is_held_by_current_thread()){
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  const auto* consumer=FindNativeFiniteSystemConsumer(at);
  auto* thread=ActualThread();const auto frame=s.ReadFrame(c,at);
  // Stop may win after the early check but before ReadFrame's allocation
  // check. No native body/read/consumer was admitted by this failed capture.
  if(!frame && (s.stopping.load(std::memory_order_acquire) || s.retired.load(std::memory_order_acquire)))
   return s.Deny(c,at,__LINE__);
  if(!consumer || !thread || !frame || frame->lr==0xBCBCBCBC ||
     thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM){
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  std::array<uint32_t,3> arguments{};
  for(uint32_t i=0;i<consumer->argument_count;++i){
   if(!DwordIs(c.r[i+3],uint32_t(c.r[i+3]))){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   arguments[i]=uint32_t(c.r[i+3]);
  }
  if(consumer->object_r3 && (arguments[0]<0x1000 || (arguments[0]&3))){
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  const auto ambient=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c);
  auto work=std::make_shared<State::Work>();work->frame=*frame;work->thread=retain_object(thread);
  work->system_frame_ambient_task=ambient.value_or(0);
  work->domain=s.config.domain->Acquire();
  work->first=arguments[0];work->second=arguments[1];work->output=arguments[2];
  work->system_thread_frame=true;work->physical_work=true;work->prebody_parked=true;
  {
   std::lock_guard lock(s.mutex);
   if(s.stopping.load() || s.retired.load() || s.uncertain.load() || !work->domain ||
      s.active.size()>=256 || !s.next_work || s.next_work==UINT64_MAX ||
      std::any_of(s.active.begin(),s.active.end(),[&](const auto& old){return old->frame==*frame;}))
    return s.Deny(c,at,__LINE__);
   if(ambient)if(const auto* node=s.tasks.FindLiveTask(*ambient)){
    work->system_frame_ambient_birth=node->birth;
    work->system_frame_ambient_periodic=bool(node->periodic_factory);
   }
   // No external Task duty is registered while this unentered frame parks.
   work->serial=s.next_work++;s.active.push_back(work);
  }
  for(;;){
   std::shared_ptr<IdentityServiceOrigin> service;
   {std::lock_guard lock(s.mutex);service=s.identity_service;}
   if(at==0x818CEE08 && s.TryAdoptIdentityServiceTick(c,*frame,service,work,false))return 1;
   const bool code=CheckFiniteSystemThreadCode(*s.config.catalog->calls(),at);
   const auto current=s.ReadFrame(c,at);
   const auto current_ambient=NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c);
   {
    std::lock_guard lock(s.mutex);
    if(s.stopping.load() || s.retired.load() || s.uncertain.load()){
     // The body never entered: withdraw only its prebody metadata reservation.
     // No Task, native callback return or event consumption is fabricated.
     const auto retiring=std::find(s.active.begin(),s.active.end(),work);
     if(retiring==s.active.end()){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     s.host_retired.push_back(std::move(work));s.active.erase(retiring);return s.Deny(c,at,__LINE__);
    }
    // Match the real render iteration's current-ticket policy: draining
    // still admits bounded OLD reads until the actual ClosingOld cut seals.
    const bool open=s.render_phase==State::RenderPhase::kOpen ||
        s.render_phase==State::RenderPhase::kDrainingOld ||
        s.render_phase==State::RenderPhase::kHiddenRefresh;
    if(open){
     bool same_arguments=true;
     for(uint32_t i=0;i<consumer->argument_count;++i)
      same_arguments=same_arguments && DwordIs(c.r[i+3],arguments[i]);
     const auto* ambient_node=current_ambient?s.tasks.FindLiveTask(*current_ambient):nullptr;
     const bool observed_ambient=bool(work->system_frame_ambient_birth.generation);
     if(!code || ActualThread()!=thread || !current || *current!=*frame || !same_arguments ||
        current_ambient.value_or(0)!=work->system_frame_ambient_task ||
        (observed_ambient && (!ambient_node ||
         !Tasks::SameBirth(ambient_node->birth,work->system_frame_ambient_birth) ||
         bool(ambient_node->periodic_factory)!=work->system_frame_ambient_periodic))){
      s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
     }
     const bool prepared=s.job && s.render_phase==State::RenderPhase::kHiddenRefresh;
     work->source=prepared?s.job->source:s.current_read_source;
     auto* pool=&s.boot_reads;
     if(s.job)pool=prepared?&s.job->writer_reads:&s.job->old_reads;
     const auto slot=std::find_if(pool->begin(),pool->end(),[](const auto& read){return bool(read);});
     if(slot==pool->end()){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     auto lifetime=std::make_shared<State::EventLifetime>();lifetime->code=s.root_code;
     lifetime->domain=s.config.domain->Acquire();lifetime->thread=work->thread;
     if(prepared)lifetime->intent=s.job->intent;
     if(!lifetime->domain){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     work->event_lifetime=lifetime;
     const Events::Root root{s.root.kernel,s.root.domain,s.root.generation};
     const bool borrow=observed_ambient && !work->system_frame_ambient_periodic;
     const auto native_birth=work->system_frame_ambient_birth;
     const Events::WorkKey key{root,borrow?native_birth.generation:0,work->serial,frame->thread,frame->pcr,
         borrow?native_birth.task:0,frame->sp,frame->lr,at,
         borrow?Events::WorkOwnerKind::kTask:Events::WorkOwnerKind::kSystemThreadFrame};
     const auto prior=s.EarlierListenerParent(frame->thread);
     if(prior){
      const bool prior_observed=prior->birth.generation || prior->system_frame_ambient_birth.generation;
      const bool unknown_ambient=(work->system_frame_ambient_task && !observed_ambient) ||
          (prior->system_thread_frame && prior->system_frame_ambient_task && !prior->system_frame_ambient_birth.generation);
      if(unknown_ambient){
       // Independent finite-frame authority is valid without genealogy; it
       // cannot downgrade an already observed parent's required ancestry.
       if(prior_observed){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
      }else{
       const bool same_ambient=!prior->system_thread_frame ||
           (prior->system_frame_ambient_task==work->system_frame_ambient_task &&
            bool(prior->system_frame_ambient_birth.generation)==observed_ambient &&
            (!observed_ambient || Tasks::SameBirth(prior->system_frame_ambient_birth,native_birth)));
       if(prior->source!=work->source || !same_ambient){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
       work->listener_parent=s.SynchronousListenerParent(*work,key);
       if(!work->listener_parent){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
      }
     }
     if(borrow){
      const auto birth=s.tasks.BeginExternalWork(*frame,native_birth.task);
      if(!birth || !Tasks::SameBirth(*birth,native_birth)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
      work->birth=work->external_birth=*birth;
     }
     work->listener_source=std::shared_ptr<const NativeIdentityEventWorkSource>(new NativeIdentityEventWorkSource(
         key,s.listeners->RetainEarlierSource(work->source),lifetime,
         work->listener_parent?work->listener_parent->listener_source:nullptr));
     work->listener_work=s.listeners->AdoptWholeWork(work->listener_source);
     if(!work->listener_work){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     work->read_pool=pool;work->read=std::move(*slot);
     work->physical_work=false;work->prebody_parked=false;
     if(!work->read->EnterSystemThreadFrame(s.kernel,thread,c.thread_state,
         frame->sp,frame->lr,at,work->serial,
         s.config.catalog->calls()->module()->hmodule_ptr(),s.root.generation)){
      s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
     }
     return 1;
    }
   }
   // Sealed phases park BEFORE guest entry: no owner/native/GCRC lock held.
   thread->Delay(0,0,uint64_t(-10000ll));
  }
 }catch(...){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
}

uint64_t NativeProfileIdentityOwner::GuardEntry(void* raw,uint64_t address) noexcept {
 if(address<=UINT32_MAX && CoversIdentityServiceBoundary(raw,uint32_t(address),false)){ClearTaskAllocationCut();return 1;}
 // Pre-entry revocation and post-admission receipts are outside ALL original
 // Core guards/locals. No native instruction runs between these exact cuts.
 if(address==0x816B7490)ObserveContextReference(raw,3);
 if(address==0x816B7F30)ObserveContextReference(raw,1);
 const auto admitted=GuardEntryCore(raw,address);
 if(admitted){
  if(address==0x816B7490)ObserveContextReference(raw,10);
  if(address==0x816B7F30)ObserveContextReference(raw,11);
 }
 return admitted;
}

uint64_t NativeProfileIdentityOwner::GuardEntryCore(void* raw,uint64_t address) noexcept {
 State::HookUse hook_use;
 ClearTaskAllocationCut();
 if(!raw || address>UINT32_MAX)return 1;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto at=uint32_t(address);
 if(ObservesNativeIdentityImage(at)){ObserveImageEntry(raw,at);return 1;}
 if(at==0x816E6AB0)ObserveImageAllocationFree(raw);
 if(at==0x816BA3C0){ObserveContextReference(raw,0);return 1;}
 if(at==0x816B8098 || at==0x816B7F30){
  auto* d=c.thread_state?static_cast<TaskReference::Data*>(
      c.thread_state->native_identity_task_reference_cell_.load(std::memory_order_acquire)):nullptr;
  return !d || d->Enter(c,at)?1:Abort(c);
 }
 if(at==0x816BD420)return PublishPreboundNativeCache(raw);
 if(at==0x816DC628)return EnterPreboundNativePublisher(raw);
 // These two entries may already own a native listener lock. Consult ONLY
 // the prebound scalar cell; neither registry lookup nor owner refcount runs.
 if(at==0x816A8328 || at==0x816A8648){
  auto* cell=c.thread_state?static_cast<Listeners::FastCell*>(
      c.thread_state->native_identity_listener_cell_.load(std::memory_order_acquire)):nullptr;
  return !cell || Listeners::FastEnter(cell,ListenerRegisters(c),at)?1:Abort(c);
 }
 if((at==0x816A2CF0 || at==0x816A2A48) && NativeXmsgProvenance::Lookup(c.kernel_state))return 1;
 if(at==0x8199E148 || at==0x8199C838){
  try{
   if(const auto owner=Lookup(c.kernel_state)){
    auto& s=*owner->state_;
    if(!hook_use.Acquire(s.hook_rundown,owner->state_,true)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    if(s.retired.load(std::memory_order_acquire) || !s.kernel || !s.config.catalog)
     return s.Deny(c,at,__LINE__);
    if(s.kernel->GetUserModuleForAddress(at).get()==s.config.catalog->calls()->module().get() &&
       !s.AwaitPreparedWorker(c,at))return s.Deny(c,at,__LINE__);
   }
  }catch(...){if(const auto owner=Lookup(c.kernel_state))owner->state_->Lose(__LINE__);return Abort(c,at);}
 }
 if(ObserveExecutionEntry(raw,at))return 1;
 try{
  auto owner=Lookup(c.kernel_state);
  if(!owner && CommonTaskFunction(at))owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);
  if(!owner)return 1;
  auto& s=*owner->state_;
  if(s.retired.load(std::memory_order_acquire))return s.Deny(c,at,__LINE__);
  if(at==0x818CEE08)if(const auto service=owner->TryEnterIdentityServiceTick(raw,at))return *service;
  const bool parkable=at==0x816F4088 || at==0x816F3A40 || bool(FindNativeFiniteSystemConsumer(at));
  if((!hook_use && !hook_use.Acquire(s.hook_rundown,owner->state_,parkable)) ||
     !hook_use.Matches(s.hook_rundown.get(),owner->state_.get())){
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  if(s.kernel->GetUserModuleForAddress(at).get()!=s.config.catalog->calls()->module().get())return 1;
  if(at==0x816F4088 || at==0x816F3A40)return owner->EnterFiniteInputBoundary(raw,at);
  if(FindNativeFiniteSystemConsumer(at)){
   const auto* actual=ActualThread();
   if(actual && actual->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM)return 1;
   return owner->EnterFiniteSystemThreadBoundary(raw,at);
  }
  // hook_use owns this entire outer frame, including delegated boundaries.
  const bool cleanup=at==0x816A88E8 || at==0x816934F8;
  if(!HooksNativeProfileIdentity(at))return 1;
  const auto* actual_thread=ActualThread();
  const uint32_t actual_thread_id=actual_thread?actual_thread->thread_id():0;
  std::optional<Tasks::CompletionRoute> completion_route;
  if(at==0x816A2588 && actual_thread &&
      actual_thread->native_frame_thread_identity().process_type==X_PROCTYPE_SYSTEM){
   // CompleteIO belongs to every native app. Classify the actual owned overlap
   // BEFORE generic identity-only caller/read admission. Unrelated SYSTEM
   // callers run the original body/result, with a real neutral own-return
   // marker so a nested ordinary call cannot consume an older identity call.
   const auto shape=s.TransportShape(c,at);
   if(!shape){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   auto marker=std::make_shared<State::Work>();marker->frame=*shape;
   marker->thread=retain_object(ActualThread());marker->transport_unclaimed=true;
   marker->physical_work=true;
   std::lock_guard lock(s.mutex);
   completion_route=Tasks::CompletionRoute::Capture(*shape,s.job,
       s.job?s.job->serial:0,s.job?s.job->source:0,s.job?s.job->overlap:0,c.r[3]);
   if(completion_route->kind()==Tasks::CompletionRoute::Kind::kInvalid){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   if(completion_route->kind()==Tasks::CompletionRoute::Kind::kOrdinary){
    marker->completion_route=std::move(completion_route);
    if(s.active.size()>=256 || !s.next_work || s.next_work==UINT64_MAX ||
        std::any_of(s.active.begin(),s.active.end(),[&](const auto& old){return old->frame==*shape;})){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
    marker->serial=s.next_work++;s.active.push_back(marker);return 1;
   }
  }
  std::optional<uint32_t> periodic_handle;
  if((at==0x8190A358 || at==0x819068D0) && s.periodic_object && DwordIs(c.r[3],s.periodic_object)){
   auto read=global_critical_region::AcquireDirect();periodic_handle=Word(*s.kernel->memory(),s.periodic_object+0xC);
  }
  State::TaskClaim task_claim;
  {std::lock_guard lock(s.mutex);task_claim=s.ClaimTaskTransport(at,c,actual_thread_id,periodic_handle);
   if(at==0x816B9AA8 && task_claim.protected_scope && Tasks::DecodeModifyFields(uint32_t(c.r[4])).options_only())
    s.first_modify_options_claim.Capture(s.ModifyOptionsSnapshot(c,task_claim,1,__LINE__));}
  // The typed common inline producer is handled by the same owner below.
  // Avatar's separately retained native Control/Init proof handles its neutral
  // Schedule lane. Capture before Profile/GCRC locks; final holders die
  // before Exit drops hooks. BA440/publication remain neutral/unjoined.
  std::shared_ptr<const NativeAvatarService::CleanupTaskCreationProof> avatar_cleanup;
  if(at==0x816B9A18 && task_claim.neutral_parent && !task_claim.parent)
   avatar_cleanup=NativeAvatarService::CaptureOwnedTaskAllocation(c.kernel_state,c);
  if(task_claim.transport && task_claim.unsupported_descendant){
   if(task_claim.reference_only){
    std::lock_guard lock(s.mutex);
    for(const auto& parent:s.active)if(parent->serial==task_claim.parent && parent->reference_release)
     parent->reference_release->receipt.Fail();
   }
   // An unsupported spawn/modify inside a real profile source stays HOLD;
   // ordinary callback membership never receives that source or read ticket.
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  if((at==0x816B8B08 || at==0x816B9F08 || at==0x816BA058 || at==0x819068D0) && !task_claim.relevant){
   std::lock_guard lock(s.mutex);
   // Unrelated newly observed functions keep their original unowned lifetime.
   // Only a nested equal-function call needs a neutral own-return marker.
   if(!s.Latest(actual_thread_id,at))return 1;
  }
  if(task_claim.transport && !task_claim.relevant){
   // Routing only, not owned admission. Preserve native execution/result even
   // if this observer cannot allocate. Exact markers disambiguate nested
   // ordinary same-function returns from an older genuinely owned operation.
   try{
    const auto shape=s.TransportShape(c,at);
    if(!shape){s.Lose(__LINE__);return 1;}
    auto marker=std::make_shared<State::Work>();marker->frame=*shape;
    marker->thread=retain_object(ActualThread());marker->transport_unclaimed=true;
    marker->physical_work=true; // excluded only from identity readiness work
    std::lock_guard lock(s.mutex);
    if(s.ClaimTaskTransport(at,c,actual_thread_id,periodic_handle)!=task_claim || s.active.size()>=256 ||
       std::any_of(s.active.begin(),s.active.end(),[&](const auto& old){return old->frame==*shape;})){
     s.Lose(__LINE__);return 1;
    }
    marker->serial=s.next_work++;
    if(!marker->serial || !s.next_work){s.Lose(__LINE__);return 1;}
    s.active.push_back(marker);return 1;
   }catch(...){s.Lose(__LINE__);return 1;}
  }
  // Avoid a second owning/metadata lookup while an outer Broadcast/GetNext
  // may already hold native locks. A collision is retained as uncertainty.
  if(ListenerOuter(at) && c.thread_state){
   auto* cell=c.thread_state->native_identity_listener_cell_.load(std::memory_order_acquire);
   if(cell && !(at==0x816A88E8 && DwordIs(c.r[3],uint32_t(c.r[3])) && s.listeners &&
       s.listeners->AllowsReferenceReleaseDestruction(cell,ListenerRegisters(c),uint32_t(c.r[3])))){
    s.Lose(__LINE__);return cleanup?1:s.Deny(c,at,__LINE__);
   }
  }
  if((task_claim.periodic_factory || task_claim.periodic_role!=State::PeriodicRole::kNone) &&
      !CheckPeriodicCode(*s.config.catalog->calls())){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  std::optional<Tasks::PeriodicFrameContext> periodic_context;
  if(task_claim.periodic_factory)if(const auto shape=s.TransportShape(c,at)){
   std::lock_guard lock(s.mutex);
   if(s.ClaimTaskTransport(at,c,actual_thread_id,periodic_handle)==task_claim)
    periodic_context=s.PeriodicContextForEntry(*shape,task_claim);
  }
  State::CodeOwner caller_code;
  const bool avatar_allocation=avatar_cleanup && avatar_cleanup->Matches(c);
  const auto frame=s.ReadFrame(c,at,ListenerOuter(at) || at==0x8199C630 || at==0x8199E3E0 ||
                              task_claim.title_task_lifetime || avatar_allocation,
                              (task_claim.transport || ListenerOuter(at))?&caller_code:nullptr,
                              task_claim.retained_cleanup,periodic_context?&*periodic_context:nullptr,
                              task_claim.periodic_factory);
  if(!frame){
   auto* actual=ActualThread();
   if(actual && actual->kernel_state()==s.kernel &&
      actual->native_frame_thread_identity().process_type==X_PROCTYPE_TITLE &&
      at!=0x8199C630 && at!=0x8199E3E0)return 1;
   s.Lose(__LINE__);return cleanup?1:s.Deny(c,at,__LINE__);
  }
  const bool profile_control_code=!(task_claim.profile_control || task_claim.profile_producer || at==0x8199C838) ||
      CheckProfileControlCode(*s.config.catalog->calls());
  if(!profile_control_code){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  const bool periodic_object_current=task_claim.periodic_role==State::PeriodicRole::kFactory ||
      task_claim.periodic_role==State::PeriodicRole::kStop ?
      s.PeriodicObjectCurrent(uint32_t(c.r[3])):true;
  const auto current_task=NativeProfileIdentityTaskContext::Read(
      *s.config.catalog->calls(),*s.config.domain,at,c);
  const bool avatar_cleanup_current=avatar_cleanup && avatar_cleanup->Matches(c) &&
      s.config.allocation->IsAccepting(); // Still outside Profile metadata lock.
  auto work=std::make_shared<State::Work>();work->frame=*frame;work->caller_code=std::move(caller_code);
  work->completion_route=std::move(completion_route);
  work->task_release=task_claim.retained_cleanup;work->release_generation=task_claim.subject;
  work->release_receipt=task_claim.release_receipt;work->reference_only=task_claim.reference_only;
  work->foreign_task_cleanup=avatar_cleanup_current && avatar_cleanup->IsUnload();
  work->foreign_avatar_task=avatar_cleanup_current || task_claim.title_task_lifetime;
  if(avatar_cleanup_current)work->avatar_allocation=avatar_cleanup;
  work->creation_only=task_claim.creation_only;work->execution_options=task_claim.execution_options;
  work->profile_producer=task_claim.profile_producer;
  work->periodic_factory=task_claim.periodic_factory;work->periodic_role=task_claim.periodic_role;
  work->periodic_context=std::move(periodic_context);
  work->thread=retain_object(ActualThread());work->domain=s.config.domain->Acquire();
  if(at==0x816A8C00){
   const auto args=Listeners::DecodeOuter(at,ListenerRegisters(c));
   work->system_discarded_identity_hint=args && s.DiscardedIdentityHintCaller(*work,*args,0);
  }
  auto memory_read=global_critical_region::AcquireDirect();
  work->first=uint32_t(c.r[3]);work->second=uint32_t(c.r[4]);
  if(!work->domain)return cleanup?1:s.Deny(c,at,__LINE__);
  std::lock_guard lock(s.mutex);
  if(task_claim.transport && s.ClaimTaskTransport(at,c,frame->thread,periodic_handle)!=task_claim){
   s.Lose(__LINE__);return cleanup?1:s.Deny(c,at,__LINE__);
  }
  if(s.retired.load(std::memory_order_acquire) ||
     (s.uncertain.load(std::memory_order_acquire) && !work->task_release &&
      (work->periodic_role==State::PeriodicRole::kNone || work->periodic_role==State::PeriodicRole::kFactory)) ||
     s.active.size()>=256 || std::any_of(s.active.begin(),s.active.end(),
       [&](const auto& old){return old->frame==*frame;})){
   s.Lose(__LINE__);return cleanup?1:s.Deny(c,at,__LINE__);
  }
  const auto earlier=s.Latest(frame->thread);
  work->source=earlier?earlier->source:kBootUnjoinedSource;
  work->transport_only=earlier && earlier->transport_only;
  work->physical_work=(earlier && earlier->physical_work) || at==0x816F44D0 ||
      at==0x816F4298 || at==0x816F4410;
  if(work->periodic_role!=State::PeriodicRole::kNone){
   work->source=kBootUnjoinedSource;work->transport_only=false;work->physical_work=true;
  }
  if(task_claim.profile_control){
   work->source=kBootUnjoinedSource;work->transport_only=false;work->physical_work=true;
   for(const auto& parent:s.active){
    if(parent->serial==task_claim.control_creator)work->control_creator=parent;
    if(parent->serial==task_claim.control_parent)work->control_parent=parent;
   }
   if(!work->control_creator || work->control_creator->normal_return){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }
  if(work->execution_options){
   work->source=kBootUnjoinedSource;work->transport_only=false;work->physical_work=true;
  }
  if(work->creation_only){
   // No ambient source inheritance: the original same-thread read scope stays
   // untouched, while this passive producer/child gets execution identity only.
   work->source=kBootUnjoinedSource;work->transport_only=false;work->physical_work=true;
   if(task_claim.creation_parent){
    for(const auto& parent:s.active)if(parent->serial==task_claim.creation_parent)work->creation_parent=parent;
    if(!work->creation_parent){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   }
  }
  if(frame->lr==0xBCBCBCBC){
   const auto expected=std::find_if(s.expected.begin(),s.expected.end(),[&](const auto& e){
    if(e->entered || e->call_returned || e->thread.get()!=ActualThread() || e->function!=at)return false;
    for(uint32_t i=0;i<e->count;++i)if(!DwordIs(c.r[i+3],e->args[i]))return false;
    return true;
   });
   if(expected==s.expected.end()){
    if(at==0x816BA440)return 1; // Other genuine continuation roles stay unchanged.
    const bool observation_only=at==0x816B9A18 || at==0x816B7490 ||
        at==0x816B7D98 || at==0x816B7500 || at==0x816E6AB0;
    if(!observation_only){s.Lose(__LINE__);return cleanup?1:s.Deny(c,at,__LINE__);}
    // Actual allocation OUTPUT / native release may establish execution/end
    // facts for an already observed generation. An unregistered sentinel is
    // NEVER a profile-read origin, callback bind or cache writer admission.
    work->source=kBootUnjoinedSource;
   }else{
    (*expected)->actual=*frame;(*expected)->entered=true;work->source=(*expected)->source;
    work->transport_only=(*expected)->transport_only;
    work->identity_continuation=(*expected)->identity_continuation;
    work->reference_release=(*expected)->reference_release;
    work->registered_executor=true;
   }
  }
  if(work->reference_only){work->source=kBootUnjoinedSource;work->physical_work=true;
   work->transport_only=false;
   if(!work->reference_release || !work->registered_executor){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }
  if(work->transport_only && (work->source!=kBootUnjoinedSource || work->read || work->commit)){
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  if(at==0x816F44D0 && (!work->registered_executor || !s.auxiliary_claimed || s.auxiliary_returned)){
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  if((at==0x8199C630 || at==0x8199E3E0) &&
     (!s.job || work->source!=s.job->source || !s.job->native_entered))return s.Deny(c,at,__LINE__);
  if(at==0x8199E3E0){
   if(!DwordIs(c.r[4],s.job->flags) || !DwordIs(c.r[5],s.job->overlap)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->output=s.job->overlap; // Original initializer operand, not Task output.
  }
  const auto* captured_node=current_task?s.tasks.FindLiveTask(*current_task):nullptr;
  const bool captured_callback=s.job && captured_node &&
      captured_node->birth.origin==s.job->source && ProfileCallback(at);
  const bool captured_wake=s.job && captured_node && s.wake && at==0x816F2618 &&
      s.wake->ActualPersistent(captured_node->birth) &&
      ((s.wake->phase_==Wake::Phase::kPublished && s.wake->prepared_==s.job->source &&
        s.wake->latest_sequence_>s.wake->last_complete_sequence_) ||
       (!s.job->native_entered && s.native_pending && NativeAtomicWord(s.native_pending)));
  if(s.stopping.load(std::memory_order_acquire) && !s.Latest(frame->thread) &&
     !captured_callback && !captured_wake && frame->lr!=0xBCBCBCBC &&
     at!=0x816B7490 && !cleanup && !work->periodic_factory &&
     !(at==0x816B9A18 && task_claim.neutral_parent && !task_claim.parent &&
       avatar_cleanup_current && avatar_cleanup->IsUnload() && avatar_cleanup->Matches(c)))return s.Deny(c,at,__LINE__);
  if(work->periodic_factory && task_claim.subject){
   const auto* node=s.tasks.FindBirth(task_claim.subject);
   if(!node || node->periodic_factory!=work->periodic_factory){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->birth=node->birth;
  }
  auto* memory=s.kernel->memory();
  if(ListenerOuter(at) || (ProfileCallback(at) && !work->physical_work) || at==0x816921C8){
   work->event_lifetime=std::make_shared<State::EventLifetime>();
   work->event_lifetime->code=s.root_code;
   work->event_lifetime->domain=s.config.domain->Acquire();
   work->event_lifetime->thread=work->thread;
   if(s.job && work->source==s.job->source)work->event_lifetime->intent=s.job->intent;
   if(!work->event_lifetime->domain){s.Lose(__LINE__);return cleanup?1:s.Deny(c,at,__LINE__);}
  }
  if(task_claim.profile_control){
   const auto target=s.tasks.ObservedTransportSubject(work->first);
   const auto parent_task=current_task?s.tasks.ObservedTransportSubject(*current_task):std::optional<Tasks::Birth>{};
   const auto* target_node=target?s.tasks.FindBirthConst(target->generation):nullptr;
   const auto* parent_node=parent_task?s.tasks.FindBirthConst(parent_task->generation):nullptr;
   const auto flags=target?Word(*memory,target->task+8):std::optional<uint32_t>{};
   const auto data=target?Word(*memory,target->task+12):std::optional<uint32_t>{};
   if(!target || !parent_task || !target_node || !parent_node || !flags || !data ||
       target_node->delete_entered || target_node->free_entered || target_node->freed ||
       parent_node->delete_entered || parent_node->free_entered || parent_node->freed ||
       !DwordIs(c.r[3],target->task)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->profile_control.emplace();
   bool admitted=false;
   if(work->control_parent && work->control_parent->profile_control){
    const auto& parent=*work->control_parent->profile_control;
    admitted=Tasks::SameBirth(parent.parent,*parent_task) &&
        (at==0x816B8B08?work->profile_control->EnterDispatch(parent,*frame,*target):
         DwordIs(c.r[30],target->task) && DwordIs(c.r[31],parent.target.task) &&
         work->profile_control->EnterRecursiveCancel(parent,*frame,*target));
   }else{
    const auto* producer=s.tasks.ProfileProducerFor(*target);
    const auto* t=work->thread.get();
    const auto tagged=producer && producer->output>=t->stack_limit() &&
        uint64_t(producer->output)+4<=t->stack_base()?Word(*memory,producer->output):std::optional<uint32_t>{};
    admitted=producer && tagged && *flags==producer->native_flags && *data==producer->native_data &&
        work->profile_control->Enter(*producer,work->control_creator->serial,work->control_creator->frame,
            *frame,*target,*parent_task,*tagged);
   }
   if(at==0x816B8B08){
    const auto callback=Word(*memory,target->task+0x30),argument=Word(*memory,target->task+0x34);
    admitted=admitted && callback && argument && *callback==target_node->callback && *argument==target_node->argument && target_node->published;
   }
   if(!admitted){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->birth=*target;work->output=*flags;work->second=*data;
  }else if(at==0x8190A358 && work->periodic_role==State::PeriodicRole::kFactory){
   if(s.stopping.load() || !periodic_object_current || !DwordIs(c.r[3],s.periodic_object)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   const auto handle=Word(*memory,s.periodic_object+0xC);
   if(!handle){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   auto* factory=work->periodic_factory?s.Periodic(work->periodic_factory):nullptr;
   if(!factory){
    if(*handle || !s.next_periodic || s.next_periodic==UINT64_MAX){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    for(auto& row:s.periodic_factories)if(!row){row.emplace();factory=&*row;break;}
    if(!factory){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    work->periodic_factory=s.next_periodic++;
   }
   if(!factory->BeginEnable(work->periodic_factory,*frame,s.periodic_object,*handle)){
    factory->failed=true;s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
  }else if(at==0x819068D0 && work->periodic_role==State::PeriodicRole::kStop){
   auto* factory=s.Periodic(work->periodic_factory);const auto handle=Word(*memory,work->first+0xC);
   if(!factory || !periodic_object_current || !handle ||
       !s.PeriodicAllocationCurrent(factory->birth,factory->serial) ||
       !factory->BeginStop(*frame,work->first,*handle)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->birth=factory->birth;
  }else if(at==0x816B8B08 && work->periodic_role==State::PeriodicRole::kDispatcher){
   const auto callback=Word(*memory,work->first+0x30),argument=Word(*memory,work->first+0x34),flags=Word(*memory,work->first+8);
   const auto* node=s.tasks.FindLiveTask(work->first);const auto* factory=s.Periodic(work->periodic_factory);
   const auto period=Word(*memory,work->first+0xC);
   if(!node || !factory || !callback || !argument || !flags || !period ||
       !Tasks::SameBirth(node->birth,factory->birth) || node->periodic_factory!=factory->serial ||
       node->argument!=factory->object || *callback!=node->callback || *argument!=node->argument ||
       !s.tasks.MatchesPeriodicAllocation(node->birth,factory->serial,{*flags,*period})){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   const auto birth=s.tasks.BeginPeriodicDispatch(*frame,work->first,*callback,*argument,{*flags,*period});
   if(!birth){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}work->birth=*birth;
  }else if(at==0x816B7600 && work->periodic_role==State::PeriodicRole::kBoundary){
   const auto dispatch=s.Latest(frame->thread,0x816B8B08);
   if(!dispatch || !dispatch->periodic_body_admitted || dispatch->periodic_factory!=work->periodic_factory ||
       !s.PeriodicAllocationCurrent(dispatch->birth,work->periodic_factory) ||
       !DwordIs(c.r[3],dispatch->birth.task) || !DwordIs(c.r[4],0x815F8D58) || c.r[5]!=14 || c.r[6] ||
       !s.tasks.PeriodicCallbackReturned(dispatch->frame,*frame,dispatch->birth)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->birth=dispatch->birth;
  }else if((at==0x816B9F08 || at==0x816BA058) && work->periodic_factory){
   auto* factory=s.Periodic(work->periodic_factory);
   if(!factory || !s.PeriodicAllocationCurrent(factory->birth,factory->serial) ||
       !factory->MatchesStopChild(*frame,at,work->first)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->birth=factory->birth;
  }else if(at==0x816A2588){
   // Original local logon uses the same native CompleteIO body. Match the
   // factual original callback/worker, not another family's equal overlap.
   if(!s.job || !work->completion_route || !work->completion_route->FrameMatches(*frame) ||
       !work->completion_route->StillOwned(s.job,s.job->serial,s.job->source,s.job->overlap,c.r[3])){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   const bool initializer_error=frame->lr==0x8199EEBC;
   const auto parent=initializer_error?s.Latest(frame->thread,0x8199E3E0):
       frame->lr==0x8199E3D0?s.Latest(frame->thread,0x8199E148):
       frame->lr==0x8199DE00?s.Latest(frame->thread,0x8199C838):nullptr;
   const auto* node=current_task?s.tasks.FindLiveTask(*current_task):nullptr;
   const auto context=Word(*memory,work->first+8);
   bool original_expected=false;
   if(initializer_error && parent &&
       Tasks::MatchesInitializerErrorCompletion(parent->frame,*frame,parent->output,work->first,
           uint32_t(c.r[4]),uint32_t(c.r[5]),uint32_t(c.r[6])) &&
       parent->thread.get()==work->thread.get() &&
       (!s.job->original_caller || s.job->original_caller.get()==work->thread.get())){
    for(const auto& e:s.expected)if(e && e->serial==s.job->serial && e->source==s.job->source &&
        e->thread.get()==work->thread.get() && e->entered && !e->returned && !e->call_returned && e->actual &&
        (e->actual->function==0x8199E3E0 || e->actual->function==0x8199C630))
     original_expected=std::any_of(s.active.begin(),s.active.end(),[&](const auto& owner){
      return owner && owner->registered_executor && !owner->normal_return && owner->frame==*e->actual;
     });
   }
   if(!parent || parent->source!=s.job->source || s.job->completion_frame || !context ||
      (initializer_error ? !original_expected || int32_t(uint32_t(c.r[5]))>=0 :
       !node || node->birth.origin!=s.job->source) ||
      !DwordIs(c.r[4],int32_t(uint32_t(c.r[5]))<0?0x65Bu:0u) || c.r[6]){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->source=parent->source;
   if(initializer_error){
    work->initializer_completion_owner=parent;s.job->initializer_completion_owner=parent;
   }else work->birth=node->birth;
   s.job->completion_frame=*frame;s.job->completion_birth=work->birth;
   // This is expected native input metadata, not adoption of an allocation
   // from a writable context word. Native code retains/frees its envelope.
   s.job->completion_context=*context&~1u;s.job->completion_error=uint32_t(c.r[4]);
   s.job->completion_extended=uint32_t(c.r[5]);s.job->completion_length=uint32_t(c.r[6]);
  }else if(ListenerOuter(at)){
   work->listener_arguments=Listeners::DecodeOuter(at,ListenerRegisters(c));
   if(!s.listeners || !work->listener_arguments){s.Lose(__LINE__);return cleanup?1:s.Deny(c,at,__LINE__);}
   const auto& args=*work->listener_arguments;
   const Events::Root root{s.root.kernel,s.root.domain,s.root.generation};
   work->listener_frame=std::shared_ptr<const Listeners::Frame>(new Listeners::Frame(root,
       reinterpret_cast<uintptr_t>(&c),reinterpret_cast<uintptr_t>(c.thread_state),
       reinterpret_cast<uintptr_t>(c.kernel_state),frame->thread,frame->pcr,frame->sp,
       frame->lr,at,work->event_lifetime,work->thread->native_frame_thread_identity().process_type,
       work->system_discarded_identity_hint?Listeners::ScalarDeliveryMode::kSystemDiscardedIdentityHint:
                                             Listeners::ScalarDeliveryMode::kNone));
   if(at==0x816A8498){
    const auto allocation=s.FindCounted(args.object);
    const auto areas=Listeners::CaptureAreas(args.object,[&](uint32_t p){return Word(*memory,p);});
    // Actual8B20 factory produced this body at SP+50, initialized its areas,
    // and invokes8498(manager,body) at8BDC. Neither manager nor HANDLE is body.
    if(!allocation || allocation->delete_entered || allocation->listener_registration_entered ||
       frame->lr!=0x816A8BE0 || allocation->producer.function!=0x816A8B20 ||
       allocation->producer.lr!=0x816A8B5C || allocation->producer.thread!=frame->thread ||
       allocation->producer.pcr!=frame->pcr || allocation->producer.sp!=frame->sp || !areas){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
    work->listener_allocation=allocation;
    work->listener_registration=s.listeners->BeforeRegistration(work->listener_frame,
        c.r[3],c.r[4],std::span(areas->areas).first(areas->count),allocation);
    if(!work->listener_registration){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    allocation->listener_registration_entered=true;
   }else if(at==0x816A88E8){
    const auto allocation=s.FindCounted(args.object);work->listener_allocation=allocation;
    const auto get_next=s.Latest(frame->thread,0x816A8C00);
    const bool nested=c.thread_state->native_identity_listener_cell_.load(std::memory_order_acquire)!=nullptr;
    if(!allocation || !allocation->delete_entered || !work->registered_executor ||
       (nested && (!get_next || get_next->listener_allocation!=allocation))){s.Lose(__LINE__);return 1;}
    if(!allocation->listener_registration_entered){
     // Real8A48 initialization failure can ObDereference the freshly counted
     // body without ever entering8498. Native88E8 skips unlink iff +4 is0.
     if(allocation->listener_registered || Word(*memory,args.object+4)!=0u ||
        Word(*memory,args.object+8)!=0u){s.Lose(__LINE__);return 1;}
     work->listener_unregistered_destruction=true;
    }else if(!allocation->listener_registered ||
             !s.listeners->BeforeDestruction(work->listener_frame,c.r[3])){
     s.Lose(__LINE__);return 1;
    }
   }else if(at==0x816A93C8){
    const auto earlier=s.listeners->RetainEarlierSource(work->source);
    auto admission=Listeners::CellAdmissionStatus::kFailed;
    work->listener_cell=s.listeners->BeforeBroadcast(work->listener_frame,earlier,c.r[4],
        c.thread_state->native_identity_listener_cell_,c.r[3],&admission);
    if(!work->listener_cell){
     if(admission!=Listeners::CellAdmissionStatus::kClosedHealthy)s.Lose(__LINE__);
     return s.Deny(c,at,__LINE__); // canceled before native body, never success/drain
    }
   }else{
    const uint32_t selected=frame->sp>=0xF0?frame->sp-0xF0:0;
    const auto mapped_output=[&](uint32_t output){return !output ||
        memory->QueryRangeAccess(output,4)!=memory::PageAccess::kNoAccess;};
    if(!selected || memory->QueryRangeAccess(selected,4)==memory::PageAccess::kNoAccess ||
       !mapped_output(args.id_output) || !mapped_output(args.data_output)){
     s.Lose(__LINE__);c.r[3]=0;return 0;
    }
    auto admission=Listeners::CellAdmissionStatus::kFailed;
    work->listener_cell=s.listeners->BeforeGetNext(work->listener_frame,
        s.EarlierListenerWork(frame->thread),c.r[3],c.r[4],c.r[5],c.r[6],
        memory->TranslateVirtual<const uint8_t*>(selected),
        args.data_output?memory->TranslateVirtual<const uint8_t*>(args.data_output):nullptr,
        c.thread_state->native_identity_listener_cell_,
        args.id_output?memory->TranslateVirtual<const uint8_t*>(args.id_output):nullptr,&admission);
    if(!work->listener_cell){
     if(admission!=Listeners::CellAdmissionStatus::kClosedHealthy)s.Lose(__LINE__);
     c.r[3]=0;return 0; // BOOL0, output words untouched, no queued event consumed
    }
   }
  }else if(at==0x81694B48){
   if(s.FindProvider(work->first) || work->first<0x1000 || (work->first&7) ||
      work->first>UINT32_MAX-0xD8 || !current_task || !s.tasks.FindLiveTask(*current_task))return s.Deny(c,at,__LINE__);
   auto provider=std::make_shared<State::Provider>();provider->object=work->first;
   provider->generation=s.next_provider++;
   if(!provider->generation || !s.next_provider){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   const auto slot=std::find_if(s.providers.begin(),s.providers.end(),[](const auto& p){
    return !p || (p->destroyed && p.use_count()==1);
   });
   if(slot==s.providers.end())return s.Deny(c,at,__LINE__);
   *slot=provider;work->provider_generation=provider->generation;
  }else if(at==0x816934F8 || at==0x81693568 || at==0x81693CD8 || at==0x816921C8){
   const auto provider=s.FindProvider(work->first);
   if(!provider || provider->constructing || (provider->revoked && !cleanup))return cleanup?1:s.Deny(c,at,__LINE__);
   work->provider_generation=provider->generation;
   if(at==0x816934F8){
    provider->revoked=true;provider->image_epoch.Revoke();provider->image_output.reset();provider->no_read_query.reset();
   }else if(at==0x81693568){
    if(provider->initialized && !provider->buffer_revoked)return s.Deny(c,at,__LINE__);
    // The verified init body issues no read and does not write A0. Retain
    // no-read history for this birth; buffer_revoked withholds current use
    // until its genuine successful own-return installs the new allocation.
    provider->image_epoch.Revoke();provider->image_output.reset();
   }else if(at==0x81693CD8){
    const auto message=work->second<=UINT32_MAX-4?Word(*memory,work->second+4):std::nullopt;
    if(!message)return s.Deny(c,at,__LINE__);
    if(*message==0xA){
     const auto terminal=Word(*memory,provider->object+0xB8);
     const bool querying=std::any_of(s.active.begin(),s.active.end(),[&](const auto& old){
      return old->provider_generation==provider->generation && old->frame.function==0x816921C8;
     });
     if(!terminal || *terminal==997 || querying)return s.Deny(c,at,__LINE__);
     // Event A frees storage, not the provider birth's no-read history.
     provider->buffer_revoked=true;provider->image_epoch.Revoke();provider->image_output.reset();
    }
   }else{
    // Retain only a positive chain from actual initialization or the last
    // exact no-read Query. An unproven/denied Query must never relearn it.
    const bool no_image_history=s.ProviderNoImageHistory(*provider) &&
        provider->image_uri_generation!=UINT64_MAX;
    const auto prior_no_read_version=provider->no_read_query?
        std::optional<uint64_t>(provider->no_read_query->version):std::nullopt;
    provider->no_read_query.reset();
    // Query can rewrite its borrowed URI even when it leaves encoded A8
    // bytes untouched. Keep key-copy continuity separate from buffer use.
    if(provider->image_uri_generation==UINT64_MAX){
     if(s.image_slot)s.image_slot->unavailable_.store(true,std::memory_order_release);
    }else ++provider->image_uri_generation;
    provider->image_key.reset();
    work->image_uri_generation=provider->image_uri_generation;
    if(DwordIs(c.r[4],uint32_t(c.r[4])) && DwordIs(c.r[5],uint32_t(c.r[5]))){
     work->image_event=uint32_t(c.r[4]);work->image_modified=uint32_t(c.r[5]);
    }
    if(!provider->initialized || provider->buffer_revoked ||
       std::any_of(s.active.begin(),s.active.end(),[&](const auto& old){
        return old->provider_generation==provider->generation && old->frame.function==0x816921C8;
       }) || !current_task)return s.Deny(c,at,__LINE__);
    const auto birth=s.tasks.BeginExternalWork(*frame,*current_task);
    if(!birth){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    work->external_birth=*birth;work->birth=*birth;
    if(no_image_history){
     // GCRC+State protect this entry observation; no guest body has run.
     const auto pending=Byte(*memory,provider->object+0x98);
     const auto version=Qword(*memory,provider->object+0xA0);
     if(pending && !*pending && version &&
        (!prior_no_read_version || *version==*prior_no_read_version))
      work->image_no_read_entry=State::Work::ImageQueryNoReadEntry{provider->image_uri_generation,*version};
    }
   }
  }else if(at==0x81670B68){
   const auto parent=s.Latest(frame->thread,0x816921C8);
   if(!parent)return 1;
   const auto provider=s.FindProvider(parent->first);
   if(!provider || provider->generation!=parent->provider_generation ||
      !provider->initialized || provider->revoked || provider->buffer_revoked ||
      frame->lr!=0x81692304 || c.r[3]>15 || c.r[4] || c.r[5] ||
      c.r[7]!=provider->buffer || c.r[8]!=uint64_t(provider->object)+0x94 ||
      c.r[9]!=uint64_t(provider->object)+0xB8)return s.Deny(c,at,__LINE__);
   const auto capacity=Word(*memory,provider->object+0x94);
   const auto version=Qword(*memory,provider->object+0xA0);
   if(!capacity || !version || *capacity!=provider->capacity || c.r[3]!=provider->type)return s.Deny(c,at,__LINE__);
   auto tile=std::make_shared<NativeIdentityTileWork::Data>();
   tile->source=parent->source;tile->provider_generation=provider->generation;
   tile->provider=provider->object;tile->output=provider->buffer;
   tile->size_address=provider->object+0x94;tile->capacity=*capacity;
   tile->version=*version;tile->user=uint32_t(c.r[6]);tile->type=uint32_t(c.r[3]);
   work->source=parent->source;work->provider_generation=provider->generation;
   work->tile=tile;provider->last_output=std::move(tile);
  }else if(at==0x816A2CF0 && work->creation_only){
   std::array<uint32_t,6> args{};
   for(uint32_t i=0;i<args.size();++i){
    if(!DwordIs(c.r[i+3],uint32_t(c.r[i+3]))){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    args[i]=uint32_t(c.r[i+3]);
   }
   work->creation.emplace();
   if(!work->creation->EnterStart(*frame,args)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }else if(at==0x816A2A48 && work->creation_only){
   const auto parent=work->creation_parent;
   if(!parent || !parent->creation || !DwordIs(c.r[3],work->first) ||
       !DwordIs(c.r[4],work->second) || !DwordIs(c.r[5],uint32_t(c.r[5])) ||
       !parent->creation->EnterCopy(*frame,work->first,work->second,uint32_t(c.r[5]))){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->output=uint32_t(c.r[5]);
  }else if(at==0x816B9D68 && work->creation_only){
   const auto parent=work->creation_parent;
   if(!parent || !parent->creation || !captured_node){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   const auto packet=parent->creation->packet;
   const auto app=Word(*memory,packet),message=Word(*memory,packet+4),overlap=Word(*memory,packet+8);
   if(!app || !message || !overlap ||
       !parent->creation->EnterInline(*frame,work->first,work->second,uint32_t(c.r[5]),
           {*app,*message,*overlap},captured_node->birth)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->birth=captured_node->birth; // Borrowed parent generation, never child birth.
  }else if(at==0x816BA440){
   if((!work->periodic_factory && Tasks::ClassifyCallback(c.r[3])!=Tasks::CallbackClaim::kCanonicalProfile) ||
      !DwordIs(c.r[4],work->second) || !DwordIs(c.r[5],uint32_t(c.r[5])) ||
      !DwordIs(c.r[6],uint32_t(c.r[6]))){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->output=uint32_t(c.r[6]);
   if(work->first==0x816F4298 || work->first==0x816F4410){
    const bool first=work->first==0x816F4298;
    const auto parent=s.Latest(frame->thread,0x816F44D0);
    if(!parent || !parent->registered_executor || !parent->physical_work ||
       frame->lr!=(first?0x816F4610u:0x816F4644u) ||
       uint64_t(frame->sp)+0x70!=parent->frame.sp || !DwordIs(c.r[3],work->first) || c.r[4] ||
       c.r[5]!=uint64_t(frame->sp)+0x50 || !DwordIs(c.r[6],first?0x81AAC29Cu:0x81AAC298u) ||
       Word(*memory,uint32_t(c.r[5]))!=0x04200001u ||
       Word(*memory,uint32_t(c.r[5])+4)!=(first?0x81A854F8u:0x81A85508u)){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
    work->physical_work=true;
   }
   if(work->transport_only && (!work->registered_executor || work->first!=0x81696558 ||
       work->source!=kBootUnjoinedSource || c.r[5])){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   if(work->identity_continuation && (!work->registered_executor || !s.job ||
       work->source!=s.job->source || work->first!=0x81696558 || work->transport_only)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   if(work->periodic_factory){
    auto* factory=s.Periodic(work->periodic_factory);
    const auto options=Word(*memory,uint32_t(c.r[5])),period=Word(*memory,uint32_t(c.r[5])+4);
    if(!factory || !options || !period){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    work->schedule=s.tasks.BeginPeriodicSchedule(*frame,*factory,work->first,work->second,
        uint32_t(c.r[5]),work->output,{*options,*period},work->thread->native_frame_thread_identity().process_type);
   }else
   work->schedule=work->transport_only ?
       s.tasks.BeginSharedContinuationSchedule(*frame,work->second,work->output):
       work->identity_continuation ?
       s.tasks.BeginIdentityContinuationSchedule(*frame,work->source,work->second,work->output):
       s.tasks.BeginSchedule(*frame,work->source,work->first,work->second,
                            uint32_t(c.r[5]),work->output);
   if(!work->schedule){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   if(task_claim.profile_producer){
    const auto creator=std::find_if(s.active.begin(),s.active.end(),[&](const auto& parent){return parent->serial==task_claim.control_creator;});
    std::optional<std::array<uint32_t,2>> options;
    if(c.r[5]){
     const auto first=Word(*memory,uint32_t(c.r[5])),second=Word(*memory,uint32_t(c.r[5])+4);
     if(!first || !second){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}options=std::array<uint32_t,2>{*first,*second};
    }
    const auto* t=work->thread.get();
    if(creator==s.active.end() || (*creator)->normal_return || !s.job || (*creator)->source!=s.job->source){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
    const auto& owner=**creator;
    const uint32_t overlap=owner.frame.function==0x8199E3E0?owner.output:0;
    uint32_t packet=0;std::array<uint32_t,3> linkage{};
    if(overlap){
     // Actual224 stores its allocator output at bodySP+54, FE at packet+0,
     // caller overlap at +8 and its Task output at +18. No allocation-size
     // guess or heap address alone supplies this prepublication authority.
     const auto allocated=Word(*memory,frame->sp+0x54);
     if(!allocated || *allocated<0x1000 || (*allocated&3) || *allocated>UINT32_MAX-0x1B ||
        memory->QueryRangeAccess(*allocated,0x1C)==memory::PageAccess::kNoAccess){
      s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
     }
     packet=*allocated;
     const auto tag=Word(*memory,packet),linked=Word(*memory,packet+8),back=Word(*memory,overlap+8);
     if(!tag || !linked || !back){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
     linkage={*tag,*linked,*back};
    }else if(work->output<t->stack_limit() || uint64_t(work->output)+4>t->stack_base()){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
    const auto output=Tasks::CaptureProfileOutput(owner.frame,*frame,work->output,overlap,packet,linkage);
    if(!output || !s.tasks.BindProfileProducer(work->schedule,owner.serial,owner.frame,options,*output)){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
    if(owner.frame.function==0x8199E3E0)s.job->initializer_scheduled=true;
   }
  }else if(at==0x816BA668){
   if(frame->lr!=0x816F4568 && frame->lr!=0x816F45B0)return 1;
   const auto parent=s.Latest(frame->thread,0x816F44D0);
   if(!parent || !parent->registered_executor || uint64_t(frame->sp)+0x70!=parent->frame.sp ||
      !DwordIs(c.r[3],work->first)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->output=work->first;
   work->schedule=s.tasks.BeginPersistentCreate(*frame,work->source,work->output);
   if(!work->schedule){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }else if(at==0x816B9A18){
   const auto parent=frame->lr==0x816BA688?s.Latest(frame->thread,0x816BA668):
       frame->lr==0x816BA46C?s.Latest(frame->thread,0x816BA440):nullptr;
   work->output=work->second;
   if(work->creation_only){
    const auto inlined=work->creation_parent;
    const auto start=inlined?inlined->creation_parent:nullptr;
    const auto current=current_task?s.tasks.FindLiveTask(*current_task):nullptr;
    if(!start || !start->creation || !current ||
        current->birth.generation!=start->creation->parent.generation ||
        current->birth.origin!=start->creation->parent.origin ||
        current->birth.task!=start->creation->parent.task ||
        !start->creation->EnterAllocation(*frame,work->first,work->output)){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
    work->detached_allocation=s.tasks.BeginDetachedAllocation(*frame,kBootUnjoinedSource,work->output);
    if(!work->detached_allocation){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   }else if(parent && parent->schedule){
    work->schedule=parent->schedule;
    if(!s.tasks.EnterAllocation(work->schedule,*frame,work->first,work->output)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   }else{
    // An observed allocation supplies execution identity only. Its source is
    // unjoined unless an already owned invocation produced it.
    work->detached_allocation=s.tasks.BeginDetachedAllocation(*frame,work->source,work->output,work->foreign_avatar_task);
    if(!work->detached_allocation){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   }
  }else if(at==0x816B9BB8){
   const auto parent=s.Latest(frame->thread,0x816BA440);
   if(!parent || !parent->schedule)return 1;
   work->schedule=parent->schedule;
   if(const auto* node=s.tasks.FindLiveTask(work->first);node && node->periodic_factory &&
       !s.PeriodicAllocationCurrent(node->birth,node->periodic_factory)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   if(const auto* call=s.tasks.FindSchedule(work->schedule);call && call->producer &&
       call->producer->output_proof.kind==Tasks::ProfileOutput::Kind::kAsyncPacket){
    const auto& p=*call->producer;const auto& output=p.output_proof;
    const auto tag=Word(*memory,output.packet),linked=Word(*memory,output.packet+8),back=Word(*memory,output.overlap+8);
    if(!tag || !linked || !back ||
       Tasks::CaptureProfileOutput(p.owner,p.schedule,p.output,output.overlap,output.packet,{*tag,*linked,*back})!=
         std::optional<Tasks::ProfileOutput>{output}){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
   }
   // This is the final packet/output read. Original B9BB8 may now run a
   // worker which closes the tagged Task and frees its FE packet before
   // Schedule or224 returns. No packet pin survives this actual entry cut.
   if(!s.tasks.BeforePublication(work->schedule,*frame,work->first,work->second,
       uint32_t(c.r[5]),Word(*memory,parent->output))){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   if(const auto* call=s.tasks.FindSchedule(work->schedule);call && call->producer){
    const auto flags=Word(*memory,work->first+8),data=Word(*memory,work->first+12);
    if(!flags || !data || !s.tasks.BindProfileNativeState(work->schedule,work->first,*flags,*data)){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
    }
   }
  }else if(at==0x816B9AA8 && work->execution_options){
   auto d=s.ModifyOptionsSnapshot(c,task_claim,2,__LINE__);d.available|=8;
   const bool input=DwordIs(c.r[7],uint32_t(c.r[7])) && uint32_t(c.r[7])>=0x1000 &&
       !(uint32_t(c.r[7])&3) && uint32_t(c.r[7])<=UINT32_MAX-7;
   const auto old_flags=Word(*memory,work->first+8),old_data=Word(*memory,work->first+12);
   const auto new_flags=input?Word(*memory,uint32_t(c.r[7])):std::optional<uint32_t>{};
   const auto new_data=input?Word(*memory,uint32_t(c.r[7])+4):std::optional<uint32_t>{};
   if(old_flags){d.available|=16;d.old_flags=*old_flags;}
   if(old_data){d.available|=32;d.old_data=*old_data;}
   if(new_flags){d.available|=64;d.new_flags=*new_flags;}
   if(new_data){d.available|=128;d.new_data=*new_data;}
   const auto observed=[&](uint32_t bit,bool value){d.evaluated|=1u<<bit;if(value)d.predicate_true|=1u<<bit;return value;};
   const bool admitted=observed(0,DwordIs(c.r[3],work->first)) && observed(1,DwordIs(c.r[4],work->second)) &&
       observed(2,input) && observed(3,bool(old_flags)) && observed(4,bool(old_data)) &&
       observed(5,bool(new_flags)) && observed(6,bool(new_data)) &&
       observed(7,Tasks::PreservesTimedOptions({*old_flags,*old_data},{*new_flags,*new_data}));
   d.abi_eligible=admitted;s.first_modify_options_admission.Capture(d);
   if(!admitted){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   const auto birth=s.tasks.BeforeExecutionOnlyTimedOptions(*frame,work->first,work->second,{*old_flags,*old_data},{*new_flags,*new_data});
   if(!birth){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}work->birth=*birth;
   work->output=uint32_t(c.r[7]); // validated original options pointer, metadata only
   // Original B9AA8 still performs the real attribute/requeue stores/calls.
   // Its r3 on return is residue, never an HRESULT or semantic completion.
  }else if(at==0x816B9AA8){
   if(frame->lr!=0x816F458C && frame->lr!=0x816F45D4)return 1;
   const auto parent=s.Latest(frame->thread,0x816F44D0);
   const uint32_t callback=frame->lr==0x816F458C?0x816F2618u:0x816F3A40u;
   const uint32_t options=frame->lr==0x816F458C?0x04200004u:0x05400004u;
   if(!parent || !parent->registered_executor || uint64_t(frame->sp)+0x70!=parent->frame.sp ||
      !DwordIs(c.r[5],callback) || c.r[6] ||
      !DwordIs(c.r[7],uint32_t(c.r[7])) || c.r[7]!=uint64_t(frame->sp)+0x50 ||
      Word(*memory,uint32_t(c.r[7]))!=options || Word(*memory,uint32_t(c.r[7])+4)!=0){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   const auto birth=s.tasks.BeforePersistentCallbackBinding(*frame,work->first,work->second,
       uint32_t(c.r[5]),Word(*memory,work->first+0x34));
   if(!birth){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->birth=*birth;
   if(callback==0x816F2618)s.wake=std::unique_ptr<Wake>(new Wake(s.root,*birth,s.current_read_source));
  }else if(at==0x816B7490){
   if(!work->reference_only && !s.tasks.FindLiveTask(work->first))return 1;
   if(!work->reference_only && work->periodic_factory){
    const auto* node=s.tasks.FindLiveTask(work->first);
    if(!node || !s.PeriodicAllocationCurrent(node->birth,work->periodic_factory)){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   }
   if(!work->reference_only && !s.tasks.EnterClose(*frame,work->first)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   if(!work->reference_only)if(const auto target=s.tasks.ObservedTransportSubject(work->first))s.tasks.ObserveProfileOwnerClose(*frame,*target);
   if(work->task_release && !work->reference_only){
    const auto e=std::find_if(s.expected.begin(),s.expected.end(),[&](const auto& e){
     return e && e->serial==work->release_receipt && e->task_release && e->actual==*frame;
    });
    const auto born=s.tasks.ObservedTransportSubject(work->first);
    if(e==s.expected.end() || !born || born->generation!=work->release_generation ||
        !(*e)->task_release->Enter(*frame,*born)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   }
  }else if(at==0x816B7D98 || at==0x816B7500){
   if(!s.tasks.FindLiveTask(work->first))return 1;
   if(!s.tasks.EnterDeletingDestructor(*frame,work->first,uint32_t(c.r[4]))){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   if(work->task_release){const auto parent=s.Latest(frame->thread,0x816B7490);
    if(!parent || !parent->task_release || parent->serial!=task_claim.parent ||
        parent->release_generation!=work->release_generation || parent->first!=work->first){
     s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   }
  }else if(at==0x816E6AB0){
   // Free receives the native object pointer, never a handle/output. Match
   // the earlier actual deleting destructor before retiring numeric lookup.
   const auto parent=s.TaskFreeParent(frame->thread);
   if(!parent || parent->serial!=task_claim.parent){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   if(work->task_release)work->release_generation=parent->release_generation;
   if(!s.tasks.EnterTaskFree(*frame,work->first)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }else if(ProfileCallback(at)){
   if(!current_task){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   const auto parent=at==0x8199C838?s.Latest(frame->thread,0x8199E148):nullptr;
   if(parent && parent->birth.generation && parent->birth.task==*current_task){
    // E148 runs C838 synchronously in its actual same worker Task. This is
    // nested whole work, not a second once-only callback start.
    work->birth=parent->birth;work->source=parent->source;
   }else{
    const auto birth=work->physical_work?
        s.tasks.BeginPersistentWork(*frame,*current_task,work->first):
        s.tasks.EnterWork(*frame,*current_task,work->first);
    if(!birth){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
    work->birth=*birth;work->source=birth->origin;
    work->owns_persistent_work=work->physical_work;work->owns_task_work=!work->physical_work;
   }
  }else if(at==0x816F25E0){
   if(!earlier || earlier->physical_work)work->source=s.current_read_source;
   work->physical_work=false;
   if(s.old_ingress_closed && (!s.job || work->source!=s.job->source))return s.Deny(c,at,__LINE__);
   if(!s.wake || !(work->wake=s.wake->EnterWake(*frame,work->source,c.r[3]!=0)))return s.Deny(c,at,__LINE__);
  }else if(at==0x816B7B68){
   const auto parent=s.Latest(frame->thread,0x816F25E0);
   if(!parent || !parent->wake || !s.wake)return 1;
   const auto* node=s.tasks.FindLiveTask(work->first&~1u);
   if(!node || !s.wake->ReachNativeReschedule(*parent->wake,*frame,node->birth)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->wake=parent->wake;
  }else if(at==0x816B7600 && work->periodic_role!=State::PeriodicRole::kBoundary){
   const auto reschedule=s.Latest(frame->thread,0x816B7B68);
   const auto wake=s.Latest(frame->thread,0x816F25E0);
   if(!reschedule || !wake || !reschedule->wake || !wake->wake)return 1;
   if(!s.wake || !s.wake->MatchesNativeRescheduleTail(*wake->wake,*reschedule->wake,
       wake->frame,reschedule->frame,*frame,reschedule->first,{c.r[3],c.r[4],c.r[5],c.r[6]})){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->wake=reschedule->wake;work->tail_reschedule=reschedule;work->tail_wake=wake;
  }else if(at==0x816F2618){
   const auto* node=current_task?s.tasks.FindLiveTask(*current_task):nullptr;
   if(!node || !s.wake || !(work->wake_work=s.wake->EnterWorker(*frame,node->birth)))return s.Deny(c,at,__LINE__);
   work->birth=node->birth;work->source=s.wake->RetainedSource(*work->wake_work);
   if(!work->source){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }else if(at==0x816E3A98){
   const auto parent=s.Latest(frame->thread,0x816F2618);
   if(!parent || !parent->wake_work || !s.wake)return 1;
   work->wake_work=parent->wake_work;work->source=parent->source;work->birth=parent->birth;
   if(!s.wake->EnterRefreshChild(*work->wake_work,*frame)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }
  work->serial=s.next_work++;
  if(!work->serial || !s.next_work){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  if(((ProfileCallback(at) && !work->physical_work) || at==0x816921C8) && current_task){
   const auto* node=s.tasks.FindLiveTask(*current_task);
   const auto earlier=s.listeners->RetainEarlierSource(work->source);
   if(!node || !work->event_lifetime){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   const Events::Root root{s.root.kernel,s.root.domain,s.root.generation};
   const Events::WorkKey key{root,node->birth.generation,work->serial,frame->thread,
       frame->pcr,*current_task,frame->sp,frame->lr,at};
   work->listener_parent=s.SynchronousListenerParent(*work,key);
   work->listener_source=std::shared_ptr<const NativeIdentityEventWorkSource>(
       new NativeIdentityEventWorkSource(key,earlier,work->event_lifetime,
           work->listener_parent?work->listener_parent->listener_source:nullptr));
   work->listener_work=s.listeners->AdoptWholeWork(work->listener_source);
   if(!work->listener_work){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }
  // Retain first. Every subsequent refusal may follow an entered read scope
  // or registered native generation; uncertainty never destroys it locally.
  s.active.push_back(work);
  const bool user_body=(ProfileCallback(at) && !work->physical_work) || at==0x816F2618 ||
      at==0x816E3A98 || at==0x816921C8;
  if(user_body){
   const auto* node=current_task?s.tasks.FindLiveTask(*current_task):nullptr;
   if(!node){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   if(!work->birth.generation)work->birth=node->birth;
   auto* pool=&s.boot_reads;
   if(s.job){pool=work->source==s.job->source?&s.job->writer_reads:&s.job->old_reads;}
   const auto slot=std::find_if(pool->begin(),pool->end(),[](const auto& read){return bool(read);});
   if(slot==pool->end()){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   work->read_pool=pool;work->read=std::move(*slot);
   if(!work->read->Enter(s.kernel,work->thread.get(),c.thread_state,node->birth.task,
       node->birth.generation,work->serial,s.config.catalog->calls()->module()->hmodule_ptr(),s.root.generation)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
  }
  if(at==0x8199C838){
   if(!s.job || work->source!=s.job->source || !work->birth.generation || !s.wake ||
      frame->sp<0x690 || work->first<0x1000 || (work->first&7) || work->first>UINT32_MAX-0xC18 ||
      memory->QueryRangeAccess(work->first,0xC18)==memory::PageAccess::kNoAccess ||
      memory->QueryRangeAccess(0x81AA28D8,0xA48)==memory::PageAccess::kNoAccess ||
      c.thread_state->native_identity_publication_cell_.load(std::memory_order_acquire)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   const auto title=Word(*memory,work->first+0xBFC),flags=Word(*memory,work->first+0xBF8);
   if(!title || !flags || (*flags&~0x83u)!=(s.job->flags&~3u)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
   auto cell=std::make_shared<State::Commit>();cell->intent=s.job->intent;
   cell->publication=s.job->publication;cell->owner=&s;cell->wake=s.wake.get();
   cell->context=&c;cell->thread_state=c.thread_state;cell->source=work->source;
   cell->claimed_nonce=s.config.factory_binding->nonce();cell->worker_birth=work->birth.generation;
   cell->whole_work=work->serial;cell->pcr=frame->pcr;cell->publisher_sp=frame->sp-0x510;
   cell->input=work->first+0x28;cell->original_flags=s.job->flags;cell->title_context=*title;
   cell->request_flags=memory->TranslateVirtual<uint32_t*>(work->first+0xBF8);
   for(size_t i=0;i<4;++i){
    cell->desired[i]=s.job->intent->prepared_roster()->slots()[i].xuid;
    cell->input_flags[i]=memory->TranslateVirtual<uint32_t*>(cell->input+uint32_t(i)*0x290);
    cell->input_xuids[i]=memory->TranslateVirtual<uint64_t*>(cell->input+8+uint32_t(i)*0x290);
    cell->native_flags[i]=memory->TranslateVirtual<uint32_t*>(0x81AA28E0+uint32_t(i)*0x290);
    cell->native_xuids[i]=memory->TranslateVirtual<uint64_t*>(0x81AA28E8+uint32_t(i)*0x290);
   }
   cell->native_title_context=memory->TranslateVirtual<uint32_t*>(0x81AA28D8);
   const auto* t=work->thread.get();
   if(!t || uint64_t(cell->publisher_sp)+0x144>t->stack_base() ||
      cell->publisher_sp<t->stack_limit() ||
      memory->QueryRangeAccess(cell->publisher_sp+0x140,4)==memory::PageAccess::kNoAccess ||
      memory->QueryRangeAccess(0x81A83028,8)==memory::PageAccess::kNoAccess ||
      memory->QueryRangeAccess(0x815FB160,4)==memory::PageAccess::kNoAccess){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   // Native C924 fills this stack pointer before its DA38 canceled branch.
   // Hot publisher validation only reads these prebound scalar locations.
   cell->canceled_title_pointer=memory->TranslateVirtual<const uint32_t*>(cell->publisher_sp+0x140);
   cell->default_provider=memory->TranslateVirtual<const uint32_t*>(0x81A83028);
   cell->default_vtable=memory->TranslateVirtual<const uint32_t*>(0x81A8302C);
   cell->default_accessor=memory->TranslateVirtual<const uint32_t*>(0x815FB160);
   cell->diagnostic_thread=frame->thread;cell->diagnostic_task=work->birth.task;
   // Inside the ORIGINAL GCRC/State scope, before cell publication. Bind no
   // new owner and read no guest word here. Optional failure changes only
   // availability; the hot publisher acquires no new lock or Task/TLS helper.
   try{
    const auto diagnostic_word=[&](uint32_t at)->const uint32_t* {
     if(at<0x1000 || (at&3) || at>UINT32_MAX-3)return nullptr;
     const auto* heap=memory->LookupHeap(at);
     if(!heap || heap->heap_type()!=HeapType::kGuestVirtual ||
        memory->LookupHeap(at+3)!=heap ||
        memory->QueryRangeAccess(at,4)==memory::PageAccess::kNoAccess)return nullptr;
     return memory->TranslateVirtual<const uint32_t*>(at);
    };
    const uint64_t stack_begin=uint64_t(cell->publisher_sp)+0xB0;
    const uint64_t stack_end=uint64_t(cell->publisher_sp)+0xD4;
    if(work->thread && work->thread->stack_limit()<work->thread->stack_base() &&
       stack_begin>=work->thread->stack_limit() && stack_end<=work->thread->stack_base()){
     cell->diagnostic_status=diagnostic_word(uint32_t(stack_begin));
     cell->diagnostic_index=diagnostic_word(uint32_t(cell->publisher_sp+0xD0));
    }
   }catch(...){cell->diagnostic_status=cell->diagnostic_index=nullptr;}
   work->commit=std::move(cell);
  }
  if(at==0x816BA440 && c.thread_state){
   auto* d=s.OwnTaskDiagnostic(c.thread_state->native_identity_task_diagnostic_cell_.load(std::memory_order_acquire));
   if(d && d->context==&c && d->kernel==s.kernel && DwordIs(c.r[1],uint32_t(d->value.body_sp))){
    d->value.schedule_sp=c.r[1];d->value.schedule_lr=c.lr;d->value.callback=c.r[3];
    d->value.argument=c.r[4];d->value.options=c.r[5];d->value.output=c.r[6];
    d->value.schedule=work->schedule;d->value.available|=4;
   }
  }
  if(at==0x816B9BB8 && c.thread_state){
   auto* d=s.OwnTaskDiagnostic(c.thread_state->native_identity_task_diagnostic_cell_.load(std::memory_order_acquire));
   if(d && d->context==&c && d->kernel==s.kernel && d->value.body_sp>=0x90 &&
      DwordIs(c.r[1],uint32_t(d->value.body_sp-0x90)) && current_task){
    d->value.consumer_native_tls_task=*current_task;d->value.available|=8192;
   }
  }
  if(at==0x8199E3E0 || at==0x8199E148)s.BindTaskDiagnostic(*work,c,current_task);
  if(work->commit){
   void* empty=nullptr;
   if(!c.thread_state->native_identity_publication_cell_.compare_exchange_strong(empty,
       work->commit.get(),std::memory_order_acq_rel)){s.Lose(__LINE__);return s.Deny(c,at,__LINE__);}
  }
  // Only this FINAL admitted-body cut can witness an actual decrement.
  // A frame captured earlier and then denied is not a native ref-count return.
  if(work->reference_release && !work->reference_release->receipt.EnterRelease(*frame)){
   s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
  }
  if(work->periodic_role!=State::PeriodicRole::kNone){
   if(work->periodic_role==State::PeriodicRole::kFactory){
    const auto* factory=s.Periodic(work->periodic_factory);
    if(factory)work->periodic_context=Tasks::RetainAdmittedPeriodicFactory(*frame,*factory);
   }
   if(!work->periodic_context || !work->periodic_context->Matches(*frame,work->periodic_factory)){
    s.Lose(__LINE__);return s.Deny(c,at,__LINE__);
   }
   work->periodic_body_admitted=true;
  }
  if(work->profile_control)work->profile_control->body_admitted=true;
  return 1;
 }catch(...){if(const auto owner=Lookup(c.kernel_state)){owner->state_->Lose(__LINE__);return owner->state_->Deny(c,at,__LINE__);}return Abort(c,at);}
}

void NativeProfileIdentityOwner::ObserveReturn(void* raw,uint64_t address) noexcept {
 if(address<=UINT32_MAX && CoversIdentityServiceBoundary(raw,uint32_t(address),true)){ClearTaskAllocationCut();return;}
 State::HookUse hook_use;
 ClearTaskAllocationCut();
 if(!raw || address>UINT32_MAX)return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto at=uint32_t(address);
 if(ObservesNativeIdentityImage(at)){ObserveImageReturn(raw,at);return;}
 if(at==0x818CEE08)if(const auto owner=Lookup(c.kernel_state))
  if(owner->ReturnIdentityServiceTick(raw,at))return;
 if(at==0x816BA3C0){ObserveContextReference(raw,5);return;}
 if(at==0x816B7490)ObserveContextReference(raw,4);
 if(at==0x816B7F30)ObserveContextReference(raw,2);
 if(at==0x816B8098 || at==0x816B7F30){
  auto* d=c.thread_state?static_cast<TaskReference::Data*>(
      c.thread_state->native_identity_task_reference_cell_.load(std::memory_order_acquire)):nullptr;
  if(d)d->Return(c,at);return;
 }
 if(at==0x816DC628){ReturnPreboundNativePublisher(raw);return;}
 if(at==0x816A8328 || at==0x816A8648){
  auto* cell=c.thread_state?static_cast<Listeners::FastCell*>(
      c.thread_state->native_identity_listener_cell_.load(std::memory_order_acquire)):nullptr;
  if(cell)Listeners::FastReturn(cell,ListenerRegisters(c),at);return;
 }
 if((at==0x816A2CF0 || at==0x816A2A48) && NativeXmsgProvenance::Lookup(c.kernel_state))return;
 if(ObserveExecutionReturn(raw,at))return;
 try{
  auto owner=Lookup(c.kernel_state);
  if(!owner && CommonTaskFunction(at))owner=NativeSystemUiService::CaptureTaskExecutionOwner(c.kernel_state);
  if(!owner)return;auto& s=*owner->state_;
  if(s.retired.load(std::memory_order_acquire))return;
  if(!hook_use.Acquire(s.hook_rundown,owner->state_,false))return;
  PublishTaskAllocationCut allocation_cut{owner.get(),c,{}};
  std::optional<AllocationFact> allocation_fact;
  const auto shape=s.TransportShape(c,at);
  std::shared_ptr<State::Work> work;
  uint32_t diagnostic_thread=0,diagnostic_pcr=0;
  {
   std::lock_guard lock(s.mutex);
   if(shape && at==0x816A2588){
    // The youngest actual same-function entry shadows every older invocation,
    // even if this return's SP/LR is malformed. Never skip its neutral marker.
    const auto youngest=s.Latest(shape->thread,at);
    if(youngest && youngest->completion_route &&
        youngest->completion_route->FrameMatches(*shape))work=youngest;
   }else if(shape)for(auto i=s.active.rbegin();i!=s.active.rend();++i)
    if((*i)->frame==*shape){work=*i;break;}
   if(!work){
    auto* thread=ActualThread();const auto prior=thread?s.Latest(thread->thread_id(),at):nullptr;
    if(at==0x816A2588 && prior && prior->transport_unclaimed && prior->completion_route){
     // A malformed return cannot be repaired later by an equal-looking frame.
     // Poison only this actual neutral memento; retain it and every older owner.
     (void)prior->completion_route->ReturnOrdinary(shape.value_or(Frame{}));
    }
    if(prior && !prior->transport_unclaimed){
     s.RecordReturnFailure(c,at,prior->frame,*prior,prior->birth,prior->birth.generation?1u:0u,false,8,__LINE__,2);
     s.Lose(__LINE__);prior->uncertain=true;
    }
    return; // Missing/malformed neutral return retains its marker and HOLD.
   }
   diagnostic_thread=work->frame.thread;diagnostic_pcr=work->frame.pcr;
   // This is the genuine own-return shape, even if original error handling
   // below retains uncertain work. Diagnostic withdrawal grants no release.
   s.ReturnTaskDiagnostic(*work,c);
   if(work->transport_unclaimed){
    if(work->completion_route && (!shape || !work->completion_route->ReturnOrdinary(*shape))){
     s.Lose(__LINE__);return; // keep the actual marker and older identity owner
    }
    if(work->normal_return){
     s.RecordReturnFailure(c,at,work->frame,*work,work->birth,work->birth.generation?1u:0u,false,9,__LINE__);
     s.Lose(__LINE__);return;
    }
    work->normal_return=true;
    const auto retiring=std::find(s.active.begin(),s.active.end(),work);
    if(retiring==s.active.end()){s.Lose(__LINE__);return;}
    s.host_retired.push_back(std::move(work));s.active.erase(retiring);s.changed.notify_all();return;
   }
   if(s.uncertain.load(std::memory_order_acquire) && !work->task_release && !work->periodic_factory)return;
   if(work->periodic_factory && !work->periodic_body_admitted)return;
   if(work->profile_control && !work->profile_control->body_admitted)return;
  }
  if((work->profile_control || work->profile_producer) && !CheckProfileControlCode(*s.config.catalog->calls())){s.Lose(__LINE__);return;}
  if(work->periodic_factory && !CheckPeriodicCode(*s.config.catalog->calls())){s.Lose(__LINE__);return;}
  if(work->system_thread_frame && !CheckFiniteSystemThreadCode(*s.config.catalog->calls(),at)){s.Lose(__LINE__);return;}
  State::CodeOwner caller_code;
  // A stored child caller remains the same leased object/load generation on
  // its own return. Never recapture another child as replacement provenance.
  const bool transport=work->periodic_factory || work->profile_control || work->creation_only || work->frame.function==0x816BA440 || work->frame.function==0x816BA668 ||
      work->frame.function==0x816B9A18 || work->frame.function==0x816B9BB8 ||
      work->frame.function==0x816B9AA8 || work->frame.function==0x816B7490 ||
      work->frame.function==0x816B7D98 || work->frame.function==0x816B7500 ||
      work->frame.function==0x816E6AB0 || work->frame.function==0x816B7B68 ||
      work->frame.function==0x816B7600;
  const auto frame=s.ReadFrame(c,at,ListenerOuter(at) || at==0x8199C630 || at==0x8199E3E0 ||
                              work->foreign_avatar_task,
                              (transport || ListenerOuter(at))?&caller_code:nullptr,work->task_release || work->system_thread_frame || work->system_discarded_identity_hint,
                              work->periodic_body_admitted && work->periodic_context?&*work->periodic_context:nullptr,
                              work->periodic_factory);
  if(!frame){
   s.RecordReturnPartial(c,at,10,__LINE__,diagnostic_thread,diagnostic_pcr);
   s.Lose(__LINE__);return;
  }
  if(work->system_discarded_identity_hint &&
     (!work->listener_arguments || !s.DiscardedIdentityHintCaller(*work,*work->listener_arguments,1))){
   s.Lose(__LINE__);return; // Never release a delivery pin after caller/code uncertainty.
  }
  // Read the actual restored Task outside metadata. Never install a parent,
  // synthesize TLS, reread a possibly freed packet, or borrow a new read ticket.
  const auto creation_current_task=work->creation_only && at==0x816B9D68 ?
      NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c):
      std::optional<uint32_t>{};
  const auto control_current=work->profile_control && work->profile_control->kind!=Tasks::ProfileControl::Kind::kDispatch?
      NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c):std::optional<uint32_t>{};
  const auto finite_current=work->system_thread_frame?
      NativeProfileIdentityTaskContext::Read(*s.config.catalog->calls(),*s.config.domain,at,c):std::optional<uint32_t>{};
  auto memory_read=global_critical_region::AcquireDirect();
  std::lock_guard lock(s.mutex);
  const auto* finite_node=finite_current?s.tasks.FindLiveTask(*finite_current):nullptr;
  const bool finite_ambient_matches=!work->system_thread_frame ||
      (finite_current.value_or(0)==work->system_frame_ambient_task &&
       (!work->system_frame_ambient_birth.generation ||
        (finite_node && Tasks::SameBirth(finite_node->birth,work->system_frame_ambient_birth) &&
         bool(finite_node->periodic_factory)==work->system_frame_ambient_periodic)));
  if(work->frame!=*frame || work->normal_return || !finite_ambient_matches ||
     work->caller_code.module.get()!=caller_code.module.get() ||
     work->caller_code.load_generation!=caller_code.load_generation ||
     bool(work->caller_code.lease)!=bool(caller_code.lease)){
   s.RecordReturnFailure(c,at,*frame,*work,work->birth,work->birth.generation?1u:0u,false,11,__LINE__);
   s.Lose(__LINE__);return;
  }
  bool exact=true;
  const auto value=uint32_t(c.r[3]);
  uint32_t return_phase=1,birth_kind=work->birth.generation?1:0;
  Tasks::Birth diagnostic_birth=work->birth;
  if(!birth_kind && work->external_birth.generation){diagnostic_birth=work->external_birth;birth_kind=2;}
  if(!birth_kind && work->release_generation){
   if(const auto* node=s.tasks.FindBirth(work->release_generation)){diagnostic_birth=node->birth;birth_kind=4;}
  }
  if(work->profile_control){
   const auto target=s.tasks.ObservedTransportSubject(work->birth.task);
   const auto parent=control_current?s.tasks.ObservedTransportSubject(*control_current):std::optional<Tasks::Birth>{};
   const auto* target_node=target?s.tasks.FindBirthConst(target->generation):nullptr;
   const auto* parent_node=parent?s.tasks.FindBirthConst(parent->generation):nullptr;
   exact=target && target_node && !target_node->delete_entered && !target_node->free_entered && !target_node->freed &&
       (work->profile_control->kind==Tasks::ProfileControl::Kind::kDispatch ||
        (parent && parent_node && !parent_node->delete_entered && !parent_node->free_entered && !parent_node->freed));
   if(exact && work->profile_control->kind==Tasks::ProfileControl::Kind::kCancel){
    const auto flags=Word(*s.kernel->memory(),target->task+8),data=Word(*s.kernel->memory(),target->task+12);
    exact=flags && data && *data==work->second && (*flags==work->output || *flags==(work->output&~0x10000000u)) &&
        s.tasks.ObserveProfileCancelState(*target,*flags,*data);
   }
   exact=exact && work->profile_control->Return(*frame,*target,parent.value_or(Tasks::Birth{}));
  }else if(at==0x8190A358 && work->periodic_role==State::PeriodicRole::kFactory){
   auto* factory=s.Periodic(work->periodic_factory);
   const auto handle=Word(*s.kernel->memory(),work->first+0xC),enabled=Word(*s.kernel->memory(),work->first+0x30);
   exact=factory && handle && enabled && s.PeriodicAllocationCurrent(factory->birth,factory->serial) &&
       factory->ReturnEnable(*frame,*handle,*enabled);
  }else if(at==0x819068D0 && work->periodic_role==State::PeriodicRole::kStop){
   auto* factory=s.Periodic(work->periodic_factory);
   const auto handle=Word(*s.kernel->memory(),work->first+0xC),enabled=Word(*s.kernel->memory(),work->first+0x30),busy=Word(*s.kernel->memory(),work->first+0x2C);
   exact=factory && handle && enabled && busy && factory->ReturnStop(*frame,*handle,*enabled,*busy);
  }else if(at==0x816B8B08 && work->periodic_role==State::PeriodicRole::kDispatcher){
   std::optional<std::array<uint32_t,2>> state;
   const auto* node=s.tasks.FindBirthConst(work->birth.generation);
   if(node && !node->free_entered && !node->freed){
    const auto flags=Word(*s.kernel->memory(),node->birth.task+8),period=Word(*s.kernel->memory(),node->birth.task+0xC);
    if(flags && period)state=std::array<uint32_t,2>{*flags,*period};
   }
   exact=s.tasks.ReturnPeriodicDispatch(*frame,work->birth,state);
  }else if((at==0x816B9F08 || at==0x816BA058) && work->periodic_factory){
   auto* factory=s.Periodic(work->periodic_factory);
   const bool native_return=at==0x816B9F08 || s.tasks.PeriodicWaitReturned(work->birth);
   exact=factory && s.PeriodicAllocationCurrent(work->birth,factory->serial) &&
       factory->ReturnStopChild(*frame,work->birth,native_return);
  }else if(at==0x816A2CF0 && work->creation_only){
   exact=work->creation && work->creation->ReturnStart(*frame);
  }else if(at==0x816A2A48 && work->creation_only){
   const auto parent=work->creation_parent;
   const auto output=int32_t(value)>=0?Word(*s.kernel->memory(),work->output):std::optional<uint32_t>{};
   exact=parent && parent->creation && parent->creation->ReturnCopy(*frame,value,output);
  }else if(at==0x816B9D68 && work->creation_only){
   const auto parent=work->creation_parent;
   const auto current=creation_current_task?s.tasks.FindLiveTask(*creation_current_task):nullptr;
   exact=parent && parent->creation && current && parent->creation->ReturnInline(*frame,current->birth);
  }else if(at==0x816A2588){
   exact=s.job && work->completion_route &&
       work->completion_route->StillOwned(s.job,s.job->serial,s.job->source,s.job->overlap,work->first) &&
       s.job->completion_frame==*frame && s.job->caller_published &&
       !s.job->completion_returned &&
       (s.job->initializer_completion_owner?
        work->initializer_completion_owner==s.job->initializer_completion_owner &&
        !work->initializer_completion_owner->normal_return &&
        s.Latest(frame->thread,0x8199E3E0)==work->initializer_completion_owner:
        s.job->completion_birth.generation==work->birth.generation);
   if(exact){
    s.job->completion_returned=true;
    OriginalLogonDiagnostic d;d.stage=OriginalLogonDiagnostic::kOverlapReturn;
    // Completion status comes from its authenticated error operands; do not
    // interpret the shared CompleteIO body's raw r3 residue as a result.
    d.entered=1;d.returned=1;
    d.completion_error_available=1;d.completion_error=s.job->completion_error;
    d.completion_extended=s.job->completion_extended;d.completion_length=s.job->completion_length;
    xam::ProfileManager::RecordNativeOriginalLogon(s.job->original_logon_observation,d);
   }
  }else if(ListenerOuter(at)){
   exact=s.listeners && work->listener_frame;
   if(exact && at==0x816A8498){
    exact=work->listener_registration && work->listener_allocation &&
        work->listener_allocation->listener_registration_entered &&
        !work->listener_allocation->listener_registered &&
        s.listeners->RegistrationReturned(*work->listener_registration,*work->listener_frame);
    if(exact)work->listener_allocation->listener_registered=true;
   }else if(exact && at==0x816A88E8){
    exact=work->listener_arguments && work->listener_allocation && work->listener_allocation->delete_entered;
    if(exact && work->listener_unregistered_destruction)
     exact=!work->listener_allocation->listener_registration_entered &&
           !work->listener_allocation->listener_registered;
    else if(exact)exact=s.listeners->DestructionReturned(work->listener_arguments->object,*work->listener_frame);
   }else if(exact)exact=work->listener_cell &&
       s.listeners->OuterReturned(work->listener_cell,work->listener_frame,value);
  }else if(at==0x81694B48 || at==0x816934F8 || at==0x81693568 ||
     at==0x81693CD8 || at==0x816921C8){
   const auto provider=s.FindProvider(work->first);
   exact=provider && provider->generation==work->provider_generation;
   if(exact && at==0x81694B48){
    const auto table=Word(*s.kernel->memory(),provider->object);
    const auto capacity=Word(*s.kernel->memory(),provider->object+0xC);
    const auto listener=Word(*s.kernel->memory(),provider->object+0x9C);
    exact=value==provider->object && table && *table==0x815F47C0 &&
        capacity && *capacity==0x1000 && listener && *listener==UINT32_MAX;
    if(exact)provider->constructing=false;
   }else if(exact && at==0x81693568){
    const auto buffer=Word(*s.kernel->memory(),provider->object+0xA8);
    const auto capacity=Word(*s.kernel->memory(),provider->object+0xC);
    const auto type=Word(*s.kernel->memory(),provider->object+0xD4);
    const auto listener=Word(*s.kernel->memory(),provider->object+0x9C);
    exact=buffer && capacity && type && listener;
    if(exact && int32_t(value)>=0 && *buffer &&
       ((*capacity==0x1000 && *type==0xB) || (*capacity==0x4000 && *type==0xA)) &&
       s.kernel->memory()->QueryRangeAccess(*buffer,*capacity)!=memory::PageAccess::kNoAccess){
     provider->buffer=*buffer;provider->capacity=*capacity;provider->type=*type;
     provider->listener=*listener;provider->initialized=true;provider->buffer_revoked=false;
     provider->image_epoch.Allocate(s.root,provider->generation,*buffer,*capacity);
     provider->image_output.reset();
    }
   }else if(exact && at==0x816934F8){
    const auto terminal=Word(*s.kernel->memory(),provider->object+0xB8);
    exact=terminal && *terminal!=997 &&
        std::none_of(s.active.begin(),s.active.end(),[&](const auto& other){
         return other!=work && other->provider_generation==provider->generation;
        });
    if(exact)provider->destroyed=true;
   }else if(exact && at==0x816921C8 && provider->last_output){
    const auto output=provider->last_output;
    const auto terminal=Word(*s.kernel->memory(),provider->object+0xB8);
    const auto extended=Word(*s.kernel->memory(),provider->object+0xD0);
    const auto ready=Word(*s.kernel->memory(),provider->object+0x90);
    const auto size=Word(*s.kernel->memory(),provider->object+0x94);
    const auto version=Qword(*s.kernel->memory(),provider->object+0xA0);
    if(output->published.load(std::memory_order_acquire) &&
       !output->status.load(std::memory_order_relaxed) && output->written.load(std::memory_order_relaxed) &&
       terminal && !*terminal && extended && !*extended && ready && *ready==provider->buffer &&
       size && *size && *size==output->required.load(std::memory_order_relaxed) && *size<=provider->capacity &&
       version && *version==output->version && output->provider_generation==provider->generation){
     provider->completed_source=output->source;provider->completed_version=*version;
     provider->completed_empty=false;provider->completed_status=0;
     provider->image_output=provider->image_epoch.Capture(provider->generation,
         output->source,*version,*size,
         output->image_first_allocation.load(std::memory_order_acquire),
         output->image_first_publication.load(std::memory_order_acquire));
    }else if(output->published.load(std::memory_order_acquire) &&
       int32_t(output->status.load(std::memory_order_relaxed))<0 &&
       !output->written.load(std::memory_order_relaxed) && terminal && *terminal!=997 &&
       extended && *extended==output->status.load(std::memory_order_relaxed) &&
       ready && !*ready && version && *version==output->version &&
       output->provider_generation==provider->generation){
      provider->completed_source=output->source;provider->completed_version=*version;
      provider->completed_empty=true;provider->completed_status=*extended;
      // Real negative read + native cleared output only. No image/readiness
      // success is fabricated and no old pointer is accepted as fresh output.
    }
   }
   if(exact && at==0x816921C8 && provider && work->image_key_candidate &&
      provider->image_uri_generation==work->image_uri_generation &&
      work->image_event<=UINT32_MAX-0x10 && work->image_modified){
    const auto key=Word(*s.kernel->memory(),work->image_event+0x10);
    const auto modified=Word(*s.kernel->memory(),work->image_modified);
    if(key && *key==provider->object+0x10 && modified && *modified==1){
     provider->image_key=work->image_key_candidate;
     provider->image_key_generation=provider->image_uri_generation;
     // The original Query can publish a real module-section default after a
     // failed native read. It is a DISTINCT successful output source, not a
     // HOST tile completion or an empty provider census. Join only this same
     // Query's authenticated fallback and actually published full memory key.
     const auto& input=*work->image_key_candidate;
     const auto module=s.config.catalog->calls()->module();
     if(DwordIs(c.r[3],0) && work->image_default_result &&
        *work->image_default_result==input && input.section && !input.provider &&
        input.valid() && input.root()==s.root && input.source()==work->source &&
        input.section->provider==provider->generation && provider->initialized &&
        !provider->revoked && !provider->buffer_revoked && !provider->destroyed &&
        module && s.root_code && s.config.domain && s.config.domain->active() &&
        s.config.domain->root()==NativeImportDomain::Member{module->hmodule_ptr(),module->load_generation()} &&
        input.section->module==module->hmodule_ptr() && input.section->module_generation==module->load_generation() &&
        s.ImageBytesCurrent(input)){
      const auto ready=Word(*s.kernel->memory(),provider->object+0x90);
      const auto size=Word(*s.kernel->memory(),provider->object+0x94);
      const auto version=Qword(*s.kernel->memory(),provider->object+0xA0);
      if(ready && size && version && *ready==input.address() && *size==input.size() &&
         *version==input.section->version &&
         s.kernel->memory()->QueryRangeAccess(input.address(),input.size())!=memory::PageAccess::kNoAccess &&
         s.kernel->GetUserModuleForAddress(input.address()).get()==module.get() &&
         s.kernel->GetUserModuleForAddress(input.address()+input.size()-1).get()==module.get() &&
         NativeMemoryKeyMatches(*s.kernel->memory(),provider->object+0x10,*ready,*size,*version)){
       provider->completed_source=input.source();provider->completed_version=*version;
       provider->completed_empty=false;provider->completed_status=0;
      }
     }
    }
   }
   if(exact && at==0x816921C8 && provider && work->image_no_read_entry &&
      DwordIs(c.r[3],0) && work->frame.function==0x816921C8 && work->frame.root==s.root &&
      work->serial && work->source && work->birth.generation && work->birth.origin && work->birth.task &&
      provider->generation==work->provider_generation &&
      provider->image_uri_generation==work->image_no_read_entry->uri_generation &&
      work->image_uri_generation==work->image_no_read_entry->uri_generation &&
      !work->image_key_candidate && !work->image_default_result && s.ProviderImageDataAbsent(*provider)){
    const auto pending=Byte(*s.kernel->memory(),provider->object+0x98);
    const auto ready=Word(*s.kernel->memory(),provider->object+0x90);
    const auto version=Qword(*s.kernel->memory(),provider->object+0xA0);
    if(pending && !*pending && ready && !*ready && version &&
       *version==work->image_no_read_entry->version){
     // Same full A0 proves that the actual refresh/read branch was not
     // taken, even if a nested read observer would have refused admission.
     // No output/source/readiness receipt is minted. The owning Work remains
     // active until ALL existing whole-return/read/listener joins succeed.
     provider->no_read_query=State::Provider::NoReadQuery{*frame,work->birth,work->serial,
         work->source,provider->generation,work->image_no_read_entry->uri_generation,*version};
    }
   }
  }else if(at==0x816B9A18){
   const auto output=Word(*s.kernel->memory(),work->output);
   std::optional<Tasks::Birth> allocated;
   if(work->schedule)allocated=s.tasks.ReturnAllocation(work->schedule,*frame,value,output);
   else if(work->detached_allocation)allocated=s.tasks.ReturnDetachedAllocation(work->detached_allocation,*frame,value,output);
   if(work->creation_only){
    const auto inlined=work->creation_parent;
    const auto start=inlined?inlined->creation_parent:nullptr;
    exact=start && start->creation && start->creation->ReturnAllocation(*frame);
   }
   if(allocated && work->periodic_factory){
    const auto flags=Word(*s.kernel->memory(),allocated->task+8),period=Word(*s.kernel->memory(),allocated->task+0xC);
    exact=exact && flags && period && s.tasks.BindPeriodicAllocation(work->schedule,*frame,*allocated,{*flags,*period});
   }
   if(allocated){diagnostic_birth=*allocated;birth_kind=3;}
   const auto* producer=work->schedule?s.tasks.FindSchedule(work->schedule):nullptr;
   const auto inline_producer=work->creation_parent?work->creation_parent:
       (frame->lr==0x816B9E08?s.Latest(frame->thread,0x816B9D68):nullptr);
   const auto producer_frame=producer?producer->frame:inline_producer?inline_producer->frame:Frame{};
   if(producer_frame.valid())allocation_fact=AllocationReturnFact(*frame,producer_frame,value,allocated);
   if(allocated && c.thread_state){
    auto* d=s.OwnTaskDiagnostic(c.thread_state->native_identity_task_diagnostic_cell_.load(std::memory_order_acquire));
    if(d && d->context==&c && d->value.allocation_task==allocated->task){
     d->value.allocated_birth=allocated->generation;d->value.available|=256;
    }
   }
  }else if(at==0x816BA440){
   std::optional<std::array<uint32_t,2>> native_state;
   if((work->profile_producer || work->periodic_factory) && int32_t(value)>=0){
    const auto* call=s.tasks.FindSchedule(work->schedule);
    const auto* node=call?s.tasks.FindBirthConst(call->birth):nullptr;
    if(node && !node->freed && !node->delete_entered && !node->free_entered &&
       (!call->producer || !call->producer->release_started)){
     const auto flags=Word(*s.kernel->memory(),node->birth.task+8),data=Word(*s.kernel->memory(),node->birth.task+12);
     if(flags && data)native_state=std::array<uint32_t,2>{*flags,*data};
    }
   }
   exact=s.tasks.ReturnSchedule(work->schedule,*frame,value,native_state);
   if(work->periodic_factory){auto* factory=s.Periodic(work->periodic_factory);
    exact=exact && factory && factory->ReturnSchedule(*frame,value,exact);}
  }
  else if(at==0x816BA668)exact=s.tasks.ReturnPersistentCreate(work->schedule,*frame,value,Word(*s.kernel->memory(),work->output));
  else if(at==0x816B9AA8){
   exact=work->execution_options?s.tasks.ReturnExecutionOnlyTimedOptions(*frame,work->birth):
       s.tasks.ReturnPersistentCallbackBinding(*frame);
   if(work->execution_options){
    // Never join return r3/r7 as entry Task/options: Modify returns residue.
    State::ModifyOptionsPayload d;d.stage=3;d.line=__LINE__;d.available=2|8|256;
    d.thread=work->frame.thread;d.process=work->thread->native_frame_thread_identity().process_type;
    d.pcr=work->frame.pcr;d.sp=work->frame.sp;d.lr=work->frame.lr;
    d.task=work->first;d.mask=work->second;d.options=work->output;
    d.origin=work->birth.origin;d.generation=work->birth.generation;d.protected_scope=true;
    if(const auto* node=s.tasks.FindBirth(work->birth.generation)){
     d.available|=4;d.execution_only=s.tasks.IsExecutionOnlyOptionsSubject(node->birth.generation);
     d.profile=s.tasks.IsProfileTransportSubject(node->birth.generation);
     d.node_flags=uint32_t(node->created_persistent)|(uint32_t(node->resident_physical)<<1)|
         (uint32_t(node->transport_only)<<2)|(uint32_t(node->continuation_ticket)<<3)|
         (uint32_t(node->foreign_avatar)<<4);
    }
    d.raw_return_residue=c.r[3];d.own_return_matched=exact;
    s.first_modify_options_return.Capture(d);
   }
  }
  else if(at==0x816B7490){
   exact=work->reference_only || s.tasks.ReturnClose(*frame,value);
   if(exact && !value && !work->reference_only){
    const auto* node=s.tasks.FindBirthConst(work->release_generation);
    if(node && node->execution_only)if(const auto lifetime=s.ExecutionLifetimeFor(node->birth))lifetime->native_freed=true;
   }
   if(work->reference_release)exact=exact && work->reference_release->receipt.ReturnRelease(*frame,c.r[3]);
   if(work->periodic_role==State::PeriodicRole::kOwnerClose){
    auto* factory=s.Periodic(work->periodic_factory);
    exact=exact && factory && factory->ReturnStopChild(*frame,work->birth,exact);
   }
   if(work->task_release && !work->reference_only){
    const auto e=std::find_if(s.expected.begin(),s.expected.end(),[&](const auto& e){
     return e && e->serial==work->release_receipt && e->task_release && e->actual==*frame;
    });
    exact=e!=s.expected.end() && (*e)->task_release->birth.generation==work->release_generation &&
        (*e)->task_release->Returned(*frame,value,exact);
   }
  }
  else if(at==0x816B7D98 || at==0x816B7500)exact=s.tasks.ReturnDeletingDestructor(*frame);
  else if(at==0x816E6AB0)exact=s.tasks.ReturnTaskFree(*frame);
  else if(work->owns_task_work)exact=s.tasks.ReturnWork(*frame,work->birth);
  else if(work->owns_persistent_work)exact=s.tasks.ReturnPersistentWork(*frame,work->birth);
  else if(at==0x816E3A98 && work->wake_work)exact=s.wake && s.wake->ReturnRefreshChild(*work->wake_work,*frame);
  else if(at==0x816F2618 && work->wake_work)exact=s.wake && bool(s.wake->ReturnWorker(*work->wake_work,*frame));
  else if(at==0x816B7600 && work->wake){
   exact=s.wake && work->tail_reschedule && work->tail_wake &&
       s.wake->ReturnNativeReschedule(*work->wake,work->tail_reschedule->frame);
  }
  if(exact && at==0x816B9A18 && work->periodic_factory){
   auto* factory=s.Periodic(work->periodic_factory);
   const auto* call=s.tasks.FindSchedule(work->schedule);
   const auto* node=call?s.tasks.FindBirth(call->birth):nullptr;
   exact=factory && node && node->periodic_factory==factory->serial &&
       node->periodic_allocation && factory->ObserveBirth(node->birth);
  }
  if(exact && work->external_birth.generation){
   exact=s.tasks.ReturnExternalWork(*frame,work->external_birth);if(!exact)return_phase=2;
  }
  if(exact && work->listener_work){
   work->listener_dependencies=s.listeners->CaptureDependencies(*work->listener_work);
   const Events::Root root{s.root.kernel,s.root.domain,s.root.generation};
   const Events::WorkKey key{root,work->birth.generation,work->serial,frame->thread,
       frame->pcr,work->birth.task,frame->sp,frame->lr,at,
       work->system_thread_frame && !work->external_birth.generation?Events::WorkOwnerKind::kSystemThreadFrame:Events::WorkOwnerKind::kTask};
   exact=work->listener_dependencies && s.listeners->FinishWholeWorkNormally(*work->listener_work,key,work->listener_source);
   if(!exact)return_phase=work->listener_dependencies?4:3;
  }
  if(work->commit){
   const bool before_commit=exact;
   void* expected=work->commit.get();
   exact=exact && work->commit->publisher_returned.load(std::memory_order_acquire) &&
       work->commit->phase.load(std::memory_order_acquire)==State::Commit::Phase::kPublished &&
       c.thread_state->native_identity_publication_cell_.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel);
   if(before_commit && !exact)return_phase=5;
  }
  if(!exact || s.tasks.lost() || (s.wake && s.wake->lost() && !work->task_release)){
   s.RecordReturnFailure(c,at,*frame,*work,diagnostic_birth,birth_kind,exact,
       !exact?return_phase:s.tasks.lost()?6u:7u,__LINE__);
   s.Lose(__LINE__);work->uncertain=true;return;
  }
  if(allocation_fact)allocation_cut.value=*allocation_fact;
  if(work->read && work->execution_read_fallback && !s.RestoreOwnedReadOnUnwind(work)){
   work->uncertain=true;return; // retain this real data owner, not global neutral poison
  }
  if(work->read && !work->read->Leave()){
   s.RecordReturnFailure(c,at,*frame,*work,diagnostic_birth,birth_kind,false,12,__LINE__);
   s.Lose(__LINE__);work->uncertain=true;return;
  }
  if(!s.RecycleRead(work)){
   s.RecordReturnFailure(c,at,*frame,*work,diagnostic_birth,birth_kind,false,13,__LINE__);
   s.Lose(__LINE__);work->uncertain=true;return;
  }
  work->normal_return=true;
  if(work->registered_executor){
   const auto expected=std::find_if(s.expected.begin(),s.expected.end(),[&](const auto& e){return e->actual==*frame;});
   if(expected==s.expected.end() || (*expected)->returned){
    s.RecordReturnFailure(c,at,*frame,*work,diagnostic_birth,birth_kind,false,14,__LINE__);
    s.Lose(__LINE__);return;
   }
   (*expected)->returned=true;
  }
  if(work->commit){
   const auto& commit=*work->commit;
   const auto job=s.job;
   const bool protocol=at==0x8199C838 && job && !job->execution_uncertain &&
       !job->protocol_returned && work->source==job->source &&
       commit.intent==job->intent && commit.publication==job->publication &&
       commit.owner==&s && commit.context==&c && commit.thread_state==c.thread_state &&
       commit.source==job->source && commit.claimed_nonce==s.config.factory_binding->nonce() &&
       commit.worker_birth==work->birth.generation && commit.whole_work==work->serial &&
       commit.publisher_returned.load(std::memory_order_acquire) &&
       commit.phase.load(std::memory_order_acquire)==State::Commit::Phase::kPublished;
   if(!protocol){
    s.RecordReturnFailure(c,at,*frame,*work,diagnostic_birth,birth_kind,false,17,__LINE__);
    s.Lose(__LINE__);work->uncertain=true;return;
   }
   job->protocol_work=work->serial;job->protocol_frame=*frame;
   job->protocol_birth=work->birth;job->protocol_value=value;
   job->protocol_returned=true;
   {OriginalLogonDiagnostic d;d.stage=OriginalLogonDiagnostic::kProtocolReturn;
    d.entered=1;d.returned=1;d.result_available=1;d.result=value;
    xam::ProfileManager::RecordNativeOriginalLogon(job->original_logon_observation,d);}
  }
  const auto retire=[&](std::shared_ptr<State::Work>& item){
   const auto i=std::find(s.active.begin(),s.active.end(),item);
   if(i==s.active.end()){
    s.RecordReturnFailure(c,at,item->frame,*item,item->birth,item->birth.generation?1u:0u,false,15,__LINE__,2);
    s.Lose(__LINE__);return false;
   }
   // Allocation failure leaves active owning the exact Work. On success,
   // shared_ptr's noexcept move empties this local/tail before the active
   // reference is removed, with no post-move dereference or destructor here.
   s.host_retired.push_back(std::move(item));s.active.erase(i);return true;
  };
  if(work->tail_reschedule){work->tail_reschedule->normal_return=true;if(!retire(work->tail_reschedule))return;}
  if(work->tail_wake){work->tail_wake->normal_return=true;if(!retire(work->tail_wake))return;}
  if(!retire(work))return;
 }catch(...){if(const auto owner=Lookup(c.kernel_state)){
  owner->state_->RecordReturnPartial(c,at,16,__LINE__);owner->state_->Lose(__LINE__);
 }}
}

uint64_t NativeProfileIdentityOwner::EnterPreboundNativePublisher(void* raw) noexcept {
 if(!raw)return 1;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 auto* cell=c.thread_state?static_cast<State::Commit*>(
     c.thread_state->native_identity_publication_cell_.load(std::memory_order_acquire)):nullptr;
 if(!cell){
  // No lookup/lock may occur here. The cached claimed bit can only DENY; it
  // never supplies a source, Task, native root or a publication permission.
  return c.kernel_state && c.kernel_state->native_profile_identity_claimed_.load(std::memory_order_acquire)?Abort(c):1;
 }
 if(cell->owner)cell->owner->RecordPublisherAbort(*cell,c);
 const bool exact=cell->owner && cell->owner->kernel==c.kernel_state &&
     cell->context==&c && cell->thread_state==c.thread_state &&
     (DwordIs(c.lr,0x8199DBEC) || DwordIs(c.lr,0x8199DA38)) && DwordIs(c.r[13],cell->pcr) &&
     DwordIs(c.r[1],cell->publisher_sp) && cell->publisher_sp>=0x180 &&
     DwordIs(c.r[3],cell->input) &&
     cell->request_flags && cell->source && cell->claimed_nonce && cell->worker_birth &&
     cell->whole_work && !cell->owner->uncertain.load(std::memory_order_acquire);
 if(!exact)return (cell->owner?cell->owner->Deny(c,0x816DC628u,__LINE__):Abort(c));
 const bool canceled=DwordIs(c.lr,0x8199DA38);
 const uint32_t title=canceled && cell->native_title_context?NativeAtomicWord(cell->native_title_context):cell->title_context;
 if(!DwordIs(c.r[5],title) || (canceled &&
     (!cell->canceled_title_pointer || !cell->default_provider || !cell->default_vtable || !cell->default_accessor ||
      NativeAtomicWord(cell->canceled_title_pointer)!=0x81AA28D8u ||
      NativeAtomicWord(cell->default_provider)!=0x81A8302Cu ||
      NativeAtomicWord(cell->default_vtable)!=0x815FB150u ||
      NativeAtomicWord(cell->default_accessor)!=0x816DF990u)))
  return (cell->owner?cell->owner->Deny(c,0x816DC628u,__LINE__):Abort(c));
 const auto actual_flags=NativeAtomicWord(cell->request_flags);
 if((actual_flags&~0x83u)!=(cell->original_flags&~0x3u) ||
    !DwordIs(c.r[4],actual_flags|0x00400000u))return (cell->owner?cell->owner->Deny(c,0x816DC628u,__LINE__):Abort(c));
 for(size_t i=0;i<4;++i){
  if(!cell->input_flags[i] || !cell->input_xuids[i])return (cell->owner?cell->owner->Deny(c,0x816DC628u,__LINE__):Abort(c));
  const auto flags=NativeAtomicWord(cell->input_flags[i]);
  if(canceled ? flags!=0 || NativeAtomicQword(cell->input_xuids[i])!=0 :
     NativeAtomicQword(cell->input_xuids[i])!=cell->desired[i] ||
     bool(flags&1u)!=bool(cell->desired[i]) || (flags&2u))
   return (cell->owner?cell->owner->Deny(c,0x816DC628u,__LINE__):Abort(c));
 }
 bool no=false;
 if(!cell->publisher_entered.compare_exchange_strong(no,true,std::memory_order_acq_rel))return (cell->owner?cell->owner->Deny(c,0x816DC628u,__LINE__):Abort(c));
 cell->publisher_lr=uint32_t(c.lr);cell->publisher_title_context=title;
 cell->outcome=DwordIs(c.lr,0x8199DA38)?xam::ProfileRosterPublicationOutcome::kCanceledEmpty:
     xam::ProfileRosterPublicationOutcome::kRequested;
 return 1;
}
void NativeProfileIdentityOwner::ReturnPreboundNativePublisher(void* raw) noexcept {
 if(!raw)return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 auto* cell=c.thread_state?static_cast<State::Commit*>(
     c.thread_state->native_identity_publication_cell_.load(std::memory_order_acquire)):nullptr;
 if(!cell || !cell->owner || cell->context!=&c || cell->thread_state!=c.thread_state ||
    cell->owner->kernel!=c.kernel_state)return;
 bool no=false;
 if(!cell->publisher_entered.load(std::memory_order_acquire) ||
    !cell->publisher_lr || !DwordIs(c.lr,cell->publisher_lr) || !DwordIs(c.r[13],cell->pcr) ||
    !DwordIs(c.r[1],cell->publisher_sp) ||
    !cell->publisher_returned.compare_exchange_strong(no,true,std::memory_order_acq_rel))
  cell->owner->Lose(__LINE__);
}
uint64_t NativeProfileIdentityOwner::PublishPreboundNativeCache(void* raw) noexcept {
 if(!raw)return 1;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 // All other native dispatch messages and native0A bytes remain unmodified.
 if(!DwordIs(c.lr,0x816DC814))return 1;
 auto* cell=c.thread_state?static_cast<State::Commit*>(
     c.thread_state->native_identity_publication_cell_.load(std::memory_order_acquire)):nullptr;
 if(!cell)return c.kernel_state && c.kernel_state->native_profile_identity_claimed_.load(std::memory_order_acquire)?Abort(c):1;
 const bool exact=cell->owner && cell->owner->kernel==c.kernel_state &&
     cell->context==&c && cell->thread_state==c.thread_state &&
     DwordIs(c.r[13],cell->pcr) && cell->publisher_sp>=0x180 &&
     DwordIs(c.r[1],cell->publisher_sp-0x180) &&
     DwordIs(c.r[3],0x8000000E) && DwordIs(c.r[4],0xA) &&
     cell->source && cell->claimed_nonce && cell->worker_birth && cell->whole_work &&
     cell->publication && cell->wake && cell->native_title_context &&
     cell->publisher_entered.load(std::memory_order_acquire) &&
     !cell->publisher_returned.load(std::memory_order_acquire) &&
     !cell->owner->uncertain.load(std::memory_order_acquire);
 bool copied=exact;
 if(copied)for(size_t i=0;i<4;++i){
  if(!cell->native_flags[i] || !cell->native_xuids[i]){copied=false;break;}
  const auto flags=NativeAtomicWord(cell->native_flags[i]);
  if(cell->outcome==xam::ProfileRosterPublicationOutcome::kCanceledEmpty?
     flags!=0 || NativeAtomicQword(cell->native_xuids[i])!=0 :
     NativeAtomicQword(cell->native_xuids[i])!=cell->desired[i] ||
     bool(flags&1u)!=bool(cell->desired[i]) || (flags&2u)){copied=false;break;}
 }
 copied=copied && NativeAtomicWord(cell->native_title_context)==cell->publisher_title_context;
 auto expected=State::Commit::Phase::kArmed;
 if(!copied || !cell->phase.compare_exchange_strong(expected,State::Commit::Phase::kPublishing,std::memory_order_acq_rel) ||
    !cell->publication->PublishFromNative(cell->outcome) || !cell->wake->MarkNativeCachePublished(cell->source)){
  cell->phase.store(State::Commit::Phase::kUncertain,std::memory_order_release);
  if(cell->owner)cell->owner->Lose(__LINE__);
  return (cell->owner?cell->owner->Deny(c,0x816BD420u,__LINE__):Abort(c)); // Native cache may have changed: no fallback or rollback.
 }
 cell->owner->published_source.store(cell->source,std::memory_order_release);
 cell->phase.store(State::Commit::Phase::kPublished,std::memory_order_release);
 return 1;
}
} // namespace xe::kernel
