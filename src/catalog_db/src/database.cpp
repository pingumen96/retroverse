#include <rv/catalog_db/database.hpp>

#include <algorithm>
#include <climits>
#include <system_error>
#include <thread>
#include <utility>

#include <windows.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::catalog_db {
namespace {

// --- SQLite handles and errors ---------------------------------------------------------------------------------------

sqlite3* raw(detail::ConnectionHandle* handle) noexcept {
    return reinterpret_cast<sqlite3*>(handle);
}
sqlite3_stmt* raw(detail::StatementHandle* handle) noexcept {
    return reinterpret_cast<sqlite3_stmt*>(handle);
}
detail::ConnectionHandle* opaque(sqlite3* connection) noexcept {
    return reinterpret_cast<detail::ConnectionHandle*>(connection);
}
detail::StatementHandle* opaque(sqlite3_stmt* statement) noexcept {
    return reinterpret_cast<detail::StatementHandle*>(statement);
}

/// SQLite's Windows VFS with the long path limit: it differs from the default "win32" only in the length of the names
/// it accepts (32,767 UTF-16 code units instead of MAX_PATH), and the paths that need it are given in their \\?\ form.
constexpr const char* kVfs = "win32-longpath";

/// The longest suffix SQLite appends to a database's name for the files beside it ("-journal"; "-wal" and "-shm" are
/// shorter).
constexpr std::size_t kLongestSuffix = 8;

/// The longest SQL text an error names as its context.
constexpr std::size_t kSqlContextLength = 160;

/// The category of an SQLite result code (extended codes included).
ErrorCode errorCodeOf(int result) noexcept {
    switch (result) {
    case SQLITE_CONSTRAINT_FOREIGNKEY:
        return ErrorCode::InvalidArgument; // a row refers to one that does not exist
    case SQLITE_CONSTRAINT_UNIQUE:
    case SQLITE_CONSTRAINT_PRIMARYKEY:
    case SQLITE_CONSTRAINT_ROWID:
        return ErrorCode::Conflict; // a row clashes with one that exists
    default:
        break;
    }
    switch (result & 0xFF) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return ErrorCode::Conflict;
    case SQLITE_CONSTRAINT:
        return ErrorCode::ValidationFailed; // CHECK, NOT NULL, the type of a STRICT column
    case SQLITE_INTERRUPT:
        return ErrorCode::Cancelled;
    case SQLITE_NOTFOUND:
        return ErrorCode::NotFound;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_FORMAT:
        return ErrorCode::ParseError;
    case SQLITE_ERROR:
    case SQLITE_MISUSE:
    case SQLITE_RANGE:
    case SQLITE_MISMATCH:
    case SQLITE_TOOBIG:
        return ErrorCode::InvalidArgument;
    case SQLITE_FULL:
    case SQLITE_IOERR:
    case SQLITE_CANTOPEN:
    case SQLITE_PERM:
    case SQLITE_READONLY:
    case SQLITE_PROTOCOL:
    case SQLITE_NOLFS:
    case SQLITE_AUTH:
        return ErrorCode::IoError;
    default:
        return ErrorCode::Unknown;
    }
}

/// An SQL text as an error's context: its runs of white space made one space, cut at a character boundary.
std::string sqlContext(std::string_view sql) {
    std::string text;
    bool space = false;
    for (const char c : sql) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            space = !text.empty();
            continue;
        }
        if (space) {
            text += ' ';
            space = false;
        }
        if (text.size() >= kSqlContextLength && (static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            text += "...";
            break;
        }
        text += c;
    }
    return text;
}

/// The error of a failed SQLite call on `connection` (null: the code's generic text), with SQLite's message: read it
/// right after the call, before any other call on the connection.
Error sqliteError(sqlite3* connection, int result, std::string_view what, std::string context) {
    const char* message = connection != nullptr ? sqlite3_errmsg(connection) : sqlite3_errstr(result);
    return Error{
        errorCodeOf(result),
        fmt::format("{}: {} (SQLite result code {})", what, message != nullptr ? message : "unknown error", result),
        std::move(context)};
}

int milliseconds(std::chrono::milliseconds duration) noexcept {
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(duration.count(), 0, INT_MAX));
}

// --- Files -----------------------------------------------------------------------------------------------------------

std::string lossy(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
}

/// SQLite's name of a database file: its absolute path in UTF-8, in its \\?\ form when the files SQLite keeps beside
/// it would otherwise pass MAX_PATH (the Win32 file functions then accept any length, whatever the system's long path
/// setting). Errors: InvalidArgument for a path that is not valid Unicode.
Expected<std::string> sqliteFileName(const std::filesystem::path& absolute) {
    std::wstring name = absolute.native();
    if (name.size() + kLongestSuffix >= MAX_PATH) {
        name = core::extendedLengthPath(absolute);
    }
    auto utf8 = core::wideToUtf8(name);
    if (!utf8) {
        return makeError(ErrorCode::InvalidArgument, "the database path is not valid Unicode", lossy(absolute));
    }
    return utf8;
}

enum class PathKind : std::uint8_t { Missing, File, Folder };

/// What is at `path` (any length). Errors: what the file system says, a missing drive or share Unavailable.
Expected<PathKind> pathKind(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(core::extendedLengthPath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        const ErrorCode code = core::fileErrorCode(error, path);
        if (code == ErrorCode::NotFound) {
            return PathKind::Missing;
        }
        return makeError(code, "cannot query the file: " + core::systemErrorMessage(error), lossy(path));
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? PathKind::Folder : PathKind::File;
}

/// Creates `folder` and the folders above it (any length); existing ones are kept.
Status createFolders(const std::filesystem::path& folder) {
    const auto kind = pathKind(folder);
    if (!kind) {
        return propagate(kind.error());
    }
    if (*kind == PathKind::Folder) {
        return ok();
    }
    if (*kind == PathKind::File) {
        return makeError(ErrorCode::InvalidArgument, "a file stands where a folder must be created", lossy(folder));
    }
    const std::filesystem::path parent = folder.parent_path();
    if (!parent.empty() && parent != folder) {
        if (auto created = createFolders(parent); !created) {
            return created;
        }
    }
    if (CreateDirectoryW(core::extendedLengthPath(folder).c_str(), nullptr) == FALSE) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return makeError(core::fileErrorCode(error, folder),
                             "cannot create the folder: " + core::systemErrorMessage(error), lossy(folder));
        }
    }
    return ok();
}

/// Deletes the file `path` (any length); nothing when it does not exist.
Status removeFile(const std::filesystem::path& path) {
    if (DeleteFileW(core::extendedLengthPath(path).c_str()) == FALSE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return makeError(core::fileErrorCode(error, path),
                             "cannot delete the file: " + core::systemErrorMessage(error), lossy(path));
        }
    }
    return ok();
}

/// Writes what the system caches of the file `path` to its disk (FlushFileBuffers).
Status flushFile(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(core::extendedLengthPath(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return makeError(core::fileErrorCode(error, path), "cannot open the file: " + core::systemErrorMessage(error),
                         lossy(path));
    }
    const bool flushed = FlushFileBuffers(file) != FALSE;
    const DWORD error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!flushed) {
        return makeError(core::fileErrorCode(error),
                         "cannot write the file to its disk: " + core::systemErrorMessage(error), lossy(path));
    }
    return ok();
}

Error withContext(Error error, std::string context) {
    error.context = std::move(context);
    return error;
}

/// `path` made absolute against the current folder and normalized.
Expected<std::filesystem::path> absolutePath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return makeError(ErrorCode::InvalidArgument, "cannot make the path absolute: " + ec.message(), lossy(path));
    }
    return absolute.lexically_normal();
}

// --- The process-level lock ------------------------------------------------------------------------------------------

/// The bytes of the lock file (DatabaseLock).
constexpr DWORD kGateByte = 0;
constexpr DWORD kLockByte = 1;
constexpr DWORD kImportByte = 2;
/// Between two attempts on a byte another holder has.
constexpr auto kLockRetry = std::chrono::milliseconds(5);

HANDLE handleOf(void* file) noexcept {
    return static_cast<HANDLE>(file);
}

/// LockFileEx on one byte of `file`, without waiting: true when taken, false when another holder has it.
Expected<bool> tryLockByte(HANDLE file, DWORD offset, bool exclusive, const std::filesystem::path& path) {
    OVERLAPPED overlapped{};
    overlapped.Offset = offset;
    const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (LockFileEx(file, flags, 0, 1, 0, &overlapped) != FALSE) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_LOCK_VIOLATION) {
        return false;
    }
    return makeError(core::fileErrorCode(error),
                     "cannot lock the database's lock file: " + core::systemErrorMessage(error), lossy(path));
}

void unlockByte(HANDLE file, DWORD offset) noexcept {
    OVERLAPPED overlapped{};
    overlapped.Offset = offset;
    UnlockFileEx(file, 0, 1, 0, &overlapped);
}

} // namespace

// --- Free functions --------------------------------------------------------------------------------------------------

std::filesystem::path databaseFile(const core::Paths& paths) {
    return paths.databaseDir() / core::pathFromUtf8(kDatabaseFileName);
}

std::filesystem::path lockFileOf(const std::filesystem::path& databaseFile) {
    std::filesystem::path lock = databaseFile;
    lock += L".lock";
    return lock;
}

bool sqliteHasFts5() noexcept {
    return sqlite3_compileoption_used("ENABLE_FTS5") != 0;
}

std::string_view sqliteVersion() noexcept {
    return sqlite3_libversion();
}

// --- Statement -------------------------------------------------------------------------------------------------------

Statement::Statement(Statement&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)), m_bindError(std::exchange(other.m_bindError, std::nullopt)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (m_handle != nullptr) {
            sqlite3_finalize(raw(m_handle));
        }
        m_handle = std::exchange(other.m_handle, nullptr);
        m_bindError = std::exchange(other.m_bindError, std::nullopt);
    }
    return *this;
}

Statement::~Statement() {
    if (m_handle != nullptr) {
        sqlite3_finalize(raw(m_handle));
    }
}

std::string_view Statement::sql() const noexcept {
    const char* text = m_handle != nullptr ? sqlite3_sql(raw(m_handle)) : nullptr;
    return text != nullptr ? std::string_view(text) : std::string_view();
}

void Statement::noteBindResult(int index, int result) noexcept {
    if (result == SQLITE_OK || m_bindError) {
        return;
    }
    m_bindError = sqliteError(m_handle != nullptr ? sqlite3_db_handle(raw(m_handle)) : nullptr, result,
                              fmt::format("cannot bind parameter {}", index), sqlContext(sql()));
}

Statement& Statement::bindInt(int index, std::int64_t value) noexcept {
    noteBindResult(index, m_handle != nullptr ? sqlite3_bind_int64(raw(m_handle), index, value) : SQLITE_MISUSE);
    return *this;
}

Statement& Statement::bindReal(int index, double value) noexcept {
    noteBindResult(index, m_handle != nullptr ? sqlite3_bind_double(raw(m_handle), index, value) : SQLITE_MISUSE);
    return *this;
}

Statement& Statement::bindText(int index, std::string_view text) noexcept {
    // A null pointer would bind NULL: an empty text is "".
    noteBindResult(index, m_handle != nullptr
                              ? sqlite3_bind_text64(raw(m_handle), index, text.data() != nullptr ? text.data() : "",
                                                    text.size(), SQLITE_TRANSIENT, SQLITE_UTF8)
                              : SQLITE_MISUSE);
    return *this;
}

Statement& Statement::bindBlob(int index, std::span<const std::byte> bytes) noexcept {
    int result = SQLITE_MISUSE;
    if (m_handle != nullptr) {
        // A null pointer would bind NULL: an empty blob is a zero-length one.
        result = bytes.empty()
                     ? sqlite3_bind_zeroblob(raw(m_handle), index, 0)
                     : sqlite3_bind_blob64(raw(m_handle), index, bytes.data(), bytes.size(), SQLITE_TRANSIENT);
    }
    noteBindResult(index, result);
    return *this;
}

Statement& Statement::bindNull(int index) noexcept {
    noteBindResult(index, m_handle != nullptr ? sqlite3_bind_null(raw(m_handle), index) : SQLITE_MISUSE);
    return *this;
}

Statement& Statement::bindOptionalInt(int index, const std::optional<std::int64_t>& value) noexcept {
    return value ? bindInt(index, *value) : bindNull(index);
}

Statement& Statement::bindOptionalText(int index, const std::optional<std::string_view>& text) noexcept {
    return text ? bindText(index, *text) : bindNull(index);
}

Expected<bool> Statement::step() {
    if (m_handle == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "no prepared statement");
    }
    if (m_bindError) {
        return propagate(*m_bindError);
    }
    const int result = sqlite3_step(raw(m_handle));
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result == SQLITE_DONE) {
        return false;
    }
    return propagate(
        sqliteError(sqlite3_db_handle(raw(m_handle)), result, "the SQL statement failed", sqlContext(sql())));
}

Status Statement::run() {
    const ScopedReset resetWhenDone(*this);
    while (true) {
        const auto row = step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            return ok();
        }
    }
}

void Statement::reset() noexcept {
    if (m_handle != nullptr) {
        sqlite3_reset(raw(m_handle));
        sqlite3_clear_bindings(raw(m_handle));
    }
    m_bindError.reset();
}

int Statement::columnCount() const noexcept {
    return m_handle != nullptr ? sqlite3_column_count(raw(m_handle)) : 0;
}

ColumnType Statement::columnType(int column) const noexcept {
    if (m_handle == nullptr) {
        return ColumnType::Null;
    }
    switch (sqlite3_column_type(raw(m_handle), column)) {
    case SQLITE_INTEGER:
        return ColumnType::Integer;
    case SQLITE_FLOAT:
        return ColumnType::Real;
    case SQLITE_TEXT:
        return ColumnType::Text;
    case SQLITE_BLOB:
        return ColumnType::Blob;
    default:
        return ColumnType::Null;
    }
}

std::int64_t Statement::columnInt(int column) const noexcept {
    return m_handle != nullptr ? sqlite3_column_int64(raw(m_handle), column) : 0;
}

double Statement::columnReal(int column) const noexcept {
    return m_handle != nullptr ? sqlite3_column_double(raw(m_handle), column) : 0.0;
}

std::string_view Statement::columnText(int column) const noexcept {
    if (m_handle == nullptr) {
        return {};
    }
    // The text first, then its length in bytes (SQLite's documented order).
    const unsigned char* text = sqlite3_column_text(raw(m_handle), column);
    if (text == nullptr) {
        return {};
    }
    return std::string_view(reinterpret_cast<const char*>(text),
                            static_cast<std::size_t>(sqlite3_column_bytes(raw(m_handle), column)));
}

std::span<const std::byte> Statement::columnBlob(int column) const noexcept {
    if (m_handle == nullptr) {
        return {};
    }
    const void* bytes = sqlite3_column_blob(raw(m_handle), column);
    if (bytes == nullptr) {
        return {};
    }
    return std::span<const std::byte>(static_cast<const std::byte*>(bytes),
                                      static_cast<std::size_t>(sqlite3_column_bytes(raw(m_handle), column)));
}

std::optional<std::int64_t> Statement::columnOptionalInt(int column) const noexcept {
    if (isNull(column)) {
        return std::nullopt;
    }
    return columnInt(column);
}

// --- Connection ------------------------------------------------------------------------------------------------------

Connection::Connection(detail::ConnectionHandle* handle, ConnectionRole role, std::filesystem::path path,
                       std::chrono::milliseconds busyTimeout, std::shared_ptr<const void> keepAlive) noexcept
    : m_handle(handle), m_role(role), m_path(std::move(path)), m_busyTimeout(busyTimeout),
      m_keepAlive(std::move(keepAlive)) {}

Connection::Connection(Connection&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)), m_role(other.m_role), m_path(std::move(other.m_path)),
      m_busyTimeout(other.m_busyTimeout), m_keepAlive(std::move(other.m_keepAlive)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_role = other.m_role;
        m_path = std::move(other.m_path);
        m_busyTimeout = other.m_busyTimeout;
        m_keepAlive = std::move(other.m_keepAlive);
    }
    return *this;
}

Connection::~Connection() {
    close();
}

void Connection::close() noexcept {
    if (m_handle != nullptr) {
        // close_v2: statements still alive keep the connection until they are finalized, instead of failing the close.
        sqlite3_close_v2(raw(m_handle));
        m_handle = nullptr;
    }
    m_keepAlive.reset();
}

Expected<Connection> Connection::open(const std::filesystem::path& file, ConnectionRole role, bool create,
                                      std::chrono::milliseconds busyTimeout, std::shared_ptr<const void> keepAlive) {
    std::string name = ":memory:";
    std::string context = name;
    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    switch (role) {
    case ConnectionRole::Writer:
        flags |= SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
        break;
    case ConnectionRole::Reader:
        flags |= SQLITE_OPEN_READONLY;
        break;
    case ConnectionRole::Memory:
        // The name ":memory:" makes it private and in memory. Not SQLITE_OPEN_MEMORY: that flag stays with the
        // connection and would make the files it ATTACHes (VACUUM INTO's output) in-memory databases too.
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        break;
    }
    if (role != ConnectionRole::Memory) {
        auto sqliteName = sqliteFileName(file);
        if (!sqliteName) {
            return propagate(sqliteName.error());
        }
        name = std::move(*sqliteName);
        context = lossy(file);
    }

    sqlite3* handle = nullptr;
    const int result = sqlite3_open_v2(name.c_str(), &handle, flags, kVfs);
    if (result != SQLITE_OK) {
        Error error = sqliteError(handle, result, "cannot open the database", context);
        sqlite3_close_v2(handle); // a failed open may still have made a handle
        return propagate(std::move(error));
    }
    // Defensive mode: no SQL may corrupt the file on purpose (PRAGMA writable_schema, journal_mode=OFF, writes to
    // shadow tables): later migrations rebuild tables by SQLite's documented procedure instead (migrator.hpp).
    sqlite3_db_config(handle, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_busy_timeout(handle, milliseconds(busyTimeout));
    Connection connection(opaque(handle), role, role == ConnectionRole::Memory ? std::filesystem::path() : file,
                          busyTimeout, std::move(keepAlive));
    if (auto enforced = connection.execute("PRAGMA foreign_keys = ON"); !enforced) {
        return propagate(enforced.error());
    }
    return connection;
}

Expected<Connection> Connection::openMemory() {
    return open({}, ConnectionRole::Memory, true, std::chrono::milliseconds(0), nullptr);
}

Status Connection::execute(std::string_view sql) {
    if (m_handle == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the connection is closed", sqlContext(sql));
    }
    if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::InvalidArgument, "the SQL text is too long", sqlContext(sql));
    }
    // Statement by statement (what sqlite3_exec does), so that an error names the statement that failed.
    const char* next = sql.data();
    const char* const end = sql.data() + sql.size();
    while (next < end) {
        sqlite3_stmt* handle = nullptr;
        const char* tail = nullptr;
        const int prepared = sqlite3_prepare_v3(raw(m_handle), next, static_cast<int>(end - next), 0, &handle, &tail);
        if (prepared != SQLITE_OK) {
            return propagate(sqliteError(raw(m_handle), prepared, "cannot prepare the SQL statement",
                                         sqlContext(std::string_view(next, static_cast<std::size_t>(end - next)))));
        }
        const Statement statement(opaque(handle)); // finalized at the end of the iteration; none for white space
        if (handle != nullptr) {
            int result = SQLITE_ROW;
            while (result == SQLITE_ROW) {
                result = sqlite3_step(handle);
            }
            if (result != SQLITE_DONE) {
                return propagate(
                    sqliteError(raw(m_handle), result, "the SQL statement failed", sqlContext(statement.sql())));
            }
        }
        if (tail == nullptr || tail <= next) {
            break;
        }
        next = tail;
    }
    return ok();
}

Expected<Statement> Connection::prepare(std::string_view sql) {
    if (m_handle == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the connection is closed", sqlContext(sql));
    }
    if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::InvalidArgument, "the SQL text is too long", sqlContext(sql));
    }
    sqlite3_stmt* handle = nullptr;
    const char* tail = nullptr;
    int result = sqlite3_prepare_v3(raw(m_handle), sql.data(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT,
                                    &handle, &tail);
    if (result != SQLITE_OK) {
        return propagate(sqliteError(raw(m_handle), result, "cannot prepare the SQL statement", sqlContext(sql)));
    }
    Statement statement(opaque(handle));
    if (!statement.valid()) {
        return makeError(ErrorCode::InvalidArgument, "no SQL statement to prepare", sqlContext(sql));
    }
    // What follows the statement must hold no other: preparing it gives none.
    const std::size_t rest = sql.size() - static_cast<std::size_t>(tail - sql.data());
    if (rest > 0) {
        sqlite3_stmt* next = nullptr;
        result = sqlite3_prepare_v3(raw(m_handle), tail, static_cast<int>(rest), 0, &next, nullptr);
        const bool another = result != SQLITE_OK || next != nullptr;
        sqlite3_finalize(next);
        if (another) {
            return makeError(ErrorCode::InvalidArgument, "more than one SQL statement to prepare", sqlContext(sql));
        }
    }
    return statement;
}

Expected<std::optional<std::int64_t>> Connection::queryInt(std::string_view sql) {
    auto statement = prepare(sql);
    if (!statement) {
        return propagate(statement.error());
    }
    const auto row = statement->step();
    if (!row) {
        return propagate(row.error());
    }
    return *row ? statement->columnOptionalInt(0) : std::nullopt;
}

Expected<std::optional<std::string>> Connection::queryText(std::string_view sql) {
    auto statement = prepare(sql);
    if (!statement) {
        return propagate(statement.error());
    }
    const auto row = statement->step();
    if (!row) {
        return propagate(row.error());
    }
    if (!*row || statement->isNull(0)) {
        return std::optional<std::string>();
    }
    return std::optional<std::string>(std::string(statement->columnText(0)));
}

Expected<int> Connection::userVersion() {
    const auto version = queryInt("PRAGMA user_version");
    if (!version) {
        return propagate(version.error());
    }
    return static_cast<int>(version->value_or(0));
}

bool Connection::inTransaction() const noexcept {
    return m_handle != nullptr && sqlite3_get_autocommit(raw(m_handle)) == 0;
}

std::int64_t Connection::lastInsertRowId() const noexcept {
    return m_handle != nullptr ? sqlite3_last_insert_rowid(raw(m_handle)) : 0;
}

std::int64_t Connection::changes() const noexcept {
    return m_handle != nullptr ? sqlite3_changes64(raw(m_handle)) : 0;
}

void Connection::interrupt() noexcept {
    if (m_handle != nullptr) {
        sqlite3_interrupt(raw(m_handle));
    }
}

// --- Transaction -----------------------------------------------------------------------------------------------------

Expected<Transaction> Transaction::begin(Connection& connection, Mode mode) {
    if (connection.inTransaction()) {
        return makeError(ErrorCode::InvalidArgument, "a transaction is open on the connection already");
    }
    if (auto begun = connection.execute(mode == Mode::Immediate ? "BEGIN IMMEDIATE" : "BEGIN"); !begun) {
        return propagate(begun.error());
    }
    return Transaction(connection);
}

Transaction::Transaction(Transaction&& other) noexcept : m_connection(std::exchange(other.m_connection, nullptr)) {}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        rollback();
        m_connection = std::exchange(other.m_connection, nullptr);
    }
    return *this;
}

Transaction::~Transaction() {
    rollback();
}

Status Transaction::commit() {
    if (m_connection == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the transaction has ended");
    }
    auto committed = m_connection->execute("COMMIT");
    if (!committed) {
        rollback(); // an error SQLite did not roll back on its own (a busy COMMIT keeps the transaction open)
    }
    m_connection = nullptr;
    return committed;
}

void Transaction::rollback() noexcept {
    Connection* connection = std::exchange(m_connection, nullptr);
    if (connection != nullptr && connection->inTransaction()) {
        if (auto rolledBack = connection->execute("ROLLBACK"); !rolledBack) {
            spdlog::warn("Catalog database: ROLLBACK failed: {}", rolledBack.error().toString());
        }
    }
}

// --- DatabaseLock ----------------------------------------------------------------------------------------------------

Expected<DatabaseLock> DatabaseLock::acquireShared(const std::filesystem::path& lockFile,
                                                   std::chrono::milliseconds wait) {
    // Read and write access (LockFileEx needs one of them), full sharing: every holder opens the same file.
    const HANDLE file = CreateFileW(core::extendedLengthPath(lockFile).c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return makeError(core::fileErrorCode(error, lockFile),
                         "cannot open the database's lock file: " + core::systemErrorMessage(error), lossy(lockFile));
    }
    DatabaseLock lock(file, lockFile);
    if (auto joined = lock.join(wait); !joined) {
        return propagate(joined.error());
    }
    return lock;
}

Status DatabaseLock::join(std::chrono::milliseconds wait) {
    const HANDLE file = handleOf(m_file);
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (true) {
        // The gate for an instant: a holder that migrates keeps it until it is done.
        const auto gate = tryLockByte(file, kGateByte, true, m_path);
        if (!gate) {
            return propagate(gate.error());
        }
        if (*gate) {
            // Nobody holds the lock exclusive without the gate: the shared lock is free to take.
            const auto shared = tryLockByte(file, kLockByte, false, m_path);
            unlockByte(file, kGateByte);
            if (!shared) {
                return propagate(shared.error());
            }
            if (*shared) {
                m_held = true;
                m_mode = LockMode::Shared;
                return ok();
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return makeError(ErrorCode::Conflict, "another Retroverse process is migrating the catalog database",
                             lossy(m_path));
        }
        std::this_thread::sleep_for(kLockRetry);
    }
}

DatabaseLock::DatabaseLock(DatabaseLock&& other) noexcept
    : m_file(std::exchange(other.m_file, nullptr)), m_path(std::move(other.m_path)), m_mode(other.m_mode),
      m_held(std::exchange(other.m_held, false)), m_gate(std::exchange(other.m_gate, false)),
      m_import(std::exchange(other.m_import, false)) {}

DatabaseLock& DatabaseLock::operator=(DatabaseLock&& other) noexcept {
    if (this != &other) {
        release();
        m_file = std::exchange(other.m_file, nullptr);
        m_path = std::move(other.m_path);
        m_mode = other.m_mode;
        m_held = std::exchange(other.m_held, false);
        m_gate = std::exchange(other.m_gate, false);
        m_import = std::exchange(other.m_import, false);
    }
    return *this;
}

DatabaseLock::~DatabaseLock() {
    release();
}

void DatabaseLock::release() noexcept {
    if (m_file == nullptr) {
        return;
    }
    // Unlocked explicitly: the system releases the locks of a closed handle only when it gets round to it.
    const HANDLE file = handleOf(m_file);
    if (m_import) {
        unlockByte(file, kImportByte);
    }
    if (m_held) {
        unlockByte(file, kLockByte);
    }
    if (m_gate) {
        unlockByte(file, kGateByte);
    }
    CloseHandle(file);
    m_file = nullptr;
    m_held = m_gate = m_import = false;
}

Expected<DatabaseLock::Upgrade> DatabaseLock::upgrade(std::chrono::milliseconds wait) {
    if (m_file == nullptr || !m_held) {
        return makeError(ErrorCode::InvalidArgument, "the database's lock is not held", lossy(m_path));
    }
    if (m_mode == LockMode::Exclusive) {
        return Upgrade::Exclusive;
    }
    const HANDLE file = handleOf(m_file);
    const auto gate = tryLockByte(file, kGateByte, true, m_path);
    if (!gate) {
        return propagate(gate.error());
    }
    if (!*gate) {
        return Upgrade::GateTaken;
    }
    // With the gate, nobody joins and nobody else can take the lock exclusive: from shared to exclusive, or back to
    // shared while the others have it, until they leave or the wait ends.
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (true) {
        unlockByte(file, kLockByte);
        const auto exclusive = tryLockByte(file, kLockByte, true, m_path);
        if (exclusive && *exclusive) {
            m_mode = LockMode::Exclusive;
            m_gate = true;
            return Upgrade::Exclusive;
        }
        const auto shared = tryLockByte(file, kLockByte, false, m_path);
        if (!shared || !*shared) {
            unlockByte(file, kGateByte);
            m_held = false; // cannot happen: the gate kept every other holder from the exclusive lock
            return makeError(ErrorCode::IoError, "the catalog database's lock was lost", lossy(m_path));
        }
        if (!exclusive) {
            unlockByte(file, kGateByte);
            return propagate(exclusive.error());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            unlockByte(file, kGateByte);
            return makeError(ErrorCode::Conflict, "another Retroverse process has the catalog database open",
                             lossy(m_path));
        }
        std::this_thread::sleep_for(kLockRetry);
    }
}

Status DatabaseLock::rejoin(std::chrono::milliseconds wait) {
    if (m_file == nullptr || !m_held || m_mode != LockMode::Shared) {
        return makeError(ErrorCode::InvalidArgument, "only a holder of the shared lock can leave and join again",
                         lossy(m_path));
    }
    unlockByte(handleOf(m_file), kLockByte);
    m_held = false;
    return join(wait);
}

void DatabaseLock::downgrade() noexcept {
    if (m_file == nullptr || m_mode != LockMode::Exclusive) {
        return;
    }
    const HANDLE file = handleOf(m_file);
    unlockByte(file, kLockByte);
    // Still behind the gate: nobody can take the lock in between.
    const auto shared = tryLockByte(file, kLockByte, false, m_path);
    if (!shared || !*shared) {
        m_held = false;
        spdlog::error("Catalog database: the lock could not be taken back shared after a migration ({})",
                      lossy(m_path));
    }
    unlockByte(file, kGateByte);
    m_gate = false;
    m_mode = LockMode::Shared;
}

Status DatabaseLock::lockImport() {
    if (m_file == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the database's lock is not held", lossy(m_path));
    }
    if (m_import) {
        return makeError(ErrorCode::Conflict, "an import of this process holds the import lock", lossy(m_path));
    }
    const auto taken = tryLockByte(handleOf(m_file), kImportByte, true, m_path);
    if (!taken) {
        return propagate(taken.error());
    }
    if (!*taken) {
        return makeError(ErrorCode::Conflict, "another import holds the import lock", lossy(m_path));
    }
    m_import = true;
    return ok();
}

void DatabaseLock::unlockImport() noexcept {
    if (m_file != nullptr && m_import) {
        unlockByte(handleOf(m_file), kImportByte);
        m_import = false;
    }
}

// --- Database --------------------------------------------------------------------------------------------------------

Database::Database(std::filesystem::path path, std::shared_ptr<DatabaseLock> lock, Connection writer) noexcept
    : m_path(std::move(path)), m_lock(std::move(lock)), m_writer(std::move(writer)) {}

namespace {

/// A file at the database's path that another application wrote is never touched: neither switched to WAL nor
/// migrated. A new database (no tables, no version) is the catalog's to make.
Status checkApplicationId(Connection& writer, const std::string& context) {
    const auto id = writer.queryInt("PRAGMA application_id");
    if (!id) {
        return propagate(id.error()); // a file that is no database at all: ParseError
    }
    const std::int64_t applicationId = id->value_or(0);
    if (applicationId == kApplicationId) {
        return ok();
    }
    if (applicationId != 0) {
        return makeError(ErrorCode::Conflict,
                         fmt::format("not a Retroverse catalog database (application id {:#010x})",
                                     static_cast<std::uint32_t>(applicationId)),
                         context);
    }
    const auto version = writer.userVersion();
    if (!version) {
        return propagate(version.error());
    }
    const auto objects = writer.queryInt("SELECT count(*) FROM sqlite_schema");
    if (!objects) {
        return propagate(objects.error());
    }
    if (*version != 0 || objects->value_or(0) != 0) {
        return makeError(ErrorCode::Conflict,
                         "not a Retroverse catalog database: it holds tables but not Retroverse's application id",
                         context);
    }
    return ok();
}

/// The writer's journal: WAL (readers never wait on it), every commit synced (synchronous FULL: no committed
/// identity or session is lost on a power cut), the WAL cut back after checkpoints (journal_size_limit).
Status configureWriter(Connection& writer, const std::string& context) {
    const auto mode = writer.queryText("PRAGMA journal_mode = WAL");
    if (!mode) {
        return propagate(mode.error());
    }
    if (!mode->has_value() || !core::equalsIgnoreCaseAscii(**mode, "wal")) {
        return makeError(
            ErrorCode::Unsupported,
            fmt::format("the database cannot use WAL journal mode (it stays in '{}' mode)", mode->value_or("")),
            context);
    }
    return writer.execute(
        fmt::format("PRAGMA synchronous = FULL; PRAGMA journal_size_limit = {}; PRAGMA cache_size = {}",
                    kJournalSizeLimitBytes, -kWriterCacheKiB));
}

} // namespace

Expected<Database> Database::open(const std::filesystem::path& file, const DatabaseOptions& options) {
    const auto absolute = absolutePath(file);
    if (!absolute) {
        return propagate(absolute.error());
    }
    const std::string context = lossy(*absolute);
    const auto kind = pathKind(*absolute);
    if (!kind) {
        return propagate(kind.error());
    }
    if (*kind == PathKind::Folder) {
        return makeError(ErrorCode::InvalidArgument, "the database path names a folder", context);
    }
    if (*kind == PathKind::Missing) {
        if (!options.create) {
            return makeError(ErrorCode::NotFound, "the catalog database does not exist", context);
        }
        if (auto created = createFolders(absolute->parent_path()); !created) {
            return propagate(created.error());
        }
    }

    auto lock = DatabaseLock::acquireShared(lockFileOf(*absolute), options.lockWait);
    if (!lock) {
        return propagate(lock.error());
    }
    auto sharedLock = std::make_shared<DatabaseLock>(std::move(*lock));
    auto writer = Connection::open(*absolute, ConnectionRole::Writer, options.create, options.busyTimeout, nullptr);
    if (!writer) {
        return propagate(writer.error());
    }
    if (!sqliteHasFts5()) {
        return makeError(ErrorCode::Unsupported, "SQLite was built without FTS5, which the catalog's search needs",
                         context);
    }
    if (auto ours = checkApplicationId(*writer, context); !ours) {
        return propagate(ours.error());
    }
    if (auto configured = configureWriter(*writer, context); !configured) {
        return propagate(configured.error());
    }
    spdlog::debug("Catalog database opened: {}", context);
    return Database(*absolute, std::move(sharedLock), std::move(*writer));
}

Expected<Connection> Database::openReader() const {
    return Connection::open(m_path, ConnectionRole::Reader, false, std::chrono::milliseconds(0), m_lock);
}

Status Database::checkpoint(std::chrono::milliseconds wait) {
    sqlite3* handle = raw(m_writer.m_handle);
    if (handle == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the connection is closed", lossy(m_path));
    }
    // The checkpoint waits through the busy handler: briefly, then the writer's own timeout again.
    sqlite3_busy_timeout(handle, milliseconds(wait));
    int walFrames = 0;
    int checkpointed = 0;
    const int result =
        sqlite3_wal_checkpoint_v2(handle, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &walFrames, &checkpointed);
    Error error;
    if (result != SQLITE_OK) {
        error = sqliteError(handle, result, "the WAL checkpoint did not complete", lossy(m_path));
    }
    sqlite3_busy_timeout(handle, milliseconds(m_writer.m_busyTimeout));
    if (result != SQLITE_OK) {
        return propagate(std::move(error));
    }
    return ok();
}

Status Database::backupTo(const std::filesystem::path& target) {
    const auto absolute = absolutePath(target);
    if (!absolute) {
        return propagate(absolute.error());
    }
    const std::string context = lossy(*absolute);
    const auto kind = pathKind(*absolute);
    if (!kind) {
        return propagate(kind.error());
    }
    if (*kind != PathKind::Missing) {
        return makeError(ErrorCode::Conflict, "the backup file exists already", context);
    }
    if (auto created = createFolders(absolute->parent_path()); !created) {
        return propagate(created.error());
    }
    // VACUUM INTO refuses a target that holds data: a copy cut short earlier is replaced.
    std::filesystem::path partial = *absolute;
    partial += L".partial";
    if (auto removed = removeFile(partial); !removed) {
        return propagate(removed.error());
    }
    const auto partialName = sqliteFileName(partial);
    if (!partialName) {
        return propagate(partialName.error());
    }
    auto vacuum = m_writer.prepare("VACUUM INTO ?1");
    if (!vacuum) {
        return propagate(vacuum.error());
    }
    vacuum->bindText(1, *partialName);
    if (auto copied = vacuum->run(); !copied) {
        static_cast<void>(removeFile(partial));
        Error error = copied.error();
        error.message = "cannot write the backup: " + error.message;
        error.context = context;
        return propagate(std::move(error));
    }
    // The copy is complete. It reaches the disk before it takes its name, whatever VACUUM INTO did about it: a backup
    // is the one copy of the database as it was before a migration.
    if (auto flushed = flushFile(partial); !flushed) {
        static_cast<void>(removeFile(partial));
        return propagate(withContext(flushed.error(), context));
    }
    if (MoveFileExW(core::extendedLengthPath(partial).c_str(), core::extendedLengthPath(*absolute).c_str(),
                    MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD error = GetLastError();
        static_cast<void>(removeFile(partial));
        return makeError(core::fileErrorCode(error, *absolute),
                         "cannot give the backup its name: " + core::systemErrorMessage(error), context);
    }
    return ok();
}

} // namespace rv::catalog_db
