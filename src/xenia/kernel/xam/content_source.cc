/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include "xenia/kernel/xam/content_source.h"
#include "xenia/base/managed_volume_authority.h"
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
    std::shared_ptr<const ManagedFormatSourceProof> managed_format_proof;
  };
  State(Access access_value, SpaceQuery value)
      : access(access_value), query(std::move(value)) {
    snapshot.revision = 1;
  }
  using QueryValues =
      std::map<std::string, std::optional<std::filesystem::space_info>>;
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
  QueryValues SampleQueries(const std::vector<ContentSource>& selected) const {
    QueryValues values;
    if (query) {
      for (const auto& source : selected) {
        values.emplace(source.id, query(source.root));
      }
    }
    return values;
  }
  ContentSourceState ObserveLocked(const ContentSource& source,
                                   const QueryValues& queries) {
    ContentSourceState result{source};
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
  void ProjectLocked() {
    // Only a managed volume is a physical device. Installed content is an
    // overlay of that device, and host-media references are not native volumes.
    std::map<uint32_t, const ContentSource*> volumes;
    for (const auto& source : sources) {
      if (source.kind != ContentSourceKind::kManaged ||
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
      devices.push_back({source->id, value ? value->generation : 0,
                         source->device_type, source->guest_volume_root,
                         value && value->available,
                         value ? value->managed_format_proof : nullptr});
    }
    if (devices != snapshot.devices) {
      if (snapshot.revision == UINT64_MAX) {
        closed.store(true);
        return;
      }
      snapshot.devices = std::move(devices);
      ++snapshot.revision;
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
        if (source.kind == ContentSourceKind::kManaged) {
          selected.push_back(source);
        }
      }
    }
    const auto queries = SampleQueries(selected);
    std::lock_guard lock(mutex);
    if (!closed.load()) {
      for (const auto& source : selected) {
        ObserveLocked(source, queries);
      }
      ProjectLocked();
    }
    return snapshot;
  }
  const Access access;
  SpaceQuery query;
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
  state_->closed.store(true);
  notifications_->Shutdown();
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
  if (source.id.empty() || !source.device_id || source.root.empty() ||
      source.device_type == DeviceType::Invalid ||
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
        retired = std::move(capability.root);
        capability = {std::move(view), generation, true, {}, std::move(authority)};
        existing_id = true;
        break;
      }
      if (existing.device_id == source.device_id &&
          (existing.device_type != source.device_type ||
           (!existing.read_only && !source.read_only))) {
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
      source.device_type == DeviceType::Invalid || source.root != identity->source()->path()) {
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
          return held.id == source.id || (held.device_id == source.device_id &&
              (held.device_type != source.device_type || !held.read_only));
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
    result.push_back(source.device_id);
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
      if (source.device_id == device_id) {
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
      result.push_back(state->ObserveLocked(source, queries));
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
    if (!selected.empty()) {
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

std::shared_ptr<filesystem::HostStorageRoot>
ContentSourceRegistry::CaptureManagedDirectory(
    const std::filesystem::path& directory, bool readonly, bool& matched,
    std::error_code& error) const {
  matched = false;
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  for (const auto device : DeviceIds()) {
    for (const auto& source : Sources(device)) {
      if (!source.available || !source.storage || !source.managed_identity) { continue; }
      auto view = source.storage->CaptureGuestDirectory(directory, readonly, matched, error);
      if (matched || error) {
        if (view) { error = source.managed_identity->CheckCurrent(); }
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
  if (!state || !state->available || (for_write && state->source.read_only) ||
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
  if (!expected.available || !expected.storage || relative.empty() ||
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
