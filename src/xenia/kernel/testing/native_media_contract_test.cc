/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_media_contract.h"
#include "xenia/kernel/native_media_service.h"
#include "xenia/kernel/native_media_object_admission.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include <cstring>
#include <algorithm>
#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/xam/selected_media_source_proof.h"
#include "xenia/kernel/native_content_storage_source.h"
#include "xenia/base/host_storage.h"
#include "xenia/base/media_selection.h"
#include "xenia/base/mutex.h"
#include "xenia/base/byte_order.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/memory.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/file.h"
#include "third_party/catch/include/catch.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <future>
#include <initializer_list>
#include <thread>

namespace xe::kernel::test {
class NativeMediaDeniedAliasTestAccess {
 public:
  static auto CountedBirth(util::ObjectTable& table) { return table.CaptureCountedBodyBirth(); }
  static bool CurrentCountedBirth(const util::ObjectTable& table,
      const std::weak_ptr<const uint8_t>& birth) { return table.CurrentCountedBodyBirth(birth); }
  static std::unique_ptr<util::GuestIoNativeMediaFactoryScope> DeniedPublication(Memory& memory) {
    // No real owner is fabricated. This is a deny-only publication scope.
    return std::unique_ptr<util::GuestIoNativeMediaFactoryScope>(
        new util::GuestIoNativeMediaFactoryScope({},0,0,memory));
  }
  static std::shared_ptr<const uint8_t> ProvisionalMetadataOnly(XObject& object) {
    auto issuer=std::make_shared<const uint8_t>(0);
    auto admission=std::shared_ptr<NativeMediaObjectAdmission>(
        new NativeMediaObjectAdmission(issuer,object.kernel_state(),1));
    auto global=global_critical_region::AcquireDirect();
    object.native_media_admission_=std::move(admission);
    return issuer;
  }
  static void SelectAliasDenialOnly(XObject& object) {
    auto global=global_critical_region::AcquireDirect();
    object.native_media_admission_->selected_.store(true);
  }
  static void DenyOnly(XObject& object) {
    // This test seam can only close real handle publication. It never creates
    // a native body, guest pointer reference, initialized query or ready owner.
    auto issuer=std::make_shared<const uint8_t>(0);
    auto admission=std::shared_ptr<NativeMediaObjectAdmission>(
        new NativeMediaObjectAdmission(issuer,object.kernel_state(),1));
    admission->ingress_closed_.store(true);
    auto global=global_critical_region::AcquireDirect();
    object.native_media_admission_=std::move(admission);
  }
};
}
namespace xe::kernel::xam::test {
// Tests can exercise ONLY the actual private nonwaiting use admission with a
// proof issued by the real registry snapshot. It cannot mint/replace a proof,
// native receipt, source result, pointer ref, Task or native completion.
class SelectedMediaNativeAdmissionAccess {
 public:
  static auto Capture(ContentSourceRegistry& registry,
      const std::shared_ptr<const SelectedMediaSourceProof>& proof,std::error_code& error) {
    return registry.CaptureSelectedMediaUse(proof,error);
  }
};
namespace {
struct MediaScratch {
  std::filesystem::path path;
  MediaScratch() {
    static std::atomic<uint64_t> next{0};
    path=std::filesystem::temp_directory_path()/("xenia-native-media-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+
        std::to_string(next.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~MediaScratch(){std::error_code error;std::filesystem::remove_all(path,error);}
};
ContentSource Source(std::string id="selected",uint32_t device=7) {
  return {std::move(id),device,DeviceType::USBMASS,ContentSourceKind::kSelectedMedia,
      {},"",u"Media",true,0,"\\Device\\Mass"+std::to_string(device)};
}
std::shared_ptr<const SelectedMediaSourceProof> Proof(ContentSourceRegistry& registry) {
  auto connection=registry.storage_notifications()->Connect([](auto){});
  REQUIRE(connection);
  REQUIRE(connection->initial.devices.size()==1);
  const auto& device=connection->initial.devices[0];
  REQUIRE(device.available);REQUIRE(device.type==DeviceType::USBMASS);
  REQUIRE_FALSE(device.managed_format_proof);
  auto proof=device.selected_media_proof;REQUIRE(proof);
  connection->subscription->BeginStop();REQUIRE(connection->subscription->Drain());
  return proof;
}
struct Selection {
  std::shared_ptr<filesystem::HostStorageRoot> root;
  std::shared_ptr<filesystem::MediaSelection> media;
  Selection(const std::filesystem::path& path) {
    std::error_code error;root=filesystem::HostStorageRoot::Open(path,true,error,0);
    REQUIRE(root);REQUIRE_FALSE(error);
    media=filesystem::MediaSelection::CaptureFiles({{"Music/song.mp3",root,"song"}},error);
    REQUIRE(media);REQUIRE_FALSE(error);
  }
};
}
TEST_CASE("Native media factory selects the exact real ABI and leaves other app enums alone",
          "[kernel][native_media_owner][abi]") {
  const std::array<uint32_t,8> original{0,kNativeMediaApp,kNativeMediaQueryRead,
      kNativeMediaQueryClose,kNativeMediaQueryBytes,13,0x80000001,0x40001000};
  REQUIRE(NativeMediaCreateArgumentsValid(original));
  REQUIRE_FALSE(NativeMediaCreateArgumentsValid(std::span(original).first(7)));
  for(size_t index:{size_t(1),size_t(2),size_t(3),size_t(4),size_t(7)}) {
    auto changed=original;changed[index]=index==7?0:changed[index]+1;
    REQUIRE_FALSE(NativeMediaCreateArgumentsValid(changed));
  }
  // Item count/user/flags remain genuine native inputs, not invented host
  // media filters or a parallel header/row serializer.
  for(size_t index:{size_t(0),size_t(5),size_t(6)}) {
    auto changed=original;changed[index]=0xFFFFFFFF;
    REQUIRE(NativeMediaCreateArgumentsValid(changed));
  }
  const auto& create=NativeMediaOriginal(NativeMediaOriginalCall::kCreate);
  REQUIRE(create.address==0x816F0F78);REQUIRE(create.arguments==8);
  REQUIRE(NativeMediaOriginal(NativeMediaOriginalCall::kGetPrivate).arguments==2);
  REQUIRE(NativeMediaOriginal(NativeMediaOriginalCall::kEnumerate).arguments==6);
  REQUIRE(NativeMediaOriginal(NativeMediaOriginalCall::kCloseProcedure).returns_void);
  REQUIRE_FALSE(NativeMediaCode(0x817002E8)); // bounded interior is not a callable entry
  const auto* free_tail=NativeMediaCode(0x816E6B10);REQUIRE(free_tail);
  REQUIRE(free_tail->bytes==4); // actual free tail, not neighboring body
  const auto* ctor=NativeMediaCode(0x81703410);REQUIRE(ctor);REQUIRE(ctor->bytes==328);
  const auto* wrapper=NativeMediaCode(0x81706738);REQUIRE(wrapper);REQUIRE(wrapper->bytes==120);
}
#if !XE_PLATFORM_WIN32
TEST_CASE("Native media operation uses require the exact live issuer and retain no path authority",
          "[kernel][native_media_owner][selected_media]") {
  MediaScratch scratch;std::ofstream(scratch.path/"song")<<"original";
  Selection selected(scratch.path);ContentSourceRegistry registry;vfs::VirtualFileSystem fs;
  REQUIRE(registry.RegisterSelectedMedia(Source(),selected.media,1,fs));
  const auto proof=Proof(registry);REQUIRE(proof->source_id()=="selected");REQUIRE(proof->generation()==1);
  const auto source=registry.Source("selected");REQUIRE(source);REQUIRE(source->available);
  REQUIRE_FALSE(source->storage);REQUIRE(source->source.root.empty());
  REQUIRE_FALSE(source->profile_writable);REQUIRE_FALSE(source->format_eligible);
  REQUIRE_FALSE(registry.Resolve("selected","Music/song.mp3",false));
  bool matched=false;std::error_code error;
  REQUIRE_FALSE(registry.CaptureSourceDirectory(scratch.path,true,matched,error));
  REQUIRE(matched);REQUIRE(error==std::errc::permission_denied);
  std::shared_ptr<SelectedMediaUse> use;
  {
    auto global=global_critical_region::AcquireDirect();
    use=SelectedMediaNativeAdmissionAccess::Capture(registry,proof,error);
    REQUIRE(use);REQUIRE_FALSE(error);REQUIRE(use->current());
  } // No filesystem observation or root capability was substituted under GCRC.
  Selection separate(scratch.path);ContentSourceRegistry other;vfs::VirtualFileSystem other_fs;
  REQUIRE(other.RegisterSelectedMedia(Source(),separate.media,1,other_fs));
  const auto other_proof=Proof(other);
  REQUIRE_FALSE(SelectedMediaNativeAdmissionAccess::Capture(other,proof,error));
  REQUIRE(error==std::errc::no_such_device);
  REQUIRE_FALSE(SelectedMediaNativeAdmissionAccess::Capture(registry,other_proof,error));
  REQUIRE(error==std::errc::no_such_device);
  REQUIRE(use->current());REQUIRE(registry.Source("selected")->available);
  other_fs.Clear();fs.Clear();
}
TEST_CASE("Controlled media withdrawal retains the actual backend until the exact use drains",
          "[kernel][native_media_owner][selected_media][lifetime]") {
  MediaScratch scratch;std::ofstream(scratch.path/"song")<<"kept";
  Selection selected(scratch.path);ContentSourceRegistry registry;vfs::VirtualFileSystem fs;
  REQUIRE(registry.RegisterSelectedMedia(Source(),selected.media,1,fs));
  const auto proof=Proof(registry);const auto old_device=fs.AcquireDevice(Source().guest_volume_root);
  REQUIRE(old_device);std::error_code error;
  auto use=SelectedMediaNativeAdmissionAccess::Capture(registry,proof,error);REQUIRE(use);REQUIRE_FALSE(error);
  REQUIRE(selected.root->open_file_count()==1);
  REQUIRE(registry.WithdrawSelectedMedia("selected",1));
  REQUIRE_FALSE(use->current());REQUIRE_FALSE(registry.Source("selected")->available);
  REQUIRE_FALSE(SelectedMediaNativeAdmissionAccess::Capture(registry,proof,error));
  REQUIRE(error==std::errc::no_such_device);
  // A use is not native Close proof. It only retains the genuine old physical
  // attachment during controlled drainage; new operations are already shut.
  REQUIRE(selected.root->open_file_count()==1);
  auto physical=registry.storage_notifications()->Connect([](auto){});REQUIRE(physical);
  REQUIRE(physical->initial.devices.size()==1);REQUIRE(physical->initial.devices[0].available);
  REQUIRE(physical->initial.devices[0].generation==1);
  physical->subscription->BeginStop();REQUIRE(physical->subscription->Drain());
  Selection replacement(scratch.path);
  REQUIRE_FALSE(registry.RegisterSelectedMedia(Source(),replacement.media,2,fs));
  REQUIRE(fs.AcquireDevice(Source().guest_volume_root)==old_device);
  use.reset();(void)registry.Source("selected");
  REQUIRE(selected.root->open_file_count()==0);
  REQUIRE(registry.RegisterSelectedMedia(Source(),replacement.media,2,fs));
  const auto fresh=Proof(registry);REQUIRE(fresh!=proof);REQUIRE(fresh->generation()==2);
  REQUIRE_FALSE(SelectedMediaNativeAdmissionAccess::Capture(registry,proof,error));
  REQUIRE(error==std::errc::no_such_device);
  auto current=SelectedMediaNativeAdmissionAccess::Capture(registry,fresh,error);REQUIRE(current);REQUIRE_FALSE(error);
  REQUIRE_FALSE(registry.WithdrawSelectedMedia("selected",1));
  REQUIRE(current->current());REQUIRE(fs.AcquireDevice(Source().guest_volume_root)!=old_device);
  fs.Clear();
}
TEST_CASE("Actual selected Device retirement closes only its exact cached operation admission",
          "[kernel][native_media_owner][selected_media][lifetime]") {
  MediaScratch scratch;std::ofstream(scratch.path/"song")<<"bytes";
  Selection selected(scratch.path);ContentSourceRegistry registry;vfs::VirtualFileSystem fs;
  REQUIRE(registry.RegisterSelectedMedia(Source(),selected.media,1,fs));const auto proof=Proof(registry);
  std::error_code error;auto use=SelectedMediaNativeAdmissionAccess::Capture(registry,proof,error);REQUIRE(use);
  const auto old=fs.AcquireDevice(Source().guest_volume_root);REQUIRE(old);
  REQUIRE(fs.UnregisterDevice(Source().guest_volume_root));
  REQUIRE(old->namespace_retiring());REQUIRE_FALSE(use->current());
  REQUIRE_FALSE(SelectedMediaNativeAdmissionAccess::Capture(registry,proof,error));
  REQUIRE(error==std::errc::no_such_device);
  Selection replacement(scratch.path);
  REQUIRE_FALSE(registry.RegisterSelectedMedia(Source(),replacement.media,2,fs));
  use.reset();REQUIRE(registry.RegisterSelectedMedia(Source(),replacement.media,2,fs));
  const auto fresh=Proof(registry);auto active=SelectedMediaNativeAdmissionAccess::Capture(registry,fresh,error);
  REQUIRE(active);REQUIRE_FALSE(error);REQUIRE(active->current());
  old->Retire(); // A stale exact device cannot close the replacement's atomic.
  REQUIRE(active->current());fs.Clear();
}
#endif
TEST_CASE("Missing native media owner refuses its exact HLE factory without publishing a handle",
          "[kernel][native_media_owner][abi][lifetime]") {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;MediaScratch scratch;
  REQUIRE(std::filesystem::create_directory(scratch.path/"content"));
  REQUIRE(std::filesystem::create_directory(scratch.path/"cache"));
  auto emulator=std::make_unique<Emulator>("",scratch.path,scratch.path/"content",scratch.path/"cache");
  REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
  auto& memory=*emulator->memory();const auto page=memory.SystemHeapAlloc(0x1000,0x1000);REQUIRE(page);
  memory.Fill(page,0x1000,0xA7);
  const auto entry=emulator->export_resolver()->GetExportByOrdinal("xam.xex",0x24E);
  REQUIRE(entry);REQUIRE(entry->function_data.trampoline);
  PPCContext context{};context.virtual_membase=memory.virtual_membase();context.r[1]=page+0x800;
  const std::array<uint32_t,8> args{0,kNativeMediaApp,kNativeMediaQueryRead,kNativeMediaQueryClose,
      kNativeMediaQueryBytes,1,0,page};
  for(size_t n=0;n<args.size();++n)context.r[n+3]=args[n];
  const auto before=emulator->kernel_state()->object_table()->GetObjectsByType<XObject>(XObject::Type::Undefined).size();
  entry->function_data.trampoline(&context);
  REQUIRE(uint32_t(context.r[3])==0x8007139F);
  REQUIRE(*memory.TranslateVirtual<xe::be<uint32_t>*>(page)==0xA7A7A7A7);
  REQUIRE(emulator->kernel_state()->object_table()->GetObjectsByType<XObject>(XObject::Type::Undefined).size()==before);
  // Final typed admission is reached only after actual provider waits. A
  // missing/revoked owner never publishes its formal4-byte API output.
  auto denied_scope=xe::kernel::test::NativeMediaDeniedAliasTestAccess::DeniedPublication(memory);
  const std::array<uint8_t,4> handle_bytes{0x01,0x02,0x03,0x04};
  const util::GuestIoWrite denied_write{page,handle_bytes};
  REQUIRE(util::WriteGuestIoBuffers(memory,std::span(&denied_write,1),*denied_scope)==X_STATUS_CANCELLED);
  REQUIRE(*memory.TranslateVirtual<xe::be<uint32_t>*>(page)==0xA7A7A7A7);
#if !XE_PLATFORM_WIN32
  constexpr uint32_t physical_alias=0xE1000000;
  memory.SetPhysicalAliasSkipHostProtect(true);
  REQUIRE(memory.LookupHeap(physical_alias)->AllocFixed(physical_alias,0x1000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
  const auto physical=memory.GetPhysicalAddress(physical_alias);REQUIRE(physical!=UINT32_MAX);
  std::memset(memory.TranslatePhysical(physical),0x5B,0x1000);
  struct Provider {uint32_t physical;bool ran=false,outside=false;} provider{physical};
  auto* token=memory.RegisterPhysicalMemoryReadCallback(
      [](void* raw,uint32_t start,uint32_t length,Memory::PhysicalAccess) {
        auto& value=*static_cast<Provider*>(raw);
        if(value.ran || uint64_t(start)+length<=value.physical || start>=value.physical+0x1000)return;
        value.ran=true;value.outside=!global_critical_region::is_held_by_current_thread();
      },&provider);
  REQUIRE(token);
  memory.EnablePhysicalMemoryAccessCallbacks(physical,0x1000,true,true);
  auto physical_scope=xe::kernel::test::NativeMediaDeniedAliasTestAccess::DeniedPublication(memory);
  const util::GuestIoWrite physical_write{physical_alias+32,handle_bytes};
  REQUIRE(util::WriteGuestIoBuffers(memory,std::span(&physical_write,1),*physical_scope)==X_STATUS_CANCELLED);
  REQUIRE(provider.ran);REQUIRE(provider.outside);
  REQUIRE(std::all_of(memory.TranslatePhysical(physical),memory.TranslatePhysical(physical)+0x1000,
      [](uint8_t byte){return byte==0x5B;}));
  memory.UnregisterPhysicalMemoryReadCallback(token);
#endif
  auto unrelated=args;unrelated[1]=0xFB;
  REQUIRE_FALSE(NativeMediaService::TryEnumerationOnOriginalCaller(emulator->kernel_state(),0x24E,unrelated));
  REQUIRE_FALSE(NativeMediaService::TryEnumerationOnOriginalCaller(emulator->kernel_state(),0x24F,std::array<uint32_t,2>{0x1234,page}));
  // Actual ObjectTable aliasing stays ordinary until denial metadata closes
  // admission. This exercises the real table and numeric slots, not a fake
  // native CountedBody/refcount or a made-up native Close result.
  auto ordinary=object_ref<XEvent>(new XEvent(emulator->kernel_state()));
  ordinary->Initialize(true,false);
  auto* table=emulator->kernel_state()->object_table();
  const auto primary=ordinary->handle();uint32_t alias=0;
  REQUIRE(table->DuplicateHandle(primary,&alias)==X_STATUS_SUCCESS);REQUIRE(alias!=primary);
  REQUIRE(table->LookupObject<XEvent>(alias).get()==ordinary.get());
  const auto metadata_issuer=xe::kernel::test::NativeMediaDeniedAliasTestAccess::ProvisionalMetadataOnly(*ordinary);
  REQUIRE(metadata_issuer);
  REQUIRE(table->ReleaseHandle(alias)==X_STATUS_SUCCESS);
  REQUIRE(table->ReleaseHandle(primary)==X_STATUS_SUCCESS);
  REQUIRE(ordinary->handles().empty());
  // A real object retained by the caller can reopen after its last alias when
  // its role is still provisional. No native FA body/readiness is fabricated.
  uint32_t reopened=0;
  REQUIRE(table->AddHandle(ordinary.get(),&reopened,true)==X_STATUS_SUCCESS);
  REQUIRE(table->LookupObject<XEvent>(reopened).get()==ordinary.get());
  xe::kernel::test::NativeMediaDeniedAliasTestAccess::SelectAliasDenialOnly(*ordinary);
  REQUIRE(table->DuplicateHandle(reopened,&alias)==X_STATUS_SUCCESS);
  REQUIRE(table->RetainHandle(reopened)==X_STATUS_SUCCESS);
  REQUIRE(table->ReleaseHandle(reopened)==X_STATUS_SUCCESS); // same slot survives
  REQUIRE(table->LookupObject<XEvent>(reopened).get()==ordinary.get());
  REQUIRE(table->ReleaseHandle(alias)==X_STATUS_SUCCESS); // another alias survives
  uint32_t surviving_alias=0;
  REQUIRE(table->DuplicateHandle(reopened,&surviving_alias)==X_STATUS_SUCCESS);
  REQUIRE(table->ReleaseHandle(surviving_alias)==X_STATUS_SUCCESS);
  REQUIRE(table->ReleaseHandle(reopened)==X_STATUS_SUCCESS); // actual final slot
  REQUIRE(ordinary->handles().empty());
  uint32_t refused=0xD7D7D7D7;
  REQUIRE(table->AddHandle(ordinary.get(),&refused,true)==X_STATUS_INVALID_HANDLE);
  REQUIRE(refused==0xD7D7D7D7);
  auto replacement=object_ref<XEvent>(new XEvent(emulator->kernel_state()));
  replacement->Initialize(true,false);const auto replacement_handle=replacement->handle();
  REQUIRE(table->LookupObject<XEvent>(replacement_handle).get()==replacement.get());
  REQUIRE(table->DuplicateHandle(replacement_handle,&alias)==X_STATUS_SUCCESS);
  REQUIRE(table->ReleaseHandle(alias)==X_STATUS_SUCCESS);
  REQUIRE(table->ReleaseHandle(replacement_handle)==X_STATUS_SUCCESS);
  ordinary.reset();replacement.reset();
  denied_scope.reset();
#if !XE_PLATFORM_WIN32
  physical_scope.reset();
#endif
  memory.SystemHeapFree(page);
  emulator.reset();
}

TEST_CASE("Media alias final-close policy stays linearized during real table publication",
          "[kernel][native_media_owner][lifetime][concurrency]") {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;MediaScratch scratch;
  REQUIRE(std::filesystem::create_directory(scratch.path/"content"));
  REQUIRE(std::filesystem::create_directory(scratch.path/"cache"));
  auto emulator=std::make_unique<Emulator>("",scratch.path,scratch.path/"content",scratch.path/"cache");
  REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
  auto* table=emulator->kernel_state()->object_table();
  const bool selected=GENERATE(false,true);
  for(size_t iteration=0;iteration<16;++iteration) {
    auto object=object_ref<XEvent>(new XEvent(emulator->kernel_state()));
    object->Initialize(true,false);
    const auto issuer=xe::kernel::test::NativeMediaDeniedAliasTestAccess::ProvisionalMetadataOnly(*object);
    REQUIRE(issuer);
    if(selected)xe::kernel::test::NativeMediaDeniedAliasTestAccess::SelectAliasDenialOnly(*object);
    const auto primary=object->handle();
    std::promise<void> ready,start;auto starting=start.get_future();
    X_STATUS published=X_STATUS_UNSUCCESSFUL;uint32_t alias=0xA7A7A7A7;
    std::thread publisher([&] {
      ready.set_value();starting.wait();
      published=table->AddHandle(object.get(),&alias,true);
    });
    ready.get_future().wait();start.set_value();
    const auto closed=table->ReleaseHandle(primary);
    publisher.join(); // No assertion or destruction races the actual publisher.
    REQUIRE(closed==X_STATUS_SUCCESS);
    if(XSUCCEEDED(published)) {
      REQUIRE(table->LookupObject<XEvent>(alias).get()==object.get());
      REQUIRE(table->ReleaseHandle(alias)==X_STATUS_SUCCESS);
    } else {
      REQUIRE(selected);REQUIRE(published==X_STATUS_INVALID_HANDLE);
      REQUIRE(alias==0xA7A7A7A7);
    }
    REQUIRE(object->handles().empty());
    uint32_t after=0xD7D7D7D7;
    const auto reopened=table->AddHandle(object.get(),&after,true);
    if(selected) {
      REQUIRE(reopened==X_STATUS_INVALID_HANDLE);REQUIRE(after==0xD7D7D7D7);
    } else {
      REQUIRE(reopened==X_STATUS_SUCCESS);
      REQUIRE(table->LookupObject<XEvent>(after).get()==object.get());
      REQUIRE(table->ReleaseHandle(after)==X_STATUS_SUCCESS);
    }
  }
  // Denial-only metadata on real XEvents exercises table locking and aliases;
  // it does not mint a native body/ref, initialization, Task or Close receipt.
  emulator.reset();
}
TEST_CASE("Counted table Reset invalidates escaped birth metadata without reminting it",
          "[kernel][native_media_owner][lifetime][counted]") {
  util::ObjectTable table;
  using Access=xe::kernel::test::NativeMediaDeniedAliasTestAccess;
  const auto old=Access::CountedBirth(table);
  REQUIRE(Access::CurrentCountedBirth(table,old));
  // This actual host object and table do not claim native guest allocation.
  auto object=object_ref<XObject>(new XObject(XObject::Type::Undefined));
  uint32_t handle=0;REQUIRE(table.AddHandle(object.get(),&handle,true)==X_STATUS_SUCCESS);
  REQUIRE(table.LookupObject<XObject>(handle).get()==object.get());
  table.Reset();REQUIRE(object->handles().empty());REQUIRE(old.expired());
  REQUIRE_FALSE(Access::CurrentCountedBirth(table,old));
  const auto fresh=Access::CountedBirth(table);
  REQUIRE(Access::CurrentCountedBirth(table,fresh));
  REQUIRE_FALSE(Access::CurrentCountedBirth(table,old));
  table.Reset();REQUIRE(fresh.expired());
}
TEST_CASE("Real sequential Kernels reject an escaped counted wrapper before current insertion",
          "[kernel][native_media_owner][lifetime][counted]") {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;MediaScratch scratch;
  REQUIRE(std::filesystem::create_directory(scratch.path/"content"));
  REQUIRE(std::filesystem::create_directory(scratch.path/"cache"));
  auto setup=[&] {
    auto owner=std::make_unique<Emulator>("",scratch.path,scratch.path/"content",scratch.path/"cache");
    REQUIRE(owner->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);return owner;
  };
  constexpr uint32_t page=0x90011000,body=page+0x110,type=page+0x300,output=page+0x500;
  auto prepare=[&](Emulator& owner) {
    auto& memory=*owner.memory();
    REQUIRE(memory.LookupHeap(page)->AllocFixed(page,0x1000,0x1000,
        kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
    memory.Fill(page,0x1000,0xA7);
    X_OBJECT_HEADER header{};header.pointer_count=5;header.object_type_ptr=type;
    std::memcpy(memory.TranslateVirtual(body-sizeof(header)),&header,sizeof(header));
    // The genuine public counted-object primitive accepts this caller-owned
    // custom type. No native FA allocation or callback success is fabricated;
    // all procedures are zero, and the retained guest pointers never hit zero.
    X_OBJECT_TYPE custom{};std::memcpy(memory.TranslateVirtual(type),&custom,sizeof(custom));
  };
  auto call=[&](Emulator& owner,uint16_t ordinal,std::initializer_list<uint32_t> args) {
    auto* entry=owner.export_resolver()->GetExportByOrdinal("xboxkrnl.exe",ordinal);
    REQUIRE(entry);REQUIRE(entry->function_data.trampoline);
    PPCContext context{};context.virtual_membase=owner.memory()->virtual_membase();
    context.kernel_state=owner.kernel_state();context.processor=owner.processor();context.r[1]=page+0x800;
    size_t index=3;for(auto arg:args)context.r[index++]=arg;
    entry->function_data.trampoline(&context);return uint32_t(context.r[3]);
  };
  auto first=setup();prepare(*first);
  REQUIRE(call(*first,0x108,{body,0,1,output})==X_STATUS_SUCCESS);
  const auto first_handle=*first->memory()->TranslateVirtual<xe::be<uint32_t>*>(output);
  auto escaped=first->kernel_state()->object_table()->LookupObject<XObject>(first_handle);
  REQUIRE(escaped);const auto* first_kernel=first->kernel_state();
  REQUIRE(escaped->kernel_state()==first_kernel);
  // A same-Kernel native pointer may genuinely reopen an empty handle list.
  REQUIRE(call(*first,0xCF,{first_handle})==X_STATUS_SUCCESS);
  REQUIRE(escaped->handles().empty());
  REQUIRE(call(*first,0x10E,{body,output})==X_STATUS_SUCCESS);
  const auto reopened=*first->memory()->TranslateVirtual<xe::be<uint32_t>*>(output);
  REQUIRE(first->kernel_state()->object_table()->LookupObject<XObject>(reopened).get()==escaped.get());
  REQUIRE(call(*first,0xCF,{reopened})==X_STATUS_SUCCESS);
  REQUIRE(escaped->handles().empty());
  REQUIRE(call(*first,0x10E,{body,output})==X_STATUS_SUCCESS);
  REQUIRE(escaped->handles().size()==1);
  first.reset(); // Actual old table/Kernel/Memory teardown; wrapper stays escaped.
  REQUIRE(escaped->handles().empty()); // The real old table withdrew its alias.
  auto second=setup();prepare(*second);
  auto* current=second->kernel_state()->object_table();
  const auto count=current->GetAllObjects().size();
  std::array<uint8_t,sizeof(X_OBJECT_HEADER)> header_before{};
  std::memcpy(header_before.data(),second->memory()->TranslateVirtual(body-sizeof(X_OBJECT_HEADER)),header_before.size());
  REQUIRE(call(*second,0x108,{body,0,1,output})==X_STATUS_INVALID_HANDLE);
  REQUIRE(*second->memory()->TranslateVirtual<xe::be<uint32_t>*>(output)==0xA7A7A7A7);
  REQUIRE(current->GetAllObjects().size()==count);REQUIRE(escaped->handles().empty());
  REQUIRE(escaped->kernel_state()==first_kernel); // pointer comparison only
  REQUIRE(std::equal(header_before.begin(),header_before.end(),
      second->memory()->TranslateVirtual<const uint8_t*>(body-sizeof(X_OBJECT_HEADER))));
  REQUIRE(call(*second,0x10E,{body,output})==X_STATUS_INVALID_HANDLE);
  REQUIRE(*second->memory()->TranslateVirtual<xe::be<uint32_t>*>(output)==0); // real export's failure convention
  REQUIRE(current->GetAllObjects().size()==count);REQUIRE(escaped->handles().empty());
  REQUIRE(std::equal(header_before.begin(),header_before.end(),
      second->memory()->TranslateVirtual<const uint8_t*>(body-sizeof(X_OBJECT_HEADER))));
  // A refused insertion did not acquire a current wrapper or guest reference.
  // The caller's actual allocation reference remains separately owned; real
  // reference/dereference primitives affect only that current header. We keep
  // it above zero and make no native allocator/free-completion claim here.
  auto* current_header=second->memory()->TranslateVirtual<X_OBJECT_HEADER*>(body-sizeof(X_OBJECT_HEADER));
  REQUIRE(current_header->pointer_count==5);REQUIRE(current_header->handle_count==0);
  (void)call(*second,0x10F,{body});
  REQUIRE(current_header->pointer_count==6);REQUIRE(current_header->handle_count==0);
  (void)call(*second,0x105,{body});
  REQUIRE(current_header->pointer_count==5);REQUIRE(current_header->handle_count==0);
  REQUIRE(current->GetAllObjects().size()==count);REQUIRE(escaped->handles().empty());
  REQUIRE(call(*second,0x108,{body,0,1,output})==X_STATUS_INVALID_HANDLE);
  REQUIRE(current_header->pointer_count==5);REQUIRE(current_header->handle_count==0);
  // Rejection preserves the exact old map entry. Only its real last host owner
  // leaving removes that entry; a fresh current-Kernel insertion then succeeds.
  escaped.reset();
  REQUIRE(call(*second,0x108,{body,0,1,output})==X_STATUS_SUCCESS);
  const auto fresh_handle=*second->memory()->TranslateVirtual<xe::be<uint32_t>*>(output);
  auto fresh=current->LookupObject<XObject>(fresh_handle);REQUIRE(fresh);
  REQUIRE(fresh->kernel_state()==second->kernel_state());
  REQUIRE(call(*second,0xCF,{fresh_handle})==X_STATUS_SUCCESS);
  REQUIRE(fresh->handles().empty());fresh.reset();second.reset();
}
TEST_CASE("Ordinary counted reference lookup and paired release preserve real guest counts",
          "[kernel][native_listener][reference]") {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;MediaScratch scratch;
  REQUIRE(std::filesystem::create_directory(scratch.path/"content"));
  REQUIRE(std::filesystem::create_directory(scratch.path/"cache"));
  auto emulator=std::make_unique<Emulator>("",scratch.path,scratch.path/"content",scratch.path/"cache");
  REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
  auto& memory=*emulator->memory();
  constexpr uint32_t page=0x90011000,body=page+0x110,type=page+0x300,output=page+0x500;
  REQUIRE(memory.LookupHeap(page)->AllocFixed(page,0x1000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
  memory.Fill(page,0x1000,0);
  X_OBJECT_HEADER header{};header.pointer_count=5;header.object_type_ptr=type;
  std::memcpy(memory.TranslateVirtual(body-sizeof(header)),&header,sizeof(header));
  // Real ordinary Object Manager primitive, caller-owned custom type. No
  // native listener body/registry/Task/readiness/close receipt is fabricated.
  const auto call=[&](uint16_t ordinal,std::initializer_list<uint32_t> args){
    auto* entry=emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",ordinal);
    REQUIRE(entry);REQUIRE(entry->function_data.trampoline);
    PPCContext context{};context.virtual_membase=memory.virtual_membase();
    context.kernel_state=emulator->kernel_state();context.processor=emulator->processor();context.r[1]=page+0x800;
    size_t index=3;for(auto arg:args)context.r[index++]=arg;
    entry->function_data.trampoline(&context);return uint32_t(context.r[3]);
  };
  REQUIRE(call(0x108,{body,0,1,output})==X_STATUS_SUCCESS);
  const uint32_t handle=*memory.TranslateVirtual<xe::be<uint32_t>*>(output);
  auto wrapper=emulator->kernel_state()->object_table()->LookupObject<XObject>(handle);REQUIRE(wrapper);
  const auto counts=[&]{return std::pair(int32_t(memory.TranslateVirtual<X_OBJECT_HEADER*>(body-sizeof(header))->pointer_count),
                                        int32_t(memory.TranslateVirtual<X_OBJECT_HEADER*>(body-sizeof(header))->handle_count));};
  const auto baseline=counts();REQUIRE(baseline.first>1);REQUIRE(baseline.second==1);
  REQUIRE(call(0x110,{handle,type,output})==X_STATUS_SUCCESS);
  CHECK(uint32_t(*memory.TranslateVirtual<xe::be<uint32_t>*>(output))==body);
  CHECK(counts()==std::pair(baseline.first+1,baseline.second));
  call(0x105,{body});CHECK(counts()==baseline);
  *memory.TranslateVirtual<xe::be<uint32_t>*>(output)=0xA7A7A7A7;
  CHECK(call(0x110,{handle,type+4,output})==X_STATUS_OBJECT_TYPE_MISMATCH);
  CHECK(uint32_t(*memory.TranslateVirtual<xe::be<uint32_t>*>(output))==0);CHECK(counts()==baseline);
  *memory.TranslateVirtual<xe::be<uint32_t>*>(output)=0xA7A7A7A7;
  CHECK(call(0x110,{0xFFFFFFFC,type,output})==X_STATUS_INVALID_HANDLE);
  CHECK(uint32_t(*memory.TranslateVirtual<xe::be<uint32_t>*>(output))==0);CHECK(counts()==baseline);
  REQUIRE(emulator->kernel_state()->object_table()->ReleaseHandle(handle)==X_STATUS_SUCCESS);
  REQUIRE(wrapper->handles().empty());wrapper.reset();emulator.reset();
}
} // namespace xe::kernel::xam::test
