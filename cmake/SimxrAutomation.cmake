include(FetchContent)
enable_language(C)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(simxr_luau
    URL https://codeload.github.com/luau-lang/luau/tar.gz/9d7748a4f324213a18cb8283fa2a77c330225fa0
    URL_HASH SHA256=35c10aa3860fd69f465d79a40263ae28f88c5d64ec598010af29ab4df7ad97e3
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_GetProperties(simxr_luau)
if(NOT simxr_luau_POPULATED)
    FetchContent_Populate(simxr_luau)
    add_subdirectory("${simxr_luau_SOURCE_DIR}" "${simxr_luau_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
add_library(simxr_automation STATIC
    src/automation/RigRunner.cpp
    src/automation/RigProgress.cpp
    src/automation/RigRuntime.cpp
    src/automation/RigReport.cpp
    src/automation/ScriptEngine.cpp)
simxr_configure_target(simxr_automation)
target_link_libraries(simxr_automation PUBLIC simxr_control simxr_recording PRIVATE Luau.Compiler Luau.VM)
add_library(simxr_rig_controller STATIC src/app/services/RigController.cpp)
simxr_configure_target(simxr_rig_controller)
target_link_libraries(simxr_rig_controller PUBLIC simxr_automation simxr_connection PRIVATE simxr_app_platform)
add_executable(simxr_runner src/automation/RunnerMain.cpp)
simxr_configure_target(simxr_runner)
simxr_set_output_directory(simxr_runner)
target_link_libraries(simxr_runner PRIVATE simxr_automation)
if(BUILD_TESTING)
    simxr_add_test(rig_controller_tests tests/app/RigControllerTests.cpp rig_controller)
    target_link_libraries(rig_controller_tests PRIVATE simxr_rig_controller)
    if(TARGET recording_controller_tests)
        target_link_libraries(recording_controller_tests PRIVATE simxr_automation)
        target_compile_definitions(recording_controller_tests PRIVATE SIMXR_TEST_AUTOMATION=1)
    endif()
    simxr_add_test(rig_runner_tests tests/automation/RigRunnerTests.cpp rig_runner)
    target_link_libraries(rig_runner_tests PRIVATE simxr_automation)
    if(TARGET control_endpoint_host AND Python3_Interpreter_FOUND)
        add_test(NAME simulation_control COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_simulation_control.py" "$<TARGET_FILE:control_endpoint_host>"
            "$<TARGET_FILE:simxr_runner>")
        set_tests_properties(simulation_control PROPERTIES TIMEOUT 30)
        add_test(NAME sequence_control COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_sequence_control.py" "$<TARGET_FILE:control_endpoint_host>")
        set_tests_properties(sequence_control PROPERTIES TIMEOUT 30)
    endif()
endif()
install(TARGETS simxr_runner RUNTIME DESTINATION bin)
install(FILES "${simxr_luau_SOURCE_DIR}/LICENSE.txt" DESTINATION share/licenses/simxr_runner RENAME Luau.txt)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/examples/rigs/" DESTINATION share/simxr/rigs)
