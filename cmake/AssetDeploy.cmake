# Runtime asset deployment.
#
# Development builds (RV_DEV_ASSET_ROOT=ON) read assets straight from <source>/assets through the
# RV_SOURCE_ASSET_DIR compile definition, so shader hot-reload edits the versioned files.
# Release-style builds read <exe>/assets: the target rv_runtime_assets mirrors <source>/assets there on every build
# (cmake/CopyAssets.cmake: new and changed files copied, files gone from the source removed; the staging area and the
# golden screenshots left out), so an edit of the assets alone reaches the next build's runs without a relink.
# Every executable that reads them lives in CMAKE_RUNTIME_OUTPUT_DIRECTORY and depends on that target.
#
#   rv_deploy_assets(<executable-target>)

function(rv_deploy_assets target)
    if(RV_DEV_ASSET_ROOT)
        return()
    endif()
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        message(FATAL_ERROR "rv_deploy_assets: CMAKE_RUNTIME_OUTPUT_DIRECTORY must name the executables' directory")
    endif()
    if(NOT TARGET rv_runtime_assets)
        # A custom target is always out of date: the mirror runs on every build and copies only what changed.
        add_custom_target(rv_runtime_assets ALL
            COMMAND "${CMAKE_COMMAND}"
                -DSRC=${PROJECT_SOURCE_DIR}/assets
                -DDST=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets
                -P "${PROJECT_SOURCE_DIR}/cmake/CopyAssets.cmake"
            COMMENT "Mirroring runtime assets into ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets"
            VERBATIM)
    endif()
    add_dependencies(${target} rv_runtime_assets)
endfunction()
