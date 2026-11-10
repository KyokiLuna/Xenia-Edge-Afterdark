#ifndef XECRYPT_RSA_H_
#define XECRYPT_RSA_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "third_party/mbedtls/include/mbedtls/bignum.h"
#include "third_party/mbedtls/include/mbedtls/rsa.h"

// Performs RSA public-key encryption/decryption matching
// XeCryptBnQwNeRsaPubCrypt. All buffers use the Xbox 360 layout: big-endian
// uint64 limbs in little-endian limb order.
//
// qw_a:    input data (num_qwords * 8 bytes)
// qw_b:    output buffer (num_qwords * 8 bytes)
// modulus: RSA modulus following the XECRYPT_RSA header (num_qwords * 8 bytes)
// num_qwords: number of 64-bit limbs
// exponent:   public exponent
//
// Returns 1 on success, 0 on failure.
inline uint32_t XeCryptBnQwNeRsaPubCrypt(const uint8_t* qw_a, uint8_t* qw_b,
                                         const uint8_t* modulus,
                                         uint32_t num_qwords,
                                         uint32_t exponent) {
  // Reject keys below 512 bits (8 qwords) to match original BCrypt behavior.
  if (num_qwords < 8) {
    return 0;
  }

  uint32_t modulus_size = num_qwords * 8;

  auto input_be = std::vector<uint8_t>(modulus_size);
  auto mod_be = std::vector<uint8_t>(modulus_size);

  // Reverse qword order to produce big-endian byte arrays
  for (uint32_t i = 0; i < num_qwords; i++) {
    std::memcpy(&input_be[i * 8], &qw_a[(num_qwords - 1 - i) * 8], 8);
    std::memcpy(&mod_be[i * 8], &modulus[(num_qwords - 1 - i) * 8], 8);
  }

  mbedtls_mpi base, exp, mod, result;
  mbedtls_mpi_init(&base);
  mbedtls_mpi_init(&exp);
  mbedtls_mpi_init(&mod);
  mbedtls_mpi_init(&result);
  auto result_be = std::vector<uint8_t>(modulus_size);
  const bool valid =
      !mbedtls_mpi_read_binary(&base, input_be.data(), modulus_size) &&
      !mbedtls_mpi_read_binary(&mod, mod_be.data(), modulus_size) &&
      !mbedtls_mpi_lset(&exp, exponent) &&
      !mbedtls_mpi_exp_mod(&result, &base, &exp, &mod, nullptr) &&
      !mbedtls_mpi_write_binary(&result, result_be.data(), modulus_size);
  mbedtls_mpi_free(&base);
  mbedtls_mpi_free(&exp);
  mbedtls_mpi_free(&mod);
  mbedtls_mpi_free(&result);
  if (!valid) {
    return 0;
  }

  // Convert back to Xbox format: reverse qword order
  for (uint32_t i = 0; i < num_qwords; i++) {
    std::memcpy(&qw_b[i * 8], &result_be[(num_qwords - 1 - i) * 8], 8);
  }

  return 1;
}

// Verifies an SHA1 PKCS#1 v1.5 signature against a bounded XECRYPT_RSA public
// key blob. The modulus uses big-endian qwords in little-endian qword order;
// the signature is a little-endian integer (not the modulus's qword layout).
// This pure helper does not read guest memory or change any input bytes.
inline bool XeCryptVerifyRsaSha1Pkcs1(std::span<const uint8_t> public_key,
                                      std::span<const uint8_t> hash,
                                      std::span<const uint8_t> signature) {
  if (public_key.size() < 0x10 || hash.size() != 20) {
    return false;
  }
  const uint32_t num_qwords = (uint32_t(public_key[0]) << 24) |
                              (uint32_t(public_key[1]) << 16) |
                              (uint32_t(public_key[2]) << 8) | public_key[3];
  // Bound every conversion and the crypto library's allocations, independently
  // of the untrusted header. Existing Xbox RSA helpers support 512+ bit keys.
  if (num_qwords < 8 || num_qwords > 64) {
    return false;
  }
  const size_t modulus_size = size_t(num_qwords) * 8;
  if (public_key.size() != 0x10 + modulus_size ||
      signature.size() != modulus_size) {
    return false;
  }
  std::array<uint8_t, 512> modulus_be{}, signature_be{};
  for (uint32_t i = 0; i < num_qwords; ++i) {
    std::memcpy(modulus_be.data() + i * 8,
                public_key.data() + 0x10 + (num_qwords - 1 - i) * 8, 8);
  }
  std::reverse_copy(signature.begin(), signature.end(), signature_be.begin());
  mbedtls_rsa_context context;
  mbedtls_rsa_init(&context);
  const bool valid =
      !mbedtls_rsa_import_raw(&context, modulus_be.data(), modulus_size,
                              nullptr, 0, nullptr, 0, nullptr, 0,
                              public_key.data() + 4, 4) &&
      !mbedtls_rsa_complete(&context) && !mbedtls_rsa_check_pubkey(&context) &&
      mbedtls_rsa_get_len(&context) == modulus_size &&
      !mbedtls_rsa_set_padding(&context, MBEDTLS_RSA_PKCS_V15,
                               MBEDTLS_MD_SHA1) &&
      // This also rejects signature integers >= N, instead of silently reducing
      // them, and compares the complete encoded padding/DigestInfo/hash block.
      !mbedtls_rsa_pkcs1_verify(&context, MBEDTLS_MD_SHA1, 20, hash.data(),
                                signature_be.data());
  mbedtls_rsa_free(&context);
  return valid;
}

#endif  // XECRYPT_RSA_H_
