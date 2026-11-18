/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/cache_storage_owner.h"
namespace xe::filesystem::cache_storage_detail {
using Digest=std::array<uint8_t,32>;
using Nonce=std::array<uint8_t,16>;
inline std::error_code Bad() {return std::make_error_code(std::errc::bad_message);}
inline std::error_code Conflict() {return std::make_error_code(std::errc::state_not_recoverable);}
inline Digest Hash(std::span<const uint8_t> bytes,std::error_code& error) {
  Digest out{};
  if(mbedtls_sha256(bytes.data(),bytes.size(),out.data(),0))error=std::make_error_code(std::errc::io_error);
  return out;
}
inline std::string Hex(std::span<const uint8_t> bytes) {
  constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(bytes.size()*2);
  for(auto b:bytes){out+=digits[b>>4];out+=digits[b&15];}return out;
}
inline void Number(std::vector<uint8_t>& out,uint64_t n) {
  for(int shift=56;shift>=0;shift-=8)out.push_back(uint8_t(n>>shift));
}
inline void Raw(std::vector<uint8_t>& out,std::span<const uint8_t> bytes) {
  out.insert(out.end(),bytes.begin(),bytes.end());
}
inline void Text(std::vector<uint8_t>& out,std::string_view text) {
  Number(out,text.size());Raw(out,{reinterpret_cast<const uint8_t*>(text.data()),text.size()});
}
inline void Seal(std::vector<uint8_t>& out,std::error_code& error) {
  if(error)return;
  if(out.size()>65504){error=std::make_error_code(std::errc::file_too_large);return;}
  auto digest=Hash(out,error);if(!error)Raw(out,digest);
}
struct Reader {
  std::span<const uint8_t> rest;
  std::error_code error;
  Reader(std::span<const uint8_t> wire,std::string_view magic) {
    if(wire.size()<40 || wire.size()>65536 || magic.size()!=8 ||
       !std::equal(magic.begin(),magic.end(),wire.begin())){error=Bad();return;}
    const auto hash=Hash(wire.first(wire.size()-32),error);
    if(error || !std::equal(hash.begin(),hash.end(),wire.end()-32)){error=Bad();return;}
    rest=wire.subspan(8,wire.size()-40);
  }
  uint64_t Number() {
    if(rest.size()<8){error=Bad();return 0;}uint64_t n=0;
    for(auto b:rest.first(8))n=(n<<8)|b;rest=rest.subspan(8);return n;
  }
  std::vector<uint8_t> Bytes(size_t maximum) {
    const auto n=Number();if(error || n>maximum || n>rest.size()){error=Bad();return {};}
    std::vector<uint8_t> out(rest.begin(),rest.begin()+size_t(n));rest=rest.subspan(size_t(n));return out;
  }
  std::string Text(size_t maximum) {
    auto bytes=Bytes(maximum);
    if(error || bytes.empty() || std::find(bytes.begin(),bytes.end(),0)!=bytes.end()){error=Bad();return {};}
    return {bytes.begin(),bytes.end()};
  }
  template<size_t N> void Raw(std::array<uint8_t,N>& out) {
    if(rest.size()<N){error=Bad();return;}std::copy_n(rest.begin(),N,out.begin());rest=rest.subspan(N);
  }
  bool Done(std::error_code& out) {
    if(!rest.empty() && !error)error=Bad();if(error)out=error;return !error;
  }
};
struct Directory {
  std::string path;
  uint64_t filesystem=0,directory=0;
  bool operator==(const Directory&) const=default;
};
inline bool Valid(const Directory& d) {
  return !d.path.empty() && d.path.size()<=4096 && d.filesystem && d.directory &&
      std::filesystem::path(d.path).is_absolute() && d.path.find('\0')==std::string::npos;
}
inline void Encode(std::vector<uint8_t>& out,const Directory& d) {
  Text(out,d.path);Number(out,d.filesystem);Number(out,d.directory);
}
inline Directory DecodeDirectory(Reader& r) {
  Directory d{r.Text(4096),r.Number(),r.Number()};if(!Valid(d))r.error=Bad();return d;
}
struct Slot {
  Directory source;
  std::string source_id;
  Nonce uuid{};
  uint64_t initial_epoch=0,binding_directory=0;
  std::vector<uint8_t> prefix;
  bool operator==(const Slot&) const=default;
};
// Immutable cache-purpose bootstrap declaration, shared by all three marker
// references. The purpose is explicit and cannot be decoded as Content B2.
struct Binding {
  uint64_t kind=0,product=0;
  Directory private_parent,table,recovery;
  Nonce nonce{};
  std::array<Slot,3> slots;
  std::vector<Directory> preserved;
  bool operator==(const Binding&) const=default;
};
inline std::vector<uint8_t> EncodeBinding(const Binding& b,std::error_code& error) {
  if(!Valid(b.private_parent) || !Valid(b.table) || !Valid(b.recovery) || b.preserved.size()>64 ||
      (b.kind!=1 && b.kind!=2) || !b.product || b.product>UINT32_MAX){error=Bad();return {};}
  for(const auto& slot:b.slots)if(!Valid(slot.source) || slot.source_id.empty() || slot.source_id.size()>256 ||
      slot.source_id.find('\0')!=std::string::npos || slot.prefix.empty() || slot.prefix.size()>16384){error=Bad();return {};}
  for(const auto& d:b.preserved)if(!Valid(d)){error=Bad();return {};}
  std::vector<uint8_t> out{'X','C','A','C','H','B','1','\n'};
  Number(out,b.kind);Number(out,b.product);Encode(out,b.private_parent);Encode(out,b.table);Encode(out,b.recovery);Raw(out,b.nonce);
  for(const auto& slot:b.slots){Encode(out,slot.source);Text(out,slot.source_id);Raw(out,slot.uuid);
    Number(out,slot.initial_epoch);Number(out,slot.binding_directory);Number(out,slot.prefix.size());Raw(out,slot.prefix);}
  Number(out,b.preserved.size());for(const auto& d:b.preserved)Encode(out,d);
  Seal(out,error);return error?std::vector<uint8_t>{}:out;
}
inline Binding DecodeBinding(std::span<const uint8_t> wire,std::error_code& error) {
  Reader r(wire,"XCACHB1\n");Binding b;b.kind=r.Number();b.product=r.Number();
  b.private_parent=DecodeDirectory(r);b.table=DecodeDirectory(r);b.recovery=DecodeDirectory(r);r.Raw(b.nonce);
  for(auto& slot:b.slots){slot.source=DecodeDirectory(r);slot.source_id=r.Text(256);r.Raw(slot.uuid);
    slot.initial_epoch=r.Number();slot.binding_directory=r.Number();slot.prefix=r.Bytes(16384);
    if(!slot.initial_epoch || !slot.binding_directory || slot.source_id.empty() || slot.prefix.empty() ||
       slot.source.filesystem!=b.table.filesystem)r.error=Bad();}
  const auto count=r.Number();if(count>64)r.error=Bad();
  if(!r.error)for(uint64_t i=0;i<count;++i){
    auto d=DecodeDirectory(r);const auto path=std::filesystem::path(d.path);
    const auto leaf=path.filename().string();
    const bool known_parent=path.parent_path()==b.private_parent.path ||
        std::any_of(b.slots.begin(),b.slots.end(),[&](const auto& slot){return path.parent_path()==slot.source.path;});
    if(!known_parent || d.filesystem!=b.table.filesystem || leaf.size()!=53 ||
       !leaf.starts_with(".xenia-bootstrap-") || !leaf.ends_with(".dir") ||
       std::any_of(b.preserved.begin(),b.preserved.end(),[&](const auto& old){return old.path==d.path ||
         (old.filesystem==d.filesystem && old.directory==d.directory);}))r.error=Bad();
    b.preserved.push_back(std::move(d));
  }
  if((b.kind!=1 && b.kind!=2) || !b.product || b.product>UINT32_MAX ||
     b.private_parent.filesystem!=b.table.filesystem || b.recovery.filesystem!=b.table.filesystem ||
     std::filesystem::path(b.recovery.path).parent_path()!=b.private_parent.path ||
     std::filesystem::path(b.recovery.path).filename()!=SnapshotRecoveryOwner::kDirectory ||
     !std::any_of(b.nonce.begin(),b.nonce.end(),[](auto x){return x!=0;}))r.error=Bad();
  r.Done(error);return b;
}
struct Commit {
  Digest purpose{};
  std::array<uint64_t,3> marker_file{};
};
inline std::vector<uint8_t> EncodeCommit(const Commit& c,std::error_code& error) {
  std::vector<uint8_t> out{'X','C','A','C','G','C','1','\n'};Raw(out,c.purpose);
  for(auto file:c.marker_file)Number(out,file);Seal(out,error);return error?std::vector<uint8_t>{}:out;
}
inline Commit DecodeCommit(std::span<const uint8_t> wire,std::error_code& error) {
  Reader r(wire,"XCACGC1\n");Commit c;r.Raw(c.purpose);
  for(auto& file:c.marker_file){file=r.Number();if(!file)r.error=Bad();}r.Done(error);return c;
}
struct Table {
  Digest purpose{};
  std::array<Nonce,3> uuid;
  std::array<uint64_t,3> epoch{};
  CacheStorageOwner::LogicalTable logical;
  bool operator==(const Table&) const=default;
};
inline bool Mountable(uint32_t sectors) {
  return sectors>=8 && sectors<=0x80 && !(sectors&(sectors-1));
}
inline std::vector<uint8_t> EncodeTable(const Table& t,std::error_code& error) {
  std::vector<uint8_t> out{'X','C','A','C','T','0','1','\n'};Raw(out,t.purpose);
  for(size_t i=0;i<3;++i){Raw(out,t.uuid[i]);Number(out,t.epoch[i]);}
  for(auto n:t.logical.sectors_per_cluster)Number(out,n);Raw(out,t.logical.bytes);Seal(out,error);
  return error?std::vector<uint8_t>{}:out;
}
inline Table DecodeTable(std::span<const uint8_t> wire,std::error_code& error) {
  Reader r(wire,"XCACT01\n");Table t;r.Raw(t.purpose);
  for(size_t i=0;i<3;++i){r.Raw(t.uuid[i]);t.epoch[i]=r.Number();if(!t.epoch[i])r.error=Bad();}
  for(auto& n:t.logical.sectors_per_cluster){const auto v=r.Number();
    if(v>UINT32_MAX || (v && !Mountable(uint32_t(v))))r.error=Bad();n=uint32_t(v);}
  r.Raw(t.logical.bytes);r.Done(error);return t;
}
struct FormatPlan {
  Digest purpose{};
  uint64_t slot=0,sectors=0,before_file=0,stage_file=0,recovery_file=0;
  std::string stage,recovery;
  std::vector<uint8_t> before,intended;
};
inline std::vector<uint8_t> EncodeFormat(const FormatPlan& p,std::error_code& error) {
  if(p.stage.find('\0')!=std::string::npos || p.recovery.find('\0')!=std::string::npos ||
      p.stage.size()>128 || p.recovery.size()>128 || p.before.size()>4096 || p.intended.size()>4096){error=Bad();return {};}
  std::vector<uint8_t> out{'X','C','A','F','M','0','1','\n'};Raw(out,p.purpose);Number(out,p.slot);
  Number(out,p.sectors);Number(out,p.before_file);Number(out,p.stage_file);Number(out,p.recovery_file);
  Text(out,p.stage);Text(out,p.recovery);
  Number(out,p.before.size());Raw(out,p.before);Number(out,p.intended.size());Raw(out,p.intended);Seal(out,error);
  return error?std::vector<uint8_t>{}:out;
}
inline FormatPlan DecodeFormat(std::span<const uint8_t> wire,std::error_code& error) {
  Reader r(wire,"XCAFM01\n");FormatPlan p;r.Raw(p.purpose);p.slot=r.Number();p.sectors=r.Number();p.before_file=r.Number();p.stage_file=r.Number();p.recovery_file=r.Number();
  p.stage=r.Text(128);p.recovery=r.Text(128);p.before=r.Bytes(4096);p.intended=r.Bytes(4096);
  if(p.slot>=3 || p.sectors>UINT32_MAX || (p.slot && !Mountable(uint32_t(p.sectors))) ||
      (!p.slot && p.sectors) || !p.before_file || !p.stage_file || !p.recovery_file ||
      p.before.empty() || p.intended.empty() ||
      p.stage.size()!=49 || !p.stage.starts_with(".xenia-stage-") || !p.stage.ends_with(".tmp") ||
      p.recovery.size()!=45 || !p.recovery.starts_with("snapshot-") || !p.recovery.ends_with(".bin"))r.error=Bad();
  const auto hex=[](std::string_view name,size_t begin,size_t end){return std::all_of(name.begin()+begin,name.begin()+end,
      [](unsigned char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});};
  if(!r.error && (!hex(p.stage,13,45) || !hex(p.recovery,9,41)))r.error=Bad();
  r.Done(error);return p;
}
}  // namespace xe::filesystem::cache_storage_detail
