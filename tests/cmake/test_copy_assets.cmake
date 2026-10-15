# ctest unit.cmake/copy_assets: cmake/CopyAssets.cmake, which deploys the assets next to Release executables
# (cmake/AssetDeploy.cmake), mirrors an asset tree: new and changed files copied, files gone from the source removed with
# the directories they leave empty, the staging area, the golden screenshots and .gitkeep placeholders never deployed.
#
#   cmake -DCOPY_ASSETS=<cmake/CopyAssets.cmake> -DWORK_DIR=<scratch directory, emptied> -P test_copy_assets.cmake

cmake_minimum_required(VERSION 3.28)

foreach(variable IN ITEMS COPY_ASSETS WORK_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "test_copy_assets.cmake requires -D${variable}")
    endif()
endforeach()

set(src "${WORK_DIR}/src")
set(dst "${WORK_DIR}/dst")
file(REMOVE_RECURSE "${WORK_DIR}")

function(mirror)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSRC=${src}" "-DDST=${dst}" -P "${COPY_ASSETS}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "CopyAssets.cmake failed (${result}):\n${output}")
    endif()
endfunction()

# Fails unless DST holds exactly the files named in ARGN.
function(expect_files)
    set(expected ${ARGN})
    list(SORT expected)
    file(GLOB_RECURSE present LIST_DIRECTORIES false RELATIVE "${dst}" "${dst}/*")
    list(SORT present)
    if(NOT "${present}" STREQUAL "${expected}")
        message(FATAL_ERROR "deployed files: '${present}', expected: '${expected}'")
    endif()
endfunction()

function(expect_content file expected)
    file(READ "${dst}/${file}" content)
    if(NOT content STREQUAL expected)
        message(FATAL_ERROR "${file} holds '${content}', expected '${expected}'")
    endif()
endfunction()

function(expect_no_directory dir)
    if(EXISTS "${dst}/${dir}")
        message(FATAL_ERROR "${dir} is still deployed")
    endif()
endfunction()

# The source tree, and a deployment of an older one: a shader since deleted, the golden screenshots an older deployment
# copied, an orphan directory.
file(WRITE "${src}/shaders/pbr/pbr.frag" "pbr")
file(WRITE "${src}/style_tests/reference_scenes/room.json" "{}")
file(WRITE "${src}/style_tests/screenshots/golden.png" "png")
file(WRITE "${src}/staging/download.glb" "glb")
file(WRITE "${src}/models/.gitkeep" "")
file(WRITE "${dst}/shaders/unlit/unlit.frag" "stale")
file(WRITE "${dst}/style_tests/screenshots/golden.png" "png")
file(WRITE "${dst}/old/deeper/orphan.txt" "orphan")

mirror()
expect_files(shaders/pbr/pbr.frag style_tests/reference_scenes/room.json)
expect_content(shaders/pbr/pbr.frag "pbr")
foreach(dir IN ITEMS shaders/unlit style_tests/screenshots staging models old)
    expect_no_directory(${dir})
endforeach()

# An edit, a deletion and an addition in the source reach the next mirror; a directory left empty goes.
file(WRITE "${src}/shaders/pbr/pbr.frag" "pbr, edited")
file(REMOVE "${src}/style_tests/reference_scenes/room.json")
file(WRITE "${src}/config/bindings.json" "[]")

mirror()
expect_files(config/bindings.json shaders/pbr/pbr.frag)
expect_content(shaders/pbr/pbr.frag "pbr, edited")
expect_content(config/bindings.json "[]")
expect_no_directory(style_tests)

# Nothing to do: the deployment stays as it is.
mirror()
expect_files(config/bindings.json shaders/pbr/pbr.frag)

file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "CopyAssets.cmake mirrors the asset tree")
