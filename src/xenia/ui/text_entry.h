/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_TEXT_ENTRY_H_
#define XENIA_UI_TEXT_ENTRY_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace xe::ui {

enum class TextEntryResult { kAccepted, kCanceled, kAborted };

// Single-line text shared by host text fields and the controller keyboard.
// Capacity is measured in UTF-16 code units, excluding the terminator.
class TextEntry {
 public:
  explicit TextEntry(size_t max_utf16_units,
                     std::u16string_view default_text = {});

  const std::u16string& text() const { return text_; }
  std::string utf8() const;
  size_t max_utf16_units() const { return max_utf16_units_; }

  // Defaults and pasted text truncate at a Unicode scalar boundary. Invalid
  // surrogate sequences become replacement characters; embedded NUL ends text.
  void SetText(std::u16string_view text);
  // Editing an already full field must preserve its previous suffix instead
  // of silently truncating it. Invalid UTF-8 is rejected as well.
  bool SetUtf8(std::string_view text);
  bool Append(char32_t character);
  bool Backspace();
  void Clear() { text_.clear(); }

 private:
  size_t max_utf16_units_;
  std::u16string text_;
};

}  // namespace xe::ui

#endif  // XENIA_UI_TEXT_ENTRY_H_
