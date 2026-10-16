#include "spike_report.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/ui/timing_samples.hpp>

namespace rv::spike_ui {
namespace {

using nlohmann::json;
using Span = std::pair<std::uint32_t, std::uint32_t>;

/// Allocations per frame of one module.
struct AllocationTimes {
    ui::TimingSummary retroverse;
    ui::TimingSummary rmlui;
};

/// The times of a set of frames.
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
    AllocationTimes updateAllocations;
    AllocationTimes renderAllocations;
    std::size_t framesWithAllocations = 0;
};

[[nodiscard]] bool inside(std::uint32_t frame, const std::vector<Span>& spans) {
    return std::any_of(spans.begin(), spans.end(),
                       [frame](const Span& span) { return frame >= span.first && frame < span.second; });
}

/// Which frames of the spans count: every frame, the quiet ones (the UI did not change), or those in which it changed
/// (the focus moved, the card was laid out again).
enum class FrameFilter : std::uint8_t { All, Quiet, Changed };

[[nodiscard]] bool changed(const FrameSample& sample) noexcept {
    return sample.focusMoved || sample.cardRefreshed;
}

[[nodiscard]] PhaseTimes summarize(const ReportData& data, const std::vector<Span>& spans,
                                   FrameFilter filter = FrameFilter::All) {
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
    ui::TimingSamples updateRetroverse(capacity);
    ui::TimingSamples updateRmlUi(capacity);
    ui::TimingSamples renderRetroverse(capacity);
    ui::TimingSamples renderRmlUi(capacity);
    PhaseTimes times;
    for (const FrameSample& sample : data.samples) {
        if (!inside(sample.frame, spans)) {
            continue;
        }
        if ((filter == FrameFilter::Quiet && changed(sample)) || (filter == FrameFilter::Changed && !changed(sample))) {
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
        if (sample.updateAllocations.measured() && sample.renderAllocations.measured()) {
            times.allocationsMeasured = true;
            updateRetroverse.add(static_cast<double>(sample.updateAllocations.retroverse));
            updateRmlUi.add(static_cast<double>(sample.updateAllocations.rmlui));
            renderRetroverse.add(static_cast<double>(sample.renderAllocations.retroverse));
            renderRmlUi.add(static_cast<double>(sample.renderAllocations.rmlui));
            const std::int64_t total = sample.updateAllocations.retroverse + sample.updateAllocations.rmlui +
                                       sample.renderAllocations.retroverse + sample.renderAllocations.rmlui;
            times.framesWithAllocations += total > 0 ? 1 : 0;
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
    times.updateAllocations = {updateRetroverse.summarize(), updateRmlUi.summarize()};
    times.renderAllocations = {renderRetroverse.summarize(), renderRmlUi.summarize()};
    return times;
}

[[nodiscard]] json summaryJson(const ui::TimingSummary& summary) {
    return json{{"count", summary.count}, {"p50", summary.p50}, {"p99", summary.p99},
                {"mean", summary.mean},   {"min", summary.min}, {"max", summary.max}};
}

[[nodiscard]] json phaseJson(const PhaseTimes& times, const ReportData& data) {
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
        phase["allocations"] =
            json{{"updatePerFrame", json{{"retroverse", summaryJson(times.updateAllocations.retroverse)},
                                         {"rmlui", summaryJson(times.updateAllocations.rmlui)}}},
                 {"renderPerFrame", json{{"retroverse", summaryJson(times.renderAllocations.retroverse)},
                                         {"rmlui", summaryJson(times.renderAllocations.rmlui)}}},
                 {"framesWithAllocations", times.framesWithAllocations}};
    } else {
        phase["allocations"] = "not measured: " + data.allocationCounting;
    }
    return phase;
}

/// The spans of the named phases; with none of them (an interactive run), every frame after the startup when
/// `wholeRunWithout` holds, else nothing.
[[nodiscard]] std::vector<Span> spansOf(const ReportData& data, std::initializer_list<std::string_view> names,
                                        bool wholeRunWithout = true) {
    std::vector<Span> spans;
    for (const Phase& phase : data.phases) {
        if (std::find(names.begin(), names.end(), phase.name) != names.end()) {
            spans.emplace_back(phase.begin, phase.end);
        }
    }
    if (spans.empty() && wholeRunWithout) {
        spans.emplace_back(30u, std::numeric_limits<std::uint32_t>::max());
    }
    return spans;
}

/// The frames the measurements judge: the quiet ones of every measurement phase; those with changes, all of them, of
/// the taps, of the held D-pad.
struct MeasuredSets {
    PhaseTimes quiet;
    PhaseTimes changes;
    PhaseTimes taps;
    PhaseTimes held;
};

[[nodiscard]] MeasuredSets measuredSets(const ReportData& data) {
    MeasuredSets sets;
    sets.quiet = summarize(data, spansOf(data, {kIdlePhase, kTapsPhase, kNavigationPhase}), FrameFilter::Quiet);
    sets.changes = summarize(data, spansOf(data, {kTapsPhase, kNavigationPhase}), FrameFilter::Changed);
    sets.taps = summarize(data, spansOf(data, {kTapsPhase}, false), FrameFilter::Changed);
    sets.held = summarize(data, spansOf(data, {kNavigationPhase}, false), FrameFilter::Changed);
    return sets;
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

/// Every check named with `prefix` passed, and there was at least one.
[[nodiscard]] bool verdict(const ReportData& data, std::string_view prefix) {
    return data.scenario && checksPassed(data, prefix) && checksNamed(data, prefix) > 0;
}

[[nodiscard]] json boxJson(const glm::vec4& box) {
    return json::array({box.x, box.y, box.z, box.w});
}

[[nodiscard]] json cpuJson(const PhaseTimes& times) {
    return json{{"frames", times.frames},
                {"cpuP50", times.cpu.p50},
                {"cpuP99", times.cpu.p99},
                {"cpuMax", times.cpu.max},
                {"updateP50", times.update.p50},
                {"renderP50", times.render.p50}};
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
    report["allocationCounting"] = data.allocationCounting;
    json scaleChanges = json::array();
    for (const auto& [frame, scale] : data.displayScaleChanges) {
        scaleChanges.push_back(json{{"frame", frame}, {"displayScale", scale}});
    }
    report["displayScaleChanges"] = scaleChanges;

    json timings = json::object();
    for (const Phase& phase : data.phases) {
        timings[phase.name] = phaseJson(summarize(data, {{phase.begin, phase.end}}), data);
    }
    const MeasuredSets sets = measuredSets(data);
    // The quiet frames of the measurement phases (the cost of showing the UI), and the frames in which it changed (the
    // cost of a change: data bindings, styles, layout, new geometry), all of them, then by phase.
    timings["quiet"] = phaseJson(sets.quiet, data);
    timings["changes"] = phaseJson(sets.changes, data);
    timings["changesTaps"] = phaseJson(sets.taps, data);
    timings["changesHeld"] = phaseJson(sets.held, data);
    report["timings"] = timings;

    const bool quietPassed = sets.quiet.cpu.count > 0 && sets.quiet.gpu.count > 0 &&
                             sets.quiet.cpu.p50 < kUiPassBudgetMs && sets.quiet.gpu.p50 < kUiPassBudgetMs;
    const bool changesPassed = sets.changes.cpu.count > 0 && sets.changes.cpu.p50 < kUiPassBudgetMs;
    const bool resized = verdict(data, "same-layout") && checksPassed(data, "layout");
    report["criteria"] = json{
        {"padOnly",
         json{{"passed", verdict(data, "expect-")},
              {"detail", "every pad-driven check of the scenario (virtual gamepad): focus, actions, history, filters, "
                         "the card, the chips row; the checks of the typed search (keyboard-*) are left out: entering "
                         "text needs a keyboard until the on-screen keyboard (I7)"}}},
        {"textEntryWithKeyboard", json{{"passed", verdict(data, "keyboard-")},
                                       {"detail", "the search typed and erased on the keyboard filters the list"}}},
        {"stableFocus",
         json{{"passed", verdict(data, "expect-steady")},
              {"detail", "a stick resting under its threshold moves nothing, one crossing it every frame steps at the "
                         "held cadence (no burst), a diagonal one steps once; single taps step once each"}}},
        {"uiPassUnder1ms",
         json{{"passed", quietPassed && changesPassed},
              {"budgetMs", kUiPassBudgetMs},
              {"quietFrames",
               json{{"passed", quietPassed},
                    {"frames", sets.quiet.frames},
                    {"cpuP50", sets.quiet.cpu.p50},
                    {"cpuP99", sets.quiet.cpu.p99},
                    {"gpuP50", sets.quiet.gpu.p50},
                    {"gpuP99", sets.quiet.gpu.p99},
                    {"detail", "frames of the idle, taps and navigation phases in which the UI did not change: median "
                               "CPU time of the pass (commands, update, render) and median GPU time of its frame"}}},
              {"framesWithChanges",
               json{{"passed", changesPassed},
                    {"frames", sets.changes.frames},
                    {"cpuP50", sets.changes.cpu.p50},
                    {"cpuP99", sets.changes.cpu.p99},
                    {"cpuMax", sets.changes.cpu.max},
                    {"detail", "frames of the taps and navigation phases in which the focus moved or the card was laid "
                               "out again: median CPU time (the GPU time of the UI's frame does not depend on them)"}}},
              {"taps", cpuJson(sets.taps)},
              {"held", cpuJson(sets.held)}}},
        {"resize", json{{"passed", resized},
                        {"detail", "RmlUi's context and the document follow the window's drawable at the display's "
                                   "density, every panel inside the view and the focused game visible at each size, "
                                   "the layout back at 1920x1080 (and back at text scale x1) equal to the first"}}},
        {"accessibility",
         json{{"passed", verdict(data, "text-scale") && verdict(data, "high-contrast")},
              {"detail", "text scale x1.2 makes the text 1.2 times larger in the same layout theme; high contrast puts "
                         "its class on the document and opaque black panels"}}},
        {"rmluiLogClean", json{{"passed", data.rmlErrors == 0 && data.rmlWarnings == 0}}}};
    report["checksNotRun"] = data.checksNotRun;

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
        json fontSizes = json::object();
        for (const auto& [id, size] : layout.fontSizes) {
            fontSizes[id] = size;
        }
        layouts.push_back(json{{"frame", layout.frame},
                               {"label", layout.label},
                               {"size", json::array({layout.size.x, layout.size.y})},
                               {"dpRatio", layout.dpRatio},
                               {"compact", layout.compact},
                               {"focused", layout.focused},
                               {"boxes", boxes},
                               {"fontSizes", fontSizes}});
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
        std::string text = fmt::format(
            "UI pass, {} ({} frames): CPU p50 {:.3f} ms p99 {:.3f} ms max {:.3f} ms (input {:.3f}, update {:.3f}, render "
            "{:.3f} = begin {:.3f} + context {:.3f} + end {:.3f} at p50; {:.0f} draws), GPU p50 {:.3f} ms p99 {:.3f} ms "
            "({} samples), frame p50 {:.3f} ms p99 {:.3f} ms",
            name, times.frames, times.cpu.p50, times.cpu.p99, times.cpu.max, times.input.p50, times.update.p50,
            times.render.p50, times.beginFrame.p50, times.contextRender.p50, times.endFrame.p50, times.drawCalls.p50,
            times.gpu.p50, times.gpu.p99, times.gpu.count, times.frame.p50, times.frame.p99);
        if (times.allocationsMeasured) {
            text += fmt::format("; allocations per frame, Retroverse + RmlUi, p50 (max): update {:.0f} ({:.0f}) + {:.0f} "
                                "({:.0f}), render {:.0f} ({:.0f}) + {:.0f} ({:.0f})",
                                times.updateAllocations.retroverse.p50, times.updateAllocations.retroverse.max,
                                times.updateAllocations.rmlui.p50, times.updateAllocations.rmlui.max,
                                times.renderAllocations.retroverse.p50, times.renderAllocations.retroverse.max,
                                times.renderAllocations.rmlui.p50, times.renderAllocations.rmlui.max);
        }
        lines.push_back(std::move(text));
    };
    for (const Phase& phase : data.phases) {
        line(phase.name, summarize(data, {{phase.begin, phase.end}}));
    }
    const MeasuredSets sets = measuredSets(data);
    line("quiet frames (the UI did not change)", sets.quiet);
    line("frames with changes (focus moved or card laid out)", sets.changes);
    line("frames with changes, single taps", sets.taps);
    line("frames with changes, D-pad held", sets.held);
    return lines;
}

} // namespace rv::spike_ui
