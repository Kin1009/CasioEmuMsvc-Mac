#!/usr/bin/env python3
"""Build a CasioEmuMsvc `.plugin` package for one or more OS/arch targets.

A `.plugin` is an ordinary ZIP (the host checks the ``PK\\x03\\x04`` magic)
containing:

    plugin.json                 manifest (schema 1), parsed by PluginManifest.cpp
    assets/info.json            declarative info document (entry_point)
    <name>-<os>-<arch>.<ext>    one or more tagged native binaries
    lib/<dependency>            optional embedded dependency dylibs

Tagged native binaries follow ``<name>-<os>-<arch>.<ext>`` and are selected by
the host at load time (see ``src/Plugin/PlatformTags.h``).  Native entries are
stored with executable bits (0755, Unix create_system) so dyld can map them
after extraction.  JSON entries use 0644.

This tool VALIDATES every field against the same rules the host enforces
(manifest schema, permission names, semver, id, OS/arch tags, safe relative ZIP
paths, case-insensitive entry collisions) and SELF-VERIFIES the output ZIP
(magic, exact entry list, per-entry bytes, mode bits, JSON parseability).

It never writes to the running app bundle.  Output goes only to ``--out``.

Examples
--------
    # Four platforms in one package, explicit per-binary tags
    python3 tools/build_plugin.py --name hello --id com.example.hello \\
        --version 1.0.0 --type tool --permissions ui.shell \\
        --binary-os macos --binary-arch x64 --binary core.dylib \\
        --binary-os macos --binary-arch arm64 --binary core-arm.dylib \\
        --binary-os linux --binary-arch x64 --binary core.so \\
        --binary-os windows --binary-arch x64 --binary core.dll \\
        --out /tmp/hello.plugin

    # Reproduce the bundled CG100 SH4 package exactly (legacy native_entry)
    python3 tools/build_plugin.py --preset cg100-sh4 \\
        --core /path/to/CG100SH4.plugin --lib /path/to/libcig100core.dylib \\
        --out /tmp/CG100SH4.plugin
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# --------------------------------------------------------------------------- #
# Host-authoritative constants.  Keep in sync with:
#   src/Plugin/PlatformTags.h          (OS/arch tags, extensions, matching)
#   src/Plugin/PluginManifest.cpp      (permissions, types, id/semver rules)
#   src/Plugin/PluginPermissions.cpp   (capability grants)
# --------------------------------------------------------------------------- #
KNOWN_OS = ("macos", "linux", "windows", "android", "any")
KNOWN_ARCH = ("x64", "x32", "arm64", "arm64-android", "any")
NATIVE_EXTS = (".dylib", ".so", ".dll", ".plugin")
PLUGIN_TYPES = ("theme", "tab", "inject", "ui_revamp", "tool")
PERMISSIONS = (
    "emulator.read", "emulator.write", "emulator.code", "emulator.inject",
    "filesystem.read", "filesystem.write", "network", "process",
    "ui.shell", "clipboard", "arch",
)
DEFAULT_EXT_OS = {".dylib": "macos", ".dll": "windows", ".so": "linux", ".plugin": "macos"}
DEFAULT_HOST_VERSION = "2.1.0"  # matches the shipped CG100 package

SEMVER_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)

# Legacy CG100 preset: byte-for-byte the manifest/info of the known-good ZIP.
CG100_MANIFEST = {
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
    "permissions": ["ui.shell", "arch"],
    "entry_point": "assets/info.json",
    "native_entry": "CG100SH4.plugin",
    "assets": ["assets/info.json"],
}
CG100_INFO = {
    "name": "CG100 SH4 Architecture Core",
    "description": "Bundled native architecture core for the CG100 board model.",
}


class BuildError(Exception):
    """A validation or verification failure (reported on stderr, exit != 0)."""


@dataclass
class Payload:
    archive_name: str
    source: Optional[Path] = None   # file to read bytes from
    data: Optional[bytes] = None    # in-memory bytes (JSON documents)
    mode: int = 0o644
    native: bool = False            # tagged native binary (mode 0755)


@dataclass
class BinarySpec:
    path: Path
    os_tag: Optional[str] = None
    arch_tag: Optional[str] = None


@dataclass
class Plan:
    manifest: dict
    info: dict
    binaries: List[Payload] = field(default_factory=list)
    libs: List[Payload] = field(default_factory=list)
    assets: List[Payload] = field(default_factory=list)

    def payloads(self) -> List[Payload]:
        head = [
            Payload("plugin.json",
                    data=(json.dumps(self.manifest, indent=2) + "\n").encode("utf-8"),
                    mode=0o644),
            Payload("assets/info.json",
                    data=(json.dumps(self.info, indent=2) + "\n").encode("utf-8"),
                    mode=0o644),
        ]
        return head + self.assets + self.binaries + self.libs


# --------------------------------------------------------------------------- #
# Validators (mirror the host's PluginManifest.cpp / PlatformTags.h rules)
# --------------------------------------------------------------------------- #
def valid_id(value: str) -> bool:
    """IsValidPluginId: 3..128 chars, lower-case/digits/._-, one dot, no edge dots."""
    if not 3 <= len(value) <= 128:
        return False
    if value.startswith(".") or value.endswith(".") or ".." in value:
        return False
    if not re.fullmatch(r"[a-z0-9_.-]+", value):
        return False
    return "." in value


def valid_semver(value: str) -> bool:
    return bool(SEMVER_RE.fullmatch(value))


def safe_relative_path(value: str) -> bool:
    """IsSafeRelativePath: no leading slash/backslash, no '\\', no '.'/'..' components."""
    if not value or value.startswith(("/", "\\")) or "\\" in value:
        return False
    if value.endswith("/"):
        value = value[:-1]
    if not value:
        return False
    return all(part not in ("", ".", "..") for part in value.split("/"))


def parse_tags(raw: Optional[str], known: Tuple[str, ...], label: str) -> List[str]:
    if not raw:
        return []
    tags: List[str] = []
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        if token not in known:
            raise BuildError(f"unknown {label} tag '{token}' (known: {', '.join(known)})")
        if token not in tags:
            tags.append(token)
    if not tags:
        raise BuildError(f"--{label} was given but contained no tags")
    return tags


def parse_permissions(raw: Optional[str]) -> List[str]:
    if not raw:
        return []
    values: List[str] = []
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        if token not in PERMISSIONS:
            raise BuildError(
                f"unknown permission '{token}' (known: {', '.join(PERMISSIONS)})"
            )
        if token in values:
            raise BuildError(f"duplicate permission '{token}'")
        values.append(token)
    return values


def tagged_name_ok(archive_name: str, os_tag: str, arch_tag: str) -> bool:
    """Mirror ParseTaggedNativeFileName(): suffix '-os-arch' must be a strict suffix of the stem."""
    dot = archive_name.rfind(".")
    if dot <= 0 or dot + 1 >= len(archive_name):
        return False
    stem = archive_name[:dot]
    suffix = f"-{os_tag}-{arch_tag}"
    return len(stem) > len(suffix) and stem.endswith(suffix)


# --------------------------------------------------------------------------- #
# Ordered CLI event handling so `--binary-os/--binary-arch` can precede each
# `--binary`.  Tags are sticky: they apply to every following --binary until
# changed.  Without an explicit tag the OS is derived from the extension and
# the architecture is 'any' (or the single declared --arch-tags value).
# --------------------------------------------------------------------------- #
class OrderedAction(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        events = getattr(namespace, "_events", None)
        if events is None:
            events = []
            setattr(namespace, "_events", events)
        events.append((self.dest, values))


def build_plan(args: argparse.Namespace) -> Plan:
    os_tags = parse_tags(args.os_tags, KNOWN_OS, "os-tags")
    arch_tags = parse_tags(args.arch_tags, KNOWN_ARCH, "arch-tags")
    permissions = parse_permissions(args.permissions)

    if not valid_id(args.id):
        raise BuildError(
            f"invalid --id '{args.id}': use reverse-DNS form, e.g. com.example.plugin "
            "(lower-case letters, digits, '.', '_' or '-', at least one dot)"
        )
    if not 1 <= len(args.name) <= 80:
        raise BuildError("--name must be 1-80 characters")
    if not valid_semver(args.version):
        raise BuildError(f"invalid --version '{args.version}': expected MAJOR.MINOR.PATCH")
    if not valid_semver(args.min_host):
        raise BuildError(f"invalid --min-host '{args.min_host}': expected semver")
    if args.type not in PLUGIN_TYPES:
        raise BuildError(f"unknown --type '{args.type}' (known: {', '.join(PLUGIN_TYPES)})")
    if args.description and len(args.description) > 500:
        raise BuildError("--description must be at most 500 characters")

    manifest = {
        "schema": 1,
        "id": args.id,
        "name": args.name,
        "version": args.version,
        "author": args.author,
        "description": args.description,
        "min_host": args.min_host,
        "type": args.type,
        "permissions": permissions,
        "entry_point": "assets/info.json",
        "assets": ["assets/info.json"],
    }
    if os_tags:
        # Optional in the manifest; when absent the host defaults to ["macos"].
        manifest["platforms"] = os_tags
    if arch_tags:
        # Advisory metadata for tooling; the host selects binaries by filename
        # tags (PlatformTags.h), not by this field.
        manifest["architectures"] = arch_tags

    info = {"name": args.name, "description": args.description}

    plan = Plan(manifest=manifest, info=info)
    pending_os: Optional[str] = None
    pending_arch: Optional[str] = None
    default_arch = arch_tags[0] if len(arch_tags) == 1 else None

    for kind, value in getattr(args, "_events", []):
        if kind == "binary_os":
            if value not in KNOWN_OS:
                raise BuildError(f"unknown --binary-os '{value}'")
            pending_os = value
        elif kind == "binary_arch":
            if value not in KNOWN_ARCH:
                raise BuildError(f"unknown --binary-arch '{value}'")
            pending_arch = value
        elif kind == "binary":
            path = Path(value)
            ext = path.suffix.lower()
            if ext not in NATIVE_EXTS:
                raise BuildError(
                    f"binary '{path}' has extension '{ext}'; native extensions: "
                    f"{', '.join(NATIVE_EXTS)}"
                )
            if not path.is_file():
                raise BuildError(f"binary not found: {path}")
            os_tag = pending_os or DEFAULT_EXT_OS[ext]
            arch_tag = pending_arch or default_arch or "any"
            if os_tags and os_tag not in os_tags and "any" not in os_tags:
                raise BuildError(
                    f"binary {path} declares os '{os_tag}' but --os-tags is "
                    f"{', '.join(os_tags)}"
                )
            if arch_tags and arch_tag not in arch_tags and "any" not in arch_tags:
                raise BuildError(
                    f"binary {path} declares arch '{arch_tag}' but --arch-tags is "
                    f"{', '.join(arch_tags)}"
                )
            archive_name = f"{args.name}-{os_tag}-{arch_tag}{ext}"
            if not tagged_name_ok(archive_name, os_tag, arch_tag):
                raise BuildError(f"derived binary name is not host-parseable: {archive_name}")
            plan.binaries.append(Payload(archive_name, source=path, mode=0o755, native=True))
        elif kind == "asset":
            src_text, sep, dest = str(value).partition("=")
            src = Path(src_text)
            if not src.is_file():
                raise BuildError(f"asset not found: {src}")
            archive_name = dest if sep else src.name
            if not safe_relative_path(archive_name):
                raise BuildError(f"unsafe --asset destination: {archive_name}")
            plan.assets.append(Payload(archive_name, source=src, mode=0o644))

    # Dependency dylibs have no ordering dependency on --binary flags.
    for value in args.lib:
        src = Path(value)
        if not src.is_file():
            raise BuildError(f"dependency dylib not found: {src}")
        archive_name = f"lib/{src.name}"
        if not safe_relative_path(archive_name):
            raise BuildError(f"unsafe --lib destination: {archive_name}")
        plan.libs.append(Payload(archive_name, source=src, mode=0o755, native=True))

    if not plan.binaries and args.type in ("tab", "tool"):
        raise BuildError(f"type '{args.type}' requires at least one --binary native entry")
    if args.type in ("theme", "inject") and plan.binaries:
        raise BuildError(f"type '{args.type}' cannot include native binaries")

    # A legacy fallback native_entry is required for tab/tool by the host and
    # is only consulted when NO tagged binary exists; pointing it at the first
    # real binary keeps legacy hosts working on that platform.
    if plan.binaries and args.type in ("tab", "tool"):
        manifest["native_entry"] = plan.binaries[0].archive_name

    ensure_unique(plan.payloads())
    return plan


def cg100_plan(args: argparse.Namespace) -> Plan:
    # Defaults mirror tools/make_bundled_plugin.py and are derived from this
    # script's location, so the repo may be renamed without touching this tool.
    repo = Path(__file__).resolve().parent.parent
    default_app = repo.parent / "build-ui3" / "CasioEmuMsvc" / "CasioEmuMsvc.app"
    core = Path(args.core) if args.core else (
        default_app / "Contents/MacOS/plugins/CG100SH4.plugin")
    lib = Path(args.lib[0]) if args.lib else (
        default_app / "Contents/MacOS/plugins/lib/libcig100core.dylib")
    for path, label in ((core, "core dylib"), (lib, "dependency dylib")):
        if not path.is_file():
            raise BuildError(f"{label} not found: {path}")
    plan = Plan(
        manifest=json.loads(json.dumps(CG100_MANIFEST)),
        info=json.loads(json.dumps(CG100_INFO)),
        binaries=[Payload("CG100SH4.plugin", source=core, mode=0o755, native=True)],
        libs=[Payload("lib/libcig100core.dylib", source=lib, mode=0o755, native=True)],
    )
    ensure_unique(plan.payloads())
    return plan


def ensure_unique(payloads: List[Payload]) -> None:
    seen: Dict[str, str] = {}
    for payload in payloads:
        folded = payload.archive_name.lower()
        if folded in seen:
            raise BuildError(
                f"duplicate/case-colliding ZIP entry '{payload.archive_name}' "
                f"(collides with '{seen[folded]}')"
            )
        seen[folded] = payload.archive_name


# --------------------------------------------------------------------------- #
# ZIP writing + verification
# --------------------------------------------------------------------------- #
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


def expected_bytes(payload: Payload) -> bytes:
    if payload.data is not None:
        return payload.data
    assert payload.source is not None
    return payload.source.read_bytes()


def write_and_verify(plan: Plan, out: Path) -> Tuple[List[zipfile.ZipInfo], str]:
    payloads = plan.payloads()
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    if tmp.exists():
        tmp.unlink()

    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as archive:
        for payload in payloads:
            add_bytes(archive, payload.archive_name, expected_bytes(payload), payload.mode)
    os.replace(tmp, out)

    # --- verification: magic, entries, bytes, modes, JSON ------------------ #
    with out.open("rb") as handle:
        if handle.read(4) != b"PK\x03\x04":
            raise BuildError("output does not have ZIP magic")
    with zipfile.ZipFile(out, "r") as archive:
        infos = archive.infolist()
        actual = {i.filename for i in infos}
        expected = {p.archive_name for p in payloads}
        if actual != expected:
            raise BuildError(
                f"entry list mismatch: missing={sorted(expected - actual)}, "
                f"extra={sorted(actual - expected)}"
            )
        for payload in payloads:
            info = archive.getinfo(payload.archive_name)
            actual_mode = (info.external_attr >> 16) & 0o7777
            expected_mode = payload.mode
            if actual_mode != expected_mode:
                raise BuildError(
                    f"mode mismatch for {payload.archive_name}: "
                    f"got {oct(actual_mode)}, expected {oct(expected_mode)}"
                )
            if info.create_system != 3:
                raise BuildError(f"{payload.archive_name}: create_system is not Unix (3)")
            if archive.read(payload.archive_name) != expected_bytes(payload):
                raise BuildError(f"content mismatch for {payload.archive_name}")
        for json_name in ("plugin.json", "assets/info.json"):
            document = json.loads(archive.read(json_name))
            if not isinstance(document, dict):
                raise BuildError(f"{json_name} is not a JSON object")
        manifest = json.loads(archive.read("plugin.json"))
        if manifest.get("schema") != 1:
            raise BuildError("generated plugin.json has schema != 1")
    return infos, sha256(out)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--preset", choices=("cg100-sh4",),
                        help="special preset: reproduce the bundled CG100 SH4 package")
    parser.add_argument("--name", help="plugin display name (1-80 chars)")
    parser.add_argument("--id", help="reverse-DNS plugin id, e.g. com.example.hello")
    parser.add_argument("--version", default="1.0.0", help="semver plugin version")
    parser.add_argument("--author", default="", help="author string")
    parser.add_argument("--description", default="", help="description (<=500 chars)")
    parser.add_argument("--min-host", default=DEFAULT_HOST_VERSION,
                        help=f"minimum host version (semver, default {DEFAULT_HOST_VERSION})")
    parser.add_argument("--os-tags", help="comma-separated OS tags: " + ", ".join(KNOWN_OS))
    parser.add_argument("--arch-tags", help="comma-separated arch tags: " + ", ".join(KNOWN_ARCH))
    parser.add_argument("--permissions",
                        help="comma-separated permissions: " + ", ".join(PERMISSIONS))
    parser.add_argument("--type", default="tool", choices=PLUGIN_TYPES,
                        help="plugin type (default: tool)")
    parser.add_argument("--binary", action=OrderedAction, default=[], metavar="PATH",
                        help="native binary to embed (repeatable)")
    parser.add_argument("--binary-os", action=OrderedAction, default=[], metavar="OS",
                        help="OS tag for following --binary values (sticky, repeatable)")
    parser.add_argument("--binary-arch", action=OrderedAction, default=[], metavar="ARCH",
                        help="arch tag for following --binary values (sticky, repeatable)")
    parser.add_argument("--asset", action=OrderedAction, default=[], metavar="SRC[=DEST]",
                        help="extra data asset (repeatable; DEST defaults to basename)")
    parser.add_argument("--lib", action="append", default=[], metavar="PATH",
                        help="dependency dylib to embed under lib/ (repeatable)")
    parser.add_argument("--core", help="[cg100-sh4 preset] raw CG100SH4.plugin binary")
    parser.add_argument("--out", type=Path, required=True, help="output .plugin ZIP path")
    return parser


def main() -> int:
    parser = make_parser()
    args = parser.parse_args()
    try:
        if not args.preset:
            for required in ("name", "id"):
                if getattr(args, required) is None:
                    raise BuildError(f"--{required} is required unless --preset is used")
        plan = cg100_plan(args) if args.preset else build_plan(args)
        infos, digest = write_and_verify(plan, args.out)
    except BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"Wrote plugin: {args.out}")
    print(f"  SHA-256: {digest}")
    print(f"  Entries ({len(infos)}):")
    for info in infos:
        mode = (info.external_attr >> 16) & 0o777
        print(f"    {info.filename}  {oct(mode)}  {info.file_size} bytes")
    print(f"  Manifest: id={plan.manifest['id']} type={plan.manifest['type']} "
          f"permissions={plan.manifest['permissions']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
