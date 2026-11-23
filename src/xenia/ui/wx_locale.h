/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WX_LOCALE_H_
#define XENIA_UI_WX_LOCALE_H_

#include <string>

namespace xe {
namespace ui {

// Initializes wxLocale from cvars::ui_locale. Call once at app init.
void InitializeWxLocale();

// Release the locale after app/window teardown, before wx library cleanup.
// Safe when initialization was skipped or cleanup runs more than once.
void ShutdownWxLocale();

// Canonical name of the currently-active wxLocale (e.g. "en_US", "ja_JP").
// Empty before initialization and after shutdown.
std::string GetActiveLocaleCanonicalName();

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WX_LOCALE_H_
