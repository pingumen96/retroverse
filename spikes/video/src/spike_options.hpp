#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rv/app/command_line.hpp>
#include <rv/core/expected.hpp>
#include <rv/media/test_video.hpp>

namespace rv::spike {

/// The command line of spike_video (docs/spikes/i1-video.md).
struct SpikeOptions {
    static constexpr int kDefaultWidth = 1920;
    static constexpr int kDefaultHeight = 1080;

    bool showHelp = false;
    /// --list-encoders: print the FFmpeg build and its video encoders, then exit.
    bool listEncoders = false;
    /// --videos <file>...: the videos to play, read in place (never written).
    std::vector<std::filesystem::path> videos;
    /// --generate-fixtures <dir>: writes the test clips (and the damaged files) there, and plays the clips when no
    /// --videos are given.
    std::optional<std::filesystem::path> fixtureDirectory;
    /// --error-cases: with --generate-fixtures, plays the damaged files (missing, empty, truncated, corrupt, not a
    /// video, audio only, unknown codec) beside one good clip.
    bool errorCases = false;
    /// --regenerate: writes the fixtures even when files of the same name exist.
    bool regenerate = false;
    /// --count N: videos played: the clips generated, or the --videos list repeated or cut (decoders of the same file
    /// are independent). Without --count, every file of --videos plays once.
    int count = 2;
    bool countGiven = false;
    /// --fixture-size WxH, --fixture-fps, --fixture-seconds, --fixture-codec mpeg4|mjpeg: the generated clips.
    media::TestVideoSpec fixture;
    /// --fixture-container mp4|mkv|avi.
    std::string fixtureExtension = ".mp4";
    /// --seconds: length of the run (after the warm-up).
    double seconds = 60.0;
    /// --warmup: seconds at the start left out of the frame time, upload and load statistics.
    double warmupSeconds = 1.0;
    /// --max-height, --queue, --decoder-threads: the decoders' settings.
    int maxHeight = 720;
    std::size_t queueCapacity = 4;
    int decoderThreads = 1;
    /// --upload-fences: guard the pixel buffers with fences (gl::VideoUploadSync::Fence) instead of the copies' end
    /// timestamps.
    bool uploadFences = false;
    /// --check-frames: decode the frame counter of every uploaded frame (test clips) and of every picture in the
    /// screenshot, so that a check can tell the texture shows the frame last uploaded (the whole upload path).
    bool checkFrames = false;
    /// --texture-self-test: run runTextureSelfTest() before playing.
    bool textureSelfTest = false;
    /// --priority normal|above-normal|high: the process priority class (SetPriorityClass), so that measurements on a
    /// machine busy with other work can tell the pipeline's own cost from preemption. Decoder threads inherit it.
    std::string priority = "normal";
    /// --report <json>: the measurements.
    std::optional<std::filesystem::path> report;
    /// --screenshot <png>: the last frame.
    std::optional<std::filesystem::path> screenshot;
    /// Window, vsync, hidden, data directory and log level for app::AppHost; always frame-limited and without dialogs.
    app::CommandLineOptions host;
};

/// Parses the arguments (program name excluded). Errors name the offending option.
[[nodiscard]] Expected<SpikeOptions> parseSpikeOptions(std::span<const std::string> arguments);

/// Usage text of --help.
[[nodiscard]] std::string spikeHelp();

} // namespace rv::spike
