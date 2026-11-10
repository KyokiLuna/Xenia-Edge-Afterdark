/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/base/mutex.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/vfs/devices/host_path_device.h"
#include <set>
namespace xe::kernel::xam {
namespace {
std::error_code Error(std::errc code) { return std::make_error_code(code); }
std::optional<std::map<uint32_t, GpdSnapshotLoad>>
LoadCurrentModels(const std::shared_ptr<ProfileStorageLease> &lease,
                  const std::map<uint32_t, GpdSnapshotLoad> *previous,
                  std::error_code &error) {
  error.clear();
  std::map<uint32_t, GpdSnapshotLoad> loads;
  auto dashboard = GpdSnapshotStore::Load(
      {lease->xuid(), lease->source_generation(), kDashboardID}, lease->view());
  if (!dashboard.readable()) {
    error = dashboard.error ? dashboard.error
                            : Error(std::errc::no_such_file_or_directory);
    return {};
  }
  const auto wire = XdbfTableView::Parse(dashboard.token->bytes());
  if (!wire) {
    error = Error(std::errc::bad_message);
    return {};
  }
  std::set<uint32_t> titles;
  if (previous) {
    for (const auto &[title, load] : *previous) {
      if (title != kDashboardID) {
        titles.insert(title);
      }
    }
  }
  for (size_t i = 0; i != wire->entries_used; ++i) {
    const auto *e = wire->entries.data() + i * 18;
    const uint16_t section = uint16_t(e[0]) << 8 | e[1];
    const uint64_t id =
        uint64_t(XdbfTableView::U32(e + 2)) << 32 | XdbfTableView::U32(e + 6);
    if (section == uint16_t(GpdSection::kTitle) && id && id <= UINT32_MAX &&
        id != kDashboardID) {
      if (!titles.contains(uint32_t(id)) && titles.size() >= 4095) {
        error = Error(std::errc::file_too_large);
        return {};
      }
      titles.insert(uint32_t(id));
    }
  }
  uint64_t aggregate = dashboard.token->bytes().size();
  loads.emplace(kDashboardID, std::move(dashboard));
  for (auto id : titles) {
    auto load = GpdSnapshotStore::Load(
        {lease->xuid(), lease->source_generation(), id}, lease->view());
    if (load.error) {
      error = load.error;
      return {};
    }
    if (load.token->bytes().size() >
        filesystem::HostStorageRoot::kMaximumSnapshotBatchBytes - aggregate) {
      error = Error(std::errc::file_too_large);
      return {};
    }
    aggregate += load.token->bytes().size();
    loads.emplace(id, std::move(load));
  }
  return loads;
}
}  // namespace
std::error_code ProfileGpdWriter::Reload() const {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_would_block);
  }
  if (!model || !owner || !recovery) {
    return Error(std::errc::no_such_device);
  }
  const auto lease = model->storage();
  std::error_code pending_error;
  const auto pending = model->Pending(pending_error);
  if (pending_error) {
    return pending_error;
  }
  if (pending) {
    return Error(std::errc::device_or_resource_busy);
  }
  auto error = ProfileGpdTransaction::ReplayBeforeLoad(lease, owner, recovery);
  if (error) {
    return error;
  }
  auto previous = model->CaptureLoads(error);
  if (error) {
    return error;
  }
  auto current = LoadCurrentModels(lease, previous.get(), error);
  if (!current) {
    return error;
  }
  auto prepared = model->PrepareLoaded(std::move(*current), error);
  if (!prepared) {
    return error;
  }
  auto cache = lease->device()->PrepareSnapshotCache(prepared->tokens(), error);
  if (!cache) {
    return error;
  }
  auto checked =
      lease->view()->ValidateSnapshotBatch(prepared->tokens(), error);
  if (!checked) {
    return error;
  }
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(
      std::move(checked));
  auto model_commit = model->TryCommit(std::move(prepared), batch, error);
  if (!model_commit) {
    return error;
  }
  auto cache_commit =
      lease->device()->TryCommitSnapshotCache(std::move(cache), batch, error);
  if (!cache_commit) {
    return error;
  }
  if (!model_commit->Apply()) {
    return std::make_error_code(std::errc::operation_canceled);
  }
  cache_commit->Apply();
  return {};
}
std::optional<ProfileGpdMutationOrigin>
ProfileGpdWriter::CaptureCurrentOrigin(const ProfileGpdMutation &command,
                                       std::error_code &error) const {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = Error(std::errc::operation_would_block);
    return {};
  }
  if (!model || !owner || !recovery) {
    error = Error(std::errc::no_such_device);
    return {};
  }
  const auto pending = model->Pending(error);
  if (error) {
    return {};
  }
  if (pending) {
    if (!pending->Matches(command)) {
      error = Error(std::errc::device_or_resource_busy);
      return {};
    }
    if (!model->IsCurrent(pending->origin())) {
      error = Error(std::errc::operation_canceled);
      return {};
    }
    return pending->origin();
  }
  for (unsigned attempt = 0; attempt != 2; ++attempt) {
    const auto captured = model->CaptureRead(error);
    if (!captured) {
      return {};
    }
    {
      const auto checked = model->storage()->view()->ValidateSnapshotBatch(
          captured->tokens, error);
      if (checked) {
        if (!model->IsCurrent(captured->origin)) {
          error = Error(std::errc::operation_canceled);
          return {};
        }
        return captured->origin;
      }
    }
    if (attempt || (error != std::errc::state_not_recoverable &&
                    error != std::errc::no_such_file_or_directory)) {
      return {};
    }
    error = Reload();
    if (error) {
      return {};
    }
  }
  error = Error(std::errc::device_or_resource_busy);
  return {};
}
ProfileGpdTransactionAttempt ProfileGpdWriter::Submit(
    ProfileGpdMutation command,
    std::optional<ProfileGpdModelStamp> required_stamp) const {
  ProfileGpdTransactionAttempt out;
  if (!model || !owner || !recovery) {
    out.error = Error(std::errc::no_such_device);
    return out;
  }
  auto admitted = CaptureCurrentOrigin(command, out.error);
  if (!admitted) {
    return out;
  }
  if (required_stamp &&
      (required_stamp->sign_in_generation !=
           admitted->stamp.sign_in_generation ||
       required_stamp->model_version != admitted->stamp.model_version)) {
    out.error = Error(std::errc::operation_canceled);
    return out;
  }
  // TouchTitle is the only creation producer. It first captures a genuine
  // absent/readable token; a default GPD model never grants create authority.
  if (command.kind == ProfileGpdMutationKind::kTouchTitle &&
      !model->Pending(out.error)) {
    auto loads = model->CaptureLoads(out.error);
    if (!loads) {
      return out;
    }
    if (!loads->contains(command.title_id)) {
      const auto lease = model->storage();
      auto current = GpdSnapshotStore::Load(
          {lease->xuid(), lease->source_generation(), command.title_id},
          lease->view());
      if (current.error) {
        out.error = current.error;
        return out;
      }
      auto next = *loads;
      next.emplace(command.title_id, std::move(current));
      auto prepared = model->PrepareLoaded(std::move(next), out.error);
      if (!prepared) {
        return out;
      }
      auto cache =
          lease->device()->PrepareSnapshotCache(prepared->tokens(), out.error);
      if (!cache) {
        return out;
      }
      auto checked =
          lease->view()->ValidateSnapshotBatch(prepared->tokens(), out.error);
      if (!checked) {
        return out;
      }
      std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(
          std::move(checked));
      auto model_commit =
          model->TryCommit(std::move(prepared), batch, out.error);
      if (!model_commit) {
        return out;
      }
      auto cache_commit = lease->device()->TryCommitSnapshotCache(
          std::move(cache), batch, out.error);
      if (!cache_commit) {
        return out;
      }
      if (!model_commit->Apply()) {
        out.error = Error(std::errc::operation_canceled);
        return out;
      }
      cache_commit->Apply();
    }
  }
  // TouchTitle's explicit absent-token extension may have installed a new
  // model version. Original stamped deferred requests may never rebind to it.
  admitted = CaptureCurrentOrigin(command, out.error);
  if (!admitted) {
    return out;
  }
  if (required_stamp &&
      (required_stamp->sign_in_generation !=
           admitted->stamp.sign_in_generation ||
       required_stamp->model_version != admitted->stamp.model_version)) {
    out.error = Error(std::errc::operation_canceled);
    return out;
  }
  bool retry = false;
  auto pending = model->Begin(std::move(command), retry, out.error,
                              required_stamp.value_or(admitted->stamp));
  if (!pending) {
    return out;
  }
  return ExecutePending(std::move(pending));
}
ProfileGpdTransactionAttempt ProfileGpdWriter::ExecutePending(
    std::shared_ptr<ProfileGpdPendingMutation> pending) const {
  ProfileGpdTransactionAttempt out;
  if (!model || !pending || pending->origin().owner.lock() != model ||
      model->Pending(out.error) != pending) {
    if (!out.error) {
      out.error = Error(std::errc::operation_canceled);
    }
    return out;
  }
  auto transaction = model->AcquireTransaction(owner, recovery, out.error);
  return transaction ? transaction->Attempt() : out;
}
DurableAchievementEvent::DurableAchievementEvent(
    std::shared_ptr<const ProfileGpdPendingMutation> pending,
    ProfileGpdMutationOrigin origin)
    : pending_(std::move(pending)), origin_(std::move(origin)) {}
bool DurableAchievementEvent::Current() const {
  auto owner = origin_.owner.lock();
  return owner && owner->IsCurrent(origin_) &&
         !origin_.storage->CheckAvailable();
}
GpdIntentBinding ProfileGpdTransaction::Binding(
    const ProfileStorageLease& lease,
    const filesystem::ProfileIntentOwner& owner) {
  return {lease.source_id(),
          lease.device_id(),
          lease.xuid(),
          lease.source_generation(),
          lease.view()->path().lexically_relative(lease.source()->path()),
          {owner.managed_identity()->uuid(),
           owner.managed_identity()->content_epoch(),
           owner.managed_identity()->directory_file_id()}};
}
std::shared_ptr<ProfileGpdTransaction> ProfileGpdTransaction::Create(
    std::shared_ptr<ProfileGpdModelAuthority> model,
    std::shared_ptr<ProfileGpdPendingMutation> pending,
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery,
    std::error_code& error) {
  error.clear();
  if (!model || !pending || !owner || !recovery ||
      pending->origin().owner.lock() != model ||
      owner->source() != pending->origin().storage->source() ||
      owner->managed_identity() !=
          pending->origin().storage->managed_identity()) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  if (owner->error() || recovery->error()) {
    error = owner->error() ? owner->error() : recovery->error();
    return {};
  }
  auto result =
      std::shared_ptr<ProfileGpdTransaction>(new ProfileGpdTransaction);
  result->model_ = model;
  result->pending_ = std::move(pending);
  result->owner_ = std::move(owner);
  result->recovery_ = std::move(recovery);
  return result;
}
ProfileGpdTransactionAttempt ProfileGpdTransaction::Attempt() {
  using State = ProfileGpdTransactionAttempt::State;
  ProfileGpdTransactionAttempt out;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    out.error = Error(std::errc::operation_would_block);
    return out;
  }
  std::unique_lock serial(mutex_, std::try_to_lock);
  if (!serial.owns_lock()) {
    out.error = Error(std::errc::device_or_resource_busy);
    return out;
  }
  if (committed_) {
    out.state = State::kAlreadyCommitted;
    return out;
  }
  if (preparation_uncertain_) {
    out.state = State::kPreparationUncertain;
    out.error = Error(std::errc::state_not_recoverable);
    out.attempted_journal = attempted_journal_;
    return out;
  }
  auto model = model_.lock();
  auto lease = pending_->origin().storage;
  out.error = lease->CheckAvailable();
  if (!model || out.error || !model->IsCurrent(pending_->origin())) {
    out.state = State::kRetired;
    if (!out.error) {
      out.error = Error(std::errc::operation_canceled);
    }
    return out;
  }
  const auto original = model->Dependencies(pending_, out.error);
  if (!original) {
    return out;
  }
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      original_tokens;
  for (const auto &[title, load] : *original) {
    original_tokens.push_back(load.token);
  }
  if (!store_ && attempted_journal_.empty()) {
    bool current = false;
    {
      const auto checked =
          lease->view()->ValidateSnapshotBatch(original_tokens, out.error);
      current = checked && model->IsCurrent(pending_->origin());
      if (!current && !out.error) {
        out.error = Error(std::errc::operation_canceled);
      }
    }
    if (!current) {
      model->DiscardUnpublished(pending_, this);
      return out;
    }
  }
  auto plan = pending_->BuildPlan(out.error);
  if (!plan || out.error) {
    if (!store_ && attempted_journal_.empty()) {
      model->DiscardUnpublished(pending_, this);
    }
    return out;
  }
  std::vector<GpdIntentMutation> mutations;
  for (const auto& [title, file] : plan->files) {
    if (file.action == ProfileGpdFileAction::kUnchanged) {
      continue;
    }
    GpdIntentAction action;
    switch (file.action) {
      case ProfileGpdFileAction::kReplace:
        action = GpdIntentAction::kReplace;
        break;
      case ProfileGpdFileAction::kCreate:
        action = GpdIntentAction::kCreate;
        break;
      case ProfileGpdFileAction::kDelete:
        action = GpdIntentAction::kDelete;
        break;
      default:
        out.error = Error(std::errc::invalid_argument);
        return out;
    }
    mutations.push_back({action, file.before, file.intended});
  }
  if (!mutations.empty()) {
    if (!store_) {
      try {
        store_ = GpdDurableIntentStore::Prepare(
            owner_, recovery_, Binding(*lease, *owner_), std::move(mutations),
            out.error, attempted_journal_, original_tokens);
      } catch (...) {
        preparation_uncertain_ = !attempted_journal_.empty();
        out.error = Error(std::errc::io_error);
        out.attempted_journal = attempted_journal_;
        if (preparation_uncertain_) {
          out.state = State::kPreparationUncertain;
        } else {
          model->DiscardUnpublished(pending_, this);
        }
        return out;
      }
      out.attempted_journal = attempted_journal_;
      if (!store_) {
        preparation_uncertain_ = !attempted_journal_.empty();
        if (preparation_uncertain_) {
          out.state = State::kPreparationUncertain;
        } else {
          model->DiscardUnpublished(pending_, this);
        }
        return out;
      }
    }
    try {
      out.storage = store_->Advance();
    } catch (...) {
      out.error = Error(std::errc::io_error);
      out.attempted_journal = attempted_journal_;
      return out;
    }
    out.error = out.storage.error;
    if (out.error || !out.storage.intent_durable || !out.storage.done_durable) {
      if (!out.error) {
        out.error = Error(std::errc::operation_would_block);
      }
      return out;
    }
  }
  out.state = State::kDurablePendingCommit;
  const auto dependencies = model->Dependencies(pending_, out.error);
  if (!dependencies) {
    return out;
  }
  auto current = LoadCurrentModels(lease, dependencies.get(), out.error);
  if (!current) {
    return out;
  }
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      tokens;
  auto prepared_model =
      model->PrepareCommit(pending_, std::move(*current), out.error);
  if (!prepared_model) {
    if (!store_ && attempted_journal_.empty()) {
      model->DiscardUnpublished(pending_, this);
    }
    return out;
  }
  tokens = prepared_model->tokens();  // Include unchanged model dependencies in
                                      // final currentness proof.
  auto prepared_cache =
      lease->device()->PrepareSnapshotCache(tokens, out.error);
  if (!prepared_cache) {
    return out;
  }
  // Preallocate the live event before any final storage/namespace/model guard.
  // This is not persisted, and no UI consumer can see it until actual commit.
  std::shared_ptr<const DurableAchievementEvent> event;
  if (pending_->command().kind == ProfileGpdMutationKind::kUnlock &&
      !plan->already_unlocked) {
    auto origin = pending_->origin();
    ++origin.stamp.model_version;
    event = std::shared_ptr<const DurableAchievementEvent>(
        new DurableAchievementEvent(pending_, std::move(origin)));
  }
  {
    auto checked =
        lease->view()->ValidateSnapshotBatch(std::move(tokens), out.error);
    if (!checked) {
      return out;
    }
    std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(
        std::move(checked));
    auto model_commit =
        model->TryCommit(std::move(prepared_model), batch, out.error);
    if (!model_commit) {
      return out;
    }
    auto cache_commit = lease->device()->TryCommitSnapshotCache(
        std::move(prepared_cache), batch, out.error);
    if (!cache_commit) {
      return out;
    }
    if (!model_commit->Apply()) {
      out.error = Error(std::errc::operation_canceled);
      out.state = State::kRetired;
      return out;
    }
    cache_commit->Apply();  // No IO, allocation or fallible metadata step.
    committed_ = true;
  }
  out.state =
      plan->already_unlocked ? State::kAlreadyUnlocked : State::kCommitted;
  out.event = std::move(event);  // Recovered history never enters this path.
  return out;
}
std::error_code ProfileGpdTransaction::ReleaseForRecovery() {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_would_block);
  }
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return Error(std::errc::device_or_resource_busy);
  }
  auto model = model_.lock();
  if (model && model->IsCurrent(pending_->origin())) {
    return Error(std::errc::operation_not_permitted);
  }
  store_.reset();  // Preserve journal/plan, release only actual target lease.
  return {};
}
std::error_code ProfileGpdTransaction::ReplayBeforeLoad(
    std::shared_ptr<ProfileStorageLease> lease,
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_would_block);
  }
  if (!lease || !owner || !recovery || owner->source() != lease->source()) {
    return Error(std::errc::invalid_argument);
  }
  auto error = lease->CheckAvailable();
  if (error) {
    return error;
  }
  const auto binding = Binding(*lease, *owner);
  const auto rows = GpdDurableIntentStore::Scan(*owner, error);
  if (error) {
    return error;
  }  // Malformed/foreign journals never become defaults.
  for (const auto& row : rows) {
    if (row.error) {
      return row.error;
    }
    if (row.binding.profile_relative != binding.profile_relative) {
      continue;
    }
    if (row.binding.xuid != binding.xuid ||
        row.binding.device_id != binding.device_id ||
        row.binding.source_id != binding.source_id ||
        row.binding.managed_source != binding.managed_source) {
      return Error(std::errc::state_not_recoverable);
    }
    if (row.done_record_valid) {
      continue;
    }
    auto store = GpdDurableIntentStore::Recover(owner, recovery, row.journal,
                                                binding, lease->view(), error);
    if (!store) {
      return error ? error : Error(std::errc::state_not_recoverable);
    }
    const auto attempt = store->Advance();
    if (attempt.error) {
      return attempt.error;
    }
    if (!attempt.intent_durable || !attempt.done_durable) {
      return Error(std::errc::operation_would_block);
    }
  }
  return {};  // Data recovery only. No event, IsUnlocked replay or new
              // counters.
}
}  // namespace xe::kernel::xam
