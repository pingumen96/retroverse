# Automated check of spike_video (ctest, label "gpu"): runs it hidden on fixtures it generates under WORK_DIR and checks
# its exit code, its JSON report and its screenshot. Thresholds are loose enough for a reference machine busy with
# builds; the 60 s measurements of docs/spikes/i1-video.md hold the real numbers.
#
#   cmake -DSPIKE=<spike_video.exe> -DWORK_DIR=<dir> -DMODE=smoke|errors -P check_spike_video.cmake
#
#   smoke   two 1280x720 30 fps clips for 3 s: both play, the screenshot shows the frames uploaded last (their burned-in
#           counters), no GL error, gl::VideoTexture's resize and busy-ring self-test passes; on time, without drift
#           and with sane frame and upload times when other programs leave the machine at most half busy
#   errors  a good clip and the damaged files of SC-08: each damaged file falls back for its reason, the good clip shows
#           the frame uploaded last, no GL error, the run completes
cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS SPIKE WORK_DIR MODE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is not set")
    endif()
endforeach()

set(report "${WORK_DIR}/report.json")
set(screenshot "${WORK_DIR}/last_frame.png")
set(log "${WORK_DIR}/spike_video_output.txt")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(REMOVE "${report}" "${screenshot}" "${log}")

set(arguments
    --generate-fixtures "${WORK_DIR}/fixtures" --fixture-seconds 3 --warmup 0.5 --hidden --check-frames
    --report "${report}" --screenshot "${screenshot}" --data-dir "${WORK_DIR}/data")
if(MODE STREQUAL "smoke")
    list(APPEND arguments --seconds 3 --texture-self-test)
elseif(MODE STREQUAL "errors")
    # The truncated and the corrupt copies fail when playback reaches their damage, about 1.5 s in.
    list(APPEND arguments --error-cases --seconds 3 --fixture-size 640x360)
else()
    message(FATAL_ERROR "unknown MODE '${MODE}' (smoke or errors)")
endif()

execute_process(COMMAND "${SPIKE}" ${arguments}
    RESULT_VARIABLE exit_code OUTPUT_FILE "${log}" ERROR_FILE "${log}" TIMEOUT 400)
if(NOT exit_code EQUAL 0)
    file(READ "${log}" output)
    # No OpenGL 4.5 core profile context (AppHost logs this line, then exits with 2): skipped like the smoke tests,
    # through the test's SKIP_REGULAR_EXPRESSION on the marker below, printed unwrapped (message(FATAL_ERROR) reflows
    # the spike's lines and can split the phrase).
    if(exit_code EQUAL 2 AND output MATCHES "requires OpenGL 4\\.5 \\(core profile\\)")
        message(STATUS "SKIPPED: no OpenGL 4.5 core profile context on this machine")
        return()
    endif()
    message(FATAL_ERROR "spike_video exited with '${exit_code}' (0 expected). Its output:\n${output}")
endif()
if(NOT EXISTS "${report}")
    message(FATAL_ERROR "spike_video wrote no report at ${report}")
endif()
file(READ "${report}" json)

set(failures "")
# Reads a value of the report: json_get(<variable> <path elements>...).
function(json_get variable)
    string(JSON value ERROR_VARIABLE error GET "${json}" ${ARGN})
    if(error)
        message(FATAL_ERROR "report: ${error}")
    endif()
    set(${variable} "${value}" PARENT_SCOPE)
endfunction()
# Reads a value that may be absent: json_get_or(<variable> <default> <path elements>...).
function(json_get_or variable fallback)
    string(JSON value ERROR_VARIABLE error GET "${json}" ${ARGN})
    if(error)
        set(value "${fallback}")
    endif()
    set(${variable} "${value}" PARENT_SCOPE)
endfunction()
# Records a failed expectation: expect(<description> <condition>...).
macro(expect description)
    if(NOT (${ARGN}))
        list(APPEND failures "${description}")
    endif()
endmacro()
# Expects video <index> to show, in the screenshot, the frame it uploaded last: the counter burned into the clip reads
# the same from the uploaded frame and from the picture on screen, and the picture is not black (a black picture would
# read as counter 0). This checks the whole path: decoder, pixel buffer, texture, shader, output.
macro(expect_frame_on_screen index)
    json_get(last_counter videos ${index} upload lastCounter)
    json_get(screen_counter videos ${index} upload screenCounter)
    json_get(screen_luma videos ${index} upload screenMeanLuma)
    expect("video ${index} shows the frame it uploaded last (uploaded ${last_counter}, on screen ${screen_counter})"
           last_counter GREATER_EQUAL 0 AND screen_counter EQUAL last_counter)
    expect("video ${index}'s picture is not black (mean luma ${screen_luma})" screen_luma GREATER 20)
endmacro()

json_get(completed run completed)
expect("the run completed" completed)

# GL errors raised by the upload path (high-severity debug messages, counted in builds with GL debug output).
json_get(debug_enabled glDebugMessages enabled)
json_get(debug_high glDebugMessages high)
if(debug_enabled)
    expect("no high-severity GL debug message (${debug_high})" debug_high EQUAL 0)
endif()

# The screenshot is a 1920x1080 PNG (the IHDR chunk holds the size, big-endian, at bytes 16 to 23).
if(EXISTS "${screenshot}")
    file(READ "${screenshot}" header LIMIT 24 HEX)
    string(SUBSTRING "${header}" 32 8 width_hex)
    string(SUBSTRING "${header}" 40 8 height_hex)
    math(EXPR width "0x${width_hex}")
    math(EXPR height "0x${height_hex}")
    expect("the screenshot is 1920x1080 (it is ${width}x${height})" width EQUAL 1920 AND height EQUAL 1080)
else()
    list(APPEND failures "no screenshot at ${screenshot}")
endif()

if(MODE STREQUAL "smoke")
    json_get(self_test textureSelfTest passed)
    json_get(self_test_failures textureSelfTest failures)
    expect("the VideoTexture self-test passes (${self_test_failures})" self_test)
    # Variables are named apart from the words compared below: expect() passes its condition unquoted.
    # Whatever depends on the render loop's pace (frames shown, late and dropped frames, drift, frame and upload times)
    # is only checked when other programs left the machine at most half busy: beyond that, other programs delay the
    # render loop's frames and swaps (10 FPS has been seen at 100% load; the 60 s runs of docs/spikes/i1-video.md show
    # long frames from about 50% on), which says nothing about the pipeline (the report records the load). The rest
    # always holds: the clips play without errors and their frames reach the screen.
    json_get(machine_busy load machineBusyPercent)
    if(machine_busy LESS 50)
        set(check_timings ON)
    else()
        set(check_timings OFF)
        message(STATUS "The machine was ${machine_busy}% busy during the run: timing thresholds not checked")
    endif()
    json_get(playing_count criteria playingVideos)
    expect("two videos play (${playing_count})" playing_count EQUAL 2)
    foreach(index RANGE 1)
        json_get(fallback videos ${index} fallback)
        json_get(state videos ${index} state)
        json_get(codec videos ${index} stream codec)
        json_get(output_width videos ${index} stream outputSize 0)
        json_get(presented videos ${index} presentation presented)
        json_get(late_or_dropped videos ${index} presentation lateOrDroppedPercent)
        json_get_or(drift 100 videos ${index} drift percent) # absent when no frame was shown in the window
        json_get(errors videos ${index} decoder decodeErrors)
        json_get(damaged videos ${index} decoder damagedFrames)
        json_get(uploads videos ${index} upload uploads)
        json_get(upload_errors videos ${index} upload errors)
        expect("video ${index} plays (state ${state}, fallback ${fallback})" state STREQUAL "playing" AND NOT fallback)
        expect("video ${index} is the 1280x720 MPEG-4 clip (${codec}, ${output_width} wide)"
               codec STREQUAL "mpeg4" AND output_width EQUAL 1280)
        expect("video ${index} decodes without errors (${errors} errors, ${damaged} damaged frames)"
               errors EQUAL 0 AND damaged EQUAL 0)
        expect("video ${index} shows frames (${presented})" presented GREATER_EQUAL 10)
        expect("video ${index} uploads frames without errors (${uploads} uploads, ${upload_errors} errors)"
               uploads GREATER_EQUAL 10 AND upload_errors EQUAL 0)
        expect_frame_on_screen(${index})
        if(check_timings)
            # 3 s at 30 fps: 90 frames.
            expect("video ${index} shows at least 80 frames in 3 s (${presented})" presented GREATER_EQUAL 80)
            expect("video ${index} has under 3% late or dropped frames (${late_or_dropped}%)" late_or_dropped LESS 3)
            expect("video ${index} drifts less than 3% (${drift}%)" drift LESS 3 AND drift GREATER -3)
        endif()
    endforeach()
    json_get(upload_gpu_count uploadGpuMs count)
    json_get(growth memory workingSetGrowthMiB)
    expect("GPU upload times arrive (${upload_gpu_count})" upload_gpu_count GREATER 0)
    expect("working set grows less than 64 MiB (${growth} MiB)" growth LESS 64)
    if(check_timings)
        # Work moved onto the render thread (a wait for the GPU, the decoder or the disk) shows as many long frames or a
        # slow upload; these bounds leave room for the delays a 3 s run sees on a machine up to half busy.
        json_get(frame_p99 frameIntervalMs p99)
        json_get(upload_cpu_p99 uploadCpuMs p99)
        json_get(long_frames framesOver16_7Ms count)
        expect("p99 frame interval under 25 ms (${frame_p99} ms)" frame_p99 LESS 25)
        expect("at most 10 frame intervals over 16.7 ms in 3 s (${long_frames})" long_frames LESS_EQUAL 10)
        expect("p99 upload CPU time under 3 ms (${upload_cpu_p99} ms)" upload_cpu_p99 LESS 3)
    endif()
else()
    # The order of FixtureSet::errorCases.
    set(expected_failures "none" "file not found" "invalid file" "truncated" "corrupt data" "invalid file"
                          "no video stream" "unsupported codec")
    string(JSON count LENGTH "${json}" videos)
    expect("eight videos (${count})" count EQUAL 8)
    set(index 0)
    foreach(expected IN LISTS expected_failures)
        json_get(file videos ${index} file)
        json_get(failure videos ${index} failure)
        json_get(fallback videos ${index} fallback)
        if(expected STREQUAL "none")
            expect("${file} plays (failure '${failure}')" failure STREQUAL "none" AND NOT fallback)
            expect_frame_on_screen(${index})
        else()
            expect("${file} falls back as '${expected}' (failure '${failure}', fallback ${fallback})"
                   failure STREQUAL expected AND fallback)
        endif()
        math(EXPR index "${index} + 1")
    endforeach()
endif()

if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "spike_video (${MODE}) did not meet its expectations:\n  ${text}\nReport: ${report}")
endif()
message(STATUS "spike_video (${MODE}): every expectation met (report ${report})")
