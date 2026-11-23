/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/console_signing_identity.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/platform_util.h"
#include "third_party/mbedtls/include/mbedtls/rsa.h"
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/mutex.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/xboxkrnl/xecrypt_rsa.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_crypt.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
#include <sys/file.h>
#endif
namespace xe::kernel {
namespace {
constexpr char kLock[]="console-signing.lock",kStage[]="console-signing.stage.v1";
// Exact private format, not a keyvault or a retail certificate. Never log its
// contents. The digest detects torn/corrupt storage; RSA validates key binding.
constexpr size_t kPrefix=16,kN=128,kP=64,kQ=64,kD=128,kE=4;
constexpr size_t kPrivate=kN+kP+kQ+kD+kE;
constexpr size_t kWire=kPrefix+ConsoleSigningIdentity::kCertificateBytes+kPrivate+32;
std::error_code Bad(){return std::make_error_code(std::errc::bad_message);}
std::error_code Io(){return std::make_error_code(std::errc::io_error);}
struct SecretBytes {
 std::vector<uint8_t> bytes;
 void Resize(size_t size){if(!bytes.empty())mbedtls_platform_zeroize(bytes.data(),bytes.size());bytes.assign(size,0);}
 ~SecretBytes(){if(!bytes.empty())mbedtls_platform_zeroize(bytes.data(),bytes.size());}
};
std::error_code ReadSecret(const filesystem::HostStorageRoot& root,
    const std::filesystem::path& path,SecretBytes& output){
 std::error_code error;
 auto file=root.OpenFile(path,filesystem::FileAccess::kFileReadData,error);
 if(!file)return error;
 const auto before=file->QueryInfo(error);if(!before)return error;
 if(!before->regular || before->directory || before->links!=1)return std::make_error_code(std::errc::permission_denied);
 if(before->size>kWire || before->size>SIZE_MAX)return std::make_error_code(std::errc::file_too_large);
 // Read DIRECTLY into the erasing owner. In particular a short/native read or
 // later QueryInfo failure cannot free an unzeroized generic temporary.
 output.Resize(size_t(before->size));size_t done=0;
 while(done<output.bytes.size()){
  const auto read=file->ReadResult(done,std::span(output.bytes).subspan(done));
  if(read.error)return read.error;
  if(!read.bytes || read.bytes>output.bytes.size()-done)return Io();
  done+=read.bytes;
 }
 const auto after=file->QueryInfo(error);if(!after)return error;
 if(!after->regular || after->directory || after->links!=1 || after->size!=before->size ||
     after->filesystem_id!=before->filesystem_id || after->file_id!=before->file_id)
  return std::make_error_code(std::errc::state_not_recoverable);
 return {};
}

int Random(void*,unsigned char* out,size_t size){
 return xboxkrnl::XeCryptHostRandom(std::span(out,size))?0:MBEDTLS_ERR_RSA_RNG_FAILED;
}
class BootstrapLock {
 public:
 explicit BootstrapLock(intptr_t fd,std::error_code& error){
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if(flock(int(fd),LOCK_EX|LOCK_NB))error=std::error_code(errno,std::generic_category());
  else fd_=int(fd);
#else
  error=std::make_error_code(std::errc::operation_not_supported);
#endif
 }
 ~BootstrapLock(){
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if(fd_>=0)while(flock(fd_,LOCK_UN)==-1 && errno==EINTR){}
#endif
 }
 private:int fd_=-1;
};
bool PublicKey(std::span<const uint8_t> certificate,std::array<uint8_t,144>& key){
 if(certificate.size()!=0x1A8 || certificate[0]!=1 || certificate[1]!=0xA8)return false;
 key={};key[3]=16;
 std::copy_n(certificate.data()+0x24,4,key.data()+4);
 std::copy_n(certificate.data()+0x28,128,key.data()+16);
 return true;
}
} // namespace
struct ConsoleSigningIdentity::State {
 std::shared_ptr<filesystem::HostStorageRoot> root;
 Certificate certificate{};
 mutable std::mutex mutex;
 mutable mbedtls_rsa_context rsa;
 explicit State(std::shared_ptr<filesystem::HostStorageRoot> r):root(std::move(r)){mbedtls_rsa_init(&rsa);}
 ~State(){mbedtls_rsa_free(&rsa);}
 bool ValidKey(){
  if(mbedtls_rsa_complete(&rsa) || mbedtls_rsa_check_privkey(&rsa) ||
     mbedtls_rsa_get_len(&rsa)!=128 ||
     mbedtls_rsa_set_padding(&rsa,MBEDTLS_RSA_PKCS_V15,MBEDTLS_MD_SHA1))return false;
  std::array<uint8_t,128> n{};std::array<uint8_t,4> e{};
  if(mbedtls_rsa_export_raw(&rsa,n.data(),n.size(),nullptr,0,nullptr,0,nullptr,0,e.data(),e.size()))return false;
  if(e!=std::array<uint8_t,4>{0,1,0,1})return false;
  for(size_t i=0;i<16;++i)if(!std::equal(n.begin()+i*8,n.begin()+(i+1)*8,certificate.begin()+0x28+(15-i)*8))return false;
  return std::equal(e.begin(),e.end(),certificate.begin()+0x24);
 }
 bool Decode(std::span<const uint8_t> wire){
  if(wire.size()!=kWire || std::memcmp(wire.data(),"XEASIGN1",8) ||
     wire[8] || wire[9] || wire[10] || wire[11]!=1 ||
     wire[12] || wire[13] || wire[14] || wire[15])return false;
  std::array<uint8_t,32> hash{};
  if(mbedtls_sha256(wire.data(),kWire-32,hash.data(),0) ||
     !std::equal(hash.begin(),hash.end(),wire.end()-32))return false;
  std::copy_n(wire.data()+kPrefix,certificate.size(),certificate.begin());
  // The explicit local marker and absence of a Microsoft signature describe
  // emulator-owned identity; neither an arbitrary imported cert nor KV is
  // silently adopted by this file format.
  if(certificate[0]!=1 || certificate[1]!=0xA8 ||
     std::memcmp(certificate.data()+0x12,"XEA1",4) ||
     certificate[0x18] || certificate[0x19] || certificate[0x1A] || certificate[0x1B]!=2 ||
     !std::all_of(certificate.begin()+0xA8,certificate.end(),[](uint8_t b){return !b;}))return false;
  auto* p=wire.data()+kPrefix+certificate.size();
  if(mbedtls_rsa_import_raw(&rsa,p,kN,p+kN,kP,p+kN+kP,kQ,p+kN+kP+kQ,kD,p+kN+kP+kQ+kD,kE))return false;
  return ValidKey();
 }
 bool Generate(SecretBytes& out){
  if(mbedtls_rsa_gen_key(&rsa,Random,nullptr,1024,65537) ||
     mbedtls_rsa_set_padding(&rsa,MBEDTLS_RSA_PKCS_V15,MBEDTLS_MD_SHA1))return false;
  out.Resize(kWire);auto* p=out.bytes.data();
  std::memcpy(p,"XEASIGN1",8);p[11]=1;
  auto* key=p+kPrefix+certificate.size();
  if(mbedtls_rsa_export_raw(&rsa,key,kN,key+kN,kP,key+kN+kP,kQ,key+kN+kP+kQ,kD,key+kN+kP+kQ+kD,kE))return false;
  certificate={};certificate[0]=1;certificate[1]=0xA8;
  std::memcpy(certificate.data()+7,"AFTERDARK",9);
  std::memcpy(certificate.data()+0x12,"XEA1",4);certificate[0x1B]=2;
  // A reproducible public console ID derives from this real public key. The
  // eight-byte manufacture-date field is zero: no retail history is invented.
  std::array<uint8_t,20> public_hash{};
  if(mbedtls_sha1(key,kN,public_hash.data()))return false;
  std::copy_n(public_hash.begin(),5,certificate.begin()+2);
  std::copy_n(key+kN+kP+kQ+kD,4,certificate.begin()+0x24);
  for(size_t i=0;i<16;++i)std::copy_n(key+i*8,8,certificate.begin()+0x28+(15-i)*8);
  if(!ValidKey())return false;
  std::copy(certificate.begin(),certificate.end(),out.bytes.begin()+kPrefix);
  return !mbedtls_sha256(p,kWire-32,p+kWire-32,0);
 }
};
ConsoleSigningIdentity::ConsoleSigningIdentity(std::unique_ptr<State> state):state_(std::move(state)){}
ConsoleSigningIdentity::~ConsoleSigningIdentity()=default;
std::shared_ptr<ConsoleSigningIdentity> ConsoleSigningIdentity::Open(
    std::shared_ptr<filesystem::HostStorageRoot> root,std::error_code& error) try {
 error.clear();
 if(global_critical_region::is_held_by_current_thread() || !root || !root->is_guest_private() || root->retiring()){
  error=std::make_error_code(std::errc::permission_denied);return {};
 }
 constexpr uint32_t read=filesystem::FileAccess::kFileReadData,write=filesystem::FileAccess::kFileWriteData;
 auto lock_file=root->OpenFile(kLock,read,error);bool created=false;
 if(!lock_file && error==std::errc::no_such_file_or_directory && !root->read_only()){
  lock_file=root->OpenFile(kLock,read|write,error,true);created=bool(lock_file);
  if(!lock_file && error==std::errc::file_exists)lock_file=root->OpenFile(kLock,read,error);
 }
 if(!lock_file)return {};
 const auto info=lock_file->QueryInfo(error);
 if(!info || !info->regular || info->links!=1 || lock_file->native_handle()<0){if(!error)error=std::make_error_code(std::errc::permission_denied);return {};}
 if(created){error=lock_file->FlushResult();if(!error)error=root->FlushDirectory();if(error)return {};}
 BootstrapLock lock(lock_file->native_handle(),error);if(error)return {};
 auto state=std::make_unique<State>(root);SecretBytes wire;
 error=ReadSecret(*root,kIdentityFile,wire);
 if(!error){
  if(!state->Decode(wire.bytes)){error=Bad();return {};}
  SecretBytes staged;const auto stage_error=ReadSecret(*root,kStage,staged);
  if(stage_error && stage_error!=std::errc::no_such_file_or_directory){error=stage_error;return {};}
  // Both-present is admissible only for the exact SAME complete identity.
  // A conflicting/partial stage is preserved but never silently ignored.
  if(!stage_error && staged.bytes!=wire.bytes){error=Bad();return {};}
 }
 else if(error==std::errc::no_such_file_or_directory && !root->read_only()){
  // One exclusive staging leaf is a resumable bootstrap, not a disposable
  // duplicate. A fully validated prior stage resumes the SAME identity;
  // malformed/short/unreadable prior bytes are preserved and refused.
  error=ReadSecret(*root,kStage,wire);
  if(!error){if(!state->Decode(wire.bytes)){error=Bad();return {};}}
  else if(error==std::errc::no_such_file_or_directory){
   error.clear();if(!state->Generate(wire)){error=Io();return {};}
   const auto write_result=root->WriteNewFile(kStage,wire.bytes,true);
   if(write_result.error || write_result.bytes!=wire.bytes.size()){error=write_result.error?write_result.error:Io();return {};}
  }else return {};
  // The canonical identity is never overwritten: an existing target requires
  // a backup in this common API, and none is supplied. Unknown/partial stages
  // are preserved; no failed create grants cleanup ownership of a name.
  const auto published=root->AtomicReplaceFile(kStage,kIdentityFile);
  if(published.error || !published.published || !published.durable){error=published.error?published.error:Io();return {};}
 }else return {};
 // This also settles a prior published-but-parent-fsync-uncertain bootstrap
 // before exposing signing. Recheck the complete same bytes after actual file
 // and parent durability; no replacement or regeneration is authorized here.
 auto durable=root->OpenFile(kIdentityFile,read,error);if(!durable)return {};
 error=durable->FlushResult();if(!error)error=root->FlushDirectory();
 if(error)return {};
 SecretBytes observed;error=ReadSecret(*root,kIdentityFile,observed);
 if(error || wire.bytes!=observed.bytes){if(!error)error=std::make_error_code(std::errc::state_not_recoverable);return {};}
 return std::shared_ptr<ConsoleSigningIdentity>(new ConsoleSigningIdentity(std::move(state)));
}catch(const std::bad_alloc&){error=std::make_error_code(std::errc::not_enough_memory);return {};}
catch(...){error=Io();return {};}
const ConsoleSigningIdentity::Certificate& ConsoleSigningIdentity::certificate() const noexcept{return state_->certificate;}
std::array<uint8_t,5> ConsoleSigningIdentity::console_id() const noexcept {
 std::array<uint8_t,5> out{};std::copy_n(state_->certificate.begin()+2,5,out.begin());return out;
}
std::array<uint8_t,12> ConsoleSigningIdentity::console_id_text() const noexcept {
 uint64_t value=0;for(auto b:console_id())value=(value<<8)|b;
 std::array<uint8_t,12> out{};constexpr char hex[]="0123456789abcdef";
 out[11]=hex[value&15];value>>=4;
 for(size_t i=11;i;i--){out[i-1]='0'+value%10;value/=10;}return out;
}
bool ConsoleSigningIdentity::Sign(std::span<const uint8_t> hash,Signature& output) const try {
 if(hash.size()!=20 || state_->root->retiring())return false;
 std::array<uint8_t,128> big{};Signature owned{};
 {std::lock_guard lock(state_->mutex);
  if(mbedtls_rsa_pkcs1_sign(&state_->rsa,Random,nullptr,MBEDTLS_MD_SHA1,20,hash.data(),big.data()))return false;}
 std::copy(state_->certificate.begin(),state_->certificate.end(),owned.begin());
 std::reverse_copy(big.begin(),big.end(),owned.begin()+kCertificateBytes);
 uint32_t difference=1;if(!Verify(hash,owned,difference) || difference)return false;
 output=owned;return true;
}catch(...){return false;}
bool ConsoleSigningIdentity::Verify(std::span<const uint8_t> hash,
    std::span<const uint8_t> signature,uint32_t& difference,
    std::span<const uint8_t> master) const {
 if(hash.size()!=20 || signature.size()!=kSignatureBytes || state_->root->retiring())return false;
 const auto cert=signature.first(kCertificateBytes);
 difference=0;for(size_t i=0;i<cert.size();++i)difference|=uint32_t(cert[i]^state_->certificate[i]);
 if(difference){
  std::array<uint8_t,20> digest{};
  if(master.size()!=272 || mbedtls_sha1(cert.data(),0xA8,digest.data()) ||
     !XeCryptVerifyRsaSha1Pkcs1(master,digest,cert.subspan(0xA8,256)))return false;
 }
 std::array<uint8_t,144> key{};
 return PublicKey(cert,key) && XeCryptVerifyRsaSha1Pkcs1(key,hash,signature.subspan(kCertificateBytes,128));
}
X_STATUS ConsoleSigningIdGuest(Memory& memory,const ConsoleSigningIdentity& identity,uint32_t raw_bytes,uint32_t text) {
 // Native public cross-check writes raw ID before its formatted text, so
 // aliased outputs retain the genuine copy order rather than being rejected.
 if(raw_bytes){const auto raw=identity.console_id();
  const auto status=util::WriteGuestIoBuffer(memory,raw_bytes,raw);if(XFAILED(status))return status;}
 if(text){const auto value=identity.console_id_text();return util::WriteGuestIoBuffer(memory,text,value);}
 return X_STATUS_SUCCESS;
}
X_STATUS ConsoleSigningCertificateGuest(Memory& memory,const ConsoleSigningIdentity& identity,uint32_t output) {
 return util::WriteGuestIoBuffer(memory,output,identity.certificate());
}
bool ConsoleSigningSignGuest(Memory& memory,const ConsoleSigningIdentity& identity,uint32_t hash,uint32_t output) {
 if(!util::GuestIoRangeValid(memory,output,0x228,true))return false;
 std::array<uint8_t,20> owned_hash;ConsoleSigningIdentity::Signature owned_output;
 if(XFAILED(util::ReadGuestIoBuffer(memory,hash,owned_hash)) || !identity.Sign(owned_hash,owned_output))return false;
 return XSUCCEEDED(util::WriteGuestIoBuffer(memory,output,owned_output));
}
bool ConsoleSigningVerifyGuest(Memory& memory,const ConsoleSigningIdentity& identity,uint32_t hash,uint32_t signature,uint32_t difference) {
 std::array<uint8_t,20> owned_hash;ConsoleSigningIdentity::Signature owned_signature;
 if(XFAILED(util::ReadGuestIoBuffer(memory,hash,owned_hash)) ||
     XFAILED(util::ReadGuestIoBuffer(memory,signature,owned_signature)))return false;
 uint32_t comparison=0;const bool valid=identity.Verify(owned_hash,owned_signature,comparison);
 const xe::be<uint32_t> encoded=comparison;
 if(difference && XFAILED(util::WriteGuestIoBuffer(memory,difference,
     {reinterpret_cast<const uint8_t*>(&encoded),sizeof(encoded)})))return false;
 return valid;
}
} // namespace xe::kernel
