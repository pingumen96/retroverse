# rv_core: foundation utilities shared by every module (errors, logging, paths, math, I/O, JSON, CLI; since I2 SHA-256,
# file-system case folding, UTF-8, UTC times, relative paths of untrusted files, the tree walker and the system probe's
# port; since I3, skeletons naming their steps: seeds, the random generator, canonical JSON, the worker pool, rotating
# backups, CPU timers). GPU-free and window-system-free.

configure_file(include/rv/core/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/rv/core/version.hpp" @ONLY)

add_library(rv_core STATIC
    src/aabb.cpp
    src/arg_parser.cpp
    src/backups.cpp
    src/canonical_json.cpp
    src/case_fold.cpp
    src/cpu_timers.cpp
    src/expected.cpp
    src/file_io.cpp
    src/file_watch_set.cpp
    src/frame_clock.cpp
    src/json.cpp
    src/log.cpp
    src/main_thread.cpp
    src/math.cpp
    src/paths.cpp
    src/random.cpp
    src/relative_path.cpp
    src/seed.cpp
    src/sha256.cpp
    src/system_error.cpp
    src/system_probe.cpp
    src/text.cpp
    src/time.cpp
    src/tree_walk.cpp
    src/utf8.cpp
    src/version.cpp
    src/worker_pool.cpp
)
add_library(rv::core ALIAS rv_core)
rv_set_target_defaults(rv_core)

target_include_directories(rv_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(rv_core PUBLIC
    glm::glm
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    tl::expected)

if(RV_DEV_ASSET_ROOT)
    target_compile_definitions(rv_core PRIVATE RV_SOURCE_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()

if(RV_ENABLE_TRACY)
    target_link_libraries(rv_core PUBLIC Tracy::TracyClient)
endif()

rv_forbid_link(rv_core)
