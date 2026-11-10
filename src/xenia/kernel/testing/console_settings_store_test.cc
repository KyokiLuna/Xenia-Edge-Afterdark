/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/console_settings_store.h"
#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/config.h"
#include "xenia/kernel/native_notification_preference_coordinator.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_xconfig.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/memory.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
#include <sys/file.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
DECLARE_int32(video_standard);
DECLARE_bool(use_50Hz_mode);
DECLARE_int32(user_language);
DECLARE_bool(widescreen);
DECLARE_uint32(audio_flag);
DECLARE_int32(xmp_default_volume);

namespace xe::kernel::test {
namespace {
constexpr ConsoleSettingKey kRetail{3, 0xC}, kOpaque{0x7FFF, 0xBEEF};
using Bytes = std::vector<uint8_t>;
void Put(Bytes& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    bytes.push_back(uint8_t(value >> ((count - i - 1) * 8)));
  }
}
Bytes FixtureFile(std::vector<std::pair<ConsoleSettingKey, Bytes>> records,
                  uint32_t version = 1) {
  Bytes bytes{'X', 'C', 'F', 'G'};
  Put(bytes, version, 4);
  Put(bytes, 1, 8);
  bytes.resize(48, 0);
  Put(bytes, records.size(), 4);
  for (const auto& [key, value] : records) {
    Put(bytes, key.category, 2);
    Put(bytes, key.setting, 2);
    Put(bytes, value.size(), 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  std::array<uint8_t, 32> hash{};
  REQUIRE(mbedtls_sha256(bytes.data(), bytes.size(), hash.data(), 0) == 0);
  bytes.insert(bytes.end(), hash.begin(), hash.end());
  return bytes;
}
class StoreFixture {
 public:
  StoreFixture() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-console-settings-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
    std::error_code error;
    space = filesystem::HostStorageSpace::ForPath(
        path, 4096, error, [this](const auto&, std::error_code& error) {
          error.clear();
          return std::filesystem::space_info{1ull << 30, available.load(),
                                             available.load()};
        });
    REQUIRE_FALSE(error);
    root = filesystem::HostStorageRoot::Open(path, false, error, 4096, space);
    REQUIRE_FALSE(error);
    REQUIRE(root);
  }
  ~StoreFixture() {
    root.reset();
    space.reset();
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::shared_ptr<ConsoleSettingsStore> Open(
      ConsoleSettingsStore::Publish hook = {},
      ConsoleSettingsStore::StageName stage_name = {}) {
    std::error_code error;
    auto store = ConsoleSettingsStore::Open(root, error, std::move(hook),
                                            std::move(stage_name));
    REQUIRE_FALSE(error);
    REQUIRE(store);
    return store;
  }
  Bytes Read(std::string_view name = "xconfig.bin") {
    std::ifstream stream(path / name, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void ExternalWrite(const Bytes& bytes,
                     std::string_view name = "xconfig.bin") {
    std::ofstream stream(path / name, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.close();
    REQUIRE(stream.good());
  }
  std::filesystem::path path;
  std::atomic<uint64_t> available{512ull << 20};
  std::shared_ptr<filesystem::HostStorageSpace> space;
  std::shared_ptr<filesystem::HostStorageRoot> root;
};
TEST_CASE(
    "Console preferences preserve unknown records and all RetailFlags "
    "bits across restart",
    "[xconfig][console_store]") {
  StoreFixture fixture;
  const Bytes opaque{0, 0xFF, 0x12, 0x80, 0x33};
  fixture.ExternalWrite(
      FixtureFile({{kRetail, {0xA5, 0xA5, 0xDE, 0xAD}}, {kOpaque, opaque}}));
  auto store = fixture.Open();
  uint32_t value = 0;
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == 0xA5A5DEAD);
  const auto changed = store->ModifyU32(kRetail, 0, 0xFFFFBDCF, 0x4020);
  REQUIRE_FALSE(changed.error);
  CHECK(changed.published);
  CHECK(changed.durable);
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == ((0xA5A5DEAD & 0xFFFFBDCF) | 0x4020));
  Bytes read;
  REQUIRE_FALSE(store->Read(kOpaque, read));
  CHECK(read == opaque);
  std::error_code error;
  auto other = ConsoleSettingsStore::Open(fixture.root, error);
  REQUIRE_FALSE(error);
  REQUIRE(other);
  REQUIRE_FALSE(other->ReadU32(kRetail, 0, value));
  CHECK(value == ((0xA5A5DEAD & 0xFFFFBDCF) | 0x4020));
  REQUIRE_FALSE(other->ModifyU32(kRetail, 0, 0xFFFFFFFF, 0x40000000).error);
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == (((0xA5A5DEAD & 0xFFFFBDCF) | 0x4020) | 0x40000000));
  other.reset();
  store.reset();
  store = fixture.Open();
  REQUIRE_FALSE(store->ReadU32(kRetail, 0xFFFFFFFF, value));
  CHECK(value == (((0xA5A5DEAD & 0xFFFFBDCF) | 0x4020) | 0x40000000));
  REQUIRE_FALSE(store->Read(kOpaque, read));
  CHECK(read == opaque);
  // Repeated edits retain only current plus the verified previous generation.
  for (uint32_t i = 0; i < 8; ++i) {
    REQUIRE_FALSE(store->WriteU32(kRetail, i).error);
  }
  size_t backups = 0, staged = 0;
  for (const auto& file : std::filesystem::directory_iterator(fixture.path)) {
    const auto name = file.path().filename().string();
    backups += name.starts_with("xconfig.backup.");
    staged += name.starts_with("xconfig.stage.");
  }
  CHECK(backups == 1);
  CHECK(staged == 0);
}
TEST_CASE(
    "Console RMW serializes concurrent bits and enforces shared "
    "admission without changing old bytes",
    "[xconfig][console_store]") {
  StoreFixture fixture;
  auto store = fixture.Open();
  uint32_t value = 0;
  REQUIRE_FALSE(store->ReadU32(kRetail, 0x40, value));
  CHECK(value == 0x40);
  CHECK_FALSE(std::filesystem::exists(fixture.path / "xconfig.bin"));
  std::atomic<uint32_t> failures{0};
  std::vector<std::thread> threads;
  for (uint32_t i = 0; i < 16; ++i) {
    threads.emplace_back([store, i, &failures] {
      if (store->ModifyU32(kRetail, 0, 0xFFFFFFFF, 1u << i).error) {
        ++failures;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(failures == 0);
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == 0xFFFF);
  const auto before = fixture.Read();
  fixture.available = 4096;
  const auto failed = store->ModifyU32(kRetail, 0, 0, 0x12345678);
  CHECK(failed.error == std::errc::no_space_on_device);
  CHECK_FALSE(failed.published);
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == 0xFFFF);
  CHECK(fixture.Read() == before);
  // Opening an existing store is a read, even when admission is exhausted.
  store.reset();
  store = fixture.Open();
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == 0xFFFF);
  // A concurrent cooperating process owns the OS lock only for its call.
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::error_code lock_error;
  auto external = fixture.root->OpenFile(
      "xconfig.lock", filesystem::FileAccess::kFileReadData, lock_error);
  REQUIRE_FALSE(lock_error);
  REQUIRE(external);
  REQUIRE(flock(int(external->native_handle()), LOCK_EX | LOCK_NB) == 0);
  value = 0x12345678;
  CHECK(store->ReadU32(kRetail, 0, value) ==
        std::errc::device_or_resource_busy);
  CHECK(value == 0x12345678);
  REQUIRE(flock(int(external->native_handle()), LOCK_UN) == 0);
  external.reset();
#endif
  std::error_code read_error;
  auto readonly = fixture.root->Subroot({}, true, read_error);
  REQUIRE_FALSE(read_error);
  REQUIRE(readonly);
  auto reader = ConsoleSettingsStore::Open(readonly, read_error);
  REQUIRE_FALSE(read_error);
  REQUIRE(reader);
  REQUIRE_FALSE(reader->ReadU32(kRetail, 0, value));
  CHECK(value == 0xFFFF);
  CHECK(reader->WriteU32(kRetail, 0).error == std::errc::read_only_file_system);
  reader.reset();
  fixture.root->Retire();
  CHECK(store->ReadU32(kRetail, 0, value));
  CHECK(store->WriteU32(kRetail, 0).error);
}
TEST_CASE(
    "Console persistence reports prepublication and "
    "published-but-not-durable failures truthfully",
    "[xconfig][console_store]") {
  StoreFixture fixture;
  auto store = fixture.Open();
  REQUIRE_FALSE(store->WriteU32(kRetail, 0x11111111).error);
  const auto old = fixture.Read();
  store.reset();
  store = fixture.Open([](const auto&, const auto&, const auto&, const auto&) {
    return filesystem::HostStorageRoot::ReplaceResult{
        std::make_error_code(std::errc::permission_denied), false, false};
  });
  auto result = store->WriteU32(kRetail, 0x22222222);
  CHECK(result.error == std::errc::permission_denied);
  CHECK_FALSE(result.published);
  CHECK(fixture.Read() == old);
  uint32_t value;
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == 0x11111111);
  store.reset();
  // Isolated publication seam executes the REAL transaction, then injects the
  // reported post-publication directory-flush error. Not a power-loss test.
  store = fixture.Open([](const auto& root, const auto& staged,
                          const auto& target, const auto& backup) {
    auto result = root.AtomicReplaceFile(staged, target, backup);
    if (result.published) {
      result.error = std::make_error_code(std::errc::io_error);
    }
    return result;
  });
  result = store->WriteU32(kRetail, 0x33333333);
  CHECK(result.error == std::errc::io_error);
  CHECK(result.published);
  CHECK_FALSE(result.durable);
  CHECK(fixture.Read() != old);
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == 0x33333333);
  result = store->WriteU32(kRetail,
                           0x33333333);  // Retry actual directory durability.
  CHECK_FALSE(result.error);
  CHECK_FALSE(result.published);
  CHECK(result.durable);
  store.reset();
  store = fixture.Open();
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, value));
  CHECK(value == 0x33333333);
}
TEST_CASE("Failed stage admission never deletes an uncertain recovery file",
          "[xconfig][console_store][recovery]") {
  StoreFixture fixture;
  auto first = fixture.Open();
  REQUIRE_FALSE(first->WriteU32(kRetail, 0xA5A5A5A5).error);
  const auto current = fixture.Read();
  first.reset();
  constexpr std::string_view recovery = "xconfig.stage.existing-recovery";
  const Bytes saved{0x53, 0x54, 0x41, 0x47, 0x45, 0x00, 0xFE, 0xA5};
  fixture.ExternalWrite(saved, recovery);
  uint32_t names = 0, publishes = 0;
  auto store = fixture.Open(
      [&publishes](const auto&, const auto&, const auto&, const auto&) {
        ++publishes;
        return filesystem::HostStorageRoot::ReplaceResult{};
      },
      [&names] {
        ++names;
        return std::filesystem::path("xconfig.stage.existing-recovery");
      });
  SECTION("Pre-open reserve failure does not establish stage ownership") {
    fixture.available = 4096;
    const auto result = store->WriteU32(kRetail, 0x11223344);
    CHECK(result.error == std::errc::no_space_on_device);
    CHECK_FALSE(result.published);
    CHECK_FALSE(result.durable);
    CHECK(names == 1);
  }
  SECTION("Exclusive-create collision never deletes someone else's stage") {
    const auto result = store->WriteU32(kRetail, 0x11223344);
    CHECK(result.error == std::errc::file_exists);
    CHECK_FALSE(result.published);
    CHECK(names == 16);
  }
  CHECK(publishes == 0);
  CHECK(fixture.Read(recovery) == saved);
  CHECK(fixture.Read() == current);
  uint32_t flags = 0;
  REQUIRE_FALSE(store->ReadU32(kRetail, 0, flags));
  CHECK(flags == 0xA5A5A5A5);
}
TEST_CASE(
    "Console store rejects corruption, duplicate records and displaced "
    "current state without rewriting",
    "[xconfig][console_store]") {
  for (int kind = 0; kind < 5; ++kind) {
    CAPTURE(kind);
    StoreFixture fixture;
    auto bytes =
        FixtureFile({{kRetail, {0, 0, 0, 0x40}}, {kOpaque, {1, 2, 3}}});
    if (kind == 0) {
      bytes.back() ^= 1;
    }
    if (kind == 1) {
      bytes.resize(51);
    }
    if (kind == 2) {
      bytes = FixtureFile({{kRetail, {0, 0, 0, 0x40}}}, 2);
    }
    if (kind == 3) {
      bytes =
          FixtureFile({{kRetail, {0, 0, 0, 0x40}}, {kRetail, {1, 2, 3, 4}}});
    }
    if (kind == 4) {
      bytes = FixtureFile({{kRetail, {1, 2, 3}}});
    }
    fixture.ExternalWrite(bytes);
    std::error_code error;
    CHECK_FALSE(ConsoleSettingsStore::Open(fixture.root, error));
    CHECK(error == std::errc::bad_message);
    CHECK(fixture.Read() == bytes);
  }
  {
    StoreFixture empty;
    auto first = empty.Open();
    empty.ExternalWrite({});
    const auto refused = first->WriteU32(kRetail, 0x40);
    CHECK(refused.error == std::errc::bad_message);
    CHECK_FALSE(refused.published);
    CHECK(empty.Read().empty());
  }
  StoreFixture fixture;
  auto store = fixture.Open();
  REQUIRE_FALSE(store->WriteU32(kRetail, 0x40).error);
  const auto changed =
      FixtureFile({{kRetail, {1, 2, 3, 4}}, {kOpaque, {8, 9}}});
  fixture.ExternalWrite(changed);
  const auto refused = store->WriteU32(kRetail, 0x12345678);
  CHECK(refused.error == std::errc::state_not_recoverable);
  CHECK_FALSE(refused.published);
  CHECK(fixture.Read() == changed);
}
TEST_CASE(
    "RetailFlags migrates only global or CLI defaults and ignores "
    "per-title host presentation",
    "[xconfig][console_store]") {
  StoreFixture fixture;
  auto store = fixture.Open();
  bool active = false;
  cvar::ConfigVar<bool> legacy("allow_nui_initialization", &active, "", "",
                               "Console", false, false);
  legacy.SetConfigValue(true);
  legacy.SetGameConfigValue(false);
  std::map<std::string, cvar::IConfigVar*> registry{{legacy.name(), &legacy}};
  auto* prior = cvar::ConfigVars;
  cvar::ConfigVars = &registry;
  struct Restore {
    decltype(prior) value;
    ~Restore() { cvar::ConfigVars = value; }
  } restore{prior};
  XConfig config(store);
  uint32_t value;
  REQUIRE_FALSE(config.ReadRetailFlags(value));
  CHECK(value == (X_RETAIL_FLAGS::DashboardInitialized |
                  X_RETAIL_FLAGS::KinectInitialized));
  CHECK_FALSE(
      active);  // A console read did not change a title's compatibility cvar.
  REQUIRE(config.ModifySettingUlong(XCONFIG_USER_CATEGORY,
                                    XCONFIG_USER_RETAIL_FLAGS, 0xFFFFBDCF,
                                    0x4020) == X_STATUS_SUCCESS);
  REQUIRE_FALSE(config.ReadRetailFlags(value));
  const auto persisted = value;
  legacy.SetCommandLineValue(false);
  XConfig next(store);
  REQUIRE_FALSE(next.ReadRetailFlags(value));
  CHECK(value == persisted);
  CHECK_FALSE(std::filesystem::exists(fixture.path / "config"));
  CHECK(next.ModifySettingUlong(XCONFIG_CONSOLE_CATEGORY,
                                XCONFIG_CONSOLE_RETAIL_EX_FLAGS, 0,
                                0) == X_STATUS_NOT_SUPPORTED);
  // An absent record does not import an active per-game value, while an
  // explicit CLI compatibility value can seed a previously unconfigured
  // console.
  legacy.OverrideConfigValue(false);
  legacy.SetGameConfigValue(true);
  {
    StoreFixture fresh;
    XConfig separate(fresh.Open());
    REQUIRE_FALSE(separate.ReadRetailFlags(value));
    CHECK(value == X_RETAIL_FLAGS::DashboardInitialized);
  }
  legacy.SetCommandLineValue(true);
  {
    StoreFixture fresh;
    XConfig separate(fresh.Open());
    REQUIRE_FALSE(separate.ReadRetailFlags(value));
    CHECK(value == (X_RETAIL_FLAGS::DashboardInitialized |
                    X_RETAIL_FLAGS::KinectInitialized));
  }
  XConfig unavailable({}, std::make_error_code(std::errc::permission_denied));
  CHECK(unavailable.ReadRetailFlags(value) == std::errc::permission_denied);
  CHECK(unavailable.ModifySettingUlong(XCONFIG_USER_CATEGORY,
                                       XCONFIG_USER_RETAIL_FLAGS, 0,
                                       0) == X_STATUS_ACCESS_DENIED);
}
TEST_CASE(
    "Legacy DWORD RMW preserves translator masks and exact per-title config",
    "[xconfig][legacy_rmw][config]") {
  StoreFixture fixture;
  struct Restore {
    std::filesystem::path folder = config::config_folder;
    std::string suffix = config::game_config_suffix;
    int32_t standard = cvars::video_standard;
    bool pal50 = cvars::use_50Hz_mode;
    int32_t language = cvars::user_language;
    bool wide = cvars::widescreen;
    uint32_t audio = cvars::audio_flag;
    int32_t music = cvars::xmp_default_volume;
    ~Restore() {
      config::config_folder = folder;
      config::game_config_suffix = suffix;
      cvars::video_standard = standard;
      cvars::use_50Hz_mode = pal50;
      cvars::user_language = language;
      cvars::widescreen = wide;
      cvars::audio_flag = audio;
      cvars::xmp_default_volume = music;
    }
  } restore;
  config::config_folder = fixture.path / "legacy-config";
  config::game_config_suffix = ".config.toml";
  constexpr uint32_t target = 0x58411111, other = 0x58412222;
  toml::table initial{{"Console", toml::table{{"unrelated", "keep"}}},
                      {"GPU", toml::table{{"resolution_scale", 2}}}};
  config::SaveGameConfig(target, initial);
  config::SaveGameConfig(other, toml::table{{"Other", "unchanged"}});
  const auto read = [](const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream), {});
  };
  const auto other_path =
      config::config_folder / "config" / "58412222.config.toml";
  const auto other_before = read(other_path);
  uint32_t saves = 0;
  XConfig legacy(
      {}, {}, [&saves](const char* name, XConfig::LegacySettingValue value) {
        ++saves;
        auto table = config::LoadGameConfig(target);
        auto* console = config::ResolveSectionTable(table, "Console");
        if (!console) {
          return std::make_error_code(std::errc::invalid_argument);
        }
        std::visit([&](auto typed) { console->insert_or_assign(name, typed); },
                   value);
        // Real legacy serializer and target path; no invented KernelState.
        config::SaveGameConfig(target, table);
        return std::error_code{};
      });
  cvars::video_standard = 1;
  cvars::use_50Hz_mode = false;
  cvars::user_language = 1;
  cvars::widescreen = false;
  cvars::audio_flag = 0x00010001;
  cvars::xmp_default_volume = 75;
  CHECK(legacy.ModifySettingUlong(XCONFIG_SECURED_CATEGORY,
                                  XCONFIG_SECURED_AV_REGION, 0xFFFFFCFF,
                                  0x00000200) == X_STATUS_SUCCESS);
  CHECK(cvars::video_standard == 2);
  CHECK_FALSE(cvars::use_50Hz_mode);
  CHECK(legacy.ReadSetting<uint32_t>(XCONFIG_SECURED_CATEGORY,
                                     XCONFIG_SECURED_AV_REGION) ==
        X_AV_REGION::NTSCJ);
  CHECK(legacy.ModifySettingUlong(XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE,
                                  0, 17) == X_STATUS_SUCCESS);
  CHECK(cvars::user_language == 17);
  CHECK(legacy.ModifySettingUlong(
            XCONFIG_USER_CATEGORY, XCONFIG_USER_VIDEO_FLAGS, 0xFFFFFFFF,
            X_VIDEO_FLAGS::Widescreen) == X_STATUS_SUCCESS);
  CHECK(cvars::widescreen);
  CHECK(legacy.ModifySettingUlong(XCONFIG_USER_CATEGORY,
                                  XCONFIG_USER_AUDIO_FLAGS, 0x7FFFFFFE,
                                  0x80000002) == X_STATUS_SUCCESS);
  CHECK(cvars::audio_flag == 0x80010002);
  CHECK(legacy.ModifySettingUlong(XCONFIG_USER_CATEGORY,
                                  XCONFIG_USER_MUSIC_VOLUME, 0xFFBFFFFF,
                                  0) == X_STATUS_SUCCESS);
  CHECK(cvars::xmp_default_volume == 50);
  CHECK(legacy.ReadSetting<uint32_t>(XCONFIG_USER_CATEGORY,
                                     XCONFIG_USER_MUSIC_VOLUME) == 0x3F000000);
  CHECK(saves == 6);  // AV region persists standard and50Hz fields separately.
  auto expected = initial;
  auto* console = config::ResolveSectionTable(expected, "Console");
  REQUIRE(console);
  console->insert_or_assign("video_standard", 2);
  console->insert_or_assign("use_50Hz_mode", false);
  console->insert_or_assign("user_language", 17);
  console->insert_or_assign("widescreen", true);
  console->insert_or_assign("audio_flag", uint32_t(0x80010002));
  console->insert_or_assign("xmp_default_volume", 50);
  std::ostringstream exact;
  exact << "# Game-specific config overrides\n"
        << "# Title ID: 58411111\n\n"
        << expected << "\n";
  const auto target_path =
      config::config_folder / "config" / "58411111.config.toml";
  const auto target_before = read(target_path);
  CHECK(target_before == exact.str());
  CHECK(read(other_path) == other_before);
  for (const auto [category, setting, expected_status] :
       {std::tuple{XCONFIG_USER_CATEGORY, uint16_t(XCONFIG_USER_COUNTRY),
                   X_STATUS_INVALID_PARAMETER},
        std::tuple{XCONFIG_USER_CATEGORY, uint16_t(XCONFIG_USER_PC_GAME),
                   X_STATUS_NOT_SUPPORTED},
        std::tuple{XCONFIG_CONSOLE_CATEGORY,
                   uint16_t(XCONFIG_CONSOLE_RETAIL_EX_FLAGS),
                   X_STATUS_NOT_SUPPORTED},
        std::tuple{XCONFIG_USER_CATEGORY, uint16_t(0xFFFF),
                   X_STATUS_NOT_SUPPORTED}}) {
    CHECK(legacy.ModifySettingUlong(category, setting, 0, 0) ==
          expected_status);
  }
  CHECK(saves == 6);
  CHECK(read(target_path) == target_before);
  CHECK(read(other_path) == other_before);
  CHECK_FALSE(std::filesystem::exists(fixture.path / "xconfig.bin"));
  // A reported adapter failure must reach the checked RMW return. Preserve
  // legacy live-before-save behavior rather than claiming atomic rollback.
  XConfig denied({}, {}, [](const char*, XConfig::LegacySettingValue) {
    return std::make_error_code(std::errc::permission_denied);
  });
  CHECK(denied.ModifySettingUlong(XCONFIG_USER_CATEGORY,
                                  XCONFIG_USER_AUDIO_FLAGS, 0,
                                  0x00030003) == X_STATUS_ACCESS_DENIED);
  CHECK(cvars::audio_flag == 0x00030003);
  CHECK(read(target_path) == target_before);
  // Actual existing SaveGameConfig open failure is also propagated, not
  // swallowed as successful persistence. Only disposable fixture paths move.
  const auto saved = target_path.string() + ".saved";
  std::filesystem::rename(target_path, saved);
  REQUIRE(std::filesystem::create_directory(target_path));
  CHECK(legacy.ModifySettingUlong(XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE,
                                  0, 12) == X_STATUS_UNSUCCESSFUL);
  CHECK(read(saved) == target_before);
  CHECK(read(other_path) == other_before);
}
TEST_CASE(
    "Actual XConfig guest adapters use WORD lengths, checked buffers and "
    "console RetailFlags authority",
    "[xconfig][console_store][memory]") {
  StoreFixture fixture;
  auto store = fixture.Open();
  XConfig config(store);
  REQUIRE_FALSE(store->WriteU32(kRetail, 0xA5B6C7D8).error);
  Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t at = 0x90011000, size = 0x2000;
  auto* heap = memory.LookupHeap(at);
  REQUIRE(heap);
  REQUIRE(heap->AllocFixed(at - 0x1000, size + 0x2000, 0x1000,
                           kMemoryAllocationReserve,
                           kMemoryProtectRead | kMemoryProtectWrite));
  REQUIRE(heap->AllocFixed(at, size, 0x1000, kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  auto* data = memory.TranslateVirtual<uint8_t*>(at);
  std::memset(data, 0xA7, size);
  constexpr uint32_t output = at + 3, required = at + 31, input = at + 0x100;
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 3, 0xC, output, 4,
                                   required) == X_STATUS_SUCCESS);
  CHECK(data[2] == 0xA7);
  CHECK(data[3] == 0xA5);
  CHECK(data[4] == 0xB6);
  CHECK(data[5] == 0xC7);
  CHECK(data[6] == 0xD8);
  CHECK(data[7] == 0xA7);
  CHECK(data[30] == 0xA7);
  CHECK(data[31] == 0);
  CHECK(data[32] == 4);
  CHECK(data[33] == 0xA7);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 3, 0xC, 0, 0, required) ==
        X_STATUS_SUCCESS);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 3, 0xC, output, 2,
                                   required) == X_STATUS_BUFFER_TOO_SMALL);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 3, 0xC, 0, 4, required) ==
        X_STATUS_INVALID_PARAMETER_3);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 3, 0xC, at + size - 3, 4,
                                   required) == X_STATUS_ACCESS_VIOLATION);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 3, 0xC, output, 4,
                                   at + size - 1) == X_STATUS_ACCESS_VIOLATION);
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(input), 0x12345678);
  REQUIRE(heap->Protect(at, 0x1000, kMemoryProtectRead));
  CHECK(xboxkrnl::WriteXConfigGuest(config, memory, 3, 0xC, input, 4) ==
        X_STATUS_SUCCESS);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 3, 0xC, output, 4,
                                   required) == X_STATUS_ACCESS_VIOLATION);
  uint32_t value;
  REQUIRE_FALSE(config.ReadRetailFlags(value));
  CHECK(value == 0x12345678);
  CHECK(xboxkrnl::WriteXConfigGuest(config, memory, 3, 0xC, at + size - 3, 4) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(xboxkrnl::WriteXConfigGuest(config, memory, 3, 0xC, input, 3) ==
        X_STATUS_INVALID_PARAMETER);
  CHECK(xboxkrnl::WriteXConfigGuest(config, memory, 3, 0xC, 0, 4) ==
        X_STATUS_INVALID_PARAMETER);
  REQUIRE_FALSE(config.ReadRetailFlags(value));
  CHECK(value == 0x12345678);
  cpu::ExportResolver resolver;
  xboxkrnl::RegisterXConfigExports(&resolver, nullptr);
  xboxkrnl::XboxkrnlModule::RegisterExportTable(&resolver);
  for (uint16_t ordinal : {uint16_t(0x10), uint16_t(0x18), uint16_t(0x2BC)}) {
    auto* entry = resolver.GetExportByOrdinal("xboxkrnl.exe", ordinal);
    REQUIRE(entry);
    CHECK(entry->is_implemented());
    CHECK((entry->tags & cpu::ExportTag::kStub) == 0);
    REQUIRE(entry->function_data.trampoline);
    PPCContext context{};
    context.r[3] = 3;
    context.r[4] = 0xC;
    entry->function_data.trampoline(&context);
    CHECK(uint32_t(context.r[3]) == X_STATUS_INVALID_PARAMETER);
  }
}

TEST_CASE(
    "Fallback preference permit covers real store IO before first native "
    "handover",
    "[xconfig][console_store][notification_preferences]") {
  StoreFixture fixture;
  std::atomic<bool> entered{false}, release{false};
  auto store = fixture.Open([&](const auto& root, const auto& staged,
                                const auto& target, const auto& backup) {
    entered = true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!release && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return root.AtomicReplaceFile(staged, target, backup);
  });
  XConfig config(store);
  NativeNotificationPreferenceCoordinator coordinator(store);
  XConfig::UlongModification result{X_STATUS_UNSUCCESSFUL, {}};
  std::thread writer([&] {
    result = config.WriteFallbackNotificationOptions(coordinator, {0, 1, 0, 1});
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!entered && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool active = entered;
  const bool transitioned =
      coordinator.BeginNativeTransition(std::make_shared<int>(1));
  const bool drained_while_io = coordinator.FallbackDrained();
  NativeNotificationPreferenceCoordinator::Options output{7, 8, 9, 10};
  const auto read_denied =
      config.ReadFallbackNotificationOptions(coordinator, output);
  release = true;
  writer.join();
  REQUIRE(active);
  REQUIRE(transitioned);
  CHECK_FALSE(drained_while_io);
  CHECK(read_denied == X_STATUS_INVALID_DEVICE_REQUEST);
  CHECK((output ==
         NativeNotificationPreferenceCoordinator::Options{7, 8, 9, 10}));
  REQUIRE(result.status == X_STATUS_SUCCESS);
  REQUIRE(result.console);
  CHECK(result.console->published);
  CHECK(result.console->durable);
  REQUIRE(coordinator.FallbackDrained());
  REQUIRE(coordinator.MarkNativeMayRun());
  const auto denied =
      config.WriteFallbackNotificationOptions(coordinator, {1, 1, 1, 1});
  CHECK(denied.status == X_STATUS_INVALID_DEVICE_REQUEST);
  CHECK_FALSE(denied.console);
  uint32_t flags;
  REQUIRE_FALSE(config.ReadRetailFlags(flags));
  CHECK((flags & 0x4230) == 0x30);
  coordinator.BeginStop();
  CHECK(coordinator.SafelyDrained());
  StoreFixture other_fixture;
  auto other = other_fixture.Open();
  NativeNotificationPreferenceCoordinator wrong_console(other);
  CHECK(config.ReadFallbackNotificationOptions(wrong_console, output) ==
        X_STATUS_INVALID_PARAMETER);
}
TEST_CASE(
    "XConfig rich RMW retains actual publication failure and unknown bits",
    "[xconfig][console_store][notification_preferences]") {
  StoreFixture fixture;
  auto store = fixture.Open();
  REQUIRE_FALSE(store->WriteU32(kRetail, 0xA5000000).error);
  const auto before = fixture.Read();
  store.reset();
  SECTION("failure before publication preserves prior bytes") {
    store =
        fixture.Open([](const auto&, const auto&, const auto&, const auto&) {
          return filesystem::HostStorageRoot::ReplaceResult{
              std::make_error_code(std::errc::permission_denied), false, false};
        });
    XConfig config(store);
    const auto result = config.ModifySettingUlongObserved(
        XCONFIG_USER_CATEGORY, 0xC, 0xFFFFBDCF, 0x4020);
    CHECK(XFAILED(result.status));
    REQUIRE(result.console);
    CHECK(result.console->error == std::errc::permission_denied);
    CHECK_FALSE(result.console->published);
    CHECK_FALSE(result.console->durable);
    CHECK(fixture.Read() == before);
  }
  SECTION(
      "real publication followed by reported flush failure stays distinct") {
    store = fixture.Open([](const auto& root, const auto& staged,
                            const auto& target, const auto& backup) {
      auto result = root.AtomicReplaceFile(staged, target, backup);
      if (result.published) {
        result.error = std::make_error_code(std::errc::io_error);
      }
      return result;
    });
    XConfig config(store);
    const auto result = config.ModifySettingUlongObserved(
        XCONFIG_USER_CATEGORY, 0xC, 0xFFFFBDCF, 0x4020);
    CHECK(XFAILED(result.status));
    REQUIRE(result.console);
    CHECK(result.console->error == std::errc::io_error);
    CHECK(result.console->published);
    CHECK_FALSE(result.console->durable);
    uint32_t flags;
    REQUIRE_FALSE(config.ReadRetailFlags(flags));
    CHECK(flags == 0xA5004020);
    CHECK(fixture.Read() != before);
  }
}
struct WatchedConfigMemory {
  static constexpr uint32_t base = 0x900C0000, extent = 0x3000,
                            physical = 0xE1800000, required = base + 0x1000;
  Memory memory;
  void* provider = nullptr;
  bool revoke = false, unlocked = false;
  uint32_t calls = 0;
  WatchedConfigMemory() {
    REQUIRE(memory.Initialize());
    REQUIRE(memory.LookupHeap(base)->AllocFixed(
        base, extent, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(base), 0xA7, extent);
    memory.SetPhysicalAliasSkipHostProtect(true);
    REQUIRE(memory.LookupHeap(physical)->AllocFixed(
        physical, 0x1000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslatePhysical(memory.GetPhysicalAddress(physical)),
                0x51, 0x1000);
  }
  ~WatchedConfigMemory() {
    if (provider) {
      memory.UnregisterPhysicalMemoryReadCallback(provider);
    }
  }
  void Watch() {
    provider = memory.RegisterPhysicalMemoryReadCallback(
        [](void* value, uint32_t start, uint32_t size,
           Memory::PhysicalAccess access) {
          auto& f = *static_cast<WatchedConfigMemory*>(value);
          const auto physical = f.memory.GetPhysicalAddress(f.physical);
          if (start >= physical + 0x1000 ||
              uint64_t(start) + size <= physical) {
            return;
          }
          ++f.calls;
          f.unlocked |= !global_critical_region::is_held_by_current_thread();
          if (access != Memory::PhysicalAccess::kDiscard) {
            xe::store_and_swap<uint32_t>(f.memory.TranslatePhysical(physical),
                                         0x12345678);
          }
          if (f.revoke) {
            f.memory.LookupHeap(f.required)
                ->Protect(f.required, 0x1000, kMemoryProtectRead);
          }
        },
        this);
    memory.EnablePhysicalMemoryAccessCallbacks(
        memory.GetPhysicalAddress(physical), 0x1000, true, true);
  }
};
TEST_CASE(
    "XConfig guest copies stage watched inputs and revalidate all outputs "
    "after providers",
    "[xconfig][console_store][notification_preferences][physical]") {
  StoreFixture fixture;
  auto store = fixture.Open();
  XConfig config(store);
  WatchedConfigMemory f;
  SECTION("readonly watched setter input supplies actual refreshed bytes") {
    f.Watch();
    REQUIRE(f.memory.LookupHeap(f.physical)
                ->Protect(f.physical, 0x1000, kMemoryProtectRead));
    CHECK(xboxkrnl::WriteXConfigGuest(config, f.memory, 3, 0xC, f.physical,
                                      4) == X_STATUS_SUCCESS);
    CHECK(f.calls > 0);
    CHECK(f.unlocked);
    uint32_t flags;
    REQUIRE_FALSE(config.ReadRetailFlags(flags));
    CHECK(flags == 0x12345678);
  }
  SECTION(
      "output provider revokes required WORD before any kernel publication") {
    REQUIRE_FALSE(store->WriteU32(kRetail, 0xABCDEF01).error);
    f.revoke = true;
    f.Watch();
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC, f.physical, 4,
                                     f.required) == X_STATUS_ACCESS_VIOLATION);
    CHECK(f.calls > 0);
    CHECK(f.unlocked);
    CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslatePhysical(
              f.memory.GetPhysicalAddress(f.physical))) == 0x12345678);
    CHECK(xe::load_and_swap<uint16_t>(f.memory.TranslateVirtual(f.required)) ==
          0xA7A7);
  }
  SECTION("only actual WORD footprint is published for BTS and size query") {
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC, f.required, 1,
                                     f.required) == X_STATUS_BUFFER_TOO_SMALL);
    CHECK(xe::load_and_swap<uint16_t>(f.memory.TranslateVirtual(f.required)) ==
          4);
    CHECK(f.memory.TranslateVirtual(f.required)[2] == 0xA7);
    f.Watch();
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC, 0, 0,
                                     f.physical + 8) == X_STATUS_SUCCESS);
    CHECK(f.calls > 0);
    CHECK(xe::load_and_swap<uint16_t>(f.memory.TranslatePhysical(
              f.memory.GetPhysicalAddress(f.physical + 8))) == 4);
  }
  SECTION(
      "setting error publishes only truthful required WORD and capacity is "
      "fully preflighted") {
    XConfig unavailable({}, std::make_error_code(std::errc::permission_denied));
    CHECK(XFAILED(xboxkrnl::ReadXConfigGuest(unavailable, f.memory, 3, 0xC,
                                             f.base, 4, f.required)));
    CHECK(xe::load_and_swap<uint16_t>(f.memory.TranslateVirtual(f.required)) ==
          4);
    CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(f.base)) ==
          0xA7A7A7A7);
    xe::store_and_swap<uint16_t>(f.memory.TranslateVirtual(f.required), 0xA7A7);
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC,
                                     f.base + f.extent - 4, 8,
                                     f.required) == X_STATUS_ACCESS_VIOLATION);
    CHECK(xe::load_and_swap<uint16_t>(f.memory.TranslateVirtual(f.required)) ==
          0xA7A7);
  }
  SECTION("direct and physical alias payload WORD overlap reject unchanged") {
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC, f.base, 4,
                                     f.base + 2) == X_STATUS_INVALID_PARAMETER);
    CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(f.base)) ==
          0xA7A7A7A7);
    const uint32_t output = f.physical + 128;
    const uint32_t alias =
        0xA0000000u + f.memory.GetPhysicalAddress(output) + 2;
    // Backing bytes alone do not commit an alternate virtual heap view.
    REQUIRE(util::GuestIoRangeValid(f.memory, output, 4, true));
    REQUIRE_FALSE(util::GuestIoRangeValid(f.memory, alias, 2, true));
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC, output, 4,
                                     alias) == X_STATUS_ACCESS_VIOLATION);
    CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslatePhysical(
              f.memory.GetPhysicalAddress(output))) == 0x51515151);
    REQUIRE(f.memory.LookupHeap(alias)->AllocFixed(
        alias & ~0xFFFFu, 0x10000, 0x10000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    // Committing another alias may recommit/scribble its common parent.
    std::memset(f.memory.TranslatePhysical(f.memory.GetPhysicalAddress(f.physical)),
                0x51, 0x1000);
    f.Watch();
    REQUIRE(util::GuestIoRangeValid(f.memory, output, 4, true));
    REQUIRE(util::GuestIoRangeValid(f.memory, alias, 2, true));
    REQUIRE(f.memory.GetPhysicalAddress(alias) ==
            f.memory.GetPhysicalAddress(output) + 2);
    REQUIRE(util::GuestIoRangesOverlap(f.memory, output, 4, alias, 2));
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC, output, 4,
                                     alias) == X_STATUS_INVALID_PARAMETER);
    CHECK(f.calls == 0);  // Reject overlap before any provider/publication.
    CHECK(std::all_of(
        f.memory.TranslatePhysical(f.memory.GetPhysicalAddress(f.physical)),
        f.memory.TranslatePhysical(f.memory.GetPhysicalAddress(f.physical)) +
            0x1000,
        [](uint8_t byte) { return byte == 0x51; }));
  }
  SECTION(
      "outer critical region is never released for config IO or providers") {
    f.Watch();
    auto held = global_critical_region::AcquireDirect();
    CHECK(xboxkrnl::ReadXConfigGuest(config, f.memory, 3, 0xC, f.physical, 4,
                                     f.required) ==
          X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(xboxkrnl::WriteXConfigGuest(config, f.memory, 3, 0xC, f.physical,
                                      4) == X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(f.calls == 0);
    CHECK(global_critical_region::is_held_by_current_thread());
  }
  SECTION(
      "known writable shapes reject giant or wrong extents before providers") {
    f.Watch();
    CHECK(xboxkrnl::WriteXConfigGuest(config, f.memory, 3, 0xC, f.physical,
                                      UINT32_MAX) ==
          X_STATUS_INVALID_PARAMETER);
    CHECK(xboxkrnl::WriteXConfigGuest(config, f.memory, 3, 0xC, f.physical,
                                      3) == X_STATUS_INVALID_PARAMETER);
    CHECK(f.calls == 0);
    CHECK(XConfig::WritableSettingSize(XCONFIG_USER_CATEGORY,
                                       XCONFIG_USER_COUNTRY) == 1);
    CHECK(XConfig::WritableSettingSize(XCONFIG_USER_CATEGORY, 0xFFFF) == 0);
  }
}
}  // namespace
}  // namespace xe::kernel::test
