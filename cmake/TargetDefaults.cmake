# Common compile settings applied to every first-party target.
#
#   rv_set_target_defaults(<target>)
#
# Third-party include directories coming from imported targets are treated as SYSTEM by CMake and
# compiled with /external:W0, so /W4 /WX only applies to Retroverse code.

function(rv_set_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:preprocessor
            /Zc:__cplusplus
            /Zc:inline
            /EHsc
            /wd4201 # nonstandard extension: nameless struct/union (glm)
        )
        if(RV_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS
        )
    endif()

    # glm: radians everywhere, experimental extensions (gtx) allowed, OpenGL clip space [-1, 1].
    target_compile_definitions(${target} PUBLIC GLM_ENABLE_EXPERIMENTAL GLM_FORCE_RADIANS)

    if(RV_ENABLE_TRACY)
        target_compile_definitions(${target} PUBLIC RV_ENABLE_TRACY=1)
    endif()

    set_target_properties(${target} PROPERTIES FOLDER "retroverse")

    # rv_apply_precompiled_headers() runs once every target and its links are known.
    set_property(GLOBAL APPEND PROPERTY RV_FIRST_PARTY_TARGETS ${target})
endfunction()

# Precompiled headers (RV_USE_PCH). Most of a translation unit's front-end time goes to the same headers: the standard
# library (<chrono> and <filesystem> above all), spdlog and fmt, nlohmann/json, glm and, in the tests, Catch2. Each
# first-party target precompiles them once and every one of its C++ files starts from that image. A target gets the
# third-party headers only when it reaches their package through its links (rv_core links glm, spdlog, nlohmann_json and
# tl::expected PUBLIC), so the image is built with the same include directories and definitions as its files.
#
#   rv_apply_precompiled_headers()   # at the end of the root CMakeLists.txt

set(RV_PCH_STD_HEADERS
    <algorithm> <array> <atomic> <chrono> <cmath> <cstddef> <cstdint> <filesystem> <functional> <limits> <map>
    <memory> <mutex> <optional> <set> <span> <string> <string_view> <system_error> <thread> <tuple> <utility>
    <variant> <vector>)
set(RV_PCH_CORE_HEADERS
    <fmt/format.h> <glm/glm.hpp> <nlohmann/json.hpp> <spdlog/spdlog.h> <tl/expected.hpp>)
set(RV_PCH_CATCH2_HEADERS
    <catch2/catch_approx.hpp> <catch2/catch_test_macros.hpp>)

# Collects into <out_var> every target reachable from <target> through LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES,
# aliases resolved. Entries that are generator expressions (other than $<LINK_ONLY:...>) or plain library names are
# skipped.
function(_rv_reachable_targets target out_var)
    set(seen "")
    set(queue ${target})
    while(queue)
        list(POP_FRONT queue current)
        if(NOT TARGET ${current})
            continue()
        endif()
        get_target_property(aliased ${current} ALIASED_TARGET)
        if(aliased)
            set(current ${aliased})
        endif()
        if(current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${current})
        set(links "")
        get_target_property(type ${current} TYPE)
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(direct ${current} LINK_LIBRARIES)
            if(direct)
                list(APPEND links ${direct})
            endif()
        endif()
        get_target_property(iface ${current} INTERFACE_LINK_LIBRARIES)
        if(iface)
            list(APPEND links ${iface})
        endif()
        foreach(link IN LISTS links)
            if(link MATCHES "^\\$<LINK_ONLY:(.+)>$")
                set(link "${CMAKE_MATCH_1}")
            endif()
            if(NOT link MATCHES "^\\$<" AND TARGET ${link})
                list(APPEND queue ${link})
            endif()
        endforeach()
    endwhile()
    set(${out_var} ${seen} PARENT_SCOPE)
endfunction()

function(rv_apply_precompiled_headers)
    if(NOT RV_USE_PCH)
        return()
    endif()
    get_property(targets GLOBAL PROPERTY RV_FIRST_PARTY_TARGETS)
    foreach(target IN LISTS targets)
        _rv_reachable_targets(${target} reached)
        set(headers ${RV_PCH_STD_HEADERS})
        if(rv_core IN_LIST reached)
            list(APPEND headers ${RV_PCH_CORE_HEADERS})
        endif()
        if(Catch2::Catch2 IN_LIST reached)
            list(APPEND headers ${RV_PCH_CATCH2_HEADERS})
        endif()
        # First-party targets hold C++ sources only (glad, the one C library, is not one of them).
        target_precompile_headers(${target} PRIVATE ${headers})
    endforeach()
endfunction()

# Declares an executable long-path aware (ADR-0008 §1): cmake/long_path_aware.manifest, a .manifest source, is merged
# into the manifest the linker generates (its trustInfo kept), so that Win32 file functions accept paths longer than
# MAX_PATH where the system enables long paths (LongPathsEnabled). File-system walks still use \\?\ paths.
#
#   rv_enable_long_paths(<executable-target>)

function(rv_enable_long_paths target)
    if(MSVC)
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/long_path_aware.manifest")
    endif()
endfunction()

# Gives an executable the Retroverse icon (assets/branding/retroverse.ico, written by scripts/branding.py;
# docs/branding.md) as its first icon resource: Explorer shows it, and SDL registers the first icon resource as its
# window class's icon, so the window's title bar and taskbar button show it too. The resource script is generated in
# the build tree and names the icon by its absolute path, as rc.exe resolves a relative one from its working directory;
# OBJECT_DEPENDS recompiles it when the icon changes.
#
#   rv_set_windows_icon(<executable-target>)

function(rv_set_windows_icon target)
    if(WIN32)
        set(icon "${PROJECT_SOURCE_DIR}/assets/branding/retroverse.ico")
        set(script "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc")
        file(CONFIGURE OUTPUT "${script}" CONTENT "#pragma code_page(65001)\n1 ICON \"@icon@\"\n" @ONLY)
        set_source_files_properties("${script}" PROPERTIES OBJECT_DEPENDS "${icon}")
        target_sources(${target} PRIVATE "${script}")
    endif()
endfunction()
