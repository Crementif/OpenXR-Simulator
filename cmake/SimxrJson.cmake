include_guard(GLOBAL)
include(FetchContent)
FetchContent_Declare(simxr_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
    URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_GetProperties(simxr_json)
if(NOT simxr_json_POPULATED)
    FetchContent_Populate(simxr_json)
endif()
