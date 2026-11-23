/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_media_service.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_content_storage_driver.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_media_contract.h"
#include "xenia/kernel/native_media_counted_object.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_task_continuation.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/xam/selected_media_source_proof.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/file.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
constexpr uint32_t kNotReady=0x8007139F,kAbort=0x80004004;
constexpr uint32_t kNativeFaDescriptor=0x81AA0E90,kNativeFaCallback=0x816FF868;
constexpr uint32_t kUsbVtable=0x816196D0,kMediaIndexVtable=0x81618940;
std::optional<uint32_t> Pointer(uint64_t value) {
  const auto low=uint32_t(value);
  return value==low || value==uint64_t(int64_t(int32_t(low)))?std::optional(low):std::nullopt;
}
std::optional<uint32_t> Word(Memory& memory,uint32_t at) {
  std::array<uint8_t,4> bytes;
  return internal::ReadNativeWaitSnapshot(memory,at,bytes)?
      std::optional(xe::load_and_swap<uint32_t>(bytes.data())):std::nullopt;
}
bool HashMatches(std::span<const uint8_t> bytes,std::string_view expected) {
  if(expected.size()!=64)return false;std::array<uint8_t,32> digest;
  mbedtls_sha256(bytes.data(),bytes.size(),digest.data(),0);
  constexpr char hex[]="0123456789abcdef";
  for(size_t n=0;n<digest.size();++n)
    if(expected[n*2]!=hex[digest[n]>>4] || expected[n*2+1]!=hex[digest[n]&15])return false;
  return true;
}
struct Frame {
  uint32_t thread=0,pcr=0,sp=0,lr=0,process=0,function=0;
  bool operator==(const Frame&) const=default;
  auto Key() const {return std::tuple(thread,pcr,sp,lr,function);}
};
std::optional<Frame> ActualFrame(KernelState& kernel,const cpu::ppc::PPCContext& context,uint32_t at) {
  if(!XThread::IsInThread())return {};
  auto* thread=XThread::GetCurrentThread();const auto sp=Pointer(context.r[1]),pcr=Pointer(context.r[13]),lr=Pointer(context.lr);
  if(!thread || !XThread::IsInThread(thread) || thread->kernel_state()!=&kernel ||
      context.kernel_state!=&kernel || context.processor!=kernel.processor() ||
      context.thread_state!=thread->thread_state() || thread->thread_state()->context()!=&context ||
      !sp || (*sp&15) || !pcr || *pcr!=thread->pcr_ptr() || !lr)return {};
  const auto process=thread->native_frame_thread_identity().process_type;
  if(process!=X_PROCTYPE_SYSTEM && process!=X_PROCTYPE_TITLE)return {};
  return Frame{thread->thread_id(),*pcr,*sp,*lr,uint32_t(process),at};
}
std::optional<std::string> NativeMassPath(Memory& memory,uint32_t pointer,bool separator) {
  std::array<uint8_t,15> bytes;
  const size_t size=separator?14:13;
  if(!pointer || !internal::ReadNativeWaitSnapshot(memory,pointer,std::span(bytes).first(size+1)))return {};
  if(bytes[size] || (separator && bytes[13]!='\\'))return {};
  std::string path(bytes.begin(),bytes.begin()+13);
  if(!path.starts_with(R"(\Device\Mass)") || path.back()<'0' || path.back()>'9')return {};
  return path;
}
} // namespace

struct NativeMediaService::State {
  explicit State(Config value):config(std::move(value)),
      kernel(config.catalog->calls()->kernel()),root(config.domain->root()) {
    scopes.reserve(256);tokens.reserve(256);attachment_calls.reserve(64);token_calls.reserve(64);direct_calls.reserve(64);
  }
  Config config;
  KernelState* kernel=nullptr;
  const NativeImportDomain::Member root;
  const std::shared_ptr<const uint8_t> issuer=std::make_shared<const uint8_t>(0);
  std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
  NativeImportDomain::Operation resident_domain;
  std::shared_ptr<NativeContentStorageDriver> storage;
  // Service owns the transport strongly. Captured operations own this owner;
  // weak here avoids Owner->Continuation->Invocation->Owner cycles.
  std::weak_ptr<NativeSystemTaskContinuation> continuation;
  mutable std::mutex mutex;
  std::atomic<bool> ingress_closed{false},fe_observed{false},retired{false},uncertain{false};
  uint64_t next_serial=1;
  std::weak_ptr<NativeMediaService> service;
  struct Attachment;
  struct Cursor {
    uint64_t serial=0;
    Frame factory;
    std::array<uint32_t,8> factory_arguments{};
    uint32_t original_output=0,scratch=0,body=0;
    bool allocation_observed=false,allocation_succeeded=false;
    bool factory_returned=false,factory_succeeded=false,delivered=false;
    bool selected=false,initialized=false,retire_ordinary=false;
    bool close_requested=false,close_in_flight=false,close_returned=false;
    bool close_message_entered=false,close_message_returned=false;
    bool cleanup_running=false,body_free_entered=false,body_free_returned=false;
    bool deferred_until_stopped=false,uncertain=false;
    uint32_t close_result=0x8007139F;
    uint32_t active=0;
    std::optional<Frame> cleanup_frame;
    std::optional<Frame> cleanup_caller;
    uint32_t cleanup_task=0;
    std::shared_ptr<NativeMediaObjectAdmission> admission;
    std::shared_ptr<NativeMediaObjectInsertion> insertion;
    std::shared_ptr<const NativeServiceBackingLease> output_backing;
    std::shared_ptr<xam::SelectedMediaUse> source_use;
    std::shared_ptr<const Attachment> selected_attachment;
  };

  struct Attachment {
    uint32_t object=0,kind=0;
    uint64_t serial=0;
    std::array<uint8_t,36> native_identity{};
    NativeContentStorageDriver::SelectedMediaProcessingReceipt processing;
    bool transitioning=false;
  };
  struct AttachmentCall {
    Frame frame;
    uint32_t object=0,remove=0;
    std::optional<std::string> input;
    std::optional<std::string> old_path;
    std::array<uint8_t,36> before_identity{};
    std::optional<NativeContentStorageDriver::SelectedMediaProcessingReceipt> processing;
  };
  struct Token {
    std::array<uint8_t,kNativeMediaSourceBytes> bytes{};
    std::weak_ptr<const Attachment> attachment;
    uint64_t attachment_serial=0;
  };
  struct TokenCall {
    Frame frame;
    uint32_t index=0,object=0,slot=0,output=0;
    std::shared_ptr<const Attachment> attachment;
  };
  // Direct original1F4 only. Caller output is staged until the actual new
  // File is paired with the prior source/device; native stream/File ownership
  // then survives this call through its genuine handle and I/O references.
  struct DirectCall {
    Memory* memory=nullptr;
    Frame frame;
    std::optional<Frame> backend;
    uint64_t serial=0;
    uint32_t message=0,original_payload=0,original_output=0,scratch=0;
    uint32_t stream=0,file_handle=0;
    bool backend_returned=false,publication_ready=false,delivered=false;
    bool uncertain=false;
    std::array<uint32_t,5> words{};
    std::shared_ptr<const NativeServiceBackingLease> backing;
    std::shared_ptr<const Attachment> attachment;
    std::shared_ptr<vfs::Device> device;
    std::vector<std::shared_ptr<xam::SelectedMediaUse>> source_uses;
    vfs::File* pending_file=nullptr;
    object_ref<XFile> file;
    ~DirectCall() { if(scratch && memory)memory->SystemHeapFree(scratch); }
  };
  std::vector<std::shared_ptr<DirectCall>> direct_calls;
  bool DirectClaim(const cpu::ppc::PPCContext&,uint32_t);
  bool EnterDirect(cpu::ppc::PPCContext&,const Frame&,bool& claimed);
  bool AdmitDirect(const cpu::ppc::PPCContext&,const Frame&,bool& claimed);
  bool ReturnDirect(cpu::ppc::PPCContext&,const Frame&);
  bool DirectCodeCurrent() const {
    constexpr uint32_t code[]={0x816A2F88,0x81703738,0x818613D0,0x81861750,
      0x81862240,0x81862318,0x81863130,0x81863508,0x8187A5D8,0x8187F310,
      0x8187F3C0,0x81880538,0x81885DD0,0x81860E98,0x818610E8,0x818611A8,
      0x819CF4A0,0x819CF4D8,0x819D69B0,0x819CF630,0x819CF7E8,0x819CF4C0,
      0x819CF900,0x819CFB80,0x819D6A30};
    for(auto at:code)if(!RootCurrent(at))return false;
    if(!RootData(kUsbVtable,0x60) || !RootData(0x81619A10,0x64))return false;
    auto global=global_critical_region::AcquireDirect();
    return HashMatches({kernel->memory()->TranslateVirtual<const uint8_t*>(kUsbVtable),0x60},
        "5c9818db380352b02334ed82949462a635a1d080725643e89f5b73c1e0449ed1") &&
      HashMatches({kernel->memory()->TranslateVirtual<const uint8_t*>(0x81619A10),0x64},
        "8c7e3eada76d55f5de92e1230c535a3837a38d7c3ba72f283080e034477e8be6");
  }
  std::optional<Frame> DirectFileFrame(uint32_t output) const {
    auto* thread=XThread::IsInThread()?XThread::GetCurrentThread():nullptr;
    if(!thread || thread->kernel_state()!=kernel)return {};
    const auto* c=thread->thread_state()->context();
    const auto frame=ActualFrame(*kernel,*c,0x818610E8);
    if(!frame || frame->lr!=0x81861158 || frame->sp>UINT32_MAX-0x50 ||
        output!=frame->sp+0x50 || !Pointer(c->r[3]) || uint32_t(c->r[3])!=output)return {};
    return frame;
  }
  std::shared_ptr<DirectCall> DirectFileCaller(const Frame& frame) const {
    for(auto i=direct_calls.rbegin();i!=direct_calls.rend();++i) {
      const auto& call=*i;
      if(call->message==kNativeMediaSourceStream && call->backend &&
          !call->backend_returned && call->frame.thread==frame.thread &&
          call->frame.pcr==frame.pcr && call->frame.process==frame.process &&
          frame.sp<call->backend->sp)return call;
    }
    return {};
  }
  // Weak source metadata only: no file/device/registry owner is cached here.
  std::map<uint32_t,std::shared_ptr<Attachment>> attachments;
  std::vector<Token> tokens;
  std::vector<AttachmentCall> attachment_calls;
  std::vector<TokenCall> token_calls;
  enum class ScopeKind { kFactory,kGetPrivate,kRead,kInit,kReadMessage,kCloseMessage,kClose };
  struct Scope {
    Frame frame;
    ScopeKind kind;
    std::shared_ptr<Cursor> cursor;
    std::shared_ptr<const NativeServiceBackingLease> backing;
    uint32_t message=0,payload=0,native_task=0;
  };
  std::vector<Scope> scopes;
  std::map<uint64_t,std::shared_ptr<Cursor>> cursors;
  std::optional<uint64_t> scheduled_cleanup;

  std::shared_ptr<Cursor> ByBody(uint32_t body) const {
    for(const auto& [serial,cursor]:cursors)if(cursor->body==body && body && !cursor->body_free_entered && !cursor->admission->body_detached_.load())return cursor;
    return {};
  }
  std::shared_ptr<Cursor> ByWorkspace(uint32_t workspace) const {
    return workspace>=kNativeMediaEnumHeaderBytes?ByBody(workspace-kNativeMediaEnumHeaderBytes):nullptr;
  }
  std::shared_ptr<Cursor> ByHandle(uint32_t handle) const {
    for(const auto& [serial,cursor]:cursors)
      if(cursor->insertion && xboxkrnl::NativeMediaCountedBodyAccess::MatchesHandle(*cursor->insertion,handle))return cursor;
    return {};
  }
  std::shared_ptr<const Attachment> TokenAttachment(std::span<const uint8_t> token) const {
    if(token.size()!=kNativeMediaSourceBytes)return {};
    for(const auto& observed:tokens) {
      if(!std::equal(token.begin(),token.end(),observed.bytes.begin()))continue;
      const auto attachment=observed.attachment.lock();
      if(!attachment || attachment->serial!=observed.attachment_serial || attachment->transitioning)continue;
      const auto found=attachments.find(attachment->object);
      if(found!=attachments.end() && found->second==attachment)return attachment;
    }
    return {};
  }
  std::optional<uint32_t> CurrentTask(XThread& thread) const {
    const auto slot=Word(*kernel->memory(),0x81A82570);uint32_t tls=0;
    if(!slot || !thread.GetTLSValue(*slot,&tls))return {};
    const auto context=tls?Word(*kernel->memory(),tls):std::nullopt;
    return context && *context && *context<=UINT32_MAX-0xC?Word(*kernel->memory(),*context+0xC):std::nullopt;
  }
  cpu::GuestCallResult Call(XThread& thread,uint32_t at,std::span<const uint32_t> args) {
    if(!RootCurrent(at) || !XThread::IsInThread(&thread) || thread.kernel_state()!=kernel ||
       global_critical_region::is_held_by_current_thread())return {};
    std::vector<cpu::GuestCallArgument> packed;
    for(auto value:args)packed.push_back(cpu::GuestCallArgument::UInt32(value));
    return cpu::ExecuteGuestCall(kernel->processor(),thread.thread_state(),at,packed);
  }
  bool EnterFactory(cpu::ppc::PPCContext&,const Frame&);
  bool EnterPrivate(cpu::ppc::PPCContext&,const Frame&,const std::shared_ptr<Cursor>&);
  bool EnterRead(cpu::ppc::PPCContext&,const Frame&,const std::shared_ptr<Cursor>&);
  bool EnterMessage(cpu::ppc::PPCContext&,const Frame&,bool& claimed);
  bool EnterClose(cpu::ppc::PPCContext&,const Frame&,const std::shared_ptr<Cursor>&);
  bool AdmitBackend(const cpu::ppc::PPCContext&,const Frame&,bool& claimed);
  bool Claimed(const cpu::ppc::PPCContext&,uint32_t address);
  void ReturnScope(cpu::ppc::PPCContext&,const Frame&);
  bool CleanupOnTask(XThread&,const std::shared_ptr<Cursor>&);


  bool RootCurrent(uint32_t at) const {
    if(retired || !config.catalog || !config.domain || !config.allocation)return false;
    const auto& calls=config.catalog->calls();auto module=calls->module();
    const auto* code=NativeMediaCode(at);
    if(!code || !kernel || retired || !root_code || !resident_domain || !config.domain->active() || !config.allocation->IsAccepting() ||
        !config.allocation->MatchesDomain(config.domain) || !module ||
        module->hmodule_ptr()!=root.hmodule || module->load_generation()!=root.generation ||
        calls->load_generation()!=root.generation || !module->native_import_ready())return false;
    auto global=global_critical_region::AcquireDirect();
    auto& memory=*kernel->memory();
    for(uint32_t offset=0;offset<code->bytes;offset+=4) {
      xex2_section_type section;
      if(kernel->GetUserModuleForAddress(at+offset).get()!=module.get() ||
          !module->xex_module()->GetPageSectionType(at+offset,&section) || section!=XEX_SECTION_CODE ||
          module->xex_module()->TranslateCode(at+offset)!=memory.TranslateVirtual<const uint8_t*>(at+offset))return false;
    }
    if(memory.QueryRangeAccess(at,code->bytes)==memory::PageAccess::kNoAccess)return false;
    return HashMatches({memory.TranslateVirtual<const uint8_t*>(at),code->bytes},code->sha256);
  }
  bool RootData(uint32_t address,uint32_t bytes) const {
    if(retired || !config.catalog || !kernel || !bytes || uint64_t(address)+bytes>(uint64_t(1)<<32))return false;
    const auto module=config.catalog->calls()->module();if(!module)return false;
    auto global=global_critical_region::AcquireDirect();
    if(kernel->memory()->QueryRangeAccess(address,bytes)==memory::PageAccess::kNoAccess)return false;
    for(uint64_t cursor=address;cursor<uint64_t(address)+bytes;cursor=(cursor&~uint64_t(0xFFF))+0x1000) {
      xex2_section_type type;
      if(kernel->GetUserModuleForAddress(uint32_t(cursor)).get()!=module.get() ||
         !module->xex_module()->GetPageSectionType(uint32_t(cursor),&type) ||
         (type!=XEX_SECTION_DATA && type!=XEX_SECTION_READONLY_DATA))return false;
    }
    return true;
  }
  bool TypeCurrent() const {
    constexpr std::array<uint32_t,7> actual_type{0x816E6CE0,0x816E6B10,0x816F1200,0,0,0x81A85484,6};
    if(!RootData(kNativeMediaEnumType,uint32_t(actual_type.size()*4)))return false;
    for(size_t n=0;n<actual_type.size();++n)
      if(Word(*kernel->memory(),kNativeMediaEnumType+uint32_t(n)*4)!=actual_type[n])return false;
    return true;
  }
  bool FaCurrent() const {
    return fe_observed && kernel && !retired && RootCurrent(kNativeFaCallback) &&
        Word(*kernel->memory(),kNativeFaDescriptor+4)==kNativeMediaApp &&
        Word(*kernel->memory(),kNativeFaDescriptor+0xC)==kNativeFaCallback &&
        Word(*kernel->memory(),kNativeFaDescriptor+0x10)==1;
  }
  void EnterAttachment(const cpu::ppc::PPCContext&,const Frame&);
  void ReturnAttachment(const cpu::ppc::PPCContext&,const Frame&);
  void EnterToken(const cpu::ppc::PPCContext&,const Frame&);
  void ReturnToken(const cpu::ppc::PPCContext&,const Frame&);
};

void NativeMediaService::State::EnterAttachment(const cpu::ppc::PPCContext& c,const Frame& frame) {
  if(frame.process!=X_PROCTYPE_SYSTEM)return;
  const auto object=Pointer(c.r[3]),path=Pointer(c.r[4]),remove=Pointer(c.r[5]);
  if(!object || !path || !remove || *object>UINT32_MAX-0x1EC ||
      Word(*kernel->memory(),*object)!=kUsbVtable)return;
  AttachmentCall call;call.frame=frame;call.object=*object;call.remove=*remove;
  call.input=NativeMassPath(*kernel->memory(),*path,false);
  call.old_path=NativeMassPath(*kernel->memory(),*object+0x1D5,true);
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),*object+0xC,call.before_identity))return;
  // Existing actual processed device admission is mandatory. Matching the
  // spelling of MassN alone can never create an attachment/source binding.
  if(call.input && !*remove && !call.old_path)
    call.processing=storage->CaptureSelectedMediaProcessing(*call.input);
  std::lock_guard lock(mutex);
  if(retired || attachment_calls.size()>=64){uncertain=true;return;}
  if(const auto found=attachments.find(*object);found!=attachments.end())found->second->transitioning=true;
  attachment_calls.push_back(std::move(call));
}
void NativeMediaService::State::ReturnAttachment(const cpu::ppc::PPCContext& c,const Frame& frame) {
  AttachmentCall call;
  {
    std::lock_guard lock(mutex);
    const auto found=std::find_if(attachment_calls.begin(),attachment_calls.end(),[&](const auto& value){return value.frame==frame;});
    if(found==attachment_calls.end())return;call=std::move(*found);attachment_calls.erase(found);
  }
  std::array<uint8_t,36> identity;
  const auto path=NativeMassPath(*kernel->memory(),call.object+0x1D5,true);
  std::array<uint8_t,1> kind{};
  const bool observed=internal::ReadNativeWaitSnapshot(*kernel->memory(),call.object+0xC,identity) &&
      internal::ReadNativeWaitSnapshot(*kernel->memory(),call.object+0x1D4,kind);
  const bool binding=observed && !call.remove && call.processing && call.input && path==call.input &&
      uint32_t(c.r[3])==1 && (kind[0]==1 || kind[0]==2) && identity!=call.before_identity &&
      storage->SelectedMediaProcessingCurrent(*call.processing);
  std::shared_ptr<Attachment> fresh;
  if(binding) {
    fresh=std::make_shared<Attachment>();fresh->object=call.object;fresh->kind=kind[0];
    fresh->native_identity=identity;fresh->processing=*call.processing;
  }
  std::lock_guard lock(mutex);
  const auto old=attachments.find(call.object);
  if(!observed){if(old!=attachments.end())old->second->transitioning=true;uncertain=true;return;}
  if(fresh && next_serial && next_serial!=UINT64_MAX) {
    fresh->serial=next_serial++;attachments.insert_or_assign(call.object,std::move(fresh));
  } else if(old!=attachments.end()) {
    if(path && path==call.old_path && identity==call.before_identity)old->second->transitioning=false;
    else attachments.erase(old);
  }
  tokens.erase(std::remove_if(tokens.begin(),tokens.end(),[](const auto& token){return token.attachment.expired();}),tokens.end());
}
void NativeMediaService::State::EnterToken(const cpu::ppc::PPCContext& c,const Frame& frame) {
  const auto index=Pointer(c.r[3]),object=Pointer(c.r[4]),slot=Pointer(c.r[5]),output=Pointer(c.r[6]);
  if(!index || !object || !slot || !output || *slot<7 || *slot>9 ||
      *index>UINT32_MAX-0x848C || Word(*kernel->memory(),*index)!=kMediaIndexVtable ||
      Word(*kernel->memory(),*index+0x8464+*slot*4)!=*object)return;
  std::lock_guard lock(mutex);const auto attachment=attachments.find(*object);
  if(retired || attachment==attachments.end() || attachment->second->transitioning || token_calls.size()>=64)return;
  token_calls.push_back({frame,*index,*object,*slot,*output,attachment->second});
}
void NativeMediaService::State::ReturnToken(const cpu::ppc::PPCContext& c,const Frame& frame) {
  TokenCall call;
  {
    std::lock_guard lock(mutex);const auto found=std::find_if(token_calls.begin(),token_calls.end(),[&](const auto& value){return value.frame==frame;});
    if(found==token_calls.end())return;call=std::move(*found);token_calls.erase(found);
  }
  Token actual;actual.attachment=call.attachment;actual.attachment_serial=call.attachment->serial;
  if(uint32_t(c.r[3]) || !storage->SelectedMediaProcessingCurrent(call.attachment->processing) ||
      !internal::ReadNativeWaitSnapshot(*kernel->memory(),call.output,actual.bytes) ||
      xe::load_and_swap<uint32_t>(actual.bytes.data())!=call.slot ||
      !std::equal(call.attachment->native_identity.begin(),call.attachment->native_identity.end(),actual.bytes.begin()+4))return;
  std::lock_guard lock(mutex);const auto current=attachments.find(call.object);
  if(retired || current==attachments.end() || current->second!=call.attachment || current->second->transitioning)return;
  const auto previous=std::find_if(tokens.begin(),tokens.end(),[&](const auto& token){return token.bytes==actual.bytes;});
  if(previous!=tokens.end())*previous=std::move(actual);
  else if(tokens.size()<256)tokens.push_back(std::move(actual));
  else uncertain=true;
}


namespace {
bool ReadWords(Memory& memory,uint32_t address,std::span<uint32_t> words) {
  if(words.size()>8)return false;
  std::array<uint8_t,32> raw{};
  if(!internal::ReadNativeWaitSnapshot(memory,address,std::span(raw).first(words.size()*4)))return false;
  for(size_t n=0;n<words.size();++n)words[n]=xe::load_and_swap<uint32_t>(raw.data()+n*4);
  return true;
}
bool ReadDirectWords(Memory& memory,uint32_t address,std::span<uint32_t> words) {
  if(words.size()!=5)return false;
  std::array<uint8_t,kNativeMediaDirectPayloadBytes> raw{};
  if(XFAILED(util::ReadGuestIoBuffer(memory,address,raw)))return false;
  for(size_t n=0;n<words.size();++n)words[n]=xe::load_and_swap<uint32_t>(raw.data()+n*4);
  return true;
}
bool Succeeded(uint32_t result) {return int32_t(result)>=0;}
}

std::shared_ptr<const NativeAvatarBackingLease> NativeMediaService::BorrowDirectBacking(
    Memory& memory,uint32_t message,uint32_t payload,std::span<const uint32_t> words) {
  if(global_critical_region::is_held_by_current_thread() ||
      !NativeMediaDirectPayloadValid(message,words))return {};
  std::vector<NativeServiceBackingRange> ranges{{payload,kNativeMediaDirectPayloadBytes,false}};
  if(message==kNativeMediaSourceMetadata) {
    // The native capacity error precedes every array access; keep that real
    // error/count behavior without pretending an undersized array was used.
    if(words[2] && words[3]>=kNativeMediaSourceMetadataCapacity)
      ranges.push_back({words[2],kNativeMediaSourceMetadataCapacity*kNativeMediaSourceMetadataBytes,true});
  } else ranges.push_back({words[1],kNativeMediaFileIdentityBytes,false});
  ranges.push_back({words[4],4,true});
  for(size_t i=0;i<ranges.size();++i) {
    const auto& a=ranges[i];
    if(!util::GuestIoRangeValid(memory,a.address,a.length,a.writable))return {};
    for(size_t j=0;j<i;++j)if(util::GuestIoRangesOverlap(memory,a.address,a.length,
        ranges[j].address,ranges[j].length))return {};
  }
  return memory.BorrowNativeServiceBacking(ranges);
}
bool NativeMediaService::State::DirectClaim(const cpu::ppc::PPCContext& c,uint32_t at) {
  if(at!=0x81706478 && uint32_t(c.r[3])!=kNativeMediaApp)return false;
  const uint32_t message=uint32_t(c.r[4]);
  if(message!=kNativeMediaSourceMetadata && message!=kNativeMediaSourceStream)return false;
  const uint32_t payload=uint32_t(c.r[at==0x816A2CF0?6:5]);
  {
    std::lock_guard lock(mutex);
    for(const auto& call:direct_calls)if(call->message==message && call->scratch==payload)return true;
    if(message==kNativeMediaSourceMetadata)return fe_observed && !attachments.empty();
  }
  std::array<uint32_t,5> words{};
  std::array<uint8_t,kNativeMediaSourceBytes> token{};
  if(at==0x81706478 || global_critical_region::is_held_by_current_thread()) {
    if(!ReadWords(*kernel->memory(),payload,words) || !words[1] ||
        !internal::ReadNativeWaitSnapshot(*kernel->memory(),words[1],token))return false;
  } else {
    if(!RootCurrent(at) || !ReadDirectWords(*kernel->memory(),payload,words) || !words[1] ||
        XFAILED(util::ReadGuestIoBuffer(*kernel->memory(),words[1],token)))return false;
  }
  std::lock_guard lock(mutex);return bool(TokenAttachment(token));
}
bool NativeMediaService::State::EnterDirect(cpu::ppc::PPCContext& c,const Frame& frame,bool& claimed) {
  claimed=DirectClaim(c,frame.function);
  // Exact direct B is the normal source inventory interface even when the
  // current selected set is empty. Unrelated native async B remains native.
  if(uint32_t(c.r[4])==kNativeMediaSourceMetadata && frame.function==0x816A2F88 && fe_observed)claimed=true;
  if(!claimed)return true;
  if(frame.function!=0x816A2F88 || ingress_closed || uncertain ||
      global_critical_region::is_held_by_current_thread() || !FaCurrent() || !DirectCodeCurrent())return false;
  const auto payload=Pointer(c.r[5]);
  if(!payload)return false;
  auto call=std::make_shared<DirectCall>();call->memory=kernel->memory();call->frame=frame;
  call->message=uint32_t(c.r[4]);call->original_payload=*payload;
  if(!ReadDirectWords(*call->memory,*payload,call->words) ||
      !NativeMediaDirectPayloadValid(call->message,call->words))return false;
  call->original_output=call->words[4];
  call->backing=NativeMediaService::BorrowDirectBacking(*call->memory,call->message,*payload,call->words);
  if(!call->backing)return false;
  std::array<uint32_t,5> repeated{};
  if(!ReadDirectWords(*call->memory,*payload,repeated) || repeated!=call->words)return false;
  std::array<uint8_t,kNativeMediaFileIdentityBytes> identity{};
  std::vector<std::shared_ptr<const Attachment>> selected;
  if(call->message==kNativeMediaSourceStream) {
    if(XFAILED(util::ReadGuestIoBuffer(*call->memory,call->words[1],identity)) ||
        !NativeMediaFileIdentityValid(identity))return false;
    std::lock_guard lock(mutex);
    call->attachment=TokenAttachment(std::span(identity).first(kNativeMediaSourceBytes));
    if(!call->attachment)return false;
    selected.push_back(call->attachment);
  } else {
    std::lock_guard lock(mutex);
    for(const auto& [object,attachment]:attachments)if(!attachment->transitioning)selected.push_back(attachment);
  }
  for(const auto& attachment:selected) {
    if(!storage->SelectedMediaProcessingCurrent(attachment->processing))return false;
    std::error_code error;
    auto use=kernel->xam_state()->content_manager()->sources().CaptureSelectedMediaUse(
        attachment->processing.source,error);
    if(error || !use)return false;
    call->source_uses.push_back(std::move(use));
  }
  if(call->attachment) {
    call->device=call->attachment->processing.source->device_.lock();
    if(!call->device)return false;
  }
  const uint32_t scratch_bytes=call->message==kNativeMediaSourceStream?
      24+kNativeMediaFileIdentityBytes:kNativeMediaDirectPayloadBytes;
  call->scratch=call->memory->SystemHeapAlloc(scratch_bytes);
  if(!call->scratch)return false;
  // Capture C's exact nested value into this actual owned request. The native
  // stream stores its File, not this temporary identity/path pointer.
  auto native=call->words;
  if(call->message==kNativeMediaSourceStream) {
    native[1]=call->scratch+24;native[4]=call->scratch+20;
    std::memcpy(call->memory->TranslateVirtual(call->scratch+24),identity.data(),identity.size());
    xe::store_and_swap<uint32_t>(call->memory->TranslateVirtual(call->scratch+20),0);
  }
  for(size_t i=0;i<native.size();++i)
    xe::store_and_swap<uint32_t>(call->memory->TranslateVirtual(call->scratch+uint32_t(i)*4),native[i]);
  {
    std::lock_guard lock(mutex);
    if(ingress_closed || retired || uncertain || !next_serial || next_serial==UINT64_MAX || direct_calls.size()>=64)return false;
    for(const auto& attachment:selected) {
      const auto actual=attachments.find(attachment->object);
      if(actual==attachments.end() || actual->second!=attachment || attachment->transitioning ||
          !attachment->processing.source->SourceCurrentCached())return false;
    }
    call->serial=next_serial++;direct_calls.push_back(call);
  }
  c.r[5]=call->scratch;
  return true;
}
bool NativeMediaService::State::AdmitDirect(const cpu::ppc::PPCContext& c,const Frame& frame,bool& claimed) {
  claimed=DirectClaim(c,frame.function);
  if(!claimed)return true;
  const auto manager=Word(*kernel->memory(),0x81AAD7A8);
  if(!manager || *manager!=uint32_t(c.r[3]) || !DirectCodeCurrent())return false;
  std::lock_guard lock(mutex);
  for(auto i=direct_calls.rbegin();i!=direct_calls.rend();++i) {
    const auto& call=*i;
    if(call->message!=uint32_t(c.r[4]) || call->scratch!=uint32_t(c.r[5]) ||
        call->frame.thread!=frame.thread || call->frame.pcr!=frame.pcr ||
        call->frame.process!=frame.process)continue;
    if(call->backend || retired || uncertain || ingress_closed || !call->backing->current())return false;
    if(call->attachment && !call->attachment->processing.source->SourceCurrentCached())return false;
    call->backend=frame;return true;
  }
  return false; // Selected async/unobserved ingresses have no direct owner.
}
bool NativeMediaService::State::ReturnDirect(cpu::ppc::PPCContext& c,const Frame& frame) {
  std::shared_ptr<DirectCall> call;
  {
    std::lock_guard lock(mutex);
    for(const auto& candidate:direct_calls) {
      if(candidate->backend && *candidate->backend==frame) {
        candidate->backend_returned=true;return true;
      }
      if(candidate->frame==frame)call=candidate;
    }
  }
  if(!call)return false;
  bool healthy=true;
  if(call->backend && !call->backend_returned)healthy=false;
  if(call->message==kNativeMediaSourceStream && call->backend_returned) {
    const auto output=Word(*call->memory,call->scratch+20);
    if(!output)healthy=false;
    else if(Succeeded(uint32_t(c.r[3]))) {
      const auto vtable=*output?Word(*call->memory,*output):std::nullopt;
      const auto refs=*output && *output<=UINT32_MAX-0x68?Word(*call->memory,*output+0x0C):std::nullopt;
      const auto file=*output && *output<=UINT32_MAX-0x68?Word(*call->memory,*output+0x2C):std::nullopt;
      const auto flags=*output && *output<=UINT32_MAX-0x68?Word(*call->memory,*output+0x50):std::nullopt;
      if(!vtable || *vtable!=0x81619A10 || !refs || *refs!=1 || !file || !flags || !(*flags&1) ||
          !call->file || call->file_handle!=*file || !DirectCodeCurrent())healthy=false;
      else {std::lock_guard lock(mutex);call->stream=*output;call->publication_ready=true;}
    } else if(*output)healthy=false;
    else {std::lock_guard lock(mutex);call->publication_ready=true;}
    if(healthy) {
      std::array<uint8_t,4> bytes{};xe::store_and_swap<uint32_t>(bytes.data(),*output);
      const util::GuestIoWrite write{call->original_output,bytes};
      util::GuestIoNativeMediaFactoryScope admission(service.lock(),call->serial,*output,*call->memory);
      const auto published=util::WriteGuestIoBuffers(*call->memory,std::span(&write,1),admission);
      if(XFAILED(published) && Succeeded(uint32_t(c.r[3])))
        c.r[3]=uint64_t(int64_t(int32_t(kAbort)));
    }
    // A real new stream was not exposed if final host publication refused.
    // Release that exact owned native ref on this original healthy caller.
    if(healthy && call->stream && !call->delivered) {
      const std::array<uint32_t,1> args{call->stream};
      const auto released=Call(*XThread::GetCurrentThread(),0x819D6A30,args);
      if(!released.executed || uint32_t(released.value)!=0)healthy=false;
    }
  }
  if(!healthy) {
    std::lock_guard lock(mutex);call->uncertain=true;uncertain=true;
    c.r[3]=uint64_t(int64_t(int32_t(kAbort)));return true;
  }
  // Local call retains all resources through removal. Final File/source/FD
  // holders and owned scratch die outside the owner metadata mutex.
  {std::lock_guard lock(mutex);std::erase(direct_calls,call);}
  return true;
}

bool NativeMediaService::State::EnterFactory(cpu::ppc::PPCContext& c,const Frame& frame) {
  std::array<uint32_t,8> args;
  for(size_t n=0;n<args.size();++n) {
    const auto value=Pointer(c.r[n+3]);if(!value)return false;args[n]=*value;
  }
  if(!NativeMediaCreateArgumentsValid(args) || ingress_closed || uncertain || !FaCurrent() || !TypeCurrent() ||
     !util::GuestIoRangeValid(*kernel->memory(),args[7],4,true))return false;
  const NativeServiceBackingRange output{args[7],4,true};
  auto backing=kernel->memory()->BorrowNativeServiceBacking(std::span(&output,1));
  if(!backing)return false;
  const auto scratch=kernel->memory()->SystemHeapAlloc(4);
  if(!scratch)return false;
  std::shared_ptr<Cursor> cursor;
  try {
    cursor=std::make_shared<Cursor>();cursor->factory=frame;cursor->factory_arguments=args;
    cursor->original_output=args[7];cursor->scratch=scratch;cursor->output_backing=std::move(backing);
    bool admitted=false;
    {
      std::lock_guard lock(mutex);
      if(!ingress_closed && !retired && next_serial && next_serial!=UINT64_MAX && cursors.size()<256 && scopes.size()<256) {
        cursor->serial=next_serial++;
        cursor->admission=std::shared_ptr<NativeMediaObjectAdmission>(
            new NativeMediaObjectAdmission(issuer,kernel,cursor->serial));
        cursors.emplace(cursor->serial,cursor);
        scopes.push_back({frame,ScopeKind::kFactory,cursor,{}});
        admitted=true;
      }
    }
    if(!admitted){kernel->memory()->SystemHeapFree(scratch);return false;}
  } catch(...) {kernel->memory()->SystemHeapFree(scratch);throw;}
  // This stages only the API out-DWORD. Actual ObjectTable insertion is not
  // hidden; its observed identity is adopted before this caller gets a handle.
  c.r[10]=scratch;
  return true;
}
bool NativeMediaService::State::EnterPrivate(cpu::ppc::PPCContext& c,
    const Frame& frame,const std::shared_ptr<Cursor>& cursor) {
  const auto output=Pointer(c.r[4]);
  if(!output || !*output || ingress_closed ||
      !util::GuestIoRangeValid(*kernel->memory(),*output,4,true))return false;
  const NativeServiceBackingRange range{*output,4,true};
  auto backing=kernel->memory()->BorrowNativeServiceBacking(std::span(&range,1));
  if(!backing)return false;
  std::lock_guard lock(mutex);
  if(retired || ingress_closed || cursor->close_requested || cursor->retire_ordinary ||
     cursor->cleanup_running || !cursor->factory_succeeded || !cursor->delivered ||
      !cursor->insertion || !cursor->insertion->actual_reference_held_ || scopes.size()>=256)return false;
  ++cursor->active;scopes.push_back({frame,ScopeKind::kGetPrivate,cursor,std::move(backing)});
  return true;
}
bool NativeMediaService::State::EnterRead(cpu::ppc::PPCContext& c,
    const Frame& frame,const std::shared_ptr<Cursor>& cursor) {
  const auto buffer=Pointer(c.r[5]),length=Pointer(c.r[6]),count=Pointer(c.r[7]),overlap=Pointer(c.r[8]);
  // First cohort owns the genuine SYSTEM synchronous route only. In
  // particular native250 on TITLE may schedule even when overlap is null.
  if(frame.process!=X_PROCTYPE_SYSTEM || !overlap || *overlap || !buffer || !length || !count ||
     !*buffer || !*length || !*count || ingress_closed ||
     !util::GuestIoRangeValid(*kernel->memory(),*buffer,*length,true) ||
     !util::GuestIoRangeValid(*kernel->memory(),*count,4,true) ||
     util::GuestIoRangesOverlap(*kernel->memory(),*buffer,*length,*count,4))return false;
  const std::array<NativeServiceBackingRange,2> ranges{{{*buffer,*length,true},{*count,4,true}}};
  auto backing=kernel->memory()->BorrowNativeServiceBacking(ranges);
  if(!backing)return false;
  std::lock_guard lock(mutex);
  if(retired || !cursor->selected || !cursor->initialized || cursor->close_requested ||
     !cursor->source_use || !cursor->selected_attachment ||
     !cursor->selected_attachment->processing.source->SourceCurrentCached() || scopes.size()>=256)return false;
  ++cursor->active;scopes.push_back({frame,ScopeKind::kRead,cursor,std::move(backing)});
  return true;
}
bool NativeMediaService::State::EnterMessage(cpu::ppc::PPCContext& c,
    const Frame& frame,bool& claimed) {
  claimed=false;
  if(uint32_t(c.r[3])!=kNativeMediaApp)return true;
  if(uint32_t(c.r[4])==kNativeMediaSourceMetadata || uint32_t(c.r[4])==kNativeMediaSourceStream)
    return EnterDirect(c,frame,claimed);
  const uint32_t message=uint32_t(c.r[4]),payload=uint32_t(c.r[frame.function==0x816A2CF0?6:5]);
  if(message!=kNativeMediaQueryInit && message!=kNativeMediaQueryRead && message!=kNativeMediaQueryClose)return true;
  std::array<uint32_t,8> request{};
  uint32_t workspace=payload;
  if(message!=kNativeMediaQueryClose) {
    if(!ReadWords(*kernel->memory(),payload,request))return true;
    workspace=request[message==kNativeMediaQueryInit?7:2];
  }
  std::shared_ptr<Cursor> cursor;
  std::shared_ptr<const Attachment> attachment;
  if(message==kNativeMediaQueryInit && bool(request[1])!=bool(request[2])) {
    std::array<uint8_t,kNativeMediaSourceBytes> token{};
    if(internal::ReadNativeWaitSnapshot(*kernel->memory(),request[1]?request[1]:request[2],token)) {
      std::lock_guard lock(mutex);attachment=TokenAttachment(token);
    }
  }
  {
    std::lock_guard lock(mutex);cursor=ByWorkspace(workspace);
    claimed=bool(attachment) || (cursor && cursor->selected);
  }
  if(!cursor)return !claimed; // An unknown cursor is never retroactively adopted.
  if(!claimed)return true; // Ordinary native media behavior remains native.
  // Async selected claims are refused in this source cohort. Ordinary StartIO
  // and every unrelated app still use their existing exact native/HLE route.
  const auto task=CurrentTask(*XThread::GetCurrentThread()).value_or(0);
  if(frame.function!=0x816A2F88 || frame.process!=X_PROCTYPE_SYSTEM || !task)return false;
  if(message==kNativeMediaQueryClose) {
    std::lock_guard lock(mutex);
    if(!cursor->cleanup_running || !cursor->close_in_flight || cursor->active ||
       !cursor->cleanup_caller || cursor->cleanup_caller->thread!=frame.thread ||
       cursor->cleanup_caller->pcr!=frame.pcr || cursor->cleanup_task!=task ||
       frame.lr!=0x816F12A0 || scopes.size()>=256)return false;
    cursor->close_message_entered=true;
    scopes.push_back({frame,ScopeKind::kCloseMessage,cursor,{},message,payload,task});return true;
  }
  if(ingress_closed || (request[7] && message==kNativeMediaQueryRead))return false;
  std::shared_ptr<xam::SelectedMediaUse> source_use;
  if(message==kNativeMediaQueryInit) {
    if(!attachment || !storage->SelectedMediaProcessingCurrent(attachment->processing))return false;
    std::error_code error;
    source_use=kernel->xam_state()->content_manager()->sources().CaptureSelectedMediaUse(
        attachment->processing.source,error);
    if(error || !source_use)return false;
  }
  std::vector<NativeServiceBackingRange> ranges{{payload,32,false}};
  if(message==kNativeMediaQueryInit)
    ranges.push_back({request[1]?request[1]:request[2],kNativeMediaSourceBytes,false});
  if(message==kNativeMediaQueryRead) {
    if(!request[3] || !request[4] || !request[5] ||
       !util::GuestIoRangeValid(*kernel->memory(),request[3],request[4],true) ||
       (request[6] && !util::GuestIoRangeValid(*kernel->memory(),request[6],4,true)) ||
       (request[6] && util::GuestIoRangesOverlap(*kernel->memory(),request[3],request[4],request[6],4)))return false;
    ranges.push_back({request[3],request[4],true});
    if(request[6])ranges.push_back({request[6],4,true});
  }
  auto backing=kernel->memory()->BorrowNativeServiceBacking(ranges);
  if(!backing)return false;
  std::lock_guard lock(mutex);
  if(retired || cursor->close_requested || !cursor->factory_succeeded || !cursor->insertion ||
      !cursor->insertion->actual_reference_held_ || scopes.size()>=256)return false;
  if(message==kNativeMediaQueryInit) {
    if(cursor->selected || cursor->initialized || cursor->active || cursor->retire_ordinary || cursor->cleanup_running)return false;
    const auto actual=attachments.find(attachment->object);
    if(actual==attachments.end() || actual->second!=attachment || attachment->transitioning)return false;
    if(!xboxkrnl::NativeMediaCountedBodyAccess::BindSelected(*cursor->insertion))return false;
    cursor->selected=true;cursor->source_use=std::move(source_use);cursor->selected_attachment=std::move(attachment);
  } else if(!cursor->initialized || !cursor->selected_attachment ||
      !cursor->selected_attachment->processing.source->SourceCurrentCached())return false;
  ++cursor->active;
  scopes.push_back({frame,message==kNativeMediaQueryInit?ScopeKind::kInit:ScopeKind::kReadMessage,cursor,std::move(backing),message,payload,task});
  return true;
}
bool NativeMediaService::State::Claimed(const cpu::ppc::PPCContext& c,uint32_t address) {
  if(address==0x816F0F78) {
    std::array<uint32_t,8> args{};
    for(size_t n=0;n<args.size();++n){const auto value=Pointer(c.r[n+3]);if(!value)return false;args[n]=*value;}
    return fe_observed.load() && NativeMediaCreateArgumentsValid(args);
  }
  if(address==0x816F1040 || address==0x816F1070 || address==0x816F1200) {
    std::lock_guard lock(mutex);
    const auto cursor=address==0x816F1200?ByBody(uint32_t(c.r[3])):ByHandle(uint32_t(c.r[3]));
    return cursor && (cursor->selected || address==0x816F1040);
  }
  if(address!=0x816A2F88 && address!=0x816A2CF0 && address!=0x816A2FF8 && address!=0x81706478)return false;
  if(DirectClaim(c,address))return true;
  if(address!=0x81706478 && uint32_t(c.r[3])!=kNativeMediaApp)return false;
  const uint32_t message=uint32_t(c.r[4]),payload=uint32_t(c.r[address==0x816A2CF0?6:5]);
  if(message!=kNativeMediaQueryInit && message!=kNativeMediaQueryRead && message!=kNativeMediaQueryClose)return false;
  std::array<uint32_t,8> request{};uint32_t workspace=payload;
  if(message!=kNativeMediaQueryClose) {
    if(!ReadWords(*kernel->memory(),payload,request))return false;
    workspace=request[message==kNativeMediaQueryInit?7:2];
  }
  if(message==kNativeMediaQueryInit && bool(request[1])!=bool(request[2])) {
    std::array<uint8_t,kNativeMediaSourceBytes> token{};
    if(internal::ReadNativeWaitSnapshot(*kernel->memory(),request[1]?request[1]:request[2],token)) {
      std::lock_guard lock(mutex);if(TokenAttachment(token))return true;
    }
  }
  std::lock_guard lock(mutex);const auto cursor=ByWorkspace(workspace);return cursor && cursor->selected;
}
bool NativeMediaService::State::AdmitBackend(const cpu::ppc::PPCContext& c,
    const Frame& frame,bool& claimed) {
  claimed=false;
  const uint32_t message=uint32_t(c.r[4]),payload=uint32_t(c.r[5]);
  if(message==kNativeMediaSourceMetadata || message==kNativeMediaSourceStream)
    return AdmitDirect(c,frame,claimed);
  if(message!=kNativeMediaQueryInit && message!=kNativeMediaQueryRead && message!=kNativeMediaQueryClose)return true;
  uint32_t workspace=payload;std::array<uint32_t,8> request{};
  std::shared_ptr<const Attachment> attachment;
  if(message!=kNativeMediaQueryClose) {
    if(!ReadWords(*kernel->memory(),payload,request))return true;
    workspace=request[message==kNativeMediaQueryInit?7:2];
    if(message==kNativeMediaQueryInit && bool(request[1])!=bool(request[2])) {
      std::array<uint8_t,kNativeMediaSourceBytes> token{};
      if(internal::ReadNativeWaitSnapshot(*kernel->memory(),request[1]?request[1]:request[2],token)) {
        std::lock_guard lock(mutex);attachment=TokenAttachment(token);
      }
    }
  }
  const auto manager=Word(*kernel->memory(),0x81AAD7A8);
  const auto task=CurrentTask(*XThread::GetCurrentThread()).value_or(0);
  std::lock_guard lock(mutex);const auto cursor=ByWorkspace(workspace);
  claimed=bool(attachment) || (cursor && cursor->selected);
  if(!claimed)return true;
  if(!cursor || !cursor->selected || !manager || *manager!=uint32_t(c.r[3]) ||
      frame.process!=X_PROCTYPE_SYSTEM)return false;
  // The actual FA dispatcher is the common final gate for unobserved native
  // async/other ingresses too. Only a retained SAME-thread original1F4 scope
  // with its exact original message/payload can enter this sync cohort.
  for(const auto& scope:scopes)if(scope.cursor==cursor &&
      scope.frame.thread==frame.thread && scope.frame.pcr==frame.pcr &&
      scope.frame.process==frame.process && scope.frame.function==0x816A2F88 &&
      scope.message==message && scope.payload==payload && task && scope.native_task==task &&
      (scope.kind==ScopeKind::kInit || scope.kind==ScopeKind::kReadMessage || scope.kind==ScopeKind::kCloseMessage))return true;
  return false;
}
bool NativeMediaService::State::EnterClose(cpu::ppc::PPCContext&,
    const Frame& frame,const std::shared_ptr<Cursor>& cursor) {
  const auto task=CurrentTask(*XThread::GetCurrentThread()).value_or(0);
  std::lock_guard lock(mutex);
  // Native close_proc is called per alias, and DUPLICATE_CLOSE_SOURCE invokes
  // it while the SAME slot survives. Neither is the cursor's final close.
  // The actual table's last-slot detach is the only logical close admission.
  if(!cursor->admission->handles_closed_.load(std::memory_order_acquire))return false;
  cursor->close_requested=true;
  cursor->admission->ingress_closed_.store(true,std::memory_order_release);
  if(!cursor->cleanup_running || frame.process!=X_PROCTYPE_SYSTEM || cursor->active ||
      !cursor->cleanup_caller || cursor->cleanup_caller->thread!=frame.thread ||
      cursor->cleanup_caller->pcr!=frame.pcr || cursor->cleanup_task!=task || !task ||
      cursor->cleanup_caller->sp<cpu::kGuestCallExecuteStackPadding ||
      frame.sp!=cursor->cleanup_caller->sp-cpu::kGuestCallExecuteStackPadding ||
      cursor->close_in_flight || scopes.size()>=256)return false;
  cursor->close_in_flight=true;cursor->cleanup_frame=frame;
  scopes.push_back({frame,ScopeKind::kClose,cursor,{}});return true;
}
void NativeMediaService::State::ReturnScope(cpu::ppc::PPCContext& c,const Frame& frame) {
  Scope scope;
  {
    std::lock_guard lock(mutex);
    const auto found=std::find_if(scopes.rbegin(),scopes.rend(),[&](const auto& entry){return entry.frame==frame;});
    if(found==scopes.rend())return;
    scope=std::move(*found);scopes.erase(std::next(found).base());
    if(scope.kind!=ScopeKind::kFactory && scope.kind!=ScopeKind::kClose &&
       scope.kind!=ScopeKind::kCloseMessage) {
      if(!scope.cursor->active){uncertain=true;scope.cursor->uncertain=true;return;}
      --scope.cursor->active;
    }
  }
  auto& cursor=*scope.cursor;
  if(scope.kind==ScopeKind::kFactory) {
    const auto actual_handle=Word(*kernel->memory(),cursor.scratch);
    const bool successful=Succeeded(uint32_t(c.r[3])) && actual_handle && cursor.insertion &&
        cursor.insertion->actual_insert_observed_ && cursor.insertion->actual_reference_held_ &&
        cursor.insertion->actual_handle_==*actual_handle &&
        xboxkrnl::NativeMediaCountedBodyAccess::MatchesHandle(*cursor.insertion,*actual_handle);
    std::array<uint8_t,4> bytes{};
    if(successful)xe::store_and_swap<uint32_t>(bytes.data(),*actual_handle);
    bool delivered=false;
    if(successful && cursor.output_backing && cursor.output_backing->current()) {
      const auto actual_owner=service.lock();
      util::GuestIoNativeMediaFactoryScope admission(actual_owner,cursor.serial,*actual_handle,*kernel->memory());
      const util::GuestIoWrite output{cursor.original_output,bytes};
      delivered=XSUCCEEDED(util::WriteGuestIoBuffers(*kernel->memory(),std::span(&output,1),admission));
    }
    std::shared_ptr<const NativeServiceBackingLease> output_backing;
    {
      std::lock_guard lock(mutex);cursor.factory_returned=true;cursor.factory_succeeded=successful;
      cursor.delivered=delivered;
      if(!delivered)cursor.close_requested=true;
      if(cursor.insertion && cursor.insertion->actual_insert_observed_ &&
          !cursor.insertion->actual_reference_held_)cursor.deferred_until_stopped=true;
      output_backing=std::move(cursor.output_backing);
    }
    output_backing.reset(); // committed backing release outside metadata locks
    // The original output register is restored; no altered formal pointer
    // escapes the public export. A refused delivery does not expose the handle.
    c.r[10]=cursor.original_output;
    if(successful && !delivered)c.r[3]=uint64_t(int64_t(int32_t(kAbort)));
    const uint32_t scratch=std::exchange(cursor.scratch,0u);
    if(scratch)kernel->memory()->SystemHeapFree(scratch);
  } else if(scope.kind==ScopeKind::kInit) {
    bool initialized=Succeeded(uint32_t(c.r[3]));
    const uint32_t workspace=cursor.body+kNativeMediaEnumHeaderBytes;
    const auto source=Word(*kernel->memory(),workspace+0x23C);
    std::array<uint8_t,kNativeMediaSourceBytes> actual{};
    initialized=initialized && source && *source>=workspace &&
        uint64_t(*source)+actual.size()<=uint64_t(workspace)+kNativeMediaQueryBytes &&
        internal::ReadNativeWaitSnapshot(*kernel->memory(),*source,actual);
    std::lock_guard lock(mutex);
    const auto attachment=initialized?TokenAttachment(actual):nullptr;
    initialized=initialized && attachment && attachment==cursor.selected_attachment;
    cursor.initialized=initialized;
    if(!initialized)cursor.close_requested=true;
  } else if(scope.kind==ScopeKind::kCloseMessage) {
    std::lock_guard lock(mutex);cursor.close_message_returned=true;cursor.close_result=uint32_t(c.r[3]);
  } else if(scope.kind==ScopeKind::kClose) {
    std::lock_guard lock(mutex);cursor.close_returned=true;cursor.close_in_flight=false;
  }
  // Backing tokens are released only after this actual synchronous own-return.
  // No return/status value is interpreted as APC delivery or worker drainage.
}

struct NativeMediaObjectCreation::State {
  std::weak_ptr<NativeMediaService> owner;
  uint64_t serial=0;
  bool completed=false;
};
NativeMediaObjectCreation::NativeMediaObjectCreation(std::unique_ptr<State> state):state_(std::move(state)) {}
NativeMediaObjectCreation::~NativeMediaObjectCreation()=default;
void NativeMediaObjectCreation::Complete(uint32_t status,uint32_t actual_body) noexcept try {
  if(!state_ || state_->completed)return;
  state_->completed=true;
  const auto owner=state_->owner.lock();if(!owner)return;
  auto& s=*owner->state_;
  std::lock_guard lock(s.mutex);
  const auto found=s.cursors.find(state_->serial);
  if(found==s.cursors.end())return;
  auto& cursor=*found->second;
  if(cursor.allocation_observed || cursor.factory_returned){cursor.uncertain=true;s.uncertain=true;return;}
  cursor.allocation_observed=true;
  cursor.allocation_succeeded=Succeeded(status) && actual_body>=sizeof(X_OBJECT_HEADER) &&
      uint64_t(actual_body)+kNativeMediaEnumHeaderBytes+kNativeMediaQueryBytes<=uint64_t(1)<<32;
  if(cursor.allocation_succeeded)cursor.body=actual_body;
} catch(...) {}
NativeMediaObjectInsertion::NativeMediaObjectInsertion(std::weak_ptr<NativeMediaService> owner,
    KernelState* kernel,uint64_t creation,uint32_t body,uint32_t bytes,
    std::shared_ptr<NativeMediaObjectAdmission> admission,const XThread* insertion_thread,
    const void* insertion_context,uint32_t insertion_sp,uint32_t insertion_lr)
    :owner_(std::move(owner)),kernel_(kernel),creation_(creation),body_(body),bytes_(bytes),
     admission_(std::move(admission)),insertion_thread_(insertion_thread),
     insertion_context_(insertion_context),insertion_sp_(insertion_sp),insertion_lr_(insertion_lr) {}
NativeMediaObjectInsertion::~NativeMediaObjectInsertion()=default;
bool NativeMediaObjectInsertion::AdoptActual(XObject& actual,uint32_t handle,bool actual_new_wrapper) noexcept {
  return xboxkrnl::NativeMediaCountedBodyAccess::Adopt(*this,actual,handle,actual_new_wrapper);
}
bool NativeMediaObjectInsertion::ObserveNoInsertionActual(uint32_t actual_body,
    uint32_t failed_status) noexcept try {
  // Leaf metadata observer inside the actual OpenCounted GCRC. The private
  // capture already proved this exact factory/allocation/context; do not look
  // up the service, lock State, inspect the table, or remint that authority.
  if(!global_critical_region::is_held_by_current_thread() || !XThread::IsInThread() ||
     !kernel_ || actual_body!=body_ || !creation_ || !admission_ || !XFAILED(failed_status) ||
     actual_insert_observed_ || actual_reference_held_ || actual_object_ ||
     actual_outcome_.load(std::memory_order_acquire)!=ActualOutcome::kPending || owner_.expired())return false;
  const auto* actual=XThread::GetCurrentThread();
  if(actual!=insertion_thread_ || actual->kernel_state()!=kernel_ ||
     actual->thread_state()->context()!=insertion_context_)return false;
  const auto* context=actual->thread_state()->context();
  if(context->kernel_state!=kernel_ || Pointer(context->r[1])!=insertion_sp_ ||
     Pointer(context->lr)!=insertion_lr_)return false;
  actual_refusal_status_=failed_status;
  actual_outcome_.store(ActualOutcome::kNeverInserted,std::memory_order_release);
  return true;
} catch(...) {return false;}
struct NativeMediaObjectFree::State {
  std::weak_ptr<NativeMediaService> owner;
  uint64_t serial=0;
  uint32_t body=0;
  bool began=false,completed=false;
};
NativeMediaObjectFree::NativeMediaObjectFree(std::unique_ptr<State> state):state_(std::move(state)) {}
NativeMediaObjectFree::~NativeMediaObjectFree()=default;
void NativeMediaObjectFree::BeforeActualFree() noexcept try {
  if(!state_ || state_->began)return;
  state_->began=true;
  const auto owner=state_->owner.lock();if(!owner)return;
  auto& s=*owner->state_;std::shared_ptr<NativeMediaObjectInsertion> insertion;
  {
    std::lock_guard lock(s.mutex);const auto found=s.cursors.find(state_->serial);
    if(found==s.cursors.end() || found->second->body!=state_->body)return;
    found->second->body_free_entered=true;insertion=found->second->insertion;
  }
  if(insertion && !insertion->NeverInserted() &&
     !xboxkrnl::NativeMediaCountedBodyAccess::WithdrawExactWrapper(*insertion)) {
    std::lock_guard lock(s.mutex);s.uncertain=true;
    if(const auto found=s.cursors.find(state_->serial);found!=s.cursors.end())found->second->uncertain=true;
  }
} catch(...) {}
void NativeMediaObjectFree::Complete(bool executed) noexcept try {
  if(!state_ || !state_->began || state_->completed)return;
  state_->completed=true;
  const auto owner=state_->owner.lock();if(!owner)return;
  auto& s=*owner->state_;std::shared_ptr<NativeMediaObjectInsertion> insertion;
  {
    std::lock_guard lock(s.mutex);const auto found=s.cursors.find(state_->serial);
    if(found==s.cursors.end() || found->second->body!=state_->body)return;
    found->second->body_free_returned=executed;
    if(!executed){found->second->uncertain=true;s.uncertain=true;}
    insertion=found->second->insertion;
  }
  if(executed && insertion && !insertion->NeverInserted())
    xboxkrnl::NativeMediaCountedBodyAccess::DetachKernelAfterBodyGone(*insertion);
} catch(...) {}

std::unique_ptr<NativeMediaObjectCreation> NativeMediaService::CaptureObjectCreation(
    KernelState* kernel,const cpu::ppc::PPCContext* c,uint32_t type,uint32_t bytes,uint32_t output) noexcept try {
  if(!kernel || !c || type!=kNativeMediaEnumType || bytes!=kNativeMediaEnumHeaderBytes+kNativeMediaQueryBytes)return {};
  const auto service=Lookup(kernel);if(!service)return {};
  const auto frame=ActualFrame(*kernel,*c,0x816F0F78);
  if(!frame || frame->lr!=0x816F0FBC || output!=frame->sp+0x50 || !service->state_->RootCurrent(0x816F0F78))return {};
  auto receipt=std::make_unique<NativeMediaObjectCreation::State>();receipt->owner=service;
  {
    auto& s=*service->state_;std::lock_guard lock(s.mutex);
    for(const auto& scope:s.scopes)if(scope.kind==State::ScopeKind::kFactory &&
        scope.frame.thread==frame->thread && scope.frame.pcr==frame->pcr &&
        scope.frame.process==frame->process && scope.frame.sp>=0xB0 && scope.frame.sp-0xB0==frame->sp) {
      if(receipt->serial || scope.cursor->allocation_observed)return {};
      receipt->serial=scope.cursor->serial;
    }
  }
  if(!receipt->serial)return {};
  return std::unique_ptr<NativeMediaObjectCreation>(new NativeMediaObjectCreation(std::move(receipt)));
} catch(...) {return {};}
std::shared_ptr<NativeMediaObjectInsertion> NativeMediaService::CaptureObjectInsertion(
    KernelState* kernel,const cpu::ppc::PPCContext* c,uint32_t body,uint32_t output) noexcept try {
  if(!kernel || !c || !body)return {};
  const auto service=Lookup(kernel);if(!service)return {};
  const auto frame=ActualFrame(*kernel,*c,0x816F0F78);
  if(!frame || frame->lr!=0x816F1008 || !service->state_->RootCurrent(0x816F0F78) || !service->state_->TypeCurrent())return {};
  auto& s=*service->state_;std::lock_guard lock(s.mutex);
  std::shared_ptr<State::Cursor> matched;
  for(const auto& scope:s.scopes)if(scope.kind==State::ScopeKind::kFactory &&
      scope.frame.thread==frame->thread && scope.frame.pcr==frame->pcr &&
      scope.frame.process==frame->process && scope.frame.sp>=0xB0 && scope.frame.sp-0xB0==frame->sp &&
      scope.cursor->allocation_succeeded && scope.cursor->body==body && scope.cursor->scratch==output) {
    if(matched || scope.cursor->insertion)return {};
    matched=scope.cursor;
  }
  if(!matched)return {};
  auto insertion=std::shared_ptr<NativeMediaObjectInsertion>(new NativeMediaObjectInsertion(
      service,kernel,matched->serial,body,kNativeMediaEnumHeaderBytes+kNativeMediaQueryBytes,matched->admission,
      XThread::GetCurrentThread(),c,frame->sp,frame->lr));
  matched->insertion=insertion;return insertion;
} catch(...) {return {};}
std::unique_ptr<NativeMediaObjectFree> NativeMediaService::CaptureObjectFree(
    KernelState* kernel,const cpu::ppc::PPCContext* c,uint32_t body,uint32_t type,
    uint32_t allocation,uint32_t free_procedure) noexcept try {
  if(!kernel || !c || type!=kNativeMediaEnumType || body<sizeof(X_OBJECT_HEADER) ||
      allocation!=body-sizeof(X_OBJECT_HEADER) || free_procedure!=0x816E6B10)return {};
  const auto service=Lookup(kernel);if(!service || !ActualFrame(*kernel,*c,0) || !service->state_->RootCurrent(0x816E6B10))return {};
  auto receipt=std::make_unique<NativeMediaObjectFree::State>();receipt->owner=service;receipt->body=body;
  {auto& s=*service->state_;std::lock_guard lock(s.mutex);
    const auto cursor=s.ByBody(body);
    if(!cursor || !cursor->allocation_succeeded || cursor->body_free_entered)return {};
    receipt->serial=cursor->serial;
  }
  return std::unique_ptr<NativeMediaObjectFree>(new NativeMediaObjectFree(std::move(receipt)));
} catch(...) {return {};}

NativeMediaService::NativeMediaService(std::shared_ptr<State> state):state_(std::move(state)) {}
NativeMediaService::~NativeMediaService()=default;
std::shared_ptr<NativeMediaService> NativeMediaService::Lookup(KernelState* kernel) {
  return NativeSystemUiService::CaptureMediaService(kernel);
}
std::shared_ptr<NativeMediaService> NativeMediaService::CreateBeforeWorkers(Config config,std::string* error) {
  const auto fail=[&](const char* why)->std::shared_ptr<NativeMediaService>{if(error)*error=why;return {};};
  if(!config.catalog || !config.domain || !config.allocation || !config.catalog->calls() ||
     !config.catalog->calls()->kernel() || !config.catalog->calls()->module() ||
     !config.allocation->MatchesDomain(config.domain))return fail("Media actual root/driver/domain prerequisites missing");
  auto state=std::make_shared<State>(std::move(config));
  state->root_code=state->kernel->AcquireUserModuleExecutionLease(state->config.catalog->calls()->module());
  state->resident_domain=state->config.domain->Acquire();
  state->storage=NativeContentStorageDriver::Lookup(state->kernel);
  if(!state->root_code || !state->resident_domain || !state->storage)return fail("Media actual root/driver lifetime unavailable");
  for(const auto& code:NativeMediaCodeProfiles())if(!state->RootCurrent(code.address))
    return fail("Original media closure code/load changed");
  if(!state->RootData(0x815F9220,16) || !state->RootData(kNativeFaDescriptor,0xBC) || !state->TypeCurrent())
    return fail("Actual media builtin/type/data ownership unavailable");
  constexpr std::array<uint32_t,4> builtin{0x815F91BC,kNativeMediaApp,kNativeFaCallback,0};
  for(size_t n=0;n<builtin.size();++n)if(Word(*state->kernel->memory(),0x815F9220+uint32_t(n)*4)!=builtin[n])
    return fail("Original FA builtin identity changed");
  // Observe the already-owned FE initialization later. No FA initializer or
  // app table/globals are written by this component.
  auto result=std::shared_ptr<NativeMediaService>(new NativeMediaService(std::move(state)));
  result->state_->service=result;
  if(error)error->clear();return result;
}
bool NativeMediaService::BindContinuation(std::shared_ptr<NativeSystemTaskContinuation> continuation) {
  if(!continuation)return false;
  std::lock_guard lock(state_->mutex);
  if(!state_->continuation.expired() || state_->retired)return false;
  state_->continuation=std::move(continuation);return true;
}
bool NativeMediaService::ObserveFeReady(XThread& actual,std::string* error) {
  auto& s=*state_;
  const auto fail=[&](const char* why){if(error)*error=why;return false;};
  if(!XThread::IsInThread(&actual) || actual.kernel_state()!=s.kernel ||
     actual.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM || !s.RootCurrent(kNativeFaCallback))
    return fail("FA observation has no actual SYSTEM/bootstrap root");
  if(Word(*s.kernel->memory(),kNativeFaDescriptor+4)!=kNativeMediaApp ||
     Word(*s.kernel->memory(),kNativeFaDescriptor+0xC)!=kNativeFaCallback ||
     Word(*s.kernel->memory(),kNativeFaDescriptor+0x10)!=1)
    return fail("Existing FE did not establish the genuine FA callback");
  s.fe_observed.store(true,std::memory_order_release);
  if(error)error->clear();return true;
}

uint64_t NativeMediaService::GuardEntry(void* raw,uint64_t address) noexcept {
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);
  if(!c || !c->kernel_state || !HooksFunction(uint32_t(address)))return 1;
  bool exact_claim=false;
  try {
    const auto owner=Lookup(c->kernel_state);if(!owner)return 1;
    auto& s=*owner->state_;
    exact_claim=s.Claimed(*c,uint32_t(address));
    const auto frame=ActualFrame(*s.kernel,*c,uint32_t(address));
    if(!frame || !s.RootCurrent(uint32_t(address)) || (exact_claim && s.uncertain)) {
      if(!exact_claim)return 1;
      s.uncertain=true;c->r[3]=uint64_t(int64_t(int32_t(kNotReady)));return 0;
    }
    if(address==0x8187AE98){s.EnterAttachment(*c,*frame);return 1;}
    if(address==0x818621F0){s.EnterToken(*c,*frame);return 1;}
    bool claimed=false,admitted=true;
    if(address==0x81706478) {
      admitted=s.AdmitBackend(*c,*frame,claimed);
      if(claimed && !admitted){c->r[3]=uint64_t(int64_t(int32_t(kNotReady)));return 0;}
      return 1;
    }
    if(address==0x816F0F78) {
      std::array<uint32_t,8> args{};bool valid=true;
      for(size_t n=0;n<args.size();++n) {
        const auto value=Pointer(c->r[n+3]);if(!value){valid=false;break;}args[n]=*value;
      }
      if(valid && NativeMediaCreateArgumentsValid(args) && s.fe_observed.load()) {
        // Every exact FA factory is provisional. No source, initialized query,
        // rows or activation are inferred from this real object allocation.
        claimed=true;admitted=s.EnterFactory(*c,*frame);
      }
    } else if(address==0x816A2F88 || address==0x816A2CF0 || address==0x816A2FF8) {
      admitted=s.EnterMessage(*c,*frame,claimed);
    } else {
      std::shared_ptr<State::Cursor> cursor;
      {std::lock_guard lock(s.mutex);
        cursor=address==0x816F1200?s.ByBody(uint32_t(c->r[3])):s.ByHandle(uint32_t(c->r[3]));
        // Cursor classification shares the same mutex as selected promotion,
        // ordinary retirement and BeginStop. The Enter methods revalidate the
        // actual admission after any intervening backing/provider work.
        if(cursor)claimed=address==0x816F1040?
            cursor->selected || !cursor->retire_ordinary:cursor->selected;
      }
      if(!cursor)return 1;
      if(address==0x816F1040) {
        if(claimed)admitted=s.EnterPrivate(*c,*frame,cursor);
      } else if(address==0x816F1070) {
        if(claimed)admitted=s.EnterRead(*c,*frame,cursor);
      } else if(address==0x816F1200) {
        if(claimed)admitted=s.EnterClose(*c,*frame,cursor);
      }
    }
    if(claimed && !admitted) {
      c->r[3]=uint64_t(int64_t(int32_t(kNotReady)));return 0;
    }
    return 1;
  } catch(...) {
    if(const auto owner=Lookup(c->kernel_state))owner->state_->uncertain=true;
    if(!exact_claim)return 1;
    c->r[3]=uint64_t(int64_t(int32_t(kAbort)));return 0;
  }
}
void NativeMediaService::ObserveReturn(void* raw,uint64_t address) noexcept {
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);
  if(!c || !c->kernel_state)return;
  try {
    const auto owner=Lookup(c->kernel_state);if(!owner)return;
    auto& s=*owner->state_;const auto frame=ActualFrame(*s.kernel,*c,uint32_t(address));
    if(!frame)return;
    if(address==0x8187AE98)s.ReturnAttachment(*c,*frame);
    else if(address==0x818621F0)s.ReturnToken(*c,*frame);
    else if(!s.ReturnDirect(*c,*frame))s.ReturnScope(*c,*frame);
  } catch(...) {if(const auto owner=Lookup(c->kernel_state))owner->state_->uncertain=true;}
}
void NativeMediaService::BeforeCompletionPublication(void*) noexcept {
  // No selected asynchronous request is admitted by this finite cohort.
  // No Status write/APC completion is treated as native worker drainage.
}
std::optional<uint32_t> NativeMediaService::TryEnumerationOnOriginalCaller(
    KernelState* kernel,uint16_t ordinal,std::span<const uint32_t> args) noexcept try {
  if(!kernel)return {};
  const auto owner=Lookup(kernel);
  if(ordinal==0x24E) {
    if(!NativeMediaCreateArgumentsValid(args))return {};
    if(!owner || !owner->state_->FaCurrent())return kNotReady;
  } else {
    if((ordinal!=0x24F && ordinal!=0x250) || args.empty() || !owner)return {};
    std::lock_guard lock(owner->state_->mutex);
    const auto cursor=owner->state_->ByHandle(args[0]);
    if(!cursor)return {};
  }
  const auto& profile=NativeMediaOriginal(ordinal==0x24E?NativeMediaOriginalCall::kCreate:
      ordinal==0x24F?NativeMediaOriginalCall::kGetPrivate:NativeMediaOriginalCall::kEnumerate);
  if(args.size()!=profile.arguments)return kNotReady;
  if(!XThread::IsInThread())return kNotReady;
  auto* caller=XThread::GetCurrentThread();
  if(!caller || !XThread::IsInThread(caller) || caller->kernel_state()!=kernel ||
      global_critical_region::is_held_by_current_thread())return kNotReady;
  const auto result=owner->state_->Call(*caller,profile.address,args);
  return result.executed?uint32_t(result.value):kAbort;
} catch(...) {return kAbort;}
std::optional<uint32_t> NativeMediaService::TryMessageOnOriginalCaller(
    KernelState* kernel,uint16_t ordinal,std::span<const uint32_t> args) noexcept try {
  if(!kernel || args.size()<4 || args[0]!=kNativeMediaApp)return {};
  const bool direct=args[1]==kNativeMediaSourceMetadata || args[1]==kNativeMediaSourceStream;
  if(!direct && args[1]!=kNativeMediaQueryInit && args[1]!=kNativeMediaQueryRead && args[1]!=kNativeMediaQueryClose)return {};
  const auto owner=Lookup(kernel);if(!owner)return {};
  auto& s=*owner->state_;
  if(direct) {
    if(!XThread::IsInThread() || global_critical_region::is_held_by_current_thread())return kNotReady;
    auto* direct_caller=XThread::GetCurrentThread();
    if(!direct_caller || direct_caller->kernel_state()!=kernel)return kNotReady;
    const size_t input=(ordinal==0x1F4 || ordinal==0x1F6)?2:3;
    if(input>=args.size())return kNotReady;
    bool claimed=args[1]==kNativeMediaSourceMetadata;
    if(!claimed) {
      std::array<uint32_t,5> words{};std::array<uint8_t,kNativeMediaSourceBytes> token{};
      if(ReadDirectWords(*kernel->memory(),args[input],words) && words[1] &&
          XSUCCEEDED(util::ReadGuestIoBuffer(*kernel->memory(),words[1],token))) {
        std::lock_guard lock(s.mutex);claimed=bool(s.TokenAttachment(token));
      }
    }
    if(!claimed)return {};
    // Direct1F4 is truly synchronous on TITLE too. The separate ProcessRequest
    // rule and original async Task/APC ownership are not inferred or bypassed.
    if(!NativeMediaDirectOriginalArgumentsValid(ordinal,args) || !XThread::IsInThread() ||
        global_critical_region::is_held_by_current_thread())return kNotReady;
    auto* caller=XThread::GetCurrentThread();
    if(!caller || caller->kernel_state()!=kernel)return kNotReady;
    const auto result=s.Call(*caller,0x816A2F88,args);
    return result.executed?uint32_t(result.value):kAbort;
  }
  const bool synchronous=ordinal==0x1F4;
  const uint32_t payload=args[(synchronous || ordinal==0x1F6)?2:3];
  std::array<uint32_t,8> request{};uint32_t workspace=payload;
  if(args[1]!=kNativeMediaQueryClose) {
    if(!ReadWords(*kernel->memory(),payload,request))return {};
    workspace=request[args[1]==kNativeMediaQueryInit?7:2];
  }
  bool claimed=false;
  std::array<uint8_t,kNativeMediaSourceBytes> token{};
  if(args[1]==kNativeMediaQueryInit && bool(request[1])!=bool(request[2]) &&
      internal::ReadNativeWaitSnapshot(*kernel->memory(),request[1]?request[1]:request[2],token)) {
    std::lock_guard lock(s.mutex);claimed=bool(s.TokenAttachment(token));
  }
  {std::lock_guard lock(s.mutex);const auto cursor=s.ByWorkspace(workspace);claimed|=cursor && cursor->selected;}
  if(!claimed)return {};
  // Deliberate temporary gate blocker: original TITLE/async ownership will
  // be integrated using the already-authenticated FA wrapper, not fabricated.
  if(!synchronous || args.size()!=4)return kNotReady;
  if(!XThread::IsInThread())return kNotReady;
  auto* caller=XThread::GetCurrentThread();
  if(!caller || !XThread::IsInThread(caller) || caller->kernel_state()!=kernel ||
      global_critical_region::is_held_by_current_thread())return kNotReady;
  const auto result=s.Call(*caller,0x816A2F88,args);
  return result.executed?uint32_t(result.value):kAbort;
} catch(...) {return kAbort;}

bool NativeMediaService::State::CleanupOnTask(XThread& actual,const std::shared_ptr<Cursor>& cursor) {
  if(!XThread::IsInThread(&actual) || actual.kernel_state()!=kernel ||
     actual.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
     !CurrentTask(actual).value_or(0) || !FaCurrent())return false;
  if(cursor->insertion && cursor->insertion->NeverInserted()) {
    std::lock_guard lock(mutex);
    if(!cursor->factory_returned || cursor->selected || cursor->uncertain)return false;
    // A normal explicit pre-insertion refusal acquired no wrapper/reference.
    // Real free remains a separate fact. If it was not observed, retain this
    // exact allocation's owner until actual Kernel stop, never fake Close/free.
    if(cursor->allocation_succeeded && !cursor->body_free_returned)
      cursor->deferred_until_stopped=true;
    return true;
  }
  if(!cursor->insertion) {
    // A genuine failed factory can free its allocation before insertion.
    return cursor->factory_returned && (!cursor->allocation_succeeded || cursor->body_free_returned);
  }
  if(!cursor->selected) {
    // The temporary creator reference is observable; never-selected/failed or
    // unrelated provisional factories relinquish it on this actual Task.
    // Their existing aliases, native close and ordinary query remain native.
    if(!cursor->delivered && !xboxkrnl::NativeMediaCountedBodyAccess::CloseActualHandles(*cursor->insertion,actual))return false;
    return xboxkrnl::NativeMediaCountedBodyAccess::RelinquishOrdinary(*cursor->insertion,actual);
  }
  if(!xboxkrnl::NativeMediaCountedBodyAccess::CloseActualHandles(*cursor->insertion,actual))return false;
  {
    std::lock_guard lock(mutex);
    if(cursor->active || !cursor->admission->handles_closed_.load())return false;
    cursor->close_message_entered=false;cursor->close_message_returned=false;
    cursor->close_returned=false;
  }
  const auto caller=ActualFrame(*kernel,*actual.thread_state()->context(),0);
  const auto task=CurrentTask(actual).value_or(0);
  if(!caller || !task)return false;
  {std::lock_guard lock(mutex);cursor->cleanup_caller=caller;cursor->cleanup_task=task;}
  const std::array<uint32_t,2> args{cursor->body,0};
  const auto close=Call(actual,0x816F1200,args);
  bool native_closed=false;
  {
    std::lock_guard lock(mutex);
    cursor->cleanup_caller.reset();cursor->cleanup_task=0;
    if(!close.executed || !cursor->close_returned){cursor->uncertain=true;uncertain=true;return false;}
    native_closed=cursor->close_message_entered && cursor->close_message_returned && Succeeded(cursor->close_result);
    if(!native_closed)cursor->deferred_until_stopped=true;
  }
  if(!native_closed) {
    // Real normally-returned failure (including source unavailable800704B1)
    // is retained, NOT promoted to successful7001E. It does not prevent the
    // existing service from joining CPUs and reaching actual stopped rundown.
    return true;
  }
  if(!xboxkrnl::NativeMediaCountedBodyAccess::Release(*cursor->insertion,actual))return false;
  std::shared_ptr<xam::SelectedMediaUse> use;
  std::shared_ptr<const Attachment> attachment;
  {std::lock_guard lock(mutex);use=std::move(cursor->source_use);attachment=std::move(cursor->selected_attachment);}
  return true;
}
bool NativeMediaService::FactoryPublicationCurrent(uint64_t serial,uint32_t handle,const Memory& memory) const noexcept {
  const auto& s=*state_;
  if(s.retired || s.uncertain || s.ingress_closed || !s.kernel || s.kernel->memory()!=&memory)return false;
  for(const auto& direct:s.direct_calls)if(direct->serial==serial) {
    if(direct->message!=kNativeMediaSourceStream || !direct->publication_ready ||
        direct->delivered || direct->uncertain || !direct->backend_returned ||
        !direct->backing || !direct->backing->current() || direct->stream!=handle)return false;
    if(!handle)return !direct->file || direct->file->file()->retained_device()==direct->device;
    const auto actual=s.kernel->object_table()->LookupObject<XFile>(direct->file_handle,true);
    return direct->file && actual.get()==direct->file.get() &&
        direct->file->file()->retained_device()==direct->device;
  }
  const auto found=s.cursors.find(serial);
  if(found==s.cursors.end())return false;
  const auto& c=*found->second;
  return !c.factory_returned && !c.close_requested && c.insertion &&
      c.insertion->actual_insert_observed_ && c.insertion->actual_reference_held_ &&
      c.insertion->actual_handle_==handle && c.admission && c.admission->serial_==serial &&
      c.admission->kernel_==s.kernel && c.admission->publication_handle_==handle &&
      c.admission->publication_live_.load(std::memory_order_acquire) &&
      !c.admission->handles_closed_.load(std::memory_order_acquire) &&
      !c.admission->body_detached_.load(std::memory_order_acquire) &&
      !c.admission->issuer_.expired() && c.output_backing && c.output_backing->current();
}
void NativeMediaService::FactoryPublicationCommitted(uint64_t serial,uint32_t value) noexcept {
  for(const auto& direct:state_->direct_calls)if(direct->serial==serial && direct->stream==value)
    direct->delivered=true;
}
void util::GuestIoNativeMediaFactoryScope::Release() noexcept {
  if(lock_.owns_lock()) {
    // WriteBatch copied every byte before this callback-free metadata commit.
    owner_->FactoryPublicationCommitted(serial_,handle_);lock_.unlock();
  }
}
X_STATUS util::GuestIoNativeMediaFactoryScope::Admit() {
  if(!owner_)return X_STATUS_CANCELLED;
  lock_=std::unique_lock(owner_->state_->mutex,std::try_to_lock);
  if(!lock_.owns_lock())return X_STATUS_CANCELLED;
  if(!owner_->FactoryPublicationCurrent(serial_,handle_,memory_)) {
    lock_.unlock();return X_STATUS_CANCELLED;
  }
  return X_STATUS_SUCCESS;
}
bool NativeMediaService::OwnsScheduledWork(uint64_t serial) const noexcept try {
  const auto& s=*state_;std::lock_guard lock(s.mutex);
  const auto cursor=s.cursors.find(serial);
  return !s.retired && !s.uncertain && s.scheduled_cleanup==serial && cursor!=s.cursors.end() &&
      cursor->second->cleanup_running && !cursor->second->active && !cursor->second->uncertain;
} catch(...) {return false;}
GuestRuntime::ExecutionResult NativeSystemTaskContinuation::InvokeOwnedMediaAndWait(
    NativeMediaService& owner,uint64_t serial,Work work) {
  if(!XThread::IsInThread())return {false,0,"Media continuation requires actual guest bootstrap"};
  const auto retained=owner.shared_from_this();
  return InvokeAndWaitImpl(*XThread::GetCurrentThread(),std::move(work),Admission::Role::kOwnedMedia,nullptr,
      [retained,serial]{return retained->OwnsScheduledWork(serial);});
}
bool NativeMediaService::StepOnBootstrap(XThread& actual) noexcept try {
  auto& s=*state_;
  if(!XThread::IsInThread(&actual) || actual.kernel_state()!=s.kernel ||
     actual.native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM)return false;
  std::shared_ptr<State::Cursor> cursor;
  {
    std::lock_guard lock(s.mutex);
    if(s.retired || s.uncertain || s.scheduled_cleanup)return !s.uncertain;
    for(const auto& [serial,entry]:s.cursors) {
      if(!entry->factory_returned || entry->active || entry->cleanup_running || entry->uncertain || entry->deferred_until_stopped)continue;
      if(entry->selected && entry->selected_attachment &&
         !entry->selected_attachment->processing.source->SourceCurrentCached())entry->close_requested=true;
      const bool ordinary_due=!entry->selected && (entry->retire_ordinary || s.ingress_closed ||
          !entry->factory_succeeded || (entry->insertion &&
            xboxkrnl::NativeMediaCountedBodyAccess::ProvisionalHasNoGuestOwner(*entry->insertion)));
      const bool selected_due=entry->selected && (entry->close_requested || entry->admission->handles_closed_.load());
      if(!ordinary_due && !selected_due)continue;
      if(ordinary_due)entry->retire_ordinary=true;
      if(selected_due) {
        entry->close_requested=true;
        entry->admission->ingress_closed_.store(true,std::memory_order_release);
      }
      entry->cleanup_running=true;s.scheduled_cleanup=serial;cursor=entry;break;
    }
  }
  if(!cursor)return true;
  const auto continuation=s.continuation.lock();
  if(!continuation) {std::lock_guard lock(s.mutex);cursor->uncertain=true;s.uncertain=true;return false;}
  const auto owner=shared_from_this();
  const auto result=continuation->InvokeOwnedMediaAndWait(*this,cursor->serial,
      [owner,cursor](XThread& task) {
        const bool healthy=owner->state_->CleanupOnTask(task,cursor);
        return GuestRuntime::ExecutionResult{healthy,healthy?0u:1u,"Owned media native cleanup"};
      });
  std::shared_ptr<State::Cursor> released;
  {
    std::lock_guard lock(s.mutex);cursor->cleanup_running=false;s.scheduled_cleanup.reset();
    if(!result.succeeded){cursor->uncertain=true;s.uncertain=true;return false;}
    if((!cursor->selected && !cursor->deferred_until_stopped) || cursor->body_free_returned) {
      const auto found=s.cursors.find(cursor->serial);
      if(found!=s.cursors.end() && found->second==cursor){released=std::move(found->second);s.cursors.erase(found);}
    } else if(cursor->close_returned && Succeeded(cursor->close_result)) {
      // Native query is closed, but borrowed guest24F pointers may keep the
      // body alive. Preserve only its exact mapping receipt until actual free
      // or stopped Kernel; do not repeatedly execute native Close.
      cursor->deferred_until_stopped=true;
    }
  }
  return true;
} catch(...) {state_->uncertain=true;return false;}
X_STATUS NativeMediaService::ValidateDirectFileOpen(KernelState* kernel,
    const vfs::File& file,uint32_t output) noexcept try {
  if(global_critical_region::is_held_by_current_thread())return X_STATUS_SUCCESS;
  const auto owner=Lookup(kernel);if(!owner)return X_STATUS_SUCCESS;
  auto& s=*owner->state_;const auto frame=s.DirectFileFrame(output);
  if(!frame)return X_STATUS_SUCCESS;
  if(!s.RootCurrent(0x818610E8))return X_STATUS_ACCESS_DENIED;
  std::lock_guard lock(s.mutex);const auto call=s.DirectFileCaller(*frame);
  if(!call)return X_STATUS_SUCCESS;
  if(call->uncertain || call->pending_file || call->file || !call->device ||
      file.retained_device()!=call->device || !file.retained_device()->is_read_only())return X_STATUS_ACCESS_DENIED;
  call->pending_file=const_cast<vfs::File*>(&file);return X_STATUS_SUCCESS;
} catch(...) {return X_STATUS_ACCESS_DENIED;}
void NativeMediaService::ObserveDirectFileOpen(KernelState* kernel,
    const object_ref<XFile>& file,uint32_t output) noexcept try {
  if(global_critical_region::is_held_by_current_thread())return;
  const auto owner=Lookup(kernel);if(!owner)return;
  auto& s=*owner->state_;const auto frame=s.DirectFileFrame(output);if(!frame)return;
  std::lock_guard lock(s.mutex);const auto call=s.DirectFileCaller(*frame);if(!call)return;
  if(!file || call->file || call->pending_file!=file->file() ||
      file->file()->retained_device()!=call->device) {
    call->uncertain=true;s.uncertain=true;return;
  }
  call->file=file;call->file_handle=file->handle();
} catch(...) {if(const auto owner=Lookup(kernel))owner->state_->uncertain=true;}
void NativeMediaService::BeginStop() noexcept {
  try {std::lock_guard lock(state_->mutex);
    state_->ingress_closed.store(true,std::memory_order_release);
    for(const auto& [serial,cursor]:state_->cursors) {
      cursor->close_requested|=cursor->selected;
      if(cursor->selected)cursor->admission->ingress_closed_.store(true,std::memory_order_release);
    }
  } catch(...) {state_->ingress_closed=true;state_->uncertain=true;}
}
bool NativeMediaService::StopOnBootstrap(XThread& actual) noexcept {
  BeginStop();
  // Every iteration completes one actual ownedTask. No spin while holding
  // locks, no fake close, and no wait for a stopped boundary that we prevent.
  for(size_t n=0;n<256;++n) {
    if(Drained())return true;
    if(!StepOnBootstrap(actual))return false;
  }
  return Drained();
}
bool NativeMediaService::Drained() const noexcept try {
  const auto& s=*state_;std::lock_guard lock(s.mutex);
  if(s.uncertain || s.scheduled_cleanup || !s.scopes.empty() || !s.direct_calls.empty() || !s.attachment_calls.empty() || !s.token_calls.empty())return false;
  for(const auto& [serial,cursor]:s.cursors) {
    if(cursor->active || cursor->cleanup_running || cursor->uncertain || !cursor->factory_returned)return false;
    if(!cursor->selected && !cursor->deferred_until_stopped)return false; // temporary ref still needs a Task
    if(!cursor->deferred_until_stopped && !cursor->body_free_returned)return false;
  }
  // Execution quiescence ONLY. deferred_until_stopped may still retain a real
  // pointer reference/source use after a genuine failed Close. It is not a
  // native close/free receipt, and post-CPU-stop retirement remains required.
  return true;
} catch(...) {return false;}
bool NativeMediaService::RetireAfterActualKernelStop() noexcept try {
  if(!Drained() || global_critical_region::is_held_by_current_thread())return false;
  auto& s=*state_;std::map<uint64_t,std::shared_ptr<State::Cursor>> snapshot,retired;
  {
    std::lock_guard lock(s.mutex);if(s.retired)return true;
    s.ingress_closed=true;snapshot=s.cursors;
  }
  for(const auto& [serial,cursor]:snapshot)
    if(cursor->insertion && !xboxkrnl::NativeMediaCountedBodyAccess::RetireAfterStoppedKernel(*cursor->insertion))return false;
  {
    std::lock_guard lock(s.mutex);s.retired=true;retired.swap(s.cursors);
    s.tokens.clear();s.attachments.clear();
  }
  for(const auto& [serial,cursor]:retired) {
    // Guest ownership is discarded with the actually stopped Kernel memory;
    // no Dereference/PPC, synthetic7001E or false native-free claim occurs.
    cursor->source_use.reset();cursor->selected_attachment.reset();cursor->output_backing.reset();
    if(cursor->scratch){s.kernel->memory()->SystemHeapFree(cursor->scratch);cursor->scratch=0;}
  }
  s.storage.reset();s.root_code.reset();s.resident_domain={};s.config={};s.kernel=nullptr;
  return true;
} catch(...) {return false;}
} // namespace xe::kernel
