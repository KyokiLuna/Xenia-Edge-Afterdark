/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xboxkrnl/xecrypt_signed_hash_guest.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/testing/signed_hash_vectors.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"
namespace xe::kernel::xboxkrnl::test {
namespace {
std::vector<uint8_t> Hex(std::string_view text) {
  auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < text.size(); i += 2) {
    bytes.push_back(uint8_t((digit(text[i]) << 4) | digit(text[i + 1])));
  }
  return bytes;
}
struct Fixture {
  static constexpr uint32_t base = 0x90040000, extent = 0x20000,
                            signature = base + 0x100, hash = base + 0x300,
                            salt = base + 0x400, key = base + 0x500,
                            first = base + 0x900, second = base + 0xA00,
                            output = base + 0x1FF0, physical = 0xE1000000;
  Memory memory;
  void* provider = nullptr;
  uint32_t calls = 0;
  bool unlocked = false;
  Fixture() {
    REQUIRE(memory.Initialize());
    REQUIRE(memory.LookupHeap(base)->AllocFixed(
        base - 0x1000, extent + 0x2000, 0x1000, kMemoryAllocationReserve,
        kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(memory.LookupHeap(base)->AllocFixed(
        base, extent, 0x1000, kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(base), 0xA5, extent);
  }
  ~Fixture() {
    if (provider) {
      memory.UnregisterPhysicalMemoryReadCallback(provider);
    }
  }
  void Put(uint32_t at, const std::vector<uint8_t>& bytes) {
    std::memcpy(memory.TranslateVirtual(at), bytes.data(), bytes.size());
  }
  std::vector<uint8_t> Before() {
    return {memory.TranslateVirtual(base),
            memory.TranslateVirtual(base) + extent};
  }
  void Signature() {
    Put(signature, Hex(kBeSigValid3));
    Put(hash, Hex(kBeSigHash));
    Put(salt, Hex(kBeSigSalt));
    Put(key, Hex(kBeSigKey3));
  }
  void PhysicalProvider() {
    memory.SetPhysicalAliasSkipHostProtect(true);
    REQUIRE(memory.LookupHeap(physical)->AllocFixed(
        physical, 0x1000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    provider = memory.RegisterPhysicalMemoryReadCallback(
        [](void* data, uint32_t start, uint32_t size,
           Memory::PhysicalAccess access) {
          auto& f = *static_cast<Fixture*>(data);
          const auto physical_address = f.memory.GetPhysicalAddress(physical);
          if (start >= physical_address + 0x1000 ||
              uint64_t(start) + size <= physical_address) {
            return;
          }
          ++f.calls;
          f.unlocked |= !global_critical_region::is_held_by_current_thread();
          if (access != Memory::PhysicalAccess::kDiscard) {
            std::memset(f.memory.TranslatePhysical(physical_address), 0x31,
                        0x1000);
          }
          REQUIRE(f.memory.LookupHeap(output)->Protect(output & ~0xFFFu, 0x1000,
                                                       kMemoryProtectRead));
        },
        this);
    memory.EnablePhysicalMemoryAccessCallbacks(
        memory.GetPhysicalAddress(physical), 0x1000, true, true);
  }
};
}  // namespace
TEST_CASE(
    "RotSum guest publication stages two spans, permits aliases and caps "
    "digest writes",
    "[kernel][crypt][signed_hash]") {
  const auto first = Hex("000102030405060708090a0b0c0d0e0f10");
  // Independent expected vector includes the four-byte/incomplete-qword tails.
  const auto expected = Hex("ad48b464ce8fab68075fedcf065322babcea8593");
  for (uint32_t size : {0u, 1u, 19u, 20u, 21u, UINT32_MAX}) {
    Fixture f;
    f.Put(Fixture::first, first);
    const auto before = f.Before();
    REQUIRE(XeCryptRotSumShaGuest(f.memory, Fixture::first, first.size(),
                                  0xFFFFFFFF, 0, Fixture::output,
                                  size) == X_STATUS_SUCCESS);
    auto after = before;
    std::copy_n(expected.begin(), std::min(size, 20u),
                after.begin() + Fixture::output - Fixture::base);
    CHECK(f.Before() == after);
  }
  Fixture f;
  f.Put(Fixture::first, first);
  REQUIRE(XeCryptRotSumShaGuest(f.memory, Fixture::first, first.size(), 0, 0,
                                Fixture::first, 20) == X_STATUS_SUCCESS);
  CHECK(std::equal(expected.begin(), expected.end(),
                   f.memory.TranslateVirtual(Fixture::first)));
}
TEST_CASE(
    "RotSum guest inputs keep independent qword tails and zero spans have no "
    "access",
    "[kernel][crypt][signed_hash]") {
  Fixture f;
  const auto
      first =
          Hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e"),
      second = Hex("000102030405060708"),
      expected = Hex("a80589d24a65a39d1327a556b41ae3e819c699f7");
  f.Put(Fixture::first, first);
  f.Put(Fixture::second, second);
  const auto before = f.Before();
  REQUIRE(XeCryptRotSumShaGuest(f.memory, Fixture::first, first.size(),
                                Fixture::second, second.size(), Fixture::output,
                                20) == X_STATUS_SUCCESS);
  auto wanted = before;
  std::copy(expected.begin(), expected.end(),
            wanted.begin() + Fixture::output - Fixture::base);
  CHECK(f.Before() == wanted);
  CHECK(XeCryptRotSumShaGuest(f.memory, UINT32_MAX, 0, UINT32_MAX, 0,
                              UINT32_MAX, 0) == X_STATUS_SUCCESS);
  CHECK(f.Before() == wanted);
}
TEST_CASE("RotSum publishes only the digest at an actual writable guard edge",
          "[kernel][crypt][signed_hash]") {
  const auto expected = Hex("c1f48a59c2629f48b955483785512db79040b3c1");
  for (uint32_t capacity : {21u, UINT32_MAX}) {
    Fixture f;
    const auto before = f.Before();
    const uint32_t destination = Fixture::base + Fixture::extent - 20;
    REQUIRE(XeCryptRotSumShaGuest(f.memory, UINT32_MAX, 0, UINT32_MAX, 0,
                                  destination, capacity) == X_STATUS_SUCCESS);
    auto wanted = before;
    std::copy(expected.begin(), expected.end(), wanted.end() - 20);
    CHECK(f.Before() == wanted);
  }
  Fixture f;
  const auto before = f.Before();
  CHECK(XeCryptRotSumShaGuest(f.memory, 0, 0, 0, 0,
                              Fixture::base + Fixture::extent - 19,
                              20) == X_STATUS_ACCESS_VIOLATION);
  CHECK(f.Before() == before);
}
TEST_CASE(
    "RotSum invalid source/output and outer critical region cannot partially "
    "publish",
    "[kernel][crypt][signed_hash]") {
  Fixture f;
  const auto before = f.Before();
  CHECK(XeCryptRotSumShaGuest(f.memory, Fixture::first, 8, 0xFFFFFFFC, 8,
                              Fixture::output,
                              20) == X_STATUS_ACCESS_VIOLATION);
  CHECK(f.Before() == before);
  REQUIRE(f.memory.LookupHeap(Fixture::output)
              ->Protect(Fixture::output & ~0xFFFu, 0x1000, kMemoryProtectRead));
  CHECK(XeCryptRotSumShaGuest(f.memory, Fixture::first, 8, 0, 0,
                              Fixture::output,
                              20) == X_STATUS_ACCESS_VIOLATION);
  CHECK(f.Before() == before);
  auto held = global_critical_region::AcquireDirect();
  CHECK(XeCryptRotSumShaGuest(f.memory, Fixture::first, 8, 0, 0,
                              Fixture::output,
                              20) == X_STATUS_INVALID_DEVICE_REQUEST);
  CHECK(global_critical_region::is_held_by_current_thread());
  CHECK(f.Before() == before);
}
TEST_CASE("RotSum revalidates output after genuine physical provider wait",
          "[kernel][crypt][signed_hash][physical]") {
  Fixture f;
  f.PhysicalProvider();
  const auto before = f.Before();
  CHECK(XeCryptRotSumShaGuest(f.memory, Fixture::physical, 32, 0, 0,
                              Fixture::output,
                              20) == X_STATUS_ACCESS_VIOLATION);
  CHECK(f.calls > 0);
  CHECK(f.unlocked);
  CHECK(f.Before() == before);
  // The provider's physical refresh is legitimate; it is not hash publication.
  CHECK(f.memory.TranslatePhysical(
            f.memory.GetPhysicalAddress(Fixture::physical))[0] == 0x31);
}
TEST_CASE(
    "BeSig guest adapter validates readonly inputs without modifying them",
    "[kernel][crypt][signed_hash]") {
  Fixture f;
  f.Signature();
  const auto before = f.Before();
  REQUIRE(f.memory.LookupHeap(Fixture::base)
              ->Protect(Fixture::base, 0x1000, kMemoryProtectRead));
  REQUIRE(XeCryptBeSigVerifyGuest(f.memory, Fixture::signature, Fixture::hash,
                                  Fixture::salt, Fixture::key));
  CHECK(f.Before() == before);
  CHECK_FALSE(XeCryptBeSigVerifyGuest(f.memory, 0xFFFFFFF0, Fixture::hash,
                                      Fixture::salt, Fixture::key));
  CHECK_FALSE(XeCryptBeSigVerifyGuest(f.memory, Fixture::signature,
                                      Fixture::hash, Fixture::salt, 0));
  auto held = global_critical_region::AcquireDirect();
  CHECK_FALSE(XeCryptBeSigVerifyGuest(f.memory, Fixture::signature,
                                      Fixture::hash, Fixture::salt,
                                      Fixture::key));
  CHECK(global_critical_region::is_held_by_current_thread());
  CHECK(f.Before() == before);
}
TEST_CASE(
    "BeSig header length cannot read beyond its actual guest key allocation",
    "[kernel][crypt][signed_hash]") {
  Fixture f;
  f.Signature();
  const auto header = Hex(kBeSigKey3);
  const uint32_t end = Fixture::base + Fixture::extent - 16;
  std::memcpy(f.memory.TranslateVirtual(end), header.data(), 16);
  const auto before = f.Before();
  CHECK_FALSE(XeCryptBeSigVerifyGuest(f.memory, Fixture::signature,
                                      Fixture::hash, Fixture::salt, end));
  CHECK(f.Before() == before);
}
TEST_CASE(
    "Signed hash exports register exact actual DWORD/void and BOOL trampolines",
    "[kernel][crypt][signed_hash][abi]") {
  Fixture f;
  cpu::ExportResolver resolver;
  RegisterCryptExports(&resolver, nullptr);
  XboxkrnlModule::RegisterExportTable(&resolver);
  const auto* rot = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x18E);
  const auto* sig = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x166);
  REQUIRE(rot);
  REQUIRE(sig);
  REQUIRE(rot->function_data.trampoline);
  REQUIRE(sig->function_data.trampoline);
  CHECK(rot->is_implemented());
  CHECK(sig->is_implemented());
  CHECK((rot->tags & cpu::ExportTag::kStub) == 0);
  CHECK((sig->tags & cpu::ExportTag::kStub) == 0);
  PPCContext context{};
  context.virtual_membase = f.memory.virtual_membase();
  context.lr = 0xABCDEF0123456789ull;
  for (size_t i = 0; i < 32; ++i) {
    context.r[i] = 0xFEEDBEEF00000000ull + i;
  }
  context.r[4] = 0xFFFFFFFF00000000ull;
  context.r[6] = 0x1234567800000000ull;
  context.r[8] = 0xABCDEF0000000000ull;
  std::array<uint64_t, 32> original;
  std::copy_n(context.r, 32, original.begin());
  const auto before = f.Before();
  rot->function_data.trampoline(&context);
  for (size_t i = 0; i < 32; ++i) {
    CHECK(context.r[i] == original[i]);
  }
  CHECK(context.lr == 0xABCDEF0123456789ull);
  CHECK(f.Before() == before);
  // No fabricated KernelState: unavailable original caller must fail BOOL.
  sig->function_data.trampoline(&context);
  CHECK(context.r[3] == 0);
  for (size_t i = 0; i < 32; ++i) {
    if (i != 3) {
      CHECK(context.r[i] == original[i]);
    }
  }
  CHECK(f.Before() == before);
}
}  // namespace xe::kernel::xboxkrnl::test
