/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_STFS_METADATA_H_
#define XENIA_VFS_STFS_METADATA_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/content_package_tree.h"
#include "xenia/vfs/title_metadata.h"
#include "xenia/xbox.h"

namespace xe {
namespace vfs {

// Exact format metadata accompanying the source owner's complete member
// snapshot. These detached values grant no File/source/publication authority.
// A caller supplies captured native FAT words, including explicit native
// unknown-zero words. FromFileInfo converts a real nonzero UTC timestamp;
// missing host timestamps refuse rather than inventing native metadata.
struct StfsSnapshotMetadata {
  std::filesystem::path relative;
  uint16_t create_date, create_time, modified_date, modified_time;
  // UTC FILETIME conversion with a caller-proven actual creation timestamp.
  // Generic HostStorageSnapshotBatch FileInfo creation follows POSIX ctime;
  // it is NOT native FAT creation metadata and must never be supplied here.
  // Captured native FAT words may instead be supplied directly in this row.
  // Out-of-range dates refuse, never clamp.
  static std::optional<StfsSnapshotMetadata> FromFileInfo(
      std::filesystem::path, const filesystem::FileInfo&, std::error_code&);
};

struct StfsSnapshotMemberLayout {
  std::filesystem::path relative;
  uint32_t entry_index = 0, start_block = 0, block_count = 0;
  bool operator==(const StfsSnapshotMemberLayout&) const = default;
};
// Immutable canonical STFS directory/data/hash image, starting at the actual
// Header's aligned data offset. It contains no Header, signature, Account
// parser, source permission, IO checkpoint or native control acknowledgement.
// Existing packed/native noncanonical layouts are NOT adopted from a Header
// descriptor. Their owner must preserve a real mapping or transition only
// through the genuine checkpoint/Header-persistence protocol.
class StfsLogicalSnapshot final {
 public:
  static constexpr uint32_t kLayoutVersion = 1;
  static constexpr size_t kMaximumMembers = 4096;
  static constexpr uint64_t kMaximumPayloadBytes = 128ull * 1024 * 1024;
  // Reads exactly the caller-owned complete immutable member source, joining
  // CheckCurrent before/after capture (not per row/chunk). A complete-source
  // validation can read every physical byte; immutable Read needs no repeated
  // census. Actual native publication must separately retain its final guarded
  // source/semantic cut. No raw HostStorage fallback for protected members.
  static std::shared_ptr<const StfsLogicalSnapshot> Build(
      filesystem::ContentPackageStreamSource&,
      std::span<const StfsSnapshotMetadata>, std::error_code&);
  const std::array<uint8_t, 36>& descriptor() const { return descriptor_; }
  std::span<const uint8_t> block_image() const { return blocks_; }
  uint64_t payload_bytes() const { return payload_bytes_; }
  size_t member_count() const { return member_count_; }
  // Exact assignments from THIS serializer, not another allocation algorithm.
  const auto& member_layout() const { return member_layout_; }

 private:
  StfsLogicalSnapshot() = default;
  std::array<uint8_t, 36> descriptor_{};
  std::vector<uint8_t> blocks_;
  uint64_t payload_bytes_ = 0;
  size_t member_count_ = 0;
  std::vector<StfsSnapshotMemberLayout> member_layout_;
};

struct StfsMetadata {
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t savegame_id = 0;
  uint32_t content_type = 0;
  // Packed XEX version, the form XexVersion::FromValue() takes.
  uint32_t version = 0;
  uint64_t content_size = 0;
  uint32_t package_license_mask = 0;
  // Physical STFS category, not SPA TitleType or a host-derived substitute.
  uint32_t native_category = 0;

  std::string display_name;
  std::string description;
  std::string publisher;
  std::string title_name;

  uint8_t disc_number = 0;
  uint8_t disc_count = 0;

  // Raw bytes of the embedded title_thumbnail (PNG). Empty if absent.
  std::vector<uint8_t> icon_data;

  TitleResourcesStatus title_resources_status =
      TitleResourcesStatus::kNotRequested;
  std::optional<TitleResourceMetadata> title_resources;

  toml::table ToToml() const;
};

// `language` selects which localized title/description slot to read.
// Falls back to English automatically if the requested slot is empty.
std::optional<StfsMetadata> ExtractStfsMetadata(
    const std::filesystem::path& path, XLanguage language = XLanguage::kEnglish,
    bool read_title_resources = false);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_STFS_METADATA_H_
