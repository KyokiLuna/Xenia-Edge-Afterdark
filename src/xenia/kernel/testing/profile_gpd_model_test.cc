/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/testing/snapshot_fixture.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/profile_picture_codec.h"
#include "xenia/kernel/testing/profile_picture_fixture.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#if XE_PLATFORM_LINUX
namespace xe::kernel::test {
namespace {
void ModelPut32(std::vector<uint8_t>& b, size_t p, uint32_t value) {
  for (unsigned i = 0; i != 4; ++i) {
    b[p + i] = value >> ((3 - i) * 8);
  }
}
void AppendUnknownRecord(std::vector<uint8_t>& wire) {
  const auto view = XdbfTableView::Parse(wire);
  REQUIRE(view);
  const uint32_t capacity = XdbfTableView::U32(wire.data() + 8);
  const uint32_t used = view->entries_used;
  REQUIRE(used < capacity);
  const size_t free_start = 24 + size_t(capacity) * 18;
  const size_t tail = free_start + size_t(view->free_entries_used - 1) * 8;
  const uint32_t eof = XdbfTableView::U32(wire.data() + tail);
  const uint32_t remaining = XdbfTableView::U32(wire.data() + tail + 4);
  REQUIRE(uint64_t(eof) + remaining == UINT32_MAX);
  REQUIRE(remaining >= 4);
  const size_t e = 24 + size_t(used) * 18;
  std::fill_n(wire.data() + e, 18, 0);
  wire[e] = 0xfe;
  wire[e + 1] = 0xdc;
  wire[e + 9] = 42;
  ModelPut32(wire, e + 10, eof);
  ModelPut32(wire, e + 14, 4);
  ModelPut32(wire, 12, used + 1);
  ModelPut32(wire, tail, eof + 4);
  ModelPut32(wire, tail + 4, remaining - 4);
  wire.insert(wire.end(), {0xca, 0xfe, 0, 0x7f});
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(wire));
}
std::vector<uint8_t> SerializeModel(const xam::GpdInfo& gpd) {
  std::vector<uint8_t> wire;
  REQUIRE_FALSE(
      gpd.SerializeChecked(wire, xam::GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(wire));
  return wire;
}
struct ModelFixture {
  filesystem::test::SnapshotFixture disk;
  vfs::VirtualFileSystem fs;
  std::shared_ptr<vfs::HostPathDevice> device;
  std::shared_ptr<xam::ProfileStorageLease> storage;
  std::shared_ptr<filesystem::HostStorageRoot> source;
  std::shared_ptr<const filesystem::ManagedDeviceIdentity> identity;
  std::shared_ptr<const filesystem::ProfileIntentOwner> intent_owner;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery;
  std::shared_ptr<xam::ProfileGpdTitleData> metadata;
  std::vector<uint8_t> dashboard_wire, game_wire;
  static constexpr uint64_t kXuid = 0xe000000000000001ull;
  static constexpr uint32_t kTitle = 0x12345678;
  explicit ModelFixture(bool unlocked = false, bool stale_summary = false, bool canonical = false) {
    std::error_code setup_error;
    REQUIRE_FALSE(disk.parent->Create("content", true));
    source = disk.parent->Subroot("content", false, setup_error);
    REQUIRE(source);
    identity = filesystem::ManagedDeviceIdentity::Open(source, "fixture",
                                                       0xf00d, setup_error);
    REQUIRE(identity);
    REQUIRE_FALSE(setup_error);
    intent_owner = filesystem::ProfileIntentOwner::Open(disk.parent, identity);
    REQUIRE_FALSE(intent_owner->error());
    recovery = std::shared_ptr<const filesystem::SnapshotRecoveryOwner>(
        std::move(disk.owner));
    const auto relative = canonical ? xam::ProfileStorageLease::ProfileRelative(kXuid)
                                    : std::filesystem::path("profile");
    std::filesystem::path prefix;
    for (const auto& part : relative) { prefix /= part; REQUIRE_FALSE(source->Create(prefix, true)); }
    disk.profile = source->Subroot(relative, false, setup_error);
    REQUIRE(disk.profile);

    metadata = std::make_shared<xam::ProfileGpdTitleData>();
    metadata->title_id = kTitle;
    metadata->title_name = u"title";
    metadata->include_in_profile = true;
    metadata->achievement_count = 1;
    metadata->total_gamerscore = 5;
    metadata->achievements.emplace_back(
        0, u"zero", u"earned", u"locked", 7, 5, X_FILETIME{},
        unlocked ? uint32_t(xam::AchievementFlags::kAchieved) : 0);
    metadata->images[7] = {1, 2, 3};
    metadata->title_icon = {4, 5, 6};
    xam::GpdInfoProfile dashboard;
    dashboard.AddNewTitle(kTitle, u"title", stale_summary ? 0 : 1,
                          stale_summary ? 0 : 5);
    if (unlocked) {
      auto* info = dashboard.GetTitleInfo(kTitle);
      info->achievements_unlocked = 1;
      info->gamerscore_earned = 5;
    }
    for (auto [id, value] :
         {std::pair{xam::UserSettingId::XPROFILE_GAMERCARD_TITLES_PLAYED, 1},
          {xam::UserSettingId::XPROFILE_GAMERCARD_CRED, unlocked ? 5 : 0},
          {xam::UserSettingId::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED,
           unlocked ? 1 : 0}}) {
      xam::UserSetting setting(id, value);
      dashboard.UpsertSetting(&setting);
    }
    xam::GpdInfoTitle game(kTitle);
    game.AddAchievement(&metadata->achievements.front());
    game.AddString(xam::kXdbfIdTitle, u"title");
    dashboard_wire = SerializeModel(dashboard);
    game_wire = SerializeModel(game);
    AppendUnknownRecord(dashboard_wire);
    AppendUnknownRecord(game_wire);
    // Seed the real backing BEFORE the ordinary mount populates its cache.
    // Later NativeWrite still uses actual VFS overwrite/write/flush, not a
    // direct host write or an invented cache entry.
    REQUIRE_FALSE(
        disk.profile->WriteNewFile("FFFE07D1.gpd", dashboard_wire).error);
    REQUIRE_FALSE(disk.profile->WriteNewFile("12345678.gpd", game_wire).error);
    auto dev = std::make_unique<vfs::HostPathDevice>(
        "Model:", disk.profile->path(), false, false, "STFS", disk.profile,
        source);
    REQUIRE(dev->Initialize());
    REQUIRE(fs.RegisterDevice(std::move(dev)));
    device = std::dynamic_pointer_cast<vfs::HostPathDevice>(
        fs.AcquireDevice("Model:"));
    std::error_code error;
    storage = xam::ProfileStorageLease::Capture(
        kXuid, "fixture", 0xf00d, 7, device, source, device->storage(), error,
        identity);
    REQUIRE(storage);
    REQUIRE_FALSE(error);
    REQUIRE(fs.ResolvePath("Model:\\FFFE07D1.gpd"));
    REQUIRE(fs.ResolvePath("Model:\\12345678.gpd"));
  }
  std::map<uint32_t, xam::GpdSnapshotLoad> Loads() {
    return {{kDashboardID, xam::GpdSnapshotStore::Load({kXuid, 7, kDashboardID},
                                                       storage->view())},
            {kTitle,
             xam::GpdSnapshotStore::Load({kXuid, 7, kTitle}, storage->view())}};
  }
  std::shared_ptr<xam::ProfileGpdModelAuthority> Authority() {
    std::error_code error;
    auto result =
        xam::ProfileGpdModelAuthority::Create(kXuid, 0, 19, storage, error);
    REQUIRE(result);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(result->InstallLoaded(Loads()));
    return result;
  }
  xam::ProfileGpdMutation Unlock() {
    xam::ProfileGpdMutation command;
    command.kind = xam::ProfileGpdMutationKind::kUnlock;
    command.title_id = kTitle;
    command.record_id = 0;
    command.guest_time = 1234;
    command.foreground_generation = 11;
    command.domain_id = 12;
    command.title_data = metadata;
    return command;
  }
  void NativeWrite(uint32_t title, std::span<const uint8_t> bytes) {
    char path[32];
    std::snprintf(path, sizeof(path), "Model:\\%08X.gpd", title);
    vfs::File *file = nullptr;
    vfs::FileAction action;
    REQUIRE(fs.OpenFile(nullptr, path, vfs::FileDisposition::kOverwriteIf,
                        vfs::FileAccess::kGenericWrite, false, true, &file,
                        &action) == X_STATUS_SUCCESS);
    REQUIRE(file);
    REQUIRE(file->SetLength(bytes.size()) == X_STATUS_SUCCESS);
    size_t count = 0;
    REQUIRE(file->WriteSync(bytes, 0, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == bytes.size());
    REQUIRE(file->Flush() == X_STATUS_SUCCESS);
    file->Destroy();
  }
  void UnchangedOnDisk() {
    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(
        storage->view()->ReadFile("FFFE07D1.gpd", 4096 * 1024, bytes));
    REQUIRE(bytes == dashboard_wire);
    REQUIRE_FALSE(
        storage->view()->ReadFile("12345678.gpd", 4096 * 1024, bytes));
    REQUIRE(bytes == game_wire);
  }
};
xam::ProfileGpdMutation ImageCommand(uint32_t title, uint64_t image,
                                       uint64_t request,
                                       std::vector<uint8_t> bytes = {}) {
  xam::ProfileGpdMutation command;
  command.kind = xam::ProfileGpdMutationKind::kUpsertImage;
  command.title_id = title;
  command.image_id = image;
  command.request_id = request;
  command.image_data = std::move(bytes);
  return command;
}
void RequireOtherRecordsUnchanged(std::span<const uint8_t> before,
                                  std::span<const uint8_t> after,
                                  uint64_t changed_image) {
  const auto old = XdbfTableView::Parse(before), next = XdbfTableView::Parse(after);
  REQUIRE(old); REQUIRE(next);
  for (size_t i = 0; i != old->entries_used; ++i) {
    const auto* e = old->entries.data() + i * 18;
    const uint16_t section = uint16_t(e[0]) << 8 | e[1];
    const uint64_t id = uint64_t(XdbfTableView::U32(e + 2)) << 32 |
                        XdbfTableView::U32(e + 6);
    if (section == uint16_t(xam::GpdSection::kImage) && id == changed_image) {
      continue;
    }
    const auto a = old->Find(section, id), b = next->Find(section, id);
    REQUIRE(a); REQUIRE(b);
    CHECK(std::vector<uint8_t>(a->begin(), a->end()) ==
          std::vector<uint8_t>(b->begin(), b->end()));
  }
}
class SettingOpaqueProbe : public xam::GpdInfo {
 public:
  SettingOpaqueProbe() : GpdInfo(0x12345678) {}
  void AddOpaque(uint32_t id, bool malformed = false) {
    auto* entry = GetEntry(uint16_t(xam::GpdSection::kSetting), id);
    REQUIRE(entry);
    xam::Entry copy = *entry;
    if (malformed) {
      copy.data.resize(3);
    } else {
      auto* header =
          reinterpret_cast<xam::X_XDBF_GPD_SETTING_HEADER*>(copy.data.data());
      header->unknown_1 = 0xabcdef01;
      std::fill_n(header->unknown, 7, char(0x5a));
      std::fill_n(reinterpret_cast<uint8_t*>(&header->base_data) + 4, 4,
                  uint8_t(0x6b));
      copy.data.insert(copy.data.end(), {0xde, 0xad});
    }
    copy.info.size = uint32_t(copy.data.size());
    UpsertEntry(&copy);
  }
};
}  // namespace
TEST_CASE(
    "Owned GPD unlock clones once and retries before IsUnlocked without live "
    "mutation",
    "[profile_gpd_model]") {
  ModelFixture f;
  auto owner = f.Authority();
  std::error_code error;
  bool retry = false;
  auto command = f.Unlock();
  auto pending = owner->Begin(command, retry, error);
  REQUIRE(pending);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(retry);
  auto plan = pending->BuildPlan(error);
  REQUIRE(plan);
  REQUIRE_FALSE(error);
  REQUIRE(plan->two_existing_replacements);
  f.UnchangedOnDisk();
  auto later = command;
  later.guest_time = 9999;
  f.metadata->images[7][0] = 99;
  auto same = owner->Begin(later, retry, error);
  REQUIRE(same == pending);
  REQUIRE(retry);
  REQUIRE(same->BuildPlan(error) == plan);
  xam::GpdInfoTitle game(ModelFixture::kTitle,
                         plan->files.at(ModelFixture::kTitle).intended);
  auto achievement = game.GetAchievementEntry(0);
  REQUIRE(achievement);
  REQUIRE(achievement->is_achievement_unlocked());
  REQUIRE(uint64_t(achievement->unlock_time) == 1234);
  REQUIRE(game.GetImage(7).front() == 1);
  xam::GpdInfoProfile dashboard(plan->files.at(kDashboardID).intended);
  REQUIRE(uint32_t(dashboard.GetTitleInfo(ModelFixture::kTitle)
                       ->achievements_unlocked) == 1);
  REQUIRE(int32_t(dashboard
                      .GetSetting(
                          uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_CRED))
                      ->base_data.s32) == 5);
  for (const auto& [id, file] : plan->files) {
    auto view = XdbfTableView::Parse(file.intended);
    REQUIRE(view);
    auto unknown = view->Find(0xfedc, 42);
    REQUIRE(unknown);
    REQUIRE(std::vector<uint8_t>(unknown->begin(), unknown->end()) ==
            std::vector<uint8_t>{0xca, 0xfe, 0, 0x7f});
  }
}
TEST_CASE(
    "A pending GPD model blocks other writes and reload but survives "
    "retirement",
    "[profile_gpd_model]") {
  ModelFixture f;
  auto owner = f.Authority();
  std::error_code error;
  bool retry = false;
  auto pending = owner->Begin(f.Unlock(), retry, error);
  REQUIRE(pending);
  auto other = f.Unlock();
  other.record_id = 1;
  REQUIRE_FALSE(owner->Begin(other, retry, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(owner->InstallLoaded(f.Loads()) ==
          std::errc::device_or_resource_busy);
  REQUIRE(owner->IsCurrent(pending->origin()));
  owner->Retire();
  REQUIRE_FALSE(owner->IsCurrent(pending->origin()));
  REQUIRE(owner->Pending(error) == pending);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(owner->Begin(f.Unlock(), retry, error));
  REQUIRE(error == std::errc::operation_canceled);
  REQUIRE(pending->BuildPlan(error));
  f.UnchangedOnDisk();
}
TEST_CASE(
    "Every existing GPD mutation family has explicit immutable plan shape",
    "[profile_gpd_model]") {
  using K = xam::ProfileGpdMutationKind;
  using A = xam::ProfileGpdFileAction;
  for (K kind :
       {K::kSetSetting, K::kAdjustSetting, K::kTouchTitle, K::kRemoveTitle,
        K::kRefreshTitleData, K::kFillMissingIcons, K::kRefreshProfileData,
        K::kRefreshSummary, K::kReload, K::kFlush}) {
    ModelFixture f(kind == K::kFillMissingIcons, kind == K::kRefreshSummary);
    auto owner = f.Authority();
    std::error_code error;
    bool retry = false;
    auto command = f.Unlock();
    command.kind = kind;
    command.request_id = 1;
    if (kind == K::kSetSetting || kind == K::kAdjustSetting ||
        kind == K::kReload || kind == K::kFlush) {
      command.title_id = kDashboardID;
    }
    if (kind == K::kSetSetting) {
      command.setting.emplace(xam::UserSettingId::XPROFILE_GAMERCARD_CRED, 3);
    }
    if (kind == K::kAdjustSetting) {
      command.record_id = uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_CRED);
      command.delta = 2;
    }
    if (kind == K::kRefreshTitleData || kind == K::kRefreshProfileData) {
      f.metadata->achievement_count = 2;
      f.metadata->total_gamerscore = 8;
      f.metadata->achievements.emplace_back(1, u"one", u"earned", u"locked", 8,
                                            3, X_FILETIME{}, 0);
    }
    auto pending = owner->Begin(command, retry, error);
    REQUIRE(pending);
    auto plan = pending->BuildPlan(error);
    INFO(int(kind));
    REQUIRE(plan);
    REQUIRE_FALSE(error);
    if (kind == K::kRemoveTitle) {
      REQUIRE(plan->files.at(ModelFixture::kTitle).action == A::kDelete);
      REQUIRE(plan->files.at(ModelFixture::kTitle).intended.empty());
      REQUIRE_FALSE(plan->two_existing_replacements);
    }
    if (kind == K::kReload || kind == K::kFlush) {
      for (const auto& [id, file] : plan->files) {
        REQUIRE(file.action == A::kUnchanged);
      }
    }
    if (kind == K::kSetSetting || kind == K::kAdjustSetting) {
      REQUIRE(plan->files.size() == 1);
      REQUIRE(plan->files.at(kDashboardID).action == A::kReplace);
    }
    if (kind == K::kFillMissingIcons || kind == K::kRefreshTitleData) {
      REQUIRE(plan->files.at(ModelFixture::kTitle).action == A::kReplace);
    }
    if (kind == K::kRefreshSummary || kind == K::kRefreshProfileData) {
      REQUIRE(plan->files.at(kDashboardID).action == A::kReplace);
    }
    f.UnchangedOnDisk();  // Planner never performs WriteGpd/Delete/repair.
  }
}
TEST_CASE("Only explicit add-title may plan a genuinely absent GPD creation",
          "[profile_gpd_model]") {
  ModelFixture f;
  REQUIRE_FALSE(f.storage->view()->Remove("12345678.gpd", false));
  auto owner = f.Authority();
  std::error_code error;
  bool retry = false;
  auto command = f.Unlock();
  command.kind = xam::ProfileGpdMutationKind::kTouchTitle;
  command.request_id = 1;
  auto pending = owner->Begin(command, retry, error);
  REQUIRE(pending);
  auto plan = pending->BuildPlan(error);
  REQUIRE(plan);
  REQUIRE_FALSE(error);
  REQUIRE(plan->files.at(ModelFixture::kTitle).action ==
          xam::ProfileGpdFileAction::kCreate);
  REQUIRE_FALSE(plan->files.at(ModelFixture::kTitle).intended.empty());
  REQUIRE_FALSE(
      std::filesystem::exists(f.storage->view()->path() / "12345678.gpd"));
  auto second = f.Authority();
  auto award = second->Begin(f.Unlock(), retry, error);
  REQUIRE(award);
  REQUIRE_FALSE(award->BuildPlan(error));
  REQUIRE(error == std::errc::no_such_file_or_directory);
}
TEST_CASE(
    "Malformed source data never grants model publication or default "
    "permission",
    "[profile_gpd_model]") {
  ModelFixture f;
  std::error_code error;
  auto owner = xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid, 0, 19,
                                                     f.storage, error);
  REQUIRE(owner);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(f.storage->view()->Remove("FFFE07D1.gpd", false));
  const auto missing = f.Loads();
  REQUIRE(missing.at(kDashboardID).missing());
  REQUIRE(owner->InstallLoaded(missing) == std::errc::no_such_file_or_directory);
  const std::array<uint8_t, 5> malformed{'X', 'D', 'B', 'F', 0};
  REQUIRE_FALSE(
      f.storage->view()->WriteNewFile("FFFE07D1.gpd", malformed).error);
  auto loads = f.Loads();
  REQUIRE(loads.at(kDashboardID).error == std::errc::bad_message);
  REQUIRE(owner->InstallLoaded(loads) == std::errc::bad_message);
  bool retry = false;
  REQUIRE_FALSE(owner->Begin(f.Unlock(), retry, error));
  REQUIRE(error == std::errc::operation_would_block);
  std::vector<uint8_t> preserved;
  REQUIRE_FALSE(f.storage->view()->ReadFile("FFFE07D1.gpd", 4096, preserved));
  REQUIRE(preserved ==
          std::vector<uint8_t>(malformed.begin(), malformed.end()));
}
TEST_CASE(
    "Model version follows validated installs and foreign original title "
    "cannot retry",
    "[profile_gpd_model]") {
  ModelFixture f;
  auto owner = f.Authority();
  REQUIRE_FALSE(owner->InstallLoaded(f.Loads()));
  std::error_code error;
  bool retry = false;
  auto pending = owner->Begin(f.Unlock(), retry, error);
  REQUIRE(pending);
  REQUIRE(pending->origin().stamp.model_version == 2);
  auto changed = f.Unlock();
  changed.foreground_generation++;
  REQUIRE_FALSE(owner->Begin(changed, retry, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(pending->origin().xuid == ModelFixture::kXuid);
  REQUIRE(pending->origin().user_slot == 0);
  REQUIRE(pending->origin().stamp.sign_in_generation == 19);
}
TEST_CASE(
    "Checked setting upsert preserves unknown header scalar and trailing bytes",
    "[profile_gpd_model]") {
  SettingOpaqueProbe model;
  const auto id = xam::UserSettingId::XPROFILE_GAMERCARD_CRED;
  xam::UserSetting initial(id, 3);
  model.UpsertSetting(&initial);
  model.AddOpaque(uint32_t(id));
  xam::UserSetting next(id, 11);
  REQUIRE_FALSE(model.UpsertSettingPreserving(next));
  auto* header = model.GetSetting(uint32_t(id));
  REQUIRE(header);
  REQUIRE(int32_t(header->base_data.s32) == 11);
  REQUIRE(uint32_t(header->unknown_1) == 0xabcdef01);
  for (char value : header->unknown) {
    REQUIRE(uint8_t(value) == 0x5a);
  }
  for (size_t i = 4; i != 8; ++i) {
    REQUIRE(reinterpret_cast<const uint8_t*>(&header->base_data)[i] == 0x6b);
  }
  const auto* entry = std::as_const(model).GetEntry(
      uint16_t(xam::GpdSection::kSetting), uint32_t(id));
  REQUIRE(entry->data[entry->data.size() - 2] == 0xde);
  REQUIRE(entry->data.back() == 0xad);
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(SerializeModel(model)));
}
TEST_CASE("Malformed setting upsert refuses to replace an unreadable payload",
          "[profile_gpd_model]") {
  SettingOpaqueProbe model;
  const auto id = xam::UserSettingId::XPROFILE_GAMERCARD_CRED;
  xam::UserSetting initial(id, 3);
  model.UpsertSetting(&initial);
  model.AddOpaque(uint32_t(id), true);
  const auto before = SerializeModel(model);
  xam::UserSetting next(id, 11);
  REQUIRE(model.UpsertSettingPreserving(next) == std::errc::bad_message);
  REQUIRE(SerializeModel(model) == before);
}
TEST_CASE(
    "Same numeric sign-in stamps do not substitute a different runtime owner",
    "[profile_gpd_model]") {
  ModelFixture f;
  auto original = f.Authority();
  auto other = f.Authority();
  std::error_code error;
  bool retry = false;
  auto pending = original->Begin(f.Unlock(), retry, error);
  REQUIRE(pending);
  REQUIRE(original->IsCurrent(pending->origin()));
  REQUIRE_FALSE(other->IsCurrent(pending->origin()));
  original.reset();
  REQUIRE(pending->origin().owner.expired());
  REQUIRE_FALSE(other->IsCurrent(pending->origin()));
  REQUIRE(pending->BuildPlan(error));
  f.UnchangedOnDisk();
}
TEST_CASE(
    "Retired storage lease rejects model admission without losing pending "
    "recovery",
    "[profile_gpd_model]") {
  ModelFixture f;
  auto owner = f.Authority();
  std::error_code error;
  bool retry = false;
  auto pending = owner->Begin(f.Unlock(), retry, error);
  REQUIRE(pending);
  REQUIRE(owner->IsCurrent(pending->origin()));
  // Actual storage lease's nonwaiting cancellation flag, no fake Observe or
  // synthetic filesystem error. The raw owned view remains available here.
  f.storage->Retire();
  REQUIRE_FALSE(owner->IsCurrent(pending->origin()));
  REQUIRE(owner->InstallLoaded(f.Loads()) == std::errc::operation_canceled);
  REQUIRE_FALSE(owner->Begin(f.Unlock(), retry, error));
  REQUIRE(error == std::errc::operation_canceled);
  REQUIRE(owner->Pending(error) == pending);
  REQUIRE_FALSE(error);
  REQUIRE(pending->BuildPlan(error));
  f.UnchangedOnDisk();
}
TEST_CASE(
    "Summary checked aggregates reject overflow and retain exact UINT32 "
    "boundary",
    "[profile_gpd_model]") {
  for (bool overflow : {true, false}) {
    ModelFixture f;
    xam::GpdInfoTitle actual(ModelFixture::kTitle);
    const uint32_t flags = uint32_t(xam::AchievementFlags::kAchieved);
    xam::AchievementDetails first(0, u"max", u"earned", u"locked", 7,
                                  UINT32_MAX, X_FILETIME{}, flags);
    xam::AchievementDetails second(1, u"next", u"earned", u"locked", 8,
                                   overflow ? 1 : 0, X_FILETIME{}, flags);
    actual.AddAchievement(&first);
    actual.AddAchievement(&second);
    f.game_wire = SerializeModel(actual);
    REQUIRE_FALSE(f.storage->view()->Remove("12345678.gpd", false));
    REQUIRE_FALSE(
        f.storage->view()->WriteNewFile("12345678.gpd", f.game_wire).error);
    auto owner = f.Authority();
    auto command = f.Unlock();
    command.kind = xam::ProfileGpdMutationKind::kRefreshSummary;
    command.request_id = 1;
    std::error_code error;
    bool retry = false;
    auto pending = owner->Begin(command, retry, error);
    REQUIRE(pending);
    auto plan = pending->BuildPlan(error);
    if (overflow) {
      REQUIRE_FALSE(plan);
      REQUIRE(error == std::errc::result_out_of_range);
    } else {
      REQUIRE(plan);
      REQUIRE_FALSE(error);
      xam::GpdInfoProfile dashboard(plan->files.at(kDashboardID).intended);
      const auto* summary = dashboard.GetTitleInfo(ModelFixture::kTitle);
      REQUIRE(summary);
      REQUIRE(uint32_t(summary->gamerscore_total) == UINT32_MAX);
      REQUIRE(uint32_t(summary->gamerscore_earned) == UINT32_MAX);
      REQUIRE(uint32_t(summary->achievements_count) == 2);
      REQUIRE(uint32_t(summary->achievements_unlocked) == 2);
    }
    f.UnchangedOnDisk();
  }
}
TEST_CASE(
    "Actual durable award jointly commits owned models and real VFS cache "
    "exactly once",
    "[profile_gpd_transaction]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  std::error_code error;
  const auto before_read = model->Read(error);
  REQUIRE(before_read);
  const auto origin = model->CaptureOrigin(error);
  REQUIRE(origin);
  auto result = writer.Submit(f.Unlock(), origin->stamp);
  REQUIRE_FALSE(result.error);
  REQUIRE(result.state == xam::ProfileGpdTransactionAttempt::State::kCommitted);
  REQUIRE(result.storage.intent_durable);
  REQUIRE(result.storage.done_durable);
  REQUIRE(result.event);
  REQUIRE(result.event->Current());
  REQUIRE(result.event->TryConsume());
  REQUIRE_FALSE(result.event->TryConsume());
  const auto after_read = model->Read(error);
  REQUIRE(after_read);
  REQUIRE(after_read != before_read);
  REQUIRE_FALSE(before_read->titles.at(ModelFixture::kTitle)
                    .GetAchievementEntry(0)
                    ->is_achievement_unlocked());
  REQUIRE(after_read->titles.at(ModelFixture::kTitle)
              .GetAchievementEntry(0)
              ->is_achievement_unlocked());
  auto* entry = f.fs.ResolvePath("Model:\\12345678.gpd");
  REQUIRE(entry);
  const auto wire = xam::GpdSnapshotStore::Load(
      {ModelFixture::kXuid, 7, ModelFixture::kTitle}, f.storage->view());
  REQUIRE(wire.readable());
  REQUIRE(entry->size() == wire.token->bytes().size());
  auto duplicate = writer.Submit(f.Unlock());
  REQUIRE_FALSE(duplicate.error);
  REQUIRE_FALSE(duplicate.event);
  const auto final_read = model->Read(error);
  REQUIRE(uint32_t(final_read->dashboard.GetTitleInfo(ModelFixture::kTitle)
                       ->achievements_unlocked) == 1);
  const auto view = XdbfTableView::Parse(wire.token->bytes());
  REQUIRE(view);
  REQUIRE(view->Find(0xfedc, 42));
}
TEST_CASE(
    "Durable cache conflict retains the exact pending award and retry commits "
    "once",
    "[profile_gpd_transaction]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  auto first = writer.Submit(f.Unlock());
  REQUIRE_FALSE(first.error);
  REQUIRE(first.event);
  // An actual open VFS handle on the dashboard blocks snapshot admission,
  // before either new replacement can truncate its old bytes.
  vfs::File* raw = nullptr;
  vfs::FileAction action;
  REQUIRE(f.fs.OpenFile(nullptr, "Model:\\FFFE07D1.gpd",
                        vfs::FileDisposition::kOpen,
                        vfs::FileAccess::kGenericRead, false, true, &raw,
                        &action) == X_STATUS_SUCCESS);
  REQUIRE(raw);
  xam::ProfileGpdMutation setting;
  setting.kind = xam::ProfileGpdMutationKind::kSetSetting;
  setting.title_id = kDashboardID;
  setting.request_id = 88;
  setting.setting.emplace(xam::UserSettingId::XPROFILE_GAMERCARD_CRED, 11);
  auto blocked = writer.Submit(setting);
  REQUIRE(blocked.error);
  REQUIRE_FALSE(blocked.event);
  std::error_code error;
  auto pending = model->Pending(error);
  REQUIRE_FALSE(pending); // Ingress rejected before any clone/journal exists.
  raw->Destroy();
  auto retried = writer.Submit(setting);
  REQUIRE_FALSE(retried.error);
  REQUIRE_FALSE(retried.event);
  REQUIRE_FALSE(model->Pending(error));
  REQUIRE(int32_t(model->Read(error)
                      ->dashboard
                      .GetSetting(
                          uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_CRED))
                      ->base_data.s32) == 11);
}
TEST_CASE(
    "Original sign-in model stamp rejects a deferred stale award without "
    "journal or toast",
    "[profile_gpd_transaction]") {
  ModelFixture f;
  auto model = f.Authority();
  std::error_code error;
  const auto origin = model->CaptureOrigin(error);
  REQUIRE(origin);
  auto command = f.Unlock();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  model->Retire();
  auto result = writer.Submit(command, origin->stamp);
  REQUIRE(result.error == std::errc::operation_canceled);
  REQUIRE_FALSE(result.event);
  REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
  REQUIRE_FALSE(error);
  f.UnchangedOnDisk();
}
TEST_CASE(
    "Typed RemoveTitle commits actual deletion and dashboard/cache removal "
    "together",
    "[profile_gpd_transaction]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  auto command = f.Unlock();
  command.kind = xam::ProfileGpdMutationKind::kRemoveTitle;
  command.request_id = 17;
  const auto result = writer.Submit(command);
  REQUIRE_FALSE(result.error);
  REQUIRE_FALSE(result.event);
  REQUIRE_FALSE(
      std::filesystem::exists(f.storage->view()->path() / "12345678.gpd"));
  REQUIRE_FALSE(f.fs.ResolvePath("Model:\\12345678.gpd"));
  std::error_code error;
  const auto read = model->Read(error);
  REQUIRE(read);
  REQUIRE_FALSE(read->titles.contains(ModelFixture::kTitle));
  REQUIRE_FALSE(read->dashboard.GetTitleInfo(ModelFixture::kTitle));
}
TEST_CASE(
    "Raw VFS GPD writes invalidate cached-token reads and install only a "
    "revalidated projection",
    "[profile_gpd_transaction]") {
  ModelFixture f;
  auto model = f.Authority();
  std::error_code error;
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  xam::ProfileGpdMutation flush;
  flush.kind = xam::ProfileGpdMutationKind::kFlush;
  flush.title_id = kDashboardID;
  flush.request_id = 60;
  REQUIRE_FALSE(writer.Submit(flush).error);
  auto old = model->CaptureRead(error);
  REQUIRE(old);
  xam::GpdInfoTitle updated(ModelFixture::kTitle, f.game_wire);
  const std::array<uint8_t, 3> payload{8, 9, 10};
  updated.AddImage(99, payload);
  const auto raw_bytes = SerializeModel(updated);
  vfs::File* file = nullptr;
  vfs::FileAction action;
  REQUIRE(f.fs.OpenFile(nullptr, "Model:\\12345678.gpd",
                        vfs::FileDisposition::kOverwrite,
                        vfs::FileAccess::kGenericWrite, false, true, &file,
                        &action) == X_STATUS_SUCCESS);
  REQUIRE(file);
  REQUIRE(file->SetLength(raw_bytes.size()) == X_STATUS_SUCCESS);
  size_t written = 0;
  REQUIRE(file->WriteSync(raw_bytes, 0, &written) == X_STATUS_SUCCESS);
  REQUIRE(written == raw_bytes.size());
  REQUIRE(file->Flush() == X_STATUS_SUCCESS);
  file->Destroy();
  REQUIRE_FALSE(f.storage->view()->ValidateSnapshotBatch(old->tokens, error));
  REQUIRE(error);
  auto prepared = model->PrepareLoaded(f.Loads(), error);
  REQUIRE(prepared);
  auto cache = f.device->PrepareSnapshotCache(prepared->tokens(), error);
  REQUIRE(cache);
  auto checked =
      f.storage->view()->ValidateSnapshotBatch(prepared->tokens(), error);
  REQUIRE(checked);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(
      std::move(checked));
  auto model_commit = model->TryCommit(std::move(prepared), batch, error);
  REQUIRE(model_commit);
  auto cache_commit =
      f.device->TryCommitSnapshotCache(std::move(cache), batch, error);
  REQUIRE(cache_commit);
  REQUIRE(model_commit->Apply());
  cache_commit->Apply();
  REQUIRE(old->models->titles.at(ModelFixture::kTitle).GetImage(99).empty());
  // Release guards before reacquiring the model's metadata mutex.
  cache_commit.reset();
  model_commit.reset();
  batch.reset();
  const auto current = model->Read(error);
  REQUIRE(current);
  REQUIRE(std::vector<uint8_t>(
              current->titles.at(ModelFixture::kTitle).GetImage(99).begin(),
              current->titles.at(ModelFixture::kTitle).GetImage(99).end()) ==
          std::vector<uint8_t>{8, 9, 10});
}
TEST_CASE(
    "Multiple settings share one durable GPD publication and preserve original "
    "unknown records",
    "[profile_gpd_transaction]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  xam::ProfileGpdMutation command;
  command.kind = xam::ProfileGpdMutationKind::kSetSettings;
  command.title_id = kDashboardID;
  command.request_id = 501;
  command.settings.emplace_back(xam::UserSettingId::XPROFILE_GAMERCARD_CRED,
                                11);
  command.settings.emplace_back(
      xam::UserSettingId::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED, 9);
  auto result = writer.Submit(command);
  REQUIRE_FALSE(result.error);
  REQUIRE_FALSE(result.event);
  REQUIRE(result.storage.files.size() == 1);
  REQUIRE(result.storage.done_durable);
  std::error_code error;
  const auto read = model->Read(error);
  REQUIRE(read);
  REQUIRE(int32_t(read->dashboard
                      .GetSetting(
                          uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_CRED))
                      ->base_data.s32) == 11);
  REQUIRE(
      int32_t(
          read->dashboard
              .GetSetting(uint32_t(
                  xam::UserSettingId::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED))
              ->base_data.s32) == 9);
  const auto current = xam::GpdSnapshotStore::Load(
      {ModelFixture::kXuid, 7, kDashboardID}, f.storage->view());
  REQUIRE(current.readable());
  const auto view = XdbfTableView::Parse(current.token->bytes());
  REQUIRE(view);
  REQUIRE(view->Find(0xfedc, 42));
}
TEST_CASE(
    "A rejected unpublished plan does not permanently occupy the profile "
    "mutation slot",
    "[profile_gpd_transaction]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  xam::ProfileGpdMutation invalid;
  invalid.kind = xam::ProfileGpdMutationKind::kSetSettings;
  invalid.title_id = kDashboardID;
  invalid.request_id = 501;
  const auto failed = writer.Submit(invalid);
  REQUIRE(failed.error == std::errc::invalid_argument);
  REQUIRE_FALSE(failed.event);
  std::error_code error;
  REQUIRE_FALSE(model->Pending(error));
  REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
  REQUIRE_FALSE(error);
  f.UnchangedOnDisk();
  auto valid = invalid;
  valid.request_id = 502;
  valid.settings.emplace_back(xam::UserSettingId::XPROFILE_GAMERCARD_CRED, 7);
  REQUIRE_FALSE(writer.Submit(valid).error);
}

TEST_CASE(
    "Native relock invalidates cached IsUnlocked and original deferred stamp",
    "[profile_gpd_transaction][native_currentness]") {
  ModelFixture f(true);
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  std::error_code error;
  const auto old = model->CaptureOrigin(error);
  REQUIRE(old);
  xam::GpdInfoTitle relocked(ModelFixture::kTitle, f.game_wire);
  auto *achievement = relocked.GetAchievementEntry(0);
  REQUIRE(achievement);
  achievement->flags = 0;
  f.NativeWrite(ModelFixture::kTitle, SerializeModel(relocked));
  xam::GpdInfoProfile native_dashboard(f.dashboard_wire);
  native_dashboard.GetTitleInfo(ModelFixture::kTitle)->achievements_unlocked =
      0;
  native_dashboard.GetTitleInfo(ModelFixture::kTitle)->gamerscore_earned = 0;
  for (auto id : {xam::UserSettingId::XPROFILE_GAMERCARD_CRED,
                  xam::UserSettingId::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED}) {
    xam::UserSetting value(id, 0);
    native_dashboard.UpsertSetting(&value);
  }
  f.NativeWrite(kDashboardID, SerializeModel(native_dashboard));
  const auto refused = writer.Submit(f.Unlock(), old->stamp);
  REQUIRE(refused.error == std::errc::operation_canceled);
  REQUIRE_FALSE(refused.event);
  REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(model->Pending(error));
  const auto fresh = writer.CaptureCurrentOrigin(f.Unlock(), error);
  REQUIRE(fresh);
  REQUIRE(fresh->stamp.model_version > old->stamp.model_version);
  REQUIRE_FALSE(model->Read(error)
                    ->titles.at(ModelFixture::kTitle)
                    .GetAchievementEntry(0)
                    ->is_achievement_unlocked());
  const auto committed = writer.Submit(f.Unlock(), fresh->stamp);
  REQUIRE_FALSE(committed.error);
  REQUIRE(committed.state ==
          xam::ProfileGpdTransactionAttempt::State::kCommitted);
  REQUIRE(committed.event);
  REQUIRE(model->Read(error)
              ->titles.at(ModelFixture::kTitle)
              .GetAchievementEntry(0)
              ->is_achievement_unlocked());
}
TEST_CASE("Native dependency write after Begin creates no intent and permits "
          "fresh retry",
          "[profile_gpd_transaction][native_currentness]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  std::error_code error;
  bool retry = false;
  auto pending = model->Begin(f.Unlock(), retry, error);
  REQUIRE(pending);
  REQUIRE(pending->BuildPlan(error));
  auto transaction =
      model->AcquireTransaction(f.intent_owner, f.recovery, error);
  REQUIRE(transaction);
  xam::GpdInfoProfile native_dashboard(f.dashboard_wire);
  native_dashboard.AddString(0x1234, u"native unrelated setting");
  const auto changed = SerializeModel(native_dashboard);
  f.NativeWrite(kDashboardID, changed);
  const auto refused = transaction->Attempt();
  REQUIRE(refused.error);
  REQUIRE(refused.attempted_journal.empty());
  REQUIRE_FALSE(refused.event);
  REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(model->Pending(error));
  const auto success = writer.Submit(f.Unlock());
  REQUIRE_FALSE(success.error);
  REQUIRE(success.event);
  const auto actual = xam::GpdSnapshotStore::Load(
      {ModelFixture::kXuid, 7, kDashboardID}, f.storage->view());
  REQUIRE(actual.readable());
  REQUIRE(XdbfTableView::Parse(actual.token->bytes())
              ->Find(uint16_t(xam::GpdSection::kString), 0x1234));
}
TEST_CASE("Done title-only intent adopts later native Dashboard bytes at joint "
          "commit",
          "[profile_gpd_transaction][native_currentness]") {
  ModelFixture f;
  auto model = f.Authority();
  std::error_code error;
  xam::ProfileGpdMutation setting;
  setting.kind = xam::ProfileGpdMutationKind::kSetSetting;
  setting.title_id = ModelFixture::kTitle;
  setting.request_id = 771;
  setting.setting.emplace(xam::UserSettingId::XPROFILE_GAMERCARD_CRED, 9);
  bool retry = false;
  auto pending = model->Begin(setting, retry, error);
  REQUIRE(pending);
  auto plan = pending->BuildPlan(error);
  REQUIRE(plan);
  REQUIRE(plan->files.at(kDashboardID).action ==
          xam::ProfileGpdFileAction::kUnchanged);
  std::vector<xam::GpdIntentMutation> changes;
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>> deps;
  for (const auto &[title, load] : *model->Dependencies(pending, error)) {
    deps.push_back(load.token);
  }
  for (const auto &[title, file] : plan->files) {
    if (file.action == xam::ProfileGpdFileAction::kReplace) {
      changes.push_back(
          {xam::GpdIntentAction::kReplace, file.before, file.intended});
    }
  }
  REQUIRE(changes.size() == 1);
  xam::GpdIntentBinding binding{
      "fixture",
      0xf00d,
      ModelFixture::kXuid,
      7,
      f.storage->view()->path().lexically_relative(f.source->path()),
      {f.identity->uuid(), f.identity->content_epoch(),
       f.identity->directory_file_id()}};
  std::filesystem::path journal;
  auto store = xam::GpdDurableIntentStore::Prepare(f.intent_owner, f.recovery,
                                                   binding, std::move(changes),
                                                   error, journal, deps);
  REQUIRE(store);
  auto durable = store->Advance();
  REQUIRE_FALSE(durable.error);
  REQUIRE(durable.done_durable);
  store.reset();
  xam::GpdInfoProfile native_dashboard(f.dashboard_wire);
  native_dashboard.AddString(0x1234, u"post-journal native bytes");
  const auto changed = SerializeModel(native_dashboard);
  f.NativeWrite(kDashboardID, changed);
  auto prepared = model->PrepareCommit(pending, f.Loads(), error);
  REQUIRE(prepared); // V1 rejected the old unchanged Dashboard forever.
  auto cache = f.device->PrepareSnapshotCache(prepared->tokens(), error);
  REQUIRE(cache);
  auto checked =
      f.storage->view()->ValidateSnapshotBatch(prepared->tokens(), error);
  REQUIRE(checked);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(
      std::move(checked));
  auto commit = model->TryCommit(std::move(prepared), batch, error);
  REQUIRE(commit);
  auto cache_commit =
      f.device->TryCommitSnapshotCache(std::move(cache), batch, error);
  REQUIRE(cache_commit);
  REQUIRE(commit->Apply());
  cache_commit->Apply();
  cache_commit.reset();
  commit.reset();
  batch.reset();
  const auto loads = model->CaptureLoads(error);
  REQUIRE(loads);
  REQUIRE(std::equal(changed.begin(), changed.end(),
                     loads->at(kDashboardID).token->bytes().begin(),
                     loads->at(kDashboardID).token->bytes().end()));
  REQUIRE_FALSE(model->Pending(error));
  REQUIRE_FALSE(xam::ProfileGpdTransaction::ReplayBeforeLoad(
      f.storage, f.intent_owner, f.recovery));
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  REQUIRE_FALSE(writer.Reload());
  REQUIRE(model->Read(error)->dashboard.GetString(0x1234) ==
          u"post-journal native bytes");
}
TEST_CASE("No-write unlocked plan rejects a later native relock without model "
          "install",
          "[profile_gpd_model][native_currentness]") {
  ModelFixture f(true);
  auto model = f.Authority();
  std::error_code error;
  bool retry = false;
  auto pending = model->Begin(f.Unlock(), retry, error);
  REQUIRE(pending);
  REQUIRE(pending->BuildPlan(error)->already_unlocked);
  xam::GpdInfoTitle relocked(ModelFixture::kTitle, f.game_wire);
  auto *entry = relocked.GetAchievementEntry(0);
  REQUIRE(entry);
  entry->flags = 0;
  f.NativeWrite(ModelFixture::kTitle, SerializeModel(relocked));
  REQUIRE_FALSE(model->PrepareCommit(pending, f.Loads(), error));
  REQUIRE(error == std::errc::state_not_recoverable);
}

TEST_CASE("Native new title invalidates captured Dashboard and joins fresh "
          "owned reads",
          "[profile_gpd_transaction][native_currentness]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  std::error_code error;
  const auto captured = model->CaptureRead(error);
  REQUIRE(captured);
  constexpr uint32_t added_title = 0x76543210;
  xam::GpdInfoTitle added(added_title);
  added.AddString(xam::kXdbfIdTitle, u"native added title");
  const auto added_wire = SerializeModel(added);
  f.NativeWrite(added_title, added_wire);
  // Unlisted bytes remain intact but are not falsely advertised as already
  // discovered by this Dashboard/prior-dependency read projection.
  REQUIRE(f.storage->view()->ValidateSnapshotBatch(captured->tokens, error));
  REQUIRE_FALSE(error);
  xam::GpdInfoProfile native_dashboard(f.dashboard_wire);
  native_dashboard.AddNewTitle(added_title, u"native added title", 0, 0);
  f.NativeWrite(kDashboardID, SerializeModel(native_dashboard));
  REQUIRE_FALSE(
      f.storage->view()->ValidateSnapshotBatch(captured->tokens, error));
  REQUIRE(error);
  REQUIRE_FALSE(writer.Reload());
  const auto fresh = model->CaptureRead(error);
  REQUIRE(fresh);
  REQUIRE(fresh->models->titles.contains(added_title));
  REQUIRE(fresh->models->titles.at(added_title).GetString(xam::kXdbfIdTitle) ==
          u"native added title");
  REQUIRE(f.storage->view()->ValidateSnapshotBatch(fresh->tokens, error));
  REQUIRE_FALSE(error);
  const auto actual = xam::GpdSnapshotStore::Load(
      {ModelFixture::kXuid, 7, added_title}, f.storage->view());
  REQUIRE(actual.readable());
  REQUIRE(std::equal(added_wire.begin(), added_wire.end(),
                     actual.token->bytes().begin(),
                     actual.token->bytes().end()));
}
TEST_CASE("Roster preparation pauses real GPD writers while preserving read observations",
          "[profile_gpd_model][roster_pause][persistence]") {
  ModelFixture f;
  auto model = f.Authority();
  std::error_code error;
  const auto origin = model->CaptureOrigin(error);
  REQUIRE(origin);
  const auto before = model->Read(error);
  REQUIRE(before);
  auto paused = model->PauseForRoster(error);
  REQUIRE(paused); REQUIRE_FALSE(error);
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  const auto blocked = writer.Submit(f.Unlock());
  CHECK(blocked.error == std::errc::device_or_resource_busy);
  CHECK_FALSE(blocked.event);
  CHECK(model->Read(error) == before);
  CHECK(model->IsCurrent(*origin));
  CHECK(model->InstallLoaded(f.Loads()) == std::errc::device_or_resource_busy);
  CHECK_FALSE(model->PauseForRoster(error));
  CHECK(error == std::errc::device_or_resource_busy);
  f.UnchangedOnDisk();
  paused.reset();
  const auto committed = writer.Submit(f.Unlock());
  REQUIRE_FALSE(committed.error);
  CHECK(committed.state == xam::ProfileGpdTransactionAttempt::State::kCommitted);
  REQUIRE(committed.event); CHECK(committed.event->Current());
}
TEST_CASE("Roster preparation cannot discard an actual pending GPD command",
          "[profile_gpd_model][roster_pause][pending]") {
  ModelFixture f;
  auto model = f.Authority();
  std::error_code error;
  bool retry = false;
  auto pending = model->Begin(f.Unlock(), retry, error);
  REQUIRE(pending);
  CHECK_FALSE(model->PauseForRoster(error));
  CHECK(error == std::errc::device_or_resource_busy);
  CHECK(model->Pending(error) == pending);
  REQUIRE(pending->BuildPlan(error));
  f.UnchangedOnDisk();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  const auto actual = writer.ExecutePending(pending);
  REQUIRE_FALSE(actual.error); REQUIRE(actual.event);
  auto paused = model->PauseForRoster(error);
  REQUIRE(paused); CHECK(actual.event->Current());
  paused.reset(); CHECK(actual.event->Current());
}

TEST_CASE("Legacy key repair preserves native custom malformed and unknown profile metadata",
          "[kernel][profile_gpd_model][picture_key][repair]") {
  ModelFixture f;
  constexpr auto public_key = xam::UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY;
  uint32_t id = uint32_t(public_key);
  std::vector<uint8_t> raw;
  const auto legacy = [](std::string_view text) {
    std::vector<uint8_t> bytes;
    for (uint8_t c : text) { bytes.push_back(c); bytes.push_back(0); }
    bytes.insert(bytes.end(), {0,0}); return bytes;
  };
  SECTION("native BE key") {
    xam::UserSetting native(public_key,std::u16string(u"FFFE07D10002000000010000"));
    raw.assign(native.get_extended_data().begin(),native.get_extended_data().end());
  }
  SECTION("native BE personal key") {
    id=uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE);
    xam::UserSetting native(static_cast<xam::UserSettingId>(id),std::u16string(u"FFFE07D10002000200010002"));
    raw.assign(native.get_extended_data().begin(),native.get_extended_data().end());
  }
  SECTION("custom picture tuple") { raw=legacy("123456780002000000010000"); }
  SECTION("unrecognized setting ID") {
    id=uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_USER_BIO);
    raw=legacy("FFFE07D10002000000010000");
  }
  SECTION("truncated legacy key") { raw=legacy("FFFE07D1"); }
  SECTION("mixed byte order") { raw=legacy("FFFE07D10002000000010000");raw[9]=1; }
  SECTION("unterminated key") { raw=legacy("FFFE07D10002000000010000");raw.back()=1; }
  SECTION("unavailable stock index") { raw=legacy("FFFE07D10002000D0001000D"); }
  SECTION("odd unreadable UTF16 payload") { raw={0x46,0,0x46}; }
  class WireDatabase : public xam::GpdInfoProfile {
   public:
    explicit WireDatabase(std::vector<uint8_t> bytes):GpdInfoProfile(std::move(bytes)){}
    void Insert(uint32_t id,std::span<const uint8_t> payload) {
      xam::X_XDBF_GPD_SETTING_HEADER h{};
      h.setting_id=id;h.setting_type=xam::X_USER_DATA_TYPE::WSTRING;
      h.base_data.unicode.size=uint32_t(payload.size());h.base_data.unicode.ptr=0xDEADBEEF;
      xam::Entry entry(id,uint16_t(xam::GpdSection::kSetting),uint32_t(sizeof(h)+payload.size()));
      std::memcpy(entry.data.data(),&h,sizeof(h));
      std::copy(payload.begin(),payload.end(),entry.data.begin()+sizeof(h));
      UpsertEntry(&entry);
    }
  } db(f.dashboard_wire);
  db.Insert(id,raw);f.dashboard_wire=SerializeModel(db);
  f.NativeWrite(kDashboardID,f.dashboard_wire);
  auto model=f.Authority();xam::ProfileGpdWriter writer{model,f.intent_owner,f.recovery};
  std::error_code error;const auto before=model->CaptureRead(error);REQUIRE(before);
  REQUIRE_FALSE(writer.RepairLegacyPictureKeys({}));
  const auto after=model->CaptureRead(error);REQUIRE(after);
  CHECK(after->origin.stamp.model_version==before->origin.stamp.model_version);
  f.UnchangedOnDisk();
  REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner,error).empty());
  REQUIRE_FALSE(error);
}
TEST_CASE("Eligible legacy stock key repair requires actual installed source before any write",
          "[kernel][profile_gpd_model][picture_key][repair][failure]") {
  ModelFixture f;
  xam::GpdInfoProfile db(f.dashboard_wire);
  const std::u16string text=u"FFFE07D10002000000010000";
  std::vector<uint8_t> little;
  for(char16_t c:text){little.push_back(uint8_t(c));little.push_back(0);}
  little.insert(little.end(),{0,0});
  xam::X_XDBF_GPD_SETTING_HEADER h{};
  const bool personal=GENERATE(false,true);
  h.setting_id=uint32_t(personal ? xam::UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE
                                 : xam::UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY);
  h.setting_type=xam::X_USER_DATA_TYPE::WSTRING;h.base_data.unicode.size=50;
  xam::UserSetting value(&h,little);REQUIRE_FALSE(db.UpsertSettingPreserving(value));
  f.dashboard_wire=SerializeModel(db);f.NativeWrite(kDashboardID,f.dashboard_wire);
  auto model=f.Authority();xam::ProfileGpdWriter writer{model,f.intent_owner,f.recovery};
  std::error_code error;const auto before=model->CaptureRead(error);REQUIRE(before);
  REQUIRE(writer.RepairLegacyPictureKeys({})==std::errc::no_such_device);
  const auto after=model->CaptureRead(error);REQUIRE(after);
  CHECK(after->origin.stamp.model_version==before->origin.stamp.model_version);
  f.UnchangedOnDisk();
  REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner,error).empty());REQUIRE_FALSE(error);
  model->Retire();
  REQUIRE(writer.RepairLegacyPictureKeys({})==std::errc::operation_canceled);
  f.UnchangedOnDisk();
}

TEST_CASE("Typed image writer retains full keys opaque bytes and empty records across restart",
          "[profile_gpd_transaction][image][persistence]") {
  // These keys share their low32 bits; narrowing would replace the other one.
  constexpr uint64_t first_id = 0x123400000007ull;
  constexpr uint64_t second_id = 0xabcd00000007ull;
  for (const uint32_t title : {kDashboardID, ModelFixture::kTitle}) {
    ModelFixture f;
    auto model = f.Authority();
    xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
    std::error_code error;
    auto before = xam::GpdSnapshotStore::Load({ModelFixture::kXuid, 7, title},
                                              f.storage->view());
    REQUIRE(before.readable());
    const auto old_read = model->Read(error);
    REQUIRE(old_read);
    uint64_t request = 1000;
    auto publish = [&](xam::ProfileGpdMutation command) {
      const auto origin = writer.CaptureCurrentOrigin(command, error);
      REQUIRE(origin); REQUIRE_FALSE(error);
      const auto result = writer.Submit(std::move(command), origin->stamp);
      REQUIRE_FALSE(result.error); REQUIRE_FALSE(result.event);
      REQUIRE(result.state == xam::ProfileGpdTransactionAttempt::State::kCommitted);
      return result;
    };
    const auto first = publish(ImageCommand(title, first_id, request++, {0, 0xff, 0x7f}));
    REQUIRE(first.storage.intent_durable); REQUIRE(first.storage.done_durable);
    REQUIRE(first.storage.files.size() == 1);
    // The real intent retains the full original container for recovery.
    std::vector<uint8_t> journal;
    REQUIRE_FALSE(f.intent_owner->root()->ReadFile(first.storage.journal,
                                                    4 * 1024 * 1024, journal));
    REQUIRE(std::search(journal.begin(), journal.end(), before.token->bytes().begin(),
                        before.token->bytes().end()) != journal.end());
    publish(ImageCommand(title, second_id, request++, {0x30, 0x40}));
    publish(ImageCommand(title, first_id, request++, {0xaa, 0xbb, 0xcc, 0xdd}));
    auto loaded = xam::GpdSnapshotStore::Load({ModelFixture::kXuid, 7, title},
                                              f.storage->view());
    REQUIRE(loaded.readable());
    auto table = XdbfTableView::Parse(loaded.token->bytes());
    REQUIRE(table);
    REQUIRE(table->Find(uint16_t(xam::GpdSection::kImage), first_id));
    CHECK(std::vector<uint8_t>(table->Find(2, first_id)->begin(),
                               table->Find(2, first_id)->end()) ==
          std::vector<uint8_t>{0xaa, 0xbb, 0xcc, 0xdd});
    CHECK(std::vector<uint8_t>(table->Find(2, second_id)->begin(),
                               table->Find(2, second_id)->end()) ==
          std::vector<uint8_t>{0x30, 0x40});
    RequireOtherRecordsUnchanged(before.token->bytes(), loaded.token->bytes(), first_id);
    // A nonnull zero-byte input is still a real namespace2 entry.
    publish(ImageCommand(title, first_id, request++));
    loaded = xam::GpdSnapshotStore::Load({ModelFixture::kXuid, 7, title},
                                         f.storage->view());
    table = XdbfTableView::Parse(loaded.token->bytes());
    REQUIRE(table); REQUIRE(table->Find(2, first_id));
    CHECK(table->Find(2, first_id)->empty());
    const auto empty_read = model->Read(error);
    REQUIRE(empty_read);
    auto remove = ImageCommand(title, first_id, request++);
    remove.kind = xam::ProfileGpdMutationKind::kDeleteImage;
    publish(remove);
    const auto deleted = xam::GpdSnapshotStore::Load({ModelFixture::kXuid, 7, title},
                                                    f.storage->view());
    const auto deleted_table = XdbfTableView::Parse(deleted.token->bytes());
    REQUIRE(deleted_table); CHECK_FALSE(deleted_table->Find(2, first_id));
    REQUIRE(deleted_table->Find(2, second_id));
    RequireOtherRecordsUnchanged(loaded.token->bytes(), deleted.token->bytes(), first_id);
    // An already absent desired-state delete is a no-write backend operation.
    const auto rows = xam::GpdDurableIntentStore::Scan(*f.intent_owner, error);
    REQUIRE_FALSE(error);
    remove.request_id = request++;
    publish(remove);
    const auto after_noop = xam::GpdSnapshotStore::Load({ModelFixture::kXuid, 7, title},
                                                       f.storage->view());
    CHECK(after_noop.token->info().file_id == deleted.token->info().file_id);
    CHECK(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).size() == rows.size());
    REQUIRE_FALSE(error);
    // Previously captured models remain immutable; no target image was added there.
    const auto& prior = title == kDashboardID
                            ? static_cast<const xam::GpdInfo&>(old_read->dashboard)
                            : static_cast<const xam::GpdInfo&>(old_read->titles.at(title));
    CHECK(prior.GetImage(first_id).empty());
    CHECK(prior.GetImage(second_id).empty());
    auto* cache_entry = f.fs.ResolvePath(title == kDashboardID
                                          ? "Model:\\FFFE07D1.gpd"
                                          : "Model:\\12345678.gpd");
    REQUIRE(cache_entry); CHECK(cache_entry->size() == after_noop.token->bytes().size());
    model->Retire();
    auto restarted = xam::ProfileGpdModelAuthority::Create(
        ModelFixture::kXuid, 0, 20, f.storage, error);
    REQUIRE(restarted);
    xam::ProfileGpdWriter next{restarted, f.intent_owner, f.recovery};
    REQUIRE_FALSE(next.Reload());
    const auto read = restarted->Read(error);
    REQUIRE(read);
    const auto& image_model = title == kDashboardID
                                 ? static_cast<const xam::GpdInfo&>(read->dashboard)
                                 : static_cast<const xam::GpdInfo&>(read->titles.at(title));
    CHECK(std::vector<uint8_t>(image_model.GetImage(second_id).begin(),
                               image_model.GetImage(second_id).end()) ==
          std::vector<uint8_t>{0x30, 0x40});
    CHECK(image_model.GetImage(first_id).empty());
  }
}
TEST_CASE("Typed image ingress owns payload and refuses stale identity or changed retry",
          "[profile_gpd_model][image][ownership][failure]") {
  ModelFixture f;
  auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  auto command = ImageCommand(ModelFixture::kTitle, 0x100000000ull, 1100, {1, 2, 3});
  std::error_code error;
  bool retry = false;
  auto pending = model->Begin(command, retry, error);
  REQUIRE(pending); REQUIRE_FALSE(error); REQUIRE_FALSE(retry);
  command.image_data[0] = 99;
  REQUIRE_FALSE(model->Begin(command, retry, error));
  CHECK(error == std::errc::device_or_resource_busy);
  command.image_data[0] = 1;
  CHECK(model->Begin(command, retry, error) == pending); CHECK(retry);
  command.image_id ^= uint64_t(1) << 40;
  REQUIRE_FALSE(model->Begin(command, retry, error));
  CHECK(error == std::errc::device_or_resource_busy);
  auto plan = pending->BuildPlan(error);
  REQUIRE(plan);
  const auto table = XdbfTableView::Parse(plan->files.at(ModelFixture::kTitle).intended);
  REQUIRE(table); REQUIRE(table->Find(2, 0x100000000ull));
  CHECK(table->Find(2, 0x100000000ull)->front() == 1);
  bool source_retired = false;
  SECTION("sign-in retirement cancels original request") { model->Retire(); }
  SECTION("managed source removal cancels original request") {
    source_retired = true;
    f.identity->Retire();
  }
  const auto result = writer.ExecutePending(pending);
  CHECK(result.error == std::errc::operation_canceled);
  CHECK_FALSE(result.event);
  CHECK(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
  if (source_retired) { CHECK(error == std::errc::operation_canceled); }
  else { REQUIRE_FALSE(error); }
  CHECK_FALSE(result.storage.intent_durable);
  f.UnchangedOnDisk();
}
TEST_CASE("Image mutation rejects missing targets invalid deletes stale stamps and native write races",
          "[profile_gpd_transaction][image][native_currentness][failure]") {
  ModelFixture f;
  std::error_code error;
  SECTION("missing target cannot implicitly create a title GPD") {
    REQUIRE_FALSE(f.storage->view()->Remove("12345678.gpd", false));
    auto model = f.Authority();
    xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
    const auto result = writer.Submit(ImageCommand(ModelFixture::kTitle, 8, 1200, {8}));
    CHECK(result.error == std::errc::no_such_file_or_directory);
    CHECK_FALSE(std::filesystem::exists(f.storage->view()->path() / "12345678.gpd"));
    CHECK(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
    REQUIRE_FALSE(error);
  }
  SECTION("delete cannot carry ignored write bytes") {
    auto model = f.Authority();
    xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
    auto command = ImageCommand(ModelFixture::kTitle, 8, 1201, {8});
    command.kind = xam::ProfileGpdMutationKind::kDeleteImage;
    const auto result = writer.Submit(command);
    CHECK(result.error == std::errc::invalid_argument);
    CHECK_FALSE(model->Pending(error));
    CHECK(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
    f.UnchangedOnDisk();
  }
  SECTION("a captured old model version cannot target a newer profile projection") {
    auto model = f.Authority();
    xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
    const auto origin = model->CaptureOrigin(error); REQUIRE(origin);
    REQUIRE_FALSE(writer.Submit(ImageCommand(ModelFixture::kTitle, 8, 1202, {8})).error);
    const auto stale = writer.Submit(ImageCommand(ModelFixture::kTitle, 9, 1203, {9}),
                                     origin->stamp);
    CHECK(stale.error == std::errc::operation_canceled);
    const auto load = xam::GpdSnapshotStore::Load(
        {ModelFixture::kXuid, 7, ModelFixture::kTitle}, f.storage->view());
    const auto table = XdbfTableView::Parse(load.token->bytes()); REQUIRE(table);
    CHECK(table->Find(2, 8)); CHECK_FALSE(table->Find(2, 9));
  }
  SECTION("real native write after Begin invalidates before bytes prior to any intent") {
    auto model = f.Authority();
    xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
    bool retry = false;
    auto pending = model->Begin(ImageCommand(ModelFixture::kTitle, 8, 1204, {8}), retry, error);
    REQUIRE(pending);
    xam::GpdInfoTitle changed(ModelFixture::kTitle, f.game_wire);
    changed.AddString(0x1234, u"actual native writer");
    const auto bytes = SerializeModel(changed);
    f.NativeWrite(ModelFixture::kTitle, bytes);
    const auto refused = writer.ExecutePending(pending);
    CHECK(refused.error); CHECK_FALSE(refused.event);
    CHECK(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty());
    REQUIRE_FALSE(error);
    const auto actual = xam::GpdSnapshotStore::Load(
        {ModelFixture::kXuid, 7, ModelFixture::kTitle}, f.storage->view());
    CHECK(std::vector<uint8_t>(actual.token->bytes().begin(), actual.token->bytes().end()) == bytes);
  }
}
TEST_CASE("Interrupted image publication replays actual durable intent before new model load",
          "[profile_gpd_transaction][image][recovery][restart]") {
  ModelFixture f;
  auto model = f.Authority();
  std::error_code error;
  const auto old_read = model->Read(error); REQUIRE(old_read);
  bool retry = false;
  auto pending = model->Begin(ImageCommand(ModelFixture::kTitle, 0xffff00000007ull,
                                            1300, {0x91, 0x92}), retry, error);
  REQUIRE(pending);
  auto plan = pending->BuildPlan(error); REQUIRE(plan);
  const auto& file = plan->files.at(ModelFixture::kTitle);
  REQUIRE(file.action == xam::ProfileGpdFileAction::kReplace);
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>> dependencies;
  for (const auto& [title, load] : *model->Dependencies(pending, error)) {
    dependencies.push_back(load.token);
  }
  xam::GpdIntentBinding binding{
      "fixture", 0xf00d, ModelFixture::kXuid, 7,
      f.storage->view()->path().lexically_relative(f.source->path()),
      {f.identity->uuid(), f.identity->content_epoch(), f.identity->directory_file_id()}};
  std::filesystem::path journal;
  auto store = xam::GpdDurableIntentStore::Prepare(
      f.intent_owner, f.recovery, binding,
      {{xam::GpdIntentAction::kReplace, file.before, file.intended}},
      error, journal, dependencies);
  REQUIRE(store); REQUIRE_FALSE(error);
  // Simulate interruption after durable bytes but BEFORE model/cache commit.
  const auto durable = store->Advance();
  REQUIRE_FALSE(durable.error); REQUIRE(durable.done_durable);
  CHECK(old_read->titles.at(ModelFixture::kTitle).GetImage(0xffff00000007ull).empty());
  store.reset(); model->Retire();
  auto restarted = xam::ProfileGpdModelAuthority::Create(
      ModelFixture::kXuid, 0, 20, f.storage, error);
  REQUIRE(restarted);
  xam::ProfileGpdWriter writer{restarted, f.intent_owner, f.recovery};
  REQUIRE_FALSE(writer.Reload());
  const auto read = restarted->Read(error); REQUIRE(read);
  CHECK(std::vector<uint8_t>(read->titles.at(ModelFixture::kTitle)
                                 .GetImage(0xffff00000007ull).begin(),
                             read->titles.at(ModelFixture::kTitle)
                                 .GetImage(0xffff00000007ull).end()) ==
        std::vector<uint8_t>{0x91, 0x92});
  auto* cache_entry = f.fs.ResolvePath("Model:\\12345678.gpd");
  REQUIRE(cache_entry); CHECK(cache_entry->size() == file.intended.size());
  const auto rows = xam::GpdDurableIntentStore::Scan(*f.intent_owner, error);
  REQUIRE_FALSE(error); REQUIRE(rows.size() == 1); CHECK(rows.front().done_record_valid);
  REQUIRE_FALSE(writer.Reload());
  CHECK(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).size() == 1);
}

namespace {
void CommitPictureLoad(ModelFixture& f,
    const std::shared_ptr<xam::ProfileGpdModelAuthority>& model,
    std::shared_ptr<const xam::ProfileGpdPictureSnapshots> pictures) {
  std::error_code error;
  auto prepared = model->PrepareLoaded(f.Loads(), error, std::move(pictures));
  REQUIRE(prepared); REQUIRE_FALSE(error);
  auto cache = f.device->PrepareSnapshotCache(prepared->tokens(), error);
  REQUIRE(cache); REQUIRE_FALSE(error);
  auto checked = f.storage->view()->ValidateSnapshotBatch(prepared->tokens(), error);
  REQUIRE(checked); REQUIRE_FALSE(error);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
  auto model_commit = model->TryCommit(std::move(prepared), batch, error);
  REQUIRE(model_commit); REQUIRE_FALSE(error);
  auto cache_commit = f.device->TryCommitSnapshotCache(std::move(cache), batch, error);
  REQUIRE(cache_commit); REQUIRE_FALSE(error);
  REQUIRE(model_commit->Apply()); cache_commit->Apply();
}
std::vector<uint8_t> PictureBytes(const xam::ProfileGpdReadCapture& read, const char* name) {
  REQUIRE(read.pictures); REQUIRE(read.pictures->contains(name));
  const auto& token = read.pictures->at(name);
  return {token->bytes().begin(), token->bytes().end()};
}
}  // namespace
TEST_CASE("Real model and VFS publish one picture GPD tuple while older owned pictures remain intact",
          "[profile_gpd_model][picture_tuple][persistence]") {
  ModelFixture f; const std::vector<uint8_t> old_png{0x89,0x50,0,0xEE};
  const std::vector<uint8_t> next_png{0x89,0x50,0,0xFF,4,0xAB,0,9};
  REQUIRE_FALSE(f.storage->view()->WriteNewFile("pp_64.png", old_png).error);
  REQUIRE_FALSE(f.storage->view()->WriteNewFile("__thumbnail.png", old_png).error);
  auto model = f.Authority(); std::error_code error;
  auto pictures = xam::CaptureProfilePictures(f.storage->view(), error);
  REQUIRE(pictures); REQUIRE_FALSE(error); CommitPictureLoad(f, model, pictures);
  const auto older = model->CaptureRead(error); REQUIRE(older); REQUIRE_FALSE(error);
  REQUIRE(older->tokens.size() == f.Loads().size() + 7);
  REQUIRE(PictureBytes(*older, "pp_64.png") == old_png);
  REQUIRE(xam::CaptureProfilePicture(*older, xam::XTileType::kPersonalGamerTile) == old_png);
  REQUIRE_FALSE(f.fs.ResolvePath("Model:\\__thumbnail.png"));
  auto* cached = f.fs.ResolvePath("Model:\\pp_64.png"); REQUIRE(cached);
  REQUIRE(cached->size() == old_png.size());
  const auto changed = filesystem::SnapshotPublisher::Publish(
      *f.recovery, pictures->at("pp_64.png"), next_png);
  REQUIRE_FALSE(changed.error); REQUIRE(changed.durable);
  // Physical replacement is not a model/cache publication or implicit reread.
  REQUIRE(PictureBytes(*model->CaptureRead(error), "pp_64.png") == old_png);
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  REQUIRE_FALSE(writer.Reload());
  const auto current = model->CaptureRead(error); REQUIRE(current); REQUIRE_FALSE(error);
  REQUIRE(current->origin.stamp.model_version == older->origin.stamp.model_version + 1);
  REQUIRE(PictureBytes(*current, "pp_64.png") == next_png);
  REQUIRE(xam::CaptureProfilePicture(*current, xam::XTileType::kPersonalGamerTile) == next_png);
  REQUIRE(xam::CaptureProfilePicture(*older, xam::XTileType::kPersonalGamerTile) == old_png);
  REQUIRE(PictureBytes(*older, "pp_64.png") == old_png);
  REQUIRE(f.fs.ResolvePath("Model:\\pp_64.png") == cached);
  REQUIRE(cached->size() == next_png.size());
  REQUIRE_FALSE(f.fs.ResolvePath("Model:\\__thumbnail.png"));
  f.UnchangedOnDisk();
  // A self-reader must not choose large in versionA and small in versionB.
  // A has only a small picture; B has a different large and small picture.
  // The shared actual reader keeps its ONE capture across either choice.
  {
    ModelFixture fallback;
    const std::vector<uint8_t> small_a{0x11,0,0x22}, large_b{0x33,0,0x44}, small_b{0x55,0,0x66};
    REQUIRE_FALSE(fallback.storage->view()->WriteNewFile("pp_32.png", small_a).error);
    auto self_model = fallback.Authority();
    auto initial = xam::CaptureProfilePictures(fallback.storage->view(), error); REQUIRE(initial);
    CommitPictureLoad(fallback, self_model, initial);
    const auto a = self_model->CaptureRead(error); REQUIRE(a);
    xam::X_XAMACCOUNTINFO account{};
    xam::UserProfile self(ModelFixture::kXuid, &account, self_model);
    REQUIRE(self.GetSelfProfileIcon() == small_a);
    const auto large = filesystem::SnapshotPublisher::Publish(*fallback.recovery, initial->at("pp_64.png"), large_b);
    REQUIRE_FALSE(large.error); REQUIRE(large.durable);
    const auto small = filesystem::SnapshotPublisher::Publish(*fallback.recovery, initial->at("pp_32.png"), small_b);
    REQUIRE_FALSE(small.error); REQUIRE(small.durable);
    xam::ProfileGpdWriter self_writer{self_model, fallback.intent_owner, fallback.recovery};
    REQUIRE_FALSE(self_writer.Reload());
    const auto b = self_model->CaptureRead(error); REQUIRE(b);
    REQUIRE(b->origin.stamp.model_version == a->origin.stamp.model_version + 1);
    REQUIRE(xam::CaptureSelfProfilePicture(*a) == small_a);
    REQUIRE(xam::CaptureSelfProfilePicture(*b) == large_b);
    REQUIRE(self.GetSelfProfileIcon() == large_b);
    REQUIRE(xam::CaptureProfilePicture(*a, xam::XTileType::kGamerTile).empty());
    REQUIRE(xam::CaptureProfilePicture(*b, xam::XTileType::kGamerTile).empty());
  }
}
TEST_CASE("A picture replaced after prepare refuses the actual whole model and cache cut",
          "[profile_gpd_model][picture_tuple][currentness]") {
  ModelFixture f; const std::vector<uint8_t> first{1,0,3}, second{4,0,6,7};
  REQUIRE_FALSE(f.storage->view()->WriteNewFile("tile_32.png", first).error);
  auto model = f.Authority(); std::error_code error;
  auto pictures = xam::CaptureProfilePictures(f.storage->view(), error); REQUIRE(pictures);
  CommitPictureLoad(f, model, pictures);
  const auto older = model->CaptureRead(error); REQUIRE(older);
  auto prepared = model->PrepareLoaded(f.Loads(), error, pictures); REQUIRE(prepared);
  auto cache = f.device->PrepareSnapshotCache(prepared->tokens(), error); REQUIRE(cache);
  const auto replaced = filesystem::SnapshotPublisher::Publish(*f.recovery, pictures->at("tile_32.png"), second);
  REQUIRE_FALSE(replaced.error); REQUIRE(replaced.published);
  REQUIRE_FALSE(f.storage->view()->ValidateSnapshotBatch(prepared->tokens(), error));
  REQUIRE(error == std::errc::state_not_recoverable);
  const auto current = model->CaptureRead(error); REQUIRE(current);
  REQUIRE(current->origin.stamp.model_version == older->origin.stamp.model_version);
  REQUIRE(PictureBytes(*current, "tile_32.png") == first);
  REQUIRE(f.fs.ResolvePath("Model:\\tile_32.png")->size() == first.size());
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery}; REQUIRE_FALSE(writer.Reload());
  REQUIRE(PictureBytes(*model->CaptureRead(error), "tile_32.png") == second);
}
TEST_CASE("Picture tuple rejects missing substituted or foreign members without replacing the model",
          "[profile_gpd_model][picture_tuple][validation]") {
  ModelFixture f, other; auto model = f.Authority(); std::error_code error;
  const auto before = model->CaptureRead(error); REQUIRE(before);
  auto genuine = xam::CaptureProfilePictures(f.storage->view(), error); REQUIRE(genuine);
  auto changed = std::make_shared<xam::ProfileGpdPictureSnapshots>(*genuine);
  SECTION("missing fixed member") { changed->erase("avtr_32.png"); }
  SECTION("extra unrecognized member") { changed->emplace("other.png", genuine->at("tile_32.png")); }
  SECTION("substituted known leaf") { changed->at("tile_64.png") = genuine->at("tile_32.png"); }
  SECTION("real foreign directory token") {
    auto foreign = xam::CaptureProfilePictures(other.storage->view(), error); REQUIRE(foreign);
    changed->at("tile_64.png") = foreign->at("tile_64.png");
  }
  REQUIRE_FALSE(model->PrepareLoaded(f.Loads(), error, changed)); REQUIRE(error);
  const auto after = model->CaptureRead(error); REQUIRE(after);
  REQUIRE(after->origin.stamp.model_version == before->origin.stamp.model_version);
  REQUIRE(after->models == before->models); REQUIRE(after->pictures == before->pictures);
  f.UnchangedOnDisk();
}
TEST_CASE("Actual picture batch keeps case folded package thumbnail hidden from guest cache",
          "[profile_gpd_model][picture_tuple][namespace]") {
  ModelFixture f; const std::vector<uint8_t> thumb{0x89,0x50,0,0xA1};
  REQUIRE_FALSE(f.storage->view()->WriteNewFile("__THUMBNAIL.PNG", thumb).error);
  auto model = f.Authority(); std::error_code error;
  auto pictures = xam::CaptureProfilePictures(f.storage->view(), error); REQUIRE(pictures); REQUIRE_FALSE(error);
  REQUIRE(pictures->at("__thumbnail.png")->relative() == "__THUMBNAIL.PNG");
  CommitPictureLoad(f, model, pictures);
  const auto current = model->CaptureRead(error); REQUIRE(current);
  REQUIRE(PictureBytes(*current, "__thumbnail.png") == thumb);
  REQUIRE_FALSE(f.fs.ResolvePath("Model:\\__thumbnail.png"));
  REQUIRE_FALSE(f.fs.ResolvePath("Model:\\__THUMBNAIL.PNG"));
}

namespace {
xam::ProfilePicturePair ActualPicturePair(bool alternate = false) {
  std::error_code error;
  const auto pair = xam::PrepareHostProfilePicturePair(SyntheticProfilePicture64(alternate), error);
  REQUIRE(pair); REQUIRE_FALSE(error); return *pair;
}
xam::ProfileGpdMutation PictureCommand(uint64_t request,
    std::vector<uint8_t> large, std::vector<uint8_t> small) {
  xam::ProfileGpdMutation command; command.kind = xam::ProfileGpdMutationKind::kSetPictures;
  command.title_id = kDashboardID; command.request_id = request;
  command.picture_changes = {
      {filesystem::ProfileArtifactKind::kPersonal64, false, std::move(large)},
      {filesystem::ProfileArtifactKind::kPersonal32, false, std::move(small)}};
  return command;
}
}  // namespace
TEST_CASE("Actual common writer joins settings and both fixed pictures through04 model cache and restart",
          "[profile_gpd_transaction][picture_tuple][persistence]") {
  ModelFixture f(false, false, true); auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  REQUIRE_FALSE(writer.Reload());
  std::error_code error; const auto older = model->CaptureRead(error); REQUIRE(older); REQUIRE(older->pictures);
  const auto pair = ActualPicturePair(); const auto large = pair.large, small = pair.small;
  auto command = PictureCommand(0x1001, large, small);
  command.settings.emplace_back(xam::UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE,
                                std::u16string(u"FFFE07D10002000000010000"));
  // The actual issued artifact reservation prevents an ordinary VFS writer
  // from bypassing this profile's common publication admission.
  {
    const auto loads = model->CaptureLoads(error); REQUIRE(loads); REQUIRE_FALSE(error);
    const auto& identity = f.storage->managed_identity(); REQUIRE(identity);
    xam::GpdIntentBinding binding{f.storage->source_id(), f.storage->device_id(),
        f.storage->xuid(), f.storage->source_generation(),
        f.storage->view()->path().lexically_relative(f.storage->source()->path()),
        {identity->uuid(), identity->content_epoch(), identity->directory_file_id()}};
    auto admission = xam::GpdDurableIntentStore::AcquireArtifacts(
        f.intent_owner, f.recovery, {}, std::move(binding),
        {{filesystem::ProfileArtifactKind::kGpd, kDashboardID, loads->at(kDashboardID).token}}, false, error);
    REQUIRE(admission); REQUIRE_FALSE(error);
    vfs::File* forbidden = nullptr; vfs::FileAction action{};
    const auto status = f.fs.OpenFile(nullptr, "Model:\\FFFE07D1.gpd", vfs::FileDisposition::kOpen,
        vfs::FileAccess::kGenericRead | vfs::FileAccess::kGenericWrite, false, true, &forbidden, &action);
    struct CloseForbidden { vfs::File*& file; ~CloseForbidden() { if (file) { file->Destroy(); } } } close_forbidden{forbidden};
    REQUIRE(XFAILED(status)); REQUIRE_FALSE(forbidden);
    REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty()); REQUIRE_FALSE(error);
    const auto captured = admission->members()->Capture("FFFE07D1.gpd", xam::GpdSnapshotStore::kMaximumBytes, error, true);
    REQUIRE(captured); REQUIRE_FALSE(error);
    REQUIRE(std::ranges::equal(captured->bytes(), f.dashboard_wire));
  }
  f.UnchangedOnDisk();
  // Conversely an actual already-open VFS write File excludes reservations.
  // No native cached DB quiescence is inferred from this FileHandle test.
  {
    vfs::File* held = nullptr; vfs::FileAction action{};
    const auto status = f.fs.OpenFile(nullptr, "Model:\\FFFE07D1.gpd", vfs::FileDisposition::kOpen,
        vfs::FileAccess::kGenericRead | vfs::FileAccess::kGenericWrite, false, true, &held, &action);
    struct CloseHeld { vfs::File*& file; ~CloseHeld() { if (file) { file->Destroy(); } } } close_held{held};
    REQUIRE(status == X_STATUS_SUCCESS); REQUIRE(held);
    const auto refused = writer.Submit(command);
    REQUIRE(refused.error == std::errc::device_or_resource_busy);
    REQUIRE_FALSE(refused.storage.intent_durable); REQUIRE_FALSE(refused.storage.done_durable);
    REQUIRE(refused.attempted_journal.empty());
    REQUIRE_FALSE(model->Pending(error)); REQUIRE_FALSE(error);
    REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner, error).empty()); REQUIRE_FALSE(error);
    const auto unchanged = model->CaptureRead(error); REQUIRE(unchanged); REQUIRE_FALSE(error);
    REQUIRE(unchanged->origin.stamp.model_version == older->origin.stamp.model_version);
    REQUIRE(unchanged->pictures == older->pictures);
    REQUIRE_FALSE(std::filesystem::exists(f.storage->view()->path() / "pp_64.png"));
    REQUIRE_FALSE(std::filesystem::exists(f.storage->view()->path() / "pp_32.png"));
    std::vector<uint8_t> held_bytes(f.dashboard_wire.size()); size_t count = 0;
    REQUIRE(held->ReadSync(held_bytes, 0, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == held_bytes.size()); REQUIRE(held_bytes == f.dashboard_wire);
    held->Destroy(); held = nullptr;
  }
  f.UnchangedOnDisk();
  // The refused unpublished command is eligible for fresh admission using the
  // exact same owned request after genuine file close; no retry bypass exists.
  const auto committed = writer.Submit(command);
  REQUIRE_FALSE(committed.error); REQUIRE(committed.state == xam::ProfileGpdTransactionAttempt::State::kCommitted);
  REQUIRE(committed.storage.intent_durable); REQUIRE(committed.storage.done_durable);
  REQUIRE(committed.storage.files.size() == 3);
  const auto read = model->CaptureRead(error); REQUIRE(read); REQUIRE_FALSE(error);
  REQUIRE(read->origin.stamp.model_version == older->origin.stamp.model_version + 1);
  REQUIRE(PictureBytes(*read, "pp_64.png") == large); REQUIRE(PictureBytes(*read, "pp_32.png") == small);
  REQUIRE_FALSE(older->pictures->at("pp_64.png")->exists());
  REQUIRE(f.fs.ResolvePath("Model:\\pp_64.png")->size() == large.size());
  auto key = read->models->Setting(kDashboardID,
      uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE), error);
  REQUIRE(key); REQUIRE_FALSE(error);
  auto expected_key = std::u16string(u"FFFE07D10002000000010000"); expected_key.push_back(0);
  REQUIRE(std::get<std::u16string>(key->get_host_data()) == expected_key);
  const auto rows = xam::GpdDurableIntentStore::Scan(*f.intent_owner, error);
  REQUIRE_FALSE(error); REQUIRE(rows.size() == 1); REQUIRE(rows[0].format_version == 4); REQUIRE(rows[0].done_record_valid);
  model->Retire(); REQUIRE_FALSE(model->ReleaseRetiredPublication());
  auto restarted = f.Authority(); xam::ProfileGpdWriter again{restarted, f.intent_owner, f.recovery};
  REQUIRE_FALSE(again.Reload());
  const auto restored = restarted->CaptureRead(error); REQUIRE(restored);
  REQUIRE(PictureBytes(*restored, "pp_64.png") == large); REQUIRE(PictureBytes(*restored, "pp_32.png") == small);
  const auto restored_key = restored->models->Setting(kDashboardID,
      uint32_t(xam::UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE), error);
  REQUIRE(restored_key); REQUIRE(restored_key->Serialize() == key->Serialize());
}
TEST_CASE("Picture command deep ownership and same request conflicts use the actual shared pending admission",
          "[profile_gpd_model][picture_tuple][lifetime]") {
  ModelFixture f(false, false, true); auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery}; REQUIRE_FALSE(writer.Reload());
  const auto pair = ActualPicturePair(); const auto large = pair.large, small = pair.small;
  auto input = PictureCommand(0x1002, large, small);
  bool retry = false; std::error_code error;
  auto pending = model->Begin(input, retry, error); REQUIRE(pending); REQUIRE_FALSE(error); REQUIRE_FALSE(retry);
  input.picture_changes[0].bytes = ActualPicturePair(true).large;
  REQUIRE_FALSE(model->Begin(input, retry, error)); REQUIRE(error == std::errc::device_or_resource_busy);
  const auto plan = pending->BuildPlan(error); REQUIRE(plan); REQUIRE_FALSE(error);
  REQUIRE(plan->pictures[0].intended == large); REQUIRE(plan->pictures[1].intended == small);
  const auto actual = writer.ExecutePending(pending); REQUIRE_FALSE(actual.error);
  REQUIRE(actual.state == xam::ProfileGpdTransactionAttempt::State::kCommitted);
  REQUIRE(PictureBytes(*model->CaptureRead(error), "pp_64.png") == large);
}
TEST_CASE("Picture prepare cannot adopt third bytes as an intended PNG or an unedited fixed member",
          "[profile_gpd_model][picture_tuple][intended]") {
  ModelFixture f(false, false, true); auto model = f.Authority();
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery}; REQUIRE_FALSE(writer.Reload());
  std::error_code error; const auto older = model->CaptureRead(error); REQUIRE(older);
  const auto pair = ActualPicturePair(), other = ActualPicturePair(true);
  const auto large = pair.large, small = pair.small;
  auto command = PictureCommand(0x1003, large, small);
  bool retry = false; auto pending = model->Begin(command, retry, error); REQUIRE(pending);
  const auto a = filesystem::SnapshotPublisher::Publish(*f.recovery, older->pictures->at("pp_64.png"), large);
  REQUIRE_FALSE(a.error); REQUIRE(a.durable);
  SECTION("wrong intended image") {
    const auto b = filesystem::SnapshotPublisher::Publish(*f.recovery, older->pictures->at("pp_32.png"), other.small);
    REQUIRE_FALSE(b.error); REQUIRE(b.durable);
  }
  SECTION("correct images but unedited public picture changed") {
    const auto b = filesystem::SnapshotPublisher::Publish(*f.recovery, older->pictures->at("pp_32.png"), small);
    REQUIRE_FALSE(b.error); REQUIRE(b.durable);
    const auto c = filesystem::SnapshotPublisher::Publish(*f.recovery, older->pictures->at("tile_64.png"), other.large);
    REQUIRE_FALSE(c.error); REQUIRE(c.durable);
  }
  auto pictures = xam::CaptureProfilePictures(f.storage->view(), error); REQUIRE(pictures);
  REQUIRE_FALSE(model->PrepareCommit(pending, f.Loads(), error, pictures));
  REQUIRE(error == std::errc::state_not_recoverable);
  const auto unchanged = model->CaptureRead(error); REQUIRE(unchanged);
  REQUIRE(unchanged->origin.stamp.model_version == older->origin.stamp.model_version);
  REQUIRE(unchanged->pictures == older->pictures);
}

TEST_CASE("Shared picture writer rejects malformed PNG before pending admission or journal mutation",
          "[profile_gpd_model][picture_tuple][validation]") {
  ModelFixture f(false,false,true); auto model=f.Authority();
  xam::ProfileGpdWriter writer{model,f.intent_owner,f.recovery}; REQUIRE_FALSE(writer.Reload());
  std::error_code error; const auto before=model->CaptureRead(error); REQUIRE(before);
  auto command=PictureCommand(0x1004,{0x89,0,1},{0x89,0,2});
  const auto result=writer.Submit(command); REQUIRE(result.error);
  REQUIRE_FALSE(model->Pending(error)); REQUIRE_FALSE(error);
  REQUIRE(xam::GpdDurableIntentStore::Scan(*f.intent_owner,error).empty()); REQUIRE_FALSE(error);
  const auto after=model->CaptureRead(error); REQUIRE(after);
  REQUIRE(after->origin.stamp.model_version==before->origin.stamp.model_version);
  REQUIRE(after->pictures==before->pictures); f.UnchangedOnDisk();
}
namespace {
// Competing real host owner, not inherited GCRC on the loader thread. The
// bounded release prevents a future blocking regression from hanging the test;
// RAII always releases/joins before the actual fixture can be torn down.
struct GpdReloadNamespaceHolder {
  std::promise<void> entered, release;
  std::future<void> ready;
  std::thread worker;
  std::atomic<bool> watchdog_released{false};
  GpdReloadNamespaceHolder() : ready(entered.get_future()) {
    auto wait = release.get_future();
    worker = std::thread([this, wait = std::move(wait)]() mutable {
      auto lock = global_critical_region::AcquireDirect();
      entered.set_value();
      if (wait.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        watchdog_released.store(true, std::memory_order_release);
    });
  }
  bool released=false;
  void Release() {
    if(released)return;
    released=true;release.set_value();worker.join();
  }
  ~GpdReloadNamespaceHolder() { Release(); }
};
}  // namespace
TEST_CASE(
    "Fresh GPD reload preserves model and cache across real namespace "
    "contention and retry",
    "[profile_gpd_model][gpd_load][contention]") {
  ModelFixture f;
  std::error_code error;
  // Authority() deliberately installs a model; this case starts BEFORE that
  // first publication, matching the incoming model in PrepareRoster.
  auto model = xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid, 1, 29,
                                                     f.storage, error);
  REQUIRE(model);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(model->CaptureLoads(error));
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(model->Pending(error));
  REQUIRE_FALSE(error);
  auto* dashboard_cache = f.fs.ResolvePath("Model:\\FFFE07D1.gpd");
  auto* title_cache = f.fs.ResolvePath("Model:\\12345678.gpd");
  REQUIRE(dashboard_cache);
  REQUIRE(title_cache);
  xam::ProfileGpdWriter writer{model, f.intent_owner, f.recovery};
  {
    GpdReloadNamespaceHolder held;
    REQUIRE(held.ready.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    REQUIRE(writer.Reload() == std::errc::device_or_resource_busy);
    REQUIRE_FALSE(held.watchdog_released.load(std::memory_order_acquire));
    REQUIRE_FALSE(model->CaptureLoads(error));
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(model->Pending(error));
    REQUIRE_FALSE(error);
  }
  REQUIRE(f.fs.ResolvePath("Model:\\FFFE07D1.gpd") == dashboard_cache);
  REQUIRE(f.fs.ResolvePath("Model:\\12345678.gpd") == title_cache);
  REQUIRE_FALSE(model->CaptureRead(error));
  REQUIRE(error == std::errc::operation_canceled);
  f.UnchangedOnDisk();
  REQUIRE_FALSE(
      writer.Reload());  // actual new attempt after the holder released
  const auto loaded = model->CaptureRead(error);
  REQUIRE(loaded);
  REQUIRE_FALSE(error);
  REQUIRE(loaded->origin.stamp.model_version == 1);
  REQUIRE(loaded->origin.user_slot == 1);
  REQUIRE(loaded->models->titles.contains(ModelFixture::kTitle));
  REQUIRE(f.fs.ResolvePath("Model:\\FFFE07D1.gpd"));
  REQUIRE(f.fs.ResolvePath("Model:\\12345678.gpd"));
  f.UnchangedOnDisk();
}


TEST_CASE("A retained cold GPD plan is issued by one exact model even on the same lease",
          "[profile_gpd_model][phase_progress][issuer]") {
  ModelFixture f;std::error_code error;
  auto issuer=xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid,0,19,f.storage,error);
  auto foreign=xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid,0,19,f.storage,error);
  REQUIRE(issuer);REQUIRE(foreign);REQUIRE_FALSE(error);
  auto prepared=issuer->PrepareLoaded(f.Loads(),error);REQUIRE(prepared);
  auto older=issuer->PrepareLoaded(f.Loads(),error);REQUIRE(older);
  const auto* exact_older=older.get();
  const auto* exact=prepared.get();const auto tokens=prepared->tokens();
  auto cache=f.device->PrepareSnapshotCache(tokens,error);REQUIRE(cache);
  auto checked=f.storage->view()->ValidateSnapshotBatch(tokens,error);REQUIRE(checked);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
  CHECK_FALSE(foreign->TryCommitPreserving(prepared,batch,error));
  CHECK(error==std::errc::operation_canceled);REQUIRE(prepared.get()==exact);
  CHECK(prepared->tokens()==tokens);
  auto model_commit=issuer->TryCommitPreserving(prepared,batch,error);REQUIRE(model_commit);
  REQUIRE_FALSE(error);CHECK_FALSE(prepared);
  auto cache_commit=f.device->TryCommitSnapshotCache(cache,batch,error);REQUIRE(cache_commit);
  REQUIRE(model_commit->Apply());cache_commit->Apply();
  cache_commit.reset();model_commit.reset();batch.reset();
  const auto read=issuer->CaptureRead(error);REQUIRE(read);REQUIRE_FALSE(error);
  CHECK(read->origin.stamp.model_version==1);CHECK(read->origin.user_slot==0);
  CHECK_FALSE(foreign->CaptureLoads(error));CHECK_FALSE(error);
  checked=f.storage->view()->ValidateSnapshotBatch(older->tokens(),error);REQUIRE(checked);
  batch=std::shared_ptr<const filesystem::HostStorageSnapshotBatch>(std::move(checked));
  CHECK_FALSE(issuer->TryCommitPreserving(older,batch,error));
  CHECK(error==std::errc::operation_canceled);CHECK(older.get()==exact_older);
  batch.reset();f.UnchangedOnDisk();
}
TEST_CASE("A final namespace refusal releases every guard and returns the same unapplied GPD plan",
          "[profile_gpd_model][phase_progress][guard_drop]") {
  ModelFixture f;std::error_code error;
  auto space=filesystem::HostStorageSpace::ForPath(f.storage->view()->path(),20ull<<30,error);
  REQUIRE(space);REQUIRE_FALSE(error);
  auto model=xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid,0,19,f.storage,error);
  REQUIRE(model);
  auto prepared=model->PrepareLoaded(f.Loads(),error);REQUIRE(prepared);
  const auto* exact=prepared.get();const auto tokens=prepared->tokens();
  auto cache=f.device->PrepareSnapshotCache(tokens,error);REQUIRE(cache);
  GpdReloadNamespaceHolder held;
  REQUIRE(held.ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  auto checked=f.storage->view()->ValidateSnapshotBatch(tokens,error);REQUIRE(checked);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
  auto guard=model->TryCommitPreserving(prepared,batch,error);REQUIRE(guard);
  REQUIRE_FALSE(prepared);
  auto why=vfs::HostPathDevice::SnapshotCacheRefusal::kNone;
  CHECK_FALSE(f.device->TryCommitSnapshotCache(cache,batch,error,&why));
  REQUIRE(error==std::errc::device_or_resource_busy);
  CHECK(why==vfs::HostPathDevice::SnapshotCacheRefusal::kNamespaceBusy);
  const auto busy=error;
  prepared=guard->ReleaseUnapplied();REQUIRE(prepared.get()==exact);
  CHECK_FALSE(guard->Apply());CHECK_FALSE(guard->ReleaseUnapplied());
  guard.reset();batch.reset();
  size_t waits=0;
  const std::function<bool()> wait=[&]{
    ++waits;std::error_code observed;
    CHECK_FALSE(model->CaptureLoads(observed));CHECK_FALSE(observed);
    auto actual=space->TryBegin(f.storage->view()->path(),observed);
    CHECK(actual);CHECK_FALSE(observed);actual.reset();
    held.Release();return true;
  };
  xam::ProfilePreparationProgress progress{std::chrono::steady_clock::now()+std::chrono::seconds(2),wait};
  REQUIRE(progress.Retry(busy));CHECK(waits==1);CHECK_FALSE(held.watchdog_released.load());
  CHECK(prepared->tokens()==tokens);
  checked=f.storage->view()->ValidateSnapshotBatch(tokens,error);REQUIRE(checked);
  batch=std::shared_ptr<const filesystem::HostStorageSnapshotBatch>(std::move(checked));
  guard=model->TryCommitPreserving(prepared,batch,error);REQUIRE(guard);REQUIRE_FALSE(prepared);
  auto cache_commit=f.device->TryCommitSnapshotCache(cache,batch,error,&why);REQUIRE(cache_commit);
  CHECK(why==vfs::HostPathDevice::SnapshotCacheRefusal::kNone);
  REQUIRE(guard->Apply());cache_commit->Apply();
  cache_commit.reset();guard.reset();batch.reset();
  const auto read=model->CaptureRead(error);REQUIRE(read);CHECK(read->origin.stamp.model_version==1);
  f.UnchangedOnDisk();
}
TEST_CASE("Retained GPD phase progress refuses changed real file tokens instead of rebasing",
          "[profile_gpd_model][phase_progress][stale]") {
  ModelFixture f;std::error_code error;
  auto model=xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid,0,19,f.storage,error);REQUIRE(model);
  auto prepared=model->PrepareLoaded(f.Loads(),error);REQUIRE(prepared);
  const auto* exact=prepared.get();const auto tokens=prepared->tokens();
  auto checked=f.storage->view()->ValidateSnapshotBatch(tokens,error);REQUIRE(checked);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
  auto guard=model->TryCommitPreserving(prepared,batch,error);REQUIRE(guard);
  prepared=guard->ReleaseUnapplied();guard.reset();batch.reset();
  REQUIRE(prepared.get()==exact);
  auto replaced=f.dashboard_wire;replaced.insert(replaced.end(),{9,8,7,6});
  f.NativeWrite(kDashboardID,replaced);
  CHECK_FALSE(f.storage->view()->ValidateSnapshotBatch(prepared->tokens(),error));
  CHECK(error==std::errc::state_not_recoverable);
  CHECK(prepared.get()==exact);CHECK(prepared->tokens()==tokens);
  CHECK_FALSE(model->CaptureLoads(error));CHECK_FALSE(error);
  xam::ProfileGpdWriter writer{model,f.intent_owner,f.recovery};
  REQUIRE_FALSE(writer.Reload()); // explicit NEW load, not a retained-plan rebase
  const auto read=model->CaptureRead(error);REQUIRE(read);CHECK(read->origin.stamp.model_version==1);
}
TEST_CASE("GPD reload with caller progress publishes once after genuine namespace release",
          "[profile_gpd_model][phase_progress][reload]") {
  ModelFixture f;std::error_code error;
  auto model=xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid,0,19,f.storage,error);REQUIRE(model);
  GpdReloadNamespaceHolder held;
  REQUIRE(held.ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  size_t waits=0;
  const std::function<bool()> wait=[&]{
    ++waits;std::error_code observed;
    CHECK_FALSE(model->CaptureLoads(observed));CHECK_FALSE(observed);
    held.Release();return true;
  };
  xam::ProfilePreparationProgress progress{std::chrono::steady_clock::now()+std::chrono::seconds(2),wait};
  xam::ProfileGpdWriter writer{model,f.intent_owner,f.recovery};
  REQUIRE_FALSE(writer.Reload(&progress));CHECK(waits==1);
  CHECK_FALSE(held.watchdog_released.load());
  const auto read=model->CaptureRead(error);REQUIRE(read);CHECK(read->origin.stamp.model_version==1);
  f.UnchangedOnDisk();
}
TEST_CASE("Retained cache metadata plans cannot publish into a withdrawn namespace generation",
          "[profile_gpd_model][phase_progress][retirement]") {
  ModelFixture f;std::error_code error;
  auto model=xam::ProfileGpdModelAuthority::Create(ModelFixture::kXuid,0,19,f.storage,error);REQUIRE(model);
  auto prepared=model->PrepareLoaded(f.Loads(),error);REQUIRE(prepared);
  auto cache=f.device->PrepareSnapshotCache(prepared->tokens(),error);REQUIRE(cache);
  auto checked=f.storage->view()->ValidateSnapshotBatch(prepared->tokens(),error);REQUIRE(checked);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
  auto guard=model->TryCommitPreserving(prepared,batch,error);REQUIRE(guard);
  f.device->BeginNamespaceRetirement();
  auto why=vfs::HostPathDevice::SnapshotCacheRefusal::kNone;
  CHECK_FALSE(f.device->TryCommitSnapshotCache(cache,batch,error,&why));
  CHECK(error==std::errc::operation_canceled);
  CHECK(why==vfs::HostPathDevice::SnapshotCacheRefusal::kNone);
  prepared=guard->ReleaseUnapplied();guard.reset();batch.reset();REQUIRE(prepared);
  CHECK_FALSE(model->CaptureLoads(error));CHECK_FALSE(error);f.UnchangedOnDisk();
}

}  // namespace xe::kernel::test
#endif
