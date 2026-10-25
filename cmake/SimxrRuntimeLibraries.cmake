option(SIMXR_STATIC_RUNTIME "Statically link Windows compiler runtime libraries" ${WIN32})
if(SIMXR_STATIC_RUNTIME)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(MINGW)
        # Includes libwinpthread as well as libgcc and libstdc++.
        add_link_options(-static)
    endif()
endif()
