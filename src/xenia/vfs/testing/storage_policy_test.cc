#include <array>
#include <algorithm>
#include <fstream>
#include <random>
#include "third_party/catch/include/catch.hpp"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/base/managed_volume_authority.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/host_path_file.h"
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/vfs/virtual_file_system.h"
#if !XE_PLATFORM_WIN32
namespace xe::vfs::test {
TEST_CASE("Retired files retain their VFS tree through mount teardown",
          "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-owned-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "data") << "value";
  std::error_code error;
  auto source = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(source);
  VirtualFileSystem fs;
  auto device = std::make_unique<HostPathDevice>("\\Device\\Owned", path, false,
                                                 false, "FATX", source);
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  File* file = nullptr;
  FileAction action;
  REQUIRE(fs.OpenFile(nullptr, "\\Device\\Owned\\data", FileDisposition::kOpen,
                      FileAccess::kGenericRead, false, true, &file,
                      &action) == X_STATUS_SUCCESS);
  REQUIRE(file);
  REQUIRE_FALSE(fs.UnregisterDevice("\\Device\\Owned"));
  REQUIRE(source->Observe().available);
  REQUIRE(fs.RetireDevice("\\Device\\Owned"));
  std::array<uint8_t, 5> bytes{};
  size_t read = 77;
  REQUIRE(file->ReadSync(bytes, 0, &read) == X_STATUS(0xC000009D));
  REQUIRE(read == 0);
  fs.Clear();  // File still owns Device -> Entry tree; its destructor must
               // remain safe.
  file->Destroy();
  REQUIRE(source->open_file_count() == 0);
  REQUIRE(source->Observe().available);
  std::filesystem::remove_all(path, error);
}
TEST_CASE("Host storage errors distinguish disk full from EOF",
          "[vfs_storage_lifetime]") {
  REQUIRE(HostStorageError(std::make_error_code(
              std::errc::no_space_on_device)) == X_STATUS_DISK_FULL);
  REQUIRE(HostStorageError(std::make_error_code(
              std::errc::read_only_file_system)) == X_STATUS_ACCESS_DENIED);
  REQUIRE(HostStorageError(std::make_error_code(std::errc::no_such_device)) ==
          X_STATUS(0xC000009D));
}
namespace {
class PartialWrite final : public filesystem::FileHandle {
 public:
  PartialWrite() : FileHandle(std::filesystem::path()) {}
  bool Read(size_t, void*, size_t, size_t* count) override {
    *count = 0;
    return false;
  }
  bool Write(size_t, const void*, size_t, size_t* count) override {
    *count = 0;
    return false;
  }
  filesystem::FileIoResult WriteResult(size_t,
                                       std::span<const uint8_t>) override {
    return {std::make_error_code(std::errc::no_space_on_device), 3};
  }
  bool SetLength(size_t) override { return false; }
  bool SetTimes(uint64_t, uint64_t, uint64_t) override { return false; }
  void Flush() override {}
};
}  // namespace
TEST_CASE("Host file preserves actual partial bytes with disk-full status",
          "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-partial-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "data") << "old";
  HostPathDevice device("\\Device\\Partial", path, false);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("data"));
  REQUIRE(entry);
  auto* file = new HostPathFile(FileAccess::kFileWriteData, entry,
                                std::make_unique<PartialWrite>());
  const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
  size_t written = 0;
  REQUIRE(file->WriteSync(bytes, 0, &written) == X_STATUS_DISK_FULL);
  REQUIRE(written == 3);
  file->Destroy();
  std::error_code error;
  std::filesystem::remove_all(path, error);
}
TEST_CASE(
    "Overwrite reservation blocks late aliases and commits exact close length",
    "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-trim-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  {
    std::ofstream file(path / "data", std::ios::binary);
    std::string data(4096, 'x');
    file.write(data.data(), data.size());
  }
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(root);
  HostPathDevice device("\\Device\\Trim", path, false, false, "FATX", root);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("data"));
  REQUIRE(entry);
  auto mapped = root->MapRead("data", 0, 0, error);
  REQUIRE(mapped);
  REQUIRE_FALSE(entry->Overwrite());
  REQUIRE(entry->size() == 4096);
  mapped.reset();
  REQUIRE(entry->Overwrite());
  File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileWriteData, &file) == X_STATUS_SUCCESS);
  const std::array<uint8_t, 1> byte{'y'};
  size_t written = 0;
  REQUIRE(file->WriteSync(byte, 0, &written) == X_STATUS_SUCCESS);
  REQUIRE(written == 1);
  // Raw aliases cannot slip between logical overwrite and final trim.
  mapped = root->MapRead("data", 0, 0, error);
  REQUIRE_FALSE(mapped);
  REQUIRE(error == std::errc::device_or_resource_busy);
  auto alias = root->OpenFile("data", FileAccess::kGenericRead, error);
  REQUIRE_FALSE(alias);
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(root->Remove("data", false) == std::errc::device_or_resource_busy);
  file->Destroy();
  REQUIRE(std::filesystem::file_size(path / "data") == 1);
  std::vector<uint8_t> stored;
  REQUIRE_FALSE(root->ReadFile("data", 4096, stored));
  REQUIRE(stored == std::vector<uint8_t>{'y'});
  // Recreate a mount as a restart surrogate: its enumeration must see the
  // actual one-byte backing file, never the old freed-cluster tail.
  HostPathDevice reopened("\\Device\\Reopened", path, false, false, "FATX",
                          root);
  REQUIRE(reopened.Initialize());
  REQUIRE(reopened.ResolvePath("data")->size() == 1);
  std::filesystem::remove_all(path, error);
}
TEST_CASE(
    "Failed following Open rolls back an unconsumed overwrite reservation",
    "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-abort-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "data") << "original";
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(root);
  HostPathDevice device("\\Device\\Abort", path, false, false, "FATX", root);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("data"));
  REQUIRE(entry->Overwrite());
  REQUIRE(entry->size() == 0);
  REQUIRE(entry->Open(FileAccess::kFileWriteData, nullptr) ==
          X_STATUS_INVALID_PARAMETER);
  REQUIRE(entry->size() == 8);
  REQUIRE(std::filesystem::file_size(path / "data") == 8);
  auto alias = root->OpenFile("data", FileAccess::kGenericRead, error);
  REQUIRE(alias);
  alias.reset();
  // A real source-liveness failure between Overwrite and Open also aborts the
  // speculative metadata and releases every owned native handle.
  REQUIRE(entry->Overwrite());
  root->Retire();
  File* out = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileWriteData, &out) ==
          X_STATUS(0xC000009D));
  REQUIRE_FALSE(out);
  REQUIRE(entry->size() == 8);
  REQUIRE(std::filesystem::file_size(path / "data") == 8);
  REQUIRE(root->open_file_count() == 0);
  std::filesystem::remove_all(path, error);
}
TEST_CASE("Overwrite reservation preserves old FATX clusters until regrowth",
          "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-clusters-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "data") << "abcdefgh";
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(root);
  HostPathDevice device("\\Device\\Clusters", path, false, false, "FATX", root);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("data"));
  REQUIRE(entry->Overwrite());
  // Even before the initiating Open, every unrelated mapping is excluded.
  REQUIRE_FALSE(root->MapRead("data", 0, 0, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead | FileAccess::kFileWriteData,
                      &file) == X_STATUS_SUCCESS);
  const std::array<uint8_t, 1> byte{'Y'};
  size_t written = 0;
  REQUIRE(file->WriteSync(byte, 4, &written) == X_STATUS_SUCCESS);
  std::array<uint8_t, 8> bytes{};
  size_t read = 0;
  REQUIRE(file->ReadSync(bytes, 0, &read) == X_STATUS_SUCCESS);
  REQUIRE(read == 5);
  REQUIRE(std::string(bytes.begin(), bytes.begin() + read) == "abcdY");
  file->Destroy();
  REQUIRE(std::filesystem::file_size(path / "data") == 5);
  std::filesystem::remove_all(path, error);
}
TEST_CASE("Logical content retirement invalidates independent public volume IO",
          "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-dependent-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directories(path / "content"));
  REQUIRE(std::filesystem::create_directory(path / "volume"));
  std::ofstream(path / "volume" / "data") << "physical";
  std::error_code error;
  auto source =
      filesystem::HostStorageRoot::Open(path / "content", false, error, 0);
  auto backing =
      filesystem::HostStorageRoot::Open(path / "volume", false, error, 0);
  REQUIRE(source);
  REQUIRE(backing);
  HostPathDevice device("\\Device\\Volume", path / "volume", false, false,
                        "FATX", backing, source);
  REQUIRE(device.Initialize());
  auto* entry = device.ResolvePath("data");
  REQUIRE(entry);
  File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead | FileAccess::kFileWriteData,
                      &file) == X_STATUS_SUCCESS);
  source->Retire();
  REQUIRE_FALSE(device.storage()->Observe().available);
  std::array<uint8_t, 1> bytes{'x'};
  size_t count = 123;
  REQUIRE(file->ReadSync(bytes, 0, &count) == X_STATUS(0xC000009D));
  REQUIRE(count == 0);
  REQUIRE(file->WriteSync(bytes, 0, &count) == X_STATUS(0xC000009D));
  REQUIRE(count == 0);
  REQUIRE(file->SetLength(1) == X_STATUS(0xC000009D));
  File* later = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead, &later) ==
          X_STATUS(0xC000009D));
  REQUIRE_FALSE(later);
  file->Destroy();
  REQUIRE(backing->Observe().available);
  std::vector<uint8_t> stored;
  REQUIRE_FALSE(backing->ReadFile("data", 16, stored));
  REQUIRE(stored ==
          std::vector<uint8_t>{'p', 'h', 'y', 's', 'i', 'c', 'a', 'l'});
  // A fresh source generation admits a fresh volume view; the old dependency
  // remains retired, and retiring the new mount never retires backing/source.
  auto next =
      filesystem::HostStorageRoot::Open(path / "content", false, error, 0);
  REQUIRE(next);
  HostPathDevice replacement("\\Device\\Replacement", path / "volume", false,
                             false, "FATX", backing, next);
  REQUIRE(replacement.Initialize());
  replacement.storage()->Retire();
  REQUIRE(next->Observe().available);
  REQUIRE(backing->Observe().available);
  REQUIRE_FALSE(device.storage()->Observe().available);
  std::filesystem::remove_all(path, error);
}
TEST_CASE(
    "Overwrite EOF shrink and regrow reuse clusters with guarded real growth",
    "[vfs_storage_lifetime]") {
  const auto path = std::filesystem::temp_directory_path() /
                    ("xenia-vfs-eof-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "data") << "abcdefgh";
  std::error_code error;
  // Reusing existing physical bytes needs no allocation. One new byte plus
  // metadata cannot fit this allowance, proving actual growth still admits.
  auto space = filesystem::HostStorageSpace::ForPath(
      path, 0, error, [](const auto&, auto&) {
        return std::filesystem::space_info{1ull << 30, 65536, 65536};
      });
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0, space);
  REQUIRE(root);
  HostPathDevice device("\\Device\\Eof", path, false, false, "FATX", root);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("data"));
  REQUIRE(entry);
  REQUIRE(entry->Overwrite());
  File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead | FileAccess::kFileWriteData,
                      &file) == X_STATUS_SUCCESS);
  REQUIRE(file->SetLength(3) == X_STATUS_SUCCESS);
  REQUIRE(entry->size() == 3);
  REQUIRE(std::filesystem::file_size(path / "data") == 8);
  std::array<uint8_t, 8> bytes{};
  size_t count = 99;
  REQUIRE(file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == 3);
  REQUIRE(std::string(bytes.begin(), bytes.begin() + count) == "abc");
  REQUIRE(file->ReadSync(bytes, 3, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == 0);
  REQUIRE(file->SetLength(8) == X_STATUS_SUCCESS);
  REQUIRE(file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == 8);
  REQUIRE(std::string(bytes.begin(), bytes.end()) == "abcdefgh");
  REQUIRE(file->SetLength(9) == X_STATUS_DISK_FULL);
  REQUIRE(entry->size() == 8);
  REQUIRE(std::filesystem::file_size(path / "data") == 8);
  REQUIRE(file->SetLength(5) == X_STATUS_SUCCESS);
  REQUIRE(std::filesystem::file_size(path / "data") == 8);
  // Write failures preserve both logical bounds and the retained backing.
  const std::array<uint8_t, 1> replacement{'Y'};
  REQUIRE(file->WriteSync(replacement, 6, &count) == X_STATUS_DISK_FULL);
  REQUIRE(count == 0);
  REQUIRE(entry->size() == 5);
  file->Destroy();
  REQUIRE(std::filesystem::file_size(path / "data") == 5);
  std::vector<uint8_t> stored;
  REQUIRE_FALSE(root->ReadFile("data", 8, stored));
  REQUIRE(std::string(stored.begin(), stored.end()) == "abcde");
  std::filesystem::remove_all(path, error);
}
TEST_CASE(
    "Populated directory rename rebases cached paths and retained ownership",
    "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-tree-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directories(path / "old" / "nested"));
  REQUIRE(std::filesystem::create_directory(path / "target"));
  std::ofstream(path / "old" / "nested" / "child") << "value";
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(root);
  HostPathDevice device("\\Device\\Tree", path, false, false, "FATX", root);
  REQUIRE(device.Initialize());
  auto* top = static_cast<HostPathEntry*>(device.ResolvePath("old"));
  auto* nested = static_cast<HostPathEntry*>(device.ResolvePath("old\\nested"));
  auto* child =
      static_cast<HostPathEntry*>(device.ResolvePath("old\\nested\\child"));
  auto* target = device.ResolvePath("target");
  REQUIRE(top);
  REQUIRE(nested);
  REQUIRE(child);
  REQUIRE(target);
  const auto original_host = child->host_path();
  const auto original_guest = child->absolute_path();
  REQUIRE(top->Rename(nested, "cycle", false) == X_STATUS_INVALID_PARAMETER);
  REQUIRE(child->host_path() == original_host);
  REQUIRE(child->absolute_path() == original_guest);
  // A physical alias outside the Entry tree still protects its source subtree.
  auto alias =
      root->OpenFile("old/nested/child", FileAccess::kGenericRead, error);
  REQUIRE(alias);
  REQUIRE(top->Rename(target, "moved", false) == X_STATUS(0xC0000043));
  REQUIRE(child->host_path() == original_host);
  REQUIRE(child->absolute_path() == original_guest);
  REQUIRE(child->parent() == nested);
  REQUIRE(std::filesystem::exists(path / "old" / "nested" / "child"));
  alias.reset();
  auto mapped = root->MapRead("old/nested/child", 0, 0, error);
  REQUIRE(mapped);
  REQUIRE(top->Rename(target, "moved", false) == X_STATUS(0xC0000043));
  REQUIRE(mapped->data()[0] == 'v');
  mapped.reset();
  // The initiating directory handle is the only lease exempted from rename.
  File* directory = nullptr;
  REQUIRE(top->Open(FileAccess::kGenericRead, &directory) == X_STATUS_SUCCESS);
  REQUIRE(top->Rename(target, "moved", false, directory->storage_lease()) ==
          X_STATUS_SUCCESS);
  REQUIRE(directory->entry() == top);
  REQUIRE(top->parent() == target);
  REQUIRE(top->name() == "moved");
  REQUIRE(nested->parent() == top);
  REQUIRE(child->parent() == nested);
  REQUIRE(top->path() == "target\\moved");
  REQUIRE(nested->path() == "target\\moved\\nested");
  REQUIRE(child->path() == "target\\moved\\nested\\child");
  REQUIRE(child->absolute_path() ==
          "\\Device\\Tree\\target\\moved\\nested\\child");
  REQUIRE(child->host_path() == path / "target" / "moved" / "nested" / "child");
  REQUIRE_FALSE(device.ResolvePath("old"));
  REQUIRE(device.ResolvePath("target\\moved\\nested\\child") == child);
  directory->Destroy();
  File* opened = nullptr;
  REQUIRE(child->Open(FileAccess::kGenericRead, &opened) == X_STATUS_SUCCESS);
  std::array<uint8_t, 5> bytes{};
  size_t count = 0;
  REQUIRE(opened->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == 5);
  REQUIRE(std::string(bytes.begin(), bytes.end()) == "value");
  opened->Destroy();
  // New entries after the move must use the rebased parent host directory too.
  REQUIRE(nested->CreateEntry("created", kFileAttributeNormal));
  REQUIRE(std::filesystem::exists(path / "target" / "moved" / "nested" /
                                  "created"));
  // A second, same-parent rename must rebase every depth again.
  REQUIRE(top->Rename(target, "again", false) == X_STATUS_SUCCESS);
  REQUIRE(child->host_path() == path / "target" / "again" / "nested" / "child");
  REQUIRE(child->absolute_path() ==
          "\\Device\\Tree\\target\\again\\nested\\child");
  REQUIRE(child->Open(FileAccess::kGenericRead, &opened) == X_STATUS_SUCCESS);
  opened->Destroy();
  std::filesystem::remove_all(path, error);
}
TEST_CASE("Renamed directory preserves collapsed payload host suffixes",
          "[vfs_storage_lifetime]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-collapsed-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directories(path / "old" / "00000001" /
                                              "package"));
  std::ofstream(path / "old" / "00000001" / "package" / "payload.bin")
      << "save";
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(root);
  HostPathDevice device("\\Device\\Collapsed", path, false, true, "FATX", root);
  REQUIRE(device.Initialize());
  auto* top = device.ResolvePath("old");
  auto* payload =
      static_cast<HostPathEntry*>(device.ResolvePath("old\\00000001\\package"));
  REQUIRE(top);
  REQUIRE(payload);
  REQUIRE(!(payload->attributes() & kFileAttributeDirectory));
  REQUIRE(top->Rename(device.ResolvePath(""), "new", false) ==
          X_STATUS_SUCCESS);
  REQUIRE(payload->path() == "new\\00000001\\package");
  REQUIRE(payload->host_path() ==
          path / "new" / "00000001" / "package" / "payload.bin");
  File* opened = nullptr;
  REQUIRE(payload->Open(FileAccess::kGenericRead, &opened) == X_STATUS_SUCCESS);
  std::array<uint8_t, 4> bytes{};
  size_t count = 0;
  REQUIRE(opened->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == 4);
  REQUIRE(std::string(bytes.begin(), bytes.end()) == "save");
  opened->Destroy();
  std::filesystem::remove_all(path, error);
}
}  // namespace xe::vfs::test
#endif

#if XE_PLATFORM_WIN32
namespace xe::vfs::test {
TEST_CASE("Windows host EOF retains its legacy NT status",
          "[vfs_windows_storage]") {
  REQUIRE(HostStorageError({ERROR_HANDLE_EOF, std::system_category()}) ==
          X_STATUS_END_OF_FILE);
}
TEST_CASE("Windows writable mappings remain available only on writable views",
          "[vfs_windows_storage]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-win-mapping-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "data") << "value";
  {
    HostPathDevice writable("\\Device\\Writable", path, false);
    REQUIRE(writable.Initialize());
    auto* entry = writable.ResolvePath("data");
    REQUIRE(entry);
    auto mapped = entry->OpenMapped(MappedMemory::Mode::kReadWrite);
    REQUIRE(mapped);
  }
  {
    HostPathDevice readonly("\\Device\\Readonly", path, true);
    REQUIRE(readonly.Initialize());
    auto* entry = readonly.ResolvePath("data");
    REQUIRE(entry);
    REQUIRE_FALSE(entry->OpenMapped(MappedMemory::Mode::kReadWrite));
  }
  std::error_code error;
  std::filesystem::remove_all(path, error);
}
}  // namespace xe::vfs::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::vfs::test {
TEST_CASE("Filtered managed Content survives all real device admissions without exposing private entries",
          "[vfs_storage_lifetime][managed_volume_authority]") {
  const auto path = std::filesystem::temp_directory_path() /
      ("xenia-filtered-content-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(root);
  REQUIRE_FALSE(root->Create("hidden-parent", true));
  auto state_parent = root->Subroot("hidden-parent", false, error);
  REQUIRE(state_parent);
  constexpr std::array<uint8_t, 3> data{1, 2, 3};
  REQUIRE_FALSE(root->WriteNewFile("public", data).error);
  filesystem::ManagedVolumeAuthority::Options options;
  options.content = root;
  options.source_id = "filtered-device";
  options.device_id = 3;
  options.state_parent_override = state_parent;
  auto owner = filesystem::ManagedVolumeAuthority::Open(std::move(options));
  REQUIRE_FALSE(owner->error());
  const auto hidden = owner->intents()->private_parent()->path().lexically_relative(path);
  VirtualFileSystem fs;
  auto device = std::make_unique<HostPathDevice>("\\Device\\Filtered", path, false,
                                                false, "FATX", owner->content());
  REQUIRE(device->Initialize());  // Pending admission + owned GuestView.
  REQUIRE(device->GuestBackings()->front().projection);
  REQUIRE(fs.RegisterDevice(std::move(device))); // Device's final admission.
  REQUIRE(fs.ResolvePath("\\Device\\Filtered\\public"));
  const auto hidden_guest = "\\Device\\Filtered\\" + hidden.generic_string();
  REQUIRE_FALSE(fs.ResolvePath(hidden_guest));
  REQUIRE_FALSE(fs.ResolvePath(hidden_guest + "\\owner.v1"));
  auto plain = std::make_unique<HostPathDevice>("\\Device\\Unfiltered", path, false);
  REQUIRE_FALSE(plain->Initialize()); // No authority to waive the overlap.
  const auto shell = owner->public_root();
  auto volume = std::make_unique<HostPathDevice>("\\Device\\PublicVolume", shell->path(),
                                                false, false, "FATX", shell, owner->content(),
                                                HostPathDevice::ControlRole::kFolderVolume);
  REQUIRE(volume->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(volume)));
  REQUIRE(fs.ResolvePath("\\Device\\PublicVolume\\Content"));
  REQUIRE_FALSE(fs.ResolvePath("\\Device\\PublicVolume\\owner.v1"));
  owner.reset();
  REQUIRE_FALSE(fs.ResolvePath(hidden_guest));
  fs.Clear();
  std::filesystem::remove_all(path, error);
}
}  // namespace xe::vfs::test

namespace xe::vfs::test {
namespace {
class SegmentedOwnedWrite final : public filesystem::FileHandle {
 public:
  enum class Stop { kNone, kErrorAfterThree, kZeroAfterTwo };
  SegmentedOwnedWrite(std::unique_ptr<filesystem::FileHandle> file, Stop stop)
      : FileHandle(std::filesystem::path()), file_(std::move(file)), stop_(stop) {}
  std::shared_ptr<const filesystem::HostStorageFileLease> storage_lease() const override {
    return file_->storage_lease();
  }
  std::optional<filesystem::FileHandleInfo> QueryInfo(std::error_code& error) const override {
    return file_->QueryInfo(error);
  }
  filesystem::FileIoResult WriteResult(size_t offset, std::span<const uint8_t> bytes) override {
    if (stop_ == Stop::kZeroAfterTwo && wrote_ >= 2) { return {{}, 0}; }
    const auto remaining = stop_ == Stop::kErrorAfterThree ? 3 - wrote_ : bytes.size();
    const auto amount = std::min<size_t>({2, remaining, bytes.size()});
    const auto result = file_->WriteResult(offset, bytes.first(amount));
    wrote_ += result.bytes;
    if (!result.error && stop_ == Stop::kErrorAfterThree && wrote_ == 3) {
      return {std::make_error_code(std::errc::no_space_on_device), result.bytes};
    }
    return result;
  }
  bool Read(size_t offset, void* bytes, size_t size, size_t* count) override {
    return file_->Read(offset, bytes, size, count);
  }
  bool Write(size_t offset, const void* bytes, size_t size, size_t* count) override {
    const auto result = WriteResult(offset, {static_cast<const uint8_t*>(bytes), size});
    *count = result.bytes; return !result.error;
  }
  bool SetLength(size_t size) override { return file_->SetLength(size); }
  bool SetTimes(uint64_t a, uint64_t b, uint64_t c) override { return file_->SetTimes(a,b,c); }
  void Flush() override { file_->Flush(); }
 private:
  std::unique_ptr<filesystem::FileHandle> file_;
  Stop stop_;
  size_t wrote_ = 0;
};
}
TEST_CASE("Regular content writes complete real short-success chunks",
          "[vfs_storage_lifetime][complete_write]") {
  const auto path = std::filesystem::temp_directory_path() /
      ("xenia-full-write-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "data", std::ios::binary) << "ABCDEFGHIJKL";
  std::error_code error;
  auto storage = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(storage); REQUIRE_FALSE(error);
  HostPathDevice device("\\Device\\FullWrite", path, false, false, "FATX", storage);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("data")); REQUIRE(entry);
  auto handle = storage->OpenFile("data", filesystem::FileAccess::kGenericAll, error);
  REQUIRE(handle); REQUIRE_FALSE(error);
  auto file = new HostPathFile(FileAccess::kFileWriteData, entry,
      std::make_unique<SegmentedOwnedWrite>(std::move(handle), SegmentedOwnedWrite::Stop::kNone));
  const std::array<uint8_t,7> bytes{0,1,2,0,4,5,6};
  size_t count = 99;
  REQUIRE(file->WriteSync(bytes, 3, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == bytes.size()); file->Destroy();
  std::vector<uint8_t> actual;
  REQUIRE_FALSE(storage->ReadFile("data", 64, actual));
  const std::vector<uint8_t> expected{'A','B','C',0,1,2,0,4,5,6,'K','L'};
  REQUIRE(actual == expected);
  std::filesystem::remove_all(path, error);
}
TEST_CASE("Regular content writes preserve real partial mutation on error or zero progress",
          "[vfs_storage_lifetime][complete_write]") {
  for (auto stop : {SegmentedOwnedWrite::Stop::kErrorAfterThree,
                    SegmentedOwnedWrite::Stop::kZeroAfterTwo}) {
    const auto path = std::filesystem::temp_directory_path() /
        ("xenia-full-write-fault-" + std::to_string(std::random_device{}()));
    REQUIRE(std::filesystem::create_directory(path));
    std::ofstream(path / "data", std::ios::binary) << "AAAAAA";
    std::error_code error;
    auto storage = filesystem::HostStorageRoot::Open(path, false, error, 0);
    REQUIRE(storage); REQUIRE_FALSE(error);
    HostPathDevice device("\\Device\\FullWriteFault", path, false, false, "FATX", storage);
    REQUIRE(device.Initialize());
    auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("data")); REQUIRE(entry);
    auto handle = storage->OpenFile("data", filesystem::FileAccess::kGenericAll, error);
    REQUIRE(handle); REQUIRE_FALSE(error);
    auto file = new HostPathFile(FileAccess::kFileWriteData, entry,
        std::make_unique<SegmentedOwnedWrite>(std::move(handle), stop));
    const std::array<uint8_t,6> bytes{1,2,3,4,5,6}; size_t count = 99;
    const auto status = file->WriteSync(bytes, 0, &count);
    REQUIRE(XFAILED(status));
    const size_t expected_count = stop == SegmentedOwnedWrite::Stop::kErrorAfterThree ? 3 : 2;
    REQUIRE(count == expected_count);
    if (stop == SegmentedOwnedWrite::Stop::kErrorAfterThree) { REQUIRE(status == X_STATUS_DISK_FULL); }
    file->Destroy();
    std::vector<uint8_t> actual; REQUIRE_FALSE(storage->ReadFile("data", 64, actual));
    REQUIRE(actual.size() == 6);
    REQUIRE(std::equal(bytes.begin(), bytes.begin() + expected_count, actual.begin()));
    REQUIRE(std::all_of(actual.begin() + expected_count, actual.end(), [](auto value) { return value == 'A'; }));
    std::filesystem::remove_all(path, error);
  }
}

namespace {
struct PreparedMountRoot {
  std::filesystem::path path;
  PreparedMountRoot() {
    path=std::filesystem::temp_directory_path()/
        ("xenia-vfs-prepared-"+std::to_string(std::random_device{}()));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~PreparedMountRoot() { std::error_code ignored;std::filesystem::remove_all(path,ignored); }
};
struct DeviceRundownObservation { uint32_t destroyed=0;bool under_namespace=false; };
class ObservedHostDevice final : public HostPathDevice {
 public:
  ObservedHostDevice(std::string_view name,const std::filesystem::path& path,
                     DeviceRundownObservation& observation)
      : HostPathDevice(name,path,false),observation_(observation) {}
  ~ObservedHostDevice() override {
    ++observation_.destroyed;
    observation_.under_namespace |= xe::global_critical_region::is_held_by_current_thread();
  }
 private:
  DeviceRundownObservation& observation_;
};
std::unique_ptr<Device> AdmittedFixtureDevice(PreparedMountRoot& root,
    DeviceRundownObservation& observation,std::string_view name="Prepared:") {
  auto device=std::make_unique<ObservedHostDevice>(name,root.path,observation);
  REQUIRE(device->Initialize());
  return device;
}
}  // namespace
TEST_CASE("Prepared device publication uses real admission before inherited namespace ownership",
          "[vfs][prepared_mount][lifetime]") {
  PreparedMountRoot root;DeviceRundownObservation observation;
  VirtualFileSystem fs;
  auto device=AdmittedFixtureDevice(root,observation);
  auto* exact=device.get();std::error_code error;
  auto prepared=fs.PrepareDeviceRegistration(device,error);
  REQUIRE(prepared);REQUIRE_FALSE(device);REQUIRE_FALSE(error);
  std::shared_ptr<Device> published;
  {
    auto namespace_lock=xe::global_critical_region::AcquireDirect();
    // A native directory Capture would refuse this inherited GCRC. The real
    // capsule permits metadata publication without doing that admission again.
    CHECK(fs.RegisterPreparedDeviceExclusive(*prepared,published)==X_STATUS_SUCCESS);
    CHECK(published.get()==exact);
    CHECK(fs.GetDevice("Prepared:")==exact);
    CHECK_FALSE(fs.RegisterPreparedDevice(*prepared));
    CHECK(observation.destroyed==0);
  }
  prepared.reset();published.reset();fs.Clear();
  CHECK(observation.destroyed==1);CHECK_FALSE(observation.under_namespace);
}
TEST_CASE("Failed prepared device publication retains native owners outside namespace locks",
          "[vfs][prepared_mount][failure]") {
  PreparedMountRoot root;DeviceRundownObservation first_observation,second_observation;
  VirtualFileSystem fs,foreign;
  auto first=AdmittedFixtureDevice(root,first_observation);
  REQUIRE(fs.RegisterDevice(std::move(first)));
  auto second=AdmittedFixtureDevice(root,second_observation);
  std::error_code error;
  auto prepared=fs.PrepareDeviceRegistration(second,error);REQUIRE(prepared);
  std::shared_ptr<Device> result;
  {
    auto namespace_lock=xe::global_critical_region::AcquireDirect();
    CHECK_FALSE(foreign.RegisterPreparedDevice(*prepared));
    CHECK(fs.RegisterPreparedDeviceExclusive(*prepared,result)==X_STATUS_OBJECT_NAME_COLLISION);
    CHECK_FALSE(result);CHECK(second_observation.destroyed==0);
  }
  prepared.reset();
  CHECK(second_observation.destroyed==1);CHECK_FALSE(second_observation.under_namespace);
  CHECK(first_observation.destroyed==0);
  fs.Clear();CHECK(first_observation.destroyed==1);CHECK_FALSE(first_observation.under_namespace);
}
TEST_CASE("Refused device admission preserves the caller and permits later real preparation",
          "[vfs][prepared_mount][admission]") {
  PreparedMountRoot root;DeviceRundownObservation observation;VirtualFileSystem fs;
  auto device=AdmittedFixtureDevice(root,observation);auto* exact=device.get();
  std::error_code error;
  {
    auto namespace_lock=xe::global_critical_region::AcquireDirect();
    CHECK_FALSE(fs.PrepareDeviceRegistration(device,error));
    CHECK(error==std::errc::device_or_resource_busy);
    CHECK(device.get()==exact);CHECK(observation.destroyed==0);
  }
  auto prepared=fs.PrepareDeviceRegistration(device,error);
  REQUIRE(prepared);REQUIRE_FALSE(error);REQUIRE_FALSE(device);
  CHECK(fs.GetDevice("Prepared:")==nullptr);
  prepared.reset();CHECK(observation.destroyed==1);CHECK_FALSE(observation.under_namespace);
}


namespace {
class DirectoryPinnedManagedLeaf final : public ManagedFileService {
 public:
  explicit DirectoryPinnedManagedLeaf(
      std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> directory)
      : directory_(std::move(directory)),path_(directory_->path()/"Account") {}
  const std::filesystem::path& backing_path() const override { return path_; }
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const override { return directory_; }
  bool retiring() const override { return false; }
  X_STATUS Open(std::shared_ptr<Device>,std::string,FileDisposition,uint32_t,
                bool,File**,FileAction*) override { return X_STATUS_NOT_IMPLEMENTED; }
 private:
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> directory_;
  std::filesystem::path path_;
};
}
TEST_CASE("Prepared device replaces the last native pinned registry before namespace publication",
          "[vfs][prepared_mount][registry_rundown]") {
  PreparedMountRoot root,old_registry_root;
  DeviceRundownObservation observation;VirtualFileSystem fs;
  auto device=AdmittedFixtureDevice(root,observation);
  std::error_code error;
  auto native_directory=filesystem::HostStorageDirectoryIdentity::Capture(
      old_registry_root.path,error);
  REQUIRE(native_directory);REQUIRE_FALSE(error);
  std::weak_ptr<const filesystem::HostStorageDirectoryIdentity> old_directory=native_directory;
  auto prior=std::make_shared<ManagedFileRegistry>();
  std::weak_ptr<ManagedFileRegistry> old_registry=prior;
  auto leaf=std::make_shared<DirectoryPinnedManagedLeaf>(std::move(native_directory));
  REQUIRE(prior->Register(leaf));
  device->SetManagedFileRegistry(prior);
  leaf.reset();prior.reset();
  // The old registry's tombstone row is now the ONLY owner of a real separate
  // directory identity/pin. The device is the ONLY owner of that registry.
  REQUIRE_FALSE(old_registry.expired());REQUIRE_FALSE(old_directory.expired());
  auto prepared=fs.PrepareDeviceRegistration(device,error);
  REQUIRE(prepared);REQUIRE_FALSE(error);
  // Last native pin rundown happened during the genuine out-of-lock prepare,
  // before any inherited namespace ownership or metadata publication.
  CHECK(old_registry.expired());CHECK(old_directory.expired());
  std::shared_ptr<Device> published;
  {
    auto namespace_lock=xe::global_critical_region::AcquireDirect();
    CHECK(fs.RegisterPreparedDeviceExclusive(*prepared,published)==X_STATUS_SUCCESS);
    CHECK(old_registry.expired());CHECK(old_directory.expired());
    CHECK(observation.destroyed==0);
  }
  prepared.reset();published.reset();fs.Clear();
  CHECK(observation.destroyed==1);CHECK_FALSE(observation.under_namespace);
}

}  // namespace xe::vfs::test
#endif
