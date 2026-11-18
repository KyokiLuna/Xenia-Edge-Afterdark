/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/media_reference_store.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
namespace xe::filesystem {
namespace {
constexpr std::array<uint8_t,8> kMagic{'A','D','M','R','E','F',1,0};
std::error_code Invalid(){return std::make_error_code(std::errc::invalid_argument);}
std::array<uint8_t,32> Hash(std::span<const uint8_t> bytes) {
  std::array<uint8_t,32> out{};mbedtls_sha256(bytes.data(),bytes.size(),out.data(),0);return out;
}
void U32(std::vector<uint8_t>& out,uint32_t v){for(int n=3;n>=0;--n)out.push_back(uint8_t(v>>(8*n)));}
void U64(std::vector<uint8_t>& out,uint64_t v){U32(out,uint32_t(v>>32));U32(out,uint32_t(v));}
void Text(std::vector<uint8_t>& out,std::string_view value){U32(out,uint32_t(value.size()));out.insert(out.end(),value.begin(),value.end());}
struct Reader {
  std::span<const uint8_t> bytes;
  size_t at=0;
  bool Take(size_t n,std::span<const uint8_t>& out){if(n>bytes.size()-at)return false;out=bytes.subspan(at,n);at+=n;return true;}
  bool Word(uint32_t& v){std::span<const uint8_t> b;if(!Take(4,b))return false;v=0;for(auto c:b)v=(v<<8)|c;return true;}
  bool Wide(uint64_t& v){uint32_t a,b;if(!Word(a)||!Word(b))return false;v=(uint64_t(a)<<32)|b;return true;}
  bool String(std::string& v,size_t max){uint32_t n;std::span<const uint8_t> b;if(!Word(n)||n>max||!Take(n,b)||std::find(b.begin(),b.end(),0)!=b.end())return false;v.assign(b.begin(),b.end());return true;}
};
bool Id(std::string_view id){return !id.empty() && id.size()<=128 && std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';});}
bool Volume(std::string_view p){return p.size()==13 && p.starts_with(R"(\Device\Mass)") && p.back()>='0' && p.back()<='9';}
bool Unknown(std::span<const uint8_t> bytes){Reader r{bytes};size_t count=0;while(r.at!=bytes.size()){uint32_t tag,n;std::span<const uint8_t> data;if(++count>64||!r.Word(tag)||!r.Word(n)||!tag||n>(1<<20)||!r.Take(n,data))return false;}return true;}
std::optional<MediaReference> Parse(std::span<const uint8_t> bytes) {
  if(bytes.size()<kMagic.size()+32 || bytes.size()>MediaReferenceStore::kMaximumWireBytes || !std::equal(kMagic.begin(),kMagic.end(),bytes.begin()))return {};
  auto payload=bytes.first(bytes.size()-32);const auto hash=Hash(payload);
  if(!std::equal(hash.begin(),hash.end(),bytes.end()-32))return {};
  Reader r{payload,kMagic.size()};MediaReference result;uint32_t folder,count;
  if(!r.String(result.id,128)||!Id(result.id)||!r.Word(result.device_id)||!result.device_id||
      !r.String(result.name,256)||!r.String(result.guest_volume_root,64)||!Volume(result.guest_volume_root)||!r.Word(folder)||folder>1||!r.Word(count)||!count||count>4096 || (folder && count!=1))return {};
  result.folder=bool(folder);
  for(uint32_t i=0;i<count;++i){MediaReference::Member m;std::string root,relative;
    if(!r.String(m.guest_path,1024)||!r.String(root,4096)||!r.String(relative,1024)||
        !r.Wide(m.filesystem)||!r.Wide(m.directory)||!r.Wide(m.file))return {};
    m.root=xe::to_path(root);m.relative=xe::to_path(relative);
    if(!m.root.is_absolute() || m.root.lexically_normal()!=m.root ||
        (result.folder && (!m.guest_path.empty() || !m.relative.empty() || m.file)))return {};
    result.members.push_back(std::move(m));
  }
  const auto unknown=payload.subspan(r.at);if(!Unknown(unknown))return {};
  result.unknown.assign(unknown.begin(),unknown.end());return result;
}
std::vector<uint8_t> Encode(const MediaReference& ref) {
  std::vector<uint8_t> out(kMagic.begin(),kMagic.end());Text(out,ref.id);U32(out,ref.device_id);Text(out,ref.name);Text(out,ref.guest_volume_root);U32(out,ref.folder);U32(out,uint32_t(ref.members.size()));
  for(const auto& m:ref.members){Text(out,m.guest_path);Text(out,path_to_utf8(m.root));Text(out,path_to_utf8(m.relative));U64(out,m.filesystem);U64(out,m.directory);U64(out,m.file);}
  out.insert(out.end(),ref.unknown.begin(),ref.unknown.end());const auto hash=Hash(out);out.insert(out.end(),hash.begin(),hash.end());return out;
}
std::optional<FileHandleInfo> DirectoryInfo(const HostStorageRoot& root,std::error_code& error) {
  // Metadata observation only, bound back to the already retained directory.
  // No path-selected file handle is ever used as a media data reader.
  auto probe=FileHandle::OpenExisting(root.path(),FileAccess::kGenericRead);
  if(!probe){error=std::make_error_code(std::errc::no_such_device);return {};}
  auto info=probe->QueryInfo(error);
  if(!info || !info->directory || !root.directory_identity() ||
      !root.directory_identity()->MatchesNativeDirectory(info->filesystem_id,info->file_id)) {
    if(!error)error=std::make_error_code(std::errc::no_such_device);return {};
  }
  return info;
}
}
std::string MediaReferenceStore::NewId() {
  std::random_device random;
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 16; ++i) {
    const auto byte = uint8_t(random());
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  if (std::all_of(result.begin(), result.end(),
                  [](char c) { return c == '0'; })) {
    result.back() = '1';
  }
  return result;
}
std::shared_ptr<MediaReferenceStore::Metadata>
MediaReferenceStore::OpenMetadata(std::shared_ptr<HostStorageRoot> parent) {
  auto result = std::make_shared<Metadata>();
  auto& error = result->error_;
  if (!parent) {
    error = std::make_error_code(std::errc::no_such_device);
    return result;
  }
  error = parent->EnsureMediaReferenceReserve();
  if (error) {
    return result;
  }
  constexpr auto directory = ".xenia-media-v1";
  constexpr std::string_view ownership = "XENIA-MEDIA-REFERENCES-1\n";
  auto raw = parent->Subroot(directory, false, error);
  if (!raw && error == std::errc::no_such_file_or_directory) {
    result->bootstrap_ =
        parent->BeginBootstrapDirectory(directory, NewId(), error);
    if (!result->bootstrap_ || error) {
      return result;
    }
    const auto marker = result->bootstrap_->root()->PublishBootstrapRecord(
        "owner.v1",
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(ownership.data()),
            ownership.size()),
        NewId());
    error = marker.error;
    if (error) {
      return result;
    }
    const auto published =
        parent->PublishBootstrapDirectory(*result->bootstrap_);
    error = published.error;
    raw = published.canonical_root;
    if (!raw) {
      return result;
    }
    // Uncertain publication retains the actual stage/canonical bootstrap
    // protectors. Only a durable capsule may transfer to the typed flat owner.
    if (error || !published.durable) {
      if (!error) {
        error = std::make_error_code(std::errc::state_not_recoverable);
      }
      return result;
    }
    result->root_ =
        parent->ProtectMediaReferenceDirectory(*result->bootstrap_, error);
    if (!result->root_) {
      return result;
    }
  }
  if (!raw) {
    return result;
  }
  if (!result->root_) {
    const auto marker = raw->CaptureSnapshot("owner.v1", 128, error);
    if (!marker || !marker->exists() ||
        marker->bytes().size() != ownership.size() ||
        !std::equal(marker->bytes().begin(), marker->bytes().end(),
                    ownership.begin())) {
      if (!error) {
        error = std::make_error_code(std::errc::bad_message);
      }
      return result;
    }
    // Reclaim only this complete canonical owner and its exact old native
    // protection receipt. Unknown staging prefixes never authorize adoption.
    result->bootstrap_ =
        parent->ResumeRetainedBootstrapDirectory(directory, *raw, error);
    if (error) {
      return result;
    }
    if (result->bootstrap_) {
      const auto published =
          parent->PublishBootstrapDirectory(*result->bootstrap_);
      error = published.error;
      raw = published.canonical_root;
      if (!raw || error || !published.durable) {
        if (!error) {
          error = std::make_error_code(std::errc::state_not_recoverable);
        }
        return result;
      }
    }
    result->root_ = result->bootstrap_
        ? parent->ProtectMediaReferenceDirectory(*result->bootstrap_, error)
        : raw->ProtectFlat(error);
    if (!result->root_) {
      return result;
    }
  }
  const auto marker = result->root_->CaptureSnapshot("owner.v1", 128, error);
  if (!marker || !marker->exists() ||
      marker->bytes().size() != ownership.size() ||
      !std::equal(marker->bytes().begin(), marker->bytes().end(),
                  ownership.begin())) {
    if (!error) {
      error = std::make_error_code(std::errc::bad_message);
    }
    return result;
  }
  const auto observed = result->root_->ObserveIntentSnapshot(*marker);
  error = observed.error;
  if (!error && !observed.durable) {
    error = std::make_error_code(std::errc::io_error);
  }
  if (!error) {
    error = parent->FlushDirectory();
  }
  // The genuine marker/directory barriers and typed protection now own the
  // metadata. Do not retain the parent EX bootstrap lease during playback.
  if (!error) {
    result->bootstrap_.reset();
  }
  return result;
}

std::vector<MediaReference> MediaReferenceStore::List(
    const HostStorageRoot& metadata, std::error_code& error) {
  auto leaves = metadata.ListMediaReferenceFiles(error);
  std::vector<MediaReference> result;
  if (error) {
    return result;
  }
  if (leaves.size() > kMaximumReferences) {
    error = std::make_error_code(std::errc::file_too_large);
    return {};
  }
  uint64_t total = 0;
  for (const auto& leaf : leaves) {
    auto token = metadata.CaptureSnapshot(leaf, kMaximumWireBytes, error, true);
    if (!token || error) {
      return {};
    }
    if (!token->exists()) {
      error = std::make_error_code(std::errc::resource_unavailable_try_again);
      return {};
    }
    if (token->bytes().size() >
        HostStorageRoot::kMaximumSnapshotBatchBytes - total) {
      error = std::make_error_code(std::errc::file_too_large);
      return {};
    }
    total += token->bytes().size();
    auto reference = Parse(token->bytes());
    if (!reference || Leaf(reference->id) != leaf) {
      error = Invalid();
      return {};
    }
    result.push_back(std::move(*reference));
  }
  return result;
}
std::filesystem::path MediaReferenceStore::Leaf(std::string_view id) {
  if(!Id(id))return {};const auto hash=Hash({reinterpret_cast<const uint8_t*>(id.data()),id.size()});
  std::string name="media-";constexpr char digits[]="0123456789abcdef";
  for(auto c:hash){name+=digits[c>>4];name+=digits[c&15];}return name+".v1";
}
std::optional<MediaReference> MediaReferenceStore::Describe(std::string id,
    uint32_t device,std::string name,std::string volume,const MediaSelection& selection,std::error_code& error) {
  if(!Id(id)||!device||name.size()>256||name.find('\0')!=std::string::npos||!Volume(volume)){error=Invalid();return {};}
  auto inputs=selection.ReferenceInputs(error);if(error || inputs.empty())return {};
  if(!selection.ProbeRetained().available){error=std::make_error_code(std::errc::no_such_device);return {};}
  MediaReference out;out.id=std::move(id);out.device_id=device;out.name=std::move(name);out.guest_volume_root=std::move(volume);out.folder=selection.folder();
  for(const auto& captured:inputs) {
    const auto& input=captured.input;
    const auto dir=DirectoryInfo(*input.root,error);if(!dir)return {};
    MediaReference::Member m;m.guest_path=input.guest_path;m.root=input.root->path();m.relative=input.relative;m.filesystem=dir->filesystem_id;m.directory=dir->file_id;
    if(!out.folder){auto file=input.root->OpenFile(input.relative,FileAccess::kGenericRead,error);if(!file)return {};const auto info=file->QueryInfo(error);if(!info || !info->regular || info->directory)return {};m.file=info->file_id;if(info->filesystem_id!=captured.opened.filesystem_id || info->file_id!=captured.opened.file_id){error=std::make_error_code(std::errc::no_such_device);return {};}if(info->filesystem_id!=m.filesystem){error=std::make_error_code(std::errc::cross_device_link);return {};}}
    out.members.push_back(std::move(m));
  }
  return out;
}
LoadedMediaReference MediaReferenceStore::Load(const HostStorageRoot& metadata,std::string_view id) {
  LoadedMediaReference out;if(!metadata.is_guest_private()){out.error=std::make_error_code(std::errc::permission_denied);return out;}const auto leaf=Leaf(id);if(leaf.empty()){out.error=Invalid();return out;}
  auto snapshot=metadata.CaptureSnapshot(leaf,kMaximumWireBytes,out.error,true);
  if(!snapshot)return out;
  if(!snapshot->exists()){out.error=std::make_error_code(std::errc::no_such_file_or_directory);return out;}
  out.reference=Parse(snapshot->bytes());if(!out.reference || out.reference->id!=id){out.error=Invalid();return out;}
  std::vector<MediaSelection::FileInput> files;
  for(const auto& m:out.reference->members){
    auto root=HostStorageRoot::Open(m.root,true,out.error);if(!root)return out;
    const auto dir=DirectoryInfo(*root,out.error);if(!dir)return out;
    if(dir->filesystem_id!=m.filesystem || dir->file_id!=m.directory){out.error=std::make_error_code(std::errc::no_such_device);return out;}
    if(out.reference->folder){out.selection=MediaSelection::CaptureFolder(root,out.error);return out;}
    auto file=root->OpenFile(m.relative,FileAccess::kGenericRead,out.error);if(!file)return out;
    const auto info=file->QueryInfo(out.error);
    if(!info || !info->regular || info->directory || info->filesystem_id!=m.filesystem || info->file_id!=m.file){if(!out.error)out.error=std::make_error_code(std::errc::no_such_device);return out;}
    files.push_back({m.guest_path,std::move(root),m.relative});
  }
  out.selection=MediaSelection::CaptureFiles(std::move(files),out.error);
  if(out.selection) {
    // Bind the ACTUALLY retained readers too; a name race between observation
    // and capture must not silently select a different host file.
    const auto captured=out.selection->ReferenceInputs(out.error);
    if(captured.size()!=out.reference->members.size())out.error=Invalid();
    for(size_t n=0;!out.error && n<captured.size();++n)
      if(captured[n].opened.filesystem_id!=out.reference->members[n].filesystem ||
          captured[n].opened.file_id!=out.reference->members[n].file)
        out.error=std::make_error_code(std::errc::no_such_device);
    if(out.error){out.selection->Retire();out.selection.reset();}
  }
  return out;
}
SnapshotPublicationReceipt MediaReferenceStore::Save(
    const HostStorageRoot& metadata, const SnapshotRecoveryOwner& recovery,
    std::string id, uint32_t device, std::string name, std::string volume,
    const MediaSelection& selection, const std::atomic<bool>* canceled,
    MediaReference* attempted) {
  SnapshotPublicationReceipt failed;const auto leaf=Leaf(id);if(leaf.empty()){failed.error=Invalid();return failed;}
  // This host intent belongs in an actual owner-private metadata directory.
  // Never put host paths into a guest Content/media namespace.
  if(!metadata.is_guest_private()){failed.error=std::make_error_code(std::errc::permission_denied);return failed;}
  auto expected=metadata.CaptureSnapshot(leaf,kMaximumWireBytes,failed.error,true);if(!expected)return failed;
  auto reference=Describe(std::move(id),device,std::move(name),std::move(volume),selection,failed.error);if(!reference)return failed;
  if(expected->exists()){const auto previous=Parse(expected->bytes());if(!previous || previous->id!=reference->id){failed.error=Invalid();return failed;}reference->unknown=previous->unknown;}
  const auto bytes=Encode(*reference);if(bytes.size()>kMaximumWireBytes){failed.error=std::make_error_code(std::errc::file_too_large);return failed;}
  if (attempted) {
    *attempted = *reference;  // copied BEFORE any publication
  }
  return SnapshotPublisher::PublishMediaReference(recovery,std::move(expected),bytes,canceled);
}
SnapshotPublicationReceipt MediaReferenceStore::Remove(const HostStorageRoot& metadata,
    const SnapshotRecoveryOwner& recovery,std::string_view id,const std::atomic<bool>* canceled) {
  SnapshotPublicationReceipt failed;const auto leaf=Leaf(id);
  if(leaf.empty()){failed.error=Invalid();return failed;}
  if(!metadata.is_guest_private()){failed.error=std::make_error_code(std::errc::permission_denied);return failed;}
  auto expected=metadata.CaptureSnapshot(leaf,kMaximumWireBytes,failed.error,true);if(!expected)return failed;
  if(!expected->exists()){failed.error=std::make_error_code(std::errc::no_such_file_or_directory);return failed;}
  const auto known=Parse(expected->bytes());if(!known || known->id!=id){failed.error=Invalid();return failed;}
  return SnapshotPublisher::DeleteInternal(recovery,std::move(expected),canceled,PrivateSnapshotPurpose::kMediaReference);
}
}  // namespace xe::filesystem
