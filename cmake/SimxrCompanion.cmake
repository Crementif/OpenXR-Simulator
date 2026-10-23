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

add_library(simxr_app STATIC src/app/Application.cpp src/app/platform/Window.cpp src/app/render/UiRenderer.cpp src/app/render/UiRenderResources.cpp src/app/render/UiRendererDevice.cpp src/app/render/UiRendererSwapchain.cpp src/app/render/UiScreenshot.cpp src/app/RuntimePanels.cpp src/app/render/PreviewTexture.cpp src/app/render/PreviewImages.cpp src/app/render/PreviewHistory.cpp src/app/WorkspacePanels.cpp)
target_sources(simxr_app PRIVATE src/app/pages/DeviceControlPanel.cpp src/app/pages/DeviceOwnership.cpp
    src/app/pages/DevicePoseControls.cpp src/app/pages/DeviceInputControls.cpp src/app/pages/DeviceTrackingControls.cpp)
simxr_configure_target(simxr_app)
target_include_directories(simxr_app PRIVATE "${PROJECT_SOURCE_DIR}/shaders/generated")
target_sources(simxr_app PRIVATE src/app/RecordingPanel.cpp src/app/TimelinePanel.cpp src/app/TimelineCanvas.cpp src/app/ShellState.cpp
    src/app/Theme.cpp src/app/widgets/Icons.cpp src/app/widgets/Controls.cpp src/app/DeviceRail.cpp
    src/app/services/RuntimeSession.cpp src/app/services/RuntimeSessionReplies.cpp src/app/services/RuntimeSessionFrames.cpp
    src/app/dialogs/ConnectionDialog.cpp src/app/Notices.cpp src/app/DocumentTree.cpp src/app/FramePage.cpp src/app/RangePage.cpp
    src/app/AdvancedSteppingPage.cpp src/app/PreviewPanel.cpp src/app/PreferencesDialog.cpp src/app/LaunchProfilesDialog.cpp
    src/app/widgets/PilotController.cpp src/app/widgets/PilotHud.cpp src/app/widgets/SpatialEditor.cpp src/app/widgets/SpatialPreview.cpp src/app/widgets/LiveField.cpp
    src/app/widgets/SpatialWidgets.cpp src/app/widgets/RotationOrb.cpp
    src/app/widgets/InputPad.cpp src/app/widgets/RigView.cpp src/app/widgets/HapticEvents.cpp src/app/platform/GamepadFeedback.cpp)
target_sources(simxr_app PRIVATE src/app/PastInput.cpp src/app/AppPicker.cpp)
target_link_libraries(simxr_app PUBLIC simxr_imgui simxr_connection simxr_app_settings simxr_recording_controller)
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
    target_sources(simxr_app PRIVATE src/app/ProofConsumer.cpp probes/CompanionFrameReceiver.cpp)
    target_include_directories(simxr_app PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_definitions(simxr_app PRIVATE SIMXR_COMPANION_PROOF)
endif()
if(SIMXR_BUILD_AUTOMATION)
    target_sources(simxr_app PRIVATE src/app/pages/Library.cpp)
    target_link_libraries(simxr_app PUBLIC simxr_rig_controller)
    target_compile_definitions(simxr_app PRIVATE SIMXR_COMPANION_AUTOMATION)
endif()
add_executable(simxr_companion src/app/Main.cpp)
simxr_configure_target(simxr_companion)
simxr_set_output_directory(simxr_companion)
target_link_libraries(simxr_companion PRIVATE simxr_app)
foreach(font_directory IN ITEMS "${SIMXR_OUTPUT_DIRECTORY}/fonts" "${CMAKE_CURRENT_BINARY_DIR}/fonts")
    file(MAKE_DIRECTORY "${font_directory}")
    configure_file("${PROJECT_SOURCE_DIR}/resources/fonts/AdwaitaSans-Regular.ttf"
        "${font_directory}/AdwaitaSans-Regular.ttf" COPYONLY)
endforeach()
install(FILES "${PROJECT_SOURCE_DIR}/resources/fonts/AdwaitaSans-Regular.ttf" DESTINATION bin/fonts)
install(FILES "${PROJECT_SOURCE_DIR}/resources/fonts/ADWAITA-SANS-LICENSE.txt"
    DESTINATION share/licenses/simxr_companion)
if(BUILD_TESTING)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(window_tests tests/app/WindowTests.cpp)
        simxr_configure_target(window_tests)
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
        simxr_configure_target(companion_interaction_tests)
        target_link_libraries(companion_interaction_tests PRIVATE simxr_app)
        add_test(NAME companion_interaction COMMAND companion_interaction_tests)
        set_tests_properties(companion_interaction PROPERTIES TIMEOUT 60)
    endif()
    add_test(NAME companion_smoke COMMAND simxr_companion --smoke-frames 8 --hidden)
    set_tests_properties(companion_smoke PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET xr_probe_linux AND SIMXR_BUILD_RUNTIME AND Python3_Interpreter_FOUND)
        add_test(NAME companion_runtime COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>")
        add_test(NAME companion_runtime_pixels COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --verify-pixels)
        add_test(NAME companion_runtime_x11 COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --visible --verify-pixels)
        set_tests_properties(companion_runtime_x11 PROPERTIES
            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1;SDL_VIDEODRIVER=x11"
            TIMEOUT 60 SKIP_RETURN_CODE 77)
        add_test(NAME companion_gamma_unorm COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --verify-pixels --midtone)
        add_test(NAME companion_gamma_srgb COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/control/test_native_companion.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_companion>" --verify-pixels --midtone --srgb)
        set_tests_properties(companion_runtime companion_runtime_pixels companion_gamma_unorm companion_gamma_srgb PROPERTIES
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
