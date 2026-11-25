/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/snapshot_publisher.h"
#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <string_view>
#include "xenia/base/platform.h"
#include "xenia/base/mutex.h"
#include "xenia/base/snapshot_io.h"
namespace xe::filesystem {
namespace {
constexpr std::string_view kOwnership =
    "Xenia Edge recovery snapshots\nversion=1\npurpose=single-file-cas\n";
auto OwnershipBytes() {
  return std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(kOwnership.data()), kOwnership.size());
}
std::string UniqueLeaf(std::string_view prefix, std::string_view suffix) {
  constexpr char hex[] = "0123456789abcdef";
  std::random_device random;
  std::string result(prefix);
  for (unsigned i = 0; i != 4; ++i) {
    const uint32_t value = random();
    for (unsigned j = 0; j != 8; ++j) {
      result.push_back(hex[(value >> (j * 4)) & 15]);
    }
  }
  result.append(suffix);
  return result;
}
bool SameBytes(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
std::unique_ptr<HostStorageSnapshotStage> NewStage(
    const std::shared_ptr<const HostStorageRoot>& root,
    const std::filesystem::path& parent, bool recovery,
    std::filesystem::path& leaf, bool& created, std::error_code& error) {
  for (unsigned i = 0; i != 8; ++i) {
    leaf = parent / UniqueLeaf(recovery ? "snapshot-" : ".xenia-stage-",
                               recovery ? ".bin" : ".tmp");
    auto stage = root->CreateSnapshotStage(leaf, created, error);
    if (stage || created || error != std::errc::file_exists) { return stage; }
  }
  return {};
}
}  // namespace
std::unique_ptr<SnapshotRecoveryOwner> SnapshotRecoveryOwner::Open(
    std::shared_ptr<const HostStorageRoot> parent) {
  return OpenInternal(std::move(parent), true);
}
std::unique_ptr<SnapshotRecoveryOwner> SnapshotRecoveryOwner::OpenExisting(
    std::shared_ptr<const HostStorageRoot> parent) {
  return OpenInternal(std::move(parent), false);
}
std::unique_ptr<SnapshotRecoveryOwner> SnapshotRecoveryOwner::OpenInternal(
    std::shared_ptr<const HostStorageRoot> parent, bool create,
    unsigned stop_boundary, HostStorageBootstrapDirectory* exact_completed,
    std::shared_ptr<const HostStorageIntentLease> same_parent_lease) {
  auto result = std::unique_ptr<SnapshotRecoveryOwner>(new SnapshotRecoveryOwner);
  auto& error = result->error_;
  if (global_critical_region::is_held_by_current_thread()) {
    error=std::make_error_code(std::errc::operation_not_permitted);return result;
  }
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  static_cast<void>(parent);static_cast<void>(create);static_cast<void>(stop_boundary);
  static_cast<void>(exact_completed);static_cast<void>(same_parent_lease);
  error = std::make_error_code(std::errc::operation_not_supported);return result;
#else
  if (!parent || parent->read_only() || parent->is_guest_private()) {
    error = std::make_error_code(std::errc::permission_denied);return result;
  }
  // A private coordinator may share only the already-held EX description
  // belonging to this exact retained parent. A path-equivalent or foreign
  // lease cannot authorize any recovery lookup, creation or receipt reclaim.
  if (same_parent_lease && !parent->IntentLeaseMatches(*same_parent_lease)) {
    error = std::make_error_code(std::errc::permission_denied);return result;
  }
  std::shared_ptr<HostStorageRoot> raw;
  if (exact_completed) {
    // Only the existing private coordinator can hand off a live capsule. Do
    // not reselect/reclaim its active receipt through an ordinary path open.
    // A foreign/staged capsule is refused before any publication attempt.
    if (create || stop_boundary || exact_completed->target()!=kDirectory ||
        !exact_completed->root() ||
        exact_completed->root()->path()!=parent->path()/kDirectory) {
      error=std::make_error_code(std::errc::permission_denied);return result;
    }
    const auto observed=parent->PublishBootstrapDirectory(*exact_completed);
    error=observed.error;raw=observed.canonical_root;
    if (!raw || error || !observed.durable) {
      if (!error) {error=std::make_error_code(std::errc::state_not_recoverable);}
      return result;
    }
  } else {
    raw=parent->Subroot(kDirectory,false,error);
  }
  if (!raw && error == std::errc::no_such_file_or_directory && create) {
    // This lower native bootstrap has no dependency on MVA or a ready recovery
    // context. Unknown prior stages are preserved, never selected by prefix.
    const auto nonce=UniqueLeaf("","");
    result->bootstrap_=parent->BeginBootstrapDirectory(kDirectory,nonce,error,same_parent_lease);
    if (!result->bootstrap_ || error) { return result; }
    if (stop_boundary==1) { error=std::make_error_code(std::errc::operation_canceled);return result; }
    const auto marker=result->bootstrap_->root()->PublishBootstrapRecord(kMarker,OwnershipBytes(),nonce);
    error=marker.error;if (error) { return result; }
    if (stop_boundary==2) { error=std::make_error_code(std::errc::operation_canceled);return result; }
    if (stop_boundary==3) { parent->bootstrap_test_boundary_.store(3); }
    const auto published=parent->PublishBootstrapDirectory(*result->bootstrap_);
    error=published.error;result->created_=published.published;raw=published.canonical_root;
    if (!raw) { return result; }
    // Deny-only native fixture: the complete canonical directory and its real
    // barriers exist, but no typed protection has yet replaced the capsule.
    if (!error && stop_boundary==4) {
      error=std::make_error_code(std::errc::operation_canceled);return result;
    }
    // Refine the SAME native directory while bootstrap protection remains
    // retained. On uncertain publication the typed protected root is returned
    // with its error, so no failure owner exposes a private canonical tree.
    std::error_code protection_error;
    result->root_=raw->ProtectRecovery(protection_error);
    if (!result->root_) { if (!error) { error=protection_error; } return result; }
    if (error) { return result; }
  } else if (!raw) { return result; }
  if (!result->root_) {
    const auto marker=raw->CaptureSnapshot(kMarker,256,error);
    if (!marker || !marker->exists() || !SameBytes(marker->bytes(),OwnershipBytes())) {
      if (!error) { error=std::make_error_code(std::errc::bad_message); }return result;
    }
    // A live unrelated guest can retain this initializer's old registered
    // protector after its capsule disappears. Reclaim only its exact native
    // receipt under a newly acquired parent EX lease, after the complete
    // canonical owner marker above has been validated. No stage prefix scan.
    if (!exact_completed) {
      result->bootstrap_=parent->ResumeRetainedBootstrapDirectory(kDirectory,*raw,error,same_parent_lease);
      if (error) { return result; }
    }
    if (result->bootstrap_) {
      const auto observed=parent->PublishBootstrapDirectory(*result->bootstrap_);
      error=observed.error;raw=observed.canonical_root;
      if (!raw || error) { return result; }
    }
    result->root_=raw->ProtectRecovery(error);if (!result->root_) { return result; }
  }
  const auto marker=result->root_->CaptureSnapshot(kMarker,256,error);
  if (!marker || !marker->exists() || !SameBytes(marker->bytes(),OwnershipBytes())) {
    if (!error) { error=std::make_error_code(std::errc::bad_message); }return result;
  }
  const auto observed=result->root_->ObserveIntentSnapshot(*marker);
  error=observed.error;
  if (!error && !observed.durable) { error=std::make_error_code(std::errc::io_error); }
  if (!error) { error=parent->FlushDirectory(); }
  if (!error) { result->bootstrap_.reset(); }
  return result;
#endif
}
std::error_code SnapshotRecoveryOwner::CheckTarget(const HostStorageRoot& target) const {
  if (error_ || !root_) {
    return error_ ? error_ : std::make_error_code(std::errc::no_such_device);
  }
  std::error_code error;
  const auto source = target.IntentRootIdentity(error);
  if (error) { return error; }
  const auto destination = root_->IntentRootIdentity(error);
  if (error) { return error; }
  return source.filesystem == destination.filesystem
      ? std::error_code{} : std::make_error_code(std::errc::cross_device_link);
}
std::unique_ptr<PreparedSnapshotPublication> SnapshotPublisher::Prepare(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> intended, const std::atomic<bool>* canceled) {
  return PrepareInternal(owner, std::move(expected), intended, canceled, {});
}
std::unique_ptr<PreparedSnapshotPublication> SnapshotPublisher::PrepareInternal(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> intended, const std::atomic<bool>* canceled,
    std::string_view exact_nonce, std::error_code post_stage_parent_error,
    PrivateSnapshotPurpose purpose,
    std::unique_ptr<HostStorageSnapshotStage> retained_stage) {
  auto prepared = std::make_unique<PreparedSnapshotPublication>();
  auto& receipt = prepared->receipt_;
  receipt.expected = expected;
  if (intended.size() > (128ull << 20)) {
    receipt.error = std::make_error_code(std::errc::file_too_large);
    return prepared;
  }
  // Snapshot the submission before any filesystem mutation. Caller retains
  // its ordinary span lifetime during this copy; subsequent work owns bytes.
  prepared->intended_.assign(intended.begin(), intended.end());
  intended = prepared->intended_;
  auto stop = [&]() {
    if (canceled && canceled->load(std::memory_order_acquire)) {
      receipt.error = std::make_error_code(std::errc::operation_canceled);
      return true;
    }
    return false;
  };
  if (owner.error() || !owner.root() || !expected ||
      expected->root()->read_only() ||
      !expected->root()->IsPrivateSnapshotTarget(expected->relative(),purpose)) {
    receipt.error = owner.error() ? owner.error()
        : std::make_error_code(std::errc::permission_denied);
    return prepared;
  }
  prepared->purpose_ = purpose;
  if (stop()) { return prepared; }
  const auto& root = expected->root();
  receipt.error = owner.CheckTarget(*root);
  if (receipt.error) { return prepared; }
  receipt.phase = SnapshotPublicationPhase::kStageCreate;
  std::unique_ptr<HostStorageSnapshotStage> stage;
  if (retained_stage) {
    // Issued CM session has already closed mutation and supplies this exact
    // owned native stage. Verify its actual retained bytes before preparing
    // the old-byte backup. CAS still independently verifies them at commit.
    stage = std::move(retained_stage);
    receipt.stage = stage->relative();
    receipt.stage_created = true;
    if (stage->root_ != root || stage->relative().parent_path() !=
        expected->relative().parent_path()) {
      receipt.error = std::make_error_code(std::errc::permission_denied);
      prepared->stage_ = std::move(stage); return prepared;
    }
    receipt.phase = SnapshotPublicationPhase::kStageWrite;
    const auto info = stage->QueryInfo(receipt.error);
    if (!info || !info->regular || info->links != 1 ||
        info->size != intended.size()) {
      if (!receipt.error) { receipt.error = std::make_error_code(std::errc::state_not_recoverable); }
      prepared->stage_ = std::move(stage); return prepared;
    }
    std::array<uint8_t, 4096> scratch;
    for (size_t offset = 0; offset < intended.size();) {
      const auto count = std::min(scratch.size(), intended.size() - offset);
      const auto read = stage->ReadResult(offset, std::span(scratch).first(count));
      if (read.error || read.bytes != count || !SameBytes(
          std::span<const uint8_t>(scratch).first(count), intended.subspan(offset, count))) {
        receipt.error = read.error ? read.error : std::make_error_code(std::errc::state_not_recoverable);
        prepared->stage_ = std::move(stage); return prepared;
      }
      offset += count;
    }
    receipt.staged_count = intended.size();
  } else {
    if (exact_nonce.empty()) {
      stage = NewStage(root, expected->relative().parent_path(), false,
                       receipt.stage, receipt.stage_created, receipt.error);
    } else {
      receipt.stage = expected->relative().parent_path() /
          (".xenia-stage-" + std::string(exact_nonce) + ".tmp");
      stage = root->CreateSnapshotStage(receipt.stage, receipt.stage_created, receipt.error);
    }
    if (!stage) { return prepared; }
    receipt.phase = SnapshotPublicationPhase::kStageWrite;
    const auto staged = WriteSnapshotBytes(*stage, intended, canceled);
    receipt.error = staged.error;
    receipt.staged_count = staged.bytes;
  }
  if (receipt.error || stop()) { prepared->stage_ = std::move(stage); return prepared; }
  receipt.phase = SnapshotPublicationPhase::kStageFlush;
  receipt.error = stage->FlushResult();
  // The durable journal records this exact stage name and native identity.
  // File fsync alone cannot preserve its directory entry across power loss.
  // Do not expose a prepared stage until both real barriers have succeeded.
  if (!receipt.error) {
    receipt.error = root->FlushDirectory(receipt.stage.parent_path());
  }
  // A private fault seam can only make a real successful barrier uncertain;
  // it cannot skip native I/O or manufacture durable preparation.
  if (!receipt.error) { receipt.error = post_stage_parent_error; }
  if (receipt.error || stop()) { return prepared; }
  std::shared_ptr<const HostStorageSnapshotToken> recovery;
  if (expected->exists()) {
    receipt.phase = SnapshotPublicationPhase::kRecoveryCreate;
    std::unique_ptr<HostStorageSnapshotStage> copy;
    if (exact_nonce.empty()) {
      copy = NewStage(owner.root(), {}, true, receipt.recovery,
                       receipt.recovery_created, receipt.error);
    } else {
      receipt.recovery = "snapshot-" + std::string(exact_nonce) + ".bin";
      copy = owner.root()->CreateSnapshotStage(receipt.recovery,
          receipt.recovery_created, receipt.error);
    }
    if (!copy) { return prepared; }
    receipt.phase = SnapshotPublicationPhase::kRecoveryWrite;
    const auto saved = WriteSnapshotBytes(*copy, expected->bytes(), canceled);
    receipt.error = saved.error;
    receipt.recovery_count = saved.bytes;
    if (receipt.error || stop()) { return prepared; }
    receipt.phase = SnapshotPublicationPhase::kRecoveryFlush;
    receipt.error = copy->FlushResult();
    if (!receipt.error) { receipt.error = owner.root()->FlushDirectory(); }
    if (receipt.error) { return prepared; }
    receipt.recovery_durable = true;
    copy.reset();  // Drop the exclusive handle before taking the load token.
    receipt.phase = SnapshotPublicationPhase::kRecoveryVerify;
    recovery = owner.root()->CaptureSnapshot(receipt.recovery,
                                             expected->bytes().size(),
                                             receipt.error);
    if (!receipt.error && (!recovery->exists() ||
        !SameBytes(recovery->bytes(), expected->bytes()))) {
      receipt.error = std::make_error_code(std::errc::state_not_recoverable);
    }
    if (receipt.error || stop()) { return prepared; }
  }
  const auto stage_info = stage->QueryInfo(receipt.error);
  if (!stage_info || receipt.error) {
    if (!receipt.error) { receipt.error = std::make_error_code(std::errc::io_error); }
    return prepared;
  }
  prepared->stage_info_ = *stage_info;
  prepared->stage_ = std::move(stage);
  prepared->recovery_ = std::move(recovery);
  return prepared;
}
SnapshotPublicationReceipt SnapshotPublisher::Commit(
    PreparedSnapshotPublication& prepared, const std::atomic<bool>* canceled) {
  auto& receipt = prepared.receipt_;
  if (prepared.consumed_) {
    auto refused = receipt;
    refused.error = std::make_error_code(std::errc::operation_not_permitted);
    return refused;
  }
  if (receipt.error) { return receipt; }
  if (!prepared.stage_ || !receipt.expected) {
    receipt.error = std::make_error_code(std::errc::invalid_argument);
    return receipt;
  }
  if (canceled && canceled->load(std::memory_order_acquire)) {
    receipt.error = std::make_error_code(std::errc::operation_canceled);
    return receipt;
  }
  prepared.consumed_ = true;
  receipt.phase = SnapshotPublicationPhase::kCompareAndPublish;
  const auto result = receipt.expected->root()->CompareAndPublishSnapshotInternal(
      *receipt.expected, *prepared.stage_, prepared.intended_, prepared.recovery_,
      prepared.purpose_);
  receipt.error = result.error;
  receipt.published = result.published;
  receipt.durable = result.durable;
  receipt.current = result.current;
  if (!receipt.error) { receipt.phase = SnapshotPublicationPhase::kComplete; }
  return receipt;
}
SnapshotPublicationReceipt SnapshotPublisher::Publish(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> intended, const std::atomic<bool>* canceled) {
  auto prepared = Prepare(owner, std::move(expected), intended, canceled);
  return Commit(*prepared, canceled);
}
SnapshotPublicationReceipt SnapshotPublisher::PublishMediaReference(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> intended, const std::atomic<bool>* canceled) {
  auto prepared = PrepareInternal(owner, std::move(expected), intended, canceled,
                                  {}, {}, PrivateSnapshotPurpose::kMediaReference);
  return Commit(*prepared, canceled);
}
std::unique_ptr<PreparedSnapshotPublication> SnapshotPublisher::PrepareCacheTable(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> intended,const std::atomic<bool>* canceled) {
  return PrepareInternal(owner,std::move(expected),intended,canceled,
      {},{},PrivateSnapshotPurpose::kCacheTable);
}
SnapshotPublicationReceipt SnapshotPublisher::DeleteCacheTable(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,const std::atomic<bool>* canceled) {
  return DeleteInternal(owner,std::move(expected),canceled,PrivateSnapshotPurpose::kCacheTable);
}
SnapshotPublicationReceipt SnapshotPublisher::PublishCacheTable(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> intended, const std::atomic<bool>* canceled) {
  auto prepared=PrepareInternal(owner,std::move(expected),intended,canceled,
      {},{},PrivateSnapshotPurpose::kCacheTable);
  return Commit(*prepared,canceled);
}
SnapshotPublicationReceipt SnapshotPublisher::Delete(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    const std::atomic<bool>* canceled) {
  return DeleteInternal(owner, std::move(expected), canceled, PrivateSnapshotPurpose::kOrdinary);
}
SnapshotPublicationReceipt SnapshotPublisher::DeleteInternal(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    const std::atomic<bool>* canceled, PrivateSnapshotPurpose purpose) {
  SnapshotPublicationReceipt receipt;
  receipt.kind = SnapshotMutationKind::kDelete;
  receipt.expected = expected;
  if (owner.error() || !owner.root() || !expected || !expected->exists() ||
      expected->relative().has_parent_path() || expected->root()->read_only() ||
      !expected->root()->IsPrivateSnapshotTarget(expected->relative(),purpose)) {
    receipt.error = owner.error()
                        ? owner.error()
                        : std::make_error_code(std::errc::invalid_argument);
    return receipt;
  }
  receipt.error = owner.CheckTarget(*expected->root());
  if (receipt.error) { return receipt; }
  if (expected->bytes().size() > (128ull << 20)) {
    receipt.error = std::make_error_code(std::errc::file_too_large);
    return receipt;
  }
  auto stop = [&]() {
    if (canceled && canceled->load(std::memory_order_acquire)) {
      receipt.error = std::make_error_code(std::errc::operation_canceled);
      return true;
    }
    return false;
  };
  if (stop()) {
    return receipt;
  }
  receipt.phase = SnapshotPublicationPhase::kRecoveryCreate;
  auto copy = NewStage(owner.root(), {}, true, receipt.recovery,
                       receipt.recovery_created, receipt.error);
  if (!copy) {
    return receipt;
  }
  receipt.phase = SnapshotPublicationPhase::kRecoveryWrite;
  const auto saved = WriteSnapshotBytes(*copy, expected->bytes(), canceled);
  receipt.error = saved.error;
  receipt.recovery_count = saved.bytes;
  if (receipt.error || stop()) {
    return receipt;
  }
  receipt.phase = SnapshotPublicationPhase::kRecoveryFlush;
  receipt.error = copy->FlushResult();
  if (!receipt.error) {
    receipt.error = owner.root()->FlushDirectory();
  }
  if (receipt.error) {
    return receipt;
  }
  receipt.recovery_durable = true;
  copy.reset();
  receipt.phase = SnapshotPublicationPhase::kRecoveryVerify;
  auto recovery = owner.root()->CaptureSnapshot(
      receipt.recovery, expected->bytes().size(), receipt.error);
  if (!receipt.error && (!recovery->exists() ||
                         !SameBytes(recovery->bytes(), expected->bytes()))) {
    receipt.error = std::make_error_code(std::errc::state_not_recoverable);
  }
  if (receipt.error || stop()) {
    return receipt;
  }
  receipt.phase = SnapshotPublicationPhase::kCompareAndDelete;
  const auto result =
      expected->root()->CompareAndDeleteSnapshotInternal(
          *expected, recovery, {}, purpose);
  receipt.error = result.error;
  receipt.published = result.published;
  receipt.durable = result.durable;
  receipt.current = result.current;
  if (!receipt.error) {
    receipt.phase = SnapshotPublicationPhase::kComplete;
  }
  return receipt;
}
}  // namespace xe::filesystem
