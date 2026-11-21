# toolchain-windows-mingw.cmake -- macOS host -> x86_64 Windows (MinGW-w64)
#
# Requires `brew install mingw-w64` (or an equivalent MinGW-w64 installation).
# Override with -DCASIO_MINGW_ROOT=<dir> or the CASIO_MINGW_ROOT environment
# variable.  The i686 toolchain is supported via CASIO_MINGW_ARCH=i686.

set(CASIO_MINGW_ARCH "x86_64" CACHE STRING "MinGW-w64 target arch (x86_64 or i686)")
set_property(CACHE CASIO_MINGW_ARCH PROPERTY STRINGS x86_64 i686)

if(CASIO_MINGW_ARCH STREQUAL "i686")
    set(CMAKE_SYSTEM_PROCESSOR x86)
    set(CASIOEMU_TARGET_ARCH "x32" CACHE STRING "CasioEmuMsvc platform tag (arch)")
else()
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
    set(CASIOEMU_TARGET_ARCH "x64" CACHE STRING "CasioEmuMsvc platform tag (arch)")
endif()
set(CMAKE_SYSTEM_NAME Windows)
set(CASIOEMU_TARGET_OS "windows" CACHE STRING "CasioEmuMsvc platform tag (OS)")

if(DEFINED ENV{CASIO_MINGW_ROOT} AND NOT CASIO_MINGW_ROOT)
    set(CASIO_MINGW_ROOT "$ENV{CASIO_MINGW_ROOT}" CACHE PATH "MinGW-w64 root")
endif()
if(NOT CASIO_MINGW_ROOT)
    # Follow the brew symlink instead of hardcoding a Cellar version.
    execute_process(
        COMMAND brew --prefix mingw-w64
        OUTPUT_VARIABLE _casio_brew_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_casio_brew_prefix)
        set(CASIO_MINGW_ROOT
            "${_casio_brew_prefix}/toolchain-${CASIO_MINGW_ARCH}"
            CACHE PATH "MinGW-w64 root")
    endif()
endif()
if(NOT CASIO_MINGW_ROOT OR NOT EXISTS "${CASIO_MINGW_ROOT}")
    message(FATAL_ERROR
        "MinGW-w64 toolchain not found. Install with `brew install mingw-w64` "
        "or set CASIO_MINGW_ROOT explicitly.")
endif()

set(_prefix "${CASIO_MINGW_ARCH}-w64-mingw32")
set(CMAKE_C_COMPILER   "${CASIO_MINGW_ROOT}/bin/${_prefix}-gcc")
set(CMAKE_CXX_COMPILER "${CASIO_MINGW_ROOT}/bin/${_prefix}-g++")
set(CMAKE_RC_COMPILER  "${CASIO_MINGW_ROOT}/bin/${_prefix}-windres")
set(CMAKE_ASM_COMPILER "${CASIO_MINGW_ROOT}/bin/${_prefix}-gcc")

set(CMAKE_FIND_ROOT_PATH "${CASIO_MINGW_ROOT}" "${CASIO_MINGW_ROOT}/${_prefix}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The project's WIN32 branch already requests -static / -static-libstdc++.
# Ask GCC to place runtime DLLs where the post-build steps expect them.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
