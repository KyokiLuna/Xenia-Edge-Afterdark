/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#include "xenia/base/byte_order.h"
#include "xenia/kernel/console_data_owner.h"
#include "xenia/kernel/console_signing_identity.h"
#include "xenia/kernel/xam/folder_content_header.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xecrypt_rsa.h"
#include "xenia/kernel/xboxkrnl/xecrypt_sha.h"
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
 // Retained CON readers need only the already-issued public certificate.
 // A signature's own certificate must never become its trust anchor.
 uint32_t public_difference=0x12345678;
 CHECK(XeCryptVerifyConsoleSignature(certificate,hash,signature,public_difference));
 CHECK(public_difference==0);
 auto bad_signature=signature;bad_signature.back()^=1;
 CHECK_FALSE(XeCryptVerifyConsoleSignature(certificate,hash,bad_signature,public_difference));
 CHECK_FALSE(XeCryptVerifyConsoleSignature(certificate,wrong_hash,signature,public_difference));
 CHECK_FALSE(XeCryptVerifyConsoleSignature(foreign_owner->signing_identity()->certificate(),
                                         hash,signature,public_difference));
 CHECK(public_difference!=0);
 auto bad_certificate=signature;bad_certificate[0x28]^=1;
 CHECK_FALSE(XeCryptVerifyConsoleSignature(certificate,hash,bad_certificate,public_difference));
 CHECK(public_difference!=0);
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

TEST_CASE("Folder CON codec signs exact native hash spans and retains padding and opaque sidecar tail",
          "[console_owner][signing][folder_con]") {
  namespace con = xam::folder_content_detail;
  ConsoleScratch scratch; auto owner = ConsoleDataOwner::Open(scratch.Root());
  REQUIRE_FALSE(owner->error()); REQUIRE_FALSE(owner->signing_error());
  REQUIRE(owner->signing_identity()); const auto& signer = *owner->signing_identity();
  vfs::XContentContainerHeader header{};
  constexpr uint32_t extra = 37, aligned = 0xA000, tail = 11;
  header.content_header.magic = vfs::XContentPackageType::kCon;
  header.content_header.header_size = uint32_t(sizeof(header) + extra);
  header.content_header.licenses[3].licensee_id = 0x0102030405060708ull;
  header.content_metadata.metadata_version = 2;
  header.content_metadata.content_type = XContentType::kProfile;
  header.content_metadata.profile_id = 0x0009000012345678ull;
  header.content_metadata.execution_info.title_id = 0xFFFE07D1;
  header.content_metadata.content_size = 0x4321;
  header.content_metadata.volume_type = vfs::XContentVolumeType::kStfs;
  header.content_metadata.volume_descriptor.stfs.descriptor_length = 36;
  const con::PackageIdentity expected{header.content_metadata.profile_id,
      header.content_metadata.execution_info.title_id, XContentType::kProfile};
  std::vector<uint8_t> bytes(aligned + tail);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::fill(bytes.begin() + sizeof(header), bytes.begin() + sizeof(header) + extra, 0x7B);
  // Actual retained old padding is deliberately nonzero. The unknown sidecar
  // tail starts AFTER R, not at H; only a genuine create producer may zero pad.
  std::fill(bytes.begin() + sizeof(header) + extra, bytes.begin() + aligned, 0xD3);
  std::fill(bytes.begin() + aligned, bytes.end(), 0xA9);
  const auto original = bytes;
  REQUIRE_FALSE(con::SignNativeHeader(bytes, signer));
  REQUIRE(std::equal(original.begin() + 0x344, original.end(), bytes.begin() + 0x344));
  REQUIRE(std::equal(original.begin() + 0x22C, original.begin() + 0x32C, bytes.begin() + 0x22C));
  std::error_code error;
  const auto facts = con::ValidateNativeHeader(bytes, expected, 0x8000,
      signer.certificate(), error);
  REQUIRE(facts); REQUIRE_FALSE(error); CHECK(facts->signature_class == 1);
  CHECK(facts->header_size == sizeof(header) + extra); CHECK(facts->data_offset == 0xA000);
  CHECK(facts->raw_extent == 0x12000); CHECK(facts->context_bytes == 0xA192);
  CHECK(facts->logical_geometry == 0x100000); CHECK(facts->content_size == 0x4321);
  // Independently sign a legacy H-only digest while retaining a long opaque
  // sidecar. Valid signature shape cannot make those unknown bytes native pad.
  auto wrong_range = bytes;
  std::array<uint8_t, 20> legacy_body{}, legacy_signed{};
  REQUIRE(mbedtls_sha1(wrong_range.data() + 0x344,
      sizeof(header) + extra - 0x344, legacy_body.data()) == 0);
  std::copy(legacy_body.begin(), legacy_body.end(), wrong_range.begin() + 0x32C);
  REQUIRE(mbedtls_sha1(wrong_range.data() + 0x22C, 0x118, legacy_signed.data()) == 0);
  ConsoleSigningIdentity::Signature legacy_signature{};
  REQUIRE(signer.Sign(legacy_signed, legacy_signature));
  std::copy(legacy_signature.begin(), legacy_signature.end(), wrong_range.begin() + 4);
  CHECK_FALSE(con::ValidateNativeHeader(wrong_range, expected, 0x8000,
      signer.certificate(), error)); CHECK(error == std::errc::bad_message);
  con::NativeFolderHeaderFormat format(signer.certificate());
  const std::filesystem::path package = "0009000012345678/FFFE07D1/00010000/0009000012345678";
  CHECK_FALSE(format.ValidateCurrent(package, bytes));
  auto native_thumb = bytes;
  auto* changed_header = reinterpret_cast<vfs::XContentContainerHeader*>(native_thumb.data());
  changed_header->content_metadata.thumbnail_size = 2;
  changed_header->content_metadata.thumbnail[0] = 0x51;
  changed_header->content_metadata.thumbnail[1] = 0x52;
  REQUIRE_FALSE(con::SignNativeHeader(native_thumb, signer));
  CHECK_FALSE(format.ValidateNativeChange(package, bytes, native_thumb));
  auto unowned = native_thumb;
  reinterpret_cast<vfs::XContentContainerHeader*>(unowned.data())->content_metadata.description_raw.uint[0][0] = u'Q';
  REQUIRE_FALSE(con::SignNativeHeader(unowned, signer));
  CHECK_FALSE(format.ValidateCurrent(package, unowned)); // valid crypto is not writer permission
  CHECK(format.ValidateNativeChange(package, bytes, unowned) == std::errc::operation_not_supported);
  unowned = native_thumb;
  reinterpret_cast<vfs::XContentContainerHeader*>(unowned.data())->content_metadata.thumbnail[10] ^= 1;
  REQUIRE_FALSE(con::SignNativeHeader(unowned, signer));
  CHECK(format.ValidateNativeChange(package, bytes, unowned) == std::errc::operation_not_supported);
  unowned = native_thumb; unowned.back() ^= 1; // unknown sidecar tail must be exact
  CHECK(format.ValidateNativeChange(package, bytes, unowned) == std::errc::operation_not_supported);
  auto tampered = bytes; tampered[sizeof(header) + extra + 3] ^= 1;
  CHECK_FALSE(con::ValidateNativeHeader(tampered, expected, 0x8000, signer.certificate(), error));
  CHECK(error == std::errc::bad_message); // alignment bytes are authenticated
  tampered = bytes; tampered[sizeof(header) + 3] ^= 1;
  CHECK_FALSE(con::ValidateNativeHeader(tampered, expected, 0x8000, signer.certificate(), error));
  CHECK(error == std::errc::bad_message);
  tampered = bytes; tampered[4 + 0x1A8 + 0x7F] ^= 1;
  CHECK_FALSE(con::ValidateNativeHeader(tampered, expected, 0x8000, signer.certificate(), error));
  CHECK(error == std::errc::bad_message);
  const auto wrong_owner = con::PackageIdentity{expected.xuid ^ 1, expected.title_id, expected.content_type};
  CHECK_FALSE(con::ValidateNativeHeader(bytes, wrong_owner, 0x8000, signer.certificate(), error));
  CHECK(error == std::errc::permission_denied);
  CHECK_FALSE(con::ValidateNativeHeader(bytes, expected, 0x4320, signer.certificate(), error));
  CHECK(error == std::errc::operation_not_supported);
  CHECK_FALSE(con::ValidateNativeHeader(bytes, expected, UINT64_MAX, signer.certificate(), error));
  CHECK(error == std::errc::operation_not_supported);
  // Native digest excludes the opaque sidecar tail; complete physical before
  // equality/journal/CAS remains the separate CM storage authority.
  tampered = bytes; tampered.back() ^= 1;
  CHECK(con::ValidateNativeHeader(tampered, expected, 0x8000, signer.certificate(), error));
  CHECK_FALSE(error);
}
TEST_CASE("Folder CON codec refuses foreign certificate trust and never changes malformed input on signing failure",
          "[console_owner][signing][folder_con]") {
  namespace con = xam::folder_content_detail;
  ConsoleScratch scratch, other;
  auto owner = ConsoleDataOwner::Open(scratch.Root());
  auto foreign = ConsoleDataOwner::Open(other.Root());
  REQUIRE_FALSE(owner->error()); REQUIRE_FALSE(owner->signing_error()); REQUIRE(owner->signing_identity());
  REQUIRE_FALSE(foreign->error()); REQUIRE_FALSE(foreign->signing_error()); REQUIRE(foreign->signing_identity());
  vfs::XContentContainerHeader header{};
  header.content_header.magic = vfs::XContentPackageType::kCon;
  header.content_header.header_size = sizeof(header);
  header.content_metadata.content_type = XContentType::kProfile;
  header.content_metadata.profile_id = 1;
  header.content_metadata.execution_info.title_id = 0xFFFE07D1;
  header.content_metadata.volume_descriptor.stfs.descriptor_length = 36;
  // Independent native 86DA0 fixture: Header then exactly zero [H,R).
  // This caller-formed test value is not a production CM create capability.
  std::vector<uint8_t> bytes(0xA000); std::memcpy(bytes.data(), &header, sizeof(header));
  REQUIRE_FALSE(con::SignNativeHeader(bytes, *owner->signing_identity()));
  const con::PackageIdentity expected{1, 0xFFFE07D1, XContentType::kProfile};
  std::error_code error;
  const auto own = con::ValidateNativeHeader(bytes, expected, 0,
      owner->signing_identity()->certificate(), error);
  REQUIRE(own); REQUIRE_FALSE(error);
  CHECK(own->raw_extent == 0xA000); CHECK(own->logical_geometry == 0x100000000ull);
  CHECK_FALSE(con::ValidateNativeHeader(bytes, expected, 0,
      foreign->signing_identity()->certificate(), error));
  CHECK(error == std::errc::operation_not_supported);
  const auto signed_bytes = bytes;
  bytes[0x340] = bytes[0x341] = bytes[0x342] = bytes[0x343] = 0;
  const auto malformed = bytes;
  CHECK(con::SignNativeHeader(bytes, *owner->signing_identity()) == std::errc::bad_message);
  CHECK(bytes == malformed);
  CHECK_FALSE(con::ValidateNativeHeader(malformed, expected, 0,
      owner->signing_identity()->certificate(), error)); CHECK(error == std::errc::bad_message);
  CHECK_FALSE(con::ValidateNativeHeader(signed_bytes, expected, 0,
      std::span<const uint8_t>{}, error)); CHECK(error == std::errc::invalid_argument);
}

TEST_CASE("Folder Header hash agrees with pinned native split buffers and rejects an H-only source",
          "[console_owner][folder_con][native_header_alignment]") {
  namespace con = xam::folder_content_detail;
  // 17559 C628: input1=ctx+0x37C,len=0x93D6, input2=ctx+0x98AC,
  // len=R-0x971A. Golden SHA1 was derived independently over raw [344,R),
  // not via SignNativeHeader/ValidateNativeHeader or their shared helper.
  constexpr uint32_t h = 0x973F, r = 0xA000;
  const std::array<uint8_t, 20> golden{
      0xE2, 0x6C, 0x49, 0x60, 0x34, 0x14, 0x80, 0x38, 0xD1, 0xF9, 0x6E, 0xD4, 0x3E, 0x2D, 0x09, 0x8C, 0x8A, 0x51, 0x43, 0x5B};
  std::vector<uint8_t> raw(r + 11, 0xA9);
  for (size_t i = 0x344; i < r; ++i) { raw[i] = uint8_t(i * 29 + 7); }
  xe::store_and_swap<uint32_t>(raw.data() + 0x340, h);
  std::vector<uint8_t> context(r + 0x192, 0xCE);
  std::copy_n(raw.begin(), 0x971A, context.begin() + 0x38);
  std::copy(raw.begin() + 0x971A, raw.begin() + r, context.begin() + 0x98AC);
  std::array<uint8_t, 20> native{}, actual{}, wrong{};
  xboxkrnl::XeCryptSha(context.data() + 0x37C, 0x93D6,
      context.data() + 0x98AC, r - 0x971A, nullptr, 0, native.data(), native.size());
  REQUIRE(native == golden);
  REQUIRE(con::HeaderBodyDigest(raw, actual)); CHECK(actual == golden);
  // Contiguous context includes control fields, not the native remainder.
  xboxkrnl::XeCryptSha(context.data() + 0x37C, r - 0x344,
      nullptr, 0, nullptr, 0, wrong.data(), wrong.size());
  CHECK(wrong != golden);
  // Unknown bytes after R never acquire raw/header identity.
  raw.back() ^= 1; REQUIRE(con::HeaderBodyDigest(raw, actual)); CHECK(actual == golden);
  raw[h + 1] ^= 1; REQUIRE(con::HeaderBodyDigest(raw, actual)); CHECK(actual != golden);
  raw.resize(h); CHECK_FALSE(con::HeaderBodyDigest(raw, actual));
  raw.resize(h + 11, 0xA9); CHECK_FALSE(con::HeaderBodyDigest(raw, actual));
  // Exact aligned H has no padding; the next byte requires a whole new page.
  raw.resize(r); xe::store_and_swap<uint32_t>(raw.data() + 0x340, r);
  CHECK(con::NativeHeaderAlignedSize(raw) == r);
  xe::store_and_swap<uint32_t>(raw.data() + 0x340, r + 1);
  CHECK(con::NativeHeaderAlignedSize(raw) == 0);
  raw.resize(0xB000); CHECK(con::NativeHeaderAlignedSize(raw) == 0xB000);
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
