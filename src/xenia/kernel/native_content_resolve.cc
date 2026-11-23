/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_content_resolve.h"
#include "xenia/kernel/native_xmsg_provenance.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/console_storage_volume.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_content_storage_driver.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_profile_identity_owner.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/profile_roster.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/file_open_context.h"
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe::kernel {
namespace {
constexpr uint32_t kCommon=0x816EE7B0, kWorker=0x8167F988, kPacketBytes=0x258;
constexpr uint32_t kAbort=0x80004004, kBusy=0x800700AA,
    kDisconnected=0x8007048F, kInvalid=0x80070057;
std::mutex installed_mutex;
std::map<KernelState*,std::shared_ptr<NativeContentResolve>> installed;
struct Code { uint32_t at, bytes; const char* sha; };
constexpr Code codes[] = {
    {0x81687A10, 312, "9b4a8a3bc0b12cbece9e2478ad3af4c4932f4d85a901940bf500fc656aa3cc3c"},
    {0x816BA440, 232, "54499cbb31f67e590a4c36e36708688274d2c696f1a03529bd26782955c73fee"},
    {0x816B9D68, 416, "7e34617cc1671d88c221cf4d0570a4b20b930a2147677770be69b70f35fafc5a"},
    {0x816B9A18, 140, "66c19612c25e4ce242ba3992d7afbe489769156b7d0e3d11b0923852588fe5d9"},
    {0x816E6AB0, 96, "cd4506be3ef237ff9fa71650947f227043d3e76d06559fea0ed12f6693a78149"},
    {0x816BA528, 4, "589dc545c388897495ab8eaf51190892ed0e1235001c1ee2852c2a22be5bd1f6"},
    {0x816B8B08, 152, "1e6e216190b9d6046af3325bf16deb0e22ff9409b457f920a67bfbb1b756c2de"},
    {0x816B9BB8, 428, "a87d1ebadbc74b3fccf020390eaebfcff674f0971fd129604b4769ea4116b59e"},
    {0x816EE7B0, 340, "24b701aee61bb66941f9c619b0c10480d007f1582666bc59709bf9d34db25a23"},
    {0x816EDED8, 200, "35c8a805a402342b3154e5ed26133d4b7b2dcb94084a80e15e4c1653f793a7d1"},
    {0x816EDFA0, 192, "bdff0a6dc1ab10d293efaf13b225abda179104fd0b11c8ad238decc6c7e0c5fe"},
    {0x816A3128, 352, "c318fa1ed904c8da6e102bfc5d24f271db71839876b2ccd4d091572a560bcb25"},
    {0x816A2CF0, 656, "e00dfeaa77f3f52c68a5f89f5bea3f6a259c38f2fb3e15ae02f922803f75339a"},
    {0x816A2A48, 128, "7ebaa0f5422b037352ac5cfb6a5bd2a3954d4e6fdc3901d5e00a9f0d3c4f053d"},
    {0x816A2588, 364, "cb8e83d7817263c223f0ac9719175540bbd78746a39ce1b5a38221550bf953d1"},
    {0x816A26F8, 352, "b1af64f353766148e34db02ed938743371561d2ff3d694bb5a8209e5c205051a"},
    {0x816A2C80, 108, "21da95341366e83781ab2e46696f0c04afa43d71ae7c11f91dc8e108d1b1db94"},
    {0x81687C00, 880, "9862d7d286364194cceba3ee13536d127657d3e94eae2becafa431c9ad8b02f2"},
    {0x8167F988, 384, "b8d2e64b8c7b89c0e7cdf9993cf6028466d3a6c2ff8deeb819797638eeaf6760"},
    {0x8167A500, 176, "275e8fd6ca8ee082e0c760580648a562b0ed53e6410d6af2130e2b20b9364976"},
    {0x8167EFC8, 1524, "f77db4d9de1478747bed257e24a7ddd3b8fa84ca197a38660ae9730e5c0fe49b"},
    {0x8167F5C0, 132, "e67c1621e2efbee68b0faabdbded6301b81fef2bd38808fde851643e6d6f73a9"},
    {0x8167F648, 148, "322838977df5e5453f787dbba0f43848967970c7f168d0d0ffdb119498030dbb"},
    {0x8167B940, 148, "d465866b9bef046593057c189f8bb6e30974b847b82ffbbc41987d0dd8ef053a"},
    {0x816E0400, 76, "d7e9ee2de20a4d927d5e4642f2801d2e04beb2b0037e18c72d7716e93bc39135"},
};
std::optional<uint32_t> Pointer(uint64_t value) {
  const auto low=uint32_t(value);
  return value==low || value==uint64_t(int64_t(int32_t(low)))?std::optional(low):std::nullopt;
}
std::optional<uint32_t> Word(Memory& memory,uint32_t at) {
  std::array<uint8_t,4> bytes;
  return internal::ReadNativeWaitSnapshot(memory,at,bytes)?
      std::optional(xe::load_and_swap<uint32_t>(bytes.data())):std::nullopt;
}
struct Frame {
  XThread* thread=nullptr;
  uint32_t id=0,pcr=0,sp=0,lr=0,process=0,function=0;
  bool operator==(const Frame&) const=default;
};
std::optional<Frame> ActualFrame(KernelState& kernel,const cpu::ppc::PPCContext& c,uint32_t at) {
  if(!XThread::IsInThread())return {};
  auto* t=XThread::GetCurrentThread();const auto sp=Pointer(c.r[1]),pcr=Pointer(c.r[13]),lr=Pointer(c.lr);
  if(!t || !XThread::IsInThread(t) || t->kernel_state()!=&kernel ||
      c.kernel_state!=&kernel || c.processor!=kernel.processor() ||
      c.thread_state!=t->thread_state() || t->thread_state()->context()!=&c ||
      !sp || !*sp || (*sp&15) || !pcr || *pcr!=t->pcr_ptr() || !lr)return {};
  const auto process=t->native_frame_thread_identity().process_type;
  if(process!=X_PROCTYPE_TITLE && process!=X_PROCTYPE_SYSTEM)return {};
  return Frame{t,t->thread_id(),*pcr,*sp,*lr,uint32_t(process),at};
}
bool WholePrefix(std::string_view path,std::string_view root) {
  if(!xe::utf8::starts_with_case(path,root))return false;
  return path.size()==root.size() || (!root.empty() && root.back()=='\\') || path[root.size()]=='\\';
}
uint32_t PacketWord(std::span<const uint8_t,kPacketBytes> raw,size_t offset) {
  return xe::load_and_swap<uint32_t>(raw.data()+offset);
}
bool HashMatches(std::span<const uint8_t> raw,std::string_view expected) {
  if(expected.size()!=64)return false;std::array<uint8_t,32> digest;
  mbedtls_sha256(raw.data(),raw.size(),digest.data(),0);
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<digest.size();++n)
    if(expected[2*n]!=hex[digest[n]>>4] || expected[2*n+1]!=hex[digest[n]&15])return false;
  return true;
}
}  // namespace
struct NativeContentResolve::State {
  static_assert(std::is_nothrow_move_assignable_v<std::optional<xam::ContentSourceState>>);
  struct Request {
    uint64_t serial=0;
    std::array<uint32_t,8> args{};
    std::array<uint8_t,kPacketBytes> payload{};
    std::string root;
    object_ref<XThread> caller;
    std::shared_ptr<KernelState::UserModuleExecutionLease> caller_code;
    NativeImportDomain::Operation domain;
    std::optional<xam::ProfileRosterSlot> user;
    Frame original,common,worker;
    std::optional<NativeXmsgProvenance::Capture> provenance;
    uint32_t caller_stack_limit=0,caller_stack_base=0;
    uint32_t native_overlap=0,namespace_id=0,handler_result=kAbort;
    // The common XMsg capsule owns request provenance only. This family owns
    // its source/host-IO duties, not a second Task birth or native completion.
    bool common_entered=false,common_returned=false,
        handler_entered=false,handler_returned=false,
        source_preparing=false,source_finished=false,source_retired=false,source_retiring=false,
        caller_retired=false,caller_retiring=false,uncertain=false,finished=false,
        retiring=false,retired=false;
    std::optional<NativeContentStorageDriver::ContentProcessingReceipt> processed;
    std::optional<xam::ContentSourceState> source;
    std::shared_ptr<vfs::Device> device;
  };
  explicit State(Config value):config(std::move(value)),
      kernel(config.catalog->calls()->kernel()),root_identity(config.domain->root()) {}
  Config config;
  KernelState* kernel=nullptr;
  const NativeImportDomain::Member root_identity;
  std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
  NativeImportDomain::Operation resident_domain;
  std::shared_ptr<NativeContentStorageDriver> storage;
  mutable std::mutex mutex;
  std::mutex reaper_mutex;
  std::condition_variable changed;
  std::thread reaper;
  std::atomic<bool> retired{false};
  bool stopping=false,reaper_stop=false,reaper_failed=false;
  uint64_t serial=0;
  std::vector<std::shared_ptr<Request>> requests;
  bool RootCurrent(uint32_t at) const {
    if(retired.load(std::memory_order_acquire) || !kernel || !root_code || !resident_domain ||
        !config.catalog || !config.domain || !config.domain->active() || !config.allocation ||
        !config.allocation->MatchesDomain(config.domain))return false;
    const auto& calls=config.catalog->calls();const auto module=calls->module();
    if(!module || !module->native_import_ready() || module->hmodule_ptr()!=root_identity.hmodule ||
        module->load_generation()!=root_identity.generation || calls->load_generation()!=root_identity.generation)return false;
    const auto found=std::find_if(std::begin(codes),std::end(codes),[&](const Code& p){
      return at>=p.at && uint64_t(at)<uint64_t(p.at)+p.bytes;
    });
    if(found==std::end(codes))return false;
    auto lock=global_critical_region::AcquireDirect();auto& memory=*kernel->memory();
    if(memory.QueryRangeAccess(found->at,found->bytes)==memory::PageAccess::kNoAccess)return false;
    for(uint32_t p=found->at;p<found->at+found->bytes;p+=4) {
      xex2_section_type type;
      if(kernel->GetUserModuleForAddress(p).get()!=module.get() ||
          !module->xex_module()->GetPageSectionType(p,&type) || type!=XEX_SECTION_CODE ||
          module->xex_module()->TranslateCode(p)!=memory.TranslateVirtual<const uint8_t*>(p))return false;
    }
    return HashMatches({memory.TranslateVirtual<const uint8_t*>(found->at),found->bytes},found->sha);
  }
  std::shared_ptr<Request> Public(XThread* t) const {
    std::shared_ptr<Request> result;
    for(const auto& r:requests)if(r->common_entered && !r->common_returned && r->common.thread==t) {
      if(result)return {};result=r;
    }
    return result;
  }
  std::shared_ptr<Request> Worker(XThread* t,bool& ambiguous) const {
    ambiguous=false;std::shared_ptr<Request> result;
    for(const auto& r:requests)if(r->handler_entered && !r->handler_returned && r->worker.thread==t) {
      if(result){ambiguous=true;return {};}result=r;
    }
    return result;
  }
  bool PacketMatches(const Request& r,std::span<const uint8_t,kPacketBytes> bytes) const {
    // The original datacenter canonicalizer may rewrite the descriptor device
    // for its proven option/type branch, irrespective of the incoming ID. Its
    // actual output is captured, never predicted from a host default policy.
    if(PacketWord(bytes,0)!=r.args[0] || PacketWord(bytes,4)!=r.args[3] ||
        PacketWord(bytes,8)!=r.args[4] || PacketWord(bytes,12)!=r.args[5] ||
        PacketWord(bytes,0x250)!=r.args[7] || PacketWord(bytes,0x254)!=r.args[2])return false;
    const auto end=std::find(bytes.begin()+0x210,bytes.begin()+0x250,0);
    return end!=bytes.begin()+0x250 && (r.args[6] || end==bytes.begin()+0x210);
  }
  std::shared_ptr<Request> Prepare(const Frame& original,const std::array<uint32_t,8>& args) {
    if(global_critical_region::is_held_by_current_thread() ||
        (args[2]!=0x134 && args[2]!=0x200) || (args[2]==0x200 && args[0]!=0xFE))return {};
    auto r=std::make_shared<Request>();r->args=args;r->original=original;r->caller=retain_object(original.thread);
    r->caller_stack_limit=original.thread->stack_limit();
    r->caller_stack_base=original.thread->stack_base();
    const auto caller=original.lr>=4?kernel->GetUserModuleForAddress(original.lr-4):nullptr;
    if(!caller || caller->module_format()!=UserModule::kModuleFormatXex)return {};
    r->caller_code=kernel->AcquireUserModuleExecutionLease(caller);
    r->domain=config.domain->Acquire();if(!r->caller_code || !r->domain)return {};
    // Native retains and accesses its own caller buffers. No translated guest
    // pointer, page pin, host-deferred output copy or Status lowering is kept.
    // The real original validator/copy/store/fault semantics remain untouched.
    // Preserve actual local-user identity across the asynchronous native copy.
    // The native account getter remains the formatter's sole XUID producer.
    if(args[0]<4) {
      std::error_code error;
      const auto roster=kernel->xam_state()->profile_manager()->CaptureRosterExpected(error);
      if(error || !roster.roster)return {};
      r->user=roster.roster->slots()[args[0]];
    }
    return r;
  }
  bool SourceCurrent(const Request& r) const {
    if(!r.provenance || !NativeXmsgProvenance::FactsAvailable(*r.provenance))return false;
    if(!r.processed || !r.device || !r.device->storage_volume() || r.device->namespace_retiring() ||
        r.device->storage_volume()!=r.processed->volume() ||
        !storage->ContentProcessingCurrent(*r.processed))return false;
    if(const auto& optical=r.processed->optical_source()) {
      return !r.source && r.device.get()==optical->identity_ &&
          !optical->device_.owner_before(r.device) && !r.device.owner_before(optical->device_) &&
          optical->volume()==r.device->storage_volume();
    }
    if(!r.source)return false;
    std::error_code error;
    const auto current=kernel->content_manager()->sources().CaptureSourceCached(r.processed->volume(),error);
    return current && !error && current->storage==r.source->storage &&
        current->volume==r.source->volume && current->managed_identity==r.source->managed_identity &&
        current->generation==r.source->generation && current->source.id==r.source->source.id &&
        current->source.device_id==r.source->source.device_id && !r.device->namespace_retiring();
  }
  bool PublicLeft(const Request& r) const {
    return r.common_returned;
  }
  bool SourceCanReap(const Request& r) const {
    return !r.uncertain && !r.source_preparing && !r.source_retired && !r.source_retiring &&
        (r.source_finished || (r.finished && !r.handler_entered));
  }
  bool CallerCanReap(const Request& r) const {
    return PublicLeft(r) && !r.caller_retired && !r.caller_retiring;
  }
  bool CanReap(const Request& r) const {
    return !r.uncertain && r.finished && PublicLeft(r) &&
        (!r.handler_entered || r.handler_returned) &&
        r.source_retired && r.caller_retired && !r.retiring && !r.retired;
  }
  bool NeedsReap(const Request& r) const {
    return SourceCanReap(r) || CallerCanReap(r) || CanReap(r);
  }
  void RefuseBeforeBody(const std::shared_ptr<Request>& r) {
    bool release=false;
    {
      std::lock_guard lock(mutex);
      if(!r->handler_entered) {
        r->source_preparing=false;r->source_finished=r->finished=true;
        release=true;changed.notify_all();
      }
    }
    // Only the synchronous host guard has run. Every provisional capture is
    // already retained by Request for out-of-lock reaping; no original body
    // or host IO callback was issued and no native own-return is invented.
    if(release && r->provenance)NativeXmsgProvenance::Release(*r->provenance);
  }
  void ObserveEndedSubscriptions() {
    std::vector<std::shared_ptr<Request>> candidates;
    {
      std::lock_guard lock(mutex);
      for(const auto& r:requests)
        if(r->provenance && !r->finished && !r->handler_entered && !r->source_preparing)
          candidates.push_back(r);
    }
    for(const auto& r:candidates) {
      const auto observed=NativeXmsgProvenance::Observe(*r->provenance);
      if(!observed || (!observed->released && !observed->authority_revoked &&
          !observed->unavailable && !observed->kernel_stopped))continue;
      bool release=false;
      {
        std::lock_guard lock(mutex);
        if(!r->finished && !r->handler_entered && !r->source_preparing) {
          // There is no admitted host source/IO duty to join. Withholding a
          // future family capability does not settle native free/cancel or
          // Complete, and an already-entered worker never takes this lane.
          r->source_finished=r->finished=true;release=true;
        }
      }
      if(release)NativeXmsgProvenance::Release(*r->provenance);
    }
  }
  void Reap() {
    std::vector<std::shared_ptr<Request>> sources,callers,old;
    sources.reserve(128);callers.reserve(128);old.reserve(128);
    {std::lock_guard lock(mutex);
      for(const auto& r:requests) {
        if(SourceCanReap(*r)){r->source_retiring=true;sources.push_back(r);}
        if(CallerCanReap(*r)){r->caller_retiring=true;callers.push_back(r);}
      }
    }
    // Genuine worker return joins every blocking host open/mkdir/alias call.
    // Every published alias, Device and XFile retains its own actual lifetime;
    // request metadata never erases or retargets that capability. A queued
    // native Complete/APC/Task still runs its original code independently.
    for(const auto& r:sources) {
      r->device.reset();r->source.reset();r->processed.reset();r->user.reset();
      std::lock_guard lock(mutex);r->source_retired=true;r->source_retiring=false;
    }
    // The host-entered public call is over. There is no delayed host caller
    // buffer write or host callback invocation requiring a DLL/code lease.
    // Resident native root/domain ownership remains with this actual service.
    for(const auto& r:callers) {
      r->caller_code.reset();r->caller.reset();r->domain=NativeImportDomain::Operation{};
      std::lock_guard lock(mutex);r->caller_retired=true;r->caller_retiring=false;
    }
    {std::lock_guard lock(mutex);
      for(const auto& r:requests)if(CanReap(*r)){r->retiring=true;old.push_back(r);}
      for(const auto& r:old) {
        r->retired=true;r->retiring=false;
        const auto i=std::find(requests.begin(),requests.end(),r);
        if(i!=requests.end())requests.erase(i);
      }
    }
    // These remaining Request holders own scalar/copied metadata only.
  }
  void Reaper() noexcept try {
    for(;;) {
      {std::unique_lock lock(mutex);changed.wait_for(lock,std::chrono::milliseconds(50),[&]{return reaper_stop ||
          std::any_of(requests.begin(),requests.end(),[&](const auto& r){return NeedsReap(*r);});});
        if(reaper_stop)return;
      }
      // The common component provides value-only observations, not callbacks
      // into family owners. Poll only this bounded retained metadata set.
      ObserveEndedSubscriptions();
      Reap();changed.notify_all();
    }
  } catch(...) {std::lock_guard lock(mutex);reaper_failed=true;changed.notify_all();}
};
NativeContentResolve::NativeContentResolve(std::shared_ptr<State> state):state_(std::move(state)) {}
NativeContentResolve::~NativeContentResolve() { BeginStop(); }
std::shared_ptr<NativeContentResolve> NativeContentResolve::Lookup(KernelState* kernel) {
  std::lock_guard lock(installed_mutex);const auto i=installed.find(kernel);
  return i!=installed.end()?i->second:nullptr;
}
std::shared_ptr<NativeContentResolve> NativeContentResolve::CreateBeforeWorkers(Config config,std::string* error) {
  const auto fail=[&](const char* text)->std::shared_ptr<NativeContentResolve>{if(error)*error=text;return {};};
  if(!config.catalog || !config.domain || !config.allocation || !config.allocation->IsAccepting() ||
      !config.catalog->system_content() || !config.allocation->MatchesDomain(config.domain))return fail("Missing actual content runtime/domain/allocation lifetime");
  auto state=std::make_shared<State>(std::move(config));
  const auto module=state->config.catalog->calls()->module();
  if(!state->kernel || !module || !state->config.domain->active())return fail("Missing current content root");
  state->root_code=state->kernel->AcquireUserModuleExecutionLease(module);
  state->resident_domain=state->config.domain->Acquire();
  state->storage=NativeContentStorageDriver::Lookup(state->kernel);
  if(!state->root_code || !state->resident_domain || !state->storage)return fail("Missing actual content storage/processing owner");
  for(const auto& code:codes)if(!state->RootCurrent(code.at))return fail("Original content producer/consumer bytes are not the pinned17559 family");
  state->requests.reserve(128);
  auto owner=std::shared_ptr<NativeContentResolve>(new NativeContentResolve(state));
  {
    std::lock_guard lock(installed_mutex);
    auto [slot,inserted]=installed.try_emplace(state->kernel);
    if(slot->second)return fail("Another content Resolve owner is installed");
    slot->second=owner;
  }
  try {state->reaper=std::thread([state]{state->Reaper();});}
  catch(...) {
    {std::lock_guard lock(installed_mutex);const auto i=installed.find(state->kernel);
      if(i!=installed.end() && i->second==owner)installed.erase(i);}
    return fail("Content Resolve host retirement tail could not start");
  }
  return owner;
}
void NativeContentResolve::ObserveProducerInstruction(void* raw,uint32_t address) noexcept {
  if(!raw || address!=kProducerInstruction)return;
  auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto owner=Lookup(c.kernel_state);
  if(!owner)return;auto& s=*owner->state_;
  try {
    const auto frame=ActualFrame(*s.kernel,c,kCommon);
    if(!frame || !s.RootCurrent(kCommon))return;
    std::shared_ptr<State::Request> request;
    {std::lock_guard lock(s.mutex);request=s.Public(frame->thread);}
    if(!request || request->common.sp<0x2F0 || frame->sp!=request->common.sp-0x2F0 ||
       Pointer(c.r[3])!=std::optional(0xFEu) || Pointer(c.r[4])!=std::optional(0x20016u) ||
       Pointer(c.r[5])!=std::optional(request->common.sp-0x2A0) || c.r[6]!=kPacketBytes ||
       Pointer(c.r[7])!=std::optional(request->args[7]))return;
    std::array<uint8_t,kPacketBytes> payload;
    if(!internal::ReadNativeWaitSnapshot(*s.kernel->memory(),uint32_t(c.r[5]),payload) ||
       !s.PacketMatches(*request,payload))return;
    const auto module=s.kernel->GetUserModuleForAddress(request->original.function);
    if(!module || !request->caller_code)return;
    NativeXmsgProvenance::ProducerFields fields;
    fields.root={s.config.allocation->owner().kernel_epoch,s.config.domain->id(),s.root_identity.generation};
    fields.public_invocation.invocation=request->serial;
    fields.public_invocation.module={module->hmodule_ptr(),module->load_generation()};
    const auto caller=s.kernel->GetUserModuleForAddress(request->original.lr-4);
    if(!caller || caller->hmodule_ptr()!=request->caller_code->hmodule())return;
    fields.public_invocation.caller_module={caller->hmodule_ptr(),caller->load_generation()};
    fields.public_invocation.function=request->original.function;
    fields.public_invocation.instruction=request->original.lr>=4?request->original.lr-4:0;
    fields.public_invocation.frame={request->original.id,request->original.pcr,request->original.sp,
                                    request->original.lr,request->original.function};
    fields.producer={frame->id,frame->pcr,frame->sp,frame->lr,kCommon};
    fields.producer_instruction=kProducerInstruction;fields.wrapper=0x816A31C8;
    fields.form=NativeXmsgProvenance::ProducerForm::kRequestWrapper;
    fields.app=0xFE;fields.message=0x20016;fields.argument=uint32_t(c.r[5]);
    fields.argument_bytes=kPacketBytes;fields.overlap=request->args[7];fields.copied_bytes_budget=kPacketBytes;
    auto capture=NativeXmsgProvenance::Begin(&c,NativeXmsgProvenance::AuthenticatedProducer(fields));
    bool discard=false;
    {
      std::lock_guard lock(s.mutex);
      if(request->common_returned || request->provenance || request->uncertain)discard=true;
      else if(!capture)request->uncertain=true;
      else {request->payload=payload;request->provenance=std::move(capture);}
    }
    // Shared provenance locks never nest beneath a family metadata lock.
    if(discard && capture)NativeXmsgProvenance::Release(*capture);
  }catch(...){/* No synthetic native call, pointer, completion or source. */}
}
uint64_t NativeContentResolve::GuardEntry(void* raw,uint64_t address) noexcept {
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);
  if(!c || !c->kernel_state || address>UINT32_MAX || !HooksFunction(uint32_t(address)))return 1;
  const auto owner=Lookup(c->kernel_state);if(!owner)return 1;
  auto& s=*owner->state_;const auto at=uint32_t(address);
  const auto deny=[&](uint32_t error)->uint64_t {
    // COMMON exposes Win32 status, while its real FE worker exposes HRESULT.
    // Match the original EE88C..EE8B0 conversion: failed non-Win32 facilities
    // become ERROR_FUNCTION_FAILED, never an arbitrary HRESULT low word.
    const uint32_t result=at!=kCommon?error:
        int32_t(error)>=0?0u:(error&0x1FFF0000u)==0x00070000u?error&0xFFFFu:0x65Bu;
    c->r[3]=at==kCommon?uint64_t(result):uint64_t(int64_t(int32_t(result)));return 0;
  };
  std::shared_ptr<State::Request> active_request;
  try {
    const auto frame=ActualFrame(*s.kernel,*c,at);
    if(!frame || !s.RootCurrent(at)) {
      if(at==kCommon || at==kWorker)return deny(kAbort);
      return 1; // Other original app/Task families remain unchanged.
    }
    auto& memory=*s.kernel->memory();
    if(at==kCommon) {
      std::array<uint32_t,8> args{};
      for(size_t n=0;n<args.size();++n) {const auto v=Pointer(c->r[n+3]);if(!v)return deny(kInvalid);args[n]=*v;}
      auto r=s.Prepare(*frame,args);if(!r)return deny(kInvalid);
      active_request=r;
      std::lock_guard lock(s.mutex);
      if(s.stopping || s.retired.load() || s.requests.size()>=128 || s.serial==UINT64_MAX ||
          s.Public(frame->thread) || r->common_entered)return deny(kBusy);
      r->serial=++s.serial;r->common=*frame;r->common_entered=true;
      s.requests.push_back(r);return 1;
    }
    if(at==kWorker) {
      if(frame->lr!=0x81687DC8 || !Pointer(c->r[3]) || c->r[4]>1)return deny(kInvalid);
      const uint32_t input=uint32_t(c->r[3]);
      std::vector<std::shared_ptr<State::Request>> candidates;
      {std::lock_guard lock(s.mutex);candidates=s.requests;}
      std::shared_ptr<State::Request> r;
      std::shared_ptr<const NativeXmsgProvenance::DispatchFact> fact;
      for(const auto& candidate:candidates){
        if(!candidate->provenance)continue;
        const auto actual=NativeXmsgProvenance::CurrentDispatch(c,*candidate->provenance);
        if(!actual || actual->app!=0xFE || actual->message!=0x20016 ||
           actual->public_invocation.invocation!=candidate->serial ||
           actual->original_argument_bytes!=kPacketBytes || candidate->common.sp<0x2A0 ||
           actual->original_argument!=candidate->common.sp-0x2A0 ||
           actual->original_overlap!=candidate->args[7] ||
           actual->root.kernel!=s.config.allocation->owner().kernel_epoch ||
           actual->root.domain!=s.config.domain->id() || actual->root.generation!=s.root_identity.generation ||
           actual->provider_module.hmodule!=s.root_identity.hmodule ||
           actual->provider_module.generation!=s.root_identity.generation || actual->provider_argument2)continue;
        const bool direct=actual->delivery==NativeXmsgProvenance::Delivery::kDirectInProcess;
        if(direct){
          if(actual->task || actual->provider_message!=0x20016 || actual->provider_argument!=input ||
             input!=actual->original_argument)continue;
        }else if(!actual->task || !actual->envelope || actual->copied_argument!=input ||
            actual->provider_message!=0x80000007 || actual->provider_argument!=actual->envelope ||
            !actual->copied_bytes || actual->copied_bytes->size()!=kPacketBytes ||
            !std::equal(actual->copied_bytes->begin(),actual->copied_bytes->end(),candidate->payload.begin()))continue;
        if(r){std::lock_guard lock(s.mutex);r->uncertain=true;candidate->uncertain=true;return deny(kAbort);}
        r=candidate;fact=actual;
      }
      if(!r || !fact)return deny(kAbort);
      active_request=r;
      const auto refuse=[&](uint32_t status){s.RefuseBeforeBody(r);return deny(status);};
      std::array<uint8_t,kPacketBytes> payload;
      if(!internal::ReadNativeWaitSnapshot(memory,input,payload) || !s.PacketMatches(*r,payload) ||
         payload!=r->payload || NativeXmsgProvenance::CurrentDispatch(c,*r->provenance)!=fact)return refuse(kInvalid);
      const uint32_t native_overlap=fact->native_overlap;
      if(r->args[0]<4) {
        std::error_code user_error;
        const auto roster=s.kernel->xam_state()->profile_manager()->CaptureRosterExpected(user_error);
        if(user_error || !roster.roster || !r->user || roster.roster->slots()[r->args[0]]!=*r->user)
          return refuse(0x800704C7);
      }
      const std::string copied_root(reinterpret_cast<const char*>(payload.data()+0x210));
      {
        std::lock_guard lock(s.mutex);
        if(r->handler_entered || r->source_preparing || r->finished || r->uncertain) {
          r->uncertain=true;return deny(kAbort);
        }
        r->worker=*frame;r->source_preparing=true;r->namespace_id=uint32_t(c->r[4]);
        r->native_overlap=native_overlap;r->root=copied_root;
      }
      std::error_code error;
      // This actual native worker is the sole writer until its epilog's
      // source_finished publication. Move source/native owners straight into
      // the retained Request BEFORE any later allocation/lock can throw. A
      // temporary callback-stack holder must never become a last FD owner.
      r->processed=s.storage->CaptureContentProcessing(PacketWord(payload,0x10));
      if(r->processed && !r->processed->optical_source())
        r->source=s.kernel->content_manager()->sources().CaptureSourceCached(r->processed->volume(),error);
      if(r->processed && (r->source || r->processed->optical_source()) && !error)
        r->device=s.kernel->file_system()->AcquirePathDevice(r->processed->guest_volume_root());
      const bool current=!error && r->processed && s.SourceCurrent(*r);
      uint32_t refused=0;
      {
        std::lock_guard lock(s.mutex);
        r->source_preparing=false;
        if(s.stopping || !NativeXmsgProvenance::FactsAvailable(*r->provenance) ||
            r->finished || r->uncertain || !current) {
          // This guard did not admit the original body. Its actual host source
          // capture is already over, so there is no native own-return to await.
          // Retire only those host duties; original failure/cancel/free remains.
          refused=s.stopping?kAbort:kDisconnected;
        }else r->handler_entered=true;
      }
      if(refused)return refuse(refused);
      return 1;
    }
    // Cancel remains the original native operation. Its task association,
    // actual Cancel/Wait and completion decide the outcome; no host cancellation
    // flag is promoted to a completed request or a successful alias deletion.
    return 1;
  } catch(...) {
    // Every source-capture host call has actually unwound before this catch.
    // Do not leave source_preparing latched merely because metadata allocation
    // failed before the original body was admitted. Retain uncertain owners
    // through factual stop; no native completion/own-return is fabricated.
    if(active_request)s.RefuseBeforeBody(active_request);
    {std::lock_guard lock(s.mutex);
      if(active_request)active_request->uncertain=true;
      for(const auto& r:s.requests)
        if((r->common.thread && r->common.thread==XThread::GetCurrentThread()) ||
            (r->worker.thread && r->worker.thread==XThread::GetCurrentThread()))r->uncertain=true;
    }
    return at==kCommon || at==kWorker?deny(kAbort):1;
  }
}
void NativeContentResolve::ObserveReturn(void* raw,uint64_t address) noexcept {
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);
  if(!c || !c->kernel_state || address>UINT32_MAX || !HooksFunction(uint32_t(address)))return;
  const auto owner=Lookup(c->kernel_state);if(!owner)return;auto& s=*owner->state_;
  std::shared_ptr<State::Request> r;bool release=false;
  try {
    const auto at=uint32_t(address);const auto frame=ActualFrame(*s.kernel,*c,at);
    if(!frame || !s.RootCurrent(at))return;
    {
      std::lock_guard lock(s.mutex);
      for(const auto& value:s.requests) {
        const bool match=at==kCommon?value->common_entered && !value->common_returned && value->common==*frame:
            value->handler_entered && !value->handler_returned && value->worker==*frame;
        if(!match)continue;
        if(r){r->uncertain=true;value->uncertain=true;return;}
        r=value;
      }
      if(!r)return;
      if(at==kWorker) {
        // Every actual blocking host open/mkdir/alias call has returned with
        // this original FE worker. Original Complete/Event/APC/Task tails may
        // still run, but this family retains no guest pointer or later copy.
        r->handler_result=uint32_t(c->r[3]);r->handler_returned=true;
        r->source_finished=true;r->finished=true;
      }else {
        r->common_returned=true;
        // A nonpending original result before worker admission created no
        // host source duty. Ending this subscription neither completes nor
        // cancels a native request. Pending copied work remains observable.
        if(uint32_t(c->r[3])!=X_ERROR_IO_PENDING && !r->handler_entered)
          r->source_finished=r->finished=true;
      }
      release=r->finished;
      s.changed.notify_all();
    }
    if(release && r->provenance)NativeXmsgProvenance::Release(*r->provenance);
  } catch(...) {
    std::lock_guard lock(s.mutex);
    if(r)r->uncertain=true;
    else for(const auto& value:s.requests)
      if(value->common.thread==XThread::GetCurrentThread() || value->worker.thread==XThread::GetCurrentThread())
        value->uncertain=true;
    s.changed.notify_all();
  }
}
std::optional<X_STATUS> NativeContentResolve::CaptureOpenSource(
    KernelState* kernel,std::string_view path,vfs::FileOpenContext& open_context) noexcept try {
  const auto owner=Lookup(kernel);if(!owner || !XThread::IsInThread())return {};
  auto& s=*owner->state_;auto* t=XThread::GetCurrentThread();
  const auto frame=ActualFrame(*kernel,*t->thread_state()->context(),kWorker);
  std::shared_ptr<State::Request> r;bool ambiguous=false;
  {std::lock_guard lock(s.mutex);r=s.Worker(t,ambiguous);}
  if(ambiguous)return X_STATUS_UNSUCCESSFUL;
  if(!r)return {};
  // Actual F648 branches on the descriptor extent. Public134 inserts the
  // genuine F5C0 local-user adapter (B0 frame) before the shared EFC8 path;
  // internal200 calls EFC8 directly. Both reach the same real NtCreateFile.
  const uint32_t depth=r->args[2]==0x134?0x680u:r->args[2]==0x200?0x5D0u:0u;
  if(!frame || !depth || !s.RootCurrent(kWorker) || !s.RootCurrent(0x81687A10) ||
      !s.RootCurrent(0x8167F648) || !s.RootCurrent(0x8167EFC8) ||
      (r->args[2]==0x134 && !s.RootCurrent(0x8167F5C0)) ||
      frame->pcr!=r->worker.pcr || frame->process!=r->worker.process ||
      r->worker.sp<depth || frame->sp!=r->worker.sp-depth || frame->lr!=0x81687ADC ||
      !r->processed || !WholePrefix(path,r->processed->guest_volume_root()) || !s.SourceCurrent(*r))
    return X_STATUS(0xC000009D);
  if(open_context.expected_device &&
      (open_context.expected_device!=r->device ||
       open_context.expected_device.owner_before(r->device) ||
       r->device.owner_before(open_context.expected_device)))return X_STATUS_ACCESS_DENIED;
  // Preserve another independently issued managed-view authority; neither
  // ingress can silently erase the other's exact source constraints.
  open_context.expected_device=r->device;return X_STATUS_SUCCESS;
} catch(...) {return X_STATUS(0xC000009D);}
std::optional<X_STATUS> NativeContentResolve::TryCreateSymbolicLink(
    KernelState* kernel,std::string_view alias,std::string_view target) noexcept try {
  const auto owner=Lookup(kernel);if(!owner || !XThread::IsInThread())return {};
  auto& s=*owner->state_;auto* t=XThread::GetCurrentThread();
  const auto frame=ActualFrame(*kernel,*t->thread_state()->context(),0x816E0400);
  std::shared_ptr<State::Request> r;bool ambiguous=false;
  {std::lock_guard lock(s.mutex);r=s.Worker(t,ambiguous);}
  if(ambiguous)return X_STATUS_UNSUCCESSFUL;
  if(!r || !frame || frame->lr!=0x816E0438 || r->worker.sp<0x3C0 ||
      frame->sp!=r->worker.sp-0x3C0 || frame->pcr!=r->worker.pcr)return {};
  if(!s.RootCurrent(kWorker) || !s.RootCurrent(0x816E0400) || r->root.empty() ||
      !r->processed || !s.SourceCurrent(*r))return X_STATUS(0xC000009D);
  auto expected=r->root;
  if(!xe::utf8::starts_with_case(expected,"\\??\\") &&
      !xe::utf8::starts_with_case(expected,"\\System??\\") && !r->namespace_id)
    expected="\\System??\\"+expected;
  expected=xboxkrnl::xeObSymbolicLinkName(expected+':');
  if(!xe::utf8::equal_case(alias,expected) ||
      !xe::utf8::equal_case(target,r->processed->guest_volume_root()))return X_STATUS_OBJECT_NAME_INVALID;
  // Namespace -> driver metadata is the existing notification-output order.
  // This narrow gate is nonwaiting and invokes no provider/IO/guest callback.
  // It prevents an old processing receipt being revoked between its final
  // check and actual bound-alias publication; the VFS checks source ancestry
  // and the exact live registered Device in the same namespace transaction.
  std::shared_ptr<const vfs::SymbolicLinkToken> issued;
  auto global=global_critical_region::AcquireDirect();
  std::error_code error;
  auto admission=s.storage->TryAdmitContentProcessing(*r->processed,error);
  if(!admission)return vfs::HostStorageError(error);
  std::optional<xam::ContentSourceRegistry::SourceGenerationAdmission> source_admission;
  if(r->source) {
    source_admission=s.kernel->content_manager()->sources().TryAdmitSourceGeneration(*r->source,error);
    if(!source_admission)return vfs::HostStorageError(error);
  }
  // Join this already-bound actual request's revocation/stop cut to namespace
  // publication, without waiting under the caller's native/namespace locks.
  // No Resolve State path takes Driver/registry/namespace while holding this
  // mutex. The real registry guard above holds generation retirement outside
  // this one metadata publication, independent of the XMsg deny-only bit.
  std::unique_lock request(s.mutex,std::try_to_lock);
  if(!request.owns_lock())return vfs::HostStorageError(std::make_error_code(std::errc::device_or_resource_busy));
  if(s.stopping || !r->provenance || !NativeXmsgProvenance::FactsAvailable(*r->provenance) ||
      !r->handler_entered || r->handler_returned || r->uncertain)
    return vfs::HostStorageError(std::make_error_code(std::errc::operation_canceled));
  return kernel->file_system()->RegisterDeviceSymbolicLink(alias,target,r->device,issued);
} catch(...) {return X_STATUS_UNSUCCESSFUL;}
void NativeContentResolve::BeginStop() noexcept try {
  std::lock_guard lock(state_->mutex);state_->stopping=true;state_->changed.notify_all();
} catch(...) {}
bool NativeContentResolve::Drained() const noexcept try {
  std::lock_guard lock(state_->mutex);
  return !state_->reaper_failed && state_->requests.empty();
} catch(...) {return false;}
bool NativeContentResolve::RetainedForActualKernelStop() const noexcept try {
  std::lock_guard lock(state_->mutex);
  // This is permission to retain exact owners THROUGH the actual CPU stop,
  // not a native drain result. No entered host-source worker/IO scope can
  // be abandoned at this boundary. Native callers remain original CPU work.
  return state_->stopping &&
      std::all_of(state_->requests.begin(),state_->requests.end(),[](const auto& r){
        return !r->source_preparing && (!r->handler_entered || r->handler_returned);
      });
} catch(...) {return false;}
bool NativeContentResolve::RetireAfterActualKernelStop() noexcept try {
  if(XThread::IsInThread() || global_critical_region::is_held_by_current_thread())return false;
  const auto keep_alive=shared_from_this();auto& s=*state_;
  std::lock_guard retirement(s.reaper_mutex);
  {std::lock_guard lock(s.mutex);if(s.retired.load())return true;if(!s.stopping || !s.kernel)return false;}
  for(const auto& t:s.kernel->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread))
    if(t->is_running())return false;
  {std::lock_guard lock(s.mutex);s.reaper_stop=true;}
  s.changed.notify_all();if(s.reaper.joinable())s.reaper.join();
  std::vector<std::shared_ptr<State::Request>> retained;
  {
    std::lock_guard lock(s.mutex);s.retired.store(true,std::memory_order_release);
    retained.swap(s.requests);
  }
  {std::lock_guard lock(installed_mutex);const auto i=installed.find(s.kernel);
    if(i!=installed.end() && i->second==keep_alive)installed.erase(i);}
  // An unfinished/uncertain request is discarded ONLY with this already-
  // stopped Kernel. It is never reported as a native completion/cancellation.
  for(const auto& r:retained)if(r->provenance)NativeXmsgProvenance::Release(*r->provenance);
  retained.clear();s.storage.reset();s.root_code.reset();
  s.resident_domain={};s.config={};s.kernel=nullptr;return true;
} catch(...) {return false;}
}  // namespace xe::kernel
