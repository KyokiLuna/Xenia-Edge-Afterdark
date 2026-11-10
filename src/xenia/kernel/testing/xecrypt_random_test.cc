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
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"

namespace xe::kernel::xboxkrnl {
void XeCryptRandom_entry(lpvoid_t buf, dword_t buf_size);
}

namespace xe::kernel::xboxkrnl::test {
namespace {
constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kDataSize = 2 * kPageSize;
constexpr uint32_t kDestination = 0x90011000;
constexpr uint8_t kCanary = 0xA7;

class RandomFixture {
 public:
  RandomFixture() {
    REQUIRE(memory.Initialize());
    auto* heap = memory.LookupHeap(kDestination);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(
        kDestination - kPageSize, kDataSize + 2 * kPageSize, kPageSize,
        kMemoryAllocationReserve, kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(kDestination, kDataSize, kPageSize,
                             kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    RegisterCryptExports(&resolver, nullptr);
    XboxkrnlModule::RegisterExportTable(&resolver);
    entry = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x18A);
    REQUIRE(entry);
    REQUIRE(entry->function_data.trampoline);
    Reset();
  }
  uint8_t* data() { return memory.TranslateVirtual(kDestination); }
  void Reset() { std::memset(data(), kCanary, kDataSize); }
  void Call(uint32_t address, uint32_t length) {
    // Use the actual typed entry for nonempty guest memory, as the existing
    // XMem tests do. A stack PPCContext has no native Windows E-window address
    // tag, so using its virtual pointer translator here would be misleading.
    shim::PointerParam pointer(address ? memory.TranslateVirtual(address)
                                       : nullptr);
    static_cast<shim::ParamBase<uint32_t>&>(pointer) = address;
    XeCryptRandom_entry(pointer, shim::ParamBase<uint32_t>(length));
  }
  void CallZeroTrampoline(uint32_t address) {
    PPCContext context{};
    context.virtual_membase = memory.virtual_membase();
    for (size_t i = 0; i < 32; ++i) {
      context.r[i] = 0xAABBCCDD00000000ull + i;
    }
    context.r[1] = kDestination + 0x100;
    context.r[3] = 0x1234567800000000ull | address;
    context.r[4] = 0xABCDEF1200000000ull;
    context.lr = 0xABCDEF1122334455ull;
    std::array<uint64_t, 32> original;
    std::copy_n(context.r, original.size(), original.begin());
    entry->function_data.trampoline(&context);
    // Actual void trampoline: no invented guest result, changed argument,
    // stack or TLS value. Pointer and DWORD arguments use their low32 bits.
    for (size_t i = 0; i < original.size(); ++i) {
      CAPTURE(i, address);
      CHECK(context.r[i] == original[i]);
    }
    CHECK(context.lr == 0xABCDEF1122334455ull);
  }
  void CheckCanaries(uint32_t offset, uint32_t length) {
    CHECK(std::all_of(data(), data() + offset,
                      [](uint8_t value) { return value == kCanary; }));
    CHECK(std::all_of(data() + offset + length, data() + kDataSize,
                      [](uint8_t value) { return value == kCanary; }));
  }
  Memory memory;
  cpu::ExportResolver resolver;
  cpu::Export* entry = nullptr;
};

TEST_CASE("XeCryptRandom registers its actual implemented void export",
          "[kernel][xecrypt_random]") {
  RandomFixture fixture;
  CHECK(std::string_view(fixture.entry->name) == "XeCryptRandom");
  CHECK(fixture.entry->get_type() == cpu::Export::Type::kFunction);
  CHECK(fixture.entry->is_implemented());
  CHECK((fixture.entry->tags & cpu::ExportTag::kStub) == 0);
}

TEST_CASE("XeCryptRandom zero bytes does not access null or guard pointers",
          "[kernel][xecrypt_random]") {
  RandomFixture fixture;
  for (uint32_t address :
       {0u, UINT32_MAX, kDestination - 1, kDestination + kDataSize}) {
    fixture.CallZeroTrampoline(address);
    fixture.CheckCanaries(0, 0);
  }
}

TEST_CASE("XeCryptRandom actual entry respects unaligned and page-end bounds",
          "[kernel][xecrypt_random]") {
  RandomFixture fixture;
  constexpr std::pair<uint32_t, uint32_t> cases[] = {{0, 1},
                                                     {1, 3},
                                                     {17, 4},
                                                     {3, 255},
                                                     {1, 256},
                                                     {5, 257},
                                                     {kPageSize - 3, 513},
                                                     {kDataSize - 257, 257},
                                                     {kDataSize - 1, 1},
                                                     {0, kDataSize}};
  for (const auto [offset, length] : cases) {
    CAPTURE(offset, length);
    fixture.Reset();
    fixture.Call(kDestination + offset, length);
    fixture.CheckCanaries(offset, length);
  }
  // OS entropy may legitimately equal a canary, a previous byte or 0xFD.
  // No payload uniqueness, frequency, timing or probabilistic claim belongs
  // in this deterministic bounds/ABI test. Full-fill follows the checked OS
  // API contract; a failure terminates rather than returning partial bytes.
}
TEST_CASE("XeCryptRandom preserves actual guest physical CPU write watches",
          "[kernel][xecrypt_random][memory_watch]") {
  RandomFixture fixture;
  constexpr uint32_t address = 0xE1000000;
  constexpr uint32_t offset = 17, length = 257;
  auto* heap = fixture.memory.LookupHeap(address);
  REQUIRE(heap);
  REQUIRE(heap->heap_type() == HeapType::kGuestPhysical);
  REQUIRE(heap->AllocFixed(address, kPageSize, kPageSize,
                           kMemoryAllocationReserve | kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  auto* page = fixture.memory.TranslateVirtual<uint8_t*>(address);
  std::memset(page, kCanary, kPageSize);
  std::atomic<uint32_t> invalidations{0};
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  // Memory::Initialize installs its actual MMIO/fault handler. No fake CPU,
  // Kernel or thread is needed; the write-watch callback runs in fault context.
  auto* handle = fixture.memory.RegisterPhysicalMemoryInvalidationCallback(
      [](void* context, uint32_t, uint32_t, bool) {
        static_cast<std::atomic<uint32_t>*>(context)->fetch_add(
            1, std::memory_order_relaxed);
        return std::pair<uint32_t, uint32_t>{0, UINT32_MAX};
      },
      &invalidations);
  struct Watch {
    Memory& memory;
    void* handle;
    ~Watch() { memory.UnregisterPhysicalMemoryInvalidationCallback(handle); }
  } watch{fixture.memory, handle};
  fixture.memory.EnablePhysicalMemoryAccessCallbacks(
      fixture.memory.GetPhysicalAddress(address), kPageSize, true, false);
  fixture.Call(address + offset, length);
  CHECK(invalidations.load(std::memory_order_relaxed) > 0);
  CHECK(std::all_of(page, page + offset,
                    [](uint8_t value) { return value == kCanary; }));
  CHECK(std::all_of(page + offset + length, page + kPageSize,
                    [](uint8_t value) { return value == kCanary; }));
}

}  // namespace
}  // namespace xe::kernel::xboxkrnl::test
