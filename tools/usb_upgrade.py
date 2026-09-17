#!/usr/bin/env python3
"""Upgrade an AT32F403A over the TinyUSB Vendor bulk protocol."""

import argparse
import ctypes
import ctypes.util
import pathlib
import struct
import sys
import zlib

VID = 0xCAFE
PID = 0x4002
EP_OUT = 0x01
EP_IN = 0x81
MAGIC = 0x50555441
FW_MAGIC = 0x57465441
CMD_GET_INFO = 1
CMD_ERASE = 2
CMD_DOWNLOAD = 3
CMD_RESET = 4
CMD_JUMP = 5
FLAG_START = 1
FLAG_DATA = 2
FLAG_END = 4
STATUS_NAMES = {
    -1: "frame", -2: "CRC", -3: "command", -4: "state",
    -5: "argument", -6: "firmware header/signature", -7: "rollback",
    -8: "Flash", -9: "sequence", -10: "Flash read-back hash",
    -11: "signature", -12: "vector table", -13: "BCB", -14: "slot",
}


class LibusbDevice:
    """Small ctypes fallback so the tool also works without PyUSB."""

    def __init__(self):
        library = ctypes.util.find_library("usb-1.0")
        if not library:
            raise SystemExit("neither PyUSB nor libusb-1.0 is available")
        self.lib = ctypes.CDLL(library)
        self.context = ctypes.c_void_p()
        self.lib.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        self.lib.libusb_init.restype = ctypes.c_int
        self.lib.libusb_open_device_with_vid_pid.argtypes = [
            ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
        self.lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
        self.lib.libusb_set_auto_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_get_configuration.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        self.lib.libusb_set_configuration.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_bulk_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_ubyte, ctypes.POINTER(ctypes.c_ubyte),
            ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
        self.lib.libusb_bulk_transfer.restype = ctypes.c_int
        if self.lib.libusb_init(ctypes.byref(self.context)) != 0:
            raise SystemExit("libusb initialization failed")
        self.handle = self.lib.libusb_open_device_with_vid_pid(self.context, VID, PID)
        if not self.handle:
            raise SystemExit("bootloader USB device cafe:4002 not found or access denied")
        self.lib.libusb_set_auto_detach_kernel_driver(self.handle, 1)

    def is_kernel_driver_active(self, interface: int) -> bool:
        return False

    def detach_kernel_driver(self, interface: int) -> None:
        pass

    def set_configuration(self) -> None:
        current = ctypes.c_int()
        result = self.lib.libusb_get_configuration(
            self.handle, ctypes.byref(current))
        if result != 0:
            raise RuntimeError(f"libusb get configuration failed: {result}")
        if current.value == 1:
            return
        result = self.lib.libusb_set_configuration(self.handle, 1)
        if result != 0:
            raise RuntimeError(f"libusb set configuration failed: {result}")

    def claim(self) -> None:
        result = self.lib.libusb_claim_interface(self.handle, 0)
        if result != 0:
            raise RuntimeError(f"libusb claim interface failed: {result}")

    def write(self, endpoint: int, data: bytes, timeout: int) -> int:
        buffer = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        transferred = ctypes.c_int()
        result = self.lib.libusb_bulk_transfer(
            self.handle, endpoint, buffer, len(data),
            ctypes.byref(transferred), timeout)
        if result != 0:
            raise RuntimeError(f"libusb bulk OUT failed: {result}")
        return transferred.value

    def read(self, endpoint: int, length: int, timeout: int) -> bytes:
        buffer = (ctypes.c_ubyte * length)()
        transferred = ctypes.c_int()
        result = self.lib.libusb_bulk_transfer(
            self.handle, endpoint, buffer, length,
            ctypes.byref(transferred), timeout)
        if result != 0:
            raise RuntimeError(f"libusb bulk IN failed: {result}")
        return bytes(buffer[:transferred.value])


class BootDevice:
    def __init__(self):
        try:
            import usb.core
            import usb.util
            self.dev = usb.core.find(idVendor=VID, idProduct=PID)
            if self.dev is None:
                raise SystemExit("bootloader USB device cafe:4002 not found")
            if self.dev.is_kernel_driver_active(0):
                self.dev.detach_kernel_driver(0)
            self.dev.set_configuration()
            usb.util.claim_interface(self.dev, 0)
        except ImportError:
            self.dev = LibusbDevice()
            self.dev.set_configuration()
            self.dev.claim()
        self.sequence = 1
        self.session = 0
        self.rx_buffer = bytearray()

    def write_all(self, data: bytes, timeout: int) -> None:
        offset = 0
        while offset < len(data):
            offset += self.dev.write(EP_OUT, data[offset:], timeout=timeout)

    def read_exact(self, length: int, timeout: int) -> bytes:
        while len(self.rx_buffer) < length:
            # Always request at least several max-size packets. Asking libusb
            # for only the 32-byte protocol header could truncate a 64-byte
            # USB packet that already contains the start of the payload.
            self.rx_buffer.extend(self.dev.read(EP_IN, 512, timeout=timeout))
        result = bytes(self.rx_buffer[:length])
        del self.rx_buffer[:length]
        return result

    def exchange(self, command: int, flags: int = 0, payload: bytes = b"",
                 timeout: int = 5000) -> bytes:
        sequence = self.sequence
        self.sequence += 1
        prefix = struct.pack("<IBBHIIIII", MAGIC, 1, 32, command, sequence,
                             len(payload), self.session, flags,
                             zlib.crc32(payload))
        frame = prefix + struct.pack("<I", zlib.crc32(prefix)) + payload
        self.write_all(frame, timeout)
        header = self.read_exact(32, timeout)
        fields = struct.unpack("<IBBHIIIIII", header)
        magic, version, size, response_command, response_sequence, payload_len, \
            response_session, _, payload_crc, header_crc = fields
        if (magic, version, size, response_command, response_sequence) != \
                (MAGIC, 1, 32, command | 0x8000, sequence):
            raise RuntimeError("invalid response frame header")
        if zlib.crc32(header[:28]) != header_crc:
            raise RuntimeError("response header CRC mismatch")
        response = self.read_exact(payload_len, timeout)
        if zlib.crc32(response) != payload_crc or len(response) < 16:
            raise RuntimeError("response payload CRC/length mismatch")
        status, detail, session, next_offset = struct.unpack_from("<iIII", response)
        self.session = response_session or session
        if status != 0:
            name = STATUS_NAMES.get(status, "unknown")
            raise RuntimeError(f"device error {status} ({name}), detail={detail}")
        return response[16:]


def show_info(device: BootDevice) -> None:
    info = device.exchange(CMD_GET_INFO)
    if len(info) < 89:
        raise RuntimeError("short GET_INFO response")
    boot_version, target = struct.unpack_from("<II", info, 0)
    uid = struct.unpack_from("<III", info, 8)
    flash_base, flash_size, erase_size = struct.unpack_from("<III", info, 20)
    slot_a, slot_b, slot_size, capacity, generation = struct.unpack_from("<IIIII", info, 32)
    fw_a, fw_b, sec_a, sec_b, size_a, size_b = struct.unpack_from("<IIIIII", info, 52)
    max_chunk = struct.unpack_from("<H", info, 76)[0]
    default, state_a, state_b = info[80:83]
    print(f"boot={boot_version >> 16}.{(boot_version >> 8) & 0xff}.{boot_version & 0xff} "
          f"target=0x{target:08x} uid={uid[0]:08x}{uid[1]:08x}{uid[2]:08x}")
    print(f"flash=0x{flash_base:08x}+{flash_size}, erase={erase_size}, BCB generation={generation}")
    print(f"App A: base=0x{slot_a:08x}, state={state_a}, fw={fw_a}, sec={sec_a}, size={size_a}")
    print(f"App B: base=0x{slot_b:08x}, state={state_b}, fw={fw_b}, sec={sec_b}, size={size_b}")
    print(f"slot size={slot_size}, image capacity={capacity}, default={default}, max chunk={max_chunk}")


def upgrade(device: BootDevice, package_path: pathlib.Path, jump: bool) -> None:
    package = package_path.read_bytes()
    if len(package) < 0x1008 or struct.unpack_from("<I", package)[0] != FW_MAGIC:
        raise SystemExit("invalid or truncated firmware package")
    header_size = struct.unpack_from("<H", package, 6)[0]
    if header_size != 0x1000:
        raise SystemExit(f"unsupported firmware header size {header_size}")
    header, body = package[:header_size], package[header_size:]
    slot, image_size = struct.unpack_from("<II", header, 12)[0], struct.unpack_from("<I", header, 24)[0]
    if slot not in (0, 1) or image_size != len(body):
        raise SystemExit("package slot/image size is inconsistent")

    print("validating signed header...")
    device.exchange(CMD_DOWNLOAD, FLAG_START, header, timeout=30000)
    print(f"erasing App {'AB'[slot]} ({len(body)} bytes)...")
    device.exchange(CMD_ERASE, payload=struct.pack("<B3x", slot), timeout=120000)
    for offset in range(0, len(body), 4096):
        chunk = body[offset:offset + 4096]
        payload = struct.pack("<II", offset, len(chunk)) + chunk
        device.exchange(CMD_DOWNLOAD, FLAG_DATA, payload, timeout=10000)
        print(f"\rprogramming: {offset + len(chunk)}/{len(body)}", end="", flush=True)
    print("\nverifying Flash SHA-256 and ECDSA signature...")
    device.exchange(CMD_DOWNLOAD, FLAG_END, timeout=30000)
    print("upgrade complete")
    if jump:
        print(f"setting App {'AB'[slot]} as default and resetting...")
        device.exchange(CMD_JUMP, payload=struct.pack("<BB2x", slot, 1))


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("info")
    install = sub.add_parser("upgrade")
    install.add_argument("package", type=pathlib.Path)
    install.add_argument("--jump", action="store_true")
    jump = sub.add_parser("jump")
    jump.add_argument("slot", choices=("A", "B"))
    sub.add_parser("reset")
    args = parser.parse_args()

    try:
        device = BootDevice()
        if args.command == "info":
            show_info(device)
        elif args.command == "upgrade":
            upgrade(device, args.package, args.jump)
        elif args.command == "jump":
            slot = 0 if args.slot == "A" else 1
            device.exchange(CMD_JUMP, payload=struct.pack("<BB2x", slot, 1),
                            timeout=30000)
        else:
            device.exchange(CMD_RESET)
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
