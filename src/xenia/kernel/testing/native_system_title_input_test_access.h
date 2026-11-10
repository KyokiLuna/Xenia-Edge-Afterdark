/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_TESTING_NATIVE_SYSTEM_TITLE_INPUT_TEST_ACCESS_H_
#define XENIA_KERNEL_TESTING_NATIVE_SYSTEM_TITLE_INPUT_TEST_ACCESS_H_

#include <memory>
#include <utility>
#include <vector>

#include "xenia/gpu/native_system_title_input.h"

namespace xe::gpu::test {
// Tests the host-owned byte container and public fetch encoding. This peer has
// no Kernel, guest object, GPU, fence or production mint authority.
class NativeSystemTitleInputTestAccess {
 public:
  static std::shared_ptr<const NativeSystemTitleInput> Create(
      NativeSystemTitleInput::Identity identity,
      NativeSystemTitleInput::Layout layout, std::vector<uint8_t> bytes) {
    return NativeSystemTitleInput::Create(identity, std::move(layout),
                                          std::move(bytes));
  }
};
}  // namespace xe::gpu::test
#endif
