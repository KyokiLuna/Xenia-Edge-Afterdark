/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/profile_intent_owner.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include "xenia/base/platform.h"
namespace xe::filesystem {
namespace {
std::string Uuid(const ManagedDeviceIdentity::Uuid& id) {
  constexpr char h[] = "0123456789abcdef";
  std::string out;
  for (auto v : id) {
    out += h[v >> 4];
    out += h[v & 15];
  }
  return out;
}
std::string Marker(const ManagedDeviceIdentity& id) {
  return "Xenia Edge profile "
         "intents\nversion=3\npurpose=paired-gpd-redo\nuuid=" +
         Uuid(id.uuid()) + "\nepoch=" + std::to_string(id.content_epoch()) +
         "\ndirectory=" + std::to_string(id.directory_file_id()) +
         "\nsource=" + id.source_id() +
         "\ndevice=" + std::to_string(id.device_id()) + "\n";
}
}  // namespace
std::unique_ptr<ProfileIntentOwner> ProfileIntentOwner::Open(
    std::shared_ptr<const HostStorageRoot> private_parent,
    std::shared_ptr<const ManagedDeviceIdentity> identity) {
  return OpenInternal(std::move(private_parent), std::move(identity), true);
}
std::unique_ptr<ProfileIntentOwner> ProfileIntentOwner::OpenExisting(
    std::shared_ptr<const HostStorageRoot> private_parent,
    std::shared_ptr<const ManagedDeviceIdentity> identity) {
  return OpenInternal(std::move(private_parent), std::move(identity), false);
}
std::unique_ptr<ProfileIntentOwner> ProfileIntentOwner::OpenInternal(
    std::shared_ptr<const HostStorageRoot> private_parent,
    std::shared_ptr<const ManagedDeviceIdentity> identity, bool create,
    std::shared_ptr<const HostStorageIntentLease> source_lease) {
  auto result = std::unique_ptr<ProfileIntentOwner>(new ProfileIntentOwner);
  result->private_parent_ = std::move(private_parent);
  result->managed_identity_ = std::move(identity);
  result->source_ =
      result->managed_identity_ ? result->managed_identity_->source() : nullptr;
  auto& error = result->error_;
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  error = std::make_error_code(std::errc::operation_not_supported);
#else
  if (!result->private_parent_ || !result->source_ ||
      result->private_parent_->read_only() ||
      result->private_parent_->is_guest_private() ||
      result->source_->read_only() || result->source_->is_guest_private()) {
    error = std::make_error_code(std::errc::permission_denied);
    return result;
  }
  error = result->managed_identity_->CheckCurrent();
  if (error) {
    return result;
  }
  // Canonical owned coordinates can reject the known inside-source topology
  // before creating a private leaf. Physical overlap is still checked below.
  const auto relative_parent =
      result->private_parent_->path().lexically_relative(
          result->source_->path());
  if (!relative_parent.empty() && !relative_parent.is_absolute() &&
      *relative_parent.begin() != "..") {
    error = result->private_parent_->PrivateExcludedFrom(*result->source_);
    if (error) { return result; }
  }
  result->source_identity_ = result->source_->IntentRootIdentity(error);
  if (error) {
    return result;
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  // The separate journal parent must have real private-namespace capability
  // before creating any leaf; an unsupported target must stay untouched.
  const auto parent_identity = result->private_parent_->IntentRootIdentity(error);
  if (error) {
    return result;
  }
  if (parent_identity.filesystem != result->source_identity_.filesystem) {
    error = std::make_error_code(std::errc::cross_device_link);
    return result;
  }
#endif
  // Stable UUID+format epoch survive ordinary OS mount generation changes.
  // Copied/rebound roots are rejected by the actual managed marker producer.
  const auto directory =
      std::string(kDirectoryPrefix) + Uuid(result->managed_identity_->uuid()) +
      "-" + std::to_string(result->managed_identity_->content_epoch());
  // One live authority for this actual managed source, even if another
  // emulator incorrectly supplies a different external journal parent.
  if (source_lease) {
    if (!result->source_->IntentLeaseMatches(*source_lease)) {
      error = std::make_error_code(std::errc::permission_denied);
      return result;
    }
    result->source_lease_ = std::move(source_lease);
  } else {
    result->source_lease_ = result->source_->AcquireIntentLease(error, true);
  }
  if (!result->source_lease_) {
    return result;
  }
  const auto marker_wire = Marker(*result->managed_identity_);
  const auto marker_bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(marker_wire.data()), marker_wire.size());
  bool created = false;
  auto raw = result->private_parent_->Subroot(directory, false, error);
  if (!raw && error == std::errc::no_such_file_or_directory && create) {
    error = result->private_parent_->Create(directory, true);
    created = !error;
    if (error && error != std::errc::file_exists) {
      return result;
    }
    raw = result->private_parent_->Subroot(directory, false, error);
  }
  if (!raw) {
    return result;
  }
  // Overlap is permitted only when the exact trusted device projection
  // excludes this actual retained private container from every guest view.
  error = raw->IntentDisjointFrom(*result->source_);
  if (error) {
    return result;
  }
  result->root_ = raw->ProtectProfileIntents(error);
  if (!result->root_) {
    return result;
  }
  result->lease_ = result->root_->AcquireIntentLease(error);
  if (!result->lease_) {
    return result;
  }
  if (created) {
    error = result->private_parent_->FlushDirectory();
    if (error) {
      return result;
    }
    const auto write = result->root_->WriteNewFile("owner.v1", marker_bytes);
    error = write.error;
    if (!error && write.bytes != marker_bytes.size()) {
      error = std::make_error_code(std::errc::io_error);
    }
    if (error) {
      return result;
    }
  }
  const auto marker = result->root_->CaptureSnapshot("owner.v1", 1024, error);
  if (!error && (!marker->exists() ||
                 !std::equal(marker->bytes().begin(), marker->bytes().end(),
                             marker_bytes.begin(), marker_bytes.end()))) {
    error = std::make_error_code(std::errc::bad_message);
  }
  // Valid-grammar corrupt/partial state retains both protection and lease.
  // The caller must fail bootstrap, rather than enabling a guest fallback.
#endif
  return result;
}
std::vector<std::filesystem::path> ProfileIntentOwner::List(
    std::error_code& error) const {
  if (error_) {
    error = error_;
    return {};
  }
  return root_->ListIntentFiles(error);
}
HostStorageDurableObservation ProfileIntentOwner::Observe(
    const HostStorageSnapshotToken& token) const {
  if (error_) {
    return {error_};
  }
  return token.root()->ObserveIntentSnapshot(token);
}
std::shared_ptr<const HostStorageIntentLease> ProfileIntentOwner::AcquireTarget(
    const HostStorageRoot& root, std::error_code& error) const {
  if (error_) {
    error = error_;
    return {};
  }
  const auto relative = root.path().lexically_relative(source_->path());
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
    error = std::make_error_code(std::errc::permission_denied);
    return {};
  }
  return root.AcquireIntentLease(error, true);
}
HostStorageRootIdentity ProfileIntentOwner::Identity(
    const HostStorageRoot& root, std::error_code& error) const {
  if (error_) {
    error = error_;
    return {};
  }
  return root.IntentRootIdentity(error);
}
}  // namespace xe::filesystem
