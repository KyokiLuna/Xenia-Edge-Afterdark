/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_FOLDER_CONTENT_HEADER_H_
#define XENIA_KERNEL_XAM_FOLDER_CONTENT_HEADER_H_
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <cstring>
#include <optional>
#include <span>
#include <system_error>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "xenia/base/content_mutation_wire.h"
#include "xenia/base/content_mutation_owner.h"
#include "xenia/base/content_package_tree.h"
#include "xenia/kernel/console_signing_identity.h"
#include "xenia/kernel/xboxkrnl/xecrypt_rsa.h"
#include "xenia/vfs/devices/stfs_xbox.h"
namespace xe::kernel::xam::folder_content_detail {
// Bounded decoded format facts ONLY; original native 80128 still performs
// its complete independent parser/permission checks. The actual CM source/header/member/File issuer
// supplies all access rights, source-generation admission and real backing.
struct HeaderFacts {
  uint32_t header_size = 0, context_bytes = 0;
  uint64_t data_offset = 0, content_size = 0, logical_geometry = 0, raw_extent = 0;
  uint8_t signature_class = 0;
  bool read_only_format = false;
  std::array<uint8_t, 36> descriptor{};
};
struct PackageIdentity {
  uint64_t xuid = 0;
  uint32_t title_id = 0;
  XContentType content_type = {};
};
inline std::error_code HeaderError(std::errc error) { return std::make_error_code(error); }
// An owned VIRTUAL address extent inside the SAME retained source/File State.
// This is not packed physical EOF, host allocation, readable hash storage or
// member->native block/chain assignment. Unknown raw ranges still refuse.
struct FolderVirtualExtent {
  uint64_t bytes = 0;
  uint32_t blocks = 0;
  uint16_t table_blocks = 0;
  uint8_t hash_copies = 0;
};
// Scalar census from the real retained metadata producer, not source/writer
// authority and not native chain/hash bytes. Shared by the actual raw issuer
// and explicit source fixtures so neither guesses descriptor block counts.
struct FolderLayoutCounts { uint32_t blocks = 0; uint16_t table_blocks = 0; };
inline std::optional<FolderLayoutCounts> CaptureFolderLayoutCounts(
    const filesystem::HostStorageContentLayout& layout, std::error_code& error) {
  error.clear();
  const auto unsupported = [&]() -> std::optional<FolderLayoutCounts> {
    error = HeaderError(std::errc::operation_not_supported); return {};
  };
  uint64_t entries = 0, payload = 0;
  bool root = false;
  for (const auto& member : layout.entries()) {
    if (member.relative.empty()) {
      if (root || !member.directory) return unsupported();
      root = true; continue;
    }
    const auto name = member.relative.filename().generic_string();
    if (name.empty() || name.size() > 40 || ++entries > UINT16_MAX ||
        (!member.directory && member.bytes > UINT32_MAX)) return unsupported();
    if (!member.directory) {
      payload += (member.bytes + 4095) / 4096;
      if (payload >= 4913000) return unsupported();
    }
  }
  const uint64_t tables = std::max(uint64_t(1), (entries + 63) / 64);
  if (!root || tables > UINT16_MAX || payload + tables >= 4913000 ||
      payload * 4096 != layout.statistics().data_extent) return unsupported();
  return FolderLayoutCounts{uint32_t(payload + tables), uint16_t(tables)};

}
inline std::optional<FolderVirtualExtent> CaptureFolderVirtualExtent(
    const filesystem::HostStorageContentLayout& layout,
    std::span<const uint8_t> header_bytes, std::error_code& error) {
  error.clear();
  const auto unsupported = [&]() -> std::optional<FolderVirtualExtent> {
    error = HeaderError(std::errc::operation_not_supported); return {};
  };
  if (header_bytes.size() < sizeof(vfs::XContentContainerHeader)) return unsupported();
  vfs::XContentContainerHeader header{};
  std::memcpy(&header, header_bytes.data(), sizeof(header));
  const auto& descriptor = header.content_metadata.volume_descriptor.stfs;
  // Exact no-free/no-overallocation subset only. Counts do not recover old
  // allocation/chain/free-state distinctions, so none are invented here.
  const uint64_t total = descriptor.total_block_count;
  if (!descriptor.is_valid() || !total || total >= 4913000 ||
      uint32_t(descriptor.free_block_count) || descriptor.flags.bits.directory_overallocated ||
      (descriptor.flags.bits.read_only_format && descriptor.flags.bits.root_active_index)) return unsupported();
  const auto counts = CaptureFolderLayoutCounts(layout, error);
  if (!counts) { return {}; }
  if (uint64_t(descriptor.file_table_block_count) != counts->table_blocks ||
      descriptor.file_table_block_number() >= total || total != counts->blocks) return unsupported();
  const uint64_t copies = descriptor.flags.bits.read_only_format ? 1 : 2;
  constexpr std::array<uint64_t, 3> level_base{170, 28900, 4913000};
  const std::array<uint64_t, 2> step{170 + copies, 28900 + 171 * copies};
  const uint64_t last = total - 1;
  uint64_t physical_data = last;
  for (const auto base : level_base) {
    physical_data += ((last + base) / base) * copies;
    if (last < base) break;
  }
  uint64_t end = physical_data + 1;
  // GetBlockHash explicitly limits the single-copy read-only FORMAT to L0;
  // Device/host read-only capability is unrelated to this native format bit.
  const uint32_t highest = copies == 1 ? 0 : total < 170 ? 0 : total < 28900 ? 1 : 2;
  // Include EVERY required structural hash range, not just last-data+1.
  // At total170/28900 the genuine decoder requires a trailing higher-level
  // table beyond that data end. Arithmetic is bounded by total<4913000.
  for (uint32_t level = 0; level <= highest; ++level) {
    uint64_t block;
    if (level == 2) block = step[1];
    else if (last < level_base[level]) block = level == 0 ? 0 : step[level - 1];
    else {
      block = (last / level_base[level]) * step[level];
      if (level == 0) {
        block += ((last / level_base[1]) + 1) * copies;
        if (last >= level_base[1]) block += copies;
      } else block += copies;
    }
    end = std::max(end, block + copies);
  }
  if (end > UINT64_MAX / 4096) return unsupported();
  return FolderVirtualExtent{end * 4096, uint32_t(total), counts->table_blocks, uint8_t(copies)};
}

inline uint32_t NativeHeaderSize(std::span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(vfs::XContentContainerHeader)) { return 0; }
  return (uint32_t(bytes[0x340]) << 24) | (uint32_t(bytes[0x341]) << 16) |
      (uint32_t(bytes[0x342]) << 8) | bytes[0x343];
}
// Original 17559 C628/80128 authenticate through the aligned data boundary,
// not merely HeaderSize: fixed 0x93D6 bytes plus actual [0x971A, aligned).
// This shape check cannot supply missing bytes or source/writer authority.
inline uint32_t NativeHeaderAlignedSize(std::span<const uint8_t> bytes) {
  const uint32_t declared = NativeHeaderSize(bytes);
  const uint64_t aligned = (uint64_t(declared) + 0xFFF) & ~uint64_t(0xFFF);
  return declared >= sizeof(vfs::XContentContainerHeader) &&
      aligned <= bytes.size() &&
      aligned <= filesystem::content_mutation_wire::kMaximumMetadataBytes
      ? uint32_t(aligned) : 0;
}
inline bool HeaderBodyDigest(std::span<const uint8_t> bytes,
    std::array<uint8_t, 20>& digest) {
  const uint32_t aligned = NativeHeaderAlignedSize(bytes);
  // Any bytes after aligned remain an opaque sidecar tail. The actual source
  // must supply every authenticated padding byte; H-only data cannot mint it.
  return aligned && !mbedtls_sha1(bytes.data() + 0x344,
                                  aligned - 0x344, digest.data());
}
// Existing admitted Header bytes only. The actual CM producer must separately
// prove their source/before token, native write participant and complete
// package identity. This cannot mint a source, CON context or File authority.
// No private key survives this synchronous call; retained Files keep only the
// exact trusted PUBLIC certificate supplied by ConsoleDataOwner/CM.
inline std::error_code SignNativeHeader(std::vector<uint8_t>& bytes,
    const ConsoleSigningIdentity& signer) {
  if (bytes.size() > filesystem::content_mutation_wire::kMaximumMetadataBytes) {
    return HeaderError(std::errc::file_too_large);
  }
  vfs::XContentContainerHeader parsed{};
  if (bytes.size() < sizeof(parsed)) { return HeaderError(std::errc::bad_message); }
  std::memcpy(&parsed, bytes.data(), sizeof(parsed));
  if (parsed.content_header.magic != vfs::XContentPackageType::kCon ||
      parsed.content_metadata.volume_type != vfs::XContentVolumeType::kStfs ||
      !parsed.content_metadata.volume_descriptor.stfs.is_valid()) {
    return HeaderError(std::errc::operation_not_supported);
  }
  std::array<uint8_t, 20> body{}, digest{};
  if (!HeaderBodyDigest(bytes, body)) { return HeaderError(std::errc::bad_message); }
  // Build ALL possibly failing output off to the side. Failure cannot leave
  // a new content digest with an old/missing signature in the caller buffer.
  auto header = parsed.content_header;
  std::copy(body.begin(), body.end(), header.content_id);
  if (mbedtls_sha1(reinterpret_cast<const uint8_t*>(&header) + 0x22C,
                    0x118, digest.data())) { return HeaderError(std::errc::io_error); }
  ConsoleSigningIdentity::Signature signature{};
  if (!signer.Sign(digest, signature)) { return HeaderError(std::errc::io_error); }
  std::copy(body.begin(), body.end(), bytes.begin() + 0x32C);
  std::copy(signature.begin(), signature.end(), bytes.begin() + 4);
  return {};
}
inline std::optional<HeaderFacts> ValidateNativeHeader(
    std::span<const uint8_t> bytes, PackageIdentity expected,
    uint64_t actually_owned_data_extent,
    std::span<const uint8_t> trusted_local_certificate,
    std::error_code& error,
    std::span<const uint8_t> trusted_master_public_key = {}) {
  error.clear();
  if (bytes.size() < sizeof(vfs::XContentContainerHeader) ||
      bytes.size() > filesystem::content_mutation_wire::kMaximumMetadataBytes ||
      trusted_local_certificate.size() != ConsoleSigningIdentity::kCertificateBytes) {
    error = HeaderError(std::errc::invalid_argument); return {};
  }
  vfs::XContentContainerHeader parsed{};
  std::memcpy(&parsed, bytes.data(), sizeof(parsed));
  const auto& metadata = parsed.content_metadata;
  if (parsed.content_header.magic != vfs::XContentPackageType::kCon ||
      metadata.volume_type != vfs::XContentVolumeType::kStfs ||
      !metadata.volume_descriptor.stfs.is_valid()) {
    error = HeaderError(std::errc::operation_not_supported); return {};
  }
  if (uint64_t(metadata.profile_id) != expected.xuid ||
      uint32_t(metadata.execution_info.title_id) != expected.title_id ||
      metadata.content_type != expected.content_type) {
    error = HeaderError(std::errc::permission_denied); return {};
  }
  std::array<uint8_t, 20> body{}, digest{};
  if (!HeaderBodyDigest(bytes, body) || !std::equal(body.begin(), body.end(),
                                                   parsed.content_header.content_id)) {
    error = HeaderError(std::errc::bad_message); return {};
  }
  if (mbedtls_sha1(bytes.data() + 0x22C, 0x118, digest.data())) {
    error = HeaderError(std::errc::io_error); return {};
  }
  uint32_t comparison = 0;
  if (!XeCryptVerifyConsoleSignature(trusted_local_certificate, digest,
      bytes.subspan(4, ConsoleSigningIdentity::kSignatureBytes), comparison,
      trusted_master_public_key)) {
    // No invented Microsoft root, foreign self-certificate trust or retail
    // attestation. Preserve unsupported foreign data exactly as it was.
    error = HeaderError(comparison && trusted_master_public_key.empty()
        ? std::errc::operation_not_supported : std::errc::bad_message); return {};
  }
  HeaderFacts result;
  result.header_size = NativeHeaderSize(bytes);
  result.data_offset = NativeHeaderAlignedSize(bytes);
  result.content_size = metadata.content_size;
  if (result.content_size > actually_owned_data_extent ||
      actually_owned_data_extent > UINT64_MAX - result.data_offset ||
      result.content_size > UINT64_MAX - 0xFFFFF ||
      result.data_offset > UINT32_MAX - 0x192) {
    error = HeaderError(std::errc::operation_not_supported); return {};
  }
  result.raw_extent = result.data_offset + actually_owned_data_extent;
  result.context_bytes = uint32_t(result.data_offset + 0x192);
  result.logical_geometry = ((result.content_size ? result.content_size :
      uint64_t(0xFFFFF000)) + 0xFFFFF) & ~uint64_t(0xFFFFF);
  result.signature_class = comparison ? 2 : 1;
  result.read_only_format = metadata.volume_descriptor.stfs.flags.bits.read_only_format;
  std::memcpy(result.descriptor.data(), &metadata.volume_descriptor.stfs, 36);
  return result;
}

// Source-bound mathematical/shape policy for the actual recovered FE20012
// metadata writer. It does not infer native task/caller/source permissions.
class NativeFolderHeaderFormat final : public filesystem::ContentMutationOwner::NativeHeaderFormat {
 public:
  explicit NativeFolderHeaderFormat(ConsoleSigningIdentity::Certificate trusted)
      : trusted_(std::move(trusted)) {}
  bool SameBinding(const filesystem::ContentMutationOwner::NativeHeaderFormat& other) const noexcept override {
    const auto* format = dynamic_cast<const NativeFolderHeaderFormat*>(&other);
    return format && format->trusted_ == trusted_;
  }
  std::error_code ValidateCurrent(const std::filesystem::path& package,
      std::span<const uint8_t> bytes) const override {
    std::vector<std::string> parts;
    for (const auto& p : package) { parts.push_back(p.generic_string()); }
    if (!filesystem::content_mutation_wire::ValidPackage(package) || parts.size() != 4 ||
        bytes.size() < sizeof(vfs::XContentContainerHeader)) { return HeaderError(std::errc::invalid_argument); }
    const auto hex = [](std::string_view value) {
      uint64_t result = 0;
      for (unsigned char c : value) { result = result * 16 +
          (c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c - 'a' + 10); }
      return result;
    };
    vfs::XContentContainerHeader parsed{}; std::memcpy(&parsed, bytes.data(), sizeof(parsed));
    std::error_code error;
    // This validates format/crypto only. Real source/backing extent is checked
    // independently by the actual raw File producer and kernel mount. No fake
    // raw EOF or namespace permission is minted from this declared content size.
    const auto checked = ValidateNativeHeader(bytes,
        {hex(parts[0]), uint32_t(hex(parts[1])), XContentType(uint32_t(hex(parts[2])))},
        parsed.content_metadata.content_size, trusted_, error);
    return checked ? std::error_code{} : error;
  }
  std::error_code ValidateNativeChange(const std::filesystem::path& package,
      std::span<const uint8_t> before, std::span<const uint8_t> intended) const override {
    if (auto error = ValidateCurrent(package, before)) { return error; }
    if (auto error = ValidateCurrent(package, intended)) { return error; }
    if (before.size() != intended.size() || NativeHeaderSize(before) != NativeHeaderSize(intended)) {
      return HeaderError(std::errc::operation_not_supported);
    }
    vfs::XContentContainerHeader parsed{}; std::memcpy(&parsed, intended.data(), sizeof(parsed));
    const uint32_t count = parsed.content_metadata.thumbnail_size;
    if (count > vfs::XContentMetadata::kThumbLengthV2) { return HeaderError(std::errc::bad_message); }
    constexpr size_t size_offset = offsetof(vfs::XContentContainerHeader, content_metadata) +
        offsetof(vfs::XContentMetadata, thumbnail_size);
    constexpr size_t picture_offset = offsetof(vfs::XContentContainerHeader, content_metadata) +
        offsetof(vfs::XContentMetadata, thumbnail);
    static_assert(size_offset == 0x1712 && picture_offset == 0x171A);
    for (size_t i = 0; i < before.size(); ++i) {
      const bool changed_by_original = (i >= 4 && i < 0x22C) || (i >= 0x32C && i < 0x340) ||
          (i >= size_offset && i < size_offset + 4) ||
          (i >= picture_offset && i < picture_offset + count);
      if (!changed_by_original && before[i] != intended[i]) {
        return HeaderError(std::errc::operation_not_supported);
      }
    }
    // No guessed VolumeDescriptor/geometry update permission: actual control
    // flush/descriptor duties remain unsupported until their real owner exists.
    return {};
  }
 private:
  const ConsoleSigningIdentity::Certificate trusted_;
};
}  // namespace xe::kernel::xam::folder_content_detail
#endif
