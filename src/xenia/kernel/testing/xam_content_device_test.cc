/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include "xenia/kernel/xam/xam_content_device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string_view>
#include <vector>
#include "xenia/base/threading.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_content_storage_source.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

#include "third_party/catch/include/catch.hpp"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/math.h"
#include "xenia/base/media_selection.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"

DECLARE_bool(mount_scratch);
DECLARE_bool(force_mount_devkit);

namespace xe::kernel::xam::test {
namespace {

struct DeviceScratch {
  std::filesystem::path root;
  DeviceScratch() {
    std::random_device rng;
    for (unsigned i = 0; i < 16; ++i) {
      auto candidate = std::filesystem::temp_directory_path() /
                       ("xenia_device_contract_" + std::to_string(rng()) + "_" +
                        std::to_string(rng()));
      if (std::filesystem::create_directory(candidate)) {
        root = std::move(candidate);
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
  }
  ~DeviceScratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  ContentSource Source(std::string id, bool read_only) {
    const auto path = root / id;
    std::filesystem::create_directory(path);
    return {std::move(id),
            1,
            DeviceType::HDD,
            read_only ? ContentSourceKind::kInstalledSystem
                      : ContentSourceKind::kManaged,
            path,
            read_only ? "\\SystemContent" : "\\HDDContent",
            read_only ? u"System files" : u"Hard Drive",
            read_only,
            30,
            read_only ? "" : "\\Device\\Harddisk0\\Partition1"};
  }
};

StorageDeviceInfo Writable(uint32_t id, uint64_t free) {
  StorageDeviceInfo result;
  result.device_id = id;
  result.device_type = DeviceType::HDD;
  result.total_bytes = free;
  result.free_bytes = free;
  result.available = true;
  result.read_only = false;
  return result;
}

}  // namespace

TEST_CASE(
    "A device snapshot owns its identity and does not sum system overlays",
    "[content][storage][devices]") {
  DeviceScratch scratch;
  std::optional<StorageDeviceInfo> retained;
  {
    ContentSourceRegistry registry([](const auto& path) {
      return std::optional(std::filesystem::space_info{
          path.filename() == "system" ? 100u : 500u, 90, 80});
    });
    REQUIRE(registry.Register(scratch.Source("system", true)));
    REQUIRE(registry.Register(scratch.Source("hdd", false)));
    retained = GetStorageDeviceInfo(registry, 1);
    REQUIRE(retained);
    REQUIRE(retained->available);
    REQUIRE_FALSE(retained->read_only);
    REQUIRE(retained->total_bytes == 500);
    REQUIRE(retained->free_bytes == 50);
    REQUIRE_FALSE(GetStorageDeviceInfo(registry, 99));
  }
  REQUIRE(retained->name == u"Hard Drive");
  REQUIRE(retained->guest_root == "\\HDDContent");
  REQUIRE(retained->guest_volume_root == "\\Device\\Harddisk0\\Partition1");
}

TEST_CASE("Volume paths use an explicit mount and native omit-slash semantics",
          "[content][storage][devices]") {
  auto device = Writable(3, 500);
  device.guest_root = "\\Unrelated\\Content";
  REQUIRE_FALSE(GetStorageDeviceVolumePath(device, false));
  device.guest_volume_root = "\\Device\\Mass0";
  REQUIRE(GetStorageDeviceVolumePath(device, false) == "\\Device\\Mass0\\");
  REQUIRE(GetStorageDeviceVolumePath(device, true) == "\\Device\\Mass0");
  device.guest_volume_root = "\\Device\\Mass0\\";
  REQUIRE(GetStorageDeviceVolumePath(device, false) == "\\Device\\Mass0\\");
  REQUIRE(GetStorageDeviceVolumePath(device, true) == "\\Device\\Mass0");
}

TEST_CASE("Selection reobserves disappeared or full writable sources",
          "[content][storage][devices]") {
  DeviceScratch scratch;
  uint64_t available = 80;
  ContentSourceRegistry registry([&](const auto&) {
    return std::optional(std::filesystem::space_info{500, 100, available});
  });
  auto hdd = scratch.Source("hdd", false);
  REQUIRE(registry.Register(hdd));
  const auto old = GetStorageDeviceInfo(registry, 1);
  REQUIRE(old);
  REQUIRE(CanStoreOnDevice(*old, 50));
  available = 79;
  REQUIRE_FALSE(CanStoreOnDevice(*GetStorageDeviceInfo(registry, 1), 50));
  std::filesystem::remove(hdd.root);
  REQUIRE_FALSE(GetStorageDeviceInfo(registry, 1)->available);
  REQUIRE_FALSE(CanStoreOnDevice(*GetStorageDeviceInfo(registry, 1), 0));
}

TEST_CASE("System overlays do not reconnect missing managed devices",
          "[content][storage][devices]") {
  DeviceScratch scratch;
  ContentSourceRegistry registry;
  auto hdd = scratch.Source("hdd", false);
  REQUIRE(registry.Register(hdd));
  REQUIRE(registry.Register(scratch.Source("system", true)));
  std::filesystem::remove(hdd.root);
  REQUIRE(registry.Source("system")->available);
  REQUIRE_FALSE(GetStorageDeviceInfo(registry, 1)->available);

  ContentSourceRegistry readonly_only;
  REQUIRE(readonly_only.Register(scratch.Source("system-only", true)));
  const auto independent = GetStorageDeviceInfo(readonly_only, 1);
  REQUIRE(independent->available);
  REQUIRE(independent->read_only);
  REQUIRE(independent->free_bytes == 0);
  REQUIRE_FALSE(CanStoreOnDevice(*independent, 0));
}

TEST_CASE("Device selection keeps 64-bit space and explicit preference",
          "[content][storage][devices]") {
  constexpr uint64_t kFourGiB = uint64_t{1} << 32;
  std::array devices{Writable(3, kFourGiB + 500), Writable(1, kFourGiB + 300)};
  auto selected = SelectStorageDevice(devices, 0, kFourGiB + 301);
  REQUIRE(selected.kind == StorageSelectionKind::kAutomatic);
  REQUIRE(selected.device_id == 3);
  selected = SelectStorageDevice(devices, 0, kFourGiB + 200, 3);
  REQUIRE(selected.device_id == 3);
  selected = SelectStorageDevice(devices, 0, kFourGiB + 200, 99);
  REQUIRE(selected.device_id == 1);
  selected = SelectStorageDevice(devices, 0, kFourGiB + 501);
  REQUIRE(selected.kind == StorageSelectionKind::kDiskFull);
}

TEST_CASE("Native force and management flags do not accept undersized devices",
          "[content][storage][devices]") {
  const std::array devices{Writable(1, 500)};
  REQUIRE(SelectStorageDevice(devices, 0, 500).kind ==
          StorageSelectionKind::kAutomatic);
  REQUIRE(SelectStorageDevice(devices, 0x200, 0).kind ==
          StorageSelectionKind::kPrompt);
  REQUIRE(SelectStorageDevice(devices, 0x10000, 0).kind ==
          StorageSelectionKind::kPrompt);
  REQUIRE(SelectStorageDevice({}, 0x200, 0).kind ==
          StorageSelectionKind::kPrompt);
  REQUIRE(SelectStorageDevice(devices, 0x100, 500).kind ==
          StorageSelectionKind::kAutomatic);
  REQUIRE(SelectStorageDevice(devices, 0x100, 501).kind ==
          StorageSelectionKind::kPrompt);
  REQUIRE_FALSE(CanStoreOnDevice(devices.front(), 501));
}

TEST_CASE("Unavailable and readonly devices cannot be writable selections",
          "[content][storage][devices]") {
  std::array devices{Writable(2, 1000), Writable(3, 1000)};
  devices[0].read_only = true;
  devices[1].available = false;
  REQUIRE(SelectStorageDevice(devices, 0, 0).kind ==
          StorageSelectionKind::kNoDevice);
  REQUIRE(SelectStorageDevice({}, 0, 0).kind ==
          StorageSelectionKind::kNoDevice);
  REQUIRE(SelectStorageDevice(devices, 0x100, 0).kind ==
          StorageSelectionKind::kPrompt);
}

}  // namespace xe::kernel::xam::test



// Actual export/Kernel dispatch regression fixture. The structural ELF starts
// the real title/dispatch machinery; no guest code or UI is executed by it.
namespace xe::kernel::xam::test {
namespace {
struct SelectorRuntime {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;
  DeviceScratch scratch;
  std::unique_ptr<Emulator> emulator;
  object_ref<UserModule> module;
  SelectorRuntime() {
    REQUIRE(KernelState::shared()==nullptr);
    REQUIRE(std::filesystem::create_directory(scratch.root/"content"));
    REQUIRE(std::filesystem::create_directory(scratch.root/"cache"));
    emulator=std::make_unique<Emulator>("",scratch.root,scratch.root/"content",scratch.root/"cache");
    REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
    // A minimal valid PPC32 big-endian executable, with one real load segment.
    // NullBackend never executes the BLR; SetExecutableModule starts the actual
    // Kernel deferred worker consumed by this production async API.
    std::vector<uint8_t> elf(0x104);
    const auto be16=[&](size_t at,uint16_t v){xe::store_and_swap<uint16_t>(elf.data()+at,v);};
    const auto be32=[&](size_t at,uint32_t v){xe::store_and_swap<uint32_t>(elf.data()+at,v);};
    elf[0]=0x7F;elf[1]='E';elf[2]='L';elf[3]='F';elf[4]=1;elf[5]=2;elf[6]=1;
    be16(16,2);be16(18,20);be32(20,1);be32(24,0x82000000);be32(28,52);
    be16(40,52);be16(42,32);be16(44,1);
    be32(52,1);be32(56,0x100);be32(60,0x82000000);be32(64,0x82000000);
    be32(68,4);be32(72,0x1000);be32(76,5);be32(80,0x1000);be32(0x100,0x4E800020);
    module=object_ref<UserModule>(new UserModule(state()));
    REQUIRE(module->LoadFromMemoryNamed("selector-async-fixture",elf.data(),elf.size())==X_STATUS_SUCCESS);
    state()->SetExecutableModule(module);
  }
  ~SelectorRuntime() { module.reset();emulator.reset(); }
  KernelState* state() const { return emulator->kernel_state(); }
  Memory& memory() const { return *emulator->memory(); }
};
struct SelectorPage {
  Memory& memory;uint32_t base;
  explicit SelectorPage(Memory& value):memory(value),base(memory.SystemHeapAlloc(0x4000,0x1000)) {
    REQUIRE(base);std::memset(memory.TranslateVirtual(base),0,0x4000);
  }
  ~SelectorPage(){memory.SystemHeapFree(base);}
  uint32_t output()const{return base+0x100;}
  uint32_t overlap()const{return base+0x200;}
};
struct SelectorResult {
  uint32_t returned=0xDEADCAFE,wait_status=0xDEADCAFE;
  uint32_t result=0xDEADCAFE,extended=0xDEADCAFE,length=0xDEADCAFE,device=0;
  uint32_t left=0,right=0;
};
struct SelectorBarrier {
  std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;
  void Release(){std::lock_guard lock(mutex);released=true;changed.notify_all();}
};
SelectorResult InvokeSelector(SelectorRuntime& f,SelectorPage& page,uint32_t user,
    uint32_t flags,uint64_t requested,uint32_t output,uint32_t overlap,
    bool retire_before_publication=false, bool retire_before_admission=false,
    bool remove_title_before_publication=false) {
  auto* state=f.state();auto& memory=f.memory();SelectorResult observed;
  std::memset(memory.TranslateVirtual(page.output()-4),0xA5,12);
  std::memset(memory.TranslateVirtual(page.overlap()),0,sizeof(XAM_OVERLAPPED));
  auto event=object_ref<XEvent>(new XEvent(state));event->Initialize(true,false);
  auto* ov=memory.TranslateVirtual<XAM_OVERLAPPED*>(page.overlap());ov->event=event->handle();
  auto barrier=std::make_shared<SelectorBarrier>();
  auto source=state->content_manager()->sources().Source("hdd");
  REQUIRE(source);REQUIRE(source->available);REQUIRE(source->storage);
  if (retire_before_admission) {source->storage->Retire();}
  auto entry=f.emulator->export_resolver()->GetExportByOrdinal("xam.xex",0x2CB);
  REQUIRE(entry);REQUIRE(entry->function_data.trampoline);
  std::mutex drain_mutex;
  const auto drain=[&] {
    // A second ShutdownDispatchThread caller may see running=false before
    // the first join finishes. Serialize all fixture timeout observers so
    // each observes the actual join before guest buffer reads/destruction.
    std::lock_guard lock(drain_mutex);state->ShutdownDispatchThread();
  };
  auto thread=object_ref<XHostThread>(new XHostThread(state,128*1024,0,[&] {
    // The first real dispatch item holds the worker until the actual selector
    // has admitted its source snapshot. This avoids a timing/sleep race.
    if (retire_before_publication || remove_title_before_publication) {
      const auto dummy=page.base+0x400;
      std::memset(memory.TranslateVirtual(dummy),0,sizeof(XAM_OVERLAPPED));
      state->CompleteOverlappedDeferredEx([barrier](uint32_t& extended,uint32_t& length){
        std::unique_lock lock(barrier->mutex);barrier->entered=true;barrier->changed.notify_all();
        if (!barrier->changed.wait_for(lock,std::chrono::seconds(3),[&]{return barrier->released;})) {
          extended=X_HRESULT_FROM_WIN32(X_RESULT_FROM_WIN32(1460));length=0;return X_RESULT_FROM_WIN32(1460);
        }
        extended=0;length=0;return X_ERROR_SUCCESS;
      },dummy);
      std::unique_lock lock(barrier->mutex);
      if (!barrier->changed.wait_for(lock,std::chrono::seconds(3),[&]{return barrier->entered;})) {
        lock.unlock();barrier->Release();drain();return 1;
      }
    }
    auto* context=XThread::GetCurrentThread()->thread_state()->context();
    context->r[3]=user;context->r[4]=uint32_t(XContentType::kSavedGame);context->r[5]=flags;
    context->r[6]=requested;context->r[7]=output;context->r[8]=overlap;
    entry->function_data.trampoline(context);observed.returned=uint32_t(context->r[3]);
    if (retire_before_publication) {source->storage->Retire();}
    if (remove_title_before_publication) {state->SetExecutableModule(nullptr);}
    if (retire_before_publication || remove_title_before_publication) {barrier->Release();}
    if (observed.returned==X_ERROR_IO_PENDING) {
      uint64_t timeout=uint64_t(-5LL*10000000);observed.wait_status=event->Wait(0,0,0,&timeout);
      if (observed.wait_status!=X_STATUS_SUCCESS) {
        // A failed wait is not proof that the producer stopped using guest
        // buffers. Join before reading them or unwinding their owner.
        drain();
      }
    }
    observed.result=XOverlappedGetResult(ov);observed.extended=uint32_t(ov->extended_error);
    observed.length=XOverlappedGetLength(ov);
    observed.device=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.output()));
    observed.left=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.output()-4));
    observed.right=xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.output()+4));
    return 0;
  },state->GetTitleProcess()));
  REQUIRE(thread->Create()==X_STATUS_SUCCESS);
  const auto waited=xe::threading::Wait(thread->thread(),false,std::chrono::seconds(10));
  barrier->Release();
  if (waited!=xe::threading::WaitResult::kSuccess) {
    // Keep all guest buffers alive while the actual dispatch worker drains;
    // never throw past a live producer retaining references to this stack.
    drain();
    if (xe::threading::Wait(thread->thread(),false,std::chrono::seconds(10))!=
        xe::threading::WaitResult::kSuccess) {
      // Never unwind stack references under an unjoined host producer.
      std::terminate();
    }
  }
  REQUIRE(waited==xe::threading::WaitResult::kSuccess);
  return observed;
}
}

TEST_CASE("Actual selector export completes the BO1 one MiB request without native UI",
          "[kernel][storage][selector][async]") {
  SelectorRuntime f;SelectorPage page(f.memory());
  REQUIRE(f.emulator->display_window()==nullptr);
  const auto answer=InvokeSelector(f,page,0,0,0x100000,page.output(),page.overlap());
  REQUIRE(answer.returned==X_ERROR_IO_PENDING);REQUIRE(answer.wait_status==X_STATUS_SUCCESS);
  REQUIRE(answer.result==X_ERROR_SUCCESS);REQUIRE(answer.extended==X_HRESULT_FROM_WIN32(X_ERROR_SUCCESS));
  REQUIRE(answer.length==0);REQUIRE(answer.device==1);
  REQUIRE(answer.left==0xA5A5A5A5);REQUIRE(answer.right==0xA5A5A5A5);
  REQUIRE_FALSE(f.state()->xam_state()->IsUIActive());
}
TEST_CASE("Actual selector export preserves full64 requested space and truthful disk full completion",
          "[kernel][storage][selector][async][capacity]") {
  SelectorRuntime f;SelectorPage page(f.memory());
  const auto answer=InvokeSelector(f,page,XUserIndexAny,0,UINT64_MAX-0x1000,page.output(),page.overlap());
  REQUIRE(answer.returned==X_ERROR_IO_PENDING);REQUIRE(answer.wait_status==X_STATUS_SUCCESS);
  REQUIRE(answer.result==X_RESULT_FROM_WIN32(0x70));REQUIRE(answer.extended==X_HRESULT_FROM_WIN32(answer.result));
  REQUIRE(answer.length==0);REQUIRE(answer.device==0xA5A5A5A5);
  REQUIRE(answer.left==0xA5A5A5A5);REQUIRE(answer.right==0xA5A5A5A5);
}
TEST_CASE("Actual selector async publication refuses a withdrawn selected source without output writes",
          "[kernel][storage][selector][async][lifetime]") {
  SelectorRuntime f;SelectorPage page(f.memory());
  const auto answer=InvokeSelector(f,page,0,0,0,page.output(),page.overlap(),true);
  REQUIRE(answer.returned==X_ERROR_IO_PENDING);REQUIRE(answer.wait_status==X_STATUS_SUCCESS);
  REQUIRE(answer.result==X_ERROR_DEVICE_NOT_CONNECTED);REQUIRE(answer.extended==X_HRESULT_FROM_WIN32(answer.result));
  REQUIRE(answer.device==0xA5A5A5A5);REQUIRE(answer.left==0xA5A5A5A5);REQUIRE(answer.right==0xA5A5A5A5);
}
TEST_CASE("Actual selector export validates guest ranges user flags and mandatory overlap before selection",
          "[kernel][storage][selector][abi]") {
  SelectorRuntime f;SelectorPage page(f.memory());
  for (const auto bad_user:std::array<uint32_t,2>{4,0xFFFFFFFE}) {
    const auto answer=InvokeSelector(f,page,bad_user,0,0,page.output(),page.overlap());
    REQUIRE(answer.returned==X_ERROR_INVALID_PARAMETER);REQUIRE(answer.device==0xA5A5A5A5);
  }
  for (const auto flag:std::array<uint32_t,2>{8,0x80000000}) {
    const auto answer=InvokeSelector(f,page,0,flag,0,page.output(),page.overlap());
    REQUIRE(answer.returned==X_ERROR_INVALID_PARAMETER);REQUIRE(answer.device==0xA5A5A5A5);
  }
  for (const auto where:std::array<std::pair<uint32_t,uint32_t>,3>{{{0,page.overlap()},{page.output(),0},{page.output(),UINT32_MAX}}}) {
    const auto answer=InvokeSelector(f,page,0,0,0,where.first,where.second);
    REQUIRE(answer.returned==X_ERROR_INVALID_PARAMETER);REQUIRE(answer.device==0xA5A5A5A5);
  }
  auto* heap=f.memory().LookupHeap(page.base);
  REQUIRE(heap);
  const auto read_only_output=page.base+0x1100;
  std::memset(f.memory().TranslateVirtual(read_only_output-4),0x5A,12);
  REQUIRE(heap->Protect(page.base+0x1000,0x1000,kMemoryProtectRead));
  const auto answer=InvokeSelector(f,page,0,0,0,page.base+0x1100,page.overlap());
  REQUIRE(answer.returned==X_ERROR_INVALID_PARAMETER);REQUIRE(answer.device==0xA5A5A5A5);
  for (uint32_t i=0;i<12;++i) {
    REQUIRE(*f.memory().TranslateVirtual<uint8_t*>(read_only_output-4+i)==0x5A);
  }
  const auto bad_overlap=InvokeSelector(f,page,0,0,0,page.output(),page.base+0x1200);
  REQUIRE(bad_overlap.returned==X_ERROR_INVALID_PARAMETER);
  REQUIRE(bad_overlap.device==0xA5A5A5A5);
  REQUIRE(heap->Protect(page.base+0x1000,0x1000,kMemoryProtectRead|kMemoryProtectWrite));
}
TEST_CASE("Actual selector no-device completion remains asynchronous without output writes",
          "[kernel][storage][selector][async]") {
  SelectorRuntime f;SelectorPage page(f.memory());
  const auto answer=InvokeSelector(f,page,0,0,0,page.output(),page.overlap(),false,true);
  REQUIRE(answer.returned==X_ERROR_IO_PENDING);REQUIRE(answer.wait_status==X_STATUS_SUCCESS);
  REQUIRE(answer.result==X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(answer.extended==X_HRESULT_FROM_WIN32(answer.result));
  REQUIRE(answer.device==0xA5A5A5A5);REQUIRE(answer.left==0xA5A5A5A5);REQUIRE(answer.right==0xA5A5A5A5);
}
TEST_CASE("Actual selector queued completion cancels a superseded retained title",
          "[kernel][storage][selector][async][lifetime]") {
  SelectorRuntime f;SelectorPage page(f.memory());
  const auto answer=InvokeSelector(f,page,0,0,0x100000,page.output(),page.overlap(),false,false,true);
  REQUIRE(answer.returned==X_ERROR_IO_PENDING);REQUIRE(answer.wait_status==X_STATUS_SUCCESS);
  REQUIRE(answer.result==X_ERROR_CANCELLED);
  REQUIRE(answer.extended==X_HRESULT_FROM_WIN32(answer.result));
  REQUIRE(answer.device==0xA5A5A5A5);REQUIRE(answer.left==0xA5A5A5A5);REQUIRE(answer.right==0xA5A5A5A5);
}
TEST_CASE("Actual selector forced prompt remains a real UI request in headless runtime",
          "[kernel][storage][selector][ui]") {
  SelectorRuntime f;SelectorPage page(f.memory());
  for (const auto flags:std::array<uint32_t,2>{0x200,0x10000}) {
    const auto answer=InvokeSelector(f,page,0,flags,0,page.output(),page.overlap());
    REQUIRE(answer.returned==X_ERROR_FUNCTION_FAILED);REQUIRE(answer.device==0xA5A5A5A5);
    REQUIRE_FALSE(f.state()->xam_state()->IsUIActive());
  }
}
}  // namespace xe::kernel::xam::test

namespace xe::kernel::xam::test {
namespace {

XCONTENT_AGGREGATE_DATA MountWireSave(SelectorRuntime& runtime,
    std::string_view root, std::string_view leaf, uint32_t device = 1) {
  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = device; data.title_id = 0x12345678;
  data.content_type = XContentType::kSavedGame;
  data.set_file_name(leaf); data.set_display_name(u"Wire fixture");
  REQUIRE(runtime.state()->content_manager()->CreateContent(root, 1, data) == X_ERROR_SUCCESS);
  vfs::File* file = nullptr; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,
      std::string(root) + ":\\marker.bin", vfs::FileDisposition::kCreate,
      vfs::FileAccess::kGenericWrite, false, true, &file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file);
  const std::array<uint8_t, 3> payload{uint8_t(device), 0, 0xFF};
  size_t written = 0;
  const auto status = file->WriteSync(payload, 0, &written);
  const auto flushed = file->Flush(); file->Destroy();
  REQUIRE(status == X_STATUS_SUCCESS); REQUIRE(written == payload.size());
  REQUIRE(flushed == X_STATUS_SUCCESS);
  return data;
}

void RegisterWireUsb(SelectorRuntime& runtime) {
  const auto root = runtime.scratch.root / "usb";
  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(runtime.state()->content_manager()->RegisterManagedSource(
      {"wire-usb", 3, DeviceType::USBMASS, ContentSourceKind::kManaged,
       root, "\\WireUSBContent", u"USB", false, 0, "\\WireUSB", {}, runtime.scratch.root}));
}

uint32_t WireWord(SelectorPage& page, uint32_t address) {
  return xe::load_and_swap<uint32_t>(page.memory.TranslateVirtual(address));
}
void RequireWireMarker(SelectorPage& page, uint32_t address, uint32_t length) {
  for (uint32_t i = 0; i != length; ++i) {
    REQUIRE(*page.memory.TranslateVirtual<uint8_t*>(address + i) == 0xA5);
  }
}

uint32_t QueryWireVolume(SelectorRuntime& runtime, SelectorPage& page,
    std::string_view root, uint32_t flag, uint32_t length = 0x4C,
    uint32_t destination = 0) {
  REQUIRE(root.size() < 64);
  const auto packet = page.base + 0x500;
  if (!destination) { destination = page.base + 0x600; }
  std::memset(page.memory.TranslateVirtual(packet), 0, 0x4C);
  std::memcpy(page.memory.TranslateVirtual(packet), root.data(), root.size());
  xe::store_and_swap<uint32_t>(page.memory.TranslateVirtual(packet + 0x40), flag);
  xe::store_and_swap<uint32_t>(page.memory.TranslateVirtual(packet + 0x44), destination);
  // The real wire has an overlapped pointer at +0x48; this synchronous query
  // neither treats it as output nor writes a completion record there.
  xe::store_and_swap<uint32_t>(page.memory.TranslateVirtual(packet + 0x48), 0xDEADCAFE);
  return runtime.state()->app_manager()->DispatchMessageSync(0xFE, 0x20021, packet, length);
}

struct VolumeWireFile {
  object_ref<XFile> file;
  VolumeWireFile(SelectorRuntime& runtime, std::string_view path) {
    vfs::File* opened = nullptr; vfs::FileAction action{};
    REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr, path,
        vfs::FileDisposition::kOpen, vfs::FileAccess::kGenericRead,
        false, true, &opened, &action) == X_STATUS_SUCCESS);
    REQUIRE(opened);
    file = object_ref<XFile>(new XFile(runtime.state(), opened, true, false, true));
  }
  ~VolumeWireFile() { file->ReleaseHandle(); file.reset(); }
};
uint32_t QueryNtWireVolume(SelectorRuntime& runtime, SelectorPage& page,
    uint32_t handle, uint32_t iosb, uint32_t output, uint32_t length) {
  const auto* entry = runtime.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe", 0xEF);
  REQUIRE(entry); REQUIRE(entry->is_implemented()); REQUIRE(entry->function_data.trampoline);
  cpu::ppc::PPCContext context{};
  context.kernel_state = runtime.state();
  context.virtual_membase = page.memory.virtual_membase();
  context.r[1] = page.base + 0x3800;
  const std::array<uint32_t, 5> arguments{handle, iosb, output, length, 4};
  for (size_t i = 0; i != arguments.size(); ++i) {
    context.r[3 + i] = 0xAABBCCDD00000000ull | arguments[i];
  }
  entry->function_data.trampoline(&context);
  return uint32_t(context.r[3]);
}

// A real bounded PIRS/STFS file, not a fake success provider. An outer package
// stores another complete package, so the second mount uses an immutable
// retained STFS Entry/File instead of reopening a host pathname.
std::vector<uint8_t> WireStfs(std::string_view name, std::span<const uint8_t> payload) {
  constexpr size_t block = 0x1000;
  auto header = std::make_unique<vfs::XContentContainerHeader>();
  header->content_header.magic = vfs::XContentPackageType::kPirs;
  header->content_header.header_size = sizeof(*header);
  auto& metadata = header->content_metadata;
  metadata.volume_type = vfs::XContentVolumeType::kStfs;
  metadata.content_type = XContentType::kXbox360Title;
  metadata.execution_info.title_id = 0x12345678;
  auto& volume = metadata.volume_descriptor.stfs;
  volume.descriptor_length = sizeof(vfs::StfsVolumeDescriptor);
  volume.flags.bits.read_only_format = true;
  volume.file_table_block_count = 1; volume.set_file_table_block_number(0);
  const auto blocks = uint32_t((payload.size() + block - 1) / block);
  vfs::StfsHashTable hash{}; vfs::StfsDirectoryBlock directory{};
  REQUIRE(blocks + 1 < std::size(hash.entries));
  REQUIRE(name.size() <= sizeof(directory.entries[0].name));
  volume.total_block_count = blocks + 1;
  for (uint32_t i = 0; i != blocks + 1; ++i) {
    hash.entries[i].set_level0_next_block(0xFFFFFF);
  }
  auto& entry = directory.entries[0];
  std::memcpy(entry.name, name.data(), name.size());
  entry.flags.name_length = uint8_t(name.size()); entry.flags.contiguous = true;
  entry.set_valid_data_blocks(blocks); entry.set_allocated_data_blocks(blocks);
  entry.set_start_block_number(1); entry.directory_index = 0xFFFF;
  entry.length = uint32_t(payload.size());
  const auto base = xe::round_up(sizeof(*header), block);
  std::vector<uint8_t> bytes(base + (blocks + 2) * block);
  std::memcpy(bytes.data(), header.get(), sizeof(*header));
  std::memcpy(bytes.data() + base, &hash, sizeof(hash));
  std::memcpy(bytes.data() + base + block, &directory, sizeof(directory));
  std::memcpy(bytes.data() + base + 2 * block, payload.data(), payload.size());
  return bytes;
}

void InstallWireNestedPackages(SelectorRuntime& runtime) {
  MountWireSave(runtime, "Host", "wire-packages");
  const std::array<uint8_t, 4> marker{0x53, 0, 0x80, 0xFF};
  const auto inner = WireStfs("payload", marker);
  const auto outer = WireStfs("child", inner);
  vfs::File* file = nullptr; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr, "Host:\\outer.stfs",
      vfs::FileDisposition::kCreate, vfs::FileAccess::kGenericWrite,
      false, true, &file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file); size_t written = 0;
  const auto status = file->WriteSync(outer, 0, &written);
  const auto flushed = file->Flush(); file->Destroy();
  REQUIRE(status == X_STATUS_SUCCESS); REQUIRE(written == outer.size());
  REQUIRE(flushed == X_STATUS_SUCCESS);
}

}  // namespace

TEST_CASE("Kernel file opens and attributes keep the original caller DOS namespace",
          "[kernel][content][namespace][file]") {
  SelectorRuntime runtime; SelectorPage page(runtime.memory());
  RegisterWireUsb(runtime);
  MountWireSave(runtime, "\\System??\\Same", "system-hdd");
  MountWireSave(runtime, "\\??\\Same", "title-usb", 3);
  auto* fs=runtime.emulator->file_system();
  const auto system=fs->AcquirePathDevice("\\System??\\Same:");
  const auto title=fs->AcquirePathDevice("Same:");
  REQUIRE(system); REQUIRE(title); REQUIRE(system!=title);
  REQUIRE(fs->RegisterSymbolicLink("OnlyTitle:",title->mount_path()));
  vfs::File* raw_directory=nullptr; vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr,system->mount_path(),vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead,true,false,&raw_directory,&action)==X_STATUS_SUCCESS);
  auto directory=object_ref<XFile>(new XFile(runtime.state(),raw_directory,true,false,true));
  const auto* open=runtime.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",0xDF);
  const auto* query=runtime.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",0xE7);
  const auto* close=runtime.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",0xCF);
  const auto* link_open=runtime.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",0xE0);
  const auto* link_query=runtime.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",0xEC);
  std::string system_target,title_target;
  REQUIRE(fs->FindSymbolicLink("\\System??\\Same:",system_target));
  REQUIRE(fs->FindSymbolicLink("Same:",title_target));
  REQUIRE(system_target!=title_target);
  for(const auto* entry:{open,query,close,link_open,link_query}){REQUIRE(entry);REQUIRE(entry->is_implemented());
    REQUIRE(entry->function_data.trampoline);}
  struct Case {std::string name;uint32_t root;uint8_t expected;bool query;uint32_t invalid=0;};
  struct Result {uint32_t opened=0xDEADCAFE,handle=0,queried=0xDEADCAFE,closed=0xDEADCAFE;
    X_STATUS read=X_STATUS_UNSUCCESSFUL;size_t count=0;uint8_t byte=0;uint64_t size=0;};
  for(const bool caller_system:{false,true}) {
    const uint8_t expected=caller_system?1:3;
    const std::array<Case,17> cases{{
      {"Same:\\marker.bin",0,expected,true},
      {"Same:\\marker.bin",0xFFFFFFFDu,expected,true},
      {"\\??\\Same:\\marker.bin",0,3,true},
      {"\\System??\\Same:\\marker.bin",0,1,true},
      {system->mount_path()+"\\marker.bin",0,1,true},
      {"marker.bin",directory->handle(),1,false},
      {"OnlyTitle:\\marker.bin",0xFFFFFFFDu,uint8_t(caller_system?0:3),true},
      {"\\sYsTeM??\\Same:\\marker.bin",0xFFFFFFFDu,1,true},
      {"\\dOsDeViCeS\\Same:\\marker.bin",0,3,true},
      {"\\System??\\Same:\\mark?.bin",0,0,true,X_STATUS_OBJECT_NAME_INVALID},
      {"\\System??\\Same:\\mark*.bin",0xFFFFFFFDu,0,true,X_STATUS_OBJECT_NAME_INVALID},
      {std::string("\\System??\\Same:\\mark")+char(0x7F)+".bin",0,0,true,X_STATUS_OBJECT_NAME_INVALID},
      {"\\Unknown??\\Same:\\marker.bin",0,0,true,X_STATUS_OBJECT_NAME_INVALID},
      {"\\System???\\Same:\\marker.bin",0,0,true,X_STATUS_OBJECT_NAME_INVALID},
      {std::string(1,char(0xFF))+"Same:\\marker.bin",0,0,true,X_STATUS_OBJECT_NAME_INVALID},
      {"mark?.bin",directory->handle(),0,false,X_STATUS_OBJECT_NAME_INVALID},
      {"\\System??\\Same:\\marker.bin",directory->handle(),0,false,X_STATUS_OBJECT_NAME_INVALID}}};
    for(const auto& c:cases)REQUIRE(c.name.size()<128);
    std::array<Result,17> results{};
    struct LinkCase {std::string name;uint32_t root;std::string target;uint32_t invalid=0;};
    const std::vector<LinkCase> links{
      {"Same:",0,caller_system?system_target:title_target},
      {"Same:",0xFFFFFFFDu,caller_system?system_target:title_target},
      {"\\??\\Same:",0,title_target},
      {"\\sYsTeM??\\Same:",0xFFFFFFFDu,system_target},
      {"\\Unknown??\\Same:",0,{},X_STATUS_OBJECT_NAME_INVALID},
      {"\\System??\\Sa?e:",0,{},X_STATUS_OBJECT_NAME_INVALID},
      {std::string("\\System??\\Same:")+char(0x7F),0,{},X_STATUS_OBJECT_NAME_INVALID},
      {std::string(1,char(0xFF))+"Same:",0,{},X_STATUS_OBJECT_NAME_INVALID},
      {"\\System??\\Same:",directory->handle(),{},X_STATUS_OBJECT_NAME_INVALID}};
    struct LinkResult {uint32_t opened=0xDEADCAFE,handle=0,queried=0xDEADCAFE,closed=0xDEADCAFE;std::string target;};
    std::vector<LinkResult> link_results(links.size());
    auto thread=object_ref<XHostThread>(new XHostThread(runtime.state(),128*1024,0,[&]{
      auto* context=XThread::GetCurrentThread()->thread_state()->context();
      for(size_t i=0;i<cases.size();++i) {
        const auto& c=cases[i];auto& r=results[i];
        const auto attributes=page.base+0x800,ansi=page.base+0x820,text=page.base+0x840;
        const auto output=page.base+0x900,iosb=page.base+0x920,info=page.base+0x940;
        auto* attrs=runtime.memory().TranslateVirtual<X_OBJECT_ATTRIBUTES*>(attributes);
        auto* name=runtime.memory().TranslateVirtual<X_ANSI_STRING*>(ansi);
        attrs->root_directory=c.root;attrs->name_ptr=ansi;attrs->attributes=0x40;
        name->length=uint16_t(c.name.size());name->maximum_length=uint16_t(c.name.size()+1);
        name->pointer=text;std::memcpy(runtime.memory().TranslateVirtual(text),c.name.c_str(),c.name.size()+1);
        // An early parameter/name failure need not overwrite the output.
        xe::store_and_swap<uint32_t>(runtime.memory().TranslateVirtual(output),0);
        context->r[3]=output;context->r[4]=0x00100001;context->r[5]=attributes;
        context->r[6]=iosb;context->r[7]=1;context->r[8]=0x60;
        open->function_data.trampoline(context);r.opened=uint32_t(context->r[3]);r.handle=WireWord(page,output);
        if(XSUCCEEDED(r.opened) && r.handle) {
          {const auto file=runtime.state()->object_table()->LookupObject<XFile>(r.handle);
            if(file){std::array<uint8_t,3> bytes{};r.read=file->file()->ReadSync(bytes,0,&r.count);r.byte=bytes[0];}}
          context->r[3]=r.handle;close->function_data.trampoline(context);r.closed=uint32_t(context->r[3]);
        }
        if(c.query) {
          std::memset(runtime.memory().TranslateVirtual(info),0,sizeof(X_FILE_NETWORK_OPEN_INFORMATION));
          context->r[3]=attributes;context->r[4]=info;query->function_data.trampoline(context);
          r.queried=uint32_t(context->r[3]);
          if(XSUCCEEDED(r.queried))r.size=runtime.memory().TranslateVirtual<X_FILE_NETWORK_OPEN_INFORMATION*>(info)->end_of_file;
        }
      }
      for(size_t i=0;i<links.size();++i) {
        const auto& c=links[i];auto& r=link_results[i];
        const auto attributes=page.base+0x800,ansi=page.base+0x820,text=page.base+0x840;
        const auto output=page.base+0x900,target_descriptor=page.base+0xA00,target_bytes=page.base+0xA20;
        auto* attrs=runtime.memory().TranslateVirtual<X_OBJECT_ATTRIBUTES*>(attributes);
        auto* name=runtime.memory().TranslateVirtual<X_ANSI_STRING*>(ansi);
        attrs->root_directory=c.root;attrs->name_ptr=ansi;attrs->attributes=0x40;
        name->length=uint16_t(c.name.size());name->maximum_length=uint16_t(c.name.size()+1);
        name->pointer=text;std::memcpy(runtime.memory().TranslateVirtual(text),c.name.c_str(),c.name.size()+1);
        xe::store_and_swap<uint32_t>(runtime.memory().TranslateVirtual(output),0);
        context->r[3]=output;context->r[4]=attributes;link_open->function_data.trampoline(context);
        r.opened=uint32_t(context->r[3]);r.handle=WireWord(page,output);
        if(XSUCCEEDED(r.opened) && r.handle) {
          auto* target=runtime.memory().TranslateVirtual<X_ANSI_STRING*>(target_descriptor);
          target->length=0;target->maximum_length=0x100;target->pointer=target_bytes;
          context->r[3]=r.handle;context->r[4]=target_descriptor;link_query->function_data.trampoline(context);
          r.queried=uint32_t(context->r[3]);
          if(XSUCCEEDED(r.queried) && target->length<=0x100)
            r.target.assign(runtime.memory().TranslateVirtual<const char*>(target_bytes),target->length);
          context->r[3]=r.handle;close->function_data.trampoline(context);r.closed=uint32_t(context->r[3]);
        }
      }
      return 0;
    },caller_system?runtime.state()->GetSystemProcess():runtime.state()->GetTitleProcess()));
    REQUIRE(thread->Create()==X_STATUS_SUCCESS);
    const auto waited=xe::threading::Wait(thread->thread(),false,std::chrono::seconds(10));
    if(waited!=xe::threading::WaitResult::kSuccess)std::terminate(); // Never unwind a live fixture borrower.
    for(size_t i=0;i<cases.size();++i) {
      CAPTURE(caller_system,cases[i].name,cases[i].root);const auto& r=results[i];
      if(cases[i].expected){CHECK(r.opened==X_STATUS_SUCCESS);CHECK(r.handle!=0);
        CHECK(r.read==X_STATUS_SUCCESS);CHECK(r.count==3);CHECK(r.byte==cases[i].expected);
        CHECK(r.closed==X_STATUS_SUCCESS);if(cases[i].query){CHECK(r.queried==X_STATUS_SUCCESS);CHECK(r.size==3);}}
      else{CHECK(XFAILED(r.opened));CHECK(r.handle==0);if(cases[i].query)CHECK(XFAILED(r.queried));}
      if(cases[i].invalid){CHECK(r.opened==cases[i].invalid);if(cases[i].query)CHECK(r.queried==cases[i].invalid);}
    }
    for(size_t i=0;i<links.size();++i) {
      CAPTURE(caller_system,links[i].name,links[i].root);const auto& c=links[i];const auto& r=link_results[i];
      if(c.invalid){CHECK(r.opened==c.invalid);CHECK(r.handle==0);}
      else{CHECK(r.opened==X_STATUS_SUCCESS);CHECK(r.handle!=0);CHECK(r.queried==X_STATUS_SUCCESS);
        CHECK(r.target==c.target);CHECK(r.closed==X_STATUS_SUCCESS);}
    }
  }
  directory->ReleaseHandle();directory.reset();
  REQUIRE(fs->UnregisterSymbolicLink("OnlyTitle:"));
  REQUIRE(runtime.state()->content_manager()->CloseContent("\\System??\\Same",system.get())==X_ERROR_SUCCESS);
  REQUIRE(runtime.state()->content_manager()->CloseContent("Same",title.get())==X_ERROR_SUCCESS);
}

TEST_CASE("Captured SYSTEM file names retain their namespace on a host I/O worker",
          "[kernel][content][namespace][host_io]") {
  SelectorRuntime runtime;
  RegisterWireUsb(runtime);
  MountWireSave(runtime,"\\System??\\Same","system-deferred");
  MountWireSave(runtime,"Same","title-deferred",3);
  std::string captured="Same:\\marker.bin";
  auto caller=object_ref<XHostThread>(new XHostThread(runtime.state(),128*1024,0,[&]{
    xboxkrnl::xeObCaptureFilePathForCaller(captured,0xFFFFFFFDu);return 0;
  },runtime.state()->GetSystemProcess()));
  REQUIRE(caller->Create()==X_STATUS_SUCCESS);
  if(xe::threading::Wait(caller->thread(),false,std::chrono::seconds(10))!=xe::threading::WaitResult::kSuccess)
    std::terminate();
  bool no_guest_thread=false;std::array<uint32_t,2> statuses{};
  std::array<uint8_t,2> selected{};std::array<size_t,2> counts{};
  std::thread io([&]{
    no_guest_thread=!XThread::IsInThread();
    std::array<std::string,2> paths{captured,"Same:\\marker.bin"};
    // The production capture also accepts a genuine host caller without
    // asserting or promoting its bare alias into the SYSTEM namespace.
    for(auto& path:paths)xboxkrnl::xeObCaptureFilePathForCaller(path,0xFFFFFFFDu);
    for(size_t i=0;i<paths.size();++i){vfs::File* file=nullptr;vfs::FileAction action{};
      statuses[i]=runtime.emulator->file_system()->OpenFile(nullptr,paths[i],vfs::FileDisposition::kOpen,
          vfs::FileAccess::kFileReadData,false,true,&file,&action);
      if(file){std::array<uint8_t,3> bytes{};statuses[i]=file->ReadSync(bytes,0,&counts[i]);
        selected[i]=bytes[0];file->Destroy();}}
  });io.join();
  CHECK(no_guest_thread);CHECK(statuses[0]==X_STATUS_SUCCESS);CHECK(statuses[1]==X_STATUS_SUCCESS);
  CHECK(counts[0]==3);CHECK(counts[1]==3);CHECK(selected[0]==1);CHECK(selected[1]==3);
  REQUIRE(runtime.state()->content_manager()->CloseContent("\\System??\\Same")==X_ERROR_SUCCESS);
  REQUIRE(runtime.state()->content_manager()->CloseContent("Same")==X_ERROR_SUCCESS);
}

TEST_CASE("Native FE volume queries follow actual source and independent System and Title aliases",
          "[kernel][content][storage][devices][volume][abi]") {
  SelectorRuntime runtime; SelectorPage page(runtime.memory());
  RegisterWireUsb(runtime);
  runtime.state()->deployment_type_ = XDeploymentType::kOpticalDisc;
  MountWireSave(runtime, "\\System??\\Same", "system-hdd");
  MountWireSave(runtime, "\\??\\Same", "title-usb", 3);
  const auto system = runtime.emulator->file_system()->AcquirePathDevice("\\System??\\Same:");
  const auto title = runtime.emulator->file_system()->AcquirePathDevice("\\??\\Same:");
  REQUIRE(system); REQUIRE(title); REQUIRE(system != title);
  REQUIRE(system->storage_volume()); REQUIRE(title->storage_volume());
  REQUIRE(system->storage_volume()->source_id() == "hdd");
  REQUIRE(title->storage_volume()->source_id() == "wire-usb");
  for (const auto& item : std::array<std::pair<std::string_view, uint32_t>, 6>{{
      {"Same", 0}, {"Same", 1}, {"Same", 0x80000000},
      {"\\System??\\Same", 1}, {"\\??\\Same", 0}, {"sAmE", 0xFFFFFFFF}}}) {
    std::memset(page.memory.TranslateVirtual(page.base + 0x5FC), 0xA5, 12);
    const bool hdd = (item.second == 0 && item.first == "Same") ||
                     item.first == "\\System??\\Same";
    REQUIRE(QueryWireVolume(runtime, page, item.first, item.second) == X_E_SUCCESS);
    REQUIRE(WireWord(page, page.base + 0x600) == (hdd ? 1 : 16));
    RequireWireMarker(page, page.base + 0x5FC, 4);
    RequireWireMarker(page, page.base + 0x604, 4);
  }
  for (const auto& bad_root : {"", "Same:", "Missing"}) {
    std::memset(page.memory.TranslateVirtual(page.base + 0x5FC), 0xA5, 12);
    REQUIRE(QueryWireVolume(runtime, page, bad_root, 1) == X_E_INVALIDARG);
    RequireWireMarker(page, page.base + 0x5FC, 12);
  }
  REQUIRE(QueryWireVolume(runtime, page, "Same", 1, 0x48) == X_E_INVALIDARG);
  RequireWireMarker(page, page.base + 0x5FC, 12);
  // Bare VFS is Title, while explicit System names preserve their own root.
  REQUIRE(runtime.emulator->file_system()->AcquirePathDevice("Same:") == title);
  REQUIRE(runtime.state()->content_manager()->CloseContent("Same", title.get()) == X_ERROR_SUCCESS);
  REQUIRE(runtime.emulator->file_system()->AcquirePathDevice("\\System??\\Same:") == system);
  std::memset(page.memory.TranslateVirtual(page.base + 0x5FC), 0xA5, 12);
  REQUIRE(QueryWireVolume(runtime, page, "Same", 1) == X_E_INVALIDARG);
  RequireWireMarker(page, page.base + 0x5FC, 12);
  REQUIRE(QueryWireVolume(runtime, page, "Same", 0) == X_E_SUCCESS);
  REQUIRE(WireWord(page, page.base + 0x600) == 1);
}

TEST_CASE("Nt volume class four matches FE source type and publishes exactly eight guarded bytes",
          "[kernel][content][storage][devices][volume][abi]") {
  SelectorRuntime runtime; SelectorPage page(runtime.memory());
  RegisterWireUsb(runtime);
  runtime.state()->deployment_type_ = XDeploymentType::kOpticalDisc;
  MountWireSave(runtime, "HddVolume", "hdd-volume");
  MountWireSave(runtime, "UsbVolume", "usb-volume", 3);
  const auto output = page.base + 0x800, iosb = page.base + 0x880;
  for (const auto& item : std::array<std::pair<std::string_view, uint32_t>, 2>{{
      {"HddVolume", 7}, {"UsbVolume", 0x2D}}}) {
    VolumeWireFile file(runtime, std::string(item.first) + ":\\marker.bin");
    std::memset(page.memory.TranslateVirtual(output - 4), 0xA5, 24);
    std::memset(page.memory.TranslateVirtual(iosb - 4), 0xA5, 16);
    REQUIRE(QueryNtWireVolume(runtime, page, file.file->handle(), iosb, output, 8) == X_STATUS_SUCCESS);
    REQUIRE(WireWord(page, output) == item.second);
    REQUIRE(WireWord(page, output + 4) == 2);
    REQUIRE(WireWord(page, iosb) == X_STATUS_SUCCESS);
    REQUIRE(WireWord(page, iosb + 4) == 8);
    RequireWireMarker(page, output - 4, 4); RequireWireMarker(page, output + 8, 12);
    RequireWireMarker(page, iosb - 4, 4); RequireWireMarker(page, iosb + 8, 4);
    REQUIRE(QueryWireVolume(runtime, page, item.first, 1) == X_E_SUCCESS);
    REQUIRE(WireWord(page, page.base + 0x600) == (item.second == 7 ? 1 : 16));
    REQUIRE(file.file->file()->retained_device()->storage_volume() ==
        runtime.emulator->file_system()->AcquirePathDevice(std::string(item.first) + ":")->storage_volume());

    for (const auto bad_length : {0u, 7u}) {
      std::memset(page.memory.TranslateVirtual(output - 4), 0xA5, 24);
      std::memset(page.memory.TranslateVirtual(iosb - 4), 0xA5, 16);
      REQUIRE(QueryNtWireVolume(runtime, page, file.file->handle(), iosb, output, bad_length) == X_STATUS_INFO_LENGTH_MISMATCH);
      RequireWireMarker(page, output - 4, 24); RequireWireMarker(page, iosb - 4, 16);
    }
    // Overlapping IOSB and data are refused before either output is changed.
    REQUIRE(QueryNtWireVolume(runtime, page, file.file->handle(), output + 4, output, 8) == X_STATUS_INVALID_PARAMETER);
    RequireWireMarker(page, output - 4, 24);
    const auto readonly_iosb = page.base + 0x1100;
    std::memset(page.memory.TranslateVirtual(readonly_iosb), 0xA5, 8);
    auto* heap = page.memory.LookupHeap(readonly_iosb); REQUIRE(heap);
    REQUIRE(heap->Protect(page.base + 0x1000, 0x1000, kMemoryProtectRead));
    REQUIRE(QueryNtWireVolume(runtime, page, file.file->handle(), readonly_iosb, output, 8) == X_STATUS_ACCESS_VIOLATION);
    RequireWireMarker(page, output - 4, 24); RequireWireMarker(page, readonly_iosb, 8);
    REQUIRE(heap->Protect(page.base + 0x1000, 0x1000, kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(page.memory.TranslateVirtual(iosb - 4), 0xA5, 16);
    REQUIRE(heap->Protect(page.base + 0x1000, 0x1000, kMemoryProtectRead));
    REQUIRE(QueryNtWireVolume(runtime, page, file.file->handle(), iosb, readonly_iosb, 8) == X_STATUS_ACCESS_VIOLATION);
    RequireWireMarker(page, iosb - 4, 16); RequireWireMarker(page, readonly_iosb, 8);
    REQUIRE(heap->Protect(page.base + 0x1000, 0x1000, kMemoryProtectRead | kMemoryProtectWrite));
  }
}

TEST_CASE("Nested real STFS mounts close with their retained parent and stale close preserves replacement",
          "[kernel][content][storage][mount][lifetime]") {
  SelectorRuntime runtime; InstallWireNestedPackages(runtime);
  auto* content = runtime.state()->content_manager(); auto* fs = runtime.emulator->file_system();
  std::shared_ptr<vfs::Device> parent, child;
  REQUIRE(content->OpenContentFile("Parent", "Host:\\outer.stfs", nullptr, 0, &parent) == X_ERROR_SUCCESS);
  REQUIRE(content->OpenContentFile("Child", "Parent:\\child", nullptr, 0, &child) == X_ERROR_SUCCESS);
  REQUIRE(parent); REQUIRE(child); REQUIRE(child != parent);
  REQUIRE(child->storage_volume() == parent->storage_volume());
  REQUIRE(fs->IsSymbolicLinkRegistered("Parent:"));
  REQUIRE(fs->IsSymbolicLinkRegistered("Child:"));
  REQUIRE(parent->storage_volume()->source_id() == "hdd");
  vfs::File* opened = nullptr; vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr, "Child:\\payload", vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead, false, true, &opened, &action) == X_STATUS_SUCCESS);
  REQUIRE(opened);
  const auto destroy_file = [](vfs::File* file) { file->Destroy(); };
  std::unique_ptr<vfs::File, decltype(destroy_file)> retained(opened, destroy_file);
  std::array<uint8_t, 4> bytes{}; size_t read = 0;
  const auto status = opened->ReadSync(bytes, 0, &read);
  REQUIRE(status == X_STATUS_SUCCESS); REQUIRE(read == bytes.size());
  REQUIRE((bytes == std::array<uint8_t, 4>{0x53, 0, 0x80, 0xFF}));
  const auto old_path = opened->entry()->absolute_path();
  REQUIRE(content->CloseContent("pArEnT", parent.get()) == X_ERROR_SUCCESS);
  REQUIRE_FALSE(content->IsRootTaken("Parent")); REQUIRE_FALSE(content->IsRootTaken("Child"));
  REQUIRE_FALSE(fs->IsSymbolicLinkRegistered("Parent:"));
  REQUIRE_FALSE(fs->IsSymbolicLinkRegistered("Child:"));
  REQUIRE_FALSE(fs->ResolvePath("Parent:\\child")); REQUIRE_FALSE(fs->ResolvePath("Child:\\payload"));
  REQUIRE(opened->entry()->device() == child.get());
  REQUIRE(opened->entry()->absolute_path() == old_path);
  retained.reset();

  std::shared_ptr<vfs::Device> replacement;
  REQUIRE(content->OpenContentFile("Parent", "Host:\\outer.stfs", nullptr, 0, &replacement) == X_ERROR_SUCCESS);
  REQUIRE(replacement); REQUIRE(replacement != parent);
  std::string replacement_target;
  REQUIRE(fs->FindSymbolicLink("Parent:", replacement_target));
  REQUIRE(content->CloseContent("PARENT", parent.get()) == X_ERROR_INVALID_HANDLE);
  REQUIRE(content->ClosePackagesMountedFrom(parent.get()) == X_ERROR_SUCCESS);
  REQUIRE(fs->AcquirePathDevice("Parent:") == replacement);
  std::string current_target;
  REQUIRE(fs->FindSymbolicLink("Parent:", current_target));
  REQUIRE(current_target == replacement_target);
  REQUIRE(fs->ResolvePath("Parent:\\child"));
  REQUIRE(content->CloseContent("parent", replacement.get()) == X_ERROR_SUCCESS);
  REQUIRE_FALSE(fs->IsSymbolicLinkRegistered("Parent:"));
}


TEST_CASE("Actual content close preserves an explicitly recreated root alias token",
          "[kernel][content][storage][mount][source_alias][ownership]") {
  const bool same_device = GENERATE(false, true);
  SelectorRuntime runtime; InstallWireNestedPackages(runtime);
  auto* content = runtime.state()->content_manager();
  auto* fs = runtime.emulator->file_system();
  std::shared_ptr<vfs::Device> old, other;
  REQUIRE(content->OpenContentFile("OldRoot", "Host:\\outer.stfs", nullptr, 0,
                                   &old) == X_ERROR_SUCCESS);
  REQUIRE(content->OpenContentFile("OtherRoot", "Host:\\outer.stfs", nullptr, 0,
                                   &other) == X_ERROR_SUCCESS);
  REQUIRE(old); REQUIRE(other); REQUIRE(old != other);
  REQUIRE(fs->UnregisterSymbolicLink("OldRoot:"));
  const auto selected = same_device ? old : other;
  std::shared_ptr<const vfs::SymbolicLinkToken> replacement;
  REQUIRE(fs->RegisterDeviceSymbolicLink("OldRoot:", selected->mount_path(),
                                       selected, replacement) == X_STATUS_SUCCESS);
  REQUIRE(content->CloseContent("OldRoot", old.get()) == X_ERROR_SUCCESS);
  REQUIRE_FALSE(content->IsRootTaken("OldRoot"));
  // Old close may not erase the newer link, even for equal source and target.
  CHECK(fs->IsSymbolicLinkRegistered("OldRoot:"));
  if (same_device) {
    CHECK(fs->ResolvePath("OldRoot:\\child") == nullptr); // retired old source
  } else {
    REQUIRE(fs->ResolvePath("OldRoot:\\child"));
    CHECK(fs->AcquirePathDevice("OldRoot:") == other);
  }
  REQUIRE(fs->UnregisterSymbolicLink(*replacement));
  REQUIRE(content->CloseContent("OtherRoot", other.get()) == X_ERROR_SUCCESS);
}

TEST_CASE("A queued borrowed mount refuses parent withdrawal before atomic publication",
          "[kernel][content][storage][mount][async][lifetime]") {
  SelectorRuntime runtime; SelectorPage page(runtime.memory());
  InstallWireNestedPackages(runtime);
  auto* state = runtime.state(); auto* content = state->content_manager();
  REQUIRE(content->OpenContentFile("Parent", "Host:\\outer.stfs") == X_ERROR_SUCCESS);
  auto barrier = std::make_shared<SelectorBarrier>();
  auto event = object_ref<XEvent>(new XEvent(state)); event->Initialize(true, false);
  auto* overlapped = page.memory.TranslateVirtual<XAM_OVERLAPPED*>(page.overlap());
  overlapped->event = event->handle();
  struct Result {
    bool blocked = false, reserved = false, unpublished = false;
    uint32_t admitted = X_ERROR_FUNCTION_FAILED, closed = X_ERROR_FUNCTION_FAILED;
    uint32_t duplicate = X_ERROR_FUNCTION_FAILED, waited = X_STATUS_UNSUCCESSFUL;
    uint32_t result = X_ERROR_FUNCTION_FAILED, extended = 0, length = UINT32_MAX;
  } observed;
  std::mutex drain_mutex;
  const auto drain = [&] { std::lock_guard lock(drain_mutex); state->ShutdownDispatchThread(); };
  auto thread = object_ref<XHostThread>(new XHostThread(state, 128 * 1024, 0, [&] {
    state->CompleteOverlappedDeferredEx([barrier](uint32_t& error, uint32_t& length) {
      std::unique_lock lock(barrier->mutex); barrier->entered = true; barrier->changed.notify_all();
      const bool released = barrier->changed.wait_for(lock, std::chrono::seconds(30), [&] { return barrier->released; });
      error = 0; length = 0; return released ? X_ERROR_SUCCESS : X_ERROR_CANCELLED;
    }, page.base + 0x400);
    {
      std::unique_lock lock(barrier->mutex);
      observed.blocked = barrier->changed.wait_for(lock, std::chrono::seconds(3), [&] { return barrier->entered; });
    }
    if (!observed.blocked) { barrier->Release(); drain(); return 1; }
    observed.admitted = content->OpenContentFile("PendingChild", "Parent:\\child", nullptr, page.overlap());
    observed.reserved = content->IsRootTaken("pEnDiNgChIlD");
    observed.unpublished = !runtime.emulator->file_system()->AcquirePathDevice("PendingChild:");
    observed.closed = content->CloseContent("Parent");
    observed.duplicate = content->OpenContentFile("PENDINGCHILD", "Host:\\outer.stfs");
    barrier->Release();
    if (observed.admitted == X_ERROR_IO_PENDING) {
      uint64_t timeout = uint64_t(-5LL * 10000000);
      observed.waited = event->Wait(0, 0, 0, &timeout);
      if (observed.waited != X_STATUS_SUCCESS) { drain(); }
    } else { drain(); }
    observed.result = XOverlappedGetResult(overlapped);
    observed.extended = uint32_t(overlapped->extended_error);
    observed.length = XOverlappedGetLength(overlapped);
    return 0;
  }, state->GetTitleProcess()));
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  const auto waited = threading::Wait(thread->thread(), false, std::chrono::seconds(10));
  barrier->Release();
  if (waited != threading::WaitResult::kSuccess) {
    drain();
    if (threading::Wait(thread->thread(), false, std::chrono::seconds(10)) != threading::WaitResult::kSuccess) {
      std::terminate();
    }
  }
  REQUIRE(waited == threading::WaitResult::kSuccess);
  // Completion signals before the dispatch closure's final shared owners
  // necessarily drop. Join its actual worker before asserting claim rundown.
  drain();
  REQUIRE(observed.blocked); REQUIRE(observed.admitted == X_ERROR_IO_PENDING);
  REQUIRE(observed.reserved); REQUIRE(observed.unpublished);
  REQUIRE(observed.closed == X_ERROR_SUCCESS); REQUIRE(observed.duplicate == X_ERROR_ALREADY_EXISTS);
  REQUIRE(observed.waited == X_STATUS_SUCCESS); REQUIRE(observed.result == X_ERROR_FUNCTION_FAILED);
  REQUIRE(observed.extended == X_HRESULT_FROM_WIN32(X_ERROR_DEVICE_NOT_CONNECTED));
  REQUIRE(observed.length == 0);
  REQUIRE_FALSE(content->IsRootTaken("PendingChild"));
  REQUIRE_FALSE(runtime.emulator->file_system()->AcquirePathDevice("PendingChild:"));
  REQUIRE(content->OpenContentFile("PendingChild", "Host:\\outer.stfs") == X_ERROR_SUCCESS);
}

}  // namespace xe::kernel::xam::test

namespace xe::kernel::xam::test {
TEST_CASE("Actual overlap waiter admits the next modal before the old delayed post",
          "[kernel][storage][selector][host_ui_fallback][completion][async]") {
  SelectorRuntime runtime;
  SelectorPage page(runtime.memory());
  auto* state = runtime.state();
  auto& memory = runtime.memory();
  X_RESULT completed_result = X_ERROR_SUCCESS;
  SECTION("accepted") { completed_result = X_ERROR_SUCCESS; }
  SECTION("canceled") { completed_result = X_ERROR_CANCELLED; }
  SECTION("invalid output") { completed_result = X_ERROR_INVALID_PARAMETER; }
  std::memset(memory.TranslateVirtual(page.overlap()), 0, sizeof(XAM_OVERLAPPED));
  auto event = object_ref<XEvent>(new XEvent(state));
  event->Initialize(true, false);
  auto* overlap = memory.TranslateVirtual<XAM_OVERLAPPED*>(page.overlap());
  overlap->event = event->handle();
  auto ingress = state->AcquireNativeUiCreation();
  REQUIRE(ingress);
  std::atomic<bool> visibility_lock_violation{false};
  std::atomic<uint32_t> visible_on{0}, visible_off{0};
  NativeUiBackendRoute prepared([&](bool visible) {
    if (global_critical_region::is_held_by_current_thread()) {
      visibility_lock_violation.store(true);
    }
    if (visible) { ++visible_on; } else { ++visible_off; }
  });
  const NativeUiBackendRoute::Cohort cohort{
      7, 11, 13, 17, 19, 0x30020000, 0x3001C000};
  auto old = prepared.Admit(cohort,
      NativeUiBackendRoute::Input::kProvenNoInitialSwap, true).host;
  REQUIRE(old);
  old->RetainHostIngress(std::move(ingress));
  old->SetVisible(true);
  auto retained_dialog = old;  // Both these old captures outlive publication.
  auto retained_post = old;
  auto post = std::make_shared<SelectorBarrier>();
  std::atomic<bool> post_finished{false}, next_kernel{false}, next_prepared{false};
  std::atomic<bool> stale_inert{false}, result_valid{false};
  std::atomic<uint32_t> wait_status{X_STATUS_UNSUCCESSFUL};
  auto waiter = object_ref<XHostThread>(new XHostThread(state, 128 * 1024, 0,
      [&, old, post, retained_post] {
        state->CompleteOverlappedDeferredEx(
            [&, old](uint32_t& extended, uint32_t& length) -> X_RESULT {
              HostUiFallbackOperationCompletion finish_before_event(old);
              auto entered = old->TryEnter();
              auto memory_lock = global_critical_region::AcquireDirect();
              extended = X_HRESULT_FROM_WIN32(completed_result);
              length = 0;
              if (!entered) { return X_ERROR_CANCELLED; }
              if (completed_result == X_ERROR_INVALID_PARAMETER) {
                // Real overflowed guest-output validation, with no invalid
                // dereference. It must still retire both admission claims.
                if (memory.QueryRangeAccess(0xFFFFFFFE, 4) !=
                    memory::PageAccess::kReadWrite) {
                  return X_ERROR_INVALID_PARAMETER;
                }
                return X_ERROR_FUNCTION_FAILED;
              }
              if (completed_result == X_ERROR_SUCCESS) {
                xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.output()),
                                              0x17559001);
              }
              return completed_result;
              // GCRC/Entry release first, then the actual production guard.
            }, page.overlap(), nullptr,
            [post, retained_post, &post_finished] {
              (void)retained_post;  // Retained old lease is the regression input.
              // Force any event waiter to run while the old post still owns
              // its stale lease and cannot have reached a hypothetical finish.
              std::unique_lock lock(post->mutex);
              post->entered = true;
              post->changed.notify_all();
              post->changed.wait_for(lock, std::chrono::seconds(3),
                                    [&] { return post->released; });
              post_finished.store(true);
            });
        uint64_t timeout = uint64_t(-5LL * 10000000);
        const auto waited = event->Wait(0, 0, 0, &timeout);
        wait_status.store(waited);
        if (waited == X_STATUS_SUCCESS) {
          result_valid.store(XOverlappedGetResult(overlap) == completed_result &&
              uint32_t(overlap->extended_error) ==
                  X_HRESULT_FROM_WIN32(completed_result));
          auto next_ingress = state->AcquireNativeUiCreation();
          next_kernel.store(bool(next_ingress));
          auto next = prepared.Admit(cohort,
              NativeUiBackendRoute::Input::kProvenNoInitialSwap, true).host;
          next_prepared.store(bool(next));
          if (next) {
            if (next_ingress) { next->RetainHostIngress(std::move(next_ingress)); }
            next->SetVisible(true);
            retained_dialog->SetVisible(false);
            retained_dialog->FinishHostOperation();
            stale_inert.store(prepared.CurrentHost(cohort) == next &&
                              !retained_dialog->TryEnter() && !post_finished.load());
            next->SetVisible(false);
            next->FinishHostOperation();
          }
        }
        post->Release();
        return 0;
      }, state->GetTitleProcess()));
  REQUIRE(waiter->Create() == X_STATUS_SUCCESS);
  const auto joined = xe::threading::Wait(waiter->thread(), false,
                                         std::chrono::seconds(10));
  post->Release();
  state->ShutdownDispatchThread();  // Join actual post before reading/unwinding.
  if (joined != xe::threading::WaitResult::kSuccess &&
      xe::threading::Wait(waiter->thread(), false, std::chrono::seconds(10)) !=
          xe::threading::WaitResult::kSuccess) { std::terminate(); }
  REQUIRE(joined == xe::threading::WaitResult::kSuccess);
  CHECK(wait_status.load() == X_STATUS_SUCCESS);
  CHECK(result_valid.load());
  CHECK(next_kernel.load());
  CHECK(next_prepared.load());
  CHECK(stale_inert.load());
  CHECK_FALSE(visibility_lock_violation.load());
  CHECK(visible_on.load() == 2);
  CHECK(visible_off.load() == 2);
  CHECK(post_finished.load());
  if (completed_result == X_ERROR_SUCCESS) {
    CHECK(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.output())) ==
          0x17559001);
  }
}
}  // namespace xe::kernel::xam::test


namespace xe::kernel::xam::test {
namespace {
// Real public XAM calls on an actual TITLE XHostThread, including ninth-argument
// stack delivery and the genuine Kernel deferred worker/event publication.
struct ReadAdmissionResult {
  uint32_t returned = UINT32_MAX, waited = UINT32_MAX;
  uint32_t result = UINT32_MAX, extended = UINT32_MAX, length = UINT32_MAX;
};
ReadAdmissionResult InvokeReadAdmissionExport(SelectorRuntime& runtime,
    SelectorPage& page, uint16_t ordinal, std::span<const uint64_t> arguments) {
  auto* state = runtime.state(); auto& memory = runtime.memory();
  auto entry = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", ordinal);
  REQUIRE(entry); REQUIRE(entry->function_data.trampoline); REQUIRE(arguments.size() <= 9);
  auto event = object_ref<XEvent>(new XEvent(state)); event->Initialize(true, false);
  auto* overlap = memory.TranslateVirtual<XAM_OVERLAPPED*>(page.overlap());
  std::memset(overlap, 0, sizeof(*overlap)); overlap->event = event->handle();
  ReadAdmissionResult observed;
  std::mutex drain_mutex;
  const auto drain = [&] { std::lock_guard lock(drain_mutex); state->ShutdownDispatchThread(); };
  auto thread = object_ref<XHostThread>(new XHostThread(state, 128 * 1024, 0, [&] {
    auto* context = XThread::GetCurrentThread()->thread_state()->context();
    const auto old_stack = context->r[1]; context->r[1] = page.base + 0x3800;
    for (size_t i = 0; i != arguments.size(); ++i) {
      if (i < 8) context->r[3 + i] = arguments[i];
      else xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page.base + 0x3800 + 0x54), uint32_t(arguments[i]));
    }
    entry->function_data.trampoline(context);
    observed.returned = uint32_t(context->r[3]); context->r[1] = old_stack;
    if (observed.returned == X_ERROR_IO_PENDING) {
      uint64_t timeout = uint64_t(-5LL * 10000000);
      observed.waited = event->Wait(0, 0, 0, &timeout);
      if (observed.waited != X_STATUS_SUCCESS) drain();
      observed.result = XOverlappedGetResult(overlap);
      observed.extended = uint32_t(overlap->extended_error);
      observed.length = XOverlappedGetLength(overlap);
    }
    return 0;
  }, state->GetTitleProcess()));
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  const auto waited = xe::threading::Wait(thread->thread(), false, std::chrono::seconds(10));
  if (waited != xe::threading::WaitResult::kSuccess) {
    drain();
    if (xe::threading::Wait(thread->thread(), false, std::chrono::seconds(10)) !=
        xe::threading::WaitResult::kSuccess) std::terminate();
  }
  REQUIRE(waited == xe::threading::WaitResult::kSuccess);
  return observed;
}
void RequireReadAdmissionCompletion(const ReadAdmissionResult& observed, X_RESULT cause,
                                     uint32_t disposition = 0) {
  REQUIRE(observed.returned == X_ERROR_IO_PENDING);
  REQUIRE(observed.waited == X_STATUS_SUCCESS);
  REQUIRE(observed.result == (cause ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS));
  REQUIRE(observed.extended == X_HRESULT_FROM_WIN32(cause));
  REQUIRE(observed.length == disposition);
}
constexpr std::string_view kReadAdmissionLeaf = "8C7CB7BEC20F69AFB35D9D2461CD7C528BD1714141";
constexpr std::array<uint8_t, 4> kReadAdmissionPayload{0x52, 0, 0xA5, 0xFF};
XCONTENT_AGGREGATE_DATA ReadAdmissionData(SelectorRuntime& runtime, uint32_t device,
                                         std::string_view leaf = kReadAdmissionLeaf) {
  XCONTENT_AGGREGATE_DATA data{}; data.device_id = device;
  data.title_id = runtime.state()->title_id(); data.content_type = XContentType::kMarketplaceContent;
  data.xuid = 0; data.set_file_name(leaf); return data;
}
std::vector<uint8_t> ReadAdmissionBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary); REQUIRE(input);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::vector<uint8_t> WriteReadAdmissionPackage(SelectorRuntime& runtime,
                                             const std::filesystem::path& path) {
  auto bytes = WireStfs("payload", kReadAdmissionPayload);
  auto* header = reinterpret_cast<vfs::XContentContainerHeader*>(bytes.data());
  header->content_metadata.content_type = XContentType::kMarketplaceContent;
  header->content_metadata.execution_info.title_id = runtime.state()->title_id();
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary); REQUIRE(output);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  REQUIRE(output.good()); output.close(); return bytes;
}
ReadAdmissionResult InvokeReadAdmissionCreate(SelectorRuntime& runtime, SelectorPage& page,
    const XCONTENT_AGGREGATE_DATA& data, uint32_t flags, std::string_view root) {
  REQUIRE(root.size() < 0x100);
  auto& memory = runtime.memory();
  std::memcpy(memory.TranslateVirtual(page.base + 0x500), root.data(), root.size());
  *memory.TranslateVirtual<uint8_t*>(page.base + 0x500 + uint32_t(root.size())) = 0;
  *memory.TranslateVirtual<XCONTENT_DATA*>(page.base + 0x600) = data;
  std::memset(memory.TranslateVirtual(page.base + 0x8FC), 0xA5, 20);
  // AnyUser selects shared XUID0 through the real public owner resolver. No
  // fabricated profile/Gold identity is necessary for this read-admission test.
  const std::array<uint64_t, 9> arguments{XUserIndexAny, page.base + 0x500,
      page.base + 0x600, flags, page.base + 0x900, page.base + 0x908, 0, 0, page.overlap()};
  auto result = InvokeReadAdmissionExport(runtime, page, 0x259, arguments);
  REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.base + 0x8FC)) == 0xA5A5A5A5);
  REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.base + 0x904)) == 0xA5A5A5A5);
  REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.base + 0x90C)) == 0xA5A5A5A5);
  if (result.returned == X_ERROR_IO_PENDING && result.waited == X_STATUS_SUCCESS)
    REQUIRE(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(page.base + 0x900)) == result.length);
  return result;
}
void RequireReadAdmissionPayload(SelectorRuntime& runtime, std::string_view root) {
  vfs::File* file = nullptr; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,
      std::string(root) + ":\\payload", vfs::FileDisposition::kOpen,
      vfs::FileAccess::kFileReadData, false, true, &file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file); std::array<uint8_t, 4> actual{}; size_t count = 0;
  const auto result = file->ReadSync(actual, 0, &count); file->Destroy();
  REQUIRE(result == X_STATUS_SUCCESS); REQUIRE(count == actual.size()); REQUIRE(actual == kReadAdmissionPayload);
}
void RequireReadAdmissionMutationsDenied(SelectorRuntime& runtime, SelectorPage& page,
                                         const XCONTENT_AGGREGATE_DATA& data) {
  auto* content = runtime.state()->content_manager();
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kCreateNew), "DeniedNew"), X_ERROR_ALREADY_EXISTS);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kCreateAlways), "DeniedReplace"), X_ERROR_ACCESS_DENIED);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kTruncateExisting), "DeniedTruncate"), X_ERROR_ACCESS_DENIED);
  *runtime.memory().TranslateVirtual<XCONTENT_DATA*>(page.base + 0x600) = data;
  const std::array<uint64_t, 3> deletion{XUserIndexAny, page.base + 0x600, page.overlap()};
  RequireReadAdmissionCompletion(InvokeReadAdmissionExport(runtime, page, 0x25B, deletion), X_ERROR_ACCESS_DENIED);
  REQUIRE(content->CreatePackage(0, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content->TruncateContent(0, data) == X_ERROR_ACCESS_DENIED);
  REQUIRE(content->DeleteContent(0, data) == X_ERROR_ACCESS_DENIED);
  for (std::string_view root : {"DeniedNew", "DeniedReplace", "DeniedTruncate"}) {
    REQUIRE_FALSE(content->IsRootTaken(root));
    REQUIRE_FALSE(runtime.emulator->file_system()->IsSymbolicLinkRegistered(std::string(root) + ':'));
  }
}
void RequireReadAdmissionCreator(SelectorRuntime& runtime, SelectorPage& page,
                                  const XCONTENT_AGGREGATE_DATA& data) {
  *runtime.memory().TranslateVirtual<XCONTENT_DATA*>(page.base + 0x600) = data;
  const std::array<uint64_t, 5> arguments{XUserIndexAny, page.base + 0x600,
      page.base + 0x910, page.base + 0x918, page.overlap()};
  RequireReadAdmissionCompletion(InvokeReadAdmissionExport(runtime, page, 0x262, arguments), X_ERROR_SUCCESS);
  REQUIRE(xe::load_and_swap<uint64_t>(runtime.memory().TranslateVirtual(page.base + 0x918)) == 0);
}
}  // namespace

TEST_CASE("Public CreateEx opens real optical DLC without granting mutation authority",
          "[kernel][content][read_admission][optical][async]") {
  SelectorRuntime runtime; SelectorPage page(runtime.memory());
  auto data = ReadAdmissionData(runtime, 2);
  const auto root = runtime.scratch.root / "optical";
  const auto package = root / "content" / "0000000000000000" /
      xe::to_path(fmt::format("{:08X}/00000002/{}", runtime.state()->title_id(), kReadAdmissionLeaf));
  const auto original = WriteReadAdmissionPackage(runtime, package);
  auto device = std::make_unique<vfs::HostPathDevice>("\\Device\\ReadOnlyOptical", root, true);
  REQUIRE(device->SetStorageVolume(filesystem::ConsoleStorageVolume::OpticalMedia()));
  REQUIRE(device->Initialize()); REQUIRE(runtime.emulator->file_system()->RegisterDevice(std::move(device)));
  runtime.state()->title_mount_path_ = "\\Device\\ReadOnlyOptical";
  runtime.state()->deployment_type_ = XDeploymentType::kOpticalDisc;
  auto* content = runtime.state()->content_manager();
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_SUCCESS);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kOpenExisting), "ReadDlc"), X_ERROR_SUCCESS, 2);
  RequireReadAdmissionPayload(runtime, "ReadDlc");
  REQUIRE(content->CloseContent("ReadDlc") == X_ERROR_SUCCESS);
  RequireReadAdmissionCreator(runtime, page, data);
  RequireReadAdmissionMutationsDenied(runtime, page, data);
  REQUIRE(ReadAdmissionBytes(package) == original);
  auto missing = data; missing.set_file_name("missing-disc-package");
  REQUIRE(content->CheckPackageName(0, missing) == X_ERROR_PATH_NOT_FOUND);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, missing,
      uint32_t(vfs::XContentFlag::kOpenExisting), "MissingDlc"), X_ERROR_PATH_NOT_FOUND);
  auto invalid = data; invalid.set_file_name("bad/name");
  REQUIRE(content->CheckPackageName(0, invalid) == X_ERROR_INVALID_NAME);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, invalid,
      uint32_t(vfs::XContentFlag::kOpenExisting), "InvalidDlc"), X_ERROR_INVALID_NAME);
  REQUIRE(runtime.emulator->file_system()->RetireDevice("\\Device\\ReadOnlyOptical"));
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kOpenExisting), "RetiredDlc"), X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE_FALSE(content->IsRootTaken("RetiredDlc"));
  REQUIRE(ReadAdmissionBytes(package) == original);
}

TEST_CASE("Public readonly installed content uses read admission and retires its exact source",
          "[kernel][content][read_admission][installed][async]") {
  SelectorRuntime runtime; SelectorPage page(runtime.memory());
  auto data = ReadAdmissionData(runtime, 7);
  const auto root = runtime.scratch.root / "installed-read";
  const auto package = root / "0000000000000000" /
      xe::to_path(fmt::format("{:08X}/00000002/{}", runtime.state()->title_id(), kReadAdmissionLeaf));
  const auto original = WriteReadAdmissionPackage(runtime, package);
  auto* content = runtime.state()->content_manager();
  std::error_code source_error;
  auto installed_authority = filesystem::HostStorageRoot::Open(root, true, source_error, 0);
  REQUIRE(installed_authority); REQUIRE_FALSE(source_error);
  const ContentSource definition{"installed-read", 7, DeviceType::HDD,
      ContentSourceKind::kInstalledSystem, root, "\\InstalledRead", u"Installed read", true, 0};
  // Match the genuine installed-image producer, whose fixed generation may
  // only be replaced by explicit RegisterReadOnly with a newer generation.
  REQUIRE(content->sources().RegisterReadOnly(definition, installed_authority, 1));
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_SUCCESS);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kOpenExisting), "ReadInstalled"), X_ERROR_SUCCESS, 2);
  RequireReadAdmissionPayload(runtime, "ReadInstalled");
  REQUIRE(content->CloseContent("ReadInstalled") == X_ERROR_SUCCESS);
  RequireReadAdmissionCreator(runtime, page, data);
  RequireReadAdmissionMutationsDenied(runtime, page, data);
  REQUIRE(ReadAdmissionBytes(package) == original);
  auto source = content->sources().Source("installed-read"); REQUIRE(source); REQUIRE(source->storage);
  REQUIRE(source->generation == 1);
  const auto old_volume = content->sources().CaptureStorageVolume(*source); REQUIRE(old_volume);
  source->storage->Retire();
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  const auto retired = content->sources().Source("installed-read"); REQUIRE(retired);
  REQUIRE_FALSE(retired->available); REQUIRE(retired->generation == source->generation);
  REQUIRE_FALSE(old_volume->available_cached());
  REQUIRE(content->sources().RegisterReadOnly(definition, installed_authority, 1));
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE_FALSE(content->sources().CaptureStorageVolume(*source));
  REQUIRE_FALSE(content->sources().Resolve(*source,
      package.lexically_relative(source->storage->path()), false));
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kOpenExisting), "RetiredInstalled"), X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(ReadAdmissionBytes(package) == original);
}

TEST_CASE("Public library content reads keep mutation denied and actual missing-file failure",
          "[kernel][content][read_admission][library][async]") {
  SelectorRuntime runtime; SelectorPage page(runtime.memory());
  const auto package = runtime.scratch.root / "library-read.stfs";
  const auto original = WriteReadAdmissionPackage(runtime, package);
  Emulator::LibraryTitle variant; variant.path = package;
  variant.title_id = runtime.state()->title_id(); variant.version = 1; variant.variant_id = 0x123;
  variant.content_type = XContentType::kMarketplaceContent;
  const auto files = InspectCatalogFiles(package); REQUIRE(files);
  variant.physical_files = *files; variant.available = variant.metadata_valid = true;
  runtime.emulator->SetLibraryTitles({variant});
  auto data = ReadAdmissionData(runtime, 1, CatalogVariantFileName(variant));
  auto* content = runtime.state()->content_manager();
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_SUCCESS);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kOpenExisting), "ReadLibrary"), X_ERROR_SUCCESS, 2);
  RequireReadAdmissionPayload(runtime, "ReadLibrary");
  REQUIRE(content->CloseContent("ReadLibrary") == X_ERROR_SUCCESS);
  RequireReadAdmissionCreator(runtime, page, data);
  RequireReadAdmissionMutationsDenied(runtime, page, data);
  REQUIRE(ReadAdmissionBytes(package) == original);
  std::filesystem::rename(package, runtime.scratch.root / "preserved-library.stfs");
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, data,
      uint32_t(vfs::XContentFlag::kOpenExisting), "MissingLibrary"), X_ERROR_PATH_NOT_FOUND);
  REQUIRE(ReadAdmissionBytes(runtime.scratch.root / "preserved-library.stfs") == original);
}

TEST_CASE("Read preflight retains valid new writable names and refuses malformed or disconnected targets",
          "[kernel][content][read_admission][validation]") {
  SelectorRuntime runtime; auto* content = runtime.state()->content_manager();
  auto data = ReadAdmissionData(runtime, 1, "new-writable-name");
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_SUCCESS);
  REQUIRE(content->CreatePackage(0, data) == X_ERROR_SUCCESS);
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_SUCCESS);
  // A real open package holds its native source/claim while a different name
  // in the same parent is admitted and created through the public transaction.
  SelectorPage sibling_page(runtime.memory());
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, sibling_page, data,
      uint32_t(vfs::XContentFlag::kOpenExisting), "HeldSibling"), X_ERROR_SUCCESS, 2);
  auto sibling = data; sibling.set_file_name("new-sibling-during-read");
  REQUIRE(content->CheckPackageName(0, sibling) == X_ERROR_SUCCESS);
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, sibling_page, sibling,
      uint32_t(vfs::XContentFlag::kCreateNew), "CreatedSibling"), X_ERROR_SUCCESS, 1);
  REQUIRE(content->ContentExists(0, sibling));
  REQUIRE(content->CloseContent("CreatedSibling") == X_ERROR_SUCCESS);
  REQUIRE(content->CloseContent("HeldSibling") == X_ERROR_SUCCESS);
  auto absent_leaf = data; absent_leaf.set_file_name("absent-leaf");
  REQUIRE(content->CheckPackageName(0, absent_leaf) == X_ERROR_SUCCESS);
  auto default_device = absent_leaf; default_device.device_id = 0;
  REQUIRE(content->CheckPackageName(0, default_device) == X_ERROR_SUCCESS);
  auto absent_parent = absent_leaf; absent_parent.title_id = 0x10203040;
  REQUIRE(content->CheckPackageName(0, absent_parent) == X_ERROR_SUCCESS);
  auto invalid = data; invalid.set_file_name("bad/name");
  REQUIRE(content->CheckPackageName(0, invalid) == X_ERROR_INVALID_NAME);
  const auto source = content->sources().Source("hdd"); REQUIRE(source); REQUIRE(source->storage);
  auto malformed = data; malformed.set_file_name("malformed-existing");
  const auto bad_path = source->storage->path() / "0000000000000000" /
      xe::to_path(fmt::format("{:08X}/00000002/malformed-existing", runtime.state()->title_id()));
  std::filesystem::create_directories(bad_path.parent_path());
  { std::ofstream bad(bad_path); bad << "not a package"; REQUIRE(bad.good()); }
  REQUIRE(content->CheckPackageName(0, malformed) == X_ERROR_PATH_NOT_FOUND);
  const std::vector<uint8_t> bad_bytes{'n','o','t',' ','a',' ','p','a','c','k','a','g','e'};
  REQUIRE(ReadAdmissionBytes(bad_path) == bad_bytes);
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  auto dangling = data; dangling.set_file_name("dangling-existing");
  const auto dangling_path = bad_path.parent_path() / "dangling-existing";
  std::filesystem::create_symlink("never-created-target", dangling_path);
  REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(dangling_path)));
  REQUIRE(content->CheckPackageName(0, dangling) != X_ERROR_SUCCESS);
  auto folded_dangling = dangling; folded_dangling.set_file_name("DaNgLiNg-ExIsTiNg");
  REQUIRE(content->CheckPackageName(0, folded_dangling) != X_ERROR_SUCCESS);
  REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(dangling_path)));
  REQUIRE(std::filesystem::read_symlink(dangling_path) == "never-created-target");
  REQUIRE_FALSE(std::filesystem::exists(dangling_path.parent_path() / "never-created-target"));
  auto linked_bad = data; linked_bad.set_file_name("linked-existing");
  const auto linked_bad_path = bad_path.parent_path() / "linked-existing";
  std::filesystem::create_symlink("malformed-existing", linked_bad_path);
  REQUIRE(content->CheckPackageName(0, linked_bad) != X_ERROR_SUCCESS);
  REQUIRE(std::filesystem::read_symlink(linked_bad_path) == "malformed-existing");
  REQUIRE(ReadAdmissionBytes(bad_path) == bad_bytes);
  // A readable legacy in-source final link retains read admission. The real
  // CreateNew disposition sees existing content and must not mint a new name.
  auto readable_link = data; readable_link.set_file_name("linked-readable");
  const auto readable_path = bad_path.parent_path() / "linked-readable";
  std::filesystem::create_directory_symlink("new-writable-name", readable_path);
  REQUIRE(content->CheckPackageName(0, readable_link) == X_ERROR_SUCCESS);
  SelectorPage page(runtime.memory());
  RequireReadAdmissionCompletion(InvokeReadAdmissionCreate(runtime, page, readable_link,
      uint32_t(vfs::XContentFlag::kCreateNew), "LinkedNew"), X_ERROR_ALREADY_EXISTS);
  REQUIRE(std::filesystem::read_symlink(readable_path) == "new-writable-name");
  REQUIRE(content->ContentExists(0, data));
#endif
  const auto new_parent_path = source->storage->path() / "0000000000000000/10203040";
  REQUIRE_FALSE(std::filesystem::exists(new_parent_path));
  REQUIRE_FALSE(std::filesystem::exists(bad_path.parent_path() / "absent-leaf"));
  source->storage->Retire();
  REQUIRE(content->CheckPackageName(0, data) == X_ERROR_DEVICE_NOT_CONNECTED);
}
}  // namespace xe::kernel::xam::test

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
TEST_CASE(
    "Normal media references add readonly files and restore without host "
    "roster state",
    "[kernel][storage][selected_media][app_reference]") {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;
  DeviceScratch scratch;
  std::filesystem::create_directory(scratch.root / "content");
  std::filesystem::create_directory(scratch.root / "cache");
  DeviceScratch input;
  std::ofstream(input.root / "song.mp3") << "payload";
  std::ofstream(input.root / "sibling") << "unselected";
  auto open = [&] {
    auto emulator = std::make_unique<Emulator>(
        "", scratch.root, scratch.root / "content", scratch.root / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_SUCCESS);
    return emulator;
  };
  auto emulator = open();
  auto* content = emulator->kernel_state()->content_manager();
  auto* fs = emulator->file_system();
  const auto added =
      content->AddMediaReference({input.root / "song.mp3"}, false, *fs);
  REQUIRE_FALSE(added.error);
  REQUIRE(added.persisted);
  REQUIRE(added.durable);
  REQUIRE(added.active);
  std::string guest_path;
  {
    const auto source = content->sources().Source(added.id);
    REQUIRE(source);
    REQUIRE(source->available);
    CHECK(source->source.read_only);
    CHECK_FALSE(source->profile_writable);
    CHECK_FALSE(source->format_eligible);
    guest_path = source->source.guest_volume_root;
  }
  const auto read = [&](Emulator& runtime) {
    vfs::File* file = nullptr;
    vfs::FileAction action{};
    REQUIRE(runtime.file_system()->OpenFile(
                nullptr, guest_path + R"(\0\song.mp3)",
                vfs::FileDisposition::kOpen, vfs::FileAccess::kFileReadData,
                false, true, &file, &action) == X_STATUS_SUCCESS);
    REQUIRE(file);
    std::array<uint8_t, 7> bytes{};
    size_t count = 0;
    const auto status = file->ReadSync(bytes, 0, &count);
    file->Destroy();
    CHECK(status == X_STATUS_SUCCESS);
    CHECK(count == bytes.size());
    CHECK(std::string(bytes.begin(), bytes.end()) == "payload");
    file = nullptr;
    CHECK(XFAILED(runtime.file_system()->OpenFile(
        nullptr, guest_path + R"(\sibling)", vfs::FileDisposition::kOpen,
        vfs::FileAccess::kFileReadData, false, true, &file, &action)));
    CHECK_FALSE(file);
    CHECK(XFAILED(runtime.file_system()->OpenFile(
        nullptr, guest_path + R"(\0\song.mp3)",
        vfs::FileDisposition::kOverwriteIf, vfs::FileAccess::kFileWriteData,
        false, true, &file, &action)));
    CHECK_FALSE(file);
  };
  read(*emulator);
  emulator.reset();
  emulator = open();
  content = emulator->kernel_state()->content_manager();
  fs = emulator->file_system();
  std::error_code error;
  const auto restored = content->RestoreMediaReferences(*fs, error);
  REQUIRE_FALSE(error);
  REQUIRE(restored.size() == 1);
  CHECK(restored[0].id == added.id);
  CHECK(restored[0].active);
  read(*emulator);
  const auto removed = content->RemoveMediaReference(added.id, *fs);
  REQUIRE_FALSE(removed.error);
  CHECK(removed.removed);
  CHECK(removed.durable);
  CHECK_FALSE(fs->AcquireDevice(guest_path));
  CHECK(std::filesystem::file_size(input.root / "song.mp3") == 7);
  CHECK(std::filesystem::file_size(input.root / "sibling") == 10);
  emulator.reset();
  emulator = open();
  content = emulator->kernel_state()->content_manager();
  CHECK(
      content->RestoreMediaReferences(*emulator->file_system(), error).empty());
  CHECK_FALSE(error);
}
TEST_CASE("Media restore preserves a replaced file as saved inactive intent",
          "[kernel][storage][selected_media][app_reference]") {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;
  DeviceScratch scratch;
  DeviceScratch input;
  std::filesystem::create_directory(scratch.root / "content");
  std::filesystem::create_directory(scratch.root / "cache");
  std::ofstream(input.root / "song") << "original";
  auto open = [&] {
    auto emulator = std::make_unique<Emulator>(
        "", scratch.root, scratch.root / "content", scratch.root / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_SUCCESS);
    return emulator;
  };
  auto emulator = open();
  auto* content = emulator->kernel_state()->content_manager();
  const auto added = content->AddMediaReference({input.root / "song"}, false,
                                                *emulator->file_system());
  REQUIRE_FALSE(added.error);
  REQUIRE(added.active);
  emulator.reset();
  std::filesystem::rename(input.root / "song", input.root / "old");
  std::ofstream(input.root / "song") << "replacement";
  emulator = open();
  content = emulator->kernel_state()->content_manager();
  std::error_code error;
  const auto rows =
      content->RestoreMediaReferences(*emulator->file_system(), error);
  REQUIRE_FALSE(error);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].id == added.id);
  CHECK_FALSE(rows[0].active);
  CHECK(rows[0].error);
  REQUIRE(content->sources().Source(added.id));
  CHECK_FALSE(content->sources().Source(added.id)->available);
  const auto removed =
      content->RemoveMediaReference(added.id, *emulator->file_system());
  CHECK_FALSE(removed.error);
  CHECK(removed.removed);
  CHECK(std::filesystem::file_size(input.root / "old") == 8);
  CHECK(std::filesystem::file_size(input.root / "song") == 11);
}
}  // namespace xe::kernel::xam::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
class SelectedMediaTimingAccess {
 public:
  static void Set(ContentSourceRegistry& registry,
                  std::function<void(const std::string&)> sampled = {},
                  std::function<void(const std::string&)> prepared = {}) {
    registry.SetSelectedMediaTimingForTest(std::move(sampled),
                                           std::move(prepared));
  }
};
TEST_CASE(
    "Media intent remains durable when actual namespace publication is refused",
    "[kernel][storage][selected_media][app_reference]") {
  SelectorRuntime runtime;
  DeviceScratch input;
  std::ofstream(input.root / "song") << "payload";
  auto* content = runtime.state()->content_manager();
  auto* fs = runtime.emulator->file_system();
  std::string occupied;
  std::shared_ptr<vfs::Device> foreign;
  SelectedMediaTimingAccess::Set(
      content->sources(), {}, [&](const std::string&) {
        // The existing preparation cut is before actual registry/namespace
        // commit. A real foreign device wins that actual name, not a fake
        // failure result.
        std::error_code reentrant;
        CHECK(content->RestoreMediaReferences(*fs, reentrant).empty());
        CHECK(reentrant == std::errc::device_or_resource_busy);
        for (unsigned i = 0; i < 10; ++i) {
          const auto name = std::string(R"(\Device\Mass)") + std::to_string(i);
          if (fs->AcquireDevice(name)) {
            continue;
          }
          auto device = std::make_unique<vfs::NullDevice>(
              name, std::initializer_list<std::string>{});
          REQUIRE(device->Initialize());
          REQUIRE(fs->RegisterDevice(std::move(device)));
          occupied = name;
          foreign = fs->AcquireDevice(name);
          break;
        }
      });
  const auto result =
      content->AddMediaReference({input.root / "song"}, false, *fs);
  SelectedMediaTimingAccess::Set(content->sources());
  REQUIRE(result.persisted);
  REQUIRE(result.durable);
  CHECK_FALSE(result.active);
  CHECK(result.error);
  REQUIRE(foreign);
  CHECK(fs->AcquireDevice(occupied) == foreign);
  CHECK_FALSE(content->sources().Source(result.id));
  REQUIRE(fs->UnregisterDevice(occupied, foreign.get()));
  std::error_code error;
  const auto restored = content->RestoreMediaReferences(*fs, error);
  REQUIRE_FALSE(error);
  REQUIRE(restored.size() == 1);
  CHECK(restored[0].id == result.id);
  CHECK(restored[0].active);
  CHECK(fs->AcquireDevice(occupied) != foreign);
  CHECK(std::filesystem::file_size(input.root / "song") == 7);
}
}  // namespace xe::kernel::xam::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
TEST_CASE("Saved media removal retains its real open VFS file namespace",
          "[kernel][storage][selected_media][app_reference]") {
  DeviceScratch input;
  DeviceScratch other;
  std::ofstream(input.root / "song") << "original";
  std::ofstream(other.root / "other") << "second";
  SelectorRuntime runtime;
  bool folder = false;
  SECTION("exact selected files") {}
  SECTION("selected folder") { folder = true; }
  auto* content = runtime.state()->content_manager();
  auto* fs = runtime.emulator->file_system();
  const auto added = content->AddMediaReference(
      {folder ? input.root : input.root / "song"}, folder, *fs);
  REQUIRE_FALSE(added.error);
  REQUIRE(added.active);
  const auto source = content->sources().Source(added.id);
  REQUIRE(source);
  const auto mount = source->source.guest_volume_root;
  const auto device = fs->AcquireDevice(mount);
  REQUIRE(device);
  vfs::File* raw = nullptr;
  vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr, mount + (folder ? R"(\song)" : R"(\0\song)"),
                       vfs::FileDisposition::kOpen,
                       vfs::FileAccess::kFileReadData, false, true, &raw,
                       &action) == X_STATUS_SUCCESS);
  const auto destroy = [](vfs::File* file) {
    if (file) {
      file->Destroy();
    }
  };
  std::unique_ptr<vfs::File, decltype(destroy)> file(raw, destroy);
  REQUIRE(file);
  REQUIRE(device->has_open_files());
  const auto removed = content->RemoveMediaReference(added.id, *fs);
  CHECK(removed.removed);
  CHECK(removed.persisted);
  CHECK(removed.durable);
  CHECK(removed.error == std::errc::device_or_resource_busy);
  CHECK(fs->AcquireDevice(mount) == device);
  CHECK(device->has_open_files());
  REQUIRE(content->sources().Source(added.id));
  // A subsequent real add may use another slot, never the still-owned mount.
  const auto second =
      content->AddMediaReference({other.root / "other"}, false, *fs);
  REQUIRE_FALSE(second.error);
  REQUIRE(second.active);
  CHECK(content->sources().Source(second.id)->source.guest_volume_root !=
        mount);
  CHECK(fs->AcquireDevice(mount) == device);
  file.reset();
  CHECK_FALSE(device->has_open_files());
  std::error_code error;
  const auto rows = content->RestoreMediaReferences(*fs, error);
  REQUIRE_FALSE(error);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().active);
  CHECK_FALSE(content->sources().Source(added.id));
  CHECK_FALSE(fs->AcquireDevice(mount));
  const auto third =
      content->AddMediaReference({input.root / "song"}, false, *fs);
  REQUIRE_FALSE(third.error);
  REQUIRE(third.active);
  CHECK(content->sources().Source(third.id)->source.guest_volume_root == mount);
  CHECK(fs->AcquireDevice(mount) != device);
  CHECK(std::filesystem::file_size(input.root / "song") == 8);
  CHECK(std::filesystem::file_size(other.root / "other") == 6);
}

TEST_CASE(
    "Saved media owner refuses foreign newer and same-generation publications",
    "[kernel][storage][selected_media][app_reference]") {
  DeviceScratch input;
  DeviceScratch other;
  std::ofstream(input.root / "song") << "original";
  std::ofstream(other.root / "song") << "foreign";
  SelectorRuntime runtime;
  bool aba = false;
  SECTION("newer independent publication") {}
  SECTION("forgotten and reused generation one") { aba = true; }
  auto* content = runtime.state()->content_manager();
  auto* fs = runtime.emulator->file_system();
  auto& registry = content->sources();
  const auto added =
      content->AddMediaReference({input.root / "song"}, false, *fs);
  REQUIRE_FALSE(added.error);
  REQUIRE(added.active);
  const auto old = registry.Source(added.id);
  REQUIRE(old);
  REQUIRE(old->generation == 1);
  const auto old_device = fs->AcquireDevice(old->source.guest_volume_root);
  REQUIRE(old_device);
  if (aba) {
    REQUIRE(registry.WithdrawSelectedMedia(added.id, 1));
    REQUIRE(registry.ForgetSelectedMedia(added.id, 1, *fs));
    REQUIRE_FALSE(registry.Source(added.id));
  }
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(other.root, true, error);
  REQUIRE(root);
  auto selection = filesystem::MediaSelection::CaptureFiles(
      {{"0/song", root, "song"}}, error);
  REQUIRE(selection);
  const uint64_t generation = aba ? 1 : 2;
  REQUIRE(
      registry.RegisterSelectedMedia(old->source, selection, generation, *fs));
  const auto foreign_device = fs->AcquireDevice(old->source.guest_volume_root);
  REQUIRE(foreign_device);
  REQUIRE(foreign_device != old_device);
  const auto rows = content->RestoreMediaReferences(*fs, error);
  REQUIRE_FALSE(error);
  REQUIRE(rows.size() == 1);
  CHECK_FALSE(rows.front().active);
  CHECK(rows.front().error == std::errc::address_in_use);
  const auto current = registry.Source(added.id);
  REQUIRE(current);
  CHECK(current->generation == generation);
  CHECK(current->selected_media == selection);
  CHECK(current->available);
  CHECK(fs->AcquireDevice(old->source.guest_volume_root) == foreign_device);
  const auto removed = content->RemoveMediaReference(added.id, *fs);
  CHECK(removed.removed);
  CHECK(removed.durable);
  CHECK(removed.error == std::errc::device_or_resource_busy);
  // The pending local cleanup record must not gain ownership on a later call.
  CHECK(content->RestoreMediaReferences(*fs, error).empty());
  CHECK_FALSE(error);
  CHECK(registry.Source(added.id)->selected_media == selection);
  CHECK(registry.Source(added.id)->available);
  CHECK(fs->AcquireDevice(old->source.guest_volume_root) == foreign_device);
  vfs::File* raw = nullptr;
  vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr, old->source.guest_volume_root + R"(\0\song)",
                       vfs::FileDisposition::kOpen,
                       vfs::FileAccess::kFileReadData, false, true, &raw,
                       &action) == X_STATUS_SUCCESS);
  const auto destroy = [](vfs::File* file) {
    if (file) {
      file->Destroy();
    }
  };
  std::unique_ptr<vfs::File, decltype(destroy)> file(raw, destroy);
  REQUIRE(file);
  std::array<uint8_t, 7> bytes{};
  size_t count = 0;
  CHECK(file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  CHECK(count == bytes.size());
  CHECK(std::string(bytes.begin(), bytes.end()) == "foreign");
  CHECK(std::filesystem::file_size(input.root / "song") == 8);
  CHECK(std::filesystem::file_size(other.root / "song") == 7);
}
}  // namespace xe::kernel::xam::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::kernel::xam::test {
TEST_CASE(
    "Normal media activation observes its own postcommit callback outcome",
    "[kernel][storage][selected_media][app_reference]") {
  // Keep this timing gate alive through the actual registry worker shutdown.
  struct PollGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
    void Wait() {
      std::unique_lock lock(mutex);
      changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
    }
    void Release() {
      std::lock_guard lock(mutex);
      released = true;
      changed.notify_all();
    }
  } gate;
  DeviceScratch input, other;
  std::ofstream(input.root / "song") << "original";
  std::ofstream(input.root / "sentinel") << "sentinel";
  std::ofstream(other.root / "song") << "foreign";
  SelectorRuntime runtime;
  bool restore = false, withdraw = false;
  SECTION("add replaced in callback") {}
  SECTION("add withdrawn in callback") { withdraw = true; }
  SECTION("restore replaced in callback") { restore = true; }
  SECTION("restore withdrawn in callback") {
    restore = true;
    withdraw = true;
  }
  auto* content = runtime.state()->content_manager();
  auto* fs = runtime.emulator->file_system();
  auto& registry = content->sources();
  const auto sentinel =
      content->AddMediaReference({input.root / "sentinel"}, false, *fs);
  REQUIRE_FALSE(sentinel.error);
  REQUIRE(sentinel.active);
  std::string target_id;
  if (restore) {
    const auto saved =
        content->AddMediaReference({input.root / "song"}, false, *fs);
    REQUIRE_FALSE(saved.error);
    REQUIRE(saved.active);
    REQUIRE(saved.durable);
    target_id = saved.id;
    const auto old = registry.Source(target_id);
    REQUIRE(old);
    // Leave real saved intent, but no live publication, so Restore must perform
    // the actual registration instead of merely returning its active fast path.
    REQUIRE(registry.WithdrawSelectedMedia(target_id, old->generation));
    REQUIRE(registry.ForgetSelectedMedia(target_id, old->generation, *fs));
  }
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(other.root, true, error);
  REQUIRE(root);
  auto foreign = filesystem::MediaSelection::CaptureFiles(
      {{"0/song", root, "song"}}, error);
  REQUIRE(foreign);
  const auto caller = std::this_thread::get_id();
  // The existing timing-only seam holds background sampling at a real retained
  // sentinel. The initiating thread still executes the genuine Publish sink,
  // making the within-call replacement deterministic without fake results.
  SelectedMediaTimingAccess::Set(registry, [&gate, caller](const std::string&) {
    if (std::this_thread::get_id() != caller) {
      gate.Wait();
    }
  });
  std::atomic<bool> attempted{false}, changed{false};
  auto connection = registry.storage_notifications()->Connect(
      [&](const NativeContentStorageSnapshot& snapshot) {
        for (const auto& row : snapshot.devices) {
          if (!row.available || row.identity == sentinel.id ||
              !row.guest_volume_root.starts_with(R"(\Device\Mass)")) {
            continue;
          }
          if (attempted.exchange(true)) {
            return;
          }
          const auto current = registry.Source(row.identity);
          if (!current) {
            return;
          }
          if (withdraw) {
            changed.store(
                registry.WithdrawSelectedMedia(row.identity, row.generation));
          } else {
            changed.store(registry.RegisterSelectedMedia(
                current->source, foreign, row.generation + 1, *fs));
          }
          return;
        }
      });
  struct ReleasePoll {
    PollGate& gate;
    ~ReleasePoll() { gate.Release(); }
  } release{gate};
  REQUIRE(connection);
  ContentManager::MediaReferenceResult added;
  std::vector<ContentManager::MediaReferenceInfo> rows;
  if (restore) {
    rows = content->RestoreMediaReferences(*fs, error);
  } else {
    added = content->AddMediaReference({input.root / "song"}, false, *fs);
    target_id = added.id;
  }
  gate.Release();
  SelectedMediaTimingAccess::Set(registry);
  connection->subscription->BeginStop();
  REQUIRE(connection->subscription->Drain());
  REQUIRE(attempted.load());
  REQUIRE(changed.load());
  REQUIRE_FALSE(target_id.empty());
  if (restore) {
    REQUIRE_FALSE(error);
    const auto row =
        std::find_if(rows.begin(), rows.end(),
                     [&](const auto& value) { return value.id == target_id; });
    REQUIRE(row != rows.end());
    CHECK_FALSE(row->active);
    CHECK(row->error == std::errc::device_or_resource_busy);
  } else {
    CHECK(added.persisted);
    CHECK(added.durable);
    CHECK_FALSE(added.active);
    CHECK(added.error == std::errc::device_or_resource_busy);
  }
  const auto current = registry.Source(target_id);
  REQUIRE(current);
  if (withdraw) {
    CHECK_FALSE(current->available);
    // A committed-but-inactive result must retain its actual issued receipt:
    // the next ordinary Restore can retire and replace that exact publication.
    rows = content->RestoreMediaReferences(*fs, error);
    REQUIRE_FALSE(error);
    const auto row =
        std::find_if(rows.begin(), rows.end(),
                     [&](const auto& value) { return value.id == target_id; });
    REQUIRE(row != rows.end());
    CHECK(row->active);
    CHECK_FALSE(row->error);
    const auto removed = content->RemoveMediaReference(target_id, *fs);
    CHECK(removed.removed);
    CHECK_FALSE(removed.error);
  } else {
    CHECK(current->available);
    CHECK(current->selected_media == foreign);
    const auto removed = content->RemoveMediaReference(target_id, *fs);
    CHECK(removed.removed);
    CHECK(removed.durable);
    CHECK(removed.error == std::errc::device_or_resource_busy);
    CHECK(registry.Source(target_id)->selected_media == foreign);
    CHECK(registry.Source(target_id)->available);
  }
  CHECK(std::filesystem::file_size(input.root / "song") == 8);
  CHECK(std::filesystem::file_size(other.root / "song") == 7);
}
}  // namespace xe::kernel::xam::test
#endif


namespace xe::kernel::xam::test {
TEST_CASE("Actual standard HDD mounts preserve a bound volume alias through Content",
          "[kernel][storage][source_alias][volume_publication]") {
  struct OptionalMounts {
    bool scratch = std::exchange(cvars::mount_scratch, false);
    bool devkit = std::exchange(cvars::force_mount_devkit, false);
    ~OptionalMounts() {
      std::swap(cvars::mount_scratch, scratch);
      std::swap(cvars::force_mount_devkit, devkit);
    }
  } optional_mounts;
  SelectorRuntime runtime;
  runtime.emulator->MountStandardDrives();
  auto* fs = runtime.emulator->file_system();
  const auto volume = fs->AcquireDevice("\\Device\\Harddisk0\\Partition1");
  const auto content = fs->AcquireDevice("\\Device\\Harddisk0\\Partition1\\Content");
  REQUIRE(volume);
  REQUIRE(content);
  CHECK(content->mount_parent() == volume);
  CHECK(content->storage_volume() == volume->storage_volume());
  std::shared_ptr<const vfs::SymbolicLinkToken> alias;
  REQUIRE(fs->RegisterDeviceSymbolicLink("\\??\\ResolvedHDD:",
      volume->mount_path(), volume, alias) == X_STATUS_SUCCESS);
  CHECK(fs->AcquirePathDevice("\\??\\ResolvedHDD:\\Content") == content);
  CHECK(fs->ResolvePath("\\??\\ResolvedHDD:\\Content"));
  REQUIRE(fs->UnregisterSymbolicLink(*alias));
}

TEST_CASE("Managed volume child collision rolls back only new mounts and preserves source",
          "[kernel][storage][source_alias][volume_publication][rollback]") {
  SelectorRuntime runtime;
  auto* manager = runtime.state()->content_manager();
  auto* fs = runtime.emulator->file_system();
  const auto root = runtime.scratch.root / "owned-volume-content";
  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(manager->RegisterManagedSource(
      {"owned-folder", 3, DeviceType::USBMASS, ContentSourceKind::kManaged,
       root, "\\FolderVolume\\Content", u"Owned folder", false, 0,
       "\\FolderVolume", {}, runtime.scratch.root}));
  const auto source = manager->sources().Source("owned-folder");
  REQUIRE(source);
  const auto storage_volume = manager->sources().CaptureStorageVolume(*source);
  REQUIRE(storage_volume);
  auto foreign = std::make_unique<vfs::NullDevice>(
      "\\FolderVolume\\Content", std::initializer_list<std::string>{"\\foreign"});
  REQUIRE(foreign->Initialize());
  REQUIRE(fs->RegisterDevice(std::move(foreign)));
  const auto original = fs->AcquireDevice("\\FolderVolume\\Content");
  REQUIRE(original);
  CHECK(manager->MountManagedSourceDevices("owned-folder") ==
        X_STATUS_OBJECT_NAME_COLLISION);
  CHECK_FALSE(fs->AcquireDevice("\\FolderVolume"));
  CHECK(fs->AcquireDevice("\\FolderVolume\\Content") == original);
  CHECK_FALSE(original->namespace_retiring());
  CHECK(fs->ResolvePath("\\FolderVolume\\Content\\foreign"));
  CHECK_FALSE(source->storage->retiring());
  CHECK(manager->sources().CaptureStorageVolume(*source) == storage_volume);
  REQUIRE(fs->UnregisterDevice(original->mount_path(), original.get()));
  REQUIRE(manager->MountManagedSourceDevices("owned-folder") == X_STATUS_SUCCESS);
  const auto volume = fs->AcquireDevice("\\FolderVolume");
  const auto content = fs->AcquireDevice("\\FolderVolume\\Content");
  REQUIRE(volume);
  REQUIRE(content);
  CHECK(content->mount_parent() == volume);
  CHECK(content->storage_volume() == storage_volume);
  std::shared_ptr<const vfs::SymbolicLinkToken> alias;
  REQUIRE(fs->RegisterDeviceSymbolicLink("\\System??\\ResolvedFolder:",
      volume->mount_path(), volume, alias) == X_STATUS_SUCCESS);
  CHECK(fs->AcquirePathDevice("\\System??\\ResolvedFolder:\\Content") == content);
  REQUIRE(fs->UnregisterSymbolicLink(*alias));
}
}  // namespace xe::kernel::xam::test

namespace xe::kernel::xam::test {
TEST_CASE("Managed Content refresh preserves real source ancestry and refuses busy or foreign views",
          "[kernel][storage][source_alias][volume_publication][refresh]") {
  SelectorRuntime runtime;
  auto* manager=runtime.state()->content_manager();auto* fs=runtime.emulator->file_system();
  const auto root=runtime.scratch.root/"refresh-content";
  REQUIRE(std::filesystem::create_directory(root));
  {std::ofstream file(root/"held.bin",std::ios::binary);file<<"held";REQUIRE(file.good());}
  REQUIRE(manager->RegisterManagedSource({"refresh-folder",3,DeviceType::USBMASS,
      ContentSourceKind::kManaged,root,"\\RefreshVolume\\Content",u"Refresh",false,0,
      "\\RefreshVolume",{},runtime.scratch.root}));
  REQUIRE(manager->MountManagedSourceDevices("refresh-folder")==X_STATUS_SUCCESS);
  const auto parent=fs->AcquireDevice("\\RefreshVolume");
  const auto before=fs->AcquireDevice("\\RefreshVolume\\Content");
  REQUIRE(parent);REQUIRE(before);REQUIRE(before->mount_parent()==parent);
  const auto source=manager->sources().Source("refresh-folder");REQUIRE(source);
  const auto identity=manager->sources().CaptureStorageVolume(*source);REQUIRE(identity);
  REQUIRE(before->storage_volume()==identity);
  std::shared_ptr<const vfs::SymbolicLinkToken> volume_alias,old_alias;
  REQUIRE(fs->RegisterDeviceSymbolicLink("\\??\\RefreshRoot:",parent->mount_path(),parent,volume_alias)==X_STATUS_SUCCESS);
  REQUIRE(fs->RegisterDeviceSymbolicLink("\\??\\OldContent:",before->mount_path(),before,old_alias)==X_STATUS_SUCCESS);
  SECTION("An actual open File refuses replacement; genuine close admits the new tree") {
    vfs::File* opened=nullptr;vfs::FileAction action{};
    REQUIRE(fs->OpenFile(nullptr,"\\??\\RefreshRoot:\\Content\\held.bin",
        vfs::FileDisposition::kOpen,vfs::FileAccess::kFileReadData,false,true,&opened,&action)==X_STATUS_SUCCESS);
    REQUIRE(opened);
    struct Close {vfs::File*& file;~Close(){if(file)file->Destroy();}} close{opened};
    {std::ofstream file(root/"late.bin",std::ios::binary);file<<"late";REQUIRE(file.good());}
    CHECK_FALSE(fs->ResolvePath("\\??\\RefreshRoot:\\Content\\late.bin"));
    CHECK(XFAILED(manager->RefreshManagedContentView("refresh-folder")));
    CHECK(fs->AcquireDevice(before->mount_path())==before);
    CHECK_FALSE(before->namespace_retiring());
    std::array<uint8_t,4> bytes{};size_t count=0;
    REQUIRE(opened->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);
    CHECK(count==4);CHECK(bytes==std::array<uint8_t,4>{'h','e','l','d'});
    opened->Destroy();opened=nullptr;
    REQUIRE(manager->RefreshManagedContentView("refresh-folder")==X_STATUS_SUCCESS);
    const auto after=fs->AcquireDevice(before->mount_path());REQUIRE(after);REQUIRE(after!=before);
    CHECK(after->mount_parent()==parent);CHECK(after->storage_volume()==identity);
    CHECK(before->namespace_retiring());CHECK_FALSE(fs->ResolvePath("\\??\\OldContent:\\held.bin"));
    REQUIRE(fs->OpenFile(nullptr,"\\??\\RefreshRoot:\\Content\\late.bin",
        vfs::FileDisposition::kOpen,vfs::FileAccess::kFileReadData,false,true,&opened,&action)==X_STATUS_SUCCESS);
    REQUIRE(opened);bytes.fill(0);count=0;
    REQUIRE(opened->ReadSync(bytes,0,&count)==X_STATUS_SUCCESS);
    CHECK(count==4);CHECK(bytes==std::array<uint8_t,4>{'l','a','t','e'});
    opened->Destroy();opened=nullptr;
    REQUIRE(fs->UnregisterSymbolicLink(*old_alias));
    std::shared_ptr<const vfs::SymbolicLinkToken> fresh;
    REQUIRE(fs->RegisterDeviceSymbolicLink("\\??\\OldContent:",after->mount_path(),after,fresh)==X_STATUS_SUCCESS);
    CHECK_FALSE(fs->UnregisterSymbolicLink(*old_alias));
    CHECK(fs->ResolvePath("\\??\\OldContent:\\late.bin"));
    REQUIRE(fs->UnregisterSymbolicLink(*fresh));
  }
  SECTION("A foreign same-name child is never replaced or attached to the source") {
    REQUIRE(fs->UnregisterDevice(before->mount_path(),before.get()));
    auto foreign=std::make_unique<vfs::NullDevice>(before->mount_path(),
        std::initializer_list<std::string>{"\\foreign"});REQUIRE(foreign->Initialize());
    REQUIRE(fs->RegisterDevice(std::move(foreign)));
    const auto actual=fs->AcquireDevice(before->mount_path());REQUIRE(actual);
    CHECK(manager->RefreshManagedContentView("refresh-folder")==X_STATUS_ACCESS_DENIED);
    CHECK(fs->AcquireDevice(before->mount_path())==actual);CHECK_FALSE(actual->namespace_retiring());
    CHECK(fs->ResolvePath("\\RefreshVolume\\Content\\foreign"));
    CHECK(manager->sources().CaptureStorageVolume(*source)==identity);
    CHECK_FALSE(source->storage->retiring());
  }
  REQUIRE(fs->UnregisterSymbolicLink(*volume_alias));
}
} // namespace xe::kernel::xam::test
