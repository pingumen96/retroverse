#include <rv/catalog_db/migrator.hpp>

#include <algorithm>
#include <map>
#include <span>
#include <system_error>
#include <thread>
#include <tuple>

#include <windows.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

#include "schema.hpp"

namespace rv::catalog_db {
namespace {

/// Free space a migration needs besides twice the database's size (its backup, and its WAL when it rebuilds tables).
constexpr std::uint64_t kFreeSpaceMarginBytes = 32ull * 1024 * 1024;
/// Between two attempts to make the lock exclusive.
constexpr auto kLockRetry = std::chrono::milliseconds(50);
/// The most problems a check lists one by one.
constexpr std::size_t kMaxDetails = 100;

std::string lossy(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
}

/// Rewords an error, keeping its code: "<prefix>: <message>".
Error withPrefix(Error error, std::string_view prefix) {
    error.message = fmt::format("{}: {}", prefix, error.message);
    return error;
}

// --- Backups ---------------------------------------------------------------------------------------------------------

/// What a backup's name holds: "<stem>-<time>-v<version>[-<counter>].sqlite".
struct BackupName {
    std::wstring time; ///< YYYYMMDDTHHMMSS.mmmZ: sorts like the times
    int version = 0;
    int counter = 1; ///< 1 when the name has none
};

/// The length of the time in a backup's name.
constexpr std::size_t kBackupTimeLength = 20;

bool isDigit(wchar_t c) noexcept {
    return c >= L'0' && c <= L'9';
}

/// Reads the decimal digits at `text[at]` on (at least one, at most 9): the value, `at` moved past them.
std::optional<int> readNumber(std::wstring_view text, std::size_t& at) {
    const std::size_t start = at;
    int value = 0;
    while (at < text.size() && isDigit(text[at]) && at - start < 9) {
        value = value * 10 + (text[at] - L'0');
        ++at;
    }
    if (at == start) {
        return std::nullopt;
    }
    return value;
}

/// The parts of `name` when it is a backup's name of the database whose file stem is `stem`.
std::optional<BackupName> parseBackupName(std::wstring_view name, std::wstring_view stem) {
    constexpr std::wstring_view kExtension = L".sqlite";
    if (name.size() < stem.size() + 1 + kBackupTimeLength + 3 + kExtension.size() || !name.starts_with(stem) ||
        name[stem.size()] != L'-' || !name.ends_with(kExtension)) {
        return std::nullopt;
    }
    std::size_t at = stem.size() + 1;
    const std::wstring_view time = name.substr(at, kBackupTimeLength);
    // YYYYMMDD T HHMMSS . mmm Z
    for (std::size_t i = 0; i < time.size(); ++i) {
        const wchar_t c = time[i];
        const bool expected = i == 8 ? c == L'T' : i == 15 ? c == L'.' : i == 19 ? c == L'Z' : isDigit(c);
        if (!expected) {
            return std::nullopt;
        }
    }
    at += kBackupTimeLength;
    if (name.substr(at, 2) != L"-v") {
        return std::nullopt;
    }
    at += 2;
    BackupName parsed{std::wstring(time)};
    const auto version = readNumber(name, at);
    if (!version) {
        return std::nullopt;
    }
    parsed.version = *version;
    if (at < name.size() && name[at] == L'-') {
        ++at;
        const auto counter = readNumber(name, at);
        if (!counter || *counter < 2) {
            return std::nullopt;
        }
        parsed.counter = *counter;
    }
    if (name.substr(at) != kExtension) {
        return std::nullopt;
    }
    return parsed;
}

/// "20261016T121314.123Z": core::formatUtcIso8601() without its separators, which file names cannot hold (':').
std::wstring backupTime(std::chrono::system_clock::time_point time) {
    std::wstring text;
    for (const char c : core::formatUtcIso8601(time)) {
        if (c != '-' && c != ':') {
            text += static_cast<wchar_t>(c);
        }
    }
    return text;
}

/// The backups found in `folder` for the file stem `stem`, newest first, with their names' parts; and, in `partials`
/// when given, the copies cut short ("<backup name>.partial").
std::vector<std::pair<std::filesystem::path, BackupName>>
findBackups(const std::filesystem::path& folder, std::wstring_view stem,
            std::vector<std::filesystem::path>* partials = nullptr) {
    constexpr std::wstring_view kPartial = L".partial";
    std::vector<std::pair<std::filesystem::path, BackupName>> found;
    std::error_code ec;
    // Listed through the \\?\ form (any length); the entries are named from the plain folder.
    std::filesystem::directory_iterator entries(std::filesystem::path(core::extendedLengthPath(folder)), ec);
    for (; !ec && entries != std::filesystem::directory_iterator(); entries.increment(ec)) {
        std::error_code typeError;
        if (!entries->is_regular_file(typeError)) {
            continue;
        }
        const std::filesystem::path name = entries->path().filename();
        const std::wstring_view text = name.native();
        if (const auto parsed = parseBackupName(text, stem)) {
            found.emplace_back(folder / name, *parsed);
        } else if (partials != nullptr && text.ends_with(kPartial) &&
                   parseBackupName(text.substr(0, text.size() - kPartial.size()), stem)) {
            partials->push_back(folder / name);
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return std::tie(a.second.time, a.second.counter, a.first) > std::tie(b.second.time, b.second.counter, b.first);
    });
    return found;
}

/// A backup name in `folder` that no file has: the time's own, else with the first free counter.
Expected<std::filesystem::path> uniqueBackupPath(const std::filesystem::path& folder, std::wstring_view stem,
                                                 std::chrono::system_clock::time_point time, int version) {
    const std::wstring base = std::wstring(stem) + L"-" + backupTime(time) + L"-v" + std::to_wstring(version);
    for (int counter = 1; counter <= 1000; ++counter) {
        const std::wstring name = base + (counter == 1 ? std::wstring() : L"-" + std::to_wstring(counter)) + L".sqlite";
        const std::filesystem::path candidate = folder / name;
        std::error_code ec;
        const bool exists = std::filesystem::exists(std::filesystem::path(core::extendedLengthPath(candidate)), ec);
        if (ec) {
            return makeError(ErrorCode::IoError, "cannot look into the backup folder: " + ec.message(), lossy(folder));
        }
        if (!exists) {
            return candidate;
        }
    }
    return makeError(ErrorCode::Conflict, "no free backup name for this time", lossy(folder));
}

/// Removes the backups of `stem` in `folder` beyond the newest `keep`, the one just `written` always kept (a clock set
/// back would have dated it the oldest), and the copies cut short (".partial": no other backup is being written, this
/// process holds the lock exclusive). What cannot be removed is logged and left. Returns the backups removed.
std::vector<std::filesystem::path> removeOldBackups(const std::filesystem::path& folder, std::wstring_view stem,
                                                    std::size_t keep, const std::filesystem::path& written) {
    const auto remove = [](const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::remove(std::filesystem::path(core::extendedLengthPath(path)), ec) && !ec) {
            return true;
        }
        spdlog::warn("Catalog database: cannot remove the old backup {}: {}", lossy(path),
                     ec ? ec.message() : std::string("not found"));
        return false;
    };
    std::vector<std::filesystem::path> partials;
    auto backups = findBackups(folder, stem, &partials);
    std::stable_partition(backups.begin(), backups.end(), [&](const auto& backup) { return backup.first == written; });
    std::vector<std::filesystem::path> removed;
    for (std::size_t i = std::max<std::size_t>(keep, 1); i < backups.size(); ++i) {
        if (remove(backups[i].first)) {
            removed.push_back(backups[i].first);
        }
    }
    for (const std::filesystem::path& partial : partials) {
        static_cast<void>(remove(partial));
    }
    return removed;
}

/// The free bytes available to this user on the volume of `folder` (GetDiskFreeSpaceExW).
Expected<std::uint64_t> systemFreeSpace(const std::filesystem::path& folder) {
    std::wstring name = core::extendedLengthPath(folder);
    if (!name.empty() && name.back() != L'\\') {
        name += L'\\';
    }
    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(name.c_str(), &available, nullptr, nullptr) == FALSE) {
        const DWORD error = GetLastError();
        return makeError(core::fileErrorCode(error, folder),
                         "cannot read the free space of the volume: " + core::systemErrorMessage(error), lossy(folder));
    }
    return static_cast<std::uint64_t>(available.QuadPart);
}

/// The database's size as SQLite sees it (its pages, the WAL's included).
Expected<std::uint64_t> databaseBytes(Connection& connection) {
    const auto pages = connection.queryInt("PRAGMA page_count");
    if (!pages) {
        return propagate(pages.error());
    }
    const auto pageSize = connection.queryInt("PRAGMA page_size");
    if (!pageSize) {
        return propagate(pageSize.error());
    }
    return static_cast<std::uint64_t>(std::max<std::int64_t>(pages->value_or(0), 0)) *
           static_cast<std::uint64_t>(std::max<std::int64_t>(pageSize->value_or(0), 0));
}

/// Backs the database up before a migration: free space first, then a new backup, then the old ones beyond `keep`.
Status backUp(Database& database, int version, const MigrationOptions& options, MigrationReport& report) {
    const std::filesystem::path folder = backupFolderOf(database.path());
    const auto bytes = databaseBytes(database.writer());
    if (!bytes) {
        return propagate(bytes.error());
    }
    // The volume of the database's folder, which exists (the backup folder may not yet): the same volume.
    const std::filesystem::path databaseFolder = database.path().parent_path();
    const auto available = options.freeSpace ? options.freeSpace(databaseFolder) : systemFreeSpace(databaseFolder);
    if (!available) {
        return propagate(available.error());
    }
    const std::uint64_t needed = 2 * *bytes + kFreeSpaceMarginBytes;
    if (*available < needed) {
        return makeError(ErrorCode::IoError,
                         fmt::format("not enough free space to back the catalog database up and migrate it: {} bytes "
                                     "needed, {} available",
                                     needed, *available),
                         lossy(databaseFolder));
    }
    const std::wstring stem = database.path().stem().native();
    const auto now = options.clock ? options.clock() : std::chrono::system_clock::now();
    const auto target = uniqueBackupPath(folder, stem, now, version);
    if (!target) {
        return propagate(target.error());
    }
    if (auto copied = database.backupTo(*target); !copied) {
        return propagate(withPrefix(copied.error(), "cannot back the catalog database up before migrating it"));
    }
    report.backup = *target;
    spdlog::info("Catalog database backed up to {}", lossy(*target));
    report.removedBackups = removeOldBackups(folder, stem, options.backupsKept, *target);
    return ok();
}

// --- Migrations ------------------------------------------------------------------------------------------------------

/// The rows PRAGMA foreign_key_check reports, as sentences (at most `limit`), and their count.
Expected<std::pair<std::vector<std::string>, std::size_t>> foreignKeyViolations(Connection& connection,
                                                                                std::size_t limit) {
    auto check = connection.prepare("PRAGMA foreign_key_check");
    if (!check) {
        return propagate(check.error());
    }
    std::vector<std::string> details;
    std::size_t count = 0;
    while (true) {
        const auto row = check->step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            break;
        }
        ++count;
        if (details.size() < limit) {
            const std::string table(check->columnText(0));
            const std::string parent(check->columnText(2));
            details.push_back(check->isNull(1) ? fmt::format("a row of {} refers to a missing row of {}", table, parent)
                                               : fmt::format("row {} of {} refers to a missing row of {}",
                                                             check->columnInt(1), table, parent));
        }
    }
    return std::make_pair(std::move(details), count);
}

Status checkMigrations(std::span<const Migration> migrations) {
    if (migrations.empty()) {
        return makeError(ErrorCode::InvalidArgument, "no migrations");
    }
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        if (migrations[i].version != static_cast<int>(i) + 1) {
            return makeError(ErrorCode::InvalidArgument, "migrations must be numbered 1, 2, ... without gaps",
                             migrations[i].name);
        }
        if (!migrations[i].apply) {
            return makeError(ErrorCode::InvalidArgument, "a migration has nothing to apply", migrations[i].name);
        }
    }
    return ok();
}

/// Runs one migration in its own transaction: the database's application id and the new version set with it, the
/// foreign keys checked before COMMIT; any error rolls it all back.
Status runMigration(Connection& connection, const Migration& migration) {
    if (migration.rebuildsTables) {
        // Outside the transaction, where the pragma has effect (SQLite's table rebuild procedure).
        if (auto off = connection.execute("PRAGMA foreign_keys = OFF"); !off) {
            return off;
        }
    }
    Status result = [&]() -> Status {
        auto transaction = Transaction::begin(connection, Transaction::Mode::Immediate);
        if (!transaction) {
            return propagate(transaction.error());
        }
        if (auto marked = connection.execute(fmt::format("PRAGMA application_id = {}", kApplicationId)); !marked) {
            return marked;
        }
        if (auto applied = migration.apply(connection); !applied) {
            return applied;
        }
        if (auto versioned = connection.execute(fmt::format("PRAGMA user_version = {}", migration.version));
            !versioned) {
            return versioned;
        }
        const auto violations = foreignKeyViolations(connection, 1);
        if (!violations) {
            return propagate(violations.error());
        }
        if (violations->second > 0) {
            return makeError(ErrorCode::ValidationFailed,
                             fmt::format("it leaves {} row(s) that break a foreign key, first: {}", violations->second,
                                         violations->first.front()));
        }
        return transaction->commit();
    }();
    if (migration.rebuildsTables) {
        if (auto on = connection.execute("PRAGMA foreign_keys = ON"); !on && result) {
            result = on;
        }
    }
    if (!result) {
        return propagate(withPrefix(result.error(), fmt::format("migration to schema version {} ({}) failed",
                                                                migration.version, migration.name)));
    }
    return ok();
}

/// Makes the database's lock exclusive (true), waiting up to options.lockWait for the other holders to close the
/// database; or finds the database at `target` (false) after another holder that wanted to migrate it too went first:
/// this one left and joined again meanwhile (DatabaseLock::rejoin()), so that the two never wait on each other.
Expected<bool> makeExclusive(Database& database, int target, const MigrationOptions& options) {
    DatabaseLock& lock = database.lock();
    const auto deadline = std::chrono::steady_clock::now() + options.lockWait;
    while (true) {
        const auto remaining =
            std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                     std::chrono::milliseconds(0));
        const auto upgraded = lock.upgrade(remaining);
        if (!upgraded) {
            if (upgraded.error().code == ErrorCode::Conflict) {
                return propagate(withPrefix(upgraded.error(),
                                            "the catalog database cannot be migrated while another Retroverse process "
                                            "has it open (close it, then try again)"));
            }
            return propagate(upgraded.error());
        }
        if (*upgraded == DatabaseLock::Upgrade::Exclusive) {
            return true;
        }
        // Another holder has the gate: it waits to migrate too, or joins this instant. Leave, so that it can, and join
        // again once it is done; then the version says whether there is anything left to do.
        if (std::chrono::steady_clock::now() >= deadline) {
            return makeError(ErrorCode::Conflict, "another Retroverse process is migrating the catalog database",
                             lossy(database.path()));
        }
        if (auto rejoined = lock.rejoin(options.otherMigrationWait); !rejoined) {
            return propagate(rejoined.error());
        }
        const auto version = database.schemaVersion();
        if (!version) {
            return propagate(version.error());
        }
        if (*version >= target) {
            return false;
        }
        std::this_thread::sleep_for(kLockRetry);
    }
}

Error newerDatabase(int version, int known, const std::filesystem::path& file) {
    return Error{ErrorCode::Conflict,
                 fmt::format("the catalog database has schema version {}, newer than this program's {}: it was "
                             "written by a newer Retroverse",
                             version, known),
                 lossy(file)};
}

/// The program's schema objects: a name's type and SQL, SQLite's own objects left out.
struct SchemaObject {
    std::string type;
    std::string sql;

    bool operator==(const SchemaObject&) const = default;
};

Expected<std::map<std::string, SchemaObject>> schemaObjects(Connection& connection) {
    auto list = connection.prepare(
        "SELECT name, type, coalesce(sql, '') FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*' ORDER BY name");
    if (!list) {
        return propagate(list.error());
    }
    std::map<std::string, SchemaObject> objects;
    while (true) {
        const auto row = list->step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            return objects;
        }
        objects.emplace(std::string(list->columnText(0)),
                        SchemaObject{std::string(list->columnText(1)), std::string(list->columnText(2))});
    }
}

/// `name` as an SQL identifier, quoted.
std::string quoteIdentifier(std::string_view name) {
    std::string quoted = "\"";
    for (const char c : name) {
        quoted += c;
        if (c == '"') {
            quoted += '"';
        }
    }
    quoted += '"';
    return quoted;
}

/// The size of the file at `path`, 0 when there is none.
std::uint64_t fileBytes(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(core::extendedLengthPath(path).c_str(), GetFileExInfoStandard, &data) == FALSE) {
        return 0;
    }
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::filesystem::path withSuffix(const std::filesystem::path& path, std::wstring_view suffix) {
    std::filesystem::path result = path;
    result += suffix;
    return result;
}

/// A check that SQLite stopped because the file is damaged (ParseError: SQLITE_CORRUPT, SQLITE_NOTADB) found a
/// problem: it is recorded in `check`. Any other error means the check could not run, and is returned.
Status noteDamage(DatabaseCheck& check, const Error& error) {
    if (error.code != ErrorCode::ParseError) {
        return propagate(error);
    }
    check.passed = false;
    ++check.problems;
    check.details.push_back("SQLite cannot read the file (damaged): " + error.message);
    return ok();
}

/// The FTS5 tables of the schema, by name.
Expected<std::vector<std::string>> fts5Tables(Connection& connection) {
    auto tables = connection.prepare("SELECT name FROM sqlite_schema WHERE type = 'table' AND "
                                     "sql LIKE 'CREATE VIRTUAL TABLE % USING fts5%' ORDER BY name");
    if (!tables) {
        return propagate(tables.error());
    }
    std::vector<std::string> names;
    while (true) {
        const auto row = tables->step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            return names;
        }
        names.emplace_back(tables->columnText(0));
    }
}

} // namespace

std::vector<Migration> programMigrations() {
    const std::array<schema::Area, 4> areas = schema::areas();
    const int last = programSchemaVersion();
    std::vector<Migration> migrations;
    for (int version = 1; version <= last; ++version) {
        std::vector<schema::AreaStep> steps;
        std::string names;
        bool rebuildsTables = false;
        for (const schema::Area& area : areas) {
            bool named = false;
            for (const schema::AreaStep& step : area.steps) {
                if (step.version != version) {
                    continue;
                }
                steps.push_back(step);
                rebuildsTables = rebuildsTables || step.rebuildsTables;
                if (!named) {
                    names += names.empty() ? "" : ", ";
                    names += area.name;
                    named = true;
                }
            }
        }
        Migration migration;
        migration.version = version;
        migration.name = fmt::format("schema v{}: {}", version, names.empty() ? std::string("no change") : names);
        migration.rebuildsTables = rebuildsTables;
        migration.apply = [steps = std::move(steps)](Connection& connection) -> Status {
            for (const schema::AreaStep& step : steps) {
                if (!step.sql.empty()) {
                    if (auto ran = connection.execute(step.sql); !ran) {
                        return ran;
                    }
                }
                if (step.apply != nullptr) {
                    if (auto applied = step.apply(connection); !applied) {
                        return applied;
                    }
                }
            }
            return ok();
        };
        migrations.push_back(std::move(migration));
    }
    return migrations;
}

int programSchemaVersion() {
    int last = 0;
    for (const schema::Area& area : schema::areas()) {
        for (const schema::AreaStep& step : area.steps) {
            last = std::max(last, step.version);
        }
    }
    return last;
}

std::filesystem::path backupFolderOf(const std::filesystem::path& databaseFile) {
    return databaseFile.parent_path() / L"backups";
}

std::vector<std::filesystem::path> listBackups(const std::filesystem::path& databaseFile) {
    std::vector<std::filesystem::path> backups;
    for (auto& [path, name] : findBackups(backupFolderOf(databaseFile), databaseFile.stem().native())) {
        backups.push_back(std::move(path));
    }
    return backups;
}

Expected<MigrationReport> migrate(Database& database, const MigrationOptions& options) {
    std::vector<Migration> program;
    std::span<const Migration> migrations = options.migrations;
    if (migrations.empty()) {
        program = programMigrations();
        migrations = program;
    }
    if (auto valid = checkMigrations(migrations); !valid) {
        return propagate(valid.error());
    }
    const int target = migrations.back().version;
    auto version = database.schemaVersion();
    if (!version) {
        return propagate(version.error());
    }
    MigrationReport report{*version, *version, std::nullopt, {}};
    if (*version > target) {
        return propagate(newerDatabase(*version, target, database.path()));
    }
    if (*version == target) {
        return report;
    }

    // No other Retroverse process may have the database open while it changes; others see the change once they open it.
    const auto exclusive = makeExclusive(database, target, options);
    if (!exclusive) {
        return propagate(exclusive.error());
    }
    struct SharedAgain {
        DatabaseLock& lock;
        ~SharedAgain() { lock.downgrade(); }
    } sharedAgain{database.lock()};

    // Another process may have migrated it meanwhile (this one then holds the lock shared, with nothing left to do).
    version = database.schemaVersion();
    if (!version) {
        return propagate(version.error());
    }
    report.fromVersion = report.toVersion = *version;
    if (*version > target) {
        return propagate(newerDatabase(*version, target, database.path()));
    }
    if (*version == target) {
        return report;
    }

    const auto objects = database.writer().queryInt("SELECT count(*) FROM sqlite_schema");
    if (!objects) {
        return propagate(objects.error());
    }
    if (objects->value_or(0) > 0) {
        if (auto backedUp = backUp(database, *version, options, report); !backedUp) {
            return propagate(backedUp.error());
        }
    }
    for (const Migration& migration : migrations) {
        if (migration.version <= report.toVersion) {
            continue;
        }
        if (auto migrated = runMigration(database.writer(), migration); !migrated) {
            spdlog::error("Catalog database {}: {} (it stays at schema version {})", lossy(database.path()),
                          migrated.error().message, report.toVersion);
            return propagate(migrated.error());
        }
        report.toVersion = migration.version;
        spdlog::info("Catalog database {}: migrated to schema version {} ({})", lossy(database.path()),
                     migration.version, migration.name);
    }
    return report;
}

Expected<Database> openCatalogDatabase(const std::filesystem::path& file, const DatabaseOptions& options) {
    auto database = Database::open(file, options);
    if (!database) {
        return propagate(database.error());
    }
    const auto report = migrate(*database);
    if (!report) {
        return propagate(report.error());
    }
    return std::move(*database);
}

Expected<DatabaseCheckReport> checkDatabase(Database& database) {
    Connection& connection = database.writer();
    DatabaseCheckReport report;
    report.programSchemaVersion = programSchemaVersion();
    const auto version = connection.userVersion();
    if (!version) {
        return propagate(version.error());
    }
    report.schemaVersion = *version;

    // SQLite's own check of the file: pages, indexes, constraints.
    DatabaseCheck integrity{"integrity", true, 0, {}};
    {
        // At most kMaxDetails problems (SQLite stops there), a line each.
        std::vector<std::string> rows;
        std::optional<Error> stopped;
        auto check = connection.prepare(fmt::format("PRAGMA integrity_check({})", kMaxDetails));
        if (!check) {
            stopped = check.error();
        }
        while (check) {
            const auto row = check->step();
            if (!row) {
                stopped = row.error();
                break;
            }
            if (!*row) {
                break;
            }
            // A row may hold several problems, a line each, the first after a "*** in database main ***" line.
            std::string_view text = check->columnText(0);
            while (!text.empty()) {
                const std::size_t end = std::min(text.find('\n'), text.size());
                if (const std::string_view line = text.substr(0, end);
                    !line.empty() && !line.starts_with("*** in database ")) {
                    rows.emplace_back(line);
                }
                text.remove_prefix(std::min(end + 1, text.size()));
            }
        }
        if (rows.size() == 1 && rows.front() == "ok" && !stopped) {
            integrity.details.push_back("ok");
        } else {
            integrity.passed = false;
            integrity.problems = rows.size();
            integrity.details = std::move(rows);
        }
        if (stopped) {
            if (auto noted = noteDamage(integrity, *stopped); !noted) {
                return propagate(noted.error());
            }
        }
    }
    report.checks.push_back(std::move(integrity));

    DatabaseCheck foreignKeys{"foreign-keys", true, 0, {}};
    if (auto violations = foreignKeyViolations(connection, kMaxDetails); !violations) {
        if (auto noted = noteDamage(foreignKeys, violations.error()); !noted) {
            return propagate(noted.error());
        }
    } else {
        foreignKeys.problems = violations->second;
        foreignKeys.passed = foreignKeys.problems == 0;
        foreignKeys.details = std::move(violations->first);
        if (foreignKeys.passed) {
            foreignKeys.details.push_back("every row's references exist");
        }
    }
    report.checks.push_back(std::move(foreignKeys));

    // FTS5 compiled in, and FTS5's own check of every FTS5 table (the search index of I2 step 9).
    DatabaseCheck fts5{"fts5", sqliteHasFts5(), 0, {}};
    if (!fts5.passed) {
        fts5.problems = 1;
        fts5.details.push_back("SQLite was built without FTS5, which the catalog's search needs");
    } else if (auto listed = fts5Tables(connection); !listed) {
        if (auto noted = noteDamage(fts5, listed.error()); !noted) {
            return propagate(noted.error());
        }
    } else {
        const std::vector<std::string>& names = *listed;
        for (const std::string& name : names) {
            const std::string table = quoteIdentifier(name);
            if (auto checked =
                    connection.execute(fmt::format("INSERT INTO {0} ({0}) VALUES ('integrity-check')", table));
                !checked) {
                fts5.passed = false;
                ++fts5.problems;
                fts5.details.push_back(fmt::format("{}: {}", name, checked.error().message));
            }
        }
        if (fts5.passed) {
            fts5.details.push_back(fmt::format("compiled in; {} FTS5 table(s) checked", names.size()));
        }
    }
    report.checks.push_back(std::move(fts5));

    const bool sameVersion = report.schemaVersion == report.programSchemaVersion;
    DatabaseCheck schemaVersion{"schema-version", sameVersion, sameVersion ? 0u : 1u, {}};
    if (report.schemaVersion < report.programSchemaVersion) {
        schemaVersion.details.push_back(
            fmt::format("schema version {} is older than this program's {}: migrate it (retroverse-cli db migrate)",
                        report.schemaVersion, report.programSchemaVersion));
    } else if (report.schemaVersion > report.programSchemaVersion) {
        schemaVersion.details.push_back(
            fmt::format("schema version {} is newer than this program's {}: it was written by a newer Retroverse",
                        report.schemaVersion, report.programSchemaVersion));
    } else {
        schemaVersion.details.push_back(fmt::format("schema version {}", report.schemaVersion));
    }
    report.checks.push_back(std::move(schemaVersion));

    // The schema itself, against the one the program's migrations make at the same version, in a private database.
    DatabaseCheck schema{"schema", true, 0, {}};
    if (report.schemaVersion != report.programSchemaVersion) {
        schema.details.push_back("not compared: the schema version differs from this program's");
    } else {
        auto expected = Connection::openMemory();
        if (!expected) {
            return propagate(expected.error());
        }
        for (const Migration& migration : programMigrations()) {
            if (migration.version > report.schemaVersion) {
                break;
            }
            if (auto built = runMigration(*expected, migration); !built) {
                return propagate(withPrefix(built.error(), "cannot build this program's schema to compare with"));
            }
        }
        const auto wanted = schemaObjects(*expected);
        if (!wanted) {
            return propagate(wanted.error());
        }
        const auto found = schemaObjects(connection);
        if (!found) {
            if (auto noted = noteDamage(schema, found.error()); !noted) {
                return propagate(noted.error());
            }
            report.checks.push_back(std::move(schema));
            return report;
        }
        for (const auto& [name, object] : *wanted) {
            const auto it = found->find(name);
            if (it == found->end()) {
                schema.details.push_back(fmt::format("{} {} is missing", object.type, name));
            } else if (it->second != object) {
                schema.details.push_back(
                    fmt::format("{} {} differs from this program's schema version {} (a database made by an "
                                "earlier build: delete it)",
                                object.type, name, report.programSchemaVersion));
            }
        }
        for (const auto& [name, object] : *found) {
            if (!wanted->contains(name)) {
                schema.details.push_back(fmt::format("{} {} is not part of this program's schema version {}",
                                                     object.type, name, report.programSchemaVersion));
            }
        }
        schema.problems = schema.details.size();
        schema.passed = schema.problems == 0;
        if (schema.passed) {
            schema.details.push_back(
                fmt::format("{} tables, indexes, triggers and views as this program makes them", wanted->size()));
        }
    }
    report.checks.push_back(std::move(schema));
    return report;
}

Expected<DatabaseInfo> describeDatabase(Database& database) {
    Connection& connection = database.writer();
    DatabaseInfo info;
    info.file = database.path();
    info.sqliteVersion = std::string(sqliteVersion());
    info.programSchemaVersion = programSchemaVersion();
    const auto version = connection.userVersion();
    if (!version) {
        return propagate(version.error());
    }
    info.schemaVersion = *version;
    const auto journalMode = connection.queryText("PRAGMA journal_mode");
    if (!journalMode) {
        return propagate(journalMode.error());
    }
    info.journalMode = journalMode->value_or("");
    for (const auto& [pragma, value] : {std::pair<std::string_view, std::int64_t*>{"page_size", &info.pageSize},
                                        {"page_count", &info.pageCount},
                                        {"freelist_count", &info.freePages}}) {
        const auto read = connection.queryInt(fmt::format("PRAGMA {}", pragma));
        if (!read) {
            return propagate(read.error());
        }
        *value = read->value_or(0);
    }
    info.databaseBytes = fileBytes(info.file);
    info.walBytes = fileBytes(withSuffix(info.file, L"-wal"));
    info.sharedMemoryBytes = fileBytes(withSuffix(info.file, L"-shm"));

    auto tables = connection.prepare("SELECT name, type FROM pragma_table_list WHERE schema = 'main' AND "
                                     "type IN ('table', 'virtual') AND name NOT GLOB 'sqlite_*' ORDER BY name");
    if (!tables) {
        return propagate(tables.error());
    }
    std::vector<std::string> names;
    while (true) {
        const auto row = tables->step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            break;
        }
        names.emplace_back(tables->columnText(0));
    }
    tables->reset();
    for (const std::string& name : names) {
        const auto rows = connection.queryInt(fmt::format("SELECT count(*) FROM {}", quoteIdentifier(name)));
        if (!rows) {
            return propagate(rows.error());
        }
        info.tables.push_back(TableRows{name, rows->value_or(0)});
        if (name == "rv_meta") {
            auto meta = connection.prepare("SELECT key, value FROM rv_meta ORDER BY key");
            if (!meta) {
                return propagate(meta.error());
            }
            while (true) {
                const auto row = meta->step();
                if (!row) {
                    return propagate(row.error());
                }
                if (!*row) {
                    break;
                }
                info.meta.emplace_back(std::string(meta->columnText(0)), std::string(meta->columnText(1)));
            }
        }
    }
    info.backups = listBackups(info.file);
    return info;
}

} // namespace rv::catalog_db
