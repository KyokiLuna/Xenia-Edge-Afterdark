/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <cstring>
#include <string_view>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_content_storage_driver.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"
#include "xenia/base/platform.h"

namespace xe::kernel::test {
TEST_CASE("Storage ordinals register their real one-argument trampolines",
          "[kernel][native_storage_exports][abi]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t output = 0x90022000;
  REQUIRE(memory.LookupHeap(output)->AllocFixed(
      output, 0x1000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  std::array<uint8_t, 24> original;
  original.fill(0xA5);
  std::memcpy(memory.TranslateVirtual(output), original.data(),
              original.size());
  cpu::ExportResolver resolver;
  xboxkrnl::RegisterStorageExports(&resolver, nullptr);
  xboxkrnl::XboxkrnlModule::RegisterExportTable(&resolver);
  const auto* registration = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x20E);
  const auto* notification = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x210);
  for (const auto* entry : {registration, notification}) {
    REQUIRE(entry);
    REQUIRE(entry->get_type() == cpu::Export::Type::kFunction);
    REQUIRE(entry->is_implemented());
    REQUIRE_FALSE(entry->tags & cpu::ExportTag::kStub);
    REQUIRE(entry->function_data.trampoline);
  }
  REQUIRE(std::string_view(registration->name) ==
          "DrvSetContentStorageCallback");
  REQUIRE(std::string_view(notification->name) ==
          "DrvGetContentStorageNotification");

  // An absent owned runtime must reject rather than register an arbitrary
  // callback or manufacture an empty success. Extra registers are not hidden
  // output arguments and must never be dereferenced or consumed.
  PPCContext context{};
  context.virtual_membase = memory.virtual_membase();
  context.r[3] = NativeContentStorageDriver::kNativeCallback;
  context.r[4] = 0xFEDCBA9876543210ull;
  context.r[5] = 0xFFFFFFFF;
  registration->function_data.trampoline(&context);
  REQUIRE(uint32_t(context.r[3]) == X_STATUS_NOT_SUPPORTED);
  REQUIRE(context.r[4] == 0xFEDCBA9876543210ull);

  for (const uint32_t pointer : {output, 0u, 0xFFFFFFFFu}) {
    context.r[3] = pointer;
    notification->function_data.trampoline(&context);
    REQUIRE(uint32_t(context.r[3]) == 0xC000009D);
    REQUIRE(context.r[4] == 0xFEDCBA9876543210ull);
    REQUIRE(std::memcmp(memory.TranslateVirtual(output), original.data(),
                        original.size()) == 0);
  }
}
}  // namespace xe::kernel::test

#if XE_PLATFORM_LINUX
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include "xenia/base/host_storage.h"
#include "xenia/base/mutex.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace test {
class NativeContentStorageDriverTestAccess {
 public:
  static std::shared_ptr<NativeContentStorageDriver> HostOnlyOwner(
      KernelState*,std::shared_ptr<void>);
  static uint32_t AllocateHostPath(
      const std::shared_ptr<NativeContentStorageDriver>&,const std::string&);
  static std::shared_ptr<void> HoldConfigUse(
      const std::shared_ptr<NativeContentStorageDriver>&);
  static bool Retire(const std::shared_ptr<NativeContentStorageDriver>&,KernelState*);
};
}  // namespace test
namespace test {
namespace {
struct StorageLifetimeRoot {
  std::filesystem::path path;
  StorageLifetimeRoot(){
    static std::atomic<uint64_t> sequence{0};
    path=std::filesystem::temp_directory_path()/
        ("xenia-storage-stop-"+std::to_string(std::chrono::steady_clock::now()
          .time_since_epoch().count())+"-"+std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~StorageLifetimeRoot(){std::error_code error;std::filesystem::remove_all(path,error);}
};
}
TEST_CASE("Uninstalled storage host resources retire before an escaped owner outlives Kernel",
          "[kernel][native_storage_exports][storage_stop_lifetime][memory]") {
  // Real Kernel/Memory/native FD/host thread joins and production host path
  // allocation only. There is NO native Driver::Create/install/domain/Task,
  // no GPU object and no claim of SYSTEM or actual GPU-stop acceptance.
  IsolatedKernelRuntimeSettings settings;
  StorageLifetimeRoot root;
  for(const auto* leaf:{"content","cache","closure"})
    REQUIRE(std::filesystem::create_directory(root.path/leaf));
  auto make_runtime=[&]{
    auto value=std::make_unique<Emulator>("",root.path,root.path/"content",root.path/"cache");
    REQUIRE(value->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
    return value;
  };
  auto emulator=make_runtime();
  auto* kernel=emulator->kernel_state();
  auto* memory=emulator->memory();
  REQUIRE(kernel);REQUIRE(memory);
  std::error_code error;
  auto native=filesystem::HostStorageRoot::Open(root.path/"closure",false,error);
  REQUIRE(native);REQUIRE_FALSE(error);
  std::weak_ptr<filesystem::HostStorageRoot> weak_native=native;
  auto owner=NativeContentStorageDriverTestAccess::HostOnlyOwner(kernel,native);
  native.reset();
  REQUIRE(owner);
  CHECK_FALSE(owner->AuthorizeCurrentCaller());
  CHECK_FALSE(NativeContentStorageDriver::Install(owner));
  const std::string path="\\Device\\StorageHostLifetime";
  const auto address=NativeContentStorageDriverTestAccess::AllocateHostPath(owner,path);
  REQUIRE(address);
  CHECK(NativeContentStorageDriverTestAccess::AllocateHostPath(owner,path)==address);
  CHECK(std::memcmp(memory->TranslateVirtual(address),path.c_str(),path.size()+1)==0);
  uint32_t allocation_size=0;
  REQUIRE(memory->LookupHeap(address)->QuerySize(address,&allocation_size));
  CHECK(allocation_size>0);
  const uint32_t output=memory->SystemHeapAlloc(24);REQUIRE(output);
  std::array<uint8_t,24> sentinel;sentinel.fill(0xA5);
  std::memcpy(memory->TranslateVirtual(output),sentinel.data(),sentinel.size());
  auto use=NativeContentStorageDriverTestAccess::HoldConfigUse(owner);REQUIRE(use);
  owner->BeginStop();REQUIRE(owner->Drain());
  // A real Kernel host dispatcher and scheduler CPU/IO stop, not a flag that
  // pretends native cleanup completed. Join actual OS tails after guest signals.
  std::shared_ptr<threading::Event> io_entered=
      threading::Event::CreateManualResetEvent(false);REQUIRE(io_entered);
  kernel->guest_scheduler()->PostHostCall([signal=io_entered]{signal->Set();},
      GuestScheduler::BlockingCallClass::kSerial);
  REQUIRE(threading::Wait(io_entered.get(),false,std::chrono::seconds(5))==
          threading::WaitResult::kSuccess);
  auto stopping_threads=kernel->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread);
  kernel->ShutdownDispatchThread();
  kernel->guest_scheduler()->Shutdown();
  for(const auto& thread:stopping_threads){
    if(thread->thread())REQUIRE(threading::Wait(thread->thread(),false,
        std::chrono::seconds(5))==threading::WaitResult::kSuccess);
    REQUIRE_FALSE(thread->is_running());
  }
  CHECK_FALSE(NativeContentStorageDriverTestAccess::Retire(owner,kernel));
  CHECK_FALSE(weak_native.expired());
  REQUIRE(memory->LookupHeap(address)->QuerySize(address,&allocation_size));
  CHECK(allocation_size>0);
  use.reset(); // Actual Config-use tail must end outside metadata/GCRC locks.
  REQUIRE(NativeContentStorageDriverTestAccess::Retire(owner,kernel));
  CHECK(weak_native.expired());
  REQUIRE(memory->LookupHeap(address)->QuerySize(address,&allocation_size));
  CHECK(allocation_size==0);
  CHECK(NativeContentStorageDriverTestAccess::Retire(owner,kernel));
  CHECK_FALSE(owner->AuthorizeCurrentCaller());
  CHECK_FALSE(owner->SetCallback(NativeContentStorageDriver::kNativeCallback));
  CHECK(owner->GetNotification(output)==0xC000009D);
  CHECK(std::memcmp(memory->TranslateVirtual(output),sentinel.data(),sentinel.size())==0);
  CHECK_FALSE(NativeContentStorageDriver::Install(owner));
  memory->SystemHeapFree(output);
  stopping_threads.clear(); // Drop actual joined XThread/PCR holders while Kernel lives.
  emulator.reset();
  REQUIRE(KernelState::shared()==nullptr);
  // Keep the retired owner alive through actual same-data-root replacement.
  auto replacement=make_runtime();
  CHECK_FALSE(owner->AuthorizeCurrentCaller());
  CHECK(owner->GetNotification(output)==0xC000009D); // No old Memory lookup.
  owner->BeginStop();CHECK(owner->Drain());
  owner.reset(); // Late destructor cannot free old addresses or use old Kernel.
  CHECK(KernelState::shared()==replacement->kernel_state());
  replacement.reset();
}
}  // namespace test
}  // namespace xe::kernel
#endif
