/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <iterator>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xevent.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/file.h"
#include "xenia/memory.h"

DECLARE_int32(log_level);
DECLARE_bool(native_system_ui_trace);

namespace xe::kernel::xam::test {
namespace {
struct ProfileAbiRoot {
  std::filesystem::path path;
  ProfileAbiRoot() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-profile-abi-" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~ProfileAbiRoot() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
// Real Kernel and Memory authority, no signed-in user, guest title or PPC.
// Setup alone suffices; no audio/GPU subsystem or guest worker is launched.
struct ProfileAbiRuntime {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;
  ProfileAbiRoot root;
  std::unique_ptr<Emulator> emulator;
  ProfileAbiRuntime() {
    REQUIRE(KernelState::shared() == nullptr);
    REQUIRE(std::filesystem::create_directory(root.path / "content"));
    REQUIRE(std::filesystem::create_directory(root.path / "cache"));
    emulator = std::make_unique<Emulator>(
        "", root.path, root.path / "content", root.path / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_SUCCESS);
    REQUIRE(KernelState::shared() == emulator->kernel_state());
    for (uint32_t slot = 0; slot != 4; ++slot) {
      REQUIRE_FALSE(emulator->kernel_state()->xam_state()->GetUserProfile(slot));
    }
  }
  ~ProfileAbiRuntime() { emulator.reset(); }
};
}  // namespace

TEST_CASE("Profile ReadEx treats seventh argument as data and eighth as size",
          "[kernel][profile][abi]") {
  ProfileAbiRuntime runtime;
  auto& memory = *runtime.emulator->memory();
  const uint32_t page = memory.SystemHeapAlloc(0x1000, 0x1000);
  REQUIRE(page);
  memory.Zero(page, 0x1000);
  const auto* entry = runtime.emulator->export_resolver()->GetExportByOrdinal(
      "xam.xex", 0x22A);
  REQUIRE(entry);
  REQUIRE(entry->function_data.trampoline);

  for (const uint32_t options : {1u, 0xFFFFFFFFu}) {
    PPCContext context{};
    context.virtual_membase = memory.virtual_membase();
    context.r[1] = page + 0x800;
    context.r[4] = XUserIndexNone;
    context.r[7] = 1;        // One setting.
    context.r[9] = options;  // Deliberately not a readable guest address.
    context.r[10] = 0;
    entry->function_data.trampoline(&context);
    CHECK(uint32_t(context.r[3]) == X_ERROR_INVALID_PARAMETER);

    // The actual size-query result must be written to the r10 pointer. This
    // stops before user lookup. Actual Kernel Memory still owns checked I/O;
    // the genuine runtime has no signed-in profile.
    auto* size = memory.TranslateVirtual<xe::be<uint32_t>*>(page + 8);
    *size = 0;
    *memory.TranslateVirtual<xe::be<uint32_t>*>(page) =
        static_cast<uint32_t>(UserSettingId::XPROFILE_GAMER_YAXIS_INVERSION);
    context.r[3] = 0;
    context.r[8] = page;
    context.r[10] = page + 8;
    entry->function_data.trampoline(&context);
    CHECK(uint32_t(context.r[3]) == X_ERROR_INSUFFICIENT_BUFFER);
    CHECK(*size == sizeof(X_USER_READ_PROFILE_SETTINGS) +
                       sizeof(X_USER_PROFILE_SETTING));
  }
  memory.SystemHeapFree(page);
}

TEST_CASE("Missing profile binary data cannot retain a stale guest pointer",
          "[kernel][profile][settings]") {
  X_XDBF_GPD_SETTING_HEADER stored{};
  stored.setting_id = 0x63E80044;  // Real 1000-byte avatar appearance key.
  stored.setting_type = X_USER_DATA_TYPE::BINARY;
  stored.base_data.binary.size = 1000;
  stored.base_data.binary.ptr = 0xFFFFFFFF;
  UserSetting setting(&stored, {});  // Truncated/absent extended payload.
  X_USER_PROFILE_SETTING output{};
  uint32_t destination = 0xDEAD0000;
  setting.WriteToGuest(&output, destination);
  CHECK(output.data.data.binary.size == 0);
  CHECK(output.data.data.binary.ptr == 0);
  CHECK(destination == 0xDEAD0000 + 1000);
}


namespace {
// Genuine Kernel/Memory/export trampolines, with two explicitly allocated guest
// PCR/KTHREAD ABI storage sets. This is not an XThread/native Task factory and
// executes no PPC. In particular the host test thread has no XThread TLS.
struct LastErrorAbiStorage {
  Memory& memory;
  uint32_t allocation;
  std::array<PPCContext, 2> contexts{};

  explicit LastErrorAbiStorage(Emulator& emulator)
      : memory(*emulator.memory()),
        allocation(memory.SystemHeapAlloc(0x6000, 0x1000)) {
    REQUIRE(allocation);
    std::memset(memory.TranslateVirtual(allocation), 0xCD, 0x6000);
    for (uint32_t i = 0; i != contexts.size(); ++i) {
      contexts[i].kernel_state = emulator.kernel_state();
      contexts[i].virtual_membase = memory.virtual_membase();
      contexts[i].r[1] = allocation + 0x4F00;
      contexts[i].r[13] = Pcr(i);
      Word(Pcr(i) + 0x100) = Thread(i);
      Word(Pcr(i) + 0x150) = 0;
      Word(Error(i)) = 0x11223344 + i;
    }
  }
  ~LastErrorAbiStorage() { memory.SystemHeapFree(allocation); }
  uint32_t Pcr(uint32_t i) const { return allocation + i * 0x1000; }
  uint32_t Thread(uint32_t i) const { return allocation + (2 + i) * 0x1000; }
  uint32_t Error(uint32_t i) const { return Thread(i) + 0x160; }
  xe::be<uint32_t>& Word(uint32_t address) {
    return *memory.TranslateVirtual<xe::be<uint32_t>*>(address);
  }
  std::vector<uint8_t> Snapshot() {
    const auto* begin = memory.TranslateVirtual(allocation);
    return {begin, begin + 0x5000};  // Sixth page may be an actual guard page.
  }
  uint64_t Call(Emulator& emulator, uint16_t ordinal, uint32_t i,
                uint64_t argument = 0) {
    const auto* entry =
        emulator.export_resolver()->GetExportByOrdinal("xam.xex", ordinal);
    REQUIRE(entry);
    REQUIRE(entry->function_data.trampoline);
    contexts[i].r[3] = argument;
    entry->function_data.trampoline(&contexts[i]);
    return contexts[i].r[3];
  }
};
}  // namespace

TEST_CASE("Native last-error exports use the passed PCR and preserve setter r3",
          "[kernel][last_error][abi]") {
  ProfileAbiRuntime runtime;
  LastErrorAbiStorage f(*runtime.emulator);
  auto before = f.Snapshot();
  constexpr uint64_t value = 0x12345678FEDCBA98ull;
  // Recursively held GCRC is a legitimate kernel-call context, not a reason to
  // drop this nonwaiting, ordinary-virtual error-word update.
  {
    auto lock = global_critical_region::AcquireDirect();
    CHECK(f.Call(*runtime.emulator, 0x3EF, 0, value) == value);
  }
  CHECK(f.Word(f.Error(0)) == 0xFEDCBA98);
  CHECK(f.Word(f.Error(1)) == 0x11223345);
  CHECK(f.Call(*runtime.emulator, 0x3EE, 0) == 0xFEDCBA98ull);
  CHECK(f.Call(*runtime.emulator, 0x416, 0) == 0xFEDCBA98ull);
  CHECK(f.Call(*runtime.emulator, 0x3EE, 1) == 0x11223345ull);
  const std::array<uint8_t, 4> expected{0xFE, 0xDC, 0xBA, 0x98};
  CHECK(std::memcmp(f.memory.TranslateVirtual(f.Error(0)), expected.data(), 4) ==
        0);
  std::copy(expected.begin(), expected.end(),
            before.begin() + (f.Error(0) - f.allocation));
  CHECK(f.Snapshot() == before);
}

TEST_CASE("Native last-error inhibition preserves storage but not NT mapping",
          "[kernel][last_error][abi]") {
  ProfileAbiRuntime runtime;
  LastErrorAbiStorage f(*runtime.emulator);
  for (uint32_t inhibit : {1u, 0x80000000u, 0xFFFFFFFFu}) {
    f.Word(f.Pcr(0) + 0x150) = inhibit;
    const auto before = f.Snapshot();
    constexpr uint64_t value = 0xA1B2C3D455667788ull;
    CHECK(f.Call(*runtime.emulator, 0x3EF, 0, value) == value);
    CHECK(f.Call(*runtime.emulator, 0x3EE, 0) == 0);
    CHECK(f.Call(*runtime.emulator, 0x3F0, 0, X_STATUS_INVALID_HANDLE) ==
          X_ERROR_INVALID_HANDLE);
    CHECK(f.Snapshot() == before);
    CHECK(f.Call(*runtime.emulator, 0x3EE, 1) == 0x11223345);
  }
  f.Word(f.Pcr(0) + 0x150) = 0;
  CHECK(f.Call(*runtime.emulator, 0x3F0, 0, X_STATUS_INVALID_HANDLE) ==
        X_ERROR_INVALID_HANDLE);
  CHECK(f.Word(f.Error(0)) == X_ERROR_INVALID_HANDLE);
  CHECK(f.Word(f.Error(1)) == 0x11223345);
}

TEST_CASE("Native NT last-error mapping stores only the selected guest word",
          "[kernel][last_error][abi]") {
  ProfileAbiRuntime runtime;
  LastErrorAbiStorage f(*runtime.emulator);
  constexpr std::array<std::pair<uint32_t, uint32_t>, 3> cases{{
      {X_STATUS_SUCCESS, X_ERROR_SUCCESS},
      {X_STATUS_INVALID_HANDLE, X_ERROR_INVALID_HANDLE},
      {X_STATUS_ACCESS_DENIED, X_ERROR_ACCESS_DENIED},
  }};
  for (const auto& [status, expected] : cases) {
    auto before = f.Snapshot();
    CHECK(f.Call(*runtime.emulator, 0x3F0, 1, status) == expected);
    CHECK(f.Call(*runtime.emulator, 0x3EE, 1) == expected);
    xe::store_and_swap<uint32_t>(
        before.data() + (f.Error(1) - f.allocation), expected);
    CHECK(f.Snapshot() == before);
  }
}

TEST_CASE("Native last-error malformed contexts preserve all unrelated bytes",
          "[kernel][last_error][abi]") {
  ProfileAbiRuntime runtime;
  LastErrorAbiStorage f(*runtime.emulator);
  auto* heap = f.memory.LookupHeap(f.allocation);
  REQUIRE(heap);
  REQUIRE(heap->Decommit(f.allocation + 0x5000, 0x1000));
  const PPCContext good = f.contexts[0];
  const auto unchanged = [&] {
    const auto before = f.Snapshot();
    constexpr uint64_t value = 0x123456789ABCDEF0ull;
    CHECK(f.Call(*runtime.emulator, 0x3EF, 0, value) == value);
    CHECK(f.Call(*runtime.emulator, 0x3EE, 0) == 0);
    CHECK(f.Call(*runtime.emulator, 0x3F0, 0, X_STATUS_INVALID_HANDLE) ==
          X_ERROR_INVALID_HANDLE);
    CHECK(f.Snapshot() == before);
  };
  for (uint64_t pcr : {uint64_t(0), uint64_t(f.Pcr(0) + 1),
                       (uint64_t(1) << 32) | f.Pcr(0),
                       uint64_t(0xFFFFFF00),
                       uint64_t(f.allocation + 0x5000 - 0x150)}) {
    f.contexts[0] = good;
    f.contexts[0].r[13] = pcr;
    unchanged();
  }
  f.contexts[0] = good;
  f.contexts[0].kernel_state = nullptr;
  unchanged();
  f.contexts[0] = good;
  f.contexts[0].virtual_membase = nullptr;
  unchanged();
  f.contexts[0] = good;
  for (uint32_t thread : {0u, f.Thread(0) + 1, 0xFFFFFEA0u,
                          f.allocation + 0x5000 - 0x160}) {
    f.Word(f.Pcr(0) + 0x100) = thread;
    unchanged();
  }
  f.Word(f.Pcr(0) + 0x100) = f.Thread(0);
  REQUIRE(heap->Protect(f.Thread(0), 0x1000, kMemoryProtectRead));
  const auto before = f.Snapshot();
  constexpr uint64_t value = 0x8877665544332211ull;
  CHECK(f.Call(*runtime.emulator, 0x3EF, 0, value) == value);
  CHECK(f.Call(*runtime.emulator, 0x3F0, 0, X_STATUS_INVALID_HANDLE) ==
        X_ERROR_INVALID_HANDLE);
  CHECK(f.Call(*runtime.emulator, 0x3EE, 0) == 0x11223344);
  CHECK(f.Snapshot() == before);
  REQUIRE(heap->Protect(f.Thread(0), 0x1000,
                        kMemoryProtectRead | kMemoryProtectWrite));
}


// R3 captured these native inputs at the registered kernel13D trampoline.
// This test executes only that HLE export with genuine Kernel/Memory ownership;
// it neither executes native XAM nor claims a native formatter return, glyph,
// notification pixel, cue or UI acceptance result.
TEST_CASE(
    "Registered wide formatter expands the captured native achievement payload",
    "[kernel][strings][abi][native_notification]") {
  ProfileAbiRuntime runtime;
  auto& memory = *runtime.emulator->memory();
  struct OwnedPage {
    Memory& memory;
    uint32_t address;
    ~OwnedPage() { memory.SystemHeapFree(address); }
  } page{memory, memory.SystemHeapAlloc(0x1000, 0x1000)};
  REQUIRE(page.address);
  std::memset(memory.TranslateVirtual(page.address), 0xA5, 0x1000);

  // Preserve the literal backslash+n. Native816A9680 performs the separate
  // later newline normalization, after _snwprintf has returned.
  constexpr std::u16string_view format = u"Achievement unlocked\\n%sG - %s";
  constexpr std::u16string_view left = u"0";
  constexpr std::u16string_view right = u"Native notification diagnostic";
  constexpr std::u16string_view expected =
      u"Achievement unlocked\\n0G - Native notification diagnostic";
  constexpr std::u16string_view wrong = u"wrong argument slot";
  const uint32_t format_address = page.address + 0x80;
  const uint32_t left_address = page.address + 0x100;
  const uint32_t right_address = page.address + 0x120;
  const uint32_t wrong_address = page.address + 0x180;
  const uint32_t output_address = page.address + 0x300;
  constexpr uint32_t capacity = 255;  // Captured r4, in UTF16 code units.
  static_assert(expected.size() + 1 < capacity);
  const auto put = [&](uint32_t address, std::u16string_view text) {
    for (size_t i = 0; i <= text.size(); ++i) {
      xe::store_and_swap<uint16_t>(memory.TranslateVirtual(address + i * 2),
                                  i == text.size() ? 0 : uint16_t(text[i]));
    }
  };
  put(format_address, format);
  put(left_address, left);
  put(right_address, right);
  put(wrong_address, wrong);

  PPCContext context{};
  context.kernel_state = runtime.emulator->kernel_state();
  context.virtual_membase = memory.virtual_membase();
  context.r[1] = page.address + 0xD00;
  context.lr = 0x816AA900;  // Captured original-XAM formatter return address.
  context.r[3] = output_address;
  context.r[4] = capacity;
  context.r[5] = format_address;
  context.r[6] = left_address;   // First variadic pointer is r6, not a stack home.
  context.r[7] = right_address;  // Second variadic pointer is r7.
  for (uint32_t i = 8; i <= 10; ++i) context.r[i] = wrong_address;
  // Valid contrasting strings make a wrong register/stack start fail by bytes
  // instead of depending on a host access violation as the test oracle.
  for (uint32_t offset = 0x50; offset < 0x90; offset += 8) {
    xe::store_and_swap<uint64_t>(
        memory.TranslateVirtual(uint32_t(context.r[1]) + offset),
        uint64_t(wrong_address));
  }
  std::array<uint8_t, 0x1000> expected_page;
  std::memcpy(expected_page.data(), memory.TranslateVirtual(page.address),
              expected_page.size());
  for (size_t i = 0; i <= expected.size(); ++i) {
    xe::store_and_swap<uint16_t>(
        expected_page.data() + (output_address - page.address) + i * 2,
        i == expected.size() ? 0 : uint16_t(expected[i]));
  }

  const auto* entry = runtime.emulator->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", 0x13D);
  REQUIRE(entry);
  REQUIRE(entry->function_data.trampoline);
  entry->function_data.trampoline(&context);

  CHECK(context.r[3] == expected.size());
  const auto* output = memory.TranslateVirtual(output_address);
  // Independent numeric endian check, then every expanded UTF16 code unit,
  // the explicit terminating NUL and the entire page's unchanged sentinels.
  CHECK(output[0] == 0x00);
  CHECK(output[1] == 0x41);
  CHECK(output[2] == 0x00);
  CHECK(output[3] == 0x63);
  for (size_t i = 0; i != expected.size(); ++i) {
    CAPTURE(i);
    CHECK(xe::load_and_swap<uint16_t>(output + i * 2) == expected[i]);
  }
  CHECK(xe::load_and_swap<uint16_t>(output + expected.size() * 2) == 0);
  CHECK(xe::load_and_swap<uint16_t>(output + 20 * 2) == uint16_t('\\'));
  CHECK(xe::load_and_swap<uint16_t>(output + 21 * 2) == uint16_t('n'));
  CHECK(xe::load_and_swap<uint16_t>(output - 2) == 0xA5A5);
  CHECK(xe::load_and_swap<uint16_t>(output + capacity * 2) == 0xA5A5);
  CHECK(std::memcmp(memory.TranslateVirtual(page.address), expected_page.data(),
                    expected_page.size()) == 0);
}


namespace {
// Actual Kernel, owned Memory and registered export trampolines. The payloads
// are counted ABI data; no native caller or retail pointer-fault behavior is
// manufactured by this fixture.
struct CountedStringAbiStorage {
  static constexpr uint32_t kBytes = 0x30000;
  Emulator& emulator;
  Memory& memory;
  uint32_t allocation;
  PPCContext context{};

  explicit CountedStringAbiStorage(Emulator& owner)
      : emulator(owner),
        memory(*owner.memory()),
        allocation(memory.SystemHeapAlloc(kBytes, 0x1000)) {
    REQUIRE(allocation);
    std::memset(memory.TranslateVirtual(allocation), 0xA5, kBytes);
    context.kernel_state = emulator.kernel_state();
    context.virtual_membase = memory.virtual_membase();
    context.r[1] = allocation + kBytes - 0x100;
  }
  ~CountedStringAbiStorage() { memory.SystemHeapFree(allocation); }
  uint32_t DestinationHeader() const { return allocation + 0x100; }
  uint32_t SourceHeader() const { return allocation + 0x120; }
  uint32_t Destination() const { return allocation + 0x1000; }
  uint32_t Source() const { return allocation + 0x11000; }
  void Header(uint32_t address, uint16_t length, uint16_t maximum,
              uint32_t pointer) {
    auto* bytes = memory.TranslateVirtual(address);
    xe::store_and_swap<uint16_t>(bytes, length);
    xe::store_and_swap<uint16_t>(bytes + 2, maximum);
    xe::store_and_swap<uint32_t>(bytes + 4, pointer);
  }
  void Put(uint32_t address, std::initializer_list<uint8_t> bytes) {
    std::copy(bytes.begin(), bytes.end(), memory.TranslateVirtual(address));
  }
  std::vector<uint8_t> Snapshot() {
    const auto* begin = memory.TranslateVirtual(allocation);
    return {begin, begin + kBytes};
  }
  void ExpectedLength(std::vector<uint8_t>& expected, uint16_t length) {
    expected[0x100] = uint8_t(length >> 8);
    expected[0x101] = uint8_t(length);
  }
  void ExpectedBytes(std::vector<uint8_t>& expected, uint32_t address,
                     std::initializer_list<uint8_t> bytes) {
    std::copy(bytes.begin(), bytes.end(),
              expected.begin() + (address - allocation));
  }
  uint32_t Call(uint16_t ordinal, uint32_t source_header) {
    const auto* entry = emulator.export_resolver()->GetExportByOrdinal(
        "xboxkrnl.exe", ordinal);
    REQUIRE(entry);
    REQUIRE(entry->function_data.trampoline);
    context.r[3] = DestinationHeader();
    context.r[4] = source_header;
    entry->function_data.trampoline(&context);
    return uint32_t(context.r[3]);
  }
  struct NoAccessPage {
    Memory& memory;
    uint32_t address;
    bool active = false;
    NoAccessPage(Memory& owner, uint32_t page)
        : memory(owner), address(page) {
      auto* heap = memory.LookupHeap(address);
      REQUIRE(heap);
      REQUIRE(heap->Protect(address, 0x1000, kMemoryProtectNoAccess));
      active = true;
    }
    void Restore() {
      REQUIRE(memory.LookupHeap(address)->Protect(
          address, 0x1000, kMemoryProtectRead | kMemoryProtectWrite));
      active = false;
    }
    ~NoAccessPage() {
      if (active) {
        memory.LookupHeap(address)->Protect(
            address, 0x1000, kMemoryProtectRead | kMemoryProtectWrite);
      }
    }
  };
};
}  // namespace

TEST_CASE("Registered Unicode copy uses byte counts at real buffer guards",
          "[kernel][strings][abi][counted]") {
  ProfileAbiRuntime runtime;
  CountedStringAbiStorage f(*runtime.emulator);
  uint32_t destination = f.Destination();
  uint32_t source = f.Source();
  uint32_t guard = destination + 0x1000;
  uint16_t source_length = 8;
  uint16_t capacity = 4;
  SECTION("destination capacity bounds the copy") {
    destination += 0xFFC;
    f.Put(source, {0x00, 0x41, 0x20, 0xAC, 0x00, 0x00, 0x00, 0x62});
  }
  SECTION("short source bounds the read") {
    source += 0xFFC;
    source_length = 4;
    capacity = 12;
    guard = f.Source() + 0x1000;
    f.Put(source, {0x00, 0x41, 0x20, 0xAC});
  }
  f.Header(f.DestinationHeader(), 0, capacity, destination);
  f.Header(f.SourceHeader(), source_length, source_length, source);
  auto expected = f.Snapshot();
  f.ExpectedLength(expected, 4);
  f.ExpectedBytes(expected, destination, {0x00, 0x41, 0x20, 0xAC});
  CountedStringAbiStorage::NoAccessPage inaccessible(f.memory, guard);
  f.Call(0x122, f.SourceHeader());  // VOID, no invented status contract.
  inaccessible.Restore();
  // Every other header/payload/guard byte remains exact: MaximumLength,
  // Buffer, source header and bytes after the four-byte result included.
  CHECK(f.Snapshot() == expected);
}

TEST_CASE("Registered Unicode null source changes only destination length",
          "[kernel][strings][abi][counted]") {
  ProfileAbiRuntime runtime;
  CountedStringAbiStorage f(*runtime.emulator);
  f.Header(f.DestinationHeader(), 2, 8, f.Destination());
  f.Put(f.Destination(), {0x00, 0x41, 0x20, 0xAC});
  auto expected = f.Snapshot();
  f.ExpectedLength(expected, 0);
  f.Call(0x122, 0);
  CHECK(f.Snapshot() == expected);
}

TEST_CASE("Registered ANSI append counts embedded NUL bytes at exact capacity",
          "[kernel][strings][abi][counted]") {
  ProfileAbiRuntime runtime;
  CountedStringAbiStorage f(*runtime.emulator);
  f.Header(f.DestinationHeader(), 3, 6, f.Destination());
  f.Header(f.SourceHeader(), 3, 3, f.Source());
  f.Put(f.Destination(), {0x61, 0x00, 0x62});
  f.Put(f.Source(), {0x63, 0x00, 0x64});
  auto expected = f.Snapshot();
  f.ExpectedLength(expected, 6);
  f.ExpectedBytes(expected, f.Destination(),
                  {0x61, 0x00, 0x62, 0x63, 0x00, 0x64});
  CHECK(f.Call(0x115, f.SourceHeader()) == X_STATUS_SUCCESS);
  // No implicit terminator or source/header modification is admitted.
  CHECK(f.Snapshot() == expected);
}

TEST_CASE("Registered ANSI empty append leaves valid counted strings intact",
          "[kernel][strings][abi][counted]") {
  ProfileAbiRuntime runtime;
  CountedStringAbiStorage f(*runtime.emulator);
  SECTION("nonempty destination already fills its buffer") {
    f.Header(f.DestinationHeader(), 3, 3, f.Destination());
    f.Put(f.Destination(), {0x61, 0x00, 0x62});
  }
  SECTION("both empty buffers may be null") {
    f.Header(f.DestinationHeader(), 0, 0, 0);
  }
  f.Header(f.SourceHeader(), 0, 0, 0);
  const auto before = f.Snapshot();
  CHECK(f.Call(0x115, f.SourceHeader()) == X_STATUS_SUCCESS);
  CHECK(f.Snapshot() == before);
}

TEST_CASE("Registered ANSI capacity refusal cannot wrap or partially append",
          "[kernel][strings][abi][counted]") {
  ProfileAbiRuntime runtime;
  CountedStringAbiStorage f(*runtime.emulator);
  SECTION("one byte beyond capacity") {
    f.Header(f.DestinationHeader(), 3, 6, f.Destination());
    f.Header(f.SourceHeader(), 4, 4, f.Source());
    f.Put(f.Destination(), {0x61, 0x62, 0x63});
    f.Put(f.Source(), {0x64, 0x65, 0x66, 0x67});
  }
  SECTION("sum exceeds the complete USHORT capacity") {
    f.Header(f.DestinationHeader(), 0xFFF0, 0xFFFF, f.Destination());
    f.Header(f.SourceHeader(), 0x20, 0x20, f.Source());
  }
  const auto before = f.Snapshot();
  CHECK(f.Call(0x115, f.SourceHeader()) == X_STATUS_BUFFER_TOO_SMALL);
  CHECK(f.Snapshot() == before);
}

TEST_CASE("Registered ANSI append supports self and overlapping payloads",
          "[kernel][strings][abi][counted]") {
  ProfileAbiRuntime runtime;
  CountedStringAbiStorage f(*runtime.emulator);
  uint32_t source_header = f.SourceHeader();
  SECTION("same source and destination descriptor") {
    f.Header(f.DestinationHeader(), 3, 6, f.Destination());
    f.Put(f.Destination(), {0x61, 0x62, 0x63});
    source_header = f.DestinationHeader();
  }
  SECTION("destination suffix overlaps source bytes") {
    f.Header(f.DestinationHeader(), 1, 5, f.Destination());
    f.Header(f.SourceHeader(), 4, 4, f.Destination());
    f.Put(f.Destination(), {0x61, 0x62, 0x63, 0x64, 0x65});
  }
  auto expected = f.Snapshot();
  if (source_header == f.DestinationHeader()) {
    f.ExpectedLength(expected, 6);
    f.ExpectedBytes(expected, f.Destination(),
                    {0x61, 0x62, 0x63, 0x61, 0x62, 0x63});
  } else {
    f.ExpectedLength(expected, 5);
    f.ExpectedBytes(expected, f.Destination(),
                    {0x61, 0x61, 0x62, 0x63, 0x64});
  }
  CHECK(f.Call(0x115, source_header) == X_STATUS_SUCCESS);
  CHECK(f.Snapshot() == expected);
}


#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
TEST_CASE("Managed format refuses actual owning-runtime profile mount before retirement",
          "[kernel][profile][managed_format][admission]") {
  ProfileAbiRuntime runtime;
  auto* kernel = runtime.emulator->kernel_state();
  auto* manager = kernel->xam_state()->content_manager();
  auto* profiles = kernel->xam_state()->profile_manager();
  REQUIRE(manager);
  REQUIRE(profiles);
  REQUIRE(manager->managed_sources_ready());
  constexpr uint64_t xuid = 0xE000000000000081ull;
  const auto relative = ProfileStorageLease::ProfileRelative(xuid);
  const auto package = runtime.root.path / "content" / relative;
  REQUIRE(std::filesystem::create_directories(package));
  X_XAMACCOUNTINFO account{};
  string_util::copy_and_swap_truncating(account.gamertag, u"Mount Fixture",
                                        xe::countof(account.gamertag));
  std::array<uint8_t, sizeof(X_XAMACCOUNTINFO) + 0x18> encrypted{};
  ProfileManager::EncryptAccountFile(&account, encrypted.data());
  {
    std::ofstream output(package / "Account", std::ios::binary);
    REQUIRE(output);
    output.write(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
    REQUIRE(output.good());
  }
  // Genuine owning ProfileManager/VFS mount and storage lease. This is an
  // active storage-profile mount, not fabricated sign-in or full avatar/GPD
  // creation acceptance. Login is deliberately not called: its global
  // SaveConfig path is unrelated to this isolated storage-admission case.
  REQUIRE(profiles->MountProfile(xuid));
  std::error_code error;
  auto lease = profiles->AcquireProfileStorageLease(xuid, error);
  REQUIRE(lease);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(lease->CheckAvailable());
  std::vector<uint8_t> account_before;
  REQUIRE_FALSE(lease->view()->ReadFile("Account", encrypted.size(), account_before));
  REQUIRE(account_before == std::vector<uint8_t>(encrypted.begin(), encrypted.end()));
  const auto source = manager->sources().Source("hdd");
  REQUIRE(source);
  REQUIRE(source->available);
  const auto volume = manager->Volume("hdd");
  REQUIRE(volume);
  const auto public_view = volume->public_root();
  REQUIRE_FALSE(source->storage->retiring());
  REQUIRE_FALSE(public_view->retiring());
  std::ifstream marker_input(runtime.root.path / "content" /
      filesystem::ManagedDeviceIdentity::kMarker, std::ios::binary);
  REQUIRE(marker_input);
  const std::vector<uint8_t> marker_before{std::istreambuf_iterator<char>(marker_input), {}};
  marker_input.close();
  auto refused = manager->BeginIdleManagedFormat("hdd", error);
  REQUIRE_FALSE(refused);
  REQUIRE(error == std::errc::device_or_resource_busy);
  const auto current = manager->sources().Source("hdd");
  REQUIRE(current);
  REQUIRE(current->available);
  REQUIRE(current->generation == source->generation);
  REQUIRE(current->storage == source->storage);
  REQUIRE(current->managed_identity == source->managed_identity);
  REQUIRE(current->volume == volume);
  REQUIRE(manager->Volume("hdd") == volume);
  REQUIRE(manager->managed_sources_ready());
  REQUIRE_FALSE(source->storage->retiring());
  REQUIRE_FALSE(public_view->retiring());
  REQUIRE_FALSE(source->managed_identity->CheckCurrent());
  REQUIRE_FALSE(lease->retiring());
  REQUIRE_FALSE(lease->CheckAvailable());
  std::vector<uint8_t> after;
  REQUIRE_FALSE(lease->view()->ReadFile("Account", encrypted.size(), after));
  REQUIRE(after == account_before);
  std::ifstream final_marker(runtime.root.path / "content" /
      filesystem::ManagedDeviceIdentity::kMarker, std::ios::binary);
  REQUIRE(final_marker);
  const std::vector<uint8_t> marker_after{std::istreambuf_iterator<char>(final_marker), {}};
  REQUIRE(marker_after == marker_before);
  REQUIRE_FALSE(manager->CapturePendingManagedFormat("hdd", error));
  REQUIRE(profiles->DismountProfile(xuid));
  REQUIRE(lease->retiring());
  // Real dismount removes the active profile reservation. The idle guard now
  // succeeds without signout, model forgery, native Task or any disk reset.
  auto idle = profiles->TryReserveIdleForManagedFormat(error);
  REQUIRE(idle);
  REQUIRE_FALSE(error);
}
#endif


TEST_CASE("Registered device enumeration writes exact owned rows without a fake Dash authority",
          "[kernel][content][abi][managed_format]") {
  ProfileAbiRuntime runtime;
  auto* kernel = runtime.emulator->kernel_state();
  auto& memory = *runtime.emulator->memory();
  const auto devices = ListStorageDevices(kernel, true);
  REQUIRE_FALSE(devices.empty());
  REQUIRE(devices.front().managed_format_proof);
  REQUIRE(devices.front().managed_format_proof->current());
  const uint32_t page = memory.SystemHeapAlloc(0x1000, 0x1000);
  REQUIRE(page);
  struct Page { Memory& memory; uint32_t at; ~Page() { memory.SystemHeapFree(at); } } owned{memory, page};
  auto* bytes = memory.TranslateVirtual<uint8_t*>(page);
  std::memset(bytes, 0xA7, 0x1000);
  auto* resolver = runtime.emulator->export_resolver();
  const auto* create = resolver->GetExportByOrdinal("xam.xex", 0x25D);
  const auto* enumerate = resolver->GetExportByOrdinal("xam.xex", 0x250);
  REQUIRE(create); REQUIRE(create->function_data.trampoline);
  REQUIRE(enumerate); REQUIRE(enumerate->function_data.trampoline);
  PPCContext c{};
  c.virtual_membase = memory.virtual_membase();
  c.kernel_state = kernel; c.processor = kernel->processor();
  c.r[1] = page + 0xF00;
  c.r[3] = 0xFFFFFFFF; c.r[4] = 0x9F000000; c.r[5] = 1;
  c.r[6] = page + 4; c.r[7] = page;
  create->function_data.trampoline(&c);
  REQUIRE(uint32_t(c.r[3]) == X_ERROR_SUCCESS);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes + 4) == 0x50);
  const uint32_t handle = xe::load_and_swap<uint32_t>(bytes);
  auto actual = kernel->object_table()->LookupObject<XEnumerator>(handle);
  REQUIRE(actual); REQUIRE(actual->item_size() == 0x50);
  REQUIRE(actual->items_per_enumerate() == 1);
  c.r[3] = handle; c.r[4] = 2; c.r[5] = page + 0x100;
  c.r[6] = 0x50; c.r[7] = page + 8; c.r[8] = 0;
  enumerate->function_data.trampoline(&c);
  REQUIRE(uint32_t(c.r[3]) == X_ERROR_SUCCESS);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes + 8) == 1);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes + 0x100) == devices.front().device_id);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes + 0x104) == uint32_t(devices.front().device_type));
  REQUIRE(xe::load_and_swap<uint64_t>(bytes + 0x108) == devices.front().total_bytes);
  // Free bytes can truthfully change between observations; the owned result
  // still cannot claim more free space than this row's actual total capacity.
  REQUIRE(xe::load_and_swap<uint64_t>(bytes + 0x110) <= devices.front().total_bytes);
  REQUIRE(bytes[0xFF] == 0xA7); REQUIRE(bytes[0x150] == 0xA7);
  // No actual Dash XThread/module/producer scope exists in this ABI fixture.
  // Ordinary output bytes/handle therefore grant no frontend format authority.
  REQUIRE(devices.front().managed_format_proof->current());
  actual.reset();
  REQUIRE(kernel->object_table()->RemoveHandle(handle) == X_STATUS_SUCCESS);
}


TEST_CASE("Registered639 localizes an actual managed source without changing its capability",
          "[kernel][content][abi][localized_receipt]") {
  ProfileAbiRuntime runtime;
  auto* kernel = runtime.emulator->kernel_state();
  auto* manager = kernel->content_manager();
  REQUIRE(manager);
  const auto usb = runtime.root.path / "localized-usb";
  REQUIRE(std::filesystem::create_directories(usb / "Content"));
  ContentSource source;
  source.id = "localized-usb";
  source.device_id = 0x55;
  source.device_type = DeviceType::USBMASS;
  source.root = usb / "Content";
  source.host_volume_root = usb;
  source.private_state_parent = runtime.root.path;
  source.guest_volume_root = "\\Device\\Mass55PartitionFile\\Storage";
  source.guest_root = source.guest_volume_root + "\\Content";
  source.name = u"My Managed Archive";
  source.read_only = false;
  REQUIRE(manager->RegisterManagedSource(source));
  const auto before = manager->sources().Source(source.id);
  REQUIRE(before); REQUIRE(before->available); REQUIRE(before->managed_format_proof);
  const auto capability = before->managed_format_proof;
  const auto generation = before->generation;
  REQUIRE(capability->current());
  auto& memory = *runtime.emulator->memory();
  const uint32_t page = memory.SystemHeapAlloc(0x1000, 0x1000);
  REQUIRE(page);
  struct Page { Memory& memory; uint32_t at; ~Page() { memory.SystemHeapFree(at); } } owned{memory, page};
  auto* bytes = memory.TranslateVirtual<uint8_t*>(page);
  std::memset(bytes, 0xC7, 0x1000);
  const auto* plain = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x25E);
  const auto* localized = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x639);
  REQUIRE(plain); REQUIRE(plain->function_data.trampoline);
  REQUIRE(localized); REQUIRE(localized->function_data.trampoline);
  PPCContext c{};
  c.virtual_membase = memory.virtual_membase(); c.kernel_state = kernel;
  c.processor = kernel->processor(); c.r[1] = page + 0xF00;
  c.r[3] = source.device_id; c.r[4] = page + 0x100;
  plain->function_data.trampoline(&c);
  REQUIRE(uint32_t(c.r[3]) == X_ERROR_SUCCESS);
  const auto read_name = [&](uint32_t offset) {
    std::u16string result;
    for (size_t i = 0; i != 28; ++i) {
      const auto ch = xe::load_and_swap<uint16_t>(bytes + offset + 0x18 + i*2);
      if (!ch) break;
      result.push_back(char16_t(ch));
    }
    return result;
  };
  REQUIRE(read_name(0x100) == source.name);
  c.r[3] = source.device_id; c.r[4] = page + 0x200;
  localized->function_data.trampoline(&c);
  REQUIRE(uint32_t(c.r[3]) == X_ERROR_SUCCESS);
  REQUIRE(read_name(0x200) == u"USB Storage");
  REQUIRE(read_name(0x200) != source.name);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes+0x200) == source.device_id);
  REQUIRE(xe::load_and_swap<uint32_t>(bytes+0x204) == uint32_t(DeviceType::USBMASS));
  REQUIRE(xe::load_and_swap<uint64_t>(bytes+0x208) == before->capacity_bytes);
  REQUIRE(xe::load_and_swap<uint64_t>(bytes+0x210) <= before->capacity_bytes);
  REQUIRE(bytes[0x1FF] == 0xC7); REQUIRE(bytes[0x250] == 0xC7);
  constexpr size_t localized_chars = std::u16string_view(u"USB Storage").size();
  REQUIRE(std::all_of(bytes+0x218+localized_chars*2, bytes+0x250,
                      [](uint8_t byte) { return byte == 0; }));
  const std::array<uint8_t, 0x50> written = [&] {
    std::array<uint8_t, 0x50> value;
    std::copy_n(bytes+0x200, value.size(), value.begin()); return value;
  }();
  c.r[3] = source.device_id; c.r[4] = 0;
  localized->function_data.trampoline(&c);
  REQUIRE(uint32_t(c.r[3]) == X_ERROR_INVALID_PARAMETER);
  REQUIRE(std::equal(written.begin(), written.end(), bytes+0x200));
  const auto after = manager->sources().Source(source.id);
  REQUIRE(after); REQUIRE(after->generation == generation);
  REQUIRE(after->managed_format_proof == capability);
  REQUIRE(capability->current());
  // No real Dash module/XThread/refresh scope exists in this registered ABI
  // fixture. The serializer output does not independently mint menu authority.
}

#if XE_PLATFORM_LINUX
namespace {
std::vector<uint8_t> SeedOwnedAccount(ProfileAbiRuntime& runtime, uint64_t xuid,
                                     X_XAMACCOUNTINFO& account) {
  const auto package = runtime.root.path / "content" /
      ProfileStorageLease::ProfileRelative(xuid);
  REQUIRE(std::filesystem::create_directories(package));
  string_util::copy_and_swap_truncating(account.gamertag, u"Account Fixture",
                                        xe::countof(account.gamertag));
  account.passport_password[7] = char(0x8e);  // Opaque existing data.
  std::vector<uint8_t> wire(ProfileAccountSnapshot::kPrefixBytes);
  ProfileManager::EncryptAccountFile(&account, wire.data());
  wire.insert(wire.end(), {0x91, 0, 0x82, 0xff});
  std::ofstream output(package / "Account", std::ios::binary);
  REQUIRE(output);
  output.write(reinterpret_cast<const char*>(wire.data()), wire.size());
  REQUIRE(output.good());
  return wire;
}
std::vector<uint8_t> OwnedAccountDisk(ProfileAbiRuntime& runtime, uint64_t xuid) {
  std::ifstream file(runtime.root.path / "content" /
      ProfileStorageLease::ProfileRelative(xuid) / "Account", std::ios::binary);
  REQUIRE(file);
  return {std::istreambuf_iterator<char>(file), {}};
}
}
TEST_CASE("Owning managed VFS never exposes Account through a nested descendant request",
          "[kernel][profile_account][vfs][exact_leaf]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t xuid=0xe0000000000000a1ull;
  X_XAMACCOUNTINFO account{};
  const auto before=SeedOwnedAccount(runtime,xuid,account);
  auto* profiles=runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(profiles->LoadAccount(xuid));
  REQUIRE(profiles->MountProfile(xuid));
  auto* fs=runtime.emulator->file_system();
  for (const auto nested : {"User_E0000000000000A1:\\Account\\evil",
                            "User_E0000000000000A1:\\Account\\Account"}) {
    for (auto disposition : {vfs::FileDisposition::kOpen,
                              vfs::FileDisposition::kSuperscede,
                              vfs::FileDisposition::kCreate}) {
      vfs::File* file=nullptr;
      vfs::FileAction action{};
      CHECK(XFAILED(fs->OpenFile(nullptr,nested,disposition,
          vfs::FileAccess::kFileReadData|vfs::FileAccess::kFileWriteData,
          false,true,&file,&action)));
      CHECK_FALSE(file);
      CHECK(OwnedAccountDisk(runtime,xuid)==before);
    }
  }
  vfs::File* valid=nullptr;
  vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr,"User_E0000000000000A1:\\Account",
      vfs::FileDisposition::kOpen,vfs::FileAccess::kFileReadData,
      false,true,&valid,&action)==X_STATUS_SUCCESS);
  std::unique_ptr<vfs::File> owned(valid);
  std::vector<uint8_t> read(before.size());
  size_t count=0;
  REQUIRE(owned->ReadSync(read,0,&count)==X_STATUS_SUCCESS);
  CHECK(count==before.size());
  CHECK(read==before);
}
TEST_CASE("Owning ProfileManager guest Account partial and complete writes publish only durable validated cache",
          "[kernel][profile_account][guest_write][persistence]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t xuid = 0xe000000000000091ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, xuid, account);
  auto* profiles = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(profiles->LoadAccount(xuid));
  REQUIRE(profiles->MountProfile(xuid));
  auto* fs = runtime.emulator->file_system();
  auto* old_entry = fs->ResolvePath("User_E000000000000091:\\Account");
  REQUIRE(old_entry);
  auto next = account;
  string_util::copy_and_swap_truncating(next.gamertag, u"Durable Update",
                                        xe::countof(next.gamertag));
  auto wire = before;
  ProfileManager::EncryptAccountFile(&next, wire.data());
  wire.insert(wire.end(), {3, 0, 4});
  vfs::File* raw_file = nullptr;
  vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr, "User_E000000000000091:\\Account",
      vfs::FileDisposition::kOverwrite, vfs::FileAccess::kGenericRead |
      vfs::FileAccess::kGenericWrite, false, true, &raw_file, &action) == X_STATUS_SUCCESS);
  auto destroy = [](vfs::File* file) { if (file) { file->Destroy(); } };
  std::unique_ptr<vfs::File, decltype(destroy)> file(raw_file, destroy);
  REQUIRE(file);
  CHECK(action == vfs::FileAction::kOverwritten);
  CHECK(file->entry()->size() == 0);
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
  size_t written = 0;
  REQUIRE(file->WriteSync(std::span<const uint8_t>(wire).first(41), 0, &written) == X_STATUS_SUCCESS);
  CHECK(written == 41);
  CHECK(XFAILED(file->Flush()));
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
  REQUIRE(profiles->GetAccount(xuid));
  CHECK(std::memcmp(profiles->GetAccount(xuid), &account, sizeof(account)) == 0);
  REQUIRE(file->WriteSync(std::span<const uint8_t>(wire).subspan(41), 41, &written) == X_STATUS_SUCCESS);
  CHECK(written == wire.size() - 41);
  CHECK(OwnedAccountDisk(runtime, xuid) == wire);
  CHECK(std::memcmp(profiles->GetAccount(xuid), &next, sizeof(next)) == 0);
  CHECK(fs->ResolvePath("User_E000000000000091:\\Account") == old_entry);
  CHECK(old_entry->size() == wire.size());
  CHECK_FALSE(file->entry()->allows_namespace_mutation());
  file.reset();
  REQUIRE(profiles->DismountProfile(xuid));
  runtime.emulator.reset();
  runtime.emulator = std::make_unique<Emulator>("", runtime.root.path,
      runtime.root.path / "content", runtime.root.path / "cache");
  REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  profiles = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(profiles->LoadAccount(xuid));
  REQUIRE(profiles->GetAccount(xuid));
  CHECK(std::memcmp(profiles->GetAccount(xuid), &next, sizeof(next)) == 0);
  CHECK(OwnedAccountDisk(runtime, xuid) == wire);
}
TEST_CASE("Owning Account registration refuses preexisting raw writer and preserves namespace tombstone",
          "[kernel][profile_account][guest_write][open_conflict]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t xuid = 0xe000000000000095ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, xuid, account);
  auto* profiles = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto* content = runtime.emulator->kernel_state()->xam_state()->content_manager();
  auto* fs = runtime.emulator->file_system();
  std::error_code error;
  auto source = content->sources().Source("hdd");
  REQUIRE(source);
  auto raw = source->storage->Subroot(ProfileStorageLease::ProfileRelative(xuid), false, error);
  REQUIRE(raw);
  auto alias = std::make_unique<vfs::HostPathDevice>(
      "RawAccount:", raw->path(), false, false, "FATX", raw, source->storage);
  REQUIRE(alias->Initialize());
  REQUIRE(fs->RegisterDevice(std::move(alias)));
  vfs::File* file = nullptr;
  vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr, "RawAccount:\\Account", vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericWrite, false, true, &file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file);
  CHECK_FALSE(profiles->MountProfile(xuid));
  CHECK_FALSE(profiles->GetAccount(xuid));
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
  file->Destroy();
  vfs::File* denied = nullptr;
  CHECK(XFAILED(fs->OpenFile(nullptr, "RawAccount:\\Account", vfs::FileDisposition::kSuperscede,
      vfs::FileAccess::kGenericWrite, false, true, &denied, &action)));
  CHECK_FALSE(denied);
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
  REQUIRE(profiles->MountProfile(xuid));
  REQUIRE(profiles->LoadAccount(xuid));
  REQUIRE(profiles->GetAccount(xuid));
  CHECK(std::memcmp(profiles->GetAccount(xuid), &account, sizeof(account)) == 0);
  REQUIRE(profiles->DismountProfile(xuid));
  REQUIRE(fs->UnregisterDevice("RawAccount:"));
}
TEST_CASE("Owning ProfileManager Account byte handle cannot borrow manager after real teardown",
          "[kernel][profile_account][guest_write][lifetime]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t xuid = 0xe000000000000092ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, xuid, account);
  auto* profiles = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(profiles->LoadAccount(xuid));
  REQUIRE(profiles->MountProfile(xuid));
  vfs::File* file = nullptr;
  vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr,
      "User_E000000000000092:\\Account", vfs::FileDisposition::kOverwrite,
      vfs::FileAccess::kGenericRead | vfs::FileAccess::kGenericWrite,
      false, true, &file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file);
  size_t count;
  REQUIRE(file->WriteSync(std::span<const uint8_t>(before).first(13), 0, &count) == X_STATUS_SUCCESS);
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
  auto malformed = before;
  malformed[0] ^= 0x20;
  CHECK(XFAILED(file->WriteSync(malformed, 0, &count)));
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
  REQUIRE(profiles->GetAccount(xuid));
  CHECK(std::memcmp(profiles->GetAccount(xuid), &account, sizeof(account)) == 0);
  runtime.emulator.reset();  // Actual manager, kernel and namespace retirement.
  CHECK(file->Flush() == X_STATUS_CANCELLED);
  CHECK(XFAILED(file->WriteSync(before, 0, &count)));
  std::error_code error;
  auto foreign = filesystem::HostStorageRoot::Open(runtime.root.path / "content" /
      ProfileStorageLease::ProfileRelative(xuid), false, error);
  REQUIRE(foreign);
  CHECK_FALSE(foreign->OpenFile("Account", filesystem::FileAccess::kGenericRead, error));
  CHECK(error == std::errc::device_or_resource_busy);
  file->Destroy();  // LAST real managed owner permits namespace rundown.
  auto readable = foreign->OpenFile("Account", filesystem::FileAccess::kGenericRead, error);
  REQUIRE(readable);
  REQUIRE_FALSE(error);
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
}
TEST_CASE("Owning ProfileManager sign-in refuses a retained old Account producer before generation publication",
          "[kernel][profile_account][identity][cancel]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t xuid = 0xe000000000000094ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, xuid, account);
  auto* profiles = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto* content = runtime.emulator->kernel_state()->xam_state()->content_manager();
  REQUIRE(profiles->LoadAccount(xuid));
  REQUIRE(profiles->MountProfile(xuid));
  std::error_code error;
  auto snapshot = profiles->CaptureDurableAccount(xuid, error);
  REQUIRE(snapshot);
  ProfileAccountWriter writer(snapshot->identity(), content->Volume("hdd")->recovery());
  auto prepared = writer.PrepareRaw(snapshot->token(), before, error);
  REQUIRE(prepared);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(snapshot->identity()->writes_drained());
  profiles->Login(xuid, 2, false);
  CHECK_FALSE(profiles->CaptureProfile(uint8_t(2)));
  CHECK(snapshot->identity()->retired());
  CHECK_FALSE(snapshot->identity()->writes_drained());
  CHECK_FALSE(writer.Commit(*prepared).durable);
  CHECK(OwnedAccountDisk(runtime, xuid) == before);
  prepared.reset();
  REQUIRE(snapshot->identity()->writes_drained());
  auto current = profiles->CaptureDurableAccount(xuid, error);
  REQUIRE(current);
  CHECK(current->identity() != snapshot->identity());
  CHECK_FALSE(current->identity()->retired());
  CHECK(snapshot->identity()->retired());
  REQUIRE(profiles->DismountProfile(xuid));
}
TEST_CASE("Owning ProfileManager typed Account update preserves raw suffix through shared producer",
          "[kernel][profile_account][persistence][cache]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t xuid = 0xe000000000000093ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, xuid, account);
  auto* profiles = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(profiles->LoadAccount(xuid));
  auto next = account;
  string_util::copy_and_swap_truncating(next.gamertag, u"Typed Update",
                                        xe::countof(next.gamertag));
  std::error_code error;
  auto old_inputs = profiles->PrepareNativeLogonAccounts(error);
  REQUIRE(old_inputs);
  REQUIRE(old_inputs->OwnershipCurrent());
  REQUIRE(profiles->UpdateAccount(xuid, &next));
  CHECK_FALSE(old_inputs->OwnershipCurrent());
  auto metadata = profiles->CaptureCachedAccount(xuid, error);
  REQUIRE(metadata);
  const auto page = runtime.emulator->memory()->SystemHeapAlloc(0x1000, 0x1000);
  REQUIRE(page);
  auto* bytes = runtime.emulator->memory()->TranslateVirtual(page);
  std::memset(bytes, 0x5a, 0x1000);
  const auto* find = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x235);
  REQUIRE(find);
  REQUIRE(find->function_data.trampoline);
  PPCContext context{};
  context.virtual_membase = runtime.emulator->memory()->virtual_membase();
  context.r[1] = page + 0xf00;
  context.r[3] = xuid;
  context.r[4] = page;
  context.r[5] = page + 0x200;
  find->function_data.trampoline(&context);
  CHECK(uint32_t(context.r[3]) == X_ERROR_SUCCESS);
  CHECK(std::memcmp(bytes, &next, sizeof(next)) == 0);
  CHECK(xe::load_and_swap<uint32_t>(bytes + 0x200) == metadata->device_id());
  CHECK(bytes[0x204] == 0x5a);
  runtime.emulator->memory()->SystemHeapFree(page);
  REQUIRE(profiles->GetAccount(xuid));
  CHECK(std::memcmp(profiles->GetAccount(xuid), &next, sizeof(next)) == 0);
  const auto actual = OwnedAccountDisk(runtime, xuid);
  REQUIRE(actual.size() == before.size());
  CHECK(std::equal(before.begin() + ProfileAccountSnapshot::kPrefixBytes,
                    before.end(), actual.begin() + ProfileAccountSnapshot::kPrefixBytes));
}
#endif


#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
TEST_CASE("An explicit ancestor volume retains managed Content IO and private exclusions across restart",
          "[kernel][content][managed_volume][public_ancestor]") {
  ProfileAbiRuntime runtime;
  const auto usb = runtime.root.path / "ancestor-volume";
  REQUIRE(std::filesystem::create_directories(usb / "Content"));
  ContentSource source;
  source.id = "ancestor-volume";
  source.device_id = 0x56;
  source.device_type = DeviceType::USBMASS;
  source.root = usb / "Content";
  source.host_volume_root = usb;
  source.private_state_parent = runtime.root.path;
  source.guest_volume_root = "\\Device\\Mass56PartitionFile\\Storage";
  source.guest_root = source.guest_volume_root + "\\Content";
  source.name = u"Explicit ancestor volume";
  source.read_only = false;
  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = source.device_id;
  data.title_id = 0x12345678;
  data.content_type = XContentType::kSavedGame;
  data.set_file_name("ancestor-save");
  data.set_display_name(u"Persistent explicit-volume save");
  constexpr std::array<uint8_t, 9> payload{0x61, 0, 0x77, 0x6F, 0x6C, 0x75, 0x6D, 0x65, 0xFF};
  filesystem::ManagedDeviceIdentity::Uuid uuid{};
  std::filesystem::path saved_relative;
  const auto guest_io = [&](bool create) {
    auto* fs = runtime.emulator->file_system();
    vfs::File* raw = nullptr;
    vfs::FileAction action{};
    REQUIRE(fs->OpenFile(nullptr, "ancesave:\\marker.bin",
        create ? vfs::FileDisposition::kCreate : vfs::FileDisposition::kOpen,
        create ? vfs::FileAccess::kGenericAll : vfs::FileAccess::kGenericRead,
        false, true, &raw, &action) == X_STATUS_SUCCESS);
    REQUIRE(raw);
    const auto destroy = [](vfs::File* file) { file->Destroy(); };
    std::unique_ptr<vfs::File, decltype(destroy)> file(raw, destroy);
    size_t count = 0;
    if (create) {
      REQUIRE(file->WriteSync(payload, 0, &count) == X_STATUS_SUCCESS);
      REQUIRE(count == payload.size());
      REQUIRE(file->Flush() == X_STATUS_SUCCESS);
    }
    std::array<uint8_t, payload.size()> read{};
    REQUIRE(file->ReadSync(read, 0, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == read.size());
    REQUIRE(read == payload);
  };
  {
    auto* manager = runtime.emulator->kernel_state()->content_manager();
    REQUIRE(manager->RegisterManagedSource(source));
    const auto selected = manager->sources().Source(source.id);
    const auto volume = manager->Volume(source.id);
    REQUIRE(selected); REQUIRE(selected->available);
    REQUIRE(selected->storage); REQUIRE(volume); REQUIRE_FALSE(volume->error());
    REQUIRE(volume->public_root()->path() == usb);
    REQUIRE(volume->identity()->configured_location() == source.root);
    REQUIRE(volume->identity()->CheckCurrent() == std::error_code{});
    uuid = volume->identity()->uuid();
    REQUIRE(manager->CreateContent("ancesave", 1, data) == X_ERROR_SUCCESS);
    guest_io(true);
    REQUIRE(manager->CloseContent("ancesave") == X_ERROR_SUCCESS);
    ContentCatalogLocation location;
    REQUIRE(manager->CaptureContentLocation(1, data, location) == X_ERROR_SUCCESS);
    REQUIRE(manager->IsContentLocationCurrent(location));
    saved_relative = location.package_path.lexically_relative(usb) / "marker.bin";
    REQUIRE_FALSE(saved_relative.empty());
    REQUIRE_FALSE(saved_relative.is_absolute());
    REQUIRE(*saved_relative.begin() != "..");
    std::filesystem::path binding;
    for (const auto& entry : std::filesystem::directory_iterator(source.root)) {
      if (entry.path().filename().string().starts_with(".xenia-device-binding-v1-")) {
        REQUIRE(binding.empty()); binding = entry.path().filename();
      }
    }
    REQUIRE_FALSE(binding.empty());
    std::error_code error;
    REQUIRE_FALSE(selected->storage->OpenFile(binding / "owner.v1",
        filesystem::FileAccess::kGenericRead, error));
    REQUIRE(error == std::errc::permission_denied);
    REQUIRE_FALSE(volume->public_root()->OpenFile(
        std::filesystem::path("Content") / binding / "owner.v1",
        filesystem::FileAccess::kGenericRead, error));
    REQUIRE(error == std::errc::permission_denied);
    auto raw_private = filesystem::HostStorageRoot::Open(source.root / binding,
                                                          true, error);
    REQUIRE(raw_private); REQUIRE_FALSE(error);
    REQUIRE_FALSE(raw_private->GuestView(error));
    REQUIRE(error == std::errc::permission_denied);
    // The declared volume itself still exposes ordinary Content browsing.
    REQUIRE(volume->public_root()->OpenFile("Content",
        filesystem::FileAccess::kGenericRead, error));
    REQUIRE_FALSE(error);
  }
  // All old live authorities go away before actual Kernel/Memory replacement.
  // Keep only package identity, stable UUID and an observed relative filename.
  runtime.emulator.reset();
  REQUIRE(KernelState::shared() == nullptr);
  runtime.emulator = std::make_unique<Emulator>(
      "", runtime.root.path, runtime.root.path / "content", runtime.root.path / "cache");
  REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  auto* manager = runtime.emulator->kernel_state()->content_manager();
  REQUIRE(manager->RegisterManagedSource(source));
  const auto selected = manager->sources().Source(source.id);
  const auto volume = manager->Volume(source.id);
  REQUIRE(selected); REQUIRE(selected->available); REQUIRE(volume);
  REQUIRE_FALSE(volume->error());
  REQUIRE(volume->identity()->uuid() == uuid);
  REQUIRE(volume->identity()->CheckCurrent() == std::error_code{});
  uint32_t license = 0;
  REQUIRE(manager->OpenContent("ancesave", 1, data, license) == X_ERROR_SUCCESS);
  guest_io(false);
  REQUIRE(manager->CloseContent("ancesave") == X_ERROR_SUCCESS);
  ContentCatalogLocation location;
  REQUIRE(manager->CaptureContentLocation(1, data, location) == X_ERROR_SUCCESS);
  const auto removed = runtime.root.path / "removed-ancestor-volume";
  std::filesystem::rename(usb, removed);
  REQUIRE_FALSE(std::filesystem::exists(usb));
  REQUIRE_FALSE(selected->storage->Observe().available);
  REQUIRE_FALSE(manager->IsContentLocationCurrent(location));
  REQUIRE(manager->CheckPackageName(1, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  auto unavailable = data;
  unavailable.set_file_name("unavailable-save");
  REQUIRE(manager->CreateContent("unavail", 1, unavailable) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE_FALSE(std::filesystem::exists(usb));
  std::ifstream persisted(removed / saved_relative, std::ios::binary);
  REQUIRE(persisted);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(persisted), {}};
  REQUIRE(bytes == std::vector<uint8_t>(payload.begin(), payload.end()));
}

TEST_CASE("An explicit managed volume cannot appropriate a live foreign PUBLIC guest",
          "[kernel][content][managed_volume][public_ancestor][foreign_guest]") {
  ProfileAbiRuntime runtime;
  const auto usb = runtime.root.path / "foreign-public-volume";
  REQUIRE(std::filesystem::create_directories(usb / "Content"));
  constexpr std::array<uint8_t, 4> payload{0xF1, 0, 0x72, 0xFF};
  {
    std::ofstream file(usb / "foreign.bin", std::ios::binary);
    file.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    REQUIRE(file.good());
  }
  std::error_code error;
  auto raw = filesystem::HostStorageRoot::Open(usb, false, error);
  REQUIRE(raw); REQUIRE_FALSE(error);
  auto foreign = raw->GuestView(error);
  REQUIRE(foreign); REQUIRE_FALSE(error);
  ContentSource source;
  source.id = "foreign-public-volume"; source.device_id = 0x57;
  source.device_type = DeviceType::USBMASS;
  source.root = usb / "Content"; source.host_volume_root = usb;
  source.private_state_parent = runtime.root.path;
  source.guest_volume_root = "\\Device\\Mass57PartitionFile\\Storage";
  source.guest_root = source.guest_volume_root + "\\Content";
  source.name = u"Foreign public guest"; source.read_only = false;
  auto* manager = runtime.emulator->kernel_state()->content_manager();
  REQUIRE_FALSE(manager->RegisterManagedSource(source));
  REQUIRE_FALSE(manager->sources().Source(source.id));
  auto still_foreign = foreign->OpenFile("foreign.bin",
      filesystem::FileAccess::kGenericRead, error);
  REQUIRE(still_foreign); REQUIRE_FALSE(error);
  std::array<uint8_t, payload.size()> read{};
  size_t count = 0;
  REQUIRE(still_foreign->Read(0, read.data(), read.size(), &count));
  REQUIRE(count == read.size());
  REQUIRE(read == payload);
  std::ifstream persisted(usb / "foreign.bin", std::ios::binary);
  REQUIRE(persisted);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(persisted), {}};
  REQUIRE(bytes == std::vector<uint8_t>(payload.begin(), payload.end()));
}

TEST_CASE("An equal PUBLIC namespace remains an exclusion rather than an ancestor permit",
          "[kernel][content][managed_volume][public_ancestor][equal_scope]") {
  ProfileAbiRuntime runtime;
  const auto folder = runtime.root.path / "equal-public-source";
  REQUIRE(std::filesystem::create_directory(folder));
  const std::string payload = "preserved equal-scope bytes";
  {
    std::ofstream file(folder / "existing.bin", std::ios::binary);
    file.write(payload.data(), payload.size()); REQUIRE(file.good());
  }
  ContentSource source;
  source.id = "equal-public-source"; source.device_id = 0x58;
  source.device_type = DeviceType::USBMASS;
  source.root = folder; source.host_volume_root = folder;
  source.private_state_parent = runtime.root.path;
  source.guest_volume_root = "\\Device\\Mass58PartitionFile\\Storage";
  source.guest_root = source.guest_volume_root + "\\Content";
  source.name = u"Equal scope"; source.read_only = false;
  auto* manager = runtime.emulator->kernel_state()->content_manager();
  REQUIRE_FALSE(manager->RegisterManagedSource(source));
  REQUIRE_FALSE(manager->sources().Source(source.id));
  std::ifstream file(folder / "existing.bin", std::ios::binary);
  REQUIRE(file);
  REQUIRE(std::string(std::istreambuf_iterator<char>(file), {}) == payload);
}
#endif

#if XE_PLATFORM_LINUX
class ProfileSettingsReadTestAccess final {
 public:
  static auto PrepareDeletion(ProfileManager& pm, uint64_t xuid) {
    return pm.PrepareHostProfileDeletion(xuid);
  }
  static auto DeletionPackage(const std::shared_ptr<ProfileManager::ProfileDeletionReservation>& deletion) {
    return deletion ? deletion->package : nullptr;
  }
  static bool AdvanceDeletion(ProfileManager& pm,
      const std::shared_ptr<ProfileManager::ProfileDeletionReservation>& deletion) {
    return pm.AdvanceProfileDeletion(deletion);
  }
  static std::unique_lock<std::recursive_mutex> HoldMetadata(ProfileManager& pm) {
    return std::unique_lock<std::recursive_mutex>(pm.profile_mount_mutex_);
  }
  static std::shared_ptr<ProfileRosterIntent> PrepareAttempt(ProfileManager& pm,
      const ProfileRosterDesired& desired,const ProfileRosterExpected& expected,
      std::error_code& error,std::shared_ptr<ProfileRosterIntent>& retained) {
    return pm.PrepareRosterAttempt(desired,expected,error,&retained);
  }
  static auto PrepareAttemptPhases(ProfileManager& pm,
      const ProfileRosterDesired& desired,const ProfileRosterExpected& expected,
      std::error_code& error,std::shared_ptr<ProfileRosterIntent>& retained,
      const ProfilePreparationProgress* progress) {
    return pm.PrepareRosterAttempt(desired,expected,error,&retained,nullptr,progress);
  }
  static auto PrepareWithProgress(ProfileManager& pm,
      const ProfileRosterDesired& desired,const ProfileRosterExpected& expected,
      std::error_code& error,std::chrono::steady_clock::time_point deadline,
      const std::function<bool()>& wait) {
    return pm.PrepareRosterWithProgress(desired,expected,error,deadline,wait);
  }
  static auto CancelPreparation(ProfileManager& pm,
      const std::shared_ptr<ProfileRosterIntent>& intent) {
    return pm.CancelRosterPreparation(intent);
  }
  static auto ClaimRoster(ProfileManager& pm) { return pm.ClaimNativeRosterFactory(); }
  static auto CurrentTicket(ProfileManager& pm,
      const std::shared_ptr<const ProfileRosterNativeBinding>& binding,std::error_code& error) {
    return pm.CaptureCurrentNativeReadTicket(binding,error);
  }
  static auto InitialEmptySequence(ProfileManager& pm,
      const std::shared_ptr<const ProfileRosterNativeBinding>& binding,
      const ProfileRosterExpected& expected,
      const std::shared_ptr<ProfileRosterNativeReadTicket>& ticket) {
    return pm.CaptureInitialEmptyNativeRosterSequence(binding,expected,ticket);
  }
  static bool PublishedEmptyRows(const std::shared_ptr<ProfileRosterIntent>& intent,
                                 uint32_t native_value = 0) {
    return intent && intent->HasPublishedEmptyUnchangedRows(native_value);
  }
  static bool PublishedUnchangedOwners(const std::shared_ptr<ProfileRosterIntent>& intent,
                                        uint32_t native_value = 0) {
    return intent && intent->HasPublishedUnchangedOwners(native_value);
  }
  static auto Ticket(ProfileRosterIntent& intent,const ProfileRosterNativeBinding& binding,
      std::error_code& error) { return intent.NativeReadTicket(false,binding,&error); }
  static void RetryCleanup(ProfileManager& pm) { pm.RetryRosterSettlement(); }
  static auto SelectedAccount(ProfileManager& pm, uint64_t xuid) {
    return pm.CaptureRosterAccount(xuid);
  }
  static auto CaptureIdentityForRebindProbe(ProfileManager& pm, uint64_t xuid,
                                            std::error_code& error) {
    return pm.CaptureAccountIdentity(xuid, error);
  }
  static auto EnterNative(ProfileManager& pm, const std::shared_ptr<ProfileRosterIntent>& intent,
      const std::shared_ptr<const ProfileRosterNativeBinding>& binding, std::error_code& error) {
    return pm.EnterNativeRoster(intent, binding, error);
  }
  static bool Publish(const std::shared_ptr<ProfileRosterPublicationCell>& cell,
      ProfileRosterPublicationOutcome outcome=ProfileRosterPublicationOutcome::kRequested) {
    return cell && cell->PublishFromNative(outcome);
  }
  static auto FollowingTicket(ProfileRosterIntent& intent,const ProfileRosterNativeBinding& binding,
      std::error_code& error) {return intent.NativeReadTicket(true,binding,&error);}
  static auto PreparedWire(const ProfileRosterIntent& intent) {return intent.NativeRowsForPrepared();}
  static size_t MountReferences(ProfileManager& pm,uint64_t xuid) {
    std::lock_guard lock(pm.profile_mount_mutex_);size_t references=0;
    for(const auto& [name,mount]:pm.profile_mounts_)if(mount.xuid==xuid)references+=mount.references;
    return references;
  }
  static auto Protocol(const std::shared_ptr<ProfileRosterIntent>& intent,
      const std::shared_ptr<const ProfileRosterNativeBinding>& binding,
      uint32_t value = 0, bool wrong_nonce = false) {
    return ProfileRosterNativeCompletion::ForProtocol(intent, binding,
        binding->nonce() + uint64_t(wrong_nonce), value);
  }
  static auto Joined(const std::shared_ptr<ProfileRosterIntent>& intent,
      const std::shared_ptr<const ProfileRosterNativeBinding>& binding) {
    return ProfileRosterNativeCompletion::ForJoined(intent, binding, binding->nonce());
  }
  static auto CommitProtocol(ProfileManager& pm, const std::shared_ptr<ProfileRosterIntent>& intent,
      const ProfileRosterNativeCompletion& completion) {
    return pm.PublishNativeRosterProtocol(intent, completion);
  }
  static auto Finalize(ProfileManager& pm, const std::shared_ptr<ProfileRosterIntent>& intent,
      const ProfileRosterNativeCompletion& completion) {
    return pm.FinalizeNativeRoster(intent, completion);
  }
  static auto Scope(const std::shared_ptr<ProfileRosterNativeReadTicket>& ticket,
      const std::shared_ptr<const ProfileRosterNativeBinding>& binding) {
    return ProfileRosterNativeReadScope::Prepare(ticket, binding);
  }
  static bool EnterSystemFrame(ProfileRosterNativeReadScope& scope, KernelState* kernel,
      XThread* thread, cpu::ThreadState* cpu, uint32_t sp, uint32_t lr,
      uint32_t function, uint64_t work = 1) {
    return scope.EnterSystemThreadFrame(kernel, thread, cpu, sp, lr, function,
                                       work, 0x81000000, 1);
  }
  static bool EnterWithoutTask(ProfileRosterNativeReadScope& scope, KernelState* kernel,
      XThread* thread, cpu::ThreadState* cpu) {
    return scope.Enter(kernel, thread, cpu, 0, 0, 1, 0x81000000, 1);
  }
  static bool Leave(ProfileRosterNativeReadScope& scope) { return scope.Leave(); }
  static auto Current(KernelState* kernel, cpu::ThreadState* cpu) {
    return ProfileRosterNativeReadScope::FromActualThread(kernel, cpu);
  }
  static void BeginStop(ProfileManager& pm) { pm.BeginRosterStop(); }
  static void Close(ProfileManager& pm) { pm.CloseRosterAuthority(); }
};
namespace {
constexpr uint32_t kReadAppearance = 0x63E80045;  // INFO2, opaque 1000 bytes.
constexpr uint32_t kReadScalar =
    uint32_t(UserSettingId::XPROFILE_GAMER_YAXIS_INVERSION);
constexpr uint32_t kReadAbsent =
    uint32_t(UserSettingId::XPROFILE_OPTION_CONTROLLER_VIBRATION);
constexpr uint32_t kReadTitle = 0x1234abcd;
std::vector<uint8_t> ReadFixtureDisk(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input), {}};
}
void ReadFixtureWrite(const std::filesystem::path& path,
                      std::span<const uint8_t> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  REQUIRE(output.good());
}
std::vector<uint8_t> ReadFixtureSerialize(const GpdInfo& model) {
  std::vector<uint8_t> wire;
  REQUIRE_FALSE(model.SerializeChecked(wire, GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(ValidateGpdSnapshot(wire));
  return wire;
}
// A real unknown used XDBF record, retained byte-for-byte by canonical reading.
void ReadFixtureUnknown(std::vector<uint8_t>& wire) {
  const auto view = XdbfTableView::Parse(wire);
  REQUIRE(view);
  const auto capacity = XdbfTableView::U32(wire.data() + 8);
  REQUIRE(view->entries_used < capacity);
  const auto tail = 24 + size_t(capacity) * 18 +
                    size_t(view->free_entries_used - 1) * 8;
  const auto eof = XdbfTableView::U32(wire.data() + tail);
  const auto remaining = XdbfTableView::U32(wire.data() + tail + 4);
  REQUIRE(uint64_t(eof) + remaining == UINT32_MAX);
  REQUIRE(remaining >= 4);
  auto* entry = wire.data() + 24 + size_t(view->entries_used) * 18;
  std::fill_n(entry, 18, 0);
  entry[0] = 0xfe; entry[1] = 0xdc; entry[9] = 0x2a;
  xe::store_and_swap<uint32_t>(entry + 10, eof);
  xe::store_and_swap<uint32_t>(entry + 14, 4);
  xe::store_and_swap<uint32_t>(wire.data() + 12, view->entries_used + 1);
  xe::store_and_swap<uint32_t>(wire.data() + tail, eof + 4);
  xe::store_and_swap<uint32_t>(wire.data() + tail + 4, remaining - 4);
  wire.insert(wire.end(), {0xca, 0xfe, 0, 0x7f});
  REQUIRE_FALSE(ValidateGpdSnapshot(wire));
}
// A real SPA container: title header, English XSTR, and one achievement. The normal
// SpaInfo parser, XamState loader, and ProfileGpdTitleData capture consume it;
// no mocked title accessor or prebuilt ProfileGpdTitleData bypasses that path.
std::shared_ptr<const SpaInfo> InstallRosterSpa(ProfileAbiRuntime& runtime,
                                               uint32_t title) {
  constexpr std::string_view name = "Roster Actual SPA";
  constexpr size_t data = sizeof(XdbfHeader) + 3 * sizeof(XdbfEntry);
  constexpr size_t title_size = sizeof(XdbfSectionHeader) + sizeof(TitleHeaderData);
  constexpr size_t strings_size = sizeof(XdbfSectionHeaderEx) +
                                  sizeof(XdbfStringTableEntry) + name.size();
  constexpr size_t achievements_size = sizeof(XdbfSectionHeaderEx) + sizeof(AchievementTableEntry);
  std::vector<uint8_t> bytes(data + title_size + strings_size + achievements_size, 0);
  auto u16 = [&](size_t at, uint16_t value) { xe::store_and_swap<uint16_t>(bytes.data() + at, value); };
  auto u32 = [&](size_t at, uint32_t value) { xe::store_and_swap<uint32_t>(bytes.data() + at, value); };
  auto u64 = [&](size_t at, uint64_t value) { xe::store_and_swap<uint64_t>(bytes.data() + at, value); };
  u32(0, kXdbfSignatureXdbf); u32(4, 0x10000); u32(8, 3); u32(12, 3);
  const size_t first = sizeof(XdbfHeader), second = first + sizeof(XdbfEntry);
  const size_t third = second + sizeof(XdbfEntry);
  u16(first, uint16_t(SpaSection::kMetadata)); u64(first + 2, kXdbfIdXthd);
  u32(first + 10, 0); u32(first + 14, title_size);
  u16(second, uint16_t(SpaSection::kStringTable)); u64(second + 2, uint64_t(XLanguage::kEnglish));
  u32(second + 10, title_size); u32(second + 14, strings_size);
  u16(third, uint16_t(SpaSection::kMetadata)); u64(third + 2, kXdbfIdXach);
  u32(third + 10, title_size + strings_size); u32(third + 14, achievements_size);
  u32(data, kXdbfSignatureXthd); u32(data + 4, 1); u32(data + 8, title_size);
  u32(data + 12, title); u32(data + 16, uint32_t(TitleType::kFull));
  u16(data + 20, 1); u32(data + 28, uint32_t(TitleFlags::kAlwaysIncludeInProfile));
  const size_t strings = data + title_size;
  u32(strings, kXdbfSignatureXstr); u32(strings + 4, 1); u32(strings + 8, strings_size);
  u16(strings + 12, 1); u16(strings + 14, kXdbfIdTitle); u16(strings + 16, name.size());
  std::memcpy(bytes.data() + strings + 18, name.data(), name.size());
  const size_t achievements = strings + strings_size;
  u32(achievements, kXdbfSignatureXach); u32(achievements + 4, 1);
  u32(achievements + 8, achievements_size); u16(achievements + 12, 1);
  const size_t achievement = achievements + sizeof(XdbfSectionHeaderEx);
  u16(achievement, 1); u16(achievement + 2, kXdbfIdTitle);
  u16(achievement + 4, kXdbfIdTitle); u16(achievement + 6, kXdbfIdTitle);
  u32(achievement + 8, 1); u16(achievement + 12, 5);
  REQUIRE(XdbfTableView::Parse(bytes));
  SpaInfo parsed(bytes);
  REQUIRE(parsed.title_id() == title); REQUIRE(parsed.include_in_profile());
  REQUIRE_FALSE(parsed.is_system_app());
  runtime.emulator->kernel_state()->xam_state()->LoadSpaInfo(&parsed);
  auto loaded = runtime.emulator->kernel_state()->xam_state()->AcquireSpaInfo();
  REQUIRE(loaded); REQUIRE(loaded->title_id() == title);
  REQUIRE(loaded->title_name() == name);
  REQUIRE(loaded->achievement_count() == 1); REQUIRE(loaded->total_gamerscore() == 5);
  return loaded;
}
struct ReadFixtureProfile {
  uint64_t xuid;
  std::filesystem::path path;
  std::vector<uint8_t> account, dashboard, title, appearance;
  ReadFixtureProfile(ProfileAbiRuntime& runtime, uint64_t id, uint8_t seed)
      : xuid(id), path(runtime.root.path / "content" /
                       ProfileStorageLease::ProfileRelative(id)),
        appearance(1000) {
    X_XAMACCOUNTINFO info{};
    account = SeedOwnedAccount(runtime, id, info);
    for (size_t i = 0; i != appearance.size(); ++i) {
      appearance[i] = uint8_t(seed + i * 37 + (i >> 3));
    }
    GpdInfoProfile dash;
    UserSetting appearance_setting(static_cast<UserSettingId>(kReadAppearance),
                                    appearance);
    UserSetting dashboard_scalar(static_cast<UserSettingId>(kReadScalar),
                                  int32_t(seed));
    dash.UpsertSetting(&appearance_setting);
    dash.UpsertSetting(&dashboard_scalar);
    dashboard = ReadFixtureSerialize(dash);
    ReadFixtureUnknown(dashboard);
    GpdInfoTitle game(kReadTitle);
    UserSetting game_scalar(static_cast<UserSettingId>(kReadScalar),
                            int32_t(seed + 100));
    game.UpsertSetting(&game_scalar);
    title = ReadFixtureSerialize(game);
    ReadFixtureUnknown(title);
    // Genuine files exist before the mounted VFS builds its directory tree.
    ReadFixtureWrite(path / "FFFE07D1.gpd", dashboard);
    ReadFixtureWrite(path / "1234ABCD.gpd", title);
    REQUIRE(runtime.emulator->kernel_state()->xam_state()->profile_manager()->
                LoadAccount(id));
  }
  void Unchanged() const {
    CHECK(ReadFixtureDisk(path / "Account") == account);
    CHECK(ReadFixtureDisk(path / "FFFE07D1.gpd") == dashboard);
    CHECK(ReadFixtureDisk(path / "1234ABCD.gpd") == title);
  }
};
struct ReadExAbiFixture {
  Emulator& emulator;
  Memory& memory;
  uint32_t allocation;
  PPCContext context{};
  explicit ReadExAbiFixture(Emulator& e)
      : emulator(e), memory(*e.memory()),
        allocation(memory.SystemHeapAlloc(0x8000, 0x1000)) {
    REQUIRE(allocation);
    std::memset(memory.TranslateVirtual(allocation), 0xa7, 0x8000);
    context.virtual_membase = memory.virtual_membase();
    context.kernel_state = e.kernel_state();
    context.processor = e.kernel_state()->processor();
    context.r[1] = allocation + 0x7000;
  }
  ~ReadExAbiFixture() { memory.SystemHeapFree(allocation); }
  uint32_t Output() const { return allocation + 0x1000; }
  uint32_t Size() const { return allocation + 0x100; }
  void Capacity(uint32_t size) { xe::store_and_swap<uint32_t>(
      memory.TranslateVirtual(Size()), size); }
  uint32_t Required() const { return xe::load_and_swap<uint32_t>(
      memory.TranslateVirtual(Size())); }
  uint32_t Call(std::span<const uint64_t> users,
                std::span<const uint32_t> settings, uint32_t output,
                uint32_t title = kReadTitle, uint32_t user_index = XUserIndexNone,
                uint32_t options = 0xffffffff) {
    for (size_t i = 0; i != users.size(); ++i) {
      xe::store_and_swap<uint64_t>(memory.TranslateVirtual(
          allocation + uint32_t(i * 8)), users[i]);
    }
    for (size_t i = 0; i != settings.size(); ++i) {
      xe::store_and_swap<uint32_t>(memory.TranslateVirtual(
          allocation + 0x200 + uint32_t(i * 4)), settings[i]);
    }
    context.r[3] = title; context.r[4] = user_index;
    context.r[5] = users.size(); context.r[6] = users.empty() ? 0 : allocation;
    context.r[7] = settings.size(); context.r[8] = allocation + 0x200;
    context.r[9] = options; context.r[10] = Size();
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(
        util::get_arg_stack_ptr<uint32_t>(&context, 0)), output);
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(
        util::get_arg_stack_ptr<uint32_t>(&context, 1)), 0);
    const auto* entry = emulator.export_resolver()->GetExportByOrdinal("xam.xex", 0x22a);
    REQUIRE(entry); REQUIRE(entry->function_data.trampoline);
    entry->function_data.trampoline(&context);
    return uint32_t(context.r[3]);
  }
  X_USER_PROFILE_SETTING Row(size_t index) const {
    X_USER_PROFILE_SETTING row;
    std::memcpy(&row, memory.TranslateVirtual(
        Output() + 8 + uint32_t(index * sizeof(row))), sizeof(row));
    return row;
  }
};
}

TEST_CASE("ReadEx observes signed-out canonical multi-XUID settings without defaults or sign-in",
          "[kernel][profile][read_provider][abi][persistence]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000b1ull, 11);
  ReadFixtureProfile b(runtime, 0xe0000000000000b2ull, 22);
  ReadExAbiFixture f(*runtime.emulator);
  const std::array<uint64_t, 3> users{b.xuid, a.xuid, b.xuid};
  const std::array<uint32_t, 3> ids{kReadAppearance, kReadScalar, kReadAbsent};
  constexpr uint32_t required = 8 + 9 * sizeof(X_USER_PROFILE_SETTING) + 3000;
  f.Capacity(0);
  REQUIRE(f.Call(users, ids, 0) == X_ERROR_INSUFFICIENT_BUFFER);
  REQUIRE(f.Required() == required);
  f.Capacity(required + 32);
  REQUIRE(f.Call(users, ids, f.Output()) == X_ERROR_SUCCESS);
  CHECK(f.Required() == required);
  CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(f.Output())) == 9);
  CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(f.Output() + 4)) ==
        f.Output() + 8);
  for (size_t i = 0; i != users.size(); ++i) {
    const auto& expected = users[i] == a.xuid ? a : b;
    const auto appearance = f.Row(i * 3), scalar = f.Row(i * 3 + 1), absent = f.Row(i * 3 + 2);
    CHECK(appearance.xuid == users[i]);
    CHECK(appearance.setting_id == kReadAppearance);
    CHECK(appearance.source == X_USER_PROFILE_SETTING_SOURCE::TITLE);
    CHECK(appearance.data.type == X_USER_DATA_TYPE::BINARY);
    REQUIRE(appearance.data.data.binary.size == expected.appearance.size());
    const auto payload = uint32_t(appearance.data.data.binary.ptr);
    REQUIRE(payload >= f.Output() + 8 + 9 * sizeof(X_USER_PROFILE_SETTING));
    REQUIRE(payload + expected.appearance.size() <= f.Output() + required);
    CHECK(std::memcmp(f.memory.TranslateVirtual(payload), expected.appearance.data(),
                      expected.appearance.size()) == 0);
    CHECK(scalar.xuid == users[i]);
    CHECK(scalar.data.data.s32 == (users[i] == a.xuid ? 111 : 122));
    CHECK(absent.xuid == users[i]);
    CHECK(absent.source == X_USER_PROFILE_SETTING_SOURCE::NO_VALUE);
    CHECK(absent.data.type == X_USER_DATA_TYPE::UNSET);
    CHECK(absent.data.data.u32 == 0);
  }
  const auto* output = f.memory.TranslateVirtual(f.Output());
  CHECK(output[-1] == 0xa7);
  CHECK(std::all_of(output + required, output + required + 32,
                    [](uint8_t v) { return v == 0xa7; }));
  for (uint8_t slot = 0; slot != 4; ++slot) {
    CHECK_FALSE(runtime.emulator->kernel_state()->xam_state()->GetUserProfile(uint32_t(slot)));
  }
  a.Unchanged(); b.Unchanged();
}

TEST_CASE("Profile read pins have independent normal references and exact retirement",
          "[kernel][profile][read_provider][lifetime][owner]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000b3ull, 33);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto* fs = runtime.emulator->file_system();
  std::error_code error;
  const std::array<uint64_t, 1> ids{p.xuid};
  auto batch = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(batch); REQUIRE_FALSE(error); REQUIRE(batch->IdentityCurrent());
  const auto alias = fmt::format("User_{:016X}:", p.xuid);
  auto device = fs->AcquireDevice(alias);
  REQUIRE(device);
  CHECK_FALSE(pm->DismountProfile(p.xuid));  // A read is not a normal reference.
  REQUIRE(pm->MountProfile(p.xuid));
  REQUIRE(pm->DismountProfile(p.xuid));
  CHECK(fs->AcquireDevice(alias) == device);
  auto idle = pm->TryReserveIdleForManagedFormat(error);
  CHECK_FALSE(idle); CHECK(error == std::errc::device_or_resource_busy);
  // A genuine signed-out observation cannot mint the signed-in writer/model.
  auto writer = pm->CaptureGpdWriter(p.xuid, error);
  CHECK_FALSE(writer.model);
  REQUIRE(error);
  // Nor does the temporary alias bypass the real managed Account validator.
  const auto account_before = OwnedAccountDisk(runtime, p.xuid);
  vfs::File* raw = nullptr; vfs::FileAction action{};
  REQUIRE(fs->OpenFile(nullptr, alias + "\\Account", vfs::FileDisposition::kOverwrite,
      vfs::FileAccess::kGenericRead | vfs::FileAccess::kGenericWrite,
      false, true, &raw, &action) == X_STATUS_SUCCESS);
  REQUIRE(raw);
  const std::array<uint8_t, 24> invalid{}; size_t written = 0;
  REQUIRE(raw->WriteSync(invalid, 0, &written) == X_STATUS_SUCCESS);
  CHECK(XFAILED(raw->Flush()));
  raw->Destroy();
  CHECK(OwnedAccountDisk(runtime, p.xuid) == account_before);
  // A genuine raw GPD read must keep the exact device busy after the read
  // observation releases. Reaping may retry; it must never bypass a File.
  vfs::File* held_gpd = nullptr;
  REQUIRE(fs->OpenFile(nullptr, alias + "\\FFFE07D1.gpd", vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead, false, true, &held_gpd, &action) == X_STATUS_SUCCESS);
  REQUIRE(held_gpd);
  auto close_gpd = [](vfs::File* file) { if (file) { file->Destroy(); } };
  std::unique_ptr<vfs::File, decltype(close_gpd)> busy(held_gpd, close_gpd);
  auto escaped_user = batch->users().front();
  batch.reset();
  pm->ReapReleasedProfileReads();
  CHECK(fs->AcquireDevice(alias) == device);
  busy.reset();
  pm->ReapReleasedProfileReads();
  CHECK_FALSE(fs->AcquireDevice(alias));
  CHECK_FALSE(escaped_user.Setting(kReadTitle, kReadAppearance, error));
  CHECK(error == std::errc::operation_canceled);
  // A new genuine observation gets a new registered device. An old reap does
  // not consume its read reference or dismount the replacement.
  auto replacement = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(replacement); REQUIRE_FALSE(error);
  auto replacement_device = fs->AcquireDevice(alias);
  REQUIRE(replacement_device); CHECK(replacement_device != device);
  pm->ReapReleasedProfileReads();
  CHECK(fs->AcquireDevice(alias) == replacement_device);
  CHECK(replacement->IdentityCurrent());
  replacement.reset(); pm->ReapReleasedProfileReads();
  REQUIRE_FALSE(fs->AcquireDevice(alias));
  // The actual registered producer spelling can differ from its normalized
  // metadata key. Default-name retirement must still select this exact device.
  const auto mixed_alias = fmt::format("uSeR_{:016x}:", p.xuid);
  REQUIRE(pm->MountProfile(p.xuid, mixed_alias));
  auto mixed_device = fs->AcquireDevice(mixed_alias);
  REQUIRE(mixed_device);
  REQUIRE(pm->AcquireProfileStorageLease(p.xuid, error));
  REQUIRE_FALSE(error);
  REQUIRE(pm->DismountProfile(p.xuid));
  CHECK_FALSE(fs->AcquireDevice(mixed_alias));
  CHECK(mixed_device->namespace_retiring());
  idle = pm->TryReserveIdleForManagedFormat(error);
  REQUIRE(idle); REQUIRE_FALSE(error);
  idle.reset();
  p.Unchanged();
}

TEST_CASE("Profile reader closes publication before runtime destruction and cannot rebind",
          "[kernel][profile][read_provider][lifetime]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000b4ull, 44);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto provider = pm->CaptureProfileSettingsReadProvider();
  REQUIRE(provider); REQUIRE(provider->Current(*pm));
  std::error_code error; const std::array<uint64_t, 1> ids{p.xuid};
  auto batch = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(batch); REQUIRE_FALSE(error);
  auto user = batch->users().front();
  runtime.emulator.reset();
  CHECK_FALSE(batch->IdentityCurrent());
  CHECK_FALSE(user.Setting(kReadTitle, kReadAppearance, error));
  CHECK(error == std::errc::operation_canceled);
  runtime.emulator = std::make_unique<Emulator>("", runtime.root.path,
      runtime.root.path / "content", runtime.root.path / "cache");
  REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  auto* replacement_pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  CHECK_FALSE(provider->Current(*replacement_pm));
  auto& memory = *runtime.emulator->memory();
  const auto page = memory.SystemHeapAlloc(0x1000, 0x1000);
  REQUIRE(page);
  std::memset(memory.TranslateVirtual(page), 0x5a, 0x1000);
  const std::array<uint8_t, 4> payload{1, 2, 3, 4};
  const std::array<util::GuestIoWrite, 1> writes{{{page, payload}}};
  CHECK(batch->Publish(memory, writes) == X_STATUS_CANCELLED);
  const auto* bytes = memory.TranslateVirtual(page);
  CHECK(std::all_of(bytes, bytes + 0x1000, [](uint8_t b) { return b == 0x5a; }));
  memory.SystemHeapFree(page);
}

TEST_CASE("Profile read snapshots survive unrelated writes but cancel source retirement",
          "[kernel][profile][read_provider][snapshot][lifetime]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000b5ull, 55);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error; const std::array<uint64_t, 1> ids{p.xuid};
  auto batch = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(batch); REQUIRE_FALSE(error);
  GpdInfoProfile newer(p.dashboard);
  UserSetting changed(static_cast<UserSettingId>(kReadAppearance),
                       std::vector<uint8_t>(1000, 0x9a));
  REQUIRE_FALSE(newer.UpsertSettingPreserving(changed));
  const auto new_wire = ReadFixtureSerialize(newer);
  ReadFixtureWrite(p.path / "FFFE07D1.gpd", new_wire);
  REQUIRE(batch->IdentityCurrent());
  auto value = batch->users().front().Setting(kReadTitle, kReadAppearance, error);
  REQUIRE(value); REQUIRE_FALSE(error);
  CHECK(std::ranges::equal(value->get_extended_data(), p.appearance));
  auto fresh = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(fresh); REQUIRE_FALSE(error);
  value = fresh->users().front().Setting(kReadTitle, kReadAppearance, error);
  REQUIRE(value);
  CHECK(std::ranges::equal(value->get_extended_data(), std::vector<uint8_t>(1000, 0x9a)));
  const auto source = runtime.emulator->kernel_state()->content_manager()->sources().Source("hdd");
  REQUIRE(source);
  source->storage->Retire();
  CHECK_FALSE(batch->IdentityCurrent()); CHECK_FALSE(fresh->IdentityCurrent());
  CHECK_FALSE(batch->users().front().Setting(kReadTitle, kReadAppearance, error));
  CHECK(error == std::errc::operation_canceled);
}

TEST_CASE("ReadEx malformed caller storage never changes output or signs users in",
          "[kernel][profile][read_provider][abi][failure]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000b6ull, 66);
  ReadExAbiFixture f(*runtime.emulator);
  const std::array<uint64_t, 1> user{p.xuid};
  const std::array<uint32_t, 1> setting{kReadAppearance};
  f.Capacity(2000);
  // The whole declared capacity is the existing ABI admission contract.
  const auto before = std::vector<uint8_t>(f.memory.TranslateVirtual(f.Output()),
                                          f.memory.TranslateVirtual(f.Output()) + 2000);
  REQUIRE(f.Call(user, setting, f.Size()) == X_ERROR_INVALID_PARAMETER);
  CHECK(f.Required() == 2000);
  CHECK(std::equal(before.begin(), before.end(), f.memory.TranslateVirtual(f.Output())));
  const std::array<uint64_t, 1> unknown{0xe00000000000ffffull};
  REQUIRE(f.Call(unknown, setting, f.Output()) == X_ERROR_FUNCTION_FAILED);
  CHECK(f.Required() == 2000);
  CHECK(std::equal(before.begin(), before.end(), f.memory.TranslateVirtual(f.Output())));
  // Malformed canonical data is a failure, not an empty/default appearance.
  ReadFixtureWrite(p.path / "FFFE07D1.gpd", std::array<uint8_t, 32>{});
  REQUIRE(f.Call(user, setting, f.Output()) == X_ERROR_FUNCTION_FAILED);
  CHECK(f.Required() == 2000);
  CHECK(std::equal(before.begin(), before.end(), f.memory.TranslateVirtual(f.Output())));
  for (uint8_t slot = 0; slot != 4; ++slot) {
    CHECK_FALSE(runtime.emulator->kernel_state()->xam_state()->GetUserProfile(uint32_t(slot)));
  }
}

TEST_CASE("Signed-out read replays the actual incomplete GPD intent before observing data",
          "[kernel][profile][read_provider][recovery]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000b7ull, 77);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto* content = runtime.emulator->kernel_state()->content_manager();
  REQUIRE(pm->MountProfile(p.xuid));
  std::error_code error;
  auto storage = pm->AcquireProfileStorageLease(p.xuid, error);
  REQUIRE(storage); REQUIRE_FALSE(error);
  const auto owner = content->ProfileIntents("hdd");
  const auto recovery = content->Volume("hdd")->recovery();
  REQUIRE(owner); REQUIRE(recovery);
  auto before = GpdSnapshotStore::Load({p.xuid, storage->source_generation(), kDashboardID}, storage->view());
  REQUIRE(before.readable());
  GpdInfoProfile updated(p.dashboard);
  UserSetting next(static_cast<UserSettingId>(kReadAppearance), std::vector<uint8_t>(1000, 0x42));
  REQUIRE_FALSE(updated.UpsertSettingPreserving(next));
  const auto intended = ReadFixtureSerialize(updated);
  GpdIntentBinding binding{storage->source_id(), storage->device_id(), p.xuid,
      storage->source_generation(), storage->view()->path().lexically_relative(storage->source()->path()),
      {owner->managed_identity()->uuid(), owner->managed_identity()->content_epoch(),
       owner->managed_identity()->directory_file_id()}};
  std::filesystem::path journal;
  auto pending = GpdDurableIntentStore::Prepare(owner, recovery, binding,
      {{GpdIntentAction::kReplace, before, intended}}, error, journal);
  REQUIRE(pending); REQUIRE_FALSE(error); REQUIRE_FALSE(journal.empty());
  pending.reset();  // Real durable Prepared record, no target publication yet.
  REQUIRE(ReadFixtureDisk(p.path / "FFFE07D1.gpd") == p.dashboard);
  const std::array<uint64_t, 1> ids{p.xuid};
  auto batch = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(batch); REQUIRE_FALSE(error);
  auto observed = batch->users().front().Setting(kReadTitle, kReadAppearance, error);
  REQUIRE(observed); REQUIRE_FALSE(error);
  CHECK(std::ranges::equal(observed->get_extended_data(), std::vector<uint8_t>(1000, 0x42)));
  CHECK(ReadFixtureDisk(p.path / "FFFE07D1.gpd") == intended);
  CHECK(OwnedAccountDisk(runtime, p.xuid) == p.account);
  const auto rows = GpdDurableIntentStore::Scan(*owner, error);
  REQUIRE_FALSE(error); REQUIRE(rows.size() == 1);
  CHECK(rows.front().done_record_valid);
  for (uint8_t slot = 0; slot != 4; ++slot) {
    CHECK_FALSE(runtime.emulator->kernel_state()->xam_state()->GetUserProfile(uint32_t(slot)));
  }
}
TEST_CASE("Read provider uses the actual local slot identity and cancels it on signout",
          "[kernel][profile][read_provider][slot][lifetime]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000b8ull, 88);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error; const std::array<uint64_t, 1> ids{p.xuid};
  auto unsigned_batch = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(unsigned_batch); REQUIRE_FALSE(error);
  pm->Login(p.xuid, 2, false);
  REQUIRE(pm->CaptureProfile(uint8_t(2)));
  CHECK_FALSE(unsigned_batch->IdentityCurrent());
  CHECK_FALSE(pm->CaptureProfileSettingsRead(uint8_t(0), kReadTitle, error));
  auto local = pm->CaptureProfileSettingsRead(uint8_t(2), kReadTitle, error);
  REQUIRE(local); REQUIRE_FALSE(error); REQUIRE(local->users().size() == 1);
  CHECK(local->users().front().xuid() == p.xuid);
  ReadExAbiFixture f(*runtime.emulator);
  const std::array<uint32_t, 1> requested{kReadAppearance};
  f.Capacity(1200);
  REQUIRE(f.Call({}, requested, f.Output(), kReadTitle, 2, 1) == X_ERROR_SUCCESS);
  const auto row = f.Row(0);
  CHECK(row.user_index == 2);
  CHECK(row.setting_id == kReadAppearance);
  REQUIRE(row.data.data.binary.size == 1000);
  CHECK(std::memcmp(f.memory.TranslateVirtual(uint32_t(row.data.data.binary.ptr)),
                    p.appearance.data(), p.appearance.size()) == 0);
  pm->Logout(2, false);
  CHECK_FALSE(local->IdentityCurrent());
  CHECK_FALSE(local->users().front().Setting(kReadTitle, kReadAppearance, error));
  CHECK(error == std::errc::operation_canceled);
  CHECK_FALSE(pm->CaptureProfile(uint8_t(2)));
  p.Unchanged();
}

TEST_CASE("Profile inventory exports own authentic Account rows and selected device identity",
          "[kernel][profile][read_provider][enumerator][abi]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000b9ull, 99);
  ReadFixtureProfile b(runtime, 0xe0000000000000baull, 100);
  auto* kernel = runtime.emulator->kernel_state();
  auto* pm = kernel->xam_state()->profile_manager();
  std::error_code error;
  const auto metadata = pm->CaptureCachedAccount(a.xuid, error);
  REQUIRE(metadata); REQUIRE_FALSE(error); REQUIRE(metadata->device_id());
  const auto rows = pm->CaptureProfileInventory(metadata->device_id(), error);
  REQUIRE_FALSE(error); REQUIRE(rows.size() == 2);
  CHECK(rows[0].xuid_offline == a.xuid);
  CHECK(rows[1].xuid_offline == b.xuid);
  CHECK(rows[0].device_id == metadata->device_id());
  CHECK(std::memcmp(&rows[0].account, &metadata->account(), sizeof(X_XAMACCOUNTINFO)) == 0);
  CHECK(pm->CaptureProfileInventory(0xfffffff0, error).empty());
  REQUIRE_FALSE(error);
  ReadExAbiFixture f(*runtime.emulator);
  const auto* create = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x231);
  const auto* enumerate = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x232);
  REQUIRE(create); REQUIRE(create->function_data.trampoline);
  REQUIRE(enumerate); REQUIRE(enumerate->function_data.trampoline);
  f.context.r[3] = metadata->device_id(); f.context.r[4] = f.Size();
  create->function_data.trampoline(&f.context);
  REQUIRE(uint32_t(f.context.r[3]) == X_ERROR_SUCCESS);
  const auto handle = f.Required();
  auto object = kernel->object_table()->LookupObject<ProfileEnumerator>(handle);
  REQUIRE(object); REQUIRE(object->item_size() == sizeof(X_PROFILEENUMRESULT));
  for (const auto& expected : rows) {
    f.context.r[3] = handle; f.context.r[4] = 0;
    f.context.r[5] = f.Output(); f.context.r[6] = 0;
    enumerate->function_data.trampoline(&f.context);
    REQUIRE(uint32_t(f.context.r[3]) == X_ERROR_SUCCESS);
    X_PROFILEENUMRESULT actual{};
    std::memcpy(&actual, f.memory.TranslateVirtual(f.Output()), sizeof(actual));
    CHECK(std::memcmp(&actual, &expected, sizeof(actual)) == 0);
  }
  object.reset();
  REQUIRE(kernel->object_table()->RemoveHandle(handle) == X_STATUS_SUCCESS);
  a.Unchanged(); b.Unchanged();
}
TEST_CASE("Profile publication rejects actual metadata contention and stores both spans atomically",
          "[kernel][profile][read_provider][publication][lifetime]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000bbull, 101);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  const std::array<uint64_t, 1> ids{p.xuid}; std::error_code error;
  auto batch = pm->CaptureProfileSettingsRead(ids, kReadTitle, error);
  REQUIRE(batch); REQUIRE_FALSE(error);
  ReadExAbiFixture f(*runtime.emulator);
  const std::array<uint8_t, 4> payload{9, 8, 7, 6}, metadata{0, 0, 0, 4};
  const std::array<util::GuestIoWrite, 2> writes{{
      {f.Output(), payload}, {f.Size(), metadata}}};
  std::promise<void> locked, release;
  auto release_future = release.get_future();
  auto locked_future = locked.get_future();
  std::thread holder([&] {
    auto lock = ProfileSettingsReadTestAccess::HoldMetadata(*pm);
    locked.set_value();
    release_future.wait();
  });
  auto finish = [&](void*) { release.set_value(); holder.join(); };
  std::unique_ptr<void, decltype(finish)> join(reinterpret_cast<void*>(1), finish);
  REQUIRE(locked_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  CHECK(batch->Publish(f.memory, writes) == X_STATUS_CANCELLED);
  CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(f.Output())) == 0xa7a7a7a7);
  CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(f.Size())) == 0xa7a7a7a7);
  join.reset();
  REQUIRE(batch->Publish(f.memory, writes) == X_STATUS_SUCCESS);
  CHECK(std::memcmp(f.memory.TranslateVirtual(f.Output()), payload.data(), 4) == 0);
  CHECK(std::memcmp(f.memory.TranslateVirtual(f.Size()), metadata.data(), 4) == 0);
  // Memory construction and MMIO are process singletons; a second live
  // Memory is not a supported test prerequisite. The separate retained-batch
  // same-root Kernel restart case exercises a real different-runtime Memory
  // rejection. The production exact live-Memory identity check stays intact;
  // its impossible concurrent-instance branch is source-verified only.
  CHECK(batch->IdentityCurrent());
}

TEST_CASE("ReadEx rechecks actual slot retirement after the physical provider wait",
          "[kernel][profile][read_provider][publication][physical]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile p(runtime, 0xe0000000000000bcull, 102);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm->Login(p.xuid, 1, false);
  REQUIRE(pm->CaptureProfile(uint8_t(1)));
  ReadExAbiFixture f(*runtime.emulator);
  auto& memory = f.memory;
  constexpr uint32_t physical_alias = 0xe1000000;
  memory.SetPhysicalAliasSkipHostProtect(true);
  REQUIRE(memory.LookupHeap(physical_alias)->AllocFixed(physical_alias, 0x1000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  const auto physical = memory.GetPhysicalAddress(physical_alias);
  REQUIRE(physical != UINT32_MAX);
  std::memset(memory.TranslatePhysical(physical), 0x5a, 0x1000);
  struct Retirement {
    ProfileManager* manager;
    uint32_t physical;
    bool ran = false, outside_namespace = false;
  } retirement{pm, physical};
  void* watch = memory.RegisterPhysicalMemoryReadCallback(
      [](void* raw, uint32_t start, uint32_t length, Memory::PhysicalAccess) {
        auto& event = *static_cast<Retirement*>(raw);
        if (event.ran || uint64_t(start) + length <= event.physical ||
            start >= event.physical + 0x1000) { return; }
        event.ran = true;
        event.outside_namespace = !global_critical_region::is_held_by_current_thread();
        event.manager->Logout(1, false);  // Actual owning producer, not a fake token.
      }, &retirement);
  REQUIRE(watch);
  struct Watch { Memory& memory; void* token; ~Watch() {
      memory.UnregisterPhysicalMemoryReadCallback(token); } } owned{memory, watch};
  memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, true);
  const std::array<uint32_t, 1> setting{kReadAppearance};
  f.Capacity(2000);
  REQUIRE(f.Call({}, setting, physical_alias + 32, kReadTitle, 1) == X_ERROR_FUNCTION_FAILED);
  REQUIRE(retirement.ran); CHECK(retirement.outside_namespace);
  CHECK_FALSE(pm->CaptureProfile(uint8_t(1)));
  CHECK(f.Required() == 2000);
  const auto* bytes = memory.TranslatePhysical(physical);
  CHECK(std::all_of(bytes, bytes + 0x1000, [](uint8_t b) { return b == 0x5a; }));
  p.Unchanged();
}
TEST_CASE("Profile enum output guards refuse invalid pages and remove undelivered handles",
          "[kernel][profile][read_provider][enumerator][abi][failure]") {
  ProfileAbiRuntime runtime;
  ReadExAbiFixture f(*runtime.emulator);
  auto* kernel = runtime.emulator->kernel_state();
  const auto* create = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x231);
  REQUIRE(create); REQUIRE(create->function_data.trampoline);
  const auto count_before = kernel->object_table()->GetAllObjects().size();
  const auto call = [&](uint32_t output) {
    f.context.r[3] = 0; f.context.r[4] = output;
    create->function_data.trampoline(&f.context);
    return uint32_t(f.context.r[3]);
  };
  auto* heap = f.memory.LookupHeap(f.Output());
  REQUIRE(heap);
  REQUIRE(heap->Protect(f.Output(), 0x1000, kMemoryProtectRead));
  CHECK(call(f.Output()) == X_ERROR_INVALID_PARAMETER);
  CHECK(kernel->object_table()->GetAllObjects().size() == count_before);
  REQUIRE(heap->Protect(f.Output(), 0x1000, kMemoryProtectRead | kMemoryProtectWrite));
  REQUIRE(heap->Decommit(f.allocation + 0x6000, 0x1000));
  CHECK(call(f.allocation + 0x6000) == X_ERROR_INVALID_PARAMETER);
  CHECK(kernel->object_table()->GetAllObjects().size() == count_before);
  constexpr uint32_t alias = 0xe1000000;
  f.memory.SetPhysicalAliasSkipHostProtect(true);
  REQUIRE(f.memory.LookupHeap(alias)->AllocFixed(alias, 0x1000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  const auto physical = f.memory.GetPhysicalAddress(alias);
  REQUIRE(physical != UINT32_MAX);
  std::memset(f.memory.TranslatePhysical(physical), 0x5a, 0x1000);
  struct Revoke {
    Memory* memory; uint32_t physical; bool ran = false, protected_ok = false;
  } revoke{&f.memory, physical};
  auto* token = f.memory.RegisterPhysicalMemoryReadCallback(
      [](void* raw, uint32_t start, uint32_t length, Memory::PhysicalAccess) {
        auto& value = *static_cast<Revoke*>(raw);
        if (value.ran || uint64_t(start) + length <= value.physical ||
            start >= value.physical + 0x1000) { return; }
        value.ran = true;
        value.protected_ok = value.memory->LookupHeap(alias)->Protect(
            alias, 0x1000, kMemoryProtectNoAccess);
      }, &revoke);
  REQUIRE(token);
  struct Watch { Memory& memory; void* token; ~Watch() {
      memory.UnregisterPhysicalMemoryReadCallback(token); } } owned{f.memory, token};
  f.memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, true);
  CHECK(call(alias + 32) == X_ERROR_FUNCTION_FAILED);
  REQUIRE(revoke.ran); REQUIRE(revoke.protected_ok);
  CHECK(kernel->object_table()->GetAllObjects().size() == count_before);
  const auto* untouched = f.memory.TranslatePhysical(physical);
  CHECK(std::all_of(untouched, untouched + 0x1000, [](uint8_t b) { return b == 0x5a; }));
}
#endif

#if XE_PLATFORM_LINUX
TEST_CASE("Roster Account pause refuses writes without changing durable data or epochs",
          "[kernel][profile_account][roster_pause][persistence]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe0000000000000d1ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, uid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto* content = runtime.emulator->kernel_state()->content_manager();
  REQUIRE(pm->LoadAccount(uid)); REQUIRE(pm->MountProfile(uid));
  std::error_code error;
  auto snapshot = pm->CaptureDurableAccount(uid, error);
  REQUIRE(snapshot); REQUIRE_FALSE(error);
  const auto identity = snapshot->identity();
  const auto serial = identity->write_serial();
  const auto generation = identity->sign_in_generation();
  auto paused = identity->PauseForRoster(error);
  REQUIRE(paused); REQUIRE_FALSE(error);
  CHECK(identity->writes_drained());
  CHECK_FALSE(identity->retired());
  CHECK_FALSE(snapshot->CheckAvailable());
  ProfileAccountWriter writer(identity, content->Volume("hdd")->recovery());
  auto changed = snapshot->account();
  string_util::copy_and_swap_truncating(changed.gamertag, u"After Pause",
                                       xe::countof(changed.gamertag));
  const auto denied = writer.Replace(snapshot, changed);
  CHECK(denied.error == std::errc::device_or_resource_busy);
  CHECK_FALSE(denied.durable);
  CHECK(identity->write_serial() == serial);
  CHECK(identity->sign_in_generation() == generation);
  CHECK(OwnedAccountDisk(runtime, uid) == before);
  CHECK_FALSE(identity->PauseForRoster(error));
  CHECK(error == std::errc::device_or_resource_busy);
  paused.reset();
  const auto actual = writer.Replace(snapshot, changed);
  REQUIRE_FALSE(actual.error); REQUIRE(actual.durable);
  CHECK(actual.durable->account().GetGamertagString() == "After Pause");
  const auto persisted = OwnedAccountDisk(runtime, uid);
  REQUIRE(persisted.size() == before.size());
  CHECK(std::equal(before.end() - 4, before.end(), persisted.end() - 4));
  CHECK(identity->sign_in_generation() == generation);
  CHECK(identity->writes_drained());
  REQUIRE(pm->DismountProfile(uid));
}
TEST_CASE("Roster Account pause requires zero actual writers and cannot reopen retirement",
          "[kernel][profile_account][roster_pause][retirement]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe0000000000000d2ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, uid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid)); REQUIRE(pm->MountProfile(uid));
  std::error_code error;
  const auto snapshot = pm->CaptureDurableAccount(uid, error);
  REQUIRE(snapshot);
  const auto identity = snapshot->identity();
  auto writer = identity->EnterWrite(error);
  REQUIRE(writer); REQUIRE_FALSE(identity->writes_drained());
  CHECK_FALSE(identity->PauseForRoster(error));
  CHECK(error == std::errc::device_or_resource_busy);
  CHECK_FALSE(identity->retired());
  writer.reset();
  auto paused = identity->PauseForRoster(error);
  REQUIRE(paused); REQUIRE(identity->writes_drained());
  identity->Retire();
  paused.reset();
  CHECK(identity->retired());
  CHECK_FALSE(identity->EnterWrite(error));
  CHECK(error == std::errc::operation_canceled);
  CHECK(OwnedAccountDisk(runtime, uid) == before);
  REQUIRE(pm->DismountProfile(uid));
}
#endif


#if XE_PLATFORM_LINUX
namespace {
ProfileRosterDesired RosterDesired(std::array<uint64_t, 4> ids) {
  ProfileRosterDesired desired;
  desired.xuids = ids;
  desired.notify_host_compatibility = false;
  desired.remember_slots = false;
  return desired;
}
void CheckRoster(ProfileManager& pm, std::array<uint64_t, 4> expected) {
  for (uint8_t slot = 0; slot != 4; ++slot) {
    const auto actual = pm.CaptureProfile(slot);
    if (expected[slot]) { REQUIRE(actual); CHECK(actual->xuid() == expected[slot]); }
    else { CHECK_FALSE(actual); }
  }
}
}
TEST_CASE("A local roster batch prepares and cancels four genuine profiles without partial sign-in",
          "[kernel][profile][roster][batch][cancel][persistence]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000d3ull, 13);
  ReadFixtureProfile b(runtime, 0xe0000000000000d4ull, 14);
  ReadFixtureProfile c(runtime, 0xe0000000000000d5ull, 15);
  ReadFixtureProfile d(runtime, 0xe0000000000000d6ull, 16);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  const auto empty = pm.CaptureRosterExpected(error);
  REQUIRE_FALSE(error); REQUIRE(empty.roster);
  const auto desired = RosterDesired({a.xuid, b.xuid, c.xuid, d.xuid});
  auto pending = pm.PrepareRoster(desired, empty, error);
  REQUIRE(pending); REQUIRE_FALSE(error);
  CHECK(pending->result().phase == ProfileRosterPhase::kPrepared);
  CHECK(pending->old_roster() == empty.roster);
  CHECK(pending->prepared_roster()->epoch() == empty.roster->epoch() + 1);
  CheckRoster(pm, {});
  CHECK(pm.CancelRoster(pending).phase == ProfileRosterPhase::kCanceled);
  CheckRoster(pm, {});
  const auto after_cancel = pm.CaptureRosterExpected(error);
  REQUIRE_FALSE(error); REQUIRE(after_cancel.roster);
  CHECK(after_cancel.roster == empty.roster);
  a.Unchanged(); b.Unchanged(); c.Unchanged(); d.Unchanged();
  for (const auto xuid : desired.xuids) {
    CHECK_FALSE(runtime.emulator->file_system()->AcquireDevice(
        fmt::format(kDefaultMountFormat, xuid) + ':'));
  }
  pending = pm.PrepareRoster(desired, after_cancel, error);
  REQUIRE(pending); REQUIRE_FALSE(error);
  const auto committed = pm.CommitHostRoster(pending);
  REQUIRE_FALSE(committed.error);
  CHECK(committed.phase == ProfileRosterPhase::kCommitted);
  CHECK(committed.after_epoch == empty.roster->epoch() + 1);
  CheckRoster(pm, desired.xuids);
  std::set<uint64_t> generations;
  for (const auto& row : pending->prepared_roster()->slots()) {
    CHECK(row.xuid); CHECK(row.device_id == 1); CHECK(row.source_id == "hdd");
    CHECK(row.source_generation); CHECK(row.sign_in_generation);
    CHECK(generations.insert(row.sign_in_generation).second);
  }
  a.Unchanged(); b.Unchanged(); c.Unchanged(); d.Unchanged();
}
TEST_CASE("Identical roster preparation remains a genuine intent and preserves identity and GPD revisions",
          "[kernel][profile][roster][noop][identity]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000d7ull, 17);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error);
  const auto desired = RosterDesired({a.xuid, 0, 0, 0});
  REQUIRE_FALSE(pm.AdoptRoster(desired, expected).error);
  expected = pm.CaptureRosterExpected(error); REQUIRE_FALSE(error);
  auto model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(model);
  const auto origin = model->CaptureOrigin(error); REQUIRE(origin);
  const auto generation = expected.roster->slots()[0].sign_in_generation;
  auto intent = pm.PrepareRoster(desired, expected, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  CHECK(intent->result().phase == ProfileRosterPhase::kPrepared);
  CHECK(intent->prepared_roster()->epoch() == expected.roster->epoch());
  CHECK(intent->prepared_roster()->slots() == expected.roster->slots());
  CHECK(model->IsCurrent(*origin));
  CHECK(pm.CancelRoster(intent).phase == ProfileRosterPhase::kCanceled);
  CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == model);
  intent = pm.PrepareRoster(desired, expected, error); REQUIRE(intent);
  REQUIRE_FALSE(pm.CommitHostRoster(intent).error);
  const auto after = pm.CaptureRosterExpected(error); REQUIRE(after.roster);
  CHECK(after.roster->epoch() == expected.roster->epoch());
  CHECK(after.roster->slots()[0].sign_in_generation == generation);
  CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == model);
  CHECK(model->IsCurrent(*origin)); a.Unchanged();
}
TEST_CASE("A roster replacement or slot move never stages by signing out the accepted old user",
          "[kernel][profile][roster][replace][currentness]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000d8ull, 18);
  ReadFixtureProfile b(runtime, 0xe0000000000000d9ull, 19);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  const auto initial = pm.CaptureRosterExpected(error);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid, b.xuid, 0, 0}), initial).error);
  const auto accepted = pm.CaptureRosterExpected(error); REQUIRE(accepted.roster);
  auto old_model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(old_model);
  auto old_origin = old_model->CaptureOrigin(error); REQUIRE(old_origin);
  auto intent = pm.PrepareRoster(RosterDesired({b.xuid, a.xuid, 0, 0}), accepted, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  CheckRoster(pm, {a.xuid, b.xuid, 0, 0});
  CHECK(old_model->IsCurrent(*old_origin));
  REQUIRE(pm.CancelRoster(intent).phase == ProfileRosterPhase::kCanceled);
  CheckRoster(pm, {a.xuid, b.xuid, 0, 0});
  CHECK(old_model->IsCurrent(*old_origin));
  intent = pm.PrepareRoster(RosterDesired({b.xuid, a.xuid, 0, 0}), accepted, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  REQUIRE_FALSE(pm.CommitHostRoster(intent).error);
  CheckRoster(pm, {b.xuid, a.xuid, 0, 0});
  auto* tracker = runtime.emulator->kernel_state()->xam_state()->user_tracker();
  CHECK_FALSE(tracker->AddUser(a.xuid)); // both actual UIDs are already tracked
  CHECK_FALSE(tracker->AddUser(b.xuid));

  CHECK_FALSE(old_model->IsCurrent(*old_origin));
  const auto moved = pm.CaptureRosterExpected(error); REQUIRE(moved.roster);
  CHECK(moved.roster->epoch() == accepted.roster->epoch() + 1);
  CHECK(moved.roster->slots()[1].sign_in_generation != accepted.roster->slots()[0].sign_in_generation);
  CHECK_FALSE(pm.PrepareRoster(RosterDesired({a.xuid, 0, 0, 0}), accepted, error));
  CHECK(error == std::errc::operation_canceled);
  CheckRoster(pm, {b.xuid, a.xuid, 0, 0}); a.Unchanged(); b.Unchanged();
}
TEST_CASE("Prepared roster input rejects altered Account rows and removed selected sources",
          "[kernel][profile][roster][source][persistence]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000daull, 20);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto* catalog = runtime.emulator->kernel_state()->content_manager();
  std::error_code error;
  const auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto desired = RosterDesired({a.xuid, 0, 0, 0});
  const auto cached = pm.CaptureCachedAccount(a.xuid, error); REQUIRE(cached);
  ProfileRosterSelectionOrigin origin;
  origin.device_id = cached->device_id(); origin.source_id = cached->source_id();
  origin.source_generation = cached->source_generation();
  origin.original_account_row.emplace(); // malformed real request bytes, not authority
  desired.origins[0] = origin;
  CHECK_FALSE(pm.PrepareRoster(desired, expected, error));
  CHECK(error == std::errc::operation_canceled);
  CheckRoster(pm, {}); a.Unchanged();
  desired.origins[0]->original_account_row.reset();
  auto intent = pm.PrepareRoster(desired, expected, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  auto source = catalog->sources().Source("hdd"); REQUIRE(source);
  source->storage->Retire(); // actual producer retirement before publication
  CHECK(pm.CommitHostRoster(intent).error);
  CheckRoster(pm, {});
  CHECK(pm.CancelRoster(intent).phase == ProfileRosterPhase::kCanceled);
  CHECK(pm.CaptureRosterExpected(error).roster->epoch() == expected.roster->epoch());
  a.Unchanged();
}
TEST_CASE("Committed roster metadata and canceled intents cannot retain an old runtime source",
          "[kernel][profile][roster][restart][lifetime]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000dbull, 21);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm->CaptureRosterExpected(error);
  auto desired = RosterDesired({0, 0, a.xuid, 0});
  auto intent = pm->PrepareRoster(desired, expected, error); REQUIRE(intent);
  REQUIRE_FALSE(pm->CommitHostRoster(intent).error);
  expected = pm->CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto canceled = pm->PrepareRoster(RosterDesired({a.xuid, 0, 0, 0}), expected, error);
  REQUIRE(canceled);
  REQUIRE(pm->CancelRoster(canceled).phase == ProfileRosterPhase::kCanceled);
  {const auto selected=ProfileSettingsReadTestAccess::SelectedAccount(*pm,a.xuid);REQUIRE(selected);
   auto account_write=selected->identity()->EnterWrite(error);REQUIRE(account_write);REQUIRE_FALSE(error);account_write.reset();
   const auto model=pm->AcquireProfileGpdModel(uint8_t(2),error);REQUIRE(model);
   CHECK(model->AllocateRequestId(error));REQUIRE_FALSE(error);}
  auto display = pm->CaptureProfile(uint8_t(2)); REQUIRE(display);
  REQUIRE(pm->GetProfile(uint8_t(2)) == display.get()); // actual legacy TLS projection
  std::weak_ptr<const filesystem::ManagedVolumeAuthority> volume;
  {
    const auto source = runtime.emulator->kernel_state()->content_manager()->sources().Source("hdd");
    REQUIRE(source); REQUIRE(source->volume); volume = source->volume;
  }
  runtime.emulator.reset();
  CHECK(volume.expired());
  CHECK(display->xuid() == a.xuid);
  CHECK_FALSE(display->CaptureGpds()); // no global Kernel lookup after weak model expires
 // intent/expected/rosters above are retained metadata
  runtime.emulator = std::make_unique<Emulator>("", runtime.root.path,
      runtime.root.path / "content", runtime.root.path / "cache");
  REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  CHECK_FALSE(pm->PrepareRoster(desired, expected, error));
  CHECK(error == std::errc::operation_canceled);
  REQUIRE(pm->LoadAccount(a.xuid));
  const auto fresh = pm->CaptureRosterExpected(error); REQUIRE_FALSE(error);
  REQUIRE_FALSE(pm->AdoptRoster(desired, fresh).error);
  CheckRoster(*pm, desired.xuids);
  auto batch = pm->CaptureProfileSettingsRead(uint8_t(2), kReadTitle, error);
  REQUIRE(batch); REQUIRE_FALSE(error);
  const auto appearance = batch->users().front().Setting(kReadTitle, kReadAppearance, error);
  REQUIRE(appearance);
  CHECK(std::ranges::equal(appearance->get_extended_data(), a.appearance));
  a.Unchanged();
}
#endif


#if XE_PLATFORM_LINUX
TEST_CASE("An ordinary Account revision stays independent from roster epochs and survives canceled adoption",
          "[kernel][profile][roster][account_revision][persistence]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000dcull, 22);
  ReadFixtureProfile b(runtime, 0xe0000000000000ddull, 23);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid, 0, 0, 0}), expected).error);
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(model);
  const auto origin = model->CaptureOrigin(error); REQUIRE(origin);
  auto changed = *pm.GetAccount(a.xuid);
  string_util::copy_and_swap_truncating(changed.gamertag, u"Renamed Account", xe::countof(changed.gamertag));
  REQUIRE(pm.UpdateAccount(a.xuid, &changed));
  REQUIRE(pm.GetAccount(a.xuid));
  CHECK(pm.GetAccount(a.xuid)->GetGamertagString() == "Renamed Account");
  CHECK(pm.CaptureProfile(uint8_t(0))->name() == "Renamed Account");
  const auto renamed = pm.CaptureRosterExpected(error); REQUIRE(renamed.roster);
  CHECK(renamed.roster->epoch() == expected.roster->epoch());
  CHECK(renamed.roster->slots() == expected.roster->slots());
  CHECK(model->IsCurrent(*origin));
  const auto persisted = OwnedAccountDisk(runtime, a.xuid);
  auto pending = pm.PrepareRoster(RosterDesired({b.xuid, 0, 0, 0}), renamed, error);
  REQUIRE(pending); REQUIRE_FALSE(error);
  auto refused = changed;
  string_util::copy_and_swap_truncating(refused.gamertag, u"Paused Write", xe::countof(refused.gamertag));
  CHECK_FALSE(pm.UpdateAccount(a.xuid, &refused));
  CHECK(pm.CancelRoster(pending).phase == ProfileRosterPhase::kCanceled);
  CheckRoster(pm, {a.xuid, 0, 0, 0});
  CHECK(pm.GetAccount(a.xuid)->GetGamertagString() == "Renamed Account");
  CHECK(OwnedAccountDisk(runtime, a.xuid) == persisted);
  CHECK(model->IsCurrent(*origin)); b.Unchanged();
}
TEST_CASE("An original local logon batch with an unavailable row cannot partially replace a valid slot",
          "[kernel][profile][roster][public_export][failure]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe0000000000000deull, 24);
  ReadFixtureProfile b(runtime, 0xe0000000000000dfull, 25);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  const auto start = pm.CaptureRosterExpected(error);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid, 0, 0, 0}), start).error);
  const auto before = pm.CaptureRosterExpected(error); REQUIRE(before.roster);
  auto model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(model);
  const auto origin = model->CaptureOrigin(error); REQUIRE(origin);
  auto& memory = *runtime.emulator->memory();
  const auto page = memory.SystemHeapAlloc(0x1000, 0x1000); REQUIRE(page);
  struct Allocation { Memory& memory; uint32_t page; ~Allocation() { memory.SystemHeapFree(page); } } owned{memory, page};
  const std::array<uint64_t, 4> request{b.xuid, 0xe00000000000fffeull, 0, 0};
  for (uint8_t i = 0; i != 4; ++i) { xe::store_and_swap<uint64_t>(memory.TranslateVirtual(page + i * 8), request[i]); }
  const auto* call = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x20B);
  REQUIRE(call); REQUIRE(call->function_data.trampoline);
  PPCContext context{}; context.virtual_membase = memory.virtual_membase();
  context.kernel_state = runtime.emulator->kernel_state(); context.processor = runtime.emulator->kernel_state()->processor();
  context.r[1] = page + 0x800; context.r[3] = page;
  context.r[4] = uint32_t(UserLogonFlags::AddUser); context.r[5] = 0;
  call->function_data.trampoline(&context);
  CHECK(uint32_t(context.r[3]) != X_ERROR_SUCCESS);
  CheckRoster(pm, {a.xuid, 0, 0, 0});
  CHECK(pm.CaptureRosterExpected(error).roster->epoch() == before.roster->epoch());
  CHECK(model->IsCurrent(*origin)); a.Unchanged(); b.Unchanged();
}
TEST_CASE("An exact secondary profile origin stays on its real device and can sign out after source removal",
          "[kernel][profile][roster][secondary_device][removal]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile primary(runtime, 0xe0000000000000e1ull, 26);
  const auto usb = runtime.root.path / "roster-usb";
  REQUIRE(std::filesystem::create_directories(usb / "Content"));
  const auto secondary_path = usb / "Content" / ProfileStorageLease::ProfileRelative(primary.xuid);
  REQUIRE(std::filesystem::create_directories(secondary_path));
  ReadFixtureWrite(secondary_path / "Account", primary.account);
  ReadFixtureWrite(secondary_path / "FFFE07D1.gpd", primary.dashboard);
  ReadFixtureWrite(secondary_path / "1234ABCD.gpd", primary.title);
  ContentSource selected;
  selected.id = "roster-usb"; selected.device_id = 0x57;
  selected.device_type = DeviceType::USBMASS; selected.read_only = false;
  selected.root = usb / "Content"; selected.host_volume_root = usb;
  selected.private_state_parent = runtime.root.path;
  selected.guest_volume_root = "\\Device\\Mass57PartitionFile\\Storage";
  selected.guest_root = selected.guest_volume_root + "\\Content";
  selected.name = u"Roster source";
  auto* catalog = runtime.emulator->kernel_state()->content_manager();
  REQUIRE(catalog->RegisterManagedSource(selected));
  auto source = catalog->sources().Source(selected.id); REQUIRE(source); REQUIRE(source->available);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  const auto empty = pm->CaptureRosterExpected(error); REQUIRE(empty.roster);
  auto desired = RosterDesired({0, primary.xuid, 0, 0});
  desired.remember_slots = true;
  ProfileRosterSelectionOrigin exact;
  exact.source_id = selected.id; exact.source_generation = source->generation;
  exact.device_id = selected.device_id; desired.origins[1] = exact;
  auto wrong = desired; ++wrong.origins[1]->source_generation;
  CHECK_FALSE(pm->PrepareRoster(wrong, empty, error));
  CheckRoster(*pm, {});
  const auto outcome = pm->AdoptRoster(desired, empty);
  REQUIRE_FALSE(outcome.error); REQUIRE(outcome.phase == ProfileRosterPhase::kCommitted);
  auto storage = pm->AcquireProfileStorageLease(primary.xuid, error); REQUIRE(storage);
  CHECK(storage->device_id() == selected.device_id);
  CHECK(storage->source_id() == selected.id);
  CHECK(storage->view()->path() == secondary_path);
  CHECK(pm->GetProfileContentPath(primary.xuid, kReadTitle, XContentType::kSavedGame) ==
      selected.root / fmt::format("{:016X}", primary.xuid) / "1234ABCD" / "00000001");
  auto actual = pm->CaptureRosterExpected(error); REQUIRE(actual.roster);
  CHECK(actual.roster->slots()[1].device_id == selected.device_id);
  CHECK(actual.roster->slots()[1].source_id == selected.id);
  CHECK(ReadFixtureDisk(secondary_path / "Account") == primary.account);
  primary.Unchanged();
  storage.reset(); source.reset();
  // The actual remembered UID+source metadata must select USB on a new Kernel,
  // despite the genuine same-XUID primary copy also being present.
  runtime.emulator.reset();
  runtime.emulator = std::make_unique<Emulator>("", runtime.root.path,
      runtime.root.path / "content", runtime.root.path / "cache");
  REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  catalog = runtime.emulator->kernel_state()->content_manager();
  REQUIRE(catalog->RegisterManagedSource(selected));
  pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm->InitializeProfiles();
  CheckRoster(*pm, {0, primary.xuid, 0, 0});
  storage = pm->AcquireProfileStorageLease(primary.xuid, error); REQUIRE(storage);
  CHECK(storage->device_id() == selected.device_id);
  CHECK(storage->source_id() == selected.id);
  CHECK(storage->view()->path() == secondary_path);
  actual = pm->CaptureRosterExpected(error); REQUIRE(actual.roster);
  storage.reset();
  const auto removed = runtime.root.path / "roster-usb-removed";
  std::filesystem::rename(usb, removed);
  const auto unavailable = catalog->sources().Source(selected.id);
  REQUIRE(unavailable); REQUIRE_FALSE(unavailable->available);
  const auto signed_out = pm->AdoptRoster(RosterDesired({}), actual);
  REQUIRE_FALSE(signed_out.error); CHECK(signed_out.phase == ProfileRosterPhase::kCommitted);
  CheckRoster(*pm, {});
  CHECK(ReadFixtureDisk(removed / "Content" / ProfileStorageLease::ProfileRelative(primary.xuid) / "Account") == primary.account);
  primary.Unchanged();
}
namespace {
// A real competing PM metadata owner, with bounded admission observation and
// unconditional join before the genuine Kernel fixture can be destroyed.
struct HeldRosterMetadata {
  std::promise<void> entered, release;
  std::future<void> ready;
  std::thread worker;
  explicit HeldRosterMetadata(ProfileManager& pm) : ready(entered.get_future()) {
    auto wait = release.get_future();
    worker = std::thread([this, &pm, wait = std::move(wait)]() mutable {
      auto held = ProfileSettingsReadTestAccess::HoldMetadata(pm);
      entered.set_value(); wait.wait();
    });
  }
  ~HeldRosterMetadata() { release.set_value(); worker.join(); }
};
}
TEST_CASE("Bootstrap roster preparation retains a real busy attempt before retry",
          "[kernel][profile][roster][bootstrap_retry]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime, 0xe0000000000000e1ull, 31);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm.Login(profile.xuid, 0, false);
  std::error_code error;
  const auto before = pm.CaptureRosterExpected(error); REQUIRE_FALSE(error); REQUIRE(before.roster);
  auto snapshot = pm.CaptureDurableAccount(profile.xuid, error); REQUIRE(snapshot);
  auto writer = snapshot->identity()->EnterWrite(error); REQUIRE(writer);
  std::shared_ptr<ProfileRosterIntent> retained;
  auto prepared = ProfileSettingsReadTestAccess::PrepareAttempt(
      pm, RosterDesired({profile.xuid, 0, 0, 0}), before, error, retained);
  CHECK_FALSE(prepared); CHECK(error == std::errc::device_or_resource_busy);
  REQUIRE(retained); // Prepare returned null after publishing a real operation.
  CHECK(pm.CancelRoster(retained).phase == ProfileRosterPhase::kCanceled);
  CheckRoster(pm, {profile.xuid, 0, 0, 0});
  writer.reset(); retained.reset();
  const auto fresh = pm.CaptureRosterExpected(error); REQUIRE_FALSE(error);
  CHECK(fresh.roster == before.roster);
  prepared = ProfileSettingsReadTestAccess::PrepareAttempt(
      pm, RosterDesired({profile.xuid, 0, 0, 0}), fresh, error, retained);
  REQUIRE(prepared); REQUIRE_FALSE(error); CHECK(retained == prepared);
  CHECK(prepared->result().phase == ProfileRosterPhase::kPrepared);
  CHECK(prepared->prepared_roster()->epoch() == before.roster->epoch());
  // One actual prepared operation owns the slot. A second attempt cannot
  // replace it, and no native publication or caller completion is simulated.
  std::shared_ptr<ProfileRosterIntent> duplicate;
  CHECK_FALSE(ProfileSettingsReadTestAccess::PrepareAttempt(
      pm, RosterDesired({profile.xuid, 0, 0, 0}), fresh, error, duplicate));
  CHECK_FALSE(duplicate);
  CHECK(pm.CancelRoster(retained).phase == ProfileRosterPhase::kCanceled);
  profile.Unchanged();
}
TEST_CASE("Bootstrap cleanup cannot prepare over a busy exact cancellation",
          "[kernel][profile][roster][bootstrap_retry][cancel]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime, 0xe0000000000000e2ull, 32);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm.Login(profile.xuid, 0, false);
  std::error_code error;
  const auto before = pm.CaptureRosterExpected(error); REQUIRE(before.roster);
  std::shared_ptr<ProfileRosterIntent> retained;
  auto prepared = ProfileSettingsReadTestAccess::PrepareAttempt(
      pm, RosterDesired({profile.xuid, 0, 0, 0}), before, error, retained);
  REQUIRE(prepared);
  {
    HeldRosterMetadata held(pm);
    REQUIRE(held.ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto blocked = pm.CancelRoster(retained);
    CHECK(blocked.error == std::errc::device_or_resource_busy);
    CHECK(blocked.phase == ProfileRosterPhase::kPrepared);
    ProfileSettingsReadTestAccess::RetryCleanup(pm);
    CHECK(retained->result().phase == ProfileRosterPhase::kPrepared);
    CHECK_FALSE(pm.CaptureRosterExpected(error).roster);
    CHECK(error == std::errc::device_or_resource_busy);
  }
  const auto canceled = pm.CancelRoster(retained);
  REQUIRE(canceled.phase == ProfileRosterPhase::kCanceled);
  retained.reset(); prepared.reset();
  const auto after = pm.CaptureRosterExpected(error); REQUIRE_FALSE(error);
  CHECK(after.roster == before.roster); CheckRoster(pm, {profile.xuid, 0, 0, 0});
  // A fatal malformed next request is not contention and never publishes a
  // pending attempt. Stop/cancel requires no synthetic native success.
  auto invalid = RosterDesired({profile.xuid, profile.xuid, 0, 0});
  CHECK_FALSE(ProfileSettingsReadTestAccess::PrepareAttempt(pm, invalid, after, error, retained));
  CHECK(error == std::errc::invalid_argument); CHECK_FALSE(retained);
  CHECK(pm.CaptureRosterExpected(error).roster == before.roster);
  profile.Unchanged();
}
namespace {
// Real global namespace contention, no fake Busy injection or cache writes.
struct HeldRosterNamespace {
  std::promise<void> entered, release;
  std::future<void> ready;
  std::thread worker;
  bool released=false;
  std::atomic<bool> watchdog_released{false};
  HeldRosterNamespace():ready(entered.get_future()) {
    auto done=release.get_future();
    worker=std::thread([this,done=std::move(done)]()mutable{
      auto guard=xe::global_critical_region::AcquireDirect();
      entered.set_value();
      if(done.wait_for(std::chrono::seconds(5))!=std::future_status::ready)
        watchdog_released.store(true,std::memory_order_release);
    });
  }
  void Release(){if(!released){released=true;release.set_value();worker.join();}}
  ~HeldRosterNamespace(){Release();}
};
}
TEST_CASE("Shared roster preparation retries real namespace contention with the same accepted roster",
          "[kernel][profile][roster][prepare_retry][namespace]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime,0xe000000000000101ull,41);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm.Login(profile.xuid,0,false);
  std::error_code error;
  const auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);REQUIRE_FALSE(error);
  const auto desired=RosterDesired({profile.xuid,0,0,0});
  auto model=pm.AcquireProfileGpdModel(uint8_t(0),error);REQUIRE(model);
  const auto origin=model->CaptureOrigin(error);REQUIRE(origin);
  HeldRosterNamespace held;
  REQUIRE(held.ready.wait_for(std::chrono::seconds(1))==std::future_status::ready);
  size_t waits=0;
  const auto prepared=ProfileSettingsReadTestAccess::PrepareWithProgress(
      pm,desired,expected,error,std::chrono::steady_clock::now()+std::chrono::seconds(2),[&]{
        ++waits;held.Release();return true;
      });
  REQUIRE(prepared);REQUIRE_FALSE(error);CHECK(waits==1);
  CHECK_FALSE(held.watchdog_released.load(std::memory_order_acquire));
  CHECK(prepared->old_roster()==expected.roster);
  CHECK(prepared->prepared_roster()->slots()==expected.roster->slots());
  CHECK(prepared->result().phase==ProfileRosterPhase::kPrepared);
  CHECK(model->IsCurrent(*origin));CheckRoster(pm,desired.xuids);
  CHECK(pm.CancelRoster(prepared).phase==ProfileRosterPhase::kCanceled);
  profile.Unchanged();
}
TEST_CASE("Host roster adoption waits for real namespace contention instead of aborting sign in",
          "[kernel][profile][roster][prepare_retry][host]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime,0xe000000000000106ull,46);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm.Login(profile.xuid,0,false);
  std::error_code error;const auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  const auto desired=RosterDesired({profile.xuid,0,0,0});
  std::promise<ProfileRosterResult> returned;auto done=returned.get_future();
  std::jthread worker;
  {
    auto foreign_namespace=xe::global_critical_region::AcquireDirect();
    worker=std::jthread([&]{returned.set_value(pm.AdoptRoster(desired,expected));});
    CHECK(done.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout);
  }
  REQUIRE(done.wait_for(std::chrono::seconds(5))==std::future_status::ready);
  const auto result=done.get();REQUIRE_FALSE(result.error);
  CHECK(result.phase==ProfileRosterPhase::kCommitted);
  const auto after=pm.CaptureRosterExpected(error);REQUIRE(after.roster);
  CHECK(after.roster->slots()==expected.roster->slots());CheckRoster(pm,desired.xuids);
  profile.Unchanged();
}
TEST_CASE("Shared roster preparation never starts a fresh attempt over busy exact cancellation",
          "[kernel][profile][roster][prepare_retry][cancel]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime,0xe000000000000102ull,42);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  const auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  std::shared_ptr<ProfileRosterIntent> retained;
  const auto prepared=ProfileSettingsReadTestAccess::PrepareAttempt(
      pm,RosterDesired({profile.xuid,0,0,0}),expected,error,retained);
  REQUIRE(prepared);REQUIRE(retained);
  {
    HeldRosterMetadata held(pm);
    REQUIRE(held.ready.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    const auto canceled=ProfileSettingsReadTestAccess::CancelPreparation(pm,retained);
    CHECK(canceled.error==std::errc::device_or_resource_busy);
    CHECK(canceled.phase==ProfileRosterPhase::kPrepared);
    size_t waits=0;
    const auto another=ProfileSettingsReadTestAccess::PrepareWithProgress(
        pm,RosterDesired({}),expected,error,std::chrono::steady_clock::now(),[&]{++waits;return true;});
    CHECK_FALSE(another);CHECK(waits==0);
    CHECK(retained->result().phase==ProfileRosterPhase::kPrepared);
  }
  size_t fresh_waits=0;
  CHECK_FALSE(ProfileSettingsReadTestAccess::PrepareWithProgress(pm,RosterDesired({}),expected,error,
      std::chrono::steady_clock::now()+std::chrono::seconds(1),[&]{++fresh_waits;return true;}));
  CHECK(error==std::errc::operation_canceled);CHECK(fresh_waits==0);
  CHECK(retained->result().phase==ProfileRosterPhase::kPrepared);
  ProfileSettingsReadTestAccess::RetryCleanup(pm);
  CHECK(retained->result().phase==ProfileRosterPhase::kCanceled);
  const auto after=pm.CaptureRosterExpected(error);REQUIRE(after.roster);
  CHECK(after.roster==expected.roster);CheckRoster(pm,{});profile.Unchanged();
}
TEST_CASE("Shared roster preparation preserves hard stale and inherited namespace refusals",
          "[kernel][profile][roster][prepare_retry][hard_error]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime,0xe000000000000103ull,43);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;const auto old=pm.CaptureRosterExpected(error);REQUIRE(old.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({profile.xuid,0,0,0}),old).error);
  size_t waits=0;
  const auto wait=[&]{++waits;return true;};
  auto prepared=ProfileSettingsReadTestAccess::PrepareWithProgress(pm,RosterDesired({}),old,error,
      std::chrono::steady_clock::now()+std::chrono::seconds(1),wait);
  CHECK_FALSE(prepared);CHECK(error==std::errc::operation_canceled);CHECK(waits==0);
  const auto current=pm.CaptureRosterExpected(error);REQUIRE(current.roster);
  prepared=ProfileSettingsReadTestAccess::PrepareWithProgress(pm,
      RosterDesired({profile.xuid,profile.xuid,0,0}),current,error,
      std::chrono::steady_clock::now()+std::chrono::seconds(1),wait);
  CHECK_FALSE(prepared);CHECK(error==std::errc::invalid_argument);CHECK(waits==0);
  {
    auto namespace_guard=xe::global_critical_region::AcquireDirect();
    prepared=ProfileSettingsReadTestAccess::PrepareWithProgress(pm,RosterDesired({}),current,error,
        std::chrono::steady_clock::now()+std::chrono::seconds(1),wait);
    CHECK_FALSE(prepared);CHECK(error==std::errc::device_or_resource_busy);CHECK(waits==0);
  }
  CheckRoster(pm,{profile.xuid,0,0,0});profile.Unchanged();
}
TEST_CASE("Deferred preparation cancellation remains owned and can finish after admission stop",
          "[kernel][profile][roster][prepare_retry][stop]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime,0xe000000000000104ull,44);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;const auto before=pm.CaptureRosterExpected(error);REQUIRE(before.roster);
  std::shared_ptr<ProfileRosterIntent> retained;
  REQUIRE(ProfileSettingsReadTestAccess::PrepareAttempt(
      pm,RosterDesired({profile.xuid,0,0,0}),before,error,retained));
  {
    HeldRosterMetadata held(pm);
    REQUIRE(held.ready.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    CHECK(ProfileSettingsReadTestAccess::CancelPreparation(pm,retained).phase==ProfileRosterPhase::kPrepared);
    ProfileSettingsReadTestAccess::BeginStop(pm);
  }
  size_t waits=0;
  CHECK_FALSE(ProfileSettingsReadTestAccess::PrepareWithProgress(pm,RosterDesired({}),before,error,
      std::chrono::steady_clock::now()+std::chrono::seconds(1),[&]{++waits;return true;}));
  CHECK(error==std::errc::operation_canceled);CHECK(waits==0);
  ProfileSettingsReadTestAccess::RetryCleanup(pm);
  CHECK(retained->result().phase==ProfileRosterPhase::kCanceled);
  CHECK_FALSE(runtime.emulator->file_system()->AcquireDevice(fmt::format(kDefaultMountFormat,profile.xuid)+':'));
  profile.Unchanged();
}
TEST_CASE("A deferred preparation cancel cannot cancel a later genuinely armed native PM operation",
          "[kernel][profile][roster][prepare_retry][late_enter]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime,0xe000000000000105ull,45);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;const auto before=pm.CaptureRosterExpected(error);REQUIRE(before.roster);
  const auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto prepared=pm.PrepareRoster(RosterDesired({profile.xuid,0,0,0}),before,error);REQUIRE(prepared);
  {
    HeldRosterMetadata held(pm);
    REQUIRE(held.ready.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    CHECK(ProfileSettingsReadTestAccess::CancelPreparation(pm,prepared).phase==ProfileRosterPhase::kPrepared);
  }
  // Real PM arm, not an invented PPC execution/protocol/completion receipt.
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,prepared,binding,error);
  REQUIRE(cell);REQUIRE_FALSE(error);
  REQUIRE(prepared->result().phase==ProfileRosterPhase::kNativeEntered);
  ProfileSettingsReadTestAccess::RetryCleanup(pm);
  CHECK(prepared->result().phase==ProfileRosterPhase::kNativeEntered);
  CHECK(pm.CancelRoster(prepared).phase==ProfileRosterPhase::kNativeEntered);
  CHECK(prepared->result().native_protocol_returned==false);
  CHECK(prepared->result().maps_committed==false);
  profile.Unchanged();
}

TEST_CASE("Account cache phase progress keeps one real preparing intent across namespace contention",
          "[kernel][profile][roster][phase_progress][account_cache]") {
  ProfileAbiRuntime runtime;ReadFixtureProfile profile(runtime,0xe000000000000107ull,47);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm.Login(profile.xuid,0,false);
  std::error_code error;const auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  const auto desired=RosterDesired({profile.xuid,0,0,0});
  HeldRosterNamespace held;
  REQUIRE(held.ready.wait_for(std::chrono::seconds(1))==std::future_status::ready);
  std::shared_ptr<ProfileRosterIntent> retained,observed;
  size_t waits=0;
  const std::function<bool()> wait=[&]{
    ++waits;observed=retained;REQUIRE(observed);
    CHECK(observed->result().phase==ProfileRosterPhase::kPreparing);
    held.Release();return true;
  };
  ProfilePreparationProgress progress{std::chrono::steady_clock::now()+std::chrono::seconds(2),wait};
  const auto prepared=ProfileSettingsReadTestAccess::PrepareAttemptPhases(
      pm,desired,expected,error,retained,&progress);
  REQUIRE(prepared);REQUIRE_FALSE(error);CHECK(waits==1);
  CHECK(prepared==observed);CHECK(prepared==retained);
  CHECK(prepared->result().phase==ProfileRosterPhase::kPrepared);
  CHECK(prepared->old_roster()==expected.roster);
  CHECK_FALSE(held.watchdog_released.load());
  CHECK(pm.CancelRoster(prepared).phase==ProfileRosterPhase::kCanceled);
  CheckRoster(pm,desired.xuids);profile.Unchanged();
}
TEST_CASE("Stopping nested profile phase progress cancels the exact preparation without another attempt",
          "[kernel][profile][roster][phase_progress][stop]") {
  ProfileAbiRuntime runtime;ReadFixtureProfile profile(runtime,0xe000000000000108ull,48);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm.Login(profile.xuid,0,false);
  std::error_code error;const auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  HeldRosterNamespace held;
  REQUIRE(held.ready.wait_for(std::chrono::seconds(1))==std::future_status::ready);
  size_t waits=0;
  const auto prepared=ProfileSettingsReadTestAccess::PrepareWithProgress(pm,
      RosterDesired({profile.xuid,0,0,0}),expected,error,
      std::chrono::steady_clock::now()+std::chrono::seconds(2),[&]{
        ++waits;ProfileSettingsReadTestAccess::BeginStop(pm);held.Release();return true;
      });
  CHECK_FALSE(prepared);CHECK(error==std::errc::operation_canceled);CHECK(waits==1);
  CHECK_FALSE(held.watchdog_released.load());
  ProfileSettingsReadTestAccess::RetryCleanup(pm);profile.Unchanged();
}
TEST_CASE("Prepared native read tickets distinguish actual PM contention from stale authority",
          "[kernel][profile][roster][bootstrap_retry][ticket]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime, 0xe0000000000000e3ull, 33);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm.Login(profile.xuid, 0, false);
  std::error_code error;
  const auto before = pm.CaptureRosterExpected(error); REQUIRE(before.roster);
  const auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  std::shared_ptr<ProfileRosterIntent> retained;
  const auto prepared = ProfileSettingsReadTestAccess::PrepareAttempt(
      pm, RosterDesired({profile.xuid, 0, 0, 0}), before, error, retained);
  REQUIRE(prepared); REQUIRE_FALSE(error);
  {
    HeldRosterMetadata held(pm);
    REQUIRE(held.ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    CHECK_FALSE(ProfileSettingsReadTestAccess::Ticket(*prepared, *binding, error));
    CHECK(error == std::errc::device_or_resource_busy);
  }
  const auto ticket = ProfileSettingsReadTestAccess::Ticket(*prepared, *binding, error);
  REQUIRE(ticket); REQUIRE_FALSE(error);
  CHECK(pm.CancelRoster(prepared).phase == ProfileRosterPhase::kCanceled);
  CHECK_FALSE(ProfileSettingsReadTestAccess::Ticket(*prepared, *binding, error));
  CHECK(error == std::errc::operation_canceled);
  CheckRoster(pm, {profile.xuid, 0, 0, 0}); profile.Unchanged();
  // This genuine common ticket owns no native execution/readiness witness.
}

#endif


#if XE_PLATFORM_LINUX
namespace {
std::vector<uint8_t> SeedDiscoveryAccount(const std::filesystem::path& content,
                                        uint64_t xuid,
                                        std::u16string_view gamertag) {
  const auto package = content / ProfileStorageLease::ProfileRelative(xuid);
  REQUIRE(std::filesystem::create_directories(package));
  X_XAMACCOUNTINFO account{};
  account.reserved_flags = 1;
  account.passport_password[7] = char(0x8e);
  string_util::copy_and_swap_truncating(account.gamertag, gamertag,
                                       xe::countof(account.gamertag));
  std::vector<uint8_t> bytes(ProfileAccountSnapshot::kPrefixBytes);
  ProfileManager::EncryptAccountFile(&account, bytes.data());
  bytes.insert(bytes.end(), {0x91, 0, 0x82, 0xff});
  ReadFixtureWrite(package / "Account", bytes);
  return bytes;
}
ContentSourceState RegisterDiscoverySource(ProfileAbiRuntime& runtime) {
  const auto volume = runtime.root.path / "late-profile-volume";
  REQUIRE(std::filesystem::create_directories(volume / "Content"));
  ContentSource descriptor{
      "late-profile-device", 3, DeviceType::USBMASS,
      ContentSourceKind::kManaged, volume / "Content",
      "\\Device\\LateProfiles\\Content", u"Late profiles", false, 0,
      "\\Device\\LateProfiles", volume, runtime.root.path};
  auto& manager = *runtime.emulator->kernel_state()->content_manager();
  auto& fs = *runtime.emulator->file_system();
  std::error_code media_error;
  // XamState already restored the real media owner during Setup. Successful
  // empty inventory requires its actual private metadata owner, not a skipped
  // failed store. Its completed bootstrap must release the data-parent EX
  // lease before another genuine MVA uses that same explicit private parent.
  const auto media_before = manager.RestoreMediaReferences(fs, media_error);
  REQUIRE_FALSE(media_error);
  REQUIRE(media_before.empty());
  REQUIRE(manager.managed_sources_ready());
  REQUIRE(manager.RegisterManagedSource(descriptor));
  const auto source = manager.sources().Source(descriptor.id);
  REQUIRE(source); REQUIRE(source->available); REQUIRE(source->profile_writable);
  REQUIRE(source->storage); REQUIRE(source->managed_identity); REQUIRE(source->volume);
  const auto volume_owner = manager.Volume(descriptor.id);
  REQUIRE(volume_owner);
  REQUIRE(volume_owner == source->volume);
  REQUIRE_FALSE(volume_owner->error());
  const auto profile_intents = manager.ProfileIntents(descriptor.id);
  REQUIRE(profile_intents);
  REQUIRE_FALSE(profile_intents->error());
  const auto media_after = manager.RestoreMediaReferences(fs, media_error);
  REQUIRE_FALSE(media_error);
  REQUIRE(media_after.empty());
  const auto after_source = manager.sources().Source(descriptor.id);
  REQUIRE(after_source);
  REQUIRE(after_source->generation == source->generation);
  // No drive alias is registered here: actual startup also constructs PM
  // after source registration but before the MU guest-drive aliases exist.
  REQUIRE_FALSE(runtime.emulator->file_system()->AcquireDevice(descriptor.guest_root));
  return *source;
}
void RestartDiscoveryRuntime(ProfileAbiRuntime& runtime) {
  runtime.emulator.reset();
  REQUIRE(KernelState::shared() == nullptr);
  runtime.emulator = std::make_unique<Emulator>(
      "", runtime.root.path, runtime.root.path / "content", runtime.root.path / "cache");
  REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
}
}

TEST_CASE("Profile startup discovers authenticated Accounts on the configured MU across restart",
          "[kernel][profile][discovery][restart][device]") {
  ProfileAbiRuntime runtime;
  runtime.emulator.reset();
  constexpr uint64_t hdd_xuid = 0xE000000000000301ull;
  constexpr uint64_t mu_xuid = 0xE000000000000302ull;
  const auto hdd = SeedDiscoveryAccount(runtime.root.path / "content", hdd_xuid, u"Primary");
  const auto mu = SeedDiscoveryAccount(runtime.root.path / "mu" / "Content", mu_xuid, u"Memory Unit");
  cvars::mount_memory_unit = true;
  for (unsigned restart = 0; restart != 2; ++restart) {
    RestartDiscoveryRuntime(runtime);
    auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
    std::error_code error;
    REQUIRE(pm.GetAccountCount() == 2);
    const auto primary = pm.CaptureCachedAccount(hdd_xuid, error);
    REQUIRE(primary); REQUIRE_FALSE(error);
    CHECK(primary->source_id() == "hdd"); CHECK(primary->device_id() == 1);
    const auto secondary = pm.CaptureCachedAccount(mu_xuid, error);
    REQUIRE(secondary); REQUIRE_FALSE(error);
    CHECK(secondary->source_id() == "memory-unit"); CHECK(secondary->device_id() == 3);
    CHECK(secondary->account().GetGamertagString() == "Memory Unit");
    const auto source = runtime.emulator->kernel_state()->content_manager()->sources().Source("memory-unit");
    REQUIRE(source); CHECK(secondary->source_generation() == source->generation);
    CHECK(secondary->source_root() == source->managed_identity->source()->path());
    {
      auto lock = global_critical_region::AcquireDirect();
      const auto inventory = pm.CaptureProfileInventory(3, error);
      REQUIRE_FALSE(error); REQUIRE(inventory.size() == 1);
      CHECK(uint64_t(inventory[0].xuid_offline) == mu_xuid);
      CHECK(uint32_t(inventory[0].device_id) == 3);
    }
    CheckRoster(pm, {}); // Discovery is not sign-in/native cache publication.
    CHECK(ReadFixtureDisk(runtime.root.path / "content" /
        ProfileStorageLease::ProfileRelative(hdd_xuid) / "Account") == hdd);
    CHECK(ReadFixtureDisk(runtime.root.path / "mu" / "Content" /
        ProfileStorageLease::ProfileRelative(mu_xuid) / "Account") == mu);
  }
}

TEST_CASE("Registered profile refresh authenticates the exact late source and keeps last-known inventory IO-free",
          "[kernel][profile][discovery][device][inventory]") {
  ProfileAbiRuntime runtime;
  const auto source = RegisterDiscoverySource(runtime);
  constexpr uint64_t xuid = 0xE000000000000303ull;
  constexpr uint64_t corrupt_xuid = 0xE000000000000304ull;
  const auto bytes = SeedDiscoveryAccount(source.storage->path(), xuid, u"Late Device");
  auto damaged = SeedDiscoveryAccount(source.storage->path(), corrupt_xuid, u"Corrupt");
  damaged[0] ^= 1;
  ReadFixtureWrite(source.storage->path() /
      ProfileStorageLease::ProfileRelative(corrupt_xuid) / "Account", damaged);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  CHECK(pm.GetAccountCount() == 0);
  pm.ReloadProfiles();
  std::error_code error;
  const auto cached = pm.CaptureCachedAccount(xuid, error);
  REQUIRE(cached); REQUIRE_FALSE(error); CHECK(cached->device_id() == 3);
  CHECK(cached->source_id() == source.source.id);
  CHECK(cached->source_generation() == source.generation);
  CHECK(cached->account().GetGamertagString() == "Late Device");
  CHECK_FALSE(pm.CaptureCachedAccount(corrupt_xuid, error));
  CHECK(error == std::errc::no_such_file_or_directory);
  REQUIRE(pm.MountProfile(xuid)); REQUIRE(pm.DismountProfile(xuid));
  source.storage->Retire();
  CHECK_FALSE(pm.LoadAccount(xuid));
  pm.ReloadProfiles(); // An unavailable source is not an empty fresh inventory.
  CHECK(pm.CaptureCachedAccount(xuid, error) == cached); CHECK_FALSE(error);
  {
    auto lock = global_critical_region::AcquireDirect();
    const auto inventory = pm.CaptureProfileInventory(3, error);
    REQUIRE_FALSE(error); REQUIRE(inventory.size() == 1);
    CHECK(uint64_t(inventory[0].xuid_offline) == xuid);
  }
  CHECK(ReadFixtureDisk(source.storage->path() /
      ProfileStorageLease::ProfileRelative(xuid) / "Account") == bytes);
  CHECK(ReadFixtureDisk(source.storage->path() /
      ProfileStorageLease::ProfileRelative(corrupt_xuid) / "Account") == damaged);
}

TEST_CASE("Duplicate profile discovery refuses unscoped choice while preserving the actual committed origin",
          "[kernel][profile][discovery][conflict][roster]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile primary(runtime, 0xE000000000000305ull, 29);
  ReadFixtureProfile unsigned_profile(runtime, 0xE000000000000306ull, 30);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  const auto before = pm.CaptureRosterExpected(error); REQUIRE(before.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({primary.xuid, 0, 0, 0}), before).error);
  const auto accepted = pm.CaptureRosterExpected(error); REQUIRE(accepted.roster);
  const auto model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(model);
  const auto source = RegisterDiscoverySource(runtime);
  const auto duplicate = SeedDiscoveryAccount(source.storage->path(), primary.xuid, u"Other Device");
  const auto unsigned_duplicate = SeedDiscoveryAccount(source.storage->path(), unsigned_profile.xuid, u"Other Unsigned");
  pm.ReloadProfiles();
  CHECK_FALSE(pm.CaptureCachedAccount(unsigned_profile.xuid, error));
  CHECK(error == std::errc::file_exists);
  CHECK_FALSE(pm.MountProfile(unsigned_profile.xuid));
  CHECK_FALSE(pm.LoadAccount(unsigned_profile.xuid));
  const auto retained_metadata = pm.CaptureCachedAccount(primary.xuid, error);
  REQUIRE(retained_metadata); REQUIRE_FALSE(error);
  const auto retained_account =
      ProfileSettingsReadTestAccess::SelectedAccount(pm, primary.xuid);
  REQUIRE(retained_account);
  const auto unchanged_selected = [&] {
    CheckRoster(pm, {primary.xuid, 0, 0, 0});
    const auto actual = pm.CaptureRosterExpected(error);
    REQUIRE(actual.roster == accepted.roster); REQUIRE_FALSE(error);
    CHECK(actual.roster->epoch() == accepted.roster->epoch());
    CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == model); REQUIRE_FALSE(error);
    CHECK(pm.CaptureCachedAccount(primary.xuid, error) == retained_metadata);
    REQUIRE_FALSE(error);
    CHECK(ProfileSettingsReadTestAccess::SelectedAccount(pm, primary.xuid) ==
          retained_account);
    CHECK_FALSE(retained_account->identity()->retired());
  };
  CHECK_FALSE(pm.DeleteProfile(unsigned_profile.xuid));
  unchanged_selected();
  CHECK_FALSE(pm.DeleteProfile(primary.xuid));
  unchanged_selected();
  // The exact selected token/origin remains readable and writable. This is
  // not permission to choose the unsigned duplicate or move a slot by XUID.
  const auto active_metadata = pm.CaptureCachedAccount(primary.xuid, error);
  REQUIRE(active_metadata); REQUIRE_FALSE(error);
  CHECK(active_metadata->source_id() == "hdd");
  CHECK(pm.GetProfileContentPath(primary.xuid, 0, XContentType::kInvalid) ==
      runtime.root.path / "content" / fmt::format("{:016X}", primary.xuid));
  REQUIRE(pm.MountProfile(primary.xuid)); REQUIRE(pm.DismountProfile(primary.xuid));
  REQUIRE(pm.LoadAccount(primary.xuid));
  const auto inventory = pm.CaptureProfileInventory(0, error);
  REQUIRE_FALSE(error); REQUIRE(inventory.size() == 1);
  CHECK(uint64_t(inventory[0].xuid_offline) == primary.xuid);
  CHECK(uint32_t(inventory[0].device_id) == 1);
  CHECK(pm.GetAccountCount() == 1);
  CheckRoster(pm, {primary.xuid, 0, 0, 0});
  CHECK(pm.CaptureRosterExpected(error).roster->epoch() == accepted.roster->epoch());
  CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == model);
  // Current selected data remains the actual original Account, even though a
  // new unscoped choice cannot select one of two physical packages by order.
  REQUIRE(pm.GetAccount(primary.xuid));
  CHECK(pm.GetAccount(primary.xuid)->GetGamertagString() == "Account Fixture");
  CHECK_FALSE(pm.PrepareRoster(RosterDesired({0, primary.xuid, 0, 0}), accepted, error));
  CheckRoster(pm, {primary.xuid, 0, 0, 0});
  auto settings = pm.CaptureProfileSettingsRead(uint8_t(0), kReadTitle, error);
  REQUIRE(settings); REQUIRE_FALSE(error);
  const auto appearance = settings->users().front().Setting(kReadTitle, kReadAppearance, error);
  REQUIRE(appearance); REQUIRE_FALSE(error);
  CHECK(std::ranges::equal(appearance->get_extended_data(), primary.appearance));
  settings.reset();
  primary.Unchanged();
  unsigned_profile.Unchanged();
  CHECK(ReadFixtureDisk(source.storage->path() /
      ProfileStorageLease::ProfileRelative(primary.xuid) / "Account") == duplicate);
  CHECK(ReadFixtureDisk(source.storage->path() /
      ProfileStorageLease::ProfileRelative(unsigned_profile.xuid) / "Account") == unsigned_duplicate);
  SECTION("A complete unique scan and authenticated receipt reconcile the conflict") {
    REQUIRE_FALSE(source.storage->Remove(ProfileStorageLease::ProfileRelative(primary.xuid), true));
    REQUIRE_FALSE(source.storage->Remove(ProfileStorageLease::ProfileRelative(unsigned_profile.xuid), true));
    pm.ReloadProfiles();
    const auto restored = pm.CaptureCachedAccount(primary.xuid, error);
    REQUIRE(restored); REQUIRE_FALSE(error);
    CHECK(restored->source_id() == "hdd"); CHECK(restored->device_id() == 1);
    const auto fresh = pm.CaptureProfileInventory(0, error);
    REQUIRE_FALSE(error); REQUIRE(fresh.size() == 2);
    CHECK(uint64_t(fresh[0].xuid_offline) == primary.xuid);
    const auto unsigned_restored = pm.CaptureCachedAccount(unsigned_profile.xuid, error);
    REQUIRE(unsigned_restored); REQUIRE_FALSE(error);
    CHECK(unsigned_restored->source_id() == "hdd");
  }
  SECTION("An unavailable duplicate source never proves a unique replacement") {
    source.storage->Retire();
    pm.ReloadProfiles();
    CHECK_FALSE(pm.CaptureCachedAccount(unsigned_profile.xuid, error));
    CHECK(error == std::errc::file_exists);
    REQUIRE(pm.CaptureCachedAccount(primary.xuid, error)); REQUIRE_FALSE(error);
  }
  CheckRoster(pm, {primary.xuid, 0, 0, 0});
  CHECK(pm.CaptureRosterExpected(error).roster->epoch() == accepted.roster->epoch());
  CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == model);
  primary.Unchanged();
  unsigned_profile.Unchanged();
  SECTION("A unique requested deletion may remove only its own reserved roster row") {
    ReadFixtureProfile deletable(runtime, 0xE000000000000307ull, 31);
    const auto current = pm.CaptureRosterExpected(error); REQUIRE(current.roster);
    const auto signed_in = pm.AdoptRoster(
        RosterDesired({primary.xuid, deletable.xuid, 0, 0}), current);
    REQUIRE_FALSE(signed_in.error);
    REQUIRE(signed_in.phase == ProfileRosterPhase::kCommitted);
    REQUIRE(pm.DeleteProfile(deletable.xuid));
    CheckRoster(pm, {primary.xuid, 0, 0, 0});
    CHECK_FALSE(pm.CaptureCachedAccount(deletable.xuid, error));
    CHECK(error == std::errc::no_such_file_or_directory);
    CHECK_FALSE(std::filesystem::exists(deletable.path));
    CHECK(std::filesystem::exists(runtime.root.path / "content" /
                                  fmt::format("{:016X}", deletable.xuid)));
    const auto remaining = pm.CaptureCachedAccount(primary.xuid, error);
    REQUIRE(remaining); REQUIRE_FALSE(error);
    CHECK(remaining->source_id() == retained_metadata->source_id());
    CHECK(remaining->source_generation() == retained_metadata->source_generation());
    CHECK(remaining->source_root() == retained_metadata->source_root());
    CHECK(std::memcmp(&remaining->account(), &retained_metadata->account(),
                      sizeof(X_XAMACCOUNTINFO)) == 0);
    const auto remaining_account =
        ProfileSettingsReadTestAccess::SelectedAccount(pm, primary.xuid);
    REQUIRE(remaining_account);
    CHECK(remaining_account->identity() == retained_account->identity());
    CHECK_FALSE(remaining_account->identity()->retired());
    CHECK(remaining_account->identity()->storage() ==
          retained_account->identity()->storage());
    CHECK(remaining_account->identity()->storage()->source_generation() ==
          retained_account->identity()->storage()->source_generation());
    CHECK(std::memcmp(&remaining_account->account(), &retained_account->account(),
                      sizeof(X_XAMACCOUNTINFO)) == 0);
    CHECK(remaining_account->token()->info().filesystem_id ==
          retained_account->token()->info().filesystem_id);
    CHECK(remaining_account->token()->info().file_id ==
          retained_account->token()->info().file_id);
    CHECK(std::ranges::equal(remaining_account->token()->bytes(),
                             retained_account->token()->bytes()));
    CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == model);
    REQUIRE_FALSE(error);
    primary.Unchanged(); unsigned_profile.Unchanged();
  }
}
#endif

#if XE_PLATFORM_LINUX
namespace {
ProfileAccountFieldEdits HostRename(std::string name) {
  ProfileAccountFieldEdits edits;
  edits.gamertag = std::move(name);
  return edits;
}
std::string HostAccountMount(uint64_t xuid) {
  return fmt::format("User_{:016X}:", xuid);
}
void RestartHostAccountRuntime(ProfileAbiRuntime& runtime) {
  runtime.emulator.reset();
  REQUIRE(KernelState::shared() == nullptr);
  runtime.emulator = std::make_unique<Emulator>("", runtime.root.path,
      runtime.root.path / "content", runtime.root.path / "cache");
  REQUIRE(runtime.emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
}
}
TEST_CASE("Host Account edits publish one durable local field transaction and survive restart without Live provisioning",
          "[kernel][profile_account][host_edit][persistence][restart]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe000000000000401ull;
  X_XAMACCOUNTINFO account{};
  account.ToggleLiveFlag(true);
  account.network_id = 0x87654321;
  account.SetCountry(XOnlineCountry(0));
  account.SetLanguage(XLanguage(0));
  REQUIRE(uint64_t(account.xuid_online) == 0);
  const auto before = SeedOwnedAccount(runtime, uid, account);
  const auto save = runtime.root.path / "content" / fmt::format("{:016X}", uid) /
      "1234ABCD" / "00000001" / "existing-save";
  const std::vector<uint8_t> save_bytes{0x91, 0, 0xff, 0x32};
  REQUIRE(std::filesystem::create_directories(save.parent_path()));
  ReadFixtureWrite(save, save_bytes);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid));
  std::error_code error;
  auto old_inputs = pm->PrepareNativeLogonAccounts(error);
  REQUIRE(old_inputs); REQUIRE(old_inputs->OwnershipCurrent());
  auto edits = HostRename("A");
  edits.country = XOnlineCountry(1); edits.language = XLanguage(1);
  auto pending = pm->PrepareAccountEdit(uid, edits, error);
  REQUIRE(pending); REQUIRE_FALSE(error);
  CHECK(pending->result().phase == ProfileAccountEditPhase::kPrepared);
  CHECK_FALSE(pending->result().published);
  CHECK(OwnedAccountDisk(runtime, uid) == before);
  const auto result = pending->Commit();
  REQUIRE(result.committed());
  CHECK(result.published); CHECK(result.durable); CHECK(result.cache_published);
  CHECK_FALSE(result.unchanged); CHECK_FALSE(result.mount_release_pending);
  CHECK_FALSE(runtime.emulator->file_system()->AcquireDevice(HostAccountMount(uid)));
  CHECK_FALSE(old_inputs->OwnershipCurrent());
  const auto metadata = pm->CaptureCachedAccount(uid, error);
  REQUIRE(metadata); REQUIRE_FALSE(error);
  const auto& actual = metadata->account();
  CHECK(actual.GetGamertagString() == "A");
  const auto* name_bytes = reinterpret_cast<const uint8_t*>(actual.gamertag);
  CHECK(name_bytes[0] == 0); CHECK(name_bytes[1] == 'A');
  CHECK(std::all_of(name_bytes + 2, name_bytes + sizeof(actual.gamertag),
                    [](uint8_t byte) { return byte == 0; }));
  X_XAMACCOUNTINFO expected;
  REQUIRE_FALSE(edits.Apply(account, expected));
  CHECK(std::memcmp(&actual, &expected, sizeof(expected)) == 0);
  CHECK(actual.IsLiveEnabled() == account.IsLiveEnabled());
  CHECK(actual.GetSubscriptionTier() == account.GetSubscriptionTier());
  CHECK(uint64_t(actual.xuid_online) == 0);  // No implicit SetLiveDefaults.
  const auto persisted = OwnedAccountDisk(runtime, uid);
  REQUIRE(persisted.size() == before.size());
  CHECK(std::equal(before.begin() + ProfileAccountSnapshot::kPrefixBytes,
      before.end(), persisted.begin() + ProfileAccountSnapshot::kPrefixBytes));
  REQUIRE_FALSE(result.recovery.empty());
  const auto recovery_root = runtime.emulator->kernel_state()->content_manager()->
      Volume("hdd")->recovery()->root()->path();
  CHECK(ReadFixtureDisk(recovery_root / result.recovery) == before);
  CHECK(ReadFixtureDisk(save) == save_bytes);

  const auto stamp = pm->PrepareNativeLogonAccounts(error);
  REQUIRE(stamp);
  const auto modified = std::filesystem::last_write_time(runtime.root.path / "content" /
      ProfileStorageLease::ProfileRelative(uid) / "Account");
  auto unchanged = pm->PrepareAccountEdit(uid, {}, error);
  REQUIRE(unchanged); REQUIRE_FALSE(error);
  const auto no_op = unchanged->Commit();
  REQUIRE(no_op.committed()); CHECK(no_op.unchanged);
  CHECK_FALSE(no_op.published); CHECK_FALSE(no_op.cache_published);
  CHECK(stamp->OwnershipCurrent());
  CHECK(OwnedAccountDisk(runtime, uid) == persisted);
  CHECK(std::filesystem::last_write_time(runtime.root.path / "content" /
      ProfileStorageLease::ProfileRelative(uid) / "Account") == modified);
  RestartHostAccountRuntime(runtime);  // Escaped handles retain values only.
  pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid));
  const auto restarted = pm->CaptureCachedAccount(uid, error);
  REQUIRE(restarted);
  CHECK(std::memcmp(&restarted->account(), &expected, sizeof(expected)) == 0);
  CHECK(OwnedAccountDisk(runtime, uid) == persisted);
  CHECK(ReadFixtureDisk(save) == save_bytes);
  CHECK(pending->Commit().error == std::errc::operation_canceled);
  CHECK(pending->result().phase == ProfileAccountEditPhase::kCommitted);
}
TEST_CASE("Two prepared host Account edits cannot overwrite a concurrent name or opaque Account snapshot",
          "[kernel][profile_account][host_edit][stale][transaction]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe000000000000402ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, uid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid));
  std::error_code error;
  const auto form = ProfileAccountFieldValues::From(account);
  auto first_edits = HostRename("First"); first_edits.country = XOnlineCountry(7);
  auto first = pm->PrepareAccountEdit(uid, first_edits, error); REQUIRE(first);
  auto stale = pm->PrepareAccountEdit(uid, HostRename("Second"), error); REQUIRE(stale);
  REQUIRE(first->Commit().committed());
  const auto committed = OwnedAccountDisk(runtime, uid);
  REQUIRE(committed != before);
  const auto refused = stale->Commit();
  CHECK_FALSE(refused.committed()); REQUIRE(refused.error);
  CHECK_FALSE(refused.published); CHECK_FALSE(refused.durable);
  CHECK(OwnedAccountDisk(runtime, uid) == committed);
  const auto refused_again = stale->Commit();
  CHECK_FALSE(refused_again.committed()); CHECK_FALSE(refused_again.published);
  if (!refused.stage.empty() || !refused.recovery.empty()) {
    REQUIRE_FALSE(refused_again.prior_artifacts.empty());
    CHECK(refused_again.prior_artifacts.front().stage == refused.stage);
    CHECK(refused_again.prior_artifacts.front().recovery == refused.recovery);
  }
  CHECK(OwnedAccountDisk(runtime, uid) == committed);
  REQUIRE(pm->CaptureCachedAccount(uid, error));
  CHECK(pm->CaptureCachedAccount(uid, error)->account().GetGamertagString() == "First");
  CHECK(pm->CaptureCachedAccount(uid, error)->account().GetCountry() == XOnlineCountry(7));
  REQUIRE(stale->Cancel().phase == ProfileAccountEditPhase::kCanceled);
  // A NEW explicit retry captures current storage but carries only fields the
  // old form actually edited; its untouched country must stay at First's7.
  auto desired = form; desired.gamertag = "Second";
  auto delta = ProfileAccountFieldEdits::Between(form, desired);
  REQUIRE(delta.gamertag); REQUIRE_FALSE(delta.country); REQUIRE_FALSE(delta.language);
  auto retry = pm->PrepareAccountEdit(uid, std::move(delta), error); REQUIRE(retry);
  REQUIRE(retry->Commit().committed());
  const auto value = pm->CaptureCachedAccount(uid, error); REQUIRE(value);
  CHECK(value->account().GetGamertagString() == "Second");
  CHECK(value->account().GetCountry() == XOnlineCountry(7));
  CHECK(value->account().passport_password[7] == account.passport_password[7]);
  const auto final = OwnedAccountDisk(runtime, uid);
  CHECK(std::equal(before.end() - 4, before.end(), final.end() - 4));
}
TEST_CASE("Invalid or canceled host Account edits leave disk and cache untouched and dropped handles release their issued mount",
          "[kernel][profile_account][host_edit][cancel][invalid]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe000000000000403ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, uid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid));
  std::error_code error;
  const auto metadata = pm->CaptureCachedAccount(uid, error); REQUIRE(metadata);
  const auto stamp = pm->PrepareNativeLogonAccounts(error); REQUIRE(stamp);
  for (const auto& name : {std::string(), std::string(16, 'A'), std::string("1Bad"),
                          std::string("Bad\0Name", 8), std::string("Bad!")}) {
    CHECK_FALSE(pm->PrepareAccountEdit(uid, HostRename(name), error));
    CHECK(error == std::errc::invalid_argument);
    CHECK_FALSE(runtime.emulator->file_system()->AcquireDevice(HostAccountMount(uid)));
  }
  auto pending = pm->PrepareAccountEdit(uid, HostRename("Canceled"), error); REQUIRE(pending);
  CHECK(pending->result().mount_release_pending);
  const auto canceled = pending->Cancel();
  CHECK(canceled.phase == ProfileAccountEditPhase::kCanceled);
  CHECK_FALSE(canceled.error); CHECK_FALSE(canceled.published);
  CHECK_FALSE(canceled.mount_release_pending);
  CHECK_FALSE(pending->Commit().committed());
  CHECK(pending->Commit().phase == ProfileAccountEditPhase::kCanceled);
  pending = pm->PrepareAccountEdit(uid, HostRename("Dropped"), error); REQUIRE(pending);
  pending.reset();  // Actual public handle destructor, not a helper-only cancel.
  CHECK_FALSE(runtime.emulator->file_system()->AcquireDevice(HostAccountMount(uid)));
  REQUIRE(pm->MountProfile(uid)); REQUIRE(pm->DismountProfile(uid));
  CHECK(pm->CaptureCachedAccount(uid, error) == metadata);
  CHECK(stamp->OwnershipCurrent());
  CHECK(OwnedAccountDisk(runtime, uid) == before);
}
TEST_CASE("Host Account edit cleanup cannot retire another prepared writer managed file or roster pause",
          "[kernel][profile_account][host_edit][rundown][ownership]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe000000000000404ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, uid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid));
  std::error_code error;
  auto edit = pm->PrepareAccountEdit(uid, HostRename("Never Saved"), error); REQUIRE(edit);
  const auto snapshot = pm->CaptureDurableAccount(uid, error); REQUIRE(snapshot);
  const auto identity = snapshot->identity();
  auto* fs = runtime.emulator->file_system();
  const auto actual_device = fs->AcquireDevice(HostAccountMount(uid)); REQUIRE(actual_device);
  SECTION("a genuine staged writer keeps its cancellation state and can commit") {
    auto next = account;
    string_util::copy_and_swap_truncating(next.gamertag, u"Other Writer", xe::countof(next.gamertag));
    auto wire = before; ProfileManager::EncryptAccountFile(&next, wire.data());
    ProfileAccountWriter writer(identity, runtime.emulator->kernel_state()->content_manager()->
        Volume("hdd")->recovery());
    auto prepared = writer.PrepareRaw(snapshot->token(), wire, error); REQUIRE(prepared);
    REQUIRE_FALSE(identity->writes_drained());
    const auto canceled = edit->Cancel();
    CHECK(canceled.phase == ProfileAccountEditPhase::kCanceled);
    CHECK(canceled.mount_release_pending); CHECK_FALSE(identity->retired());
    CHECK(fs->AcquireDevice(HostAccountMount(uid)) == actual_device);
    const auto actual = writer.Commit(*prepared);
    REQUIRE(actual.durable); REQUIRE_FALSE(actual.error);
    CHECK(OwnedAccountDisk(runtime, uid) == wire);
    prepared.reset(); REQUIRE(identity->writes_drained());
  }
  SECTION("an actual managed guest Account file still publishes through the cache lane") {
    vfs::File* raw = nullptr; vfs::FileAction action{};
    REQUIRE(fs->OpenFile(nullptr, HostAccountMount(uid) + "\\Account",
        vfs::FileDisposition::kOverwrite, vfs::FileAccess::kGenericRead |
        vfs::FileAccess::kGenericWrite, false, true, &raw, &action) == X_STATUS_SUCCESS);
    auto destroy = [](vfs::File* file) { if (file) { file->Destroy(); } };
    std::unique_ptr<vfs::File, decltype(destroy)> file(raw, destroy); REQUIRE(file);
    const auto canceled = edit->Cancel();
    CHECK(canceled.mount_release_pending); CHECK_FALSE(identity->retired());
    auto next = account;
    string_util::copy_and_swap_truncating(next.gamertag, u"Guest Writer", xe::countof(next.gamertag));
    auto wire = before; ProfileManager::EncryptAccountFile(&next, wire.data());
    size_t count = 0;
    REQUIRE(file->WriteSync(wire, 0, &count) == X_STATUS_SUCCESS);
    CHECK(count == wire.size()); CHECK(OwnedAccountDisk(runtime, uid) == wire);
    const auto cached = pm->CaptureCachedAccount(uid, error); REQUIRE(cached);
    CHECK(cached->account().GetGamertagString() == "Guest Writer");
    file.reset();
  }
  SECTION("a real roster pause is not changed into retirement by cancel") {
    auto paused = identity->PauseForRoster(error); REQUIRE(paused);
    const auto serial = identity->write_serial();
    const auto canceled = edit->Cancel();
    CHECK(canceled.mount_release_pending); CHECK_FALSE(identity->retired());
    CHECK_FALSE(identity->EnterWrite(error));
    CHECK(error == std::errc::device_or_resource_busy);
    CHECK(identity->write_serial() == serial);
    CHECK(OwnedAccountDisk(runtime, uid) == before);
    paused.reset();
  }
  const auto settled = edit->Cancel();
  CHECK(settled.phase == ProfileAccountEditPhase::kCanceled);
  CHECK_FALSE(settled.mount_release_pending);
  CHECK(identity->retired());
  CHECK_FALSE(fs->AcquireDevice(HostAccountMount(uid)));
}
TEST_CASE("Host Account edit release never decrements a replacement mount and escaped handles cannot borrow a stopped manager",
          "[kernel][profile_account][host_edit][generation][lifetime]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe000000000000405ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, uid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid));
  std::error_code error;
  auto old = pm->PrepareAccountEdit(uid, HostRename("Old Request"), error); REQUIRE(old);
  SECTION("actual old namespace closure followed by a replacement generation") {
    const auto old_device = runtime.emulator->file_system()->AcquireDevice(HostAccountMount(uid));
    REQUIRE(old_device);
    REQUIRE(pm->DismountProfile(uid));  // Actual namespace/view retirement.
    REQUIRE(old_device->namespace_retiring());
    REQUIRE(pm->MountProfile(uid));
    const auto replacement = runtime.emulator->file_system()->AcquireDevice(HostAccountMount(uid));
    REQUIRE(replacement); REQUIRE(replacement != old_device);
    const auto released = old->Cancel();
    CHECK(released.phase == ProfileAccountEditPhase::kCanceled);
    CHECK_FALSE(released.mount_release_pending);
    CHECK(runtime.emulator->file_system()->AcquireDevice(HostAccountMount(uid)) == replacement);
    CHECK(OwnedAccountDisk(runtime, uid) == before);
    REQUIRE(pm->DismountProfile(uid));
  }
  SECTION("external VFS closure settles the same old PM record before remount") {
    auto* fs = runtime.emulator->file_system();
    const auto old_device = fs->AcquireDevice(HostAccountMount(uid)); REQUIRE(old_device);
    REQUIRE(fs->UnregisterDevice(HostAccountMount(uid)));
    REQUIRE(old_device->namespace_retiring());
    CHECK_FALSE(fs->AcquireDevice(HostAccountMount(uid)));
    const auto released = old->Cancel();
    CHECK(released.phase == ProfileAccountEditPhase::kCanceled);
    CHECK_FALSE(released.mount_release_pending);
    REQUIRE(pm->MountProfile(uid));
    const auto replacement = fs->AcquireDevice(HostAccountMount(uid)); REQUIRE(replacement);
    CHECK(replacement != old_device);
    CHECK(OwnedAccountDisk(runtime, uid) == before);
    CHECK_FALSE(old->Cancel().mount_release_pending);
    CHECK(fs->AcquireDevice(HostAccountMount(uid)) == replacement);
    REQUIRE(pm->DismountProfile(uid));
  }
  SECTION("real manager stop revokes native owners retained by a live UI handle") {
    RestartHostAccountRuntime(runtime);
    const auto retired = old->result();
    CHECK(retired.phase == ProfileAccountEditPhase::kRetired);
    CHECK(retired.error == std::errc::operation_canceled);
    CHECK_FALSE(retired.published); CHECK_FALSE(retired.mount_release_pending);
    CHECK(old->Commit().error == std::errc::operation_canceled);
    CHECK(old->Cancel().error == std::errc::operation_canceled);
    pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
    REQUIRE(pm->LoadAccount(uid));
    auto fresh = pm->PrepareAccountEdit(uid, HostRename("New Owner"), error); REQUIRE(fresh);
    REQUIRE(fresh->Commit().committed());
    const auto actual = OwnedAccountDisk(runtime, uid);
    CHECK(actual != before);
    CHECK(old->Cancel().error == std::errc::operation_canceled);
    CHECK(OwnedAccountDisk(runtime, uid) == actual);
  }
}
TEST_CASE("Host Account edits reject actual source retirement and cannot revive it during cleanup",
          "[kernel][profile_account][host_edit][source][retirement]") {
  ProfileAbiRuntime runtime;
  const auto source = RegisterDiscoverySource(runtime);
  constexpr uint64_t uid = 0xe000000000000406ull;
  const auto before = SeedDiscoveryAccount(source.storage->path(), uid, u"Removed Source");
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  pm->ReloadProfiles();
  std::error_code error;
  const auto metadata = pm->CaptureCachedAccount(uid, error); REQUIRE(metadata);
  REQUIRE(metadata->source_id() == source.source.id);
  auto pending = pm->PrepareAccountEdit(uid, HostRename("Cannot Save"), error); REQUIRE(pending);
  source.storage->Retire();
  const auto refused = pending->Commit();
  CHECK(refused.phase == ProfileAccountEditPhase::kRetired);
  CHECK(refused.error == std::errc::operation_canceled);
  CHECK_FALSE(refused.published); CHECK_FALSE(refused.committed());
  CHECK(source.storage->retiring());
  CHECK(pm->CaptureCachedAccount(uid, error) == metadata);
  CHECK(ReadFixtureDisk(source.storage->path() /
      ProfileStorageLease::ProfileRelative(uid) / "Account") == before);
  CHECK_FALSE(pending->Commit().committed());
}
TEST_CASE("Host Account rename updates the real signed-in projection without changing roster identity GPDs or avatar appearance",
          "[kernel][profile_account][host_edit][roster][save_isolation]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime, 0xe000000000000407ull, 37);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto before = pm.CaptureRosterExpected(error); REQUIRE(before.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({profile.xuid, 0, 0, 0}), before).error);
  before = pm.CaptureRosterExpected(error); REQUIRE(before.roster);
  const auto model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(model);
  const auto origin = model->CaptureOrigin(error); REQUIRE(origin);
  const auto inputs = pm.PrepareNativeLogonAccounts(error); REQUIRE(inputs);
  const auto identity = inputs->accounts()[0]->identity();
  auto pending = pm.PrepareAccountEdit(profile.xuid, HostRename("A"), error); REQUIRE(pending);
  REQUIRE(pending->Commit().committed());
  CHECK(pm.CaptureProfile(uint8_t(0))->name() == "A");
  const auto after = pm.CaptureRosterExpected(error); REQUIRE(after.roster);
  CHECK(after.roster->epoch() == before.roster->epoch());
  CHECK(after.roster->slots() == before.roster->slots());
  CHECK(model->IsCurrent(*origin)); CHECK_FALSE(identity->retired());
  CHECK_FALSE(inputs->OwnershipCurrent());
  CHECK(ReadFixtureDisk(profile.path / "FFFE07D1.gpd") == profile.dashboard);
  CHECK(ReadFixtureDisk(profile.path / "1234ABCD.gpd") == profile.title);
  auto appearance = pm.CaptureProfileSettingsRead(uint8_t(0), kReadTitle, error); REQUIRE(appearance);
  const auto setting = appearance->users().front().Setting(kReadTitle, kReadAppearance, error);
  REQUIRE(setting);
  CHECK(std::ranges::equal(setting->get_extended_data(), profile.appearance));
}
#endif

#if XE_PLATFORM_LINUX
TEST_CASE("A physically published host Account edit retains its exact receipt through cache busy cancel drop and owner stop",
          "[kernel][profile_account][host_edit][cache_retry][publication]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid = 0xe000000000000408ull;
  X_XAMACCOUNTINFO account{};
  const auto before = SeedOwnedAccount(runtime, uid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  REQUIRE(pm->LoadAccount(uid));
  std::error_code error;
  const auto original = pm->CaptureCachedAccount(uid, error); REQUIRE(original);
  auto pending = pm->PrepareAccountEdit(uid, HostRename("Cache Pending"), error); REQUIRE(pending);
  ProfileAccountEditResult physical;
  {
    HeldRosterMetadata held(*pm);  // Same real metadata mutex as cache admission.
    REQUIRE(held.ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    physical = pending->Commit();
    CHECK(physical.phase == ProfileAccountEditPhase::kDurablePendingCache);
    CHECK(physical.published); CHECK(physical.durable); CHECK_FALSE(physical.cache_published);
    CHECK(physical.error == std::errc::device_or_resource_busy);
    const auto refused = pending->Cancel();
    CHECK(refused.error == std::errc::operation_in_progress);
    CHECK(refused.published); CHECK(refused.durable);
    CHECK(refused.phase == ProfileAccountEditPhase::kDurablePendingCache);
    CHECK(refused.mount_release_pending);
  }  // Always releases and joins before real fixture/PM destruction.
  const auto published = OwnedAccountDisk(runtime, uid);
  REQUIRE(published != before);
  CHECK(pm->CaptureCachedAccount(uid, error) == original);
  SECTION("retry publishes only the original durable snapshot into cache") {
    const auto result = pending->Commit();
    REQUIRE(result.committed()); CHECK(result.cache_published);
    CHECK(result.stage == physical.stage); CHECK(result.recovery == physical.recovery);
    CHECK(OwnedAccountDisk(runtime, uid) == published);
    const auto value = pm->CaptureCachedAccount(uid, error); REQUIRE(value);
    CHECK(value->account().GetGamertagString() == "Cache Pending");
  }
  SECTION("dropping the UI leaves the real pending receipt in the manager registry") {
    pending.reset();
    REQUIRE(pm->MountProfile(uid));  // Reaps abandoned actual cache publication.
    const auto value = pm->CaptureCachedAccount(uid, error); REQUIRE(value);
    CHECK(value->account().GetGamertagString() == "Cache Pending");
    CHECK(OwnedAccountDisk(runtime, uid) == published);
    REQUIRE(pm->DismountProfile(uid));
  }
  SECTION("actual owner stop preserves physical outcome without pinning an old native source") {
    RestartHostAccountRuntime(runtime);
    const auto result = pending->result();
    CHECK(result.phase == ProfileAccountEditPhase::kRetired);
    CHECK(result.published); CHECK(result.durable); CHECK_FALSE(result.cache_published);
    CHECK(result.error == std::errc::operation_canceled);
    CHECK(result.stage == physical.stage); CHECK(result.recovery == physical.recovery);
    CHECK_FALSE(result.mount_release_pending);
    CHECK_FALSE(pending->Commit().committed());
    pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
    REQUIRE(pm->LoadAccount(uid));
    const auto value = pm->CaptureCachedAccount(uid, error); REQUIRE(value);
    CHECK(value->account().GetGamertagString() == "Cache Pending");
    CHECK(OwnedAccountDisk(runtime, uid) == published);
  }
}
TEST_CASE("Native protocol commits selected maps without claiming image joins or old-owner retirement",
          "[kernel][profile][roster][native][protocol][retirement]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe000000000000151ull, 51);
  ReadFixtureProfile b(runtime, 0xe000000000000152ull, 52);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid, 0, 0, 0}), expected).error);
  auto old_model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(old_model);
  const auto old_origin = old_model->CaptureOrigin(error); REQUIRE(old_origin);
  auto old_writer = pm.CaptureGpdWriter(a.xuid, error); REQUIRE(old_writer.model == old_model);
  // Capture a genuine command before PrepareRoster pauses this old writer.
  ProfileGpdMutation refused; refused.kind = ProfileGpdMutationKind::kSetSetting;
  refused.title_id = kDashboardID; refused.request_id = old_model->AllocateRequestId(error);
  REQUIRE(refused.request_id); refused.setting.emplace(static_cast<UserSettingId>(kReadScalar), int32_t(99));
  REQUIRE_FALSE(error);
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto intent = pm.PrepareRoster(RosterDesired({b.xuid, 0, 0, 0}), expected, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  auto old_ticket = ProfileSettingsReadTestAccess::Ticket(*intent, *binding, error); REQUIRE(old_ticket);
  REQUIRE(ProfileSettingsReadTestAccess::Scope(old_ticket, binding));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Protocol(intent, binding));
  auto cell = ProfileSettingsReadTestAccess::EnterNative(pm, intent, binding, error); REQUIRE(cell);
  CHECK_FALSE(pm.CaptureProfile(uint8_t(0)));
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  CHECK_FALSE(pm.CaptureProfile(uint8_t(0)));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Joined(intent, binding));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Protocol(intent, binding, 0, true));
  auto protocol = ProfileSettingsReadTestAccess::Protocol(intent, binding, 0x80000001u); REQUIRE(protocol);
  CHECK(ProfileSettingsReadTestAccess::Finalize(pm, intent, *protocol).error);
  const auto committed = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
  REQUIRE_FALSE(committed.error);
  CHECK(committed.phase == ProfileRosterPhase::kPublished);
  CHECK(committed.native_protocol_returned); CHECK(committed.native_protocol_value == 0x80000001u);
  CHECK(committed.selected_reads_available); CHECK(committed.maps_committed); CHECK(committed.writers_available);
  CHECK_FALSE(committed.owners_retired);
  CheckRoster(pm, {b.xuid, 0, 0, 0});
  auto new_model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(new_model);
  CHECK(new_model != old_model); CHECK(new_model->xuid() == b.xuid);
  CHECK(pm.CaptureGpdWriter(b.xuid, error).model == new_model);
  CHECK_FALSE(ProfileSettingsReadTestAccess::CaptureIdentityForRebindProbe(pm, a.xuid, error)); REQUIRE(error);
  CHECK(old_model->IsCurrent(*old_origin));
  CHECK_FALSE(old_model->AllocateRequestId(error));
  CHECK(error == std::errc::device_or_resource_busy);
  const auto denied = old_writer.Submit(refused);
  REQUIRE(denied.error); CHECK(denied.error == std::errc::device_or_resource_busy);
  REQUIRE(ProfileSettingsReadTestAccess::Scope(old_ticket, binding));
  CHECK_FALSE(pm.CaptureRosterExpected(error).roster); REQUIRE(error);
  auto joined = ProfileSettingsReadTestAccess::Joined(intent, binding); REQUIRE(joined);
  CHECK(ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *joined).error);
  const auto finished = ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined);
  REQUIRE_FALSE(finished.error); CHECK(finished.phase == ProfileRosterPhase::kCommitted);
  CHECK(finished.owners_retired); CHECK_FALSE(old_model->IsCurrent(*old_origin));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Scope(old_ticket, binding));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Protocol(intent, binding));
  CHECK(ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol).error);
  // The actual notification EventLifetime may retain this intent after
  // producer Finish. It owns metadata only, not Operation's write pauses.
  const std::shared_ptr<const void> passive_event_intent=intent;
  REQUIRE(passive_event_intent);REQUIRE(cell);REQUIRE(protocol);REQUIRE(joined);
  const auto selected_account=ProfileSettingsReadTestAccess::SelectedAccount(pm,b.xuid);REQUIRE(selected_account);
  auto account_write=selected_account->identity()->EnterWrite(error);
  REQUIRE(account_write);REQUIRE_FALSE(error);account_write.reset();
  auto selected_writer=pm.CaptureGpdWriter(b.xuid,error);REQUIRE(selected_writer.model==new_model);
  ProfileGpdMutation ordinary;ordinary.kind=ProfileGpdMutationKind::kSetSetting;
  ordinary.title_id=kDashboardID;ordinary.request_id=new_model->AllocateRequestId(error);
  REQUIRE(ordinary.request_id);REQUIRE_FALSE(error);
  ordinary.setting.emplace(static_cast<UserSettingId>(kReadScalar),int32_t(101));
  const auto written=selected_writer.Submit(ordinary);
  REQUIRE_FALSE(written.error);CHECK(written.state==ProfileGpdTransactionAttempt::State::kCommitted);
  const auto selected_read=pm.CaptureGpdRead(uint8_t(0),error);REQUIRE(selected_read);
  const auto setting=selected_read->Setting(kDashboardID,kReadScalar,error);REQUIRE(setting);REQUIRE_FALSE(error);
  CHECK(std::get<int32_t>(setting->get_host_data())==101);
  // A subsequent genuine preparation/cancellation releases its own pauses
  // even while the earlier committed intent and old closed ticket remain held.
  auto after=pm.CaptureRosterExpected(error);REQUIRE(after.roster);REQUIRE_FALSE(error);
  const auto canceled=pm.PrepareRoster(RosterDesired({b.xuid,0,0,0}),after,error);REQUIRE(canceled);
  REQUIRE(pm.CancelRoster(canceled).phase==ProfileRosterPhase::kCanceled);
  account_write=selected_account->identity()->EnterWrite(error);REQUIRE(account_write);REQUIRE_FALSE(error);account_write.reset();
  CHECK_FALSE(ProfileSettingsReadTestAccess::Scope(old_ticket,binding));
  CHECK_FALSE(old_model->AllocateRequestId(error));REQUIRE(error);
  CheckRoster(pm, {b.xuid, 0, 0, 0});a.Unchanged();
  CHECK(ReadFixtureDisk(b.path/"Account")==b.account);
  CHECK(ReadFixtureDisk(b.path/"1234ABCD.gpd")==b.title);
  CHECK(ReadFixtureDisk(b.path/"FFFE07D1.gpd")!=b.dashboard);
}
TEST_CASE("Empty native alignment requires actual unchanged row owners and protocol map writer commitment",
          "[kernel][profile][roster][native][empty_initial_frame]") {
  ProfileAbiRuntime runtime;
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster); REQUIRE_FALSE(error);
  for (const auto& slot : expected.roster->slots()) { REQUIRE(slot == ProfileRosterSlot{}); }
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto desired = RosterDesired({0, 0, 0, 0});
  desired.notify_host_compatibility = false; desired.remember_slots = false;
  auto intent = pm.PrepareRoster(desired, expected, error); REQUIRE(intent); REQUIRE_FALSE(error);
  CHECK(intent->old_roster() == expected.roster);
  CHECK(intent->prepared_roster()->epoch() == expected.roster->epoch());
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent));
  auto cell = ProfileSettingsReadTestAccess::EnterNative(pm, intent, binding, error);
  REQUIRE(cell); REQUIRE_FALSE(error);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent));
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  // Original cache publication alone is neither protocol nor host-map completion.
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent));
  auto protocol = ProfileSettingsReadTestAccess::Protocol(intent, binding, 0); REQUIRE(protocol);
  const auto committed = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
  REQUIRE_FALSE(committed.error); REQUIRE(committed.phase == ProfileRosterPhase::kPublished);
  CHECK(committed.native_protocol_returned); CHECK(committed.selected_reads_available);
  CHECK(committed.maps_committed); CHECK(committed.writers_available); CHECK_FALSE(committed.owners_retired);
  for (const auto changed : committed.changed) { CHECK_FALSE(changed); }
  CHECK(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent, 1));
  // This metadata fact has no frame/GPU/pixel authority. Genuine finalization
  // still needs the separate existing joined role even for an empty roster.
  CHECK(ProfileSettingsReadTestAccess::Finalize(pm, intent, *protocol).error);
  auto joined = ProfileSettingsReadTestAccess::Joined(intent, binding); REQUIRE(joined);
  const auto finished = ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined);
  REQUIRE_FALSE(finished.error); REQUIRE(finished.phase == ProfileRosterPhase::kCommitted);
  CHECK(finished.owners_retired); CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent));
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster); REQUIRE_FALSE(error);
  auto canceled = pm.PrepareRoster(desired, expected, error); REQUIRE(canceled); REQUIRE_FALSE(error);
  REQUIRE(pm.CancelRoster(canceled).phase == ProfileRosterPhase::kCanceled);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(canceled));
}
TEST_CASE("Published nonempty no-op and real signout never become empty initial alignment",
          "[kernel][profile][roster][native][empty_initial_frame][refusal]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime, 0xe000000000000551ull, 71);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster); REQUIRE_FALSE(error);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({profile.xuid, 0, 0, 0}), expected).error);
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster); REQUIRE_FALSE(error);
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto desired = RosterDesired({profile.xuid, 0, 0, 0});
  bool signout = false;
  SECTION("an unchanged nonempty roster retains its actual profile image duties") {}
  SECTION("empty after a real signout is not the initial empty before tuple") {
    desired = RosterDesired({0, 0, 0, 0}); signout = true;
  }
  auto intent = pm.PrepareRoster(desired, expected, error); REQUIRE(intent); REQUIRE_FALSE(error);
  REQUIRE(intent->old_roster()->slots()[0].xuid == profile.xuid);
  CHECK(intent->result().changed[0] == signout);
  auto cell = ProfileSettingsReadTestAccess::EnterNative(pm, intent, binding, error);
  REQUIRE(cell); REQUIRE_FALSE(error); REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  auto protocol = ProfileSettingsReadTestAccess::Protocol(intent, binding, 0); REQUIRE(protocol);
  const auto committed = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
  REQUIRE_FALSE(committed.error); REQUIRE(committed.phase == ProfileRosterPhase::kPublished);
  REQUIRE(committed.maps_committed); REQUIRE(committed.writers_available);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent));
  auto joined = ProfileSettingsReadTestAccess::Joined(intent, binding); REQUIRE(joined);
  const auto finished = ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined);
  REQUIRE_FALSE(finished.error); REQUIRE(finished.phase == ProfileRosterPhase::kCommitted);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent)); profile.Unchanged();
}
TEST_CASE("An occupied initial alignment retains exact row owners only after real publication and protocol duties",
          "[kernel][profile][roster][native][unchanged_initial_frame]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe000000000000561ull, 81);
  ReadFixtureProfile b(runtime, 0xe000000000000562ull, 82);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected=pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto desired=RosterDesired({0,a.xuid,0,b.xuid});
  desired.notify_host_compatibility=false;desired.remember_slots=false;
  REQUIRE_FALSE(pm.AdoptRoster(desired,expected).error);
  expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  const auto profile=pm.CaptureProfile(uint8_t(1));REQUIRE(profile);
  const auto model=pm.AcquireProfileGpdModel(uint8_t(1),error);REQUIRE(model);
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto canceled=pm.PrepareRoster(desired,expected,error);REQUIRE(canceled);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(canceled));
  REQUIRE(pm.CancelRoster(canceled).phase==ProfileRosterPhase::kCanceled);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(canceled));
  auto intent=pm.PrepareRoster(desired,expected,error);REQUIRE(intent);REQUIRE_FALSE(error);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,intent,binding,error);REQUIRE(cell);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  auto protocol=ProfileSettingsReadTestAccess::Protocol(intent,binding,0);REQUIRE(protocol);
  const auto committed=ProfileSettingsReadTestAccess::CommitProtocol(pm,intent,*protocol);
  REQUIRE_FALSE(committed.error);REQUIRE(committed.phase==ProfileRosterPhase::kPublished);
  CHECK(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent,1));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent));
  CHECK(pm.CaptureProfile(uint8_t(1))==profile);
  CHECK(pm.AcquireProfileGpdModel(uint8_t(1),error)==model);
  CHECK(ProfileSettingsReadTestAccess::Finalize(pm,intent,*protocol).error);
  auto joined=ProfileSettingsReadTestAccess::Joined(intent,binding);REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm,intent,*joined).error);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(canceled));
  CheckRoster(pm,desired.xuids);a.Unchanged();b.Unchanged();
}
TEST_CASE("Initial unchanged-owner evidence refuses real replacement signout slot movement and canceled publication",
          "[kernel][profile][roster][native][unchanged_initial_frame][refusal]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime,0xe000000000000563ull,83);
  ReadFixtureProfile b(runtime,0xe000000000000564ull,84);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({0,a.xuid,0,0}),expected).error);
  expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  auto desired=RosterDesired({0,a.xuid,0,0});bool cancel=false;
  SECTION("replacement") {desired=RosterDesired({0,b.xuid,0,0});}
  SECTION("signout") {desired=RosterDesired({0,0,0,0});}
  SECTION("same identity moving to another slot") {desired=RosterDesired({a.xuid,0,0,0});}
  SECTION("canceled-empty publication with unchanged requested rows") {cancel=true;}
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto intent=pm.PrepareRoster(desired,expected,error);REQUIRE(intent);
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,intent,binding,error);REQUIRE(cell);
  const auto outcome=cancel?ProfileRosterPublicationOutcome::kCanceledEmpty:
                            ProfileRosterPublicationOutcome::kRequested;
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell,outcome));
  auto protocol=ProfileSettingsReadTestAccess::Protocol(intent,binding,cancel?0x800704c7u:0);REQUIRE(protocol);
  const auto committed=ProfileSettingsReadTestAccess::CommitProtocol(pm,intent,*protocol);
  REQUIRE_FALSE(committed.error);REQUIRE(committed.phase==ProfileRosterPhase::kPublished);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  auto joined=ProfileSettingsReadTestAccess::Joined(intent,binding);REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm,intent,*joined).error);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));a.Unchanged();b.Unchanged();
}
TEST_CASE("Initial unchanged-owner evidence compares the complete current durable Account snapshot",
          "[kernel][profile][roster][native][unchanged_initial_frame][account_revision]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime,0xe000000000000565ull,85);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  const auto desired=RosterDesired({0,a.xuid,0,0});
  REQUIRE_FALSE(pm.AdoptRoster(desired,expected).error);
  expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  const auto snapshot=ProfileSettingsReadTestAccess::SelectedAccount(pm,a.xuid);REQUIRE(snapshot);
  auto wire=a.account;auto account=snapshot->account();
  SECTION("changed known Account field") {
    string_util::copy_and_swap_truncating(account.gamertag,u"Revised durable Account",xe::countof(account.gamertag));
    ProfileManager::EncryptAccountFile(&account,wire.data());
  }
  SECTION("changed opaque Account field") {
    account.passport_password[7]^=char(0x37);
    ProfileManager::EncryptAccountFile(&account,wire.data());
  }
  SECTION("unknown retained Account trailer") {wire.push_back(0x5a);}
  ProfileAccountWriter writer(snapshot->identity(),runtime.emulator->kernel_state()->content_manager()->Volume("hdd")->recovery());
  auto prepared=writer.PrepareRaw(snapshot->token(),wire,error);REQUIRE(prepared);
  const auto written=writer.Commit(*prepared);REQUIRE(written.durable);REQUIRE_FALSE(written.error);
  prepared.reset();
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto intent=pm.PrepareRoster(desired,expected,error);REQUIRE(intent);REQUIRE_FALSE(error);
  CHECK(intent->prepared_roster()->epoch()==expected.roster->epoch());
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,intent,binding,error);REQUIRE(cell);
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  auto protocol=ProfileSettingsReadTestAccess::Protocol(intent,binding,0);REQUIRE(protocol);
  const auto committed=ProfileSettingsReadTestAccess::CommitProtocol(pm,intent,*protocol);
  REQUIRE_FALSE(committed.error);REQUIRE(committed.phase==ProfileRosterPhase::kPublished);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  auto joined=ProfileSettingsReadTestAccess::Joined(intent,binding);REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm,intent,*joined).error);
  CHECK(ReadFixtureDisk(a.path/"Account")==wire);
  CHECK(ReadFixtureDisk(a.path/"FFFE07D1.gpd")==a.dashboard);
  CHECK(ReadFixtureDisk(a.path/"1234ABCD.gpd")==a.title);
}
TEST_CASE("A genuine pending unchanged model delays host maps without hiding the published read row",
          "[kernel][profile][roster][native][protocol][pending_model]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe000000000000153ull, 53);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  const auto desired = RosterDesired({a.xuid, 0, 0, 0});
  REQUIRE_FALSE(pm.AdoptRoster(desired, expected).error);
  auto writer = pm.CaptureGpdWriter(a.xuid, error); REQUIRE(writer.model);
  const auto before = writer.model->CaptureOrigin(error); REQUIRE(before);
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto intent = pm.PrepareRoster(desired, expected, error); REQUIRE(intent);
  // Canceled-empty can remove this otherwise unchanged row. It is drained and
  // paused before arm, then the real Requested publisher reopens only this
  // known selected model using its exact pause serial without dropping owners.
  CHECK_FALSE(writer.model->AllocateRequestId(error));
  CHECK(error == std::errc::device_or_resource_busy);
  auto cell = ProfileSettingsReadTestAccess::EnterNative(pm, intent, binding, error); REQUIRE(cell);
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  ProfileGpdMutation command; command.kind = ProfileGpdMutationKind::kSetSetting;
  command.title_id = kDashboardID; command.request_id = writer.model->AllocateRequestId(error);
  REQUIRE(command.request_id); command.setting.emplace(static_cast<UserSettingId>(kReadScalar), int32_t(93));
  bool retry = false;
  auto pending = writer.model->Begin(command, retry, error); REQUIRE(pending); REQUIRE_FALSE(error); CHECK_FALSE(retry);
  auto protocol = ProfileSettingsReadTestAccess::Protocol(intent, binding); REQUIRE(protocol);
  const auto busy = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
  CHECK(busy.error == std::errc::device_or_resource_busy); CHECK(busy.selected_reads_available);
  CHECK_FALSE(busy.maps_committed); CHECK_FALSE(busy.writers_available); CHECK_FALSE(busy.owners_retired);
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  CheckRoster(pm, desired.xuids);
  CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == writer.model);
  CHECK_FALSE(pm.CaptureGpdWriter(a.xuid, error).model); REQUIRE(error);
  const auto written = writer.ExecutePending(pending); REQUIRE_FALSE(written.error);
  CHECK(written.state == ProfileGpdTransactionAttempt::State::kCommitted);
  const auto advanced = writer.model->CaptureOrigin(error); REQUIRE(advanced);
  CHECK(advanced->stamp.model_version > before->stamp.model_version);
  const auto committed = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
  REQUIRE_FALSE(committed.error); CHECK(committed.maps_committed); CHECK(committed.writers_available);
  CHECK(committed.phase == ProfileRosterPhase::kPublished); CHECK_FALSE(committed.owners_retired);
  // Shared ordinary model evolution is not an identity-authority replacement.
  CHECK(ProfileSettingsReadTestAccess::PublishedUnchangedOwners(intent));
  REQUIRE(pm.AcquireProfileGpdModel(uint8_t(0), error) == writer.model);
  const auto read = pm.CaptureGpdRead(uint8_t(0), error); REQUIRE(read);
  const auto value = read->Setting(kDashboardID, kReadScalar, error); REQUIRE(value);
  CHECK(std::get<int32_t>(value->get_host_data()) == 93);
  auto joined = ProfileSettingsReadTestAccess::Joined(intent, binding); REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined).error);
  CHECK(ReadFixtureDisk(a.path / "Account") == a.account);
  CHECK(ReadFixtureDisk(a.path / "1234ABCD.gpd") == a.title);
  CHECK(ReadFixtureDisk(a.path / "FFFE07D1.gpd") != a.dashboard);
}
TEST_CASE("Native roster title effects retain a real SPA command through closed-writer retry",
          "[kernel][profile][roster][native][protocol][spa][persistence]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe000000000000158ull, 58);
  ReadFixtureProfile b(runtime, 0xe000000000000159ull, 59);
  auto* xam = runtime.emulator->kernel_state()->xam_state();
  auto& pm = *xam->profile_manager();
  auto* tracker = xam->user_tracker();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid, 0, 0, 0}), expected).error);
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  const auto old_epoch = expected.roster->epoch();
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  uint64_t selected_xuid = b.xuid;
  bool same_row = false;
  SECTION("new selected user receives the real AddUser title effect") {}
  SECTION("initial unchanged alignment repairs the title effect refused while preparing") {
    selected_xuid = a.xuid; same_row = true;
  }
  auto intent = pm.PrepareRoster(RosterDesired({selected_xuid, 0, 0, 0}), expected, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  constexpr uint32_t title = 0x4d5300aa;
  // The real UpdateSpaInfo path runs while the ordinary capture is excluded.
  // It may not bypass the roster gate; this Operation owns its later effect.
  auto spa = InstallRosterSpa(runtime, title); REQUIRE(spa);
  auto cell = ProfileSettingsReadTestAccess::EnterNative(pm, intent, binding, error); REQUIRE(cell);
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  auto protocol = ProfileSettingsReadTestAccess::Protocol(intent, binding); REQUIRE(protocol);
  ProfileRosterResult busy;
  uint64_t next_after_first = 0, next_after_second = 0;
  std::shared_ptr<ProfileGpdModelAuthority> model;
  {
    auto held = xe::global_critical_region::AcquireDirect();
    busy = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
    REQUIRE(busy.error == std::errc::operation_would_block);
    CHECK(busy.maps_committed); CHECK(busy.selected_reads_available);
    CHECK_FALSE(busy.writers_available); CHECK_FALSE(busy.owners_retired);
    model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(model);
    REQUIRE(model->xuid() == selected_xuid);
    // The command's real model-issued ID is retained after preparation. A
    // second busy attempt must not allocate another command/request identity.
    next_after_first = model->AllocateRequestId(error); REQUIRE(next_after_first); REQUIRE_FALSE(error);
    const auto again = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
    REQUIRE(again.error == std::errc::operation_would_block);
    next_after_second = model->AllocateRequestId(error); REQUIRE_FALSE(error);
    CHECK(next_after_second == next_after_first + 1);
    CHECK_FALSE(model->Pending(error)); REQUIRE_FALSE(error);
    CHECK_FALSE(pm.CaptureGpdWriter(selected_xuid, error).model); REQUIRE(error);
  }
  CHECK_FALSE(tracker->AddUser(selected_xuid)); // tracking alone did not settle the effect
  a.Unchanged(); b.Unchanged();
  const auto committed = ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol);
  REQUIRE_FALSE(committed.error); CHECK(committed.maps_committed); CHECK(committed.writers_available);
  CHECK(committed.phase == ProfileRosterPhase::kPublished); CHECK_FALSE(committed.owners_retired);
  CHECK(intent->prepared_roster()->epoch() == old_epoch + (same_row ? 0 : 1));
  const auto read = model->CaptureRead(error); REQUIRE(read); REQUIRE_FALSE(error);
  const auto* actual_title = read->models->dashboard.GetTitleInfo(title); REQUIRE(actual_title);
  CHECK(actual_title->last_played.is_valid());
  CHECK(actual_title->achievements_count == 1); CHECK(actual_title->gamerscore_total == 5);
  CHECK(read->models->dashboard.GetTitleName(title) == u"Roster Actual SPA");
  REQUIRE(read->models->titles.contains(title));
  CHECK(read->models->titles.at(title).GetString(kXdbfIdTitle) == u"Roster Actual SPA");
  const auto saved_dashboard = ReadFixtureDisk((same_row ? a.path : b.path) / "FFFE07D1.gpd");
  const auto view = XdbfTableView::Parse(saved_dashboard); REQUIRE(view);
  const auto opaque = view->Find(0xfedc, 0x2a); REQUIRE(opaque);
  const std::vector<uint8_t> expected_opaque{0xca, 0xfe, 0, 0x7f};
  CHECK(std::vector<uint8_t>(opaque->begin(), opaque->end()) == expected_opaque);
  const auto titles = tracker->GetPlayedTitles(selected_xuid);
  REQUIRE(titles.size() == 1); CHECK(titles[0].id == title);
  CHECK(titles[0].title_name == u"Roster Actual SPA"); CHECK(titles[0].WasTitlePlayed());
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol).error);
  const auto again = model->CaptureRead(error); REQUIRE(again);
  CHECK(again->origin.stamp.model_version == read->origin.stamp.model_version);
  CHECK(ReadFixtureDisk((same_row ? a.path : b.path) / "FFFE07D1.gpd") == saved_dashboard);
  auto joined = ProfileSettingsReadTestAccess::Joined(intent, binding); REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined).error);
  CHECK(ReadFixtureDisk(a.path / "Account") == a.account);
  CHECK(ReadFixtureDisk(b.path / "Account") == b.account);
  CHECK(ReadFixtureDisk(a.path / "1234ABCD.gpd") == a.title);
  CHECK(ReadFixtureDisk(b.path / "1234ABCD.gpd") == b.title);
  const auto appearance = read->models->Setting(kDashboardID, kReadAppearance, error); REQUIRE(appearance);
  CHECK(std::ranges::equal(appearance->get_extended_data(), same_row ? a.appearance : b.appearance));
}
TEST_CASE("Published native slot swaps read one exact row before and after canonical commitment",
          "[kernel][profile][roster][native][protocol][swap]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe000000000000156ull, 56);
  ReadFixtureProfile b(runtime, 0xe000000000000157ull, 57);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid, b.xuid, 0, 0}), expected).error);
  auto old_a = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(old_a);
  auto old_b = pm.AcquireProfileGpdModel(uint8_t(1), error); REQUIRE(old_b);
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto intent = pm.PrepareRoster(RosterDesired({b.xuid, a.xuid, 0, 0}), expected, error); REQUIRE(intent);
  auto cell = ProfileSettingsReadTestAccess::EnterNative(pm, intent, binding, error); REQUIRE(cell);
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  CHECK_FALSE(pm.CaptureProfile(uint8_t(0))); CHECK_FALSE(pm.CaptureProfile(uint8_t(1)));
  auto protocol = ProfileSettingsReadTestAccess::Protocol(intent, binding); REQUIRE(protocol);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol).error);
  CheckRoster(pm, {b.xuid, a.xuid, 0, 0});
  const auto new_b = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(new_b);
  const auto new_a = pm.AcquireProfileGpdModel(uint8_t(1), error); REQUIRE(new_a);
  CHECK(new_b != old_b); CHECK(new_a != old_a);
  CHECK(new_b->user_slot() == 0); CHECK(new_a->user_slot() == 1);
  CHECK(pm.GetUserIndexAssignedToProfile(a.xuid) == 1);
  CHECK(pm.GetUserIndexAssignedToProfile(b.xuid) == 0);
  for (uint8_t slot = 0; slot != 2; ++slot) {
    const auto model = slot ? new_a : new_b;
    const auto row = intent->prepared_roster()->slots()[slot];
    CHECK(model->xuid() == row.xuid); CHECK(model->sign_in_generation() == row.sign_in_generation);
    CHECK(model->storage()->source_id() == row.source_id);
    CHECK(model->storage()->source_generation() == row.source_generation);
    const auto read = pm.CaptureGpdRead(slot, error); REQUIRE(read);
    const auto scalar = read->Setting(kDashboardID, kReadScalar, error); REQUIRE(scalar);
    CHECK(std::get<int32_t>(scalar->get_host_data()) == (slot ? 56 : 57));
    CHECK(pm.CaptureGpdWriter(row.xuid, error).model == model);
  }
  auto joined = ProfileSettingsReadTestAccess::Joined(intent, binding); REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined).error);
  CHECK(pm.AcquireProfileGpdModel(uint8_t(0), error) == new_b);
  CHECK(pm.AcquireProfileGpdModel(uint8_t(1), error) == new_a);
  a.Unchanged(); b.Unchanged();
}
TEST_CASE("Native joined retirement keeps a busy old File and reuses its exact map commitment",
          "[kernel][profile][roster][native][protocol][busy_file]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime, 0xe000000000000154ull, 54);
  ReadFixtureProfile b(runtime, 0xe000000000000155ull, 55);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid, 0, 0, 0}), expected).error);
  auto old_model = pm.AcquireProfileGpdModel(uint8_t(0), error); REQUIRE(old_model);
  const auto old_origin = old_model->CaptureOrigin(error); REQUIRE(old_origin);
  const auto alias = fmt::format(kDefaultMountFormat, a.xuid) + ':';
  vfs::File* file = nullptr; vfs::FileAction action{};
  REQUIRE(runtime.emulator->file_system()->OpenFile(nullptr, alias + "\\Account",
      vfs::FileDisposition::kOpen, vfs::FileAccess::kGenericRead, false, false,
      &file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file);
  std::unique_ptr<vfs::File, void(*)(vfs::File*)> held(file, [](vfs::File* value) { value->Destroy(); });
  expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto intent = pm.PrepareRoster(RosterDesired({b.xuid, 0, 0, 0}), expected, error); REQUIRE(intent);
  auto cell = ProfileSettingsReadTestAccess::EnterNative(pm, intent, binding, error); REQUIRE(cell);
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell));
  auto protocol = ProfileSettingsReadTestAccess::Protocol(intent, binding); REQUIRE(protocol);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::CommitProtocol(pm, intent, *protocol).error);
  const auto selected = pm.CaptureProfile(uint8_t(0)); REQUIRE(selected); REQUIRE(selected->xuid() == b.xuid);
  CHECK(old_model->IsCurrent(*old_origin));
  auto joined = ProfileSettingsReadTestAccess::Joined(intent, binding); REQUIRE(joined);
  const auto busy = ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined);
  CHECK(busy.error == std::errc::device_or_resource_busy); CHECK(busy.phase == ProfileRosterPhase::kFinalizing);
  CHECK(busy.maps_committed); CHECK(busy.writers_available); CHECK_FALSE(busy.owners_retired);
  CHECK(pm.CaptureProfile(uint8_t(0)) == selected); CHECK_FALSE(old_model->IsCurrent(*old_origin));
  held.reset();
  const auto done = ProfileSettingsReadTestAccess::Finalize(pm, intent, *joined);
  REQUIRE_FALSE(done.error); CHECK(done.phase == ProfileRosterPhase::kCommitted); CHECK(done.owners_retired);
  CHECK(pm.CaptureProfile(uint8_t(0)) == selected); a.Unchanged(); b.Unchanged();
}

namespace {
// These execute the production read-stack/count on a real SYSTEM XHostThread.
// Frame coordinates are test metadata, not a claim to execute retail PPC,
// issue a XamTask, pin a native root image, or establish native rendering.
struct SystemReadThreadJoin {
  object_ref<XHostThread>& thread;
  xe::threading::Event* release = nullptr;
  bool created = false;
  ~SystemReadThreadJoin() {
    if (release) release->Set();
    if (created && xe::threading::Wait(thread->thread(), false,
            std::chrono::seconds(10)) != xe::threading::WaitResult::kSuccess)
      std::terminate(); // Never unwind live read scopes or test captures.
  }
};
}
TEST_CASE("Bounded SYSTEM thread reads use the real native scope stack without a Task",
          "[kernel][profile][roster][native][system_thread_read]") {
  ProfileAbiRuntime runtime;
  auto* kernel = runtime.emulator->kernel_state();
  auto& pm = *kernel->xam_state()->profile_manager();
  std::error_code error;
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto intent = pm.PrepareRoster(RosterDesired({0, 0, 0, 0}), expected, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  auto ticket = ProfileSettingsReadTestAccess::Ticket(*intent, *binding, error); REQUIRE(ticket);
  auto outer = ProfileSettingsReadTestAccess::Scope(ticket, binding); REQUIRE(outer);
  auto inner = ProfileSettingsReadTestAccess::Scope(ticket, binding); REQUIRE(inner);
  std::array<bool, 15> facts{};
  object_ref<XHostThread> thread;
  SystemReadThreadJoin join{thread};
  thread = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, 0, [&] {
    auto* self = XThread::GetCurrentThread(); auto* cpu = self->thread_state();
    auto* c = cpu->context();
    const auto saved_sp = c->r[1], saved_lr = c->lr;
    c->r[1] = self->stack_base() - 0x100; c->lr = 0x81001000;
    const auto sp = uint32_t(c->r[1]), lr = uint32_t(c->lr);
    facts[0] = self->native_frame_thread_identity().process_type == X_PROCTYPE_SYSTEM;
    facts[1] = !ProfileSettingsReadTestAccess::EnterWithoutTask(*outer, kernel, self, cpu);
    facts[2] = !ProfileSettingsReadTestAccess::EnterSystemFrame(*outer, kernel, self, cpu,
                                                            sp - 16, lr, 0x81A3F848);
    facts[3] = !ProfileSettingsReadTestAccess::EnterSystemFrame(*outer, kernel, self, cpu,
                                                            sp, lr + 4, 0x81A3F848);
    facts[4] = !ProfileSettingsReadTestAccess::EnterSystemFrame(*outer, kernel, self, cpu,
                                                            sp, lr, 0x81A3F849);
    facts[5] = !ProfileSettingsReadTestAccess::EnterSystemFrame(*outer, kernel, self, cpu,
                                                            sp, lr, 0x81A3F848, 0);
    facts[6] = ProfileSettingsReadTestAccess::EnterSystemFrame(*outer, kernel, self, cpu,
                                                           sp, lr, 0x81A3F848);
    if (facts[6]) {
      facts[7] = ProfileSettingsReadTestAccess::Current(kernel, cpu) == outer.get();
      facts[8] = !ProfileSettingsReadTestAccess::EnterSystemFrame(*outer, kernel, self, cpu,
                                                              sp, lr, 0x81A3F848);
      c->r[1] = sp - 0x40; c->lr = 0x81A3F890;
      facts[9] = ProfileSettingsReadTestAccess::EnterSystemFrame(*inner, kernel, self, cpu,
          uint32_t(c->r[1]), uint32_t(c->lr), 0x816A8C00, 2);
      if (facts[9]) {
        facts[10] = !ProfileSettingsReadTestAccess::Leave(*outer) &&
                    ProfileSettingsReadTestAccess::Current(kernel, cpu) == inner.get();
        facts[11] = ProfileSettingsReadTestAccess::Leave(*inner) &&
                    ProfileSettingsReadTestAccess::Current(kernel, cpu) == outer.get();
      }
      c->r[1] = sp; c->lr = lr;
      facts[12] = ProfileSettingsReadTestAccess::Leave(*outer);
      facts[13] = ProfileSettingsReadTestAccess::Current(kernel, cpu) == nullptr;
      facts[14] = !ProfileSettingsReadTestAccess::Leave(*outer);
    }
    c->r[1] = saved_sp; c->lr = saved_lr;
    return 0;
  }, kernel->GetSystemProcess()));
  join.created = thread->Create() == X_STATUS_SUCCESS; REQUIRE(join.created);
  REQUIRE(xe::threading::Wait(thread->thread(), false, std::chrono::seconds(10)) ==
          xe::threading::WaitResult::kSuccess);
  for (const bool fact : facts) CHECK(fact);
  CHECK_FALSE(ProfileSettingsReadTestAccess::Leave(*outer));
}
TEST_CASE("Actual SYSTEM read scope keeps profile authority close pending until Leave",
          "[kernel][profile][roster][native][system_thread_read][lifetime]") {
  ProfileAbiRuntime runtime;
  auto* kernel = runtime.emulator->kernel_state();
  auto& pm = *kernel->xam_state()->profile_manager();
  std::error_code error;
  auto binding = ProfileSettingsReadTestAccess::ClaimRoster(pm); REQUIRE(binding);
  auto expected = pm.CaptureRosterExpected(error); REQUIRE(expected.roster);
  auto intent = pm.PrepareRoster(RosterDesired({0, 0, 0, 0}), expected, error);
  REQUIRE(intent); REQUIRE_FALSE(error);
  auto ticket = ProfileSettingsReadTestAccess::Ticket(*intent, *binding, error); REQUIRE(ticket);
  auto scope = ProfileSettingsReadTestAccess::Scope(ticket, binding); REQUIRE(scope);
  auto entered = xe::threading::Event::CreateManualResetEvent(false);
  auto release = xe::threading::Event::CreateManualResetEvent(false);
  REQUIRE(entered); REQUIRE(release);
  std::atomic<bool> admitted{false}, left{false}, held{false};
  object_ref<XHostThread> thread;
  SystemReadThreadJoin join{thread, release.get()};
  thread = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, 0, [&] {
    auto* self = XThread::GetCurrentThread(); auto* cpu = self->thread_state();
    auto* c = cpu->context(); const auto saved_sp = c->r[1], saved_lr = c->lr;
    c->r[1] = self->stack_base() - 0x100; c->lr = 0x81001000;
    admitted.store(ProfileSettingsReadTestAccess::EnterSystemFrame(*scope, kernel, self, cpu,
        uint32_t(c->r[1]), uint32_t(c->lr), 0x81A3F848));
    entered->Set();
    if (admitted.load()) {
      held.store(xe::threading::Wait(release.get(), false, std::chrono::seconds(5)) ==
                 xe::threading::WaitResult::kSuccess);
      left.store(ProfileSettingsReadTestAccess::Leave(*scope));
    }
    c->r[1] = saved_sp; c->lr = saved_lr; return 0;
  }, kernel->GetSystemProcess()));
  join.created = thread->Create() == X_STATUS_SUCCESS; REQUIRE(join.created);
  REQUIRE(xe::threading::Wait(entered.get(), false, std::chrono::seconds(5)) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE(admitted.load());
  CHECK_FALSE(ProfileSettingsReadTestAccess::Leave(*scope)); // Wrong actual thread.
  ProfileSettingsReadTestAccess::BeginStop(pm);
  CHECK_FALSE(ProfileSettingsReadTestAccess::Scope(ticket, binding));
  auto close = std::async(std::launch::async, [&] { ProfileSettingsReadTestAccess::Close(pm); });
  const auto waiting = close.wait_for(std::chrono::milliseconds(50));
  release->Set();
  REQUIRE(xe::threading::Wait(thread->thread(), false, std::chrono::seconds(10)) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE(close.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  close.get();
  CHECK(waiting == std::future_status::timeout);
  CHECK(held.load()); CHECK(left.load());
  CHECK_FALSE(ProfileSettingsReadTestAccess::Scope(ticket, binding));
}
#endif

namespace {
class OverlappedLogging {
 public:
  OverlappedLogging() : previous_(cvars::log_level) {
    // The Debug ABI formatter dereferences lpdword output parameters before
    // entry. Guard-page probes exercise the ordinary registered ABI/body.
    cvars::log_level = std::min(previous_, int32_t(2));
  }
  ~OverlappedLogging() { cvars::log_level = previous_; }
 private:
  int32_t previous_;
};
struct OverlappedAbiFixture {
  OverlappedLogging logging;
  ProfileAbiRuntime runtime;
  LastErrorAbiStorage callers{*runtime.emulator};
  Memory& memory = *runtime.emulator->memory();
  uint32_t page = memory.SystemHeapAlloc(0x1000, 0x1000);
  OverlappedAbiFixture() { REQUIRE(page); Reset(0); }
  ~OverlappedAbiFixture() { memory.SystemHeapFree(page); }
  uint32_t overlap() const { return page + 0x100; }
  uint32_t output() const { return page + 0x200; }
  xe::be<uint32_t>& Word(uint32_t at) { return callers.Word(at); }
  void Reset(uint32_t result, uint32_t event = 0, uint32_t length = 0x12345678) {
    std::memset(memory.TranslateVirtual(page), 0xA7, 0x1000);
    memory.Zero(overlap(), sizeof(XAM_OVERLAPPED));
    Word(overlap()) = result; Word(overlap() + 4) = length;
    Word(overlap() + 0xC) = event;
  }
  const cpu::Export* Entry(uint16_t ordinal) {
    const auto* entry = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", ordinal);
    REQUIRE(entry); REQUIRE(entry->is_implemented()); REQUIRE(entry->function_data.trampoline);
    return entry;
  }
  uint64_t Call(uint16_t ordinal, uint32_t pointer, uint32_t length, uint32_t wait,
                uint32_t caller = 0) {
    auto& c = callers.contexts[caller];
    c.r[3] = 0xBADC0FFE00000000ull | pointer;
    c.r[4] = 0xCAFEF00D00000000ull | length;
    c.r[5] = 0xFEDCBA9800000000ull | wait;
    Entry(ordinal)->function_data.trampoline(&c);
    return c.r[3];
  }
  uint32_t GuardedPage() {
    constexpr uint32_t address = 0x90011000;
    auto* heap = memory.LookupHeap(address); REQUIRE(heap);
    REQUIRE(heap->AllocFixed(address - 0x1000, 0x3000, 0x1000,
        kMemoryAllocationReserve, kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(address, 0x1000, 0x1000,
        kMemoryAllocationCommit, kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(address), 0xA7, 0x1000);
    return address;
  }
};
}  // namespace
TEST_CASE("Registered overlapped queries return completed operation DWORDs and length",
          "[kernel][overlapped][abi]") {
  const auto ordinal = GENERATE(uint16_t(0x1FB), uint16_t(0x43B));
  const auto result = GENERATE(0u, 5u, 0x80004005u);
  OverlappedAbiFixture f;
  const auto* entry = f.Entry(ordinal);
  CHECK(std::string_view(entry->name) == (ordinal == 0x43B ? "XGetOverlappedResult" : "XamGetOverlappedResult"));
  f.Reset(result, 0xFFFFFFFF); // Completed calls must not look this event up.
  const auto original = f.callers.Word(f.callers.Error(0));
  CHECK(f.Call(ordinal, f.overlap(), f.output(), 1) == uint64_t(result));
  CHECK(f.Word(f.output()) == 0x12345678);
  CHECK(f.Word(f.output() - 4) == 0xA7A7A7A7);
  CHECK(f.Word(f.output() + 4) == 0xA7A7A7A7);
  CHECK(f.Word(f.overlap()) == result);
  CHECK(f.callers.Word(f.callers.Error(0)) == original);
  CHECK(f.callers.Word(f.callers.Error(1)) == 0x11223345);
}
TEST_CASE("Pending nonwaiting and eventless queries leave output and LastError alone",
          "[kernel][overlapped][abi]") {
  const auto ordinal = GENERATE(uint16_t(0x1FB), uint16_t(0x43B));
  const bool eventless = GENERATE(false, true);
  OverlappedAbiFixture f;
  f.Reset(X_ERROR_IO_PENDING, eventless ? 0u : 0xFFFFFFFFu);
  CHECK(f.Call(ordinal, f.overlap(), f.output(), eventless ? 1u : 0u) == X_ERROR_IO_INCOMPLETE);
  CHECK(f.Word(f.output()) == 0xA7A7A7A7);
  CHECK(f.Word(f.overlap()) == X_ERROR_IO_PENDING);
  CHECK(f.callers.Word(f.callers.Error(0)) == 0x11223344);
}
TEST_CASE("Overlapped queries access only native branch-selected words across guards",
          "[kernel][overlapped][guards]") {
  const auto ordinal = GENERATE(uint16_t(0x1FB), uint16_t(0x43B));
  OverlappedAbiFixture f;
  const uint32_t last_word = f.GuardedPage() + 0xFFC;
  f.Word(last_word) = 0x80004005;
  CHECK(f.Call(ordinal, last_word, 0, 1) == 0x80004005u); // No event/length read.
  f.Word(last_word) = X_ERROR_IO_PENDING;
  CHECK(f.Call(ordinal, last_word, f.output(), 0) == X_ERROR_IO_INCOMPLETE);
  CHECK(f.Word(f.output()) == 0xA7A7A7A7);
  CHECK(f.Call(ordinal, last_word, f.output(), 1) == X_ERROR_INVALID_PARAMETER);
  f.Word(last_word) = 0;
  CHECK(f.Call(ordinal, last_word, f.output(), 0) == X_ERROR_INVALID_PARAMETER);
  CHECK(f.Word(f.output()) == 0xA7A7A7A7);
  CHECK(f.Call(ordinal, 0, f.output(), 0) == X_ERROR_INVALID_PARAMETER);
  CHECK(f.Call(ordinal, f.overlap(), last_word + 4, 0) == X_ERROR_INVALID_PARAMETER);
  CHECK(f.Word(last_word) == 0);
}
TEST_CASE("Native length-store then result-reread order survives aliasing and inherited GCRC",
          "[kernel][overlapped][abi]") {
  const auto ordinal = GENERATE(uint16_t(0x1FB), uint16_t(0x43B));
  OverlappedAbiFixture f;
  f.Reset(5, 0xFFFFFFFF, 0x6ABCDEF0);
  {
    auto lock = global_critical_region::AcquireDirect();
    CHECK(f.Call(ordinal, f.overlap(), f.overlap(), 0) == 0x6ABCDEF0u);
  }
  CHECK(f.Word(f.overlap()) == 0x6ABCDEF0);
  f.Reset(0x80004005, 0xFFFFFFFF, 0x6ABCDEF0);
  CHECK(f.Call(ordinal, f.overlap(), f.overlap() + 0xC, 1) == 0x80004005u);
  CHECK(f.Word(f.overlap() + 0xC) == 0x6ABCDEF0); // No completed-event lookup.
  CHECK(f.callers.Word(f.callers.Error(0)) == 0x11223344);
}

#if XE_PLATFORM_LINUX
namespace {
// Observe entry to the REAL registered event's wait handle. Return exactly the
// production primitive; no wait status/completion is supplied by this observer.
class OverlappedObservedEvent final : public XEvent {
 public:
  OverlappedObservedEvent(KernelState* kernel, std::shared_ptr<xe::threading::Event> entered)
      : XEvent(kernel), entered_(std::move(entered)) {}
 protected:
  xe::threading::WaitHandle* GetWaitHandle() override {
    entered_->Set();
    return XEvent::GetWaitHandle();
  }
 private:
  std::shared_ptr<xe::threading::Event> entered_;
};
struct OverlappedThreadJoin {
  object_ref<XHostThread>& thread;
  object_ref<XEvent> release;
  bool created = false;
  ~OverlappedThreadJoin() {
    if (!created) { return; }
    // Test failure cleanup releases a real wait before fixture destruction;
    // this never becomes a successful operation/completion assertion.
    if (release) { release->Set(0, false); }
    if (xe::threading::Wait(thread->thread(), false, std::chrono::seconds(10)) !=
        xe::threading::WaitResult::kSuccess) { std::terminate(); }
  }
};
}  // namespace
TEST_CASE("Actual SYSTEM query waits for a later registered completion and rereads error or success",
          "[kernel][overlapped][wait][lifetime]") {
  const auto ordinal = GENERATE(uint16_t(0x1FB), uint16_t(0x43B));
  const auto operation = GENERATE(0u, 5u, 0x80004005u);
  OverlappedAbiFixture f;
  auto* kernel = f.runtime.emulator->kernel_state();
  auto entered = std::shared_ptr<xe::threading::Event>(xe::threading::Event::CreateManualResetEvent(false));
  REQUIRE(entered);
  object_ref<XEvent> event(new OverlappedObservedEvent(kernel, entered));
  event->Initialize(false, false);
  f.Reset(X_ERROR_IO_PENDING, event->handle(), 0xBAADF00D);
  const auto* query = f.Entry(ordinal); const auto* complete = f.Entry(0x1F5);
  uint32_t result = 0xBAADF00D, completion = 0xBAADF00D, error_after = 0;
  bool actual_system = false;
  object_ref<XHostThread> waiter, producer;
  OverlappedThreadJoin waiter_join{waiter, event}, producer_join{producer, event};
  waiter = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, 0, [&] {
    auto* self = XThread::GetCurrentThread(); auto* c = self->thread_state()->context();
    actual_system = self->native_frame_thread_identity().process_type == X_PROCTYPE_SYSTEM;
    const uint32_t kt = f.Word(uint32_t(c->r[13]) + 0x100);
    f.Word(kt + 0x160) = 0x31415926;
    c->r[3] = f.overlap(); c->r[4] = f.output(); c->r[5] = 1;
    query->function_data.trampoline(c);
    result = uint32_t(c->r[3]); error_after = f.Word(kt + 0x160);
    return 0;
  }, kernel->GetSystemProcess()));
  waiter_join.created = waiter->Create() == X_STATUS_SUCCESS; REQUIRE(waiter_join.created);
  REQUIRE(xe::threading::Wait(entered.get(), false, std::chrono::seconds(5)) == xe::threading::WaitResult::kSuccess);
  CHECK(xe::threading::Wait(waiter->thread(), false, std::chrono::milliseconds(0)) == xe::threading::WaitResult::kTimeout);
  CHECK(f.Word(f.output()) == 0xA7A7A7A7);
  producer = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, 0, [&] {
    auto* c = XThread::GetCurrentThread()->thread_state()->context();
    c->r[3] = f.overlap(); c->r[4] = operation; c->r[5] = operation; c->r[6] = 0x76543210;
    complete->function_data.trampoline(c); completion = uint32_t(c->r[3]); return 0;
  }, kernel->GetSystemProcess()));
  producer_join.created = producer->Create() == X_STATUS_SUCCESS; REQUIRE(producer_join.created);
  REQUIRE(xe::threading::Wait(producer->thread(), false, std::chrono::seconds(10)) == xe::threading::WaitResult::kSuccess);
  REQUIRE(xe::threading::Wait(waiter->thread(), false, std::chrono::seconds(10)) == xe::threading::WaitResult::kSuccess);
  CHECK(actual_system); CHECK(completion == X_ERROR_SUCCESS); CHECK(result == operation);
  CHECK(f.Word(f.output()) == 0x76543210); CHECK(error_after == 0x31415926);
  CHECK(f.Word(f.overlap()) == operation);
}
TEST_CASE("Successful real event wait rereads a still-pending operation instead of inventing success",
          "[kernel][overlapped][wait][lifetime]") {
  const auto ordinal = GENERATE(uint16_t(0x1FB), uint16_t(0x43B));
  OverlappedAbiFixture f; auto* kernel = f.runtime.emulator->kernel_state();
  object_ref<XEvent> event(new XEvent(kernel)); event->Initialize(false, true);
  f.Reset(X_ERROR_IO_PENDING, event->handle(), 0x12345678);
  const auto* query = f.Entry(ordinal); uint32_t result = 0xBAADF00D;
  object_ref<XHostThread> thread; OverlappedThreadJoin join{thread, event};
  thread = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, 0, [&] {
    auto* c = XThread::GetCurrentThread()->thread_state()->context();
    c->r[3] = f.overlap(); c->r[4] = f.output(); c->r[5] = 1;
    query->function_data.trampoline(c); result = uint32_t(c->r[3]); return 0;
  }, kernel->GetSystemProcess()));
  join.created = thread->Create() == X_STATUS_SUCCESS; REQUIRE(join.created);
  REQUIRE(xe::threading::Wait(thread->thread(), false, std::chrono::seconds(10)) == xe::threading::WaitResult::kSuccess);
  CHECK(result == X_ERROR_IO_PENDING); CHECK(f.Word(f.output()) == 0x12345678);
  CHECK(f.Word(f.overlap()) == X_ERROR_IO_PENDING);
}
TEST_CASE("Pending invalid-event wait uses per-API errors and the passed PCR instead of TLS",
          "[kernel][overlapped][wait][last_error]") {
  const auto ordinal = GENERATE(uint16_t(0x1FB), uint16_t(0x43B));
  const bool inhibited = GENERATE(false, true);
  OverlappedAbiFixture f; auto* kernel = f.runtime.emulator->kernel_state();
  f.Reset(X_ERROR_IO_PENDING, 0xFFFFFFFF);
  f.Word(f.callers.Pcr(0) + 0x150) = inhibited ? 1u : 0u;
  const auto* query = f.Entry(ordinal);
  uint32_t result = 0xBAADF00D, actual_error = 0;
  object_ref<XHostThread> thread; OverlappedThreadJoin join{thread, {}};
  thread = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, 0, [&] {
    auto* actual = XThread::GetCurrentThread()->thread_state()->context();
    const uint32_t kt = f.Word(uint32_t(actual->r[13]) + 0x100);
    f.Word(kt + 0x160) = 0x31415926;
    auto& supplied = f.callers.contexts[0];
    supplied.r[3] = f.overlap(); supplied.r[4] = f.output(); supplied.r[5] = 1;
    query->function_data.trampoline(&supplied);
    result = uint32_t(supplied.r[3]); actual_error = f.Word(kt + 0x160); return 0;
  }, kernel->GetSystemProcess()));
  join.created = thread->Create() == X_STATUS_SUCCESS; REQUIRE(join.created);
  REQUIRE(xe::threading::Wait(thread->thread(), false, std::chrono::seconds(10)) == xe::threading::WaitResult::kSuccess);
  CHECK(result == (ordinal == 0x43B && inhibited ? 0u : X_ERROR_INVALID_HANDLE));
  CHECK(f.Word(f.callers.Error(0)) == (ordinal == 0x43B && !inhibited ? X_ERROR_INVALID_HANDLE : 0x11223344u));
  CHECK(f.Word(f.callers.Error(1)) == 0x11223345);
  CHECK(actual_error == 0x31415926); CHECK(f.Word(f.output()) == 0xA7A7A7A7);
}
#endif


#if XE_PLATFORM_LINUX
TEST_CASE("Registered profile mount survives namespace contention for new and reused aliases",
          "[kernel][profile_creation][profile_mount][contention][abi]") {
  const bool reuse = GENERATE(false, true);
  ProfileAbiRuntime runtime;
  constexpr uint64_t xuid = 0xE000000000000481ull;
  X_XAMACCOUNTINFO account{};
  const auto original = SeedOwnedAccount(runtime, xuid, account);
  auto* pm = runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto* fs = runtime.emulator->file_system();
  REQUIRE(pm->LoadAccount(xuid));
  const auto alias = HostAccountMount(xuid);
  if (reuse) { REQUIRE(pm->MountProfile(xuid)); }
  const auto previous_device = fs->AcquireDevice(alias);
  REQUIRE(bool(previous_device) == reuse);
  ReadExAbiFixture f(*runtime.emulator);
  const auto* entry = runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex", 0x237);
  // Ordinal237 is XamProfileOpen in this export table.
  REQUIRE(entry); REQUIRE(entry->function_data.trampoline);
  *f.memory.TranslateVirtual<uint8_t*>(f.allocation + 0x80) = 0;
  auto context = f.context;
  context.r[3] = xuid; context.r[4] = f.allocation + 0x80;
  context.r[5] = 0; context.r[6] = f.Output();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<uint32_t> completed;
  auto result = completed.get_future();
  auto namespace_owner = global_critical_region::AcquireDirect();
  std::thread caller([&] {
    entered.set_value();
    try {
      entry->function_data.trampoline(&context);
      completed.set_value(uint32_t(context.r[3]));
    } catch (...) { completed.set_exception(std::current_exception()); }
  });
  auto finish = [&](void*) {
    if (namespace_owner.owns_lock()) { namespace_owner.unlock(); }
    if (caller.joinable()) { caller.join(); }
  };
  std::unique_ptr<void, decltype(finish)> join(reinterpret_cast<void*>(1), finish);
  REQUIRE(entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  // Keep a real competing GCRC owner until this observation completes. The
  // operation must not report INVALID_PARAMETER just because it contends.
  CHECK(result.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout);
  // A namespace-waiting opener must not retain PM metadata and prevent the
  // current namespace owner from making this ordinary metadata observation.
  CHECK(pm->GetAccountCount() == 1);
  join.reset();
  const auto status = result.get();
  CHECK(status == X_ERROR_SUCCESS);
  if (status == X_ERROR_SUCCESS) {
    XCONTENT_AGGREGATE_DATA descriptor{};
    std::memcpy(&descriptor, f.memory.TranslateVirtual(f.Output()), sizeof(descriptor));
    CHECK(descriptor.content_type == XContentType::kProfile);
    CHECK(uint32_t(descriptor.title_id) == kDashboardID);
    CHECK(uint32_t(descriptor.device_id) == 1);
    CHECK(descriptor.file_name() == xe::string_util::to_hex_string(xuid));
    CHECK(*f.memory.TranslateVirtual<uint8_t*>(f.Output() - 1) == 0xA7);
    CHECK(*f.memory.TranslateVirtual<uint8_t*>(f.Output() + sizeof(descriptor)) == 0xA7);
    const auto actual_device = fs->AcquireDevice(alias); REQUIRE(actual_device);
    if (reuse) { CHECK(actual_device == previous_device); }
    REQUIRE(pm->DismountProfile(xuid)); // Release exactly this successful open.
  }
  if (reuse) { REQUIRE(pm->DismountProfile(xuid)); }
  CHECK_FALSE(fs->AcquireDevice(alias));
  CHECK(OwnedAccountDisk(runtime, xuid) == original);
  // An inherited caller GCRC still refuses before host I/O. This is distinct
  // from waiting for another thread's short namespace ownership.
  std::memset(f.memory.TranslateVirtual(f.Output()), 0xA7, sizeof(XCONTENT_AGGREGATE_DATA));
  context = f.context;
  context.r[3] = xuid; context.r[4] = f.allocation + 0x80;
  context.r[5] = 0; context.r[6] = f.Output();
  {
    auto inherited = global_critical_region::AcquireDirect();
    entry->function_data.trampoline(&context);
    CHECK(uint32_t(context.r[3]) == X_ERROR_INVALID_PARAMETER);
  }
  const auto* output = f.memory.TranslateVirtual<uint8_t*>(f.Output());
  CHECK(std::all_of(output, output + sizeof(XCONTENT_AGGREGATE_DATA),
                    [](uint8_t value) { return value == 0xA7; }));
  CHECK_FALSE(fs->AcquireDevice(alias));
  CHECK(OwnedAccountDisk(runtime, xuid) == original);
}
#endif

}  // namespace xe::kernel::xam::test

namespace xe::kernel::xam::test {
namespace {
struct OriginalLogonTraceSetting {
  bool before=cvars::native_system_ui_trace;
  OriginalLogonTraceSetting(){cvars::native_system_ui_trace=true;}
  ~OriginalLogonTraceSetting(){cvars::native_system_ui_trace=before;}
};
}
// Journal payload tests confer no native entry/completion/read authority.
TEST_CASE("Original logon scalar journal survives issuer teardown and preserves stage correlation",
          "[kernel][profile_creation][original_logon][lifetime]") {
  OriginalLogonTraceSetting trace;
  using D=ProfileManager::NativeOriginalLogonDiagnostic;
  NativeOriginalLogonObservation first,second;
  {
    ProfileAbiRuntime runtime;
    auto sink=runtime.emulator->kernel_state()->xam_state()->profile_manager()->CaptureNativeCreationDiagnosticSink();
    first=ProfileManager::BeginNativeOriginalLogon(sink,0x20B,0x24,true);
    second=ProfileManager::BeginNativeOriginalLogon(sink,0x224,0x120424,false);
    REQUIRE(first.active());REQUIRE(second.active());CHECK(first.attempt()!=second.attempt());
    sink.reset();runtime.emulator.reset();CHECK(KernelState::shared()==nullptr);
  }
  D out;uint32_t observed=99;out.result=0xCD;
  CHECK_FALSE(ProfileManager::CopyNativeOriginalLogon(first,D::kNativeReturn,out,&observed));
  CHECK(observed==0);CHECK(out.result==0xCD);
  D value;value.stage=D::kProtocolReturn;value.result_available=1;value.result=0x1234;value.input_mask_available=1;value.input_nonempty_mask=0;value.input_nonempty_count=0;
  ProfileManager::RecordNativeOriginalLogon(first,value);
  value.stage=D::kExportReturn;value.result=0;
  ProfileManager::RecordNativeOriginalLogon(first,value);
  REQUIRE(ProfileManager::CopyNativeOriginalLogon(first,D::kProtocolReturn,out,&observed));
  CHECK(observed==2);CHECK(out.ordinal==0x20B);CHECK(out.flags==0x24);CHECK(out.overlap_present==1);CHECK(out.result==0x1234);CHECK(out.journal!=0);CHECK(out.input_mask_available==1);CHECK(out.input_nonempty_mask==0);CHECK(out.input_nonempty_count==0);
  const auto journal=out.journal;const auto protocol_sequence=out.capture_sequence;CHECK(protocol_sequence!=0);
  value.stage=D::kProtocolReturn;value.result=0xFFFF;
  ProfileManager::RecordNativeOriginalLogon(first,value);
  REQUIRE(ProfileManager::CopyNativeOriginalLogon(first,D::kProtocolReturn,out));CHECK(out.result==0x1234);
  REQUIRE(ProfileManager::CopyNativeOriginalLogon(first,D::kExportReturn,out));CHECK(out.result==0);CHECK(out.capture_sequence>protocol_sequence);
  CHECK_FALSE(ProfileManager::CopyNativeOriginalLogon(second,D::kProtocolReturn,out));
  REQUIRE(ProfileManager::CopyNativeOriginalLogon(second,D::kEntry,out));
  CHECK(out.ordinal==0x224);CHECK(out.flags==0x120424);CHECK(out.overlap_present==0);CHECK(out.journal==journal);
}
TEST_CASE("Original logon scalar journal bounds attempts and publishes one complete concurrent stage",
          "[kernel][profile_creation][original_logon][lifetime]") {
  OriginalLogonTraceSetting trace;ProfileAbiRuntime runtime;
  using D=ProfileManager::NativeOriginalLogonDiagnostic;
  auto sink=runtime.emulator->kernel_state()->xam_state()->profile_manager()->CaptureNativeCreationDiagnosticSink();
  std::array<NativeOriginalLogonObservation,8> attempts;
  for(size_t i=0;i<attempts.size();++i){attempts[i]=ProfileManager::BeginNativeOriginalLogon(sink,i&1?0x224:0x20B,uint32_t(i),false);REQUIRE(attempts[i].active());CHECK(attempts[i].attempt()==i+1);}
  CHECK_FALSE(ProfileManager::BeginNativeOriginalLogon(sink,0x20B,0x24,true).active());
  runtime.emulator.reset();sink.reset();
  const auto publish=[&](uint32_t result){D d;d.stage=D::kNativeReturn;d.result_available=1;d.result=result;d.error_available=1;d.error_value=result^0x55;ProfileManager::RecordNativeOriginalLogon(attempts[0],d);};
  std::thread one(publish,0x1234),two(publish,0x5678);one.join();two.join();
  D out;uint32_t observed=0;
  REQUIRE(ProfileManager::CopyNativeOriginalLogon(attempts[0],D::kNativeReturn,out,&observed));
  CHECK(observed==2);CHECK((out.result==0x1234 || out.result==0x5678));CHECK(out.error_value==(out.result^0x55));
  for(size_t i=0;i<attempts.size();++i){REQUIRE(ProfileManager::CopyNativeOriginalLogon(attempts[i],D::kEntry,out));CHECK(out.flags==i);}
}
TEST_CASE("Registered original logon exports retain invalid-buffer ABI and observable legacy return",
          "[kernel][profile_creation][original_logon][abi]") {
  OriginalLogonTraceSetting trace;ProfileAbiRuntime runtime;
  using D=ProfileManager::NativeOriginalLogonDiagnostic;
  auto* kernel=runtime.emulator->kernel_state();
  auto sink=kernel->xam_state()->profile_manager()->CaptureNativeCreationDiagnosticSink();
  for(const uint32_t ordinal:{0x20Bu,0x224u}){
    const auto* entry=runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex",ordinal);
    REQUIRE(entry);REQUIRE(entry->function_data.trampoline);
    PPCContext context{};context.kernel_state=kernel;context.r[3]=0;context.r[4]=0x24;context.r[5]=0;
    entry->function_data.trampoline(&context);CHECK(context.r[3]==X_ERROR_INVALID_PARAMETER);
  }
  D out;uint32_t observed=0;
  for(uint32_t attempt=1;attempt<=2;++attempt){
    REQUIRE(ProfileManager::CopyNativeOriginalLogon(sink,attempt,D::kEntry,out,&observed));
    CHECK(observed==2);CHECK(out.ordinal==(attempt==1?0x20Bu:0x224u));CHECK(out.flags==0x24);CHECK(out.overlap_present==0);
    REQUIRE(ProfileManager::CopyNativeOriginalLogon(sink,attempt,D::kAdmissionReturn,out));
    CHECK(out.disposition_available==1);CHECK(out.disposition==0); // unavailable native; real legacy route
    REQUIRE(ProfileManager::CopyNativeOriginalLogon(sink,attempt,D::kExportReturn,out));
    CHECK(out.result_available==1);CHECK(out.result==X_ERROR_INVALID_PARAMETER);
    CHECK_FALSE(ProfileManager::CopyNativeOriginalLogon(sink,attempt,D::kNativeReturn,out));
  }
  const auto third=ProfileManager::BeginNativeOriginalLogon(sink,0x20B,0,false);
  REQUIRE(third.active());CHECK(third.attempt()==3);
}

TEST_CASE("Initial empty native baseline validates unchanged common authority without a transaction",
          "[kernel][profile][roster][native][baseline]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile unsigned_inventory(runtime,0xe000000000000341ull,53);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;auto expected=pm.CaptureRosterExpected(error);
  REQUIRE(expected.roster);REQUIRE_FALSE(error);
  const auto epoch=expected.roster->epoch();
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto ticket=ProfileSettingsReadTestAccess::CurrentTicket(pm,binding,error);
  REQUIRE(ticket);REQUIRE_FALSE(error);
  const auto before=ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,expected,ticket);
  REQUIRE(before);CHECK(*before!=0);
  for(const auto& slot:expected.roster->slots())CHECK(slot==ProfileRosterSlot{});
  auto after=pm.CaptureRosterExpected(error);REQUIRE(after.roster);REQUIRE_FALSE(error);
  CHECK(after.roster==expected.roster);CHECK(after.roster->epoch()==epoch);
  CHECK(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,after,ticket)==before);
  ProfileRosterExpected unissued;unissued.roster=expected.roster;
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,unissued,ticket));
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,{},expected,ticket));
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,expected,{}));
  unsigned_inventory.Unchanged();
}
TEST_CASE("Prepared and canceled identity operations invalidate initial empty baseline freshness",
          "[kernel][profile][roster][native][baseline]") {
  ProfileAbiRuntime runtime;
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto ticket=ProfileSettingsReadTestAccess::CurrentTicket(pm,binding,error);REQUIRE(ticket);
  const auto sequence=ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,expected,ticket);REQUIRE(sequence);
  auto intent=pm.PrepareRoster(RosterDesired({}),expected,error);REQUIRE(intent);REQUIRE_FALSE(error);
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,expected,ticket));
  const auto attempt_ticket=ProfileSettingsReadTestAccess::Ticket(*intent,*binding,error);REQUIRE(attempt_ticket);
  const auto canceled=pm.CancelRoster(intent);CHECK(canceled.phase==ProfileRosterPhase::kCanceled);CHECK_FALSE(canceled.error);
  auto after=pm.CaptureRosterExpected(error);REQUIRE(after.roster);REQUIRE_FALSE(error);
  CHECK(after.roster==expected.roster);CHECK(after.roster->epoch()==expected.roster->epoch());
  const auto later=ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,after,ticket);REQUIRE(later);
  CHECK(*later!=*sequence); // Owner must refuse the old captured sequence, even with the same roster.
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,after,attempt_ticket));
}
TEST_CASE("Initial empty baseline refuses real metadata contention without altering the roster",
          "[kernel][profile][roster][native][baseline]") {
  ProfileAbiRuntime runtime;
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto ticket=ProfileSettingsReadTestAccess::CurrentTicket(pm,binding,error);REQUIRE(ticket);
  const auto sequence=ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,expected,ticket);REQUIRE(sequence);
  {
    HeldRosterMetadata held(pm);
    REQUIRE(held.ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,expected,ticket));
  }
  CHECK(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,expected,ticket)==sequence);
  auto after=pm.CaptureRosterExpected(error);REQUIRE(after.roster);CHECK(after.roster==expected.roster);
}
TEST_CASE("Initial empty baseline refuses occupied roster and expired foreign factory binding",
          "[kernel][profile][roster][native][baseline]") {
  std::shared_ptr<const ProfileRosterNativeBinding> foreign;
  {
    ProfileAbiRuntime previous;
    auto& pm=*previous.emulator->kernel_state()->xam_state()->profile_manager();
    foreign=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(foreign);
  }
  ProfileAbiRuntime runtime;
  ReadFixtureProfile profile(runtime,0xe000000000000342ull,54);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;auto empty=pm.CaptureRosterExpected(error);REQUIRE(empty.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({profile.xuid,0,0,0}),empty).error);
  auto occupied=pm.CaptureRosterExpected(error);REQUIRE(occupied.roster);
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto ticket=ProfileSettingsReadTestAccess::CurrentTicket(pm,binding,error);REQUIRE(ticket);
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,occupied,ticket));
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,binding,empty,ticket));
  CHECK_FALSE(ProfileSettingsReadTestAccess::InitialEmptySequence(pm,foreign,occupied,ticket));
  CHECK(pm.CaptureRosterExpected(error).roster==occupied.roster);
}

#if XE_PLATFORM_LINUX
TEST_CASE("Native canceled-empty publication removes unchanged old rows without changing the requested roster",
          "[kernel][profile][roster][native][native_terminal]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile a(runtime,0xe000000000000a11ull,81);
  ReadFixtureProfile b(runtime,0xe000000000000a12ull,82);
  ReadFixtureProfile c(runtime,0xe000000000000a13ull,83);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::error_code error;auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid,b.xuid,0,0}),expected).error);
  auto old_a=pm.AcquireProfileGpdModel(uint8_t(0),error);REQUIRE(old_a);
  auto old_b=pm.AcquireProfileGpdModel(uint8_t(1),error);REQUIRE(old_b);
  const auto origin_a=old_a->CaptureOrigin(error);REQUIRE(origin_a);
  const auto origin_b=old_b->CaptureOrigin(error);REQUIRE(origin_b);
  expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  const auto desired=RosterDesired({a.xuid,b.xuid,c.xuid,0});
  auto intent=pm.PrepareRoster(desired,expected,error);REQUIRE(intent);REQUIRE_FALSE(error);
  const auto requested=intent->prepared_roster();
  const auto wire=ProfileSettingsReadTestAccess::PreparedWire(*intent);
  CHECK_FALSE(intent->publication_outcome());CHECK_FALSE(intent->published_roster());
  CHECK_FALSE(old_a->AllocateRequestId(error));CHECK(error==std::errc::device_or_resource_busy);
  CHECK_FALSE(old_b->AllocateRequestId(error));CHECK(error==std::errc::device_or_resource_busy);
  const auto staged=ProfileSettingsReadTestAccess::MountReferences(pm,c.xuid);REQUIRE(staged>0);
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,intent,binding,error);REQUIRE(cell);
  CHECK_FALSE(ProfileSettingsReadTestAccess::Publish(cell,ProfileRosterPublicationOutcome(255)));
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell,ProfileRosterPublicationOutcome::kCanceledEmpty));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Publish(cell));
  CHECK(intent->publication_outcome()==ProfileRosterPublicationOutcome::kCanceledEmpty);
  REQUIRE(intent->published_roster());CHECK(intent->published_roster()!=requested);
  CHECK(intent->published_roster()->epoch()==expected.roster->epoch()+1);
  CHECK(intent->prepared_roster()==requested);CHECK(requested->slots()[2].xuid==c.xuid);
  CHECK(ProfileSettingsReadTestAccess::PreparedWire(*intent)==wire);
  CHECK_FALSE(old_a->AllocateRequestId(error));CHECK(error==std::errc::device_or_resource_busy);
  auto protocol=ProfileSettingsReadTestAccess::Protocol(intent,binding,0x800704c7u);REQUIRE(protocol);
  const auto published=ProfileSettingsReadTestAccess::CommitProtocol(pm,intent,*protocol);
  REQUIRE_FALSE(published.error);CHECK(published.maps_committed);CHECK(published.writers_available);
  CHECK_FALSE(published.owners_retired);CHECK(published.native_protocol_value==0x800704c7u);
  CHECK((published.changed==std::array<bool,4>{true,true,false,false}));
  CheckRoster(pm,{0,0,0,0});
  CHECK(ProfileSettingsReadTestAccess::MountReferences(pm,c.xuid)==staged);
  CHECK(old_a->IsCurrent(*origin_a));CHECK(old_b->IsCurrent(*origin_b));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent,0x800704c7u));
  CHECK_FALSE(pm.CaptureGpdWriter(a.xuid,error).model);REQUIRE(error);
  auto joined=ProfileSettingsReadTestAccess::Joined(intent,binding);REQUIRE(joined);
  const auto done=ProfileSettingsReadTestAccess::Finalize(pm,intent,*joined);
  REQUIRE_FALSE(done.error);CHECK(done.phase==ProfileRosterPhase::kCommitted);
  CHECK(done.owners_retired);CHECK_FALSE(old_a->IsCurrent(*origin_a));CHECK_FALSE(old_b->IsCurrent(*origin_b));
  CHECK(ProfileSettingsReadTestAccess::MountReferences(pm,c.xuid)==0);
  CHECK(ProfileSettingsReadTestAccess::MountReferences(pm,a.xuid)==0);
  CHECK(ProfileSettingsReadTestAccess::MountReferences(pm,b.xuid)==0);
  CHECK(intent->prepared_roster()==requested);CHECK(intent->published_roster()->slots()[2].xuid==0);
  REQUIRE(pm.CaptureCachedAccount(c.xuid,error));REQUIRE_FALSE(error);
  a.Unchanged();b.Unchanged();c.Unchanged();
}
TEST_CASE("Canceled-empty from an empty baseline is a distinct factual outcome and not an initial-frame receipt",
          "[kernel][profile][roster][native][native_terminal]") {
  ProfileAbiRuntime runtime;ReadFixtureProfile a(runtime,0xe000000000000a14ull,84);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();std::error_code error;
  auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto intent=pm.PrepareRoster(RosterDesired({a.xuid,0,0,0}),expected,error);REQUIRE(intent);
  const auto requested=intent->prepared_roster();
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,intent,binding,error);REQUIRE(cell);
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell,ProfileRosterPublicationOutcome::kCanceledEmpty));
  CHECK(intent->published_roster()!=expected.roster);CHECK(intent->published_roster()!=requested);
  CHECK(intent->published_roster()->epoch()==expected.roster->epoch());
  auto protocol=ProfileSettingsReadTestAccess::Protocol(intent,binding,0x800704c7u);REQUIRE(protocol);
  const auto maps=ProfileSettingsReadTestAccess::CommitProtocol(pm,intent,*protocol);REQUIRE_FALSE(maps.error);
  CHECK(std::none_of(maps.changed.begin(),maps.changed.end(),[](bool v){return v;}));
  CHECK_FALSE(ProfileSettingsReadTestAccess::PublishedEmptyRows(intent,0x800704c7u));
  CheckRoster(pm,{0,0,0,0});CHECK(intent->prepared_roster()->slots()[0].xuid==a.xuid);
  auto joined=ProfileSettingsReadTestAccess::Joined(intent,binding);REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm,intent,*joined).error);
  CHECK(ProfileSettingsReadTestAccess::MountReferences(pm,a.xuid)==0);a.Unchanged();
}
TEST_CASE("Actual fixed and following SYSTEM read scopes keep old and canceled-empty projections distinct",
          "[kernel][profile][roster][native][native_terminal][system_thread_read]") {
  ProfileAbiRuntime runtime;ReadFixtureProfile a(runtime,0xe000000000000a15ull,85);
  auto* kernel=runtime.emulator->kernel_state();auto& pm=*kernel->xam_state()->profile_manager();
  std::error_code error;auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({a.xuid,0,0,0}),expected).error);
  auto old=pm.CaptureProfile(uint8_t(0));REQUIRE(old);
  expected=pm.CaptureRosterExpected(error);auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto intent=pm.PrepareRoster(RosterDesired({a.xuid,0,0,0}),expected,error);REQUIRE(intent);
  auto fixed_ticket=ProfileSettingsReadTestAccess::Ticket(*intent,*binding,error);REQUIRE(fixed_ticket);
  auto following_ticket=ProfileSettingsReadTestAccess::FollowingTicket(*intent,*binding,error);REQUIRE(following_ticket);
  auto fixed=ProfileSettingsReadTestAccess::Scope(fixed_ticket,binding);REQUIRE(fixed);
  auto following=ProfileSettingsReadTestAccess::Scope(following_ticket,binding);REQUIRE(following);
  auto entered=xe::threading::Event::CreateManualResetEvent(false);
  auto published=xe::threading::Event::CreateManualResetEvent(false);REQUIRE(entered);REQUIRE(published);
  std::array<bool,7> facts{};object_ref<XHostThread> thread;SystemReadThreadJoin join{thread,published.get()};
  thread=object_ref<XHostThread>(new XHostThread(kernel,128*1024,0,[&]{
    auto* self=XThread::GetCurrentThread();auto* cpu=self->thread_state();auto* c=cpu->context();
    const auto saved_sp=c->r[1],saved_lr=c->lr;
    c->r[1]=self->stack_base()-0x100;c->lr=0x81001000;
    facts[0]=ProfileSettingsReadTestAccess::EnterSystemFrame(*fixed,kernel,self,cpu,uint32_t(c->r[1]),uint32_t(c->lr),0x81A3F848);
    entered->Set();
    if(facts[0]){
      facts[1]=xe::threading::Wait(published.get(),false,std::chrono::seconds(5))==xe::threading::WaitResult::kSuccess;
      facts[2]=pm.CaptureProfile(uint8_t(0))==old;
      c->r[1]-=0x40;c->lr=0x81A3F890;
      facts[3]=ProfileSettingsReadTestAccess::EnterSystemFrame(*following,kernel,self,cpu,uint32_t(c->r[1]),uint32_t(c->lr),0x816A8C00,2);
      if(facts[3]){facts[4]=!pm.CaptureProfile(uint8_t(0));facts[5]=ProfileSettingsReadTestAccess::Leave(*following);}
      c->r[1]+=0x40;c->lr=0x81001000;facts[6]=ProfileSettingsReadTestAccess::Leave(*fixed);
    }
    c->r[1]=saved_sp;c->lr=saved_lr;return 0;
  },kernel->GetSystemProcess()));
  join.created=thread->Create()==X_STATUS_SUCCESS;REQUIRE(join.created);
  REQUIRE(xe::threading::Wait(entered.get(),false,std::chrono::seconds(5))==xe::threading::WaitResult::kSuccess);
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,intent,binding,error);REQUIRE(cell);
  REQUIRE(ProfileSettingsReadTestAccess::Publish(cell,ProfileRosterPublicationOutcome::kCanceledEmpty));
  auto protocol=ProfileSettingsReadTestAccess::Protocol(intent,binding,0x800704c7u);REQUIRE(protocol);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::CommitProtocol(pm,intent,*protocol).error);
  CheckRoster(pm,{0,0,0,0});published->Set();
  REQUIRE(xe::threading::Wait(thread->thread(),false,std::chrono::seconds(10))==xe::threading::WaitResult::kSuccess);
  for(const auto fact:facts)CHECK(fact);
  auto joined=ProfileSettingsReadTestAccess::Joined(intent,binding);REQUIRE(joined);
  REQUIRE_FALSE(ProfileSettingsReadTestAccess::Finalize(pm,intent,*joined).error);
  CHECK_FALSE(ProfileSettingsReadTestAccess::Scope(fixed_ticket,binding));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Scope(following_ticket,binding));a.Unchanged();
}
TEST_CASE("A native-entered operation without a publisher cannot forge a terminal roster or rollback",
          "[kernel][profile][roster][native][native_terminal][refusal]") {
  ProfileAbiRuntime runtime;ReadFixtureProfile a(runtime,0xe000000000000a16ull,86);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();std::error_code error;
  auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  auto intent=pm.PrepareRoster(RosterDesired({a.xuid,0,0,0}),expected,error);REQUIRE(intent);
  auto cell=ProfileSettingsReadTestAccess::EnterNative(pm,intent,binding,error);REQUIRE(cell);
  CHECK_FALSE(intent->published_roster());CHECK_FALSE(intent->publication_outcome());
  CHECK_FALSE(ProfileSettingsReadTestAccess::Protocol(intent,binding,0x80004004u));
  CHECK_FALSE(ProfileSettingsReadTestAccess::Joined(intent,binding));
  const auto canceled=pm.CancelRoster(intent);CHECK(canceled.error==std::errc::device_or_resource_busy);
  CHECK(canceled.phase==ProfileRosterPhase::kNativeEntered);
  CHECK_FALSE(canceled.maps_committed);CHECK_FALSE(canceled.owners_retired);
  CHECK(ProfileSettingsReadTestAccess::MountReferences(pm,a.xuid)>0);
  CHECK(intent->prepared_roster()->slots()[0].xuid==a.xuid);
  CHECK_FALSE(pm.CaptureRosterExpected(error).roster);REQUIRE(error);
  // Fixture teardown uses real Kernel/PM close. No fake publisher, native
  // rollback, protocol, or worker birth is introduced to release this state.
  a.Unchanged();
}
TEST_CASE("Canceled-capable preparation drains a genuine pending model even for a requested unchanged row",
          "[kernel][profile][roster][native][native_terminal][pending_model]") {
  ProfileAbiRuntime runtime;ReadFixtureProfile a(runtime,0xe000000000000a17ull,87);
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();std::error_code error;
  auto expected=pm.CaptureRosterExpected(error);REQUIRE(expected.roster);
  const auto desired=RosterDesired({a.xuid,0,0,0});REQUIRE_FALSE(pm.AdoptRoster(desired,expected).error);
  auto writer=pm.CaptureGpdWriter(a.xuid,error);REQUIRE(writer.model);
  ProfileGpdMutation command;command.kind=ProfileGpdMutationKind::kSetSetting;command.title_id=kDashboardID;
  command.request_id=writer.model->AllocateRequestId(error);REQUIRE(command.request_id);
  command.setting.emplace(static_cast<UserSettingId>(kReadScalar),int32_t(97));
  bool retry=false;auto pending=writer.model->Begin(command,retry,error);REQUIRE(pending);REQUIRE_FALSE(error);
  expected=pm.CaptureRosterExpected(error);auto binding=ProfileSettingsReadTestAccess::ClaimRoster(pm);REQUIRE(binding);
  CHECK_FALSE(pm.PrepareRoster(desired,expected,error));CHECK(error==std::errc::device_or_resource_busy);
  const auto written=writer.ExecutePending(pending);REQUIRE_FALSE(written.error);
  CHECK(written.state==ProfileGpdTransactionAttempt::State::kCommitted);
  auto intent=pm.PrepareRoster(desired,expected,error);REQUIRE(intent);REQUIRE_FALSE(error);
  CHECK_FALSE(writer.model->AllocateRequestId(error));CHECK(error==std::errc::device_or_resource_busy);
  CHECK(pm.CancelRoster(intent).phase==ProfileRosterPhase::kCanceled);
  REQUIRE(writer.model->AllocateRequestId(error));REQUIRE_FALSE(error);
}
namespace {
std::filesystem::path DeletionHeader(const ReadFixtureProfile& profile) {
  return profile.path.parent_path().parent_path() / "Headers" / "00010000" /
      (fmt::format("{:016X}", profile.xuid) + ".header");
}
std::vector<uint8_t> SeedDeletionHeader(const ReadFixtureProfile& profile) {
  // The genuine host aggregate format, including a separate license/unknown
  // suffix. This is deliberately NOT a manufactured native CON Header.
  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1; data.content_type = XContentType::kProfile;
  data.title_id = kDashboardID; data.xuid = profile.xuid;
  data.set_file_name(fmt::format("{:016X}", profile.xuid));
  data.set_display_name(u"Exact profile deletion fixture");
  std::vector<uint8_t> header(sizeof(data) + 13, 0xB6);
  std::memcpy(header.data(), &data, sizeof(data));
  xe::store_and_swap<uint32_t>(header.data() + sizeof(data), 0x5A39C87D);
  std::error_code error;
  std::filesystem::create_directories(DeletionHeader(profile).parent_path(), error);
  REQUIRE_FALSE(error);
  ReadFixtureWrite(DeletionHeader(profile), header);
  return header;
}
}
TEST_CASE("Profile deletion joins package Header cache and restart while preserving game saves",
          "[kernel][profile][profile_deletion][persistence][restart]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile target(runtime, 0xE000000000000611ull, 63);
  ReadFixtureProfile other(runtime, 0xE000000000000612ull, 64);
  const auto header = SeedDeletionHeader(target);
  const auto other_header = SeedDeletionHeader(other);
  const std::vector<uint8_t> unknown{0x45,0xA3,0x00,0x77,0xF0,0x29};
  const std::vector<uint8_t> save{0xE1,0x00,0xA8,0x3C,0x47,0x2B,0x91};
  ReadFixtureWrite(target.path / "unknown.member", unknown);
  const auto save_path = runtime.root.path / "content" /
      fmt::format("{:016X}", target.xuid) / "1234ABCD" / "00000001" / "slot" / "save.bin";
  REQUIRE(std::filesystem::create_directories(save_path.parent_path()));
  ReadFixtureWrite(save_path, save);
  {
    auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
    std::error_code error;
    const auto expected = pm.CaptureRosterExpected(error); REQUIRE_FALSE(error);
    REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({other.xuid,target.xuid,0,0}),expected).error);
    REQUIRE(pm.DeleteProfile(target.xuid));
    CheckRoster(pm,{other.xuid,0,0,0});
    CHECK_FALSE(pm.CaptureCachedAccount(target.xuid,error));
    CHECK(error==std::errc::no_such_file_or_directory);
    CHECK_FALSE(std::filesystem::exists(target.path));
    CHECK_FALSE(std::filesystem::exists(DeletionHeader(target)));
    CHECK(ReadFixtureDisk(save_path)==save);
    CHECK(ReadFixtureDisk(DeletionHeader(other))==other_header);
    other.Unchanged();
  }
  RestartHostAccountRuntime(runtime);
  auto& restarted = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  restarted.ReloadProfiles();
  std::error_code error;
  CHECK_FALSE(restarted.CaptureCachedAccount(target.xuid,error));
  CHECK(error==std::errc::no_such_file_or_directory);
  REQUIRE(restarted.CaptureCachedAccount(other.xuid,error)); REQUIRE_FALSE(error);
  CHECK_FALSE(std::filesystem::exists(target.path));
  CHECK_FALSE(std::filesystem::exists(DeletionHeader(target)));
  CHECK(ReadFixtureDisk(save_path)==save);
  CHECK(ReadFixtureDisk(DeletionHeader(other))==other_header);
  other.Unchanged();
}
TEST_CASE("Profile deletion preserves real Header readers and exact source retirement",
          "[kernel][profile][profile_deletion][namespace][source]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile target(runtime, 0xE000000000000613ull, 65);
  const auto header = SeedDeletionHeader(target);
  auto& manager = *runtime.emulator->kernel_state()->content_manager();
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  auto source = manager.sources().Source("hdd"); REQUIRE(source);
  std::error_code error;
  SECTION("A real independent Header lease denies deletion before any physical change") {
    const auto relative = DeletionHeader(target).lexically_relative(source->storage->path());
    auto reader = source->storage->OpenFile(relative,filesystem::FileAccess::kGenericRead,error);
    REQUIRE(reader); REQUIRE_FALSE(error);
    CHECK_FALSE(pm.DeleteProfile(target.xuid));
    target.Unchanged(); CHECK(ReadFixtureDisk(DeletionHeader(target))==header);
    REQUIRE(pm.CaptureCachedAccount(target.xuid,error)); REQUIRE_FALSE(error);
    reader.reset();
    REQUIRE(pm.DeleteProfile(target.xuid));
    CHECK_FALSE(std::filesystem::exists(target.path));
    CHECK_FALSE(std::filesystem::exists(DeletionHeader(target)));
  }
  SECTION("A prepared old generation never deletes through a replacement or retired root") {
    auto deletion = ProfileSettingsReadTestAccess::PrepareDeletion(pm,target.xuid);
    REQUIRE(deletion);
    auto package = ProfileSettingsReadTestAccess::DeletionPackage(deletion); REQUIRE(package);
    source->storage->Retire();
    CHECK_FALSE(ProfileSettingsReadTestAccess::AdvanceDeletion(pm,deletion));
    target.Unchanged(); CHECK(ReadFixtureDisk(DeletionHeader(target))==header);
    REQUIRE(pm.CaptureCachedAccount(target.xuid,error)); REQUIRE_FALSE(error);
  }
}
TEST_CASE("Durable profile removal retains its exact PM handoff across namespace and metadata contention",
          "[kernel][profile][profile_deletion][cache][contention][persistence]") {
  ProfileAbiRuntime runtime;
  ReadFixtureProfile target(runtime, 0xE000000000000614ull, 66);
  SeedDeletionHeader(target);
  auto& pm = *runtime.emulator->kernel_state()->xam_state()->profile_manager();
  std::weak_ptr<ProfileGpdModelAuthority> retired_model;
  bool verify_retired_model=false;
  SECTION("Real signed-in writers drain and retired model ownership reaches the exact deletion handoff") {
    std::error_code error;
    auto expected=pm.CaptureRosterExpected(error);REQUIRE_FALSE(error);
    REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({target.xuid,0,0,0}),expected).error);
    {
      auto model=pm.AcquireProfileGpdModel(uint8_t(0),error);REQUIRE(model);REQUIRE_FALSE(error);
      auto source=runtime.emulator->kernel_state()->content_manager()->sources().Source("hdd");REQUIRE(source);
      const auto& storage=model->storage();
      CHECK(storage->source()==source->managed_identity->source());
      CHECK(storage->guest_generation()==source->storage);
      CHECK(storage->source()!=storage->guest_generation());
      CHECK(storage->view()->path()==target.path);
      auto account=pm.CaptureDurableAccount(target.xuid,error);REQUIRE(account);REQUIRE_FALSE(error);
      auto writer=account->identity()->EnterWrite(error);REQUIRE(writer);REQUIRE_FALSE(error);
      CHECK(account->identity()->storage()->source()==source->managed_identity->source());
      CHECK(account->identity()->storage()->guest_generation()==source->storage);
      XCONTENT_AGGREGATE_DATA requested{};requested.device_id=1;requested.title_id=kDashboardID;
      requested.content_type=XContentType::kProfile;requested.xuid=target.xuid;
      requested.set_file_name(fmt::format("{:016X}",target.xuid));
      // Exact source write resolution first sees the live Account namespace
      // claim. Its existing public refusal is ACCESS_DENIED, before the PM's
      // later signed-in/writer drain checks; this is not a PM inner-guard test.
      CHECK(runtime.emulator->kernel_state()->content_manager()->DeleteContent(0,requested)==X_ERROR_ACCESS_DENIED);
      CHECK_FALSE(account->identity()->writes_drained());
      CheckRoster(pm,{target.xuid,0,0,0});target.Unchanged();
      writer.reset();CHECK(account->identity()->writes_drained());
      expected=pm.CaptureRosterExpected(error);REQUIRE_FALSE(error);
      const auto signed_out=pm.AdoptRoster(RosterDesired({}),expected);
      REQUIRE_FALSE(signed_out.error);REQUIRE(signed_out.phase==ProfileRosterPhase::kCommitted);
      CHECK(account->identity()->retired());
      retired_model=model;verify_retired_model=true;
    }
    // A real subsequent empty roster operation reaps the old envelope. The
    // PM's genuine retired-model owner must still retain it until deletion.
    expected=pm.CaptureRosterExpected(error);REQUIRE_FALSE(error);
    REQUIRE_FALSE(pm.AdoptRoster(RosterDesired({}),expected).error);
    REQUIRE_FALSE(retired_model.expired());
  }
  auto deletion = ProfileSettingsReadTestAccess::PrepareDeletion(pm,target.xuid);
  REQUIRE(deletion);
  auto package = ProfileSettingsReadTestAccess::DeletionPackage(deletion); REQUIRE(package);
  const auto durable = package->Advance();
  REQUIRE_FALSE(durable.error); REQUIRE(durable.complete);
  CHECK_FALSE(std::filesystem::exists(target.path));
  CHECK_FALSE(std::filesystem::exists(DeletionHeader(target)));
  std::error_code error;
  REQUIRE(pm.CaptureCachedAccount(target.xuid,error)); REQUIRE_FALSE(error);
  SECTION("The completed physical outcome does not need a VFS namespace lock for PM cache publication") {
    auto held = global_critical_region::AcquireDirect();
    auto publish = std::async(std::launch::async,[&] {
      return ProfileSettingsReadTestAccess::AdvanceDeletion(pm,deletion);
    });
    const auto ready = publish.wait_for(std::chrono::seconds(2));
    held.unlock(); // Independent bounded release, even if a hidden lock blocks.
    const bool result = publish.get();
    CHECK(ready==std::future_status::ready); REQUIRE(result);
  }
  SECTION("A genuinely observed completed removal can erase only its old cache after device retirement") {
    auto source = runtime.emulator->kernel_state()->content_manager()->sources().Source("hdd");
    REQUIRE(source); REQUIRE(source->storage);
    // Physical removal and its actual durable observation already completed
    // above. This does not authorize any new IO through the retired source.
    source->storage->Retire();
    REQUIRE(ProfileSettingsReadTestAccess::AdvanceDeletion(pm,deletion));
  }
  SECTION("A busy metadata cut keeps the same completed capsule for the existing host reaper") {
    auto held = ProfileSettingsReadTestAccess::HoldMetadata(pm);
    auto publish = std::async(std::launch::async,[&] {
      return ProfileSettingsReadTestAccess::AdvanceDeletion(pm,deletion);
    });
    const auto ready = publish.wait_for(std::chrono::seconds(2));
    held.unlock();
    const bool result = publish.get();
    CHECK(ready==std::future_status::ready); CHECK_FALSE(result);
    REQUIRE(pm.CaptureCachedAccount(target.xuid,error)); REQUIRE_FALSE(error);
    pm.ReapReleasedProfileReads();
  }
  REQUIRE(ProfileSettingsReadTestAccess::AdvanceDeletion(pm,deletion));
  CHECK_FALSE(pm.CaptureCachedAccount(target.xuid,error));
  CHECK(error==std::errc::no_such_file_or_directory);
  REQUIRE(ProfileSettingsReadTestAccess::AdvanceDeletion(pm,deletion));
  const auto same_outcome = package->Advance();
  CHECK_FALSE(same_outcome.error); CHECK(same_outcome.complete);
  CHECK_FALSE(std::filesystem::exists(target.path));
  CHECK_FALSE(std::filesystem::exists(DeletionHeader(target)));
  if(verify_retired_model){
    CHECK_FALSE(retired_model.expired()); // The actual capsule/cache operation still owns the old holder.
    deletion.reset(); // Drop that last owner HERE, outside every production guard.
    CHECK(retired_model.expired()); // A missed retired-vector match would leak the genuine old model.
  }
}

namespace {
struct ProfileDeleteExportOutcome {
  uint32_t returned = 0, result = 0, extended = 0, length = 0, context = 0, event_state = 0;
};
ProfileDeleteExportOutcome CallProfileDelete270(ProfileAbiRuntime& runtime,
    const XCONTENT_AGGREGATE_DATA& data, bool asynchronous) {
  auto* kernel = runtime.emulator->kernel_state(); auto& memory = *runtime.emulator->memory();
  struct Page { Memory& memory; uint32_t address; ~Page() { if(address)memory.SystemHeapFree(address); } } page{memory,memory.SystemHeapAlloc(0x1000,0x1000)};
  REQUIRE(page.address); memory.Zero(page.address,0x1000);
  std::memcpy(memory.TranslateVirtual(page.address),&data,sizeof(data));
  const uint32_t overlap_address=page.address+0x400;
  auto* overlap=memory.TranslateVirtual<XAM_OVERLAPPED*>(overlap_address);
  object_ref<XEvent> event(new XEvent(kernel)); event->Initialize(true,false);
  overlap->result=X_ERROR_IO_PENDING;overlap->context=0x12345678;overlap->event=event->handle();
  const auto* entry=runtime.emulator->export_resolver()->GetExportByOrdinal("xam.xex",0x270);
  REQUIRE(entry);REQUIRE(entry->function_data.trampoline);
  ProfileDeleteExportOutcome result;
  object_ref<XHostThread> thread;OverlappedThreadJoin join{thread,{}};
  thread=object_ref<XHostThread>(new XHostThread(kernel,128*1024,0,[&]{
    auto* context=XThread::GetCurrentThread()->thread_state()->context();
    context->r[3]=page.address;context->r[4]=asynchronous?overlap_address:0;
    entry->function_data.trampoline(context);result.returned=uint32_t(context->r[3]);return 0;
  },kernel->GetSystemProcess()));
  join.created=thread->Create()==X_STATUS_SUCCESS;REQUIRE(join.created);
  REQUIRE(xe::threading::Wait(thread->thread(),false,std::chrono::seconds(10))==xe::threading::WaitResult::kSuccess);
  result.result=overlap->result;result.extended=overlap->extended_error;
  result.length=overlap->length;result.context=overlap->context;
  uint32_t event_type=0;event->Query(&event_type,&result.event_state);
  return result;
}
void CheckProfileDeleteCompletion(const ProfileDeleteExportOutcome& out,bool asynchronous,uint32_t actual) {
  if(!asynchronous){CHECK(out.returned==actual);return;}
  CHECK(out.returned==X_ERROR_IO_PENDING);
  CHECK(out.result==(actual?X_ERROR_FUNCTION_FAILED:X_ERROR_SUCCESS));
  CHECK(out.extended==X_HRESULT_FROM_WIN32(actual));CHECK(out.length==0);
  CHECK(out.context==0);CHECK(out.event_state==1);
}
}
TEST_CASE("Registered profile-only content deletion joins exact storage and cache without signing out",
          "[kernel][profile][profile_deletion][content_delete][abi][persistence]") {
  ProfileAbiRuntime runtime;
  constexpr uint64_t uid=0xE000000000000621ull;
  X_XAMACCOUNTINFO account{};const auto original=SeedOwnedAccount(runtime,uid,account);
  auto* manager=runtime.emulator->kernel_state()->content_manager();
  auto& pm=*runtime.emulator->kernel_state()->xam_state()->profile_manager();
  const auto path=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(uid);
  GpdInfoProfile dashboard; UserSetting scalar(static_cast<UserSettingId>(kReadScalar),int32_t(73));
  dashboard.UpsertSetting(&scalar);ReadFixtureWrite(path/"FFFE07D1.gpd",ReadFixtureSerialize(dashboard));
  GpdInfoTitle title(kReadTitle);title.UpsertSetting(&scalar);ReadFixtureWrite(path/"1234ABCD.gpd",ReadFixtureSerialize(title));
  XCONTENT_AGGREGATE_DATA data{};data.device_id=1;data.title_id=kDashboardID;
  data.content_type=XContentType::kProfile;data.xuid=uid;data.set_file_name(fmt::format("{:016X}",uid));
  const auto header=path.parent_path().parent_path()/"Headers"/"00010000"/(fmt::format("{:016X}",uid)+".header");
  REQUIRE(std::filesystem::create_directories(header.parent_path()));
  std::vector<uint8_t> header_bytes(sizeof(data)+17,0x5B);std::memcpy(header_bytes.data(),&data,sizeof(data));ReadFixtureWrite(header,header_bytes);
  auto save_path=runtime.root.path/"content"/fmt::format("{:016X}",uid)/"1234ABCD"/"00000001"/"slot"/"save.bin";
  REQUIRE(std::filesystem::create_directories(save_path.parent_path()));
  const std::vector<uint8_t> save{0x91,0,0xF5,0xA8};ReadFixtureWrite(save_path,save);
  std::error_code error;
  SECTION("Synchronous cached deletion removes only its exact package and Header") {
    REQUIRE(pm.LoadAccount(uid));REQUIRE(pm.CaptureCachedAccount(uid,error));
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_SUCCESS);
    CHECK_FALSE(pm.CaptureCachedAccount(uid,error));CHECK(error==std::errc::no_such_file_or_directory);
    CHECK_FALSE(std::filesystem::exists(path));CHECK_FALSE(std::filesystem::exists(header));
    // A repeated actual missing package preserves the old content ABI error.
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_FILE_NOT_FOUND);
  }
  SECTION("Asynchronous deletion needs no cached Account and publishes its real event and result tuple") {
    REQUIRE_FALSE(pm.CaptureCachedAccount(uid,error));
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,true),true,X_ERROR_SUCCESS);
    CHECK_FALSE(std::filesystem::exists(path));CHECK_FALSE(std::filesystem::exists(header));
  }
  SECTION("A signed-in cached profile refuses without an implicit host signout") {
    REQUIRE(pm.LoadAccount(uid));pm.Login(uid,1,false);CheckRoster(pm,{0,uid,0,0});
    // The retained Account namespace denies source write resolution first;
    // CompleteRequest must preserve that exact public error in its HRESULT.
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,true),true,X_ERROR_ACCESS_DENIED);
    CheckRoster(pm,{0,uid,0,0});REQUIRE(pm.CaptureCachedAccount(uid,error));
    CHECK(ReadFixtureDisk(path/"Account")==original);CHECK(ReadFixtureDisk(header)==header_bytes);
  }
  SECTION("A genuine independent Header reader refuses before effects then permits real retry") {
    REQUIRE(pm.LoadAccount(uid));auto source=manager->sources().Source("hdd");REQUIRE(source);
    auto reader=source->storage->OpenFile(header.lexically_relative(source->storage->path()),filesystem::FileAccess::kGenericRead,error);
    REQUIRE(reader);REQUIRE_FALSE(error);
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_SHARING_VIOLATION);
    REQUIRE(pm.CaptureCachedAccount(uid,error));CHECK(ReadFixtureDisk(path/"Account")==original);
    reader.reset();CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_SUCCESS);
    CHECK_FALSE(pm.CaptureCachedAccount(uid,error));
  }
  SECTION("The selected device does not erase another source's same-XUID cache or signed-in row") {
    REQUIRE(pm.LoadAccount(uid));pm.Login(uid,1,false);
    const auto usb=runtime.root.path/"delete-usb";const auto usb_path=usb/"Content"/ProfileStorageLease::ProfileRelative(uid);
    REQUIRE(std::filesystem::create_directories(usb_path));ReadFixtureWrite(usb_path/"Account",original);
    const auto usb_header=usb_path.parent_path().parent_path()/"Headers"/"00010000"/(fmt::format("{:016X}",uid)+".header");
    REQUIRE(std::filesystem::create_directories(usb_header.parent_path()));ReadFixtureWrite(usb_header,header_bytes);
    ContentSource selected;selected.id="delete-usb";selected.device_id=0x57;selected.device_type=DeviceType::USBMASS;
    selected.read_only=false;selected.root=usb/"Content";selected.host_volume_root=usb;selected.private_state_parent=runtime.root.path;
    selected.guest_volume_root="\\Device\\Mass57PartitionFile\\Storage";selected.guest_root=selected.guest_volume_root+"\\Content";selected.name=u"Delete source";
    REQUIRE(manager->RegisterManagedSource(selected));data.device_id=selected.device_id;
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_SUCCESS);
    CHECK_FALSE(std::filesystem::exists(usb_path));CHECK_FALSE(std::filesystem::exists(usb_header));
    auto cached=pm.CaptureCachedAccount(uid,error);REQUIRE(cached);CHECK(cached->source_id()=="hdd");
    CheckRoster(pm,{0,uid,0,0});CHECK(ReadFixtureDisk(path/"Account")==original);CHECK(ReadFixtureDisk(header)==header_bytes);
  }
  SECTION("Physically case-varied canonical FATX components retain their exact deletion path") {
    auto lower=[](std::string value){std::transform(value.begin(),value.end(),value.begin(),
        [](unsigned char c){return char(c>='A'&&c<='Z'?c+('a'-'A'):c);});return value;};
    const auto varied_owner=runtime.root.path/"content"/lower(fmt::format("{:016X}",uid));
    std::filesystem::rename(path.parent_path().parent_path().parent_path(),varied_owner);
    const auto varied_title=varied_owner/"fFfE07d1";
    std::filesystem::rename(varied_owner/"FFFE07D1",varied_title);
    const auto varied_path=varied_title/"00010000"/lower(fmt::format("{:016X}",uid));
    std::filesystem::rename(varied_title/"00010000"/fmt::format("{:016X}",uid),varied_path);
    const auto varied_headers=varied_title/"hEaDeRs";
    std::filesystem::rename(varied_title/"Headers",varied_headers);
    const auto varied_header=varied_headers/"00010000"/(lower(fmt::format("{:016X}",uid))+".HeAdEr");
    std::filesystem::rename(varied_headers/"00010000"/(fmt::format("{:016X}",uid)+".header"),varied_header);
    save_path=varied_owner/"1234ABCD"/"00000001"/"slot"/"save.bin";
    auto source=manager->sources().Source("hdd");REQUIRE(source);
    const auto resolved=manager->sources().Resolve(*source,ProfileStorageLease::ProfileRelative(uid),false);
    REQUIRE(resolved);CHECK(*resolved==varied_path);
    REQUIRE_FALSE(pm.CaptureCachedAccount(uid,error));
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_SUCCESS);
    CHECK_FALSE(std::filesystem::exists(varied_path));CHECK_FALSE(std::filesystem::exists(varied_header));
    CHECK_FALSE(pm.CaptureCachedAccount(uid,error));
  }
  SECTION("Case-only physical siblings do not erase or wait on another cached profile") {
    REQUIRE(pm.LoadAccount(uid));const auto cached=pm.CaptureCachedAccount(uid,error);REQUIRE(cached);
    auto lower_name=fmt::format("{:016X}",uid);
    std::transform(lower_name.begin(),lower_name.end(),lower_name.begin(),
        [](unsigned char c){return char(c>='A'&&c<='Z'?c+('a'-'A'):c);});
    const auto sibling=path.parent_path()/lower_name;REQUIRE(std::filesystem::create_directory(sibling));
    ReadFixtureWrite(sibling/"Account",original);
    const auto sibling_header=header.parent_path()/(lower_name+".header");ReadFixtureWrite(sibling_header,header_bytes);
    data.set_file_name(lower_name);
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_ACCESS_DENIED);
    CHECK(pm.CaptureCachedAccount(uid,error)==cached);REQUIRE_FALSE(error);
    CHECK(ReadFixtureDisk(path/"Account")==original);CHECK(ReadFixtureDisk(header)==header_bytes);
    CHECK(ReadFixtureDisk(sibling/"Account")==original);CHECK(ReadFixtureDisk(sibling_header)==header_bytes);
  }
  SECTION("A canonical-profile host symlink cannot retarget deletion or cached identity") {
    REQUIRE(pm.LoadAccount(uid));
    constexpr uint64_t alias_uid=0xE000000000000622ull;
    const auto alias=runtime.root.path/"content"/ProfileStorageLease::ProfileRelative(alias_uid);
    REQUIRE(std::filesystem::create_directories(alias.parent_path()));
    std::filesystem::create_directory_symlink(path,alias,error);REQUIRE_FALSE(error);
    const auto alias_header=alias.parent_path().parent_path()/"Headers"/"00010000"/(fmt::format("{:016X}",alias_uid)+".header");
    REQUIRE(std::filesystem::create_directories(alias_header.parent_path()));ReadFixtureWrite(alias_header,header_bytes);
    data.xuid=alias_uid;data.set_file_name(fmt::format("{:016X}",alias_uid));
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_ACCESS_DENIED);
    CHECK(std::filesystem::is_symlink(alias));CHECK(ReadFixtureDisk(alias_header)==header_bytes);
    REQUIRE(pm.CaptureCachedAccount(uid,error));CHECK(ReadFixtureDisk(path/"Account")==original);CHECK(ReadFixtureDisk(header)==header_bytes);
  }
  SECTION("A noncanonical profile container retains exact spelling without deleting the cached canonical profile") {
    REQUIRE(pm.LoadAccount(uid));
    const auto other=path.parent_path()/"MiXeD-Profile";REQUIRE(std::filesystem::create_directory(other));
    ReadFixtureWrite(other/"opaque.member",save);const auto other_header=header.parent_path()/"MiXeD-Profile.header";ReadFixtureWrite(other_header,header_bytes);
    data.set_file_name("mixed-profile");
    CheckProfileDeleteCompletion(CallProfileDelete270(runtime,data,false),false,X_ERROR_SUCCESS);
    CHECK_FALSE(std::filesystem::exists(other));CHECK_FALSE(std::filesystem::exists(other_header));
    REQUIRE(pm.CaptureCachedAccount(uid,error));CHECK(ReadFixtureDisk(path/"Account")==original);CHECK(ReadFixtureDisk(header)==header_bytes);
  }
  CHECK(ReadFixtureDisk(save_path)==save);
}

#endif

} // namespace xe::kernel::xam::test
