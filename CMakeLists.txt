cmake_minimum_required(VERSION 3.24)
project(OpenXR-Simulator VERSION 2.0.0 LANGUAGES CXX)

configure_file(cmake/Version.h.in "${PROJECT_BINARY_DIR}/generated/simxr/Version.h" @ONLY)

include(CTest)
include(cmake/SimxrRuntimeLibraries.cmake)
include(cmake/SimxrTargets.cmake)
find_package(Threads REQUIRED)

option(SIMXR_ENABLE_PCH "Use private precompiled headers" OFF)
option(SIMXR_BUILD_RUNTIME "Build the OpenXR runtime" ON)
option(SIMXR_BUILD_PROBES "Build native graphics compatibility probes" ON)
option(SIMXR_BUILD_AUTOMATION "Build Luau rig runtime and headless runner" ON)
option(SIMXR_BUILD_COMPANION "Build the separate SDL3/Vulkan companion" OFF)
set(SIMXR_SANITIZER "" CACHE STRING "Sanitizer: address, thread, or empty")
set_property(CACHE SIMXR_SANITIZER PROPERTY STRINGS "" address thread)
set(SIMXR_MCP_TEST_PYTHON "" CACHE FILEPATH "Optional Python interpreter with the MCP SDK for stdio integration tests")
set(SIMXR_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin" CACHE PATH "Simulator binary/manifest output directory")

add_library(simxr_openxr_headers INTERFACE)
target_include_directories(simxr_openxr_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(simxr_core STATIC
    src/Json.cpp
    src/math/Math.cpp
    src/platform/Clock.cpp
    src/graphics/SnapshotBudget.cpp
    src/graphics/FramePool.cpp
    src/simulation/ActionRegistry.cpp
    src/simulation/InputState.cpp
    src/simulation/LegacyControllerMotion.cpp
    src/simulation/CommandScheduler.cpp
    src/simulation/CommandState.cpp
    src/simulation/CommandRecorder.cpp
    src/simulation/GameBridge.cpp
    src/runtime/extensions/EyeGazeInteraction.cpp
    src/runtime/extensions/TrackerInteraction.cpp
    src/runtime/EventQueue.cpp
    src/runtime/Instance.cpp
    src/runtime/EnumNames.cpp
    src/runtime/Runtime.cpp
    src/runtime/Session.cpp
    src/runtime/SessionInteraction.cpp
    src/runtime/SessionCaptureCoordinator.cpp
    src/runtime/SessionPublication.cpp
    src/runtime/SpaceQueries.cpp
    src/runtime/ExtensionRegistry.cpp
    src/runtime/DispatchTable.cpp
    src/runtime/SimulationExtensions.cpp
    src/runtime/validation/LayerValidation.cpp
    src/runtime/validation/Path.cpp
    src/runtime/validation/StructureChain.cpp
    src/simulation/HapticState.cpp
    src/runtime/extensions/BodyTracking.cpp
    src/simulation/BodyModel.cpp
    src/simulation/HeadsetProfiles.cpp
    src/simulation/PoseSequence.cpp
    src/simulation/PoseSequenceExecution.cpp
    src/simulation/PoseHistory.cpp
    src/simulation/TrackingSystem.cpp
    src/simulation/SpaceSystem.cpp
    src/simulation/ReferenceSpaceState.cpp
    src/simulation/ViewConfiguration.cpp
    src/presentation/FrameHistory.cpp
    src/presentation/FrameImageHistory.cpp
    src/presentation/FramePreviewHistory.cpp
    src/media/Mp4Video.cpp
    src/presentation/FrameDiagnosticsJournal.cpp
    src/presentation/SnapshotStore.cpp
    src/presentation/DebugController.cpp
    src/presentation/PresentationController.cpp
    src/presentation/VirtualDisplay.cpp
    src/presentation/RotationalDisplay.cpp
    src/presentation/DisplayTimeline.cpp
    src/presentation/FrameScheduler.cpp)
simxr_configure_target(simxr_core)
target_include_directories(simxr_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(simxr_core PUBLIC simxr_openxr_headers Threads::Threads)
target_compile_features(simxr_core PUBLIC cxx_std_20)
if(WIN32)
    target_sources(simxr_core PRIVATE src/platform/windows/WindowsProcessIdentity.cpp
        src/platform/windows/ModuleReference.cpp src/platform/windows/WindowsLibrary.cpp)
    target_link_libraries(simxr_core PUBLIC advapi32)
else()
    target_sources(simxr_core PRIVATE src/platform/ProcessIdentity.cpp)
endif()

add_library(simxr_control STATIC
    src/control/VideoExport.cpp
    src/control/legacy/LegacyCaptureImage.cpp
    src/control/legacy/LegacyInput.cpp
    src/control/legacy/WindowsLegacyChannel.cpp
    src/control/legacy/WindowsLegacyCapture.cpp
    src/control/legacy/WindowsLegacyFiles.cpp
    src/control/legacy/WindowsLegacyFormat.cpp
    src/control/HapticObservation.cpp
    src/control/WireProtocol.cpp
    src/control/InspectionDocument.cpp
    src/control/FrameInspection.cpp
    src/control/ImageInspection.cpp
    src/control/ControlService.cpp
    src/control/ControlSessions.cpp
    src/control/ControlSchema.cpp
    src/control/ConnectionImageWindows.cpp
    src/control/ConnectionImages.cpp
    src/control/ConnectionLeases.cpp
    src/control/ConnectionRequests.cpp
    src/control/DebugProtocol.cpp
    src/control/RecordingControl.cpp
    src/control/SequenceControl.cpp
    src/control/SimulationConnection.cpp
    src/control/SimulationControl.cpp
    src/control/SimulationCommandCodec.cpp)
simxr_configure_target(simxr_control)
target_link_libraries(simxr_control PUBLIC simxr_core)
target_compile_features(simxr_control PUBLIC cxx_std_20)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(simxr_control PRIVATE src/platform/linux/LocalControlServer.cpp src/platform/linux/LocalControlClient.cpp
        src/platform/linux/ControlFd.cpp src/platform/linux/LegacyFileChannel.cpp
        src/platform/linux/LinuxRuntimeStatus.cpp)
elseif(WIN32)
    target_sources(simxr_control PRIVATE src/platform/windows/LocalControlServer.cpp src/platform/windows/LocalControlClient.cpp
        src/platform/windows/WindowsControl.cpp src/platform/windows/WindowsRuntimeInput.cpp)
    target_link_libraries(simxr_control PRIVATE advapi32 user32)
endif()
add_library(simxr_recording STATIC src/automation/Recording.cpp src/automation/RecordedRig.cpp)
simxr_configure_target(simxr_recording)
target_link_libraries(simxr_recording PUBLIC simxr_control)

add_library(simxr_app_platform STATIC)
if(WIN32)
    target_sources(simxr_app_platform PRIVATE src/app/platform/windows/FileStorage.cpp
        src/app/platform/windows/LaunchSupport.cpp src/app/platform/windows/RuntimeEndpoints.cpp)
else()
    target_sources(simxr_app_platform PRIVATE src/app/platform/FileStorage.cpp src/app/platform/RuntimeEndpoints.cpp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(simxr_app_platform PRIVATE src/app/platform/linux/RunningApps.cpp)
elseif(WIN32)
    target_sources(simxr_app_platform PRIVATE src/app/platform/windows/RunningApps.cpp)
else()
    target_sources(simxr_app_platform PRIVATE src/app/platform/RunningApps.cpp)
endif()
simxr_configure_target(simxr_app_platform)

add_library(simxr_app_settings STATIC
    src/app/services/CompanionSettings.cpp
    src/app/services/RunningApps.cpp
    src/app/services/PilotBindings.cpp
    src/app/services/LaunchTypes.cpp
    src/app/services/ProcessLauncher.cpp
    src/app/services/ProcessSupervisor.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(simxr_app_settings PRIVATE src/app/platform/linux/ChildProcess.cpp)
elseif(WIN32)
    target_sources(simxr_app_settings PRIVATE src/app/platform/windows/ChildProcess.cpp
        src/app/platform/windows/PrivateProtonRuntime.cpp)
else()
    target_sources(simxr_app_settings PRIVATE src/app/platform/ChildProcess.cpp)
endif()
simxr_configure_target(simxr_app_settings)
target_include_directories(simxr_app_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(simxr_app_settings PUBLIC Threads::Threads PRIVATE simxr_app_platform)
if(WIN32)
    target_link_libraries(simxr_app_settings PRIVATE advapi32)
endif()

add_library(simxr_connection STATIC
    src/app/services/RuntimeConnection.cpp
    src/app/services/LaunchAttachment.cpp
    src/app/services/SimulationEdits.cpp
    src/app/services/UndoStack.cpp
    src/app/services/PlayerRig.cpp
    src/app/services/RuntimePanelProtocol.cpp
    src/app/services/FrameHistoryFeed.cpp
    src/app/services/TimelineHistory.cpp
    src/app/services/HistoryStore.cpp
    src/app/services/InputHistory.cpp
    src/app/services/SpatialMath.cpp
    src/app/services/HapticFeedback.cpp
    src/app/services/ManualInputController.cpp
    src/app/services/WorkerConnection.cpp
    src/app/services/RuntimePanelRequests.cpp
    src/app/services/DebuggerPanelController.cpp
    src/app/services/InspectionWorkspace.cpp
    src/app/services/ImageExportTask.cpp)
if(WIN32)
    target_sources(simxr_connection PRIVATE src/app/platform/windows/LaunchAttachment.cpp)
else()
    target_sources(simxr_connection PRIVATE src/app/platform/LaunchAttachment.cpp)
endif()
simxr_configure_target(simxr_connection)
target_link_libraries(simxr_connection PUBLIC simxr_control PRIVATE simxr_app_platform)

add_library(simxr_recording_controller STATIC src/app/services/RecordingController.cpp src/app/services/LibraryCatalog.cpp)
simxr_configure_target(simxr_recording_controller)
target_link_libraries(simxr_recording_controller PUBLIC simxr_connection simxr_recording PRIVATE simxr_app_platform)

add_library(simxr_graphics STATIC
    src/graphics/ColorPixels.cpp
    src/graphics/Swapchain.cpp
    src/graphics/SwapchainAccess.cpp
    src/graphics/SwapchainWaits.cpp
    src/graphics/LayerBuilder.cpp
    src/graphics/Compositor.cpp
    src/graphics/FrameSubmission.cpp
    src/graphics/SessionComposition.cpp
    src/graphics/RotationalReprojection.cpp
    src/graphics/CompositionBounds.cpp)
simxr_configure_target(simxr_graphics)
target_link_libraries(simxr_graphics PUBLIC simxr_core)
target_compile_features(simxr_graphics PUBLIC cxx_std_20)
target_link_libraries(simxr_control PUBLIC simxr_graphics)

if(WIN32)
    find_path(SIMXR_OPENGL_INCLUDE_DIR GL/glext.h REQUIRED)
    if(NOT EXISTS "${SIMXR_OPENGL_INCLUDE_DIR}/GL/wglext.h")
        message(FATAL_ERROR "SIMXR_OPENGL_INCLUDE_DIR must also contain GL/wglext.h")
    endif()
    add_library(simxr_opengl_headers INTERFACE)
    target_include_directories(simxr_opengl_headers SYSTEM INTERFACE "${SIMXR_OPENGL_INCLUDE_DIR}")
    file(READ "${PROJECT_SOURCE_DIR}/shaders/Compositor.hlsl" SIMXR_COMPOSITOR_HLSL)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/shaders/Compositor.hlsl")
    configure_file(cmake/CompositorHlsl.h.in generated/CompositorHlsl.h @ONLY)
    target_sources(simxr_core PRIVATE src/control/legacy/WindowsRuntimeSettings.cpp
        src/platform/windows/WindowsClock.cpp src/platform/windows/WindowsRuntimePaths.cpp)
    add_library(simxr_d3d11 STATIC src/graphics/backends/d3d11/D3D11Backend.cpp)
    simxr_configure_target(simxr_d3d11)
    target_include_directories(simxr_d3d11 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(simxr_d3d11 PUBLIC simxr_graphics)
    add_library(simxr_d3d12 STATIC src/graphics/backends/d3d12/D3D12Backend.cpp)
    simxr_configure_target(simxr_d3d12)
    target_include_directories(simxr_d3d12 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(simxr_d3d12 PUBLIC simxr_graphics)

    if(BUILD_TESTING)
        simxr_add_test(d3d11_compositor_tests tests/graphics/D3D11CompositorTests.cpp d3d11_compositor)
        target_link_libraries(d3d11_compositor_tests PRIVATE simxr_d3d11 simxr_d3d12 d3d11 d3d12 dxgi)
    endif()
endif()

if(SIMXR_BUILD_RUNTIME OR SIMXR_BUILD_PROBES OR SIMXR_BUILD_COMPANION)
    find_path(SIMXR_VULKAN_INCLUDE_DIR vulkan/vulkan.h
        HINTS "$ENV{SIMXR_VULKAN_INCLUDE_DIR}" "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
    if(NOT SIMXR_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Install Vulkan headers or set SIMXR_VULKAN_INCLUDE_DIR / VULKAN_SDK.")
    endif()
endif()

if(SIMXR_BUILD_RUNTIME OR SIMXR_BUILD_PROBES OR SIMXR_BUILD_COMPANION)
    if(WIN32)
        find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    else()
        find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
    endif()
    include(cmake/SimxrVulkanLoader.cmake)
    add_library(simxr_vulkan STATIC
        src/graphics/backends/vulkan/VulkanVideoHistory.cpp
        src/graphics/backends/vulkan/VulkanVideoConversion.cpp
        src/graphics/backends/vulkan/VulkanVideoSession.cpp
        src/graphics/backends/vulkan/VulkanDevice.cpp
        src/graphics/backends/vulkan/VulkanResources.cpp
        src/graphics/backends/vulkan/VulkanExternal.cpp
        src/graphics/backends/vulkan/VulkanReprojection.cpp
        src/graphics/backends/vulkan/VulkanInspection.cpp
        src/graphics/backends/vulkan/VulkanInspectionWork.cpp
        src/graphics/backends/vulkan/VulkanReadbackBatch.cpp
        src/graphics/backends/vulkan/VulkanInspectionDevice.cpp
        src/graphics/backends/vulkan/VulkanSubmission.cpp
        src/graphics/backends/vulkan/VulkanFrameSync.cpp
        src/graphics/backends/vulkan/VulkanBackend.cpp)
    target_sources(simxr_vulkan PRIVATE src/graphics/backends/vulkan/VulkanSwapchain.cpp
        src/graphics/backends/vulkan/VulkanSessionGraphics.cpp
        src/graphics/backends/vulkan/VulkanInstanceGraphics.cpp
        src/graphics/backends/vulkan/VulkanFrameSubmission.cpp)
    simxr_configure_target(simxr_vulkan)
    target_include_directories(simxr_vulkan PUBLIC "${SIMXR_VULKAN_INCLUDE_DIR}" PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(simxr_vulkan PUBLIC simxr_graphics simxr_vulkan_loader)
    if(WIN32)
        file(READ "${PROJECT_SOURCE_DIR}/shaders/generated/Compositor.vertex.glsl" SIMXR_COMPOSITOR_VERTEX_GLSL)
        file(READ "${PROJECT_SOURCE_DIR}/shaders/generated/Compositor.pixel.glsl" SIMXR_COMPOSITOR_PIXEL_GLSL)
        file(READ "${PROJECT_SOURCE_DIR}/shaders/generated/Compositor.depth_ms.glsl" SIMXR_COMPOSITOR_DEPTH_MS_GLSL)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/shaders/generated/Compositor.vertex.glsl"
            "${PROJECT_SOURCE_DIR}/shaders/generated/Compositor.pixel.glsl"
            "${PROJECT_SOURCE_DIR}/shaders/generated/Compositor.depth_ms.glsl")
        configure_file(cmake/CompositorGlsl.h.in generated/CompositorGlsl.h @ONLY)
        add_library(simxr_opengl STATIC src/graphics/backends/opengl/OpenGLBackend.cpp
            src/platform/windows/OpenGLContextScope.cpp src/platform/windows/OpenGLLibrary.cpp)
        simxr_configure_target(simxr_opengl)
        target_include_directories(simxr_opengl PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(simxr_opengl PUBLIC simxr_vulkan simxr_opengl_headers gdi32 user32)
        if(BUILD_TESTING)
            simxr_add_test(opengl_compositor_tests tests/graphics/OpenGLCompositorTests.cpp opengl_compositor)
            target_link_libraries(opengl_compositor_tests PRIVATE simxr_opengl opengl32)
            if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 8)
                target_compile_definitions(opengl_compositor_tests PRIVATE SIMXR_TEST_WGL_FAILURE)
                target_link_options(opengl_compositor_tests PRIVATE "LINKER:--wrap=__imp_GetProcAddress" "LINKER:--wrap=__imp_FreeLibrary")
            endif()
        endif()
        target_sources(simxr_vulkan PRIVATE src/graphics/backends/windows/WindowsVulkanImport.cpp)
        target_sources(simxr_d3d12 PRIVATE src/graphics/backends/d3d12/D3D12VulkanImport.cpp)
        target_link_libraries(simxr_d3d12 PUBLIC simxr_vulkan)
        target_sources(simxr_d3d11 PRIVATE src/graphics/backends/d3d11/D3D11VulkanImport.cpp)
        target_link_libraries(simxr_d3d11 PUBLIC simxr_vulkan)
        add_library(simxr_windows_graphics STATIC src/graphics/backends/windows/WindowsSessionGraphics.cpp
            src/graphics/backends/windows/WindowsGraphicsCapabilities.cpp)
        simxr_configure_target(simxr_windows_graphics)
        target_link_libraries(simxr_windows_graphics PUBLIC simxr_d3d11 simxr_d3d12 simxr_opengl)
        if(TARGET d3d11_compositor_tests)
            target_link_libraries(d3d11_compositor_tests PRIVATE simxr_windows_graphics)
            target_compile_definitions(d3d11_compositor_tests PRIVATE SIMXR_TEST_D3D11_VULKAN)
            add_test(NAME d3d12_compositor COMMAND d3d11_compositor_tests --d3d12)
        endif()
    endif()
    add_library(simxr_vulkan_control STATIC src/control/backends/vulkan/VulkanImageInspection.cpp
        src/control/backends/vulkan/VulkanControlHandle.cpp
        src/control/backends/vulkan/VulkanImageConsumer.cpp
        src/control/legacy/backends/vulkan/VulkanLegacyCapture.cpp
        src/control/legacy/backends/vulkan/WindowsVulkanCaptureChannel.cpp)
    simxr_configure_target(simxr_vulkan_control)
    target_link_libraries(simxr_vulkan_control PUBLIC simxr_control simxr_vulkan simxr_diagnostics)

    if(SIMXR_BUILD_PROBES OR SIMXR_BUILD_COMPANION OR TARGET d3d11_compositor_tests)
        add_library(simxr_probe_vulkan STATIC probes/VulkanProbeInstance.cpp)
        simxr_configure_target(simxr_probe_vulkan)
        target_include_directories(simxr_probe_vulkan PUBLIC "${SIMXR_VULKAN_INCLUDE_DIR}")
        target_link_libraries(simxr_probe_vulkan PUBLIC simxr_vulkan_loader)
        if(TARGET d3d11_compositor_tests)
            target_link_libraries(d3d11_compositor_tests PRIVATE simxr_probe_vulkan)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_library(simxr_probe_sharing STATIC probes/VulkanProbeSupport.cpp
                probes/LinuxFdChannel.cpp probes/VulkanProcessPeer.cpp)
            simxr_configure_target(simxr_probe_sharing)
            target_link_libraries(simxr_probe_sharing PUBLIC simxr_probe_vulkan)
        endif()
    endif()
endif()

add_library(simxr_diagnostics STATIC
    src/flicker_detector.cpp
    src/diagnostics/FlickerSample.cpp
    src/diagnostics/FlickerMonitors.cpp
    src/diagnostics/platform/ArtifactFile.cpp
    src/diagnostics/platform/RenderDocCapture.cpp
    src/diagnostics/FlickerArtifacts.cpp)
simxr_configure_target(simxr_diagnostics)
target_include_directories(simxr_diagnostics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(simxr_diagnostics PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

if(SIMXR_BUILD_RUNTIME)
    if(WIN32)
        set(SIMXR_RUNTIME_SOURCE src/runtime.cpp)
        list(APPEND SIMXR_RUNTIME_SOURCE src/platform/windows/OpenGLSwapchain.cpp
            src/platform/windows/Direct3DSwapchain.cpp)
    else()
        set(SIMXR_RUNTIME_SOURCE src/runtime_linux.cpp src/platform/linux/LinuxRuntime.cpp)
    endif()
    add_library(openxr_simulator SHARED ${SIMXR_RUNTIME_SOURCE})
    simxr_configure_target(openxr_simulator)
    simxr_set_output_directory(openxr_simulator)
    target_include_directories(openxr_simulator PRIVATE "${SIMXR_VULKAN_INCLUDE_DIR}")
    target_link_libraries(openxr_simulator PRIVATE simxr_core simxr_graphics simxr_control simxr_diagnostics Threads::Threads ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(openxr_simulator PRIVATE simxr_windows_graphics)
        target_link_libraries(openxr_simulator PRIVATE simxr_vulkan simxr_vulkan_control)
    else()
        find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
        target_link_libraries(openxr_simulator PRIVATE "${SIMXR_VULKAN_LIBRARY}")
        target_link_libraries(openxr_simulator PRIVATE simxr_vulkan simxr_vulkan_control)
    endif()

    file(MAKE_DIRECTORY "${SIMXR_OUTPUT_DIRECTORY}")
    set(SIMXR_LIBRARY_FILENAME "${CMAKE_SHARED_LIBRARY_PREFIX}openxr_simulator${CMAKE_SHARED_LIBRARY_SUFFIX}")
    configure_file(cmake/RuntimeManifest.json.in "${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json" @ONLY)
    install(TARGETS openxr_simulator RUNTIME DESTINATION bin LIBRARY DESTINATION bin)
    install(FILES "${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json" DESTINATION bin)
endif()

if(BUILD_TESTING)
    simxr_add_test(recording_tests tests/automation/RecordingTests.cpp recording)
    target_link_libraries(recording_tests PRIVATE simxr_recording)
    simxr_add_test(companion_settings_tests tests/app/CompanionSettingsTests.cpp companion_settings)
    target_link_libraries(companion_settings_tests PRIVATE simxr_app_settings simxr_core)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        simxr_add_test(process_supervisor_tests tests/app/ProcessSupervisorTests.cpp process_supervisor)
        target_link_libraries(process_supervisor_tests PRIVATE simxr_app_settings)
    endif()
    if(WIN32)
        add_library(windows_module_lifetime_fixture SHARED tests/platform/WindowsModuleLifetimeFixture.cpp)
        simxr_configure_target(windows_module_lifetime_fixture)
        target_link_libraries(windows_module_lifetime_fixture PRIVATE simxr_core)
        add_executable(windows_module_lifetime_tests tests/platform/WindowsModuleLifetimeTests.cpp)
        simxr_configure_target(windows_module_lifetime_tests)
        target_link_libraries(windows_module_lifetime_tests PRIVATE simxr_core)
        if(MINGW)
            target_link_options(windows_module_lifetime_tests PRIVATE -municode)
        endif()
        add_test(NAME windows_module_lifetime COMMAND windows_module_lifetime_tests
            "$<TARGET_FILE:windows_module_lifetime_fixture>")
        set_tests_properties(windows_module_lifetime PROPERTIES TIMEOUT 20)
        add_executable(windows_data_directory_tests tests/platform/WindowsDataDirectoryTests.cpp)
        simxr_configure_target(windows_data_directory_tests)
        target_link_libraries(windows_data_directory_tests PRIVATE simxr_core)
        if(MINGW)
            target_link_options(windows_data_directory_tests PRIVATE -static)
        endif()
        foreach(mode override default fallback)
            add_test(NAME windows_data_directory_${mode} COMMAND windows_data_directory_tests ${mode})
            set_tests_properties(windows_data_directory_${mode} PROPERTIES TIMEOUT 10)
        endforeach()
        add_executable(windows_running_apps_tests tests/app/WindowsRunningAppsTests.cpp)
        simxr_configure_target(windows_running_apps_tests)
        target_link_libraries(windows_running_apps_tests PRIVATE simxr_app_settings)
        target_include_directories(windows_running_apps_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        if(MINGW)
            target_link_options(windows_running_apps_tests PRIVATE -static)
        endif()
        add_test(NAME windows_running_apps COMMAND windows_running_apps_tests)
        set_tests_properties(windows_running_apps PROPERTIES TIMEOUT 10)
        add_executable(windows_launch_tests tests/app/WindowsLaunchTests.cpp)
        simxr_configure_target(windows_launch_tests)
        target_link_libraries(windows_launch_tests PRIVATE simxr_app_settings)
        if(MINGW)
            target_link_options(windows_launch_tests PRIVATE -municode -static)
        endif()
        add_test(NAME windows_launch COMMAND windows_launch_tests)
        set_tests_properties(windows_launch PROPERTIES TIMEOUT 20)
    endif()
    simxr_add_test(game_bridge_tests tests/automation/GameBridgeTests.cpp game_bridge)
    target_link_libraries(game_bridge_tests PRIVATE simxr_control)
    simxr_add_test(legacy_controller_motion_tests tests/simulation/LegacyControllerMotionTests.cpp legacy_controller_motion)
    target_link_libraries(legacy_controller_motion_tests PRIVATE simxr_core)
    simxr_add_test(command_scheduler_tests tests/simulation/CommandSchedulerTests.cpp command_scheduler)
    target_link_libraries(command_scheduler_tests PRIVATE simxr_core)
    simxr_add_test(history_store_tests tests/app/HistoryStoreTests.cpp history_store)
    target_link_libraries(history_store_tests PRIVATE simxr_connection)
    simxr_add_test(player_rig_tests tests/app/PlayerRigTests.cpp player_rig)
    target_link_libraries(player_rig_tests PRIVATE simxr_connection)
    simxr_add_test(spatial_math_tests tests/app/SpatialMathTests.cpp spatial_math)
    target_link_libraries(spatial_math_tests PRIVATE simxr_connection)
    simxr_add_test(simulation_edits_tests tests/app/SimulationEditsTests.cpp simulation_edits)
    target_link_libraries(simulation_edits_tests PRIVATE simxr_connection)
    simxr_add_test(haptic_feedback_tests tests/app/HapticFeedbackTests.cpp haptic_feedback)
    target_link_libraries(haptic_feedback_tests PRIVATE simxr_connection)
    simxr_add_test(runtime_panel_state_tests tests/app/RuntimePanelStateTests.cpp runtime_panel_state)
    target_link_libraries(runtime_panel_state_tests PRIVATE simxr_connection)
    simxr_add_test(library_catalog_tests tests/app/LibraryCatalogTests.cpp library_catalog)
    target_link_libraries(library_catalog_tests PRIVATE simxr_recording_controller)
    simxr_add_test(runtime_connection_tests tests/app/RuntimeConnectionTests.cpp runtime_connection)
    target_link_libraries(runtime_connection_tests PRIVATE simxr_connection)
    simxr_add_test(mp4_video_tests tests/media/Mp4VideoTests.cpp mp4_video)
    target_link_libraries(mp4_video_tests PRIVATE simxr_control)
    simxr_add_test(frame_preview_history_tests tests/presentation/FramePreviewHistoryTests.cpp frame_preview_history)
    target_link_libraries(frame_preview_history_tests PRIVATE simxr_core)
    simxr_add_test(frame_image_history_tests tests/presentation/FrameImageHistoryTests.cpp frame_image_history)
    target_link_libraries(frame_image_history_tests PRIVATE simxr_core)
    simxr_add_test(inspection_document_tests tests/control/InspectionDocumentTests.cpp inspection_document)
    target_link_libraries(inspection_document_tests PRIVATE simxr_control)
    simxr_add_test(wire_protocol_tests tests/control/WireProtocolTests.cpp wire_protocol)
    target_link_libraries(wire_protocol_tests PRIVATE simxr_control)
    simxr_add_test(control_service_tests tests/control/ControlServiceTests.cpp control_service)
    target_link_libraries(control_service_tests PRIVATE simxr_control)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        simxr_add_test(recording_controller_tests tests/app/RecordingControllerTests.cpp recording_controller)
        target_link_libraries(recording_controller_tests PRIVATE simxr_recording_controller)
        simxr_add_test(rig_launch_attachment_tests tests/app/LaunchAttachmentTests.cpp rig_launch_attachment)
        target_link_libraries(rig_launch_attachment_tests PRIVATE simxr_connection)
        simxr_add_test(control_fd_tests tests/control/ControlFdTests.cpp control_fd)
        target_link_libraries(control_fd_tests PRIVATE simxr_control)
        simxr_add_test(legacy_file_channel_tests tests/control/LegacyFileChannelTests.cpp legacy_file_channel)
        target_link_libraries(legacy_file_channel_tests PRIVATE simxr_control)
        simxr_add_test(linux_runtime_status_tests tests/control/LinuxRuntimeStatusTests.cpp linux_runtime_status)
        target_link_libraries(linux_runtime_status_tests PRIVATE simxr_control)
        simxr_add_test(image_control_service_tests tests/control/ImageControlServiceTests.cpp image_control_service)
        target_link_libraries(image_control_service_tests PRIVATE simxr_control)
        simxr_add_test(live_preview_control_tests tests/control/LivePreviewControlTests.cpp live_preview_control)
        target_link_libraries(live_preview_control_tests PRIVATE simxr_control)
        simxr_add_test(local_control_client_tests tests/control/LocalControlClientTests.cpp local_control_client)
        target_link_libraries(local_control_client_tests PRIVATE simxr_control)
        add_executable(image_control_endpoint_host tests/control/ImageEndpointHost.cpp)
        simxr_configure_target(image_control_endpoint_host)
        target_link_libraries(image_control_endpoint_host PRIVATE simxr_control)
        add_executable(control_endpoint_host tests/control/ControlEndpointHost.cpp)
        simxr_configure_target(control_endpoint_host)
        target_link_libraries(control_endpoint_host PRIVATE simxr_control)
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME python_video COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_video.py")
            add_test(NAME python_recording COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_recording.py")
            add_test(NAME python_pose_sequence_adapter COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_pose_sequence_adapter.py")
            add_test(NAME python_legacy_control COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_legacy_control.py")
            set(SIMXR_MCP_APPLICATION_PYTHON "${Python3_EXECUTABLE}")
            if(SIMXR_MCP_TEST_PYTHON)
                set(SIMXR_MCP_APPLICATION_PYTHON "${SIMXR_MCP_TEST_PYTHON}")
            endif()
            add_test(NAME python_mcp_application COMMAND "${SIMXR_MCP_APPLICATION_PYTHON}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_mcp_application.py" "$<TARGET_FILE:control_endpoint_host>")
            set_tests_properties(python_mcp_application PROPERTIES TIMEOUT 30)
            add_test(NAME python_windows_transport_model COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_windows_transport.py")
            add_test(NAME python_control_fds COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_fd_control.py")
            set_tests_properties(python_control_fds PROPERTIES TIMEOUT 30)
            add_test(NAME python_image_control COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_image_control.py" "$<TARGET_FILE:image_control_endpoint_host>")
            set_tests_properties(python_image_control PROPERTIES TIMEOUT 30)
            add_test(NAME python_inspection COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_inspection_document.py" "$<TARGET_FILE:inspection_document_tests>")
            add_test(NAME python_control COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_python_control.py" "$<TARGET_FILE:control_endpoint_host>")
            set_tests_properties(python_control PROPERTIES TIMEOUT 30)
            add_test(NAME python_mcp_control COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/control/test_mcp_control.py" "$<TARGET_FILE:control_endpoint_host>")
            set_tests_properties(python_mcp_control PROPERTIES TIMEOUT 30)
        endif()
    endif()
    simxr_add_test(projection_timing_tests tests/projection_timing_tests.cpp projection_timing)
    simxr_add_test(math_tests tests/simulation/MathTests.cpp math)
    target_link_libraries(math_tests PRIVATE simxr_core)
    simxr_add_test(frame_history_tests tests/presentation/FrameHistoryTests.cpp frame_history)
    target_link_libraries(frame_history_tests PRIVATE simxr_core)
    simxr_add_test(frame_pool_tests tests/graphics/FramePoolTests.cpp frame_pool)
    target_link_libraries(frame_pool_tests PRIVATE simxr_core)
    simxr_add_test(snapshot_store_tests tests/presentation/SnapshotStoreTests.cpp snapshot_store)
    target_link_libraries(snapshot_store_tests PRIVATE simxr_core)
    simxr_add_test(debug_controller_tests tests/presentation/DebugControllerTests.cpp debug_controller)
    target_link_libraries(debug_controller_tests PRIVATE simxr_core)
    simxr_add_test(presentation_controller_tests tests/presentation/PresentationControllerTests.cpp presentation_controller)
    target_link_libraries(presentation_controller_tests PRIVATE simxr_core)
    simxr_add_test(virtual_display_tests tests/presentation/VirtualDisplayTests.cpp virtual_display)
    target_link_libraries(virtual_display_tests PRIVATE simxr_core)
    simxr_add_test(rotational_display_tests tests/presentation/RotationalDisplayTests.cpp rotational_display)
    target_link_libraries(rotational_display_tests PRIVATE simxr_core)
    simxr_add_test(frame_scheduler_tests tests/presentation/FrameSchedulerTests.cpp frame_scheduler)
    target_link_libraries(frame_scheduler_tests PRIVATE simxr_core)
    simxr_add_test(layer_builder_tests tests/graphics/LayerBuilderTests.cpp layer_builder)
    target_link_libraries(layer_builder_tests PRIVATE simxr_graphics)
    simxr_add_test(rotational_reprojection_tests tests/graphics/RotationalReprojectionTests.cpp rotational_reprojection)
    target_link_libraries(rotational_reprojection_tests PRIVATE simxr_graphics)
    simxr_add_test(compositor_tests tests/graphics/CompositorTests.cpp compositor)
    target_link_libraries(compositor_tests PRIVATE simxr_graphics)
    simxr_add_test(swapchain_access_tests tests/graphics/SwapchainAccessTests.cpp swapchain_access)
    target_link_libraries(swapchain_access_tests PRIVATE simxr_graphics)
    set_tests_properties(swapchain_access PROPERTIES TIMEOUT 15)
    simxr_add_test(swapchain_tests tests/graphics/SwapchainTests.cpp swapchain)
    target_link_libraries(swapchain_tests PRIVATE simxr_graphics)
    simxr_add_test(event_queue_tests tests/runtime/EventQueueTests.cpp event_queue)
    target_link_libraries(event_queue_tests PRIVATE simxr_core)
    simxr_add_test(haptic_state_tests tests/simulation/HapticStateTests.cpp haptic_state)
    target_link_libraries(haptic_state_tests PRIVATE simxr_core)
    simxr_add_test(extension_registry_tests tests/runtime/ExtensionRegistryTests.cpp extension_registry)
    target_link_libraries(extension_registry_tests PRIVATE simxr_core)
    simxr_add_test(handle_registry_tests tests/runtime/HandleRegistryTests.cpp handle_registry)
    target_link_libraries(handle_registry_tests PRIVATE simxr_core)
    simxr_add_test(layer_validation_tests tests/runtime/LayerValidationTests.cpp layer_validation)
    target_link_libraries(layer_validation_tests PRIVATE simxr_core)
    simxr_add_test(action_registry_tests tests/simulation/ActionRegistryTests.cpp action_registry)
    target_link_libraries(action_registry_tests PRIVATE simxr_core)
    simxr_add_test(runtime_tests tests/runtime/RuntimeTests.cpp runtime)
    target_link_libraries(runtime_tests PRIVATE simxr_core)
    simxr_add_test(windows_legacy_channel_tests tests/control/WindowsLegacyChannelTests.cpp windows_legacy_channel)
    target_link_libraries(windows_legacy_channel_tests PRIVATE simxr_control)
    simxr_add_test(flicker_diagnostics_tests tests/diagnostics/FlickerDiagnosticsTests.cpp flicker_diagnostics)
    target_link_libraries(flicker_diagnostics_tests PRIVATE simxr_diagnostics simxr_core)
    simxr_add_test(legacy_capture_image_tests tests/control/LegacyCaptureImageTests.cpp legacy_capture_image)
    target_link_libraries(legacy_capture_image_tests PRIVATE simxr_control)
    simxr_add_test(legacy_input_tests tests/control/LegacyInputTests.cpp legacy_input)
    target_link_libraries(legacy_input_tests PRIVATE simxr_control)
    simxr_add_test(session_tests tests/runtime/SessionTests.cpp session)
    target_link_libraries(session_tests PRIVATE simxr_core)
    simxr_add_test(session_failure_tests tests/runtime/SessionFailureTests.cpp session_failure)
    target_link_libraries(session_failure_tests PRIVATE simxr_core)
    simxr_add_test(simulation_extensions_tests tests/runtime/SimulationExtensionsTests.cpp simulation_extensions)
    target_link_libraries(simulation_extensions_tests PRIVATE simxr_core)
    simxr_add_test(space_queries_tests tests/runtime/SpaceQueriesTests.cpp space_queries)
    target_link_libraries(space_queries_tests PRIVATE simxr_core)
    simxr_add_test(space_system_tests tests/simulation/SpaceSystemTests.cpp space_system)
    target_link_libraries(space_system_tests PRIVATE simxr_core)
    simxr_add_test(interaction_devices_tests tests/runtime/extensions/InteractionDevicesTests.cpp interaction_devices)
    target_link_libraries(interaction_devices_tests PRIVATE simxr_core)
    simxr_add_test(body_tracking_tests tests/runtime/extensions/BodyTrackingTests.cpp body_tracking)
    target_link_libraries(body_tracking_tests PRIVATE simxr_core)
    simxr_add_test(body_model_tests tests/simulation/BodyModelTests.cpp body_model)
    target_link_libraries(body_model_tests PRIVATE simxr_core)
    simxr_add_test(tracking_tests tests/simulation/TrackingTests.cpp tracking)
    target_link_libraries(tracking_tests PRIVATE simxr_core)
    simxr_add_test(quad_views_tests tests/runtime/QuadViewsTests.cpp quad_views)
    target_link_libraries(quad_views_tests PRIVATE simxr_graphics)
    simxr_add_test(view_configuration_tests tests/simulation/ViewConfigurationTests.cpp view_configuration)
    target_link_libraries(view_configuration_tests PRIVATE simxr_core)
    if(WIN32)
        simxr_add_test(opengl_swapchain_tests tests/platform/OpenGLSwapchainTests.cpp opengl_swapchain)
        target_sources(opengl_swapchain_tests PRIVATE src/platform/windows/OpenGLSwapchain.cpp
            src/platform/windows/OpenGLContextScope.cpp src/platform/windows/OpenGLLibrary.cpp)
        target_link_libraries(opengl_swapchain_tests PRIVATE simxr_core simxr_opengl_headers opengl32 user32 gdi32)
        set_tests_properties(opengl_swapchain PROPERTIES SKIP_RETURN_CODE 77)
        simxr_add_test(windows_control_transport_tests tests/control/WindowsControlTransportTests.cpp windows_control_transport)
        target_link_libraries(windows_control_transport_tests PRIVATE simxr_control)
        simxr_add_test(ui_resolution_tests tests/ui_resolution_tests.cpp ui_resolution)
        simxr_add_test(ui_input_polling_tests tests/ui_input_polling_tests.cpp ui_input_polling)
        target_link_libraries(ui_resolution_tests PRIVATE simxr_core)
        target_link_libraries(ui_input_polling_tests PRIVATE simxr_core)
        simxr_add_test(mcp_command_tests tests/mcp_command_tests.cpp mcp_commands)
        target_link_libraries(mcp_command_tests PRIVATE simxr_control)
    endif()
endif()

if(SIMXR_BUILD_PROBES)
    if(WIN32)
        find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    else()
        find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
    endif()
    add_executable(foveation_capabilities_probe probes/FoveationCapabilitiesProbe.cpp)
    simxr_configure_target(foveation_capabilities_probe)
    simxr_set_output_directory(foveation_capabilities_probe)
    target_include_directories(foveation_capabilities_probe PRIVATE "${SIMXR_VULKAN_INCLUDE_DIR}")
    target_link_libraries(foveation_capabilities_probe PRIVATE simxr_probe_vulkan)
    if(BUILD_TESTING)
        add_test(NAME foveation_capabilities COMMAND foveation_capabilities_probe)
        add_test(NAME foveation_density_map COMMAND foveation_capabilities_probe --require-density-map)
        set_tests_properties(foveation_capabilities foveation_density_map PROPERTIES TIMEOUT 30)
        set_tests_properties(foveation_capabilities foveation_density_map PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    if(BUILD_TESTING)
        add_executable(vulkan_external_fence_probe probes/VulkanExternalFenceProbe.cpp)
        simxr_configure_target(vulkan_external_fence_probe)
        simxr_set_output_directory(vulkan_external_fence_probe)
        target_link_libraries(vulkan_external_fence_probe PRIVATE simxr_vulkan simxr_probe_vulkan)
        if(WIN32)
            set(fence_test windows_vulkan_external_completion)
            add_test(NAME ${fence_test} COMMAND vulkan_external_fence_probe --timeline-only)
        else()
            set(fence_test linux_vulkan_external_fence)
            add_test(NAME ${fence_test} COMMAND vulkan_external_fence_probe)
        endif()
        set_tests_properties(${fence_test} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
    add_executable(vulkan_video_probe probes/VulkanVideoProbe.cpp)
    simxr_configure_target(vulkan_video_probe)
    simxr_set_output_directory(vulkan_video_probe)
    target_link_libraries(vulkan_video_probe PRIVATE simxr_vulkan simxr_probe_vulkan)
    if(NOT WIN32)
        add_executable(vulkan_compositor_probe probes/VulkanCompositorProbe.cpp)
        simxr_configure_target(vulkan_compositor_probe)
        simxr_set_output_directory(vulkan_compositor_probe)
        target_link_libraries(vulkan_compositor_probe PRIVATE simxr_vulkan simxr_probe_vulkan)
        add_executable(vulkan_inspection_probe probes/VulkanInspectionProbe.cpp probes/VulkanImageDeliveryProbe.cpp
            probes/VulkanReprojectionProbe.cpp probes/VulkanRetentionProbe.cpp)
        simxr_configure_target(vulkan_inspection_probe)
        simxr_set_output_directory(vulkan_inspection_probe)
        target_link_libraries(vulkan_inspection_probe PRIVATE simxr_vulkan simxr_probe_vulkan)
        target_link_libraries(vulkan_inspection_probe PRIVATE simxr_vulkan_control)
        add_executable(vulkan_control_image_probe probes/VulkanControlImageProbe.cpp probes/VulkanImageDeliveryProbe.cpp)
        simxr_configure_target(vulkan_control_image_probe)
        simxr_set_output_directory(vulkan_control_image_probe)
        target_link_options(vulkan_control_image_probe PRIVATE "LINKER:--wrap=vkQueueSubmit" "LINKER:--wrap=vkEndCommandBuffer")
        target_link_libraries(vulkan_control_image_probe PRIVATE simxr_vulkan_control simxr_probe_vulkan)
        target_link_options(vulkan_inspection_probe PRIVATE "LINKER:--wrap=vkQueueSubmit" "LINKER:--wrap=vkEndCommandBuffer")
        if(BUILD_TESTING)
            add_test(NAME linux_vulkan_compositor COMMAND vulkan_compositor_probe)
            add_test(NAME linux_vulkan_compositor_pixels COMMAND vulkan_compositor_probe --verify-pixels)
            add_test(NAME linux_vulkan_inspection COMMAND vulkan_inspection_probe)
            add_test(NAME linux_vulkan_inspection_pixels COMMAND vulkan_inspection_probe --verify-pixels)
            set_tests_properties(linux_vulkan_inspection linux_vulkan_inspection_pixels
                PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
            set_tests_properties(linux_vulkan_compositor linux_vulkan_compositor_pixels PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif()
        add_executable(vulkan_sharing_probe probes/VulkanSharingProbe.cpp)
        simxr_configure_target(vulkan_sharing_probe)
        simxr_set_output_directory(vulkan_sharing_probe)
        target_include_directories(vulkan_sharing_probe PRIVATE "${SIMXR_VULKAN_INCLUDE_DIR}")
        target_link_libraries(vulkan_sharing_probe PRIVATE simxr_probe_sharing)
        if(BUILD_TESTING)
            add_test(NAME linux_vulkan_sharing COMMAND vulkan_sharing_probe)
            add_test(NAME linux_vulkan_sharing_pixels COMMAND vulkan_sharing_probe --verify-pixels)
            add_test(NAME linux_vulkan_sharing_process COMMAND vulkan_sharing_probe --cross-process)
            add_test(NAME linux_vulkan_sharing_process_pixels COMMAND vulkan_sharing_probe --cross-process --verify-pixels)
            add_test(NAME linux_vulkan_sharing_disconnect COMMAND vulkan_sharing_probe --verify-disconnect)
            set_tests_properties(linux_vulkan_sharing linux_vulkan_sharing_pixels
                linux_vulkan_sharing_process linux_vulkan_sharing_process_pixels linux_vulkan_sharing_disconnect PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
        endif()
    endif()
    if(NOT WIN32)
        find_package(OpenGL QUIET COMPONENTS OpenGL EGL)
        if(TARGET OpenGL::OpenGL AND TARGET OpenGL::EGL)
            add_executable(opengl_vulkan_sharing_probe probes/OpenGLVulkanSharingProbe.cpp)
            simxr_configure_target(opengl_vulkan_sharing_probe)
            simxr_set_output_directory(opengl_vulkan_sharing_probe)
            target_include_directories(opengl_vulkan_sharing_probe PRIVATE "${SIMXR_VULKAN_INCLUDE_DIR}")
            target_link_libraries(opengl_vulkan_sharing_probe PRIVATE simxr_probe_sharing OpenGL::OpenGL OpenGL::EGL)
            if(BUILD_TESTING)
                add_test(NAME linux_opengl_vulkan_sharing COMMAND opengl_vulkan_sharing_probe)
                add_test(NAME linux_opengl_vulkan_sharing_pixels COMMAND opengl_vulkan_sharing_probe --verify-pixels)
                add_test(NAME linux_opengl_vulkan_sharing_process COMMAND opengl_vulkan_sharing_probe --cross-process)
                add_test(NAME linux_opengl_vulkan_sharing_process_pixels COMMAND opengl_vulkan_sharing_probe --cross-process --verify-pixels)
                add_test(NAME linux_opengl_vulkan_sharing_disconnect COMMAND opengl_vulkan_sharing_probe --verify-disconnect)
                set_tests_properties(linux_opengl_vulkan_sharing linux_opengl_vulkan_sharing_pixels
                    linux_opengl_vulkan_sharing_process linux_opengl_vulkan_sharing_process_pixels linux_opengl_vulkan_sharing_disconnect
                    PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
            endif()
        else()
            message(STATUS "OpenGL/EGL development libraries unavailable; GL/Vulkan sharing probe excluded")
        endif()
    endif()
    find_package(OpenXR CONFIG QUIET)
    if(TARGET OpenXR::openxr_loader)
        set(SIMXR_OPENXR_LOADER_TARGET OpenXR::openxr_loader)
    else()
        find_library(SIMXR_OPENXR_LOADER NAMES openxr_loader libopenxr_loader.so.1
            HINTS "$ENV{OPENXR_SDK}/lib" "$ENV{OPENXR_SDK}/lib/x64")
        if(SIMXR_OPENXR_LOADER)
            add_library(simxr_openxr_loader UNKNOWN IMPORTED)
            set_target_properties(simxr_openxr_loader PROPERTIES IMPORTED_LOCATION "${SIMXR_OPENXR_LOADER}")
            set(SIMXR_OPENXR_LOADER_TARGET simxr_openxr_loader)
        endif()
    endif()
    if(SIMXR_OPENXR_LOADER_TARGET)
        if(WIN32)
            add_executable(xr_probe_d3d11 probe/xr_probe_windows.cpp)
            simxr_configure_target(xr_probe_d3d11)
            simxr_set_output_directory(xr_probe_d3d11)
            target_link_libraries(xr_probe_d3d11 PRIVATE ${SIMXR_OPENXR_LOADER_TARGET} simxr_opengl_headers d3d11 dxgi)
            add_executable(xr_probe_d3d12 probe/xr_probe_windows.cpp)
            simxr_configure_target(xr_probe_d3d12)
            simxr_set_output_directory(xr_probe_d3d12)
            target_compile_definitions(xr_probe_d3d12 PRIVATE SIMXR_PROBE_D3D12)
            target_link_libraries(xr_probe_d3d12 PRIVATE ${SIMXR_OPENXR_LOADER_TARGET} simxr_opengl_headers d3d12 dxgi)
            add_executable(xr_probe_opengl probe/xr_probe_windows.cpp)
            simxr_configure_target(xr_probe_opengl)
            simxr_set_output_directory(xr_probe_opengl)
            target_compile_definitions(xr_probe_opengl PRIVATE SIMXR_PROBE_OPENGL)
            target_link_libraries(xr_probe_opengl PRIVATE ${SIMXR_OPENXR_LOADER_TARGET} simxr_opengl_headers opengl32 gdi32 user32)
            add_executable(xr_probe probe/xr_probe.cpp
                src/control/legacy/WindowsLegacyFiles.cpp src/platform/windows/WindowsRuntimePaths.cpp)
            simxr_configure_target(xr_probe)
            simxr_set_output_directory(xr_probe)
            target_link_libraries(xr_probe PRIVATE ${SIMXR_OPENXR_LOADER_TARGET} d3d12 dxgi)
            find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib" REQUIRED)
            add_executable(xr_probe_vk probe/xr_probe_vk.cpp)
            simxr_configure_target(xr_probe_vk)
            simxr_set_output_directory(xr_probe_vk)
            target_include_directories(xr_probe_vk PRIVATE "${SIMXR_VULKAN_INCLUDE_DIR}")
            target_link_libraries(xr_probe_vk PRIVATE ${SIMXR_OPENXR_LOADER_TARGET} "${SIMXR_VULKAN_LIBRARY}")
            target_sources(xr_probe_vk PRIVATE probe/WindowsPendingDelivery.cpp)
            target_link_libraries(xr_probe_vk PRIVATE simxr_vulkan_control simxr_probe_vulkan)
            foreach(probe xr_probe_d3d11 xr_probe_d3d12 xr_probe_opengl xr_probe_vk)
                target_sources(${probe} PRIVATE probe/WindowsRuntimeModule.cpp)
                if(MINGW)
                    target_link_options(${probe} PRIVATE -municode)
                endif()
            endforeach()
            if(BUILD_TESTING AND SIMXR_BUILD_RUNTIME)
                add_test(NAME windows_d3d12_loader COMMAND xr_probe)
                add_test(NAME windows_vulkan_loader COMMAND xr_probe_vk)
                add_test(NAME windows_vulkan_loader_v1 COMMAND xr_probe_vk)
                foreach(binding d3d11 d3d12 opengl vk)
                    add_test(NAME windows_${binding}_unload COMMAND xr_probe_${binding}
                        --verify-unload "$<TARGET_FILE:openxr_simulator>")
                    set_tests_properties(windows_${binding}_unload PROPERTIES TIMEOUT 90
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;XR_PROBE_FRAMES=12")
                endforeach()
                add_test(NAME windows_vulkan_pending_unload COMMAND xr_probe_vk
                    --verify-pending-unload "$<TARGET_FILE:openxr_simulator>")
                add_test(NAME windows_vulkan_gated_unload COMMAND xr_probe_vk
                    --verify-gated-unload "$<TARGET_FILE:openxr_simulator>")
                set_tests_properties(windows_vulkan_pending_unload windows_vulkan_gated_unload PROPERTIES TIMEOUT 120
                    ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;XR_PROBE_FRAMES=12")
                set_tests_properties(windows_d3d12_loader windows_vulkan_loader windows_vulkan_loader_v1 PROPERTIES TIMEOUT 60)
                set_tests_properties(windows_d3d12_loader windows_vulkan_loader PROPERTIES
                    ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json")
                set_tests_properties(windows_vulkan_loader_v1 PROPERTIES
                    ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;XR_PROBE_VK_ENABLE1=1")
            endif()
        else()
            find_library(SIMXR_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
            add_executable(xr_probe_linux probe/xr_probe_linux.cpp probe/VulkanCreationChecks.cpp)
            simxr_configure_target(xr_probe_linux)
            simxr_set_output_directory(xr_probe_linux)
            target_include_directories(xr_probe_linux PRIVATE "${SIMXR_VULKAN_INCLUDE_DIR}")
            target_link_libraries(xr_probe_linux PRIVATE ${SIMXR_OPENXR_LOADER_TARGET}
                "${SIMXR_VULKAN_LIBRARY}" Threads::Threads ${CMAKE_DL_LIBS})
            if(BUILD_TESTING AND SIMXR_BUILD_RUNTIME)
                foreach(probe_mode IN ITEMS default v1 threaded bgra body devices quad close_retry creation_rollback)
                    if(probe_mode STREQUAL "default")
                        set(probe_test linux_vulkan_loader)
                    else()
                        set(probe_test linux_vulkan_loader_${probe_mode})
                    endif()
                    add_test(NAME ${probe_test} COMMAND xr_probe_linux)
                    set(probe_environment "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json"
                        "SIMXR_DATA_DIR=${CMAKE_BINARY_DIR}/probe-${probe_mode}" "SIMXR_DETERMINISTIC=1")
                    if(probe_mode STREQUAL "v1")
                        list(APPEND probe_environment "SIMXR_PROBE_V1=1")
                    elseif(probe_mode STREQUAL "threaded")
                        list(APPEND probe_environment "SIMXR_PROBE_THREADED=1" "SIMXR_PROBE_FRAMES=128")
                    elseif(probe_mode STREQUAL "quad")
                        list(APPEND probe_environment "SIMXR_PROBE_QUAD=1" "SIMXR_PROBE_GPU_INSPECTION=1")
                    elseif(probe_mode STREQUAL "close_retry")
                        list(APPEND probe_environment "SIMXR_PROBE_CLOSE_RETRY=1")
                    elseif(probe_mode STREQUAL "creation_rollback")
                        list(APPEND probe_environment "SIMXR_PROBE_CREATION_ROLLBACK=1"
                            "SIMXR_CONTROL_SOCKET=${CMAKE_BINARY_DIR}/creation.sock")
                    elseif(probe_mode STREQUAL "devices")
                        list(APPEND probe_environment "SIMXR_PROBE_DEVICES=1")
                    elseif(probe_mode STREQUAL "body")
                        list(APPEND probe_environment "SIMXR_PROBE_BODY=1")
                    elseif(probe_mode STREQUAL "bgra")
                        list(APPEND probe_environment "SIMXR_PROBE_BGRA=1" "SIMXR_PROBE_SRGB=1")
                    endif()
                    set_tests_properties(${probe_test} PROPERTIES ENVIRONMENT "${probe_environment}" TIMEOUT 60)
                endforeach()
                if(Python3_Interpreter_FOUND)
                    add_test(NAME linux_vulkan_control COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_control.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_control PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 60 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_mcp_sequence COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_mcp_sequence.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_mcp_sequence PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_sequence COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_sequence.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_sequence PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_cadence COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_cadence.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_cadence PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json"
                        TIMEOUT 90)
                    add_test(NAME linux_vulkan_reprojection COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_reprojection.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_reprojection PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json"
                        TIMEOUT 90)
                    add_test(NAME linux_vulkan_realtime_recording COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_realtime_recording.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_realtime_recording PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_frame_recording COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_frame_recording.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_frame_recording PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    if(SIMXR_BUILD_AUTOMATION)
                        add_test(NAME linux_vulkan_input_replay COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_input_replay.py"
                            "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_runner>")
                        set_tests_properties(linux_vulkan_input_replay PROPERTIES
                            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                            TIMEOUT 60 SKIP_RETURN_CODE 77)
                        add_test(NAME linux_vulkan_luau_recording COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_luau_recording.py"
                            "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_runner>")
                        set_tests_properties(linux_vulkan_luau_recording PROPERTIES
                            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                            TIMEOUT 60 SKIP_RETURN_CODE 77)
                        add_test(NAME linux_vulkan_luau_depth COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_luau_depth.py"
                            "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_runner>")
                        set_tests_properties(linux_vulkan_luau_depth PROPERTIES
                            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                            TIMEOUT 60 SKIP_RETURN_CODE 77)
                        add_test(NAME linux_vulkan_image_history COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_image_history.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_runner>")
                        set_tests_properties(linux_vulkan_image_history PROPERTIES
                            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                            TIMEOUT 90 SKIP_RETURN_CODE 77)
                    endif()
                    add_test(NAME linux_vulkan_image_window COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_image_window.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_image_window PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_mcp_incident COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_mcp_incident.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_mcp_incident PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    if(SIMXR_BUILD_AUTOMATION)
                        add_test(NAME linux_vulkan_history_bindings COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_history_bindings.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_runner>")
                        add_test(NAME linux_vulkan_ui_history_bindings COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_history_bindings.py" "$<TARGET_FILE:xr_probe_linux>" "$<TARGET_FILE:simxr_runner>" --ui)
                        set_tests_properties(linux_vulkan_history_bindings linux_vulkan_ui_history_bindings PROPERTIES
                            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                            TIMEOUT 90 SKIP_RETURN_CODE 77)
                    endif()
                    add_test(NAME linux_vulkan_ui_window COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_image_window.py" "$<TARGET_FILE:xr_probe_linux>" --ui)
                    add_test(NAME linux_vulkan_mcp_ui_incident COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_mcp_incident.py" "$<TARGET_FILE:xr_probe_linux>" --ui)
                    set_tests_properties(linux_vulkan_ui_window linux_vulkan_mcp_ui_incident PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_filtered_capture COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_filtered_capture.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_filtered_capture PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 60 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_mcp_screenshot COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_mcp_screenshot.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_mcp_screenshot PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 60 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_capture_bundle COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_depth.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_capture_bundle PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1;SIMXR_PROBE_BUNDLE=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    foreach(depth_format d32 d16 d24s8 d32s8)
                        add_test(NAME linux_vulkan_msaa_depth_${depth_format} COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_depth.py" "$<TARGET_FILE:xr_probe_linux>")
                        set_tests_properties(linux_vulkan_msaa_depth_${depth_format} PROPERTIES
                            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1;SIMXR_PROBE_MSAA=1;SIMXR_PROBE_DEPTH_MSAA=1;SIMXR_PROBE_DEPTH_FORMAT=${depth_format}"
                            TIMEOUT 90 SKIP_RETURN_CODE 77)
                        set(depth_test linux_vulkan_control_depth_${depth_format})
                        if(depth_format STREQUAL "d32")
                            set(depth_test linux_vulkan_control_depth)
                        endif()
                        add_test(NAME ${depth_test} COMMAND "${Python3_EXECUTABLE}"
                            "${PROJECT_SOURCE_DIR}/tests/control/test_native_depth.py" "$<TARGET_FILE:xr_probe_linux>")
                        set_tests_properties(${depth_test} PROPERTIES
                            ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1;SIMXR_PROBE_DEPTH_FORMAT=${depth_format}"
                            TIMEOUT 90 SKIP_RETURN_CODE 77)
                    endforeach()
                    add_test(NAME linux_vulkan_continuous_previews COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_continuous_previews.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_continuous_previews PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json"
                        TIMEOUT 120)
                    add_test(NAME linux_vulkan_msaa_sources COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_quad.py" "$<TARGET_FILE:xr_probe_linux>")
                    add_test(NAME linux_vulkan_msaa_reprojection COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_reprojection.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_msaa_sources linux_vulkan_msaa_reprojection PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_PROBE_MSAA=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_mip_reprojection COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_reprojection.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_mip_reprojection PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_PROBE_MIPS=1;SIMXR_PROBE_MIP_MINIFY=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_mip_sources COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_quad.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_mip_sources PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1;SIMXR_PROBE_MIPS=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_control_quad COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_quad.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_vulkan_control_quad PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_control_foveated COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_quad.py" "$<TARGET_FILE:xr_probe_linux>" --foveated)
                    set_tests_properties(linux_vulkan_control_foveated PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_control_foveated_raw COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_quad.py" "$<TARGET_FILE:xr_probe_linux>" --foveated --raw-views)
                    set_tests_properties(linux_vulkan_control_foveated_raw PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                    add_test(NAME linux_vulkan_control_images COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_native_images.py" "$<TARGET_FILE:xr_probe_linux>"
                        "$<TARGET_FILE:vulkan_control_image_probe>")
                    set_tests_properties(linux_vulkan_control_images PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 90 SKIP_RETURN_CODE 77)
                endif()
                if(SIMXR_MCP_TEST_PYTHON)
                    add_test(NAME linux_mcp_debug_stdio COMMAND "${SIMXR_MCP_TEST_PYTHON}"
                        "${PROJECT_SOURCE_DIR}/tests/control/test_mcp_stdio_control.py" "$<TARGET_FILE:xr_probe_linux>")
                    set_tests_properties(linux_mcp_debug_stdio PROPERTIES
                        ENVIRONMENT "XR_RUNTIME_JSON=${SIMXR_OUTPUT_DIRECTORY}/openxr_simulator.json;SIMXR_DETERMINISTIC=1"
                        TIMEOUT 60 SKIP_RETURN_CODE 77)
                endif()
            endif()
        endif()
    else()
        message(STATUS "OpenXR loader not found; native loader probes are unavailable")
    endif()
endif()

if(SIMXR_BUILD_AUTOMATION)
    include(cmake/SimxrAutomation.cmake)
endif()

if(SIMXR_BUILD_COMPANION)
    include(cmake/SimxrCompanion.cmake)
endif()

install(FILES include/simxr/game_bridge.h DESTINATION include/simxr)

if(TARGET openxr_simulator AND NOT WIN32 AND BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_executable(game_bridge_probe probes/GameBridgeProbe.cpp)
    simxr_configure_target(game_bridge_probe)
    simxr_set_output_directory(game_bridge_probe)
    target_link_libraries(game_bridge_probe PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    add_test(NAME native_game_bridge COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/control/test_game_bridge_native.py" "$<TARGET_FILE:game_bridge_probe>"
        "$<TARGET_FILE:openxr_simulator>")
    set_tests_properties(native_game_bridge PROPERTIES TIMEOUT 20)
    add_test(NAME native_game_example COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/control/test_game_example.py" "$<TARGET_FILE:openxr_simulator>")
    set_tests_properties(native_game_example PROPERTIES TIMEOUT 20)
endif()

include(cmake/SimxrPackage.cmake)
