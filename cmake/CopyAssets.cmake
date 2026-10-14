# Script mode helper: cmake -DSRC=<dir> -DDST=<dir> -P CopyAssets.cmake
# Copies the runtime asset tree, excluding the never-distributed staging area.

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "CopyAssets.cmake requires -DSRC and -DDST")
endif()

file(MAKE_DIRECTORY "${DST}")
file(COPY "${SRC}/" DESTINATION "${DST}"
    PATTERN "staging" EXCLUDE
    PATTERN ".gitkeep" EXCLUDE)
