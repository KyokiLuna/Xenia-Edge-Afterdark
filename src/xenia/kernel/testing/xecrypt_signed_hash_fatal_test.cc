/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/filesystem.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace xe::kernel::xboxkrnl::test {
namespace {
struct Process {
  pid_t pid = -1;
  ~Process() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      int status;
      waitpid(pid, &status, 0);
    }
  }
  int Run() {
    const auto executable = filesystem::GetExecutablePath().string();
    std::vector<std::string> strings;
    for (char** env = environ; *env; ++env) {
      if (!std::string_view(*env).starts_with(
              "XENIA_SIGNED_HASH_FATAL_CHILD=")) {
        strings.emplace_back(*env);
      }
    }
    strings.push_back("XENIA_SIGNED_HASH_FATAL_CHILD=1");
    std::vector<char*> environment;
    for (auto& s : strings) {
      environment.push_back(s.data());
    }
    environment.push_back(nullptr);
    std::array<std::string, 4> args{executable, "[.signed_hash_fatal_probe]",
                                    "--reporter", "compact"};
    std::array<char*, 5> argv{args[0].data(), args[1].data(), args[2].data(),
                              args[3].data(), nullptr};
    if (posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(),
                    environment.data())) {
      return -1;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto done = waitpid(pid, &status, WNOHANG);
      if (done == pid) {
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
      if (done < 0) {
        pid = -1;
        return -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
  }
};
}  // namespace
TEST_CASE("RotSum void unavailable provider fails closed in a real subprocess",
          "[kernel][crypt][signed_hash]") {
  Process child;
  CHECK(child.Run() == EXIT_FAILURE);
}
TEST_CASE("RotSum fatal trampoline subprocess helper",
          "[.signed_hash_fatal_probe]") {
  if (!std::getenv("XENIA_SIGNED_HASH_FATAL_CHILD")) {
    return;
  }
  cpu::ExportResolver resolver;
  RegisterCryptExports(&resolver, nullptr);
  XboxkrnlModule::RegisterExportTable(&resolver);
  auto* entry = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x18E);
  if (!entry || !entry->function_data.trampoline) {
    std::_Exit(90);
  }
  PPCContext context{};
  context.r[3] = 0xFFFFFFFF00000001ull;
  context.r[4] = 1;
  context.r[7] = 0x90040000;
  context.r[8] = 20;
  // Actual export, no fake KernelState. Ownership absence must not return a
  // stale meaningful digest; fallback-through or test-code error uses90/91.
  entry->function_data.trampoline(&context);
  std::_Exit(91);
}
}  // namespace xe::kernel::xboxkrnl::test
#endif
