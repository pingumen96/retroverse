# Script mode helper: cmake -DSRC=<dir> -DDST=<dir> -P CopyAssets.cmake
# Mirrors the runtime asset tree SRC into DST: copies the files whose content is new or changed (compared byte for
# byte, not by timestamp: file(COPY) would skip an edit made within a second of the previous one) and removes from DST
# whatever is not deployed (files deleted from SRC, or left by an older deployment), so DST holds exactly the deployed
# files. Never deployed: the never-distributed staging area, the golden screenshots of the visual tests
# (style_tests/screenshots: reference images and their metadata, which the application never reads) and .gitkeep
# placeholders. tests/cmake/test_copy_assets.cmake checks it.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "CopyAssets.cmake requires -DSRC and -DDST")
endif()
if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "CopyAssets.cmake: no asset directory at ${SRC}")
endif()

# Paths relative to SRC that are never deployed.
set(not_deployed "^(staging|style_tests/screenshots)(/|$)|(^|/)\\.gitkeep$")

# The deployed files, and the directories that hold them, relative to SRC.
file(GLOB_RECURSE source_files LIST_DIRECTORIES false RELATIVE "${SRC}" "${SRC}/*")
set(deployed_files "")
set(deployed_dirs "")
foreach(file IN LISTS source_files)
    if(file MATCHES "${not_deployed}")
        continue()
    endif()
    list(APPEND deployed_files "${file}")
    get_filename_component(dir "${file}" DIRECTORY)
    while(NOT dir STREQUAL "" AND NOT dir IN_LIST deployed_dirs)
        list(APPEND deployed_dirs "${dir}")
        get_filename_component(dir "${dir}" DIRECTORY)
    endwhile()
endforeach()

# Remove what is not deployed: a directory that holds no deployed file with its content, any other file. Entries under
# a directory removed before them no longer exist, and removing them again does nothing.
if(IS_DIRECTORY "${DST}")
    file(GLOB_RECURSE present LIST_DIRECTORIES true RELATIVE "${DST}" "${DST}/*")
    foreach(entry IN LISTS present)
        if(IS_DIRECTORY "${DST}/${entry}")
            if(NOT entry IN_LIST deployed_dirs)
                message(STATUS "Removing ${DST}/${entry}: no longer deployed")
                file(REMOVE_RECURSE "${DST}/${entry}")
            endif()
        elseif(EXISTS "${DST}/${entry}" AND NOT entry IN_LIST deployed_files)
            message(STATUS "Removing ${DST}/${entry}: no longer deployed")
            file(REMOVE "${DST}/${entry}")
        endif()
    endforeach()
endif()

# Copy what is new or changed; a copy with the same content is left alone, timestamp included.
file(MAKE_DIRECTORY "${DST}")
foreach(file IN LISTS deployed_files)
    get_filename_component(dir "${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${DST}/${dir}")
    file(COPY_FILE "${SRC}/${file}" "${DST}/${file}" ONLY_IF_DIFFERENT)
endforeach()
