/**
 * Xenia : Xbox 360 Emulator Research Project
 * Adapted from ExCrypt, Copyright(c)2020 emoose, and Xbox-360-Crypto,
 * Copyright(c)2022 GoobyCorp. BSD3 notices in docs/licenses/{ExCrypt,
 * Xbox-360-Crypto}-BSD-3-Clause.txt. See pinned provenance in the source.
 */
#ifndef XENIA_KERNEL_XBOXKRNL_XECRYPT_SIGNED_HASH_H_
#define XENIA_KERNEL_XBOXKRNL_XECRYPT_SIGNED_HASH_H_
#include <array>
#include <cstdint>
#include <span>
namespace xe::kernel::xboxkrnl {
// Each input contributes whole qwords separately to a shared RotSum state;
// every byte, including each input's tail, contributes to SHA1. Full digest
// is assigned only on success. No guest memory, allocation or input mutation.
bool XeCryptComputeRotSumSha(std::span<const uint8_t> first,
                             std::span<const uint8_t> second,
                             std::array<uint8_t, 20>& digest);
// Xbox salted BeSig/Montgomery/RC4 format, distinct from ordinary PKCS1.
// Inputs are exact256/20/10/272B; key is a bounded 2048-bit XECRYPT_RSA public
// key (qword-limb layout, exponent3 or65537). All inputs remain unchanged.
bool XeCryptVerifyBeSignature(std::span<const uint8_t> signature,
                              std::span<const uint8_t> hash,
                              std::span<const uint8_t> salt,
                              std::span<const uint8_t> public_key);
}  // namespace xe::kernel::xboxkrnl
#endif
