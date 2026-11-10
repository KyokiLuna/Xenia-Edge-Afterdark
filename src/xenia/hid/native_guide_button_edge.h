/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_HID_NATIVE_GUIDE_BUTTON_EDGE_H_
#define XENIA_HID_NATIVE_GUIDE_BUTTON_EDGE_H_
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include "xenia/base/native_ui_backend_route.h"
namespace xe::kernel {
class NativeInputDriver;
}
namespace xe::hid {
class InputSystem;
namespace test {
class NativeGuideButtonEdgeTestAccess;
}
enum class NativeGuideDisposition {
  kInactive,
  kAccepted,
  kDenied,
  kHostFallback
};
struct NativeGuideDispatch {
  NativeGuideDisposition disposition = NativeGuideDisposition::kDenied;
  std::shared_ptr<NativeUiBackendRoute::HostLease> host;
  bool operator==(NativeGuideDisposition other) const {
    return disposition == other;
  }
};
// One physical rising-edge sample. The driver's closure retains the exact
// native device/user/registration identity, never a guessed user0 or a guest
// Task. A stale/denied native edge cannot fall back into a second host menu.
class NativeGuideButtonEdge {
 public:
  uint64_t generation() const { return generation_; }
  uint32_t source_slot() const { return source_slot_; }
  uint64_t trace_id() const { return trace_id_; }

 private:
  friend class InputSystem;
  friend class test::NativeGuideButtonEdgeTestAccess;
  friend class xe::kernel::NativeInputDriver;
  NativeGuideButtonEdge(uint64_t generation, uint32_t slot,
                        std::function<NativeGuideDispatch()> dispatch)
      : generation_(generation),
        source_slot_(slot),
        dispatch_(std::move(dispatch)) {}
  // Diagnostic host correlation only, never a guest argument or authority.
  uint64_t trace_id_ = 0;
  uint64_t generation_;
  uint32_t source_slot_;
  // Keep existing synthetic edge tests on the old boolean shape, while the
  // production driver supplies an explicit typed host reservation.
  NativeGuideButtonEdge(uint64_t generation, uint32_t slot,
                        std::function<bool()> dispatch)
      : NativeGuideButtonEdge(generation, slot,
                              std::function<NativeGuideDispatch()>(
                                  [dispatch = std::move(dispatch)] {
                                    return NativeGuideDispatch{
                                        dispatch()
                                            ? NativeGuideDisposition::kAccepted
                                            : NativeGuideDisposition::kDenied,
                                        {}};
                                  })) {}
  std::function<NativeGuideDispatch()> dispatch_;
  mutable std::atomic<bool> consumed_{false};
};
}  // namespace xe::hid
#endif
