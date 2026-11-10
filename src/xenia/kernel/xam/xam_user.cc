/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xsession.h"
#include "xenia/xbox.h"
#include "third_party/stb/stb_image.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/kernel/xam/tile_request.h"

enum X_USER_AGE_GROUP : uint32_t { CHILD, TEEN, ADULT };

namespace xe {
namespace kernel {
namespace xam {

X_HRESULT_result_t XamUserGetXUID_entry(dword_t user_index, dword_t type_mask,
                                        lpqword_t xuid_ptr) {
  assert_true(type_mask == 1 || type_mask == 2 || type_mask == 3 ||
              type_mask == 4 || type_mask == 7);
  if (!xuid_ptr) {
    return X_E_INVALIDARG;
  }

  *xuid_ptr = 0;

  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);

  // Only a user signed in to Live has an online XUID to give.
  uint32_t types = X_USER_XUID_OFFLINE;
  if (user_profile->signin_state() ==
      static_cast<uint32_t>(SignInState::SignedInToLive)) {
    types |= X_USER_XUID_ONLINE;
  }
  types &= type_mask;

  if (types & X_USER_XUID_ONLINE) {
    *xuid_ptr = user_profile->GetOnlineXUID();
  } else if (types & X_USER_XUID_OFFLINE) {
    *xuid_ptr = user_profile->xuid();
  } else {
    return X_E_NO_SUCH_USER;
  }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetXUID, kUserProfiles, kImplemented);

dword_result_t XamUserGetIndexFromXUID_entry(qword_t xuid, dword_t flags,
                                             lpdword_t index) {
  if (!index) {
    return X_E_INVALIDARG;
  }

  const uint8_t user_index = kernel_state()
                                 ->xam_state()
                                 ->profile_manager()
                                 ->GetUserIndexAssignedToProfile(xuid);

  if (user_index == XUserIndexAny) {
    return X_E_NO_SUCH_USER;
  }

  *index = user_index;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetIndexFromXUID, kUserProfiles, kImplemented);

dword_result_t XamUserGetSigninState_entry(dword_t user_index) {
  uint32_t signin_state = 0;
  if (user_index >= XUserMaxUserCount) {
    return signin_state;
  }

  if (kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    const auto& user_profile =
        kernel_state()->xam_state()->GetUserProfile(user_index);
    signin_state = user_profile->signin_state();
  }
  return signin_state;
}
DECLARE_XAM_EXPORT2(XamUserGetSigninState, kUserProfiles, kImplemented,
                    kHighFrequency);

X_HRESULT_result_t XamUserGetSigninInfo_entry(
    dword_t user_index, dword_t flags, pointer_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return X_E_INVALIDARG;
  }

  info.Zero();
  info->sponsor_user_index = XUserIndexNone;

  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);

  xe::string_util::copy_truncating(info->name, user_profile->name(),
                                   xe::countof(info->name));

  if (user_profile->IsLiveEnabled()) {
    info->flags = X_USER_INFO_FLAG_LIVE_ENABLED;
  }

  // ONLINE_XUID_ONLY wins when both are set.
  if (flags & X_USER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY) {
    info->xuid = user_profile->GetOnlineXUID();
  } else if (flags & X_USER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY) {
    info->xuid = user_profile->xuid();
  } else {
    info->xuid = user_profile->GetLogonXUID();
  }

  info->signin_state = user_profile->signin_state();
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetSigninInfo, kUserProfiles, kImplemented);

dword_result_t XamUserGetName_entry(dword_t user_index, dword_t buffer,
                                    dword_t buffer_len) {
  // Based on XAM only first byte is cleared on failure.
  if (user_index >= XUserMaxUserCount) {
    kernel_memory()->Zero(buffer, 1);
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    kernel_memory()->Zero(buffer, 1);
    return X_ERROR_NO_SUCH_USER;
  }

  const auto& user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);

  // Because name is always limited to 15 characters we can assume length will
  // never exceed that limit.
  const auto& user_name = user_profile->name();

  // buffer_len counts the null terminator that user_name lacks.
  // A buffer_len of 0 takes the whole name.
  const uint32_t name_bytes = static_cast<uint32_t>(user_name.length()) + 1;
  const uint32_t bytes_to_copy =
      buffer_len ? std::min(buffer_len.value(), name_bytes) : name_bytes;

  char* str_buffer = kernel_memory()->TranslateVirtual<char*>(buffer);
  xe::string_util::copy_truncating(str_buffer, user_name, bytes_to_copy);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetName, kUserProfiles, kImplemented);

dword_result_t XamUserGetGamerTag_entry(dword_t user_index, dword_t buffer,
                                        dword_t buffer_len) {
  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);
  auto user_name = xe::to_utf16(user_profile->name());

  char16_t* str_buffer = kernel_memory()->TranslateVirtual<char16_t*>(buffer);

  xe::string_util::copy_and_swap_truncating(
      str_buffer, user_name, std::min(buffer_len.value(), uint32_t(16)));
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetGamerTag, kUserProfiles, kImplemented);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(
    uint32_t title_id, uint32_t user_index, uint32_t xuid_count, uint32_t xuids, uint32_t setting_count,
                                      uint32_t setting_ids,
    uint32_t unused,
                                      uint32_t buffer_size_address,
                                      uint32_t buffer_address,
    lpvoid_t overlapped_ptr) {
  auto* kernel = kernel_state();
  auto& memory = *kernel->memory();
  // Bounded observation of owned read preparation/publication only. Existing
  // Kernel Debug logging must be enabled; no payload bytes or account identity
  // are logged. Each process admits at most32 local-user requests, including
  // size queries. No retry, guest read or public behavior is added.
  static std::atomic<uint32_t> diagnostic_requests{0};
  uint32_t diagnostic_id = 0;
  if (user_index < XUserMaxUserCount && !xuids &&
      logging::ShouldLog(LogLevel::Debug, LogSrc::Kernel)) {
    auto next = diagnostic_requests.load(std::memory_order_relaxed);
    while (next < 32) {
      if (diagnostic_requests.compare_exchange_strong(
              next, next + 1, std::memory_order_relaxed)) {
        diagnostic_id = next + 1;
        break;
      }
    }
  }
  const auto diagnostic =
      [diagnostic_id, title_id, user_index, setting_count, buffer_address,
       async = bool(overlapped_ptr)](
          const char* stage, uint32_t result,
          const std::error_code& host_error = {}, uint32_t setting_id = 0,
          uint32_t rows = 0) -> uint32_t {
    if (diagnostic_id) {
      logging::AppendLogLineFormat(
          LogSrc::Kernel, LogLevel::Debug, logging::kPrefixCharDebug,
          "Profile settings read diagnostic {}: stage={} title={:08X} "
          "user={} requested={} output={:08X} async={} result={:08X} "
          "host_error={} category={} setting={:08X} rows={}",
          diagnostic_id, stage, title_id, user_index, setting_count,
          buffer_address, async, result, host_error.value(),
          host_error.category().name(), setting_id, rows);
    }
    return result;
  };
  if (setting_count < 1 || setting_count > 32 || !buffer_size_address ||
      !setting_ids ||
      !util::GuestIoRangeValid(memory, buffer_size_address, 4, true)) {
    return diagnostic("input_range", X_ERROR_INVALID_PARAMETER);
  }
  std::array<uint8_t, 4> size_bytes{};
  if (XFAILED(
          util::ReadGuestIoBuffer(memory, buffer_size_address, size_bytes))) {
    return diagnostic("size_read", X_ERROR_INVALID_PARAMETER);
  }
  const auto capacity = xe::load_and_swap<uint32_t>(size_bytes.data());
  if (capacity && !buffer_address) {
    return diagnostic("output_absent", X_ERROR_INVALID_PARAMETER);
  }
  std::vector<uint8_t> id_bytes(size_t(setting_count) * 4);
  if (XFAILED(util::ReadGuestIoBuffer(memory, setting_ids, id_bytes))) {
    return diagnostic("ids_read", X_ERROR_INVALID_PARAMETER);
  }
  std::vector<uint32_t> ids;
  ids.reserve(setting_count);
  uint64_t header_bytes =
               uint64_t(setting_count) * sizeof(X_USER_PROFILE_SETTING),
           data_bytes = 0;
  for (uint32_t i = 0; i != setting_count; ++i) {
    const uint32_t id = xe::load_and_swap<uint32_t>(id_bytes.data() + i * 4);
    ids.push_back(id);
    if (diagnostic_id) {
      logging::AppendLogLineFormat(
          LogSrc::Kernel, LogLevel::Debug, logging::kPrefixCharDebug,
          "Profile settings read diagnostic {}: requested_id={:08X}",
          diagnostic_id, id);
    }
    const auto key =
        UserData::get_attribute_key(id);
    if (key.type == uint32_t(X_USER_DATA_TYPE::BINARY) ||
        key.type == uint32_t(X_USER_DATA_TYPE::WSTRING)) {
      data_bytes += key.size;
    }
  }
  if (xuids) {
    header_bytes *= xuid_count;
    data_bytes *= xuid_count;
  }
  const uint64_t required = 8 + header_bytes + data_bytes;
  if (diagnostic_id) {
    logging::AppendLogLineFormat(
        LogSrc::Kernel, LogLevel::Debug, logging::kPrefixCharDebug,
        "Profile settings read diagnostic {}: capacity={} required={} "
        "effective_title={:08X} limit=32",
        diagnostic_id, capacity, required,
        title_id ? title_id : kernel->title_id());
  }
  if (required > UINT32_MAX) {
    return diagnostic("required_overflow", X_ERROR_INVALID_PARAMETER);
  }
  if (!buffer_address || capacity < required) {
    if (!capacity) {
      xe::store_and_swap<uint32_t>(size_bytes.data(), uint32_t(required));
      if (XFAILED(util::WriteGuestIoBuffer(memory, buffer_size_address,
                                           size_bytes))) {
        return diagnostic("size_publish", X_ERROR_FUNCTION_FAILED);
      }
    }
    return diagnostic("size_query", X_ERROR_INSUFFICIENT_BUFFER);
  }
  if (xuids && (!xuid_count || xuid_count > 16)) {
    return diagnostic("xuid_count", X_ERROR_FUNCTION_FAILED);
  }
  // The legacy implementation only consumed the first XUID despite claiming
  // all rows. Do not fabricate the still-unqualified remote/multi-XUID result.
  if (xuids && xuid_count != 1) {
    return diagnostic("xuid_batch_unsupported", X_ERROR_FUNCTION_FAILED);
  }
  if (!util::GuestIoRangeValid(memory, buffer_address, capacity, true) ||
      util::GuestIoRangesOverlap(memory, buffer_address, capacity,
                                 buffer_size_address, 4)) {
    return diagnostic("output_range_or_alias", X_ERROR_INVALID_PARAMETER);
  }
  std::error_code error;
  ProfileGpdWriter writer;
  if (xuids) {
    std::array<uint8_t, 8> xuid{};
    if (XFAILED(util::ReadGuestIoBuffer(memory, xuids, xuid))) {
      return diagnostic("xuid_read", X_ERROR_INVALID_PARAMETER);
    }
    writer = kernel->xam_state()->profile_manager()->CaptureGpdWriter(
        xe::load_and_swap<uint64_t>(xuid.data()), error);
  } else if (user_index < XUserMaxUserCount) {
    writer = kernel->xam_state()->profile_manager()->CaptureGpdWriter(
        uint8_t(user_index), error);
  }
  if (!writer.model) {
    return diagnostic("writer_capture", X_ERROR_FUNCTION_FAILED, error);
  }
  const auto models = kernel->xam_state()->profile_manager()->CaptureGpdRead(
      writer.model->xuid(), error);
  if (!models) {
    return diagnostic("models_capture", X_ERROR_FUNCTION_FAILED, error);
  }
  const auto captured = writer.model->CaptureRead(error);
  if (!captured || captured->models != models) {
    return diagnostic(captured ? "models_changed" : "origin_capture",
                      X_ERROR_FUNCTION_FAILED, error);
  }
  std::vector<uint8_t> output(size_t(required), 0);
  size_t record_at = 8, data_at = 8 + size_t(header_bytes);
  uint32_t valid = 0;
  for (auto id : ids) {
    if (!UserSetting::is_setting_valid(id)) {
      diagnostic("setting_skipped", X_ERROR_SUCCESS, {}, id);
      continue;
    }
    const auto value =
        models->Setting(title_id ? title_id : kernel->title_id(), id, error);
    if (error || !value) {
      return diagnostic("setting_lookup", X_ERROR_FUNCTION_FAILED, error, id);
    }
    X_USER_PROFILE_SETTING row{};
    row.setting_id = id;
    row.source = value->get_setting_source();
    row.data = *value->get_data();
    if (xuids) {
      row.xuid = writer.model->xuid();
    } else {
      row.xuid = UINT64_MAX;
      row.user_index = user_index;
    }
    if (UserSetting::requires_additional_data(id)) {
      const auto owned = value->get_extended_data();
      const uint32_t count = std::min<uint32_t>(uint32_t(owned.size()),
                                                UserSetting::get_max_size(id));
      if (data_at > output.size() || count > output.size() - data_at) {
        return diagnostic("payload_bound", X_ERROR_FUNCTION_FAILED, {}, id);
      }
      row.data.data.binary.size = count;
      row.data.data.binary.ptr = count ? buffer_address + uint32_t(data_at) : 0;
      if (count) {
        std::memcpy(output.data() + data_at, owned.data(), count);
      }
      data_at += UserSetting::get_max_size(id);
    }
    if (diagnostic_id) {
      // Hash only the same already-owned bytes selected for the prepared row.
      // "prepared" is not delivery; only the later published result witnesses
      // successful publication of this unchanged owned output vector.
      const auto payload = value->get_extended_data();
      const uint32_t payload_size = UserSetting::requires_additional_data(id)
                                        ? uint32_t(row.data.data.binary.size)
                                        : 0;
      std::array<char, 65> digest_text{};
      const char* hash = "not-binary";
      if (value->get_type() == X_USER_DATA_TYPE::BINARY) {
        std::array<uint8_t, 32> digest{};
        constexpr uint8_t empty = 0;
        hash = "unavailable";
        if (!mbedtls_sha256(payload_size ? payload.data() : &empty,
                            payload_size, digest.data(), 0)) {
          constexpr char hex[] = "0123456789abcdef";
          for (size_t n = 0; n != digest.size(); ++n) {
            digest_text[n * 2] = hex[digest[n] >> 4];
            digest_text[n * 2 + 1] = hex[digest[n] & 15];
          }
          hash = digest_text.data();
        }
      }
      logging::AppendLogLineFormat(
          LogSrc::Kernel, LogLevel::Debug, logging::kPrefixCharDebug,
          "Profile settings read diagnostic {}: stage=prepared_row "
          "setting={:08X} source={} type={} binary_or_string_bytes={} "
          "sha256={}",
          diagnostic_id, id, uint32_t(value->get_setting_source()),
          uint32_t(value->get_type()), payload_size, hash);
    }
    std::memcpy(output.data() + record_at, &row, sizeof(row));
    record_at += sizeof(row);
    ++valid;
  }
  xe::store_and_swap<uint32_t>(output.data(), valid);
  xe::store_and_swap<uint32_t>(output.data() + 4, buffer_address + 8);
  auto run = [kernel, model = writer.model, origin = captured->origin,
              buffer_address, output = std::move(output), diagnostic, valid](
                 uint32_t& extended_error, uint32_t& length) {
    extended_error = X_E_FAIL;
    length = 0;
    if (!model->IsCurrent(origin)) {
      return diagnostic("publish_origin_changed", X_ERROR_FUNCTION_FAILED,
                        {}, 0, valid);
    }
    if (XFAILED(util::WriteGuestIoBuffer(*kernel->memory(), buffer_address,
                                         output))) {
      extended_error = X_E_INVALIDARG;
      return diagnostic("publish_guest_write", X_ERROR_FUNCTION_FAILED,
                        {}, 0, valid);
    }
    extended_error = X_E_SUCCESS;
      return diagnostic("published", X_ERROR_SUCCESS, {}, 0, valid);
  };

  if (!overlapped_ptr) {
    uint32_t extended_error = 0, length = 0;
    return run(extended_error, length);
  }
  kernel->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return diagnostic("queued", X_ERROR_IO_PENDING);
}

dword_result_t XamUserReadProfileSettings_entry(
    dword_t title_id, dword_t user_index, dword_t xuid_count, lpqword_t xuids,
    dword_t setting_count, lpdword_t setting_ids, lpdword_t buffer_size_ptr,
    lpvoid_t buffer_ptr, lpvoid_t overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids.guest_address(),
                                      setting_count, setting_ids.guest_address(), 0,
                                      buffer_size_ptr.guest_address(), buffer_ptr.guest_address(), overlapped);
}
DECLARE_XAM_EXPORT1(XamUserReadProfileSettings, kUserProfiles, kImplemented);

dword_result_t XamUserReadProfileSettingsEx_entry(
    dword_t title_id, dword_t user_index, dword_t xuid_count, lpqword_t xuids,
    dword_t setting_count, lpdword_t setting_ids, dword_t options,
    lpdword_t buffer_size_ptr, lpvoid_t buffer_ptr, lpvoid_t overlapped) {
  // Retail17559 passes an opaque DWORD in r9 and the size pointer in r10.
  // Native avatar/profile consumers also place output/overlap in the two
  // following stack homes. Options must never be interpreted as an address.
  return XamUserReadProfileSettingsEx(
      title_id, user_index, xuid_count, xuids.guest_address(), setting_count, setting_ids.guest_address(),
      options, buffer_size_ptr.guest_address(), buffer_ptr.guest_address(), overlapped);
}
DECLARE_XAM_EXPORT1(XamUserReadProfileSettingsEx, kUserProfiles, kImplemented);

dword_result_t XamUserWriteProfileSettings_entry(
    dword_t title_id, dword_t user_index, dword_t setting_count,
    pointer_t<X_USER_PROFILE_SETTING> settings, lpvoid_t overlapped) {
  if (!setting_count || setting_count > 64 || !settings ||
      user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* kernel = kernel_state();
  auto& memory = *kernel->memory();
  std::error_code capture_error;
  auto writer = kernel->xam_state()->profile_manager()->CaptureGpdWriter(
      uint8_t(user_index), capture_error);
  if (!writer.model) {
    return X_ERROR_NO_SUCH_USER;
  }
  std::vector<uint8_t> bytes(size_t(setting_count) *
                             sizeof(X_USER_PROFILE_SETTING));
  if (XFAILED(
          util::ReadGuestIoBuffer(memory, settings.guest_address(), bytes))) {
    return X_ERROR_INVALID_PARAMETER;
  }
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kSetSettings;
  command.title_id = title_id ? title_id.value() : kernel->title_id();
  command.request_id = writer.model->AllocateRequestId(capture_error);
  if (capture_error || !command.title_id) {
    return X_ERROR_FUNCTION_FAILED;
  }
  bool avatar_changed = false;
  for (uint32_t n = 0; n != setting_count; ++n) {
    X_USER_PROFILE_SETTING input{};
    std::memcpy(&input, bytes.data() + n * sizeof(input), sizeof(input));
    const uint32_t id = input.setting_id;
    const auto type = UserSetting::get_type(id);
    if (type > X_USER_DATA_TYPE::DATETIME) {
      return X_ERROR_INVALID_PARAMETER;
    }
    std::vector<uint8_t> extended;
    if (type == X_USER_DATA_TYPE::BINARY || type == X_USER_DATA_TYPE::WSTRING) {
      const uint32_t count = input.data.data.binary.size;
      if (count > UserSetting::get_max_size(id) || count > kMaxUserDataSize ||
          (type == X_USER_DATA_TYPE::WSTRING && (count & 1))) {
        return X_ERROR_INVALID_PARAMETER;
      }
      extended.resize(count);
      if (count && XFAILED(util::ReadGuestIoBuffer(
                       memory, input.data.data.binary.ptr, extended))) {
        return X_ERROR_INVALID_PARAMETER;
      }
    }
    X_XDBF_GPD_SETTING_HEADER header{};
    header.setting_id = id;
    header.setting_type = type;
    std::memcpy(&header.base_data, &input.data.data, sizeof(header.base_data));
    command.settings.emplace_back(&header, extended);
    avatar_changed |=
        id == uint32_t(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1);
  }
  auto origin = writer.CaptureCurrentOrigin(command, capture_error);
  if (!origin) {
    return X_ERROR_FUNCTION_FAILED;
  }
  auto run = [kernel, writer = std::move(writer), origin = std::move(*origin),
              command = std::move(command),
              avatar_changed](uint32_t& extended_error, uint32_t& length) {
    const auto result = writer.Submit(command, origin.stamp);
    using S = ProfileGpdTransactionAttempt::State;
    const bool committed =
        !result.error &&
        (result.state == S::kCommitted || result.state == S::kAlreadyCommitted);
    const auto status = committed ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
    extended_error = committed ? X_E_SUCCESS : X_E_FAIL;
      length = 0;
    if (committed && result.state == S::kCommitted) {
      // Recheck the original owner after IO. Never notify a replacement slot.
      std::error_code current_error;
      auto current = writer.model->CaptureOrigin(current_error);
      if (current && current->stamp.sign_in_generation ==
                         origin.stamp.sign_in_generation) {
        const uint32_t bit = 1u << origin.user_slot;
        kernel->BroadcastNotification(
        kXNotificationSystemProfileSettingChanged,
                                      bit);
    if (avatar_changed) {
          kernel->BroadcastNotification(kXNotificationSystemAvatarChanged, bit);
    }
      }
    }
    return status;
  };

  if (!overlapped) {
    uint32_t error = 0, length = 0;
    return run(error, length);
  }
  kernel->CompleteOverlappedDeferredEx(run, overlapped);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamUserWriteProfileSettings, kUserProfiles, kImplemented);

// The console sends user preference requests to the XGI app and returns a
// Win32 error from the result.
static dword_result_t SendUserPreferenceRequest(uint32_t message,
                                                uint32_t user_index,
                                                uint32_t preference,
                                                uint32_t value,
                                                uint32_t overlapped_ptr) {
  const uint32_t request_ptr = kernel_memory()->SystemHeapAlloc(0xC);
  auto* request =
      kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(request_ptr);
  request[0] = user_index;
  request[1] = preference;
  request[2] = value;
  const X_HRESULT result = kernel_state()->app_manager()->DispatchMessageAsync(
      0xFB, message, request_ptr, 0xC, overlapped_ptr);
  kernel_memory()->SystemHeapFree(request_ptr);
  if (result == X_ERROR_IO_PENDING ||
      result == X_HRESULT_FROM_WIN32(X_ERROR_IO_PENDING)) {
    return X_ERROR_IO_PENDING;
  }
  // A title gets the app's S_FALSE as ERROR_NO_MORE_ITEMS.
  if (result == 1) {
    return 0x103;
  }
  return XFAILED(result) ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS;
}

dword_result_t XamUserReadUserPreference_entry(
    dword_t user_index, dword_t preference, lpdword_t value_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (user_index >= XUserMaxUserCount || !value_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return SendUserPreferenceRequest(0x000B0082, user_index, preference,
                                   value_ptr.guest_address(),
                                   overlapped_ptr.guest_address());
}
DECLARE_XAM_EXPORT1(XamUserReadUserPreference, kUserProfiles, kImplemented);

dword_result_t XamUserWriteUserPreference_entry(
    dword_t user_index, dword_t preference, dword_t value,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return SendUserPreferenceRequest(0x000B0083, user_index, preference, value,
                                   overlapped_ptr.guest_address());
}
DECLARE_XAM_EXPORT1(XamUserWriteUserPreference, kUserProfiles, kImplemented);

X_RESULT CheckUserPrivilege(uint32_t user_index, uint32_t mask,
                            uint32_t* out_value) {
  *out_value = 0;

  // With no Live service, a user signed in to Live holds every Live privilege.
  const bool granted = mask >= 0xD1 && mask <= 0xFF;

  // Every Live user holds the same privileges so the first one answers.
  if (user_index == XUserIndexAny) {
    X_RESULT result = X_ERROR_NO_SUCH_USER;
    for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
      const auto user_profile = kernel_state()->xam_state()->GetUserProfile(i);
      if (!user_profile) {
        continue;
      }
      if (user_profile->signin_state() ==
          static_cast<uint32_t>(SignInState::SignedInToLive)) {
        *out_value = granted;
        return X_ERROR_SUCCESS;
      }
      result = X_ERROR_NOT_LOGGED_ON;
    }
    return result;
  }

  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  if (kernel_state()->xam_state()->GetUserProfile(user_index)->signin_state() !=
      static_cast<uint32_t>(SignInState::SignedInToLive)) {
    return X_ERROR_NOT_LOGGED_ON;
  }

  *out_value = granted;
  return X_ERROR_SUCCESS;
}

dword_result_t XamUserCheckPrivilege_entry(dword_t user_index, dword_t mask,
                                           lpdword_t out_value) {
  uint32_t value;
  const X_RESULT result = CheckUserPrivilege(user_index, mask, &value);
  *out_value = value;
  return result;
}
DECLARE_XAM_EXPORT1(XamUserCheckPrivilege, kUserProfiles, kSketchy);

dword_result_t XamUserContentRestrictionGetFlags_entry(dword_t user_index,
                                                       lpdword_t out_flags) {
  if (user_index >= XUserMaxUserCount && user_index != XUserIndexAny) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // No restrictions? Written before the user is checked.
  *out_flags = 0;

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionGetFlags, kUserProfiles, kStub);

// The rating system and highest allowed rating settings per content type:
// games, movies, TV, explicit video and unrated video.
constexpr std::pair<uint16_t, uint16_t> kContentRestrictionSettings[] = {
    {XCONFIG_USER_PC_GAME_RATING, XCONFIG_USER_PC_GAME},
    {XCONFIG_USER_PC_MOVIE_RATING, XCONFIG_USER_PC_MOVIE},
    {XCONFIG_USER_PC_TV_RATING, XCONFIG_USER_PC_TV},
    {XCONFIG_USER_PC_EXPLICIT_VIDEO_RATING, XCONFIG_USER_PC_EXPLICIT_VIDEO},
    {XCONFIG_USER_PC_UNRATED_VIDEO_RATING, XCONFIG_USER_PC_UNRATED_VIDEO}};
constexpr uint32_t kContentTypeExplicitVideo = 3;
// Explicit video also needs this privilege.
constexpr uint32_t kExplicitVideoPrivilege = 0xED;

X_RESULT GetContentRestrictionRating(uint32_t user_index, uint32_t content_type,
                                     uint32_t* rating_system,
                                     uint32_t* max_rating) {
  if (user_index >= XUserMaxUserCount && user_index != XUserIndexAny) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto xconfig = kernel_state()->xconfig();
  const bool pc_enabled = (xconfig->ReadSetting<uint8_t>(
                               XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_FLAGS) &
                           X_PC_FLAGS::PCEnabled) != 0;
  // Without parental controls only explicit video has a rating, and the user
  // isn't looked up for the others.
  if (!pc_enabled && content_type != kContentTypeExplicitVideo) {
    return X_ERROR_NOT_FOUND;
  }
  if (content_type >= std::size(kContentRestrictionSettings)) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t system = 0;
  uint32_t max = 0;
  if (pc_enabled) {
    system = xconfig->ReadSetting<uint32_t>(
        XCONFIG_USER_CATEGORY, kContentRestrictionSettings[content_type].first);
    max = xconfig->ReadSetting<uint32_t>(
        XCONFIG_USER_CATEGORY,
        kContentRestrictionSettings[content_type].second);
  }
  if (content_type == kContentTypeExplicitVideo) {
    uint32_t granted;
    if (CheckUserPrivilege(user_index, kExplicitVideoPrivilege, &granted)) {
      // With parental controls the settings stand alone.
      if (!pc_enabled) {
        return X_ERROR_INVALID_PARAMETER;
      }
    } else {
      const uint32_t system_mask = granted ? 7 : 0;
      const uint32_t max_mask = granted ? 0xFF : 0;
      system = pc_enabled ? system & system_mask : system_mask;
      max = pc_enabled ? max & max_mask : max_mask;
    }
  }
  *rating_system = system;
  *max_rating = max;
  return X_ERROR_SUCCESS;
}

dword_result_t XamUserContentRestrictionGetRating_entry(
    dword_t user_index, dword_t content_type, lpdword_t rating_system_ptr,
    lpdword_t max_rating_ptr) {
  if (!rating_system_ptr || !max_rating_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  uint32_t rating_system, max_rating;
  const X_RESULT result = GetContentRestrictionRating(
      user_index, content_type, &rating_system, &max_rating);
  if (result) {
    return result;
  }
  *rating_system_ptr = rating_system;
  *max_rating_ptr = max_rating;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionGetRating, kUserProfiles, kStub);

dword_result_t XamUserContentRestrictionCheckAccess_entry(
    dword_t user_index, dword_t mode, dword_t content_type,
    dword_t rating_system, dword_t rating, lpdword_t access_ptr,
    dword_t overlapped_ptr) {
  if (!access_ptr ||
      (user_index >= XUserMaxUserCount && user_index != XUserIndexAny) ||
      mode > 3) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t user_rating_system, max_rating;
  const X_RESULT result = GetContentRestrictionRating(
      user_index, content_type, &user_rating_system, &max_rating);
  // Nothing rated is allowed.
  if (result == X_ERROR_NOT_FOUND) {
    *access_ptr = 1;
    return X_ERROR_SUCCESS;
  }
  if (result) {
    return result;
  }

  if (rating_system !=
      (user_rating_system > 0x40 ? 0x3F : user_rating_system)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // Unrated explicit video is denied too. A denied request in modes 1 and 2
  // asks for the passcode on a console, pending on the overlapped. Xenia has
  // no passcode, so it stays denied.
  *access_ptr = rating <= max_rating &&
                !(content_type == kContentTypeExplicitVideo && !rating);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionCheckAccess, kUserProfiles, kStub);

dword_result_t XamUserIsOnlineEnabled_entry(dword_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return 0;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return 0;
  }

  return kernel_state()
      ->xam_state()
      ->GetUserProfile(user_index)
      ->IsLiveEnabled();
}
DECLARE_XAM_EXPORT1(XamUserIsOnlineEnabled, kUserProfiles, kImplemented);

dword_result_t XamUserGetMembershipTier_entry(dword_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierNone;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierNone;
  }

  return kernel_state()
      ->xam_state()
      ->GetUserProfile(user_index)
      ->GetSubscriptionTier();
}
DECLARE_XAM_EXPORT1(XamUserGetMembershipTier, kUserProfiles, kImplemented);

dword_result_t XamUserGetMembershipTierFromXUID_entry(qword_t xuid) {
  const auto profile = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!profile) {
    return X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierNone;
  }

  return profile->GetSubscriptionTier();
}
DECLARE_XAM_EXPORT1(XamUserGetMembershipTierFromXUID, kUserProfiles,
                    kImplemented);

dword_result_t XamUserAreUsersFriends_entry(
    dword_t user_index, lpqword_t xuids_ptr, dword_t xuids_count,
    lpdword_t are_friends_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  bool are_friends = false;

  // 415607D2 provides are_friends_ptr and overlapped_ptr possibly a bug?
  assert_true(!overlapped_ptr);

  if (are_friends_ptr) {
    *are_friends_ptr = 0;
  }

  if (user_index >= XUserMaxUserCount) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    if (kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
      const auto& user_profile =
          kernel_state()->xam_state()->GetUserProfile(user_index);

      // Check if we are signed into live
      if (user_profile->signin_state() != 2) {
        result = X_ERROR_NOT_LOGGED_ON;
      } else {
        // No friends!
        result = X_ERROR_SUCCESS;
      }
    } else {
      result = X_ERROR_NO_SUCH_USER;
    }
  }

  if (overlapped_ptr) {
    assert_true(!are_friends_ptr);
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped_ptr,
        result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result), are_friends);

    return X_ERROR_SUCCESS;
  }

  if (!overlapped_ptr && are_friends_ptr) {
    *are_friends_ptr = are_friends;
  }

  return result;
}
DECLARE_XAM_EXPORT1(XamUserAreUsersFriends, kUserProfiles, kSketchy);

dword_result_t XamUserGetAgeGroup_entry(
    dword_t user_index, lpdword_t age_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!age_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  auto run = [user_index, age_ptr, overlapped_ptr](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_SUCCESS;

    *age_ptr = X_USER_AGE_GROUP::ADULT;

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = 0;

    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamUserGetAgeGroup, kUserProfiles, kImplemented);

dword_result_t XamUserCreateAchievementEnumerator_entry(
    dword_t title_id, dword_t user_index, qword_t xuid, dword_t flags,
    dword_t offset, dword_t count, lpdword_t buffer_size_ptr,
    lpdword_t handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  *buffer_size_ptr = static_cast<uint32_t>(entry_size * count);

  auto e = object_ref<XAchievementEnumerator>(
      new XAchievementEnumerator(kernel_state(), count, offset, flags));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  const auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint64_t requester_xuid = user->xuid();
  if (xuid) {
    requester_xuid = xuid;
  }

  const uint32_t title_id_ =
      title_id ? static_cast<uint32_t>(title_id) : kernel_state()->title_id();

  const auto user_title_achievements =
      kernel_state()->achievement_manager()->GetTitleAchievements(
          requester_xuid, title_id_);

  for (const auto& entry : user_title_achievements) {
    auto unlock_time = X_FILETIME();
    if (entry.IsUnlocked() && entry.unlock_time.is_valid()) {
      unlock_time = entry.unlock_time;
    }

    auto item = AchievementDetails(
        entry.achievement_id, entry.achievement_name.c_str(),
        entry.unlocked_description.c_str(), entry.locked_description.c_str(),
        entry.image_id, entry.gamerscore, unlock_time, entry.flags);

    e->AppendItem(item);
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateAchievementEnumerator, kUserProfiles,
                    kSketchy);

dword_result_t XamUserCreateTitlesPlayedEnumerator_entry(
    dword_t title_id, dword_t user_index, qword_t xuid, dword_t starting_index,
    dword_t game_count, lpdword_t buffer_size_ptr, lpdword_t handle_ptr) {
  if (user_index >= XUserMaxUserCount || !game_count || !buffer_size_ptr ||
      !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Both are written before the user is looked up.
  *buffer_size_ptr = game_count * sizeof(XTitleEnumerator::XTITLE_PLAYED);
  *handle_ptr = X_INVALID_HANDLE_VALUE;

  const auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_FUNCTION_FAILED;
  }

  uint64_t requester_xuid = user->xuid();
  if (xuid) {
    requester_xuid = xuid;
  }

  auto e = object_ref<XTitleEnumerator>(
      new XTitleEnumerator(kernel_state(), game_count, starting_index));

  auto result =
      e->Initialize(user_index, 0xFB, 0xB0050, 0xB000B, 0, 0x28, nullptr);

  if (XFAILED(result)) {
    return result;
  }

  const auto user_titles =
      kernel_state()->xam_state()->user_tracker()->GetPlayedTitles(
          requester_xuid);

  for (const auto& title : user_titles) {
    e->AppendItem(title);
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateTitlesPlayedEnumerator, kUserProfiles, kStub);

dword_result_t XamReadTile_entry(dword_t tile_type, dword_t title_id,
                                 qword_t item_id, dword_t user_index,
                                 lpdword_t output_ptr, lpdword_t buffer_size_ptr,
                                 pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto* kernel = kernel_state();
  TileRequestArguments args;
  args.type = tile_type; args.title = title_id; args.item = item_id; args.user = user_index;
  args.destination = output_ptr.guest_address(); args.size_address = buffer_size_ptr.guest_address();
  args.overlapped = overlapped_ptr.guest_address();
  X_HRESULT status;
  auto request = CaptureTileRequest(*kernel, args, status);
  if (!request) { return X_ERROR_FUNCTION_FAILED; }
  auto run = [kernel, request](uint32_t& error, uint32_t& length) {
    return CompleteTileRequest(*kernel, *request, error, length);
  };
  if (!overlapped_ptr) { uint32_t error = 0, length = 0; return run(error, length); }
  kernel->CompleteOverlappedDeferredEx(std::move(run), overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamReadTile, kUserProfiles, kSketchy);

dword_result_t XamReadTileEx_entry(dword_t tile_type, dword_t game_id,
                                   qword_t item_id, dword_t offset,
                                   dword_t unk1, dword_t unk2,
                                   lpdword_t output_ptr,
                                   lpdword_t buffer_size_ptr,
                                   pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (tile_type == 12 || unk1 || unk2) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return XamReadTile_entry(tile_type, game_id, item_id, offset, output_ptr,
                           buffer_size_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamReadTileEx, kUserProfiles, kSketchy);

dword_result_t XamParseGamerTileKey_entry(pointer_t<X_USER_DATA> key_ptr,
                                          lpdword_t title_id_ptr,
                                          lpdword_t big_tile_id_ptr,
                                          lpdword_t small_tile_id_ptr) {
  if (!key_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // xam ignores the type and the size it's given. It sets the size from the
  // string, up to 48 characters, then parses three 8 character fields at fixed
  // offsets with wcstoul, so a short or non-hex key still succeeds.
  const uint32_t string_address = key_ptr->data.unicode.ptr;
  const auto* string =
      string_address ? kernel_memory()->TranslateVirtual<const be<char16_t>*>(
                           string_address)
                     : nullptr;
  uint32_t length = 0;
  while (string && length < 48 && string[length]) {
    ++length;
  }
  key_ptr->data.unicode.size = string ? (length + 1) * sizeof(char16_t) : 0;

  uint32_t ids[3] = {};
  for (uint32_t i = 0; string && i < 3; ++i) {
    std::string field;
    for (uint32_t j = 0; j < 8; ++j) {
      const char16_t c = string[i * 8 + j];
      if (!c) {
        break;
      }
      field.push_back(c < 0x80 ? char(c) : '?');
    }
    ids[i] = uint32_t(std::strtoul(field.c_str(), nullptr, 16));
  }
  // No string, or an avatar key, gives the default picture.
  if (!string || IsGamerPictureAvatar(ids[0])) {
    ids[0] = kDashboardID;
    ids[1] = 0x00020000;
    ids[2] = 0x00010000;
  }

  if (title_id_ptr) {
    *title_id_ptr = ids[0];
  }
  if (big_tile_id_ptr) {
    *big_tile_id_ptr = ids[1];
  }
  if (small_tile_id_ptr) {
    *small_tile_id_ptr = ids[2];
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamParseGamerTileKey, kUserProfiles, kImplemented);

dword_result_t XamReadTileToTexture_entry(dword_t tile_type, dword_t title_id,
                                          qword_t tile_id, dword_t user_index,
                                          lpvoid_t buffer_ptr, dword_t stride,
                                          dword_t tile_height, dword_t overlapped_ptr) {
  auto* kernel = kernel_state();
  TileRequestArguments args;
  args.texture = true; args.type = tile_type; args.title = title_id; args.item = tile_id;
  args.user = user_index; args.destination = buffer_ptr.guest_address(); args.stride = stride;
  args.height = tile_height; args.overlapped = overlapped_ptr;
  X_HRESULT status;
  auto request = CaptureTileRequest(*kernel, args, status);
  if (!request) { return X_ERROR_FUNCTION_FAILED; }
  auto run = [kernel, request](uint32_t& error, uint32_t& length) {
    return CompleteTileRequest(*kernel, *request, error, length);
  };
  if (!overlapped_ptr) { uint32_t error = 0, length = 0; return run(error, length); }
  kernel->CompleteOverlappedDeferredEx(std::move(run), overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamReadTileToTexture, kUserProfiles, kSketchy);

dword_result_t XamReadTileToTextureEx_entry(dword_t type, dword_t title,
                                            qword_t item, dword_t user,
                                            dword_t extra28, dword_t extra2c,
                                            lpvoid_t destination,
                                            dword_t stride, dword_t height,
                                            dword_t overlapped) {
  if (type == 12 || extra28 || extra2c) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return XamReadTileToTexture_entry(type, title, item, user, destination,
                                    stride, height, overlapped);
}
DECLARE_XAM_EXPORT1(XamReadTileToTextureEx, kUserProfiles, kSketchy);

// Alias XUserAwardGamerPicture
dword_result_t XamWriteGamerTile_entry(
    dword_t user_index, dword_t title_id, dword_t big_tile_id,
    dword_t small_tile_id, dword_t for_enumerate,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_FUNCTION_FAILED;
  }

  // What is 0x10 flag?
  const uint32_t flags = (for_enumerate != 0 ? 0 : 0x10) | 1;

  const WriteTileType tile_type = static_cast<WriteTileType>((flags & 0xF));

  auto WriteGamerTileByKey = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
    if (!user) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_FUNCTION_FAILED;
    }

    const uint32_t content_title_id =
        title_id ? title_id.value() : kernel_state()->title_id();

    const std::string gamerpic_key =
        fmt::format("{:08x}{:08x}{:08x}", content_title_id, big_tile_id.value(),
                    small_tile_id.value());

    const std::string big_gamerpic_filename =
        fmt::format("64_{}.png", gamerpic_key);

    const std::string small_gamerpic_filename =
        fmt::format("32_{}.png", gamerpic_key);

    const std::string common_content_str = fmt::format("{:016X}", 0);
    const std::string content_type_str =
        fmt::format("{:08X}", uint32_t(XContentType::kGamerPicture));
    const std::string content_title_id_str =
        fmt::format("{:08x}", content_title_id);

    const std::filesystem::path gamer_pictures_storage_path =
        kernel_state()->emulator()->content_root() / common_content_str /
        kDashboardStringID / content_type_str / content_title_id_str;

    const std::filesystem::path big_gamerpic_path =
        gamer_pictures_storage_path / big_gamerpic_filename;

    const std::filesystem::path small_gamerpic_path =
        gamer_pictures_storage_path / small_gamerpic_filename;

    const auto gamerpic_big_png =
        kernel_state()->xam_state()->spa_info()->GetIcon(big_tile_id);

    const auto gamerpic_small_png =
        kernel_state()->xam_state()->spa_info()->GetIcon(small_tile_id);

    const std::error_code ec =
        xe::filesystem::CreateFolder(gamer_pictures_storage_path);

    FILE* big_gamerpic_file = xe::filesystem::OpenFile(big_gamerpic_path, "ab");
    FILE* small_gamerpic_file =
        xe::filesystem::OpenFile(small_gamerpic_path, "ab");

    X_RESULT result = X_ERROR_SUCCESS;

    if (ec || !big_gamerpic_file || !small_gamerpic_file) {
      extended_error = X_E_FUNCTION_FAILED;
      return X_ERROR_FUNCTION_FAILED;
    }

    fwrite(gamerpic_big_png.data(), 1, gamerpic_big_png.size(),
           big_gamerpic_file);
    fclose(big_gamerpic_file);

    fwrite(gamerpic_small_png.data(), 1, gamerpic_small_png.size(),
           small_gamerpic_file);
    fclose(small_gamerpic_file);

    XELOGI("Player: {} Unlocked Gamerpic: {}", user->name(),
           big_gamerpic_filename);

    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = WriteGamerTileByKey(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(WriteGamerTileByKey,
                                               overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamWriteGamerTile, kUserProfiles, kSketchy);

dword_result_t XamSessionCreateHandle_entry(lpdword_t handle_ptr) {
  auto session = object_ref<XSession>(new XSession(kernel_state()));
  const X_STATUS result = session->Initialize();
  if (XFAILED(result)) {
    return result;
  }
  *handle_ptr = session->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSessionCreateHandle, kUserProfiles, kImplemented);

// The caller drops the reference with ObDereferenceObject.
dword_result_t XamSessionRefObjByHandle_entry(dword_t handle,
                                              lpdword_t obj_ptr) {
  *obj_ptr = 0;
  auto session = kernel_state()->object_table()->LookupObject<XSession>(handle);
  if (!session) {
    return X_STATUS_INVALID_HANDLE;
  }
  session->RetainHandle();
  *obj_ptr = session->guest_object();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSessionRefObjByHandle, kUserProfiles, kImplemented);

dword_result_t XamUserIsUnsafeProgrammingAllowed_entry(dword_t user_index,
                                                       dword_t pc_check,
                                                       lpdword_t result_ptr) {
  if (!result_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index != XUserIndexAny && user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // ExGetXConfigSetting(3,0xf,&pc_flags,1,*size_out);
  // uint32_t result = XamUserCheckPrivilege_entry(user_index, 0xD4u,
  // result_ptr);

  *result_ptr = 1;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserIsUnsafeProgrammingAllowed, kUserProfiles, kStub);

dword_result_t XamUserGetSubscriptionType_entry(
    dword_t user_index, lpdword_t subscription_length_ptr,
    lpdword_t subscription_payment_ptr) {
  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }

  if (!subscription_length_ptr || !subscription_payment_ptr) {
    return X_E_INVALIDARG;
  }

  const auto user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);

  if (!user_profile) {
    return X_E_NO_SUCH_USER;
  }

  const auto [subscription_period, subscription_payment] =
      kernel_state()->xam_state()->user_tracker()->GetUserSubscriptionData(
          user_profile);

  *subscription_length_ptr = subscription_period;
  *subscription_payment_ptr = subscription_payment;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetSubscriptionType, kUserProfiles, kImplemented);

dword_result_t XamUserGetCachedUserFlags_entry(dword_t user_index) {
  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return 0;
  }

  const auto& user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);

  return user_profile->GetCachedFlags();
}
DECLARE_XAM_EXPORT1(XamUserGetCachedUserFlags, kUserProfiles, kImplemented);

dword_result_t XamUserGetUserFlags_entry(dword_t user_index) {
  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return 0;
  }

  const auto& user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);

  return user_profile->GetCachedFlags();
}
DECLARE_XAM_EXPORT1(XamUserGetUserFlags, kUserProfiles, kImplemented);

dword_result_t XamUserGetUserFlagsFromXUID_entry(qword_t xuid) {
  const auto& user_profile = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user_profile) {
    return 0;
  }

  return user_profile->GetCachedFlags();
}
DECLARE_XAM_EXPORT1(XamUserGetUserFlagsFromXUID, kUserProfiles, kImplemented);

// A profile without a language or country, like an offline one, gets the
// console's setting.
dword_result_t XamUserGetOnlineLanguageFromXUID_entry(qword_t xuid) {
  const auto& user = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user || !user->GetLanguage()) {
    return kernel_state()->xconfig()->ReadSetting<uint32_t>(
        XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE);
  }
  return user->GetLanguage();
}
DECLARE_XAM_EXPORT1(XamUserGetOnlineLanguageFromXUID, kUserProfiles,
                    kImplemented);

dword_result_t XamUserGetOnlineCountryFromXUID_entry(qword_t xuid) {
  const auto& user = kernel_state()->xam_state()->GetUserProfile(xuid);
  if (!user || !user->GetCountry()) {
    return kernel_state()->xconfig()->ReadSetting<uint8_t>(
        XCONFIG_USER_CATEGORY, XCONFIG_USER_COUNTRY);
  }
  return user->GetCountry();
}
DECLARE_XAM_EXPORT1(XamUserGetOnlineCountryFromXUID, kUserProfiles,
                    kImplemented);

dword_result_t XamUserIsParentalControlled_entry(dword_t user_index) {
  /* Notes:
      - if (data_address < 1 || XamExecutingOnBehalfOfTitle == 0 || title id ==
     kDashboardID || user_type != offline) (type mask used in XamUserGetXUID) go
     forward else return false
  */
  const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user || !user->IsParentalControlled()) {
    return false;
  }
  return true;
}
DECLARE_XAM_EXPORT1(XamUserIsParentalControlled, kUserProfiles, kImplemented);

dword_result_t XamUserCreateStatsEnumerator_entry(
    dword_t title_id, dword_t type, qword_t pivot, dword_t num_rows,
    dword_t num_specs,
    pointer_t<XUserStatsEnumerator::XUSER_STATS_SPEC> specs_ptr,
    lpdword_t buffer_size_ptr, lpdword_t handle_ptr) {
  if (!num_specs || !specs_ptr || !num_rows || num_rows > 100 ||
      !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // The buffer has room for every requested row: 0x30 bytes per row and 0x1C
  // per column.
  const XUserStatsEnumerator::XUSER_STATS_SPEC* specs = specs_ptr;
  std::vector<uint32_t> view_ids;
  uint32_t buffer_size = sizeof(XUserStatsEnumerator::XUSER_STATS_READ_RESULTS);
  for (uint32_t i = 0; i < num_specs; ++i) {
    view_ids.push_back(specs[i].ViewId);
    buffer_size += sizeof(XUserStatsEnumerator::XUSER_STATS_VIEW) +
                   num_rows * (0x30 + specs[i].NumColumnIds * 0x1C);
  }

  auto e = object_ref<XUserStatsEnumerator>(new XUserStatsEnumerator(
      kernel_state(), num_rows, std::move(view_ids), buffer_size));
  const X_STATUS result =
      e->Initialize(XUserIndexNone, 0xFB, 0xB0023, 0xB0024, 0);
  if (XFAILED(result)) {
    return result;
  }

  *buffer_size_ptr = buffer_size;
  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateStatsEnumerator, kUserProfiles, kSketchy);

dword_result_t XamUserGetUserTenure_entry(
    dword_t user_index, lpdword_t tenure_level_ptr, lpdword_t milestone_ptr,
    lpqword_t milestone_date_ptr, pointer_t<XAM_OVERLAPPED> overlap_ptr) {
  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile =
      kernel_state()->xam_state()->GetUserProfile(user_index);

  if (const auto setting =
          kernel_state()->xam_state()->user_tracker()->GetSetting(
              user_profile, kDashboardID,
              static_cast<uint32_t>(UserSettingId::XPROFILE_TENURE_LEVEL))) {
    *tenure_level_ptr = std::get<int32_t>(setting->get_host_data());
  }

  if (const auto setting =
          kernel_state()->xam_state()->user_tracker()->GetSetting(
              user_profile, kDashboardID,
              static_cast<uint32_t>(
                  UserSettingId::XPROFILE_TENURE_MILESTONE))) {
    *milestone_ptr = std::get<int32_t>(setting->get_host_data());
  }

  if (const auto setting =
          kernel_state()->xam_state()->user_tracker()->GetSetting(
              user_profile, kDashboardID,
              static_cast<uint32_t>(
                  UserSettingId::XPROFILE_TENURE_NEXT_MILESTONE_DATE))) {
    *milestone_date_ptr = std::get<int64_t>(setting->get_host_data());
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetUserTenure, kUserProfiles, kImplemented);

// Signs in or out the profile of each slot with a XUID, one XUID per slot.
static X_RESULT UserLogon(const std::vector<uint64_t>& xuids, uint32_t flags,
                          uint32_t overlapped_ptr) {
  auto run = [xuids, flags](uint32_t& extended_error,
                            uint32_t& length) -> X_RESULT {
    if (xe::global_critical_region::is_held_by_current_thread()) {
      extended_error = X_E_NOT_IMPLEMENTED;
      length = 0;
      return X_ERROR_FUNCTION_FAILED;
    }
    auto const profile_manager = kernel_state()->xam_state()->profile_manager();

    X_STATUS result = X_ERROR_SUCCESS;

    // The console adds each profile to the slot its XUID is in.
    if (flags & static_cast<uint32_t>(UserLogonFlags::AddUser)) {
      for (uint8_t user_index = 0; user_index < XUserMaxUserCount;
           user_index++) {
        if (xuids[user_index]) {
          profile_manager->Login(xuids[user_index], user_index, true);
          const auto actual = profile_manager->CaptureProfile(user_index);
          if (!actual || actual->xuid() != xuids[user_index]) {
            result = X_ERROR_FUNCTION_FAILED;
          }
        }
      }
    }

    if (flags & static_cast<uint32_t>(UserLogonFlags::RemoveUser)) {
      for (auto& xuid : xuids) {
        const uint8_t assigned_index =
            profile_manager->GetUserIndexAssignedToProfile(xuid);

        if (xuid) {
          if (kernel_state()->xam_state()->IsUserSignedIn(xuid)) {
            profile_manager->Logout(assigned_index, true);
          }
        }
      }
    }

    // Without Add or Remove the array is one XUID per slot, and only
    // ForceLiveLogOff signs out an empty slot.
    if (!(flags & (static_cast<uint32_t>(UserLogonFlags::AddUser) |
                   static_cast<uint32_t>(UserLogonFlags::RemoveUser)))) {
      const bool sign_out_empty =
          flags & static_cast<uint32_t>(UserLogonFlags::ForceLiveLogOff);
      for (uint8_t user_index = 0; user_index < XUserMaxUserCount;
           user_index++) {
        if (!xuids[user_index] && sign_out_empty &&
            kernel_state()->xam_state()->IsUserSignedIn(
                static_cast<uint32_t>(user_index))) {
          profile_manager->Logout(user_index, true);
        }
      }
      for (uint8_t user_index = 0; user_index < XUserMaxUserCount;
           user_index++) {
        if (xuids[user_index]) {
          profile_manager->Login(xuids[user_index], user_index, true);
          const auto actual = profile_manager->CaptureProfile(user_index);
          if (!actual || actual->xuid() != xuids[user_index]) {
            result = X_ERROR_FUNCTION_FAILED;
          }
        }
      }
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = 0;

    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length = 0;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}

// https://github.com/TeaModz/XeLiveStealth-Full-Source/blob/d4a7439ac6241c4a13e883a6f156623d1c08f6eb/XeLive/Utils.cpp#L416
dword_result_t XamUserLogon_entry(lpqword_t xuids_ptr, dword_t flags,
                                  pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!xuids_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::array<uint8_t, XUserMaxUserCount * 8> bytes{};
  if (XFAILED(util::ReadGuestIoBuffer(*kernel_memory(),
                                      xuids_ptr.guest_address(), bytes))) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::vector<uint64_t> xuids;
  xuids.reserve(XUserMaxUserCount);
  for (uint32_t i = 0; i != XUserMaxUserCount; ++i) {
    xuids.push_back(xe::load_and_swap<uint64_t>(bytes.data() + i * 8));
  }
  return UserLogon(xuids,
      flags, overlapped_ptr.guest_address());
}
DECLARE_XAM_EXPORT1(XamUserLogon, kUserProfiles, kImplemented);

// The console's XamUserLogon puts each XUID in an otherwise empty profile
// record and logs on with those as this does, so this takes a record per slot
// and logs on the profiles they name the same way. The dashboard signs in the
// profile chosen on its sign-in screen with it.
dword_result_t XamUserLogonEx_entry(pointer_t<X_PROFILEENUMRESULT> profiles_ptr,
                                    dword_t flags,
                                    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!profiles_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::array<uint8_t, sizeof(X_PROFILEENUMRESULT) * XUserMaxUserCount> bytes{};
  if (XFAILED(util::ReadGuestIoBuffer(*kernel_memory(),
                                      profiles_ptr.guest_address(), bytes))) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::vector<uint64_t> xuids;
  xuids.reserve(XUserMaxUserCount);
  for (uint32_t i = 0; i != XUserMaxUserCount; ++i) {
    X_PROFILEENUMRESULT profile{};
    std::memcpy(&profile, bytes.data() + i * sizeof(profile), sizeof(profile));
    xuids.push_back(profile.xuid_offline);
  }
  return UserLogon(xuids, flags, overlapped_ptr.guest_address());
}
DECLARE_XAM_EXPORT1(XamUserLogonEx, kUserProfiles, kImplemented);

// The last active user data is 8 bytes handed from one title to the next; a
// get consumes it.
static std::mutex last_active_user_data_mutex_;
static bool last_active_user_data_valid_ = false;
static uint64_t last_active_user_data_ = 0;

dword_result_t XamSetLastActiveUserData_entry(lpqword_t data_ptr) {
  std::lock_guard<std::mutex> lock(last_active_user_data_mutex_);
  if (data_ptr) {
    last_active_user_data_valid_ = true;
    last_active_user_data_ = *data_ptr;
  } else {
    last_active_user_data_valid_ = false;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(XamSetLastActiveUserData, kUserProfiles, kImplemented);

dword_result_t XamGetLastActiveUserData_entry(lpqword_t data_ptr) {
  std::lock_guard<std::mutex> lock(last_active_user_data_mutex_);
  if (!data_ptr || !last_active_user_data_valid_) {
    return 0;
  }
  *data_ptr = last_active_user_data_;
  last_active_user_data_valid_ = false;
  return 1;
}
DECLARE_XAM_EXPORT1(XamGetLastActiveUserData, kUserProfiles, kImplemented);

// Tokens are only handed out once XampXAuthStartup has started XAuth, which
// needs a profile signed in to LIVE and never succeeds here. A request that
// gets past its argument checks fails with ERROR_NOT_READY.
constexpr uint32_t kXamTokenMaxAppliesToLength = 0x400;
constexpr X_RESULT kErrorBufferOverflow = 0x6F;

dword_result_t XamGetAppliesTo_entry(lpvoid_t url_ptr, dword_t url_length,
                                     lpdword_t token_ptr) {
  if (!url_ptr || !token_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // The URL needs a scheme followed by "//" at its first slash.
  const std::string_view url(url_ptr.as<const char*>(), url_length);
  const size_t slash = url.find('/');
  if (url_length < 4 || url_length > kXamTokenMaxAppliesToLength ||
      slash == std::string_view::npos || !slash || url[slash - 1] != ':' ||
      url.substr(slash, 2) != "//") {
    return kErrorBufferOverflow;
  }
  return X_ERROR_NOT_READY;
}
DECLARE_XAM_EXPORT1(XamGetAppliesTo, kUserProfiles, kSketchy);

dword_result_t XamGetToken_entry(dword_t user_index, lpvoid_t applies_to_ptr,
                                 dword_t applies_to_length, lpdword_t token_ptr,
                                 pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // User index 4 gets past this and fails later, after XAuth's check.
  if ((user_index > XUserMaxUserCount && user_index != XUserIndexAny) ||
      !token_ptr || !applies_to_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  *token_ptr = 0;
  if (applies_to_length > kXamTokenMaxAppliesToLength) {
    return kErrorBufferOverflow;
  }
  // This fails before anything is queued and leaves the overlapped alone.
  return X_ERROR_NOT_READY;
}
DECLARE_XAM_EXPORT1(XamGetToken, kUserProfiles, kSketchy);

void XamFreeToken_entry(dword_t token_ptr) {
  if (token_ptr) {
    kernel_state()->memory()->SystemHeapFree(token_ptr);
  }
}
DECLARE_XAM_EXPORT1(XamFreeToken, kUserProfiles, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(User);
