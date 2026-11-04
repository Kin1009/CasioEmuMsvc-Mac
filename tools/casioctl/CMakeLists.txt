# casioctl: standalone CLI debugger for a running CasioEmuMsvc session.
# POSIX (macOS/Linux) desktop only; no third-party dependencies.

if(CASIOEMU_WEB OR EMSCRIPTEN OR ANDROID OR WIN32 OR IOS)
    message(STATUS "casioctl: skipped (POSIX desktop only)")
    return()
endif()

set(CASIOCTL_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../CasioEmuMsvc/src/Ctl")

add_executable(casioctl
    main.cpp
    Commands.cpp
    CtlClient.cpp
    SelfTest.cpp
    "${CASIOCTL_SHARED_DIR}/CtlJson.cpp"
    "${CASIOCTL_SHARED_DIR}/CtlSession.cpp"
)

set_target_properties(casioctl PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(casioctl PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CASIOCTL_SHARED_DIR}"
)

file(CHMOD "${CMAKE_CURRENT_SOURCE_DIR}/e2e-test.sh"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

enable_testing()

add_test(NAME casioctl-selftest
    COMMAND $<TARGET_FILE:casioctl> selftest
)

add_test(NAME casioctl-help
    COMMAND $<TARGET_FILE:casioctl> --help
)

# ------------------------------------------------------------------ end-to-end
# Starts the real emulator in offscreen headless mode and drives it through the
# real casioctl binary. Skipped (exit 77) when no model directory is available;
# point CASIOCTL_E2E_MODEL at a model folder to enable it in any build tree.
set(CASIOCTL_E2E_MODEL "" CACHE PATH "Model directory for the casioctl end-to-end test")
if(NOT CASIOCTL_E2E_MODEL)
    foreach(_candidate
        "$ENV{CASIOCTL_TEST_MODEL}"
        "${CMAKE_SOURCE_DIR}/models/fx-580VN X"
        "${CMAKE_BINARY_DIR}/CasioEmuMsvc/CasioEmuMsvc.app/Contents/MacOS/models/fx-580VN X"
    )
        if(_candidate AND IS_DIRECTORY "${_candidate}")
            set(CASIOCTL_E2E_MODEL "${_candidate}" CACHE PATH "Model directory for the casioctl end-to-end test" FORCE)
            break()
        endif()
    endforeach()
endif()

if(TARGET CasioEmuMsvc)
    add_test(NAME casioctl-e2e
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/e2e-test.sh"
            "$<TARGET_FILE:CasioEmuMsvc>"
            "$<TARGET_FILE:casioctl>"
            "${CASIOCTL_E2E_MODEL}"
    )
    set_tests_properties(casioctl-e2e PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 240
    )

    # Shutdown-lifetime regression: open two sessions, then leave the main loop
    # while their LCD workers are running. Exit status must stay 0 (no SIGABRT).
    add_test(NAME screen-exit
        COMMAND "${CMAKE_SOURCE_DIR}/CasioEmuMsvc/tests/screen-exit-test.sh"
            "$<TARGET_FILE:CasioEmuMsvc>"
            "$<TARGET_FILE:casioctl>"
            "${CASIOCTL_E2E_MODEL}"
            3
    )
    set_tests_properties(screen-exit PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
    )
endif()
