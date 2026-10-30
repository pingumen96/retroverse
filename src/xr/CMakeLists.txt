# rv_xr: VR through OpenXR (I10, ADR-0017 §3), between rv_render and rv_app.

# GPU-free part, always built so that rv_tests covers it in every configuration: poses and the tracking space composed
# with the world, the runtime's interface and the session's state machine and frame loop over it, the fake runtime,
# the frame statistics (missed refreshes), the swapchain format's choice; since step 3 the movement in VR (turns, the
# teleport's arc, the head through the collision world) and xr.json; since step 4 the controllers (the virtual pad, the
# pointing hand) and xr_bindings.json; since step 5 where the UI's panel stands and the ray on it; since step 7 the
# eyes' dynamic render scale. No OpenGL, no SDL, no OpenXR loader.
add_library(rv_xr_model STATIC
    src/controllers.cpp
    src/fake_runtime.cpp
    src/frame_stats.cpp
    src/locomotion.cpp
    src/panels.cpp
    src/pose.cpp
    src/render_scale.cpp
    src/session.cpp
    src/swapchain_format.cpp
    src/xr_bindings.cpp
    src/xr_settings.cpp
)
add_library(rv::xr_model ALIAS rv_xr_model)
rv_set_target_defaults(rv_xr_model)
target_include_directories(rv_xr_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_xr_model PUBLIC rv_core rv_scene glm::glm)
rv_forbid_link(rv_xr_model EXTRA ${RV_OPENXR_LIBRARIES})

# The GPU side: the fake runtime's swapchain images as GL textures, and with RV_ENABLE_XR the system's OpenXR runtime
# (the Khronos loader, XR_KHR_opengl_enable on rv_platform's context; RV_XR_OPENXR is then defined for its users).
add_library(rv_xr STATIC
    src/gl_images.cpp
)
add_library(rv::xr ALIAS rv_xr)
rv_set_target_defaults(rv_xr)
target_include_directories(rv_xr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_xr PUBLIC rv_xr_model rv_core rv_render)
if(RV_ENABLE_XR)
    target_sources(rv_xr PRIVATE src/openxr_runtime.cpp)
    # opengl32: wglGetCurrentDC and wglGetCurrentContext, the handles XR_KHR_opengl_enable takes.
    target_link_libraries(rv_xr PRIVATE OpenXR::openxr_loader opengl32)
    target_compile_definitions(rv_xr PUBLIC RV_XR_OPENXR=1)
endif()
