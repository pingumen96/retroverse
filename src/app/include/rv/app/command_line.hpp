#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/common.h>

#include <rv/core/expected.hpp>

namespace rv::app {

/// Scene loaded when --scene is not given.
inline constexpr std::string_view kDefaultSceneName = "m0_testroom";
/// Directory of the scenes selected by name, relative to the asset root.
inline constexpr std::string_view kSceneDirectory = "style_tests/reference_scenes";

/// Validated retroverse.exe command line. Defaults describe an interactive run in a 1280x720 window.
struct CommandLineOptions {
    static constexpr int kDefaultWindowWidth = 1280;
    static constexpr int kDefaultWindowHeight = 720;
    static constexpr int kMinWindowExtent = 64;
    static constexpr int kMaxWindowExtent = 16384;
    /// Frames rendered by --smoke and --exit-after-frames when --frames is not given.
    static constexpr std::uint32_t kDefaultFrameLimit = 3;
    static constexpr int kDefaultScreenshotWidth = 1920;
    static constexpr int kDefaultScreenshotHeight = 1080;
    /// Largest screenshot side: the PNG encoder's limit (rv::assets::kMaxImageDimension), which every OpenGL 4.5
    /// implementation supports as a framebuffer size.
    static constexpr int kMaxScreenshotExtent = 16384;
    /// Largest --fixed-dt step in seconds.
    static constexpr double kMaxFixedDeltaSeconds = 1.0;

    bool showHelp = false;    ///< --help / -h
    bool showVersion = false; ///< --version

    int windowWidth = kDefaultWindowWidth;   ///< --width, window coordinates
    int windowHeight = kDefaultWindowHeight; ///< --height, window coordinates
    bool fullscreen = false;                 ///< --fullscreen: start in borderless fullscreen
    bool hidden = false;                     ///< --hidden: never show the window (frame-limited runs only)
    bool vsync = true;                       ///< cleared by --no-vsync

    bool smoke = false;           ///< --smoke: render frameLimit frames, exit; GL errors or an early stop fail the run
    bool exitAfterFrames = false; ///< --exit-after-frames (alias --exit): stop after frameLimit frames
    /// Frames to render before exiting: --frames, or kDefaultFrameLimit with --smoke / --exit-after-frames. Empty: run
    /// until the user quits.
    std::optional<std::uint32_t> frameLimit;
    bool glSelfTest = false; ///< --gl-selftest: provoke one GL_INVALID_ENUM after initialization

    std::optional<std::filesystem::path> assetRoot; ///< --asset-root
    std::optional<std::filesystem::path> dataDir;   ///< --data-dir
    bool portable = false;                          ///< --portable

    spdlog::level::level_enum logLevel = spdlog::level::info; ///< --log-level
    /// --no-dialogs: report failures in the log only. Implied by every frame-limited run (--smoke,
    /// --exit-after-frames), which may be unattended: a modal message box would keep it from ever exiting.
    bool noDialogs = false;

    /// --scene: a scene name (see isSceneName) or a path to a scene file; empty selects kDefaultSceneName. Resolve it
    /// with resolveScenePath().
    std::optional<std::string> scene;
    /// --screenshot <png>: PNG of the last frame of a frame-limited run.
    std::optional<std::filesystem::path> screenshot;
    /// --screenshot-size WxH: resolution of --screenshot and F12 screenshots, independent of the window.
    int screenshotWidth = kDefaultScreenshotWidth;
    int screenshotHeight = kDefaultScreenshotHeight;

    /// --input-script <json>: action values that replace the device input on chosen frames (rv::input::InputScript).
    std::optional<std::filesystem::path> inputScript;
    /// --fixed-dt <seconds>: time step of every frame's simulation (onUpdate) instead of the measured frame time, in
    /// (0, kMaxFixedDeltaSeconds]. With --input-script it makes automated runs reproducible.
    std::optional<double> fixedDeltaSeconds;

    /// --model <file>: accepted and kept for the step that implements it; the host logs that it is ignored.
    std::optional<std::filesystem::path> model;
};

/// Parses and validates arguments (UTF-8, program name excluded). Errors name the offending option:
///  - unknown options, missing values, malformed numbers;
///  - --width / --height outside [64, 16384]; --frames below 1;
///  - --frames without --smoke or --exit-after-frames, --hidden without a frame limit (nobody could close the
///    window), --screenshot without a frame limit (there would be no last frame to capture);
///  - --screenshot not ending in ".png"; --screenshot-size other than WIDTHxHEIGHT with sides in [1, 16384];
///  - --scene names with characters other than letters, digits, '_', '-' and '.', or starting with '.';
///  - --fixed-dt that is not a number in (0, kMaxFixedDeltaSeconds];
///  - --log-level other than trace, debug, info, warn or error; empty paths or names.
/// The --input-script file itself is read by the application (a bad script fails its initialization).
/// With --help or --version the remaining options are not validated.
[[nodiscard]] Expected<CommandLineOptions> parseCommandLine(std::span<const std::string> arguments);
/// Convenience overload for main(): skips argv[0].
[[nodiscard]] Expected<CommandLineOptions> parseCommandLine(int argc, const char* const* argv);

/// Usage text printed by --help, including the exit codes.
[[nodiscard]] std::string commandLineHelp();

/// True when a --scene value names a scene instead of a file: it has no directory separator ('/' or '\\'), no drive
/// colon and no ".json" extension (letter case ignored).
[[nodiscard]] bool isSceneName(std::string_view value) noexcept;

/// Scene file selected by CommandLineOptions::scene: a name resolves to <assetRoot>/style_tests/reference_scenes/
/// <name>.json, a path is used as given (a relative path is relative to the working directory), and no value selects
/// kDefaultSceneName.
[[nodiscard]] std::filesystem::path resolveScenePath(const std::optional<std::string>& scene,
                                                     const std::filesystem::path& assetRoot);

} // namespace rv::app
