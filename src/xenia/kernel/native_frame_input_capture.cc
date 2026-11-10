/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_frame_input_capture.h"
#include <algorithm>
#include <cstring>
#include "xenia/base/mutex.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
bool Valid(memory::PhysicalRange r) {
  return r.length && r.length <= gpu::NativeFrameInputSnapshot::kMaxReadBytes &&
         r.address < 0x20000000 && r.length <= 0x20000000 - r.address;
}
bool Overlap(memory::PhysicalRange a, memory::PhysicalRange b) {
  return uint64_t(a.address) < uint64_t(b.address) + b.length &&
         uint64_t(b.address) < uint64_t(a.address) + a.length;
}
}  // namespace
NativeFrameInputCapture::NativeFrameInputCapture(
    std::shared_ptr<NativeFrameOwnerSession> session, Memory* memory,
    NativeFrameOwnerSession::ThreadIdentity thread,
    std::shared_ptr<const NativeFrameSource> source,
    std::shared_ptr<const gpu::NativeFrameCommandPlan> plan,
    memory::PhysicalRange primary, memory::PhysicalRange output)
    : session_(std::move(session)),
      memory_(memory),
      thread_(thread),
      source_(std::move(source)),
      plan_(std::move(plan)),
      primary_(primary),
      output_(output) {}
NativeFrameInputCapture::~NativeFrameInputCapture() {
  session_->EndOperation(operation_);
}
bool NativeFrameInputCapture::Current() const {
  if (!session_->MatchesThread(thread_)) {
    return false;
  }
  std::lock_guard lock(session_->mutex_);
  return operation_ &&
         session_->phase_ == NativeFrameOwnerSession::Phase::kFrozen &&
         session_->active_operation_ == operation_ &&
         session_->SourceMatches(source_) && !session_->submission_attempted_ &&
         !session_->active_preparation_;
}
bool NativeFrameInputCapture::Capture(memory::PhysicalRange range) {
  std::lock_guard guard(mutex_);
  // Imported pixels never satisfy native-owned geometry/shader capture.
  if (failed_ || !Current() || !Valid(range) || Overlap(range, output_) ||
      (source_->title_input() &&
       Overlap(range, source_->title_input()->range()))) {
    failed_ = true;
    return false;
  }
  if (gpu::NativeFrameInputSnapshot::CoversBlocks(blocks_, range)) {
    return true;
  }
  std::vector<gpu::NativeFrameInputSnapshot::Block> added;
  uint64_t cursor = range.address, end = cursor + range.length, bytes = 0;
  for (const auto& block : blocks_) {
    const uint64_t stop = uint64_t(block.range.address) + block.range.length;
    if (stop <= cursor) {
      continue;
    }
    if (block.range.address >= end) {
      break;
    }
    if (block.range.address > cursor) {
      const uint32_t length = uint32_t(block.range.address - cursor);
      added.push_back(
          {{uint32_t(cursor), length}, std::vector<uint8_t>(length)});
      bytes += length;
    }
    cursor = std::min(end, stop);
    if (cursor == end) {
      break;
    }
  }
  if (cursor < end) {
    const uint32_t length = uint32_t(end - cursor);
    added.push_back({{uint32_t(cursor), length}, std::vector<uint8_t>(length)});
    bytes += length;
  }
  if (blocks_.size() + added.size() >
          gpu::NativeFrameInputSnapshot::kMaxBlocks ||
      (source_->title_input() &&
       source_->title_input()->bytes().size() >
           gpu::NativeFrameInputSnapshot::kMaxBytes - byte_size_) ||
      bytes > gpu::NativeFrameInputSnapshot::kMaxBytes - byte_size_ -
                  (source_->title_input()
                       ? source_->title_input()->bytes().size()
                       : 0)) {
    failed_ = true;
    return false;
  }
  // Acquire the real owner pin before physical reads. Attach to the Session
  // before any copy so exceptions/revocation cannot discard backing ownership.
  auto pin = memory_->PinOwnedPhysicalRanges(
      session_->owner(), std::span<const memory::PhysicalRange>(&range, 1));
  if (!pin) {
    failed_ = true;
    return false;
  }
  {
    std::lock_guard lock(session_->mutex_);
    if (session_->phase_ != NativeFrameOwnerSession::Phase::kFrozen ||
        session_->active_operation_ != operation_ ||
        !session_->SourceMatches(source_) || session_->submission_attempted_ ||
        session_->active_preparation_ || session_->pins_.size() >= 512) {
      failed_ = true;
      return false;
    }
    session_->pins_.push_back(pin.pin);
  }
  blocks_.reserve(blocks_.size() + added.size());
  {
    auto lock = global_critical_region::AcquireDirect();
    auto* heap = memory_->GetPhysicalHeap();
    for (auto& block : added) {
      if (!heap || heap->QueryRangeAccess(
                       block.range.address,
                       block.range.address + block.range.length - 1) ==
                       memory::PageAccess::kNoAccess) {
        failed_ = true;
        return false;
      }
      std::memcpy(block.bytes.data(),
                  memory_->TranslatePhysical(block.range.address),
                  block.bytes.size());
    }
  }
  if (!Current()) {
    failed_ = true;
    return false;
  }
  for (auto& block : added) {
    blocks_.push_back(std::move(block));
  }
  std::sort(blocks_.begin(), blocks_.end(), [](const auto& a, const auto& b) {
    return a.range.address < b.range.address;
  });
  byte_size_ += uint32_t(bytes);
  return true;
}
bool NativeFrameInputCapture::Read(memory::PhysicalRange range,
                                   std::span<uint8_t> out) const {
  std::lock_guard guard(mutex_);
  return !failed_ && Current() &&
         gpu::NativeFrameInputSnapshot::ReadBlocks(blocks_, range, out);
}
std::shared_ptr<const gpu::NativeFrameInputSnapshot>
NativeFrameInputCapture::Seal() {
  std::lock_guard guard(mutex_);
  if (failed_ || !Current()) {
    return {};
  }
  auto snapshot = std::shared_ptr<const gpu::NativeFrameInputSnapshot>(
      new gpu::NativeFrameInputSnapshot(
          {session_->owner(), source_->serial()}, plan_, primary_, output_,
          std::move(blocks_),
          byte_size_ + (source_->title_input()
                            ? uint32_t(source_->title_input()->bytes().size())
                            : 0),
          source_->title_input()));
  if (!snapshot->valid() || !Current()) {
    failed_ = true;
    return {};
  }
  session_->EndOperation(operation_);
  operation_ = 0;
  return snapshot;
}
}  // namespace xe::kernel
