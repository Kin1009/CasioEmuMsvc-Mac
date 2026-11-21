#!/usr/bin/env bash
# Build and package the CG100 SH4 architecture plugin for Windows x64.
#
# Requires: MinGW-w64 (CASIO_MINGW_ROOT), Python 3, and the current cg100core.dll
# from /Volumes/Code/casio-tools/temp/cg100-plugin-win/artifacts/cg100core.dll
# (39 exports: 23 cg100_* + 16 cg100_usb_*).  No Windows machine is needed.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$HERE/../.." && pwd -P)"
CORE_SOURCE="${CG100_CORE_DLL:-/Volumes/Code/casio-tools/temp/cg100-plugin-win/artifacts/cg100core.dll}"
OBJDUMP="${CASIO_MINGW_ROOT:-/usr/local/opt/mingw-w64/toolchain-x86_64}/bin/x86_64-w64-mingw32-objdump"
DLLTOOL="${CASIO_MINGW_ROOT:-/usr/local/opt/mingw-w64/toolchain-x86_64}/bin/x86_64-w64-mingw32-dlltool"
WINPTHREAD="${CASIO_MINGW_ROOT:-/usr/local/opt/mingw-w64/toolchain-x86_64}/x86_64-w64-mingw32/bin/libwinpthread-1.dll"

cd "$HERE"

# 1. Regenerate the import library from the core DLL's export table.
"$OBJDUMP" -p "$CORE_SOURCE" \
  | awk '/\[Ordinal\/Name Pointer\] Table/{f=1;next} /Import Table|Export Table/{if(f)f=0} f && /\+base\[/ && NF>=5 {print $NF}' \
  | grep -E '^[A-Za-z_]' | sort -u > /tmp/cg100_exports.txt
test "$(wc -l < /tmp/cg100_exports.txt)" -ge 39 || {
  echo "error: cg100core.dll exports fewer than 39 symbols" >&2
  exit 2
}
cp "$CORE_SOURCE" cg100core.dll
{
  echo 'LIBRARY cg100core.dll'
  echo 'EXPORTS'
  cat /tmp/cg100_exports.txt
} > cg100core.def
"$DLLTOOL" -d cg100core.def -D cg100core.dll -l libcg100core.a

# 2. Cross-compile the plugin DLL with the same MinGW toolchain as the host.
rm -rf build
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-windows-mingw.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j4

# 3. Verify the ABI exports before packaging.
"$OBJDUMP" -p artifacts/CG100SH4.dll \
  | awk '/\[Ordinal\/Name Pointer\] Table/{f=1;next} /Import Table|Export Table/{if(f)f=0} f && /\+base\[/ && NF>=5 {print $NF}' \
  | grep -E '^(fPluginLoad|fPluginLoadArch|casioemu_arch_core_)' | sort -u

# 4. Package a platform-tagged .plugin ZIP (Windows x64 native entry + deps).
mkdir -p artifacts
cp "$WINPTHREAD" artifacts/libwinpthread-1.dll
python3 "$ROOT/CasioEmuMsvc/tools/build_plugin.py" \
  --name CG100SH4 \
  --id com.casioemu.arch.cg100sh4 \
  --version 1.0.0 \
  --author CasioEmuMsvc \
  --description 'Native SH4 emulator core for the CG100 board model (Windows x64 build)' \
  --min-host 2.1.0 \
  --os-tags windows --arch-tags x64 \
  --permissions ui.shell,arch \
  --type tool \
  --binary-os windows --binary-arch x64 --binary artifacts/CG100SH4.dll \
  --lib cg100core.dll \
  --lib artifacts/libwinpthread-1.dll \
  --out artifacts/CG100SH4-Windows.plugin

echo "Packaged: $HERE/artifacts/CG100SH4-Windows.plugin"
