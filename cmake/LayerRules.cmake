# Architectural layering rules (specification §15.1).
#
# GPU-free libraries (core, geometry, input, assets, scene and, later, domain/catalog/world) must stay
# testable without a window, an OpenGL context or a UI toolkit. Mark them with:
#
#   rv_forbid_link(<target>)
#
# At the end of the configure step every marked target is checked, transitively through first-party
# targets, and configuration fails if it reaches one of the forbidden libraries below.

set(RV_GPU_LIBRARIES
    SDL3::SDL3
    SDL3::SDL3-shared
    SDL3::SDL3-static
    rv::glad
    rv_glad
    imgui::imgui
    CACHE INTERNAL "Libraries that GPU-free targets must not link")

function(rv_forbid_link target)
    # DEFER arguments are evaluated when the call runs, so bake the target name in now.
    cmake_language(EVAL CODE
        "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL _rv_check_layer [[${target}]])")
endfunction()

function(_rv_collect_links target out_var)
    set(result "")
    foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libs ${target} ${property})
        if(libs)
            foreach(lib IN LISTS libs)
                # Skip generator expressions; first-party targets never hide GPU links behind them.
                if(NOT lib MATCHES "^\\$<")
                    list(APPEND result "${lib}")
                endif()
            endforeach()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES result)
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(_rv_check_layer root)
    set(pending "${root}")
    set(visited "")
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST visited)
            continue()
        endif()
        list(APPEND visited "${current}")
        _rv_collect_links(${current} links)
        foreach(lib IN LISTS links)
            if(lib IN_LIST RV_GPU_LIBRARIES)
                message(FATAL_ERROR
                    "Layer rule violated: GPU-free target '${root}' reaches '${lib}' through '${current}'.")
            endif()
            if(TARGET ${lib} AND lib MATCHES "^rv_")
                list(APPEND pending "${lib}")
            endif()
        endforeach()
    endwhile()
endfunction()
