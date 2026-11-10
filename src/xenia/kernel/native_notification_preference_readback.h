/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_NOTIFICATION_PREFERENCE_READBACK_H_
#define XENIA_KERNEL_NATIVE_NOTIFICATION_PREFERENCE_READBACK_H_
#include <array>
#include <cstdint>
#include "xenia/kernel/native_notification_preference_coordinator.h"
#include "xenia/xbox.h"
namespace xe {
class Memory;
}
namespace xe::kernel {
// The caller owns and retains this whole guest allocation through the genuine
// synchronous getter, including an uncertain return. This helper executes no
// PPC.
struct NativeNotificationPreferenceReadback {
  static constexpr uint32_t kBytes = 48;
  enum class Kind { kWritten, kAmbiguous, kInvalid };
  struct Result {
    Kind kind = Kind::kInvalid;
    X_STATUS status = X_STATUS_UNSUCCESSFUL;
    NativeNotificationPreferenceCoordinator::Options options{};
  };
  static std::array<uint32_t, 4> Arguments(uint32_t allocation);
  // Attempt0 and1 use distinct complementary sentinels. Arbitrary native cache
  // DWORDs are valid; equality remains conservatively ambiguous, never false
  // proof that the getter skipped a store. No canonical0/1 assumption.
  static X_STATUS Prepare(Memory&, uint32_t allocation, unsigned attempt);
  static Result Read(Memory&, uint32_t allocation, unsigned attempt);
};
}  // namespace xe::kernel
#endif
