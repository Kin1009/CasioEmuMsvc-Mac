#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
mkdir -p "$HERE/lib"

# Universal binary: the app ships arm64 + x86_64 slices, so the plugin must too.
ARCHS="${ARCHS:--arch arm64 -arch x86_64}"

# shellcheck disable=SC2086
clang++ -std=c++20 -dynamiclib -undefined dynamic_lookup -fPIC $ARCHS \
    -I"$ROOT/CasioEmuMsvc/src" \
    -I"$ROOT/CasioEmuMsvc/src/Plugin" \
    -I"$ROOT/CasioEmuMsvc/src/Gui/imgui" \
    -o "$HERE/lib/tab_notes.dylib" \
    "$HERE/src/tab_notes.cpp"

echo "Built $HERE/lib/tab_notes.dylib"
lipo -info "$HERE/lib/tab_notes.dylib"
