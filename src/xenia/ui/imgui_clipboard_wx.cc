/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_clipboard.h"

#include <cstring>
#include <limits>

#include "third_party/imgui/imgui_internal.h"
#include "wx/app.h"
#include "wx/clipbrd.h"
#include "wx/dataobj.h"
#include "wx/thread.h"

namespace xe::ui {
namespace {

wxClipboard* AvailableClipboard() {
  if (!wxTheApp || !wxIsMainThread()) {
    return nullptr;
  }
  auto* clipboard = wxTheClipboard;
  // The locker always closes its clipboard. Do not borrow an open clipboard
  // from another wx operation or a reentrant clipboard request.
  return clipboard && !clipboard->IsOpened() ? clipboard : nullptr;
}

class ClipboardSelectionScope {
 public:
  explicit ClipboardSelectionScope(wxClipboard* clipboard)
      : clipboard_(clipboard), primary_(clipboard->IsUsingPrimarySelection()) {
    clipboard_->UsePrimarySelection(false);
  }
  ~ClipboardSelectionScope() { clipboard_->UsePrimarySelection(primary_); }

 private:
  wxClipboard* clipboard_;
  bool primary_;
};

const char* GetClipboardText(ImGuiContext* context) {
  auto* clipboard = context ? AvailableClipboard() : nullptr;
  if (!clipboard) {
    return "";
  }
  ClipboardSelectionScope selection(clipboard);
  wxClipboardLocker lock(clipboard);
  if (!lock) {
    return "";
  }
  wxTextDataObject text;
  if (!clipboard->GetData(text)) {
    return "";
  }
  const wxString clipboard_text = text.GetText();
  const auto utf8 = clipboard_text.utf8_str();
  const size_t length = utf8.length();
  if (!utf8.data() || length >= size_t(std::numeric_limits<int>::max())) {
    return "";
  }
  // ImGui owns this per-context buffer and releases it on context shutdown.
  // The returned UTF-8 remains valid after wx closes the clipboard.
  auto& buffer = context->ClipboardHandlerData;
  buffer.resize(int(length + 1));
  std::memcpy(buffer.Data, utf8.data(), length);
  buffer[int(length)] = '\0';
  return buffer.Data;
}

void SetClipboardText(ImGuiContext* context, const char* text) {
  auto* clipboard = context ? AvailableClipboard() : nullptr;
  if (!clipboard) {
    return;
  }
  ClipboardSelectionScope selection(clipboard);
  wxClipboardLocker lock(clipboard);
  if (!lock) {
    return;
  }
  // wx takes ownership after SetData, including keeping text alive for an
  // asynchronous system clipboard request. Never retain an ImGui text pointer.
  clipboard->SetData(
      new wxTextDataObject(wxString::FromUTF8(text ? text : "")));
}

}  // namespace

void ConfigureImGuiClipboardForWx() {
  auto* context = ImGui::GetCurrentContext();
  if (!context || !wxTheApp || !wxIsMainThread()) {
    return;
  }
  context->PlatformIO.Platform_GetClipboardTextFn = GetClipboardText;
  context->PlatformIO.Platform_SetClipboardTextFn = SetClipboardText;
}

}  // namespace xe::ui
