# rv_app: composition root (application host, main loop, scenes, screenshots). Links every module.

add_library(rv_app STATIC
    src/app.cpp
)
add_library(rv::app ALIAS rv_app)
rv_set_target_defaults(rv_app)
target_include_directories(rv_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app PUBLIC
    rv_core
    rv_geometry
    rv_input
    rv_assets
    rv_scene
    rv_platform
    rv_render)

if(RV_DEV_UI)
    target_link_libraries(rv_app PUBLIC rv_devui)
    target_compile_definitions(rv_app PUBLIC RV_DEV_UI=1)
endif()
if(RV_GL_DEBUG)
    target_compile_definitions(rv_app PUBLIC RV_GL_DEBUG=1)
endif()
