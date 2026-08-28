#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crypto_self_test.h"
#include "psa/crypto.h"

static bool self_test_passed;

static bool sha256_known_answer_test(void)
{
  static uint8_t const message[] = "abc";
  static uint8_t const expected[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
  };
  uint8_t digest[32];
  size_t digest_length = 0u;

  psa_status_t const status = psa_hash_compute(
      PSA_ALG_SHA_256,
      message,
      sizeof(message) - 1u,
      digest,
      sizeof(digest),
      &digest_length);

  return status == PSA_SUCCESS &&
         digest_length == sizeof(expected) &&
         memcmp(digest, expected, sizeof(expected)) == 0;
}

static bool ecdsa_p256_verify_test(void)
{
  /* RFC 6979 A.2.5: ECDSA P-256/SHA-256, message "sample". */
  static uint8_t const message[] = "sample";
  static uint8_t const public_key[65] = {
    0x04,
    0x60, 0xfe, 0xd4, 0xba, 0x25, 0x5a, 0x9d, 0x31,
    0xc9, 0x61, 0xeb, 0x74, 0xc6, 0x35, 0x6d, 0x68,
    0xc0, 0x49, 0xb8, 0x92, 0x3b, 0x61, 0xfa, 0x6c,
    0xe6, 0x69, 0x62, 0x2e, 0x60, 0xf2, 0x9f, 0xb6,
    0x79, 0x03, 0xfe, 0x10, 0x08, 0xb8, 0xbc, 0x99,
    0xa4, 0x1a, 0xe9, 0xe9, 0x56, 0x28, 0xbc, 0x64,
    0xf2, 0xf1, 0xb2, 0x0c, 0x2d, 0x7e, 0x9f, 0x51,
    0x77, 0xa3, 0xc2, 0x94, 0xd4, 0x46, 0x22, 0x99
  };
  static uint8_t const signature[64] = {
    0xef, 0xd4, 0x8b, 0x2a, 0xac, 0xb6, 0xa8, 0xfd,
    0x11, 0x40, 0xdd, 0x9c, 0xd4, 0x5e, 0x81, 0xd6,
    0x9d, 0x2c, 0x87, 0x7b, 0x56, 0xaa, 0xf9, 0x91,
    0xc3, 0x4d, 0x0e, 0xa8, 0x4e, 0xaf, 0x37, 0x16,
    0xf7, 0xcb, 0x1c, 0x94, 0x2d, 0x65, 0x7c, 0x41,
    0xd4, 0x36, 0xc7, 0xa1, 0xb6, 0xe2, 0x9f, 0x65,
    0xf3, 0xe9, 0x00, 0xdb, 0xb9, 0xaf, 0xf4, 0x06,
    0x4d, 0xc4, 0xab, 0x2f, 0x84, 0x3a, 0xcd, 0xa8
  };
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t key = 0;
  uint8_t digest[32];
  uint8_t invalid_signature[sizeof(signature)];
  size_t digest_length = 0u;
  bool passed = false;

  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
  psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attributes, 256u);

  psa_status_t status = psa_import_key(&attributes,
                                       public_key,
                                       sizeof(public_key),
                                       &key);
  if (status != PSA_SUCCESS) {
    goto cleanup;
  }

  status = psa_hash_compute(PSA_ALG_SHA_256,
                            message,
                            sizeof(message) - 1u,
                            digest,
                            sizeof(digest),
                            &digest_length);
  if (status != PSA_SUCCESS || digest_length != sizeof(digest)) {
    goto cleanup;
  }

  status = psa_verify_hash(key,
                           PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                           digest,
                           digest_length,
                           signature,
                           sizeof(signature));
  if (status != PSA_SUCCESS) {
    goto cleanup;
  }

  memcpy(invalid_signature, signature, sizeof(invalid_signature));
  invalid_signature[0] ^= 0x01u;
  status = psa_verify_hash(key,
                           PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                           digest,
                           digest_length,
                           invalid_signature,
                           sizeof(invalid_signature));
  passed = status == PSA_ERROR_INVALID_SIGNATURE;

cleanup:
  if (key != 0u) {
    (void)psa_destroy_key(key);
  }
  psa_reset_key_attributes(&attributes);
  return passed;
}

bool crypto_self_test_run(void)
{
  self_test_passed = psa_crypto_init() == PSA_SUCCESS &&
                     sha256_known_answer_test() &&
                     ecdsa_p256_verify_test();
  return self_test_passed;
}

char const *crypto_self_test_status_string(void)
{
  return self_test_passed
             ? "Mbed TLS PSA SHA-256/ECDSA-P256 self-test: PASS\r\n"
             : "Mbed TLS PSA SHA-256/ECDSA-P256 self-test: FAIL\r\n";
}
