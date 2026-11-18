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
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/file.h"
#include "xenia/memory.h"

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
  static std::unique_lock<std::recursive_mutex> HoldMetadata(ProfileManager& pm) {
    return std::unique_lock<std::recursive_mutex>(pm.profile_mount_mutex_);
  }
  static std::shared_ptr<ProfileRosterIntent> PrepareAttempt(ProfileManager& pm,
      const ProfileRosterDesired& desired,const ProfileRosterExpected& expected,
      std::error_code& error,std::shared_ptr<ProfileRosterIntent>& retained) {
    return pm.PrepareRosterAttempt(desired,expected,error,&retained);
  }
  static auto ClaimRoster(ProfileManager& pm) { return pm.ClaimNativeRosterFactory(); }
  static auto Ticket(ProfileRosterIntent& intent,const ProfileRosterNativeBinding& binding,
      std::error_code& error) { return intent.NativeReadTicket(false,binding,&error); }
  static void RetryCleanup(ProfileManager& pm) { pm.RetryRosterSettlement(); }
  static auto SelectedAccount(ProfileManager& pm, uint64_t xuid) {
    return pm.CaptureRosterAccount(xuid);
  }
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

}  // namespace xe::kernel::xam::test
