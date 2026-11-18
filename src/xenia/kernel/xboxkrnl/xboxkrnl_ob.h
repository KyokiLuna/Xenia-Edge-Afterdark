/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_OB_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_OB_H_

#include <string>
#include <string_view>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/kernel.h"

using PPCContext = xe::cpu::ppc::PPCContext;

namespace xe {
namespace kernel {
namespace xboxkrnl {

void xeObDereferenceObject(PPCContext* context, uint32_t native_ptr);
void xeObSplitName(X_ANSI_STRING input_string,
                   X_ANSI_STRING* leading_path_component,
                   X_ANSI_STRING* remaining_path_components,
                   PPCContext* context);
uint32_t xeObHashObjectName(X_ANSI_STRING* ElementName, PPCContext* context);
uint32_t xeObCreateObject(X_OBJECT_TYPE* object_factory,
                          X_OBJECT_ATTRIBUTES* optional_attributes,
                          uint32_t object_size_sans_headers,
                          uint32_t* out_object, cpu::ppc::PPCContext* context);
// The native DOS object-directory pseudo-root, not a real directory handle.
inline constexpr uint32_t kObDosDevicesRoot = 0xFFFFFFFDu;
// Drops only the explicit TITLE DOS prefix, so \??\game:\file is game:\file.
void xeObStripDosDevicesPrefix(std::string& path);
// Capture bare DOS aliases from the actual original caller before host I/O.
// Explicit namespaces/absolute names and actual directory handles are unchanged.
void xeObCaptureFilePathForCaller(std::string& path, uint32_t root_directory);
// Excludes only a recognized DOS object-directory prefix from strict payload
// validation for root0/ObDosDevices. The actual path and realm are unchanged.
std::string_view xeObDosPathValidationView(std::string_view path,
                                           uint32_t root_directory) noexcept;
// Canonical link name without \??\; explicit SYSTEM names remain distinct.
std::string xeObSymbolicLinkName(std::string_view name);
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_OB_H_
