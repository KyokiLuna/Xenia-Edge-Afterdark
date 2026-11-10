#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/host_storage.h"
#include "xenia/base/mutex.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/virtual_file_system.h"
#if !XE_PLATFORM_WIN32
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if XE_PLATFORM_LINUX
#include <sched.h>
#include <sys/mount.h>
#endif
extern char** environ;
#endif
#if !XE_PLATFORM_WIN32
namespace xe::vfs::test {
namespace {
using filesystem::GuestHostBacking;
using filesystem::HostStorageRoot;
struct Scratch {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xenia-console-protection-" + std::to_string(std::random_device{}()));
  Scratch() {
    REQUIRE(std::filesystem::create_directory(path));
    REQUIRE(std::filesystem::create_directory(path / "private"));
    REQUIRE(std::filesystem::create_directory(path / "public"));
    std::filesystem::permissions(path / "private",
                                 std::filesystem::perms::owner_all);
  }
  ~Scratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::shared_ptr<HostStorageRoot> Root(std::string_view child) {
    std::error_code error;
    auto root = HostStorageRoot::Open(path / child, false, error, 0);
    REQUIRE(root);
    return root;
  }
};
std::shared_ptr<HostStorageRoot> Protect(
    const std::shared_ptr<HostStorageRoot>& raw) {
  std::error_code error;
  auto root = raw->ProtectFlat(error);
#if XE_PLATFORM_LINUX
  REQUIRE(root);
  REQUIRE_FALSE(error);
#else
  REQUIRE_FALSE(root);
  REQUIRE(error == std::errc::operation_not_supported);
  WARN(
      "UNVERIFIED: protected mount-coordinate proof is unsupported on this "
      "platform");
#endif
  return root;
}
void New(const std::shared_ptr<HostStorageRoot>& root, const char* name,
         uint8_t byte) {
  REQUIRE_FALSE(root->WriteNewFile(name, std::array<uint8_t, 1>{byte}).error);
}
class UnknownDevice final : public NullDevice {
 public:
  UnknownDevice() : NullDevice("unknown:", {"Cache0"}) {}
  std::optional<std::vector<GuestHostBacking>> GuestBackings() const override {
    return std::nullopt;
  }
};
}  // namespace
TEST_CASE(
    "Protected flat authority rejects ancestor exact descendant and unknown "
    "mounts",
    "[vfs][console_authority]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  auto authority = Protect(raw);
  if (!authority) {
    return;
  }
  REQUIRE(authority->is_guest_private());
  New(authority, "xconfig.bin", 42);
  std::error_code error;
  for (const auto& path : {scratch.path, scratch.path / "private",
                           scratch.path / "private" / "future"}) {
    REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
        std::vector<GuestHostBacking>{{path, true}}, error));
    REQUIRE(error == std::errc::permission_denied);
  }
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "private" / "future"));
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(std::nullopt, error));
  REQUIRE(error == std::errc::operation_not_supported);
  auto ordinary = scratch.Root("public");
  auto guest = ordinary->GuestView(error);
  REQUIRE(guest);
  New(ordinary, "ordinary", 7);
  auto read =
      guest->OpenFile("ordinary", filesystem::FileAccess::kFileReadData, error);
  REQUIRE(read);
  read.reset();
  // Guest admission owns the protected dependency even after its explicit
  // console owner is released; dropping the store cannot expose its old bytes.
  authority.reset();
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{scratch.path, true}}, error));
  REQUIRE(error == std::errc::permission_denied);
}
TEST_CASE(
    "Private admission fails around preexisting broad or unknown guest scopes",
    "[vfs][console_authority]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  std::error_code error;
  auto broad = HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{scratch.path, true}}, error);
  REQUIRE(broad);
  REQUIRE_FALSE(raw->ProtectFlat(error));
#if XE_PLATFORM_LINUX
  REQUIRE(error == std::errc::permission_denied);
#else
  REQUIRE(error == std::errc::operation_not_supported);
#endif
  broad.reset();
  auto unknown = HostStorageRoot::AdmitGuestMount(std::nullopt, error);
  REQUIRE(unknown);
  REQUIRE_FALSE(raw->ProtectFlat(error));
  unknown.reset();
  auto authority = Protect(raw);
  if (!authority) {
    return;
  }
  // Component-aware overlap must not confuse a neighboring spelling.
  REQUIRE(std::filesystem::create_directory(scratch.path / "private-sibling"));
  REQUIRE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{scratch.path / "private-sibling", true}},
      error));
}
TEST_CASE(
    "External authority hardlinks deny guest content and preserve private "
    "generations",
    "[vfs][console_authority][hardlink]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  auto authority = Protect(raw);
  if (!authority) {
    return;
  }
  New(authority, "xconfig.lock", 0);
  New(authority, "xconfig.bin", 11);
  auto public_root = scratch.Root("public");
  std::error_code error;
  auto guest = public_root->GuestView(error);
  REQUIRE(guest);
  // External creation after both authority and public source were admitted.
  std::filesystem::create_hard_link(scratch.path / "private" / "xconfig.bin",
                                    scratch.path / "public" / "alias");
  REQUIRE_FALSE(
      guest->OpenFile("alias", filesystem::FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{scratch.path / "public" / "alias", false}},
      error));
  REQUIRE(error == std::errc::permission_denied);
  // Unlinking/rotating a private generation while an outside alias survives
  // would erase its membership evidence. Retain it and report failure instead.
  REQUIRE(authority->Remove("xconfig.bin", false) ==
          std::errc::permission_denied);
  REQUIRE(authority->Rename("xconfig.bin", "previous", false) ==
          std::errc::permission_denied);
  REQUIRE(authority->WriteNewFile("stage", std::array<uint8_t, 1>{22}).error ==
          std::errc::permission_denied);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(raw->ReadFile("xconfig.bin", 4, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{11});
  // Unrelated hardlinks remain readable through an admitted source.
  New(public_root, "unrelated", 9);
  std::filesystem::create_hard_link(scratch.path / "public" / "unrelated",
                                    scratch.path / "public" / "unrelated-link");
  REQUIRE_FALSE(guest->ReadFile("unrelated-link", 4, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{9});
  std::filesystem::remove(scratch.path / "public" / "alias");
  New(authority, "stage", 22);
  const auto exported = authority->AtomicReplaceFile(
      "stage", "xconfig.bin", public_root, "outside-copy");
  REQUIRE(exported.error == std::errc::permission_denied);
  REQUIRE_FALSE(exported.published);
  REQUIRE_FALSE(exported.backup_created);
  REQUIRE_FALSE(
      std::filesystem::exists(scratch.path / "public" / "outside-copy"));
  REQUIRE(std::filesystem::exists(scratch.path / "private" / "stage"));
  REQUIRE_FALSE(
      authority->AtomicReplaceFile("stage", "xconfig.bin", "previous").error);
  REQUIRE_FALSE(raw->ReadFile("previous", 4, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{11});
  std::filesystem::create_hard_link(scratch.path / "private" / "previous",
                                    scratch.path / "public" / "previous-alias");
  REQUIRE(authority->Remove("previous", false) == std::errc::permission_denied);
  REQUIRE_FALSE(guest->OpenFile("previous-alias",
                                filesystem::FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::permission_denied);
}
TEST_CASE(
    "Flat authority preserves internal recovery links and rejects unknown "
    "nonflat content",
    "[vfs][console_authority][confinement]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  New(raw, "xconfig.bin", 1);
  std::filesystem::create_hard_link(scratch.path / "private" / "xconfig.bin",
                                    scratch.path / "private" / "previous");
  std::filesystem::create_hard_link(
      scratch.path / "private" / "xconfig.bin",
      scratch.path / "public" / "outside-before-admission");
  std::error_code initial_error;
  REQUIRE_FALSE(raw->ProtectFlat(initial_error));
  REQUIRE(initial_error == std::errc::permission_denied);
  REQUIRE(std::filesystem::exists(scratch.path / "public" /
                                  "outside-before-admission"));
  std::filesystem::remove(scratch.path / "public" / "outside-before-admission");
  auto authority = Protect(raw);
  if (!authority) {
    return;
  }
  std::error_code error;
  REQUIRE(authority->Create("child", true) == std::errc::permission_denied);
  REQUIRE_FALSE(authority->Subroot("child", false, error));
  auto guest = scratch.Root("public")->GuestView(error);
  REQUIRE(guest);
  std::ofstream(scratch.path / "public" / "plain") << "public";
  REQUIRE(
      std::filesystem::create_directory(scratch.path / "private" / "unknown"));
  std::ofstream(scratch.path / "private" / "unknown" / "preserve") << "unknown";
  auto unrelated =
      guest->OpenFile("plain", filesystem::FileAccess::kFileReadData, error);
  REQUIRE(unrelated);
  unrelated.reset();
  // Unknown private subtrees fail authority writes, while unrelated normal
  // single-link guest data remains usable.
  std::filesystem::create_hard_link(scratch.path / "private" / "xconfig.bin",
                                    scratch.path / "public" / "private-alias");
  REQUIRE_FALSE(guest->OpenFile("private-alias",
                                filesystem::FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE(authority->WriteNewFile("stage", std::array<uint8_t, 1>{2}).error ==
          std::errc::permission_denied);
  REQUIRE(std::filesystem::exists(scratch.path / "private" / "unknown" /
                                  "preserve"));
  REQUIRE(std::filesystem::file_size(scratch.path / "private" /
                                     "xconfig.bin") == 1);
}
TEST_CASE(
    "Private admission preserves symbolic links without reading their targets",
    "[vfs][console_authority][confinement]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  std::ofstream(scratch.path / "public" / "outside") << "untouched";
  std::filesystem::create_symlink(scratch.path / "public" / "outside",
                                  scratch.path / "private" / "unknown-link");
  std::error_code error;
  REQUIRE_FALSE(raw->ProtectFlat(error));
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  REQUIRE(error == std::errc::permission_denied);
#else
  REQUIRE(error == std::errc::operation_not_supported);
#endif
  REQUIRE(
      std::filesystem::is_symlink(scratch.path / "private" / "unknown-link"));
  REQUIRE(std::filesystem::file_size(scratch.path / "public" / "outside") == 9);
}
TEST_CASE(
    "Common VFS admission rejects broad custom devices before private child "
    "creation",
    "[vfs][console_authority][mount]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  auto authority = Protect(raw);
  if (!authority) {
    return;
  }
  New(authority, "xconfig.bin", 4);
  VirtualFileSystem fs;
  auto broad = std::make_unique<HostPathDevice>("host:", scratch.path, false);
  REQUIRE_FALSE(broad->Initialize());
  auto future = std::make_unique<HostPathDevice>(
      "future:", scratch.path / "private" / "future", false);
  REQUIRE_FALSE(future->Initialize());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "private" / "future"));
  auto unknown = std::make_unique<UnknownDevice>();
  REQUIRE(unknown->Initialize());
  REQUIRE_FALSE(fs.RegisterDevice(std::move(unknown)));
  auto raw_guest = std::make_unique<NullDevice>(
      "raw:", std::initializer_list<std::string>{"Cache0"});
  REQUIRE(raw_guest->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(raw_guest)));
  std::filesystem::create_hard_link(scratch.path / "private" / "xconfig.bin",
                                    scratch.path / "public" / "private-alias");
  std::filesystem::create_directory_symlink(
      scratch.path / "private", scratch.path / "public" / "private-directory");
  auto safe =
      std::make_unique<HostPathDevice>("safe:", scratch.path / "public", false);
  REQUIRE(safe->Initialize());
  REQUIRE_FALSE(safe->ResolvePath("private-alias"));
  REQUIRE_FALSE(safe->ResolvePath("private-directory"));
  REQUIRE(fs.RegisterDevice(std::move(safe)));
  fs.Clear();
  REQUIRE(authority->Observe().available);
}
TEST_CASE(
    "Flat admission is bounded and nonwaiting under existing filesystem "
    "contention",
    "[vfs][console_authority][admission]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  std::error_code error;
  auto space =
      filesystem::HostStorageSpace::ForPath(scratch.path / "private", 0, error);
  REQUIRE(space);
  auto held = space->Begin(scratch.path / "private", error);
  REQUIRE(held);
  auto outer = global_critical_region::AcquireDirect();
  REQUIRE_FALSE(raw->ProtectFlat(error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(global_critical_region::is_held_by_current_thread());
  outer.unlock();
  held.reset();
  for (unsigned i = 0; i < 65; ++i) {
    const auto name =
        scratch.path / "private" / ("unknown-" + std::to_string(i));
    std::ofstream(name) << "x";
    std::filesystem::permissions(name, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
  }
  REQUIRE_FALSE(raw->ProtectFlat(error));
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  REQUIRE(error == std::errc::value_too_large);
#else
  REQUIRE(error == std::errc::operation_not_supported);
#endif
  REQUIRE(std::filesystem::exists(scratch.path / "private" / "unknown-64"));
}
TEST_CASE(
    "Guest archive admission rejects path replacement of a retained backing "
    "handle",
    "[vfs][console_authority][lifetime]") {
  Scratch scratch;
  auto raw = scratch.Root("private");
  auto authority = Protect(raw);
  if (!authority) {
    return;
  }
  New(authority, "xconfig.bin", 1);
  auto public_root = scratch.Root("public");
  New(public_root, "fragment", 3);
  std::error_code error;
  auto opened = public_root->OpenFile(
      "fragment", filesystem::FileAccess::kFileReadData, error);
  REQUIRE(opened);
  const auto identity = opened->QueryInfo(error);
  REQUIRE(identity);
  std::filesystem::rename(scratch.path / "public" / "fragment",
                          scratch.path / "public" / "old-fragment");
  New(public_root, "fragment", 4);
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{
          {scratch.path / "public" / "fragment", false, *identity}},
      error));
  REQUIRE(error == std::errc::state_not_recoverable);
  std::array<uint8_t, 1> bytes{};
  const auto read = opened->ReadResult(0, bytes);
  REQUIRE_FALSE(read.error);
  REQUIRE(read.bytes == 1);
  REQUIRE(bytes[0] == 3);
  REQUIRE(std::filesystem::file_size(scratch.path / "public" / "fragment") ==
          1);
}
#if !XE_PLATFORM_WIN32
TEST_CASE("Protected console bind-alias namespace fixture",
          "[console_protection_mount_probe]") {
  const char* requested = std::getenv("XENIA_CONSOLE_PROTECTION_PROBE");
  if (!requested) {
    SUCCEED("Helper only; no namespace coverage claimed");
    return;
  }
  const std::filesystem::path path = requested;
#if XE_PLATFORM_LINUX
  const auto uid = getuid(), gid = getgid();
  auto unavailable = [&](const char* reason) {
    std::ofstream(path / "UNVERIFIED") << reason << ":" << errno;
  };
  if (unshare(CLONE_NEWUSER | CLONE_NEWNS)) {
    unavailable("unshare");
    return;
  }
  {
    std::ofstream deny("/proc/self/setgroups");
    if (deny) {
      deny << "deny";
    }
  }
  // Keep the same numeric owner so the fixed per-user host storage lock root
  // remains the same authority inside and outside this private user namespace.
  {
    std::ofstream map("/proc/self/uid_map");
    map << uid << " " << uid << " 1\n";
    map.flush();
    if (!map) {
      unavailable("uid map");
      return;
    }
  }
  {
    std::ofstream map("/proc/self/gid_map");
    map << gid << " " << gid << " 1\n";
    map.flush();
    if (!map) {
      unavailable("gid map");
      return;
    }
  }
  if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr)) {
    unavailable("private propagation");
    return;
  }
  std::filesystem::create_directories(path / "private");
  std::filesystem::create_directories(path / "public");
  std::filesystem::permissions(path / "private",
                               std::filesystem::perms::owner_all);
  std::error_code error;
  auto raw = HostStorageRoot::Open(path / "private", false, error, 0);
  REQUIRE(raw);
  auto authority = raw->ProtectFlat(error);
  REQUIRE(authority);
  New(authority, "xconfig.bin", 8);
  std::filesystem::create_directory(path / "public" / "bound");
  if (mount((path / "private").c_str(), (path / "public" / "bound").c_str(),
            nullptr, MS_BIND, nullptr)) {
    unavailable("directory bind");
    return;
  }
  struct stat original{}, alias{};
  REQUIRE(stat((path / "private").c_str(), &original) == 0);
  REQUIRE(stat((path / "public" / "bound").c_str(), &alias) == 0);
  REQUIRE(original.st_dev == alias.st_dev);
  REQUIRE(original.st_ino == alias.st_ino);
  auto bound = HostStorageRoot::Open(path / "public" / "bound", true, error, 0);
  REQUIRE(bound);
  REQUIRE_FALSE(bound->GuestView(error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{path / "public" / "bound", true}}, error));
  REQUIRE(error == std::errc::permission_denied);
  bound.reset();
  REQUIRE(umount2((path / "public" / "bound").c_str(), MNT_DETACH) == 0);
  std::ofstream(path / "public" / "bound-file") << "unchanged";
  if (mount((path / "private" / "xconfig.bin").c_str(),
            (path / "public" / "bound-file").c_str(), nullptr, MS_BIND,
            nullptr)) {
    unavailable("file bind");
    return;
  }
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{path / "public" / "bound-file", false}},
      error));
  REQUIRE((error == std::errc::cross_device_link ||
          error == std::errc::permission_denied));
  REQUIRE(umount2((path / "public" / "bound-file").c_str(), MNT_DETACH) == 0);
  REQUIRE(std::filesystem::file_size(path / "public" / "bound-file") == 9);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(raw->ReadFile("xconfig.bin", 4, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{8});
  std::ofstream(path / "VERIFIED")
      << "same-FS directory and file bind refused; originals preserved";
#else
  std::ofstream(path / "UNVERIFIED")
      << "mount-coordinate proof/fixture unavailable on this platform";
#endif
}
TEST_CASE(
    "Protected authority refuses mounted aliases with explicit fixture status",
    "[vfs][console_authority][mount]") {
  Scratch scratch;
  const auto executable = filesystem::GetExecutablePath();
  std::vector<std::string> variables;
  for (char** p = environ; *p; ++p) {
    if (std::string_view(*p).find("XENIA_CONSOLE_PROTECTION_PROBE=") != 0) {
      variables.emplace_back(*p);
    }
  }
  variables.emplace_back("XENIA_CONSOLE_PROTECTION_PROBE=" +
                         scratch.path.string());
  std::vector<char*> environment;
  for (auto& value : variables) {
    environment.push_back(value.data());
  }
  environment.push_back(nullptr);
  std::string binary = executable.string(),
              filter = "[console_protection_mount_probe]";
  char* arguments[] = {binary.data(), filter.data(), nullptr};
  pid_t child = 0;
  REQUIRE(posix_spawn(&child, binary.c_str(), nullptr, nullptr, arguments,
                      environment.data()) == 0);
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  if (std::filesystem::exists(scratch.path / "UNVERIFIED")) {
    std::ifstream reason(scratch.path / "UNVERIFIED");
    std::string text;
    std::getline(reason, text);
    WARN("UNVERIFIED mount-alias acceptance: " << text);
    return;
  }
  REQUIRE(std::filesystem::exists(scratch.path / "VERIFIED"));
}
#endif
}  // namespace xe::vfs::test
#endif  // !XE_PLATFORM_WIN32
