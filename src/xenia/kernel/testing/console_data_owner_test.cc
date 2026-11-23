/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#include "xenia/kernel/console_data_owner.h"
#include "xenia/kernel/console_signing_identity.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xecrypt_rsa.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace xe::kernel::test {
#if XE_PLATFORM_LINUX
namespace {
using filesystem::HostStorageRoot;
constexpr ConsoleSettingKey kRetail{3, 0xC};
struct ConsoleScratch {
  std::filesystem::path path;
  ConsoleScratch() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-console-owner-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~ConsoleScratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path Private() const {
    return path / ConsoleDataOwner::kDirectory;
  }
  std::shared_ptr<HostStorageRoot> Root() {
    std::error_code error;
    auto root = HostStorageRoot::Open(path, false, error, 0);
    REQUIRE(root);
    REQUIRE_FALSE(error);
    return root;
  }
};
std::vector<uint8_t> Read(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  file.write(bytes.data(), bytes.size());
  file.close();
  REQUIRE_FALSE(file.fail());
}
} // namespace
TEST_CASE("Console owner persists settings through VFS clear and reopen",
          "[console_owner][persistence]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(owner->error());
  REQUIRE(owner->private_root());
  REQUIRE(owner->settings());
  const auto update = owner->settings()->WriteU32(kRetail, 0x80123456);
  REQUIRE_FALSE(update.error);
  REQUIRE(update.published);
  REQUIRE(update.durable);
  const auto before = Read(scratch.Private() / "xconfig.bin");
  REQUIRE(std::filesystem::create_directory(scratch.path / "hdd"));
  vfs::VirtualFileSystem vfs;
  for (int title = 0; title < 2; ++title) {
    auto device = std::make_unique<vfs::HostPathDevice>(
        "hdd:", scratch.path / "hdd", false);
    REQUIRE(device->Initialize());
    REQUIRE(vfs.RegisterDevice(std::move(device)));
    vfs.Clear();
    uint32_t flags = 0;
    REQUIRE_FALSE(owner->settings()->ReadU32(kRetail, 0, flags));
    REQUIRE(flags == 0x80123456);
    std::error_code error;
    REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
        std::vector<filesystem::GuestHostBacking>{{scratch.path, true}},
        error));
    REQUIRE(error == std::errc::permission_denied);
  }
  owner.reset();
  owner = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(owner->error());
  uint32_t flags = 0;
  REQUIRE_FALSE(owner->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0x80123456);
  REQUIRE(Read(scratch.Private() / "xconfig.bin") == before);
}
TEST_CASE("Unknown or interrupted console bootstrap is preserved and rejected",
          "[console_owner][recovery]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  REQUIRE(std::filesystem::create_directory(scratch.Private()));
  SECTION("empty preexisting directory is not adopted") {}
  SECTION("foreign bytes are not replaced") {
    Write(scratch.Private() / ConsoleDataOwner::kMarker, "unknown owner");
    Write(scratch.Private() / "xconfig.bin", "preserve me");
  }
  const bool had_marker =
      std::filesystem::exists(scratch.Private() / ConsoleDataOwner::kMarker);
  const auto marker = had_marker
                          ? Read(scratch.Private() / ConsoleDataOwner::kMarker)
                          : std::vector<uint8_t>{};
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error());
  REQUIRE_FALSE(owner->settings());
  REQUIRE_FALSE(owner->private_root());
  REQUIRE(std::filesystem::exists(scratch.Private() /
                                  ConsoleDataOwner::kMarker) == had_marker);
  if (had_marker) {
    REQUIRE(Read(scratch.Private() / ConsoleDataOwner::kMarker) == marker);
    REQUIRE((Read(scratch.Private() / "xconfig.bin") ==
             std::vector<uint8_t>{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                                  'm', 'e'}));
  }
  REQUIRE_FALSE(std::filesystem::exists(scratch.Private() / "xconfig.lock"));
}
TEST_CASE("Console owner rejects linked directory without altering target",
          "[console_owner][confinement]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  REQUIRE(std::filesystem::create_directory(scratch.path / "other"));
  Write(scratch.path / "other" / "keep", "host original");
  std::filesystem::create_directory_symlink(scratch.path / "other",
                                            scratch.Private());
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error());
  REQUIRE_FALSE(owner->settings());
  REQUIRE(std::filesystem::is_symlink(scratch.Private()));
  REQUIRE(
      std::distance(std::filesystem::directory_iterator(scratch.path / "other"),
                    std::filesystem::directory_iterator()) == 1);
  REQUIRE((Read(scratch.path / "other" / "keep") ==
           std::vector<uint8_t>{'h', 'o', 's', 't', ' ', 'o', 'r', 'i', 'g',
                                'i', 'n', 'a', 'l'}));
}
TEST_CASE("Corrupt console store remains protected without empty replacement",
          "[console_owner][recovery]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  {
    auto owner = ConsoleDataOwner::Open(root);
    REQUIRE_FALSE(owner->error());
    REQUIRE_FALSE(owner->settings()->WriteU32(kRetail, 7).error);
  }
  Write(scratch.Private() / "xconfig.bin", "damaged database");
  const auto before = Read(scratch.Private() / "xconfig.bin");
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error() == std::errc::bad_message);
  REQUIRE_FALSE(owner->settings());
  REQUIRE(owner->private_root());
  std::error_code error;
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<filesystem::GuestHostBacking>{{scratch.path, true}}, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE(Read(scratch.Private() / "xconfig.bin") == before);
}
TEST_CASE("Console owner cannot follow a replaced directory generation",
          "[console_owner][lifetime]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(owner->error());
  REQUIRE_FALSE(owner->settings()->WriteU32(kRetail, 42).error);
  const auto bytes = Read(scratch.Private() / "xconfig.bin");
  std::filesystem::rename(scratch.Private(), scratch.path / "retained");
  REQUIRE(std::filesystem::create_directory(scratch.Private()));
  uint32_t flags = 0xDEADBEEF;
  REQUIRE(owner->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0xDEADBEEF);
  auto next = ConsoleDataOwner::Open(root);
  REQUIRE(next->error());
  REQUIRE_FALSE(next->settings());
  REQUIRE(std::filesystem::is_empty(scratch.Private()));
  REQUIRE(Read(scratch.path / "retained" / "xconfig.bin") == bytes);
}
TEST_CASE("Console bootstrap obeys space admission and preserves parent errors",
          "[console_owner][admission]") {
  ConsoleScratch scratch;
  std::error_code error;
  auto space = filesystem::HostStorageSpace::ForPath(
      scratch.path, filesystem::kHostStorageReserve, error,
      [](const auto &, std::error_code &ec) {
        ec.clear();
        return std::filesystem::space_info{40ull << 30, 20ull << 30,
                                           20ull << 30};
      });
  REQUIRE(space);
  auto root = HostStorageRoot::Open(scratch.path, false, error,
                                    filesystem::kHostStorageReserve, space);
  REQUIRE(root);
  auto owner = ConsoleDataOwner::Open(root);
  REQUIRE(owner->error() == std::errc::no_space_on_device);
  REQUIRE_FALSE(owner->settings());
  REQUIRE_FALSE(std::filesystem::exists(scratch.Private()));
  const auto original = std::make_error_code(std::errc::read_only_file_system);
  owner = ConsoleDataOwner::Open({}, original);
  REQUIRE(owner->error() == original);
  REQUIRE_FALSE(owner->private_root());
}
TEST_CASE("Two console owners use the same durable authority",
          "[console_owner][persistence]") {
  ConsoleScratch scratch;
  auto root = scratch.Root();
  auto first = ConsoleDataOwner::Open(root);
  auto second = ConsoleDataOwner::Open(root);
  REQUIRE_FALSE(first->error());
  REQUIRE_FALSE(second->error());
  REQUIRE_FALSE(first->settings()->WriteU32(kRetail, 0xABCDEF01).error);
  uint32_t flags = 0;
  REQUIRE_FALSE(second->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0xABCDEF01);
  const auto update = second->settings()->ModifyU32(kRetail, 0, ~0xFFu, 0x57);
  REQUIRE_FALSE(update.error);
  REQUIRE(update.durable);
  REQUIRE_FALSE(first->settings()->ReadU32(kRetail, 0, flags));
  REQUIRE(flags == 0xABCDEF57);
}
TEST_CASE("Console signing identity persists and verifies real digest signatures",
          "[console_owner][signing][persistence]") {
 ConsoleScratch scratch;auto root=scratch.Root();auto owner=ConsoleDataOwner::Open(root);
 REQUIRE_FALSE(owner->error());REQUIRE_FALSE(owner->signing_error());
 auto identity=owner->signing_identity();REQUIRE(identity);
 const auto certificate=identity->certificate();const auto id=identity->console_id();
 CHECK(std::equal(id.begin(),id.end(),certificate.begin()+2));
 std::array<uint8_t,20> hash{};hash[0]=0x83;hash[19]=0xE1;
 ConsoleSigningIdentity::Signature signature{};
 REQUIRE(identity->Sign(hash,signature));
 REQUIRE(std::equal(certificate.begin(),certificate.end(),signature.begin()));
 uint32_t difference=0xA5A5A5A5;REQUIRE(identity->Verify(hash,signature,difference));CHECK(difference==0);
 // Independently exercise the existing real public RSA verifier on the
 // certificate key and actual full byte-reversed signature output.
 std::array<uint8_t,144> public_key{};public_key[3]=16;
 std::copy_n(certificate.begin()+0x24,4,public_key.begin()+4);
 std::copy_n(certificate.begin()+0x28,128,public_key.begin()+16);
 CHECK(XeCryptVerifyRsaSha1Pkcs1(public_key,hash,std::span(signature).subspan(0x1A8)));
 auto tampered=signature;tampered.back()^=1;
 CHECK_FALSE(identity->Verify(hash,tampered,difference));
 auto wrong_hash=hash;wrong_hash[3]^=1;
 CHECK_FALSE(identity->Verify(wrong_hash,signature,difference));
 tampered=signature;tampered[0x12]^=1;difference=0;
 CHECK_FALSE(identity->Verify(hash,tampered,difference));CHECK(difference!=0);
 const auto before=Read(scratch.Private()/ConsoleSigningIdentity::kIdentityFile);
 const auto formatted=identity->console_id_text();
 uint64_t decimal=0;for(size_t i=0;i<11;++i){REQUIRE(formatted[i]>='0');REQUIRE(formatted[i]<='9');decimal=decimal*10+formatted[i]-'0';}
 uint64_t raw=0;for(auto byte:id)raw=raw*256+byte;
 CHECK(decimal==raw>>4);CHECK(formatted[11]==uint8_t("0123456789abcdef"[raw&15]));
 identity.reset();owner.reset();owner=ConsoleDataOwner::Open(root);
 REQUIRE_FALSE(owner->signing_error());REQUIRE(owner->signing_identity());
 CHECK(owner->signing_identity()->certificate()==certificate);
 CHECK(owner->signing_identity()->Verify(hash,signature,difference));
 // Boolean comparison prevents Catch from dumping any private key on failure.
 CHECK(bool(Read(scratch.Private()/ConsoleSigningIdentity::kIdentityFile)==before));
 auto second=ConsoleDataOwner::Open(root);REQUIRE_FALSE(second->signing_error());
 REQUIRE(second->signing_identity());CHECK(second->signing_identity()->certificate()==certificate);
 REQUIRE(second->signing_identity()->Sign(hash,tampered));
 CHECK(owner->signing_identity()->Verify(hash,tampered,difference));
 ConsoleScratch foreign;auto foreign_owner=ConsoleDataOwner::Open(foreign.Root());
 REQUIRE_FALSE(foreign_owner->signing_error());REQUIRE(foreign_owner->signing_identity());
 CHECK_FALSE(foreign_owner->signing_identity()->Verify(hash,signature,difference));CHECK(difference!=0);
 std::error_code error;CHECK_FALSE(HostStorageRoot::AdmitGuestMount(
     std::vector<filesystem::GuestHostBacking>{{scratch.Private(),true}},error));
 CHECK(error==std::errc::permission_denied);
}
TEST_CASE("Console signing bootstrap resumes exact stages and preserves damaged state",
          "[console_owner][signing][recovery]") {
 ConsoleScratch scratch;auto root=scratch.Root();auto owner=ConsoleDataOwner::Open(root);
 REQUIRE_FALSE(owner->signing_error());REQUIRE(owner->signing_identity());
 const auto certificate=owner->signing_identity()->certificate();
 auto protected_root=owner->private_root();REQUIRE(protected_root);owner.reset();
 const auto write_stage=[&](std::span<const uint8_t> bytes){
  const auto written=protected_root->WriteNewFile("console-signing.stage.v1",bytes,true);
  REQUIRE_FALSE(written.error);REQUIRE(written.bytes==bytes.size());
  REQUIRE_FALSE(protected_root->FlushDirectory());
 };
 const auto write_partial_stage=[&](std::string_view text){
  write_stage({reinterpret_cast<const uint8_t*>(text.data()),text.size()});
 };
 const auto target=scratch.Private()/ConsoleSigningIdentity::kIdentityFile;
 const auto stage=scratch.Private()/"console-signing.stage.v1";
 SECTION("fully written prepublication stage resumes the same identity"){
  std::filesystem::rename(target,stage);const auto staged=Read(stage);
  owner=ConsoleDataOwner::Open(root);REQUIRE_FALSE(owner->error());REQUIRE_FALSE(owner->signing_error());REQUIRE(owner->signing_identity());
  CHECK(owner->signing_identity()->certificate()==certificate);
  CHECK_FALSE(std::filesystem::exists(stage));CHECK(bool(Read(target)==staged));
 }
 SECTION("identical complete stage may remain beside authoritative canonical"){
  const auto canonical=Read(target);
  write_stage(canonical);
  owner=ConsoleDataOwner::Open(root);REQUIRE_FALSE(owner->error());REQUIRE_FALSE(owner->signing_error());REQUIRE(owner->signing_identity());
  CHECK(owner->signing_identity()->certificate()==certificate);
  CHECK(bool(Read(target)==canonical));CHECK(bool(Read(stage)==canonical));
 }
 SECTION("partial stage beside valid canonical is refused and both preserved"){
  const auto canonical=Read(target);write_partial_stage("incomplete conflicting stage");const auto partial=Read(stage);
  owner=ConsoleDataOwner::Open(root);REQUIRE_FALSE(owner->error());REQUIRE(owner->signing_error());CHECK_FALSE(owner->signing_identity());
  CHECK(bool(Read(target)==canonical));CHECK(bool(Read(stage)==partial));
 }
 SECTION("different valid stage beside canonical is refused and neither identity changes"){
  ConsoleScratch other;auto foreign=ConsoleDataOwner::Open(other.Root());REQUIRE_FALSE(foreign->signing_error());
  const auto different=Read(other.Private()/ConsoleSigningIdentity::kIdentityFile);const auto canonical=Read(target);
  write_stage(different);
  owner=ConsoleDataOwner::Open(root);REQUIRE_FALSE(owner->error());REQUIRE(owner->signing_error());CHECK_FALSE(owner->signing_identity());
  CHECK(bool(Read(target)==canonical));CHECK(bool(Read(stage)==different));
 }
 SECTION("corrupt canonical is not replaced by a new key"){
  Write(target,"partial original signing state");const auto damaged=Read(target);
  owner=ConsoleDataOwner::Open(root);REQUIRE_FALSE(owner->error());REQUIRE(owner->signing_error());CHECK_FALSE(owner->signing_identity());
  REQUIRE(owner->settings());CHECK(bool(Read(target)==damaged));
  CHECK_FALSE(std::filesystem::exists(stage));
 }
 SECTION("interrupted stage remains preserved and cannot generate another identity"){
  std::filesystem::remove(target);write_partial_stage("interrupted stage");const auto damaged=Read(stage);
  owner=ConsoleDataOwner::Open(root);REQUIRE_FALSE(owner->error());REQUIRE(owner->signing_error());CHECK_FALSE(owner->signing_identity());
  CHECK_FALSE(std::filesystem::exists(target));CHECK(bool(Read(stage)==damaged));
 }
 SECTION("foreign hard-linked identity is never adopted"){
  std::filesystem::create_hard_link(target,scratch.path/"foreign-link");const auto before=Read(target);
  owner=ConsoleDataOwner::Open(root);CHECK(owner->error());CHECK_FALSE(owner->signing_identity());
  CHECK(bool(Read(target)==before));
 }
}
TEST_CASE("Console signing guest boundaries write the real full envelope and reject invalid IO",
          "[console_owner][signing][abi]") {
 ConsoleScratch scratch;auto owner=ConsoleDataOwner::Open(scratch.Root());
 REQUIRE_FALSE(owner->signing_error());REQUIRE(owner->signing_identity());const auto& identity=*owner->signing_identity();
 Memory memory;REQUIRE(memory.Initialize());constexpr uint32_t base=0x90080000;
 REQUIRE(memory.LookupHeap(base)->AllocFixed(base,0x2000,0x1000,
     kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
 std::memset(memory.TranslateVirtual(base),0xA5,0x2000);
 constexpr uint32_t hash=base+0x100,output=base+0xDF0,difference=base+0x80;
 std::array<uint8_t,20> digest{};digest[0]=0x37;digest.back()=0xE9;
 std::memcpy(memory.TranslateVirtual(hash),digest.data(),digest.size());
 REQUIRE(ConsoleSigningSignGuest(memory,identity,hash,output));
 CHECK(memory.TranslateVirtual(output)[0x228]==0xA5);
 CHECK(memory.TranslateVirtual(output)[-1]==0xA5);
 CHECK(std::memcmp(memory.TranslateVirtual(output),identity.certificate().data(),0x1A8)==0);
 REQUIRE(ConsoleSigningVerifyGuest(memory,identity,hash,output,difference));
 CHECK(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(difference))==0);
 memory.TranslateVirtual(hash)[3]^=1;CHECK_FALSE(ConsoleSigningVerifyGuest(memory,identity,hash,output,difference));
 memory.TranslateVirtual(hash)[3]^=1;
 // Public GetConsoleCertificate copies exactly1A8 bytes and no signature tail.
 const uint32_t certificate=base+0x300;REQUIRE(ConsoleSigningCertificateGuest(memory,identity,certificate)==X_STATUS_SUCCESS);
 CHECK(memory.TranslateVirtual(certificate)[0x1A8]==0xA5);
 REQUIRE(ConsoleSigningIdGuest(memory,identity,base+0x40,base+0x40)==X_STATUS_SUCCESS);
 const auto text=identity.console_id_text();CHECK(std::memcmp(memory.TranslateVirtual(base+0x40),text.data(),text.size())==0);
 const std::vector<uint8_t> unchanged(memory.TranslateVirtual(base),memory.TranslateVirtual(base)+0x2000);
 CHECK_FALSE(ConsoleSigningSignGuest(memory,identity,0,output));
 CHECK_FALSE(ConsoleSigningSignGuest(memory,identity,hash,base+0x1F00));
 {auto global=global_critical_region::AcquireDirect();CHECK_FALSE(ConsoleSigningSignGuest(memory,identity,hash,output));}
 CHECK(bool(std::equal(unchanged.begin(),unchanged.end(),memory.TranslateVirtual(base))));
 cpu::ExportResolver resolver;xboxkrnl::RegisterCryptExports(&resolver,nullptr);
 xboxkrnl::XboxkrnlModule::RegisterExportTable(&resolver);
 for(uint32_t ordinal:{0x1Fu,0x246u,0x256u,0x257u}){
  const auto* entry=resolver.GetExportByOrdinal("xboxkrnl.exe",ordinal);REQUIRE(entry);REQUIRE(entry->is_implemented());
  REQUIRE(entry->function_data.trampoline);CHECK_FALSE(entry->tags&cpu::ExportTag::kStub);
  PPCContext context{};context.virtual_membase=memory.virtual_membase();context.r[3]=hash;context.r[4]=output;context.r[5]=difference;
  entry->function_data.trampoline(&context);
  CHECK(uint32_t(context.r[3])==((ordinal==0x256 || ordinal==0x257)?0u:X_STATUS_UNSUCCESSFUL));
  CHECK(bool(std::equal(unchanged.begin(),unchanged.end(),memory.TranslateVirtual(base))));
 }
}

#else
TEST_CASE("Console owner records unsupported protection without creating data",
          "[console_owner]") {
  auto owner = ConsoleDataOwner::Open({});
  REQUIRE(owner->error() == std::errc::operation_not_supported);
  REQUIRE_FALSE(owner->private_root());
  REQUIRE_FALSE(owner->settings());
  WARN("UNVERIFIED: console authority mount protection on this platform");
}
#endif
} // namespace xe::kernel::test
