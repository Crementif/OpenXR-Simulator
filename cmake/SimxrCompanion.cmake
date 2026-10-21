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
FetchContent_MakeAvailable(simxr_sdl simxr_imgui)
find_path(SIMXR_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/include" REQUIRED)
find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan vulkan-1 libvulkan.so.1 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)

add_library(simxr_imgui STATIC
    ${simxr_imgui_SOURCE_DIR}/imgui.cpp
    ${simxr_imgui_SOURCE_DIR}/imgui_draw.cpp
    ${simxr_imgui_SOURCE_DIR}/imgui_tables.cpp
    ${simxr_imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${simxr_imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${simxr_imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_compile_features(simxr_imgui PRIVATE cxx_std_20)
target_include_directories(simxr_imgui SYSTEM PUBLIC ${simxr_imgui_SOURCE_DIR} ${simxr_imgui_SOURCE_DIR}/backends "${SIMXR_VULKAN_INCLUDE_DIR}")
target_link_libraries(simxr_imgui PUBLIC SDL3::SDL3-static "${SIMXR_VULKAN_LIBRARY}")

add_library(simxr_app STATIC src/app/Application.cpp src/app/Window.cpp src/app/UiRenderer.cpp src/app/RuntimePanels.cpp src/app/PreviewTexture.cpp src/app/PreviewImages.cpp src/app/DeviceControlPanel.cpp src/app/WorkspacePanels.cpp)
simxr_configure_target(simxr_app)
target_include_directories(simxr_app PRIVATE "${PROJECT_SOURCE_DIR}/shaders/generated")
target_sources(simxr_app PRIVATE src/app/RecordingPanel.cpp src/app/TimelinePanel.cpp)
target_link_libraries(simxr_app PUBLIC simxr_imgui simxr_connection simxr_app_settings simxr_recording_controller)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(simxr_app PRIVATE simxr_vulkan_control)
    target_sources(simxr_app PRIVATE src/app/ProofConsumer.cpp probes/CompanionFrameReceiver.cpp)
    target_include_directories(simxr_app PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_definitions(simxr_app PRIVATE SIMXR_COMPANION_PROOF)
endif()
if(SIMXR_BUILD_AUTOMATION)
    target_sources(simxr_app PRIVATE src/app/RigPanel.cpp)
    target_link_libraries(simxr_app PUBLIC simxr_automation)
    target_compile_definitions(simxr_app PRIVATE SIMXR_COMPANION_AUTOMATION)
endif()
add_executable(simxr_companion src/app/Main.cpp)
simxr_configure_target(simxr_companion)
simxr_set_output_directory(simxr_companion)
target_link_libraries(simxr_companion PRIVATE simxr_app)
foreach(font_directory IN ITEMS "${SIMXR_OUTPUT_DIRECTORY}/fonts" "${CMAKE_CURRENT_BINARY_DIR}/fonts")
    file(MAKE_DIRECTORY "${font_directory}")
    configure_file("${PROJECT_SOURCE_DIR}/resources/fonts/LiberationSans-Regular.ttf"
        "${font_directory}/LiberationSans-Regular.ttf" COPYONLY)
endforeach()
install(FILES "${PROJECT_SOURCE_DIR}/resources/fonts/LiberationSans-Regular.ttf" DESTINATION bin/fonts)
install(FILES "${PROJECT_SOURCE_DIR}/resources/fonts/LIBERATION-LICENSE.txt"
    DESTINATION share/licenses/simxr_companion)
if(BUILD_TESTING)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
