# Developer tools.
#  - retroverse-asset-preview (style guide §19.3): its GPU-free core is built in every configuration, so that rv_tests
#    covers it; the executable needs the Dear ImGui developer UI (RV_DEV_UI) and is skipped without it.
#  - retroverse-screenshot-diff (style guide §26.1): compares a screenshot with its golden image within a tolerance;
#    GPU-free, built in every configuration (the GPU smoke tests link its core).

add_subdirectory(screenshot-diff)
add_subdirectory(asset-preview)
