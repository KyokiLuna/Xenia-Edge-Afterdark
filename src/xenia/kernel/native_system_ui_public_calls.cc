/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_ui_public_calls.h"
namespace xe::kernel {
namespace {
constexpr NativeSystemUiPublicCallProfile profiles[] = {
    {NativeSystemUiPublicCall::kManagedFormatStart,
     "native-managed-format-start-original", 0x816A2CF0, 656,
     "e00dfeaa77f3f52c68a5f89f5bea3f6a259c38f2fb3e15ae02f922803f75339a", 6, 0,
     NativeSystemPublicFamily::kManagedFormatIo,
     kOwnedSystemRuntime | kNativeTaskTransport},
    {NativeSystemUiPublicCall::kManagedFormatCancel,
     "native-managed-format-cancel-original", 0x816A26F8, 352,
     "b1af64f353766148e34db02ed938743371561d2ff3d694bb5a8209e5c205051a", 2, 0,
     NativeSystemPublicFamily::kManagedFormatIo,
     kOwnedSystemRuntime | kNativeTaskTransport},
    {NativeSystemUiPublicCall::kInputGetCapabilities,
     "native-public-input-caps", 0x816F2E70, 96,
     "eea2d728a36c5e98887269827095659b611d559f363fcf45710197a9837057b0", 3},
    {NativeSystemUiPublicCall::kInputGetState, "native-public-input-state",
     0x816F14E8, 468,
     "218d2b2479d523401f38d90586d24013dca12b3540ea298dcea691c2f2681785", 3},
    {NativeSystemUiPublicCall::kInputSetState, "native-public-input-set",
     0x816F1740, 160,
     "c0ece20911d25d5e6e0427b7a65257dae22451cfc2ef8b293f93404617bb7ed6", 6},
    {NativeSystemUiPublicCall::kInputGetKeystroke,
     "native-public-input-keystroke", 0x816F4AB8, 40,
     "4575febad65bb4842f9fa58e04f02e9dd98ce9319b9b63dd12491201824f169e", 3},
    {NativeSystemUiPublicCall::kInputGetKeystrokeEx,
     "native-public-input-keystroke-ex", 0x816F4778, 828,
     "cad0c322f7489a999baaf9ee37119d22b391d94ea6a2aaf7d196868bea20f0b5", 3},
    {NativeSystemUiPublicCall::kEnableSystemAppInput,
     "native-public-input-enable", 0x816F2480, 348,
     "2248bd3097104a24fa194c970351b5f047bdb8324263e7093713b17641aa8f7b", 2},
    {NativeSystemUiPublicCall::kIsUiActive, "native-public-is-ui-active",
     0x816C7D78, 16,
     "7d6eef6ba06629576aeb7ae0989ca39a5b8a7ad4f9f5d0998a8008cdbc69584f", 0},
    {NativeSystemUiPublicCall::kShowGuide, "native-public-show-guide",
     0x816C3580, 12,
     "b0a1a25d253a4712bf3ffd09a735fbb990bcda72dff82c708f159868445c3afa", 1},
    {NativeSystemUiPublicCall::kShowKeyboard, "native-public-show-keyboard",
     0x816C1F38, 132,
     "9e20591bb745e86753a3fac242be606741345d934395d0edb6d14c5438661ae5", 8},
    {NativeSystemUiPublicCall::kShowMessageBox, "native-public-show-messagebox",
     0x816C5508, 536,
     "f28d164282ae1eec032b1a22e7bd87151d43332ae108a6530f263ebccb46c6b8", 9},
    {NativeSystemUiPublicCall::kShowDeviceSelector,
     "native-public-show-device-selector", 0x816C5300, 36,
     "07ff8ca536b92c61acb8e2834efb441076ed14bd3376c0483b5400b270abdb45", 6,
     1u << 3},
    {NativeSystemUiPublicCall::kNotifyPosition, "native-public-notify-position",
     0x816aba78, 80,
     "a8c34508acd463f7b349ef43883632d57d4a3621d1ccb0c25cdf91251a70823b", 1, 0x0,
     NativeSystemPublicFamily::kNotifications,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeNonmodalPresentation |
         kNativeNotificationCue | kNativeNotificationPreferences |
         kCurrentLocalUserProvider},
    {NativeSystemUiPublicCall::kNotifyDelay, "native-public-notify-delay",
     0x816abb28, 8,
     "9b515d1bfacfec6986872f41c01ce5400ae5a42f70d862af0fb0b55b24a4e70a", 1, 0x0,
     NativeSystemPublicFamily::kNotifications,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeNonmodalPresentation |
         kNativeNotificationCue | kNativeNotificationPreferences |
         kCurrentLocalUserProvider},
    {NativeSystemUiPublicCall::kNotifyQueue, "native-public-notify-queue",
     0x816aac08, 20,
     "53f6713b28488b29e00a3e237faf6e56ffd9f151af1c2622e7b87b48902b7854", 5,
     0x14, NativeSystemPublicFamily::kNotifications,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeNonmodalPresentation |
         kNativeNotificationCue | kNativeNotificationPreferences |
         kCurrentLocalUserProvider},
    {NativeSystemUiPublicCall::kNotifySetOptions,
     "native-public-notify-set-options", 0x816abb38, 132,
     "23887d384fb656c7a5fd7e951050e35b04bd25c1bf16078ab0091c895dcd07a2", 4, 0x0,
     NativeSystemPublicFamily::kNotifications,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeNonmodalPresentation |
         kNativeNotificationCue | kNativeNotificationPreferences |
         kCurrentLocalUserProvider},
    {NativeSystemUiPublicCall::kNotifyGetOptions,
     "native-public-notify-get-options", 0x816abbc0, 80,
     "677fa8e7796c8caee34f092c77a6397d820d3d4570753819df19130e3b2b10e5", 4, 0x0,
     NativeSystemPublicFamily::kNotifications,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeNonmodalPresentation |
         kNativeNotificationCue | kNativeNotificationPreferences |
         kCurrentLocalUserProvider},
    {NativeSystemUiPublicCall::kNotifyQueueEx, "native-public-notify-queue-ex",
     0x816a9de8, 128,
     "fe1db216e8f6c40a9a1fda94a7733f582a07dfc1231c870b55d6b9c152e16cd7", 8,
     0x84, NativeSystemPublicFamily::kNotifications,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeNonmodalPresentation |
         kNativeNotificationCue | kNativeNotificationPreferences |
         kCurrentLocalUserProvider},
    {NativeSystemUiPublicCall::kAvatarInitialize,
     "native-public-avatar-initialize", 0x816755E8, 228,
     "4b7eceaaaee759f0ac9585337c83635793118744e823eebccb6b1261706bb919", 6, 0,
     NativeSystemPublicFamily::kAvatarEngine,
     kOwnedSystemRuntime | kNativeTaskTransport | kSystemContentIndexAndOpen},
    {NativeSystemUiPublicCall::kAvatarShutdown, "native-public-avatar-shutdown",
     0x81674940, 12,
     "29ef149b5c35c7293435787d1e5452bd7512979291def903f8398aa8507d6517", 0, 0,
     NativeSystemPublicFamily::kAvatarEngine,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeAvatarDrain},
    {NativeSystemUiPublicCall::kAvatarGetManifestLocalUser,
     "native-public-avatar-get-manifest-local-user", 0x81674950, 112,
     "17a7a28f96a7b41cff8bdabe93005204dd70c8c6d91b3f80ae798666318b8f9a", 3, 0,
     NativeSystemPublicFamily::kAvatarProfiles,
     kOwnedSystemRuntime | kNativeTaskTransport | kPersistentProfileProvider},
    {NativeSystemUiPublicCall::kAvatarGetAssetsResultSize,
     "native-public-avatar-get-assets-result-size", 0x81674A70, 104,
     "679af48429b8a772df8fcc0a5cd9f2e2a08948d39140a3e33d1c9466a29a32da", 3, 0,
     NativeSystemPublicFamily::kAvatarEngine,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeAvatarEngineReady},
    {NativeSystemUiPublicCall::kAvatarGetAssets,
     "native-public-avatar-get-assets", 0x81674AD8, 148,
     "61530c9835f633f75976229959fef2d9349fcfcf9502ca3bcd338518f7462487", 6, 0,
     NativeSystemPublicFamily::kAvatarEngine,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeAvatarEngineReady},
    {NativeSystemUiPublicCall::kAvatarSetManifest,
     "native-public-avatar-set-manifest", 0x81674DE0, 124,
     "7bfbf817fe0b93b940533db13adc5021055a5f4afac1119e3eb17fe42f26f984", 3, 0,
     NativeSystemPublicFamily::kAvatarProfiles,
     kOwnedSystemRuntime | kNativeTaskTransport | kPersistentProfileProvider},
    {NativeSystemUiPublicCall::kAvatarGetMetadataRandom,
     "native-public-avatar-get-metadata-random", 0x81675258, 152,
     "1d258cad8c81e3eeaf0e41b3eb4e534533866ec6426952da397e9246b1172c79", 4, 0,
     NativeSystemPublicFamily::kAvatarEngine,
     kOwnedSystemRuntime | kNativeTaskTransport | kNativeAvatarEngineReady},
    {NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfileCount,
     "native-public-avatar-get-metadata-signed-out-profile-count", 0x816752F0,
     100, "1ec3c7580d78f1d013895b6a165aabc23d387f677329b88738d4b24e02777beb", 2,
     0, NativeSystemPublicFamily::kAvatarProfiles,
     kOwnedSystemRuntime | kNativeTaskTransport | kPersistentProfileProvider},
    {NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile,
     "native-public-avatar-get-metadata-signed-out-profile", 0x81675358, 112,
     "7b23e0c61b74c91ab4d3b9530728c6567c390cff15e02a8397f29196d856a724", 3, 0,
     NativeSystemPublicFamily::kAvatarProfiles,
     kOwnedSystemRuntime | kNativeTaskTransport | kPersistentProfileProvider},
    {NativeSystemUiPublicCall::kAvatarManifestGetBodyType,
     "native-public-avatar-manifest-get-body-type", 0x816753C8, 48,
     "8106aac6d3b87c22abf7b9843660513d49b5edf2deb7864ebb7a624521e15120", 1, 0,
     NativeSystemPublicFamily::kAvatarManifest, kOwnedSystemRuntime},
    {NativeSystemUiPublicCall::kShowAvatarMiniCreator,
     "native-public-show-avatar-mini-creator", 0x816C45F8, 128,
     "30c6eb6ba6414ad0637a7e92e4e725ada2c51e4f5f179312c366c58716c3db29", 2, 0,
     NativeSystemPublicFamily::kAvatarMiniCreator,
     kOwnedSystemRuntime | kNativeTaskTransport | kPersistentProfileProvider |
         kSystemContentIndexAndOpen | kNativeUiOwnership},
    {NativeSystemUiPublicCall::kLaunchAvatarEditor,
     "native-public-launch-avatar-editor", 0x816E2ED0, 124,
     "18f12d1e2d01f5f0b0b39cee2fff39700367b197c825609247cf6dec9976cb6a", 3, 0,
     NativeSystemPublicFamily::kAvatarEditor,
     kOwnedSystemRuntime | kNativeTaskTransport | kPersistentProfileProvider |
         kSystemContentIndexAndOpen | kNativeTitleHandoff},
};
}  // namespace
std::span<const NativeSystemUiPublicCallProfile>
NativeSystemUiPublicProfiles() {
  return profiles;
}
bool NativeSystemPublicFamilyRouteEnabled(NativeSystemPublicFamily family) {
  return family == NativeSystemPublicFamily::kUiInput;
}
const NativeSystemUiPublicCallProfile* NativeSystemUiPublicProfile(
    NativeSystemUiPublicCall call) {
  for (const auto& profile : profiles) {
    if (profile.call == call) {
      return &profile;
    }
  }
  return nullptr;
}
bool NativeSystemUiPublicArgumentsValid(NativeSystemUiPublicCall call,
                                        std::span<const uint64_t> arguments) {
  const auto* profile = NativeSystemUiPublicProfile(call);
  if (!profile || arguments.size() != profile->argument_count) {
    return false;
  }
  for (size_t i = 0; i < arguments.size(); ++i) {
    const bool wide = i < 8 && (profile->uint64_register_mask & (1u << i));
    if (!wide && arguments[i] > UINT32_MAX) {
      return false;
    }
  }
  return true;
}
}  // namespace xe::kernel
