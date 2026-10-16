#include "spike_report.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/ui/timing_samples.hpp>

namespace rv::spike_ui {
namespace {

using nlohmann::json;

/// The times of a span of frames.
struct PhaseTimes {
    std::size_t frames = 0;
    ui::TimingSummary input;
    ui::TimingSummary update;
    ui::TimingSummary render;
    ui::TimingSummary beginFrame;
    ui::TimingSummary contextRender;
    ui::TimingSummary endFrame;
    ui::TimingSummary drawCalls;
    ui::TimingSummary cpu; ///< input + update + render
    ui::TimingSummary gpu;
    ui::TimingSummary frame;
    bool allocationsMeasured = false;
    /// Per frame: the commands and update, the render.
    ui::TimingSummary updateAllocations;
    ui::TimingSummary renderAllocations;
    std::size_t framesWithAllocations = 0;
};

[[nodiscard]] bool inside(std::uint32_t frame, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& spans) {
    return std::any_of(spans.begin(), spans.end(),
                       [frame](const auto& span) { return frame >= span.first && frame < span.second; });
}

/// Which frames of a span count: every frame, only the frames in which the focus moved, or only the others.
enum class FrameFilter : std::uint8_t { All, FocusMoved, FocusStill };

[[nodiscard]] PhaseTimes summarize(const ReportData& data,
                                   const std::vector<std::pair<std::uint32_t, std::uint32_t>>& spans,
                                   FrameFilter filter = FrameFilter::All) {
    std::vector<std::uint32_t> moved;
    for (const FocusRecord& record : data.focusChanges) {
        moved.push_back(record.frame);
    }
    std::sort(moved.begin(), moved.end());
    const std::size_t capacity = std::max<std::size_t>(data.samples.size(), 1);
    ui::TimingSamples input(capacity);
    ui::TimingSamples update(capacity);
    ui::TimingSamples render(capacity);
    ui::TimingSamples beginFrame(capacity);
    ui::TimingSamples contextRender(capacity);
    ui::TimingSamples endFrame(capacity);
    ui::TimingSamples drawCalls(capacity);
    ui::TimingSamples cpu(capacity);
    ui::TimingSamples gpu(capacity);
    ui::TimingSamples frame(capacity);
    ui::TimingSamples updateAllocations(capacity);
    ui::TimingSamples renderAllocations(capacity);
    PhaseTimes times;
    for (const FrameSample& sample : data.samples) {
        if (!inside(sample.frame, spans)) {
            continue;
        }
        const bool focusMoved = std::binary_search(moved.begin(), moved.end(), sample.frame);
        if ((filter == FrameFilter::FocusMoved && !focusMoved) || (filter == FrameFilter::FocusStill && focusMoved)) {
            continue;
        }
        ++times.frames;
        input.add(sample.inputMs);
        update.add(sample.updateMs);
        render.add(sample.renderMs);
        beginFrame.add(sample.beginFrameMs);
        contextRender.add(sample.contextRenderMs);
        endFrame.add(sample.endFrameMs);
        drawCalls.add(static_cast<double>(sample.drawCalls));
        cpu.add(sample.inputMs + sample.updateMs + sample.renderMs);
        if (!std::isnan(sample.gpuMs)) {
            gpu.add(sample.gpuMs);
        }
        frame.add(sample.frameMs);
        if (sample.updateAllocations >= 0 && sample.renderAllocations >= 0) {
            times.allocationsMeasured = true;
            updateAllocations.add(static_cast<double>(sample.updateAllocations));
            renderAllocations.add(static_cast<double>(sample.renderAllocations));
            times.framesWithAllocations += sample.updateAllocations + sample.renderAllocations > 0 ? 1 : 0;
        }
    }
    times.input = input.summarize();
    times.update = update.summarize();
    times.render = render.summarize();
    times.beginFrame = beginFrame.summarize();
    times.contextRender = contextRender.summarize();
    times.endFrame = endFrame.summarize();
    times.drawCalls = drawCalls.summarize();
    times.cpu = cpu.summarize();
    times.gpu = gpu.summarize();
    times.frame = frame.summarize();
    times.updateAllocations = updateAllocations.summarize();
    times.renderAllocations = renderAllocations.summarize();
    return times;
}

[[nodiscard]] json summaryJson(const ui::TimingSummary& summary) {
    return json{{"count", summary.count}, {"p50", summary.p50}, {"p99", summary.p99},
                {"mean", summary.mean},   {"min", summary.min}, {"max", summary.max}};
}

[[nodiscard]] json phaseJson(const PhaseTimes& times) {
    json phase{{"frames", times.frames},
               {"inputCpuMs", summaryJson(times.input)},
               {"updateCpuMs", summaryJson(times.update)},
               {"renderCpuMs", summaryJson(times.render)},
               {"beginFrameCpuMs", summaryJson(times.beginFrame)},
               {"contextRenderCpuMs", summaryJson(times.contextRender)},
               {"endFrameCpuMs", summaryJson(times.endFrame)},
               {"drawCalls", summaryJson(times.drawCalls)},
               {"uiCpuMs", summaryJson(times.cpu)},
               {"uiGpuMs", summaryJson(times.gpu)},
               {"frameMs", summaryJson(times.frame)}};
    if (times.allocationsMeasured) {
        phase["allocations"] = json{{"updatePerFrame", summaryJson(times.updateAllocations)},
                                    {"renderPerFrame", summaryJson(times.renderAllocations)},
                                    {"framesWithAllocations", times.framesWithAllocations}};
    } else {
        phase["allocations"] = "not measured (Debug builds only)";
    }
    return phase;
}

/// The spans of the steady-state measurement: the idle and navigation phases.
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> steadySpans(const ReportData& data) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
    for (const Phase& phase : data.phases) {
        if (phase.name == "idle" || phase.name == "navigation") {
            spans.emplace_back(phase.begin, phase.end);
        }
    }
    if (spans.empty()) {
        spans.emplace_back(30u, std::numeric_limits<std::uint32_t>::max()); // interactive: all but the startup
    }
    return spans;
}

[[nodiscard]] bool checksPassed(const ReportData& data, std::string_view prefix) {
    return std::all_of(data.checks.begin(), data.checks.end(), [prefix](const CheckRecord& check) {
        return !check.name.starts_with(prefix) || check.passed || check.skipped;
    });
}

/// Checks made (not skipped) whose name starts with `prefix`.
[[nodiscard]] std::size_t checksNamed(const ReportData& data, std::string_view prefix) {
    return static_cast<std::size_t>(
        std::count_if(data.checks.begin(), data.checks.end(),
                      [prefix](const CheckRecord& c) { return c.name.starts_with(prefix) && !c.skipped; }));
}

[[nodiscard]] json boxJson(const glm::vec4& box) {
    return json::array({box.x, box.y, box.z, box.w});
}

} // namespace

json reportJson(const ReportData& data) {
    json report;
    report["spike"] = "I1 (d) product UI: RmlUi on SDL3 and OpenGL 3, gamepad navigation";
    report["build"] = data.build;
    report["rmlui"] = data.rmluiVersion;
    report["gl"] = json{{"renderer", data.glRenderer}, {"version", data.glVersion}};
    report["window"] = json{{"width", data.initialSize.x}, {"height", data.initialSize.y},
                            {"displayScale", data.displayScale}};
    report["textScale"] = data.textScale;
    report["highContrast"] = data.highContrast;
    report["scenario"] = data.scenario;
    report["fixedDtSeconds"] = data.fixedDeltaSeconds ? json(*data.fixedDeltaSeconds) : json(nullptr);
    report["vsync"] = data.vsync;
    report["fpsCap"] = data.fpsCap;
    report["framesRendered"] = data.framesRendered;
    report["fonts"] = data.fonts;
    report["rmlLog"] = json{{"errors", data.rmlErrors}, {"warnings", data.rmlWarnings}};
    report["commandsDropped"] = data.commandsDropped;

    json timings = json::object();
    for (const Phase& phase : data.phases) {
        timings[phase.name] = phaseJson(summarize(data, {{phase.begin, phase.end}}));
    }
    const PhaseTimes steady = summarize(data, steadySpans(data));
    timings["steady"] = phaseJson(steady);
    // The cost of a focus move (relayout, data bindings, new geometry) against the frames in which nothing moved.
    timings["steadyFocusMoved"] = phaseJson(summarize(data, steadySpans(data), FrameFilter::FocusMoved));
    timings["steadyFocusStill"] = phaseJson(summarize(data, steadySpans(data), FrameFilter::FocusStill));
    report["timings"] = timings;

    const bool padOnly = data.scenario && checksPassed(data, "expect-") && checksNamed(data, "expect-") > 0;
    const bool stable = data.scenario && checksPassed(data, "expect-steady") && checksNamed(data, "expect-steady") > 0;
    const bool resized = data.scenario && checksPassed(data, "layout") && checksPassed(data, "same-layout") &&
                         checksNamed(data, "same-layout") > 0;
    const bool budget = steady.cpu.count > 0 && steady.gpu.count > 0 && steady.cpu.p50 < kUiPassBudgetMs &&
                        steady.gpu.p50 < kUiPassBudgetMs;
    report["criteria"] = json{
        {"padOnly", json{{"passed", padOnly},
                         {"detail", "every scripted step (virtual gamepad only) reached its expected focus, action or "
                                    "filter"}}},
        {"stableFocus",
         json{{"passed", stable},
              {"detail", "a stick resting near its threshold moves nothing, a diagonal one steps once"}}},
        {"uiPassUnder1ms",
         json{{"passed", budget},
              {"budgetMs", kUiPassBudgetMs},
              {"cpuP50", steady.cpu.p50},
              {"cpuP99", steady.cpu.p99},
              {"gpuP50", steady.gpu.p50},
              {"gpuP99", steady.gpu.p99},
              {"p99UnderBudget", steady.cpu.p99 < kUiPassBudgetMs && steady.gpu.p99 < kUiPassBudgetMs},
              {"detail", "steady phases (idle, navigation): median CPU (input + update + render) and GPU time of "
                         "the UI pass"}}},
        {"resize", json{{"passed", resized},
                        {"detail", "the view follows the drawable at the display's density, every panel inside the "
                                   "view and the focused game visible at each size, the layout back at 1920x1080 "
                                   "equal to the first"}}},
        {"rmluiLogClean", json{{"passed", data.rmlErrors == 0 && data.rmlWarnings == 0}}}};

    json checks = json::array();
    for (const CheckRecord& check : data.checks) {
        checks.push_back(json{{"frame", check.frame},
                              {"name", check.name},
                              {"passed", check.passed},
                              {"skipped", check.skipped},
                              {"detail", check.detail}});
    }
    report["checks"] = checks;
    json focus = json::array();
    for (const FocusRecord& record : data.focusChanges) {
        focus.push_back(json{{"frame", record.frame}, {"region", record.region}, {"element", record.element}});
    }
    report["focusChanges"] = focus;
    json actions = json::array();
    for (const ActionRecord& record : data.actions) {
        actions.push_back(json{{"frame", record.frame}, {"action", record.action}, {"game", record.game}});
    }
    report["actions"] = actions;
    json layouts = json::array();
    for (const LayoutRecord& layout : data.layouts) {
        json boxes = json::object();
        for (const auto& [id, box] : layout.boxes) {
            boxes[id] = boxJson(box);
        }
        layouts.push_back(json{{"frame", layout.frame},
                               {"label", layout.label},
                               {"size", json::array({layout.size.x, layout.size.y})},
                               {"dpRatio", layout.dpRatio},
                               {"focused", layout.focused},
                               {"boxes", boxes}});
    }
    report["layouts"] = layouts;
    json screenshots = json::array();
    for (const ScreenshotRecord& shot : data.screenshots) {
        screenshots.push_back(json{{"frame", shot.frame}, {"path", shot.path}, {"written", shot.written}});
    }
    report["screenshots"] = screenshots;
    return report;
}

Status writeReport(const std::filesystem::path& path, const ReportData& data) {
    return core::writeTextFileAtomic(path, reportJson(data).dump(2) + "\n");
}

std::vector<std::string> timingSummaryLines(const ReportData& data) {
    std::vector<std::string> lines;
    const auto line = [&lines](std::string_view name, const PhaseTimes& times) {
        lines.push_back(fmt::format("UI pass, {} ({} frames): CPU p50 {:.3f} ms p99 {:.3f} ms (input {:.3f}, update "
                                    "{:.3f}, render {:.3f} = begin {:.3f} + context {:.3f} + end {:.3f} at p50; {:.0f} "
                                    "draws), GPU p50 {:.3f} ms p99 {:.3f} ms ({} samples), frame p50 {:.3f} ms p99 "
                                    "{:.3f} ms",
                                    name, times.frames, times.cpu.p50, times.cpu.p99, times.input.p50,
                                    times.update.p50, times.render.p50, times.beginFrame.p50, times.contextRender.p50,
                                    times.endFrame.p50, times.drawCalls.p50, times.gpu.p50, times.gpu.p99,
                                    times.gpu.count, times.frame.p50, times.frame.p99));
    };
    for (const Phase& phase : data.phases) {
        line(phase.name, summarize(data, {{phase.begin, phase.end}}));
    }
    line("steady", summarize(data, steadySpans(data)));
    line("steady, frames where the focus moved", summarize(data, steadySpans(data), FrameFilter::FocusMoved));
    line("steady, frames where it stayed", summarize(data, steadySpans(data), FrameFilter::FocusStill));
    return lines;
}

} // namespace rv::spike_ui
