/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/text_entry.h"

#include "third_party/utfcpp/source/utf8.h"
#include "xenia/base/string.h"

namespace xe::ui {
namespace {
bool IsHighSurrogate(char16_t value) {
  return value >= 0xD800 && value <= 0xDBFF;
}
bool IsLowSurrogate(char16_t value) {
  return value >= 0xDC00 && value <= 0xDFFF;
}
bool IsPrintable(char32_t value) {
  return value >= 0x20 && !(value >= 0x7F && value <= 0x9F) &&
         !(value >= 0xD800 && value <= 0xDFFF) && value <= 0x10FFFF;
}
}  // namespace

TextEntry::TextEntry(size_t max_utf16_units, std::u16string_view default_text)
    : max_utf16_units_(max_utf16_units) {
  SetText(default_text);
}

std::string TextEntry::utf8() const { return xe::to_utf8(text_); }

void TextEntry::SetText(std::u16string_view text) {
  // Allow callers to normalize the current text in place too.
  const std::u16string source(text);
  text = source;
  text_.clear();
  for (size_t i = 0; i < text.size() && text[i]; ++i) {
    char32_t character = text[i];
    if (IsHighSurrogate(text[i])) {
      if (i + 1 < text.size() && IsLowSurrogate(text[i + 1])) {
        character =
            0x10000 + ((text[i] - 0xD800) << 10) + (text[i + 1] - 0xDC00);
        ++i;
      } else {
        character = 0xFFFD;
      }
    } else if (IsLowSurrogate(text[i])) {
      character = 0xFFFD;
    }
    if (!IsPrintable(character)) {
      continue;
    }
    if (!Append(character)) {
      break;
    }
  }
}

bool TextEntry::SetUtf8(std::string_view text) {
  if (!::utf8::is_valid(text.begin(), text.end())) {
    return false;
  }
  auto converted = xe::to_utf16(text);
  if (converted.size() > max_utf16_units_) {
    return false;
  }
  // The same single-line policy applies to pasted and physical keyboard text.
  for (auto it = text.begin(); it != text.end();) {
    if (!IsPrintable(::utf8::next(it, text.end()))) {
      return false;
    }
  }
  text_ = std::move(converted);
  return true;
}

bool TextEntry::Append(char32_t character) {
  if (!IsPrintable(character)) {
    return false;
  }
  const size_t units = character > 0xFFFF ? 2 : 1;
  if (units > max_utf16_units_ - text_.size()) {
    return false;
  }
  if (units == 2) {
    character -= 0x10000;
    text_.push_back(char16_t(0xD800 + (character >> 10)));
    text_.push_back(char16_t(0xDC00 + (character & 0x3FF)));
  } else {
    text_.push_back(char16_t(character));
  }
  return true;
}

bool TextEntry::Backspace() {
  if (text_.empty()) {
    return false;
  }
  if (IsLowSurrogate(text_.back()) && text_.size() >= 2 &&
      IsHighSurrogate(text_[text_.size() - 2])) {
    text_.pop_back();
  }
  text_.pop_back();
  return true;
}

}  // namespace xe::ui
