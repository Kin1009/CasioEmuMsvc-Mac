cmake_minimum_required(VERSION 3.18)
project(CG100SH4Windows LANGUAGES CXX)

# Standalone MinGW-w64 cross-build of the CG100 SH4 architecture plugin for
# the Windows host.  Source is the current shared plugin (casio-tools/temp/
# cg100-plugin-mac/src/dllmain.cpp) ported to Win32:
#   * dlfcn/dlsym(RTLD_DEFAULT) -> GetProcAddress(GetModuleHandle(nullptr))
#   * extern "C" ABI symbols are __declspec(dllexport)
#   * cg100core.dll is bound through a generated import library
#   * the host's own ImGui sources are compiled in (one private copy)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../CasioEmuMsvc/src" CACHE PATH "Host CasioEmuMsvc/src")
set(IMGUI_DIR "${HOST_SRC}/Gui/imgui")
set(PLUGIN_API_DIR "${HOST_SRC}/Plugin")
set(SDL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../SDL/include")

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)

add_library(CG100SH4Windows SHARED
    dllmain.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(CG100SH4Windows PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PLUGIN_API_DIR}
    ${IMGUI_DIR}
    ${SDL_INCLUDE_DIR}
)

set_target_properties(CG100SH4Windows PROPERTIES
    PREFIX ""
    OUTPUT_NAME "CG100SH4"
    SUFFIX ".dll"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/artifacts"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/artifacts"
)

target_compile_definitions(CG100SH4Windows PRIVATE
    UNICODE _UNICODE _WIN32_WINNT=0x0601
    NOMINMAX
    CASIOEMU_PLUGIN_API
)

target_link_libraries(CG100SH4Windows PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libcg100core.a"
    imm32 gdi32 user32 shell32 kernel32
)

if(NOT MSVC)
    target_compile_options(CG100SH4Windows PRIVATE -Wall -Wno-unused-parameter -Wno-unused-variable)
    target_link_options(CG100SH4Windows PRIVATE -static-libgcc -static-libstdc++)
endif()
