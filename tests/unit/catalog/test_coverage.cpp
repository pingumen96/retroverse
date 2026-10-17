// Media coverage (rv/catalog/coverage.hpp, I2 step 8) on the domain alone: which entries each population counts, the
// counts and shares per system and in total, members whose media are not known, and what measureCoverage() asks of a
// resolver (a scripted one here; the ES-DE resolver on the standard fixture is in tests/unit/esde).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/catalog/coverage.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/catalog/snapshot.hpp>

using rv::ErrorCode;
using rv::Expected;
using rv::makeError;
using rv::catalog::Availability;
using rv::catalog::CatalogEntry;
using rv::catalog::CoveragePopulation;
using rv::catalog::EntryMedia;
using rv::catalog::EntryState;
using rv::catalog::MediaKind;

namespace {

rv::catalog::SystemName systemNamed(std::string_view name) {
    auto parsed = rv::catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// An entry of `system` at `relativePath` (empty: outside its system folder) in `state`; `gamelist` gives it a gamelist
/// path, the file is seen for every state but MissingFile and OutsideSystemFolder.
CatalogEntry entryOf(std::string_view system, std::string_view relativePath, EntryState state, bool gamelist,
                     bool directory = false) {
    CatalogEntry entry;
    entry.system = systemNamed(system);
    entry.state = state;
    if (!relativePath.empty()) {
        auto key = rv::catalog::makeLookupKey(relativePath);
        REQUIRE(key.has_value());
        entry.key = *key;
    }
    if (gamelist) {
        entry.gamelistPath = relativePath.empty() ? std::string("./../elsewhere.gb") : "./" + std::string(relativePath);
    }
    if (state == EntryState::MissingFile) {
        entry.availability = Availability::Missing;
    } else if (state != EntryState::OutsideSystemFolder) {
        entry.file = rv::catalog::FileFacts{directory, directory ? 0u : 1024u, 0, false};
    }
    return entry;
}

EntryMedia withKinds(std::initializer_list<MediaKind> kinds) {
    EntryMedia media;
    for (const MediaKind kind : kinds) {
        media[kind] = rv::catalog::MediaPresence{true, ".png"};
    }
    return media;
}

/// A resolver that answers from a table by "<system>/<relative path>", records what it was asked, and makes the keys
/// it is told Unavailable or InvalidArgument.
class ScriptedResolver final : public rv::catalog::IMediaResolver {
public:
    std::map<std::string, EntryMedia> media;
    std::map<std::string, ErrorCode> failures;
    std::vector<std::pair<std::string, bool>> asked; ///< "<system>/<relative path>" and `directory`

    Expected<EntryMedia> presence(const rv::catalog::SystemName& system, const rv::catalog::LookupKey& key,
                                  bool directory) override {
        const std::string name = system.str() + "/" + key.relativePath();
        asked.emplace_back(name, directory);
        if (const auto failure = failures.find(name); failure != failures.end()) {
            return makeError(failure->second, "scripted failure for " + name, name);
        }
        const auto found = media.find(name);
        return found == media.end() ? EntryMedia{} : found->second;
    }
    Expected<std::filesystem::path> path(const rv::catalog::SystemName&, const rv::catalog::LookupKey&, bool, MediaKind,
                                         const rv::catalog::MediaPresence&) const override {
        return makeError(ErrorCode::Unsupported, "not scripted");
    }
};

} // namespace

TEST_CASE("catalog/coverage: the populations an entry belongs to", "[catalog][coverage]") {
    using P = CoveragePopulation;
    const auto in = [](const CatalogEntry& entry) {
        std::vector<P> populations;
        for (const P population : rv::catalog::kCoveragePopulations) {
            if (rv::catalog::inPopulation(entry, population)) {
                populations.push_back(population);
            }
        }
        return populations;
    };
    // A scraped game: every population.
    CHECK(in(entryOf("gb", "A.gb", EntryState::Game, true)) ==
          std::vector<P>{P::GamelistEntries, P::GamelistEntriesWithFile, P::ScrapedGames, P::Games});
    // An unscraped game: the games only.
    CHECK(in(entryOf("gb", "B.gb", EntryState::Game, false)) == std::vector<P>{P::Games});
    // A gamelist entry whose file is gone: ADR-0003's denominator only.
    CHECK(in(entryOf("gb", "C.gb", EntryState::MissingFile, true)) == std::vector<P>{P::GamelistEntries});
    // An existing file ES-DE does not show (ps2's .cue): a gamelist entry with its file, no game.
    auto notAGame = entryOf("ps2", "D.cue", EntryState::NotAGame, true);
    notAGame.notAGameReason = rv::catalog::NotAGameReason::ExtensionNotConfigured;
    CHECK(in(notAGame) == std::vector<P>{P::GamelistEntries, P::GamelistEntriesWithFile});
    CHECK(in(entryOf("ps2", "E.cue", EntryState::NotAGame, false)).empty());
    // A disc member a gamelist names.
    CHECK(in(entryOf("psx", "F (Track 1).bin", EntryState::DiscMember, true)) ==
          std::vector<P>{P::GamelistEntries, P::GamelistEntriesWithFile});
    CHECK(in(entryOf("psx", "G (Track 1).bin", EntryState::DiscMember, false)).empty());
    // A folder is no game, whatever its gamelist entry.
    CHECK(in(entryOf("snes", "Hacks", EntryState::Folder, true, true)).empty());
    // Outside its system folder: a gamelist entry, never a file.
    CHECK(in(entryOf("gb", "", EntryState::OutsideSystemFolder, true)) == std::vector<P>{P::GamelistEntries});
    // A scraped game under a folder that could not be listed: a game, its file not seen.
    auto unavailable = entryOf("gb", "H.gb", EntryState::Game, true);
    unavailable.availability = Availability::SourceUnavailable;
    unavailable.file.reset();
    CHECK(in(unavailable) == std::vector<P>{P::GamelistEntries, P::ScrapedGames, P::Games});

    for (const P population : rv::catalog::kCoveragePopulations) {
        CHECK(rv::catalog::parseCoveragePopulation(rv::catalog::toString(population)) == population);
    }
    CHECK(rv::catalog::toString(P::GamelistEntries) == "gamelist-entries");
    CHECK(rv::catalog::toString(P::GamelistEntriesWithFile) == "gamelist-entries-with-file");
    CHECK(rv::catalog::toString(P::ScrapedGames) == "scraped-games");
    CHECK(rv::catalog::toString(P::Games) == "games");
    CHECK_FALSE(rv::catalog::parseCoveragePopulation("Games").has_value());
}

TEST_CASE("catalog/coverage: counts and shares per system and in total; unknown media count in no share",
          "[catalog][coverage]") {
    rv::catalog::CoverageBuilder builder;
    const EntryMedia coverAndVideo = withKinds({MediaKind::Cover, MediaKind::Video});
    const EntryMedia none;
    builder.add(entryOf("snes", "A.sfc", EntryState::Game, true), &coverAndVideo);
    builder.add(entryOf("gb", "A.gb", EntryState::Game, true), &coverAndVideo);
    builder.add(entryOf("gb", "B.gb", EntryState::Game, false), &none);
    builder.add(entryOf("gb", "C.gb", EntryState::MissingFile, true), &coverAndVideo);
    builder.add(entryOf("gb", "D.gb", EntryState::Game, true), nullptr);
    builder.add(entryOf("gb", "Folder", EntryState::Folder, true, true), &coverAndVideo); // in no population
    builder.noteUnknown(systemNamed("gb"), "the media folder cannot be listed");
    builder.noteUnknown(systemNamed("gb"), "a later reason is not kept");
    const rv::catalog::MediaCoverage coverage = builder.finish();

    REQUIRE(coverage.systems.size() == 2);
    CHECK(coverage.systems[0].system.str() == "gb"); // by name
    CHECK(coverage.systems[1].system.str() == "snes");
    const rv::catalog::SystemCoverage* gb = coverage.find(systemNamed("gb"));
    REQUIRE(gb != nullptr);
    CHECK(coverage.find(systemNamed("psx")) == nullptr);

    const rv::catalog::CoverageCounts& entries = (*gb)[CoveragePopulation::GamelistEntries];
    CHECK(entries.entries == 3); // A, C, D
    CHECK(entries.unknown == 1); // D
    CHECK(entries.known() == 2);
    CHECK(entries.with(MediaKind::Cover) == 2);
    CHECK(entries.with(MediaKind::Manual) == 0);
    CHECK(entries.withAny == 2);
    CHECK(entries.share(MediaKind::Cover) == 1.0);
    CHECK(entries.share(MediaKind::Manual) == 0.0);
    const rv::catalog::CoverageCounts& games = (*gb)[CoveragePopulation::Games];
    CHECK(games.entries == 3); // A, B, D
    CHECK(games.unknown == 1);
    CHECK(games.with(MediaKind::Video) == 1);
    CHECK(games.share(MediaKind::Video) == 0.5);
    CHECK(games.anyShare() == 0.5);
    CHECK((*gb)[CoveragePopulation::GamelistEntriesWithFile].entries == 2); // A, D
    CHECK((*gb)[CoveragePopulation::ScrapedGames].entries == 2);

    const rv::catalog::CoverageCounts& total = coverage.total[CoveragePopulation::Games];
    CHECK(coverage.total.system.empty());
    CHECK(total.entries == 4);
    CHECK(total.unknown == 1);
    CHECK(total.with(MediaKind::Cover) == 2);
    CHECK(total.withAny == 2);

    REQUIRE(coverage.issues.size() == 1);
    CHECK(coverage.issues[0].code == rv::catalog::kCoverageIssueMediaUnknown);
    CHECK(coverage.issues[0].system == "gb");
    CHECK(coverage.issues[0].severity == rv::catalog::IssueSeverity::Warning);
    CHECK(coverage.issues[0].reason.find("1 entry of gb") != std::string::npos);
    CHECK(coverage.issues[0].reason.find("the media folder cannot be listed") != std::string::npos);

    // Nothing known: no share.
    rv::catalog::CoverageCounts unknown;
    unknown.entries = 2;
    unknown.unknown = 2;
    CHECK_FALSE(unknown.share(MediaKind::Cover).has_value());
    CHECK_FALSE(unknown.anyShare().has_value());
    CHECK_FALSE(rv::catalog::CoverageCounts{}.share(MediaKind::Cover).has_value());
    CHECK(rv::catalog::CoverageBuilder{}.finish() == rv::catalog::MediaCoverage{});
}

TEST_CASE("catalog/coverage: measureCoverage asks the resolver for the members only", "[catalog][coverage]") {
    rv::catalog::LibrarySnapshot snapshot;
    snapshot.entries = {
        entryOf("gb", "A.gb", EntryState::Game, true),
        entryOf("gb", "Collection", EntryState::Folder, true, true),
        entryOf("gb", "Game Folder.gb", EntryState::Game, false, true),
        entryOf("gb", "", EntryState::OutsideSystemFolder, true),
        entryOf("gb", "Gone.gb", EntryState::MissingFile, true),
        entryOf("gb", "Locked/B.gb", EntryState::Game, false),
        entryOf("ps2", "C.cue", EntryState::NotAGame, false),
        entryOf("snes", "D.sfc", EntryState::Game, true),
    };
    std::sort(snapshot.entries.begin(), snapshot.entries.end(), rv::catalog::entryOrder);
    ScriptedResolver resolver;
    resolver.media["gb/A.gb"] = withKinds({MediaKind::Cover});
    resolver.media["gb/Game Folder.gb"] = withKinds({MediaKind::Screenshot});
    resolver.media["gb/Gone.gb"] = withKinds({MediaKind::Cover, MediaKind::Manual});
    resolver.media["snes/D.sfc"] = withKinds({MediaKind::Video});
    resolver.failures["gb/Locked/B.gb"] = ErrorCode::Unavailable;

    const auto coverage = rv::catalog::measureCoverage(snapshot, resolver);
    REQUIRE(coverage.has_value());
    // The folder, the entry outside its folder (no key) and the file that is no game in any population are not asked;
    // a game that is a folder is asked as one.
    CHECK(resolver.asked == std::vector<std::pair<std::string, bool>>{{"gb/A.gb", false},
                                                                      {"gb/Game Folder.gb", true},
                                                                      {"gb/Gone.gb", false},
                                                                      {"gb/Locked/B.gb", false},
                                                                      {"snes/D.sfc", false}});
    const rv::catalog::SystemCoverage* gb = coverage->find(systemNamed("gb"));
    REQUIRE(gb != nullptr);
    const auto& entries = (*gb)[CoveragePopulation::GamelistEntries];
    CHECK(entries.entries == 3); // A, outside, Gone
    CHECK(entries.unknown == 0);
    CHECK(entries.with(MediaKind::Cover) == 2);
    CHECK(entries.with(MediaKind::Manual) == 1);
    const auto& games = (*gb)[CoveragePopulation::Games];
    CHECK(games.entries == 3); // A, Game Folder, Locked/B
    CHECK(games.unknown == 1);
    CHECK(games.with(MediaKind::Screenshot) == 1);
    REQUIRE(coverage->issues.size() == 1);
    CHECK(coverage->issues[0].system == "gb");
    CHECK(coverage->find(systemNamed("ps2")) == nullptr);
    CHECK(coverage->total[CoveragePopulation::Games].with(MediaKind::Video) == 1);

    // One system only.
    resolver.asked.clear();
    const auto snes = rv::catalog::measureCoverage(snapshot, resolver, systemNamed("snes"));
    REQUIRE(snes.has_value());
    CHECK(resolver.asked == std::vector<std::pair<std::string, bool>>{{"snes/D.sfc", false}});
    CHECK(snes->systems.size() == 1);
    CHECK(snes->total[CoveragePopulation::ScrapedGames].entries == 1);

    // Any other error says that the entries are wrong: passed on.
    resolver.failures["gb/A.gb"] = ErrorCode::InvalidArgument;
    const auto failed = rv::catalog::measureCoverage(snapshot, resolver);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ErrorCode::InvalidArgument);
}
