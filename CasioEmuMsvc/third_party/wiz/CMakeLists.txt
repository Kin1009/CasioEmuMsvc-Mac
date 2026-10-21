# CMakeLists.txt - the Wiz / CWiz toolchain, vendored for the emulator.
#
# The sources under src/ and the data files under data/ are a copy of the
# `compiler/` tree of the Wiz repository (see README.md). Only the core library
# is built here: the CLI tools and the test suite stay in that repository.
#
# CasioEmuMsvc links `wizcore` and uses it from src/Gui/WizToolchain.cpp, so the
# debugger can compile Wiz (.wz) and CWiz (.cwiz) programs in-process and load
# the result into the running model.
cmake_minimum_required(VERSION 3.16)

file(GLOB WIZ_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(wizcore STATIC ${WIZ_CORE_SOURCES})
target_include_directories(wizcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(wizcore PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wizcore PRIVATE -Wall -Wextra)
endif()

# The directory holding runtime.asm and the boot templates, for embedders that
# cannot rely on the working directory. The GUI resolves it at run time
# (WIZ_DATA_DIR env, the app bundle, then this default).
target_compile_definitions(wizcore PUBLIC WIZ_VENDORED_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
