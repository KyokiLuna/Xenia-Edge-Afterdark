/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/base/mutex.h"
#include "xenia/base/logging.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/kernel/xam/tile_service.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <source_location>
#include "xenia/base/xdbf_view.h"
#include "xenia/vfs/devices/host_path_device.h"
#include <set>
namespace xe::kernel::xam {
bool ProfilePreparationProgress::Retry(const std::error_code& error) const {
  if((error!=std::errc::device_or_resource_busy &&
      error!=std::errc::resource_unavailable_try_again) ||
      global_critical_region::is_held_by_current_thread() ||
      (admission_open && !admission_open->load(std::memory_order_acquire)) ||
      std::chrono::steady_clock::now()>=deadline || !wait)return false;
  return wait() && (!admission_open || admission_open->load(std::memory_order_acquire)) &&
      std::chrono::steady_clock::now()<deadline;
}
namespace {
std::error_code Error(std::errc code) { return std::make_error_code(code); }
// Scalar-only optional observation. Declare first in each observed function,
// so every model/cache/filesystem/native guard is destroyed before emission.
// No ProfileManager, Kernel, model, account, path or native owner is retained.
class GpdLoadRefusal final {
 public:
  explicit GpdLoadRefusal(bool repair) noexcept
      : enabled_(cvars::native_system_ui_trace), repair_(repair) {}
  ~GpdLoadRefusal() {
    // An inherited caller GCRC is not ours to release. Keep that real refusal
    // unchanged and do not format/log under it (as for admission diagnostics).
    if (!enabled_ || !stage_ ||
        global_critical_region::is_held_by_current_thread())
      return;
    static NativeSystemUiTraceLimiter trace;
    trace.EmitTransition(
        (uint64_t(repair_) << 63) | (uint64_t(cache_refusal_ & 3u) << 56) |
            (uint64_t(stage_) << 32) | error_,
        "profile", repair_ ? "gpd_repair_refusal" : "gpd_reload_refusal",
        {{"stage", stage_},
         {"source_line", line_},
         {"error", error_},
         {"category", category_},{"cache_refusal",cache_refusal_}});
  }
  std::error_code Result(
      uint32_t stage, std::error_code error,
      std::source_location where = std::source_location::current()) noexcept {
    // Disabled mode performs no field/category/line capture or formatting.
    if (enabled_ && error && !stage_) {
      stage_ = stage;
      line_ = where.line();
      error_ = uint32_t(error.value());
      category_ = error.category() == std::generic_category()  ? 1u
                  : error.category() == std::system_category() ? 2u
                                                               : 3u;
    }
    return error;
  }
  std::error_code CacheResult(uint32_t stage,std::error_code error,
      vfs::HostPathDevice::SnapshotCacheRefusal why,
      std::source_location where=std::source_location::current()) noexcept {
    if(enabled_ && error && !stage_)cache_refusal_=uint32_t(why);
    return Result(stage,error,where);
  }

 private:
  const bool enabled_;
  const bool repair_;
  uint32_t stage_ = 0, line_ = 0, error_ = 0, category_ = 0, cache_refusal_ = 0;
};
GpdSnapshotLoad LoadCurrentFile(const std::shared_ptr<ProfileStorageLease>& lease,
    uint32_t title, const filesystem::SnapshotPublicationSet* members) {
  const GpdSnapshotIdentity identity{lease->xuid(), lease->source_generation(), title};
  if (!members) { return GpdSnapshotStore::Load(identity, lease->view()); }
  GpdSnapshotLoad out; out.identity = identity;
  if (members->base() != lease->view()) { out.error = Error(std::errc::invalid_argument); return out; }
  char leaf[13]; std::snprintf(leaf, sizeof(leaf), "%08X.gpd", title);
  out.token = members->Capture(leaf, GpdSnapshotStore::kMaximumBytes, out.error, true);
  if (!out.error && out.token->exists()) { out.error = ValidateGpdSnapshot(out.token->bytes()); }
  return out;
}
std::optional<std::map<uint32_t, GpdSnapshotLoad>>
LoadCurrentModels(const std::shared_ptr<ProfileStorageLease> &lease,
                  const std::map<uint32_t, GpdSnapshotLoad> *previous,
                  std::error_code &error,
                  const filesystem::SnapshotPublicationSet* members = nullptr) {
  error.clear();
  std::map<uint32_t, GpdSnapshotLoad> loads;
  auto dashboard = LoadCurrentFile(lease, kDashboardID, members);
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
    auto load = LoadCurrentFile(lease, id, members);
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
// A known broken host producer wrote exactly 24 ASCII hex characters plus
// NUL in UTF-16LE. A valid BE key can never satisfy this predicate. Retain
// every unrecognized, truncated, custom or ambiguous value without coercion.
std::optional<std::pair<std::u16string, std::array<uint32_t, 3>>> LegacyLittleEndianKey(
    const UserSetting& setting) {
  const auto bytes = setting.get_extended_data();
  if (bytes.size() != 50 || bytes[48] || bytes[49]) { return {}; }
  std::u16string key;
  std::array<uint32_t, 3> fields{};
  for (size_t i = 0; i != 24; ++i) {
    const uint8_t c = bytes[i * 2];
    if (bytes[i * 2 + 1]) { return {}; }
    uint32_t digit;
    if (c >= '0' && c <= '9') { digit = c - '0'; }
    else if (c >= 'A' && c <= 'F') { digit = c - 'A' + 10; }
    else if (c >= 'a' && c <= 'f') { digit = c - 'a' + 10; }
    else { return {}; }
    key.push_back(char16_t(c));
    fields[i / 8] = (fields[i / 8] << 4) | digit;
  }
  return std::pair{std::move(key), fields};
}
}  // namespace
std::error_code ProfileGpdWriter::RepairLegacyPictureKeys(
    std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> installed,
    const ProfilePreparationProgress* progress) const {
  GpdLoadRefusal diagnostic(true);
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return diagnostic.Result(1, Error(std::errc::operation_would_block));
  }
  if (!model || !owner || !recovery) {
    return diagnostic.Result(2, Error(std::errc::no_such_device));
  }
  std::error_code error;
  const auto captured = model->CaptureRead(error);
  if (!captured) {
    return diagnostic.Result(3, error);
  }
  ProfileGpdMutation command;
  command.kind = ProfileGpdMutationKind::kSetSettings;
  command.title_id = kDashboardID;
  std::vector<uint32_t> indexes;
  for (const auto id : {UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY,
                        UserSettingId::XPROFILE_GAMERCARD_PERSONAL_PICTURE}) {
    std::optional<UserSetting> stored;
    if (UserSetting::ReadFromGpd(captured->models->dashboard, uint32_t(id), stored)
        != X_ERROR_SUCCESS) {
      XELOGW("Profile {:016X}: unreadable picture key {:08X} retained without repair",
             model->xuid(), uint32_t(id));
      continue;
    }
    if (!stored) { continue; }
    const auto corrected = LegacyLittleEndianKey(*stored);
    if (!corrected) { continue; }
    const auto& fields = corrected->second;
    const uint32_t index = fields[1] & 0xFFFF;
    if (fields[0] != kDashboardID || fields[1] != (0x00020000u | index) ||
        fields[2] != (0x00010000u | index) || index >= TileService::kStockCount) {
      XELOGW("Profile {:016X}: unrecognized legacy picture tuple {:08X} retained without repair",
             model->xuid(), uint32_t(id));
      continue;
    }
    command.settings.emplace_back(id, corrected->first);
    indexes.push_back(index);
  }
  // Valid BE, custom and unknown metadata need no source and no write.
  if (command.settings.empty()) { return {}; }
  X_HRESULT status;
  const auto stock = TileService::CaptureStockSource(std::move(installed), status);
  if (!stock || status != X_E_SUCCESS) {
    return diagnostic.Result(4, Error(std::errc::no_such_device));
  }
  // Revalidate the actual tuple against genuine installed resources. Repair
  // changes only its proven byte order; it does not create/replace any image.
  for (const auto index : indexes) {
    for (bool small : {false, true}) {
      const auto picture = TileService::ReadStock(*stock, {index, small});
      if (picture.status != X_E_SUCCESS || picture.bytes.empty()) {
        return diagnostic.Result(5,
                                 Error(std::errc::no_such_file_or_directory));
      }
    }
  }
  command.request_id = model->AllocateRequestId(error);
  if (error) {
    return diagnostic.Result(6, error);
  }
  // Existing checked upsert/journal preserves the old GPD as recovery bytes,
  // unknown headers/suffixes, actual source, identity and model-version CAS.
  for(;;){
    const auto attempt=Submit(command,captured->origin.stamp);
    if(attempt.error){
      if(progress && progress->Retry(attempt.error))continue;
      return diagnostic.Result(7,attempt.error);
    }
    if(attempt.state==ProfileGpdTransactionAttempt::State::kCommitted ||
        attempt.state==ProfileGpdTransactionAttempt::State::kAlreadyCommitted)return {};
    const auto busy=Error(std::errc::operation_would_block);
    if((attempt.state!=ProfileGpdTransactionAttempt::State::kPending &&
        attempt.state!=ProfileGpdTransactionAttempt::State::kDurablePendingCommit) ||
        !progress || !progress->Retry(busy))return diagnostic.Result(8,busy);
    // SAME command/request/stamp; the real pending transaction owns any
    // durable intent. Never allocate/rebase a second repair after partial IO.
  }
}
std::error_code ProfileGpdWriter::Reload(const ProfilePreparationProgress* progress) const {
  GpdLoadRefusal diagnostic(false);
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return diagnostic.Result(1, Error(std::errc::operation_would_block));
  }
  if (!model || !owner || !recovery) {
    return diagnostic.Result(2, Error(std::errc::no_such_device));
  }
  const auto lease = model->storage();
  std::error_code pending_error;
  const auto pending = model->Pending(pending_error);
  if (pending_error) {
    return diagnostic.Result(3, pending_error);
  }
  if (pending) {
    return diagnostic.Result(4, Error(std::errc::device_or_resource_busy));
  }
  auto error = ProfileGpdTransaction::ReplayBeforeLoad(lease, owner, recovery, metadata);
  if (error) {
    return diagnostic.Result(5, error);
  }
  auto previous = model->CaptureLoads(error);
  if (error) {
    return diagnostic.Result(6, error);
  }
  auto current = LoadCurrentModels(lease, previous.get(), error);
  if (!current) {
    return diagnostic.Result(7, error);
  }
  auto pictures = CaptureProfilePictures(lease->view(), error);
  if (!pictures) {
    return diagnostic.Result(8, error);
  }
  auto prepared = model->PrepareLoaded(std::move(*current), error, std::move(pictures));
  if (!prepared) {
    return diagnostic.Result(9, error);
  }
  std::shared_ptr<vfs::HostPathDevice::PreparedSnapshotCache> cache;
  for(;;){
    if(!cache){
      cache=lease->device()->PrepareSnapshotCache(prepared->tokens(),error);
      if(!cache){
        if(progress && progress->Retry(error))continue;
        return diagnostic.CacheResult(10,error,error==std::errc::device_or_resource_busy?
            vfs::HostPathDevice::SnapshotCacheRefusal::kNamespaceBusy:vfs::HostPathDevice::SnapshotCacheRefusal::kNone);
      }
    }
    auto checked=lease->view()->ValidateSnapshotBatch(prepared->tokens(),error);
    if(!checked){
      if(progress && progress->Retry(error))continue;
      return diagnostic.Result(11,error);
    }
    std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
    auto model_commit=model->TryCommitPreserving(prepared,batch,error);
    if(!model_commit){
      batch.reset(); // actual filesystem guard ends BEFORE any wait
      if(progress && progress->Retry(error))continue;
      return diagnostic.Result(12,error);
    }
    vfs::HostPathDevice::SnapshotCacheRefusal why;
    auto cache_commit=lease->device()->TryCommitSnapshotCache(cache,batch,error,&why);
    if(!cache_commit){
      prepared=model_commit->ReleaseUnapplied();model_commit.reset();batch.reset();
      if(!prepared)return diagnostic.Result(13,Error(std::errc::operation_canceled));
      if(why==vfs::HostPathDevice::SnapshotCacheRefusal::kCapacityChanged)cache.reset();
      // Immutable models/tokens stay owned. Every new final attempt performs
      // genuine snapshot validation; stale bytes never become a fresh load.
      if(progress && progress->Retry(error))continue;
      return diagnostic.CacheResult(13,error,why);
    }
    if(!model_commit->Apply())return diagnostic.Result(14,Error(std::errc::operation_canceled));
    cache_commit->Apply();return {};
  }
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
  if (command.kind == ProfileGpdMutationKind::kSetPictures) {
    const auto existing = model->Pending(out.error);
    if (out.error) { return out; }
    if (!existing) {
      const auto read = model->CaptureRead(out.error);
      if (!read) { return out; }
      if (!read->pictures && (out.error = Reload())) { return out; }
    }
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
  auto transaction = model->AcquireTransaction(owner, recovery, out.error, metadata);
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
    std::error_code& error, std::shared_ptr<filesystem::ContentMutationOwner> metadata) {
  return CreateImpl(std::move(model), std::move(pending), std::move(owner),
                    std::move(recovery), error, std::move(metadata), {});
}
std::shared_ptr<ProfileGpdTransaction> ProfileGpdTransaction::CreateImpl(
    std::shared_ptr<ProfileGpdModelAuthority> model,
    std::shared_ptr<ProfileGpdPendingMutation> pending,
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery,
    std::error_code& error, std::shared_ptr<filesystem::ContentMutationOwner> metadata,
    std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication> native_owner) {
  error.clear();
  if (pending && pending->native_issued() && !pending->NativeOwnedBy(native_owner)) {
    error = Error(std::errc::device_or_resource_busy); return {};
  }
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
  if (!metadata && std::any_of(pending->command().picture_changes.begin(),
      pending->command().picture_changes.end(), [](const auto& edit) {
        return edit.kind == filesystem::ProfileArtifactKind::kThumbnail;
      })) {
    error = Error(std::errc::no_such_device); return {};
  }
  auto result =
      std::shared_ptr<ProfileGpdTransaction>(new ProfileGpdTransaction);
  result->model_ = model;
  result->pending_ = std::move(pending);
  result->owner_ = std::move(owner);
  result->recovery_ = std::move(recovery);
  result->metadata_ = std::move(metadata);
  return result;
}
ProfileGpdTransactionAttempt ProfileGpdTransaction::Attempt() {
  return AttemptImpl({});
}
ProfileGpdTransactionAttempt ProfileGpdTransaction::AttemptNative(
    std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication> owner) {
  return AttemptImpl(std::move(owner));
}
ProfileGpdTransactionAttempt ProfileGpdTransaction::AttemptNativeRecovery(
    std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication> owner) {
  return AttemptImpl(std::move(owner), true);
}
ProfileGpdTransactionAttempt ProfileGpdTransaction::AttemptImpl(
    std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication> native_owner,
    bool attempted_recovery_only) {
  using State = ProfileGpdTransactionAttempt::State;
  ProfileGpdTransactionAttempt out;
  if (pending_->native_issued() && !pending_->NativeOwnedBy(native_owner)) {
    out.error = Error(std::errc::device_or_resource_busy); return out;
  }
  if (xe::global_critical_region::is_held_by_current_thread()) {
    out.error = Error(std::errc::operation_would_block);
    return out;
  }
  std::unique_lock serial(mutex_, std::try_to_lock);
  if (!serial.owns_lock()) {
    out.error = Error(std::errc::device_or_resource_busy);
    return out;
  }
  // Inspect the ACTUAL store/attempted path/uncertainty under the SAME
  // transaction serial that owns first preparation and cancellation. Caller
  // outcome flags or a sealed payload alone cannot authorize recovery writes.
  if (attempted_recovery_only && !store_ && attempted_journal_.empty() &&
      !preparation_uncertain_ && !committed_) {
    out.error = Error(std::errc::operation_not_permitted);
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
  auto plan = pending_->BuildPlan(out.error);
  if (!plan || out.error) {
    if (!store_ && attempted_journal_.empty() && !native_owner) model->DiscardUnpublished(pending_, this);
    return out;
  }
  if (plan->native_artifacts || plan->package_header) {
    if (!native_owner || !pending_->NativeOwnedBy(native_owner) || !plan->native_artifacts ||
        !plan->native_artifacts->Matches(owner_, recovery_, Binding(*lease, *owner_)) ||
        !plan->native_artifacts->members() || plan->native_artifacts->members()->base() != lease->view() ||
        bool(plan->native_artifacts->header()) != bool(plan->package_header) ||
        (artifact_admission_ && artifact_admission_ != plan->native_artifacts)) {
      out.error = Error(std::errc::permission_denied); return out;
    }
    // Transfer the actual early-issued reservation/session. Re-acquiring here
    // would conflict with our own native stage and lose its original owner.
    artifact_admission_ = plan->native_artifacts;
  }
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      original_tokens;
  for (const auto &[title, load] : *original) {
    original_tokens.push_back(load.token);
  }
  if (!plan->pictures.empty() && pending_->pictures_before()) {
    for (const auto& [name, token] : *pending_->pictures_before()) { original_tokens.push_back(token); }
  }
  if (!store_ && attempted_journal_.empty()) {
    bool current = false;
    {
      const auto checked = artifact_admission_
          ? artifact_admission_->members()->Validate(original_tokens, out.error)
          : lease->view()->ValidateSnapshotBatch(original_tokens, out.error);
      current = checked && model->IsCurrent(pending_->origin());
      if (!current && !out.error) {
        out.error = Error(std::errc::operation_canceled);
      }
    }
    if (!current) {
      if (!native_owner) { model->DiscardUnpublished(pending_, this); }
      return out;
    }
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
  if (!mutations.empty() || !plan->pictures.empty() || plan->native_artifacts) {
    if (!store_) {
      try {
        if (!plan->pictures.empty() || plan->native_artifacts) {
          using Kind = filesystem::ProfileArtifactKind;
          using Action = filesystem::ProfileArtifactAction;
          std::vector<filesystem::ProfileArtifactExpected> expected;
          std::vector<filesystem::ProfileArtifactMutation> edits;
          for (const auto& [title, file] : plan->files) {
            // Keep one genuine GPD member even for picture-only edits; all
            // other unchanged models remain final guard dependencies.
            if (file.action == ProfileGpdFileAction::kUnchanged && title != kDashboardID) { continue; }
            expected.push_back({Kind::kGpd, title, file.before.token});
            Action action = Action::kObserve;
            switch (file.action) {
              case ProfileGpdFileAction::kReplace: action = Action::kReplace; break;
              case ProfileGpdFileAction::kCreate: action = Action::kCreate; break;
              case ProfileGpdFileAction::kDelete: action = Action::kDelete; break;
              case ProfileGpdFileAction::kUnchanged: break;
            }
            edits.push_back({Kind::kGpd, title, action, file.intended});
          }
          static constexpr std::array<const char*, 7> names{
              "tile_64.png", "tile_32.png", "pp_64.png", "pp_32.png",
              "avtr_64.png", "avtr_32.png", "__thumbnail.png"};
          bool thumbnail = false;
          for (const auto& edit : plan->pictures) {
            const auto index = uint64_t(edit.kind) - uint64_t(Kind::kPublic64);
            if (index >= names.size() || !pending_->pictures_before()) {
              out.error = Error(std::errc::invalid_argument); break;
            }
            if (edit.kind == Kind::kThumbnail) { thumbnail = true; }
            else { expected.push_back({edit.kind, 0, pending_->pictures_before()->at(names[size_t(index)])}); }
            edits.push_back(edit);
          }
          if (!out.error && plan->native_artifacts) {
            // An early participant may include an unchanged GPD specifically
            // for this request. Preserve it as an observed dependency; never
            // drop a changed output simply because it was not admitted.
            std::vector<filesystem::ProfileArtifactMutation> joined;
            for (const auto& member : artifact_admission_->expected()) {
              if (member.kind == Kind::kPackageHeader) {
                if (!plan->package_header) { out.error = Error(std::errc::invalid_argument); break; }
                joined.push_back(*plan->package_header); continue;
              }
              const auto edit = std::find_if(edits.begin(), edits.end(), [&](const auto& value) {
                return value.kind == member.kind && value.title_id == member.title_id;
              });
              if (member.kind == Kind::kGpd) {
                const auto file = plan->files.find(member.title_id);
                if (file == plan->files.end() || file->second.before.token != member.token) {
                  out.error = Error(std::errc::invalid_argument); break;
                }
                if (edit == edits.end()) {
                  if (file->second.action != ProfileGpdFileAction::kUnchanged) {
                    out.error = Error(std::errc::invalid_argument); break;
                  }
                  joined.push_back({Kind::kGpd, member.title_id, Action::kObserve, {}});
                } else joined.push_back(*edit);
              } else {
                const auto index = uint64_t(member.kind) - uint64_t(Kind::kPublic64);
                if (index >= names.size() || !pending_->pictures_before() ||
                    pending_->pictures_before()->at(names[size_t(index)]) != member.token) {
                  out.error = Error(std::errc::invalid_argument); break;
                }
                joined.push_back(edit == edits.end()
                    ? filesystem::ProfileArtifactMutation{member.kind, 0, Action::kObserve, {}} : *edit);
              }
            }
            for (const auto& edit : edits) {
              if (std::none_of(joined.begin(), joined.end(), [&](const auto& value) {
                    return value.kind == edit.kind && value.title_id == edit.title_id;
                  })) { out.error = Error(std::errc::invalid_argument); break; }
            }
            if (!out.error) edits = std::move(joined);
          } else if (!out.error && !artifact_admission_) {
            artifact_admission_ = GpdDurableIntentStore::AcquireArtifacts(
                owner_, recovery_, metadata_, Binding(*lease, *owner_), std::move(expected), thumbnail, out.error);
          }
          if (!out.error && artifact_admission_) {
            store_ = GpdDurableIntentStore::PrepareArtifacts(artifact_admission_, std::move(edits),
                out.error, attempted_journal_, original_tokens);
          }
        } else {
          store_ = GpdDurableIntentStore::Prepare(
              owner_, recovery_, Binding(*lease, *owner_), std::move(mutations),
              out.error, attempted_journal_, original_tokens);
        }
      } catch (...) {
        preparation_uncertain_ = !attempted_journal_.empty();
        out.error = Error(std::errc::io_error);
        out.attempted_journal = attempted_journal_;
        if (preparation_uncertain_) {
          out.state = State::kPreparationUncertain;
        } else {
          if (!native_owner) { model->DiscardUnpublished(pending_, this); }
        }
        return out;
      }
      out.attempted_journal = attempted_journal_;
      if (!store_) {
        preparation_uncertain_ = !attempted_journal_.empty();
        if (preparation_uncertain_) {
          out.state = State::kPreparationUncertain;
        } else {
          if (!native_owner) { model->DiscardUnpublished(pending_, this); }
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
  const auto* members = artifact_admission_ ? artifact_admission_->members().get() : nullptr;
  auto current = LoadCurrentModels(lease, dependencies.get(), out.error, members);
  if (!current) {
    return out;
  }
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      tokens;
  auto pictures = CaptureProfilePictures(lease->view(), out.error, members);
  if (!pictures) { return out; }
  auto prepared_model =
      model->PrepareCommitImpl(pending_, std::move(*current), out.error, std::move(pictures), native_owner);
  if (!prepared_model) {
    if (!store_ && attempted_journal_.empty()) {
      if (!native_owner) { model->DiscardUnpublished(pending_, this); }
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
    auto checked = members ? members->Validate(std::move(tokens), out.error)
                           : lease->view()->ValidateSnapshotBatch(std::move(tokens), out.error);
    if (!checked) {
      return out;
    }
    // External Header is outside the profile-only token batch. Keep the SAME
    // CM participant/receipt under this actual final publication guard, before
    // acquiring model/GCRC cache guards and their nonfallible Apply boundary.
    if (store_ && (out.error = store_->CheckPublishedArtifactsWithSnapshotGuard(*checked))) return out;
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
  // Both model and namespace guards have dropped before actual target/CM
  // admissions release; exact retries now use the retained committed receipt.
  if (artifact_admission_) { store_.reset(); artifact_admission_.reset(); }
  return out;
}
std::error_code ProfileGpdTransaction::CancelNativeUnpublished(
    const std::shared_ptr<const ::xe::kernel::NativeProfileSavePublication>& owner) {
  if (!pending_->NativeOwnedBy(owner)) { return Error(std::errc::operation_canceled); }
  std::unique_lock serial(mutex_, std::try_to_lock);
  if (!serial.owns_lock()) { return Error(std::errc::device_or_resource_busy); }
  if (store_ || !attempted_journal_.empty() || preparation_uncertain_ || committed_) {
    return Error(std::errc::device_or_resource_busy);
  }
  const auto model = model_.lock();
  auto error = model ? model->CancelUnpublishedNative(pending_,owner,this)
                     : Error(std::errc::operation_canceled);
  // PrepareArtifacts may have admitted the real members but failed before any
  // journal attempt. Release that exact admission only after model cleanup,
  // outside its lock; never leave a successful cancel blocking other writers.
  if (!error) { artifact_admission_.reset(); }
  return error;
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
  artifact_admission_.reset();
  return {};
}
std::error_code ProfileGpdTransaction::ReplayBeforeLoad(
    std::shared_ptr<ProfileStorageLease> lease,
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery,
    std::shared_ptr<filesystem::ContentMutationOwner> metadata) {
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
                                                binding, lease->view(), error, metadata);
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
