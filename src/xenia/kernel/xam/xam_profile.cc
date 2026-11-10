/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"
#include <array>
#include <cstring>
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamProfileFindAccount_entry(
    qword_t offline_xuid, pointer_t<X_XAMACCOUNTINFO> account_ptr,
    lpdword_t device_id) {
  if (!account_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  account_ptr.Zero();

  std::error_code error;
  const auto snapshot = kernel_state()->xam_state()->profile_manager()
                            ->CaptureCachedAccount(offline_xuid, error);
  if (!snapshot) {
    return error == std::errc::device_or_resource_busy ? X_ERROR_BUSY
                                                     : X_ERROR_NO_SUCH_USER;
  }
  *account_ptr = snapshot->account();
  if (device_id) { *device_id = snapshot->device_id(); }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileFindAccount, kUserProfiles, kImplemented);

dword_result_t XamProfileOpen_entry(
    qword_t xuid, lpstring_t mount_path, dword_t flags,
    pointer_t<XCONTENT_AGGREGATE_DATA> content_data_ptr) {
  /* Notes:
      - If xuid is not local then returns X_ERROR_INVALID_PARAMETER
  */
  if (!kernel_state()->xam_state()->profile_manager()->MountProfile(
          xuid, mount_path.value())) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (content_data_ptr) {
    content_data_ptr->content_type = XContentType::kProfile;
    content_data_ptr->device_id = 1;
    content_data_ptr->set_file_name(xe::string_util::to_hex_string(xuid));
    content_data_ptr->title_id = kDashboardID;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileOpen, kNone, kImplemented);

dword_result_t XamProfileCreate_entry(
    dword_t flags, dword_t device_id, qword_t xuid,
    pointer_t<X_XAMACCOUNTINFO> account,
    pointer_t<X_USER_PAYMENT_INFO> payment_info,
    pointer_t<X_PASSPORT_SESSION_TOKEN> user_token,
    pointer_t<X_PASSPORT_SESSION_TOKEN> owner_token,
    lpdword_t profile_info_ptr) {
  auto* kernel = kernel_state();
  auto& memory = *kernel->memory();
  const auto output = profile_info_ptr.guest_address();
  if (!account || !output ||
      !util::GuestIoRangeValid(memory, output, 4, true)) {
    return X_E_INVALIDARG;
  }
  // Retail17559 r4 is a DWORD device VALUE, not an output pointer. Preserve
  // its complete 0xAC0 completion layout, including the actual 0x17C Account.
  std::array<uint8_t, 0xAC0> bytes{};
  xe::store_and_swap<uint32_t>(bytes.data(), uint32_t(flags) & 0x7fffffffu);
  xe::store_and_swap<uint32_t>(bytes.data() + 4, device_id);
  const uint64_t proper_xuid = xuid ? uint64_t(xuid) : GenerateXuid();
  xe::store_and_swap<uint64_t>(bytes.data() + 0xA78, proper_xuid);
  xe::store_and_swap<uint32_t>(bytes.data() + 0xABC, uint32_t(flags) >> 31);
  if (XFAILED(util::ReadGuestIoBuffer(memory, account.guest_address(),
      std::span<uint8_t>(bytes).subspan(8, 0x17C))) ||
      (payment_info && XFAILED(util::ReadGuestIoBuffer(memory,
          payment_info.guest_address(),
          std::span<uint8_t>(bytes).subspan(0x184, 0x8F0)))) ||
      (user_token && XFAILED(util::ReadGuestIoBuffer(memory,
          user_token.guest_address(),
          std::span<uint8_t>(bytes).subspan(0xA80, 0x1C)))) ||
      (owner_token && XFAILED(util::ReadGuestIoBuffer(memory,
          owner_token.guest_address(),
          std::span<uint8_t>(bytes).subspan(0xA9C, 0x1C))))) {
    return X_E_INVALIDARG;
  }
  auto* manager = kernel->xam_state()->profile_manager();
  const auto source = kernel->xam_state()->content_manager()->sources().Source("hdd");
  if (!source || !source->available ||
      uint32_t(device_id) != source->source.device_id) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  std::error_code error;
  const auto record = manager->AllocateProfileCreationResult(bytes, error);
  if (!record) { return X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY); }
  X_XAMACCOUNTINFO copied_account{};
  std::memcpy(&copied_account, bytes.data() + 8, sizeof(copied_account));
  const auto result = manager->CreateProfileDetailed(copied_account, proper_xuid);
  // This HLE implementation completes the real producer synchronously. The
  // owned completion is not a fabricated retail Task or fabricated success.
  if (!manager->CompleteProfileCreationResult(record, result)) {
    manager->DiscardProfileCreationResult(record);
    return X_E_FAIL;
  }
  std::array<uint8_t, 4> pointer_bytes{};
  xe::store_and_swap<uint32_t>(pointer_bytes.data(), record);
  if (XFAILED(util::WriteGuestIoBuffer(memory, output, pointer_bytes))) {
    manager->DiscardProfileCreationResult(record);
    return X_E_INVALIDARG;
  }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileCreate, kNone, kSketchy);

dword_result_t XamProfileClose_entry(lpstring_t mount_name) {
  std::string guest_name = mount_name.value();
  const bool result = kernel_state()->xam_state()->profile_manager()->
      DismountProfile(guest_name);

  return result ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamProfileClose, kNone, kStub);

dword_result_t XamProfileGetCreationStatus_entry(
    pointer_t<X_PROFILE_CREATION_INFO> profile_info, lpqword_t offline_xuid) {
  return kernel_state()->xam_state()->profile_manager()->
      ConsumeProfileCreationResult(profile_info.guest_address(),
                                    offline_xuid.guest_address());
}
DECLARE_XAM_EXPORT1(XamProfileGetCreationStatus, kNone, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Profile);
