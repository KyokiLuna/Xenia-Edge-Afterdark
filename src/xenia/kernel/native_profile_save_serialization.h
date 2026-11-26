/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
#include <span>
#include <system_error>
namespace xe::kernel::save_detail {
// Pure validation only, NOT a writer/source/completion authority. The actual
// native SAVE owner must supply the exact owned sealed staged file, the real
// native stream's complete header/index buffer captured under its actual DB
// use, and the genuine pending command's intended typed GPD plan. No filename,
// caller-filled done flag, parseable layout or this comparison grants writes.
//
// Native table/data placement may differ from the host planner. Preserve the
// exact native bytes after proving all typed keys and opaque record payloads
// equal the allowed plan, including unknown namespaces and zero-size records.
// The original native buffer must match the entire staged table prefix; a
// host reserialization may never leave cached native offsets pointing at a
// different raw layout. Orphan/slack bytes remain in the exact staged image;
// the original full before image stays in genuine journal/recovery storage.
std::error_code ValidateNativeGpdSerialization(
    std::span<const uint8_t> intended_typed_plan,
    std::span<const uint8_t> sealed_native_file,
    std::span<const uint8_t> actual_native_header_index);

// Original25AC0→25248 ancestry distinguishes a terminal NEW-record insert
// header from helper activity while replacing an existing record. Scalar ABI
// classification only: it grants no source/DB/File/publication authority.
enum class ImageWriteCheckpoint {kNone,kInPlace,kHeader};
constexpr ImageWriteCheckpoint ClassifyImageWriteCheckpoint(
    uint32_t edit_function,uint32_t edit_sp,uint32_t stream_sp,uint32_t stream_lr,
    bool r23_zero,bool insert_live,uint32_t insert_sp,uint32_t insert_lr) noexcept {
  if(edit_function==0x81925110 && edit_sp>=0x90 && stream_sp==edit_sp-0x90 && stream_lr==0x81925220)
    return ImageWriteCheckpoint::kHeader;
  if(edit_function!=0x81925AC0 || edit_sp<0xC0)return ImageWriteCheckpoint::kNone;
  if(stream_sp==edit_sp-0xC0) {
    if(stream_lr==0x81925C5C)return ImageWriteCheckpoint::kHeader;
    if(stream_lr==0x81925BC4 && r23_zero)return ImageWriteCheckpoint::kInPlace;
  }
  // NEW insert call25B40 returns25B44; resized replacement's25C18 returns25C1C
  // with autosave temporarily cleared, and does not own this terminal header.
  if(insert_live && insert_sp==edit_sp-0xC0 && insert_sp>=0xB0 &&
      insert_lr==0x81925B44 && stream_sp==insert_sp-0xB0 && stream_lr==0x81925374)
    return ImageWriteCheckpoint::kHeader;
  return ImageWriteCheckpoint::kNone;
}
}  // namespace xe::kernel::save_detail
