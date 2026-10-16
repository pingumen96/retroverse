#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/arg_template.hpp>
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

using rv::ErrorCode;
using rv::launcher::checkTemplateElement;
using rv::launcher::expandTemplate;
using rv::launcher::expandTemplateElement;
using rv::launcher::isAllowedEnvironmentVariable;
using rv::launcher::isValidVariableName;
using rv::launcher::parseTemplateElement;
using rv::launcher::TemplateContext;
using rv::launcher::TemplatePart;
using rv::launcher::TemplateToken;
using rv::launcher::TemplateVariables;

namespace {

/// A context for a ROM, a system and an emulator folder, the environment from a map.
struct Values {
    TemplateVariables launch;
    TemplateVariables system;
    TemplateVariables profile;
    std::map<std::string, std::string> environment{{"APPDATA", R"(C:\Users\Player\AppData\Roaming)"},
                                                   {"PROGRAMFILES", R"(C:\Program Files)"}};

    TemplateContext context(std::filesystem::path rom = rv::core::pathFromUtf8(R"(C:\Games\gb\Mole Mania (USA).gb)")) {
        TemplateContext context;
        context.rom = std::move(rom);
        context.system = "gb";
        context.emulatorDirectory = std::filesystem::path(LR"(C:\Emulators\RetroArch)");
        context.variables = {&launch, &system, &profile};
        context.environment = [this](std::string_view name) -> std::optional<std::string> {
            const auto found = environment.find(std::string(name));
            return found == environment.end() ? std::nullopt : std::optional<std::string>(found->second);
        };
        return context;
    }
};

std::string expanded(std::string_view element, const TemplateContext& context) {
    const auto result = expandTemplateElement(element, context);
    INFO(element << ": " << (result ? std::string() : result.error().toString()));
    REQUIRE(result.has_value());
    return *result;
}

rv::Error failure(std::string_view element, const TemplateContext& context) {
    const auto result = expandTemplateElement(element, context);
    INFO(element << " expanded to " << (result ? *result : std::string()));
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

#if defined(_WIN32)
/// What a started program sees: the Windows shell's own parser.
std::vector<std::string> commandLineToArgv(const std::string& commandLine) {
    const auto wide = rv::core::utf8ToWide(commandLine);
    REQUIRE(wide.has_value());
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(wide->c_str(), &count);
    std::vector<std::string> arguments;
    REQUIRE(argv != nullptr);
    for (int i = 0; i < count; ++i) {
        arguments.push_back(rv::core::wideToUtf8Lossy(argv[i]));
    }
    LocalFree(argv);
    return arguments;
}
#endif

} // namespace

TEST_CASE("launcher/arg_template: elements are split into text and tokens", "[launcher][arg_template]") {
    const auto parts = parseTemplateElement("-image={rom};{var:core}x{{literal}}{env:APPDATA}");
    REQUIRE(parts.has_value());
    const std::vector<TemplatePart> expected{
        {std::nullopt, "-image="},
        {TemplateToken::Rom, ""},
        {std::nullopt, ";"},
        {TemplateToken::Variable, "core"},
        {std::nullopt, "x{literal}"},
        {TemplateToken::Environment, "APPDATA"},
    };
    CHECK(*parts == expected);

    const auto plain = parseTemplateElement("");
    REQUIRE(plain.has_value());
    CHECK(plain->empty());
    const auto braces = parseTemplateElement("{{rom}}");
    REQUIRE(braces.has_value());
    REQUIRE(braces->size() == 1);
    CHECK((*braces)[0] == TemplatePart{std::nullopt, "{rom}"}); // doubled braces are text, never a token
    for (const auto& [text, token] : std::vector<std::pair<std::string_view, TemplateToken>>{
             {"{rom}", TemplateToken::Rom},
             {"{rom_dir}", TemplateToken::RomDir},
             {"{rom_name}", TemplateToken::RomName},
             {"{rom_file}", TemplateToken::RomFile},
             {"{rom_ext}", TemplateToken::RomExt},
             {"{emulator_dir}", TemplateToken::EmulatorDir},
             {"{system}", TemplateToken::System},
         }) {
        const auto one = parseTemplateElement(text);
        REQUIRE(one.has_value());
        REQUIRE(one->size() == 1);
        CHECK((*one)[0].token == token);
        CHECK("{" + std::string(rv::launcher::toString(token)) + "}" == text);
    }
}

TEST_CASE("launcher/arg_template: malformed elements are refused, the token named", "[launcher][arg_template]") {
    struct Case {
        std::string_view element;
        std::string_view named;
    };
    const Case cases[] = {
        {"{rom", "'{' without its '}'"},
        {"-x={rom", "'{' without its '}'"},
        {"{ro{m}", "'{' without its '}'"},
        {"rom}", "'}' without its '{'"},
        {"{}", "empty token"},
        {"{ROM}", "unknown token {ROM}"},          // tokens are lower case
        {"{romname}", "unknown token {romname}"},
        {"{rom_path}", "unknown token {rom_path}"},
        {"{var:}", "{var:} needs a variable name"},
        {"{var:a b}", "{var:a b} needs a variable name"},
        {"{var:\xC3\xA9}", "needs a variable name"},
        {"{env:}", "{env:} needs an environment variable name"},
        {"{env:A=B}", "{env:A=B} needs an environment variable name"},
        {"{%ROM%}", "unknown token {%ROM%}"},
    };
    for (const Case& c : cases) {
        INFO(c.element);
        const auto parts = parseTemplateElement(c.element);
        REQUIRE_FALSE(parts.has_value());
        CHECK(parts.error().code == ErrorCode::ParseError);
        CHECK(parts.error().message.find(c.named) != std::string::npos);
        CHECK(parts.error().context == c.element);
    }
}

TEST_CASE("launcher/arg_template: the ROM, system and emulator tokens", "[launcher][arg_template]") {
    Values values;
    const TemplateContext context = values.context();
    CHECK(expanded("{rom}", context) == R"(C:\Games\gb\Mole Mania (USA).gb)");
    CHECK(expanded("{rom_dir}", context) == R"(C:\Games\gb)");
    CHECK(expanded("{rom_name}", context) == "Mole Mania (USA)");
    CHECK(expanded("{rom_file}", context) == "Mole Mania (USA).gb");
    CHECK(expanded("{rom_ext}", context) == ".gb");
    CHECK(expanded("{rom_name}{rom_ext}", context) == expanded("{rom_file}", context));
    CHECK(expanded("{system}", context) == "gb");
    CHECK(expanded("{emulator_dir}", context) == R"(C:\Emulators\RetroArch)");
    CHECK(expanded("-image={rom}", context) == R"(-image=C:\Games\gb\Mole Mania (USA).gb)");
    CHECK(expanded("{rom_dir}\\{rom_name}.sav", context) == R"(C:\Games\gb\Mole Mania (USA).sav)");
    CHECK(expanded("{{{rom_ext}}}", context) == "{.gb}");
    CHECK(expanded("", context).empty());

    // '/' becomes '\'; a folder named with a trailing separator is the folder (the games that are folders).
    CHECK(expanded("{rom}", values.context("C:/Games/gb/sub dir/a b.gb")) == R"(C:\Games\gb\sub dir\a b.gb)");
    const TemplateContext folder = values.context(LR"(D:\Roms\ps3\Some Game.ps3\)");
    CHECK(expanded("{rom}", folder) == R"(D:\Roms\ps3\Some Game.ps3)");
    CHECK(expanded("{rom_file}", folder) == "Some Game.ps3");
    CHECK(expanded("{rom_name}", folder) == "Some Game");
    CHECK(expanded("{rom_dir}", folder) == R"(D:\Roms\ps3)");
    // No extension, a leading dot, several dots: the last extension only.
    const TemplateContext bare = values.context(LR"(C:\Games\dos\GAME)");
    CHECK(expanded("{rom_ext}", bare).empty());
    CHECK(expanded("{rom_name}", bare) == "GAME");
    CHECK(expanded("{rom_ext}", values.context(LR"(C:\Games\gb\.hidden)")).empty());
    const TemplateContext dots = values.context(LR"(C:\Games\x\Game v1.1 (Disc 1).tar.gz)");
    CHECK(expanded("{rom_name}", dots) == "Game v1.1 (Disc 1).tar");
    CHECK(expanded("{rom_ext}", dots) == ".gz");
    // Non-ASCII names come out in UTF-8.
    const TemplateContext unicode = values.context(rv::core::pathFromUtf8("C:\\Giochi\\Pok\xC3\xA9mon \xE2\x80\x93 "
                                                                         "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83"
                                                                         "\xB3 \xF0\x9F\x8E\xAE.gb"));
    CHECK(expanded("{rom_name}", unicode) ==
          "Pok\xC3\xA9mon \xE2\x80\x93 \xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3 \xF0\x9F\x8E\xAE");
}

TEST_CASE("launcher/arg_template: tokens without a value are errors", "[launcher][arg_template]") {
    TemplateContext empty;
    for (const std::string_view element : {"{rom}", "{rom_dir}", "{rom_name}", "{rom_file}", "{rom_ext}",
                                           "{emulator_dir}", "{system}", "{env:APPDATA}"}) {
        const rv::Error error = failure(element, empty);
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message.find(element) != std::string::npos);
    }
    // Checked rather than expanded, the paths need no value; what is wrong is still found.
    CHECK(checkTemplateElement("-L {rom} {rom_dir} {emulator_dir} {system}", empty).has_value());
    const auto unknown = checkTemplateElement("{var:core}", empty);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    CHECK_FALSE(checkTemplateElement("{bogus}", empty).has_value());
}

TEST_CASE("launcher/arg_template: variables, most specific scope first", "[launcher][arg_template]") {
    Values values;
    values.profile = {{"cores", "{emulator_dir}\\cores"}, {"core", "{var:cores}\\default_libretro.dll"}, {"x", "p"}};
    values.system = {{"core", "{var:cores}\\gambatte_libretro.dll"}, {"x", "s"}};
    values.launch = {{"x", "l"}};
    const TemplateContext context = values.context();
    CHECK(expanded("{var:core}", context) == R"(C:\Emulators\RetroArch\cores\gambatte_libretro.dll)");
    CHECK(expanded("{var:x}", context) == "l");
    // A variable's own variables are looked up from the first scope again: the launch's cores win in the system's core.
    values.launch = {{"cores", R"(D:\My Cores)"}};
    CHECK(expanded("-L {var:core}", values.context()) == R"(-L D:\My Cores\gambatte_libretro.dll)");
    // Names are exact: case matters.
    const rv::Error wrongCase = failure("{var:Core}", values.context());
    CHECK(wrongCase.code == ErrorCode::NotFound);
    CHECK(wrongCase.message == "unknown variable {var:Core}");
    // Inside a variable, the outer one is named.
    values.system = {{"core", "{var:missing}\\x.dll"}};
    const rv::Error nested = failure("{var:core}", values.context());
    CHECK(nested.code == ErrorCode::NotFound);
    CHECK(nested.message == "unknown variable {var:missing} (in {var:core})");
    // A variable whose value does not parse.
    values.system = {{"core", "{bogus}"}};
    const rv::Error malformed = failure("{var:core}", values.context());
    CHECK(malformed.code == ErrorCode::ParseError);
    CHECK(malformed.message.find("in {var:core}: unknown token {bogus}") != std::string::npos);
    // Values of variables expand into one argument, separators and all.
    values.system = {{"core", "a b \"c\" & d"}};
    CHECK(expanded("{var:core}", values.context()) == "a b \"c\" & d");

    CHECK(isValidVariableName("core"));
    CHECK(isValidVariableName("Core_2.x-y"));
    CHECK_FALSE(isValidVariableName(""));
    CHECK_FALSE(isValidVariableName("a b"));
    CHECK_FALSE(isValidVariableName("a:b"));
    CHECK_FALSE(isValidVariableName(std::string(65, 'a')));
    CHECK(isValidVariableName(std::string(64, 'a')));
}

TEST_CASE("launcher/arg_template: variables that refer to each other are named", "[launcher][arg_template]") {
    Values values;
    values.profile = {{"a", "{var:b}"}, {"b", "x{var:a}"}, {"self", "{var:self}"}};
    const rv::Error cycle = failure("{var:a}", values.context());
    CHECK(cycle.code == ErrorCode::ValidationFailed);
    CHECK(cycle.message == "variables refer to each other: {var:a} -> {var:b} -> {var:a}");
    const rv::Error self = failure("--{var:self}", values.context());
    CHECK(self.message == "variables refer to each other: {var:self} -> {var:self}");

    // A long chain that ends is fine up to 32 levels.
    values.profile.clear();
    for (int i = 0; i < 40; ++i) {
        values.profile["v" + std::to_string(i)] = i == 39 ? "end" : "{var:v" + std::to_string(i + 1) + "}";
    }
    CHECK(expanded("{var:v8}", values.context()) == "end");
    const rv::Error deep = failure("{var:v0}", values.context());
    CHECK(deep.code == ErrorCode::ValidationFailed);
    CHECK(deep.message.find("deeper than 32") != std::string::npos);
}

TEST_CASE("launcher/arg_template: environment variables of the allow-list only", "[launcher][arg_template]") {
    Values values;
    const TemplateContext context = values.context();
    CHECK(expanded("{env:APPDATA}\\RetroArch", context) == R"(C:\Users\Player\AppData\Roaming\RetroArch)");
    CHECK(expanded("{env:ProgramFiles}", context) == R"(C:\Program Files)"); // names ignore case, as in Windows
    const rv::Error unset = failure("{env:LOCALAPPDATA}", context);
    CHECK(unset.code == ErrorCode::NotFound);
    CHECK(unset.message == "the environment variable {env:LOCALAPPDATA} is not set");
    for (const std::string_view secret : {"{env:PATH}", "{env:GITHUB_TOKEN}", "{env:RV_LAUNCH_ALLOWLIST}",
                                          "{env:USERNAME}"}) {
        const rv::Error refused = failure(secret, context);
        CHECK(refused.code == ErrorCode::ValidationFailed);
        CHECK(refused.message.find(secret) != std::string::npos);
    }
    CHECK(isAllowedEnvironmentVariable("PROGRAMFILES(X86)"));
    CHECK(isAllowedEnvironmentVariable("systemdrive"));
    CHECK_FALSE(isAllowedEnvironmentVariable("PATH"));
    CHECK_FALSE(isAllowedEnvironmentVariable(""));
}

TEST_CASE("launcher/arg_template: results are UTF-8 without NUL characters", "[launcher][arg_template]") {
    Values values;
    values.launch = {{"bad", "\xC3\x28"}, {"nul", std::string("a\0b", 3)}, {"ok", "\xE6\x97\xA5"}};
    const TemplateContext context = values.context();
    const std::string ok = expanded("{var:ok}", context);
    CHECK(rv::launcher::isValidUtf8(ok));
    CHECK(failure("{var:bad}", context).code == ErrorCode::ValidationFailed);
    CHECK(failure("{var:nul}", context).code == ErrorCode::ValidationFailed);
}

TEST_CASE("launcher/arg_template: one element is one argument, whatever the ROM path holds",
          "[launcher][arg_template]") {
    const std::vector<std::string> elements{"-L", "{var:core}", "{rom}", "-image={rom}", "{rom_name}", "--",
                                            "{rom_dir}"};
    std::vector<std::string> hostile{
        R"(C:\Games\gb\two  spaces\Mole Mania (USA, Europe) (SGB Enhanced).gb)",
        R"(C:\Games\it's a game\Tom's "quoted" Adventure.gb)",
        R"(C:\Games\a & b\R&D %PATH% ^caret^ ;semi; =equals=\x.gb)",
        R"(C:\Games\trailing backslash\)",
        "C:\\Giochi\\Pok\xC3\xA9mon \xE2\x80\x93 \xC3\x89" "dition Bleue\\\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83"
        "\xB3 \xF0\x9F\x8E\xAE.gb",
        R"(\\nas\share\roms\gb\game.gb)",
    };
    std::string deep = R"(C:\Games\)";
    while (deep.size() < 300) {
        deep += "A folder with a long name and spaces\\";
    }
    hostile.push_back(deep + "Game (Europe) (En,Fr,De).gb");
    for (const std::string& rom : hostile) {
        INFO(rom);
        Values values;
        values.launch = {{"core", R"(C:\Emulators\RetroArch\cores\gambatte_libretro.dll)"}};
        const TemplateContext context = values.context(rv::core::pathFromUtf8(rom));
        const auto arguments = expandTemplate(elements, context);
        REQUIRE(arguments.has_value());
        REQUIRE(arguments->size() == elements.size());
        std::string expectedRom = rom;
        if (expectedRom.ends_with('\\')) {
            expectedRom.pop_back(); // a folder named with its separator
        }
        CHECK((*arguments)[2] == expectedRom);
        CHECK((*arguments)[3] == "-image=" + expectedRom);
        for (const std::string& argument : *arguments) {
            CHECK(rv::launcher::isValidUtf8(argument));
        }
#if defined(_WIN32)
        const auto line = rv::launcher::buildCommandLine(R"(C:\Emulators\RetroArch\retroarch.exe)", *arguments);
        REQUIRE(line.has_value());
        std::vector<std::string> expected{R"(C:\Emulators\RetroArch\retroarch.exe)"};
        expected.insert(expected.end(), arguments->begin(), arguments->end());
        CHECK(commandLineToArgv(*line) == expected);
#endif
    }
}

TEST_CASE("launcher/arg_template: an error names the element", "[launcher][arg_template]") {
    Values values;
    const std::vector<std::string> elements{"-L", "{var:core}", "{rom}"};
    const auto arguments = expandTemplate(elements, values.context());
    REQUIRE_FALSE(arguments.has_value());
    CHECK(arguments.error().code == ErrorCode::NotFound);
    CHECK(arguments.error().message == "unknown variable {var:core}");
    CHECK(arguments.error().context == "[1] {var:core}");
}
