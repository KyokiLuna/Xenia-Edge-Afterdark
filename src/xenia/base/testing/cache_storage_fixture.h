/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_TESTING_CACHE_STORAGE_FIXTURE_H_
#define XENIA_BASE_TESTING_CACHE_STORAGE_FIXTURE_H_
#include "xenia/base/cache_storage_owner.h"
#include "xenia/base/testing/snapshot_fixture.h"
#include <fstream>
namespace xe::filesystem {
// Denial/cut control and actual native observations only. No permit, success,
// format epoch, source UUID or GuestView is synthesized by these helpers.
class CacheStorageOwnerTestAccess final {
 public:
  static CacheProductIdentity Product(uint32_t id,uint64_t generation=1,
      CacheProductIdentity::Kind kind=CacheProductIdentity::Kind::kXbox360){return {kind,id,generation};}
  static std::shared_ptr<CacheStorageOwner> StopOpen(CacheProductIdentity product,
      CacheStorageOwner::Options options,unsigned boundary,std::error_code& error){
    return CacheStorageOwner::OpenInternal(product,std::move(options),boundary,error);
  }
  static CacheStorageOwner::Receipt StopFormat(CacheFormatHandoff& handoff,unsigned boundary){
    return CacheStorageOwner::FinishFormat(handoff,false,boundary);
  }
  static auto NativeSource(const CacheStorageOwner& owner,CacheStorageOwner::Role role){return owner.NativeSource(role);}
  static auto Identity(const CacheStorageOwner& owner,CacheStorageOwner::Role role){return owner.CaptureIdentity(role);}
  static auto RegistrationGuard(const std::shared_ptr<CacheStorageOwner>& owner,std::error_code& error){
    constexpr auto role=CacheStorageOwner::Role::kLegacy;
    auto root=owner->CaptureRoot(role,error);if(error)return std::unique_ptr<CacheStorageOwner::RegistrationAdmission>{};
    auto identity=owner->CaptureIdentity(role);
    return owner->TryAdmitRegistration(role,root,identity,1);
  }
};
#if XE_PLATFORM_LINUX
namespace test {
struct CacheFixture {
  SnapshotScratch scratch;
  std::shared_ptr<HostStorageRoot> root;
  std::array<std::shared_ptr<HostStorageRoot>,3> parents;
  std::shared_ptr<HostStorageRoot> private_base;
  CacheFixture(){
    root=scratch.Root();std::error_code error;
    constexpr std::array<const char*,3> names{"cache","cache0","cache1"};
    for(size_t i=0;i<3;++i){REQUIRE_FALSE(root->Create(names[i],true));
      parents[i]=root->Subroot(names[i],false,error);REQUIRE(parents[i]);REQUIRE_FALSE(error);
      const std::array<uint8_t,4> original{uint8_t(i),0x55,0,0xAA};
      REQUIRE_FALSE(parents[i]->WriteNewFile("original-user-cache.bin",original).error);}
    REQUIRE_FALSE(root->Create("cache-private",true));private_base=root->Subroot("cache-private",false,error);
    REQUIRE(private_base);REQUIRE_FALSE(error);
  }
  CacheStorageOwner::Options Options(uint32_t id){
    const auto product=CacheStorageOwnerTestAccess::Product(id);std::error_code error;
    auto private_parent=private_base->Subroot(product.key(),false,error);
    if(!private_parent && error==std::errc::no_such_file_or_directory){
      REQUIRE_FALSE(private_base->Create(product.key(),true));private_parent=private_base->Subroot(product.key(),false,error);}
    REQUIRE(private_parent);REQUIRE_FALSE(error);return {parents,private_parent};
  }
  auto Open(uint32_t id,std::error_code& error,uint64_t generation=1){
    return CacheStorageOwner::Open(CacheStorageOwnerTestAccess::Product(id,generation),Options(id),error);
  }
  static std::vector<uint8_t> NativeBytes(const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary);REQUIRE(input.is_open());return {std::istreambuf_iterator<char>(input),{}};
  }
  std::filesystem::path Source(uint32_t id,size_t role) const {
    return parents[role]->path()/CacheStorageOwnerTestAccess::Product(id).key();
  }
  void Originals() const {
    for(size_t i=0;i<3;++i)REQUIRE(NativeBytes(parents[i]->path()/"original-user-cache.bin")==
        std::vector<uint8_t>{uint8_t(i),0x55,0,0xAA});
  }
};
} // namespace test
#endif
} // namespace xe::filesystem
#endif
