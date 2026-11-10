/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/xbox.h"

DEFINE_bool(dashboard_game_library, true,
            "List the game library as the dashboard's games. Off lists the "
            "game packages installed in content instead.",
            "Content");

namespace xe {
namespace kernel {
namespace xam {

// A record through license_mask, which is all of it before xam 4532.
constexpr size_t kLegacyContentRecordSize = 0x148;

// True when the title was built for a xam before 4532, whose title 0 lists
// every title without kAllTitlesFlag.
static bool BuiltForLegacyXam() {
  auto exe_module = kernel_state()->GetExecutableModule();
  if (!exe_module || !exe_module->xex_module()) {
    return false;
  }
  for (const auto& library : *exe_module->xex_module()->import_libraries()) {
    if (library.name == "xam") {
      return library.version.build < 4532;
    }
  }
  return false;
}

// Alias XContentCreateCrossTitleEnumerator. Title 0 lists the content of every
// title, type 0xFFFFFFFF that of every type and XUID 0 that of every user.
dword_result_t XamContentAggregateCreateEnumerator_entry(qword_t xuid,
                                                         dword_t device_id,
                                                         dword_t content_type,
                                                         dword_t title_id,
                                                         lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0
                         ? std::optional<StorageDeviceInfo>{}
                         : GetStorageDeviceInfo(kernel_state(), device_id);
  if ((device_id && !device_info) || !handle_out) {
    return X_E_INVALIDARG;
  }
  if (device_info && !device_info->available) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const bool legacy = BuiltForLegacyXam();
  auto e = make_object<ContentAggregateEnumerator>(
      kernel_state(), 1,
      legacy ? kLegacyContentRecordSize : sizeof(XCONTENT_DATA_INTERNAL));
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(XUserIndexAny, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }

  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  // Fetch any alternate title IDs defined in the XEX header
  // (used by games to load saves from other titles, etc)
  std::vector<uint32_t> alt_ids;
  auto exe_module = kernel_state()->GetExecutableModule();
  if (exe_module && exe_module->xex_module()) {
    alt_ids = exe_module->xex_module()->opt_alternate_title_ids();
  }
  std::vector<uint32_t> own_title_ids{kernel_state()->title_id()};
  std::ranges::copy(alt_ids, std::back_inserter(own_title_ids));

  // The dashboard lists the game library in place of the game packages.
  const bool library_games = cvars::dashboard_game_library && !title_id &&
                             (xuid == 0 || xuid == -1) &&
                             kernel_state()->title_id() == kDashboardID;

  auto content_manager = kernel_state()->content_manager();
  const auto hdd = GetStorageDeviceInfo(
      kernel_state(), static_cast<uint32_t>(DummyDeviceId::HDD));
  const bool library_visible =
      library_games && hdd && hdd->available &&
      (!device_id || device_id == static_cast<uint32_t>(DummyDeviceId::HDD));

  auto append_device = [&](uint32_t device_index, uint64_t device_xuid) {
    std::vector<uint32_t> title_ids;
    if (title_id) {
      title_ids.push_back(title_id.value());
      std::ranges::copy(alt_ids, std::back_inserter(title_ids));
    } else {
      title_ids =
          content_manager->ListContentTitleIds(device_index, device_xuid);
    }
    for (const uint32_t item_title_id : title_ids) {
      // Title 0 only reaches past the title's own content with
      // ContentAggregateEnumerator::kAllTitlesFlag.
      const bool other_title =
          !legacy && !title_id &&
          std::ranges::find(own_title_ids, item_title_id) ==
              own_title_ids.end();
      std::vector<XContentType> types;
      if (content_type == 0xFFFFFFFF) {
        types = content_manager->ListContentTypes(device_index, device_xuid,
                                                  item_title_id);
      } else {
        types.push_back(static_cast<XContentType>(content_type.value()));
      }
      for (const XContentType type : types) {
        const auto content_datas =
            device_index == static_cast<uint32_t>(DummyDeviceId::ODD)
                ? content_manager->ListContentODD(device_index, device_xuid,
                                                  item_title_id, type)
                : content_manager->ListContent(device_index, device_xuid,
                                               item_title_id, type);
        for (const auto& content_data : content_datas) {
          if (library_visible &&
              content_manager->LibraryContainsPhysicalContent(device_xuid,
                                                              content_data)) {
            continue;
          }
          e->AppendItem(content_data, other_title);
        }
      }
    }
  };

  for (const auto& device : ListStorageDevices(kernel_state(), true)) {
    if (device_id && device.device_id != device_id) {
      continue;
    }
    if (device.device_type == DeviceType::ODD) {
      append_device(device.device_id, 0);
      continue;
    }
    // XUID -1 only lists the content shared by every user.
    const std::vector<uint64_t> xuids =
        xuid == 0 ? content_manager->ListContentXuids(device.device_id)
                  : std::vector<uint64_t>{
                        xuid == -1 ? 0 : static_cast<uint64_t>(xuid)};
    for (const uint64_t hdd_xuid : xuids) {
      append_device(device.device_id, hdd_xuid);
    }
    if (library_games &&
        device.device_id == static_cast<uint32_t>(DummyDeviceId::HDD)) {
      for (const auto& content_data : content_manager->ListLibraryContent()) {
        if (content_type == 0xFFFFFFFF ||
            static_cast<uint32_t>(content_data.content_type.get()) ==
                content_type) {
          e->AppendItem(content_data, !legacy);
        }
      }
    }
  }

  XELOGD(
      "XamContentAggregateCreateEnumerator: added {} items to enumerator, "
      "{:X}-byte records",
      e->item_count(), e->item_size());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentAggregateCreateEnumerator, kContent, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(ContentAggregate);
