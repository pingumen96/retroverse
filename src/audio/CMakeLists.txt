# rv_audio: the clips' sound (I5 step 5, ADR-0016 §9). GPU-free, rv_core only: the bounded, timestamped ring of
# decoded audio between a decoder's thread and the mixer (AudioRing), the spatial gain and pan (spatial.hpp), the sync to
# the video's media time (AudioSync), the mixer of the screens' sources with its fades, duck and mute (Mixer), the output
# (AudioEngine: miniaudio's shared-mode default endpoint, or the null output the main thread pulls, deterministic under
# --fixed-dt, its mix optionally written as a WAV for the tests), WAV files and the tone analysis of the tests
# (analysis.hpp). miniaudio is header-only and private: its implementation is compiled in miniaudio_impl.cpp, its
# header an external include so that /W4 /WX applies to our code only. FFmpeg never: rv_media decodes, rv_app connects
# the two.

find_path(RV_MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

add_library(rv_audio STATIC
    src/analysis.cpp
    src/audio_engine.cpp
    src/audio_ring.cpp
    src/audio_sync.cpp
    src/miniaudio_impl.cpp
    src/mixer.cpp
    src/spatial.cpp
    src/wav_file.cpp
)
add_library(rv::audio ALIAS rv_audio)
rv_set_target_defaults(rv_audio)
target_include_directories(rv_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rv_audio SYSTEM PRIVATE "${RV_MINIAUDIO_INCLUDE_DIR}")
target_link_libraries(rv_audio PUBLIC rv_core)
rv_forbid_link(rv_audio EXTRA ${RV_AUDIO_FORBIDDEN_LINKS})
