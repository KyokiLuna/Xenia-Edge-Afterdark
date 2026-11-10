/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/text_entry.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "third_party/catch/include/catch.hpp"

namespace xe::ui::test {
namespace {

TEST_CASE("Text entry limits UTF-16 units rather than UTF-8 bytes",
          "[ui][text_entry]") {
  struct Example {
    size_t capacity;
    std::u16string_view utf16;
    std::string_view utf8;
  };
  constexpr std::array<Example, 3> examples = {{
      {2, u"\u3042\u3044", "\xE3\x81\x82\xE3\x81\x84"},
      {2, u"\U0001F642", "\xF0\x9F\x99\x82"},
      {4, u"A\u3042\U0001F642", "A\xE3\x81\x82\xF0\x9F\x99\x82"},
  }};
  for (const auto& example : examples) {
    CAPTURE(example.capacity);
    TextEntry entry(example.capacity);
    CHECK(entry.max_utf16_units() == example.capacity);
    REQUIRE(entry.SetUtf8(example.utf8));
    CHECK(entry.text() == example.utf16);
    CHECK(entry.text().size() == example.capacity);
    CHECK(entry.utf8() == example.utf8);

    const std::string too_long = std::string(example.utf8) + "!";
    CHECK_FALSE(entry.SetUtf8(too_long));
    CHECK(entry.text() == example.utf16);
    CHECK(entry.utf8() == example.utf8);
  }
}

TEST_CASE("Default text truncation preserves whole Unicode scalars",
          "[ui][text_entry]") {
  const TextEntry before_pair(2, u"A\U0001F642Z");
  CHECK(before_pair.text() == u"A");
  const TextEntry after_pair(3, u"A\U0001F642Z");
  CHECK(after_pair.text() == u"A\U0001F642");
  const TextEntry pair_does_not_fit(1, u"\U0001F642Z");
  CHECK(pair_does_not_fit.text().empty());
  const TextEntry bmp_boundary(2, u"\u3042\u3044\u3046");
  CHECK(bmp_boundary.text() == u"\u3042\u3044");

  TextEntry replacement(3, u"old");
  replacement.SetText(u"\U0001F642\u3042Z");
  CHECK(replacement.text() == u"\U0001F642\u3042");
}

TEST_CASE("Text entry sanitizes malformed UTF-16 and stops at NUL",
          "[ui][text_entry]") {
  constexpr std::array<char16_t, 6> malformed = {
      char16_t(0xD800), u'x',
      char16_t(0xDC00), char16_t(0xD83D),
      char16_t(0xDE42), char16_t(0xD800)};
  const std::u16string_view malformed_text(malformed.data(), malformed.size());
  TextEntry entry(16, malformed_text);
  CHECK(entry.text() == u"\uFFFDx\uFFFD\U0001F642\uFFFD");
  CHECK(entry.utf8() ==
        "\xEF\xBF\xBDx\xEF\xBF\xBD\xF0\x9F\x99\x82\xEF\xBF\xBD");

  constexpr std::array<char16_t, 3> with_nul = {u'A', u'\0', u'B'};
  entry.SetText(std::u16string_view(with_nul.data(), with_nul.size()));
  CHECK(entry.text() == u"A");
  CHECK(entry.utf8() == "A");

  constexpr std::array<char16_t, 3> high_before_nul = {char16_t(0xD800), u'\0',
                                                       char16_t(0xDC00)};
  entry.SetText(
      std::u16string_view(high_before_nul.data(), high_before_nul.size()));
  CHECK(entry.text() == u"\uFFFD");

  constexpr std::array<char16_t, 8> with_controls = {
      u'A',  char16_t(1),    u'\t',          u'B',
      u'\n', char16_t(0x7F), char16_t(0x85), u'C'};
  TextEntry single_line(
      3, std::u16string_view(with_controls.data(), with_controls.size()));
  CHECK(single_line.text() == u"ABC");
  single_line.SetText(single_line.text());
  CHECK(single_line.text() == u"ABC");
}

TEST_CASE("Zero and one-unit capacities remain usable and scalar safe",
          "[ui][text_entry]") {
  TextEntry zero(0, u"default");
  CHECK(zero.text().empty());
  CHECK(zero.utf8().empty());
  CHECK_FALSE(zero.Append(U'A'));
  CHECK_FALSE(zero.Append(U'\U0001F642'));
  CHECK_FALSE(zero.Backspace());
  CHECK(zero.SetUtf8(""));
  CHECK_FALSE(zero.SetUtf8("A"));
  zero.SetText(u"A");
  CHECK(zero.text().empty());

  TextEntry one(1, u"\u3042B");
  CHECK(one.text() == u"\u3042");
  CHECK_FALSE(one.Append(U'B'));
  CHECK(one.Backspace());
  CHECK_FALSE(one.Append(U'\U0001F642'));
  CHECK(one.text().empty());
  CHECK(one.Append(U'A'));
  CHECK(one.text() == u"A");
}

TEST_CASE("Backspace removes one scalar including an entire surrogate pair",
          "[ui][text_entry]") {
  TextEntry entry(4, u"A\u3042\U0001F642");
  REQUIRE(entry.Backspace());
  CHECK(entry.text() == u"A\u3042");
  CHECK(entry.utf8() == "A\xE3\x81\x82");
  REQUIRE(entry.Backspace());
  CHECK(entry.text() == u"A");
  REQUIRE(entry.Backspace());
  CHECK(entry.text().empty());
  CHECK_FALSE(entry.Backspace());
  REQUIRE(entry.Append(U'\U0001F642'));
  CHECK(entry.text() == u"\U0001F642");
  entry.Clear();
  CHECK(entry.text().empty());
  CHECK(entry.max_utf16_units() == 4);
  REQUIRE(entry.Append(U'\u3042'));
  CHECK(entry.utf8() == "\xE3\x81\x82");
}

TEST_CASE("Append rejects control characters and invalid scalars unchanged",
          "[ui][text_entry]") {
  TextEntry entry(16, u"kept");
  constexpr std::array<char32_t, 16> rejected = {U'\0',
                                                 U'\t',
                                                 U'\n',
                                                 U'\r',
                                                 char32_t(0x1F),
                                                 char32_t(0x7F),
                                                 char32_t(0x80),
                                                 char32_t(0x85),
                                                 char32_t(0x9F),
                                                 char32_t(0xD800),
                                                 char32_t(0xDBFF),
                                                 char32_t(0xDC00),
                                                 char32_t(0xDFFF),
                                                 char32_t(0x110000),
                                                 char32_t(0x7FFFFFFF),
                                                 char32_t(0xFFFFFFFF)};
  for (char32_t scalar : rejected) {
    CAPTURE(static_cast<uint32_t>(scalar));
    CHECK_FALSE(entry.Append(scalar));
    CHECK(entry.text() == u"kept");
    CHECK(entry.utf8() == "kept");
  }
  REQUIRE(entry.Append(U' '));
  REQUIRE(entry.Append(U'\u00A0'));
  REQUIRE(entry.Append(U'\U0001F642'));
  CHECK(entry.text() == u"kept \u00A0\U0001F642");
}

TEST_CASE("Malformed or oversized UTF-8 edits retain the previous text",
          "[ui][text_entry]") {
  TextEntry entry(5, u"kept");
  constexpr std::array<std::string_view, 5> malformed = {
      "\xFF", "\xC0\xAF", "\xE3\x81", "\xED\xA0\x80", "\xF4\x90\x80\x80"};
  for (std::string_view text : malformed) {
    CHECK_FALSE(entry.SetUtf8(text));
    CHECK(entry.text() == u"kept");
  }
  CHECK_FALSE(entry.SetUtf8("longer"));
  CHECK(entry.text() == u"kept");
  CHECK(entry.SetUtf8(""));
  CHECK(entry.text().empty());
}

TEST_CASE("UTF-8 edits cannot inject NUL or single-line control characters",
          "[ui][text_entry]") {
  TextEntry entry(5, u"kept");
  constexpr std::array<std::string_view, 6> rejected = {
      "A\nB",
      "A\rB",
      "A\tB",
      "A\x7F"
      "B",
      "\xC2\x85",
      std::string_view("A\0B", 3)};
  for (std::string_view text : rejected) {
    CHECK_FALSE(entry.SetUtf8(text));
    CHECK(entry.text() == u"kept");
  }
}

}  // namespace
}  // namespace xe::ui::test
