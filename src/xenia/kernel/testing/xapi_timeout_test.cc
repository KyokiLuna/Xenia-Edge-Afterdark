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
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/memory.h"

DECLARE_int32(log_level);

namespace xe::kernel::xam::test {
namespace {
constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kDestination = 0x90011000;
constexpr uint8_t kCanary = 0xCD;

// The generic Debug argument printer reads nonnull lpqword_t before entry.
// Exercise the ordinary export ABI/body, including INFINITE no-access, with
// that pre-call formatter disabled and restore the caller's setting afterward.
class OrdinaryKernelLogging {
 public:
  OrdinaryKernelLogging() : previous_(cvars::log_level) {
    cvars::log_level = std::min(previous_, int32_t(2));
  }
  ~OrdinaryKernelLogging() { cvars::log_level = previous_; }

 private:
  int32_t previous_;
};

// Use the real base allocator and production ThreadState context address tag.
// A stack PPCContext has arbitrary low32 bits on Windows, which can incorrectly
// add the E-window offset to an ordinary guest virtual pointer. No ThreadState,
// Kernel, Task, TLS value or scheduler is created by this context-only fixture.
class TaggedPPCContext {
 public:
  TaggedPPCContext() {
    const size_t granularity = xe::memory::allocation_granularity();
    allocation_bytes_ = granularity + sizeof(PPCContext);
    for (uint32_t high = 0x40; high < 8192; ++high) {
      const uintptr_t address =
          ((uint64_t(high) << 32) | uint64_t(0xE0000000)) - granularity;
      allocation_ = xe::memory::AllocFixed(
          reinterpret_cast<void*>(address), allocation_bytes_,
          xe::memory::AllocationType::kReserveCommit,
          xe::memory::PageAccess::kReadWrite);
      if (allocation_) {
        context_ = ::new (static_cast<uint8_t*>(allocation_) + granularity)
            PPCContext{};
        break;
      }
    }
    REQUIRE(context_);
  }
  ~TaggedPPCContext() {
    if (allocation_) {
      context_->~PPCContext();
      xe::memory::DeallocFixed(allocation_, allocation_bytes_,
                               xe::memory::DeallocationType::kRelease);
    }
  }
  TaggedPPCContext(const TaggedPPCContext&) = delete;
  TaggedPPCContext& operator=(const TaggedPPCContext&) = delete;
  PPCContext* get() { return context_; }

 private:
  void* allocation_ = nullptr;
  size_t allocation_bytes_ = 0;
  PPCContext* context_ = nullptr;
};

class TimeoutFixture {
 public:
  TimeoutFixture() {
    REQUIRE(memory.Initialize());
    auto* heap = memory.LookupHeap(kDestination);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(kDestination - kPageSize, 3 * kPageSize, kPageSize,
                             kMemoryAllocationReserve,
                             kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(kDestination, kPageSize, kPageSize,
                             kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    // Allocate after Memory initializes its actual guest address space.
    context = std::make_unique<TaggedPPCContext>();
    context->get()->virtual_membase = memory.virtual_membase();
    REQUIRE(context->get()->TranslateVirtual(kDestination) == data());
    RegisterInfoExports(&resolver, nullptr);
    XamModule::RegisterExportTable(&resolver);
    entry = resolver.GetExportByOrdinal("xam.xex", 0x421);
    REQUIRE(entry);
    REQUIRE(entry->function_data.trampoline);
    Reset();
  }
  uint8_t* data() { return memory.TranslateVirtual(kDestination); }
  void Reset() { std::memset(data(), kCanary, kPageSize); }
  uint64_t Call(uint64_t result_register, uint64_t milliseconds_register) {
    auto* ctx = context->get();
    for (size_t i = 0; i < 32; ++i) {
      ctx->r[i] = 0xAABBCCDD00000000ull + i;
    }
    ctx->r[1] = kDestination + 0x100;
    ctx->r[3] = result_register;
    ctx->r[4] = milliseconds_register;
    ctx->lr = 0x0123456789ABCDEFull;
    std::array<uint64_t, 32> original;
    std::copy_n(ctx->r, original.size(), original.begin());
    entry->function_data.trampoline(ctx);
    for (size_t i = 0; i < original.size(); ++i) {
      if (i != 3) {
        CAPTURE(i);
        CHECK(ctx->r[i] == original[i]);
      }
    }
    CHECK(ctx->lr == 0x0123456789ABCDEFull);
    return ctx->r[3];
  }
  void CheckCanaries(uint32_t offset, uint32_t written) {
    CHECK(std::all_of(data(), data() + offset,
                      [](uint8_t byte) { return byte == kCanary; }));
    CHECK(std::all_of(data() + offset + written, data() + kPageSize,
                      [](uint8_t byte) { return byte == kCanary; }));
  }

  OrdinaryKernelLogging logging;
  Memory memory;
  std::unique_ptr<TaggedPPCContext> context;
  cpu::ExportResolver resolver;
  cpu::Export* entry = nullptr;
};

TEST_CASE("XapiFormatTimeOut registers its actual implemented export",
          "[kernel][xapi_timeout]") {
  TimeoutFixture fixture;
  CHECK(std::string_view(fixture.entry->name) == "XapiFormatTimeOut");
  CHECK(fixture.entry->get_type() == cpu::Export::Type::kFunction);
  CHECK(fixture.entry->is_implemented());
  CHECK((fixture.entry->tags & cpu::ExportTag::kStub) == 0);
}

TEST_CASE("XapiFormatTimeOut actual trampoline writes console timeout literals",
          "[kernel][xapi_timeout]") {
  TimeoutFixture fixture;
  // Upstream author-recorded RGH Jasper/17559 observations, not a console run
  // performed by us: xam/testing/xam__gen_xapi.json at commit
  // a88d0d88b5da1ca258b0186a134e820b4811a0b2 (Git blob
  // 97ce1fe3a56bb90e441a010db8733176c58621d6). Expectations are literal BE
  // bytes, not a copy of the implementation's timeout multiplication.
  struct TimeoutCase {
    uint32_t milliseconds;
    std::array<uint8_t, 8> bytes;
  };
  constexpr std::array<TimeoutCase, 6> cases = {{
      {0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
      {1, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xD8, 0xF0}},
      {1000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x67, 0x69, 0x80}},
      {0x7FFFFFFF, {0xFF, 0xFF, 0xEC, 0x78, 0x00, 0x00, 0x27, 0x10}},
      {0x80000000, {0xFF, 0xFF, 0xEC, 0x78, 0x00, 0x00, 0x00, 0x00}},
      {0xFFFFFFFE, {0xFF, 0xFF, 0xD8, 0xF0, 0x00, 0x00, 0x4E, 0x20}},
  }};
  // Upper-register probes follow the independently checked supplied native421
  // body81722DA0 (32B
  // SHA61132f26fb6c9ddf4a8bf47987adc4c73ca1f80a65f55d58999ae052323fc2d6), not
  // extra console observations: only ms.low32 is consumed, full r3 returns.
  for (const auto& timeout : cases) {
    for (uint32_t offset : {0u, kPageSize - 8}) {
      for (uint64_t upper :
           {0ull, 0x0000000100000000ull, 0xFEDCBA9800000000ull}) {
        const uint64_t incoming = upper | (kDestination + offset);
        CAPTURE(timeout.milliseconds, offset, incoming);
        fixture.Reset();
        CHECK(fixture.Call(incoming, 0xA5A55A5A00000000ull |
                                         timeout.milliseconds) == incoming);
        CHECK(std::equal(timeout.bytes.begin(), timeout.bytes.end(),
                         fixture.data() + offset));
        fixture.CheckCanaries(offset, 8);
      }
    }
  }
}

TEST_CASE("XapiFormatTimeOut INFINITE avoids output access and writes",
          "[kernel][xapi_timeout]") {
  TimeoutFixture fixture;
  // Only the center page is committed. Either guard must fault if read/written;
  // INFINITE must return0 without touching null, guards, MAX or a valid buffer.
  for (uint32_t address :
       {0u, kDestination - kPageSize, kDestination + kPageSize, UINT32_MAX,
        kDestination}) {
    for (uint64_t upper : {0ull, 0xFEDCBA9800000000ull}) {
      CAPTURE(address, upper);
      fixture.Reset();
      CHECK(fixture.Call(upper | address, 0x12345678FFFFFFFFull) == 0);
      fixture.CheckCanaries(0, 0);
    }
  }
}

}  // namespace
}  // namespace xe::kernel::xam::test
