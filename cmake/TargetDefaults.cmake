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
