/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <cstring>
#include <string_view>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_content_storage_driver.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
TEST_CASE("Storage ordinals register their real one-argument trampolines",
          "[kernel][native_storage_exports][abi]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t output = 0x90022000;
  REQUIRE(memory.LookupHeap(output)->AllocFixed(
      output, 0x1000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  std::array<uint8_t, 24> original;
  original.fill(0xA5);
  std::memcpy(memory.TranslateVirtual(output), original.data(),
              original.size());
  cpu::ExportResolver resolver;
  xboxkrnl::RegisterStorageExports(&resolver, nullptr);
  xboxkrnl::XboxkrnlModule::RegisterExportTable(&resolver);
  const auto* registration = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x20E);
  const auto* notification = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x210);
  for (const auto* entry : {registration, notification}) {
    REQUIRE(entry);
    REQUIRE(entry->get_type() == cpu::Export::Type::kFunction);
    REQUIRE(entry->is_implemented());
    REQUIRE_FALSE(entry->tags & cpu::ExportTag::kStub);
    REQUIRE(entry->function_data.trampoline);
  }
  REQUIRE(std::string_view(registration->name) ==
          "DrvSetContentStorageCallback");
  REQUIRE(std::string_view(notification->name) ==
          "DrvGetContentStorageNotification");

  // An absent owned runtime must reject rather than register an arbitrary
  // callback or manufacture an empty success. Extra registers are not hidden
  // output arguments and must never be dereferenced or consumed.
  PPCContext context{};
  context.virtual_membase = memory.virtual_membase();
  context.r[3] = NativeContentStorageDriver::kNativeCallback;
  context.r[4] = 0xFEDCBA9876543210ull;
  context.r[5] = 0xFFFFFFFF;
  registration->function_data.trampoline(&context);
  REQUIRE(uint32_t(context.r[3]) == X_STATUS_NOT_SUPPORTED);
  REQUIRE(context.r[4] == 0xFEDCBA9876543210ull);

  for (const uint32_t pointer : {output, 0u, 0xFFFFFFFFu}) {
    context.r[3] = pointer;
    notification->function_data.trampoline(&context);
    REQUIRE(uint32_t(context.r[3]) == 0xC000009D);
    REQUIRE(context.r[4] == 0xFEDCBA9876543210ull);
    REQUIRE(std::memcmp(memory.TranslateVirtual(output), original.data(),
                        original.size()) == 0);
  }
}
}  // namespace xe::kernel::test
