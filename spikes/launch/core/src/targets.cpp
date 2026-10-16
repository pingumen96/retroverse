#include <spike_launch/targets.hpp>

#include <fmt/format.h>

#include <rv/core/text.hpp>

namespace rv::spikes::launch {
namespace {

std::string utf8(const std::filesystem::path& path) {
    return core::pathToUtf8(path);
}

} // namespace

std::string_view toString(TargetKind kind) noexcept {
    switch (kind) {
    case TargetKind::Fake:
        return "fake";
    case TargetKind::RetroArch:
        return "retroarch";
    case TargetKind::Notepad:
        return "notepad";
    }
    return "unknown";
}

std::optional<TargetKind> parseTargetKind(std::string_view text) noexcept {
    if (core::equalsIgnoreCaseAscii(text, "fake")) {
        return TargetKind::Fake;
    }
    if (core::equalsIgnoreCaseAscii(text, "retroarch")) {
        return TargetKind::RetroArch;
    }
    if (core::equalsIgnoreCaseAscii(text, "notepad")) {
        return TargetKind::Notepad;
    }
    return std::nullopt;
}

std::vector<std::string> fakeArguments(const FakeScenario& scenario) {
    std::vector<std::string> arguments;
    if (scenario.wrapper) {
        arguments.emplace_back("--spawn-child-and-exit");
    }
    arguments.insert(arguments.end(), {"--frames", std::to_string(scenario.frames)});
    if (scenario.exitCode != 0) {
        arguments.insert(arguments.end(), {"--exit-code", fmt::format("0x{:X}", scenario.exitCode)});
    }
    if (scenario.crash) {
        arguments.emplace_back("--crash");
    }
    if (scenario.exclusive) {
        arguments.emplace_back("--exclusive");
    }
    if (scenario.stealFocus) {
        arguments.emplace_back("--steal-focus");
    }
    if (scenario.inputAtEnd) {
        arguments.emplace_back("--input-at-end");
    }
    if (!scenario.log.empty()) {
        arguments.insert(arguments.end(), {"--log", scenario.log});
    }
    if (!scenario.color.empty()) {
        arguments.insert(arguments.end(), {"--color", scenario.color});
    }
    return arguments;
}

std::filesystem::path retroArchAppendConfigPath(const std::filesystem::path& scratch) {
    return scratch / "retroverse-isolated.cfg";
}

std::vector<std::filesystem::path> retroArchScratchDirectories(const std::filesystem::path& scratch) {
    return {scratch / "saves",  scratch / "states",    scratch / "screenshots", scratch / "logs",
            scratch / "config", scratch / "config" / "remaps", scratch / "playlists",
            scratch / "playlists" / "logs", scratch / "cache"};
}

std::string retroArchAppendConfig(const std::filesystem::path& scratch) {
    const auto entry = [](std::string_view key, const std::string& value) {
        return fmt::format("{} = \"{}\"\n", key, value);
    };
    std::string text = "# Written by spike_launch (Retroverse, docs/spikes/i1-launch.md): appended to the user's\n"
                       "# configuration so that this run writes nothing outside this directory.\n";
    // Nothing saved on RetroArch's own initiative.
    text += entry("config_save_on_exit", "false");
    text += entry("remap_save_on_exit", "false");
    text += entry("history_list_enable", "false");
    text += entry("content_runtime_log", "false");
    text += entry("content_runtime_log_aggregate", "false");
    text += entry("core_info_cache_enable", "false");
    text += entry("savestate_auto_save", "false");
    text += entry("savestate_auto_load", "false");
    text += entry("autosave_interval", "0");
    text += entry("auto_overrides_enable", "false");
    text += entry("auto_remaps_enable", "false");
    text += entry("savefiles_in_content_dir", "false");
    text += entry("savestates_in_content_dir", "false");
    text += entry("systemfiles_in_content_dir", "false");
    text += entry("screenshots_in_content_dir", "false");
    // Every directory or file it could write into, inside the scratch directory.
    text += entry("savefile_directory", utf8(scratch / "saves"));
    text += entry("savestate_directory", utf8(scratch / "states"));
    text += entry("screenshot_directory", utf8(scratch / "screenshots"));
    text += entry("log_dir", utf8(scratch / "logs"));
    text += entry("rgui_config_directory", utf8(scratch / "config"));
    text += entry("input_remapping_directory", utf8(scratch / "config" / "remaps"));
    text += entry("core_options_path", utf8(scratch / "config" / "retroarch-core-options.cfg"));
    text += entry("playlist_directory", utf8(scratch / "playlists"));
    text += entry("runtime_log_directory", utf8(scratch / "playlists" / "logs"));
    text += entry("content_history_path", utf8(scratch / "playlists" / "content_history.lpl"));
    text += entry("content_favorites_path", utf8(scratch / "playlists" / "content_favorites.lpl"));
    text += entry("content_image_history_path", utf8(scratch / "playlists" / "content_image_history.lpl"));
    text += entry("content_music_history_path", utf8(scratch / "playlists" / "content_music_history.lpl"));
    text += entry("content_video_history_path", utf8(scratch / "playlists" / "content_video_history.lpl"));
    text += entry("cache_directory", utf8(scratch / "cache"));
    // The run itself: fullscreen as a player would have it, silent, offline.
    text += entry("video_fullscreen", "true");
    text += entry("audio_mute_enable", "true");
    text += entry("cheevos_enable", "false");
    text += entry("discord_allow", "false");
    text += entry("network_cmd_enable", "false");
    return text;
}

std::vector<std::string> retroArchArguments(const RetroArchSetup& setup) {
    return {
        "-L",
        utf8(setup.core),
        "--appendconfig=" + utf8(retroArchAppendConfigPath(setup.scratch)),
        "--max-frames=" + std::to_string(setup.maxFrames),
        "--sram-mode=noload-nosave",
        "--log-file=" + utf8(setup.logFile.empty() ? setup.scratch / "logs" / "retroarch.log" : setup.logFile),
        "--verbose",
        utf8(setup.rom),
    };
}

} // namespace rv::spikes::launch
