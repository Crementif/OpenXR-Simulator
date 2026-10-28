include(FetchContent)

FetchContent_Declare(simxr_httplib
    URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.58.0
    URL_HASH SHA256=31932ce8b33f2905472a987dc1984b9d8a7d338083a066021349587cc5f1cfea
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
include(cmake/SimxrJson.cmake)
FetchContent_GetProperties(simxr_httplib)
if(NOT simxr_httplib_POPULATED)
    FetchContent_Populate(simxr_httplib)
endif()


add_library(simxr_mcp STATIC
    src/app/mcp/McpArtifactCalls.cpp
    src/app/mcp/McpConfiguration.cpp
    src/app/mcp/McpDebug.cpp
    src/app/mcp/McpDiagnostics.cpp
    src/app/mcp/McpGame.cpp
    src/app/mcp/McpFlickerMonitor.cpp
    src/app/mcp/McpFlickerCoverage.cpp
    src/app/mcp/McpHistory.cpp
    src/app/mcp/McpLogs.cpp
    src/app/mcp/McpIncident.cpp
    src/app/mcp/McpIncidentAnalysis.cpp
    src/app/mcp/McpIncidentSave.cpp
    src/app/mcp/McpIncidentIdentifier.cpp
    src/app/mcp/McpCompatibility.cpp
    src/app/mcp/McpImage.cpp
    src/app/mcp/McpImageCache.cpp
    src/app/mcp/McpImageEncode.cpp
    src/app/mcp/McpImageSave.cpp
    src/app/mcp/McpInspectionJson.cpp
    src/app/mcp/McpScreenshot.cpp
    src/app/mcp/McpSequence.cpp
    src/app/mcp/McpSequenceCapture.cpp
    src/app/mcp/McpSequenceJobs.cpp
    src/app/mcp/McpStereoAnalysis.cpp
    src/app/mcp/StereoSurface.cpp
    src/app/mcp/StereoImageOps.cpp
    src/app/mcp/StereoConsistency.cpp
    src/app/mcp/StereoConsistencyImage.cpp
    src/app/mcp/McpTransientDebug.cpp
    src/app/mcp/McpVideoSave.cpp
    src/app/mcp/McpSimulation.cpp
    src/app/mcp/McpSimulationCommands.cpp
    src/app/mcp/McpServer.cpp
    src/app/mcp/McpSocket.cpp)
if(WIN32)
    target_sources(simxr_mcp PRIVATE src/app/mcp/windows/McpCredentials.cpp src/app/mcp/windows/McpLogFile.cpp)
else()
    target_sources(simxr_mcp PRIVATE src/app/mcp/linux/McpCredentials.cpp src/app/mcp/linux/McpLogFile.cpp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(simxr_mcp PRIVATE src/app/mcp/linux/McpInstance.cpp)
    target_compile_definitions(simxr_mcp PUBLIC SIMXR_MCP_DISCOVERY)
elseif(WIN32)
    target_sources(simxr_mcp PRIVATE src/app/mcp/windows/McpInstance.cpp)
    target_compile_definitions(simxr_mcp PUBLIC SIMXR_MCP_DISCOVERY)
endif()
simxr_configure_target(simxr_mcp)
target_include_directories(simxr_mcp SYSTEM PUBLIC "${simxr_httplib_SOURCE_DIR}"
    "${simxr_json_SOURCE_DIR}/single_include")
target_include_directories(simxr_mcp SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")
target_include_directories(simxr_mcp PRIVATE "${SIMXR_VULKAN_INCLUDE_DIR}")
target_link_libraries(simxr_mcp PUBLIC Threads::Threads PRIVATE simxr_app_platform simxr_control simxr_graphics simxr_diagnostics)
if(WIN32)
    target_link_libraries(simxr_mcp PRIVATE ws2_32 bcrypt advapi32)
endif()
add_executable(simxr_mcp_connector
    src/app/mcp/ConnectorMain.cpp
    src/app/mcp/McpConnector.cpp)
if(WIN32)
    target_sources(simxr_mcp_connector PRIVATE src/app/mcp/windows/McpCancelablePost.cpp)
endif()
simxr_configure_target(simxr_mcp_connector)
simxr_set_output_directory(simxr_mcp_connector)
target_link_libraries(simxr_mcp_connector PRIVATE simxr_mcp)
if(TARGET simxr_install_use)
    target_link_libraries(simxr_mcp_connector PRIVATE simxr_install_use)
endif()
if(MINGW)
    target_link_options(simxr_mcp_connector PRIVATE -municode)
endif()
install(TARGETS simxr_mcp_connector RUNTIME DESTINATION bin)
install(FILES "${simxr_httplib_SOURCE_DIR}/LICENSE" DESTINATION share/licenses/simxr_mcp RENAME cpp-httplib.txt)
install(FILES "${simxr_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION share/licenses/simxr_mcp RENAME nlohmann-json.txt)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/stb/LICENSE" DESTINATION share/licenses/simxr_mcp RENAME stb-image-write.txt)
