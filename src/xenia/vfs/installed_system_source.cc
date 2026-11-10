/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/vfs/installed_system_source.h"

#include <utility>

#include "xenia/base/filesystem.h"

namespace xe::vfs {
namespace {
std::error_code Error(std::errc value) { return std::make_error_code(value); }
bool Below(const std::filesystem::path& parent,
           const std::filesystem::path& child) {
  auto a = parent.begin(), b = child.begin();
  for (; a != parent.end(); ++a, ++b) {
    if (b == child.end() || *a != *b) {
      return false;
    }
  }
  return true;
}
std::filesystem::path Absolute(const std::filesystem::path& value,
                               std::error_code& error) {
  return std::filesystem::absolute(value, error).lexically_normal();
}
}  // namespace

struct InstalledSystemOperationGate::State {
  mutable std::mutex mutex;
  bool install = false;
  uint32_t launches = 0;
};
InstalledSystemOperationGate::InstalledSystemOperationGate()
    : state_(std::make_shared<State>()) {}
InstalledSystemOperationGate::Reservation::~Reservation() {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  if (install_) {
    state_->install = false;
  } else {
    --state_->launches;
  }
}
std::optional<InstalledSystemOperationGate::Reservation>
InstalledSystemOperationGate::TryInstall() {
  std::lock_guard lock(state_->mutex);
  if (state_->install || state_->launches) {
    return {};
  }
  state_->install = true;
  return Reservation(state_, true);
}
std::optional<InstalledSystemOperationGate::Reservation>
InstalledSystemOperationGate::TryLaunch() {
  std::lock_guard lock(state_->mutex);
  if (state_->install || state_->launches == UINT32_MAX) {
    return {};
  }
  ++state_->launches;
  return Reservation(state_, false);
}
bool InstalledSystemOperationGate::installing() const {
  std::lock_guard lock(state_->mutex);
  return state_->install;
}

InstalledSystemSource::InstalledSystemSource(std::filesystem::path local,
                                             std::filesystem::path shared)
    : local_(std::move(local)), shared_(std::move(shared)) {}

std::shared_ptr<InstalledSystemSource> InstalledSystemSource::Create(
    const std::filesystem::path& local, const std::filesystem::path& shared,
    std::error_code& error) {
  error.clear();
  if (local.empty()) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  auto local_path = Absolute(local, error);
  if (error) {
    return {};
  }
  auto shared_path =
      shared.empty() ? std::filesystem::path{} : Absolute(shared, error);
  if (error) {
    return {};
  }
  if (!shared_path.empty()) {
    const auto canonical_shared =
        std::filesystem::canonical(shared_path, error);
    if (error) {
      return {};
    }
    // The explicit flag names the attested image itself, not a path alias.
    if (canonical_shared != shared_path) {
      error = Error(std::errc::permission_denied);
      return {};
    }
    const auto canonical_local =
        std::filesystem::weakly_canonical(local_path, error);
    if (error) {
      return {};
    }
    if (Below(canonical_local, shared_path) ||
        Below(shared_path, canonical_local)) {
      error = Error(std::errc::invalid_argument);
      return {};  // Installer writes must not overlap the fixed shared image.
    }
  }
  auto result = std::shared_ptr<InstalledSystemSource>(
      new InstalledSystemSource(std::move(local_path), std::move(shared_path)));
  auto initial = result->Build(1, error);
  if (!initial) {
    return {};
  }
  result->current_ = std::move(initial);
  return result;
}

InstalledSystemSource::~InstalledSystemSource() {
  auto old = snapshot();
  if (old && old->root_) {
    old->root_->Retire();
  }
}

std::shared_ptr<const InstalledSystemSource::Snapshot>
InstalledSystemSource::snapshot() const {
  std::lock_guard lock(mutex_);
  return current_;
}

std::shared_ptr<const InstalledSystemSource::Snapshot>
InstalledSystemSource::Build(uint64_t generation, std::error_code& error) {
  error.clear();
  auto value = std::shared_ptr<Snapshot>(new Snapshot);
  value->owner_ = shared_from_this();
  value->generation_ = generation;
  value->shared_ = !shared_.empty();
  value->path_ = value->shared_ ? shared_ : local_;
  value->root_ =
      filesystem::HostStorageRoot::Open(value->path_, true, error, 0);
  if (!value->root_) {
    if (!value->shared_ && error == std::errc::no_such_file_or_directory) {
      error.clear();
      return value;
    }
    return {};
  }
  value->root_ = value->root_->GuestView(error);
  if (!value->root_ || error) {
    return {};
  }
  value->path_ = value->root_->path();
  constexpr const char* folders[] = {"flash", "sysext", "sysaux"};
  for (size_t i = 0; i < value->partitions_.size(); ++i) {
    value->partitions_[i] = value->root_->Subroot(folders[i], true, error);
    if (!value->partitions_[i] &&
        error != std::errc::no_such_file_or_directory) {
      return {};
    }
    error.clear();
  }
  // A shared source is specifically an installed image, not an arbitrary
  // storage root. Optional auxiliary partitions may be absent.
  if (value->shared_ && !value->partitions_[0]) {
    error = Error(std::errc::no_such_file_or_directory);
    return {};
  }
  if (value->partitions_[1]) {
    value->content_ = value->partitions_[1]->Subroot("Content", true, error);
    if (!value->content_ && error != std::errc::no_such_file_or_directory) {
      return {};
    }
    error.clear();
  }
  return value;
}

std::shared_ptr<const InstalledSystemSource::Snapshot>
InstalledSystemSource::PrepareRefresh(std::error_code& error, bool installed) {
  error.clear();
  auto old = snapshot();
  if (!old) {
    error = Error(std::errc::state_not_recoverable);
    return {};
  }
  if (old->shared_) {
    if (old->valid()) {
      return old;
    }
    error = Error(std::errc::no_such_device);
    return {};  // Never reopen an explicitly pinned shared namespace.
  }
  if (!installed && old->root_ && old->valid()) {
    return old;
  }
  if (old->generation_ == UINT64_MAX) {
    error = Error(std::errc::value_too_large);
    return {};
  }
  auto next = Build(old->generation_ + 1, error);
  if (next && !installed && !old->root_ && !next->root_) {
    return old;
  }
  return next;
}

bool InstalledSystemSource::Commit(std::shared_ptr<const Snapshot> next,
                                   std::error_code& error) {
  error.clear();
  if (!next || next->owner_.lock().get() != this || !next->valid()) {
    error = Error(std::errc::invalid_argument);
    return false;
  }
  std::shared_ptr<const Snapshot> old;
  {
    std::lock_guard lock(mutex_);
    if (next == current_) {
      return true;
    }
    if (!current_ || current_->generation_ == UINT64_MAX ||
        next->generation_ != current_->generation_ + 1) {
      error = Error(std::errc::device_or_resource_busy);
      return false;
    }
    old = std::exchange(current_, std::move(next));
  }
  if (old->root_) {
    old->root_->Retire();
  }
  return true;
}

bool InstalledSystemSource::MatchesSelection(
    const std::filesystem::path& selected) const {
  if (selected.empty()) {
    return shared_.empty();
  }
  std::error_code error;
  return !shared_.empty() && Absolute(selected, error) == shared_ && !error;
}

bool InstalledSystemSource::Snapshot::valid() const {
  if (!root_) {
    return !shared_;
  }
  if (!root_->Observe().available) {
    return false;
  }
  for (const auto& part : partitions_) {
    if (part && !part->Observe().available) {
      return false;
    }
  }
  return !content_ || content_->Observe().available;
}
bool InstalledSystemSource::Snapshot::available() const {
  return root_ && valid();
}
std::shared_ptr<filesystem::HostStorageRoot>
InstalledSystemSource::Snapshot::PartitionRoot(Partition part,
                                               std::error_code& error) const {
  error.clear();
  const auto index = size_t(part);
  if (!valid() || index >= partitions_.size()) {
    error = Error(std::errc::no_such_device);
    return {};
  }
  auto result = partitions_[index];
  if (!result) {
    error = Error(std::errc::no_such_file_or_directory);
  }
  return result;
}
std::shared_ptr<filesystem::HostStorageRoot>
InstalledSystemSource::Snapshot::ContentRoot(std::error_code& error) const {
  error.clear();
  if (!valid()) {
    error = Error(std::errc::no_such_device);
    return {};
  }
  if (!content_) {
    error = Error(std::errc::no_such_file_or_directory);
  }
  return content_;
}
bool InstalledSystemSource::Snapshot::Contains(
    const std::filesystem::path& path) const {
  std::error_code error;
  const auto absolute = Absolute(path, error);
  if (error) {
    return false;
  }
  if (Below(path_, absolute)) {
    return true;
  }
  // Classification precedes any writable title-root fallback. A spelling
  // outside the image may still name an installed file through a host alias.
  const auto canonical = std::filesystem::canonical(absolute, error);
  return !error && Below(path_, canonical);
}
std::shared_ptr<filesystem::HostStorageRoot>
InstalledSystemSource::Snapshot::DirectoryRoot(
    const std::filesystem::path& directory, std::error_code& error) const {
  error.clear();
  auto absolute = std::filesystem::canonical(directory, error);
  if (error || !root_ || !valid() || !Below(path_, absolute)) {
    if (!error) {
      error = Error(std::errc::permission_denied);
    }
    return {};
  }
  // Derive from the captured partition, not a fresh path-open through the
  // parent: an external replacement must invalidate this generation.
  for (const auto& partition : partitions_) {
    if (partition && Below(partition->path(), absolute)) {
      // The captured partition itself is a same-root derivation, whose
      // HostStorageRoot spelling is empty; "." remains invalid traversal.
      const auto relative =
          absolute == partition->path()
              ? std::filesystem::path{}
              : absolute.lexically_relative(partition->path());
      return partition->Subroot(relative, true, error);
    }
  }
  if (absolute == path_) {
    return root_->Subroot({}, true, error);
  }
  error = Error(std::errc::permission_denied);
  return {};
}
std::optional<std::filesystem::path>
InstalledSystemSource::Snapshot::DashboardPath(std::error_code& error) const {
  auto flash = PartitionRoot(Partition::kFlash, error);
  if (!flash) {
    return {};
  }
  auto file =
      flash->OpenFile("dash.xex", filesystem::FileAccess::kFileReadData, error);
  if (!file) {
    return {};
  }
  auto info = file->QueryInfo(error);
  if (!info || !info->regular || info->directory) {
    if (!error) {
      error = Error(std::errc::invalid_argument);
    }
    return {};
  }
  return flash->path() / "dash.xex";
}
}  // namespace xe::vfs
