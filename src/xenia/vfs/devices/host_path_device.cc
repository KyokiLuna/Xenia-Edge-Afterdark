/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_device.h"
#include <algorithm>
#include <limits>
#include <system_error>
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/host_path_entry.h"

namespace xe {
namespace vfs {

HostPathDevice::HostPathDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path,
    bool read_only, bool collapse_content_packages, const std::string_view name,
    std::shared_ptr<filesystem::HostStorageRoot> storage,
    std::shared_ptr<const filesystem::HostStorageRoot> geometry,
    ControlRole control_role)
    : Device(mount_path),
      storage_(std::move(storage)),
      geometry_(std::move(geometry)),
      name_(name),
      host_path_(host_path),
      read_only_(read_only),
      collapse_content_packages_(collapse_content_packages),
      control_role_(control_role) {}

HostPathDevice::~HostPathDevice() = default;
HostPathDevice::PreparedSnapshotCache::~PreparedSnapshotCache() = default;
HostPathDevice::SnapshotCacheCommit::~SnapshotCacheCommit() = default;

std::shared_ptr<HostPathDevice::PreparedSnapshotCache>
HostPathDevice::PrepareSnapshotCache(
    std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
        tokens,
    std::error_code& error) {
  error.clear();
  if (read_only_ || !storage_ || tokens.empty() ||
      tokens.size() >
          filesystem::HostStorageRoot::kMaximumSnapshotBatchTokens) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  auto result = std::make_shared<PreparedSnapshotCache>();
  result->device =
      std::dynamic_pointer_cast<HostPathDevice>(shared_from_this());
  if (!result->device) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  result->rows.reserve(tokens.size());
  uint64_t total = 0;
  for (auto& token : tokens) {
    if (token &&
        token->bytes().size() >
            filesystem::HostStorageRoot::kMaximumSnapshotBatchBytes - total) {
      error = std::make_error_code(std::errc::file_too_large);
      return {};
    }
    if (token) {
      total += token->bytes().size();
    }
    if (!token || token->root() != storage_ || token->relative().empty() ||
        token->relative().has_parent_path() || token->relative() == "." ||
        token->relative() == "..") {
      error = std::make_error_code(std::errc::invalid_argument);
      return {};
    }
    auto leaf = std::make_unique<HostPathEntry>(
        this, nullptr, xe::path_to_utf8(token->relative()),
        host_path_ / token->relative());
    result->rows.push_back({std::move(token), std::move(leaf)});
  }
  auto lock = xe::global_critical_region::TryAcquire();
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  auto* root = static_cast<HostPathEntry*>(root_entry_.get());
  if (!root) {
    error = std::make_error_code(std::errc::no_such_device);
    return {};
  }
  // Allocation is allowed here, before the common currentness guard. Final
  // admission rechecks capacity in case ordinary namespace work consumed it.
  root->children_.reserve(root->children_.size() + result->rows.size());
  return result;
}
std::unique_ptr<HostPathDevice::SnapshotCacheCommit>
HostPathDevice::TryCommitSnapshotCache(
    std::shared_ptr<PreparedSnapshotCache> prepared,
    std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch,
    std::error_code& error) {
  error.clear();
  if (!prepared || prepared->device.get() != this || !batch ||
      batch->root() != storage_ || read_only_ ||
      prepared->rows.size() != batch->tokens().size()) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  auto result = std::unique_ptr<SnapshotCacheCommit>(new SnapshotCacheCommit);
  result->existing_.resize(prepared->rows.size());
  result->prepared_ = std::move(prepared);
  result->batch_ = std::move(batch);
  result->namespace_lock_ = xe::global_critical_region::TryAcquire();
  if (!result->namespace_lock_.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  auto* root = static_cast<HostPathEntry*>(root_entry_.get());
  if (!root) {
    error = std::make_error_code(std::errc::no_such_device);
    return {};
  }
  if (result->prepared_->consumed) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  size_t additions = 0;
  for (size_t i = 0; i != result->prepared_->rows.size(); ++i) {
    const auto& row = result->prepared_->rows[i];
    if (!row.new_leaf || row.token != result->batch_->tokens()[i] ||
        bool(result->batch_->metadata()[i]) != row.token->exists()) {
      error = std::make_error_code(std::errc::invalid_argument);
      return {};
    }
    const auto& name = row.new_leaf->name();
    const auto same_ascii = [](std::string_view a, std::string_view b) {
      if (a.size() != b.size()) {
        return false;
      }
      for (size_t n = 0; n != a.size(); ++n) {
        const auto fold = [](char c) {
          return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
        };
        if (fold(a[n]) != fold(b[n])) {
          return false;
        }
      }
      return true;
    };
    Entry* raw = nullptr;
    size_t aliases = 0;
    for (const auto& child : root->children_) {
      if (same_ascii(child->name(), name)) {
        ++aliases;
        raw = child.get();
      }
    }
    auto* existing = dynamic_cast<HostPathEntry*>(raw);
    if ((raw && !existing) || (existing && existing->name() != name)) {
      error = std::make_error_code(std::errc::state_not_recoverable);
      return {};
    }
    if (aliases > 1) {
      error = std::make_error_code(std::errc::state_not_recoverable);
      return {};
    }
    if (existing &&
        ((existing->attributes_ & kFileAttributeDirectory) ||
         existing->has_open_files() || existing->overwritten_.load() ||
         existing->overwrite_handle_ || existing->overwrite_lease_ ||
         existing->reported_size_fixed_)) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
    result->existing_[i] = existing;
    additions += row.token->exists() && !existing;
    if (row.token->exists() && result->batch_->metadata()[i]->total_size >
                                   SIZE_MAX - bytes_per_sector()) {
      error = std::make_error_code(std::errc::value_too_large);
      return {};
    }
  }
  if (root->children_.capacity() - root->children_.size() < additions) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  return result;
}
void HostPathDevice::SnapshotCacheCommit::Apply() noexcept {
  prepared_->device->ApplySnapshotCache(*this);
}
void HostPathDevice::ApplySnapshotCache(SnapshotCacheCommit& commit) noexcept {
  if (commit.applied_) {
    return;
  }
  auto& device = *commit.prepared_->device;
  auto* root = static_cast<HostPathEntry*>(device.root_entry_.get());
  for (size_t i = 0; i != commit.prepared_->rows.size(); ++i) {
    auto& row = commit.prepared_->rows[i];
    auto* entry = commit.existing_[i];
    if (!row.token->exists()) {
      if (entry) {
        std::erase_if(root->children_, [entry](const auto& child) {
          return child.get() == entry;
        });
      }
      continue;
    }
    const auto& metadata = *commit.batch_->metadata()[i];
    if (!entry) {
      entry = row.new_leaf.get();
      entry->parent_ = root;
      entry->attributes_ = kFileAttributeNormal;
      entry->create_timestamp_ = metadata.create_timestamp;
      root->children_.push_back(std::move(row.new_leaf));
    }
    entry->size_ = metadata.total_size;
    entry->allocation_size_ =
        xe::round_up(metadata.total_size, device.bytes_per_sector());
    entry->access_timestamp_ = metadata.access_timestamp;
    entry->write_timestamp_ = metadata.write_timestamp;
  }
  commit.prepared_->consumed = true;
  commit.applied_ = true;
}

bool HostPathDevice::Initialize() {
  std::error_code admission_error;
  auto pending_admission = filesystem::HostStorageRoot::AdmitGuestMount(
      std::vector<filesystem::GuestHostBacking>{
          storage_ ? storage_->GuestBacking()
                   : filesystem::GuestHostBacking{host_path_, true}},
      admission_error);
  if (!pending_admission) {
    return false;
  }
  if (storage_) {
    if (!storage_->Observe().available) {
      return false;
    }
    host_path_ = storage_->path();
    read_only_ = read_only_ || storage_->read_only();
  }
  if (!std::filesystem::exists(host_path_)) {
    if (storage_) {
      return false;
    }
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      XELOGE("Host path does not exist");
      return false;
    }
  }

  std::error_code policy_error;
  if (!storage_) {
    storage_ =
        filesystem::HostStorageRoot::Open(host_path_, read_only_, policy_error);
  }
  if (!storage_) {
    return false;
  }
  // Each mount has its own retirement boundary, even when it names the same
  // directory as the source. Package close cannot retire the source/siblings.
  storage_ = storage_->Subroot({}, read_only_, policy_error);
  if (!storage_) {
    return false;
  }
  if (geometry_) {
    storage_ = storage_->WithDependency(geometry_, policy_error);
    if (!storage_) {
      return false;
    }
  }
  storage_ = storage_->GuestView(policy_error);
  if (!storage_) {
    return false;
  }
  host_path_ = storage_->path();
  read_only_ = storage_->read_only();
  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  return true;
}

void HostPathDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("HostPathDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

namespace {

// Content-type folders are named with an 8-digit hex value (e.g. 00000001).
bool IsContentTypeName(const std::string& name) {
  if (name.size() != 8) {
    return false;
  }
  for (char c : name) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f');
    if (!hex) {
      return false;
    }
  }
  return true;
}

// A single-file content package is a directory holding one payload file. The
// thumbnail is Xenia bookkeeping, not game data, so it is ignored. Returns that
// payload's info so the package can be presented as the file itself.
bool ResolveSinglePackagePayload(const std::filesystem::path& package_dir,
                                 xe::filesystem::FileInfo& out_payload) {
  xe::filesystem::FileInfo* payload = nullptr;
  auto entries = xe::filesystem::ListFiles(package_dir);
  for (auto& entry : entries) {
    if (entry.type == xe::filesystem::FileInfo::Type::kDirectory) {
      return false;
    }
    if (entry.name == kPackageThumbnailFileName) {
      continue;
    }
    if (payload) {
      return false;
    }
    payload = &entry;
  }
  if (!payload) {
    return false;
  }
  out_payload = *payload;
  return true;
}

// Size the 360 STFS container would have for a single-file saved-game package.
// Games that stat the raw content path validate this container size, not the
// raw payload. Mirrors the block layout in stfs_container_device.cc.
uint64_t StfsSingleFileContainerSize(uint64_t payload_size) {
  constexpr uint64_t kBlockSize = 0x1000;
  constexpr uint32_t kBlocksPerHash = 2;    // read-write (CON) packages
  constexpr uint64_t kHeaderSize = 0xA000;  // round_up(0x971A metadata, block)
  constexpr uint32_t kLevel[3] = {0xAA, 0xAA * 0xAA, 0xAA * 0xAA * 0xAA};
  const uint32_t data_blocks =
      uint32_t((payload_size + kBlockSize - 1) / kBlockSize);
  // One file-table block, the data blocks, and one spare, matching containers.
  const uint32_t content_blocks = 1 + data_blocks + 1;
  uint64_t block = content_blocks;
  for (int i = 0; i < 3; i++) {
    block += ((content_blocks + kLevel[i]) / kLevel[i]) * kBlocksPerHash;
    if (content_blocks < kLevel[i]) {
      break;
    }
  }
  return kHeaderSize + block * kBlockSize;
}

uint32_t BytesToUnits(uintmax_t bytes, const Device& device) {
  uintmax_t unit_size = uintmax_t(device.sectors_per_allocation_unit()) *
                        device.bytes_per_sector();
  return uint32_t(std::min<uintmax_t>(bytes / unit_size,
                                      std::numeric_limits<uint32_t>::max()));
}

}  // namespace

filesystem::HostStorageSnapshot HostPathDevice::storage_space() const {
  auto result =
      storage_ ? storage_->Observe() : filesystem::HostStorageSnapshot{};
  if (result.available && geometry_) {
    result = geometry_->Observe();
    result.read_only = read_only_;
    if (result.read_only) {
      result.usable_bytes = 0;
    }
  }
  return result;
}
bool HostPathDevice::has_open_files() const {
  if (has_managed_files()) { return true; }
  return (root_entry_ && root_entry_->has_open_files()) ||
         (storage_ && storage_->open_file_count());
}
bool HostPathDevice::InstallSnapshotReservation(
    std::shared_ptr<filesystem::HostStorageRoot> reserved) {
  if (!weak_from_this().expired() || !storage_ || !reserved ||
      reserved->retiring() || reserved->path() != storage_->path() ||
      !reserved->IsSnapshotReservationOf(*storage_)) { return false; }
  storage_ = std::move(reserved);
  snapshot_reservation_ = true;
  return true;
}
void HostPathDevice::Retire() {
  if (storage_) {
    storage_->Retire();
    if (snapshot_reservation_ && !has_managed_files()) {
      storage_->ReleaseRetiredSnapshotReservation();
    }
  }
}
void HostPathDevice::OnManagedFileRundown() {
  if (snapshot_reservation_ && storage_ && storage_->retiring()) {
    storage_->ReleaseRetiredSnapshotReservation();
  }
}
uint32_t HostPathDevice::total_allocation_units() const {
  return BytesToUnits(storage_space().capacity_bytes, *this);
}

uint32_t HostPathDevice::available_allocation_units() const {
  return BytesToUnits(storage_space().usable_bytes, *this);
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  std::vector<std::filesystem::path> ancestors;
  PopulateEntry(parent_entry, ancestors);
}

void HostPathDevice::PopulateEntry(
    HostPathEntry* parent_entry,
    std::vector<std::filesystem::path>& ancestors) {
  std::error_code ec;
  auto canonical_self =
      std::filesystem::canonical(parent_entry->host_path(), ec);
  if (!ec) {
    for (const auto& seen : ancestors) {
      if (seen == canonical_self) {
        XELOGW(
            "HostPathDevice: {} is already on the path being walked; not "
            "following it again",
            xe::strip_pii(xe::path_to_utf8(parent_entry->host_path())));
        return;
      }
    }
    ancestors.push_back(canonical_self);
  }

  auto child_infos = xe::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    // Xenia bookkeeping, never part of the package the console hands out.
    if (child_info.type == xe::filesystem::FileInfo::Type::kFile &&
        child_info.name == kPackageThumbnailFileName) {
      continue;
    }

    // Do not populate private aliases (including symlinked directories) into
    // the cached guest tree. Bytes are guarded again at actual open time.
    if (storage_ && (storage_->has_guest_projection() ||
         filesystem::HostStorageRoot::HasProtectedGuestStorage())) {
      std::error_code error;
      auto visible =
          storage_->OpenFile((parent_entry->host_path() / child_info.name)
                                 .lexically_relative(storage_->path()),
                             filesystem::FileAccess::kFileReadData, error);
      if (!visible) {
        continue;
      }
    }

    // On the content partition, present a single-file package (a directory
    // under a content-type folder holding one payload file) as that file, the
    // way the console stores it. A game that stats the raw content path then
    // sees the save instead of an empty directory.
    if (collapse_content_packages_ &&
        child_info.type == xe::filesystem::FileInfo::Type::kDirectory &&
        IsContentTypeName(parent_entry->name())) {
      const auto package_dir = parent_entry->host_path() / child_info.name;
      xe::filesystem::FileInfo payload;
      if (ResolveSinglePackagePayload(package_dir, payload)) {
        const auto payload_host = package_dir / payload.name;
        if (storage_ &&
            (storage_->has_guest_projection() ||
         filesystem::HostStorageRoot::HasProtectedGuestStorage())) {
          std::error_code error;
          auto visible = storage_->OpenFile(
              payload_host.lexically_relative(storage_->path()),
              filesystem::FileAccess::kFileReadData, error);
          if (!visible) {
            continue;
          }
        }
        const uint64_t container_size =
            StfsSingleFileContainerSize(payload.total_size);
        payload.name = child_info.name;
        auto file_entry =
            HostPathEntry::Create(this, parent_entry, payload_host, payload);
        // Report the STFS container size, not the raw payload size, and keep it
        // pinned so update() does not reset it to the backing file's size.
        file_entry->size_ = container_size;
        file_entry->allocation_size_ =
            xe::round_up(container_size, bytes_per_sector());
        file_entry->reported_size_fixed_ = true;
        parent_entry->children_.push_back(std::unique_ptr<Entry>(file_entry));
        continue;
      }
    }

    auto child = HostPathEntry::Create(
        this, parent_entry, parent_entry->host_path() / child_info.name,
        child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child, ancestors);
    }
  }

  if (!ec) {
    ancestors.pop_back();
  }
}

}  // namespace vfs
}  // namespace xe
