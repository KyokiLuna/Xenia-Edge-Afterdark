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
 CHECK_FALSE(Access::Capture(0x20B,bytes,0x200000,{},error));CHECK(error==std::errc::operation_not_supported);
 CHECK_FALSE(Access::Capture(0x20B,std::span(bytes).first(31),0,{},error));CHECK(error==std::errc::invalid_argument);
 CHECK_FALSE(Access::Capture(0x224,bytes,0,{},error));CHECK(error==std::errc::invalid_argument);
 CHECK_FALSE(Access::Capture(0x999,bytes,0,{},error));CHECK(error==std::errc::invalid_argument);
}
} // namespace xe::kernel::test
