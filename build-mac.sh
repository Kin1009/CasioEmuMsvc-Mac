#!/bin/bash

set -euo pipefail

INFO='\033[1;34m'
SUCCESS='\033[1;32m'
WARNING='\033[1;33m'
ERROR='\033[1;31m'
NC='\033[0m'

log() {
    echo -e "${INFO}[BUILD] $1${NC}"
}

ok() {
    echo -e "${SUCCESS}[OK] $1${NC}"
}

warn() {
    echo -e "${WARNING}[WARN] $1${NC}"
}

err() {
    echo -e "${ERROR}[ERROR] $1${NC}"
}

beep_10s() {
    end=$((SECONDS + 10))
    while [ $SECONDS -lt $end ]; do
        printf '\a'
        sleep 0.3
    done
}

trap 'err "Build terminated."; beep_10s' ERR INT TERM

if [ "$(uname)" != "Darwin" ]; then
    err "This script can only be run on macOS."
    exit 1
fi

START_TIME=$SECONDS

# ---------------- CLEAN ----------------
log "Cleaning build directory..."
time rm -rf build
time mkdir build
ok "Clean complete"

# ---------------- CONFIGURE ----------------
log "Configuring CMake (verbose mode)..."

CMAKE_CMD=(
    cmake -S . -B build
    -G Ninja
    -DCMAKE_C_COMPILER=clang
    -DCMAKE_CXX_COMPILER=clang++
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_EXECUTABLE=ON
)

echo -e "${INFO}Running:${NC}"
printf ' %q' "${CMAKE_CMD[@]}"
echo ""

time "${CMAKE_CMD[@]}"

ok "CMake configuration done"

# ---------------- BUILD ----------------
log "Starting build..."

if command -v sysctl >/dev/null 2>&1; then
    JOBS=$(sysctl -n hw.logicalcpu)
else
    JOBS=4
fi

echo -e "${INFO}Using ${JOBS} parallel jobs${NC}"

BUILD_CMD=(
    cmake --build build
    --
    -j"$JOBS"
)

echo -e "${INFO}Running:${NC}"
printf ' %q' "${BUILD_CMD[@]}"
echo ""

time "${BUILD_CMD[@]}"

# ---------------- DONE ----------------
ELAPSED=$((SECONDS - START_TIME))

echo ""
ok "Build completed successfully in ${ELAPSED}s"