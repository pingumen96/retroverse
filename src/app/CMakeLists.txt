# rv_app: composition root (application host, main loop, scenes, screenshots). Links every module.

# GPU-free part of the application layer: command line options and the exit-code policy. Kept apart from rv_app so that
# rv_tests can unit-test it without SDL or OpenGL.
add_library(rv_app_cli STATIC
    src/command_line.cpp
    src/exit_code.cpp
)
add_library(rv::app_cli ALIAS rv_app_cli)
rv_set_target_defaults(rv_app_cli)
target_include_directories(rv_app_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app_cli PUBLIC rv_core)
rv_forbid_link(rv_app_cli)

add_library(rv_app STATIC
    src/app.cpp
    src/app_host.cpp
    src/app_logic.cpp
    src/retroverse_app.cpp
    src/triangle_demo.cpp
)
add_library(rv::app ALIAS rv_app)
rv_set_target_defaults(rv_app)
target_include_directories(rv_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app PUBLIC
    rv_app_cli
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

if(RV_BUILD_TESTS)
    # rv_tests (GPU-free, defined later in tests/unit) unit-tests rv_app_cli. tests/ is configured after src/, so the
    # link is added once the whole project has been read; rv_forbid_link(rv_tests) still validates the result.
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
        CALL target_link_libraries rv_tests PRIVATE rv_app_cli)
endif()
