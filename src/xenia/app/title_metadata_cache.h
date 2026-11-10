/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_APP_TITLE_METADATA_CACHE_H_
#define XENIA_APP_TITLE_METADATA_CACHE_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "xenia/app/game_library.h"
#include "xenia/title_catalog.h"

namespace xe::app {

using TitleFileStamp = CatalogFileStamp;

struct PhysicalTitleFingerprint {
  std::filesystem::path source_path;
  // Primary executable/container followed by sorted SVOD companion files.
  std::vector<TitleFileStamp> files;
  uint64_t value = 0;
  bool operator==(const PhysicalTitleFingerprint &) const = default;
};

struct PhysicalTitleMetadata {
  PhysicalTitleFingerprint fingerprint;
  GameFileInfo info;
  std::filesystem::path icon_path;
  bool from_cache = false;
};

std::optional<PhysicalTitleFingerprint>
FingerprintPhysicalTitle(const std::filesystem::path &path);
// Worker-only filesystem/metadata operation. No active config or cvars are
// consulted. Cache hits verify exact path, every fingerprint component and
// bounded payload integrity. Changed-during-read results are discarded.
std::optional<PhysicalTitleMetadata>
ReadCachedPhysicalTitle(const std::filesystem::path &cache_root,
                        const std::filesystem::path &path);

// Pure composition; config precedence and verified DB membership are supplied
// by the UI publisher, never guessed or persisted in the intrinsic cache.
TitleCatalogVariant ResolveCatalogVariant(const LibraryEntry &entry,
                                          const LibraryPath &path,
                                          const PhysicalTitleMetadata *metadata,
                                          uint32_t effective_config_mask,
                                          uint32_t verified_family_id = 0);

} // namespace xe::app
#endif // XENIA_APP_TITLE_METADATA_CACHE_H_
