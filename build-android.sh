#!/usr/bin/env bash
# build-android.sh -- native library / APK / AAB build driver for the Android port.
#
#   ./build-android.sh native   # CMake + NDK -> build-android/CasioEmuMsvc/libmain.so
#   ./build-android.sh apk      # Gradle -> app/build/outputs/apk/.../CasioEmuMsvc-*.apk
#   ./build-android.sh aab      # Gradle -> app/build/outputs/bundle/.../*.aab
#   ./build-android.sh all      # native + APK
#   ./build-android.sh plugin   # CG100 SH4 .plugin asset for arm64-v8a
#
# Environment: ANDROID_NDK, ANDROID_HOME/ANDROID_SDK_ROOT, CASIO_BUILD_JOBS.
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-/Volumes/Code/casio-tools/toolchains/android-ndk-r27c}}"
ABI="${ANDROID_ABI:-arm64-v8a}"
JOBS="${CASIO_BUILD_JOBS:-4}"
MODE="${1:-native}"

native() {
    cmake -S "$ROOT" -B "$ROOT/build-android" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-android.cmake" \
        -DANDROID_NDK="$NDK" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM=android-24 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCASIOEMU_ENABLE_SENTRY=OFF \
        -DCASIOEMU_PLUGIN_TESTS=OFF \
        -DCASIOEMU_WIZ_TOOLCHAIN=ON \
        -DBUILD_MCP_PLUGIN=OFF || return $?
    cmake --build "$ROOT/build-android" --target main -- -j"$JOBS"
}

GRADLE_NDK_ARG="-Pandroid.ndkPath=$NDK"

case "$MODE" in
    native) native ;;
    apk)    cd "$ROOT" && ./gradlew :app:assembleDebug "$GRADLE_NDK_ARG" ;;
    aab)    cd "$ROOT" && ./gradlew :app:bundleDebug "$GRADLE_NDK_ARG" ;;
    plugin) "$ROOT/plugins/cg100-sh4/build-android.sh" ;;
    all)    native && cd "$ROOT" && ./gradlew :app:assembleDebug "$GRADLE_NDK_ARG" ;;
    *) echo "Usage: $0 {native|apk|aab|plugin|all}" >&2; exit 2 ;;
esac
