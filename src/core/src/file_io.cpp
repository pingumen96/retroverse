#include <rv/core/file_io.hpp>

#include <atomic>
#include <fstream>
#include <system_error>

#include <rv/core/text.hpp>

namespace rv::core {
namespace {

template <class Container>
Expected<Container> readWholeFile(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    const std::string context = pathToUtf8(path);
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
        return makeError(ErrorCode::NotFound, "file does not exist", context);
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

} // namespace

Expected<std::string> readTextFile(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    return readWholeFile<std::string>(path, maxBytes);
}

Expected<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    return readWholeFile<std::vector<std::byte>>(path, maxBytes);
}

Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data) {
    const std::string context = pathToUtf8(path);
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
            return makeError(ErrorCode::IoError, "cannot open temporary file for writing", pathToUtf8(temporary));
        }
        if (!data.empty()) {
            stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return makeError(ErrorCode::IoError, "write failed", pathToUtf8(temporary));
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
