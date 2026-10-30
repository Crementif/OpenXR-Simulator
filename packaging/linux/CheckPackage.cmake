# Validate an extracted Linux package for the release baseline:
#   cmake -DSTAGE=<extracted directory> [-DVERSION=<x.y.z>] -P CheckPackage.cmake
cmake_minimum_required(VERSION 3.29)
include("${CMAKE_CURRENT_LIST_DIR}/Abi.cmake")

if(NOT IS_DIRECTORY "${STAGE}")
    message(FATAL_ERROR "Pass the extracted package directory as -DSTAGE=")
endif()
set(required
    README.txt install.sh launch-desktop.sh bin/openxr_simulator.json
    bin/simxr_companion bin/simxr_mcp_connector bin/simxr_runner
    bin/libopenxr_simulator.so bin/fonts/AdwaitaSans-Regular.ttf
    share/simxr/branding/openxr-simulator.png share/simxr/rigs/pose_sweep.luau
    include/simxr/game_bridge.h share/doc/simxr/BUILDING.md
    share/simxr/sdk/python/pyproject.toml share/simxr/sdk/python/simxr_control/client.py)
set(licenses
    simxr/LICENSE simxr_runner/Luau.txt simxr_companion/SDL3.txt simxr_companion/ImGui.txt
    simxr_companion/ImGuizmo.txt simxr_companion/NanoSVG.txt simxr_companion/ADWAITA-SANS-LICENSE.txt
    simxr_companion/curl-headers.txt simxr_mcp/cpp-httplib.txt simxr_mcp/nlohmann-json.txt
    simxr_mcp/stb-image-write.txt)
foreach(relative IN LISTS required)
    if(NOT EXISTS "${STAGE}/${relative}" OR IS_DIRECTORY "${STAGE}/${relative}")
        message(FATAL_ERROR "The package is incomplete: ${relative}")
    endif()
endforeach()
foreach(license IN LISTS licenses)
    file(SIZE "${STAGE}/share/licenses/${license}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "The package license is empty: ${license}")
    endif()
endforeach()
if(VERSION)
    file(STRINGS "${STAGE}/README.txt" title LIMIT_COUNT 1)
    if(NOT title STREQUAL "OpenXR Simulator ${VERSION}")
        message(FATAL_ERROR "The package README does not name version ${VERSION}")
    endif()
endif()
file(READ "${STAGE}/bin/openxr_simulator.json" manifest)
string(JSON library ERROR_VARIABLE error GET "${manifest}" runtime library_path)
if(NOT library STREQUAL "./libopenxr_simulator.so")
    message(FATAL_ERROR "The runtime manifest must point to its adjacent shared library")
endif()
foreach(relative install.sh launch-desktop.sh bin/simxr_companion bin/simxr_mcp_connector bin/simxr_runner)
    if(NOT IS_EXECUTABLE "${STAGE}/${relative}")
        message(FATAL_ERROR "The packaged file is not executable: ${relative}")
    endif()
endforeach()

file(GLOB_RECURSE entries LIST_DIRECTORIES TRUE "${STAGE}/*")
set(checked 0)
foreach(path IN LISTS entries)
    if(IS_SYMLINK "${path}")
        message(FATAL_ERROR "The package contains an unexpected symlink: ${path}")
    endif()
    if(NOT IS_DIRECTORY "${path}")
        file(READ "${path}" magic LIMIT 4 HEX)
        if(magic STREQUAL "7f454c46")
            simxr_abi_check_elf("${path}")
            math(EXPR checked "${checked} + 1")
        endif()
    endif()
endforeach()
message(STATUS "Checked ${checked} ELF files in ${STAGE}")
