/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_TEXT_INPUT_DATA_H_
#define XENIA_KERNEL_NATIVE_TEXT_INPUT_DATA_H_

#include <cstdint>

namespace xe {
class Memory;
namespace kernel {

// Structural metadata only. Compatible bytes do not establish a live scene,
// native Task, caller, input audience, device assignment or input permission.
// Production must hold the actual serialized UI Task/domain/code lifetime
// before borrowing the graph. No pointer from this snapshot is retained.
struct NativeUiTextFocusData {
  enum class Status : uint8_t {
    kUnreadable,
    kInactive,
    kTransition,
    kContext,
    kScene,
    kHandle,
    kClassChain,
    kParent,
    kUser,
    kCompatible,
  };
  Status status = Status::kUnreadable;
  uint32_t context = 0;
  uint32_t scene = 0;
  uint32_t parent_handle = 0;
  uint32_t child_handle = 0;
  uint32_t user = UINT32_MAX;
  bool compatible() const { return status == Status::kCompatible; }
};

NativeUiTextFocusData ReadNativeUiTextFocusData(Memory&);

}  // namespace kernel
}  // namespace xe
#endif  // XENIA_KERNEL_NATIVE_TEXT_INPUT_DATA_H_
