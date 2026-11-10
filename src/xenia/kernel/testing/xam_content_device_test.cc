/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include "xenia/kernel/xam/xam_content_device.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <utility>

#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include <filesystem>
#include <random>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/content_source.h"

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
