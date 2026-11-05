if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "gen_build_stamp.cmake requires -DOUTPUT=<path>")
endif()
if(NOT DEFINED REPO OR REPO STREQUAL "")
    set(REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

execute_process(
    COMMAND git -C "${REPO}" rev-parse --short HEAD
    OUTPUT_VARIABLE commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT commit)
    set(commit "unknown")
endif()

execute_process(
    COMMAND git -C "${REPO}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(dirty)
    string(APPEND commit "-dirty")
endif()

string(TIMESTAMP build_time "%m-%d %H:%M")

string(REPLACE "\\" "\\\\" commit_escaped "${commit}")
string(REPLACE "\"" "\\\"" commit_escaped "${commit_escaped}")
string(REPLACE "\\" "\\\\" time_escaped "${build_time}")
string(REPLACE "\"" "\\\"" time_escaped "${time_escaped}")

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT}" "const char* g_build_commit = \"${commit_escaped}\";\nconst char* g_build_time = \"${time_escaped}\";\n")
