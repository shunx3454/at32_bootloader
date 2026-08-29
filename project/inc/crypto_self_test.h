#ifndef CRYPTO_SELF_TEST_H
#define CRYPTO_SELF_TEST_H

#include <stdbool.h>

/* 运行 SHA-256 已知答案测试及 ECDSA P-256 正/反例验签测试。 */
bool crypto_self_test_run(void);

/* 返回最近一次自检结果对应的串口输出字符串。 */
char const *crypto_self_test_status_string(void);

#endif
