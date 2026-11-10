/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_XEX_METADATA_H_
#define XENIA_VFS_XEX_METADATA_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/vfs/title_metadata.h"

namespace xe {
namespace vfs {

// Offline metadata listing never allocates an arbitrary declared executable
// member/header or decoded image. These limits do not constrain runtime loads.
inline constexpr size_t kMaxOfflineXexHeaderBytes = 16u * 1024 * 1024;
inline constexpr size_t kMaxOfflineXexFileBytes = 256u * 1024 * 1024;
inline constexpr size_t kMaxOfflineXexImageBytes = 256u * 1024 * 1024;

enum class XexFormat {
  kUnknown,
  kXex1,
  kXex2,
};

struct XexVersion {
  uint8_t major = 0;
  uint8_t minor = 0;
  uint16_t build = 0;
  uint8_t qfe = 0;

  // "major.minor.build.qfe".
  std::string ToString() const;
  // Packed as it appears in the XEX execution info.
  uint32_t value() const;
  static XexVersion FromValue(uint32_t value);
  // Parses what ToString() produces. Empty unless the text is exactly that,
  // so a version can never be spelled two ways.
  static std::optional<XexVersion> FromString(std::string_view text);

  bool operator==(const XexVersion& other) const = default;
};

struct XexMetadata {
  XexFormat format = XexFormat::kUnknown;
  std::string module_name;
  // The only path the executable may load from.
  std::string bounding_path;

  // From XEX_HEADER_EXECUTION_INFO
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t savegame_id = 0;
  XexVersion version = {};
  XexVersion base_version = {};
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;

  // From the security info
  uint32_t allowed_media_types = 0;

  // Opt-in offline SPA summary; independent of profiles and guest execution.
  TitleResourcesStatus title_resources_status =
      TitleResourcesStatus::kNotRequested;
  std::optional<TitleResourceMetadata> title_resources;

  // From XEX_HEADER_GAME_RATINGS
  struct {
    bool present = false;
    uint8_t esrb = 0xFF;
    uint8_t pegi = 0xFF;
    uint8_t cero = 0xFF;
    uint8_t usk = 0xFF;
    uint8_t oflc_au = 0xFF;
    uint8_t oflc_nz = 0xFF;
  } ratings;

  // True for an executable no disc can hold, so one an STFS package held.
  bool RunsFromPackageOnly() const;

  toml::table ToToml() const;
};

std::optional<XexMetadata> ExtractXexMetadata(
    const std::filesystem::path& path, bool read_title_resources = false);
std::optional<XexMetadata> ExtractXexMetadata(
    const uint8_t* data, size_t size, bool read_title_resources = false);

// Bounded installed/offline module resource snapshot; shares the title decoder.
enum class XexResourceStatus { kLoaded, kNotFound, kInvalid, kTooLarge };
struct XexResourceResult {
  XexResourceStatus status = XexResourceStatus::kInvalid;
  std::vector<uint8_t> bytes;
};
XexResourceResult ExtractXexResource(std::span<const uint8_t> xex,
                                     std::string_view section);

std::optional<TitleResourceMetadata> ExtractXexTitleResources(
    std::span<const uint8_t> xex);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_XEX_METADATA_H_
