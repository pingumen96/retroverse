# rv_input: device-neutral input codes, action mapping and resolution, scripted input for automated runs. GPU-free
# and SDL-free (the SDL event collector lives in rv_platform).

add_library(rv_input STATIC
    src/action.cpp
    src/action_map.cpp
    src/binding.cpp
    src/binding_labels.cpp
    src/codes.cpp
    src/input_script.cpp
    src/input_system.cpp
)
add_library(rv::input ALIAS rv_input)
rv_set_target_defaults(rv_input)
target_include_directories(rv_input PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_input PUBLIC rv_core)
rv_forbid_link(rv_input)
