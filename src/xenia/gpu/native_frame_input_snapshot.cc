/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_frame_input_snapshot.h"
#include <algorithm>
#include <array>
#include <cstring>
namespace xe::gpu {
namespace {
bool Valid(memory::PhysicalRange r) {
  return r.length && r.length <= NativeFrameInputSnapshot::kMaxReadBytes &&
         r.address < 0x20000000 && r.length <= 0x20000000 - r.address;
}
bool Overlap(memory::PhysicalRange a, memory::PhysicalRange b) {
  return uint64_t(a.address) < uint64_t(b.address) + b.length &&
         uint64_t(b.address) < uint64_t(a.address) + a.length;
}
}  // namespace
bool NativeFrameInputSnapshot::CoversBlocks(
    const std::vector<Block>& blocks, memory::PhysicalRange range) noexcept {
  if (!Valid(range)) {
    return false;
  }
  uint64_t cursor = range.address, end = cursor + range.length;
  for (const auto& block : blocks) {
    const uint64_t stop = uint64_t(block.range.address) + block.range.length;
    if (stop <= cursor) {
      continue;
    }
    if (block.range.address > cursor ||
        block.bytes.size() != block.range.length) {
      return false;
    }
    cursor = std::min(stop, end);
    if (cursor == end) {
      return true;
    }
  }
  return false;
}
bool NativeFrameInputSnapshot::ReadBlocks(const std::vector<Block>& blocks,
                                          memory::PhysicalRange range,
                                          std::span<uint8_t> out) noexcept {
  if (!out.data() || out.size() != range.length ||
      !CoversBlocks(blocks, range)) {
    return false;
  }
  uint64_t cursor = range.address, end = cursor + range.length;
  for (const auto& block : blocks) {
    const uint64_t stop = uint64_t(block.range.address) + block.range.length;
    if (stop <= cursor) {
      continue;
    }
    const auto length = size_t(std::min(stop, end) - cursor);
    std::memcpy(out.data() + size_t(cursor - range.address),
                block.bytes.data() + size_t(cursor - block.range.address),
                length);
    cursor += length;
    if (cursor == end) {
      return true;
    }
  }
  return false;
}
bool NativeFrameInputSnapshot::Covers(
    memory::PhysicalRange range) const noexcept {
  return CoversBlocks(blocks_, range) ||
         (title_input_ && title_input_->Covers(range));
}
bool NativeFrameInputSnapshot::Read(memory::PhysicalRange range,
                                    std::span<uint8_t> out) const noexcept {
  if (title_input_ && title_input_->Covers(range)) {
    return title_input_->Read(range, out);
  }
  return ReadBlocks(blocks_, range, out);
}
bool NativeFrameInputSnapshot::valid() const noexcept {
  if (!identity_.valid() || !plan_ || !Valid(primary_) ||
      (!plan_->is_overlay_control() ? !Valid(output_)
                                    : (output_.address || output_.length)) ||
      primary_.length != plan_->words().size() * 4 ||
      Overlap(primary_, output_) || blocks_.empty() ||
      blocks_.size() > kMaxBlocks || byte_size_ > kMaxBytes) {
    return false;
  }
  if (title_input_ != plan_->title_input() ||
      (title_input_ &&
       (!title_input_->valid() || Overlap(title_input_->range(), output_) ||
        Overlap(title_input_->range(), primary_)))) {
    return false;
  }
  uint64_t total = title_input_ ? title_input_->bytes().size() : 0, end = 0;
  for (const auto& block : blocks_) {
    if (!Valid(block.range) || block.bytes.size() != block.range.length ||
        block.range.address < end || Overlap(block.range, output_) ||
        (title_input_ && Overlap(block.range, title_input_->range()))) {
      return false;
    }
    end = uint64_t(block.range.address) + block.range.length;
    total += block.range.length;
  }
  if (total != byte_size_ || !Covers(primary_)) {
    return false;
  }
  // No allocation in validation: compare every immutable primary DWORD.
  std::array<uint8_t, 4> word;
  for (size_t i = 0; i < plan_->words().size(); ++i) {
    if (!Read({primary_.address + uint32_t(i * 4), 4}, word)) {
      return false;
    }
    const uint32_t value = uint32_t(word[0]) << 24 | uint32_t(word[1]) << 16 |
                           uint32_t(word[2]) << 8 | word[3];
    if (value != plan_->words()[i]) {
      return false;
    }
  }
  return true;
}
}  // namespace xe::gpu
