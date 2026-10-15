# I1 risk spikes (docs/roadmap.md): one measurement executable per spike, built on the first version of the module it
# de-risks. Each spike directory registers its own ctest cases (label "gpu" for automated GPU checks, "desktop" for
# checks that take over the screen and are run on demand).
#   spikes/video   two FFmpeg-decoded videos uploaded through persistently mapped PBOs (rv_media, gl::VideoTexture)
#   spikes/ui      an RmlUi screen navigated by gamepad (rv_ui)
#   spikes/launch  emulator launch and return to a borderless fullscreen window (rv_launcher, rv_platform)

foreach(spike IN ITEMS video ui launch)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${spike}/CMakeLists.txt")
        add_subdirectory(${spike})
    endif()
endforeach()
