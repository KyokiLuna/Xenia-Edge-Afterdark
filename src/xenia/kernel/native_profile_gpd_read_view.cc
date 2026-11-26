/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_gpd_read_view.h"
#include <algorithm>
#include <cstdio>
#include "xenia/base/mutex.h"
#include "xenia/base/retained_file_metadata.h"
#include "xenia/base/snapshot_publication_set.h"
namespace xe::kernel {
namespace {
std::error_code Error(std::errc value) {return std::make_error_code(value);}
bool Leaf(std::string_view actual,uint32_t title) {
  char expected[13];std::snprintf(expected,sizeof(expected),"%08X.gpd",title);
  if(actual.size()!=12) {return false;}
  auto fold=[](char c){return c>='A'&&c<='Z' ? char(c+('a'-'A')) : c;};
  return std::equal(actual.begin(),actual.end(),expected,[&](char a,char b){return fold(a)==fold(b);});
}
}
std::shared_ptr<NativeProfileGpdReadView> NativeProfileGpdReadView::Capture(
    std::shared_ptr<const xam::ProfileGpdModelAuthority> model,xam::ProfileGpdReadCapture read,
    uint32_t title,std::error_code& error,const filesystem::SnapshotPublicationSet* members) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread() || !model || !title ||
      read.origin.owner.lock()!=model || read.origin.storage!=model->storage() ||
      (members && members->base()!=read.origin.storage->view()) ||
      !model->IsCurrent(read.origin)) {error=Error(std::errc::operation_canceled);return {};}
  auto result=std::shared_ptr<NativeProfileGpdReadView>(new NativeProfileGpdReadView);
  for(const auto& token:read.tokens) {
    if(token && token->root()==read.origin.storage->view() && !token->relative().has_parent_path() &&
        token->ascii_folded_leaf() && Leaf(token->relative().generic_string(),title)) {
      if(result->token_) {error=Error(std::errc::bad_message);return {};}
      result->token_=token;
    }
  }
  if(!result->token_ || !result->token_->exists()) {error=Error(std::errc::no_such_file_or_directory);return {};}
  error=xam::ValidateGpdSnapshot(result->token_->bytes());if(error) {return {};}
  const auto& storage=read.origin.storage;
  error=storage->CheckAvailable();if(error) {return {};}
  if (members) {
    const auto actual=members->ObserveRetainedMetadata(result->token_,error);
    if (!actual) return {};
    result->information_.end_of_file=actual->size;
    result->information_.access_timestamp=actual->access_timestamp;
    result->information_.write_timestamp=actual->write_timestamp;
    result->information_.change_timestamp=actual->change_timestamp;
    result->native_allocated_bytes_=actual->allocated_bytes;
  } else {
    // Exact retained source capability, not a VFS/raw-path fallback. This
    // temporary metadata descriptor never escapes or becomes native view IO.
    auto file=storage->view()->OpenFile(result->token_->relative(),filesystem::FileAccess::kGenericRead,error);
    if(!file) {return {};}
    const auto info=file->QueryInfo(error);if(!info) {return {};}
    const auto actual=filesystem::QueryRetainedFileMetadata(*file,error);if(!actual) {return {};}
    const auto& expected=result->token_->info();
    if(!info->regular || info->links!=1 || info->filesystem_id!=expected.filesystem_id ||
        info->file_id!=expected.file_id || info->size!=result->token_->bytes().size() ||
        actual->size!=info->size) {error=Error(std::errc::operation_canceled);return {};}
    result->information_.end_of_file=actual->size;
    result->information_.access_timestamp=actual->access_timestamp;
    result->information_.write_timestamp=actual->write_timestamp;
    result->information_.change_timestamp=actual->change_timestamp;
    result->native_allocated_bytes_=actual->allocated_bytes;
  } // genuine metadata descriptor and its normal namespace lease close HERE
  {
    auto checked=members ? members->Validate(read.tokens,error)
                         : storage->view()->ValidateSnapshotBatch(read.tokens,error);
    if(!checked) {return {};}
    if(!model->IsCurrent(read.origin) || storage->retiring()) {error=Error(std::errc::operation_canceled);return {};}
    // Snapshot metadata verifies the real same-guard physical tuple. Generic
    // POSIX ctime is not native creation time; birth remains unknown here.
    const auto found=std::find(read.tokens.begin(),read.tokens.end(),result->token_);
    const auto index=size_t(found-read.tokens.begin());
    if(found==read.tokens.end() || checked->metadata().size()!=read.tokens.size() || !checked->metadata()[index]) {
      error=Error(std::errc::bad_message);return {};
    }
    result->information_.create_timestamp=0;
    result->information_.attributes=0x80; // genuine regular GPD snapshot view
    result->read_=std::move(read);
  }
  return result;
}
std::error_code NativeProfileGpdReadView::Available() const {
  if(global_critical_region::is_held_by_current_thread()) {return Error(std::errc::operation_would_block);}
  if(retired_.load(std::memory_order_acquire)) {return Error(std::errc::operation_canceled);}
  return read_.origin.storage->CheckAvailable();
}
void NativeProfileGpdReadView::RetireAtActualQuiescence() {
  retired_.store(true,std::memory_order_release);
}
}  // namespace xe::kernel
