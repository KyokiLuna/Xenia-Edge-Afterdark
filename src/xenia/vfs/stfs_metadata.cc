/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/stfs_metadata.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "xenia/base/mutex.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/base/string.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/xex_metadata.h"

namespace xe {
namespace vfs {

namespace {

std::string U16ToUtf8(const std::u16string& s) {
  if (s.empty()) {
    return {};
  }
  return xe::to_utf8_lossy(s);
}

}  // namespace

namespace {
constexpr uint32_t kSnapshotBlockBytes = 0x1000;
constexpr uint32_t kSnapshotEnd = 0xFFFFFF;
constexpr std::array<uint32_t, 3> kSnapshotCoverage{170, 28900, 4913000};
constexpr uint32_t kSnapshotCopies = 2;
uint64_t SnapshotDataBlock(uint32_t index) {
  uint64_t block = index;
  for (const auto coverage : kSnapshotCoverage) {
    block += ((uint64_t(index) + coverage) / coverage) * kSnapshotCopies;
    if (index < coverage) break;
  }
  return block;
}
uint64_t SnapshotHashBlock(uint32_t index, uint32_t level) {
  constexpr uint32_t step0 = 170 + kSnapshotCopies;
  constexpr uint32_t step1 = 28900 + 171 * kSnapshotCopies;
  if (level == 2) return step1;
  if (index < kSnapshotCoverage[level]) return level == 0 ? 0 : step0;
  uint64_t block =
      uint64_t(index / kSnapshotCoverage[level]) * (level == 0 ? step0 : step1);
  if (level == 0) {
    block += uint64_t(index / kSnapshotCoverage[1] + 1) * kSnapshotCopies;
    if (index < kSnapshotCoverage[1]) return block;
  }
  return block + kSnapshotCopies;
}
std::string SnapshotFold(std::string value) {
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
bool SnapshotPath(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() ||
      path.native().size() > 4096)
    return false;
  for (const auto& component : path) {
    const auto name = component.generic_string();
    if (name.empty() || name == "." || name == ".." || name.size() > 40)
      return false;
    // POSIX's current Win1252 conversion is a no-op. Do not mislabel UTF8 as
    // native STFS bytes or rename an unsupported member to make it fit.
    for (const unsigned char c : name)
      if (c < 0x20 || c > 0x7E || c == '\\' || c == ':' || c == '/')
        return false;
  }
  return true;
}
}  // namespace

std::optional<StfsSnapshotMetadata> StfsSnapshotMetadata::FromFileInfo(
    std::filesystem::path relative, const filesystem::FileInfo& info,
    std::error_code& error) {
  error.clear();
  const auto encode =
      [](uint64_t timestamp) -> std::optional<std::array<uint16_t, 2>> {
    // Exact UTC 1980-01-01 <= value < 2108-01-01 in Windows FILETIME units.
    // Only the format's intrinsic two-second precision is discarded.
    constexpr uint64_t first = 119600064000000000ull;
    constexpr uint64_t end = 159992928000000000ull;
    if (timestamp < first || timestamp >= end) return {};
    const auto value = std::chrono::sys_seconds(
        std::chrono::seconds(int64_t(timestamp / 10000000) - 11644473600ll));
    const auto days = std::chrono::floor<std::chrono::days>(value);
    const std::chrono::year_month_day date(days);
    const std::chrono::hh_mm_ss clock(value - days);
    const auto fat_date =
        uint16_t(((int(date.year()) - 1980) << 9) |
                 (unsigned(date.month()) << 5) | unsigned(date.day()));
    const auto fat_time = uint16_t((clock.hours().count() << 11) |
                                   (clock.minutes().count() << 5) |
                                   (clock.seconds().count() / 2));
    return std::array<uint16_t, 2>{fat_date, fat_time};
  };
  const auto created = encode(info.create_timestamp);
  const auto modified = encode(info.write_timestamp);
  if (!created || !modified) {
    error = std::make_error_code(std::errc::operation_not_supported);
    return {};
  }
  return StfsSnapshotMetadata{std::move(relative), (*created)[0], (*created)[1],
                              (*modified)[0], (*modified)[1]};
}

std::shared_ptr<const StfsLogicalSnapshot> StfsLogicalSnapshot::Build(
    filesystem::ContentPackageStreamSource& source,
    std::span<const StfsSnapshotMetadata> metadata, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_not_permitted);
    return {};
  }
  const auto fail =
      [&](std::errc why) -> std::shared_ptr<const StfsLogicalSnapshot> {
    error = std::make_error_code(why);
    return {};
  };
  try {
    error = source.CheckCurrent();
    if (error) return {};
    const auto members = source.members();
    if (members.size() > kMaximumMembers || metadata.size() != members.size())
      return fail(std::errc::operation_not_supported);
    struct Row {
      std::filesystem::path path;
      std::string key;
      size_t source_index;
      bool directory;
      uint32_t length, blocks, start = kSnapshotEnd;
      StfsSnapshotMetadata metadata;
    };
    std::vector<Row> rows;
    rows.reserve(members.size());
    std::set<std::string> unique;
    uint64_t payload = 0, data_blocks = 0;
    for (size_t i = 0; i < members.size(); ++i) {
      const auto& member = members[i];
      if (!SnapshotPath(member.relative) ||
          metadata[i].relative != member.relative || member.size > UINT32_MAX ||
          (member.directory && member.size))
        return fail(std::errc::operation_not_supported);
      auto key = member.relative.generic_string();
      if (!unique.insert(SnapshotFold(key)).second)
        return fail(std::errc::file_exists);
      if (member.size > kMaximumPayloadBytes - payload)
        return fail(std::errc::file_too_large);
      payload += member.size;
      const uint32_t count = uint32_t((member.size + kSnapshotBlockBytes - 1) /
                                      kSnapshotBlockBytes);
      data_blocks += count;
      rows.push_back({member.relative, std::move(key), i, member.directory,
                      uint32_t(member.size), count, kSnapshotEnd, metadata[i]});
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    std::map<std::string, uint16_t> ordinals;
    for (size_t i = 0; i < rows.size(); ++i) {
      const auto parent = rows[i].path.parent_path().generic_string();
      if (!parent.empty()) {
        const auto found = ordinals.find(parent);
        if (found == ordinals.end() || !rows[found->second].directory)
          return fail(std::errc::invalid_argument);
      }
      ordinals.emplace(rows[i].key, uint16_t(i));
    }
    const uint32_t table_blocks =
        uint32_t(std::max(size_t(1), (rows.size() + 63) / 64));
    const uint64_t total64 = data_blocks + table_blocks;
    if (!total64 || total64 > kSnapshotCoverage.back() ||
        table_blocks > UINT16_MAX)
      return fail(std::errc::file_too_large);
    const uint32_t total = uint32_t(total64);
    const uint32_t highest = total <= 170 ? 0 : total <= 28900 ? 1 : 2;
    uint64_t physical = SnapshotDataBlock(total - 1) + 1;
    for (uint32_t level = 0; level <= highest; ++level)
      physical = std::max(
          physical, SnapshotHashBlock(total - 1, level) + kSnapshotCopies);
    if (physical > SIZE_MAX / kSnapshotBlockBytes)
      return fail(std::errc::file_too_large);
    auto output =
        std::shared_ptr<StfsLogicalSnapshot>(new StfsLogicalSnapshot());
    output->blocks_.resize(size_t(physical * kSnapshotBlockBytes));
    output->payload_bytes_ = payload;
    output->member_count_ = rows.size();
    std::vector<uint32_t> next(total, kSnapshotEnd);
    std::vector<StfsDirectoryBlock> directories(table_blocks);
    for (uint32_t i = 0; i + 1 < table_blocks; ++i) next[i] = i + 1;
    uint32_t cursor = table_blocks;
    for (size_t i = 0; i < rows.size(); ++i) {
      auto& row = rows[i];
      auto& entry = directories[i / 64].entries[i % 64];
      const auto name = row.path.filename().generic_string();
      std::copy(name.begin(), name.end(), entry.name);
      entry.flags.name_length = uint8_t(name.size());
      entry.flags.directory = row.directory;
      entry.flags.contiguous = false;
      const auto parent = row.path.parent_path().generic_string();
      entry.directory_index = parent.empty() ? 0xFFFF : ordinals.at(parent);
      entry.length = row.length;
      entry.create_date = row.metadata.create_date;
      entry.create_time = row.metadata.create_time;
      entry.modified_date = row.metadata.modified_date;
      entry.modified_time = row.metadata.modified_time;
      row.start = row.blocks ? cursor : kSnapshotEnd;
      output->member_layout_.push_back(
          {row.path, uint32_t(i), row.start, row.blocks});
      entry.set_valid_data_blocks(row.blocks);
      entry.set_allocated_data_blocks(row.blocks);
      entry.set_start_block_number(row.start);
      for (uint32_t block = 0; block + 1 < row.blocks; ++block)
        next[cursor + block] = cursor + block + 1;
      // Source bytes are an already-owned complete immutable snapshot. A
      // CheckCurrent here would re-read the entire physical package for every
      // row on the actual admitted source adapter. Join once before capture
      // and once after all bytes/hashes; native publication still validates
      // its same final guarded source/semantic cut separately.
      uint64_t offset = 0;
      for (uint32_t block = 0; block < row.blocks; ++block) {
        const size_t count = size_t(
            std::min<uint64_t>(kSnapshotBlockBytes, row.length - offset));
        auto bytes = std::span(output->blocks_)
                         .subspan(size_t(SnapshotDataBlock(cursor + block) *
                                         kSnapshotBlockBytes),
                                  count);
        const auto result = source.Read(row.source_index, offset, bytes);
        if (result.error) {
          error = result.error;
          return {};
        }
        if (result.bytes != count) return fail(std::errc::io_error);
        offset += count;
      }
      cursor += row.blocks;
    }
    for (uint32_t i = 0; i < table_blocks; ++i)
      std::memcpy(
          output->blocks_.data() + SnapshotDataBlock(i) * kSnapshotBlockBytes,
          &directories[i], sizeof(StfsDirectoryBlock));
    std::map<uint64_t, StfsHashTable> hashes;
    for (uint32_t block = 0; block < total; ++block) {
      auto& entry = hashes[SnapshotHashBlock(block, 0)].entries[block % 170];
      entry.set_level0_allocation_state(StfsHashState::kInUse);
      entry.set_level0_next_block(next[block]);
      if (mbedtls_sha1(output->blocks_.data() +
                           SnapshotDataBlock(block) * kSnapshotBlockBytes,
                       kSnapshotBlockBytes, entry.sha1))
        return fail(std::errc::io_error);
    }
    for (uint32_t level = 1; level <= highest; ++level) {
      const auto coverage = kSnapshotCoverage[level - 1];
      for (uint32_t block = 0; block < total; block += coverage) {
        const auto& child = hashes.at(SnapshotHashBlock(block, level - 1));
        auto& parent = hashes[SnapshotHashBlock(block, level)];
        auto& entry = parent.entries[(block / coverage) % 170];
        entry.set_levelN_writeable(true);
        if (mbedtls_sha1(reinterpret_cast<const uint8_t*>(&child),
                         sizeof(child), entry.sha1))
          return fail(std::errc::io_error);
        parent.num_blocks =
            std::min(kSnapshotCoverage[level],
                     total - (block / kSnapshotCoverage[level]) *
                                 kSnapshotCoverage[level]);
      }
    }
    StfsVolumeDescriptor descriptor{};
    descriptor.descriptor_length = sizeof(descriptor);
    descriptor.version = 0;
    descriptor.flags.as_byte = 0;
    descriptor.file_table_block_count = uint16_t(table_blocks);
    descriptor.set_file_table_block_number(0);
    descriptor.total_block_count = total;
    descriptor.free_block_count = 0;
    const auto& root = hashes.at(SnapshotHashBlock(0, highest));
    if (mbedtls_sha1(reinterpret_cast<const uint8_t*>(&root), sizeof(root),
                     descriptor.top_hash_table_hash))
      return fail(std::errc::io_error);
    for (const auto& [block, table] : hashes)
      for (uint32_t copy = 0; copy < kSnapshotCopies; ++copy)
        std::memcpy(
            output->blocks_.data() + (block + copy) * kSnapshotBlockBytes,
            &table, sizeof(table));
    std::memcpy(output->descriptor_.data(), &descriptor, sizeof(descriptor));
    // Descriptor's table count is little endian even though its counts/hash
    // records are big endian. Preserve that wire form independently of host.
    output->descriptor_[3] = uint8_t(table_blocks);
    output->descriptor_[4] = uint8_t(table_blocks >> 8);
    error = source.CheckCurrent();
    if (error) return {};
    return output;
  } catch (const std::bad_alloc&) {
    return fail(std::errc::not_enough_memory);
  }
}
toml::table StfsMetadata::ToToml() const {
  toml::table root;

  toml::table content_table;
  content_table.insert("title_id", fmt::format("{:08X}", title_id));
  content_table.insert("media_id", fmt::format("{:08X}", media_id));
  content_table.insert("savegame_id", fmt::format("{:08X}", savegame_id));
  content_table.insert("content_type", fmt::format("{:08X}", content_type));
  content_table.insert("version", XexVersion::FromValue(version).ToString());
  content_table.insert("content_size", static_cast<int64_t>(content_size));
  content_table.insert("package_license_mask", package_license_mask);
  content_table.insert("native_category", native_category);
  content_table.insert("disc_number", static_cast<int64_t>(disc_number));
  content_table.insert("disc_count", static_cast<int64_t>(disc_count));
  root.insert("content_info", std::move(content_table));

  toml::table display_table;
  if (!display_name.empty()) {
    display_table.insert("display_name", display_name);
  }
  if (!description.empty()) {
    display_table.insert("description", description);
  }
  if (!publisher.empty()) {
    display_table.insert("publisher", publisher);
  }
  if (!title_name.empty()) {
    display_table.insert("title_name", title_name);
  }
  if (!display_table.empty()) {
    root.insert("display_info", std::move(display_table));
  }

  if (title_resources_status != TitleResourcesStatus::kNotRequested) {
    root.insert("title_resources_status", uint32_t(title_resources_status));
    if (title_resources) {
      root.insert("title_resources", title_resources->ToToml());
    }
  }

  return root;
}

std::optional<StfsMetadata> ExtractStfsMetadata(
    const std::filesystem::path& path, XLanguage language,
    bool read_title_resources) {
  auto header = XContentContainerDevice::ReadContainerHeader(path);
  if (!header) {
    return std::nullopt;
  }

  if (!header->content_header.is_magic_valid()) {
    return std::nullopt;
  }

  // Fixed guest strings may lack a terminator or contain isolated UTF16
  // surrogates. Bound the local header copy before its legacy accessors; only
  // display conversion is repaired, never the file or numeric identities.
  auto &bounded = header->content_metadata;
  for (uint32_t i = 0; i < XContentMetadata::kNumLanguagesV1; ++i) {
    bounded.display_name_raw.uint[i][127] = 0;
    bounded.description_raw.uint[i][127] = 0;
  }
  if (bounded.metadata_version >= 2) {
    for (uint32_t i = 0; i < XContentMetadata::kNumLanguagesV2 -
                                 XContentMetadata::kNumLanguagesV1;
         ++i) {
      bounded.display_name_ex_raw.uint[i][127] = 0;
      bounded.description_ex_raw.uint[i][127] = 0;
    }
  }
  bounded.publisher_raw.uint[63] = 0;
  bounded.title_name_raw.uint[63] = 0;
  StfsMetadata metadata;
  const auto& content_meta = header->content_metadata;
  const auto& exec_info = content_meta.execution_info;

  metadata.title_id = exec_info.title_id;
  metadata.media_id = exec_info.media_id;
  metadata.savegame_id = exec_info.savegame_id;
  metadata.version = exec_info.version_value;
  metadata.disc_number = exec_info.disc_number;
  metadata.disc_count = exec_info.disc_count;

  metadata.content_type = static_cast<uint32_t>(
      static_cast<XContentType>(content_meta.content_type));
  metadata.content_size = content_meta.content_size;
  metadata.native_category = content_meta.category;
  metadata.package_license_mask =
      XContentContainerDevice::LicenseMaskFromHeader(*header);

  // Tiered: requested language → English → first populated slot. Publishers
  // routinely misuse slots (e.g. Japanese text in the English slot for
  // region-locked games), so this is best-effort.
  auto pick_localized = [&content_meta, language](auto getter) {
    auto str = getter(content_meta, language);
    if (str.empty() && language != XLanguage::kEnglish) {
      str = getter(content_meta, XLanguage::kEnglish);
    }
    if (str.empty()) {
      for (uint32_t i = uint32_t(XLanguage::kEnglish);
           i <= content_meta.kNumLanguagesV2; ++i) {
        auto candidate = static_cast<XLanguage>(i);
        if (candidate == language || candidate == XLanguage::kEnglish) {
          continue;
        }
        auto alt = getter(content_meta, candidate);
        if (!alt.empty()) {
          str = std::move(alt);
          break;
        }
      }
    }
    return U16ToUtf8(str);
  };
  metadata.display_name = pick_localized(
      [](const auto& m, XLanguage l) { return m.display_name(l); });
  metadata.description = pick_localized(
      [](const auto& m, XLanguage l) { return m.description(l); });
  metadata.publisher = U16ToUtf8(content_meta.publisher());
  metadata.title_name = U16ToUtf8(content_meta.title_name());

  const uint32_t thumb_size = content_meta.title_thumbnail_size;
  const uint32_t thumb_limit = content_meta.metadata_version >= 2
                                   ? XContentMetadata::kThumbLengthV2
                                   : XContentMetadata::kThumbLengthV1;
  if (thumb_size > 0 && thumb_size <= thumb_limit) {
    metadata.icon_data.assign(content_meta.title_thumbnail,
                              content_meta.title_thumbnail + thumb_size);
  }

  if (read_title_resources) {
    metadata.title_resources_status = TitleResourcesStatus::kAbsent;
    auto device = XContentContainerDevice::CreateContentDevice("", path);
    if (!device || !device->Initialize()) {
      metadata.title_resources_status = TitleResourcesStatus::kInvalid;
    } else if (auto* entry =
                   static_cast<Device&>(*device).ResolvePath("default.xex")) {
      metadata.title_resources_status = TitleResourcesStatus::kInvalid;
      File* raw_file = nullptr;
      if (entry->size() <= kMaxOfflineXexFileBytes &&
          entry->Open(xe::filesystem::FileAccess::kFileReadData, &raw_file) ==
              X_STATUS_SUCCESS) {
        struct CloseFile {
          void operator()(File* f) const { f->Destroy(); }
        };
        std::unique_ptr<File, CloseFile> file(raw_file);
        std::vector<uint8_t> executable(entry->size());
        size_t read = 0;
        if (file->ReadSync(executable, 0, &read) == X_STATUS_SUCCESS &&
            read == executable.size()) {
          if (auto xex = ExtractXexMetadata(executable.data(),
                                            executable.size(), true)) {
            metadata.title_resources = std::move(xex->title_resources);
            metadata.title_resources_status = xex->title_resources_status;
          }
        }
      }
    }
  }
  return metadata;
}

}  // namespace vfs
}  // namespace xe
