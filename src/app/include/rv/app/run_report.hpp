#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <rv/app/command_line.hpp>
#include <rv/app/self_check.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/frame_stats.hpp>

namespace rv::app {

/// Version of the run report format. parseRunReport() refuses other versions; fields are only ever added within one.
inline constexpr int kRunReportSchemaVersion = 1;

/// The OpenGL context a run rendered with, as the driver names it (gl::GLCapabilities).
struct RunReportContext {
    std::string vendor;      ///< GL_VENDOR
    std::string renderer;    ///< GL_RENDERER, e.g. "NVIDIA GeForce RTX 2080 SUPER/PCIe/SSE2"
    std::string version;     ///< GL_VERSION, which names the driver too, e.g. "4.5.0 NVIDIA 617.42"
    std::string glslVersion; ///< GL_SHADING_LANGUAGE_VERSION

    bool operator==(const RunReportContext&) const = default;
};

/// OpenGL debug messages a run received, by severity (gl::GLDevice::debugMessageCount: repeats the log suppressed
/// included, notifications and known informational ids excluded).
struct DebugMessageCounts {
    std::uint64_t high = 0;
    std::uint64_t medium = 0;
    std::uint64_t low = 0;

    bool operator==(const DebugMessageCounts&) const = default;
};

/// The --screenshot a run wrote.
struct ScreenshotRecord {
    std::filesystem::path file; ///< as given on the command line
    int width = 0;
    int height = 0;

    bool operator==(const ScreenshotRecord&) const = default;
};

/// The outcome of a --self-check.
struct SelfCheckRecord {
    SelfCheck check = SelfCheck::Srgb;
    bool passed = false;
    /// The measurements as logged, or why the check could not run.
    std::string summary;

    bool operator==(const SelfCheckRecord&) const = default;
};

/// What a run of an application built on AppHost did (retroverse.exe, retroverse-asset-preview), written as JSON by
/// --run-report <file.json> when the run ends, whether it succeeded or failed (formatRunReport). Automated checks (the
/// GPU smoke tests of tests/smoke, scripts/gate.ps1) read it back with readRunReport instead of scraping the log.
///
/// The host fills the executable, the directories, the outcome, the OpenGL context, the debug message counts and the
/// frames (AppHost); the application logic adds what it rendered through AppContext::report() (SceneGraphics: the last
/// view's statistics, the instance buffer, the screenshot; RetroverseApp: the walker's final pose and the self-check).
struct RunReport {
    /// AppIdentity::name, e.g. "Retroverse".
    std::string application;
    /// core::kVersion and core::kBuildType of the executable.
    std::string version;
    std::string buildType;
    /// Built with the developer overlay (RV_DEV_UI).
    bool developerOverlay = false;
    /// Built to route OpenGL debug output to the log (RV_GL_DEBUG); without it, debugMessages stays empty.
    bool glDebugOutput = false;

    /// The asset root and where it came from (core::toString(core::AssetRootSource): "command line", "RV_ASSET_ROOT",
    /// "source tree", "executable directory"); empty when the directories could not be resolved.
    std::filesystem::path assetRoot;
    std::string assetRootSource;

    /// The process exit code (ExitCode) and its name (toString(ExitCode)).
    int exitCode = 0;
    std::string outcome;
    /// What failed, when the run failed: the initialization error, the first failure the logic reported
    /// (AppContext::failRun), the smoke check or an unexpected error.
    std::optional<std::string> failure;
    /// The initialization failed because no OpenGL 4.5 core profile context could be created (the window's context, or
    /// the GL device that requires 4.5): the machine cannot run Retroverse, which tests may treat as a skip.
    bool openGlUnavailable = false;

    /// The OpenGL context; absent when none was created.
    std::optional<RunReportContext> context;
    /// Debug messages over the whole run; absent without debug output (glDebugOutput off).
    std::optional<DebugMessageCounts> debugMessages;

    /// CommandLineOptions::frameLimit, and the frames presented.
    std::optional<std::uint32_t> frameLimit;
    std::uint64_t framesRendered = 0;

    /// What the last view of the run drew (the window's, or the screenshot's when no window view was drawn); absent
    /// when no view was rendered. Its GPU times are those FrameStats reported for that view.
    std::optional<gl::FrameStats> lastView;
    /// Instance matrices per region of the renderer's instance buffer when the run ended: it starts at
    /// gl::SceneRenderer::kInitialInstanceCapacity and grows when a view needs more. 0 without a renderer.
    std::size_t instanceCapacity = 0;
    /// The --screenshot of the last frame, when it was written.
    std::optional<ScreenshotRecord> screenshot;
    /// Where the walker of retroverse.exe ended, as --spawn takes it (feet, yaw and pitch): a run started with that
    /// --spawn and no input sees the same view.
    std::optional<SpawnPoint> finalCamera;
    /// The --self-check of the run.
    std::optional<SelfCheckRecord> selfCheck;
};

/// The report as JSON text (two-space indented, keys in the order of RunReport, absent optionals left out, paths
/// UTF-8 with '/' separators, floating point values that read back exactly).
[[nodiscard]] std::string formatRunReport(const RunReport& report);

/// Parses formatRunReport() output. Keys it does not know are ignored (a newer writer may add some). Errors
/// (ValidationFailed, context "<context>:<JSON pointer>"): another schemaVersion, a missing required key (every
/// non-optional field but failure), a value of the wrong type or out of range; ParseError for malformed JSON.
[[nodiscard]] Expected<RunReport> parseRunReport(std::string_view json, std::string_view context);

/// Writes formatRunReport() atomically (core::writeTextFileAtomic: parent directories created).
[[nodiscard]] Status writeRunReport(const std::filesystem::path& path, const RunReport& report);

/// Reads and parses a report file (parseRunReport, with the path as context).
[[nodiscard]] Expected<RunReport> readRunReport(const std::filesystem::path& path);

} // namespace rv::app
