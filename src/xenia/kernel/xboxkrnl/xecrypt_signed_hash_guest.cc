/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xboxkrnl/xecrypt_signed_hash_guest.h"
#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/xboxkrnl/xecrypt_signed_hash.h"
namespace xe::kernel::xboxkrnl {
X_STATUS XeCryptRotSumShaGuest(Memory& memory, uint32_t first,
                               uint32_t first_size, uint32_t second,
                               uint32_t second_size, uint32_t output,
                               uint32_t out_size) {
  try {
    const uint32_t written = std::min(out_size, 20u);
    if ((first_size || second_size || written) &&
        global_critical_region::is_held_by_current_thread()) {
      return X_STATUS_INVALID_DEVICE_REQUEST;
    }
    if (!util::GuestIoRangeValid(memory, first, first_size, false) ||
        !util::GuestIoRangeValid(memory, second, second_size, false) ||
        !util::GuestIoRangeValid(memory, output, written, true)) {
      return X_STATUS_ACCESS_VIOLATION;
    }
    std::vector<uint8_t> a(first_size), b(second_size);
    auto status = util::ReadGuestIoBuffer(memory, first, a);
    if (XFAILED(status)) {
      return status;
    }
    status = util::ReadGuestIoBuffer(memory, second, b);
    if (XFAILED(status)) {
      return status;
    }
    std::array<uint8_t, 20> digest;
    if (!XeCryptComputeRotSumSha(a, b, digest)) {
      return X_STATUS_UNSUCCESSFUL;
    }
    return util::WriteGuestIoBuffer(
        memory, output, std::span<const uint8_t>(digest).first(written));
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  } catch (const std::length_error&) {
    return X_STATUS_NO_MEMORY;
  } catch (...) {
    return X_STATUS_UNSUCCESSFUL;
  }
}
bool XeCryptBeSigVerifyGuest(Memory& memory, uint32_t signature, uint32_t hash,
                             uint32_t salt, uint32_t key) {
  try {
    if (global_critical_region::is_held_by_current_thread() ||
        !util::GuestIoRangeValid(memory, signature, 256, false) ||
        !util::GuestIoRangeValid(memory, hash, 20, false) ||
        !util::GuestIoRangeValid(memory, salt, 10, false) ||
        !util::GuestIoRangeValid(memory, key, 16, false)) {
      return false;
    }
    std::array<uint8_t, 16> header;
    if (XFAILED(util::ReadGuestIoBuffer(memory, key, header))) {
      return false;
    }
    const uint32_t cqw = (uint32_t(header[0]) << 24) |
                         (uint32_t(header[1]) << 16) |
                         (uint32_t(header[2]) << 8) | header[3];
    if (cqw != 32 || !util::GuestIoRangeValid(memory, key, 272, false)) {
      return false;
    }
    std::array<uint8_t, 256> sig;
    std::array<uint8_t, 20> digest;
    std::array<uint8_t, 10> owned_salt;
    std::array<uint8_t, 272> owned_key;
    if (XFAILED(util::ReadGuestIoBuffer(memory, signature, sig)) ||
        XFAILED(util::ReadGuestIoBuffer(memory, hash, digest)) ||
        XFAILED(util::ReadGuestIoBuffer(memory, salt, owned_salt)) ||
        XFAILED(util::ReadGuestIoBuffer(memory, key, owned_key))) {
      return false;
    }
    return XeCryptVerifyBeSignature(sig, digest, owned_salt, owned_key);
  } catch (...) {
    return false;
  }
}
}  // namespace xe::kernel::xboxkrnl
