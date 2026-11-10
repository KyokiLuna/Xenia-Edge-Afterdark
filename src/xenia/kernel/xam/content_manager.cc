/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/content_manager.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <charconv>
#include <span>
#include <set>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/host_storage.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/content_mutation_owner.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/host_storage_errors.h"

DECLARE_int32(license_mask);

namespace xe {
namespace kernel {
namespace xam {

struct ContentManager::ManagedFormat::Owners {
  std::mutex registration, mutex;
  bool closed = false, catalog = false;
  std::atomic<bool> ready{true};
  struct Registration {
    ContentSource source;
    filesystem::ManagedVolumeAuthority::Options options;
  };
  std::map<std::string, Registration> registrations;
  std::set<std::string> required;
  void RefreshReadyLocked() {
    const bool current = !catalog && !closed &&
        std::all_of(required.begin(), required.end(), [&](const auto& id) {
          const auto found = volumes.find(id);
          return found != volumes.end() && found->second &&
              !found->second->error() && found->second->profile_writable() &&
              !pending.contains(id);
        });
    ready.store(current, std::memory_order_release);
  }
  std::map<std::string, std::shared_ptr<const filesystem::ManagedVolumeAuthority>> volumes;
  // Impl has no strong backlink, so this is not a reference cycle.
  std::map<std::string, std::shared_ptr<Impl>> pending;
};
struct ContentManager::ManagedFormat::Impl {
  std::mutex execution;
  std::string source_id;
  std::unique_ptr<ContentSourceRegistry::ManagedTransition> registry;
  std::unique_ptr<filesystem::ManagedVolumeFormatHandoff> volume;
  std::error_code error;
  bool finished = false, preparing = true, ready_formatted = false;
  std::atomic<bool> canceled{false};
  std::optional<Owners::Registration> recovery_config;
  std::function<std::unique_ptr<ContentSourceRegistry::ManagedTransition>(
      const filesystem::ManagedVolumeFormatHandoff&, std::error_code&)> recovery_registry;
  std::shared_ptr<const filesystem::ManagedVolumeAuthority> ready;
  filesystem::ManagedDeviceFormatReceipt receipt;
};

static const char* kGameContentHeaderDirName = "Headers";
static const char* kSpaFilename = "spa.bin";

static std::atomic<uint64_t> content_device_id_{0};

static void TerminateMetadataStrings(vfs::XContentMetadata& metadata) {
  for (uint32_t i = 0; i < vfs::XContentMetadata::kNumLanguagesV1; ++i) {
    metadata.display_name_raw.uint[i][127] = 0;
    metadata.description_raw.uint[i][127] = 0;
  }
  if (metadata.metadata_version >= 2) {
    for (uint32_t i = 0; i < vfs::XContentMetadata::kNumLanguagesV2 -
                                 vfs::XContentMetadata::kNumLanguagesV1;
         ++i) {
      metadata.display_name_ex_raw.uint[i][127] = 0;
      metadata.description_ex_raw.uint[i][127] = 0;
    }
  }
  metadata.publisher_raw.uint[63] = 0;
  metadata.title_name_raw.uint[63] = 0;
}

ContentPackage::ContentPackage(KernelState* kernel_state,
                               const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path,
                               const std::string_view root_path, bool read_only,
                               std::shared_ptr<filesystem::HostStorageRoot> storage)
    : ContentPackage(kernel_state, root_name, data, root_path) {
  MountHostPath(package_path, read_only, std::move(storage));
}

ContentPackage::ContentPackage(KernelState* kernel_state, std::string_view root_name,
    const XCONTENT_AGGREGATE_DATA& data, const std::filesystem::path& path,
    std::string_view root_path, std::unique_ptr<vfs::XContentReadBacking> backing)
    : ContentPackage(kernel_state, root_name, data, root_path) {
  package_path_ = path;
  MountContainer(vfs::XContentContainerDevice::CreateContentDevice(
      device_path_, std::move(backing)), xe::strip_pii(xe::path_to_utf8(path)));
}

ContentPackage::ContentPackage(KernelState* kernel_state,
                               const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               vfs::Entry* package_entry)
    : ContentPackage(kernel_state, root_name, data, std::string_view()) {
  // A host path also mounts an extracted package directory, writable only
  // where the entry is.
  if (auto host_entry = dynamic_cast<vfs::HostPathEntry*>(package_entry)) {
    MountHostPath(host_entry->host_path(), host_entry->is_read_only());
    return;
  }
  MountContainer(vfs::XContentContainerDevice::CreateContentDevice(
                     device_path_, package_entry),
                 package_entry->absolute_path());
}

ContentPackage::ContentPackage(KernelState* kernel_state,
                               const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::string_view root_path)
    : kernel_state_(kernel_state),
      root_name_(root_name),
      content_data_(data),
      license_(cvars::license_mask), root_path_(root_path) {
  device_path_ = fmt::format("\\Device\\Content\\{0}\\",
      content_device_id_.fetch_add(1, std::memory_order_relaxed) + 1);
}

void ContentPackage::MountHostPath(const std::filesystem::path& package_path,
                                   bool read_only,
                                   std::shared_ptr<filesystem::HostStorageRoot> storage) {
  package_path_ = package_path;
  std::error_code ec;
  if (std::filesystem::is_directory(package_path, ec)) {
    auto device = std::make_unique<vfs::HostPathDevice>(
        device_path_, package_path, read_only, false, "STFS", std::move(storage));
    if (device->Initialize()) { PublishMount(std::move(device)); }
  } else {
    // A title can install content by copying the package file itself, so a
    // package is either an extracted directory or a container.
    MountContainer(vfs::XContentContainerDevice::CreateContentDevice(
                       device_path_, package_path),
                   xe::strip_pii(xe::path_to_utf8(package_path)));
  }
}

void ContentPackage::MountContainer(
    std::unique_ptr<vfs::XContentContainerDevice> device,
    const std::string_view name) {
  if (!device || !device->Initialize()) {
    XELOGE("{}: Cannot mount content container {}", __func__, name);
    return;
  }
  license_ = device->license_mask();
  PublishMount(std::move(device));
}

void ContentPackage::PublishMount(std::unique_ptr<vfs::Device> device) {
  auto* fs = kernel_state_->file_system();
  std::shared_ptr<vfs::Device> published;
  // Initialize/AdmitGuestStorage observes actual storage before namespace
  // publication. The mutable managed caller does not hold an outer GCRC.
  if (XFAILED(fs->RegisterDeviceExclusive(std::move(device), published))) { return; }
  registered_device_ = published;
  {
    auto lock = global_critical_region::AcquireDirect();
    if (fs->AcquireDevice(device_path_) == published &&
        fs->RegisterSymbolicLink(root_name_ + ":",
            xe::utf8::join_guest_paths(device_path_, root_path_))) {
      link_owned_ = fs->FindSymbolicLink(root_name_ + ":", published_link_target_);
      mounted_ = link_owned_;
    }
  }
  if (!mounted_) {
    fs->UnregisterDevice(device_path_, published.get());
    registered_device_.reset();
  }
}

ContentPackage::~ContentPackage() {
  auto* fs = kernel_state_->file_system();
  auto published = registered_device_.lock();
  {
    auto lock = global_critical_region::AcquireDirect();
    std::string current;
    const auto actual = fs->AcquireDevice(device_path_);
    if (link_owned_ && published && (!actual || actual == published) &&
        fs->FindSymbolicLink(root_name_ + ":", current) &&
        xe::utf8::equal_case(current, published_link_target_)) {
      fs->UnregisterSymbolicLink(root_name_ + ":");
    }
  }
  // Expected-pointer withdrawal never removes a replacement device. Destruct
  // retained storage only outside our short namespace critical region.
  if (published) { fs->UnregisterDevice(device_path_, published.get()); }
}

void ContentPackage::LoadPackageLicenseMask(
    const std::filesystem::path header_path) {
  auto header = vfs::XContentContainerDevice::ReadContainerHeader(header_path);
  if (header && header->content_header.is_magic_valid()) {
    license_ = vfs::XContentContainerDevice::LicenseMaskFromHeader(*header);
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(header_path, ec) || ec) {
    return;
  }

  auto file = xe::filesystem::OpenFile(header_path, "rb");
  if (!file) {
    return;
  }
  const auto file_size = std::filesystem::file_size(header_path, ec);
  if (ec || file_size < sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    fclose(file);
    return;
  }

  fseek(file, sizeof(XCONTENT_AGGREGATE_DATA), SEEK_SET);
  uint32_t stored_license = 0;
  if (fread(&stored_license, 1, sizeof(stored_license), file) ==
      sizeof(stored_license)) {
    license_ = stored_license;
  }
  fclose(file);
}

struct ContentManager::RootAdmission final {
  ContentManager* owner;
  string_key_insensitive key;
  RootAdmission(ContentManager* value, string_key_insensitive name)
      : owner(value), key(std::move(name)) {}
  ~RootAdmission() {
    auto lock = owner->global_critical_region_.Acquire();
    owner->pending_package_roots_.erase(key);
  }
};
std::unique_ptr<ContentManager::RootAdmission> ContentManager::ClaimRootName(
    std::string_view root_name) {
  auto key = string_key_insensitive::create(root_name);
  auto lock = global_critical_region_.Acquire();
  if (open_packages_.contains(key) || pending_package_roots_.contains(key)) { return {}; }
  auto admission = std::make_unique<RootAdmission>(this, key);
  pending_package_roots_.insert(key);
  return admission;
}

struct ContentManager::PackageAdmission {
  std::shared_ptr<PackageAdmissionState> state;
  std::filesystem::path path;
  bool mutation, active = false;
  PackageAdmission(std::shared_ptr<PackageAdmissionState> s, std::filesystem::path p, bool m)
      : state(std::move(s)), path(std::move(p)), mutation(m) {}
  ~PackageAdmission() {
    if (!active) { return; }
    std::lock_guard lock(state->mutex);
    const auto found = state->pending.find(path);
    if (found == state->pending.end()) { return; }
    if (mutation) { found->second.mutation = false; } else { --found->second.readers; }
    if (!found->second.mutation && !found->second.readers) { state->pending.erase(found); }
  }
};
std::shared_ptr<ContentManager::PackageAdmission> ContentManager::ClaimPackagePath(
    const std::filesystem::path& physical, bool mutation) {
  const auto path = physical.lexically_normal();
  auto result = std::make_shared<PackageAdmission>(package_admissions_, path, mutation);
  // Existing GCRC protects only the open-package map. The independent claim
  // state contains no filesystem owner and is never locked across I/O/callbacks.
  auto map_lock = global_critical_region_.Acquire();
  std::lock_guard lock(package_admissions_->mutex);
  if (package_admissions_->closed) { return {}; }
  const auto pending = package_admissions_->pending.find(path);
  if (pending != package_admissions_->pending.end() &&
      (pending->second.mutation || (mutation && pending->second.readers))) { return {}; }
  if (mutation && std::ranges::any_of(open_packages_, [&](const auto& pair) {
        return pair.second->package_path().lexically_normal() == path;
      })) { return {}; }
  auto& slot = package_admissions_->pending[path];
  if (mutation) { slot.mutation = true; } else { ++slot.readers; }
  result->active = true;
  return result;
}

ContentManager::ContentManager(ReadOnlyCatalogTag,
                               const std::filesystem::path& root_path)
    : root_path_(root_path),
      managed_owners_(std::make_shared<ManagedFormat::Owners>()),
      sources_(ContentSourceRegistry::Access::kReadOnlyCatalog) {
  // A host reference is sufficient for exact-ID discovery. It is not an
  // initialized HDD and must not create metadata or publish a physical volume.
  managed_owners_->catalog = true;
  managed_owners_->ready.store(false, std::memory_order_release);
  sources_.Register({"hdd", static_cast<uint32_t>(DummyDeviceId::HDD),
                     DeviceType::HDD, ContentSourceKind::kHostMedia, root_path,
                     "\\Device\\Harddisk0\\Partition1\\Content", u"Catalog",
                     true, 0});
}

ContentManager::ContentManager(
    std::shared_ptr<const filesystem::ManagedStorageContext> storage_context,
    const std::filesystem::path& root_path, KernelState* guest_runtime)
    : kernel_state_(guest_runtime),
      storage_context_(std::move(storage_context)),
      root_path_(root_path),
      managed_owners_(std::make_shared<ManagedFormat::Owners>()) {
  RegisterRequiredManagedSource(
      {"hdd", static_cast<uint32_t>(DummyDeviceId::HDD), DeviceType::HDD,
       ContentSourceKind::kManaged, root_path,
       "\\Device\\Harddisk0\\Partition1\\Content", u"Hard Drive", false, 0,
       "\\Device\\Harddisk0\\Partition1"});
}

bool ContentManager::RegisterRequiredManagedSource(ContentSource source) {
  {
    std::lock_guard lock(managed_owners_->mutex);
    managed_owners_->required.insert(source.id);
  }
  const bool ready = RegisterManagedSource(std::move(source));
  {
    std::lock_guard lock(managed_owners_->mutex);
    managed_owners_->RefreshReadyLocked();
  }
  return ready;
}
bool ContentManager::managed_sources_ready() const {
  return managed_owners_->ready.load(std::memory_order_acquire);
}

bool ContentManager::RegisterManagedSource(ContentSource source) {
  if (!storage_context_ || sources_.read_only_catalog() ||
      source.kind != ContentSourceKind::kManaged || source.read_only ||
      source.id.empty() || !source.device_id || source.root.empty() ||
      source.device_type == DeviceType::Invalid) {
    return false;
  }
  if (xe::global_critical_region::is_held_by_current_thread()) { return false; }
  std::unique_lock registration(managed_owners_->registration, std::try_to_lock);
  if (!registration.owns_lock() || storage_context_->CheckCurrent()) {
    return false;
  }
  {
    std::lock_guard lock(managed_owners_->mutex);
    if (managed_owners_->closed || managed_owners_->volumes.contains(source.id) ||
        managed_owners_->pending.contains(source.id)) { return false; }
  }
  std::error_code error;
  const auto& data_parent = storage_context_->data_parent();
  const auto& selected_state = storage_context_->state_parent_override();
  if (source.id == "hdd" && selected_state) {
    if (!source.private_state_parent.empty() &&
        source.private_state_parent != selected_state->path()) {
      return false;
    }
    source.private_state_parent = selected_state->path();
  }
  auto storage = filesystem::HostStorageRoot::Open(source.root, false, error,
                                                   source.reserved_bytes);
  if (!storage && error == std::errc::no_such_file_or_directory) {
    // Creation is anchored at the owned data root or the explicit selected
    // Content path's existing parent. This does not select a journal parent.
    auto managed_parent = data_parent;
    auto relative =
        source.root.lexically_normal().lexically_relative(data_parent->path());
    if (relative.empty() || relative.is_absolute() ||
        *relative.begin() == "..") {
      managed_parent = filesystem::HostStorageRoot::Open(
          source.root.parent_path(), false, error, source.reserved_bytes);
      relative = source.root.filename();
    }
    if (!managed_parent) {
      return false;
    }
    for (const auto& part : relative) {
      if (part.empty() || part == ".") {
        continue;
      }
      if (part == "..") {
        return false;
      }
      auto child = managed_parent->Subroot(part, false, error);
      if (!child && error == std::errc::no_such_file_or_directory) {
        error = managed_parent->Create(part, true);
        if (error && error != std::errc::file_exists) {
          return false;
        }
        child = managed_parent->Subroot(part, false, error);
      }
      if (!child) {
        return false;
      }
      managed_parent = std::move(child);
    }
    storage = std::move(managed_parent);
  }
  if (!storage) {
    return false;
  }
  filesystem::ManagedVolumeAuthority::Options options;
  options.content = storage;
  options.source_id = source.id;
  options.device_id = source.device_id;
  // The primary default HDD keeps its old journal/recovery/public paths.
  // Nondefault Content, including another host FS, receives its own bundle.
  const bool primary =
      source.id == "hdd" && storage->path() == data_parent->path() / "content";
  if (primary && !source.private_state_parent.empty()) {
    XELOGE("Primary HDD legacy owner locations cannot be overridden");
    return false;
  }
  if (primary) {
    auto volume = data_parent;
    for (const auto& leaf : {"volumes", "hdd0", "partition1", "Content"}) {
      auto next = volume->Subroot(leaf, false, error);
      if (!next && error == std::errc::no_such_file_or_directory) {
        error = volume->Create(leaf, true);
        if (!error || error == std::errc::file_exists) {
          next = volume->Subroot(leaf, false, error);
        }
      }
      if (!next) { return false; }
      if (std::string_view(leaf) == "Content") { break; }
      volume = std::move(next);
    }
    options.preserve_legacy_layout = true;
    options.legacy_private_parent = data_parent;
    options.legacy_public_root = std::move(volume);
    options.legacy_recovery = storage_context_->legacy_recovery();
  } else if (!source.host_volume_root.empty()) {
    options.legacy_public_root = filesystem::HostStorageRoot::Open(
        source.host_volume_root, false, error, source.reserved_bytes);
    if (!options.legacy_public_root) { return false; }
  }
  if (!primary) {
    // Pre-V1 RegisterManagedSource used this exact data-parent journal and
    // recovery. The HDD shell is a producer-known path; MU supplies its
    // explicit public root. Capture only existing directories, never create a
    // legacy candidate or infer Parent(Content).
    auto known_public = options.legacy_public_root;
    if (!known_public && source.id == "hdd") {
      known_public = data_parent->Subroot(
          std::filesystem::path("volumes") / "hdd0" / "partition1", false,
          error);
    }
    if (known_public) {
      options.existing_legacy =
          filesystem::ManagedVolumeAuthority::Options::ExistingLegacy{
              data_parent, std::move(known_public),
              storage_context_->legacy_recovery()};
    }
  }
  if (!source.private_state_parent.empty()) {
    if (source.private_state_parent == data_parent->path()) {
      options.state_parent_override = data_parent;
    } else if (selected_state &&
               source.private_state_parent == selected_state->path()) {
      options.state_parent_override = selected_state;
    } else {
      // This is an explicit per-source choice, never an error fallback.
      options.state_parent_override = filesystem::HostStorageRoot::Open(
          source.private_state_parent, false, error, source.reserved_bytes);
    }
    if (!options.state_parent_override) { return false; }
  }
  // Retain only producer capabilities/configuration for explicit noncreating
  // recovery. Options contains no MVA, intent owner or source flock.
  source.root = options.content->path();
  {
    std::lock_guard lock(managed_owners_->mutex);
    managed_owners_->registrations.emplace(source.id,
        ManagedFormat::Owners::Registration{source, options});
  }
  const auto volume = filesystem::ManagedVolumeAuthority::Open(std::move(options));
  {
    std::lock_guard lock(managed_owners_->mutex);
    // No retry silently replaces a live/error owner for the same source.
    if (!managed_owners_->volumes.emplace(source.id, volume).second) { return false; }
  }
  if (!volume || volume->error()) {
    XELOGE("Managed source {} authority unavailable: {}", source.id,
           volume ? volume->error().message() : "missing owner");
    return false;
  }
  return sources_.RegisterManaged(std::move(source), volume->identity(), volume);
}
std::shared_ptr<const filesystem::ManagedVolumeAuthority>
ContentManager::Volume(const std::string& source_id) const {
  std::unique_lock lock(managed_owners_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) { return {}; }
  const auto found = managed_owners_->volumes.find(source_id);
  return found == managed_owners_->volumes.end() ? nullptr : found->second;
}
std::shared_ptr<const filesystem::ProfileIntentOwner>
ContentManager::ProfileIntents(const std::string& source_id) const {
  const auto volume = Volume(source_id);
  return volume && volume->profile_writable() ? volume->intents() : nullptr;
}

ContentManager::~ContentManager() {
  {
    std::lock_guard lock(package_admissions_->mutex);
    package_admissions_->closed = true;
  }
  {
    std::lock_guard lock(managed_owners_->mutex);
    managed_owners_->closed = true;
    managed_owners_->ready.store(false, std::memory_order_release);
  }
  // Kernel teardown has joined dispatch/CPU producers and reset guest handles.
  // These raw map values are OWNED packages, not borrowed catalog entries.
  // Leaving them behind retains source_->volume and its real HDD flock across
  // the next Kernel setup, even after the old VFS has been destroyed.
  decltype(open_packages_) closing;
  {
    auto lock = global_critical_region_.Acquire();
    closing.swap(open_packages_);
  }
  struct RetiringPackage {
    ContentPackage* package;
    std::shared_ptr<vfs::Device> device;
    const vfs::Device* parent = nullptr;
  };
  std::vector<RetiringPackage> packages;
  packages.reserve(closing.size());
  {
    auto lock = global_critical_region_.Acquire();
    for (const auto& [name, package] : closing) {
      auto device = package->registered_device_.lock();
      const auto* container =
          dynamic_cast<const vfs::XContentContainerDevice*>(device.get());
      const auto* entry = container ? container->package_entry() : nullptr;
      packages.push_back({package, std::move(device),
                          entry ? entry->device() : nullptr});
    }
  }
  // Child containers may hold a File/Entry in their parent package. Keep the
  // exact Device alive and withdraw children first; expected-pointer unmount
  // and all last native-owner releases happen outside BOTH metadata locks.
  while (!packages.empty()) {
    const auto leaf = std::find_if(packages.begin(), packages.end(),
        [&](const RetiringPackage& candidate) {
          return std::none_of(packages.begin(), packages.end(),
              [&](const RetiringPackage& other) {
                return other.parent && other.parent == candidate.device.get();
              });
        });
    if (leaf == packages.end()) {
      // Actual package mounts precede their children, so normal backing is a
      // DAG. An unknown cycle lacks a proven rundown order: retain ALL of its
      // owners and stop before deleting any cyclic package or reusing storage.
      XELOGE("Cyclic content backing prevents safe Kernel teardown");
      xe::FlushLog();
      std::quick_exit(EXIT_FAILURE);
    }
    auto retired = std::move(*leaf);
    packages.erase(leaf);
    delete retired.package;
  }
}
ContentManager::ManagedFormat::ManagedFormat(
    std::shared_ptr<Impl> impl, std::shared_ptr<Owners> owners)
    : impl_(std::move(impl)), owners_(std::move(owners)) {}
ContentManager::ManagedFormat::~ManagedFormat() = default;
void ContentManager::ManagedFormat::Cancel() {
  // The base ticket observes cancellation only before durable Prepared. It
  // never lies about cancellation after the irreversible handoff boundary.
  impl_->canceled.store(true, std::memory_order_release);
}
std::unique_ptr<ContentManager::ManagedFormat>
ContentManager::CapturePendingManagedFormat(const std::string& id,
                                           std::error_code& error) {
  error.clear();
  std::unique_lock lock(managed_owners_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  const auto found = managed_owners_->pending.find(id);
  if (managed_owners_->closed || found == managed_owners_->pending.end()) {
    error = std::make_error_code(std::errc::no_such_device); return {};
  }
  if (found->second->preparing) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  return std::unique_ptr<ManagedFormat>(new ManagedFormat(found->second, managed_owners_));
}
std::unique_ptr<ContentManager::ManagedFormat>
ContentManager::BeginIdleManagedFormat(const std::string& id,
                                     std::error_code& error) {
  return BeginIdleManagedFormatExpected(id, {}, error);
}
std::unique_ptr<ContentManager::ManagedFormat>
ContentManager::BeginIdleManagedFormat(
    std::shared_ptr<const ManagedFormatSourceProof> expected,
    std::error_code& error) {
  if (!expected) {
    error = std::make_error_code(std::errc::invalid_argument); return {};
  }
  const auto id = expected->source_id();
  return BeginIdleManagedFormatExpected(id, std::move(expected), error);
}
std::unique_ptr<ContentManager::ManagedFormat>
ContentManager::BeginIdleManagedFormatExpected(const std::string& id,
    std::shared_ptr<const ManagedFormatSourceProof> expected,
    std::error_code& error) {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  // The weak State check can release its transient rundown owner. Do it
  // before registration/profile/manager/registry metadata locks are acquired.
  if (expected && !expected->current()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  std::unique_lock registration(managed_owners_->registration, std::try_to_lock);
  if (!registration.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  auto source = sources_.Source(id);
  auto volume = Volume(id);
  if (!source || !source->available || !source->format_eligible || !volume ||
      volume != source->volume || !volume->format_eligible()) {
    error = std::make_error_code(std::errc::no_such_device); return {};
  }
  if (expected && (source->managed_format_proof != expected ||
      source->generation != expected->generation() ||
      !expected->MatchesIdentity(source->managed_identity) ||
      !expected->MatchesVolume(volume))) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  auto impl = std::make_shared<ManagedFormat::Impl>();
  impl->source_id = id;
  impl->volume = volume->CaptureFormatHandoff();
  if (!impl->volume || impl->volume->error()) {
    error = impl->volume ? impl->volume->error()
                        : std::make_error_code(std::errc::permission_denied);
    return {};
  }
  std::unique_ptr<ProfileManager::ManagedFormatIdleReservation> idle;
  if (kernel_state_) {
    auto* xam = kernel_state_->xam_state();
    auto* profiles = xam ? xam->profile_manager() : nullptr;
    if (!profiles) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    idle = profiles->TryReserveIdleForManagedFormat(error);
    if (!idle) { return {}; }
  }
  std::vector<std::shared_ptr<const filesystem::ManagedVolumeAuthority>> detached;
  detached.reserve(3);
  {
    std::unique_lock owners(managed_owners_->mutex, std::try_to_lock);
    if (!owners.owns_lock()) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    const auto current = managed_owners_->volumes.find(id);
    if (managed_owners_->closed ||
        current == managed_owners_->volumes.end() || current->second != volume ||
        managed_owners_->pending.contains(id) ||
        (expected && (source->managed_format_proof != expected ||
                      source->generation != expected->generation() ||
                      !expected->MatchesIdentity(source->managed_identity)))) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    // Allocate the durable in-process owner before any logical retirement.
    managed_owners_->pending.emplace(id, impl);
    impl->registry = sources_.BeginManagedTransition(
        id, source->generation, volume, detached, error, expected);
    if (!impl->registry) {
      managed_owners_->pending.erase(id); return {};
    }
    impl->volume->ClosePublishedPublicView();
    detached.push_back(std::move(current->second));
    managed_owners_->volumes.erase(current);
    managed_owners_->RefreshReadyLocked();
  }
  idle.reset();  // Release namespace/profile guards BEFORE any destruction/IO.
  registration.unlock();
  source->volume.reset();
  volume.reset();
  detached.clear();  // Actual source/intent leases release outside all locks.
  // A reentrant sink must still observe BUSY. Publish the initial withdrawal
  // before any captured operation can execute or publish a fresh generation.
  impl->registry->PublishWithdrawal();
  {
    std::lock_guard lock(managed_owners_->mutex);
    impl->preparing = false;
  }
  return std::unique_ptr<ManagedFormat>(new ManagedFormat(impl, managed_owners_));
}
std::unique_ptr<ContentManager::ManagedFormat>
ContentManager::InspectManagedFormatRecovery(const std::string& id,
                                            std::error_code& error) {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  std::unique_lock registration(managed_owners_->registration, std::try_to_lock);
  if (!registration.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  ManagedFormat::Owners::Registration config;
  auto impl = std::make_shared<ManagedFormat::Impl>();
  impl->source_id = id;
  std::shared_ptr<const filesystem::ManagedVolumeAuthority> detached;
  {
    std::lock_guard lock(managed_owners_->mutex);
    const auto known = managed_owners_->registrations.find(id);
    const auto failed = managed_owners_->volumes.find(id);
    if (managed_owners_->closed || managed_owners_->pending.contains(id) ||
        known == managed_owners_->registrations.end() ||
        failed == managed_owners_->volumes.end() || !failed->second ||
        failed->second->error() != std::errc::operation_in_progress) {
      error = std::make_error_code(std::errc::operation_not_permitted); return {};
    }
    config = known->second;
  }
  std::unique_ptr<ProfileManager::ManagedFormatIdleReservation> idle;
  if (kernel_state_) {
    auto* xam = kernel_state_->xam_state();
    auto* profiles = xam ? xam->profile_manager() : nullptr;
    if (!profiles) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    idle = profiles->TryReserveIdleForManagedFormat(error);
    if (!idle) { return {}; }
  }
  {
    std::unique_lock lock(managed_owners_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    const auto failed = managed_owners_->volumes.find(id);
    if (failed == managed_owners_->volumes.end() ||
        !failed->second || failed->second->error() != std::errc::operation_in_progress ||
        managed_owners_->pending.contains(id)) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    impl->recovery_config = config;
    impl->recovery_registry = sources_.CaptureManagedRecoveryAdmission(config.source);
    managed_owners_->pending.emplace(id, impl);
    detached = std::move(failed->second);
    managed_owners_->volumes.erase(failed);
    managed_owners_->RefreshReadyLocked();
  }
  idle.reset();
  registration.unlock();
  detached.reset();  // Release failed bootstrap source flock outside all locks.
  impl->volume = filesystem::ManagedVolumeAuthority::InspectFormatRecovery(config.options);
  impl->error = impl->volume ? impl->volume->error()
      : std::make_error_code(std::errc::io_error);
  // No data or epoch creation has happened. Keep actual inspected uncertainty
  // owned even on refusal; never re-open a generic writable folder fallback.
  if (!impl->error) {
    impl->registry = sources_.BeginManagedRecovery(config.source, *impl->volume,
                                                  impl->error);
  }
  // Snapshot the inspection outcome before exposing mutable execution state.
  // No factory/registry/profile/namespace locks cross the sink callback.
  const auto inspection_error = impl->error;
  if (impl->registry) { impl->registry->PublishWithdrawal(); }
  {
    std::lock_guard lock(managed_owners_->mutex);
    impl->preparing = false;
    managed_owners_->RefreshReadyLocked();
  }
  error = inspection_error;
  return std::unique_ptr<ManagedFormat>(new ManagedFormat(impl, managed_owners_));
}

filesystem::ManagedDeviceFormatReceipt
ContentManager::ManagedFormat::Execute() {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    filesystem::ManagedDeviceFormatReceipt busy;
    busy.error = std::make_error_code(std::errc::operation_would_block); return busy;
  }
  std::unique_lock run(impl_->execution, std::try_to_lock);
  if (!run.owns_lock()) {
    filesystem::ManagedDeviceFormatReceipt busy;
    busy.error = std::make_error_code(std::errc::device_or_resource_busy); return busy;
  }
  if (impl_->finished) { return impl_->receipt; }
  {
    std::lock_guard lock(owners_->mutex);
    if (owners_->closed) { impl_->canceled.store(true, std::memory_order_release); }
  }
  if (!impl_->registry) {
    impl_->receipt.error = impl_->error ? impl_->error
        : std::make_error_code(std::errc::operation_not_permitted);
    return impl_->receipt;
  }
  if (impl_->canceled.load(std::memory_order_acquire)) { impl_->volume->Cancel(); }
  const bool retry_publication = bool(impl_->ready);
  if (!retry_publication) {
    // An uncertain durable handoff requires explicit Resume, never a second
    // implicit data reset. A phase-zero refusal may retry unchanged restore.
    if (!impl_->error || !impl_->volume->phase()) {
      impl_->receipt = filesystem::ManagedVolumeAuthority::ExecuteFormat(
          *impl_->volume, &impl_->canceled);
      impl_->error = impl_->receipt.error;
    }
  }
  std::shared_ptr<const filesystem::ManagedVolumeAuthority> next = impl_->ready;
  const bool unchanged = !impl_->receipt.complete && !impl_->volume->phase();
  if (!next && impl_->error && unchanged) {
    next = filesystem::ManagedVolumeAuthority::RestoreUnchangedFormat(*impl_->volume);
  } else if (!next && !impl_->error) {
    next = filesystem::ManagedVolumeAuthority::FromCompletedFormat(*impl_->volume);
  }
  if (next && !next->error()) {
    if (!retry_publication) { impl_->ready_formatted = !unchanged; }
    impl_->ready = next;
    // Keep the manager owner ready before publishing callbacks. Registry still
    // excludes guest admission until exact fresh generation publication below.
    {
      std::lock_guard lock(owners_->mutex);
      owners_->volumes.insert_or_assign(impl_->source_id, next);
    }
    const auto publish_error = impl_->registry->Publish(next);
    if (!publish_error) {
      impl_->finished = true;
      impl_->error.clear();
      if (!unchanged) {
        impl_->receipt.error.clear();
        impl_->receipt.current = next->identity();
      }
      {
        std::lock_guard lock(owners_->mutex);
        owners_->pending.erase(impl_->source_id);
        owners_->RefreshReadyLocked();
      }
      impl_->ready.reset();  // Completed handle must not pin an old source flock.
    } else {
      impl_->error = publish_error;
    }
  } else if (next) {
    impl_->error = next->error();
  }
  if (impl_->error && !unchanged) { impl_->receipt.error = impl_->error; }
  else if (impl_->error && !impl_->receipt.error) { impl_->receipt.error = impl_->error; }
  return impl_->receipt;
}
std::error_code ContentManager::ManagedFormat::Resume() {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return std::make_error_code(std::errc::operation_would_block);
  }
  std::unique_lock run(impl_->execution, std::try_to_lock);
  if (!run.owns_lock()) {
    return std::make_error_code(std::errc::device_or_resource_busy);
  }
  if (impl_->finished) { return {}; }
  if (!impl_->registry && impl_->recovery_config) {
    // Explicit retry after a real cooperating owner released its source flock.
    // Old inspected error owners/metadata are dropped before a new description.
    impl_->volume.reset();
    impl_->volume = filesystem::ManagedVolumeAuthority::InspectFormatRecovery(
        impl_->recovery_config->options);
    impl_->error = impl_->volume ? impl_->volume->error()
        : std::make_error_code(std::errc::io_error);
    if (!impl_->error) {
      impl_->registry = impl_->recovery_registry(*impl_->volume, impl_->error);
    }
  }
  if (!impl_->registry) {
    return impl_->error ? impl_->error
        : std::make_error_code(std::errc::operation_not_permitted);
  }
  const bool retry_publication = bool(impl_->ready);
  auto next = impl_->ready ? impl_->ready
      : filesystem::ManagedVolumeAuthority::ResumeFormat(*impl_->volume);
  if (!next || next->error()) {
    impl_->error = next ? next->error() : std::make_error_code(std::errc::io_error);
    return impl_->error;
  }
  if (!retry_publication) { impl_->ready_formatted = true; }
  impl_->ready = next;
  {
    std::lock_guard lock(owners_->mutex);
    owners_->volumes.insert_or_assign(impl_->source_id, next);
  }
  impl_->error = impl_->registry->Publish(next);
  if (!impl_->error) {
    impl_->finished = true;
    if (impl_->ready_formatted) {
      impl_->receipt.error.clear();
      impl_->receipt.complete = true;
      impl_->receipt.epoch_durable = true;
      if (!impl_->receipt.epoch_published) { impl_->receipt.epoch_observed = true; }
      impl_->receipt.content_epoch = next->identity()->content_epoch();
      impl_->receipt.current = next->identity();
    }
    {
      std::lock_guard lock(owners_->mutex);
      owners_->pending.erase(impl_->source_id);
      owners_->RefreshReadyLocked();
    }
    impl_->ready.reset();
  }
  return impl_->error;
}

bool ContentManager::ResolveTitleId(uint32_t& title_id) const {
  if (title_id != kCurrentlyRunningTitleId) {
    return true;
  }
  if (!kernel_state_) {
    return false;
  }
  title_id = kernel_state_->title_id();
  return true;
}

std::vector<ContentManager::PackageSourcePath> ContentManager::ReadPackageRoots(
    uint32_t device_id, uint64_t xuid, uint32_t title_id,
    XContentType content_type) const {
  if (!ResolveTitleId(title_id)) {
    return {};
  }
  const auto relative =
      xe::to_path(fmt::format("{:016X}/{:08X}/{:08X}", xuid, title_id,
                              static_cast<uint32_t>(content_type)));
  std::vector<PackageSourcePath> result;
  for (const auto& source : sources_.Sources(device_id)) {
    if (auto path = sources_.Resolve(source.source.id, relative, false)) {
      result.push_back({source.source.id, std::move(*path)});
    }
  }
  return result;
}

// The error a package's file name fails with in FATX, if any.
static X_RESULT CheckFileName(const std::string_view name) {
  // FATX refuses "." and ".." and these characters in any component.
  constexpr std::string_view kInvalidCharacters = "\"*+,/:;<=>?|";
  size_t component_start = 0;
  for (size_t i = 0; i <= name.size(); ++i) {
    if (i < name.size() && name[i] != '\\') {
      if (uint8_t(name[i]) < 0x20 ||
          kInvalidCharacters.find(name[i]) != std::string_view::npos) {
        return X_ERROR_INVALID_NAME;
      }
      continue;
    }
    const std::string_view component =
        name.substr(component_start, i - component_start);
    if (component == "." || component == "..") {
      return X_ERROR_INVALID_NAME;
    }
    component_start = i + 1;
  }
  // A package is a file in FATX so a name with a separator in it names none.
  // Only the first byte may be non-ASCII so a name with any other doesn't
  // either.
  if (name.empty() || name.find('\\') != std::string_view::npos ||
      std::any_of(name.begin(), name.end(),
                  [](const char c) { return uint8_t(c) >= 0x80; })) {
    return X_ERROR_PATH_NOT_FOUND;
  }
  return X_ERROR_SUCCESS;
}

// The name a package is stored under and the XUID it's stored for.
static X_RESULT ResolveStoredPackageName(const uint64_t xuid,
                                         const XCONTENT_AGGREGATE_DATA& data,
                                         std::string& file_name,
                                         uint64_t& stored_xuid) {
  file_name = data.file_name();
  const bool shared =
      !file_name.empty() && (uint8_t(file_name[0]) & kSharedContentFileNameBit);
  if (shared) {
    file_name[0] = char(uint8_t(file_name[0]) & ~kSharedContentFileNameBit);
  }
  file_name = xe::string_util::trim(file_name);
  if (const X_RESULT result = CheckFileName(file_name)) {
    return result;
  }

  stored_xuid = (data.xuid != -1 && data.xuid != 0) ? data.xuid.get() : xuid;
  // All DLCs are stored in common directory, so we need to override xuid for
  // them and probably some other types.
  if (shared || data.content_type == XContentType::kMarketplaceContent) {
    stored_xuid = 0;
  }
  return X_ERROR_SUCCESS;
}

static bool IsDiscContent(const XCONTENT_AGGREGATE_DATA& data) {
  return data.device_id == static_cast<uint32_t>(DummyDeviceId::ODD);
}

// The game library stands in for the dashboard's game packages with a record
// per release, named after it.
static std::vector<uint8_t> ReadCatalogIcon(const std::filesystem::path &path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || !size || size > 4 * 1024 * 1024) {
    return {};
  }
  auto file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return {};
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  const auto read = fread(bytes.data(), 1, bytes.size(), file);
  fclose(file);
  return read == bytes.size() ? bytes : std::vector<uint8_t>{};
}

static std::string LibraryFileName(const Emulator::LibraryTitle& title) {
  return CatalogVariantFileName(title);
}

static std::optional<Emulator::LibraryTitle> FindLibraryTitle(
    Emulator* emulator, const XCONTENT_AGGREGATE_DATA& data) {
  if (!emulator ||
      data.device_id != static_cast<uint32_t>(DummyDeviceId::HDD)) {
    return std::nullopt;
  }
  const auto parsed = ParseCatalogVariantFileName(data.file_name());
  if (!parsed) {
    return std::nullopt;
  }
  const auto titles = emulator->LibraryTitles();
  if (!titles) {
    return std::nullopt;
  }
  const auto index = UniqueCatalogVariantIndex(*titles, *parsed);
  if (!index || !CatalogVariantMatchesPhysicalFile((*titles)[*index],
                                                   (*titles)[*index].path)) {
    return std::nullopt;
  }
  return (*titles)[*index];
}

X_RESULT ContentManager::ResolvePackagePath(const uint64_t xuid,
                                            const XCONTENT_AGGREGATE_DATA& data,
                                            std::filesystem::path& path,
                                            std::string* source_id,
                                            ContentSourceState* retained) {
  if (source_id) {
    source_id->clear();
  }

  if (data.title_id == kCurrentlyRunningTitleId && !kernel_state_) {
    return X_ERROR_NOT_SUPPORTED;
  }
  if (IsDiscContent(data)) {
    return X_ERROR_ACCESS_DENIED;
  }
  std::string file_name;
  uint64_t used_xuid = 0;
  if (const X_RESULT result =
          ResolveStoredPackageName(xuid, data, file_name, used_xuid)) {
    return result;
  }
  const uint32_t device_id = data.device_id ? uint32_t(data.device_id) : 1;
  const auto writable = sources_.WritableSource(device_id);
  if (!writable) {
    const auto sources = sources_.Sources(device_id);
    // An unplugged writable device stays disconnected even when its readonly
    // overlay remains present. A device configured only for reads is connected
    // but cannot accept a new package.
    const bool has_writable = std::any_of(
        sources.begin(), sources.end(),
        [](const auto& source) { return !source.source.read_only; });
    if (!has_writable &&
        std::any_of(sources.begin(), sources.end(),
                    [](const auto& source) { return source.available; })) {
      return X_ERROR_ACCESS_DENIED;
    }
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto get_package_path = [&](uint32_t title_id) {
    if (title_id == kCurrentlyRunningTitleId) {
      title_id = kernel_state_->title_id();
    }
    const auto relative = xe::to_path(
        fmt::format("{:016X}/{:08X}/{:08X}/{}", used_xuid, title_id,
                    static_cast<uint32_t>(data.content_type.get()), file_name));
    return retained ? sources_.Resolve(*writable, relative, true)
                    : sources_.Resolve(writable->source.id, relative, true);
  };
  if (data.content_type == XContentType::kPublisher) {
    for (const auto title_id :
         FindPublisherTitleIds(xuid, data.title_id, device_id)) {
      auto candidate = get_package_path(title_id);
      std::error_code ec;
      if (candidate && std::filesystem::exists(*candidate, ec) && !ec) {
        path = std::move(*candidate);
        if (retained) { *retained = *writable; }
        if (source_id) {
          *source_id = writable->source.id;
        }
        return X_ERROR_SUCCESS;
      }
    }
  }
  auto candidate = get_package_path(data.title_id);
  if (!candidate) {
    const auto state = sources_.Source(writable->source.id);
    return !state || !state->available ? X_ERROR_DEVICE_NOT_CONNECTED
                                       : X_ERROR_ACCESS_DENIED;
  }
  path = std::move(*candidate);
  if (retained) { *retained = *writable; }
  if (source_id) {
    *source_id = writable->source.id;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ResolveWritePath(uint64_t xuid,
                                          const XCONTENT_AGGREGATE_DATA& data,
                                          std::filesystem::path& path,
                                          std::string* source_id,
                                          ContentSourceState* retained) {
  if (source_id) {
    source_id->clear();
  }
  if (IsDiscContent(data) ||
      FindLibraryTitle(kernel_state_ ? kernel_state_->emulator() : nullptr,
                       data)) {
    return X_ERROR_ACCESS_DENIED;
  }
  bool read_only = false;
  std::filesystem::path existing;
  std::string selected_source;
  const auto read_result = ResolveReadPath(xuid, data, existing, nullptr,
                                           &read_only, &selected_source, retained);
  if (!read_result) {
    if (read_only) {
      return X_ERROR_ACCESS_DENIED;
    }
    // Revalidate the selected physical source, rather than redirecting this
    // operation to a different writable package with the same logical name.
    auto checked = retained
        ? sources_.Resolve(*retained, existing.lexically_relative(retained->storage->path()), true)
        : ResolveSourcePath(existing, existing, true, selected_source);
    if (!checked) {
      return X_ERROR_ACCESS_DENIED;
    }
    path = std::move(*checked);
    if (source_id) {
      *source_id = std::move(selected_source);
    }
    return X_ERROR_SUCCESS;
  }
  return ResolvePackagePath(xuid, data, path, source_id, retained);
}

X_RESULT ContentManager::CheckPackageName(const uint64_t xuid,
                                          const XCONTENT_AGGREGATE_DATA& data) {
  std::filesystem::path path;
  return ResolvePackagePath(xuid, data, path);
}

static std::filesystem::path PackageHeaderPath(
    const std::filesystem::path& package_path) {
  // Header path:
  // content_root/xuid/title_id/Headers/content_type/package_name.header
  const auto content_type_path = package_path.parent_path();
  return content_type_path.parent_path() / kGameContentHeaderDirName /
         content_type_path.filename() /
         package_path.filename().concat(".header");
}

std::optional<std::filesystem::path> ContentManager::ResolveSourcePath(
    const std::filesystem::path& package_path,
    const std::filesystem::path& target_path, bool for_write,
    std::string_view source_id) const {
  std::error_code ec;
  const auto package = std::filesystem::absolute(package_path, ec);
  if (ec) {
    return std::nullopt;
  }
  const auto target = std::filesystem::absolute(target_path, ec);
  if (ec) {
    return std::nullopt;
  }
  std::vector<ContentSourceState> candidates;
  if (!source_id.empty()) {
    if (auto source = sources_.Source(std::string(source_id))) {
      candidates.push_back(std::move(*source));
    }
  } else {
    for (const auto device_id : sources_.DeviceIds()) {
      auto sources = sources_.Sources(device_id);
      candidates.insert(candidates.end(), sources.begin(), sources.end());
    }
  }
  for (const auto& state : candidates) {
    if (!state.available || (for_write && state.source.read_only)) {
      continue;
    }
    // Registry resolution returns canonical paths. Accept those as well as
    // the registered spelling when the source root itself is a symlink.
    const auto canonical_root =
        std::filesystem::canonical(state.source.root, ec);
    if (ec) {
      continue;
    }
    for (const auto& root : {state.source.root, canonical_root}) {
      const auto relative_package = package.lexically_relative(root);
      const auto checked_package =
          sources_.Resolve(state.source.id, relative_package, for_write);
      if (!checked_package) {
        continue;
      }
      return sources_.Resolve(state.source.id, target.lexically_relative(root),
                              for_write);
    }
  }
  return std::nullopt;
}

bool ContentManager::ReadPhysicalContentData(
    const std::filesystem::path& package_path, uint64_t xuid, uint32_t title_id,
    XContentType content_type, XCONTENT_AGGREGATE_DATA& data,
    std::string_view source_id) const {
  const auto file_name = xe::path_to_utf8(package_path.filename());
  if (file_name.size() > sizeof(data.file_name_raw) ||
      CheckFileName(file_name)) {
    return false;
  }
  const auto checked =
      ResolveSourcePath(package_path, package_path, false, source_id);
  std::error_code ec;
  if (!checked || !std::filesystem::exists(*checked, ec) || ec) {
    return false;
  }
  data = {};
  if (std::filesystem::is_directory(*checked, ec)) {
    XCONTENT_AGGREGATE_DATA stored{};
    if (XSUCCEEDED(ReadContentHeaderFileFromSource(
            package_path, xuid, title_id, content_type, stored, source_id))) {
      data = stored;
      return true;
    }
  } else {
    // Validate before reserving this name in the merged catalog. A corrupt
    // higher-priority file must not hide a valid lower-priority package.
    auto header = vfs::XContentContainerDevice::ReadContainerHeader(*checked);
    if (!header || !header->content_header.is_magic_valid()) {
      return false;
    }
    TerminateMetadataStrings(header->content_metadata);
    data = vfs::XContentContainerDevice::ContentDataFromHeader(*header);
  }
  data.content_type = content_type;
  data.title_id = title_id;
  data.xuid = xuid;
  data.set_file_name(file_name);
  if (data.display_name().empty()) {
    data.set_display_name(xe::path_to_utf16(package_path.filename()));
  }
  return true;
}

std::unordered_set<uint32_t> ContentManager::FindPublisherTitleIds(
    const uint64_t xuid, uint32_t base_title_id, uint32_t device_id) const {
  if (!ResolveTitleId(base_title_id)) {
    return {};
  }
  std::unordered_set<uint32_t> title_ids;
  for (const uint32_t title_id : ListContentTitleIds(device_id, xuid)) {
    if (title_id == base_title_id ||
        (title_id >> 16) != (base_title_id >> 16)) {
      continue;
    }
    const auto types = ListContentTypes(device_id, xuid, title_id);
    if (std::find(types.begin(), types.end(), XContentType::kPublisher) !=
        types.end()) {
      title_ids.insert(title_id);
    }
  }
  return title_ids;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(
    const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId && !kernel_state_) {
    return {};
  }

  std::vector<XCONTENT_AGGREGATE_DATA> result;
  std::unordered_set<string_key_insensitive> seen;

  std::unordered_set<uint32_t> title_ids = {title_id};

  if (content_type == XContentType::kPublisher) {
    title_ids = FindPublisherTitleIds(xuid, title_id, device_id);
  }

  for (const uint32_t& title_id : title_ids) {
    // Search path:
    // content_root/xuid/title_id/type_name/*
    for (const auto& package_root :
         ReadPackageRoots(device_id, xuid, title_id, content_type)) {
      auto file_infos = xe::filesystem::ListFiles(package_root.path);

      for (const auto& file_info : file_infos) {
        // An SVOD package (Games on Demand) keeps its data in a folder named
        // after the package file with .data added, which is part of it.
        if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory &&
            file_info.name.extension() == ".data" &&
            std::filesystem::is_regular_file(file_info.path /
                                             file_info.name.stem())) {
          continue;
        }
        XCONTENT_AGGREGATE_DATA content_data{};
        if (!ReadPhysicalContentData(file_info.path / file_info.name, xuid,
                                     title_id, content_type, content_data,
                                     package_root.source_id)) {
          continue;
        }
        // Only valid records claim a name. Opening uses the same physical
        // validation and source order, including fallback past corrupt files.
        if (!seen.insert(string_key_insensitive::create(fmt::format(
                             "{:08X}:{}", title_id, content_data.file_name())))
                 .second) {
          continue;
        }
        content_data.device_id = device_id;
        result.emplace_back(std::move(content_data));
      }
    }
  }
  return result;
}

std::vector<uint32_t> ContentManager::ListHexFolders(
    const uint32_t device_id, const uint64_t xuid,
    const std::optional<uint32_t> title_id) const {
  if ((device_id == static_cast<uint32_t>(DummyDeviceId::ODD) ||
       title_id == kCurrentlyRunningTitleId) &&
      !kernel_state_) {
    return {};
  }

  std::vector<std::string> names;
  if (device_id == static_cast<uint32_t>(DummyDeviceId::ODD)) {
    std::string path = fmt::format("{}\\content\\{:016X}",
                                   kernel_state_->title_mount_path_, xuid);
    if (title_id) {
      path += fmt::format("\\{:08X}", *title_id);
    }
    if (const vfs::Entry* entry =
            kernel_state_->file_system()->ResolvePath(path)) {
      for (const auto& child : entry->children()) {
        if (child->attributes() & vfs::kFileAttributeDirectory) {
          names.push_back(child->name());
        }
      }
    }
  } else {
    for (const auto& source : sources_.Sources(device_id)) {
      auto relative = xe::to_path(fmt::format("{:016X}", xuid));
      if (title_id) {
        relative /= fmt::format("{:08X}", *title_id);
      }
      const auto path = sources_.Resolve(source.source.id, relative, false);
      if (!path) {
        continue;
      }
      for (const auto& file_info : xe::filesystem::ListFiles(*path)) {
        if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory &&
            sources_.Resolve(source.source.id, relative / file_info.name,
                             false)) {
          names.push_back(xe::path_to_utf8(file_info.name));
        }
      }
    }
  }
  std::vector<uint32_t> result;
  for (const std::string& name : names) {
    if (name.size() == 8 && std::ranges::all_of(name, [](char c) {
          return std::isxdigit(static_cast<unsigned char>(c));
        })) {
      result.push_back(xe::string_util::from_string<uint32_t>(name, true));
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<uint64_t> ContentManager::ListContentXuids(uint32_t device_id) const {
  std::vector<uint64_t> result;
  for (const auto& source : sources_.Sources(device_id)) {
    if (!source.available) {
      continue;
    }
    for (const auto& file_info :
         xe::filesystem::ListFiles(source.source.root)) {
      const std::string name = xe::path_to_utf8(file_info.name);
      if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory &&
          name.size() == 16 &&
          std::ranges::all_of(name,
                              [](char c) {
                                return std::isxdigit(
                                    static_cast<unsigned char>(c));
                              }) &&
          sources_.Resolve(source.source.id, file_info.name, false)) {
        result.push_back(xe::string_util::from_string<uint64_t>(name, true));
      }
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<uint32_t> ContentManager::ListContentTitleIds(
    const uint32_t device_id, const uint64_t xuid) const {
  return ListHexFolders(device_id, xuid, std::nullopt);
}

std::vector<XContentType> ContentManager::ListContentTypes(
    const uint32_t device_id, const uint64_t xuid,
    const uint32_t title_id) const {
  std::vector<XContentType> result;
  for (const uint32_t type : ListHexFolders(device_id, xuid, title_id)) {
    result.push_back(static_cast<XContentType>(type));
  }
  return result;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContentODD(
    const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type) const {
  if (!kernel_state_) {
    return {};
  }
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  auto entry = kernel_state_->file_system()->ResolvePath(
      ResolveDiscPackageRoot(xuid, title_id, content_type));

  if (!entry) {
    return {};
  }

  for (const auto& child : entry->children()) {
    XCONTENT_AGGREGATE_DATA content_data{};

    content_data.device_id = device_id;
    content_data.content_type = content_type;
    content_data.set_display_name(xe::path_to_utf16(child->name()));
    content_data.set_file_name(xe::path_to_utf8(child->name()));
    content_data.title_id = title_id;
    content_data.xuid = xuid;
    result.emplace_back(std::move(content_data));
  }

  return result;
}

std::string ContentManager::ResolveDiscPackageRoot(
    const uint64_t xuid, uint32_t title_id,
    const XContentType content_type) const {
  if (!kernel_state_) {
    return {};
  }
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }
  return fmt::format("{}\\content\\{:016X}\\{:08X}\\{:08X}",
                     kernel_state_->title_mount_path_, xuid, title_id,
                     static_cast<uint32_t>(content_type));
}

vfs::Entry* ContentManager::ResolveDiscPackage(
    const XCONTENT_AGGREGATE_DATA& data) const {
  if (!kernel_state_) {
    return nullptr;
  }
  std::string file_name;
  uint64_t used_xuid = 0;
  if (ResolveStoredPackageName(0, data, file_name, used_xuid)) {
    return nullptr;
  }
  // A disc holds no user's content, only XUID 0's.
  return kernel_state_->file_system()->ResolvePath(xe::utf8::join_guest_paths(
      ResolveDiscPackageRoot(0, data.title_id, data.content_type), file_name));
}

X_RESULT ContentManager::SelectPackageAt(PackageSelection& selected,
    std::filesystem::path path, bool library, bool read_only,
    std::optional<ContentSourceState> source) {
  std::error_code canonical_error;
  path = std::filesystem::canonical(path, canonical_error);
  if (canonical_error) { return X_ERROR_PATH_NOT_FOUND; }
  selected.path = std::move(path); selected.library = library;
  selected.read_only = read_only; selected.source = std::move(source);
  if (selected.claim && selected.claim->path != selected.path.lexically_normal()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!selected.claim) { selected.claim = ClaimPackagePath(selected.path, false); }
  if (!selected.claim) { return X_ERROR_SHARING_VIOLATION; }
  std::error_code error;
  const bool directory = std::filesystem::is_directory(selected.path, error);
  if (error) { return X_ERROR_PATH_NOT_FOUND; }
  if (selected.source) {
    const auto relative = selected.path.lexically_relative(selected.source->storage->path());
    if (sources_.Resolve(*selected.source, relative, false) != selected.path) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (directory) {
      selected.directory = selected.source->storage->Subroot(relative, read_only, error);
    } else {
      selected.file = selected.source->storage->OpenFile(
          relative, filesystem::FileAccess::kFileReadData, error);
    }
  } else if (library) {
    if (directory) {
      selected.directory = filesystem::HostStorageRoot::Open(selected.path, true, error, 0);
    } else {
      selected.file = filesystem::FileHandle::OpenExisting(
          selected.path, filesystem::FileAccess::kFileReadData);
      if (!selected.file) { error = std::make_error_code(std::errc::io_error); }
    }
  } else { return X_ERROR_DEVICE_NOT_CONNECTED; }
  if (error || (directory ? !selected.directory : !selected.file)) { return X_ERROR_ACCESS_DENIED; }
  if (selected.file) {
    selected.file_info = selected.file->QueryInfo(error);
    if (error || !selected.file_info || !selected.file_info->regular) { return X_ERROR_ACCESS_DENIED; }
  }
  return ValidatePackageSelection(selected) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}
std::unique_ptr<ContentManager::PackageSelection> ContentManager::CapturePackageSelection(
    uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data, X_RESULT& result) {
  auto selected = std::make_unique<PackageSelection>();
  std::filesystem::path path; bool library = false, read_only = false;
  ContentSourceState source;
  result = ResolveReadPath(xuid, data, path, &library, &read_only, nullptr, &source,
                           selected.get());
  if (result) { return {}; }
  return selected;
}
bool ContentManager::ValidatePackageSelection(const PackageSelection& selected) const {
  if (!selected.claim) { return false; }
  if (selected.source) {
    const auto relative = selected.path.lexically_relative(selected.source->storage->path());
    if (sources_.Resolve(*selected.source, relative, false) != selected.path) { return false; }
  }
  if (selected.directory) {
    return selected.directory->path() == selected.path && selected.directory->Observe().available;
  }
  if (!selected.file_info) { return false; }
  std::error_code error;
  auto actual = selected.source
      ? selected.source->storage->OpenFile(
          selected.path.lexically_relative(selected.source->storage->path()),
          filesystem::FileAccess::kFileReadData, error)
      : filesystem::FileHandle::OpenExisting(selected.path, filesystem::FileAccess::kFileReadData);
  if (!actual || error) { return false; }
  const auto info = actual->QueryInfo(error);
  return info && !error && info->regular &&
      info->filesystem_id == selected.file_info->filesystem_id &&
      info->file_id == selected.file_info->file_id && info->size == selected.file_info->size;
}
std::unique_ptr<ContentPackage> ContentManager::MountPackageSelection(
    std::string_view root_name, const XCONTENT_AGGREGATE_DATA& data,
    PackageSelection& selected, uint32_t disc_number) {
  if (!kernel_state_ || !ValidatePackageSelection(selected)) { return {}; }
  const auto root_path = disc_number != uint32_t(-1) ? fmt::format("disc{:03}", disc_number) : "";
  std::unique_ptr<ContentPackage> package;
  if (selected.directory) {
    package = std::make_unique<ContentPackage>(kernel_state_, root_name, data,
        selected.path, root_path, selected.read_only, selected.directory);
  } else {
    if (!selected.file || !selected.file_info) { return {}; }
    auto backing = vfs::XContentReadBacking::FromFileHandle(std::move(selected.file),
        std::vector<filesystem::GuestHostBacking>{{selected.path, false, *selected.file_info}}, true);
    const auto header = backing ? vfs::XContentContainerDevice::ReadContainerHeader(*backing) : nullptr;
    if (!header || !header->content_header.is_magic_valid()) { return {}; }
    if (header->content_metadata.volume_type == vfs::XContentVolumeType::kStfs) {
      // Direct new is in this friend; make_unique cannot access private ctor.
      package.reset(new ContentPackage(kernel_state_, root_name, data, selected.path,
          root_path, std::move(backing)));
    } else if (header->content_metadata.volume_type == vfs::XContentVolumeType::kSvod) {
      // Preserve the real fragmented SVOD reader. The manager claim excludes
      // cooperating package mutation, but this lane does NOT claim an immutable
      // FD snapshot of all fragments or defeat adversarial external host swaps.
      package = std::make_unique<ContentPackage>(kernel_state_, root_name, data,
          selected.path, root_path, selected.read_only);
      package->validation_backing_ = std::move(backing);
    } else { return {}; }
  }
  package->source_ = selected.source;
  package->package_admission_ = selected.claim;
  if (!package->is_mounted() || !ValidatePackageSelection(selected)) { return {}; }
  return package;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    std::string_view root_name, uint64_t xuid,
    const XCONTENT_AGGREGATE_DATA& data, uint32_t disc_number) {
  if (!kernel_state_) { return {}; }
  X_RESULT result = X_ERROR_SUCCESS;
  auto selected = CapturePackageSelection(xuid, data, result);
  return selected ? MountPackageSelection(root_name, data, *selected, disc_number) : nullptr;
}

bool ContentManager::ContentExists(const uint64_t xuid,
                                   const XCONTENT_AGGREGATE_DATA& data) {
  if (IsDiscContent(data)) {
    return ResolveDiscPackage(data) != nullptr;
  }
  std::filesystem::path path;
  std::error_code ec;
  return !ResolveReadPath(xuid, data, path) &&
         std::filesystem::exists(path, ec);
}

static X_RESULT ContentMutationError(const std::error_code& error) {
  if (error == std::errc::operation_canceled) { return X_ERROR_CANCELLED; }
  return xboxkrnl::xeRtlNtStatusToDosError(vfs::HostStorageError(error));
}

X_RESULT ContentManager::WriteContentHeaderFile(uint64_t xuid,
                                                XCONTENT_AGGREGATE_DATA data) {
  return PublishContentHeader(xuid, std::move(data), false);
}

X_RESULT ContentManager::PublishContentHeader(uint64_t xuid,
                                               XCONTENT_AGGREGATE_DATA data,
                                               bool create, PackageSelection* created_selection,
                                               std::span<const filesystem::ContentPackageInitialMember> initial_members) {
  // Only host-operation context may enter the durable owner. No filesystem
  // observation, space admission or callbacks under GCRC.
  if (global_critical_region::is_held_by_current_thread()) {
    return X_ERROR_BUSY;
  }
  uint32_t title_id = data.title_id;
  if (!ResolveTitleId(title_id)) { return X_ERROR_NOT_SUPPORTED; }
  data.title_id = title_id;
  std::string requested_name;
  uint64_t physical_xuid = 0;
  if (const auto result = ResolveStoredPackageName(xuid, data, requested_name, physical_xuid)) {
    return result;
  }
  std::filesystem::path package_path;
  std::string source_id;
  ContentSourceState captured_source;
  if (const auto result = ResolveWritePath(xuid, data, package_path, &source_id, &captured_source)) {
    return result;
  }
  // Retain the exact source selected by resolution; never choose another
  // writable device after discovering installed/read-only content.
  const auto source = std::optional<ContentSourceState>(std::move(captured_source));
  if (!source || !source->available || !source->storage || !source->volume ||
      source->source.kind != ContentSourceKind::kManaged || source->source.read_only ||
      source->volume->identity() != source->managed_identity ||
      !source->volume->mutations()) { return X_ERROR_ACCESS_DENIED; }
  const auto checked = sources_.Resolve(*source,
      PackageHeaderPath(package_path).lexically_relative(source->storage->path()), true);
  if (!checked) { return X_ERROR_ACCESS_DENIED; }
  const auto package = package_path.lexically_relative(source->storage->path());
  const auto target = checked->lexically_relative(source->storage->path());
  auto owner = source->volume->mutations();
  using Kind = filesystem::ContentMutationOwner::Kind;
  std::error_code error;
  const auto expected = owner->CaptureMetadata(create ? Kind::kCreate : Kind::kHeader,
                                               package, target, error);
  if (!expected) { return ContentMutationError(error); }
  data.device_id = source->source.device_id;
  data.xuid = physical_xuid;
  data.set_file_name(xe::path_to_utf8(package_path.filename()));
  std::vector<uint8_t> bytes;
  if (expected->exists()) {
    const auto old = expected->bytes();
    if (old.size() >= 4 &&
        (std::memcmp(old.data(), "CON ", 4) == 0 ||
         std::memcmp(old.data(), "PIRS", 4) == 0 ||
         std::memcmp(old.data(), "LIVE", 4) == 0)) {
      return X_ERROR_NOT_SUPPORTED;  // A real container needs its actual writer.
    }
    if (old.size() < sizeof(data)) { return xboxkrnl::xeRtlNtStatusToDosError(X_STATUS_END_OF_FILE); }
    XCONTENT_AGGREGATE_DATA existing{};
    std::memcpy(&existing, old.data(), sizeof(existing));
    std::string name; uint64_t owner_xuid = 0;
    if (existing.content_type != data.content_type ||
        (existing.title_id && existing.title_id != title_id) ||
        (existing.xuid && existing.xuid != data.xuid) ||
        existing.display_name_raw.uint[127] ||
        ResolveStoredPackageName(physical_xuid, existing, name, owner_xuid) ||
        owner_xuid != physical_xuid || !xe::utf8::equal_case(name, data.file_name())) {
      return X_ERROR_FILE_CORRUPT;
    }
    bytes.assign(old.begin(), old.end());
  } else { bytes.resize(sizeof(data), 0); }
  // Only declared fields change. Preserve padding/alignment bytes, appended
  // license mask and every unknown tail byte from the complete old snapshot.
  const auto* begin = reinterpret_cast<const uint8_t*>(&data);
  auto field = [&](const auto& value) {
    const auto* address = reinterpret_cast<const uint8_t*>(&value);
    std::copy_n(address, sizeof(value), bytes.begin() + (address - begin));
  };
  field(data.device_id); field(data.content_type); field(data.display_name_raw);
  field(data.file_name_raw); field(data.xuid); field(data.title_id);
  if (created_selection) {
    created_selection->claim = ClaimPackagePath(package_path, false);
    if (!created_selection->claim) { return X_ERROR_SHARING_VIOLATION; }
  }
  const auto receipt = !initial_members.empty()
      ? owner->CreateInitial(package, expected, bytes, initial_members)
      : create ? owner->Create(package, expected, bytes)
               : owner->WriteMetadata(Kind::kHeader, package, expected, bytes);
  if (!receipt.complete) {
    return ContentMutationError(receipt.error ? receipt.error : std::make_error_code(std::errc::io_error));
  }
  if (created_selection) {
    return SelectPackageAt(*created_selection, std::move(package_path), false, false, source);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ReadContentHeaderFile(
    const std::filesystem::path& package_path, const uint64_t xuid,
    const uint32_t title_id, XContentType content_type,
    XCONTENT_AGGREGATE_DATA& data) const {
  return ReadContentHeaderFileFromSource(package_path, xuid, title_id,
                                         content_type, data, {});
}

X_RESULT ContentManager::ReadContentHeaderFileFromSource(
    const std::filesystem::path& package_path, uint64_t xuid, uint32_t title_id,
    XContentType content_type, XCONTENT_AGGREGATE_DATA& data,
    std::string_view source_id) const {
  if (!ResolveTitleId(title_id)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  const auto checked = ResolveSourcePath(
      package_path, PackageHeaderPath(package_path), false, source_id);
  if (!checked) {
    return X_STATUS_NO_SUCH_FILE;
  }
  const auto& header_file_path = *checked;
  constexpr uint32_t header_size = sizeof(XCONTENT_AGGREGATE_DATA);

  std::error_code ec;
  if (std::filesystem::exists(header_file_path, ec) && !ec) {
    auto file = xe::filesystem::OpenFile(header_file_path, "rb");
    if (!file) {
      return X_STATUS_NO_SUCH_FILE;
    }

    // Upstream canary rewrites the header file as the whole container header,
    // followed by the file name the save was created with.
    auto header = vfs::XContentContainerDevice::ReadContainerHeader(file);
    if (header && header->content_header.is_magic_valid()) {
      fclose(file);
      TerminateMetadataStrings(header->content_metadata);
      data = vfs::XContentContainerDevice::ContentDataFromHeader(*header);
      // Identify the package by its location, as listing without a header does.
      data.content_type = content_type;
      data.title_id = title_id;
      data.xuid = xuid;
      const std::string file_name = xe::path_to_utf8(package_path.filename());
      // Some headers append the original save name. The physical leaf is the
      // reopen/delete identity even when that optional name differs.
      data.set_file_name(file_name);
      return X_STATUS_SUCCESS;
    }
    fseek(file, 0, SEEK_SET);

    std::array<uint8_t, header_size> buffer;

    const auto file_size = std::filesystem::file_size(header_file_path, ec);
    if (ec || file_size < header_size) {
      fclose(file);
      return X_STATUS_END_OF_FILE;
    }

    size_t result = fread(buffer.data(), 1, header_size, file);
    if (result != header_size) {
      fclose(file);
      return X_STATUS_END_OF_FILE;
    }

    fclose(file);
    std::memcpy(&data, buffer.data(), buffer.size());
    // Legacy loose-content metadata has no magic. Validate its meaningful
    // fields before exposing it as a different package or reading its strings.
    // Older writers omitted the owning XUID, so zero remains an unknown value,
    // not a reason to reassign the profile or declare its payload corrupt.
    if (data.content_type != content_type || !data.file_name_raw[0] ||
        data.display_name_raw.uint[127] != 0 ||
        (data.title_id && data.title_id != title_id) ||
        (data.xuid && data.xuid != xuid)) {
      return X_STATUS_UNSUCCESSFUL;
    }
    std::string stored_name;
    uint64_t stored_xuid = 0;
    const auto physical_name = xe::path_to_utf8(package_path.filename());
    if (ResolveStoredPackageName(xuid, data, stored_name, stored_xuid) ||
        stored_xuid != xuid ||
        !xe::utf8::equal_case(stored_name, physical_name)) {
      return X_STATUS_UNSUCCESSFUL;
    }
    data.content_type = content_type;
    data.title_id = title_id;
    data.xuid = xuid;
    data.set_file_name(physical_name);
    return X_STATUS_SUCCESS;
  }
  return X_STATUS_NO_SUCH_FILE;
}

X_RESULT ContentManager::CreatePackage(uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  return PublishContentHeader(xuid, data, true);
}

X_RESULT ContentManager::CreateInitialPackage(
    uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
    std::span<const filesystem::ContentPackageInitialMember> members) {
  if (members.empty()) { return X_ERROR_INVALID_PARAMETER; }
  return PublishContentHeader(xuid, data, true, nullptr, members);
}

X_RESULT ContentManager::CreateContent(std::string_view root_name,
                                       uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  if (!kernel_state_) { return X_ERROR_NOT_SUPPORTED; }
  if (global_critical_region::is_held_by_current_thread()) { return X_ERROR_BUSY; }
  auto claim = ClaimRootName(root_name);
  if (!claim) { return X_ERROR_ALREADY_EXISTS; }
  PackageSelection selected;
  if (const auto result = PublishContentHeader(xuid, data, true, &selected)) { return result; }
  auto package = MountPackageSelection(root_name, data, selected, uint32_t(-1));
  if (!package) { return X_ERROR_ACCESS_DENIED; }
  {
    auto lock = global_critical_region_.Acquire();
    if (open_packages_.contains(claim->key) ||
        (package->source_ && package->source_->storage->retiring())) { return X_ERROR_DEVICE_NOT_CONNECTED; }
    open_packages_.emplace(claim->key, package.release());
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(const std::string_view root_name,
                                     const uint64_t xuid,
                                     const XCONTENT_AGGREGATE_DATA& data,
                                     uint32_t& content_license,
                                     const uint32_t disc_number) {
  if (!kernel_state_) {
    return X_ERROR_NOT_SUPPORTED;
  }
  if (global_critical_region::is_held_by_current_thread()) { return X_ERROR_BUSY; }
  auto claim = ClaimRootName(root_name);
  if (!claim) { return X_ERROR_ALREADY_EXISTS; }

  std::unique_ptr<PackageSelection> selected;
  std::unique_ptr<ContentPackage> package;
  if (IsDiscContent(data)) {
    // Existing readonly borrowed-entry lane retains its namespace protection;
    // this cohort does not invent an independent mutable Entry lifetime.
    auto disc_lock = global_critical_region_.Acquire();
    vfs::Entry* package_entry = ResolveDiscPackage(data);
    if (!package_entry) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = std::make_unique<ContentPackage>(kernel_state_, root_name, data,
                                               package_entry);
    if (!package->is_mounted()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
  } else {
    X_RESULT result = X_ERROR_SUCCESS;
    selected = CapturePackageSelection(xuid, data, result);
    if (!selected) { return result; }
    package = MountPackageSelection(root_name, data, *selected, disc_number);
    if (!package) { return X_ERROR_FILE_NOT_FOUND; }
    // The header/license belongs to the SAME retained physical selection.
    if (!selected->library && selected->source) {
      const auto header = sources_.Resolve(*selected->source,
          PackageHeaderPath(selected->path).lexically_relative(selected->source->storage->path()), false);
      if (header) { package->LoadPackageLicenseMask(*header); }
    }
    if (!ValidatePackageSelection(*selected)) { return X_ERROR_DEVICE_NOT_CONNECTED; }
  }

  content_license = package->GetPackageLicense();
  if (static_cast<uint32_t>(cvars::license_mask) > 0) {
    content_license |= cvars::license_mask;
  }

  // Check for SPA file in package. Check it only for DLCs
  if (data.content_type == XContentType::kMarketplaceContent) {
    const std::string spa_path = fmt::format("{}:\\{}", root_name, kSpaFilename);
    UpdateSpaData(spa_path);
  }

  {
    auto lock = global_critical_region_.Acquire();
    if (open_packages_.contains(claim->key) ||
        (package->source_ && package->source_->storage->retiring())) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    open_packages_.emplace(claim->key, package.release());
  }

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContentFile(const std::string_view root_name,
                                         vfs::Entry* package_entry) {
  if (!kernel_state_) {
    return X_ERROR_NOT_SUPPORTED;
  }
  auto global_lock = global_critical_region_.Acquire();

  if (open_packages_.count(string_key_insensitive(root_name))) {
    return X_ERROR_ALREADY_EXISTS;
  }

  auto package = std::make_unique<ContentPackage>(
      kernel_state_, root_name, XCONTENT_AGGREGATE_DATA(), package_entry);
  if (!package->is_mounted()) {
    return X_ERROR_FILE_CORRUPT;
  }

  open_packages_.insert(
      {string_key_insensitive::create(root_name), package.release()});
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  if (!kernel_state_) {
    return X_ERROR_NOT_SUPPORTED;
  }
  auto global_lock = global_critical_region_.Acquire();

  // 415607D6 - Uses XamContentCreate with name "save", but XamContentClose with
  // "SAVE".
  auto it = open_packages_.find(string_key_insensitive(root_name));
  if (it == open_packages_.end()) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  ClosePackage(it->second);
  return X_ERROR_SUCCESS;
}

void ContentManager::ClosePackage(ContentPackage* package) {
  if (!kernel_state_ || !package) {
    return;
  }
  // Packages mounted from files in this package read through this package's
  // device.
  ClosePackagesMountedFrom(
      kernel_state_->file_system()->GetDevice(package->device_path()));
  CloseOpenedFilesFromContent(*package);
  std::erase_if(open_packages_,
                [package](const auto& open) { return open.second == package; });
  delete package;
}

void ContentManager::ClosePackagesMountedFrom(const vfs::Device* device) {
  if (!kernel_state_) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  if (!device) {
    return;
  }
  std::vector<std::pair<std::string, ContentPackage*>> mounted;
  for (const auto& open : open_packages_) {
    auto container = dynamic_cast<const vfs::XContentContainerDevice*>(
        kernel_state_->file_system()->GetDevice(open.second->device_path()));
    const vfs::Entry* source = container ? container->package_entry() : nullptr;
    if (source && source->device() == device) {
      mounted.emplace_back(std::string(open.first.view()), open.second);
    }
  }
  for (const auto& [root_name, package] : mounted) {
    XELOGW("Closing {}: the device it was mounted from is going away",
           root_name);
    ClosePackage(package);
  }
}

X_RESULT ContentManager::GetContentThumbnail(
    const uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
    std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();
  if (const auto title = FindLibraryTitle(
          kernel_state_ ? kernel_state_->emulator() : nullptr, data)) {
    *buffer = ReadCatalogIcon(title->icon_path);
    return buffer->empty() ? X_ERROR_FILE_NOT_FOUND : X_ERROR_SUCCESS;
  }

  std::filesystem::path package_path;
  std::string source_id;
  if (const X_RESULT result = ResolveReadPath(xuid, data, package_path, nullptr,
                                              nullptr, &source_id)) {
    return result;
  }
  const auto thumb_path = ResolveSourcePath(
      package_path, package_path / vfs::kPackageThumbnailFileName, false,
      source_id);
  std::error_code ec;
  if (thumb_path && std::filesystem::is_regular_file(*thumb_path, ec)) {
    auto file = xe::filesystem::OpenFile(*thumb_path, "rb");
    if (!file) {
      return X_ERROR_ACCESS_DENIED;
    }
    size_t file_len = std::filesystem::file_size(*thumb_path, ec);
    if (ec) {
      fclose(file);
      return X_ERROR_ACCESS_DENIED;
    }
    buffer->resize(file_len);
    const auto read = fread(buffer->data(), 1, buffer->size(), file);
    fclose(file);
    return read == buffer->size() ? X_ERROR_SUCCESS : X_ERROR_ACCESS_DENIED;
  }

  // A deleted package leaves its header behind.
  if (!std::filesystem::exists(package_path, ec) || ec) {
    return X_ERROR_PATH_NOT_FOUND;
  }

  // Upstream canary moves the thumbnail into the header file.
  const auto header_path = ResolveSourcePath(
      package_path, PackageHeaderPath(package_path), false, source_id);
  if (!header_path) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  auto header = vfs::XContentContainerDevice::ReadContainerHeader(*header_path);
  if (!header || !header->content_header.is_magic_valid()) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  const auto& metadata = header->content_metadata;
  const uint32_t max_thumbnail_size =
      metadata.metadata_version >= 2 ? vfs::XContentMetadata::kThumbLengthV2
                                     : vfs::XContentMetadata::kThumbLengthV1;
  if (metadata.thumbnail_size == 0 ||
      metadata.thumbnail_size > max_thumbnail_size) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  buffer->assign(metadata.thumbnail,
                 metadata.thumbnail + metadata.thumbnail_size);
  return X_ERROR_SUCCESS;
}

// A disc image or bare executable has no type of its own and counts as a
// title.
static XContentType LibraryContentType(const Emulator::LibraryTitle& title) {
  return title.content_type == XContentType::kInvalid
             ? XContentType::kXbox360Title
             : title.content_type;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListLibraryContent()
    const {
  if (!kernel_state_) {
    return {};
  }
  std::vector<XCONTENT_AGGREGATE_DATA> content;
  const auto titles = kernel_state_->emulator()->LibraryTitles();
  if (!titles) {
    return content;
  }
  for (const auto index : PreferredCatalogVariants(*titles)) {
    const auto &title = (*titles)[index];
    // A release without a name, like a loose executable, isn't listed.
    if (title.name.empty() || !title.available || !title.metadata_valid ||
        !UniqueCatalogVariantIndex(
            *titles, CatalogVariantName{title.title_id, title.version,
                                        title.variant_id})) {
      continue;
    }
    XCONTENT_AGGREGATE_DATA data{};
    data.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
    data.content_type = LibraryContentType(title);
    data.set_display_name(xe::to_utf16(title.name));
    data.set_file_name(LibraryFileName(title));
    data.title_id = title.title_id;
    content.push_back(data);
  }
  return content;
}

std::optional<std::filesystem::path> ContentManager::LibraryContentPath(
    const XCONTENT_AGGREGATE_DATA& data) const {
  if (auto title = FindLibraryTitle(
          kernel_state_ ? kernel_state_->emulator() : nullptr, data)) {
    return std::move(title->path);
  }
  return std::nullopt;
}

std::optional<TitleCatalogVariant>
ContentManager::LibraryContentVariant(uint64_t xuid,
                                      const XCONTENT_AGGREGATE_DATA &data) {
  if (auto synthetic = FindLibraryTitle(
          kernel_state_ ? kernel_state_->emulator() : nullptr, data)) {
    return synthetic;
  }
  if (!kernel_state_) {
    return std::nullopt;
  }
  const auto titles = kernel_state_->emulator()->LibraryTitles();
  std::filesystem::path physical;
  if (!titles || ResolveReadPath(xuid, data, physical)) {
    return std::nullopt;
  }
  for (const auto &variant : *titles) {
    if (CatalogVariantMatchesPhysicalFile(variant, physical)) {
      return variant;
    }
  }
  return std::nullopt;
}

bool ContentManager::LibraryContainsPhysicalContent(
    uint64_t xuid, const XCONTENT_AGGREGATE_DATA &data) {
  // Stored content is omitted only when this very file has a usable catalog
  // variant. Package type, title ID or a matching display name are
  // insufficient.
  const auto variant = LibraryContentVariant(xuid, data);
  return variant && variant->available && variant->metadata_valid;
}

X_RESULT ContentManager::ResolveGuestContentPath(
    uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data, bool for_write,
    std::string& path) {
  if (IsDiscContent(data)) {
    if (for_write) {
      return X_ERROR_ACCESS_DENIED;
    }
    const auto* entry = ResolveDiscPackage(data);
    if (!entry) {
      return X_ERROR_PATH_NOT_FOUND;
    }
    path = entry->absolute_path();
    return X_ERROR_SUCCESS;
  }
  std::filesystem::path physical;
  bool library = false;
  std::string source_id;
  const auto result = for_write
                          ? ResolveWritePath(xuid, data, physical, &source_id)
                          : ResolveReadPath(xuid, data, physical, &library,
                                            nullptr, &source_id);
  if (result) {
    return result;
  }
  if (library) {
    return X_ERROR_NOT_SUPPORTED;
  }
  const auto source = sources_.Source(source_id);
  if (!source || !source->available || source->source.guest_root.empty() ||
      (for_write && source->source.read_only)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::error_code ec;
  const auto root = std::filesystem::canonical(source->source.root, ec);
  if (ec) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const auto relative = physical.lexically_relative(root);
  const auto checked = sources_.Resolve(source_id, relative, for_write);
  if (!checked || *checked != physical) {
    return X_ERROR_PATH_NOT_FOUND;
  }
  path = xe::utf8::join_guest_paths(source->source.guest_root,
                                    xe::utf8::fix_path_separators(
                                        xe::path_to_utf8(relative), '\\'));
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ResolveReadPath(const uint64_t xuid,
                                         const XCONTENT_AGGREGATE_DATA& data,
                                         std::filesystem::path& path,
                                         bool* library, bool* read_only,
                                         std::string* source_id,
                                         ContentSourceState* retained, PackageSelection* selecting) {
  if (source_id) {
    source_id->clear();
  }

  if (data.title_id == kCurrentlyRunningTitleId && !kernel_state_) {
    return X_ERROR_NOT_SUPPORTED;
  }
  if (read_only) {
    *read_only = false;
  }
  auto title = FindLibraryTitle(
      kernel_state_ ? kernel_state_->emulator() : nullptr, data);
  if (library) {
    *library = title.has_value();
  }
  if (title) {
    if (read_only) {
      *read_only = true;
    }
    path = std::move(title->path);
    if (selecting) { return SelectPackageAt(*selecting, path, true, true, {}); }
    return X_ERROR_SUCCESS;
  }
  std::string file_name;
  uint64_t used_xuid = 0;
  if (const auto error =
          ResolveStoredPackageName(xuid, data, file_name, used_xuid)) {
    return error;
  }
  const uint32_t device_id = data.device_id ? uint32_t(data.device_id) : 1;
  uint32_t title_id = data.title_id;
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }
  std::vector<uint32_t> title_ids{title_id};
  if (data.content_type == XContentType::kPublisher) {
    const auto publisher_ids = FindPublisherTitleIds(xuid, title_id, device_id);
    title_ids.insert(title_ids.end(), publisher_ids.begin(),
                     publisher_ids.end());
  }
  bool available = false;
  const auto sources = sources_.Sources(device_id);
  for (const auto id : title_ids) {
    const auto relative = xe::to_path(
        fmt::format("{:016X}/{:08X}/{:08X}/{}", used_xuid, id,
                    static_cast<uint32_t>(data.content_type.get()), file_name));
    for (const auto& source : sources) {
      available |= source.available;
      const auto candidate = retained ? sources_.Resolve(source, relative, false)
          : sources_.Resolve(source.source.id, relative, false);
      XCONTENT_AGGREGATE_DATA physical{};
      if (!candidate) { continue; }
      std::unique_ptr<PackageSelection> pending;
      if (selecting) {
        pending = std::make_unique<PackageSelection>();
        const auto admission = SelectPackageAt(*pending, *candidate, false,
            source.source.read_only, source);
        if (admission == X_ERROR_PATH_NOT_FOUND || admission == X_ERROR_FILE_NOT_FOUND) { continue; }
        if (admission) { return admission; }
      }
      if (!ReadPhysicalContentData(*candidate, used_xuid, id, data.content_type,
                                   physical, source.source.id)) {
        if (pending && !ValidatePackageSelection(*pending)) { return X_ERROR_DEVICE_NOT_CONNECTED; }
        continue;
      }
      if (pending && !ValidatePackageSelection(*pending)) { return X_ERROR_DEVICE_NOT_CONNECTED; }
      if (retained) {
        // Legacy metadata readers are observational only; a raced replacement
        // cannot authorize mutation of a newer root after their read.
        if (sources_.Resolve(source, relative, false) != candidate) { continue; }
        *retained = source;
      }
      if (selecting) { *selecting = std::move(*pending); }
      path = *candidate;
      if (source_id) {
        *source_id = source.source.id;
      }
      if (read_only) {
        *read_only = source.source.read_only;
      }
      return X_ERROR_SUCCESS;
    }
  }
  // A missing or malformed read is never converted to a writable destination.
  // Creation uses ResolvePackagePath and the same checked source resolver.
  return available ? X_ERROR_PATH_NOT_FOUND : X_ERROR_DEVICE_NOT_CONNECTED;
}

// The header of content without one of its own to take the metadata from.
static std::unique_ptr<vfs::XContentContainerHeader> MakeContentHeader(
    const XContentType content_type, const uint32_t title_id,
    const std::u16string_view name, const std::span<const uint8_t> thumbnail) {
  auto header = std::make_unique<vfs::XContentContainerHeader>();
  auto& metadata = header->content_metadata;
  metadata.content_type = content_type;
  metadata.execution_info.title_id = title_id;
  for (auto& display_name : metadata.display_name_raw.chars) {
    xe::string_util::copy_and_swap_truncating(display_name, name,
                                              xe::countof(display_name));
  }
  xe::string_util::copy_and_swap_truncating(
      metadata.title_name_raw.chars, name,
      xe::countof(metadata.title_name_raw.chars));
  if (!thumbnail.empty() &&
      thumbnail.size() <= vfs::XContentMetadata::kThumbLengthV2) {
    std::memcpy(metadata.thumbnail, thumbnail.data(), thumbnail.size());
    std::memcpy(metadata.title_thumbnail, thumbnail.data(), thumbnail.size());
    metadata.thumbnail_size = static_cast<uint32_t>(thumbnail.size());
    metadata.title_thumbnail_size = static_cast<uint32_t>(thumbnail.size());
  }
  return header;
}

X_RESULT ContentManager::GetContentMetadata(const uint64_t xuid,
                                            const XCONTENT_AGGREGATE_DATA& data,
                                            std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();
  const auto title = FindLibraryTitle(
      kernel_state_ ? kernel_state_->emulator() : nullptr, data);
  std::filesystem::path package_path;
  std::string source_id;
  if (title) {
    package_path = title->path;
  } else if (const X_RESULT result = ResolveReadPath(
                 xuid, data, package_path, nullptr, nullptr, &source_id)) {
    return result;
  }
  std::error_code ec;
  if (!std::filesystem::exists(package_path, ec)) {
    return X_ERROR_PATH_NOT_FOUND;
  }
  // A package kept as a file is its own header. An extracted one keeps it in
  // the header file, which a game library release has none of.
  const bool extracted = std::filesystem::is_directory(package_path, ec);
  std::unique_ptr<vfs::XContentContainerHeader> header;
  if (!extracted || !title) {
    const auto header_path =
        extracted
            ? ResolveSourcePath(package_path, PackageHeaderPath(package_path),
                                false, source_id)
            : std::optional(package_path);
    if (header_path) {
      header = vfs::XContentContainerDevice::ReadContainerHeader(*header_path);
    }
  }
  if (title && (!header || !header->content_header.is_magic_valid())) {
    // A disc image or executable has only what the game library knows.
    header = MakeContentHeader(LibraryContentType(*title), title->title_id,
                               xe::to_utf16(title->name),
                               ReadCatalogIcon(title->icon_path));
  } else if (!header || !header->content_header.is_magic_valid()) {
    // A header file with only the package's content data gives its type,
    // title and name.
    XCONTENT_AGGREGATE_DATA stored;
    if (!extracted ||
        ReadContentHeaderFileFromSource(package_path, xuid, data.title_id,
                                        data.content_type, stored, source_id)) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    header = MakeContentHeader(stored.content_type, stored.title_id,
                               stored.display_name(), {});
  }
  // The package's numeric category is already a native field; XEX-only SPA
  // metadata has no verified native wire category and remains unspecified.
  if (title && title->native_category) {
    header->content_metadata.category = *title->native_category;
  }
  // Every string ends in a terminator, whatever the header holds.
  auto& metadata = header->content_metadata;
  TerminateMetadataStrings(metadata);
  const auto* bytes = reinterpret_cast<const uint8_t*>(&metadata);
  buffer->assign(bytes, bytes + sizeof(metadata));
  return X_ERROR_SUCCESS;
}

// Games, demos and the like keep their data in files beside the package.
static bool HasDataFiles(const XContentType content_type) {
  switch (content_type) {
    case XContentType::kMarketplaceContent:
    case XContentType::kInstalledGame:
    case XContentType::kXboxTitle:
    case XContentType::kXbox360Title:
    case XContentType::kGameDemo:
    case XContentType::kArcadeTitle:
      return true;
    default:
      return (static_cast<uint32_t>(content_type) & 0xFFF00000) != 0;
  }
}

static uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
  }
  // An extracted package is as big as its files.
  uint64_t total = 0;
  std::filesystem::recursive_directory_iterator it(path, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    // A file it can't read leaves out only that file.
    std::error_code entry_ec;
    if (it->is_regular_file(entry_ec)) {
      const auto size = it->file_size(entry_ec);
      if (!entry_ec) {
        total += size;
      }
    }
  }
  return total;
}

X_RESULT ContentManager::GetContentAttributes(
    const uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
    ContentAttributes* attributes) {
  std::filesystem::path package_path;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (IsDiscContent(data)) {
      const vfs::Entry* entry = ResolveDiscPackage(data);
      if (!entry) {
        return X_ERROR_FILE_NOT_FOUND;
      }
      attributes->creation_time = entry->create_timestamp();
      attributes->last_access_time = entry->access_timestamp();
      attributes->last_write_time = entry->write_timestamp();
      attributes->size = entry->size();
      return X_ERROR_SUCCESS;
    }
    if (const X_RESULT result = ResolveReadPath(xuid, data, package_path)) {
      return result;
    }
  }
  // Without the lock, which a large extracted package would hold for long.
  const auto info = xe::filesystem::GetInfo(package_path);
  if (!info) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  attributes->creation_time = info->create_timestamp;
  attributes->last_access_time = info->access_timestamp;
  attributes->last_write_time = info->write_timestamp;
  attributes->size = FileSize(package_path);
  // An extracted package's size already counts its data.
  if (!HasDataFiles(data.content_type) ||
      info->type == xe::filesystem::FileInfo::Type::kDirectory) {
    return X_ERROR_SUCCESS;
  }
  const auto header =
      vfs::XContentContainerDevice::ReadContainerHeader(package_path);
  if (header && header->content_header.is_magic_valid()) {
    attributes->size += header->content_metadata.data_file_size;
    return X_ERROR_SUCCESS;
  }
  // Without a header, count the data files there are.
  auto data_path = package_path;
  data_path += ".data";
  for (uint32_t i = 0;; ++i) {
    const auto data_file = data_path / fmt::format("Data{:04}", i);
    std::error_code ec;
    if (!std::filesystem::exists(data_file, ec)) {
      break;
    }
    attributes->size += FileSize(data_file);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::SetContentThumbnail(
    uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
    std::vector<uint8_t> bytes) {
  if (global_critical_region::is_held_by_current_thread()) { return X_ERROR_BUSY; }
  std::filesystem::path package_path; std::string source_id;
  if (const auto result = ResolveWritePath(xuid, data, package_path, &source_id)) {
    return result;
  }
  const auto source = sources_.Source(source_id);
  if (!source || !source->available || !source->storage || !source->volume ||
      source->source.kind != ContentSourceKind::kManaged || source->source.read_only ||
      source->volume->identity() != source->managed_identity ||
      !source->volume->mutations()) { return X_ERROR_ACCESS_DENIED; }
  const auto target = ResolveSourcePath(package_path,
      package_path / vfs::kPackageThumbnailFileName, true, source_id);
  if (!target) { return X_ERROR_ACCESS_DENIED; }
  const auto package = package_path.lexically_relative(source->storage->path());
  std::error_code error;
  auto owner = source->volume->mutations();
  const auto expected = owner->CaptureMetadata(filesystem::ContentMutationOwner::Kind::kThumbnail,
      package, target->lexically_relative(source->storage->path()), error);
  if (!expected) { return ContentMutationError(error); }
  const auto receipt = owner->WriteMetadata(filesystem::ContentMutationOwner::Kind::kThumbnail,
                                             package, expected, bytes);
  return receipt.complete ? X_ERROR_SUCCESS : ContentMutationError(receipt.error ? receipt.error :
      std::make_error_code(std::errc::io_error));
}

struct ContentManager::ContentFlush final {
  std::shared_ptr<filesystem::ContentMutationOwner> owner;
  std::unique_ptr<filesystem::ContentMutationOwner::FlushReservation> reservation;
  std::shared_ptr<filesystem::HostStorageRoot> mount;
  std::filesystem::path header;
  object_ref<XEvent> event;
  std::atomic<uint64_t> remaining{0};
  std::atomic<X_STATUS> first_error{X_STATUS_SUCCESS};
  std::atomic<X_RESULT> result{X_ERROR_IO_PENDING};
  std::atomic<bool> done{false};
  void Finish(bool dropped) {
    X_STATUS status = first_error.load(std::memory_order_acquire);
    if (dropped && XSUCCEEDED(status)) { status = X_STATUS_CANCELLED; }
    X_RESULT output;
    if (XFAILED(status)) { output = xboxkrnl::xeRtlNtStatusToDosError(status); }
    else if (!mount || mount->retiring()) { output = X_ERROR_DEVICE_NOT_CONNECTED; }
    else { output = ContentMutationError(owner->Flush(*reservation, header)); }
    // No common/MVA owner is stored on the waiter's reclaimable fiber stack.
    // Release this operation's filesystem pins BEFORE touching guest event
    // metadata. Existing file handles/queue runners retain their own refs and
    // actual format drain still checks those separately.
    reservation.reset(); owner.reset(); mount.reset();
    result.store(output, std::memory_order_relaxed);
    done.store(true, std::memory_order_release);
    event->Set(0, false);
  }
  void Arrive(X_STATUS status) {
    if (XFAILED(status)) {
      X_STATUS success = X_STATUS_SUCCESS;
      first_error.compare_exchange_strong(success, status, std::memory_order_acq_rel);
    }
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) { Finish(false); }
  }
};

X_RESULT ContentManager::FlushContent(std::string_view root_name) {
  if (!kernel_state_ || global_critical_region::is_held_by_current_thread()) {
    return X_ERROR_NOT_SUPPORTED;
  }
  std::filesystem::path package_path;
  std::string device_path;
  std::optional<ContentSourceState> captured;
  std::shared_ptr<vfs::Device> device;
  {
    auto lock = global_critical_region_.Acquire();
    const auto found = open_packages_.find(string_key_insensitive(root_name));
    if (found == open_packages_.end()) { return X_ERROR_INVALID_HANDLE; }
    package_path = found->second->package_path();
    device_path = found->second->device_path();
    captured = found->second->source_;
    device = found->second->registered_device_.lock();
    if (device != kernel_state_->file_system()->AcquireDevice(device_path)) {
      return X_ERROR_INVALID_HANDLE;
    }
  }
  if (!device) { return X_ERROR_INVALID_HANDLE; }
  if (device->is_read_only()) {
    // Immutable sources never admitted mutable package I/O. This is not an
    // STFS writer/control implementation or a fabricated host flush effect.
    if (const auto* host = dynamic_cast<vfs::HostPathDevice*>(device.get())) {
      if (!host->storage() || !host->storage()->Observe().available) {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }
    return X_ERROR_SUCCESS;
  }
  if (GuestScheduler::CurrentThreadIsBlockingCallWorker() ||
      !XThread::GetCurrentFiberThread()) { return X_ERROR_NOT_SUPPORTED; }
  if (!captured || !captured->volume || !captured->volume->mutations()) {
    return X_ERROR_NOT_SUPPORTED;
  }
  auto current = sources_.Source(captured->source.id);
  if (!current || !current->available || current->generation != captured->generation ||
      current->storage != captured->storage || current->volume != captured->volume) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const auto relative = package_path.lexically_relative(captured->storage->path());
  auto header = ResolveSourcePath(package_path, PackageHeaderPath(package_path),
                                   false, captured->source.id);
  if (!header) { return X_ERROR_ACCESS_DENIED; }
  auto owner = captured->volume->mutations();
  std::error_code error;
  auto reservation = owner->BeginFlush(relative, error);
  if (!reservation) { return ContentMutationError(error); }
  std::vector<object_ref<XFile>> files;
  std::shared_ptr<filesystem::HostStorageRoot> mount;
  {
    auto lock = global_critical_region_.Acquire();
    const auto found = open_packages_.find(string_key_insensitive(root_name));
    if (found == open_packages_.end() || found->second->device_path() != device_path ||
        kernel_state_->file_system()->AcquireDevice(device_path) != device) {
      return X_ERROR_INVALID_HANDLE;
    }
    const auto* owned = dynamic_cast<vfs::HostPathDevice*>(device.get());
    if (!owned || !owned->storage()) { return X_ERROR_NOT_SUPPORTED; }
    mount = owned->storage();
    // Close cooperating new opens BEFORE this snapshot. Include existing
    // retained folder aliases/child mounts rather than only a root-name ID.
    for (auto& file : kernel_state_->object_table()->GetObjectsByType<XFile>(XObject::Type::File)) {
      const auto retained = file->file()->retained_device();
      const auto* host = dynamic_cast<vfs::HostPathDevice*>(retained.get());
      if (!host || !host->storage() || host->storage()->retiring()) { continue; }
      const auto* entry = dynamic_cast<vfs::HostPathEntry*>(file->entry());
      if (!entry) { continue; }
      const auto suffix = entry->host_path().lexically_relative(package_path);
      if (!suffix.empty() && !suffix.is_absolute() && *suffix.begin() != "..") {
        if (files.size() == 4096) { return X_ERROR_BUSY; }
        files.push_back(file);
      }
    }
  }
  auto state = std::make_shared<ContentFlush>();
  state->owner = std::move(owner); state->reservation = std::move(reservation);
  state->mount = std::move(mount);
  state->header = header->lexically_relative(captured->storage->path());
  state->event = object_ref<XEvent>(new XEvent(kernel_state_, true));
  state->event->Initialize(false, false);
  state->event->set_signal_ring_quiet(true);
  state->remaining.store(files.size(), std::memory_order_relaxed);
  uint64_t serial = 0;
  {
    std::lock_guard lock(content_flush_mutex_);
    if (content_flushes_.size() < 64 && next_content_flush_) {
      serial = next_content_flush_++;
      content_flushes_.emplace(serial, state);
    }
  }
  if (!serial) {
    state->event->ReleaseHandle();
    return X_ERROR_BUSY;
  }
  if (files.empty()) {
    kernel_state_->guest_scheduler()->PostHostCall(
        [state] { state->Finish(false); }, GuestScheduler::BlockingCallClass::kConcurrent,
        [state](GuestScheduler::HostCallDropReason) { state->Finish(true); });
  } else {
    for (const auto& file : files) {
      const auto status = file->FlushQueued([state](X_STATUS result) { state->Arrive(result); });
      if (XFAILED(status)) { state->Arrive(status); }
    }
  }
  auto* waiting = state.get();
  // Authority-bearing locals must be empty before a guest-fiber wait. Kernel
  // shutdown may reclaim an abandoned fiber without C++ stack unwinding.
  files.clear(); captured.reset(); current.reset(); device.reset();
  owner.reset(); reservation.reset(); mount.reset(); state.reset();
  while (!waiting->done.load(std::memory_order_acquire)) {
    // Same noninterruptible request discipline as KernelState::RunBlockingIo.
    waiting->event->Wait(0, 1, 0, nullptr, false);
  }
  const auto result = waiting->result.load(std::memory_order_relaxed);
  std::shared_ptr<ContentFlush> completed;
  {
    std::lock_guard lock(content_flush_mutex_);
    const auto found = content_flushes_.find(serial);
    if (found != content_flushes_.end() && found->second.get() == waiting) {
      completed = std::move(found->second); content_flushes_.erase(found);
    }
  }
  // Native queue completion never publishes a user overlap. Only this real
  // returned caller performs CompleteRequest at the XAM boundary.
  if (completed) { completed->event->ReleaseHandle(); }
  return result;
}

X_RESULT ContentManager::ResolvePackageMutation(
    uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
    PackageMutationTarget& target, bool retain_source) {
  target = {};
  std::filesystem::path package_path;
  std::string source_id;
  if (const X_RESULT result =
          ResolveWritePath(xuid, data, package_path, &source_id,
                           retain_source ? &target.source : nullptr)) {
    return result;
  }
  if (IsContentOpen(package_path)) {
    return X_ERROR_SHARING_VIOLATION;
  }
  const auto source = retain_source ? std::optional<ContentSourceState>(target.source)
                                    : sources_.Source(source_id);
  if (!source || !source->available || !source->storage) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (source->source.read_only || source->storage->read_only() ||
      source->source.kind != ContentSourceKind::kManaged ||
      !source->managed_identity || !source->volume) {
    return X_ERROR_ACCESS_DENIED;
  }
  const auto relative =
      package_path.lexically_relative(source->storage->path());
  if (relative.empty() || relative.is_absolute() ||
      std::any_of(relative.begin(), relative.end(),
                  [](const auto& part) { return part == ".."; })) {
    return X_ERROR_ACCESS_DENIED;
  }
  auto error = source->managed_identity->CheckCurrent();
  if (!error) { error = source->storage->CheckGuestAccess(relative, true); }
  if (error) { return ContentMutationError(error); }
  target.source = *source;
  target.relative = relative;
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::TruncateContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  return MutateContentPackage(xuid, data, true);
}
X_RESULT ContentManager::DeleteContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  return MutateContentPackage(xuid, data, false);
}
X_RESULT ContentManager::MutateContentPackage(const uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data, bool truncate) {
  if (global_critical_region::is_held_by_current_thread()) { return X_ERROR_BUSY; }
  PackageMutationTarget target;
  if (const X_RESULT result = ResolvePackageMutation(xuid, data, target, true)) { return result; }
  const auto physical = target.source.storage->path() / target.relative;
  auto package_claim = ClaimPackagePath(physical, true);
  if (!package_claim) { return X_ERROR_SHARING_VIOLATION; }
  auto owner = target.source.volume->mutations();
  if (!owner || target.source.volume->identity() != target.source.managed_identity) {
    return X_ERROR_ACCESS_DENIED;
  }
  const auto checked = sources_.Resolve(target.source,
      PackageHeaderPath(physical).lexically_relative(target.source.storage->path()), true);
  if (!checked) { return X_ERROR_ACCESS_DENIED; }
  std::error_code error;
  auto tree = owner->CapturePackage(target.relative, error, target.source.storage);
  if (!tree) { return ContentMutationError(error); }
  if (truncate && !tree->top_info().directory) { return X_ERROR_NOT_SUPPORTED; }
  auto header = owner->CaptureMetadata(filesystem::ContentMutationOwner::Kind::kHeader,
      target.relative, checked->lexically_relative(target.source.storage->path()), error);
  if (!header) { return ContentMutationError(error); }
  const auto result = truncate ? owner->Truncate(std::move(tree), std::move(header))
                               : owner->Delete(std::move(tree), std::move(header));
  return result.complete ? X_ERROR_SUCCESS : ContentMutationError(result.error ? result.error :
      std::make_error_code(std::errc::io_error));
}

std::filesystem::path ContentManager::ResolveGameUserContentPath(
    const uint64_t xuid) {
  if (!kernel_state_) {
    return {};
  }
  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id = fmt::format("{:08X}", kernel_state_->title_id());

  return root_path_ / xuid_str / kDashboardStringID / "00010000" / xuid_str /
         title_id;
}

bool ContentManager::IsContentOpen(
    const std::filesystem::path& package_path) const {
  // Snapshot cached coordinates under the short map guard. Exact mutation
  // admission separately closes new mount-publication races. Physical alias
  // checks run outside this helper's guard; legacy callers already holding
  // GCRC keep their old behavior until their own operation is migrated.
  std::vector<std::filesystem::path> paths;
  {
    auto lock = global_critical_region_.Acquire();
    for (const auto& [_, content] : open_packages_) { paths.push_back(content->package_path()); }
  }
  return std::ranges::any_of(paths, [&](const auto& path) {
    std::error_code ec;
    return std::filesystem::equivalent(package_path, path, ec);
  });
}

void ContentManager::CloseOpenedFilesFromContent(
    const ContentPackage& package) {
  if (!kernel_state_) {
    return;
  }
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<object_ref<XFile>> all_files_handles =
      kernel_state_->object_table()->GetObjectsByType<XFile>(
          XObject::Type::File);

  for (const object_ref<XFile>& file : all_files_handles) {
    std::string file_path = file->entry()->absolute_path();
    bool is_file_inside_content =
        utf8::starts_with(file_path, package.device_path());

    if (is_file_inside_content) {
      file->ReleaseHandle();
    }
  }
}

uint64_t ContentManager::GetContentTotalSpace() const {
  if (sources_.read_only_catalog()) {
    return 0;
  }

  const auto source = sources_.Source("hdd");
  return source && source->available ? source->capacity_bytes : 0;
}

uint64_t ContentManager::GetContentFreeSpace() const {
  if (sources_.read_only_catalog()) {
    return 0;
  }

  const auto source = sources_.Source("hdd");
  return source && source->available ? source->free_bytes : 0;
}

bool ContentManager::UpdateSpaData(std::string_view guest_path) {
  if (!kernel_state_ || global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  vfs::File* opened = nullptr;
  vfs::FileAction action{};
  const auto status = kernel_state_->file_system()->OpenFile(
      nullptr, guest_path, vfs::FileDisposition::kOpen,
      filesystem::FileAccess::kFileReadData, false, true, &opened, &action);
  const auto destroy = [](vfs::File* file) { if (file) { file->Destroy(); } };
  std::unique_ptr<vfs::File, decltype(destroy)> file(opened, destroy);
  if (XFAILED(status) || !file) { return false; }
  size_t size = 0;
  {
    // VFS open retains the actual device/tree. Only cached Entry metadata is
    // read under this short guard; no file read or database callback here.
    auto lock = global_critical_region_.Acquire();
    if (!file->entry()) { return false; }
    size = file->entry()->size();
  }
  // Explicit host metadata bound, not a claimed retail SPA size limit.
  if (!size || size > filesystem::content_mutation_wire::kMaximumMetadataBytes) {
    return false;
  }
  std::vector<uint8_t> data(size);
  size_t read_bytes = 0;
  if (file->ReadSync(data, 0, &read_bytes) != X_STATUS_SUCCESS || read_bytes != size) {
    return false;
  }
  xam::SpaInfo new_spa_data(std::span<uint8_t>(data.data(), data.size()));
  // Preserve the existing SPA consumer semantics; this is outside the outer
  // namespace guard, with the real input backing alive through both callbacks.
  kernel_state_->xam_state()->LoadSpaInfo(&new_spa_data);
  kernel_state_->emulator()->game_info_database()->Update(&new_spa_data);
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
