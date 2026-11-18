/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/host_metadata_writer.h"
#include <algorithm>
#include "xenia/base/mutex.h"
namespace xe::filesystem {
namespace {
std::error_code Error(std::errc e) { return std::make_error_code(e); }
bool Canceled(const std::atomic<bool>* stop) {
  return stop && stop->load(std::memory_order_acquire);
}
}
HostMetadataWriter::~HostMetadataWriter() = default;
bool HostMetadataWriter::Relative(const std::filesystem::path& relative) {
  if(relative.empty() || relative.is_absolute() || relative.has_root_path())return false;
  size_t count=0;
  for(const auto& part:relative){
    if(part.empty() || part=="." || part==".." || ++count>16 || part.native().size()>255 ||
       std::find(part.native().begin(),part.native().end(),std::filesystem::path::value_type{})!=part.native().end() ||
       part==SnapshotRecoveryOwner::kDirectory)return false;
  }
  return true;
}
std::shared_ptr<HostMetadataWriter> HostMetadataWriter::Open(
    const std::filesystem::path& requested,std::error_code& error) {
  auto owner=std::shared_ptr<HostMetadataWriter>(new HostMetadataWriter);
  auto& failure=owner->error_;
  if(requested.empty() ||
     std::find(requested.native().begin(),requested.native().end(),std::filesystem::path::value_type{})!=requested.native().end() ||
     global_critical_region::is_held_by_current_thread()){
    failure=Error(std::errc::invalid_argument);error=failure;return owner;
  }
  owner->path_=std::filesystem::absolute(requested,failure).lexically_normal();
  if(failure){error=failure;return owner;}
  auto ancestor=owner->path_;
  std::vector<std::filesystem::path> missing;
  std::shared_ptr<HostStorageRoot> current;
  for(unsigned depth=0;depth<64;++depth){
    current=HostStorageRoot::Open(ancestor,false,failure,kHostStorageReserve);
    if(current)break;
    if(failure!=std::errc::no_such_file_or_directory || ancestor==ancestor.root_path())break;
    missing.push_back(ancestor.filename());ancestor=ancestor.parent_path();
  }
  if(!current){if(!failure)failure=Error(std::errc::filename_too_long);error=failure;return owner;}
  owner->uncertain_ancestor_=current;
  for(auto it=missing.rbegin();it!=missing.rend();++it){
    auto child=current->Subroot(*it,false,failure);
    if(!child && failure==std::errc::no_such_file_or_directory){
      owner->last_.attempted_directories.push_back(current->path()/ *it);
      failure=current->Create(*it,true);
      if(!failure)owner->last_.created_directories.push_back(current->path()/ *it);
      if(!failure || failure==std::errc::file_exists)child=current->Subroot(*it,false,failure);
    }
    if(child && !failure) failure=current->FlushDirectory();
    if(child && !failure) failure=child->FlushDirectory();
    if(!child || failure){if(!failure)failure=Error(std::errc::io_error);break;}
    current=std::move(child);owner->uncertain_ancestor_=current;
  }
  if(failure){
    owner->last_.error=failure;
    owner->Remember(owner->last_); // Immutable retained attempt survives later Write(error_).
    error=failure;return owner;
  }
  owner->root_=std::move(current);
  owner->recovery_=SnapshotRecoveryOwner::Open(owner->root_);
  failure=owner->recovery_->error();owner->last_.error=failure;
  if(failure)owner->Remember(owner->last_);
  if(!failure)owner->uncertain_ancestor_.reset();
  error=failure;return owner;
}
std::shared_ptr<HostStorageRoot> HostMetadataWriter::Directory(
    const std::filesystem::path& relative,bool create,
    HostMetadataWriteResult* evidence,std::error_code& error) const {
  error.clear();if(!root_){error=error_?error_:Error(std::errc::no_such_device);return {};}
  auto current=root_;
  for(const auto& part:relative){
    auto next=current->Subroot(part,false,error);
    if(!next && create && error==std::errc::no_such_file_or_directory){
      if(evidence)evidence->attempted_directories.push_back(current->path()/part);
      error=current->Create(part,true);
      if(!error && evidence)evidence->created_directories.push_back(current->path()/part);
      if(!error || error==std::errc::file_exists)next=current->Subroot(part,false,error);
    }
    // This also covers an externally created directory winning the mkdir
    // race. Its entry is not assumed durable merely because Subroot opened it.
    if(next && !error && create)error=current->FlushDirectory();
    if(next && !error && create)error=next->FlushDirectory();
    if(!next || error)return {};
    current=std::move(next);
  }
  return current;
}
std::shared_ptr<const HostStorageSnapshotToken> HostMetadataWriter::Capture(
    const std::filesystem::path& relative,uint64_t maximum,std::error_code& error) const {
  error.clear();
  if(!Relative(relative) || maximum>kMaximumBytes || global_critical_region::is_held_by_current_thread()){
    error=Error(std::errc::invalid_argument);return {};
  }
  std::lock_guard lock(mutex_);
  auto parent=Directory(relative.parent_path(),false,nullptr,error);
  if(!parent){if(!error)error=Error(std::errc::io_error);return {};}
  auto token=parent->CaptureSnapshot(relative.filename(),maximum,error);
  if(!token && !error)error=Error(std::errc::io_error);
  return token;
}
bool HostMetadataWriter::Owns(const HostStorageSnapshotToken& token) const {
  if(!root_ || !token.root() || !token.root()->directory_identity() || !root_->directory_identity())return false;
  const auto at=root_->directory_identity()->RelativeEntry(
      *token.root()->directory_identity(),token.relative());
  return at && Relative(*at) && !token.relative().has_parent_path();
}
HostMetadataWriteResult HostMetadataWriter::Remember(HostMetadataWriteResult result) {
  last_=result;
  if(result.error && (result.publication.stage_created || result.publication.recovery_created ||
      !result.attempted_directories.empty()) && failures_.size()<kMaximumFailures)failures_.push_back(result);
  return result;
}
HostMetadataWriteResult HostMetadataWriter::PublishLocked(
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> intended,const std::atomic<bool>* canceled,bool erase) {
  HostMetadataWriteResult result;
  if(intended.size()>kMaximumBytes || (expected && expected->bytes().size()>kMaximumBytes)){
    result.error=Error(std::errc::file_too_large);return result;
  }
  if(error_ || !recovery_ || !expected || !Owns(*expected)){
    result.error=error_?error_:Error(std::errc::invalid_argument);return result;
  }
  result.publication.expected=expected;
  if(Canceled(canceled)){result.error=Error(std::errc::operation_canceled);return result;}
  if(!erase && expected->exists() && std::equal(expected->bytes().begin(),expected->bytes().end(),
                                               intended.begin(),intended.end())){
    // Same bytes still require exact current identity and actual file+parent
    // barriers. A load token is never mislabeled as a published CAS receipt.
    const auto observed=expected->root()->ObserveSnapshotDurability(*expected);
    result.error=observed.error;result.durable=observed.durable;
    if(!result.error && !result.durable)result.error=Error(std::errc::io_error);
    result.unchanged=!result.error && result.durable;
    result.publication.expected=expected;
    return result;
  }
  if(failures_.size()>=kMaximumFailures){result.error=Error(std::errc::no_buffer_space);return result;}
  result.publication=erase?SnapshotPublisher::Delete(*recovery_,expected,canceled):
      SnapshotPublisher::Publish(*recovery_,expected,intended,canceled);
  result.error=result.publication.error;
  result.durable=!result.error && result.publication.durable;
  if(!result.error && !result.durable)result.error=Error(std::errc::io_error);
  return result;
}
HostMetadataWriteResult HostMetadataWriter::Write(const std::filesystem::path& relative,
    std::span<const uint8_t> intended,const std::atomic<bool>* canceled,bool require_absent) {
  HostMetadataWriteResult result;
  if(global_critical_region::is_held_by_current_thread() || !Relative(relative) || intended.size()>kMaximumBytes){
    result.error=Error(std::errc::invalid_argument);return result;
  }
  std::lock_guard lock(mutex_);
  if(error_ || Canceled(canceled) || failures_.size()>=kMaximumFailures){
    result.error=error_?error_:Canceled(canceled)?Error(std::errc::operation_canceled):Error(std::errc::no_buffer_space);
    return Remember(std::move(result));
  }
  auto parent=Directory(relative.parent_path(),true,&result,result.error);
  if(!parent){if(!result.error)result.error=Error(std::errc::io_error);return Remember(std::move(result));}
  auto expected=parent->CaptureSnapshot(relative.filename(),kMaximumBytes,result.error);
  if(!expected){if(!result.error)result.error=Error(std::errc::io_error);return Remember(std::move(result));}
  if(require_absent && expected->exists()){
    result.error=Error(std::errc::file_exists);return Remember(std::move(result));
  }
  auto published=PublishLocked(std::move(expected),intended,canceled);
  published.created_directories=std::move(result.created_directories);
  published.attempted_directories=std::move(result.attempted_directories);
  return Remember(std::move(published));
}
HostMetadataWriteResult HostMetadataWriter::Publish(
    std::shared_ptr<const HostStorageSnapshotToken> expected,std::span<const uint8_t> bytes,
    const std::atomic<bool>* canceled) {
  if(global_critical_region::is_held_by_current_thread()){
    HostMetadataWriteResult r;r.error=Error(std::errc::operation_not_permitted);return r;
  }
  std::lock_guard lock(mutex_);return Remember(PublishLocked(std::move(expected),bytes,canceled));
}
HostMetadataWriteResult HostMetadataWriter::PublishAt(
    const std::filesystem::path& relative,
    std::shared_ptr<const HostStorageSnapshotToken> expected,std::span<const uint8_t> bytes,
    const std::atomic<bool>* canceled) {
  HostMetadataWriteResult result;
  if(global_critical_region::is_held_by_current_thread() || !Relative(relative)){
    result.error=Error(std::errc::invalid_argument);return result;
  }
  std::lock_guard lock(mutex_);
  auto parent=Directory(relative.parent_path(),false,nullptr,result.error);
  if(!parent || !expected || !expected->root() || !expected->root()->directory_identity() ||
      !parent->directory_identity() || expected->relative()!=relative.filename() ||
      !parent->directory_identity()->SameEntry({},*expected->root()->directory_identity(),{})){
    if(!result.error)result.error=Error(std::errc::state_not_recoverable);
    return Remember(std::move(result));
  }
  return Remember(PublishLocked(std::move(expected),bytes,canceled));
}
HostMetadataWriteResult HostMetadataWriter::Erase(
    std::shared_ptr<const HostStorageSnapshotToken> expected,const std::atomic<bool>* canceled) {
  if(global_critical_region::is_held_by_current_thread()){
    HostMetadataWriteResult r;r.error=Error(std::errc::operation_not_permitted);return r;
  }
  std::lock_guard lock(mutex_);return Remember(PublishLocked(std::move(expected),{},canceled,true));
}
HostMetadataWriteResult HostMetadataWriter::last_result() const {
  std::lock_guard lock(mutex_);return last_;
}
std::vector<HostMetadataWriteResult> HostMetadataWriter::retained_failures() const {
  std::lock_guard lock(mutex_);return failures_;
}
}  // namespace xe::filesystem
