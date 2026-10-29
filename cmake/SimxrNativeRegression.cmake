# One selection supplies both the build dependencies and CTest gate on each platform.
# These software contract checks require neither a physical headset nor a GPU driver.
set(simxr_native_regression_tests
    automation_lifecycle
    command_scheduler
    session
    snapshot_store
    recording
    recording_archive
    recording_controller
    rig_runner
    rig_controller
    headset_body
    headset_body_capabilities
    headset_trackers
    headset_projection
    headset_output
    headset_space
    headset_haptics
    headset_actions
    headset_tracking
    headset_session
    headset_selection)
if(NOT WIN32)
    list(APPEND simxr_native_regression_tests
        headset_images headset_image_bridge headset_input_bridge headset_clock)
endif()

add_custom_target(simxr_native_regression_tests)
foreach(test IN LISTS simxr_native_regression_tests)
    add_dependencies(simxr_native_regression_tests ${test}_tests)
    set_property(TEST ${test} APPEND PROPERTY LABELS native-regression)
endforeach()
