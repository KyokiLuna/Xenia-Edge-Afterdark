/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <cstdint>
#include <cstring>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/kernel/native_text_input_data.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
namespace {
// Public handle-table/element layout data in real Memory. These compatible
// bytes never stand in for a Kernel, actual UI Task, scene lifetime or input
// admission. The production reader must supply those separate proofs.
struct TextFocusMemory {
  static constexpr uint32_t kManager = 0x81AA20E0;
  static constexpr uint32_t kGlobal = 0x81A9DE68;
  static constexpr uint32_t kTable = 0x81AC8328;
  static constexpr uint32_t kNodes = 0x20000000;
  static constexpr uint32_t kContext = kNodes;
  static constexpr uint32_t kParentObject = kNodes + 0x1000;
  static constexpr uint32_t kChildObject = kNodes + 0x1100;
  static constexpr uint32_t kChildBase = kNodes + 0x1200;
  static constexpr uint32_t kParentElement = kNodes + 0x2000;
  static constexpr uint32_t kChildElement = kNodes + 0x2100;
  static constexpr uint32_t kScene = kNodes + 0x2200;
  static constexpr uint32_t kEntries = kNodes + 0x3000;
  static constexpr uint32_t kParent = 0x00070001;
  static constexpr uint32_t kChild = 0x00090002;
  Memory memory;

  TextFocusMemory() {
    REQUIRE(memory.Initialize());
    Map(0x81A90000, 0x10000);
    Map(0x81AA0000, 0x10000);
    Map(0x81AC0000, 0x10000);
    Map(kNodes, 0x4000);
    Store(kManager + 0x80, kContext);
    Store(kManager + 0xA8, 1);
    Store(kManager + 0xB0, 1);
    Store(kManager + 0xB4, 1);
    Store(kGlobal, kScene);
    Store(kContext + 8, kParent);
    Store(kContext + 0x38, kScene);
    Store(kScene, 0x815F46DC);
    Store(kScene + 8, kChild);
    Store(kScene + 0x60, 1);
    Store(kTable, kEntries);
    Store(kTable + 0x420, 3);
    Store(kEntries + 8, kParent >> 16);
    Store(kEntries + 12, kParentObject);
    Store(kEntries + 16, kChild >> 16);
    Store(kEntries + 20, kChildObject);
    Store(kParentObject + 0x20, kParentElement);
    Store(kChildObject + 8, kChildBase);
    Store(kChildObject + 0x20, kScene);
    Store(kChildBase + 0x20, kChildElement);
    Store(kChildElement + 0x18, kParentElement);
  }
  void Map(uint32_t at, uint32_t bytes) {
    auto* heap = memory.LookupHeap(at);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(at, bytes, heap->page_size(),
                             kMemoryAllocationReserve | kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(at), 0, bytes);
  }
  void Store(uint32_t at, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(at), value);
  }
  NativeUiTextFocusData Read() { return ReadNativeUiTextFocusData(memory); }
};
}  // namespace

TEST_CASE("NativeTextFocusData.fresh_child_and_parent_are_distinct") {
  TextFocusMemory f;
  std::array<uint8_t, 0x4000> before;
  std::memcpy(before.data(), f.memory.TranslateVirtual(f.kNodes),
              before.size());
  const auto data = f.Read();
  REQUIRE(data.compatible());
  CHECK(data.user == 1);
  CHECK(data.context == f.kContext);
  CHECK(data.scene == f.kScene);
  CHECK(data.parent_handle == f.kParent);
  CHECK(data.child_handle == f.kChild);
  CHECK(data.parent_handle != data.child_handle);
  CHECK(std::memcmp(before.data(), f.memory.TranslateVirtual(f.kNodes),
                    before.size()) == 0);
  REQUIRE(f.memory.LookupHeap(f.kNodes)->Protect(f.kNodes, 0x4000,
                                                 kMemoryProtectRead));
  CHECK(f.Read().compatible());
}

TEST_CASE("NativeTextFocusData.rejects_stale_generations_and_aliases") {
  TextFocusMemory f;
  SECTION("child_generation") { f.Store(f.kEntries + 16, 10); }
  SECTION("parent_generation") { f.Store(f.kEntries + 8, 8); }
  SECTION("removed_handle_keeps_old_raw_scene") { f.Store(f.kEntries + 20, 0); }
  SECTION("reused_slot_maps_another_instance") {
    f.Store(f.kChildObject + 0x20, f.kScene + 0x80);
  }
  SECTION("global_alias_is_not_the_context_scene") {
    f.Store(f.kGlobal, f.kScene + 0x80);
  }
  SECTION("destroy_cleared_native_handle") { f.Store(f.kScene + 8, 0); }
  SECTION("destroy_reset_native_class") { f.Store(f.kScene, 0x815F468C); }
  SECTION("parent_is_not_child") { f.Store(f.kContext + 8, f.kChild); }
  SECTION("self_parent_cannot_validate_its_own_attachment") {
    f.Store(f.kContext + 8, f.kChild);
    f.Store(f.kChildElement + 0x18, f.kChildElement);
  }
  SECTION("distinct_handles_cannot_alias_a_self_parent_element") {
    f.Store(f.kParentObject + 0x20, f.kChildElement);
    f.Store(f.kChildElement + 0x18, f.kChildElement);
  }
  SECTION("wrong_attachment") {
    f.Store(f.kChildElement + 0x18, f.kParentElement + 4);
  }
  SECTION("detached_child") { f.Store(f.kChildElement + 0x18, 0); }
  CHECK_FALSE(f.Read().compatible());
}

TEST_CASE("NativeTextFocusData.native_handle_page_and_last_slot_boundaries") {
  TextFocusMemory f;
  uint32_t child = 0;
  SECTION("first_slot_of_second_page") {
    child = 0x00090100;
    f.Store(f.kTable + 0x420, 0x101);
    f.Store(f.kTable + 4, f.kEntries + 0x800);
    f.Store(f.kEntries + 0x800, 9);
    f.Store(f.kEntries + 0x804, f.kChildObject);
  }
  SECTION("last_representable_slot_ends_at_mapping_boundary") {
    child = 0x0009FFFF;
    f.Store(f.kTable + 0x420, 0x10000);
    f.Store(f.kTable + 0x3FC, f.kEntries + 0x800);
    f.Store(f.kEntries + 0xFF8, 9);
    f.Store(f.kEntries + 0xFFC, f.kChildObject);
  }
  f.Store(f.kScene + 8, child);
  const auto data = f.Read();
  REQUIRE(data.compatible());
  CHECK(data.child_handle == child);
}

TEST_CASE("NativeTextFocusData.bounds_handle_pages_and_class_chains") {
  TextFocusMemory f;
  SECTION("index_at_count") { f.Store(f.kTable + 0x420, 2); }
  SECTION("unrepresentable_count") { f.Store(f.kTable + 0x420, 0x10001); }
  SECTION("entry_address_overflow") { f.Store(f.kTable, 0xFFFFFFFC); }
  SECTION("missing_entry_page") { f.Store(f.kTable, 0x1000); }
  SECTION("class_cycle") { f.Store(f.kChildObject + 8, f.kChildObject); }
  SECTION("class_chain_unreadable") { f.Store(f.kChildObject + 8, 0x1000); }
  SECTION("missing_base_state") { f.Store(f.kChildBase + 0x20, 0); }
  SECTION("unmapped_scene_storage") {
    REQUIRE(f.memory.LookupHeap(f.kNodes)->Decommit(f.kNodes + 0x2000, 0x1000));
  }
  CHECK_FALSE(f.Read().compatible());
}

TEST_CASE("NativeTextFocusData.requires_stable_ready_current_user_metadata") {
  TextFocusMemory f;
  SECTION("inactive") { f.Store(f.kManager + 0xA8, 0); }
  SECTION("not_initialized") { f.Store(f.kManager + 0xB0, 0); }
  SECTION("not_started") { f.Store(f.kManager + 0xB4, 0); }
  SECTION("manager_transition") { f.Store(f.kManager, 2); }
  SECTION("missing_context") { f.Store(f.kManager + 0x80, 0); }
  SECTION("unselected_native_user") { f.Store(f.kScene + 0x60, 0xFE); }
  SECTION("any_user_is_not_an_assignment") { f.Store(f.kScene + 0x60, 0xFF); }
  SECTION("out_of_range_user") { f.Store(f.kScene + 0x60, 4); }
  CHECK_FALSE(f.Read().compatible());
}

}  // namespace xe::kernel::test
