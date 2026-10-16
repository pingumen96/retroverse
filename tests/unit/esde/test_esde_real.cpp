// The ES-DE configuration of the real machine (ADR-0008 §10, I2 step 2). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the
// machine's folders in environment variables (every one needed, else the test is skipped):
//   RV_REAL_ESDE_HOME     the ES-DE home discovery must find (the user's home folder's ES-DE)
//   RV_REAL_ESDE_INSTALL  the install folder the ES-DE uninstall entry must lead to
//   RV_REAL_ROM_ROOT      the ROM root es_settings.xml's ROMDirectory gives
//   RV_REAL_MEDIA_ROOT    the media root MediaDirectory gives
// It discovers ES-DE as `retroverse-cli discover` does without options, then checks the reference machine's figures
// (ES-DE 3.4.1, 195 systems with 889 commands, 119 emulators and 1 core in the find rules with 444 entries, the token
// census, the commands whose program is not one %EMULATOR_X% element, the user's hand-added PCSX2 entry:
// docs/i2/step-2.md), splits every command as CommandLineToArgvW does and prints the census. Nothing is written
// anywhere.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")

#include <fmt/format.h>

#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/command_template.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/platform_win/system_probe.hpp>

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// The same folder, compared as the file system compares names.
bool samePlace(const fs::path& a, const fs::path& b) {
    return rv::core::pathIsWithin(a, b) && rv::core::pathIsWithin(b, a);
}

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

std::vector<std::string> shellSplit(const std::string& commandLine) {
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

} // namespace

TEST_CASE("esde/real: the real installation's discovery, settings, systems, find rules and command tokens",
          "[esde][.real-library]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }

    // Discovery as `retroverse-cli discover` runs it without options: the registry, the user's home folder.
    const rv::platform_win::Win32SystemProbe probe;
    const auto discovery = rv::esde::discoverEsde({}, probe);
    REQUIRE(discovery.has_value());
    for (const auto& issue : discovery->issues) {
        WARN(issue.code << ": " << issue.reason);
    }
    REQUIRE(discovery->installFolder.has_value());
    CHECK(discovery->installSource == rv::esde::InstallSource::UninstallEntry);
    CHECK(samePlace(*discovery->installFolder, *install));
    CHECK(discovery->version == "3.4.1");
    CHECK(discovery->executableFound);
    REQUIRE(discovery->esdeHome.has_value());
    CHECK(samePlace(*discovery->esdeHome, *home));
    CHECK(discovery->esdeHomeExists);
    CHECK(discovery->homeSource == rv::esde::HomeSource::Default);
    const auto profile = rv::esde::profileOf(*discovery, rv::catalog::LibraryId());
    REQUIRE(profile.has_value());

    const auto settings = rv::esde::readSettings(profile->esdeHome);
    REQUIRE(settings.has_value());
    CHECK(settings->source == rv::esde::SettingsSource::File);
    const auto rom = rv::esde::resolveRomRoot(*settings, *profile);
    const auto media = rv::esde::resolveMediaRoot(*settings, *profile);
    REQUIRE(rom.path.has_value());
    REQUIRE(media.path.has_value());
    CHECK(samePlace(*rom.path, *romRoot));
    CHECK(samePlace(*media.path, *mediaRoot));
    CHECK(rom.source == rv::esde::RootSource::Setting);
    CHECK(media.source == rv::esde::RootSource::Setting);

    const auto systems = rv::esde::loadSystems(*profile);
    INFO((systems ? "systems read" : systems.error().toString()));
    REQUIRE(systems.has_value());
    CHECK(systems->systems.size() == 195);
    CHECK(systems->commandCount() == 889);
    CHECK(systems->fallbacks.empty());
    CHECK_FALSE(systems->customFile.has_value());
    std::vector<rv::catalog::Issue> infoIssues;
    const auto infos = rv::esde::systemInfos(*systems, rom, *profile, infoIssues);
    CHECK(infoIssues.empty());
    const auto withFolder = std::count_if(infos.begin(), infos.end(), [](const rv::catalog::SystemInfo& system) {
        return system.availability == rv::catalog::Availability::Available;
    });

    const rv::esde::FindRules rules = rv::esde::loadFindRules(*profile);
    CHECK(rules.emulators.size() == 119);
    CHECK(rules.cores.size() == 1);
    CHECK(rules.issues.empty());

    // Every command splits as CommandLineToArgvW does, and parses into the same elements; the census of its tokens.
    std::map<std::string, std::pair<std::size_t, std::size_t>> census; // key -> (occurrences, commands)
    std::size_t placeholders = 0;
    std::vector<std::string> notEmulatorPrograms;
    for (const rv::esde::SystemDefinition& system : systems->systems) {
        for (const rv::esde::SystemCommand& command : system.commands) {
            INFO(system.name.str() << " / " << command.label << ": " << command.text);
            const std::vector<std::string> expected = shellSplit(command.text);
            CHECK(rv::esde::splitCommandLine(command.text) == expected);
            const rv::esde::CommandTemplate parsed = rv::esde::parseCommandTemplate(command.text);
            std::vector<std::string> elements;
            std::set<std::string> keys;
            for (const rv::esde::CommandElement& element : parsed.elements) {
                elements.push_back(element.text);
                for (const rv::esde::CommandToken& token : element.tokens) {
                    const std::string key = rv::esde::censusKey(token);
                    ++census[key].first;
                    keys.insert(key);
                }
            }
            CHECK(elements == expected);
            for (const std::string& key : keys) {
                ++census[key].second;
            }
            placeholders += command.placeholder ? 1 : 0;
            const bool emulatorProgram =
                parsed.programIndex < parsed.elements.size() &&
                parsed.elements[parsed.programIndex].tokens.size() == 1 &&
                parsed.elements[parsed.programIndex].tokens[0].kind == rv::esde::CommandTokenKind::Emulator &&
                parsed.elements[parsed.programIndex].tokens[0].text == parsed.elements[parsed.programIndex].text;
            if (!emulatorProgram) {
                notEmulatorPrograms.push_back(system.name.str() + " / " + command.label);
            }
        }
    }
    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> sorted(census.begin(), census.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    fmt::print("ES-DE {} at {}: {} systems ({} with a ROM folder), {} commands; find rules: {} emulators, {} core(s), "
               "{} entries\n",
               discovery->version, rv::core::pathToUtf8(*discovery->installFolder), systems->systems.size(), withFolder,
               systems->commandCount(), rules.emulators.size(), rules.cores.size(), rules.entryCount());
    fmt::print("Command token census (occurrences, commands):\n");
    for (const auto& [key, counts] : sorted) {
        fmt::print("  {:<22} {:>5} {:>5}\n", key, counts.first, counts.second);
    }
    fmt::print("Placeholder commands: {}\n", placeholders);
    fmt::print("Commands whose program is not one %EMULATOR_X% element: {}\n", notEmulatorPrograms.size());
    for (const std::string& command : notEmulatorPrograms) {
        fmt::print("  {}\n", command);
    }
    // The reference machine's figures docs/i2/step-2.md reports: the commands whose program is not one %EMULATOR_X%
    // element (the placeholders and the user's hand edits of gc and ps2), the census, the user's hand-added PCSX2 entry.
    CHECK(placeholders == 4);
    CHECK(notEmulatorPrograms == std::vector<std::string>{"androidapps / ", "androidgames / ", "gc / Dolphin",
                                                          "gc / Dolphin (Standalone)", "lutris / ",
                                                          "ps2 / PCSX2 (Standalone)", "xboxone / "});
    const std::map<std::string, std::pair<std::size_t, std::size_t>> expectedCensus{
        {"%EMULATOR_X%", {882, 882}},  {"%ROM%", {799, 799}},           {"%CORE_X%", {413, 413}},
        {"%STARTDIR%=", {176, 176}},   {"%EMUDIR%", {158, 158}},        {"%GAMEDIR%", {116, 110}},
        {"%HIDEWINDOW%", {115, 115}},  {"%ROMPATH%", {110, 107}},       {"%BASENAME%", {98, 89}},
        {"%INJECT%=", {41, 41}},       {"%ESCAPESPECIALS%", {34, 34}},  {"%GAMEDIRRAW%", {25, 22}},
        {"%ROMRAW%", {18, 18}},        {"%RUNINBACKGROUND%", {13, 13}}, {"%FILENAME%", {3, 3}},
        {"%RPCS3_GAMEID%", {2, 2}},
    };
    CHECK(census == expectedCensus);
    CHECK(rules.entryCount() == 444);
    const rv::esde::FindRuleSet* pcsx2 = rules.findEmulator("PCSX2");
    REQUIRE(pcsx2 != nullptr);
    const std::vector<std::string> staticPaths = pcsx2->entries(rv::esde::FindRuleType::StaticPath);
    REQUIRE_FALSE(staticPaths.empty());
    CHECK(staticPaths.front() == R"(K:\Giochi\Emulatori\PS2\pcsx2-qt.exe)");
}
