# Add release notes, the optional Python wheel and checksums to a directory of built packages:
#   cmake -DVERSION=<x.y.z> -DDIRECTORY=<packages> [-DWHEELS=<wheel directory>] -P PrepareRelease.cmake
if(NOT VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Pass a three-component -DVERSION=")
endif()
if(NOT IS_DIRECTORY "${DIRECTORY}")
    message(FATAL_ERROR "Pass the package directory as -DDIRECTORY=")
endif()
set(prefix "openxr-simulator-${VERSION}")
set(packages
    "${prefix}-windows-x64-setup.exe" "${prefix}-windows-x64.zip"
    "${prefix}-linux-x64.tar.gz" "${prefix}-source.tar.gz")
foreach(name IN LISTS packages)
    if(IS_SYMLINK "${DIRECTORY}/${name}" OR NOT EXISTS "${DIRECTORY}/${name}" OR IS_DIRECTORY "${DIRECTORY}/${name}")
        message(FATAL_ERROR "Missing regular package: ${name}")
    endif()
endforeach()
foreach(name RELEASE_NOTES.md SHA256SUMS)
    if(EXISTS "${DIRECTORY}/${name}" OR IS_SYMLINK "${DIRECTORY}/${name}")
        message(FATAL_ERROR "Refusing to replace ${name}")
    endif()
endforeach()

set(wheel_note "")
if(WHEELS)
    set(wheel "simxr_control-${VERSION}-py3-none-any.whl")
    if(IS_SYMLINK "${WHEELS}/${wheel}" OR NOT EXISTS "${WHEELS}/${wheel}")
        message(FATAL_ERROR "Missing Python distribution: ${WHEELS}/${wheel}")
    endif()
    file(COPY_FILE "${WHEELS}/${wheel}" "${DIRECTORY}/${wheel}")
    list(APPEND packages "${wheel}")
    set(wheel_note "\nAn optional Python SDK wheel is included.\n")
endif()

file(WRITE "${DIRECTORY}/RELEASE_NOTES.md" "# OpenXR Simulator ${VERSION} preview

Launch OpenXR applications, control simulated devices and inspect stereo output without a headset.

- Windows: run `${prefix}-windows-x64-setup.exe`, or extract `${prefix}-windows-x64.zip` and launch `bin/simxr_companion.exe`.
- Linux (Ubuntu 24.04, Fedora 40 or newer): extract `${prefix}-linux-x64.tar.gz` and run `./install.sh` or `bin/simxr_companion`.
- Source: extract `${prefix}-source.tar.gz` and follow `docs/BUILDING.md`.

Both platforms require a Vulkan driver and loader. Choose **Add application**, select an executable and launch it. For MCP, copy the configuration from **Preferences → MCP**. See the package README for installation and removal instructions.
${wheel_note}")
list(APPEND packages RELEASE_NOTES.md)

set(sums "")
foreach(name IN LISTS packages)
    file(SHA256 "${DIRECTORY}/${name}" hash)
    string(APPEND sums "${hash}  ${name}\n")
endforeach()
file(WRITE "${DIRECTORY}/SHA256SUMS" "${sums}")
message(STATUS "Prepared release notes and checksums for ${VERSION} in ${DIRECTORY}")
