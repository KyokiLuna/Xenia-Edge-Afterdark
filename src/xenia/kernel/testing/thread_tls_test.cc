/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <cstdint>
#include <cstring>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/kernel/util/thread_tls.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
namespace {
using internal::CheckedThreadTlsSlot;
using internal::ReadThreadTlsSlot;
using internal::ThreadTlsStorage;
using internal::WriteThreadTlsSlot;

// Actual Memory and the same checked storage operations used by XThread. No
// Kernel, XThread, native TLS allocation, Task or runtime readiness is modeled.
struct TlsMemory {
  static constexpr uint32_t kBase = 0x20000000;
  static constexpr uint32_t kPage = 0x1000;
  Memory memory;
  BaseHeap* heap;

  TlsMemory() {
    REQUIRE(memory.Initialize());
    heap = memory.LookupHeap(kBase);
    REQUIRE(heap);
    REQUIRE(heap->page_size() == kPage);
    REQUIRE(heap->AllocFixed(kBase, 3 * kPage, kPage, kMemoryAllocationReserve,
                             kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(kBase + kPage, kPage, kPage,
                             kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(kBase + kPage), 0xCD, kPage);
  }
};
}  // namespace

TEST_CASE("ThreadTls.dynamic_slots_exclude_static_extension") {
  constexpr uint32_t base = TlsMemory::kBase;
  const ThreadTlsStorage range{base, base + 0x100, 0x108};
  REQUIRE(CheckedThreadTlsSlot(range, 0));
  CHECK(*CheckedThreadTlsSlot(range, 0) == base + 0x100);
  CHECK(*CheckedThreadTlsSlot(range, 1) == base + 0x104);
  CHECK_FALSE(CheckedThreadTlsSlot(range, 2));
  CHECK_FALSE(CheckedThreadTlsSlot(range, 0x40000000));
  CHECK_FALSE(CheckedThreadTlsSlot(range, UINT32_MAX));
  CHECK_FALSE(CheckedThreadTlsSlot({0, 0x100, 0x108}, 0));
  CHECK_FALSE(CheckedThreadTlsSlot({base, 0, 0x108}, 0));
  CHECK_FALSE(CheckedThreadTlsSlot({base + 4, base, 8}, 0));
  CHECK_FALSE(CheckedThreadTlsSlot({base, base + 8, 8}, 0));
  CHECK_FALSE(CheckedThreadTlsSlot({base, base, 0}, 0));
  CHECK_FALSE(CheckedThreadTlsSlot({0xFFFFFFF0, 0xFFFFFFF0, 0x100}, 0));
  REQUIRE(CheckedThreadTlsSlot({0xFFFFFFFC, 0xFFFFFFFC, 4}, 0));
  CHECK(*CheckedThreadTlsSlot({0xFFFFFFFC, 0xFFFFFFFC, 4}, 0) == 0xFFFFFFFC);
  CHECK_FALSE(CheckedThreadTlsSlot({0xFFFFFFFC, 0xFFFFFFFC, 4}, 1));
}

TEST_CASE("ThreadTls.real_storage_is_big_endian_and_checks_exact_extent") {
  TlsMemory f;
  const uint32_t dynamic = TlsMemory::kBase + TlsMemory::kPage;
  const ThreadTlsStorage range{TlsMemory::kBase, dynamic, TlsMemory::kPage + 8};
  REQUIRE(WriteThreadTlsSlot(f.memory, range, 0, 0x01234567));
  REQUIRE(WriteThreadTlsSlot(f.memory, range, 1, 0x89ABCDEF));
  const uint8_t expected[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  CHECK(std::memcmp(f.memory.TranslateVirtual(dynamic), expected,
                    sizeof(expected)) == 0);
  uint32_t value = 0;
  REQUIRE(ReadThreadTlsSlot(f.memory, range, 1, &value));
  CHECK(value == 0x89ABCDEF);
  value = 0xA5A5A5A5;
  CHECK_FALSE(ReadThreadTlsSlot(f.memory, range, 2, &value));
  CHECK(value == 0xA5A5A5A5);
  CHECK_FALSE(WriteThreadTlsSlot(f.memory, range, 2, 0));
  CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(dynamic + 8)) ==
        0xCDCDCDCD);
  CHECK_FALSE(ReadThreadTlsSlot(f.memory, range, 0, nullptr));
  CHECK_FALSE(ReadThreadTlsSlot(f.memory, range, UINT32_MAX, &value));
  CHECK_FALSE(WriteThreadTlsSlot(f.memory, range, 0x40000000, 0));
  CHECK(value == 0xA5A5A5A5);
}

TEST_CASE("ThreadTls.last_slot_and_guard_pages_preserve_failed_outputs") {
  TlsMemory f;
  const uint32_t dynamic = TlsMemory::kBase + TlsMemory::kPage;
  const ThreadTlsStorage range{dynamic - 0x20, dynamic,
                               TlsMemory::kPage + 0x20};
  constexpr uint32_t last = TlsMemory::kPage / 4 - 1;
  REQUIRE(WriteThreadTlsSlot(f.memory, range, last, 0x11223344));
  uint32_t value = 0;
  REQUIRE(ReadThreadTlsSlot(f.memory, range, last, &value));
  CHECK(value == 0x11223344);
  value = 0x55667788;
  CHECK_FALSE(ReadThreadTlsSlot(f.memory, range, last + 1, &value));
  CHECK_FALSE(WriteThreadTlsSlot(f.memory, range, last + 1, 0));
  CHECK(value == 0x55667788);

  REQUIRE(f.heap->Protect(dynamic, TlsMemory::kPage, kMemoryProtectRead));
  REQUIRE(ReadThreadTlsSlot(f.memory, range, last, &value));
  CHECK(value == 0x11223344);
  CHECK_FALSE(WriteThreadTlsSlot(f.memory, range, last, 0));
  REQUIRE(ReadThreadTlsSlot(f.memory, range, last, &value));
  CHECK(value == 0x11223344);

  const uint32_t crossing = dynamic + TlsMemory::kPage - 1;
  value = 0xCAFEBABE;
  CHECK_FALSE(ReadThreadTlsSlot(f.memory, {crossing, crossing, 4}, 0, &value));
  CHECK_FALSE(WriteThreadTlsSlot(f.memory, {crossing, crossing, 4}, 0, 0));
  CHECK(value == 0xCAFEBABE);
  REQUIRE(f.heap->Decommit(dynamic, TlsMemory::kPage));
  CHECK_FALSE(ReadThreadTlsSlot(f.memory, range, last, &value));
  CHECK_FALSE(WriteThreadTlsSlot(f.memory, range, last, 0));
  CHECK(value == 0xCAFEBABE);
}

}  // namespace xe::kernel::test
