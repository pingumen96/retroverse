# Script mode helper: cmake -DSRC=<dir> -DDST=<dir> -P CopyAssets.cmake
# Copies the runtime asset tree, excluding the never-distributed staging area and the golden screenshots of the visual
# tests (style_tests/screenshots: reference images and their metadata, which the application never reads).

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "CopyAssets.cmake requires -DSRC and -DDST")
endif()

file(MAKE_DIRECTORY "${DST}")
file(COPY "${SRC}/" DESTINATION "${DST}"
    PATTERN "staging" EXCLUDE
    REGEX "/style_tests/screenshots$" EXCLUDE
    PATTERN ".gitkeep" EXCLUDE)
