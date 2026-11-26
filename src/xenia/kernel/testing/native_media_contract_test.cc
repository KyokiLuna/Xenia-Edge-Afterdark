/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_media_contract.h"
#include "xenia/kernel/native_xmsg_provenance_state.h"
#include "xenia/kernel/native_media_service.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xam/content_manager.h"
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
// Non-admitting range borrow only. This seam cannot create a service, native
// attachment/stream/File witness, Task, callback, code or readiness receipt.
class NativeMediaDirectTestAccess {
 public:
  static auto Borrow(Memory& memory,uint32_t message,uint32_t payload,
                     std::span<const uint32_t> words) {
    return NativeMediaService::BorrowDirectBacking(memory,message,payload,words);
  }
};
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
TEST_CASE("Direct native source metadata and stream requests preserve their distinct real ABI",
          "[kernel][native_media_owner][direct][abi]") {
  const std::array<uint32_t,4> original{kNativeMediaApp,kNativeMediaSourceStream,0x90010000,0};
  REQUIRE(NativeMediaDirectOriginalArgumentsValid(0x1F4,original));
  for(uint16_t ordinal:{uint16_t(0x1F6),uint16_t(0x1F7),uint16_t(0x1FC),uint16_t(0x49F)})
    CHECK_FALSE(NativeMediaDirectOriginalArgumentsValid(ordinal,original));
  CHECK_FALSE(NativeMediaDirectOriginalArgumentsValid(0x1F4,std::span(original).first(3)));
  // B's scalar filter is never a pointer. The actual native writer requires
  // ten B4-byte slots when an array is supplied, or just count4 for sizing.
  std::array<uint32_t,5> metadata{0,0xFFFF1234,0,0,0x90012000};
  REQUIRE(NativeMediaDirectPayloadValid(kNativeMediaSourceMetadata,metadata));
  metadata[2]=0x90014000;metadata[3]=9;
  // It remains a valid wire request: the original native body returns its
  // actual capacity error before touching the undersized array.
  CHECK(NativeMediaDirectPayloadValid(kNativeMediaSourceMetadata,metadata));
  metadata[3]=10;REQUIRE(NativeMediaDirectPayloadValid(kNativeMediaSourceMetadata,metadata));
  CHECK(kNativeMediaSourceMetadataCapacity*kNativeMediaSourceMetadataBytes==0x708);
  const std::array<uint32_t,5> stream{0,0x90016000,0x40,0,0x90012000};
  REQUIRE(NativeMediaDirectPayloadValid(kNativeMediaSourceStream,stream));
  std::array<uint8_t,kNativeMediaFileIdentityBytes> identity;
  identity.fill(0x7F);
  auto* descriptor=identity.data()+kNativeMediaFileDescriptorOffset;
  descriptor[kNativeMediaFileNameBytes]=1;
  CHECK_FALSE(NativeMediaFileIdentityValid(identity));
  // The final permitted UTF16 terminator is at index260, immediately before
  // the tag. No terminator outside that real field can qualify this input.
  descriptor[0x208]=descriptor[0x209]=0;
  REQUIRE(NativeMediaFileIdentityValid(identity));
  CHECK_FALSE(NativeMediaFileIdentityValid(std::span(identity).first(0x28)));
  descriptor[kNativeMediaFileNameBytes]=2;
  descriptor[0x208]=descriptor[0x209]=0x7F;
  descriptor[0]=descriptor[1]=0; // Tag2 consumes descriptor+6, not this prefix.
  CHECK_FALSE(NativeMediaFileIdentityValid(identity));
  descriptor[6]=descriptor[7]=0;REQUIRE(NativeMediaFileIdentityValid(identity));
  descriptor[kNativeMediaFileNameBytes]=3;CHECK_FALSE(NativeMediaFileIdentityValid(identity));
}
TEST_CASE("Direct media backing protects the actual nested outputs and cannot substitute aliases",
          "[kernel][native_media_owner][direct][memory]") {
  Memory memory;REQUIRE(memory.Initialize());
  constexpr uint32_t payload=0x90010000,count=0x90012000,identity=0x90014000,array=0x90016FF0;
  constexpr uint32_t rw=kMemoryProtectRead|kMemoryProtectWrite;
  auto* heap=memory.LookupHeap(payload);
  for(uint32_t page:{payload,count,identity})REQUIRE(heap->AllocFixed(page,0x1000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,rw));
  REQUIRE(heap->AllocFixed(0x90016000,0x2000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,rw));
  using Access=xe::kernel::test::NativeMediaDirectTestAccess;
  std::array<uint32_t,5> words{0,0xFFFF1234,array,10,count};
  auto borrowed=Access::Borrow(memory,kNativeMediaSourceMetadata,payload,words);
  REQUIRE(borrowed);REQUIRE(borrowed->current());
  CHECK_FALSE(heap->Decommit(payload,0x1000));
  CHECK_FALSE(heap->Decommit(count,0x1000));
  CHECK_FALSE(heap->Decommit(0x90017000,0x1000)); // B output actually crosses it.
  CHECK_FALSE(heap->Release(0x90016000));
  borrowed.reset();
  REQUIRE(heap->Protect(identity,0x1000,kMemoryProtectRead));
  words={0,identity,0x40,0,count};
  borrowed=Access::Borrow(memory,kNativeMediaSourceStream,payload,words);
  REQUIRE(borrowed);CHECK_FALSE(heap->Release(identity));
  CHECK_FALSE(heap->Protect(count,0x1000,kMemoryProtectNoAccess));
  words[4]=payload+4; // Writable input/output alias still cannot qualify.
  CHECK_FALSE(Access::Borrow(memory,kNativeMediaSourceStream,payload,words));
  borrowed.reset();
  REQUIRE(heap->Protect(count,0x1000,kMemoryProtectRead));
  words[4]=count;CHECK_FALSE(Access::Borrow(memory,kNativeMediaSourceStream,payload,words));
  REQUIRE(heap->Protect(count,0x1000,rw));
  words={0,1,array,9,count};
  borrowed=Access::Borrow(memory,kNativeMediaSourceMetadata,payload,words);
  REQUIRE(borrowed); // Native capacity failure needs no array access/borrow.
  CHECK(heap->Decommit(0x90017000,0x1000));
  borrowed.reset();
  // The range lease preserves pages, not custom-heap allocation ownership.
  for(uint32_t page:{payload,count,identity,0x90016000u})REQUIRE(heap->Release(page));
}
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
TEST_CASE("Direct media source use transfers into a real XFile through final close and saved removal",
          "[kernel][native_media_owner][direct][selected_media][lifetime]") {
  const bool folder=GENERATE(false,true);
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;MediaScratch runtime,input;
  REQUIRE(std::filesystem::create_directory(runtime.path/"content"));
  REQUIRE(std::filesystem::create_directory(runtime.path/"cache"));
  std::ofstream(input.path/"song")<<"original";
  const auto setup=[&] {
    auto result=std::make_unique<Emulator>("",runtime.path,runtime.path/"content",runtime.path/"cache");
    REQUIRE(result->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);return result;
  };
  auto emulator=setup();auto* kernel=emulator->kernel_state();
  auto* content=kernel->content_manager();auto* fs=emulator->file_system();auto* table=kernel->object_table();
  const auto added=content->AddMediaReference({folder?input.path:input.path/"song"},folder,*fs);
  REQUIRE_FALSE(added.error);REQUIRE(added.persisted);REQUIRE(added.durable);REQUIRE(added.active);
  auto source=content->sources().Source(added.id);REQUIRE(source);
  const auto mount=source->source.guest_volume_root;
  auto direct_use=content->sources().CaptureSelectedMediaUse(*source);REQUIRE(direct_use);
  auto device=fs->AcquireDevice(mount);REQUIRE(device);
  vfs::File* raw=nullptr;vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr,mount+(folder?R"(\song)":R"(\0\song)"),
      vfs::FileDisposition::kOpen,vfs::FileAccess::kFileReadData,false,true,&raw,&action)==X_STATUS_SUCCESS);
  REQUIRE(raw);REQUIRE(raw->retained_device()==device);
  auto file=object_ref<XFile>(new XFile(kernel,raw,false,false,true));
  const uint32_t primary=file->handle();uint32_t alias=0;
  REQUIRE(table->DuplicateHandle(primary,&alias)==X_STATUS_SUCCESS);
  auto in_flight=table->LookupObject<XFile>(primary);REQUIRE(in_flight.get()==file.get());
  // This is the actual common File authority after direct-call handoff, with
  // a real I/O-style object_ref. No native stream/Task/readiness is fabricated.
  direct_use.reset();file.reset();
  const auto removed=content->RemoveMediaReference(added.id,*fs);
  REQUIRE(removed.removed);REQUIRE(removed.persisted);REQUIRE(removed.durable);
  REQUIRE(removed.error==std::errc::device_or_resource_busy);
  REQUIRE(fs->AcquireDevice(mount)==device);REQUIRE(device->has_open_files());
  auto* close=emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",0xCF);
  REQUIRE(close);REQUIRE(close->function_data.trampoline);
  const auto actual_close=[&](uint32_t handle) {
    PPCContext context{};context.virtual_membase=emulator->memory()->virtual_membase();
    context.kernel_state=kernel;context.processor=emulator->processor();context.r[3]=handle;
    close->function_data.trampoline(&context);return uint32_t(context.r[3]);
  };
  REQUIRE(actual_close(primary)==X_STATUS_SUCCESS);
  REQUIRE(table->LookupObject<XFile>(alias).get()==in_flight.get());
  REQUIRE(actual_close(alias)==X_STATUS_SUCCESS);REQUIRE(in_flight->handles().empty());
  std::error_code error;
  REQUIRE(content->RestoreMediaReferences(*fs,error).empty());REQUIRE_FALSE(error);
  REQUIRE(fs->AcquireDevice(mount)==device);REQUIRE(device->has_open_files());
  in_flight.reset();REQUIRE_FALSE(device->has_open_files());
  REQUIRE(content->RestoreMediaReferences(*fs,error).empty());REQUIRE_FALSE(error);
  REQUIRE_FALSE(fs->AcquireDevice(mount));REQUIRE_FALSE(content->sources().Source(added.id));
  source.reset();device.reset();emulator.reset();
  // The real durable removal cannot resurrect the source after a fresh Kernel.
  emulator=setup();content=emulator->kernel_state()->content_manager();fs=emulator->file_system();
  REQUIRE(content->RestoreMediaReferences(*fs,error).empty());REQUIRE_FALSE(error);
  REQUIRE_FALSE(content->sources().Source(added.id));REQUIRE_FALSE(fs->AcquireDevice(mount));
  std::ifstream original(input.path/"song");std::string bytes;original>>bytes;REQUIRE(bytes=="original");
}
#endif
} // namespace xe::kernel::xam::test

namespace xe::kernel::test {
// Actual private production metadata methods, not a native service fixture.
// These tests cannot publish a service/Task/source or bypass its code gates.
class NativeXmsgProvenanceTestAccess final {
 public:
  using Owner=NativeXmsgProvenance;
  using State=Owner::State;
  using Record=Owner::Capture::Record;
  using Frame=Owner::FrameFact;
  static constexpr Owner::RootFact kRoot{17,23,29};
  static std::shared_ptr<Record> Request(const std::shared_ptr<State>& state,uint64_t capture=1) {
    auto record=std::make_shared<Record>();record->owner=state;record->capture=capture;
    record->producer.root=kRoot;record->producer.app=0xFB;record->producer.message=0xB0004;
    record->producer.argument=0x90100100;record->producer.argument_bytes=4;
    record->producer.overlap=0x90101000;record->copy_entered=true;
    state->records.push_back(record);return record;
  }
  static void DistinctProducerForms() {
    auto state=std::make_shared<State>(kRoot);const auto record=Request(state);
    cpu::ppc::PPCContext context{};
    context.r[3]=0xFB;context.r[4]=0xB0004;context.r[5]=0x90100100;context.r[6]=4;context.r[7]=0x90101000;
    REQUIRE(state->PublicArguments(*record,context,false));
    CHECK_FALSE(state->PublicArguments(*record,context,true));
    std::swap(context.r[5],context.r[7]);std::swap(context.r[6],context.r[7]);
    // Genuine direct Start r5=overlap,r6=argument,r7=copy extent.
    REQUIRE(state->PublicArguments(*record,context,true));
    CHECK_FALSE(state->PublicArguments(*record,context,false));
    context.r[6]=0x1122334490100100ull;CHECK_FALSE(state->PublicArguments(*record,context,true));
    context.r[6]=0xFFFFFFFF90100100ull;REQUIRE(state->PublicArguments(*record,context,true));
    // Common provider callback arg2 is opaque and independently observed.
    Owner::DispatchFact fact;fact.original_argument_bytes=4;fact.provider_argument2=0;
    CHECK(fact.original_argument_bytes==4);CHECK(fact.provider_argument2==0);
    // This real metadata object is not an installed/authenticated native owner.
    Owner::Capture capture(record);CHECK(Owner::FactsAvailable(capture));
    CHECK_FALSE(Owner::CurrentDispatch(&context,capture));
  }
  static void CopyFreeAndReuse() {
    auto state=std::make_shared<State>(kRoot);const auto old=Request(state);
    const std::array<uint8_t,4> input{0x31,0,0x72,0xA4};
    Memory memory;REQUIRE(memory.Initialize());constexpr uint32_t page=0x90200000;
    REQUIRE(memory.LookupHeap(page)->AllocFixed(page,0x1000,0x1000,
        kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
    std::memcpy(memory.TranslateVirtual(page+0x1C),input.data(),input.size());
    const auto owned=State::ReadCopiedPayload(memory,page+0x1C,4);REQUIRE(owned);
    REQUIRE(state->BindCopiedPayload(*old,0,true,true,page,page+0x1C,owned));
    const auto old_copy=old->copy_serial;REQUIRE(old_copy);
    CHECK((old->copied_bytes && std::equal(input.begin(),input.end(),old->copied_bytes->begin())));
    std::memset(memory.TranslateVirtual(page+0x1C),0xCC,input.size());
    REQUIRE(memory.LookupHeap(page)->Release(page));
    CHECK(std::equal(input.begin(),input.end(),old->copied_bytes->begin()));
    CHECK_FALSE(State::ReadCopiedPayload(memory,page+0x1C,4));
    CHECK_FALSE(state->BindCopiedPayload(*old,old->version,true,true,0x90300000,0x9030001C,owned));
    Owner::Capture retained(old);
    state->Revoke(0);state->Revoke(0x99990000);CHECK(state->Live(*old));
    REQUIRE(Owner::FactsAvailable(retained));
    const auto before=Owner::Observe(retained);REQUIRE(before);
    CHECK_FALSE(before->authority_revoked);CHECK_FALSE(before->kernel_stopped);
    state->Revoke(0x90200000);CHECK_FALSE(state->Live(*old));CHECK(old->revoked);
    CHECK_FALSE(Owner::FactsAvailable(retained));
    const auto revoked=Owner::Observe(retained);REQUIRE(revoked);
    CHECK(revoked->authority_revoked);CHECK_FALSE(revoked->released);
    CHECK_FALSE(revoked->kernel_stopped); // Free ENTRY is no completed free/stop.
    const auto fresh=Request(state,2);
    REQUIRE(state->BindCopiedPayload(*fresh,0,true,true,0x90200000,0x9020001C,owned));
    REQUIRE(fresh->copy_serial!=old_copy);CHECK(fresh->envelope==old->envelope);
    CHECK(Owner::FactsAvailable(Owner::Capture(fresh)));
    CHECK_FALSE(Owner::FactsAvailable(retained));
    const auto selected=state->Unique([](const auto& r){return r.envelope==0x90200000;});
    CHECK(selected==fresh);CHECK_FALSE(state->Live(*old));
    auto canceled=Request(state,3);Owner::Capture cancellation(canceled);Owner::Release(cancellation);
    CHECK_FALSE(state->BindCopiedPayload(*canceled,canceled->version,true,true,0x90400000,0x9040001C,owned));
    auto wrong=Request(state,4);
    CHECK_FALSE(state->BindCopiedPayload(*wrong,0,true,true,0x90500000,0xDEAD0000,owned));
    CHECK(wrong->unavailable);CHECK(wrong->envelope==0);CHECK(wrong->copy_serial==0);
    auto failed=Request(state,5);
    REQUIRE(state->BindCopiedPayload(*failed,0,false,true,0,0,{}));
    CHECK(failed->copy_returned);CHECK(failed->envelope==0);CHECK(failed->copy_serial==0);
    auto no_entry=Request(state,6);no_entry->copy_entered=false;
    CHECK_FALSE(state->BindCopiedPayload(*no_entry,0,true,true,0x90600000,0x9060001C,owned));
  }
  static void MetadataOnlyCopyIsolation() {
    auto state=std::make_shared<State>(kRoot);uint64_t capture=1;
    const auto metadata=[&] {
      auto record=Request(state,capture++);record->producer.app=0xFC;
      record->producer.message=0x58001;record->producer.argument_bytes=0x568;
      record->identity_service_metadata_only=true;return record;
    };
    constexpr uint32_t envelope=0x90200000,argument=envelope+0x1C;
    const auto old=metadata();
    REQUIRE(state->BindCopiedPayload(*old,0,true,true,envelope,argument,{}));
    const auto old_copy=old->copy_serial;REQUIRE(old_copy);
    CHECK(old->copy_returned);CHECK(old->envelope==envelope);
    CHECK(old->copied_argument==argument);CHECK_FALSE(old->copied_bytes);
    // Copy metadata is no authenticated service origin or provider callback.
    CHECK(old->identity_service.expired());CHECK_FALSE(old->callback_entered);
    CHECK(state->providers.empty());Owner::Capture retained(old);
    REQUIRE(Owner::FactsAvailable(retained));
    CHECK_FALSE(Owner::CurrentDispatch(nullptr,retained));
    state->Revoke(envelope);CHECK_FALSE(state->Live(*old));
    CHECK_FALSE(Owner::FactsAvailable(retained));
    const auto fresh=metadata();
    REQUIRE(state->BindCopiedPayload(*fresh,0,true,true,envelope,argument,{}));
    CHECK(fresh->copy_serial!=old_copy);CHECK_FALSE(fresh->copied_bytes);
    CHECK(state->Unique([&](const auto& r){return r.envelope==envelope;})==fresh);
    CHECK_FALSE(Owner::FactsAvailable(retained));
    CHECK(fresh->identity_service.expired());CHECK_FALSE(fresh->callback_entered);
    CHECK_FALSE(Owner::CurrentDispatch(nullptr,Owner::Capture(fresh)));
    CHECK_FALSE(state->BindCopiedPayload(*fresh,fresh->version,true,true,
                                        0x90300000,0x9030001C,{}));
    const auto payload=std::make_shared<const std::vector<uint8_t>>(0x568,0xA5);
    // Equal extent does not let ordinary data requests omit their owned copy.
    const auto ordinary=Request(state,capture++);ordinary->producer.argument_bytes=0x568;
    CHECK_FALSE(state->BindCopiedPayload(*ordinary,0,true,true,0x90400000,0x9040001C,{}));
    CHECK(ordinary->unavailable);CHECK_FALSE(Owner::FactsAvailable(Owner::Capture(ordinary)));
    const auto data=Request(state,capture++);data->producer.argument_bytes=0x568;
    REQUIRE(state->BindCopiedPayload(*data,0,true,true,0x90400000,0x9040001C,payload));
    CHECK(data->copied_bytes==payload);CHECK_FALSE(data->identity_service_metadata_only);
    const auto wrong_size=metadata();wrong_size->producer.argument_bytes=0x564;
    CHECK_FALSE(state->BindCopiedPayload(*wrong_size,0,true,true,0x90500000,0x9050001C,{}));
    const auto supplied_payload=metadata();
    CHECK_FALSE(state->BindCopiedPayload(*supplied_payload,0,true,true,0x90600000,0x9060001C,payload));
    const auto wrong_argument=metadata();
    CHECK_FALSE(state->BindCopiedPayload(*wrong_argument,0,true,true,0x90700000,0x90700020,{}));
    for(const auto& rejected:{wrong_size,supplied_payload,wrong_argument}) {
      CHECK(rejected->unavailable);CHECK(rejected->envelope==0);
      CHECK(rejected->copy_serial==0);CHECK_FALSE(rejected->copied_bytes);
      CHECK_FALSE(Owner::FactsAvailable(Owner::Capture(rejected)));
    }
    const auto failed=metadata();
    REQUIRE(state->BindCopiedPayload(*failed,0,false,true,0,0,{}));
    CHECK(failed->copy_returned);CHECK(failed->envelope==0);
    CHECK(failed->copy_serial==0);CHECK_FALSE(failed->copied_bytes);
    CHECK(failed->identity_service.expired());CHECK(state->providers.empty());
  }
  static void ProviderShadowsAndHistoricalCapture() {
    auto state=std::make_shared<State>(kRoot);const auto record=Request(state);
    int actual_thread=0,other_thread=0;
    const Frame outer{3,0x80004000,0x70810000,0x816A2BE8,0x816A29C0};
    auto body=outer;body.sp-=0x80;body.lr=0x816A2A24;
    State::Provider provider;provider.frame=outer;provider.actual_thread=&actual_thread;
    provider.record=record;provider.serial=1;provider.fact=std::make_shared<Owner::DispatchFact>();
    state->providers.push_back(provider);
    REQUIRE(state->CurrentProvider(*record,body,&actual_thread));
    CHECK_FALSE(state->CurrentProvider(*record,outer,&actual_thread));
    CHECK_FALSE(state->CurrentProvider(*record,body,&other_thread));
    State::Provider unadmitted;unadmitted.frame=outer;unadmitted.frame.sp-=0x180;
    unadmitted.actual_thread=&actual_thread;unadmitted.serial=2;
    state->providers.push_back(unadmitted);auto nested=body;nested.sp-=0x180;
    CHECK_FALSE(state->CurrentProvider(*record,nested,&actual_thread)); // never search older matching app
    state->providers.back().returned=true;state->Prune();
    REQUIRE(state->CurrentProvider(*record,body,&actual_thread));
    state->provider_overflow=true;CHECK_FALSE(state->CurrentProvider(*record,body,&actual_thread));
    state->provider_overflow=false;Owner::Capture historical(record);Owner::Release(historical);
    CHECK(historical);CHECK_FALSE(state->Live(*record));CHECK_FALSE(Owner::FactsAvailable(historical));
    CHECK_FALSE(state->CurrentProvider(*record,body,&actual_thread));
    CHECK_FALSE(Owner::CurrentDispatch(nullptr,historical));
  }
  static void SharedReleaseHasNoHandleWrite() {
    auto state=std::make_shared<State>(kRoot);const auto record=Request(state);Owner::Capture capture(record);
    const auto historical=record->capture;std::array<std::thread,2> workers;
    struct Join {std::array<std::thread,2>& workers;~Join(){for(auto& worker:workers)if(worker.joinable())worker.join();}} join{workers};
    workers[0]=std::thread([&]{for(unsigned n=0;n<100;++n)Owner::Release(capture);});
    workers[1]=std::thread([&]{for(unsigned n=0;n<100;++n){
      (void)Owner::FactsAvailable(capture);(void)Owner::Observe(capture);Owner::Release(capture);
    }});
    for(auto& worker:workers)worker.join();
    CHECK(capture);CHECK(record->released);CHECK(record->capture==historical);
    CHECK_FALSE(state->Live(*record));CHECK_FALSE(Owner::FactsAvailable(capture));
    const auto observed=Owner::Observe(capture);REQUIRE(observed);CHECK(observed->released);
    CHECK_FALSE(Owner::CurrentDispatch(nullptr,capture));
  }
};
TEST_CASE("Original XMsg request and direct Start producer forms retain their distinct ABI", "[kernel][xmsg_provenance][abi]") {
  NativeXmsgProvenanceTestAccess::DistinctProducerForms();
}
TEST_CASE("Observed native XMsg copies revoke on real free and never retarget equal address reuse", "[kernel][xmsg_provenance][copy][lifetime]") {
  NativeXmsgProvenanceTestAccess::CopyFreeAndReuse();
}
TEST_CASE("Identity service metadata copies keep lifetime and data-provider authority separate", "[kernel][xmsg_provenance][identity_service][copy][lifetime]") {
  NativeXmsgProvenanceTestAccess::MetadataOnlyCopyIsolation();
}
TEST_CASE("Unadmitted nested XMsg providers and released historical captures cannot lend older facts", "[kernel][xmsg_provenance][provider][lifetime]") {
  NativeXmsgProvenanceTestAccess::ProviderShadowsAndHistoricalCapture();
}
TEST_CASE("Shared XMsg release changes only synchronized subscription metadata", "[kernel][xmsg_provenance][concurrency]") {
  NativeXmsgProvenanceTestAccess::SharedReleaseHasNoHandleWrite();
}
}  // namespace xe::kernel::test
