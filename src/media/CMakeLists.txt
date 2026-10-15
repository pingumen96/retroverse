# rv_media: video decoding with FFmpeg (demux, decode, frame queue, presentation clock). GPU-free: decoded frames are
# CPU images that rv_render uploads; FFmpeg types never appear in the public headers.

add_library(rv_media STATIC
    src/media_module.cpp
)
add_library(rv::media ALIAS rv_media)
rv_set_target_defaults(rv_media)
target_include_directories(rv_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_media PUBLIC rv_core PRIVATE rv::ffmpeg)
rv_forbid_link(rv_media)
