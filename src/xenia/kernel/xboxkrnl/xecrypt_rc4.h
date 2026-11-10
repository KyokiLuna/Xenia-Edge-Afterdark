/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XBOXKRNL_XECRYPT_RC4_H_
#define XENIA_KERNEL_XBOXKRNL_XECRYPT_RC4_H_
#include <cstdint>
#include <span>
namespace xe::kernel::xboxkrnl {
// Shared exact legacy export state/layout and RC4 core. No guest pointers.
struct XECRYPT_RC4_STATE {
  uint8_t S[256];
  uint8_t i;
  uint8_t j;
};
static_assert(sizeof(XECRYPT_RC4_STATE) == 0x102);
inline bool XeCryptRc4Key(XECRYPT_RC4_STATE& state,
                          std::span<const uint8_t> key) {
  // Match existing export: zero-length key resets i/j/S then does no schedule.
  state.i = state.j = 0;
  for (uint32_t x = 0; x < 256; ++x) {
    state.S[x] = uint8_t(x);
  }
  if (key.empty()) {
    return false;
  }
  uint32_t index = 0;
  for (uint32_t x = 0; x < 256; ++x) {
    index = (index + state.S[x] + key[x % key.size()]) % 256;
    const auto temp = state.S[index];
    state.S[index] = state.S[x];
    state.S[x] = temp;
  }
  return true;
}
inline void XeCryptRc4Ecb(XECRYPT_RC4_STATE& state, std::span<uint8_t> data) {
  for (auto& value : data) {
    state.i = uint8_t(state.i + 1);
    state.j = uint8_t(state.j + state.S[state.i]);
    const auto temp = state.S[state.i];
    state.S[state.i] = state.S[state.j];
    state.S[state.j] = temp;
    value ^= state.S[uint8_t(state.S[state.i] + state.S[state.j])];
  }
}
}  // namespace xe::kernel::xboxkrnl
#endif
