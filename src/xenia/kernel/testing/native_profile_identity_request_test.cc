/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <array>
#include <system_error>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/native_profile_identity_request.h"

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
 CHECK((add->desired()==std::array<uint64_t,4>{kA,kC,0,0}));
 CHECK(std::equal(add->original().begin(),add->original().end(),original.begin(),original.end()));
 const auto replacement=Access::Capture(0x20B,original,0,{kA,kB,0,0},error);
 REQUIRE(replacement);CHECK_FALSE(error);
 CHECK((replacement->desired()==std::array<uint64_t,4>{0,kC,0,0}));
 CHECK(std::equal(replacement->original().begin(),replacement->original().end(),original.begin(),original.end()));
}
TEST_CASE("Native logon gives Add precedence and validates removal in the original slot",
          "[native-identity][wire]"){
 std::error_code error;const auto one=FourXuids({kC,0,0,0});
 const auto both=Access::Capture(0x20B,one,0xC,{kA,kB,0,0},error);
 REQUIRE(both);CHECK_FALSE(error);CHECK(both->flags()==0xC);
 CHECK((both->desired()==std::array<uint64_t,4>{kC,kB,0,0}));
 const auto remove=Access::Capture(0x20B,FourXuids({0,kB,0,0}),8,{kA,kB,0,0},error);
 REQUIRE(remove);CHECK_FALSE(error);CHECK((remove->desired()==std::array<uint64_t,4>{kA,0,0,0}));
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
 struct Scenario {
  uint32_t flags;
  std::array<uint64_t,4> input, desired;
  int selected;
 };
 const std::array<uint64_t,4> before{kA,kB,0,0};
 const Scenario scenarios[]{
  {0x20,{0,0,0,kC},{0,0,0,kC},3},       // Replacement, including zero removals.
  {0x24,{0,0,kC,0},{kA,kB,kC,0},2},     // Actual creator Add4|UI-suppression20.
  {0x25,{0,0,kC,0},{kA,kB,kC,0},2},     // Existing raw bit1 is still preserved.
  {0x28,{0,kB,0,0},{kA,0,0,0},-1},      // Removal still validates its own slot.
  {0x2C,{kC,0,0,0},{kC,kB,0,0},0},      // Add wins over Remove unchanged.
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
} // namespace xe::kernel::test
