#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
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
/// included, notifications and known informational ids excluded), those --gl-selftest provoked included
/// (RunReport::glSelfTest).
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

/// The quick catalog of a run of retroverse.exe (the product UI exists only when the run has a catalog).
struct CatalogRecord {
    /// Where its games come from: "mock" (--mock-catalog, until the ES-DE catalog reaches the application in I2 step
    /// 13).
    std::string source;
    /// The --mock-catalog file, as given.
    std::filesystem::path file;
    /// Games the source lists.
    std::size_t games = 0;
    /// Times the catalog was opened, and whether it was open when the run ended.
    std::uint64_t opened = 0;
    bool open = false;
    /// The selected game when the run ended: its position in the list and its id; none while the list is empty.
    std::optional<std::size_t> selectedPosition;
    std::string selectedId;

    bool operator==(const CatalogRecord&) const = default;
};

/// One launch from the quick catalog (LaunchFlow fills it): what was asked, what the session did, how the host waited.
struct LaunchRecord {
    std::string gameId;
    /// The emulator profile's id.
    std::string profile;
    /// The session's final state (launcher::toString(SessionState): "Returned", "Crashed", "FailedToStart"; a run that
    /// ends while the game runs ends its session too, as Returned with quitDuringGame), "Blocked" for a launch the plan
    /// refused (nothing started), "Pending" when the run ended before it could start.
    std::string state;
    /// The outcome as the product UI tells it (LaunchMessage: launch_outcome::k*, its reason and detail).
    std::string outcome;
    std::string reason;
    std::string detail;
    /// The authoritative exit code, when something exited.
    std::optional<std::uint32_t> exitCode;
    /// The wait strategy in effect (launcher::toString(WaitStrategy)); empty when nothing started.
    std::string strategy;
    /// The started process's id (0 when nothing started).
    std::uint32_t processId = 0;
    /// launcher::LaunchOutcome's flags.
    bool quickExit = false;
    bool processesRemained = false;
    bool confirmedByUser = false;
    bool wasStillRunning = false;
    /// The run ended (its window closed, a quit request) while the emulator ran: it was left running.
    bool quitDuringGame = false;
    /// A frame-limited run stopped waiting for the game after its --launch-wait-limit (LaunchFlow::setWaitLimit): the
    /// session ended as left running.
    bool waitLimitReached = false;
    /// The frame whose input asked for the launch; the frame index when the emulator started and when its session
    /// ended (the next frame to render: every frame before it had been presented). Equal start and end frames: no
    /// frame was rendered while the game ran.
    std::uint64_t requestFrame = 0;
    std::optional<std::uint64_t> startFrame;
    std::optional<std::uint64_t> endFrame;
    /// Frames rendered during the session: for a question of the game-session panel (StillRunning, a confirmation, the
    /// user back in Retroverse), and while the game had the screen: none (the host waits instead), but the frame after a
    /// choice to go back to the game (it shows the "Gioco in corso" notice, which the window keeps behind it).
    std::uint64_t framesWithPanel = 0;
    std::uint64_t framesWhileRunning = 0;
    /// Times the host woke from its wait while the session ran (the runner's wake events, other events, the polling
    /// interval).
    std::uint64_t idleWakes = 0;
    /// Milliseconds: from the start to the process running (the window's hand-over included), the process's run (to
    /// the authoritative exit, or to the end of the session when nothing exited), and from that exit to the end of the
    /// session.
    std::optional<double> startMs;
    std::optional<double> runMs;
    std::optional<double> returnMs;
    /// Milliseconds from that exit (the end of the session when nothing exited) to Retroverse's window back in the
    /// foreground (platform::WindowHandover); absent for hidden windows, which have no hand-over, and while it is not.
    std::optional<double> foregroundMs;
    /// The panels the launch showed, in order (toString(SessionPanel): "still-running", "confirm", "playing",
    /// "outcome").
    std::vector<std::string> panels;

    bool operator==(const LaunchRecord&) const = default;
};

/// What a run of an application built on AppHost did (retroverse.exe, retroverse-asset-preview), written as JSON by
/// --run-report <file.json> when the run ends, whether it succeeded or failed (formatRunReport). Only the host writes
/// it: a command line that does not parse, --help, --version and --write-fixture end before the host starts and write
/// none. Automated checks (the GPU smoke tests of tests/smoke, scripts/gate.ps1) read it back with readRunReport
/// instead of scraping the log.
///
/// The host fills the executable, the directories, the outcome, the OpenGL context, the debug message counts, the
/// --gl-selftest outcome and the frames (AppHost); the application logic adds what it rendered through
/// AppContext::report() (SceneGraphics: the last view's statistics, the instance buffer, the screenshot; RetroverseApp:
/// the walker's final pose, the self-check, the quick catalog and its launches).
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
    /// The outcome of --gl-selftest; absent without it. The messages it provoked are counted in debugMessages as well,
    /// so the run had no unexpected error when debugMessages.high equals glSelfTest.highSeverityCaptured (the rule
    /// exitCodeForCompletedRun applies to --smoke runs).
    std::optional<SelfTestOutcome> glSelfTest;

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
    /// --spawn and no input sees the same view up to float rounding (the yaw goes through radians and back, so a few
    /// edges may move by a fraction of a pixel).
    std::optional<SpawnPoint> finalCamera;
    /// The --self-check of the run.
    std::optional<SelfCheckRecord> selfCheck;
    /// The quick catalog; absent in runs without one (no --mock-catalog).
    std::optional<CatalogRecord> catalog;
    /// The launches from the quick catalog, in order.
    std::vector<LaunchRecord> launches;
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
