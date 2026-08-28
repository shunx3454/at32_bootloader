#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "at32f403a_407_conf.h"
#include "slib_provision.h"
#include "wk_system.h"

#if !defined(AT32_SLIB_PROVISION_PASSWORD)
#error "AT32_SLIB_PROVISION_PASSWORD is required for an sLib provision build"
#endif

#define SLIB_INSTRUCTION_SECTOR  62u
#define SLIB_DATA_SECTOR         63u
#define SLIB_INSTRUCTION_ADDRESS 0x0801F000u
#define SLIB_DATA_ADDRESS        0x0801F800u
#define SLIB_SECTOR_SIZE         0x800u
#define SLIB_CONFIRM_TIMEOUT_MS  60000u

/*
 * Public, non-secret provision record written into SLIB_DATA. The payload is
 * the product P-256 public key in SEC1 uncompressed form, copied from
 * /home/qxun/secure_keys/ecdsa_public_sec1.bin. The private key is never
 * included in this firmware.
 *
 *  0..3   magic "SLIB"
 *  4      record version (1)
 *  5      encoding (2 = SEC1 uncompressed)
 *  6..7   key length (65, little-endian)
 *  8..11  test key id (1, little-endian)
 * 12..76  0x04 || X || Y
 * 77..79  erased padding
 */
static uint8_t const slib_test_record[80] = {
  0x53, 0x4c, 0x49, 0x42, 0x01, 0x02, 0x41, 0x00,
  0x01, 0x00, 0x00, 0x00,
  0x04,
  0x41, 0x84, 0x3d, 0x2f, 0xa3, 0x9d, 0x48, 0xaf,
  0xfc, 0x63, 0x50, 0xfd, 0x49, 0x35, 0x21, 0x1e,
  0x9d, 0x40, 0xab, 0xc5, 0xf1, 0xc6, 0xd1, 0x50,
  0xa8, 0x51, 0x63, 0xf9, 0x08, 0xf5, 0x15, 0xc1,
  0x75, 0x21, 0x75, 0xa1, 0xc1, 0xde, 0x8b, 0x41,
  0xf6, 0x0b, 0xfb, 0x86, 0x92, 0x1b, 0xc5, 0x63,
  0x0c, 0x96, 0x56, 0x90, 0xb1, 0xfa, 0x37, 0x4d,
  0xc2, 0xd5, 0x86, 0x6c, 0xaa, 0x7a, 0x74, 0xa4,
  0xff, 0xff, 0xff
};

_Static_assert(sizeof(slib_test_record) == 80u,
               "sLib test record layout must remain exactly 80 bytes");

static bool slib_record_matches(void)
{
  void const *const stored = (void const *)(uintptr_t)SLIB_DATA_ADDRESS;
  return memcmp(stored, slib_test_record, sizeof(slib_test_record)) == 0;
}

static bool slib_range_matches(void)
{
  return flash_slib_start_sector_get() == SLIB_INSTRUCTION_SECTOR &&
         flash_slib_datastart_sector_get() == SLIB_DATA_SECTOR &&
         flash_slib_end_sector_get() == SLIB_DATA_SECTOR;
}

static bool uart_confirmation_received(void)
{
  static char const confirmation[] = "PROVISION SLIB";
  static uint32_t const receive_error_mask = USART_PERR_FLAG |
                                              USART_FERR_FLAG |
                                              USART_NERR_FLAG |
                                              USART_ROERR_FLAG;
  size_t matched = 0u;
  uint32_t received_count = 0u;
  uint32_t receive_error_count = 0u;
  uint32_t const start_tick = wk_timebase_get();

  /*
   * Poll continuously while receiving. At 115200 baud a character arrives
   * about every 87 us; a 1 ms delay here would overflow the one-byte USART
   * receive register and discard most of a pasted command.
   */
  while ((wk_timebase_get() - start_tick) < SLIB_CONFIRM_TIMEOUT_MS) {
    uint32_t const status = USART1->sts;

    if ((status & (USART_RDBF_FLAG | receive_error_mask)) != 0u) {
      char const received = (char)usart_data_receive(USART1);

      if ((status & receive_error_mask) != 0u) {
        ++receive_error_count;
        matched = 0u;
        continue;
      }

      ++received_count;
      if (received == confirmation[matched]) {
        ++matched;
        if (matched == sizeof(confirmation) - 1u) {
          return true;
        }
      } else {
        matched = received == confirmation[0] ? 1u : 0u;
      }
    }
  }

  printf("[SLIB] RX timeout: received=%lu, errors=%lu, matched=%u/%u bytes.\r\n",
         (unsigned long)received_count,
         (unsigned long)receive_error_count,
         (unsigned int)matched,
         (unsigned int)(sizeof(confirmation) - 1u));
  return false;
}

static void slib_fatal(char const *operation, flash_status_type status)
{
  flash_lock();
  printf("[SLIB] FAIL: %s, flash status=%u\r\n",
         operation,
         (unsigned int)status);
  printf("[SLIB] Do not reset or remove power; configuration may be pending.\r\n");
  while (1) {
  }
}

static void slib_program_record(void)
{
  flash_status_type status;

  flash_unlock();

  /* Avoid the BSP helper's unbounded unlock wait if the MCU rejects it. */
  FLASH->slib_unlock = SLIB_UNLOCK_KEY;
  if (FLASH->slib_misc_sts_bit.slib_ulkf == RESET) {
    slib_fatal("unlock sLib configuration", FLASH_OPERATE_TIMEOUT);
  }

  status = flash_slib_enable((uint32_t)AT32_SLIB_PROVISION_PASSWORD,
                             SLIB_INSTRUCTION_SECTOR,
                             SLIB_DATA_SECTOR,
                             SLIB_DATA_SECTOR);
  if (status != FLASH_OPERATE_DONE) {
    slib_fatal("configure range/password", status);
  }

  status = flash_sector_erase(SLIB_INSTRUCTION_ADDRESS);
  if (status != FLASH_OPERATE_DONE) {
    slib_fatal("erase instruction sector 62", status);
  }

  status = flash_sector_erase(SLIB_DATA_ADDRESS);
  if (status != FLASH_OPERATE_DONE) {
    slib_fatal("erase data sector 63", status);
  }

  for (size_t index = 0u; index < sizeof(slib_test_record); ++index) {
    if (slib_test_record[index] == 0xffu) {
      continue;
    }
    status = flash_byte_program(SLIB_DATA_ADDRESS + (uint32_t)index,
                                slib_test_record[index]);
    if (status != FLASH_OPERATE_DONE) {
      slib_fatal("program test record", status);
    }
  }

  if (!slib_record_matches()) {
    slib_fatal("verify test record", FLASH_PROGRAM_ERROR);
  }

  flash_lock();
  printf("[SLIB] Record programmed and verified. Resetting to enable sLib.\r\n");
  wk_delay_ms(100u);
  nvic_system_reset();
  while (1) {
  }
}

void slib_provision_test_run(void)
{
  printf("[SLIB] state=%s, remaining configuration count=%lu\r\n",
         flash_slib_state_get() == SET ? "enabled" : "disabled",
         (unsigned long)flash_slib_remaining_count_get());

  if (flash_slib_state_get() == SET) {
    if (!slib_range_matches()) {
      printf("[SLIB] FAIL: active range is %u/%u/%u, expected 62/63/63.\r\n",
             (unsigned int)flash_slib_start_sector_get(),
             (unsigned int)flash_slib_datastart_sector_get(),
             (unsigned int)flash_slib_end_sector_get());
      return;
    }

    printf("[SLIB] range 62/63/63: PASS\r\n");
    printf("[SLIB] protected data record: %s\r\n",
           slib_record_matches() ? "PASS" : "FAIL");
    return;
  }

  if (flash_slib_remaining_count_get() == 0u) {
    printf("[SLIB] FAIL: no remaining sLib configuration slots.\r\n");
    return;
  }

  printf("[SLIB] WARNING: provisioning changes flash protection state.\r\n");
  printf("[SLIB] USART1 RX is PA10; connect adapter TX to PA10 and share GND.\r\n");
  printf("[SLIB] Keep power stable and type PROVISION SLIB within %lu seconds.\r\n",
         (unsigned long)(SLIB_CONFIRM_TIMEOUT_MS / 1000u));
  if (!uart_confirmation_received()) {
    printf("[SLIB] Cancelled: confirmation timeout.\r\n");
    return;
  }

  printf("[SLIB] Confirmation accepted; provisioning sectors 62 and 63.\r\n");
  slib_program_record();
}
