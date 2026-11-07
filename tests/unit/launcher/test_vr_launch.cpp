// I11 step 12 (ADR-0020 §1): emulators.json's programs and the launch adapted to its program, in VR and on the desktop,
// on ES-DE 3.4.1's commands as its translation gives them (each element one argument).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/vr_launch.hpp>

#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

using rv::ErrorCode;
using rv::launcher::AdaptedLaunch;
using rv::launcher::adaptLaunch;
using rv::launcher::AppendConfig;
using rv::launcher::AppendFile;
using rv::launcher::appendFileBytes;
using rv::launcher::EmulatorProfiles;
using rv::launcher::InGameLoad;
using rv::launcher::LaunchRequest;
using rv::launcher::loadProfileDocument;
using rv::launcher::mergeProfileDocuments;
using rv::launcher::parseProfileDocument;
using rv::launcher::ProfileDocument;
using rv::launcher::ProfileOrigin;
using rv::launcher::ProgramProfile;
using rv::launcher::WindowRule;
using rv::launcher::writeAppendFile;
using rv::test::TempDir;
using rv::test::writeProgramStub;

namespace {

using Arguments = std::vector<std::string>;

nlohmann::json parsed(std::string_view text) {
    const auto json = rv::core::parseJson(text, "test");
    REQUIRE(json.has_value());
    return *json;
}

/// A schema 2 document with `programs`.
ProfileDocument withPrograms(const nlohmann::json& programs, ProfileOrigin origin = ProfileOrigin::Shipped) {
    return parseProfileDocument(
        nlohmann::json{{"schemaVersion", 2}, {"profiles", nlohmann::json::array()}, {"programs", programs}},
        "emulators.json", origin);
}

const rv::launcher::ProfileIssue& onlyIssue(const ProfileDocument& document) {
    REQUIRE(document.issues.size() == 1);
    return document.issues.front();
}

/// The shipped document, with its issues shown.
ProfileDocument shipped() {
    ProfileDocument document = loadProfileDocument(
        std::filesystem::path(RV_TEST_ASSET_DIR) / "launcher" / "emulators.json", ProfileOrigin::Shipped);
    for (const auto& issue : document.issues) {
        UNSCOPED_INFO(issue.toString());
    }
    REQUIRE(document.issues.empty());
    return document;
}

EmulatorProfiles shippedProfiles() {
    return mergeProfileDocuments(shipped(), ProfileDocument{});
}

/// A request for the program stub `program` with `arguments`.
LaunchRequest requestFor(const std::filesystem::path& program, Arguments arguments) {
    LaunchRequest request;
    request.executable = program;
    request.arguments = std::move(arguments);
    request.label = "Test Game with test";
    return request;
}

} // namespace

TEST_CASE("launcher/vr_launch: a program with every field", "[launcher][vr_launch]") {
    const ProfileDocument document = withPrograms(parsed(R"([{
        "id": "emu",
        "displayName": "Emu",
        "files": ["emu.exe", "emu-*.exe"],
        "window": {"prefer": [{"classContains": "EmuRender"}], "skip": [{"titleContains": "Updater"}],
                   "minWidth": 320, "minHeight": 240},
        "vr": {
            "addArguments": ["--windowed", "--no-pause"],
            "dropArguments": ["--fullscreen", "-f"],
            "appendConfig": {"option": "--append", "file": "emu.cfg", "joinWith": ";", "lines": ["a = 1", "", "b = 2"]},
            "inGame": "heavy",
            "heavyWith": ["big_*.dll"]
        },
        "verified": true
    }])"));
    REQUIRE(document.issues.empty());
    REQUIRE(document.programs.size() == 1);
    const ProgramProfile& program = document.programs.front();
    CHECK(program.id == "emu");
    CHECK(program.displayName == "Emu");
    CHECK(program.files == Arguments{"emu.exe", "emu-*.exe"});
    REQUIRE(program.window.rules.size() == 2);
    CHECK(program.window.rules[0] == WindowRule{"EmuRender", "", WindowRule::Effect::Prefer});
    CHECK(program.window.rules[1] == WindowRule{"", "Updater", WindowRule::Effect::Skip});
    CHECK(program.window.minWidth == 320);
    CHECK(program.window.minHeight == 240);
    CHECK(program.vr.addArguments == Arguments{"--windowed", "--no-pause"});
    CHECK(program.vr.dropArguments == Arguments{"--fullscreen", "-f"});
    REQUIRE(program.vr.appendConfig.has_value());
    CHECK(program.vr.appendConfig->option == "--append");
    CHECK(program.vr.appendConfig->fileName == "emu.cfg");
    CHECK(program.vr.appendConfig->joinWith == ";");
    CHECK(program.vr.appendConfig->lines == Arguments{"a = 1", "", "b = 2"});
    CHECK(program.vr.inGame == InGameLoad::Heavy);
    CHECK(program.vr.heavyWith == Arguments{"big_*.dll"});
    CHECK(program.verified);
    CHECK(program.origin == ProfileOrigin::Shipped);
    CHECK(program.source == "emulators.json");
}

TEST_CASE("launcher/vr_launch: a program's defaults", "[launcher][vr_launch]") {
    const ProfileDocument document = withPrograms(parsed(R"([{"id": "emu", "files": ["emu.exe"]}])"));
    REQUIRE(document.issues.empty());
    const ProgramProfile& program = document.programs.front();
    CHECK(program.displayName == "emu");
    CHECK(program.window == rv::launcher::WindowRules{});
    CHECK(program.vr == rv::launcher::VrSettings{});
    CHECK(program.vr.inGame == InGameLoad::Light);
    CHECK_FALSE(program.verified);
}

TEST_CASE("launcher/vr_launch: a program in error is left out and named", "[launcher][vr_launch]") {
    struct Case {
        std::string_view name;
        std::string_view program;
        ErrorCode code;
        std::string_view context;
    };
    const Case cases[] = {
        {"no files", R"({"id": "emu"})", ErrorCode::ValidationFailed, "emulators.json:/programs/0"},
        {"empty files", R"({"id": "emu", "files": []})", ErrorCode::ValidationFailed, "emulators.json:/programs/0"},
        {"a file with a folder", R"({"id": "emu", "files": ["bin\\emu.exe"]})", ErrorCode::ValidationFailed,
         "emulators.json:/programs/0/files/0"},
        {"a bad id", R"({"id": "Emu", "files": ["emu.exe"]})", ErrorCode::ValidationFailed,
         "emulators.json:/programs/0/id"},
        {"an unknown field", R"({"id": "emu", "files": ["emu.exe"], "arguments": []})", ErrorCode::ParseError,
         "emulators.json:/programs/0/arguments"},
        {"an append-file in a folder",
         R"({"id": "emu", "files": ["emu.exe"], "vr": {"appendConfig": {"option": "-c", "file": "..\\x.cfg", "lines": []}}})",
         ErrorCode::ValidationFailed, "emulators.json:/programs/0/vr/appendConfig/file"},
        {"a hidden append-file",
         R"({"id": "emu", "files": ["emu.exe"], "vr": {"appendConfig": {"option": "-c", "file": ".cfg", "lines": []}}})",
         ErrorCode::ValidationFailed, "emulators.json:/programs/0/vr/appendConfig/file"},
        {"a line break in a line",
         R"({"id": "emu", "files": ["emu.exe"], "vr": {"appendConfig": {"option": "-c", "file": "e.cfg", "lines": ["a\nb"]}}})",
         ErrorCode::ValidationFailed, "emulators.json:/programs/0/vr/appendConfig/lines/0"},
        {"no lines",
         R"({"id": "emu", "files": ["emu.exe"], "vr": {"appendConfig": {"option": "-c", "file": "e.cfg"}}})",
         ErrorCode::ParseError, "emulators.json:/programs/0/vr/appendConfig"},
        {"an empty argument", R"({"id": "emu", "files": ["emu.exe"], "vr": {"addArguments": [""]}})",
         ErrorCode::ValidationFailed, "emulators.json:/programs/0/vr/addArguments/0"},
        {"an unknown load", R"({"id": "emu", "files": ["emu.exe"], "vr": {"inGame": "medium"}})",
         ErrorCode::ValidationFailed, "emulators.json:/programs/0/vr/inGame"},
        {"a rule without text", R"({"id": "emu", "files": ["emu.exe"], "window": {"prefer": [{}]}})",
         ErrorCode::ValidationFailed, "emulators.json:/programs/0/window/prefer/0"},
        {"a negative size", R"({"id": "emu", "files": ["emu.exe"], "window": {"minWidth": -1}})",
         ErrorCode::ValidationFailed, "emulators.json:/programs/0/window/minWidth"},
    };
    for (const Case& test : cases) {
        INFO(test.name);
        const ProfileDocument document = withPrograms(nlohmann::json::array({parsed(test.program)}));
        CHECK(document.programs.empty());
        CHECK(onlyIssue(document).error.code == test.code);
        CHECK(onlyIssue(document).error.context == test.context);
    }
    SECTION("the other programs are kept, a second id is refused") {
        const ProfileDocument document = withPrograms(parsed(
            R"([{"id": "a", "files": ["a.exe"]}, {"id": "b"}, {"id": "a", "files": ["other.exe"]}, {"id": "c", "files": ["c.exe"]}])"));
        REQUIRE(document.programs.size() == 2);
        CHECK(document.programs[0].id == "a");
        CHECK(document.programs[0].files == Arguments{"a.exe"});
        CHECK(document.programs[1].id == "c");
        REQUIRE(document.issues.size() == 2);
        CHECK(document.issues[0].profileId == "b");
        CHECK(document.issues[1].profileId == "a");
        CHECK(document.issues[1].error.context == "emulators.json:/programs/2/id");
    }
    SECTION("programs need schemaVersion 2; the profiles still load") {
        const ProfileDocument document = parseProfileDocument(
            parsed(R"({"schemaVersion": 1, "profiles": [], "programs": [{"id": "a", "files": ["a.exe"]}]})"),
            "emulators.json", ProfileOrigin::User);
        CHECK(document.programs.empty());
        CHECK(onlyIssue(document).error.code == ErrorCode::ParseError);
        CHECK(onlyIssue(document).error.message.find("schemaVersion 2") != std::string::npos);
    }
    SECTION("schemaVersion 3 is not read") {
        const ProfileDocument document = parseProfileDocument(parsed(R"({"schemaVersion": 3, "profiles": []})"),
                                                              "emulators.json", ProfileOrigin::User);
        CHECK(onlyIssue(document).error.code == ErrorCode::Unsupported);
    }
}

TEST_CASE("launcher/vr_launch: a program matches its file names", "[launcher][vr_launch]") {
    const ProfileDocument document =
        withPrograms(parsed(R"([{"id": "duck", "files": ["duckstation-qt-*.exe", "Duck.exe"]}])"));
    REQUIRE(document.programs.size() == 1);
    const ProgramProfile& program = document.programs.front();
    CHECK(program.matches(R"(C:\Emulators\duckstation-qt-x64-ReleaseLTCG.exe)"));
    CHECK(program.matches(R"(D:\Duck\DUCK.EXE)")); // the file-system casing
    CHECK_FALSE(program.matches(R"(C:\Emulators\duckstation-nogui-x64-ReleaseLTCG.exe)"));
    CHECK_FALSE(program.matches(R"(C:\Emulators\Duck.exe.bak)"));
    CHECK_FALSE(program.matches(R"(C:\Duck.exe\other.exe)")); // the file name only
}

TEST_CASE("launcher/vr_launch: the user's programs replace the shipped ones and are matched first",
          "[launcher][vr_launch]") {
    ProfileDocument shippedDocument = withPrograms(parsed(
        R"([{"id": "a", "files": ["a.exe"], "vr": {"addArguments": ["--shipped"]}}, {"id": "b", "files": ["b.exe"]}])"));
    ProfileDocument userDocument = withPrograms(
        parsed(
            R"([{"id": "a", "files": ["a.exe"], "vr": {"addArguments": ["--user"]}}, {"id": "mine", "files": ["b.exe"]}])"),
        ProfileOrigin::User);
    const EmulatorProfiles profiles = mergeProfileDocuments(std::move(shippedDocument), std::move(userDocument));
    REQUIRE(profiles.programs.size() == 3);
    CHECK(profiles.programs[0].id == "a");
    CHECK(profiles.programs[0].origin == ProfileOrigin::User);
    CHECK(profiles.programs[0].vr.addArguments == Arguments{"--user"});
    CHECK(profiles.programs[1].id == "b");
    CHECK(profiles.programs[2].id == "mine");
    REQUIRE(profiles.programFor(R"(C:\x\B.exe)") != nullptr);
    CHECK(profiles.programFor(R"(C:\x\B.exe)")->id == "mine");
    CHECK(profiles.programFor(R"(C:\x\a.exe)")->id == "a");
    CHECK(profiles.programFor(R"(C:\x\c.exe)") == nullptr);
}

TEST_CASE("launcher/vr_launch: the shipped programs", "[launcher][vr_launch]") {
    const EmulatorProfiles profiles = shippedProfiles();
    // The programs of ADR-0020 §1, by the file names ES-DE's find rules give.
    const std::pair<std::string_view, std::string_view> expected[] = {
        {"retroarch.exe", "retroarch"},
        {"Dolphin.exe", "dolphin"},
        {"pcsx2-qt.exe", "pcsx2"},
        {"pcsx2-qtx64-avx2.exe", "pcsx2"},
        {"duckstation-qt-x64-ReleaseLTCG.exe", "duckstation"},
        {"rpcs3.exe", "rpcs3"},
        {"AppleWin.exe", "applewin"},
        {"Altirra64.exe", "altirra"},
        {"sdl2trs64.exe", "sdl2trs"},
    };
    for (const auto& [file, id] : expected) {
        INFO(file);
        const ProgramProfile* program = profiles.programFor(std::filesystem::path("C:\\Emulators") / file);
        REQUIRE(program != nullptr);
        CHECK(program->id == id);
        CHECK_FALSE(program->verified);
    }
    CHECK(profiles.programFor(R"(C:\Emulators\cmd.exe)") == nullptr);
    for (const ProgramProfile& program : profiles.programs) {
        INFO(program.id);
        // A program never asks for exclusive fullscreen in VR: its full-screen switches are dropped.
        for (const std::string& added : program.vr.addArguments) {
            CHECK(added.find("ullscreen=True") == std::string::npos);
        }
    }
}

TEST_CASE("launcher/vr_launch: ES-DE 3.4.1's commands in VR and on the desktop", "[launcher][vr_launch]") {
    const TempDir dir("rv-vr-launch");
    const std::filesystem::path vrFolder = dir.file("data/launcher/vr");
    const std::string vrFile = (vrFolder / "retroarch.cfg").lexically_normal().make_preferred().string();
    const EmulatorProfiles profiles = shippedProfiles();
    const std::string rom = dir.file("roms/Test Game (World).iso").make_preferred().string();
    const auto adapt = [&](std::string_view file, Arguments arguments, bool vr) {
        const std::filesystem::path program = writeProgramStub(dir.file("emulators/" + std::string(file)));
        return adaptLaunch(requestFor(program, std::move(arguments)), profiles.programFor(program), vr, vrFolder);
    };

    SECTION("RetroArch: the append-file in front, light or heavy by its core") {
        const std::string core = R"(C:\RetroArch\cores\snes9x_libretro.dll)";
        const AdaptedLaunch desktop = adapt("retroarch.exe", {"-L", core, rom}, false);
        CHECK(desktop.programId == "retroarch");
        CHECK(desktop.request.arguments == Arguments{"-L", core, rom});
        CHECK_FALSE(desktop.appendFile.has_value());
        CHECK(desktop.added.empty());
        REQUIRE(desktop.request.windowRules.rules.size() == 1);
        CHECK(desktop.request.windowRules.rules[0].classContains == "RetroArch");

        const AdaptedLaunch vr = adapt("retroarch.exe", {"-L", core, rom}, true);
        CHECK_FALSE(vr.refused.has_value());
        CHECK(vr.request.arguments == Arguments{"--appendconfig", vrFile, "-L", core, rom});
        CHECK(vr.added == Arguments{"--appendconfig", vrFile});
        CHECK(vr.dropped.empty());
        CHECK(vr.inGame == InGameLoad::Light);
        REQUIRE(vr.appendFile.has_value());
        CHECK(vr.appendFile->path.string() == vrFile);
        CHECK(vr.appendFile->bytes ==
              "# Written by Retroverse for games played in VR, read over retroarch.cfg; rewritten at each launch in "
              "VR.\n"
              "video_fullscreen = \"true\"\n"
              "video_windowed_fullscreen = \"true\"\n"
              "pause_nonactive = \"false\"\n"
              "config_save_on_exit = \"false\"\n");
        CHECK(vr.request.windowRules == desktop.request.windowRules);

        const AdaptedLaunch heavy =
            adapt("retroarch.exe", {"-L", R"(C:\RetroArch\cores\dolphin_libretro.dll)", rom}, true);
        CHECK(heavy.inGame == InGameLoad::Heavy);
    }
    SECTION("RetroArch: an append-file the launch already gives is joined") {
        const AdaptedLaunch vr =
            adapt("retroarch.exe", {"--appendconfig", R"(C:\mine.cfg)", "-L", "core.dll", rom}, true);
        CHECK(vr.request.arguments ==
              Arguments{"--appendconfig", std::string(R"(C:\mine.cfg)") + "|" + vrFile, "-L", "core.dll", rom});
        CHECK(vr.added == Arguments{"|" + vrFile});
    }
    SECTION("Dolphin (Standalone): -b -e %ROM%") {
        const AdaptedLaunch desktop = adapt("Dolphin.exe", {"-b", "-e", rom}, false);
        CHECK(desktop.request.arguments == Arguments{"-b", "-e", rom});
        REQUIRE(desktop.request.windowRules.rules.size() == 1);
        CHECK(desktop.request.windowRules.rules[0].titleContains == " | ");
        const AdaptedLaunch vr = adapt("Dolphin.exe", {"-b", "-e", rom}, true);
        CHECK(vr.request.arguments == Arguments{"-C", "Dolphin.Display.Fullscreen=False", "-C",
                                                "Dolphin.Display.RenderToMain=False", "-C",
                                                "Dolphin.Interface.PauseOnFocusLost=False", "-C",
                                                "Dolphin.Input.BackgroundInput=True", "-b", "-e", rom});
        CHECK(vr.inGame == InGameLoad::Heavy);
        CHECK_FALSE(vr.appendFile.has_value());
    }
    SECTION("PCSX2 (Standalone): -batch %ROM%") {
        CHECK(adapt("pcsx2-qt.exe", {"-batch", rom}, false).request.arguments == Arguments{"-batch", rom});
        const AdaptedLaunch vr = adapt("pcsx2-qt.exe", {"-batch", "-fullscreen", rom}, true);
        CHECK(vr.request.arguments == Arguments{"-nofullscreen", "-nogui", "-batch", rom});
        CHECK(vr.dropped == Arguments{"-fullscreen"});
        CHECK(vr.inGame == InGameLoad::Heavy);
    }
    SECTION("DuckStation (Standalone): -batch %ROM%") {
        const AdaptedLaunch vr = adapt("duckstation-qt-x64-ReleaseLTCG.exe", {"-batch", rom}, true);
        CHECK(vr.request.arguments == Arguments{"-nofullscreen", "-nogui", "-batch", rom});
        CHECK(vr.inGame == InGameLoad::Light);
    }
    SECTION("RPCS3 (Standalone): --no-gui %ROM%") {
        CHECK(adapt("rpcs3.exe", {"--no-gui", rom}, true).request.arguments == Arguments{"--no-gui", rom});
        const AdaptedLaunch vr = adapt("rpcs3.exe", {"--no-gui", "--fullscreen", rom}, true);
        CHECK(vr.request.arguments == Arguments{"--no-gui", rom});
        CHECK(vr.dropped == Arguments{"--fullscreen"});
    }
    SECTION("AppleWin (Standalone): -f -d1 %ROM%") {
        CHECK(adapt("AppleWin.exe", {"-f", "-d1", rom}, false).request.arguments == Arguments{"-f", "-d1", rom});
        const AdaptedLaunch vr = adapt("AppleWin.exe", {"-f", "-d1", rom}, true);
        CHECK(vr.request.arguments == Arguments{"-no-full-screen", "-d1", rom});
        CHECK(vr.dropped == Arguments{"-f"});
    }
    SECTION("Altirra and sdl2trs: windowed already") {
        CHECK(adapt("Altirra64.exe", {rom}, true).request.arguments == Arguments{rom});
        const Arguments trs{"-rom", R"(C:\roms\trs-80\level2.rom)", "-disk0", rom};
        CHECK(adapt("sdl2trs64.exe", trs, true).request.arguments == trs);
    }
    SECTION("a program Retroverse does not know is launched as asked") {
        const AdaptedLaunch vr = adapt("unknown-emu.exe", {"-fullscreen", rom}, true);
        CHECK(vr.programId.empty());
        CHECK(vr.request.arguments == Arguments{"-fullscreen", rom});
        CHECK(vr.request.windowRules == rv::launcher::WindowRules{});
        CHECK(vr.inGame == InGameLoad::Light);
    }
}

TEST_CASE("launcher/vr_launch: a VR request Windows would refuse keeps the one asked for", "[launcher][vr_launch]") {
    const TempDir dir("rv-vr-launch-refused");
    const std::filesystem::path program = writeProgramStub(dir.file("emu.exe"));
    const ProfileDocument document =
        withPrograms(nlohmann::json::array({{{"id", "emu"},
                                             {"files", {"emu.exe"}},
                                             {"window", {{"minWidth", 10}}},
                                             {"vr", {{"addArguments", {std::string(1000, 'x')}}}}}}));
    REQUIRE(document.programs.size() == 1);
    // Close to the Windows limit of 32,767 characters: 1,000 more do not fit.
    const Arguments arguments(32, std::string(1000, 'r'));
    const AdaptedLaunch adapted =
        adaptLaunch(requestFor(program, arguments), &document.programs.front(), true, dir.file("vr"));
    REQUIRE(adapted.refused.has_value());
    CHECK(adapted.request.arguments == arguments);
    CHECK(adapted.added.empty());
    CHECK(adapted.request.windowRules.minWidth == 10);
}

TEST_CASE("launcher/vr_launch: the append-file is Retroverse's own, written only when it changes",
          "[launcher][vr_launch]") {
    const TempDir dir("rv-vr-append");
    // An emulator's own configuration beside it: never opened for writing (ADR-0020 §1).
    const std::filesystem::path emulatorConfig = dir.file("RetroArch/retroarch.cfg");
    std::filesystem::create_directories(emulatorConfig.parent_path());
    {
        std::ofstream(emulatorConfig, std::ios::binary) << "video_fullscreen = \"false\"\n";
    }
    std::filesystem::permissions(emulatorConfig, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::remove);
    const auto configTime = std::filesystem::last_write_time(emulatorConfig);

    const EmulatorProfiles profiles = shippedProfiles();
    const std::filesystem::path program = writeProgramStub(dir.file("RetroArch/retroarch.exe"));
    const AdaptedLaunch adapted = adaptLaunch(requestFor(program, {"-L", "core.dll", "game.sfc"}),
                                              profiles.programFor(program), true, dir.file("data/launcher/vr"));
    REQUIRE(adapted.appendFile.has_value());
    REQUIRE(writeAppendFile(*adapted.appendFile));
    auto written = rv::core::readTextFile(adapted.appendFile->path);
    REQUIRE(written);
    CHECK(*written == adapted.appendFile->bytes);

    // The same bytes: the file is left as it is.
    const auto writtenTime = std::filesystem::last_write_time(adapted.appendFile->path);
    REQUIRE(writeAppendFile(*adapted.appendFile));
    CHECK(std::filesystem::last_write_time(adapted.appendFile->path) == writtenTime);

    // Other bytes replace it.
    AppendFile changed = *adapted.appendFile;
    changed.bytes += "extra = \"1\"\n";
    REQUIRE(writeAppendFile(changed));
    written = rv::core::readTextFile(changed.path);
    REQUIRE(written);
    CHECK(*written == changed.bytes);

    // The emulator's configuration is as it was.
    auto config = rv::core::readTextFile(emulatorConfig);
    REQUIRE(config);
    CHECK(*config == "video_fullscreen = \"false\"\n");
    CHECK(std::filesystem::last_write_time(emulatorConfig) == configTime);
    std::filesystem::permissions(emulatorConfig, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
}

TEST_CASE("launcher/vr_launch: the append-file's bytes", "[launcher][vr_launch]") {
    CHECK(appendFileBytes(AppendConfig{"-c", "a.cfg", "", {}}).empty());
    CHECK(appendFileBytes(AppendConfig{"-c", "a.cfg", "", {"a = 1", "", "b = \"two words\""}}) ==
          "a = 1\n\nb = \"two words\"\n");
}
