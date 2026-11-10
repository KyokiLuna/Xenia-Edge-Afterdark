/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/memory.h"

namespace xe::kernel::xam {
pointer_result_t XMemSet_entry(lpvoid_t destination, dword_t value,
                               dword_t count);
pointer_result_t XMemCpy_entry(lpvoid_t destination, lpvoid_t source,
                               dword_t count);
pointer_result_t XMemCpyStreaming_entry(lpvoid_t destination, lpvoid_t source,
                                        dword_t count);
}  // namespace xe::kernel::xam

namespace xe::kernel::xam::test {
namespace {

constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kDataSize = 4 * kPageSize;
constexpr uint32_t kDestination = 0x90011000;
constexpr uint32_t kSource = 0x90031000;
constexpr uint8_t kCanary = 0xA7;

class MemoryFixture {
 public:
  MemoryFixture() {
    REQUIRE(memory.Initialize());
    for (uint32_t address : {kDestination, kSource}) {
      auto* heap = memory.LookupHeap(address);
      REQUIRE(heap != nullptr);
      // Four data pages bordered by reserved, inaccessible guard pages.
      REQUIRE(heap->AllocFixed(address - kPageSize, kDataSize + 2 * kPageSize,
                               kPageSize, kMemoryAllocationReserve,
                               kMemoryProtectRead | kMemoryProtectWrite));
      REQUIRE(heap->AllocFixed(address, kDataSize, kPageSize,
                               kMemoryAllocationCommit,
                               kMemoryProtectRead | kMemoryProtectWrite));
    }
    for (uint32_t i = 0; i < kDataSize; ++i) {
      source()[i] = uint8_t((i * 37) ^ (i >> 3) ^ 0x5A);
    }
    REQUIRE(memory.LookupHeap(kSource)->Protect(kSource, kDataSize,
                                                kMemoryProtectRead));
    ResetDestination();
  }

  uint8_t* destination() { return memory.TranslateVirtual(kDestination); }
  const uint8_t* source() const {
    return memory.TranslateVirtual<const uint8_t*>(kSource);
  }
  uint8_t* source() { return memory.TranslateVirtual(kSource); }
  void ResetDestination() { std::memset(destination(), kCanary, kDataSize); }

  shim::PointerParam Pointer(uint32_t address) {
    // Exercise the actual entry points with their paired host/guest pointers
    // without constructing a CPU thread or relying on its platform address tag.
    shim::PointerParam pointer(address ? memory.TranslateVirtual(address)
                                       : nullptr);
    static_cast<shim::ParamBase<uint32_t>&>(pointer) = address;
    return pointer;
  }

  uint32_t Set(uint32_t address, uint32_t value, uint32_t count) {
    return XMemSet_entry(Pointer(address), shim::ParamBase<uint32_t>(value),
                         shim::ParamBase<uint32_t>(count));
  }

  uint32_t Copy(bool streaming, uint32_t destination, uint32_t source,
                uint32_t count) {
    auto copy = streaming ? XMemCpyStreaming_entry : XMemCpy_entry;
    return copy(Pointer(destination), Pointer(source),
                shim::ParamBase<uint32_t>(count));
  }

  void CheckCanaries(uint32_t offset, uint32_t length) {
    CHECK(std::all_of(destination(), destination() + offset,
                      [](uint8_t value) { return value == kCanary; }));
    CHECK(std::all_of(destination() + offset + length,
                      destination() + kDataSize,
                      [](uint8_t value) { return value == kCanary; }));
  }

  Memory memory;
};

TEST_CASE("XAM memory exports register callable guest ABI trampolines",
          "[xmem]") {
  MemoryFixture fixture;
  cpu::ExportResolver resolver;
  // This declaration comes from the module's export-group list, and the XAM
  // module constructor uses that same group to retain the static object.
  RegisterMemoryExports(&resolver, nullptr);
  XamModule::RegisterExportTable(&resolver);
  constexpr std::array<uint16_t, 3> ordinals = {0x438, 0x468, 0x469};
  constexpr std::array<std::string_view, 3> names = {"XMemSet", "XMemCpy",
                                                     "XMemCpyStreaming"};
  for (size_t i = 0; i < ordinals.size(); ++i) {
    CAPTURE(ordinals[i]);
    auto* entry = resolver.GetExportByOrdinal("xam.xex", ordinals[i]);
    REQUIRE(entry != nullptr);
    CHECK(std::string_view(entry->name) == names[i]);
    CHECK(entry->is_implemented());
    REQUIRE(entry->function_data.trampoline != nullptr);

    // Zero-length ABI calls need no CPU thread and must not access either
    // invalid pointer. They still decode r3/r4/r5 and store the real return.
    PPCContext context{};
    context.virtual_membase = fixture.memory.virtual_membase();
    context.r[3] = 0xFFFFFFFF;
    context.r[4] = i ? 0 : 0x12345678;
    context.r[5] = 0;
    entry->function_data.trampoline(&context);
    CHECK(static_cast<uint32_t>(context.r[3]) == 0xFFFFFFFF);
  }
}

TEST_CASE("XMemSet fills exactly a byte count with the low value byte",
          "[xmem]") {
  MemoryFixture fixture;
  constexpr std::array<uint32_t, 6> values = {
      0, 0x100, 0x11223344, 0xFFFFFF80, 0xFFFFFFFF, 0xA5};
  constexpr std::array<uint32_t, 12> lengths = {0,   1,   3,   15,  16,  127,
                                                128, 129, 255, 256, 257, 4099};
  for (uint32_t value : values) {
    for (uint32_t length : lengths) {
      constexpr uint32_t offset = kPageSize - 3;
      CAPTURE(value, length);
      fixture.ResetDestination();
      CHECK(fixture.Set(kDestination + offset, value, length) ==
            kDestination + offset);
      CHECK(std::all_of(
          fixture.destination() + offset,
          fixture.destination() + offset + length,
          [value](uint8_t byte) { return byte == uint8_t(value); }));
      fixture.CheckCanaries(offset, length);
    }
  }
}

TEST_CASE("Both XMem copy exports preserve arbitrary unaligned byte ranges",
          "[xmem]") {
  const bool streaming = GENERATE(false, true);
  MemoryFixture fixture;
  constexpr std::array<uint32_t, 6> offsets = {0, 1, 3, 7, 15, 31};
  constexpr std::array<uint32_t, 12> lengths = {0,   1,   3,   15,  16,  127,
                                                128, 129, 255, 256, 257, 4099};
  for (uint32_t offset : offsets) {
    for (uint32_t length : lengths) {
      const uint32_t source_offset = kPageSize - 2 + offset;
      const uint32_t destination_offset = kPageSize - 1 + offset;
      CAPTURE(streaming, offset, length);
      fixture.ResetDestination();
      CHECK(fixture.Copy(streaming, kDestination + destination_offset,
                         kSource + source_offset,
                         length) == kDestination + destination_offset);
      CHECK(std::equal(fixture.source() + source_offset,
                       fixture.source() + source_offset + length,
                       fixture.destination() + destination_offset));
      fixture.CheckCanaries(destination_offset, length);
    }
  }
}

TEST_CASE("XMem operations stop at an inaccessible page boundary", "[xmem]") {
  MemoryFixture fixture;
  constexpr std::array<uint32_t, 10> lengths = {1,   3,   15,  16,  17,
                                                127, 128, 129, 256, 4095};
  for (uint32_t length : lengths) {
    const uint32_t offset = kDataSize - length;
    CAPTURE(length);
    fixture.ResetDestination();
    CHECK(fixture.Set(kDestination + offset, 0x1234, length) ==
          kDestination + offset);
    CHECK(std::all_of(fixture.destination() + offset,
                      fixture.destination() + kDataSize,
                      [](uint8_t byte) { return byte == 0x34; }));
    fixture.CheckCanaries(offset, length);
    for (bool streaming : {false, true}) {
      CAPTURE(streaming);
      fixture.ResetDestination();
      CHECK(fixture.Copy(streaming, kDestination + offset, kSource + offset,
                         length) == kDestination + offset);
      CHECK(std::equal(fixture.source() + offset, fixture.source() + kDataSize,
                       fixture.destination() + offset));
      fixture.CheckCanaries(offset, length);
    }
  }
}

TEST_CASE("Zero-length XMem calls return the guest destination without access",
          "[xmem]") {
  MemoryFixture fixture;
  for (uint32_t destination : {0u, 0xFFFFFFFFu, kDestination - kPageSize}) {
    CAPTURE(destination);
    CHECK(fixture.Set(destination, 0xFF, 0) == destination);
    CHECK(fixture.Copy(false, destination, 0xFFFFFFFF, 0) == destination);
    CHECK(fixture.Copy(true, destination, 0, 0) == destination);
  }
  CHECK(std::all_of(fixture.destination(), fixture.destination() + kDataSize,
                    [](uint8_t value) { return value == kCanary; }));
}

}  // namespace
}  // namespace xe::kernel::xam::test
