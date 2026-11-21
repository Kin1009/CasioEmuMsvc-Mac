# toolchain-linux.cmake -- macOS (or any Unix) host -> x86_64-unknown-linux-gnu
#
# Uses the prebuilt GCC cross toolchain from:
#   https://github.com/messense/homebrew-macos-cross-toolchains
# (release asset x86_64-unknown-linux-gnu-x86_64-darwin.tar.gz, GCC 15.2.0).
#
# Override the install location with -DCASIO_LINUX_TOOLCHAIN_ROOT=<dir> or the
# CASIO_LINUX_TOOLCHAIN_ROOT environment variable.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CASIOEMU_TARGET_OS "linux" CACHE STRING "CasioEmuMsvc platform tag (OS)")
set(CASIOEMU_TARGET_ARCH "x64" CACHE STRING "CasioEmuMsvc platform tag (arch)")

if(DEFINED ENV{CASIO_LINUX_TOOLCHAIN_ROOT} AND NOT CASIO_LINUX_TOOLCHAIN_ROOT)
    set(CASIO_LINUX_TOOLCHAIN_ROOT "$ENV{CASIO_LINUX_TOOLCHAIN_ROOT}" CACHE PATH "Linux cross toolchain root")
endif()
if(NOT CASIO_LINUX_TOOLCHAIN_ROOT)
    set(CASIO_LINUX_TOOLCHAIN_ROOT
        "/Volumes/Code/casio-tools/toolchains/x86_64-unknown-linux-gnu"
        CACHE PATH "Linux cross toolchain root")
endif()
if(NOT EXISTS "${CASIO_LINUX_TOOLCHAIN_ROOT}")
    message(FATAL_ERROR
        "Linux cross toolchain not found at ${CASIO_LINUX_TOOLCHAIN_ROOT}. "
        "Set CASIO_LINUX_TOOLCHAIN_ROOT or download the messense release asset.")
endif()

# The release tarball nests the actual toolchain one level deeper.  Accept
# either the outer archive directory or the inner toolchain directory.
if(NOT EXISTS "${CASIO_LINUX_TOOLCHAIN_ROOT}/bin" AND
   EXISTS "${CASIO_LINUX_TOOLCHAIN_ROOT}/x86_64-unknown-linux-gnu/bin")
    set(CASIO_LINUX_TOOLCHAIN_ROOT
        "${CASIO_LINUX_TOOLCHAIN_ROOT}/x86_64-unknown-linux-gnu")
endif()

file(GLOB _casio_gcc_candidates "${CASIO_LINUX_TOOLCHAIN_ROOT}/bin/*linux-gnu-gcc")
if(NOT _casio_gcc_candidates)
    message(FATAL_ERROR
        "No *linux-gnu-gcc driver found in ${CASIO_LINUX_TOOLCHAIN_ROOT}/bin")
endif()
list(SORT _casio_gcc_candidates)
list(GET _casio_gcc_candidates 0 _casio_gcc)
set(CMAKE_C_COMPILER "${_casio_gcc}")
string(REGEX REPLACE "-gcc$" "-g++" _casio_gxx "${_casio_gcc}")
if(NOT EXISTS "${_casio_gxx}")
    message(FATAL_ERROR "Matching C++ driver ${_casio_gxx} not found")
endif()
set(CMAKE_CXX_COMPILER "${_casio_gxx}")

# Locate the target sysroot inside the toolchain bundle.
set(CMAKE_SYSROOT "")
foreach(_cand
        "${CASIO_LINUX_TOOLCHAIN_ROOT}/x86_64-unknown-linux-gnu/sysroot"
        "${CASIO_LINUX_TOOLCHAIN_ROOT}/x86_64-unknown-linux-gnu"
        "${CASIO_LINUX_TOOLCHAIN_ROOT}/sysroot"
        "${CASIO_LINUX_TOOLCHAIN_ROOT}")
    if(EXISTS "${_cand}/usr/include")
        set(CMAKE_SYSROOT "${_cand}")
        break()
    endif()
endforeach()
if(NOT CMAKE_SYSROOT)
    message(WARNING "Could not locate a Linux sysroot under ${CASIO_LINUX_TOOLCHAIN_ROOT}; using toolchain defaults")
endif()

set(CMAKE_FIND_ROOT_PATH "${CASIO_LINUX_TOOLCHAIN_ROOT}")
if(CMAKE_SYSROOT)
    list(APPEND CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)  # host tools (git, python) allowed
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Keep the produced ELF runnable on a stock glibc x86_64 Linux distribution.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libgcc -static-libstdc++")
