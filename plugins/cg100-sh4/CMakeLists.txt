cmake_minimum_required(VERSION 3.15)
project(CG100NativeSH4 LANGUAGES CXX)

# Self-contained native C++ SH4 / fx-CG100 architecture plugin for
# CasioEmuMsvc. No host source tree, ImGui, SDL or Go dylib is required:
# the ABI is vendored under abi/ and SDL entry points are resolved at runtime.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Android: build only the plugin shared object.  The host-side harness/test
# binaries cannot be executed by the cross toolchain and are not packaged.
if(ANDROID)
  set(SH4_BUILD_SELFTEST OFF CACHE BOOL "Android: no host self-test" FORCE)
  set(SH4_BUILD_VALIDATE OFF CACHE BOOL "Android: no host harness" FORCE)
  set(SH4_BUILD_MOCK OFF CACHE BOOL "Android: no host mock" FORCE)
  set(SH4_BUILD_TESTS OFF CACHE BOOL "Android: no host tests" FORCE)
endif()

set(SH4_SOURCES
    src/plugin.cpp
    sh4_interp.cpp
    cg100_mmio.cpp
)

add_library(CG100NativeSH4 SHARED ${SH4_SOURCES})

# The repository root holds the real interpreter + the native_core.h adapter.
target_include_directories(CG100NativeSH4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/abi
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(CG100NativeSH4 PRIVATE
    CASIOEMU_PLUGIN_API
    SH4_NATIVE_PLUGIN
)

# Output directory is derived from the generator so single-config (Ninja /
# Makefiles) and multi-config (Visual Studio) builds both work.
set(SH4_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/bin")

if(APPLE)
    set_target_properties(CG100NativeSH4 PROPERTIES
        PREFIX ""
        SUFFIX ".plugin"
        OUTPUT_NAME "CG100NativeSH4"
        LIBRARY_OUTPUT_DIRECTORY "${SH4_OUTPUT_DIR}"
    )
    target_compile_options(CG100NativeSH4 PRIVATE
        -fvisibility=default -Wall -Wextra -Wno-unused-parameter
    )
    # SDL functions are looked up with dlsym(RTLD_DEFAULT, ...); no link dep.
    target_link_options(CG100NativeSH4 PRIVATE -undefined dynamic_lookup)
elseif(ANDROID)
    # The host picks the embedded library by its platform tag:
    #   cg100sh4-android-arm64-android.so
    if(ANDROID_ABI STREQUAL "arm64-v8a")
      set(SH4_ANDROID_TAG "arm64-android")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
      set(SH4_ANDROID_TAG "any")
    elseif(ANDROID_ABI STREQUAL "x86_64")
      set(SH4_ANDROID_TAG "x64")
    elseif(ANDROID_ABI STREQUAL "x86")
      set(SH4_ANDROID_TAG "x32")
    else()
      set(SH4_ANDROID_TAG "any")
    endif()
    set_target_properties(CG100NativeSH4 PROPERTIES
        PREFIX "lib"
        SUFFIX ".so"
        OUTPUT_NAME "cg100sh4-android-${SH4_ANDROID_TAG}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/artifacts/android/${ANDROID_ABI}"
    )
    target_compile_options(CG100NativeSH4 PRIVATE
        -fvisibility=default -Wall -Wextra -Wno-unused-parameter
    )
    target_compile_definitions(CG100NativeSH4 PRIVATE
        "CASIOEMU_PLATFORM_OS=\"android\""
        "CASIOEMU_PLATFORM_ARCH=\"${SH4_ANDROID_TAG}\""
    )
elseif(WIN32)
    set_target_properties(CG100NativeSH4 PROPERTIES
        PREFIX ""
        SUFFIX ".dll"
        OUTPUT_NAME "CasioEmuMsvc.Plugin.CG100SH4"
        RUNTIME_OUTPUT_DIRECTORY "${SH4_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${SH4_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${SH4_OUTPUT_DIR}"
    )
    # Multi-config generators append a config subdirectory; collapse it.
    foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(CG100NativeSH4 PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${cfg} "${SH4_OUTPUT_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_${cfg} "${SH4_OUTPUT_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${SH4_OUTPUT_DIR}"
        )
    endforeach()
    target_compile_definitions(CG100NativeSH4 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    set_target_properties(CG100NativeSH4 PROPERTIES
        PREFIX ""
        SUFFIX ".plugin"
        OUTPUT_NAME "CG100NativeSH4"
        LIBRARY_OUTPUT_DIRECTORY "${SH4_OUTPUT_DIR}"
    )
    target_compile_options(CG100NativeSH4 PRIVATE
        -fvisibility=default -Wall -Wextra -Wno-unused-parameter
    )
endif()

# Optional standalone sanity test for the register model / bus / decoder.
# It never boots the CG100 OS.
option(SH4_BUILD_SELFTEST "Build the native core self-test executable" ON)
if(SH4_BUILD_SELFTEST)
    add_executable(sh4_native_selftest tests/native_core_selftest.cpp src/sh4_cpu.cpp src/mmio.cpp)
    target_include_directories(sh4_native_selftest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/abi
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set_target_properties(sh4_native_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SH4_OUTPUT_DIR}"
    )
    if(WIN32)
        foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
            set_target_properties(sh4_native_selftest PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${cfg} "${SH4_OUTPUT_DIR}"
            )
        endforeach()
    endif()
endif()

# ===========================================================================
# Build-system / validation harness targets (integration agent).
# These are independent of the native core sources above so the harness can be
# built and run while the interpreter is still in progress.
# ===========================================================================
set(SH4_HARNESS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/artifacts")
file(MAKE_DIRECTORY "${SH4_HARNESS_DIR}")

option(SH4_BUILD_VALIDATE "Build dlopen/LoadLibrary load+registration harness" ON)
option(SH4_BUILD_MOCK    "Build minimal ABI mock plugin for harness self-test" ON)
option(SH4_BUILD_TESTS   "Build interpreter unit-test executable" ON)

# Load / registration validation harness.
if(SH4_BUILD_VALIDATE)
    add_executable(sh4_validate validate.cpp)
    target_include_directories(sh4_validate PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/abi
    )
    set_target_properties(sh4_validate PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SH4_HARNESS_DIR}")
    if(WIN32)
        target_link_libraries(sh4_validate PRIVATE kernel32)
    else()
        target_link_libraries(sh4_validate PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

# Minimal ABI-conformant mock plugin: proves the harness + build in isolation.
if(SH4_BUILD_MOCK)
    if(WIN32)
        add_library(sh4_mock_plugin SHARED mock_plugin.cpp)
        set_target_properties(sh4_mock_plugin PROPERTIES
            OUTPUT_NAME "CasioEmuMsvc.Plugin.CG100SH4.Mock"
            PREFIX "" SUFFIX ".dll")
    else()
        add_library(sh4_mock_plugin MODULE mock_plugin.cpp)
        set_target_properties(sh4_mock_plugin PROPERTIES
            OUTPUT_NAME "sh4_mock" PREFIX "lib" SUFFIX ".plugin")
    endif()
    target_include_directories(sh4_mock_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/abi
    )
    set_target_properties(sh4_mock_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${SH4_HARNESS_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${SH4_HARNESS_DIR}")
endif()

# Interpreter unit tests (self-contained; grows with the interpreter API).
if(SH4_BUILD_TESTS)
    add_executable(sh4_interpreter_tests tests/test_interpreter.cpp)
    set_target_properties(sh4_interpreter_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SH4_HARNESS_DIR}")
    enable_testing()
    add_test(NAME sh4_interpreter COMMAND sh4_interpreter_tests)

    # Real CPU/MMIO interpreter unit tests (big-endian fetch, add, mov.w, bra,
    # bt, delay slots, memory mapping, KEYSC IRQ).
    add_executable(cg100_interp_tests test_interp.cpp sh4_interp.cpp cg100_mmio.cpp)
    target_include_directories(cg100_interp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(cg100_interp_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SH4_HARNESS_DIR}")
    add_test(NAME cg100_interp COMMAND cg100_interp_tests)
endif()
