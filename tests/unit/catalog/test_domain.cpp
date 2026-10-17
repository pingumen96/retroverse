// The catalog's plain types and ports (entry, system, media, disc group, library, play session, change set, snapshot,
// query; ICatalogQuery, ILibrarySource, ICatalogStore, IMediaResolver): names of values, helpers, and minimal in-memory
// implementations that use each port as its header documents it (the paging contract of ADR-0008 §5 above all).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <rv/catalog/catalog_query.hpp>
#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/change_set.hpp>
#include <rv/catalog/disc_group.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/identity.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>

using namespace rv::catalog;

namespace {

SystemName systemNamed(std::string_view name) {
    return SystemName::parse(name).value();
}

LookupKey key(std::string_view relativePath) {
    return makeLookupKey(relativePath).value();
}

GameId id(std::uint64_t value) {
    return GameId::fromValue(value).value();
}

/// Every value of an enumeration from 0 to `last` has a name of its own that parses back.
template <class Enum, class Parse>
void checkNames(Enum last, Parse parse) {
    std::vector<std::string_view> names;
    for (int value = 0; value <= static_cast<int>(last); ++value) {
        const auto e = static_cast<Enum>(value);
        const std::string_view name = toString(e);
        INFO(name);
        CHECK(name != "unknown");
        CHECK(parse(name) == e);
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
    CHECK_FALSE(parse("unknown").has_value());
    CHECK_FALSE(parse("").has_value());
}

CatalogEntry entry(std::string_view systemName, std::string_view relativePath, std::string title,
                   EntryState state = EntryState::Game) {
    CatalogEntry e;
    e.system = systemNamed(systemName);
    e.key = key(relativePath);
    e.state = state;
    e.title = std::move(title);
    e.file = FileFacts{false, 1024, 0, false};
    return e;
}

/// An in-memory ICatalogQuery over indexed entries: games and missing files by default, the search a case-insensitive
/// substring of the title (enough for the contract; the real search is FTS5's), Title and System orders.
class MemoryCatalogQuery final : public ICatalogQuery {
public:
    explicit MemoryCatalogQuery(std::vector<IndexedEntry> entries) : m_entries(std::move(entries)) {}

    void remove(GameId gameId) {
        std::erase_if(m_entries, [&](const IndexedEntry& e) { return e.id == gameId; });
    }

    rv::Expected<std::vector<GameId>> run(const CatalogQuery& query) override {
        const std::vector<EntryState> states =
            query.states.empty() ? std::vector<EntryState>{EntryState::Game, EntryState::MissingFile} : query.states;
        const std::string text = sortKeyOf(query.text).value();
        std::vector<const IndexedEntry*> matches;
        for (const IndexedEntry& e : m_entries) {
            const CatalogEntry& c = e.entry;
            const bool stateOk = std::find(states.begin(), states.end(), c.state) != states.end();
            const bool systemOk = query.systems.empty() || std::find(query.systems.begin(), query.systems.end(),
                                                                     c.system) != query.systems.end();
            const bool availabilityOk =
                (query.includeUnavailable || c.availability == Availability::Available) &&
                (query.availabilities.empty() || std::find(query.availabilities.begin(), query.availabilities.end(),
                                                           c.availability) != query.availabilities.end());
            const bool textOk = text.empty() || e.sortKey.find(text) != std::string::npos;
            if (stateOk && systemOk && availabilityOk && textOk) {
                matches.push_back(&e);
            }
        }
        std::sort(matches.begin(), matches.end(), [&](const IndexedEntry* a, const IndexedEntry* b) {
            if (query.sort == SortOrder::System && a->entry.system != b->entry.system) {
                return a->entry.system < b->entry.system;
            }
            return std::tie(a->sortKey, a->id) < std::tie(b->sortKey, b->id);
        });
        std::vector<GameId> ids;
        for (const IndexedEntry* e : matches) {
            ids.push_back(e->id);
        }
        return ids;
    }

    rv::Expected<std::vector<CatalogRow>> fetch(std::span<const GameId> ids) override {
        std::vector<CatalogRow> rows;
        for (const GameId gameId : ids) {
            const auto found =
                std::find_if(m_entries.begin(), m_entries.end(), [&](const IndexedEntry& e) { return e.id == gameId; });
            if (found == m_entries.end()) {
                continue;
            }
            const CatalogEntry& c = found->entry;
            CatalogRow row;
            row.id = found->id;
            row.system = c.system;
            row.title = c.title;
            row.state = c.state;
            row.availability = c.availability;
            row.releaseDate = c.metadata.releaseDate;
            row.developer = displayText(c.metadata.developer);
            row.favorite = c.flags.favorite;
            rows.push_back(std::move(row));
        }
        return rows;
    }

private:
    std::vector<IndexedEntry> m_entries;
};

IndexedEntry indexed(std::uint64_t value, CatalogEntry e) {
    std::string sortKey = sortKeyOf(e).value();
    return IndexedEntry{id(value), std::move(e), EntryMedia{}, std::move(sortKey)};
}

} // namespace

TEST_CASE("catalog/domain: every value has a name that parses back", "[catalog][domain]") {
    checkNames(EntryState::OutsideSystemFolder, parseEntryState);
    checkNames(NotAGameReason::ExcludedBySetting, parseNotAGameReason);
    checkNames(Availability::Missing, parseAvailability);
    checkNames(SourceUnavailableReason::NotListable, parseSourceUnavailableReason);
    checkNames(GamelistState::Failed, parseGamelistState);
    checkNames(DiscGroupKind::M3u, parseDiscGroupKind);
    checkNames(LibraryAdapter::Esde, parseLibraryAdapter);
    checkNames(PlaySessionOutcome::Interrupted, parsePlaySessionOutcome);
    checkNames(MoveReason::ReviewResolved, parseMoveReason);
    checkNames(IssueSeverity::Error, parseIssueSeverity);
    checkNames(SortOrder::ReleaseDate, parseSortOrder);
    checkNames(MediaKind::Custom, parseMediaKind);
    CHECK(toString(EntryState::MissingFile) == "missing-file");
    CHECK(toString(Availability::SourceUnavailable) == "source-unavailable");
    CHECK(toString(LibraryAdapter::Esde) == "esde");
}

TEST_CASE("catalog/domain: the twelve media kinds and their ES-DE folders", "[catalog][domain]") {
    REQUIRE(kMediaKinds.size() == kMediaKindCount);
    const std::vector<std::string_view> folders{"covers",      "backcovers",   "3dboxes",  "physicalmedia",
                                                "screenshots", "titlescreens", "marquees", "miximages",
                                                "fanart",      "manuals",      "videos",   "custom"};
    for (std::size_t i = 0; i < kMediaKindCount; ++i) {
        CHECK(static_cast<std::size_t>(kMediaKinds[i]) == i);
        CHECK(toString(kMediaKinds[i]) == folders[i]);
    }
    CHECK_FALSE(parseMediaKind("Covers").has_value()); // ES-DE's folders are lowercase

    EntryMedia media;
    CHECK_FALSE(media.any());
    CHECK(media.presenceMask() == 0);
    media[MediaKind::Cover] = MediaPresence{true, ".png"};
    media[MediaKind::Video] = MediaPresence{true, ".mp4"};
    CHECK(media.any());
    CHECK(media.presenceMask() == ((1u << 0) | (1u << 10)));
    CHECK(media[MediaKind::Cover].extension == ".png");
    CHECK_FALSE(media[MediaKind::Screenshot].present);
    EntryMedia same = media;
    CHECK(same == media);
    same[MediaKind::Cover].extension = ".jpg";
    CHECK(same != media);
}

TEST_CASE("catalog/domain: media are named after the relative path, a folder's name kept whole", "[catalog][domain]") {
    // A file: the last extension of its name removed, sub-folders kept as they are (dots in them too).
    CHECK(mediaStem("Collection A/Pocket Puzzle (World).gb", false) == "Collection A/Pocket Puzzle (World)");
    CHECK(mediaStem("Deep Shock (Europe) (En,Fr,De,Es,It).dec.iso", false) ==
          "Deep Shock (Europe) (En,Fr,De,Es,It).dec");
    CHECK(mediaStem("witching hour", false) == "witching hour");
    CHECK(mediaStem("Sports Vol. 2/witching hour", false) == "Sports Vol. 2/witching hour");
    CHECK(mediaStem(".Dot Hero (World).gb", false) == ".Dot Hero (World)");
    // Never trimmed, never made up: what ES-DE builds.
    CHECK(mediaStem("Galactic Disk .atr", false) == "Galactic Disk ");
    CHECK(mediaStem("Sub/.gb", false) == "Sub/");
    // A folder (a Folder entry, or a game that is a folder): its whole name.
    CHECK(mediaStem("Arcade/Sports Vol. 2", true) == "Arcade/Sports Vol. 2");
    CHECK(mediaStem("Top Picks - example.org", true) == "Top Picks - example.org");
    CHECK(mediaStem("Deep Shock (Europe).ps3", true) == "Deep Shock (Europe).ps3");
}

TEST_CASE("catalog/domain: file times in FILETIME ticks", "[catalog][domain]") {
    // 2001-02-03T04:05:06Z is 12,625,646,706 s after 1601-01-01 (a Unix time of 981,173,106 plus 11,644,473,600).
    const auto utc = rv::core::parseUtcIso8601("2001-02-03T04:05:06Z").value();
    const auto fileTime = std::chrono::clock_cast<std::chrono::file_clock>(utc);
    CHECK(fileTimeTicks(fileTime) == 126'256'467'060'000'000);
    CHECK(fileTimeFromTicks(126'256'467'060'000'000) == fileTime);
    CHECK(fileTimeTicks(fileTimeFromTicks(1)) == 1);
}

TEST_CASE("catalog/domain: an entry's title and sort key", "[catalog][domain]") {
    CatalogEntry tiny = entry("gb", "Tiny Tennis (Europe).gb", "Tiny Tennis");
    CHECK(sortKeyOf(tiny).value() == "tiny tennis");
    tiny.metadata.sortName = "  Tennis, Tiny ";
    CHECK(sortKeyOf(tiny).value() == "tennis, tiny");
    tiny.metadata.sortName = " \n";
    CHECK(sortKeyOf(tiny).value() == "tiny tennis");
    CHECK_FALSE(tiny.fromGamelist());
    tiny.gamelistPath = "./Tiny Tennis (Europe).gb";
    CHECK(tiny.fromGamelist());
    CHECK(tiny == tiny);
    CatalogEntry other = tiny;
    other.flags.favorite = true;
    CHECK(other != tiny);
}

TEST_CASE("catalog/domain: snapshots order entries by system, key, then gamelist path", "[catalog][domain]") {
    LibrarySnapshot snapshot;
    snapshot.entries = {entry("snes", "Alpha Quest (USA).sfc", "Alpha Quest"),
                        entry("gb", "Tiny Tennis (Europe).gb", "Tiny Tennis"),
                        entry("gb", "TINY TENNIS (EUROPE).gb", "TINY TENNIS"),
                        entry("gb", "Collection A/Pocket Puzzle (World).gb", "Pocket Puzzle")};
    CatalogEntry outsideB = entry("gb", "x.gb", "Wrong Way B", EntryState::OutsideSystemFolder);
    outsideB.key = LookupKey();
    outsideB.gamelistPath = "./../snes/b.sfc";
    CatalogEntry outsideA = outsideB;
    outsideA.gamelistPath = "./../snes/a.sfc";
    snapshot.entries.push_back(outsideB);
    snapshot.entries.push_back(outsideA);
    std::sort(snapshot.entries.begin(), snapshot.entries.end(), entryOrder);
    std::vector<std::string> order;
    for (const CatalogEntry& e : snapshot.entries) {
        order.push_back(e.system.str() + ":" + (e.key.empty() ? e.gamelistPath : e.key.relativePath()));
    }
    CHECK(order == std::vector<std::string>{"gb:./../snes/a.sfc", "gb:./../snes/b.sfc",
                                            "gb:Collection A/Pocket Puzzle (World).gb", "gb:TINY TENNIS (EUROPE).gb",
                                            "gb:Tiny Tennis (Europe).gb", "snes:Alpha Quest (USA).sfc"});

    SystemInfo gb;
    gb.name = systemNamed("gb");
    gb.fullName = "Nintendo Game Boy";
    SystemInfo snes;
    snes.name = systemNamed("snes");
    snes.gamelist = GamelistState::Read;
    snes.alternativeEmulator = "Fake Emulator (Hidden Console)"; // the gamelist's <alternativeEmulator>
    snapshot.systems = {gb, snes};
    REQUIRE(snapshot.findSystem(systemNamed("gb")) != nullptr);
    CHECK(snapshot.findSystem(systemNamed("gb"))->fullName == "Nintendo Game Boy");
    CHECK(snapshot.findSystem(systemNamed("gb"))->alternativeEmulator.empty());
    CHECK(snapshot.findSystem(systemNamed("snes"))->alternativeEmulator == "Fake Emulator (Hidden Console)");
    CHECK(snapshot.findSystem(systemNamed("psx")) == nullptr);
    CHECK(snapshot == snapshot);
}

TEST_CASE("catalog/domain: play sessions mirror the launcher's outcomes", "[catalog][domain]") {
    rv::launcher::LaunchOutcome outcome;
    outcome.state = rv::launcher::SessionState::Running;
    CHECK(sessionOutcomeOf(outcome) == PlaySessionOutcome::Running);
    outcome.state = rv::launcher::SessionState::Crashed;
    CHECK(sessionOutcomeOf(outcome) == PlaySessionOutcome::Crashed);
    outcome.state = rv::launcher::SessionState::FailedToStart;
    CHECK(sessionOutcomeOf(outcome) == PlaySessionOutcome::FailedToStart);

    PlaySession session;
    session.game = id(7);
    session.commandLabel = "Fake Emulator";
    session.started = rv::core::parseUtcIso8601("2026-10-16T10:00:00Z").value();
    session.owner = SessionOwner{1234, 133'000'000'000'000'000};
    CHECK(session.outcome == PlaySessionOutcome::Running);
    CHECK_FALSE(session.ended.has_value());

    outcome.state = rv::launcher::SessionState::Returned;
    outcome.exitCode = 0;
    outcome.exitKind = rv::launcher::ExitKind::Success;
    outcome.quickExit = true;
    outcome.message = "The game ended right away.";
    const auto ended = rv::core::parseUtcIso8601("2026-10-16T10:00:01Z").value();
    recordOutcome(session, outcome, ended);
    CHECK(session.outcome == PlaySessionOutcome::Returned);
    CHECK(session.ended == ended);
    CHECK(session.exitCode == 0u);
    CHECK(session.quickExit);
    CHECK(session.message == "The game ended right away.");
    CHECK(session.owner == SessionOwner{1234, 133'000'000'000'000'000});
    CHECK(SessionOwner{1234, 1} != SessionOwner{1234, 2}); // the same id reused by a later process
}

TEST_CASE("catalog/domain: change sets count what they hold", "[catalog][domain]") {
    ChangeSet changes;
    CHECK(changes.empty());
    CHECK(changes.counts() == ChangeSetCounts{});
    changes.issues.push_back(Issue{IssueSeverity::Warning, "outside-system-folder", "gb", "", "a path leads out"});
    CHECK(changes.empty()); // issues aside
    changes.added.push_back(ChangedGame{id(1), systemNamed("gb"), key("a.gb")});
    changes.moved.push_back(
        MovedGame{id(2), systemNamed("gb"), key("old/b.gb"), key("new/b.gb"), MoveReason::SizeTimeName});
    changes.reviews.push_back(ReviewRow{0,
                                        id(3),
                                        systemNamed("gb"),
                                        key("c.gb"),
                                        {key("c1.gb"), key("c2.gb")},
                                        "two new files of equal size, time and name"});
    CHECK_FALSE(changes.empty());
    const ChangeSetCounts counts = changes.counts();
    CHECK(counts.added == 1);
    CHECK(counts.moved == 1);
    CHECK(counts.reviews == 1);
    CHECK(counts.issues == 1);
    CHECK(counts.removed == 0);
}

TEST_CASE("catalog/domain: the paging contract", "[catalog][domain]") {
    CatalogEntry missing = entry("gb", "Gone Fishing (USA).gb", "Gone Fishing", EntryState::MissingFile);
    missing.availability = Availability::Missing;
    missing.file.reset();
    CatalogEntry member =
        entry("psx", "Twin Tracks (Europe) (Track 1).bin", "Twin Tracks (Europe) (Track 1)", EntryState::DiscMember);
    member.discHead = key("Twin Tracks (Europe).cue");
    MemoryCatalogQuery source({
        indexed(50, entry("gb", "Tiny Tennis (Europe).gb", "Tiny Tennis")),
        indexed(40, entry("snes", "Alpha Quest (USA).sfc", "Alpha Quest")),
        indexed(30, entry("gb", "Collection B/Pocket Puzzle (World).gb", "Pocket Puzzle (World)")),
        indexed(20, entry("gb", "Collection A/Pocket Puzzle (World).gb", "Pocket Puzzle (World)")),
        indexed(10, missing),
        indexed(60, member),
    });

    // The default query: games, available, by title; ties (two "Pocket Puzzle (World)") by GameId.
    const std::vector<GameId> list = source.run(CatalogQuery{}).value();
    CHECK(list == std::vector<GameId>{id(40), id(20), id(30), id(50)});
    // The count is the list's size; a page is a slice; positions are indices.
    CHECK(list.size() == 4);
    CHECK(std::vector<GameId>(pageOf(list, 1, 2).begin(), pageOf(list, 1, 2).end()) ==
          std::vector<GameId>{id(20), id(30)});
    CHECK(pageOf(list, 3, 10).size() == 1);
    CHECK(pageOf(list, 4, 10).empty());
    CHECK(pageOf(list, 100, 10).empty());
    CHECK(positionOf(list, id(30)) == 2u);
    CHECK_FALSE(positionOf(list, id(10)).has_value());

    // Unavailable games only when asked for; other states only by name.
    CatalogQuery all;
    all.includeUnavailable = true;
    CHECK(source.run(all).value().size() == 5);
    CatalogQuery onlyMissing = all;
    onlyMissing.availabilities = {Availability::Missing};
    CHECK(source.run(onlyMissing).value() == std::vector<GameId>{id(10)});
    CatalogQuery members;
    members.states = {EntryState::DiscMember};
    CHECK(source.run(members).value() == std::vector<GameId>{id(60)});
    CatalogQuery gb;
    gb.systems = {systemNamed("gb")};
    gb.text = "pocket";
    CHECK(source.run(gb).value() == std::vector<GameId>{id(20), id(30)});
    CatalogQuery bySystem;
    bySystem.sort = SortOrder::System;
    CHECK(source.run(bySystem).value() == std::vector<GameId>{id(20), id(30), id(50), id(40)});

    // Rows come in the order of the ids asked for; an id removed since the list was made has no row.
    const auto page = pageOf(list, 0, 3);
    auto rows = source.fetch(page).value();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].id == id(40));
    CHECK(rows[0].title == "Alpha Quest");
    CHECK(rows[2].id == id(30));
    source.remove(id(20));
    rows = source.fetch(page).value();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == id(40));
    CHECK(rows[1].id == id(30));
    // The list is materialized again after the change, and a selection restored by GameId.
    const std::vector<GameId> after = source.run(CatalogQuery{}).value();
    CHECK(positionOf(after, id(30)) == 1u);
}

TEST_CASE("catalog/domain: the other ports can be implemented as documented", "[catalog][domain]") {
    // A library source that honours cancellation and takes back the disc groups of the last import.
    class FixedSource final : public ILibrarySource {
    public:
        rv::Expected<LibrarySnapshot> load(const LibraryProfile& profile, const LibraryLoadOptions& options) override {
            if (options.cancel != nullptr && options.cancel->load()) {
                return rv::makeError(rv::ErrorCode::Cancelled, "the load was cancelled");
            }
            LibrarySnapshot snapshot;
            snapshot.profile = profile;
            // Every head here is unchanged: its cached group stands without the head being read.
            for (const CachedDiscGroup& cached : options.discGroups) {
                snapshot.discGroups.push_back(cached.group);
            }
            return snapshot;
        }
    };
    FixedSource source;
    LibraryProfile profile;
    profile.esdeHome = std::filesystem::path(L"C:\\Users\\player\\ES-DE");
    CHECK(profile.id.str() == "esde");
    CHECK(profile.adapter == LibraryAdapter::Esde);
    CHECK(source.load(profile, {}).value().profile == profile);
    std::atomic<bool> cancel{true};
    LibraryLoadOptions cancelled;
    cancelled.cancel = &cancel;
    const auto stopped = source.load(profile, cancelled);
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error().code == rv::ErrorCode::Cancelled);
    const DiscGroup twin{systemNamed("psx"),
                         key("Twin Tracks (Europe).cue"),
                         DiscGroupKind::Cue,
                         {key("Twin Tracks (Europe) (Track 1).bin"), key("Twin Tracks (Europe) (Track 2).bin")}};
    const std::vector<CachedDiscGroup> cache{CachedDiscGroup{twin, 172, 126'256'467'060'000'000}};
    LibraryLoadOptions again;
    again.discGroups = cache;
    CHECK(source.load(profile, again).value().discGroups == std::vector<DiscGroup>{twin});

    // A media resolver deriving ES-DE's paths: <media>/<system>/<kind>/<mediaStem()><ext>.
    class LayoutResolver final : public IMediaResolver {
    public:
        rv::Expected<EntryMedia> presence(const SystemName&, const LookupKey&, bool) override { return EntryMedia{}; }
        rv::Expected<std::filesystem::path> path(const SystemName& system, const LookupKey& key, bool directory,
                                                 MediaKind kind, const MediaPresence& media) const override {
            if (!media.present) {
                return rv::makeError(rv::ErrorCode::NotFound, "no such media", key.relativePath());
            }
            return std::filesystem::path(L"M:\\media") / rv::core::pathFromUtf8(system.str()) /
                   rv::core::pathFromUtf8(toString(kind)) /
                   rv::core::pathFromUtf8(mediaStem(key.relativePath(), directory) + media.extension);
        }
    };
    LayoutResolver resolver;
    const auto cover = resolver.path(systemNamed("gb"), key("Collection A/Pocket Puzzle (World).gb"), false,
                                     MediaKind::Cover, MediaPresence{true, ".png"});
    REQUIRE(cover.has_value());
    CHECK(rv::core::pathToGenericUtf8(*cover) == "M:/media/gb/covers/Collection A/Pocket Puzzle (World).png");
    // A Folder entry's media keep its whole name, dots included.
    const auto folder =
        resolver.path(systemNamed("snes"), key("Sports Vol. 2"), true, MediaKind::Cover, MediaPresence{true, ".png"});
    REQUIRE(folder.has_value());
    CHECK(rv::core::pathToGenericUtf8(*folder) == "M:/media/snes/covers/Sports Vol. 2.png");
    CHECK_FALSE(resolver.path(systemNamed("gb"), key("a.gb"), false, MediaKind::Cover, MediaPresence{}).has_value());

    // A store as catalog_store.hpp describes it: one import at a time; its own reads see the open transaction's
    // writes; the transaction applies them for everyone on commit() only and leaves nothing when dropped.
    class MemoryStore final : public ICatalogStore {
        struct Tables {
            std::map<GameId, StoredIdentity> identities;
            std::map<std::string, LibraryProfile> libraries;
            std::map<GameId, IndexedEntry> entries;
        };

    public:
        class Transaction final : public IImportTransaction {
        public:
            explicit Transaction(MemoryStore& store) : m_store(store) { m_store.m_open = m_store.m_committed; }
            ~Transaction() override {
                if (m_active) {
                    m_store.m_open.reset(); // rolled back
                }
            }
            rv::Status putLibrary(const LibraryProfile& library) override {
                if (!m_active) {
                    return ended();
                }
                m_store.m_open->libraries[library.id.str()] = library;
                return rv::ok();
            }
            rv::Status putIdentity(const StoredIdentity& identity) override {
                if (!m_active) {
                    return ended();
                }
                m_store.m_open->identities[identity.id] = identity;
                return rv::ok();
            }
            rv::Status putSystem(const LibraryId&, const SystemInfo&) override { return m_active ? rv::ok() : ended(); }
            rv::Status putEntry(const IndexedEntry& entry) override {
                if (!m_active) {
                    return ended();
                }
                if (!m_store.m_open->identities.contains(entry.id) || entry.entry.key.empty()) {
                    return rv::makeError(rv::ErrorCode::InvalidArgument, "no identity, or no key", entry.id.toString());
                }
                m_store.m_open->entries[entry.id] = entry;
                return rv::ok();
            }
            rv::Status removeEntry(GameId gameId) override {
                if (!m_active) {
                    return ended();
                }
                m_store.m_open->entries.erase(gameId);
                return rv::ok();
            }
            // What the import of I2 step 11a adds, kept minimal here: no search index to suspend, no disc groups,
            // issues or records kept.
            rv::Expected<bool> prepareWrites(std::size_t) override {
                return m_active ? rv::Expected<bool>(false) : rv::Expected<bool>(ended());
            }
            rv::Status clearIndex(const LibraryId&) override {
                if (!m_active) {
                    return ended();
                }
                m_store.m_open->entries.clear();
                return rv::ok();
            }
            rv::Status removeSystem(const LibraryId&, const SystemName&) override {
                return m_active ? rv::ok() : ended();
            }
            rv::Status putDiscGroup(const IndexedDiscGroup&) override { return m_active ? rv::ok() : ended(); }
            rv::Status removeDiscGroup(GameId) override { return m_active ? rv::ok() : ended(); }
            rv::Status putImportIssues(const LibraryId&, std::span<const Issue>, std::span<const SystemName>) override {
                return m_active ? rv::ok() : ended();
            }
            rv::Status recordImport(const ImportRecord&) override { return m_active ? rv::ok() : ended(); }
            rv::Status commit() override {
                if (!m_active) {
                    return ended();
                }
                m_store.m_committed = std::move(*m_store.m_open);
                m_store.m_open.reset();
                m_active = false;
                return rv::ok();
            }

        private:
            static rv::Status ended() {
                return rv::makeError(rv::ErrorCode::InvalidArgument, "the import transaction has ended");
            }

            MemoryStore& m_store;
            bool m_active = true;
        };

        rv::Expected<std::vector<LibraryProfile>> libraries() override {
            std::vector<LibraryProfile> result;
            for (const auto& [libraryId, library] : tables().libraries) {
                result.push_back(library);
            }
            return result;
        }
        rv::Expected<std::vector<StoredIdentity>> identities(const LibraryId& library,
                                                             const SystemName& systemName) override {
            std::vector<StoredIdentity> result;
            for (const auto& [gameId, identity] : tables().identities) {
                if (identity.library == library && identity.system == systemName) {
                    result.push_back(identity);
                }
            }
            std::sort(result.begin(), result.end(),
                      [](const StoredIdentity& a, const StoredIdentity& b) { return a.key < b.key; });
            return result;
        }
        rv::Expected<std::optional<StoredIdentity>> identity(GameId gameId) override {
            const auto found = tables().identities.find(gameId);
            return found == tables().identities.end() ? std::nullopt : std::optional<StoredIdentity>(found->second);
        }
        rv::Expected<std::vector<GameId>> takenGameIds() override {
            std::vector<GameId> taken;
            for (const auto& [gameId, identity] : tables().identities) {
                taken.push_back(gameId);
            }
            return taken;
        }
        rv::Expected<std::vector<IndexedEntry>> indexedEntries(const LibraryId&,
                                                               const SystemName& systemName) override {
            std::vector<IndexedEntry> result;
            for (const auto& [gameId, entry] : tables().entries) {
                if (entry.entry.system == systemName) {
                    result.push_back(entry);
                }
            }
            std::sort(result.begin(), result.end(),
                      [](const IndexedEntry& a, const IndexedEntry& b) { return entryOrder(a.entry, b.entry); });
            return result;
        }
        rv::Expected<std::vector<SystemInfo>> systems(const LibraryId&) override { return std::vector<SystemInfo>{}; }
        rv::Expected<std::vector<SystemName>> identitySystems(const LibraryId& library) override {
            std::vector<SystemName> names;
            for (const auto& [gameId, identity] : tables().identities) {
                if (identity.library == library &&
                    std::find(names.begin(), names.end(), identity.system) == names.end()) {
                    names.push_back(identity.system);
                }
            }
            std::sort(names.begin(), names.end());
            return names;
        }
        rv::Expected<std::vector<IndexedDiscGroup>> discGroups(const LibraryId&) override {
            return std::vector<IndexedDiscGroup>{};
        }
        rv::Expected<std::optional<ImportRecord>> lastImport(const LibraryId&) override {
            return std::optional<ImportRecord>{};
        }
        rv::Expected<std::unique_ptr<IImportLock>> lockImport(const LibraryId&) override {
            if (m_open) {
                return rv::makeError(rv::ErrorCode::Conflict, "another import holds the import lock");
            }
            return std::make_unique<IImportLock>();
        }
        rv::Expected<std::unique_ptr<IImportTransaction>> beginImport(const LibraryId&) override {
            if (m_open) {
                return rv::makeError(rv::ErrorCode::Conflict, "another import holds the import lock");
            }
            return std::unique_ptr<IImportTransaction>(std::make_unique<Transaction>(*this));
        }

    private:
        /// What this instance's reads see: its open transaction's tables, else the committed ones.
        const Tables& tables() const { return m_open ? *m_open : m_committed; }

        Tables m_committed;
        std::optional<Tables> m_open;
    };

    // The import's first steps through the port: it begins its transaction (the import lock) before it reads; new keys
    // get their ids; identities and entries are written; the store's reads see them at once, a dropped transaction
    // leaves nothing, and a committed one leaves them for everyone.
    MemoryStore store;
    const CatalogEntry tiny = entry("gb", "Tiny Tennis (Europe).gb", "Tiny Tennis");
    const auto assignTiny = [&]() {
        const std::vector<GameId> taken = store.takenGameIds().value();
        return assignGameIds(
                   profile.id, std::vector<GameIdRequest>{{tiny.system, tiny.key}},
                   [&](GameId candidate) { return std::binary_search(taken.begin(), taken.end(), candidate); })
            .value();
    };
    GameId tinyId;
    {
        auto transaction = store.beginImport(profile.id).value();
        const auto second = store.beginImport(profile.id);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == rv::ErrorCode::Conflict); // one import at a time
        const auto assigned = assignTiny();
        REQUIRE(assigned.size() == 1);
        tinyId = assigned[0].id;
        REQUIRE(transaction->putLibrary(profile).has_value());
        const auto orphan = transaction->putEntry(indexed(tinyId.value(), tiny));
        CHECK_FALSE(orphan.has_value()); // its identity comes first
        StoredIdentity identity{tinyId, profile.id, tiny.system, tiny.key, currentLookupKeyVersion(), true, tiny.file};
        REQUIRE(transaction->putIdentity(identity).has_value());
        REQUIRE(transaction->putEntry(indexed(tinyId.value(), tiny)).has_value());
        // The store's own reads see the open transaction's writes.
        CHECK(store.identities(profile.id, systemNamed("gb")).value().size() == 1);
        CHECK(store.takenGameIds().value() == std::vector<GameId>{tinyId});
        // An entry outside its system folder has no key: never indexed.
        CatalogEntry outside = tiny;
        outside.state = EntryState::OutsideSystemFolder;
        outside.key = LookupKey();
        outside.gamelistPath = "./../snes/Alpha Quest (USA).sfc";
        CHECK_FALSE(transaction->putEntry(indexed(tinyId.value(), outside)).has_value());
    }
    CHECK(store.identities(profile.id, systemNamed("gb")).value().empty()); // dropped without commit(): rolled back
    CHECK(store.takenGameIds().value().empty());
    {
        auto transaction = store.beginImport(profile.id).value(); // the lock was released
        const auto assigned = assignTiny();
        REQUIRE(assigned.size() == 1);
        CHECK(assigned[0].id == tinyId); // the same tuple, the same id
        StoredIdentity identity{tinyId, profile.id, tiny.system, tiny.key, currentLookupKeyVersion(), true, tiny.file};
        REQUIRE(transaction->putIdentity(identity).has_value());
        REQUIRE(transaction->putEntry(indexed(tinyId.value(), tiny)).has_value());
        REQUIRE(transaction->commit().has_value());
        CHECK_FALSE(transaction->removeEntry(tinyId).has_value()); // the transaction has ended
    }
    const auto identities = store.identities(profile.id, systemNamed("gb")).value();
    REQUIRE(identities.size() == 1);
    CHECK(identities[0].key == tiny.key);
    CHECK(store.identity(tinyId).value()->system == systemNamed("gb"));
    CHECK(store.indexedEntries(profile.id, systemNamed("gb")).value().at(0).entry == tiny);
    CHECK(store.takenGameIds().value() == std::vector<GameId>{tinyId});
}
