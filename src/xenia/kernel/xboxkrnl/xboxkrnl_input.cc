/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstring>

#include "xenia/base/mutex.h"
#include "xenia/kernel/native_input_driver.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

namespace xe::kernel::xboxkrnl {
namespace {
constexpr X_STATUS kDeviceNotConnected = 0xC000009D;

bool Writable(uint32_t address, uint32_t length) {
  const uint64_t end = uint64_t(address) + length;
  if (!address || !length || end > 0x100000000ull) {
    return false;
  }
  auto* memory = kernel_state()->memory();
  uint64_t cursor = address;
  while (cursor < end) {
    auto* heap = memory->LookupHeap(uint32_t(cursor));
    HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(cursor), &info, 1) ||
        !(info.state & kMemoryAllocationCommit) ||
        !IsWritableProtect(info.protect)) {
      return false;
    }
    const uint64_t page_size = heap->page_size();
    const uint64_t next = (cursor & ~(page_size - 1)) + page_size;
    if (next <= cursor) {
      return false;
    }
    cursor = next;
  }
  return true;
}

std::shared_ptr<NativeInputDriver> Driver() {
  auto driver = NativeInputDriver::Lookup(kernel_state());
  return driver && driver->AuthorizeCurrentCaller() ? driver : nullptr;
}

uint32_t RegisterCallback(NativeInputDriver::Callback kind, uint32_t address) {
  auto driver = Driver();
  return driver && driver->SetCallback(kind, address) ? X_STATUS_SUCCESS
                                                      : X_STATUS_NOT_SUPPORTED;
}
}  // namespace

// Verified17559 caller816F15E4..15F8: split standard16-byte state plus a
// separate BOOL. The ordinary virtual HID producer profile is corroborated by
// the retail hook in Durg5/riffmaster-rgh360
// commit4203cbe3316278779b4ead1b7b29d730f9e31e93, main.cpp3532/3652: that
// profile explicitly supplies FALSE. Its wider hardware meaning is not
// inferred, and this does not authorize other device classes.
dword_result_t XInputdReadState_entry(dword_t handle, lpdword_t packet,
                                      pointer_t<hid::X_INPUT_GAMEPAD> gamepad,
                                      lpdword_t opaque_output) {
  auto driver = Driver();
  if (!driver || !Writable(packet.guest_address(), 4) ||
      !Writable(gamepad.guest_address(), sizeof(hid::X_INPUT_GAMEPAD)) ||
      !Writable(opaque_output.guest_address(), 4)) {
    // Native191 maps only this failure to DEVICE_NOT_CONNECTED reliably.
    return kDeviceNotConnected;
  }
  hid::X_INPUT_STATE sampled{};
  const X_STATUS result = driver->ReadState(handle, &sampled);
  if (XSUCCEEDED(result)) {
    *packet = sampled.packet_number;
    std::memcpy(static_cast<hid::X_INPUT_GAMEPAD*>(gamepad), &sampled.gamepad,
                sizeof(sampled.gamepad));
    *opaque_output = 0;
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT2(XInputdReadState, kInput, kSketchy, kHighFrequency);

dword_result_t XInputdGetCapabilities_entry(
    dword_t handle, pointer_t<hid::X_INPUT_CAPABILITIES_EX> caps,
    lpdword_t opaque_output) {
  // Native816F12B8 initializes a32-byte descriptor and FALSE selects the
  // ordinary-controller branch. This virtual profile reports real base
  // capabilities, with no additional hardware identity/extension claims.
  auto driver = Driver();
  if (!driver ||
      !Writable(caps.guest_address(), sizeof(hid::X_INPUT_CAPABILITIES_EX)) ||
      !Writable(opaque_output.guest_address(), 4)) {
    return kDeviceNotConnected;
  }
  hid::X_INPUT_CAPABILITIES_EX sampled{};
  const X_STATUS result = driver->GetCapabilities(handle, &sampled);
  if (XSUCCEEDED(result)) {
    std::memcpy(static_cast<hid::X_INPUT_CAPABILITIES_EX*>(caps), &sampled,
                sizeof(sampled));
    *opaque_output = 0;
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetCapabilities, kInput, kSketchy);

// The supplied17559 F18C0/F1928 wrappers pass handle, selector, output and
// optional DWORD. Their Aux consumers use connection status and the DWORD;
// the native power-dialog path also consumes the class word at+0xC.
// This is the declared virtual RGC profile, not USB/radio hardware telemetry.
// Independent producer-layout corroboration (28 bytes, class+0xC, port+0x10):
// c373z/xenia-guide-hud commit9498cff93c4e7fba45d96e64d76c6f35e69318bd,
// xboxkrnl_xinputd.cc. Its unmodeled counters are not hardware measurements.
dword_result_t XInputdGetDeviceStats_entry(dword_t handle, dword_t selector,
                                          lpvoid_t stats,
                                          lpdword_t opaque_output) {
  auto driver = Driver();
  if (!driver) {
    return kDeviceNotConnected;
  }
  {
    auto memory_guard = global_critical_region::AcquireDirect();
    if (!Writable(stats.guest_address(),
                  NativeInputDriver::kVirtualDeviceStatsBytes) ||
        (opaque_output && !Writable(opaque_output.guest_address(), 4))) {
      return X_STATUS_INVALID_PARAMETER;
    }
  }
  std::array<uint8_t, NativeInputDriver::kVirtualDeviceStatsBytes> sampled{};
  uint32_t opaque = 0;
  const X_STATUS result =
      driver->GetDeviceStats(handle, selector, &sampled, &opaque);
  if (XSUCCEEDED(result)) {
    // Provider sampling takes the input lock, so keep it outside the memory
    // transaction. Revalidate after it, then exclude NtFree/Protect while both
    // caller outputs are copied.
    auto memory_guard = global_critical_region::AcquireDirect();
    if (!Writable(stats.guest_address(), sampled.size()) ||
        (opaque_output && !Writable(opaque_output.guest_address(), 4))) {
      return X_STATUS_INVALID_PARAMETER;
    }
    std::memcpy(stats.as<uint8_t*>(), sampled.data(), sampled.size());
    if (opaque_output) {
      *opaque_output = opaque;
    }
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetDeviceStats, kInput, kSketchy);

// Native F4298 consumes this DWORD even when it ignores the return status.
// Validate its storage before consuming the cohort-owned failure latch. The
// error output is defined, but a denied/stopped query still returns failure.
dword_result_t XInputdGetFailedConnectionOrBind_entry(lpdword_t output) {
  // Take has no physical-provider/PPC work. Preserve the output mapping from
  // validation through latch consumption and store, so a failed write cannot
  // silently discard a real pending failure.
  auto memory_guard = global_critical_region::AcquireDirect();
  if (!Writable(output.guest_address(), 4)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  *output = 0;
  auto driver = Driver();
  if (!driver) {
    return kDeviceNotConnected;
  }
  uint32_t failed_handle = 0;
  const X_STATUS result =
      driver->TakeFailedConnectionOrBind(&failed_handle);
  if (XSUCCEEDED(result)) {
    *output = failed_handle;
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetFailedConnectionOrBind, kInput, kSketchy);

// The native selector forwards its user index, not its looked-up driverhandle.
// It owns400/100ms repeat policy after this physical-edge queue returns EMPTY.
dword_result_t XInputdReadTextKeystroke_entry(
    dword_t user, pointer_t<hid::X_INPUT_KEYSTROKE> stroke,
    const ppc_context_t& context) {
  auto driver = Driver();
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  {
    auto guard = global_critical_region::AcquireDirect();
    if (!Writable(stroke.guest_address(), sizeof(hid::X_INPUT_KEYSTROKE))) {
      return X_ERROR_INVALID_PARAMETER;
    }
  }
  hid::X_INPUT_KEYSTROKE sampled{};
  // ContextParam is deliberately LAST: it consumes a host shim ordinal but
  // does not add a third guest argument or shift the real user/output ABI.
  const X_RESULT result = driver->ReadText(user, &sampled, *context.value());
  if (result == X_ERROR_SUCCESS) {
    auto guard = global_critical_region::AcquireDirect();
    if (!Writable(stroke.guest_address(), sizeof(sampled))) {
      return X_ERROR_INVALID_PARAMETER;
    }
    std::memcpy(static_cast<hid::X_INPUT_KEYSTROKE*>(stroke), &sampled,
                sizeof(sampled));
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT2(XInputdReadTextKeystroke, kInput, kSketchy,
                         kHighFrequency);

// A virtual opaque16-bit word. No physical LED or host Caps/Num/Scroll mask
// interpretation is inferred. Native1C1 expects BOOL success, NOT NTSTATUS.
dword_result_t XInputdGetTextDeviceKeyLocks_entry(
    dword_t user, pointer_t<xe::be<uint16_t>> output) {
  auto driver = Driver();
  if (!driver) {
    return 0;
  }
  {
    auto guard = global_critical_region::AcquireDirect();
    if (!Writable(output.guest_address(), 2)) {
      return 0;
    }
  }
  uint16_t word = 0;
  if (!driver->GetTextKeyLocks(user, &word)) {
    return 0;
  }
  auto guard = global_critical_region::AcquireDirect();
  if (!Writable(output.guest_address(), 2)) {
    return 0;
  }
  *output = word;
  return 1;
}
DECLARE_XBOXKRNL_EXPORT2(XInputdGetTextDeviceKeyLocks, kInput, kSketchy,
                         kHighFrequency);

dword_result_t XInputdSetTextDeviceKeyLocks_entry(dword_t user, dword_t value,
                                                  dword_t mask) {
  auto driver = Driver();
  return driver ? driver->SetTextKeyLocks(user, uint16_t(value), uint16_t(mask))
                : kDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSetTextDeviceKeyLocks, kInput, kSketchy);

// Native28A8 reads the BE64 output only on status0. The provider uses the same
// Clock::QueryGuestInterruptTime as the real KeQueryInterruptTime bundle.
dword_result_t XInputdGetLastTextInputTime_entry(
    dword_t user, pointer_t<xe::be<uint64_t>> output) {
  auto driver = Driver();
  if (!driver) {
    return kDeviceNotConnected;
  }
  {
    auto guard = global_critical_region::AcquireDirect();
    if (!Writable(output.guest_address(), 8)) {
      return X_STATUS_INVALID_PARAMETER;
    }
  }
  uint64_t time = 0;
  const X_STATUS status = driver->GetLastTextInputTime(user, &time);
  if (status != X_STATUS_SUCCESS) {
    return status;
  }
  auto guard = global_critical_region::AcquireDirect();
  if (!Writable(output.guest_address(), 8)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  *output = time;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XInputdGetLastTextInputTime, kInput, kSketchy,
                         kHighFrequency);

// The separate raw-HID consumer treats EVERY nonnegative result as success.
// An unsupported producer must return negative and leave its output untouched.
dword_result_t HidGetLastInputTime_entry(lpvoid_t) {
  return kDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT2(HidGetLastInputTime, kInput, kStub, kHighFrequency);

// Native816F1400..1448 uses a one-byte keyboard type from this fallback. No
// unverified additional HID capability fields are written.
dword_result_t HidGetCapabilities_entry(dword_t user,
                                        pointer_t<uint8_t> keyboard_type) {
  // Only the first consumed byte is known. A successful complete HID
  // descriptor/type cannot be inferred from a host keyboard's existence.
  return kDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(HidGetCapabilities, kInput, kStub);

dword_result_t DrvSetUserBindingCallback_entry(dword_t address) {
  return RegisterCallback(NativeInputDriver::Callback::kUserBinding, address);
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetUserBindingCallback, kInput, kSketchy);

dword_result_t DrvSetDeviceConfigChangeCallback_entry(dword_t address) {
  return RegisterCallback(NativeInputDriver::Callback::kDeviceConfig, address);
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetDeviceConfigChangeCallback, kInput, kSketchy);

dword_result_t XInputdSetFailedConnectionOrBindCallback_entry(dword_t address) {
  return RegisterCallback(NativeInputDriver::Callback::kFailedConnectionOrBind,
                          address);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSetFailedConnectionOrBindCallback, kInput,
                         kSketchy);

dword_result_t DrvSetSysReqCallback_entry(dword_t address) {
  return RegisterCallback(NativeInputDriver::Callback::kSystemRequest, address);
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetSysReqCallback, kInput, kSketchy);

// These are different protocols from standard state/vibration. Raw packets
// carry a length byte at+20 and a packet DWORD at+24; WriteState forwards six
// arguments. Their unverified fields/operations must never authorize a read of
// untouched caller storage via the native wrapper's broad status mapping.
dword_result_t XInputdRawState_entry(dword_t handle, dword_t device_class,
                                     dword_t operation, lpvoid_t packet) {
  return kDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdRawState, kInput, kStub);

dword_result_t XInputdWriteState_entry(dword_t handle, dword_t mode,
                                       lpvoid_t packet, dword_t arg4,
                                       dword_t arg5, dword_t arg6) {
  return kDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdWriteState, kInput, kStub);

dword_result_t XInputdFFDeviceControl_entry(dword_t handle, dword_t command,
                                            dword_t argument) {
  // Native196 sends80000005/6. Their physical actuator semantics are not
  // established by an ignored return in that caller.
  return X_STATUS_NOT_SUPPORTED;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdFFDeviceControl, kInput, kStub);
}  // namespace xe::kernel::xboxkrnl

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Input);
