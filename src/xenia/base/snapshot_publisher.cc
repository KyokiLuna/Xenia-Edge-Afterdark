/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/snapshot_publisher.h"
#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <string_view>
#include "xenia/base/platform.h"
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
    std::shared_ptr<const HostStorageRoot> parent, bool create) {
  auto result = std::unique_ptr<SnapshotRecoveryOwner>(new SnapshotRecoveryOwner);
  auto& error = result->error_;
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  static_cast<void>(parent);
  error = std::make_error_code(std::errc::operation_not_supported);
  return result;
#else
  if (!parent || parent->read_only() || parent->is_guest_private()) {
    error = std::make_error_code(std::errc::permission_denied);
    return result;
  }
#if XE_PLATFORM_MAC
  // Prove capability before creating a private leaf. An unsupported SDK or
  // physical namespace must not leave a partial directory for later adoption.
  static_cast<void>(parent->IntentRootIdentity(error));
  if (error) { return result; }
#endif
  auto raw = parent->Subroot(kDirectory, false, error);
  if (!raw && error == std::errc::no_such_file_or_directory && create) {
    error = parent->Create(kDirectory, true);
    result->created_ = !error;
    if (error && error != std::errc::file_exists) { return result; }
    raw = parent->Subroot(kDirectory, false, error);
  }
  if (!raw) { return result; }
  if (!result->created_) {
    // Never adopt an empty/partial/foreign directory. A portable marker is
    // ownership/version metadata, not a host secret or an inode substitute.
    auto marker = raw->CaptureSnapshot(kMarker, 256, error);
    if (error) { return result; }
    if (!marker->exists() || !SameBytes(marker->bytes(), OwnershipBytes())) {
      error = std::make_error_code(std::errc::bad_message);
      return result;
    }
  }
  result->root_ = raw->ProtectRecovery(error);
  if (!result->root_) { return result; }
  if (result->created_) {
    // Establish guest exclusion BEFORE writing any private payload/marker.
    // Bootstrap failures keep both the directory and protection owner alive.
    error = parent->FlushDirectory();
    if (error) { return result; }
    const auto written = result->root_->WriteNewFile(kMarker, OwnershipBytes());
    error = written.error;
    if (!error && written.bytes != OwnershipBytes().size()) {
      error = std::make_error_code(std::errc::io_error);
    }
    if (error) { return result; }
  }
  auto marker = result->root_->CaptureSnapshot(kMarker, 256, error);
  if (!error && (!marker->exists() ||
                 !SameBytes(marker->bytes(), OwnershipBytes()))) {
    error = std::make_error_code(std::errc::bad_message);
  }
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
    std::string_view exact_nonce, std::error_code post_stage_parent_error) {
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
      expected->root()->read_only() || expected->root()->is_guest_private()) {
    receipt.error = owner.error() ? owner.error()
        : std::make_error_code(std::errc::permission_denied);
    return prepared;
  }
  if (stop()) { return prepared; }
  const auto& root = expected->root();
  receipt.error = owner.CheckTarget(*root);
  if (receipt.error) { return prepared; }
  receipt.phase = SnapshotPublicationPhase::kStageCreate;
  std::unique_ptr<HostStorageSnapshotStage> stage;
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
  if (receipt.error || stop()) { return prepared; }
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
  const auto result = receipt.expected->root()->CompareAndPublishSnapshot(
      *receipt.expected, *prepared.stage_, prepared.intended_, prepared.recovery_);
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
SnapshotPublicationReceipt SnapshotPublisher::Delete(
    const SnapshotRecoveryOwner& owner,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    const std::atomic<bool>* canceled) {
  SnapshotPublicationReceipt receipt;
  receipt.kind = SnapshotMutationKind::kDelete;
  receipt.expected = expected;
  if (owner.error() || !owner.root() || !expected || !expected->exists() ||
      expected->relative().has_parent_path() || expected->root()->read_only() ||
      expected->root()->is_guest_private()) {
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
      expected->root()->CompareAndDeleteSnapshot(*expected, recovery);
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
