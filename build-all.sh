#!/usr/bin/env bash
# build-all.sh -- configure + build the four CasioEmuMsvc target platforms.
#
#   ./build-all.sh                 # all four targets
#   ./build-all.sh macos linux     # a subset
#   ./build-all.sh --verify-only   # only re-check existing artifacts
#
# The repository root is derived from this script, so the checkout can be
# renamed freely (e.g. CasioEmuMsvc-Mac -> CasioEmuMsvc-Mod).  Toolchain and
# log locations are overridable through the CASIO_* environment variables.
set -uo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
TOOL_DIR="${CASIO_TOOLCHAIN_DIR:-/Volumes/Code/casio-tools/toolchains}"
LOG_DIR="${CASIO_BUILD_LOG_DIR:-/Volumes/Code/casio-tools/temp/build-4target-logs}"
JOBS="${CASIO_BUILD_JOBS:-4}"
mkdir -p "$LOG_DIR"

CCACHE_BIN="${CCACHE_BIN:-$(command -v ccache || true)}"
# Keeping two arguments even when ccache is absent avoids empty-array expansion
# under old Bash (macOS ships 3.2).  An empty launcher simply disables ccache.
launcher_args=(-DCMAKE_C_COMPILER_LAUNCHER="$CCACHE_BIN" -DCMAKE_CXX_COMPILER_LAUNCHER="$CCACHE_BIN")

ANDROID_NDK_DIR="${ANDROID_NDK:-${ANDROID_NDK_HOME:-$TOOL_DIR/android-ndk-r27c}}"
LINUX_TOOLCHAIN_DIR="${CASIO_LINUX_TOOLCHAIN_ROOT:-$TOOL_DIR/x86_64-unknown-linux-gnu}"

VERIFY_ONLY=0
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        --verify-only) VERIFY_ONLY=1 ;;
        all) TARGETS+=(macos linux windows android) ;;
        macos|linux|windows|android) TARGETS+=("$arg") ;;
        -h|--help)
            echo "Usage: $0 [macos] [linux] [windows] [android] [--verify-only]"
            echo "Env: CASIO_TOOLCHAIN_DIR, CASIO_BUILD_LOG_DIR, CASIO_BUILD_JOBS,"
            echo "     CASIO_LINUX_TOOLCHAIN_ROOT, ANDROID_NDK, CCACHE_BIN"
            exit 0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done
if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS=(macos linux windows android)
fi

RESULTS=()
log() { printf '%s\n' "$*" | tee -a "$LOG_DIR/build-all.log"; }

# describe_artifact <path> -- prints "file(1)" output or a clear error.
describe_artifact() {
    local path="$1"
    if [ -e "$path" ]; then
        file -b "$path"
    else
        printf 'MISSING: %s\n' "$path"
    fi
}

run_target() {
    local name="$1" build_dir="$2" expected="$3"
    shift 3
    local cfg_cmd=("$@")
    local rc=0
    local cmake_target="CasioEmuMsvc"
    if [ "$name" = "android" ]; then cmake_target="main"; fi

    log ""
    log "===== ${name} ====="
    log "build dir : ${build_dir}"
    log "artifact  : ${expected}"

    if [ "$VERIFY_ONLY" -eq 0 ]; then
        log "configure : ${cfg_cmd[*]}"
        "${cfg_cmd[@]}" >"$LOG_DIR/${name}-configure.log" 2>&1 || rc=$?
        if [ "$rc" -ne 0 ]; then
            log "CONFIGURE FAILED (rc=$rc); tail of ${LOG_DIR}/${name}-configure.log:"
            tail -n 30 "$LOG_DIR/${name}-configure.log" | tee -a "$LOG_DIR/build-all.log"
            RESULTS+=("${name}|FAIL|configure rc=$rc")
            return 1
        fi
        log "build     : cmake --build ${build_dir} --target ${cmake_target} -j${JOBS}"
        cmake --build "$build_dir" --target "$cmake_target" -- -j"$JOBS" \
            >"$LOG_DIR/${name}-build.log" 2>&1 || rc=$?
        if [ "$rc" -ne 0 ]; then
            log "BUILD FAILED (rc=$rc); tail of ${LOG_DIR}/${name}-build.log:"
            tail -n 40 "$LOG_DIR/${name}-build.log" | tee -a "$LOG_DIR/build-all.log"
            RESULTS+=("${name}|FAIL|build rc=$rc")
            return 1
        fi
    fi

    if [ -e "$expected" ]; then
        local info
        info="$(describe_artifact "$expected")"
        log "artifact  : ${info}"
        RESULTS+=("${name}|PASS|${info}")
        return 0
    fi
    log "ARTIFACT MISSING: ${expected}"
    RESULTS+=("${name}|FAIL|no artifact at ${expected}")
    return 1
}

for t in "${TARGETS[@]}"; do
    case "$t" in
        macos)
            run_target macos "$ROOT_DIR/build-ui3" \
                "$ROOT_DIR/build-ui3/CasioEmuMsvc/CasioEmuMsvc.app/Contents/MacOS/CasioEmuMsvc" \
                cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build-ui3" -G Ninja \
                    -DCMAKE_BUILD_TYPE=Release
            ;;
        linux)
            run_target linux "$ROOT_DIR/build-linux" \
                "$ROOT_DIR/build-linux/CasioEmuMsvc/CasioEmuMsvc" \
                cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build-linux" -G Ninja \
                    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-linux.cmake" \
                    -DCASIO_LINUX_TOOLCHAIN_ROOT="$LINUX_TOOLCHAIN_DIR" \
                    -DCMAKE_BUILD_TYPE=Release \
                    -DCASIOEMU_ENABLE_SENTRY=OFF \
                    -DCASIOEMU_PLUGIN_TESTS=OFF \
                    -DCASIOEMU_WIZ_TOOLCHAIN=ON \
                    -DBUILD_MCP_PLUGIN=OFF \
                    "${launcher_args[@]}"
            ;;
        windows)
            run_target windows "$ROOT_DIR/build-windows" \
                "$ROOT_DIR/build-windows/CasioEmuMsvc/CasioEmuMsvc.exe" \
                cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build-windows" -G Ninja \
                    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-windows-mingw.cmake" \
                    -DCMAKE_BUILD_TYPE=Release \
                    -DCASIOEMU_ENABLE_SENTRY=OFF \
                    -DCASIOEMU_PLUGIN_TESTS=OFF \
                    -DCASIOEMU_WIZ_TOOLCHAIN=ON \
                    -DBUILD_MCP_PLUGIN=OFF \
                    "${launcher_args[@]}"
            ;;
        android)
            # Android builds produce a shared library ("main") rather than an
            # executable; the NDK toolchain sets BUILD_EXECUTABLE=OFF.
            run_target android "$ROOT_DIR/build-android" \
                "$ROOT_DIR/build-android/CasioEmuMsvc/libmain.so" \
                cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build-android" -G Ninja \
                    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-android.cmake" \
                    -DANDROID_NDK="$ANDROID_NDK_DIR" \
                    -DANDROID_ABI=arm64-v8a \
                    -DANDROID_PLATFORM=android-24 \
                    -DCMAKE_BUILD_TYPE=Release \
                    -DCASIOEMU_ENABLE_SENTRY=OFF \
                    -DCASIOEMU_PLUGIN_TESTS=OFF \
                    -DCASIOEMU_WIZ_TOOLCHAIN=OFF \
                    -DBUILD_MCP_PLUGIN=OFF \
                    "${launcher_args[@]}"
            ;;
    esac
done

log ""
log "===== SUMMARY ====="
for r in "${RESULTS[@]}"; do
    log "$r"
done
