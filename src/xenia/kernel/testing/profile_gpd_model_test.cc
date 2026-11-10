/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/testing/snapshot_fixture.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"
#include <algorithm>
#include <cstdio>
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
  explicit ModelFixture(bool unlocked = false, bool stale_summary = false) {
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
    REQUIRE_FALSE(source->Create("profile", true));
    disk.profile = source->Subroot("profile", false, setup_error);
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
}  // namespace xe::kernel::test
#endif
