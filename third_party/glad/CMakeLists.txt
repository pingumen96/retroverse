# glad2 OpenGL loader, generated and vendored (see README.md in this directory for the exact command).
add_library(rv_glad STATIC src/gl.c)
add_library(rv::glad ALIAS rv_glad)
target_include_directories(rv_glad SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(rv_glad PROPERTIES FOLDER "third_party" C_STANDARD 11)
