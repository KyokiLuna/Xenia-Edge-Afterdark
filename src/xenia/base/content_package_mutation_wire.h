/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_CONTENT_PACKAGE_MUTATION_WIRE_H_
#define XENIA_BASE_CONTENT_PACKAGE_MUTATION_WIRE_H_
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include "xenia/base/content_mutation_wire.h"
#include "xenia/base/content_package_tree.h"

namespace xe::filesystem::content_package_mutation_wire {
inline constexpr uint64_t kMaximumRecordBytes = 256ull << 20;
inline constexpr uint64_t kMaximumAggregatePathBytes = 16ull << 20;
inline constexpr uint32_t kMaximumEntries = 65536;
inline constexpr uint32_t kMaximumPathBytes = 4096;
inline constexpr uint32_t kMaximumDepth = 64;
using Identity = content_mutation_wire::Identity;
enum class Kind : uint32_t { kDelete = 1, kTruncate = 2 };
// Data only. Neither a valid checksum nor caller-filled identities/tree entries
// grant live source, namespace, stage, retirement, or publication authority.
struct Record {
  Identity identity;
  Kind kind = Kind::kDelete;
  std::string nonce;  // Exactly32 lowercase hex, nonzero.
  std::filesystem::path package;  // Physical XUID16/TITLE8/TYPE8/FATX leaf.
  uint64_t op_directory = 0;
  std::filesystem::path header_target;  // Exact physical semantic Header slot.
  bool header_exists = false;
  uint64_t header_file = 0;
  std::vector<uint8_t> header;  // Complete raw bytes, including readable empty.
  std::filesystem::path preserved_leaf;  // Empty, or actual-case __thumbnail.png.
  uint64_t replacement_directory = 0;  // Only packed-file truncate.
  std::vector<ContentPackageTreeEntry> before;  // Root first, then parents first.
  bool operator==(const Record&) const = default;
};
// POSIX native bytes are preserved without locale conversion. Wide native-path
// platforms truthfully refuse this version rather than persist guessed bytes.
std::vector<uint8_t> Encode(const Record&, std::error_code&);
bool Decode(std::span<const uint8_t>, Record&, std::error_code&);
std::vector<uint8_t> Retire(std::span<const uint8_t> prepared, std::error_code&);
bool CheckRetire(std::span<const uint8_t> retire,
                 std::span<const uint8_t> prepared, std::error_code&);
std::vector<uint8_t> Done(std::span<const uint8_t> prepared,
                         std::span<const uint8_t> retire, std::error_code&);
bool CheckDone(std::span<const uint8_t> done,
               std::span<const uint8_t> prepared,
               std::span<const uint8_t> retire, std::error_code&);
}  // namespace xe::filesystem::content_package_mutation_wire
#endif
