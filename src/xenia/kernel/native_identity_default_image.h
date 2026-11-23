/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include "xenia/kernel/native_identity_image_journal.h"

namespace xe::kernel::identity_detail {
// These are transient, already-authenticated ORIGINAL native observations.
// This value helper reads no guest bytes and retains no strings, native refs,
// streams, package/cache entries, modules or authority. Its caller owns exact
// frame/code/module/currentness and joins actual original call/return facts.
struct NativeDefaultRootSection {
  NativeProfileIdentityTasks::Root root;
  uint32_t module = 0;
  uint64_t module_generation = 0;
  std::string_view name;
  uint32_t address = 0, size = 0;
};
struct NativeDefaultRowObservation {
  NativeProfileIdentityTasks::Root root;
  uint64_t initializer = 0;
  uint32_t index = 0, row_output = 0, module = 0;
  uint64_t module_generation = 0;
  std::u16string_view section, entry;
};
struct NativeDefaultResourceOpenObservation {
  NativeProfileIdentityTasks::Root root;
  uint64_t initializer = 0, invocation = 0;
  uint32_t row_output = 0, module = 0, stream = 0, result = 0x80004005;
  std::u16string_view section, entry;
};
struct NativeDefaultGetSectionObservation {
  NativeProfileIdentityTasks::Root root;
  uint32_t module = 0;
  uint64_t module_generation = 0;
  std::string_view name;
  uint32_t address = 0, size = 0, result = 0x80004005;
};
struct NativeDefaultGetBufferObservation {
  NativeProfileIdentityTasks::Root root;
  uint64_t initializer = 0, resource_open = 0;
  uint32_t row_output = 0, stream = 0, address = 0, size = 0;
  uint32_t result = 0x80004005;
};
struct NativeDefaultFallbackObservation {
  NativeProfileIdentityTasks::Root root;
  uint32_t current_module = 0;
  uint64_t current_module_generation = 0;
  uint32_t index = 0;
  uint64_t provider_birth = 0, source = 0, version = 0;
  uint32_t address = 0, size = 0, result = 0x8000FFFF;
};

// Immutable scalar row genealogy inside the SAME Owner. It is not a resource
// cache/registry and does not extend backing lifetime. A cached/unknown native
// resource route without its independently authenticated original root-section
// and package-entry join cannot create these observations.
class NativeDefaultImageRow final {
 public:
  static std::optional<NativeDefaultImageRow> Observe(
      const NativeDefaultRootSection& current,
      const NativeDefaultRowObservation& row,
      const NativeDefaultResourceOpenObservation& open,
      const NativeDefaultGetSectionObservation& section,
      const NativeDefaultGetBufferObservation& buffer) noexcept {
    if (!current.root.valid() || !current.module ||
        current.module_generation != current.root.generation ||
        row.root != current.root || open.root != current.root ||
        section.root != current.root || buffer.root != current.root ||
        !row.initializer || !open.invocation || row.index >= kRows.size() ||
        row.module != current.module || open.module != current.module ||
        section.module != current.module ||
        row.module_generation != current.module_generation ||
        section.module_generation != current.module_generation) return {};
    const auto& known = kRows[row.index];
    // 8D68's R31 points to this exact row's pointer/size output pair. The
    // expected labels/relative entries come from the pinned original table
    // and XUIZv3 indexes, never from a naked PNG address or supplied filename.
    const uint32_t expected_row = 0x81A8271C + row.index * 0x10;
    if (row.row_output != expected_row || open.row_output != expected_row ||
        buffer.row_output != expected_row ||
        open.initializer != row.initializer ||
        buffer.initializer != row.initializer ||
        buffer.resource_open != open.invocation ||
        row.section != known.section || row.entry != known.entry ||
        open.section != row.section || open.entry != row.entry ||
        current.name != known.section_ascii || section.name != current.name ||
        current.size != known.section_size ||
        section.address != current.address || section.size != current.size ||
        !current.address ||
        uint64_t(current.address) + current.size > 0x100000000ull ||
        !Nonnegative(open.result) || !Nonnegative(section.result) ||
        !Nonnegative(buffer.result) || !open.stream || (open.stream & 3) ||
        buffer.stream != open.stream || buffer.size != known.entry_size ||
        uint64_t(current.address) + known.entry_offset != buffer.address ||
        uint64_t(buffer.address) + buffer.size >
            uint64_t(current.address) + current.size) return {};
    return NativeDefaultImageRow(current.root, current.module,
        current.module_generation, row.index, current.address, current.size,
        buffer.address, buffer.size);
  }

  std::optional<NativeSectionImageSource> AtOriginalFallback(
      const NativeDefaultFallbackObservation& actual) const noexcept {
    // 8E10 returns exactly zero only when the original selected table row has
    // a non-null pointer. Current module ownership is supplied independently;
    // this scalar row value never keeps a former root/module alive.
    if (actual.root != root_ || actual.current_module != module_ ||
        actual.current_module_generation != module_generation_ ||
        actual.index != index_ || actual.result != 0 ||
        !actual.provider_birth || !actual.source ||
        actual.address != address_ || actual.size != size_) return {};
    NativeSectionImageSource value{root_, actual.source, actual.provider_birth,
        actual.version, module_generation_, module_, section_, section_size_,
        address_, size_};
    return value.valid() ? std::optional(value) : std::nullopt;
  }
  uint32_t index() const noexcept { return index_; }

 private:
  struct KnownRow {
    std::u16string_view section, entry;
    std::string_view section_ascii;
    uint32_t section_size, entry_offset, entry_size;
  };
  inline static constexpr std::array<KnownRow, 3> kRows{{
      {u"xam", u"defaultsystemimage.png", "xam", 0xEA39, 0x281F, 522},
      {u"xam", u"dashicon.png", "xam", 0xEA39, 0x3893, 7479},
      {u"shrdres", u"ico_64x_licensestore.png", "shrdres", 0x36AA7,
       0x3546, 2078}}};
  static bool Nonnegative(uint32_t result) noexcept {
    return !(result & 0x80000000u);
  }
  NativeDefaultImageRow(NativeProfileIdentityTasks::Root root, uint32_t module,
      uint64_t generation, uint32_t index, uint32_t section,
      uint32_t section_size, uint32_t address, uint32_t size) noexcept
      : root_(root), module_(module), module_generation_(generation),
        index_(index), section_(section), section_size_(section_size),
        address_(address), size_(size) {}
  NativeProfileIdentityTasks::Root root_;
  uint32_t module_;
  uint64_t module_generation_;
  uint32_t index_, section_, section_size_, address_, size_;
};
}  // namespace xe::kernel::identity_detail
