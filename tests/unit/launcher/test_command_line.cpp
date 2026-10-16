#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <rv/launcher/command_line.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")
#endif

using rv::launcher::appendQuotedArgument;
using rv::launcher::buildCommandLine;
using rv::launcher::isValidUtf8;
using rv::launcher::kMaxCommandLineUnits;
using rv::launcher::quoteWindowsArgument;
using rv::launcher::utf16Length;

namespace {

#if defined(_WIN32)
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

/// What a started program sees: the Windows shell's own parser (SDL's main and most C runtimes agree with it).
std::vector<std::string> commandLineToArgv(const std::string& commandLine) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(widen(commandLine).c_str(), &count);
    std::vector<std::string> arguments;
    if (argv == nullptr) {
        return arguments;
    }
    for (int i = 0; i < count; ++i) {
        arguments.push_back(narrow(argv[i]));
    }
    LocalFree(argv);
    return arguments;
}
#endif

} // namespace

TEST_CASE("launcher/command_line: arguments are quoted only when they must be", "[launcher][command_line]") {
    struct Case {
        std::string_view argument;
        std::string_view quoted;
    };
    const Case cases[] = {
        {"", R"("")"},
        {"plain", "plain"},
        {"-L", "-L"},
        {R"(C:\path\file.dll)", R"(C:\path\file.dll)"},     // backslashes alone are literal
        {R"(trailing\)", R"(trailing\)"},                   // no quotes: nothing to protect
        {"two words", R"("two words")"},
        {"tab\there", "\"tab\there\""},
        {"line\nbreak", "\"line\nbreak\""},
        {R"(say "hi")", R"("say \"hi\"")"},
        {R"(")", R"("\"")"},
        {R"(dir with space\)", R"("dir with space\\")"},    // doubled before the closing quote
        {R"(a\"b)", R"("a\\\"b")"},                          // doubled before an escaped quote
        {R"(a\\"b)", R"("a\\\\\"b")"},
        {R"(\\server\share\rom.zip)", R"(\\server\share\rom.zip)"},
        {"it's", "it's"},
        {"%PATH%", "%PATH%"},                                // never meaningful to CreateProcessW
        {"Pok\xC3\xA9mon", "Pok\xC3\xA9mon"},
    };
    for (const Case& c : cases) {
        INFO("argument [" << c.argument << "]");
        CHECK(quoteWindowsArgument(c.argument) == c.quoted);
    }
    std::string line = "prefix";
    appendQuotedArgument(line, "a b");
    CHECK(line == "prefix\"a b\"");
}

TEST_CASE("launcher/command_line: the command line of a program and its arguments", "[launcher][command_line]") {
    const std::vector<std::string> arguments{
        "-L", R"(E:\Programmi\Retroarch\cores\gambatte_libretro.dll)",
        R"(I:\Giochi\Emulatori\gb\1 GB US - H-Q\Mole Mania (USA, Europe) (SGB Enhanced).gb)"};
    const auto line = buildCommandLine(R"(E:\Programmi\Retroarch\retroarch.exe)", arguments);
    REQUIRE(line.has_value());
    CHECK(*line == R"("E:\Programmi\Retroarch\retroarch.exe" -L E:\Programmi\Retroarch\cores\gambatte_libretro.dll )"
                   R"("I:\Giochi\Emulatori\gb\1 GB US - H-Q\Mole Mania (USA, Europe) (SGB Enhanced).gb")");

    const auto alone = buildCommandLine(R"(C:\Program Files\Emu\emu.exe)", {});
    REQUIRE(alone.has_value());
    CHECK(*alone == R"("C:\Program Files\Emu\emu.exe")");
}

TEST_CASE("launcher/command_line: refused command lines", "[launcher][command_line]") {
    CHECK_FALSE(buildCommandLine("", {}).has_value());
    CHECK_FALSE(buildCommandLine(R"(C:\a"b.exe)", {}).has_value());
    CHECK_FALSE(buildCommandLine(std::string("C:\\a\0b.exe", 10), {}).has_value());
    const std::vector<std::string> withNul{std::string("a\0b", 3)};
    const auto nul = buildCommandLine(R"(C:\emu.exe)", withNul);
    REQUIRE_FALSE(nul.has_value());
    CHECK(nul.error().context == "argv[1]");
    const std::vector<std::string> malformed{"ok", "\xC3\x28"};
    const auto bad = buildCommandLine(R"(C:\emu.exe)", malformed);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().context == "argv[2]");

    // CreateProcessW's limit counts UTF-16 code units: 2 per emoji, 1 per accented letter.
    const std::string program = R"(C:\emu.exe)"; // 12 units with its quotes
    const std::size_t room = kMaxCommandLineUnits - 12 - 1; // minus the separating space
    CHECK(buildCommandLine(program, std::vector<std::string>{std::string(room, 'a')}).has_value());
    CHECK_FALSE(buildCommandLine(program, std::vector<std::string>{std::string(room + 1, 'a')}).has_value());
    std::string emoji;
    for (std::size_t i = 0; i < room / 2; ++i) {
        emoji += "\xF0\x9F\x8E\xAE";
    }
    CHECK(buildCommandLine(program, std::vector<std::string>{emoji}).has_value());
    CHECK_FALSE(buildCommandLine(program, std::vector<std::string>{emoji + "\xC3\xA9\xC3\xA9"}).has_value());
}

TEST_CASE("launcher/command_line: UTF-8 validation and UTF-16 length", "[launcher][command_line]") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("ascii"));
    CHECK(isValidUtf8("\xC3\xA9\xE6\x97\xA5\xF0\x9F\x8E\xAE"));    // é 日 🎮
    CHECK(isValidUtf8(std::string("nul\0inside", 10)));             // the command line refuses NUL separately
    CHECK_FALSE(isValidUtf8("\x80"));                               // stray continuation byte
    CHECK_FALSE(isValidUtf8("\xC3"));                               // truncated
    CHECK_FALSE(isValidUtf8("\xC3\x28"));                           // bad continuation
    CHECK_FALSE(isValidUtf8("\xC0\xAF"));                           // overlong '/'
    CHECK_FALSE(isValidUtf8("\xE0\x80\xAF"));                       // overlong
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));                       // UTF-16 surrogate
    CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80"));                   // above U+10FFFF
    CHECK_FALSE(isValidUtf8("\xF8\x88\x80\x80\x80"));               // 5-byte form

    CHECK(utf16Length("") == 0);
    CHECK(utf16Length("abc") == 3);
    CHECK(utf16Length("\xC3\xA9") == 1);
    CHECK(utf16Length("\xE6\x97\xA5\xE6\x9C\xAC") == 2);
    CHECK(utf16Length("\xF0\x9F\x8E\xAE") == 2);
    CHECK(utf16Length("a\xF0\x9F\x8E\xAE\xC3\xA9") == 4);
}

#if defined(_WIN32)
TEST_CASE("launcher/command_line: CommandLineToArgvW gives back every argument", "[launcher][command_line]") {
    const std::string program = R"(C:\Program Files (x86)\Emulators\Retro Arch\retroarch.exe)";
    const std::vector<std::string> arguments{
        "",
        " ",
        "plain",
        "two words",
        "tab\there",
        "quote\"inside",
        "\"",
        "\"\"",
        "trailing backslash\\",
        "C:\\Program Files\\Emulator\\",
        "\\\\server\\share\\rom.zip",
        "back\\\\slashes\\\\\"and a quote",
        "ends with a backslash and a quote\\\"",
        "it's an apostrophe",
        "%PATH% ^& | < > !",
        "--",
        "I:\\Giochi\\Emulatori\\gb\\1 GB US - H-Q\\Mole Mania (USA, Europe) (SGB Enhanced).gb",
        "Pok\xC3\xA9mon \xE2\x80\x93 \xC3\x89" "dition Bleue",
        "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3",
        "\xF0\x9F\x8E\xAE",
    };
    const auto line = buildCommandLine(program, arguments);
    REQUIRE(line.has_value());
    const std::vector<std::string> parsed = commandLineToArgv(*line);
    REQUIRE(parsed.size() == arguments.size() + 1);
    CHECK(parsed[0] == program);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        INFO("argument " << i << ": [" << arguments[i] << "] in " << *line);
        CHECK(parsed[i + 1] == arguments[i]);
    }
}

TEST_CASE("launcher/command_line: random arguments survive CommandLineToArgvW", "[launcher][command_line]") {
    // Deterministic: a fixed seed and std::mt19937, whose sequence the standard defines.
    std::mt19937 random(0x5EED1A0Cu);
    const std::vector<std::string_view> pieces{"a", "Z", " ", "  ", "\t", "\"", "\\", "\\\\", "'", "%", "^", "&",
                                               "\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x8E\xAE", "-", "=", "("};
    const std::vector<std::string_view> programs{R"(C:\emu.exe)", R"(C:\Program Files\Emu Lator\emu.exe)",
                                                 R"(\\server\share\emu.exe)", "E:\\\xC3\xA9mu\\emu.exe"};
    std::uniform_int_distribution<std::size_t> pick(0, pieces.size() - 1);
    std::uniform_int_distribution<int> count(0, 6);
    std::uniform_int_distribution<int> length(0, 10);
    for (int iteration = 0; iteration < 3000; ++iteration) {
        const std::string program(programs[static_cast<std::size_t>(iteration) % programs.size()]);
        std::vector<std::string> arguments(static_cast<std::size_t>(count(random)));
        for (std::string& argument : arguments) {
            const int pieceCount = length(random);
            for (int p = 0; p < pieceCount; ++p) {
                argument += pieces[pick(random)];
            }
        }
        const auto line = buildCommandLine(program, arguments);
        REQUIRE(line.has_value());
        std::vector<std::string> expected{program};
        expected.insert(expected.end(), arguments.begin(), arguments.end());
        INFO("iteration " << iteration << ": " << *line);
        REQUIRE(commandLineToArgv(*line) == expected);
    }
}

TEST_CASE("launcher/command_line: the launch ES-DE logged for Mole Mania parses to the same arguments",
          "[launcher][command_line]") {
    // ES-DE's log on the reference machine (spec §11.3: equivalence with ES-DE's command, not its text).
    const std::string esde =
        R"(E:\Programmi\Retroarch\retroarch.exe -L E:\Programmi\Retroarch\cores\gambatte_libretro.dll )"
        R"("I:\Giochi\Emulatori\gb\1 GB US - H-Q\Mole Mania (USA, Europe) (SGB Enhanced).gb")";
    const std::vector<std::string> arguments{
        "-L", R"(E:\Programmi\Retroarch\cores\gambatte_libretro.dll)",
        R"(I:\Giochi\Emulatori\gb\1 GB US - H-Q\Mole Mania (USA, Europe) (SGB Enhanced).gb)"};
    const auto ours = buildCommandLine(R"(E:\Programmi\Retroarch\retroarch.exe)", arguments);
    REQUIRE(ours.has_value());
    CHECK(commandLineToArgv(*ours) == commandLineToArgv(esde));
}
#endif
