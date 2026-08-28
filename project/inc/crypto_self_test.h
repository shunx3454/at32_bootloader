#ifndef CRYPTO_SELF_TEST_H
#define CRYPTO_SELF_TEST_H

#include <stdbool.h>

bool crypto_self_test_run(void);
char const *crypto_self_test_status_string(void);

#endif
