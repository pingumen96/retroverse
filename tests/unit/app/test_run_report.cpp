#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/text.hpp>

#include "support/temp_dir.hpp"

using rv::ErrorCode;
using rv::app::RunReport;

namespace {

/// A report with every field set, as a frame-limited run of retroverse.exe with --gl-selftest, a screenshot and a
/// self-check would write it.
RunReport fullReport() {
    RunReport report;
    report.application = "Retroverse";
    report.version = "0.1.0";
    report.buildType = "Debug";
    report.developerOverlay = true;
    report.glDebugOutput = true;
    report.assetRoot = rv::core::pathFromUtf8("E:/Progetti/retroverse/assets");
    report.assetRootSource = "source tree";
    report.exitCode = 3;
    report.outcome = "runtime failure";
    report.failure = "Run failed: ValidationFailed: --screenshot was not written";
    report.openGlUnavailable = false;
    report.context = rv::app::RunReportContext{"NVIDIA Corporation", "NVIDIA GeForce RTX 2080 SUPER/PCIe/SSE2",
                                               "4.5.0 NVIDIA 617.42", "4.50 NVIDIA"};
    report.debugMessages = rv::app::DebugMessageCounts{1, 2, 5};
    // The GL_INVALID_ENUM of --gl-selftest, counted in debugMessages too.
    report.glSelfTest = rv::app::SelfTestOutcome{true, true, 1, 1};
    report.frameLimit = 30;
    report.framesRendered = 30;
    rv::gl::FrameStats view;
    view.items = 231;
    view.culledItems = 6;
    view.batches = 129;
    view.drawCalls = 129;
    view.instances = 225;
    view.triangles = 18352;
    view.shaderSwitches = 2;
    view.materialSwitches = 113;
    view.debugLines = 40;
    view.shadowDrawCalls = 101;
    view.shadowInstances = 179;
    view.depthDrawCalls = 129;
    view.shadowGpuMilliseconds = 0.0625;
    view.sceneGpuMilliseconds = 0.33;
    report.lastView = view;
    report.instanceCapacity = 2048;
    report.screenshot = rv::app::ScreenshotRecord{rv::core::pathFromUtf8("build/screenshots/room.png"), 1920, 1080};
    rv::app::SpawnPoint pose;
    pose.feet = glm::vec3(0.1f, 0.0f, -3.2000003f);
    pose.yawDeg = -179.99998f;
    pose.pitchDeg = -6.0000005f;
    report.finalCamera = pose;
    report.selfCheck = rv::app::SelfCheckRecord{rv::app::SelfCheck::Bloom, true, "emitter keeps 98.6% of its energy"};
    return report;
}

void checkSameStats(const rv::gl::FrameStats& a, const rv::gl::FrameStats& b) {
    CHECK(a.items == b.items);
    CHECK(a.culledItems == b.culledItems);
    CHECK(a.batches == b.batches);
    CHECK(a.drawCalls == b.drawCalls);
    CHECK(a.instances == b.instances);
    CHECK(a.triangles == b.triangles);
    CHECK(a.shaderSwitches == b.shaderSwitches);
    CHECK(a.materialSwitches == b.materialSwitches);
    CHECK(a.debugLines == b.debugLines);
    CHECK(a.shadowDrawCalls == b.shadowDrawCalls);
    CHECK(a.shadowInstances == b.shadowInstances);
    CHECK(a.depthDrawCalls == b.depthDrawCalls);
    CHECK(a.shadowGpuMilliseconds == b.shadowGpuMilliseconds);
    CHECK(a.depthGpuMilliseconds == b.depthGpuMilliseconds);
    CHECK(a.ssaoGpuMilliseconds == b.ssaoGpuMilliseconds);
    CHECK(a.sceneGpuMilliseconds == b.sceneGpuMilliseconds);
    CHECK(a.postGpuMilliseconds == b.postGpuMilliseconds);
}

/// formatRunReport of `report` with `edit` applied to the JSON.
template <class Edit>
std::string editedReport(const RunReport& report, Edit edit) {
    nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(report));
    edit(json);
    return json.dump();
}

} // namespace

TEST_CASE("app/run_report: a full report reads back field for field", "[app][run_report]") {
    const RunReport report = fullReport();
    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "report.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->application == report.application);
    CHECK(parsed->version == report.version);
    CHECK(parsed->buildType == report.buildType);
    CHECK(parsed->developerOverlay);
    CHECK(parsed->glDebugOutput);
    CHECK(parsed->assetRoot == report.assetRoot);
    CHECK(parsed->assetRootSource == report.assetRootSource);
    CHECK(parsed->exitCode == 3);
    CHECK(parsed->outcome == report.outcome);
    CHECK(parsed->failure == report.failure);
    CHECK_FALSE(parsed->openGlUnavailable);
    CHECK(parsed->context == report.context);
    CHECK(parsed->debugMessages == report.debugMessages);
    CHECK(parsed->glSelfTest == report.glSelfTest);
    CHECK(parsed->frameLimit == report.frameLimit);
    CHECK(parsed->framesRendered == report.framesRendered);
    REQUIRE(parsed->lastView.has_value());
    checkSameStats(*parsed->lastView, *report.lastView);
    CHECK(parsed->instanceCapacity == report.instanceCapacity);
    CHECK(parsed->screenshot == report.screenshot);
    CHECK(parsed->selfCheck == report.selfCheck);
    // The final pose reads back bit for bit, so that --spawn of it reproduces the view.
    CHECK(parsed->finalCamera == report.finalCamera);
}

TEST_CASE("app/run_report: optional parts are left out and read back as absent", "[app][run_report]") {
    RunReport report;
    report.application = "Retroverse Asset Preview";
    report.version = "0.1.0";
    report.buildType = "Release";
    report.exitCode = 2;
    report.outcome = "initialization failed";
    report.openGlUnavailable = true;
    const std::string text = rv::app::formatRunReport(report);
    const nlohmann::json json = nlohmann::json::parse(text);
    for (const char* key :
         {"failure", "context", "debugMessages", "glSelfTest", "lastView", "screenshot", "finalCamera", "selfCheck"}) {
        CAPTURE(key);
        CHECK_FALSE(json.contains(key));
    }
    CHECK_FALSE(json.at("frames").contains("limit"));

    const auto parsed = rv::app::parseRunReport(text, "report.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->openGlUnavailable);
    CHECK(parsed->exitCode == 2);
    CHECK(parsed->assetRoot.empty());
    CHECK_FALSE(parsed->failure.has_value());
    CHECK_FALSE(parsed->context.has_value());
    CHECK_FALSE(parsed->debugMessages.has_value());
    CHECK_FALSE(parsed->glSelfTest.has_value());
    CHECK_FALSE(parsed->frameLimit.has_value());
    CHECK(parsed->framesRendered == 0);
    CHECK_FALSE(parsed->lastView.has_value());
    CHECK_FALSE(parsed->screenshot.has_value());
    CHECK_FALSE(parsed->finalCamera.has_value());
    CHECK_FALSE(parsed->selfCheck.has_value());
}

TEST_CASE("app/run_report: unknown keys are ignored, a newer writer may add some", "[app][run_report]") {
    const std::string text = editedReport(fullReport(), [](nlohmann::json& json) {
        json["benchmark"] = {{"p99Milliseconds", 4.2}};
        json["lastView"]["meshletCount"] = 12;
    });
    const auto parsed = rv::app::parseRunReport(text, "report.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->lastView->drawCalls == 129);
}

TEST_CASE("app/run_report: malformed reports are errors that point at the key", "[app][run_report]") {
    const RunReport report = fullReport();
    const auto parseEdited = [&](auto edit) { return rv::app::parseRunReport(editedReport(report, edit), "r.json"); };

    const auto version = parseEdited([](nlohmann::json& json) { json["schemaVersion"] = 2; });
    REQUIRE_FALSE(version.has_value());
    CHECK(version.error().code == ErrorCode::ValidationFailed);
    CHECK(version.error().context == "r.json:/schemaVersion");

    const auto missing = parseEdited([](nlohmann::json& json) { json.erase("exitCode"); });
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().context == "r.json:/exitCode");

    const auto wrongType = parseEdited([](nlohmann::json& json) { json["lastView"]["drawCalls"] = "129"; });
    REQUIRE_FALSE(wrongType.has_value());
    CHECK(wrongType.error().context == "r.json:/lastView/drawCalls");

    const auto negative = parseEdited([](nlohmann::json& json) { json["debugMessages"]["medium"] = -1; });
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().context == "r.json:/debugMessages/medium");

    const auto selfTestType = parseEdited([](nlohmann::json& json) { json["glSelfTest"]["errorFlagRaised"] = "yes"; });
    REQUIRE_FALSE(selfTestType.has_value());
    CHECK(selfTestType.error().context == "r.json:/glSelfTest/errorFlagRaised");

    // The high-severity messages the self-test provoked are a part of all it provoked.
    const auto selfTestCounts =
        parseEdited([](nlohmann::json& json) { json["glSelfTest"]["highSeverityCaptured"] = 2; });
    REQUIRE_FALSE(selfTestCounts.has_value());
    CHECK(selfTestCounts.error().context == "r.json:/glSelfTest/highSeverityCaptured");

    const auto camera = parseEdited([](nlohmann::json& json) { json["finalCamera"]["feet"] = {1.0, 2.0}; });
    REQUIRE_FALSE(camera.has_value());
    CHECK(camera.error().context == "r.json:/finalCamera/feet");

    const auto check = parseEdited([](nlohmann::json& json) { json["selfCheck"]["name"] = "gamma"; });
    REQUIRE_FALSE(check.has_value());
    CHECK(check.error().context == "r.json:/selfCheck/name");

    const auto frames = parseEdited([](nlohmann::json& json) { json.erase("frames"); });
    REQUIRE_FALSE(frames.has_value());
    CHECK(frames.error().context == "r.json:/frames");

    const auto notAnObject = rv::app::parseRunReport("[1, 2]", "r.json");
    REQUIRE_FALSE(notAnObject.has_value());
    CHECK(notAnObject.error().code == ErrorCode::ValidationFailed);

    const auto malformed = rv::app::parseRunReport("{\"schemaVersion\": ", "r.json");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == ErrorCode::ParseError);
}

TEST_CASE("app/run_report: the quick catalog and its launches read back record for record", "[app][run_report]") {
    RunReport report = fullReport();
    rv::app::CatalogRecord catalog;
    catalog.source = "mock";
    catalog.file = rv::core::pathFromUtf8("build/smoke/launch/catalog.json");
    catalog.games = 13;
    catalog.opened = 2;
    catalog.open = true;
    catalog.selectedPosition = 4;
    catalog.selectedId = "g000000000000a005";
    catalog.cardSqueezed = {"rows"};
    catalog.listed = 9;
    catalog.shownCount = 9;
    report.catalog = catalog;
    rv::app::LaunchRecord returned;
    returned.gameId = "g000000000000a001";
    returned.profile = "fake-return";
    returned.state = "Returned";
    returned.outcome = "returned";
    returned.detail = "0 (success)";
    returned.exitCode = 0;
    returned.strategy = "DirectProcess";
    returned.processId = 37792;
    returned.requestFrame = 8;
    returned.startFrame = 9;
    returned.endFrame = 9;
    returned.idleWakes = 26;
    returned.startMs = 35.5929;
    returned.runMs = 2548.2331;
    returned.returnMs = 1.3926;
    returned.foregroundMs = 74.25;
    rv::app::LaunchRecord stillRunning;
    stillRunning.gameId = "g000000000000a007";
    stillRunning.profile = "fake-still-running";
    stillRunning.state = "Returned";
    stillRunning.outcome = "left-running";
    stillRunning.reason = "confirmed";
    stillRunning.exitCode = 0xC0000005u;
    stillRunning.strategy = "DirectProcess";
    stillRunning.processesRemained = true;
    stillRunning.confirmedByUser = true;
    stillRunning.wasStillRunning = true;
    stillRunning.quickExit = true;
    stillRunning.quitDuringGame = true;
    stillRunning.waitLimitReached = true;
    stillRunning.requestFrame = 30;
    stillRunning.startFrame = 31;
    stillRunning.endFrame = 36;
    stillRunning.framesWithPanel = 5;
    stillRunning.panels = {"still-running", "outcome"};
    rv::app::LaunchRecord blocked;
    blocked.gameId = "g000000000000a009";
    blocked.profile = "fake-unset-variable";
    blocked.state = "Blocked";
    blocked.outcome = "not-configured";
    blocked.reason = "template";
    blocked.detail = "{var:core}";
    blocked.requestFrame = 50;
    report.launches = {returned, stillRunning, blocked};

    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "report.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->catalog == report.catalog);
    REQUIRE(parsed->launches.size() == 3);
    CHECK(parsed->launches[0] == returned);
    CHECK(parsed->launches[1] == stillRunning);
    CHECK(parsed->launches[2] == blocked);
    // Optional parts of a record are left out: a blocked launch started nothing.
    const nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(report));
    CHECK_FALSE(json.at("launches").at(2).contains("exitCode"));
    CHECK_FALSE(json.at("launches").at(2).at("frames").contains("start"));
    CHECK(json.at("launches").at(2).at("times").empty());

    // A malformed record points at its field.
    const auto wrong = rv::app::parseRunReport(editedReport(report,
                                                            [](nlohmann::json& edited) {
                                                                edited["launches"][1]["frames"]["withPanel"] = "5";
                                                            }),
                                               "r.json");
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().context == "r.json:/launches/1/frames/withPanel");
    const auto panels = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["launches"][0]["panels"] = {1}; }), "r.json");
    REQUIRE_FALSE(panels.has_value());
    CHECK(panels.error().context == "r.json:/launches/0/panels");
}

TEST_CASE("app/run_report: runs without a catalog or launches write neither key", "[app][run_report]") {
    const RunReport report = fullReport();
    const nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(report));
    CHECK_FALSE(json.contains("catalog"));
    CHECK_FALSE(json.contains("launches"));
    const auto parsed = rv::app::parseRunReport(json.dump(), "report.json");
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->catalog.has_value());
    CHECK(parsed->launches.empty());
}

TEST_CASE("app/run_report: written atomically and read back from a file", "[app][run_report]") {
    const rv::test::TempDir dir("rv-run-report");
    const auto path = dir.file("nested/run report.json");
    REQUIRE(rv::app::writeRunReport(path, fullReport()).has_value());
    const auto read = rv::app::readRunReport(path);
    REQUIRE(read.has_value());
    CHECK(read->outcome == "runtime failure");

    const auto absent = rv::app::readRunReport(dir.file("absent.json"));
    REQUIRE_FALSE(absent.has_value());
}

TEST_CASE("app/run_report: --run-report takes a .json file", "[app][run_report][command_line]") {
    const auto none = rv::app::parseCommandLine(std::vector<std::string>{});
    REQUIRE(none.has_value());
    CHECK_FALSE(none->runReport.has_value());

    const auto given = rv::app::parseCommandLine(
        std::vector<std::string>{"--smoke", "--hidden", "--run-report", "build/smoke/run report.JSON"});
    REQUIRE(given.has_value());
    CHECK(given->runReport == rv::core::pathFromUtf8("build/smoke/run report.JSON"));

    // Interactive runs write one at exit too.
    const auto interactive = rv::app::parseCommandLine(std::vector<std::string>{"--run-report=report.json"});
    REQUIRE(interactive.has_value());
    CHECK(interactive->runReport == rv::core::pathFromUtf8("report.json"));

    const auto wrongExtension = rv::app::parseCommandLine(std::vector<std::string>{"--run-report", "report.txt"});
    REQUIRE_FALSE(wrongExtension.has_value());
    CHECK(wrongExtension.error().code == ErrorCode::InvalidArgument);
    CHECK(wrongExtension.error().context == "--run-report=report.txt");

    const auto empty = rv::app::parseCommandLine(std::vector<std::string>{"--run-report="});
    REQUIRE_FALSE(empty.has_value());
}
