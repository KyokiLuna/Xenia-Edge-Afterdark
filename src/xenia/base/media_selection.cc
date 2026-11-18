/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/media_selection.h"
#include <algorithm>
#include <limits>
#include <mutex>
#include <set>
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
namespace xe::filesystem {
namespace {
std::error_code Gone() { return std::make_error_code(std::errc::no_such_device); }
bool Relative(std::string_view p) {
  if (p.empty() || p.size()>1024 || p.front()=='/' || p.front()=='\\') return false;
  size_t start=0, depth=0;
  for (size_t i=0;i<=p.size();++i) {
    if (i!=p.size() && p[i]!='/' && p[i]!='\\') {
      if (uint8_t(p[i])<0x20 || std::string_view(":*?\"<>|").find(p[i])!=std::string_view::npos) return false;
      continue;
    }
    auto part=p.substr(start,i-start);
    if (part.empty() || part=="." || part==".." || part.size()>255 || ++depth>32) return false;
    start=i+1;
  }
  return true;
}
std::string Normalize(std::string p) {
  std::replace(p.begin(),p.end(),'/', '\\'); return p;
}
bool Below(const std::filesystem::path& a,const std::filesystem::path& b) {
  auto j=b.begin();for(auto i=a.begin();i!=a.end();++i,++j) if(j==b.end() || *i!=*j)return false;return true;
}
}
struct MediaSelection::State {
  struct Native {
    std::shared_ptr<HostStorageRoot> root;
    std::filesystem::path relative;
    std::shared_ptr<FileHandle> file;
    FileHandleInfo info;
  };
  mutable std::mutex mutex;
  std::atomic<bool> closed{false};
  bool folder=false;
  std::vector<Entry> entries;
  std::vector<std::filesystem::path> selected_paths;
  std::vector<std::shared_ptr<Native>> files;
  std::shared_ptr<HostStorageRoot> folder_root;
  std::shared_ptr<const HostStorageGuestMount> admission;
  std::string source_id;
  std::weak_ptr<const uint8_t> issuer;
  bool bound=false;
  uint64_t generation=0;
  static std::error_code Check(const Native& n) {
    if (!n.root || n.root->retiring()) return Gone();
    auto denied=n.root->CheckGuestAccess(n.relative);if(denied)return denied;
    std::error_code error;
    // Observe the selected name but never adopt a replacement's bytes.
    auto probe=n.root->OpenFile(n.relative,FileAccess::kGenericRead,error);
    if (!probe) return error ? error : Gone();
    const auto named=probe->QueryInfo(error);
    if (!named || !named->regular || named->directory || named->links!=1 ||
        named->size!=n.info.size || named->filesystem_id!=n.info.filesystem_id ||
        named->file_id!=n.info.file_id) return error ? error : Gone();
    const auto retained=n.file->QueryInfo(error);
    return retained && retained->regular && !retained->directory && retained->links==1 &&
        retained->size==n.info.size && retained->filesystem_id==n.info.filesystem_id &&
        retained->file_id==n.info.file_id ? std::error_code{} : (error?error:Gone());
  }
};
MediaSelection::MediaSelection(std::shared_ptr<State> s):state_(std::move(s)){}
MediaSelection::~MediaSelection(){Retire();}
std::shared_ptr<MediaSelection> MediaSelection::CaptureFolder(
    const std::shared_ptr<HostStorageRoot>& root,std::error_code& error) {
  error.clear();if(!root || global_critical_region::is_held_by_current_thread()) {error=std::make_error_code(std::errc::invalid_argument);return {};}
  auto view=root->Subroot({},true,error);if(view)view=view->GuestView(error);
  if(!view || error)return {};
  auto s=std::make_shared<State>();s->folder=true;s->folder_root=std::move(view);
  s->selected_paths.push_back(s->folder_root->path());return std::shared_ptr<MediaSelection>(new MediaSelection(std::move(s)));
}
std::shared_ptr<MediaSelection> MediaSelection::CaptureFiles(
    std::vector<FileInput> input,std::error_code& error) {
  error.clear();if(input.empty() || input.size()>4096 || global_critical_region::is_held_by_current_thread()) {error=std::make_error_code(std::errc::invalid_argument);return {};}
  auto s=std::make_shared<State>();std::set<std::string> paths;size_t names=0;
  for(auto& i:input) {
    const auto native=path_to_utf8(i.relative);
    if(!i.root || !Relative(i.guest_path) || !Relative(native) || i.relative.is_absolute() || (names+=i.guest_path.size())>256*1024) {error=std::make_error_code(std::errc::invalid_argument);return {};}
    auto path=Normalize(std::move(i.guest_path));auto folded=path;
    for(auto& c:folded)if(c>='A' && c<='Z')c=char(c-'A'+'a');
    // Neither a file/ancestor nor case-only duplicate may hide another entry.
    for(const auto& old:paths)if(old==folded || old.starts_with(folded+"\\") || folded.starts_with(old+"\\")) {error=std::make_error_code(std::errc::file_exists);return {};}
    paths.insert(std::move(folded));
    auto root=i.root->Subroot({},true,error);if(!root)return {};
    error=root->CheckGuestAccess(i.relative);if(error)return {};
    auto file=root->OpenFile(i.relative,FileAccess::kGenericRead,error);if(!file)return {};
    const auto info=file->QueryInfo(error);
    if(!info || !info->regular || info->directory || info->links!=1) {if(!error)error=std::make_error_code(std::errc::operation_not_supported);return {};}
    auto n=std::make_shared<State::Native>();n->root=std::move(root);n->relative=std::move(i.relative);n->info=*info;n->file=std::move(file);
    s->entries.push_back({std::move(path),info->size});s->selected_paths.push_back(n->root->path()/n->relative);s->files.push_back(std::move(n));
  }
  auto result=std::shared_ptr<MediaSelection>(new MediaSelection(std::move(s)));
  auto backing=result->GuestBackings(error);
  // Exact file backings only; never admits the broader native parent.
  auto admission=error?nullptr:HostStorageRoot::AdmitGuestMount(std::move(backing),error);
  if(!admission)return {};
  result->state_->admission=std::move(admission);
  return result;
}
bool MediaSelection::folder() const {std::lock_guard l(state_->mutex);return state_->folder;}
std::vector<MediaSelection::Entry> MediaSelection::entries() const {std::lock_guard l(state_->mutex);return state_->entries;}
bool MediaSelection::current_cached() const {return !state_->closed.load(std::memory_order_acquire);}
void MediaSelection::CloseIngress() const noexcept {
  std::lock_guard l(state_->mutex);state_->closed=true;
}
bool MediaSelection::Bind(const std::shared_ptr<const uint8_t>& issuer,
    const std::string& id,uint64_t generation) {
  std::lock_guard l(state_->mutex);
  if(state_->closed || !issuer)return false;
  if(state_->bound)return !state_->issuer.expired() &&
      !state_->issuer.owner_before(issuer) && !issuer.owner_before(state_->issuer) &&
      state_->source_id==id && state_->generation==generation;
  state_->source_id=id;state_->generation=generation;state_->issuer=issuer;
  state_->bound=true;return true;
}
bool MediaSelection::ObservationRefused(const std::error_code& error) noexcept {
  return error==std::errc::operation_would_block ||
      error==std::errc::device_or_resource_busy ||
      error==std::errc::interrupted || error==std::errc::operation_canceled ||
      error==std::errc::timed_out;
}
void MediaSelection::Retire() const noexcept {
  std::vector<std::shared_ptr<State::Native>> files;std::shared_ptr<HostStorageRoot> folder;
  std::shared_ptr<const HostStorageGuestMount> admission;
  {std::lock_guard l(state_->mutex);state_->closed=true;files.swap(state_->files);folder=std::move(state_->folder_root);admission=std::move(state_->admission);}
  if(folder)folder->Retire();
}
HostStorageSnapshot MediaSelection::Observe() const {
  if(!current_cached()){HostStorageSnapshot out;out.error=Gone();return out;}
  auto out=ProbeRetained();
  // Refused observing IO is not proof that the retained native object vanished.
  // Return the real refusal; never turn cached availability into fresh success.
  if(!out.available && !ObservationRefused(out.error))Retire();
  return out;
}
HostStorageSnapshot MediaSelection::ProbeRetained() const {
  HostStorageSnapshot out;out.read_only=true;
  if(global_critical_region::is_held_by_current_thread()){out.error=std::make_error_code(std::errc::operation_would_block);return out;}
  std::vector<std::shared_ptr<State::Native>> files;std::shared_ptr<HostStorageRoot> folder;
  {std::lock_guard l(state_->mutex);files=state_->files;folder=state_->folder_root;}
  if(folder){out=folder->Observe();out.read_only=true;out.available_bytes=out.usable_bytes=0;return out;}
  if(files.empty()){out.error=Gone();return out;}
  for(const auto& n:files){out.error=State::Check(*n);if(out.error)return out;}
  // A file allowlist is a logical readonly volume, not physical free space.
  out.available=true;return out;
}
FileIoResult MediaSelection::Read(size_t index,uint64_t offset,std::span<uint8_t> bytes,const std::atomic<bool>* canceled) const {
  if(canceled && canceled->load())return {std::make_error_code(std::errc::operation_canceled),0};
  if(global_critical_region::is_held_by_current_thread())return {std::make_error_code(std::errc::operation_would_block),0};
  std::shared_ptr<State::Native> native;
  {std::lock_guard l(state_->mutex);if(state_->closed || index>=state_->files.size())return {Gone(),0};native=state_->files[index];}
  const auto error=State::Check(*native);if(error)return {error,0};
  if(offset>SIZE_MAX || bytes.size()>SIZE_MAX-size_t(offset))return {std::make_error_code(std::errc::value_too_large),0};
  if(canceled && canceled->load())return {std::make_error_code(std::errc::operation_canceled),0};
  return native->file->ReadResult(size_t(offset),bytes);
}
std::shared_ptr<HostStorageRoot> MediaSelection::CaptureFolderRoot(std::error_code& error) const {
  std::lock_guard l(state_->mutex);error.clear();if(state_->closed || !state_->folder_root){error=Gone();return {};}return state_->folder_root;
}
std::optional<uint64_t> MediaSelection::QueryAllocatedBytes(size_t index,std::error_code& error) const {
  error.clear();std::shared_ptr<State::Native> native;
  if(global_critical_region::is_held_by_current_thread()){error=std::make_error_code(std::errc::operation_would_block);return {};}
  {std::lock_guard l(state_->mutex);if(state_->closed || index>=state_->files.size()){error=Gone();return {};}native=state_->files[index];}
  error=State::Check(*native);if(error)return {};
  return native->file->QueryAllocatedBytes(error);
}
std::vector<GuestHostBacking> MediaSelection::GuestBackings(std::error_code& error) const {
  std::vector<std::shared_ptr<State::Native>> files;std::shared_ptr<HostStorageRoot> folder;
  {std::lock_guard l(state_->mutex);error.clear();if(state_->closed){error=Gone();return {};}files=state_->files;folder=state_->folder_root;}
  if(folder)return {folder->GuestBacking()};std::vector<GuestHostBacking> out;
  for(const auto& n:files){auto b=n->root->GuestBacking();b.path=n->root->path()/n->relative;b.subtree=false;b.opened_file=n->info;out.push_back(std::move(b));}return out;
}
bool MediaSelection::IntersectsDirectory(const std::filesystem::path& path) const {
  const auto normalized=path.lexically_normal();
  std::vector<std::shared_ptr<State::Native>> files;
  std::shared_ptr<HostStorageRoot> folder;
  {
    std::lock_guard l(state_->mutex);
    for(const auto& selected:state_->selected_paths)
      if(Below(normalized,selected) || (state_->folder && Below(selected,normalized)))return true;
    files=state_->files;folder=state_->folder_root;
  }
  // Bind aliases are denial too. A spelling comparison cannot grant a raw
  // directory capability around an active exact-file selection.
  std::error_code error;
  const auto candidate=HostStorageDirectoryIdentity::Capture(path,error);
  if(!candidate)return false;
  for(const auto& n:files) {
    const auto& identity=n->root->directory_identity();
    if(identity && candidate->RelativeEntry(*identity,n->relative))return true;
  }
  if(folder && folder->directory_identity()) {
    if(candidate->RelativeEntry(*folder->directory_identity(),{}) ||
        folder->directory_identity()->RelativeEntry(*candidate,{}))return true;
  }
  return false;
}
std::vector<MediaSelection::ReferenceInput> MediaSelection::ReferenceInputs(std::error_code& error) const {
  error.clear();std::vector<ReferenceInput> out;
  std::lock_guard l(state_->mutex);
  if(state_->closed){error=Gone();return {};}
  if(state_->folder_root){out.push_back({{{},state_->folder_root,{}},{}});return out;}
  for(size_t n=0;n<state_->files.size();++n)
    out.push_back({{state_->entries[n].path,state_->files[n]->root,state_->files[n]->relative},state_->files[n]->info});
  return out;
}
}  // namespace xe::filesystem
