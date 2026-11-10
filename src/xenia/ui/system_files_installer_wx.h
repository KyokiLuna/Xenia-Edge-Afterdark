/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SYSTEM_FILES_INSTALLER_WX_H_
#define XENIA_UI_SYSTEM_FILES_INSTALLER_WX_H_

#include <cstddef>
#include <filesystem>
#include <functional>

namespace xe {
namespace ui {

enum class SystemFilesResult {
  kSuccess,
  kCancelled,
  // The zip isn't a USB system update.
  kNotSystemUpdate,
  kReadFailed,
  kWriteFailed,
  kMissingBaseFont,
  kInvalidBaseFont,
  kBaseFontMismatch,
  kBackupFailed,
  // The install failed and the previous one couldn't be put back. Its folders
  // are left in unrestored (or staging/previous if that move failed). Further
  // installs refuse either recovery location until it is dealt with.
  kRestoreFailed,
};

struct SystemFilesInstallOptions {
  // Optional explicitly selected user-owned retail JK base. Never modified.
  std::filesystem::path base_font;
};

// Stages a USB system update and a verified matching retail base before any
// installed-folder replacement. MissingBaseFont is a prepublication result;
// an update supplying only a patch cannot create its missing base. A valid
// existing installed base is reused. Known generated substitutes are backed
// up durably outside temporary staging before an explicit repair is published.
SystemFilesResult InstallSystemFiles(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& system_path,
    const std::function<bool(size_t done, size_t total)>& progress,
    const SystemFilesInstallOptions& options = {});

// Where InstallSystemFiles leaves a previous install it couldn't put back.
std::filesystem::path GetUnrestoredSystemFilesPath(
    const std::filesystem::path& system_path);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SYSTEM_FILES_INSTALLER_WX_H_
