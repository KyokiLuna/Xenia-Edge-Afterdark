/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XBOXKRNL_XECRYPT_SIGNED_HASH_GUEST_H_
#define XENIA_KERNEL_XBOXKRNL_XECRYPT_SIGNED_HASH_GUEST_H_
#include <cstdint>
#include "xenia/xbox.h"
namespace xe {
class Memory;
}
namespace xe::kernel::xboxkrnl {
// Internal status is not a guest result: the public RotSumSha ABI is void and
// fail-stops on a meaningful operation failure. Inputs are snapshotted before
// atomic output publication; aliases are allowed, no translated borrow survives
// a provider wait. Only min(out_size,20) output bytes are written.
X_STATUS XeCryptRotSumShaGuest(Memory&, uint32_t input1, uint32_t size1,
                               uint32_t input2, uint32_t size2, uint32_t output,
                               uint32_t out_size);
// Exact256/20/10/272B caller-owned inputs, unchanged on all outcomes.
bool XeCryptBeSigVerifyGuest(Memory&, uint32_t signature, uint32_t hash,
                             uint32_t salt, uint32_t key);
}  // namespace xe::kernel::xboxkrnl
#endif
