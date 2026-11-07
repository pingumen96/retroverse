// I2 step 10: ES-DE commands translated into launch requests (ADR-0008 §8). The token table; the program rule (one
// %EMULATOR_X% element or a literal absolute path) and the shells refused by name; the quirks of the commands in use
// (the apple2 "MAME - Current" command's nested escaped quotes, -image=%ROM%, ';' lists, trailing backslashes before
// quotes, '%' and '^' in names), each checked against an independent oracle: ES-DE's own string expansion parsed by the
// real CommandLineToArgvW; %STARTDIR% and %HIDEWINDOW%; the %INJECT% cases; the tokens that require launcher
// configuration, named; a system's readiness without a game.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")

#include <rv/core/system_probe.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/settings.hpp>

#include "launch_test_machine.hpp"

namespace fs = std::filesystem;
using rv::catalog::LaunchReadiness;
using rv::esde::FindRule;
using rv::esde::FindRuleType;

namespace {

std::string text(const fs::path& path) {
    return rv::core::pathToUtf8(path);
}

std::wstring widen(std::string_view utf8) {
    const int units = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(units), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), units);
    return wide;
}

std::string narrow(std::wstring_view wide) {
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

/// The real CommandLineToArgvW.
std::vector<std::string> shellSplit(const std::string& commandLine) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(widen(commandLine).c_str(), &count);
    REQUIRE(argv != nullptr);
    std::vector<std::string> elements;
    for (int i = 0; i < count; ++i) {
        elements.push_back(narrow(argv[i]));
    }
    LocalFree(argv);
    return elements;
}

std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

/// ES-DE's getEscapedPath on Windows: quoted when it holds a space.
std::string escaped(const std::string& path) {
    return path.find(' ') != std::string::npos ? "\"" + path + "\"" : path;
}

/// The independent oracle: ES-DE 3.4.1's string expansion of a command (FileData::launchGame, for commands without
/// flags, %STARTDIR% or %INJECT%), then CommandLineToArgvW. Values as ES-DE computes them, its quirks included
/// (%FILENAME% takes the extension of the quoted %ROM%, so it ends with a quote when the path holds a space).
std::vector<std::string> esdeArgv(std::string command, const std::string& emulator, const std::string& coreFolder,
                                  const std::string& rom, const std::string& romDirectory) {
    const std::size_t slash = rom.rfind('\\');
    const std::string gameDir = rom.substr(0, slash);
    const std::string name = rom.substr(slash + 1);
    const std::string stem = name.substr(0, name.rfind('.'));
    const std::string romPath = escaped(rom);
    const std::string extension = romPath.substr(romPath.rfind('.'));
    command = replaceAll(std::move(command), "%EMULATOR_RA%", escaped(emulator));
    command = replaceAll(std::move(command), "%CORE_RA%", coreFolder);
    command = replaceAll(std::move(command), "%ROM%", romPath);
    command = replaceAll(std::move(command), "%BASENAME%", stem);
    command = replaceAll(std::move(command), "%FILENAME%", stem + extension);
    command = replaceAll(std::move(command), "%ROMRAW%", rom);
    command = replaceAll(std::move(command), "%ROMPATH%", escaped(romDirectory));
    command = replaceAll(std::move(command), "%GAMEDIR%", escaped(gameDir));
    command = replaceAll(std::move(command), "%GAMEDIRRAW%", gameDir);
    return shellSplit(command);
}

/// argv of a request: the program, then the arguments.
std::vector<std::string> argvOf(const rv::launcher::LaunchRequest& request) {
    std::vector<std::string> argv{text(request.executable)};
    argv.insert(argv.end(), request.arguments.begin(), request.arguments.end());
    return argv;
}

/// A machine with RetroArch-like emulator "RA" under the install folder and its core folder.
struct RaMachine {
    rv::test::LaunchMachine machine;
    fs::path exe;
    fs::path cores;

    RaMachine() {
        exe = machine.program(machine.install() / "Emulators" / "RetroArch" / "retroarch.exe");
        cores = exe.parent_path() / "cores";
        machine.emulator("RA", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\Emulators\\RetroArch\\retroarch.exe"}}});
        machine.core("RA", {"%EMUPATH%\\cores"});
    }

    rv::catalog::LaunchTranslation translate(const std::string& command, const rv::catalog::LaunchGame* game,
                                             std::string_view system = "gb") {
        const rv::esde::SystemDefinition definition = rv::test::testSystem({{"Test", command}}, system);
        return rv::esde::translateCommand(machine.context(), definition, definition.commands.front(), game);
    }
};

} // namespace

TEST_CASE("esde/launch translation: the token table", "[esde][launch]") {
    RaMachine ra;
    const fs::path rom = ra.machine.file(ra.machine.roms() / "gb" / "Sub Dir" / "Mole (USA).gb");
    const rv::catalog::LaunchGame game = rv::test::testGame("Sub Dir/Mole (USA).gb");
    const auto translation =
        ra.translate("%EMULATOR_RA% %ROM% %ROMRAW% %ROMPATH% %BASENAME% %FILENAME% %GAMEDIR% %GAMEDIRRAW% %EMUDIR% "
                     "%ESPATH% ~\\cfg \"%ROM%\" -image=%ROM% 50%-%ROM% x%y%ROM%",
                     &game);
    INFO(rv::test::blockerText(translation));
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    REQUIRE(translation.request);
    const std::string romText = text(rom);
    const std::string gameDir = text(rom.parent_path());
    const std::string home = rv::esde::esdeGenericText(ra.machine.home()); // ES-DE's text of '~': '/' separators
    CHECK(translation.request->arguments ==
          std::vector<std::string>{romText, romText, text(ra.machine.roms()), "Mole (USA)", "Mole (USA).gb", gameDir,
                                   gameDir, text(ra.exe.parent_path()), text(ra.machine.install()), home + "\\cfg",
                                   romText, "-image=" + romText, "50%-" + romText, "x%y" + romText});
    CHECK(translation.request->executable == ra.exe);
    CHECK(translation.request->workingDirectory == ra.exe.parent_path()); // no %STARTDIR%: the emulator's folder
    CHECK_FALSE(translation.request->hideConsoleWindow);
    CHECK(translation.request->label == "Mole (USA).gb with Test");
    REQUIRE(translation.program);
    CHECK(translation.program->rule == "staticpath");
}

TEST_CASE("esde/launch translation: %BASENAME% and %FILENAME% as ES-DE builds them", "[esde][launch]") {
    RaMachine ra;
    const auto values = [&](const std::string& relative, bool directory) {
        const rv::catalog::LaunchGame game = rv::test::testGame(relative, directory);
        const auto translation = ra.translate("%EMULATOR_RA% %BASENAME% %FILENAME%", &game);
        REQUIRE(translation.request);
        return translation.request->arguments;
    };
    CHECK(values("Game.v1.2.zip", false) == std::vector<std::string>{"Game.v1.2", "Game.v1.2.zip"});
    // No extension: ES-DE's getExtension gives ".", which %FILENAME% gains.
    CHECK(values("runme", false) == std::vector<std::string>{"runme", "runme."});
    // A folder game: getStem keeps a folder's whole name, and %FILENAME% repeats its extension.
    CHECK(values("Game (USA).ps3", true) == std::vector<std::string>{"Game (USA).ps3", "Game (USA).ps3.ps3"});
    CHECK(values(".Dot Hero (World).gb", false) ==
          std::vector<std::string>{".Dot Hero (World)", ".Dot Hero (World).gb"});
}

TEST_CASE("esde/launch translation: a folder game holding a file of its own name, as ES-DE launches it",
          "[esde][launch]") {
    RaMachine ra;
    const fs::path folder = ra.machine.roms() / "psx" / "Final Fantasy VII.m3u";
    const fs::path inner = ra.machine.file(folder / "Final Fantasy VII.m3u", "disc1.chd\r\n");
    const rv::catalog::LaunchGame game = rv::test::testGame("Final Fantasy VII.m3u", true);
    const auto translate = [&](const std::string& command, const rv::catalog::LaunchGame& which) {
        return ra.translate(command, &which, "psx");
    };

    SECTION("%ROM% is the inner file, %ROMRAW% the folder, %BASENAME% cut at the first '.'") {
        const auto translation =
            translate("%EMULATOR_RA% %ROM% %ROMRAW% %BASENAME% %FILENAME% %GAMEDIR% %GAMEDIRRAW%", game);
        INFO(rv::test::blockerText(translation));
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(translation.request->arguments == std::vector<std::string>{text(inner), text(folder),
                                                                         "Final Fantasy VII", "Final Fantasy VII.m3u",
                                                                         text(folder), text(folder)});
        REQUIRE_FALSE(translation.notes.empty());
        CHECK(translation.notes.front().find("its own name") != std::string::npos);
        // The first '.', not the last: "Super Mario Bros. 3.m3u" gives "Super Mario Bros".
        const fs::path dotted = ra.machine.roms() / "psx" / "Super Mario Bros. 3.m3u";
        ra.machine.file(dotted / "Super Mario Bros. 3.m3u");
        const auto cut = translate("%EMULATOR_RA% %BASENAME% %FILENAME%",
                                   rv::test::testGame("Super Mario Bros. 3.m3u", true));
        REQUIRE(cut.request);
        CHECK(cut.request->arguments == std::vector<std::string>{"Super Mario Bros", "Super Mario Bros.m3u"});
    }
    SECTION("%STARTDIR%=%GAMEDIR% and a relative %INJECT% file are in the folder; %INJECT%=%ROM% is the folder") {
        auto translation = translate("%STARTDIR%=%GAMEDIR% %EMULATOR_RA% %ROM%", game);
        REQUIRE(translation.request);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK(translation.request->workingDirectory == folder);
        ra.machine.file(folder / "Final Fantasy VII.cfg", "-fullscreen");
        translation = translate("%EMULATOR_RA% %INJECT%=%BASENAME%.cfg %ROM%", game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"inject-file"});
        CHECK(rv::test::blockerText(translation).find(text(folder / "Final Fantasy VII.cfg")) != std::string::npos);
        // romRaw, the folder, is no file: nothing injected.
        translation = translate("%EMULATOR_RA% %INJECT%=%ROM% %ROM%", game);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(translation.request->arguments == std::vector<std::string>{text(inner)});
    }
    SECTION("only a file of exactly the folder's name: not another case, not a folder") {
        const fs::path otherCase = ra.machine.roms() / "psx" / "Case Game.m3u";
        ra.machine.file(otherCase / "case game.m3u");
        auto translation =
            translate("%EMULATOR_RA% %ROM% %BASENAME%", rv::test::testGame("Case Game.m3u", true));
        REQUIRE(translation.request);
        CHECK(translation.request->arguments == std::vector<std::string>{text(otherCase), "Case Game.m3u"});
        const fs::path nested = ra.machine.roms() / "psx" / "Nested.m3u";
        fs::create_directories(nested / "Nested.m3u");
        translation = translate("%EMULATOR_RA% %ROM% %GAMEDIR%", rv::test::testGame("Nested.m3u", true));
        REQUIRE(translation.request);
        CHECK(translation.request->arguments == std::vector<std::string>{text(nested), text(nested.parent_path())});
        CHECK(translation.notes.empty());
    }
}

TEST_CASE("esde/launch translation: the commands in use, argv for argv with ES-DE's own expansion", "[esde][launch]") {
    RaMachine ra;
    const std::string romDirectory = text(ra.machine.roms());
    if (romDirectory.find(' ') != std::string::npos) {
        SKIP("the temporary folder holds a space: ES-DE's own argv breaks then (a deliberate difference)");
    }
    ra.machine.listed(ra.cores / "mame_libretro.dll");
    const fs::path rom = ra.machine.file(ra.machine.roms() / "apple2" / "Disk Images" / "Lode Runner (USA).dsk");
    const rv::catalog::LaunchGame game = rv::test::testGame("Disk Images/Lode Runner (USA).dsk");
    const std::vector<std::string> commands{
        // apple2 "MAME - Current": nested escaped quotes, %GAMEDIRRAW%, %ROMPATH%, %FILENAME% (ES-DE's own quirk).
        R"(%EMULATOR_RA% -L %CORE_RA%\mame_libretro.dll "apple2e -rompath \"%GAMEDIRRAW%;%ROMPATH%\apple2\" -gameio joy -flop1 \"%GAMEDIRRAW%\%FILENAME%\"")",
        // MAME's ';' lists of folders.
        R"(%EMULATOR_RA% -rompath %GAMEDIR%\;%ROMPATH%\adam adam -cass1 %ROM%)",
        // %ROM% glued to an option.
        R"(%EMULATOR_RA% -image=%ROM% -source:%ROM% "%ROMRAW%")",
        // Backslashes before quotes: two before a closing one, one before a literal one.
        R"(%EMULATOR_RA% -dir "%GAMEDIRRAW%\\" -x "%GAMEDIRRAW%\" -y %ROMRAW%")",
    };
    for (const std::string& command : commands) {
        INFO(command);
        const auto translation = ra.translate(command, &game, "apple2");
        INFO(rv::test::blockerText(translation));
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(argvOf(*translation.request) ==
              esdeArgv(command, text(ra.exe), text(ra.cores), text(rom), romDirectory));
    }
    // The apple2 argument as the emulator receives it.
    const auto apple2 = ra.translate(commands[0], &game, "apple2");
    REQUIRE(apple2.request);
    const std::string gameDir = text(rom.parent_path());
    CHECK(apple2.request->arguments.at(2) == "apple2e -rompath \"" + gameDir + ";" + romDirectory +
                                                 "\\apple2\" -gameio joy -flop1 \"" + gameDir +
                                                 "\\Lode Runner (USA).dsk\"");
    CHECK(apple2.request->arguments.at(1) == text(ra.cores / "mame_libretro.dll"));
    REQUIRE(apple2.core);
    CHECK(apple2.core->entry == "%EMUPATH%\\cores");
}

TEST_CASE("esde/launch translation: hostile names stay one argument, never expanded again", "[esde][launch]") {
    RaMachine ra;
    for (const std::string name : {"100% ^Fun^ & More; (USA).gb", "Game %BASENAME% %ROM% 50%.gb", "Quote's ~ (J).gb",
                                   "Ünïcödé 日本 (Japan).gb"}) {
        INFO(name);
        const fs::path rom = ra.machine.file(ra.machine.roms() / "gb" / rv::core::pathFromUtf8(name));
        const rv::catalog::LaunchGame game = rv::test::testGame(name);
        const auto translation = ra.translate("%EMULATOR_RA% %ROM% %BASENAME%", &game);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        REQUIRE(translation.request->arguments.size() == 2);
        CHECK(translation.request->arguments[0] == text(rom));
        CHECK(translation.request->arguments[1] == name.substr(0, name.rfind('.')));
    }
}

TEST_CASE("esde/launch translation: the program rule and the refused shells", "[esde][launch]") {
    RaMachine ra;
    const rv::catalog::LaunchGame game = rv::test::testGame("Mole (USA).gb");
    ra.machine.file(ra.machine.roms() / "gb" / "Mole (USA).gb");
    ra.machine.emulator("OS-SHELL", {FindRule{FindRuleType::SystemPath, {"cmd.exe"}}});
    ra.machine.program(ra.machine.root() / "Windows" / "System32" / "cmd.exe");
    ra.machine.emulator("PWSH", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\Tools\\PowerShell.EXE"}}});
    ra.machine.program(ra.machine.install() / "Tools" / "PowerShell.EXE");

    SECTION("cmd.exe found by its find rule, refused by name; %ESCAPESPECIALS% named") {
        const auto translation = ra.translate("%HIDEWINDOW% %ESCAPESPECIALS% %EMULATOR_OS-SHELL% /K %ROM%", &game);
        CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"token-unsupported", "program-refused"});
        CHECK(rv::test::blockerText(translation).find("%ESCAPESPECIALS%") != std::string::npos);
        CHECK(rv::test::blockerText(translation).find("cmd.exe") != std::string::npos);
        REQUIRE(translation.program);
        CHECK(translation.program->rule == "systempath");
    }
    SECTION("\"open the game's file\" never runs cmd.exe: a ROM is no file Retroverse opens (I11 step 13)") {
        const auto translation = ra.translate("%HIDEWINDOW% %ESCAPESPECIALS% %EMULATOR_OS-SHELL% /C %ROM%", &game);
        CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"file-not-opened"});
        CHECK_FALSE(translation.program);
    }
    SECTION("\"open the game's file\" whose shell is not cmd.exe: refused as before") {
        const auto translation = ra.translate("%HIDEWINDOW% %ESCAPESPECIALS% %EMULATOR_PWSH% /C %ROM%", &game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"program-refused", "token-unsupported"});
    }
    SECTION("a launcher found under another case and extension's case") {
        const auto translation = ra.translate("%EMULATOR_PWSH% -File %ROM%", &game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"program-refused"});
    }
    SECTION("a shell's 8.3 alias, refused by the file it opens") {
        const fs::path shell = ra.machine.program(ra.machine.root() / "Shell Tools" / "powershell.exe");
        std::wstring shortPath(MAX_PATH, L'\0');
        const DWORD length = GetShortPathNameW(shell.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
        REQUIRE(length > 0);
        shortPath.resize(length);
        const fs::path alias(shortPath);
        if (alias.filename() == shell.filename()) {
            SKIP("the volume of the temporary folder makes no 8.3 names");
        }
        // Registered in App Paths under its short name (a command could not hold it: ES-DE replaces every '~').
        ra.machine.listed(alias);
        ra.machine.probe.setRegistryString(rv::core::RegistryRoot::LocalMachine,
                                           "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\shell.exe", "",
                                           text(alias));
        ra.machine.emulator("SHORT", {FindRule{FindRuleType::WinRegistryPath, {"shell.exe"}}});
        const auto translation = ra.translate("%EMULATOR_SHORT% -File %ROM%", &game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"program-refused"});
        CHECK(rv::test::blockerText(translation).find("it opens powershell.exe") != std::string::npos);
        REQUIRE(translation.request);
        CHECK(translation.request->executable == alias); // the path as found
    }
    SECTION("a symbolic link named like an emulator, refused by the shell it opens") {
        const fs::path shell = ra.machine.program(ra.machine.root() / "Shells" / "cmd.exe");
        const fs::path link = ra.machine.root() / "Emus" / "emu.exe";
        fs::create_directories(link.parent_path());
        std::error_code error;
        fs::create_symlink(shell, link, error);
        if (error) {
            SKIP("this user cannot make symbolic links: " + error.message());
        }
        ra.machine.listed(link);
        ra.machine.emulator("LINKED", {FindRule{FindRuleType::StaticPath, {text(link)}}});
        const auto translation = ra.translate("%EMULATOR_LINKED% %ROM%", &game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"program-refused"});
        CHECK(rv::test::blockerText(translation).find("emu.exe (it opens cmd.exe)") != std::string::npos);
    }
    SECTION("a literal absolute program, quoted or not: found, or emulator not found") {
        const fs::path dolphin = ra.machine.program(ra.machine.root() / "Emu Dir" / "Dolphin.exe");
        auto translation = ra.translate("\"" + text(dolphin) + "\" -b -e %ROM%", &game);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.program);
        CHECK(translation.program->rule == "command");
        translation = ra.translate("K:\\Giochi\\Emulatori\\PS2\\pcsx2-qt.exe -batch %ROM%", &game);
        CHECK(translation.readiness() == LaunchReadiness::EmulatorNotFound);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"emulator-not-found"});
        // '~' and %ESPATH% are the only tokens a literal program may hold.
        ra.machine.program(ra.machine.home() / "Emus" / "home.exe");
        translation = ra.translate("~\\Emus\\home.exe %ROM%", &game);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        ra.machine.program(ra.machine.install() / "Emus" / "es.exe");
        translation = ra.translate("%ESPATH%\\Emus\\es.exe %ROM%", &game);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
    }
    SECTION("anything else is no program") {
        for (const std::string command : {"%ROM%", "PLAIN %ROM%", "%EMULATOR_RA%x %ROM%", "pre%EMULATOR_RA% %ROM%",
                                          "C:\\Emus\\%BASENAME%.exe"}) {
            INFO(command);
            const auto translation = ra.translate(command, &game);
            CHECK(rv::test::blockerCodes(translation).front() == "program-not-allowed");
            CHECK_FALSE(translation.request);
        }
        const auto none = ra.translate("%STARTDIR%=\"C:\\Games %EMULATOR_RA% %ROM%", &game);
        CHECK(rv::test::blockerCodes(none) == std::vector<std::string>{"program-missing"});
    }
    SECTION("placeholders, missing find rules") {
        rv::esde::SystemDefinition system = rv::test::testSystem({{"", "PLACEHOLDER %ROM%"}});
        system.commands.front().placeholder = true;
        const auto placeholder =
            rv::esde::translateCommand(ra.machine.context(), system, system.commands.front(), &game);
        CHECK(rv::test::blockerCodes(placeholder) == std::vector<std::string>{"command-placeholder"});
        const auto noRules = ra.translate("%EMULATOR_UNKNOWN% %ROM%", &game);
        CHECK(rv::test::blockerCodes(noRules) == std::vector<std::string>{"find-rules-missing"});
        CHECK(noRules.readiness() == LaunchReadiness::RequiresConfiguration);
    }
}

TEST_CASE("esde/launch translation: tokens that require launcher configuration, each named once", "[esde][launch]") {
    RaMachine ra;
    const rv::catalog::LaunchGame game = rv::test::testGame("Mole (USA).gb");
    ra.machine.file(ra.machine.roms() / "gb" / "Mole (USA).gb");
    const auto check = [&](const std::string& command, const std::string& code, const std::string& named) {
        INFO(command);
        const auto translation = ra.translate(command, &game);
        CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{code});
        CHECK(rv::test::blockerText(translation).find(named) != std::string::npos);
    };
    // I11 step 13: %RUNINBACKGROUND% follows the game's window.
    const auto background = ra.translate("%RUNINBACKGROUND% %EMULATOR_RA% %ROM%", &game);
    CHECK(background.readiness() == LaunchReadiness::Ready);
    REQUIRE(background.request);
    CHECK(background.request->waitStrategy == rv::launcher::WaitStrategy::FollowWindow);
    check("%EMULATOR_RA% --no-gui %RPCS3_GAMEID%:%INJECT%=%BASENAME%.ps3", "token-unsupported", "%RPCS3_GAMEID%");
    check("%EMULATOR_RA% %PRECOMMAND_X% %ROM%", "token-unsupported", "%PRECOMMAND_X%");
    check("%EMULATOR_RA% %GAMEENTRYDIR%\\x %ROM% %GAMEENTRYDIR%", "token-unsupported", "%GAMEENTRYDIR%");
    check("%EMULATOR_RA% %EMUPATH%\\x %ROM%", "token-unsupported", "%EMUPATH%");
    check("%EMULATOR_RA% %APPDATA%\\cfg %ROM%", "token-unsupported", "%APPDATA%");
    check("%EMULATOR_RA% %STARTDIR% %ROM%", "token-malformed", "%STARTDIR%");
    check("%EMULATOR_RA% %ROM% %EMULATOR_RA%", "program-repeated", "%EMULATOR_RA%");
    check("%EMULATOR_RA% -a%STARTDIR%=x %ROM%", "start-folder-glued", "%STARTDIR%=");
    // The token stays in the argument a dry run shows, as ES-DE would pass it.
    const auto translation = ra.translate("%EMULATOR_RA% %APPDATA%\\cfg %ROM%", &game);
    REQUIRE(translation.request);
    CHECK(translation.request->arguments.front() == "%APPDATA%\\cfg");
    // An unknown install folder leaves %ESPATH% unexpanded.
    ra.machine.profile.installFolder.reset();
    ra.machine.emulator("ABS", {FindRule{FindRuleType::StaticPath, {text(ra.exe)}}});
    const auto noInstall = ra.translate("%EMULATOR_ABS% -cfg %ESPATH%\\x %ROM%", &game);
    CHECK(rv::test::blockerCodes(noInstall) == std::vector<std::string>{"install-unknown"});
}

TEST_CASE("esde/launch translation: %STARTDIR% and %HIDEWINDOW%", "[esde][launch]") {
    RaMachine ra;
    const fs::path rom = ra.machine.file(ra.machine.roms() / "gb" / "Sub" / "Mole (USA).gb");
    const rv::catalog::LaunchGame game = rv::test::testGame("Sub/Mole (USA).gb");

    auto translation = ra.translate("%HIDEWINDOW% %STARTDIR%=%EMUDIR% %EMULATOR_RA% -- %ROM%", &game);
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    REQUIRE(translation.request);
    CHECK(translation.request->hideConsoleWindow);
    CHECK(translation.request->workingDirectory == ra.exe.parent_path());
    CHECK(translation.request->arguments == std::vector<std::string>{"--", text(rom)});

    translation = ra.translate("%STARTDIR%=%GAMEDIR% %EMULATOR_RA% %ROM%", &game);
    REQUIRE(translation.request);
    CHECK(translation.request->workingDirectory == rom.parent_path());
    CHECK_FALSE(translation.request->hideConsoleWindow);

    // A quoted value with a space; '~' in it.
    fs::create_directories(ra.machine.home() / "Emu Saves");
    translation = ra.translate("%STARTDIR%=\"~\\Emu Saves\" %EMULATOR_RA% %ROM%", &game);
    INFO(rv::test::blockerText(translation));
    REQUIRE(translation.request);
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    CHECK(translation.request->workingDirectory == ra.machine.home() / "Emu Saves");

    // A folder ES-DE would create: Retroverse writes nothing.
    translation = ra.translate("%STARTDIR%=%EMUDIR%\\missing %EMULATOR_RA% %ROM%", &game);
    CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"start-folder-missing"});
    CHECK_FALSE(fs::exists(ra.exe.parent_path() / "missing"));
    // A folder that does not depend on the game is the system's too: its games share the blocker.
    translation = ra.translate("%STARTDIR%=%EMUDIR%\\missing %EMULATOR_RA% %ROM%", nullptr);
    CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"start-folder-missing"});
    translation = ra.translate("%STARTDIR%=\"~\\Emu Saves\" %EMULATOR_RA% %ROM%", nullptr);
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    // The game's folder is each game's.
    translation = ra.translate("%STARTDIR%=%GAMEDIR% %EMULATOR_RA% %ROM%", nullptr);
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    CHECK(translation.notes ==
          std::vector<std::string>{"the working folder is the game's folder (%STARTDIR%=%GAMEDIR%)"});

    // A flag in the middle of a command is removed, as ES-DE removes it.
    translation = ra.translate("%EMULATOR_RA% %HIDEWINDOW% %ROM%", &game);
    REQUIRE(translation.request);
    CHECK(translation.request->hideConsoleWindow);
    CHECK(translation.request->arguments == std::vector<std::string>{text(rom)});
}

TEST_CASE("esde/launch translation: %INJECT% files", "[esde][launch]") {
    RaMachine ra;
    const fs::path rom = ra.machine.file(ra.machine.roms() / "psx" / "Solo Disc (USA).chd");
    const rv::catalog::LaunchGame game = rv::test::testGame("Solo Disc (USA).chd");
    const std::string command = "%EMULATOR_RA% %INJECT%=%BASENAME%.commands -- %ROM%";
    const fs::path inject = rom.parent_path() / "Solo Disc (USA).commands";
    const auto translate = [&](const std::string& text) { return ra.translate(text, &game, "psx"); };

    SECTION("no file: the element is dropped, as ES-DE injects nothing") {
        const auto translation = translate(command);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(translation.request->arguments == std::vector<std::string>{"--", text(rom)});
        REQUIRE(translation.notes.size() == 1);
        CHECK(translation.notes[0].find("does not exist") != std::string::npos);
    }
    SECTION("an empty file, or line ends only: dropped") {
        ra.machine.file(inject, "");
        CHECK(translate(command).readiness() == LaunchReadiness::Ready);
        ra.machine.file(inject, "\r\n\r\n\n");
        const auto translation = translate(command);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK(translation.notes.at(0).find("is empty") != std::string::npos);
    }
    SECTION("more than 4,096 bytes: dropped") {
        ra.machine.file(inject, std::string(4097, 'a'));
        const auto translation = translate(command);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK(translation.notes.at(0).find("4096") != std::string::npos);
        // Line ends do not count: 4,096 bytes over many lines are injected.
        std::string lines;
        for (int i = 0; i < 64; ++i) {
            lines += std::string(64, 'b') + "\r\n";
        }
        ra.machine.file(inject, lines);
        CHECK(rv::test::blockerCodes(translate(command)) == std::vector<std::string>{"inject-file"});
    }
    SECTION("a file of more than a MiB: decided on its start, as ES-DE stops reading past 4,096 bytes") {
        ra.machine.file(inject, std::string(1536 * 1024, 'a'));
        const auto translation = translate(command);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK(translation.notes.at(0).find("4096") != std::string::npos);
        // Its content up to a Ctrl+Z, where ES-DE's text-mode reading ends.
        ra.machine.file(inject, "-x\x1A" + std::string(1536 * 1024, 'a'));
        CHECK(rv::test::blockerCodes(translate(command)) == std::vector<std::string>{"inject-file"});
        // Line ends only for its first MiB: undecided, taken as a file ES-DE may inject.
        ra.machine.file(inject, std::string(1536 * 1024, '\n'));
        CHECK(rv::test::blockerCodes(translate(command)) == std::vector<std::string>{"inject-file"});
    }
    SECTION("a file ES-DE would inject requires launcher configuration") {
        ra.machine.file(inject, "-cheats on\r\n");
        const auto translation = translate(command);
        CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"inject-file"});
        CHECK(rv::test::blockerText(translation).find("Solo Disc (USA).commands") != std::string::npos);
    }
    SECTION("a glued prefix stays, without the value") {
        const auto translation = translate("%EMULATOR_RA% -id=%INJECT%=%BASENAME%.3dsen");
        REQUIRE(translation.request);
        CHECK(translation.request->arguments == std::vector<std::string>{"-id="});
    }
    SECTION("a file with an absolute path, a value of exactly %ROM%") {
        const fs::path elsewhere = ra.machine.file(ra.machine.root() / "inject.txt", "-x");
        CHECK(rv::test::blockerCodes(translate("%EMULATOR_RA% %INJECT%=" + text(elsewhere) + " %ROM%")) ==
              std::vector<std::string>{"inject-file"});
        CHECK(rv::test::blockerCodes(translate("%EMULATOR_RA% %INJECT%=%ROM%")) ==
              std::vector<std::string>{"inject-file"});
    }
    SECTION("a system's readiness leaves the file to each game") {
        const auto translation = ra.translate(command, nullptr, "psx");
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK_FALSE(translation.request);
        REQUIRE(translation.notes.size() == 1);
        CHECK(translation.notes[0].find("read for each game") != std::string::npos);
    }
}

TEST_CASE("esde/launch translation: cores and a system's readiness", "[esde][launch]") {
    RaMachine ra;
    const std::string command = "%EMULATOR_RA% -L %CORE_RA%\\gambatte_libretro.dll %ROM%";
    auto translation = ra.translate(command, nullptr);
    CHECK(translation.readiness() == LaunchReadiness::CoreNotFound);
    CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"core-not-found"});
    CHECK(translation.attempts.back().find("gambatte_libretro.dll does not exist") != std::string::npos);
    ra.machine.listed(ra.cores / "gambatte_libretro.dll");
    translation = ra.translate(command, nullptr);
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    CHECK_FALSE(translation.request);
    REQUIRE(translation.core);
    CHECK(translation.core->path == ra.cores / "gambatte_libretro.dll");
    // Configuration comes before location: a token Retroverse does not run outranks a missing emulator.
    translation = ra.translate("%EMULATOR_MISSING% %RPCS3_GAMEID%", nullptr);
    ra.machine.emulator("MISSING", {FindRule{FindRuleType::SystemPath, {"missing.exe"}}});
    translation = ra.translate("%EMULATOR_MISSING% %RPCS3_GAMEID%", nullptr);
    CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"emulator-not-found", "token-unsupported"});
    CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
    // A program Windows cannot start (a batch file) requires launcher configuration, before a missing core.
    const fs::path batch = ra.machine.root() / "Emus" / "run.bat";
    ra.machine.file(batch, "@echo off");
    ra.machine.listed(batch);
    ra.machine.emulator("BATCH", {FindRule{FindRuleType::StaticPath, {text(batch)}}});
    ra.machine.core("BATCH", {"%EMUPATH%\\cores"});
    translation = ra.translate("%EMULATOR_BATCH% -L %CORE_BATCH%\\x.dll %ROM%", nullptr);
    CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"core-not-found", "invalid-request"});
    CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
}

TEST_CASE("esde/launch translation: what ES-DE refuses before it looks for a core", "[esde][launch]") {
    RaMachine ra;
    ra.machine.listed(ra.cores / "gambatte_libretro.dll");
    const rv::catalog::LaunchGame game = rv::test::testGame("Mole (USA).gb");
    ra.machine.file(ra.machine.roms() / "gb" / "Mole (USA).gb");

    SECTION("a core file that ends the command: ES-DE finds no end to it") {
        auto translation = ra.translate("%EMULATOR_RA% %ROM% -L %CORE_RA%\\gambatte_libretro.dll", &game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"core-malformed"});
        CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
        // Quoted, it ends at its closing quote; a flag after it leaves its space.
        translation = ra.translate("%EMULATOR_RA% %ROM% -L \"%CORE_RA%\\gambatte_libretro.dll\"", &game);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        translation = ra.translate("%EMULATOR_RA% %ROM% -L %CORE_RA%\\gambatte_libretro.dll %HIDEWINDOW%", &game);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(translation.request->arguments.back() == text(ra.cores / "gambatte_libretro.dll"));
    }
    SECTION("a core without corepath entries, an emulator without entries of the four types: no rules") {
        ra.machine.core("EMPTY", {});
        auto translation = ra.translate("%EMULATOR_RA% -L %CORE_EMPTY%\\x.dll %ROM%", &game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"find-rules-missing"});
        CHECK(rv::test::blockerText(translation).find("EMPTY") != std::string::npos);
        ra.machine.emulator("HOLLOW", {FindRule{FindRuleType::SystemPath, {}},
                                       FindRule{FindRuleType::CorePath, {"%EMUPATH%\\cores"}}});
        translation = ra.translate("%EMULATOR_HOLLOW% %ROM%", &game);
        CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"find-rules-missing"});
        CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
        // Known even when the program is not found: configuration comes before location.
        ra.machine.emulator("MISSING", {FindRule{FindRuleType::SystemPath, {"missing.exe"}}});
        translation = ra.translate("%EMULATOR_MISSING% -L %CORE_EMPTY%\\x.dll %ROM%", nullptr);
        CHECK(rv::test::blockerCodes(translation) ==
              std::vector<std::string>{"emulator-not-found", "find-rules-missing"});
        CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
    }
}

TEST_CASE("esde/launch translation: listedPath spells a path as the folder lists it", "[esde][launch]") {
    rv::test::LaunchMachine machine;
    machine.file(machine.roms() / "GB" / "1 GB US - H-Q" / "Mole Mania (USA, Europe).gb");
    fs::create_directories(machine.roms() / "GB" / "Game.ps3");
    auto listed = rv::esde::listedPath(machine.roms() / "gb", "1 gb us - h-q/mole mania (usa, europe).GB");
    REQUIRE(listed.has_value());
    CHECK(listed->relativePath == "1 GB US - H-Q/Mole Mania (USA, Europe).gb");
    CHECK_FALSE(listed->directory);
    listed = rv::esde::listedPath(machine.roms() / "gb", "game.PS3");
    REQUIRE(listed.has_value());
    CHECK(listed->relativePath == "Game.ps3");
    CHECK(listed->directory);
    CHECK(rv::esde::listedPath(machine.roms() / "gb", "gone.gb").error().code == rv::ErrorCode::NotFound);
    for (const std::string bad : {"*.gb", "a/../b.gb", "a//b.gb", "x?.gb", ""}) {
        INFO(bad);
        CHECK(rv::esde::listedPath(machine.roms() / "gb", bad).error().code == rv::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("esde/launch translation: a program chosen comes before the find rules and is never passed over",
          "[esde][launch]") {
    rv::test::LaunchMachine machine;
    const std::filesystem::path ruled = machine.program(machine.root() / "Rules" / "emu.exe");
    machine.emulator("EMU", {FindRule{FindRuleType::StaticPath, {rv::core::pathToUtf8(ruled)}}});
    const rv::esde::SystemDefinition system = rv::test::testSystem({{"Emu", "%EMULATOR_EMU% %ROM%"}});
    rv::esde::ProgramChoices programs;
    rv::esde::TranslationContext context = machine.context();
    context.programChoices = &programs;

    // Nothing chosen: the rules, and the translation names the emulator.
    rv::catalog::LaunchTranslation found = rv::esde::translateCommand(context, system, system.commands.front(), nullptr);
    REQUIRE(found.program.has_value());
    CHECK(found.program->path == ruled);
    CHECK(found.emulator == "EMU");

    const std::filesystem::path chosen = machine.program(machine.root() / "Chosen" / "mine.exe");
    programs["EMU"] = chosen;
    found = rv::esde::translateCommand(context, system, system.commands.front(), nullptr);
    REQUIRE(found.program.has_value());
    CHECK(found.program->path == chosen);
    CHECK(found.program->rule == "retroverse");
    CHECK(found.readiness() == LaunchReadiness::Ready);

    // Gone: not found, naming it; the rules' program must not start in its place.
    programs["EMU"] = machine.root() / "Chosen" / "gone.exe";
    found = rv::esde::translateCommand(context, system, system.commands.front(), nullptr);
    CHECK_FALSE(found.program.has_value());
    CHECK(found.readiness() == LaunchReadiness::EmulatorNotFound);
    REQUIRE(found.attempts.size() == 1);
    CHECK(found.attempts.front().find("gone.exe") != std::string::npos);

    // An emulator no find rule defines takes a program too: what repairs "find-rules-missing".
    const rv::esde::SystemDefinition unruled = rv::test::testSystem({{"None", "%EMULATOR_UNRULED% %ROM%"}});
    programs["UNRULED"] = chosen;
    found = rv::esde::translateCommand(context, unruled, unruled.commands.front(), nullptr);
    CHECK(found.readiness() == LaunchReadiness::Ready);
}

TEST_CASE("esde/launch translation: a program chosen is refused by the name found and by the file it opens",
          "[esde][launch]") {
    rv::test::LaunchMachine machine;
    const rv::esde::SystemDefinition system = rv::test::testSystem({{"Emu", "%EMULATOR_EMU% %ROM%"}});
    rv::esde::ProgramChoices programs;
    rv::esde::TranslationContext context = machine.context();
    context.programChoices = &programs;

    const std::filesystem::path shell = machine.program(machine.root() / "Bin" / "cmd.exe");
    programs["EMU"] = shell;
    const rv::catalog::LaunchTranslation named = rv::esde::translateCommand(context, system, system.commands.front(), nullptr);
    REQUIRE_FALSE(named.blockers.empty());
    CHECK(named.blockers.front().code == "program-refused");
    CHECK(rv::esde::checkProgramChoice(shell).error().code == rv::ErrorCode::ValidationFailed);

    // A symbolic link named like an emulator (a machine without the right to make one skips this part).
    const std::filesystem::path link = machine.root() / "Bin" / "harmless.exe";
    std::error_code denied;
    std::filesystem::create_symlink(shell, link, denied);
    if (!denied) {
        machine.probe.addFile(link);
        programs["EMU"] = link;
        const rv::catalog::LaunchTranslation opened =
            rv::esde::translateCommand(context, system, system.commands.front(), nullptr);
        REQUIRE_FALSE(opened.blockers.empty());
        CHECK(opened.blockers.front().code == "program-refused");
        CHECK(opened.blockers.front().message.find("it opens cmd.exe") != std::string::npos);
        CHECK(rv::esde::checkProgramChoice(link).error().code == rv::ErrorCode::ValidationFailed);
    }

    CHECK(rv::esde::checkProgramChoice(machine.program(machine.root() / "Bin" / "fine.exe")).has_value());
    CHECK(rv::esde::checkProgramChoice(machine.root() / "Bin" / "gone.exe").error().code == rv::ErrorCode::NotFound);
    CHECK(rv::esde::checkProgramChoice("relative.exe").error().code == rv::ErrorCode::InvalidArgument);
    // Not a program Windows starts.
    CHECK_FALSE(rv::esde::checkProgramChoice(machine.file(machine.root() / "Bin" / "text.exe", "not a program")).has_value());
}
