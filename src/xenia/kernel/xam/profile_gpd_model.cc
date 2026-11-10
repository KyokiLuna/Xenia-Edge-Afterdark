/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/profile_gpd_model.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/kernel/xam/profile_gpd_transaction.h"
namespace xe::kernel::xam {
namespace {
std::shared_ptr<const ProfileGpdReadModels> ReadModels(
    const std::map<uint32_t, GpdSnapshotLoad>&, std::error_code&);
std::error_code Error(std::errc e) { return std::make_error_code(e); }
std::vector<uint8_t> Bytes(std::span<const uint8_t> s) {
  return {s.begin(), s.end()};
}
bool SameTitleData(const std::shared_ptr<const ProfileGpdTitleData>& a,
                   const std::shared_ptr<const ProfileGpdTitleData>& b) {
  if (!a || !b) {
    return !a && !b;
  }
  if (a->title_id != b->title_id || a->title_name != b->title_name ||
      a->include_in_profile != b->include_in_profile ||
      a->achievement_count != b->achievement_count ||
      a->total_gamerscore != b->total_gamerscore ||
      a->title_icon != b->title_icon || a->images != b->images ||
      a->achievements.size() != b->achievements.size()) {
    return false;
  }
  for (size_t i = 0; i != a->achievements.size(); ++i) {
    const auto& x = a->achievements[i];
    const auto& y = b->achievements[i];
    if (x.id != y.id || x.image_id != y.image_id ||
        x.gamerscore != y.gamerscore || x.flags != y.flags ||
        x.label != y.label || x.description != y.description ||
        x.unachieved != y.unachieved ||
        uint32_t(x.unlock_time.high_part) !=
            uint32_t(y.unlock_time.high_part) ||
        uint32_t(x.unlock_time.low_part) != uint32_t(y.unlock_time.low_part)) {
      return false;
    }
  }
  return true;
}
bool SameCommand(const ProfileGpdMutation& a, const ProfileGpdMutation& b) {
  if (a.settings.size() != b.settings.size()) {
    return false;
  }
  for (size_t i = 0; i != a.settings.size(); ++i) {
    if (a.settings[i].get_setting_id() != b.settings[i].get_setting_id() ||
        a.settings[i].Serialize() != b.settings[i].Serialize()) {
      return false;
    }
  }
  if (a.kind != b.kind || a.title_id != b.title_id ||
      a.record_id != b.record_id ||
      a.foreground_generation != b.foreground_generation ||
      a.domain_id != b.domain_id) {
    return false;
  }
  // Repeated same award retains its original immutable time/SPA projection.
  if (a.kind == ProfileGpdMutationKind::kUnlock) {
    return true;
  }
  return a.request_id == b.request_id && a.guest_time == b.guest_time &&
         a.delta == b.delta && bool(a.setting) == bool(b.setting) &&
         (!a.setting || a.setting->Serialize() == b.setting->Serialize()) &&
         SameTitleData(a.title_data, b.title_data);
}
std::error_code ValidateLoaded(const GpdSnapshotLoad& loaded,
                               const ProfileStorageLease& storage,
                               uint32_t title) {
  if (loaded.error) {
    return loaded.error;
  }
  if (!loaded.token || loaded.identity.title_id != title ||
      loaded.identity.xuid != storage.xuid() ||
      loaded.identity.source_generation != storage.source_generation() ||
      loaded.token->root() != storage.view()) {
    return Error(std::errc::invalid_argument);
  }
  if (!loaded.token->ascii_folded_leaf() ||
      loaded.token->relative().has_parent_path()) {
    return Error(std::errc::invalid_argument);
  }
  char expected[13];
  std::snprintf(expected, sizeof(expected), "%08X.gpd", title);
  const auto leaf = loaded.token->relative().string();
  auto lower = [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
  };
  if (leaf.size() != sizeof(expected) - 1 ||
      !std::equal(leaf.begin(), leaf.end(), expected,
                  [&](unsigned char a, unsigned char b) {
                    return lower(a) == lower(b);
                  })) {
    return Error(std::errc::invalid_argument);
  }
  return loaded.token->exists() ? ValidateGpdSnapshot(loaded.token->bytes())
                                : std::error_code{};
}
std::error_code KnownRecordBounds(std::span<const uint8_t> bytes,
                                  bool dashboard) {
  const auto view = XdbfTableView::Parse(bytes);
  if (!view) {
    return Error(std::errc::bad_message);
  }
  for (size_t i = 0; i != view->entries_used; ++i) {
    const auto* entry = view->entries.data() + i * 18;
    const uint16_t section = uint16_t(entry[0]) << 8 | entry[1];
    const uint64_t id = uint64_t(XdbfTableView::U32(entry + 2)) << 32 |
                        XdbfTableView::U32(entry + 6);
    if (id == 0x100000000ull || id == 0x200000000ull) {
      continue;
    }
    const size_t length = XdbfTableView::U32(entry + 14);
    if (dashboard && section == uint16_t(GpdSection::kTitle) &&
        (id > UINT32_MAX || length < sizeof(X_XDBF_GPD_TITLE_PLAYED))) {
      return Error(std::errc::bad_message);
    }
    if (dashboard && section == uint16_t(GpdSection::kTitle)) {
      const auto data = view->Find(section, id);
      if (XdbfTableView::U32(data->data()) != id) {
        return Error(std::errc::bad_message);
      }
    }
    if (!dashboard && section == uint16_t(GpdSection::kAchievement) &&
        (id > UINT32_MAX || length < sizeof(X_XDBF_GPD_ACHIEVEMENT))) {
      return Error(std::errc::bad_message);
    }
    if (!dashboard && section == uint16_t(GpdSection::kAchievement)) {
      const auto data = view->Find(section, id);
      if (XdbfTableView::U32(data->data() + 4) != id) {
        return Error(std::errc::bad_message);
      }
    }
  }
  return {};
}
std::error_code Adjust(GpdInfo& model, uint32_t id, int64_t delta) {
  if (UserData::get_type(id) != X_USER_DATA_TYPE::INT32) {
    return Error(std::errc::invalid_argument);
  }
  std::optional<UserSetting> stored;
  if (UserSetting::ReadFromGpd(model, id, stored) != X_ERROR_SUCCESS) {
    return Error(std::errc::bad_message);
  }
  if (stored && stored->get_type() != X_USER_DATA_TYPE::INT32) {
    return Error(std::errc::bad_message);
  }
  const int64_t prior = stored ? int32_t(stored->get_data()->data.s32) : 0;
  const int64_t next = prior + delta;
  if (next < 0 || next > INT32_MAX) {
    return Error(std::errc::result_out_of_range);
  }
  if (stored) {
    // Counter updates retain every unknown header/tail byte in this record.
    model.GetSetting(id)->base_data.s32 = int32_t(next);
  } else {
    UserSetting value(static_cast<UserSettingId>(id), int32_t(next));
    model.UpsertSetting(&value);
  }
  return {};
}
std::error_code Adjust(GpdInfo& model, UserSettingId id, int64_t delta) {
  return Adjust(model, static_cast<uint32_t>(id), delta);
}
std::error_code SetSetting(GpdInfo& model, const UserSetting& setting) {
  // Shared checked upsert preserves recognized values AND unknown header/tail
  // fields; unlike the legacy zero-header rebuilding setter, it reports errors.
  return model.UpsertSettingPreserving(setting);
}
std::error_code SummarizeChecked(GpdInfoTitle& game,
                                 X_XDBF_GPD_TITLE_PLAYED& summary) {
  uint64_t count = 0, unlocked = 0, total = 0, earned = 0;
  for (const auto id : game.GetAchievementsIds()) {
    const auto* entry = game.GetAchievementEntry(id);
    if (!entry) {
      return Error(std::errc::bad_message);
    }
    ++count;
    total += uint32_t(entry->gamerscore);
    if (entry->is_achievement_unlocked()) {
      ++unlocked;
      earned += uint32_t(entry->gamerscore);
    }
    if (count > UINT32_MAX || unlocked > UINT32_MAX || total > UINT32_MAX ||
        earned > UINT32_MAX) {
      return Error(std::errc::result_out_of_range);
    }
  }
  // Publish to the private model only after every aggregate has been checked.
  summary.achievements_count = uint32_t(count);
  summary.achievements_unlocked = uint32_t(unlocked);
  summary.gamerscore_total = uint32_t(total);
  summary.gamerscore_earned = uint32_t(earned);
  return {};
}
bool PreservesUnknown(std::span<const uint8_t> before,
                      std::span<const uint8_t> intended,
                      std::optional<uint32_t> removed_title = {}) {
  const auto old = XdbfTableView::Parse(before),
             next = XdbfTableView::Parse(intended);
  if (!old || !next) {
    return false;
  }
  for (size_t i = 0; i != old->entries_used; ++i) {
    const auto* e = old->entries.data() + i * 18;
    const uint16_t section = uint16_t(e[0]) << 8 | e[1];
    const uint64_t id =
        uint64_t(XdbfTableView::U32(e + 2)) << 32 | XdbfTableView::U32(e + 6);
    if (removed_title && section == uint16_t(GpdSection::kTitle) &&
        id == *removed_title) {
      continue;
    }
    const auto a = old->Find(section, id), b = next->Find(section, id);
    if (!a || !b) {
      return false;
    }
    if ((section < 1 || section > 5) &&
        (a->size() != b->size() ||
         !std::equal(a->begin(), a->end(), b->begin()))) {
      return false;
    }
  }
  return true;
}
std::shared_ptr<const ProfileGpdModelPlan> MakePlan(
    const ProfileGpdMutation& command,
    const std::map<uint32_t, GpdSnapshotLoad>& loads, std::error_code& error) {
  error.clear();
  const auto dash = loads.find(kDashboardID);
  if (dash == loads.end() || !dash->second.readable()) {
    error = Error(std::errc::no_such_file_or_directory);
    return {};
  }
  error = KnownRecordBounds(dash->second.token->bytes(), true);
  if (error) {
    return {};
  }
  GpdInfoProfile dashboard(Bytes(dash->second.token->bytes()));
  const auto title = loads.find(command.title_id);
  const bool needs_title = command.title_id != kDashboardID &&
                           command.kind != ProfileGpdMutationKind::kReload &&
                           command.kind != ProfileGpdMutationKind::kFlush;
  std::optional<GpdInfoTitle> game;
  if (needs_title) {
    if (title == loads.end()) {
      error = Error(std::errc::no_such_file_or_directory);
      return {};
    }
    if (title->second.missing()) {
      if (command.kind != ProfileGpdMutationKind::kTouchTitle) {
        error = Error(std::errc::no_such_file_or_directory);
        return {};
      }
      game.emplace(
          command.title_id);  // Explicit AddTitle create authority only.
    } else {
      if (!title->second.readable()) {
        error = Error(std::errc::bad_message);
        return {};
      }
      error = KnownRecordBounds(title->second.token->bytes(), false);
      if (error) {
        return {};
      }
      game.emplace(command.title_id, Bytes(title->second.token->bytes()));
    }
  }
  std::vector<uint8_t> old_dashboard, old_game;
  error = dashboard.SerializeChecked(old_dashboard,
                                     GpdSnapshotStore::kMaximumBytes);
  if (!error && game) {
    error = game->SerializeChecked(old_game, GpdSnapshotStore::kMaximumBytes);
  }
  if (error) {
    return {};
  }
  auto plan = std::make_shared<ProfileGpdModelPlan>();
  bool delete_game = false;
  auto title_info = dashboard.GetTitleInfo(command.title_id);
  auto metadata = command.title_data;
  auto require_metadata = [&]() {
    if (!metadata || metadata->title_id != command.title_id) {
      error = Error(std::errc::invalid_argument);
      return false;
    }
    return true;
  };
  auto import_title = [&]() {
    for (const auto& achievement : metadata->achievements) {
      game->AddAchievement(&achievement);
    }
    game->AddImage(kXdbfIdTitle, metadata->title_icon);
    game->AddString(kXdbfIdTitle, metadata->title_name);
  };
  switch (command.kind) {
    case ProfileGpdMutationKind::kUnlock: {
      if (!game || !title_info || !require_metadata()) {
        if (!error) {
          error = Error(std::errc::no_such_file_or_directory);
        }
        break;
      }
      auto achievement = game->GetAchievementEntry(command.record_id);
      if (!achievement) {
        error = Error(std::errc::no_such_file_or_directory);
        break;
      }
      const auto supplied = std::find_if(
          metadata->achievements.begin(), metadata->achievements.end(),
          [&](const auto& item) { return item.id == command.record_id; });
      if (supplied == metadata->achievements.end()) {
        error = Error(std::errc::no_such_file_or_directory);
        break;
      }
      if (supplied->gamerscore != uint32_t(achievement->gamerscore) ||
          supplied->image_id != uint32_t(achievement->image_id)) {
        error = Error(std::errc::state_not_recoverable);
        break;
      }
      if (uint32_t(achievement->flags) &
          (uint32_t(AchievementFlags::kAchieved) |
           uint32_t(AchievementFlags::kAchievedOnline))) {
        plan->already_unlocked = true;
        break;
      }
      const uint32_t score = achievement->gamerscore;
      if (uint64_t(title_info->achievements_unlocked) + 1 > UINT32_MAX ||
          uint64_t(title_info->gamerscore_earned) + score > UINT32_MAX) {
        error = Error(std::errc::result_out_of_range);
        break;
      }
      // AddImage can reallocate the entry vector: never retain this pointer
      // across it. Capture scalar identity, then relookup the private record.
      const uint32_t image_id = achievement->image_id;
      if (auto found = metadata->images.find(image_id);
          found != metadata->images.end()) {
        game->AddImage(image_id, found->second);
      }
      achievement = game->GetAchievementEntry(command.record_id);
      achievement->flags =
          uint32_t(achievement->flags) | uint32_t(AchievementFlags::kAchieved);
      achievement->unlock_time = command.guest_time;
      title_info->achievements_unlocked =
          uint32_t(title_info->achievements_unlocked) + 1;
      title_info->gamerscore_earned =
          uint32_t(title_info->gamerscore_earned) + score;
      error = Adjust(dashboard, UserSettingId::XPROFILE_GAMERCARD_CRED, score);
      if (!error) {
        error =
            Adjust(dashboard,
                   UserSettingId::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED, 1);
      }
      if (!error) {
        error = Adjust(
            *game, UserSettingId::XPROFILE_GAMERCARD_TITLE_CRED_EARNED, score);
      }
      if (!error) {
        error = Adjust(
            *game, UserSettingId::XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED,
            1);
      }
      break;
    }
    case ProfileGpdMutationKind::kSetSettings:
      if (command.settings.empty() || command.settings.size() > 64) {
        error = Error(std::errc::invalid_argument);
        break;
      }
      for (const auto& setting : command.settings) {
        error = SetSetting(game ? static_cast<GpdInfo&>(*game)
                                : static_cast<GpdInfo&>(dashboard),
                           setting);
        if (error) {
          break;
        }
      }
      break;
    case ProfileGpdMutationKind::kSetSetting:
      if (!command.setting) {
        error = Error(std::errc::invalid_argument);
        break;
      }
      error = SetSetting(game ? static_cast<GpdInfo&>(*game)
                              : static_cast<GpdInfo&>(dashboard),
                         *command.setting);
      break;
    case ProfileGpdMutationKind::kAdjustSetting:
      error = Adjust(game ? static_cast<GpdInfo&>(*game)
                          : static_cast<GpdInfo&>(dashboard),
                     command.record_id, command.delta);
      break;
    case ProfileGpdMutationKind::kTouchTitle:
      if (!game || !require_metadata()) {
        if (!error) {
          error = Error(std::errc::invalid_argument);
        }
        break;
      }
      if (!metadata->include_in_profile) {
        break;
      }
      import_title();
      if (!title_info) {
        dashboard.AddNewTitle(command.title_id, metadata->title_name,
                              metadata->achievement_count,
                              metadata->total_gamerscore);
        error = Adjust(dashboard,
                       UserSettingId::XPROFILE_GAMERCARD_TITLES_PLAYED, 1);
      }
      if (!error) {
        // Settings/add-title can relocate entries; relookup after all growth.
        title_info = dashboard.GetTitleInfo(command.title_id);
        title_info->last_played = command.guest_time;
      }
      break;
    case ProfileGpdMutationKind::kRemoveTitle: {
      if (!game || !title_info) {
        error = Error(std::errc::no_such_file_or_directory);
        break;
      }
      const int64_t score = uint32_t(title_info->gamerscore_earned);
      const int64_t count = uint32_t(title_info->achievements_unlocked);
      if (!dashboard.RemoveTitle(command.title_id)) {
        error = Error(std::errc::bad_message);
        break;
      }
      error = Adjust(dashboard, UserSettingId::XPROFILE_GAMERCARD_TITLES_PLAYED,
                     -1);
      if (!error) {
        error =
            Adjust(dashboard, UserSettingId::XPROFILE_GAMERCARD_CRED, -score);
      }
      if (!error) {
        error = Adjust(dashboard,
                       UserSettingId::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED,
                       -count);
      }
      delete_game = !error;
      break;
    }
    case ProfileGpdMutationKind::kRefreshTitleData:
      if (!game || !require_metadata()) {
        if (!error) {
          error = Error(std::errc::invalid_argument);
        }
        break;
      }
      import_title();
      break;
    case ProfileGpdMutationKind::kFillMissingIcons:
      if (!game || !require_metadata()) {
        if (!error) {
          error = Error(std::errc::invalid_argument);
        }
        break;
      }
      for (auto id : game->GetAchievementsIds()) {
        const auto entry = game->GetAchievementEntry(id);
        if (!entry->is_achievement_unlocked()) {
          continue;
        }
        const uint32_t image_id = entry->image_id;
        if (game->GetImage(image_id).empty()) {
          if (auto found = metadata->images.find(image_id);
              found != metadata->images.end()) {
            game->AddImage(image_id, found->second);
          }
        }
      }
      break;
    case ProfileGpdMutationKind::kRefreshProfileData:
      if (!title_info || !require_metadata()) {
        if (!error) {
          error = Error(std::errc::no_such_file_or_directory);
        }
        break;
      }
      if (uint32_t(title_info->achievements_count) <
          metadata->achievement_count) {
        title_info->achievements_count = metadata->achievement_count;
        title_info->gamerscore_total = metadata->total_gamerscore;
      }
      break;
    case ProfileGpdMutationKind::kRefreshSummary:
      if (!game || !title_info) {
        error = Error(std::errc::no_such_file_or_directory);
        break;
      }
      error = SummarizeChecked(*game, *title_info);
      break;
    case ProfileGpdMutationKind::kReload:
    case ProfileGpdMutationKind::kFlush:
      // A reload consumes newly validated real Load results; flush cannot adopt
      // untracked live edits/defaults. There is no implicit WriteGpd fallback.
      break;
  }
  if (error) {
    return {};
  }
  auto add = [&](uint32_t id, const GpdSnapshotLoad& before, GpdInfo& model,
                 const std::vector<uint8_t>& normalized_before, bool remove) {
    ProfileGpdPlannedFile file;
    file.before = before;
    if (remove) {
      file.action = ProfileGpdFileAction::kDelete;
    } else {
      std::vector<uint8_t> after;
      error = model.SerializeChecked(after, GpdSnapshotStore::kMaximumBytes);
      if (!error) {
        error = ValidateGpdSnapshot(after);
      }
      if (error) {
        return;
      }
      if (before.missing()) {
        file.action = ProfileGpdFileAction::kCreate;
        file.intended = std::move(after);
      } else if (after != normalized_before) {
        const auto removed =
            command.kind == ProfileGpdMutationKind::kRemoveTitle &&
                    id == kDashboardID
                ? std::optional<uint32_t>(command.title_id)
                : std::nullopt;
        if (!PreservesUnknown(before.token->bytes(), after, removed)) {
          error = Error(std::errc::permission_denied);
          return;
        }
        file.action = ProfileGpdFileAction::kReplace;
        file.intended = std::move(after);
      }
    }
    plan->files.emplace(id, std::move(file));
  };
  add(kDashboardID, dash->second, dashboard, old_dashboard, false);
  if (!error && game &&
      !(command.kind == ProfileGpdMutationKind::kTouchTitle && metadata &&
        !metadata->include_in_profile)) {
    add(command.title_id, title->second, *game, old_game, delete_game);
  }
  if (error) {
    return {};
  }
  plan->two_existing_replacements =
      plan->files.size() == 2 &&
      std::all_of(plan->files.begin(), plan->files.end(), [](const auto& item) {
        return item.second.action == ProfileGpdFileAction::kReplace &&
               item.second.before.readable();
      });
  return plan;
}
}  // namespace
std::error_code ValidateProfileGpdRecords(std::span<const uint8_t> bytes,
                                          bool dashboard) {
  const auto wire = ValidateGpdSnapshot(bytes);
  return wire ? wire : KnownRecordBounds(bytes, dashboard);
}
std::optional<UserSetting> ProfileGpdReadModels::Setting(
    uint32_t title, uint32_t id, std::error_code& error) const {
  error.clear();
  std::optional<UserSetting> output;
  if (auto game = titles.find(title); game != titles.end()) {
    const auto status = UserSetting::ReadFromGpd(game->second, id, output);
    if (status) {
      error = Error(std::errc::bad_message);
      return {};
    }
    if (output) {
      return output;
    }
  }
  const auto status = UserSetting::ReadFromGpd(dashboard, id, output);
  if (status) {
    error = Error(std::errc::bad_message);
    return {};
  }
  return output ? output : UserSetting::GetDefaultSetting(id);
}
std::shared_ptr<const ProfileGpdTitleData> ProfileGpdTitleData::Capture(
    std::shared_ptr<const SpaInfo> owned, XLanguage language) {
  if (!owned) {
    return {};
  }
  const auto& spa = *owned;
  auto result = std::make_shared<ProfileGpdTitleData>();
  result->title_id = spa.title_id();
  result->include_in_profile = spa.include_in_profile() && !spa.is_system_app();
  result->title_name = xe::to_utf16(spa.title_name());
  result->achievement_count = spa.achievement_count();
  result->total_gamerscore = spa.total_gamerscore();
  constexpr uint64_t maximum = 64ull << 20;
  uint64_t owned_bytes = uint64_t(result->title_name.size()) * sizeof(char16_t);
  if (owned_bytes > maximum) {
    return {};
  }
  const auto title_icon = spa.title_icon();
  if (title_icon.size() > maximum - owned_bytes) {
    return {};
  }
  owned_bytes += title_icon.size();
  result->title_icon = Bytes(title_icon);
  language = spa.GetExistingLanguage(language);
  const auto achievements = spa.GetAchievements();
  if (achievements.size() > 65535 ||
      achievements.size() != spa.achievement_count()) {
    return {};
  }
  std::set<uint32_t> ids;
  uint64_t score = 0;
  for (const auto* entry : achievements) {
    if (!entry || !ids.insert(entry->id).second) {
      return {};
    }
    score += uint32_t(entry->gamerscore);
    if (score > UINT32_MAX) {
      return {};
    }
    AchievementDetails detail(entry, &spa, language);
    const uint64_t strings =
        uint64_t(detail.label.size() + detail.description.size() +
                 detail.unachieved.size()) *
        sizeof(char16_t);
    if (strings > maximum - owned_bytes) {
      return {};
    }
    owned_bytes += strings;
    result->achievements.push_back(std::move(detail));
    const uint32_t image = entry->image_id;
    if (!result->images.contains(image)) {
      const auto bytes = spa.GetIcon(image);
      if (bytes.size() > maximum - owned_bytes) {
        return {};
      }
      owned_bytes += bytes.size();
      result->images.emplace(image, Bytes(bytes));
    }
  }
  result->total_gamerscore = uint32_t(score);
  return result;
}
ProfileGpdPendingMutation::ProfileGpdPendingMutation(
    ProfileGpdMutationOrigin origin, ProfileGpdMutation command,
    std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>> baselines)
    : origin_(std::move(origin)),
      command_(std::move(command)),
      baselines_(std::move(baselines)) {}
bool ProfileGpdPendingMutation::Matches(
    const ProfileGpdMutation &command) const {
  return SameCommand(command_, command);
}
std::shared_ptr<const ProfileGpdModelPlan> ProfileGpdPendingMutation::BuildPlan(
    std::error_code& error) {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (!plan_) {
    plan_ = MakePlan(command_, *baselines_, error);
  }
  return plan_;
}
ProfileGpdModelAuthority::ProfileGpdModelAuthority(
    uint64_t xuid, uint32_t slot, uint64_t generation,
    std::shared_ptr<ProfileStorageLease> storage)
    : xuid_(xuid),
      sign_in_generation_(generation),
      slot_(slot),
      storage_(std::move(storage)) {}
std::shared_ptr<ProfileGpdModelAuthority> ProfileGpdModelAuthority::Create(
    uint64_t xuid, uint32_t slot, uint64_t generation,
    std::shared_ptr<ProfileStorageLease> storage, std::error_code& error) {
  error.clear();
  if (!xuid || slot >= 4 || !generation || !storage ||
      storage->xuid() != xuid) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  return std::shared_ptr<ProfileGpdModelAuthority>(
      new ProfileGpdModelAuthority(xuid, slot, generation, std::move(storage)));
}
uint64_t ProfileGpdModelAuthority::AllocateRequestId(std::error_code& error) {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return 0;
  }
  if (retired_.load() || storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return 0;
  }
  if (!next_request_id_ || next_request_id_ == UINT64_MAX) {
    error = Error(std::errc::value_too_large);
    return 0;
  }
  return next_request_id_++;
}
void ProfileGpdModelAuthority::Retire() {
  // Sign-out may hold GCRC. Never wait for the metadata/planner lane here.
  retired_.store(true, std::memory_order_release);
  // Pending state and exact storage survive for journal recovery.
}
std::unique_ptr<ProfileGpdModelAuthority::PreparedCommit>
ProfileGpdModelAuthority::PrepareLoaded(
    std::map<uint32_t, GpdSnapshotLoad> loaded, std::error_code& error) {
  error.clear();
  const auto dashboard = loaded.find(kDashboardID);
  if (dashboard == loaded.end()) {
    error = Error(std::errc::no_such_file_or_directory);
    return {};
  }
  // Authenticate the required load and preserve malformed/unreadable errors;
  // only an observed missing token grants the distinct missing-file outcome.
  error = ValidateLoaded(dashboard->second, *storage_, kDashboardID);
  if (error) {
    return {};
  }
  if (dashboard->second.missing()) {
    error = Error(std::errc::no_such_file_or_directory);
    return {};
  }
  for (const auto& [id, load] : loaded) {
    error = ValidateLoaded(load, *storage_, id);
    if (error) {
      return {};
    }
  }
  auto read_models = ReadModels(loaded, error);
  if (!read_models) {
    return {};
  }
  auto prepared = std::unique_ptr<PreparedCommit>(new PreparedCommit);
  for (const auto& [id, load] : loaded) {
    prepared->checked.push_back(load.token);
  }
  prepared->baselines =
      std::make_shared<const std::map<uint32_t, GpdSnapshotLoad>>(
          std::move(loaded));
  prepared->models = std::move(read_models);
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (retired_.load() || storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (pending_ || version_ == UINT64_MAX) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  prepared->expected_version = version_;
  return prepared;
}
std::error_code ProfileGpdModelAuthority::InstallLoaded(
    std::map<uint32_t, GpdSnapshotLoad> loaded) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_would_block);
  }
  std::error_code error;
  auto prepared = PrepareLoaded(std::move(loaded), error);
  if (!prepared) {
    return error;
  }
  auto checked =
      storage_->view()->ValidateSnapshotBatch(prepared->tokens(), error);
  if (!checked) {
    return error;
  }
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(
      std::move(checked));
  auto commit = TryCommit(std::move(prepared), batch, error);
  if (!commit) {
    return error;
  }
  return commit->Apply() ? std::error_code{}
                         : Error(std::errc::operation_canceled);
}

std::shared_ptr<ProfileGpdPendingMutation> ProfileGpdModelAuthority::Begin(
    ProfileGpdMutation command, bool& retry, std::error_code& error,
    std::optional<ProfileGpdModelStamp> required_stamp) {
  error.clear();
  retry = false;
  if (storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (uint32_t(command.kind) > uint32_t(ProfileGpdMutationKind::kFlush) ||
      !command.title_id ||
      (command.kind != ProfileGpdMutationKind::kUnlock &&
       !command.request_id) ||
      (command.kind == ProfileGpdMutationKind::kUnlock &&
       !command.foreground_generation)) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  // Deep copy payload ownership before metadata locking. No borrowed guest,
  // profile, SPA or mutable caller-owned projection survives this boundary.
  if (command.title_data) {
    command.title_data =
        std::make_shared<const ProfileGpdTitleData>(*command.title_data);
  }
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (retired_.load(std::memory_order_acquire) || storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (required_stamp &&
      (required_stamp->sign_in_generation != sign_in_generation_ ||
       required_stamp->model_version != version_)) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (pending_) {
    if (SameCommand(pending_->command(), command)) {
      retry = true;
      return pending_;
    }
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (!baselines_ || !version_) {
    error = Error(std::errc::operation_would_block);
    return {};
  }
  ProfileGpdMutationOrigin origin{xuid_,
                                  slot_,
                                  {sign_in_generation_, version_},
                                  storage_,
                                  weak_from_this()};
  pending_ =
      std::shared_ptr<ProfileGpdPendingMutation>(new ProfileGpdPendingMutation(
          std::move(origin), std::move(command), baselines_));
  return pending_;
}
std::optional<ProfileGpdMutationOrigin> ProfileGpdModelAuthority::CaptureOrigin(
    std::error_code& error) const {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (retired_.load() || storage_->retiring() || !version_) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  return ProfileGpdMutationOrigin{xuid_,
                                  slot_,
                                  {sign_in_generation_, version_},
                                  storage_,
                                  weak_from_this()};
}
std::shared_ptr<const ProfileGpdPendingMutation>
ProfileGpdModelAuthority::Pending(std::error_code& error) const {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  return pending_;
}
bool ProfileGpdModelAuthority::IsCurrent(
    const ProfileGpdMutationOrigin& origin) const {
  std::unique_lock lock(mutex_, std::try_to_lock);
  return lock.owns_lock() && !retired_.load(std::memory_order_acquire) &&
         origin.xuid == xuid_ && !storage_->retiring() &&
         origin.owner.lock().get() == this && origin.user_slot == slot_ &&
         origin.storage == storage_ &&
         origin.stamp.sign_in_generation == sign_in_generation_ &&
         origin.stamp.model_version == version_;
}
namespace {
std::shared_ptr<const ProfileGpdReadModels> ReadModels(
    const std::map<uint32_t, GpdSnapshotLoad>& loads, std::error_code& error) {
  error.clear();
  const auto dashboard = loads.find(kDashboardID);
  if (dashboard == loads.end() || !dashboard->second.readable()) {
    error = Error(std::errc::no_such_file_or_directory);
    return {};
  }
  error = KnownRecordBounds(dashboard->second.token->bytes(), true);
  if (error) {
    return {};
  }
  auto result =
      std::make_shared<ProfileGpdReadModels>(dashboard->second.token->bytes());
  uint64_t bytes = dashboard->second.token->bytes().size();
  if (loads.size() > 4096) {
    error = Error(std::errc::file_too_large);
    return {};
  }
  for (const auto& [title, load] : loads) {
    if (title == kDashboardID || load.missing()) {
      continue;
    }
    if (!load.readable()) {
      error = load.error ? load.error : Error(std::errc::bad_message);
      return {};
    }
    bytes += load.token->bytes().size();
    if (bytes > (256ull << 20)) {
      error = Error(std::errc::file_too_large);
      return {};
    }
    error = KnownRecordBounds(load.token->bytes(), false);
    if (error) {
      return {};
    }
    result->titles.emplace(title,
                           GpdInfoTitle(title, Bytes(load.token->bytes())));
  }
  return result;
}
}  // namespace
std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>>
ProfileGpdModelAuthority::Dependencies(
    const std::shared_ptr<ProfileGpdPendingMutation>& pending,
    std::error_code& error) const {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (!pending || pending_ != pending || retired_.load() ||
      storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  return baselines_;
}
std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>>
ProfileGpdModelAuthority::CaptureLoads(std::error_code& error) const {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (retired_.load() || storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (pending_) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  return baselines_;
}
std::optional<ProfileGpdReadCapture> ProfileGpdModelAuthority::CaptureRead(
    std::error_code& error) const {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (retired_.load() || storage_->retiring() || !models_ || !baselines_) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  ProfileGpdReadCapture out;
  out.origin = {xuid_,
                slot_,
                {sign_in_generation_, version_},
                storage_,
                weak_from_this()};
  out.models = models_;
  out.tokens.reserve(baselines_->size());
  for (const auto& [title, load] : *baselines_) {
    out.tokens.push_back(load.token);
  }
  return out;
}
std::shared_ptr<const ProfileGpdReadModels> ProfileGpdModelAuthority::Read(
    std::error_code& error) const {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (retired_.load(std::memory_order_acquire) || storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (!models_) {
    error = Error(std::errc::operation_would_block);
    return {};
  }
  return models_;
}
std::shared_ptr<ProfileGpdTransaction>
ProfileGpdModelAuthority::AcquireTransaction(
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery,
    std::error_code& error) {
  error.clear();
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  if (retired_.load() || storage_->retiring()) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (!pending_) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  if (pending_transaction_) {
    return pending_transaction_;
  }
  pending_transaction_ = ProfileGpdTransaction::Create(
      shared_from_this(), pending_, std::move(owner), std::move(recovery),
      error);  // Metadata only, before IO.
  return pending_transaction_;
}
void ProfileGpdModelAuthority::DiscardUnpublished(
    const std::shared_ptr<ProfileGpdPendingMutation>& pending,
    const ProfileGpdTransaction* transaction) {
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (lock.owns_lock() && pending_ == pending &&
      pending_transaction_.get() == transaction &&
      pending->origin().stamp.model_version == version_) {
    pending_.reset();
    pending_transaction_.reset();
  }
}
std::error_code ProfileGpdModelAuthority::ReleaseRetiredPublication() {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_would_block);
  }
  std::shared_ptr<ProfileGpdTransaction> transaction;
  {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return Error(std::errc::device_or_resource_busy);
    }
    if (!retired_.load()) {
      return Error(std::errc::operation_not_permitted);
    }
    transaction = pending_transaction_;
  }
  return transaction ? transaction->ReleaseForRecovery() : std::error_code{};
}
std::unique_ptr<ProfileGpdModelAuthority::PreparedCommit>
ProfileGpdModelAuthority::PrepareCommit(
    std::shared_ptr<ProfileGpdPendingMutation> pending,
    std::map<uint32_t, GpdSnapshotLoad> current, std::error_code& error) {
  error.clear();
  if (!pending || pending->origin().owner.lock().get() != this) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  auto plan = pending->BuildPlan(error);
  if (!plan || error) {
    return {};
  }
  std::shared_ptr<const std::map<uint32_t, GpdSnapshotLoad>> original;
  {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      error = Error(std::errc::device_or_resource_busy);
      return {};
    }
    if (pending_ != pending ||
        pending->origin().stamp.model_version != version_) {
      error = Error(std::errc::state_not_recoverable);
      return {};
    }
    original = baselines_;
  }
  for (const auto &[title, before] : *original) {
    if (!current.contains(title)) {
      error = Error(std::errc::invalid_argument);
      return {};
    }
  }
  for (const auto &[title, load] : current) {
    error = ValidateLoaded(load, *storage_, title);
    if (error) {
      return {};
    }
  }
  const bool no_write =
      std::all_of(plan->files.begin(), plan->files.end(), [](const auto &row) {
        return row.second.action == ProfileGpdFileAction::kUnchanged;
      });
  if (no_write) {
    for (const auto &[title, before] : *original) {
      const auto &now = current.at(title);
      const auto a = before.token->bytes(), b = now.token->bytes();
      if (before.missing() != now.missing() ||
          (!before.missing() &&
           (before.token->info().file_id != now.token->info().file_id ||
            before.token->info().filesystem_id !=
                now.token->info().filesystem_id)) ||
          a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin())) {
        error = Error(std::errc::state_not_recoverable);
        return {};
      }
    }
  }
  // Unchanged dependencies are fresh real reads, never normalized writebacks.
  // The common final batch proves all of them still current together.
  auto next = std::make_shared<std::map<uint32_t, GpdSnapshotLoad>>(current);
  auto prepared = std::unique_ptr<PreparedCommit>(new PreparedCommit);
  prepared->checked.reserve(current.size());
  for (const auto& [title, file] : plan->files) {
    auto found = current.find(title);
    if (found == current.end()) {
      error = Error(std::errc::invalid_argument);
      return {};
    }
    const auto& load = found->second;
    error = ValidateLoaded(load, *storage_, title);
    if (error) {
      return {};
    }
    // Only changed targets carry an intended-byte invariant. Native/VFS
    // changes to other files remain authoritative, including a Dashboard
    // member present in the plan solely as an unchanged dependency.
    if (file.action == ProfileGpdFileAction::kUnchanged) {
      continue;
    }
    const bool absent = file.action == ProfileGpdFileAction::kDelete;
    const auto expected = std::span<const uint8_t>(file.intended);
    if (load.missing() != absent ||
        (!absent && (load.token->bytes().size() != expected.size() ||
                     !std::equal(expected.begin(), expected.end(),
                                 load.token->bytes().begin())))) {
      error = Error(std::errc::state_not_recoverable);
      return {};
    }
    next->insert_or_assign(title, load);
  }
  for (const auto& [title, load] : *next) {
    prepared->checked.push_back(load.token);
  }
  prepared->models = ReadModels(*next, error);
  if (error) {
    return {};
  }
  prepared->expected_version = pending->origin().stamp.model_version;
  prepared->baselines = std::move(next);
  prepared->pending = std::move(pending);
  return prepared;
}
std::unique_ptr<ProfileGpdModelAuthority::CommitGuard>
ProfileGpdModelAuthority::TryCommit(
    std::unique_ptr<PreparedCommit> prepared,
    std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch,
    std::error_code& error) {
  error.clear();
  if (!prepared || !batch || batch->root() != storage_->view() ||
      batch->tokens() != prepared->checked) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  auto result = std::unique_ptr<CommitGuard>(new CommitGuard);
  result->owner_ = shared_from_this();
  result->prepared_ = std::move(prepared);
  result->batch_ = std::move(batch);
  result->lock_ = std::unique_lock(mutex_, std::try_to_lock);
  if (!result->lock_.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  const auto& prepared_state = *result->prepared_;
  const auto& pending = prepared_state.pending;
  if (retired_.load(std::memory_order_acquire) || storage_->retiring() ||
      pending_ != pending || prepared_state.expected_version != version_ ||
      version_ == UINT64_MAX) {
    error = Error(std::errc::operation_canceled);
    return {};
  }
  if (pending) {
    const auto& origin = pending->origin();
    if (origin.storage != storage_ || origin.owner.lock().get() != this ||
        origin.xuid != xuid_ || origin.user_slot != slot_ ||
        origin.stamp.sign_in_generation != sign_in_generation_ ||
        origin.stamp.model_version != version_) {
      error = Error(std::errc::operation_canceled);
      return {};
    }
  }
  return result;
}
bool ProfileGpdModelAuthority::CommitGuard::Apply() noexcept {
  if (applied_) {
    return true;
  }
  if (owner_->retired_.load(std::memory_order_acquire) ||
      owner_->storage_->retiring()) {
    return false;
  }
  owner_->baselines_ = std::move(prepared_->baselines);
  owner_->models_ = std::move(prepared_->models);
  ++owner_->version_;
  owner_->pending_.reset();
  owner_->pending_transaction_.reset();
  applied_ = true;
  return true;
}
}  // namespace xe::kernel::xam
