/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_TITLE_CATALOG_H_
#define XENIA_TITLE_CATALOG_H_

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/platform.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
#include <sys/stat.h>
#endif
#include "xenia/vfs/title_metadata.h"
#include "xenia/xbox.h"

namespace xe {

struct CatalogFileStamp {
  std::filesystem::path source_path;
  std::filesystem::path physical_path;
  uint64_t size = 0;
  int64_t mtime = 0;
  uint64_t header_hash = 0;
  bool native_identity = false;
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t change_seconds = 0;
  uint32_t change_nanoseconds = 0;
  bool operator==(const CatalogFileStamp &) const = default;
};

inline std::optional<std::vector<CatalogFileStamp>>
InspectCatalogFiles(const std::filesystem::path &path,
                    bool sample_headers = true) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
  if (ec) {
    return std::nullopt;
  }
  std::vector<std::filesystem::path> inputs{absolute};
  auto companions = absolute;
  companions += ".data";
  if (std::filesystem::is_directory(companions, ec)) {
    std::filesystem::directory_iterator it(companions, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      if (it->is_regular_file(ec) && !ec) {
        inputs.push_back(it->path());
      }
    }
    if (ec) {
      return std::nullopt;
    }
    std::sort(inputs.begin() + 1, inputs.end());
  } else if (ec && ec != std::errc::no_such_file_or_directory) {
    return std::nullopt;
  }
  std::vector<CatalogFileStamp> result;
  for (const auto &input : inputs) {
    CatalogFileStamp stamp;
    stamp.source_path = input;
    stamp.physical_path = std::filesystem::canonical(input, ec);
    if (ec || !std::filesystem::is_regular_file(stamp.physical_path, ec) ||
        ec) {
      return std::nullopt;
    }
    stamp.size = std::filesystem::file_size(stamp.physical_path, ec);
    if (ec) {
      return std::nullopt;
    }
    const auto time = std::filesystem::last_write_time(stamp.physical_path, ec);
    if (ec) {
      return std::nullopt;
    }
    stamp.mtime = int64_t(time.time_since_epoch().count());
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    struct stat native;
    if (::stat(stamp.physical_path.c_str(), &native)) {
      return std::nullopt;
    }
    stamp.native_identity = true;
    stamp.device = uint64_t(native.st_dev);
    stamp.inode = uint64_t(native.st_ino);
#if XE_PLATFORM_MAC
    stamp.change_seconds = int64_t(native.st_ctimespec.tv_sec);
    stamp.change_nanoseconds = uint32_t(native.st_ctimespec.tv_nsec);
#else
    stamp.change_seconds = int64_t(native.st_ctim.tv_sec);
    stamp.change_nanoseconds = uint32_t(native.st_ctim.tv_nsec);
#endif
#endif
    if (sample_headers || !stamp.native_identity) {
      std::array<char, 64 * 1024> bytes;
      const size_t count = size_t(std::min<uint64_t>(stamp.size, bytes.size()));
      std::ifstream stream(stamp.physical_path, std::ios::binary);
      if (!stream || !stream.read(bytes.data(), count)) {
        return std::nullopt;
      }
      stamp.header_hash = 14695981039346656037ull;
      for (size_t i = 0; i < count; ++i) {
        stamp.header_hash =
            (stamp.header_hash ^ uint8_t(bytes[i])) * 1099511628211ull;
      }
    }
    result.push_back(std::move(stamp));
  }
  return result;
}

// An immutable presentation snapshot for one exact physical launch variant.
// Family membership affects display only. All operations retain actual IDs,
// version, media and launch path, including variants hidden by a full release.
struct TitleCatalogVariant {
  uint32_t title_id = 0;
  uint32_t version = 0;
  std::string name;
  std::filesystem::path path;
  std::filesystem::path icon_path;
  XContentType content_type = XContentType::kInvalid;
  uint32_t media_id = 0;
  uint64_t variant_id = 0;
  std::filesystem::path physical_path;
  uint64_t physical_size = 0;
  int64_t physical_mtime = 0;
  std::vector<CatalogFileStamp> physical_files;
  uint32_t family_id = 0;
  bool verified_family = false;
  bool is_default = false;
  bool available = false;
  bool metadata_valid = false;
  uint32_t package_license_mask = 0;
  // The STFS field may be copied verbatim. SPA TitleType is not this enum.
  std::optional<uint32_t> native_category;
  vfs::TitleResourcesStatus resources_status =
      vfs::TitleResourcesStatus::kNotRequested;
  vfs::TitlePresentationDescriptor presentation;
};

struct CatalogVariantName {
  uint32_t title_id = 0;
  uint32_t version = 0;
  std::optional<uint64_t> variant_id;
};

inline std::string CatalogVariantFileName(const TitleCatalogVariant &variant) {
  std::string result = "XLIB";
  for (const auto [value, width] :
       std::array<std::pair<uint64_t, size_t>, 3>{{{variant.title_id, 8},
                                                   {variant.version, 8},
                                                   {variant.variant_id, 16}}}) {
    char digits[16];
    const auto converted =
        std::to_chars(digits, digits + sizeof(digits), value, 16);
    result.append(width - size_t(converted.ptr - digits), '0');
    for (const char *digit = digits; digit != converted.ptr; ++digit) {
      result.push_back(char(std::toupper(static_cast<unsigned char>(*digit))));
    }
  }
  return result;
}

inline std::optional<CatalogVariantName>
ParseCatalogVariantFileName(std::string_view name) {
  if ((name.size() != 20 && name.size() != 36) ||
      std::toupper(static_cast<unsigned char>(name[0])) != 'X' ||
      std::toupper(static_cast<unsigned char>(name[1])) != 'L' ||
      std::toupper(static_cast<unsigned char>(name[2])) != 'I' ||
      std::toupper(static_cast<unsigned char>(name[3])) != 'B') {
    return std::nullopt;
  }
  auto number = [&](size_t at, size_t count) -> std::optional<uint64_t> {
    uint64_t value = 0;
    const auto parsed =
        std::from_chars(name.data() + at, name.data() + at + count, value, 16);
    return parsed.ec == std::errc() && parsed.ptr == name.data() + at + count
               ? std::optional(value)
               : std::nullopt;
  };
  const auto title = number(4, 8), version = number(12, 8);
  const auto variant =
      name.size() == 36 ? number(20, 16) : std::optional<uint64_t>{};
  if (!title || !version || (name.size() == 36 && !variant)) {
    return std::nullopt;
  }
  return CatalogVariantName{uint32_t(*title), uint32_t(*version), variant};
}

inline std::optional<size_t>
UniqueCatalogVariantIndex(std::span<const TitleCatalogVariant> variants,
                          const CatalogVariantName &name) {
  std::optional<size_t> result;
  for (size_t i = 0; i < variants.size(); ++i) {
    const auto &variant = variants[i];
    if (variant.title_id != name.title_id || variant.version != name.version ||
        (name.variant_id && variant.variant_id != *name.variant_id)) {
      continue;
    }
    if (result) {
      return std::nullopt;
    }
    result = i;
  }
  return result;
}

inline bool IsSharedXbligHost(uint32_t title_id) {
  return title_id == 0x584E07D1 || title_id == 0x584E07D2;
}

inline std::string CatalogFamilyKey(const TitleCatalogVariant &variant) {
  if (!variant.title_id || IsSharedXbligHost(variant.title_id)) {
    // The host's title ID does not identify the independent game it hosts.
    return "xblig:" + variant.path.generic_string();
  }
  const uint32_t family = variant.verified_family && variant.family_id
                              ? variant.family_id
                              : variant.title_id;
  return std::to_string(uint32_t(variant.presentation.kind)) + ":" +
         std::to_string(family);
}

inline std::vector<size_t>
PreferredCatalogVariants(std::span<const TitleCatalogVariant> variants) {
  std::vector<size_t> result;
  std::unordered_map<std::string, size_t> groups;
  auto rank = [](const TitleCatalogVariant &variant) {
    return (variant.available ? 8 : 0) + (variant.metadata_valid ? 4 : 0) +
           (variant.presentation.entitlement == vfs::TitleEntitlement::kFull
                ? 2
                : 0) +
           (variant.is_default ? 1 : 0);
  };
  for (size_t i = 0; i < variants.size(); ++i) {
    const auto [where, inserted] =
        groups.emplace(CatalogFamilyKey(variants[i]), result.size());
    if (inserted) {
      result.push_back(i);
    } else if (rank(variants[i]) > rank(variants[result[where->second]])) {
      result[where->second] = i;
    }
  }
  return result;
}

inline bool SameCatalogFileStamp(const CatalogFileStamp &a,
                                 const CatalogFileStamp &b,
                                 bool source_spelling) {
  if ((source_spelling && a.source_path != b.source_path) ||
      a.physical_path != b.physical_path || a.size != b.size ||
      a.mtime != b.mtime || a.native_identity != b.native_identity) {
    return false;
  }
  if (a.native_identity) {
    return a.device == b.device && a.inode == b.inode &&
           a.change_seconds == b.change_seconds &&
           a.change_nanoseconds == b.change_nanoseconds;
  }
  // A portable fallback is a cache fingerprint, never full-byte identity proof.
  return a.header_hash == b.header_hash;
}

inline bool
CatalogVariantMatchesPhysicalFile(const TitleCatalogVariant &variant,
                                  const std::filesystem::path &path) {
  if (!variant.metadata_valid || !variant.available ||
      variant.physical_files.empty()) {
    return false;
  }
  // POSIX native identity/change time avoids hashing package data on a UI
  // publish or guest query. Worker scans also sample only bounded headers.
  const auto current = InspectCatalogFiles(variant.path, false);
  if (!current || current->size() != variant.physical_files.size()) {
    return false;
  }
  for (size_t i = 0; i < current->size(); ++i) {
    if (!SameCatalogFileStamp((*current)[i], variant.physical_files[i], true)) {
      return false;
    }
  }
  if (path == variant.path) {
    return true;
  }
  const auto candidate = InspectCatalogFiles(path, false);
  if (!candidate || candidate->size() != current->size()) {
    return false;
  }
  for (size_t i = 0; i < current->size(); ++i) {
    // Aliases may use different spellings, but every actual file/companion
    // must be identical. A title/media/name match is insufficient.
    if (!SameCatalogFileStamp((*candidate)[i], (*current)[i], false)) {
      return false;
    }
  }
  return true;
}

} // namespace xe
#endif // XENIA_TITLE_CATALOG_H_
