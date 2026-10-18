# Architectural layering rules (specification §15.1, ADR-0008, ADR-0010 §1).
#
# GPU-free libraries (core, geometry, input, assets, scene, media, launcher, the catalog modules, the GPU-free parts of
# the GPU-side modules and of the tools) must stay testable without a window, an OpenGL context or a UI toolkit. Mark
# them with:
#
#   rv_forbid_link(<target> [EXTRA <library>...])
#
# At the end of the configure step every marked target is checked, transitively through first-party targets, and
# configuration fails if it reaches one of the GPU libraries below or one of the libraries its EXTRA list names
# (third-party targets such as ${RV_SQLITE_LIBRARIES} or first-party ones such as rv_esde): the catalog domain may not
# reach SQLite, pugixml or the Windows-only targets, each adapter may not reach the other's library (ADR-0008 §2), the
# world generator may not reach RmlUi, SQLite, pugixml, the video decoder or the ES-DE, database and Windows modules
# (ADR-0010 §1, src/world/forbidden_links.cmake). tests/cmake/test_layer_rules.cmake proves that a forbidden link
# fails the configure. Aliases match the targets they name on both sides: EXTRA rv::esde forbids a link to rv_esde, and the reverse.

set(RV_GPU_LIBRARIES
    SDL3::SDL3
    SDL3::SDL3-shared
    SDL3::SDL3-static
    rv::glad
    rv_glad
    imgui::imgui
    CACHE INTERNAL "Libraries that GPU-free targets must not link")

# Every target name a package exports for one library, for EXTRA lists.
set(RV_SQLITE_LIBRARIES
    unofficial::sqlite3::sqlite3
    CACHE INTERNAL "Target names of SQLite (vcpkg port sqlite3)")
set(RV_PUGIXML_LIBRARIES
    pugixml::pugixml
    pugixml::shared
    pugixml::static
    pugixml
    CACHE INTERNAL "Target names of pugixml")
set(RV_RMLUI_LIBRARIES
    RmlUi::RmlUi
    RmlUi::Core
    RmlUi::Debugger
    CACHE INTERNAL "Target names of RmlUi (vcpkg port rmlui)")

function(rv_forbid_link target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "EXTRA")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "rv_forbid_link(${target}): unexpected arguments '${arg_UNPARSED_ARGUMENTS}' (use EXTRA)")
    endif()
    # DEFER arguments are evaluated when the call runs, so bake the target name and the extra libraries in now.
    set(call "_rv_check_layer [[${target}]]")
    foreach(library IN LISTS arg_EXTRA)
        string(APPEND call " [[${library}]]")
    endforeach()
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL ${call})")
endfunction()

function(_rv_collect_links target out_var)
    set(result "")
    foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libs ${target} ${property})
        if(libs)
            foreach(lib IN LISTS libs)
                # Skip generator expressions; first-party targets never hide forbidden links behind them.
                if(NOT lib MATCHES "^\\$<")
                    list(APPEND result "${lib}")
                endif()
            endforeach()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES result)
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# _rv_check_layer(<root> [<extra forbidden library>...])
function(_rv_check_layer root)
    # An EXTRA entry given as an alias (rv::esde) also forbids the target it names. The check runs once every target
    # exists, so the aliases resolve here.
    set(extra "")
    foreach(library IN LISTS ARGN)
        list(APPEND extra "${library}")
        if(TARGET "${library}")
            get_target_property(aliased "${library}" ALIASED_TARGET)
            if(aliased)
                list(APPEND extra "${aliased}")
            endif()
        endif()
    endforeach()
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
            # An alias (rv::core, rv::esde) stands for the target it names.
            set(names "${lib}")
            set(real "${lib}")
            if(TARGET "${lib}")
                get_target_property(aliased "${lib}" ALIASED_TARGET)
                if(aliased)
                    set(real "${aliased}")
                    list(APPEND names "${aliased}")
                endif()
            endif()
            foreach(name IN LISTS names)
                if(name IN_LIST RV_GPU_LIBRARIES)
                    message(FATAL_ERROR
                        "Layer rule violated: GPU-free target '${root}' reaches '${lib}' through '${current}'.")
                endif()
                if(name IN_LIST extra)
                    message(FATAL_ERROR
                        "Layer rule violated: '${root}' must not reach '${lib}' (the EXTRA list of its "
                        "rv_forbid_link call), but it does through '${current}'.")
                endif()
            endforeach()
            if(TARGET "${real}" AND real MATCHES "^rv_")
                list(APPEND pending "${real}")
            endif()
        endforeach()
    endwhile()
endfunction()
