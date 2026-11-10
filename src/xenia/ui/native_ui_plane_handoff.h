/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_UI_NATIVE_UI_PLANE_HANDOFF_H_
#define XENIA_UI_NATIVE_UI_PLANE_HANDOFF_H_

#include "xenia/ui/presenter.h"

namespace xe::ui {

// Record-time metadata revalidation of an already admitted image/placement
// pair. The same previously checked live nonmodal epoch may advance after
// Select; modal permission remains exact. Requires the backend lock and performs
// no full liveness query. It cannot select a new image or grant new authority.
bool CanRecordNativeUiPlaneAfterHandoff(
    const NativeUiPlaneVisibility& admitted,
    const NativeUiPlaneVisibility& current,
    const NativeUiPlaneImageSelection::Selection& selection) noexcept;

}  // namespace xe::ui

#endif  // XENIA_UI_NATIVE_UI_PLANE_HANDOFF_H_
