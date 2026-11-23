/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_MEDIA_CONTRACT_H_
#define XENIA_KERNEL_NATIVE_MEDIA_CONTRACT_H_
#include <cstdint>
#include <span>
#include <string_view>
namespace xe::kernel {
// Actual shared native ABI, not a host media-row/header serializer.
inline constexpr uint32_t kNativeMediaApp=0xFA;
inline constexpr uint32_t kNativeMediaSourceMetadata=0x7002B;
inline constexpr uint32_t kNativeMediaSourceStream=0x7002C;
inline constexpr uint32_t kNativeMediaDirectPayloadBytes=20;
inline constexpr uint32_t kNativeMediaSourceMetadataCapacity=10;
inline constexpr uint32_t kNativeMediaSourceMetadataBytes=0xB4;
inline constexpr uint32_t kNativeMediaFileIdentityBytes=0x23C;
inline constexpr uint32_t kNativeMediaFileDescriptorOffset=0x2C;
inline constexpr uint32_t kNativeMediaFileNameBytes=0x20A;
// Exact original B/C inputs. B+4 is a scalar filter; C+4 is an identity.
// These validate ABI bounds only, never native/source/Task readiness.
bool NativeMediaDirectOriginalArgumentsValid(uint16_t ordinal,
    std::span<const uint32_t>) noexcept;
bool NativeMediaDirectPayloadValid(uint32_t message,
                                  std::span<const uint32_t>) noexcept;
bool NativeMediaFileIdentityValid(std::span<const uint8_t>) noexcept;
inline constexpr uint32_t kNativeMediaQueryInit=0x7001C;
inline constexpr uint32_t kNativeMediaQueryRead=0x7001D;
inline constexpr uint32_t kNativeMediaQueryClose=0x7001E;
inline constexpr uint32_t kNativeMediaEnumType=0x81A85494;
inline constexpr uint32_t kNativeMediaEnumHeaderBytes=0x18;
inline constexpr uint32_t kNativeMediaQueryBytes=0x14E0;
inline constexpr uint32_t kNativeMediaRowBytes=0x334;
inline constexpr uint32_t kNativeMediaSourceBytes=0x28;
struct NativeMediaCodeProfile {
  uint32_t address,bytes;
  std::string_view sha256;
};
std::span<const NativeMediaCodeProfile> NativeMediaCodeProfiles();
const NativeMediaCodeProfile* NativeMediaCode(uint32_t address);
enum class NativeMediaOriginalCall { kCreate, kGetPrivate, kEnumerate,
                                    kMessage, kStart, kStartEx, kCancel, kCloseProcedure };
struct NativeMediaOriginalProfile {
  NativeMediaOriginalCall call;
  uint32_t address,arguments;
  bool returns_void;
};
const NativeMediaOriginalProfile& NativeMediaOriginal(NativeMediaOriginalCall);
// Original creator type/source and whole entry signatures must agree. A
// successful creation is not a 7001C initialization or ready/readable cursor.
bool NativeMediaCreateArgumentsValid(std::span<const uint32_t>) noexcept;
} // namespace xe::kernel
#endif
