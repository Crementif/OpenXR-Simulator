# Windows cannot overwrite or delete a running executable but can rename it. Moves FILE aside so the
# linker can write a new one, and deletes earlier copies once no process runs them.
file(GLOB retired "${FILE}.*.retired")
if(EXISTS "${FILE}")
    string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
    file(RENAME "${FILE}" "${FILE}.${suffix}.retired" RESULT renamed)
    if(renamed STREQUAL "0")
        list(APPEND retired "${FILE}.${suffix}.retired")
    endif()
endif()
foreach(path IN LISTS retired)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${path}" RESULT_VARIABLE ignored ERROR_QUIET)
endforeach()
