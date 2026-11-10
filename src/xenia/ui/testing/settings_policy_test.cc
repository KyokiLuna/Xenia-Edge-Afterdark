/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/config.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/ui/config_helpers.h"

namespace xe::ui::test {
namespace {

TEST_CASE("Settings hide only audited internal names and preserve real choices",
          "[settings][ownership]") {
  for (const char* name : {"default_achievements_backend",
                           "logged_profile_slot_0_xuid",
                           "logged_profile_slot_1_xuid",
                           "logged_profile_slot_2_xuid",
                           "logged_profile_slot_3_xuid",
                           "system_readonly_source_root",
                           "host_storage_protected_root",
                           "managed_device_state_root",
                           "native_system_ui",
                           "native_system_ui_startup_plan",
                           "native_system_ui_trace",
                           "native_guide_bootstrap_probe",
                           "native_xui_probe",
                           "native_system_first_record_probe",
                           "native_system_first_record_report",
                           "native_file_io_contract_report",
                           "native_notification_preferences_probe_report",
                           "native_notification_queue_probe_report",
                           "native_notification_render_probe_report",
                           "native_xui_runtime_plan",
                           "native_xui_render_probe",
                           "native_guide_scene_probe",
                           "native_xui_record_capture",
                           "native_hud_controller_probe",
                           "native_xui_target_probe",
                           "native_system_device_binding_probe",
                           "native_gpu_context_probe",
                           "native_guide_host_probe",
                           "native_guide_real_time_probe",
                           "native_frame_lease_probe",
                           "native_frame_execute_probe",
                           "native_frame_composite_probe",
                           "native_system_ui_output_capture_base",
                           "native_system_ui_output_capture_source",
                           "native_system_ui_output_capture_b_down",
                           "kernel_build_version",
                           "staging_mode",
                           "data_center_mode"}) {
    CAPTURE(name);
    bool active = false;
    cvar::ConfigVar<bool> variable(name, &active, "", "", "Kernel", false,
                                   false);
    CHECK_FALSE(IsUserEditableCvar(variable));
    CHECK_FALSE(IsUserEditableCvar(variable, true));
    CHECK_FALSE(CanOverrideCvarForGame(variable));
    CHECK_FALSE(variable.is_transient());
  }
  for (const char* name :
       {"gpu", "apu", "hid", "volume", "apu_max_queued_frames", "fullscreen",
        "present_letterbox", "font_size", "custom_font_path", "ui_locale",
        "keyboard_passthrough", "vibration", "left_stick_deadzone_percentage",
        "right_stick_deadzone_percentage", "xefu_path", "xefu_launcher",
        "license_mask", "apply_title_update", "in_process_title_relaunch",
        "storage_request_timing", "user_language", "user_country", "audio_flag",
        "widescreen", "video_standard", "use_50Hz_mode", "xmp_default_volume",
        // Keep current fallback controls until real native replacements land.
        "guide_button", "dashboard_game_library", "allow_avatar_initialization",
        "show_achievement_notification",
        "achievement_notification_position_by_game", "achievement_sound_path",
        // An unreviewed future native option is not hidden by prefix/category.
        "native_texture_copy", "logged_profile_slot_debug_visualization"}) {
    CAPTURE(name);
    bool active = false;
    cvar::ConfigVar<bool> variable(name, &active, "", "", "Kernel.Debug", false,
                                   false);
    CHECK(IsUserEditableCvar(variable));
    CHECK(IsUserEditableCvar(variable, true));
  }
  for (const char* name : {"storage_root", "content_root", "cache_root"}) {
    bool active = false;
    cvar::ConfigVar<bool> variable(name, &active, "", "", "Storage", false,
                                   false);
    CHECK(IsUserEditableCvar(variable));
    CHECK_FALSE(IsUserEditableCvar(variable, true));
    CHECK_FALSE(CanOverrideCvarForGame(variable));
  }
  bool active = false;
  cvar::ConfigVar<bool> transient("storage_selection_dialog", &active, "", "",
                                  "UI", false, true);
  cvar::ConfigVar<bool> config("defaults_date", &active, "", "", "Config",
                               false, false);
  CHECK_FALSE(IsUserEditableCvar(transient));
  CHECK_FALSE(CanOverrideCvarForGame(transient));
  CHECK_FALSE(IsUserEditableCvar(config));
  CHECK_FALSE(CanOverrideCvarForGame(config));
}

class ConfigFixture {
 public:
  explicit ConfigFixture(std::map<std::string, cvar::IConfigVar*>& variables)
      : prior_registry_(cvar::ConfigVars),
        prior_folder_(config::config_folder),
        prior_suffix_(config::game_config_suffix),
        prior_warnings_(cvar::config_type_mismatch_warnings) {
    const auto serial =
        std::chrono::steady_clock::now().time_since_epoch().count();
    folder = std::filesystem::temp_directory_path() /
             ("xenia-settings-policy-" + std::to_string(serial));
    REQUIRE(std::filesystem::create_directory(folder));
    cvar::ConfigVars = &variables;
    cvar::config_type_mismatch_warnings = &warnings_;
    config::config_folder = folder;
    config::game_config_suffix = ".config.toml";
  }
  ~ConfigFixture() {
    cvar::ConfigVars = prior_registry_;
    cvar::config_type_mismatch_warnings = prior_warnings_;
    config::config_folder = prior_folder_;
    config::game_config_suffix = prior_suffix_;
    std::error_code error;
    std::filesystem::remove_all(folder, error);
  }
  std::filesystem::path WriteTitle(uint32_t title_id) {
    // A bounded, genuine XEX2 optional execution header. No guest execution or
    // SPA/title-resource parsing is needed by the actual config loader.
    std::array<uint8_t, 0x80> bytes{};
    const auto be = [&](size_t at, uint32_t value) {
      for (size_t i = 0; i < 4; ++i) {
        bytes[at + i] = uint8_t(value >> (24 - i * 8));
      }
    };
    be(0, 0x58455832);
    be(8, bytes.size());
    be(20, 1);
    be(24, XEX_HEADER_EXECUTION_INFO);
    be(28, 0x40);
    be(0x4C, title_id);
    auto path = folder / (std::to_string(title_id) + ".xex");
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.close();
    REQUIRE(stream.good());
    return path;
  }
  static std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  std::filesystem::path folder;

 private:
  decltype(cvar::ConfigVars) prior_registry_;
  std::filesystem::path prior_folder_;
  std::string prior_suffix_;
  decltype(cvar::config_type_mismatch_warnings) prior_warnings_;
  std::vector<std::string> warnings_;
};

TEST_CASE(
    "Actual game config files cannot replace global identity or runtime "
    "ownership",
    "[settings][ownership][config]") {
  std::string profile_value, plan_value, storage_value, handoff_value,
      gpu_value, system_source_value;
  bool native_value = false, notification_value = true;
  int32_t license_value = 0, kernel_value = 17559;
  cvar::ConfigVar<std::string> profile("logged_profile_slot_1_xuid",
                                       &profile_value, "", "", "Profiles",
                                       false, false);
  cvar::ConfigVar<std::string> plan("native_system_ui_startup_plan",
                                    &plan_value, "", "", "Kernel", false,
                                    false);
  cvar::ConfigVar<bool> native("native_system_ui", &native_value, "", "",
                               "Kernel", false, false);
  cvar::ConfigVar<std::string> storage("storage_root", &storage_value, "", "",
                                       "Storage", false, false);
  cvar::ConfigVar<std::string> system_source("system_readonly_source_root",
                                             &system_source_value, "", "",
                                             "Storage", false, false);
  std::string protected_value = "/verified/project", state_value = "/verified/parent";
  cvar::ConfigVar<std::string> protected_root("host_storage_protected_root", &protected_value,
                                               "", "", "Storage", false, false);
  cvar::ConfigVar<std::string> state_root("managed_device_state_root", &state_value,
                                           "", "", "Storage", false, false);
  cvar::ConfigVar<std::string> handoff(
      "slot_bindings_passthrough", &handoff_value, "", "", "HID", false, true);
  cvar::ConfigVar<std::string> gpu("gpu", &gpu_value, "", "", "GPU", false,
                                   false);
  cvar::ConfigVar<int32_t> license("license_mask", &license_value, "", "",
                                   "Content", false, false);
  cvar::ConfigVar<int32_t> kernel("kernel_build_version", &kernel_value, "", "",
                                  "Kernel", false, false);
  cvar::ConfigVar<bool> notification("show_achievement_notification",
                                     &notification_value, "", "", "UI", false,
                                     false);
  profile.SetConfigValue("E000000000000001");
  plan.SetConfigValue("/verified/startup.json");
  native.SetConfigValue(true);
  storage.SetConfigValue("/owned/console");
  handoff.SetConfigValue("physical-controller");
  gpu.SetConfigValue("vulkan");
  std::map<std::string, cvar::IConfigVar*> variables;
  for (auto* variable : std::vector<cvar::IConfigVar*>{
           &profile, &plan, &native, &storage, &handoff, &gpu, &license,
           &kernel, &notification, &system_source, &protected_root, &state_root}) {
    variables.emplace(variable->name(), variable);
  }
  ConfigFixture fixture(variables);
  constexpr uint32_t title_id = 0x5841ABCD;
  const auto title = fixture.WriteTitle(title_id);
  toml::table file = toml::parse(R"(
[Profiles]
logged_profile_slot_1_xuid = "E000000000000002"
[Kernel]
native_system_ui = false
native_system_ui_startup_plan = "/untrusted/other.json"
kernel_build_version = 999
[Storage]
storage_root = "/another/console"
system_readonly_source_root = "/untrusted/shared/system"
host_storage_protected_root = "/untrusted/project"
managed_device_state_root = "/untrusted/parent"
[HID]
slot_bindings_passthrough = "other-controller"
[Content]
license_mask = 1
[GPU]
gpu = "null"
[UI]
show_achievement_notification = false
)");
  config::SaveGameConfig(title_id, file);
  const auto config_path = fixture.folder / "config" / "5841ABCD.config.toml";
  const auto before = ConfigFixture::Read(config_path);
  CHECK(config::ResolveInt32GameConfig(title_id, "license_mask") == 1);
  CHECK(config::ResolveInt32GameConfig(title_id, "kernel_build_version") ==
        17559);
  REQUIRE(config::LoadGameConfigForFile(title) == title_id);
  CHECK(profile_value == "E000000000000001");
  CHECK(plan_value == "/verified/startup.json");
  CHECK(native_value);
  CHECK(storage_value == "/owned/console");
  CHECK(system_source_value.empty());  // Unpinned default remains local.
  CHECK(system_source.GetTypedConfigValue().empty());
  CHECK(protected_value == "/verified/project");
  CHECK(state_value == "/verified/parent");
  CHECK(handoff_value == "physical-controller");
  CHECK(kernel_value == 17559);
  CHECK(license_value == 1);
  CHECK(gpu_value == "null");
  CHECK_FALSE(notification_value);  // The current fallback was not retired.
  CHECK(profile.GetTypedConfigValue() == "E000000000000001");
  CHECK(plan.GetTypedConfigValue() == "/verified/startup.json");
  CHECK(gpu.GetTypedConfigValue() == "vulkan");
  CHECK(ConfigFixture::Read(config_path) == before);  // Read does not rewrite.

  // Explicit CLI remains highest priority for allowed and global-only values.
  license.SetCommandLineValue(0);
  kernel.SetCommandLineValue(1888);
  system_source.SetCommandLineValue("/verified/read-only-system");
  CHECK(config::ResolveInt32GameConfig(title_id, "license_mask") == 0);
  CHECK(config::ResolveInt32GameConfig(title_id, "kernel_build_version") ==
        1888);
  REQUIRE(config::LoadGameConfigForFile(title) == title_id);
  CHECK(license_value == 0);
  CHECK(kernel_value == 1888);
  CHECK(system_source_value == "/verified/read-only-system");
  const auto second = fixture.WriteTitle(title_id + 1);
  REQUIRE(config::LoadGameConfigForFile(second) == title_id + 1);
  CHECK(gpu_value == "vulkan");
  CHECK(notification_value);
  CHECK(system_source_value == "/verified/read-only-system");
  CHECK(profile_value == "E000000000000001");
}

TEST_CASE(
    "User settings reset preserves hidden persisted state and global "
    "diagnostics",
    "[settings][ownership][config]") {
  std::string profile_value, plan_value;
  int32_t volume_value = 100;
  cvar::ConfigVar<std::string> profile("logged_profile_slot_0_xuid",
                                       &profile_value, "", "", "Profiles",
                                       false, false);
  cvar::ConfigVar<std::string> plan("native_system_ui_startup_plan",
                                    &plan_value, "", "", "Kernel", false,
                                    false);
  cvar::ConfigVar<int32_t> volume("volume", &volume_value, "", "", "APU", false,
                                  false);
  profile.SetConfigValue("E000000000000001");
  plan.SetConfigValue("/verified/startup.json");
  volume.SetConfigValue(42);
  for (auto* variable :
       std::vector<cvar::IConfigVar*>{&profile, &plan, &volume}) {
    if (IsUserEditableCvar(*variable)) {
      variable->ResetConfigValueToDefault();
    }
  }
  CHECK(profile_value == "E000000000000001");
  CHECK(plan_value == "/verified/startup.json");
  CHECK(volume_value == 100);
  CHECK_FALSE(profile.is_transient());
  CHECK_FALSE(plan.is_transient());
  // Hiding is not deprecation: a global config/diagnostic reload still works.
  const auto reload = toml::parse(R"(value = "/new/verified.json")");
  plan.LoadConfigValue(reload.get("value"));
  CHECK(plan_value == "/new/verified.json");
  CHECK(plan.GetTypedConfigValue() == "/new/verified.json");
}

}  // namespace
}  // namespace xe::ui::test
