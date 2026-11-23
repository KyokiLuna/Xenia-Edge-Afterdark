/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
namespace xe::kernel {
// Entry/own-return hooks only. The separate hash cohort also contains helper
// bodies needed to authenticate these paths; those are not extra callbacks.
// Original image functions use the same entry/own-return ABI, but the Owner
// routes them to observer-only metadata. They never return a native denial.
constexpr bool ObservesNativeIdentityImage(uint32_t at) noexcept {
 switch(at){
  case 0x817B4A80:
  case 0x817B4A50:
  case 0x817ADD70:
  case 0x817ACA98:
  case 0x817CDF08:
  case 0x817CC858:
  case 0x817C7B38:
  case 0x8177EA08:
  case 0x8177D288:
  case 0x8177D1D0:
  case 0x8177EB58:
  case 0x817836D8:
  case 0x8177E778:
  case 0x8177E728:
  case 0x817851B8:
  case 0x81784D80:
  case 0x8178E138:
  case 0x816B0B50:case 0x8172D590:
  case 0x81785308:
  case 0x81780C50:case 0x81780CE0:
  case 0x81783688:
  case 0x81780DD0:
  case 0x8177BD80:
  case 0x8177C870:
  case 0x817D2BB8:
  case 0x81780720:
  case 0x8177D000:
  case 0x81798270:
  case 0x81798740:
  case 0x81798788:
  case 0x81839590:
  case 0x81839C18:
  case 0x81839780:case 0x81839C60:
  case 0x8183A180:
  case 0x816C8D68:
  case 0x816C8E10:
  case 0x817DD2E0:case 0x817DB560:case 0x817DAF80:case 0x816C82C8:
   return true;
  default:return false;
 }
}
constexpr bool HooksNativeProfileIdentity(uint32_t at) noexcept {
 if(ObservesNativeIdentityImage(at))return true;
 switch(at){
  case 0x8190A358:case 0x819068D0:case 0x816B8B08:case 0x816B7DE8:
  case 0x816B9F08:case 0x816BA058:return true;
  case 0x8199C630:case 0x8199E3E0:case 0x8199C838:case 0x8199E148:
  case 0x816DC628:case 0x816BD420:
  case 0x816A2588:
  case 0x816A2CF0:case 0x816A2A48:case 0x816B9D68:
  case 0x816BA440:case 0x816B9A18:case 0x816B9BB8:
  case 0x816BA668:case 0x816B9AA8:case 0x816B7490:
  case 0x816B8098:case 0x816B7F30:
  case 0x816B7D98:case 0x816B7500:case 0x816E6AB0:
  case 0x816DA428:case 0x816F44D0:case 0x816F3A40:
  case 0x816F25E0:case 0x816B7B68:case 0x816B7600:
  case 0x816F2618:case 0x816E3A98:
  case 0x816F4298:case 0x816F4410:case 0x816F4088:
  case 0x816A8B20:case 0x816A8498:case 0x816A88E8:
  case 0x816A93C8:case 0x816A8C00:case 0x816A8328:case 0x816A8648:
  case 0x816921C8:case 0x81670B68:
  case 0x81694B48:case 0x816934F8:case 0x81693CD8:case 0x81693568:return true;
  default:return false;
 }
}
} // namespace xe::kernel
