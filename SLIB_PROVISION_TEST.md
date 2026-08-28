# AT32F403A sLib provisioning test

This test programs a public, non-secret 80-byte record into the AT32F403A
sLib data area and enables its protection. It follows AN0040 V2.0.3 and uses:

| Region | Sector | Address | Test use |
|---|---:|---|---|
| SLIB_INSTRUCTION | 62 | `0x0801F000..0x0801F7FF` | Erased/empty |
| SLIB_DATA | 63 | `0x0801F800..0x0801FFFF` | 80-byte test record |

The record contains the product P-256 public key copied from
`/home/qxun/secure_keys/ecdsa_public_sec1.bin` in SEC1 uncompressed format.
The corresponding private key is not included in this firmware.

## Warning

Enabling sLib changes persistent flash protection state and consumes one of
the limited sLib configuration operations. Do this only on a development MCU
whose contents may be lost.

Removing sLib protection requires its original 32-bit password and triggers a
full main-flash erase, including the sLib contents. This project intentionally
does not provide or call `flash_slib_disable()`.

Do not use `0x00000000` or `0xFFFFFFFF` as the password. Keep the selected
password outside the repository.

## Build

Use a separate build directory so the normal Debug configuration remains a
non-provisioning image:

```sh
cmake --preset Debug \
  -B build/SLibProvision \
  -DAT32_SLIB_PROVISION=ON \
  -DAT32_SLIB_PASSWORD=0xYOUR8HEX

cmake --build build/SLibProvision --parallel
```

Replace `0xYOUR8HEX` with a valid, retained 32-bit password. The provisioning
link restricts the image to sectors 0 through 61 and fails if it grows into
the sLib target area. The password is necessarily present in this temporary
provisioning ELF and its build metadata; remove the separate build directory
after the test and do not distribute that image.

## Run

1. Confirm the development MCU has no active sLib, FAP, or EPP protection.
2. Connect USART1 at 115200, 8-N-1 and keep power stable. Connect MCU PA9
   (USART1 TX) to the adapter RX, MCU PA10 (USART1 RX) to the adapter TX, and
   connect the grounds. Disable local echo in the terminal if possible.
3. Program `build/SLibProvision/at32_usb_dfu.elf` with the existing OpenOCD
   configuration and reset/run the MCU. The normal VS Code download task uses
   `build/Debug`, so do not use it for this provisioning image. For example:

```sh
/home/qxun/Downloads/openocd-at32/bin-linux_x86_64/openocd \
  -s /home/qxun/Downloads/openocd-at32/scripts \
  -f openocd.cfg \
  -c "program build/SLibProvision/at32_usb_dfu.elf verify reset exit"
```

4. Read the warning over USART1.
5. Type the exact ASCII text `PROVISION SLIB` within 60 seconds. Without this
   confirmation the program performs no flash write.
6. The MCU configures sLib, erases sectors 62 and 63, writes and verifies the
   record, and resets itself.
7. After reset, expect:

```text
[SLIB] state=enabled, remaining configuration count=...
[SLIB] range 62/63/63: PASS
[SLIB] protected data record: PASS
```

Do not infer success from the OpenOCD download result alone. Success is the
post-reset USART output above.

## Normal builds

`cmake --preset Debug` does not compile or call the provisioner. No sLib write
can occur from the normal Debug image.

Once a product actually uses this sLib layout, its final linker script must
permanently exclude `0x0801F000..0x0801FFFF`, even if code later grows beyond
that hole into flash starting at `0x08020000`.
