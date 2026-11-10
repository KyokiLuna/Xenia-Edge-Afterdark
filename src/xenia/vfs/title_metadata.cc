/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/vfs/title_metadata.h"

#include <algorithm>
#include "third_party/utfcpp/source/utf8.h"

#include "xenia/base/xdbf_view.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"

namespace xe::vfs {
toml::table TitleResourceMetadata::ToToml() const {
  toml::table result;
  result.insert("title_id", title_id);
  result.insert("raw_title_type", raw_title_type);
  result.insert("flags", flags);
  result.insert("default_language", default_language);
  result.insert("icon_bytes", int64_t(icon_png.size()));
  toml::table localized;
  for (const auto& [language, name] : names) {
    localized.insert(std::to_string(language), name);
  }
  result.insert("names", std::move(localized));
  return result;
}

TitlePresentationDescriptor DescribeTitlePresentation(
    uint32_t physical_content_type, const TitleResourceMetadata* resources,
    uint32_t effective_license_mask) {
  TitlePresentationDescriptor result;
  result.effective_license_mask = effective_license_mask;
  if (resources) {
    if (resources->raw_title_type == 0) {
      result.kind = TitleSemanticKind::kSystemApplication;
      return result;
    }
    if (resources->raw_title_type == 5) {
      result.kind = TitleSemanticKind::kApplication;
      return result;
    }
  }
  const auto type = static_cast<XContentType>(physical_content_type);
  switch (type) {
    case XContentType::kInstalledGame:
    case XContentType::kXboxTitle:
    case XContentType::kXbox360Title:
    case XContentType::kArcadeTitle:
    case XContentType::kGameDemo:
    case XContentType::kXNACommunity:
    case XContentType::kXNA:
    case XContentType::kCommunityGame:
      result.kind = TitleSemanticKind::kGame;
      break;
    default:
      if (resources &&
          (resources->raw_title_type == 1 || resources->raw_title_type == 2)) {
        result.kind = TitleSemanticKind::kGame;
      }
      break;
  }
  if (result.kind != TitleSemanticKind::kGame) {
    return result;
  }
  if (type == XContentType::kArcadeTitle) {
    result.entitlement = effective_license_mask & 1 ? TitleEntitlement::kFull
                                                    : TitleEntitlement::kTrial;
  } else if (type == XContentType::kGameDemo ||
             (resources && resources->raw_title_type == 2)) {
    result.entitlement = TitleEntitlement::kTrial;
  } else if (type == XContentType::kXboxTitle ||
             type == XContentType::kInstalledGame ||
             (resources && resources->raw_title_type == 1)) {
    result.entitlement = TitleEntitlement::kFull;
  }
  return result;
}

std::string TitleResourceMetadata::Name(uint32_t language) const {
  auto found = names.find(language);
  if (found == names.end()) {
    found = names.find(default_language);
  }
  return found == names.end() ? std::string() : found->second;
}

std::optional<TitleResourceMetadata> ExtractTitleResourceMetadata(
    std::span<const uint8_t> xdbf) {
  using namespace kernel::xam;
  const auto table = XdbfTableView::Parse(xdbf);
  if (!table) {
    return std::nullopt;
  }
  auto u32 = XdbfTableView::U32;
  const auto title = table->Find(uint16_t(SpaSection::kMetadata), kXdbfIdXthd);
  if (!title ||
      title->size() < sizeof(XdbfSectionHeader) + sizeof(TitleHeaderData) ||
      u32(title->data()) != kXdbfSignatureXthd || u32(title->data() + 4) != 1) {
    return std::nullopt;
  }
  TitleResourceMetadata result;
  const auto* info = title->data() + sizeof(XdbfSectionHeader);
  result.title_id = u32(info);
  result.raw_title_type = u32(info + 4);
  result.flags = u32(info + 16);
  if (const auto language =
          table->Find(uint16_t(SpaSection::kMetadata), kXdbfIdXstc)) {
    if (language->size() < sizeof(XdbfXstc) ||
        u32(language->data()) != kXdbfSignatureXstc) {
      return std::nullopt;
    }
    result.default_language = u32(language->data() + 12);
  }
  // Language ids on disk are the XLanguage values, not host locale indices.
  for (uint32_t language = 1; language < uint32_t(XLanguage::kMaxLanguages);
       ++language) {
    auto strings = table->Find(uint16_t(SpaSection::kStringTable), language);
    if (!strings) {
      continue;
    }
    if (strings->size() < sizeof(XdbfSectionHeaderEx) ||
        u32(strings->data()) != kXdbfSignatureXstr ||
        u32(strings->data() + 4) != 1) {
      return std::nullopt;
    }
    const uint32_t count = uint32_t((*strings)[12]) << 8 | (*strings)[13];
    size_t offset = sizeof(XdbfSectionHeaderEx);
    for (uint32_t i = 0; i < count; ++i) {
      if (offset > strings->size() || strings->size() - offset < 4) {
        return std::nullopt;
      }
      const auto* entry = strings->data() + offset;
      const uint16_t id = uint16_t(entry[0]) << 8 | entry[1];
      const uint16_t length = uint16_t(entry[2]) << 8 | entry[3];
      offset += 4;
      if (length > strings->size() - offset) {
        return std::nullopt;
      }
      if (id == kXdbfIdTitle) {
        std::string name(
            reinterpret_cast<const char*>(strings->data() + offset), length);
        while (!name.empty() && name.back() == '\0') {
          name.pop_back();
        }
        if (::utf8::is_valid(name) && name.find('\0') == std::string::npos) {
          result.names[language] = std::move(name);
        }
      }
      offset += length;
    }
  }
  if (const auto icon =
          table->Find(uint16_t(SpaSection::kImage), kXdbfIdTitle)) {
    constexpr uint8_t kPng[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (icon->size() <= 4 * 1024 * 1024 && icon->size() >= 33 &&
        std::equal(std::begin(kPng), std::end(kPng), icon->begin()) &&
        u32(icon->data() + 12) == 0x49484452 && u32(icon->data() + 16) > 0 &&
        u32(icon->data() + 16) <= 1024 && u32(icon->data() + 20) > 0 &&
        u32(icon->data() + 20) <= 1024) {
      result.icon_png.assign(icon->begin(), icon->end());
    }
  }
  return result;
}
}  // namespace xe::vfs
