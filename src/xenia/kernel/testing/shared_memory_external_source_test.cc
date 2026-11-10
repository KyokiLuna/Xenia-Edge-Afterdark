/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/shared_memory.h"
#include "xenia/memory.h"

namespace xe::gpu::test {
namespace {
// Tests the actual SharedMemory policy and page-validity implementation using
// real Memory. Uploads below only record host metadata; no GPU execution or
// completion is represented by this fixture.
class ResidencyRecorder final : public SharedMemory {
 public:
  explicit ResidencyRecorder(Memory& memory) : SharedMemory(memory) {}
  bool Initialize(ExternalSource* source = nullptr) {
    SetExternalSource(source);
    return InitializeCommon(false);
  }
  uint32_t upload_calls = 0;

 protected:
  bool UploadRanges(const std::pair<uint32_t, uint32_t>* ranges,
                    uint32_t count) override {
    ++upload_calls;
    for (uint32_t i = 0; i < count; ++i) {
      MakeRangeValid(ranges[i].first << page_size_log2(),
                     ranges[i].second << page_size_log2(), false);
    }
    return true;
  }
};

bool Contains(memory::PhysicalRange outer, uint32_t start, size_t size) {
  return start >= outer.address &&
         uint64_t(start) + size <= uint64_t(outer.address) + outer.length;
}
class OwnedSource final : public SharedMemory::ExternalSource {
 public:
  memory::PhysicalRange input{}, output{}, written{};
  std::vector<uint8_t> bytes;
  bool CoversRead(uint32_t start, uint32_t size) const noexcept override {
    return !size || Contains(input, start, size) ||
           (written.length && Contains(written, start, size));
  }
  bool CoversWrite(uint32_t start, uint32_t size) const noexcept override {
    return !size || Contains(output, start, size);
  }
  bool Read(uint32_t start,
            std::span<uint8_t> destination) const noexcept override {
    if (!Contains(input, start, destination.size())) {
      return false;
    }
    std::memcpy(destination.data(), bytes.data() + start - input.address,
                destination.size());
    return true;
  }
  void GpuWritten(uint32_t start, uint32_t length) noexcept override {
    if (CoversWrite(start, length)) {
      written = {start, length};
    }
  }
};
struct Fixture {
  Memory memory;
  OwnedSource source;
  ResidencyRecorder detached{memory};
  ResidencyRecorder title{memory};
  uint32_t physical;
  Fixture() {
    REQUIRE(memory.Initialize());
    auto* heap = memory.LookupHeap(0xE1000000);
    REQUIRE(heap->AllocFixed(0xE1000000, 0x2000, 0x1000,
                             kMemoryAllocationReserve | kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    physical = memory.GetPhysicalAddress(0xE1000000);
    REQUIRE(physical != UINT32_MAX);
    source.input = {physical + 0x20, 0x80};
    source.output = {physical + 0x1000, 0x100};
    source.bytes.assign(source.input.length, 0xA5);
    std::memset(memory.TranslatePhysical(physical), 0x31, 0x2000);
    REQUIRE(detached.Initialize(&source));
    REQUIRE(title.Initialize());
  }
};
TEST_CASE("External SharedMemory CPU reads use owned bytes after guest changes",
          "[gpu][shared_memory][snapshot]") {
  Fixture f;
  std::array<uint8_t, 16> sealed{}, live{};
  auto* guest = f.memory.TranslatePhysical<uint8_t*>(f.source.input.address);
  std::fill_n(guest, sealed.size(), 0x7C);
  REQUIRE(f.detached.ReadRangeForCpu(f.source.input.address, sealed));
  REQUIRE(f.title.ReadRangeForCpu(f.source.input.address, live));
  CHECK(std::all_of(sealed.begin(), sealed.end(),
                    [](auto b) { return b == 0xA5; }));
  CHECK(
      std::all_of(live.begin(), live.end(), [](auto b) { return b == 0x7C; }));
}
TEST_CASE("Resident page padding cannot grant uncovered snapshot reads",
          "[gpu][shared_memory][snapshot]") {
  Fixture f;
  REQUIRE(
      f.detached.RequestRange(f.source.input.address, f.source.input.length));
  CHECK(f.detached.upload_calls == 1);
  CHECK(f.detached.IsRangeValid(f.source.input.address, f.source.input.length));
  CHECK_FALSE(f.detached.RequestRange(f.physical, 4));
  CHECK_FALSE(f.detached.IsRangeValid(f.physical, 4));
  CHECK_FALSE(f.detached.RequestRange(f.source.input.address + 0x7C, 8));
  CHECK(f.detached.upload_calls == 1);
}
TEST_CASE("GPU output initialization does not grant an input read",
          "[gpu][shared_memory][snapshot]") {
  Fixture f;
  const auto output = f.source.output;
  REQUIRE(f.detached.RequestRangeForGpuWrite(output.address, output.length));
  CHECK_FALSE(f.detached.RequestRange(output.address, output.length));
  CHECK_FALSE(f.detached.IsRangeValid(output.address, output.length));
  CHECK_FALSE(f.detached.RequestRangeForGpuWrite(output.address - 4, 8));
  f.detached.RangeWrittenByGpu(output.address + 16, 32);
  CHECK(f.detached.RequestRange(output.address + 16, 32));
  CHECK_FALSE(f.detached.RequestRange(output.address + 12, 36));
  std::array<uint8_t, 8> cpu;
  cpu.fill(0xDC);
  CHECK_FALSE(f.detached.ReadRangeForCpu(output.address + 16, cpu));
  CHECK(std::all_of(cpu.begin(), cpu.end(), [](auto b) { return b == 0xDC; }));
}
TEST_CASE("Snapshot and Title SharedMemory keep separate page validity",
          "[gpu][shared_memory][snapshot]") {
  Fixture f;
  const auto input = f.source.input;
  REQUIRE(f.detached.RequestRange(input.address, input.length));
  REQUIRE(f.title.RequestRange(input.address, input.length));
  f.detached.MemoryInvalidationCallback(input.address, input.length, true);
  CHECK_FALSE(f.detached.IsRangeValid(input.address, input.length));
  CHECK(f.title.IsRangeValid(input.address, input.length));
  REQUIRE(f.detached.RequestRange(input.address, input.length));
  f.title.MemoryInvalidationCallback(input.address, input.length, true);
  CHECK(f.detached.IsRangeValid(input.address, input.length));
  CHECK_FALSE(f.title.IsRangeValid(input.address, input.length));
}
TEST_CASE("Denied snapshot CPU ranges leave caller storage unchanged",
          "[gpu][shared_memory][snapshot]") {
  Fixture f;
  std::array<uint8_t, 16> destination;
  destination.fill(0x67);
  CHECK_FALSE(
      f.detached.ReadRangeForCpu(f.source.input.address - 4, destination));
  CHECK_FALSE(
      f.detached.ReadRangeForCpu(SharedMemory::kBufferSize - 4, destination));
  CHECK(std::all_of(destination.begin(), destination.end(),
                    [](auto b) { return b == 0x67; }));
  CHECK(f.detached.ReadRangeForCpu(SharedMemory::kBufferSize, {}));
}
TEST_CASE(
    "New source invalidation forces actual SharedMemory upload scheduling",
    "[gpu][shared_memory][snapshot]") {
  Fixture f;
  const auto input = f.source.input;
  REQUIRE(f.detached.RequestRange(input.address, input.length));
  REQUIRE(f.detached.RequestRange(input.address, input.length));
  CHECK(f.detached.upload_calls == 1);
  f.source.bytes.assign(input.length, 0xB6);
  f.detached.MemoryInvalidationCallback(0, SharedMemory::kBufferSize, true);
  REQUIRE(f.detached.RequestRange(input.address, input.length));
  CHECK(f.detached.upload_calls == 2);
  std::array<uint8_t, 8> current;
  REQUIRE(f.detached.ReadRangeForCpu(input.address, current));
  CHECK(std::all_of(current.begin(), current.end(),
                    [](auto b) { return b == 0xB6; }));
}
}  // namespace
}  // namespace xe::gpu::test
