#!/usr/bin/env bash
# Build the CG100 SH4 architecture plugin as a native Linux x86-64 .so and
# wrap it in a platform-tagged .plugin ZIP for CasioEmuMsvc on Linux.
#
# Output (default OUT_DIR = <repo>/build-linux/plugin):
#   libcig100core.so        Go c-shared SH4 core (ELF x86-64)
#   CG100SH4.so             native plugin (ELF x86-64, RPATH $ORIGIN:$ORIGIN/lib)
#   CG100SH4.plugin         ZIP: plugin.json, assets/info.json,
#                           CG100SH4-linux-x64.so, lib/libcig100core.so
#
# Overrides (environment):
#   CASIO_LINUX_TOOLCHAIN_ROOT  cross toolchain root
#   GO_BIN                      Go 1.21+ binary (default /usr/local/go/bin/go)
#   CG100_GO_SRC                Go core module dir (default from sibling checkout)
#   OUT_DIR                     output directory
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd -P)"
HOST_SRC="$REPO_ROOT/CasioEmuMsvc/src"
PLUGIN_SRC="$REPO_ROOT/CasioEmuMsvc/plugins/CG100SH4/src"
SDL_INC="${SDL_INC:-$REPO_ROOT/SDL/include}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build-linux/plugin}"
TOOL_ROOT="${CASIO_LINUX_TOOLCHAIN_ROOT:-/Volumes/Code/casio-tools/toolchains/x86_64-unknown-linux-gnu}"
GO_BIN="${GO_BIN:-/usr/local/go/bin/go}"
CG100_GO_SRC="${CG100_GO_SRC:-/Volumes/Code/cg100-os/emu_go}"

# Accept both the outer archive dir and the inner toolchain dir.
if [ -d "$TOOL_ROOT/x86_64-unknown-linux-gnu/bin" ] && [ ! -d "$TOOL_ROOT/bin" ]; then
    TOOL_ROOT="$TOOL_ROOT/x86_64-unknown-linux-gnu"
fi
CXX="$TOOL_ROOT/bin/x86_64-linux-gnu-g++"
CC="$TOOL_ROOT/bin/x86_64-linux-gnu-gcc"
READELF="$TOOL_ROOT/bin/x86_64-linux-gnu-readelf"
NM="$TOOL_ROOT/bin/x86_64-linux-gnu-nm"

mkdir -p "$OUT_DIR" "$OUT_DIR/obj"

# 1. Cross-build the Go SH4 core for linux/amd64 with CGO enabled.
echo "==> Building libcig100core.so (Go, CGO, linux/amd64)"
(cd "$CG100_GO_SRC" && \
    CC="$CC" CGO_ENABLED=1 GOOS=linux GOARCH=amd64 \
    "$GO_BIN" build -buildmode=c-shared -o "$OUT_DIR/libcig100core.so" ./cmd/cg100core)

# 2. Compile the plugin's dllmain.cpp and a private (hidden) copy of ImGui.
echo "==> Compiling CG100SH4.so"
CXXFLAGS=(-std=c++20 -fPIC -O2 -Wall -Wno-unused-parameter
    -DCASIOEMU_PLUGIN_API
    -I"$PLUGIN_SRC" -I"$HOST_SRC/Plugin" -I"$HOST_SRC/Gui/imgui" -I"$SDL_INC")
"$CXX" "${CXXFLAGS[@]}" -c "$PLUGIN_SRC/dllmain.cpp" -o "$OUT_DIR/obj/dllmain.o"
for src in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp; do
    "$CXX" -std=c++20 -fPIC -O2 -fvisibility=hidden \
        -I"$HOST_SRC/Gui/imgui" -c "$HOST_SRC/Gui/imgui/$src" \
        -o "$OUT_DIR/obj/${src%.cpp}.o"
done

# 3. Link.  Do NOT link SDL: the plugin resolves SDL entry points from the
#    host process with dlsym(RTLD_DEFAULT, ...).  Static libstdc++/libgcc keep
#    the .so independent of the build machine's C++ runtime version.
"$CXX" -shared -o "$OUT_DIR/CG100SH4.so" \
    "$OUT_DIR/obj/dllmain.o" \
    "$OUT_DIR/obj/imgui.o" "$OUT_DIR/obj/imgui_draw.o" \
    "$OUT_DIR/obj/imgui_tables.o" "$OUT_DIR/obj/imgui_widgets.o" \
    -L"$OUT_DIR" -lcig100core \
    -Wl,-rpath,'$ORIGIN:$ORIGIN/lib' -ldl -static-libgcc -static-libstdc++

# 4. Package as a platform-tagged .plugin (ZIP).
echo "==> Packaging CG100SH4.plugin"
python3 "$REPO_ROOT/CasioEmuMsvc/tools/build_plugin.py" \
    --name CG100SH4 --id com.casioemu.arch.cg100sh4 --version 1.0.0 \
    --author "CasioEmuMsvc Linux port" \
    --description "Native SH4 emulator core for the CG100 board model (Linux x86-64); bundles libcig100core.so inside the container." \
    --min-host 2.1.0 --os-tags linux --arch-tags x64 \
    --permissions ui.shell,arch --type tool \
    --binary-os linux --binary-arch x64 --binary "$OUT_DIR/CG100SH4.so" \
    --lib "$OUT_DIR/libcig100core.so" \
    --out "$OUT_DIR/CG100SH4.plugin"

# 5. Sanity checks on the produced ELF artifacts.
echo "==> Verification"
file "$OUT_DIR/libcig100core.so" "$OUT_DIR/CG100SH4.so"
"$READELF" -d "$OUT_DIR/CG100SH4.so" | grep -E 'NEEDED|RPATH'
"$NM" -D --defined-only "$OUT_DIR/CG100SH4.so" \
    | grep -E 'fPluginLoad|fPluginLoadArch|casioemu_arch_core' | head -20
echo "==> Done: $OUT_DIR/CG100SH4.plugin"
