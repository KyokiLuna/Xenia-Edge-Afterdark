/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include "xenia/kernel/native_identity_tile_work.h"
#include "xenia/kernel/native_identity_image_frame.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/xboxkrnl/ob_symbolic_link.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe::kernel::util::test {
class NativeImagePublicationTestAccess final {
 public:
  static bool Entered(const NativeIdentityTilePublication& p){return p.stores_entered_;}
  static bool Committed(const NativeIdentityTilePublication& p){return p.stores_committed_;}
};
namespace {
struct Fixture {
  static constexpr uint32_t v = 0x90028000, p = 0xE1000000;
  Memory memory;
  void* provider = nullptr;
  bool unlocked = false;
  uint32_t calls = 0;
  Fixture() {
    REQUIRE(memory.Initialize());
    memory.SetPhysicalAliasSkipHostProtect(true);
    REQUIRE(memory.LookupHeap(v)->AllocFixed(
        v, 0x2000, 0x1000, kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(memory.LookupHeap(p)->AllocFixed(
        p, 0x2000, 0x1000, kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(v), 0xA5, 0x2000);
    std::memset(memory.TranslatePhysical(memory.GetPhysicalAddress(p)), 0x31,
                0x2000);
  }
  ~Fixture() {
    if (provider) {
      memory.UnregisterPhysicalMemoryReadCallback(provider);
    }
  }
  void RevokeDescriptorDuringProvider() {
    provider = memory.RegisterPhysicalMemoryReadCallback(
        [](void* data, uint32_t start, uint32_t size,
           Memory::PhysicalAccess access) {
          auto& f = *static_cast<Fixture*>(data);
          const uint32_t physical = f.memory.GetPhysicalAddress(p);
          if (start >= physical + 0x1000 ||
              uint64_t(start) + size <= physical) {
            return;
          }
          ++f.calls;
          f.unlocked |= !global_critical_region::is_held_by_current_thread();
          if (access != Memory::PhysicalAccess::kDiscard) {
            std::memset(f.memory.TranslatePhysical(physical), 0xC7, 0x1000);
          }
          REQUIRE(f.memory.LookupHeap(v)->Protect(v + 0x1000, 0x1000,
                                                  kMemoryProtectRead));
        },
        this);
    memory.EnablePhysicalMemoryAccessCallbacks(memory.GetPhysicalAddress(p),
                                               0x1000, true, true);
  }
};
}  // namespace
TEST_CASE("Guest batch rejects a later invalid span without an early store",
          "[kernel][guest_io_batch]") {
  Fixture f;
  const std::array<uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
  uint32_t destination = Fixture::v + 0x1000;
  SECTION("guard") { destination = 0xFFFFFFFC; }
  SECTION("readonly") {
    REQUIRE(f.memory.LookupHeap(destination)
                ->Protect(destination, 0x1000, kMemoryProtectRead));
  }
  const std::array<GuestIoWrite, 2> writes{
      {{Fixture::v, bytes}, {destination, bytes}}};
  CHECK(WriteGuestIoBuffers(f.memory, writes) == X_STATUS_ACCESS_VIOLATION);
  CHECK(std::all_of(f.memory.TranslateVirtual(Fixture::v),
                    f.memory.TranslateVirtual(Fixture::v + 0x2000),
                    [](uint8_t b) { return b == 0xA5; }));
  auto lock = global_critical_region::AcquireDirect();
  CHECK(WriteGuestIoBuffers(f.memory,
                            std::span<const GuestIoWrite>(writes.data(), 1)) ==
        X_STATUS_INVALID_DEVICE_REQUEST);
  CHECK(global_critical_region::is_held_by_current_thread());
}
TEST_CASE("Guest batch detects overlapping physical aliases before publication",
          "[kernel][guest_io_batch][physical]") {
  Fixture f;
  const uint32_t physical = f.memory.GetPhysicalAddress(Fixture::p);
  const uint32_t alias = 0xA0000000 + physical;
  REQUIRE(f.memory.LookupHeap(alias)->AllocFixed(
      alias & ~0xFFFFu, 0x10000, 0x10000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  std::memset(f.memory.TranslatePhysical(physical), 0x31, 0x1000);
  REQUIRE(GuestIoRangeValid(f.memory, alias + 32, 8, true));
  REQUIRE(GuestIoRangeValid(f.memory, Fixture::p + 32, 8, true));
  CHECK(GuestIoRangesOverlap(f.memory, alias + 32, 8, Fixture::p + 36, 8));
  const std::array<uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
  const std::array<GuestIoWrite, 2> writes{
      {{alias + 32, bytes}, {Fixture::p + 36, bytes}}};
  CHECK(WriteGuestIoBuffers(f.memory, writes) == X_STATUS_INVALID_PARAMETER);
  CHECK(std::all_of(f.memory.TranslatePhysical(physical),
                    f.memory.TranslatePhysical(physical + 0x1000),
                    [](uint8_t b) { return b == 0x31; }));
}
TEST_CASE("Kernel113 revalidates both outputs after physical provider refresh",
          "[kernel][namespace][guest_io_batch][physical]") {
  Fixture f;
  constexpr uint32_t oa = Fixture::v, input = oa + 0x40, text = oa + 0x100,
                     output = oa + 0x1100;
  const std::string name = "\\Device\\Disk\\package";
  auto word = [&](uint32_t a, uint32_t value) {
    xe::store_and_swap<uint32_t>(f.memory.TranslateVirtual(a), value);
  };
  auto half = [&](uint32_t a, uint16_t value) {
    xe::store_and_swap<uint16_t>(f.memory.TranslateVirtual(a), value);
  };
  word(oa, 0);
  word(oa + 4, input);
  word(oa + 8, 0x40);
  half(input, uint16_t(name.size()));
  half(input + 2, uint16_t(name.size()));
  word(input + 4, text);
  std::memcpy(f.memory.TranslateVirtual(text), name.data(), name.size());
  half(output, 0xBEEF);
  half(output + 2, 0x104);
  word(output + 4, Fixture::p + 32);
  std::array<uint8_t, 8> before{};
  std::memcpy(before.data(), f.memory.TranslateVirtual(output), 8);
  f.RevokeDescriptorDuringProvider();
  vfs::VirtualFileSystem fs;
  CHECK(xboxkrnl::TranslateSymbolicLinkGuest(f.memory, fs, oa, output) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(f.calls != 0);
  CHECK(f.unlocked);
  CHECK(std::memcmp(before.data(), f.memory.TranslateVirtual(output), 8) == 0);
  // Refresh itself is legitimate provider work, not a partial113 publication.
  CHECK(std::all_of(
      f.memory.TranslatePhysical(f.memory.GetPhysicalAddress(Fixture::p)),
      f.memory.TranslatePhysical(f.memory.GetPhysicalAddress(Fixture::p) +
                                 0x1000),
      [](uint8_t b) { return b == 0xC7; }));
}
TEST_CASE("Typed image observation preserves real aggregate stores without an installed owner", "[kernel][guest_io_batch][identity_image]"){
 Fixture f;
 const std::array<uint8_t,8> bytes{1,2,3,4,5,6,7,8};
 const GuestIoWrite write{Fixture::v,bytes};
 NativeIdentityTilePublication observed(nullptr,{{{Fixture::v,8},{0,0}}});
 REQUIRE(WriteGuestIoBuffers(f.memory,std::span(&write,1),observed)==X_STATUS_SUCCESS);
 CHECK(NativeImagePublicationTestAccess::Entered(observed));
 CHECK(NativeImagePublicationTestAccess::Committed(observed));
 CHECK(std::equal(bytes.begin(),bytes.end(),f.memory.TranslateVirtual(Fixture::v)));
}
TEST_CASE("Typed image observation cannot turn rejected aggregate IO into a byte publication", "[kernel][guest_io_batch][identity_image]"){
 Fixture f;
 const std::array<uint8_t,8> bytes{1,2,3,4,5,6,7,8};
 const std::array<GuestIoWrite,2> writes{{{Fixture::v,bytes},{0xFFFFFFFC,bytes}}};
 NativeIdentityTilePublication observed(nullptr,{{{Fixture::v,8},{0xFFFFFFFC,8}}});
 REQUIRE(WriteGuestIoBuffers(f.memory,writes,observed)==X_STATUS_ACCESS_VIOLATION);
 CHECK_FALSE(NativeImagePublicationTestAccess::Entered(observed));
 CHECK_FALSE(NativeImagePublicationTestAccess::Committed(observed));
 CHECK(std::all_of(f.memory.TranslateVirtual(Fixture::v),f.memory.TranslateVirtual(Fixture::v+8),
     [](uint8_t value){return value==0xA5;}));
}
TEST_CASE("Image publication keeps known stores when the real later physical observer throws", "[kernel][guest_io_batch][identity_image][physical]"){
 Fixture f;
 struct Pending {Memory* memory;uint32_t physical;bool provided=false,unlocked=false,locked=false;uint32_t supplies=0;};
 Pending pending{&f.memory,f.memory.GetPhysicalAddress(Fixture::p)};
 f.provider=f.memory.RegisterPhysicalMemoryReadCallback(
     [](void* context,uint32_t start,uint32_t length,Memory::PhysicalAccess access){
      auto& p=*static_cast<Pending*>(context);
      if(access==Memory::PhysicalAccess::kDiscard || start>=p.physical+0x1000 ||
         uint64_t(start)+length<=p.physical)return;
      if(!global_critical_region::is_held_by_current_thread()){
       p.unlocked=true;
       if(!p.provided){std::memset(p.memory->TranslatePhysical(p.physical),0xA5,0x1000);p.provided=true;++p.supplies;}
      }else{p.locked=true;throw std::runtime_error("original poststore observer");}
     },&pending);
 f.memory.EnablePhysicalMemoryAccessCallbacks(pending.physical,0x1000,true,true);
 const std::array<uint8_t,8> bytes{1,2,3,4,5,6,7,8};
 const GuestIoWrite write{Fixture::p+32,bytes};
 NativeIdentityTilePublication observed(nullptr,{{{Fixture::p+32,8},{0,0}}});
 CHECK_THROWS_AS(WriteGuestIoBuffers(f.memory,std::span(&write,1),observed),std::runtime_error);
 CHECK(pending.unlocked);CHECK(pending.locked);CHECK(pending.supplies==1);
 CHECK(NativeImagePublicationTestAccess::Entered(observed));
 CHECK(NativeImagePublicationTestAccess::Committed(observed));
 CHECK(std::equal(bytes.begin(),bytes.end(),f.memory.TranslatePhysical(pending.physical+32)));
 CHECK(std::all_of(f.memory.TranslatePhysical(pending.physical),f.memory.TranslatePhysical(pending.physical+32),
     [](uint8_t value){return value==0xA5;}));
 CHECK_FALSE(global_critical_region::is_held_by_current_thread());
 f.memory.UnregisterPhysicalMemoryReadCallback(f.provider);f.provider=nullptr;
}
TEST_CASE("Owned native image key output must match pointer size and full version",
          "[kernel][guest_io_batch][identity_image]") {
 Fixture f;constexpr auto text=u"memorykey://A1234567,20AF_8123456789abcdef";
 std::array<uint8_t,128> bytes{};
 for(size_t i=0;i<sizeof(u"memorykey://A1234567,20AF_8123456789abcdef")/sizeof(char16_t);++i){
  bytes[i*2]=uint8_t(text[i]>>8);bytes[i*2+1]=uint8_t(text[i]);
 }
 const GuestIoWrite write{Fixture::v,bytes};REQUIRE(WriteGuestIoBuffers(f.memory,std::span(&write,1))==X_STATUS_SUCCESS);
 const auto matches=[&](uint32_t pointer,uint32_t size,uint64_t version){
  return identity_detail::NativeImageFullKeyMatches(f.memory,Fixture::v,pointer,size,version);
 };
 REQUIRE(matches(0xA1234567,0x20AF,0x8123456789ABCDEFull));
 CHECK_FALSE(matches(0xA1234568,0x20AF,0x8123456789ABCDEFull));
 CHECK_FALSE(matches(0xA1234567,0x20B0,0x8123456789ABCDEFull));
 CHECK_FALSE(matches(0xA1234567,0x20AF,0x0123456789ABCDEFull));
 CHECK_FALSE(matches(0xA1234567,0x20AF,0x8123456789ABCDEEull));
 bytes[0]=0;bytes[1]='x';REQUIRE(WriteGuestIoBuffers(f.memory,std::span(&write,1))==X_STATUS_SUCCESS);
 CHECK_FALSE(matches(0xA1234567,0x20AF,0x8123456789ABCDEFull));
 CHECK_FALSE(identity_detail::NativeImageFullKeyMatches(f.memory,Fixture::v+1,0xA1234567,0x20AF,0x8123456789ABCDEFull));
}
TEST_CASE("Native decode copy refuses a physical alias into any future encoded bytes",
          "[kernel][guest_io_batch][identity_image][physical]") {
 Fixture f;const uint32_t physical=f.memory.GetPhysicalAddress(Fixture::p);
 const uint32_t alias=0xA0000000u+physical;
 REQUIRE(f.memory.GetPhysicalAddress(alias)==physical);
 // This destination does not touch the current 16-byte input chunk, yet
 // would overwrite later bytes of the SAME 512-byte borrowed encoded source.
 CHECK_FALSE(GuestIoRangesOverlap(f.memory,Fixture::p,16,alias+128,16));
 CHECK(GuestIoRangesOverlap(f.memory,Fixture::p,512,alias+128,16));
 CHECK_FALSE(GuestIoRangesOverlap(f.memory,Fixture::p,512,alias+1024,16));
 CHECK_FALSE(GuestIoRangesOverlap(f.memory,Fixture::p,512,Fixture::v,16));
}
}  // namespace xe::kernel::util::test
