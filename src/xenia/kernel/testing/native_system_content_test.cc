/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <set>
#include <string>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/native_system_content.h"
#include "xenia/kernel/native_system_ui_public_calls.h"

namespace xe::kernel::test {
namespace {
ValidatedGuestRuntimePlan ContentPlan() {
  ValidatedGuestRuntimePlan plan;
  for (const auto& profile : NativeSystemContentProfiles()) {
    GuestRuntime::Function function;
    function.identity = profile.identity;
    function.address = profile.address;
    function.arguments.resize(profile.argument_count);
    function.return_kind = profile.returns_void
                               ? GuestRuntime::ReturnKind::kVoid
                               : GuestRuntime::ReturnKind::kHResult;
    plan.catalog.emplace(function.identity, function);
    auto& contract = plan.contracts[function.identity];
    contract.caller = ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly;
    contract.result = profile.returns_void
                          ? ValidatedGuestRuntimePlan::ResultPolicy::kVoid
                          : ValidatedGuestRuntimePlan::ResultPolicy::kHResult;
  }
  return plan;
}

TEST_CASE(
    "Native content extension rejects partial or changed execution contracts",
    "[kernel][native_system_content]") {
  using F = NativeSystemContentPlanFamily;
  CHECK(CheckNativeSystemContentPlanFamily({}) == F::kAbsent);
  auto plan = ContentPlan();
  REQUIRE(CheckNativeSystemContentPlanFamily(plan) == F::kComplete);
  const std::string attach = "native-system-content-source-attach";
  SECTION("A legacy Guide plan has no content permission") {
    plan.catalog.clear();
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kAbsent);
  }
  SECTION("Partial content families cannot silently become legacy Guide") {
    plan.catalog.erase(attach);
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
  SECTION("An ordinary SYSTEM caller is not an actual native Task") {
    plan.contracts.at(attach).caller =
        ValidatedGuestRuntimePlan::CallerPolicy::kSystemOnly;
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
  SECTION("Original TITLE callers cannot initialize system sources") {
    plan.contracts.at(attach).caller =
        ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem;
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
  SECTION("A fixed source constant cannot replace actual source observation") {
    plan.catalog.at(attach).arguments[0] = 1;
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
  SECTION("HRESULT failure must not be accepted as any return") {
    plan.contracts.at(attach).result =
        ValidatedGuestRuntimePlan::ResultPolicy::kAny;
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
  SECTION("The prototype must match the parser's HRESULT return kind") {
    plan.catalog.at(attach).return_kind = GuestRuntime::ReturnKind::kUInt32;
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
  SECTION("Deferred source calls cannot become heap initializers") {
    plan.plan.phases.push_back({"bad-initializer", {plan.catalog.at(attach)}});
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
  SECTION("Unknown additions do not acquire native source authority") {
    auto function = plan.catalog.at(attach);
    function.identity = "native-system-content-pretend-ready";
    plan.catalog.emplace(function.identity, function);
    CHECK(CheckNativeSystemContentPlanFamily(plan) == F::kInvalid);
  }
}

TEST_CASE("Zeroed native manager memory is not a constructed package service",
          "[kernel][native_system_content]") {
  NativeSystemContentManagerData data;
  CHECK_FALSE(data.PristineConstructor());
  data.list_links = {0x81AAC1D4, 0x81AAC1D4, 0x81AAC1DC, 0x81AAC1DC};
  REQUIRE(data.PristineConstructor());
  data.sources[0] = {0, 1, 1, 0};
  CHECK_FALSE(data.PristineConstructor());
  REQUIRE(data.HasBuiltinSource(1));
  SECTION("A duplicate device does not prove one attachment") {
    data.sources[1] = data.sources[0];
    CHECK_FALSE(data.HasBuiltinSource(1));
  }
  SECTION("Removable and builtin source contracts are distinct") {
    data.sources[0].type = 2;
    CHECK_FALSE(data.HasBuiltinSource(1));
  }
  SECTION("A path override cannot masquerade as the fixed builtin root") {
    data.sources[0].path = 0x90010000;
    CHECK_FALSE(data.HasBuiltinSource(1));
  }
  SECTION("Extended partition files are a separate source family") {
    data.sources[0].extended_file = 1;
    CHECK_FALSE(data.HasBuiltinSource(1));
  }
}

TEST_CASE("Native index output must stay inside readonly system content",
          "[kernel][native_system_content]") {
  constexpr std::string_view prefix =
      "\\Device\\Harddisk0\\SystemExtPartition\\Content\\";
  constexpr std::string_view relative =
      "0000000000000000\\FFFE07DF\\00008000\\FFFE07DF00000002";
  const auto path = std::string(prefix) + std::string(relative);
  REQUIRE(NativeSystemContentRelativePackagePath(path));
  CHECK(*NativeSystemContentRelativePackagePath(path) ==
        "0000000000000000/FFFE07DF/00008000/FFFE07DF00000002");
  CHECK(NativeSystemContentRelativePackagePath(
      "\\device\\harddisk0\\systemextpartition\\content\\0000000000000000\\fffe"
      "07df\\00008000\\fffe07df00000002"));
  for (const auto& invalid :
       {"\\Device\\Harddisk0\\Partition1\\Content\\" + std::string(relative),
        std::string(prefix) + "..\\" + std::string(relative),
        std::string(prefix) + std::string(relative) + "\\extra",
        std::string(prefix) +
            "0000000000000001\\FFFE07DF\\00008000\\FFFE07DF00000002",
        std::string(prefix) + "0000000000000000\\FFFE07DF\\00008000\\..",
        std::string(prefix) +
            "0000000000000000/FFFE07DF/00008000/FFFE07DF00000002",
        std::string(prefix) +
            "0000000000000000\\FFFE07DF\\00008000\\FFFE07DF00000002:stream"}) {
    CAPTURE(invalid);
    CHECK_FALSE(NativeSystemContentRelativePackagePath(invalid));
  }
}

TEST_CASE("Native storage readiness needs its real initialized ownership",
          "[kernel][native_system_content]") {
  NativeSystemStorageData data;
  REQUIRE_FALSE(data.Pristine());
  REQUIRE_FALSE(data.FormattedBuiltinHdd());
  data.table_zero = true;
  data.next_device = 1;
  data.work_event = {0x01000000, 0, 0x81A81350, 0x81A81350};
  data.acknowledgement = {0, 1, 0x81A81360, 0x81A81360};
  REQUIRE(data.Pristine());
  SECTION("A previous owned task cannot be silently initialized twice") {
    data.worker_task = 0x40000101;
    REQUIRE_FALSE(data.Pristine());
  }
  SECTION("Zero is not the native initial device-ID sequence") {
    data.next_device = 0;
    REQUIRE_FALSE(data.Pristine());
  }
  SECTION("A formatted slot is a separate observation from zeroed storage") {
    data.table_zero = false;
    data.volumes[0] = {"\\Device\\Harddisk0\\Partition1\\", 1, 1, 3};
    REQUIRE_FALSE(data.Pristine());
    REQUIRE(data.FormattedBuiltinHdd() == 1);
    data.volumes[1] = data.volumes[0];
    REQUIRE_FALSE(data.FormattedBuiltinHdd());
  }
  SECTION("An unformatted native device cannot satisfy source readiness") {
    data.volumes[0] = {"\\Device\\Harddisk0\\Partition1\\", 1, 1, 4};
    REQUIRE_FALSE(data.FormattedBuiltinHdd());
  }
}

TEST_CASE("Avatar catalog documents prerequisites without enabling its routes",
          "[kernel][native_system_content][native_avatar_catalog]") {
  std::set<uint16_t> ordinals;
  size_t avatars = 0;
  size_t notifications = 0;
  size_t managed_io = 0;
  for (const auto& profile : NativeSystemUiPublicProfiles()) {
    REQUIRE(ordinals.insert(uint16_t(profile.call)).second);
    if (profile.family == NativeSystemPublicFamily::kUiInput) {
      CHECK(NativeSystemPublicFamilyRouteEnabled(profile.family));
      continue;
    }
    CHECK_FALSE(NativeSystemPublicFamilyRouteEnabled(profile.family));
    CHECK(profile.prerequisites & kOwnedSystemRuntime);
    if (profile.family == NativeSystemPublicFamily::kNotifications) {
      ++notifications;
      const uint8_t expected_mask =
          profile.call == NativeSystemUiPublicCall::kNotifyQueue ? 0x14 :
          profile.call == NativeSystemUiPublicCall::kNotifyQueueEx ? 0x84 : 0;
      CHECK(profile.uint64_register_mask == expected_mask);
      CHECK(profile.prerequisites & kNativeNonmodalPresentation);
      CHECK(profile.prerequisites & kNativeNotificationCue);
      CHECK(profile.prerequisites & kNativeNotificationPreferences);
      CHECK(profile.prerequisites & kCurrentLocalUserProvider);
      continue;
    }
    if (profile.family == NativeSystemPublicFamily::kManagedFormatIo) {
      ++managed_io;
      CHECK(profile.prerequisites & kNativeTaskTransport);
      CHECK(profile.uint64_register_mask == 0);
      continue;
    }
    ++avatars;
    CHECK(profile.uint64_register_mask == 0);
  }
  CHECK(avatars == 12);
  CHECK(managed_io == 2);
  CHECK(notifications == 6);
  const auto* init =
      NativeSystemUiPublicProfile(NativeSystemUiPublicCall::kAvatarInitialize);
  REQUIRE(init);
  CHECK(init->prerequisites & kSystemContentIndexAndOpen);
  const auto* signed_out = NativeSystemUiPublicProfile(
      NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile);
  REQUIRE(signed_out);
  CHECK(signed_out->family == NativeSystemPublicFamily::kAvatarProfiles);
  CHECK(signed_out->prerequisites & kPersistentProfileProvider);
  CHECK_FALSE(signed_out->prerequisites & kNativeAvatarEngineReady);
}
}  // namespace
}  // namespace xe::kernel::test
