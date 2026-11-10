/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_KEYBOARD_H_
#define XENIA_KERNEL_XAM_XAM_KEYBOARD_H_

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include "xenia/base/native_ui_backend_route.h"

#include "xenia/base/byte_order.h"
#include "xenia/ui/text_entry.h"
#include "xenia/xbox.h"

namespace xe::kernel::xam {

// Writes only the accepted text and its terminator. Capacity includes NUL;
// failure leaves the caller's entire buffer intact.
bool WriteKeyboardText(std::u16string_view text,
                       std::span<xe::be<uint16_t>> buffer);

struct KeyboardUiCompletion {
  X_RESULT result;
  uint32_t extended_error;
  uint32_t length;
};

KeyboardUiCompletion CompleteKeyboardText(xe::ui::TextEntryResult result,
                                          std::u16string_view text,
                                          std::span<xe::be<uint16_t>> buffer);

X_RESULT ShowKeyboardUI(
    uint32_t user_index, uint32_t flags, std::u16string default_text,
    std::u16string title, std::u16string description, uint32_t buffer,
    uint32_t buffer_length, uint32_t overlapped,
    std::shared_ptr<NativeUiBackendRoute::HostLease> host_route = {});

}  // namespace xe::kernel::xam

#endif  // XENIA_KERNEL_XAM_XAM_KEYBOARD_H_
