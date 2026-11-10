/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_INPUT_DATA_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_INPUT_DATA_H_
#include <array>
#include <cstdint>
namespace xe {
class Memory;
namespace kernel {
// Read-only selected17559 class0 prerequisites. These values grant no execution
// authority; the catalog separately proves the actual
// module/domain/code/imports.
struct NativeSystemInputData {
  enum class Status {
    kValid,
    kUnreadable,
    kUserTable,
    kClassTable,
    kVoiceUnsupported,
    kRecordsNotPristine,
    kEventNotPristine,
    kTimerNotPristine,
    kDpcNotPristine,
    kFailureFlag
  };
  Status status = Status::kUnreadable;
  uint32_t known = 0;
  uint32_t user_table = 0, class_table = 0, class0_callback = 0;
  uint32_t voice_gate = 0, voice_user = 0;
  std::array<uint32_t, 4> event{};
  std::array<uint32_t, 10> timer{};
  std::array<uint32_t, 7> dpc{};
  uint32_t failure_flag = 0;
};
// One-time BEFORE any registration/enqueue: require actual pristine records and
// real initialized event. Never call this again after native binding mutates
// them.
NativeSystemInputData ReadNativeSystemInputBootstrapData(Memory&);
// Future physical changes: table pointers/no-voice profile stay proved, while
// native records and the lazily materialized event are allowed to change.
NativeSystemInputData ReadNativeSystemInputBindingData(Memory&);
}  // namespace kernel
}  // namespace xe
#endif
