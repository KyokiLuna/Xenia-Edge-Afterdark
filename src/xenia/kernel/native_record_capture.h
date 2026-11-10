/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_RECORD_CAPTURE_H_
#define XENIA_KERNEL_NATIVE_RECORD_CAPTURE_H_

#include <cstdint>
#include <filesystem>
#include <span>

namespace xe {
class Memory;
namespace kernel {

// Explicit private diagnostic, synchronous before native recording/device
// resources are released. The caller keeps the producer and its allocations
// stable until return. The heap lock prevents unmapping during each checked
// copy; it does not stop another guest CPU from writing those allocations.
//
// Creates absolute_new_path exclusively, plus a new sibling directory named
// <filename>.capture containing manifest.json and declared read snapshots.
// Parents must already exist. Existing files/directories are never replaced.
// A failed write may leave new partial artifacts for inspection; do not retry
// with the same destination. Primary PM4 is preserved even if analysis rejects
// its profile or a dependency is unreadable/over budget.
//
// True means host artifacts were written successfully, NOT that dependency or
// shader closure passed. Read/budget failures are recorded in the manifest;
// host I/O, hashing or invalid invocation returns false. execution_ready is
// always false. No guest execution, GPU submission or CPU virtual-alias
// translation is performed. Total output is capped at 16 MiB.
bool CaptureNativeRecord(Memory* memory,
                         const std::filesystem::path& absolute_new_path,
                         std::span<const uint8_t> big_endian_pm4);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NATIVE_RECORD_CAPTURE_H_
