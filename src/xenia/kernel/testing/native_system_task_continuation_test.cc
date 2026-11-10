/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_system_task_continuation.h"
#include <array>
#include <memory>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_managed_format_io.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
namespace xe::kernel::test {
class NativeManagedFormatIoTestAccess {
 public:
  using Pair = NativeManagedFormatIo::Pair;
};
class NativeSystemTaskContinuationTestAccess {
 public:
  using Admission = NativeSystemTaskContinuation::Admission;
};
namespace {
using Admission = NativeSystemTaskContinuationTestAccess::Admission;
using Role = Admission::Role;
// Actual production admission/owner policy only. These tests execute no Kernel,
// guest Task, Schedule/Wait/Close, module or PPC and assert no native readiness.
void CompleteStartup(Admission& a) {
  REQUIRE(a.Claim(Role::kStartup));
  REQUIRE(a.BindOwner(0x10000, std::make_shared<int>(1)));
  a.Finish(true, true);
}
TEST_CASE("Native Task continuation preserves once-only serialized roles",
          "[kernel][native_task_continuation][owner]") {
  Admission a;
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
  REQUIRE(a.Claim(Role::kStartup));
  CHECK_FALSE(a.Claim(Role::kStartup));
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> retained = owner;
  REQUIRE(a.BindOwner(0x10000, owner));
  owner.reset();
  CHECK_FALSE(retained.expired());
  CHECK_FALSE(a.BindOwner(0x10001, std::make_shared<int>(2)));
  a.Finish(true, true);
  CHECK(retained.expired());
  CHECK_FALSE(a.Claim(Role::kStartup));
  REQUIRE(a.Claim(Role::kPostBootDiagnostic));
  REQUIRE(a.BindOwner(0x10001, std::make_shared<int>(2)));
  a.Finish(true, true);
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
  CHECK_FALSE(a.uncertain);
}
TEST_CASE("Native Task continuation cannot retarget a stale callback ticket",
          "[kernel][native_task_continuation][owner]") {
  Admission a;
  REQUIRE(a.Claim(Role::kStartup));
  REQUIRE(a.BindOwner(0x10000, std::make_shared<int>(1)));
  CHECK(a.MatchesTicket(0x10000));
  CHECK_FALSE(a.MatchesTicket(0));
  CHECK_FALSE(a.MatchesTicket(0x10001));
  a.Finish(true, true);
  CHECK_FALSE(a.MatchesTicket(0x10000));
  REQUIRE(a.Claim(Role::kPostBootDiagnostic));
  REQUIRE(a.BindOwner(0x10001, std::make_shared<int>(2)));
  CHECK_FALSE(a.MatchesTicket(0x10000));
  CHECK(a.MatchesTicket(0x10001));
  a.Finish(true, true);
  CHECK_FALSE(a.MatchesTicket(0x10001));
}
TEST_CASE("Failed normal startup does not authorize postboot work",
          "[kernel][native_task_continuation][owner]") {
  Admission a;
  REQUIRE(a.Claim(Role::kStartup));
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> retained = owner;
  REQUIRE(a.BindOwner(0x10000, owner));
  owner.reset();
  a.Finish(true, false);
  CHECK(retained.expired());
  CHECK_FALSE(a.startup_succeeded);
  CHECK_FALSE(a.uncertain);
  CHECK_FALSE(a.Claim(Role::kStartup));
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
}
TEST_CASE("Uncertain native Task retains its preowned work through stop",
          "[kernel][native_task_continuation][owner]") {
  Admission a;
  CompleteStartup(a);
  REQUIRE(a.Claim(Role::kPostBootDiagnostic));
  auto captured_buffer = std::make_shared<int>(42);
  std::weak_ptr<int> retained = captured_buffer;
  REQUIRE(a.BindOwner(0x10001, captured_buffer));
  captured_buffer.reset();
  a.Poison();
  a.Finish(false, false);
  CHECK(a.uncertain);
  CHECK_FALSE(retained.expired());
  CHECK_FALSE(a.MatchesTicket(0x10001));
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
  a.stopping = true;
  a.Finish(true, true);  // Cannot relabel a completed uncertain invocation.
  CHECK(a.uncertain);
  CHECK_FALSE(retained.expired());
  CHECK_FALSE(a.Claim(Role::kStartup));
  // Owner destruction stands only for stopped host metadata destruction,
  // not evidence of Kernel/CPU/GPU or real native Task rundown.
}
TEST_CASE("Stop before Task owner publication refuses native entry",
          "[kernel][native_task_continuation][owner]") {
  Admission a;
  REQUIRE(a.Claim(Role::kStartup));
  a.stopping = true;
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> retained = owner;
  CHECK_FALSE(a.BindOwner(0x10000, owner));
  owner.reset();
  CHECK(retained.expired());
  a.Finish(true, false);
  CHECK_FALSE(a.owner);
  CHECK_FALSE(a.uncertain);
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
}
TEST_CASE("Stop during admitted Task keeps ownership until normal completion",
          "[kernel][native_task_continuation][owner]") {
  Admission a;
  CompleteStartup(a);
  REQUIRE(a.Claim(Role::kPostBootDiagnostic));
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> retained = owner;
  REQUIRE(a.BindOwner(0x10001, owner));
  owner.reset();
  a.stopping = true;
  CHECK_FALSE(retained.expired());
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
  a.Finish(true, true);
  CHECK(retained.expired());
  CHECK_FALSE(a.uncertain);
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
}
TEST_CASE("Normal postboot rejection consumes the role without quarantine",
          "[kernel][native_task_continuation][owner]") {
  Admission a;
  CompleteStartup(a);
  REQUIRE(a.Claim(Role::kPostBootDiagnostic));
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> retained = owner;
  REQUIRE(a.BindOwner(0x10001, owner));
  owner.reset();
  a.Finish(true, false);
  CHECK(retained.expired());
  CHECK_FALSE(a.uncertain);
  CHECK_FALSE(a.Claim(Role::kPostBootDiagnostic));
}
TEST_CASE("Managed native IO profiles remain scoped and preserve DWORD ABI",
          "[kernel][native_managed_format][abi]") {
  for (auto call : {NativeSystemUiPublicCall::kManagedFormatStart,
                    NativeSystemUiPublicCall::kManagedFormatCancel}) {
    const auto* profile = NativeSystemUiPublicProfile(call);
    REQUIRE(profile);
    CHECK(profile->family == NativeSystemPublicFamily::kManagedFormatIo);
    CHECK_FALSE(NativeSystemPublicFamilyRouteEnabled(profile->family));
    CHECK(profile->uint64_register_mask == 0);
  }
  const std::array<uint64_t, 6> args{0xFE,       0x2000B, 0x90001020,
                                     0x90001058, 4,       0x90001060};
  REQUIRE(NativeSystemUiPublicArgumentsValid(
      NativeSystemUiPublicCall::kManagedFormatStart, args));
  auto invalid = args;
  invalid[3] += UINT64_C(0x100000000);
  CHECK_FALSE(NativeSystemUiPublicArgumentsValid(
      NativeSystemUiPublicCall::kManagedFormatStart, invalid));
  CHECK_FALSE(NativeSystemUiPublicArgumentsValid(
      NativeSystemUiPublicCall::kManagedFormatStart,
      std::span<const uint64_t>(args).first(5)));
  CHECK_FALSE(NativeSystemUiPublicArgumentsValid(
      NativeSystemUiPublicCall::kManagedFormatCancel, args));
}
TEST_CASE(
    "Managed native owned-copy pairing rejects abandoned and replayed returns",
    "[kernel][native_managed_format][lifetime]") {
  // This is the real production pairing helper, not a native Task/selection
  // factory. Values only name RAM observations; they never grant authority.
  using Pair = NativeManagedFormatIoTestAccess::Pair;
  Pair normal;
  REQUIRE(normal.Enter(0x90001000, 0x816A2D30, 0x90001050));
  REQUIRE(normal.Bind(0x90001000, 0x816A2D30, 0x40100000));
  CHECK(normal.packet == 0x40100000);
  CHECK_FALSE(normal.Bind(0x90001000, 0x816A2D30, 0x40100000));
  CHECK(normal.poisoned);
  Pair abandoned;
  REQUIRE(abandoned.Enter(0x90001000, 0x816A2D30, 0x90001050));
  CHECK_FALSE(abandoned.Enter(0x90001000, 0x816A2D30, 0x90001050));
  CHECK_FALSE(abandoned.Bind(0x90001000, 0x816A2D30, 0x40100000));
  CHECK_FALSE(abandoned.bound);
  Pair wrong_stack;
  REQUIRE(wrong_stack.Enter(0x90001000, 0x816A2D30, 0x90001050));
  CHECK_FALSE(wrong_stack.Bind(0x90002000, 0x816A2D30, 0x40100000));
  CHECK_FALSE(wrong_stack.bound);
}
TEST_CASE(
    "Managed native completion requires its own one-use normal-return pair",
    "[kernel][native_managed_format][lifetime]") {
  using Pair = NativeManagedFormatIoTestAccess::Pair;
  Pair completion;
  CHECK_FALSE(completion.Bind(0x90001000, 0x81687F54, 0x90002000));
  CHECK(completion.poisoned);
  Pair pending;
  REQUIRE(pending.Enter(0x90001000, 0x81687F54, 0x90002000));
  CHECK_FALSE(pending.Bind(0x90001000, 0x816A2D30, 0x90002000));
  CHECK_FALSE(pending.bound);
  Pair normal;
  REQUIRE(normal.Enter(0x90001000, 0x81687F54, 0x90002000));
  REQUIRE(normal.Bind(0x90001000, 0x81687F54, 0x90002000));
  CHECK(normal.bound);
  CHECK_FALSE(normal.poisoned);
}

}  // namespace
}  // namespace xe::kernel::test
