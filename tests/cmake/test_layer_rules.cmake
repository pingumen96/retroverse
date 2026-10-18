# Run by ctest in CMake script mode: cmake/LayerRules.cmake fails the configure of a project in which rv_world reaches a
# library its rules forbid (ADR-0010 §1). For each case a small project without languages, written under WORK_DIR, has
# rv_world as the real one is marked (rv_forbid_link with src/world/forbidden_links.cmake's list) and stand-ins of
# the libraries under their real target names; rv_world reaches the case's library through a first-party target in
# between, so the check is transitive. The project without a forbidden link must configure; every other must fail
# naming the rule.
#
# Inputs (-D): SOURCE_DIR (the source tree), WORK_DIR (scratch, recreated), GENERATOR and MAKE_PROGRAM (those of the
# build; a project without languages needs no compiler).

foreach(variable IN ITEMS SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "test_layer_rules.cmake: -D${variable}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(failures 0)
macro(fail message)
    message(SEND_ERROR "FAIL: ${message}")
    math(EXPR failures "${failures} + 1")
endmacro()

# configure_case(<name> <library or empty> <expected: pass | fail>)
function(configure_case name library expected)
    set(dir "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${dir}/source")
    set(link "")
    if(NOT library STREQUAL "")
        set(link "target_link_libraries(rv_world_dependency INTERFACE ${library})")
    endif()
    file(WRITE "${dir}/source/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.28)
project(layer_rules_${name} LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[${SOURCE_DIR}/cmake]])
include(LayerRules)

# Stand-ins under the real target names: the GPU libraries, then those of rv_world's EXTRA list.
add_library(SDL3::SDL3 INTERFACE IMPORTED)
add_library(rv_glad INTERFACE)
add_library(rv::glad ALIAS rv_glad)
add_library(imgui::imgui INTERFACE IMPORTED)
add_library(RmlUi::Core INTERFACE IMPORTED)
add_library(unofficial::sqlite3::sqlite3 INTERFACE IMPORTED)
add_library(pugixml::pugixml INTERFACE IMPORTED)
foreach(target IN ITEMS rv_media rv_esde rv_catalog_db rv_platform_win rv_platform)
    add_library(\${target} INTERFACE)
endforeach()
add_library(rv::esde ALIAS rv_esde)
# Allowed: a library no rule names.
add_library(fmt::fmt INTERFACE IMPORTED)

# rv_world as src/world/CMakeLists.txt marks it, reaching the case's library through a first-party target.
add_library(rv_world_dependency INTERFACE)
add_library(rv_world INTERFACE)
target_link_libraries(rv_world INTERFACE rv_world_dependency)
${link}
include([[${SOURCE_DIR}/src/world/forbidden_links.cmake]])
rv_forbid_link(rv_world EXTRA \${RV_WORLD_FORBIDDEN_LINKS})
")
    set(arguments -S "${dir}/source" -B "${dir}/build" -G "${GENERATOR}")
    if(DEFINED MAKE_PROGRAM AND NOT MAKE_PROGRAM STREQUAL "")
        list(APPEND arguments "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${arguments}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expected STREQUAL "pass")
        if(NOT code EQUAL 0)
            fail("case '${name}' (${library}) should configure, but failed:\n${err}")
        endif()
    else()
        if(code EQUAL 0)
            fail("case '${name}': rv_world reaching ${library} configured, the layer rule did not fail it")
        elseif(NOT err MATCHES "Layer rule violated: [^\n]*'rv_world'")
            fail("case '${name}' (${library}) failed without naming the layer rule:\n${err}")
        endif()
    endif()
    set(failures ${failures} PARENT_SCOPE)
endfunction()

configure_case(clean "" pass)
configure_case(allowed fmt::fmt pass)
configure_case(sdl3 SDL3::SDL3 fail)
configure_case(glad rv::glad fail)
configure_case(imgui imgui::imgui fail)
configure_case(rmlui RmlUi::Core fail)
configure_case(sqlite unofficial::sqlite3::sqlite3 fail)
configure_case(pugixml pugixml::pugixml fail)
configure_case(media rv_media fail)
configure_case(esde rv::esde fail)
configure_case(catalog_db rv_catalog_db fail)
configure_case(platform_win rv_platform_win fail)
configure_case(platform rv_platform fail)

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} layer rule case(s) failed")
endif()
message(STATUS "rv_world's layer rules: every forbidden link fails the configure, the allowed ones pass")
