#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
mkdir -p "$HERE/lib"

clang++ -std=c++20 -dynamiclib -undefined dynamic_lookup -fPIC \
    -I"$ROOT/CasioEmuMsvc/src" \
    -I"$ROOT/CasioEmuMsvc/src/Plugin" \
    -I"$ROOT/CasioEmuMsvc/src/Gui/imgui" \
    -o "$HERE/lib/tab_notes.dylib" \
    "$HERE/src/tab_notes.cpp"

echo "Built $HERE/lib/tab_notes.dylib"
