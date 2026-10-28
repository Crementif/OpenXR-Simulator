include(FetchContent)
enable_language(C)

# Upstream sources remain unmodified; overrides use standard FetchContent source directories.
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(simxr_sdl
    URL https://codeload.github.com/libsdl-org/SDL/tar.gz/829a65d769d935c4852f8159e964312c0957260a
    URL_HASH SHA256=48e15cf1a6928453d77afa955d1fdfbbca9169e2bc3d76d111acb9b8bb076d66
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(simxr_imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/af84778eeaab75bee0b54420f3157cf3ee00618f
    URL_HASH SHA256=60f709af3e53ef89874eba3162f4426f87beb5fd0682a681cc3189f0d7d246ad
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(simxr_imguizmo
    URL https://codeload.github.com/CedricGuillemet/ImGuizmo/tar.gz/105e4fde45e4e74aae8973d98750820ef7aca7e2
    URL_HASH SHA256=d625a9f3207f15b7a5b7706269747676f17b7e4df00f898ced9480e5063d493a
    SOURCE_SUBDIR simxr-source-only
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(simxr_nanosvg
    URL https://codeload.github.com/memononen/nanosvg/tar.gz/239e102ec2c691f2902e20ace2ed36ee4a35cfe6
    URL_HASH SHA256=2bc68bdb518d7800252042e5cad50a0ab321596f0cbf49ef2a752926329063d2
    SOURCE_SUBDIR simxr-source-only
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(simxr_sdl simxr_imgui simxr_imguizmo simxr_nanosvg)
find_path(SIMXR_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/include" REQUIRED)
include(cmake/SimxrMcp.cmake)
find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan vulkan-1 libvulkan.so.1 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)

add_library(simxr_imgui STATIC
    ${simxr_imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
    ${simxr_imgui_SOURCE_DIR}/imgui.cpp
    ${simxr_imgui_SOURCE_DIR}/imgui_draw.cpp
    ${simxr_imgui_SOURCE_DIR}/imgui_tables.cpp
    ${simxr_imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${simxr_imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${simxr_imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_compile_features(simxr_imgui PRIVATE cxx_std_20)
target_include_directories(simxr_imgui SYSTEM PUBLIC ${simxr_imguizmo_SOURCE_DIR}/src ${simxr_imgui_SOURCE_DIR} ${simxr_imgui_SOURCE_DIR}/backends "${SIMXR_VULKAN_INCLUDE_DIR}")
target_link_libraries(simxr_imgui PUBLIC SDL3::SDL3-static "${SIMXR_VULKAN_LIBRARY}")

add_library(simxr_app STATIC
    src/app/mcp/McpApplication.cpp
    src/app/mcp/McpHost.cpp
    src/app/Application.cpp
    src/app/RuntimePanels.cpp
    src/app/WorkspacePanels.cpp

    src/app/dialogs/ConnectionDialog.cpp
    src/app/dialogs/LaunchProfilesDialog.cpp
    src/app/dialogs/PreferencesDialog.cpp

    src/app/pages/AdvancedSteppingPage.cpp
    src/app/pages/DeviceControlPanel.cpp
    src/app/pages/DeviceInputControls.cpp
    src/app/pages/DeviceOwnership.cpp
    src/app/pages/DevicePoseControls.cpp
    src/app/pages/DeviceTrackingControls.cpp
    src/app/pages/FramePage.cpp
    src/app/pages/RangePage.cpp
    src/app/pages/PhysicalHeadsetPage.cpp

    src/app/platform/GamepadFeedback.cpp
    src/app/platform/PackageDocumentation.cpp
    src/app/platform/Window.cpp

    src/app/qa/RuntimeSmoke.cpp

    src/app/render/PreviewHistory.cpp
    src/app/render/PreviewImages.cpp
    src/app/render/PreviewTexture.cpp
    src/app/render/UiRenderResources.cpp
    src/app/render/UiRenderer.cpp
    src/app/render/UiRendererDevice.cpp
    src/app/render/UiRendererSwapchain.cpp
    src/app/render/UiScreenshot.cpp

    src/app/services/HeadsetHost.cpp
    src/app/services/HeadlessApplications.cpp
    src/app/services/RangeExportTask.cpp
    src/app/services/RuntimeSession.cpp
    src/app/services/RuntimeSessionFrames.cpp
    src/app/services/RuntimeSessionReplies.cpp
    src/app/services/ShellState.cpp
    src/app/services/UpdateChecker.cpp

    src/app/shell/ApplicationTabs.cpp
    src/app/shell/WorkspaceLayout.cpp
    src/app/shell/WorkspacePages.cpp
    src/app/shell/WorkspaceToolbar.cpp

    src/app/widgets/AppPicker.cpp
    src/app/widgets/Controls.cpp
    src/app/widgets/DeviceRail.cpp
    src/app/widgets/DocumentTree.cpp
    src/app/widgets/HapticEvents.cpp
    src/app/widgets/Icons.cpp
    src/app/widgets/InputPad.cpp
    src/app/widgets/LiveField.cpp
    src/app/widgets/Notices.cpp
    src/app/widgets/PastInput.cpp
    src/app/widgets/PilotController.cpp
    src/app/widgets/PilotHud.cpp
    src/app/widgets/PreviewPanel.cpp
    src/app/widgets/RecordingPanel.cpp
    src/app/widgets/RigView.cpp
    src/app/widgets/RotationOrb.cpp
    src/app/widgets/SpatialEditor.cpp
    src/app/widgets/SpatialPreview.cpp
    src/app/widgets/SpatialWidgets.cpp
    src/app/widgets/Theme.cpp
    src/app/widgets/TimelineCanvas.cpp
    src/app/widgets/TimelinePanel.cpp)
simxr_configure_target(simxr_app app/UiPch.h)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    FetchContent_Declare(simxr_curl_headers
        URL https://curl.se/download/curl-8.10.1.tar.xz
        URL_HASH SHA256=73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_GetProperties(simxr_curl_headers)
    if(NOT simxr_curl_headers_POPULATED)
        FetchContent_Populate(simxr_curl_headers)
    endif()
    target_sources(simxr_app PRIVATE src/app/platform/OpenLocalPath.cpp src/app/platform/linux/UpdateTransport.cpp)
    target_include_directories(simxr_app SYSTEM PRIVATE "${simxr_curl_headers_SOURCE_DIR}/include")
    target_link_libraries(simxr_app PRIVATE ${CMAKE_DL_LIBS})
    install(FILES "${simxr_curl_headers_SOURCE_DIR}/COPYING" DESTINATION share/licenses/simxr_companion RENAME curl-headers.txt)
elseif(WIN32)
    target_sources(simxr_app PRIVATE src/app/platform/windows/OpenLocalPath.cpp
        src/app/platform/windows/UpdateTransport.cpp src/app/platform/windows/UpdateWorker.cpp)
    target_link_libraries(simxr_app PRIVATE winhttp)
else()
    target_sources(simxr_app PRIVATE src/app/platform/OpenLocalPath.cpp src/app/platform/UpdateTransportUnavailable.cpp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SDL_X11)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(simxr_app PRIVATE SIMXR_X11_WINDOW=1)
        target_include_directories(simxr_app PRIVATE ${X11_INCLUDE_DIR})
    endif()
endif()
target_include_directories(simxr_app PRIVATE "${PROJECT_SOURCE_DIR}/shaders/generated")
target_link_libraries(simxr_app PUBLIC simxr_imgui simxr_connection simxr_app_settings simxr_recording_controller simxr_mcp)
# The icon SVGs are compiled in as {file stem, source} pairs; Icons.cpp parses them once.
file(GLOB simxr_icon_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/resources/icons/*.svg")
list(SORT simxr_icon_files)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${simxr_icon_files})
set(simxr_icon_sources "")
foreach(icon_file IN LISTS simxr_icon_files)
    get_filename_component(icon_name "${icon_file}" NAME_WE)
    file(READ "${icon_file}" icon_svg)
    string(APPEND simxr_icon_sources "{\"${icon_name}\", R\"svg(${icon_svg})svg\"},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/IconSources.inc" CONTENT "${simxr_icon_sources}" @ONLY)
target_include_directories(simxr_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(simxr_app SYSTEM PRIVATE "${simxr_nanosvg_SOURCE_DIR}/src")
target_link_libraries(simxr_app PRIVATE simxr_vulkan_control)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(simxr_app PRIVATE src/app/render/ProofConsumer.cpp probes/CompanionFrameReceiver.cpp)
    target_link_libraries(simxr_app PRIVATE simxr_probe_sharing)
    target_include_directories(simxr_app PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_definitions(simxr_app PRIVATE SIMXR_COMPANION_PROOF)
endif()
if(SIMXR_BUILD_AUTOMATION)
    target_sources(simxr_app PRIVATE src/app/pages/Library.cpp)
    target_link_libraries(simxr_app PUBLIC simxr_rig_controller)
    target_compile_definitions(simxr_app PUBLIC SIMXR_COMPANION_AUTOMATION)
    target_compile_definitions(simxr_mcp PRIVATE SIMXR_COMPANION_AUTOMATION)
endif()
add_executable(simxr_companion WIN32 src/app/Main.cpp src/app/platform/StartupFailure.cpp)
add_dependencies(simxr_companion simxr_mcp_connector)
if(WIN32)
    enable_language(RC)
    configure_file("${PROJECT_SOURCE_DIR}/cmake/WindowsAppIcon.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/WindowsAppIcon.rc" @ONLY)
    target_sources(simxr_companion PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/WindowsAppIcon.rc")
endif()
simxr_configure_target(simxr_companion app/UiPch.h)
simxr_set_output_directory(simxr_companion)
target_link_libraries(simxr_companion PRIVATE simxr_app)
if(TARGET simxr_install_use)
    target_link_libraries(simxr_companion PRIVATE simxr_install_use)
endif()
configure_file("${PROJECT_SOURCE_DIR}/resources/branding/openxr-simulator.bmp"
    "${SIMXR_OUTPUT_DIRECTORY}/openxr-simulator.bmp" COPYONLY)
install(FILES "${PROJECT_SOURCE_DIR}/resources/branding/openxr-simulator.bmp" DESTINATION bin)
install(FILES "${PROJECT_SOURCE_DIR}/resources/branding/openxr-simulator.svg"
    "${PROJECT_SOURCE_DIR}/resources/branding/openxr-simulator.png"
    "${PROJECT_SOURCE_DIR}/resources/branding/openxr-simulator.ico" DESTINATION share/simxr/branding)
if(MINGW)
    target_link_options(simxr_companion PRIVATE -municode)
endif()
foreach(font_directory IN ITEMS "${SIMXR_OUTPUT_DIRECTORY}/fonts" "${CMAKE_CURRENT_BINARY_DIR}/fonts")
    file(MAKE_DIRECTORY "${font_directory}")
    configure_file("${PROJECT_SOURCE_DIR}/resources/fonts/AdwaitaSans-Regular.ttf"
        "${font_directory}/AdwaitaSans-Regular.ttf" COPYONLY)
endforeach()
install(FILES "${PROJECT_SOURCE_DIR}/resources/fonts/AdwaitaSans-Regular.ttf" DESTINATION bin/fonts)
install(FILES "${PROJECT_SOURCE_DIR}/resources/fonts/ADWAITA-SANS-LICENSE.txt"
    DESTINATION share/licenses/simxr_companion)
if(BUILD_TESTING)
    if(NOT Python3_Interpreter_FOUND)
        find_package(Python3 QUIET COMPONENTS Interpreter)
    endif()
    if(WIN32)
        add_executable(mcp_restart_fixture tests/app/WindowsRestartFixture.cpp)
        simxr_configure_target(mcp_restart_fixture)
        target_link_libraries(mcp_restart_fixture PRIVATE simxr_control user32)
        if(MINGW)
            target_link_options(mcp_restart_fixture PRIVATE -municode)
        endif()
        add_executable(mcp_discovery_fixture tests/app/WindowsMcpDiscoveryFixture.cpp)
        simxr_configure_target(mcp_discovery_fixture)
        target_link_libraries(mcp_discovery_fixture PRIVATE simxr_app)
        add_executable(mcp_windows_job_tests tests/app/WindowsMcpJobTests.cpp)
        simxr_configure_target(mcp_windows_job_tests)
        target_link_libraries(mcp_windows_job_tests PRIVATE simxr_mcp)
        if(MINGW)
            target_link_options(mcp_windows_job_tests PRIVATE -municode)
        endif()
        if(Python3_Interpreter_FOUND)
            add_test(NAME windows_mcp_discovery COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_discovery_windows.py"
                "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:mcp_discovery_fixture>"
                "$<TARGET_FILE:mcp_windows_job_tests>")
            set_tests_properties(windows_mcp_discovery PROPERTIES TIMEOUT 60)
        endif()
    endif()
    add_executable(mcp_application_tests tests/app/McpApplicationTests.cpp)
    simxr_configure_target(mcp_application_tests app/mcp/McpPch.h)
    target_link_libraries(mcp_application_tests PRIVATE simxr_app)
    add_test(NAME mcp_application COMMAND mcp_application_tests)
    add_executable(package_documentation_tests tests/app/PackageDocumentationTests.cpp)
    simxr_configure_target(package_documentation_tests)
    target_link_libraries(package_documentation_tests PRIVATE simxr_app)
    add_test(NAME package_documentation COMMAND package_documentation_tests)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Python3_Interpreter_FOUND)
        add_test(NAME package_documentation_open COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_package_documentation_linux.py" "$<TARGET_FILE:package_documentation_tests>")
        set_tests_properties(package_documentation_open PROPERTIES TIMEOUT 30)
    endif()
    if(WIN32)
        add_executable(windows_manifest_alias_fixture tests/app/WindowsManifestAliasFixture.cpp)
        simxr_configure_target(windows_manifest_alias_fixture)
        target_link_libraries(windows_manifest_alias_fixture PRIVATE simxr_app_settings)
        if(MINGW)
            target_link_options(windows_manifest_alias_fixture PRIVATE -municode)
        endif()
    endif()
    set_tests_properties(mcp_application PROPERTIES TIMEOUT 15)
    add_executable(startup_failure_tests tests/app/StartupFailureTests.cpp src/app/platform/StartupFailure.cpp)
    simxr_configure_target(startup_failure_tests)
    target_link_libraries(startup_failure_tests PRIVATE SDL3::SDL3-static)
    add_test(NAME startup_failure_format COMMAND startup_failure_tests)
    set_tests_properties(startup_failure_format PROPERTIES TIMEOUT 15)
    add_executable(mcp_host_tests tests/app/McpHostTests.cpp)
    simxr_configure_target(mcp_host_tests app/mcp/McpPch.h)
    target_link_libraries(mcp_host_tests PRIVATE simxr_app)
    add_test(NAME mcp_host COMMAND mcp_host_tests)
    set_tests_properties(mcp_host PROPERTIES TIMEOUT 15)
    add_executable(mcp_image_encode_tests tests/app/McpImageEncodeTests.cpp)
    simxr_configure_target(mcp_image_encode_tests)
    target_link_libraries(mcp_image_encode_tests PRIVATE simxr_mcp)
    add_test(NAME mcp_image_encode COMMAND mcp_image_encode_tests)
    set_tests_properties(mcp_image_encode PROPERTIES TIMEOUT 15)
    add_executable(mcp_simulation_command_tests tests/app/McpSimulationCommandTests.cpp)
    simxr_configure_target(mcp_simulation_command_tests)
    target_link_libraries(mcp_simulation_command_tests PRIVATE simxr_mcp simxr_control)
    add_test(NAME mcp_simulation_commands COMMAND mcp_simulation_command_tests)
    set_tests_properties(mcp_simulation_commands PROPERTIES TIMEOUT 15)
    add_executable(stereo_surface_tests tests/app/StereoSurfaceTests.cpp)
    simxr_configure_target(stereo_surface_tests)
    target_link_libraries(stereo_surface_tests PRIVATE simxr_mcp)
    add_test(NAME stereo_surface COMMAND stereo_surface_tests)
    set_tests_properties(stereo_surface PROPERTIES TIMEOUT 60)
    add_executable(stereo_consistency_tests tests/app/StereoConsistencyTests.cpp)
    simxr_configure_target(stereo_consistency_tests)
    target_link_libraries(stereo_consistency_tests PRIVATE simxr_mcp)
    add_test(NAME stereo_consistency COMMAND stereo_consistency_tests)
    set_tests_properties(stereo_consistency PROPERTIES TIMEOUT 60)
    add_executable(mcp_history_tests tests/app/McpHistoryTests.cpp)
    simxr_configure_target(mcp_history_tests)
    target_link_libraries(mcp_history_tests PRIVATE simxr_mcp simxr_control)
    add_test(NAME mcp_history COMMAND mcp_history_tests)
    set_tests_properties(mcp_history PROPERTIES TIMEOUT 15)
    add_executable(update_checker_tests tests/app/UpdateCheckerTests.cpp)
    simxr_configure_target(update_checker_tests)
    target_link_libraries(update_checker_tests PRIVATE simxr_app)
    add_test(NAME update_checker COMMAND update_checker_tests)
    set_tests_properties(update_checker PROPERTIES TIMEOUT 15)
    if(WIN32 AND Python3_Interpreter_FOUND)
        add_test(NAME windows_update_timeout COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_update_timeout_windows.py" "$<TARGET_FILE:update_checker_tests>")
        set_tests_properties(windows_update_timeout PROPERTIES TIMEOUT 25)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME mcp_connector_cancellation COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_connector_queue.py" "$<TARGET_FILE:simxr_mcp_connector>")
        set_tests_properties(mcp_connector_cancellation PROPERTIES TIMEOUT 30)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
            add_test(NAME mcp_connector_configuration COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_configuration.py" "$<TARGET_FILE:simxr_mcp_connector>")
            set_tests_properties(mcp_connector_configuration PROPERTIES TIMEOUT 30)
            add_test(NAME mcp_connector_protocol COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_connector_protocol.py"
                "$<TARGET_FILE:simxr_companion>" "$<TARGET_FILE:simxr_mcp_connector>")
            set_tests_properties(mcp_connector_protocol PROPERTIES TIMEOUT 60)
        endif()
        add_test(NAME mcp_image_encode_pixels COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_image_encoding.py" "$<TARGET_FILE:mcp_image_encode_tests>")
        set_tests_properties(mcp_image_encode_pixels PROPERTIES TIMEOUT 15)
    endif()
    if(WIN32 AND Python3_Interpreter_FOUND)
        if(TARGET xr_probe_vk AND TARGET openxr_simulator)
            add_test(NAME windows_mcp_image COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_image_windows.py" "$<TARGET_FILE:simxr_companion>"
                "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_vk>"
                "$<TARGET_FILE_DIR:openxr_simulator>/openxr_simulator.json")
            set_tests_properties(windows_mcp_image PROPERTIES TIMEOUT 90
                ENVIRONMENT "SIMXR_EXPECT_MCP_LUAU=$<BOOL:${SIMXR_BUILD_AUTOMATION}>")
            add_test(NAME windows_mcp_screenshot COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_screenshot_windows.py" "$<TARGET_FILE:simxr_companion>"
                "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_vk>"
                "$<TARGET_FILE_DIR:openxr_simulator>/openxr_simulator.json")
            set_tests_properties(windows_mcp_screenshot PROPERTIES TIMEOUT 90)
            add_test(NAME windows_retained_inspection COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_windows_retained_inspection.py" "$<TARGET_FILE:xr_probe_vk>"
                "$<TARGET_FILE_DIR:openxr_simulator>/openxr_simulator.json")
            set_tests_properties(windows_retained_inspection PROPERTIES TIMEOUT 100 SKIP_RETURN_CODE 77)
        endif()
        add_test(NAME windows_mcp_windowless COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_windowless_windows.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:mcp_restart_fixture>")
        set_tests_properties(windows_mcp_windowless PROPERTIES TIMEOUT 60)
        add_test(NAME windows_mcp_preference COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_preference_windows.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:mcp_host_tests>")
        set_tests_properties(windows_mcp_preference PROPERTIES TIMEOUT 60)
    endif()
    add_executable(headset_host_service_tests tests/app/HeadsetHostTests.cpp)
    simxr_configure_target(headset_host_service_tests)
    target_link_libraries(headset_host_service_tests PRIVATE simxr_app)
    add_test(NAME headset_host_service COMMAND headset_host_service_tests)
    set_tests_properties(headset_host_service PROPERTIES TIMEOUT 25)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(window_tests tests/app/WindowTests.cpp)
        simxr_configure_target(window_tests app/UiPch.h)
        target_link_libraries(window_tests PRIVATE simxr_app)
        target_link_options(window_tests PRIVATE "LINKER:--wrap=SDL_SetWindowRelativeMouseMode")
        add_test(NAME window COMMAND window_tests)
        set_tests_properties(window PROPERTIES TIMEOUT 30)
        add_executable(preview_texture_tests tests/app/PreviewTextureTests.cpp)
        simxr_configure_target(preview_texture_tests)
        target_link_libraries(preview_texture_tests PRIVATE simxr_app)
        add_test(NAME preview_texture COMMAND preview_texture_tests)
        set_tests_properties(preview_texture PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    endif()
    if(SIMXR_BUILD_AUTOMATION)
        add_executable(companion_interaction_tests tests/app/CompanionInteractionTests.cpp)
        simxr_configure_target(companion_interaction_tests app/UiPch.h)
        target_link_libraries(companion_interaction_tests PRIVATE simxr_app)
        add_dependencies(companion_interaction_tests headset_host_service_tests)
        add_test(NAME companion_interaction COMMAND companion_interaction_tests)
        set_tests_properties(companion_interaction PROPERTIES TIMEOUT 60)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME companion_multi_tabs COMMAND companion_interaction_tests)
            set_tests_properties(companion_multi_tabs PROPERTIES ENVIRONMENT "SIMXR_TABS_QA=1" TIMEOUT 60)
            add_test(NAME companion_multi_launch COMMAND companion_interaction_tests)
            set_tests_properties(companion_multi_launch PROPERTIES ENVIRONMENT "SIMXR_TAB_LAUNCH_QA=1" TIMEOUT 30)
        endif()
    endif()
    add_test(NAME companion_smoke COMMAND simxr_companion --smoke-frames 8 --hidden)
    set_tests_properties(companion_smoke PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET xr_probe_linux AND SIMXR_BUILD_RUNTIME AND Python3_Interpreter_FOUND)
        add_test(NAME companion_profile_command COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_profile_command.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:companion_settings_tests>")
        set_tests_properties(companion_profile_command PROPERTIES TIMEOUT 30)
        add_test(NAME companion_mcp_http COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_http.py" "$<TARGET_FILE:simxr_companion>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_http PROPERTIES TIMEOUT 60)
        add_test(NAME companion_mcp_connector COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_connector.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_connector PROPERTIES TIMEOUT 90
            ENVIRONMENT "SIMXR_EXPECT_MCP_LUAU=$<BOOL:${SIMXR_BUILD_AUTOMATION}>")
        add_test(NAME companion_mcp_discovery COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_discovery.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_discovery PROPERTIES TIMEOUT 90)
        add_test(NAME companion_mcp_windowless COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_windowless.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_windowless PROPERTIES TIMEOUT 90
            ENVIRONMENT "SIMXR_EXPECT_MCP_LUAU=$<BOOL:${SIMXR_BUILD_AUTOMATION}>")
        add_test(NAME companion_mcp_image_history COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_image_history.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_image_history PROPERTIES TIMEOUT 90)
        add_test(NAME companion_mcp_screenshot COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_screenshot.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_screenshot PROPERTIES TIMEOUT 90)
        add_test(NAME companion_mcp_stereo_consistency COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_stereo_consistency.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_stereo_consistency PROPERTIES TIMEOUT 120)
        add_test(NAME companion_mcp_logs COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_logs.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_logs PROPERTIES TIMEOUT 90)
        add_test(NAME companion_mcp_incident COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_incident.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_incident PROPERTIES TIMEOUT 120)
        add_test(NAME companion_mcp_flicker_monitor COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_flicker_monitor.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_flicker_monitor PROPERTIES TIMEOUT 120)
        add_test(NAME companion_mcp_compatibility COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_compatibility.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_compatibility PROPERTIES TIMEOUT 90)
        add_test(NAME companion_mcp_sequence COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_sequence.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_sequence PROPERTIES TIMEOUT 90)
        add_test(NAME companion_mcp_sequence_capture COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_sequence_capture.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:xr_probe_linux>")
        set_tests_properties(companion_mcp_sequence_capture PROPERTIES TIMEOUT 90)
        add_test(NAME companion_mcp_preference COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app/test_mcp_preference.py" "$<TARGET_FILE:simxr_companion>"
            "$<TARGET_FILE:simxr_mcp_connector>" "$<TARGET_FILE:mcp_host_tests>")
        set_tests_properties(companion_mcp_preference PROPERTIES TIMEOUT 90)
        add_test(NAME companion_arguments COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --cli-only)
        set_tests_properties(companion_arguments PROPERTIES TIMEOUT 30)
        add_test(NAME companion_runtime COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>")
        add_test(NAME companion_runtime_pixels COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --verify-pixels)
        if(TARGET companion_interaction_tests)
            add_test(NAME companion_multi_gpu COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:companion_interaction_tests>" --multi-tab)
            add_test(NAME companion_four_gpu COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:companion_interaction_tests>" --multi-tab --four-tab)
            add_test(NAME companion_tab_input COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:companion_interaction_tests>" --multi-tab --tab-input)
            add_test(NAME companion_portable_tabs COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:companion_interaction_tests>" --multi-tab --portable-tabs --four-tab)
            add_test(NAME companion_snapshot_tabs COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:companion_interaction_tests>" --multi-tab --portable-tabs --four-tab --budget-tabs)
            add_test(NAME companion_replay_close COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:companion_interaction_tests>" --multi-tab --replay-close)
            set_tests_properties(companion_multi_gpu companion_four_gpu companion_tab_input companion_portable_tabs companion_snapshot_tabs companion_replay_close PROPERTIES
                ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif()
        add_test(NAME companion_export_failure COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --fail-export)
        add_test(NAME companion_runtime_x11 COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --visible --verify-pixels)
        set_tests_properties(companion_runtime_x11 PROPERTIES
            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1;SDL_VIDEODRIVER=x11"
            TIMEOUT 60 SKIP_RETURN_CODE 77)
        add_test(NAME companion_gamma_unorm COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --verify-pixels --midtone)
        add_test(NAME companion_gamma_srgb COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --verify-pixels --midtone --srgb)
        set_tests_properties(companion_runtime companion_runtime_pixels companion_export_failure companion_gamma_unorm companion_gamma_srgb PROPERTIES
            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
            TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    foreach(producer IN ITEMS vulkan opengl_vulkan)
        if(TARGET ${producer}_sharing_probe)
            add_test(NAME companion_${producer}_transport COMMAND ${producer}_sharing_probe
                --companion $<TARGET_FILE:simxr_companion>)
            add_test(NAME companion_${producer}_transport_pixels COMMAND ${producer}_sharing_probe
                --companion $<TARGET_FILE:simxr_companion> --verify-pixels)
            set_tests_properties(companion_${producer}_transport companion_${producer}_transport_pixels
                PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()
install(TARGETS simxr_companion RUNTIME DESTINATION bin)

install(FILES ${simxr_sdl_SOURCE_DIR}/LICENSE.txt DESTINATION share/licenses/simxr_companion RENAME SDL3.txt)
install(FILES ${simxr_imgui_SOURCE_DIR}/LICENSE.txt DESTINATION share/licenses/simxr_companion RENAME ImGui.txt)

install(FILES ${simxr_imguizmo_SOURCE_DIR}/LICENSE DESTINATION share/licenses/simxr_companion RENAME ImGuizmo.txt)
install(FILES ${simxr_nanosvg_SOURCE_DIR}/LICENSE.txt DESTINATION share/licenses/simxr_companion RENAME NanoSVG.txt)
