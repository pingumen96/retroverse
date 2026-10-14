#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include <spdlog/common.h>

#include <rv/core/expected.hpp>

namespace rv::app {

/// Validated retroverse.exe command line. Defaults describe an interactive run in a 1280x720 window.
struct CommandLineOptions {
    static constexpr int kDefaultWindowWidth = 1280;
    static constexpr int kDefaultWindowHeight = 720;
    static constexpr int kMinWindowExtent = 64;
    static constexpr int kMaxWindowExtent = 16384;
    static constexpr std::uint32_t kDefaultSmokeFrames = 3;

    bool showHelp = false;    ///< --help / -h
    bool showVersion = false; ///< --version

    int windowWidth = kDefaultWindowWidth;   ///< --width, window coordinates
    int windowHeight = kDefaultWindowHeight; ///< --height, window coordinates
    bool fullscreen = false;                 ///< --fullscreen: start in borderless fullscreen
    bool hidden = false;                     ///< --hidden: never show the window (frame-limited runs only)
    bool vsync = true;                       ///< cleared by --no-vsync

    bool smoke = false;           ///< --smoke: render frameLimit frames, exit; GL errors fail the run
    bool exitAfterFrames = false; ///< --exit-after-frames (alias --exit): stop after frameLimit frames
    /// Frames to render before exiting: --frames, or 3 with --smoke. Empty: run until the user quits.
    std::optional<std::uint32_t> frameLimit;
    bool glSelfTest = false; ///< --gl-selftest: provoke one GL_INVALID_ENUM after initialization

    std::optional<std::filesystem::path> assetRoot; ///< --asset-root
    std::optional<std::filesystem::path> dataDir;   ///< --data-dir
    bool portable = false;                          ///< --portable

    spdlog::level::level_enum logLevel = spdlog::level::info; ///< --log-level
    bool noDialogs = false; ///< --no-dialogs, implied by --smoke: report failures in the log only

    // Accepted and kept for the steps that implement them; the host logs that they are not implemented yet.
    std::optional<std::string> scene;                ///< --scene <name>
    std::optional<std::filesystem::path> screenshot; ///< --screenshot <png>
    std::optional<std::filesystem::path> model;      ///< --model <file>
};

/// Parses and validates arguments (UTF-8, program name excluded). Errors name the offending option:
///  - unknown options, missing values, malformed numbers;
///  - --width / --height outside [64, 16384]; --frames below 1;
///  - --frames without --smoke or --exit-after-frames, --exit-after-frames without --frames, --hidden without a
///    frame limit (nobody could close the window);
///  - --log-level other than trace, debug, info, warn or error; empty paths or names.
/// With --help or --version the remaining options are not validated.
[[nodiscard]] Expected<CommandLineOptions> parseCommandLine(std::span<const std::string> arguments);
/// Convenience overload for main(): skips argv[0].
[[nodiscard]] Expected<CommandLineOptions> parseCommandLine(int argc, const char* const* argv);

/// Usage text printed by --help, including the exit codes.
[[nodiscard]] std::string commandLineHelp();

} // namespace rv::app
