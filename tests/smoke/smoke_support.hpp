#pragma once

// Helpers of the GPU smoke tests (tests/smoke): the executables under test, the directories of the runs, running an
// executable frame-limited and reading its run report, the checks every run must pass. The helpers that check use
// Catch2 assertions: call them from a test case.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/assets/image_data.hpp>

namespace rv::smoke {

/// What the build under test was configured with (compile definitions of tests/smoke/CMakeLists.txt).
struct BuildConfig {
    bool developerOverlay = false; ///< RV_DEV_UI: the overlay and the asset preview exist
    bool glDebugOutput = false;    ///< RV_GL_DEBUG: runs count OpenGL debug messages
    bool devAssetRoot = false;     ///< RV_DEV_ASSET_ROOT: assets come from the source tree, not deployed ones
};

[[nodiscard]] BuildConfig buildConfig() noexcept;
/// The repository root.
[[nodiscard]] std::filesystem::path sourceDir();
/// <build>/smoke: the directories of the test cases.
[[nodiscard]] std::filesystem::path workRoot();
/// retroverse.exe of this build.
[[nodiscard]] std::filesystem::path retroverseExe();
/// retroverse-asset-preview.exe of this build; empty without the developer overlay.
[[nodiscard]] std::optional<std::filesystem::path> assetPreviewExe();

/// <work root>/<name>, emptied: files of an earlier run must never pass for this one's.
[[nodiscard]] std::filesystem::path freshCaseDir(std::string_view name);

/// Longest a single run may take before it is terminated (a Debug run of the test room takes about 6 s).
inline constexpr std::chrono::seconds kRunTimeout{240};

/// One run of an executable under test.
struct AppRun {
    std::string name;
    int exitCode = -1;
    bool timedOut = false;
    /// Standard output and error of the run (its log): <case>/<name>.log.
    std::filesystem::path outputFile;
    /// The run report: <case>/<name>.json (empty for runCommand), and why it could not be read when it could not.
    std::filesystem::path reportFile;
    std::optional<app::RunReport> report;
    std::string reportError;

    /// The exit code, the outcome and failure of the report and the last lines of the output, for failure messages.
    [[nodiscard]] std::string describe() const;
};

/// Runs `executable` in `caseDir` with `arguments` exactly, its output in <caseDir>/<name>.log, without RV_ASSET_ROOT
/// in its environment (so that each configuration reads its own assets), terminated after kRunTimeout. REQUIREs that
/// the process could be started and did not time out. The run has no report.
[[nodiscard]] AppRun runCommand(const std::filesystem::path& executable, const std::filesystem::path& caseDir,
                                std::string_view name, std::vector<std::string> arguments);

/// runCommand() with --hidden, --data-dir <caseDir>/data and --run-report <caseDir>/<name>.json after `arguments`
/// (which must make the run frame-limited), and the report read back. Checks (I5 step 5) that a report's audio output
/// is null or off, and that every retroverse.exe run that exits 0 reports one.
[[nodiscard]] AppRun runApp(const std::filesystem::path& executable, const std::filesystem::path& caseDir,
                            std::string_view name, std::vector<std::string> arguments);

/// I5 step 5: a run's report says its audio output was null or off (no test plays sound), and a retroverse.exe run
/// (`retroverse`) that exited 0 reports one. runApp() and StartedApp::finish() check every run.
void checkAudioOutput(const AppRun& run, bool retroverse);

/// Skips the running test case (Catch2 SKIP: the test executable exits with 77) when the run could not create an
/// OpenGL 4.5 core profile context: this machine cannot run Retroverse at all.
void skipWithoutOpenGl(const AppRun& run);

/// The checks every run must pass: exit code 0 and a report that says so, every frame of its limit rendered, no high or
/// medium OpenGL debug message (builds with debug output), the assets of its configuration (the source tree in
/// developer builds, the deployed copy next to the executable otherwise).
void requireCleanRun(const AppRun& run);
/// requireCleanRun() of a run given `assetRoot` with --asset-root (its assets from the command line); none: the build's.
void requireCleanRun(const AppRun& run, const std::optional<std::filesystem::path>& assetRoot);

/// Checks that the run wrote `file` at `size` (its report and the PNG) and that the picture shows something: neither
/// uniform nor black. Returns the image (empty when it cannot be read).
assets::ImageData requireScreenshot(const AppRun& run, const std::filesystem::path& file, glm::ivec2 size);

/// "x,y,z,yaw,pitch" for --spawn, every value printed so that it reads back exactly.
[[nodiscard]] std::string spawnArgument(const app::SpawnPoint& pose);

/// A UTF-8 path argument.
[[nodiscard]] std::string arg(const std::filesystem::path& path);

/// Reads the scene file `base` (comments allowed), lets `edit` change it and writes the result to `target` (without
/// comments): a variant of a test scene that differs in one setting.
void writeSceneVariant(const std::filesystem::path& base, const std::filesystem::path& target,
                       void (*edit)(nlohmann::json& scene));

} // namespace rv::smoke
