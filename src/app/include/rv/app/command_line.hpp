#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/vec3.hpp>
#include <spdlog/common.h>

#include <rv/app/self_check.hpp>
#include <rv/audio/audio_output.hpp>
#include <rv/core/expected.hpp>

namespace rv::core {
class ArgParser;
class ParsedArgs;
} // namespace rv::core

namespace rv::app {

/// Scene loaded when --scene is not given.
inline constexpr std::string_view kDefaultSceneName = "m0_testroom";
/// Directory of the scenes selected by name, relative to the asset root.
inline constexpr std::string_view kSceneDirectory = "style_tests/reference_scenes";

/// --simulate-minimized FRAME:SECONDS (tests, I5 step 2's review): after `frame` frames the host takes its minimized
/// branch for `seconds` of its waits, as an interactive run whose window is minimized, the window left as it is.
struct SimulatedMinimize {
    /// Longest simulated span, seconds.
    static constexpr double kMaxSeconds = 600.0;

    std::uint64_t frame = 0;
    double seconds = 0.0;

    bool operator==(const SimulatedMinimize&) const = default;
};

/// Where --spawn places the walker at the start instead of the scene camera (a primitive teleport, plan step 12).
struct SpawnPoint {
    /// Largest |coordinate| accepted, meters.
    static constexpr float kMaxCoordinate = 100000.0f;
    /// Largest |pitch| accepted, degrees (the scene camera's limit; the walker clamps further, to its pitch limit).
    static constexpr float kMaxPitchDeg = 89.0f;

    /// The walker's feet, meters (the eye is the walker's eye height above): what the final camera pose of a
    /// frame-limited run logs as "feet".
    glm::vec3 feet{0.0f};
    /// Degrees, the scene camera's yaw when absent (Camera conventions: 0 looks at -Z, positive turns right).
    std::optional<float> yawDeg;
    /// Degrees in [-kMaxPitchDeg, kMaxPitchDeg], the scene camera's pitch when absent.
    std::optional<float> pitchDeg;

    bool operator==(const SpawnPoint&) const = default;
};

/// Parses the value of --spawn: "x,y,z", "x,y,z,yawDeg" or "x,y,z,yawDeg,pitchDeg", decimal numbers (spaces around
/// them allowed). Errors (context "--spawn=<text>"): ParseError for another count of values or a value that is not a
/// number, OutOfRange for a coordinate beyond kMaxCoordinate, a non-finite angle or a pitch beyond kMaxPitchDeg.
[[nodiscard]] Expected<SpawnPoint> parseSpawnPoint(std::string_view text);

/// The runtime the VR mode talks to (--xr-runtime).
enum class XrRuntimeChoice : std::uint8_t {
    /// The system's OpenXR runtime (Meta Horizon Link, SteamVR, ...), or the one --xr-runtime-json names.
    OpenXr,
    /// No headset: xr::FakeRuntime, for tests and for trying the VR path on the desktop.
    Fake,
};

/// --vr and its options (I10 step 2, ADR-0017 §8).
struct XrOptions {
    static constexpr double kMinRenderScale = 0.25;
    static constexpr double kMaxRenderScale = 2.0;

    /// --vr: the run renders in a headset, the window mirroring the left eye; without a runtime or a headset it stays
    /// on the desktop (logged).
    bool enabled = false;
    XrRuntimeChoice runtime = XrRuntimeChoice::OpenXr; ///< --xr-runtime openxr|fake
    std::optional<std::filesystem::path> runtimeJson;   ///< --xr-runtime-json: a runtime's manifest (XR_RUNTIME_JSON)
    /// --xr-render-scale: the eyes' size over the runtime's recommendation, in [kMinRenderScale, kMaxRenderScale]; none:
    /// xr.json's.
    std::optional<double> renderScale;
    /// --xr-refresh-rate <Hz>: the refresh rate asked of the headset (I10 step 9), over xr.json's; none: xr.json's.
    std::optional<float> refreshRate;
    /// --xr-fake-lose-after <frames>: the fake runtime loses its session after this many frames (the desktop
    /// fallback's tests).
    std::optional<std::uint64_t> fakeLoseAfterFrames;
    /// --xr-fake-untracked-frames <frames>: the fake runtime's first frames locate no eyes, as a real runtime's first
    /// ones ask for no views: the render targets' test.
    std::optional<std::uint64_t> fakeUntrackedFrames;

    bool operator==(const XrOptions&) const = default;
};

/// "openxr" or "fake".
[[nodiscard]] std::string_view toString(XrRuntimeChoice runtime) noexcept;

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
    /// --run-report <json>: where the host writes the run report (RunReport, formatRunReport) when the run ends.
    std::optional<std::filesystem::path> runReport;

    /// --model <file>: a glTF model (.gltf or .glb) shown in the model viewer instead of a scene: at the origin, lit by
    /// a three-point light rig, with a camera framing it (makeModelViewerScene). Exclusive with --scene.
    std::optional<std::filesystem::path> model;
    /// --spawn x,y,z[,yawDeg[,pitchDeg]]: the walker starts there instead of at the scene camera (parseSpawnPoint).
    std::optional<SpawnPoint> spawn;
    /// --debug-draw: draw debug lines over the scene: the bounds of every object, the axes of its sockets and a world
    /// grid.
    bool debugDraw = false;

    /// --self-check <name>: rendering check performed after initialization by a frame-limited run (SelfCheck).
    std::optional<SelfCheck> selfCheck;

    /// --devui-open: show the developer overlay (F1) from the first frame, for automation and screenshots. Builds
    /// without the overlay (RV_DEV_UI off) accept it and log that it does nothing; in developer builds a frame-limited
    /// run fails when the overlay cannot start (overlayRequired).
    bool devUiOpen = false;
    /// --screenshot-include-ui: --screenshot and F12 screenshots also show the developer overlay when it is open,
    /// drawn over the scene as in the window, scaled uniformly to fit the screenshot size and centered
    /// (devui::fitOverlayCapture). Without it screenshots show the scene only.
    bool screenshotIncludeUi = false;

    /// --mock-catalog <json>: the quick catalog's games, invented, with their launch targets and emulator profiles
    /// (MockCatalog), for tests: it takes precedence over the ES-DE catalog of the user data directory (I2 step 13),
    /// which runs without it show.
    std::optional<std::filesystem::path> mockCatalog;
    /// --launch-wait-limit <seconds>, frame-limited runs only, where it defaults to kDefaultLaunchWaitLimitSeconds: a
    /// launch still running after this long is ended as left running (never terminated: LaunchFlow::setWaitLimit), so
    /// that the run reaches its --frames and writes its report. Absent in interactive runs, where a game runs as long as
    /// the user plays.
    std::optional<double> launchWaitLimitSeconds;
    static constexpr double kDefaultLaunchWaitLimitSeconds = 120.0;
    static constexpr double kMaxLaunchWaitLimitSeconds = 3600.0;

    // Options of I3, parsed and validated from I3 step 0a on and refused (pendingOptionError) until the step that
    // implements each.
    /// --refresh-library: refresh the catalog from ES-DE once it is open, as "Aggiorna libreria" does (I3 step 13r).
    bool refreshLibrary = false;
    /// --video-freeze-pts <seconds>, frame-limited runs only: the hero CRT's video seeks to this time and holds the
    /// frame, for goldens (ADR-0012 §7; I3 step 11). In [0, kMaxVideoFreezePtsSeconds].
    std::optional<double> videoFreezePtsSeconds;
    static constexpr double kMaxVideoFreezePtsSeconds = 86400.0;
    /// --max-decoders <1|2>: the screens' decoder budget, over video.json's maxDecoders (I5 step 2, ADR-0016 §2: 2 in
    /// Balanced, 1 in Low). None: video.json's.
    std::optional<int> maxDecoders;
    /// --watch-late-frames (tests, I5 step 2's review): the late-frame drop is watched in a --fixed-dt run too (it is
    /// off there by default: the frames are presented on simulated time and decoded on the wall clock).
    bool watchLateFrames = false;
    /// --simulate-minimized FRAME:SECONDS (tests), frame-limited runs only: SimulatedMinimize.
    std::optional<SimulatedMinimize> simulateMinimized;
    /// --no-texture-compression: the collection's texture arrays in RGBA8 at half the resolution instead of BC1, the
    /// fallback without the S3TC extensions (ADR-0011 §4; implemented by I3 step 6: gl::chooseCollectionArrayFormat,
    /// SceneGraphics::collectionArrayFormat).
    bool noTextureCompression = false;
    /// --stereo-preview: the window and the screenshots show a headset's two eyes side by side (I10 step 1, ADR-0017
    /// §4), each an off-axis view of a Quest 3S's field 64 mm apart, culled once for both and sharing the frame's
    /// shadow map: the VR mode's rendering checked on the desktop.
    bool stereoPreview = false;
    /// --vr and the --xr-* options (I10 step 2): the VR mode.
    XrOptions xr;
    /// --start-room <room id>: the room of the building the run starts in (I4 step 5a, ADR-0013 §9: its doorway camera
    /// on its floor; --spawn, the input scripts' poses and the run report's walker poses in its frame), a valid room id
    /// ("room:snes", "area:arcade", ...). Empty: the ground floor's lobby. Only in the generated building's runs.
    std::string startRoom;
    /// --wizard: the first-run wizard in a frame-limited run (ADR-0018 §4), which needs `esdeHome`: no automated run ever
    /// discovers the machine's ES-DE.
    bool wizard = false;
    /// --esde-home <dir>, with --wizard: the ES-DE home, or the folder that holds it as ES-DE's own --home names it;
    /// discovery stands on it and the system probe is never asked.
    std::optional<std::filesystem::path> esdeHome;
    /// --esde-install <dir>, with --esde-home: ES-DE's install folder.
    std::optional<std::filesystem::path> esdeInstall;
    /// --audio device|null|off (I5 step 5, ADR-0016 §9): where the clips' sound goes. None: null in frame-limited runs,
    /// the device otherwise; RV_AUDIO=null or off forces its value over both (resolveAudioOutput).
    std::optional<audio::AudioOutput> audio;
    /// --mute: the run starts muted (toggle_mute, F8, sounds it again).
    bool mute = false;
    /// --safe-bindings (ADR-0018 §7): settings.json's bindings are left out of this run.
    bool safeBindings = false;
    /// --audio-capture <wav> (tests), frame-limited runs only: the null output's mix written to this 32-bit float WAV
    /// at the end of the run (the offline mix the GPU tests analyse).
    std::optional<std::filesystem::path> audioCapture;
};

/// Why `options` cannot run yet: the first option it sets that this build parses but does not implement yet, refused
/// (Unsupported, "not implemented yet (I3 step <n>)", the option as context) so that a run never silently ignores it;
/// ok when it sets none. parseCommandLine() returns this error, so such a run exits 2 like any invalid command line.
[[nodiscard]] Status pendingOptionError(const CommandLineOptions& options);

/// Declares on `parser` the options of the host that every application built on AppHost understands (retroverse.exe,
/// retroverse-asset-preview): --version, the window (--width, --height, --fullscreen, --hidden, --no-vsync), the run
/// length (--smoke, --frames, --exit-after-frames and its alias --exit), --gl-selftest, the directories (--asset-root,
/// --data-dir, --portable), --log-level, --no-dialogs, the screenshots (--screenshot, --screenshot-size), --fixed-dt
/// and --run-report. --help is always accepted (core::ArgParser).
void addHostOptions(core::ArgParser& parser);

/// Validates the host options of `args` (declared by addHostOptions) into the matching fields of `options`. Errors
/// name the offending option:
///  - malformed numbers; --width / --height outside [64, 16384]; --frames below 1;
///  - --frames without --smoke or --exit-after-frames, --hidden without a frame limit (nobody could close the
///    window), --screenshot without a frame limit (there would be no last frame to capture);
///  - --screenshot not ending in ".png"; --screenshot-size other than WIDTHxHEIGHT with sides in [1, 16384];
///  - --fixed-dt that is not a number in (0, kMaxFixedDeltaSeconds];
///  - --run-report not ending in ".json";
///  - --log-level other than trace, debug, info, warn or error; empty paths.
/// Every frame-limited run implies --no-dialogs.
[[nodiscard]] Status applyHostOptions(const core::ParsedArgs& args, CommandLineOptions& options);

/// Parses and validates arguments (UTF-8, program name excluded): the host options (applyHostOptions, with its
/// errors) and those of retroverse.exe. Errors name the offending option:
///  - unknown options, missing values;
///  - --self-check without a frame limit (an unattended check must end on its own) or with an unknown check name;
///  - --scene names with characters other than letters, digits, '_', '-' and '.', or starting with '.';
///  - --model not ending in ".gltf" or ".glb", or given together with --scene;
///  - --spawn that parseSpawnPoint refuses;
///  - --mock-catalog not ending in ".json";
///  - --launch-wait-limit outside (0, kMaxLaunchWaitLimitSeconds], or without --smoke or --exit-after-frames;
///  - --video-freeze-pts outside [0, kMaxVideoFreezePtsSeconds], or without --smoke or --exit-after-frames;
///  - empty paths or names;
///  - a valid option that is not implemented yet (pendingOptionError: none since I3 steps 11 and 13r).
/// The --input-script and --mock-catalog files themselves are read by the application (a bad one fails its
/// initialization).
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
