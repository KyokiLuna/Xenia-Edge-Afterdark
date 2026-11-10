/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_FRAME_INPUT_CAPTURE_H_
#define XENIA_KERNEL_NATIVE_FRAME_INPUT_CAPTURE_H_
#include <memory>
#include <mutex>
#include "xenia/gpu/native_frame_input_snapshot.h"
#include "xenia/kernel/native_frame_owner.h"
namespace xe::kernel {
// Only Session constructs a capture while reserving its exact Frozen Source.
// Captures new physical bytes once. An overlapping later read reuses the old
// bytes; it never combines two observations of the same address.
class NativeFrameInputCapture {
 public:
  ~NativeFrameInputCapture();
  bool Capture(memory::PhysicalRange);
  bool Read(memory::PhysicalRange, std::span<uint8_t>) const;
  std::shared_ptr<const gpu::NativeFrameInputSnapshot> Seal();

 private:
  friend class NativeFrameOwnerSession;
  NativeFrameInputCapture(std::shared_ptr<NativeFrameOwnerSession>, Memory*,
                          NativeFrameOwnerSession::ThreadIdentity,
                          std::shared_ptr<const NativeFrameSource>,
                          std::shared_ptr<const gpu::NativeFrameCommandPlan>,
                          memory::PhysicalRange primary,
                          memory::PhysicalRange output);
  bool Current() const;
  const std::shared_ptr<NativeFrameOwnerSession> session_;
  Memory* const memory_;
  const NativeFrameOwnerSession::ThreadIdentity thread_;
  const std::shared_ptr<const NativeFrameSource> source_;
  const std::shared_ptr<const gpu::NativeFrameCommandPlan> plan_;
  const memory::PhysicalRange primary_, output_;
  mutable std::mutex mutex_;
  uint64_t operation_ = 0;
  uint32_t byte_size_ = 0;
  bool failed_ = false;
  std::vector<gpu::NativeFrameInputSnapshot::Block> blocks_;
};
}  // namespace xe::kernel
#endif
