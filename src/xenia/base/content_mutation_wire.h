/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_CONTENT_MUTATION_WIRE_H_
#define XENIA_BASE_CONTENT_MUTATION_WIRE_H_
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "xenia/base/content_package_tree.h"

namespace xe::filesystem::content_mutation_wire {
inline constexpr uint64_t kMaximumMetadataBytes = 128ull << 20;
inline constexpr uint64_t kMaximumRecordBytes =
    2 * kMaximumMetadataBytes + 65536;
enum class Kind : uint32_t { kHeader = 1, kThumbnail = 2, kCreate = 3 };
// Persisted directory/file IDs are stable source-local IDs. Volatile st_dev,
// mount IDs and live registry generations must never become restart identity.
struct Identity {
  std::array<uint8_t, 16> uuid{};
  uint64_t epoch = 0, content_directory = 0;
  uint32_t device = 0;
  std::string source;
  bool operator==(const Identity&) const = default;
};
struct Record {
  Identity identity;
  Kind kind = Kind::kHeader;
  std::string nonce;  // Exactly32 lowercase hexadecimal characters.
  // Exactly physical XUID16/TITLE8/TYPE8/FATX package leaf, never an alias.
  std::filesystem::path package;
  // Exact retained physical spelling of the header/thumbnail. It must be
  // ASCII-fold equivalent to the derived semantic slot, never an arbitrary
  // alternate path. Existing Headers/thumbnail case is significant on host FS.
  std::filesystem::path target;
  bool before_exists = false;
  uint64_t before_file = 0, stage_file = 0, recovery_file = 0;
  std::vector<uint8_t> before, intended;
  std::filesystem::path stage, recovery;
  // Only create owns this exact private staged directory identity.
  uint64_t directory_stage = 0;
  // Empty retains exact v2 empty-create wire bytes. Nonempty is v3 initial-tree
  // creation; the root and EVERY actual member are authenticated before Done.
  std::vector<ContentPackageInitialEntry> initial_tree;
};
inline constexpr uint64_t kMaximumInitialBytes = 128ull << 20;
std::error_code ValidateInitialMembers(std::span<const ContentPackageInitialMember>);
bool ValidPackage(const std::filesystem::path&);
std::filesystem::path Target(const Record&);
bool ValidNonce(std::string_view);
std::array<uint8_t, 32> Digest(std::span<const uint8_t>, std::error_code&);
std::vector<uint8_t> Encode(const Record&, std::error_code&);
bool Decode(std::span<const uint8_t>, Record&, std::error_code&);
std::vector<uint8_t> Done(std::span<const uint8_t> prepared, std::error_code&);
bool CheckDone(std::span<const uint8_t> done,
               std::span<const uint8_t> prepared, std::error_code&);
}  // namespace xe::filesystem::content_mutation_wire
#endif
