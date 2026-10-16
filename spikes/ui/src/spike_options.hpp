#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rv/app/command_line.hpp>
#include <rv/core/expected.hpp>

#include "scenario.hpp"

namespace rv::spike_ui {

/// A screenshot of the window's frame (scene and UI) at a chosen frame: --screenshot-at <frame>:<png>.
struct ScreenshotRequest {
    std::uint32_t frame = 0;
    std::filesystem::path path;
};

/// The command line of spike_ui: the host options of every Retroverse executable (app::addHostOptions) and the spike's
/// own.
struct SpikeOptions {
    app::CommandLineOptions run;
    /// --screenshot-at <frame>:<png>, repeatable; frames counted from 0, sorted.
    std::vector<ScreenshotRequest> screenshots;
    /// --report <json>: the measurements and checks of the run, written when it ends.
    std::optional<std::filesystem::path> report;
    /// Cleared by --interactive and --input-script: no scripted gamepad, the window is driven by hand or by the input
    /// script.
    bool scenario = true;
    /// --scenario <name>: which scripted scenario the virtual gamepad plays.
    ScenarioKind scenarioKind = ScenarioKind::QuickCatalog;
    /// --input-script <json>: UI actions that replace the devices' on chosen frames (rv::input::InputScript), as
    /// retroverse.exe plays them.
    std::optional<std::filesystem::path> inputScript;
    /// --expect-selection <position>: with --input-script, the run fails (exit code 3) unless the selection ends there.
    std::optional<std::size_t> expectSelection;
    /// --ui-scale <factor>: text scaling (specification §12.4), multiplies the dp ratio.
    float textScale = 1.0f;
    /// --high-contrast: the high-contrast theme (specification §12.4).
    bool highContrast = false;
    /// --fps-cap <n>: paces the frames at n per second (0: as fast as possible). Measurements use 60, as a 60 Hz
    /// display with vsync would: an uncapped run with --no-vsync is GPU-bound, and the driver then blocks the CPU in
    /// whichever OpenGL call finds its queue full, which can be one of the UI pass's and inflates its CPU time.
    int fpsCap = 0;
};

/// The window spike_ui opens when --width and --height are not given: the 1080p of the success criteria.
inline constexpr int kSpikeWindowWidth = 1920;
inline constexpr int kSpikeWindowHeight = 1080;
/// The simulation step of scripted runs without --fixed-dt (60 Hz).
inline constexpr double kScenarioFixedDeltaSeconds = 1.0 / 60.0;

/// Parses the arguments (UTF-8, program name excluded). Errors name the offending option: those of
/// app::applyHostOptions, a --screenshot-at that is not <frame>:<file.png> or names a frame the run never reaches,
/// a --report without the .json extension, a --ui-scale outside [0.5, 3], an unknown --scenario, --scenario or
/// --input-script with --interactive, --scenario with --input-script, --expect-selection without --input-script or a
/// frame limit.
[[nodiscard]] Expected<SpikeOptions> parseSpikeOptions(std::span<const std::string> arguments);
[[nodiscard]] Expected<SpikeOptions> parseSpikeOptions(int argc, const char* const* argv);
[[nodiscard]] std::string spikeHelp();

} // namespace rv::spike_ui
