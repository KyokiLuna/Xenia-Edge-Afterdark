/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_text_input_data.h"

#include <algorithm>
#include <array>
#include <optional>

#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {
constexpr uint32_t kManager = 0x81AA20E0;
constexpr uint32_t kSceneAlias = 0x81A9DE68;
constexpr uint32_t kHandles = 0x81AC8328;
constexpr uint32_t kSceneVtable = 0x815F46DC;

struct Reader {
  Memory& memory;
  bool Readable(uint32_t at, uint32_t bytes) const {
    return at && bytes && uint64_t(at) + bytes <= 0x100000000ull &&
           memory.QueryRangeAccess(at, bytes) != memory::PageAccess::kNoAccess;
  }
  uint32_t Word(uint32_t at) const {
    return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at));
  }
  struct Handle {
    uint32_t entry, object, instance, element;
  };
  std::optional<Handle> Resolve(uint32_t handle) const {
    if (!handle || !Readable(kHandles, 0x424)) {
      return {};
    }
    const uint32_t count = Word(kHandles + 0x420);
    const uint32_t index = handle & 0xFFFF;
    if (count > 0x10000 || index >= count) {
      return {};
    }
    const uint32_t page = Word(kHandles + (index >> 8) * 4);
    const uint64_t entry = uint64_t(page) + (index & 255) * 8;
    if (!page || entry > UINT32_MAX || !Readable(uint32_t(entry), 8) ||
        Word(uint32_t(entry)) != handle >> 16) {
      return {};
    }
    Handle result{uint32_t(entry), Word(uint32_t(entry) + 4), 0, 0};
    if (!Readable(result.object, 0x24)) {
      return {};
    }
    result.instance = Word(result.object + 0x20);
    std::array<uint32_t, 16> seen{};
    uint32_t object = result.object;
    for (size_t depth = 0; depth < seen.size(); ++depth) {
      if (!Readable(object, 0x24) ||
          std::find(seen.begin(), seen.begin() + depth, object) !=
              seen.begin() + depth) {
        return {};
      }
      seen[depth] = object;
      const uint32_t next = Word(object + 8);
      if (!next) {
        result.element = Word(object + 0x20);
        if (!Readable(result.element, 0x1C) ||
            Word(result.entry) != handle >> 16 ||
            Word(result.entry + 4) != result.object) {
          return {};
        }
        return result;
      }
      object = next;
    }
    return {};
  }
};
}  // namespace

NativeUiTextFocusData ReadNativeUiTextFocusData(Memory& memory) {
  auto guard = global_critical_region::AcquireDirect();
  Reader r{memory};
  NativeUiTextFocusData result;
  if (!r.Readable(kManager, 0xB8) || !r.Readable(kSceneAlias, 4)) {
    return result;
  }
  if (!r.Word(kManager + 0xA8) || r.Word(kManager + 0xB0) != 1 ||
      r.Word(kManager + 0xB4) != 1) {
    result.status = NativeUiTextFocusData::Status::kInactive;
    return result;
  }
  if (r.Word(kManager)) {
    result.status = NativeUiTextFocusData::Status::kTransition;
    return result;
  }
  result.context = r.Word(kManager + 0x80);
  if (!r.Readable(result.context, 0x48)) {
    result.status = NativeUiTextFocusData::Status::kContext;
    return result;
  }
  result.parent_handle = r.Word(result.context + 8);
  result.scene = r.Word(result.context + 0x38);
  // This first alias read is a candidate only. A fresh generation-bearing
  // native handle and its parent attachment must validate it below.
  if (!r.Readable(result.scene, 0x7C) || r.Word(result.scene) != kSceneVtable ||
      r.Word(kSceneAlias) != result.scene) {
    result.status = NativeUiTextFocusData::Status::kScene;
    return result;
  }
  result.child_handle = r.Word(result.scene + 8);
  if (result.child_handle == result.parent_handle) {
    result.status = NativeUiTextFocusData::Status::kParent;
    return result;
  }
  const auto child = r.Resolve(result.child_handle);
  const auto parent = r.Resolve(result.parent_handle);
  if (!child || !parent || child->instance != result.scene) {
    result.status = NativeUiTextFocusData::Status::kHandle;
    return result;
  }
  if (child->element == parent->element ||
      r.Word(child->element + 0x18) != parent->element) {
    result.status = NativeUiTextFocusData::Status::kParent;
    return result;
  }
  result.user = r.Word(result.scene + 0x60);
  if (result.user >= 4) {
    result.status = NativeUiTextFocusData::Status::kUser;
    return result;
  }
  if (r.Word(kManager) || r.Word(kManager + 0x80) != result.context ||
      r.Word(result.context + 8) != result.parent_handle ||
      r.Word(result.context + 0x38) != result.scene ||
      r.Word(kSceneAlias) != result.scene ||
      r.Word(result.scene + 8) != result.child_handle ||
      r.Word(child->entry) != result.child_handle >> 16 ||
      r.Word(child->entry + 4) != child->object ||
      r.Word(parent->entry) != result.parent_handle >> 16 ||
      r.Word(parent->entry + 4) != parent->object) {
    result.status = NativeUiTextFocusData::Status::kTransition;
    return result;
  }
  result.status = NativeUiTextFocusData::Status::kCompatible;
  return result;
}

}  // namespace xe::kernel
