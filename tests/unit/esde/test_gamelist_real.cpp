// The real library's gamelists through rv::esde::readGamelist (I2 step 3, ADR-0008 §10). Hidden ([.real-library]):
// ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and
// after, and the folders in environment variables:
//   RV_REAL_ESDE_HOME  the ES-DE home: every <home>/gamelists/<system>/gamelist.xml is read (and nothing else)
//   RV_REAL_ROM_ROOT   ES-DE's ROMDirectory: <root>/<system> is the folder each gamelist's paths resolve against
//                      (lexically: nothing under it is read)
// The expected figures are the reference library's (ADR-0008, the survey of 2026-10-16); a library scraped or edited
// since changes them, and the test prints what it read. An independent count of the <game> and <folder> elements
// through the XML outline checks that no entry was lost on the way.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/xml_outline.hpp>

namespace fs = std::filesystem;
using rv::esde::GamelistEntryKind;

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

/// The systems with a gamelist under <home>/gamelists, by name (ordinal).
std::vector<std::string> systemsWithGamelists(const fs::path& home) {
    const auto listed = rv::core::listTree(home / "gamelists");
    REQUIRE(listed.has_value());
    std::vector<std::string> systems;
    for (const rv::core::TreeEntry& entry : *listed) {
        REQUIRE(entry.kind != rv::core::TreeEntryKind::Error);
        const std::size_t slash = entry.relativePath.find('/');
        if (entry.kind == rv::core::TreeEntryKind::File && slash != std::string::npos &&
            entry.relativePath.substr(slash) == "/gamelist.xml") {
            systems.push_back(entry.relativePath.substr(0, slash));
        }
    }
    return systems;
}

/// The <game> and <folder> elements of a gamelist's first <gameList>, counted on the XML outline.
std::pair<std::size_t, std::size_t> elementCounts(const fs::path& file) {
    const auto outline = rv::esde::outlineXmlFile(file, rv::esde::kMaxGamelistBytes);
    REQUIRE(outline.has_value());
    const rv::esde::XmlElementOutline* gameList = nullptr;
    for (const rv::esde::XmlElementOutline& element : outline->elements) {
        if (gameList == nullptr && element.name == "gameList") {
            gameList = &element;
        }
    }
    REQUIRE(gameList != nullptr);
    std::size_t games = 0;
    std::size_t folders = 0;
    for (const rv::esde::XmlElementOutline& child : gameList->children) {
        games += child.name == "game" ? 1 : 0;
        folders += child.name == "folder" ? 1 : 0;
    }
    return {games, folders};
}

} // namespace

TEST_CASE("esde/gamelist: the real library's gamelists", "[esde][gamelist][.real-library]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    if (!home || !romRoot) {
        SKIP("set RV_REAL_ESDE_HOME and RV_REAL_ROM_ROOT (scripts/real-library.ps1 does)");
    }

    const std::vector<std::string> systems = systemsWithGamelists(*home);
    std::size_t games = 0;
    std::size_t folders = 0;
    std::size_t alternativeEmulators = 0;
    std::size_t failures = 0;
    std::size_t outside = 0;
    std::vector<rv::catalog::Issue> issues;
    std::vector<rv::esde::Gamelist> gamelists;
    const auto start = std::chrono::steady_clock::now();
    for (const std::string& name : systems) {
        const auto system = rv::catalog::SystemName::parse(name);
        REQUIRE(system.has_value());
        auto read = rv::esde::readGamelist(rv::esde::gamelistPath(*home, *system), *system,
                                           *romRoot / rv::core::pathFromUtf8(name));
        if (!read) {
            ++failures;
            UNSCOPED_INFO(name << ": " << read.error().toString());
            gamelists.emplace_back();
            continue;
        }
        games += read->count(GamelistEntryKind::Game);
        folders += read->count(GamelistEntryKind::Folder);
        alternativeEmulators += read->alternativeEmulator.empty() ? 0 : 1;
        for (const rv::esde::GamelistEntry& entry : read->entries) {
            outside += entry.outsideSystemFolder() ? 1 : 0;
        }
        issues.insert(issues.end(), read->issues.begin(), read->issues.end());
        gamelists.push_back(std::move(*read));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "esde/gamelist real library: " << systems.size() << " gamelists, " << games << " games, " << folders
              << " folders, " << alternativeEmulators << " alternative emulators, " << failures << " failures, "
              << outside << " entries outside their folder, " << issues.size() << " issues, read in " << seconds
              << " s\n";
    for (const rv::catalog::Issue& issue : issues) {
        UNSCOPED_INFO(rv::catalog::toString(issue.severity) << " " << issue.code << " " << issue.system << ": "
                                                            << issue.reason);
    }

    CHECK(systems.size() == 45);
    CHECK(failures == 0);
    CHECK(games == 40870);
    CHECK(folders == 155);
    CHECK(alternativeEmulators == 6);
    CHECK(outside == 0);
    CHECK(issues.empty());

    // Every <game> and <folder> element became an entry: none skipped, none merged as a duplicate.
    for (std::size_t i = 0; i < systems.size(); ++i) {
        INFO(systems[i]);
        const auto [gameElements, folderElements] =
            elementCounts(rv::esde::gamelistPath(*home, rv::catalog::SystemName::parse(systems[i]).value()));
        CHECK(gamelists[i].count(GamelistEntryKind::Game) == gameElements);
        CHECK(gamelists[i].count(GamelistEntryKind::Folder) == folderElements);
    }
}
