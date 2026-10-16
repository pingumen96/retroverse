# RmlUi backends (vendored from RmlUi 6.3, see README.md): the SDL3 platform backend (system interface, SDL event
# translation, text input) and the OpenGL 3 renderer, built on our glad loader (third_party/glad) through
# RMLUI_GL3_CUSTOM_LOADER, so that no second OpenGL loader and no duplicate glad symbols exist.
# Added by src/ui/CMakeLists.txt (rv_ui is its only user).

add_library(rv_rmlui_backends STATIC
    RmlUi_Platform_SDL.cpp
    RmlUi_Renderer_GL3.cpp
)
add_library(rv::rmlui_backends ALIAS rv_rmlui_backends)
target_include_directories(rv_rmlui_backends PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(rv_rmlui_backends PUBLIC cxx_std_20)
target_compile_definitions(rv_rmlui_backends
    PUBLIC RMLUI_SDL_VERSION_MAJOR=3
    PRIVATE "RMLUI_GL3_CUSTOM_LOADER=<glad/gl.h>" NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(rv_rmlui_backends PUBLIC RmlUi::Core SDL3::SDL3 PRIVATE rv::glad)
if(MSVC)
    # Third-party code: compiled at the project's warning level, warnings as errors, with the one warning the upstream
    # sources trigger silenced here instead of patched (README.md): C4100, the parameter of RmlGL3::Initialize that is
    # unused with RMLUI_GL3_CUSTOM_LOADER.
    target_compile_options(rv_rmlui_backends PRIVATE /W4 /wd4100 /permissive- /utf-8 /Zc:preprocessor
                                                     /Zc:__cplusplus /EHsc)
    if(RV_WARNINGS_AS_ERRORS)
        target_compile_options(rv_rmlui_backends PRIVATE /WX)
    endif()
endif()
set_target_properties(rv_rmlui_backends PROPERTIES FOLDER "third_party")
