/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/xam/selected_media_source_proof.h"
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/media_selection.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/media_reference_device.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include "xenia/base/filesystem.h"
#include "xenia/base/mutex.h"
#include "xenia/base/host_storage.h"
#include "xenia/base/string.h"
#include "xenia/kernel/native_content_storage_source.h"
#include "xenia/base/managed_device_identity.h"

namespace xe::kernel::xam {
namespace {

bool ParticipatesInContentDevices(const ContentSource& source) {
  return source.kind!=ContentSourceKind::kCache;
}
bool ConflictsWithContentDevice(const ContentSource& held,const ContentSource& desired) {
  return ParticipatesInContentDevices(held) && held.device_id==desired.device_id &&
      (held.device_type!=desired.device_type || (!held.read_only && !desired.read_only));
}

class CacheFolderDevice final : public vfs::HostPathDevice {
 public:
  CacheFolderDevice(std::string_view mount,std::shared_ptr<filesystem::HostStorageRoot> root,uint32_t sectors)
      :HostPathDevice(mount,root->path(),false,false,"FATX",root,root,ControlRole::kFolderVolume),
       sectors_(sectors?sectors:1){}
  // Before any actual committed slot format this is the existing logical
  // HostPath folder unit. A recovered format changes it only in a NEW exact
  // source/device generation, never by modifying a retained old File/device.
  uint32_t sectors_per_allocation_unit() const override {return sectors_;}
 private:
  const uint32_t sectors_;
};

bool IsBelow(const std::filesystem::path& root,
             const std::filesystem::path& path) {
  auto child = path.begin();
  for (auto part = root.begin(); part != root.end(); ++part, ++child) {
    if (child == path.end() || *part != *child) {
      return false;
    }
  }
  return true;
}

std::optional<std::filesystem::path> ResolveBelow(
    const std::filesystem::path& root, const std::filesystem::path& relative,
    unsigned symlink_depth = 0) {
  std::error_code ec;
  // FATX compares every component without case. Resolve the actual host names
  // before canonicalization, including the XUID/title/type and auxiliary paths.
  // An exact host match wins; ambiguous case-only aliases are refused.
  auto resolved = root;
  for (const auto& part : relative) {
    if (part == "." || part.empty()) {
      continue;
    }
    auto next = resolved / part;
    auto status = std::filesystem::symlink_status(next, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      return std::nullopt;
    }
    if (status.type() == std::filesystem::file_type::not_found) {
      ec.clear();
      if (std::filesystem::is_directory(resolved, ec)) {
        std::filesystem::path match;
        std::filesystem::directory_iterator it(resolved, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
          if (xe::utf8::equal_case(xe::path_to_utf8(it->path().filename()),
                                   xe::path_to_utf8(part))) {
            if (!match.empty()) {
              return std::nullopt;
            }
            match = it->path();
          }
        }
        if (ec) {
          return std::nullopt;
        }
        if (!match.empty()) {
          next = std::move(match);
        }
      } else if (ec && ec != std::errc::no_such_file_or_directory) {
        return std::nullopt;
      }
      ec.clear();
      status = std::filesystem::symlink_status(next, ec);
    }
    // weakly_canonical alone leaves a dangling symlink unresolved. Check its
    // target explicitly so a later create cannot follow it outside the source.
    if (std::filesystem::is_symlink(status)) {
      const auto target = std::filesystem::read_symlink(next, ec);
      if (ec) {
        return std::nullopt;
      }
      next = (target.is_absolute() ? target : next.parent_path() / target)
                 .lexically_normal();
      if (!IsBelow(root, next) || symlink_depth >= 40) {
        return std::nullopt;
      }
      // Resolve every link in a dangling chain, including links in the target's
      // parent components. A future create must not change containment.
      const auto linked =
          ResolveBelow(root, next.lexically_relative(root), symlink_depth + 1);
      if (!linked) {
        return std::nullopt;
      }
      next = *linked;
    }
    ec.clear();
    resolved = std::filesystem::weakly_canonical(next, ec);
    if (ec || !IsBelow(root, resolved)) {
      return std::nullopt;
    }
  }
  return resolved;
}

}  // namespace

// The catalog and notification producer have owned state independent of the
// registry wrapper. Captured roots and admitted callbacks never borrow it.
struct ManagedFormatSourceProof::Impl {
  std::string source_id;
  uint64_t generation = 0;
  std::weak_ptr<const filesystem::ManagedDeviceIdentity> identity;
  std::weak_ptr<const filesystem::ManagedVolumeAuthority> volume;
  std::function<bool(const ManagedFormatSourceProof&)> current;
};
ManagedFormatSourceProof::ManagedFormatSourceProof(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ManagedFormatSourceProof::~ManagedFormatSourceProof() = default;
const std::string& ManagedFormatSourceProof::source_id() const { return impl_->source_id; }
uint64_t ManagedFormatSourceProof::generation() const { return impl_->generation; }
bool ManagedFormatSourceProof::MatchesIdentity(
    const std::shared_ptr<const filesystem::ManagedDeviceIdentity>& identity) const {
  return identity && !impl_->identity.expired() &&
      !impl_->identity.owner_before(identity) && !identity.owner_before(impl_->identity);
}
bool ManagedFormatSourceProof::MatchesVolume(
    const std::shared_ptr<const filesystem::ManagedVolumeAuthority>& volume) const {
  return volume && !impl_->volume.expired() &&
      !impl_->volume.owner_before(volume) && !volume.owner_before(impl_->volume);
}
bool ManagedFormatSourceProof::current() const noexcept try {
  return impl_->current && impl_->current(*this);
} catch (...) { return false; }

struct ContentSourceRegistry::State : public std::enable_shared_from_this<State> {
  struct Capability {
    std::shared_ptr<filesystem::HostStorageRoot> root;
    uint64_t generation = 0;
    bool fixed = false;
    std::shared_ptr<const filesystem::ManagedDeviceIdentity> managed_identity;
    std::shared_ptr<filesystem::HostStorageRoot> authority;
    std::shared_ptr<const filesystem::ManagedVolumeAuthority> volume;
    bool transitioning = false;
    std::shared_ptr<filesystem::MediaSelection> media;
    std::shared_ptr<const SelectedMediaSourceProof> media_proof;
    std::shared_ptr<const SelectedMediaPublication> media_publication;
    // App-owned removal keeps actual backings until BOTH counted native uses
    // and ordinary VFS Files have drained, rather than reaping at use-count 0.
    bool media_forget_pending = false;
    bool media_unavailable = false;
    bool media_draining = false, media_physical_available = false;
    std::shared_ptr<std::atomic<size_t>> media_uses;
    std::shared_ptr<std::atomic<bool>> media_accepting;
    std::weak_ptr<vfs::Device> media_device;
    std::shared_ptr<const ManagedFormatSourceProof> managed_format_proof;
    std::shared_ptr<const filesystem::ConsoleStorageVolume> storage_volume;
    std::weak_ptr<filesystem::CacheStorageOwner> cache_owner;
    filesystem::CacheStorageOwner::Role cache_role=filesystem::CacheStorageOwner::Role::kLegacy;
    std::weak_ptr<vfs::Device> cache_device;
  };
  State(Access access_value, SpaceQuery value)
      : access(access_value), query(std::move(value)) {
    snapshot.revision = 1;
  }
  struct MediaObservation {
    bool member = false;
    std::shared_ptr<const SelectedMediaPublication> publication;
    uint64_t generation=0;
    std::shared_ptr<filesystem::MediaSelection> selection;
    std::weak_ptr<vfs::Device> device;
    // This strong pin is acquired/released outside the metadata mutex.
    std::shared_ptr<vfs::Device> retained_device;
    filesystem::HostStorageSnapshot value;
  };
  struct CacheObservation {
    uint64_t generation=0;
    std::weak_ptr<filesystem::CacheStorageOwner> owner_identity;
    // Strong pin only on the observing lane, outside metadata/GCRC guards.
    std::shared_ptr<filesystem::CacheStorageOwner> owner;
    std::shared_ptr<filesystem::HostStorageRoot> root;
    filesystem::HostStorageSnapshot value;
  };
  struct QueryValues : std::map<std::string, std::optional<std::filesystem::space_info>> {
    std::map<std::string, MediaObservation> media;
    std::map<std::string, CacheObservation> caches;
  };
  // Caller holds mutex. Return a borrowed internal row only in this scope;
  // never acquire a weak source/backing owner or perform observing IO here.
  const ContentSourceState* CachedVolumeLocked(
      const std::shared_ptr<const filesystem::ConsoleStorageVolume>& volume) const {
    using Volume=filesystem::ConsoleStorageVolume;
    if(!volume || volume->origin()!=Volume::Origin::kRegisteredSource ||
        !volume->device_id() || !volume->generation() || volume->source_id().empty() ||
        !volume->available_cached())return nullptr;
    const auto row=observed.find(volume->source_id());
    const auto cap=capabilities.find(volume->source_id());
    if(closed.load() || row==observed.end() || cap==capabilities.end() ||
        cap->second.transitioning || !row->second.available || !row->second.storage ||
        row->second.storage->retiring() || cap->second.root!=row->second.storage ||
        cap->second.generation!=volume->generation() || row->second.generation!=volume->generation() ||
        row->second.source.device_id!=volume->device_id() || row->second.source.device_type!=volume->type() ||
        row->second.storage_volume!=volume || cap->second.storage_volume!=volume ||
        row->second.storage_volume.owner_before(volume) || volume.owner_before(row->second.storage_volume) ||
        cap->second.managed_identity!=row->second.managed_identity ||
        cap->second.volume!=row->second.volume || !volume->available_cached())return nullptr;
    return &row->second;
  }
  // Metadata-only selected-media description. This deliberately does NOT
  // weaken CachedVolumeLocked's retained directory/source admission: a file
  // selection still cannot acquire a HostStorageRoot or content package root.
  const ContentSourceState* CachedDescriptionLocked(
      const std::shared_ptr<const filesystem::ConsoleStorageVolume>& volume) const {
    if(const auto* row=CachedVolumeLocked(volume))return row;
    if(!volume || volume->origin()!=filesystem::ConsoleStorageVolume::Origin::kRegisteredSource ||
        !volume->available_cached() || !volume->device_id() || !volume->generation())return nullptr;
    const auto found=observed.find(volume->source_id());
    const auto actual=capabilities.find(volume->source_id());
    if(closed.load() || found==observed.end() || actual==capabilities.end())return nullptr;
    const auto& row=found->second;const auto& cap=actual->second;
    if(row.source.kind!=ContentSourceKind::kSelectedMedia || !row.source.read_only ||
        !row.available || row.observation_error || cap.transitioning ||
        cap.media_unavailable || cap.media_draining || !cap.media ||
        !cap.media->current_cached() || row.selected_media!=cap.media ||
        !cap.media_proof || !cap.media_proof->SourceCurrentCached() ||
        !cap.media_accepting || !cap.media_accepting->load(std::memory_order_acquire) ||
        row.generation!=volume->generation() || cap.generation!=volume->generation() ||
        row.source.device_id!=volume->device_id() || row.source.device_type!=volume->type() ||
        row.storage_volume!=volume || cap.storage_volume!=volume ||
        row.storage_volume.owner_before(volume) || volume.owner_before(row.storage_volume) ||
        !volume->available_cached())return nullptr;
    return &row;
  }
  // Caller holds mutex. Inspect weak ownership without locking the MVA: the
  // last actual owner must never be destroyed by this cached check under it.
  bool ProofCurrentLocked(const ManagedFormatSourceProof& proof,
      const std::weak_ptr<const filesystem::ManagedDeviceIdentity>& identity,
      const std::weak_ptr<const filesystem::ManagedVolumeAuthority>& volume) const {
    if (closed.load() || access != Access::kNormal) { return false; }
    const auto found = capabilities.find(proof.source_id());
    const auto row = observed.find(proof.source_id());
    if (found == capabilities.end() || row == observed.end()) { return false; }
    const auto& cap = found->second;
    const auto& observed = row->second;
    return !cap.fixed && !cap.transitioning &&
        cap.managed_format_proof.get() == &proof &&
        cap.generation == proof.generation() &&
        cap.managed_identity && !identity.expired() &&
        !identity.owner_before(cap.managed_identity) && !cap.managed_identity.owner_before(identity) &&
        !cap.managed_identity->retiring() && cap.root && !cap.root->retiring() &&
        cap.authority && !cap.authority->retiring() && cap.volume &&
        !volume.expired() && !volume.owner_before(cap.volume) &&
        !cap.volume.owner_before(volume) && cap.volume->format_eligible() &&
        cap.volume->public_root() && !cap.volume->public_root()->retiring() &&
        observed.available && observed.format_eligible &&
        observed.managed_format_proof.get() == &proof &&
        observed.source.kind == ContentSourceKind::kManaged && !observed.source.read_only;
  }
  // Caller holds the existing metadata mutex. A copied definition is not
  // membership after a concurrent forget or a same-ID replacement.
  bool HasSourceLocked(const ContentSource& expected) const {
    return std::any_of(sources.begin(), sources.end(), [&](const auto& actual) {
      return actual.id == expected.id &&
             actual.device_id == expected.device_id &&
             actual.device_type == expected.device_type &&
             actual.kind == expected.kind && actual.root == expected.root &&
             actual.guest_root == expected.guest_root &&
             actual.name == expected.name &&
             actual.read_only == expected.read_only &&
             actual.reserved_bytes == expected.reserved_bytes &&
             actual.guest_volume_root == expected.guest_volume_root &&
             actual.host_volume_root == expected.host_volume_root &&
             actual.private_state_parent == expected.private_state_parent;
    });
  }
  QueryValues SampleQueries(const std::vector<ContentSource>& selected) {
    QueryValues values;
    const bool context_refused=global_critical_region::is_held_by_current_thread();
    std::vector<std::shared_ptr<filesystem::MediaSelection>> retired;
    if(!context_refused) {
      std::lock_guard lock(mutex);
      for (auto& [id, cap] : capabilities) {
        if (cap.media_draining && !cap.media_forget_pending && cap.media_uses &&
            !cap.media_uses->load()) {
          cap.media_draining = false;
          retired.push_back(cap.media);
        }
      }
    }
    // Actual native pins/final descriptor destruction never occur under the
    // source metadata mutex, GCRC, callback or a SelectedMediaUse destructor.
    for(const auto& media:retired)media->Retire();
    if (query) {
      for (const auto& source : selected) {
        if (source.kind == ContentSourceKind::kSelectedMedia || source.kind==ContentSourceKind::kCache) continue;
        values.emplace(source.id, query(source.root));
      }
    }
    for(const auto& source:selected)if(source.kind==ContentSourceKind::kCache){
      CacheObservation sample;std::weak_ptr<filesystem::CacheStorageOwner> weak;
      filesystem::CacheStorageOwner::Role role{};
      std::function<void(const std::string&)> checkpoint;
      {std::lock_guard lock(mutex);const auto cap=capabilities.find(source.id);
        if(cap!=capabilities.end()) {sample.generation=cap->second.generation;weak=cap->second.cache_owner;
          sample.owner_identity=weak;role=cap->second.cache_role;sample.root=cap->second.root;}
        checkpoint=sampled_for_test;}
      // Timing only: every resumed sample still probes its actual captured
      // owner/root generation outside source metadata/GCRC locks.
      if(context_refused)sample.value.error=std::make_error_code(std::errc::operation_would_block);
      else {
        if(checkpoint)checkpoint(source.id);
        if((sample.owner=weak.lock())){
          std::error_code error;const auto current=sample.owner->CaptureRoot(role,error);
          if(error)sample.value.error=error;
          else if(current!=sample.root)sample.value.error=std::make_error_code(std::errc::no_such_device);
          else if(sample.root)sample.value=sample.root->Observe();
        }else sample.value.error=std::make_error_code(std::errc::no_such_device);
      }
      values.caches.emplace(source.id,std::move(sample));
    }
    for (const auto& source : selected) {
      if (source.kind != ContentSourceKind::kSelectedMedia) continue;
      MediaObservation sample;bool draining=false;
      std::function<void(const std::string&)> checkpoint;
      {
        std::lock_guard lock(mutex);const auto found=capabilities.find(source.id);
        sample.member = HasSourceLocked(source);
        if (sample.member && found != capabilities.end()) {
          sample.publication = found->second.media_publication;
          sample.generation=found->second.generation;sample.selection=found->second.media;
          draining=found->second.media_draining;sample.device=found->second.media_device;
        }
        checkpoint=sampled_for_test;
      }
      if(sample.selection) {
        if(context_refused) {
          sample.value.error=std::make_error_code(std::errc::operation_would_block);
        } else {
          // This private checkpoint controls timing only. The following probe
          // still observes the actual captured selection and registered device.
          if(checkpoint)checkpoint(source.id);
          sample.retained_device=sample.device.lock();
          if(!sample.retained_device || sample.retained_device->namespace_closing_cached())
            sample.value.error=std::make_error_code(std::errc::no_such_device);
          else sample.value=ContentSourceRegistry::ProbeSelected(sample.selection,draining);
        }
      }
      values.media.emplace(source.id,std::move(sample));
    }
    return values;
  }
  ContentSourceState ObserveLocked(const ContentSource& source,
                                   const QueryValues& queries) {
    ContentSourceState result{source};
    if (!HasSourceLocked(source)) {
      result.observation_error =
          std::make_error_code(std::errc::operation_canceled);
      return result;
    }
    if(source.kind==ContentSourceKind::kCache){
      const auto found=capabilities.find(source.id);if(found==capabilities.end())return result;
      auto& cap=found->second;result.generation=cap.generation;
      if(cap.transitioning){observed.insert_or_assign(source.id,result);return result;}
      const auto observation=queries.caches.find(source.id);
      const bool same=observation!=queries.caches.end() && observation->second.generation==cap.generation &&
          observation->second.root==cap.root &&
          !cap.cache_owner.owner_before(observation->second.owner_identity) &&
          !observation->second.owner_identity.owner_before(cap.cache_owner);
      const auto e=observation==queries.caches.end()?std::make_error_code(std::errc::operation_canceled):
          observation->second.value.error;
      const bool refused=e==std::errc::device_or_resource_busy || e==std::errc::operation_in_progress ||
          e==std::errc::operation_would_block || e==std::errc::operation_not_permitted || e==std::errc::text_file_busy;
      if(!same || refused){
        // Local refusal only. An in-flight old generation or denied probe must
        // never overwrite the replacement's already-published observed row.
        result.observation_error=same?e:std::make_error_code(std::errc::operation_canceled);return result;
      }
      if(!observation->second.value.available){
        result.observation_error=e;
        if(cap.root)cap.root->Retire();
        if(cap.storage_volume)cap.storage_volume->RetireKnownSource();
        observed.insert_or_assign(source.id,result);return result;
      }
      if(cap.root && !cap.root->retiring() && cap.storage_volume && cap.storage_volume->available_cached()){
        result.available=true;result.storage=cap.root;result.storage_volume=cap.storage_volume;
        result.capacity_bytes=observation->second.value.capacity_bytes;
        result.free_bytes=observation->second.value.usable_bytes;
      }
      observed.insert_or_assign(source.id,result);return result;
    }
    if(source.kind==ContentSourceKind::kSelectedMedia) {
      // Observation is never a selected-source producer. In particular a late
      // sample must not recreate a default capability after actual Forget.
      const auto found = capabilities.find(source.id);
      if (found == capabilities.end()) {
        result.observation_error =
            std::make_error_code(std::errc::operation_canceled);
        return result;
      }
      auto& cap = found->second;
      result.generation = cap.generation;
      result.selected_media=cap.media;
      const auto observation=queries.media.find(source.id);
      const bool same =
          observation != queries.media.end() && observation->second.member &&
          observation->second.publication &&
          observation->second.publication == cap.media_publication &&
          observation->second.generation == cap.generation &&
          observation->second.selection == cap.media &&
          !observation->second.device.owner_before(cap.media_device) &&
          !cap.media_device.owner_before(observation->second.device);
      if (!same || (cap.media && filesystem::MediaSelection::ObservationRefused(
                                     observation->second.value.error))) {
        // This includes metadata-only unavailable publications and numeric ABA.
        // A stale sample returns denial metadata but cannot mutate any live
        // row.
        result.observation_error =
            same ? observation->second.value.error
                 : std::make_error_code(std::errc::operation_canceled);
        return result;
      }
      const auto* space=observation==queries.media.end()?nullptr:&observation->second.value;
      cap.media_physical_available=space && space->available;
      const bool available=!cap.media_unavailable && cap.media && cap.media->current_cached() &&
          space && space->available;
      if(!available) {
        cap.media_unavailable=true;
        if(cap.media_accepting)cap.media_accepting->store(false,std::memory_order_release);
        if(cap.storage_volume)cap.storage_volume->RetireKnownSource();
      } else {
        if(!cap.storage_volume)cap.storage_volume=std::shared_ptr<const filesystem::ConsoleStorageVolume>(
            new filesystem::ConsoleStorageVolume(source.device_type,source.device_id,source.id,
                cap.generation,filesystem::ConsoleStorageVolume::Origin::kRegisteredSource));
        result.available=true;result.storage=cap.root;
        result.capacity_bytes=space->capacity_bytes;result.storage_volume=cap.storage_volume;
      }
      observed.insert_or_assign(source.id,result);return result;
    }
    const auto volume = capabilities[source.id].volume;
    result.volume = volume;
    result.profile_writable = volume && volume->profile_writable();
    result.format_eligible = volume && volume->format_eligible();
    filesystem::HostStorageSnapshot space;
    auto& capability = capabilities[source.id];
    if (capability.root) {
      space = capability.root->Observe();
      if (!space.available) {
        // Retire this physical generation even if the same folder later
        // returns.
        if (capability.storage_volume) { capability.storage_volume->RetireKnownSource(); }
        capability.storage_volume.reset();
        capability.root->Retire();
        capability.root.reset();
      }
    }
    if (!capability.root && !capability.fixed && !capability.managed_identity &&
        !closed.load()) {
      std::error_code error;
      auto opened = filesystem::HostStorageRoot::Open(
          source.root, source.read_only, error, source.reserved_bytes);
      if (opened) {
        opened = opened->GuestView(error);
      }
      if (opened) {
        capability.root = std::move(opened);
        ++capability.generation;
        space = capability.root->Observe();
      }
    }
    result.generation = capability.generation;
    result.managed_identity = capability.managed_identity;
    if (!capability.transitioning && capability.root && space.available &&
        (!capability.managed_identity ||
         (!capability.managed_identity->retiring() && (!volume || volume->profile_writable())))) {
      result.storage = capability.root;
      result.available = true;
      result.capacity_bytes = space.capacity_bytes;
      result.free_bytes = source.read_only ? 0 : space.usable_bytes;
      if (const auto sampled = queries.find(source.id);
          sampled != queries.end()) {
        // The external geometry seam was sampled outside all catalog locks.
        const auto& info = sampled->second;
        if (info) {
          result.capacity_bytes = info->capacity;
          result.free_bytes =
              source.read_only ? 0
                               : (info->available > source.reserved_bytes
                                      ? info->available - source.reserved_bytes
                                      : 0);
        } else {
          result.available = false;
          result.capacity_bytes = result.free_bytes = 0;
        }
      }
    }
    if (capability.managed_identity && capability.managed_identity->retiring()) {
      if (capability.storage_volume) { capability.storage_volume->RetireKnownSource(); }
      capability.storage_volume.reset();
    }
    if (result.available) {
      if (!capability.storage_volume) {
        capability.storage_volume = std::shared_ptr<const filesystem::ConsoleStorageVolume>(
            new filesystem::ConsoleStorageVolume(source.device_type,source.device_id,
                source.id,capability.generation,
                filesystem::ConsoleStorageVolume::Origin::kRegisteredSource));
      }
      result.storage_volume=capability.storage_volume;
    }
    result.profile_writable = result.available && result.profile_writable;
    result.format_eligible = result.available && result.format_eligible;
    if (access == Access::kNormal && result.format_eligible &&
        source.kind == ContentSourceKind::kManaged && !source.read_only) {
      // Stable object per actual capability generation, including transient
      // observations of unavailable space. No refresh mints another proof.
      if (!capability.managed_format_proof) {
        capability.managed_format_proof = ContentSourceRegistry::MintManagedFormatProof(
            shared_from_this(), source.id, capability.generation,
            capability.managed_identity, capability.volume);
      }
      result.managed_format_proof = capability.managed_format_proof;
    }
    observed.insert_or_assign(source.id, result);
    return result;
  }
  // Build the same physical projection for either live or fully staged
  // metadata. All allocations precede the caller's publication/commit point.
  static bool Project(const std::vector<ContentSource>& sources,
                      const std::map<std::string, Capability>& capabilities,
                      const std::map<std::string, ContentSourceState>& observed,
                      NativeContentStorageSnapshot& snapshot) {
    // Managed storage and explicitly admitted selected readonly media have
    // volume projections. Installed content is an overlay; old host-package
    // catalog references remain excluded.
    std::map<uint32_t, const ContentSource*> volumes;
    for (const auto& source : sources) {
      if ((source.kind != ContentSourceKind::kManaged &&
           source.kind != ContentSourceKind::kSelectedMedia) ||
          source.guest_volume_root.empty()) {
        continue;
      }
      auto& selected = volumes[source.device_id];
      if (!selected || (selected->read_only && !source.read_only) ||
          (selected->read_only == source.read_only &&
           source.id < selected->id)) {
        selected = &source;
      }
    }
    std::vector<NativeContentStorageDevice> devices;
    devices.reserve(volumes.size());
    for (const auto& volume : volumes) {
      const auto* source = volume.second;
      const auto found = observed.find(source->id);
      const ContentSourceState* value =
          found == observed.end() ? nullptr : &found->second;
      const auto cap=capabilities.find(source->id);
      if(cap!=capabilities.end() && cap->second.media_draining &&
          cap->second.media_physical_available && cap->second.media_uses && cap->second.media_uses->load()) {
        // Preserve the exact previously published still-attached physical
        // generation during a CONTROLLED close. Never invent availability
        // after real backing failure; ingress is already unavailable above.
        const auto old=std::find_if(snapshot.devices.begin(),snapshot.devices.end(),
            [&](const auto& d){return d.identity==source->id && d.generation==cap->second.generation;});
        if(old!=snapshot.devices.end()) {devices.push_back(*old);continue;}
      }
      devices.push_back({source->id, value ? value->generation : 0,
                         source->device_type, source->guest_volume_root,
                         value && value->available,
                         value ? value->managed_format_proof : nullptr,
                         value && value->available && cap!=capabilities.end() ? cap->second.media_proof : nullptr,
                         value && value->available ? value->storage_volume : nullptr});
    }
    if (devices != snapshot.devices) {
      if (snapshot.revision == UINT64_MAX) {
        return false;
      }
      snapshot.devices = std::move(devices);
      ++snapshot.revision;
    }
    return true;
  }
  void ProjectLocked() {
    if (!Project(sources, capabilities, observed, snapshot)) {
      closed.store(true);
    }
  }
  NativeContentStorageSnapshot Refresh() {
    std::vector<ContentSource> selected;
    {
      std::lock_guard lock(mutex);
      if (closed.load()) {
        return snapshot;
      }
      for (const auto& source : sources) {
        if (source.kind == ContentSourceKind::kManaged || source.kind==ContentSourceKind::kSelectedMedia) {
          selected.push_back(source);
        }
      }
    }
    const auto queries = SampleQueries(selected);
    std::lock_guard lock(mutex);
    if (!closed.load()) {
      for (const auto& source : selected) {
        if (HasSourceLocked(source)) {
          ObserveLocked(source, queries);
        }
      }
      ProjectLocked();
    }
    return snapshot;
  }
  const Access access;
  SpaceQuery query;
  const std::shared_ptr<const uint8_t> media_issuer=std::make_shared<const uint8_t>(0);
  std::function<void(const std::string&)> sampled_for_test,prepared_for_test;
  std::mutex mutex;
  std::atomic<bool> closed{false};
  std::vector<ContentSource> sources;
  std::map<std::string, Capability> capabilities;
  std::map<std::string, ContentSourceState> observed;
  NativeContentStorageSnapshot snapshot;
};

std::shared_ptr<const ManagedFormatSourceProof>
ContentSourceRegistry::MintManagedFormatProof(const std::shared_ptr<State>& state,
    const std::string& source, uint64_t generation,
    std::weak_ptr<const filesystem::ManagedDeviceIdentity> identity,
    std::weak_ptr<const filesystem::ManagedVolumeAuthority> volume) {
  auto impl = std::make_unique<ManagedFormatSourceProof::Impl>();
  impl->source_id = source;
  impl->generation = generation;
  impl->identity = std::move(identity);
  impl->volume = std::move(volume);
  impl->current = [weak = std::weak_ptr<State>(state)](const ManagedFormatSourceProof& proof) {
    auto state = weak.lock();
    if (!state || state->closed.load()) { return false; }
    std::unique_lock lock(state->mutex, std::try_to_lock);
    return lock.owns_lock() && state->ProofCurrentLocked(
        proof, proof.impl_->identity, proof.impl_->volume);
  };
  return std::shared_ptr<const ManagedFormatSourceProof>(
      new ManagedFormatSourceProof(std::move(impl)));
}

namespace {
struct SinkFrame {
  const void* source;
  const void* subscription;
  const SinkFrame* previous;
};
thread_local const SinkFrame* active_sink = nullptr;
bool IsInSink(const void* source, const void* subscription = nullptr) {
  for (auto frame = active_sink; frame; frame = frame->previous) {
    if (frame->source == source &&
        (!subscription || frame->subscription == subscription)) {
      return true;
    }
  }
  return false;
}
}  // namespace

class ContentSourceRegistry::Notifications final
    : public NativeContentStorageSource,
      public std::enable_shared_from_this<Notifications> {
 public:
  explicit Notifications(std::shared_ptr<State> state)
      : state_(std::move(state)) {
    latest_.revision = 1;
  }
  ~Notifications() override { Shutdown(); }
  std::optional<Connection> Connect(Sink sink) override;
  void Publish(NativeContentStorageSnapshot snapshot);
  void Shutdown() noexcept;
  void Wake() noexcept { changed_.notify_all(); }

 private:
  struct Entry;
  bool HasSubscribersLocked();
  void Run();
  std::shared_ptr<State> state_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool closed_ = false;
  std::vector<std::weak_ptr<Entry>> subscribers_;
  NativeContentStorageSnapshot latest_;
  std::thread worker_;
};

struct ContentSourceRegistry::Notifications::Entry final
    : NativeContentStorageSource::Subscription {
  Entry(Sink value, std::weak_ptr<Notifications> source)
      : sink(std::make_shared<Sink>(std::move(value))),
        owner(std::move(source)) {}
  ~Entry() override {
    BeginStop();
    Drain();
  }
  void BeginStop() noexcept override {
    accepting.store(false);
    if (auto source = owner.lock()) {
      source->Wake();
    }
  }
  bool Drain() noexcept override {
    BeginStop();
    auto source = owner.lock();
    if (source && IsInSink(source.get(), this)) {
      return false;
    }
    try {
      std::shared_ptr<Sink> release;
      {
        std::unique_lock lock(mutex);
        drained.wait(lock, [&] { return active == 0; });
        release = std::move(sink);
      }
      return true;
    } catch (...) {
      return false;
    }
  }
  void Deliver(const NativeContentStorageSnapshot& snapshot,
               const Notifications* source) {
    std::shared_ptr<Sink> call;
    {
      std::lock_guard lock(mutex);
      if (!accepting.load()) {
        return;
      }
      call = sink;
      ++active;
    }
    const SinkFrame frame{source, this, active_sink};
    active_sink = &frame;
    try {
      (*call)(snapshot);
    } catch (...) {
      // A failing receiver closes only its own producer admission.
      BeginStop();
    }
    active_sink = frame.previous;
    call.reset();
    {
      std::lock_guard lock(mutex);
      --active;
      drained.notify_all();
    }
  }
  std::atomic<bool> accepting{true};
  std::mutex mutex;
  std::condition_variable drained;
  size_t active = 0;
  std::shared_ptr<Sink> sink;
  std::weak_ptr<Notifications> owner;
};

bool ContentSourceRegistry::Notifications::HasSubscribersLocked() {
  std::erase_if(subscribers_, [](const auto& weak) { return weak.expired(); });
  return std::ranges::any_of(subscribers_, [](const auto& weak) {
    const auto entry = weak.lock();
    return entry && entry->accepting.load();
  });
}

std::optional<NativeContentStorageSource::Connection>
ContentSourceRegistry::Notifications::Connect(Sink sink) {
  if (!sink || state_->closed.load()) {
    return std::nullopt;
  }
  try {
    // Refresh before the registration transaction. Publish may have advanced
    // latest_ concurrently; the lock below always selects its newer revision.
    auto snapshot = state_->Refresh();
    auto entry = std::make_shared<Entry>(std::move(sink), weak_from_this());
    std::vector<std::shared_ptr<Entry>> previous;
    NativeContentStorageSnapshot initial;
    {
      std::lock_guard lock(mutex_);
      if (closed_ || state_->closed.load()) {
        return std::nullopt;
      }
      if (snapshot.revision > latest_.revision) {
        latest_ = std::move(snapshot);
        for (const auto& weak : subscribers_) {
          if (auto value = weak.lock()) {
            previous.push_back(std::move(value));
          }
        }
      }
      initial = latest_;
      subscribers_.push_back(entry);
      if (!worker_.joinable()) {
        try {
          // The worker owns state, never a pointer to the registry wrapper.
          worker_ = std::thread([self = shared_from_this()] { self->Run(); });
        } catch (...) {
          subscribers_.pop_back();
          entry->BeginStop();
          return std::nullopt;
        }
      }
    }
    changed_.notify_all();
    for (const auto& value : previous) {
      value->Deliver(initial, this);
    }
    return Connection{std::move(initial), std::move(entry)};
  } catch (...) {
    return std::nullopt;
  }
}

void ContentSourceRegistry::Notifications::Publish(
    NativeContentStorageSnapshot snapshot) {
  std::vector<std::shared_ptr<Entry>> receivers;
  {
    std::lock_guard lock(mutex_);
    if (closed_ || snapshot.revision <= latest_.revision) {
      return;
    }
    latest_ = std::move(snapshot);
    for (const auto& weak : subscribers_) {
      if (auto value = weak.lock()) {
        receivers.push_back(std::move(value));
      }
    }
    snapshot = latest_;
  }
  for (const auto& receiver : receivers) {
    receiver->Deliver(snapshot, this);
  }
}

void ContentSourceRegistry::Notifications::Run() {
  for (;;) {
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] { return closed_ || HasSubscribersLocked(); });
      if (closed_) {
        return;
      }
      if (changed_.wait_for(lock, std::chrono::milliseconds(250), [&] {
            return closed_ || !HasSubscribersLocked();
          })) {
        if (closed_) {
          return;
        }
        continue;
      }
    }
    try {
      Publish(state_->Refresh());
    } catch (...) {
      // Fail closed on an unproved producer refresh rather than terminate the
      // process or manufacture a device transition.
      Shutdown();
      return;
    }
  }
}

void ContentSourceRegistry::Notifications::Shutdown() noexcept {
  std::vector<std::weak_ptr<Entry>> entries;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    entries.swap(subscribers_);
  }
  for (const auto& weak : entries) {
    if (const auto entry = weak.lock()) {
      entry->BeginStop();
    }
  }
  changed_.notify_all();
  const bool from_sink = IsInSink(this);
  if (worker_.joinable()) {
    if (from_sink || worker_.get_id() == std::this_thread::get_id()) {
      // Self shutdown cannot join its own callback. The detached worker still
      // owns this producer/state and exits before dropping that ownership.
      worker_.detach();
    } else {
      worker_.join();
    }
  }
  if (!from_sink) {
    for (const auto& weak : entries) {
      if (const auto entry = weak.lock()) {
        entry->Drain();
      }
    }
  }
}

ContentSourceRegistry::ContentSourceRegistry(SpaceQuery query)
    : ContentSourceRegistry(Access::kNormal, std::move(query)) {}
ContentSourceRegistry::ContentSourceRegistry(Access access, SpaceQuery query)
    : state_(std::make_shared<State>(access, std::move(query))),
      notifications_(std::make_shared<Notifications>(state_)) {}
bool ContentSourceRegistry::read_only_catalog() const {
  return state_->access == Access::kReadOnlyCatalog;
}
ContentSourceRegistry::~ContentSourceRegistry() {
  std::vector<std::shared_ptr<filesystem::MediaSelection>> media;
  std::vector<std::shared_ptr<filesystem::HostStorageRoot>> media_roots;
  state_->closed.store(true);
  {
    std::lock_guard lock(state_->mutex);
    for (auto& [id,capability] : state_->capabilities) {
      if(capability.media_accepting)capability.media_accepting->store(false,std::memory_order_release);
      if(capability.media) {
        media.push_back(std::move(capability.media));
        if(capability.root)media_roots.push_back(std::move(capability.root));
        const auto row=state_->observed.find(id);
        if(row!=state_->observed.end() && row->second.storage)
          media_roots.push_back(std::move(row->second.storage));
      }
      if (capability.storage_volume) { capability.storage_volume->RetireKnownSource(); }
    }
  }
  notifications_->Shutdown();
  for(const auto& selection:media)selection->Retire();
}

std::shared_ptr<NativeContentStorageSource>
ContentSourceRegistry::storage_notifications() const {
  return notifications_;
}

bool ContentSourceRegistry::Register(ContentSource source) {
  return RegisterImpl(std::move(source), {}, 0, false);
}

bool ContentSourceRegistry::RegisterManaged(
    ContentSource source,
    std::shared_ptr<const filesystem::ManagedDeviceIdentity> identity,
    std::shared_ptr<const filesystem::ManagedVolumeAuthority> volume) {
  if (read_only_catalog()) {
    return false;
  }
  if (volume && (volume->error() || volume->identity() != identity)) {
    return false;
  }
  if (!identity || identity->retiring() || identity->pending_format() ||
      identity->source_id() != source.id ||
      identity->device_id() != source.device_id ||
      identity->source()->path() !=
          std::filesystem::absolute(source.root).lexically_normal() ||
      source.kind != ContentSourceKind::kManaged || source.read_only ||
      identity->CheckCurrent()) {
    return false;
  }
  auto authority =
      std::const_pointer_cast<filesystem::HostStorageRoot>(identity->source());
  return RegisterImpl(std::move(source), std::move(authority), 1, false,
                      std::move(identity), std::move(volume));
}

bool ContentSourceRegistry::RegisterReadOnly(
    ContentSource source,
    std::shared_ptr<filesystem::HostStorageRoot> authority,
    uint64_t generation) {
  return RegisterImpl(std::move(source), std::move(authority), generation,
                      true);
}

void ContentSourceRegistry::SetSelectedMediaTimingForTest(
    std::function<void(const std::string&)> sampled,
    std::function<void(const std::string&)> prepared) {
  std::lock_guard lock(state_->mutex);
  state_->sampled_for_test=std::move(sampled);state_->prepared_for_test=std::move(prepared);
}

bool ContentSourceRegistry::RegisterSelectedMedia(
    ContentSource source, std::shared_ptr<filesystem::MediaSelection> selection,
    uint64_t generation, vfs::VirtualFileSystem& fs,
    std::shared_ptr<const SelectedMediaPublication>* publication) {
  if (publication) {
    publication->reset();
  }
  bool published=false;
  try {
  if(global_critical_region::is_held_by_current_thread() || read_only_catalog() ||
      !selection || !generation || source.id.empty() || source.id.size()>128 || source.name.size()>63 || !source.device_id ||
      source.kind!=ContentSourceKind::kSelectedMedia || source.device_type!=DeviceType::USBMASS ||
      !source.read_only || !source.host_volume_root.empty() || !source.private_state_parent.empty() ||
      source.reserved_bytes || source.guest_volume_root.size()!=13 ||
      !source.guest_volume_root.starts_with(R"(\Device\Mass)") ||
      source.guest_volume_root.back()<'0' || source.guest_volume_root.back()>'9')return false;
  if(source.guest_root.empty())source.guest_root=source.guest_volume_root;
  if(source.guest_root!=source.guest_volume_root ||
      !selection->Bind(state_->media_issuer,source.id,generation))return false;
  const auto space=selection->Observe();if(!space.available)return false;
  std::error_code error;std::shared_ptr<filesystem::HostStorageRoot> folder;
  if(selection->folder()) {
    folder=selection->CaptureFolderRoot(error);if(!folder || error)return false;
    if(!source.root.empty() && source.root!=folder->path())return false;source.root=folder->path();
  } else if(!source.root.empty())return false;
  // Retain the actual old node before taking source metadata locks. No graph
  // lookup, observing IO or last native-owner destruction under that mutex.
  const auto mounted=fs.AcquireDevice(source.guest_volume_root);
  uint64_t previous_generation=0;bool previous_record=false;
  std::weak_ptr<vfs::Device> previous_device;
  std::function<void(const std::string&)> checkpoint;
  const auto compatible=[&](const ContentSource& old) {
    return old.kind==source.kind && old.device_id==source.device_id &&
        old.guest_volume_root==source.guest_volume_root && old.guest_root==source.guest_root &&
        old.name==source.name && (old.root.empty() || old.root==source.root);
  };
  {
    std::lock_guard lock(state_->mutex);if(state_->closed.load())return false;
    for(const auto& old:state_->sources) {
      if(old.id!=source.id) {
        if(old.device_id==source.device_id || xe::utf8::equal_case(old.guest_volume_root,source.guest_volume_root))return false;
        continue;
      }
      // A fresh owner's receipt must never adopt or replace an intervening
      // publication, including a compatible metadata-only generation zero.
      if (!compatible(old) || publication) {
        return false;
      }
      const auto& cap = state_->capabilities.at(source.id);
      if (cap.transitioning) {
        return false;
      }
      previous_generation = cap.generation;
      previous_record = true;
      previous_device=cap.media_device;
      if(generation<cap.generation || (generation==cap.generation && cap.media!=selection))return false;
      if (generation == cap.generation) {
        return !publication && !cap.media_unavailable &&
               selection->current_cached() && mounted &&
               !mounted->namespace_retiring() &&
               !cap.media_device.owner_before(mounted) &&
               !mounted.owner_before(cap.media_device);
      }
      if(cap.media_uses && cap.media_uses->load())return false;
      if(mounted && (cap.media_device.owner_before(mounted) || mounted.owner_before(cap.media_device)))return false;
    }
    checkpoint=state_->prepared_for_test;
  }
  if(mounted && !previous_record)return false;
  auto volume=std::shared_ptr<const filesystem::ConsoleStorageVolume>(new filesystem::ConsoleStorageVolume(
      source.device_type,source.device_id,source.id,generation,filesystem::ConsoleStorageVolume::Origin::kRegisteredSource));
  const auto ingress=std::make_shared<std::atomic<bool>>(true);
  std::unique_ptr<vfs::Device> device;
  if(folder)device=std::make_unique<vfs::HostPathDevice>(source.guest_volume_root,folder->path(),true,false,"FATX",folder);
  else device=std::make_unique<vfs::MediaReferenceDevice>(source.guest_volume_root,selection);
  if(!device->BindSelectedMediaReadOnly(ingress) || !device->SetStorageVolume(volume) || !device->Initialize())return false;
  auto* const actual_prepared=device.get();
  auto prepared=fs.PrepareDeviceRegistration(device,error);if(!prepared)return false;
  // The opaque prepared capsule owns this SAME object; no path relookup or
  // postcommit allocation is required to retain its weak native identity.
  auto proof=std::shared_ptr<const SelectedMediaSourceProof>(new SelectedMediaSourceProof(
      state_->media_issuer,source.id,generation,source.device_id,source.guest_volume_root,
      selection,actual_prepared->weak_from_this(),volume,ingress));
  State::Capability next;next.root=folder;next.fixed=true;next.generation=generation;
  next.media=selection;next.media_proof=std::move(proof);next.storage_volume=volume;next.media_physical_available=true;
  auto issued = std::shared_ptr<const SelectedMediaPublication>(
      new SelectedMediaPublication);
  next.media_publication = issued;
  next.media_uses=std::make_shared<std::atomic<size_t>>(0);
  next.media_accepting=ingress;
  ContentSourceState row{source};row.available=true;row.capacity_bytes=space.capacity_bytes;
  row.storage=folder;row.generation=generation;row.selected_media=selection;row.storage_volume=volume;
  if(checkpoint)checkpoint(source.id);
  // These transient transaction copies retain old owners until AFTER all locks
  // on rollback and success. They are not another persistent source registry.
  decltype(state_->sources) staged_sources;
  decltype(state_->capabilities) staged_capabilities;
  decltype(state_->observed) staged_observed;
  NativeContentStorageSnapshot staged_snapshot,snapshot;
  std::shared_ptr<vfs::Device> registered,retired_device;
  std::shared_ptr<filesystem::MediaSelection> retired_selection;
  {
    auto namespace_lock=global_critical_region::AcquireDirect();
    std::lock_guard lock(state_->mutex);
    const auto found=state_->capabilities.find(source.id);
    const bool same_before =
        previous_record
            ? found != state_->capabilities.end() &&
                  found->second.generation == previous_generation &&
                  !found->second.transitioning &&
                  !found->second.media_device.owner_before(previous_device) &&
                  !previous_device.owner_before(found->second.media_device)
            : found == state_->capabilities.end();
    if(state_->closed.load() || !same_before || !selection->current_cached() ||
        state_->snapshot.revision==UINT64_MAX ||
        (found!=state_->capabilities.end() && found->second.media_uses && found->second.media_uses->load()))return false;
    // Cross-ID conflicts must be rechecked in the SAME final admission, even
    // when two different Mass names were prepared concurrently.
    for(const auto& old:state_->sources) {
      if(old.id==source.id) {if(!compatible(old))return false;}
      else if(old.device_id==source.device_id || xe::utf8::equal_case(old.guest_volume_root,source.guest_volume_root))return false;
    }
    staged_sources=state_->sources;staged_capabilities=state_->capabilities;
    staged_observed=state_->observed;staged_snapshot=state_->snapshot;
    const auto definition=std::find_if(staged_sources.begin(),staged_sources.end(),
        [&](const auto& old){return old.id==source.id;});
    if(definition==staged_sources.end())staged_sources.push_back(source);else *definition=source;
    staged_capabilities.insert_or_assign(source.id,std::move(next));
    staged_observed.insert_or_assign(source.id,std::move(row));
    state_->sources.swap(staged_sources);state_->capabilities.swap(staged_capabilities);
    state_->observed.swap(staged_observed);std::swap(state_->snapshot,staged_snapshot);
    const auto rollback=[&]() noexcept {
      state_->sources.swap(staged_sources);state_->capabilities.swap(staged_capabilities);
      state_->observed.swap(staged_observed);std::swap(state_->snapshot,staged_snapshot);
    };
    try {
      state_->ProjectLocked();snapshot=state_->snapshot;
      const auto status=fs.ReplacePreparedDeviceExclusive(*prepared,mounted,registered,retired_device);
      if(XFAILED(status)) {rollback();return false;}
      published=true;
    } catch(...) {rollback();throw;}
    // No allocations/callbacks/observing IO remain after the actual graph swap.
    auto& actual=state_->capabilities.at(source.id);actual.media_device=registered;
    if (publication) {
      *publication = std::move(issued);
    }
    const auto old=staged_capabilities.find(source.id);
    if(old!=staged_capabilities.end()) {
      if(old->second.media_accepting)old->second.media_accepting->store(false,std::memory_order_release);
      if(old->second.storage_volume)old->second.storage_volume->RetireKnownSource();
      retired_selection=std::move(old->second.media);
    }
  }
  if(retired_device)retired_device->Retire();
  if(retired_selection)retired_selection->Retire();
  notifications_->Publish(std::move(snapshot));return true;
  } catch(...) {
    // Before publication every exception is a refusal with the old state kept.
    // A receiver/publication failure cannot undo an already committed mount;
    // the existing refresh producer still owns the actual new source snapshot.
    return published;
  }
}

bool ContentSourceRegistry::RegisterCache(ContentSource source,
    std::shared_ptr<filesystem::CacheStorageOwner> owner,
    filesystem::CacheStorageOwner::Role role,uint64_t generation,vfs::VirtualFileSystem& fs) {
  bool published=false;
  try {
    const size_t slot=size_t(role);constexpr std::array<const char*,3> mounts{R"(\CACHE)",R"(\CACHE0)",R"(\CACHE1)"};
    constexpr std::array<const char*,3> aliases{"cache:","cache0:","cache1:"};
    const auto no_nul=[](const auto& text){return std::find(text.begin(),text.end(),0)==text.end();};
    if(global_critical_region::is_held_by_current_thread() || read_only_catalog() ||
        !owner || !generation || slot>=3 || source.kind!=ContentSourceKind::kCache ||
        source.id.empty() || source.id.size()>128 || !no_nul(source.id) || source.name.size()>63 || !no_nul(source.name) ||
        source.device_type!=DeviceType::HDD || source.read_only || source.device_id!=1 ||
        source.guest_volume_root!=mounts[slot] || source.guest_root!=source.guest_volume_root ||
        !source.host_volume_root.empty() || !source.private_state_parent.empty() ||
        source.reserved_bytes!=filesystem::CacheStorageOwner::kMinimumFreeBytes)return false;
    std::error_code error;const auto folder=owner->CaptureRoot(role,error);
    const auto identity=owner->CaptureIdentity(role);
    if(!folder || error || !identity || source.id!=identity->source_id() ||
        source.device_id!=identity->device_id() || source.root!=folder->path() ||
        !owner->BindRegistry(state_->media_issuer))return false;
    const auto space=folder->Observe();if(!space.available)return false;
    const auto logical=owner->table(error);if(error)return false;
    const uint32_t sectors=slot?logical.sectors_per_cluster[slot-1]:1;
    const auto mounted=fs.AcquireDevice(source.guest_volume_root);
    bool previous_record=false;uint64_t previous_generation=0;
    std::weak_ptr<vfs::Device> previous_device;
    const auto compatible=[&](const ContentSource& old){
      return old.kind==ContentSourceKind::kCache && old.device_id==source.device_id &&
          old.device_type==source.device_type && old.root==source.root && old.guest_root==source.guest_root &&
          old.guest_volume_root==source.guest_volume_root && old.name==source.name && !old.read_only &&
          old.reserved_bytes==source.reserved_bytes && old.host_volume_root.empty() && old.private_state_parent.empty();};
    {
      std::lock_guard lock(state_->mutex);if(state_->closed.load())return false;
      for(const auto& old:state_->sources){
        if(old.id!=source.id){
          if(xe::utf8::equal_case(old.guest_volume_root,source.guest_volume_root))return false;
          continue;
        }
        if(!compatible(old))return false;
        const auto& cap=state_->capabilities.at(source.id);previous_record=true;
        previous_generation=cap.generation;previous_device=cap.cache_device;
        if(generation<cap.generation || (generation==cap.generation && cap.root!=folder))return false;
        if(generation==cap.generation)return !cap.transitioning && mounted && !mounted->namespace_retiring() &&
            !cap.cache_owner.owner_before(owner) && !owner.owner_before(cap.cache_owner) &&
            !cap.cache_device.owner_before(mounted) && !mounted.owner_before(cap.cache_device);
        // HostPathDevice retirement cancels its exact storage Root. A higher
        // namespace generation may replace only a genuine new guest root;
        // sharing the old root would cancel the new device during old rundown.
        if(cap.root==folder)return false;
        if(mounted && (cap.cache_device.owner_before(mounted) || mounted.owner_before(cap.cache_device)))return false;
      }
    }
    if(mounted && !previous_record)return false;
    auto volume=std::shared_ptr<const filesystem::ConsoleStorageVolume>(new filesystem::ConsoleStorageVolume(
        source.device_type,source.device_id,source.id,generation,filesystem::ConsoleStorageVolume::Origin::kRegisteredSource));
    std::unique_ptr<vfs::Device> device=std::make_unique<CacheFolderDevice>(source.guest_volume_root,folder,sectors);
    if(!device->SetStorageVolume(volume) || !device->Initialize())return false;
    auto prepared=fs.PrepareDeviceRegistration(device,error);if(!prepared || error)return false;
    State::Capability next;next.root=folder;next.fixed=true;next.generation=generation;
    next.cache_owner=owner;next.cache_role=role;next.storage_volume=volume;
    ContentSourceState row{source};row.available=true;row.storage=folder;row.generation=generation;
    row.capacity_bytes=space.capacity_bytes;row.free_bytes=space.usable_bytes;row.storage_volume=volume;
    decltype(state_->sources) staged_sources;
    decltype(state_->capabilities) staged_capabilities;
    decltype(state_->observed) staged_observed;
    std::shared_ptr<vfs::Device> registered,retired;
    std::unique_ptr<filesystem::CacheStorageOwner::RegistrationAdmission> admission;
    {
      auto namespace_lock=global_critical_region::AcquireDirect();std::lock_guard lock(state_->mutex);
      const auto found=state_->capabilities.find(source.id);
      const bool same_before=previous_record?found!=state_->capabilities.end() &&
          found->second.generation==previous_generation &&
          !found->second.cache_device.owner_before(previous_device) && !previous_device.owner_before(found->second.cache_device):
          found==state_->capabilities.end();
      if(state_->closed.load() || !same_before || !owner->current_cached() || folder->retiring())return false;
      admission=owner->TryAdmitRegistration(role,folder,identity,sectors);if(!admission)return false;
      for(const auto& old:state_->sources){
        if(old.id==source.id){if(!compatible(old))return false;}
        else if(xe::utf8::equal_case(old.guest_volume_root,source.guest_volume_root))return false;
      }
      staged_sources=state_->sources;staged_capabilities=state_->capabilities;staged_observed=state_->observed;
      const auto definition=std::find_if(staged_sources.begin(),staged_sources.end(),[&](const auto& old){return old.id==source.id;});
      if(definition==staged_sources.end())staged_sources.push_back(source);else *definition=source;
      staged_capabilities.insert_or_assign(source.id,std::move(next));staged_observed.insert_or_assign(source.id,std::move(row));
      state_->sources.swap(staged_sources);state_->capabilities.swap(staged_capabilities);state_->observed.swap(staged_observed);
      const auto rollback=[&]() noexcept {state_->sources.swap(staged_sources);state_->capabilities.swap(staged_capabilities);state_->observed.swap(staged_observed);};
      bool alias_created=false;
      try {
        std::string existing_target;
        if(fs.FindSymbolicLink(aliases[slot],existing_target)){
          // An existing same-target alias is owned only with the exact prior
          // cache record+registered device retained above. No path-only claim.
          if(!previous_record || !mounted || !xe::utf8::equal_case(existing_target,source.guest_volume_root)){
            rollback();return false;}
        }else{
          if(!fs.RegisterSymbolicLink(aliases[slot],source.guest_volume_root)){rollback();return false;}
          alias_created=true;
        }
        const auto status=fs.ReplacePreparedDeviceExclusive(*prepared,mounted,registered,retired);
        if(XFAILED(status)){
          if(alias_created)fs.UnregisterSymbolicLink(aliases[slot]);rollback();return false;
        }
        published=true;
      }catch(...){if(alias_created)fs.UnregisterSymbolicLink(aliases[slot]);rollback();throw;}
      state_->capabilities.at(source.id).cache_device=registered;
      const auto old=staged_capabilities.find(source.id);
      if(old!=staged_capabilities.end() && old->second.storage_volume)old->second.storage_volume->RetireKnownSource();
    }
    admission.reset();
    if(retired)retired->Retire();return true;
  }catch(...){return published;}
}
bool ContentSourceRegistry::WithdrawCache(const std::string& id,uint64_t generation,
    vfs::VirtualFileSystem& fs) {
  if(global_critical_region::is_held_by_current_thread())return false;
  std::shared_ptr<filesystem::HostStorageRoot> root;
  std::shared_ptr<const filesystem::ConsoleStorageVolume> volume;
  std::weak_ptr<vfs::Device> weak;
  std::string mount,alias;
  {
    std::lock_guard lock(state_->mutex);const auto cap=state_->capabilities.find(id);
    const auto source=std::find_if(state_->sources.begin(),state_->sources.end(),[&](const auto& s){return s.id==id;});
    if(state_->closed.load() || cap==state_->capabilities.end() || source==state_->sources.end() ||
        source->kind!=ContentSourceKind::kCache || cap->second.generation!=generation)return false;
    const auto slot=size_t(cap->second.cache_role);if(slot>=3)return false;
    constexpr std::array<const char*,3> aliases{"cache:","cache0:","cache1:"};
    mount=source->guest_volume_root;alias=aliases[slot];
    root=cap->second.root;volume=cap->second.storage_volume;weak=cap->second.cache_device;
    cap->second.transitioning=true;
    if(root)root->Retire();if(volume)volume->RetireKnownSource();
    const auto row=state_->observed.find(id);
    if(row!=state_->observed.end()){row->second.available=false;row->second.storage.reset();row->second.storage_volume.reset();}
  }
  const auto device=weak.lock();
  if(device){
    X_STATUS status;auto plan=fs.PrepareDeviceRetirement(device,true,status,vfs::DeviceRetirementPolicy::kWithdraw);
    if(!plan || XFAILED(status))return false;
    {
      auto namespace_lock=global_critical_region::AcquireDirect();
      const auto current=fs.AcquireDevice(mount);std::string target;
      const bool has_alias=fs.FindSymbolicLink(alias,target);
      // If another caller already detached our device, a name alone cannot
      // prove this surviving alias is still ours. Preserve the closing record
      // until both entries are absent, or exact device ownership is available.
      if(current!=device && (current || has_alias))return false;
      if(XFAILED(fs.CommitDeviceRetirement(*plan)))return false;
      if(current==device && has_alias && xe::utf8::equal_case(target,mount))
        fs.UnregisterSymbolicLink(alias);
    }
    if(XFAILED(fs.RetireDeviceBackings(*plan)))return false;
  }else{
    auto namespace_lock=global_critical_region::AcquireDirect();std::string target;
    if(fs.AcquireDevice(mount) || fs.FindSymbolicLink(alias,target))return false;
  }
  // Exact role record only; foreign same-name/root sources are untouched.
  State::Capability detached;
  {
    std::lock_guard lock(state_->mutex);const auto cap=state_->capabilities.find(id);
    if(cap==state_->capabilities.end() || cap->second.generation!=generation ||
        cap->second.root!=root || !cap->second.transitioning)return false;
    detached=std::move(cap->second);state_->capabilities.erase(cap);state_->observed.erase(id);
    std::erase_if(state_->sources,[&](const auto& source){return source.id==id && source.kind==ContentSourceKind::kCache;});
  }
  return true;
}

bool ContentSourceRegistry::RegisterUnavailableSelectedMedia(
    ContentSource source,
    std::shared_ptr<const SelectedMediaPublication>* publication) {
  if (publication) {
    publication->reset();
  }
  bool published = false;
  try {
    if (read_only_catalog() || source.id.empty() || source.id.size() > 128 ||
        source.name.size() > 63 || !source.device_id ||
        source.kind != ContentSourceKind::kSelectedMedia ||
        !source.root.empty() || source.device_type != DeviceType::USBMASS ||
        !source.read_only || source.reserved_bytes ||
        !source.host_volume_root.empty() ||
        !source.private_state_parent.empty() ||
        source.guest_volume_root.size() != 13 ||
        !source.guest_volume_root.starts_with(R"(\Device\Mass)") ||
        source.guest_volume_root.back() < '0' ||
        source.guest_volume_root.back() > '9') {
      return false;
    }
    if (source.guest_root.empty()) {
      source.guest_root = source.guest_volume_root;
    }
    if (source.guest_root != source.guest_volume_root) {
      return false;
    }
    auto issued = std::shared_ptr<const SelectedMediaPublication>(
        new SelectedMediaPublication);
    State::Capability next;
    next.fixed = true;
    next.media_unavailable = true;
    next.media_publication = issued;
    ContentSourceState row{source};
    // Stage every owning container and the final notification copy before any
    // live mutation. Outer holders drop copied native resources after unlock.
    decltype(state_->sources) staged_sources;
    decltype(state_->capabilities) staged_capabilities;
    decltype(state_->observed) staged_observed;
    NativeContentStorageSnapshot staged_snapshot, snapshot;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->closed.load()) {
        return false;
      }
      for (const auto& old : state_->sources) {
        if (old.id == source.id) {
          const auto& cap = state_->capabilities.at(source.id);
          return !publication && old.kind == source.kind &&
                 !cap.transitioning && !cap.media && !cap.generation &&
                 old.device_id == source.device_id &&
                 old.guest_volume_root == source.guest_volume_root &&
                 old.name == source.name;
        }
        if (old.device_id == source.device_id ||
            xe::utf8::equal_case(old.guest_volume_root,
                                 source.guest_volume_root)) {
          return false;
        }
      }
      staged_sources = state_->sources;
      staged_capabilities = state_->capabilities;
      staged_observed = state_->observed;
      staged_snapshot = state_->snapshot;
      staged_sources.push_back(source);
      if (!staged_capabilities.emplace(source.id, std::move(next)).second) {
        return false;
      }
      staged_observed.insert_or_assign(source.id, std::move(row));
      if (!State::Project(staged_sources, staged_capabilities, staged_observed,
                          staged_snapshot)) {
        return false;
      }
      snapshot = staged_snapshot;
      // No throwing allocation or callback remains in this actual commit.
      state_->sources.swap(staged_sources);
      state_->capabilities.swap(staged_capabilities);
      state_->observed.swap(staged_observed);
      std::swap(state_->snapshot, staged_snapshot);
      if (publication) {
        *publication = std::move(issued);
      }
      published = true;
    }
    notifications_->Publish(std::move(snapshot));
    return true;
  } catch (...) {
    // Before commit no source row was exposed. After commit preserve the actual
    // issued ownership; notification failure cannot undo a real publication.
    return published;
  }
}

bool ContentSourceRegistry::OwnsSelectedMedia(
    const ContentSourceState& expected,
    const std::shared_ptr<const SelectedMediaPublication>& publication) const {
  if (!publication) {
    return false;
  }
  std::lock_guard lock(state_->mutex);
  const auto found = state_->capabilities.find(expected.source.id);
  return !state_->closed.load() && found != state_->capabilities.end() &&
         found->second.media_publication == publication &&
         found->second.generation == expected.generation &&
         found->second.media == expected.selected_media;
}

bool ContentSourceRegistry::SelectedMediaAvailable(
    const std::string& id,
    const std::shared_ptr<const SelectedMediaPublication>& publication) const {
  if (!publication) {
    return false;
  }
  std::lock_guard lock(state_->mutex);
  const auto found = state_->capabilities.find(id);
  const auto row = state_->observed.find(id);
  if (state_->closed.load() || found == state_->capabilities.end() ||
      row == state_->observed.end()) {
    return false;
  }
  const auto& cap = found->second;
  return cap.media_publication == publication && !cap.transitioning &&
         !cap.media_unavailable && cap.media && cap.media->current_cached() &&
         cap.media_accepting &&
         cap.media_accepting->load(std::memory_order_acquire) &&
         cap.storage_volume && cap.storage_volume->available_cached() &&
         (!cap.root || !cap.root->retiring()) && row->second.available &&
         row->second.generation == cap.generation &&
         row->second.selected_media == cap.media;
}

bool ContentSourceRegistry::WithdrawSelectedMedia(
    const std::string& id, uint64_t generation,
    const std::shared_ptr<const SelectedMediaPublication>& expected) {
  NativeContentStorageSnapshot snapshot;std::shared_ptr<filesystem::MediaSelection> retired;
  {
    std::lock_guard lock(state_->mutex);const auto found=state_->capabilities.find(id);
    if (state_->closed.load() || found == state_->capabilities.end() ||
        !found->second.media || found->second.generation != generation ||
        (expected && found->second.media_publication != expected)) {
      return false;
    }
    auto& cap=found->second;cap.media_unavailable=true;retired=cap.media;
    cap.media_forget_pending = cap.media_forget_pending || bool(expected);
    cap.media_draining =
        cap.media_forget_pending || (cap.media_uses && cap.media_uses->load());
    if(cap.media_accepting)cap.media_accepting->store(false,std::memory_order_release);
    if(cap.storage_volume)cap.storage_volume->RetireKnownSource();
    auto row=state_->observed.find(id);if(row!=state_->observed.end()) {row->second.available=false;row->second.storage.reset();row->second.storage_volume.reset();}
    state_->ProjectLocked();snapshot=state_->snapshot;
  }
  retired->CloseIngress();
  bool draining=false;{std::lock_guard lock(state_->mutex);const auto f=state_->capabilities.find(id);
    draining = f != state_->capabilities.end() &&
               f->second.generation == generation &&
               f->second.media == retired && f->second.media_draining;
  }
  if(!draining)retired->Retire();notifications_->Publish(std::move(snapshot));return true;
}

bool ContentSourceRegistry::ForgetSelectedMedia(
    const std::string& id, uint64_t generation, vfs::VirtualFileSystem& fs,
    const std::shared_ptr<const SelectedMediaPublication>& expected) {
  if (global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  std::shared_ptr<vfs::Device> device;
  std::shared_ptr<const SelectedMediaPublication> actual;
  std::string mount;
  {
    std::lock_guard lock(state_->mutex);
    const auto cap = state_->capabilities.find(id);
    const auto source =
        std::find_if(state_->sources.begin(), state_->sources.end(),
                     [&](const auto& row) { return row.id == id; });
    if (state_->closed.load() || cap == state_->capabilities.end() ||
        source == state_->sources.end() ||
        source->kind != ContentSourceKind::kSelectedMedia ||
        cap->second.generation != generation ||
        (expected && cap->second.media_publication != expected) ||
        !cap->second.media_unavailable || cap->second.transitioning ||
        (cap->second.media_uses && cap->second.media_uses->load())) {
      return false;
    }
    device = cap->second.media_device.lock();
    actual = cap->second.media_publication;
    mount = source->guest_volume_root;
    cap->second.transitioning = true;
  }
  // This denial reservation is released on EVERY refusal. No observing IO,
  // last resource drop or notification callback occurs under registry locks.
  struct Reservation {
    std::shared_ptr<State> state;
    const std::string& id;
    uint64_t generation;
    std::shared_ptr<const SelectedMediaPublication> actual;
    bool done = false;
    ~Reservation() {
      if (!done) {
        std::lock_guard lock(state->mutex);
        const auto it = state->capabilities.find(id);
        if (it != state->capabilities.end() &&
            it->second.generation == generation &&
            it->second.media_publication == actual) {
          it->second.transitioning = false;
        }
      }
    }
  } reservation{state_, id, generation, actual};
  if (device) {
    X_STATUS status;
    auto retired = fs.PrepareDeviceRetirement(
        device, true, status, vfs::DeviceRetirementPolicy::kDrainOpenFiles);
    if (!retired || XFAILED(status)) {
      return false;
    }
    {
      auto namespace_lock = global_critical_region::AcquireDirect();
      const auto current = fs.AcquireDevice(mount);
      if ((current && current != device) ||
          XFAILED(fs.CommitDeviceRetirement(*retired))) {
        return false;
      }
    }
    if (XFAILED(fs.RetireDeviceBackings(*retired))) {
      return false;
    }
  } else {
    auto namespace_lock = global_critical_region::AcquireDirect();
    if (fs.AcquireDevice(mount)) {
      return false;
    }
  }
  State::Capability detached;
  NativeContentStorageSnapshot snapshot;
  {
    std::lock_guard lock(state_->mutex);
    const auto cap = state_->capabilities.find(id);
    if (state_->closed.load() || cap == state_->capabilities.end() ||
        cap->second.generation != generation ||
        cap->second.media_publication != actual || !cap->second.transitioning ||
        !cap->second.media_unavailable ||
        (cap->second.media_uses && cap->second.media_uses->load())) {
      return false;
    }
    detached = std::move(cap->second);
    state_->capabilities.erase(cap);
    std::erase_if(state_->sources,
                  [&](const auto& row) { return row.id == id; });
    state_->observed.erase(id);
    state_->ProjectLocked();
    snapshot = state_->snapshot;
    reservation.done = true;
  }
  // Only real VFS handle/backing rundown above authorizes final backing
  // release. In particular a zero SelectedMediaUse count alone does not
  // authorize it.
  if (detached.media) {
    detached.media->Retire();
  }
  notifications_->Publish(std::move(snapshot));
  return true;
}

std::shared_ptr<filesystem::MediaSelection> ContentSourceRegistry::CaptureSelectedMedia(
    const ContentSourceState& expected) const {
  if(!expected.available || expected.observation_error || !expected.selected_media || !expected.storage_volume)return {};
  std::lock_guard lock(state_->mutex);const auto found=state_->capabilities.find(expected.source.id);
  if(state_->closed.load() || found==state_->capabilities.end())return {};
  const auto& cap=found->second;
  const auto row=state_->observed.find(expected.source.id);
  if(row==state_->observed.end() || !row->second.available ||
      expected.source.kind!=ContentSourceKind::kSelectedMedia ||
      expected.source.device_id!=row->second.source.device_id ||
      expected.source.device_type!=row->second.source.device_type ||
      !expected.source.read_only || expected.source.root!=row->second.source.root ||
      expected.storage!=cap.root)return {};
  if(cap.media_unavailable || cap.generation!=expected.generation || cap.media!=expected.selected_media ||
      cap.storage_volume!=expected.storage_volume || !expected.storage_volume->available_cached() ||
      !cap.media->current_cached())return {};
  return cap.media;
}

struct SelectedMediaUse::Impl {
  std::shared_ptr<std::atomic<size_t>> uses;
  std::shared_ptr<std::atomic<bool>> accepting;
  ~Impl(){if(uses)uses->fetch_sub(1,std::memory_order_acq_rel);}
};
SelectedMediaUse::SelectedMediaUse(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
SelectedMediaUse::~SelectedMediaUse()=default;
bool SelectedMediaUse::current() const noexcept {return impl_->accepting->load(std::memory_order_acquire);}
filesystem::HostStorageSnapshot ContentSourceRegistry::ProbeSelected(
    const std::shared_ptr<filesystem::MediaSelection>& media,bool draining) {
  return draining?media->ProbeRetained():media->Observe();
}
std::shared_ptr<SelectedMediaUse> ContentSourceRegistry::CaptureSelectedMediaUse(
    const ContentSourceState& expected) const {
  const auto selected=CaptureSelectedMedia(expected);if(!selected)return {};
  auto impl=std::make_unique<SelectedMediaUse::Impl>();
  {
    std::lock_guard lock(state_->mutex);const auto cap=state_->capabilities.find(expected.source.id);
    if(state_->closed.load() || cap==state_->capabilities.end() || cap->second.media!=selected ||
        cap->second.generation!=expected.generation || cap->second.media_unavailable || !selected->current_cached() ||
        !cap->second.media_uses || !cap->second.media_accepting ||
        !cap->second.media_accepting->load() || cap->second.media_uses->load()==SIZE_MAX)return {};
    cap->second.media_uses->fetch_add(1,std::memory_order_acq_rel);
    impl->uses=cap->second.media_uses;impl->accepting=cap->second.media_accepting;
  }
  return std::shared_ptr<SelectedMediaUse>(new SelectedMediaUse(std::move(impl)));
}

std::shared_ptr<SelectedMediaUse> ContentSourceRegistry::CaptureSelectedMediaUse(
    const std::shared_ptr<const SelectedMediaSourceProof>& proof,std::error_code& error) const {
  error.clear();
  if(!proof || !proof->SourceCurrentCached()) {
    error=std::make_error_code(std::errc::no_such_device);return {};
  }
  // All allocation is before the nonwaiting cached admission. A rejected token
  // owns no count; admitted token destruction decrements the same atom only.
  auto result=std::shared_ptr<SelectedMediaUse>(new SelectedMediaUse(
      std::make_unique<SelectedMediaUse::Impl>()));
  {
    std::unique_lock lock(state_->mutex,std::try_to_lock);
    if(!lock.owns_lock()) {error=std::make_error_code(std::errc::operation_would_block);return {};}
    const auto cap=state_->capabilities.find(proof->source_id());
    const auto row=state_->observed.find(proof->source_id());
    if(state_->closed.load() || cap==state_->capabilities.end() || row==state_->observed.end() ||
        cap->second.media_proof!=proof || cap->second.generation!=proof->generation() ||
        cap->second.media_unavailable || !cap->second.media || !cap->second.media->current_cached() ||
        !row->second.available || row->second.observation_error ||
        !cap->second.media_uses || !cap->second.media_accepting || !cap->second.media_accepting->load() ||
        !cap->second.storage_volume || !cap->second.storage_volume->available_cached() ||
        cap->second.media_uses->load()==SIZE_MAX || !proof->SourceCurrentCached()) {
      error=std::make_error_code(std::errc::no_such_device);return {};
    }
    cap->second.media_uses->fetch_add(1,std::memory_order_acq_rel);
    result->impl_->uses=cap->second.media_uses;result->impl_->accepting=cap->second.media_accepting;
  }
  return result;
}

bool ContentSourceRegistry::RegisterImpl(
    ContentSource source,
    std::shared_ptr<filesystem::HostStorageRoot> authority, uint64_t generation,
    bool fixed,
    std::shared_ptr<const filesystem::ManagedDeviceIdentity> managed,
    std::shared_ptr<const filesystem::ManagedVolumeAuthority> volume) {
  const auto state = state_;
  const auto notifications = notifications_;
  // This immutable mode cannot be escalated by any public registration path.
  // References remain catalog records, never managed physical volumes.
  if (state->access == Access::kReadOnlyCatalog &&
      (!source.read_only || source.kind == ContentSourceKind::kManaged ||
       managed || volume)) {
    return false;
  }
  if (source.kind==ContentSourceKind::kSelectedMedia || source.kind==ContentSourceKind::kCache ||
      source.id.empty() || !source.device_id || source.root.empty() ||
      !filesystem::IsConsoleStorageType(source.device_type) ||
      (!source.read_only && source.kind != ContentSourceKind::kManaged) ||
      (fixed && (!generation || !source.read_only ||
                 source.kind != ContentSourceKind::kInstalledSystem))) {
    return false;
  }
  std::error_code error;
  source.root =
      std::filesystem::absolute(source.root, error).lexically_normal();
  if (error) {
    return false;
  }
  std::shared_ptr<filesystem::HostStorageRoot> view;
  if (authority) {
    if ((!fixed && !managed) || (fixed && !authority->read_only()) || authority->path() != source.root ||
        !authority->Observe().available) {
      return false;
    }
    // Guest generation retirement must not cancel the marker/format owner.
    // Both fixed and managed sources publish independently retired children.
    view = authority->GuestView(error);
    if (!view || error) {
      return false;
    }
  }
  const auto queries = state->SampleQueries({source});
  NativeContentStorageSnapshot snapshot;
  std::shared_ptr<filesystem::HostStorageRoot> retired;
  {
    std::lock_guard lock(state->mutex);
    if (state->closed.load()) {
      return false;
    }
    bool existing_id = false;
    for (const auto& existing : state->sources) {
      // The selected-media physical USB identity is exclusive in both entry
      // directions. Managed installed-system overlays retain their old policy.
      if(existing.kind==ContentSourceKind::kSelectedMedia && existing.id!=source.id &&
          (existing.device_id==source.device_id ||
           xe::utf8::equal_case(existing.guest_volume_root,source.guest_volume_root)))return false;
      if (existing.id == source.id) {
        if (existing.device_id != source.device_id ||
            existing.device_type != source.device_type ||
            existing.kind != source.kind || existing.root != source.root ||
            existing.guest_root != source.guest_root ||
            existing.name != source.name ||
            existing.read_only != source.read_only ||
            existing.reserved_bytes != source.reserved_bytes ||
            existing.guest_volume_root != source.guest_volume_root ||
            existing.host_volume_root != source.host_volume_root ||
            existing.private_state_parent != source.private_state_parent) {
          return false;
        }
        auto& capability = state->capabilities[source.id];
        if (capability.fixed != fixed) {
          return false;
        }
        if (managed) {
          if (capability.transitioning ||
              capability.managed_identity != managed ||
              capability.authority != authority || capability.volume != volume ||
              !capability.root || capability.root->retiring()) {
            return false;
          }
          return true;
        }
        if (!fixed) {
          return !capability.managed_identity;
        }
        if (generation < capability.generation ||
            (generation == capability.generation &&
             authority != capability.authority)) {
          return false;
        }
        if (generation == capability.generation) {
          return true;
        }
        if (capability.storage_volume) { capability.storage_volume->RetireKnownSource(); }
        retired = std::move(capability.root);
        capability = {std::move(view), generation, true, {}, std::move(authority)};
        existing_id = true;
        break;
      }
      if (ConflictsWithContentDevice(existing,source)) {
        return false;
      }
    }
    if (!existing_id) {
      state->sources.push_back(source);
      if (fixed || managed) {
        state->capabilities[source.id] = {std::move(view), generation, fixed,
                                          std::move(managed),
                                          std::move(authority), std::move(volume)};
      }
    }
    state->ObserveLocked(source, queries);
    state->ProjectLocked();
    snapshot = state->snapshot;
  }
  if (retired) {
    retired->Retire();
  }
  notifications->Publish(std::move(snapshot));
  return true;
}

struct ContentSourceRegistry::ManagedTransition::Impl {
  std::shared_ptr<State> state;
  std::shared_ptr<Notifications> notifications;
  ContentSource source;
  uint64_t old_generation = 0, next_generation = 0;
  std::shared_ptr<filesystem::HostStorageRoot> old_view, authority;
  std::shared_ptr<const filesystem::ManagedDeviceIdentity> identity;
  NativeContentStorageSnapshot withdrawal;
  bool withdrawal_published = false, finished = false;
};
ContentSourceRegistry::ManagedTransition::ManagedTransition(
    std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ContentSourceRegistry::ManagedTransition::~ManagedTransition() = default;
void ContentSourceRegistry::ManagedTransition::PublishWithdrawal() {
  if (!impl_->withdrawal_published) {
    impl_->withdrawal_published = true;
    impl_->notifications->Publish(impl_->withdrawal);
  }
}
std::unique_ptr<ContentSourceRegistry::ManagedTransition>
ContentSourceRegistry::BeginManagedTransition(
    const std::string& id, uint64_t generation,
    const std::shared_ptr<const filesystem::ManagedVolumeAuthority>& expected,
    std::vector<std::shared_ptr<const filesystem::ManagedVolumeAuthority>>& drop,
    std::error_code& error,
    std::shared_ptr<const ManagedFormatSourceProof> expected_proof) {
  error.clear();
  if (!expected || expected->error() || !drop.empty()) {
    error = std::make_error_code(std::errc::invalid_argument); return {};
  }
  auto impl = std::make_unique<ManagedTransition::Impl>();
  impl->state = state_; impl->notifications = notifications_;
  // Reserve the release-holder slots BEFORE locking/retiring anything.
  drop.reserve(2);
  {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    const auto found = state_->capabilities.find(id);
    const auto config = std::find_if(state_->sources.begin(), state_->sources.end(),
        [&](const auto& source) { return source.id == id; });
    if (state_->closed.load() || found == state_->capabilities.end() ||
        config == state_->sources.end() || config->kind != ContentSourceKind::kManaged ||
        found->second.fixed || found->second.transitioning ||
        found->second.generation != generation || generation == UINT64_MAX ||
        found->second.volume != expected ||
        found->second.managed_identity != expected->identity() ||
        found->second.authority != expected->content() ||
        !found->second.root || found->second.root->retiring() ||
        (expected_proof &&
         (found->second.managed_format_proof != expected_proof ||
          !state_->ProofCurrentLocked(*expected_proof, expected_proof->impl_->identity,
                                     expected_proof->impl_->volume)))) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    auto& cap = found->second;
    impl->source = *config;
    impl->old_generation = generation; impl->next_generation = generation + 1;
    impl->identity = cap.managed_identity; impl->authority = cap.authority;
    impl->old_view = std::move(cap.root);
    if (cap.storage_volume) { cap.storage_volume->RetireKnownSource(); }
    cap.storage_volume.reset();
    cap.transitioning = true;
    drop.push_back(std::move(cap.volume));
    if (auto row = state_->observed.find(id); row != state_->observed.end()) {
      drop.push_back(std::move(row->second.volume));
      row->second.storage.reset();
      row->second.available = row->second.profile_writable =
          row->second.format_eligible = false;
      row->second.capacity_bytes = row->second.free_bytes = 0;
      row->second.managed_format_proof.reset();
    }
    // Pure atomic retirement; authority/identity remain current for lowFormat.
    impl->old_view->Retire();
    state_->ProjectLocked();
    impl->withdrawal = state_->snapshot;
  }
  return std::unique_ptr<ManagedTransition>(new ManagedTransition(std::move(impl)));
}
std::unique_ptr<ContentSourceRegistry::ManagedTransition>
ContentSourceRegistry::BeginManagedRecovery(
    ContentSource source, const filesystem::ManagedVolumeFormatHandoff& recovery,
    std::error_code& error) {
  return BeginManagedRecoveryOwned(state_, notifications_, std::move(source), recovery, error);
}
ContentSourceRegistry::ManagedRecoveryAdmission
ContentSourceRegistry::CaptureManagedRecoveryAdmission(ContentSource source) const {
  return [state = state_, notifications = notifications_, source = std::move(source)](
      const filesystem::ManagedVolumeFormatHandoff& recovery, std::error_code& error) {
    return BeginManagedRecoveryOwned(state, notifications, source, recovery, error);
  };
}
std::unique_ptr<ContentSourceRegistry::ManagedTransition>
ContentSourceRegistry::BeginManagedRecoveryOwned(
    std::shared_ptr<State> state, std::shared_ptr<Notifications> notifications,
    ContentSource source, const filesystem::ManagedVolumeFormatHandoff& recovery,
    std::error_code& error) {
  error.clear();
  const auto identity = recovery.captured_identity();
  if (state->access != Access::kNormal || recovery.error() ||
      recovery.phase() < 1 || recovery.phase() > 3 || !identity ||
      source.kind != ContentSourceKind::kManaged || source.read_only ||
      source.id != identity->source_id() || source.device_id != identity->device_id() ||
      !filesystem::IsConsoleStorageType(source.device_type) || source.root != identity->source()->path()) {
    error = std::make_error_code(std::errc::invalid_argument); return {};
  }
  auto impl = std::make_unique<ManagedTransition::Impl>();
  impl->state = state; impl->notifications = notifications;
  impl->source = source;
  impl->identity = identity;
  impl->authority = std::const_pointer_cast<filesystem::HostStorageRoot>(identity->source());
  impl->next_generation = 1;
  {
    std::unique_lock lock(state->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    if (state->closed.load() || state->capabilities.contains(source.id) ||
        std::any_of(state->sources.begin(), state->sources.end(), [&](const auto& held) {
          return held.id == source.id || ConflictsWithContentDevice(held,source);
        })) {
      error = std::make_error_code(std::errc::state_not_recoverable); return {};
    }
    state->sources.push_back(source);
    state->capabilities.emplace(source.id, State::Capability{
        {}, 0, false, identity, impl->authority, {}, true});
    ContentSourceState unavailable{source};
    unavailable.managed_identity = identity;
    state->observed.emplace(source.id, std::move(unavailable));
    state->ProjectLocked(); impl->withdrawal = state->snapshot;
  }
  return std::unique_ptr<ManagedTransition>(new ManagedTransition(std::move(impl)));
}
std::error_code ContentSourceRegistry::ManagedTransition::Publish(
    std::shared_ptr<const filesystem::ManagedVolumeAuthority> volume) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return std::make_error_code(std::errc::operation_would_block);
  }
  if (impl_->finished || !volume || volume->error() || !volume->profile_writable() ||
      volume->identity()->source_id() != impl_->source.id ||
      volume->identity()->device_id() != impl_->source.device_id ||
      volume->identity()->uuid() != impl_->identity->uuid() ||
      volume->content()->path() != impl_->authority->path()) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  auto error = volume->identity()->CheckCurrent();
  if (error) { return error; }
  auto view = volume->content()->GuestView(error);
  if (!view) { return error; }
  const auto queries = impl_->state->SampleQueries({impl_->source});
  NativeContentStorageSnapshot snapshot;
  {
    std::lock_guard lock(impl_->state->mutex);
    auto found = impl_->state->capabilities.find(impl_->source.id);
    if (impl_->state->closed.load() || found == impl_->state->capabilities.end() ||
        !found->second.transitioning ||
        found->second.generation != impl_->old_generation ||
        found->second.managed_identity != impl_->identity ||
        found->second.authority != impl_->authority || found->second.volume ||
        found->second.root) {
      return std::make_error_code(std::errc::state_not_recoverable);
    }
    found->second = {std::move(view), impl_->next_generation, false,
                    volume->identity(), volume->content(), std::move(volume), false};
    impl_->state->ObserveLocked(impl_->source, queries);
    impl_->state->ProjectLocked(); snapshot = impl_->state->snapshot;
    impl_->finished = true;
  }
  PublishWithdrawal();
  impl_->notifications->Publish(std::move(snapshot));
  return {};
}

std::vector<uint32_t> ContentSourceRegistry::DeviceIds() const {
  const auto state = state_;
  std::vector<uint32_t> result;
  std::lock_guard lock(state->mutex);
  for (const auto& source : state->sources) {
    if(ParticipatesInContentDevices(source))result.push_back(source.device_id);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<ContentSourceState> ContentSourceRegistry::Sources(
    uint32_t device_id) const {
  const auto state = state_;
  const auto notifications = notifications_;
  std::vector<ContentSource> selected;
  {
    std::lock_guard lock(state->mutex);
    for (const auto& source : state->sources) {
      if (ParticipatesInContentDevices(source) && source.device_id == device_id) {
        selected.push_back(source);
      }
    }
  }
  const auto queries = state->SampleQueries(selected);
  std::vector<ContentSourceState> result;
  NativeContentStorageSnapshot snapshot;
  {
    std::lock_guard lock(state->mutex);
    for (const auto& source : selected) {
      if (state->HasSourceLocked(source)) {
        result.push_back(state->ObserveLocked(source, queries));
      }
    }
    state->ProjectLocked();
    snapshot = state->snapshot;
  }
  notifications->Publish(std::move(snapshot));
  return result;
}

std::optional<ContentSourceState> ContentSourceRegistry::Source(
    const std::string& id) const {
  const auto state = state_;
  const auto notifications = notifications_;
  std::vector<ContentSource> selected;
  {
    std::lock_guard lock(state->mutex);
    for (const auto& source : state->sources) {
      if (source.id == id) {
        selected.push_back(source);
        break;
      }
    }
  }
  const auto queries = state->SampleQueries(selected);
  std::optional<ContentSourceState> result;
  NativeContentStorageSnapshot snapshot;
  {
    std::lock_guard lock(state->mutex);
    if (!selected.empty() && state->HasSourceLocked(selected.front())) {
      result = state->ObserveLocked(selected.front(), queries);
    }
    state->ProjectLocked();
    snapshot = state->snapshot;
  }
  notifications->Publish(std::move(snapshot));
  return result;
}

std::optional<ContentSourceState> ContentSourceRegistry::WritableSource(
    uint32_t device_id) const {
  for (const auto& state : Sources(device_id)) {
    if (state.available && !state.source.read_only) {
      return state;
    }
  }
  return std::nullopt;
}

std::shared_ptr<const filesystem::ConsoleStorageVolume>
ContentSourceRegistry::CaptureStorageVolume(const ContentSourceState& expected) const {
  if(expected.selected_media) {
    return CaptureSelectedMedia(expected) ? expected.storage_volume : nullptr;
  }
  if (!expected.available || !expected.storage || !expected.storage_volume) { return {}; }
  std::lock_guard lock(state_->mutex);
  const auto cap=state_->capabilities.find(expected.source.id);
  const auto observed=state_->observed.find(expected.source.id);
  if (state_->closed.load() || cap==state_->capabilities.end() ||
      observed==state_->observed.end()) { return {}; }
  const auto& actual=observed->second;
  if (cap->second.transitioning || cap->second.root!=expected.storage ||
      cap->second.generation!=expected.generation || expected.storage->retiring() ||
      cap->second.storage_volume!=expected.storage_volume || !actual.available ||
      actual.storage!=expected.storage || actual.generation!=expected.generation ||
      actual.source.device_id!=expected.source.device_id ||
      actual.source.device_type!=expected.source.device_type ||
      actual.source.root!=expected.source.root || actual.source.kind!=expected.source.kind ||
      cap->second.managed_identity!=expected.managed_identity ||
      cap->second.volume!=expected.volume || !expected.storage_volume->available_cached()) { return {}; }
  return expected.storage_volume;
}

std::optional<ContentSourceRegistry::SourceGenerationAdmission>
ContentSourceRegistry::TryAdmitSourceGeneration(
    const ContentSourceState& expected,std::error_code& error) const noexcept try {
  error.clear();
  if(!global_critical_region::is_held_by_current_thread()) {
    error=std::make_error_code(std::errc::operation_would_block);return {};
  }
  // Selected-media attachment has its own issued use admission. It cannot
  // borrow this managed/installed Content capability by filling a descriptor.
  if(!expected.available || expected.selected_media || !expected.storage ||
      !expected.storage_volume) {
    error=std::make_error_code(std::errc::no_such_device);return {};
  }
  SourceGenerationAdmission admission(state_->mutex);
  if(!admission.lock_.owns_lock()) {
    error=std::make_error_code(std::errc::device_or_resource_busy);return {};
  }
  const auto cap=state_->capabilities.find(expected.source.id);
  const auto row=state_->observed.find(expected.source.id);
  if(state_->closed.load() || cap==state_->capabilities.end() || row==state_->observed.end()) {
    error=std::make_error_code(std::errc::no_such_device);return {};
  }
  const auto& actual=row->second;
  const auto same_owner=[](const auto& a,const auto& b) {
    return a==b && !a.owner_before(b) && !b.owner_before(a);
  };
  if(cap->second.transitioning || !same_owner(cap->second.root,expected.storage) ||
      cap->second.generation!=expected.generation || expected.storage->retiring() ||
      !same_owner(cap->second.storage_volume,expected.storage_volume) || !actual.available ||
      !same_owner(actual.storage,expected.storage) || actual.generation!=expected.generation ||
      actual.source.device_id!=expected.source.device_id ||
      actual.source.device_type!=expected.source.device_type ||
      actual.source.root!=expected.source.root || actual.source.kind!=expected.source.kind ||
      actual.source.read_only!=expected.source.read_only ||
      actual.source.guest_root!=expected.source.guest_root ||
      actual.source.guest_volume_root!=expected.source.guest_volume_root ||
      !same_owner(cap->second.managed_identity,expected.managed_identity) ||
      !same_owner(cap->second.volume,expected.volume) ||
      !expected.storage_volume->available_cached()) {
    error=std::make_error_code(std::errc::no_such_device);return {};
  }
  return std::optional<SourceGenerationAdmission>(std::move(admission));
} catch(...) {error=std::make_error_code(std::errc::io_error);return {};}

std::optional<ContentSourceRegistry::SourceGenerationAdmission>
ContentSourceRegistry::TryAdmitSourceGeneration(
    const ContentSourceDescription& expected,std::error_code& error) const noexcept {
  return TryAdmitSourceGenerations(std::span<const ContentSourceDescription>(&expected,1),error);
}
std::optional<ContentSourceRegistry::SourceGenerationAdmission>
ContentSourceRegistry::TryAdmitSourceGenerations(
    std::span<const ContentSourceDescription> expected,std::error_code& error) const noexcept try {
  error.clear();
  if(!global_critical_region::is_held_by_current_thread()) {
    error=std::make_error_code(std::errc::operation_would_block);return {};
  }
  if(expected.empty()) {error=std::make_error_code(std::errc::invalid_argument);return {};}
  SourceGenerationAdmission admission(state_->mutex);
  if(!admission.lock_.owns_lock()) {
    error=std::make_error_code(std::errc::device_or_resource_busy);return {};
  }
  for(const auto& value:expected) {
    const auto* row=state_->CachedDescriptionLocked(value.identity);
    if(!row || row->generation!=value.generation ||
        row->source.id!=value.source.id || row->source.kind!=value.source.kind ||
        row->source.device_id!=value.source.device_id || row->source.device_type!=value.source.device_type ||
        row->source.read_only!=value.source.read_only || row->source.root!=value.source.root ||
        row->source.guest_root!=value.source.guest_root ||
        row->source.guest_volume_root!=value.source.guest_volume_root) {
      error=std::make_error_code(std::errc::no_such_device);return {};
    }
  }
  return std::optional<SourceGenerationAdmission>(std::move(admission));
} catch(...) {error=std::make_error_code(std::errc::io_error);return {};}

std::optional<ContentSourceState> ContentSourceRegistry::CaptureSourceCached(
    const std::shared_ptr<const filesystem::ConsoleStorageVolume>& volume,
    std::error_code& error) const {
  error.clear();
  // All actual retained resource destruction follows the lock on every tail.
  std::optional<ContentSourceState> captured;
  std::unique_lock lock(state_->mutex,std::try_to_lock);
  if(!lock.owns_lock()) {
    error=std::make_error_code(std::errc::device_or_resource_busy);return {};
  }
  const auto* row=state_->CachedVolumeLocked(volume);
  if(!row){error=std::make_error_code(std::errc::no_such_device);return {};}
  captured=*row;return captured;
}
std::optional<ContentSourceDescription> ContentSourceRegistry::DescribeSourceCached(
    const std::shared_ptr<const filesystem::ConsoleStorageVolume>& volume,
    std::error_code& error) const {
  error.clear();std::optional<ContentSourceDescription> described;
  std::unique_lock lock(state_->mutex,std::try_to_lock);
  if(!lock.owns_lock()) {
    error=std::make_error_code(std::errc::device_or_resource_busy);return {};
  }
  const auto* row=state_->CachedDescriptionLocked(volume);
  if(!row){error=std::make_error_code(std::errc::no_such_device);return {};}
  described.emplace();described->source=row->source;described->generation=row->generation;
  described->capacity_bytes=row->capacity_bytes;described->free_bytes=row->free_bytes;
  described->identity=row->storage_volume;described->managed_format_proof=row->managed_format_proof;
  return described;
}

std::vector<ContentSourceDescription> ContentSourceRegistry::DescribeSourcesCached(
    uint32_t device_id,std::error_code& error) const {
  error.clear();std::vector<ContentSourceDescription> described;
  std::unique_lock lock(state_->mutex,std::try_to_lock);
  if(!lock.owns_lock()) {
    error=std::make_error_code(std::errc::device_or_resource_busy);return {};
  }
  for(const auto& [id,row]:state_->observed) {
    if(row.source.device_id!=device_id)continue;
    const auto* actual=state_->CachedDescriptionLocked(row.storage_volume);
    if(!actual || actual!=&row)continue;
    ContentSourceDescription value;
    value.source=row.source;value.generation=row.generation;
    value.capacity_bytes=row.capacity_bytes;value.free_bytes=row.free_bytes;
    value.identity=row.storage_volume;value.managed_format_proof=row.managed_format_proof;
    described.push_back(std::move(value));
  }
  return described;
}
bool ContentSourceRegistry::IsDescriptionCurrentCached(
    const ContentSourceDescription& expected) const noexcept try {
  std::unique_lock lock(state_->mutex,std::try_to_lock);
  if(!lock.owns_lock())return false;
  const auto* row=state_->CachedDescriptionLocked(expected.identity);
  return row && row->generation==expected.generation &&
      row->source.id==expected.source.id && row->source.kind==expected.source.kind &&
      row->source.device_id==expected.source.device_id &&
      row->source.device_type==expected.source.device_type &&
      row->source.read_only==expected.source.read_only &&
      row->source.root==expected.source.root &&
      row->source.guest_root==expected.source.guest_root &&
      row->source.guest_volume_root==expected.source.guest_volume_root;
} catch(...) {return false;}

std::shared_ptr<filesystem::HostStorageRoot>
ContentSourceRegistry::CaptureManagedDirectory(
    const std::filesystem::path& directory,bool readonly,bool& matched,
    std::error_code& error,std::shared_ptr<const filesystem::ConsoleStorageVolume>* volume) const {
  return CaptureDirectory(directory,readonly,matched,error,true,volume);
}
std::shared_ptr<filesystem::HostStorageRoot>
ContentSourceRegistry::CaptureSourceDirectory(
    const std::filesystem::path& directory,bool readonly,bool& matched,
    std::error_code& error,std::shared_ptr<const filesystem::ConsoleStorageVolume>* volume) const {
  return CaptureDirectory(directory,readonly,matched,error,false,volume);
}
std::shared_ptr<filesystem::HostStorageRoot>
ContentSourceRegistry::CaptureDirectory(
    const std::filesystem::path& directory, bool readonly, bool& matched,
    std::error_code& error,bool managed_only,
    std::shared_ptr<const filesystem::ConsoleStorageVolume>* volume) const {
  matched = false;
  error.clear();
  if (volume) { volume->reset(); }
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  for (const auto device : DeviceIds()) {
    for (const auto& source : Sources(device)) {
      if(source.selected_media && (!source.available || !source.selected_media->folder()) &&
          source.selected_media->IntersectsDirectory(directory)) {
        matched=true;error=std::make_error_code(std::errc::permission_denied);return {};
      }
      if (!source.available || !source.storage ||
          ((managed_only || !source.source.read_only) && !source.managed_identity)) { continue; }
      auto view = source.storage->CaptureGuestDirectory(directory, readonly, matched, error);
      if (matched || error) {
        if (view) {
          if (source.managed_identity) { error = source.managed_identity->CheckCurrent(); }
          auto identity = error ? nullptr : CaptureStorageVolume(source);
          if (!error && !identity) { error=std::make_error_code(std::errc::no_such_device); }
          if (!error && volume) { *volume=std::move(identity); }
        }
        return error ? nullptr : view;
      }
    }
  }
  return {};
}

std::optional<std::filesystem::path> ContentSourceRegistry::Resolve(
    const std::string& source_id, const std::filesystem::path& relative,
    bool for_write) const {
  const auto state = Source(source_id);
  if (!state || state->source.kind==ContentSourceKind::kSelectedMedia || state->source.kind==ContentSourceKind::kCache ||
      !state->available || (for_write && state->source.read_only) ||
      relative.empty() || relative.is_absolute() || relative.has_root_name()) {
    return std::nullopt;
  }
  for (const auto& part : relative) {
    if (part == "..") {
      return std::nullopt;
    }
  }
  std::error_code ec;
  const auto root = std::filesystem::canonical(state->source.root, ec);
  if (ec) {
    return std::nullopt;
  }
  const auto resolved = ResolveBelow(root, relative);
  if (!resolved || *resolved == root || !state->storage ||
      state->storage->CheckGuestAccess(resolved->lexically_relative(root), for_write)) {
    return std::nullopt;
  }
  return resolved;
}

std::optional<std::filesystem::path> ContentSourceRegistry::Resolve(
    const ContentSourceState& expected, const std::filesystem::path& relative,
    bool for_write) const {
  if (expected.source.kind==ContentSourceKind::kSelectedMedia || expected.source.kind==ContentSourceKind::kCache ||
      !expected.available || !expected.storage || relative.empty() ||
      relative.is_absolute() || relative.has_root_name() ||
      std::any_of(relative.begin(), relative.end(),
          [](const auto& part) { return part == ".."; })) { return std::nullopt; }
  auto current = [&] {
    std::lock_guard lock(state_->mutex);
    const auto cap = state_->capabilities.find(expected.source.id);
    const auto row = state_->observed.find(expected.source.id);
    if (state_->closed.load() || cap == state_->capabilities.end() ||
        row == state_->observed.end()) { return false; }
    const auto& actual = row->second;
    return !cap->second.transitioning && actual.available &&
        cap->second.generation == expected.generation &&
        cap->second.root == expected.storage && !expected.storage->retiring() &&
        cap->second.managed_identity == expected.managed_identity &&
        cap->second.volume == expected.volume &&
        actual.storage == expected.storage && actual.generation == expected.generation &&
        actual.source.device_id == expected.source.device_id &&
        actual.source.kind == expected.source.kind && actual.source.root == expected.source.root &&
        (!for_write || (state_->access == Access::kNormal &&
                        !actual.source.read_only && !expected.storage->read_only() &&
                        (!expected.managed_identity || !expected.managed_identity->retiring()) &&
                        (!expected.volume || expected.volume->profile_writable())));
  };
  if (!current()) { return std::nullopt; }
  // Retained native root validates its real current identity; no path-only
  // reopening or authority upgrade if a device disappears/reappears by ID.
  if (!expected.storage->Observe().available) { return std::nullopt; }
  const auto root = expected.storage->path();
  const auto resolved = ResolveBelow(root, relative);
  if (!resolved || *resolved == root ||
      expected.storage->CheckGuestAccess(resolved->lexically_relative(root), for_write) ||
      !current()) { return std::nullopt; }
  return resolved;
}

}  // namespace xe::kernel::xam
