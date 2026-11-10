/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/app/title_metadata_cache.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/filesystem.h"
#include "xenia/base/math.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/stfs_metadata.h"

namespace xe::app::test {
namespace {
struct CatalogScratch {
  std::filesystem::path root;
  CatalogScratch() {
    std::random_device rng;
    for (unsigned i = 0; i < 16; ++i) {
      auto candidate = std::filesystem::temp_directory_path() /
                       ("xenia_variant_catalog_" + std::to_string(rng()) + "_" +
                        std::to_string(rng()));
      if (std::filesystem::create_directory(candidate)) {
        root = std::move(candidate);
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
  }
  ~CatalogScratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

std::vector<uint8_t> SpaExecutable() {
  auto be = [](std::vector<uint8_t> &bytes, size_t at, uint64_t value,
               size_t count = 4) {
    for (size_t i = 0; i < count; ++i) {
      bytes[at + count - i - 1] = uint8_t(value >> (8 * i));
    }
  };
  const uint8_t png[] = {137, 80, 78, 71, 13, 10, 26, 10, 0,  0,   0,
                         13,  73, 72, 68, 82, 0,  0,  0,  1,  0,   0,
                         0,   1,  8,  6,  0,  0,  0,  31, 21, 196, 137};
  std::vector<uint8_t> spa(24 + 2 * 18 + 44 + sizeof(png));
  be(spa, 0, 0x58444246);
  be(spa, 4, 0x10000);
  be(spa, 8, 2);
  be(spa, 12, 2);
  be(spa, 24, 1, 2);
  be(spa, 26, 0x58544844, 8);
  be(spa, 38, 44);
  be(spa, 42, 2, 2);
  be(spa, 44, 0x8000, 8);
  be(spa, 52, 44);
  be(spa, 56, sizeof(png));
  be(spa, 60, 0x58544844);
  be(spa, 64, 1);
  be(spa, 68, 44);
  be(spa, 72, 0x584111F7);
  be(spa, 76, 5);
  std::memcpy(spa.data() + 104, png, sizeof(png));
  std::vector<uint8_t> xex(0x400);
  be(xex, 0, 0x58455832);
  be(xex, 8, 0x400);
  be(xex, 16, 0x100);
  be(xex, 20, 3);
  be(xex, 24, XEX_HEADER_FILE_FORMAT_INFO);
  be(xex, 28, 0x40);
  be(xex, 32, XEX_HEADER_EXECUTION_INFO);
  be(xex, 36, 0x60);
  be(xex, 40, XEX_HEADER_RESOURCE_INFO);
  be(xex, 44, 0x80);
  be(xex, 0x40, 8);
  be(xex, 0x64, 0x100);
  be(xex, 0x6C, 0x584111F7);
  be(xex, 0x80, 20);
  std::memcpy(xex.data() + 0x84, "584111F7", 8);
  be(xex, 0x8C, 0x82000000);
  be(xex, 0x90, spa.size());
  be(xex, 0x104, spa.size());
  be(xex, 0x210, 0x82000000);
  xex.insert(xex.end(), spa.begin(), spa.end());
  return xex;
}

std::vector<uint8_t> Package(uint32_t mask, uint32_t media = 10,
                             bool has_spa = false) {
  auto header = std::make_unique<vfs::XContentContainerHeader>();
  header->content_header.magic = vfs::XContentPackageType::kPirs;
  header->content_header.header_size = sizeof(*header);
  header->content_header.licenses[0].license_bits = mask;
  auto &metadata = header->content_metadata;
  metadata.volume_type = vfs::XContentVolumeType::kStfs;
  metadata.metadata_version = 2;
  metadata.content_type = XContentType::kArcadeTitle;
  metadata.execution_info.title_id = 0x584111F7;
  metadata.execution_info.version_value = 0x100;
  metadata.execution_info.media_id = media;
  metadata.category = 17; // Arbitrary raw native value, never inferred.
  metadata.set_display_name(XLanguage::kEnglish, u"Physical title");
  metadata.set_title_name(u"Physical title");
  const uint8_t png[] = {137, 80, 78, 71, 13, 10, 26, 10, 0,  0,   0,
                         13,  73, 72, 68, 82, 0,  0,  0,  1,  0,   0,
                         0,   1,  8,  6,  0,  0,  0,  31, 21, 196, 137};
  metadata.title_thumbnail_size = sizeof(png);
  std::memcpy(metadata.title_thumbnail, png, sizeof(png));
  metadata.title_thumbnail[sizeof(png) - 1] = uint8_t(media);
  auto &volume = metadata.volume_descriptor.stfs;
  volume.descriptor_length = sizeof(volume);
  volume.flags.bits.read_only_format = 1;
  volume.file_table_block_count = 1;
  volume.set_file_table_block_number(0);
  volume.total_block_count = has_spa ? 2 : 1;
  const size_t base = xe::round_up(sizeof(*header), size_t(0x1000));
  std::vector<uint8_t> bytes(base + (has_spa ? 0x3000 : 0x2000));
  std::memcpy(bytes.data(), header.get(), sizeof(*header));
  vfs::StfsHashTable hash{};
  hash.entries[0].set_level0_next_block(0xFFFFFF);
  std::memcpy(bytes.data() + base, &hash, sizeof(hash));
  if (has_spa) {
    const auto executable = SpaExecutable();
    vfs::StfsDirectoryBlock directory{};
    auto &entry = directory.entries[0];
    std::memcpy(entry.name, "default.xex", 11);
    entry.flags.name_length = 11;
    entry.flags.contiguous = true;
    entry.set_valid_data_blocks(1);
    entry.set_allocated_data_blocks(1);
    entry.set_start_block_number(1);
    entry.directory_index = 0xFFFF;
    entry.length = uint32_t(executable.size());
    std::memcpy(bytes.data() + base + 0x1000, &directory, sizeof(directory));
    std::memcpy(bytes.data() + base + 0x2000, executable.data(),
                executable.size());
  }
  return bytes;
}

void Write(const std::filesystem::path &path,
           const std::vector<uint8_t> &bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  REQUIRE(file.good());
}
std::string Text(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
LibraryEntry Entry(const std::filesystem::path &a,
                   const std::filesystem::path &b) {
  LibraryEntry entry;
  entry.title_id = 0x584111F7;
  entry.version = 0x100;
  entry.name = "Custom name";
  entry.paths = {{a, true}, {b, false}};
  // Deliberately stale release-level metadata must never bleed into a variant.
  entry.content_type = XContentType::kGameDemo;
  entry.package_license_mask = 0xFFFFFFFF;
  entry.title_resources.emplace();
  entry.title_resources->raw_title_type = 0;
  return entry;
}
} // namespace

TEST_CASE("Physical metadata cache survives restart without mixing variants",
          "[title-catalog]") {
  CatalogScratch scratch;
  const auto a = scratch.root / "trial";
  const auto b = scratch.root / "full";
  const auto cache = scratch.root / "cache";
  Write(a, Package(0, 10));
  Write(b, Package(1, 20));
  const auto first_a = ReadCachedPhysicalTitle(cache, a);
  const auto first_b = ReadCachedPhysicalTitle(cache, b);
  REQUIRE(first_a);
  REQUIRE(first_b);
  REQUIRE_FALSE(first_a->from_cache);
  REQUIRE_FALSE(first_b->from_cache);
  const auto again_a = ReadCachedPhysicalTitle(cache, a);
  const auto again_b = ReadCachedPhysicalTitle(cache, b);
  REQUIRE(again_a);
  REQUIRE(again_b);
  REQUIRE(again_a->from_cache);
  REQUIRE(again_b->from_cache);
  REQUIRE(again_a->info.package_license_mask == 0);
  REQUIRE(again_b->info.package_license_mask == 1);
  REQUIRE(again_a->info.media_id == 10);
  REQUIRE(again_b->info.media_id == 20);
  REQUIRE(again_a->icon_path != again_b->icon_path);
  REQUIRE(xe::filesystem::ReadAllBytes(again_a->icon_path) ==
          again_a->info.icon);
  REQUIRE(xe::filesystem::ReadAllBytes(again_b->icon_path) ==
          again_b->info.icon);

  auto entry = Entry(a, b);
  std::vector<TitleCatalogVariant> variants{
      ResolveCatalogVariant(entry, entry.paths[0], &*again_a, 0),
      ResolveCatalogVariant(entry, entry.paths[1], &*again_b, 0)};
  REQUIRE(variants[0].presentation.kind == vfs::TitleSemanticKind::kGame);
  REQUIRE(variants[0].presentation.entitlement ==
          vfs::TitleEntitlement::kTrial);
  REQUIRE(variants[1].presentation.entitlement == vfs::TitleEntitlement::kFull);
  REQUIRE(variants[0].content_type == XContentType::kArcadeTitle);
  REQUIRE(variants[0].native_category == 17);
  REQUIRE(PreferredCatalogVariants(variants) == std::vector<size_t>{1});
  const auto manifest = again_a->icon_path.parent_path() / "metadata.toml";
  const auto intrinsic = Text(manifest);
  variants[0] = ResolveCatalogVariant(entry, entry.paths[0], &*again_a, 1);
  REQUIRE(variants[0].presentation.entitlement == vfs::TitleEntitlement::kFull);
  REQUIRE(PreferredCatalogVariants(variants) == std::vector<size_t>{0});
  REQUIRE(Text(manifest) == intrinsic);
  REQUIRE(intrinsic.find("effective_license_mask") == std::string::npos);
  REQUIRE(intrinsic.find("family_id") == std::string::npos);
  const auto foreign =
      ResolveCatalogVariant(entry, entry.paths[1], &*again_a, 1);
  REQUIRE_FALSE(foreign.metadata_valid);
  REQUIRE(foreign.icon_path.empty());
  REQUIRE(foreign.presentation.kind == vfs::TitleSemanticKind::kUnknown);
}

TEST_CASE("Persistent variant art preserves distinct package and SPA images",
          "[title-catalog]") {
  CatalogScratch scratch;
  const auto path = scratch.root / "with-spa";
  const auto cache = scratch.root / "cache";
  Write(path, Package(1, 10, true));
  const auto first = ReadCachedPhysicalTitle(cache, path);
  REQUIRE(first);
  REQUIRE(first->info.title_resources);
  REQUIRE(first->info.title_resources->raw_title_type == 5);
  REQUIRE_FALSE(first->info.title_resources->icon_png.empty());
  REQUIRE(first->info.title_resources->icon_png != first->info.icon);
  const auto again = ReadCachedPhysicalTitle(cache, path);
  REQUIRE(again);
  REQUIRE(again->from_cache);
  REQUIRE(again->info.title_resources);
  REQUIRE(again->info.icon == first->info.icon);
  REQUIRE(again->info.title_resources->icon_png ==
          first->info.title_resources->icon_png);
  const auto variant = ResolveCatalogVariant(
      Entry(path, path), LibraryPath{path, true}, &*again, 0);
  REQUIRE(variant.presentation.kind == vfs::TitleSemanticKind::kApplication);
  REQUIRE(variant.presentation.entitlement == vfs::TitleEntitlement::kUnknown);
  REQUIRE(variant.native_category == 17); // Raw field, not derived from SPA5.
}

TEST_CASE("Corrupt or displaced persistent metadata is a cache miss",
          "[title-catalog]") {
  CatalogScratch scratch;
  const auto a = scratch.root / "a";
  const auto b = scratch.root / "b";
  const auto cache = scratch.root / "cache";
  Write(a, Package(0, 10));
  Write(b, Package(1, 20));
  const auto ma = ReadCachedPhysicalTitle(cache, a);
  const auto mb = ReadCachedPhysicalTitle(cache, b);
  REQUIRE(ma);
  REQUIRE(mb);
  const auto a_manifest = ma->icon_path.parent_path() / "metadata.toml";
  const auto b_manifest = mb->icon_path.parent_path() / "metadata.toml";
  SECTION("Wrong source fingerprint cannot cross cache slots") {
    REQUIRE(std::filesystem::copy_file(
        a_manifest, b_manifest,
        std::filesystem::copy_options::overwrite_existing));
    const auto restored = ReadCachedPhysicalTitle(cache, b);
    REQUIRE(restored);
    REQUIRE_FALSE(restored->from_cache);
    REQUIRE(restored->info.media_id == 20);
    REQUIRE(restored->info.package_license_mask == 1);
  }
  SECTION("Corrupt numeric payload cannot replace physical entitlement") {
    auto manifest = toml::parse_file(xe::path_to_utf8(a_manifest));
    manifest["metadata"].as_table()->insert_or_assign("package_license_mask",
                                                      1);
    std::ofstream(a_manifest) << manifest;
    const auto restored = ReadCachedPhysicalTitle(cache, a);
    REQUIRE(restored);
    REQUIRE_FALSE(restored->from_cache);
    REQUIRE(restored->info.package_license_mask == 0);
  }
  SECTION("Truncated cached art is not published") {
    std::ofstream(ma->icon_path) << "broken";
    const auto restored = ReadCachedPhysicalTitle(cache, a);
    REQUIRE(restored);
    REQUIRE_FALSE(restored->from_cache);
    REQUIRE(xe::filesystem::ReadAllBytes(restored->icon_path) ==
            restored->info.icon);
  }
}

TEST_CASE("Fingerprints invalidate changed same-size input and companion files",
          "[title-catalog]") {
  CatalogScratch scratch;
  const auto source = scratch.root / "package";
  const auto cache = scratch.root / "cache";
  Write(source, Package(0));
  const auto first = ReadCachedPhysicalTitle(cache, source);
  REQUIRE(first);
  const auto old_time = std::filesystem::last_write_time(source);
  Write(source, Package(1));
  std::filesystem::last_write_time(source, old_time);
  const auto changed = ReadCachedPhysicalTitle(cache, source);
  REQUIRE(changed);
  REQUIRE_FALSE(changed->from_cache);
  REQUIRE(changed->fingerprint.value != first->fingerprint.value);
  REQUIRE(changed->info.package_license_mask == 1);
  REQUIRE(changed->icon_path != first->icon_path);
  Write(source, Package(3));
  std::filesystem::last_write_time(source, old_time);
  REQUIRE(ReadCachedPhysicalTitle(cache, source));
  size_t icons = 0;
  for (const auto &file :
       std::filesystem::directory_iterator(first->icon_path.parent_path())) {
    icons += file.path().extension() == ".png";
  }
  REQUIRE(icons <= 2);
  const auto data = scratch.root / "package.data";
  REQUIRE(std::filesystem::create_directory(data));
  const auto fragment = data / "Data0000";
  Write(fragment, {1, 2, 3});
  const auto with_data = ReadCachedPhysicalTitle(cache, source);
  REQUIRE(with_data);
  REQUIRE(with_data->fingerprint.files.size() == 2);
  const auto fragment_time = std::filesystem::last_write_time(fragment);
  Write(fragment, {3, 2, 1});
  std::filesystem::last_write_time(fragment, fragment_time);
  const auto changed_data = ReadCachedPhysicalTitle(cache, source);
  REQUIRE(changed_data);
  REQUIRE_FALSE(changed_data->from_cache);
  REQUIRE(changed_data->fingerprint.value != with_data->fingerprint.value);
  REQUIRE(std::filesystem::remove(source));
  REQUIRE_FALSE(ReadCachedPhysicalTitle(cache, source));
}

TEST_CASE("A changed symlink target cannot reuse another physical variant",
          "[title-catalog]") {
  CatalogScratch scratch;
  const auto a = scratch.root / "a", b = scratch.root / "b";
  const auto alias = scratch.root / "alias";
  Write(a, Package(0, 10));
  Write(b, Package(1, 20));
  std::error_code ec;
  std::filesystem::create_symlink(a, alias, ec);
  if (ec) {
    WARN("Host does not permit symlink fixtures");
    return;
  }
  const auto first = ReadCachedPhysicalTitle(scratch.root / "cache", alias);
  REQUIRE(first);
  REQUIRE(std::filesystem::remove(alias));
  std::filesystem::create_symlink(b, alias);
  const auto second = ReadCachedPhysicalTitle(scratch.root / "cache", alias);
  REQUIRE(second);
  REQUIRE_FALSE(second->from_cache);
  REQUIRE(second->info.media_id == 20);
  REQUIRE(second->fingerprint.files[0].physical_path ==
          std::filesystem::canonical(b));
}

TEST_CASE("Native change identity invalidates edits beyond sampled headers",
          "[title-catalog]") {
  CatalogScratch scratch;
  auto executable = SpaExecutable();
  const std::vector<uint8_t> spa(executable.begin() + 0x400, executable.end());
  executable.resize(0x20000);
  executable[8] = 0;
  executable[9] = 2;
  executable[10] = executable[11] = 0;
  executable.insert(executable.end(), spa.begin(), spa.end());
  const auto source = scratch.root / "large-header.xex";
  const auto cache = scratch.root / "cache";
  Write(source, executable);
  const auto first = ReadCachedPhysicalTitle(cache, source);
  REQUIRE(first);
  REQUIRE(first->info.title_resources);
  REQUIRE(first->info.title_resources->raw_title_type == 5);
  if (!first->fingerprint.files[0].native_identity) {
    SUCCEED("Portable cache sampling does not claim full-byte identity");
    return;
  }
  const auto old_time = std::filesystem::last_write_time(source);
  executable[0x20000 + 79] = 1; // SPA TitleType beyond first64KiB.
  bool replaced = false;
  SECTION("In-place edit with restored modification time") {
    Write(source, executable);
    std::filesystem::last_write_time(source, old_time);
  }
  SECTION(
      "Replacement with matching size, sampled header and modification time") {
    replaced = true;
    const auto replacement = scratch.root / "replacement";
    Write(replacement, executable);
    std::filesystem::last_write_time(replacement, old_time);
    std::filesystem::rename(replacement, source);
  }
  const auto second = ReadCachedPhysicalTitle(cache, source);
  REQUIRE(second);
  REQUIRE_FALSE(second->from_cache);
  REQUIRE(second->info.title_resources);
  REQUIRE(second->info.title_resources->raw_title_type == 1);
  const auto &a = first->fingerprint.files[0];
  const auto &b = second->fingerprint.files[0];
  REQUIRE(a.size == b.size);
  REQUIRE(a.mtime == b.mtime);
  REQUIRE(a.header_hash == b.header_hash);
  REQUIRE_FALSE(a == b);
  if (replaced) {
    REQUIRE(a.inode != b.inode);
  }
  const auto old = ResolveCatalogVariant(Entry(source, source),
                                         LibraryPath{source, true}, &*first, 0);
  REQUIRE_FALSE(CatalogVariantMatchesPhysicalFile(old, source));
}

TEST_CASE("Only verified families group distinct title identities",
          "[title-catalog]") {
  TitleCatalogVariant trial, full;
  trial.title_id = 0x584111F7;
  full.title_id = 0x4D530A81;
  trial.name = full.name = "same display name";
  trial.path = "digital";
  full.path = "disc";
  trial.presentation.kind = full.presentation.kind =
      vfs::TitleSemanticKind::kGame;
  trial.available = full.available = true;
  trial.metadata_valid = full.metadata_valid = true;
  trial.presentation.entitlement = vfs::TitleEntitlement::kTrial;
  full.presentation.entitlement = vfs::TitleEntitlement::kFull;
  trial.family_id = full.family_id = 0x4D530A81;
  std::vector<TitleCatalogVariant> variants{trial, full};
  REQUIRE(PreferredCatalogVariants(variants).size() == 2);
  variants[0].verified_family = variants[1].verified_family = true;
  REQUIRE(PreferredCatalogVariants(variants) == std::vector<size_t>{1});
  REQUIRE(variants[1].title_id == 0x4D530A81);
  REQUIRE(variants[1].path == "disc");
  variants[0].title_id = variants[1].title_id = 0x584E07D2;
  REQUIRE(PreferredCatalogVariants(variants).size() == 2);
  variants[0].title_id = variants[1].title_id = 0;
  REQUIRE(PreferredCatalogVariants(variants).size() == 2);
}

TEST_CASE("Native catalog names round-trip exact variants within the ABI field",
          "[title-catalog]") {
  TitleCatalogVariant variant;
  variant.title_id = 0x584111F7;
  variant.version = 0xFFFFFFFF;
  variant.variant_id = 0xFEDCBA9876543210;
  const auto name = CatalogVariantFileName(variant);
  REQUIRE(name.size() == 36);
  REQUIRE(name.size() <= 42);
  const auto parsed = ParseCatalogVariantFileName(name);
  REQUIRE(parsed);
  REQUIRE(parsed->title_id == variant.title_id);
  REQUIRE(parsed->version == variant.version);
  REQUIRE(parsed->variant_id == variant.variant_id);
  const auto legacy = ParseCatalogVariantFileName(name.substr(0, 20));
  REQUIRE(legacy);
  REQUIRE_FALSE(legacy->variant_id);
  auto collision = variant;
  collision.path = "another-file";
  std::vector<TitleCatalogVariant> variants{variant, collision};
  REQUIRE_FALSE(UniqueCatalogVariantIndex(variants, *parsed));
  variants[1].variant_id ^= 1;
  REQUIRE(UniqueCatalogVariantIndex(variants, *parsed) == 0);
  REQUIRE_FALSE(UniqueCatalogVariantIndex(variants, *legacy));
  REQUIRE_FALSE(ParseCatalogVariantFileName(""));
  REQUIRE_FALSE(ParseCatalogVariantFileName(name + "extra"));
  auto bad = name;
  bad.back() = 'G';
  REQUIRE_FALSE(ParseCatalogVariantFileName(bad));
}

TEST_CASE("STFS display reads bound strings and preserve raw numeric metadata",
          "[title-catalog]") {
  CatalogScratch scratch;
  auto bytes = Package(1);
  auto *header = reinterpret_cast<vfs::XContentContainerHeader *>(bytes.data());
  auto &display = header->content_metadata;
  for (auto &name : display.display_name_raw.uint) {
    std::fill(std::begin(name), std::end(name), uint16_t('d'));
  }
  display.description_raw.uint[0][0] = 0xD800;
  for (auto &value : display.title_name_raw.uint) {
    value = uint16_t('t');
  }
  const auto path = scratch.root / "unterminated";
  Write(path, bytes);
  const auto metadata =
      vfs::ExtractStfsMetadata(path, XLanguage::kEnglish, false);
  REQUIRE(metadata);
  REQUIRE(metadata->display_name == std::string(127, 'd'));
  REQUIRE(metadata->title_name == std::string(63, 't'));
  REQUIRE(metadata->description == "\xEF\xBF\xBD");
  REQUIRE(metadata->title_id == 0x584111F7);
  REQUIRE(metadata->version == 0x100);
  REQUIRE(metadata->native_category == 17);
  REQUIRE(metadata->package_license_mask == 1);
  REQUIRE(xe::filesystem::ReadAllBytes(path) == bytes);
}

TEST_CASE("Native package category identifies installed apps without SPA",
          "[title-catalog]") {
  uint32_t title_id = 0, media_id = 0;
  SECTION("Twitch package identity") {
    title_id = 0x334A07D1;
    media_id = 0x176CD276;
  }
  SECTION("YouTube package identity") {
    title_id = 0x423607D1;
    media_id = 0x1FDD7045;
  }
  CatalogScratch scratch;
  const auto path = scratch.root / "content";
  auto bytes = Package(0, media_id);
  auto* header = reinterpret_cast<vfs::XContentContainerHeader*>(bytes.data());
  header->content_metadata.content_type = XContentType::kXbox360Title;
  header->content_metadata.category = 5;
  header->content_metadata.execution_info.title_id = title_id;
  Write(path, bytes);
  const auto metadata = ReadCachedPhysicalTitle(scratch.root / "cache", path);
  REQUIRE(metadata);
  REQUIRE(metadata->info.title_resources_status ==
          vfs::TitleResourcesStatus::kAbsent);
  REQUIRE_FALSE(metadata->info.title_resources);
  auto entry = Entry(path, path);
  entry.title_id = title_id;
  entry.name = "Unrelated custom display name";
  entry.content_type =
      XContentType::kGameDemo;  // Stale UI metadata is not input.
  const auto resolved =
      ResolveCatalogVariant(entry, entry.paths[0], &*metadata, 1);
  REQUIRE(resolved.metadata_valid);
  CHECK(resolved.presentation.kind == vfs::TitleSemanticKind::kApplication);
  CHECK(resolved.presentation.entitlement == vfs::TitleEntitlement::kUnknown);
  CHECK(resolved.presentation.effective_license_mask == 1);
  CHECK(resolved.native_category == 5);
  CHECK(resolved.content_type == XContentType::kXbox360Title);
  CHECK(resolved.title_id == title_id);
  CHECK(resolved.media_id == media_id);
  CHECK(resolved.version == 0x100);
  CHECK(resolved.path == path);
  CHECK(resolved.name == entry.name);
  CHECK_FALSE(resolved.icon_path.empty());
  CHECK(xe::filesystem::ReadAllBytes(resolved.icon_path) ==
        metadata->info.icon);
  const auto cached = ReadCachedPhysicalTitle(scratch.root / "cache", path);
  REQUIRE(cached);
  REQUIRE(cached->from_cache);
  CHECK(ResolveCatalogVariant(entry, entry.paths[0], &*cached, 0)
            .presentation.kind == vfs::TitleSemanticKind::kApplication);
  CHECK(xe::filesystem::ReadAllBytes(path) == bytes);
}

TEST_CASE("Native app category is scoped to its physical type and authority",
          "[title-catalog]") {
  XContentType physical_type = XContentType::kXbox360Title;
  uint32_t category = 0;
  bool invalid_spa = false;
  auto expected = vfs::TitleSemanticKind::kGame;
  SECTION("zero category stays a game") {}
  SECTION("nonzero is a predicate not an invented category enum") {
    category = 1;
    expected = vfs::TitleSemanticKind::kApplication;
  }
  SECTION("installed disc 4000 is distinct from package 7000") {
    physical_type = XContentType::kInstalledGame;
    category = 5;
  }
  SECTION("Arcade retains its trial full license semantics") {
    physical_type = XContentType::kArcadeTitle;
    category = 5;
  }
  SECTION("real category survives failed optional SPA decode") {
    category = 5;
    invalid_spa = true;
    expected = vfs::TitleSemanticKind::kApplication;
  }
  SECTION("invalid SPA without app category remains unknown") {
    invalid_spa = true;
    expected = vfs::TitleSemanticKind::kUnknown;
  }
  CatalogScratch scratch;
  const auto path = scratch.root / "content";
  auto bytes = Package(0, 10, invalid_spa);
  auto* header = reinterpret_cast<vfs::XContentContainerHeader*>(bytes.data());
  header->content_metadata.content_type = physical_type;
  header->content_metadata.category = category;
  if (invalid_spa) {
    const auto base = xe::round_up(sizeof(*header), size_t(0x1000));
    bytes[base + 0x2000] =
        0;  // Actual malformed nested executable, not a mock.
  }
  Write(path, bytes);
  const auto metadata = ReadCachedPhysicalTitle(scratch.root / "cache", path);
  REQUIRE(metadata);
  CHECK(metadata->info.title_resources_status ==
        (invalid_spa ? vfs::TitleResourcesStatus::kInvalid
                     : vfs::TitleResourcesStatus::kAbsent));
  const auto entry = Entry(path, path);
  const auto trial =
      ResolveCatalogVariant(entry, entry.paths[0], &*metadata, 0);
  const auto full = ResolveCatalogVariant(entry, entry.paths[0], &*metadata, 1);
  REQUIRE(trial.metadata_valid);
  CHECK(trial.presentation.kind == expected);
  CHECK(full.presentation.kind == expected);
  CHECK(trial.native_category == category);
  CHECK(full.content_type == physical_type);
  CHECK(full.title_id == entry.title_id);
  CHECK(full.media_id == 10);
  CHECK(full.path == path);
  if (physical_type == XContentType::kArcadeTitle) {
    CHECK(trial.presentation.entitlement == vfs::TitleEntitlement::kTrial);
    CHECK(full.presentation.entitlement == vfs::TitleEntitlement::kFull);
  } else if (expected == vfs::TitleSemanticKind::kApplication) {
    CHECK(full.presentation.entitlement == vfs::TitleEntitlement::kUnknown);
  }
  CHECK(xe::filesystem::ReadAllBytes(path) == bytes);
}

} // namespace xe::app::test
