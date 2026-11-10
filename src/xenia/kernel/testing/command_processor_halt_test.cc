/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <memory>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/memory.h"

namespace xe::gpu::test {
namespace {

// Exercise the actual common PM4 decoder and guest Memory. No host GPU device
// or guest GPU object is fabricated. A backend wait hook injects the terminal
// failure at the same common halt API used by native execution failures.
class HostGraphicsSystem final : public GraphicsSystem {
 public:
  explicit HostGraphicsSystem(Memory& memory) { memory_ = &memory; }
  ~HostGraphicsSystem() override {
    memory::DeallocFixed(register_file_, 0, memory::DeallocationType::kRelease);
    register_file_ = nullptr;
  }
  std::string name() const override { return "CP halt test"; }

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return {};
  }
};

class HostCommandProcessor final : public CommandProcessor {
 public:
  explicit HostCommandProcessor(HostGraphicsSystem& graphics)
      : CommandProcessor(&graphics, nullptr) {}
  void TracePlaybackWroteMemory(uint32_t, uint32_t) override {}
  void RestoreEdramSnapshot(const void*) override {}
  void Halt() { HaltGuestCommandProcessing(); }
  bool halted() const { return IsGuestCommandProcessingHalted(); }
  bool running() const { return worker_running_.load(); }
  void DecodeDirect(uint32_t address, uint32_t dwords) {
    CommandProcessor::ExecutePacket(address, dwords);
  }
  void DecodeIndirect(uint32_t address, uint32_t dwords) {
    ExecuteIndirectBuffer(address, dwords);
  }
  uint32_t DecodePrimary(uint32_t address, uint32_t dwords,
                         uint32_t writeback) {
    primary_buffer_ptr_ = address;
    primary_buffer_size_ = 0x1000;
    read_ptr_update_freq_ = 1;
    read_ptr_writeback_ptr_ = writeback;
    return ExecutePrimaryBuffer(0, dwords);
  }
  void SetSavedReader(uint32_t address) {
    reader_ = RingBuffer(memory_->TranslatePhysical(address), 0x100);
    reader_.set_read_offset(8);
    reader_.set_write_offset(16);
  }
  uint32_t saved_reader_offset() const { return reader_.read_offset(); }
  bool DecodeOne() { return CommandProcessor::ExecutePacket(); }
  void WriteFence(uint32_t address, uint32_t value) {
    WriteGuestFence(memory_->TranslatePhysical(address), address, value);
  }
  void DispatchInterrupt() { DispatchGuestInterrupt(1); }
  NativeFrameExecutionResult DefaultNativeResult() {
    NativeFrameExecutionResult result;
    result.status = NativeFrameExecutionResult::Status::kRejected;
    QueueNativeFrameAtSafeBoundary({}, [&](auto value) {
      ++native_callbacks;
      result = value;
    });
    return result;
  }
  unsigned waits = 0;
  unsigned native_callbacks = 0;

 private:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  void PrepareForWait() override {
    ++waits;
    HaltGuestCommandProcessing();
  }
};

class HostMemory final : public Memory {
 public:
  HostMemory() { REQUIRE(Initialize()); }
};

class Fixture {
 public:
  Fixture() : graphics(memory), cp(graphics) {
    auto* heap = memory.LookupHeap(0xE1000000);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(0xE1000000, 0x4000, 0x1000,
                             kMemoryAllocationReserve | kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    primary = memory.GetPhysicalAddress(0xE1000000);
    indirect = primary + 0x1000;
    output = primary + 0x2000;
    writeback = primary + 0x3000;
    Store(output, 0);
    Store(writeback, 0xA5A5A5A5);
    cp.SetSavedReader(output + 0x100);
  }
  void Store(uint32_t address, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslatePhysical(address), value);
  }
  uint32_t Load(uint32_t address) const {
    return xe::load_and_swap<uint32_t>(memory.TranslatePhysical(address));
  }
  void StoreWords(uint32_t address, const std::vector<uint32_t>& words) {
    for (auto word : words) {
      Store(address, word);
      address += 4;
    }
  }
  std::vector<uint32_t> WaitThenWrite() const {
    return {xenos::MakePacketType3(xenos::PM4_WAIT_REG_MEM, 5),
            0x13,
            output,
            1,
            UINT32_MAX,
            0x100,
            xenos::MakePacketType3(xenos::PM4_MEM_WRITE, 2),
            output,
            0x12345678};
  }
  HostMemory memory;
  HostGraphicsSystem graphics;
  HostCommandProcessor cp;
  uint32_t primary = 0, indirect = 0, output = 0, writeback = 0;
};

TEST_CASE("Terminal CP halt stops the real direct PM4 decoder",
          "[graphics][cp_halt]") {
  Fixture f;
  const auto words = f.WaitThenWrite();
  f.StoreWords(f.primary, words);
  f.cp.DecodeDirect(f.primary, uint32_t(words.size()));
  CHECK(f.cp.halted());
  CHECK_FALSE(f.cp.running());
  CHECK(f.cp.waits == 1);
  CHECK(f.Load(f.output) == 0);
  CHECK(f.cp.saved_reader_offset() == 8);
  // Already halted entry points do not even translate this invalid address.
  f.cp.DecodeDirect(UINT32_MAX, UINT32_MAX);
  f.cp.DecodeIndirect(UINT32_MAX, UINT32_MAX);
  CHECK_FALSE(f.cp.DecodeOne());
  CHECK(f.cp.saved_reader_offset() == 8);
}

TEST_CASE("Common PM4 memory writes retain their ordinary behavior",
          "[graphics][cp_halt]") {
  Fixture f;
  const std::vector<uint32_t> words{
      xenos::MakePacketType3(xenos::PM4_MEM_WRITE, 2), f.output, 0x12345678};
  f.StoreWords(f.primary, words);
  f.cp.DecodeDirect(f.primary, uint32_t(words.size()));
  CHECK(xe::load<uint32_t>(f.memory.TranslatePhysical(f.output)) == 0x12345678);
  CHECK_FALSE(f.cp.halted());
  CHECK(f.cp.running());
  CHECK(f.cp.saved_reader_offset() == 8);
}

TEST_CASE("Terminal CP halt suppresses primary ring acknowledgement",
          "[graphics][cp_halt]") {
  Fixture f;
  const auto words = f.WaitThenWrite();
  f.StoreWords(f.primary, words);
  const auto next =
      f.cp.DecodePrimary(f.primary, uint32_t(words.size()), f.writeback);
  CHECK(f.cp.halted());
  // The failed wait was decoded; the following write was not.
  CHECK(next == 6);
  CHECK(next != words.size());
  CHECK(f.Load(f.output) == 0);
  CHECK(f.Load(f.writeback) == 0xA5A5A5A5);
  CHECK(f.cp.saved_reader_offset() == 8);
}

TEST_CASE("Terminal CP halt unwinds nested indirect readers without resuming",
          "[graphics][cp_halt]") {
  Fixture f;
  const auto nested = f.WaitThenWrite();
  f.StoreWords(f.indirect, nested);
  const std::vector<uint32_t> primary{
      xenos::MakePacketType3(xenos::PM4_INDIRECT_BUFFER, 2),
      f.indirect,
      uint32_t(nested.size()),
      xenos::MakePacketType3(xenos::PM4_MEM_WRITE, 2),
      f.output,
      0x87654321};
  f.StoreWords(f.primary, primary);
  const auto next =
      f.cp.DecodePrimary(f.primary, uint32_t(primary.size()), f.writeback);
  CHECK(f.cp.halted());
  CHECK(f.cp.waits == 1);
  CHECK(next == 3);
  CHECK(f.Load(f.output) == 0);
  CHECK(f.Load(f.writeback) == 0xA5A5A5A5);
  CHECK(f.cp.saved_reader_offset() == 8);
}

TEST_CASE("Terminal CP halt suppresses guest fence and interrupt effects",
          "[graphics][cp_halt]") {
  Fixture f;
  f.cp.Halt();
  f.cp.WriteFence(f.output, 0x12345678);
  f.cp.DispatchInterrupt();
  CHECK(f.Load(f.output) == 0);
  CHECK(f.cp.halted());
  CHECK_FALSE(f.cp.running());
}

TEST_CASE("Default native safe-boundary hook remains unsupported",
          "[graphics][cp_halt]") {
  Fixture f;
  CHECK(f.cp.DefaultNativeResult().status ==
        NativeFrameExecutionResult::Status::kUnsupported);
  CHECK(f.cp.native_callbacks == 1);
  CHECK_FALSE(f.cp.halted());
}

TEST_CASE("Native controls reject before a real worker exists without queuing",
          "[graphics][cp_halt][native_noswap]") {
  Fixture f;
  const auto capacity = f.cp.GetNativeSystemFrameCapacity();
  CHECK(capacity.state == NativeSystemFrameCapacity::State::kUnsupported);
  CHECK_FALSE(capacity.supported);
  CHECK_FALSE(capacity.can_queue());
  CHECK_FALSE(f.cp.supports_native_system_frames());
  CHECK(capacity.backend_epoch == 0);
  CHECK(capacity.backend_stop_proof.backend_epoch() == 0);
  CHECK_FALSE(capacity.backend_stop_proof.stopped());
  unsigned executes = 0, reads = 0, observations = 0;
  f.cp.ExecuteNativeFrameAsync({}, [&](const auto& result) {
    ++executes;
    CHECK(result.status == NativeFrameExecutionResult::Status::kRejected);
    CHECK(result.failing_stage ==
          NativeFrameExecutionResult::Stage::kAdmission);
    // Reentrant controls must reject outside the admission lock, never wait
    // for a missing worker or recursively execute a backend operation.
    f.cp.ReadCompletedNativeFrameAsync({}, [&](const auto& read) {
      ++reads;
      CHECK(read.status == NativeFrameReadbackResult::Status::kRejected);
    });
    f.cp.CaptureNativeFramePresentationAsync({}, [&](const auto& observed) {
      ++observations;
      CHECK(observed.status ==
            NativeFramePresentationStatus::Status::kUnsupported);
    });
  });
  CHECK(executes == 1);
  CHECK(reads == 1);
  CHECK(observations == 1);
  CHECK(f.cp.native_callbacks == 0);
  CHECK(f.cp.waits == 0);
  CHECK(f.cp.saved_reader_offset() == 8);
  CHECK(f.Load(f.output) == 0);
  CHECK(f.Load(f.writeback) == 0xA5A5A5A5);
  CHECK_FALSE(f.cp.halted());
  f.cp.Shutdown();
  CHECK(executes == 1);
  CHECK(reads == 1);
  CHECK(observations == 1);
}

TEST_CASE("Sealed native controls complete once with actual base stop metadata",
          "[graphics][cp_halt][native_noswap]") {
  Fixture f;
  f.cp.Shutdown();
  const auto capacity = f.cp.GetNativeSystemFrameCapacity();
  CHECK(capacity.state == NativeSystemFrameCapacity::State::kStopping);
  CHECK_FALSE(capacity.supported);
  CHECK_FALSE(capacity.can_queue());
  CHECK_FALSE(f.cp.supports_native_system_frames());
  CHECK(capacity.backend_epoch == 0);
  // Stopping CPU ingress cannot invent any backend/GPU stopped evidence.
  CHECK_FALSE(capacity.backend_stop_proof.stopped());
  unsigned executes = 0, reads = 0, observations = 0;
  f.cp.ExecuteNativeFrameAsync({}, [&](const auto& result) {
    ++executes;
    CHECK(result.status == NativeFrameExecutionResult::Status::kRejected);
  });
  f.cp.ReadCompletedNativeFrameAsync({}, [&](const auto& read) {
    ++reads;
    CHECK(read.status == NativeFrameReadbackResult::Status::kRejected);
  });
  f.cp.CaptureNativeFramePresentationAsync({}, [&](const auto& observed) {
    ++observations;
    CHECK(observed.status ==
          NativeFramePresentationStatus::Status::kUnsupported);
  });
  f.cp.Shutdown();
  CHECK(executes == 1);
  CHECK(reads == 1);
  CHECK(observations == 1);
  CHECK(f.cp.native_callbacks == 0);
  CHECK(f.cp.waits == 0);
  CHECK_FALSE(f.cp.running());
  CHECK_FALSE(f.cp.halted());
  CHECK(f.cp.saved_reader_offset() == 8);
  CHECK(f.Load(f.output) == 0);
  CHECK(f.Load(f.writeback) == 0xA5A5A5A5);
}

}  // namespace
}  // namespace xe::gpu::test
