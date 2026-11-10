/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_input_data.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
NativeSystemInputData Read(Memory& memory, bool bootstrap) {
  auto memory_lock = global_critical_region::AcquireDirect();
  NativeSystemInputData out;
  const auto word = [&](uint32_t at, uint32_t& value, uint32_t bit) {
    if (memory.QueryRangeAccess(at, 4) == xe::memory::PageAccess::kNoAccess) {
      return false;
    }
    value = xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at));
    out.known |= bit;
    return true;
  };
  if (!word(0x81A82FF4, out.user_table, 1) ||
      !word(0x81A83014, out.class_table, 2) ||
      !word(0x81A82FF8, out.class0_callback, 4) ||
      !word(0x81AACBF0, out.voice_gate, 8) ||
      !word(0x81A85C88, out.voice_user, 16)) {
    return out;
  }
  if (out.user_table != 0x81AA3D90) {
    out.status = NativeSystemInputData::Status::kUserTable;
    return out;
  }
  if (out.class_table != 0x81A82FF8 || out.class0_callback != 0x816F27F8) {
    out.status = NativeSystemInputData::Status::kClassTable;
    return out;
  }
  if (out.voice_gate || out.voice_user != 0xFE) {
    out.status = NativeSystemInputData::Status::kVoiceUnsupported;
    return out;
  }
  if (memory.QueryRangeAccess(out.user_table, 0x100) ==
      xe::memory::PageAccess::kNoAccess) {
    return out;
  }
  if (bootstrap) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!word(0x81A854F8 + i * 4, out.event[i], 32u << i)) {
        return out;
      }
    }
    const auto* bytes = memory.TranslateVirtual<const uint8_t*>(out.user_table);
    for (uint32_t i = 0; i < 0x100; ++i) {
      if (bytes[i]) {
        out.status = NativeSystemInputData::Status::kRecordsNotPristine;
        return out;
      }
    }
    if (out.event !=
        std::array<uint32_t, 4>{0x01000000, 0, 0x81A85500, 0x81A85500}) {
      out.status = NativeSystemInputData::Status::kEventNotPristine;
      return out;
    }
    for (uint32_t i = 0; i < out.timer.size(); ++i) {
      if (!word(0x81A854B0 + i * 4, out.timer[i], 0)) {
        return out;
      }
    }
    out.known |= 512;
    for (uint32_t i = 0; i < out.dpc.size(); ++i) {
      if (!word(0x81A854D8 + i * 4, out.dpc[i], 0)) {
        return out;
      }
    }
    out.known |= 1024;
    if (!word(0x81AAC4B0, out.failure_flag, 2048)) {
      return out;
    }
    if (out.timer != std::array<uint32_t, 10>{0x08000200, 0, 0x81A854B8,
                                              0x81A854B8, 0, 0, 0, 0, 0, 0}) {
      out.status = NativeSystemInputData::Status::kTimerNotPristine;
      return out;
    }
    if (out.dpc !=
        std::array<uint32_t, 7>{0x00130002, 0, 0, 0x816F27E0, 0, 0, 0}) {
      out.status = NativeSystemInputData::Status::kDpcNotPristine;
      return out;
    }
    if (out.failure_flag) {
      out.status = NativeSystemInputData::Status::kFailureFlag;
      return out;
    }
  }
  out.status = NativeSystemInputData::Status::kValid;
  return out;
}
}  // namespace
NativeSystemInputData ReadNativeSystemInputBootstrapData(Memory& memory) {
  return Read(memory, true);
}
NativeSystemInputData ReadNativeSystemInputBindingData(Memory& memory) {
  return Read(memory, false);
}
}  // namespace xe::kernel
