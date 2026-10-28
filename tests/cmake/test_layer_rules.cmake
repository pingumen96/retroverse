# Run by ctest in CMake script mode: cmake/LayerRules.cmake fails the configure of a project in which rv_world (ADR-0010
# §1) or rv_audio (ADR-0016 §9) reaches a library its rules forbid. For each case a small project without languages,
# written under WORK_DIR, has the subject as the real one is marked (rv_forbid_link with
# src/world/forbidden_links.cmake's list, or RV_AUDIO_FORBIDDEN_LINKS) and stand-ins of the libraries under their real
# target names; the subject reaches the case's library through a first-party target in between, so the check is
# transitive. The project without a forbidden link must configure; every other must fail naming the rule.
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

# configure_case(<subject: rv_world | rv_audio> <name> <library or empty> <expected: pass | fail>)
function(configure_case subject name library expected)
    set(dir "${WORK_DIR}/${subject}_${name}")
    file(MAKE_DIRECTORY "${dir}/source")
    set(link "")
    if(NOT library STREQUAL "")
        set(link "target_link_libraries(${subject}_dependency INTERFACE ${library})")
    endif()
    if(subject STREQUAL "rv_world")
        set(mark "include([[${SOURCE_DIR}/src/world/forbidden_links.cmake]])
rv_forbid_link(rv_world EXTRA \${RV_WORLD_FORBIDDEN_LINKS})")
    else()
        set(mark "rv_forbid_link(rv_audio EXTRA \${RV_AUDIO_FORBIDDEN_LINKS})")
    endif()
    file(WRITE "${dir}/source/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.28)
project(layer_rules_${subject}_${name} LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH [[${SOURCE_DIR}/cmake]])
include(LayerRules)

# Stand-ins under the real target names: the GPU libraries, then those of the EXTRA lists.
add_library(SDL3::SDL3 INTERFACE IMPORTED)
add_library(rv_glad INTERFACE)
add_library(rv::glad ALIAS rv_glad)
add_library(imgui::imgui INTERFACE IMPORTED)
add_library(RmlUi::Core INTERFACE IMPORTED)
add_library(unofficial::sqlite3::sqlite3 INTERFACE IMPORTED)
add_library(pugixml::pugixml INTERFACE IMPORTED)
foreach(target IN ITEMS rv_media rv_esde rv_catalog_db rv_platform_win rv_platform rv_ffmpeg)
    add_library(\${target} INTERFACE)
endforeach()
add_library(rv::esde ALIAS rv_esde)
add_library(rv::ffmpeg ALIAS rv_ffmpeg)
# Allowed: a library no rule names.
add_library(fmt::fmt INTERFACE IMPORTED)

# The subject as its CMakeLists.txt marks it, reaching the case's library through a first-party target.
add_library(${subject}_dependency INTERFACE)
add_library(${subject} INTERFACE)
target_link_libraries(${subject} INTERFACE ${subject}_dependency)
${link}
${mark}
")
    set(arguments -S "${dir}/source" -B "${dir}/build" -G "${GENERATOR}")
    if(DEFINED MAKE_PROGRAM AND NOT MAKE_PROGRAM STREQUAL "")
        list(APPEND arguments "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${arguments}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expected STREQUAL "pass")
        if(NOT code EQUAL 0)
            fail("case '${subject}/${name}' (${library}) should configure, but failed:\n${err}")
        endif()
    else()
        if(code EQUAL 0)
            fail("case '${subject}/${name}': ${subject} reaching ${library} configured, the layer rule did not fail it")
        elseif(NOT err MATCHES "Layer rule violated: [^\n]*'${subject}'")
            fail("case '${subject}/${name}' (${library}) failed without naming the layer rule:\n${err}")
        endif()
    endif()
    set(failures ${failures} PARENT_SCOPE)
endfunction()

configure_case(rv_world clean "" pass)
configure_case(rv_world allowed fmt::fmt pass)
configure_case(rv_world sdl3 SDL3::SDL3 fail)
configure_case(rv_world glad rv::glad fail)
configure_case(rv_world imgui imgui::imgui fail)
configure_case(rv_world rmlui RmlUi::Core fail)
configure_case(rv_world sqlite unofficial::sqlite3::sqlite3 fail)
configure_case(rv_world pugixml pugixml::pugixml fail)
configure_case(rv_world media rv_media fail)
configure_case(rv_world esde rv::esde fail)
configure_case(rv_world catalog_db rv_catalog_db fail)
configure_case(rv_world platform_win rv_platform_win fail)
configure_case(rv_world platform rv_platform fail)

# rv_audio (ADR-0016 §9): FFmpeg stays rv_media's, and the mixer never reaches the video decoder.
configure_case(rv_audio clean "" pass)
configure_case(rv_audio allowed fmt::fmt pass)
configure_case(rv_audio ffmpeg rv::ffmpeg fail)
configure_case(rv_audio ffmpeg_target rv_ffmpeg fail)
configure_case(rv_audio media rv_media fail)
configure_case(rv_audio sdl3 SDL3::SDL3 fail)
configure_case(rv_audio rmlui RmlUi::Core fail)

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} layer rule case(s) failed")
endif()
message(STATUS "rv_world's and rv_audio's layer rules: every forbidden link fails the configure, the allowed ones pass")
