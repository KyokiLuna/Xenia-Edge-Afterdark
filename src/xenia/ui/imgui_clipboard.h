/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_CLIPBOARD_H_
#define XENIA_UI_IMGUI_CLIPBOARD_H_

namespace xe::ui {

// Configure the current ImGui context on the wx main UI thread. Without a wx
// app or current context, leave ImGui's default clipboard handlers untouched.
// Clipboard contents are read only when ImGui invokes its paste callback.
void ConfigureImGuiClipboardForWx();

}  // namespace xe::ui

#endif  // XENIA_UI_IMGUI_CLIPBOARD_H_
