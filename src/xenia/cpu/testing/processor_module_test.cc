/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#endif

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::testing;

TEST_CASE("REMOVE_MODULE_KEEPS_MODULE_WITH_SAME_NAME", "[processor]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(CreateBackend());
  auto add = [&] {
    auto module = std::make_unique<TestModule>(
        processor.get(), "Patched", [](uint32_t) { return false; },
        [](hir::HIRBuilder&) { return false; });
    Module* added = module.get();
    REQUIRE(processor->AddModule(std::move(module)));
    return added;
  };
  Module* kept = add();
  Module* removed = add();
  processor->RemoveModule(removed);
  auto modules = processor->GetModules();
  CHECK(std::ranges::find(modules, kept) != modules.end());
  CHECK(std::ranges::find(modules, removed) == modules.end());
}


#if XE_ARCH_AMD64 || XE_ARCH_ARM64
TEST_CASE("REMOVE_MODULE_RETIRES_OLD_NORMAL_DISPATCH_BEYOND_SMALLER_REPLACEMENT",
          "[processor][backend][module_retirement]") {
  constexpr uint32_t kCaller = 0x80040000;
  constexpr uint32_t kOldBase = 0x90100000;
  constexpr uint32_t kNear = kOldBase + 0x1000;
  constexpr uint32_t kFar = kOldBase + 0x11000;
  constexpr uint32_t kOldEnd = kOldBase + 0x20000;
  constexpr uint32_t kSmallerEnd = kOldBase + 0x10000;
  constexpr uint64_t kOldMarker = 0xA111A222A333A444ull;
  constexpr uint64_t kNewMarker = 0xB111B222B333B444ull;
  constexpr uint64_t kFarGuardMarker = 0xC111C222C333C444ull;
  constexpr uint64_t kCallerCanary = 0x1122334455667788ull;

  auto memory = std::make_unique<Memory>();
  REQUIRE(memory->Initialize());
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  REQUIRE(processor->Setup(CreateBackend()));
#if XE_ARCH_AMD64
  auto* cache = static_cast<backend::x64::X64CodeCache*>(
      processor->backend()->code_cache());
#else
  auto* cache = static_cast<backend::a64::A64CodeCache*>(
      processor->backend()->code_cache());
#endif
  REQUIRE(cache);
  REQUIRE(cache->has_indirection_table());
  REQUIRE(cache->HasIndirectionSlot(kFar));

  // These are real TestModules compiled by the production assembler/backend.
  // No mock cache, replaced resolver or supplied host target is installed.
  const auto add_marker = [&](std::string_view name,
                              std::array<uint32_t, 2> addresses,
                              uint64_t marker, int count_register) {
    auto module = std::make_unique<TestModule>(
        processor.get(), name,
        [addresses](uint32_t address) {
          return address == addresses[0] || address == addresses[1];
        },
        [marker, count_register](hir::HIRBuilder& b) {
          StoreGPR(b, count_register,
                   b.Add(LoadGPR(b, count_register),
                         b.LoadConstantUint64(1)));
          StoreGPR(b, 3, b.LoadConstantUint64(marker));
          b.Return();
          return true;
        });
    Module* raw = module.get();
    REQUIRE(processor->AddModule(std::move(module)));
    return raw;
  };
  auto caller_module = std::make_unique<TestModule>(
      processor.get(), "ResidentCaller",
      [](uint32_t address) { return address == kCaller; },
      [](hir::HIRBuilder& b) {
        // A context-loaded target stays indirect, so this exercises the real
        // normal-address table rather than folding to a literal host call.
        b.CallIndirect(LoadGPR(b, 12));
        StoreGPR(b, 20, b.LoadConstantUint64(kCallerCanary));
        b.Return();
        return true;
      },
      // As in GUEST_TO_GUEST_CALL, preserve the post-call HIR canary block.
      /*skip_cf_simplification=*/true);
  Module* resident = caller_module.get();
  REQUIRE(processor->AddModule(std::move(caller_module)));
  Module* old = add_marker("Replaceable", {kNear, kFar}, kOldMarker, 22);
  processor->backend()->CommitExecutableRange(kCaller, kCaller + 0x1000);
  processor->backend()->CommitExecutableRange(kOldBase, kOldEnd);

  // Read the actual committed normal table through its public emitter
  // accessors. The reference resolver value is observed from an unused slot,
  // not reconstructed from or tied to either backend's encoding.
  const auto slot = [&](uint32_t address) {
    REQUIRE(cache->HasIndirectionSlot(address));
    const auto host = cache->indirection_table_base_address() +
                      (address - cache->indirection_guest_base());
    uint32_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(host), sizeof(value));
    return value;
  };
  const uint32_t resolver = slot(kFar + 0x100);
  REQUIRE(slot(kFar) == resolver);
  auto* caller = processor->ResolveFunction(kCaller);
  REQUIRE(caller);

  struct StackAllocation {
    Memory& memory;
    uint32_t address;
    ~StackAllocation() { if (address) { memory.SystemHeapFree(address); } }
  } stack{*memory, memory->SystemHeapAlloc(64 * 1024)};
  REQUIRE(stack.address);
  auto thread = std::make_unique<ThreadState>(
      processor.get(), 0x100, stack.address + 64 * 1024);
  auto* context = thread->context();
  processor->backend()->SetGuestRoundingMode(context, 0);
  context->r[22] = context->r[23] = context->r[24] = 0;
  const auto call = [&](uint32_t target) {
    context->lr = 0xBCBCBCBC;
    context->r[12] = target;
    context->r[3] = context->r[20] = 0;
    REQUIRE(caller->Call(thread.get(), uint32_t(context->lr)));
    CHECK(context->r[20] == kCallerCanary);
    return context->r[3];
  };
  REQUIRE(call(kNear) == kOldMarker);
  REQUIRE(call(kFar) == kOldMarker);
  REQUIRE(context->r[22] == 2);
  REQUIRE(slot(kFar) != resolver);  // Actual old dispatch was populated.

  processor->RemoveModule(old);
  CHECK(processor->QueryFunction(kNear) == nullptr);
  CHECK(processor->QueryFunction(kFar) == nullptr);
  CHECK(processor->QueryFunction(kCaller) == caller);
  const auto remaining = processor->GetModules();
  CHECK(std::ranges::find(remaining, resident) != remaining.end());

  // Same name/base, but the replacement's committed executable range is
  // smaller and excludes the previous far helper. Loading it must not be
  // relied upon to retire that old trailing slot.
  add_marker("Replaceable", {kNear, kNear}, kNewMarker, 23);
  processor->backend()->CommitExecutableRange(kOldBase, kSmallerEnd);
  STATIC_REQUIRE(kFar >= kSmallerEnd);
  INFO("Fail before dispatch if unload left a retired host target reachable");
  REQUIRE(slot(kFar) == resolver);

  // No old Function or machine-code pointer is dereferenced/called after
  // RemoveModule. The REQUIRE above deliberately stops a failing regression
  // before any possible dispatch into freed owner metadata.
  context->r[22] = context->r[23] = context->r[24] = 0;
  REQUIRE(call(kNear) == kNewMarker);
  CHECK(context->r[22] == 0);
  CHECK(context->r[23] == 1);

  // A live independently registered target makes the old far VA safe to
  // exercise. Do NOT reset/commit its slot here: this same compiled caller
  // must take the retirement-produced resolver and resolve the current owner.
  add_marker("LiveFarGuard", {kFar, kFar}, kFarGuardMarker, 24);
  REQUIRE(call(kFar) == kFarGuardMarker);
  CHECK(context->r[22] == 0);
  CHECK(context->r[23] == 1);
  CHECK(context->r[24] == 1);
  CHECK(processor->QueryFunction(kCaller) == caller);
}
#endif
