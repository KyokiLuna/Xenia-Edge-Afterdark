/**
 * Xenia : Xbox 360 Emulator Research Project
 * Adapted from ExCrypt, Copyright(c)2020 emoose, and Xbox-360-Crypto,
 * Copyright(c)2022 GoobyCorp. BSD3 notices in docs/licenses/{ExCrypt,
 * Xbox-360-Crypto}-BSD-3-Clause.txt.
 * Primary references:
 * github.com/emoose/ExCrypt/blob/088094585aee1694cdeef9a9330d29d14aef9203/src/excrypt_rotsum.c
 * github.com/emoose/ExCrypt/blob/088094585aee1694cdeef9a9330d29d14aef9203/src/excrypt_bn_sig.c
 * github.com/GoobyCorp/Xbox-360-Crypto/blob/ad62bee90e0342cbfe482a4dddf60f9a0f456660/XeCrypt.py
 */
#include "xenia/kernel/xboxkrnl/xecrypt_signed_hash.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include "third_party/mbedtls/include/mbedtls/bignum.h"
// This vendored header has no C++ linkage guard of its own.
extern "C" {
#include "third_party/mbedtls/include/mbedtls/constant_time.h"
}
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "xenia/kernel/xboxkrnl/xecrypt_rc4.h"
namespace xe::kernel::xboxkrnl {
namespace {
uint32_t Read32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
uint64_t Read64(const uint8_t* p) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8) | p[i];
  }
  return value;
}
void RotSum(std::array<uint64_t, 4>& state, std::span<const uint8_t> bytes) {
  for (size_t i = 0; i < bytes.size() / 8; ++i) {
    const uint64_t data = Read64(bytes.data() + i * 8);
    state[1] += data;
    state[3] -= data;
    state[0] += state[1] < data;
    state[2] -= state[3] > data;
    state[1] = std::rotl(state[1], 29);
    state[3] = std::rotl(state[3], 31);
  }
}
struct Sha {
  mbedtls_sha1_context state;
  Sha() { mbedtls_sha1_init(&state); }
  ~Sha() { mbedtls_sha1_free(&state); }
  bool Update(std::span<const uint8_t> data) {
    return data.empty() ||
           mbedtls_sha1_update(&state, data.data(), data.size()) == 0;
  }
};
struct Mpi {
  mbedtls_mpi value;
  Mpi() { mbedtls_mpi_init(&value); }
  ~Mpi() { mbedtls_mpi_free(&value); }
};
void QwordsToBigEndian(std::span<const uint8_t, 256> limbs,
                       std::array<uint8_t, 256>& output) {
  for (size_t i = 0; i < 32; ++i) {
    std::copy_n(limbs.data() + (31 - i) * 8, 8, output.data() + i * 8);
  }
}
}  // namespace
bool XeCryptComputeRotSumSha(std::span<const uint8_t> first,
                             std::span<const uint8_t> second,
                             std::array<uint8_t, 20>& digest) {
  std::array<uint64_t, 4> state{};
  RotSum(state, first);
  RotSum(state, second);
  std::array<uint8_t, 32> encoded;
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 8; ++j) {
      encoded[i * 8 + j] = uint8_t(state[i] >> ((7 - j) * 8));
    }
  }
  Sha sha;
  if (mbedtls_sha1_starts(&sha.state) || !sha.Update(encoded) ||
      !sha.Update(encoded) || !sha.Update(first) || !sha.Update(second)) {
    return false;
  }
  for (auto& value : encoded) {
    value ^= 0xFF;
  }
  std::array<uint8_t, 20> result;
  if (!sha.Update(encoded) || !sha.Update(encoded) ||
      mbedtls_sha1_finish(&sha.state, result.data())) {
    return false;
  }
  digest = result;
  return true;
}
bool XeCryptVerifyBeSignature(std::span<const uint8_t> signature,
                              std::span<const uint8_t> hash,
                              std::span<const uint8_t> salt,
                              std::span<const uint8_t> key) {
  if (signature.size() != 256 || hash.size() != 20 || salt.size() != 10 ||
      key.size() != 272 || Read32(key.data()) != 32) {
    return false;
  }
  const uint32_t exponent = Read32(key.data() + 4);
  if (exponent != 3 && exponent != 65537) {
    return false;
  }
  std::array<uint8_t, 256> modulus_bytes, signature_bytes, block;
  QwordsToBigEndian(std::span<const uint8_t, 256>(key.data() + 16, 256),
                    modulus_bytes);
  QwordsToBigEndian(std::span<const uint8_t, 256>(signature.data(), 256),
                    signature_bytes);
  // Require the declared 2048-bit public modulus, odd and normalized, and a
  // canonical signature integer. Never reduce malformed signatures modulo N.
  if (!(modulus_bytes[0] & 0x80) || !(modulus_bytes[255] & 1)) {
    return false;
  }
  Mpi modulus, encoded, e, decoded, two, domain_exponent, domain, inverse,
      product;
  if (mbedtls_mpi_read_binary(&modulus.value, modulus_bytes.data(), 256) ||
      mbedtls_mpi_read_binary(&encoded.value, signature_bytes.data(), 256) ||
      mbedtls_mpi_cmp_mpi(&encoded.value, &modulus.value) >= 0 ||
      mbedtls_mpi_lset(&e.value, exponent) ||
      mbedtls_mpi_exp_mod(&decoded.value, &encoded.value, &e.value,
                          &modulus.value, nullptr) ||
      mbedtls_mpi_lset(&two.value, 2) ||
      mbedtls_mpi_lset(&domain_exponent.value, (exponent - 1) * 2048u) ||
      mbedtls_mpi_exp_mod(&domain.value, &two.value, &domain_exponent.value,
                          &modulus.value, nullptr) ||
      mbedtls_mpi_inv_mod(&inverse.value, &domain.value, &modulus.value) ||
      mbedtls_mpi_mul_mpi(&product.value, &decoded.value, &inverse.value) ||
      mbedtls_mpi_mod_mpi(&decoded.value, &product.value, &modulus.value) ||
      mbedtls_mpi_write_binary(&decoded.value, block.data(), 256)) {
    return false;
  }
  std::array<uint8_t, 20> expected;
  const std::array<uint8_t, 8> zeros{};
  Sha sha;
  if (mbedtls_sha1_starts(&sha.state) || !sha.Update(zeros) ||
      !sha.Update(hash) || !sha.Update(salt) ||
      mbedtls_sha1_finish(&sha.state, expected.data())) {
    return false;
  }
  // ExCrypt's native-compatible Difference formats the entire expected block,
  // including the masked first ciphertext byte, then compares all256 bytes.
  // Merely decrypting and skipping a padding byte would be a weaker check.
  std::array<uint8_t, 256> formatted{};
  formatted[0xE0] = 1;
  std::copy(salt.begin(), salt.end(), formatted.begin() + 0xE1);
  XECRYPT_RC4_STATE rc4;
  if (!XeCryptRc4Key(rc4, expected)) {
    return false;
  }
  XeCryptRc4Ecb(rc4, std::span<uint8_t>(formatted).first(0xEB));
  formatted[0] &= 0x7F;
  std::copy(expected.begin(), expected.end(), formatted.begin() + 0xEB);
  formatted[0xFF] = 0xBC;
  return mbedtls_ct_memcmp(block.data(), formatted.data(), formatted.size()) ==
         0;
}
}  // namespace xe::kernel::xboxkrnl
