#!/usr/bin/env python3
"""Create tests/fixtures/import-fixture.package (and an encrypted variant).

The packages are real CasioEmuMsvc ROM packages understood by
src/Ext/RomPackage.h.  They are intentionally tiny and use a synthetic 32 KiB
ROM so the launcher can import and register them without copyrighted data.

Usage:
    python3 tools/make_import_fixture.py [output.package] [--password PW]
"""
import hashlib
import hmac
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from package_extract import chacha20  # noqa: E402  (same repo helper)

ROM_SIZE = 0x8000


def u8(v):
    return struct.pack('<B', v)


def u16(v):
    return struct.pack('<H', v)


def u32(v):
    return struct.pack('<I', v)


def i32(v):
    return struct.pack('<i', v)


def blob(data):
    return struct.pack('<Q', len(data)) + data


def string(text):
    return blob(text.encode('utf-8'))


def rect(x, y, w, h):
    return i32(x) + i32(y) + i32(w) + i32(h)


def one_pixel_png():
    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack(
            '>I', zlib.crc32(tag + payload) & 0xffffffff)
    signature = b'\x89PNG\r\n\x1a\n'
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
    idat = chunk(b'IDAT', zlib.compress(b'\x00\x80\x80\x80'))
    iend = chunk(b'IEND', b'')
    return signature + ihdr + idat + iend


def model_info(name):
    header = "\n\nnx-U16/U8 Emulator Configuration file v53\n\nImport fixture\n\n"
    out = string(header)
    out += u16(0)          # csr_mask
    out += u16(4)          # hardware_id = HW_CLASSWIZ
    out += u8(0)           # real_hardware
    out += u8(0)           # pd_value
    out += struct.pack('<Q', 0)  # buttons
    out += struct.pack('<Q', 1)  # sprites
    out += string('rsd_interface')
    out += rect(0, 0, 1, 1) + rect(0, 0, 1, 1)
    out += i32(0) + i32(0) + i32(0)  # ink_color
    out += string('interface.png')
    out += string(name)
    out += string('rom.bin')
    out += string('')      # flash_path
    out += u8(0)           # enable_new_screen
    out += u8(0)           # is_sample_rom
    out += u8(0)           # legacy_ko
    out += u8(1)           # u16_mode
    out += u8(1)           # LARGE_model
    out += u8(0)           # ml620_mirroring
    out += struct.pack('<Q', 0)  # extra
    return out


def derive(key, label):
    return hashlib.sha256(key + label).digest()


def build(password, name):
    rom = b'\x00' * ROM_SIZE
    flash = b''
    interface = one_pixel_png()
    info = model_info(name)
    payload = b''
    if password:
        salt = bytes(range(16))
        nonce = bytes(range(16, 28))
        base = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 10000, 32)
        k_rom = derive(base, b'ROM')
        k_flash = derive(base, b'FLASH')
        k_interface = derive(base, b'INTERFACE')
        k_mac = derive(base, b'MAC')
        rom = chacha20(k_rom, nonce, rom)
        flash = chacha20(k_flash, nonce, flash)
        interface = chacha20(k_interface, nonce, interface)
        tag = hmac.new(k_mac, rom + flash + interface, hashlib.sha256).digest()
        payload += blob(rom) + blob(flash) + blob(interface)
        payload += u8(2)
        payload += blob(salt) + blob(nonce) + blob(tag)
        payload += u32(0)
        payload += info
    else:
        payload += blob(rom) + blob(flash) + blob(interface)
        payload += u8(0)
        payload += u32(zlib.crc32(rom) & 0xffffffff)
        payload += info
    return payload


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    args = [a for a in sys.argv[1:]]
    password = ''
    if '--password' in args:
        index = args.index('--password')
        password = args[index + 1]
        del args[index:index + 2]
    output = args[0] if args else os.path.join(
        root, 'CasioEmuMsvc', 'tests', 'fixtures',
        'import-fixture-encrypted.package' if password else 'import-fixture.package')
    name = 'Encrypted Import Fixture' if password else 'Import Fixture'
    payload = build(password, name)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, 'wb') as stream:
        stream.write(payload)
    print('%s (%d bytes, rom=%d, encrypted=%s)' % (
        output, len(payload), ROM_SIZE, 'yes' if password else 'no'))


if __name__ == '__main__':
    main()
