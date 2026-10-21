// SqliteCatalogQuery::roomEntries (I3 step 2; ADR-0010 §2, §12): the room's bulk snapshot of one system, in one read on
// the catalog worker's reader. Four statements on one snapshot (a deferred read transaction, none of its own when the
// connection is inside one already, as the other reads of catalog_query.cpp): the system's entries, their media, their
// play sessions, and rv_meta's sort key version. Each statement reaches idx_games through idx_games_system, once per
// library (rv_libraries drives the join: the index's first column is the library), never by a scan; the entries come
// sorted by GameId, the media and sessions are matched to them by binary search. Within 50 ms for 2,000 games in
// Release (docs/i3/step-2.md has the figures). The statements are prepared per call: a few tens of microseconds,
// against the read's milliseconds, and the query object's statement cache stays private to catalog_query.cpp.

#include <rv/catalog_db/catalog_query.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/time.hpp>

namespace rv::catalog_db {
namespace {

using catalog::GameId;
using catalog::RoomEntry;

/// Why the snapshot's media are not known when this query answers: the database keeps the presence its imports found,
/// not whether the media root can be reached now (esde::checkRoomMedia() says that).
constexpr std::string_view kMediaNotChecked =
    "the media root has not been checked: the catalog's database cannot tell whether it can be reached";

/// Retroverse's sessions that started the game: every outcome but a failure to start and a launch blocked before it, of
/// the launches that were no launch tests (retroverse-cli launch --test).
constexpr std::string_view kStartedSessions = "s.test = 0 AND s.outcome NOT IN ('failed-to-start', 'blocked')";

tl::unexpected<Error> unreadable(std::string_view what, std::string context) {
    return makeError(ErrorCode::ParseError, fmt::format("the catalog database holds {}", what), std::move(context));
}

Expected<GameId> readGameId(std::int64_t value, std::string_view table) {
    const auto id = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
    if (!id) {
        return unreadable(fmt::format("a row whose GameId {} is none", value), std::string(table));
    }
    return *id;
}

/// An enumeration stored as its toString() name, read back with its parse function.
template <class Enum, class Parse>
Expected<Enum> readEnum(std::string_view text, Parse parse, std::string_view what, const std::string& context) {
    const std::optional<Enum> value = parse(text);
    if (!value) {
        return unreadable(fmt::format("an unknown {} '{}'", what, text), context);
    }
    return *value;
}

/// Steps `statement` through its rows, calling `read(statement)` for each (a Status: an error stops the read).
template <class Read>
Status forEachRow(Statement& statement, Read read) {
    const ScopedReset reset(statement);
    while (true) {
        const auto row = statement.step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            return ok();
        }
        if (auto status = read(statement); !status) {
            return status;
        }
    }
}

/// The entry of `entries` (sorted by GameId) with the id in column 0 of `row`. Errors: ParseError when none has it,
/// which a consistent database never gives (every statement reads the same snapshot through the same join).
Expected<RoomEntry*> entryOf(std::vector<RoomEntry>& entries, const Statement& row, std::string_view table) {
    const auto id = readGameId(row.columnInt(0), table);
    if (!id) {
        return propagate(id.error());
    }
    const auto found = std::lower_bound(entries.begin(), entries.end(), *id,
                                        [](const RoomEntry& entry, GameId value) { return entry.id < value; });
    if (found == entries.end() || found->id != *id) {
        return unreadable(fmt::format("a row of {} without its idx_games row", id->toString()), std::string(table));
    }
    return &*found;
}

/// An entry from a row of the entries' statement (see roomEntries()).
Expected<RoomEntry> readEntry(const Statement& row) {
    const auto id = readGameId(row.columnInt(0), "idx_games");
    if (!id) {
        return propagate(id.error());
    }
    const std::string context = fmt::format("idx_games row {}", id->value());
    RoomEntry entry;
    entry.id = *id;
    auto key = catalog::makeLookupKey(row.columnText(1));
    if (!key) {
        return unreadable(fmt::format("a relative path that makes no lookup key ({})", key.error().message), context);
    }
    entry.key = std::move(*key);
    const auto state =
        readEnum<catalog::EntryState>(row.columnText(3), catalog::parseEntryState, "entry state", context);
    const auto availability =
        readEnum<catalog::Availability>(row.columnText(4), catalog::parseAvailability, "availability", context);
    if (!state || !availability) {
        return propagate(!state ? state.error() : availability.error());
    }
    entry.state = *state;
    entry.availability = *availability;
    // Its file facts' kind when it was listed, else what the entry is (the import's rule for the media's name).
    entry.directory = row.isNull(2) ? entry.state == catalog::EntryState::Folder : row.columnInt(2) != 0;
    entry.title = std::string(row.columnText(5));
    entry.sortKey = std::string(row.columnText(6));
    if (!row.isNull(7)) {
        entry.year = static_cast<int>(row.columnInt(7) / 10000);
    }
    if (!row.isNull(8)) {
        entry.rating = catalog::Rating{static_cast<int>(row.columnInt(8))};
    }
    entry.favorite = row.columnInt(9) != 0;
    entry.hidden = row.columnInt(10) != 0;
    const std::int64_t playCount = row.columnInt(11);
    if (playCount < 0) {
        return unreadable("a negative play count", context);
    }
    entry.esdePlayCount = static_cast<std::uint64_t>(playCount);
    return entry;
}

/// The statements of the read, `?1` the system's name. rv_libraries drives each join, so that idx_games is read through
/// idx_games_system (library, system) for each library: a CROSS JOIN, which SQLite never reorders (I4 step 6b: with a
/// plain JOIN its planner, without statistics, scanned the whole of idx_games, or of idx_games_system, for every
/// system, about 70 ms a system on a catalog of 100,000 entries, whatever the system held).
constexpr std::string_view kSystemGames =
    "rv_libraries AS l CROSS JOIN idx_games AS g ON g.library = l.id AND g.system = ?1";

std::string entriesSql() {
    return fmt::format("SELECT g.id, g.relative_path, g.is_directory, g.state, g.availability, g.title, g.sort_key, "
                       "g.release_date, g.rating, g.favorite, g.hidden, g.play_count FROM {} ORDER BY g.id",
                       kSystemGames);
}

std::string mediaSql() {
    return fmt::format("SELECT m.game, m.kind, m.extension FROM {} JOIN idx_media AS m ON m.game = g.id", kSystemGames);
}

std::string sessionsSql() {
    // The system's games drive the sessions too, each looked up through rv_play_sessions_game.
    return fmt::format("SELECT s.game, count(*), max(s.started_at) FROM {} CROSS JOIN rv_play_sessions AS s ON s.game "
                       "= g.id WHERE {} GROUP BY s.game",
                       kSystemGames, kStartedSessions);
}

/// The read itself, inside the caller's snapshot.
Expected<catalog::RoomSnapshot> readSnapshot(Connection& connection, const catalog::SystemName& system) {
    catalog::RoomSnapshot snapshot;
    snapshot.system = system;
    snapshot.mediaKnown = false;
    snapshot.mediaUnknownReason = std::string(kMediaNotChecked);

    auto version = connection.queryText("SELECT value FROM rv_meta WHERE key = 'sort_key_version'");
    if (!version) {
        return propagate(version.error());
    }
    std::uint32_t sortKeyVersion = 0;
    const std::string versionText = version->value_or(std::string());
    const auto parsed = std::from_chars(versionText.data(), versionText.data() + versionText.size(), sortKeyVersion);
    if (versionText.empty() || parsed.ec != std::errc() || parsed.ptr != versionText.data() + versionText.size() ||
        sortKeyVersion == 0) {
        return unreadable(fmt::format("a sort key version that is none ('{}')", versionText), "rv_meta");
    }
    snapshot.sortKeyVersion = sortKeyVersion;

    auto entries = connection.prepare(entriesSql());
    if (!entries) {
        return propagate(entries.error());
    }
    entries->bindText(1, system.str());
    if (auto read = forEachRow(*entries,
                               [&](const Statement& row) -> Status {
                                   auto entry = readEntry(row);
                                   if (!entry) {
                                       return propagate(entry.error());
                                   }
                                   snapshot.entries.push_back(std::move(*entry));
                                   return ok();
                               });
        !read) {
        return propagate(read.error());
    }

    auto media = connection.prepare(mediaSql());
    if (!media) {
        return propagate(media.error());
    }
    media->bindText(1, system.str());
    if (auto read = forEachRow(*media,
                               [&](const Statement& row) -> Status {
                                   auto entry = entryOf(snapshot.entries, row, "idx_media");
                                   if (!entry) {
                                       return propagate(entry.error());
                                   }
                                   const std::int64_t kind = row.columnInt(1);
                                   if (kind < 0 || kind >= static_cast<std::int64_t>(catalog::kMediaKindCount)) {
                                       return unreadable(fmt::format("an unknown media kind {}", kind),
                                                         fmt::format("idx_media of {}", (*entry)->id.value()));
                                   }
                                   (*entry)->media.kinds[static_cast<std::size_t>(kind)] =
                                       catalog::MediaPresence{true, std::string(row.columnText(2))};
                                   return ok();
                               });
        !read) {
        return propagate(read.error());
    }

    auto sessions = connection.prepare(sessionsSql());
    if (!sessions) {
        return propagate(sessions.error());
    }
    sessions->bindText(1, system.str());
    if (auto read = forEachRow(*sessions,
                               [&](const Statement& row) -> Status {
                                   auto entry = entryOf(snapshot.entries, row, "rv_play_sessions");
                                   if (!entry) {
                                       return propagate(entry.error());
                                   }
                                   (*entry)->sessionCount = static_cast<std::uint64_t>(row.columnInt(1));
                                   const auto last = core::parseUtcIso8601(row.columnText(2));
                                   if (!last) {
                                       return unreadable(
                                           fmt::format("a session start that is no time ({})", last.error().message),
                                           fmt::format("rv_play_sessions of {}", (*entry)->id.value()));
                                   }
                                   (*entry)->lastSession = *last;
                                   return ok();
                               });
        !read) {
        return propagate(read.error());
    }
    return snapshot;
}

} // namespace

Expected<catalog::RoomSnapshot> SqliteCatalogQuery::roomEntries(const catalog::SystemName& system) {
    Connection& reader = connection();
    Transaction transaction;
    if (!reader.inTransaction()) {
        auto begun = Transaction::begin(reader, Transaction::Mode::Deferred);
        if (!begun) {
            return propagate(begun.error());
        }
        transaction = std::move(*begun);
    }
    auto known = hasSystem(system);
    if (!known) {
        return propagate(known.error());
    }
    if (!*known) {
        return makeError(ErrorCode::NotFound, "the catalog knows no such system", system.str());
    }
    auto snapshot = readSnapshot(reader, system);
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    // A read commits nothing: its end only lets the snapshot go.
    if (transaction.active()) {
        if (auto finished = transaction.commit(); !finished) {
            return propagate(finished.error());
        }
    }
    return snapshot;
}

Expected<std::optional<catalog::GameId>> SqliteCatalogQuery::aliasTarget(catalog::GameId alias) {
    auto select = connection().prepare("SELECT game FROM rv_identity_aliases WHERE alias = ?1");
    if (!select) {
        return propagate(select.error());
    }
    select->bindInt(1, static_cast<std::int64_t>(alias.value()));
    auto row = select->step();
    if (!row) {
        return propagate(row.error());
    }
    if (!*row) {
        return std::optional<catalog::GameId>();
    }
    const std::int64_t value = select->columnInt(0);
    auto game = catalog::GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
    if (!game) {
        return makeError(ErrorCode::ParseError, "an alias of no GameId",
                         fmt::format("rv_identity_aliases row {}", alias.value()));
    }
    return std::optional<catalog::GameId>(*game);
}

} // namespace rv::catalog_db
