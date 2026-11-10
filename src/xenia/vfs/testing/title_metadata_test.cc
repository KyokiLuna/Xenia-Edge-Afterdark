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
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/aes.h"
#include "third_party/zarchive/include/zarchive/zarchivereader.h"
#include "third_party/zarchive/include/zarchive/zarchivewriter.h"
#include "xenia/app/game_library.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/gdfx_util.h"
#include "xenia/vfs/iso_metadata.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/xex_metadata.h"
#include "xenia/vfs/zar_metadata.h"
#include "xenia/xbox.h"

namespace xe::vfs::test {
namespace {
void BE(std::vector<uint8_t>& bytes, size_t at, uint64_t value,
        size_t size = 4) {
  for (size_t i = 0; i < size; ++i) {
    bytes[at + size - i - 1] = uint8_t(value >> (i * 8));
  }
}

std::vector<uint8_t> Spa() {
  std::vector<std::vector<uint8_t>> entries(4);
  entries[0].resize(44);
  BE(entries[0], 0, 0x58544844);  // XTHD
  BE(entries[0], 4, 1);
  BE(entries[0], 8, 44);
  BE(entries[0], 12, 0x584111F7);
  BE(entries[0], 16, 5);  // App, deliberately sharing a game package type.
  const std::string name = "Embedded title";
  entries[1].resize(18 + name.size());
  BE(entries[1], 0, 0x58535452);  // XSTR
  BE(entries[1], 4, 1);
  BE(entries[1], 8, entries[1].size());
  BE(entries[1], 12, 1, 2);
  BE(entries[1], 14, 0x8000, 2);
  BE(entries[1], 16, name.size(), 2);
  std::copy(name.begin(), name.end(), entries[1].begin() + 18);
  entries[2].resize(16);
  BE(entries[2], 0, 0x58535443);  // XSTC
  BE(entries[2], 4, 1);
  BE(entries[2], 8, 16);
  BE(entries[2], 12, 3);  // German default/fallback.
  entries[3] = {137, 80, 78, 71, 13, 10, 26, 10, 0,  0,   0,
                13,  73, 72, 68, 82, 0,  0,  0,  1,  0,   0,
                0,   1,  8,  6,  0,  0,  0,  31, 21, 196, 137};
  const uint16_t sections[] = {1, 3, 1, 2};
  const uint64_t ids[] = {0x58544844, 3, 0x58535443, 0x8000};
  std::vector<uint8_t> spa(24 + 4 * 18 + 8);
  BE(spa, 0, 0x58444246);
  BE(spa, 4, 0x10000);
  BE(spa, 8, 4);
  BE(spa, 12, 4);
  BE(spa, 16, 1);
  BE(spa, 20, 1);
  size_t offset = 0;
  for (size_t i = 0; i < 4; ++i) {
    BE(spa, 24 + i * 18, sections[i], 2);
    BE(spa, 26 + i * 18, ids[i], 8);
    BE(spa, 34 + i * 18, offset);
    BE(spa, 38 + i * 18, entries[i].size());
    spa.insert(spa.end(), entries[i].begin(), entries[i].end());
    offset += entries[i].size();
  }
  BE(spa, 24 + 4 * 18, offset);
  BE(spa, 28 + 4 * 18, 0xFFFFFFFF);
  return spa;
}

std::vector<uint8_t> Xex(bool encrypted) {
  auto image = Spa();
  image.resize((image.size() + 15) & ~size_t(15));
  std::vector<uint8_t> xex(0x400);
  BE(xex, 0, 0x58455832);
  BE(xex, 8, 0x400);
  BE(xex, 16, 0x100);
  BE(xex, 20, 3);
  BE(xex, 24, XEX_HEADER_FILE_FORMAT_INFO);
  BE(xex, 28, 0x40);
  BE(xex, 32, XEX_HEADER_EXECUTION_INFO);
  BE(xex, 36, 0x60);
  BE(xex, 40, XEX_HEADER_RESOURCE_INFO);
  BE(xex, 44, 0x80);
  BE(xex, 0x40, 8);
  BE(xex, 0x44, encrypted ? 1 : 0, 2);
  BE(xex, 0x60 + 4, 0x100);
  BE(xex, 0x60 + 12, 0x584111F7);
  BE(xex, 0x80, 20);
  std::memcpy(xex.data() + 0x84, "584111F7", 8);
  BE(xex, 0x8C, 0x82000000);
  BE(xex, 0x90, Spa().size());
  BE(xex, 0x100 + 4, image.size());
  BE(xex, 0x100 + 0x110, 0x82000000);
  if (encrypted) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    uint8_t zero[16]{};
    REQUIRE(mbedtls_aes_setkey_enc(&aes, zero, 128) == 0);
    // Devkit master/session keys are zero. Retail is tried first and must not
    // produce a false title summary; the shared reader then retries devkit.
    REQUIRE(mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, zero,
                                  xex.data() + 0x100 + 0x150) == 0);
    uint8_t iv[16]{};
    std::vector<uint8_t> cipher(image.size());
    REQUIRE(mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, image.size(), iv,
                                  image.data(), cipher.data()) == 0);
    image = std::move(cipher);
    mbedtls_aes_free(&aes);
  }
  xex.insert(xex.end(), image.begin(), image.end());
  return xex;
}

void SparseFile(const std::filesystem::path& path,
                std::span<const uint8_t> prefix, size_t size) {
  REQUIRE(size >= prefix.size());
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  file.seekp(size - 1);
  file.put(0);
  REQUIRE(file.good());
}
}  // namespace

TEST_CASE("Offline SPA metadata uses validated shared XDBF tables",
          "[title-metadata]") {
  auto spa = Spa();
  auto metadata = ExtractTitleResourceMetadata(spa);
  REQUIRE(metadata);
  REQUIRE(metadata->title_id == 0x584111F7);
  REQUIRE(metadata->raw_title_type == 5);
  REQUIRE(metadata->Name(2) == "Embedded title");
  REQUIRE(metadata->icon_png.size() == 33);

  SECTION("Entry count and entry ranges are bounded before copying") {
    BE(spa, 12, 0xFFFFFFFF);
    REQUIRE_FALSE(XdbfTableView::Parse(spa));
    REQUIRE_FALSE(ExtractTitleResourceMetadata(spa));
    spa = Spa();
    BE(spa, 34, 0xFFFFFFF0);
    REQUIRE_FALSE(XdbfTableView::Parse(spa));
  }
  SECTION("Nested string lengths are checked inside the entry") {
    BE(spa, 24 + 4 * 18 + 8 + 44 + 16, 0xFFFF, 2);
    REQUIRE_FALSE(ExtractTitleResourceMetadata(spa));
  }
}

TEST_CASE("Executable resources load offline without a profile or guest",
          "[title-metadata]") {
  for (bool encrypted : {false, true}) {
    auto xex = Xex(encrypted);
    auto header = ExtractXexMetadata(xex.data(), xex.size());
    REQUIRE(header);
    REQUIRE(header->title_resources_status ==
            TitleResourcesStatus::kNotRequested);
    auto metadata = ExtractXexMetadata(xex.data(), xex.size(), true);
    REQUIRE(metadata);
    REQUIRE(metadata->title_resources_status == TitleResourcesStatus::kLoaded);
    REQUIRE(metadata->title_resources);
    REQUIRE(metadata->title_resources->Name() == "Embedded title");
    REQUIRE(metadata->title_resources->raw_title_type == 5);
    BE(xex, 0x8C, 0x81FFFFFF);  // Resource precedes the image.
    metadata = ExtractXexMetadata(xex.data(), xex.size(), true);
    REQUIRE(metadata);
    REQUIRE(metadata->title_resources_status == TitleResourcesStatus::kInvalid);
  }
}

TEST_CASE("Offline executable buffers have bounded headers and input members",
          "[title-metadata]") {
  struct Temporary {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("xenia-metadata-bounds-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temporary() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  } temporary;
  REQUIRE(std::filesystem::create_directory(temporary.root));
  auto executable = Xex(true);

  SECTION("Huge trailing encrypted input does not size decoder buffers") {
    // A normal-CBC payload would otherwise allocate decrypted and compressed
    // vectors from the entire member, despite this tiny declared output.
    BE(executable, 0x40, 36);
    BE(executable, 0x46, 2, 2);
    BE(executable, 0x48, 1u << 15);
    BE(executable, 0x4C, 32);
    BE(executable, 0x104, 16);
    const auto path = temporary.root / "large.xex";
    SparseFile(path, executable, kMaxOfflineXexFileBytes + 16);
    const auto metadata = ExtractXexMetadata(path, true);
    REQUIRE(metadata);
    REQUIRE(metadata->title_id == 0x584111F7);
    REQUIRE(metadata->title_resources_status == TitleResourcesStatus::kInvalid);
    REQUIRE_FALSE(metadata->title_resources);
    const auto mapping = MappedMemory::Open(path, MappedMemory::Mode::kRead);
    REQUIRE(mapping);
    REQUIRE_FALSE(ExtractXexTitleResources({mapping->data(), mapping->size()}));
  }
  SECTION("Oversized headers are rejected before their block tables") {
    BE(executable, 8, kMaxOfflineXexHeaderBytes + 1);
    const auto path = temporary.root / "large-header.xex";
    SparseFile(path, executable, kMaxOfflineXexHeaderBytes + 1);
    REQUIRE_FALSE(ExtractXexMetadata(path, false));
    REQUIRE_FALSE(ExtractXexMetadata(path, true));
  }
  SECTION("Declared image allocation is independently bounded") {
    BE(executable, 0x104, kMaxOfflineXexImageBytes + 1);
    const auto metadata =
        ExtractXexMetadata(executable.data(), executable.size(), true);
    REQUIRE(metadata);
    REQUIRE(metadata->title_resources_status == TitleResourcesStatus::kInvalid);
  }
  SECTION("A large ISO member preserves header identity without decoding") {
    std::vector<uint8_t> image(36 * kGdfxSectorSize);
    const size_t volume = 32 * kGdfxSectorSize;
    std::memcpy(image.data() + volume, kGdfxMagic, kGdfxMagicSize);
    auto le = [&](size_t at, uint32_t value) {
      for (int i = 0; i < 4; ++i) {
        image[at + i] = uint8_t(value >> (i * 8));
      }
    };
    le(volume + 20, 33);
    le(volume + 24, kGdfxSectorSize);
    const size_t directory = 33 * kGdfxSectorSize;
    le(directory + 4, 34);
    le(directory + 8, kMaxOfflineXexFileBytes + 16);
    image[directory + 13] = 11;
    std::memcpy(image.data() + directory + 14, "default.xex", 11);
    const size_t offset = 34 * kGdfxSectorSize;
    std::copy(executable.begin(), executable.end(), image.begin() + offset);
    const auto path = temporary.root / "large.iso";
    SparseFile(path, image, offset + kMaxOfflineXexFileBytes + 16);
    const auto metadata = ExtractIsoMetadata(path, true);
    REQUIRE(metadata);
    REQUIRE(metadata->title_id == 0x584111F7);
    REQUIRE(metadata->title_resources_status == TitleResourcesStatus::kInvalid);
    REQUIRE_FALSE(metadata->title_resources);
  }
  SECTION("ZArchive bounds declared header and full member allocations") {
    size_t size = 0;
    bool large_header = false;
    SECTION("Oversized header") {
      large_header = true;
      size = kMaxOfflineXexHeaderBytes + 1;
      BE(executable, 8, size);
    }
    SECTION("Oversized full member") { size = kMaxOfflineXexFileBytes + 1; }
    struct Output {
      std::filesystem::path path;
      std::ofstream stream;
    } output;
    output.path = temporary.root / "large.zar";
    ZArchiveWriter writer(
        [](int32_t part, void* context) {
          // This vendored writer opens one unsplit archive using sentinel-1.
          REQUIRE(part == -1);
          auto& out = *static_cast<Output*>(context);
          REQUIRE_FALSE(out.stream.is_open());
          out.stream.open(out.path, std::ios::binary | std::ios::trunc);
          REQUIRE(out.stream.good());
        },
        [](const void* bytes, size_t count, void* context) {
          auto& out = *static_cast<Output*>(context);
          out.stream.write(static_cast<const char*>(bytes), count);
          REQUIRE(out.stream.good());
        },
        &output);
    REQUIRE(writer.StartNewFile("default.xex"));
    writer.AppendData(executable.data(), executable.size());
    std::array<uint8_t, 64 * 1024> zeros{};
    for (size_t written = executable.size(); written < size;) {
      const size_t count = std::min(zeros.size(), size - written);
      writer.AppendData(zeros.data(), count);
      written += count;
    }
    writer.Finalize();
    output.stream.flush();
    REQUIRE(output.stream.good());
    output.stream.close();
    REQUIRE_FALSE(output.stream.fail());
    // Prove the negative case contains a real complete ZAR member before the
    // metadata bound rejects its declared header or full-member allocation.
    std::unique_ptr<ZArchiveReader> reader(
        ZArchiveReader::OpenFromFile(output.path));
    REQUIRE(reader);
    const auto member = reader->LookUp("default.xex", true, false);
    REQUIRE(member != ZARCHIVE_INVALID_NODE);
    REQUIRE(reader->GetFileSize(member) == size);
    const auto metadata = ExtractZarMetadata(output.path, true);
    if (large_header) {
      REQUIRE_FALSE(metadata);
    } else {
      REQUIRE(metadata);
      REQUIRE(metadata->title_id == 0x584111F7);
      REQUIRE(metadata->title_resources_status ==
              TitleResourcesStatus::kInvalid);
      REQUIRE_FALSE(metadata->title_resources);
    }
  }
}

TEST_CASE("Presentation keeps kind and configured entitlement distinct",
          "[title-metadata]") {
  TitleResourceMetadata resources;
  resources.raw_title_type = 5;
  auto app = DescribeTitlePresentation(uint32_t(XContentType::kXbox360Title),
                                       &resources, 1);
  REQUIRE(app.kind == TitleSemanticKind::kApplication);
  REQUIRE(app.entitlement == TitleEntitlement::kUnknown);
  resources.raw_title_type = 0;
  REQUIRE(DescribeTitlePresentation(uint32_t(XContentType::kXbox360Title),
                                    &resources, 1)
              .kind == TitleSemanticKind::kSystemApplication);
  resources.raw_title_type = 2;
  for (uint32_t mask : {0u, 1u, 0xFFFFFFFFu}) {
    auto arcade = DescribeTitlePresentation(
        uint32_t(XContentType::kArcadeTitle), &resources, mask);
    REQUIRE(arcade.kind == TitleSemanticKind::kGame);
    REQUIRE(arcade.entitlement ==
            (mask & 1 ? TitleEntitlement::kFull : TitleEntitlement::kTrial));
    auto demo = DescribeTitlePresentation(uint32_t(XContentType::kGameDemo),
                                          &resources, mask);
    REQUIRE(demo.entitlement == TitleEntitlement::kTrial);
  }
  REQUIRE(DescribeTitlePresentation(0, nullptr, 1).kind ==
          TitleSemanticKind::kUnknown);
}

TEST_CASE("Container metadata uses the same offline executable resources",
          "[title-metadata]") {
  struct Temporary {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("xenia-title-metadata-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temporary() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  } temporary;
  REQUIRE(std::filesystem::create_directory(temporary.root));
  const auto executable = Xex(false);

  SECTION("GDFX disc") {
    std::vector<uint8_t> image(36 * kGdfxSectorSize);
    const size_t volume = 32 * kGdfxSectorSize;
    std::memcpy(image.data() + volume, kGdfxMagic, kGdfxMagicSize);
    auto le = [&](size_t at, uint32_t value) {
      for (int i = 0; i < 4; ++i) {
        image[at + i] = uint8_t(value >> (i * 8));
      }
    };
    le(volume + 20, 33);
    le(volume + 24, kGdfxSectorSize);
    const size_t directory = 33 * kGdfxSectorSize;
    le(directory + 4, 34);
    le(directory + 8, uint32_t(executable.size()));
    image[directory + 13] = 11;
    std::memcpy(image.data() + directory + 14, "default.xex", 11);
    std::copy(executable.begin(), executable.end(),
              image.begin() + 34 * kGdfxSectorSize);
    const auto metadata = ExtractIsoMetadata(image.data(), image.size(), true);
    REQUIRE(metadata);
    REQUIRE(metadata->title_resources);
    REQUIRE(metadata->title_resources->Name() == "Embedded title");
  }
  SECTION("ZArchive") {
    struct Output {
      std::filesystem::path path;
      std::ofstream stream;
    } output;
    output.path = temporary.root / "title.zar";
    ZArchiveWriter writer(
        [](int32_t part, void* context) {
          // This vendored writer opens one unsplit archive using sentinel-1.
          REQUIRE(part == -1);
          auto& out = *static_cast<Output*>(context);
          REQUIRE_FALSE(out.stream.is_open());
          out.stream.open(out.path, std::ios::binary | std::ios::trunc);
          REQUIRE(out.stream.good());
        },
        [](const void* bytes, size_t size, void* context) {
          auto& out = *static_cast<Output*>(context);
          out.stream.write(static_cast<const char*>(bytes), size);
          REQUIRE(out.stream.good());
        },
        &output);
    REQUIRE(writer.StartNewFile("default.xex"));
    writer.AppendData(executable.data(), executable.size());
    writer.Finalize();
    output.stream.flush();
    REQUIRE(output.stream.good());
    output.stream.close();
    REQUIRE_FALSE(output.stream.fail());
    const auto metadata = ExtractZarMetadata(output.path, true);
    REQUIRE(metadata);
    REQUIRE(metadata->title_resources);
    REQUIRE(metadata->title_resources->raw_title_type == 5);
  }
  SECTION("STFS package retains its physical header identity") {
    constexpr size_t block_size = 0x1000;
    const size_t base =
        xe::round_up(sizeof(XContentContainerHeader), block_size);
    std::vector<uint8_t> package(base + 3 * block_size);
    auto header = std::make_unique<XContentContainerHeader>();
    std::memset(header.get(), 0, sizeof(*header));
    header->content_header.magic = XContentPackageType::kPirs;
    header->content_header.header_size = sizeof(*header);
    header->content_metadata.volume_type = XContentVolumeType::kStfs;
    header->content_metadata.content_type = XContentType::kXbox360Title;
    header->content_metadata.execution_info.title_id = 0x584111F7;
    auto& volume = header->content_metadata.volume_descriptor.stfs;
    volume.descriptor_length = sizeof(StfsVolumeDescriptor);
    volume.flags.bits.read_only_format = 1;
    volume.file_table_block_count = 1;
    volume.set_file_table_block_number(0);
    volume.total_block_count = 2;
    std::memcpy(package.data(), header.get(), sizeof(*header));
    StfsHashTable hash{};
    hash.entries[0].set_level0_next_block(0xFFFFFF);
    std::memcpy(package.data() + base, &hash, sizeof(hash));
    StfsDirectoryBlock directory{};
    auto& entry = directory.entries[0];
    std::memcpy(entry.name, "default.xex", 11);
    entry.flags.name_length = 11;
    entry.flags.contiguous = true;
    entry.set_valid_data_blocks(1);
    entry.set_allocated_data_blocks(1);
    entry.set_start_block_number(1);
    entry.directory_index = 0xFFFF;
    entry.length = uint32_t(executable.size());
    bool oversized = false;
    SECTION("Small executable member") {}
    SECTION("Oversized declared executable member is not buffered") {
      oversized = true;
      entry.length = kMaxOfflineXexFileBytes + 1;
      const uint32_t blocks =
          uint32_t((kMaxOfflineXexFileBytes + block_size) / block_size);
      entry.set_valid_data_blocks(blocks);
      entry.set_allocated_data_blocks(blocks);
    }
    std::memcpy(package.data() + base + block_size, &directory,
                sizeof(directory));
    std::memcpy(package.data() + base + 2 * block_size, executable.data(),
                executable.size());
    const auto path = temporary.root / "package";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(package.data()), package.size());
    const auto metadata = ExtractStfsMetadata(path, XLanguage::kEnglish, true);
    REQUIRE(metadata);
    REQUIRE(metadata->content_type == uint32_t(XContentType::kXbox360Title));
    if (oversized) {
      REQUIRE(metadata->title_resources_status ==
              TitleResourcesStatus::kInvalid);
      REQUIRE_FALSE(metadata->title_resources);
    } else {
      REQUIRE(metadata->title_resources);
      REQUIRE(metadata->title_resources->Name() == "Embedded title");
    }
  }
}

TEST_CASE("Library refresh persists intrinsic metadata without a profile",
          "[title-metadata]") {
  struct Temporary {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("xenia-library-metadata-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temporary() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  } temporary;
  const auto directory = temporary.root / "library/584111F7/0.0.1.0";
  REQUIRE(std::filesystem::create_directories(directory));
  const auto executable = Xex(false);
  const auto path = temporary.root / "default.xex";
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(executable.data()),
             executable.size());
  toml::table old;
  old.insert("title_id", "584111F7");
  old.insert("version", "0.0.1.0");
  old.insert("content_type", "00007000");
  old.insert("name", "My custom name");
  toml::array paths;
  toml::table source;
  source.insert("path", path.generic_string());
  source.insert("default", true);
  paths.push_back(std::move(source));
  old.insert("paths", std::move(paths));
  std::ofstream(directory / "game.toml") << old;

  const app::LibraryKey key{0x584111F7, 0x100};
  app::GameLibrary library(temporary.root / "library");
  library.Load();
  auto missing = library.FindMissingFields();
  REQUIRE(missing.size() == 1);  // Valid old type still needs SPA revision.
  const auto info = app::GameLibrary::ReadMissingFields(missing.front());
  REQUIRE(info);
  REQUIRE(info->title_resources);
  REQUIRE(library.FillMissingFields(key, *info));
  REQUIRE(std::filesystem::exists(library.IconPath(key)));

  app::GameLibrary restarted(temporary.root / "library");
  restarted.Load();
  REQUIRE(restarted.FindMissingFields().empty());
  const auto* entry = restarted.Find(key);
  REQUIRE(entry);
  REQUIRE(entry->name == "My custom name");
  REQUIRE(entry->content_type == XContentType::kXbox360Title);
  REQUIRE(entry->title_resources);
  REQUIRE(entry->title_resources->raw_title_type == 5);
  REQUIRE(entry->title_resources->Name() == "Embedded title");
  REQUIRE(std::filesystem::file_size(restarted.IconPath(key)) == 33);
  REQUIRE_FALSE(std::filesystem::exists(temporary.root / "profiles"));
}
}  // namespace xe::vfs::test
