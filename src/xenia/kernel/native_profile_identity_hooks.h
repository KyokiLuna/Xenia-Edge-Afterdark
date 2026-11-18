/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
namespace xe::kernel {
// Entry/own-return hooks only. The separate hash cohort also contains helper
// bodies needed to authenticate these paths; those are not extra callbacks.
constexpr bool HooksNativeProfileIdentity(uint32_t at) noexcept {
 switch(at){
  case 0x8190A358:case 0x819068D0:case 0x816B8B08:
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
