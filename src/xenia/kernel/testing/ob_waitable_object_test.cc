/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/ob_waitable_object.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xobject.h"
#include "xenia/memory.h"

namespace xe::kernel::xboxkrnl::test {
namespace {

constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kStorage = 0x90011000;
constexpr uint32_t kBody = kStorage + 0x110;
constexpr uint32_t kType = kStorage + 0x300;
constexpr uint32_t kAbsoluteTarget = kStorage + 0x500;
constexpr uint32_t kLowDefault = 0x20011000;
using Kind = WaitableObjectResolutionKind;
using Snapshot = std::array<uint8_t, 2 * kPageSize>;

class WaitableFixture {
 public:
  WaitableFixture() {
    REQUIRE(memory.Initialize());
    for (uint32_t address : {kStorage, kLowDefault}) {
      auto* heap = memory.LookupHeap(address);
      REQUIRE(heap);
      REQUIRE(heap->AllocFixed(address - kPageSize, 3 * kPageSize, kPageSize,
                               kMemoryAllocationReserve,
                               kMemoryProtectRead | kMemoryProtectWrite));
      REQUIRE(heap->AllocFixed(address, kPageSize, kPageSize,
                               kMemoryAllocationCommit,
                               kMemoryProtectRead | kMemoryProtectWrite));
      std::memset(memory.TranslateVirtual(address), 0xA7, kPageSize);
    }
    X_OBJECT_HEADER header{};
    header.pointer_count = 17;
    header.handle_count = 9;
    header.object_type_ptr = kType;
    header.flags = 0x6201;
    header.hash_index = 5;
    std::memcpy(memory.TranslateVirtual(kBody - sizeof(header)), &header,
                sizeof(header));
    // Real native listener bodies may begin with an XAM vtable, not a
    // dispatcher type. Resolution must not normalize that leading 0x81.
    Store(kBody, 0x81A91234);
    SetDefault(0xC);
    SetDispatcher(kBody + 0xC);
    SetDispatcher(kAbsoluteTarget);
    SetDispatcher(kLowDefault);
  }

  void Store(uint32_t address, uint32_t value) {
    xe::be<uint32_t> word = value;
    std::memcpy(memory.TranslateVirtual(address), &word, sizeof(word));
  }
  void SetDefault(uint32_t value) {
    X_OBJECT_TYPE type{};
    type.allocate_proc = 0x81A01010;
    type.free_proc = 0x81A01020;
    type.close_proc = 0x81A01030;
    type.delete_proc = 0x81A01040;
    type.unknown_proc = 0x81A01050;
    type.unknown_size_or_object_ = value;
    type.pool_tag = 0x4E6F744C;
    std::memcpy(memory.TranslateVirtual(kType), &type, sizeof(type));
  }
  void SetType(uint32_t address) { Store(kBody - 8, address); }
  void SetDispatcher(uint32_t address) {
    X_DISPATCH_HEADER dispatcher{};
    dispatcher.type = EventSynchronizationObject;
    dispatcher.signal_state = 1;
    dispatcher.wait_list.flink_ptr = address + 8;
    dispatcher.wait_list.blink_ptr = address + 8;
    std::memcpy(memory.TranslateVirtual(address), &dispatcher,
                sizeof(dispatcher));
  }
  Snapshot CopyBytes() const {
    Snapshot result;
    std::memcpy(result.data(), memory.TranslateVirtual(kStorage), kPageSize);
    std::memcpy(result.data() + kPageSize, memory.TranslateVirtual(kLowDefault),
                kPageSize);
    return result;
  }
  void CheckUnchanged(const Snapshot& before) const {
    CHECK(CopyBytes() == before);
  }
  WaitableObjectResolution Resolve(uint32_t body = kBody,
                                   uint32_t kernel_default = 0) {
    return ResolveWaitableObject(memory, body, kernel_default);
  }
  Memory memory;
};

TEST_CASE("ObGetWaitableObject resolves borrowed embedded dispatchers",
          "[kernel][ob_waitable]") {
  WaitableFixture fixture;
  for (uint32_t offset : {0u, 0xCu, 0x38u}) {
    CAPTURE(offset);
    fixture.SetDefault(offset);
    fixture.Store(kBody, 0x81A91234);
    fixture.SetDispatcher(kBody + offset);
    const auto before = fixture.CopyBytes();
    const auto result = fixture.Resolve();
    CHECK(result.kind == Kind::kOffset);
    CHECK(result.body == kBody);
    CHECK(result.object_type == kType);
    CHECK(result.default_object == offset);
    CHECK(result.waitable == kBody + offset);
    // The prefix, counts, vtable, object type and target are all borrowed.
    fixture.CheckUnchanged(before);
  }
}

TEST_CASE("ObGetWaitableObject resolves qualified absolute default objects",
          "[kernel][ob_waitable]") {
  WaitableFixture fixture;
  SECTION("Retail high address is a readable absolute dispatcher") {
    fixture.SetDefault(kAbsoluteTarget);
    const auto before = fixture.CopyBytes();
    const auto result = fixture.Resolve();
    CHECK(result.kind == Kind::kAbsolute);
    CHECK(result.waitable == kAbsoluteTarget);
    fixture.CheckUnchanged(before);
  }
  SECTION("Exact low HLE default identity is absolute") {
    fixture.SetDefault(kLowDefault);
    const auto before = fixture.CopyBytes();
    const auto result = fixture.Resolve(kBody, kLowDefault);
    CHECK(result.kind == Kind::kKernelDefault);
    CHECK(result.default_object == kLowDefault);
    CHECK(result.waitable == kLowDefault);
    fixture.CheckUnchanged(before);
  }
  SECTION("Arbitrary mapped low pointer is not the Kernel default identity") {
    fixture.SetDefault(kLowDefault);
    REQUIRE(fixture.memory.QueryRangeAccess(kBody + kLowDefault, 16) ==
            memory::PageAccess::kNoAccess);
    const auto before = fixture.CopyBytes();
    const auto result = fixture.Resolve(kBody, kLowDefault + 4);
    CHECK(result.kind == Kind::kUnreadableTarget);
    CHECK(result.waitable == 0);
    fixture.CheckUnchanged(before);
  }
}

TEST_CASE("ObGetWaitableObject accepts readable readonly borrowed storage",
          "[kernel][ob_waitable]") {
  WaitableFixture fixture;
  const auto before = fixture.CopyBytes();
  REQUIRE(fixture.memory.LookupHeap(kStorage)->Protect(kStorage, kPageSize,
                                                       kMemoryProtectRead));
  CHECK(fixture.memory.QueryRangeAccess(kBody - 16, 16) ==
        memory::PageAccess::kReadOnly);
  const auto result = fixture.Resolve();
  CHECK(result.kind == Kind::kOffset);
  CHECK(result.waitable == kBody + 0xC);
  fixture.CheckUnchanged(before);
}

TEST_CASE("ObGetWaitableObject refuses missing or truncated object metadata",
          "[kernel][ob_waitable]") {
  WaitableFixture fixture;
  const auto before = fixture.CopyBytes();
  for (uint32_t body : {0u, 8u, kStorage - 16, kStorage + 8}) {
    CAPTURE(body);
    const auto result = fixture.Resolve(body);
    CHECK(result.kind == (body ? Kind::kUnreadableHeader : Kind::kNullObject));
    CHECK(result.waitable == 0);
    fixture.CheckUnchanged(before);
  }
  for (uint32_t type :
       {0u, kStorage - 16, kStorage + kPageSize - 16, UINT32_MAX - 12}) {
    CAPTURE(type);
    fixture.SetType(type);
    const auto changed = fixture.CopyBytes();
    const auto result = fixture.Resolve();
    CHECK(result.kind == Kind::kUnreadableType);
    CHECK(result.waitable == 0);
    fixture.CheckUnchanged(changed);
  }
}

TEST_CASE("ObGetWaitableObject refuses uncovered targets and offset overflow",
          "[kernel][ob_waitable]") {
  WaitableFixture fixture;
  for (uint32_t value :
       {kPageSize, kStorage + kPageSize + 8, kStorage + kPageSize - 8,
        UINT32_MAX - 8, 0x7FFFFFFFu}) {
    CAPTURE(value);
    fixture.SetDefault(value);
    const auto before = fixture.CopyBytes();
    const auto result = fixture.Resolve();
    CHECK(result.kind == (value == 0x7FFFFFFFu ? Kind::kOffsetOverflow
                                               : Kind::kUnreadableTarget));
    CHECK(result.waitable == 0);
    fixture.CheckUnchanged(before);
  }
}

TEST_CASE("ObGetWaitableObject registered null trampoline needs no Kernel",
          "[kernel][ob_waitable]") {
  cpu::ExportResolver resolver;
  RegisterObExports(&resolver, nullptr);
  XboxkrnlModule::RegisterExportTable(&resolver);
  auto* entry = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x107);
  REQUIRE(entry);
  CHECK(std::string_view(entry->name) == "ObGetWaitableObject");
  CHECK(entry->get_type() == cpu::Export::Type::kFunction);
  CHECK(entry->is_implemented());
  CHECK((entry->tags & cpu::ExportTag::kStub) == 0);
  REQUIRE(entry->function_data.trampoline);
  PPCContext context{};
  for (size_t i = 0; i < 32; ++i) {
    context.r[i] = 0xAABBCCDD00000000ull + i;
  }
  context.r[3] = 0x1234567800000000ull;
  context.lr = 0xABCDEF1122334455ull;
  std::array<uint64_t, 32> before;
  std::copy_n(context.r, before.size(), before.begin());
  entry->function_data.trampoline(&context);
  CHECK(context.r[3] == 0);
  for (size_t i = 0; i < before.size(); ++i) {
    if (i != 3) {
      CAPTURE(i);
      CHECK(context.r[i] == before[i]);
    }
  }
  CHECK(context.lr == 0xABCDEF1122334455ull);
  // Nonnull export routing needs a real Kernel and native object lifetime;
  // only the pure resolver is exercised with real Memory above.
}

}  // namespace
}  // namespace xe::kernel::xboxkrnl::test

#if XE_PLATFORM_LINUX
DECLARE_string(cpu);
DECLARE_bool(debug);
DECLARE_string(system_readonly_source_root);
DECLARE_string(logged_profile_slot_0_xuid);
DECLARE_string(logged_profile_slot_1_xuid);
DECLARE_string(logged_profile_slot_2_xuid);
DECLARE_string(logged_profile_slot_3_xuid);
DECLARE_uint32(kernel_build_version);

namespace xe::kernel::xboxkrnl::test {
namespace {

// These assertions bind the preexisting guest layout, not just the order of the
// new fields. The former block ended immediately after background_processors.
static_assert(offsetof(KernelGuestGlobals, XboxKernelDefaultObject) == 0x118);
static_assert(offsetof(KernelGuestGlobals, idle_process) == 0x128);
static_assert(offsetof(KernelGuestGlobals, title_process) == 0x188);
static_assert(offsetof(KernelGuestGlobals, system_process) == 0x1E8);
static_assert(offsetof(KernelGuestGlobals, dispatcher_lock) == 0x248);
static_assert(offsetof(KernelGuestGlobals, ob_lock) == 0x24C);
static_assert(offsetof(KernelGuestGlobals, tls_lock) == 0x250);
static_assert(offsetof(KernelGuestGlobals, UsbdBootEnumerationDoneEvent) ==
              0x254);
static_assert(offsetof(KernelGuestGlobals, background_processors) == 0x264);
static_assert(offsetof(KernelGuestGlobals, UsbdDriverLoadRequiredEvent) ==
              0x268);
static_assert(offsetof(KernelGuestGlobals, StfsDeviceErrorEvent) == 0x278);
static_assert(sizeof(KernelGuestGlobals) == 0x288);
static_assert(sizeof(KernelVersion) == 8);

struct KernelDataSettings {
  std::string cpu = cvars::cpu;
  uint32_t build = cvars::kernel_build_version;
  bool debug = cvars::debug;
  std::array<std::string*, 5> values{
      &cvars::system_readonly_source_root, &cvars::logged_profile_slot_0_xuid,
      &cvars::logged_profile_slot_1_xuid, &cvars::logged_profile_slot_2_xuid,
      &cvars::logged_profile_slot_3_xuid};
  std::array<std::string, 5> saved;

  explicit KernelDataSettings(uint32_t version) {
    for (size_t i = 0; i < values.size(); ++i) {
      saved[i] = *values[i];
    }
    for (auto* value : values) {
      value->clear();
    }
    // Use Setup's existing real NullBackend path. No guest PPC is executed.
    cvars::debug = false;
    cvars::cpu = "null";
    cvars::kernel_build_version = version;
  }
  ~KernelDataSettings() {
    cvars::cpu.swap(cpu);
    cvars::debug = debug;
    cvars::kernel_build_version = build;
    for (size_t i = 0; i < values.size(); ++i) {
      values[i]->swap(saved[i]);
    }
  }
};

struct KernelDataScratch {
  std::filesystem::path path;
  KernelDataScratch() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-kernel-globals-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    // Collision is a refusal, never adoption of an existing directory.
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~KernelDataScratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

class KernelDataFixture {
 public:
  explicit KernelDataFixture(uint32_t build = 17559) : settings_(build) {
    REQUIRE(KernelState::shared() == nullptr);
    REQUIRE(std::filesystem::create_directory(scratch_.path / "content"));
    REQUIRE(std::filesystem::create_directory(scratch_.path / "cache"));
    emulator = std::make_unique<Emulator>(
        "", scratch_.path, scratch_.path / "content", scratch_.path / "cache");
    // Genuine components, no fake Kernel, null component fixture, Task/TLS, or
    // replay-purpose bypass. Only guest execution and A/V backends are unused.
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_SUCCESS);
    REQUIRE(emulator->memory());
    REQUIRE(emulator->processor());
    REQUIRE(emulator->kernel_state());
    REQUIRE(emulator->export_resolver());
    CHECK(emulator->processor()->backend()->name() == "null");
  }

  cpu::Export* Variable(uint16_t ordinal) {
    auto* entry = emulator->export_resolver()->GetExportByOrdinal(
        "xboxkrnl.exe", ordinal);
    REQUIRE(entry);
    REQUIRE(entry->get_type() == cpu::Export::Type::kVariable);
    REQUIRE(entry->is_implemented());
    REQUIRE(entry->variable_ptr != 0);
    return entry;
  }
  uint32_t Call(const char* module, uint16_t ordinal,
                std::initializer_list<uint64_t> arguments = {}) {
    REQUIRE(arguments.size() <= 8);
    auto* entry =
        emulator->export_resolver()->GetExportByOrdinal(module, ordinal);
    REQUIRE(entry);
    REQUIRE(entry->is_implemented());
    REQUIRE(entry->function_data.trampoline);
    // This is the registered HLE ABI on real Kernel/Memory, not an executed
    // guest thread or a fabricated native Task/PPC TLS context.
    PPCContext context{};
    context.virtual_membase = emulator->memory()->virtual_membase();
    std::copy(arguments.begin(), arguments.end(), context.r + 3);
    entry->function_data.trampoline(&context);
    return static_cast<uint32_t>(context.r[3]);
  }
  uint32_t Poll(uint32_t event) {
    if (!timeout_) {
      timeout_ = emulator->memory()->SystemHeapAlloc(sizeof(uint64_t));
      REQUIRE(timeout_);
      *emulator->memory()->TranslateVirtual<xe::be<uint64_t>*>(timeout_) = 0;
    }
    return Call("xboxkrnl.exe", 0xB0, {event, 3, 1, 0, timeout_});
  }
  KernelGuestGlobals* globals() {
    return emulator->memory()->TranslateVirtual<KernelGuestGlobals*>(
        emulator->kernel_state()->GetKernelGuestGlobals());
  }

 private:
  // Emulator dies first, while its exact roots and restored-later cvars live.
  KernelDataSettings settings_;
  KernelDataScratch scratch_;
  uint32_t timeout_ = 0;

 public:
  std::unique_ptr<Emulator> emulator;
};

TEST_CASE(
    "Kernel data events export owned independent healthy dispatcher bodies",
    "[kernel][kernel_globals][event]") {
  KernelDataFixture fixture;
  auto* kernel = fixture.emulator->kernel_state();
  auto* globals = fixture.globals();
  const uint32_t base = kernel->GetKernelGuestGlobals();
  const uint32_t boot = fixture.Variable(0x2DB)->variable_ptr;
  CHECK(boot ==
        base + offsetof(KernelGuestGlobals, UsbdBootEnumerationDoneEvent));
  CHECK(fixture.Poll(boot) == X_STATUS_SUCCESS);
  for (const auto [ordinal, offset] :
       std::array<std::pair<uint16_t, uint32_t>, 2>{
           {{0x2F1, offsetof(KernelGuestGlobals, UsbdDriverLoadRequiredEvent)},
            {0x2DC, offsetof(KernelGuestGlobals, StfsDeviceErrorEvent)}}}) {
    CAPTURE(ordinal);
    const uint32_t address = fixture.Variable(ordinal)->variable_ptr;
    CHECK(address == base + offset);
    CHECK(address != boot);
    CHECK(address !=
          base + offsetof(KernelGuestGlobals, XboxKernelDefaultObject));
    REQUIRE(fixture.emulator->memory()->QueryRangeAccess(address,
                                                         sizeof(X_KEVENT)) ==
            xe::memory::PageAccess::kReadWrite);
    auto* event =
        fixture.emulator->memory()->TranslateVirtual<X_KEVENT*>(address);
    CHECK(event->header.type == EventNotificationObject);
    CHECK(event->header.signal_state == 0);
    auto first = XObject::GetNativeObject<XEvent>(kernel, event);
    REQUIRE(first);
    auto second = XObject::GetNativeObject<XEvent>(kernel, event);
    REQUIRE(second);
    CHECK(first.get() == second.get());
    CHECK(first->guest_object() == address);
    CHECK(fixture.Poll(address) == X_STATUS_TIMEOUT);
    CHECK(fixture.Call("xboxkrnl.exe", 0x9D, {address, 1, 0}) == 0);
    CHECK(event->header.signal_state == 1);
    CHECK(fixture.Poll(address) == X_STATUS_SUCCESS);
    CHECK(fixture.Poll(address) ==
          X_STATUS_SUCCESS);  // Explicit manual policy.
    CHECK(fixture.Call("xboxkrnl.exe", 0x8F, {address}) == 1);
    CHECK(event->header.signal_state == 0);
    CHECK(fixture.Poll(address) == X_STATUS_TIMEOUT);
  }
  CHECK(globals->UsbdDriverLoadRequiredEvent.header.signal_state == 0);
  CHECK(globals->StfsDeviceErrorEvent.header.signal_state == 0);
  CHECK(globals->UsbdBootEnumerationDoneEvent.header.signal_state == 1);
  CHECK(fixture.Call("xboxkrnl.exe", 0x25A, {0, 5, 0}) ==
        X_STATUS_INVALID_PARAMETER);
  CHECK(globals->StfsDeviceErrorEvent.header.signal_state == 0);
  auto* required_drivers =
      fixture.emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",
                                                              0x2F2);
  REQUIRE(required_drivers);
  CHECK_FALSE(required_drivers->is_implemented());
}

TEST_CASE(
    "Kernel base and current version exports are distinct coherent BE copies",
    "[kernel][kernel_globals][version]") {
  KernelDataFixture fixture;
  auto* memory = fixture.emulator->memory();
  const uint32_t base = fixture.Variable(0x157)->variable_ptr;
  const uint32_t current = fixture.Variable(0x158)->variable_ptr;
  CHECK(base != current);
  REQUIRE(memory->QueryRangeAccess(base, 8) ==
          xe::memory::PageAccess::kReadWrite);
  REQUIRE(memory->QueryRangeAccess(current, 8) ==
          xe::memory::PageAccess::kReadWrite);
  const std::array<uint8_t, 8> expected_base{0, 2, 0, 0, 7, 0x60, 0, 0};
  const std::array<uint8_t, 8> expected_current{0, 2, 0, 0, 0x44, 0x97, 0, 0};
  CHECK(std::memcmp(memory->TranslateVirtual(base), expected_base.data(), 8) ==
        0);
  CHECK(std::memcmp(memory->TranslateVirtual(current), expected_current.data(),
                    8) == 0);
  CHECK(fixture.Call("xam.xex", 0x2A6) == 0x20076000);
  CHECK(fixture.Call("xam.xex", 0x2A7) == 0x20449700);
  CHECK(fixture.Call("xam.xex", 0x282) == 0x20449700);
  CHECK(fixture.emulator->kernel_state()->GetKernelVersion()->build == 17559);
  *memory->TranslateVirtual<xe::be<uint16_t>*>(base + 4) = 9999;
  CHECK(std::memcmp(memory->TranslateVirtual(current), expected_current.data(),
                    8) == 0);
  CHECK(fixture.Call("xam.xex", 0x2A6) == 0x20076000);
  CHECK(fixture.Call("xam.xex", 0x2A7) == 0x20449700);
  CHECK(fixture.emulator->kernel_state()->GetKernelVersion()->build == 17559);
}

TEST_CASE(
    "Kernel data events and version policy restart with genuine Kernel "
    "lifetime",
    "[kernel][kernel_globals][lifetime]") {
  for (uint32_t build : {1u, 17559u, 65535u}) {
    CAPTURE(build);
    {
      KernelDataFixture fixture(build);
      const uint32_t expected_build = std::max<uint32_t>(1888, build);
      for (uint16_t ordinal : {0x2F1, 0x2DC}) {
        const uint32_t address = fixture.Variable(ordinal)->variable_ptr;
        CHECK(fixture.Poll(address) == X_STATUS_TIMEOUT);
        CHECK(fixture.Call("xboxkrnl.exe", 0x9D, {address, 1, 0}) == 0);
        CHECK(fixture.Poll(address) == X_STATUS_SUCCESS);
        // Leave it signaled: the next Kernel must not inherit this state.
      }
      const uint32_t current = fixture.Variable(0x158)->variable_ptr;
      const uint32_t base = fixture.Variable(0x157)->variable_ptr;
      CHECK(fixture.emulator->memory()
                ->TranslateVirtual<KernelVersion*>(current)
                ->build == expected_build);
      CHECK(fixture.emulator->memory()
                ->TranslateVirtual<KernelVersion*>(base)
                ->build == 1888);
      CHECK(fixture.Call("xam.xex", 0x2A7) ==
            (0x20000000 | expected_build << 8));
      CHECK(fixture.Call("xam.xex", 0x2A6) == 0x20076000);
    }
    CHECK(KernelState::shared() == nullptr);
  }
}

}  // namespace
}  // namespace xe::kernel::xboxkrnl::test
#endif  // XE_PLATFORM_LINUX
