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
#include <iterator>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/devices/host_path_device.h"
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

}  // namespace xe::kernel::xam::test
