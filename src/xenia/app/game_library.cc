/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_library.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <fstream>
#include <regex>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/host_metadata_writer.h"
#include "xenia/base/platform.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/title_ids.h"
#include "xenia/vfs/iso_metadata.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/xbe_metadata.h"
#include "xenia/vfs/xex_metadata.h"
#include "xenia/vfs/zar_metadata.h"

namespace xe {
namespace app {

// A release folder name, or empty when the name is not one. Only the exact
// spelling VersionToString() produces is accepted, so one version can never
// end up naming two folders.
static std::optional<uint32_t> ParseVersionFolder(
    const std::filesystem::path& name) {
  const auto parsed = vfs::XexVersion::FromString(xe::path_to_utf8(name));
  return parsed ? std::optional<uint32_t>(parsed->value()) : std::nullopt;
}

// Two paths can name the same file through a symlink or just a different case.
// Falls back to comparing the strings when the file no longer exists.
static bool SamePath(const std::filesystem::path& a,
                     const std::filesystem::path& b) {
  if (a == b) {
    return true;
  }
  std::error_code ec;
  return std::filesystem::equivalent(a, b, ec);
}

// '/' separators avoid TOML escaping and round-trip through std::filesystem.
static std::string PathToToml(const std::filesystem::path& path) {
  std::string s = xe::path_to_utf8(path);
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

// Force exactly one default: keep the first flagged one, else flag the first.
static void NormalizeDefault(LibraryEntry& entry) {
  bool seen = false;
  for (auto& p : entry.paths) {
    if (p.is_default && !seen) {
      seen = true;
    } else {
      p.is_default = false;
    }
  }
  if (!seen && !entry.paths.empty()) {
    entry.paths.front().is_default = true;
  }
}

const LibraryPath& LibraryEntry::default_path() const {
  assert_false(paths.empty());
  for (const auto& p : paths) {
    if (p.is_default) {
      return p;
    }
  }
  return paths.front();
}

std::string VersionToString(uint32_t version) {
  return vfs::XexVersion::FromValue(version).ToString();
}

namespace {
// An original Xbox game describes itself in its executable's certificate,
// which has no media id.
GameFileInfo FromXbe(vfs::XbeMetadata m) {
  GameFileInfo info;
  info.version = m.version;
  info.disc_number = uint8_t(m.disc_number);
  info.icon = std::move(m.icon_png);
  info.title_name = std::move(m.title_name);
  info.content_type = XContentType::kXboxTitle;
  info.title_id = m.title_id;
  return info;
}
GameFileInfo FromXex(vfs::XexMetadata metadata) {
  GameFileInfo info;
  info.media_id = metadata.media_id;
  info.version = metadata.version.value();
  info.disc_number = metadata.disc_number;
  info.disc_count = metadata.disc_count;
  info.title_id = metadata.title_id;
  info.title_resources_status = metadata.title_resources_status;
  info.title_resources = std::move(metadata.title_resources);
  if (info.title_resources) {
    info.title_name = info.title_resources->Name();
    info.display_name = info.title_name;
    info.icon = info.title_resources->icon_png;
  }
  return info;
}
}  // namespace

std::optional<GameFileInfo> ReadGameFileInfo(const std::filesystem::path &path,
                                             bool read_title_resources) {
  const auto extension =
      xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  if (extension == ".xbe") {
    if (auto m = vfs::ExtractXbeMetadata(path)) {
      return FromXbe(std::move(*m));
    }
  } else if (extension == ".xex") {
    if (auto m = vfs::ExtractXexMetadata(path, read_title_resources)) {
      return FromXex(std::move(*m));
    }
  } else if (extension == ".iso") {
    if (auto m = vfs::ExtractIsoMetadata(path, read_title_resources)) {
      return FromXex(std::move(*m));
    }
    if (auto m = vfs::ExtractXbeMetadata(path)) {
      return FromXbe(std::move(*m));
    }
  } else if (extension == ".zar") {
    if (auto m = vfs::ExtractZarMetadata(path, read_title_resources)) {
      return FromXex(std::move(*m));
    }
  } else if (auto m = vfs::ExtractStfsMetadata(path, XLanguage::kEnglish,
                                               read_title_resources)) {
    // An Xbox Original package is listed as the game its executable names.
    if (m->content_type == uint32_t(XContentType::kXboxTitle)) {
      if (auto xbe = vfs::ExtractXbeMetadata(path)) {
        return FromXbe(std::move(*xbe));
      }
      return std::nullopt;
    }
    // The container describes this release specifically, so take all of it
    // while we are here. The title-level record cannot tell releases apart.
    GameFileInfo info{m->media_id,
                      m->version,
                      m->disc_number,
                      m->disc_count,
                      std::move(m->icon_data),
                      std::move(m->title_name),
                      std::move(m->display_name),
                      XContentType(m->content_type)};
    info.title_id = m->title_id;
    info.package_license_mask = m->package_license_mask;
    info.native_category = m->native_category;
    info.title_resources_status = m->title_resources_status;
    info.title_resources = std::move(m->title_resources);
    if (info.title_resources) {
      if (info.icon.empty()) {
        info.icon = info.title_resources->icon_png;
      }
      if (info.title_name.empty()) {
        info.title_name = info.title_resources->Name();
      }
    }
    return info;
  }
  return std::nullopt;
}

GameLibrary::GameLibrary(std::filesystem::path root) : root_(std::move(root)) {}

std::shared_ptr<filesystem::HostMetadataWriter> GameLibrary::Writer() const {
  if (!writer_) {
    std::error_code error;
    writer_ = filesystem::HostMetadataWriter::Open(root_, error);
    if (error) XELOGW("GameLibrary metadata authority: {}", error.message());
  }
  return writer_;
}

std::shared_ptr<const filesystem::HostStorageSnapshotToken> GameLibrary::Observed(
    const LibraryKey& key) const {
  const auto found = std::find_if(observations_.begin(), observations_.end(),
      [&](const auto& item) { return item.key == key; });
  return found == observations_.end() || found->ambiguous ? nullptr : found->snapshot;
}
bool GameLibrary::HasObservation(const LibraryKey& key) const {
  return std::any_of(observations_.begin(), observations_.end(),
      [&](const auto& item) { return item.key == key; });
}
void GameLibrary::Remember(const LibraryKey& key,
    std::shared_ptr<const filesystem::HostStorageSnapshotToken> snapshot) {
  const auto found = std::find_if(observations_.begin(), observations_.end(),
      [&](const auto& item) { return item.key == key; });
  if (found == observations_.end()) observations_.push_back({key, std::move(snapshot)});
  else found->snapshot = std::move(snapshot);
}

std::filesystem::path GameLibrary::TitleDir(uint32_t title_id) const {
  return root_ / fmt::format("{:08X}", title_id);
}

std::filesystem::path GameLibrary::EntryDir(const LibraryKey& key) const {
  return TitleDir(key.title_id) / xe::to_path(VersionToString(key.version));
}

std::filesystem::path GameLibrary::IconPath(const LibraryKey& key) const {
  return EntryDir(key) / "icon.png";
}

LibraryEntry* GameLibrary::Find(const LibraryKey& key) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const LibraryEntry& e) { return e.key() == key; });
  return it == entries_.end() ? nullptr : &*it;
}

const LibraryEntry* GameLibrary::Find(const LibraryKey& key) const {
  return const_cast<GameLibrary*>(this)->Find(key);
}

LibraryEntry* GameLibrary::FindByTitle(uint32_t title_id) {
  auto it = std::find_if(
      entries_.begin(), entries_.end(),
      [=](const LibraryEntry& e) { return e.title_id == title_id; });
  return it == entries_.end() ? nullptr : &*it;
}

const LibraryEntry* GameLibrary::FindByTitle(uint32_t title_id) const {
  return const_cast<GameLibrary*>(this)->FindByTitle(title_id);
}

LibraryEntry* GameLibrary::FindByPath(uint32_t title_id,
                                      const std::filesystem::path& path) {
  for (auto& entry : entries_) {
    if (entry.title_id != title_id) {
      continue;
    }
    for (const auto& entry_path : entry.paths) {
      if (SamePath(entry_path.path, path)) {
        return &entry;
      }
    }
  }
  return nullptr;
}

const LibraryEntry* GameLibrary::FindByPath(
    uint32_t title_id, const std::filesystem::path& path) const {
  return const_cast<GameLibrary*>(this)->FindByPath(title_id, path);
}

bool GameLibrary::ReadEntry(const std::filesystem::path& game_toml,
                            LibraryEntry& entry, bool* removed,
                            std::shared_ptr<const filesystem::HostStorageSnapshotToken>* observed) const {
  if (observed) observed->reset();
  const auto writer = Writer();
  std::error_code read_error;
  const auto absolute = std::filesystem::absolute(game_toml, read_error).lexically_normal();
  if (read_error) return false;
  const auto snapshot = writer->Capture(absolute.lexically_relative(writer->path()),
                                        2ull << 20, read_error);
  std::string contents;
  if (snapshot && !read_error && snapshot->exists()) {
    const auto bytes = snapshot->bytes();
    contents.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  } else {
#if XE_PLATFORM_WIN32
    // Preserve bounded read-only compatibility while the common durable
    // snapshot publisher is unsupported on Windows. Never raw-write fallback.
    if (read_error != std::errc::operation_not_supported) return false;
    const auto size = std::filesystem::file_size(game_toml, read_error);
    if (read_error || size > (2ull << 20)) return false;
    std::ifstream file(game_toml, std::ios::binary);
    contents.resize(size_t(size));
    if (!file || !file.read(contents.data(), std::streamsize(contents.size()))) return false;
#else
    return false;
#endif
  }
  toml::table tbl;
  try {
    tbl = toml::parse(contents);
  } catch (const toml::parse_error& e) {
    XELOGE("GameLibrary: failed to parse {}: {}",
           xe::strip_pii(xe::path_to_utf8(game_toml)), e.description());
    return false;
  }

  const auto removal = tbl["removed"].value<bool>();
  if (tbl.contains("removed") && !removal) return false;
  if (removed) *removed = removal.value_or(false);
  else if (removal.value_or(false)) return false;
  auto title_id_str = tbl["title_id"].value<std::string>();
  if (!title_id_str) {
    XELOGW("GameLibrary: missing title_id, skipping {}",
           xe::strip_pii(xe::path_to_utf8(game_toml)));
    return false;
  }
  entry.title_id = xe::string_util::from_string<uint32_t>(*title_id_str, true);
  entry.name = tbl["name"].value_or(std::string());

  if (auto version_str = tbl["version"].value<std::string>()) {
    if (auto parsed = vfs::XexVersion::FromString(*version_str)) {
      entry.version = parsed->value();
    }
  }
  entry.last_played =
      static_cast<time_t>(tbl["last_played"].value_or(int64_t(0)));
  if (auto content_type_str = tbl["content_type"].value<std::string>()) {
    entry.content_type = XContentType(
        xe::string_util::from_string<uint32_t>(*content_type_str, true));
  }

  entry.metadata_revision = tbl["metadata_revision"].value_or(uint32_t(0));
  entry.package_license_mask =
      tbl["package_license_mask"].value_or(uint32_t(0));
  const auto resource_status =
      tbl["title_resources_status"].value_or(uint32_t(0));
  entry.title_resources_status =
      resource_status <= uint32_t(vfs::TitleResourcesStatus::kInvalid)
          ? static_cast<vfs::TitleResourcesStatus>(resource_status)
          : vfs::TitleResourcesStatus::kNotRequested;
  if (auto* spa = tbl["title_resources"].as_table()) {
    vfs::TitleResourceMetadata resources;
    resources.title_id = (*spa)["title_id"].value_or(uint32_t(0));
    resources.raw_title_type = (*spa)["raw_title_type"].value_or(uint32_t(4));
    resources.flags = (*spa)["flags"].value_or(uint32_t(0));
    resources.default_language =
        (*spa)["default_language"].value_or(uint32_t(1));
    if (auto* names = (*spa)["names"].as_table()) {
      for (const auto& [key, value] : *names) {
        uint32_t language = 0;
        const auto text = key.str();
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), language);
        if (auto name = value.value<std::string>();
            name && parsed.ec == std::errc() &&
            parsed.ptr == text.data() + text.size() && language > 0 &&
            language < uint32_t(XLanguage::kMaxLanguages)) {
          resources.names.emplace(language, *name);
        }
      }
    }
    entry.title_resources = std::move(resources);
  }

  if (auto* paths = tbl["paths"].as_array()) {
    for (auto&& node : *paths) {
      auto* pt = node.as_table();
      if (!pt) {
        continue;
      }
      auto path_str = (*pt)["path"].value<std::string>();
      if (!path_str || path_str->empty()) {
        continue;
      }
      LibraryPath lp;
      lp.path = xe::to_path(*path_str);
      lp.is_default = (*pt)["default"].value_or(false);
      if (auto media_id_str = (*pt)["media_id"].value<std::string>()) {
        lp.media_id =
            xe::string_util::from_string<uint32_t>(*media_id_str, true);
      }
      lp.disc_number = static_cast<uint8_t>((*pt)["disc"].value_or(0));
      entry.paths.push_back(std::move(lp));
    }
  }

  NormalizeDefault(entry);
  if (observed) *observed = snapshot;
  return true;
}

std::vector<GameLibrary::Release> GameLibrary::SplitIntoReleases(
    const LibraryEntry& entry, bool& identified) const {
  std::vector<Release> result;
  identified = false;
  if (entry.paths.empty()) {
    result.push_back(Release{entry});
    return result;
  }

  std::vector<std::optional<GameFileInfo>> infos;
  infos.reserve(entry.paths.size());
  for (const auto& path : entry.paths) {
    infos.push_back(ReadGameFileInfo(path.path));
  }

  // A file that can't be read right now (moved, unplugged) must not look like
  // a release of its own, or a missing disc would split the title in two.
  // Those ride along with the first release we could identify. If none were
  // readable the entry stays whole and unidentified, to retry another time.
  identified = std::any_of(
      infos.begin(), infos.end(),
      [](const std::optional<GameFileInfo>& info) { return info.has_value(); });
  if (!identified) {
    result.push_back(Release{entry});
    return result;
  }

  // Group on version. Discs of a set share one, so they land together with
  // no special handling, and two releases of the game differ in it.
  //
  // The paths we could read come first, so that every release exists before
  // an unreadable path looks for one to join. Interleaving the two lets the
  // very first path be the unreadable one, with no release to attach it to.
  std::vector<uint8_t> best_disc;
  for (size_t i = 0; i < entry.paths.size(); ++i) {
    if (!infos[i]) {
      continue;
    }
    const GameFileInfo& info = *infos[i];
    auto it = std::find_if(result.begin(), result.end(), [&](const Release& r) {
      return r.entry.version == info.version;
    });
    if (it == result.end()) {
      Release part;
      part.entry.title_id = entry.title_id;
      part.entry.version = info.version;
      part.entry.name = entry.name;
      part.entry.content_type = info.content_type;
      part.entry.metadata_revision = kLibraryMetadataRevision;
      part.entry.package_license_mask = info.package_license_mask;
      part.entry.title_resources_status = info.title_resources_status;
      part.entry.title_resources = info.title_resources;
      result.push_back(std::move(part));
      best_disc.push_back(0);
      it = std::prev(result.end());
    }
    const size_t slot = static_cast<size_t>(it - result.begin());

    // A release takes its art and names from its first disc, the one updates
    // are built against.
    const uint8_t disc =
        static_cast<uint8_t>(info.disc_number ? info.disc_number : 1);
    if (!best_disc[slot] || disc < best_disc[slot]) {
      best_disc[slot] = disc;
      it->icon = info.icon;
      it->title_name = info.title_name;
      it->display_name = info.display_name;
    }

    LibraryPath lp = entry.paths[i];
    lp.media_id = info.media_id;
    lp.disc_number = info.disc_number;
    it->entry.paths.push_back(std::move(lp));
  }

  // Then the ones we couldn't read. `identified` guarantees a release exists.
  for (size_t i = 0; i < entry.paths.size(); ++i) {
    if (!infos[i]) {
      result.front().entry.paths.push_back(entry.paths[i]);
    }
  }

  NameReleases(result, entry.name);

  for (auto& part : result) {
    NormalizeDefault(part.entry);
  }
  return result;
}

void GameLibrary::NameReleases(std::vector<Release>& releases,
                               const std::string& fallback) const {
  // An expansion shares its base game's title_name, so if every release says
  // the same thing that field cannot be what tells them apart.
  const bool titles_differ =
      std::any_of(releases.begin(), releases.end(), [&](const Release& r) {
        return r.title_name != releases.front().title_name;
      });
  const bool prefer_display = releases.size() > 1 && !titles_differ;

  for (auto& release : releases) {
    const std::string& first =
        prefer_display ? release.display_name : release.title_name;
    const std::string& second =
        prefer_display ? release.title_name : release.display_name;
    if (!first.empty()) {
      release.entry.name = first;
    } else if (!second.empty()) {
      release.entry.name = second;
    } else {
      release.entry.name = fallback;
    }
  }
}

bool GameLibrary::PlaceIcon(const std::filesystem::path& dir,
                            const std::vector<uint8_t>& icon,
                            const std::filesystem::path& fallback_dir) const {
  const auto writer = Writer();
  std::error_code error;
  const auto relative = std::filesystem::absolute(dir, error).lexically_normal().lexically_relative(writer->path()) / "icon.png";
  if (error) return false;
  const auto current = writer->Capture(relative, 4ull << 20, error);
  if (current && current->exists()) {
    // Existing custom art wins over stale legacy/package art. Observe its
    // actual barriers; never overwrite it merely because migration retries.
    const auto observed = current->root()->ObserveSnapshotDurability(*current);
    return !observed.error && observed.durable;
  }
  if (error && error != std::errc::no_such_file_or_directory) return false;
  if (!icon.empty()) return bool(writer->Write(relative, icon, nullptr, true));
  const auto fallback = writer->Capture(
      std::filesystem::absolute(fallback_dir, error).lexically_normal().lexically_relative(writer->path()) / "icon.png",
      4ull << 20, error);
  if (!fallback || error || !fallback->exists()) return false;
  return bool(writer->Write(relative, fallback->bytes(), nullptr, true));
}

void GameLibrary::Load() {
  observations_.clear();
  entries_.clear();
  split_titles_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(root_, ec)) {
    return;
  }

  const auto title_dirs = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(root_), std::regex("[0-9A-Fa-f]{8}"));

  for (const auto& title_dir : title_dirs) {
    // XeFu runs original Xbox games, which are listed as themselves.
    if (xe::string_util::from_string<uint32_t>(xe::path_to_utf8(title_dir.name),
                                               true) == kXeFuTitleId) {
      continue;
    }
    const auto dir = title_dir.path / title_dir.name;

    std::vector<std::filesystem::path> removed_paths;
    const uint32_t title_id = xe::string_util::from_string<uint32_t>(
        xe::path_to_utf8(title_dir.name), true);
    for (const auto& sub : xe::filesystem::ListDirectories(dir)) {
      const auto version = ParseVersionFolder(sub.name);
      if (!version) continue;
      LibraryEntry entry;
      bool removed = false;
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> observed;
      if (!ReadEntry(sub.path / sub.name / "game.toml", entry, &removed, &observed) ||
          entry.title_id != title_id) continue;
      entry.version = *version;
      entry.canonical_snapshot = observed;
      if (observed) {
        const auto prior = std::find_if(observations_.begin(), observations_.end(),
            [&](const auto& item) { return item.key == entry.key(); });
        if (prior != observations_.end()) prior->ambiguous = true;
        else Remember(entry.key(), std::move(observed));
      }
      if (removed) {
        for (const auto& path : entry.paths) removed_paths.push_back(path.path);
      } else {
        entries_.push_back(std::move(entry));
      }
    }
    // Partial migration is retryable even when another release succeeded.
    // Existing canonical slots (including unreadable/removal records) block
    // stale legacy writes. Original metadata and artwork are never deleted.
    MigrateTitle(dir, removed_paths);
  }
  if (changed_callback_) {
    changed_callback_();
  }
}

void GameLibrary::MigrateTitle(const std::filesystem::path& title_dir,
    const std::vector<std::filesystem::path>& removed_paths) {
  std::error_code ec;

  // Where the title's data actually is. A build that keyed releases on media
  // id left <title_id>/<media_id>/ folders holding newer data than the root
  // file, so those win outright when present. Never both, or the root file's
  // stale copy of the same paths would come along too.
  LibraryEntry merged;
  bool found = false;
  for (const auto& sub :
       xe::filesystem::FilterByName(xe::filesystem::ListDirectories(title_dir),
                                    std::regex("[0-9A-Fa-f]{8}"))) {
    const auto sub_dir = sub.path / sub.name;
    LibraryEntry entry;
    if (!ReadEntry(sub_dir / "game.toml", entry)) {
      continue;
    }

    // Pooled rather than migrated one folder at a time. Those folders were
    // keyed on media id, which gave each disc of a set its own, and two of
    // them now resolve to the same version folder. Splitting the pool is
    // also what puts such a set back together.
    found = true;
    merged.title_id = entry.title_id;
    if (merged.name.empty()) {
      merged.name = entry.name;
    }
    for (auto& disc : entry.paths) {
      const bool known = std::any_of(
          merged.paths.begin(), merged.paths.end(),
          [&](const LibraryPath& p) { return SamePath(p.path, disc.path); });
      if (!known) {
        merged.paths.push_back(std::move(disc));
      }
    }
  }
  if (!found && !ReadEntry(title_dir / "game.toml", merged)) {
    return;
  }

  const auto expected_title = xe::string_util::from_string<uint32_t>(
      xe::path_to_utf8(title_dir.filename()), true);
  if (merged.title_id != expected_title) return;
  // Only exact recorded paths are suppressed. No title-family guess, inode
  // alias discovery or broad release suppression is used for removal.
  const auto recorded = [](const auto& a, const auto& b) {
    return a.lexically_normal() == b.lexically_normal();
  };
  std::erase_if(merged.paths, [&](const LibraryPath& path) {
    if (std::any_of(removed_paths.begin(), removed_paths.end(),
        [&](const auto& old) { return recorded(path.path, old); })) return true;
    return std::any_of(entries_.begin(), entries_.end(), [&](const LibraryEntry& existing) {
      return existing.title_id == merged.title_id &&
          std::any_of(existing.paths.begin(), existing.paths.end(),
              [&](const auto& known) { return recorded(path.path, known.path); });
    });
  });
  if (merged.paths.empty()) return;
  bool identified = false;
  auto parts = SplitIntoReleases(merged, identified);
  if (!identified) {
    // No readable path, so no version to name a folder with. Leave the title
    // where it is and try again once the files are back.
    merged.transient_legacy = true;
    entries_.push_back(std::move(merged));
    return;
  }
  if (parts.size() > 1) {
    split_titles_.push_back(merged.name.empty()
                                ? fmt::format("{:08X}", merged.title_id)
                                : merged.name);
  }
  for (auto& part : parts) {
    // A prior canonical observation is authoritative even if a foreign actor
    // removed/replaced it during this Load. Legacy is never a stale overwrite.
    if (HasObservation(part.entry.key())) continue;
    const auto release_dir = EntryDir(part.entry.key());
    const auto writer = Writer();
    std::error_code error;
    const auto relative = std::filesystem::absolute(release_dir, error).lexically_normal().lexically_relative(writer->path()) / "game.toml";
    if (error) continue;
    const auto current = writer->Capture(relative, 2ull << 20, error);
    if ((current && current->exists()) ||
        (error && error != std::errc::no_such_file_or_directory)) continue;
    // Art and metadata are independently durable single-file publications.
    // A failed part remains retryable; no multi-file transaction is claimed.
    PlaceIcon(release_dir, part.icon, title_dir);
    const bool durable = WriteEntryAtomic(part.entry, release_dir, false, true);
    if (!durable && writer->last_result().error == std::errc::file_exists) continue;
    part.entry.transient_legacy = !durable;
    if (durable) part.entry.canonical_snapshot = Observed(part.entry.key());
    entries_.push_back(std::move(part.entry));
  }
  // Every legacy source and custom artwork remains intact, on all outcomes.

}

std::vector<GameLibrary::MissingFields> GameLibrary::FindMissingFields() const {
  std::vector<MissingFields> missing;
  std::error_code ec;
  for (const auto& entry : entries_) {
    // A title still in an older layout gets its fields when it is migrated.
    const bool icon_missing = !std::filesystem::exists(IconPath(entry.key()), ec) && !ec;
    if (entry.transient_legacy ||
        (entry.content_type != XContentType::kInvalid &&
         entry.metadata_revision == kLibraryMetadataRevision && !icon_missing) ||
        !std::filesystem::exists(EntryDir(entry.key()) / "game.toml", ec)) {
      continue;
    }
    MissingFields release{entry.key()};
    if (!entry.paths.empty()) {
      release.paths.push_back(entry.default_path().path);
    }
    for (const auto& path : entry.paths) {
      if (release.paths.empty() ||
          !SamePath(path.path, release.paths.front())) {
        release.paths.push_back(path.path);
      }
    }
    missing.push_back(std::move(release));
  }
  return missing;
}

std::optional<GameFileInfo> GameLibrary::ReadMissingFields(
    const MissingFields& missing) {
  for (const auto& path : missing.paths) {
    // A path that was unreadable when the title was split can belong to
    // another release.
    auto info = ReadGameFileInfo(path);
    if (info && info->version == missing.key.version &&
        info->title_id == missing.key.title_id) {
      return info;
    }
  }
  return std::nullopt;
}

bool GameLibrary::FillMissingFields(const LibraryKey& key,
                                    const GameFileInfo& info) {
  const auto* existing = Find(key);
  if (!existing || info.content_type == XContentType::kInvalid ||
      info.title_id != key.title_id || info.version != key.version) {
    return false;
  }
  LibraryEntry entry = *existing;
  entry.content_type = info.content_type;
  entry.metadata_revision = kLibraryMetadataRevision;
  entry.package_license_mask = info.package_license_mask;
  entry.title_resources_status = info.title_resources_status;
  entry.title_resources = info.title_resources;
  if (entry.name.empty()) {
    entry.name = info.title_name.empty() ? info.display_name : info.title_name;
  }
  if (!Upsert(std::move(entry))) {
    return false;
  }
  std::error_code icon_error;
  if (!info.icon.empty() &&
      !std::filesystem::exists(IconPath(key), icon_error) && !icon_error) {
    if (!SetIcon(key, info.icon)) return false;
  }
  return true;
}

bool GameLibrary::WriteEntryAtomic(const LibraryEntry& entry,
                                   const std::filesystem::path& dir,
                                   bool removed, bool require_absent) {
  toml::array paths;
  for (const auto& p : entry.paths) {
    toml::table pt;
    if (p.disc_number) {
      pt.insert("disc", static_cast<int64_t>(p.disc_number));
    }
    pt.insert("media_id", fmt::format("{:08X}", p.media_id));
    pt.insert("path", PathToToml(p.path));
    if (p.is_default) {
      pt.insert("default", true);
    }
    pt.is_inline(true);
    paths.push_back(std::move(pt));
  }

  toml::table tbl;
  if (removed) tbl.insert("removed", true);
  // Omitted while unknown, which is what makes Load() look for it.
  if (entry.content_type != XContentType::kInvalid) {
    tbl.insert("content_type",
               fmt::format("{:08X}", uint32_t(entry.content_type)));
  }
  // Seconds since the epoch. Omitted while unset so a release that has never
  // been launched doesn't carry a field claiming it was.
  if (entry.last_played) {
    tbl.insert("last_played", static_cast<int64_t>(entry.last_played));
  }
  tbl.insert("metadata_revision", entry.metadata_revision);
  tbl.insert("package_license_mask", entry.package_license_mask);
  tbl.insert("title_resources_status", uint32_t(entry.title_resources_status));
  if (entry.title_resources) {
    const auto& resources = *entry.title_resources;
    toml::table spa;
    spa.insert("title_id", resources.title_id);
    spa.insert("raw_title_type", resources.raw_title_type);
    spa.insert("flags", resources.flags);
    spa.insert("default_language", resources.default_language);
    toml::table names;
    for (const auto& [language, name] : resources.names) {
      names.insert(std::to_string(language), name);
    }
    spa.insert("names", std::move(names));
    tbl.insert("title_resources", std::move(spa));
  }
  tbl.insert("name", entry.name);
  tbl.insert("paths", std::move(paths));
  tbl.insert("title_id", fmt::format("{:08X}", entry.title_id));
  tbl.insert("version", VersionToString(entry.version));

  std::ostringstream stream;
  stream << tbl << "\n";
  if (!stream) return false;
  const auto text = stream.str();
  const auto writer = Writer();
  std::error_code error;
  const auto relative = std::filesystem::absolute(dir, error).lexically_normal().lexically_relative(writer->path()) / "game.toml";
  if (error) return false;
  const auto model_observed = Observed(entry.key());
  const bool has_observation = HasObservation(entry.key());
  if ((require_absent && has_observation) ||
      (has_observation && !model_observed)) return false;
  const auto expected = entry.canonical_snapshot ? entry.canonical_snapshot : model_observed;
  const std::span<const uint8_t> intended{
      reinterpret_cast<const uint8_t*>(text.data()), text.size()};
  // Never reacquire a newer token for an older in-memory model. Explicit new
  // slots may only be created absent; an existing unobserved/unknown/removal
  // slot requires Load before an update or explicit re-add can be attempted.
  const auto result = expected ? writer->PublishAt(relative, expected, intended)
      : writer->Write(relative, intended, nullptr, true);
  if (!result) {
    XELOGW("GameLibrary metadata publication: {}", result.error.message());
    return false;
  }
  const auto current = result.unchanged ? expected : result.publication.current;
  if (!current) return false;
  Remember(entry.key(), current);
  return true;
}

bool GameLibrary::Upsert(LibraryEntry entry) {
  if (entry.title_id == kXeFuTitleId) {
    return false;
  }
  NormalizeDefault(entry);

  auto* existing = Find(entry.key());
  if (!WriteEntryAtomic(entry, EntryDir(entry.key()))) {
    return false;
  }

  entry.transient_legacy = false;
  entry.canonical_snapshot = Observed(entry.key());
  if (existing) {
    *existing = std::move(entry);
  } else {
    entries_.push_back(std::move(entry));
  }
  if (changed_callback_) {
    changed_callback_();
  }
  return true;
}

bool GameLibrary::AddDisc(uint32_t title_id, const std::string& name,
                          const std::filesystem::path& path) {
  if (title_id == 0 || path.empty()) {
    return false;
  }

  // Which release this belongs to is the file's own version. Discs of a set
  // share one, so a disc joins its siblings without anything having to work
  // out that they are siblings.
  const auto info = ReadGameFileInfo(path);
  const LibraryKey key{title_id, info ? info->version : 0};
  LibraryEntry* existing = Find(key);

  LibraryEntry entry;
  if (existing) {
    entry = *existing;
  } else {
    entry.title_id = title_id;
    entry.version = key.version;
    entry.name = name;
    if (entry.name.empty() && info) {
      entry.name =
          info->title_name.empty() ? info->display_name : info->title_name;
    }
    if (entry.name.empty()) {
      // A recorded disc swap supplies no name. Borrow the title's, or the
      // release shows up in the list as a nameless entry.
      if (const auto* sibling = FindByTitle(title_id)) {
        entry.name = sibling->name;
      }
    }
  }

  const bool known =
      std::any_of(entry.paths.begin(), entry.paths.end(),
                  [&](const LibraryPath& p) { return SamePath(p.path, path); });
  if (known) {
    return false;
  }

  LibraryPath added;
  added.path = path;
  if (info) {
    added.media_id = info->media_id;
    added.disc_number = info->disc_number;
    if (!existing || existing->paths.empty()) {
      entry.metadata_revision = kLibraryMetadataRevision;
      entry.package_license_mask = info->package_license_mask;
      entry.title_resources_status = info->title_resources_status;
      entry.title_resources = info->title_resources;
    }
    if (entry.content_type == XContentType::kInvalid) {
      entry.content_type = info->content_type;
    }
  }
  entry.paths.push_back(std::move(added));
  if (!Upsert(std::move(entry))) {
    return false;
  }
  std::error_code icon_error;
  if (info && !info->icon.empty() &&
      !std::filesystem::exists(IconPath(key), icon_error) && !icon_error) {
    SetIcon(key, info->icon);
  }
  return true;
}

bool GameLibrary::MarkPlayed(const LibraryKey& key, time_t when) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  if (existing->last_played == when) {
    return true;  // same second, skip the rewrite
  }
  LibraryEntry entry = *existing;
  entry.last_played = when;
  return Upsert(std::move(entry));
}

bool GameLibrary::SetDefaultPath(const LibraryKey& key,
                                 const std::filesystem::path& path) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  const bool present =
      std::any_of(existing->paths.begin(), existing->paths.end(),
                  [&](const LibraryPath& p) { return SamePath(p.path, path); });
  if (!present) {
    return false;
  }
  if (SamePath(existing->default_path().path, path)) {
    return true;  // already default, skip the rewrite
  }

  LibraryEntry entry = *existing;
  for (auto& p : entry.paths) {
    p.is_default = SamePath(p.path, path);
  }
  return Upsert(std::move(entry));
}

bool GameLibrary::RemovePath(const LibraryKey& key,
                             const std::filesystem::path& path) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  auto it = std::find_if(
      existing->paths.begin(), existing->paths.end(),
      [&](const LibraryPath& p) { return SamePath(p.path, path); });
  if (it == existing->paths.end()) {
    return false;
  }
  LibraryEntry entry = *existing;
  entry.paths.erase(entry.paths.begin() +
                    std::distance(existing->paths.begin(), it));
  return Upsert(std::move(entry));  // picks a new default if needed
}

bool GameLibrary::PruneMissingPaths(const LibraryKey& key) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  LibraryEntry entry = *existing;
  const size_t before = entry.paths.size();
  std::error_code ec;
  entry.paths.erase(std::remove_if(entry.paths.begin(), entry.paths.end(),
                                   [&](const LibraryPath& p) {
                                     return !std::filesystem::exists(p.path,
                                                                     ec);
                                   }),
                    entry.paths.end());
  if (entry.paths.size() == before) {
    return false;
  }
  return Upsert(std::move(entry));  // picks a new default if needed
}

bool GameLibrary::SetIcon(const LibraryKey& key, std::span<const uint8_t> png) {
  const auto writer = Writer();
  const auto relative = TitleDir(key.title_id).filename() /
      xe::to_path(VersionToString(key.version)) / "icon.png";
  const auto result = writer->Write(relative, png);
  if (!result) XELOGW("GameLibrary art publication: {}", result.error.message());
  return bool(result);
}

bool GameLibrary::Remove(const LibraryKey& key) {
  const auto* current = Find(key);
  if (!current) return false;
  // Retain exact original paths in the ordinary record, so an unreadable
  // version0 legacy entry cannot reappear under a newly readable version.
  // Only an explicit durable Upsert can replace this same-slot removal state.
  if (!WriteEntryAtomic(*current, EntryDir(key), true)) return false;
  std::erase_if(entries_, [&](const auto& entry) { return entry.key() == key; });
  if (changed_callback_) changed_callback_();
  // Packages, legacy metadata, artwork, stages and unknown/symlink entries
  // remain untouched. The exact old game.toml is in common durable recovery.
  return true;
}

}  // namespace app
}  // namespace xe
