# Runtime asset deployment.
#
# Development builds (RV_DEV_ASSET_ROOT=ON) read assets straight from <source>/assets through the
# RV_SOURCE_ASSET_DIR compile definition, so shader hot-reload edits the versioned files.
# Release-style builds copy <source>/assets (minus the staging area) next to the executable.
#
#   rv_deploy_assets(<executable-target>)

function(rv_deploy_assets target)
    if(RV_DEV_ASSET_ROOT)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            -DSRC=${PROJECT_SOURCE_DIR}/assets
            -DDST=$<TARGET_FILE_DIR:${target}>/assets
            -P "${PROJECT_SOURCE_DIR}/cmake/CopyAssets.cmake"
        COMMENT "Deploying runtime assets next to ${target}"
        VERBATIM)
endfunction()
