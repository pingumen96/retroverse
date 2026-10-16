#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rv::spikes::launch {

/// What spike_launch starts.
enum class TargetKind : std::uint8_t {
    Fake,      ///< rv_fake_emulator (spikes/launch/fake_emulator)
    RetroArch, ///< a RetroArch installation, isolated from the user's configuration and saves
    Notepad,   ///< the system's notepad.exe: on Windows 11 a stub that hands over to the packaged app and exits
};

[[nodiscard]] std::string_view toString(TargetKind kind) noexcept;
[[nodiscard]] std::optional<TargetKind> parseTargetKind(std::string_view text) noexcept;

/// What the fake emulator does in one launch.
struct FakeScenario {
    int frames = 150;          ///< frames of its fullscreen window (vsync paced)
    std::uint32_t exitCode = 0;
    bool crash = false;        ///< an access violation after the frames
    bool exclusive = false;    ///< exclusive fullscreen in another display mode
    bool stealFocus = false;   ///< forces itself into the foreground on its first frame
    bool inputAtEnd = false;   ///< a key tap into its own window before it closes: the player's last input,
                               ///< which keeps Windows' foreground lock active when it exits
    bool wrapper = false;      ///< starts a copy of itself and exits at once (ProcessTree waiting)
    std::string log;           ///< where it writes what it saw (JSON), empty for none
    std::string color = "7A2E8C";
};

/// The fake emulator's arguments for `scenario`.
[[nodiscard]] std::vector<std::string> fakeArguments(const FakeScenario& scenario);

/// A RetroArch run that leaves the user's installation, configuration and saves untouched: the main configuration
/// is read as it is (never saved), and an appended configuration (--appendconfig) moves everything RetroArch could
/// write into `scratch` (saves, states, screenshots, logs, the configuration directory with core options, overrides and
/// remaps, playlists and history, runtime logs, the cache) and turns off the writes it would do on its own (saving the
/// configuration and remaps on exit, the history, runtime logs, the core info cache, SRAM autosave, automatic save
/// states). SRAM is neither loaded nor saved (--sram-mode=noload-nosave); --max-frames makes it exit by itself; audio
/// is muted.
struct RetroArchSetup {
    std::filesystem::path executable;
    std::filesystem::path core;
    std::filesystem::path rom;
    std::filesystem::path scratch;
    /// RetroArch's own log of the run (--log-file); empty: <scratch>/logs/retroarch.log.
    std::filesystem::path logFile;
    int maxFrames = 240;
};

/// The appended configuration file's text for `scratch`.
[[nodiscard]] std::string retroArchAppendConfig(const std::filesystem::path& scratch);
/// Where retroArchAppendConfig() is written.
[[nodiscard]] std::filesystem::path retroArchAppendConfigPath(const std::filesystem::path& scratch);
/// The directories under `scratch` to create before the launch.
[[nodiscard]] std::vector<std::filesystem::path> retroArchScratchDirectories(const std::filesystem::path& scratch);
/// RetroArch's arguments: -L <core> --appendconfig=<file> --max-frames=<n> --sram-mode=noload-nosave
/// --log-file=<log> --verbose <rom>.
[[nodiscard]] std::vector<std::string> retroArchArguments(const RetroArchSetup& setup);

} // namespace rv::spikes::launch
