/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_VFS_TITLE_METADATA_H_
#define XENIA_VFS_TITLE_METADATA_H_
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "third_party/tomlplusplus/toml.hpp"

namespace xe::vfs {
enum class TitleResourcesStatus { kNotRequested, kAbsent, kLoaded, kInvalid };
enum class TitleSemanticKind {
  kUnknown,
  kGame,
  kApplication,
  kSystemApplication
};
enum class TitleEntitlement { kUnknown, kTrial, kFull };

struct TitlePresentationDescriptor {
  TitleSemanticKind kind = TitleSemanticKind::kUnknown;
  TitleEntitlement entitlement = TitleEntitlement::kUnknown;
  uint32_t effective_license_mask = 0;
};

struct TitleResourceMetadata {
  // Raw SPA TitleType. This is NOT XCONTENT_DATA_INTERNAL.category.
  uint32_t title_id = 0;
  uint32_t raw_title_type = 4;
  uint32_t flags = 0;
  uint32_t default_language = 1;
  std::map<uint32_t, std::string> names;
  std::vector<uint8_t> icon_png;

  std::string Name(uint32_t language = 1) const;
  toml::table ToToml() const;
};

// Uses the same validated XDBF table view as the runtime XdbfFile loader.
std::optional<TitleResourceMetadata> ExtractTitleResourceMetadata(
    std::span<const uint8_t> xdbf);

// Presentation only: never changes the physical content type or configuration.
// The caller resolves config/package licenses and supplies their effective
// mask.
TitlePresentationDescriptor DescribeTitlePresentation(
    uint32_t physical_content_type, const TitleResourceMetadata* resources,
    uint32_t effective_license_mask);
}  // namespace xe::vfs
#endif  // XENIA_VFS_TITLE_METADATA_H_
