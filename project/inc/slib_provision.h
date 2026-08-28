#ifndef SLIB_PROVISION_H
#define SLIB_PROVISION_H

/*
 * Runs the UART-confirmed sLib provisioning test. This function is only
 * linked when CMake option AT32_SLIB_PROVISION is enabled.
 */
void slib_provision_test_run(void);

#endif
