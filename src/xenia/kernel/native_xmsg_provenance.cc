/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_xmsg_provenance.h"
#include "xenia/kernel/native_xmsg_provenance_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {
using Frame=NativeXmsgProvenance::FrameFact;
using Root=NativeXmsgProvenance::RootFact;
using Module=NativeXmsgProvenance::ModuleFact;
using Form=NativeXmsgProvenance::ProducerForm;
constexpr uint32_t kRequest=0x816A3128, kRequestEx=0x816A31C8,
    kStartWrapper=0x816A2F80,kStart=0x816A2CF0,kCopy=0x816A2A48,
    kInProcess=0x816A2F88,kInvoke=0x816A29C0,kCallback=0x816A2C80,
    kSchedule=0x816BA440,kInline=0x816B9D68,kAllocate=0x816B9A18,
    kFree=0x816E6AB0,kEnvelopeBytes=0x1C;
constexpr uint32_t kIdentityServiceProducer=0x81830878,kIdentityServiceAccept=0x818CD588,
    kIdentityServiceComplete=0x816A2588,kIdentityServiceCancel=0x816A26F8;
constexpr size_t kCaptureLimit=128,kProviderDepthLimit=256;
constexpr uint32_t kCopyBudgetLimit=1024*1024;
struct Code {uint32_t address,bytes;const char* hash;};
// Complete selected original bodies, not guessed callback allowlists. The
// eight-byte direct Start leaf is its actual tail wrapper, not a new frame.
constexpr Code kCode[]={
 {kIdentityServiceProducer,352,"2d25796d52b308dec369b0f1e2de415d0688950364c1cfc3cbcd042118ab230e"},
 {kIdentityServiceAccept,252,"c8980e538f08e68fb8a1202e4e3b447d01388d885abb38d83dc3e06ecc096361"},
 {kIdentityServiceComplete,364,"cb8e83d7817263c223f0ac9719175540bbd78746a39ce1b5a38221550bf953d1"},
 {kIdentityServiceCancel,352,"b1af64f353766148e34db02ed938743371561d2ff3d694bb5a8209e5c205051a"},
 {kRequest,160,"eff55fae08849d9f77335f79e43d95faf1c060280b9eeecdef31e403661469e0"},
 {kRequestEx,192,"b5050cffde4ee93439285e1ffbd59772e0b93ccb760f6ca08819e76f80d48f9a"},
 {kStartWrapper,8,"63324809f1fc1f0af90fe10acf82eb80b80f5852144749f4a1341a5a4890a292"},
 {kStart,656,"e00dfeaa77f3f52c68a5f89f5bea3f6a259c38f2fb3e15ae02f922803f75339a"},
 {kCopy,128,"7ebaa0f5422b037352ac5cfb6a5bd2a3954d4e6fdc3901d5e00a9f0d3c4f053d"},
 {kInProcess,108,"54aa9344cb2e1d5b8b6999a513a4356308f1b87452aefe73d1ad574b4b2b3e74"},
 {kInvoke,136,"c2eba3f74a1f3c6c838caf8c35345c5dd68f63e3bb20335c06dad73dcaf8c1ce"},
 {0x816A2B90,116,"3dfc6fc1d67c11edbebfa7dbcf4a547ac510bb742d6c567fc6ef5a00d70b0bf8"},
 {0x816A2C08,120,"ad5b30fd51636ad75b859557d4057f0d258be540077f7a632938a7cd3898baae"},
 {kCallback,108,"21da95341366e83781ab2e46696f0c04afa43d71ae7c11f91dc8e108d1b1db94"},
 {kFree,96,"cd4506be3ef237ff9fa71650947f227043d3e76d06559fea0ed12f6693a78149"},
};
constexpr std::array<NativeXmsgProvenance::InstructionSite,1> kSites{{
 {kInvoke,0x816A2A1C,0x7D6903A6}
}};
std::mutex registry_mutex;
std::map<KernelState*,std::weak_ptr<NativeXmsgProvenance>> registry;
std::optional<uint32_t> Dword(uint64_t raw) noexcept {
 const auto low=uint32_t(raw);
 if(raw==low || raw==uint64_t(int64_t(int32_t(low))))return low;
 return {};
}
std::optional<uint32_t> Word(Memory& memory,uint32_t address) {
 std::array<uint8_t,4> bytes;
 if(!internal::ReadNativeWaitSnapshot(memory,address,bytes))return {};
 return xe::load_and_swap<uint32_t>(bytes.data());
}
uint32_t Value(std::span<const uint8_t> bytes,size_t offset) {
 return xe::load_and_swap<uint32_t>(bytes.data()+offset);
}
bool SameThread(const Frame& a,const Frame& b) noexcept {
 return a.thread==b.thread && a.pcr==b.pcr;
}
bool EnteredFrom(const Frame& entered,const Frame& parent,uint32_t instruction,
                 uint32_t depth) noexcept {
 return SameThread(entered,parent) && parent.sp>=depth &&
     entered.sp==parent.sp-depth && entered.lr==instruction+4;
}
std::optional<Frame> Actual(KernelState& kernel,cpu::ppc::PPCContext& c,
                            uint32_t function) {
 auto* thread=XThread::IsInThread()?XThread::GetCurrentThread():nullptr;
 const auto sp=Dword(c.r[1]),lr=Dword(c.lr),pcr=Dword(c.r[13]);
 if(!thread || !XThread::IsInThread(thread) || thread->kernel_state()!=&kernel ||
    c.kernel_state!=&kernel || c.processor!=kernel.processor() ||
    c.thread_state!=thread->thread_state() || c.thread_state->context()!=&c ||
    !sp || *sp<0x200 || (*sp&15) || !lr || *lr<4 || (*lr&3) ||
    !pcr || *pcr!=thread->pcr_ptr())return {};
 const auto process=thread->native_frame_thread_identity().process_type;
 if(process!=X_PROCTYPE_TITLE && process!=X_PROCTYPE_SYSTEM)return {};
 return Frame{thread->thread_id(),*pcr,*sp,*lr,function};
}
bool SameExecution(const NativeProfileIdentityOwner::TaskExecutionObservation& a,
                   const NativeProfileIdentityOwner::TaskExecutionObservation& b) {
 return a.kernel==b.kernel && a.domain==b.domain && a.root_generation==b.root_generation &&
  a.birth_generation==b.birth_generation && a.invocation==b.invocation &&
  a.module_generation==b.module_generation && a.task==b.task && a.thread==b.thread &&
  a.pcr==b.pcr && a.dispatcher_sp==b.dispatcher_sp && a.body_sp==b.body_sp &&
  a.callback==b.callback && a.argument==b.argument && a.module==b.module && a.callback_lr==b.callback_lr;
}
bool Hash(std::span<const uint8_t> bytes,const char* expected) {
 if(!expected || std::char_traits<char>::length(expected)!=64)return false;
 std::array<uint8_t,32> digest{};
 if(mbedtls_sha256(bytes.data(),bytes.size(),digest.data(),0))return false;
 constexpr char hex[]="0123456789abcdef";
 for(size_t n=0;n<digest.size();++n)
  if(expected[n*2]!=hex[digest[n]>>4] || expected[n*2+1]!=hex[digest[n]&15])return false;
 return true;
}
}  // namespace

NativeXmsgProvenance::State::State(Config value):config(std::move(value)),
    kernel(config.catalog->calls()->kernel()),
    root{config.allocation->owner().kernel_epoch,config.domain->id(),config.domain->root().generation}{}
NativeXmsgProvenance::State::~State() {
 for(const auto& weak:records)if(const auto record=weak.lock())
  record->facts_available.store(false,std::memory_order_release);
}
bool NativeXmsgProvenance::State::CodeCurrent(uint32_t at) const {
  const auto found=std::find_if(std::begin(kCode),std::end(kCode),[&](const Code& c){return c.address==at;});
  if(found==std::end(kCode) || !kernel || !config.catalog || !config.domain || !config.domain->active() ||
     !config.allocation || !config.allocation->MatchesDomain(config.domain))return false;
  const auto calls=config.catalog->calls();const auto module=calls?calls->module():nullptr;
  if(!module || !module->xex_module() || !module->native_import_ready() ||
     calls->load_generation()!=root.generation || module->load_generation()!=root.generation ||
     config.domain->root()!=NativeImportDomain::Member{module->hmodule_ptr(),root.generation})return false;
  auto read=global_critical_region::AcquireDirect();auto& memory=*kernel->memory();
  if(memory.QueryRangeAccess(at,found->bytes)==memory::PageAccess::kNoAccess)return false;
  for(uint32_t n=0;n<found->bytes;n+=4){xex2_section_type type;
   if(kernel->GetUserModuleForAddress(at+n).get()!=module.get() ||
      kernel->processor()->LookupModule(at+n)!=module->xex_module() ||
      !module->xex_module()->GetPageSectionType(at+n,&type) || type!=XEX_SECTION_CODE ||
      module->xex_module()->TranslateCode(at+n)!=memory.TranslateVirtual<const uint8_t*>(at+n))return false;
  }
  return Hash({memory.TranslateVirtual<const uint8_t*>(at),found->bytes},found->hash);
 }
std::optional<Module> NativeXmsgProvenance::State::ModuleAt(uint32_t at,bool require_domain) const {
  if(at<4 || (at&3))return {};
  auto read=global_critical_region::AcquireDirect();const auto module=kernel->GetUserModuleForAddress(at);
  xex2_section_type type;
  if(!module || !module->xex_module() || !module->load_generation() ||
     (require_domain && !config.domain->Contains({module->hmodule_ptr(),module->load_generation()})) ||
     kernel->processor()->LookupModule(at)!=module->xex_module() ||
     !module->xex_module()->GetPageSectionType(at,&type) || type!=XEX_SECTION_CODE ||
     module->xex_module()->TranslateCode(at)!=kernel->memory()->TranslateVirtual<const uint8_t*>(at))return {};
  return Module{module->hmodule_ptr(),module->load_generation()};
 }
void NativeXmsgProvenance::State::Prune() {
  records.erase(std::remove_if(records.begin(),records.end(),[](const auto& r){return r.expired();}),records.end());
  // A historical fact owns values only. No source/File/module/native owner
  // can be destroyed by this metadata retirement under the mutex.
  providers.erase(std::remove_if(providers.begin(),providers.end(),[](const Provider& p){return p.returned;}),providers.end());
 }
NativeXmsgProvenance::State::Provider* NativeXmsgProvenance::State::Innermost(const Frame& frame,const void* actual_thread) {
  for(auto i=providers.rbegin();i!=providers.rend();++i)
   if(!i->returned && i->actual_thread==actual_thread && SameThread(i->frame,frame))return &*i;
  return nullptr;
 }
NativeXmsgProvenance::State::Provider* NativeXmsgProvenance::State::CurrentProvider(
    const Record& record,const Frame& frame,const void* actual_thread) {
 if(provider_overflow || !Live(record) || record.owner.lock().get()!=this)return nullptr;
 auto* current=Innermost(frame,actual_thread);
 if(!current || current->unavailable || current->record.lock().get()!=&record || !current->fact ||
    current->frame.sp<0x80 || frame.sp>current->frame.sp-0x80)return nullptr;
 return current;
}
bool NativeXmsgProvenance::State::PublicArguments(const Record& r,cpu::ppc::PPCContext& c,bool start_form) const {
  const auto& p=r.producer;
  return Dword(c.r[3])==std::optional(p.app) && Dword(c.r[4])==std::optional(p.message) &&
    Dword(c.r[start_form?6:5])==std::optional(p.argument) &&
    Dword(c.r[start_form?7:6])==std::optional(p.argument_bytes) &&
    Dword(c.r[start_form?5:7])==std::optional(p.overlap);
 }
bool NativeXmsgProvenance::State::Execution(const Record& r,const NativeProfileIdentityOwner::TaskExecutionObservation& x) const {
  const auto module=config.catalog->calls()->module();
  return x.kernel==root.kernel && x.domain==root.domain && x.root_generation==root.generation &&
   x.birth_generation && x.birth_generation==r.birth_generation && x.task==r.task &&
   x.callback==kCallback && x.argument==r.envelope && x.invocation &&
   x.module==module->hmodule_ptr() && x.module_generation==root.generation &&
   x.callback_lr==(r.delivery==Delivery::kQueuedTask?0x816B8B64u:0x816B7EA4u);
 }

std::shared_ptr<const std::vector<uint8_t>> NativeXmsgProvenance::State::ReadCopiedPayload(
    Memory& memory,uint32_t address,uint32_t size) {
 if(size>kCopyBudgetLimit || uint64_t(address)+size>uint64_t(UINT32_MAX)+1)return {};
 auto bytes=std::make_shared<std::vector<uint8_t>>(size);
 if(size && !internal::ReadNativeWaitSnapshot(memory,address,*bytes))return {};
 return bytes; // Owned bytes only: no raw pointer/page/allocator pin escapes.
}
void NativeXmsgProvenance::State::Revoke(uint32_t pointer) noexcept {
 if(!pointer)return;
 for(const auto& weak:records)if(const auto r=weak.lock();r && !r->released &&
     (pointer==r->envelope || pointer==r->task)){
  r->revoked=true;r->facts_available.store(false,std::memory_order_release);++r->version;
 }
}
bool NativeXmsgProvenance::State::BindCopiedPayload(Record& r,uint64_t expected_version,
    bool success,bool valid,uint32_t envelope,uint32_t copied_argument,
    std::shared_ptr<const std::vector<uint8_t>> payload) noexcept {
 if(!Live(r) || !r.copy_entered || r.version!=expected_version || r.copy_returned)return false;
 r.copy_returned=true;
 if(!valid || (success && (!envelope || (envelope&3) ||
     (r.identity_service_metadata_only ? (payload || r.producer.argument_bytes!=0x568) :
       (!payload || payload->size()!=r.producer.argument_bytes)) ||
     uint64_t(envelope)+kEnvelopeBytes+r.producer.argument_bytes>uint64_t(UINT32_MAX)+1 ||
     copied_argument!=(r.producer.argument?envelope+kEnvelopeBytes:0) ||
     !next_copy || next_copy==UINT64_MAX))){r.unavailable=true;r.facts_available.store(false,std::memory_order_release);return false;}
 if(success){r.copy_serial=next_copy++;r.envelope=envelope;r.copied_argument=copied_argument;
  r.copied_bytes=std::move(payload);}
 ++r.version;return true;
}

NativeXmsgProvenance::NativeXmsgProvenance(std::shared_ptr<State> state)
    :state_(std::move(state)){}
std::shared_ptr<NativeXmsgProvenance> NativeXmsgProvenance::Lookup(KernelState* kernel) {
 if(!kernel)return {};
 std::lock_guard lock(registry_mutex);const auto found=registry.find(kernel);
 return found==registry.end()?nullptr:found->second.lock();
}
std::shared_ptr<NativeXmsgProvenance> NativeXmsgProvenance::CreateBeforeWorkers(
    Config config,std::string* error) {
 const auto fail=[&](const char* why)->std::shared_ptr<NativeXmsgProvenance>{if(error)*error=why;return {};};
 if(!config.catalog || !config.domain || !config.allocation)return fail("Missing actual XMsg root owners");
 auto state=std::make_shared<State>(std::move(config));
 if(!state->kernel || !state->root.kernel || !state->root.domain || !state->root.generation)
  return fail("Missing actual XMsg root identity");
 for(const auto& code:kCode)if(!state->CodeCurrent(code.address))return fail("Original XMsg code differs from pinned17559 protocol");
 state->records.reserve(kCaptureLimit);state->providers.reserve(kProviderDepthLimit);
 auto owner=std::shared_ptr<NativeXmsgProvenance>(new NativeXmsgProvenance(state));
 {
  std::lock_guard lock(registry_mutex);auto& prior=registry[state->kernel];
  if(!prior.expired())return fail("XMsg provenance owner already installed");
  prior=owner;
 }
 if(error)error->clear();return owner;
}
bool NativeXmsgProvenance::HooksFunction(uint32_t at) noexcept {
 switch(at){
  case kRequest:case kRequestEx:case kStartWrapper:case kStart:case kCopy:
  case kInProcess:case 0x816A2B90:case 0x816A2C08:case kInvoke:case kCallback:case kSchedule:case kInline:
  case kAllocate:case kFree:case kIdentityServiceProducer:case kIdentityServiceAccept:
  case kIdentityServiceComplete:case kIdentityServiceCancel:return true;
  default:return false;
 }
}
std::span<const NativeXmsgProvenance::InstructionSite>
NativeXmsgProvenance::InstructionSites() noexcept {return kSites;}

std::optional<NativeXmsgProvenance::Capture> NativeXmsgProvenance::Begin(
    void* raw,AuthenticatedProducer capsule) noexcept try {
 if(!raw)return {};
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 const auto owner=Lookup(c.kernel_state);if(!owner)return {};auto& s=*owner->state_;
 struct Exit{State& state;bool entered=false;~Exit(){if(entered){std::lock_guard lock(state.mutex);--state.hooks;}}} exit{s};
 {
  std::lock_guard lock(s.mutex);
  if(s.retired || s.stopping || !s.kernel || s.hooks==SIZE_MAX)return {};
  ++s.hooks;exit.entered=true;
 }
 const auto& p=capsule.fields_;
 const bool identity_service=bool(capsule.identity_service_);
 if(identity_service && (p.producer.function!=kIdentityServiceProducer || p.wrapper!=kStartWrapper ||
    p.form!=Form::kDirectStart || p.app!=0xFC || p.message!=0x58001 ||
    p.argument_bytes!=0x568 || p.overlap || p.copied_bytes_budget))return {};
 if(p.root!=s.root || (!identity_service && (!p.public_invocation.invocation ||
    !p.public_invocation.function || !p.public_invocation.module.hmodule ||
    !p.public_invocation.module.generation || !p.public_invocation.caller_module.hmodule ||
    !p.public_invocation.caller_module.generation)) || !p.producer_instruction ||
    p.producer_instruction<4 || (p.producer_instruction&3) || !p.producer.function ||
    (p.argument_bytes && !p.argument) ||
    (!identity_service && p.argument_bytes>p.copied_bytes_budget) || p.copied_bytes_budget>kCopyBudgetLimit ||
    (p.form==Form::kRequestWrapper && p.wrapper!=kRequest && p.wrapper!=kRequestEx) ||
    (p.form==Form::kDirectStart && p.wrapper!=kStartWrapper) ||
    (p.form!=Form::kRequestWrapper && p.form!=Form::kDirectStart))return {};
 const auto frame=Actual(*s.kernel,c,p.producer.function);
 if(!frame || *frame!=p.producer || (!identity_service && (!SameThread(p.public_invocation.frame,p.producer) ||
    p.public_invocation.frame.sp<p.producer.sp || p.public_invocation.frame.lr<4)) ||
    !s.CodeCurrent(p.wrapper))return {};
 const auto public_module=s.ModuleAt(p.public_invocation.function);
 const auto caller_module=s.ModuleAt(p.public_invocation.frame.lr-4,false);
 const auto producer_module=s.ModuleAt(p.producer_instruction);
 if((!identity_service && (public_module!=std::optional(p.public_invocation.module) ||
    caller_module!=std::optional(p.public_invocation.caller_module))) || !producer_module)return {};
 const auto branch=Word(*s.kernel->memory(),p.producer_instruction);
 if(!branch || (*branch&0xFC000003u)!=0x48000001u)return {};
 const int32_t displacement=int32_t((*branch&0x03FFFFFCu)<<6)>>6;
 if(p.producer_instruction+uint32_t(displacement)!=p.wrapper)return {};
 auto record=std::make_shared<Capture::Record>();record->producer=p;
 record->identity_service=capsule.identity_service_;record->identity_service_metadata_only=identity_service;
 record->producer_thread=XThread::GetCurrentThread();record->owner=owner->state_;
 if(!s.PublicArguments(*record,c,p.form==Form::kDirectStart))return {};
 {
  std::lock_guard lock(s.mutex);s.Prune();
  if(s.stopping || s.retired || s.records.size()>=kCaptureLimit || !s.next_capture ||
     s.next_capture==UINT64_MAX)return {};
  for(const auto& weak:s.records)if(const auto old=weak.lock();old && s.Live(*old) &&
      old->producer_thread==record->producer_thread && old->producer.producer==p.producer &&
      old->producer.producer_instruction==p.producer_instruction)return {};
  record->capture=s.next_capture++;s.records.push_back(record);
 }
 return Capture(std::move(record));
}catch(...){return {};}

bool NativeXmsgProvenance::FactsAvailable(const Capture& capture) noexcept {
 return capture.record_ && capture.record_->facts_available.load(std::memory_order_acquire);
}
std::optional<NativeXmsgProvenance::ObservationState>
NativeXmsgProvenance::Observe(const Capture& capture) noexcept {
 const auto record=capture.record_;if(!record)return {};
 const auto state=record->owner.lock();if(!state)return {};
 std::lock_guard lock(state->mutex);
 return ObservationState{record->released,record->revoked,record->unavailable,state->retired};
}
void NativeXmsgProvenance::Release(const Capture& capture) noexcept {
 const auto record=capture.record_;
 if(record)if(const auto state=record->owner.lock()){
  std::lock_guard lock(state->mutex);record->released=true;record->facts_available.store(false,std::memory_order_release);++record->version;
 }
 // The handle remains historical metadata. Leaving its shared_ptr unchanged
 // also permits a real concurrent CurrentDispatch/Release without a data race
 // on the handle itself. No source/File/module/native owner is held here.
}
void NativeXmsgProvenance::BeginStop() noexcept {
 std::lock_guard lock(state_->mutex);state_->stopping=true;
}
bool NativeXmsgProvenance::Drained() const noexcept {
 std::lock_guard lock(state_->mutex);
 if(!state_->stopping || state_->hooks)return false;
 for(const auto& weak:state_->records)if(const auto record=weak.lock();record && !record->released)return false;
 return true;
}
bool NativeXmsgProvenance::RetainedForActualKernelStop() const noexcept {
 std::lock_guard lock(state_->mutex);
 return state_->stopping && !state_->retired && !state_->hooks;
}
bool NativeXmsgProvenance::RetireAfterActualKernelStop() noexcept {
 if(XThread::IsInThread() || global_critical_region::is_held_by_current_thread())return false;
 std::lock_guard retirement(state_->retirement_mutex);
 // Only the service's factual joined-CPU hook calls this. Keep a defensive
 // check against accidental early withdrawal, without inferring native drain.
 KernelState* observed=nullptr;
 {std::lock_guard lock(state_->mutex);observed=state_->kernel;}
 if(observed)for(const auto& thread:observed->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread))
  if(thread->is_running())return false;
 Config retired;KernelState* kernel=nullptr;
 {
  std::lock_guard lock(state_->mutex);
  if(!state_->stopping || state_->hooks)return false;
  if(state_->retired)return true;
  state_->retired=true;kernel=std::exchange(state_->kernel,nullptr);
  for(const auto& weak:state_->records)if(const auto r=weak.lock()){r->revoked=true;r->facts_available.store(false,std::memory_order_release);r->released=true;++r->version;}
  state_->providers.clear();state_->records.clear();retired=std::move(state_->config);
 }
 {std::lock_guard lock(registry_mutex);const auto found=registry.find(kernel);
  if(found!=registry.end() && found->second.lock().get()==this)registry.erase(found);}
 // Existing shared service owners drop only at the caller's factual stopped
 // boundary, outside provenance/registry/GCRC; no PPC destructor is invoked.
 return true;
}

void NativeXmsgProvenance::ObserveEntry(void* raw,uint64_t address) noexcept {
 if(!raw || address>UINT32_MAX || !HooksFunction(uint32_t(address)))return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 if(uint32_t(address)==kIdentityServiceComplete){
  (void)NativeProfileIdentityOwner::ObserveIdentityServiceFastBoundary(raw,uint32_t(address),false);return;
 }
 if(uint32_t(address)==kFree && NativeProfileIdentityOwner::ObserveIdentityServiceFastBoundary(
      raw,uint32_t(address),false))return;
 const auto owner=Lookup(c.kernel_state);if(!owner)return;auto& s=*owner->state_;
 struct Exit{State& state;bool entered=false;~Exit(){if(entered){std::lock_guard lock(state.mutex);--state.hooks;}}} exit{s};
 std::shared_ptr<Capture::Record> touched;
 const auto at=uint32_t(address);
 try {
  {
   std::lock_guard lock(s.mutex);s.Prune();
   const bool identity_boundary=at==kIdentityServiceProducer || at==kIdentityServiceAccept ||
       at==kIdentityServiceComplete || at==kIdentityServiceCancel || at==kStartWrapper || at==kFree;
   if(s.retired || !s.kernel || (s.records.empty() && !identity_boundary) || s.hooks==SIZE_MAX)return;
   ++s.hooks;exit.entered=true;
  }
  const auto frame=Actual(*s.kernel,c,at);
  if(!frame){if(at==kInvoke){std::lock_guard lock(s.mutex);s.provider_overflow=true;}return;}
  const auto* thread=XThread::GetCurrentThread();
  // Task-only authenticates these complete bodies. This component observes
  // their real scalar ancestry; it does not install a second Task protocol.
  const bool task_function=at==kSchedule || at==kInline || at==kAllocate;
  if(!task_function && !s.CodeCurrent(at)){
   if(at==kInvoke){std::lock_guard lock(s.mutex);s.provider_overflow=true;}return;
  }
  if(at==kIdentityServiceProducer || at==kIdentityServiceAccept ||
     at==kIdentityServiceComplete || at==kIdentityServiceCancel){
   NativeProfileIdentityOwner::ObserveIdentityServiceBoundary(raw,at,false);return;
  }
  if(at==kStartWrapper){
   const auto origin=NativeProfileIdentityOwner::CaptureIdentityServiceStart(raw);
   if(origin){
    ProducerFields p;p.root=s.root;p.producer={frame->thread,frame->pcr,frame->sp,frame->lr,kIdentityServiceProducer};
    p.producer_instruction=0x8183099C;p.wrapper=kStartWrapper;p.form=Form::kDirectStart;
    p.app=0xFC;p.message=0x58001;p.argument=uint32_t(c.r[6]);p.argument_bytes=0x568;
    if(auto capture=Begin(raw,AuthenticatedProducer(p,origin))){
     auto link=std::shared_ptr<NativeXmsgIdentityServiceLink>(new NativeXmsgIdentityServiceLink);
     link->capture_=std::move(*capture);NativeProfileIdentityOwner::BindIdentityServiceLink(origin,std::move(link));
    }
   }
  }
  if(at==kFree){
   NativeProfileIdentityOwner::ObserveIdentityServiceBoundary(raw,at,false);
   const auto pointer=Dword(c.r[3]);if(!pointer)return;
   std::lock_guard lock(s.mutex);
   s.Revoke(*pointer);
   return; // Revocation, never a native free/completion/refcount receipt.
  }
  if(at==kRequest || at==kRequestEx || at==kStartWrapper){
   std::lock_guard lock(s.mutex);
   auto record=s.Unique([&](const auto& r){return r.producer_thread==thread &&
       !r.wrapper_entered && r.producer.wrapper==at &&
       EnteredFrom(*frame,r.producer.producer,r.producer.producer_instruction,0);});
   bool inner=false;
   if(!record && at==kRequest){
    record=s.Unique([&](const auto& r){return r.producer_thread==thread &&
      r.producer.wrapper==kRequestEx && r.wrapper_entered && !r.wrapper_returned &&
      !r.inner_entered && EnteredFrom(*frame,r.wrapper,0x816A327C,0x90);});inner=bool(record);
   }
   if(!record)return;touched=record;
   if(!s.PublicArguments(*record,c,record->producer.form==Form::kDirectStart)){
    record->unavailable=true;record->facts_available.store(false,std::memory_order_release);return;
   }
   if(inner){record->inner_wrapper=*frame;record->inner_entered=true;}
   else {record->wrapper=*frame;record->wrapper_entered=true;}
   ++record->version;return;
  }
  if(at==kStart){
   std::lock_guard lock(s.mutex);
   const auto r=s.Unique([&](const auto& r){
    if(r.producer_thread!=thread || !r.wrapper_entered || r.wrapper_returned || r.start_entered)return false;
    if(r.producer.form==Form::kDirectStart)return SameThread(*frame,r.wrapper) &&
      frame->sp==r.wrapper.sp && frame->lr==r.wrapper.lr;
    if(r.producer.wrapper==kRequestEx && !r.inner_entered)
     return EnteredFrom(*frame,r.wrapper,0x816A3230,0x90);
    const auto& parent=r.inner_entered?r.inner_wrapper:r.wrapper;
    return !r.inner_returned && EnteredFrom(*frame,parent,0x816A318C,0x80);
   });
   if(!r)return;touched=r;
   const auto options=Dword(c.r[8]);
   const uint32_t expected_options=frame->lr==0x816A3234?frame->sp+0x50:0;
   if(!s.PublicArguments(*r,c,true) || !options || *options!=expected_options){r->unavailable=true;r->facts_available.store(false,std::memory_order_release);return;}
   r->start=*frame;r->start_entered=true;r->parameters=*options;++r->version;return;
  }
  if(at==kCopy){
   std::lock_guard lock(s.mutex);
   const auto r=s.Unique([&](const auto& r){return r.producer_thread==thread &&
      r.start_entered && !r.start_returned && !r.copy_entered &&
      EnteredFrom(*frame,r.start,0x816A2D2C,0xD0);});
   if(!r)return;touched=r;
   if(Dword(c.r[3])!=std::optional(r->producer.argument) ||
      Dword(c.r[4])!=std::optional(r->producer.argument_bytes) ||
      frame->sp>UINT32_MAX-0x50 || Dword(c.r[5])!=std::optional(frame->sp+0x50)){
    r->unavailable=true;r->facts_available.store(false,std::memory_order_release);return;
   }
   r->copier=*frame;r->copy_output=frame->sp+0x50;r->copy_entered=true;++r->version;return;
  }
  if(at==kSchedule || at==kInline){
   std::lock_guard lock(s.mutex);
   const auto r=s.Unique([&](const auto& r){return r.producer_thread==thread &&
      r.start_entered && !r.start_returned && r.copy_returned && r.envelope &&
      !r.schedule_entered && EnteredFrom(*frame,r.start,at==kSchedule?0x816A2EC4:0x816A2EB4,0xD0);});
   if(!r)return;touched=r;
   if(Dword(c.r[3])!=std::optional(kCallback) || Dword(c.r[4])!=std::optional(r->envelope) ||
      Dword(c.r[5])!=std::optional(r->parameters) ||
      (at==kSchedule && Dword(c.r[6])!=std::optional(r->envelope+0x18))){r->unavailable=true;r->facts_available.store(false,std::memory_order_release);return;}
   r->schedule=*frame;r->schedule_entered=true;
   r->delivery=at==kSchedule?Delivery::kQueuedTask:Delivery::kInlineTask;++r->version;return;
  }
  if(at==kCallback){
   const auto packet=Dword(c.r[3]);if(!packet)return;
   {
    std::lock_guard lock(s.mutex);
    touched=s.Unique([&](const auto& r){return r.envelope==*packet && r.task && r.birth_generation &&
      r.schedule_entered && !r.callback_entered;});
   }
   if(!touched)return;
   const auto actual=NativeProfileIdentityOwner::CurrentTaskExecution(&c,touched->task);
   std::array<uint8_t,kEnvelopeBytes> header{};
   const bool valid=actual && s.Execution(*touched,*actual) &&
     actual->thread==frame->thread && actual->pcr==frame->pcr && actual->body_sp==frame->sp &&
     actual->callback_lr==frame->lr &&
     internal::ReadNativeWaitSnapshot(*s.kernel->memory(),*packet,header) &&
     Value(header,0)==touched->producer.app && Value(header,4)==touched->producer.message &&
     Value(header,12)==touched->copied_argument;
   std::lock_guard lock(s.mutex);
   if(!s.Live(*touched))return;
   if(!valid || touched->callback_entered){touched->unavailable=true;touched->facts_available.store(false,std::memory_order_release);return;}
   touched->execution=*actual;touched->callback=*frame;touched->callback_entered=true;
   touched->native_overlap=Value(header,8);++touched->version;return;
  }
  if(at==kInProcess){
   std::lock_guard lock(s.mutex);
   const auto r=s.Unique([&](const auto& r){
    if(r.producer_thread!=thread || r.producer.form!=Form::kRequestWrapper ||
       !r.wrapper_entered || r.wrapper_returned || r.inprocess_entered)return false;
    const auto& parent=r.inner_entered?r.inner_wrapper:r.wrapper;
    return parent.function==kRequest && !r.inner_returned && EnteredFrom(*frame,parent,0x816A316C,0x80);
   });
   if(!r)return;touched=r;
   if(thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM || r->producer.overlap ||
      Dword(c.r[3])!=std::optional(r->producer.app) || Dword(c.r[4])!=std::optional(r->producer.message) ||
      Dword(c.r[5])!=std::optional(r->producer.argument) || Dword(c.r[6])!=std::optional(0u)){
    r->unavailable=true;r->facts_available.store(false,std::memory_order_release);return;
   }
   r->inprocess=*frame;r->inprocess_entered=true;r->delivery=Delivery::kDirectInProcess;++r->version;return;
  }
  if(at==0x816A2B90 || at==0x816A2C08){
   std::lock_guard lock(s.mutex);
   const bool direct=at==0x816A2B90;
   const auto r=s.Unique([&](const auto& r){return direct?
     r.producer_thread==thread && r.inprocess_entered && !r.inprocess_returned && !r.shared_entered &&
       EnteredFrom(*frame,r.inprocess,0x816A2FD8,0x80):
     r.callback_entered && !r.callback_returned && !r.controls_entered &&
       EnteredFrom(*frame,r.callback,0x816A2CBC,0x80);});
   if(!r)return;touched=r;
   const auto object=Dword(c.r[3]);
   if(!object || !*object || (*object&3) ||
      (direct && (Dword(c.r[4])!=std::optional(r->producer.message) ||
        Dword(c.r[5])!=std::optional(r->producer.argument) || Dword(c.r[6])!=std::optional(0u))) ||
      (!direct && Dword(c.r[4])!=std::optional(r->envelope))){r->unavailable=true;r->facts_available.store(false,std::memory_order_release);return;}
   r->provider_object=*object;
   if(direct){r->shared=*frame;r->shared_entered=true;}
   else {r->controls=*frame;r->controls_entered=true;}
   ++r->version;return;
  }
  if(at==kInvoke){
   const auto object=Dword(c.r[3]),message=Dword(c.r[4]),argument=Dword(c.r[5]),bytes=Dword(c.r[6]);
   std::lock_guard lock(s.mutex);
   if(s.providers.size()>=kProviderDepthLimit || !s.next_provider || s.next_provider==UINT64_MAX){
    s.provider_overflow=true;return; // No older provider fact can become current.
   }
   State::Provider boundary{*frame,thread};boundary.serial=s.next_provider++;
   if(object && message && argument && bytes){
    boundary.object=*object;boundary.message=*message;boundary.argument=*argument;boundary.bytes=*bytes;
    touched=s.Unique([&](const auto& r){
     if(r.provider_object!=*object || std::any_of(s.providers.begin(),s.providers.end(),
         [&](const auto& older){return !older.returned && older.record.lock().get()==&r;}))return false;
     if(r.delivery==Delivery::kDirectInProcess)return r.producer_thread==thread &&
       r.shared_entered && !r.shared_returned && EnteredFrom(*frame,r.shared,0x816A2BE4,0x90) &&
       *message==r.producer.message && *argument==r.producer.argument && !*bytes;
     if(!r.controls_entered || r.controls_returned || !r.execution ||
        !SameThread(*frame,r.callback) || r.next_control>=3)return false;
     constexpr uint32_t calls[]={0x816A2C34,0x816A2C4C,0x816A2C64};
     constexpr uint32_t messages[]={0x8000000B,0x80000007,0x8000000C};
     return EnteredFrom(*frame,r.controls,calls[r.next_control],0x70) &&
       *message==messages[r.next_control] && *argument==(r.next_control==1?r.envelope:0) && !*bytes;
    });
    if(touched){boundary.record=touched;if(touched->delivery!=Delivery::kDirectInProcess)++touched->next_control;}
   }else boundary.unavailable=true;
   // Even an unadmitted nested provider is a real shadow boundary. It never
   // inherits or searches backwards for an older matching app/message.
   s.providers.push_back(std::move(boundary));return;
  }
 }catch(...){
  {std::lock_guard lock(s.mutex);
   if(touched){touched->unavailable=true;touched->facts_available.store(false,std::memory_order_release);++touched->version;}
   if(at==kInvoke)s.provider_overflow=true; // Lost shadow cannot lend an older fact.
  }
  // Observation failure does not deny/replace original native execution.
 }
}

void NativeXmsgProvenance::ObserveInstruction(void* raw,uint32_t address) noexcept {
 if(!raw || address!=kSites[0].address)return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 const auto owner=Lookup(c.kernel_state);if(!owner)return;auto& s=*owner->state_;
 struct Exit{State& state;bool entered=false;~Exit(){if(entered){std::lock_guard lock(state.mutex);--state.hooks;}}} exit{s};
 std::shared_ptr<Capture::Record> record;uint64_t serial=0,version=0;
 try {
  {
   std::lock_guard lock(s.mutex);
   if(s.retired || !s.kernel || s.records.empty() || s.hooks==SIZE_MAX)return;
   ++s.hooks;exit.entered=true;
  }
  const auto body=Actual(*s.kernel,c,kInvoke);if(!body || !s.CodeCurrent(kInvoke))return;
  const auto* thread=XThread::GetCurrentThread();
  Frame parent;uint32_t object=0,message=0,argument=0,bytes=0;
  {
   std::lock_guard lock(s.mutex);const auto boundary=s.Innermost(*body,thread);
   if(!boundary || boundary->unavailable || boundary->fact || s.provider_overflow)return;
   record=boundary->record.lock();if(!record || !s.Live(*record))return;
   serial=boundary->serial;parent=boundary->frame;object=boundary->object;
   message=boundary->message;argument=boundary->argument;bytes=boundary->bytes;version=record->version;
  }
  const auto target=Dword(c.r[11]);const auto module=target?s.ModuleAt(*target):std::nullopt;
  const bool scalar=parent.sp>=0x80 && body->sp==parent.sp-0x80 &&
    SameThread(*body,parent) && target && *target && module &&
    Dword(c.r[3])==std::optional(message) && Dword(c.r[4])==std::optional(argument) &&
    Dword(c.r[5])==std::optional(bytes);
  const auto actual=record->delivery==Delivery::kDirectInProcess?
    std::optional<NativeProfileIdentityOwner::TaskExecutionObservation>{}:
    NativeProfileIdentityOwner::CurrentTaskExecution(&c,record->task);
  const bool execution=record->delivery==Delivery::kDirectInProcess ||
    (actual && record->execution && s.Execution(*record,*actual) && SameExecution(*actual,*record->execution));
  if(!scalar || !execution){std::lock_guard lock(s.mutex);record->unavailable=true;record->facts_available.store(false,std::memory_order_release);++record->version;return;}
  auto fact=std::make_shared<DispatchFact>();fact->root=s.root;fact->public_invocation=record->producer.public_invocation;
  fact->capture=record->capture;fact->copy=record->copy_serial;fact->provider_invocation=serial;
  fact->delivery=record->delivery;fact->producer=record->producer.producer;fact->wrapper=record->wrapper;
  fact->start=record->start;fact->copier=record->copier;fact->callback=record->callback;fact->provider=parent;
  fact->provider_module=*module;fact->app=record->producer.app;fact->message=record->producer.message;
  fact->original_argument=record->producer.argument;fact->original_argument_bytes=record->producer.argument_bytes;
  fact->original_overlap=record->producer.overlap;fact->envelope=record->envelope;
  fact->copied_argument=record->copied_argument;fact->copied_argument_bytes=record->producer.argument_bytes;
  fact->native_overlap=record->native_overlap;fact->copied_bytes=record->copied_bytes;
  fact->provider_object=object;fact->provider_message=message;fact->provider_argument=argument;
  fact->provider_argument2=bytes;fact->provider_callback=*target;fact->task=actual;
  {
   std::lock_guard lock(s.mutex);const auto boundary=s.Innermost(*body,thread);
   if(!boundary || boundary->serial!=serial || boundary->record.lock()!=record ||
      boundary->unavailable || boundary->fact || !s.Live(*record) || record->version!=version || s.provider_overflow)return;
   boundary->fact=std::move(fact);
  }
 }catch(...){
  if(record){std::lock_guard lock(s.mutex);record->unavailable=true;record->facts_available.store(false,std::memory_order_release);++record->version;}
 }
}

std::shared_ptr<const NativeXmsgProvenance::DispatchFact>
NativeXmsgProvenance::CurrentDispatch(void* raw,const Capture& capture) noexcept try {
 if(!raw || !capture.record_)return {};
 const auto record=capture.record_;const auto state=record->owner.lock();if(!state)return {};
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto& s=*state;
 struct Exit{State& state;bool entered=false;~Exit(){if(entered){std::lock_guard lock(state.mutex);--state.hooks;}}} exit{s};
 {
  std::lock_guard lock(s.mutex);
  if(s.retired || !s.kernel || c.kernel_state!=s.kernel || s.hooks==SIZE_MAX)return {};
  ++s.hooks;exit.entered=true;
 }
 const auto frame=Actual(*s.kernel,c,kInvoke);if(!frame || !s.CodeCurrent(kInvoke))return {};
 const auto* thread=XThread::GetCurrentThread();std::shared_ptr<const DispatchFact> fact;uint64_t version=0;
 {
  std::lock_guard lock(s.mutex);const auto current=s.CurrentProvider(*record,*frame,thread);
  if(!current)return {};
  fact=current->fact;version=record->version;
 }
 if(s.ModuleAt(fact->provider_callback)!=std::optional(fact->provider_module))return {};
 if(fact->delivery!=Delivery::kDirectInProcess){
  const auto actual=NativeProfileIdentityOwner::CurrentTaskExecution(&c,fact->task?fact->task->task:0);
  if(!actual || !fact->task || !SameExecution(*actual,*fact->task))return {};
 }
 {
  std::lock_guard lock(s.mutex);const auto current=s.CurrentProvider(*record,*frame,thread);
  if(record->version!=version || !current || current->fact!=fact)return {};
 }
 return fact;
}catch(...){return {};}

void NativeXmsgProvenance::ObserveReturn(void* raw,uint64_t address) noexcept {
 if(!raw || address>UINT32_MAX || !HooksFunction(uint32_t(address)))return;
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 if(uint32_t(address)==kIdentityServiceComplete){
  (void)NativeProfileIdentityOwner::ObserveIdentityServiceFastBoundary(raw,uint32_t(address),true);return;
 }
 if(uint32_t(address)==kFree && NativeProfileIdentityOwner::ObserveIdentityServiceFastBoundary(
      raw,uint32_t(address),true))return;
 const auto owner=Lookup(c.kernel_state);if(!owner)return;auto& s=*owner->state_;
 struct Exit{State& state;bool entered=false;~Exit(){if(entered){std::lock_guard lock(state.mutex);--state.hooks;}}} exit{s};
 std::shared_ptr<Capture::Record> touched;const auto at=uint32_t(address);
 try {
  {
   std::lock_guard lock(s.mutex);
   const bool identity_boundary=at==kIdentityServiceProducer || at==kIdentityServiceAccept ||
       at==kIdentityServiceComplete || at==kIdentityServiceCancel || at==kStartWrapper || at==kFree;
   if(s.retired || !s.kernel || (s.records.empty() && !identity_boundary) || s.hooks==SIZE_MAX)return;
   ++s.hooks;exit.entered=true;
  }
  const auto frame=Actual(*s.kernel,c,at);
  if(!frame){if(at==kInvoke){std::lock_guard lock(s.mutex);s.provider_overflow=true;}return;}
  if(at==kAllocate){
   // The Task-only owner has just completed its real normal-return observer
   // and released State/GCRC. This same adapter runs before original PPC.
   const auto actual=NativeProfileIdentityOwner::TakeTaskAllocationReturnAtNativeCut(&c);
   if(!actual || actual->root.kernel!=s.root.kernel || actual->root.domain!=s.root.domain ||
      actual->root.generation!=s.root.generation || actual->allocator.thread!=frame->thread ||
      actual->allocator.pcr!=frame->pcr || actual->allocator.sp!=frame->sp ||
      actual->allocator.lr!=frame->lr || actual->allocator.function!=at)return;
   const Frame producer{actual->producer.thread,actual->producer.pcr,actual->producer.sp,
                        actual->producer.lr,actual->producer.function};
   std::lock_guard lock(s.mutex);
   const auto r=s.Unique([&](const auto& r){return r.schedule_entered && !r.schedule_returned &&
      !r.task && r.schedule==producer;});
   if(!r)return;touched=r;
   if(int32_t(actual->native_status)<0)return; // No failed allocation birth.
   if(!actual->validated_birth || !actual->task || (actual->task&3) ||
      !actual->birth_generation || !actual->birth_origin){r->unavailable=true;r->facts_available.store(false,std::memory_order_release);return;}
   r->task=actual->task;r->birth_generation=actual->birth_generation;r->birth_origin=actual->birth_origin;
   ++r->version;return;
  }
  if(at!=kSchedule && at!=kInline && !s.CodeCurrent(at)){
   if(at==kInvoke){std::lock_guard lock(s.mutex);s.provider_overflow=true;}return;
  }
  if(at==kIdentityServiceProducer || at==kIdentityServiceAccept ||
     at==kIdentityServiceComplete || at==kIdentityServiceCancel || at==kFree){
   NativeProfileIdentityOwner::ObserveIdentityServiceBoundary(raw,at,true);return;
  }
  const auto* thread=XThread::GetCurrentThread();
  if(at==kInvoke){
   std::lock_guard lock(s.mutex);auto* current=s.Innermost(*frame,thread);
   if(current && current->frame==*frame){current->returned=true;return;}
   if(current){
    current->unavailable=true;
    if(const auto r=current->record.lock()){
     r->unavailable=true;r->facts_available.store(false,std::memory_order_release);
    }
   }
   for(auto& earlier:s.providers)if(!earlier.returned && earlier.actual_thread==thread && earlier.frame==*frame){
    earlier.returned=true;earlier.unavailable=true;
    if(const auto r=earlier.record.lock()){
     r->unavailable=true;r->facts_available.store(false,std::memory_order_release);
    }
   }
   return; // Never settle a different provider's completion.
  }
  uint64_t version=0;
  {
   std::lock_guard lock(s.mutex);
   touched=s.Unique([&](const auto& r){
    if(at==kCopy)return r.copy_entered && !r.copy_returned && r.copier==*frame;
    if(at==kStart)return r.start_entered && !r.start_returned && r.start==*frame;
    if(at==kSchedule || at==kInline)return r.schedule_entered && !r.schedule_returned && r.schedule==*frame;
    if(at==kCallback)return r.callback_entered && !r.callback_returned && r.callback==*frame;
    if(at==kInProcess)return r.inprocess_entered && !r.inprocess_returned && r.inprocess==*frame;
    if(at==0x816A2B90)return r.shared_entered && !r.shared_returned && r.shared==*frame;
    if(at==0x816A2C08)return r.controls_entered && !r.controls_returned && r.controls==*frame;
    if(at==kRequest && r.inner_entered && !r.inner_returned && r.inner_wrapper==*frame)return true;
    return (at==kRequest || at==kRequestEx) && r.wrapper_entered && !r.wrapper_returned && r.wrapper==*frame;
   });
   if(touched)version=touched->version;
  }
  if(!touched)return;
  if(at==kCopy){
   const bool success=int32_t(uint32_t(c.r[3]))>=0;
   const auto envelope=success?Word(*s.kernel->memory(),touched->copy_output):std::optional<uint32_t>{};
   std::shared_ptr<const std::vector<uint8_t>> payload;
   uint32_t copied_argument=0;bool valid=!success;
   if(success && envelope && *envelope && !(*envelope&3) &&
      uint64_t(*envelope)+kEnvelopeBytes+touched->producer.argument_bytes<=uint64_t(UINT32_MAX)+1){
    const auto argument=Word(*s.kernel->memory(),*envelope+12);
    copied_argument=touched->producer.argument?*envelope+kEnvelopeBytes:0;
    if(argument==std::optional(copied_argument)){
     if(touched->identity_service_metadata_only)valid=true;
     else {payload=s.ReadCopiedPayload(*s.kernel->memory(),*envelope+kEnvelopeBytes,
         touched->producer.argument_bytes);valid=bool(payload);}
    }
   }
   std::lock_guard lock(s.mutex);
   s.BindCopiedPayload(*touched,version,success,valid,envelope.value_or(0),
       copied_argument,std::move(payload));return;
  }
  std::lock_guard lock(s.mutex);if(!s.Live(*touched) || touched->version!=version)return;
  if(at==kStart)touched->start_returned=true;
  else if(at==kSchedule || at==kInline)touched->schedule_returned=true;
  else if(at==kCallback)touched->callback_returned=true;
  else if(at==kInProcess)touched->inprocess_returned=true;
  else if(at==0x816A2B90)touched->shared_returned=true;
  else if(at==0x816A2C08)touched->controls_returned=true;
  else if(at==kRequest && touched->inner_entered && touched->inner_wrapper==*frame)touched->inner_returned=true;
  else touched->wrapper_returned=true;
  ++touched->version;
 }catch(...){
  std::lock_guard lock(s.mutex);
  if(touched){touched->unavailable=true;touched->facts_available.store(false,std::memory_order_release);++touched->version;}
  if(at==kInvoke)s.provider_overflow=true;
 }
}

std::optional<NativeXmsgProvenance::IdentityServiceEnvelope>
NativeXmsgProvenance::IdentityServiceRequest(const std::shared_ptr<NativeXmsgIdentityServiceLink>& link) noexcept try {
 if(!link || !link->capture_.record_)return {};
 const auto record=link->capture_.record_;const auto owner=record->owner.lock();if(!owner)return {};
 std::lock_guard lock(owner->mutex);
 if(!owner->Live(*record) || !record->identity_service_metadata_only || record->identity_service.expired() ||
    !record->copy_returned || !record->copy_serial || !record->callback_entered ||
    !record->task || !record->birth_generation || !record->native_overlap)return {};
 return IdentityServiceEnvelope{owner->root,record->capture,record->copy_serial,record->envelope,
     record->copied_argument,record->native_overlap,record->task,record->birth_generation,record->birth_origin};
}catch(...){return {};}
std::shared_ptr<const NativeXmsgProvenance::DispatchFact>
NativeXmsgProvenance::IdentityServiceDispatch(void* raw,const std::shared_ptr<NativeXmsgIdentityServiceLink>& link) noexcept {
 return link?CurrentDispatch(raw,link->capture_):nullptr;
}
void NativeXmsgProvenance::ReleaseIdentityService(const std::shared_ptr<NativeXmsgIdentityServiceLink>& link) noexcept {
 if(link)Release(link->capture_);
}
std::atomic<bool>* NativeXmsgProvenance::IdentityServiceAvailability(
    const std::shared_ptr<NativeXmsgIdentityServiceLink>& link) noexcept {
 return link && link->capture_.record_?&link->capture_.record_->facts_available:nullptr;
}

std::optional<NativeXmsgProvenance::TitleTaskProducerFact>
NativeXmsgProvenance::CaptureTitleTaskProducer(void* raw,uint32_t function) noexcept try {
 if(!raw || (function!=kSchedule && function!=kInline))return {};
 auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
 const auto owner=Lookup(c.kernel_state);if(!owner)return {};auto& s=*owner->state_;
 struct Exit{State& state;bool entered=false;~Exit(){if(entered){std::lock_guard lock(state.mutex);--state.hooks;}}} exit{s};
 {
  std::lock_guard lock(s.mutex);
  if(s.retired || !s.kernel || s.hooks==SIZE_MAX)return {};
  ++s.hooks;exit.entered=true;
 }
 const auto* thread=XThread::IsInThread()?XThread::GetCurrentThread():nullptr;
 const auto frame=Actual(*s.kernel,c,function);
 if(!thread || thread->native_frame_thread_identity().process_type!=X_PROCTYPE_TITLE ||
    !frame || !s.CodeCurrent(kStart) || !s.CodeCurrent(kCopy))return {};
 std::lock_guard lock(s.mutex);
 const auto record=s.Unique([&](const auto& r){return r.producer_thread==thread &&
     r.schedule_entered && !r.schedule_returned && r.schedule==*frame &&
     r.start_entered && !r.start_returned && r.copy_returned && r.copy_serial && r.envelope;});
 if(!record || Dword(c.r[3])!=std::optional(kCallback) ||
    Dword(c.r[4])!=std::optional(record->envelope) ||
    Dword(c.r[5])!=std::optional(record->parameters) ||
    (function==kSchedule && Dword(c.r[6])!=std::optional(record->envelope+0x18)))return {};
 return TitleTaskProducerFact{s.root,*frame,record->capture,record->copy_serial,
     uint32_t(thread->native_frame_thread_identity().process_type),kCallback,
     record->envelope,record->parameters,function==kSchedule?record->envelope+0x18:0};
}catch(...){return {};}
}  // namespace xe::kernel
