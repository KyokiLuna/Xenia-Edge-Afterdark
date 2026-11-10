/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/xboxkrnl/xboxkrnl_xconfig.h"
#include <array>
#include <new>
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/memory.h"
namespace xe::kernel::xboxkrnl {
X_STATUS ReadXConfigGuest(XConfig& config, Memory& memory, uint16_t category,
                          uint16_t setting, uint32_t address, uint16_t capacity,
                          uint32_t required) {
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  const auto kind = static_cast<X_CONFIG_CATEGORY>(category);
  if (!address && capacity) {
    return X_STATUS_INVALID_PARAMETER_3;
  }
  if ((required && !util::GuestIoRangeValid(memory, required, 2, true)) ||
      (address && !util::GuestIoRangeValid(memory, address, capacity, true))) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  // GetSettingSize itself may read the console store. No guest translation
  // or global critical region spans that IO or the checked setting read.
  const uint16_t size = config.GetSettingSize(kind, setting);
  std::array<uint8_t, 2> required_bytes;
  xe::store_and_swap<uint16_t>(required_bytes.data(), size);
  const auto publish_required = [&] {
    return required ? util::WriteGuestIoBuffer(memory, required, required_bytes)
                    : X_STATUS_SUCCESS;
  };
  if (!size) {
    const auto published = publish_required();
    return XFAILED(published) ? published : X_STATUS_NOT_SUPPORTED;
  }
  if (!address) {
    return publish_required();  // Native WORD-sized length query.
  }
  if (capacity < size) {
    const auto published = publish_required();
    return XFAILED(published) ? published : X_STATUS_BUFFER_TOO_SMALL;
  }
  if (size > XConfig::kMaxSettingBytes) {
    return X_STATUS_NOT_SUPPORTED;
  }
  std::array<uint8_t, XConfig::kMaxSettingBytes> owned{};
  const auto status =
      config.ReadSettingChecked(kind, setting, std::span(owned).first(size));
  if (XFAILED(status)) {
    const auto published = publish_required();
    return XFAILED(published) ? published : status;
  }
  const std::array<util::GuestIoWrite, 2> writes{
      {{address, std::span<const uint8_t>(owned).first(size)},
       {required, required ? std::span<const uint8_t>(required_bytes)
                           : std::span<const uint8_t>{}}}};
  // Canonical backing overlap is explicitly unsupported, including physical
  // aliases. The common batch rejects it without publishing either output.
  return util::WriteGuestIoBuffers(memory, writes);
}
X_STATUS WriteXConfigGuest(XConfig& config, Memory& memory, uint16_t category,
                           uint16_t setting, uint32_t address, uint32_t size) {
  const auto kind = static_cast<X_CONFIG_CATEGORY>(category);
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  // Every implemented field is bounded by the existing BuildSetting buffer.
  // Refuse other extents without an arbitrary DWORD-sized allocation.
  const auto required = XConfig::WritableSettingSize(kind, setting);
  if (!required) {
    return X_STATUS_NOT_SUPPORTED;
  }
  if (!address || size != required || size > XConfig::kMaxSettingBytes) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!util::GuestIoRangeValid(memory, address, size, false)) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  std::array<uint8_t, XConfig::kMaxSettingBytes> owned;
  auto status =
      util::ReadGuestIoBuffer(memory, address, std::span(owned).first(size));
  if (XFAILED(status)) {
    return status;
  }
  return config.WriteSettingChecked(
      kind, setting, std::span<const uint8_t>(owned).first(size));
}
// r3/r4 WORD selectors, r5 pointer, r6 WORD capacity, r7 WORD* required size.
// Keep addresses un-translated DWORDs until complete ranges are checked.
dword_result_t ExGetXConfigSetting_entry(word_t category, word_t setting,
                                         dword_t buffer, word_t capacity,
                                         dword_t required,
                                         const ppc_context_t& context) {
  if (!context.value() || !context->kernel_state) {
    return X_STATUS_INVALID_PARAMETER;
  }
  try {
    return ReadXConfigGuest(*context->kernel_state->xconfig(),
                            *context->kernel_state->memory(), category, setting,
                            buffer, capacity, required);
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  }
}
DECLARE_XBOXKRNL_EXPORT1(ExGetXConfigSetting, kModules, kImplemented);
dword_result_t ExSetXConfigSetting_entry(word_t category, word_t setting,
                                         dword_t buffer, dword_t size,
                                         const ppc_context_t& context) {
  if (!context.value() || !context->kernel_state) {
    return X_STATUS_INVALID_PARAMETER;
  }
  try {
    return WriteXConfigGuest(*context->kernel_state->xconfig(),
                             *context->kernel_state->memory(), category,
                             setting, buffer, size);
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  }
}
DECLARE_XBOXKRNL_EXPORT1(ExSetXConfigSetting, kModules, kImplemented);
dword_result_t ExReadModifyWriteXConfigSettingUlong_entry(
    word_t category, word_t setting, dword_t and_mask, dword_t or_bits,
    const ppc_context_t& context) {
  if (!context.value() || !context->kernel_state) {
    return X_STATUS_INVALID_PARAMETER;
  }
  try {
    auto attribution =
        NativeSystemUiService::CaptureNotificationPreferencePersistence(
            context->kernel_state, context.value(), category, setting, and_mask,
            or_bits);
    const auto result =
        context->kernel_state->xconfig()->ModifySettingUlongObserved(
            static_cast<X_CONFIG_CATEGORY>(category.value()), setting, and_mask,
            or_bits);
    if (attribution && result.console) {
      attribution->Complete(*result.console);
    }
    return result.status;
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  }
}
DECLARE_XBOXKRNL_EXPORT1(ExReadModifyWriteXConfigSettingUlong, kModules,
                         kImplemented);
}  // namespace xe::kernel::xboxkrnl
DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(XConfig);
