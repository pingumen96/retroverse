#pragma once

#include <cstddef>
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

#include "allocation_counter.hpp"
#include "scenario.hpp"

namespace rv::spike_ui {

/// The UI pass of one frame.
struct FrameSample {
    std::uint32_t frame = 0;
    double inputMs = 0.0;  ///< CPU: the frame's commands applied to the screen (focus moves, data model changes)
    double updateMs = 0.0; ///< CPU: Context::Update and the screen's after-update work
    double renderMs = 0.0; ///< CPU: Context::Render and the renderer's frame
    double beginFrameMs = 0.0;    ///< CPU part of renderMs: the renderer's BeginFrame
    double contextRenderMs = 0.0; ///< CPU part of renderMs: Context::Render
    double endFrameMs = 0.0;      ///< CPU part of renderMs: the renderer's EndFrame
    std::uint32_t drawCalls = 0;  ///< draws of the UI's frame
    /// GPU: the renderer's frame, when the GPU timer's latest GL_TIME_ELAPSED result changed during this frame (results
    /// are read frames later, so the span measured is an earlier frame's; a result equal to the previous one, or one
    /// followed by another within the same frame, is not seen: gl::GpuTimer reports the latest result only).
    double gpuMs = std::numeric_limits<double>::quiet_NaN();
    double frameMs = 0.0; ///< the whole frame, the host's clock (vsync, the scene and the UI)
    /// This frame's update and render handled a change of the UI: the focus moved to another element, the card was
    /// filled again (its text laid out again), the pool's rows were bound to another window (the list scrolled), the
    /// selection moved. A change made after the context's update (the card settling once the selection rests, the
    /// window following the layout) counts for the next frame, which lays it out. Frames with none are the quiet ones.
    bool focusMoved = false;
    bool cardRefreshed = false;
    bool rowsBound = false;
    bool selectionMoved = false;
    /// Heap allocations of the UI pass on the main thread (AllocationCounter), during the commands and the update, and
    /// during the render.
    AllocationCounts updateAllocations;
    AllocationCounts renderAllocations;
};

struct FocusRecord {
    std::uint32_t frame = 0;
    std::string region;
    std::string element;
};

/// The selection at a new position of the list (a move, a page, a jump, another list).
struct SelectionRecord {
    std::uint32_t frame = 0;
    std::size_t position = 0;
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

/// The layout of the screen at one drawable size: element boxes (x, y, width, height in pixels) and the font sizes of
/// its text elements (pixels).
struct LayoutRecord {
    std::uint32_t frame = 0;
    std::string label;
    glm::ivec2 size{0};
    float dpRatio = 1.0f;
    float displayScale = 1.0f;
    bool compact = false;
    /// The rows of the list the screen showed (the model's window).
    std::size_t windowRows = 0;
    std::vector<std::pair<std::string, glm::vec4>> boxes;
    std::vector<std::pair<std::string, float>> fontSizes;
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
    /// quick-catalog, user-bindings, input-script or interactive.
    std::string scenarioName;
    std::optional<double> fixedDeltaSeconds;
    bool vsync = true;
    int fpsCap = 0;
    std::uint64_t framesRendered = 0;
    std::vector<std::string> fonts;
    /// The catalog: games, the time to generate and index them, the pool of rows and the rows the screen showed at the
    /// end, the selection then, the user's bindings file applied (empty: none).
    std::size_t catalogGames = 0;
    double catalogMs = 0.0;
    std::size_t poolRows = 0;
    std::size_t windowRows = 0;
    std::optional<std::size_t> finalSelection;
    std::string finalSelectionId;
    std::string userBindings;
    std::uint64_t rmlErrors = 0;
    std::uint64_t rmlWarnings = 0;
    /// How allocations were counted, or why they were not.
    std::string allocationCounting;
    /// Frames at which the window's display scale changed under the run (another program switched the display mode),
    /// with the new scale: the frames after one are laid out at another density, so such runs do not measure the same.
    std::vector<std::pair<std::uint32_t, float>> displayScaleChanges;
    std::vector<Phase> phases;
    std::vector<FrameSample> samples;
    std::vector<FocusRecord> focusChanges;
    std::vector<SelectionRecord> selections;
    std::vector<ActionRecord> actions;
    std::vector<CheckRecord> checks;
    /// Checks of the scenario that never ran: the run ended before their frame.
    std::uint64_t checksNotRun = 0;
    std::vector<LayoutRecord> layouts;
    std::vector<ScreenshotRecord> screenshots;
    std::uint64_t commandsDropped = 0;
};

/// The UI pass under 1 ms at 1080p (ADR-0005), judged twice: on the quiet frames of the measurement phases (median
/// CPU time of the pass, commands, update and render, and median GPU time of its frame) and on the frames in which the
/// UI changed (median CPU time). The criterion passes when both are below this budget.
inline constexpr double kUiPassBudgetMs = 1.0;

/// The report as JSON: the run, the checks, the focus changes and actions, the layouts, the screenshots, and for every
/// phase, for the quiet frames and for the frames with changes (all of them, those of the taps, those of the held
/// D-pad) the p50/p99/mean/max of each time and the allocations, with the verdicts of the success criteria.
[[nodiscard]] nlohmann::json reportJson(const ReportData& data);
/// Writes reportJson(data) atomically (indented).
[[nodiscard]] Status writeReport(const std::filesystem::path& path, const ReportData& data);
/// One line per phase and per set of frames with its medians and 99th percentiles, for the log.
[[nodiscard]] std::vector<std::string> timingSummaryLines(const ReportData& data);

} // namespace rv::spike_ui
