set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# Packages ship the OpenXR loader as a DLL with jsoncpp linked into it
if(PORT STREQUAL "openxr-loader")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
