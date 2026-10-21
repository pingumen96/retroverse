// Reconciliation (I2 step 11b, rv/catalog/reconciliation.hpp) on the SQLite store, with snapshots scripted by the test
// and the quick hashes of a scripted file reader (what each file holds, which files cannot be read, which were read):
// SC-07 (a file renamed, moved to another folder, moved with its old gamelist entry kept); the quick hash read only for
// new files of a stored hash's size; size, time and name without a stored hash, refused when not unique; ties by name,
// then by the longest common path suffix; identical copies paired in key order; ambiguous cases as review rows with
// nothing merged; reviews resolved by pairing and by keeping separate, then a re-import that changes nothing; a file
// moved back (the holder becomes an alias, every state row naming it follows, a table unique per game keeping the moved
// identity's row), and a file back at its own path, or at the path of an identity that had another file, never given to
// a vanished identity; an equal quick hash confirmed by the file name or the time, else reviewed; a resolution only
// with a candidate unless forced; the history of moves, aliases and re-keyed identities; the quick-hash pass resumed
// after a cancel, bounded, deterministic; the pass, the resolution and an import never overlapping.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/fingerprint.hpp>
#include <rv/catalog/identity.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/sha256.hpp>

#include "import_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using rv::test::changesOf;
using rv::test::ImportCatalog;
using rv::test::lookupKey;
using rv::test::systemName;

namespace {

/// A file of the scripted library: its path below gb's folder, size, last write time, what it holds (a tag whose
/// SHA-256 stands for its quick hash) and whether its gamelist names it.
struct File {
    std::string path;
    std::uint64_t size = 1000;
    std::int64_t time = 126'000'000'000'000'000;
    std::string content;
    bool gamelist = false;
};

/// The quick hash a file holding `content` has (its digest stands for the file's first and last 64 KiB).
QuickHash hashOf(const File& file) {
    return QuickHash{rv::core::sha256("content:" + file.content), file.size, file.time};
}

/// What the files of the library hold now, and what reading them did: the quick-hash reader of the passes.
class ScriptedFiles {
public:
    std::map<std::string, File> files;
    std::set<std::string> denied;
    std::vector<std::string> reads;
    /// Called before each read (to cancel, to try an import in the middle).
    std::function<void(std::string_view)> onRead;

    rv::Expected<QuickHash> read(std::string_view relativePath) {
        reads.emplace_back(relativePath);
        if (onRead) {
            onRead(relativePath);
        }
        if (denied.contains(std::string(relativePath))) {
            return rv::makeError(rv::ErrorCode::IoError, "cannot open the file: access denied",
                                 std::string(relativePath));
        }
        const auto found = files.find(std::string(relativePath));
        if (found == files.end()) {
            return rv::makeError(rv::ErrorCode::NotFound, "cannot open the file: no such file",
                                 std::string(relativePath));
        }
        return hashOf(found->second);
    }

    [[nodiscard]] QuickHashReader reader() {
        return [this](const std::filesystem::path&, std::string_view relativePath) { return read(relativePath); };
    }
};

/// Four files that stay where they are: the mass-disappearance guard never holds back what a test moves.
std::vector<File> fillers() {
    std::vector<File> files;
    for (int i = 1; i <= 4; ++i) {
        files.push_back(File{fmt::format("Filler {}.gb", i), 50'000u + static_cast<std::uint64_t>(i),
                             126'000'000'000'000'000 + i, fmt::format("filler {}", i)});
    }
    return files;
}

/// A scripted gb library on a catalog, imported with the pairing pass of step 11b.
class Library {
public:
    explicit Library(std::string_view prefix)
        : m_dir(prefix), catalog(ImportCatalog::open(m_dir.file("data/retroverse.sqlite"))),
          profile(rv::test::scriptedLibrary()), pairing(files.reader()) {
        source.snapshot.systems = {rv::test::scriptedSystem("gb")};
    }

    /// The snapshot holds `present` on disk (and the fillers), and the gamelist entries of `missing` whose files are
    /// gone.
    void put(std::vector<File> present, const std::vector<std::string>& missing = {}) {
        files.files.clear();
        source.snapshot.entries.clear();
        for (const File& filler : fillers()) {
            present.push_back(filler);
        }
        for (const File& file : present) {
            files.files[file.path] = file;
            CatalogEntry entry = rv::test::diskGame("gb", file.path, file.size);
            entry.file->lastWriteTime = file.time;
            if (file.gamelist) {
                entry.gamelistPath = "./" + file.path;
            }
            source.snapshot.entries.push_back(std::move(entry));
        }
        for (const std::string& path : missing) {
            source.snapshot.entries.push_back(rv::test::missingGame("gb", path, "Missing " + path));
        }
        rv::test::sortEntries(source.snapshot);
    }

    ImportResult import(const ImportOptions& options = {}) {
        files.reads.clear();
        return rv::test::importOk(catalog, source, media, profile, options, &pairing);
    }

    QuickHashPassResult hashPass(QuickHashPassOptions options = {}) {
        files.reads.clear();
        options.reader = files.reader();
        auto result = runQuickHashPass(*catalog.store, profile.id, options);
        INFO((result ? std::string("hashed") : result.error().toString()));
        REQUIRE(result.has_value());
        return std::move(*result);
    }

    /// The identity at the exact relative path, REQUIREd.
    StoredIdentity at(std::string_view path) {
        const auto identities = catalog.store->identities(profile.id, systemName("gb"));
        REQUIRE(identities.has_value());
        const auto found = std::find_if(identities->begin(), identities->end(), [&](const StoredIdentity& identity) {
            return identity.key.relativePath() == path;
        });
        INFO(path);
        REQUIRE(found != identities->end());
        return *found;
    }
    [[nodiscard]] bool has(std::string_view path) {
        const auto identities = catalog.store->identities(profile.id, systemName("gb")).value();
        return std::any_of(identities.begin(), identities.end(),
                           [&](const StoredIdentity& identity) { return identity.key.relativePath() == path; });
    }
    /// The index row of a GameId, if any.
    std::optional<IndexedEntry> row(GameId id) {
        // A vector of its own: a loop over the value of a temporary Expected would outlive it.
        const std::vector<IndexedEntry> rows = catalog.store->indexedEntries(profile.id, systemName("gb")).value();
        for (const IndexedEntry& entry : rows) {
            if (entry.id == id) {
                return entry;
            }
        }
        return std::nullopt;
    }
    /// The index row of a GameId, REQUIREd.
    IndexedEntry rowOf(GameId id) {
        std::optional<IndexedEntry> found = row(id);
        INFO(id.toString());
        REQUIRE(found.has_value());
        return std::move(*found);
    }
    std::vector<IdentityEvent> history(GameId id) { return catalog.store->history(id).value(); }
    std::vector<StoredReview> reviews(bool openOnly = true) {
        return catalog.store->reviews(profile.id, openOnly).value();
    }
    rv::catalog_db::Connection& writer() { return catalog.database->writer(); }
    std::int64_t count(const std::string& sql) { return writer().queryInt(sql).value().value_or(-1); }

    /// A second import changes nothing, and reads no file.
    void checkSteady() {
        const ImportResult steady = import();
        CHECK(steady.statistics.identitiesWritten == 0);
        CHECK(steady.statistics.entriesWritten == 0);
        CHECK(steady.statistics.entriesRemoved == 0);
        CHECK(steady.statistics.aliasesWritten == 0);
        CHECK(steady.statistics.historyWritten == 0);
        CHECK(steady.statistics.reviewsWritten == 0);
        CHECK(steady.changes.moved.empty());
        CHECK(steady.changes.reviews.empty());
        CHECK(files.reads.empty());
    }

private:
    rv::test::TempDir m_dir;

public:
    ScriptedFiles files;
    ImportCatalog catalog;
    LibraryProfile profile;
    rv::test::ScriptedSource source;
    rv::test::ScriptedMedia media;
    IdentityPairing pairing;
};

constexpr std::int64_t kTime = 126'000'000'000'000'000;

GameId firstId(std::string_view relativePath, std::uint32_t counter = 0) {
    return stableGameId(LibraryId(), systemName("gb"), lookupKey(relativePath).key(), counter).value();
}

} // namespace

TEST_CASE("catalog_db/reconciliation: a renamed file keeps its identity by its quick hash; only new files of a hashed "
          "size are read",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-hash");
    library.put({File{"Old Name (USA).gb", 100, kTime + 1, "game"}, File{"Other (USA).gb", 200, kTime + 2, "other"}});
    library.import();
    const GameId game = library.at("Old Name (USA).gb").id;
    // No ROM byte read by an import that pairs nothing; the pass hashes every present file, the fillers included.
    CHECK(library.files.reads.empty());
    const QuickHashPassResult pass = library.hashPass();
    CHECK(pass.candidates == 6);
    CHECK(pass.hashed == 6);
    CHECK(library.at("Old Name (USA).gb").quickHash == hashOf(File{"", 100, kTime + 1, "game"}));

    // Renamed, its time kept as a rename keeps it (no size, time and name could pair it: the name changed); a new file
    // of another size beside it.
    library.put({File{"New Name (USA).gb", 100, kTime + 1, "game"}, File{"Other (USA).gb", 200, kTime + 2, "other"},
                 File{"Big (USA).gb", 300, kTime + 3, "big"}});
    const ImportResult moved = library.import();
    // Only the new file of the stored hash's size was read.
    CHECK(library.files.reads == std::vector<std::string>{"New Name (USA).gb"});
    REQUIRE(moved.changes.moved.size() == 1);
    CHECK(moved.changes.moved[0].id == game);
    CHECK(moved.changes.moved[0].reason == MoveReason::QuickHash);
    CHECK(moved.changes.moved[0].from.relativePath() == "Old Name (USA).gb");
    CHECK(moved.changes.moved[0].to.relativePath() == "New Name (USA).gb");
    CHECK_FALSE(moved.changes.moved[0].alias.has_value());
    CHECK(changesOf(moved.changes, "gb").moved == 1);
    CHECK(changesOf(moved.changes, "gb").newIdentities == 1); // the big file
    const StoredIdentity identity = library.at("New Name (USA).gb");
    CHECK(identity.id == game);
    CHECK(identity.present);
    // The hash the pairing read is the moved identity's now, current for its new file.
    CHECK(identity.quickHash == hashOf(File{"", 100, kTime + 1, "game"}));
    CHECK(currentQuickHash(identity).has_value());
    CHECK_FALSE(library.has("Old Name (USA).gb"));
    CHECK(library.rowOf(game).entry.key.relativePath() == "New Name (USA).gb");
    // The move in the history, with its reason.
    const std::vector<IdentityEvent> history = library.history(game);
    REQUIRE(history.size() == 1);
    CHECK(history[0].kind == IdentityEventKind::Moved);
    CHECK(history[0].reason == MoveReason::QuickHash);
    CHECK(history[0].fromPath == "Old Name (USA).gb");
    CHECK(history[0].toPath == "New Name (USA).gb");
    CHECK(history[0].toKey == lookupKey("New Name (USA).gb").key());
    CHECK_FALSE(history[0].at.empty());
    CHECK(moved.statistics.historyWritten == 1);
    CHECK(library.pairing.statistics().filesHashed == 1);
    library.checkSteady();
}

TEST_CASE("catalog_db/reconciliation: SC-07, a file renamed, moved to another folder, moved with its gamelist entry "
          "left behind",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-sc07");
    // A state table of a later step (play sessions, say), naming identities.
    REQUIRE(library.writer()
                .execute("CREATE TABLE rv_test_sessions (id INTEGER PRIMARY KEY, game INTEGER NOT NULL REFERENCES "
                         "rv_game_identities (id), note TEXT NOT NULL) STRICT")
                .has_value());
    library.put({File{"Renamed (USA).gb", 1001, kTime + 1, "renamed"}, File{"Moved (USA).gb", 1002, kTime + 2, "moved"},
                 File{"Listed (USA).gb", 1003, kTime + 3, "listed", true}});
    library.import();
    library.hashPass();
    const GameId renamed = library.at("Renamed (USA).gb").id;
    const GameId movedId = library.at("Moved (USA).gb").id;
    const GameId listed = library.at("Listed (USA).gb").id;
    for (const GameId id : {renamed, movedId, listed}) {
        REQUIRE(library.writer()
                    .execute(fmt::format("INSERT INTO rv_test_sessions (game, note) VALUES ({}, 'played')", id.value()))
                    .has_value());
    }

    // Renamed in place, moved to a sub-folder, and moved with ES-DE's gamelist entry left at the old path (a missing
    // file there now): their times kept, as a move keeps them.
    library.put({File{"Renamed (USA) (Rev 1).gb", 1001, kTime + 1, "renamed"},
                 File{"Sub/Moved (USA).gb", 1002, kTime + 2, "moved"},
                 File{"Sub/Listed (USA).gb", 1003, kTime + 3, "listed"}},
                {"Listed (USA).gb"});
    const ImportResult result = library.import();
    CHECK(result.changes.moved.size() == 3);
    CHECK(changesOf(result.changes, "gb").moved == 3);
    CHECK(changesOf(result.changes, "gb").gone == 0);
    CHECK(changesOf(result.changes, "gb").outcome == SystemOutcome::Applied);
    for (const MovedGame& move : result.changes.moved) {
        CHECK(move.reason == MoveReason::QuickHash);
    }
    // Each kept its GameId, its history and its state rows.
    CHECK(library.at("Renamed (USA) (Rev 1).gb").id == renamed);
    CHECK(library.at("Sub/Moved (USA).gb").id == movedId);
    CHECK(library.at("Sub/Listed (USA).gb").id == listed);
    for (const GameId id : {renamed, movedId, listed}) {
        CHECK(library.history(id).size() == 1);
        CHECK(library.count(fmt::format("SELECT count(*) FROM rv_test_sessions WHERE game = {}", id.value())) == 1);
    }
    // The gamelist entry left at the old path has a new GameId (the counter: its own value is the moved identity's),
    // its file not present.
    const StoredIdentity left = library.at("Listed (USA).gb");
    CHECK(left.id == firstId("Listed (USA).gb", 1));
    CHECK_FALSE(left.present);
    CHECK(library.rowOf(left.id).entry.state == EntryState::MissingFile);
    CHECK(library.rowOf(listed).entry.key.relativePath() == "Sub/Listed (USA).gb");
    library.checkSteady();
}

TEST_CASE("catalog_db/reconciliation: without a stored hash a unique new file of equal size, time and name pairs; "
          "anything else is reviewed, never merged",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-stn");
    library.put({File{"A/Game (USA).gb", 700, kTime + 7, "game"}, File{"A/Twin (USA).gb", 800, kTime + 8, "twin 1"},
                 File{"B/Twin (USA).gb", 800, kTime + 8, "twin 2"}});
    library.import(); // no pass: no quick hash stored
    const GameId game = library.at("A/Game (USA).gb").id;

    SECTION("a unique file of equal size, time and name") {
        library.put({File{"C/Game (USA).gb", 700, kTime + 7, "game"}, File{"A/Twin (USA).gb", 800, kTime + 8, "twin 1"},
                     File{"B/Twin (USA).gb", 800, kTime + 8, "twin 2"}});
        const ImportResult result = library.import();
        REQUIRE(result.changes.moved.size() == 1);
        CHECK(result.changes.moved[0].reason == MoveReason::SizeTimeName);
        CHECK(library.at("C/Game (USA).gb").id == game);
        CHECK(library.files.reads.empty()); // no stored hash: nothing to read
        CHECK(library.history(game).at(0).reason == MoveReason::SizeTimeName);
    }
    SECTION("two files of equal size, time and name: a review row, nothing merged") {
        library.put({File{"C/Game (USA).gb", 700, kTime + 7, "game"}, File{"D/Game (USA).gb", 700, kTime + 7, "game"},
                     File{"A/Twin (USA).gb", 800, kTime + 8, "twin 1"},
                     File{"B/Twin (USA).gb", 800, kTime + 8, "twin 2"}});
        const ImportResult result = library.import();
        CHECK(result.changes.moved.empty());
        REQUIRE(result.changes.reviews.size() == 1);
        const ReviewRow& review = result.changes.reviews[0];
        CHECK(review.id > 0);
        CHECK(review.game == game);
        CHECK(review.system == systemName("gb"));
        CHECK(review.vanished.relativePath() == "A/Game (USA).gb");
        REQUIRE(review.candidates.size() == 2);
        CHECK(review.candidates[0].relativePath() == "C/Game (USA).gb");
        CHECK(review.candidates[1].relativePath() == "D/Game (USA).gb");
        CHECK(review.reason.find("2 new files have its size, time and name") != std::string::npos);
        // Nothing merged: the identity vanished, the two files have identities of their own.
        CHECK_FALSE(library.at("A/Game (USA).gb").present);
        CHECK(library.at("C/Game (USA).gb").id != game);
        CHECK(library.at("D/Game (USA).gb").id != game);
        // The row is stored as the change set says.
        const std::vector<StoredReview> stored = library.reviews();
        REQUIRE(stored.size() == 1);
        CHECK(stored[0].review == review);
        CHECK(stored[0].open());
        CHECK_FALSE(stored[0].createdAt.empty());
        CHECK(result.statistics.reviewsWritten == 1);
        // The next import neither repeats it nor pairs it (the identity is no longer present).
        library.checkSteady();
        CHECK(library.reviews().size() == 1);
    }
    SECTION("a file two vanished identities could be: both reviewed") {
        library.put({File{"A/Game (USA).gb", 700, kTime + 7, "game"}, File{"C/Twin (USA).gb", 800, kTime + 8, "twin"}});
        const ImportResult result = library.import();
        CHECK(result.changes.moved.empty());
        REQUIRE(result.changes.reviews.size() == 2);
        for (const ReviewRow& review : result.changes.reviews) {
            REQUIRE(review.candidates.size() == 1);
            CHECK(review.candidates[0].relativePath() == "C/Twin (USA).gb");
            CHECK(review.reason.find("another vanished identity's candidate") != std::string::npos);
        }
    }
    SECTION("its name, another time: reviewed") {
        library.put({File{"C/Game (USA).gb", 700, kTime + 70, "game"},
                     File{"A/Twin (USA).gb", 800, kTime + 8, "twin 1"},
                     File{"B/Twin (USA).gb", 800, kTime + 8, "twin 2"}});
        const ImportResult result = library.import();
        CHECK(result.changes.moved.empty());
        REQUIRE(result.changes.reviews.size() == 1);
        CHECK(result.changes.reviews[0].reason.find("its file name but not its size or time") != std::string::npos);
    }
    SECTION("more candidates than a row lists: the first in key order, the reason says how many") {
        std::vector<File> present{File{"A/Twin (USA).gb", 800, kTime + 8, "twin 1"},
                                  File{"B/Twin (USA).gb", 800, kTime + 8, "twin 2"}};
        for (std::size_t i = 0; i < kMaxReviewCandidates + 2; ++i) {
            present.push_back(File{fmt::format("Copy {:03}/Game (USA).gb", i), 7000 + i, kTime, "another dump"});
        }
        library.put(present);
        const ImportResult result = library.import();
        REQUIRE(result.changes.reviews.size() == 1);
        const ReviewRow& review = result.changes.reviews[0];
        REQUIRE(review.candidates.size() == kMaxReviewCandidates);
        CHECK(review.candidates.front().relativePath() == "Copy 000/Game (USA).gb");
        CHECK(review.reason.find(fmt::format("(the first {} of {} candidates listed)", kMaxReviewCandidates,
                                             kMaxReviewCandidates + 2)) != std::string::npos);
        CHECK(library.reviews().at(0).review.candidates.size() == kMaxReviewCandidates);
    }
    SECTION("nothing it could be: vanished, no review") {
        library.put({File{"A/Twin (USA).gb", 800, kTime + 8, "twin 1"},
                     File{"B/Twin (USA).gb", 800, kTime + 8, "twin 2"},
                     File{"Unrelated (USA).gb", 700, kTime + 7, "other"}});
        const ImportResult result = library.import();
        CHECK(result.changes.moved.empty());
        CHECK(result.changes.reviews.empty());
        CHECK(changesOf(result.changes, "gb").vanished == 1);
    }
}

TEST_CASE("catalog_db/reconciliation: ties go to the equal file name, then to the longest common path suffix",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-ties");
    library.put(
        {File{"A/Sub/Game (USA).gb", 900, kTime + 9, "game"}, File{"P/Quest (USA).gb", 901, kTime + 10, "quest"}});
    library.import();
    library.hashPass();
    const GameId game = library.at("A/Sub/Game (USA).gb").id;
    const GameId quest = library.at("P/Quest (USA).gb").id;
    // Copies of each, every one with the hash: the paths alone tell them apart.
    library.put({File{"B/Sub/Game (USA).gb", 900, kTime + 1, "game"},
                 File{"C/Other/Game (USA).gb", 900, kTime + 2, "game"},
                 File{"D/Renamed (USA).gb", 900, kTime + 3, "game"}, File{"R/Quest (USA).gb", 901, kTime + 4, "quest"},
                 File{"S/Elsewhere (USA).gb", 901, kTime + 5, "quest"}});
    const ImportResult result = library.import();
    REQUIRE(result.changes.moved.size() == 2);
    // The longest common path suffix: "Sub/Game (USA).gb" over "Game (USA).gb" over nothing.
    CHECK(library.at("B/Sub/Game (USA).gb").id == game);
    // The equal file name.
    CHECK(library.at("R/Quest (USA).gb").id == quest);
    for (const MovedGame& move : result.changes.moved) {
        CHECK(move.reason == MoveReason::QuickHash);
    }
    CHECK(result.changes.reviews.empty());
    library.checkSteady();
}

TEST_CASE("catalog_db/reconciliation: identical copies moved together pair in key order; uneven copies are reviewed",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-copies");
    library.put({File{"Copies/First (USA).gb", 500, kTime + 5, "copy"},
                 File{"Copies/Second (USA).gb", 500, kTime + 5, "copy"}});
    library.import();
    library.hashPass();
    const GameId first = library.at("Copies/First (USA).gb").id;
    const GameId second = library.at("Copies/Second (USA).gb").id;

    SECTION("as many copies as identities, nothing to tell them apart: key order") {
        // Renamed and moved, their times kept.
        library.put(
            {File{"Moved/One (USA).gb", 500, kTime + 5, "copy"}, File{"Moved/Two (USA).gb", 500, kTime + 5, "copy"}});
        const ImportResult result = library.import();
        REQUIRE(result.changes.moved.size() == 2);
        CHECK(result.changes.moved[0].reason == MoveReason::IdenticalCopies);
        CHECK(result.changes.moved[1].reason == MoveReason::IdenticalCopies);
        CHECK(library.at("Moved/One (USA).gb").id == first);
        CHECK(library.at("Moved/Two (USA).gb").id == second);
        CHECK(library.history(first).at(0).reason == MoveReason::IdenticalCopies);
        library.checkSteady();
    }
    SECTION("as many files, but neither their names nor their times confirm the hash: reviewed, nothing merged") {
        library.put(
            {File{"Moved/One (USA).gb", 500, kTime + 6, "copy"}, File{"Moved/Two (USA).gb", 500, kTime + 6, "copy"}});
        const ImportResult result = library.import();
        CHECK(result.changes.moved.empty());
        REQUIRE(result.changes.reviews.size() == 2);
        for (const ReviewRow& review : result.changes.reviews) {
            CHECK(review.candidates.size() == 2);
            CHECK(review.reason.find("neither its file name nor its time") != std::string::npos);
        }
        CHECK(library.at("Moved/One (USA).gb").id != first);
        CHECK(library.at("Moved/Two (USA).gb").id != second);
    }
    SECTION("more copies than identities: reviewed, nothing merged") {
        library.put({File{"Moved/One (USA).gb", 500, kTime + 6, "copy"},
                     File{"Moved/Two (USA).gb", 500, kTime + 6, "copy"},
                     File{"Moved/Three (USA).gb", 500, kTime + 6, "copy"}});
        const ImportResult result = library.import();
        CHECK(result.changes.moved.empty());
        REQUIRE(result.changes.reviews.size() == 2);
        for (const ReviewRow& review : result.changes.reviews) {
            CHECK(review.candidates.size() == 3);
            CHECK(review.reason.find("3 new files have its quick hash") != std::string::npos);
        }
        CHECK_FALSE(library.at("Copies/First (USA).gb").present);
        CHECK_FALSE(library.at("Copies/Second (USA).gb").present);
    }
    SECTION("a copy that cannot be read: reviewed, not paired by its hash") {
        library.put(
            {File{"Moved/One (USA).gb", 500, kTime + 6, "copy"}, File{"Moved/Two (USA).gb", 500, kTime + 6, "copy"}});
        library.files.denied = {"Moved/Two (USA).gb"};
        const ImportResult result = library.import();
        // One copy readable for two identities: neither is safe; the unreadable one is an issue and a candidate.
        CHECK(result.changes.moved.empty());
        REQUIRE(result.changes.reviews.size() == 2);
        CHECK(std::any_of(result.changes.issues.begin(), result.changes.issues.end(), [](const Issue& issue) {
            return issue.code == kQuickHashIssueUnreadable && issue.system == "gb";
        }));
        CHECK(library.pairing.statistics().filesUnreadable == 1);
    }
}

TEST_CASE("catalog_db/reconciliation: a review resolved by pairing applies the move; kept separate changes nothing; "
          "the next import changes nothing either",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-resolve");
    library.put(
        {File{"Kept (USA).gb", 600, kTime + 6, "kept", true}, File{"Other/Game (USA).gb", 610, kTime + 1, "g"}});
    library.import();
    const GameId vanished = library.at("Kept (USA).gb").id;
    // Two copies of equal size, time and name elsewhere; the gamelist entry stays at the old path.
    library.put({File{"B/Kept (USA).gb", 600, kTime + 6, "kept"}, File{"C/Kept (USA).gb", 600, kTime + 6, "kept"},
                 File{"Other/Game (USA).gb", 610, kTime + 1, "g"}},
                {"Kept (USA).gb"});
    const ImportResult reviewed = library.import();
    REQUIRE(reviewed.changes.reviews.size() == 1);
    const std::int64_t review = reviewed.changes.reviews[0].id;
    const GameId chosen = library.at("B/Kept (USA).gb").id;
    const auto indexBefore = rv::test::dumpIndex(library.writer());

    SECTION("refusals") {
        const auto refused = [&](ReviewDecision decision, rv::ErrorCode code) {
            const auto result = resolveReview(*library.catalog.store, library.profile.id, decision);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == code);
        };
        refused({review + 100, std::nullopt}, rv::ErrorCode::NotFound);
        refused({review, GameId::fromValue(42).value()}, rv::ErrorCode::NotFound); // no such identity
        refused({review, vanished}, rv::ErrorCode::InvalidArgument);                // itself
        // A present identity of the system that is none of the review's candidates (a mistyped GameId).
        refused({review, library.at("Other/Game (USA).gb").id}, rv::ErrorCode::InvalidArgument);
        CHECK(rv::test::dumpIndex(library.writer()) == indexBefore);
        CHECK(library.reviews().size() == 1);
    }
    SECTION("paired with a candidate") {
        const auto resolved = resolveReview(*library.catalog.store, library.profile.id, {review, chosen});
        INFO((resolved ? std::string("resolved") : resolved.error().toString()));
        REQUIRE(resolved.has_value());
        CHECK(resolved->review.resolution == ReviewResolution::Paired);
        CHECK(resolved->review.resolvedWith == chosen);
        CHECK_FALSE(resolved->review.open());
        REQUIRE(resolved->moved.has_value());
        CHECK(resolved->moved->id == vanished);
        CHECK(resolved->moved->alias == chosen);
        CHECK(resolved->moved->reason == MoveReason::ReviewResolved);
        // The vanished identity has the file now; the chosen one is its alias; the gamelist entry left at the old
        // path has an identity of its own, as an import gives one.
        const StoredIdentity identity = library.at("B/Kept (USA).gb");
        CHECK(identity.id == vanished);
        CHECK(identity.present);
        CHECK_FALSE(library.catalog.store->identity(chosen).value().has_value());
        const auto alias = library.catalog.store->alias(chosen).value();
        REQUIRE(alias.has_value());
        CHECK(alias->game == vanished);
        CHECK(alias->reason == MoveReason::ReviewResolved);
        {
            // The reader's side (I4 step 5a, the app's building job): the alias's identity, none for an identity.
            auto reader = library.catalog.database->openReader();
            REQUIRE(reader.has_value());
            auto query = rv::catalog_db::SqliteCatalogQuery::open(std::move(*reader));
            REQUIRE(query.has_value());
            CHECK((*query)->aliasTarget(chosen).value() == std::optional<GameId>(vanished));
            CHECK_FALSE((*query)->aliasTarget(vanished).value().has_value());
        }
        REQUIRE(resolved->leftAtOldPath.has_value());
        CHECK(library.at("Kept (USA).gb").id == *resolved->leftAtOldPath);
        CHECK(*resolved->leftAtOldPath == firstId("Kept (USA).gb", 1));
        CHECK(library.rowOf(vanished).entry.key.relativePath() == "B/Kept (USA).gb");
        CHECK(library.rowOf(*resolved->leftAtOldPath).entry.state == EntryState::MissingFile);
        CHECK_FALSE(library.row(chosen).has_value());
        // The history: the move, the alias, the resolution.
        const std::vector<IdentityEvent> history = library.history(vanished);
        REQUIRE(history.size() == 3);
        CHECK(history[0].kind == IdentityEventKind::Moved);
        CHECK(history[0].reason == MoveReason::ReviewResolved);
        CHECK(history[0].review == review);
        CHECK(history[1].kind == IdentityEventKind::Alias);
        CHECK(history[1].alias == chosen);
        CHECK(history[2].kind == IdentityEventKind::ReviewResolved);
        CHECK(library.reviews().empty());
        // Resolved once: again is refused.
        const auto again = resolveReview(*library.catalog.store, library.profile.id, {review, std::nullopt});
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == rv::ErrorCode::Conflict);
        library.checkSteady();
        CHECK(library.at("B/Kept (USA).gb").id == vanished);
    }
    SECTION("paired with an identity that is none of the candidates: only when forced") {
        const GameId other = library.at("Other/Game (USA).gb").id;
        ReviewDecision forced{review, other};
        forced.force = true;
        const auto resolved = resolveReview(*library.catalog.store, library.profile.id, forced);
        INFO((resolved ? std::string("resolved") : resolved.error().toString()));
        REQUIRE(resolved.has_value());
        CHECK(resolved->review.resolvedWith == other);
        CHECK(library.at("Other/Game (USA).gb").id == vanished);
        CHECK(library.catalog.store->alias(other).value().has_value());
        library.checkSteady();
    }
    SECTION("kept separate") {
        const auto stateBefore = rv::test::dumpState(library.writer());
        const auto resolved = resolveReview(*library.catalog.store, library.profile.id, {review, std::nullopt});
        REQUIRE(resolved.has_value());
        CHECK(resolved->review.resolution == ReviewResolution::KeptSeparate);
        CHECK_FALSE(resolved->moved.has_value());
        CHECK(rv::test::dumpIndex(library.writer()) == indexBefore);
        auto stateAfter = rv::test::dumpState(library.writer());
        // Only the review row and the history changed.
        CHECK(stateAfter["rv_game_identities"] == stateBefore.at("rv_game_identities"));
        CHECK(stateAfter["rv_identity_aliases"] == stateBefore.at("rv_identity_aliases"));
        CHECK(library.history(vanished).back().kind == IdentityEventKind::ReviewResolved);
        CHECK(library.reviews().empty());
        CHECK(library.reviews(false).size() == 1);
        library.checkSteady();
    }
}

TEST_CASE("catalog_db/reconciliation: a resolution refuses identities of another system, not present, or while an "
          "import holds the lock",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-refusals");
    library.source.snapshot.systems.push_back(rv::test::scriptedSystem("nes"));
    library.put({File{"Kept (USA).gb", 600, kTime + 6, "kept"}, File{"Gone (USA).gb", 620, kTime + 2, "gone"}});
    library.source.snapshot.entries.push_back(rv::test::diskGame("nes", "Elsewhere (USA).nes", 600));
    rv::test::sortEntries(library.source.snapshot);
    library.import();
    const GameId nes = [&] {
        const auto identities = library.catalog.store->identities(library.profile.id, systemName("nes")).value();
        REQUIRE(identities.size() == 1);
        return identities[0].id;
    }();
    const GameId gone = library.at("Gone (USA).gb").id;
    // Kept's copies of equal size, time and name, and a file of Gone's name of another size: two review rows.
    library.put({File{"B/Kept (USA).gb", 600, kTime + 6, "kept"}, File{"C/Kept (USA).gb", 600, kTime + 6, "kept"},
                 File{"D/Gone (USA).gb", 999, kTime + 9, "not gone"}});
    library.source.snapshot.entries.push_back(rv::test::diskGame("nes", "Elsewhere (USA).nes", 600));
    rv::test::sortEntries(library.source.snapshot);
    const ImportResult reviewed = library.import();
    REQUIRE(reviewed.changes.reviews.size() == 2);
    const auto reviewOf = [&](GameId game) {
        const auto found = std::find_if(reviewed.changes.reviews.begin(), reviewed.changes.reviews.end(),
                                        [&](const ReviewRow& review) { return review.game == game; });
        REQUIRE(found != reviewed.changes.reviews.end());
        return found->id;
    };
    const std::int64_t review = reviewOf(library.at("Kept (USA).gb").id);
    const std::int64_t goneReview = reviewOf(gone);
    CHECK_FALSE(library.at("Gone (USA).gb").present);

    const auto refused = [&](ReviewDecision decision, rv::ErrorCode code) {
        const auto result = resolveReview(*library.catalog.store, library.profile.id, decision);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == code);
        return result.error().message;
    };
    refused({review, nes}, rv::ErrorCode::InvalidArgument);  // another system
    refused({review, gone}, rv::ErrorCode::InvalidArgument); // its file is not present
    // Another holder of the database imports: the resolution waits for nothing, it is refused.
    auto other = rv::catalog_db::Database::open(library.catalog.database->path());
    REQUIRE(other.has_value());
    auto otherStore = rv::catalog_db::SqliteCatalogStore::open(*other);
    REQUIRE(otherStore.has_value());
    {
        auto lock = (*otherStore)->lockImport(library.profile.id);
        REQUIRE(lock.has_value());
        CHECK(refused({review, std::nullopt}, rv::ErrorCode::Conflict).find("import lock") != std::string::npos);
    }
    // Paired with B's copy, which becomes an alias: an alias is no identity to pair another review with.
    const GameId copy = library.at("B/Kept (USA).gb").id;
    REQUIRE(resolveReview(*library.catalog.store, library.profile.id, {review, copy}).has_value());
    CHECK(refused({goneReview, copy}, rv::ErrorCode::InvalidArgument).find("is an alias of") != std::string::npos);
    CHECK(refused({review, std::nullopt}, rv::ErrorCode::Conflict).find("resolved already") != std::string::npos);
    CHECK(resolveReview(*library.catalog.store, library.profile.id, {goneReview, std::nullopt}).has_value());
    CHECK(library.reviews().empty());
}

TEST_CASE("catalog_db/reconciliation: a file moved back keeps its identity; the holder's state rows follow it, a table "
          "unique per game keeping the moved identity's row",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-back");
    // State tables of later steps, found by the store in the schema: sessions (many per game) and launch choices (one
    // per game). Made before the store's first alias, which looks for them.
    REQUIRE(library.writer()
                .execute("CREATE TABLE rv_test_sessions (id INTEGER PRIMARY KEY, game INTEGER NOT NULL REFERENCES "
                         "rv_game_identities (id), note TEXT NOT NULL) STRICT;"
                         "CREATE TABLE rv_test_choices (game INTEGER PRIMARY KEY REFERENCES rv_game_identities (id), "
                         "choice TEXT NOT NULL) STRICT")
                .has_value());
    library.put({File{"Dir1/Game (USA).gb", 777, kTime + 7, "game", true}});
    library.import();
    library.hashPass();
    const GameId game = library.at("Dir1/Game (USA).gb").id;

    // Moved, its gamelist entry left behind: the entry at Dir1 gets a new identity (the holder).
    library.put({File{"Dir2/Game (USA).gb", 777, kTime + 7, "game"}}, {"Dir1/Game (USA).gb"});
    REQUIRE(library.import().changes.moved.size() == 1);
    const GameId holder = library.at("Dir1/Game (USA).gb").id;
    CHECK(holder == firstId("Dir1/Game (USA).gb", 1));
    // State rows that name the holder, and the moved identity: sessions, a choice each, an alias that stands for the
    // holder, a history row and an open review row of the holder.
    REQUIRE(library.writer()
                .execute(fmt::format(
                    "INSERT INTO rv_test_sessions (game, note) VALUES ({0}, 'holder session'), ({1}, 'game session');"
                    "INSERT INTO rv_test_choices (game, choice) VALUES ({0}, 'holder choice'), ({1}, 'game choice');"
                    "INSERT INTO rv_identity_aliases (alias, game, lookup_key, relative_path, at, reason) VALUES "
                    "(777, {0}, 'ELDER.GB', 'Elder.gb', '2026-10-17T00:00:00.000Z', 'moved-back');"
                    "INSERT INTO rv_identity_history (game, at, event, detail) VALUES ({0}, "
                    "'2026-10-17T00:00:00.000Z', 'rekeyed', 'the holder''s own');"
                    "INSERT INTO rv_identity_reviews (game, vanished_key, vanished_path, candidates, reason, "
                    "created_at) VALUES ({0}, 'X.GB', 'x.gb', '[]', 'a test', '2026-10-17T00:00:00.000Z')",
                    holder.value(), game.value()))
                .has_value());

    // Moved back where its gamelist entry stayed: the holder becomes an alias of the moved identity.
    library.put({File{"Dir1/Game (USA).gb", 777, kTime + 7, "game", true}});
    const ImportResult back = library.import();
    REQUIRE(back.changes.moved.size() == 1);
    CHECK(back.changes.moved[0].id == game);
    CHECK(back.changes.moved[0].alias == holder);
    CHECK(back.changes.moved[0].reason == MoveReason::QuickHash);
    CHECK(library.at("Dir1/Game (USA).gb").id == game);
    CHECK_FALSE(library.catalog.store->identity(holder).value().has_value());
    const auto alias = library.catalog.store->alias(holder).value();
    REQUIRE(alias.has_value());
    CHECK(alias->game == game);
    CHECK(alias->reason == MoveReason::MovedBack);
    CHECK(alias->key.relativePath() == "Dir1/Game (USA).gb");

    // Every state row that named the holder names the moved identity now.
    const auto sessions = [&](GameId id) {
        return library.count(fmt::format("SELECT count(*) FROM rv_test_sessions WHERE game = {}", id.value()));
    };
    CHECK(sessions(game) == 2);
    CHECK(sessions(holder) == 0);
    // One choice per game: the moved identity's stays, the holder's goes.
    CHECK(library.count("SELECT count(*) FROM rv_test_choices") == 1);
    CHECK(library.writer()
              .queryText(fmt::format("SELECT choice FROM rv_test_choices WHERE game = {}", game.value()))
              .value() == std::optional<std::string>("game choice"));
    // The alias that stood for the holder stands for the moved identity (aliases never chain).
    const std::vector<StoredAlias> aliases = library.catalog.store->aliasesOf(game).value();
    REQUIRE(aliases.size() == 2);
    CHECK(aliases[0].alias == GameId::fromValue(777).value());
    CHECK(aliases[1].alias == holder);
    CHECK(library.catalog.store->aliasesOf(holder).value().empty());
    // The holder's history is the moved identity's; its open review row was resolved, then moved.
    const std::vector<IdentityEvent> history = library.history(game);
    CHECK(std::any_of(history.begin(), history.end(),
                      [](const IdentityEvent& event) { return event.detail == "the holder's own"; }));
    const std::size_t moves =
        static_cast<std::size_t>(std::count_if(history.begin(), history.end(), [](const IdentityEvent& event) {
            return event.kind == IdentityEventKind::Moved;
        }));
    CHECK(moves == 2);
    const auto aliasEvent = std::find_if(history.begin(), history.end(), [](const IdentityEvent& event) {
        return event.kind == IdentityEventKind::Alias;
    });
    REQUIRE(aliasEvent != history.end());
    CHECK(aliasEvent->alias == holder);
    CHECK(aliasEvent->reason == MoveReason::MovedBack);
    const std::vector<StoredReview> reviews = library.reviews(false);
    REQUIRE(reviews.size() == 1);
    CHECK(reviews[0].review.game == game);
    CHECK(reviews[0].resolution == ReviewResolution::Aliased);
    CHECK(reviews[0].resolvedWith == game);
    CHECK(library.count("PRAGMA foreign_key_check") == -1); // no row breaks a key
    library.checkSteady();
}

TEST_CASE("catalog_db/reconciliation: a file back at its own path keeps its identity when an identical copy vanishes "
          "meanwhile; nothing is merged",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-own-back");
    REQUIRE(library.writer()
                .execute("CREATE TABLE rv_test_sessions (id INTEGER PRIMARY KEY, game INTEGER NOT NULL REFERENCES "
                         "rv_game_identities (id), note TEXT NOT NULL) STRICT")
                .has_value());
    // Two identical copies of one name, size and time.
    const File a{"A/Tetris (USA).gb", 400, kTime + 4, "tetris"};
    const File b{"B/Tetris (USA).gb", 400, kTime + 4, "tetris"};
    library.put({a, b});
    library.import();
    SECTION("their quick hashes stored") {
        library.hashPass();
    }
    SECTION("no quick hash stored") {}
    const GameId first = library.at(a.path).id;
    const GameId second = library.at(b.path).id;
    for (const GameId id : {first, second}) {
        REQUIRE(library.writer()
                    .execute(fmt::format("INSERT INTO rv_test_sessions (game, note) VALUES ({}, 'played')", id.value()))
                    .has_value());
    }
    const auto sessions = [&](GameId id) {
        return library.count(fmt::format("SELECT count(*) FROM rv_test_sessions WHERE game = {}", id.value()));
    };

    // A leaves the library for one import...
    library.put({b});
    CHECK(changesOf(library.import().changes, "gb").vanished == 1);
    CHECK_FALSE(library.at(a.path).present);
    // ... and comes back unchanged while B is deleted: A's own identity takes its file back (SC-05), B's is not given
    // it, whatever its hash or its size, time and name say.
    library.put({a});
    const ImportResult back = library.import();
    CHECK(back.changes.moved.empty());
    CHECK(back.statistics.aliasesWritten == 0);
    CHECK(changesOf(back.changes, "gb").returned == 1);
    CHECK(changesOf(back.changes, "gb").vanished == 1);
    const StoredIdentity returned = library.at(a.path);
    CHECK(returned.id == first);
    CHECK(returned.present);
    CHECK(library.at(b.path).id == second);
    CHECK_FALSE(library.at(b.path).present);
    CHECK(library.catalog.store->aliasesOf(first).value().empty());
    CHECK(library.catalog.store->aliasesOf(second).value().empty());
    CHECK(library.history(first).empty());
    CHECK(library.history(second).empty());
    CHECK(sessions(first) == 1);
    CHECK(sessions(second) == 1);
    // The vanished copy is offered the file for review (the user may know that B moved there), never given it.
    REQUIRE(back.changes.reviews.size() == 1);
    const ReviewRow& review = back.changes.reviews[0];
    CHECK(review.game == second);
    REQUIRE(review.candidates.size() == 1);
    CHECK(review.candidates[0].relativePath() == a.path);
    CHECK(review.reason.find("at the path of another identity") != std::string::npos);
    library.checkSteady();
}

TEST_CASE("catalog_db/reconciliation: a vanished identity's file at the path of an identity that had another file "
          "there is reviewed, never merged",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-contested");
    library.put({File{"A/Game (USA).gb", 400, kTime + 4, "first game"},
                 File{"B/Game (USA).gb", 500, kTime + 5, "second game"}});
    library.import();
    library.hashPass();
    const GameId first = library.at("A/Game (USA).gb").id;
    const GameId second = library.at("B/Game (USA).gb").id;
    // A is deleted...
    library.put({File{"B/Game (USA).gb", 500, kTime + 5, "second game"}});
    library.import();
    // ... then B is moved where A was: its quick hash says B, the path says A.
    library.put({File{"A/Game (USA).gb", 500, kTime + 5, "second game"}});
    const ImportResult result = library.import();
    CHECK(result.changes.moved.empty());
    CHECK(result.statistics.aliasesWritten == 0);
    // The path's identity takes the file, as without a pairing pass; B's identity vanished, with a review row.
    CHECK(library.at("A/Game (USA).gb").id == first);
    CHECK(library.at("A/Game (USA).gb").present);
    CHECK_FALSE(library.at("B/Game (USA).gb").present);
    REQUIRE(result.changes.reviews.size() == 1);
    const ReviewRow& review = result.changes.reviews[0];
    CHECK(review.game == second);
    REQUIRE(review.candidates.size() == 1);
    CHECK(review.candidates[0].relativePath() == "A/Game (USA).gb");
    CHECK(review.reason.find("its quick hash but is at the path of another identity") != std::string::npos);

    // The user pairs them: the identity that had the path becomes the alias, as the user decided.
    const auto resolved = resolveReview(*library.catalog.store, library.profile.id, {review.id, first});
    INFO((resolved ? std::string("resolved") : resolved.error().toString()));
    REQUIRE(resolved.has_value());
    CHECK(library.at("A/Game (USA).gb").id == second);
    const auto alias = library.catalog.store->alias(first).value();
    REQUIRE(alias.has_value());
    CHECK(alias->game == second);
    library.checkSteady();
}

TEST_CASE("catalog_db/reconciliation: an equal quick hash under another file name and another time is reviewed, never "
          "paired",
          "[catalog_db][reconciliation]") {
    // Hacks, translations and alternative dumps of one game often share their first and last 64 KiB.
    Library library("rv-reconcile-hash-names");
    library.put({File{"Base Tweaks (Hack).gb", 300, kTime + 3, "same ends"}});
    library.import();
    library.hashPass();
    const GameId hack = library.at("Base Tweaks (Hack).gb").id;

    SECTION("another name, another time: a review row") {
        library.put({File{"Other Tweaks (Hack).gb", 300, kTime + 30, "same ends"}});
        const ImportResult result = library.import();
        CHECK(result.changes.moved.empty());
        CHECK_FALSE(library.at("Base Tweaks (Hack).gb").present);
        CHECK(library.at("Other Tweaks (Hack).gb").id != hack);
        REQUIRE(result.changes.reviews.size() == 1);
        CHECK(result.changes.reviews[0].game == hack);
        REQUIRE(result.changes.reviews[0].candidates.size() == 1);
        CHECK(result.changes.reviews[0].candidates[0].relativePath() == "Other Tweaks (Hack).gb");
        CHECK(result.changes.reviews[0].reason.find("neither its file name nor its time") != std::string::npos);
        library.checkSteady();
    }
    SECTION("another name, its time kept (a rename): paired") {
        library.put({File{"Renamed Tweaks (Hack).gb", 300, kTime + 3, "same ends"}});
        const ImportResult result = library.import();
        REQUIRE(result.changes.moved.size() == 1);
        CHECK(result.changes.moved[0].reason == MoveReason::QuickHash);
        CHECK(library.at("Renamed Tweaks (Hack).gb").id == hack);
    }
    SECTION("its name, another time (moved elsewhere): paired") {
        library.put({File{"Sub/Base Tweaks (Hack).gb", 300, kTime + 30, "same ends"}});
        const ImportResult result = library.import();
        REQUIRE(result.changes.moved.size() == 1);
        CHECK(result.changes.moved[0].reason == MoveReason::QuickHash);
        CHECK(library.at("Sub/Base Tweaks (Hack).gb").id == hack);
    }
}

TEST_CASE("catalog_db/reconciliation: re-keyed identities and files that come back are in the history and resolve "
          "their reviews",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-rekey");
    library.put({File{"Versioned (USA).gb", 300, kTime + 3, "v"}, File{"Returner (USA).gb", 310, kTime + 4, "r"},
                 File{"Cased (USA).gb", 320, kTime + 5, "c"}});
    library.import();
    const GameId versioned = library.at("Versioned (USA).gb").id;
    const GameId returner = library.at("Returner (USA).gb").id;
    const GameId cased = library.at("Cased (USA).gb").id;
    {
        // Keys of an older casing table, as a Windows update leaves them.
        auto transaction = library.catalog.store->beginImport(library.profile.id);
        REQUIRE(transaction.has_value());
        for (const std::string_view path : {"Versioned (USA).gb", "Cased (USA).gb"}) {
            StoredIdentity old = library.at(path);
            old.keyVersion.casingTable = "older.table";
            REQUIRE((*transaction)->putIdentity(old).has_value());
        }
        REQUIRE((*transaction)->commit().has_value());
    }
    // The returner vanishes with a candidate of its name (another size): a review row. Cased is renamed in case only.
    library.put({File{"Versioned (USA).gb", 300, kTime + 3, "v"}, File{"Sub/Returner (USA).gb", 999, kTime + 4, "x"},
                 File{"CASED (USA).gb", 320, kTime + 5, "c"}});
    const ImportResult rekeyed = library.import();
    CHECK(changesOf(rekeyed.changes, "gb").rekeyed == 2);
    const std::vector<IdentityEvent> history = library.history(versioned);
    REQUIRE(history.size() == 1);
    CHECK(history[0].kind == IdentityEventKind::Rekeyed);
    CHECK(history[0].toPath == "Versioned (USA).gb");
    CHECK(history[0].detail.find("older.table") != std::string::npos);
    // Re-keyed and renamed in case in one import: the event names the path it has now.
    const std::vector<IdentityEvent> casedHistory = library.history(cased);
    REQUIRE(casedHistory.size() == 1);
    CHECK(casedHistory[0].fromPath == "Cased (USA).gb");
    CHECK(casedHistory[0].toPath == "CASED (USA).gb");
    CHECK(library.at("CASED (USA).gb").id == cased);
    REQUIRE(rekeyed.changes.reviews.size() == 1);
    CHECK(rekeyed.changes.reviews[0].game == returner);
    // Back at its path: its review is resolved (returned).
    library.put({File{"Versioned (USA).gb", 300, kTime + 3, "v"}, File{"Returner (USA).gb", 310, kTime + 4, "r"},
                 File{"Sub/Returner (USA).gb", 999, kTime + 4, "x"}});
    const ImportResult returned = library.import();
    CHECK(changesOf(returned.changes, "gb").returned == 1);
    CHECK(library.reviews().empty());
    const std::vector<StoredReview> all = library.reviews(false);
    REQUIRE(all.size() == 1);
    CHECK(all[0].resolution == ReviewResolution::Returned);
    library.checkSteady();
}

TEST_CASE("catalog_db/reconciliation: the quick-hash pass commits in batches, resumes after a cancel and stops at its "
          "limit, in a fixed order",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-pass");
    std::vector<File> present;
    for (int i = 0; i < 10; ++i) {
        present.push_back(File{fmt::format("Game {:02} (USA).gb", 9 - i), 2000u + static_cast<std::uint64_t>(i),
                               kTime + i, fmt::format("game {}", i)});
    }
    library.put(present);
    library.import();
    const std::vector<StoredIdentity> identities =
        library.catalog.store->identities(library.profile.id, systemName("gb")).value();

    std::atomic<bool> cancel{false};
    QuickHashPassOptions options;
    options.cancel = &cancel;
    options.batchFiles = 3;
    std::size_t progressCalls = 0;
    options.progress = [&](const QuickHashProgress&) { ++progressCalls; };
    library.files.onRead = [&](std::string_view) {
        if (library.files.reads.size() == 5) {
            cancel = true;
        }
    };
    const QuickHashPassResult cancelled = library.hashPass(options);
    CHECK(cancelled.cancelled);
    CHECK(cancelled.candidates == 14);
    CHECK(cancelled.hashed == 5); // what was read is committed: 3, then the 2 left at the cancel
    CHECK(cancelled.batches == 2);
    CHECK(cancelled.remaining == 9);
    CHECK(progressCalls >= 5);
    // In the stored order: by key, then path.
    std::vector<std::string> expected;
    for (const StoredIdentity& identity : identities) {
        expected.push_back(identity.key.relativePath());
    }
    expected.resize(5);
    CHECK(library.files.reads == expected);
    const auto hashedNow = [&] {
        return library.count("SELECT count(*) FROM rv_game_identities WHERE quick_hash IS NOT NULL");
    };
    CHECK(hashedNow() == 5);

    // Resumed: what is committed stays and is not read again; a limit bounds the run.
    library.files.onRead = nullptr;
    cancel = false;
    QuickHashPassOptions limited;
    limited.limit = 4;
    const QuickHashPassResult partial = library.hashPass(limited);
    CHECK(partial.current == 5);
    CHECK(partial.candidates == 9);
    CHECK(partial.hashed == 4);
    CHECK(partial.remaining == 5);
    CHECK(hashedNow() == 9);
    const QuickHashPassResult rest = library.hashPass();
    CHECK(rest.hashed == 5);
    CHECK(rest.remaining == 0);
    const QuickHashPassResult none = library.hashPass();
    CHECK(none.candidates == 0);
    CHECK(none.current == 14);
    CHECK(library.files.reads.empty());
    // Every hash is the file's, current.
    const std::vector<StoredIdentity> hashed =
        library.catalog.store->identities(library.profile.id, systemName("gb")).value();
    for (const StoredIdentity& identity : hashed) {
        INFO(identity.key.relativePath());
        CHECK(identity.quickHash == hashOf(library.files.files.at(identity.key.relativePath())));
    }
}

TEST_CASE("catalog_db/reconciliation: the pass skips files changed or gone since the last import and reports the "
          "unreadable; a changed file's hash goes stale",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-pass-files");
    library.put({File{"Stable (USA).gb", 100, kTime + 1, "stable"}, File{"Changed (USA).gb", 110, kTime + 2, "c1"},
                 File{"Gone (USA).gb", 120, kTime + 3, "gone"}, File{"Denied (USA).gb", 130, kTime + 4, "d"}});
    library.import();
    // On disk since the import: one file rewritten, one deleted, one denied to this user.
    library.files.files["Changed (USA).gb"] = File{"Changed (USA).gb", 111, kTime + 20, "c2"};
    library.files.files.erase("Gone (USA).gb");
    library.files.denied = {"Denied (USA).gb"};
    const QuickHashPassResult pass = library.hashPass();
    CHECK(pass.hashed == 5); // the stable file and the fillers
    CHECK(pass.changed == 1);
    CHECK(pass.gone == 1);
    CHECK(pass.unreadable == 1);
    REQUIRE(pass.issues.size() == 1);
    CHECK(pass.issues[0].code == kQuickHashIssueUnreadable);
    CHECK(pass.issues[0].severity == IssueSeverity::Warning);
    CHECK_FALSE(library.at("Changed (USA).gb").quickHash.has_value());

    // The stable file changes: the next import records its new facts, its hash goes stale and the pass hashes it again.
    const StoredIdentity stable = library.at("Stable (USA).gb");
    REQUIRE(currentQuickHash(stable).has_value());
    library.files.denied.clear();
    library.put({File{"Stable (USA).gb", 101, kTime + 30, "stable 2"}, File{"Changed (USA).gb", 111, kTime + 20, "c2"},
                 File{"Denied (USA).gb", 130, kTime + 4, "d"}});
    library.import();
    const StoredIdentity stale = library.at("Stable (USA).gb");
    CHECK(stale.quickHash == stable.quickHash);
    CHECK_FALSE(currentQuickHash(stale).has_value());

    // A stale hash says nothing of the content that vanished: moved now, it pairs by size, time and name.
    library.put({File{"Sub/Stable (USA).gb", 101, kTime + 30, "stable 2"},
                 File{"Changed (USA).gb", 111, kTime + 20, "c2"}, File{"Denied (USA).gb", 130, kTime + 4, "d"}});
    const ImportResult moved = library.import();
    REQUIRE(moved.changes.moved.size() == 1);
    CHECK(moved.changes.moved[0].reason == MoveReason::SizeTimeName);
    CHECK(library.files.reads.empty());
    const QuickHashPassResult again = library.hashPass();
    CHECK(again.hashed == 3); // the moved file, the changed one, the one denied before
    CHECK(currentQuickHash(library.at("Sub/Stable (USA).gb")).has_value());
}

TEST_CASE("catalog_db/reconciliation: the quick-hash pass and an import never overlap",
          "[catalog_db][reconciliation]") {
    Library library("rv-reconcile-overlap");
    library.put({File{"One (USA).gb", 100, kTime + 1, "one"}, File{"Two (USA).gb", 200, kTime + 2, "two"}});
    library.import();
    // Another holder of the database (another process's connection and store).
    auto other = rv::catalog_db::Database::open(library.catalog.database->path());
    REQUIRE(other.has_value());
    auto otherStore = rv::catalog_db::SqliteCatalogStore::open(*other);
    REQUIRE(otherStore.has_value());

    SECTION("an import holds the lock: the pass is refused before it reads anything") {
        auto lock = (*otherStore)->lockImport(library.profile.id);
        REQUIRE(lock.has_value());
        QuickHashPassOptions options;
        options.reader = library.files.reader();
        const auto refused = runQuickHashPass(*library.catalog.store, library.profile.id, options);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == rv::ErrorCode::Conflict);
        CHECK(library.files.reads.empty());
    }
    SECTION("the pass holds the lock: an import of another holder is refused while it runs") {
        std::optional<rv::ErrorCode> importRefused;
        library.files.onRead = [&](std::string_view) {
            if (importRefused) {
                return;
            }
            rv::test::ScriptedSource source = library.source;
            rv::test::ScriptedMedia media;
            ImportService service(source, media, **otherStore);
            const auto run = service.run(library.profile);
            importRefused = run ? rv::ErrorCode::Unknown : run.error().code;
        };
        const QuickHashPassResult pass = library.hashPass();
        CHECK(importRefused == rv::ErrorCode::Conflict);
        CHECK(pass.hashed == 6);
        // Once it is done, the import runs.
        rv::test::ScriptedSource source = library.source;
        rv::test::ScriptedMedia media;
        ImportService service(source, media, **otherStore);
        CHECK(service.run(library.profile).has_value());
    }
}
