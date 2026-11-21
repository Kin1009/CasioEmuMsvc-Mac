#!/usr/bin/env bash
# Cross-build the CG100 SH4 architecture plugin for Android and package it as
# an app-asset .plugin ZIP container.  No macOS-specific APIs are used.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$HERE/../.." && pwd -P)"
NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-/Volumes/Code/casio-tools/toolchains/android-ndk-r27c}}"
ABI="${ANDROID_ABI:-arm64-v8a}"
BUILD_DIR="${SH4_ANDROID_BUILD_DIR:-$HERE/build-android-$ABI}"
OUT="${SH4_ANDROID_PLUGIN_OUT:-$ROOT/app/src/main/assets/plugins/CG100SH4-Android.plugin}"

if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "Android NDK not found at $NDK (set ANDROID_NDK)" >&2
    exit 2
fi

echo "[1/3] configure ($ABI, NDK $NDK)"
cmake -S "$HERE" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-android.cmake" \
    -DANDROID_NDK="$NDK" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release

echo "[2/3] build CG100NativeSH4"
cmake --build "$BUILD_DIR" --target CG100NativeSH4 -- -j"${CASIO_BUILD_JOBS:-4}"

if [ "$ABI" = "arm64-v8a" ]; then
    TAG="arm64-android"
else
    TAG="any"
fi
SO="$HERE/artifacts/android/$ABI/libcg100sh4-android-$TAG.so"
if [ ! -f "$SO" ]; then
    echo "expected plugin library missing: $SO" >&2
    exit 3
fi

echo "[3/3] package $OUT"
python3 "$HERE/package-android.py" --so "$SO" --abi "$ABI" --out "$OUT"
file -b "$SO"
