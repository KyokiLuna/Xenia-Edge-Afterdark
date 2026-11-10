/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/base/xui_resource.h"
#include <algorithm>
#include "third_party/utfcpp/source/utf8.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
namespace xe::resource {
namespace {
bool ValidUtf16(std::u16string_view text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] >= 0xD800 && text[i] <= 0xDBFF) {
      if (++i == text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF) {
        return false;
      }
    } else if (text[i] >= 0xDC00 && text[i] <= 0xDFFF) {
      return false;
    }
  }
  return true;
}
}  // namespace
static std::optional<std::u16string> ReadXuiString(
    std::span<const uint8_t> data, size_t* offset, size_t length) {
  if (*offset > data.size() || length > (data.size() - *offset) / 2) {
    return std::nullopt;
  }
  std::u16string string(length, u'\0');
  for (size_t i = 0; i < length; ++i) {
    string[i] =
        char16_t(xe::load_and_swap<uint16_t>(data.data() + *offset + i * 2));
  }
  *offset += length * 2;
  return string;
}

// A file in an XUIZ package, the archive XUI resources keep their files in.
// Version 1 names the files in UTF-16 and version 3 in UTF-8, with the same
// entries otherwise.
XuiPackageLookup LookupXuiPackageFile(std::span<const uint8_t> package,
                                      const std::string_view name) {
  constexpr uint32_t kXuiPackageMagic = 0x5855495A;  // XUIZ
  if (name.empty() || name.size() > 4096 || !::utf8::is_valid(name) ||
      name.find('\0') != std::string_view::npos || package.size() < 0x16 ||
      xe::load_and_swap<uint32_t>(package.data()) != kXuiPackageMagic) {
    return {};
  }
  const uint32_t version = xe::load_and_swap<uint32_t>(package.data() + 4);
  if (version != 1 && version != 3) {
    return {XuiPackageStatus::kUnsupported, {}};
  }
  const size_t data_start =
      0x16 + size_t(xe::load_and_swap<uint32_t>(package.data() + 0x10));
  if (data_start > package.size()) {
    return {};
  }
  const uint16_t file_count =
      xe::load_and_swap<uint16_t>(package.data() + 0x14);
  size_t offset = 0x16;
  for (uint16_t i = 0; i < file_count; ++i) {
    if (data_start > package.size() || offset > data_start ||
        data_start - offset < 9) {
      return {};
    }
    const uint32_t size = xe::load_and_swap<uint32_t>(package.data() + offset);
    const uint32_t file_offset =
        xe::load_and_swap<uint32_t>(package.data() + offset + 4);
    const size_t name_length = package[offset + 8];
    offset += 9;
    std::string file_name;
    if (version == 3) {
      if (name_length > data_start - offset) {
        return {};
      }
      file_name.assign(reinterpret_cast<const char*>(package.data() + offset),
                       name_length);
      if (!::utf8::is_valid(file_name)) {
        return {};
      }
      offset += name_length;
    } else {
      auto wide_name =
          ReadXuiString(package.first(data_start), &offset, name_length);
      if (!wide_name) {
        return {};
      }
      if (!ValidUtf16(*wide_name)) {
        return {};
      }
      file_name = xe::to_utf8(*wide_name);
    }
    if (xe::utf8::equal_case(file_name, name)) {
      if (file_offset > package.size() - data_start ||
          size > package.size() - data_start - file_offset) {
        return {};
      }
      return {XuiPackageStatus::kFound,
              package.subspan(data_start + file_offset, size)};
    }
  }
  return {XuiPackageStatus::kNotFound, {}};
}

std::optional<std::span<const uint8_t>> FindXuiPackageFile(
    std::span<const uint8_t> package, std::string_view name) {
  const auto result = LookupXuiPackageFile(package, name);
  return result.status == XuiPackageStatus::kFound ? std::optional(result.bytes)
                                                   : std::nullopt;
}

// An XUIS string table, empty strings stand for missing ones. Version 1 has
// UTF-16 strings after their length and version 2 NUL-terminated UTF-8 ones.
std::optional<std::vector<std::u16string>> ParseXuiStringTable(
    std::span<const uint8_t> data, uint8_t* out_flags) {
  constexpr uint32_t kXuiStringTableMagic = 0x58554953;  // XUIS
  if (!out_flags || data.size() < 0xC ||
      xe::load_and_swap<uint32_t>(data.data()) != kXuiStringTableMagic ||
      (data[4] != 1 && data[4] != 2)) {
    return std::nullopt;
  }
  const uint8_t version = data[4];
  const uint8_t flags = data[5];
  std::vector<std::u16string> strings(
      xe::load_and_swap<uint16_t>(data.data() + 0xA));
  size_t offset = 0xC;
  auto read_string = [&]() -> std::optional<std::u16string> {
    if (version == 2) {
      if (offset >= data.size()) {
        return std::nullopt;
      }
      const auto start = data.begin() + offset;
      const auto end = std::find(start, data.end(), uint8_t(0));
      if (end == data.end()) {
        return std::nullopt;
      }
      const size_t length = size_t(end - start);
      const std::string_view text(
          reinterpret_cast<const char*>(data.data() + offset), length);
      if (!::utf8::is_valid(text)) {
        return std::nullopt;
      }
      offset += length + 1;
      return xe::to_utf16(text);
    }
    if (offset + 2 > data.size()) {
      return std::nullopt;
    }
    const uint16_t length = xe::load_and_swap<uint16_t>(data.data() + offset);
    offset += 2;
    auto value = ReadXuiString(data, &offset, length);
    return value && ValidUtf16(*value) ? value : std::nullopt;
  };
  for (auto& string : strings) {
    auto value = read_string();
    if (!value) {
      return std::nullopt;
    }
    string = std::move(*value);
    // Indexed tables have nothing after the string, the others an id or a key.
    if (flags & 2) {
      continue;
    }
    if (flags & 1) {
      offset += 4;
    } else if (!read_string()) {
      return std::nullopt;
    }
  }
  if (offset > data.size()) {
    return std::nullopt;
  }
  *out_flags = flags;
  return strings;
}

}  // namespace xe::resource
