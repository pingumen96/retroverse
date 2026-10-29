# I1 risk spikes (docs/roadmap.md): one measurement executable per spike, built on the first version of the module it
# de-risks. Each spike directory registers its own ctest cases (label "unit" for GPU-free unit tests such as
# spike_launch_tests, "gpu" for automated GPU checks, skipped without an OpenGL 4.5 context like the smoke tests,
# "desktop" for checks that take over the screen and are run on demand).
#   spikes/video   two FFmpeg-decoded videos uploaded through persistently mapped PBOs (rv_media, gl::VideoTexture)
#   spikes/ui      an RmlUi screen navigated by gamepad (rv_ui)
#   spikes/launch  emulator launch and return to a borderless fullscreen window (rv_launcher, rv_platform)
#   spikes/xr      I10 step 0: an OpenXR session on an OpenGL context, a stereo room in the headset (RV_ENABLE_XR)

foreach(spike IN ITEMS video ui launch xr)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${spike}/CMakeLists.txt")
        add_subdirectory(${spike})
    endif()
endforeach()
