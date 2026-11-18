/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/host_metadata_writer.h"
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
namespace {
struct MetadataScratch {
  std::filesystem::path path;
  MetadataScratch(){
    std::random_device random;
    for(unsigned i=0;i<16;++i){
      path=std::filesystem::temp_directory_path()/
          ("xenia-host-metadata-"+std::to_string(random())+"-"+std::to_string(random()));
      if(std::filesystem::create_directory(path))return;
    }
    FAIL("No exclusive owned metadata scratch directory");
  }
  ~MetadataScratch(){std::error_code error;std::filesystem::remove_all(path,error);}
};
std::vector<uint8_t> Read(const std::filesystem::path& path){
  std::ifstream file(path,std::ios::binary);
  REQUIRE(file);
  return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
std::shared_ptr<HostMetadataWriter> Open(const std::filesystem::path& path){
  std::error_code error;auto writer=HostMetadataWriter::Open(path,error);
  REQUIRE(writer);REQUIRE_FALSE(error);return writer;
}
}
TEST_CASE("Host metadata publishes real nested snapshots and same-byte barriers",
          "[host_metadata]") {
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  const std::array<uint8_t,3> old{0,255,7},next{5,4,3};
  auto first=writer->Write("title/release/game.toml",old);
  REQUIRE(first);REQUIRE(first.publication.published);
  REQUIRE(first.publication.current);
  REQUIRE(first.publication.current->root()->Observe().reserve_bytes>=kHostStorageReserve);
  const auto inode=first.publication.current->info().file_id;
  const auto same=writer->Write("title/release/game.toml",old);
  REQUIRE(same);REQUIRE(same.unchanged);REQUIRE_FALSE(same.publication.published);
  REQUIRE(same.publication.expected->info().file_id==inode);
  auto replaced=writer->Write("title/release/game.toml",next);
  REQUIRE(replaced);REQUIRE(replaced.publication.recovery_durable);
  REQUIRE(replaced.publication.current->info().file_id!=inode);
  REQUIRE(Read(writer->path()/SnapshotRecoveryOwner::kDirectory/replaced.publication.recovery)==
          std::vector<uint8_t>(old.begin(),old.end()));
  writer.reset();auto reopened=Open(scratch.path/"index");
  std::error_code error;auto token=reopened->Capture("title/release/game.toml",128,error);
  REQUIRE(token);REQUIRE_FALSE(error);
  REQUIRE(std::vector<uint8_t>(token->bytes().begin(),token->bytes().end())==std::vector<uint8_t>(next.begin(),next.end()));
}
TEST_CASE("Host metadata cancellation does not create new parent directories",
          "[host_metadata]") {
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  const std::array<uint8_t,1> bytes{7};std::atomic<bool> canceled{true};
  const auto stopped=writer->Write("new/release/icon.png",bytes,&canceled);
  REQUIRE(stopped.error==std::errc::operation_canceled);
  REQUIRE_FALSE(stopped.publication.stage_created);
  REQUIRE_FALSE(std::filesystem::exists(writer->path()/"new"));
  canceled=false;REQUIRE(writer->Write("new/release/icon.png",bytes,&canceled));
}
TEST_CASE("Host metadata retains actual failed CAS stages and exact old backups",
          "[host_metadata]") {
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  const std::array<uint8_t,3> old{1,2,3},next{7,8,9};
  REQUIRE(writer->Write("game.toml",old));
  std::error_code error;const auto expected=writer->Capture("game.toml",128,error);
  REQUIRE(expected);
  // A genuine foreign in-place writer races the immutable load token.
  {std::ofstream file(writer->path()/"game.toml",std::ios::binary);file<<"xyz";REQUIRE(file.good());}
  const auto failed=writer->Publish(expected,next);
  REQUIRE(failed.error==std::errc::state_not_recoverable);
  REQUIRE_FALSE(failed.publication.published);
  REQUIRE(failed.publication.stage_created);REQUIRE(failed.publication.recovery_durable);
  REQUIRE(Read(writer->path()/"game.toml")==std::vector<uint8_t>{'x','y','z'});
  REQUIRE(Read(writer->path()/SnapshotRecoveryOwner::kDirectory/failed.publication.recovery)==
          std::vector<uint8_t>(old.begin(),old.end()));
  REQUIRE(writer->retained_failures().size()==1);
  const auto stage=writer->path()/failed.publication.stage;
  REQUIRE(std::filesystem::exists(stage));
  writer.reset();auto reopened=Open(scratch.path/"index");
  REQUIRE(reopened->Write("game.toml",next));
  REQUIRE(std::filesystem::exists(stage)); // Never selected/deleted by prefix.
}
TEST_CASE("Host metadata rejects foreign tokens symlinks and escape targets",
          "[host_metadata]") {
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  auto foreign=Open(scratch.path/"foreign");const std::array<uint8_t,2> bytes{4,5};
  REQUIRE(foreign->Write("art.png",bytes));std::error_code error;
  const auto token=foreign->Capture("art.png",128,error);REQUIRE(token);
  REQUIRE(writer->Publish(token,bytes).error==std::errc::invalid_argument);
  std::filesystem::create_symlink(foreign->path()/"art.png",writer->path()/"icon.png",error);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(writer->Write("icon.png",bytes));
  REQUIRE(Read(foreign->path()/"art.png")==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  REQUIRE_FALSE(writer->Write("../foreign/art.png",bytes));
  REQUIRE_FALSE(writer->Write(foreign->path()/"art.png",bytes));
  REQUIRE_FALSE(writer->Write(std::filesystem::path(SnapshotRecoveryOwner::kDirectory)/"owner.v1",bytes));
  {std::ofstream file(writer->path()/".xenia-stage-user.tmp");file<<"unowned";}
  REQUIRE(writer->Write("other.png",bytes));
  REQUIRE(Read(writer->path()/".xenia-stage-user.tmp")==std::vector<uint8_t>{'u','n','o','w','n','e','d'});
  std::vector<uint8_t> large(HostMetadataWriter::kMaximumBytes+1,0x6D);
  {std::ofstream file(writer->path()/"unknown-large.bin",std::ios::binary);
   file.write(reinterpret_cast<const char*>(large.data()),large.size());REQUIRE(file.good());}
  auto raw=HostStorageRoot::Open(writer->path(),false,error);REQUIRE(raw);
  const auto oversized=raw->CaptureSnapshot("unknown-large.bin",large.size(),error);REQUIRE(oversized);
  const auto denied=writer->Publish(oversized,bytes);
  REQUIRE(denied.error==std::errc::file_too_large);
  REQUIRE_FALSE(denied.publication.stage_created);
  REQUIRE(Read(writer->path()/"unknown-large.bin")==large);
}
TEST_CASE("Host metadata handle conflicts preserve original until genuine close",
          "[host_metadata]") {
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  const std::array<uint8_t,1> old{3},next{8};REQUIRE(writer->Write("art.png",old));
  std::error_code error;const auto expected=writer->Capture("art.png",128,error);REQUIRE(expected);
  auto held=expected->root()->OpenFile("art.png",FileAccess::kGenericRead,error);REQUIRE(held);
  const auto blocked=writer->Publish(expected,next);
  REQUIRE(blocked.error==std::errc::device_or_resource_busy);
  REQUIRE_FALSE(blocked.publication.published);
  REQUIRE(Read(writer->path()/"art.png")==std::vector<uint8_t>{3});
  held.reset();REQUIRE(writer->Write("art.png",next));
  REQUIRE(std::filesystem::exists(writer->path()/blocked.publication.stage));
}
TEST_CASE("Host metadata explicit erase backs up exact bytes and never deletes missing fiction",
          "[host_metadata]") {
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  const std::array<uint8_t,2> old{6,9};REQUIRE(writer->Write("controlled.toml",old));
  std::error_code error;const auto token=writer->Capture("controlled.toml",128,error);REQUIRE(token);
  const auto erased=writer->Erase(token);REQUIRE(erased);
  REQUIRE(erased.publication.kind==SnapshotMutationKind::kDelete);
  REQUIRE(erased.publication.published);REQUIRE(erased.publication.recovery_durable);
  REQUIRE_FALSE(std::filesystem::exists(writer->path()/"controlled.toml"));
  REQUIRE(Read(writer->path()/SnapshotRecoveryOwner::kDirectory/erased.publication.recovery)==
          std::vector<uint8_t>(old.begin(),old.end()));
  const auto absent=writer->Capture("controlled.toml",128,error);REQUIRE(absent);
  const auto repeated=writer->Erase(absent);
  REQUIRE(repeated.error==std::errc::invalid_argument);
  REQUIRE_FALSE(repeated.publication.published);
}
TEST_CASE("Host metadata retains initialization attempts after later refused writes",
          "[host_metadata]") {
  MetadataScratch scratch;const auto parent=scratch.path/"blocked";
  REQUIRE(std::filesystem::create_directory(parent));
  struct Restore {
    std::filesystem::path path;
    ~Restore(){std::error_code error;std::filesystem::permissions(path,
        std::filesystem::perms::owner_all,std::filesystem::perm_options::add,error);}
  } restore{parent};
  std::filesystem::permissions(parent,std::filesystem::perms::owner_read|
      std::filesystem::perms::owner_exec,std::filesystem::perm_options::replace);
  std::error_code error;auto writer=HostMetadataWriter::Open(parent/"index",error);REQUIRE(writer);
  if(!error){WARN("OS privilege bypasses mode-bit mkdir refusal");return;}
  REQUIRE(error==std::errc::permission_denied);
  const auto initial=writer->retained_failures();REQUIRE(initial.size()==1);
  REQUIRE(initial[0].attempted_directories.size()==1);
  REQUIRE(initial[0].attempted_directories[0].filename()=="index");
  const std::array<uint8_t,1> bytes{4};
  REQUIRE(writer->Write("game.toml",bytes).error==error);
  REQUIRE(writer->Write("icon.png",bytes).error==error);
  const auto retained=writer->retained_failures();REQUIRE(retained.size()==1);
  REQUIRE(retained[0].attempted_directories==initial[0].attempted_directories);
  REQUIRE(retained[0].created_directories==initial[0].created_directories);
  REQUIRE_FALSE(std::filesystem::exists(parent/"index"));
}
TEST_CASE("Host metadata rejects embedded NUL before prefix target access",
          "[host_metadata]") {
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  const std::array<uint8_t,2> old{2,3},next{8,9};
  REQUIRE(writer->Write("game.toml",old));
  std::string leaf="game.toml";leaf.push_back('\0');leaf+="unrequested";
  const auto denied=writer->Write(std::filesystem::path(leaf),next);
  REQUIRE(denied.error==std::errc::invalid_argument);
  REQUIRE_FALSE(denied.publication.stage_created);
  REQUIRE(Read(writer->path()/"game.toml")==std::vector<uint8_t>(old.begin(),old.end()));
  std::error_code error;REQUIRE_FALSE(writer->Capture(std::filesystem::path(leaf),128,error));
  REQUIRE(error==std::errc::invalid_argument);
  std::string root=writer->path().string();root.push_back('\0');root+="unrequested";
  const auto invalid=HostMetadataWriter::Open(std::filesystem::path(root),error);REQUIRE(invalid);
  REQUIRE(error==std::errc::invalid_argument);
  REQUIRE(Read(writer->path()/"game.toml")==std::vector<uint8_t>(old.begin(),old.end()));
  REQUIRE_FALSE(std::filesystem::exists(writer->path()/"unrequested"));
}
TEST_CASE("Host metadata native filesystem floor must run alone in its process",
          "[.][host_metadata_floor]") {
  // No fake query/private authority and no disk filling. Raise the SAME live
  // native filesystem's reserve above actual available bytes. This hidden
  // case is run alone because the common reserve is monotonic per process.
  MetadataScratch scratch;auto writer=Open(scratch.path/"index");
  const std::array<uint8_t,1> old{2},next{9};REQUIRE(writer->Write("game.toml",old));
  std::error_code error;const auto available=std::filesystem::space(writer->path(),error).available;
  REQUIRE_FALSE(error);REQUIRE(available<std::numeric_limits<uint64_t>::max());
  const auto native=HostStorageRoot::Open(writer->path(),false,error,available+1);
  REQUIRE(native);REQUIRE_FALSE(error);REQUIRE(native->native_filesystem_authority());
  const auto denied=writer->Write("game.toml",next);
  REQUIRE(denied.error==std::errc::no_space_on_device);
  REQUIRE_FALSE(denied.publication.published);
  REQUIRE(Read(writer->path()/"game.toml")==std::vector<uint8_t>{2});
  // File+parent observation needs no allocation and remains idempotent.
  const auto same=writer->Write("game.toml",old);
  REQUIRE(same);REQUIRE(same.unchanged);
}
}  // namespace xe::filesystem::test
#endif
