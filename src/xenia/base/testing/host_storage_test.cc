#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "xenia/base/host_storage.h"
#include <array>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include "third_party/catch/include/catch.hpp"
#if XE_PLATFORM_MAC
#include <fcntl.h>
#include "xenia/base/managed_device_identity.h"
#include "xenia/base/profile_intent_owner.h"
#include "xenia/base/snapshot_publisher.h"
#endif
#if !XE_PLATFORM_WIN32
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if XE_PLATFORM_LINUX
#include <sched.h>
#include <sys/mount.h>
#endif
extern char** environ;
namespace xe::filesystem::test {
namespace {
struct Scratch {
  std::filesystem::path path;
  Scratch() {
    std::random_device rng;
    for (unsigned i = 0; i < 16; ++i) {
      path = std::filesystem::temp_directory_path() /
             ("xenia-policy-" + std::to_string(rng()) + "-" +
              std::to_string(rng()));
      if (std::filesystem::create_directory(path)) {
        return;
      }
    }
    FAIL("Cannot create private scratch directory");
  }
  ~Scratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::shared_ptr<HostStorageRoot> Root(bool readonly = false) {
    std::error_code error;
    auto space =
        HostStorageSpace::ForPath(path, 0, error, [](const auto&, auto&) {
          return std::filesystem::space_info{1ull << 40, 1ull << 30,
                                             1ull << 30};
        });
    auto root = HostStorageRoot::Open(path, readonly, error, 0, space);
    REQUIRE_FALSE(error);
    REQUIRE(root);
    return root;
  }
};
#if XE_PLATFORM_MAC
// Use genuine filesystem authority and the common 20 GiB reserve. Scratch::Root
// deliberately uses an isolated query and cannot establish private ownership.
std::shared_ptr<HostStorageRoot> DarwinNativeRoot(
    const std::filesystem::path& path) {
  std::error_code error;
  auto root = HostStorageRoot::Open(path, false, error);
  REQUIRE_FALSE(error);
  REQUIRE(root);
  return root;
}
struct DarwinDirectoryFd {
  int value;
  explicit DarwinDirectoryFd(const std::filesystem::path& path)
      : value(open(path.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) {}
  DarwinDirectoryFd(const DarwinDirectoryFd&) = delete;
  DarwinDirectoryFd& operator=(const DarwinDirectoryFd&) = delete;
  ~DarwinDirectoryFd() {
    if (value >= 0) {
      close(value);
    }
  }
};
#endif
}  // namespace
#if XE_PLATFORM_MAC
TEST_CASE(
    "Darwin managed identity uses the actual directory and reopens durably",
    "[storage_policy][darwin][managed_identity]") {
  Scratch scratch;
  auto parent = DarwinNativeRoot(scratch.path);
  REQUIRE_FALSE(parent->Create("managed", true));
  std::error_code error;
  auto source = parent->Subroot("managed", false, error);
  REQUIRE_FALSE(error);
  REQUIRE(source);
  auto first = ManagedDeviceIdentity::Open(source, "darwin-test", 1, error);
  REQUIRE_FALSE(error);
  REQUIRE(first);
  DarwinDirectoryFd directory(source->path());
  REQUIRE(directory.value >= 0);
  struct stat native{};
  REQUIRE(fstat(directory.value, &native) == 0);
  REQUIRE(first->directory_file_id() == uint64_t(native.st_ino));
  REQUIRE_FALSE(first->CheckCurrent());
  const auto uuid = first->uuid();
  const auto inode = first->directory_file_id();
  first.reset();
  std::vector<uint8_t> marker;
  REQUIRE_FALSE(
      source->ReadFile(ManagedDeviceIdentity::kMarker, 65536, marker));
  REQUIRE_FALSE(marker.empty());
  auto fresh = DarwinNativeRoot(source->path());
  auto reopened = ManagedDeviceIdentity::Open(fresh, "darwin-test", 1, error);
  REQUIRE_FALSE(error);
  REQUIRE(reopened);
  REQUIRE(reopened->uuid() == uuid);
  REQUIRE(reopened->directory_file_id() == inode);
  REQUIRE(reopened->content_epoch() == 1);
  REQUIRE_FALSE(reopened->CheckCurrent());
  REQUIRE_FALSE(parent->Create("copied", true));
  auto copied = parent->Subroot("copied", false, error);
  REQUIRE_FALSE(error);
  REQUIRE(copied);
  REQUIRE_FALSE(
      copied->WriteNewFile(ManagedDeviceIdentity::kMarker, marker).error);
  auto foreign = ManagedDeviceIdentity::Open(copied, "darwin-test", 1, error);
  REQUIRE_FALSE(foreign);
  REQUIRE(error == std::errc::state_not_recoverable);
  std::vector<uint8_t> preserved;
  REQUIRE_FALSE(
      copied->ReadFile(ManagedDeviceIdentity::kMarker, 65536, preserved));
  REQUIRE(preserved == marker);
}

TEST_CASE(
    "Darwin private owners exclude aliases and preserve snapshot recovery",
    "[storage_policy][darwin][snapshot_publication]") {
  Scratch scratch;
  auto parent = DarwinNativeRoot(scratch.path);
  REQUIRE_FALSE(parent->Create("managed", true));
  std::error_code error;
  auto source = parent->Subroot("managed", false, error);
  REQUIRE_FALSE(error);
  REQUIRE(source);
  auto identity = ManagedDeviceIdentity::Open(source, "darwin-test", 1, error);
  REQUIRE_FALSE(error);
  REQUIRE(identity);
  auto intents = ProfileIntentOwner::Open(parent, identity);
  REQUIRE_FALSE(intents->error());
  REQUIRE(intents->root());
  REQUIRE(intents->root()->is_profile_intent_private());
  REQUIRE(intents->source_identity().directory ==
          identity->directory_file_id());
  auto recovery = SnapshotRecoveryOwner::Open(parent);
  REQUIRE_FALSE(recovery->error());
  REQUIRE(recovery->root());
  REQUIRE(recovery->root()->is_recovery_private());
  auto guest = source->GuestView(error);
  REQUIRE_FALSE(error);
  REQUIRE(guest);
  const std::array<uint8_t, 3> old{0, 2, 0xFF}, next{7, 0, 9};
  REQUIRE_FALSE(guest->WriteNewFile("profile.gpd", old).error);
  auto expected = guest->CaptureSnapshot("profile.gpd", 4096, error);
  REQUIRE_FALSE(error);
  REQUIRE(expected);
  const auto written = SnapshotPublisher::Publish(*recovery, expected, next);
  REQUIRE_FALSE(written.error);
  REQUIRE(written.published);
  REQUIRE(written.durable);
  REQUIRE(written.recovery_created);
  REQUIRE(written.recovery_durable);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(guest->ReadFile("profile.gpd", 4096, bytes));
  REQUIRE(bytes == std::vector<uint8_t>(next.begin(), next.end()));
  REQUIRE_FALSE(recovery->root()->ReadFile(written.recovery, 4096, bytes));
  REQUIRE(bytes == std::vector<uint8_t>(old.begin(), old.end()));
  for (const auto& protected_root : {intents->root(), recovery->root()}) {
    const auto alias =
        scratch.path / (protected_root->is_recovery_private() ? "recovery-alias"
                                                              : "intent-alias");
    std::filesystem::create_directory_symlink(protected_root->path(), alias,
                                              error);
    REQUIRE_FALSE(error);
    auto alias_root = DarwinNativeRoot(alias);
    REQUIRE_FALSE(alias_root->GuestView(error));
    REQUIRE(error == std::errc::permission_denied);
    auto raw = protected_root->CaptureSnapshot("owner.v1", 1024, error);
    REQUIRE_FALSE(error);
    REQUIRE(raw);
    REQUIRE(raw->exists());
    REQUIRE_FALSE(raw->bytes().empty());
  }
}

TEST_CASE("Darwin selected root replacement invalidates retained authority",
          "[storage_policy][darwin][retirement]") {
  Scratch scratch;
  auto parent = DarwinNativeRoot(scratch.path);
  REQUIRE_FALSE(parent->Create("selected", true));
  std::error_code error;
  auto selected = parent->Subroot("selected", false, error);
  REQUIRE_FALSE(error);
  REQUIRE(selected);
  const std::array<uint8_t, 2> payload{4, 5};
  REQUIRE_FALSE(selected->WriteNewFile("data", payload).error);
  auto identity =
      ManagedDeviceIdentity::Open(selected, "darwin-test", 1, error);
  REQUIRE_FALSE(error);
  REQUIRE(identity);
  auto guest = selected->GuestView(error);
  REQUIRE_FALSE(error);
  REQUIRE(guest);
  auto held = guest->OpenFile("data", FileAccess::kGenericRead, error);
  REQUIRE_FALSE(error);
  REQUIRE(held);
  const auto old_directory = identity->directory_file_id();
  const auto moved = scratch.path / "retained-old";
  std::filesystem::rename(selected->path(), moved, error);
  REQUIRE_FALSE(error);
  REQUIRE(std::filesystem::create_directory(scratch.path / "selected", error));
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(selected->Observe().available);
  REQUIRE(identity->CheckCurrent());
  std::array<uint8_t, 2> unchanged{0xA5, 0xA5};
  REQUIRE(held->ReadResult(0, unchanged).error == std::errc::no_such_device);
  REQUIRE(unchanged == std::array<uint8_t, 2>{0xA5, 0xA5});
  held.reset();
  guest.reset();
  identity.reset();
  std::filesystem::copy_file(
      moved / ManagedDeviceIdentity::kMarker,
      scratch.path / "selected" / ManagedDeviceIdentity::kMarker, error);
  REQUIRE_FALSE(error);
  auto replacement = DarwinNativeRoot(scratch.path / "selected");
  DarwinDirectoryFd directory(replacement->path());
  REQUIRE(directory.value >= 0);
  struct stat native{};
  REQUIRE(fstat(directory.value, &native) == 0);
  REQUIRE(uint64_t(native.st_ino) != old_directory);
  std::vector<uint8_t> marker;
  REQUIRE_FALSE(
      replacement->ReadFile(ManagedDeviceIdentity::kMarker, 65536, marker));
  auto rejected =
      ManagedDeviceIdentity::Open(replacement, "darwin-test", 1, error);
  REQUIRE_FALSE(rejected);
  REQUIRE(error == std::errc::state_not_recoverable);
  std::vector<uint8_t> preserved;
  REQUIRE_FALSE(
      replacement->ReadFile(ManagedDeviceIdentity::kMarker, 65536, preserved));
  REQUIRE(preserved == marker);
  REQUIRE(std::filesystem::exists(moved / "data"));
}
#endif
TEST_CASE("Storage reads are bounded regular-file reads with preserved output",
          "[storage_policy]") {
  Scratch scratch;
  auto root = scratch.Root();
  const std::array<uint8_t, 3> data{1, 2, 3};
  REQUIRE_FALSE(root->WriteNewFile("Account", data).error);
  std::vector<uint8_t> out{9};
  REQUIRE(root->ReadFile("Account", 2, out));
  REQUIRE(out == std::vector<uint8_t>{9});
  REQUIRE_FALSE(root->ReadFile("Account", 3, out));
  REQUIRE(out == std::vector<uint8_t>{1, 2, 3});
  REQUIRE(mkfifo((scratch.path / "pipe").c_str(), 0600) == 0);
  REQUIRE(root->ReadFile("pipe", 1024, out));
  REQUIRE(out == std::vector<uint8_t>{1, 2, 3});
}
TEST_CASE("Storage capabilities prevent symlink and hardlink mutation escapes",
          "[storage_policy]") {
  Scratch scratch;
  std::filesystem::create_directory(scratch.path / "owned");
  std::ofstream(scratch.path / "outside") << "original";
  std::error_code error;
  auto root = HostStorageRoot::Open(scratch.path / "owned", false, error, 0);
  REQUIRE(root);
  std::filesystem::create_symlink(scratch.path / "outside",
                                  scratch.path / "owned" / "escape");
  REQUIRE_FALSE(root->OpenFile("escape", FileAccess::kFileWriteData, error));
  REQUIRE(error);
  std::filesystem::create_hard_link(scratch.path / "outside",
                                    scratch.path / "owned" / "linked");
  auto linked = root->OpenFile("linked", FileAccess::kFileWriteData, error);
  REQUIRE(linked);
  const std::array<uint8_t, 1> bytes{'x'};
  REQUIRE(linked->WriteResult(0, bytes).error == std::errc::permission_denied);
  std::ifstream original(scratch.path / "outside");
  std::string text;
  original >> text;
  REQUIRE(text == "original");
  REQUIRE(root->Create("../escape", true));
  REQUIRE(root->Remove({}, true));
}
TEST_CASE(
    "Mount retirement is local while source retirement reaches every child",
    "[storage_policy]") {
  Scratch scratch;
  auto source = scratch.Root();
  const std::array<uint8_t, 1> bytes{42};
  REQUIRE_FALSE(source->WriteNewFile("data", bytes).error);
  std::error_code error;
  auto first = source->Subroot({}, false, error);
  auto second = source->Subroot({}, false, error);
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(first->path() == source->path());
  REQUIRE(second->path() == source->path());
  auto guest = source->GuestView(error);
  REQUIRE(guest);
  REQUIRE(guest->path() == source->path());
  REQUIRE(guest.get() != source.get());
  std::vector<uint8_t> guest_bytes;
  REQUIRE_FALSE(guest->ReadFile("data", 1, guest_bytes));
  REQUIRE(guest_bytes == std::vector<uint8_t>{42});
  guest->Retire();
  REQUIRE_FALSE(guest->Observe().available);
  REQUIRE(source->Observe().available);
  REQUIRE(first->Observe().available);
  auto a = first->OpenFile("data", FileAccess::kFileReadData, error);
  auto b = second->OpenFile("data", FileAccess::kFileReadData, error);
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(source->open_file_count() == 2);
  first->Retire();
  std::array<uint8_t, 1> out{};
  REQUIRE(a->ReadResult(0, out).error == std::errc::no_such_device);
  REQUIRE_FALSE(b->ReadResult(0, out).error);
  REQUIRE(source->Observe().available);
  source->Retire();
  REQUIRE(b->ReadResult(0, out).error == std::errc::no_such_device);
  a.reset();
  b.reset();
  REQUIRE(source->open_file_count() == 0);
}
TEST_CASE("Readonly views never gain rename resize or writable-file access",
          "[storage_policy]") {
  Scratch scratch;
  auto source = scratch.Root();
  const std::array<uint8_t, 1> bytes{42};
  REQUIRE_FALSE(source->WriteNewFile("data", bytes).error);
  std::error_code error;
  auto readonly = source->Subroot({}, true, error);
  REQUIRE(readonly);
  auto file = readonly->OpenFile("data", FileAccess::kGenericWrite, error);
  REQUIRE(file);
  REQUIRE(file->WriteResult(0, bytes).error ==
          std::errc::read_only_file_system);
  REQUIRE(file->ResizeResult(0) == std::errc::read_only_file_system);
  REQUIRE(readonly->Rename("data", "other", false) ==
          std::errc::read_only_file_system);
  REQUIRE(readonly->Remove("data", false) == std::errc::read_only_file_system);
  auto narrowed = readonly->Subroot({}, false, error);
  REQUIRE(narrowed->read_only());
}
TEST_CASE("One live allowance serializes concurrent growth across views",
          "[storage_policy]") {
  Scratch scratch;
  std::error_code error;
  constexpr uint64_t reserve = 1024, payload = 4096, metadata = 64 * 1024;
  auto space = HostStorageSpace::ForPath(
      scratch.path, reserve, error, [&](const auto& path, auto&) {
        uint64_t used = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
          if (entry.is_regular_file()) {
            used += entry.file_size();
          }
        }
        return std::filesystem::space_info{1ull << 30, 1ull << 30,
                                           reserve + metadata + payload - used};
      });
  auto root = HostStorageRoot::Open(scratch.path, false, error, reserve, space);
  REQUIRE(root);
  auto first = root->OpenFile("a", FileAccess::kFileWriteData, error, true);
  REQUIRE(first);
  auto view = root->Subroot({}, false, error);
  auto second = view->OpenFile("b", FileAccess::kFileWriteData, error, true);
  REQUIRE(second);
  std::array<uint8_t, payload> bytes{};
  FileIoResult a, b;
  std::barrier ready(3);
  std::thread one([&] {
    ready.arrive_and_wait();
    a = first->WriteResult(0, bytes);
  });
  std::thread two([&] {
    ready.arrive_and_wait();
    b = second->WriteResult(0, bytes);
  });
  ready.arrive_and_wait();
  one.join();
  two.join();
  REQUIRE(a.bytes + b.bytes == payload);
  REQUIRE(bool(a.error) != bool(b.error));
  REQUIRE((a.error ? a.error : b.error) == std::errc::no_space_on_device);
  REQUIRE(root->Observe().available_bytes >= reserve);
}
TEST_CASE(
    "Atomic publication retains the original and distinguishes committed state",
    "[storage_policy]") {
  Scratch scratch;
  auto root = scratch.Root();
  const std::array<uint8_t, 3> old{'o', 'l', 'd'}, next{'n', 'e', 'w'};
  REQUIRE_FALSE(root->WriteNewFile("Account", old).error);
  REQUIRE_FALSE(root->WriteNewFile("staged", next).error);
  auto refused = root->AtomicReplaceFile("staged", "Account");
  REQUIRE(refused.error);
  REQUIRE_FALSE(refused.published);
  auto result =
      root->AtomicReplaceFile("staged", "Account", "Account.original");
  REQUIRE_FALSE(result.error);
  REQUIRE(result.published);
  REQUIRE(result.backup_created);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(root->ReadFile("Account.original", 3, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{'o', 'l', 'd'});
  REQUIRE_FALSE(root->ReadFile("Account", 3, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{'n', 'e', 'w'});
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "staged"));
}
TEST_CASE("Space query failure fails closed and still permits cleanup",
          "[storage_policy]") {
  Scratch scratch;
  bool fail = false;
  std::error_code error;
  auto space = HostStorageSpace::ForPath(
      scratch.path, 0, error, [&](const auto&, auto& ec) {
        if (fail) {
          ec = std::make_error_code(std::errc::io_error);
        }
        return std::filesystem::space_info{1ull << 30, 1ull << 30, 1ull << 30};
      });
  auto root = HostStorageRoot::Open(scratch.path, false, error, 0, space);
  auto file = root->OpenFile("data", FileAccess::kFileWriteData, error, true);
  REQUIRE(file);
  fail = true;
  const std::array<uint8_t, 1> bytes{1};
  const auto result = file->WriteResult(0, bytes);
  REQUIRE(result.error == std::errc::io_error);
  REQUIRE(result.bytes == 0);
  REQUIRE_FALSE(root->Observe().available);
  file.reset();
  REQUIRE_FALSE(root->Remove("data", false));
}
TEST_CASE("Physical leases cover raw aliases mappings and exact subtrees",
          "[storage_policy]") {
  Scratch scratch;
  std::error_code error;
  auto source = HostStorageRoot::Open(scratch.path, false, error, 0);
  REQUIRE(source);
  auto raw = HostStorageRoot::Open(scratch.path, false, error, 0);
  REQUIRE(raw);
  REQUIRE_FALSE(source->Create("profile", true));
  const std::array<uint8_t, 3> bytes{1, 2, 3};
  REQUIRE_FALSE(source->WriteNewFile("profile/Account", bytes).error);
  auto profile = source->Subroot("profile", false, error);
  REQUIRE(profile);
  auto held =
      raw->OpenFile("profile/Account", FileAccess::kFileReadData, error);
  REQUIRE(held);
  REQUIRE(profile->Remove("Account", false) ==
          std::errc::device_or_resource_busy);
  REQUIRE(source->Remove("profile", true) ==
          std::errc::device_or_resource_busy);
  REQUIRE(profile->Rename("Account", "other", false) ==
          std::errc::device_or_resource_busy);
  auto mapped = raw->MapRead("profile/Account", 0, 0, error);
  REQUIRE(mapped);
  held.reset();
  REQUIRE(profile->Remove("Account", false) ==
          std::errc::device_or_resource_busy);
  mapped.reset();
  REQUIRE_FALSE(profile->Remove("Account", false));
  REQUIRE_FALSE(source->Remove("profile", true));
}
TEST_CASE("Handle rename exempts only the exact initiating lease",
          "[storage_policy]") {
  Scratch scratch;
  auto root = scratch.Root();
  std::error_code error;
  const std::array<uint8_t, 1> bytes{1};
  REQUIRE_FALSE(root->WriteNewFile("data", bytes).error);
  auto held = root->OpenFile("data", FileAccess::kFileReadData, error);
  REQUIRE(held);
  REQUIRE_FALSE(root->Rename("data", "renamed", false, held->storage_lease()));
  REQUIRE(root->Remove("renamed", false) == std::errc::device_or_resource_busy);
  held.reset();
  REQUIRE_FALSE(root->Remove("renamed", false));
}
TEST_CASE("Protected backup vault is separate from the guest target root",
          "[storage_policy]") {
  Scratch scratch;
  std::filesystem::create_directory(scratch.path / "guest");
  std::filesystem::create_directory(scratch.path / "vault");
  std::error_code error;
  auto guest = HostStorageRoot::Open(scratch.path / "guest", false, error, 0);
  auto vault = HostStorageRoot::Open(scratch.path / "vault", false, error, 0);
  REQUIRE(guest);
  REQUIRE(vault);
  const std::array<uint8_t, 1> old{1}, next{2};
  REQUIRE_FALSE(guest->WriteNewFile("Account", old).error);
  REQUIRE_FALSE(guest->WriteNewFile("staged", next).error);
  const auto result =
      guest->AtomicReplaceFile("staged", "Account", vault, "original");
  REQUIRE_FALSE(result.error);
  REQUIRE(result.published);
  REQUIRE(result.backup_created);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(vault->ReadFile("original", 1, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{1});
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "guest" / "original"));
  REQUIRE_FALSE(guest->WriteNewFile("staged-again", old).error);
  const auto refused =
      guest->AtomicReplaceFile("staged-again", "Account", vault, "original");
  REQUIRE(refused.error == std::errc::file_exists);
  REQUIRE_FALSE(refused.published);
  REQUIRE_FALSE(guest->ReadFile("Account", 1, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{2});
}
namespace {
bool Marker(const std::filesystem::path& path, unsigned milliseconds = 5000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::filesystem::exists(path);
}
struct ProbeProcess {
  pid_t pid = -1;
  ~ProbeProcess() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      int status;
      waitpid(pid, &status, 0);
    }
  }
  void Start(const std::filesystem::path& root,
             const std::filesystem::path& temporary, const char* role,
             const char* filter = "[storage_policy_lock_probe]") {
    const std::string executable = GetExecutablePath().string();
    std::vector<std::string> values;
    for (char** env = environ; *env; ++env) {
      const std::string value = *env;
      if (value.starts_with("TMPDIR=") ||
          value.starts_with("XDG_RUNTIME_DIR=") ||
          value.starts_with("XENIA_STORAGE_PROBE_")) {
        continue;
      }
      values.push_back(value);
    }
    values.push_back("TMPDIR=" + temporary.string());
    values.push_back("XDG_RUNTIME_DIR=" + temporary.string());
    values.push_back("XENIA_STORAGE_PROBE_ROOT=" + root.string());
    values.push_back(std::string("XENIA_STORAGE_PROBE_ROLE=") + role);
    std::vector<char*> environment;
    for (auto& value : values) {
      environment.push_back(value.data());
    }
    environment.push_back(nullptr);
    std::array<std::string, 4> argument_values{executable, filter, "--reporter",
                                               "compact"};
    std::array<char*, 5> arguments{
        argument_values[0].data(), argument_values[1].data(),
        argument_values[2].data(), argument_values[3].data(), nullptr};
    REQUIRE(posix_spawn(&pid, executable.c_str(), nullptr, nullptr,
                        arguments.data(), environment.data()) == 0);
  }
  int Finish() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = waitpid(pid, &status, WNOHANG);
      if (result == pid) {
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
      if (result < 0) {
        pid = -1;
        return -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
  }
};
}  // namespace
// This helper is selected only by the exec-based test. It creates no emulator.
TEST_CASE("Storage lock subprocess helper", "[storage_policy_lock_probe]") {
  const char* root_env = std::getenv("XENIA_STORAGE_PROBE_ROOT");
  const char* role = std::getenv("XENIA_STORAGE_PROBE_ROLE");
  if (!root_env || !role) {
    SUCCEED("Helper executes only in the subprocess lock test");
    return;
  }
  const std::filesystem::path root = root_env;
  std::error_code error;
  auto space = HostStorageSpace::ForPath(root, 0, error);
  REQUIRE(space);
  if (*role == 'a') {
    auto guard = space->Begin(root, error);
    REQUIRE(guard);
    std::ofstream(root / "a-held") << "held";
    REQUIRE(Marker(root / "release"));
  } else {
    std::ofstream(root / "b-attempt") << "waiting";
    auto guard = space->Begin(root, error);
    REQUIRE(guard);
    std::ofstream(root / "b-entered") << "entered";
  }
}
TEST_CASE(
    "Processes with different TMPDIR and XDG overrides share the host lock",
    "[storage_policy]") {
  Scratch scratch;
  std::filesystem::create_directory(scratch.path / "tmp-a");
  std::filesystem::create_directory(scratch.path / "tmp-b");
  ProbeProcess first, second;
  first.Start(scratch.path, scratch.path / "tmp-a", "a");
  REQUIRE(Marker(scratch.path / "a-held"));
  second.Start(scratch.path, scratch.path / "tmp-b", "b");
  REQUIRE(Marker(scratch.path / "b-attempt"));
  REQUIRE_FALSE(Marker(scratch.path / "b-entered", 150));
  std::ofstream(scratch.path / "release") << "release";
  REQUIRE(first.Finish() == 0);
  REQUIRE(second.Finish() == 0);
  REQUIRE(std::filesystem::exists(scratch.path / "b-entered"));
}
TEST_CASE(
    "Read mappings and other aliases prevent resize before physical change",
    "[storage_policy]") {
  Scratch scratch;
  auto root = scratch.Root();
  std::error_code error;
  std::vector<uint8_t> data(4096, 0x5a);
  REQUIRE_FALSE(root->WriteNewFile("data", data).error);
  auto writer = root->OpenFile("data", FileAccess::kFileWriteData, error);
  REQUIRE(writer);
  auto alias = root->OpenFile("data", FileAccess::kFileReadData, error);
  REQUIRE(alias);
  auto mapped = root->MapRead("data", 0, 0, error);
  REQUIRE(mapped);
  REQUIRE(writer->ResizeResult(0) == std::errc::device_or_resource_busy);
  REQUIRE(writer->QueryInfo(error)->size == data.size());
  REQUIRE(mapped->data()[4095] == 0x5a);
  mapped.reset();
  REQUIRE(writer->ResizeResult(8192) == std::errc::device_or_resource_busy);
  alias.reset();
  REQUIRE_FALSE(writer->ResizeResult(2048));
  REQUIRE(writer->QueryInfo(error)->size == 2048);
}
TEST_CASE("Hardlink timestamp mutation is refused even through a read handle",
          "[storage_policy]") {
  Scratch scratch;
  std::filesystem::create_directory(scratch.path / "owned");
  std::ofstream(scratch.path / "outside") << "original";
  const auto before =
      std::filesystem::last_write_time(scratch.path / "outside");
  std::filesystem::create_hard_link(scratch.path / "outside",
                                    scratch.path / "owned" / "linked");
  std::error_code error;
  auto root = HostStorageRoot::Open(scratch.path / "owned", false, error, 0);
  REQUIRE(root);
  auto file = root->OpenFile("linked", FileAccess::kFileReadData, error);
  REQUIRE(file);
  REQUIRE_FALSE(file->SetTimes(0, 0, 132537600000000000ull));
  REQUIRE(file->LastError() == std::errc::permission_denied);
  REQUIRE(std::filesystem::last_write_time(scratch.path / "outside") == before);
}
TEST_CASE(
    "Recursive removal deletes only the prepared subtree and not symlink "
    "targets",
    "[storage_policy]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("tree", true));
  REQUIRE_FALSE(root->Create("tree/child", true));
  const std::array<uint8_t, 1> bytes{42};
  REQUIRE_FALSE(root->WriteNewFile("outside", bytes).error);
  REQUIRE_FALSE(root->WriteNewFile("tree/child/data", bytes).error);
  std::filesystem::create_symlink(scratch.path / "outside",
                                  scratch.path / "tree" / "link");
  REQUIRE(root->Remove("tree", false));
  REQUIRE(std::filesystem::exists(scratch.path / "tree" / "child" / "data"));
  REQUIRE_FALSE(root->Remove("tree", true));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "tree"));
  std::vector<uint8_t> out;
  REQUIRE_FALSE(root->ReadFile("outside", 1, out));
  REQUIRE(out == std::vector<uint8_t>{42});
}

TEST_CASE("Directory reset preserves owned directory and exact retained leaf",
          "[storage_policy][content_mutation]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("package", true));
  REQUIRE_FALSE(root->Create("package/nested", true));
  const std::array<uint8_t, 3> bytes{1, 2, 3};
  REQUIRE_FALSE(root->WriteNewFile("package/nested/save", bytes).error);
  REQUIRE_FALSE(root->WriteNewFile("package/thumbnail", bytes).error);
  REQUIRE_FALSE(root->WriteNewFile("outside", bytes).error);
  std::filesystem::create_symlink(scratch.path / "outside",
                                  scratch.path / "package/link");
  std::error_code error;
  auto directory = root->Subroot("package", false, error);
  REQUIRE(directory);
  auto retained = root->OpenFile("package/thumbnail", FileAccess::kFileReadData,
                                 error);
  REQUIRE(retained);
  auto held = root->OpenFile("package/nested/save", FileAccess::kFileReadData,
                             error);
  REQUIRE(held);
  REQUIRE(root->ResetDirectory("package", "thumbnail") ==
          std::errc::device_or_resource_busy);
  REQUIRE(std::filesystem::exists(scratch.path / "package/nested/save"));
  held.reset();
  REQUIRE_FALSE(root->ResetDirectory("package", "thumbnail"));
  // Reading through this retained root validates the same directory identity;
  // a replaced inode cannot be silently reopened through its pathname.
  std::vector<uint8_t> thumbnail;
  REQUIRE_FALSE(directory->ReadFile("thumbnail", bytes.size(), thumbnail));
  REQUIRE(thumbnail == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  REQUIRE(std::filesystem::exists(scratch.path / "package/thumbnail"));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "package/nested"));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "package/link"));
  std::vector<uint8_t> read;
  REQUIRE_FALSE(root->ReadFile("outside", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

TEST_CASE("Removal preflight detects a leased child through an outside alias",
          "[storage_policy][content_mutation]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("package", true));
  const std::array<uint8_t, 2> bytes{7, 9};
  REQUIRE_FALSE(root->WriteNewFile("package/save", bytes).error);
  std::error_code error;
  std::filesystem::create_hard_link(scratch.path / "package/save",
                                   scratch.path / "alias", error);
  REQUIRE_FALSE(error);
  auto held = root->OpenFile("alias", FileAccess::kFileReadData, error);
  REQUIRE(held);
  REQUIRE(root->Remove("package", true) ==
          std::errc::device_or_resource_busy);
  REQUIRE(root->ResetDirectory("package") ==
          std::errc::device_or_resource_busy);
  REQUIRE(std::filesystem::exists(scratch.path / "package/save"));
  held.reset();
  REQUIRE_FALSE(root->ResetDirectory("package"));
  REQUIRE(std::filesystem::is_directory(scratch.path / "package"));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "package/save"));
  std::vector<uint8_t> read;
  REQUIRE_FALSE(root->ReadFile("alias", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

TEST_CASE("File reset admits directory metadata before destroying source bytes",
          "[storage_policy][content_mutation]") {
  Scratch scratch;
  std::error_code error;
  uint64_t available = kHostStorageReserve + (1ull << 20);
  auto space = HostStorageSpace::ForPath(
      scratch.path, kHostStorageReserve, error,
      [&](const auto&, auto&) {
        return std::filesystem::space_info{1ull << 40, available, available};
      });
  REQUIRE(space);
  auto root = HostStorageRoot::Open(scratch.path, false, error,
                                     kHostStorageReserve, space);
  REQUIRE(root);
  const std::array<uint8_t, 2> bytes{4, 8};
  REQUIRE_FALSE(root->WriteNewFile("package", bytes).error);
  available = kHostStorageReserve;
  REQUIRE(root->ResetDirectory("package") == std::errc::no_space_on_device);
  std::vector<uint8_t> read;
  REQUIRE_FALSE(root->ReadFile("package", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  REQUIRE(root->ResetDirectory("package", "../outside") ==
          std::errc::invalid_argument);
  available += 1ull << 20;
  REQUIRE_FALSE(root->ResetDirectory("package"));
  REQUIRE(std::filesystem::is_directory(scratch.path / "package"));
  root->Retire();
  REQUIRE(root->ResetDirectory("package") == std::errc::no_such_device);
  REQUIRE(std::filesystem::is_directory(scratch.path / "package"));
}

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
TEST_CASE("Tree flush covers closed and open files without capacity admission",
          "[storage_policy][content_flush]") {
  Scratch scratch;
  std::error_code error;
  uint64_t available = kHostStorageReserve + (1ull << 20);
  size_t queries = 0;
  auto space = HostStorageSpace::ForPath(scratch.path, kHostStorageReserve, error,
      [&](const auto&, auto&) {
        ++queries;
        return std::filesystem::space_info{1ull << 40, available, available};
      });
  REQUIRE_FALSE(error);
  REQUIRE(space);
  auto root = HostStorageRoot::Open(scratch.path, false, error,
                                     kHostStorageReserve, space);
  REQUIRE_FALSE(error);
  REQUIRE(root);
  REQUIRE_FALSE(root->Create("package", true));
  REQUIRE_FALSE(root->Create("package/nested", true));
  REQUIRE_FALSE(root->Create("package/empty", true));
  const std::array<uint8_t, 4> bytes{0, 0x7f, 0x80, 0xff};
  REQUIRE_FALSE(root->WriteNewFile("package/nested/closed", bytes, false).error);
  REQUIRE_FALSE(root->WriteNewFile("package/open", bytes, false).error);
  REQUIRE_FALSE(root->WriteNewFile("package/zero", {}, false).error);
  auto writer = root->OpenFile("package/open", FileAccess::kGenericAll, error);
  REQUIRE_FALSE(error);
  REQUIRE(writer);
  const std::array<uint8_t, 2> replacement{5, 9};
  REQUIRE_FALSE(writer->WriteResult(1, replacement).error);
  auto mapped = root->MapRead("package/nested/closed", 0, 0, error);
  REQUIRE_FALSE(error);
  REQUIRE(mapped);
  struct stat before_directory{}, before_file{};
  REQUIRE(stat((scratch.path / "package").c_str(), &before_directory) == 0);
  REQUIRE(stat((scratch.path / "package/nested/closed").c_str(), &before_file) == 0);
  const auto directory_time = std::filesystem::last_write_time(scratch.path / "package");
  const auto file_time = std::filesystem::last_write_time(scratch.path / "package/nested/closed");
  available = kHostStorageReserve;
  const size_t before_queries = queries;
  REQUIRE_FALSE(root->FlushTree("package"));
  REQUIRE_FALSE(root->FlushTree("package"));
  REQUIRE_FALSE(root->FlushTree());
  // No new allocation is admitted, and a directory-only barrier cannot stand
  // in for the retained regular-file pass (including the already-closed file).
  REQUIRE(queries == before_queries);
  struct stat after_directory{}, after_file{};
  REQUIRE(stat((scratch.path / "package").c_str(), &after_directory) == 0);
  REQUIRE(stat((scratch.path / "package/nested/closed").c_str(), &after_file) == 0);
  REQUIRE(after_directory.st_ino == before_directory.st_ino);
  REQUIRE(after_file.st_ino == before_file.st_ino);
  REQUIRE(after_file.st_size == before_file.st_size);
  REQUIRE(std::filesystem::last_write_time(scratch.path / "package") == directory_time);
  REQUIRE(std::filesystem::last_write_time(scratch.path / "package/nested/closed") == file_time);
  std::vector<uint8_t> read;
  REQUIRE_FALSE(root->ReadFile("package/nested/closed", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  REQUIRE_FALSE(root->ReadFile("package/open", 16, read));
  REQUIRE(read == (std::vector<uint8_t>{0, 5, 9, 0xff}));
  REQUIRE_FALSE(root->ReadFile("package/zero", 16, read));
  REQUIRE(read.empty());
  REQUIRE(std::filesystem::is_empty(scratch.path / "package/empty"));
  REQUIRE(mapped->data()[3] == bytes[3]);
}

TEST_CASE("Tree flush refuses unsupported members without changing them",
          "[storage_policy][content_flush]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("tree", true));
  const std::array<uint8_t, 3> bytes{1, 4, 7};
  REQUIRE_FALSE(root->WriteNewFile("tree/owned", bytes, false).error);
  REQUIRE_FALSE(root->WriteNewFile("outside", bytes, false).error);
  const auto invalid = scratch.path / "tree/invalid";
  SECTION("symlink") {
    std::filesystem::create_symlink(scratch.path / "outside", invalid);
  }
  SECTION("dangling symlink") {
    std::filesystem::create_symlink(scratch.path / "absent", invalid);
  }
  SECTION("outside hardlink") {
    std::filesystem::create_hard_link(scratch.path / "outside", invalid);
  }
  SECTION("FIFO") { REQUIRE(mkfifo(invalid.c_str(), 0600) == 0); }
  const auto type = std::filesystem::symlink_status(invalid).type();
  REQUIRE(root->FlushTree("tree") == std::errc::permission_denied);
  REQUIRE(std::filesystem::symlink_status(invalid).type() == type);
  std::vector<uint8_t> read;
  REQUIRE_FALSE(root->ReadFile("outside", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  REQUIRE_FALSE(root->ReadFile("tree/owned", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  REQUIRE(std::filesystem::remove(invalid));
  REQUIRE_FALSE(root->FlushTree("tree"));
}

TEST_CASE("Tree flush preserves exclusive overwrite completion ownership",
          "[storage_policy][content_flush]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("package", true));
  const std::array<uint8_t, 3> bytes{3, 6, 9};
  REQUIRE_FALSE(root->WriteNewFile("package/save", bytes, false).error);
  std::error_code error;
  auto reserved = root->ReserveOverwrite("package/save", error);
  REQUIRE_FALSE(error);
  REQUIRE(reserved);
  REQUIRE(root->FlushTree("package") == std::errc::device_or_resource_busy);
  REQUIRE(std::filesystem::file_size(scratch.path / "package/save") == bytes.size());
  reserved.reset();
  REQUIRE_FALSE(root->FlushTree("package"));
  std::vector<uint8_t> read;
  REQUIRE_FALSE(root->ReadFile("package/save", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

TEST_CASE("Tree flush rejects invalid readonly retired and replaced roots",
          "[storage_policy][content_flush]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("package", true));
  const std::array<uint8_t, 1> bytes{42};
  REQUIRE_FALSE(root->WriteNewFile("package/save", bytes, false).error);
  REQUIRE(root->FlushTree("../outside") == std::errc::invalid_argument);
  REQUIRE(root->FlushTree(scratch.path) == std::errc::invalid_argument);
  REQUIRE(root->FlushTree("package/save") == std::errc::not_a_directory);
  REQUIRE(root->FlushTree("missing") == std::errc::no_such_file_or_directory);
  std::error_code error;
  auto readonly = root->Subroot("package", true, error);
  REQUIRE_FALSE(error);
  REQUIRE(readonly);
  REQUIRE(readonly->FlushTree() == std::errc::read_only_file_system);
  auto old = root->Subroot("package", false, error);
  REQUIRE_FALSE(error);
  REQUIRE(old);
  std::filesystem::rename(scratch.path / "package", scratch.path / "old-package");
  REQUIRE(std::filesystem::create_directory(scratch.path / "package"));
  std::ofstream(scratch.path / "package/new") << "new-generation";
  REQUIRE(old->FlushTree() == std::errc::no_such_device);
  auto current = root->Subroot("package", false, error);
  REQUIRE_FALSE(error);
  REQUIRE(current);
  REQUIRE_FALSE(current->FlushTree());
  root->Retire();
  REQUIRE(current->FlushTree() == std::errc::no_such_device);
  REQUIRE(std::filesystem::exists(scratch.path / "old-package/save"));
  REQUIRE(std::filesystem::exists(scratch.path / "package/new"));
}

TEST_CASE("Tree flush bounds depth before issuing any data barrier",
          "[storage_policy][content_flush]") {
  Scratch scratch;
  auto root = scratch.Root();
  auto deepest = scratch.path;
  for (unsigned i = 0; i < 64; ++i) {
    deepest /= "d";
    REQUIRE(std::filesystem::create_directory(deepest));
  }
  std::ofstream(deepest / "sentinel") << "preserved";
  REQUIRE(root->FlushTree() == std::errc::value_too_large);
  REQUIRE(std::filesystem::file_size(deepest / "sentinel") == 9);
  REQUIRE(std::filesystem::remove(deepest / "sentinel"));
  REQUIRE(std::filesystem::remove(deepest));
  REQUIRE_FALSE(root->FlushTree());
}

TEST_CASE("Tree flush refuses protected overlaps through preexisting roots",
          "[storage_policy][content_flush]") {
  Scratch scratch;
  std::error_code error;
  auto root = HostStorageRoot::Open(scratch.path, false, error);
  REQUIRE_FALSE(error);
  REQUIRE(root);
  REQUIRE_FALSE(root->Create("private", true));
  REQUIRE_FALSE(root->Create("ordinary", true));
  auto private_root = root->Subroot("private", false, error);
  REQUIRE_FALSE(error);
  REQUIRE(private_root);
  const std::array<uint8_t, 2> bytes{2, 8};
  REQUIRE_FALSE(private_root->WriteNewFile("owner", bytes, false).error);
  auto protected_root = private_root->ProtectFlat(error);
  REQUIRE_FALSE(error);
  REQUIRE(protected_root);
  REQUIRE(root->FlushTree() == std::errc::permission_denied);
  REQUIRE(root->FlushTree("private") == std::errc::permission_denied);
  REQUIRE(private_root->FlushTree() == std::errc::permission_denied);
  REQUIRE(protected_root->FlushTree() == std::errc::permission_denied);
  REQUIRE_FALSE(root->FlushTree("ordinary"));
  std::vector<uint8_t> read;
  REQUIRE_FALSE(protected_root->ReadFile("owner", 16, read));
  REQUIRE(read == std::vector<uint8_t>(bytes.begin(), bytes.end()));
}
#endif

TEST_CASE("Recursive removal mount namespace subprocess fixture",
          "[storage_policy_mount_probe]") {
  const char* root_env = std::getenv("XENIA_STORAGE_PROBE_ROOT");
  if (!root_env) {
    SUCCEED("Mount fixture is selected only by its subprocess test");
    return;
  }
  const std::filesystem::path path = root_env;
#if XE_PLATFORM_LINUX
  const auto uid = getuid();
  const auto gid = getgid();
  auto unavailable = [&](const char* reason) {
    const int failure = errno;
    std::ofstream(path / "mount-unavailable") << reason << ": " << failure;
    SUCCEED("UNVERIFIED: private mount fixture unavailable");
  };
  if (unshare(CLONE_NEWUSER | CLONE_NEWNS)) {
    unavailable("unshare");
    SUCCEED("UNVERIFIED: private mount namespace unavailable");
    return;
  }
  {
    std::ofstream deny("/proc/self/setgroups");
    if (deny) {
      deny << "deny";
    }
  }
  {
    std::ofstream map("/proc/self/uid_map");
    map << "0 " << uid << " 1\n";
    map.flush();
    if (!map) {
      unavailable("uid map");
      return;
    }
  }
  {
    std::ofstream map("/proc/self/gid_map");
    map << "0 " << gid << " 1\n";
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
  std::filesystem::create_directories(path / "tree" / "foreign");
  std::ofstream(path / "tree" / "owned-before") << "keep";
  if (mount("tmpfs", (path / "tree" / "foreign").c_str(), "tmpfs",
            MS_NODEV | MS_NOSUID, "size=1m,mode=0700")) {
    unavailable("tmpfs");
    return;
  }
  std::ofstream(path / "tree" / "foreign" / "protected") << "foreign";
  std::error_code error;
  // Isolated query avoids treating mapped uid0 as the host's privileged UID
  // for the unrelated process-lock fixture. This case tests traversal only.
  auto space =
      HostStorageSpace::ForPath(path, 0, error, [](const auto& root, auto& ec) {
        return std::filesystem::space(root, ec);
      });
  auto root = HostStorageRoot::Open(path, false, error, 0, space);
  REQUIRE(root);
  REQUIRE(root->FlushTree("tree/foreign") == std::errc::cross_device_link);
  REQUIRE(root->FlushTree("tree") == std::errc::cross_device_link);
  REQUIRE(root->Remove("tree/foreign", true) == std::errc::cross_device_link);
  REQUIRE(root->Remove("tree", true) == std::errc::cross_device_link);
  REQUIRE(std::filesystem::exists(path / "tree" / "owned-before"));
  REQUIRE(std::filesystem::exists(path / "tree" / "foreign" / "protected"));
  REQUIRE(umount2((path / "tree" / "foreign").c_str(), MNT_DETACH) == 0);
  // A same-filesystem bind has equal st_dev yet is a separate mount boundary.
  // The selected root may itself be that mount; only descendant crossings fail.
  std::filesystem::create_directory(path / "bind-source");
  std::filesystem::create_directory(path / "tree" / "bound");
  std::ofstream(path / "bind-source" / "protected") << "same-filesystem";
  if (mount((path / "bind-source").c_str(), (path / "tree" / "bound").c_str(),
            nullptr, MS_BIND, nullptr)) {
    unavailable("same-filesystem bind");
    return;
  }
  struct stat source_stat{}, bound_stat{};
  REQUIRE(stat((path / "bind-source").c_str(), &source_stat) == 0);
  REQUIRE(stat((path / "tree" / "bound").c_str(), &bound_stat) == 0);
  REQUIRE(source_stat.st_dev == bound_stat.st_dev);
  REQUIRE_FALSE(root->Subroot("tree/bound", false, error));
  REQUIRE(error == std::errc::cross_device_link);
  REQUIRE_FALSE(
      root->OpenFile("tree/bound/protected", FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::cross_device_link);
  REQUIRE_FALSE(root->OpenFile("tree/bound", FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::cross_device_link);
  REQUIRE(root->FlushTree("tree/bound") == std::errc::cross_device_link);
  REQUIRE(root->FlushTree("tree") == std::errc::cross_device_link);
  REQUIRE(root->Remove("tree/bound", true) == std::errc::cross_device_link);
  REQUIRE(root->Remove("tree", true) == std::errc::cross_device_link);
  REQUIRE(std::filesystem::exists(path / "tree" / "owned-before"));
  REQUIRE(std::filesystem::exists(path / "bind-source" / "protected"));
  auto selected_mount =
      HostStorageRoot::Open(path / "tree" / "bound", false, error, 0, space);
  REQUIRE(selected_mount);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(selected_mount->ReadFile("protected", 32, bytes));
  REQUIRE(std::string(bytes.begin(), bytes.end()) == "same-filesystem");
  REQUIRE_FALSE(selected_mount->FlushTree());
  selected_mount.reset();
  REQUIRE(umount2((path / "tree" / "bound").c_str(), MNT_DETACH) == 0);
  // Linux also permits bind-mounted regular-file leaves. Final-leaf checks
  // must reject these even though Parent did not cross a directory boundary.
  std::ofstream(path / "tree" / "bound-file") << "placeholder";
  if (mount((path / "bind-source" / "protected").c_str(),
            (path / "tree" / "bound-file").c_str(), nullptr, MS_BIND,
            nullptr)) {
    unavailable("same-filesystem file bind");
    return;
  }
  REQUIRE_FALSE(
      root->OpenFile("tree/bound-file", FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::cross_device_link);
  REQUIRE(root->FlushTree("tree") == std::errc::cross_device_link);
  REQUIRE(root->Remove("tree", true) == std::errc::cross_device_link);
  REQUIRE(std::filesystem::exists(path / "tree" / "owned-before"));
  REQUIRE(std::filesystem::exists(path / "bind-source" / "protected"));
  REQUIRE(umount2((path / "tree" / "bound-file").c_str(), MNT_DETACH) == 0);
  std::ofstream(path / "mount-verified")
      << "tmpfs, same-FS directory bind, same-FS file bind verified";
#else
  std::ofstream(path / "mount-unavailable")
      << "UNVERIFIED: Linux private mount fixture is unavailable on this "
         "platform";
  SUCCEED("UNVERIFIED: no private mount fixture on this platform");
#endif
}
TEST_CASE(
    "Recursive removal preflight refuses mounted descendants before deletion",
    "[storage_policy]") {
  Scratch scratch;
  ProbeProcess probe;
  probe.Start(scratch.path, scratch.path, "m", "[storage_policy_mount_probe]");
  REQUIRE(probe.Finish() == 0);
  if (std::filesystem::exists(scratch.path / "mount-unavailable")) {
    std::ifstream reason(scratch.path / "mount-unavailable");
    std::string text;
    std::getline(reason, text);
    WARN("UNVERIFIED mount-namespace refusal coverage: " << text);
    SUCCEED("Fixture was unavailable; this is not mount-refusal acceptance");
    return;
  }
  REQUIRE(std::filesystem::exists(scratch.path / "mount-verified"));
}
}  // namespace xe::filesystem::test
#endif
