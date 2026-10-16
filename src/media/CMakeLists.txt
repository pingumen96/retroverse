# rv_media: video decoding with FFmpeg (specification §9.4): demuxing and decoding on a worker thread per video
# (VideoDecoder), a bounded queue of frames (FrameQueue) fed from a pool of reusable frames (FramePool), the
# presentation clock that picks frames by timestamp (PresentationClock), the playback of one video for the render
# thread (VideoPlayback), sample statistics, and the synthetic video fixtures of the tests and the video spike
# (test_video.hpp). GPU-free: decoded frames are RGBA8 CPU images that rv_render uploads (gl::VideoTexture); FFmpeg types
# never appear in the public headers.

add_library(rv_media STATIC
    src/ffmpeg_support.cpp
    src/frame_queue.cpp
    src/output_size.cpp
    src/presentation_clock.cpp
    src/sample_stats.cpp
    src/test_video.cpp
    src/video_decoder.cpp
    src/video_frame.cpp
    src/video_playback.cpp
)
add_library(rv::media ALIAS rv_media)
rv_set_target_defaults(rv_media)
target_include_directories(rv_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# rv_assets: the bitmap font of the test clips' frame numbers.
target_link_libraries(rv_media PUBLIC rv_core PRIVATE rv_assets rv::ffmpeg)
rv_forbid_link(rv_media)
