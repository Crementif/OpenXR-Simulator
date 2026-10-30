# Check the release ABI gates against representative readelf output.
include("${CMAKE_CURRENT_LIST_DIR}/../../packaging/linux/Abi.cmake")

function(expect_accept kind text)
    cmake_language(CALL simxr_abi_${kind}_error error "${text}" fixture ${ARGN})
    if(error)
        message(FATAL_ERROR "Rejected compatible requirements: ${error}")
    endif()
endfunction()

function(expect_reject kind text expected)
    cmake_language(CALL simxr_abi_${kind}_error error "${text}" fixture ${ARGN})
    string(FIND "${error}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Expected an error naming ${expected}, got: '${error}'")
    endif()
endfunction()

set(current "Version needs section '.gnu.version_r' contains 3 entries:
  Name: GLIBC_2.39  Flags: none
  Name: GLIBCXX_3.4.33  Flags: none
  Name: CXXABI_1.3.15  Flags: none
  Name: GCC_14.0.0  Flags: none
  Name: GLIBC_ABI_DT_RELR  Flags: none
")
expect_accept(version "${current}\nVersion definition section '.gnu.version_d' contains 1 entry:\n  Name: GLIBC_2.99\n" FALSE)
expect_reject(version "${current}  Name: GLIBC_2.40\n" GLIBC_2.40 FALSE)
expect_reject(version "${current}  Name: GLIBCXX_3.4.34\n" GLIBCXX_3.4.34 FALSE)
expect_reject(version "${current}  Name: GLIBC_PRIVATE\n" GLIBC_PRIVATE FALSE)
expect_accept(version "No version information found in this file." FALSE)
expect_reject(version "No version information found in this file." "could not inspect" TRUE)

set(dynamic "0x0001 (NEEDED) Shared library: [libc.so.6]
0x0001 (NEEDED) Shared library: [libvulkan.so.1]
0x001d (RUNPATH) Library runpath: [$ORIGIN]
")
expect_accept(dynamic "${dynamic}")
expect_accept(dynamic "0x000f (RPATH) Library rpath: [\${ORIGIN}:\$ORIGIN]\n")
expect_reject(dynamic "${dynamic}0x0001 (NEEDED) Shared library: [libnew.so.1]\n" libnew.so.1)
expect_reject(dynamic "${dynamic}0x001d (RUNPATH) Library runpath: [/tmp/build]\n" /tmp/build)
expect_reject(dynamic "0x001d (RUNPATH) Library runpath: [\$ORIGIN:/usr/lib]\n" /usr/lib)
