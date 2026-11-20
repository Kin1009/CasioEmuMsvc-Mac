#!/usr/bin/env python3
"""Build a self-contained `.plugin` ZIP container for the CG100 architecture core.

A bundled .plugin is an ordinary ZIP archive (the host detects the `PK\\x03\\x04`
magic) with this layout:

    plugin.json                 manifest consumed by PluginManifest.cpp
    assets/info.json            declarative info document (entry_point)
    CG100SH4.plugin             the Mach-O dylib itself (native_entry)
    lib/libcig100core.dylib     embedded dependency, found at runtime through
                                the dylib's @loader_path/lib LC_RPATH

The archive keeps the `.plugin` file extension.  dylib entries are stored with
executable bits (0755) so dyld can map them after extraction.

Usage:
    python3 tools/make_bundled_plugin.py \
        --core /path/to/CG100SH4.plugin \
        --lib  /path/to/libcig100core.dylib \
        --out  /path/to/output/CG100SH4.plugin

This tool only READS the live app's binaries; it never overwrites them.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
import zipfile
from pathlib import Path

MANIFEST = {
    "schema": 1,
    "id": "com.casioemu.arch.cg100sh4",
    "name": "CG100 SH4 Architecture Core",
    "version": "1.0.0",
    "author": "CasioEmuMsvc",
    "description": (
        "Native SH4 emulator core for the CG100 board model; bundles its "
        "libcig100core.dylib dependency inside the .plugin container."
    ),
    "min_host": "2.1.0",
    "platforms": ["macos"],
    "type": "tool",
    "permissions": ["ui.shell"],
    "entry_point": "assets/info.json",
    "native_entry": "CG100SH4.plugin",
    "assets": ["assets/info.json"],
}

INFO = {
    "name": "CG100 SH4 Architecture Core",
    "description": "Bundled native architecture core for the CG100 board model.",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def add_bytes(archive: zipfile.ZipFile, name: str, data: bytes, mode: int) -> None:
    info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (stat.S_IFREG | mode) << 16
    info.create_system = 3  # Unix, so the mode bits are honoured on extraction
    archive.writestr(info, data)


def build(core: Path, lib: Path, out: Path) -> int:
    for path, label in ((core, "core dylib"), (lib, "dependency dylib")):
        if not path.is_file():
            print(f"error: {label} not found: {path}", file=sys.stderr)
            return 2

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    if tmp.exists():
        tmp.unlink()

    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as archive:
        add_bytes(archive, "plugin.json",
                  (json.dumps(MANIFEST, indent=2) + "\n").encode("utf-8"), 0o644)
        add_bytes(archive, "assets/info.json",
                  (json.dumps(INFO, indent=2) + "\n").encode("utf-8"), 0o644)
        add_bytes(archive, "CG100SH4.plugin", core.read_bytes(), 0o755)
        add_bytes(archive, "lib/libcig100core.dylib", lib.read_bytes(), 0o755)

    os.replace(tmp, out)

    # Self-verification: magic, entry list and per-entry integrity.
    with out.open("rb") as handle:
        magic = handle.read(4)
    if magic != b"PK\x03\x04":
        print("error: output does not have ZIP magic", file=sys.stderr)
        return 3
    with zipfile.ZipFile(out, "r") as archive:
        names = archive.namelist()
        expected = {"plugin.json", "assets/info.json", "CG100SH4.plugin",
                    "lib/libcig100core.dylib"}
        if set(names) != expected:
            print(f"error: unexpected entries: {names}", file=sys.stderr)
            return 3
        if archive.read("CG100SH4.plugin") != core.read_bytes():
            print("error: core dylib checksum mismatch", file=sys.stderr)
            return 3
        if archive.read("lib/libcig100core.dylib") != lib.read_bytes():
            print("error: dependency dylib checksum mismatch", file=sys.stderr)
            return 3
        json.loads(archive.read("plugin.json"))

    print(f"Wrote bundled plugin: {out}")
    print(f"  SHA-256: {sha256(out)}")
    print(f"  Entries: {', '.join(sorted(names))}")
    return 0


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    default_app = repo.parent / "build-ui3" / "CasioEmuMsvc" / "CasioEmuMsvc.app"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path,
                        default=default_app / "Contents/MacOS/plugins/CG100SH4.plugin")
    parser.add_argument("--lib", type=Path,
                        default=default_app / "Contents/MacOS/plugins/lib/libcig100core.dylib")
    parser.add_argument("--out", type=Path,
                        default=Path.cwd() / "CG100SH4.plugin")
    args = parser.parse_args()
    return build(args.core, args.lib, args.out)


if __name__ == "__main__":
    raise SystemExit(main())
