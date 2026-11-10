/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_FRAME_INPUT_SNAPSHOT_H_
#define XENIA_GPU_NATIVE_FRAME_INPUT_SNAPSHOT_H_
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include "xenia/base/physical_range_leases.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/gpu/native_frame_command_plan.h"
namespace xe::kernel {
class NativeFrameInputCapture;
}
namespace xe::gpu {
enum class NativeFrameInputProvenance { kLiveOwnerBacking, kImmutableSnapshot };
// A CPU byte view, not GPU execution/visibility or a claim that guest writers
// stopped. It owns every accepted byte once; later consumers cannot reread RAM.
class NativeFrameInputSnapshot {
 public:
  struct Block {
    memory::PhysicalRange range;
    std::vector<uint8_t> bytes;
  };
  static constexpr size_t kMaxBlocks = 512;
  static constexpr uint32_t kMaxBytes = 15 * 1024 * 1024;
  static constexpr uint32_t kMaxReadBytes = 4 * 1024 * 1024;
  const NativeFrameSubmissionIdentity& identity() const { return identity_; }
  const std::shared_ptr<const NativeFrameCommandPlan>& plan() const {
    return plan_;
  }
  const std::vector<Block>& blocks() const { return blocks_; }
  const memory::PhysicalRange& primary_range() const { return primary_; }
  const memory::PhysicalRange& output_range() const { return output_; }
  uint32_t byte_size() const { return byte_size_; }
  const std::shared_ptr<const NativeSystemTitleInput>& title_input() const {
    return title_input_;
  }
  bool Covers(memory::PhysicalRange range) const noexcept;
  // Failure never modifies destination, including a hole between two blocks.
  bool Read(memory::PhysicalRange range,
            std::span<uint8_t> destination) const noexcept;
  bool valid() const noexcept;

 private:
  friend class xe::kernel::NativeFrameInputCapture;
  NativeFrameInputSnapshot(
      NativeFrameSubmissionIdentity identity,
      std::shared_ptr<const NativeFrameCommandPlan> plan,
      memory::PhysicalRange primary, memory::PhysicalRange output,
      std::vector<Block> blocks, uint32_t bytes,
      std::shared_ptr<const NativeSystemTitleInput> title_input = {})
      : identity_(identity),
        plan_(std::move(plan)),
        primary_(primary),
        output_(output),
        blocks_(std::move(blocks)),
        byte_size_(bytes),
        title_input_(std::move(title_input)) {}
  static bool ReadBlocks(const std::vector<Block>& blocks,
                         memory::PhysicalRange range,
                         std::span<uint8_t> destination) noexcept;
  static bool CoversBlocks(const std::vector<Block>& blocks,
                           memory::PhysicalRange range) noexcept;
  const NativeFrameSubmissionIdentity identity_;
  const std::shared_ptr<const NativeFrameCommandPlan> plan_;
  const memory::PhysicalRange primary_, output_;
  const std::vector<Block> blocks_;
  const uint32_t byte_size_;
  const std::shared_ptr<const NativeSystemTitleInput> title_input_;
};
}  // namespace xe::gpu
#endif
