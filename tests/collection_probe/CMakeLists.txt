# rv_collection_probe: the driver of the collection's texture arrays in the GPU smoke tests (I3 step 6; ADR-0011 §4,
# §6): a wall of test items drawn through gl::CollectionArrays with the host and renderer of rv_app (two arrays per
# class for 2,049 items, the arrays grown mid-run, the RGBA8 fallback with --no-texture-compression), whose screenshots
# and run reports tests/smoke/test_collection_arrays.cpp checks. probe_layout.hpp, GPU-free, is shared with that test.
# Built with the tests; it opens a window (hidden in every test run) like retroverse.exe.

add_executable(rv_collection_probe main.cpp probe_app.cpp)
rv_set_target_defaults(rv_collection_probe)
target_include_directories(rv_collection_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rv_collection_probe PRIVATE rv_app)
# Release runs read the assets deployed next to the executables.
rv_deploy_assets(rv_collection_probe)
