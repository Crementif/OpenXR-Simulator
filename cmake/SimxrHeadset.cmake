include("${CMAKE_CURRENT_LIST_DIR}/SimxrOpenXrLoader.cmake")
if(NOT SIMXR_OPENXR_LOADER_TARGET)
    message(FATAL_ERROR "The headset host requires the OpenXR loader. Set OPENXR_SDK or SIMXR_OPENXR_LOADER.")
endif()
add_library(simxr_headset STATIC
    src/headset/RuntimeSelection.cpp
    src/headset/OpenXrRuntime.cpp
    src/headset/HeadsetBodyCapabilities.cpp
    src/headset/HeadsetBody.cpp
    src/headset/HeadsetGraphics.cpp
    src/headset/HeadsetImageProbe.cpp
    src/headset/HeadsetImageBridge.cpp
    src/headset/HeadsetImages.cpp
    src/headset/HeadsetOutput.cpp
    src/headset/HeadsetProjection.cpp
    src/headset/HeadsetSession.cpp
    src/headset/HeadsetSpace.cpp
    src/headset/HeadsetActions.cpp
    src/headset/HeadsetControls.cpp
    src/headset/HeadsetHaptics.cpp
    src/headset/HeadsetTrackers.cpp
    src/headset/RuntimeInputTime.cpp
    src/headset/HeadsetTracking.cpp
    src/headset/HeadsetInputBridge.cpp
    src/headset/HeadsetTestPattern.cpp
    src/Json.cpp)
simxr_configure_target(simxr_headset)
target_link_libraries(simxr_headset PUBLIC ${SIMXR_OPENXR_LOADER_TARGET} simxr_vulkan_loader PRIVATE simxr_vulkan_control)
target_link_libraries(simxr_headset PUBLIC Vulkan::Headers)
if(WIN32)
    target_compile_definitions(simxr_headset PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    target_sources(simxr_headset PRIVATE src/headset/platform/windows/HostEnvironment.cpp
        src/headset/platform/windows/RuntimeClock.cpp src/headset/platform/windows/HostControl.cpp)
else()
    target_sources(simxr_headset PRIVATE src/headset/platform/linux/HostEnvironment.cpp
        src/headset/platform/linux/RuntimeClock.cpp src/headset/platform/linux/HostControl.cpp)
endif()
add_executable(simxr_headset_host src/headset/Main.cpp)
simxr_configure_target(simxr_headset_host)
simxr_set_output_directory(simxr_headset_host)
target_link_libraries(simxr_headset_host PRIVATE simxr_headset)
if(TARGET simxr_install_use)
    target_link_libraries(simxr_headset_host PRIVATE simxr_install_use)
endif()
if(MINGW)
    target_link_options(simxr_headset_host PRIVATE -municode)
endif()
install(TARGETS simxr_headset_host RUNTIME DESTINATION bin)
if(BUILD_TESTING)
    simxr_add_test(headset_body_tests tests/headset/HeadsetBodyTests.cpp headset_body)
    target_sources(headset_body_tests PRIVATE src/headset/HeadsetBody.cpp)
    target_link_libraries(headset_body_tests PRIVATE simxr_core)
    simxr_add_test(headset_body_capabilities_tests tests/headset/HeadsetBodyCapabilitiesTests.cpp headset_body_capabilities)
    target_sources(headset_body_capabilities_tests PRIVATE src/headset/HeadsetBodyCapabilities.cpp)
    target_link_libraries(headset_body_capabilities_tests PRIVATE simxr_core)
    simxr_add_test(headset_trackers_tests tests/headset/HeadsetTrackersTests.cpp headset_trackers)
    target_sources(headset_trackers_tests PRIVATE src/headset/HeadsetTrackers.cpp)
    target_link_libraries(headset_trackers_tests PRIVATE simxr_core)
    simxr_add_test(headset_projection_tests tests/headset/HeadsetProjectionTests.cpp headset_projection)
    target_sources(headset_projection_tests PRIVATE src/headset/HeadsetProjection.cpp)
    target_link_libraries(headset_projection_tests PRIVATE simxr_graphics)
    simxr_add_test(headset_output_tests tests/headset/HeadsetOutputTests.cpp headset_output)
    target_sources(headset_output_tests PRIVATE src/headset/HeadsetOutput.cpp src/headset/HeadsetProjection.cpp)
    target_link_libraries(headset_output_tests PRIVATE simxr_graphics)
    target_link_libraries(headset_output_tests PRIVATE Vulkan::Headers)
    simxr_add_test(headset_space_tests tests/headset/HeadsetSpaceTests.cpp headset_space)
    target_sources(headset_space_tests PRIVATE src/headset/HeadsetSpace.cpp)
    target_link_libraries(headset_space_tests PRIVATE simxr_core)
    simxr_add_test(headset_haptics_tests tests/headset/HeadsetHapticsTests.cpp headset_haptics)
    target_sources(headset_haptics_tests PRIVATE src/headset/HeadsetHaptics.cpp)
    target_link_libraries(headset_haptics_tests PRIVATE simxr_core)
    simxr_add_test(headset_actions_tests tests/headset/HeadsetActionsTests.cpp headset_actions)
    target_sources(headset_actions_tests PRIVATE src/headset/HeadsetActions.cpp src/headset/HeadsetControls.cpp src/headset/HeadsetTrackers.cpp)
    target_link_libraries(headset_actions_tests PRIVATE simxr_core)
    simxr_add_test(headset_tracking_tests tests/headset/HeadsetTrackingTests.cpp headset_tracking)
    target_sources(headset_tracking_tests PRIVATE src/headset/HeadsetTracking.cpp)
    target_link_libraries(headset_tracking_tests PRIVATE simxr_core)
    if(NOT WIN32)
        simxr_add_test(headset_images_tests tests/headset/HeadsetImagesTests.cpp headset_images)
        target_sources(headset_images_tests PRIVATE src/headset/HeadsetImages.cpp src/headset/HeadsetImageBridge.cpp)
        target_link_libraries(headset_images_tests PRIVATE simxr_control)
        target_link_libraries(headset_images_tests PRIVATE Vulkan::Headers)
        simxr_add_test(headset_image_bridge_tests tests/headset/HeadsetImageBridgeTests.cpp headset_image_bridge)
        target_sources(headset_image_bridge_tests PRIVATE src/headset/HeadsetImageBridge.cpp)
        target_link_libraries(headset_image_bridge_tests PRIVATE simxr_control)
        simxr_add_test(headset_input_bridge_tests tests/headset/HeadsetInputBridgeTests.cpp headset_input_bridge)
        target_sources(headset_input_bridge_tests PRIVATE src/headset/HeadsetInputBridge.cpp src/headset/HeadsetControls.cpp src/headset/HeadsetTracking.cpp src/headset/HeadsetHaptics.cpp)
        target_link_libraries(headset_input_bridge_tests PRIVATE simxr_control)
        simxr_add_test(headset_clock_tests tests/headset/RuntimeClockTests.cpp headset_clock)
        target_sources(headset_clock_tests PRIVATE src/headset/RuntimeInputTime.cpp src/headset/platform/linux/RuntimeClock.cpp)
        target_link_libraries(headset_clock_tests PRIVATE simxr_core)
    endif()
    simxr_add_test(headset_session_tests tests/headset/HeadsetSessionTests.cpp headset_session)
    target_sources(headset_session_tests PRIVATE src/headset/HeadsetSession.cpp src/headset/HeadsetSpace.cpp)
    target_link_libraries(headset_session_tests PRIVATE simxr_core)
    target_link_libraries(headset_session_tests PRIVATE Vulkan::Headers)
    simxr_add_test(headset_selection_tests tests/headset/RuntimeSelectionTests.cpp headset_selection)
    target_link_libraries(headset_selection_tests PRIVATE simxr_headset)
    add_executable(headset_parent_control_probe tests/headset/HostControlProbe.cpp)
    simxr_configure_target(headset_parent_control_probe)
    target_link_libraries(headset_parent_control_probe PRIVATE simxr_headset)
    if(Python3_Interpreter_FOUND)
        add_test(NAME headset_host_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/headset/test_host_cli.py" "$<TARGET_FILE:simxr_headset_host>")
        set_tests_properties(headset_host_cli PROPERTIES TIMEOUT 30)
        add_test(NAME headset_parent_control COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/headset/test_parent_control.py" "$<TARGET_FILE:headset_parent_control_probe>")
        set_tests_properties(headset_parent_control PROPERTIES TIMEOUT 30)
    endif()
endif()

if(TARGET vulkan_compositor_probe)
    target_sources(vulkan_compositor_probe PRIVATE src/headset/HeadsetProjection.cpp)
    target_compile_definitions(vulkan_compositor_probe PRIVATE SIMXR_HEADSET_PROJECTION)
    if(BUILD_TESTING)
        add_test(NAME headset_projection_native COMMAND vulkan_compositor_probe --headset-projection-only --verify-pixels)
        set_tests_properties(headset_projection_native PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
endif()
