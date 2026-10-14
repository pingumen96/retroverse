# Developer tools. retroverse-asset-preview (style guide §19.3) is added in M0 step 13 and requires the
# Dear ImGui developer UI.

if(NOT RV_DEV_UI)
    message(STATUS "Developer tools need RV_DEV_UI=ON; skipping tools/")
    return()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/asset-preview/CMakeLists.txt")
    add_subdirectory(asset-preview)
endif()
