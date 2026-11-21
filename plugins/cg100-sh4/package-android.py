#!/usr/bin/env python3
"""Package the Android SH4 plugin as a host-loadable .plugin ZIP container.

Layout follows CasioEmuMsvc's plugin format:

    plugin.json                       manifest (platforms: android)
    assets/info.json                  declarative entry point
    libcg100sh4-android-arm64-android.so   native_entry, stored with mode 0755

Usage:
    package-android.py --so <path-to-libcg100sh4-...so> \
        --abi arm64-v8a --out <output.plugin>
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import zipfile
from pathlib import Path


def manifest(abi: str, native: str) -> dict:
    tag = "arm64-android" if abi == "arm64-v8a" else "any"
    return {
        "schema": 1,
        "id": "com.casioemu.arch.cg100sh4",
        "name": "CG100 SH4 Architecture Core (Android)",
        "version": "1.0.0",
        "author": "CasioEmuMsvc",
        "description": (
            "Self-contained native SH4 core for the fx-CG100 board model, "
            f"built for Android {abi} ({tag})."
        ),
        "min_host": "2.1.0",
        "platforms": ["android", f"android-{tag}"],
        "type": "tool",
        "permissions": ["ui.shell"],
        "entry_point": "assets/info.json",
        "native_entry": native,
        "assets": ["assets/info.json"],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--so", required=True, type=Path, help="built libcg100sh4-*.so")
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    so = args.so.resolve()
    if not so.is_file():
        raise SystemExit(f"missing native library: {so}")
    native_name = so.name
    info = {
        "name": "CG100 SH4 Architecture Core (Android)",
        "description": f"Native SH4/emulator bridge for Android {args.abi}.",
        "native_entry": native_name,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(
            "plugin.json", json.dumps(manifest(args.abi, native_name), indent=2) + "\n"
        )
        archive.writestr("assets/info.json", json.dumps(info, indent=2) + "\n")
        entry = zipfile.ZipInfo(native_name)
        entry.external_attr = (stat.S_IFREG | 0o755) << 16
        archive.writestr(entry, so.read_bytes())
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
