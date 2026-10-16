// ES-DE commands split into argv elements (I2 step 2, ADR-0008 §8): the splitter against shell32's CommandLineToArgvW
// on chosen edge cases, on many generated command lines (a fixed seed) and on every command of the fixtures; the
// template parser's program element and its tokens.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")

#include <rv/core/text.hpp>
#include <rv/esde/command_template.hpp>
#include <rv/esde/systems.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::esde::CommandToken;
using rv::esde::CommandTokenKind;
using rv::esde::parseCommandTemplate;
using rv::esde::splitCommandLine;

namespace {

std::wstring widen(std::string_view utf8) {
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                          nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(units), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), units);
    return wide;
}

std::string narrow(std::wstring_view wide) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                          nullptr);
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

/// shell32's split of a (non-empty) command line.
std::vector<std::string> shellSplit(std::string_view commandLine) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(widen(commandLine).c_str(), &count);
    std::vector<std::string> elements;
    REQUIRE(argv != nullptr);
    for (int i = 0; i < count; ++i) {
        elements.push_back(narrow(argv[i]));
    }
    LocalFree(argv);
    return elements;
}

/// Non-printable bytes as \xNN, for messages.
std::string shown(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            out += "\\x";
            out += "0123456789ABCDEF"[byte >> 4];
            out += "0123456789ABCDEF"[byte & 0xF];
        } else {
            out += c;
        }
    }
    return out;
}

void checkLikeShell(std::string_view commandLine) {
    INFO("command line: [" << shown(commandLine) << "]");
    const std::vector<std::string> expected = shellSplit(commandLine);
    const std::vector<std::string> actual = splitCommandLine(commandLine);
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("element " << i);
        CHECK(shown(actual[i]) == shown(expected[i]));
    }
}

/// The commands of a fixture's es_systems.xml files (read from the source tree: commands hold no fixture path).
std::vector<std::string> fixtureCommands(std::string_view fixture, std::string_view relativeFile) {
    const fs::path file =
        rv::test::fixturesDir() / rv::core::pathFromUtf8(fixture) / rv::core::pathFromUtf8(relativeFile);
    const auto systems = rv::esde::loadSystemsFiles(std::nullopt, file);
    INFO(rv::core::pathToUtf8(file) << ": " << (systems ? "read" : systems.error().toString()));
    REQUIRE(systems.has_value());
    std::vector<std::string> commands;
    for (const auto* list : {&systems->systems, &systems->fallbacks}) {
        for (const rv::esde::SystemDefinition& system : *list) {
            for (const rv::esde::SystemCommand& command : system.commands) {
                commands.push_back(command.text);
            }
        }
    }
    return commands;
}

std::vector<std::string> elementTexts(const rv::esde::CommandTemplate& parsed) {
    std::vector<std::string> texts;
    for (const rv::esde::CommandElement& element : parsed.elements) {
        texts.push_back(element.text);
    }
    return texts;
}

CommandToken token(CommandTokenKind kind, std::size_t offset, std::string text, std::string name,
                   std::string target = {}) {
    return CommandToken{kind, offset, std::move(text), std::move(name), std::move(target)};
}

} // namespace

TEST_CASE("esde/command_template: the splitter splits as CommandLineToArgvW on its edge cases",
          "[esde][command_template]") {
    const std::vector<std::string> cases{
        // The program-name rule.
        "a b c", "\"a b\" c", "\"a b\"c d", "\"C:\\dir\\\" x", "a\"b c\" d", "C:\\a\"b\"c d", "\"a b c", "\"\" a",
        "a\nb c", "a\rb c", "a\x0B" "b c", "a\x0C" "b c", "a\x01" "b c", "a\x1C" "b c", "a\x1F" "b c", "a \n b",
        "a\n\nb", "a\n b", "a\t\nb", "\"a\"\nb", "\"a\"\tb", "a\x01\x01" "b", "   ", "\t", "\n", "\na", "\" a",
        " a b", "a b  ", "a\xC2\xA0" "b c", "a\xE3\x80\x80" "b c",
        // The elements after the first.
        "x a b", "x a\tb", "x a\nb", "x a\rb", "x \"a b\"", "x a\"b c\" d", "x \"\"", "x a \"\" b", "x \"",
        "x a\\\"b", "x a\\\\\"b c\" d", "x a\\\\\\\"b", "x a\\\\b", "x a\\", "x \\\\", "x \"a\\\\\"",
        "x \"\"\"b\"\"\"", "x \"a\"\"b c\" d", "x \"\"\"\" y", "x \"\"\"\"\" y", "x \"\"\"\"\"\" y",
        "x \"\"\"\"\"\"\" y", "x a\"\"b", "x \\\"\" y", "x \"a\\\"b\"", "x %ROM% \"%ROM%\" \\%ROM%\\",
        // ES-DE's commands.
        "%EMULATOR_RETROARCH% -L %CORE_RETROARCH%\\mame_libretro.dll \"apple2e -rompath "
        "\\\"%GAMEDIRRAW%;%ROMPATH%\\apple2\\\" -gameio joy -flop1 \\\"%GAMEDIRRAW%\\%FILENAME%\\\"\"",
        "%HIDEWINDOW% %STARTDIR%=%EMUDIR% %EMULATOR_MAME% -rompath %GAMEDIR%\\;%ROMPATH%\\adam adam -flop1 %ROM%",
        "\"K:\\Giochi\\Emulatori\\WII\\Dolphin.exe\" -b -e %ROM%",
        "K:\\Giochi\\Emulatori\\PS2\\pcsx2-qt.exe -batch %ROM%",
        "%EMULATOR_RPCS3% --no-gui %RPCS3_GAMEID%:%INJECT%=%BASENAME%.ps3", "%EMULATOR_X% -image=%ROM% -source:%ROM%",
    };
    for (const std::string& commandLine : cases) {
        checkLikeShell(commandLine);
    }
    // Where CommandLineToArgvW returns the calling program's path, the splitter returns nothing; a NUL ends the text.
    CHECK(splitCommandLine("").empty());
    CHECK(splitCommandLine(std::string_view("a b\0c d", 7)) == std::vector<std::string>{"a", "b"});
    CHECK(splitCommandLine("   ") == std::vector<std::string>{""});
}

TEST_CASE("esde/command_template: the splitter splits 20,000 generated command lines as CommandLineToArgvW",
          "[esde][command_template]") {
    // Characters the rules treat specially, several times over, and ordinary ones (non-ASCII among them).
    const std::vector<std::string> alphabet{"a",  "b",  "Z",  " ",    " ",    "\t", "\"", "\"", "\"",  "\\",
                                            "\\", "\\", "%",  "~",    "=",    ",",  ";",  "\n", "\r",  "\x01",
                                            "\x1F", "-", ":", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80"};
    std::mt19937 random(20261016u);
    for (int round = 0; round < 20000; ++round) {
        const std::size_t length = 1 + random() % 24;
        std::string commandLine;
        for (std::size_t i = 0; i < length; ++i) {
            commandLine += alphabet[random() % alphabet.size()];
        }
        checkLikeShell(commandLine);
    }
}

TEST_CASE("esde/command_template: every command of the ES-DE fixtures splits as CommandLineToArgvW, and parses into "
          "the same elements",
          "[esde][command_template]") {
    std::vector<std::string> commands =
        fixtureCommands("esde/standard", "Programs/ES-DE/resources/systems/windows/es_systems.xml");
    REQUIRE(commands.size() == 10);
    for (const auto& [fixture, file] : {
             std::pair{"esde/portable", "ES-DE/resources/systems/windows/es_systems.xml"},
             std::pair{"esde/custom-dirs", "Programs/ES-DE/resources/systems/windows/es_systems.xml"},
             std::pair{"esde/custom-systems", "Programs/ES-DE/resources/systems/windows/es_systems.xml"},
             std::pair{"esde/custom-systems", "ES-DE/custom_systems/es_systems.xml"},
         }) {
        const std::vector<std::string> more = fixtureCommands(fixture, file);
        commands.insert(commands.end(), more.begin(), more.end());
    }
    for (const std::string& command : commands) {
        checkLikeShell(command);
        // These commands start with their program or with flags that hold no quote: the parser gives the same elements.
        CHECK(elementTexts(parseCommandTemplate(command)) == shellSplit(command));
    }
}

TEST_CASE("esde/command_template: the tokens of ES-DE's commands", "[esde][command_template]") {
    SECTION("the apple2 MAME command: escaped quotes keep one argument, its tokens inside") {
        const auto parsed = parseCommandTemplate(
            R"(%EMULATOR_RETROARCH% -L %CORE_RETROARCH%\mame_libretro.dll )"
            R"("apple2e -rompath \"%GAMEDIRRAW%;%ROMPATH%\apple2\" -gameio joy -flop1 \"%GAMEDIRRAW%\%FILENAME%\"")");
        REQUIRE(parsed.elements.size() == 4);
        CHECK(parsed.programIndex == 0);
        CHECK(parsed.elements[0].tokens ==
              std::vector{token(CommandTokenKind::Emulator, 0, "%EMULATOR_RETROARCH%", "EMULATOR_RETROARCH",
                                "RETROARCH")});
        CHECK(parsed.elements[1].text == "-L");
        CHECK(parsed.elements[1].tokens.empty());
        CHECK(parsed.elements[2].tokens ==
              std::vector{token(CommandTokenKind::Core, 0, "%CORE_RETROARCH%", "CORE_RETROARCH", "RETROARCH")});
        const auto& mame = parsed.elements[3];
        CHECK(mame.text ==
              R"(apple2e -rompath "%GAMEDIRRAW%;%ROMPATH%\apple2" -gameio joy -flop1 "%GAMEDIRRAW%\%FILENAME%")");
        const std::string& text = mame.text;
        CHECK(mame.tokens ==
              std::vector{token(CommandTokenKind::Variable, text.find("%GAMEDIRRAW%"), "%GAMEDIRRAW%", "GAMEDIRRAW"),
                          token(CommandTokenKind::Variable, text.find("%ROMPATH%"), "%ROMPATH%", "ROMPATH"),
                          token(CommandTokenKind::Variable, text.rfind("%GAMEDIRRAW%"), "%GAMEDIRRAW%", "GAMEDIRRAW"),
                          token(CommandTokenKind::Variable, text.find("%FILENAME%"), "%FILENAME%", "FILENAME")});
        CHECK(mame.tokens[0].offset == 18);
    }
    SECTION("flags and the start folder lead the command: the program comes after them") {
        const auto parsed = parseCommandTemplate(
            R"(%HIDEWINDOW% %STARTDIR%=%EMUDIR% %EMULATOR_MAME% -rompath %GAMEDIR%\;%ROMPATH%\adam adam -flop1 %ROM%)");
        CHECK(elementTexts(parsed) == std::vector<std::string>{"%HIDEWINDOW%", "%STARTDIR%=%EMUDIR%", "%EMULATOR_MAME%",
                                                              "-rompath", R"(%GAMEDIR%\;%ROMPATH%\adam)", "adam",
                                                              "-flop1", "%ROM%"});
        CHECK(parsed.programIndex == 2);
        CHECK(parsed.elements[0].tokens ==
              std::vector{token(CommandTokenKind::Flag, 0, "%HIDEWINDOW%", "HIDEWINDOW")});
        CHECK(parsed.elements[1].tokens ==
              std::vector{token(CommandTokenKind::Assignment, 0, "%STARTDIR%=", "STARTDIR"),
                          token(CommandTokenKind::Variable, 11, "%EMUDIR%", "EMUDIR")});
        CHECK(parsed.elements[4].tokens == std::vector{token(CommandTokenKind::Variable, 0, "%GAMEDIR%", "GAMEDIR"),
                                                       token(CommandTokenKind::Variable, 11, "%ROMPATH%", "ROMPATH")});
    }
    SECTION("a quoted start folder before a quoted program: one element each, as ES-DE removes the folder first") {
        const std::string command = R"(%STARTDIR%="C:\Games dir" "C:\Emu dir\emu.exe" %ROM%)";
        const auto parsed = parseCommandTemplate(command);
        CHECK(elementTexts(parsed) ==
              std::vector<std::string>{R"(%STARTDIR%=C:\Games dir)", R"(C:\Emu dir\emu.exe)", "%ROM%"});
        CHECK(parsed.programIndex == 1);
        // The program-name rule would have cut the quoted folder at its space.
        CHECK(splitCommandLine(command) != elementTexts(parsed));
        // Blanks ES-DE trims, and every flag ES-DE removes.
        const auto flags = parseCommandTemplate("  %RUNINBACKGROUND%\t%ESCAPESPECIALS%  \"C:\\Emu dir\\emu.exe\"\\ x");
        CHECK(elementTexts(flags) ==
              std::vector<std::string>{"%RUNINBACKGROUND%", "%ESCAPESPECIALS%", R"(C:\Emu dir\emu.exe)", "\\", "x"});
        CHECK(flags.programIndex == 2);
        CHECK(parseCommandTemplate("  %EMULATOR_X% %ROM%").programIndex == 0);
        CHECK(elementTexts(parseCommandTemplate("  %EMULATOR_X% %ROM%")) ==
              std::vector<std::string>{"%EMULATOR_X%", "%ROM%"});
    }
    SECTION("assignments, find-rule names, unknown tokens, '~', malformed tokens") {
        CHECK(rv::esde::scanCommandTokens("%RPCS3_GAMEID%:%INJECT%=%BASENAME%.ps3") ==
              std::vector{token(CommandTokenKind::Unknown, 0, "%RPCS3_GAMEID%", "RPCS3_GAMEID"),
                          token(CommandTokenKind::Assignment, 15, "%INJECT%=", "INJECT"),
                          token(CommandTokenKind::Variable, 24, "%BASENAME%", "BASENAME")});
        CHECK(rv::esde::scanCommandTokens("%EMULATOR_PLAY!%") ==
              std::vector{token(CommandTokenKind::Emulator, 0, "%EMULATOR_PLAY!%", "EMULATOR_PLAY!", "PLAY!")});
        CHECK(rv::esde::scanCommandTokens("%EMULATOR_OS-SHELL%") ==
              std::vector{
                  token(CommandTokenKind::Emulator, 0, "%EMULATOR_OS-SHELL%", "EMULATOR_OS-SHELL", "OS-SHELL")});
        CHECK(rv::esde::scanCommandTokens("%PRECOMMAND_WRAP%") ==
              std::vector{token(CommandTokenKind::PreCommand, 0, "%PRECOMMAND_WRAP%", "PRECOMMAND_WRAP", "WRAP")});
        // A quoted name with a space is the find rule's name up to the next '%', as ES-DE reads it.
        CHECK(rv::esde::scanCommandTokens("%EMULATOR_MY EMU%") ==
              std::vector{token(CommandTokenKind::Emulator, 0, "%EMULATOR_MY EMU%", "EMULATOR_MY EMU", "MY EMU")});
        CHECK(rv::esde::scanCommandTokens("~\\Emulators\\x.exe") ==
              std::vector{token(CommandTokenKind::HomeFolder, 0, "~", "~")});
        CHECK(rv::esde::scanCommandTokens(R"(-cfg=%APPDATA%\x.cfg)") ==
              std::vector{token(CommandTokenKind::Unknown, 5, "%APPDATA%", "APPDATA")});
        // A stray '%' never hides a token, nor does an unknown name before it; ES-DE's tokens are upper-case.
        CHECK(rv::esde::scanCommandTokens("50%-%ROM%") ==
              std::vector{token(CommandTokenKind::Variable, 4, "%ROM%", "ROM")});
        CHECK(rv::esde::scanCommandTokens("x%y%ROM%") ==
              std::vector{token(CommandTokenKind::Variable, 3, "%ROM%", "ROM")});
        CHECK(rv::esde::scanCommandTokens("%y%%ROM%") ==
              std::vector{token(CommandTokenKind::Unknown, 0, "%y%", "y"),
                          token(CommandTokenKind::Variable, 3, "%ROM%", "ROM")});
        CHECK(rv::esde::scanCommandTokens("-x%A%EMULATOR_MAME%") ==
              std::vector{token(CommandTokenKind::Emulator, 4, "%EMULATOR_MAME%", "EMULATOR_MAME", "MAME")});
        CHECK(rv::esde::scanCommandTokens("%rom%") == std::vector{token(CommandTokenKind::Unknown, 0, "%rom%", "rom")});
        CHECK(rv::esde::scanCommandTokens("100%").empty());
        CHECK(rv::esde::scanCommandTokens("%%").empty());
        CHECK(rv::esde::scanCommandTokens("%1%").empty());
        CHECK(rv::esde::scanCommandTokens("%%ROM%") ==
              std::vector{token(CommandTokenKind::Variable, 1, "%ROM%", "ROM")});
        CHECK(rv::esde::scanCommandTokens("%ROM%%BASENAME%") ==
              std::vector{token(CommandTokenKind::Variable, 0, "%ROM%", "ROM"),
                          token(CommandTokenKind::Variable, 5, "%BASENAME%", "BASENAME")});
        CHECK(rv::esde::scanCommandTokens("%STARTDIR%") ==
              std::vector{token(CommandTokenKind::Malformed, 0, "%STARTDIR%", "STARTDIR")});
        CHECK(rv::esde::scanCommandTokens("%INJECT%x") ==
              std::vector{token(CommandTokenKind::Malformed, 0, "%INJECT%", "INJECT")});
        CHECK(rv::esde::scanCommandTokens("%EMULATOR_%") ==
              std::vector{token(CommandTokenKind::Unknown, 0, "%EMULATOR_%", "EMULATOR_")});
        CHECK(rv::esde::scanCommandTokens("%ANDROIDAPP%=%FILEINJECT%") ==
              std::vector{token(CommandTokenKind::Unknown, 0, "%ANDROIDAPP%=", "ANDROIDAPP"),
                          token(CommandTokenKind::Unknown, 13, "%FILEINJECT%", "FILEINJECT")});
        CHECK(rv::esde::scanCommandTokens("%ProgramFiles(x86)%\\Emu") ==
              std::vector{token(CommandTokenKind::Unknown, 0, "%ProgramFiles(x86)%", "ProgramFiles(x86)")});
        // Every token ES-DE handles on Windows is known.
        for (const std::string_view name : {"ROM", "ROMRAW", "ROMPATH", "BASENAME", "FILENAME", "GAMEDIR", "GAMEDIRRAW",
                                            "GAMEENTRYDIR", "EMUDIR", "ESPATH", "EMUPATH"}) {
            const auto tokens = rv::esde::scanCommandTokens("%" + std::string(name) + "%");
            REQUIRE(tokens.size() == 1);
            CHECK(tokens[0].kind == CommandTokenKind::Variable);
        }
        for (const std::string_view name : {"HIDEWINDOW", "ESCAPESPECIALS", "RUNINBACKGROUND"}) {
            const auto tokens = rv::esde::scanCommandTokens("%" + std::string(name) + "%");
            REQUIRE(tokens.size() == 1);
            CHECK(tokens[0].kind == CommandTokenKind::Flag);
        }
    }
    SECTION("placeholders, empty commands, census keys") {
        const auto placeholder = parseCommandTemplate("PLACEHOLDER %ROM%");
        CHECK(elementTexts(placeholder) == std::vector<std::string>{"PLACEHOLDER", "%ROM%"});
        CHECK(placeholder.programIndex == 0);
        CHECK(placeholder.elements[0].tokens.empty());
        const auto empty = parseCommandTemplate("");
        CHECK(empty.elements.empty());
        CHECK(empty.programIndex == 0);
        CHECK(parseCommandTemplate("%HIDEWINDOW%").programIndex == 1);
        CHECK(rv::esde::censusKey(token(CommandTokenKind::Emulator, 0, "%EMULATOR_MAME%", "EMULATOR_MAME", "MAME")) ==
              "%EMULATOR_X%");
        CHECK(rv::esde::censusKey(
                  token(CommandTokenKind::Core, 0, "%CORE_RETROARCH%", "CORE_RETROARCH", "RETROARCH")) == "%CORE_X%");
        CHECK(rv::esde::censusKey(token(CommandTokenKind::Assignment, 0, "%INJECT%=", "INJECT")) == "%INJECT%=");
        CHECK(rv::esde::toString(CommandTokenKind::PreCommand) == "pre-command");
    }
}
