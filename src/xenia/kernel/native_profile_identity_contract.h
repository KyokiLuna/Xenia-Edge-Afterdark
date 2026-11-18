/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
namespace xe::kernel {
struct ValidatedGuestRuntimePlan;
struct NativeProfileIdentityBridgeProfile {
  uint16_t ordinal;uint32_t address,bytes;std::string_view sha256;
};
std::span<const NativeProfileIdentityBridgeProfile> NativeProfileIdentityBridgeProfiles();
const NativeProfileIdentityBridgeProfile* NativeProfileIdentityBridge(uint16_t);
struct NativeProfileIdentityIngressProfile {
  uint16_t ordinal;std::string_view identity;
  uint32_t address,bytes;std::string_view sha256;
};
std::span<const NativeProfileIdentityIngressProfile> NativeProfileIdentityIngressProfiles();
enum class NativeProfileIdentityPlanFamily {kAbsent,kComplete,kInvalid};
// Explicit coherent optional family only. Guide/Avatar plans without the two
// ingress roles/extra bridge witnesses never claim the common roster factory.
NativeProfileIdentityPlanFamily CheckNativeProfileIdentityPlanFamily(
    const ValidatedGuestRuntimePlan&,std::string* error=nullptr);
} // namespace xe::kernel
