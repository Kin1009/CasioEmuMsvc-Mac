#!/usr/bin/env bash
# Build a tiny cross-platform "hello" .plugin from DUMMY binaries and list it.
# These are placeholder text files, NOT real native libraries — they only
# demonstrate archive layout, naming, modes and host-side selection.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUMMY="$ROOT/docs/examples/dummy"
OUT="${1:-/tmp/hello.plugin}"

python3 "$ROOT/tools/build_plugin.py" \
  --name hello --id com.example.hello --version 1.0.0 \
  --author "Example Author" --description "Tiny cross-platform example plugin." \
  --type tool --permissions ui.shell \
  --os-tags macos,linux,windows,android \
  --arch-tags x64,arm64,arm64-android \
  --binary-os macos   --binary-arch x64           --binary "$DUMMY/hello-core.dylib" \
  --binary-os macos   --binary-arch arm64         --binary "$DUMMY/hello-core-arm64.dylib" \
  --binary-os linux   --binary-arch x64           --binary "$DUMMY/hello-core.so" \
  --binary-os windows --binary-arch x64           --binary "$DUMMY/hello-core.dll" \
  --binary-os android --binary-arch arm64-android --binary "$DUMMY/hello-core-android.so" \
  --out "$OUT"

python3 - "$OUT" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    print("\nzipfile listing (name, Unix mode, size):")
    for i in z.infolist():
        print(f"  {i.filename:40s} {oct((i.external_attr >> 16) & 0o777):>6s} {i.file_size:>5d}")
PY
