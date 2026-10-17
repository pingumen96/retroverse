#include <rv/catalog/fingerprint.hpp>

#include <array>
#include <cstddef>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

namespace rv::catalog {

bool isCurrent(const QuickHash& hash, const FileFacts& file) noexcept {
    return !file.directory && hash.size == file.size && hash.lastWriteTime == file.lastWriteTime;
}

core::Sha256Digest quickHashDigest(std::span<const std::byte> content) noexcept {
    if (content.size() <= kQuickHashWholeFileLimit) {
        return core::sha256(content);
    }
    core::Sha256 hash;
    hash.update(content.first(static_cast<std::size_t>(kQuickHashPartBytes)));
    hash.update(content.last(static_cast<std::size_t>(kQuickHashPartBytes)));
    return hash.finish();
}

std::string toHex(const core::Sha256Digest& digest) {
    return core::toHex(digest);
}

Expected<QuickHash> readQuickHash(const std::filesystem::path& file) {
    return detail::readQuickHashObserved(file, {});
}

Expected<QuickHash> detail::readQuickHashObserved(const std::filesystem::path& file,
                                                  const std::function<void(QuickHashReadStage)>& observer) {
    const auto stage = [&](QuickHashReadStage reached) {
        if (observer) {
            observer(reached);
        }
    };
    // The facts the hash is stored with are those a listing gives (what an import records and isCurrent() compares
    // with), which on NTFS may lag behind the file record the handle reads; the handle's are only checked for a write
    // during the read, the listing's again after it for a file replaced before the open.
    const auto listed = core::listedFileFacts(file);
    if (!listed) {
        return propagate(listed.error());
    }
    stage(QuickHashReadStage::Listed);
    auto reader = core::SharedFileReader::open(file);
    if (!reader) {
        return propagate(reader.error());
    }
    const auto before = reader->facts();
    if (!before) {
        return propagate(before.error());
    }
    stage(QuickHashReadStage::Opened);
    // Two reads at most, into one buffer: the whole file, or its head then its tail.
    static thread_local std::array<std::byte, kQuickHashWholeFileLimit> buffer;
    const std::uint64_t size = before->size;
    core::Sha256Digest digest;
    if (size <= kQuickHashWholeFileLimit) {
        const std::span<std::byte> whole(buffer.data(), static_cast<std::size_t>(size));
        if (auto read = reader->read(0, whole); !read) {
            return propagate(read.error());
        }
        digest = core::sha256(whole);
    } else {
        const std::span<std::byte> head(buffer.data(), static_cast<std::size_t>(kQuickHashPartBytes));
        const std::span<std::byte> tail(buffer.data() + kQuickHashPartBytes,
                                        static_cast<std::size_t>(kQuickHashPartBytes));
        if (auto read = reader->read(0, head); !read) {
            return propagate(read.error());
        }
        if (auto read = reader->read(size - kQuickHashPartBytes, tail); !read) {
            return propagate(read.error());
        }
        digest =
            core::sha256(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(kQuickHashWholeFileLimit)));
    }
    stage(QuickHashReadStage::Read);
    // A file written while it was read would give a hash of neither its old nor its new content.
    const auto after = reader->facts();
    if (!after) {
        return propagate(after.error());
    }
    if (!(*after == *before)) {
        return makeError(ErrorCode::IoError,
                         fmt::format("the file changed while it was read ({} bytes, then {})", before->size,
                                     after->size),
                         core::pathToUtf8Lossy(file));
    }
    // A file replaced or written between the listing and the open was hashed whole, but the facts listed first are
    // another file's: the hash would be stored as current for content the listing never described.
    const auto relisted = core::listedFileFacts(file);
    if (!relisted) {
        return propagate(relisted.error());
    }
    if (!(*relisted == *listed)) {
        return makeError(ErrorCode::IoError,
                         fmt::format("the file was replaced or written while it was read ({} bytes listed, then {})",
                                     listed->size, relisted->size),
                         core::pathToUtf8Lossy(file));
    }
    return QuickHash{digest, listed->size, listed->lastWriteTime};
}

Expected<QuickHash> readQuickHashBelow(const std::filesystem::path& folder, std::string_view relativePath) {
    auto path = core::extendedLengthPathBelow(folder, relativePath);
    if (!path) {
        return propagate(path.error());
    }
    return readQuickHash(*path);
}

} // namespace rv::catalog
