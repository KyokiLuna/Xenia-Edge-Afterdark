/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <array>
#include <system_error>
#include <algorithm>
#include <vector>
#include <functional>
#include <stdexcept>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/native_profile_identity_request.h"
#include "xenia/kernel/native_profile_save_request.h"
#include "xenia/kernel/native_profile_save_owner.h"
#include "xenia/memory.h"
#include "xenia/kernel/native_profile_save_serialization.h"
#include "xenia/kernel/native_profile_save_semantic_uses.h"
#include "xenia/base/profile_gpd_snapshot.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/xbox.h"

namespace xe::kernel::test {
class NativeProfileIdentityRequestTestAccess {
 public:
  static auto Capture(uint16_t ordinal,std::span<const uint8_t> bytes,uint32_t flags,
      const std::array<uint64_t,4>& before,std::error_code& error){
    return NativeProfileIdentityRequest::Capture(ordinal,bytes,flags,before,error);
  }
  static bool Matches(const NativeProfileIdentityRequest& request,std::span<const uint8_t> rows){
    return request.MatchesPreparedRows(rows);
  }
};
namespace {
using Access=NativeProfileIdentityRequestTestAccess;
constexpr uint64_t kA=0xE000000000000041ull,kB=0xE000000000000042ull,kC=0xE000000000000043ull;
std::array<uint8_t,32> FourXuids(std::array<uint64_t,4> ids){
 std::array<uint8_t,32> bytes{};
 for(size_t i=0;i<ids.size();++i)xe::store_and_swap<uint64_t>(bytes.data()+8*i,ids[i]);
 return bytes;
}
}
TEST_CASE("Original native logon retains its four-XUID request independently of desired roster",
          "[native-identity][wire]"){
 const auto original=FourXuids({0,kC,0,0});std::error_code error;
 const auto add=Access::Capture(0x20B,original,4,{kA,kB,0,0},error);
 REQUIRE(add);REQUIRE_FALSE(error);
 CHECK(add->ordinal()==0x20B);CHECK(add->flags()==4);
 CHECK(add->initializer_phase(true)==NativeProfileIdentityRequest::InitializerPhase::kQueuedAsync);
 CHECK((add->desired()==std::array<uint64_t,4>{kA,kC,0,0}));
 CHECK(std::equal(add->original().begin(),add->original().end(),original.begin(),original.end()));
 const auto replacement=Access::Capture(0x20B,original,0,{kA,kB,0,0},error);
 REQUIRE(replacement);CHECK_FALSE(error);
 CHECK(replacement->initializer_phase(true)==NativeProfileIdentityRequest::InitializerPhase::kQueuedAsync);
 CHECK(replacement->initializer_phase(false)==NativeProfileIdentityRequest::InitializerPhase::kAfterOldDrain);
 CHECK((replacement->desired()==std::array<uint64_t,4>{0,kC,0,0}));
 CHECK(std::equal(replacement->original().begin(),replacement->original().end(),original.begin(),original.end()));
}
TEST_CASE("Native logon gives Add precedence and validates removal in the original slot",
          "[native-identity][wire]"){
 std::error_code error;const auto one=FourXuids({kC,0,0,0});
 const auto both=Access::Capture(0x20B,one,0xC,{kA,kB,0,0},error);
 REQUIRE(both);CHECK_FALSE(error);CHECK(both->flags()==0xC);
 CHECK(both->initializer_phase(true)==NativeProfileIdentityRequest::InitializerPhase::kQueuedAsync);
 CHECK(both->initializer_phase(false)==NativeProfileIdentityRequest::InitializerPhase::kAfterOldDrain);
 CHECK((both->desired()==std::array<uint64_t,4>{kC,kB,0,0}));
 const auto remove=Access::Capture(0x20B,FourXuids({0,kB,0,0}),8,{kA,kB,0,0},error);
 REQUIRE(remove);CHECK_FALSE(error);CHECK((remove->desired()==std::array<uint64_t,4>{kA,0,0,0}));
 CHECK(remove->initializer_phase(true)==NativeProfileIdentityRequest::InitializerPhase::kAfterOldDrain);
 CHECK_FALSE(Access::Capture(0x20B,FourXuids({kB,0,0,0}),8,{kA,kB,0,0},error));
 CHECK(error==std::errc::invalid_argument);
 CHECK_FALSE(Access::Capture(0x20B,FourXuids({kA,kA,0,0}),0,{},error));
 CHECK(error==std::errc::invalid_argument);
}
TEST_CASE("Original224 Account and device bytes must match the genuine prepared row",
          "[native-identity][wire][account]"){
 std::array<uint8_t,0x620> original{};
 xe::store_and_swap<uint64_t>(original.data(),kA);
 // Opaque payload bytes, including the final Account byte, are preserved.
 original[8]=0x8A;original[0x101]=0xD7;original[0x183]=0x6E;
 xe::store_and_swap<uint32_t>(original.data()+0x184,7);
 std::error_code error;const auto request=Access::Capture(0x224,original,0,{},error);
 REQUIRE(request);REQUIRE_FALSE(error);
 CHECK(request->original().size()==0x620);
 CHECK(std::equal(request->original().begin(),request->original().end(),original.begin(),original.end()));
 CHECK(Access::Matches(*request,original));
 auto altered=original;altered[0x183]^=1;CHECK_FALSE(Access::Matches(*request,altered));
 altered=original;xe::store_and_swap<uint32_t>(altered.data()+0x184,8);
 CHECK_FALSE(Access::Matches(*request,altered));
 altered=original;xe::store_and_swap<uint64_t>(altered.data(),kB);
 CHECK_FALSE(Access::Matches(*request,altered));
 CHECK_FALSE(Access::Matches(*request,std::span(original).first(0x61F)));
}
TEST_CASE("Unproved native logon flag families and malformed wire extents are refused before entry",
          "[native-identity][wire]"){
 const auto bytes=FourXuids({kA,0,0,0});std::error_code error;
 CHECK_FALSE(Access::Capture(0x20B,bytes,0x4000,{},error));CHECK(error==std::errc::invalid_argument);
 for (const uint32_t flags : {0x10u,0x40u,0x80u,0x100u,0x200000u,
                              0x200024u,0x80000024u,0x84u,0x400u,0x20000u,0x100000u,
                              0x120400u,0x120421u,0x12042Cu,0x1204A0u,0x1204A4u,0x1204A5u,0x120430u}) {
  CHECK_FALSE(Access::Capture(0x20B,bytes,flags,{},error));
  CHECK(error==std::errc::operation_not_supported);
 }
 CHECK_FALSE(Access::Capture(0x20B,std::span(bytes).first(31),0,{},error));CHECK(error==std::errc::invalid_argument);
 CHECK_FALSE(Access::Capture(0x224,bytes,0,{},error));CHECK(error==std::errc::invalid_argument);
 CHECK_FALSE(Access::Capture(0x999,bytes,0,{},error));CHECK(error==std::errc::invalid_argument);
}
TEST_CASE("Native popup suppression preserves original logon flags and slot semantics",
          "[native-identity][wire][logon_popup]") {
 using Phase=NativeProfileIdentityRequest::InitializerPhase;
 struct Scenario {
  uint32_t flags;
  std::array<uint64_t,4> input, desired;
  int selected;
  Phase initializer;
 };
 const std::array<uint64_t,4> before{kA,kB,0,0};
 const Scenario scenarios[]{
  {0x20,{0,0,0,kC},{0,0,0,kC},3,Phase::kQueuedAsync},
  {0x24,{0,0,kC,0},{kA,kB,kC,0},2,Phase::kQueuedAsync},
  {0x25,{0,0,kC,0},{kA,kB,kC,0},2,Phase::kAfterOldDrain},
  {0x28,{0,kB,0,0},{kA,0,0,0},-1,Phase::kAfterOldDrain},
  {0x2C,{kC,0,0,0},{kC,kB,0,0},0,Phase::kQueuedAsync},
 };
 for (const uint16_t ordinal : {uint16_t(0x20B),uint16_t(0x224)}) {
  for (const auto& scenario : scenarios) {
   INFO("ordinal=" << ordinal << " flags=" << scenario.flags);
   std::array<uint8_t,0x620> wire{};
   const size_t stride=ordinal==0x20B?8:0x188;
   const size_t extent=ordinal==0x20B?32:wire.size();
   for (size_t slot=0;slot<4;++slot) {
    xe::store_and_swap<uint64_t>(wire.data()+slot*stride,scenario.input[slot]);
    if (ordinal==0x224) {
     // Synthetic opaque Account/device bytes; no installed profile is read.
     for (size_t offset=8;offset<0x184;++offset)
      wire[slot*stride+offset]=uint8_t(offset*37+slot*19);
     xe::store_and_swap<uint32_t>(wire.data()+slot*stride+0x184,uint32_t(slot+7));
    }
   }
   const auto original=wire;
   std::error_code error;
   const auto request=Access::Capture(ordinal,std::span<const uint8_t>(wire).first(extent),
                                      scenario.flags,before,error);
   REQUIRE(request);REQUIRE_FALSE(error);
   CHECK(request->ordinal()==ordinal);CHECK(request->flags()==scenario.flags);
   // This detached request says only which native first-return phase was
   // proved. The actual caller/old-work and queued worker gates remain owned.
   CHECK(request->initializer_phase(false)==NativeProfileIdentityRequest::InitializerPhase::kAfterOldDrain);
   CHECK(request->initializer_phase(true)==scenario.initializer);
   CHECK(request->desired()==scenario.desired);
   CHECK(request->original().size()==extent);
   wire.fill(0xCD); // Real detached input ownership, not a borrowed span.
   CHECK(std::equal(request->original().begin(),request->original().end(),
                    original.begin(),original.begin()+extent));
   std::array<uint8_t,0x620> prepared{};
   if (ordinal==0x224) prepared=original;
   for (size_t slot=0;slot<4;++slot)
    xe::store_and_swap<uint64_t>(prepared.data()+slot*0x188,scenario.desired[slot]);
   CHECK(Access::Matches(*request,prepared));
   auto altered=prepared;
   xe::store_and_swap<uint64_t>(altered.data()+3*0x188,kA);
   CHECK_FALSE(Access::Matches(*request,altered));
   if (ordinal==0x224 && scenario.selected>=0) {
    const size_t offset=size_t(scenario.selected)*0x188;
    altered=prepared;altered[offset+0x183]^=1;
    CHECK_FALSE(Access::Matches(*request,altered));
    altered=prepared;altered[offset+0x187]^=1;
    CHECK_FALSE(Access::Matches(*request,altered));
   }
   CHECK_FALSE(Access::Matches(*request,std::span<const uint8_t>(prepared).first(0x61F)));
   CHECK_FALSE(Access::Capture(ordinal,std::span<const uint8_t>(original).first(extent-1),
                               scenario.flags,before,error));
   CHECK(error==std::errc::invalid_argument);
  }
 }
 // The UI-only modifier cannot excuse duplicate identities or wrong-row remove.
 std::error_code error;
 CHECK_FALSE(Access::Capture(0x20B,FourXuids({kA,kA,0,0}),0x20,{},error));
 CHECK(error==std::errc::invalid_argument);
 CHECK_FALSE(Access::Capture(0x20B,FourXuids({kB,0,0,0}),0x28,before,error));
 CHECK(error==std::errc::invalid_argument);
}
TEST_CASE("Native SignIn policy flags retain exact wire rows and original request ownership",
          "[native-identity][wire][logon_common]") {
 const std::array<uint64_t,4> before{kA,kB,0,0};
 for (const uint32_t flags : {0x00120420u,0x00120424u}) {
  const bool add=bool(flags&4u);
  const std::array<uint64_t,4> input{0,0,kC,0};
  const std::array<uint64_t,4> desired=add?std::array<uint64_t,4>{kA,kB,kC,0}:input;
  for (const uint16_t ordinal : {uint16_t(0x20B),uint16_t(0x224)}) {
   INFO("ordinal=" << ordinal << " flags=" << flags);
   std::array<uint8_t,0x620> wire{};
   const size_t stride=ordinal==0x20B?8:0x188;
   const size_t extent=ordinal==0x20B?32:wire.size();
   for (size_t slot=0;slot<4;++slot) {
    xe::store_and_swap<uint64_t>(wire.data()+slot*stride,input[slot]);
    if (ordinal==0x224) {
     for (size_t offset=8;offset<0x184;++offset)
      wire[slot*stride+offset]=uint8_t(offset*13+slot*29);
     xe::store_and_swap<uint32_t>(wire.data()+slot*stride+0x184,uint32_t(slot+5));
    }
   }
   const auto original=wire;
   std::error_code error;
   const auto request=Access::Capture(ordinal,std::span<const uint8_t>(wire).first(extent),
                                      flags,before,error);
   REQUIRE(request);REQUIRE_FALSE(error);
   CHECK(request->flags()==flags);CHECK(request->ordinal()==ordinal);
   CHECK(request->initializer_phase(true)==NativeProfileIdentityRequest::InitializerPhase::kQueuedAsync);
   CHECK(request->initializer_phase(false)==NativeProfileIdentityRequest::InitializerPhase::kAfterOldDrain);
   CHECK(request->desired()==desired);CHECK(request->original().size()==extent);
   wire.fill(0xCD);
   CHECK(std::equal(request->original().begin(),request->original().end(),
                    original.begin(),original.begin()+extent));
   std::array<uint8_t,0x620> prepared{};
   if (ordinal==0x224) prepared=original;
   for (size_t slot=0;slot<4;++slot)
    xe::store_and_swap<uint64_t>(prepared.data()+slot*0x188,desired[slot]);
   CHECK(Access::Matches(*request,prepared));
   auto wrong=prepared;
   xe::store_and_swap<uint64_t>(wrong.data()+2*0x188,kA);
   CHECK_FALSE(Access::Matches(*request,wrong));
   if (ordinal==0x224) {
    wrong=prepared;wrong[2*0x188+0x183]^=1;
    CHECK_FALSE(Access::Matches(*request,wrong));
    wrong=prepared;wrong[2*0x188+0x187]^=1;
    CHECK_FALSE(Access::Matches(*request,wrong));
   }
   CHECK_FALSE(Access::Matches(*request,std::span<const uint8_t>(prepared).first(0x61F)));
   CHECK_FALSE(Access::Capture(ordinal,std::span<const uint8_t>(original).first(extent-1),
                               flags,before,error));
   CHECK(error==std::errc::invalid_argument);
   CHECK_FALSE(Access::Capture(ordinal,std::span<const uint8_t>(original).first(extent),
                               flags|0x4000u,before,error));
   CHECK(error==std::errc::invalid_argument);
  }
  std::error_code error;
  CHECK_FALSE(Access::Capture(0x20B,FourXuids({kC,kC,0,0}),flags,before,error));
  CHECK(error==std::errc::invalid_argument);
 }
}
class NativeProfileSaveRequestTestAccess {
 public:
  static auto CaptureHeader(Memory& memory,uint32_t context,std::error_code& error) {
    return NativeProfileSaveOwner::CaptureNativeHeaderBefore(memory,context,error);
  }
  static auto CaptureIndex(Memory& memory,uint32_t stream,uint32_t handle,std::error_code& error) {
    return NativeProfileSaveOwner::CaptureNativeGpdIndex(memory,stream,handle,error);
  }
  static bool SelectionExpansion(const NativeProfileSaveRequest& request,std::span<const uint8_t> expanded) {
    return request.MatchesOriginalSelectionExpansion(expanded);
  }
  static auto CaptureRam(Memory& memory,uint32_t message,
      std::span<const uint8_t> descriptor,std::error_code& error) {
    return NativeProfileSaveRequest::CaptureOriginalFb(message,descriptor,
        [&](uint32_t address,std::span<uint8_t> output) {
          return NativeProfileSaveOwner::ReadOwnedPayload(memory,address,output);
        },error);
  }
  static auto Capture(uint32_t message, std::span<const uint8_t> descriptor,
      const std::function<uint32_t(uint32_t, std::span<uint8_t>)>& read, std::error_code& error) {
    return NativeProfileSaveRequest::CaptureOriginalFb(message, descriptor, read, error);
  }
};
namespace {
void SaveWord(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i != 4; ++i) { bytes[offset+i] = uint8_t(value >> ((3-i)*8)); }
}
std::array<uint8_t,40> SaveImage(uint32_t type, uint32_t pointer, uint32_t count) {
  std::array<uint8_t,40> wire{};
  SaveWord(wire, 0, type); SaveWord(wire, 4, 0x12345678);
  SaveWord(wire, 8, 0xFEDCBA98); SaveWord(wire, 12, 0x76543210);
  SaveWord(wire, 0x10, 2); SaveWord(wire, 0x14, pointer); SaveWord(wire, 0x1C, count);
  SaveWord(wire, 0x20, 0xCAFEBABE); SaveWord(wire, 0x24, 0x10203040);
  return wire;
}
}
TEST_CASE("Native Header pre-edit capture owns both real native ranges and excludes the control gap",
          "[native-save][native_header][memory][lifetime]") {
  Memory memory;REQUIRE(memory.Initialize());
  constexpr uint32_t context=0x90070000,h=0x971A,r=0xA000;
  REQUIRE(memory.LookupHeap(context)->AllocFixed(context,0xB000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
  auto* ram=memory.TranslateVirtual<uint8_t*>(context);
  std::fill(ram,ram+0xB000,0xED);
  std::vector<uint8_t> expected(r);
  for(size_t n=0;n<expected.size();++n)expected[n]=uint8_t((n*37u)^(n>>7));
  xe::store_and_swap<uint32_t>(expected.data(),0x434F4E20);
  xe::store_and_swap<uint32_t>(expected.data()+0x340,h);
  std::copy_n(expected.begin(),h,ram+0x38);
  std::copy(expected.begin()+h,expected.end(),ram+0x98AC);
  std::error_code error;
  auto captured=NativeProfileSaveRequestTestAccess::CaptureHeader(memory,context,error);
  REQUIRE(captured);REQUIRE_FALSE(error);CHECK(*captured==expected);
  // This deliberately different live native control state must not be copied
  // as raw padding. The actual stored source image survives later native reuse.
  std::fill(ram+0x9752,ram+0x98AC,0x12);
  auto repeated=NativeProfileSaveRequestTestAccess::CaptureHeader(memory,context,error);
  REQUIRE(repeated);CHECK(*repeated==expected);
  std::fill(ram,ram+0xB000,0x33);
  CHECK(*captured==expected);CHECK(*repeated==expected);
}
TEST_CASE("Native Header pre-edit capture refuses missing alignment bytes malformed layout and address wrap",
          "[native-save][native_header][memory][failure]") {
  Memory memory;REQUIRE(memory.Initialize());
  constexpr uint32_t context=0x90080000;
  REQUIRE(memory.LookupHeap(context)->AllocFixed(context,0xB000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
  auto* ram=memory.TranslateVirtual<uint8_t*>(context);std::fill(ram,ram+0xB000,0xA7);
  xe::store_and_swap<uint32_t>(ram+0x38,0x434F4E20);
  xe::store_and_swap<uint32_t>(ram+0x378,0x971A);
  std::error_code error;
  // Full first H is readable, but the independently retained second range is
  // genuinely incomplete. No zeros, H-only success or partial vector escapes.
  REQUIRE(memory.LookupHeap(context)->Protect(context+0xA000,0x1000,kMemoryProtectNoAccess));
  CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureHeader(memory,context,error));
  CHECK(error==std::errc::bad_address);
  REQUIRE(memory.LookupHeap(context)->Protect(context+0xA000,0x1000,kMemoryProtectRead|kMemoryProtectWrite));
  xe::store_and_swap<uint32_t>(ram+0x378,0xA000);
  CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureHeader(memory,context,error));
  CHECK(error==std::errc::operation_not_supported);
  xe::store_and_swap<uint32_t>(ram+0x378,0x971A);ram[0x38]^=1;
  CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureHeader(memory,context,error));
  CHECK(error==std::errc::bad_message);
  CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureHeader(memory,0xFFFFF000,error));
  CHECK(error==std::errc::invalid_argument);
  CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureHeader(memory,context+1,error));
  CHECK(error==std::errc::invalid_argument);
}
TEST_CASE("Native GPD cached prefix capture owns actual cross-page RAM and refuses mismatched control extents",
          "[native-save][native_gpd][memory][abi][lifetime]") {
  Memory memory;REQUIRE(memory.Initialize());
  constexpr uint32_t base=0x90087000,stream=base,index=base+0x1FF0,handle=0x1234ABCD;
  REQUIRE(memory.LookupHeap(base)->AllocFixed(base,0x4000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
  auto* control=memory.TranslateVirtual<uint8_t*>(stream);
  std::vector<uint8_t> expected(24+18);
  for(size_t n=0;n<expected.size();++n)expected[n]=uint8_t((n*29u)^(n>>2));
  SaveWord(expected,0,0x58444246);SaveWord(expected,4,1);SaveWord(expected,8,1);
  SaveWord(expected,12,0);SaveWord(expected,16,0);SaveWord(expected,20,0);
  SaveWord(std::span(control,16),0,0x81630230);SaveWord(std::span(control,16),4,index);
  SaveWord(std::span(control,16),8,uint32_t(expected.size()));SaveWord(std::span(control,16),12,handle);
  std::copy(expected.begin(),expected.end(),memory.TranslateVirtual<uint8_t*>(index));
  std::error_code error;
  SECTION("complete actual prefix remains owned after native RAM and stream reuse") {
    auto captured=NativeProfileSaveRequestTestAccess::CaptureIndex(memory,stream,handle,error);
    REQUIRE(captured);REQUIRE_FALSE(error);REQUIRE(*captured==expected);
    std::fill(control,control+16,0xAA);
    std::fill(memory.TranslateVirtual<uint8_t*>(index),memory.TranslateVirtual<uint8_t*>(index)+expected.size(),0xCC);
    CHECK(*captured==expected);
  }
  SECTION("real inaccessible second page cannot escape as a short prefix or zero fill") {
    REQUIRE(memory.LookupHeap(base)->Protect(base+0x2000,0x1000,kMemoryProtectNoAccess));
    CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureIndex(memory,stream,handle,error));
    CHECK(error==std::errc::bad_address);
  }
  SECTION("foreign retained handle is not the actual stream File") {
    CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureIndex(memory,stream,handle+4,error));
    CHECK(error==std::errc::bad_message);
  }
  SECTION("native capacity arithmetic must agree with actual retained prefix extent") {
    SaveWord(std::span(memory.TranslateVirtual<uint8_t*>(index),expected.size()),8,UINT32_MAX);
    CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureIndex(memory,stream,handle,error));
    CHECK(error==std::errc::bad_message);
  }
  SECTION("wrapped native stream address and inherited host GCRC are rejected") {
    CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureIndex(memory,0xFFFFFFF8,handle,error));
    CHECK(error==std::errc::invalid_argument);
    auto guard=global_critical_region::AcquireDirect();
    CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureIndex(memory,stream,handle,error));
    CHECK(error==std::errc::operation_would_block);
  }
  // This tests real Memory capture ABI only; matching bytes never grant
  // original caller/held-CS/source/File/stage mutation or native completion.
}
TEST_CASE("Native SAVE actual RAM adapter captures nonempty chunked inputs as status success and refuses inaccessible backing",
          "[native-save][wire][memory][lifetime]") {
  Memory memory;REQUIRE(memory.Initialize());
  constexpr uint32_t page=0x90049000;
  REQUIRE(memory.LookupHeap(page)->AllocFixed(page,0x2000,0x1000,
      kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
  std::vector<uint8_t> expected(777);
  for(size_t at=0;at<expected.size();++at)expected[at]=uint8_t((at*37u)^(at>>3));
  auto* ram=memory.TranslateVirtual<uint8_t*>(page+0x40);
  std::copy(expected.begin(),expected.end(),ram);
  auto wire=SaveImage(0,page+0x40,uint32_t(expected.size()));const auto original=wire;
  std::error_code error;
  const auto copy=NativeProfileSaveRequestTestAccess::CaptureRam(memory,0xB0031,wire,error);
  REQUIRE(copy);REQUIRE_FALSE(error);
  CHECK(copy->large().bytes==expected);CHECK(copy->large().declared_bytes==expected.size());
  CHECK(copy->item()==UINT64_C(0xFEDCBA9876543210));
  std::fill(ram,ram+expected.size(),0x33);wire.fill(0x55);
  CHECK(copy->large().bytes==expected);
  CHECK(std::ranges::equal(copy->descriptor(),original));
  // This genuinely allocated second page becomes unreadable. The actual
  // native RAM adapter must refuse the crossing input, not publish a partial
  // payload or confuse its copied-byte count with the callback's status.
  REQUIRE(memory.LookupHeap(page)->Protect(page+0x1000,0x1000,kMemoryProtectNoAccess));
  const auto crossing=SaveImage(0,page+0xF80,256);
  CHECK_FALSE(NativeProfileSaveRequestTestAccess::CaptureRam(memory,0xB0031,crossing,error));
  CHECK(error==std::errc::bad_address);
  CHECK(copy->large().bytes==expected);
}
TEST_CASE("Native SAVE image capture owns exact nested bytes full key and opaque descriptor across caller reuse",
          "[native-save][wire][lifetime]") {
  const std::vector<uint8_t> expected{0,0xEE,3,0xFF}; auto payload = expected;
  auto wire = SaveImage(0,0x12345000,uint32_t(payload.size())); const auto original = wire;
  unsigned reads = 0; std::error_code error;
  auto copy = NativeProfileSaveRequestTestAccess::Capture(0xB0031,wire,
      [&](uint32_t at,std::span<uint8_t> out) {
        ++reads; REQUIRE(at == 0x12345000); REQUIRE(out.size() == payload.size());
        std::copy(payload.begin(),payload.end(),out.begin()); return uint32_t(X_ERROR_SUCCESS);
      },error);
  REQUIRE(copy); REQUIRE_FALSE(error); REQUIRE(reads == 1);
  wire.fill(0x55); payload.assign(9,0x44);
  REQUIRE(copy->family() == NativeProfileSaveRequest::Family::kImage);
  REQUIRE(copy->user_slot() == 2); REQUIRE(copy->raw_title() == 0x12345678);
  REQUIRE(copy->image_key() == 0xFEDCBA9876543210ull);
  REQUIRE(copy->large().bytes == expected); REQUIRE_FALSE(copy->large().null_pointer());
  REQUIRE(std::equal(copy->descriptor().begin(),copy->descriptor().end(),original.begin(),original.end()));
  for (const auto [type,key] : {std::pair{1u,0x8000ull}, {6u,0x8003ull}, {15u,0x8007ull}}) {
    auto mapped = NativeProfileSaveRequestTestAccess::Capture(0xB0031,SaveImage(type,0x1000,0),{},error);
    REQUIRE(mapped); REQUIRE_FALSE(error); REQUIRE(mapped->image_key() == key);
  }
}
TEST_CASE("Native SAVE NULL deletion ignores count while nonnull zero count remains empty write without memory access",
          "[native-save][wire][delete]") {
  std::error_code error; unsigned reads = 0;
  const auto forbidden = [&](uint32_t,std::span<uint8_t>) { ++reads; return uint32_t(X_ERROR_FUNCTION_FAILED); };
  auto deleted = NativeProfileSaveRequestTestAccess::Capture(0xB0031,SaveImage(15,0,UINT32_MAX),forbidden,error);
  REQUIRE(deleted); REQUIRE_FALSE(error); REQUIRE(deleted->large().null_pointer());
  REQUIRE(deleted->large().declared_bytes == UINT32_MAX); REQUIRE(deleted->large().bytes.empty());
  auto empty = NativeProfileSaveRequestTestAccess::Capture(0xB0031,SaveImage(15,0xFFFFF000,0),forbidden,error);
  REQUIRE(empty); REQUIRE_FALSE(error); REQUIRE_FALSE(empty->large().null_pointer());
  REQUIRE(empty->large().declared_bytes == 0); REQUIRE(empty->large().bytes.empty());
  REQUIRE(deleted->image_key() == empty->image_key()); REQUIRE(reads == 0);
}
TEST_CASE("Native SAVE pair captures both actual pointer count tuples and refuses a bad second input as one request",
          "[native-save][wire][pair]") {
  std::array<uint8_t,44> wire{};
  SaveWord(wire,0,1); SaveWord(wire,4,3); SaveWord(wire,8,0xFFFE0854);
  SaveWord(wire,12,0x12345678); SaveWord(wire,16,0x9ABCDEF0); SaveWord(wire,20,0x32);
  SaveWord(wire,24,0x12340000); SaveWord(wire,28,5);
  SaveWord(wire,32,0x56780000); SaveWord(wire,36,3); SaveWord(wire,40,0xCAFEBABE);
  const auto original = wire; std::error_code error; std::vector<uint32_t> addresses;
  bool second_valid = false;
  const auto reader = [&](uint32_t at,std::span<uint8_t> out) {
    addresses.push_back(at);
    if (at == 0x12340000) { REQUIRE(out.size() == 5); std::fill(out.begin(),out.end(),0x63); return uint32_t(X_ERROR_SUCCESS); }
    REQUIRE(at == 0x56780000); REQUIRE(out.size() == 3);
    if (!second_valid) { return uint32_t(X_ERROR_FUNCTION_FAILED); }
    std::fill(out.begin(),out.end(),0xAE); return uint32_t(X_ERROR_SUCCESS);
  };
  REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0037,wire,reader,error));
  REQUIRE(error == std::errc::bad_address); REQUIRE(wire == original);
  REQUIRE((addresses == std::vector<uint32_t>{0x12340000,0x56780000}));
  second_valid = true; addresses.clear();
  const auto copy = NativeProfileSaveRequestTestAccess::Capture(0xB0037,wire,reader,error);
  REQUIRE(copy); REQUIRE_FALSE(error); wire.fill(0x55);
  REQUIRE(copy->user_slot() == 3); REQUIRE(copy->flags() == 0x32); REQUIRE(copy->raw_title() == 0xFFFE0854);
  REQUIRE(copy->large_id() == 0x12345678); REQUIRE(copy->small_id() == 0x9ABCDEF0);
  REQUIRE(copy->large().bytes == std::vector<uint8_t>(5,0x63));
  REQUIRE(copy->small().bytes == std::vector<uint8_t>(3,0xAE));
  REQUIRE(std::equal(copy->descriptor().begin(),copy->descriptor().end(),original.begin(),original.end()));
}
TEST_CASE("Native SAVE selection preserves original flags and sync preserves mask semantics without inventing user index or flush",
          "[native-save][wire][mask]") {
  std::array<uint8_t,24> selection{};
  for (unsigned i = 0; i != 6; ++i) { SaveWord(selection,i*4,i+1); }
  SaveWord(selection,4,2); SaveWord(selection,8,0); SaveWord(selection,20,0x11);
  std::error_code error;
  const auto selected = NativeProfileSaveRequestTestAccess::Capture(0xB0033,selection,{},error);
  REQUIRE(selected); REQUIRE_FALSE(error); REQUIRE(selected->family() == NativeProfileSaveRequest::Family::kSelection);
  REQUIRE(selected->user_slot() == 2); REQUIRE(selected->raw_title() == 0);
  REQUIRE(selected->for_enumerate() == 1); REQUIRE(selected->flags() == 0x11);
  REQUIRE(selected->large_id() == 4); REQUIRE(selected->small_id() == 5);
  std::array<uint8_t,44> expanded{};
  std::copy(selection.begin(),selection.end(),expanded.begin());
  REQUIRE(NativeProfileSaveRequestTestAccess::SelectionExpansion(*selected,expanded));
  auto wrong=expanded;wrong[5]^=1;
  REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::SelectionExpansion(*selected,wrong));
  wrong=expanded;wrong.back()=1;
  REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::SelectionExpansion(*selected,wrong));
  REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::SelectionExpansion(*selected,std::span(expanded).first(43)));
  std::array<uint8_t,4> mask{}; SaveWord(mask,0,0x8000000A);
  const auto sync = NativeProfileSaveRequestTestAccess::Capture(0xB0004,mask,{},error);
  REQUIRE(sync); REQUIRE_FALSE(error); REQUIRE(sync->raw_user_mask() == 0x8000000A);
  REQUIRE(sync->selected_user_mask() == 0xA); REQUIRE_FALSE(sync->image_key());
}
TEST_CASE("Native SAVE refuses invalid ABI extent slot and payload bounds before reads and reports reader failure truthfully",
          "[native-save][wire][failure]") {
  auto image = SaveImage(0,0x1000,1); std::error_code error; unsigned reads = 0;
  const auto reader = [&](uint32_t,std::span<uint8_t>) { ++reads; return uint32_t(X_ERROR_FUNCTION_FAILED); };
  SECTION("short descriptor") { REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0031,std::span(image).first(39),reader,error)); }
  SECTION("unknown message") { REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xBFFFF,image,reader,error)); }
  SECTION("unknown image type") { SaveWord(image,0,2); REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0031,image,reader,error)); }
  SECTION("invalid user") { SaveWord(image,0x10,4); REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0031,image,reader,error)); }
  SECTION("bounded size") { SaveWord(image,0x1C,(64u<<20)+1); REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0031,image,reader,error)); }
  SECTION("pointer overflow") { SaveWord(image,0x14,0xFFFFFFFE); SaveWord(image,0x1C,4); REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0031,image,reader,error)); }
  SECTION("actual reader refusal") { REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0031,image,reader,error)); REQUIRE(error == std::errc::bad_address); REQUIRE(reads == 1); reads = 0; }
  SECTION("actual reader throws") {
    const auto throws = [](uint32_t,std::span<uint8_t>) -> uint32_t { throw std::runtime_error("source failed"); };
    REQUIRE_FALSE(NativeProfileSaveRequestTestAccess::Capture(0xB0031,image,throws,error)); REQUIRE(error == std::errc::io_error);
  }
  REQUIRE(error); REQUIRE(reads == 0);
}
namespace {
// Independent raw fixture: three records (opaque high64-bit image, unknown
// namespace, present-empty image). This intentionally differs in capacity,
// entry order and payload placement; it never invokes a production serializer.
std::vector<uint8_t> NativeSaveRawGpd(bool relayout) {
  const uint32_t entry_capacity = relayout ? 6 : 4, free_capacity = relayout ? 3 : 2;
  const size_t prefix = 24 + size_t(entry_capacity)*18 + size_t(free_capacity)*8;
  std::vector<uint8_t> bytes(prefix+7,0);
  SaveWord(bytes,0,0x58444246); SaveWord(bytes,4,0x10000);
  SaveWord(bytes,8,entry_capacity); SaveWord(bytes,12,3);
  SaveWord(bytes,16,free_capacity); SaveWord(bytes,20,1);
  struct Entry {uint16_t section; uint64_t key; uint32_t offset,size;};
  const std::array<Entry,3> rows{{{2,0xFEDCBA9876543210ull,relayout?3u:0u,4},
                               {0xFECD,42,relayout?0u:4u,3}, {2,7,7,0}}};
  for (size_t i=0;i<rows.size();++i) {
    const auto& e=rows[relayout?rows.size()-1-i:i]; const size_t at=24+i*18;
    bytes[at]=uint8_t(e.section>>8); bytes[at+1]=uint8_t(e.section);
    SaveWord(bytes,at+2,uint32_t(e.key>>32)); SaveWord(bytes,at+6,uint32_t(e.key));
    SaveWord(bytes,at+10,e.offset); SaveWord(bytes,at+14,e.size);
  }
  SaveWord(bytes,24+size_t(entry_capacity)*18,7);
  SaveWord(bytes,24+size_t(entry_capacity)*18+4,UINT32_MAX-7);
  const std::array<uint8_t,4> image{0x89,0,0xF3,0xA5};
  const std::array<uint8_t,3> unknown{0xCA,0,0x7F};
  std::copy(image.begin(),image.end(),bytes.begin()+prefix+(relayout?3:0));
  std::copy(unknown.begin(),unknown.end(),bytes.begin()+prefix+(relayout?0:4));
  return bytes;
}
std::vector<uint8_t> NativeSaveTable(const std::vector<uint8_t>& bytes) {
  const auto prefix=24+size_t(XdbfTableView::U32(bytes.data()+8))*18+
                      size_t(XdbfTableView::U32(bytes.data()+16))*8;
  return {bytes.begin(),bytes.begin()+prefix};
}
}
TEST_CASE("Native SAVE accepts genuine alternate table layout only with matching actual table and exact opaque typed delta",
          "[native-save][serialization][opaque]") {
  const auto intended=NativeSaveRawGpd(false); auto staged=NativeSaveRawGpd(true);
  REQUIRE_FALSE(filesystem::ValidateProfileGpdSnapshot(intended));
  REQUIRE_FALSE(filesystem::ValidateProfileGpdSnapshot(staged));
  REQUIRE(intended!=staged); auto actual_table=NativeSaveTable(staged);
  REQUIRE_FALSE(save_detail::ValidateNativeGpdSerialization(intended,staged,actual_table));
  SECTION("a changed unknown record cannot be silently adopted") {
    staged[actual_table.size()]^=0x80;
    REQUIRE_FALSE(filesystem::ValidateProfileGpdSnapshot(staged));
    REQUIRE(save_detail::ValidateNativeGpdSerialization(intended,staged,actual_table)==std::errc::state_not_recoverable);
  }
  SECTION("a narrowed high64-bit image key is a different record") {
    SaveWord(staged,24+2*18+2,0); actual_table=NativeSaveTable(staged);
    REQUIRE_FALSE(filesystem::ValidateProfileGpdSnapshot(staged));
    REQUIRE(save_detail::ValidateNativeGpdSerialization(intended,staged,actual_table)==std::errc::state_not_recoverable);
  }
  SECTION("present empty image and deleted image remain distinct") {
    // Remove the first (empty) entry without changing other payload bytes.
    std::copy_n(staged.begin()+24+18,36,staged.begin()+24); SaveWord(staged,12,2);
    actual_table=NativeSaveTable(staged);
    REQUIRE_FALSE(filesystem::ValidateProfileGpdSnapshot(staged));
    REQUIRE(save_detail::ValidateNativeGpdSerialization(intended,staged,actual_table)==std::errc::state_not_recoverable);
  }
  SECTION("mismatched cached native offsets cannot claim the staged layout") {
    actual_table[24+2*18+13]^=1;
    REQUIRE(save_detail::ValidateNativeGpdSerialization(intended,staged,actual_table)==std::errc::bad_message);
  }
  SECTION("partial native table cannot claim serialization complete") {
    actual_table.pop_back();
    REQUIRE(save_detail::ValidateNativeGpdSerialization(intended,staged,actual_table)==std::errc::bad_message);
  }
  SECTION("a truncated staged image cannot satisfy an intact native table") {
    staged.pop_back();
    REQUIRE(save_detail::ValidateNativeGpdSerialization(intended,staged,actual_table)==std::errc::bad_message);
  }
}
TEST_CASE("Native SAVE semantic use refuses rebind across the original getter acquisition gap",
          "[native-save][lifetime]") {
  using Uses=save_detail::SemanticUses;using Target=Uses::Target;
  constexpr uint32_t profile=0x90060000;
  const Uses::Frame rebind{7,0x90010000,0x90080000,0x8190E940,0x81913D18};
  Uses uses;
  SECTION("already active rebind cannot be relearned from matching return fields") {
    uses.Enter(rebind,Target::kProfile,profile);
    const auto old=uses.BeginGetter();REQUIRE(old);CHECK_FALSE(old->unchanged());
    REQUIRE(uses.Leave(rebind));
    CHECK_FALSE(uses.BindProfile(old,profile));CHECK_FALSE(old->unchanged());
  }
  SECTION("unknown getter object is revoked even by a different eventual object") {
    const auto old=uses.BeginGetter();REQUIRE(old);REQUIRE(old->unchanged());
    uses.Enter(rebind,Target::kProfile,profile+0x2000);
    REQUIRE(uses.Leave(rebind));
    CHECK_FALSE(uses.BindProfile(old,profile));CHECK_FALSE(old->unchanged());
  }
  const auto fresh=uses.BeginGetter();REQUIRE(fresh);
  REQUIRE(uses.BindProfile(fresh,profile));CHECK(fresh->unchanged());
  uses.Retire(fresh);CHECK_FALSE(fresh->unchanged());
  CHECK_FALSE(uses.BindProfile(fresh,profile));
}
TEST_CASE("Native SAVE semantic use keeps exact profile and resource generations separate",
          "[native-save][lifetime]") {
  using Uses=save_detail::SemanticUses;using Target=Uses::Target;
  constexpr uint32_t a=0x90060000,b=0x90064000,resource=0x81B309F8;
  const Uses::Frame change{7,0x90010000,0x90080000,0x81913404,0x81928528};
  Uses uses,foreign;
  const auto left=uses.BeginGetter(),right=uses.BeginGetter();REQUIRE(left);REQUIRE(right);
  REQUIRE(uses.BindProfile(left,a));REQUIRE(uses.BindProfile(right,b));
  REQUIRE(uses.BindDatabase(left,a+0x134,resource));
  REQUIRE(uses.BindDatabase(right,b+0x134,resource+0x28));
  // A second Owner cannot bind/retire these exact original metadata spans.
  CHECK_FALSE(foreign.BindProfile(left,b));foreign.Retire(left);CHECK(left->unchanged());
  SECTION("same physical address reused later cannot rehabilitate the old use") {
    uses.Enter(change,Target::kProfile,a);REQUIRE(uses.Leave(change));
    CHECK_FALSE(left->unchanged());CHECK(right->unchanged());
    const auto replacement=uses.BeginGetter();REQUIRE(replacement);
    REQUIRE(uses.BindProfile(replacement,a));REQUIRE(replacement->unchanged());
    uses.Retire(left);CHECK(replacement->unchanged());
  }
  SECTION("embedded DB configuration revokes only its exact known profile") {
    uses.Enter(change,Target::kDatabase,a+0xD8);REQUIRE(uses.Leave(change));
    CHECK_FALSE(left->unchanged());CHECK(right->unchanged());
  }
  SECTION("fixed resource reuse is distinct from an unrelated retained resource") {
    uses.Enter(change,Target::kResource,resource);REQUIRE(uses.Leave(change));
    CHECK_FALSE(left->unchanged());CHECK(right->unchanged());
  }
  SECTION("changing a previously bound DB cannot silently select another title") {
    CHECK_FALSE(uses.BindDatabase(left,a+0xD8,resource));
    CHECK_FALSE(left->unchanged());CHECK(right->unchanged());
  }
}
TEST_CASE("Native SAVE semantic observer loss stays deny-only until actual owner rundown",
          "[native-save][lifetime]") {
  using Uses=save_detail::SemanticUses;using Target=Uses::Target;
  Uses uses;const auto old=uses.BeginGetter();REQUIRE(old);
  REQUIRE(uses.BindProfile(old,0x90060000));
  const Uses::Frame mutation{7,0x90010000,0x90080000,0x8190E940,0x81913D18};
  SECTION("actual GCRC-held observation loss immediately withdraws every old span") {
    const auto second=uses.BeginGetter();REQUIRE(second);
    REQUIRE(uses.BindProfile(second,0x90068000));
    {auto actual_guard=global_critical_region::AcquireDirect();
      REQUIRE(global_critical_region::is_held_by_current_thread());
      uses.Unavailable();CHECK_FALSE(old->unchanged());CHECK_FALSE(second->unchanged());}
    CHECK_FALSE(uses.BeginGetter());
  }
  SECTION("actual host owner destruction withdraws retained old metadata") {
    std::shared_ptr<Uses::Span> retained;
    {Uses scoped;retained=scoped.BeginGetter();REQUIRE(retained);
      REQUIRE(scoped.BindProfile(retained,0x90068000));REQUIRE(retained->unchanged());}
    CHECK_FALSE(retained->unchanged());CHECK(old->unchanged());
  }
  SECTION("unmatched return cannot discharge a different active native frame") {
    uses.Enter(mutation,Target::kProfile,0x90064000);
    auto wrong=mutation;wrong.sp-=0x90;
    CHECK_FALSE(uses.Leave(wrong));CHECK_FALSE(old->unchanged());
    REQUIRE(uses.Leave(mutation));CHECK_FALSE(uses.BeginGetter());
  }
  SECTION("missing return bounds metadata instead of evicting an active generation") {
    for(uint32_t i=0;i<129;++i) {
      auto actual=mutation;actual.sp-=i*0x90;
      uses.Enter(actual,Target::kProfile,0x90064000);
    }
    CHECK_FALSE(old->unchanged());CHECK_FALSE(uses.BeginGetter());
    // Finishing a known old frame cannot reopen a sticky lost-observation state.
    REQUIRE(uses.Leave(mutation));CHECK_FALSE(uses.BeginGetter());
  }
}

TEST_CASE("Native image checkpoint distinguishes terminal new insertion from nested replacement with the genuine caller stack",
          "[native-save][abi][lifetime]") {
  using Kind=save_detail::ImageWriteCheckpoint;
  const uint32_t caller=0x90080000,upsert_sp=caller-0xC0,insert_sp=upsert_sp-0xB0;
  const auto site=[&](uint32_t stream_sp,uint32_t lr,bool insert_live,uint32_t parent_lr,bool r23_zero=false){
    return save_detail::ClassifyImageWriteCheckpoint(0x81925AC0,caller,stream_sp,lr,
        r23_zero,insert_live,upsert_sp,parent_lr);
  };
  // Whole25248 at25344 tests real autosave; its header call25370 is terminal
  // for parent25B40. That parent's25B50 branches to25C60 without another write.
  CHECK(site(insert_sp,0x81925374,true,0x81925B44)==Kind::kHeader);
  CHECK(site(insert_sp,0x81925374,false,0x81925B44)==Kind::kNone);
  CHECK(site(insert_sp,0x81925374,true,0x81925C1C)==Kind::kNone);
  CHECK(site(insert_sp+0x10,0x81925374,true,0x81925B44)==Kind::kNone);
  CHECK(site(upsert_sp,0x81925374,true,0x81925B44)==Kind::kNone);
  // Resizing disables autosave across25C18 then restores it before the real
  // outer header25C58. Same-size payload has its own terminal25BC0 call.
  CHECK(site(upsert_sp,0x81925C5C,true,0x81925C1C)==Kind::kHeader);
  CHECK(site(upsert_sp,0x81925BC4,false,0,true)==Kind::kInPlace);
  CHECK(site(upsert_sp,0x81925BC4,false,0,false)==Kind::kNone);
  CHECK(save_detail::ClassifyImageWriteCheckpoint(0x81925110,caller,caller-0x90,
      0x81925220,false,false,0,0)==Kind::kHeader);
  CHECK(save_detail::ClassifyImageWriteCheckpoint(0x81925110,caller,insert_sp,
      0x81925220,false,true,upsert_sp,0x81925B44)==Kind::kNone);
  CHECK(save_detail::ClassifyImageWriteCheckpoint(0x81925AC0,0x80,0xFFFFFFC0,
      0x81925C5C,false,false,0,0)==Kind::kNone);
  CHECK(save_detail::ClassifyImageWriteCheckpoint(0x81925AC0,0x140,0xFFFFFFD0,
      0x81925374,false,true,0x80,0x81925B44)==Kind::kNone);
}
} // namespace xe::kernel::test
