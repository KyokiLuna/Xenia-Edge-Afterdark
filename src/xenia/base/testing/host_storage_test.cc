#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "xenia/base/host_storage.h"
#include "xenia/base/mutex.h"
#include "xenia/base/utf8.h"
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
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
TEST_CASE("Storage admission observes the original same-thread and contending guard without changing retry",
          "[storage_policy]") {
  Scratch scratch;
  std::error_code error;
  auto space=HostStorageSpace::ForPath(scratch.path,0,error,[](const auto&,auto&){
    return std::filesystem::space_info{1ull<<40,1ull<<30,1ull<<30};});
  REQUIRE(space); REQUIRE_FALSE(error);
  auto held=space->Begin(scratch.path,error); REQUIRE(held); REQUIRE_FALSE(error);
  HostStorageFailureObservation same;
  CHECK_FALSE(space->TryBegin(scratch.path,error,{},&same));
  CHECK(error==std::errc::device_or_resource_busy);
  CHECK(same.operation==1); CHECK(same.reason==2); CHECK(same.line!=0);
  CHECK(same.error_category==1); CHECK(same.error_value==int(std::errc::device_or_resource_busy));
  CHECK(same.guard_owner==same.guard_request);
  const auto first_line=same.line;
  // A later independent refusal may not replace the originating cut.
  CHECK_FALSE(space->TryBegin(scratch.path/std::string("bad\0leaf",8),error,{},&same));
  CHECK(error==std::errc::invalid_argument); CHECK(same.reason==2); CHECK(same.line==first_line);
  HostStorageFailureObservation competing;
  std::error_code concurrent_error; bool acquired=false;
  std::thread contender([&]{auto guard=space->TryBegin(scratch.path,concurrent_error,{},&competing);
    acquired=bool(guard);});
  contender.join();
  CHECK_FALSE(acquired); CHECK(concurrent_error==std::errc::device_or_resource_busy);
  CHECK(competing.operation==1); CHECK(competing.reason==4);
  CHECK(competing.guard_owner!=competing.guard_request);
  held.reset();
  HostStorageFailureObservation retry;
  auto admitted=space->TryBegin(scratch.path,error,{},&retry);
  REQUIRE(admitted); CHECK_FALSE(error); CHECK(retry.line==0);
  CHECK(same.line==first_line); CHECK(competing.reason==4);
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
  } else if (*role == 'c') {
    HostStoragePreparationWait wait(std::chrono::steady_clock::now()+std::chrono::milliseconds(80));
    HostStorageFailureObservation diagnostic;
    auto guard=space->BeginPreparation(root,error,wait,{},&diagnostic);
    CHECK_FALSE(guard); CHECK(error==std::errc::timed_out);
    CHECK(diagnostic.operation==1); CHECK(diagnostic.reason==7);
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

TEST_CASE("Preparation guard waits for real handoff and cancels without admitting a guard",
          "[storage_policy][prepared_mount]") {
  Scratch scratch; std::error_code error;
  auto space=HostStorageSpace::ForPath(scratch.path,0,error,[](const auto&,auto&){
    return std::filesystem::space_info{1ull<<40,1ull<<30,1ull<<30};});
  REQUIRE(space); auto held=space->Begin(scratch.path,error); REQUIRE(held);
  HostStoragePreparationWait same(std::chrono::steady_clock::now()+std::chrono::seconds(1));
  CHECK_FALSE(space->BeginPreparation(scratch.path,error,same));
  CHECK(error==std::errc::device_or_resource_busy);
  std::promise<void> started,finished; auto start=started.get_future(); auto done=finished.get_future();
  bool acquired=false; std::error_code result;
  std::thread thread([&]{
    HostStoragePreparationWait wait(std::chrono::steady_clock::now()+std::chrono::seconds(1));
    started.set_value(); auto guard=space->BeginPreparation(scratch.path,result,wait);
    acquired=bool(guard); finished.set_value();
  });
  start.wait(); const bool actually_waited=done.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
  held.reset(); thread.join();
  CHECK(actually_waited); CHECK(acquired); CHECK_FALSE(result);
  held=space->Begin(scratch.path,error); REQUIRE(held);
  std::stop_source stop; std::promise<void> second_started; auto second=second_started.get_future();
  std::thread canceled([&]{
    HostStoragePreparationWait wait(std::chrono::steady_clock::now()+std::chrono::seconds(1));
    wait.LinkCancellation(stop.get_token()); second_started.set_value();
    auto guard=space->BeginPreparation(scratch.path,result,wait); acquired=bool(guard);
  });
  second.wait(); stop.request_stop(); canceled.join();
  CHECK_FALSE(acquired); CHECK(result==std::errc::operation_canceled);
  // Same deadline applies to a later phase, rather than being reset per lock.
  HostStoragePreparationWait expired(std::chrono::steady_clock::now()-std::chrono::milliseconds(1));
  held.reset(); CHECK_FALSE(space->BeginPreparation(scratch.path,error,expired));
  CHECK(error==std::errc::timed_out);
}
TEST_CASE("Preparation guard external flock contention has a real bounded deadline",
          "[storage_policy][prepared_mount]") {
  Scratch scratch;
  std::filesystem::create_directory(scratch.path/"tmp-a");
  std::filesystem::create_directory(scratch.path/"tmp-c");
  ProbeProcess first,second;
  first.Start(scratch.path,scratch.path/"tmp-a","a");
  REQUIRE(Marker(scratch.path/"a-held"));
  second.Start(scratch.path,scratch.path/"tmp-c","c");
  const auto timed_result=second.Finish();
  std::ofstream(scratch.path/"release")<<"release";
  CHECK(timed_result==0); CHECK(first.Finish()==0);
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

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem {
class HostStorageBootstrapTestAccess final {
 public:
  static auto Metadata(const std::shared_ptr<HostStorageRoot>& root,
      std::span<const uint8_t> initial, std::string_view nonce,
      std::error_code& error, std::shared_ptr<const HostStorageIntentLease> lease = {}) {
    const auto mode = lease ? HostStorageRoot::ManagedMetadataOpen::kCreatePending :
                              HostStorageRoot::ManagedMetadataOpen::kCreate;
    return root->OpenManagedMetadata(".xenia-managed-source-v1",error,
        mode,initial,nonce,std::move(lease));
  }
  static auto Browse(const HostStorageRoot& directory,const HostStorageRoot& source) {
    return directory.CheckBootstrapBrowseDirectory(source);
  }
  static auto Lease(const std::shared_ptr<HostStorageRoot>& root,std::error_code& error) {
    return root->AcquireIntentLease(error,true);
  }
  static void Stop(const std::shared_ptr<HostStorageRoot>& root,unsigned point) { root->bootstrap_test_boundary_.store(point); }
  static auto Temporary(std::string_view nonce) { return HostStorageRoot::MetadataBootstrapTemporary(nonce); }
  static auto DirectoryTemporary(std::string_view target,std::string_view nonce) { return HostStorageRoot::DirectoryBootstrapTemporary(target,nonce); }
  static auto Append(HostStorageManagedMetadata& metadata,std::span<const uint8_t> suffix,
      std::shared_ptr<const HostStorageIntentLease> lease,std::string_view nonce) {
    return metadata.AppendBootstrapAtomically(suffix,std::move(lease),nonce);
  }
  static auto Bytes(const HostStorageManagedMetadata& metadata) { return metadata.bytes_; }
  static bool Guest(const HostStorageManagedMetadata& metadata) { return bool(metadata.guest_view_); }
  static auto Current(const HostStorageManagedMetadata& metadata) { return metadata.Check(); }
  static auto Begin(const std::shared_ptr<HostStorageRoot>& parent,std::string_view target,
      std::string_view nonce,std::error_code& error,
      std::shared_ptr<const HostStorageIntentLease> lease = {}) { return parent->BeginBootstrapDirectory(target,nonce,error,std::move(lease)); }
  static auto Resume(const std::shared_ptr<HostStorageRoot>& parent,
      const std::filesystem::path& target,const std::filesystem::path& stage,uint64_t id,std::error_code& error) {
    return parent->ResumeBootstrapDirectory(target,stage,id,error);
  }
  static auto Publish(const std::shared_ptr<HostStorageRoot>& parent,HostStorageBootstrapDirectory& directory) {
    return parent->PublishBootstrapDirectory(directory);
  }
  static auto Media(const std::shared_ptr<HostStorageRoot>& parent,
      HostStorageBootstrapDirectory& directory, std::error_code& error) {
    return parent->ProtectMediaReferenceDirectory(directory,error);
  }
  static auto Admit(const std::shared_ptr<HostStorageRoot>& parent,HostStorageBootstrapDirectory& directory,
      std::error_code& error,const HostStorageRoot* source = nullptr) {
    return parent->AdmitBootstrapPublicDirectory(directory,error,source);
  }
  static auto Projection(const std::shared_ptr<HostStorageRoot>& source,
      std::vector<std::shared_ptr<const HostStorageRoot>> private_roots,
      std::vector<std::shared_ptr<const HostStorageRoot>> namespace_roots,
      std::span<HostStorageBootstrapDirectory* const> settled_private,
      HostStorageBootstrapDirectory* public_capsule,std::error_code& error) {
    return source->InstallBootstrapProjection(std::move(private_roots),std::move(namespace_roots),
        false,settled_private,public_capsule,error);
  }
  static auto OrdinaryProjection(const std::shared_ptr<HostStorageRoot>& source,
      std::error_code& error) { return source->InstallProjection({},{},false,error); }
  static auto Record(const std::shared_ptr<HostStorageRoot>& root,const std::filesystem::path& leaf,
      std::span<const uint8_t> bytes,std::string_view nonce) { return root->PublishBootstrapRecord(leaf,bytes,nonce); }
  static auto List(const std::shared_ptr<HostStorageRoot>& root,std::error_code& error) { return root->ListBootstrapRecords(error); }
};
}
namespace xe::filesystem::test {
namespace {
std::shared_ptr<HostStorageRoot> BootstrapNativeRoot(Scratch& scratch) {
  std::error_code error;auto root=HostStorageRoot::Open(scratch.path,false,error);
  REQUIRE(root);REQUIRE_FALSE(error);return root;
}
std::vector<uint8_t> BootstrapBytes(const std::filesystem::path& path) {
  std::ifstream in(path,std::ios::binary);REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in),{}};
}
constexpr std::string_view kBootstrapNonce="0123456789abcdef0123456789abcdef";
constexpr std::string_view kBootstrapNext="1123456789abcdef0123456789abcdef";
}
TEST_CASE("Atomic managed metadata creation never publishes an empty canonical marker",
          "[storage_policy][bootstrap][metadata][native]") {
  const unsigned boundary=GENERATE(1u,2u,3u);
  Scratch scratch;auto root=BootstrapNativeRoot(scratch);std::error_code error;
  const std::array<uint8_t,7> complete{1,0,3,4,0xff,6,9};
  HostStorageBootstrapTestAccess::Stop(root,boundary);
  auto metadata=HostStorageBootstrapTestAccess::Metadata(root,complete,kBootstrapNonce,error);
  REQUIRE(error==std::errc::operation_canceled);
  const auto canonical=scratch.path/".xenia-managed-source-v1";
  REQUIRE(std::filesystem::exists(canonical)==(boundary==3));
  if (boundary==3) {
    REQUIRE(metadata);REQUIRE_FALSE(HostStorageBootstrapTestAccess::Guest(*metadata));
    REQUIRE(BootstrapBytes(canonical)==std::vector<uint8_t>(complete.begin(),complete.end()));
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Current(*metadata));
  } else {
    REQUIRE_FALSE(metadata);
    const auto temporary=scratch.path/HostStorageBootstrapTestAccess::Temporary(kBootstrapNonce);
    REQUIRE(std::filesystem::exists(temporary));
    REQUIRE(std::filesystem::file_size(temporary)==(boundary==1?0:complete.size()));
    // A previous temporary is not adopted by a later create attempt.
    metadata=HostStorageBootstrapTestAccess::Metadata(root,complete,kBootstrapNonce,error);
    REQUIRE_FALSE(metadata);REQUIRE(error);REQUIRE_FALSE(std::filesystem::exists(canonical));
  }
}
TEST_CASE("Legacy bootstrap suffix preserves old control inode and transfers to locked new metadata",
          "[storage_policy][bootstrap][metadata][legacy]") {
  Scratch scratch;auto root=BootstrapNativeRoot(scratch);std::error_code error;
  auto lease=HostStorageBootstrapTestAccess::Lease(root,error);REQUIRE(lease);REQUIRE_FALSE(error);
  const std::array<uint8_t,5> prefix{0,2,4,0xff,8},suffix{9,7,0,3,1};
  auto metadata=HostStorageBootstrapTestAccess::Metadata(root,prefix,kBootstrapNonce,error,lease);
  REQUIRE(metadata);REQUIRE_FALSE(error);REQUIRE_FALSE(HostStorageBootstrapTestAccess::Guest(*metadata));
  struct stat old{};REQUIRE(lstat((scratch.path/".xenia-managed-source-v1").c_str(),&old)==0);
  HostStorageBootstrapTestAccess::Stop(root,3);
  const auto appended=HostStorageBootstrapTestAccess::Append(*metadata,suffix,lease,kBootstrapNext);
  REQUIRE(appended.error==std::errc::operation_canceled);REQUIRE(appended.published);REQUIRE_FALSE(appended.durable);
  REQUIRE(appended.bytes==suffix.size());REQUIRE(appended.previous_file==uint64_t(old.st_ino));
  REQUIRE(BootstrapBytes(scratch.path/appended.previous)==std::vector<uint8_t>(prefix.begin(),prefix.end()));
  struct stat previous{},current{};
  REQUIRE(lstat((scratch.path/appended.previous).c_str(),&previous)==0);
  REQUIRE(lstat((scratch.path/".xenia-managed-source-v1").c_str(),&current)==0);
  REQUIRE(previous.st_ino==old.st_ino);REQUIRE(current.st_ino!=old.st_ino);
  auto expected=std::vector<uint8_t>(prefix.begin(),prefix.end());expected.insert(expected.end(),suffix.begin(),suffix.end());
  REQUIRE(HostStorageBootstrapTestAccess::Bytes(*metadata)==expected);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Current(*metadata));
  auto competing=HostStorageBootstrapTestAccess::Metadata(root,{},kBootstrapNext,error);
  REQUIRE_FALSE(competing);REQUIRE(error==std::errc::device_or_resource_busy);
  // Exact final wire remains, with no replacement or metadata GuestView.
  REQUIRE(BootstrapBytes(scratch.path/".xenia-managed-source-v1")==expected);
}
TEST_CASE("Bootstrap directories resume only recorded IDs and retain protection across uncertain publication",
          "[storage_policy][bootstrap][directory][native]") {
  Scratch scratch;auto parent=BootstrapNativeRoot(scratch);std::error_code error;
  auto stage=HostStorageBootstrapTestAccess::Begin(parent,"published-owner",kBootstrapNonce,error);
  REQUIRE(stage);REQUIRE_FALSE(error);const auto id=stage->native_directory();const auto leaf=stage->staging();
  REQUIRE(leaf==HostStorageBootstrapTestAccess::DirectoryTemporary("published-owner",kBootstrapNonce));
  const std::array<uint8_t,3> owner{4,0,9};
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Record(stage->root(),"owner.v1",owner,kBootstrapNonce).error);
  REQUIRE_FALSE(stage->root()->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  stage.reset();stage=HostStorageBootstrapTestAccess::Resume(parent,"published-owner",leaf,id,error);
  REQUIRE(stage);REQUIRE_FALSE(error);
  HostStorageBootstrapTestAccess::Stop(parent,3);
  const auto published=HostStorageBootstrapTestAccess::Publish(parent,*stage);
  REQUIRE(published.published);REQUIRE_FALSE(published.durable);
  REQUIRE(published.error==std::errc::operation_canceled);REQUIRE(published.canonical_root);
  REQUIRE_FALSE(published.canonical_root->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  REQUIRE(BootstrapBytes(scratch.path/"published-owner/owner.v1")==std::vector<uint8_t>(owner.begin(),owner.end()));
  const auto durable=HostStorageBootstrapTestAccess::Publish(parent,*stage);
  REQUIRE_FALSE(durable.error);REQUIRE(durable.durable);REQUIRE_FALSE(durable.published);
  auto admitted=HostStorageBootstrapTestAccess::Admit(parent,*stage,error);
  REQUIRE(admitted);REQUIRE_FALSE(error);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Admit(parent,*stage,error));REQUIRE(error);
}
TEST_CASE("Bootstrap resume refuses wrong IDs both-present and native aliases unchanged",
          "[storage_policy][bootstrap][directory][refusal]") {
  Scratch scratch;auto parent=BootstrapNativeRoot(scratch);std::error_code error;
  auto stage=HostStorageBootstrapTestAccess::Begin(parent,"owner",kBootstrapNonce,error);
  REQUIRE(stage);const auto id=stage->native_directory();const auto leaf=stage->staging();stage.reset();
  SECTION("Wrong source-local inode") {
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Resume(parent,"owner",leaf,id+1,error));REQUIRE(error);
  }
  SECTION("Both actual stage and canonical exist") {
    REQUIRE(std::filesystem::create_directory(scratch.path/"owner"));
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Resume(parent,"owner",leaf,id,error));REQUIRE(error);
    REQUIRE(std::filesystem::exists(scratch.path/leaf));REQUIRE(std::filesystem::exists(scratch.path/"owner"));
  }
  SECTION("Symlink cannot stand in for the canonical directory") {
    REQUIRE(symlink(leaf.c_str(),(scratch.path/"owner").c_str())==0);
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Resume(parent,"owner",leaf,id,error));REQUIRE(error);
    REQUIRE(std::filesystem::is_symlink(scratch.path/"owner"));
  }
}
TEST_CASE("Bootstrap records preserve partial control bytes and never overwrite conflicts",
          "[storage_policy][bootstrap][record][native]") {
  Scratch scratch;auto root=BootstrapNativeRoot(scratch);std::error_code error;
  const std::array<uint8_t,21> bytes{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21};
  HostStorageBootstrapTestAccess::Stop(root,1);
  auto cut=HostStorageBootstrapTestAccess::Record(root,"bootstrap.pending.v1",bytes,kBootstrapNonce);
  REQUIRE(cut.error);REQUIRE_FALSE(cut.published);REQUIRE_FALSE(cut.durable);
  const auto partial=scratch.path/("bootstrap.pending.v1.partial-"+std::string(kBootstrapNonce));
  REQUIRE(std::filesystem::file_size(partial)==17);const auto preserved=BootstrapBytes(partial);
  auto complete=HostStorageBootstrapTestAccess::Record(root,"bootstrap.pending.v1",bytes,kBootstrapNext);
  REQUIRE_FALSE(complete.error);REQUIRE(complete.published);REQUIRE(complete.durable);
  auto observed=HostStorageBootstrapTestAccess::Record(root,"bootstrap.pending.v1",bytes,kBootstrapNonce);
  REQUIRE_FALSE(observed.error);REQUIRE_FALSE(observed.published);REQUIRE(observed.durable);
  const std::array<uint8_t,1> other{99};
  REQUIRE(HostStorageBootstrapTestAccess::Record(root,"bootstrap.pending.v1",other,kBootstrapNonce).error);
  REQUIRE(BootstrapBytes(partial)==preserved);
  const auto listed=HostStorageBootstrapTestAccess::List(root,error);REQUIRE_FALSE(error);REQUIRE(listed.size()==2);
  REQUIRE(chmod((scratch.path/"bootstrap.pending.v1").c_str(),0644)==0);
  REQUIRE(HostStorageBootstrapTestAccess::Record(root,"bootstrap.pending.v1",bytes,kBootstrapNext).error);
  REQUIRE(BootstrapBytes(scratch.path/"bootstrap.pending.v1")==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  REQUIRE(chmod((scratch.path/"bootstrap.pending.v1").c_str(),0600)==0);
  REQUIRE(symlink("bootstrap.pending.v1",(scratch.path/"bootstrap.commit.v1").c_str())==0);
  REQUIRE(HostStorageBootstrapTestAccess::List(root,error).empty());REQUIRE(error);
}
TEST_CASE("Bootstrap admission denial follows genuine native capacity and leaves no new artifact",
          "[storage_policy][bootstrap][space][refusal]") {
  Scratch scratch;auto root=BootstrapNativeRoot(scratch);
  std::ofstream(scratch.path/"sentinel")<<"original bytes";
  const auto original=BootstrapBytes(scratch.path/"sentinel");
  const std::array<uint8_t,1> bytes{42};
  HostStorageBootstrapTestAccess::Stop(root,4);
  const auto denied=HostStorageBootstrapTestAccess::Record(root,"bootstrap.pending.v1",bytes,kBootstrapNonce);
  REQUIRE(denied.error==std::errc::no_space_on_device);
  REQUIRE_FALSE(denied.published);REQUIRE_FALSE(denied.durable);
  REQUIRE(std::distance(std::filesystem::directory_iterator(scratch.path),std::filesystem::directory_iterator{})==1);
  REQUIRE(BootstrapBytes(scratch.path/"sentinel")==original);
  const auto retry=HostStorageBootstrapTestAccess::Record(root,"bootstrap.pending.v1",bytes,kBootstrapNonce);
  REQUIRE_FALSE(retry.error);REQUIRE(retry.durable);
}

TEST_CASE("Bootstrap metadata never adopts native aliases or admits an unvalidated marker",
          "[storage_policy][bootstrap][metadata][refusal]") {
  Scratch scratch;auto root=BootstrapNativeRoot(scratch);std::error_code error;
  const std::array<uint8_t,3> bytes{7,0,8};
  auto metadata=HostStorageBootstrapTestAccess::Metadata(root,bytes,kBootstrapNonce,error);
  REQUIRE(metadata);REQUIRE_FALSE(error);REQUIRE_FALSE(HostStorageBootstrapTestAccess::Guest(*metadata));
  metadata.reset();const auto canonical=scratch.path/".xenia-managed-source-v1";
  SECTION("A second native hard link is refused without changing either name") {
    REQUIRE(link(canonical.c_str(),(scratch.path/"original-control-alias").c_str())==0);
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Metadata(root,{},kBootstrapNext,error));REQUIRE(error);
    REQUIRE(BootstrapBytes(canonical)==std::vector<uint8_t>(bytes.begin(),bytes.end()));
    REQUIRE(BootstrapBytes(scratch.path/"original-control-alias")==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  }
  SECTION("A canonical symlink is refused without following it") {
    std::filesystem::rename(canonical,scratch.path/"original-control");
    REQUIRE(symlink("original-control",canonical.c_str())==0);
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Metadata(root,{},kBootstrapNext,error));REQUIRE(error);
    REQUIRE(std::filesystem::is_symlink(canonical));
    REQUIRE(BootstrapBytes(scratch.path/"original-control")==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  }
}
TEST_CASE("Bootstrap record inventory refuses an oversized native directory without deleting rows",
          "[storage_policy][bootstrap][record][bounds]") {
  Scratch scratch;auto root=BootstrapNativeRoot(scratch);std::error_code error;
  for(unsigned i=0;i!=65;++i) {
    if(i==64) { REQUIRE(HostStorageBootstrapTestAccess::List(root,error).size()==64);REQUIRE_FALSE(error); }
    auto nonce=std::string(kBootstrapNonce);
    constexpr char hex[]="0123456789abcdef";
    nonce[0]=hex[i/16];nonce[1]=hex[i%16];
    const auto file=scratch.path/("bootstrap-preserve-"+nonce+".v1");
    std::ofstream(file,std::ios::binary)<<"preserved";
    REQUIRE(chmod(file.c_str(),0600)==0);
  }
  REQUIRE(HostStorageBootstrapTestAccess::List(root,error).empty());REQUIRE(error);
  const std::array<uint8_t,1> bytes{0};
  REQUIRE(HostStorageBootstrapTestAccess::Record(root,"bootstrap.commit.v1",bytes,kBootstrapNext).error);
  REQUIRE(std::distance(std::filesystem::directory_iterator(scratch.path),std::filesystem::directory_iterator{})==65);
}

TEST_CASE("Resumed bootstrap transfers its exact registered family while an unrelated guest retains old protectors",
          "[storage_policy][bootstrap][directory][retained_guest]") {
  Scratch scratch, unrelated_scratch, foreign_scratch;
  auto parent=BootstrapNativeRoot(scratch);
  auto unrelated=BootstrapNativeRoot(unrelated_scratch);
  std::error_code error;
  auto unrelated_guest=unrelated->GuestView(error);REQUIRE(unrelated_guest);REQUIRE_FALSE(error);
  REQUIRE(chmod(foreign_scratch.path.c_str(),0700)==0);
  auto foreign=BootstrapNativeRoot(foreign_scratch);
  auto foreign_protector=foreign->ProtectFlat(error);REQUIRE(foreign_protector);REQUIRE_FALSE(error);
  uint64_t directory=0;std::filesystem::path stage_leaf;
  const std::array<uint8_t,4> owner{5,0,8,0xff};
  {
    auto initial=HostStorageBootstrapTestAccess::Begin(parent,"public-owner",kBootstrapNonce,error);
    REQUIRE(initial);REQUIRE_FALSE(error);
    directory=initial->native_directory();stage_leaf=initial->staging();
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Record(initial->root(),"owner.v1",owner,kBootstrapNonce).error);
    const auto published=HostStorageBootstrapTestAccess::Publish(parent,*initial);
    REQUIRE_FALSE(published.error);REQUIRE(published.published);REQUIRE(published.durable);
    REQUIRE_FALSE(published.canonical_root->GuestView(error));REQUIRE(error);
  }  // capsule, receipt and canonical construction root all die; guest stays
  auto resumed=HostStorageBootstrapTestAccess::Resume(parent,"public-owner",stage_leaf,directory,error);
  REQUIRE(resumed);REQUIRE_FALSE(error);
  const auto observed=HostStorageBootstrapTestAccess::Publish(parent,*resumed);
  REQUIRE_FALSE(observed.error);REQUIRE_FALSE(observed.published);REQUIRE(observed.durable);
  // An independent flat policy is not interchangeable with this receipt.
  REQUIRE_FALSE(resumed->root()->ProtectFlat(error));REQUIRE(error);
  auto public_view=HostStorageBootstrapTestAccess::Admit(parent,*resumed,error);
  REQUIRE(public_view);REQUIRE_FALSE(error);
  std::vector<uint8_t> read;
  REQUIRE_FALSE(public_view->ReadFile("owner.v1",32,read));
  REQUIRE(read==std::vector<uint8_t>(owner.begin(),owner.end()));
  REQUIRE_FALSE(public_view->WriteNewFile("saved.data",owner).error);
  REQUIRE_FALSE(unrelated_guest->WriteNewFile("ordinary.data",owner).error);
  // Exact group retirement must leave unrelated private authority enforced.
  REQUIRE_FALSE(foreign->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  auto reopened=parent->Subroot("public-owner",false,error);REQUIRE(reopened);REQUIRE_FALSE(error);
  auto reopened_guest=reopened->GuestView(error);REQUIRE(reopened_guest);REQUIRE_FALSE(error);
  REQUIRE_FALSE(reopened_guest->ReadFile("saved.data",32,read));
  REQUIRE(read==std::vector<uint8_t>(owner.begin(),owner.end()));
}

TEST_CASE("Media private handoff requires its complete durable exact bootstrap receipt",
          "[storage_policy][bootstrap][directory][media]") {
  const unsigned variant=GENERATE(0u,1u,2u,3u);
  Scratch scratch, foreign_scratch;
  auto parent=BootstrapNativeRoot(scratch);
  auto foreign=BootstrapNativeRoot(foreign_scratch);
  std::error_code error;
  const auto target=variant==1 ? "other-private" : ".xenia-media-v1";
  auto capsule=HostStorageBootstrapTestAccess::Begin(parent,target,kBootstrapNonce,error);
  REQUIRE(capsule);REQUIRE_FALSE(error);
  const std::string owner=variant==2 ? "foreign-owner" : "XENIA-MEDIA-REFERENCES-1\n";
  const auto bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(owner.data()),owner.size());
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Record(capsule->root(),"owner.v1",bytes,kBootstrapNonce).error);
  // Neither a staged capsule nor a foreign parent can authorize this role.
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Media(parent,*capsule,error));
  REQUIRE(error==std::errc::permission_denied);
  if (variant==3) { HostStorageBootstrapTestAccess::Stop(parent,3); }
  auto published=HostStorageBootstrapTestAccess::Publish(parent,*capsule);
  REQUIRE(published.canonical_root);REQUIRE(published.published);
  if (variant==3) {
    REQUIRE(published.error==std::errc::operation_canceled);REQUIRE_FALSE(published.durable);
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Media(parent,*capsule,error));
    REQUIRE(error==std::errc::permission_denied);
    // The uncertain canonical protector is still real and blocks guest use.
    REQUIRE_FALSE(published.canonical_root->GuestView(error));REQUIRE(error);
    published=HostStorageBootstrapTestAccess::Publish(parent,*capsule);
    REQUIRE_FALSE(published.published);
  }
  REQUIRE_FALSE(published.error);REQUIRE(published.durable);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Media(foreign,*capsule,error));
  REQUIRE(error==std::errc::permission_denied);
  REQUIRE_FALSE(published.canonical_root->ProtectFlat(error));REQUIRE(error);
  auto flat=HostStorageBootstrapTestAccess::Media(parent,*capsule,error);
  if (variant==1 || variant==2) {
    REQUIRE_FALSE(flat);REQUIRE(error);
    REQUIRE_FALSE(published.canonical_root->GuestView(error));REQUIRE(error);
    return;
  }
  REQUIRE(flat);REQUIRE_FALSE(error);
  REQUIRE_FALSE(flat->GuestView(error));REQUIRE(error);
  // The typed handoff cannot later become public or settle a second time.
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Admit(parent,*capsule,error));
  REQUIRE(error==std::errc::permission_denied);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Media(parent,*capsule,error));
  REQUIRE(error==std::errc::permission_denied);
  // Actual parent EX survives protection handoff and ends with the capsule.
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Lease(parent,error));
  REQUIRE(error==std::errc::device_or_resource_busy);
  capsule.reset();
  auto lease=HostStorageBootstrapTestAccess::Lease(parent,error);
  REQUIRE(lease);REQUIRE_FALSE(error);lease.reset();
  // General reopen reuses the existing flat owner, then works after retirement.
  auto reopened=parent->Subroot(target,false,error);REQUIRE(reopened);REQUIRE_FALSE(error);
  auto reused=reopened->ProtectFlat(error);REQUIRE(reused);REQUIRE_FALSE(error);
  reused.reset();flat.reset();
  auto restored=reopened->ProtectFlat(error);REQUIRE(restored);REQUIRE_FALSE(error);
  REQUIRE_FALSE(restored->GuestView(error));REQUIRE(error);
  std::vector<uint8_t> read;
  REQUIRE_FALSE(restored->ReadFile("owner.v1",128,read));
  REQUIRE(read==std::vector<uint8_t>(bytes.begin(),bytes.end()));
}

TEST_CASE("Bootstrap projection settles exact private roles but cannot admit guests without real recovered coordinator owners",
          "[storage_policy][bootstrap][projection][native]") {
  Scratch scratch, unrelated_scratch;
  auto source=BootstrapNativeRoot(scratch);auto unrelated=BootstrapNativeRoot(unrelated_scratch);
  std::error_code error;auto unrelated_guest=unrelated->GuestView(error);
  REQUIRE(unrelated_guest);REQUIRE_FALSE(error);
  auto source_lease=HostStorageBootstrapTestAccess::Lease(source,error);
  REQUIRE(source_lease);REQUIRE_FALSE(error);
  const std::array<uint8_t,3> owner{9,0,6};
  const auto make_component=[&](std::string_view name,std::string_view nonce) {
    auto capsule=HostStorageBootstrapTestAccess::Begin(source,name,nonce,error,source_lease);
    REQUIRE(capsule);REQUIRE_FALSE(error);
    REQUIRE_FALSE(HostStorageBootstrapTestAccess::Record(capsule->root(),"owner.v1",owner,nonce).error);
    const auto published=HostStorageBootstrapTestAccess::Publish(source,*capsule);
    REQUIRE_FALSE(published.error);REQUIRE(published.published);REQUIRE(published.durable);
    return capsule;
  };
  auto private_capsule=make_component("private-state",kBootstrapNonce);
  auto public_capsule=make_component("public-shell",kBootstrapNext);
  auto unrelated_private=make_component("other-private","2123456789abcdef0123456789abcdef");
  const std::array<HostStorageBootstrapDirectory*,1> settled{private_capsule.get()};
  const auto private_root=private_capsule->root(), public_root=public_capsule->root();
  // A capsule is not a caller-provided skip flag: actual exclusion coverage
  // is required, and refusal leaves the original protections unchanged.
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Projection(source,{},
      {public_root},settled,public_capsule.get(),error));REQUIRE(error);
  REQUIRE_FALSE(private_root->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  auto projection=HostStorageBootstrapTestAccess::Projection(source,{private_root},
      {public_root},settled,public_capsule.get(),error);
  REQUIRE(projection);REQUIRE_FALSE(error);
  // The unlisted other-private capsule is auto-included at its CURRENT
  // canonical location. A stale registered stage pathname would fail here.
  REQUIRE_FALSE(source->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
  REQUIRE_FALSE(private_root->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  REQUIRE(HostStorageBootstrapTestAccess::Publish(source,*private_capsule).error);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Admit(source,*public_capsule,error,unrelated.get()));REQUIRE(error);
  REQUIRE_FALSE(public_root->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  auto public_view=HostStorageBootstrapTestAccess::Admit(source,*public_capsule,error,source.get());
  REQUIRE_FALSE(public_view);REQUIRE(error==std::errc::operation_in_progress);
  REQUIRE_FALSE(source->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{source->GuestBacking()},error));
  REQUIRE(error==std::errc::operation_in_progress);
  // These lower native capsules have no actual coordinator, durable binding
  // and ordinarily recovered typed owner, so no successful release is faked.
  // Real commit/Recover/reopen admission belongs to the combined MVA fixtures.
  REQUIRE_FALSE(unrelated_guest->WriteNewFile("ordinary.data",owner).error);
  REQUIRE(link((unrelated_scratch.path/"ordinary.data").c_str(),
      (unrelated_scratch.path/"ordinary-alias.data").c_str())==0);
  // Real post-initialization private control growth must not inherit the
  // capsule's bounded bootstrap inventory policy forever. The permanent
  // projection continues to protect every native file and alias instead.
  for(unsigned i=0;i!=513;++i) {
    const auto control=private_root->path()/("journal-fixture-"+std::to_string(i));
    std::ofstream(control,std::ios::binary)<<"x";REQUIRE(chmod(control.c_str(),0600)==0);
  }
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(unrelated_guest->ReadFile("ordinary.data",32,bytes));
  REQUIRE(bytes==std::vector<uint8_t>(owner.begin(),owner.end()));
  REQUIRE_FALSE(private_root->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  REQUIRE_FALSE(source->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
  REQUIRE_FALSE(unrelated_guest->WriteNewFile("still-usable.data",owner).error);
}

TEST_CASE("Bootstrap guest seal covers child raw and reopened admissions without new capsules",
          "[storage_policy][bootstrap][guest_seal][native]") {
  Scratch scratch, public_scratch, unrelated_scratch;
  auto source=BootstrapNativeRoot(scratch), shell=BootstrapNativeRoot(public_scratch);
  auto unrelated=BootstrapNativeRoot(unrelated_scratch);
  std::error_code error;const std::array<uint8_t,3> bytes{3,0,7};
  REQUIRE_FALSE(source->Create("child",true));
  REQUIRE_FALSE(source->WriteNewFile("source.data",bytes).error);
  auto child=source->Subroot("child",false,error);REQUIRE(child);REQUIRE_FALSE(error);
  auto reopened=HostStorageRoot::Open(scratch.path,false,error);REQUIRE(reopened);REQUIRE_FALSE(error);
  // A committed reopen can have zero new native capsules. It is still the
  // bootstrap factory role, and cannot inherit ordinary admitted behavior.
  auto projection=HostStorageBootstrapTestAccess::Projection(source,{}, {shell},{},nullptr,error);
  REQUIRE(projection);REQUIRE_FALSE(error);
  REQUIRE_FALSE(source->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
  REQUIRE_FALSE(child->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
  REQUIRE_FALSE(reopened->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
  REQUIRE_FALSE(shell->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
  for (const auto& backing : std::vector<GuestHostBacking>{
      {scratch.path,true},{scratch.path/"child",true},
      {scratch.path/"source.data",false},{public_scratch.path,true}}) {
    REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(std::vector<GuestHostBacking>{backing},error));
    REQUIRE(error==std::errc::operation_in_progress);
  }
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(std::nullopt,error));
  REQUIRE(error==std::errc::operation_in_progress);
  auto unrelated_guest=unrelated->GuestView(error);REQUIRE(unrelated_guest);REQUIRE_FALSE(error);
  REQUIRE_FALSE(unrelated_guest->WriteNewFile("unrelated.data",bytes).error);
  std::vector<uint8_t> actual;REQUIRE_FALSE(source->ReadFile("source.data",32,actual));
  REQUIRE(actual==std::vector<uint8_t>(bytes.begin(),bytes.end()));
}

TEST_CASE("Ordinary projection guest admission keeps its existing default behavior",
          "[storage_policy][projection][guest_seal][native]") {
  Scratch scratch;auto source=BootstrapNativeRoot(scratch);std::error_code error;
  auto projection=HostStorageBootstrapTestAccess::OrdinaryProjection(source,error);
  REQUIRE(projection);REQUIRE_FALSE(error);
  auto guest=source->GuestView(error);REQUIRE(guest);REQUIRE_FALSE(error);
  auto raw=HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{scratch.path,true}},error);
  REQUIRE(raw);REQUIRE_FALSE(error);
  const std::array<uint8_t,2> bytes{4,9};
  REQUIRE_FALSE(guest->WriteNewFile("ordinary.data",bytes).error);
  std::vector<uint8_t> actual;REQUIRE_FALSE(guest->ReadFile("ordinary.data",32,actual));
  REQUIRE(actual==std::vector<uint8_t>(bytes.begin(),bytes.end()));
}

TEST_CASE("Bootstrap guest seal cannot be installed beneath a live raw source or public guest",
          "[storage_policy][bootstrap][guest_seal][native]") {
  Scratch scratch, public_scratch;
  auto source=BootstrapNativeRoot(scratch), shell=BootstrapNativeRoot(public_scratch);
  std::error_code error;
  const bool public_backing=GENERATE(false,true);
  auto admitted=HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{public_backing?public_scratch.path:scratch.path,true}},error);
  REQUIRE(admitted);REQUIRE_FALSE(error);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Projection(source,{}, {shell},{},nullptr,error));
  REQUIRE(error==std::errc::device_or_resource_busy);
  // Refusal does not install a projection or retire any native authority.
  REQUIRE_FALSE(source->has_guest_projection());
  admitted.reset();
  auto projection=HostStorageBootstrapTestAccess::Projection(source,{}, {shell},{},nullptr,error);
  REQUIRE(projection);REQUIRE_FALSE(error);
  REQUIRE_FALSE(source->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
}

TEST_CASE("Ordinary bootstrap browse directories cannot hide distinct existing native payloads",
          "[storage_policy][bootstrap][browse][native]") {
  Scratch scratch;auto source=BootstrapNativeRoot(scratch);std::error_code error;
  const std::array<uint8_t,3> bytes{8,0,4};
  REQUIRE_FALSE(source->WriteNewFile("source-payload",bytes).error);
  auto same=source->Subroot({},false,error);REQUIRE(same);REQUIRE_FALSE(error);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Browse(*same,*source));
  REQUIRE_FALSE(source->Create("public-content",true));
  REQUIRE(chmod((scratch.path/"public-content").c_str(),0755)==0);
  auto browse=source->Subroot("public-content",false,error);REQUIRE(browse);REQUIRE_FALSE(error);
  REQUIRE_FALSE(HostStorageBootstrapTestAccess::Browse(*browse,*source));
  REQUIRE_FALSE(browse->WriteNewFile("existing-payload",bytes).error);
  REQUIRE(HostStorageBootstrapTestAccess::Browse(*browse,*source)==std::errc::directory_not_empty);
  std::vector<uint8_t> actual;REQUIRE_FALSE(browse->ReadFile("existing-payload",32,actual));
  REQUIRE(actual==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  REQUIRE_FALSE(source->ReadFile("source-payload",32,actual));
  REQUIRE(actual==std::vector<uint8_t>(bytes.begin(),bytes.end()));
}

TEST_CASE("Storage capabilities reject NUL roots before native prefix probes",
          "[storage_policy][native_path]") {
  Scratch scratch;
  REQUIRE(std::filesystem::create_directory(scratch.path / "prefix"));
  const auto malformed = scratch.path / std::filesystem::path(std::string("prefix\0suffix", 13));
  std::error_code error;
  REQUIRE_FALSE(HostStorageRoot::Open(malformed, false, error));
  CHECK(error == std::errc::invalid_argument);
  REQUIRE_FALSE(HostStorageDirectoryIdentity::Capture(malformed, error));
  CHECK(error == std::errc::invalid_argument);
  REQUIRE_FALSE(HostStorageSpace::ForPath(malformed, 0, error));
  CHECK(error == std::errc::invalid_argument);
  auto space = HostStorageSpace::ForPath(scratch.path, 0, error);
  REQUIRE(space); REQUIRE_FALSE(error);
  const auto observation = space->Observe(malformed);
  CHECK_FALSE(observation.available);
  CHECK(observation.error == std::errc::invalid_argument);
  REQUIRE_FALSE(space->Begin(malformed, error));
  CHECK(error == std::errc::invalid_argument);
  REQUIRE_FALSE(space->TryBegin(malformed, error));
  CHECK(error == std::errc::invalid_argument);
  auto root = HostStorageRoot::Open(scratch.path, false, error);
  REQUIRE(root); REQUIRE_FALSE(error);
  bool matched = false;
  REQUIRE_FALSE(root->CaptureGuestDirectory(malformed, true, matched, error));
  CHECK(matched); CHECK(error == std::errc::invalid_argument);
  CHECK(std::filesystem::is_directory(scratch.path / "prefix"));
}
TEST_CASE("Storage NUL components cannot read or mutate their existing prefix",
          "[storage_policy][native_path]") {
  Scratch scratch; std::error_code error;
  auto root = HostStorageRoot::Open(scratch.path, false, error);
  REQUIRE(root); REQUIRE_FALSE(error);
  const std::array<uint8_t, 3> original{0x41, 0, 0xFE}, changed{1, 2, 3};
  REQUIRE_FALSE(root->WriteNewFile("victim", original).error);
  REQUIRE_FALSE(root->Create("directory", true));
  REQUIRE_FALSE(root->WriteNewFile("directory/item", original).error);
  const auto malformed = std::filesystem::path(std::string("victim\0suffix", 13));
  const auto directory = std::filesystem::path(std::string("directory\0suffix", 16));
  REQUIRE_FALSE(root->OpenFile(malformed, FileAccess::kGenericRead, error));
  CHECK(error);
  REQUIRE_FALSE(root->OpenFile(malformed, FileAccess::kGenericWrite, error));
  CHECK(error);
  std::vector<uint8_t> bytes{9};
  CHECK(root->ReadFile(malformed, 32, bytes));
  CHECK(bytes == std::vector<uint8_t>{9});
  const auto written = root->WriteNewFile(malformed, changed);
  CHECK(written.error); CHECK(written.bytes == 0);
  CHECK(root->Remove(malformed, false));
  CHECK(root->Rename(malformed, "moved", false));
  CHECK(root->Rename("victim", malformed, true));
  CHECK(root->Create(std::filesystem::path(std::string("created\0suffix", 14)), true));
  REQUIRE_FALSE(root->Subroot(directory, false, error)); CHECK(error);
  REQUIRE_FALSE(root->Subroot(directory / "child", false, error)); CHECK(error);
  REQUIRE_FALSE(root->CaptureSnapshot(malformed, 32, error)); CHECK(error);
  REQUIRE_FALSE(root->ReadFile("victim", 32, bytes));
  CHECK(bytes == std::vector<uint8_t>(original.begin(), original.end()));
  REQUIRE_FALSE(root->ReadFile("directory/item", 32, bytes));
  CHECK(bytes == std::vector<uint8_t>(original.begin(), original.end()));
  CHECK_FALSE(std::filesystem::exists(scratch.path / "moved"));
  CHECK_FALSE(std::filesystem::exists(scratch.path / "created"));
  // Existing empty-view and ordinary filename semantics remain usable.
  auto same = root->Subroot({}, false, error); REQUIRE(same); REQUIRE_FALSE(error);
  REQUIRE_FALSE(same->WriteNewFile("valid name", changed).error);
  REQUIRE_FALSE(same->ReadFile("valid name", 32, bytes));
  CHECK(bytes == std::vector<uint8_t>(changed.begin(), changed.end()));
}

TEST_CASE("Retained package layout is metadata-only with live Files and readonly views",
          "[storage_policy][content_layout]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("package", true));
  const std::array<uint8_t, 4> bytes{2, 4, 6, 8};
  REQUIRE_FALSE(root->WriteNewFile("package/save", bytes, false).error);
  std::error_code error;
  auto writer = root->OpenFile("package/save", FileAccess::kGenericWrite, error);
  REQUIRE(writer); REQUIRE_FALSE(error);
  auto reader = root->OpenFile("package/save", FileAccess::kGenericRead, error);
  REQUIRE(reader); REQUIRE_FALSE(error);
  auto readonly = scratch.Root(true);
  auto captured = readonly->CaptureContentLayout("package", error);
  REQUIRE(captured); REQUIRE_FALSE(error);
  REQUIRE(captured->entries().size() == 2);
  const auto& payload = captured->entries().back();
  CHECK(payload.relative == "save"); CHECK_FALSE(payload.directory);
  CHECK(payload.bytes == bytes.size()); CHECK(payload.file != 0);
  const auto actual_allocation = reader->QueryAllocatedBytes(error);
  REQUIRE(actual_allocation); REQUIRE_FALSE(error);
  CHECK(captured->statistics().file_bytes == bytes.size());
  CHECK(captured->statistics().data_extent == 4096);
  CHECK(captured->statistics().allocated_bytes == *actual_allocation);
  CHECK_FALSE(captured->CheckCurrent());
  // The census holds no payload read/write lease and does not turn into an
  // exclusive overwrite or a whole-package retirement permission.
  std::array<uint8_t, 4> actual{};
  const auto read = reader->ReadResult(0, actual);
  REQUIRE_FALSE(read.error); REQUIRE(read.bytes == actual.size());
  CHECK(actual == bytes);
  // The census is metadata-only, but the ordinary reader still owns its
  // live File lease. Resize must preserve that lease until it is released.
  REQUIRE(writer->ResizeResult(12) == std::errc::device_or_resource_busy);
  CHECK_FALSE(captured->CheckCurrent());
  reader.reset();
  REQUIRE_FALSE(writer->ResizeResult(12));
  CHECK(captured->entries().back().bytes == 4);
  CHECK(captured->CheckCurrent() == std::errc::state_not_recoverable);
  auto after = readonly->CaptureContentLayout("package", error);
  REQUIRE(after); REQUIRE_FALSE(error);
  CHECK(after->entries().back().bytes == 12);
  CHECK(after->statistics().file_bytes == 12);
  CHECK(after->statistics().data_extent == 4096);
  const auto resized_allocation = writer->QueryAllocatedBytes(error);
  REQUIRE(resized_allocation); REQUIRE_FALSE(error);
  CHECK(after->statistics().allocated_bytes == *resized_allocation);
  CHECK_FALSE(after->CheckCurrent());
}

TEST_CASE("Retained package layout rejects member replacement and unsafe native kinds",
          "[storage_policy][content_layout]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("package", true));
  const std::array<uint8_t, 3> bytes{7, 8, 9};
  REQUIRE_FALSE(root->WriteNewFile("package/save", bytes, false).error);
  std::error_code error;
  auto captured = root->CaptureContentLayout("package", error);
  REQUIRE(captured); REQUIRE_FALSE(error);
  const auto original = captured->entries();
  SECTION("Equal bytes at a different retained inode are not the old layout") {
    std::filesystem::rename(scratch.path / "package/save", scratch.path / "old-save");
    REQUIRE_FALSE(root->WriteNewFile("package/save", bytes, false).error);
    CHECK(captured->CheckCurrent() == std::errc::state_not_recoverable);
    CHECK(captured->entries() == original);
    auto current = root->CaptureContentLayout("package", error);
    REQUIRE(current); REQUIRE_FALSE(error);
    CHECK(current->entries().back().file != original.back().file);
    CHECK_FALSE(current->CheckCurrent());
  }
  SECTION("A new actual member cannot borrow the old namespace census") {
    REQUIRE_FALSE(root->WriteNewFile("package/new", bytes, false).error);
    CHECK(captured->CheckCurrent() == std::errc::state_not_recoverable);
    CHECK(captured->entries() == original);
  }
  SECTION("Symlink is never adopted as an owned native extent") {
    std::filesystem::create_symlink(scratch.path / "old-save", scratch.path / "package/alias");
    CHECK_FALSE(root->CaptureContentLayout("package", error)); CHECK(error);
    CHECK(std::filesystem::is_symlink(scratch.path / "package/alias"));
  }
  SECTION("Hardlinked payload is refused without deleting either alias") {
    std::filesystem::create_hard_link(scratch.path / "package/save", scratch.path / "alias");
    CHECK_FALSE(root->CaptureContentLayout("package", error)); CHECK(error);
    CHECK(std::filesystem::exists(scratch.path / "alias"));
    CHECK(std::filesystem::file_size(scratch.path / "package/save") == bytes.size());
  }
  SECTION("Non-ASCII case stays distinct under the actual member lookup rule") {
    const std::string upper = "\xC3\x89", lower = "\xC3\xA9";
    REQUIRE_FALSE(xe::utf8::equal_case(upper, lower));
    REQUIRE_FALSE(root->WriteNewFile(std::filesystem::path("package") / upper, bytes, false).error);
    REQUIRE_FALSE(root->WriteNewFile(std::filesystem::path("package") / lower, bytes, false).error);
    auto unicode = root->CaptureContentLayout("package", error);
    REQUIRE(unicode); REQUIRE_FALSE(error);
    CHECK(unicode->entries().size() == 4);
    CHECK_FALSE(unicode->CheckCurrent());
  }
  SECTION("ASCII case collision after a UTF8 prefix matches real member lookup") {
    const std::string upper = "\xC3\xA9" "A", lower = "\xC3\xA9" "a";
    REQUIRE(xe::utf8::equal_case(upper, lower));
    REQUIRE_FALSE(root->WriteNewFile(std::filesystem::path("package") / upper, bytes, false).error);
    REQUIRE_FALSE(root->WriteNewFile(std::filesystem::path("package") / lower, bytes, false).error);
    CHECK_FALSE(root->CaptureContentLayout("package", error));
    CHECK(error == std::errc::state_not_recoverable);
  }
  SECTION("Invalid UTF8 is rejected before member Device equality can decode it") {
    const auto invalid = std::filesystem::path("package") / (std::string("bad") + char(0xFF));
    REQUIRE_FALSE(root->WriteNewFile(invalid, bytes, false).error);
    CHECK_FALSE(root->CaptureContentLayout("package", error));
    CHECK(error == std::errc::illegal_byte_sequence);
    CHECK(std::filesystem::exists(scratch.path / invalid));
  }
  SECTION("FIFO is refused without opening it for payload IO") {
    REQUIRE(mkfifo((scratch.path / "package/fifo").c_str(), 0600) == 0);
    CHECK_FALSE(root->CaptureContentLayout("package", error)); CHECK(error);
    struct stat native{};
    REQUIRE(lstat((scratch.path / "package/fifo").c_str(), &native) == 0);
    CHECK(S_ISFIFO(native.st_mode));
  }
}

TEST_CASE("Retained package layout refuses GCRC invalid paths and retired sources",
          "[storage_policy][content_layout]") {
  Scratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create("package", true));
  const std::array<uint8_t, 1> bytes{42};
  REQUIRE_FALSE(root->WriteNewFile("package/save", bytes, false).error);
  std::error_code error;
  auto captured = root->CaptureContentLayout("package", error);
  REQUIRE(captured); REQUIRE_FALSE(error);
  {
    auto namespace_lock = global_critical_region::AcquireDirect();
    CHECK_FALSE(root->CaptureContentLayout("package", error));
    CHECK(error == std::errc::operation_not_permitted);
    CHECK(captured->CheckCurrent() == std::errc::operation_not_permitted);
  }
  CHECK_FALSE(root->CaptureContentLayout("../package", error)); CHECK(error);
  CHECK_FALSE(root->CaptureContentLayout(scratch.path / "package", error)); CHECK(error);
  CHECK_FALSE(root->CaptureContentLayout("package/save", error));
  CHECK(error == std::errc::not_a_directory);
  root->Retire();
  CHECK(captured->CheckCurrent() == std::errc::no_such_device);
  CHECK(captured->entries().back().bytes == bytes.size());
  CHECK(std::filesystem::file_size(scratch.path / "package/save") == bytes.size());
}

}
#endif
