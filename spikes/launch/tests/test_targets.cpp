#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <spike_launch/targets.hpp>

using rv::spikes::launch::FakeScenario;
using rv::spikes::launch::fakeArguments;
using rv::spikes::launch::parseTargetKind;
using rv::spikes::launch::RetroArchSetup;
using rv::spikes::launch::retroArchAppendConfig;
using rv::spikes::launch::retroArchArguments;
using rv::spikes::launch::retroArchScratchDirectories;
using rv::spikes::launch::TargetKind;

namespace {

/// The key = "value" lines of a RetroArch configuration.
std::map<std::string, std::string> parseConfig(const std::string& text) {
    std::map<std::string, std::string> entries;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find(" = \"");
        REQUIRE(equals != std::string::npos);
        REQUIRE(line.back() == '"');
        entries[line.substr(0, equals)] = line.substr(equals + 4, line.size() - equals - 5);
    }
    return entries;
}

} // namespace

TEST_CASE("launch_spike/targets: target names", "[launch_spike][targets]") {
    CHECK(parseTargetKind("fake") == TargetKind::Fake);
    CHECK(parseTargetKind("RetroArch") == TargetKind::RetroArch);
    CHECK(parseTargetKind("notepad") == TargetKind::Notepad);
    CHECK_FALSE(parseTargetKind("duckstation").has_value());
}

TEST_CASE("launch_spike/targets: the fake emulator's arguments", "[launch_spike][targets]") {
    FakeScenario scenario;
    scenario.frames = 90;
    scenario.color.clear();
    CHECK(fakeArguments(scenario) == std::vector<std::string>{"--frames", "90"});

    scenario.wrapper = true;
    scenario.crash = true;
    scenario.exclusive = true;
    scenario.stealFocus = true;
    scenario.inputAtEnd = true;
    scenario.exitCode = 0xC0000005u;
    scenario.log = "C:\\data dir\\fake-1.json";
    const std::vector<std::string> expected{"--spawn-child-and-exit", "--frames", "90", "--exit-code", "0xC0000005",
                                            "--crash", "--exclusive", "--steal-focus", "--input-at-end",
                                            "--log",
                                            "C:\\data dir\\fake-1.json"};
    CHECK(fakeArguments(scenario) == expected);
}

TEST_CASE("launch_spike/targets: RetroArch writes nothing outside its scratch directory", "[launch_spike][targets]") {
    const std::filesystem::path scratch = std::filesystem::path("E:\\build\\spike data") / "retroarch";
    const auto config = parseConfig(retroArchAppendConfig(scratch));
    // Nothing saved on its own initiative.
    for (const char* key : {"config_save_on_exit", "remap_save_on_exit", "history_list_enable", "content_runtime_log",
                            "content_runtime_log_aggregate", "core_info_cache_enable", "savestate_auto_save",
                            "savefiles_in_content_dir", "savestates_in_content_dir", "systemfiles_in_content_dir",
                            "screenshots_in_content_dir"}) {
        INFO(key);
        CHECK(config.at(key) == "false");
    }
    CHECK(config.at("autosave_interval") == "0");
    // Every place it writes to is inside the scratch directory.
    const std::string root = scratch.string();
    for (const char* key :
         {"savefile_directory", "savestate_directory", "screenshot_directory", "log_dir", "rgui_config_directory",
          "input_remapping_directory", "core_options_path", "playlist_directory", "runtime_log_directory",
          "content_history_path", "content_favorites_path", "content_image_history_path",
          "content_music_history_path", "content_video_history_path", "cache_directory"}) {
        INFO(key);
        CHECK(config.at(key).starts_with(root));
    }
    CHECK(config.at("video_fullscreen") == "true");
    CHECK(config.at("audio_mute_enable") == "true");

    const auto directories = retroArchScratchDirectories(scratch);
    CHECK(std::all_of(directories.begin(), directories.end(),
                      [&](const std::filesystem::path& directory) { return directory.string().starts_with(root); }));
}

TEST_CASE("launch_spike/targets: RetroArch's arguments", "[launch_spike][targets]") {
    RetroArchSetup setup;
    setup.executable = "E:\\Programmi\\Retroarch\\retroarch.exe";
    setup.core = "E:\\Programmi\\Retroarch\\cores\\gambatte_libretro.dll";
    setup.rom = "I:\\Giochi\\Emulatori\\gb\\1 GB US - H-Q\\Mole Mania (USA, Europe) (SGB Enhanced).gb";
    setup.scratch = "E:\\build\\data\\retroarch";
    setup.maxFrames = 300;
    const std::vector<std::string> expected{
        "-L",
        "E:\\Programmi\\Retroarch\\cores\\gambatte_libretro.dll",
        "--appendconfig=E:\\build\\data\\retroarch\\retroverse-isolated.cfg",
        "--max-frames=300",
        "--sram-mode=noload-nosave",
        "--log-file=E:\\build\\data\\retroarch\\logs\\retroarch.log",
        "--verbose",
        "I:\\Giochi\\Emulatori\\gb\\1 GB US - H-Q\\Mole Mania (USA, Europe) (SGB Enhanced).gb",
    };
    CHECK(retroArchArguments(setup) == expected);
    setup.logFile = "E:\\build\\data\\retroarch\\logs\\retroarch-2.log";
    CHECK(retroArchArguments(setup)[5] == "--log-file=E:\\build\\data\\retroarch\\logs\\retroarch-2.log");
}
