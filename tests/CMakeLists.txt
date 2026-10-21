# Tests.
#   tests/unit          GPU-free Catch2 tests (label "unit"), executable rv_tests (no SDL, no OpenGL); tests tagged
#                       [.real-library] read the real ES-DE installation and run only through scripts/real-library.ps1
#   tests/platform      SDL platform layer tests (label "unit"), executable rv_platform_tests (SDL, no OpenGL)
#   tests/support       shared helpers: temporary directories, log capture, fixtures materialized under the build tree,
#                       a program stub, a child process runner with stdout and stderr apart, a fake system probe,
#                       junctions and folders that cannot be listed
#   tests/fixtures      the fixtures' source trees (tests/support/fixture.hpp materializes them)
#   tests/fake_emulator rv_fake_emulator, the stand-in emulator the launch tests start
#   tests/collection_probe rv_collection_probe, the collection's texture arrays drawn for the GPU smoke tests
#   tests/ui_probe      rv_ui_probe, the product UI's screens from fixed models for the GPU smoke tests (the directory)
#   tests/input_scripts --input-script files of the verification runs, also played by tests/unit/integration
#   tests/smoke         GPU smoke tests (label "gpu"), executable rv_smoke_tests: runs retroverse.exe and the asset
#                       preview frame-limited and checks their run reports and screenshots; excluded from the default
#                       test presets (msvc-x64-debug-gpu and msvc-x64-release-gpu run them)
#   tests/cmake         tests run by ctest in CMake script mode (label "unit"): the Release asset deployment
#                       (cmake/CopyAssets.cmake) and retroverse-cli as a process (exit codes, stdout and stderr apart)

add_subdirectory(fake_emulator)
add_subdirectory(collection_probe)
add_subdirectory(ui_probe)
add_subdirectory(support)
add_subdirectory(unit)
add_subdirectory(platform)
add_subdirectory(smoke)
add_subdirectory(cmake)
