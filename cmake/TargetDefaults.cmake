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
