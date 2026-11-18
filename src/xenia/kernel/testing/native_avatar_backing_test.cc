/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_stream.h"
#include "xenia/base/byte_order.h"
#include "xenia/memory.h"
#include "xenia/kernel/native_avatar_contract.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/native_system_task_continuation.h"
namespace xe::kernel::test {
// Same exact friend definition as the existing continuation test TU.
class NativeSystemTaskContinuationTestAccess {
 public:
  using Admission = NativeSystemTaskContinuation::Admission;
};
class NativeAvatarServiceTestAccess {
 public:
  static std::shared_ptr<const NativeAvatarBackingLease> Borrow(
      Memory& memory, std::span<const NativeAvatarBackingRange> ranges) {
    return memory.BorrowNativeAvatarBacking(ranges);
  }
};
namespace {
constexpr uint32_t kRW = kMemoryProtectRead | kMemoryProtectWrite;
}
TEST_CASE("Avatar backing borrow preserves existing virtual pages without owning suballocations",
          "[kernel][native_avatar][memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t at = 0x90010000;
  auto* heap = memory.LookupHeap(at);
  REQUIRE(heap->AllocFixed(at,0x3000,0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,kRW));
  const std::array ranges{NativeAvatarBackingRange{at+0xFF0,0x20,true}};
  auto borrowed = NativeAvatarServiceTestAccess::Borrow(memory,ranges);
  REQUIRE(borrowed);
  REQUIRE(borrowed->current());
  // Both partially covered pages are retained. Unrelated bytes/pages are not
  // allocation authority; direct guest writes remain the original API's duty.
  std::memset(memory.TranslateVirtual(at+0xFF0),0x5A,0x20);
  CHECK_FALSE(heap->Decommit(at,0x1000));
  CHECK_FALSE(heap->Decommit(at+0x1000,0x1000));
  CHECK_FALSE(heap->Release(at));
  CHECK_FALSE(heap->Protect(at+0x1000,0x1000,kMemoryProtectNoAccess));
  CHECK_FALSE(heap->AllocFixed(at+0x1000,0x1000,0x1000,
                                kMemoryAllocationCommit,kRW));
  std::array<uint8_t,16> untouched{};
  ByteStream save(untouched.data(),untouched.size());
  CHECK_FALSE(memory.Save(&save)); CHECK(save.offset() == 0);
  CHECK_FALSE(memory.Restore(&save)); CHECK(save.offset() == 0);
  CHECK_FALSE(heap->Save(&save)); CHECK(save.offset() == 0);
  CHECK_FALSE(heap->Restore(&save)); CHECK(save.offset() == 0);
  CHECK_FALSE(memory.Reset());
  CHECK(memory.QueryRangeAccess(at,0x3000) == memory::PageAccess::kReadWrite);
  CHECK(heap->Decommit(at+0x2000,0x1000));
  borrowed.reset();
  CHECK(heap->Protect(at+0x1000,0x1000,kMemoryProtectRead));
  CHECK(heap->Release(at));
}
TEST_CASE("Avatar backing borrow does not retag title physical memory and covers real aliases",
          "[kernel][native_avatar][memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  auto* alias = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE1000000));
  uint32_t at = 0;
  REQUIRE(alias->AllocRange(0xE1000000,0xE1400000,0x2000,0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,kRW,true,&at));
  const uint32_t physical = memory.GetPhysicalAddress(at);
  const std::array ranges{NativeAvatarBackingRange{at+0x100,0x1800,true}};
  auto borrowed = NativeAvatarServiceTestAccess::Borrow(memory,ranges);
  REQUIRE(borrowed);
  const std::array physical_range{memory::PhysicalRange{physical,0x2000}};
  CHECK(memory.PinOwnedPhysicalRanges({7,11,23,0x30001000},physical_range).status ==
        memory::PhysicalLeaseStatus::kUnowned);
  CHECK_FALSE(alias->Release(at));
  CHECK_FALSE(alias->Decommit(at,0x2000));
  CHECK_FALSE(alias->Protect(at,0x1000,kMemoryProtectRead));
  CHECK_FALSE(memory.GetPhysicalHeap()->Release(physical));
  CHECK_FALSE(memory.GetPhysicalHeap()->Decommit(physical,0x2000));
  CHECK_FALSE(memory.GetPhysicalHeap()->Protect(physical,0x1000,kMemoryProtectRead));
  auto* other_alias = static_cast<PhysicalHeap*>(memory.LookupHeap(0xA0000000));
  const uint32_t other = 0xA0000000u + physical;
  CHECK_FALSE(other_alias->AllocFixed(other,0x2000,0x10000,
                                      kMemoryAllocationCommit,kRW));
  borrowed.reset();
  CHECK(alias->Release(at));
}
TEST_CASE("Avatar borrow refuses invalid, inaccessible and cross-parent ranges",
          "[kernel][native_avatar][memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  auto* heap = memory.LookupHeap(0x90010000);
  REQUIRE(heap->AllocFixed(0x90010000,0x1000,0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,kRW));
  REQUIRE(heap->AllocFixed(0x90011000,0x1000,0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,kMemoryProtectRead));
  for (const auto range : {NativeAvatarBackingRange{0,4,true},
                           NativeAvatarBackingRange{0x90010000,0,true},
                           NativeAvatarBackingRange{UINT32_MAX,2,true},
                           NativeAvatarBackingRange{0x90010FFF,2,true},
                           NativeAvatarBackingRange{0x90011000,4,true}}) {
    const std::array ranges{range};
    CHECK_FALSE(NativeAvatarServiceTestAccess::Borrow(memory,ranges));
  }
  const std::array readable{NativeAvatarBackingRange{0x90011000,4,false}};
  auto borrowed = NativeAvatarServiceTestAccess::Borrow(memory,readable);
  REQUIRE(borrowed);
  CHECK_FALSE(heap->Protect(0x90011000,0x1000,kRW));
  borrowed.reset();
  CHECK(heap->Release(0x90011000)); CHECK(heap->Release(0x90010000));
}
TEST_CASE("Escaped avatar backing token is invalid after actual Memory teardown",
          "[kernel][native_avatar][memory]") {
  std::shared_ptr<const NativeAvatarBackingLease> retained;
  {
    Memory memory;
    REQUIRE(memory.Initialize());
    auto* heap = memory.LookupHeap(0x90010000);
    REQUIRE(heap->AllocFixed(0x90010000,0x1000,0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,kRW));
    const std::array range{NativeAvatarBackingRange{0x90010000,0x1000,true}};
    retained = NativeAvatarServiceTestAccess::Borrow(memory,range);
    REQUIRE(retained); REQUIRE(retained->current());
  }
  CHECK_FALSE(retained->current());
  retained.reset();  // No retired Memory pointer is dereferenced.
}

TEST_CASE("Native stop policy retains real borrowed pages across actual OS cleanup join",
          "[kernel][native_task_continuation][stop_lifecycle][memory]") {
  // Real backing and a real OS host-thread cleanup tail. This is retention
  // coverage, never a fabricated native Schedule/Task/Complete or Kernel stop.
  Memory memory; REQUIRE(memory.Initialize());
  constexpr uint32_t at=0x90010000;
  auto* heap=memory.LookupHeap(at);
  REQUIRE(heap->AllocFixed(at,0x1000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,kRW));
  std::memset(memory.TranslateVirtual(at),0x5A,0x1000);
  const std::array ranges{NativeAvatarBackingRange{at,0x1000,true}};
  struct Payload {std::shared_ptr<const NativeAvatarBackingLease> backing;};
  auto payload=std::make_shared<Payload>();
  payload->backing=NativeAvatarServiceTestAccess::Borrow(memory,ranges);
  REQUIRE(payload->backing);
  std::weak_ptr<Payload> weak=payload;
  auto cleanup_entered=threading::Event::CreateManualResetEvent(false);
  auto release_cleanup=threading::Event::CreateManualResetEvent(false);
  REQUIRE(cleanup_entered);REQUIRE(release_cleanup);
  using Admission=NativeSystemTaskContinuationTestAccess::Admission;
  {
    Admission admission;
    REQUIRE(admission.Claim(Admission::Role::kStartup));
    // Host metadata ticket only, never a native Task/handle/source receipt.
    REQUIRE(admission.BindOwner(0x12345,payload));
    auto native=threading::Thread::Create({},
        [held=std::move(payload),entered=cleanup_entered.get(),
         release=release_cleanup.get()]() mutable {
          struct Tail {
            std::shared_ptr<Payload> held;
            threading::Event* entered;
            threading::Event* release;
            ~Tail(){entered->Set();threading::Wait(release,false);}
          } tail{std::move(held),entered,release};
          // The user body now ends, but actual OS cleanup has not returned.
        });
    REQUIRE(native);
    struct Join {
      threading::Event* release;threading::Thread* thread;
      ~Join(){release->Set();threading::Wait(thread,false);}
    } join{release_cleanup.get(),native.get()};
    CHECK(threading::Wait(cleanup_entered.get(),false,std::chrono::seconds(2))==
          threading::WaitResult::kSuccess);
    admission.stopping=true;admission.Poison();admission.Finish(false,false);
    CHECK(admission.uncertain);
    CHECK_FALSE(admission.Claim(Admission::Role::kPostBootDiagnostic));
    CHECK(threading::Wait(native.get(),false,std::chrono::milliseconds(0))==
          threading::WaitResult::kTimeout);
    CHECK_FALSE(heap->Release(at));
    release_cleanup->Set();
    REQUIRE(threading::Wait(native.get(),false)==threading::WaitResult::kSuccess);
    // The OS join alone does not release an uncertain native owner's pages.
    CHECK_FALSE(weak.expired());CHECK_FALSE(heap->Release(at));
    CHECK(memory.QueryRangeAccess(at,0x1000)==memory::PageAccess::kReadWrite);
    CHECK(*memory.TranslateVirtual<const uint8_t*>(at)==0x5A);
    CHECK(*memory.TranslateVirtual<const uint8_t*>(at+0xFFF)==0x5A);
  }
  // Actual host metadata-owner destruction is now safe in this fixture. It
  // does not stand for Service's separate actual CPU/IO/GPU retirement proof.
  CHECK(weak.expired());CHECK(heap->Release(at));
}


TEST_CASE("Avatar animation borrows original control and payload without freezing their values",
          "[kernel][native_avatar][memory][native_avatar_animation]") {
  Memory memory;REQUIRE(memory.Initialize());
  constexpr uint32_t control=0x90010000,data=0x90020000,id=0x90030000,capacity=0x4000;
  auto* heap=memory.LookupHeap(control);
  REQUIRE(heap->AllocFixed(control,0x6000,0x1000,kMemoryAllocationReserve|kMemoryAllocationCommit,kRW));
  REQUIRE(heap->AllocFixed(data,capacity,0x1000,kMemoryAllocationReserve|kMemoryAllocationCommit,kRW));
  REQUIRE(heap->AllocFixed(id,0x1000,0x1000,kMemoryAllocationReserve|kMemoryAllocationCommit,kRW));
  // Original retail control fields, written at independently recovered
  // literal offsets. This fixture exercises real mapped control bytes.
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(control+0x5988),capacity);
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(control+0x598C),data);
  const auto captured=CaptureNativeAvatarAnimationBuffers(control,
      xe::load_and_swap<uint32_t>(memory.TranslateVirtual(control+NativeAvatarAnimationBuffers::kCapacityOffset)),
      xe::load_and_swap<uint32_t>(memory.TranslateVirtual(control+NativeAvatarAnimationBuffers::kDataOffset)));
  REQUIRE(captured);
  const std::array ranges{NativeAvatarBackingRange{id,16,false},
      NativeAvatarBackingRange{captured->control,NativeAvatarAnimationBuffers::kControlBytes,true},
      NativeAvatarBackingRange{captured->data,captured->capacity,true}};
  auto borrowed=NativeAvatarServiceTestAccess::Borrow(memory,ranges);REQUIRE(borrowed);
  CHECK_FALSE(heap->Release(control));CHECK_FALSE(heap->Release(data));CHECK_FALSE(heap->Release(id));
  // Actual guest writes are still possible. The production ingress policy can
  // detect an observed changed value; a lease itself is no value freeze.
  *memory.TranslateVirtual<uint8_t*>(data)=0x5A;
  *memory.TranslateVirtual<uint8_t*>(control+NativeAvatarAnimationBuffers::kControlBytes-1)=0xA5;
  CHECK(captured->Matches(control,capacity,data));
  CHECK_FALSE(captured->Matches(control,
      xe::load_and_swap<uint32_t>(memory.TranslateVirtual(control+0x5988)),
      xe::load_and_swap<uint32_t>(memory.TranslateVirtual(control+0x598C))));
  CHECK_FALSE(captured->Matches(control,capacity-1,data));
  CHECK_FALSE(captured->Matches(control,capacity,data+4));
  CHECK_FALSE(CaptureNativeAvatarAnimationBuffers(UINT32_MAX-0x598E,capacity,data));
  CHECK_FALSE(CaptureNativeAvatarAnimationBuffers(control,0x200,UINT32_MAX-0x100));
  CHECK_FALSE(CaptureNativeAvatarAnimationBuffers(control,capacity,0));
  CHECK(CaptureNativeAvatarAnimationBuffers(control,0,0));
  borrowed.reset();
  CHECK(*memory.TranslateVirtual<uint8_t*>(data)==0x5A);
  CHECK(*memory.TranslateVirtual<uint8_t*>(control+NativeAvatarAnimationBuffers::kControlBytes-1)==0xA5);
  CHECK(heap->Release(control));CHECK(heap->Release(data));CHECK(heap->Release(id));
}

}  // namespace xe::kernel::test
