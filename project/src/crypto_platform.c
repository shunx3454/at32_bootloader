#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "psa/crypto.h"

psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context, uint8_t *output,
                                             size_t output_size, size_t *output_length) {
    (void)context;

    if (output_length == NULL || (output == NULL && output_size != 0u)) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    if (output_size != 0u) {
        memset(output, 0, output_size);
    }
    *output_length = 0u;

    /*
     * 当前 Bootloader 只做 SHA-256 和公钥验签，不需要随机数。
     * 若将来增加密钥生成/签名/随机挑战，必须接入真实硬件熵源；这里明确返回失败，
     * 绝不能用确定性伪随机数据冒充密码学随机数。
     */
    return PSA_ERROR_INSUFFICIENT_ENTROPY;
}
