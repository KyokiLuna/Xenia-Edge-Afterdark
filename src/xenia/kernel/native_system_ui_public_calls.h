/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_UI_PUBLIC_CALLS_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_UI_PUBLIC_CALLS_H_
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include "xenia/base/native_ui_backend_route.h"
namespace xe::kernel {
enum class NativeSystemUiPublicCall : uint16_t {
  kManagedFormatStart = 0x1FC,
  kManagedFormatCancel = 0x1F8,
  kInputGetCapabilities = 0x190,
  kInputGetState = 0x191,
  kInputSetState = 0x192,
  kInputGetKeystroke = 0x193,
  kInputGetKeystrokeEx = 0x198,
  kEnableSystemAppInput = 0x196,
  kIsUiActive = 0x2EC,
  kShowGuide = 0x304,
  kShowKeyboard = 0x2C1,
  kShowMessageBox = 0x2CA,
  kShowDeviceSelector = 0x2CB,
  kNotifyPosition = 0x28C,
  kNotifyDelay = 0x28D,
  kNotifyQueue = 0x290,
  kNotifySetOptions = 0x292,
  kNotifyGetOptions = 0x293,
  kNotifyQueueEx = 0x298,
  kAvatarInitialize = 0x5DC,
  kAvatarShutdown = 0x5DD,
  kAvatarGetManifestLocalUser = 0x5DE,
  kAvatarGetAssetsResultSize = 0x5E0,
  kAvatarGetAssets = 0x5E1,
  kAvatarSetManifest = 0x5E4,
  kAvatarGetMetadataRandom = 0x5EB,
  kAvatarGetMetadataSignedOutProfileCount = 0x5EE,
  kAvatarGetMetadataSignedOutProfile = 0x5EF,
  kAvatarManifestGetBodyType = 0x5F1,
  kShowAvatarMiniCreator = 0x6A4,
  kLaunchAvatarEditor = 0x7DB,
};
enum class NativeSystemPublicFamily {
  kUiInput,
  kNotifications,
  kAvatarEngine,
  kAvatarProfiles,
  kAvatarManifest,
  kAvatarMiniCreator,
  kAvatarEditor,
  // Exact owned IO scope only; globally disabled like all non-UI families.
  kManagedFormatIo
};
enum NativeSystemPublicPrerequisite : uint32_t {
  kOwnedSystemRuntime = 1u << 0,
  kNativeTaskTransport = 1u << 1,
  kPersistentProfileProvider = 1u << 2,
  kSystemContentIndexAndOpen = 1u << 3,
  kNativeAvatarEngineReady = 1u << 4,
  kNativeUiOwnership = 1u << 5,
  kNativeTitleHandoff = 1u << 6,
  kNativeAvatarDrain = 1u << 7,
  kNativeNonmodalPresentation = 1u << 8,
  kNativeNotificationCue = 1u << 9,
  kNativeNotificationPreferences = 1u << 10,
  kCurrentLocalUserProvider = 1u << 11,
};
struct NativeSystemUiPublicCallProfile {
  NativeSystemUiPublicCall call;
  std::string_view identity;
  uint32_t address, code_size;
  std::string_view code_sha256;
  size_t argument_count;
  // Logical positions whose complete r3..r10 value is part of the ABI.
  // Every other argument is a checked DWORD, including guest pointers.
  uint8_t uint64_register_mask = 0;
  NativeSystemPublicFamily family = NativeSystemPublicFamily::kUiInput;
  uint32_t prerequisites = 0;
};
// Exact supplied17559 native entry/prototype. No arbitrary function identity,
// address, host pointer or caller/process substitution is accepted.
const NativeSystemUiPublicCallProfile* NativeSystemUiPublicProfile(
    NativeSystemUiPublicCall);
std::span<const NativeSystemUiPublicCallProfile> NativeSystemUiPublicProfiles();
// Avatar catalog entries are source contracts only until the owned runtime has
// concrete provider/index/engine admission. A caller cannot assert readiness.
bool NativeSystemPublicFamilyRouteEnabled(NativeSystemPublicFamily);
bool NativeSystemUiPublicArgumentsValid(NativeSystemUiPublicCall,
                                        std::span<const uint64_t>);
struct NativeSystemUiPublicCallResult {
  enum class Disposition {
    kInactive,
    kReturned,
    kDenied,
    kExecutionFailed,
    kHostFallback
  };
  Disposition disposition = Disposition::kInactive;
  uint32_t value = 0;
  std::shared_ptr<NativeUiBackendRoute::HostLease> host_fallback;
  bool handled() const {
    return disposition != Disposition::kInactive &&
           disposition != Disposition::kHostFallback;
  }
};
}  // namespace xe::kernel
#endif
