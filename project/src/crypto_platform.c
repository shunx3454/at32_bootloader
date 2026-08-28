#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "psa/crypto.h"

psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context,
    uint8_t *output,
    size_t output_size,
    size_t *output_length)
{
  (void)context;

  if (output_length == NULL || (output == NULL && output_size != 0u)) {
    return PSA_ERROR_INVALID_ARGUMENT;
  }

  if (output_size != 0u) {
    memset(output, 0, output_size);
  }
  *output_length = 0u;

  /* Never substitute a deterministic PRNG for cryptographic entropy. */
  return PSA_ERROR_INSUFFICIENT_ENTROPY;
}
