#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/text.hpp>
#include <rv/render/gpu_memory.hpp>

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
    CHECK(a.outlineGpuMilliseconds == b.outlineGpuMilliseconds);
    CHECK(a.staticGroups == b.staticGroups);
    CHECK(a.staticBatches == b.staticBatches);
    CHECK(a.clusters == b.clusters);
    CHECK(a.culledClusters == b.culledClusters);
    CHECK(a.staticDrawCalls == b.staticDrawCalls);
    CHECK(a.staticInstances == b.staticInstances);
    CHECK(a.outlinedInstances == b.outlinedInstances);
}

/// formatRunReport of `report` with `edit` applied to the JSON.
template <class Edit>
std::string editedReport(const RunReport& report, Edit edit) {
    nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(report));
    edit(json);
    return json.dump();
}

/// A report with the measurements of step 0d: a 600-frame run with two phases, two CPU sections and some GPU memory.
RunReport measuredReport() {
    RunReport report = fullReport();
    report.frameLimit = 600;
    report.framesRendered = 600;
    report.frameTimes = rv::app::FrameTimesRecord{30, rv::app::TimeSummary{570, 6.5, 7.25, 9.0, 31.5}};
    report.phases = {rv::app::PhaseRecord{"benchmark", 1, false, 300, rv::app::TimeSummary{300, 6.25, 7.0, 8.5, 12.0}},
                     // Opened twice within the warm-up and still open: frames, but no time.
                     rv::app::PhaseRecord{"refresh", 2, true, 12, std::nullopt}};
    report.cpuSections = {rv::app::CpuSectionRecord{"productUi", rv::app::TimeSummary{600, 0.125, 0.25, 0.5, 2.0}},
                          rv::app::CpuSectionRecord{"sceneSubmit", rv::app::TimeSummary{600, 0.5, 0.75, 1.0, 4.0}}};
    rv::app::GpuMemoryRecord memory;
    memory.totalBytes = 83886080 + 60000000 + 4096;
    memory.peakTotalBytes = 83886080 + 70000000 + 4096;
    memory.kinds[static_cast<std::size_t>(rv::gl::GpuMemoryKind::ProceduralSurfaces)] = {83886080, 83886080, 23};
    memory.kinds[static_cast<std::size_t>(rv::gl::GpuMemoryKind::Targets)] = {60000000, 70000000, 14};
    memory.kinds[static_cast<std::size_t>(rv::gl::GpuMemoryKind::Buffers)] = {4096, 4096, 3};
    report.gpuMemory = memory;
    return report;
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

TEST_CASE("app/run_report: the sections of I3 are always written and required", "[app][run_report]") {
    // Every report holds them, the minimal one of a failed initialization included.
    RunReport minimal;
    minimal.application = "Retroverse";
    minimal.exitCode = 2;
    minimal.outcome = "initialization failed";
    const nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(minimal));
    // Empty in a run without what they record.
    for (const char* key : {"world", "residency", "video"}) {
        CAPTURE(key);
        REQUIRE(json.contains(key));
        CHECK(json.at(key).is_object());
        CHECK(json.at(key).empty());
    }
    // Step 0d's: no frame measured, no phase, no section, nothing allocated.
    for (const char* key : {"phases", "cpuSections"}) {
        CAPTURE(key);
        REQUIRE(json.contains(key));
        CHECK(json.at(key).is_array());
        CHECK(json.at(key).empty());
    }
    CHECK(json.at("frameTimes") == nlohmann::json{{"warmUpFrames", 0}, {"count", 0}});
    CHECK(json.at("gpuMemory").at("totalBytes") == 0);
    CHECK(json.at("gpuMemory").at("kinds").size() == rv::gl::kGpuMemoryKindCount);

    const RunReport report = measuredReport();
    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->world == rv::app::WorldRecord{});
    CHECK(parsed->residency == rv::app::ResidencyRecord{});
    CHECK(parsed->video == rv::app::VideoRecord{});

    // A reader refuses a report without one, or with one of the wrong type.
    for (const char* key : {"world", "residency", "video", "frameTimes", "phases", "cpuSections", "gpuMemory"}) {
        CAPTURE(key);
        const auto missing = rv::app::parseRunReport(
            editedReport(report, [key](nlohmann::json& edited) { edited.erase(key); }), "r.json");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().context == std::string("r.json:/") + key);
        const auto wrongType = rv::app::parseRunReport(
            editedReport(report, [key](nlohmann::json& edited) { edited[key] = 1; }), "r.json");
        REQUIRE_FALSE(wrongType.has_value());
        CHECK(wrongType.error().code == ErrorCode::ValidationFailed);
    }
    const auto unnamed = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["phases"][0].erase("name"); }), "r.json");
    REQUIRE_FALSE(unnamed.has_value());
    CHECK(unnamed.error().context == "r.json:/phases/0/name");
}

TEST_CASE("app/run_report: the generated room and its layers read back field for field (I3 step 10b)",
          "[app][run_report]") {
    RunReport report = measuredReport();
    rv::app::WorldRecord& world = report.world;
    world.bound = true;
    world.state = "room";
    world.notice = "media-unknown-kept";
    world.reason = "the media are unknown";
    world.catalogState = "";
    world.room = "room:snes";
    world.library = "esde";
    world.outcome = "updated";
    world.changeOutcome = "updated";
    world.rebuild = "local-reflow";
    world.planDigest = std::string(64, 'a');
    world.planWritten = true;
    world.saveError = "";
    world.placements = 51;
    world.featured = 44;
    world.band = 6;
    world.spine = 1;
    world.unplaced = 5;
    world.duplicateTitle = 2;
    world.noCover = 3;
    world.unavailable = 0;
    world.capacity = 0;
    world.added = 1;
    world.kept = 50;
    world.hardFindings = 0;
    world.softFindings = 2;
    world.objects = 42;
    world.lights = 6;
    world.undressed = 0;
    world.items = 51;
    world.clusters = 5;
    world.skipped = 0;
    world.staticRecords = 153;
    world.pickTargets = 51;
    world.snapshotAtMs = 812.5;
    world.committedAtMs = 1203.25;
    world.snapshotMs = 6.5;
    world.buildMs = 120.75;
    world.commitMs = 45.5;
    world.snapshotToCommitMs = 390.75;
    world.firstFrame = 0;
    world.hoverChanges = 3;
    world.hoveredPickId = "0100C00C";
    world.hoveredGame = "g000000000000b002";
    rv::app::ResidencyRecord& residency = report.residency;
    residency.bound = true;
    residency.format = "bc1-srgb";
    residency.arrayBytes = 20103168;
    residency.items = 51;
    residency.frontsResident = 51;
    residency.spinesResident = 50;
    residency.pending = 1;
    residency.covers = 48;
    residency.cachedCovers = 1;
    residency.labels = 2;
    residency.coverFailures = 2;
    residency.failed = 0;
    residency.cacheHits = 99;
    residency.cacheHitsWithoutSource = 1;
    residency.cacheMisses = 4;
    residency.cacheStale = 1;
    residency.cacheCorrupt = 0;
    residency.cacheStored = 2;
    residency.cacheStoreFailures = 0;
    residency.shownAtMs = 1203.25;
    residency.allResidentAtMs = 1500.5;
    residency.residentAfterShownMs = 297.25;
    residency.allResidentFrame = 18;
    residency.uploadRounds = 4;
    residency.maxLayersPerRound = 64;
    // I3 step 12: the loader's order, budget, waits, pause, retries and hero textures.
    residency.viewItems = 12;
    residency.viewResidentAfterShownMs = 40.5;
    residency.residentAtViewResident = 14;
    residency.inViewAmongFirst = 12;
    residency.arrivalDistanceQuarters = {1.5, 2.25, 3.0, 6.75};
    residency.overBudgetRounds = 1;
    residency.maxRoundMs = 1.25;
    residency.lockstepWaits = 2;
    residency.lockstepWaitMs = 30.5;
    residency.pausedRounds = 7;
    residency.retryRounds = 1;
    residency.retriedItems = 3;
    residency.retriedChanged = 2;
    residency.adoptedItems = 50;
    residency.adoptedChanged = 1;
    residency.hero.requests = 3;
    residency.hero.loaded = 2;
    residency.hero.failed = 1;
    residency.hero.evicted = 0;
    residency.hero.resident = 2;
    residency.hero.bytes = 3616280;
    residency.hero.peakBytes = 3616280;
    residency.hero.maxUploadMs = 0.75;
    residency.hero.shownGame = "g000000000000b002";
    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->world == world);
    CHECK(parsed->residency == residency);

    // Optional values left out read back as absent; a bound record needs its fields.
    world.firstFrame.reset();
    world.committedAtMs.reset();
    residency.allResidentAtMs.reset();
    residency.allResidentFrame.reset();
    residency.viewResidentAfterShownMs.reset();
    const auto sparse = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(sparse.has_value());
    CHECK(sparse->world == world);
    CHECK(sparse->residency == residency);
    const nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(report));
    CHECK_FALSE(json.at("world").contains("firstFrame"));
    CHECK(json.at("world").at("placements").at("featured") == 44);
    const auto missing = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["world"]["unplaced"].erase("capacity"); }), "r.json");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().context == "r.json:/world/unplaced/capacity");
    const auto badCount = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["residency"]["cache"]["hits"] = -1; }), "r.json");
    REQUIRE_FALSE(badCount.has_value());
    CHECK(badCount.error().context == "r.json:/residency/cache/hits");
}

TEST_CASE("app/run_report: the persisted start, its reconciliation and the object fallbacks read back (I3 step 10c)",
          "[app][run_report]") {
    RunReport report = measuredReport();
    rv::app::WorldRecord& world = report.world;
    world.bound = true;
    world.state = "room";
    world.room = "room:snes";
    world.library = "esde";
    world.outcome = "unchanged";
    world.planDigest = std::string(64, 'b');
    world.firstFrame = 0;
    rv::app::WorldPreviewRecord preview;
    preview.frame = 0;
    preview.committedAtMs = 950.5;
    preview.buildMs = 20.25;
    preview.commitMs = 48.5;
    preview.planDigest = std::string(64, 'b');
    preview.items = 50;
    preview.itemsKnown = 50;
    preview.frontsResident = 50;
    preview.spinesResident = 50;
    world.preview = preview;
    world.reconcile = rv::app::WorldReconcileRecord{"textures", 1, 50, 0, 0, 0, 0, true};
    world.itemsWritten = true;
    world.screenGames = 52;
    world.objectFallbacks = {{"furniture.shelf.hero_module.generic", "family generic", "the manifest is missing"},
                             {"decor.bookends.generic", "family procedural", "the manifest is missing"}};
    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->world == world);

    // A run without a stored plan has neither the preview nor the reconciliation; a preview not yet rendered has no
    // residency; the fallbacks are an array that must be there.
    world.preview->frontsResident.reset();
    world.preview->spinesResident.reset();
    world.preview->committedAtMs.reset();
    const auto unrendered = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(unrendered.has_value());
    CHECK(unrendered->world == world);
    world.preview.reset();
    world.reconcile.reset();
    world.objectFallbacks.clear();
    const auto none = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(none.has_value());
    CHECK(none->world == world);
    const nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(report));
    CHECK_FALSE(json.at("world").contains("preview"));
    CHECK(json.at("world").at("objectFallbacks").is_array());
    const auto missing = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["world"].erase("objectFallbacks"); }), "r.json");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().context == "r.json:/world/objectFallbacks");
    const auto badKind = rv::app::parseRunReport(editedReport(report,
                                                              [](nlohmann::json& edited) {
                                                                  edited["world"]["reconcile"] = nlohmann::json{
                                                                      {"kind", "textures"}, {"frame", 1}};
                                                              }),
                                                 "r.json");
    REQUIRE_FALSE(badKind.has_value());
    CHECK(badKind.error().context == "r.json:/world/reconcile/kept");
}

TEST_CASE("app/run_report: the hero CRT's video reads back field for field (I3 step 11)", "[app][run_report]") {
    RunReport report = measuredReport();
    rv::app::VideoRecord& video = report.video;
    video.bound = true;
    video.screen = "crt_tv";
    video.gameId = "g000000000000b002";
    video.picture = "video";
    video.phase = "running";
    video.decoderState = "playing";
    video.freezePtsSeconds = 1.0;
    video.frozenFrameShown = true;
    video.selections = 2;
    video.targets = 3;
    video.decodersStarted = 4;
    video.maxDecoderThreads = 1;
    video.framesUploaded = 321;
    video.ringBusy = 1;
    video.pauses = 2;
    video.firstFrameMs = 18.25;
    video.clipFramesUploaded = 60;
    video.firstPtsSeconds = 0.5;
    video.lastPtsSeconds = 2.5;
    video.clipUploadSeconds = 2.0;
    video.videoBytes = 4915200;
    video.stillBytes = 1638400;
    video.joinedAtExit = true;
    video.fallbacks = {{"g000000000000b003", "video", "corrupt-data"},
                       {"g000000000000b003", "screenshot", "not-found"}};
    rv::app::VideoLaunchRecord launch;
    launch.decoderRan = true;
    launch.stopMs = 3.5;
    launch.positionSeconds = 0.625;
    launch.reopened = true;
    launch.reopenedAtSeconds = 0.625;
    video.launches = {launch, rv::app::VideoLaunchRecord{}};
    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->video == video);

    // Optional values left out read back as absent; a bound record needs its fields.
    video.freezePtsSeconds.reset();
    video.firstFrameMs.reset();
    video.firstPtsSeconds.reset();
    video.lastPtsSeconds.reset();
    const auto sparse = rv::app::parseRunReport(rv::app::formatRunReport(report), "r.json");
    REQUIRE(sparse.has_value());
    CHECK(sparse->video == video);
    const nlohmann::json json = nlohmann::json::parse(rv::app::formatRunReport(report));
    CHECK_FALSE(json.at("video").contains("freezePtsSeconds"));
    const auto missing = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["video"].erase("maxDecoderThreads"); }), "r.json");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().context == "r.json:/video/maxDecoderThreads");
    const auto badLaunch = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["video"]["launches"][1]["stopMs"] = "soon"; }),
        "r.json");
    REQUIRE_FALSE(badLaunch.has_value());
    CHECK(badLaunch.error().context == "r.json:/video/launches/1/stopMs");
}

TEST_CASE("app/run_report: frame times, phases, CPU sections and GPU memory read back record for record",
          "[app][run_report]") {
    const RunReport report = measuredReport();
    const std::string text = rv::app::formatRunReport(report);
    const auto parsed = rv::app::parseRunReport(text, "r.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->frameTimes == report.frameTimes);
    CHECK(parsed->phases == report.phases);
    CHECK(parsed->cpuSections == report.cpuSections);
    CHECK(parsed->gpuMemory == report.gpuMemory);

    // The layout scripts read: percentiles flat beside the count, left out with a count of 0; kinds by name.
    const nlohmann::json json = nlohmann::json::parse(text);
    CHECK(json.at("frameTimes") == nlohmann::json{{"warmUpFrames", 30},
                                                  {"count", 570},
                                                  {"p50Ms", 6.5},
                                                  {"p95Ms", 7.25},
                                                  {"p99Ms", 9.0},
                                                  {"maxMs", 31.5}});
    CHECK(json.at("phases").at(0).at("name") == "benchmark");
    CHECK(json.at("phases").at(0).at("p99Ms") == 8.5);
    CHECK(json.at("phases").at(1) ==
          nlohmann::json{{"name", "refresh"}, {"opened", 2}, {"open", true}, {"frames", 12}, {"count", 0}});
    CHECK(json.at("cpuSections").at(0).at("name") == "productUi");
    CHECK(json.at("cpuSections").at(0).at("count") == 600);
    CHECK(json.at("gpuMemory").at("kinds").at("proceduralSurfaces") ==
          nlohmann::json{{"bytes", 83886080}, {"peakBytes", 83886080}, {"allocations", 23}});
    CHECK(json.at("gpuMemory").at("kinds").at("targets").at("peakBytes") == 70000000);

    // A count of 0 needs no percentile; any other count needs every one.
    const auto noPercentile = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["frameTimes"].erase("p99Ms"); }), "r.json");
    REQUIRE_FALSE(noPercentile.has_value());
    CHECK(noPercentile.error().context == "r.json:/frameTimes/p99Ms");
    const auto sectionWithout = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["cpuSections"][0].erase("maxMs"); }), "r.json");
    REQUIRE_FALSE(sectionWithout.has_value());
    CHECK(sectionWithout.error().context == "r.json:/cpuSections/0/maxMs");
    // Every kind this build knows is required; a kind of a newer writer is ignored.
    const auto noKind = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["gpuMemory"]["kinds"].erase("video"); }), "r.json");
    REQUIRE_FALSE(noKind.has_value());
    CHECK(noKind.error().context == "r.json:/gpuMemory/kinds/video");
    const auto newerKind = rv::app::parseRunReport(
        editedReport(report,
                     [](nlohmann::json& edited) { edited["gpuMemory"]["kinds"]["probes"] = nlohmann::json{{"bytes", 1}}; }),
        "r.json");
    REQUIRE(newerKind.has_value());
    CHECK(newerKind->gpuMemory == report.gpuMemory);
}

TEST_CASE("app/run_report: the GPU memory and CPU sections are taken from the ledger and the timers",
          "[app][run_report]") {
    rv::gl::GpuMemoryLedger ledger;
    {
        const rv::gl::GpuMemoryAllocation target(rv::gl::GpuMemoryKind::Targets, 1000, ledger);
        const rv::gl::GpuMemoryAllocation surface(rv::gl::GpuMemoryKind::ProceduralSurfaces, 200, ledger);
    }
    const rv::gl::GpuMemoryAllocation video(rv::gl::GpuMemoryKind::Video, 30, ledger);
    const rv::app::GpuMemoryRecord memory = rv::app::gpuMemoryRecord(ledger);
    CHECK(memory.totalBytes == 30);
    CHECK(memory.peakTotalBytes == 1200);
    CHECK(memory.usage(rv::gl::GpuMemoryKind::Targets) == rv::gl::GpuMemoryUsage{0, 1000, 0});
    CHECK(memory.usage(rv::gl::GpuMemoryKind::Video) == rv::gl::GpuMemoryUsage{30, 30, 1});

    rv::core::CpuTimers timers;
    timers.add("sceneSubmit", 2.0);
    timers.add("productUi", 0.5);
    timers.endFrame();
    timers.add("productUi", 0.25);
    timers.endFrame();
    const std::vector<rv::app::CpuSectionRecord> sections = rv::app::cpuSectionRecords(timers);
    REQUIRE(sections.size() == 2);
    CHECK(sections[0].name == "productUi");
    CHECK(sections[0].times == rv::app::TimeSummary{2, 0.25, 0.5, 0.5, 0.5});
    CHECK(sections[1].name == "sceneSubmit");
    CHECK(sections[1].times.count == 1);
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

TEST_CASE("app/run_report: the static groups and the outline of a view read back, absent from views without them",
          "[app][run_report]") {
    // I3 step 8: the static instance groups' counts and the outline's GPU time and instances.
    RunReport report = fullReport();
    report.lastView->staticGroups = 1;
    report.lastView->staticBatches = 3;
    report.lastView->clusters = 88;
    report.lastView->culledClusters = 70;
    report.lastView->staticDrawCalls = 12;
    report.lastView->staticInstances = 1350;
    report.lastView->outlinedInstances = 3;
    report.lastView->outlineGpuMilliseconds = 0.0123;
    const std::string text = rv::app::formatRunReport(report);
    const nlohmann::json json = nlohmann::json::parse(text);
    CHECK(json["lastView"]["staticGroups"] == nlohmann::json{{"groups", 1},          {"batches", 3},
                                                             {"clusters", 88},       {"culledClusters", 70},
                                                             {"drawCalls", 12},      {"instances", 1350}});
    CHECK(json["lastView"]["outlinedInstances"] == 3);
    CHECK(json["lastView"]["gpuMilliseconds"]["outline"] == 0.0123);
    const auto parsed = rv::app::parseRunReport(text, "report.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->lastView.has_value());
    checkSameStats(*parsed->lastView, *report.lastView);

    // The reference scenes' views have neither: their reports read as before step 8.
    const nlohmann::json plain = nlohmann::json::parse(rv::app::formatRunReport(fullReport()));
    CHECK_FALSE(plain["lastView"].contains("staticGroups"));
    CHECK_FALSE(plain["lastView"].contains("outlinedInstances"));
    CHECK_FALSE(plain["lastView"]["gpuMilliseconds"].contains("outline"));
    // A malformed count points at its key.
    const auto wrong = rv::app::parseRunReport(
        editedReport(report, [](nlohmann::json& edited) { edited["lastView"]["staticGroups"]["clusters"] = -1; }),
        "r.json");
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().context == "r.json:/lastView/staticGroups/clusters");
}