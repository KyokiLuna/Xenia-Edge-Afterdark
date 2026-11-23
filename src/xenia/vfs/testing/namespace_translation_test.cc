/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <memory>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/managed_file_service.h"
#include "xenia/base/platform.h"
#include <string>
#include "third_party/catch/include/catch.hpp"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe::vfs::test {
TEST_CASE("Namespace translation selects longest whole-component alias",
          "[vfs][namespace]") {
  VirtualFileSystem fs;
  for (const std::string name : {"Outer", "Inner"}) {
    auto device = std::make_unique<NullDevice>(
        "\\Device\\" + name, std::initializer_list<std::string>{"\\file"});
    REQUIRE(device->Initialize());
    REQUIRE(fs.RegisterDevice(std::move(device)));
  }
  REQUIRE(fs.RegisterSymbolicLink("GAME:", "\\Device\\Outer"));
  REQUIRE(fs.RegisterSymbolicLink("game:\\nested", "\\Device\\Inner"));
  std::string output = "unchanged", target = "unchanged";
  REQUIRE(fs.TranslateSymbolicPath("gAmE:\\NeStEd\\file", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Inner\\file");
  REQUIRE(fs.FindSymbolicLink("GAME:\\nested\\file", target));
  CHECK(target == "\\Device\\Inner");
  auto* entry = fs.ResolvePath("GAME:\\nested\\file");
  REQUIRE(entry);
  CHECK(entry->device() == fs.GetDevice("\\Device\\Inner"));
  REQUIRE(fs.TranslateSymbolicPath("game:\\nestedLong\\file", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Outer\\nestedLong\\file");
  REQUIRE(fs.TranslateSymbolicPath("game:file", output) == X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Outer\\file");
  CHECK(fs.ResolvePath("game:file") ==
        fs.GetDevice("\\Device\\Outer")->ResolvePath("\\file"));
}
TEST_CASE("DOS aliases share title roots while SYSTEM roots remain distinct",
          "[vfs][namespace]") {
  VirtualFileSystem fs;
  std::string output = "old", target = "old";
  REQUIRE(fs.RegisterSymbolicLink("\\System??\\X:", "\\Device\\Disk"));
  REQUIRE(fs.RegisterSymbolicLink("\\??\\x:", "\\Device\\Other"));
  CHECK_FALSE(fs.RegisterSymbolicLink("\\DosDevices\\X:", "\\Device\\Collision"));
  REQUIRE(fs.TranslateSymbolicPath("\\DosDevices\\x:\\a\\..\\b", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Other\\b");
  REQUIRE(fs.TranslateSymbolicPath("\\System??\\x:\\a\\..\\b", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\Disk\\b");
  REQUIRE(fs.RegisterSymbolicLink("\\Alias", "\\Device\\Short"));
  CHECK_FALSE(fs.FindSymbolicLink("\\AliasLong\\file", target));
  CHECK(target == "old");
  REQUIRE(fs.TranslateSymbolicPath("\\Device\\NotMounted\\future", output) ==
          X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\NotMounted\\future");
  CHECK(fs.ResolvePath(output) == nullptr);
  REQUIRE(fs.UnregisterSymbolicLink("\\??\\X:"));
  CHECK_FALSE(fs.IsSymbolicLinkRegistered("x:"));
  CHECK(fs.IsSymbolicLinkRegistered("\\System??\\X:"));
}
TEST_CASE("Namespace loops growth and limits never publish a partial result",
          "[vfs][namespace]") {
  VirtualFileSystem fs;
  std::string output = "sentinel";
  REQUIRE(fs.RegisterSymbolicLink("A:", "b:"));
  REQUIRE(fs.RegisterSymbolicLink("B:", "a:"));
  CHECK(XFAILED(fs.TranslateSymbolicPath("a:\\file", output)));
  CHECK(output == "sentinel");
  REQUIRE(fs.RegisterSymbolicLink("grow:", "grow:\\more"));
  CHECK(XFAILED(fs.TranslateSymbolicPath("grow:\\file", output)));
  CHECK(output == "sentinel");
  REQUIRE(fs.RegisterSymbolicLink(
      "long:", std::string(VirtualFileSystem::kMaximumTranslatedPath, 'q')));
  CHECK(fs.TranslateSymbolicPath("long:\\x", output) ==
        X_STATUS_BUFFER_TOO_SMALL);
  CHECK(output == "sentinel");
  CHECK(XFAILED(fs.TranslateSymbolicPath(std::string(0x10000, 'q'), output)));
  CHECK(output == "sentinel");
  CHECK(XFAILED(fs.TranslateSymbolicPath(std::string("a\0b", 3), output)));
  CHECK(output == "sentinel");
  for (uint32_t i = 0; i < 33; ++i) {
    REQUIRE(fs.RegisterSymbolicLink("d" + std::to_string(i) + ":",
                                    "d" + std::to_string(i + 1) + ":"));
  }
  CHECK(XFAILED(fs.TranslateSymbolicPath("d0:\\file", output)));
  CHECK(output == "sentinel");
  fs.Clear();
  REQUIRE(fs.TranslateSymbolicPath("\\Device\\Disk", output) ==
          X_STATUS_SUCCESS);
}
TEST_CASE(
    "Malformed namespace bytes are rejected before checked UTF8 iteration",
    "[vfs][namespace][encoding]") {
  VirtualFileSystem fs;
  REQUIRE(fs.RegisterSymbolicLink("data:", "\\Device\\Disk"));
  for (const auto bad : {std::string("\xFF", 1), std::string("\xE2\x82", 2),
                         std::string("\xC0\xAF", 2)}) {
    std::string output = "sentinel", target = "sentinel";
    CHECK(fs.TranslateSymbolicPath(bad, output) ==
          X_STATUS_OBJECT_NAME_INVALID);
    CHECK(output == "sentinel");
    CHECK_FALSE(fs.RegisterSymbolicLink(bad, "\\Device\\Disk"));
    CHECK_FALSE(fs.RegisterSymbolicLink("bad:", bad));
    CHECK_FALSE(fs.FindSymbolicLink(bad, target));
    CHECK(target == "sentinel");
    CHECK_FALSE(fs.UnregisterSymbolicLink(bad));
    CHECK_FALSE(fs.IsSymbolicLinkRegistered(bad));
    CHECK(fs.ResolvePath(bad) == nullptr);
  }
}

TEST_CASE("Source-bound aliases cannot follow a replacement at the same device path",
          "[vfs][namespace][source_alias][lifetime]") {
  VirtualFileSystem fs;
  auto old = std::make_unique<NullDevice>(
      "\\Device\\Disk", std::initializer_list<std::string>{"\\file"});
  REQUIRE(old->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(old)));
  const auto original = fs.AcquireDevice("\\Device\\Disk");
  REQUIRE(original);
  std::shared_ptr<const SymbolicLinkToken> token;
  REQUIRE(fs.RegisterDeviceSymbolicLink("save:", "\\Device\\Disk", original,
                                       token) == X_STATUS_SUCCESS);
  REQUIRE(token);
  REQUIRE(fs.ResolvePath("save:\\file"));
  REQUIRE(fs.UnregisterDevice("\\Device\\Disk", original.get()));
  auto replacement = std::make_unique<NullDevice>(
      "\\Device\\Disk", std::initializer_list<std::string>{"\\file"});
  REQUIRE(replacement->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(replacement)));
  REQUIRE(fs.ResolvePath("\\Device\\Disk\\file"));
  std::string output = "unchanged", target = "unchanged";
  CHECK(XFAILED(fs.TranslateSymbolicPath("save:\\file", output)));
  CHECK(output == "unchanged");
  CHECK_FALSE(fs.FindSymbolicLink("save:", target));
  CHECK(target == "unchanged");
  CHECK(fs.ResolvePath("save:\\file") == nullptr);
  CHECK(fs.IsSymbolicLinkRegistered("save:")); // exact tombstone reserves name
  REQUIRE(fs.UnregisterSymbolicLink(*token));
  CHECK_FALSE(fs.IsSymbolicLinkRegistered("save:"));
}

TEST_CASE("Expected alias cleanup cannot erase a later explicitly published alias",
          "[vfs][namespace][source_alias][ownership]") {
  VirtualFileSystem fs;
  auto device = std::make_unique<NullDevice>(
      "\\Device\\Disk", std::initializer_list<std::string>{"\\file"});
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  const auto owner = fs.AcquireDevice("\\Device\\Disk");
  std::shared_ptr<const SymbolicLinkToken> first, second, refused;
  REQUIRE(fs.RegisterDeviceSymbolicLink("save:", "\\Device\\Disk", owner,
                                       first) == X_STATUS_SUCCESS);
  CHECK(fs.RegisterDeviceSymbolicLink("SAVE:", "\\Device\\Disk", owner,
                                     refused) == X_STATUS_OBJECT_NAME_COLLISION);
  CHECK_FALSE(refused);
  REQUIRE(fs.UnregisterSymbolicLink("save:")); // explicit current guest deletion
  REQUIRE(fs.RegisterDeviceSymbolicLink("save:", "\\Device\\Disk", owner,
                                       second) == X_STATUS_SUCCESS);
  CHECK_FALSE(fs.UnregisterSymbolicLink(*first));
  REQUIRE(fs.ResolvePath("save:\\file"));
  VirtualFileSystem other;
  CHECK_FALSE(other.UnregisterSymbolicLink(*second));
  REQUIRE(fs.UnregisterSymbolicLink(*second));
}

TEST_CASE("Bound aliases preserve independent SYSTEM TITLE and ordinary future names",
          "[vfs][namespace][source_alias][caller]") {
  VirtualFileSystem fs;
  auto device = std::make_unique<NullDevice>(
      "\\Device\\Disk", std::initializer_list<std::string>{"\\file"});
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  const auto owner = fs.AcquireDevice("\\Device\\Disk");
  std::shared_ptr<const SymbolicLinkToken> system, title;
  REQUIRE(fs.RegisterDeviceSymbolicLink("\\System??\\save:", "\\Device\\Disk",
                                       owner, system) == X_STATUS_SUCCESS);
  REQUIRE(fs.RegisterDeviceSymbolicLink("\\??\\save:", "\\Device\\Disk",
                                       owner, title) == X_STATUS_SUCCESS);
  REQUIRE(fs.UnregisterSymbolicLink(*title));
  CHECK(fs.ResolvePath("save:\\file") == nullptr);
  REQUIRE(fs.ResolvePath("\\System??\\save:\\file"));
  REQUIRE(fs.RegisterSymbolicLink("future:", "\\Device\\NotMounted"));
  std::string output;
  REQUIRE(fs.TranslateSymbolicPath("future:\\file", output) == X_STATUS_SUCCESS);
  CHECK(output == "\\Device\\NotMounted\\file");
  owner->BeginNamespaceRetirement();
  output = "unchanged";
  CHECK(XFAILED(fs.TranslateSymbolicPath("\\System??\\save:\\file", output)));
  CHECK(output == "unchanged");
  REQUIRE(fs.UnregisterSymbolicLink(*system));
}

TEST_CASE("Bound alias publication refuses a foreign or already retiring source",
          "[vfs][namespace][source_alias][failure]") {
  VirtualFileSystem fs, foreign;
  auto device = std::make_unique<NullDevice>(
      "\\Device\\Disk", std::initializer_list<std::string>{"\\file"});
  REQUIRE(device->Initialize());
  REQUIRE(foreign.RegisterDevice(std::move(device)));
  const auto owner = foreign.AcquireDevice("\\Device\\Disk");
  std::shared_ptr<const SymbolicLinkToken> token;
  CHECK(XFAILED(fs.RegisterDeviceSymbolicLink("save:", "\\Device\\Disk", owner,
                                            token)));
  CHECK_FALSE(token);
  CHECK_FALSE(fs.IsSymbolicLinkRegistered("save:"));
  CHECK(foreign.RegisterDeviceSymbolicLink("save:", "\\Device\\Other", owner,
                                         token) == X_STATUS_OBJECT_NAME_INVALID);
  CHECK_FALSE(token);
  owner->BeginNamespaceRetirement();
  CHECK(XFAILED(foreign.RegisterDeviceSymbolicLink("save:", "\\Device\\Disk", owner,
                                                 token)));
  CHECK_FALSE(token);
  CHECK_FALSE(foreign.IsSymbolicLinkRegistered("save:"));
}

TEST_CASE("Source aliases admit genuine child mounts but reject unrelated nested devices",
          "[vfs][namespace][source_alias][ancestry]") {
  VirtualFileSystem fs;
  auto disk = std::make_unique<NullDevice>(
      "\\Device\\Disk", std::initializer_list<std::string>{"\\file"});
  REQUIRE(disk->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(disk)));
  const auto owner = fs.AcquireDevice("\\Device\\Disk");
  std::shared_ptr<const SymbolicLinkToken> token;
  REQUIRE(fs.RegisterDeviceSymbolicLink("save:", "\\Device\\Disk", owner,
                                       token) == X_STATUS_SUCCESS);
  auto other = std::make_unique<NullDevice>(
      "\\Device\\Disk\\Nested", std::initializer_list<std::string>{"\\file"});
  REQUIRE(other->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(other)));
  REQUIRE(fs.ResolvePath("\\Device\\Disk\\Nested\\file"));
  std::string translated = "unchanged";
  CHECK(XFAILED(fs.TranslateSymbolicPath("save:\\Nested\\file", translated)));
  CHECK(translated == "unchanged");
  CHECK(fs.ResolvePath("save:\\Nested\\file") == nullptr);
  REQUIRE(fs.UnregisterDevice("\\Device\\Disk\\Nested"));
  auto child = std::make_unique<NullDevice>(
      "\\Device\\Disk\\Nested", std::initializer_list<std::string>{"\\file"});
  REQUIRE(child->Initialize());
  std::unique_ptr<Device> prepared_child = std::move(child);
  std::error_code error;
  auto prepared = fs.PrepareDeviceRegistration(prepared_child, error, owner);
  REQUIRE(prepared);
  REQUIRE_FALSE(error);
  REQUIRE(fs.RegisterPreparedDevice(*prepared));
  REQUIRE(fs.ResolvePath("save:\\Nested\\file"));
  REQUIRE(fs.RegisterSymbolicLink("\\Device\\Disk\\Redirect", "\\Device\\Elsewhere"));
  auto elsewhere = std::make_unique<NullDevice>(
      "\\Device\\Elsewhere", std::initializer_list<std::string>{"\\file"});
  REQUIRE(elsewhere->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(elsewhere)));
  translated = "unchanged";
  CHECK(XFAILED(fs.TranslateSymbolicPath("save:\\Redirect\\file", translated)));
  CHECK(translated == "unchanged");
  REQUIRE(fs.UnregisterSymbolicLink(*token));
}

TEST_CASE("Bound folder aliases use the actual registered namespace rather than a device prefix guess",
          "[vfs][namespace][source_alias][folder]") {
  VirtualFileSystem fs;
  auto folder = std::make_unique<NullDevice>(
      "\\FolderVolume", std::initializer_list<std::string>{"\\Content\\file"});
  REQUIRE(folder->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(folder)));
  const auto owner = fs.AcquireDevice("\\FolderVolume");
  std::shared_ptr<const SymbolicLinkToken> token;
  REQUIRE(fs.RegisterDeviceSymbolicLink("usb:", "\\FolderVolume", owner,
                                       token) == X_STATUS_SUCCESS);
  REQUIRE(fs.ResolvePath("usb:\\Content\\file"));
  REQUIRE(fs.AcquirePathDevice("usb:\\Content\\file") == owner);
  REQUIRE(fs.UnregisterSymbolicLink(*token));
}
}  // namespace xe::vfs::test

namespace xe::vfs::test {
namespace {
struct SourceOpenScratch {
  std::filesystem::path path;
  SourceOpenScratch() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() / ("xenia-source-open-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~SourceOpenScratch() { std::error_code error; std::filesystem::remove_all(path,error); }
};
}
TEST_CASE("An admitted source open cannot create on a replacement or foreign nested mount",
          "[vfs][source_open][generation][mutation]") {
  SourceOpenScratch scratch;
  REQUIRE(std::filesystem::create_directory(scratch.path / "first"));
  REQUIRE(std::filesystem::create_directory(scratch.path / "other"));
  VirtualFileSystem fs;
  auto first = std::make_unique<HostPathDevice>("\\Device\\SourceOpen", scratch.path / "first", false);
  REQUIRE(first->Initialize()); REQUIRE(fs.RegisterDevice(std::move(first)));
  auto source = fs.AcquireDevice("\\Device\\SourceOpen"); REQUIRE(source);
  std::string target = "\\Device\\SourceOpen\\new";
  std::filesystem::path forbidden = scratch.path / "other" / "new";
  SECTION("same path replacement") {
    REQUIRE(fs.UnregisterDevice("\\Device\\SourceOpen"));
    auto other = std::make_unique<HostPathDevice>("\\Device\\SourceOpen", scratch.path / "other", false);
    REQUIRE(other->Initialize()); REQUIRE(fs.RegisterDevice(std::move(other)));
  }
  SECTION("unrelated nested mount") {
    auto other = std::make_unique<HostPathDevice>("\\Device\\SourceOpen\\nested", scratch.path / "other", false);
    REQUIRE(other->Initialize()); REQUIRE(fs.RegisterDevice(std::move(other)));
    target = "\\Device\\SourceOpen\\nested\\new";
  }
  File* opened = nullptr; FileAction action{};
  const auto status = fs.OpenFile(nullptr,target,FileDisposition::kCreate,
      FileAccess::kGenericWrite,true,false,&opened,&action,FileOpenContext{source});
  CHECK(XFAILED(status)); CHECK(opened == nullptr);
  CHECK_FALSE(std::filesystem::exists(forbidden));
  CHECK_FALSE(std::filesystem::exists(scratch.path / "first" / "new"));
}
TEST_CASE("A source-constrained open admits real children and reports readonly writes truthfully",
          "[vfs][source_open][ancestry][readonly]") {
  SourceOpenScratch scratch;
  REQUIRE(std::filesystem::create_directory(scratch.path / "child"));
  { std::ofstream file(scratch.path / "child" / "readme"); file << "old"; }
  VirtualFileSystem fs;
  auto parent = std::make_unique<HostPathDevice>("\\Device\\SourceRead",scratch.path,false);
  REQUIRE(parent->Initialize()); REQUIRE(fs.RegisterDevice(std::move(parent)));
  const auto source = fs.AcquireDevice("\\Device\\SourceRead"); REQUIRE(source);
  auto child = std::make_unique<HostPathDevice>("\\Device\\SourceRead\\child",scratch.path / "child",true);
  REQUIRE(child->Initialize()); std::unique_ptr<Device> unpublished = std::move(child);
  std::error_code error; auto prepared = fs.PrepareDeviceRegistration(unpublished,error,source);
  REQUIRE(prepared); REQUIRE_FALSE(error); REQUIRE(fs.RegisterPreparedDevice(*prepared));
  File* opened = nullptr; FileAction action{};
  REQUIRE(fs.OpenFile(nullptr,"\\Device\\SourceRead\\child\\readme",FileDisposition::kOpen,
      FileAccess::kGenericRead,false,true,&opened,&action,FileOpenContext{source}) == X_STATUS_SUCCESS);
  REQUIRE(opened); opened->Destroy(); opened = nullptr;
  CHECK(fs.OpenFile(nullptr,"\\Device\\SourceRead\\child\\readme",FileDisposition::kOpen,
      FileAccess::kGenericWrite,false,true,&opened,&action,FileOpenContext{source}) == X_STATUS_ACCESS_DENIED);
  CHECK(opened == nullptr);
  CHECK(fs.OpenFile(nullptr,"\\Device\\SourceRead\\child\\new",FileDisposition::kCreate,
      FileAccess::kGenericWrite,true,false,&opened,&action,FileOpenContext{source}) == X_STATUS_ACCESS_DENIED);
  CHECK_FALSE(std::filesystem::exists(scratch.path / "child" / "new"));
  source->BeginNamespaceRetirement();
  CHECK(XFAILED(fs.OpenFile(nullptr,"\\Device\\SourceRead\\child\\readme",FileDisposition::kOpen,
      FileAccess::kGenericRead,false,true,&opened,&action,FileOpenContext{source})));
  CHECK(opened == nullptr);
}
TEST_CASE("RootDirectory relative opens cross the actual registered content child and preserve source constraints",
          "[vfs][source_open][root_directory][readonly][ancestry]") {
  SourceOpenScratch scratch;
  REQUIRE(std::filesystem::create_directories(scratch.path/"primary"/"Content"));
  REQUIRE(std::filesystem::create_directory(scratch.path/"installed"));
  {std::ofstream file(scratch.path/"primary"/"Content"/"item");file<<"stale";}
  {std::ofstream file(scratch.path/"installed"/"item");file<<"mounted";}
  VirtualFileSystem fs;
  auto parent=std::make_unique<HostPathDevice>("\\Device\\RelativeContent",scratch.path/"primary",false);
  REQUIRE(parent->Initialize());REQUIRE(fs.RegisterDevice(std::move(parent)));
  const auto source=fs.AcquireDevice("\\Device\\RelativeContent");REQUIRE(source);
  auto* root=source->ResolvePath("");REQUIRE(root);
  auto child=std::make_unique<HostPathDevice>("\\Device\\RelativeContent\\Content",scratch.path/"installed",true);
  REQUIRE(child->Initialize());std::unique_ptr<Device> unpublished=std::move(child);
  bool genuine=true;
  SECTION("registered actual source child") {
    std::error_code error;auto prepared=fs.PrepareDeviceRegistration(unpublished,error,source);
    REQUIRE(prepared);REQUIRE_FALSE(error);REQUIRE(fs.RegisterPreparedDevice(*prepared));
  }
  SECTION("an unrelated same-prefix mount cannot inherit the root handle's source") {
    genuine=false;REQUIRE(fs.RegisterDevice(std::move(unpublished)));
  }
  SECTION("a withdrawing actual child never reopens the parent's stale cached file") {
    genuine=false;
    std::error_code error;auto prepared=fs.PrepareDeviceRegistration(unpublished,error,source);
    REQUIRE(prepared);REQUIRE_FALSE(error);REQUIRE(fs.RegisterPreparedDevice(*prepared));
    const auto withdrawing=fs.AcquireDevice("\\Device\\RelativeContent\\Content");REQUIRE(withdrawing);
    withdrawing->BeginNamespaceRetirement();
    CHECK_FALSE(fs.AcquirePathDevice("\\Device\\RelativeContent\\Content\\item"));
  }
  File* opened=nullptr;FileAction action{};
  struct Close {File*& value;~Close(){if(value)value->Destroy();}} close{opened};
  const auto result=fs.OpenFile(root,"Content\\item",FileDisposition::kOpen,
      FileAccess::kGenericRead,false,true,&opened,&action,FileOpenContext{source});
  if(!genuine) {CHECK(XFAILED(result));CHECK(opened==nullptr);return;}
  REQUIRE(result==X_STATUS_SUCCESS);REQUIRE(opened);
  CHECK(opened->retained_device()!=source);
  std::array<uint8_t,7> actual{};size_t read=0;
  REQUIRE(opened->ReadSync(actual,0,&read)==X_STATUS_SUCCESS);REQUIRE(read==actual.size());
  CHECK(std::string(actual.begin(),actual.end())=="mounted");
  opened->Destroy();opened=nullptr;
  CHECK(fs.OpenFile(root,"Content\\new",FileDisposition::kCreate,
      FileAccess::kGenericWrite,false,true,&opened,&action,FileOpenContext{source})==X_STATUS_ACCESS_DENIED);
  CHECK(opened==nullptr);
  CHECK_FALSE(std::filesystem::exists(scratch.path/"primary"/"Content"/"new"));
  CHECK_FALSE(std::filesystem::exists(scratch.path/"installed"/"new"));
}
#if XE_PLATFORM_LINUX
namespace {
struct UnrecognizedNativeFileAuthority final : ManagedFileOpenAuthority {};
class LegacyManagedOpenPolicy final : public ManagedFileService {
 public:
  LegacyManagedOpenPolicy(std::filesystem::path path,
      std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent)
      :path_(std::move(path)),parent_(std::move(parent)) {}
  const std::filesystem::path& backing_path() const override {return path_;}
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const override {return parent_;}
  bool retiring() const override {return false;}
  X_STATUS Open(std::shared_ptr<Device>,std::string,FileDisposition,uint32_t,
      bool,File** file,FileAction* action) override {
    ++calls;*file=nullptr;*action=FileAction::kDoesNotExist;
    return X_STATUS_NOT_SUPPORTED;
  }
  unsigned calls=0;
 private:
  std::filesystem::path path_;
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent_;
};
}
TEST_CASE("An issued managed open context cannot fall through to raw create or an unrelated leaf policy",
          "[vfs][source_open][managed_context][no_mutation]") {
  // Real retained folder/VFS/registered leaf; the test authority is explicitly
  // unrecognized. It grants no native SAVE/Task/profile or success claim.
  SourceOpenScratch scratch;
  {std::ofstream file(scratch.path/"saved");file<<"preserved";}
  VirtualFileSystem fs;
  auto device=std::make_unique<HostPathDevice>("\\Device\\ManagedContext",scratch.path,false);
  REQUIRE(device->Initialize());REQUIRE(fs.RegisterDevice(std::move(device)));
  const auto source=fs.AcquireDevice("\\Device\\ManagedContext");REQUIRE(source);
  FileOpenContext context;
  context.expected_device=source;
  context.managed_authority=std::make_shared<const UnrecognizedNativeFileAuthority>();
  File* file=nullptr;FileAction action{};
  SECTION("a missing policy does not create an ordinary file") {
    CHECK(fs.OpenFile(nullptr,"\\Device\\ManagedContext\\new",FileDisposition::kCreate,
        FileAccess::kGenericWrite,false,true,&file,&action,context)==X_STATUS_ACCESS_DENIED);
    CHECK(file==nullptr);CHECK(action==FileAction::kDoesNotExist);
    CHECK_FALSE(std::filesystem::exists(scratch.path/"new"));
  }
  SECTION("an existing different policy rejects before overwrite and preserves legacy behavior without authority") {
    const auto parent=source->ManagedDirectoryIdentity();REQUIRE(parent);
    auto service=std::make_shared<LegacyManagedOpenPolicy>(scratch.path/"saved",parent);
    REQUIRE(fs.RegisterManagedFile(service));
    CHECK(fs.OpenFile(nullptr,"\\Device\\ManagedContext\\saved",FileDisposition::kOverwrite,
        FileAccess::kGenericWrite,false,true,&file,&action,context)==X_STATUS_ACCESS_DENIED);
    CHECK(file==nullptr);CHECK(service->calls==0);
    CHECK(fs.OpenFile(nullptr,"\\Device\\ManagedContext\\saved",FileDisposition::kOpen,
        FileAccess::kGenericRead,false,true,&file,&action,FileOpenContext{source})==X_STATUS_NOT_SUPPORTED);
    CHECK(file==nullptr);CHECK(service->calls==1);
  }
  std::ifstream input(scratch.path/"saved");std::string bytes;
  input>>bytes;CHECK(bytes=="preserved");
}
#endif  // XE_PLATFORM_LINUX
}  // namespace xe::vfs::test
