# toolchain-android.cmake -- Android NDK cross build (arm64-v8a by default)
#
# Thin wrapper around the official NDK toolchain file.  Set ANDROID_NDK (env or
# cache) and optionally ANDROID_ABI / ANDROID_PLATFORM before configuring:
#
#   cmake -S . -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-android.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
#
# The default NDK location matches the direct download used by build-all.sh;
# override via -DANDROID_NDK=<path> or the ANDROID_NDK_HOME environment var.

if(DEFINED ENV{ANDROID_NDK} AND NOT ANDROID_NDK)
    set(ANDROID_NDK "$ENV{ANDROID_NDK}")
endif()
if(DEFINED ENV{ANDROID_NDK_HOME} AND NOT ANDROID_NDK)
    set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
endif()
if(NOT ANDROID_NDK)
    set(ANDROID_NDK "/Volumes/Code/casio-tools/toolchains/android-ndk-r27c")
endif()
if(NOT EXISTS "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    message(FATAL_ERROR
        "Android NDK toolchain file not found under ${ANDROID_NDK}. "
        "Set ANDROID_NDK to an NDK r25+ installation.")
endif()

if(NOT ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI")
endif()
if(NOT ANDROID_PLATFORM)
    set(ANDROID_PLATFORM "android-24" CACHE STRING "Android API level")
endif()
set(ANDROID_STL "c++_static" CACHE STRING "Android STL")

include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")

# The NDK toolchain already sets ANDROID/ANDROID_ABI.  Mirror them into the
# CasioEmuMsvc tag variables so detection is unambiguous even with old CMake.
set(CASIOEMU_TARGET_OS "android" CACHE STRING "CasioEmuMsvc platform tag (OS)")
if(ANDROID_ABI MATCHES "arm64-v8a")
    set(CASIOEMU_TARGET_ARCH "arm64-android" CACHE STRING "CasioEmuMsvc platform tag (arch)")
elseif(ANDROID_ABI MATCHES "armeabi-v7a")
    set(CASIOEMU_TARGET_ARCH "any" CACHE STRING "CasioEmuMsvc platform tag (arch)")
elseif(ANDROID_ABI MATCHES "x86_64")
    set(CASIOEMU_TARGET_ARCH "x64" CACHE STRING "CasioEmuMsvc platform tag (arch)")
elseif(ANDROID_ABI MATCHES "x86")
    set(CASIOEMU_TARGET_ARCH "x32" CACHE STRING "CasioEmuMsvc platform tag (arch)")
else()
    set(CASIOEMU_TARGET_ARCH "any" CACHE STRING "CasioEmuMsvc platform tag (arch)")
endif()
