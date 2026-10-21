#!/usr/bin/env python3
"""package_extract.py - unpack a CasioEmuMsvc .package ROM file (no C++ build needed).

The format is implemented from the emulator's own reader:
    CasioEmuMsvc/src/Ext/RomPackage.h, src/Binary.h, src/ModelInfo.h

A .package is:
    RomData      : u64 length + bytes
    FlashData    : u64 length + bytes
    InterfaceData: u64 length + bytes
    encVersion   : u8   (0 = plain, 1 = xor, 2 = ChaCha20 + PBKDF2/HMAC)
    if version 2 : Salt (u64+bytes), Nonce (u64+bytes), Hmac (u64+bytes)
    Crc32        : u32
    ModelInfo    : the rest of the file (its own Binary serialization, plaintext)

Encryption v2 keys: K = PBKDF2-HMAC-SHA256(password, salt, 10000, 32)
    K_rom/flash/interface/mac = SHA256(K + b"ROM"/b"FLASH"/b"INTERFACE"/b"MAC")
    HMAC-SHA256(K_mac, rom + flash + interface) must equal Hmac
    then ChaCha20(K_xxx, nonce) decrypts each stream.

Usage:
    python3 package_extract.py <file.package> <password> <output-dir> [--name NAME]
"""
import argparse
import hashlib
import hmac
import os
import struct
import sys


# ------------------------------------------------------------------ binary helpers
class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def take(self, n):
        b = self.d[self.p:self.p + n]
        if len(b) != n:
            raise ValueError('unexpected end of data at %d' % self.p)
        self.p += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack('<H', self.take(2))[0]

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def i32(self):
        return struct.unpack('<i', self.take(4))[0]

    def u64(self):
        return struct.unpack('<Q', self.take(8))[0]

    def blobbytes(self):
        return self.take(self.u64())

    def string(self):
        return self.blobbytes().decode('utf-8', 'replace')

    def boolean(self):
        return self.u8() != 0


def chacha20_block(key_words, counter, nonce_words):
    # NOTE: the emulator's constant is 0x61787065 (not the standard 0x61707865);
    # we reproduce it byte-for-byte so decryption matches.
    state = [0x61787065, 0x3320646E, 0x79622D32, 0x6B206574] + list(key_words) \
        + [counter] + list(nonce_words)

    def rotl(v, c):
        return ((v << c) | (v >> (32 - c))) & 0xFFFFFFFF

    def qr(x, a, b, c, d):
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF
        x[d] = rotl(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF
        x[b] = rotl(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF
        x[d] = rotl(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF
        x[b] = rotl(x[b] ^ x[c], 7)

    x = state[:]
    for _ in range(10):
        qr(x, 0, 4, 8, 12); qr(x, 1, 5, 9, 13); qr(x, 2, 6, 10, 14); qr(x, 3, 7, 11, 15)
        qr(x, 0, 5, 10, 15); qr(x, 1, 6, 11, 12); qr(x, 2, 7, 8, 13); qr(x, 3, 4, 9, 14)
    out = [(x[i] + state[i]) & 0xFFFFFFFF for i in range(16)]
    return b''.join(struct.pack('<I', w) for w in out)


def chacha20(key, nonce, data):
    key_words = list(struct.unpack('<8I', key))
    nonce_words = list(struct.unpack('<3I', nonce))
    out = bytearray(len(data))
    counter = 0
    for off in range(0, len(data), 64):
        ks = chacha20_block(key_words, counter, nonce_words)
        chunk = data[off:off + 64]
        for i, b in enumerate(chunk):
            out[off + i] = b ^ ks[i]
        counter += 1
    return bytes(out)


def derive(k, label):
    return hashlib.sha256(k + label).digest()


# ------------------------------------------------------------------ ModelInfo
class ModelInfo:
    def __init__(self):
        self.csr_mask = 0
        self.hardware_id = 0
        self.real_hardware = False
        self.pd_value = 0
        self.buttons = []
        self.sprites = {}
        self.ink_color = (0, 0, 0)
        self.interface_path = ''
        self.model_name = ''
        self.rom_path = ''
        self.flash_path = ''
        self.enable_new_screen = False
        self.is_sample_rom = False
        self.legacy_ko = False
        self.u16_mode = False
        self.LARGE_model = False
        self.ml620_mirroring = False
        self.extra = {}


def parse_rect(r):
    return (r.i32(), r.i32(), r.i32(), r.i32())


def parse_model_info(buf):
    r = Reader(buf)
    r.string()                       # version header (ignored by the emulator's Read)
    m = ModelInfo()
    m.csr_mask = r.u16()
    m.hardware_id = r.u16()
    m.real_hardware = r.boolean()
    m.pd_value = r.u8()
    for _ in range(r.u64()):
        rect = parse_rect(r)
        kiko = r.i32()
        name = r.string()
        m.buttons.append((rect, kiko, name))
    for _ in range(r.u64()):
        key = r.string()
        src = parse_rect(r)
        dest = parse_rect(r)
        m.sprites[key] = (src, dest)
    m.ink_color = (r.i32(), r.i32(), r.i32())
    m.interface_path = r.string()
    m.model_name = r.string()
    m.rom_path = r.string()
    m.flash_path = r.string()
    m.enable_new_screen = r.boolean()
    m.is_sample_rom = r.boolean()
    m.legacy_ko = r.boolean()
    m.u16_mode = r.boolean()
    m.LARGE_model = r.boolean()
    m.ml620_mirroring = r.boolean()
    for _ in range(r.u64()):
        k = r.string()
        v = r.string()
        m.extra[k] = v
    return m, r.p


# ------------------------------------------------------------------ main
def extract(path, password, outdir, name=None):
    data = open(path, 'rb').read()
    r = Reader(data)
    rom = r.blobbytes()
    flash = r.blobbytes()
    interface = r.blobbytes()
    enc_version = r.u8()
    salt = nonce = hmac_tag = None
    if enc_version == 2:
        salt = r.blobbytes()
        nonce = r.blobbytes()
        hmac_tag = r.blobbytes()
    elif enc_version == 1:
        pass
    crc32 = r.u32()
    config_start = r.p
    model, consumed = parse_model_info(data[config_start:])
    config_bytes = data[config_start:config_start + consumed]

    if enc_version == 2:
        if not password:
            raise SystemExit('password required')
        K = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 10000, 32)
        K_rom, K_flash, K_iface, K_mac = (derive(K, b'ROM'), derive(K, b'FLASH'),
                                          derive(K, b'INTERFACE'), derive(K, b'MAC'))
        auth = rom + flash + interface
        calc = hmac.new(K_mac, auth, hashlib.sha256).digest()
        if not hmac.compare_digest(calc, hmac_tag):
            raise SystemExit('wrong password (HMAC mismatch)')
        rom = chacha20(K_rom, nonce, rom)
        flash = chacha20(K_flash, nonce, flash)
        interface = chacha20(K_iface, nonce, interface)
    elif enc_version == 1:
        raise SystemExit('encryption v1 (xor) not implemented; use the C++ RomPacker')
    elif enc_version != 0:
        raise SystemExit('unsupported encryption version %d' % enc_version)

    folder = name or model.model_name.strip() or os.path.splitext(os.path.basename(path))[0]
    dest = os.path.join(outdir, folder)
    os.makedirs(dest, exist_ok=True)

    open(os.path.join(dest, model.rom_path), 'wb').write(rom)
    if model.flash_path:
        open(os.path.join(dest, model.flash_path), 'wb').write(flash)
    open(os.path.join(dest, model.interface_path), 'wb').write(interface)
    open(os.path.join(dest, 'config.bin'), 'wb').write(config_bytes)

    print('%-28s -> %s' % (os.path.basename(path), dest))
    print('    model_name   : %s' % model.model_name)
    print('    hardware_id  : %d' % model.hardware_id)
    print('    rom_path     : %s (%d bytes)' % (model.rom_path, len(rom)))
    print('    flash_path   : %s%s' % (model.flash_path or '(none)',
                                        ' (%d bytes)' % len(flash) if model.flash_path else ''))
    print('    interface    : %s (%d bytes)' % (model.interface_path, len(interface)))
    print('    encryption   : v%d' % enc_version)
    return dest


def main():
    ap = argparse.ArgumentParser(description='unpack a CasioEmuMsvc .package')
    ap.add_argument('package')
    ap.add_argument('password', nargs='?', default='')
    ap.add_argument('outdir', nargs='?', default='.')
    ap.add_argument('--name', default=None, help='output folder name (default: model_name)')
    a = ap.parse_args()
    extract(a.package, a.password, a.outdir, a.name)


if __name__ == '__main__':
    main()
