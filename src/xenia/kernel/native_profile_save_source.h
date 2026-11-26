/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "xenia/kernel/xam/profile_gpd_model.h"
#include <string>
#include <system_error>
#include <utility>
#include "xenia/base/mutex.h"
#include "xenia/kernel/xam/profile_gpd_publication.h"
namespace xe::kernel {
class NativeProfileSaveOwner;
class NativeProfileSaveManagedGpd;
class NativeProfileSaveManagedPicture;
namespace xam::test {class NativeProfileSavePublicationTestAccess;}
// Exact physical source/profile binding for the existing per-leaf managed
// policy. This is NOT a selected model, writer, profile mount or sign-in lease.
// A later real model may use this same physical leaf only through its own
// independently current committed filesystem tuple or scoped native request.
// Ordinary immutable reads grant no selected-user/native-object/write authority. Old File
// owners keep their actual old model/storage tuple; the policy never retargets
// one, pins a roster, or converts a read into permission to write.
class NativeProfileSaveSource final {
 private:
  friend class NativeProfileSaveOwner;
  friend class NativeProfileSaveManagedGpd;
  friend class NativeProfileSaveManagedPicture;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  static std::shared_ptr<NativeProfileSaveSource> Capture(
      const std::shared_ptr<xam::ProfileStorageLease>& storage,std::error_code& error,
      std::shared_ptr<const std::atomic<bool>> family_closed={}) {
    error.clear();
    if(global_critical_region::is_held_by_current_thread() || (family_closed && family_closed->load(std::memory_order_acquire)) ||
        !storage || storage->retiring() || !storage->source() ||
        !storage->managed_identity() || !storage->view()->directory_identity()) {
      error=std::make_error_code(std::errc::invalid_argument);return {};
    }
    if((error=storage->CheckAvailable())) {return {};}
    return std::shared_ptr<NativeProfileSaveSource>(new NativeProfileSaveSource(*storage,std::move(family_closed)));
  }
  // Preparatory request/capability comparison only: no native stat, path,
  // directory query or physical-identity proof. The real managed-open worker
  // MUST also run Matches + exact requested leaf/token admission before any
  // disposition/File effect. This permits original-caller capture before the
  // existing owned IO offload without doing IO under a native DB lock.
  bool MatchesMetadata(const xam::ProfileStorageLease& storage) const {
    return !retiring() && !storage.retiring() && storage.xuid()==xuid_ &&
        storage.source_id()==source_id_ && storage.device_id()==device_id_ &&
        storage.source_generation()==source_generation_ &&
        SameOwner(storage.source(),source_) && SameOwner(storage.managed_identity(),managed_) &&
        SameOwner(storage.guest_generation(),guest_generation_);
  }
  // Native physical identity observation is permitted only outside namespace,
  // model and IO publication locks. The family supplies the actual independently
  // selected model/lease; matching a path or XUID alone supplies no authority.
  bool Matches(const xam::ProfileStorageLease& storage) const {
    const auto& parent=storage.view()->directory_identity();
    return !global_critical_region::is_held_by_current_thread() && MatchesMetadata(storage) && parent &&
        parent_->SameEntry({},*parent,{});
  }
  // Metadata-only, including under the real registry/GCRC guard. Holding this
  // source does not retain a retired model, profile File, mount or read ticket.
  bool retiring() const {
    return retired_.load(std::memory_order_acquire) ||
        (family_closed_ && family_closed_->load(std::memory_order_acquire)) || source_->retiring() || managed_->retiring() || guest_generation_->retiring();
  }
  // Only successful HOST model load/preparation joins a candidate. Weak
  // references cannot pin old rosters/models; there is no slot/Task/default
  // lookup and no "latest" replacement by path. Matching metadata is followed
  // by physical source/token proof at every real owned-IO open.
  std::error_code JoinCommittedReadModel(const std::shared_ptr<xam::ProfileGpdModelAuthority>& model) {
    if(global_critical_region::is_held_by_current_thread() || !model ||
        !MatchesMetadata(*model->storage()))return std::make_error_code(std::errc::operation_canceled);
    std::error_code error;
    const auto ready=model->CaptureRead(error);if(!ready)return error;
    std::unique_lock lock(read_models_mutex_,std::try_to_lock);
    if(!lock.owns_lock())return std::make_error_code(std::errc::device_or_resource_busy);
    std::erase_if(read_models_,[](const auto& weak){return weak.expired();});
    for(const auto& weak:read_models_)if(weak.lock()==model)return {};
    if(read_models_.size()>=4096)return std::make_error_code(std::errc::no_buffer_space);
    if(read_models_generation_==UINT64_MAX)return std::make_error_code(std::errc::value_too_large);
    read_models_.push_back(model);++read_models_generation_;return {};
  }
  std::optional<xam::ProfileGpdReadCapture> CaptureCommittedFileRead(std::error_code& error) const {
    error.clear();
    if(global_critical_region::is_held_by_current_thread() || retiring()) {
      error=std::make_error_code(std::errc::operation_canceled);return {};
    }
    std::vector<std::weak_ptr<xam::ProfileGpdModelAuthority>> candidates;uint64_t generation=0;
    {std::unique_lock lock(read_models_mutex_,std::try_to_lock);
      if(!lock.owns_lock()){error=std::make_error_code(std::errc::device_or_resource_busy);return {};}
      candidates=read_models_;generation=read_models_generation_;
    }
    // First census ALL matching live model owners. A pending owner anywhere
    // in the set refuses this open, even if an earlier model still matches.
    // Capture order never elects a selected/latest model or grants permission.
    struct Candidate {std::shared_ptr<xam::ProfileGpdModelAuthority> model;xam::ProfileGpdReadCapture read;};
    std::vector<Candidate> ready;ready.reserve(candidates.size());
    for(const auto& weak:candidates) {
      const auto model=weak.lock();if(!model || !MatchesMetadata(*model->storage()))continue;
      auto read=model->CaptureCommittedFileRead(error);
      if(!read) {
        if(error==std::errc::operation_canceled)continue; // Factual retired model only.
        return {};
      }
      ready.push_back({model,std::move(*read)});
    }
    std::error_code mismatch;
    for(auto& candidate:ready) {
      if(!Matches(*candidate.model->storage())){error=std::make_error_code(std::errc::operation_canceled);return {};}
      auto checked=candidate.model->storage()->view()->ValidateSnapshotBatch(candidate.read.tokens,error);
      if(!checked) {
        // A still-retained old model can legitimately describe the previous
        // committed physical generation. Only exact snapshot mismatch/missing
        // may try another independently HOST-admitted tuple. Busy or unknown
        // IO/admission failures never fall through to a weaker candidate.
        if(error==std::errc::state_not_recoverable || error==std::errc::no_such_file_or_directory) {
          mismatch=error;continue;
        }
        return {};
      }
      // While the selected physical tuple's read fence is still held, require
      // every census participant to remain current and nonpending. A later
      // owner cannot race the first-success shortcut or silently change epoch.
      for(const auto& other:ready) {
        if(!MatchesMetadata(*other.model->storage()) || !other.model->IsCommittedFileReadCurrent(other.read)) {
          error=std::make_error_code(std::errc::device_or_resource_busy);return {};
        }
      }
      {std::unique_lock membership(read_models_mutex_,std::try_to_lock);
        if(!membership.owns_lock() || generation!=read_models_generation_) {
          error=std::make_error_code(std::errc::device_or_resource_busy);return {};
        }
      }
      error.clear();return std::move(candidate.read);
    }
    if(mismatch){error=mismatch;return {};}
    error=std::make_error_code(std::errc::operation_canceled);return {};
  }
  // Real family/source rundown withdraws only new capability/open admission.
  // It does not destroy or falsely complete independently owned old Files.
  void RetireAdmission() {retired_.store(true,std::memory_order_release);}
  template<class T> static bool SameOwner(const std::shared_ptr<T>& a,const std::shared_ptr<T>& b) {
    return a && b && a.get()==b.get() && !a.owner_before(b) && !b.owner_before(a);
  }
  explicit NativeProfileSaveSource(const xam::ProfileStorageLease& storage,
      std::shared_ptr<const std::atomic<bool>> family_closed)
      :xuid_(storage.xuid()),source_generation_(storage.source_generation()),device_id_(storage.device_id()),
       source_id_(storage.source_id()),source_(storage.source()),managed_(storage.managed_identity()),
       parent_(storage.view()->directory_identity()),guest_generation_(storage.guest_generation()),family_closed_(std::move(family_closed)) {}
  const uint64_t xuid_,source_generation_;
  const uint32_t device_id_;
  const std::string source_id_;
  const std::shared_ptr<const filesystem::HostStorageRoot> source_;
  const std::shared_ptr<const filesystem::ManagedDeviceIdentity> managed_;
  const std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent_;
  const std::shared_ptr<const filesystem::HostStorageRoot> guest_generation_;
  const std::shared_ptr<const std::atomic<bool>> family_closed_;
  mutable std::mutex read_models_mutex_;
  std::vector<std::weak_ptr<xam::ProfileGpdModelAuthority>> read_models_;
  uint64_t read_models_generation_=1;
  std::atomic<bool> retired_{false};
};
}  // namespace xe::kernel
