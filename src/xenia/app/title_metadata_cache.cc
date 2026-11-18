/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/app/title_metadata_cache.h"
#include "xenia/base/host_metadata_writer.h"
#include "xenia/base/platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/xxhash.h"

namespace xe::app {
namespace {
constexpr uint32_t kPhysicalMetadataRevision = 1;
constexpr uint64_t kMaxCacheMetadataBytes = 2 * 1024 * 1024;
constexpr uint64_t kMaxCacheIconBytes = 4 * 1024 * 1024;

uint64_t Hash(std::string_view text) {
  return XXH3_64bits(text.data(), text.size());
}
std::string Hex(uint64_t value) { return fmt::format("{:016X}", value); }
std::string Dump(const toml::table &value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}
std::filesystem::path Absolute(const std::filesystem::path &path) {
  std::error_code ec;
  const auto result = std::filesystem::absolute(path, ec);
  return ec ? path : result.lexically_normal();
}

toml::table EncodeFingerprint(const PhysicalTitleFingerprint &fingerprint) {
  toml::table result;
  result.insert("source", fingerprint.source_path.generic_string());
  result.insert("value", Hex(fingerprint.value));
  toml::array files;
  for (const auto &stamp : fingerprint.files) {
    toml::table file;
    file.insert("path", stamp.physical_path.generic_string());
    file.insert("source", stamp.source_path.generic_string());
    file.insert("size", Hex(stamp.size));
    file.insert("mtime", stamp.mtime);
    file.insert("header", Hex(stamp.header_hash));
    file.insert("native_identity", stamp.native_identity);
    file.insert("device", Hex(stamp.device));
    file.insert("inode", Hex(stamp.inode));
    file.insert("ctime_seconds", stamp.change_seconds);
    file.insert("ctime_nanoseconds", stamp.change_nanoseconds);
    files.push_back(std::move(file));
  }
  result.insert("files", std::move(files));
  return result;
}

toml::table EncodeInfo(const GameFileInfo &info) {
  toml::table result;
  result.insert("title_id", info.title_id);
  result.insert("version", info.version);
  result.insert("media_id", info.media_id);
  result.insert("disc_number", uint32_t(info.disc_number));
  result.insert("disc_count", uint32_t(info.disc_count));
  result.insert("content_type", uint32_t(info.content_type));
  result.insert("package_license_mask", info.package_license_mask);
  result.insert("title_name", info.title_name);
  result.insert("display_name", info.display_name);
  result.insert("resource_status", uint32_t(info.title_resources_status));
  if (info.native_category) {
    result.insert("native_category", *info.native_category);
  }
  if (info.title_resources) {
    auto resources = info.title_resources->ToToml();
    // Art belongs to this physical variant's immutable image file, not TOML.
    resources.erase("icon_bytes");
    result.insert("resources", std::move(resources));
  }
  return result;
}

std::optional<GameFileInfo> DecodeInfo(const toml::table &value) {
  const auto title_id = value["title_id"].value<uint32_t>();
  const auto version = value["version"].value<uint32_t>();
  const auto media_id = value["media_id"].value<uint32_t>();
  const auto type = value["content_type"].value<uint32_t>();
  const auto mask = value["package_license_mask"].value<uint32_t>();
  const auto status = value["resource_status"].value<uint32_t>();
  const auto disc_number = value["disc_number"].value<uint8_t>();
  const auto disc_count = value["disc_count"].value<uint8_t>();
  const auto title_name = value["title_name"].value<std::string>();
  const auto display_name = value["display_name"].value<std::string>();
  if (!title_id || !version || !media_id || !type || !mask || !status ||
      !disc_number || !disc_count || !title_name || !display_name ||
      *status > uint32_t(vfs::TitleResourcesStatus::kInvalid)) {
    return std::nullopt;
  }
  GameFileInfo info;
  info.title_id = *title_id;
  info.version = *version;
  info.media_id = *media_id;
  info.content_type = XContentType(*type);
  info.package_license_mask = *mask;
  info.disc_number = *disc_number;
  info.disc_count = *disc_count;
  info.title_name = *title_name;
  info.display_name = *display_name;
  info.title_resources_status = vfs::TitleResourcesStatus(*status);
  if (value.contains("native_category")) {
    info.native_category = value["native_category"].value<uint32_t>();
    if (!info.native_category) {
      return std::nullopt;
    }
  }
  if (const auto *resources = value["resources"].as_table()) {
    const auto spa_title = (*resources)["title_id"].value<uint32_t>();
    const auto raw_type = (*resources)["raw_title_type"].value<uint32_t>();
    const auto flags = (*resources)["flags"].value<uint32_t>();
    const auto language = (*resources)["default_language"].value<uint32_t>();
    const auto *names = (*resources)["names"].as_table();
    if (!spa_title || !raw_type || !flags || !language || !names) {
      return std::nullopt;
    }
    vfs::TitleResourceMetadata spa;
    spa.title_id = *spa_title;
    spa.raw_title_type = *raw_type;
    spa.flags = *flags;
    spa.default_language = *language;
    for (const auto &[key, node] : *names) {
      uint32_t id = 0;
      const auto text = key.str();
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), id);
      const auto name = node.value<std::string>();
      if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
          !id || id >= uint32_t(XLanguage::kMaxLanguages) || !name ||
          name->size() > 65535 || name->find('\0') != std::string::npos) {
        return std::nullopt;
      }
      spa.names.emplace(id, *name);
    }
    info.title_resources = std::move(spa);
  }
  if ((info.title_resources_status == vfs::TitleResourcesStatus::kLoaded) !=
      info.title_resources.has_value()) {
    return std::nullopt;
  }
  return info;
}

std::optional<std::vector<uint8_t>>
BoundedBytes(const std::shared_ptr<filesystem::HostMetadataWriter>& writer,
             const std::filesystem::path &path, uint64_t limit) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
  if (error) return std::nullopt;
  const auto token = writer->Capture(absolute.lexically_relative(writer->path()), limit, error);
  if (token && !error && token->exists())
    return std::vector<uint8_t>(token->bytes().begin(), token->bytes().end());
#if XE_PLATFORM_WIN32
  // Existing cache reads stay available; no raw Windows publication fallback.
  if (error != std::errc::operation_not_supported) return std::nullopt;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > limit) return std::nullopt;
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
    return std::nullopt;
  return bytes;
#else
  return std::nullopt;
#endif
}
} // namespace

std::optional<PhysicalTitleFingerprint>
FingerprintPhysicalTitle(const std::filesystem::path &path) {
  const auto files = InspectCatalogFiles(path);
  if (!files || files->empty()) {
    return std::nullopt;
  }
  PhysicalTitleFingerprint result;
  result.source_path = files->front().source_path;
  result.files = *files;
  result.value = Hash(Dump(EncodeFingerprint(result)));
  return result;
}

std::optional<PhysicalTitleMetadata>
ReadCachedPhysicalTitle(const std::filesystem::path &cache_root,
                        const std::filesystem::path &path,
                        std::shared_ptr<filesystem::HostMetadataWriter> writer) {
  const auto before = FingerprintPhysicalTitle(path);
  if (!before) {
    return std::nullopt;
  }
  std::error_code writer_error;
  if (!writer) writer = filesystem::HostMetadataWriter::Open(cache_root, writer_error);
  const auto expected_root = std::filesystem::absolute(cache_root, writer_error).lexically_normal();
  if (writer_error || writer->path() != expected_root) return std::nullopt;
  const auto folder =
      cache_root / Hex(Hash(before->source_path.generic_string()));
  const auto manifest_path = folder / "metadata.toml";
  const auto icon_path = folder / (Hex(before->value) + ".png");
  const auto manifest_bytes = BoundedBytes(writer, manifest_path, kMaxCacheMetadataBytes);
  if (manifest_bytes) {
    try {
      const auto manifest = toml::parse(std::string_view(
          reinterpret_cast<const char*>(manifest_bytes->data()), manifest_bytes->size()));
      const auto seal = manifest["record_hash"].value<std::string>();
      auto unsealed = manifest;
      unsealed.erase("record_hash");
      const auto *fingerprint = manifest["fingerprint"].as_table();
      const auto *payload = manifest["metadata"].as_table();
      const auto payload_hash = manifest["metadata_hash"].value<std::string>();
      if (seal && *seal == Hex(Hash(Dump(unsealed))) &&
          manifest["revision"].value_or(uint32_t(0)) ==
              kPhysicalMetadataRevision &&
          fingerprint &&
          Dump(*fingerprint) == Dump(EncodeFingerprint(*before)) && payload &&
          payload_hash && *payload_hash == Hex(Hash(Dump(*payload)))) {
        if (auto info = DecodeInfo(*payload)) {
          const auto icon_hash = manifest["icon_hash"].value<std::string>();
          const bool has_icon = manifest["has_icon"].value_or(false);
          if (has_icon) {
            const auto bytes = BoundedBytes(writer, icon_path, kMaxCacheIconBytes);
            if (!bytes || !icon_hash ||
                *icon_hash != Hex(XXH3_64bits(bytes->data(), bytes->size()))) {
              info.reset();
            } else {
              info->icon = *bytes;
            }
          }
          if (info && manifest["has_spa_icon"].value_or(false)) {
            const bool shared_icon = manifest["spa_uses_icon"].value_or(false);
            const auto spa_path =
                shared_icon ? icon_path
                            : folder / (Hex(before->value) + ".spa.png");
            const auto bytes = BoundedBytes(writer, spa_path, kMaxCacheIconBytes);
            const auto hash = manifest["spa_icon_hash"].value<std::string>();
            if (!info->title_resources || !bytes || !hash ||
                *hash != Hex(XXH3_64bits(bytes->data(), bytes->size()))) {
              info.reset();
            } else {
              info->title_resources->icon_png = *bytes;
            }
          }
          if (info) {
            // A cheap header read also keeps numeric physical identity and
            // package entitlement authoritative over editable cache data.
            const auto identity = ReadGameFileInfo(path, false);
            if (!identity || identity->title_id != info->title_id ||
                identity->version != info->version ||
                identity->media_id != info->media_id ||
                identity->disc_number != info->disc_number ||
                identity->disc_count != info->disc_count ||
                identity->content_type != info->content_type ||
                identity->package_license_mask != info->package_license_mask ||
                identity->native_category != info->native_category) {
              info.reset();
            }
          }
          if (info) {
            const auto after = FingerprintPhysicalTitle(path);
            if (after && *after == *before) {
              return PhysicalTitleMetadata{
                  *before, std::move(*info),
                  has_icon ? icon_path : std::filesystem::path{}, true, writer};
            }
          }
        }
      }
    } catch (const toml::parse_error &) {
      // A corrupt/displaced cache entry is a miss, never title identity.
    }
  }
  auto info = ReadGameFileInfo(path);
  const auto after = FingerprintPhysicalTitle(path);
  if (!info || !after || *after != *before) {
    return std::nullopt;
  }
  PhysicalTitleMetadata result{*after, std::move(*info), {}, false, writer};
  const auto payload = EncodeInfo(result.info);
  toml::table manifest;
  manifest.insert("revision", kPhysicalMetadataRevision);
  manifest.insert("fingerprint", EncodeFingerprint(*after));
  manifest.insert("metadata", payload);
  manifest.insert("metadata_hash", Hex(Hash(Dump(payload))));
  manifest.insert("has_icon", !result.info.icon.empty());
  if (!result.info.icon.empty()) {
    manifest.insert("icon_hash", Hex(XXH3_64bits(result.info.icon.data(),
                                                 result.info.icon.size())));
  }
  const std::span<const uint8_t> spa_icon =
      result.info.title_resources
          ? std::span<const uint8_t>(result.info.title_resources->icon_png)
          : std::span<const uint8_t>{};
  const bool shared_icon =
      !spa_icon.empty() &&
      std::equal(spa_icon.begin(), spa_icon.end(), result.info.icon.begin(),
                 result.info.icon.end());
  manifest.insert("has_spa_icon", !spa_icon.empty());
  manifest.insert("spa_uses_icon", shared_icon);
  if (!spa_icon.empty()) {
    manifest.insert("spa_icon_hash",
                    Hex(XXH3_64bits(spa_icon.data(), spa_icon.size())));
  }
  manifest.insert("record_hash", Hex(Hash(Dump(manifest))));
  const auto relative = folder.lexically_relative(cache_root);
  if (result.info.icon.size() <= kMaxCacheIconBytes &&
      spa_icon.size() <= kMaxCacheIconBytes &&
      (result.info.icon.empty() || writer->Write(relative / icon_path.filename(), result.info.icon)) &&
      (spa_icon.empty() || shared_icon ||
       writer->Write(relative / (Hex(after->value) + ".spa.png"), spa_icon))) {
    const auto text = Dump(manifest);
    if (text.size() <= kMaxCacheMetadataBytes &&
        writer->Write(relative / "metadata.toml",
            {reinterpret_cast<const uint8_t*>(text.data()), text.size()})) {
      result.icon_path = result.info.icon.empty() ? std::filesystem::path{} : icon_path;
      // Prior named files lack an exact retirement/ownership receipt after
      // restart. Keep them; never delete unknown artwork by a hex-name scan.
      // The same shared20GiB admission floor bounds future cache growth.
    }
  }
  return result;
}

TitleCatalogVariant ResolveCatalogVariant(const LibraryEntry &entry,
                                          const LibraryPath &path,
                                          const PhysicalTitleMetadata *metadata,
                                          uint32_t effective_config_mask,
                                          uint32_t verified_family_id) {
  TitleCatalogVariant result;
  result.title_id = entry.title_id;
  result.version = entry.version;
  result.name = entry.name;
  result.path = path.path;
  result.is_default = path.is_default;
  result.variant_id = Hash(Absolute(path.path).generic_string());
  result.family_id = verified_family_id ? verified_family_id : entry.title_id;
  result.verified_family =
      verified_family_id && !IsSharedXbligHost(entry.title_id);
  if (!metadata || metadata->fingerprint.source_path != Absolute(path.path) ||
      metadata->fingerprint.files.empty() ||
      metadata->info.title_id != entry.title_id ||
      metadata->info.version != entry.version) {
    return result;
  }
  const auto &info = metadata->info;
  const auto &physical = metadata->fingerprint.files.front();
  result.metadata_valid = result.available = true;
  result.physical_path = physical.physical_path;
  result.physical_size = physical.size;
  result.physical_mtime = physical.mtime;
  result.physical_files = metadata->fingerprint.files;
  result.variant_id = metadata->fingerprint.value;
  result.icon_path = metadata->icon_path;
  result.content_type = info.content_type;
  result.media_id = info.media_id;
  result.package_license_mask = info.package_license_mask;
  result.native_category = info.native_category;
  result.resources_status = info.title_resources_status;
  result.presentation = vfs::DescribeTitlePresentation(
      uint32_t(info.content_type),
      info.title_resources ? &*info.title_resources : nullptr,
      info.package_license_mask | effective_config_mask);
  // 17559 Marketplace.GameLibrary.ConvertToLibraryItem uses the physical
  // 0x7000 type with nonzero Category for its app presentation. This is the
  // actual STFS field; it is not a cast of SPA TitleType or a name heuristic.
  const bool native_application =
      info.content_type == XContentType::kXbox360Title &&
      info.native_category && *info.native_category != 0;
  if (native_application) {
    result.presentation.kind = vfs::TitleSemanticKind::kApplication;
    result.presentation.entitlement = vfs::TitleEntitlement::kUnknown;
  } else if (info.title_resources_status ==
             vfs::TitleResourcesStatus::kInvalid) {
    // Apps share game package types. A failed resource read cannot establish
    // their semantic kind; expose Unknown until the shared blocker is fixed.
    result.presentation.kind = vfs::TitleSemanticKind::kUnknown;
  }
  if (result.name.empty()) {
    result.name = info.title_name.empty() ? info.display_name : info.title_name;
  }
  return result;
}
} // namespace xe::app
