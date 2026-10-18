#include <rv/core/backups.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

/// "YYYYMMDDTHHMMSS.mmmZ": the length of a backup name's time.
constexpr std::size_t kTimeLength = 20;
/// The counters tried for one millisecond: 1 (no suffix), then "-2" ...
constexpr unsigned kMaxCounter = 1000;

/// The parts of a backup's name after "<stem>-".
struct BackupName {
    std::wstring time; ///< fixed width: sorts like the times
    unsigned counter = 1;
};

bool isDigit(wchar_t c) noexcept {
    return c >= L'0' && c <= L'9';
}

/// "20261018T121314.123Z": formatUtcIso8601() without its separators, which file names cannot hold (':').
std::wstring backupTime(std::chrono::system_clock::time_point time) {
    std::wstring text;
    for (const char c : formatUtcIso8601(time)) {
        if (c != '-' && c != ':') {
            text += static_cast<wchar_t>(c);
        }
    }
    return text;
}

/// The time and counter of `name` when it is a backup name of (stem, extension).
std::optional<BackupName> parseBackupName(std::wstring_view name, std::wstring_view stem, std::wstring_view extension) {
    if (!name.starts_with(stem) || name.size() < stem.size() + 1 + kTimeLength || name[stem.size()] != L'-') {
        return std::nullopt;
    }
    name.remove_prefix(stem.size() + 1);
    const std::wstring_view time = name.substr(0, kTimeLength);
    for (std::size_t i = 0; i < kTimeLength; ++i) {
        const wchar_t expected = i == 8 ? L'T' : i == 15 ? L'.' : i == 19 ? L'Z' : L'0';
        if (expected == L'0' ? !isDigit(time[i]) : time[i] != expected) {
            return std::nullopt;
        }
    }
    name.remove_prefix(kTimeLength);
    BackupName parsed{std::wstring(time), 1};
    if (name.starts_with(L'-')) {
        name.remove_prefix(1);
        std::size_t digits = 0;
        unsigned counter = 0;
        while (digits < name.size() && isDigit(name[digits]) && digits < 4) {
            counter = counter * 10 + static_cast<unsigned>(name[digits] - L'0');
            ++digits;
        }
        // As backUpFile() writes them: 2 to kMaxCounter, without leading zeros.
        if (digits == 0 || name[0] == L'0' || counter < 2 || counter > kMaxCounter) {
            return std::nullopt;
        }
        parsed.counter = counter;
        name.remove_prefix(digits);
    }
    if (name != extension) {
        return std::nullopt;
    }
    return parsed;
}

/// The backups of (stem, extension) in `folder`, newest first.
std::vector<std::pair<std::filesystem::path, BackupName>>
findBackups(const std::filesystem::path& folder, std::wstring_view stem, std::wstring_view extension) {
    std::vector<std::pair<std::filesystem::path, BackupName>> found;
    std::error_code ec;
    // Listed through the \\?\ form (any length); the entries are named from the plain folder.
    std::filesystem::directory_iterator entries(std::filesystem::path(extendedLengthPath(folder)), ec);
    for (; !ec && entries != std::filesystem::directory_iterator(); entries.increment(ec)) {
        std::error_code typeError;
        if (!entries->is_regular_file(typeError)) {
            continue;
        }
        const std::filesystem::path name = entries->path().filename();
        if (auto parsed = parseBackupName(name.native(), stem, extension)) {
            found.emplace_back(folder / name, std::move(*parsed));
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return std::tie(a.second.time, a.second.counter, a.first) > std::tie(b.second.time, b.second.counter, b.first);
    });
    return found;
}

} // namespace

Expected<BackupReport> backUpFile(const std::filesystem::path& file, const std::filesystem::path& folder,
                                  const BackupOptions& options) {
    const auto content = readBinaryFile(file);
    if (!content) {
        return propagate(content.error());
    }
    const std::wstring stem = file.stem().native();
    const std::wstring extension = file.extension().native();
    const std::wstring time = backupTime(options.clock ? options.clock() : std::chrono::system_clock::now());

    std::optional<std::filesystem::path> target;
    for (unsigned counter = 1; counter <= kMaxCounter && !target; ++counter) {
        const std::wstring name =
            stem + L"-" + time + (counter == 1 ? std::wstring() : L"-" + std::to_wstring(counter)) + extension;
        const std::filesystem::path candidate = folder / name;
        std::error_code ec;
        const bool exists = std::filesystem::exists(std::filesystem::path(extendedLengthPath(candidate)), ec);
        if (ec) {
            return makeError(ErrorCode::IoError, "cannot look into the backup folder: " + ec.message(),
                             pathToUtf8Lossy(folder));
        }
        if (!exists) {
            target = candidate;
        }
    }
    if (!target) {
        return makeError(ErrorCode::Conflict, "no free backup name for this time", pathToUtf8Lossy(folder));
    }
    if (auto written = writeFileAtomic(*target, *content); !written) {
        return propagate(std::move(written.error()));
    }

    BackupReport report{*target, {}};
    auto backups = findBackups(folder, stem, extension);
    // The backup just written first, whatever its time says.
    std::stable_partition(backups.begin(), backups.end(), [&](const auto& backup) { return backup.first == *target; });
    for (std::size_t i = std::max<std::size_t>(options.keep, 1); i < backups.size(); ++i) {
        const std::filesystem::path& old = backups[i].first;
        std::error_code ec;
        if (std::filesystem::remove(std::filesystem::path(extendedLengthPath(old)), ec) && !ec) {
            report.removed.push_back(old);
        } else {
            spdlog::warn("Cannot remove the old backup {}: {}", pathToUtf8Lossy(old),
                         ec ? ec.message() : std::string("not found"));
        }
    }
    return report;
}

std::vector<std::filesystem::path> listBackupsOf(const std::filesystem::path& file,
                                                 const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> backups;
    for (auto& [path, name] : findBackups(folder, file.stem().native(), file.extension().native())) {
        backups.push_back(std::move(path));
    }
    return backups;
}

} // namespace rv::core
