#include "smoke_support.hpp"

#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include "image_checks.hpp"
#include "support/child_process.hpp"

namespace rv::smoke {
namespace {

/// Lines of a run's output quoted in failure messages.
constexpr std::size_t kQuotedOutputLines = 25;
/// A screenshot "shows something" when its luma spans at least this much (a uniform frame spans the dither, 1 or 2)...
constexpr double kMinScreenshotLumaRange = 32.0;
/// ... and averages at least this luma (a black frame averages 0).
constexpr double kMinScreenshotMeanLuma = 4.0;

/// Both paths name the same existing file or directory.
bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    return std::filesystem::equivalent(a, b, error) && !error;
}

/// The last `count` lines of a text file.
std::string tail(const std::filesystem::path& file, std::size_t count) {
    const auto text = core::readTextFile(file);
    if (!text) {
        return "(no output: " + text.error().toString() + ")";
    }
    std::vector<std::string_view> lines;
    std::string_view rest(*text);
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        lines.push_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    const std::size_t first = lines.size() > count ? lines.size() - count : 0;
    std::string quoted;
    for (std::size_t i = first; i < lines.size(); ++i) {
        quoted += "    ";
        quoted += lines[i];
        quoted += '\n';
    }
    return quoted;
}

} // namespace

BuildConfig buildConfig() noexcept {
    BuildConfig config;
    config.developerOverlay = RV_SMOKE_DEVELOPER_BUILD != 0;
    config.glDebugOutput = RV_SMOKE_GL_DEBUG != 0;
    config.devAssetRoot = RV_SMOKE_DEV_ASSET_ROOT != 0;
    return config;
}

std::filesystem::path sourceDir() {
    return core::pathFromUtf8(RV_SMOKE_SOURCE_DIR);
}

std::filesystem::path workRoot() {
    return core::pathFromUtf8(RV_SMOKE_WORK_DIR);
}

std::filesystem::path retroverseExe() {
    return core::pathFromUtf8(RV_SMOKE_RETROVERSE);
}

std::optional<std::filesystem::path> assetPreviewExe() {
#if defined(RV_SMOKE_ASSET_PREVIEW)
    return core::pathFromUtf8(RV_SMOKE_ASSET_PREVIEW);
#else
    return std::nullopt;
#endif
}

std::filesystem::path freshCaseDir(std::string_view name) {
    const std::filesystem::path dir = workRoot() / core::pathFromUtf8(name);
    INFO("case directory " << core::pathToUtf8(dir));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    INFO(error.message());
    REQUIRE_FALSE(error);
    std::filesystem::create_directories(dir, error);
    INFO(error.message());
    REQUIRE_FALSE(error);
    return dir;
}

std::string AppRun::describe() const {
    std::string text = fmt::format("run '{}': exit code {}{}", name, exitCode, timedOut ? " (timed out)" : "");
    if (report) {
        text += fmt::format(", outcome '{}'", report->outcome);
        if (report->failure) {
            text += ", failure: " + *report->failure;
        }
    } else if (reportFile.empty()) {
        text += ", no run report (" + reportError + ")";
    } else {
        text += fmt::format(", no run report at {} ({})", core::pathToUtf8(reportFile), reportError);
    }
    text += fmt::format("\n  log {}, last lines:\n{}", core::pathToUtf8(outputFile),
                        tail(outputFile, kQuotedOutputLines));
    return text;
}

AppRun runCommand(const std::filesystem::path& executable, const std::filesystem::path& caseDir, std::string_view name,
                  std::vector<std::string> arguments) {
    AppRun run;
    run.name = std::string(name);
    run.outputFile = caseDir / core::pathFromUtf8(run.name + ".log");
    run.reportError = "the run writes none";

    test::ChildRequest request;
    request.executable = executable;
    request.arguments = std::move(arguments);
    request.workingDirectory = caseDir;
    // One log: standard output and error in the order they were written.
    request.stdoutFile = run.outputFile;
    request.stderrFile = run.outputFile;
    // Each configuration must read its own assets: the source tree, or the copy deployed next to a Release executable.
    request.environment = {{"RV_ASSET_ROOT", std::nullopt}};
    request.timeout = kRunTimeout;
    const auto outcome = test::runChild(request);
    if (!outcome) {
        FAIL("could not run " << core::pathToUtf8(executable) << ": " << outcome.error().toString());
    }
    run.exitCode = outcome->exitCode;
    run.timedOut = outcome->timedOut;
    INFO(run.describe());
    REQUIRE_FALSE(run.timedOut);
    return run;
}

AppRun runApp(const std::filesystem::path& executable, const std::filesystem::path& caseDir, std::string_view name,
              std::vector<std::string> arguments) {
    const std::filesystem::path reportFile = caseDir / core::pathFromUtf8(std::string(name) + ".json");
    arguments.insert(arguments.end(),
                     {"--hidden", "--data-dir", arg(caseDir / "data"), "--run-report", arg(reportFile)});
    AppRun run = runCommand(executable, caseDir, name, std::move(arguments));
    run.reportFile = reportFile;
    if (auto report = app::readRunReport(reportFile)) {
        run.report = std::move(*report);
        run.reportError.clear();
    } else {
        run.reportError = report.error().toString();
    }
    return run;
}

void skipWithoutOpenGl(const AppRun& run) {
    if (run.report && run.report->openGlUnavailable) {
        SKIP("no OpenGL 4.5 core profile context on this machine: " << run.report->failure.value_or("(no detail)"));
    }
}

void requireCleanRun(const AppRun& run) {
    INFO(run.describe());
    REQUIRE(run.report.has_value());
    const app::RunReport& report = *run.report;
    REQUIRE(run.exitCode == 0);
    CHECK(report.exitCode == 0);
    CHECK(report.outcome == "ok");
    CHECK_FALSE(report.failure.has_value());
    REQUIRE(report.frameLimit.has_value());
    CHECK(report.framesRendered == *report.frameLimit);
    REQUIRE(report.context.has_value());

    const BuildConfig config = buildConfig();
    CHECK(report.developerOverlay == config.developerOverlay);
    CHECK(report.glDebugOutput == config.glDebugOutput);
    if (report.glSelfTest) {
        CHECK(report.glSelfTest->passed());
    }
    if (config.glDebugOutput) {
        REQUIRE(report.debugMessages.has_value());
        // What --gl-selftest provoked is counted too: high severity on the reference driver, maybe medium or low on
        // another. Any other high or medium message is an error.
        const std::uint64_t provokedHigh = report.glSelfTest ? report.glSelfTest->highSeverityCaptured : 0;
        const std::uint64_t provokedOther = report.glSelfTest ? report.glSelfTest->messagesCaptured - provokedHigh : 0;
        CHECK(report.debugMessages->high == provokedHigh);
        CHECK(report.debugMessages->medium <= provokedOther);
    } else {
        CHECK_FALSE(report.debugMessages.has_value());
    }
    // The sections of I3 are in every report (readRunReport requires them) and empty until the steps that fill them:
    // each step changes its own line here.
    CHECK(report.world == app::WorldRecord{});         // I3 step 10b
    CHECK(report.residency == app::ResidencyRecord{}); // I3 step 12
    CHECK(report.video == app::VideoRecord{});         // I3 step 11
    CHECK(report.phases.empty());                      // I3 step 0d
    CHECK(report.gpuMemory == app::GpuMemoryRecord{}); // I3 step 0d
    if (config.devAssetRoot) {
        CHECK(report.assetRootSource == "source tree");
        CHECK(samePath(report.assetRoot, sourceDir() / "assets"));
    } else {
        // A Release build renders from the assets deployed next to its executable.
        CHECK(report.assetRootSource == "executable directory");
        CHECK(samePath(report.assetRoot, retroverseExe().parent_path() / "assets"));
    }
}

assets::ImageData requireScreenshot(const AppRun& run, const std::filesystem::path& file, glm::ivec2 size) {
    INFO(run.describe());
    REQUIRE(run.report.has_value());
    REQUIRE(run.report->screenshot.has_value());
    CHECK(samePath(run.report->screenshot->file, file));
    CHECK(run.report->screenshot->width == size.x);
    CHECK(run.report->screenshot->height == size.y);
    auto image = assets::loadImage(file);
    INFO("screenshot " << core::pathToUtf8(file));
    REQUIRE(image.has_value());
    CHECK(image->width == size.x);
    CHECK(image->height == size.y);
    CHECK(image->channels == 3);
    const LumaSummary luma = lumaSummary(*image);
    INFO("luma mean " << luma.mean << ", from " << luma.minimum << " to " << luma.maximum);
    CHECK(luma.maximum - luma.minimum >= kMinScreenshotLumaRange); // not uniform
    CHECK(luma.mean >= kMinScreenshotMeanLuma);                    // not black
    return std::move(*image);
}

std::string spawnArgument(const app::SpawnPoint& pose) {
    return fmt::format("{:.9g},{:.9g},{:.9g},{:.9g},{:.9g}", pose.feet.x, pose.feet.y, pose.feet.z,
                       pose.yawDeg.value_or(0.0f), pose.pitchDeg.value_or(0.0f));
}

std::string arg(const std::filesystem::path& path) {
    return core::pathToUtf8(path);
}

void writeSceneVariant(const std::filesystem::path& base, const std::filesystem::path& target,
                       void (*edit)(nlohmann::json& scene)) {
    auto scene = core::readJsonFile(base);
    INFO("scene " << core::pathToUtf8(base));
    REQUIRE(scene.has_value());
    edit(*scene);
    REQUIRE(core::writeTextFileAtomic(target, scene->dump(2)).has_value());
}

} // namespace rv::smoke
