#include <rv/core/file_io.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <system_error>
#include <utility>

#include <windows.h>

#include <fmt/format.h>

#include <rv/core/system_error.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

/// The error of a failed std::filesystem query: the category of its Windows error code (MSVC's std::filesystem reports
/// them in the system category).
ErrorCode errorCodeOf(const std::error_code& ec) noexcept {
    return ec.category() == std::system_category() ? fileErrorCode(static_cast<std::uint32_t>(ec.value()))
                                                   : ErrorCode::IoError;
}

template <class Container>
Expected<Container> readWholeFile(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    // Messages only: a path that does not convert exactly still names the file well enough, and never throws.
    const std::string context = pathToUtf8Lossy(path);
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        // A missing drive or share is unavailable, not missing.
        const ErrorCode code = fileErrorCode(ERROR_PATH_NOT_FOUND, path);
        return makeError(code, code == ErrorCode::NotFound ? "file does not exist" : "its drive is not available",
                         context);
    }
    if (ec) {
        return makeError(errorCodeOf(ec), "cannot query the file: " + ec.message(), context);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return makeError(ErrorCode::InvalidArgument, "not a regular file", context);
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return makeError(ErrorCode::IoError, "cannot query file size: " + ec.message(), context);
    }
    if (size > maxBytes) {
        return makeError(ErrorCode::OutOfRange,
                         "file is larger than the allowed " + std::to_string(maxBytes) + " bytes", context);
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return makeError(ErrorCode::IoError, "cannot open file for reading", context);
    }
    Container data(static_cast<std::size_t>(size), typename Container::value_type{});
    if (size > 0) {
        stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (stream.gcount() != static_cast<std::streamsize>(size)) {
            return makeError(ErrorCode::IoError, "short read", context);
        }
    }
    return data;
}

std::atomic<unsigned> g_tempCounter{0};

/// Closes a file handle on scope exit.
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) noexcept : m_handle(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (valid()) {
            CloseHandle(m_handle);
        }
    }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }

private:
    HANDLE m_handle;
};

/// Read in pieces of at most this many bytes (ReadFile takes a 32-bit count).
constexpr DWORD kReadChunkBytes = 16u * 1024u * 1024u;

} // namespace

Expected<std::string> readTextFile(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    return readWholeFile<std::string>(path, maxBytes);
}

Expected<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    return readWholeFile<std::vector<std::byte>>(path, maxBytes);
}

std::wstring extendedLengthPath(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\")) {
        return native;
    }
    // absolute() normalizes as Windows would (GetFullPathNameW): "." and "..", separators, trailing dots and spaces.
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::wstring text = absolute.native();
    if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\")) {
        return text; // a device name such as "NUL" became "\\.\NUL"
    }
    text = absolute.lexically_normal().native();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + text.substr(2);
    }
    if (text.size() >= 2 && text[1] == L':') {
        return L"\\\\?\\" + text;
    }
    return text;
}

Expected<std::filesystem::path> extendedLengthPathBelow(const std::filesystem::path& root,
                                                        std::string_view relativeUtf8) {
    const auto fail = [&](std::string_view why) {
        const std::string named = isValidUtf8(relativeUtf8) ? " \"" + std::string(relativeUtf8) + "\"" : "";
        return makeError(ErrorCode::InvalidArgument,
                         "the relative path" + named + " cannot be used: " + std::string(why), pathToUtf8Lossy(root));
    };
    if (relativeUtf8.empty()) {
        return fail("it is empty");
    }
    const auto relative = utf8ToWide(relativeUtf8);
    if (!relative) {
        return fail(relative.error().message);
    }
    std::wstring path = extendedLengthPath(root);
    while (path.size() > 4 && path.ends_with(L'\\')) {
        path.pop_back(); // "\\?\C:\" becomes "\\?\C:", followed by "\" and the first element
    }
    std::size_t start = 0;
    while (start <= relative->size()) {
        std::size_t end = relative->find_first_of(L"/\\", start);
        if (end == std::wstring::npos) {
            end = relative->size();
        }
        const std::wstring_view element = std::wstring_view(*relative).substr(start, end - start);
        if (element.empty()) {
            return fail("it has an empty element");
        }
        if (element == L"." || element == L"..") {
            return fail("\".\" and \"..\" elements are not allowed");
        }
        if (element.find(L':') != std::wstring_view::npos) {
            return fail("':' is not allowed in a name (alternate data streams)");
        }
        if (element.find(L'\0') != std::wstring_view::npos) {
            return fail("it holds a NUL character");
        }
        path += L'\\';
        path += element;
        start = end + 1;
    }
    return std::filesystem::path(std::move(path));
}

Expected<std::string> readFileShared(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    const std::string context = pathToUtf8Lossy(path);
    const std::wstring target = extendedLengthPath(path);
    // Reading only, sharing everything: the program that owns the file may rewrite, rename or delete it meanwhile.
    const FileHandle file(CreateFileW(target.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            // CreateFileW refuses to open a folder this way with "access denied".
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return makeError(ErrorCode::InvalidArgument, "not a regular file (a folder)", context);
            }
        }
        return makeError(fileErrorCode(error, path), "cannot open the file: " + systemErrorMessage(error), context);
    }
    if (GetFileType(file.get()) != FILE_TYPE_DISK) {
        return makeError(ErrorCode::InvalidArgument, "not a regular file (a device)", context);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        const DWORD error = GetLastError();
        return makeError(fileErrorCode(error, path), "cannot query the file size: " + systemErrorMessage(error),
                         context);
    }
    const auto bytes = static_cast<std::uintmax_t>(size.QuadPart);
    if (bytes > maxBytes) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("the file holds {} bytes, more than the allowed {}", bytes, maxBytes), context);
    }

    std::string data(static_cast<std::size_t>(bytes), '\0');
    std::size_t done = 0;
    while (done < data.size()) {
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, kReadChunkBytes));
        DWORD read = 0;
        if (!ReadFile(file.get(), data.data() + done, wanted, &read, nullptr)) {
            // A range another program locked (LockFileEx) is a conflict, a drive unplugged meanwhile unavailable.
            const DWORD error = GetLastError();
            return makeError(fileErrorCode(error, path), "cannot read the file: " + systemErrorMessage(error),
                             context);
        }
        if (read == 0) {
            return makeError(ErrorCode::IoError,
                             fmt::format("short read: {} of {} bytes (the file shrank while it was read)", done,
                                         data.size()),
                             context);
        }
        done += read;
    }
    return data;
}

Expected<SharedFileReader> SharedFileReader::open(const std::filesystem::path& path) {
    std::string context = pathToUtf8Lossy(path);
    const std::wstring target = extendedLengthPath(path);
    // Reading only, sharing everything, as readFileShared(): the program that owns the file is never blocked.
    HANDLE file = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return makeError(ErrorCode::InvalidArgument, "not a regular file (a folder)", std::move(context));
            }
        }
        return makeError(fileErrorCode(error, path), "cannot open the file: " + systemErrorMessage(error),
                         std::move(context));
    }
    SharedFileReader reader(file, std::move(context));
    if (GetFileType(file) != FILE_TYPE_DISK) {
        return makeError(ErrorCode::InvalidArgument, "not a regular file (a device)", reader.m_context);
    }
    return reader;
}

SharedFileReader::SharedFileReader(SharedFileReader&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)), m_context(std::move(other.m_context)) {}

SharedFileReader& SharedFileReader::operator=(SharedFileReader&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_context = std::move(other.m_context);
    }
    return *this;
}

SharedFileReader::~SharedFileReader() {
    close();
}

void SharedFileReader::close() noexcept {
    if (m_handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

Expected<SharedFileReader::Facts> SharedFileReader::facts() const {
    BY_HANDLE_FILE_INFORMATION info{};
    if (m_handle == nullptr || !GetFileInformationByHandle(static_cast<HANDLE>(m_handle), &info)) {
        const DWORD error = m_handle == nullptr ? ERROR_INVALID_HANDLE : GetLastError();
        return makeError(fileErrorCode(error), "cannot query the file: " + systemErrorMessage(error), m_context);
    }
    Facts facts;
    facts.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    facts.lastWriteTime = static_cast<std::int64_t>((static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime)
                                                     << 32) |
                                                    info.ftLastWriteTime.dwLowDateTime);
    return facts;
}

Status SharedFileReader::read(std::uint64_t offset, std::span<std::byte> out) const {
    std::size_t done = 0;
    while (done < out.size()) {
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, kReadChunkBytes));
        // A positioned read on a synchronous handle: the offset in the OVERLAPPED structure, the call waits.
        const std::uint64_t at = offset + done;
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(at & 0xFFFF'FFFFu);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD read = 0;
        if (m_handle == nullptr ||
            !ReadFile(static_cast<HANDLE>(m_handle), out.data() + done, wanted, &read, &position)) {
            const DWORD error = m_handle == nullptr ? ERROR_INVALID_HANDLE : GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                return makeError(ErrorCode::IoError,
                                 fmt::format("short read: {} of {} bytes at offset {} (the file shrank while it was "
                                             "read)",
                                             done, out.size(), offset),
                                 m_context);
            }
            return makeError(fileErrorCode(error), "cannot read the file: " + systemErrorMessage(error), m_context);
        }
        if (read == 0) {
            return makeError(ErrorCode::IoError,
                             fmt::format("short read: {} of {} bytes at offset {} (the file shrank while it was read)",
                                         done, out.size(), offset),
                             m_context);
        }
        done += read;
    }
    return ok();
}

Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data) {
    const std::string context = pathToUtf8Lossy(path);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return makeError(ErrorCode::IoError, "cannot create parent directory: " + ec.message(), context);
        }
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp-" + std::to_string(g_tempCounter.fetch_add(1));
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return makeError(ErrorCode::IoError, "cannot open temporary file for writing", pathToUtf8Lossy(temporary));
        }
        if (!data.empty()) {
            stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return makeError(ErrorCode::IoError, "write failed", pathToUtf8Lossy(temporary));
        }
    }

    // std::filesystem::rename replaces an existing regular file (MoveFileExW with MOVEFILE_REPLACE_EXISTING).
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return makeError(ErrorCode::IoError, "cannot replace destination: " + ec.message(), context);
    }
    return ok();
}

Status writeTextFileAtomic(const std::filesystem::path& path, std::string_view text) {
    return writeFileAtomic(path, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

std::optional<std::filesystem::file_time_type> lastWriteTime(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return time;
}

} // namespace rv::core
