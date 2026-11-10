/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_notification_preference_readback.h"
#include <array>
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/util/guest_io_buffer.h"
namespace xe::kernel {
namespace {
uint32_t Sentinel(unsigned slot, unsigned attempt) {
  const uint32_t value = 0x6E6F7000u + slot;
  return attempt ? ~value : value;
}
uint32_t Guard(unsigned slot, bool after) {
  return (after ? 0xC0DEC000u : 0xB0ADB000u) + slot;
}
bool Address(uint32_t address) {
  return address && !(address & 3) && uint64_t(address) + 48 <= 0x100000000ull;
}
}  // namespace
std::array<uint32_t, 4> NativeNotificationPreferenceReadback::Arguments(
    uint32_t allocation) {
  return {allocation + 4, allocation + 16, allocation + 28, allocation + 40};
}
X_STATUS NativeNotificationPreferenceReadback::Prepare(Memory& memory,
                                                       uint32_t allocation,
                                                       unsigned attempt) {
  if (!Address(allocation) || attempt > 1) {
    return X_STATUS_INVALID_PARAMETER;
  }
  std::array<uint8_t, kBytes> bytes;
  for (unsigned i = 0; i < 4; ++i) {
    xe::store_and_swap<uint32_t>(bytes.data() + i * 12, Guard(i, false));
    xe::store_and_swap<uint32_t>(bytes.data() + i * 12 + 4,
                                 Sentinel(i, attempt));
    xe::store_and_swap<uint32_t>(bytes.data() + i * 12 + 8, Guard(i, true));
  }
  return util::WriteGuestIoBuffer(memory, allocation, bytes);
}
NativeNotificationPreferenceReadback::Result
NativeNotificationPreferenceReadback::Read(Memory& memory, uint32_t allocation,
                                           unsigned attempt) {
  if (!Address(allocation) || attempt > 1) {
    return {Kind::kInvalid, X_STATUS_INVALID_PARAMETER, {}};
  }
  std::array<uint8_t, kBytes> bytes;
  const auto status = util::ReadGuestIoBuffer(memory, allocation, bytes);
  if (XFAILED(status)) {
    return {Kind::kInvalid, status, {}};
  }
  Result result{Kind::kWritten, X_STATUS_SUCCESS, {}};
  for (unsigned i = 0; i < 4; ++i) {
    if (xe::load_and_swap<uint32_t>(bytes.data() + i * 12) != Guard(i, false) ||
        xe::load_and_swap<uint32_t>(bytes.data() + i * 12 + 8) !=
            Guard(i, true)) {
      return {Kind::kInvalid, X_STATUS_INVALID_PARAMETER, {}};
    }
    result.options[i] = xe::load_and_swap<uint32_t>(bytes.data() + i * 12 + 4);
    if (result.options[i] == Sentinel(i, attempt)) {
      result.kind = Kind::kAmbiguous;
    }
  }
  return result;
}
}  // namespace xe::kernel
