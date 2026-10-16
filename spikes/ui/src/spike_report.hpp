#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json_fwd.hpp>

#include <rv/core/expected.hpp>

#include "scenario.hpp"

namespace rv::spike_ui {

/// The UI pass of one frame.
struct FrameSample {
    std::uint32_t frame = 0;
    double inputMs = 0.0;  ///< CPU: the frame's commands applied to the screen (focus moves, data model changes)
    double updateMs = 0.0; ///< CPU: Context::Update
    double renderMs = 0.0; ///< CPU: Context::Render and the renderer's frame
    double beginFrameMs = 0.0;    ///< CPU part of renderMs: the renderer's BeginFrame
    double contextRenderMs = 0.0; ///< CPU part of renderMs: Context::Render
    double endFrameMs = 0.0;      ///< CPU part of renderMs: the renderer's EndFrame
    std::uint32_t drawCalls = 0;  ///< draws of the UI's frame
    /// GPU: the renderer's frame, when a new GL_TIME_ELAPSED result arrived during this frame (read frames later).
    double gpuMs = std::numeric_limits<double>::quiet_NaN();
    double frameMs = 0.0; ///< the whole frame, the host's clock (vsync, the scene and the UI)
    /// Heap allocations of the UI pass on the main thread (the debug CRT's allocation hook, so Debug builds only: it
    /// sees our code and rmlui.dll, not the driver), during the commands and the update, and during the render; -1 when
    /// not measured.
    std::int64_t updateAllocations = -1;
    std::int64_t renderAllocations = -1;
};

struct FocusRecord {
    std::uint32_t frame = 0;
    std::string region;
    std::string element;
};

/// A product action the screen asked for: launch, media, info-shown, info-hidden, close.
struct ActionRecord {
    std::uint32_t frame = 0;
    std::string action;
    std::string game;
};

struct CheckRecord {
    std::uint32_t frame = 0;
    std::string name;
    bool passed = false;
    std::string detail;
    /// The check could not be made (the machine's display changed under the run): neither passed nor failed.
    bool skipped = false;
};

/// The layout of the screen at one drawable size: element boxes (x, y, width, height in pixels).
struct LayoutRecord {
    std::uint32_t frame = 0;
    std::string label;
    glm::ivec2 size{0};
    float dpRatio = 1.0f;
    std::vector<std::pair<std::string, glm::vec4>> boxes;
    std::string focused;
};

struct ScreenshotRecord {
    std::uint32_t frame = 0;
    std::string path;
    bool written = false;
};

/// Everything a run of spike_ui reports.
struct ReportData {
    std::string build;            ///< Debug, Release, ...
    std::string rmluiVersion;
    std::string glRenderer;
    std::string glVersion;
    glm::ivec2 initialSize{0};
    float displayScale = 1.0f;
    float textScale = 1.0f;
    bool highContrast = false;
    bool scenario = true;
    std::optional<double> fixedDeltaSeconds;
    bool vsync = true;
    int fpsCap = 0;
    std::uint64_t framesRendered = 0;
    std::vector<std::string> fonts;
    std::uint64_t rmlErrors = 0;
    std::uint64_t rmlWarnings = 0;
    std::vector<Phase> phases;
    std::vector<FrameSample> samples;
    std::vector<FocusRecord> focusChanges;
    std::vector<ActionRecord> actions;
    std::vector<CheckRecord> checks;
    std::vector<LayoutRecord> layouts;
    std::vector<ScreenshotRecord> screenshots;
    std::uint64_t commandsDropped = 0;
};

/// The UI pass under 1 ms at 1080p (ADR-0005): the median CPU time of the pass (input, update and render) and the
/// median GPU time of its frame, each below this budget, in the steady-state phases.
inline constexpr double kUiPassBudgetMs = 1.0;

/// The report as JSON: the run, the checks, the focus changes and actions, the layouts, the screenshots, and for every
/// phase (and "steady", the idle and navigation phases together) the p50/p99/mean/max of each time, with the verdicts
/// of the success criteria.
[[nodiscard]] nlohmann::json reportJson(const ReportData& data);
/// Writes reportJson(data) atomically (indented).
[[nodiscard]] Status writeReport(const std::filesystem::path& path, const ReportData& data);
/// One line per phase with its medians and 99th percentiles, for the log.
[[nodiscard]] std::vector<std::string> timingSummaryLines(const ReportData& data);

} // namespace rv::spike_ui
